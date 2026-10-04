"""Run the Python unit tests listed in tools\\tests\\py_tests.txt, as parallel processes.

    python tools\\tests\\run_py_tests.py [--list FILE]... [--since REV]

Each list line is "<order> <repo-relative path> [shards=N] [when=<glob>[,<glob>...]]". A
py_tests_private.txt beside py_tests.txt, where the tree has one, adds more lines in the same
form; the two lists are merged and ordered by <order>, so a module keeps its place in the output
whichever list names it. --list replaces both with the named files (repeatable). A listed module
that is missing, a malformed line, an order or path named twice, or lists naming no module at all
fail the run.

Every module runs through py_shard.py beside this file, as N processes when the row says
shards=N (1 to 32; default 1), each running the tests whose loader index i has i % N == K-1.
A module passes only when every shard exited 0 and wrote its result, the shards agree on the
module's test count and test ids, their selections are disjoint and cover every test, the tests
run add up to that count, and the count is above zero. Up to PY_JOBS shard processes run at once
(default: half the logical cores), longest first by the times recorded in
%LOCALAPPDATA%\\KEO\\timings\\py_tests.json (KEO_TIMINGS_DIR overrides the folder); with no
usable record, sharded modules go first by shard count, then list order. Each shard process
holds one host-wide cpu token from tools/build/slots.py while it runs, and inherits KEO_CPU_HELD=1
so that the processes it starts take none. A runner started under someone else's cpu token
(KEO_CPU_HELD=1) runs one shard at a time.

Each module's output is printed as one block, in list order, followed by
    py: <stem>: <n> tests, <N> shard(s), <seconds> s
or, when it failed, "py: <stem>: FAILED (<reason>), <N> shard(s), <seconds> s". The last line is
"python tests: <name>, <name>, ... OK", naming each module that ran in list order, or
"python tests FAILED: <name> (<reason>), ..."; exit 1 on any failure or invalid list.

Selection is off unless --since REV or PY_TESTS_SINCE=REV is given. Then a row with when= runs
only when a path changed since REV (git diff against the working tree, plus git status, untracked
files included) matches one of its globs or is the module's own file; globs are repo-relative
with forward slashes, matched case-insensitively with fnmatch, so * and ** both cross folders.
A change to this runner, py_shard.py or a list runs every module, and rows without when= always
run. Skipped modules are named on a "python test selection: skipped ..." line. When REV does not
resolve or git fails, every module runs, after "python test selection: off (<reason>), running
every module".
"""
import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
LIST_DIR = os.path.join(REPO, 'tools', 'tests')
LISTS = [os.path.join(LIST_DIR, 'py_tests.txt'), os.path.join(LIST_DIR, 'py_tests_private.txt')]
PY_SHARD = os.path.join(LIST_DIR, 'py_shard.py')
sys.path.insert(0, os.path.join(REPO, 'tools', 'build'))
import slots  # noqa: E402
RUNNER_FILES = ['tools/tests/run_py_tests.py', 'tools/tests/py_shard.py',
                'tools/tests/py_tests.txt', 'tools/tests/py_tests_private.txt']
MAX_SHARDS = 32
ROW_RE = re.compile(r'^(\d+)\s+(.+?)((?:\s+[A-Za-z_]+=\S*)*)\s*$')


class Row(object):
    def __init__(self, order, rel, shards, when):
        self.order = order
        self.rel = rel
        self.shards = shards
        self.when = when
        self.stem = os.path.splitext(os.path.basename(rel))[0]
        self.key = rel.replace('\\', '/')


def parse_options(text, where, errors):
    """Returns (shards, when) from the row's trailing key=value tokens, or None on an error."""
    shards, when = 1, None
    seen = set()
    for token in text.split():
        key, _, value = token.partition('=')
        if key in seen:
            errors.append('%s: %s= given twice' % (where, key))
            return None
        seen.add(key)
        if key == 'shards':
            if not value.isdigit() or not 1 <= int(value) <= MAX_SHARDS:
                errors.append('%s: shards=%s is not 1 to %d' % (where, value, MAX_SHARDS))
                return None
            shards = int(value)
        elif key == 'when':
            globs = value.split(',')
            bad = [g for g in globs if not g or '\\' in g or g.startswith('/') or ':' in g]
            if bad:
                errors.append('%s: when= needs repo-relative globs with forward slashes' % where)
                return None
            when = globs
        else:
            errors.append('%s: unknown option %s=' % (where, key))
            return None
    return shards, when


def read_list(path, errors):
    """Returns [Row] for the list at path; appends a message to errors per bad line."""
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8')
    out = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        where = '%s:%d' % (os.path.basename(path), lineno)
        m = ROW_RE.match(line)
        if not m:
            errors.append('%s: expected "<order> <path> [shards=N] [when=<globs>]"' % where)
            continue
        opts = parse_options(m.group(3), where, errors)
        if opts is None:
            continue
        out.append(Row(int(m.group(1)), m.group(2).strip(), opts[0], opts[1]))
    return out


def collect(lists, errors):
    """Returns the merged [Row] of lists [(path, required)], sorted by order."""
    rows = []
    for path, required in lists:
        if required or os.path.exists(path):
            if not os.path.exists(path):
                errors.append('%s: no such list' % path)
                continue
            rows.extend(read_list(path, errors))
    seen_orders, seen_paths = set(), set()
    for row in rows:
        key = os.path.normcase(os.path.normpath(row.rel))
        if row.order in seen_orders:
            errors.append('order %d named twice' % row.order)
        if key in seen_paths:
            errors.append('%s named twice' % row.rel)
        seen_orders.add(row.order)
        seen_paths.add(key)
    return sorted(rows, key=lambda r: r.order)


# ---- selection ---------------------------------------------------------------------------

def git_out(*args):
    p = subprocess.run(['git', '-C', REPO] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    if p.returncode != 0:
        msg = p.stderr.decode('utf-8', 'replace').strip().splitlines()
        raise RuntimeError('git %s failed%s' % (args[0], (': ' + msg[0]) if msg else ''))
    return p.stdout


def changed_since(rev):
    """The repo-relative paths changed since rev, committed or not, untracked included."""
    try:
        git_out('rev-parse', '--verify', '--quiet', rev + '^{commit}')
    except (OSError, RuntimeError):
        raise RuntimeError('cannot resolve %s' % rev)
    paths = set()
    for raw in git_out('diff', '--name-only', '--no-renames', '-z', rev, '--').split(b'\0'):
        if raw:
            paths.add(raw.decode('utf-8', 'surrogateescape'))
    fields = git_out('status', '--porcelain', '-z', '--no-renames', '--untracked-files=all').split(b'\0')
    i = 0
    while i < len(fields):
        entry = fields[i]
        i += 1
        if len(entry) < 4:
            continue
        paths.add(entry[3:].decode('utf-8', 'surrogateescape'))
        if entry[:2].strip()[:1] in (b'R', b'C') and i < len(fields):
            paths.add(fields[i].decode('utf-8', 'surrogateescape'))
            i += 1
    return sorted(p.replace('\\', '/') for p in paths)


def matches(path, globs):
    low = path.lower()
    return any(fnmatch.fnmatchcase(low, g.lower()) for g in globs)


def select(rows, rev, list_paths):
    """Returns (rows to run, [selection line]) for --since rev; every row on any doubt."""
    try:
        changed = changed_since(rev)
    except (OSError, RuntimeError) as exc:
        return rows, ['python test selection: off (%s), running every module' % exc]
    runner = set(RUNNER_FILES)
    for path in list_paths:
        try:
            rel = os.path.relpath(os.path.abspath(path), REPO)
        except ValueError:
            continue
        if not rel.startswith('..'):
            runner.add(rel.replace('\\', '/'))
    hit = [p for p in changed if p.lower() in set(r.lower() for r in runner)]
    if hit:
        return rows, ['python test selection: every module runs (%s changed since %s)' % (hit[0], rev)]
    keep, skipped = [], []
    for row in rows:
        if row.when is None or matches_any(changed, row.when + [row.key]):
            keep.append(row)
        else:
            skipped.append(row.stem)
    if not skipped:
        return keep, ['python test selection: nothing skipped since %s' % rev]
    return keep, ['python test selection: skipped %s (no change under their paths since %s)'
                  % (', '.join(skipped), rev)]


def matches_any(paths, globs):
    return any(matches(p, globs) for p in paths)


# ---- timings -----------------------------------------------------------------------------

def timings_path():
    base = os.environ.get('KEO_TIMINGS_DIR')
    if not base:
        base = os.path.join(os.environ.get('LOCALAPPDATA') or os.path.expanduser('~'), 'KEO', 'timings')
    return os.path.join(base, 'py_tests.json')


def load_timings(path):
    """{module key: seconds of shard time} or None when the record is missing or unusable."""
    if not os.path.exists(path):
        return None, None
    try:
        with open(path, 'rb') as f:
            data = json.loads(f.read().decode('utf-8'))
        mods = data['modules']
        out = {}
        for key, rec in mods.items():
            secs = float(rec['shard_s'])
            if secs < 0:
                raise ValueError('negative time for %s' % key)
            out[key] = secs
        return out, None
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        return None, 'python test timings: %s unreadable (%s), ordering by shard count' % (path, exc)


def save_timings(path, old, modules):
    data = {'version': 1, 'modules': {}}
    for key, secs in (old or {}).items():
        data['modules'][key] = {'shard_s': secs}
    for mod in modules:
        if mod.ok:
            data['modules'][mod.row.key] = {'shard_s': round(sum(s.wall for s in mod.shard_runs), 3),
                                            'shards': mod.row.shards}
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix='py_tests.', suffix='.tmp', dir=os.path.dirname(path))
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=1, sort_keys=True)
        os.replace(tmp, path)
    except OSError as exc:
        print('python test timings: not saved (%s)' % exc)


def schedule(modules, timings):
    """The (module, shard) jobs, longest expected first."""
    jobs = [(m, k) for m in modules for k in range(1, m.row.shards + 1)]
    if timings is None:
        return sorted(jobs, key=lambda j: (-j[0].row.shards, j[0].index, j[1]))

    def key(job):
        m, k = job
        secs = timings.get(m.row.key)
        if secs is None:
            return (0, -m.row.shards, 0.0, m.index, k)
        return (1, 0, -secs / m.row.shards, m.index, k)
    return sorted(jobs, key=key)


# ---- running -----------------------------------------------------------------------------

class ShardRun(object):
    def __init__(self, k, rc, out_path, res_path, t0, t1, error=None):
        self.k = k
        self.rc = rc
        self.out_path = out_path
        self.res_path = res_path
        self.t0 = t0
        self.t1 = t1
        self.wall = t1 - t0
        self.error = error


class Module(object):
    def __init__(self, row, index, path):
        self.row = row
        self.index = index
        self.path = path
        self.shard_runs = []
        self.start = None
        self.end = None
        self.ok = False
        self.total = 0
        self.reason = None


def run_shard(mod, k, tmp):
    n = mod.row.shards
    base = os.path.join(tmp, '%03d_%s.%d' % (mod.index, mod.row.stem, k))
    out_path, res_path = base + '.out', base + '.json'
    cmd = [sys.executable, PY_SHARD, mod.path, '--shard', '%d/%d' % (k, n), '--result', res_path]
    t0 = time.time()
    try:
        with slots.cpu_token('py %s %d/%d' % (mod.row.stem, k, n)):
            t0 = time.time()
            with open(out_path, 'wb') as out:
                rc = subprocess.call(cmd, cwd=REPO, stdout=out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=slots.child_env())
            return ShardRun(k, rc, out_path, res_path, t0, time.time())
    except (OSError, ValueError, slots.SlotTimeout) as exc:
        return ShardRun(k, None, out_path, res_path, t0, time.time(), error=str(exc))


def read_result(path):
    try:
        with open(path, 'rb') as f:
            return json.loads(f.read().decode('utf-8'))
    except (OSError, ValueError):
        return None


def judge(mod):
    """(total, None) when the module passed, else (total, reason)."""
    n = mod.row.shards
    runs = sorted(mod.shard_runs, key=lambda s: s.k)
    label = lambda k: ('shard %d/%d ' % (k, n)) if n > 1 else ''
    results = []
    for s in runs:
        res = read_result(s.res_path)
        if s.error is not None:
            return 0, '%scould not start: %s' % (label(s.k), s.error)
        if s.rc != 0:
            if res is not None and res.get('total') == 0:
                return 0, 'no tests'
            if res is not None and not res.get('selected'):
                return res.get('total', 0), '%sselected no tests' % label(s.k)
            if res is not None and (res.get('failures') or res.get('errors')):
                return res.get('total', 0), '%s%d failure(s), %d error(s)' % (
                    label(s.k), res.get('failures', 0), res.get('errors', 0))
            return 0, '%sexit %s%s' % (label(s.k), s.rc, '' if res is not None else ', no result')
        if res is None:
            return 0, '%swrote no result' % label(s.k)
        if (res.get('shard') != s.k or res.get('shards') != n
                or os.path.normcase(res.get('module', '')) != os.path.normcase(mod.path)):
            return 0, '%sresult names another shard' % label(s.k)
        results.append(res)
    totals = set(r.get('total') for r in results)
    if len(totals) != 1:
        return 0, 'shards disagree on the test count'
    total = totals.pop()
    if not isinstance(total, int) or total <= 0:
        return 0, 'no tests'
    if len(set(r.get('ids_sha256') for r in results)) != 1:
        return total, 'shards disagree on the test ids'
    seen = set()
    for r in results:
        sel = r.get('selected') or []
        if not sel:
            return total, '%sselected no tests' % label(r.get('shard'))
        if seen.intersection(sel) or len(set(sel)) != len(sel):
            return total, 'shards overlap'
        seen.update(sel)
    if len(seen) != total:
        return total, 'shards cover %d of %d tests' % (len(seen), total)
    ran = sum(r.get('run', 0) for r in results)
    if ran != total:
        return total, 'ran %d of %d tests' % (ran, total)
    if not all(r.get('ok') is True for r in results):
        return total, 'a shard reported a failure'
    return total, None


def write_raw(data):
    sys.stdout.flush()
    if data:
        sys.stdout.buffer.write(data)
        if not data.endswith(b'\n'):
            sys.stdout.buffer.write(os.linesep.encode('ascii'))
        sys.stdout.buffer.flush()


def print_block(mod):
    n = mod.row.shards
    for s in sorted(mod.shard_runs, key=lambda s: s.k):
        if n > 1:
            print('py-shard %s %d/%d: exit %s, %.1f s' % (mod.row.stem, s.k, n, s.rc, s.wall))
        try:
            with open(s.out_path, 'rb') as f:
                write_raw(f.read())
        except OSError:
            pass
    wall = (mod.end - mod.start) if mod.start is not None else 0.0
    if mod.ok:
        print('py: %s: %d tests, %d shard(s), %.1f s' % (mod.row.stem, mod.total, n, wall))
    else:
        print('py: %s: FAILED (%s), %d shard(s), %.1f s' % (mod.row.stem, mod.reason, n, wall))
    sys.stdout.flush()


def jobs_count():
    value = os.environ.get('PY_JOBS', '').strip()
    if not value:
        return max(1, (os.cpu_count() or 2) // 2)
    if not value.isdigit() or int(value) < 1:
        raise ValueError('PY_JOBS=%s is not a positive whole number' % value)
    return int(value)


def run_modules(modules, jobs_max, tmp):
    timings_file = timings_path()
    timings, note = load_timings(timings_file)
    if note:
        print(note)
    pending = dict((m.index, m.row.shards) for m in modules)
    by_index = dict((m.index, m) for m in modules)
    order = [m.index for m in modules]
    printed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs_max) as pool:
        futures = {}
        for mod, k in schedule(modules, timings):
            futures[pool.submit(run_shard, mod, k, tmp)] = mod
        for fut in concurrent.futures.as_completed(futures):
            mod = futures[fut]
            run = fut.result()
            mod.shard_runs.append(run)
            mod.start = run.t0 if mod.start is None else min(mod.start, run.t0)
            mod.end = run.t1 if mod.end is None else max(mod.end, run.t1)
            pending[mod.index] -= 1
            if pending[mod.index] == 0:
                mod.total, mod.reason = judge(mod)
                mod.ok = mod.reason is None
            while printed < len(order) and pending[order[printed]] == 0:
                print_block(by_index[order[printed]])
                printed += 1
    save_timings(timings_file, timings, modules)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Run the listed Python unit test modules.')
    ap.add_argument('--list', action='append', dest='lists', metavar='FILE',
                    help='a test list to run instead of the default lists (repeatable)')
    ap.add_argument('--since', metavar='REV',
                    help='run a when= module only if its paths changed since REV')
    args = ap.parse_args(argv)
    lists = [(p, True) for p in args.lists] if args.lists else [(LISTS[0], True), (LISTS[1], False)]

    errors = []
    rows = collect(lists, errors)
    if not rows and not errors:
        errors.append('no modules listed')
    if errors:
        for e in errors:
            print('python tests FAILED: bad list: %s' % e)
        return 1
    try:
        jobs_max = jobs_count()
        if jobs_max > 1 and slots.cpu_held():
            print('python test jobs: 1 (started under a cpu token)')
            jobs_max = 1
    except ValueError as exc:
        print('python tests FAILED: %s' % exc)
        return 1

    since = args.since or os.environ.get('PY_TESTS_SINCE', '').strip()
    if since:
        rows, lines = select(rows, since, [p for p, _ in lists])
        for line in lines:
            print(line)
        sys.stdout.flush()

    failed, modules = [], []
    for index, row in enumerate(rows):
        path = os.path.join(REPO, row.rel)
        if not os.path.exists(path):
            failed.append((index, '%s (missing)' % row.stem))
            continue
        modules.append(Module(row, index, os.path.abspath(path)))

    tmp = tempfile.mkdtemp(prefix='run_py_tests_')
    try:
        run_modules(modules, jobs_max, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for mod in modules:
        if not mod.ok:
            failed.append((mod.index, '%s (%s)' % (mod.row.stem, mod.reason)))
    if failed:
        print('python tests FAILED: %s' % ', '.join(text for _, text in sorted(failed)))
        return 1
    print('python tests: %s OK' % ', '.join(m.row.stem for m in modules))
    return 0


if __name__ == '__main__':
    sys.exit(main())

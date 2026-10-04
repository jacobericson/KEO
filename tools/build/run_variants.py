"""Build KEO variants: one cl process per source, then each variant's link (Python 3, stdlib only).

Called after the VS 2010 x64 environment and tools\\kenshilib\\build_env.bat are set up, so every
child inherits that environment: by build_opt_step4.bat (and so build_opt.bat) for the optimizer,
and by build.bat for the profiler (--kind prof --compile-only; build.bat links it itself).

Each variant compiles every source of its list in its own
    cl <cl_args()> /showIncludes /c <source> /Fo<OBJDIR>\\<stem>.obj
process; cl_args() is the one definition of the compile flags. Then, unless --compile-only, it
links through tools\\build\\variant.bat link. A variant's whole output goes to <OBJDIR>\\build.log:
a header, each source's cl output in list order without its "Note: including file:" lines (so the
log reads as a single cl run's would), then the link's. The console gets the list check's line
(--kind opt), one line per finished variant, any compiler/linker warning lines, and on failure the tail of the
failing variant's log.

Before an object can reach a link:
  - every listed object (and objects.json) is deleted before the compile, and each object must
    exist afterwards with an mtime at or after the compile's start (check_objects_fresh);
  - <OBJDIR>\\objects.json records this run's build id, the sha256 of the compile command
    (json.dumps of ['cl'] + cl_args()), and per source the object's sha256 and size and the
    include files cl reported (os.path.normcase(os.path.abspath()) of each, first occurrence
    order). validate_manifest() reads it back, re-reads the list file, and refuses another run's
    manifest, a list that changed, and a missing object or one whose bytes changed.

Usage:
  run_variants.py --fail-prefix PREFIX [--kind opt|prof] [--sources LIST] [--compile-only]
                  [--defines "<common>"] --variant OUTDIR OBJDIR "<extra defines>" "<label>" ...

  --kind        opt (default): the optimizer; DEV flags when OUTDIR contains "_dev", else PROD.
                prof: the profiler's flags.
  --sources     the source list (default tools\\build\\coresrc.txt for opt, profsrc.txt for prof).
                An opt link reads coresrc.txt, so another list needs --compile-only.
  --compile-only  compile and check the objects, no link.
  --defines     defines shared by every variant; each variant compiles with "<common> <extra>".
  --variant     one variant; LABEL "" means OUTDIR.

For --kind opt the list is checked first by tools\\build\\check_coresrc.py; the profiler's caller
runs check_coresrc.py --profsrc itself. Any list is refused when two sources share an object name.

Environment:
  BUILD_JOBS  how many variants are built at once (default: logical cores / 2, at least 1, at
              most the number of variants). BUILD_JOBS=1 builds one variant after another in the
              order given.
  BUILD_MP    how many child processes (a cl, or a variant's link) this run starts at once,
              across its variants. Unset = logical cores; 0 or 1 = one at a time. When the caller
              already holds a cpu token (KEO_CPU_HELD) it is one at a time.
  Host-wide slots (tools\\build\\slots.py): the run holds one heavy slot, each cl and each link one
  cpu token; KEO_SLOTS=off turns them off.
  KEO_TIMINGS_DIR  where compile.json keeps each source's last compile time, used to start the
              longest compiles first (default %LOCALAPPDATA%\\KEO\\timings; else source size).
  TEST_NOOP_CL=1     test only: each compile is a command that exits 0 and writes nothing.
  TEST_OBJ_TAMPER=1  test only: the first listed object is overwritten after the compile.

  Memory per process: a cl process needs about 260 MB and an LTCG link about 90 MB.

Failure: a compile error does not stop its variant's other compiles, but once a variant fails no
new variant is started; a variant already started still finishes (its link runs once its own
compile succeeded). A failure of the runner itself (no cpu token within KEO_CPU_WAIT, cl not
started) stops the run at once: no compile starts after it, and every queued one fails its
variant uncompiled. The script then prints "<fail-prefix> <OUTDIR>" for the first failed variant
in the order given and exits 1. Exit 0 when every variant built.
"""
import argparse
import hashlib
import heapq
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid

sys.dont_write_bytecode = True  # no tools\build\__pycache__ in the worktree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_coresrc  # noqa: E402
import slots  # noqa: E402

VARIANT_BAT = r'tools\build\variant.bat'
DEFAULT_SOURCES = {'opt': r'tools\build\coresrc.txt', 'prof': r'tools\build\profsrc.txt'}
MANIFEST = 'objects.json'
TIMINGS = 'compile.json'
TAIL_LINES = 40
WARNING_RE = re.compile(r'\bwarning [A-Z]+\d+', re.IGNORECASE)
ERROR_RE = re.compile(r'\berror\b|was not compiled', re.IGNORECASE)
FAILED_LISTED = 10
NOTE = b'Note: including file:'
TAMPER = b'TEST_OBJ_TAMPER!'

FLAVOURS = {
    'dev': 'DEV, folder ends in _dev: cl /O2 /GL without /Gy; link /LTCG without /OPT:REF /OPT:ICF',
    'prod': 'PROD: cl /O2 /GL /Gy; link /LTCG /OPT:REF /OPT:ICF',
    'prof': 'profiler: cl /O2 /GL; build.bat links it',
}


def cl_args(kind, flavour, defines, env=None):
    """cl's flags for one compile, without /showIncludes, the source and /Fo. The include roots
    are KENSHILIB and BOOST_ROOT exactly as the environment holds them: a header path a DLL
    records comes from them, so they are never normalised."""
    env = os.environ if env is None else env
    if kind == 'opt' and flavour == 'dev':
        codegen = ['/GL']
    elif kind == 'opt' and flavour == 'prod':
        codegen = ['/GL', '/Gy']
    elif kind == 'prof' and flavour == 'prof':
        codegen = ['/GL']
    else:
        raise ValueError('no compile flags for kind %r, flavour %r' % (kind, flavour))
    kenshilib = env.get('KENSHILIB', '')
    boost = env.get('BOOST_ROOT', '')
    if not kenshilib or not boost:
        raise ValueError('KENSHILIB and BOOST_ROOT must be set (tools\\kenshilib\\build_env.bat)')
    return (['/nologo', '/EHsc', '/O2'] + codegen +
            ['/MD', '/W3', '/DNDEBUG', '/DWIN32_LEAN_AND_MEAN', '/DBOOST_ALL_NO_LIB',
             '/DBOOST_ERROR_CODE_HEADER_ONLY', '/DBOOST_SYSTEM_NO_DEPRECATED'] +
            defines.split() +
            ['/I' + kenshilib + '\\Include', '/I' + kenshilib + '\\Include\\ogre', '/I' + boost, '/Isrc'])


def cmd_sha256(args):
    return hashlib.sha256(json.dumps(['cl'] + list(args)).encode('utf-8')).hexdigest()


def env_count(name, minimum):
    """Whole number from the environment, None when unset."""
    raw = os.environ.get(name, '').strip()
    if not raw:
        return None
    if not raw.isdigit() or int(raw) < minimum:
        raise ValueError('%s must be a whole number >= %d (got "%s")' % (name, minimum, raw))
    return int(raw)


def quote(arg):
    if '"' in arg:
        raise ValueError('argument must not contain a double quote: %s' % arg)
    if arg.endswith('\\'):
        raise ValueError('argument must not end with a backslash: %s' % arg)
    return '"%s"' % arg


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def append_log(path, message):
    with open(path, 'ab') as log:
        log.write((message.replace('\r\n', '\n').replace('\n', '\r\n') + '\r\n').encode('mbcs', 'replace'))


def load_sources(path):
    """The list's entries in order: a blank line or one starting with '#' is skipped."""
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r\n', '\n')
    sources = [line.strip() for line in text.split('\n')]
    sources = [s for s in sources if s and not s.startswith('#')]
    if not sources:
        raise ValueError('%s is empty' % path)
    seen = {}
    for source in sources:
        key = obj_name(source).lower()
        if key in seen:
            raise ValueError('%s: %s and %s share one object name' % (path, seen[key], source))
        seen[key] = source
    return sources


def obj_name(source):
    return os.path.splitext(os.path.basename(source))[0] + '.obj'


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def split_notes(output):
    """(cl's output without its "Note: including file:" lines, the include files they name)."""
    kept = []
    includes = []
    seen = set()
    for line in output.splitlines(True):
        if line.startswith(NOTE):
            path = line[len(NOTE):].strip().decode('mbcs', 'replace')
            path = os.path.normcase(os.path.abspath(path))
            if path not in seen:
                seen.add(path)
                includes.append(path)
        else:
            kept.append(line)
    text = b''.join(kept)
    if text and not text.endswith(b'\n'):
        text += b'\r\n'
    return text, includes


def timings_path():
    d = os.environ.get('KEO_TIMINGS_DIR', '').strip()
    if not d:
        d = os.path.join(os.environ.get('LOCALAPPDATA') or os.path.expanduser('~'), 'KEO', 'timings')
    return os.path.join(d, TIMINGS)


def load_timings():
    """{source key: seconds} from the last runs; empty when there is none or it is unreadable."""
    try:
        with open(timings_path(), 'r', encoding='utf-8') as f:
            data = json.load(f)
        seconds = data.get('seconds', {})
        return dict((k, float(v)) for k, v in seconds.items())
    except (OSError, ValueError, AttributeError, TypeError):
        return {}


def save_timings(measured):
    """Merges this run's compile times into compile.json; best effort (only the order uses it)."""
    if not measured:
        return
    path = timings_path()
    seconds = load_timings()
    seconds.update(measured)
    tmp = '%s.%d.tmp' % (path, os.getpid())
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'version': 1, 'seconds': seconds}, f, indent=0, sort_keys=True)
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def timing_key(source):
    return source.replace('/', '\\').lower()


class Variant(object):
    def __init__(self, index, kind, outdir, objdir, defines, label, list_path, compile_only, build_id):
        self.index = index
        self.kind = kind
        self.outdir = outdir
        self.objdir = objdir
        self.defines = defines
        self.label = label or outdir
        if kind == 'prof':
            self.flavour = 'prof'
        else:
            self.flavour = 'dev' if '_dev' in outdir.lower() else 'prod'  # variant.bat's rule
        self.list_path = list_path
        self.compile_only = compile_only
        self.build_id = build_id
        self.log = os.path.join(objdir, 'build.log')
        self.manifest = os.path.join(objdir, MANIFEST)
        self.ok_marker = self.label + (' compile OK:' if compile_only else ' build OK:')
        self.sources = []        # the list's sources, set once in main()
        self.args = []           # cl_args() for this variant
        self.results = {}        # list position -> (returncode, output bytes, includes)
        self.left = 0            # compiles not yet reported
        self.state = 'pending'   # pending, compiling, finishing, done
        self.returncode = None
        self.start = None
        self.compile_start = None
        self.seconds = 0.0

    def compile_command(self, cl, source):
        if os.environ.get('TEST_NOOP_CL') == '1':
            return [os.environ.get('ComSpec', 'cmd.exe'), '/d', '/c', 'exit /b 0']
        return [cl] + self.args + ['/showIncludes', '/c', source,
                                   '/Fo' + os.path.join(self.objdir, obj_name(source))]


def check_objects_fresh(objdir, sources, start_time):
    """Problems with the objects `sources` should have produced in objdir: a
    missing .obj, or one whose mtime predates start_time (a leftover from
    before this compile, somehow not removed). Returns a list of strings,
    empty when everything is fresh."""
    problems = []
    for source in sources:
        path = os.path.join(objdir, obj_name(source))
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            problems.append('%s: no object was produced (%s)' % (source, path))
            continue
        if mtime < start_time:
            problems.append('%s: %s predates this compile (mtime %.3f < start %.3f) -- not deleted first?'
                             % (source, path, mtime, start_time))
    return problems


def write_manifest(v):
    entries = []
    for i, source in enumerate(v.sources):
        path = os.path.join(v.objdir, obj_name(source))
        entries.append({'src': source, 'obj': path, 'how': 'compiled', 'sha256': file_sha256(path),
                        'size': os.path.getsize(path), 'includes': v.results[i][2]})
    data = {'build_id': v.build_id, 'kind': v.kind, 'flavour': v.flavour,
            'cmd_sha256': cmd_sha256(v.args), 'sources': entries}
    tmp = v.manifest + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, v.manifest)


def validate_manifest(v):
    """Problems that refuse the link: objects.json not this run's, the list file changed since the
    compile, or an object missing or not the bytes the compile left. Empty when all is well."""
    try:
        with open(v.manifest, 'r', encoding='utf-8') as f:
            data = json.load(f)
        entries = data['sources']
        recorded = [e['src'] for e in entries]
    except (OSError, ValueError, KeyError, TypeError) as error:
        return ['%s: unreadable (%s)' % (v.manifest, error)]
    problems = []
    if data.get('build_id') != v.build_id:
        problems.append('%s: build id %s is not this run\'s (%s)' % (v.manifest, data.get('build_id'), v.build_id))
    if (data.get('kind'), data.get('flavour')) != (v.kind, v.flavour):
        problems.append('%s: kind %s/%s, expected %s/%s' % (v.manifest, data.get('kind'), data.get('flavour'),
                                                           v.kind, v.flavour))
    if data.get('cmd_sha256') != cmd_sha256(v.args):
        problems.append('%s: the compile command\'s sha256 differs from this run\'s' % v.manifest)
    try:
        listed = load_sources(v.list_path)
    except (OSError, ValueError) as error:
        listed = None
        problems.append('%s: cannot re-read the source list (%s)' % (v.list_path, error))
    if listed is not None and listed != recorded:
        problems.append('%s: the source list differs from the %d source(s) compiled' % (v.list_path, len(recorded)))
    for e in entries:
        source, path = e.get('src'), e.get('obj')
        if path != os.path.join(v.objdir, obj_name(source or '')):
            problems.append('%s: recorded object %s is not this folder\'s' % (source, path))
            continue
        try:
            size = os.path.getsize(path)
            digest = file_sha256(path)
        except OSError:
            problems.append('%s: %s is missing' % (source, path))
            continue
        if size != e.get('size') or digest != e.get('sha256'):
            problems.append('%s: %s does not match its recorded sha256 (rewritten after the compile)'
                            % (source, path))
    return problems


def start_variant(v):
    """Writes the log header and deletes this variant's objects; False (logged) when it cannot."""
    v.start = time.time()
    try:
        os.makedirs(v.objdir, exist_ok=True)
        with open(v.log, 'wb'):
            pass
        append_log(v.log, '=== Building %s ===' % v.label)
        append_log(v.log, 'Flavour: %s' % FLAVOURS[v.flavour])
        append_log(v.log, 'CL: %s' % subprocess.list2cmdline(
            ['cl'] + v.args + ['/showIncludes', '/c', '<source>', '/Fo' + os.path.join(v.objdir, '<stem>.obj')]))
        append_log(v.log, '  one cl process per source, %d source(s) from %s; output below in list order'
                   % (len(v.sources), v.list_path))
        v.compile_start = time.time()
        # This variant's objects from a previous build must not survive into this one: a compile
        # that silently turns into a no-op would otherwise exit 0 and let the link reuse the old
        # objects. Deleted first, such an object is simply missing, which check_objects_fresh()
        # catches.
        for path in [os.path.join(v.objdir, obj_name(s)) for s in v.sources] + [v.manifest]:
            if os.path.exists(path):
                os.remove(path)
    except OSError as error:
        message = 'run_variants.py: could not start the compile: %s' % error
        try:
            append_log(v.log, message)
        except OSError:
            print('%s: %s' % (v.label, message))
            sys.stdout.flush()
        return False
    return True


def not_compiled(source, why):
    return ('run_variants.py: %s was not compiled: %s\r\n' % (source, why)).encode('mbcs', 'replace')


def run_compile(v, position, source, cl, stop, done):
    """Compiles one source; always reports on the queue, since the main loop waits for it. A
    failure of the runner itself (no cpu token in time, cl not started) is reported as `broken`,
    which stops the run; a compile error is not."""
    rc, output, includes, seconds, broken = 1, b'', [], None, None
    try:
        command = v.compile_command(cl, source)
        with slots.cpu_token('cl ' + source):
            if stop.is_set():
                output = not_compiled(source, 'the run stopped')
                return
            t0 = time.time()
            p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, env=slots.child_env())
            seconds = time.time() - t0
        output, includes = split_notes(p.stdout)
        rc = p.returncode
    except (OSError, ValueError, slots.SlotTimeout) as error:
        broken = '%s: %s' % (source, error)
        output = not_compiled(source, error)
    except Exception as error:  # never leave the main loop waiting for this compile
        broken = '%s: unexpected error %r' % (source, error)
        output = not_compiled(source, 'unexpected error %r' % error)
    finally:
        done.put(('compile', v, (position, rc, output, includes, seconds, broken)))


def finish_variant(v, done):
    """After the last compile: the log's compile output, the object checks, then the link."""
    rc = 1
    try:
        with open(v.log, 'ab') as log:
            for i in range(len(v.sources)):
                log.write(v.results[i][1])
        failed = [i for i in range(len(v.sources)) if v.results[i][0] != 0]
        if failed:
            # The console shows only the log's tail, so each failed source's first error is
            # repeated here (never a warning line, which would be counted twice).
            lines = ['', '%s COMPILE FAILED (%d source(s)):' % (v.label, len(failed))]
            for i in failed[:FAILED_LISTED]:
                text = v.results[i][1].decode('mbcs', 'replace').splitlines()
                first = [l.strip() for l in text if ERROR_RE.search(l) and not WARNING_RE.search(l)]
                lines.append('  %s: %s' % (v.sources[i], first[0] if first else 'exit %s, see above' % v.results[i][0]))
            if len(failed) > FAILED_LISTED:
                lines.append('  ... and %d more, see above' % (len(failed) - FAILED_LISTED))
            append_log(v.log, '\n'.join(lines))
            return
        problems = check_objects_fresh(v.objdir, v.sources, v.compile_start)
        if problems:
            append_log(v.log, 'run_variants.py: compile reported success but produced no fresh '
                              'object for %d source(s):\n  %s' % (len(problems), '\n  '.join(problems)))
            return
        write_manifest(v)
        if os.environ.get('TEST_OBJ_TAMPER') == '1':
            path = os.path.join(v.objdir, obj_name(v.sources[0]))
            with open(path, 'r+b') as f:
                f.write(TAMPER)
            append_log(v.log, 'run_variants.py: TEST_OBJ_TAMPER=1: overwrote the first bytes of %s (test only)' % path)
        problems = validate_manifest(v)
        if problems:
            append_log(v.log, 'run_variants.py: %s refused the link, %d problem(s):\n  %s'
                       % (MANIFEST, len(problems), '\n  '.join(problems)))
            return
        if v.compile_only:
            append_log(v.log, '%s %s\\' % (v.ok_marker, v.objdir))
            rc = 0
            return
        command = '%s /d /c %s link %s %s %s' % (
            os.environ.get('ComSpec', 'cmd.exe'), VARIANT_BAT, quote(v.outdir), quote(v.objdir), quote(v.label))
        with slots.cpu_token('link ' + v.outdir):
            with open(v.log, 'ab') as log:
                rc = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     env=slots.child_env())
    except (OSError, ValueError, slots.SlotTimeout) as error:
        rc = 1
        message = 'run_variants.py: could not finish the variant: %s' % error
        try:
            append_log(v.log, message)
        except OSError:
            print('%s: %s' % (v.label, message))
            sys.stdout.flush()
    except Exception as error:  # never leave the main loop waiting for this variant
        rc = 1
        print('%s: run_variants.py: unexpected error: %r' % (v.label, error))
        sys.stdout.flush()
    finally:
        done.put(('finish', v, rc))


def report(variant):
    lines = read_log(variant.log)
    warnings = [l.strip() for l in lines if WARNING_RE.search(l)]
    for line in warnings:
        print('  %s: %s' % (variant.label, line))
    if variant.returncode == 0:
        ok = [l for l in lines if l.startswith(variant.ok_marker)]
        print('%s   [%.1f s]' % (ok[-1] if ok else variant.ok_marker.rstrip(':'), variant.seconds))
    else:
        print('')
        print('%s FAILED (exit %s) after %.1f s. Last %d lines of %s:' % (
            variant.label, variant.returncode, variant.seconds, TAIL_LINES, variant.log))
        for line in lines[-TAIL_LINES:]:
            print('    ' + line)
    sys.stdout.flush()


def build(variants, jobs, mp, cl):
    """Runs every variant; returns the failed ones in the order they failed."""
    estimates = load_timings()
    measured = {}
    pending = list(variants)
    active = []
    heap = []
    failed = []
    running = 0
    done = queue.Queue()
    stop = threading.Event()
    while True:
        # Variants start in the order given; none after a failure.
        while pending and len(active) < jobs and not failed and not stop.is_set():
            v = pending.pop(0)
            if start_variant(v):
                v.state = 'compiling'
                v.left = len(v.sources)
                active.append(v)
                for position, source in enumerate(v.sources):
                    estimate = estimates.get(timing_key(source))
                    if estimate is None:
                        try:
                            estimate = os.path.getsize(source) / 1e6
                        except OSError:
                            estimate = 0.0
                    heapq.heappush(heap, (v.index, -estimate, position, source))
            else:
                v.returncode = 1
                v.seconds = time.time() - v.start
                v.state = 'done'
                report(v)
                failed.append(v)
        # A finished compile's link goes first: it is on the critical path.
        for v in active:
            if running >= mp:
                break
            if v.state == 'compiling' and v.left == 0:
                v.state = 'finishing'
                threading.Thread(target=finish_variant, args=(v, done)).start()
                running += 1
        while running < mp and heap:
            index, _, position, source = heapq.heappop(heap)
            v = variants[index]
            threading.Thread(target=run_compile, args=(v, position, source, cl, stop, done)).start()
            running += 1
        if not running:
            break
        what, v, payload = done.get()
        running -= 1
        if what == 'compile':
            position, rc, output, includes, seconds, broken = payload
            v.results[position] = (rc, output, includes)
            v.left -= 1
            if rc == 0 and seconds is not None and os.environ.get('TEST_NOOP_CL') != '1':
                measured[timing_key(v.sources[position])] = round(seconds, 2)
            if broken and not stop.is_set():
                # Every later compile would likely fail the same way, each after its own wait:
                # nothing more starts, and what was queued fails its variant uncompiled.
                stop.set()
                print('run_variants.py: stopping the run: %s; %d queued compile(s) not started'
                      % (broken, len(heap)))
                sys.stdout.flush()
                while heap:
                    index, _, position, source = heapq.heappop(heap)
                    w = variants[index]
                    w.results[position] = (1, not_compiled(source, 'the run stopped'), [])
                    w.left -= 1
        else:
            v.returncode = payload
            v.seconds = time.time() - v.start
            v.state = 'done'
            active.remove(v)
            report(v)
            if payload != 0:
                failed.append(v)
    save_timings(measured)
    return failed


def main():
    try:
        sys.stdout.reconfigure(errors='replace')
    except AttributeError:
        pass
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--fail-prefix', required=True)
    parser.add_argument('--kind', choices=('opt', 'prof'), default='opt')
    parser.add_argument('--sources')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--defines', default='')
    parser.add_argument('--variant', nargs=4, action='append', required=True,
                        metavar=('OUTDIR', 'OBJDIR', 'DEFINES', 'LABEL'))
    args = parser.parse_args()
    list_path = args.sources or DEFAULT_SOURCES[args.kind]

    if args.kind == 'opt':
        # A source dropped from coresrc.txt has no other guard: it usually becomes an
        # unresolved-external link error, but a translation unit nothing else references would
        # vanish from every build silently.
        if check_coresrc.main(['--coresrc', list_path.replace('\\', '/')]) != 0:
            return 1

    build_id = uuid.uuid4().hex
    variants = []
    try:
        if (args.kind == 'opt' and not args.compile_only and
                os.path.normcase(os.path.abspath(list_path)) != os.path.normcase(os.path.abspath(DEFAULT_SOURCES['opt']))):
            raise ValueError('variant.bat links %s, so --sources %s needs --compile-only'
                             % (DEFAULT_SOURCES['opt'], list_path))
        sources = load_sources(list_path)
        for index, (outdir, objdir, extra, label) in enumerate(args.variant):
            defines = ' '.join(part for part in (args.defines.strip(), extra.strip()) if part)
            v = Variant(index, args.kind, outdir, objdir, defines, label, list_path, args.compile_only, build_id)
            v.sources = sources
            v.args = cl_args(v.kind, v.flavour, v.defines)
            for arg in (v.outdir, v.objdir, v.defines, v.label):
                quote(arg)
            variants.append(v)

        cores = os.cpu_count() or 1
        jobs = env_count('BUILD_JOBS', 1)
        if jobs is None:
            jobs = max(1, cores // 2)
        jobs = min(jobs, len(variants))
        mp = env_count('BUILD_MP', 0)
        if mp is None:
            mp = jobs * max(1, cores // jobs)
        mp = max(1, mp)
        held = slots.cpu_held()  # its children take no tokens, so one job at a time
        if held:
            mp = 1
        cl = shutil.which('cl.exe')
        if os.environ.get('TEST_NOOP_CL') != '1' and not cl:
            raise ValueError('cl.exe is not on PATH (run vcvarsall.bat amd64 first)')
    except (OSError, ValueError) as error:
        print('ERROR: %s' % error)
        return 1

    print('Building %d variant(s), %d at a time, %d process(es) at once%s, one cl per source; '
          'per-variant logs: <object folder>\\build.log'
          % (len(variants), jobs, mp, ' (cpu token held by the caller)' if held else ''))
    sys.stdout.flush()

    try:
        with slots.heavy('run_variants ' + ' '.join(v.outdir for v in variants)):
            failed = build(variants, jobs, mp, cl)
    except (slots.SlotTimeout, ValueError) as error:
        print('ERROR: %s' % error)
        return 1

    unfinished = [v for v in variants if v.returncode is None]
    if failed or unfinished:
        if unfinished:
            print('Not built after the failure: %s' % ' '.join(v.outdir for v in unfinished))
        if not failed:  # cannot happen (a started variant always finishes); never report success
            print('')
            print('%s %s' % (args.fail_prefix, unfinished[0].outdir))
            return 1
        first = min(failed, key=lambda v: v.index)
        if len(failed) > 1:
            print('Also failed: %s' % ' '.join(v.outdir for v in sorted(failed, key=lambda v: v.index) if v is not first))
        print('')
        print('%s %s' % (args.fail_prefix, first.outdir))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

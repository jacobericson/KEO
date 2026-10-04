"""Compile and run every host unit-test suite as concurrent processes
(Python 3, stdlib only), modelled on tools\\build\\run_variants.py. Each
process it starts holds one host-wide cpu token from tools\\build\\slots.py
(KEO_SLOTS=off turns the pools off).

Called by build_tests.bat after it has set up the VS 2010 x64 environment.
Suites are data, not code: tools\\tests\\suites.txt lists one per line as

    name | unit .cpp | extra sources | defines

"name" also names the .exe (build\\tests\\<name>.exe) and its own build log
(build\\tests\\<name>.log). "unit .cpp" and each entry of "extra sources" are
paths from the repo root. "defines" is optional extra /D flags; most suites
have none. Blank lines and lines starting with # are ignored.

The run is a graph of jobs, one process each:
  compile  "cl /c" for one source. A suite's unit source compiles into
           build\\tests\\obj\\<name>\\. An extra source compiles once per
           (source, defines) pair however many suites list it, into
           build\\tests\\obj\\_shared\\<first 8 hex of sha1(defines)>\\, named
           after its path with each separator as "__".
  link     "cl /nologo <the suite's objects in source order> /Fe<exe>", once
           every object it names has compiled.
  run      the .exe, from the repo root, its output appended to the log.
At most TEST_JOBS jobs run at once (default: logical core count). A ready
run or link starts before any compile, and compiles start largest suite
first.

Every compile deletes its object first and must leave one dated no earlier
than its own start; every link does the same with the .exe. A tool that
exits 0 without writing its output fails the suite rather than letting it
link or run something left from an earlier run.

A suite fails to compile when any compile it needs (its own or a shared one)
or its link fails, so a shared compile's failure fails every suite listing
that source. Each suite's log holds the command and output of each of its
compiles in source order, then the link's, then a "RUN" line and the .exe's
output. A failed suite is recorded and the run continues -- no suite is
skipped because another failed -- so one run reports every failure at once.
The script exits 1 if any suite failed or if the suite list named a source
that does not exist, printing each failed suite's name and its log's tail;
it exits 0, printing "N suites, all passed", only when every suite compiled,
ran and returned 0. A suite's "[x.xs]" runs from the start of the first
compile it needs (a shared one included) to the end of its last job.

Usage: python tools\\tests\\run_suites.py [--suites PATH]
Environment: TEST_JOBS (default: os.cpu_count()).
  Test-only: TEST_SUITES_NOOP_CL=1 replaces every compile, and
  TEST_SUITES_NOOP_LINK=1 every link, with a command that exits 0 and writes
  nothing; both must fail the run.
"""
import argparse
import hashlib
import heapq
import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, 'build'))
import slots  # noqa: E402

SUITES_TXT = r'tools\tests\suites.txt'
TAIL_LINES = 30
TESTS_DIR = os.path.join('build', 'tests')
OBJ_DIR = os.path.join(TESTS_DIR, 'obj')
SHARED_DIR = os.path.join(OBJ_DIR, '_shared')
CL_FLAGS = ['/nologo', '/EHsc', '/O2', '/W3', '/Isrc']
NOOP_COMMAND = [os.environ.get('ComSpec', 'cmd.exe'), '/d', '/c', 'exit /b 0']

# Ready jobs start in this order: a run or link finishes a suite, a compile
# only feeds one.
PRIORITY_RUN, PRIORITY_LINK, PRIORITY_COMPILE = 0, 1, 2


def parse_suites(path):
    suites = []
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8').replace('\r\n', '\n')
    for lineno, raw in enumerate(text.split('\n'), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split('|')]
        if len(parts) != 4:
            raise ValueError('%s:%d: expected 4 "|"-separated fields, got %d: %r'
                              % (path, lineno, len(parts), raw))
        name, unit, extra, defines = parts
        if not name or not unit:
            raise ValueError('%s:%d: name and unit source are required: %r' % (path, lineno, raw))
        suites.append({
            'name': name,
            'unit': unit,
            'extra': extra.split() if extra else [],
            'defines': defines,
        })
    names = [s['name'] for s in suites]
    dupes = sorted(set(n for n in names if names.count(n) > 1))
    if dupes:
        raise ValueError('%s: duplicate suite name(s): %s' % (path, ', '.join(dupes)))
    return suites


class Suite(object):
    def __init__(self, data):
        self.name = data['name']
        self.unit = data['unit']
        self.extra = data['extra']
        self.defines = data['defines'].split() if data['defines'] else []
        # The unit's object goes in the suite's own folder: two suites can
        # share a unit source under different defines, and cl names an
        # object after its source's basename.
        self.objdir = os.path.join(OBJ_DIR, self.name)
        self.exe = os.path.join(TESTS_DIR, self.name + '.exe')
        self.log = os.path.join(TESTS_DIR, self.name + '.log')
        self.compiles = []     # CompileJob per source, in source order
        self.waiting = 0       # distinct compiles not yet finished
        self.compiled = None   # returncode, once known
        self.ran = None        # returncode, once known
        self.seconds = 0.0
        self.reported = False


class Job(object):
    seq = 0

    def __init__(self, kind, priority):
        self.kind = kind
        self.priority = priority
        Job.seq += 1
        self.order = Job.seq
        self.start = None
        self.end = None
        self.rc = None

    def key(self):
        return (self.priority, self.order)


class CompileJob(Job):
    def __init__(self, source, defines, obj):
        Job.__init__(self, 'compile', None)
        self.source = source
        self.obj = obj
        self.cmd = ['cl'] + CL_FLAGS + defines + ['/c', source, '/Fo' + obj]
        self.users = []        # suites that link this object
        self.output = b''

    def key(self):
        # Largest suite first: its link waits on the most compiles.
        return (PRIORITY_COMPILE, -max(len(s.compiles) for s in self.users), self.order)


class SuiteJob(Job):
    def __init__(self, kind, priority, suite):
        Job.__init__(self, kind, priority)
        self.suite = suite


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def test_command(flag, cmd):
    """(command to run, note for the log): `cmd`, or the no-op when the
    test-only `flag` is set."""
    if os.environ.get(flag, '').strip() != '1':
        return cmd, b''
    note = 'run_suites.py: %s=1 ran "%s" instead\n' % (flag, ' '.join(NOOP_COMMAND))
    return NOOP_COMMAND, note.encode('utf-8')


def shared_object(source, defines):
    """(key, object path) for an extra source compiled under `defines`."""
    norm = os.path.normcase(os.path.normpath(source))
    define_text = ' '.join(defines)
    folder = hashlib.sha1(define_text.encode('utf-8')).hexdigest()[:8]
    stem = os.path.splitext(norm)[0]
    if os.path.isabs(norm) or os.path.splitdrive(norm)[0] or norm.startswith('..'):
        # A source outside the tree: its whole path would overrun MAX_PATH.
        flat = '%s-%s' % (os.path.basename(stem), hashlib.sha1(norm.encode('utf-8')).hexdigest()[:10])
    else:
        flat = stem.replace('\\', '__').replace('/', '__')
    return (norm, define_text), os.path.join(SHARED_DIR, folder, flat + '.obj')


def unit_object(suite):
    return os.path.join(suite.objdir, os.path.splitext(os.path.basename(suite.unit))[0] + '.obj')


def plan(suites):
    """Builds every suite's compile jobs, one per (source, defines) for extra
    sources. Returns the distinct compile jobs; raises ValueError when two
    different sources would write one object path."""
    jobs = []
    shared = {}
    owners = {}
    for s in suites:
        unit = CompileJob(s.unit, s.defines, unit_object(s))
        jobs.append(unit)
        s.compiles.append(unit)
        for source in s.extra:
            key, obj = shared_object(source, s.defines)
            job = shared.get(key)
            if job is None:
                owner = owners.get(os.path.normcase(obj))
                if owner is not None:
                    raise ValueError('%s and %s would both compile to %s' % (owner[0], source, obj))
                owners[os.path.normcase(obj)] = (source, key)
                job = CompileJob(source, s.defines, obj)
                shared[key] = job
                jobs.append(job)
            s.compiles.append(job)
    for s in suites:
        for job in s.compiles:
            if s not in job.users:
                job.users.append(s)
                s.waiting += 1
    return jobs


def stale_output(path, start, what):
    """None when `path` exists with an mtime no earlier than `start`, else why
    not (the same test run_variants.py applies to its objects)."""
    try:
        mtime = os.stat(path).st_mtime
    except OSError:
        return 'no %s was produced (%s)' % (what, path)
    if mtime < start:
        return '%s predates this step (mtime %.3f < start %.3f) -- not deleted first?' % (path, mtime, start)
    return None


def remove_if_present(path):
    if os.path.exists(path):
        os.remove(path)


def do_compile(job):
    os.makedirs(os.path.dirname(job.obj), exist_ok=True)
    remove_if_present(job.obj)
    command, note = test_command('TEST_SUITES_NOOP_CL', job.cmd)
    with slots.cpu_token('suite compile %s' % job.source):
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, env=slots.child_env())
    job.output = note + proc.stdout
    if proc.returncode != 0:
        return proc.returncode
    problem = stale_output(job.obj, job.start, 'object')
    if problem:
        job.output += ('run_suites.py: %s: compile reported success but %s\n' % (job.source, problem)).encode('utf-8')
        return 1
    return 0


def write_compile_sections(log, suite):
    log.write(('=== %s ===\n' % suite.name).encode('utf-8'))
    for job in suite.compiles:
        log.write(('CL: %s\n' % ' '.join(job.cmd)).encode('utf-8'))
        log.write(job.output)


def do_link(job):
    suite = job.suite
    cmd = ['cl', '/nologo'] + [c.obj for c in suite.compiles] + ['/Fe' + suite.exe]
    command, note = test_command('TEST_SUITES_NOOP_LINK', cmd)
    with open(suite.log, 'wb') as log:
        write_compile_sections(log, suite)
        log.write(('LINK: %s\n' % ' '.join(cmd)).encode('utf-8') + note)
        log.flush()
        remove_if_present(suite.exe)
        with slots.cpu_token('suite link %s' % suite.name):
            rc = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=slots.child_env())
        if rc != 0:
            return rc
        problem = stale_output(suite.exe, job.start, 'executable')
        if problem:
            log.write(('run_suites.py: link reported success but %s\n' % problem).encode('utf-8'))
            return 1
    return 0


def do_run(job):
    suite = job.suite
    with open(suite.log, 'ab') as log:
        log.write(b'RUN\n')
        log.flush()
        with slots.cpu_token('suite run %s' % suite.name):
            return subprocess.call([suite.exe], stdout=log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, env=slots.child_env())


def note_failure(job, message):
    """Records why a job could not run where its suites' logs will show it."""
    if job.kind == 'compile':
        job.output += (message + '\n').encode('utf-8', 'replace')
        return
    try:
        with open(job.suite.log, 'ab') as log:
            log.write((message + '\n').encode('utf-8', 'replace'))
    except OSError:
        print(message)
        sys.stdout.flush()


def execute(job, results):
    """Always puts (job) on the queue exactly once, whatever happens: the main
    loop waits for one report per started job, so an uncaught exception here
    (cl missing from PATH, a log-open failure, ...) would otherwise hang the
    whole run on results.get() forever instead of failing it."""
    job.start = time.time()
    what = job.source if job.kind == 'compile' else job.suite.name
    try:
        if job.kind == 'compile':
            job.rc = do_compile(job)
        elif job.kind == 'link':
            job.rc = do_link(job)
        else:
            job.rc = do_run(job)
    except (OSError, ValueError, slots.SlotTimeout) as error:
        job.rc = 1
        note_failure(job, 'run_suites.py: could not %s %s: %s' % (job.kind, what, error))
    except Exception as error:  # never leave the main loop waiting for this job
        job.rc = 1
        print('%s: run_suites.py: unexpected error: %r' % (what, error))
        sys.stdout.flush()
        note_failure(job, 'run_suites.py: unexpected error in %s %s: %r' % (job.kind, what, error))
    finally:
        job.end = time.time()
        results.put(job)


def report(suite, finished):
    suite.reported = True
    starts = [c.start for c in suite.compiles if c.start is not None]
    suite.seconds = (time.time() - min(starts)) if starts else 0.0
    finished.append(suite)
    if suite.compiled != 0:
        print('%s: COMPILE FAILED (exit %s) after %.1fs' % (suite.name, suite.compiled, suite.seconds))
    elif suite.ran != 0:
        print('%s: FAILED (exit %s) after %.1fs' % (suite.name, suite.ran, suite.seconds))
    else:
        print('%s: ok [%.1fs]' % (suite.name, suite.seconds))
    sys.stdout.flush()


def settle_compiles(suite, ready, finished):
    """Called once every compile `suite` needs has finished: queues its link,
    or fails it with every compile's output in its log."""
    failed = [c for c in suite.compiles if c.rc != 0]
    if not failed:
        push(ready, SuiteJob('link', PRIORITY_LINK, suite))
        return
    suite.compiled = failed[0].rc
    try:
        with open(suite.log, 'wb') as log:
            write_compile_sections(log, suite)
    except OSError as error:
        print('%s: run_suites.py: could not write %s: %s' % (suite.name, suite.log, error))
    report(suite, finished)


def push(ready, job):
    heapq.heappush(ready, job.key() + (job,))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--suites', default=SUITES_TXT)
    args = parser.parse_args()

    try:
        suites = [Suite(d) for d in parse_suites(args.suites)]
    except ValueError as error:
        print('ERROR: %s' % error)
        return 1

    missing = [s for s in suites for src in [s.unit] + s.extra if not os.path.isfile(src)]
    if missing:
        for s in missing:
            print('ERROR: %s: a listed source does not exist' % s.name)
        return 1

    try:
        compiles = plan(suites)
    except ValueError as error:
        print('ERROR: %s' % error)
        return 1

    os.makedirs(TESTS_DIR, exist_ok=True)
    # A suite that never gets as far as writing its log or linking must not
    # leave the last run's in place to be read as this run's.
    for s in suites:
        for path in (s.log, s.exe):
            try:
                remove_if_present(path)
            except OSError:
                pass

    jobs_raw = os.environ.get('TEST_JOBS', '').strip()
    if jobs_raw:
        if not jobs_raw.isdigit() or int(jobs_raw) < 1:
            print('ERROR: TEST_JOBS must be a whole number >= 1 (got "%s")' % jobs_raw)
            return 1
        jobs = int(jobs_raw)
    else:
        jobs = os.cpu_count() or 1
    jobs = min(jobs, len(compiles))

    print('Running %d suite(s), %d at a time; per-suite logs: build\\tests\\<name>.log' % (len(suites), jobs))
    sys.stdout.flush()

    results = queue.Queue()
    ready = []
    for job in compiles:
        push(ready, job)
    running = 0
    finished = []
    threads = []
    while ready or running:
        while ready and running < jobs:
            job = heapq.heappop(ready)[-1]
            t = threading.Thread(target=execute, args=(job, results))
            threads.append(t)
            t.start()
            running += 1
        job = results.get()
        running -= 1
        if job.kind == 'compile':
            for s in job.users:
                s.waiting -= 1
                if s.waiting == 0:
                    settle_compiles(s, ready, finished)
        elif job.kind == 'link':
            job.suite.compiled = job.rc
            if job.rc == 0:
                push(ready, SuiteJob('run', PRIORITY_RUN, job.suite))
            else:
                report(job.suite, finished)
        else:
            job.suite.ran = job.rc
            report(job.suite, finished)
    for t in threads:
        t.join()

    unreported = [s.name for s in suites if not s.reported]
    if unreported or len(finished) != len(suites):
        print('ERROR: run_suites.py stopped with %d suite(s) never reported: %s'
              % (len(unreported), ', '.join(unreported)))
        return 1

    failed = [s for s in finished if s.compiled != 0 or s.ran != 0]
    if failed:
        print('')
        print('%d of %d suite(s) FAILED: %s' % (len(failed), len(suites), ', '.join(s.name for s in failed)))
        for s in failed:
            print('')
            print('--- %s: last %d lines of %s ---' % (s.name, TAIL_LINES, s.log))
            for line in read_log(s.log)[-TAIL_LINES:]:
                print('    ' + line)
        return 1

    print('')
    print('%d suites, all passed' % len(suites))
    return 0


if __name__ == '__main__':
    sys.exit(main())

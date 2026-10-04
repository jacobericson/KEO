"""Compile and run every host unit-test suite as concurrent processes
(Python 3, stdlib only). Each process it starts holds one host-wide cpu
token from tools\\build\\slots.py (KEO_SLOTS=off turns the pools off);
started under a token itself (KEO_CPU_HELD=1), it runs one job at a time.

Run by tools\\tests\\test_gate.py, which build_tests.bat calls once it has
set up the VS 2010 x64 environment.
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
A job that cannot start its process (cl missing, an output or log locked by
another process) fails that job's suite the same way. Only a cpu token that
does not come within KEO_CPU_WAIT stops the run: no job starts its process
after it, and every suite left unfinished is reported "NOT RUN" and counts
as failed.

The script exits 1 if any suite failed or if the suite list named a source
that does not exist, printing each failed suite's name and the output of the
step that failed it (the failing compiles, the link, or the run's last
lines); it exits 0, printing "N suites, all passed", only when every suite
compiled, ran and returned 0. A suite's "[x.xs]" runs from the start of the
first compile it needs (a shared one included) to the end of its last job.

Usage: python tools\\tests\\run_suites.py [--suites PATH]
Environment: TEST_JOBS (default: os.cpu_count()).
  Test-only: TEST_SUITES_NOOP_CL=1 replaces every compile, and
  TEST_SUITES_NOOP_LINK=1 every link, with a command that exits 0 and writes
  nothing; both must fail the run.
"""
import argparse
import contextlib
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
TAIL_LINES = 30          # a failed run's output: its last lines carry the summary
HEAD_LINES = 60          # failed compiles or link: the first errors are the cause
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
        self.not_run = None    # why a job it needs never started its process
        self.link_at = None    # log offsets where the link's and the run's
        self.run_at = None     # sections start, once written


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
        self.skipped = False

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
    not. Each step deletes its output before it starts, so one left from an
    earlier run cannot pass."""
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


class Skipped(Exception):
    """The run stopped before this job started its process."""


@contextlib.contextmanager
def cpu_token(label, stop):
    """One cpu token for the block, unless the run stopped while this job
    waited for it: then the job starts nothing."""
    with slots.cpu_token(label):
        if stop.is_set():
            raise Skipped()
        yield


def do_compile(job, stop):
    os.makedirs(os.path.dirname(job.obj), exist_ok=True)
    remove_if_present(job.obj)
    command, note = test_command('TEST_SUITES_NOOP_CL', job.cmd)
    with cpu_token('suite compile %s' % job.source, stop):
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, env=slots.child_env(leaf=True))
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


def do_link(job, stop):
    suite = job.suite
    cmd = ['cl', '/nologo'] + [c.obj for c in suite.compiles] + ['/Fe' + suite.exe]
    command, note = test_command('TEST_SUITES_NOOP_LINK', cmd)
    with open(suite.log, 'wb') as log:
        write_compile_sections(log, suite)
        suite.link_at = log.tell()
        log.write(('LINK: %s\n' % ' '.join(cmd)).encode('utf-8') + note)
        log.flush()
        remove_if_present(suite.exe)
        with cpu_token('suite link %s' % suite.name, stop):
            rc = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=slots.child_env(leaf=True))
        if rc != 0:
            return rc
        problem = stale_output(suite.exe, job.start, 'executable')
        if problem:
            log.write(('run_suites.py: link reported success but %s\n' % problem).encode('utf-8'))
            return 1
    return 0


def do_run(job, stop):
    # RUN is written only once the token is held, so a run the stop skips
    # leaves no RUN line with no output after it.
    suite = job.suite
    with cpu_token('suite run %s' % suite.name, stop):
        with open(suite.log, 'ab') as log:
            suite.run_at = log.tell()
            log.write(b'RUN\n')
            log.flush()
            return subprocess.call([suite.exe], stdout=log, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, env=slots.child_env(leaf=True))


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


class Stop(object):
    """Set by the first job whose cpu token did not come within KEO_CPU_WAIT:
    no job starts a process after it, so a jammed slot pool costs one token
    wait, not one per remaining job. Any other launch error fails only its
    own job."""

    def __init__(self):
        self._event = threading.Event()
        self._lock = threading.Lock()
        self.reason = None

    def set(self, reason):
        with self._lock:
            if self.reason is None:
                self.reason = reason
        self._event.set()

    def is_set(self):
        return self._event.is_set()


def execute(job, results, stop):
    """Always puts (job) on the queue exactly once, whatever happens: the main
    loop waits for one report per started job, so an uncaught exception here
    (cl missing from PATH, a log-open failure, ...) would otherwise hang the
    whole run on results.get() forever instead of failing it."""
    job.start = time.time()
    what = job.source if job.kind == 'compile' else job.suite.name
    try:
        if stop.is_set():
            raise Skipped()
        if job.kind == 'compile':
            job.rc = do_compile(job, stop)
        elif job.kind == 'link':
            job.rc = do_link(job, stop)
        else:
            job.rc = do_run(job, stop)
    except Skipped:
        job.rc = 1
        job.skipped = True
        note_failure(job, 'run_suites.py: %s %s not started: the run stopped' % (job.kind, what))
    except slots.SlotTimeout as error:
        job.rc = 1
        message = 'run_suites.py: could not %s %s: %s' % (job.kind, what, error)
        stop.set(message)
        note_failure(job, message)
    except (OSError, ValueError) as error:
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
    if suite.not_run:
        print('%s: NOT RUN (%s) after %.1fs' % (suite.name, suite.not_run, suite.seconds))
    elif suite.compiled != 0:
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


def note_not_run(suite):
    try:
        with open(suite.log, 'ab') as log:
            if log.tell() == 0:
                log.write(('=== %s ===\n' % suite.name).encode('utf-8'))
            log.write(('run_suites.py: not run: %s\n' % suite.not_run).encode('utf-8'))
    except OSError as error:
        print('%s: run_suites.py: could not write %s: %s' % (suite.name, suite.log, error))


def read_from(path, offset):
    try:
        with open(path, 'rb') as f:
            f.seek(offset)
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def capped(lines, path, head):
    """At most HEAD_LINES of `lines` from the start, or TAIL_LINES from the end."""
    limit = HEAD_LINES if head else TAIL_LINES
    if len(lines) <= limit:
        return lines
    more = '... %d more line(s) in %s' % (len(lines) - limit, path)
    return lines[:limit] + [more] if head else [more] + lines[-limit:]


def failure_output(suite):
    """(title, lines) of the step that failed `suite`: its failed compiles,
    its link, or the end of its run; the log's tail when none of those
    applies (a suite that never ran a step)."""
    failed = []
    for job in suite.compiles:
        if job.rc not in (0, None) and not job.skipped and job not in failed:
            failed.append(job)
    if not suite.not_run and failed:
        lines = []
        for job in failed:
            lines.append('CL: %s' % ' '.join(job.cmd))
            lines += job.output.decode('mbcs', errors='replace').splitlines()
        return 'failed compile(s), from %s' % suite.log, capped(lines, suite.log, True)
    if not suite.not_run and suite.compiled not in (0, None) and suite.link_at is not None:
        return 'link, from %s' % suite.log, capped(read_from(suite.log, suite.link_at), suite.log, True)
    if not suite.not_run and suite.ran not in (0, None) and suite.run_at is not None:
        lines = capped(read_from(suite.log, suite.run_at), suite.log, False)
        return 'run, last %d lines of %s' % (TAIL_LINES, suite.log), lines
    return 'last %d lines of %s' % (TAIL_LINES, suite.log), read_log(suite.log)[-TAIL_LINES:]


def run_jobs(compiles, jobs, finished):
    """Runs the job graph from `compiles`, at most `jobs` at once, reporting
    each suite into `finished` as it settles. Returns the run's Stop: once a
    cpu token wait runs out, nothing new starts, and the suites left
    unsettled are the caller's to report."""
    results = queue.Queue()
    stop = Stop()
    ready = []
    for job in compiles:
        push(ready, job)
    running = 0
    announced = False
    threads = []
    while running or (ready and not stop.is_set()):
        while ready and running < jobs and not stop.is_set():
            job = heapq.heappop(ready)[-1]
            t = threading.Thread(target=execute, args=(job, results, stop))
            threads.append(t)
            t.start()
            running += 1
        if not running:
            break
        job = results.get()
        running -= 1
        if stop.is_set() and not announced:
            announced = True
            print('run_suites.py: stopping, no further job starts: %s' % stop.reason)
            sys.stdout.flush()
        if job.skipped:
            for s in (job.users if job.kind == 'compile' else [job.suite]):
                s.not_run = 'the run stopped'
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
    return stop


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
    # Launched under someone else's cpu token, no child here takes a token of
    # its own, so only one may run at a time.
    under_token = slots.cpu_held() and jobs > 1
    if under_token:
        jobs = 1

    print('Running %d suite(s), %d at a time; per-suite logs: build\\tests\\<name>.log' % (len(suites), jobs))
    if under_token:
        print('run_suites.py: running under a cpu token (KEO_CPU_HELD=1), one job at a time')
    sys.stdout.flush()

    finished = []
    stop = run_jobs(compiles, jobs, finished)

    unreported = [s for s in suites if not s.reported]
    lost = unreported and not stop.is_set()
    if lost:
        print('ERROR: run_suites.py stopped with %d suite(s) never reported: %s'
              % (len(unreported), ', '.join(s.name for s in unreported)))
    for s in unreported:
        s.not_run = s.not_run or ('the run stopped' if stop.is_set() else 'never reported')
        note_not_run(s)
        report(s, finished)
    consistent = len(finished) == len(suites) and not lost
    if not consistent and not lost:
        print('ERROR: run_suites.py reported %d suite result(s) for %d suite(s)' % (len(finished), len(suites)))

    failed = [s for s in finished if s.not_run or s.compiled != 0 or s.ran != 0]
    if failed or not consistent:
        print('')
        print('%d of %d suite(s) FAILED: %s' % (len(failed), len(suites), ', '.join(s.name for s in failed)))
        for s in failed:
            title, lines = failure_output(s)
            print('')
            print('--- %s: %s ---' % (s.name, title))
            for line in lines:
                print('    ' + line)
        return 1

    print('')
    print('%d suites, all passed' % len(suites))
    return 0


if __name__ == '__main__':
    sys.exit(main())

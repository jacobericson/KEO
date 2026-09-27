"""Compile and run every host unit-test suite as concurrent processes
(Python 3, stdlib only), modelled on tools\\build\\run_variants.py.

Called by build_tests.bat after it has set up the VS 2010 x64 environment.
Suites are data, not code: tools\\tests\\suites.txt lists one per line as

    name | unit .cpp | extra sources | defines

"name" also names the .exe (build\\tests\\<name>.exe) and its own build log
(build\\tests\\<name>.log). "unit .cpp" and each entry of "extra sources" are
paths from the repo root. "defines" is optional extra /D flags; most suites
have none. Blank lines and lines starting with # are ignored.

Each suite runs as up to two processes -- compile, then (on success) run --
capped at TEST_JOBS processes at once (default: logical core count). A suite
that fails to compile or run is recorded and the suite count continues; every
other suite still gets its turn (no suite is skipped because an earlier one
failed), so one run reports every failure at once. The script exits 1 if any
suite failed or if suites.txt named a source that does not exist, printing
each failed suite's name and its log's tail; it exits 0, printing "N suites,
all passed", only when every suite compiled, ran and returned 0.

Usage: python tools\\tests\\run_suites.py [--suites PATH]
Environment: TEST_JOBS (default: os.cpu_count()), TEST_MP (unused; suites are
  single-file compiles, so cl /MP has nothing to parallelize within one).
"""
import argparse
import os
import queue
import subprocess
import sys
import threading
import time

SUITES_TXT = r'tools\tests\suites.txt'
TAIL_LINES = 30


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
        self.defines = data['defines']
        # Each suite's objects go in their own subfolder: two suites can name
        # the same shared source (zone_prep_ledger.cpp, unstitch_probe_policy.cpp,
        # ...), and cl always names an object after its source's basename, so
        # running them concurrently into one shared build\tests\ would have
        # two cl processes fighting over the same .obj path ("Cannot open
        # compiler generated file ... Permission denied"). The serial
        # build_tests.bat never hit this (one cl at a time, harmlessly
        # overwriting); running suites in parallel makes it a race.
        self.objdir = os.path.join('build', 'tests', 'obj', self.name)
        self.exe = os.path.join('build', 'tests', self.name + '.exe')
        self.log = os.path.join('build', 'tests', self.name + '.log')
        self.compiled = None   # returncode, once known
        self.ran = None        # returncode, once known
        self.seconds = 0.0
        self.start = None


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def run_suite(suite, results):
    """Always puts (suite) on the queue exactly once, whatever happens: the
    main loop waits for one report per started suite, so an uncaught
    exception here (cl missing from PATH, a log-open failure, ...) would
    otherwise hang the whole run on results.get() forever instead of failing
    it. Mirrors run_variants.run_task's try/except/finally shape."""
    start = time.time()
    try:
        os.makedirs(suite.objdir, exist_ok=True)
        sources = [suite.unit] + suite.extra
        cmd_compile = ['cl', '/nologo', '/EHsc', '/O2', '/W3', '/Isrc'] + \
            (suite.defines.split() if suite.defines else []) + sources + \
            ['/Fo' + suite.objdir + '\\', '/Fe' + suite.exe]
        with open(suite.log, 'wb') as log:
            log.write(('=== %s ===\nCL: %s\n' % (suite.name, ' '.join(cmd_compile))).encode('utf-8'))
            log.flush()
            rc = subprocess.call(cmd_compile, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        suite.compiled = rc
        if rc == 0:
            with open(suite.log, 'ab') as log:
                log.write(b'RUN\n')
                log.flush()
                rc = subprocess.call([suite.exe], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            suite.ran = rc
    except (OSError, ValueError) as error:
        suite.compiled = suite.compiled if suite.compiled is not None else 1
        message = 'run_suites.py: could not run %s: %s' % (suite.name, error)
        try:
            with open(suite.log, 'ab') as log:
                log.write((message + '\n').encode('utf-8', 'replace'))
        except OSError:
            print(message)
            sys.stdout.flush()
    except Exception as error:  # never leave the main loop waiting for this suite
        suite.compiled = suite.compiled if suite.compiled is not None else 1
        print('%s: run_suites.py: unexpected error: %r' % (suite.name, error))
        sys.stdout.flush()
    finally:
        suite.seconds = time.time() - start
        results.put(suite)


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

    os.makedirs(os.path.join('build', 'tests'), exist_ok=True)

    jobs_raw = os.environ.get('TEST_JOBS', '').strip()
    if jobs_raw:
        if not jobs_raw.isdigit() or int(jobs_raw) < 1:
            print('ERROR: TEST_JOBS must be a whole number >= 1 (got "%s")' % jobs_raw)
            return 1
        jobs = int(jobs_raw)
    else:
        jobs = os.cpu_count() or 1
    jobs = min(jobs, len(suites))

    print('Running %d suite(s), %d at a time; per-suite logs: build\\tests\\<name>.log' % (len(suites), jobs))
    sys.stdout.flush()

    results = queue.Queue()
    pending = list(suites)
    running = 0
    finished = []
    threads = []
    while pending or running:
        while pending and running < jobs:
            s = pending.pop(0)
            t = threading.Thread(target=run_suite, args=(s, results))
            threads.append(t)
            t.start()
            running += 1
        s = results.get()
        running -= 1
        finished.append(s)
        if s.compiled != 0:
            print('%s: COMPILE FAILED (exit %s) after %.1fs' % (s.name, s.compiled, s.seconds))
        elif s.ran != 0:
            print('%s: FAILED (exit %s) after %.1fs' % (s.name, s.ran, s.seconds))
        else:
            print('%s: ok [%.1fs]' % (s.name, s.seconds))
        sys.stdout.flush()
    for t in threads:
        t.join()

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

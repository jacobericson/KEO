"""Build and run every injection harness (Python 3, standard library only).

Called by tools\\tests\\run_injections.bat after it has set up the VS 2010 x64 environment.
Harnesses are data: tools\\tests\\injections.txt lists one per line as

    name | EH mode | defines | include directories | sources | evidence

Blank lines and lines starting with # are ignored; every field is stripped, and defines and
include directories may be empty. Defines, include directories and sources are space-separated.
The first source must be tools\\tests\\<name>.cpp, and every tools\\tests\\*_injection.cpp must be
the first source of one row. A %NAME% in an include directory is expanded from the environment;
KENSHILIB, when unset, first defaults to default_kenshilib(<repository root>) from
tools\\kenshilib\\resources_root.py, the same resources folder resources_root.bat finds for the
batch scripts at any worktree depth. The whole table is checked before anything is built, and
every refusal is printed, in table order; any refusal ends the run with exit 1.

Each row compiles as

    cl /nologo <EH> /O2 /W3 <defines> /DWIN32_LEAN_AND_MEAN /Isrc /I"<dir>"... <sources>
       /Fobuild\\tests\\inj\\<name>\\ /Febuild\\tests\\inj\\<name>.exe

and, when that succeeds, runs build\\tests\\inj\\<name>.exe from the repository root, killed after
RUN_TIMEOUT_S. Its log build\\tests\\inj\\<name>.log starts with '=== <name> ===' and
'CL: <command>', then holds the compiler's output, then the run's stdout and stderr. A row passes
only when its harness exits 0 and its output contains the evidence text. Rows build and run on
threads capped at TEST_JOBS (default os.cpu_count()), each into its own object folder, since two
harnesses may share a source and cl names every object after its source.

Every run first proves the classification on two scratch harnesses under
build\\tests\\inj\\_selftest\\ (one exits 0 without the evidence, one lets an access violation
escape), and stops with exit 1 if either is accepted.

Usage: python tools\\tests\\run_injections.py [--list FILE]
Exit 0 only when every row passed.
"""
import sys
sys.dont_write_bytecode = True

import argparse  # noqa: E402
import ctypes  # noqa: E402
import glob  # noqa: E402
import os  # noqa: E402
import queue  # noqa: E402
import re  # noqa: E402
import subprocess  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'kenshilib'))

import resources_root  # noqa: E402

LIST_TXT = r'tools\tests\injections.txt'
OUT_DIR = r'build\tests\inj'
SELFTEST_DIR = OUT_DIR + r'\_selftest'
RUN_TIMEOUT_S = 120
TAIL_LINES = 30
ACCESS_VIOLATION = 3221225477
SEM_FAILCRITICALERRORS = 0x0001
SEM_NOGPFAULTERRORBOX = 0x0002
FIELDS = 6
VAR_RE = re.compile(r'%([^%]+)%')

SELFTEST_EVIDENCE = 'fault taken'
SELFTEST_SILENT = '''\
// _selftest_silent.cpp: passes its own checks and never faults.
#include <cstdio>
int main() { std::printf("_selftest_silent: all checks passed\\n"); return 0; }
'''
SELFTEST_ESCAPE = '''\
// _selftest_escape.cpp: its fault is never caught. The top-level filter ends
// the process with the exception code as the exit code, and no dialog.
#include <windows.h>
static LONG WINAPI Filter(EXCEPTION_POINTERS*) { return EXCEPTION_EXECUTE_HANDLER; }
int main() { SetUnhandledExceptionFilter(Filter); volatile int* p = 0; return *p; }
'''


class Row(object):
    def __init__(self, name, eh, defines, includes, sources, evidence, outdir=OUT_DIR):
        self.name = name
        self.eh = eh
        self.defines = defines
        self.includes = includes
        self.sources = sources
        self.evidence = evidence
        self.objdir = outdir + '\\' + name + '\\'
        self.exe = outdir + '\\' + name + '.exe'
        self.log = outdir + '\\' + name + '.log'
        self.passed = False
        self.text = ''
        self.seconds = 0.0

    def command(self):
        return ' '.join(['cl', '/nologo', self.eh, '/O2', '/W3'] + self.defines +
                        ['/DWIN32_LEAN_AND_MEAN', '/Isrc'] +
                        ['/I"%s"' % d for d in self.includes] + self.sources +
                        ['/Fo' + self.objdir, '/Fe' + self.exe])


def expand(token):
    """%NAME% replaced from the environment; an unset name stays as written, so its path fails."""
    return VAR_RE.sub(lambda m: os.environ.get(m.group(1), m.group(0)), token)


def parse_table(path, display=None):
    """(rows, refusals): the table's rows and every refusal line found, in table order. Paths
    in the table are read from the current directory; display names the table in refusals."""
    display = display or path
    rows = []
    refusals = []
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8').replace('\r\n', '\n')
    names = set()
    for lineno, raw in enumerate(text.split('\n'), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split('|')]
        if len(parts) != FIELDS:
            refusals.append('%s:%d: expected %d "|"-separated fields, got %d'
                            % (display, lineno, FIELDS, len(parts)))
            continue
        name, eh, defines, includes, sources, evidence = parts
        if not name or not eh or not sources or not evidence:
            refusals.append('%s:%d: name, EH mode, sources and evidence must not be empty'
                            % (display, lineno))
            continue
        if name in names:
            refusals.append('%s:%d: duplicate name %s' % (display, lineno, name))
            continue
        names.add(name)
        row = Row(name, eh, defines.split(), [expand(d) for d in includes.split()],
                  sources.split(), evidence)
        first = 'tools\\tests\\%s.cpp' % name
        if row.sources[0] != first:
            refusals.append('%s: the first source must be %s' % (name, first))
        for src in row.sources:
            if not os.path.isfile(src):
                refusals.append('%s: source %s does not exist' % (name, src))
        for d in row.includes:
            if not os.path.isdir(d):
                refusals.append('%s: include directory %s does not exist' % (name, d))
        rows.append(row)
    return rows, refusals


def unlisted(rows):
    """The tools/tests/*_injection.cpp files that are no row's first source."""
    firsts = set(os.path.normcase(r.sources[0]) for r in rows)
    found = sorted(glob.glob(os.path.join('tools', 'tests', '*_injection.cpp')))
    return found, [p for p in found if os.path.normcase(p) not in firsts]


def classify(compile_rc, run_rc, timed_out, output, evidence):
    """(passed, text) for one harness, the first matching case in this order."""
    if compile_rc != 0:
        return False, 'COMPILE FAILED (exit %s)' % compile_rc
    if timed_out:
        return False, 'FAILED (no exit after %d s)' % RUN_TIMEOUT_S
    if run_rc == ACCESS_VIOLATION or run_rc == ACCESS_VIOLATION - (1 << 32):
        return False, 'FAILED (exit 0xC0000005: an access violation escaped the harness)'
    if run_rc != 0:
        return False, 'FAILED (exit %s)' % run_rc
    if evidence not in output:
        return False, 'FAILED (exit 0 without its evidence "%s")' % evidence
    return True, 'ok (evidence "%s")' % evidence


def build_and_run(row):
    """Compiles and runs one row into its log, then sets row.passed and row.text."""
    start = time.time()
    compile_rc, run_rc, timed_out, output = 1, None, False, ''
    try:
        if not os.path.isdir(row.objdir):
            os.makedirs(row.objdir)
        command = row.command()
        with open(row.log, 'wb') as log:
            log.write(('=== %s ===\nCL: %s\n' % (row.name, command)).encode('utf-8'))
            log.flush()
            compile_rc = subprocess.call(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                         stdin=subprocess.DEVNULL)
        if compile_rc == 0:
            proc = subprocess.Popen([os.path.join(ROOT, row.exe)], cwd=ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            try:
                data, _ = proc.communicate(timeout=RUN_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.kill()
                data, _ = proc.communicate()
                timed_out = True
            run_rc = proc.returncode
            with open(row.log, 'ab') as log:
                log.write(b'RUN\n')
                log.write(data)
            output = data.decode('mbcs', errors='replace')
        row.passed, row.text = classify(compile_rc, run_rc, timed_out, output, row.evidence)
    except Exception as error:  # a row that could not run fails; it never passes silently
        row.passed, row.text = False, 'FAILED (runner error: %r)' % (error,)
    row.seconds = time.time() - start


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def self_test():
    """None when both scratch harnesses are refused as expected, else the failure's description."""
    if not os.path.isdir(SELFTEST_DIR):
        os.makedirs(SELFTEST_DIR)
    cases = [('_selftest_silent', SELFTEST_SILENT,
              'FAILED (exit 0 without its evidence "%s")' % SELFTEST_EVIDENCE),
             ('_selftest_escape', SELFTEST_ESCAPE,
              'FAILED (exit 0xC0000005: an access violation escaped the harness)')]
    for name, source, expected in cases:
        path = SELFTEST_DIR + '\\' + name + '.cpp'
        with open(path, 'w') as f:
            f.write(source)
        row = Row(name, '/EHa', [], [], [path], SELFTEST_EVIDENCE, SELFTEST_DIR)
        build_and_run(row)
        if row.passed or row.text != expected:
            return '%s classified as "%s", expected "%s" (log %s)' % (name, row.text, expected, row.log)
    return None


def jobs_from_env(count):
    raw = os.environ.get('TEST_JOBS', '').strip()
    if raw:
        if not raw.isdigit() or int(raw) < 1:
            raise ValueError('TEST_JOBS must be a whole number >= 1 (got "%s")' % raw)
        jobs = int(raw)
    else:
        jobs = os.cpu_count() or 1
    return max(1, min(jobs, count))


def run_rows(rows, jobs):
    results = queue.Queue()
    pending = list(rows)
    running = 0

    def work(row):
        try:
            build_and_run(row)
        finally:
            results.put(row)

    threads = []
    while pending or running:
        while pending and running < jobs:
            t = threading.Thread(target=work, args=(pending.pop(0),))
            threads.append(t)
            t.start()
            running += 1
        row = results.get()
        running -= 1
        print('%s: %s [%.1fs]' % (row.name, row.text, row.seconds))
        sys.stdout.flush()
    for t in threads:
        t.join()


def main():
    ctypes.windll.kernel32.SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--list', default=LIST_TXT)
    args = parser.parse_args()
    list_name = args.list
    list_path = os.path.abspath(list_name)
    os.chdir(ROOT)

    if not os.environ.get('KENSHILIB'):
        os.environ['KENSHILIB'] = resources_root.default_kenshilib(ROOT)

    try:
        rows, refusals = parse_table(list_path, list_name)
    except (OSError, UnicodeDecodeError) as error:
        print('run_injections: %s: cannot read: %s' % (list_name, error))
        return 1
    found, missing = unlisted(rows)
    for path in missing:
        refusals.append('%s has no row in %s' % (path.replace('\\', '/'), list_name))
    if not rows:
        refusals.append('%s: no rows' % list_name)
    if refusals:
        for line in refusals:
            print('run_injections: ' + line)
        return 1
    print('run_injections: %d row(s) in %s, %d *_injection.cpp file(s), none unlisted'
          % (len(rows), list_name, len(found)))
    sys.stdout.flush()

    try:
        jobs = jobs_from_env(len(rows))
    except ValueError as error:
        print('run_injections: %s' % error)
        return 1

    failure = self_test()
    if failure:
        print('run_injections: SELF-TEST FAILED: %s' % failure)
        return 1
    print('run_injections: self-test: a harness that exited 0 without its evidence was refused')
    print('run_injections: self-test: an access violation that escaped its harness was refused')
    sys.stdout.flush()

    run_rows(rows, jobs)

    failed = [r for r in rows if not r.passed]
    if failed:
        print('run_injections: %d of %d harness(es) FAILED: %s'
              % (len(failed), len(rows), ', '.join(r.name for r in failed)))
        for r in failed:
            print('--- %s: last %d lines of %s ---' % (r.name, TAIL_LINES, r.log))
            for line in read_log(r.log)[-TAIL_LINES:]:
                print('    ' + line)
        return 1
    print('run_injections: %d harnesses, all passed' % len(rows))
    return 0


if __name__ == '__main__':
    sys.exit(main())

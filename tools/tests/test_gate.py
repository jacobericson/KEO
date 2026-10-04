"""The host test gate: the guards, the C++ suites and the Python tests as three concurrent
processes, their output printed afterwards in a fixed order (Python 3, standard library only).

    python tools\\tests\\test_gate.py [--since REV]

Run by build_tests.bat once the VS 2010 x64 environment is set up. Every child runs in the
repository root, whatever the caller's directory.

The suite list is tools\\tests\\suites.txt; where tools\\tests\\suites_private.txt exists, the
list is build\\tests\\suites_merged.txt instead, written as suites.txt's bytes, a CRLF, then the
private list's bytes.

    phase   process                                  log (stdout and stderr)
    guards  check_test_guards.py                     build\\tests\\gate-guards.log
    suites  run_suites.py --suites <list>            build\\tests\\gate-suites.log
    python  run_py_tests.py [--since REV]            build\\tests\\gate-python.log

The three start together, the Python tests (the longest) first. Each is a runner that takes its
own host-wide cpu tokens (tools\\build\\slots.py), so none starts under one; the gate holds one
heavy slot for the whole run unless it already runs under one. PY_TESTS_SINCE and every other
setting reach the children through the environment.

When all three have finished, the guards log is printed, then "build_tests: merging the private
suite list suites_private.txt" when the lists were merged, then the suites log and the Python log,
then "build_tests: guards <a> s, suites <b> s, python <c> s". A phase passes only when its
process exited 0 and the last line of its log is its closing line ("check_test_guards: all checks
passed", "<S> suites, all passed", "python tests: <names> OK"); one that could not start, crashed,
was killed or left an empty log fails. Each failed phase gets a "build_tests: <phase> failed:
<reason>" line, then "build_tests: FAILED: <phase>[, <phase>]" ends the output and the exit code
is 1.

Test-only: TEST_GATE_FORCE_FAIL=<phase>[,<phase>] replaces each named phase with a process that
prints a marker and exits 1; TEST_GATE_NO_CLOSE=<phase>[,<phase>] with one that prints a marker
and exits 0 without the closing line. Both must fail the run.
"""
import argparse
import os
import re
import subprocess
import sys
import time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir))
sys.path.insert(0, os.path.join(REPO, 'tools', 'build'))
import slots  # noqa: E402

SUITES = r'tools\tests\suites.txt'
PRIVATE_SUITES = r'tools\tests\suites_private.txt'
MERGED_SUITES = r'build\tests\suites_merged.txt'
LOG_DIR = os.path.join(REPO, 'build', 'tests')
MERGE_LINE = 'build_tests: merging the private suite list suites_private.txt'
PHASES = ('guards', 'suites', 'python')      # the order their logs are printed in
START_ORDER = ('python', 'suites', 'guards')
CLOSING = {
    'guards': re.compile(r'^check_test_guards: all checks passed$'),
    'suites': re.compile(r'^\d+ suites, all passed$'),
    'python': re.compile(r'^python tests: .* OK$'),
}
POLL_S = 0.1


class Phase(object):
    def __init__(self, name, cmd):
        self.name = name
        self.cmd = cmd
        self.log = os.path.join(LOG_DIR, 'gate-%s.log' % name)
        self.proc = None
        self.start = None
        self.end = None
        self.rc = None
        self.error = None

    def seconds(self):
        if self.start is None or self.end is None:
            return 0.0
        return self.end - self.start


def test_phases(variable):
    """The phases a test-only variable names; a name that is no phase is refused."""
    raw = os.environ.get(variable, '').strip()
    if not raw:
        return set()
    names = set(p.strip() for p in raw.split(','))
    unknown = sorted(names - set(PHASES))
    if unknown:
        raise ValueError('%s=%s: %s is not a phase (%s)' % (variable, raw, ', '.join(unknown),
                                                            ', '.join(PHASES)))
    return names


def merge_suites():
    """(suite list path, whether the private list was merged in)."""
    private = os.path.join(REPO, PRIVATE_SUITES)
    if not os.path.exists(private):
        return SUITES, False
    with open(os.path.join(REPO, SUITES), 'rb') as f:
        public = f.read()
    with open(private, 'rb') as f:
        extra = f.read()
    os.makedirs(LOG_DIR, exist_ok=True)
    with open(os.path.join(REPO, MERGED_SUITES), 'wb') as f:
        f.write(public + b'\r\n' + extra)
    return MERGED_SUITES, True


def commands(suite_list, since, forced, no_close):
    py = [sys.executable, '-u']
    cmds = {
        'guards': py + [r'tools\tests\check_test_guards.py'],
        'suites': py + [r'tools\tests\run_suites.py', '--suites', suite_list],
        'python': py + [r'tools\tests\run_py_tests.py'] + (['--since', since] if since else []),
    }
    for name in PHASES:
        if name in forced:
            cmds[name] = py + ['-c', 'print("test_gate: TEST_GATE_FORCE_FAIL replaced the %s phase")\n'
                                     'raise SystemExit(1)' % name]
        elif name in no_close:
            cmds[name] = py + ['-c', 'print("test_gate: TEST_GATE_NO_CLOSE replaced the %s phase")\n'
                                     'raise SystemExit(0)' % name]
    return cmds


def start(phase, env):
    phase.start = time.monotonic()
    try:
        with open(phase.log, 'wb') as log:
            phase.proc = subprocess.Popen(phase.cmd, cwd=REPO, env=env, stdout=log,
                                          stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    except OSError as exc:
        phase.error = 'could not start: %s' % exc
        phase.end = time.monotonic()


def wait_all(phases):
    """Polls rather than blocking in wait(), so an interrupt reaches this thread at once."""
    while True:
        running = 0
        for p in phases:
            if p.proc is None or p.rc is not None:
                continue
            rc = p.proc.poll()
            if rc is None:
                running += 1
            else:
                p.rc = rc
                p.end = time.monotonic()
        if not running:
            return
        time.sleep(POLL_S)


def stop_all(phases):
    for p in phases:
        if p.proc is not None and p.proc.poll() is None:
            try:
                p.proc.kill()
                p.proc.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                pass


def read_log(phase):
    try:
        with open(phase.log, 'rb') as f:
            return f.read()
    except OSError:
        return None


def judge(phase):
    """None when the phase passed, else why not."""
    if phase.error:
        return phase.error
    if phase.rc != 0:
        return 'exit %s' % phase.rc
    data = read_log(phase)
    if data is None:
        return 'no log (%s)' % phase.log
    lines = [l.strip() for l in data.decode('utf-8', 'replace').splitlines() if l.strip()]
    if not lines:
        return 'exit 0 with an empty log'
    if not CLOSING[phase.name].match(lines[-1]):
        return 'exit 0 without its closing line (its last line: %s)' % lines[-1][:160]
    return None


def write_raw(data):
    sys.stdout.flush()
    if data:
        sys.stdout.buffer.write(data)
        if not data.endswith(b'\n'):
            sys.stdout.buffer.write(os.linesep.encode('ascii'))
    sys.stdout.buffer.flush()


def say(line):
    print(line)
    sys.stdout.flush()


def run(since, forced, no_close):
    try:
        suite_list, merged = merge_suites()
    except OSError as exc:
        say('build_tests: could not merge the private suite list: %s' % exc)
        say('build_tests: FAILED: merge')
        return 1
    cmds = commands(suite_list, since, forced, no_close)
    by_name = dict((name, Phase(name, cmds[name])) for name in PHASES)
    phases = [by_name[name] for name in PHASES]
    env = slots.child_env(leaf=False)
    for p in phases:
        try:
            os.remove(p.log)
        except FileNotFoundError:
            pass
    try:
        for name in START_ORDER:
            start(by_name[name], env)
        wait_all(phases)
    except BaseException:
        stop_all(phases)
        raise

    for p in phases:
        data = read_log(p)
        if data is None:
            say('build_tests: %s: no log (%s)' % (p.name, p.log))
        else:
            write_raw(data)
        if p.name == 'guards' and merged:
            say(MERGE_LINE)
    say('build_tests: %s' % ', '.join('%s %.1f s' % (p.name, p.seconds()) for p in phases))
    failed = []
    for p in phases:
        why = judge(p)
        if why:
            failed.append(p.name)
            say('build_tests: %s failed: %s' % (p.name, why))
    if failed:
        say('build_tests: FAILED: %s' % ', '.join(failed))
        return 1
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='Run the host test gate.')
    ap.add_argument('--since', metavar='REV', help='passed on to run_py_tests.py')
    args = ap.parse_args(argv)
    try:
        forced = test_phases('TEST_GATE_FORCE_FAIL')
        no_close = test_phases('TEST_GATE_NO_CLOSE')
        with slots.heavy('build_tests'):
            return run(args.since, forced, no_close)
    except slots.SlotTimeout as exc:
        say('build_tests: FAILED: no heavy slot (%s)' % exc)
        return 1
    except ValueError as exc:
        say('build_tests: FAILED: %s' % exc)
        return 1
    except KeyboardInterrupt:
        say('build_tests: FAILED: interrupted')
        return 1


if __name__ == '__main__':
    sys.exit(main())

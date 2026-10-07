"""The host test gate: the guards, the C++ suites and the Python tests as three concurrent
processes, their output printed afterwards in a fixed order (Python 3, standard library only).

    python tools\\tests\\test_gate.py [--since REV | --all]
    python tools\\tests\\test_gate.py --only NAME[,NAME]

Run by build_tests.bat once the VS 2010 x64 environment is set up. Every child runs in the
repository root, whatever the caller's directory.

The suite list is tools\\tests\\suites.txt; where tools\\tests\\suites_private.txt exists, the
list is build\\tests\\suites_merged.txt instead, written as suites.txt's bytes, a CRLF, then the
private list's bytes.

    phase   process                                  log (stdout and stderr)
    guards  check_test_guards.py [--since REV]       build\\tests\\gate-guards.log
    suites  run_suites.py --suites <list>            build\\tests\\gate-suites.log
    python  run_py_tests.py [--since REV]            build\\tests\\gate-python.log

The three start together, the Python tests (the longest) first. Each is a runner that takes its
own host-wide cpu tokens (tools\\build\\slots.py), so none starts under one. A gate started under
someone else's cpu token (KEO_CPU_HELD=1) passes that on, so each runner runs one job at a time,
and runs the three one after another instead, in the printed order. The gate holds one heavy slot
for the whole run unless it already runs under one. Children write UTF-8 (PYTHONIOENCODING=utf-8;
PYTHONUTF8 as inherited) and the gate prints their logs as those bytes, its own lines in UTF-8 too.
Every other setting reaches the children through the environment. On an interrupt or an error
of its own, the gate kills each phase's whole process tree.

The Python tests run with selection (run_py_tests.py --since <rev>) from the first of: --since,
PY_TESTS_SINCE, and the merge-base of HEAD with main. There is no default when that merge-base is
HEAD itself or git cannot name it (main missing, git failing); then every module runs. --all runs
every module and is refused together with --since. The choice is printed first, as one
"build_tests: python selection since <rev> (<source>)" or "build_tests: python selection off
(<why>), running every module" line, and PY_TESTS_SINCE never reaches a child. The same rev goes
to the guards, which then skip their runner controls when no runner file changed since it.

When all three have finished, the guards log is printed, then "build_tests: merging the private
suite list suites_private.txt" when the lists were merged, then the suites log and the Python log,
then "build_tests: guards <a> s, suites <b> s, python <c> s". A phase passes only when its
process exited 0 and the last line of its log is its closing line ("check_test_guards: all checks
passed", "<S> suites, all passed", "python tests: <names> OK"); one that could not start, crashed,
was killed or left an empty log fails. Each failed phase gets a "build_tests: <phase> failed:
<reason>" line, then "build_tests: FAILED: <phase>[, <phase>]" ends the output and the exit code
is 1.

--only runs just the names given: each is a suite of the suite list above, else the stem of one
module in py_tests.txt or py_tests_private.txt. The rows are written verbatim to
build\\tests\\suites_only.txt and build\\tests\\py_tests_only.txt, and run through
run_suites.py --suites and run_py_tests.py --list (no selection: every row listed runs). The
guards do not run and no heavy slot is taken; the runners still take their cpu tokens. The last
line is "build_tests: only <names>: suites <a> s, python <b> s", naming only the phases that ran,
so an --only run never reads as a full one. An unknown name, a stem two modules share, an empty
list, or --only with --all or --since is refused: "build_tests: FAILED: <why>", exit 1.

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
import test_gate_select  # noqa: E402

SUITES = r'tools\tests\suites.txt'
PRIVATE_SUITES = r'tools\tests\suites_private.txt'
MERGED_SUITES = r'build\tests\suites_merged.txt'
PY_LISTS = (r'tools\tests\py_tests.txt', r'tools\tests\py_tests_private.txt')
ONLY_SUITES = r'build\tests\suites_only.txt'
ONLY_PY = r'build\tests\py_tests_only.txt'
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


def log_path(name):
    return os.path.join(LOG_DIR, 'gate-%s.log' % name)


class Phase(object):
    def __init__(self, name, cmd):
        self.name = name
        self.cmd = cmd
        self.log = log_path(name)
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


def git_resolve():
    """(merge-base of HEAD with main, HEAD) as full shas; RuntimeError names what failed."""
    def rev(*args):
        try:
            p = subprocess.run(['git', '-C', REPO] + list(args), stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
        except OSError as exc:
            raise RuntimeError('git did not start: %s' % exc)
        out = p.stdout.decode('utf-8', 'replace').strip()
        if p.returncode != 0 or not out:
            raise RuntimeError('git %s failed' % ' '.join(args))
        return out
    try:
        rev('rev-parse', '--verify', '--quiet', 'main^{commit}')
    except RuntimeError:
        raise RuntimeError('main does not resolve')
    return rev('merge-base', 'HEAD', 'main'), rev('rev-parse', 'HEAD')


def merge_suites():
    """(suite list path, whether the private list was merged in)."""
    private = os.path.join(REPO, PRIVATE_SUITES)
    if not os.path.exists(private):
        return SUITES, False
    with open(os.path.join(REPO, SUITES), 'rb') as f:
        public = f.read()
    with open(private, 'rb') as f:
        extra = f.read()
    with open(os.path.join(REPO, MERGED_SUITES), 'wb') as f:
        f.write(public + b'\r\n' + extra)
    return MERGED_SUITES, True


def commands(suite_list, since, forced, no_close, py_list=None):
    py = [sys.executable, '-u']
    cmds = {
        'guards': py + [r'tools\tests\check_test_guards.py'] + (['--since', since] if since else []),
        'suites': py + [r'tools\tests\run_suites.py', '--suites', suite_list],
        'python': py + [r'tools\tests\run_py_tests.py'] + (['--list', py_list] if py_list else [])
                  + (['--since', since] if since else []),
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
    """Kills each running phase with its descendants (compilers, test executables, shards)."""
    for p in phases:
        if p.proc is None or p.proc.poll() is not None:
            continue
        try:
            subprocess.call(['taskkill', '/T', '/F', '/PID', str(p.proc.pid)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL)
        except OSError:
            pass
        try:
            if p.proc.poll() is None:
                p.proc.kill()
            p.proc.wait(timeout=30)
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


def setup():
    """(suite list path, whether the private list was merged in), or None after a FAILED line."""
    try:
        os.makedirs(LOG_DIR, exist_ok=True)
    except OSError as exc:
        say('build_tests: could not create %s: %s' % (LOG_DIR, exc))
        say('build_tests: FAILED: setup')
        return None
    for name in PHASES:
        try:
            os.remove(log_path(name))
        except OSError:
            pass  # start() truncates it, or fails the phase when it cannot
    try:
        return merge_suites()
    except OSError as exc:
        say('build_tests: could not merge the private suite list: %s' % exc)
        say('build_tests: FAILED: merge')
        return None


def run_phases(wanted, cmds):
    """Runs the wanted phases; returns them in the printed order."""
    by_name = dict((name, Phase(name, cmds[name])) for name in PHASES if name in wanted)
    phases = [by_name[name] for name in PHASES if name in by_name]
    env = slots.child_env(leaf=False)
    env['PYTHONIOENCODING'] = 'utf-8'
    env.pop('PY_TESTS_SINCE', None)
    try:
        if slots.cpu_held():
            for p in phases:
                start(p, env)
                wait_all([p])
        else:
            for name in START_ORDER:
                if name in by_name:
                    start(by_name[name], env)
            wait_all(phases)
    except BaseException:
        stop_all(phases)
        raise
    return phases


def run(since, selection_line, forced, no_close):
    say(selection_line)
    lists = setup()
    if lists is None:
        return 1
    suite_list, merged = lists
    phases = run_phases(PHASES, commands(suite_list, since, forced, no_close))
    return report(phases, merged, 'build_tests: %s' % ', '.join('%s %.1f s' % (p.name, p.seconds())
                                                                for p in phases))


def read_repo_text(rel):
    with open(os.path.join(REPO, rel), 'rb') as f:
        return f.read().decode('utf-8')


def write_repo_lines(rel, lines):
    with open(os.path.join(REPO, rel), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def run_only(names, forced, no_close):
    """The named suites and Python modules alone: no guards, no selection, no heavy slot. The
    names are resolved, and refused, before any log is deleted or list written."""
    suites, modules = [], []
    for rel in (SUITES, PRIVATE_SUITES):
        if rel == SUITES or os.path.exists(os.path.join(REPO, rel)):
            suites += test_gate_select.suite_rows(read_repo_text(rel))
    for rel in PY_LISTS:
        if os.path.exists(os.path.join(REPO, rel)):
            modules += test_gate_select.py_rows(read_repo_text(rel))
    s_lines, p_lines = test_gate_select.resolve_only(names, suites, modules)
    if setup() is None:
        return 1
    wanted = []
    if s_lines:
        write_repo_lines(ONLY_SUITES, s_lines)
        wanted.append('suites')
    if p_lines:
        write_repo_lines(ONLY_PY, p_lines)
        wanted.append('python')
    phases = run_phases(wanted, commands(ONLY_SUITES, None, forced, no_close, py_list=ONLY_PY))
    return report(phases, False, test_gate_select.only_line(names, [(p.name, p.seconds()) for p in phases]))


def report(phases, merged, last_line):
    for p in phases:
        data = read_log(p)
        if data is None:
            say('build_tests: %s: no log (%s)' % (p.name, p.log))
        else:
            write_raw(data)
        if p.name == 'guards' and merged:
            say(MERGE_LINE)
    say(last_line)
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
    ap.add_argument('--since', metavar='REV', help='select the Python tests changed since REV')
    ap.add_argument('--all', action='store_true', help='run every Python test module')
    ap.add_argument('--only', metavar='NAME[,NAME]',
                    help='run only these suites and Python test modules (no guards, no heavy slot)')
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    try:
        forced = test_phases('TEST_GATE_FORCE_FAIL')
        no_close = test_phases('TEST_GATE_NO_CLOSE')
        if args.only is not None:
            return run_only(test_gate_select.only_names(args.only, args.all, args.since), forced, no_close)
        since, line = test_gate_select.python_selection(args.since, args.all,
                                                        os.environ.get('PY_TESTS_SINCE'), git_resolve)
        with slots.heavy('build_tests'):
            return run(since, line, forced, no_close)
    except slots.SlotTimeout as exc:
        say('build_tests: FAILED: no heavy slot (%s)' % exc)
        return 1
    except (ValueError, OSError) as exc:
        say('build_tests: FAILED: %s' % exc)
        return 1
    except KeyboardInterrupt:
        say('build_tests: FAILED: interrupted')
        return 1


if __name__ == '__main__':
    sys.exit(main())

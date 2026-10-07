"""The test gate's choices, as pure functions (Python 3, standard library only): which revision
the Python test selection runs from, and which suites and Python modules an --only run names.

Nothing here starts a process or reads git; test_gate.py passes in what it read, and
test_test_gate_select.py checks every rule.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_py_tests import ROW_RE  # noqa: E402

PREFIX = 'build_tests: python selection'


class Refusal(ValueError):
    """A combination of arguments the gate will not run; the message names why."""


def python_selection(since_arg, all_flag, env_since, resolve):
    """(rev or None, the line to print). Precedence: --since, then PY_TESTS_SINCE, then the
    merge-base of HEAD with main. --all turns selection off and is refused with --since.
    resolve() returns (merge-base, HEAD) as full shas, or raises OSError or RuntimeError with
    the reason git could not answer; no default is taken when they are equal."""
    if all_flag and since_arg:
        raise Refusal('--all and --since together (--all runs every Python module)')
    if all_flag:
        return None, '%s off (--all), running every module' % PREFIX
    if since_arg:
        return since_arg, '%s since %s (--since)' % (PREFIX, since_arg)
    env = (env_since or '').strip()
    if env:
        return env, '%s since %s (PY_TESTS_SINCE)' % (PREFIX, env)
    try:
        base, head = resolve()
    except (OSError, RuntimeError) as exc:
        return None, '%s off (%s), running every module' % (PREFIX, exc)
    if not base or not head:
        return None, '%s off (no merge-base with main), running every module' % PREFIX
    if base == head:
        return None, '%s off (HEAD is its merge-base with main), running every module' % PREFIX
    return base, '%s since %s (merge-base with main; --all runs every module)' % (PREFIX, base)


# ---- the guards' runner controls -----------------------------------------------------------

# A change to any of these since the selection rev runs the guards' runner controls.
RUNNER_CONTROL_FILES = ('tools/tests/run_suites.py', 'tools/tests/run_py_tests.py', 'tools/tests/py_shard.py',
                        'tools/tests/check.h', 'tools/tests/check_test_guards.py', 'tools/tests/test_gate.py',
                        'tools/tests/test_gate_select.py', 'tools/build/slots.py')


def runner_controls_skip(since, changed):
    """The skip line when the runner controls may be skipped, else None (they run). changed is
    the paths changed since the rev (forward slashes), or None when git could not say."""
    if not since or changed is None:
        return None
    want = set(p.lower() for p in RUNNER_CONTROL_FILES)
    if any(p.replace('\\', '/').lower() in want for p in changed):
        return None
    return 'check_test_guards: runner controls skipped (no runner change since %s)' % since


# ---- --only ------------------------------------------------------------------------------

def suite_rows(text):
    """[(name, line)] of a suite list's text, the line as written."""
    out = []
    for raw in text.replace('\r\n', '\n').split('\n'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        out.append((line.split('|', 1)[0].strip(), line))
    return out


def py_rows(text):
    """[(stem, line)] of a Python test list's text, the line as written."""
    out = []
    for raw in text.replace('\r\n', '\n').split('\n'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        m = ROW_RE.match(line)
        if m:
            rel = m.group(2).strip().replace('\\', '/')
            out.append((os.path.splitext(rel.rsplit('/', 1)[-1])[0], line))
    return out


def only_names(raw, all_flag, since_arg):
    """The names an --only value lists, in order, each once; refuses an empty list and the
    selection arguments --only cannot take."""
    if all_flag:
        raise Refusal('--only and --all together (--only runs exactly the names given)')
    if since_arg:
        raise Refusal('--only and --since together (--only runs exactly the names given)')
    names = []
    for part in (raw or '').split(','):
        name = part.strip()
        if name and name not in names:
            names.append(name)
    if not names:
        raise Refusal('--only names nothing')
    return names


def resolve_only(names, suites, modules):
    """(suite lines, Python lines) for the names: each a suite row of suites [(name, line)] or
    the stem of exactly one row of modules [(stem, line)]. An unknown name, a stem that two rows
    share, or a name that is both a suite and a module stem is refused."""
    suite_by = dict(suites)
    by_stem = {}
    for stem, line in modules:
        by_stem.setdefault(stem, []).append(line)
    s_lines, p_lines, unknown, twice, both = [], [], [], [], []
    for name in names:
        if name in suite_by and name in by_stem:
            both.append(name)
        elif name in suite_by:
            s_lines.append(suite_by[name])
        elif len(by_stem.get(name, [])) == 1:
            p_lines.append(by_stem[name][0])
        elif name in by_stem:
            twice.append(name)
        else:
            unknown.append(name)
    if unknown:
        raise Refusal('--only: %s is neither a suite nor a Python test module' % ', '.join(unknown))
    if twice:
        raise Refusal('--only: %s names more than one Python test module' % ', '.join(twice))
    if both:
        raise Refusal('--only: %s is both a suite and a Python test module' % ', '.join(both))
    return s_lines, p_lines


def only_line(names, seconds):
    """The --only run's last line; seconds is [(phase, s)] for the phases that ran."""
    return 'build_tests: only %s: %s' % (', '.join(names),
                                         ', '.join('%s %.1f s' % (p, s) for p, s in seconds))

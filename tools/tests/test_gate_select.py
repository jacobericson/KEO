"""The test gate's choices, as pure functions (Python 3, standard library only): which revision
the Python test selection runs from.

Nothing here starts a process or reads git; test_gate.py passes in what it read, and
test_test_gate_select.py checks every rule.
"""

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


"""Tests of test_gate_select.py (the Python test selection's source and its printed line, the
runner-control skip, --only's names) and of test_gate.main's wiring of them."""
import contextlib
import io
import os
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_gate_select as s  # noqa: E402

BASE = 'a' * 40
HEAD = 'b' * 40


def resolver(base=BASE, head=HEAD, error=None):
    calls = []

    def resolve():
        calls.append(1)
        if error is not None:
            raise error
        return base, head
    resolve.calls = calls
    return resolve


class TestPythonSelection(unittest.TestCase):
    def test_default_is_the_merge_base_with_main(self):
        rev, line = s.python_selection(None, False, None, resolver())
        self.assertEqual(rev, BASE)
        self.assertEqual(line, 'build_tests: python selection since %s '
                               '(merge-base with main; --all runs every module)' % BASE)

    def test_since_beats_the_environment_and_the_default(self):
        r = resolver()
        rev, line = s.python_selection('abc', False, 'def', r)
        self.assertEqual((rev, line), ('abc', 'build_tests: python selection since abc (--since)'))
        self.assertEqual(r.calls, [])

    def test_the_environment_beats_the_default(self):
        r = resolver()
        rev, line = s.python_selection(None, False, ' def ', r)
        self.assertEqual((rev, line), ('def', 'build_tests: python selection since def (PY_TESTS_SINCE)'))
        self.assertEqual(r.calls, [])

    def test_a_blank_environment_value_is_no_value(self):
        rev, _ = s.python_selection(None, False, '  ', resolver())
        self.assertEqual(rev, BASE)

    def test_all_turns_selection_off_even_with_the_environment(self):
        r = resolver()
        rev, line = s.python_selection(None, True, 'def', r)
        self.assertIsNone(rev)
        self.assertEqual(line, 'build_tests: python selection off (--all), running every module')
        self.assertEqual(r.calls, [])

    def test_all_with_since_is_refused(self):
        with self.assertRaises(s.Refusal) as cm:
            s.python_selection('abc', True, None, resolver())
        self.assertIn('--all and --since', str(cm.exception))
        self.assertIsInstance(cm.exception, ValueError)

    def test_no_default_on_main_itself(self):
        rev, line = s.python_selection(None, False, None, resolver(base=HEAD))
        self.assertIsNone(rev)
        self.assertEqual(line, 'build_tests: python selection off (HEAD is its merge-base with main), '
                               'running every module')

    def test_no_default_when_git_fails_or_answers_nothing(self):
        for err in (RuntimeError('main does not resolve'), OSError('git did not start')):
            rev, line = s.python_selection(None, False, None, resolver(error=err))
            self.assertIsNone(rev)
            self.assertEqual(line, 'build_tests: python selection off (%s), running every module' % err)
        rev, line = s.python_selection(None, False, None, resolver(base=''))
        self.assertIsNone(rev)
        self.assertIn('off (no merge-base with main)', line)

    def test_no_line_reads_as_the_runners_own_selection_line(self):
        lines = [s.python_selection(*a)[1] for a in (
            (None, False, None, resolver()), ('x', False, None, resolver()), (None, False, 'y', resolver()),
            (None, True, None, resolver()), (None, False, None, resolver(base=HEAD)))]
        for line in lines:
            self.assertTrue(line.startswith('build_tests: python selection '), line)
            self.assertFalse(line.startswith('python test selection'), line)


class TestRunnerControls(unittest.TestCase):
    def test_skipped_only_with_a_rev_and_no_runner_change(self):
        self.assertEqual(s.runner_controls_skip('abc', ['src/x.cpp', 'tools/tests/py_tests.txt']),
                         'check_test_guards: runner controls skipped (no runner change since abc)')
        self.assertEqual(s.runner_controls_skip('abc', []),
                         'check_test_guards: runner controls skipped (no runner change since abc)')

    def test_every_runner_file_runs_them(self):
        for path in s.RUNNER_CONTROL_FILES:
            self.assertIsNone(s.runner_controls_skip('abc', ['src/x.cpp', path]), path)
            self.assertIsNone(s.runner_controls_skip('abc', [path.upper().replace('/', '\\')]), path)
        self.assertEqual(len(s.RUNNER_CONTROL_FILES), 8)

    def test_no_rev_or_an_unknown_diff_runs_them(self):
        for since in (None, ''):
            self.assertIsNone(s.runner_controls_skip(since, ['src/x.cpp']))
        self.assertIsNone(s.runner_controls_skip('abc', None))


REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir))
LOCAL_DIRS = ('tools/tests', 'tools/build')
# A Python import (a plain statement, every name of `import a, b`, or one inside a scratch module's
# source string), or a .py or .h file the code names (a script it runs, a header its scratch suites
# include). Batch files are left out: the runners' docstrings name build_tests.bat, which calls
# them, and no control runs one.
REFERENCE_RE = re.compile(r'(?:\bimport\s+|\bfrom\s+)(\w+(?:\s*,\s*\w+)*)|\b(\w+\.(?:py|h))\b')


def listed_test_modules():
    """The stems of the listed unittest modules: named in docstrings, never run by the controls
    (which run scratch modules of their own)."""
    stems = set()
    for name in ('py_tests.txt', 'py_tests_private.txt'):
        path = os.path.join(REPO, 'tools', 'tests', name)
        if os.path.isfile(path):
            with open(path, 'r', encoding='utf-8') as f:
                stems.update(stem for stem, _ in s.py_rows(f.read()))
    return stems


def references(rel, skip):
    """The local files rel imports or names, repo-relative with forward slashes, less the
    unittest modules (stems in skip)."""
    with open(os.path.join(REPO, rel), 'r', encoding='utf-8') as f:
        text = f.read()
    out = set()
    for modules, name in REFERENCE_RE.findall(text):
        names = [name] if name else [m.strip() + '.py' for m in modules.split(',')]
        for name in names:
            if os.path.splitext(name)[0] in skip:
                continue
            for folder in LOCAL_DIRS:
                if os.path.isfile(os.path.join(REPO, folder, name)):
                    out.add('%s/%s' % (folder, name))
    return out


def closure(start):
    skip = listed_test_modules()
    seen, todo = set(), list(start)
    while todo:
        rel = todo.pop()
        if rel not in seen:
            seen.add(rel)
            todo.extend(references(rel, skip))
    return seen


class TestRunnerClosure(unittest.TestCase):
    def test_the_runner_list_holds_what_the_controls_import_or_run(self):
        found = closure(['tools/tests/check_test_guards.py'])
        listed = set(p.lower() for p in s.RUNNER_CONTROL_FILES)
        missing = sorted(p for p in found if p.lower() not in listed)
        self.assertEqual(missing, [], 'the runner controls import or run %s, which RUNNER_CONTROL_FILES does not '
                                      'list: a change to it would skip the controls' % ', '.join(missing))
        for must in ('tools/tests/run_suites.py', 'tools/tests/py_shard.py', 'tools/tests/check.h',
                     'tools/build/slots.py'):
            self.assertIn(must, found)


SUITES = ('# comment\r\n'
          'alpha_units | tools\\tests\\alpha_units.cpp | src\\a.cpp |\r\n'
          '\r\n'
          'beta_units | tools\\tests\\beta_units.cpp |  | /DX\r\n')
PY_PUBLIC = ('# excluded: tools/tests/test_gate.py - not a module\n'
             '95 tools\\build\\test_slots.py\n'
             '98 tools\\tests\\test_test_gate_select.py when=tools/tests/test_gate*.py\n')
PY_PRIVATE = ('130 tools\\release\\test_keo_replay.py shards=5 when=tools/release/**\n'
              '140 tools\\other\\test_slots.py\n'
              'not a row\n')


class TestOnly(unittest.TestCase):
    def setUp(self):
        self.suites = s.suite_rows(SUITES)
        self.modules = s.py_rows(PY_PUBLIC) + s.py_rows(PY_PRIVATE)

    def test_rows_are_read_with_their_lines_verbatim(self):
        self.assertEqual(self.suites, [
            ('alpha_units', 'alpha_units | tools\\tests\\alpha_units.cpp | src\\a.cpp |'),
            ('beta_units', 'beta_units | tools\\tests\\beta_units.cpp |  | /DX')])
        self.assertEqual([m[0] for m in self.modules],
                         ['test_slots', 'test_test_gate_select', 'test_keo_replay', 'test_slots'])
        self.assertEqual(self.modules[2][1], '130 tools\\release\\test_keo_replay.py shards=5 when=tools/release/**')

    def test_suites_and_unique_module_stems_and_no_name_in_both(self):
        s_lines, p_lines = s.resolve_only(['beta_units', 'test_keo_replay', 'test_test_gate_select'],
                                          self.suites, self.modules)
        self.assertEqual(s_lines, ['beta_units | tools\\tests\\beta_units.cpp |  | /DX'])
        self.assertEqual(p_lines, ['130 tools\\release\\test_keo_replay.py shards=5 when=tools/release/**',
                                   '98 tools\\tests\\test_test_gate_select.py when=tools/tests/test_gate*.py'])
        with self.assertRaises(s.Refusal) as cm:
            s.resolve_only(['alpha_units'], self.suites, [('alpha_units', '1 x\\alpha_units.py')])
        self.assertIn('alpha_units is both a suite and a Python test module', str(cm.exception))

    def test_unknown_and_shared_names_are_refused(self):
        with self.assertRaises(s.Refusal) as cm:
            s.resolve_only(['alpha_units', 'no_such_suite', 'nope'], self.suites, self.modules)
        self.assertIn('no_such_suite, nope is neither a suite nor a Python test module', str(cm.exception))
        with self.assertRaises(s.Refusal) as cm:
            s.resolve_only(['test_slots'], self.suites, self.modules)
        self.assertIn('test_slots names more than one Python test module', str(cm.exception))
        with self.assertRaises(s.Refusal):
            s.resolve_only(['alpha_units.cpp'], self.suites, self.modules)

    def test_names_are_split_trimmed_and_deduplicated(self):
        self.assertEqual(s.only_names(' a , b,a,, ', False, None), ['a', 'b'])

    def test_empty_lists_and_selection_arguments_are_refused(self):
        for raw in ('', ' ', ',', ' , ,'):
            with self.assertRaises(s.Refusal) as cm:
                s.only_names(raw, False, None)
            self.assertIn('names nothing', str(cm.exception))
        with self.assertRaises(s.Refusal) as cm:
            s.only_names('a', True, None)
        self.assertIn('--only and --all', str(cm.exception))
        with self.assertRaises(s.Refusal) as cm:
            s.only_names('a', False, 'HEAD~1')
        self.assertIn('--only and --since', str(cm.exception))

    def test_the_last_line_names_only_the_phases_that_ran(self):
        self.assertEqual(s.only_line(['a', 'test_b'], [('suites', 1.25), ('python', 3.0)]),
                         'build_tests: only a, test_b: suites 1.2 s, python 3.0 s')
        self.assertEqual(s.only_line(['test_b'], [('python', 0.5)]), 'build_tests: only test_b: python 0.5 s')
        self.assertFalse(s.only_line(['a'], [('suites', 1.0)]).startswith('build_tests: guards'))


class TestGateWiring(unittest.TestCase):
    """test_gate.main's use of these choices, every process and file write mocked out."""

    def run_main(self, argv, heavy):
        import test_gate
        seen = {}

        def run_phases(wanted, cmds):
            seen['wanted'], seen['cmds'] = list(wanted), cmds
            return []
        out = io.TextIOWrapper(io.BytesIO(), encoding='utf-8')
        env = dict((k, v) for k, v in os.environ.items() if k != 'PY_TESTS_SINCE')
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(test_gate.slots, 'heavy', heavy), \
                mock.patch.object(test_gate, 'setup', return_value=(test_gate.SUITES, False)), \
                mock.patch.object(test_gate, 'write_repo_lines'), \
                mock.patch.object(test_gate, 'git_resolve', return_value=(BASE, HEAD)), \
                mock.patch.object(test_gate, 'run_phases', side_effect=run_phases), \
                mock.patch.object(test_gate, 'report', return_value=0), \
                contextlib.redirect_stdout(out):
            rc = test_gate.main(argv)
        out.flush()
        return rc, seen, out.buffer.getvalue().decode('utf-8')

    def test_only_takes_no_heavy_slot_and_runs_no_guards(self):
        import test_gate
        with open(os.path.join(test_gate.REPO, test_gate.SUITES), 'rb') as f:
            name = s.suite_rows(f.read().decode('utf-8'))[0][0]

        def heavy(label=None):
            raise AssertionError('--only took the heavy slot')
        rc, seen, _ = self.run_main(['--only', name], heavy)
        self.assertEqual(rc, 0)
        self.assertEqual(seen['wanted'], ['suites'])

    def test_a_refused_name_deletes_and_writes_nothing(self):
        import test_gate
        out = io.TextIOWrapper(io.BytesIO(), encoding='utf-8')
        with mock.patch.object(test_gate, 'setup') as setup, \
                mock.patch.object(test_gate, 'write_repo_lines') as write, \
                mock.patch.object(test_gate, 'run_phases') as run, contextlib.redirect_stdout(out):
            rc = test_gate.main(['--only', 'no_such_suite'])
        out.flush()
        self.assertEqual(rc, 1)
        self.assertIn('build_tests: FAILED: --only: no_such_suite is neither', out.buffer.getvalue().decode('utf-8'))
        for m in (setup, write, run):
            m.assert_not_called()

    def test_the_default_selection_reaches_the_python_runner_and_the_guards(self):
        rc, seen, text = self.run_main([], lambda label=None: contextlib.nullcontext())
        self.assertEqual(rc, 0)
        self.assertEqual(list(seen['wanted']), ['guards', 'suites', 'python'])
        self.assertEqual(seen['cmds']['python'][-2:], ['--since', BASE])
        self.assertEqual(seen['cmds']['guards'][-2:], ['--since', BASE])
        self.assertIn('build_tests: python selection since %s (merge-base with main' % BASE, text)
        rc, seen, _ = self.run_main(['--all'], lambda label=None: contextlib.nullcontext())
        self.assertNotIn('--since', seen['cmds']['python'] + seen['cmds']['guards'])


if __name__ == '__main__':
    unittest.main()

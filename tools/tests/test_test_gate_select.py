"""Tests of test_gate_select.py: the Python test selection's source and its printed line."""
import os
import sys
import unittest

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

    def test_suites_first_then_unique_module_stems(self):
        s_lines, p_lines = s.resolve_only(['beta_units', 'test_keo_replay', 'test_test_gate_select'],
                                          self.suites, self.modules)
        self.assertEqual(s_lines, ['beta_units | tools\\tests\\beta_units.cpp |  | /DX'])
        self.assertEqual(p_lines, ['130 tools\\release\\test_keo_replay.py shards=5 when=tools/release/**',
                                   '98 tools\\tests\\test_test_gate_select.py when=tools/tests/test_gate*.py'])
        suite_named = s.resolve_only(['alpha_units'], self.suites, [('alpha_units', '1 x\\alpha_units.py')])
        self.assertEqual(suite_named, (['alpha_units | tools\\tests\\alpha_units.cpp | src\\a.cpp |'], []))

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


if __name__ == '__main__':
    unittest.main()

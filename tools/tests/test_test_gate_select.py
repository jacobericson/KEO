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


if __name__ == '__main__':
    unittest.main()

"""Host tests for check_headers_pin.py (Python 3, standard library only)."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_headers_pin as chp
import resources_root as rr
from test_resources_root import fenced_scratch


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def _make_tree(root):
    """Include/B.h = b'b', Include/a/x.h = b'x'."""
    _write(os.path.join(root, 'Include', 'B.h'), b'b')
    _write(os.path.join(root, 'Include', 'a', 'x.h'), b'x')


class TestIncludeSha256(unittest.TestCase):
    def test_known_answer(self):
        with tempfile.TemporaryDirectory() as tmp:
            _make_tree(tmp)
            expected = hashlib.sha256(
                b'Include/a/x.h\0x\0' + b'Include/B.h\0b\0'
            ).hexdigest()
            digest, count = chp.include_sha256(tmp)
            self.assertEqual(digest, expected)
            self.assertEqual(count, 2)

    def test_one_byte_changes_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            _make_tree(tmp)
            base_digest, _ = chp.include_sha256(tmp)
            _write(os.path.join(tmp, 'Include', 'B.h'), b'c')
            changed_digest, _ = chp.include_sha256(tmp)
            self.assertNotEqual(base_digest, changed_digest)

    def test_rename_changes_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            _make_tree(tmp)
            base_digest, _ = chp.include_sha256(tmp)
        with tempfile.TemporaryDirectory() as tmp:
            _write(os.path.join(tmp, 'Include', 'B2.h'), b'b')
            _write(os.path.join(tmp, 'Include', 'a', 'x.h'), b'x')
            renamed_digest, _ = chp.include_sha256(tmp)
        self.assertNotEqual(base_digest, renamed_digest)


class TestMain(unittest.TestCase):
    def test_refused_names_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            _make_tree(tmp)
            baseline = os.path.join(tmp, 'dependency-baseline.json')
            with open(baseline, 'w', encoding='utf-8') as f:
                json.dump({
                    'clang_headers': {
                        'commit': 'ca2883bba9152c4a7484781aa9bb31db68da0a7e',
                        'include_sha256': 'f' * 64,
                        'path': tmp,
                    }
                }, f)
            old_baseline = chp.BASELINE
            chp.BASELINE = baseline
            try:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = chp.main(['--headers', tmp])
                self.assertEqual(rc, 1)
                self.assertIn('expected ca2883b', out.getvalue())
            finally:
                chp.BASELINE = old_baseline

    def test_print_path_is_absolute(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline = os.path.join(tmp, 'dependency-baseline.json')
            with open(baseline, 'w', encoding='utf-8') as f:
                json.dump({
                    'clang_headers': {
                        'commit': 'ca2883bba9152c4a7484781aa9bb31db68da0a7e',
                        'include_sha256': 'f' * 64,
                        'path': '..\\resources\\KenshiLib-pr-headers',
                    }
                }, f)
            old_baseline = chp.BASELINE
            chp.BASELINE = baseline
            try:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = chp.main(['--print-path'])
                self.assertEqual(rc, 0)
                printed = out.getvalue().strip()
                self.assertTrue(os.path.isabs(printed))
            finally:
                chp.BASELINE = old_baseline


class TestPinFromWorktree(unittest.TestCase):
    """clang_headers.path resolves against the checkout beside resources, read from a worktree
    nested below it (the fixture is test_resources_root's fenced scratch tree)."""

    def test_pin_path_resolves_from_a_worktree(self):
        root, tmp = fenced_scratch('test_check_headers_pin')
        try:
            _write(os.path.join(tmp, 'resources', rr.MARKER), b'lib')
            worktree = os.path.join(tmp, 'Main', '.worktrees', 'wt')
            os.makedirs(worktree)
            baseline = os.path.join(tmp, 'dependency-baseline.json')
            with open(baseline, 'w', encoding='utf-8') as f:
                json.dump({'clang_headers': {'path': '..\\resources\\KenshiLib-pr-headers'}}, f)
            old_repo = chp.REPO
            chp.REPO = worktree
            try:
                pin = chp.load_pin(baseline)
            finally:
                chp.REPO = old_repo
            self.assertEqual(pin['abs_path'], os.path.join(tmp, 'resources', 'KenshiLib-pr-headers'))
        finally:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()

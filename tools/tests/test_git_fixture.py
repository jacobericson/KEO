"""Tests for git_fixture.rmtree: a test repository with read-only object files, or one with a file
held open for a moment, is removed.

    python tools\\tests\\test_git_fixture.py
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import git_fixture  # noqa: E402

CONFIG = (('user.email', 't@example.com'), ('user.name', 't'), ('core.autocrlf', 'false'))


def committed_repo(parent):
    """A repository under parent with one commit; returns (root, its object files)."""
    root = os.path.join(parent, 'repo')
    os.makedirs(root)
    git_fixture.init(root, CONFIG, branch='main')
    with open(os.path.join(root, 'a.txt'), 'w') as f:
        f.write('a\n')
    for args in (('add', '-A'), ('commit', '-q', '-m', 'c')):
        subprocess.check_call(['git', '-C', root] + list(args), stdout=subprocess.DEVNULL)
    objects = []
    for dirpath, _dirs, names in os.walk(os.path.join(root, '.git', 'objects')):
        objects.extend(os.path.join(dirpath, n) for n in names)
    return root, objects


class RmtreeTests(unittest.TestCase):
    def setUp(self):
        self.parent = tempfile.mkdtemp(prefix='git-fixture-test-')
        self.addCleanup(git_fixture.rmtree, self.parent)

    def test_read_only_object_files_are_removed(self):
        root, objects = committed_repo(self.parent)
        self.assertTrue(objects)
        # Git leaves its objects read-only; set it on every one so the case holds on any platform.
        for path in objects:
            os.chmod(path, stat.S_IREAD)
        git_fixture.rmtree(self.parent)
        self.assertFalse(os.path.exists(self.parent))

    @unittest.skipUnless(os.name == 'nt', 'only Windows refuses to delete a read-only file')
    def test_ignore_errors_rmtree_leaves_the_repository(self):
        # The control: the removal the helper replaces leaves the tree behind without a word.
        root, objects = committed_repo(self.parent)
        os.chmod(objects[0], stat.S_IREAD)
        shutil.rmtree(root, ignore_errors=True)
        self.assertTrue(os.path.exists(objects[0]))
        git_fixture.rmtree(root)
        self.assertFalse(os.path.exists(root))

    @unittest.skipUnless(os.name == 'nt', 'only Windows refuses to delete a file another handle holds')
    def test_a_briefly_held_file_is_removed_once_released(self):
        root, objects = committed_repo(self.parent)
        os.chmod(objects[0], stat.S_IREAD)
        # Python's open() shares read and write but not delete, as a scanner's handle can.
        held = open(objects[0], 'rb')
        try:
            self.assertRaises(PermissionError, os.remove, objects[0])
            release = threading.Timer(0.15, held.close)
            release.start()
            try:
                git_fixture.rmtree(root)
            finally:
                release.join()
        finally:
            held.close()
        self.assertFalse(os.path.exists(root))

    def test_a_directory_not_yet_empty_is_retried(self):
        root, _objects = committed_repo(self.parent)
        real_rmdir = os.rmdir
        refused = []

        def rmdir(path, *args, **kwargs):
            if os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(root)) and not refused:
                refused.append(path)
                raise OSError(None, 'The directory is not empty', None, 145)
            return real_rmdir(path, *args, **kwargs)
        with mock.patch('os.rmdir', side_effect=rmdir):
            git_fixture.rmtree(root)
        self.assertEqual(len(refused), 1)
        self.assertFalse(os.path.exists(root))

    def test_another_os_error_is_not_retried(self):
        root, _objects = committed_repo(self.parent)
        real_rmdir = os.rmdir
        calls = []

        def rmdir(path, *args, **kwargs):
            if os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(root)):
                calls.append(path)
                raise OSError(None, 'An I/O device error', None, 1117)
            return real_rmdir(path, *args, **kwargs)
        with mock.patch('os.rmdir', side_effect=rmdir):
            self.assertRaises(OSError, git_fixture.rmtree, root)
        self.assertEqual(len(calls), 2)

    def test_missing_path_is_not_an_error(self):
        missing = os.path.join(self.parent, 'never-made')
        git_fixture.rmtree(missing)
        self.assertFalse(os.path.exists(missing))


if __name__ == '__main__':
    unittest.main()

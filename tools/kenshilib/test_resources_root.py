"""Host tests for resources_root.py and resources_root.bat (Python 3, standard library only).

Every fixture is built under build\\ in this checkout, rr.LEVELS folders below its own scratch
root. A walk of rr.LEVELS parents from any folder inside the fixture therefore stays inside the
scratch root and never reaches a real resources folder above the checkout.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import resources_root as rr

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
RESOLVER_BAT = os.path.join(HERE, 'resources_root.bat')


def fenced_scratch(name):
    """(root, tmp): a new scratch root under REPO\\build\\<name>, and the fixture folder tmp
    rr.LEVELS folders below it. The caller removes root."""
    parent = os.path.join(REPO, 'build', name)
    os.makedirs(parent, exist_ok=True)
    root = tempfile.mkdtemp(dir=parent)
    tmp = os.path.join(root, *['f%d' % i for i in range(rr.LEVELS)])
    os.makedirs(tmp)
    return root, tmp


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def _below(tmp, depth):
    """A new folder depth levels below tmp."""
    path = os.path.join(tmp, *['d%d' % i for i in range(depth)])
    os.makedirs(path)
    return path


def _norm(path):
    return os.path.normcase(os.path.normpath(path))


class ScratchCase(unittest.TestCase):
    def setUp(self):
        self.root, self.tmp = fenced_scratch('test_resources_root')

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def marked(self):
        """tmp\\resources holds the KenshiLib marker."""
        _write(os.path.join(self.tmp, 'resources', rr.MARKER), b'lib')

    def main_and_worktree(self):
        """tmp\\Main is a checkout beside tmp\\resources; tmp\\Main\\.worktrees\\wt a worktree
        nested two folders deeper."""
        main = os.path.join(self.tmp, 'Main')
        worktree = os.path.join(main, '.worktrees', 'wt')
        os.makedirs(os.path.join(worktree, 'tools', 'kenshilib'))
        return main, worktree


class TestPythonWalk(ScratchCase):
    def test_main_and_worktree_find_the_checkout_beside_resources(self):
        self.marked()
        main, worktree = self.main_and_worktree()
        self.assertEqual(rr.beside_resources(main), main)
        self.assertEqual(rr.beside_resources(worktree), main)
        self.assertEqual(rr.beside_resources(os.path.join(worktree, 'tools', 'kenshilib')), main)
        kenshilib = os.path.join(self.tmp, 'resources', 'KenshiLib')
        self.assertEqual(rr.default_kenshilib(main), kenshilib)
        self.assertEqual(rr.default_kenshilib(worktree), kenshilib)

    def test_the_sixth_parent_is_searched(self):
        self.marked()
        start = _below(self.tmp, rr.LEVELS)
        self.assertEqual(rr.beside_resources(start), os.path.join(self.tmp, 'd0'))
        self.assertEqual(rr.default_kenshilib(start),
                         os.path.join(self.tmp, 'resources', 'KenshiLib'))

    def test_the_seventh_parent_is_not_searched(self):
        self.marked()
        start = _below(self.tmp, rr.LEVELS + 1)
        self.assertIsNone(rr.beside_resources(start))
        self.assertEqual(rr.default_kenshilib(start),
                         os.path.normpath(os.path.join(start, '..', 'resources', 'KenshiLib')))

    def test_resources_without_the_library_is_not_a_match(self):
        os.makedirs(os.path.join(self.tmp, 'resources', 'KenshiLib', 'Include'))
        main, _ = self.main_and_worktree()
        self.assertIsNone(rr.beside_resources(main))


@unittest.skipUnless(os.name == 'nt', 'resources_root.bat needs cmd.exe')
class TestBatchWalk(ScratchCase):
    """A copy of resources_root.bat at <checkout>\\tools\\kenshilib, run through a driver."""

    def bat_resources(self, checkout):
        tools = os.path.join(checkout, 'tools', 'kenshilib')
        os.makedirs(tools, exist_ok=True)
        shutil.copy(RESOLVER_BAT, tools)
        driver = os.path.join(checkout, 'probe.bat')
        with open(driver, 'w') as f:
            f.write('@call "%~dp0tools\\kenshilib\\resources_root.bat"\n'
                    '@echo KENSHI_RESOURCES=%KENSHI_RESOURCES%\n')
        env = dict(os.environ)
        env['KENSHI_RESOURCES'] = 'stale'
        proc = subprocess.run(['cmd', '/d', '/c', driver], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = proc.stdout.decode('mbcs', 'replace') + proc.stderr.decode('mbcs', 'replace')
        self.assertEqual(proc.returncode, 0, out)
        lines = [l for l in out.splitlines() if l.startswith('KENSHI_RESOURCES=')]
        self.assertEqual(len(lines), 1, out)
        return lines[0][len('KENSHI_RESOURCES='):]

    def test_main_and_worktree_find_the_same_resources(self):
        self.marked()
        main, worktree = self.main_and_worktree()
        resources = os.path.join(self.tmp, 'resources')
        self.assertEqual(_norm(self.bat_resources(main)), _norm(resources))
        self.assertEqual(_norm(self.bat_resources(worktree)), _norm(resources))

    def test_the_sixth_parent_is_searched(self):
        self.marked()
        checkout = _below(self.tmp, rr.LEVELS)
        self.assertEqual(_norm(self.bat_resources(checkout)),
                         _norm(os.path.join(self.tmp, 'resources')))

    def test_the_seventh_parent_is_not_searched(self):
        self.marked()
        checkout = _below(self.tmp, rr.LEVELS + 1)
        self.assertEqual(_norm(self.bat_resources(checkout)),
                         _norm(os.path.join(checkout, '..', 'resources')))


if __name__ == '__main__':
    unittest.main()

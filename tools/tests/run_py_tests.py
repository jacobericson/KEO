"""Run the Python unit tests listed in tools\\tests\\py_tests.txt, one process each, in order.

    python tools\\tests\\run_py_tests.py

The public tree carries only some of the listed tests. Where tools\\release\\public_manifest.txt
exists (the full tree) a missing test is a failure; elsewhere it is skipped and named, so a
shrunken run is visible in the output. Exit 1 if any test failed or a required test is missing.
"""
import os
import subprocess
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PY_TESTS_TXT = os.path.join(REPO, 'tools', 'tests', 'py_tests.txt')
FULL_TREE_MARKER = os.path.join(REPO, 'tools', 'release', 'public_manifest.txt')


def read_list(path):
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8')
    return [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith('#')]


def main():
    full_tree = os.path.exists(FULL_TREE_MARKER)
    ran, skipped, failed = [], [], []
    for rel in read_list(PY_TESTS_TXT):
        name = os.path.splitext(os.path.basename(rel))[0]
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            if full_tree:
                failed.append(name + ' (missing)')
            else:
                skipped.append(name)
            continue
        if subprocess.call([sys.executable, path], cwd=REPO) != 0:
            failed.append(name)
        ran.append(name)
    if skipped:
        print('python tests not in this tree: %s' % ', '.join(skipped))
    if failed:
        print('python tests FAILED: %s' % ', '.join(failed))
        return 1
    print('python tests: %s OK' % ', '.join(ran))
    return 0


if __name__ == '__main__':
    sys.exit(main())

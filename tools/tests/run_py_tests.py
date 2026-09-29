"""Run the Python unit tests listed in tools\\tests\\py_tests.txt, one process each, in order.

    python tools\\tests\\run_py_tests.py

Each list line is "<order> <repo-relative path>". A py_tests_private.txt beside py_tests.txt, where
the tree has one, adds more lines in the same form; the two lists are merged and run in ascending
order, so a module keeps its place in the run whichever list names it. A listed module that is
missing, a malformed line, or an order or path named twice fails the run. Exit 1 if any test
failed or a list is invalid.
"""
import os
import subprocess
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
LIST_DIR = os.path.join(REPO, 'tools', 'tests')
LISTS = [os.path.join(LIST_DIR, 'py_tests.txt'), os.path.join(LIST_DIR, 'py_tests_private.txt')]


def read_list(path, errors):
    """Returns [(order, rel)] for the list at path; appends a message to errors per bad line."""
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8')
    out = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or not parts[0].isdigit():
            errors.append('%s:%d: expected "<order> <path>"' % (os.path.basename(path), lineno))
            continue
        out.append((int(parts[0]), parts[1].strip()))
    return out


def collect(errors):
    """Returns the merged [(order, rel)] of every list present, sorted by order."""
    entries = []
    for i, path in enumerate(LISTS):
        if i == 0 or os.path.exists(path):
            entries.extend(read_list(path, errors))
    seen_orders, seen_paths = set(), set()
    for order, rel in entries:
        key = os.path.normcase(os.path.normpath(rel))
        if order in seen_orders:
            errors.append('order %d named twice' % order)
        if key in seen_paths:
            errors.append('%s named twice' % rel)
        seen_orders.add(order)
        seen_paths.add(key)
    return sorted(entries)


def main():
    errors = []
    entries = collect(errors)
    if errors:
        for e in errors:
            print('python tests FAILED: bad list: %s' % e)
        return 1
    ran, failed = [], []
    for _, rel in entries:
        name = os.path.splitext(os.path.basename(rel))[0]
        path = os.path.join(REPO, rel)
        if not os.path.exists(path):
            failed.append(name + ' (missing)')
            continue
        if subprocess.call([sys.executable, path], cwd=REPO) != 0:
            failed.append(name)
        ran.append(name)
    if failed:
        print('python tests FAILED: %s' % ', '.join(failed))
        return 1
    print('python tests: %s OK' % ', '.join(ran))
    return 0


if __name__ == '__main__':
    sys.exit(main())

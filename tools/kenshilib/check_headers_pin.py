"""Check that a KenshiLib header tree is the pinned one (Python 3, standard library only).

The measure: SHA-256 over every file under <tree>/Include, in the order of their relative
paths lower-cased (forward slashes); each file contributes 'Include/<path>' in UTF-8, a NUL,
its raw bytes and a NUL. Bytes are hashed as they are on disk, so a checkout with
core.autocrlf=true does not match. This measure reproduces headers_include_sha256 in
dependency-baseline.json for the 0.5.1 tree.

Usage:
  check_headers_pin.py [--headers DIR]  hash DIR (else KENSHILIB_HEADERS, else the pinned path)
                                        and compare it with the pin
  check_headers_pin.py --print-path     print the pinned tree's absolute path
  check_headers_pin.py --hash DIR       print DIR's measure, no comparison
Exit 0 when the tree is the pinned one, 1 when it is refused, 2 when the baseline or the tree
cannot be read.
"""
import argparse
import hashlib
import json
import os
import sys

from resources_root import beside_resources

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BASELINE = os.path.join(REPO, 'tools', 'kenshilib', 'dependency-baseline.json')

def include_sha256(tree):
    """(hex digest, file count) of tree/Include by the measure above."""
    inc = os.path.join(tree, 'Include')
    if not os.path.isdir(inc):
        raise ValueError('%s has no Include folder' % tree)
    rels = []
    for folder, dirs, files in os.walk(inc):
        for name in files:
            rels.append(os.path.relpath(os.path.join(folder, name), inc).replace(os.sep, '/'))
    rels.sort(key=lambda r: (r.lower(), r))
    h = hashlib.sha256()
    for rel in rels:
        h.update(('Include/' + rel).encode('utf-8'))
        h.update(b'\0')
        with open(os.path.join(inc, *rel.split('/')), 'rb') as f:
            h.update(f.read())
        h.update(b'\0')
    return h.hexdigest(), len(rels)

def load_pin(path=None):
    """The baseline's clang_headers block, with 'abs_path' resolved against the checkout beside
    the resources folder (resources_root.beside_resources), else against the repository."""
    with open(path or BASELINE, encoding='utf-8') as f:
        pin = dict(json.load(f)['clang_headers'])
    base = beside_resources(REPO) or REPO
    pin['abs_path'] = os.path.normpath(os.path.join(base, pin['path']))
    return pin

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--headers')
    mode.add_argument('--print-path', action='store_true')
    mode.add_argument('--hash')
    args = parser.parse_args(argv)
    try:
        if args.hash:
            digest, count = include_sha256(args.hash)
            print('check_headers_pin: %s (%d files) %s' % (digest, count, args.hash))
            return 0
        pin = load_pin()
        if args.print_path:
            print(pin['abs_path'])
            return 0
        tree = args.headers or os.environ.get('KENSHILIB_HEADERS') or pin['abs_path']
        digest, count = include_sha256(tree)
    except (OSError, ValueError, KeyError) as error:
        print('check_headers_pin: ERROR: %s' % error)
        return 2
    commit7 = pin['commit'][:7]
    if digest != pin['include_sha256']:
        print('check_headers_pin: REFUSED %s: include sha256 %s != pinned %s (expected %s; a CRLF '
              'checkout or a different commit)' % (tree, digest[:12], pin['include_sha256'][:12], commit7))
        return 1
    print('check_headers_pin: OK %s (%d files, %s)' % (commit7, count, tree))
    return 0

if __name__ == '__main__':
    sys.exit(main())

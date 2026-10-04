"""Run one shard of a Python unittest module and record what it ran.

    python tools\\tests\\py_shard.py PATH --shard K/N --result RESULT.json

PATH is loaded as "python PATH" would load it for unittest.main(): its folder first on sys.path,
sys.argv = [PATH], the default warning filter unittest.main() sets, and run from the caller's
working directory. It is loaded under its file stem rather than as __main__, so test ids read
"<stem>.<Class>.<test>". The tests are flattened in loader order; the shard runs those whose
index i has i % N == K-1, writing the runner's output to stderr as unittest.main() does.

RESULT.json is deleted first and written atomically at the end:
    {"module", "shard", "shards", "total", "ids_sha256", "selected", "run",
     "failures", "errors", "skipped", "ok"}
where "selected" lists the shard's test ids and "ids_sha256" hashes every id of the module in
loader order, so shards of one module can be checked against each other.

Exit 0 only when every selected test ran and passed and at least one was selected; 1 when a test
failed or nothing was selected (the result is still written); 2 on bad arguments, an import error
or a load error (no result).

TEST_PY_SHARD_SKEW=1 makes shard 1 select i % N == 1 instead, and TEST_PY_SHARD_CRASH=1 makes
shard 1 exit 0 after its tests without writing its result. Both exist only so the runner's own
checks can be shown to catch them; nothing else sets them.
"""
import argparse
import hashlib
import json
import os
import sys
import unittest


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            for test in flatten(item):
                yield test
        else:
            yield item


def load_module(path):
    """Executes the file at path as a module named after its stem and returns it."""
    stem = os.path.splitext(os.path.basename(path))[0]
    sys.path[0] = os.path.dirname(path)
    sys.argv = [path]
    with open(path, 'rb') as f:
        source = f.read()
    code = compile(source, path, 'exec')
    import types
    mod = types.ModuleType(stem)
    mod.__file__ = path
    sys.modules[stem] = mod
    exec(code, mod.__dict__)
    return mod


def write_result(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


def parse_shard(text):
    k, sep, n = text.partition('/')
    if not sep or not k.isdigit() or not n.isdigit():
        raise ValueError
    k, n = int(k), int(n)
    if n < 1 or not 1 <= k <= n:
        raise ValueError
    return k, n


def main(argv):
    ap = argparse.ArgumentParser(description='Run one shard of a unittest module.')
    ap.add_argument('path')
    ap.add_argument('--shard', required=True, help='K/N: run shard K of N')
    ap.add_argument('--result', required=True, help='where to write the result JSON')
    args = ap.parse_args(argv)
    try:
        k, n = parse_shard(args.shard)
    except ValueError:
        sys.stderr.write('py_shard: bad --shard %r (expected K/N with 1 <= K <= N)\n' % args.shard)
        return 2
    path = os.path.abspath(args.path)
    result_path = os.path.abspath(args.result)
    if os.path.exists(result_path):
        os.remove(result_path)
    stem = os.path.splitext(os.path.basename(path))[0]

    try:
        mod = load_module(path)
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.stderr.write('py_shard: %s failed to import\n' % stem)
        return 2

    loader = unittest.TestLoader()
    tests = list(flatten(loader.loadTestsFromModule(mod)))
    failed_loads = [t.id() for t in tests if isinstance(t, unittest.loader._FailedTest)]
    if loader.errors or failed_loads:
        for err in loader.errors:
            sys.stderr.write(err + '\n')
        sys.stderr.write('py_shard: %s has load errors: %s\n' % (stem, ', '.join(failed_loads) or 'see above'))
        return 2
    ids = [t.id() for t in tests]
    if len(set(ids)) != len(ids):
        dupes = sorted(set(i for i in ids if ids.count(i) > 1))
        sys.stderr.write('py_shard: %s loads a test more than once: %s\n' % (stem, ', '.join(dupes)))
        return 2

    pick = k - 1
    if k == 1 and os.environ.get('TEST_PY_SHARD_SKEW') == '1':
        pick = 1 % n
    selected = [t for i, t in enumerate(tests) if i % n == pick]

    stream_args = {}
    if not sys.warnoptions:
        stream_args['warnings'] = 'default'
    result = unittest.TextTestRunner(**stream_args).run(unittest.TestSuite(selected)) if selected else None

    run = result.testsRun if result else 0
    data = {
        'module': path,
        'shard': k,
        'shards': n,
        'total': len(ids),
        'ids_sha256': hashlib.sha256('\n'.join(ids).encode('utf-8')).hexdigest(),
        'selected': [t.id() for t in selected],
        'run': run,
        'failures': len(result.failures) if result else 0,
        'errors': len(result.errors) if result else 0,
        'skipped': len(result.skipped) if result else 0,
        'ok': bool(result and result.wasSuccessful()),
    }
    if k == 1 and os.environ.get('TEST_PY_SHARD_CRASH') == '1':
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(0)
    write_result(result_path, data)
    if not ids:
        sys.stderr.write('py_shard: %s has no tests\n' % stem)
        return 1
    if not selected:
        sys.stderr.write('py_shard: %s shard %d/%d selected none of its %d tests\n' % (stem, k, n, len(ids)))
        return 1
    return 0 if data['ok'] and run == len(selected) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

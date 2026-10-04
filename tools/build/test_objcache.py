"""Tests of tools\\build\\objcache.py, the object cache, and of run_variants.py's use of it
(unittest, standard library only).

A fake cl (FAKE_CL below, run by this Python) stands in for the compiler: it echoes the source's
name, prints a "Note: including file:" line per include it resolves (the including file's folder,
then each /I folder), honours the CL variable, and writes an "object" that hashes the source, its
includes and the defines. Each test builds a small checkout in its own temporary folder, with its
own cache folder, and drives run_variants.build() on it. Every file's and folder's times, the
change time included, are set a minute back first, since a compile does not publish what changed
within 2 s of its start.
"""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import objcache  # noqa: E402
import objstore  # noqa: E402
import run_variants  # noqa: E402

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes
    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _k32.CreateFileW.restype = wintypes.HANDLE
    _k32.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    _k32.SetFileInformationByHandle.restype = wintypes.BOOL
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]


def set_times(path, seconds):
    """Sets path's write and change times (and the others) to `seconds` since 1970: what a
    deliberate forgery would do, and the only way to age a fixture's change time."""
    if os.name != 'nt':
        os.utime(path, (seconds, seconds))
        return
    ticks = int(seconds * 1e7) + objstore.EPOCH_DIFF_100NS
    info = objstore._BasicInfo(ticks, ticks, ticks, ticks, 0)
    handle = _k32.CreateFileW(path, 0x100, 7, None, 3, 0x02000000, None)  # FILE_WRITE_ATTRIBUTES
    if handle is None or handle == objstore._INVALID_HANDLE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not _k32.SetFileInformationByHandle(handle, 0, ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        _k32.CloseHandle(handle)

FAKE_CL = r'''
import hashlib, os, sys
args = os.environ.get('CL', '').split() + sys.argv[1:]
mode = os.environ.get('FAKE_CL_MODE', '')
dirs = [a[2:] for a in args if a.startswith('/I')]
defines = ' '.join(a for a in args if a.startswith('/D')).encode()
src = args[args.index('/c') + 1]
obj = [a[3:] for a in args if a.startswith('/Fo')][0]
print(os.path.basename(src))
seen = []
def resolve(name, here):
    for d in [here] + dirs:
        if os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name)
def scan(path):
    data = open(path, 'rb').read()
    parts = [data]
    for line in data.decode().splitlines():
        if line.startswith('#include "'):
            p = resolve(line.split('"')[1], os.path.dirname(path))
            if p is None:
                print('%s(1) : fatal error C1083: Cannot open include file' % path)
                sys.exit(2)
            seen.append(p)
            if mode != 'nonotes':
                print('Note: including file: %s' % p)
            parts.append(scan(p))
        elif line.startswith('#warn'):
            print('%s(1) : warning C4101: seeded' % path)
        elif line.startswith('#error'):
            print('%s(1) : error C1189: seeded' % path)
            sys.exit(2)
    return b''.join(parts)
body = scan(src)
if mode == 'ghost':
    print('Note: including file: %s' % os.path.join(os.path.dirname(src), 'ghost.h'))
if mode == 'abspath':
    print('%s(1) : warning C4101: absolute' % os.path.abspath(src))
if mode == 'touch' and seen:
    os.utime(seen[0])
if mode == 'editsrc':
    open(src, 'ab').write(b'// edited during the compile\n')
if mode == 'revert' and os.path.basename(src) == 'a.cpp':
    # what restoring a backup with Copy-Item, robocopy or cp -p does: old bytes, old mtime
    for p in seen:
        if os.path.basename(p) == 'a.h':
            open(p, 'w').write('// a\n')
            t = __import__('time').time() - 600
            os.utime(p, (t, t))
if mode == 'poke' and os.path.basename(src) == 'c.cpp':
    # another source's include rewritten in place, its mtime kept
    p = os.path.join(os.path.dirname(src), 'a.h')
    st = os.stat(p)
    open(p, 'w').write('// poked\n')
    os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
open(obj, 'wb').write(hashlib.sha256(body + defines).digest() + body)
'''

TREE = {
    'src/a.cpp': '#include "a.h"\n#include "common.h"\nint a;\n',
    'src/b.cpp': '#include "common.h"\n#warn\nint b;\n',
    'src/c.cpp': '#include "b.h"\n#include "lib.h"\nint c;\n',
    'src/a.h': '// a\n',
    'src/b.h': '// b\n',
    'inc/common.h': '// common\n',
}
SOURCES = ['src\\a.cpp', 'src\\b.cpp', 'src\\c.cpp']
CACHE_KEYS = ('BUILD_CACHE', 'KEO_CACHE_DIR', 'KEO_CACHE_MAX_GB', 'KEO_SLOTS', 'KEO_TIMINGS_DIR',
              'KENSHILIB', 'BOOST_ROOT', 'CL', '_CL_', 'TEST_NOOP_CL', 'TEST_OBJ_TAMPER', 'FAKE_CL_MODE',
              'KEO_CPU_HELD', 'KEO_HEAVY_HELD')


def write(path, text, mode='w'):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, mode) as f:
        f.write(text)


def age(top, seconds=60):
    """Sets every file's and folder's times under top (and top's) back by seconds."""
    t = time.time() - seconds
    for folder, dirs, files in os.walk(top, topdown=False):
        for name in files:
            set_times(os.path.join(folder, name), t)
        set_times(folder, t)


class Run(object):
    """One run_variants build of the checkout: its records, cache line and log."""

    def __init__(self, failed, objects, line, log):
        self.failed, self.objects, self.line, self.log = failed, objects, line, log

    def how(self):
        return dict((e['src'], e['how']) for e in self.objects)

    def note(self, source):
        return [e['cache'] for e in self.objects if e['src'] == source][0]


class CacheTestBase(unittest.TestCase):

    def setUp(self):
        self.saved = dict((k, os.environ.get(k)) for k in CACHE_KEYS)
        for k in CACHE_KEYS:
            os.environ.pop(k, None)
        self.cwd = os.getcwd()
        self.tmp = tempfile.mkdtemp(prefix='keo_objcache_test_')
        self.cache_dir = os.path.join(self.tmp, 'cache')
        self.kenshilib = os.path.join(self.tmp, 'kl')
        write(os.path.join(self.kenshilib, 'Include', 'lib.h'), '// lib\n')
        os.environ.update({'KEO_SLOTS': 'off', 'KEO_TIMINGS_DIR': os.path.join(self.tmp, 'timings'),
                           'KEO_CACHE_DIR': self.cache_dir, 'KENSHILIB': self.kenshilib,
                           'BOOST_ROOT': os.path.join(self.tmp, 'boost')})
        self.root = self.checkout('one')

    def tearDown(self):
        os.chdir(self.cwd)
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.tmp, ignore_errors=True)

    def checkout(self, name):
        root = os.path.join(self.tmp, name)
        for rel, text in TREE.items():
            write(os.path.join(root, rel), text)
        write(os.path.join(root, 'fake_cl.py'), FAKE_CL)
        write(os.path.join(root, 'list.txt'), '\n'.join(SOURCES) + '\n')
        age(root)
        age(self.kenshilib)
        return root

    def build(self, root=None, mp=4, **env):
        """Builds the checkout's three sources once with the given extra environment, mp processes
        at once."""
        root = root or self.root
        saved = dict((k, os.environ.get(k)) for k in env)
        os.environ.update(env)
        os.chdir(root)
        try:
            v = run_variants.Variant(0, 'prof', os.path.join('build', 'out'), os.path.join('build', 'obj'), '', '',
                                     'list.txt', True, uuid.uuid4().hex)
            v.sources = run_variants.load_sources('list.txt')
            v.args = [os.path.join(root, 'fake_cl.py'), '/nologo', '/Iinc',
                      '/I' + os.path.join(self.kenshilib, 'Include')]
            cache = objcache.ObjectCache.from_env(os.getcwd())
            v.cache_desc = cache.describe()
            os.makedirs(v.objdir, exist_ok=True)
            with contextlib.redirect_stdout(io.StringIO()):
                failed = run_variants.build([v], 1, mp, sys.executable, cache)
            cache.finish()
            with open(v.log, 'rb') as f:
                log = f.read().decode('mbcs', 'replace')
            lines = [l for l in log.splitlines() if l.startswith('object cache: ')]
            objects = []
            if os.path.exists(v.manifest):
                with open(v.manifest, encoding='utf-8') as f:
                    objects = json.load(f)['sources']
            return Run(failed, objects, lines[0] if len(lines) == 1 else lines, log)
        finally:
            os.chdir(self.cwd)
            for k, value in saved.items():
                if value is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = value

    def obj(self, name, root=None):
        with open(os.path.join(root or self.root, 'build', 'obj', name), 'rb') as f:
            return f.read()

    def entries(self):
        top = os.path.join(self.cache_dir, 'objects')
        if not os.path.isdir(top):
            return []
        return [os.path.join(top, d, e) for d in os.listdir(top) for e in os.listdir(os.path.join(top, d))]

    def edit(self, rel, text, root=None):
        write(os.path.join(root or self.root, rel), text, 'a')
        age(root or self.root)

    def line(self, h, c, r, u, mode='BUILD_CACHE=on'):
        return 'object cache: %d restored, %d compiled, %d rejected, %d uncacheable (%s)' % (h, c, r, u, mode)


class RestoreTest(CacheTestBase):

    def test_cold_then_warm_restores_every_source_with_the_same_objects_and_log(self):
        cold = self.build()
        self.assertEqual((cold.failed, cold.line), ([], self.line(0, 3, 0, 0)))
        self.assertEqual(set(cold.how().values()), {'compiled'})
        self.assertEqual([cold.note(s) for s in SOURCES], ['published'] * 3)
        objs = dict((n, self.obj(n)) for n in ('a.obj', 'b.obj', 'c.obj'))
        warm = self.build()
        self.assertEqual((warm.failed, warm.line), ([], self.line(3, 0, 0, 0)))
        self.assertEqual(set(warm.how().values()), {'restored'})
        for n, data in objs.items():
            self.assertEqual(self.obj(n), data)
        strip = lambda log: [l for l in log.splitlines() if not l.startswith('object cache: ')]
        self.assertEqual(strip(warm.log), strip(cold.log))
        self.assertEqual(warm.log.count('warning C4101'), 1)
        self.assertEqual([e['includes'] for e in warm.objects], [e['includes'] for e in cold.objects])
        self.assertEqual([e['key'] for e in warm.objects], [e['key'] for e in cold.objects])

    def test_a_header_edit_misses_exactly_its_dependents(self):
        self.build()
        self.edit('inc/common.h', '// edited\n')
        run = self.build()
        self.assertEqual(run.how(), {'src\\a.cpp': 'compiled', 'src\\b.cpp': 'compiled', 'src\\c.cpp': 'restored'})
        self.assertEqual(run.line, self.line(1, 2, 0, 0))
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_reverting_a_header_finds_the_earlier_entry(self):
        self.build()
        path = os.path.join(self.root, 'src', 'b.h')
        self.edit('src/b.h', '// changed\n')
        self.assertEqual(self.build().how()['src\\c.cpp'], 'compiled')
        write(path, TREE['src/b.h'])
        age(self.root)
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_a_new_header_of_an_included_basename_misses_its_includers(self):
        self.build()
        write(os.path.join(self.root, 'src', 'deeper', 'common.h'), '// shadow\n')
        age(self.root)
        run = self.build()
        self.assertEqual(run.how(), {'src\\a.cpp': 'compiled', 'src\\b.cpp': 'compiled', 'src\\c.cpp': 'restored'})
        write(os.path.join(self.kenshilib, 'Include', 'more', 'b.h'), '// shadow\n')
        age(self.kenshilib)
        self.assertEqual(self.build().how()['src\\c.cpp'], 'compiled')

    def test_the_cl_variable_changes_every_key(self):
        self.build()
        run = self.build(CL='/DFOO')
        self.assertEqual(run.line, self.line(0, 3, 0, 0))
        self.assertNotEqual(self.obj('a.obj'), b'')
        self.assertEqual(self.build(CL='/DFOO').line, self.line(3, 0, 0, 0))
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_a_second_checkout_restores_with_its_own_paths(self):
        self.build()
        other = self.checkout('two')
        run = self.build(root=other)
        self.assertEqual(run.line, self.line(3, 0, 0, 0))
        self.assertEqual(self.obj('a.obj', other), self.obj('a.obj'))
        root = os.path.normcase(other)
        for e in run.objects:
            inside = [p for p in e['includes'] if 'kl' not in os.path.basename(os.path.dirname(os.path.dirname(p)))]
            self.assertTrue(inside and all(p.startswith(root + os.sep) for p in inside), e['includes'])

    def test_the_candidate_window_keeps_a_header_state_found_again(self):
        self.build()
        for i in range(objcache.CANDIDATES):
            self.edit('src/b.h', '// state %d\n' % i)
            self.build()
        write(os.path.join(self.root, 'src', 'b.h'), TREE['src/b.h'])
        age(self.root)
        self.assertEqual(self.build().how()['src\\c.cpp'], 'compiled')  # the first state is the 9th newest
        self.assertEqual(self.build().how()['src\\c.cpp'], 'restored')  # its republish made it newest


class RefusalTest(CacheTestBase):

    def test_a_corrupt_entry_is_rejected_moved_aside_and_recompiled(self):
        self.build()
        good = self.obj('b.obj')
        key = [e['key'] for e in self.build().objects if e['src'] == 'src\\b.cpp'][0]
        path = os.path.join(self.cache_dir, 'objects', key[:2], key, 'obj')
        with open(path, 'r+b') as f:
            f.seek(10)
            byte = f.read(1)
            f.seek(10)
            f.write(bytes([byte[0] ^ 0xFF]))
        run = self.build()
        self.assertEqual((run.failed, run.line), ([], self.line(2, 1, 1, 0)))
        self.assertEqual(self.obj('b.obj'), good)
        bad = os.listdir(os.path.join(self.cache_dir, 'bad'))
        self.assertEqual(sorted(n.split('.')[0] for n in bad), [key] * 3)
        self.assertTrue(any(n.endswith('.why.txt') for n in bad))
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_entry_and_candidate_faults_are_each_rejected(self):
        self.build()
        keys = dict((e['src'], e['key']) for e in self.build().objects)
        entry = lambda k: os.path.join(self.cache_dir, 'objects', k[:2], k)
        os.remove(os.path.join(entry(keys['src\\a.cpp']), 'meta.json'))
        write(os.path.join(entry(keys['src\\b.cpp']), 'out.txt'), 'other output\n')
        cands = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(self.cache_dir, 'manifests'))
                 for f in fs if f.startswith(keys['src\\c.cpp'])]
        write(cands[0], '{"schema": 1}')
        run = self.build()
        self.assertEqual((run.failed, run.line), ([], self.line(0, 3, 3, 0)))
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_a_tampered_object_never_reaches_the_cache(self):
        clean = self.build(BUILD_CACHE='off')
        self.assertEqual(clean.line, self.line(0, 3, 0, 0, 'BUILD_CACHE=off'))
        good = self.obj('a.obj')
        tampered = self.build(TEST_OBJ_TAMPER='1')
        self.assertTrue(tampered.failed)
        self.assertIn('does not match its recorded sha256', tampered.log)
        warm = self.build()
        self.assertEqual(warm.line, self.line(3, 0, 0, 0))
        self.assertEqual(self.obj('a.obj'), good)

    def test_noop_compiles_take_nothing_from_the_cache(self):
        self.build()
        run = self.build(TEST_NOOP_CL='1')
        self.assertTrue(run.failed)
        self.assertEqual(run.line, self.line(0, 3, 0, 3, 'BUILD_CACHE=on, off under TEST_NOOP_CL=1'))
        self.assertEqual(run.log.count('no object was produced'), 3)


class UncacheableTest(CacheTestBase):

    def assertNothingPublished(self, run, reason):
        self.assertEqual(run.line, self.line(0, 3, 0, 3))
        for e in run.objects:
            self.assertTrue(e['cache'].startswith('uncacheable: '), e['cache'])
            self.assertIn(reason, e['cache'])
        self.assertEqual(self.entries(), [])

    def test_no_include_notes(self):
        self.assertNothingPublished(self.build(FAKE_CL_MODE='nonotes'), 'Note: including file')

    def test_an_include_missing_after_the_compile(self):
        self.assertNothingPublished(self.build(FAKE_CL_MODE='ghost'), 'ghost.h')

    def test_an_include_changed_during_the_compile(self):
        self.assertNothingPublished(self.build(FAKE_CL_MODE='touch'), 'changed during the compile')

    def test_the_source_changed_during_the_compile(self):
        self.assertNothingPublished(self.build(FAKE_CL_MODE='editsrc'), 'the source changed')

    def test_output_naming_the_checkout(self):
        self.assertNothingPublished(self.build(FAKE_CL_MODE='abspath'), 'names the checkout')

    def test_a_watched_folder_changed_during_the_compile(self):
        write(os.path.join(self.root, 'src', 'unrelated.txt'), 'new\n')
        self.assertNothingPublished(self.build(), 'a watched folder changed')

    def test_a_failed_compile(self):
        self.edit('src/b.cpp', '#error\n')
        run = self.build()
        self.assertTrue(run.failed)
        self.assertEqual(run.line, self.line(0, 3, 0, 1))
        self.assertEqual(len(self.entries()), 2)


class StaleInputTest(CacheTestBase):
    """Inputs rewritten in place with their old mtime kept (a backup restored with Copy-Item,
    robocopy or cp -p): only their change time shows it."""

    def compile_one(self, cache, source='src\\a.cpp'):
        """Compiles source in the checkout (the caller's cwd) the way run_compile does; returns
        the Compiled record publishing needs."""
        obj = os.path.join('build', 'api', os.path.basename(source)[:-4] + '.obj')
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        command = [sys.executable, os.path.join(self.root, 'fake_cl.py'), '/Iinc',
                   '/I' + os.path.join(self.kenshilib, 'Include'), '/showIncludes', '/c', source, '/Fo' + obj]
        key = cache.key(command, source)
        start = time.time()
        p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output, includes = run_variants.split_notes(p.stdout)
        captured, reason = cache.capture(key, start, includes)
        return objcache.Compiled(key, start, p.returncode, obj, run_variants.file_sha256(obj), output,
                                 includes, captured, reason)

    def test_an_include_restored_during_the_compile_is_never_published(self):
        write(os.path.join(self.root, 'src', 'a.h'), '// a EDITED\n')
        age(self.root)
        first = self.build(FAKE_CL_MODE='revert')  # a.cpp compiled from the edit, a.h put back
        self.assertTrue(first.note('src\\a.cpp').startswith('uncacheable: '), first.note('src\\a.cpp'))
        self.assertIn('a.h changed during the compile', first.note('src\\a.cpp'))
        age(self.root)  # a while later
        warm = self.build()
        self.assertEqual(warm.how()['src\\a.cpp'], 'compiled')
        restored = self.build().how()['src\\a.cpp']
        kept = self.obj('a.obj')
        self.assertEqual(restored, 'restored')
        self.build(BUILD_CACHE='off')
        self.assertEqual(self.obj('a.obj'), kept)
        self.assertNotIn(b'EDITED', kept)

    def test_an_include_restored_after_the_compile_is_never_published(self):
        os.chdir(self.root)
        try:
            cache = objcache.ObjectCache.from_env(self.root)
            c = self.compile_one(cache)
            self.assertIsNone(c.reason)
            path = os.path.join(self.root, 'src', 'a.h')
            st = os.stat(path)
            write(path, '// b\n')  # the same size, and the mtime put back
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
            (state, detail), = cache.publish_all([c])
            self.assertEqual(state, 'uncacheable')
            self.assertIn('a.h changed after the compile', detail)
            self.assertEqual(self.entries(), [])
            age(self.root)
            (state, _), = cache.publish_all([self.compile_one(cache)])  # the control: quiet, published
            self.assertEqual(state, 'published')
        finally:
            os.chdir(self.cwd)

    def test_a_restored_input_changed_during_the_build_refuses_the_link(self):
        self.build()
        self.edit('src/c.cpp', '// misses\n')
        write(os.path.join(self.tmp, 'timings', 'compile.json'), json.dumps(
            {'version': 1, 'seconds': {'src\\a.cpp': 9.0, 'src\\c.cpp': 5.0, 'src\\b.cpp': 1.0}}))
        run = self.build(mp=1, FAKE_CL_MODE='poke')  # a.cpp restored, then c.cpp's compile rewrites a.h
        self.assertTrue(run.failed)
        self.assertIn('the object cache refused the link, 1 restored object(s) whose inputs changed '
                      'during the build:', run.log)
        self.assertIn('src\\a.cpp: ', run.log)
        self.assertIn('a.h changed after its object was restored', run.log)
        self.assertEqual(run.objects, [])

    def test_write_replaces_a_stale_entry(self):
        self.build()
        key = [e['key'] for e in self.build().objects if e['src'] == 'src\\a.cpp'][0]
        folder = os.path.join(self.cache_dir, 'objects', key[:2], key)
        stale = b'a stale object that still matches its meta.json'
        with open(os.path.join(folder, 'meta.json'), 'rb') as f:
            meta = json.loads(f.read().decode('ascii'))
        meta['blobs']['obj'] = {'sha256': objstore.digest(stale), 'size': len(stale)}
        write(os.path.join(folder, 'obj'), stale.decode())
        write(os.path.join(folder, 'meta.json'), json.dumps(meta))
        self.assertEqual(self.build().how()['src\\a.cpp'], 'restored')
        self.assertEqual(self.obj('a.obj'), stale)  # self-consistent, so nothing but write repairs it
        run = self.build(BUILD_CACHE='write')
        self.assertEqual(run.note('src\\a.cpp'), 'published')
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))
        good = self.obj('a.obj')
        self.build(BUILD_CACHE='off')
        self.assertEqual(good, self.obj('a.obj'))
        with open(os.path.join(self.cache_dir, 'bad', [n for n in os.listdir(os.path.join(self.cache_dir, 'bad'))
                                                       if n.startswith(key) and n.endswith('.why.txt')][0])) as f:
            self.assertIn('BUILD_CACHE=write', f.read())

    def test_the_compiler_folder_and_include_folders_are_in_the_key(self):
        cc = os.path.join(self.tmp, 'cc')
        for name in ('cl.exe', 'c1xx.dll', 'cl.exe.config', os.path.join('1033', 'clui.dll'), 'notes.txt'):
            write(os.path.join(cc, name), name)
        os.chdir(self.root)
        try:
            def key(*tokens):
                cache = objcache.ObjectCache.from_env(os.getcwd())
                return cache.key([os.path.join(cc, 'cl.exe')] + list(tokens) + ['/c', 'src\\a.cpp'], 'src\\a.cpp').M
            first = key('/Iinc')
            write(os.path.join(cc, 'notes.txt'), 'other')
            self.assertEqual(key('/Iinc'), first)
            for name in (os.path.join('1033', 'clui.dll'), 'cl.exe.config', 'c1xx.dll'):
                write(os.path.join(cc, name), 'changed')
                changed = key('/Iinc')
                self.assertNotEqual(changed, first, name)
                first = changed
            near = key('/I..\\shared')
        finally:
            os.chdir(self.cwd)
        deep = self.checkout(os.path.join('deep', 'two'))
        os.chdir(deep)
        try:
            self.assertNotEqual(key('/I..\\shared'), near)  # the same token, another folder
            self.assertEqual(key('/Iinc'), first)            # inside the checkout: shared
        finally:
            os.chdir(self.cwd)


class ModeTest(CacheTestBase):

    def test_write_compiles_and_publishes_off_does_neither(self):
        run = self.build(BUILD_CACHE='off')
        self.assertEqual(run.line, self.line(0, 3, 0, 0, 'BUILD_CACHE=off'))
        self.assertEqual([e['cache'] for e in run.objects], ['off'] * 3)
        self.assertFalse(os.path.exists(self.cache_dir))
        run = self.build(BUILD_CACHE='write')
        self.assertEqual(run.line, self.line(0, 3, 0, 0, 'BUILD_CACHE=write'))
        self.assertEqual(len(self.entries()), 3)
        run = self.build(BUILD_CACHE='write')
        self.assertEqual([e['cache'] for e in run.objects], ['published'] * 3)  # each entry replaced
        self.assertEqual(len(self.entries()), 3)
        why = [n for n in os.listdir(os.path.join(self.cache_dir, 'bad')) if n.endswith('.why.txt')]
        self.assertEqual(len(why), 3)
        self.assertEqual(self.build(BUILD_CACHE='off').line, self.line(0, 3, 0, 0, 'BUILD_CACHE=off'))
        self.assertEqual(self.build().line, self.line(3, 0, 0, 0))

    def test_bad_settings_are_refused(self):
        for name, value in (('BUILD_CACHE', 'maybe'), ('BUILD_CACHE', 'on '), ('BUILD_CACHE', 'ON'),
                            ('KEO_CACHE_DIR', 'relative\\cache'), ('KEO_CACHE_DIR', 'C:cache'),
                            ('KEO_CACHE_DIR', '\\no\\drive'), ('KEO_CACHE_MAX_GB', 'lots'),
                            ('KEO_CACHE_MAX_GB', '0'), ('KEO_CACHE_MAX_GB', '-1'),
                            ('KEO_CACHE_MAX_GB', 'inf'), ('KEO_CACHE_MAX_GB', 'nan')):
            env = dict(os.environ)
            env[name] = value
            with self.assertRaises(ValueError, msg='%s=%r' % (name, value)):
                objcache.settings(env)
        env = dict(os.environ)
        env.pop('KEO_CACHE_DIR')
        env['LOCALAPPDATA'] = 'C:\\Local'
        self.assertEqual(objcache.settings(env), ('on', 'C:\\Local\\KEO\\objcache\\v1', 20 * objcache.GIB))

    def test_the_runner_refuses_a_bad_setting_before_building(self):
        os.chdir(self.root)
        argv = sys.argv
        sys.argv = ['run_variants.py', '--kind', 'prof', '--sources', 'list.txt', '--compile-only',
                    '--fail-prefix', 'FAILED at', '--variant', 'build\\out', 'build\\obj', '', '']
        os.environ.update({'BUILD_CACHE': 'bogus', 'TEST_NOOP_CL': '1'})
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                rc = run_variants.main()
        finally:
            sys.argv = argv
            os.chdir(self.cwd)
        self.assertEqual(rc, 1)
        self.assertEqual(out.getvalue().splitlines(),
                         ['ERROR: BUILD_CACHE="bogus" must be on, write or off (unset means on)', '',
                          'FAILED at build\\out'])
        self.assertFalse(os.path.exists(os.path.join(self.root, 'build')))


class StoreTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='keo_objcache_store_')
        self.store = objcache.Store(os.path.join(self.tmp, 'cache'))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def publish(self, i, size=1000):
        M = objcache.digest(b'key %d' % i)
        content = {'schema': objcache.SCHEMA, 'includes': [['a:x', str(i)]], 'roots': [], 'shadow': {}}
        return M, self.store.publish(M, content, {'obj': os.urandom(size)}, b'out %d\r\n' % i)

    def test_concurrent_publishes_of_one_entry_leave_one_good_entry(self):
        M = objcache.digest(b'shared')
        content = {'schema': objcache.SCHEMA, 'includes': [], 'roots': [], 'shadow': {}}
        blob = os.urandom(200000)
        barrier = threading.Barrier(8)
        results, errors = [], []

        def one():
            barrier.wait()
            try:
                results.append(self.store.publish(M, content, {'obj': blob}, b'out\r\n'))
            except Exception as error:  # noqa: BLE001 -- the test reports any failure
                errors.append(error)
        threads = [threading.Thread(target=one) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        self.assertEqual(len(set(R for R, _ in results)), 1)
        self.assertIn('published', [state for _, state in results])
        R = results[0][0]
        self.assertEqual(self.store.load(M, R), ({'obj': blob}, b'out\r\n'))
        self.assertEqual(os.listdir(self.store.path('tmp')), [])
        self.assertEqual(len(os.listdir(self.store.path('manifests', M))), 1)

    def test_publishes_from_two_processes_leave_one_good_entry(self):
        script = ('import sys, os; sys.path.insert(0, %r); import objcache; s = objcache.Store(%r); '
                  'M = objcache.digest(b"proc"); c = {"schema": 1, "includes": [], "roots": [], "shadow": {}}; '
                  'import time; t = float(sys.argv[1])\nwhile time.time() < t: pass\n'
                  'print(s.publish(M, c, {"obj": b"x" * 500000}, b"o")[1])') % (HERE, self.store.dir)
        start = repr(time.time() + 1.0)
        procs = [subprocess.Popen([sys.executable, '-c', script, start], stdout=subprocess.PIPE,
                                  universal_newlines=True) for _ in range(2)]
        states = [p.communicate()[0].strip() for p in procs]
        self.assertEqual([p.returncode for p in procs], [0, 0])
        self.assertIn('published', states)
        M = objcache.digest(b'proc')
        (R, _), = self.store.candidates(M)
        self.assertEqual(self.store.load(M, R)[0], {'obj': b'x' * 500000})

    def test_eviction_removes_the_least_recently_used_down_to_80_percent(self):
        published = []
        for i in range(10):
            M, (R, _) = self.publish(i, 100000)
            t = time.time() - 1000 + i
            os.utime(os.path.join(self.store.entry_dir(R), objcache.META), (t, t))
            published.append((M, R))
        _, total = self.store._usage()
        removed, before, after = self.store.evict(int(total * 0.5))
        self.assertEqual(before, total)
        self.assertLessEqual(after, total * 0.5 * objcache.EVICT_TO)
        kept = [(M, R) for M, R in published if os.path.isdir(self.store.entry_dir(R))]
        self.assertEqual(kept, published[removed:])
        for M, R in published[:removed]:
            self.assertFalse(os.path.exists(self.store.candidate_path(M, R)))
        self.assertEqual(self.store.evict(10 ** 12), (0, after, after))

    def test_eviction_skips_an_entry_a_reader_holds_and_a_held_lock(self):
        M, (R, _) = self.publish(1)
        with open(os.path.join(self.store.entry_dir(R), 'obj'), 'rb'):
            self.assertEqual(self.store.evict(1)[0], 0)
            self.assertTrue(os.path.isdir(self.store.entry_dir(R)))
        script = ('import msvcrt, os, sys, time; fd = os.open(%r, os.O_RDWR | os.O_CREAT); '
                  'msvcrt.locking(fd, msvcrt.LK_NBLCK, 1); print("held", flush=True); sys.stdin.read()'
                  % self.store.path('evict.lock'))
        holder = subprocess.Popen([sys.executable, '-c', script], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  universal_newlines=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), 'held')
            self.assertIsNone(self.store.evict(1))
        finally:
            holder.stdin.close()
            holder.wait()
            holder.stdout.close()
        self.assertEqual(self.store.evict(1)[0], 1)
        self.assertFalse(os.path.exists(self.store.entry_dir(R)))
        self.assertFalse(os.path.exists(self.store.candidate_path(M, R)))

    def test_a_full_walk_runs_hourly_or_when_the_estimate_is_over_the_cap(self):
        for i in range(4):
            self.publish(i, 10000)
        self.assertTrue(self.store.walk_due(10 ** 12))  # never walked
        removed, before, after = self.store.evict(10 ** 12)
        self.assertEqual((removed, before), (0, after))
        self.assertFalse(self.store.walk_due(10 ** 12))  # walked just now, far under the cap
        self.publish(4, 10000)
        self.assertTrue(self.store.walk_due(before))      # five entries at the last average > four's bytes
        self.assertFalse(self.store.walk_due(before * 2))
        path = self.store.path(objstore.USAGE)
        with open(path, 'rb') as f:
            usage = json.loads(f.read().decode('ascii'))
        usage['walked'] -= objstore.WALK_EVERY
        write(path, json.dumps(usage))
        self.assertTrue(self.store.walk_due(10 ** 12))  # an hour since the last walk
        write(path, '{broken')
        self.assertTrue(self.store.walk_due(10 ** 12))

    def test_load_checks_every_blob_and_the_output(self):
        M, (R, state) = self.publish(3)
        self.assertEqual(state, 'published')
        folder = self.store.entry_dir(R)
        self.store.load(M, R)
        with self.assertRaises(objcache.EntryError):
            self.store.load(objcache.digest(b'another key'), R)
        with open(os.path.join(folder, 'out.txt'), 'ab') as f:
            f.write(b'x')
        with self.assertRaises(objcache.EntryError):
            self.store.load(M, R)


if __name__ == '__main__':
    unittest.main()

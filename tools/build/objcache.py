"""Object cache for tools\\build\\run_variants.py: a compile whose inputs match an earlier one is
restored from the cache instead of run (Python 3, standard library only).

Settings (each refused when malformed):
  BUILD_CACHE       on (default): look up, and publish what was compiled; write: compile every
                    source and publish; off: neither. TEST_NOOP_CL=1 turns lookups and
                    publishes off.
  KEO_CACHE_DIR     the cache, an absolute path shared by every checkout on the machine
                    (default %LOCALAPPDATA%\\KEO\\objcache\\v1).
  KEO_CACHE_MAX_GB  the size, in GiB, above which a run that published evicts (default 20).

Layout:
  manifests\\<M>\\<R>.json       one candidate per earlier compile of key M, written once
  objects\\<R[:2]>\\<R>\\          its entry: the named blobs ("obj"), out.txt, and meta.json,
                               which records each one's sha256 and size and is written last
  bad\\                         candidates and entries a reader refused, kept until evicted
  tmp\\, trash\\                  entries being published or evicted;  evict.lock

The key M is the sha256 of: the schema version; the compiler (the sha256 of the executable and
of c1xx.dll, c2.dll and c1.dll beside it); the executed command without the executable, /Fo...
and /showIncludes, the checkout's root replaced by a fixed token except in a token naming
KENSHILIB or BOOST_ROOT (those paths reach the DLL); the source path relative to the checkout;
the INCLUDE, CL and _CL_ variables; and the source's bytes.

A candidate holds what M cannot: every file cl reported including (its "Note: including file:"
lines), each with its sha256, paths inside the checkout relative to it and others absolute; and
the shadow sets, for each included basename every file of that name under the watched roots (the
top folders of the source and of its includes inside the checkout, and KENSHILIB\\Include), where
a new header would win the include search. R = sha256(M + the candidate's canonical JSON).

Lookup: the newest 8 candidates of M, in turn; one matches when every include still has its
recorded content and every shadow set is unchanged (both checked within the last second). Its
entry's blobs and out.txt are read and checked against meta.json, the object is written beside
its destination and moved into place, and out.txt stands in for cl's output. A candidate or entry
that cannot be read or does not check out is moved to bad\\ and counted as rejected, and the
source is compiled.

Publish, once a variant's compiles have all ended: each compiled source, unless it is not
cacheable (cl failed; no include note; an include missing or at the checkout's root; an include,
the source or a watched folder changed since 2 s before its compile started; its object changed
since the compile; its output naming the checkout). The entry is assembled under tmp\\ and moved
into place; when another writer got there first, theirs stands. The candidate follows.

Eviction: after a run that published, when the cache exceeds KEO_CACHE_MAX_GB, the least recently
used entries (meta.json's mtime, touched on each hit) go until it is under 80 %, by one process
at a time (a non-blocking lock on evict.lock); an entry a reader holds open is skipped.

Not detected, so a clean build (BUILD_CACHE=off) stays the check:
  - a new header under BOOST_ROOT or the INCLUDE folders that would shadow an included one;
  - an object that depends on the absolute path it was compiled at. Objects are shared between
    checkouts, which is sound while the DLLs linked from them are path-independent: no DLL source
    holds an anonymous namespace (MSVC names one after the source's absolute path) or uses
    __DATE__ or __TIME__;
  - a header edited, or a folder changed, within a second of a lookup in the same run.
"""
import hashlib
import json
import os
import re
import shutil
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

try:
    import msvcrt
except ImportError:
    msvcrt = None

SCHEMA = 1
MODES = ('on', 'write', 'off')
CANDIDATES = 8          # candidates of one key tried per lookup, newest first
SLACK = 2.0             # seconds before a compile's start that must already be quiet
FRESH = 1.0             # how old a file or folder check a lookup may reuse, in seconds
EVICT_TO = 0.8          # eviction stops below this share of the cap
TMP_AGE = 3600.0        # an unfinished publish older than this is garbage
PUBLISH_THREADS = 8
GIB = 1 << 30
ROOT_TOKEN = '<checkout>'
META = 'meta.json'
OUTPUT = 'out.txt'
OBJ = 'obj'
COMPILER_FILES = ('c1xx.dll', 'c2.dll', 'c1.dll')
ENV_NAMES = ('INCLUDE', 'CL', '_CL_')
BLOB_NAME_RE = re.compile(r'^[a-z0-9_]+$')
HEX64_RE = re.compile(r'^[0-9a-f]{64}$')
CANDIDATE_RE = re.compile(r'^([0-9a-f]{64})\.json$')


# ---- settings and hashing ----------------------------------------------------------------------

def settings(env=None):
    """(mode, cache directory, cap in bytes) from the environment; ValueError when malformed."""
    env = os.environ if env is None else env
    mode = env.get('BUILD_CACHE', '')
    if mode == '':
        mode = 'on'
    if mode not in MODES:
        raise ValueError('BUILD_CACHE="%s" must be on, write or off (unset means on)' % mode)
    directory = env.get('KEO_CACHE_DIR', '')
    if directory == '':
        base = env.get('LOCALAPPDATA') or os.path.expanduser('~')
        directory = os.path.join(base, 'KEO', 'objcache', 'v1')
    drive, rest = os.path.splitdrive(directory)
    if not drive or not rest.startswith(('\\', '/')):
        raise ValueError('KEO_CACHE_DIR="%s" must be an absolute path' % directory)
    raw = env.get('KEO_CACHE_MAX_GB', '')
    try:
        gib = 20.0 if raw == '' else float(raw)
    except ValueError:
        gib = -1.0
    if not (0 < gib < float('inf')):
        raise ValueError('KEO_CACHE_MAX_GB="%s" must be a number above 0' % raw)
    return mode, os.path.normpath(directory), int(gib * GIB)


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('ascii')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def candidate_id(M, content):
    return digest(M.encode('ascii') + canonical(content))


class Changed(OSError):
    """A file changed while it was being hashed."""


class Hashes(object):
    """Each file's content sha256, memoized for the process by (path, size, mtime_ns). A caller
    names how old a stat it accepts, since a stat costs more than reusing one."""

    def __init__(self):
        self._memo = {}  # path -> (time the stat began, stat, sha256)

    def get(self, path, after):
        """(stat, sha256) of path from a stat begun at or after `after`; OSError when missing,
        Changed when it changed while being hashed."""
        seen = self._memo.get(path)
        if seen is not None and seen[0] >= after:
            return seen[1], seen[2]
        began = time.time()
        st = os.stat(path)
        if seen is not None and (seen[1].st_size, seen[1].st_mtime_ns) == (st.st_size, st.st_mtime_ns):
            sha = seen[2]
        else:
            sha = file_digest(path)
            again = os.stat(path)
            if (again.st_size, again.st_mtime_ns) != (st.st_size, st.st_mtime_ns):
                raise Changed('%s changed while it was hashed' % path)
        self._memo[path] = (began, st, sha)
        return st, sha


class Folders(object):
    """Files by lower-cased basename under each watched root, and every folder's mtime. NTFS
    changes a folder's mtime when an entry in it is created, deleted or renamed, so a root is
    walked again only when one of its folders' mtimes moved."""

    def __init__(self):
        self._lock = threading.Lock()
        self._views = {}  # root -> (time the check began, names, folders)

    def view(self, root, after):
        """(names {basename: [paths]}, folders {path: mtime_ns or None}) of root from a check
        begun at or after `after`. A missing root is one folder whose mtime is None."""
        with self._lock:
            seen = self._views.get(root)
            if seen is not None and seen[0] >= after:
                return seen[1], seen[2]
            began = time.time()
            if seen is None or self._moved(seen[2]):
                names, folders = self._walk(root)
            else:
                names, folders = seen[1], seen[2]
            self._views[root] = (began, names, folders)
            return names, folders

    @staticmethod
    def _moved(folders):
        for path, mtime in folders.items():
            try:
                if os.stat(path).st_mtime_ns != mtime:
                    return True
            except OSError:
                if mtime is not None:
                    return True
        return False

    @staticmethod
    def _walk(root):
        names, folders = {}, {}
        try:
            folders[root] = os.stat(root).st_mtime_ns
        except OSError:
            return names, {root: None}
        pending = [root]
        while pending:
            folder = pending.pop()
            with os.scandir(folder) as entries:
                for e in entries:
                    path = os.path.normcase(e.path)
                    if e.is_dir(follow_symlinks=False):
                        folders[path] = os.stat(path).st_mtime_ns  # the listing's copy can lag
                        pending.append(path)
                    else:
                        names.setdefault(e.name.lower(), []).append(path)
        return names, folders


# ---- the store: candidates and entries of named blobs plus an output text -----------------------

class EntryError(Exception):
    """A candidate or entry that cannot be read or does not check out."""


def write_file(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def write_replace(path, data):
    """Writes data beside path, then moves it into place."""
    tmp = '%s.%d.%s.tmp' % (path, os.getpid(), uuid.uuid4().hex[:8])
    try:
        write_file(tmp, data)
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


class Store(object):
    """Immutable candidates and entries under one directory; generic over the blobs it holds."""

    def __init__(self, directory):
        self.dir = directory

    def path(self, *parts):
        return os.path.join(self.dir, *parts)

    def entry_dir(self, R):
        return self.path('objects', R[:2], R)

    def candidate_path(self, M, R):
        return self.path('manifests', M, R + '.json')

    def candidates(self, M):
        """[(R, path)] of key M, newest first, at most CANDIDATES."""
        found = []
        try:
            with os.scandir(self.path('manifests', M)) as entries:
                for e in entries:
                    m = CANDIDATE_RE.match(e.name)
                    if m:
                        try:
                            found.append((e.stat().st_mtime_ns, m.group(1), e.path))
                        except OSError:
                            pass
        except OSError:
            return []
        found.sort(reverse=True)
        return [(R, path) for _, R, path in found[:CANDIDATES]]

    def read_candidate(self, M, R, path):
        try:
            with open(path, 'rb') as f:
                content = json.loads(f.read().decode('ascii'))
        except (OSError, ValueError) as error:
            raise EntryError('candidate unreadable (%s)' % error)
        if candidate_id(M, content) != R:
            raise EntryError('candidate does not hash to its name')
        return content

    def load(self, M, R):
        """({name: bytes}, output bytes) of entry R, each checked against its meta.json."""
        folder = self.entry_dir(R)
        try:
            with open(os.path.join(folder, META), 'rb') as f:
                meta = json.loads(f.read().decode('ascii'))
            if (meta.get('schema'), meta.get('R'), meta.get('M')) != (SCHEMA, R, M):
                raise EntryError('meta.json is not this entry\'s')
            blobs = {}
            for name, info in sorted(meta['blobs'].items()):
                if not BLOB_NAME_RE.match(name):
                    raise EntryError('meta.json names a bad blob %r' % name)
                blobs[name] = self._checked(folder, name, info['sha256'], info['size'])
            output = self._checked(folder, OUTPUT, meta['out_sha256'], meta['out_size'])
        except EntryError:
            raise
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            raise EntryError('entry unreadable (%s: %s)' % (type(error).__name__, error))
        return blobs, output

    @staticmethod
    def _checked(folder, name, sha, size):
        with open(os.path.join(folder, name), 'rb') as f:
            data = f.read()
        if len(data) != size or digest(data) != sha:
            raise EntryError('%s does not match meta.json' % name)
        return data

    def touch(self, M, R):
        for path in (os.path.join(self.entry_dir(R), META), self.candidate_path(M, R)):
            try:
                os.utime(path)
            except OSError:
                pass

    def publish(self, M, content, blobs, output):
        """Stores an entry and its candidate; returns (R, 'published') or (R, 'exists') when both
        were already there. OSError when neither could be written."""
        R = candidate_id(M, content)
        final = self.entry_dir(R)
        created = False
        if not os.path.isdir(final):
            tmp = self.path('tmp', '%s.%d.%s' % (R[:16], os.getpid(), uuid.uuid4().hex[:8]))
            os.makedirs(tmp)
            try:
                meta = {'schema': SCHEMA, 'R': R, 'M': M, 'blobs': {}, 'created': time.time(),
                        'out_sha256': digest(output), 'out_size': len(output)}
                for name, data in sorted(blobs.items()):
                    if not BLOB_NAME_RE.match(name):
                        raise ValueError('bad blob name %r' % name)
                    write_file(os.path.join(tmp, name), data)
                    meta['blobs'][name] = {'sha256': digest(data), 'size': len(data)}
                write_file(os.path.join(tmp, OUTPUT), output)
                write_file(os.path.join(tmp, META), canonical(meta))
                os.makedirs(os.path.dirname(final), exist_ok=True)
                try:
                    os.replace(tmp, final)
                    created = True
                except OSError:
                    if not os.path.isdir(final):  # not a lost race: a real failure
                        raise
            finally:
                if not created:
                    shutil.rmtree(tmp, ignore_errors=True)
        cand = self.candidate_path(M, R)
        os.makedirs(os.path.dirname(cand), exist_ok=True)
        tmp = os.path.join(os.path.dirname(cand), '.%s.%d.%s.tmp' % (R[:16], os.getpid(), uuid.uuid4().hex[:8]))
        write_file(tmp, canonical(content))
        try:
            os.rename(tmp, cand)
            created = True
        except OSError:
            os.remove(tmp)
            if not os.path.isfile(cand):
                raise
            try:
                os.utime(cand)  # newest again, so it stays among the candidates a lookup reads
            except OSError:
                pass
        return R, 'published' if created else 'exists'

    def reject(self, M, R, why):
        """Moves entry R and its candidate to bad\\ with a note why; best effort."""
        bad = self.path('bad')
        stamp = '%s.%s.%d.%s' % (R, time.strftime('%Y%m%d-%H%M%S'), os.getpid(), uuid.uuid4().hex[:8])
        try:
            os.makedirs(bad, exist_ok=True)
            write_file(os.path.join(bad, stamp + '.why.txt'), why.encode('utf-8', 'replace'))
        except OSError:
            return
        for source, target in ((self.entry_dir(R), stamp), (self.candidate_path(M, R), stamp + '.json')):
            try:
                os.replace(source, os.path.join(bad, target))
            except OSError:
                pass

    def evict(self, cap):
        """Deletes least recently used entries until the cache is under EVICT_TO of cap. Returns
        (removed, bytes before, bytes after), or None when another process is evicting."""
        os.makedirs(self.dir, exist_ok=True)
        fd = os.open(self.path('evict.lock'), os.O_RDWR | os.O_CREAT, 0o666)
        try:
            if msvcrt is not None:
                try:
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                except OSError:
                    return None
            try:
                items, total = self._usage()
                before = total
                removed = 0
                if total > cap:
                    for _, size, kind, path, R in sorted(items):
                        if total <= cap * EVICT_TO:
                            break
                        freed = self._remove(kind, path, R)
                        if freed is not None:
                            total -= size + freed
                            removed += 1
                return removed, before, total
            finally:
                if msvcrt is not None:
                    os.lseek(fd, 0, os.SEEK_SET)
                    msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        finally:
            os.close(fd)

    def _usage(self):
        """[(mtime, size, kind, path, R)] of what eviction may delete, and the cache's size."""
        items, total = [], 0
        now = time.time()
        for top in ('objects', 'bad', 'tmp', 'trash', 'manifests'):
            try:
                tops = list(os.scandir(self.path(top)))
            except OSError:
                continue
            for e in tops:
                if top == 'objects' and e.is_dir():
                    try:
                        entries = list(os.scandir(e.path))
                    except OSError:
                        entries = []
                    for f in entries:
                        size = tree_size(f.path)
                        total += size
                        if HEX64_RE.match(f.name):
                            try:
                                mtime = os.stat(os.path.join(f.path, META)).st_mtime
                            except OSError:
                                mtime = 0.0  # no meta: never complete, goes first
                            items.append((mtime, size, 'entry', f.path, f.name))
                    continue
                size = tree_size(e.path)
                total += size
                try:
                    mtime = e.stat().st_mtime
                except OSError:
                    continue
                if top in ('bad', 'trash') or (top == 'tmp' and now - mtime > TMP_AGE):
                    items.append((0.0, size, top, e.path, ''))  # refused or abandoned: first to go
        return items, total

    def _remove(self, kind, path, R):
        """Deletes one item; returns the bytes freed beyond its own size (an entry's candidate),
        or None when it could not be deleted."""
        if kind != 'entry':
            remove_tree(path)
            return None if os.path.exists(path) else 0
        try:
            with open(os.path.join(path, META), 'rb') as f:
                M = json.loads(f.read().decode('ascii')).get('M')
        except (OSError, ValueError, AttributeError):
            M = None
        trash = self.path('trash', '%s.%s' % (R, uuid.uuid4().hex[:8]))
        try:
            os.makedirs(self.path('trash'), exist_ok=True)
            os.replace(path, trash)  # refused while a reader holds one of its files open
        except OSError:
            return None
        freed = 0
        if isinstance(M, str) and HEX64_RE.match(M):
            try:
                freed = os.stat(self.candidate_path(M, R)).st_size
                os.remove(self.candidate_path(M, R))
                os.rmdir(self.path('manifests', M))
            except OSError:
                pass
        remove_tree(trash)
        return freed


def tree_size(path):
    if not os.path.isdir(path):
        try:
            return os.stat(path).st_size
        except OSError:
            return 0
    total = 0
    for folder, _, files in os.walk(path):
        for name in files:
            try:
                total += os.stat(os.path.join(folder, name)).st_size
            except OSError:
                pass
    return total


def remove_tree(path):
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)
    else:
        try:
            os.remove(path)
        except OSError:
            pass


# ---- the cl object use ------------------------------------------------------------------------

class Key(object):
    """One source's lookup key and what publishing it needs to re-check."""
    __slots__ = ('M', 'source', 'source_sha')

    def __init__(self, M, source, source_sha):
        self.M, self.source, self.source_sha = M, source, source_sha


class Compiled(object):
    """What publishing needs from one finished compile."""
    __slots__ = ('key', 'start', 'rc', 'obj_path', 'obj_sha', 'output', 'includes')

    def __init__(self, key, start, rc, obj_path, obj_sha, output, includes):
        self.key, self.start, self.rc, self.obj_path = key, start, rc, obj_path
        self.obj_sha, self.output, self.includes = obj_sha, output, includes


class ObjectCache(object):
    """The cache as one variant run uses it for cl objects; safe to share between threads."""

    def __init__(self, mode, directory, cap, root, keep=(), include_root=None, noop=False):
        self.mode, self.directory, self.cap, self.noop = mode, directory, cap, noop
        self.lookups = mode == 'on' and not noop
        self.publishes = mode in ('on', 'write') and not noop
        self.store = Store(directory)
        self.root = os.path.normcase(os.path.abspath(root))
        self.keep = [k.lower() for k in keep if k]
        self.include_root = os.path.normcase(os.path.abspath(include_root)) if include_root else None
        parts = [re.escape(p) for p in re.split(r'[\\/]+', self.root) if p]
        self.root_re = re.compile(r'[\\/]+'.join(parts) + r'(?=[\\/"\'\s]|$)', re.IGNORECASE)
        self.hashes = Hashes()
        self.folders = Folders()
        self._compiler = {}
        self._lock = threading.Lock()
        self.published = 0

    @classmethod
    def from_env(cls, root, env=None):
        """ValueError when a setting is malformed."""
        env = os.environ if env is None else env
        mode, directory, cap = settings(env)
        kenshilib = env.get('KENSHILIB', '')
        return cls(mode, directory, cap, root, (kenshilib, env.get('BOOST_ROOT', '')),
                   os.path.join(kenshilib, 'Include') if kenshilib else None,
                   noop=env.get('TEST_NOOP_CL') == '1')

    def describe(self):
        if self.noop and self.mode != 'off':
            return 'BUILD_CACHE=%s, off under TEST_NOOP_CL=1' % self.mode
        return 'BUILD_CACHE=%s' % self.mode

    # paths inside the checkout are kept relative to it ("r:"), others absolute ("a:")
    def tag(self, path):
        path = os.path.normcase(os.path.abspath(path))
        if path.startswith(self.root + os.sep):
            return 'r:' + path[len(self.root) + 1:]
        return 'a:' + path

    def untag(self, tagged):
        if tagged.startswith('r:'):
            return os.path.join(self.root, tagged[2:])
        if tagged.startswith('a:'):
            return tagged[2:]
        raise ValueError('not a cache path: %r' % tagged)

    @staticmethod
    def top(tagged):
        """The watched root of a path inside the checkout: its first folder; None at the root or
        outside the checkout."""
        if not tagged.startswith('r:'):
            return None
        head, sep, _ = tagged[2:].partition(os.sep)
        return 'r:' + head if sep else None

    def compiler(self, exe):
        with self._lock:
            if exe not in self._compiler:
                folder = os.path.dirname(exe)
                found = {}
                for name, path in [('exe', exe)] + [(n, os.path.join(folder, n)) for n in COMPILER_FILES]:
                    try:
                        found[name] = self.hashes.get(os.path.normcase(path), 0)[1]
                    except OSError:
                        found[name] = None
                self._compiler[exe] = found
            return self._compiler[exe]

    def key(self, command, source, env=None):
        """The Key of running command (the executable first) to compile source. OSError when the
        source or the compiler cannot be read."""
        env = os.environ if env is None else env
        tokens = []
        for token in command[1:]:
            if token[:1] in ('/', '-') and (token[1:] == 'showIncludes' or token[1:3] == 'Fo'):
                continue
            if any(k in token.lower() for k in self.keep):
                tokens.append(token)
            else:
                tokens.append(self.root_re.sub(ROOT_TOKEN, token))
        source_sha = file_digest(source)
        parts = {'schema': SCHEMA, 'compiler': self.compiler(command[0]), 'command': tokens,
                 'source': self.tag(source), 'env': dict((n, env.get(n)) for n in ENV_NAMES),
                 'source_sha256': source_sha}
        return Key(digest(canonical(parts)), source, source_sha)

    def restore(self, key, obj_path):
        """Writes the cached object of key to obj_path. Returns ((output, includes, R) or None,
        the number of candidates and entries refused)."""
        rejected = 0
        for R, path in self.store.candidates(key.M):
            try:
                content = self.store.read_candidate(key.M, R, path)
                try:
                    if not self._current(content):
                        continue
                except OSError:  # a watched folder could not be listed: not this entry's fault
                    continue
                blobs, output = self.store.load(key.M, R)
                if sorted(blobs) != [OBJ]:
                    raise EntryError('blobs %s, expected only %s' % (sorted(blobs), OBJ))
                includes = [self.untag(p) for p, _ in content['includes']]
            except (EntryError, KeyError, TypeError, ValueError) as error:
                self.store.reject(key.M, R, '%s: %s' % (key.source, error))
                rejected += 1
                continue
            try:
                write_replace(obj_path, blobs[OBJ])
            except OSError:
                return None, rejected
            self.store.touch(key.M, R)
            return (output, includes, R), rejected
        return None, rejected

    def _current(self, content):
        """Whether every include still has its recorded content and every shadow set is as it was."""
        after = time.time() - FRESH
        for tagged, sha in content['includes']:
            try:
                if self.hashes.get(self.untag(tagged), after)[1] != sha:
                    return False
            except OSError:
                return False
        return self._shadow(content['roots'], list(content['shadow']), after)[0] == content['shadow']

    def _shadow(self, roots, basenames, after):
        """({basename: sorted tagged paths} over the roots, the newest folder mtime in ns)."""
        found = dict((b, set()) for b in basenames)
        newest = 0
        for root in roots:
            names, folders = self.folders.view(self.untag(root), after)
            for b in basenames:
                found[b].update(names.get(b, ()))
            newest = max([newest] + [m for m in folders.values() if m is not None])
        return dict((b, sorted(self.tag(p) for p in paths)) for b, paths in found.items()), newest

    def publish_all(self, compiled):
        """Publishes each finished compile; returns one (state, detail) per item in order: state
        'published' or 'exists' with R as detail, or 'uncacheable' with the reason."""
        after = time.time()  # every compile has ended: a check from here on sees its whole window
        with ThreadPoolExecutor(PUBLISH_THREADS) as pool:
            results = list(pool.map(lambda c: self._publish_one(c, after), compiled))
        with self._lock:
            self.published += sum(1 for state, _ in results if state == 'published')
        return results

    def _publish_one(self, c, after):
        if c.rc != 0:
            return 'uncacheable', 'cl exit %s' % c.rc
        if not c.includes:
            return 'uncacheable', 'no "Note: including file:" line'
        if c.obj_sha is None:
            return 'uncacheable', 'no object after the compile'
        if self.root_re.search(c.output.decode('mbcs', 'replace')):
            return 'uncacheable', 'the output names the checkout'
        quiet = c.start - SLACK
        try:
            listed = []
            roots = set([self.top(self.tag(c.key.source))])  # None: outside the checkout or at its root
            if self.include_root:
                roots.add(self.tag(self.include_root))
            for path in c.includes:
                st, sha = self.hashes.get(path, after)
                if st.st_mtime >= quiet:
                    return 'uncacheable', '%s changed during the compile' % path
                tagged = self.tag(path)
                if tagged.startswith('r:'):
                    roots.add(self.top(tagged))
                listed.append([tagged, sha])
            if None in roots:
                return 'uncacheable', 'the source or an include outside a folder of the checkout'
            if os.stat(c.key.source).st_mtime >= quiet or file_digest(c.key.source) != c.key.source_sha:
                return 'uncacheable', 'the source changed during the compile'
            basenames = sorted(set(os.path.basename(p).lower() for p in c.includes))
            shadow, newest = self._shadow(sorted(roots), basenames, after)
            if newest >= quiet * 1e9:
                return 'uncacheable', 'a watched folder changed during the compile'
            with open(c.obj_path, 'rb') as f:
                obj = f.read()
            if digest(obj) != c.obj_sha:
                return 'uncacheable', 'the object changed after the compile'
            content = {'schema': SCHEMA, 'includes': listed, 'roots': sorted(roots), 'shadow': shadow}
            R, state = self.store.publish(c.key.M, content, {OBJ: obj}, c.output)
            return state, R
        except Changed as error:
            return 'uncacheable', str(error)
        except OSError as error:
            return 'uncacheable', 'not published (%s)' % error

    def finish(self):
        """After the run: evicts when this run published and the cache is over its cap. Returns a
        line to print, or None."""
        if not self.published:
            return None
        try:
            result = self.store.evict(self.cap)
        except OSError as error:
            return 'object cache eviction failed (%s)' % error
        if not result or not result[0]:
            return None
        removed, before, after = result
        return ('object cache eviction: %d entr%s removed, %.2f GiB -> %.2f GiB (KEO_CACHE_MAX_GB %.2f)'
                % (removed, 'y' if removed == 1 else 'ies', before / float(GIB), after / float(GIB),
                   self.cap / float(GIB)))

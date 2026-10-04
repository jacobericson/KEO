"""The generic core of tools\\build\\objcache.py (Python 3, standard library only): file change
times, a content-hash memo, a watched-folder index, and the store of immutable candidates and
entries (named blobs plus an output text) with its eviction. Nothing here knows about cl.

A file's "times" are its last write time and its change time, in ns since 1970. NTFS moves the
change time on every write, rename or attribute change, including the mtime a copy tool puts
back afterwards, and copy tools leave it alone, so equal times mean equal bytes unless someone
set the change time on purpose (SetFileInformationByHandle can). Elsewhere st_ctime_ns serves.

Store layout:
  manifests\\<M>\\<R>.json       one candidate per earlier result of key M, written once
  objects\\<R[:2]>\\<R>\\          its entry: the named blobs, out.txt, and meta.json, which
                               records each one's sha256 and size and is written last
  bad\\                         candidates and entries refused or replaced, kept until evicted
  tmp\\, trash\\                  entries being published or evicted
  evict.lock, usage.json       one evictor at a time; the last full walk's size and time
"""
import hashlib
import json
import os
import re
import shutil
import threading
import time
import uuid

try:
    import msvcrt
except ImportError:
    msvcrt = None

SCHEMA = 1
CANDIDATES = 8          # candidates of one key tried per lookup, newest first
EVICT_TO = 0.8          # eviction stops below this share of the cap
WALK_EVERY = 3600.0     # at most one full walk of the store per this many seconds, unless over the cap
TMP_AGE = 3600.0        # an unfinished publish older than this is garbage
META = 'meta.json'
OUTPUT = 'out.txt'
USAGE = 'usage.json'
BLOB_NAME_RE = re.compile(r'^[a-z0-9_]+$')
HEX64_RE = re.compile(r'^[0-9a-f]{64}$')
CANDIDATE_RE = re.compile(r'^([0-9a-f]{64})\.json$')
EPOCH_DIFF_100NS = 116444736000000000  # 1601-01-01 to 1970-01-01


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


if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _k32.CreateFileW.restype = wintypes.HANDLE
    _k32.GetFileInformationByHandleEx.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                  wintypes.DWORD]
    _k32.GetFileInformationByHandleEx.restype = wintypes.BOOL
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.restype = wintypes.BOOL
    _INVALID_HANDLE = ctypes.c_void_p(-1).value

    class _BasicInfo(ctypes.Structure):  # FILE_BASIC_INFO
        _fields_ = [('creation', ctypes.c_longlong), ('access', ctypes.c_longlong),
                    ('write', ctypes.c_longlong), ('change', ctypes.c_longlong),
                    ('attributes', wintypes.DWORD)]

    def file_times(path):
        """(write, change) times of a file or folder, ns since 1970; OSError when missing."""
        # FILE_READ_ATTRIBUTES, share everything, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS (folders)
        handle = _k32.CreateFileW(path, 0x80, 7, None, 3, 0x02000000, None)
        if handle is None or handle == _INVALID_HANDLE:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            info = _BasicInfo()
            if not _k32.GetFileInformationByHandleEx(handle, 0, ctypes.byref(info), ctypes.sizeof(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            return ((info.write - EPOCH_DIFF_100NS) * 100, (info.change - EPOCH_DIFF_100NS) * 100)
        finally:
            _k32.CloseHandle(handle)
else:
    def file_times(path):
        st = os.stat(path)
        return st.st_mtime_ns, st.st_ctime_ns


def newest(times):
    """The later of a file's two times, in seconds since 1970."""
    return max(times) / 1e9


class Changed(OSError):
    """A file changed while it was being hashed."""


class Hashes(object):
    """Each file's content sha256, memoized for the process by (path, times). A caller names how
    old a times read it accepts, since a read costs more than reusing one."""

    def __init__(self):
        self._memo = {}  # path -> (time the read began, times, sha256)

    def get(self, path, after):
        """(times, sha256) of path from a read begun at or after `after`; OSError when missing,
        Changed when it changed while being hashed."""
        seen = self._memo.get(path)
        if seen is not None and seen[0] >= after:
            return seen[1], seen[2]
        began = time.time()
        times = file_times(path)
        if seen is not None and seen[1] == times:
            sha = seen[2]
        else:
            sha = file_digest(path)
            if file_times(path) != times:
                raise Changed('%s changed while it was hashed' % path)
        self._memo[path] = (began, times, sha)
        return times, sha


class Folders(object):
    """Files by lower-cased basename under each watched root, and every folder's times. A folder's
    times move when an entry in it is created, deleted or renamed, so a root is walked again only
    when one of its folders' times moved."""

    def __init__(self):
        self._lock = threading.Lock()
        self._views = {}  # root -> (time the check began, names, folders)

    def view(self, root, after):
        """(names {basename: [paths]}, folders {path: times or None}) of root from a check begun
        at or after `after`. A missing root is one folder whose times are None."""
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
        for path, times in folders.items():
            try:
                if file_times(path) != times:
                    return True
            except OSError:
                if times is not None:
                    return True
        return False

    @staticmethod
    def _walk(root):
        names, folders = {}, {}
        try:
            folders[root] = file_times(root)
        except OSError:
            return names, {root: None}
        pending = [root]
        while pending:
            folder = pending.pop()
            with os.scandir(folder) as entries:
                for e in entries:
                    path = os.path.normcase(e.path)
                    if e.is_dir(follow_symlinks=False):
                        folders[path] = file_times(path)  # the listing's copy can lag
                        pending.append(path)
                    else:
                        names.setdefault(e.name.lower(), []).append(path)
        return names, folders


class EntryError(Exception):
    """A candidate or entry that cannot be read or does not check out."""


class Kept(OSError):
    """An entry to be replaced could not be moved aside (a reader holds it open)."""


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


def tree_size(path):
    """Bytes under path, from the folder listings (no file is opened)."""
    if not os.path.isdir(path):
        try:
            return os.stat(path).st_size
        except OSError:
            return 0
    total, pending = 0, [path]
    while pending:
        try:
            with os.scandir(pending.pop()) as entries:
                for e in entries:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            pending.append(e.path)
                        else:
                            total += e.stat(follow_symlinks=False).st_size
                    except OSError:
                        pass
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

    def publish(self, M, content, blobs, output, replace=False):
        """Stores an entry and its candidate; returns (R, 'published') or (R, 'exists') when both
        were already there. With replace, an existing entry R is moved to bad\\ first (Kept when
        it cannot be). OSError when neither could be written."""
        R = candidate_id(M, content)
        final = self.entry_dir(R)
        if replace and os.path.isdir(final):
            self.reject(M, R, 'replaced: published again under BUILD_CACHE=write')
            if os.path.isdir(final):
                raise Kept('the entry already stored could not be moved aside (in use)')
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

    def walk_due(self, cap):
        """Whether eviction must walk the store: no walk within WALK_EVERY, or the entry count
        times the last walk's bytes per entry is above cap. Counting reads one listing per
        objects\\<xx> folder and opens no entry."""
        try:
            with open(self.path(USAGE), 'rb') as f:
                usage = json.loads(f.read().decode('ascii'))
            walked, size, entries = float(usage['walked']), int(usage['bytes']), int(usage['entries'])
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            return True
        now = time.time()
        if not (walked <= now < walked + WALK_EVERY):
            return True
        count = 0
        try:
            with os.scandir(self.path('objects')) as tops:
                for top in tops:
                    try:
                        count += len(os.listdir(top.path))
                    except OSError:
                        pass
        except OSError:
            return True
        return count * (size / float(max(entries, 1))) > cap

    def evict(self, cap):
        """Walks the store and deletes least recently used entries until it is under EVICT_TO of
        cap; records the walk in usage.json. Returns (removed, bytes before, bytes after), or None
        when another process is evicting."""
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
                before, removed = total, 0
                entries = sum(1 for item in items if item[2] == 'entry')
                if total > cap:
                    for _, size, kind, path, R in sorted(items):
                        if total <= cap * EVICT_TO:
                            break
                        freed = self._remove(kind, path, R)
                        if freed is not None:
                            total -= size + freed
                            removed += 1
                            entries -= kind == 'entry'
                try:
                    write_replace(self.path(USAGE), canonical(
                        {'walked': time.time(), 'bytes': total, 'entries': entries}))
                except OSError:
                    pass
                return removed, before, total
            finally:
                if msvcrt is not None:
                    os.lseek(fd, 0, os.SEEK_SET)
                    msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        finally:
            os.close(fd)

    def _usage(self):
        """[(mtime, size, kind, path, R)] of what eviction may delete, and the store's size."""
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
                        size, used = self._entry_usage(f.path)
                        total += size
                        if HEX64_RE.match(f.name):
                            items.append((used, size, 'entry', f.path, f.name))
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

    @staticmethod
    def _entry_usage(path):
        """(bytes, last use) of an entry from its listing; last use 0 when it has no meta.json
        (never completed, so it goes first)."""
        size, used = 0, 0.0
        try:
            with os.scandir(path) as files:
                for f in files:
                    try:
                        st = f.stat(follow_symlinks=False)
                    except OSError:
                        continue
                    size += st.st_size
                    if f.name == META:
                        used = st.st_mtime
        except OSError:
            pass
        return size, used

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

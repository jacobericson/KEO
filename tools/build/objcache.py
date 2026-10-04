"""Object cache for tools\\build\\run_variants.py: a compile whose inputs match an earlier one is
restored from the cache instead of run (Python 3, standard library only). The store, the file
times and the hash memo it rests on are tools\\build\\objstore.py.

Settings (each refused when malformed):
  BUILD_CACHE       on (default): look up, and publish what was compiled; write: compile every
                    source and publish, moving any entry already stored for the same inputs to
                    bad\\ first (the one way an entry is replaced, so it repairs a bad one); off:
                    neither. TEST_NOOP_CL=1 turns lookups and publishes off.
  KEO_CACHE_DIR     the cache, an absolute path shared by every checkout on the machine
                    (default %LOCALAPPDATA%\\KEO\\objcache\\v1). Deleting it is always safe and
                    is the recovery from any doubt about its contents.
  KEO_CACHE_MAX_GB  the size, in GiB, above which a run that published evicts (default 20).

The key M is the sha256 of: the schema version; the compiler (the sha256 of every .exe, .dll and
.config in cl's folder and its 1033 folder); the executed command without the executable, /Fo...
and /showIncludes, the checkout's root replaced by a fixed token except in a token naming
KENSHILIB or BOOST_ROOT (those paths reach the DLL); each /I folder resolved (relative to the
checkout inside it, else absolute); the source path relative to the checkout; the INCLUDE, CL
and _CL_ variables; and the source's bytes.

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
source is compiled. Rejections alone are no defect: a lookup racing a publish or an eviction, or
a file a scanner holds for a moment, is refused the same way. Before the link, every restored
object's inputs and shadow sets are read afresh, and any that changed during the build refuses
the link.

Right after each compile, the source's and every include's times and sha256 are captured; a file
whose write or change time falls after 2 s before the compile started makes the compile not
cacheable. Publish, once a variant's compiles have all ended, reads them again: each compiled
source is published unless it is not cacheable (cl failed; no include note; an input missing or
at the checkout's root; an input whose times or sha256 moved since the capture; a watched folder
changed since 2 s before its compile started; its object changed since the compile; its output
naming the checkout). The entry is assembled under tmp\\ and moved into place; when another writer
got there first, theirs stands. The candidate follows.

Eviction: after a run that published, when the cache is over KEO_CACHE_MAX_GB, the least recently
used entries (meta.json's mtime, touched on each hit) go until it is under 80 %, by one process
at a time; an entry a reader holds open is skipped. The full walk runs at most once an hour
unless the entry count times the last walk's bytes per entry is over the cap. Every run that
published also deletes what has been in bad\\ over an hour (refused, or replaced by write).

Not detected, so a clean build (BUILD_CACHE=off) stays the check:
  - a new header under BOOST_ROOT or the INCLUDE folders that would shadow an included one;
  - an input whose change time was set back on purpose (a copy tool never does);
  - an input written through a memory map (neither write time nor change time moves);
  - an object that depends on the absolute path it was compiled at. Objects are shared between
    checkouts, which is sound while the DLLs linked from them are path-independent: no DLL source
    holds an anonymous namespace (MSVC names one after the source's absolute path) or uses
    __DATE__ or __TIME__;
  - a folder changed within a second of a lookup in the same run.
"""
import os
import re
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from objstore import (CANDIDATES, EVICT_TO, META, SCHEMA, Changed, EntryError, Folders,  # noqa: F401
                      Hashes, Kept, Store, canonical, digest, file_digest, newest, write_replace)

MODES = ('on', 'write', 'off')
SLACK = 2.0             # seconds before a compile's start that must already be quiet
FRESH = 1.0             # how old a file or folder check a lookup may reuse, in seconds
PUBLISH_THREADS = 8
GIB = 1 << 30
ROOT_TOKEN = '<checkout>'
OBJ = 'obj'
COMPILER_EXTENSIONS = ('.exe', '.dll', '.config')
ENV_NAMES = ('INCLUDE', 'CL', '_CL_')
LISTED = 20             # problems named before the link is refused


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


class Key(object):
    """One source's lookup key and what publishing it needs to re-check."""
    __slots__ = ('M', 'source', 'source_sha')

    def __init__(self, M, source, source_sha):
        self.M, self.source, self.source_sha = M, source, source_sha


class Compiled(object):
    """What publishing needs from one finished compile: captured is [(path, times, sha256)] for
    the source and then each include, read right after the compile, or None with reason."""
    __slots__ = ('key', 'start', 'rc', 'obj_path', 'obj_sha', 'output', 'includes', 'captured', 'reason')

    def __init__(self, key, start, rc, obj_path, obj_sha, output, includes, captured=None, reason=None):
        self.key, self.start, self.rc, self.obj_path = key, start, rc, obj_path
        self.obj_sha, self.output, self.includes = obj_sha, output, includes
        self.captured, self.reason = captured, reason


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

    @staticmethod
    def norm(path):
        return os.path.normcase(os.path.abspath(path))

    def compiler(self, exe):
        """{file: sha256} of every .exe, .dll and .config in exe's folder and its 1033 folder,
        plus which executable runs."""
        with self._lock:
            if exe not in self._compiler:
                folder = os.path.dirname(os.path.abspath(exe))
                found = {'run': os.path.basename(exe).lower()}
                for sub in ('', '1033'):
                    try:
                        names = sorted(os.listdir(os.path.join(folder, sub)))
                    except OSError:
                        continue
                    for name in names:
                        if os.path.splitext(name)[1].lower() in COMPILER_EXTENSIONS:
                            path = self.norm(os.path.join(folder, sub, name))
                            try:
                                found[os.path.join(sub, name).lower()] = self.hashes.get(path, 0)[1]
                            except OSError:
                                found[os.path.join(sub, name).lower()] = None
                self._compiler[exe] = found
            return self._compiler[exe]

    def key(self, command, source, env=None):
        """The Key of running command (the executable first) to compile source. OSError when the
        source or the compiler cannot be read."""
        env = os.environ if env is None else env
        tokens, include_dirs = [], []
        args = command[1:]
        for i, token in enumerate(args):
            if token[:1] in ('/', '-') and (token[1:] == 'showIncludes' or token[1:3] == 'Fo'):
                continue
            if token[:1] in ('/', '-') and token[1:2] == 'I':
                folder = token[2:] or (args[i + 1] if i + 1 < len(args) else '')
                if folder:
                    include_dirs.append(self.tag(folder))
            if any(k in token.lower() for k in self.keep):
                tokens.append(token)
            else:
                tokens.append(self.root_re.sub(ROOT_TOKEN, token))
        source_sha = file_digest(source)
        parts = {'schema': SCHEMA, 'compiler': self.compiler(command[0]), 'command': tokens,
                 'include_dirs': include_dirs, 'source': self.tag(source),
                 'env': dict((n, env.get(n)) for n in ENV_NAMES), 'source_sha256': source_sha}
        return Key(digest(canonical(parts)), source, source_sha)

    def restore(self, key, obj_path):
        """Writes the cached object of key to obj_path. Returns ((output, includes, R, inputs) or
        None, the number of candidates and entries refused); inputs, for recheck(), is
        ([(path, sha256)] of the source and each include, the candidate's roots, its shadow sets)."""
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
                inputs = [(self.untag(p), sha) for p, sha in content['includes']]
            except (EntryError, KeyError, TypeError, ValueError) as error:
                self.store.reject(key.M, R, '%s: %s' % (key.source, error))
                rejected += 1
                continue
            try:
                write_replace(obj_path, blobs[OBJ])
            except OSError:
                return None, rejected
            self.store.touch(key.M, R)
            includes = [p for p, _ in inputs]
            checks = ([(key.source, key.source_sha)] + inputs, content['roots'], content['shadow'])
            return (output, includes, R, checks), rejected
        return None, rejected

    def _current(self, content):
        """Whether every include still has its recorded content and every shadow set is as it was."""
        if content.get('schema') != SCHEMA:
            return False
        after = time.time() - FRESH
        for tagged, sha in content['includes']:
            try:
                if self.hashes.get(self.untag(tagged), after)[1] != sha:
                    return False
            except OSError:
                return False
        return self._shadow(content['roots'], list(content['shadow']), after)[0] == content['shadow']

    def _shadow(self, roots, basenames, after):
        """({basename: sorted tagged paths} over the roots, the newest folder time in seconds)."""
        found = dict((b, set()) for b in basenames)
        latest = 0.0
        for root in roots:
            names, folders = self.folders.view(self.untag(root), after)
            for b in basenames:
                found[b].update(names.get(b, ()))
            latest = max([latest] + [newest(t) for t in folders.values() if t is not None])
        return dict((b, sorted(self.tag(p) for p in paths)) for b, paths in found.items()), latest

    def capture(self, key, start, includes):
        """Right after a compile: [(path, times, sha256)] of the source and each include, read
        now, or (None, reason) when one is missing or changed since 2 s before start."""
        after = time.time()
        quiet = start - SLACK
        captured = []
        for path in [key.source] + list(includes):
            what = 'the source' if not captured else path
            try:
                times, sha = self.hashes.get(self.norm(path), after, hashed_after=start)  # never a lookup's hash
            except Changed:
                return None, '%s changed while it was hashed' % what
            except OSError as error:
                return None, '%s is unreadable (%s)' % (what, error)
            if newest(times) >= quiet:
                return None, '%s changed during the compile' % what
            captured.append((path, times, sha))
        if captured[0][2] != key.source_sha:
            return None, 'the source changed during the compile'
        return captured, None

    def recheck(self, inputs):
        """Before a link: problems with restored objects whose inputs or shadow sets (per object,
        as restore() returned them) changed during the build, read afresh now."""
        after = time.time()
        problems = []
        for listed, roots, shadow in inputs:
            source = listed[0][0]
            for path, sha in listed:
                try:
                    now = self.hashes.get(self.norm(path), after)[1]
                except OSError as error:
                    problems.append('%s: %s is unreadable (%s)' % (source, path, error))
                    break
                if now != sha:
                    problems.append('%s: %s changed after its object was restored' % (source, path))
                    break
            else:
                try:
                    moved = self._shadow(roots, list(shadow), after)[0] != shadow
                except OSError as error:
                    moved, error_text = True, ' (%s)' % error
                else:
                    error_text = ''
                if moved:
                    problems.append('%s: a header named like one of its includes appeared or went '
                                    'after its object was restored%s' % (source, error_text))
        return problems

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
        if c.captured is None:
            return 'uncacheable', c.reason or 'no inputs captured'
        if self.root_re.search(c.output.decode('mbcs', 'replace')):
            return 'uncacheable', 'the output names the checkout'
        try:
            for path, times, sha in c.captured:
                if self.hashes.get(self.norm(path), after) != (times, sha):
                    return 'uncacheable', '%s changed after the compile' % path
            roots = set([self.top(self.tag(c.key.source))])  # None: outside the checkout or at its root
            if self.include_root:
                roots.add(self.tag(self.include_root))
            listed = []
            for path, _, sha in c.captured[1:]:
                tagged = self.tag(path)
                if tagged.startswith('r:'):
                    roots.add(self.top(tagged))
                listed.append([tagged, sha])
            if None in roots:
                return 'uncacheable', 'the source or an include outside a folder of the checkout'
            basenames = sorted(set(os.path.basename(p).lower() for p in c.includes))
            shadow, latest = self._shadow(sorted(roots), basenames, after)
            if latest >= c.start - SLACK:
                return 'uncacheable', 'a watched folder changed during the compile'
            with open(c.obj_path, 'rb') as f:
                obj = f.read()
            if digest(obj) != c.obj_sha:
                return 'uncacheable', 'the object changed after the compile'
            content = {'schema': SCHEMA, 'includes': listed, 'roots': sorted(roots), 'shadow': shadow}
            R, state = self.store.publish(c.key.M, content, {OBJ: obj}, c.output, replace=self.mode == 'write')
            return state, R
        except Changed as error:
            return 'uncacheable', str(error)
        except OSError as error:
            return 'uncacheable', 'not published (%s)' % error

    def finish(self):
        """After a run that published: deletes what has been in bad\\ over an hour, then evicts
        when the cache is over its cap. Returns a line to print, or None."""
        if not self.published:
            return None
        notes = []
        try:
            pruned, freed = self.store.prune_bad()
            if pruned:
                notes.append('bad\\: %d item(s) over an hour old removed (%.2f GiB)' % (pruned, freed / float(GIB)))
            result = self.store.evict(self.cap) if self.store.walk_due(self.cap) else None
        except OSError as error:
            notes.append('failed (%s)' % error)
            result = None
        if result and result[0]:
            removed, before, after = result
            notes.insert(0, '%d entr%s removed, %.2f GiB -> %.2f GiB (KEO_CACHE_MAX_GB %.2f)'
                         % (removed, 'y' if removed == 1 else 'ies', before / float(GIB),
                            after / float(GIB), self.cap / float(GIB)))
        return 'object cache eviction: ' + '; '.join(notes) if notes else None

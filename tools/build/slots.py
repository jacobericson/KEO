"""Host-wide build slots: a cap on how many build and test processes run at once on this machine,
shared by every checkout and every caller.

Two pools of lock files live in one directory, KEO_SLOTS_DIR (absolute; default
%LOCALAPPDATA%\\KEO\\slots, or a temp-directory fallback that says so once when LOCALAPPDATA is
unset):

    cpu     one token per compiler, linker, test executable or test shard process;
            capacity KEO_CPU_SLOTS (default os.cpu_count())
    heavy   one slot per build entry point or gate phase; capacity KEO_HEAVY_SLOTS (default 2)

A capacity setting may lower its pool's default but not raise it: a larger value is clamped to the
default, and the first use in a process writes "slots: KEO_HEAVY_SLOTS=4 clamped to 2
(KEO_SLOTS_RAISE=1 to raise)" to stderr (the same for KEO_CPU_SLOTS). KEO_SLOTS_RAISE=1 honors it.

Slot k of pool p is the file "<p>-<k>.lock". Holding a slot is holding a lock on its first byte
(msvcrt.locking, non-blocking); the OS drops the lock when the holder exits or is killed, so no slot
is ever left stale. A process opens each lock file once and keeps it open until it exits, so the
directory cannot be cleaned while one runs. "<p>-<k>.owner" (JSON:
pid, start, label, cmd, cwd, worktree) is written beside it for messages only and is ignored
whenever its lock is free.

API (Python 3, standard library only):

    with heavy(label=None) as sid:      one heavy slot, waiting while the pool is full
    with cpu_token(label=None) as sid:  one cpu token; each thread of a process may hold its own
    child_env(base=None, leaf=True)     a copy of base (default os.environ) for a child process
    run(cmd, label=None, leaf=True, **kw)
                                        subprocess.run(cmd, env=child_env(kw.get('env'), leaf),
                                        **kw), under one cpu token when leaf
    cpu_held(), heavy_held()            whether this thread runs under a cpu token / heavy slot
    status()                            one dict per held slot: id, pool, slot, mine, owner
    SlotTimeout                         raised when a wait runs out

sid is the slot's id ("heavy-1", "cpu-7"), or None when the request took nothing.

Nesting:
- While this process holds a heavy slot, KEO_HEAVY_HELD (the held ids) is set in os.environ, and
  child_env() carries it (also into an explicit base), so a child takes no heavy slot of its own.
  The variable is set to '' (never deleted) when the last one is released; '' means not held.
  An inherited KEO_HEAVY_HELD makes heavy() here take nothing while a slot it names is held; when
  none is, the marker is stale, heavy() says so once and takes the pool normally.
- child_env(leaf=True) adds KEO_CPU_HELD=1: the child is a leaf of a cpu token and takes none.
  leaf=False is for a child that is a runner itself and takes its own tokens: it gets
  KEO_CPU_HELD only when it really runs under one (this thread holds a token, or this process
  runs under KEO_CPU_HELD=1), and run(leaf=False) takes no token. KEO_CPU_HELD accepts only 1.
- A runner that finds cpu_held() true runs under someone else's token, and its children take no
  tokens of their own: it must cap itself to one concurrent job.
- Within one thread both pools are reentrant: a nested request takes nothing. Each other thread
  of the process takes its own slot.
- The pools are ordered heavy before cpu: heavy() takes nothing under KEO_CPU_HELD or while the
  calling thread holds a cpu token, so a token holder never waits for a heavy slot.
- Never hold a heavy slot in a thread that waits on threads which take heavy slots themselves: a
  coordinator either holds heavy for the whole phase and its threads and children rely on the
  marker, or holds nothing and each phase thread takes its own.

Waiting: each attempt tries every slot of the pool once, then sleeps about 50 ms (jittered). A heavy
waiter writes "slots: waiting for a heavy slot (held: ...)" to stderr on its first failed attempt
and every KEO_HEAVY_NOTE seconds (default 60) after. A cpu waiter that has waited KEO_HEAVY_NOTE
seconds writes the cpu line, at most once per KEO_HEAVY_NOTE seconds for the whole process. A
wait gives up after KEO_HEAVY_WAIT or KEO_CPU_WAIT seconds (default 1800 each), writes
"slots: FAILED: no <pool> slot within <n> s (held: ...)" and raises SlotTimeout. Holders are listed
per owner process ("pid 1234 <labels> in <dir> since <time> x16").

KEO_SLOTS=off turns both pools off: every request takes nothing, and the first one in a process
says so on stderr. A platform without msvcrt behaves the same way. A malformed setting raises
ValueError.

Limits: no fairness between processes (a waiter can be overtaken); a capacity is the requester's
own (a process with a smaller KEO_CPU_SLOTS, or a larger one under KEO_SLOTS_RAISE=1, uses fewer
or more slot files); status() and the wait lines tell a free slot from a held one by taking it for
an instant; an inherited heavy marker counts as live while any slot it names is held, whoever
holds it now; the owner files are best effort, so a message can read "owner unknown".

CLI:
    python tools\\build\\slots.py status
    python tools\\build\\slots.py heavy [--label X] -- <command...>
        runs the command holding a heavy slot and exits with its exit code; when the wait runs
        out it writes the FAILED line and exits 75 (EX_TEMPFAIL) without running the command.
    python tools\\build\\slots.py cpu [--label X] -- <command...>
        the same with one cpu token, for a single leaf process such as a link; the command runs
        with KEO_CPU_HELD=1, as under run().
"""
import argparse
import contextlib
import json
import os
import random
import re
import subprocess
import sys
import tempfile
import threading
import time

try:
    import msvcrt
except ImportError:
    msvcrt = None

__all__ = ['heavy', 'cpu_token', 'child_env', 'run', 'cpu_held', 'heavy_held', 'status',
           'slots_dir', 'capacity', 'SlotTimeout', 'EXIT_TIMEOUT']

POLL_S = 0.05
EXIT_TIMEOUT = 75  # EX_TEMPFAIL
OWNER_TRIES = 5
LOCK_NAME_RE = re.compile(r'^(cpu|heavy)-(\d+)\.lock$')
HEAVY_ID_RE = re.compile(r'^heavy-(\d+)$')

_lock = threading.Lock()  # guards everything below and every lock-file call
_fds = {}                 # lock-file path -> fd, opened once per process and kept open
_held = {}                # lock-file path -> holding thread's ident, for slots this process holds
_heavy_ids = []           # this process's held heavy slot ids, in the order taken
_cpu_note = [None]        # monotonic time of this process's last cpu waiting line
_noted = set()            # one-time notes already written
_worktrees = {}
_tls = threading.local()  # per-thread hold depth of each pool


class SlotTimeout(RuntimeError):
    """No slot of the pool came free within its wait limit."""


def _say(line):
    stream = sys.stderr
    if stream is not None:
        stream.write(line + '\n')
        stream.flush()


def _say_once(key, line):
    with _lock:
        first = key not in _noted
        _noted.add(key)
    if first:
        _say(line)


def _env_number(name, default, cast, minimum):
    raw = os.environ.get(name, '').strip()
    if not raw:
        return default
    try:
        value = cast(raw)
    except ValueError:
        raise ValueError('slots: %s=%r is not a number' % (name, raw))
    if not value >= minimum:  # also refuses nan
        raise ValueError('slots: %s=%r is below %s' % (name, raw, minimum))
    return value


def slots_dir():
    """The slots directory: KEO_SLOTS_DIR (absolute), else %LOCALAPPDATA%\\KEO\\slots."""
    d = os.environ.get('KEO_SLOTS_DIR', '').strip()
    if d:
        if not os.path.isabs(d):
            raise ValueError('slots: KEO_SLOTS_DIR=%r is not an absolute path' % d)
        return os.path.normpath(d)
    base = os.environ.get('LOCALAPPDATA', '').strip()
    if base:
        return os.path.join(os.path.abspath(base), 'KEO', 'slots')
    d = os.path.join(tempfile.gettempdir(), 'KEO', 'slots')
    _say_once('tempdir', 'slots: LOCALAPPDATA is unset, so the pools live in %s and are shared '
                         'only by processes of this session that see the same temp directory' % d)
    return d


def _raise_allowed():
    value = os.environ.get('KEO_SLOTS_RAISE', '').strip()
    if value in ('', '0'):
        return False
    if value != '1':
        raise ValueError('slots: KEO_SLOTS_RAISE=%r (expected 1, 0 or unset)' % value)
    return True


def capacity(pool):
    """This process's capacity of pool ('cpu' or 'heavy'). A setting above the default is
    clamped to it, with one line per pool and process, unless KEO_SLOTS_RAISE=1."""
    if pool == 'cpu':
        name, default = 'KEO_CPU_SLOTS', os.cpu_count() or 1
    elif pool == 'heavy':
        name, default = 'KEO_HEAVY_SLOTS', 2
    else:
        raise ValueError('slots: unknown pool %r' % pool)
    raise_ok = _raise_allowed()
    value = _env_number(name, default, int, 1)
    if value <= default or raise_ok:
        return value
    _say_once(('clamp', pool), 'slots: %s=%d clamped to %d (KEO_SLOTS_RAISE=1 to raise)'
                               % (name, value, default))
    return default


def _off_reason():
    if msvcrt is None:
        return 'no msvcrt on this platform'
    mode = os.environ.get('KEO_SLOTS', '').strip().lower()
    if mode in ('', 'on'):
        return None
    if mode != 'off':
        raise ValueError('slots: KEO_SLOTS=%r (expected on or off)' % mode)
    return 'KEO_SLOTS=off'


def _pools_off():
    """True when the pools are off; says so once per process."""
    reason = _off_reason()
    if reason is None:
        return False
    _say_once('off', 'slots: %s, the host-wide slot pools are off' % reason)
    return True


def _cpu_marker():
    value = os.environ.get('KEO_CPU_HELD', '').strip()
    if not value:
        return False
    if value != '1':
        raise ValueError('slots: KEO_CPU_HELD=%r (expected 1, or unset)' % value)
    return True


def _depth(pool):
    return getattr(_tls, pool, 0)


def _lock_path(d, pool, k):
    return os.path.join(d, '%s-%d.lock' % (pool, k))


def _owner_path(lock_path):
    return lock_path[:-len('.lock')] + '.owner'


def _slot_id(lock_path):
    return os.path.basename(lock_path)[:-len('.lock')]


def _fd(path, create):
    """The process's fd on path, opened on first use; None when absent and not create.
    The caller holds _lock."""
    fd = _fds.get(path)
    if fd is None:
        try:
            fd = os.open(path, os.O_RDWR | (os.O_CREAT if create else 0), 0o666)
        except FileNotFoundError:
            if create:
                raise
            return None
        _fds[path] = fd
    return fd


def _try_lock(fd):
    """Locks byte 0 through fd; False when another handle holds it. The caller holds _lock."""
    os.lseek(fd, 0, os.SEEK_SET)  # msvcrt.locking locks from the current position
    try:
        msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
    except PermissionError:
        return False
    return True


def _unlock(path):
    """Unlocks byte 0 of path's fd. The caller holds _lock."""
    fd = _fds[path]
    os.lseek(fd, 0, os.SEEK_SET)
    try:
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
    except OSError:
        # Closing drops every lock of the handle, so the slot is free whatever went wrong.
        del _fds[path]
        os.close(fd)
        raise


def _try_take(d, pool, cap):
    """Locks the lowest free slot of pool; returns its lock path, or None. The caller holds _lock."""
    for k in range(cap):
        path = _lock_path(d, pool, k)
        if path in _held:  # held by a thread of this process: never touch its lock
            continue
        if _try_lock(_fd(path, True)):
            _held[path] = threading.get_ident()
            return path
    return None


def _probe_busy(path):
    """True when the slot at path is held, here or elsewhere. The caller holds _lock."""
    if path in _held:
        return True
    fd = _fd(path, False)
    if fd is None:
        return False
    if not _try_lock(fd):
        return True
    _unlock(path)
    return False


def _worktree(cwd):
    """The nearest directory at or above cwd that holds a .git entry, or ''."""
    hit = _worktrees.get(cwd)
    if hit is None:
        hit, d = '', cwd
        while d:
            if os.path.exists(os.path.join(d, '.git')):
                hit = d
                break
            parent = os.path.dirname(d)
            d = '' if parent == d else parent
        _worktrees[cwd] = hit
    return hit


def _put_owner(lock_path, text):
    """Replaces the owner file with text ('' blanks it). A reader holding the file open makes
    os.replace fail on Windows, so it is retried and then rewritten in place."""
    path = _owner_path(lock_path)
    tmp = '%s.%d.%d.tmp' % (path, os.getpid(), threading.get_ident())
    try:
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write(text)
        for attempt in range(OWNER_TRIES):
            try:
                os.replace(tmp, path)
                return
            except PermissionError:
                time.sleep(0.005)
    except OSError:
        pass
    try:
        os.remove(tmp)
    except OSError:
        pass
    try:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
    except OSError:
        pass


def _write_owner(lock_path, label):
    try:
        cwd = os.getcwd()
    except OSError:
        cwd = ''
    _put_owner(lock_path, json.dumps({
        'pid': os.getpid(), 'start': time.time(), 'label': label or '',
        'cmd': subprocess.list2cmdline(sys.argv)[:400], 'cwd': cwd,
        'worktree': _worktree(cwd) if cwd else ''}))


def _drop_owner(lock_path):
    """Removes the owner file, or blanks it when a reader keeps it from going."""
    path = _owner_path(lock_path)
    for attempt in range(OWNER_TRIES):
        try:
            os.remove(path)
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.005)
    _put_owner(lock_path, '')


def _read_owner(lock_path):
    try:
        with open(_owner_path(lock_path), 'r', encoding='utf-8') as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    return info if isinstance(info, dict) else None


def _start(owner):
    try:
        return float(owner.get('start') or 0)
    except (TypeError, ValueError):
        return 0.0


def _since(owner):
    try:
        return time.strftime('%H:%M:%S', time.localtime(_start(owner)))
    except (OverflowError, OSError, ValueError):
        return '?'


def _describe(owner):
    if not owner:
        return 'owner unknown'
    return 'pid %s %s in %s since %s' % (owner.get('pid', '?'), owner.get('label') or '(no label)',
                                         owner.get('worktree') or owner.get('cwd') or '?',
                                         _since(owner))


def _holders(d, pool, cap):
    """The pool's holders, one entry per owner process."""
    groups = []
    by_key = {}
    for k in range(cap):
        path = _lock_path(d, pool, k)
        with _lock:
            busy = _probe_busy(path)
        if not busy:
            continue
        owner = _read_owner(path)
        key = (owner.get('pid'), owner.get('worktree') or owner.get('cwd')) if owner else None
        if key not in by_key:
            by_key[key] = {'owner': owner, 'labels': [], 'count': 0}
            groups.append(by_key[key])
        g = by_key[key]
        g['count'] += 1
        if owner:
            if _start(owner) < _start(g['owner']):
                g['owner'] = owner
            label = owner.get('label') or '(no label)'
            if label not in g['labels']:
                g['labels'].append(label)
    parts = []
    for g in groups:
        owner = g['owner']
        if owner:
            labels = g['labels'][:3] + (['...'] if len(g['labels']) > 3 else [])
            text = 'pid %s %s in %s since %s' % (owner.get('pid', '?'), ','.join(labels),
                                                 owner.get('worktree') or owner.get('cwd') or '?',
                                                 _since(owner))
            if owner.get('pid') == os.getpid():
                text += ' (this process)'
        else:
            text = 'owner unknown'
        parts.append(text + (' x%d' % g['count'] if g['count'] > 1 else ''))
    return '; '.join(parts) or 'none'


def _set_heavy_marker():
    """Publishes this process's held heavy ids to children; '' when none. Never deletes the
    variable, which would race another thread copying os.environ. The caller holds _lock."""
    os.environ['KEO_HEAVY_HELD'] = ','.join(_heavy_ids)


def _inherited_heavy_live():
    """True when this process holds no heavy slot and its inherited KEO_HEAVY_HELD names a slot
    that is held; a marker naming only free slots is stale and says so once."""
    d = slots_dir()
    with _lock:
        # Read under _lock: _heavy_ids and the variable change together under it.
        if _heavy_ids:  # the variable is this process's own
            return False
        value = os.environ.get('KEO_HEAVY_HELD', '').strip()
        if not value:
            return False
        ids = value.split(',')
        if not all(HEAVY_ID_RE.match(i) for i in ids):
            raise ValueError('slots: KEO_HEAVY_HELD=%r is not a list of heavy slot ids' % value)
        live = any(_probe_busy(_lock_path(d, 'heavy', int(HEAVY_ID_RE.match(i).group(1))))
                   for i in ids)
    if not live:
        _say_once(('stale', value),
                  'slots: ignoring stale KEO_HEAVY_HELD=%s (that slot is free)' % value)
    return live


def _acquire(pool, label, wait_s, note_s):
    """Takes one slot of pool, waiting up to wait_s; returns its lock path."""
    d = slots_dir()
    cap = capacity(pool)
    os.makedirs(d, exist_ok=True)
    t0 = time.monotonic()
    deadline = t0 + wait_s
    next_note = t0
    noted = False
    while True:
        with _lock:
            path = _try_take(d, pool, cap)
            if path is not None and pool == 'heavy':
                _heavy_ids.append(_slot_id(path))
                _set_heavy_marker()
        if path is not None:
            break
        now = time.monotonic()
        if now >= deadline:
            line = 'slots: FAILED: no %s slot within %g s (held: %s)' % (
                pool, wait_s, _holders(d, pool, cap))
            _say(line)
            raise SlotTimeout(line)
        if pool == 'heavy':
            due = now >= next_note
            next_note = now + note_s if due else next_note
            noted = noted or due
        else:
            with _lock:
                last = _cpu_note[0]
                due = now - t0 >= note_s and (last is None or now - last >= note_s)
                if due:
                    _cpu_note[0] = now
        if due:
            _say('slots: waiting for a %s slot (held: %s)' % (pool, _holders(d, pool, cap)))
        time.sleep(min(POLL_S * random.uniform(0.5, 1.5), deadline - now))
    try:
        _write_owner(path, label)
        if noted:
            _say('slots: took %s after %.1f s' % (_slot_id(path), time.monotonic() - t0))
    except BaseException:
        _release(pool, path)
        raise
    return path


def _release(pool, path):
    # The owner file goes while the lock is still held, so it can never remove a successor's.
    _drop_owner(path)
    with _lock:
        del _held[path]
        if pool == 'heavy':
            _heavy_ids.remove(_slot_id(path))
            _set_heavy_marker()
        _unlock(path)


@contextlib.contextmanager
def _hold(pool, label, wait_s, note_s):
    path = _acquire(pool, label, wait_s, note_s)
    setattr(_tls, pool, _depth(pool) + 1)
    try:
        yield _slot_id(path)
    finally:
        setattr(_tls, pool, _depth(pool) - 1)
        _release(pool, path)


@contextlib.contextmanager
def heavy(label=None):
    """Holds one heavy slot for the block; yields its id, or None when nothing was taken."""
    if (_depth('heavy') or _depth('cpu') or _cpu_marker() or _pools_off()
            or _inherited_heavy_live()):
        yield None
        return
    wait_s = _env_number('KEO_HEAVY_WAIT', 1800.0, float, 0.0)
    note_s = _env_number('KEO_HEAVY_NOTE', 60.0, float, 0.1)
    with _hold('heavy', label, wait_s, note_s) as sid:
        yield sid


@contextlib.contextmanager
def cpu_token(label=None):
    """Holds one cpu token for the block; yields its id, or None when nothing was taken."""
    if _depth('cpu') or _cpu_marker() or _pools_off():
        yield None
        return
    wait_s = _env_number('KEO_CPU_WAIT', 1800.0, float, 0.0)
    note_s = _env_number('KEO_HEAVY_NOTE', 60.0, float, 0.1)
    with _hold('cpu', label, wait_s, note_s) as sid:
        yield sid


def cpu_held():
    """True when this thread holds a cpu token or this process runs under one (KEO_CPU_HELD=1)."""
    return bool(_depth('cpu')) or _cpu_marker()


def heavy_held():
    """True when this thread holds a heavy slot or this process runs under a parent's live one."""
    if _depth('heavy'):
        return True
    return _off_reason() is None and _inherited_heavy_live()


def child_env(base=None, leaf=True):
    """A copy of base (default os.environ) for a child process. It carries this process's heavy
    marker (held or inherited), and KEO_CPU_HELD=1 when leaf or when the child really runs under
    a cpu token (this thread holds one, or this process runs under one)."""
    env = dict(os.environ if base is None else base)
    with _lock:
        marker = ','.join(_heavy_ids) or os.environ.get('KEO_HEAVY_HELD', '').strip()
    if marker:
        env['KEO_HEAVY_HELD'] = marker
    if leaf or cpu_held():
        env['KEO_CPU_HELD'] = '1'
    return env


def run(cmd, label=None, leaf=True, **kw):
    """subprocess.run(cmd, **kw) with env=child_env(kw.get('env'), leaf); a leaf child runs
    under one cpu token, a runner child (leaf=False) takes its own."""
    kw['env'] = child_env(kw.get('env'), leaf)
    if not leaf:
        return subprocess.run(cmd, **kw)
    with cpu_token(label):
        return subprocess.run(cmd, **kw)


def status():
    """One dict per held slot file in the slots directory, heavy first:
    {'id', 'pool', 'slot', 'mine', 'owner'}; owner is the parsed .owner JSON or None."""
    if msvcrt is None:
        return []
    d = slots_dir()
    try:
        names = os.listdir(d)
    except FileNotFoundError:
        return []
    found = []
    for name in names:
        m = LOCK_NAME_RE.match(name)
        if m:
            found.append((m.group(1) != 'heavy', int(m.group(2)), m.group(1)))
    out = []
    for _, k, pool in sorted(found):
        path = _lock_path(d, pool, k)
        with _lock:
            mine = path in _held
            busy = _probe_busy(path)
        if busy:
            out.append({'id': '%s-%d' % (pool, k), 'pool': pool, 'slot': k, 'mine': mine,
                        'owner': _read_owner(path)})
    return out


def _forget_files():
    """Closes every cached lock-file fd this process does not hold (so its directory can go)."""
    with _lock:
        for path in [p for p in _fds if p not in _held]:
            os.close(_fds.pop(path))


def _signed32(rc):
    rc &= 0xFFFFFFFF
    return rc - 0x100000000 if rc > 0x7FFFFFFF else rc


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    cmd = []
    if '--' in argv:
        i = argv.index('--')
        argv, cmd = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(prog='slots.py', description='Host-wide build slots.')
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('status', help='list the held slots')
    run_heavy = sub.add_parser(
        'heavy', help='run a command holding a heavy slot',
        description='Runs the command after -- holding a heavy slot and exits with its exit '
                    'code. When KEO_HEAVY_WAIT runs out first, writes the FAILED line and exits '
                    '%d (EX_TEMPFAIL) without running it.' % EXIT_TIMEOUT)
    run_heavy.add_argument('--label')
    run_cpu = sub.add_parser(
        'cpu', help='run a command holding one cpu token',
        description='Runs the command after -- holding one cpu token (the command gets '
                    'KEO_CPU_HELD=1) and exits with its exit code. When KEO_CPU_WAIT runs out '
                    'first, writes the FAILED line and exits %d (EX_TEMPFAIL) without running it.'
                    % EXIT_TIMEOUT)
    run_cpu.add_argument('--label')
    args = parser.parse_args(argv)
    if args.action == 'status':
        held = status()
        counts = dict((p, sum(1 for s in held if s['pool'] == p)) for p in ('heavy', 'cpu'))
        print('slots: %s: heavy %d/%d held, cpu %d/%d held' % (
            slots_dir(), counts['heavy'], capacity('heavy'), counts['cpu'], capacity('cpu')))
        for s in held:
            owner = s['owner'] or {}
            print('%s: %s%s' % (s['id'], _describe(s['owner']),
                                (' cmd: %s' % owner['cmd']) if owner.get('cmd') else ''))
        return 0
    if not cmd:
        parser.error('%s needs a command after --' % args.action)
    label = args.label or os.path.basename(cmd[0])
    try:
        if args.action == 'cpu':
            rc = run(cmd, label).returncode
        else:
            with heavy(label):
                rc = subprocess.call(cmd)
    except SlotTimeout:
        return EXIT_TIMEOUT
    return _signed32(rc)


if __name__ == '__main__':
    sys.exit(main())

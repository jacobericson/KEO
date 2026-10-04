"""Host-wide build slots: a cap on how many build and test processes run at once on this machine,
shared by every checkout and every caller.

Two pools of lock files live in one directory, KEO_SLOTS_DIR (default %LOCALAPPDATA%\\KEO\\slots):

    cpu     one token per compiler, linker, test executable or test shard process;
            capacity KEO_CPU_SLOTS (default os.cpu_count())
    heavy   one slot per build entry point or gate phase; capacity KEO_HEAVY_SLOTS (default 2)

Slot k of pool p is the file "<p>-<k>.lock". Holding a slot is holding a lock on its first byte
(msvcrt.locking, non-blocking); the OS drops the lock when the holder exits or is killed, so no slot
is ever left stale. "<p>-<k>.owner" (JSON: pid, start, label, cmd, cwd, worktree) is written beside
it for messages only and is ignored whenever its lock is free.

API (Python 3, standard library only):

    with heavy(label=None) as sid:      one heavy slot, waiting while the pool is full
    with cpu_token(label=None) as sid:  one cpu token; each thread of a process may hold its own
    child_env(base=None)                a copy of base (default os.environ) with KEO_CPU_HELD=1
    run(cmd, label=None, **kw)          subprocess.run(cmd, **kw) under one cpu token, with
                                        env=child_env(kw.get('env'))
    status()                            one dict per held slot: id, pool, slot, mine, owner
    SlotTimeout                         raised when a wait runs out

sid is the slot's id ("heavy-1", "cpu-7"), or None when the request took nothing.

Nesting:
- While this process holds a heavy slot, KEO_HEAVY_HELD (the held ids) is set in os.environ, so a
  child started with the inherited environment takes no heavy slot. It is removed when the
  process's last heavy slot is released. A marker the process inherited makes every heavy() here
  take nothing.
- A child started with child_env() (run() does this) has KEO_CPU_HELD=1 and takes no cpu token.
- Within one thread both pools are reentrant: a nested request takes nothing. Each other thread of
  the process takes its own slot.
- The pools are ordered heavy before cpu: heavy() takes nothing under KEO_CPU_HELD or while the
  calling thread holds a cpu token, so a token holder never waits for a heavy slot.

Waiting: each attempt tries every slot of the pool once, then sleeps about 50 ms (jittered). A heavy
waiter writes "slots: waiting for a heavy slot (held: ...)" to stderr on its first failed attempt
and every KEO_HEAVY_NOTE seconds (default 60) after; a cpu wait is silent. A wait gives up after
KEO_HEAVY_WAIT or KEO_CPU_WAIT seconds (default 1800 each), writes
"slots: FAILED: no <pool> slot within <n> s (held: ...)" and raises SlotTimeout.

KEO_SLOTS=off turns both pools off: every request takes nothing, and the first one in a process
says so on stderr. A platform without msvcrt behaves the same way.

Limits: no fairness between processes (a waiter can be overtaken); the KEO_*_HELD markers are
trusted, not checked; a capacity is the requester's own (a process with a larger KEO_CPU_SLOTS
uses more slot files); status() tells a free slot from a held one by taking it for an instant.

CLI:
    python tools\\build\\slots.py status
    python tools\\build\\slots.py heavy [--label X] -- <command...>
        runs the command holding a heavy slot and exits with its exit code; when the wait runs
        out it writes the FAILED line and exits 3 without running the command.
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

__all__ = ['heavy', 'cpu_token', 'child_env', 'run', 'status', 'slots_dir', 'capacity',
           'SlotTimeout']

POLL_S = 0.05
EXIT_TIMEOUT = 3
LOCK_NAME_RE = re.compile(r'^(cpu|heavy)-(\d+)\.lock$')

_lock = threading.Lock()  # guards _held, _heavy_ids, KEO_HEAVY_HELD and every lock-file call
_held = {}                # lock-file path -> fd, for each slot this process holds
_heavy_ids = []           # this process's held heavy slot ids, in the order taken
_tls = threading.local()  # per-thread hold depth of each pool
_noted_off = [False]
_worktrees = {}


class SlotTimeout(RuntimeError):
    """No slot of the pool came free within its wait limit."""


def _say(line):
    stream = sys.stderr
    if stream is not None:
        stream.write(line + '\n')
        stream.flush()


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
    """The slots directory: KEO_SLOTS_DIR, else %LOCALAPPDATA%\\KEO\\slots."""
    d = os.environ.get('KEO_SLOTS_DIR', '').strip()
    if not d:
        d = os.path.join(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir(), 'KEO', 'slots')
    return os.path.abspath(d)


def capacity(pool):
    """This process's capacity of pool ('cpu' or 'heavy')."""
    if pool == 'cpu':
        return _env_number('KEO_CPU_SLOTS', os.cpu_count() or 1, int, 1)
    if pool == 'heavy':
        return _env_number('KEO_HEAVY_SLOTS', 2, int, 1)
    raise ValueError('slots: unknown pool %r' % pool)


def _pools_off():
    """True when the pools are off; says so once per process."""
    if msvcrt is None:
        reason = 'no msvcrt on this platform'
    else:
        mode = os.environ.get('KEO_SLOTS', '').strip().lower()
        if mode in ('', 'on'):
            return False
        if mode != 'off':
            raise ValueError('slots: KEO_SLOTS=%r (expected on or off)' % mode)
        reason = 'KEO_SLOTS=off'
    with _lock:
        first = not _noted_off[0]
        _noted_off[0] = True
    if first:
        _say('slots: %s, the host-wide slot pools are off' % reason)
    return True


def _depth(pool):
    return getattr(_tls, pool, 0)


def _lock_path(d, pool, k):
    return os.path.join(d, '%s-%d.lock' % (pool, k))


def _owner_path(lock_path):
    return lock_path[:-len('.lock')] + '.owner'


def _try_take(d, pool, cap):
    """Locks the lowest free slot of pool; returns its lock path, or None. The caller holds _lock."""
    for k in range(cap):
        path = _lock_path(d, pool, k)
        if path in _held:
            continue
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            os.close(fd)
            continue
        _held[path] = fd
        return path
    return None


def _probe_busy(path):
    """True when another handle holds the slot at path. The caller holds _lock."""
    if path in _held:
        return True
    try:
        fd = os.open(path, os.O_RDWR)
    except FileNotFoundError:
        return False
    try:
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            return True
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        return False
    finally:
        os.close(fd)


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


def _write_owner(lock_path, label):
    try:
        cwd = os.getcwd()
    except OSError:
        cwd = ''
    info = {'pid': os.getpid(), 'start': time.time(), 'label': label or '',
            'cmd': subprocess.list2cmdline(sys.argv)[:400], 'cwd': cwd,
            'worktree': _worktree(cwd) if cwd else ''}
    path = _owner_path(lock_path)
    tmp = '%s.%d.%d.tmp' % (path, os.getpid(), threading.get_ident())
    try:
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(info, f)
        os.replace(tmp, path)
    except OSError:
        # Messages only: a reader holding the file open makes the replace fail on Windows.
        try:
            os.remove(tmp)
        except OSError:
            pass


def _read_owner(lock_path):
    try:
        with open(_owner_path(lock_path), 'r', encoding='utf-8') as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    return info if isinstance(info, dict) else None


def _describe(owner):
    if not owner:
        return 'owner unknown'
    try:
        since = time.strftime('%H:%M:%S', time.localtime(float(owner.get('start', 0))))
    except (TypeError, ValueError, OverflowError, OSError):
        since = '?'
    return 'pid %s %s in %s since %s' % (owner.get('pid', '?'), owner.get('label') or '(no label)',
                                         owner.get('worktree') or owner.get('cwd') or '?', since)


def _holders(d, pool, cap):
    parts = []
    for k in range(cap):
        path = _lock_path(d, pool, k)
        with _lock:
            mine = path in _held
            busy = _probe_busy(path)
        if busy:
            parts.append(_describe(_read_owner(path)) + (' (this process)' if mine else ''))
    return '; '.join(parts) or 'none'


def _set_heavy_marker():
    """Publishes this process's held heavy ids to children. The caller holds _lock."""
    if _heavy_ids:
        os.environ['KEO_HEAVY_HELD'] = ','.join(_heavy_ids)
    else:
        os.environ.pop('KEO_HEAVY_HELD', None)


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
                _heavy_ids.append(os.path.basename(path)[:-len('.lock')])
                _set_heavy_marker()
        if path is not None:
            break
        now = time.monotonic()
        if now >= deadline:
            line = 'slots: FAILED: no %s slot within %g s (held: %s)' % (
                pool, wait_s, _holders(d, pool, cap))
            _say(line)
            raise SlotTimeout(line)
        if note_s is not None and now >= next_note:
            _say('slots: waiting for a %s slot (held: %s)' % (pool, _holders(d, pool, cap)))
            noted = True
            next_note = now + note_s
        time.sleep(min(POLL_S * random.uniform(0.5, 1.5), deadline - now))
    try:
        _write_owner(path, label)
        if noted:
            _say('slots: took %s after %.1f s' % (os.path.basename(path)[:-len('.lock')],
                                                   time.monotonic() - t0))
    except BaseException:
        _release(pool, path)
        raise
    return path


def _release(pool, path):
    # The owner file goes while the lock is still held, so it can never remove a successor's.
    try:
        os.remove(_owner_path(path))
    except OSError:
        pass
    with _lock:
        fd = _held.pop(path)
        try:
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        except OSError:
            pass  # the close below drops the lock either way
        os.close(fd)
        if pool == 'heavy':
            _heavy_ids.remove(os.path.basename(path)[:-len('.lock')])
            _set_heavy_marker()


@contextlib.contextmanager
def _hold(pool, label, wait_s, note_s):
    path = _acquire(pool, label, wait_s, note_s)
    setattr(_tls, pool, _depth(pool) + 1)
    try:
        yield os.path.basename(path)[:-len('.lock')]
    finally:
        setattr(_tls, pool, _depth(pool) - 1)
        _release(pool, path)


def _heavy_bypassed():
    if _depth('heavy') or _depth('cpu') or os.environ.get('KEO_CPU_HELD'):
        return True
    with _lock:
        if os.environ.get('KEO_HEAVY_HELD') and not _heavy_ids:
            return True
    return _pools_off()


@contextlib.contextmanager
def heavy(label=None):
    """Holds one heavy slot for the block; yields its id, or None when nothing was taken."""
    if _heavy_bypassed():
        yield None
        return
    wait_s = _env_number('KEO_HEAVY_WAIT', 1800.0, float, 0.0)
    note_s = _env_number('KEO_HEAVY_NOTE', 60.0, float, 0.0)
    with _hold('heavy', label, wait_s, note_s) as sid:
        yield sid


@contextlib.contextmanager
def cpu_token(label=None):
    """Holds one cpu token for the block; yields its id, or None when nothing was taken."""
    if _depth('cpu') or os.environ.get('KEO_CPU_HELD') or _pools_off():
        yield None
        return
    wait_s = _env_number('KEO_CPU_WAIT', 1800.0, float, 0.0)
    with _hold('cpu', label, wait_s, None) as sid:
        yield sid


def child_env(base=None):
    """A copy of base (default os.environ) with KEO_CPU_HELD=1, for a child run under a token."""
    env = dict(os.environ if base is None else base)
    env['KEO_CPU_HELD'] = '1'
    return env


def run(cmd, label=None, **kw):
    """subprocess.run(cmd, **kw) under one cpu token, the child's environment from child_env."""
    kw['env'] = child_env(kw.get('env'))
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
    run_heavy = sub.add_parser('heavy', help='run a command holding a heavy slot')
    run_heavy.add_argument('--label')
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
        parser.error('heavy needs a command after --')
    try:
        with heavy(args.label or os.path.basename(cmd[0])):
            rc = subprocess.call(cmd)
    except SlotTimeout:
        return EXIT_TIMEOUT
    return _signed32(rc)


if __name__ == '__main__':
    sys.exit(main())

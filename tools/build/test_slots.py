"""Tests of tools\\build\\slots.py, the host-wide slot pools (unittest, standard library only).

Every test runs against its own temporary KEO_SLOTS_DIR with every KEO_* variable of the caller
removed first, so a run under a slot-holding parent tests the pools rather than their bypass.
Child processes report through stdout lines and block on release files, and threads meet at
barriers, so no assertion depends on how the scheduler orders them.
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

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slots  # noqa: E402

SLOTS_PY = os.path.join(HERE, 'slots.py')
PY = sys.executable
WAIT = 10.0  # upper bound on any expected event; a broken pool fails here instead of hanging

# Holds one slot of argv[2] until the file argv[3] exists; prints 'got <id> <pid>' inside it.
HOLDER = r'''
import os, sys, time
sys.path.insert(0, sys.argv[1])
import slots
take = slots.heavy if sys.argv[2] == 'heavy' else slots.cpu_token
with take('holder') as sid:
    print('got %s %d' % (sid, os.getpid()), flush=True)
    end = time.monotonic() + 30
    while not os.path.exists(sys.argv[3]) and time.monotonic() < end:
        time.sleep(0.01)
'''

# Takes one slot of argv[2] argv[3] times in a row, printing 'got <id>' each time; exit 3 on a
# wait timeout.
TAKE = r'''
import sys
sys.path.insert(0, sys.argv[1])
import slots
take = slots.heavy if sys.argv[2] == 'heavy' else slots.cpu_token
for _ in range(int(sys.argv[3])):
    try:
        with take('take') as sid:
            print('got %s' % sid, flush=True)
    except slots.SlotTimeout:
        sys.exit(3)
'''


class Child(object):
    """A child process whose stdout and stderr lines are collected as they arrive."""

    def __init__(self, args, env):
        self.p = subprocess.Popen([PY] + args, env=env, cwd=HERE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, universal_newlines=True)
        self.lines = {'out': [], 'err': []}
        self.cv = threading.Condition()
        self.pumps = [threading.Thread(target=self._pump, args=(stream, name))
                      for stream, name in ((self.p.stdout, 'out'), (self.p.stderr, 'err'))]
        for t in self.pumps:
            t.daemon = True
            t.start()

    def _pump(self, stream, name):
        for line in stream:
            with self.cv:
                self.lines[name].append(line.rstrip('\r\n'))
                self.cv.notify_all()

    def matching(self, name, prefix):
        with self.cv:
            return [line for line in self.lines[name] if line.startswith(prefix)]

    def wait_lines(self, name, prefix, count=1, timeout=WAIT):
        """The first line of stream name starting with prefix once count of them arrived, else None."""
        end = time.monotonic() + timeout
        with self.cv:
            while True:
                hits = [line for line in self.lines[name] if line.startswith(prefix)]
                if len(hits) >= count:
                    return hits[0]
                left = end - time.monotonic()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def finish(self, timeout=WAIT):
        rc = self.p.wait(timeout)
        for t in self.pumps:
            t.join(timeout)
        return rc

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        for t in self.pumps:
            t.join(WAIT)
        self.p.stdout.close()
        self.p.stderr.close()


class Stamped(object):
    """A stderr stand-in recording each write with its time."""

    def __init__(self):
        self.writes = []
        self.guard = threading.Lock()

    def write(self, text):
        with self.guard:
            self.writes.append((time.monotonic(), text))

    def flush(self):
        pass

    def lines(self, prefix):
        with self.guard:
            return [(t, s) for t, s in self.writes if s.startswith(prefix)]


def touch(path):
    open(path, 'w').close()


def pid_of(got_line):
    return int(got_line.split()[-1])


def run_threads(target, count):
    threads = [threading.Thread(target=target) for _ in range(count)]
    for t in threads:
        t.start()
    return threads


@unittest.skipUnless(slots.msvcrt, 'the slot pools need msvcrt (Windows)')
class SlotsTest(unittest.TestCase):

    def setUp(self):
        self.saved = dict((k, v) for k, v in os.environ.items() if k.startswith('KEO_'))
        for k in self.saved:
            del os.environ[k]
        self.tmp = tempfile.mkdtemp(prefix='keo_slots_test_')
        os.environ['KEO_SLOTS_DIR'] = self.tmp
        # Bounded waits, here and in every child, so a broken bypass fails instead of hanging.
        os.environ['KEO_HEAVY_WAIT'] = os.environ['KEO_CPU_WAIT'] = str(WAIT)
        slots._noted.clear()
        slots._cpu_note[0] = None
        self.children = []
        self.releases = 0

    def tearDown(self):
        for c in self.children:
            c.stop()
        for k in [k for k in os.environ if k.startswith('KEO_')]:
            del os.environ[k]
        os.environ.update(self.saved)
        held = dict(slots._held)
        slots._forget_files()
        shutil.rmtree(self.tmp, ignore_errors=True)
        self.assertEqual(held, {}, 'a slot was left held')
        self.assertEqual(slots._heavy_ids, [])

    def env(self, **extra):
        """os.environ without this process's markers, plus extra."""
        e = dict(os.environ)
        e.pop('KEO_HEAVY_HELD', None)
        e.pop('KEO_CPU_HELD', None)
        e.update((k, str(v)) for k, v in extra.items())
        return e

    def spawn(self, args, env):
        c = Child(args, env)
        self.children.append(c)
        return c

    def holder(self, pool, env):
        """Starts a child holding one slot of pool; returns (child, its 'got' line, release path)."""
        self.releases += 1
        release = os.path.join(self.tmp, 'release-%d' % self.releases)
        c = self.spawn(['-c', HOLDER, HERE, pool, release], env)
        got = c.wait_lines('out', 'got %s-' % pool)
        self.assertIsNotNone(got, 'holder took no %s slot: %r' % (pool, c.lines))
        return c, got, release

    def take(self, pool, env, times=1):
        c = self.spawn(['-c', TAKE, HERE, pool, str(times)], env)
        return c, c.finish()

    def test_third_heavy_process_waits_until_one_releases(self):
        e = self.env(KEO_HEAVY_SLOTS=2)
        a, got_a, release_a = self.holder('heavy', e)
        b, got_b, release_b = self.holder('heavy', e)
        self.assertEqual(sorted([got_a.split()[1], got_b.split()[1]]), ['heavy-0', 'heavy-1'])
        c = self.spawn(['-c', HOLDER, HERE, 'heavy', os.path.join(self.tmp, 'release-c')], e)
        note = c.wait_lines('err', 'slots: waiting for a heavy slot (held: ')
        self.assertIsNotNone(note, c.lines)
        self.assertIn('pid %d holder in ' % pid_of(got_a), note)
        self.assertIn('pid %d holder in ' % pid_of(got_b), note)
        self.assertIsNone(c.wait_lines('out', 'got', timeout=0.3), 'a third holder got in')
        touch(release_a)
        self.assertEqual(a.finish(), 0)
        self.assertEqual(c.wait_lines('out', 'got heavy-').split()[1], got_a.split()[1])
        touch(release_b)
        touch(os.path.join(self.tmp, 'release-c'))
        self.assertEqual(b.finish(), 0)
        self.assertEqual(c.finish(), 0)
        self.assertEqual(len(c.matching('err', 'slots: waiting for')), 1)
        self.assertEqual(len(c.matching('err', 'slots: took heavy-')), 1)

    def test_killed_holder_frees_its_slot(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        os.environ['KEO_HEAVY_WAIT'] = '0'
        _, got, _ = self.holder('heavy', self.env())
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(slots.SlotTimeout):
            with slots.heavy('before kill'):
                pass
        self.assertIn('slots: FAILED: no heavy slot within 0 s (held: pid %d holder' % pid_of(got),
                      err.getvalue())
        subprocess.run(['taskkill', '/F', '/PID', str(pid_of(got))], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
        t0 = time.monotonic()
        os.environ['KEO_HEAVY_WAIT'] = str(WAIT)
        with contextlib.redirect_stderr(io.StringIO()):
            with slots.heavy('after kill') as sid:
                freed = time.monotonic() - t0
        self.assertEqual(sid, 'heavy-0')
        self.assertLess(freed, 5.0)

    def test_heavy_marker_passes_to_children(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        with slots.heavy('outer') as sid:
            self.assertEqual(sid, 'heavy-0')
            self.assertEqual(os.environ.get('KEO_HEAVY_HELD'), 'heavy-0')
            self.assertTrue(slots.heavy_held())
            self.assertEqual(slots.child_env({'A': 'b'}, leaf=False),
                             {'A': 'b', 'KEO_HEAVY_HELD': 'heavy-0'})
            c, rc = self.take('heavy', dict(os.environ, KEO_HEAVY_WAIT='0'))
            self.assertEqual((rc, c.lines['out']), (0, ['got None']))
            c, rc = self.take('heavy', slots.child_env(self.env(KEO_HEAVY_WAIT=0), leaf=False))
            self.assertEqual((rc, c.lines['out']), (0, ['got None']))
            c, rc = self.take('heavy', self.env(KEO_HEAVY_WAIT=0))
            self.assertEqual(rc, 3)
            self.assertEqual(len(c.matching('err', 'slots: FAILED: no heavy slot within 0 s (held: '
                                            'pid %d outer' % os.getpid())), 1)
        self.assertFalse(os.environ.get('KEO_HEAVY_HELD'))
        self.assertFalse(slots.heavy_held())

    def test_inherited_heavy_marker_is_trusted_only_while_its_slot_is_held(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        _, got, release = self.holder('heavy', self.env())
        os.environ['KEO_HEAVY_HELD'] = got.split()[1]
        self.assertTrue(slots.heavy_held())
        with slots.heavy('under parent') as sid:
            self.assertIsNone(sid)
        self.assertEqual(slots.child_env({}, leaf=False), {'KEO_HEAVY_HELD': 'heavy-0'})
        touch(release)
        self.assertEqual(self.children[0].finish(), 0)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertFalse(slots.heavy_held())
            with slots.heavy('after parent') as sid:
                self.assertEqual(sid, 'heavy-0')
                self.assertEqual(os.environ['KEO_HEAVY_HELD'], 'heavy-0')
        self.assertEqual(err.getvalue().splitlines(),
                         ['slots: ignoring stale KEO_HEAVY_HELD=heavy-0 (that slot is free)'])
        self.assertEqual(os.environ['KEO_HEAVY_HELD'], '')

    def test_cpu_marker_takes_nothing(self):
        os.environ['KEO_CPU_HELD'] = '1'
        self.assertTrue(slots.cpu_held())
        with slots.cpu_token('c') as cid, slots.heavy('h') as hid:
            self.assertEqual((cid, hid), (None, None))
        self.assertEqual(slots.child_env({}, leaf=False), {'KEO_CPU_HELD': '1'})
        del os.environ['KEO_CPU_HELD']
        self.assertFalse(slots.cpu_held())
        with slots.heavy('h') as hid, slots.cpu_token('c') as cid:
            self.assertEqual((hid, cid), ('heavy-0', 'cpu-0'))

    def test_cpu_marker_passes_to_children(self):
        os.environ['KEO_CPU_SLOTS'] = '1'
        with slots.cpu_token('outer') as sid:
            self.assertEqual(sid, 'cpu-0')
            self.assertTrue(slots.cpu_held())
            self.assertNotIn('KEO_CPU_HELD', os.environ)
            with slots.cpu_token('nested') as inner, slots.heavy('under cpu') as hid:
                self.assertEqual((inner, hid), (None, None))
            self.assertEqual(slots.child_env({}, leaf=False), {'KEO_CPU_HELD': '1'})
            c, rc = self.take('cpu', slots.child_env(self.env(KEO_CPU_WAIT=0)))
            self.assertEqual((rc, c.lines['out']), (0, ['got None']))
            c, rc = self.take('cpu', self.env(KEO_CPU_WAIT=0))
            self.assertEqual(rc, 3)
            self.assertEqual(len(c.matching('err', 'slots: FAILED: no cpu slot within 0 s')), 1)
        self.assertFalse(slots.cpu_held())

    def test_run_leaf_and_runner_children(self):
        os.environ['KEO_CPU_SLOTS'] = '1'
        code = ('import os, sys; sys.path.insert(0, sys.argv[1]); import slots; '
                'print(os.environ.get("KEO_CPU_HELD")); '
                'print(" ".join(s["id"] for s in slots.status() if not s["mine"]) or "-")')
        args = [PY, '-c', code, HERE]
        r = slots.run(args, env=self.env(), stdout=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(r.stdout.split(), ['1', 'cpu-0'])
        r = slots.run(args, leaf=False, env=self.env(), stdout=subprocess.PIPE,
                      universal_newlines=True)
        self.assertEqual(r.stdout.split(), ['None', '-'])
        self.assertEqual(slots.status(), [])
        base = {'A': 'b'}
        self.assertEqual(slots.child_env(base), {'A': 'b', 'KEO_CPU_HELD': '1'})
        self.assertEqual(slots.child_env(base, leaf=False), {'A': 'b'})
        self.assertEqual(base, {'A': 'b'})

    def test_slots_off_takes_nothing_and_says_so_once(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        os.environ['KEO_CPU_SLOTS'] = '1'
        with slots.heavy('h') as hid, slots.cpu_token('c') as cid:
            self.assertEqual((hid, cid), ('heavy-0', 'cpu-0'))
            e = self.env(KEO_SLOTS='off', KEO_HEAVY_WAIT=0, KEO_CPU_WAIT=0)
            for pool in ('heavy', 'cpu'):
                c, rc = self.take(pool, e, times=2)
                self.assertEqual((rc, c.lines['out']), (0, ['got None', 'got None']))
                self.assertEqual(c.lines['err'],
                                 ['slots: KEO_SLOTS=off, the host-wide slot pools are off'])

    def _fill_cpu(self, holders):
        """Holds `holders` cpu tokens from as many threads, then checks one more is refused."""
        ready = threading.Barrier(holders + 1, timeout=WAIT)
        release = threading.Event()
        ids, errors = [], []

        def hold():
            try:
                with slots.cpu_token('fill') as sid:
                    ids.append(sid)
                    ready.wait()
                    release.wait(WAIT)
            except BaseException as ex:  # reported below; a thread cannot fail the test itself
                errors.append(repr(ex))

        threads = run_threads(hold, holders)
        try:
            ready.wait()
            os.environ['KEO_CPU_WAIT'] = '0'
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(slots.SlotTimeout):
                with slots.cpu_token('one more'):
                    pass
        finally:
            os.environ['KEO_CPU_WAIT'] = str(WAIT)
            release.set()
            for t in threads:
                t.join(WAIT)
        self.assertEqual(errors, [])
        self.assertEqual(len(set(ids)), holders)

    def _hammer_cpu(self, threads, rounds):
        """Returns the most tokens held at once while threads x rounds holds ran."""
        start = threading.Barrier(threads, timeout=WAIT)
        state = {'now': 0, 'max': 0, 'done': 0, 'ids': set()}
        guard = threading.Lock()
        errors = []

        def work():
            try:
                start.wait()
                for _ in range(rounds):
                    with slots.cpu_token('thread') as sid:
                        with guard:
                            if sid is None or sid in state['ids']:
                                errors.append('token %r taken twice or not at all' % sid)
                            state['ids'].add(sid)
                            state['now'] += 1
                            state['max'] = max(state['max'], state['now'])
                        time.sleep(0.02)
                        with guard:
                            state['ids'].discard(sid)
                            state['now'] -= 1
                            state['done'] += 1
            except BaseException as ex:
                errors.append(repr(ex))

        for t in run_threads(work, threads):
            t.join(WAIT * 2)
        self.assertEqual(errors, [])
        self.assertEqual(state['done'], threads * rounds)
        return state['max']

    def test_cpu_tokens_from_many_threads_never_exceed_capacity(self):
        os.environ['KEO_CPU_SLOTS'] = '3'
        self._fill_cpu(3)
        self.assertLessEqual(self._hammer_cpu(10, 3), 3)
        _, _, release = self.holder('cpu', self.env())
        self._fill_cpu(2)
        self.assertLessEqual(self._hammer_cpu(10, 3), 2)
        touch(release)

    def test_threads_take_their_own_heavy_slots(self):
        os.environ['KEO_HEAVY_SLOTS'] = '2'
        ready = threading.Barrier(3, timeout=WAIT)
        release = threading.Event()
        got, errors = [], []

        def hold():
            try:
                with slots.heavy('thread') as sid:
                    with slots.heavy('nested') as inner:
                        got.append((sid, inner))
                    ready.wait()
                    release.wait(WAIT)
            except BaseException as ex:
                errors.append(repr(ex))

        threads = run_threads(hold, 2)
        try:
            ready.wait()
            self.assertEqual(sorted(got), [('heavy-0', None), ('heavy-1', None)])
            self.assertEqual(sorted(os.environ['KEO_HEAVY_HELD'].split(',')),
                             ['heavy-0', 'heavy-1'])
            os.environ['KEO_HEAVY_WAIT'] = '0.2'
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(slots.SlotTimeout):
                with slots.heavy('third'):
                    pass
            self.assertIn('slots: FAILED: no heavy slot within 0.2 s (held: pid %d thread in '
                          % os.getpid(), err.getvalue())
            self.assertIn('(this process) x2)', err.getvalue())
        finally:
            release.set()
            for t in threads:
                t.join(WAIT)
        self.assertEqual(errors, [])
        self.assertFalse(os.environ.get('KEO_HEAVY_HELD'))

    def test_environ_copies_never_race_the_marker(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        stop = time.monotonic() + 1.5
        errors, counts = [], {'churn': 0, 'copy': 0}

        def churn():
            try:
                while time.monotonic() < stop:
                    with slots.heavy('churn'):
                        pass
                    counts['churn'] += 1
            except BaseException as ex:
                errors.append(repr(ex))

        def copy():
            try:
                while time.monotonic() < stop:
                    slots.child_env()
                    dict(os.environ)
                    os.environ.copy()
                    counts['copy'] += 1
            except BaseException as ex:
                errors.append(repr(ex))

        threads = run_threads(churn, 1) + run_threads(copy, 3)
        for t in threads:
            t.join(WAIT)
        self.assertEqual(errors, [])
        self.assertGreater(counts['churn'], 50)
        self.assertGreater(counts['copy'], 50)

    def test_wait_line_once_then_every_interval(self):
        e = self.env(KEO_HEAVY_SLOTS=1)
        _, _, release = self.holder('heavy', e)
        cmd = ['--', PY, '-c', 'pass']
        slow = self.spawn([SLOTS_PY, 'heavy', '--label', 'slow'] + cmd, e)
        fast = self.spawn([SLOTS_PY, 'heavy', '--label', 'fast'] + cmd,
                          dict(e, KEO_HEAVY_NOTE='0.2'))
        self.assertIsNotNone(fast.wait_lines('err', 'slots: waiting for a heavy slot', count=3),
                             fast.lines)
        self.assertIsNotNone(slow.wait_lines('err', 'slots: waiting for a heavy slot'))
        touch(release)
        self.assertEqual((slow.finish(), fast.finish()), (0, 0))
        self.assertEqual(len(slow.matching('err', 'slots: waiting for')), 1)

    def test_cpu_wait_line_is_once_per_process_and_interval(self):
        os.environ['KEO_CPU_SLOTS'] = '1'
        os.environ['KEO_HEAVY_NOTE'] = '0.25'
        _, got, release = self.holder('cpu', self.env())
        err = Stamped()
        errors = []

        def wait():
            try:
                with slots.cpu_token('waiter'):
                    pass
            except BaseException as ex:
                errors.append(repr(ex))

        with contextlib.redirect_stderr(err):
            t0 = time.monotonic()
            threads = run_threads(wait, 4)
            end = t0 + WAIT
            while len(err.lines('slots: waiting for a cpu slot')) < 3 and time.monotonic() < end:
                time.sleep(0.02)
            touch(release)
            for t in threads:
                t.join(WAIT)
        self.assertEqual(errors, [])
        lines = err.lines('slots: waiting for a cpu slot')
        self.assertGreaterEqual(len(lines), 3)
        stamps = [t for t, _ in lines]
        self.assertGreaterEqual(stamps[0] - t0, 0.25)
        for a, b in zip(stamps, stamps[1:]):  # one line per thread would come in a burst
            self.assertGreaterEqual(b - a, 0.1)
        self.assertIn('(held: pid %d holder in ' % pid_of(got), lines[0][1])
        self.assertEqual(err.lines('slots: took'), [])

    def test_heavy_wait_timeout_is_named_and_skips_the_command(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        marker = os.path.join(self.tmp, 'ran')
        with slots.heavy('holder'):
            c = self.spawn([SLOTS_PY, 'heavy', '--label', 'late', '--', PY, '-c',
                            'open(%r, "w").close()' % marker], self.env(KEO_HEAVY_WAIT='0.3'))
            rc = c.finish()
        self.assertEqual((rc, slots.EXIT_TIMEOUT), (75, 75))
        self.assertEqual(len(c.matching('err', 'slots: FAILED: no heavy slot within 0.3 s (held: '
                                        'pid %d holder in ' % os.getpid())), 1)
        self.assertFalse(os.path.exists(marker))

    def test_stale_owner_does_not_block(self):
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        os.environ['KEO_HEAVY_WAIT'] = '0'
        dead = subprocess.Popen([PY, '-c', 'pass'])
        dead.wait()
        owner = os.path.join(self.tmp, 'heavy-0.owner')
        touch(os.path.join(self.tmp, 'heavy-0.lock'))
        with open(owner, 'w') as f:
            json.dump({'pid': dead.pid, 'start': 0, 'label': 'crashed', 'cwd': 'C:\\gone'}, f)
        self.assertEqual(slots.status(), [])
        with slots.heavy('fresh') as sid:
            self.assertEqual(sid, 'heavy-0')
            [s] = slots.status()
            self.assertTrue(s['mine'])
            self.assertEqual((s['owner']['pid'], s['owner']['label']), (os.getpid(), 'fresh'))
        self.assertFalse(os.path.exists(owner))

    def test_status_lists_slots_above_this_capacity(self):
        e = self.env(KEO_HEAVY_SLOTS=3)
        held = [self.holder('heavy', e) for _ in range(3)]
        self.assertEqual([got.split()[1] for _, got, _ in held], ['heavy-0', 'heavy-1', 'heavy-2'])
        for child, _, release in held[:2]:
            touch(release)
            self.assertEqual(child.finish(), 0)
        os.environ['KEO_HEAVY_SLOTS'] = '1'
        [s] = slots.status()
        self.assertEqual((s['id'], s['mine'], s['owner']['pid']),
                         ('heavy-2', False, pid_of(held[2][1])))

    def test_a_held_slot_is_never_tried_again_by_its_own_process(self):
        os.environ['KEO_CPU_SLOTS'] = '2'
        tried, real = [], slots._try_lock

        def spy(fd):
            tried.append(fd)
            return real(fd)

        slots._try_lock = spy
        try:
            with slots.cpu_token('first') as first:
                held_fd = slots._fds[os.path.join(self.tmp, first + '.lock')]
                del tried[:]
                second = []

                def other():
                    with slots.cpu_token('second') as sid:
                        second.append(sid)
                        slots.status()

                for t in run_threads(other, 1):
                    t.join(WAIT)
        finally:
            slots._try_lock = real
        self.assertEqual((first, second), ('cpu-0', ['cpu-1']))
        self.assertTrue(tried)
        self.assertNotIn(held_fd, tried)

    def test_deleting_a_held_lock_file_is_refused(self):
        with slots.heavy('h') as sid:
            with self.assertRaises(PermissionError):
                os.remove(os.path.join(self.tmp, sid + '.lock'))

    def test_cli_status_and_exit_codes(self):
        e = self.env(KEO_HEAVY_SLOTS=1)
        _, got, release = self.holder('heavy', e)
        s = subprocess.run([PY, SLOTS_PY, 'status'], env=e, stdout=subprocess.PIPE,
                           universal_newlines=True)
        self.assertEqual(s.returncode, 0)
        self.assertIn('heavy 1/1 held', s.stdout)
        self.assertIn('heavy-0: pid %d holder in ' % pid_of(got), s.stdout)
        touch(release)
        check = 'import os, sys; sys.exit(7 if os.environ.get("KEO_HEAVY_HELD") == "heavy-0" else 1)'
        r = subprocess.run([PY, SLOTS_PY, 'heavy', '--label', 'x', '--', PY, '-c', check], env=e)
        self.assertEqual(r.returncode, 7)
        r = subprocess.run([PY, SLOTS_PY, 'heavy', '--', PY, '-c',
                            'import sys; sys.exit(-1073741819)'], env=e)
        self.assertEqual(r.returncode, 0xC0000005)
        h = subprocess.run([PY, SLOTS_PY, 'heavy', '--help'], stdout=subprocess.PIPE,
                           universal_newlines=True)
        self.assertIn('exits 75 (EX_TEMPFAIL)', ' '.join(h.stdout.split()))

    def test_cli_cpu_runs_a_leaf_under_one_token(self):
        e = self.env(KEO_CPU_SLOTS=1)
        check = 'import os, sys; sys.exit(7 if os.environ.get("KEO_CPU_HELD") == "1" else 1)'
        r = subprocess.run([PY, SLOTS_PY, 'cpu', '--label', 'x', '--', PY, '-c', check], env=e)
        self.assertEqual(r.returncode, 7)
        os.environ['KEO_CPU_SLOTS'] = '1'
        marker = os.path.join(self.tmp, 'ran')
        with slots.cpu_token('holder'):
            c = self.spawn([SLOTS_PY, 'cpu', '--label', 'late', '--', PY, '-c',
                            'open(%r, "w").close()' % marker], self.env(KEO_CPU_SLOTS=1, KEO_CPU_WAIT='0.3'))
            rc = c.finish()
        self.assertEqual(rc, slots.EXIT_TIMEOUT)
        self.assertEqual(len(c.matching('err', 'slots: FAILED: no cpu slot within 0.3 s (held: '
                                        'pid %d holder in ' % os.getpid())), 1)
        self.assertFalse(os.path.exists(marker))
        h = subprocess.run([PY, SLOTS_PY, 'cpu', '--help'], stdout=subprocess.PIPE,
                           universal_newlines=True)
        self.assertIn('exits 75 (EX_TEMPFAIL)', ' '.join(h.stdout.split()))

    def test_temp_fallback_says_so(self):
        e = self.env(TMP=self.tmp, TEMP=self.tmp)
        for name in ('KEO_SLOTS_DIR', 'LOCALAPPDATA'):
            e.pop(name, None)
        s = subprocess.run([PY, SLOTS_PY, 'status'], env=e, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True)
        fallback = os.path.join(self.tmp, 'KEO', 'slots')
        self.assertEqual(s.returncode, 0)
        self.assertEqual(s.stderr.splitlines(),
                         ['slots: LOCALAPPDATA is unset, so the pools live in %s and are shared '
                          'only by processes of this session that see the same temp directory'
                          % fallback])
        self.assertTrue(s.stdout.startswith('slots: %s: ' % fallback))

    def test_bad_settings_are_refused(self):
        cwd = os.getcwd()
        os.chdir(self.tmp)  # a relative directory accepted by mistake lands in the temp tree
        try:
            for name, value in (('KEO_HEAVY_SLOTS', '0'), ('KEO_CPU_SLOTS', 'many'),
                                ('KEO_HEAVY_WAIT', 'nan'), ('KEO_CPU_WAIT', '-1'),
                                ('KEO_HEAVY_NOTE', 'x'), ('KEO_SLOTS', 'maybe'),
                                ('KEO_CPU_HELD', 'yes'), ('KEO_HEAVY_HELD', 'bogus'),
                                ('KEO_SLOTS_DIR', 'relative\\slots')):
                saved = os.environ.get(name)
                os.environ[name] = value
                try:
                    with self.assertRaises(ValueError, msg=name):
                        with slots.heavy('bad'), slots.cpu_token('bad'):
                            pass
                finally:
                    if saved is None:
                        del os.environ[name]
                    else:
                        os.environ[name] = saved
        finally:
            os.chdir(cwd)


if __name__ == '__main__':
    unittest.main()

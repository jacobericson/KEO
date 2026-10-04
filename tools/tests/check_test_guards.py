"""Guard the ways the test run has silently reported success, or could.

1. A suite dropped from tools\\tests\\suites.txt (or never added when a new
   *_units.cpp landed) would just never run, with nothing saying so. Every
   tools\\tests\\*_units.cpp must be either the "unit" field of some row in
   suites.txt, or named on an "# excluded: <path> - <reason>" line at the top
   of the file.
2. Originally a dropped "if errorlevel 1 exit /b 1" after a suite's .exe let
   cmd carry the next command's status forward, so a failing suite still left
   build_tests.bat reporting success -- twice, each time while the run was
   being cited as evidence the suites passed. Now that run_suites.py runs the
   suites instead of hand-written .bat blocks, the equivalent regression
   would be the runner itself failing to propagate a suite's failure into its
   own exit code. Checked here empirically, not by reading the runner's
   source: a scratch suite that always fails is compiled and run through
   run_suites.py, and this script fails unless that comes back non-zero and
   names the suite.
3. Every suite now shares tools\\tests\\check.h instead of defining its own
   Check(). A check.h whose Check()/CHECK() never actually counted a failure,
   or whose CheckExit() returned 0 regardless, would make every suite that
   switched to it pass no matter what it checked. A scratch suite that runs
   Check(false, ...) and CHECK(false, ...) through check.h must fail the run
   and report both failures.
4. run_suites.py's no-op knobs (TEST_SUITES_NOOP_CL=1, TEST_SUITES_NOOP_LINK=1)
   turn every compile or link into a command that exits 0 and writes nothing.
   A valid scratch suite run under each must still fail, through the runner's
   check that each step left a fresh output, or a compile or link that quietly
   did nothing would pass on the previous run's files.
5. Every git-tracked tools\\**\\test_*.py must be a row of
   tools\\tests\\py_tests.txt or py_tests_private.txt, or named on an
   "# excluded: <path> - <reason>" line in one of them, and every when= glob
   on a row must match some file of the tree.
6. run_py_tests.py must refuse, through its own checks, a failing module, an
   import crash, a module with no tests, a shard that selects no tests, a
   shard whose selection overlaps another's (TEST_PY_SHARD_SKEW=1) and a
   shard that exits 0 without its result (TEST_PY_SHARD_CRASH=1); a cpu
   token that never comes (a holder process on a scratch slots folder with
   one slot) must fail the first shard after one wait and every queued one
   at once; and with a base it cannot read it must still run, and pass,
   every module, a sharded one included, whose shards see KEO_CPU_HELD=1
   (each runs under a cpu token). Each case runs on scratch modules with its
   own timings and slots folders, so a busy host cannot delay it; the output
   is shown only when a case is not refused.

Run from the repo root (test_gate.py starts it there whatever the caller's
directory; a relative invocation from elsewhere is refused rather than
passing by accident); prints what it found and exits 1 on any problem, or ends with
"check_test_guards: all checks passed" and exits 0.
"""
import concurrent.futures
import fnmatch
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_py_tests  # noqa: E402

SUITES_TXT = "tools/tests/suites.txt"
PRIVATE_SUITES_TXT = os.path.join(os.path.dirname(SUITES_TXT), "suites_private.txt")
UNITS_GLOB = "tools/tests/*_units.cpp"
RUN_SUITES = "tools/tests/run_suites.py"
EXCLUDE_RE = re.compile(r"^#\s*excluded:\s*(\S+)\s*-\s*\S", re.IGNORECASE)


def to_slash(path):
    return path.replace("\\", "/")


def load_suites_file(path):
    """(unit sources listed as a suite, paths named on an "# excluded" line)."""
    try:
        text = open(path, encoding="utf-8", newline="").read()
    except IOError as exc:
        raise ValueError("cannot read %s (%s)" % (path, exc))
    units = set()
    excluded = set()
    for line in text.split("\n"):
        stripped = line.strip()
        m = EXCLUDE_RE.match(stripped)
        if m:
            excluded.add(to_slash(m.group(1)))
            continue
        if not stripped or stripped.startswith("#"):
            continue
        parts = [p.strip() for p in stripped.split("|")]
        if len(parts) == 4 and parts[1]:
            units.add(to_slash(parts[1]))
    return units, excluded


def check_coverage():
    if not os.path.isfile(SUITES_TXT):
        print("check_test_guards: cannot find %s (run from the repo root)" % SUITES_TXT)
        return False
    try:
        listed, excluded = load_suites_file(SUITES_TXT)
    except ValueError as exc:
        print("check_test_guards: %s" % exc)
        return False
    if os.path.isfile(PRIVATE_SUITES_TXT):
        try:
            private_listed, private_excluded = load_suites_file(PRIVATE_SUITES_TXT)
        except ValueError as exc:
            print("check_test_guards: %s" % exc)
            return False
        listed |= private_listed
        excluded |= private_excluded

    on_disk = set(to_slash(p) for p in glob.glob(UNITS_GLOB))
    if not on_disk:
        print("check_test_guards: matched no tools/tests/*_units.cpp -- the glob is stale")
        return False

    missing = sorted(on_disk - listed - excluded)
    stale_excludes = sorted(p for p in excluded if p not in on_disk)
    ok = True
    if missing:
        ok = False
        print("check_test_guards: %d file(s) not in %s and not excluded:" % (len(missing), SUITES_TXT))
        for p in missing:
            print("  %s" % p)
        print('Add a suite row, or an "# excluded: %s - <reason>" line.' % (missing[0] if missing else ""))
    if stale_excludes:
        ok = False
        print("check_test_guards: %d exclude line(s) name a file that no longer exists:" % len(stale_excludes))
        for p in stale_excludes:
            print("  %s" % p)
    if ok:
        # Kept as "N suites, all guarded" (the phrase the original per-block
        # .bat guard printed): callers match this exact line.
        # "Guarded" now means covered by suites.txt (or excluded with a
        # reason) and provably fails the run on failure (checked below,
        # not by this line), rather than "has an errorlevel check" -- the
        # guard moved from a per-block .bat pattern into run_suites.py.
        print("check_test_guards: %d suites, all guarded (%d *_units.cpp file(s), %d excluded)"
              % (len(listed), len(on_disk), len(excluded)))
    return ok


def _run_scratch_suite(name, body, extra_files=(), env=None):
    """Compiles and runs one scratch suite named `name` with the given C++
    main() body through run_suites.py, in an isolated scratch directory, and
    returns the finished subprocess.CompletedProcess. Caller cleans up.

    `extra_files` are repo-root-relative paths copied into the scratch
    directory before compiling, so a quoted #include next to the scratch
    .cpp resolves the way it would next to any real tools\\tests\\*.cpp.
    `env` is the runner's environment (default: this process's); the runner
    writes UTF-8, read back with replacement, so no output byte can fail it."""
    scratch_dir = tempfile.mkdtemp(prefix="check_test_guards_")
    cpp = os.path.join(scratch_dir, name + ".cpp")
    suites_path = os.path.join(scratch_dir, "suites.txt")
    for extra in extra_files:
        shutil.copy(extra, os.path.join(scratch_dir, os.path.basename(extra)))
    with open(cpp, "w", encoding="utf-8") as f:
        f.write(body)
    with open(suites_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("%s | %s | | \n" % (name, cpp))
    env = dict(os.environ if env is None else env)
    env["PYTHONIOENCODING"] = "utf-8"
    try:
        return subprocess.run(
            [sys.executable, RUN_SUITES, "--suites", suites_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
            encoding="utf-8", errors="replace")
    finally:
        # run_suites.py gives each suite its own build\tests\obj\<name>\
        # (so suites never race on a shared object name); clean up both that
        # and the top-level .exe/.log it leaves in build\tests\.
        shutil.rmtree(os.path.join("build", "tests", "obj", name), ignore_errors=True)
        for ext in (".exe", ".log"):
            try:
                os.remove(os.path.join("build", "tests", name + ext))
            except OSError:
                pass
        shutil.rmtree(scratch_dir, ignore_errors=True)


def check_runner_fails_on_failure():
    """Two scratch suites, proving run_suites.py cannot silently swallow a
    failure the way the hand-written .bat blocks once did (a dropped
    "if errorlevel 1 exit /b 1" let cmd carry the next command's status
    forward):

    1. A suite that compiles cleanly and then fails at run time (main()
       returns 1) must make run_suites.py exit non-zero and report
       "FAILED (exit 1)" for it, never "COMPILE FAILED" -- the run-exit-code
       path is the one this guard exists to prove, and a non-zero exit whose
       output merely contains the suite's name is not enough: a suite that
       failed to *compile* would satisfy that without ever exercising it.
    2. A suite that fails to compile must be reported as "COMPILE FAILED",
       not run at all.
    """
    ok = True

    run_result = _run_scratch_suite("_selftest_deliberate_fail", "int main() { return 1; }\n")
    run_ok = (run_result.returncode != 0
              and "_selftest_deliberate_fail: FAILED (exit 1)" in run_result.stdout
              and "COMPILE FAILED" not in run_result.stdout)
    if run_ok:
        print("check_test_guards: run_suites.py correctly failed (exit %s) on a suite that "
              "compiled and then failed at run time" % run_result.returncode)
    else:
        ok = False
        print("check_test_guards: run_suites.py did not correctly fail a suite that compiles "
              "and then fails at run time (exit %s):" % run_result.returncode)
        print(run_result.stdout)

    compile_result = _run_scratch_suite("_selftest_compile_fail", "this is not C++\n")
    compile_ok = (compile_result.returncode != 0
                  and "_selftest_compile_fail: COMPILE FAILED" in compile_result.stdout)
    if compile_ok:
        print("check_test_guards: run_suites.py correctly failed (exit %s) on a suite that "
              "does not compile" % compile_result.returncode)
    else:
        ok = False
        print("check_test_guards: run_suites.py did not correctly fail a suite that does not "
              "compile (exit %s):" % compile_result.returncode)
        print(compile_result.stdout)

    return ok


CHECK_H = "tools/tests/check.h"


def check_check_h_counts():
    """A suite whose check.h Check(false, ...) and CHECK(false, ...) run must fail the run
    and report both: run_suites.py judges exit codes only, so a Check that never counted,
    or a CheckExit that returned 0, would otherwise pass every suite."""
    result = _run_scratch_suite(
        "_selftest_check_h",
        '#include "check.h"\n'
        'int main() { Check(false, "Check"); CHECK(1 == 2, "CHECK"); '
        'return CheckExit("_selftest_check_h"); }\n',
        extra_files=(CHECK_H,))
    ok = (result.returncode != 0
          and "_selftest_check_h: FAILED (exit 1)" in result.stdout
          and "_selftest_check_h: 2 failure(s)" in result.stdout)
    if ok:
        print("check_test_guards: run_suites.py correctly failed (exit %s) on a suite whose "
              "check.h Check(false, ...) ran" % result.returncode)
    else:
        print("check_test_guards: check.h did not fail a suite whose Check(false, ...) and "
              "CHECK(false, ...) ran (exit %s):" % result.returncode)
        print(result.stdout)
    return ok


# (step, knob, the runner's message for a step that left no output)
NOOP_KNOBS = (("compile", "TEST_SUITES_NOOP_CL", "compile reported success but no object was produced"),
              ("link", "TEST_SUITES_NOOP_LINK", "link reported success but no executable was produced"))


def check_runner_refuses_noops():
    """A valid scratch suite run with each no-op knob must fail as COMPILE FAILED (exit 1), its
    output must show the knob fired and the runner's own no-output message, and the no-op link
    must leave no RUN line."""
    ok = True
    for step, knob, message in NOOP_KNOBS:
        name = "_selftest_noop_" + step
        env = dict(os.environ)
        for _, other, _ in NOOP_KNOBS:
            env.pop(other, None)
        env[knob] = "1"
        result = _run_scratch_suite(name, "int main() { return 0; }\n", env=env)
        lines = result.stdout.splitlines()
        refused = (result.returncode == 1
                   and "%s: COMPILE FAILED (exit 1)" % name in result.stdout
                   and "%s=1 ran" % knob in result.stdout
                   and message in result.stdout
                   and not any(l.strip() == "RUN" for l in lines))
        if refused:
            print("check_test_guards: run_suites.py refused a no-op %s (exit %s)" % (step, result.returncode))
        else:
            ok = False
            print("check_test_guards: run_suites.py did not refuse a no-op %s under %s=1 (exit %s):"
                  % (step, knob, result.returncode))
            print(result.stdout)
    return ok


RUN_PY_TESTS = "tools/tests/run_py_tests.py"
PY_LISTS = ("tools/tests/py_tests.txt", "tools/tests/py_tests_private.txt")
SKIP_DIRS = {"build", "__pycache__"}  # and every folder whose name starts with "."


def tree_files():
    """Repo-relative paths of the tree: git's tracked files, or a walk where git lists none."""
    try:
        out = subprocess.run(["git", "ls-files", "-z"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=True).stdout
        paths = [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]
    except (OSError, subprocess.CalledProcessError):
        paths = []
    if paths:
        return paths
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
        for name in files:
            paths.append(to_slash(os.path.relpath(os.path.join(root, name), ".")))
    return paths


def check_python_coverage():
    """Every git-tracked tools/**/test_*.py is listed or excluded, and no when= glob is stale."""
    errors, rows, excluded = [], [], set()
    for path in PY_LISTS:
        if not os.path.isfile(path):
            if path == PY_LISTS[0]:
                print("check_test_guards: cannot find %s (run from the repo root)" % path)
                return False
            continue
        rows.extend(run_py_tests.read_list(path, errors))
        for line in open(path, encoding="utf-8").read().split("\n"):
            m = EXCLUDE_RE.match(line.strip())
            if m:
                excluded.add(to_slash(m.group(1)).lower())
    if errors:
        for e in errors:
            print("check_test_guards: %s" % e)
        return False
    listed = set(to_slash(r.rel).lower() for r in rows)
    files = tree_files()
    tests = sorted(set(p for p in files if p.startswith("tools/") and os.path.isfile(p)
                       and fnmatch.fnmatch(p.rsplit("/", 1)[-1], "test_*.py")))
    if not tests:
        print("check_test_guards: matched no tools/**/test_*.py -- the search is stale")
        return False
    missing = [p for p in tests if p.lower() not in listed and p.lower() not in excluded]
    stale = sorted(p for p in excluded if not os.path.isfile(p))
    lowered = [p.lower() for p in files]
    dead = ["%s: %s" % (r.stem, g) for r in rows for g in (r.when or [])
            if not any(fnmatch.fnmatchcase(p, g.lower()) for p in lowered)]
    ok = True
    if missing:
        ok = False
        print("check_test_guards: %d python test module(s) in neither %s nor %s:"
              % (len(missing), PY_LISTS[0], PY_LISTS[1]))
        for p in missing:
            print("  %s" % p)
        print('Add a row, or an "# excluded: %s - <reason>" line.' % missing[0])
    if stale:
        ok = False
        print("check_test_guards: %d python exclude line(s) name a file that no longer exists:" % len(stale))
        for p in stale:
            print("  %s" % p)
    if dead:
        ok = False
        print("check_test_guards: %d when= glob(s) match no file of the tree:" % len(dead))
        for d in dead:
            print("  %s" % d)
    if ok:
        print("check_test_guards: %d python test modules, all listed (%d excluded)"
              % (len(tests), sum(1 for p in tests if p.lower() in excluded)))
    return ok


SCRATCH_TWO = ("import unittest\n\n\nclass T(unittest.TestCase):\n"
               "    def test_a(self):\n        pass\n\n    def test_b(self):\n        pass\n")
SCRATCH_FOUR = SCRATCH_TWO + "\n    def test_c(self):\n        pass\n\n    def test_d(self):\n        pass\n"
SCRATCH_FAIL = ("import unittest\n\n\nclass T(unittest.TestCase):\n"
                "    def test_a(self):\n        pass\n\n    def test_b(self):\n"
                "        self.assertEqual(1, 2)\n")
SCRATCH_IMPORT = "raise RuntimeError('scratch import crash marker')\n"
SCRATCH_EMPTY = "import unittest\n\n\nclass T(unittest.TestCase):\n    def helper(self):\n        pass\n"

# (case, [(stem, source, row options)], extra environment, reason in the FAILED line,
#  text the output must also carry)
PY_REFUSALS = [
    ("a failing module", [("_pyguard_fail", SCRATCH_FAIL, "")], {}, "1 failure(s), 0 error(s)",
     "AssertionError: 1 != 2"),
    ("an import crash", [("_pyguard_import", SCRATCH_IMPORT, "")], {}, "exit 2, no result",
     "scratch import crash marker"),
    ("a zero-test module", [("_pyguard_empty", SCRATCH_EMPTY, "")], {}, "no tests", None),
    ("over-sharding (shards=3 on 2 tests)", [("_pyguard_over", SCRATCH_TWO, "shards=3")], {},
     "shard 3/3 selected no tests", None),
    ("a skewed shard (TEST_PY_SHARD_SKEW=1)", [("_pyguard_skew", SCRATCH_FOUR, "shards=2")],
     {"TEST_PY_SHARD_SKEW": "1"}, "shards overlap", None),
    ("a shard with no result (TEST_PY_SHARD_CRASH=1)", [("_pyguard_crash", SCRATCH_TWO, "shards=2")],
     {"TEST_PY_SHARD_CRASH": "1"}, "shard 1/2 wrote no result", None),
]
SCRATCH_HELD = ("import os\nimport unittest\n\n\nclass T(unittest.TestCase):\n"
                "    def test_a(self):\n        self.assertEqual(os.environ.get('KEO_CPU_HELD'), '1')\n\n"
                "    def test_b(self):\n        self.assertEqual(os.environ.get('KEO_CPU_HELD'), '1')\n")
PY_FAIL_OPEN = [("_pyguard_a", SCRATCH_HELD, "shards=2"),
                ("_pyguard_b", SCRATCH_TWO, "when=no/such/folder/**")]
PY_ENV_CLEAR = ("PY_TESTS_SINCE", "TEST_PY_SHARD_SKEW", "TEST_PY_SHARD_CRASH",
                "KEO_CPU_HELD", "KEO_HEAVY_HELD", "KEO_SLOTS")
TOKEN_CASE = "a cpu token that never comes, once for the whole queue"
TOKEN_MODULES = [("_pyguard_tok%d" % i, SCRATCH_TWO, "") for i in (1, 2, 3)]
TOKEN_WAIT_S = 2
TOKEN_SKIPPED = "could not start: an earlier shard could not get a cpu token"
HOLDER = ("import sys\nsys.path.insert(0, sys.argv[1])\nimport slots\n"
          "with slots.cpu_token('check_test_guards holder'):\n"
          "    sys.stdout.write('held\\n')\n    sys.stdout.flush()\n    sys.stdin.read()\n")


def _run_scratch_py(root, name, modules, env_extra, args=(), base_env=None):
    """Writes the scratch modules and their list under root/name and runs run_py_tests.py on them."""
    folder = os.path.join(root, name)
    os.makedirs(folder)
    lines = []
    for i, (stem, source, opts) in enumerate(modules):
        path = os.path.join(folder, stem + ".py")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(source)
        lines.append(("%d %s %s" % (10 * (i + 1), path, opts)).rstrip())
    listing = os.path.join(folder, "list.txt")
    with open(listing, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ if base_env is None else base_env)
    for key in PY_ENV_CLEAR:
        env.pop(key, None)
    env["KEO_TIMINGS_DIR"] = os.path.join(folder, "timings")
    if base_env is None:
        env["KEO_SLOTS_DIR"] = os.path.join(folder, "slots")
    env.update(env_extra)
    return subprocess.run([sys.executable, RUN_PY_TESTS, "--list", listing] + list(args),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                          stdin=subprocess.DEVNULL)


def _run_token_timeout(root):
    """Runs three one-shard scratch modules, one job at a time, while a holder process keeps the
    only cpu slot of a scratch slots folder. Returns (result or None, holder note)."""
    folder = os.path.join(root, "token")
    slots_dir = os.path.join(folder, "slots")
    os.makedirs(slots_dir)
    env = dict(os.environ)
    for key in PY_ENV_CLEAR:
        env.pop(key, None)
    env.update({"KEO_SLOTS_DIR": slots_dir, "KEO_CPU_SLOTS": "1"})
    holder = subprocess.Popen([sys.executable, "-c", HOLDER, os.path.abspath("tools/build")],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env=env)
    try:
        first = holder.stdout.readline().strip()
        if first != b"held":
            return None, "the holder did not take the slot: %r" % first
        env.update({"KEO_CPU_WAIT": str(TOKEN_WAIT_S), "PY_JOBS": "1"})
        listing_root = os.path.join(folder, "run")
        os.makedirs(listing_root)
        return _run_scratch_py(listing_root, "list", TOKEN_MODULES, {}, base_env=env), None
    finally:
        try:
            holder.stdin.close()
            holder.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            holder.kill()


def _show(result):
    text = result.stdout.decode("utf-8", "replace")
    for line in text.splitlines():
        print("  | %s" % line)


def start_py_runner_controls(pool, root):
    """Starts every run_py_tests.py control on pool; check_py_runner_controls reports them."""
    futures = []
    for i, (case, modules, env_extra, _, _) in enumerate(PY_REFUSALS):
        futures.append(pool.submit(_run_scratch_py, root, "refuse%d" % i, modules, env_extra))
    futures.append(pool.submit(_run_token_timeout, root))
    futures.append(pool.submit(_run_scratch_py, root, "failopen", PY_FAIL_OPEN, {},
                               ("--since", "no-such-rev-for-check-test-guards")))
    return futures


def check_py_runner_controls(futures):
    ok = True
    for (case, modules, _, reason, shown), fut in zip(PY_REFUSALS, futures):
        result = fut.result()
        text = result.stdout.decode("utf-8", "replace")
        lines = text.splitlines()
        want = "python tests FAILED: %s (%s)" % (modules[0][0], reason)
        if (result.returncode == 1 and lines and lines[-1].strip() == want
                and not any(l.startswith("python tests: ") for l in lines)
                and (shown is None or shown in text)):
            print("check_test_guards: run_py_tests.py refused %s (exit %s)" % (case, result.returncode))
        else:
            ok = False
            print('check_test_guards: run_py_tests.py did not refuse %s with "%s" (exit %s):'
                  % (case, want, result.returncode))
            _show(result)
    result, note = futures[len(PY_REFUSALS)].result()
    text = result.stdout.decode("utf-8", "replace") if result is not None else ""
    lines = text.splitlines()
    first, rest = TOKEN_MODULES[0][0], [stem for stem, _, _ in TOKEN_MODULES[1:]]
    head = "python tests FAILED: %s (could not start: slots: FAILED: no cpu slot within " % first
    tail = ", ".join("%s (%s)" % (stem, TOKEN_SKIPPED) for stem in rest)
    # The queued modules' own clocks, not the run's wall, show they did not wait: a loaded
    # host can stretch the runner's start-up past a second.
    waits = [re.search(r"^py: %s: FAILED \(%s\), 1 shard\(s\), ([\d.]+) s\r?$"
                       % (stem, re.escape(TOKEN_SKIPPED)), text, re.M) for stem in rest]
    if (result is not None and result.returncode == 1 and lines
            and lines[-1].strip().startswith(head) and lines[-1].strip().endswith("), " + tail)
            and all(m and float(m.group(1)) < TOKEN_WAIT_S / 2.0 for m in waits)):
        print("check_test_guards: run_py_tests.py refused %s (exit %s)" % (TOKEN_CASE, result.returncode))
    else:
        ok = False
        print("check_test_guards: run_py_tests.py did not refuse %s: want one %d s wait, then "
              '"%s" at once for the rest (exit %s)%s'
              % (TOKEN_CASE, TOKEN_WAIT_S, TOKEN_SKIPPED, None if result is None else result.returncode,
                 (": " + note) if note else ":"))
        if result is not None:
            _show(result)
    result = futures[len(PY_REFUSALS) + 1].result()
    lines = [l.strip() for l in result.stdout.decode("utf-8", "replace").splitlines()]
    want = "python tests: %s OK" % ", ".join(stem for stem, _, _ in PY_FAIL_OPEN)
    if (result.returncode == 0 and lines and lines[-1] == want
            and any(l.startswith("python test selection: off (") for l in lines)
            and "py: _pyguard_a: 2 tests, 2 shard(s)" in "\n".join(lines)):
        print("check_test_guards: run_py_tests.py ran every module when the base could not be read")
    else:
        ok = False
        print('check_test_guards: run_py_tests.py did not run and pass every module, a sharded one '
              'included, after an unreadable base (exit %s):' % result.returncode)
        _show(result)
    return ok


def main():
    py_root = tempfile.mkdtemp(prefix="check_test_guards_py_")
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(PY_REFUSALS) + 2) as pool:
            py_futures = start_py_runner_controls(pool, py_root)
            coverage_ok = check_coverage()
            runner_ok = check_runner_fails_on_failure()
            check_h_ok = check_check_h_counts()
            noop_ok = check_runner_refuses_noops()
            py_coverage_ok = check_python_coverage()
            py_runner_ok = check_py_runner_controls(py_futures)
    finally:
        shutil.rmtree(py_root, ignore_errors=True)
    if coverage_ok and runner_ok and check_h_ok and noop_ok and py_coverage_ok and py_runner_ok:
        print("check_test_guards: all checks passed")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

"""Guard the three ways the test run has silently reported success before.

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

Run from the repo root (build_tests.bat anchors its own cd before calling
this, so a relative invocation from elsewhere is refused rather than passing
by accident); prints what it found and exits 1 on any problem.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SUITES_TXT = "tools/tests/suites.txt"
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
        # .bat guard printed): session checklists quote this exact line.
        # "Guarded" now means covered by suites.txt (or excluded with a
        # reason) and provably fails the run on failure (checked below,
        # not by this line), rather than "has an errorlevel check" -- the
        # guard moved from a per-block .bat pattern into run_suites.py.
        print("check_test_guards: %d suites, all guarded (%d *_units.cpp file(s), %d excluded)"
              % (len(listed), len(on_disk), len(excluded)))
    return ok


def _run_scratch_suite(name, body, extra_files=()):
    """Compiles and runs one scratch suite named `name` with the given C++
    main() body through run_suites.py, in an isolated scratch directory, and
    returns the finished subprocess.CompletedProcess. Caller cleans up.

    `extra_files` are repo-root-relative paths copied into the scratch
    directory before compiling, so a quoted #include next to the scratch
    .cpp resolves the way it would next to any real tools\\tests\\*.cpp."""
    scratch_dir = tempfile.mkdtemp(prefix="check_test_guards_")
    cpp = os.path.join(scratch_dir, name + ".cpp")
    suites_path = os.path.join(scratch_dir, "suites.txt")
    for extra in extra_files:
        shutil.copy(extra, os.path.join(scratch_dir, os.path.basename(extra)))
    with open(cpp, "w", encoding="utf-8") as f:
        f.write(body)
    with open(suites_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("%s | %s | | \n" % (name, cpp))
    try:
        return subprocess.run(
            [sys.executable, RUN_SUITES, "--suites", suites_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    finally:
        # run_suites.py gives each suite its own build\tests\obj\<name>\
        # (the fix for the cross-suite object-name race); clean up both that
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


def main():
    coverage_ok = check_coverage()
    runner_ok = check_runner_fails_on_failure()
    check_h_ok = check_check_h_counts()
    return 0 if (coverage_ok and runner_ok and check_h_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

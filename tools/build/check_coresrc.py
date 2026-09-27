"""Guard tools\\build\\coresrc.txt the same way check_test_guards.py guards
tools\\tests\\suites.txt: every tracked source must be listed, or explicitly
excluded with a reason.

A suite dropped from suites.txt is caught by check_test_guards.py (a
*_units.cpp on disk but not listed). A source dropped from coresrc.txt had no
equivalent guard: it usually becomes an unresolved-external link error, but a
translation unit nothing else references would vanish from every build
silently. Every src\\**\\*.cpp must be in coresrc.txt, or named on
an "# excluded: <path> - <reason>" line at the top of the file, exactly like
suites.txt's convention.

coresrc.txt's order is the link order (tools\\build\\variant.bat reads it
straight into the link's response file), so this also refuses a duplicate
line (harmless to the link itself -- LNK4042, still a complete DLL -- but a
shape that should never occur) and two entries whose object names collide,
which silently drops one translation unit's object before the link ever
runs (the second overwrites the first in the object directory).

Run from the repo root; prints what it found and exits 1 on any problem.
Usage: python tools\\build\\check_coresrc.py [--coresrc PATH]
"""
import argparse
import collections
import glob
import os
import re
import sys

CORESRC_TXT = "tools/build/coresrc.txt"
CPP_GLOB = "src/**/*.cpp"
EXCLUDE_RE = re.compile(r"^#\s*excluded:\s*(\S+)\s*-\s*\S", re.IGNORECASE)


def to_slash(path):
    return path.replace("\\", "/")


def load_coresrc(path):
    """(listed sources in file order, paths named on an "# excluded" line). A blank line is
    skipped; a line whose first character is "#" is a comment (an "# excluded: <path> -
    <reason>" comment also names an exclusion); every other line is one entry, and it must
    carry no leading or trailing blanks (its line ending aside; a line of blanks alone is
    fine) -- raises ValueError("<path>:<n>: leading or trailing blanks") otherwise."""
    try:
        text = open(path, encoding="utf-8", newline="").read()
    except IOError as exc:
        raise ValueError("cannot read %s (%s)" % (path, exc))
    listed = []
    excluded = set()
    for n, raw in enumerate(text.split("\n"), 1):
        core = raw[:-1] if raw.endswith("\r") else raw
        if not core.strip():
            continue
        if core[:1] == "#":
            m = EXCLUDE_RE.match(core)
            if m:
                excluded.add(to_slash(m.group(1)))
            continue
        if core != core.strip():
            raise ValueError("%s:%d: leading or trailing blanks" % (path, n))
        listed.append(to_slash(core))
    return listed, excluded


def on_disk():
    return set(to_slash(p) for p in glob.glob(CPP_GLOB, recursive=True))


def profiler_sources():
    """Every *.cpp directly under profiler/ when that folder exists, else every *.cpp at the
    repository root (the rule survey.profiler_files uses), plus every src/**/klib_*.cpp."""
    if os.path.isdir("profiler"):
        roots = glob.glob("profiler/*.cpp")
    else:
        roots = glob.glob("*.cpp")
    klib = glob.glob("src/**/klib_*.cpp", recursive=True)
    return set(to_slash(p) for p in roots) | set(to_slash(p) for p in klib)


def _run_checks(list_path, expected, list_name, distinguish_not_source=False, empty_msg=None):
    """Shared duplicate / object-name-collision / coverage checks for one list file against an
    expected set of source paths. distinguish_not_source splits a listed, non-expected entry
    into 'no longer exists on disk' (the default pass's line) vs 'is not a <list_name> source'
    (a listed file that is real but outside the expected set). Returns (ok, listed_set, excluded)."""
    if not os.path.isfile(list_path):
        print("check_coresrc: cannot find %s (run from the repo root)" % list_path)
        return False, set(), set()
    try:
        listed, excluded = load_coresrc(list_path)
    except ValueError as exc:
        print("check_coresrc: %s" % exc)
        return False, set(), set()

    if empty_msg is not None and not expected:
        print(empty_msg)
        return False, set(), set()

    ok = True

    counts = collections.Counter(listed)
    duplicates = sorted(p for p, n in counts.items() if n > 1)
    if duplicates:
        ok = False
        dup_count = sum(counts[p] - 1 for p in duplicates)
        print("check_coresrc: %d duplicate line(s) in %s:" % (dup_count, list_path))
        for p in duplicates:
            print("  %s" % p)

    listed_set = set(listed)
    by_object = collections.defaultdict(list)
    for p in listed_set:
        by_object[os.path.splitext(os.path.basename(p))[0].lower()].append(p)
    for name, paths in sorted(by_object.items()):
        if len(paths) > 1:
            ok = False
            a, b = sorted(paths)[:2]
            print("check_coresrc: two sources share one object name: %s and %s" % (a, b))

    missing = sorted(expected - listed_set - excluded)
    stale_excludes = sorted(p for p in excluded if p not in expected)
    not_in_expected = sorted(p for p in listed_set if p not in expected)
    if distinguish_not_source:
        stale_listed = [p for p in not_in_expected if not os.path.isfile(p)]
        not_source = [p for p in not_in_expected if os.path.isfile(p)]
    else:
        stale_listed = not_in_expected
        not_source = []

    if missing:
        ok = False
        print("check_coresrc: %d file(s) not in %s and not excluded:" % (len(missing), list_path))
        for p in missing:
            print("  %s" % p)
        print('Add it to %s (its order is the link order: append, never re-sort), '
              'or an "# excluded: %s - <reason>" line.' % (list_name, missing[0]))
    if stale_excludes:
        ok = False
        print("check_coresrc: %d exclude line(s) name a file that no longer exists:" % len(stale_excludes))
        for p in stale_excludes:
            print("  %s" % p)
    if stale_listed:
        ok = False
        print("check_coresrc: %d listed file(s) no longer exist on disk (a stale entry links a "
              "deleted file, or a corresponding .obj lingers):" % len(stale_listed))
        for p in stale_listed:
            print("  %s" % p)
    if not_source:
        ok = False
        print("check_coresrc: %d listed file(s) are not profiler sources:" % len(not_source))
        for p in not_source:
            print("  %s" % p)

    return ok, listed_set, excluded


def main(argv=()):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--coresrc", default=CORESRC_TXT)
    parser.add_argument("--profsrc", nargs="?", const="tools/build/profsrc.txt", default=None)
    args = parser.parse_args(list(argv))

    if args.profsrc is not None:
        expected = profiler_sources()
        ok, listed_set, excluded = _run_checks(args.profsrc, expected, "profsrc.txt",
                                                distinguish_not_source=True)
        if ok:
            print("check_coresrc --profsrc: %d source(s) covering %d profiler file(s), %d excluded"
                  % (len(listed_set), len(expected), len(excluded)))
        return 0 if ok else 1

    coresrc_txt = args.coresrc
    disk = on_disk()
    ok, listed_set, excluded = _run_checks(
        coresrc_txt, disk, "coresrc.txt",
        empty_msg="check_coresrc: matched no src/**/*.cpp -- the glob is stale")
    if ok:
        print("check_coresrc: %d source(s) covering %d src/**/*.cpp file(s), %d excluded"
              % (len(listed_set), len(disk), len(excluded)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

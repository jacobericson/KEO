"""Build KEO variants as concurrent processes (Python 3, stdlib only).

Called by build_opt_step4.bat (and so build_opt.bat) after
they have set up the VS 2010 x64 environment and run build_env.bat; every child
inherits that environment. Each variant runs tools\\build\\variant.bat twice,
"compile" then "link", each in its own cmd process with its own arguments,
and writes its whole output to <OBJDIR>\\build.log. The console gets one line
per finished variant (its "build OK" line), any compiler/linker warning lines,
and on failure the tail of the failing variant's log. Every variant compiles
every tools\\build\\coresrc.txt source.

Usage:
  run_variants.py --fail-prefix "STEP4 FAILED at" [--defines "<common>"]
                  --variant OUTDIR OBJDIR "<extra defines>" "<label>" [--variant ...]

  --defines     defines shared by every variant; each variant compiles with
                "<common> <extra>", in that order.
  --variant     one variant; LABEL "" means OUTDIR.

Environment:
  BUILD_JOBS  how many processes (a variant's compile or its link) run at once
              (default: logical cores / 2, at least 1, at most the number of
              variants). BUILD_JOBS=1 builds one variant after another in the
              order given.
  BUILD_MP    cl /MP process count per compile. Unset = logical cores divided
              by BUILD_JOBS; 0 = no /MP; N = /MPN.

  Memory per process: a cl process needs about 260 MB and an LTCG link about
  90 MB.

Failure: once a variant fails, no new variant is started; a variant already
started still finishes (its link runs once its own compile succeeded). The
script then prints "<fail-prefix> <OUTDIR>" for the first failed variant in
the order given (the variant the one-at-a-time scripts would have stopped
at) and exits 1. Exit 0 when every variant built.
"""
import argparse
import os
import queue
import re
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True  # no tools\build\__pycache__ in the worktree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_coresrc  # noqa: E402

VARIANT_BAT = r'tools\build\variant.bat'
CORESRC_TXT = r'tools\build\coresrc.txt'
TAIL_LINES = 40
WARNING_RE = re.compile(r'\bwarning [A-Z]+\d+', re.IGNORECASE)


def env_count(name, minimum):
    """Positive integer from the environment, None when unset."""
    raw = os.environ.get(name, '').strip()
    if not raw:
        return None
    if not raw.isdigit() or int(raw) < minimum:
        raise ValueError('%s must be a whole number >= %d (got "%s")' % (name, minimum, raw))
    return int(raw)


def quote(arg):
    if '"' in arg:
        raise ValueError('argument must not contain a double quote: %s' % arg)
    if arg.endswith('\\'):
        raise ValueError('argument must not end with a backslash: %s' % arg)
    return '"%s"' % arg


def read_log(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('mbcs', errors='replace').splitlines()
    except OSError:
        return []


def append_log(path, message):
    with open(path, 'ab') as log:
        log.write((message + '\r\n').encode('mbcs', 'replace'))


def core_sources(path=CORESRC_TXT):
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r\n', '\n')
    sources = [line.strip() for line in text.split('\n')]
    sources = [s for s in sources if s and not s.startswith('#')]
    if not sources:
        raise ValueError('%s is empty' % path)
    return sources


def obj_name(source):
    return os.path.splitext(os.path.basename(source))[0] + '.obj'


class Variant(object):
    def __init__(self, index, outdir, objdir, defines, label):
        self.index = index
        self.outdir = outdir
        self.objdir = objdir
        self.defines = defines
        self.label = label or outdir
        self.flavour = 'dev' if '_dev' in outdir.lower() else 'prod'  # variant.bat's rule
        self.log = os.path.join(objdir, 'build.log')
        self.sources = []        # every CORESRC source, set once in main()
        self.returncode = None
        self.start = None
        self.seconds = 0.0


class Task(object):
    def __init__(self, variant, kind):
        self.variant = variant
        self.kind = kind         # 'compile' or 'link'
        self.state = 'pending'   # pending, running, ok, failed
        self.deps = []           # tasks that must be 'ok' first

    def ready(self):
        return self.state == 'pending' and all(d.state == 'ok' for d in self.deps)


def check_objects_fresh(objdir, sources, start_time):
    """Problems with the objects `sources` should have produced in objdir: a
    missing .obj, or one whose mtime predates start_time (a leftover from
    before this compile, somehow not removed). Returns a list of strings,
    empty when everything is fresh."""
    problems = []
    for source in sources:
        path = os.path.join(objdir, obj_name(source))
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            problems.append('%s: no object was produced (%s)' % (source, path))
            continue
        if mtime < start_time:
            problems.append('%s: %s predates this compile (mtime %.3f < start %.3f) -- not deleted first?'
                             % (source, path, mtime, start_time))
    return problems


def run_task(task, mpflag, done):
    """Runs one variant.bat step; always reports (task, returncode) on the queue:
    the main loop waits for one report per started task, so a thread that died
    without one would hang the build."""
    v = task.variant
    rc = 1
    try:
        if task.kind == 'compile':
            os.makedirs(v.objdir, exist_ok=True)
            compile_start = time.time()
            # This variant's own objects from a *previous* build must not
            # survive into this one: a compile that silently turns into a
            # no-op (a future variant.bat or run_variants bug) would
            # otherwise exit 0 and let the link reuse the old objects with
            # no error anywhere. Deleting them first means a no-op compile
            # leaves the object simply missing, caught by
            # check_objects_fresh() below.
            for source in v.sources:
                path = os.path.join(v.objdir, obj_name(source))
                if os.path.exists(path):
                    os.remove(path)
            mode, openmode = 'compile', 'wb'
        else:
            mode, openmode = 'link', 'ab'
        command = '%s /d /c %s %s %s %s %s %s %s' % (
            os.environ.get('ComSpec', 'cmd.exe'), VARIANT_BAT, mode, quote(v.outdir), quote(v.objdir),
            quote(v.defines), quote(mpflag), quote(v.label))
        with open(v.log, openmode) as log:
            rc = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        if task.kind == 'compile' and rc == 0:
            # Belt and braces on top of the pre-compile delete above: even if
            # something recreated a stale object (or the delete itself
            # failed silently), this is the last point before the object
            # could reach a link.
            problems = check_objects_fresh(v.objdir, v.sources, compile_start)
            if problems:
                rc = 1
                append_log(v.log, 'run_variants.py: compile reported success but produced no fresh '
                                  'object for %d source(s):\n  %s' % (len(problems), '\n  '.join(problems)))
    except (OSError, ValueError) as error:
        rc = 1
        message = 'run_variants.py: could not run the %s step: %s' % (task.kind, error)
        try:
            # The object folder may be what failed (os.makedirs), in which case
            # there is no log to append to; the message then goes to stdout.
            append_log(v.log, message)
        except OSError:
            print('%s: %s' % (v.label, message))
            sys.stdout.flush()
    except Exception as error:  # never leave the main loop waiting for this task
        rc = 1
        print('%s: run_variants.py: unexpected error: %r' % (v.label, error))
        sys.stdout.flush()
    finally:
        done.put((task, rc))


def report(variant):
    lines = read_log(variant.log)
    warnings = [l.strip() for l in lines if WARNING_RE.search(l)]
    for line in warnings:
        print('  %s: %s' % (variant.label, line))
    if variant.returncode == 0:
        ok = [l for l in lines if l.startswith(variant.label + ' build OK:')]
        print('%s   [%.1f s]' % (ok[-1] if ok else variant.label + ' build OK', variant.seconds))
    else:
        print('')
        print('%s FAILED (exit %s) after %.1f s. Last %d lines of %s:' % (
            variant.label, variant.returncode, variant.seconds, TAIL_LINES, variant.log))
        for line in lines[-TAIL_LINES:]:
            print('    ' + line)
    sys.stdout.flush()


def main():
    try:
        sys.stdout.reconfigure(errors='replace')
    except AttributeError:
        pass
    # A source dropped from coresrc.txt has no equivalent guard otherwise: it
    # usually becomes an unresolved-external link error, but a translation
    # unit nothing else references would vanish from every build silently.
    if check_coresrc.main() != 0:
        return 1
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--fail-prefix', required=True)
    parser.add_argument('--defines', default='')
    parser.add_argument('--variant', nargs=4, action='append', required=True,
                        metavar=('OUTDIR', 'OBJDIR', 'DEFINES', 'LABEL'))
    args = parser.parse_args()

    variants = []
    for index, (outdir, objdir, extra, label) in enumerate(args.variant):
        defines = ' '.join(part for part in (args.defines.strip(), extra.strip()) if part)
        variants.append(Variant(index, outdir, objdir, defines, label))

    try:
        cores = os.cpu_count() or 1
        jobs = env_count('BUILD_JOBS', 1)
        if jobs is None:
            jobs = max(1, cores // 2)
        jobs = min(jobs, len(variants))
        mp = env_count('BUILD_MP', 0)
        if mp is None:
            mp = max(1, cores // jobs)
        mpflag = '/MP%d' % mp if mp > 1 else ''
        sources = core_sources()
        for v in variants:
            v.sources = sources
        for v in variants:
            for arg in (v.outdir, v.objdir, v.defines, mpflag, v.label):
                quote(arg)
    except (OSError, ValueError) as error:
        print('ERROR: %s' % error)
        return 1

    print('Building %d variant(s), %d processes at a time%s; per-variant logs: <object folder>\\build.log' % (
        len(variants), jobs, ', cl ' + mpflag if mpflag else ', no cl /MP'))
    sys.stdout.flush()

    # Task list in priority order: each variant's compile, then its link. A
    # link waits for its own compile only.
    compiles = dict((v.index, Task(v, 'compile')) for v in variants)
    tasks = []
    for v in variants:
        link = Task(v, 'link')
        link.deps = [compiles[v.index]]
        tasks += [compiles[v.index], link]

    done = queue.Queue()
    running = 0
    failed = []
    while True:
        # After a failure no new variant starts, but a variant already started
        # still finishes (its link runs once its compile is done).
        for task in tasks:
            if running >= jobs:
                break
            if task.ready() and (not failed or task.variant.start is not None):
                task.state = 'running'
                if task.variant.start is None:
                    task.variant.start = time.time()
                threading.Thread(target=run_task, args=(task, mpflag, done)).start()
                running += 1
        if not running:
            break
        task, rc = done.get()
        running -= 1
        task.state = 'ok' if rc == 0 else 'failed'
        v = task.variant
        if rc != 0 or task.kind == 'link':
            v.returncode = rc
            v.seconds = time.time() - v.start
            report(v)
            if rc != 0:
                failed.append(v)

    unfinished = [v for v in variants if v.returncode is None]
    if failed or unfinished:
        if unfinished:
            print('Not built after the failure: %s' % ' '.join(v.outdir for v in unfinished))
        if not failed:  # cannot happen (links only wait on earlier compiles); never report success
            print('')
            print('%s %s' % (args.fail_prefix, unfinished[0].outdir))
            return 1
        first = min(failed, key=lambda v: v.index)
        if len(failed) > 1:
            print('Also failed: %s' % ' '.join(v.outdir for v in sorted(failed, key=lambda v: v.index) if v is not first))
        print('')
        print('%s %s' % (args.fail_prefix, first.outdir))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

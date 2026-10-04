"""Compile a source list with clang-cl, one process per source, in parallel
(Python 3, stdlib only).

clang-cl accepts /MP but ignores it ("argument unused during compilation")
and compiles its sources one after another, so the clang build scripts
(tools\\build\\variant_clang.bat, build_clang.bat) compile through here: one
clang-cl process per source, at most --jobs at once, each given the same
flags response file. Each object is named after its source's basename in
--objdir, as cl's /Fo<dir>\\ names them, so the link lists built from the
same source list find them.

Every unit must compile with /EHa. Under any other model clang does not
treat a plain load or store as able to fault, so a __try gets no handler, or
a scope table that leaves the guarded load outside it, and the fault it
exists to catch ends the process. The /EH options each unit would get (the
flags file's, then any per-source one, applied in order as clang-cl applies
them) are resolved before anything compiles, and a unit left without /EHa
refuses the build unless it is named by --ehsc. clang-cl reads CL (before
its command line) and _CL_ (after it) from the environment, so a leftover
_CL_=/EHsc would override that resolution unseen; every clang-cl this runs,
the /E runs included, gets an environment without either, and a line says
so when either was set.

--ehsc SOURCE (repeatable) compiles that source with /EHsc after the flags
file. It is refused when the source's preprocessed text (clang-cl /E, with
the same flags) holds a __try: an inline function from a header is compiled
into every unit that uses it, and the linker may keep the /EHsc unit's copy
for the whole DLL.

Every compiler message goes to --log, warnings included. The console gets one
summary line and, on failure, each failing source's first error lines. Exit
0 only when every source compiled and left a fresh object; 1 otherwise, with
the failures named.

Usage:
  clang_compile.py --clang <clang-cl.exe> --flags <file.rsp> --sources <list.txt>
                   --objdir <dir> --log <file> [--ehsc <source> ...] [--syntax-only]
  clang_compile.py --syntax-only --clang <clang-cl.exe>
                   --pass NAME FLAGS SOURCES LOG [--ehsc <source> ...] [--pass ...]
    --sources  a text file, one source path per line (tools\\build\\coresrc.txt)
    --syntax-only  clang-cl /Zs: parse and type-check, write no object, leave
                   --objdir out. Every source is fresh by definition.
    --pass     one syntax-only pass: its name, flags file, source list and log,
               then its own --ehsc sources. Repeatable; the single-pass form is
               one pass.

A syntax-only run puts every pass through one queue: the passes' /E checks
first, then all their sources longest first (by the time the pass's previous
log recorded, else by file size scaled to the median recorded seconds per
byte). Every pass's checks above run before anything starts, and every pass's
log is deleted before the queue starts, so a run that stops leaves no log that
reads as clean. Each log is written once the queue is done, in its list's
order, exactly as one pass alone writes it; a pass refused by its /E check
writes no log. A pass's summary line times it from the queue's start to its
last process's end. The run holds one host-wide heavy slot and each clang-cl
one cpu token (tools\\build\\slots.py); started under a cpu token, it runs
one process at a time.

Environment: BUILD_MP = processes at once (unset = logical cores, 0 = one).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True

ERROR_RE = re.compile(r': (fatal )?error:')
WARNING_RE = re.compile(r': warning:')
LOG_HEADER_RE = re.compile(r'^=== (\S+)  rc=-?\d+  ([0-9.]+)s\r?$')
FIRST_ERRORS = 8


def read_list(path):
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r\n', '\n')
    return [line.strip() for line in text.split('\n') if line.strip() and not line.strip().startswith('#')]


def job_count():
    """BUILD_MP as the MSVC scripts read it: unset = logical cores, 0 = no
    parallel compile (one process), N = N processes."""
    raw = os.environ.get('BUILD_MP', '').strip()
    if not raw:
        return os.cpu_count() or 1
    if not raw.isdigit():
        raise ValueError('BUILD_MP must be a whole number >= 0 (got "%s")' % raw)
    return max(1, int(raw))


def check_vs2010_environment():
    """clang-cl takes its system headers from INCLUDE and lld-link its
    libraries from LIB. Both must be VS 2010's alone: an entry from a later
    Visual Studio or a Windows Kits SDK behind them would supply any header
    or library 2010 lacks, silently, from another runtime."""
    for var in ('INCLUDE', 'LIB'):
        entries = [e for e in os.environ.get(var, '').split(';') if e.strip()]
        if not entries:
            raise ValueError('%s is empty; run VS 2010 vcvarsall amd64 first' % var)
        wrong = [e for e in entries
                 if re.search(r'Microsoft Visual Studio(?! 10\.0)', e, re.I) or re.search(r'Windows Kits', e, re.I)]
        if wrong:
            raise ValueError('%s holds a toolset other than VS 2010: %s (start from an empty %s)'
                             % (var, wrong[0], var))


def read_flag_tokens(path):
    """The flags file's arguments. Quotes only ever wrap a path, so a token is
    a run of non-space characters and quoted spans."""
    with open(path, 'rb') as f:
        text = f.read().decode('mbcs', 'replace')
    return re.findall(r'(?:[^\s"]+|"[^"]*")+', text)


def eh_async(tokens):
    """True when the /EH options in tokens, applied in order, leave /EHa on.
    Mirrors clang-cl: 'a' turns asynchronous handling on (and 's' off),
    's' turns it off, a trailing '-' negates the letter before it."""
    asynch = False
    for tok in tokens:
        m = re.match(r'^[/-]EH(.*)$', tok)
        if not m:
            continue
        v = m.group(1)
        i = 0
        while i < len(v):
            neg = i + 1 < len(v) and v[i + 1] == '-'
            if v[i] == 'a':
                asynch = not neg
            elif v[i] == 's' and not neg:
                asynch = False
            i += 2 if neg else 1
    return asynch


def clang_child_env():
    """(environment for clang-cl, the CL/_CL_ settings left out of it).
    clang-cl adds CL's options before its command line and _CL_'s after it,
    so either could change a unit's flags past every check made here."""
    env = {}
    dropped = []
    for key, value in os.environ.items():
        if key.upper() in ('CL', '_CL_'):
            dropped.append('%s=%s' % (key.upper(), value))
        else:
            env[key] = value
    return env, sorted(dropped)


def check_sources(flags, sources_path, ehsc_args):
    """(sources, normcased --ehsc set) of one source list; ValueError naming the
    first problem: an empty list, a missing source, two sources with one object
    name, an --ehsc source not listed, or a unit the flags leave without /EHa."""
    sources = read_list(sources_path)
    if not sources:
        raise ValueError('%s lists no sources' % sources_path)
    missing = [s for s in sources if not os.path.isfile(s)]
    if missing:
        raise ValueError('listed source(s) not found: ' + ', '.join(missing))
    stems = {}
    for s in sources:
        stem = os.path.splitext(os.path.basename(s))[0].lower()
        if stem in stems:
            raise ValueError('two sources would share one object name: %s and %s' % (stems[stem], s))
        stems[stem] = s
    ehsc = set(os.path.normcase(s) for s in ehsc_args)
    unknown = [s for s in ehsc_args if os.path.normcase(s) not in set(os.path.normcase(x) for x in sources)]
    if unknown:
        raise ValueError('--ehsc names a source not in the list: ' + ', '.join(unknown))
    flag_tokens = read_flag_tokens(flags)
    no_eha = [s for s in sources
              if os.path.normcase(s) not in ehsc and not eh_async(flag_tokens)]
    if no_eha:
        raise ValueError('%d source(s) would compile without /EHa (flags file %s), e.g. %s; every unit '
                         'builds /EHa except those named by --ehsc' % (len(no_eha), flags, no_eha[0]))
    return sources, ehsc


def preprocessed_try_count(clang, flags, source, env):
    """__try occurrences in the source as clang-cl /EHsc would see it, headers
    included and comments removed."""
    proc = subprocess.run([clang, '@' + flags, '/EHsc', '/E', source], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    if proc.returncode != 0:
        raise ValueError('preprocessing %s failed: %s' % (source, proc.stderr.decode('mbcs', 'replace')[:300]))
    return len(re.findall(rb'\b__try\b', proc.stdout))


def compile_one(clang, flags, source, obj, ehsc, env, syntax_only=False):
    if syntax_only:
        cmd = [clang, '@' + flags, '/Zs', source]
    else:
        if os.path.exists(obj):
            os.remove(obj)
        cmd = [clang, '@' + flags, '/c', source, '/Fo' + obj]
    if ehsc:
        cmd.insert(2, '/EHsc')
    start = time.time()
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          stdin=subprocess.DEVNULL)
    return source, cmd, proc.returncode, proc.stdout.decode('mbcs', 'replace'), time.time() - start


def log_entry(source, rc, seconds, cmd, output):
    """One source's bytes in a log: its header line, its command, its messages."""
    data = ('=== %s  rc=%d  %.1fs\n%s\n' % (source, rc, seconds, ' '.join(cmd))).encode('mbcs', 'replace')
    if output:
        data += output.replace('\r\n', '\n').encode('mbcs', 'replace')
    return data


def print_failures(failed):
    for s, rc, fresh, errors in failed:
        reason = 'rc=%d' % rc if rc != 0 else 'exit 0 but no fresh object'
        print('FAILED %s (%s, %d error line(s))' % (s, reason, len(errors)))
        for line in errors[:FIRST_ERRORS]:
            print('    ' + line.strip()[:300])


def compile_main(args):
    """The object build: one pass, every source to an object in --objdir."""
    child_env, dropped = clang_child_env()
    if dropped:
        print('clang_compile: left out of clang-cl\'s environment (it would add them to every command line): %s'
              % ', '.join(dropped))
    try:
        check_vs2010_environment()
        jobs = job_count()
        sources, ehsc = check_sources(args.flags, args.sources, args.ehsc)
        if not os.path.isfile(args.clang):
            raise ValueError('clang-cl not found: ' + args.clang)
        ehsc_tries = [(s, preprocessed_try_count(args.clang, args.flags, s, child_env)) for s in args.ehsc]
        seh = ['%s (%d)' % (s, n) for s, n in ehsc_tries if n]
        if seh:
            raise ValueError('--ehsc refused for a source whose preprocessed text holds __try (its handlers '
                             'would be dropped): ' + ', '.join(seh))
    except (OSError, ValueError) as error:
        print('clang_compile.py: ERROR: %s' % error)
        return 1

    os.makedirs(args.objdir, exist_ok=True)
    jobs = max(1, min(jobs, len(sources)))
    start = time.time()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = []
        for s in sources:
            obj = os.path.join(args.objdir, os.path.splitext(os.path.basename(s))[0] + '.obj')
            futures.append(pool.submit(compile_one, args.clang, args.flags, s, obj,
                                       os.path.normcase(s) in ehsc, child_env))
        for fut in concurrent.futures.as_completed(futures):
            source, cmd, rc, output, seconds = fut.result()
            results[source] = (cmd, rc, output, seconds)
    elapsed = time.time() - start

    warnings = 0
    failed = []
    with open(args.log, 'wb') as log:
        for s in sources:
            cmd, rc, output, seconds = results[s]
            obj = cmd[-1][len('/Fo'):]
            fresh = os.path.isfile(obj) and os.path.getmtime(obj) >= start - 2
            lines = output.splitlines()
            warnings += sum(1 for line in lines if WARNING_RE.search(line))
            log.write(log_entry(s, rc, seconds, cmd, output))
            if rc != 0 or not fresh:
                failed.append((s, rc, fresh, [line for line in lines if ERROR_RE.search(line)]))

    print('clang_compile: %d source(s) in %.1f s, %d at a time; %d warning line(s); %d failed; messages: %s'
          % (len(sources), elapsed, jobs, warnings, len(failed), args.log))
    if args.ehsc:
        print('clang_compile: /EHsc for %s (preprocessed __try count: %s)'
              % (', '.join(args.ehsc), ', '.join(str(n) for _, n in ehsc_tries)))
    if failed:
        print_failures(failed)
        return 1
    return 0


class Pass(object):
    """One syntax-only pass and, once the queue is done, its results."""

    def __init__(self, name, flags, sources_path, log, ehsc):
        self.name = name
        self.flags = flags
        self.sources_path = sources_path
        self.log = log
        self.ehsc = list(ehsc)
        self.sources = []
        self.ehsc_set = set()
        self.previous = {}  # source -> seconds, from the pass's previous log
        self.results = {}   # source -> compile_one's result
        self.tries = {}     # --ehsc source -> __try count, or the /E failure text
        self.end = 0.0      # seconds from the queue's start to this pass's last process

    def prefix(self, multi):
        return '%s: ' % self.name if multi else ''


def split_passes(argv):
    """(arguments before the first --pass, one argument list per --pass)."""
    head, groups = [], []
    for tok in argv:
        if tok == '--pass':
            groups.append([])
        elif groups:
            groups[-1].append(tok)
        else:
            head.append(tok)
    return head, groups


def parse_pass(tokens):
    parser = argparse.ArgumentParser(prog='clang_compile.py --pass')
    parser.add_argument('name')
    parser.add_argument('flags')
    parser.add_argument('sources')
    parser.add_argument('log')
    parser.add_argument('--ehsc', action='append', default=[])
    a = parser.parse_args(tokens)
    return Pass(a.name, a.flags, a.sources, a.log, a.ehsc)


def previous_times(log_path):
    """{source: seconds} from a log's header lines; empty when there is no log."""
    try:
        with open(log_path, 'rb') as f:
            text = f.read().decode('mbcs', 'replace')
    except FileNotFoundError:
        return {}
    times = {}
    for line in text.split('\n'):
        m = LOG_HEADER_RE.match(line)
        if m:
            times[m.group(1)] = float(m.group(2))
    return times


def queue_order(passes):
    """Every (pass index, source), longest expected first; ties keep pass and list order."""
    entries = []
    for pi, p in enumerate(passes):
        for li, s in enumerate(p.sources):
            entries.append((pi, li, s, p.previous.get(s), os.path.getsize(s)))
    ratios = sorted(t / size for _, _, _, t, size in entries if t is not None and size > 0)
    scale = ratios[len(ratios) // 2] if ratios else 1.0
    entries.sort(key=lambda e: (-(e[3] if e[3] is not None else e[4] * scale), e[0], e[1]))
    return [(pi, s) for pi, _, s, _, _ in entries]


def load_slots():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import slots
    return slots


def run_task(slots, clang, p, kind, source, env):
    """One clang-cl under one cpu token: an /E check's __try count (or its
    failure text), or compile_one's /Zs result."""
    with slots.cpu_token('clang-cl'):
        if kind == 'E':
            try:
                return preprocessed_try_count(clang, p.flags, source, env)
            except ValueError as error:
                return str(error)
        return compile_one(clang, p.flags, source, None, os.path.normcase(source) in p.ehsc_set, env,
                           syntax_only=True)


def report_pass(p, multi, jobs):
    """Writes p's log and prints its lines; True when the pass failed."""
    for s in p.ehsc:
        if not isinstance(p.tries[s], int):
            print('clang_compile.py: ERROR: %s%s' % (p.prefix(multi), p.tries[s]))
            return True
    seh = ['%s (%d)' % (s, p.tries[s]) for s in p.ehsc if p.tries[s]]
    if seh:
        print('clang_compile.py: ERROR: %s--ehsc refused for a source whose preprocessed text holds __try '
              '(its handlers would be dropped): %s' % (p.prefix(multi), ', '.join(seh)))
        return True

    warnings = 0
    failed = []
    with open(p.log, 'wb') as log:
        for s in p.sources:
            _, cmd, rc, output, seconds = p.results[s]
            lines = output.splitlines()
            warnings += sum(1 for line in lines if WARNING_RE.search(line))
            log.write(log_entry(s, rc, seconds, cmd, output))
            if rc != 0:
                failed.append((s, rc, True, [line for line in lines if ERROR_RE.search(line)]))
    print('clang_compile: %d source(s) in %.1f s, %d at a time; %d warning line(s); %d failed; messages: %s'
          % (len(p.sources), p.end, jobs, warnings, len(failed), p.log))
    if p.ehsc:
        print('clang_compile: /EHsc for %s (preprocessed __try count: %s)'
              % (', '.join(p.ehsc), ', '.join(str(p.tries[s]) for s in p.ehsc)))
    print_failures(failed)
    return bool(failed)


def syntax_main(clang, passes, multi):
    """The syntax-only run: every pass through one queue (see the module text)."""
    child_env, dropped = clang_child_env()
    if dropped:
        print('clang_compile: left out of clang-cl\'s environment (it would add them to every command line): %s'
              % ', '.join(dropped))
    try:
        slots = load_slots()
        check_vs2010_environment()
        jobs = job_count()
        for p in passes:
            try:
                p.sources, p.ehsc_set = check_sources(p.flags, p.sources_path, p.ehsc)
            except (OSError, ValueError) as error:
                raise ValueError(p.prefix(multi) + str(error))
        if not os.path.isfile(clang):
            raise ValueError('clang-cl not found: ' + clang)
        for p in passes:
            p.previous = previous_times(p.log)
        for p in passes:
            if os.path.exists(p.log):
                os.remove(p.log)
        if slots.cpu_held():
            jobs = 1
    except (OSError, ValueError) as error:
        print('clang_compile.py: ERROR: %s' % error)
        return 1

    tasks = [('E', pi, s) for pi, p in enumerate(passes) for s in p.ehsc]
    tasks += [('Z', pi, s) for pi, s in queue_order(passes)]
    jobs = max(1, min(jobs, len(tasks)))
    sys.stdout.flush()
    try:
        with slots.heavy('clang_check'):
            env = slots.child_env(child_env)
            start = time.time()
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                futures = {}
                for kind, pi, s in tasks:
                    futures[pool.submit(run_task, slots, clang, passes[pi], kind, s, env)] = (kind, pi, s)
                for fut in concurrent.futures.as_completed(futures):
                    kind, pi, s = futures[fut]
                    p = passes[pi]
                    if kind == 'E':
                        p.tries[s] = fut.result()
                    else:
                        p.results[s] = fut.result()
                    p.end = max(p.end, time.time() - start)
            elapsed = time.time() - start
    except slots.SlotTimeout:
        return 1
    except ValueError as error:
        print('clang_compile.py: ERROR: %s' % error)
        return 1

    failed = False
    for p in passes:
        failed = report_pass(p, multi, jobs) or failed
    if multi:
        print('clang_compile: %d passes, %d clang-cl run(s) in %.1f s, %d at a time'
              % (len(passes), len(tasks), elapsed, jobs))
    return 1 if failed else 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    head, groups = split_passes(argv)
    single = not groups
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--flags', required=single)
    parser.add_argument('--sources', required=single)
    parser.add_argument('--objdir')
    parser.add_argument('--log', required=single)
    parser.add_argument('--ehsc', action='append', default=[])
    parser.add_argument('--syntax-only', action='store_true')
    args = parser.parse_args(head)
    if single:
        if not args.syntax_only and not args.objdir:
            parser.error('--objdir is required unless --syntax-only')
        if not args.syntax_only:
            return compile_main(args)
        return syntax_main(args.clang, [Pass('', args.flags, args.sources, args.log, args.ehsc)], False)

    if not args.syntax_only:
        parser.error('--pass needs --syntax-only')
    if args.flags or args.sources or args.log or args.ehsc or args.objdir:
        parser.error('--pass replaces --flags, --sources, --log, --ehsc and --objdir')
    passes = [parse_pass(g) for g in groups]
    names = [p.name for p in passes]
    logs = [os.path.normcase(os.path.abspath(p.log)) for p in passes]
    if len(set(names)) != len(names) or len(set(logs)) != len(logs):
        parser.error('two passes share one name or one log')
    return syntax_main(args.clang, passes, True)


if __name__ == '__main__':
    sys.exit(main())

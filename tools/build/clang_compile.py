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
    --sources  a text file, one source path per line (tools\\build\\coresrc.txt)
    --syntax-only  clang-cl /Zs: parse and type-check, write no object, leave
                   --objdir out. Every source is fresh by definition.

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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--flags', required=True)
    parser.add_argument('--sources', required=True)
    parser.add_argument('--objdir')
    parser.add_argument('--log', required=True)
    parser.add_argument('--ehsc', action='append', default=[])
    parser.add_argument('--syntax-only', action='store_true')
    args = parser.parse_args()
    if not args.syntax_only and not args.objdir:
        parser.error('--objdir is required unless --syntax-only')

    child_env, dropped = clang_child_env()
    if dropped:
        print('clang_compile: left out of clang-cl\'s environment (it would add them to every command line): %s'
              % ', '.join(dropped))
    try:
        check_vs2010_environment()
        jobs = job_count()
        sources = read_list(args.sources)
        if not sources:
            raise ValueError('%s lists no sources' % args.sources)
        missing = [s for s in sources if not os.path.isfile(s)]
        if missing:
            raise ValueError('listed source(s) not found: ' + ', '.join(missing))
        stems = {}
        for s in sources:
            stem = os.path.splitext(os.path.basename(s))[0].lower()
            if stem in stems:
                raise ValueError('two sources would share one object name: %s and %s' % (stems[stem], s))
            stems[stem] = s
        ehsc = set(os.path.normcase(s) for s in args.ehsc)
        unknown = [s for s in args.ehsc if os.path.normcase(s) not in set(os.path.normcase(x) for x in sources)]
        if unknown:
            raise ValueError('--ehsc names a source not in the list: ' + ', '.join(unknown))
        flag_tokens = read_flag_tokens(args.flags)
        no_eha = [s for s in sources
                  if os.path.normcase(s) not in ehsc and not eh_async(flag_tokens)]
        if no_eha:
            raise ValueError('%d source(s) would compile without /EHa (flags file %s), e.g. %s; every unit '
                             'builds /EHa except those named by --ehsc' % (len(no_eha), args.flags, no_eha[0]))
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

    if args.objdir:
        os.makedirs(args.objdir, exist_ok=True)
    jobs = max(1, min(jobs, len(sources)))
    start = time.time()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = []
        for s in sources:
            obj = (os.path.join(args.objdir, os.path.splitext(os.path.basename(s))[0] + '.obj')
                  if args.objdir else None)
            futures.append(pool.submit(compile_one, args.clang, args.flags, s, obj,
                                       os.path.normcase(s) in ehsc, child_env, args.syntax_only))
        for fut in concurrent.futures.as_completed(futures):
            source, cmd, rc, output, seconds = fut.result()
            results[source] = (cmd, rc, output, seconds)
    elapsed = time.time() - start

    warnings = 0
    failed = []
    with open(args.log, 'wb') as log:
        for s in sources:
            cmd, rc, output, seconds = results[s]
            if args.syntax_only:
                fresh = True
            else:
                obj = cmd[-1][len('/Fo'):]
                fresh = os.path.isfile(obj) and os.path.getmtime(obj) >= start - 2
            lines = output.splitlines()
            warnings += sum(1 for line in lines if WARNING_RE.search(line))
            log.write(('=== %s  rc=%d  %.1fs\n%s\n' % (s, rc, seconds, ' '.join(cmd))).encode('mbcs', 'replace'))
            if output:
                log.write(output.replace('\r\n', '\n').encode('mbcs', 'replace'))
            if rc != 0 or not fresh:
                failed.append((s, rc, fresh, [line for line in lines if ERROR_RE.search(line)]))

    print('clang_compile: %d source(s) in %.1f s, %d at a time; %d warning line(s); %d failed; messages: %s'
          % (len(sources), elapsed, jobs, warnings, len(failed), args.log))
    if args.ehsc:
        print('clang_compile: /EHsc for %s (preprocessed __try count: %s)'
              % (', '.join(args.ehsc), ', '.join(str(n) for _, n in ehsc_tries)))
    if failed:
        for s, rc, fresh, errors in failed:
            reason = 'rc=%d' % rc if rc != 0 else 'exit 0 but no fresh object'
            print('FAILED %s (%s, %d error line(s))' % (s, reason, len(errors)))
            for line in errors[:FIRST_ERRORS]:
                print('    ' + line.strip()[:300])
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

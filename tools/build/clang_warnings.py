"""Warning classes of the clang checking tier's logs against a committed baseline (Python 3).

Reads <logdir>\\dev.log, prod.log and prof.log as tools\\build\\clang_compile.py writes them:
'=== <source>  rc=<n>  <s>s' per source, then its messages. A warning's class is the first
'-W' name in the last '[...]' of its line, '(none)' without one. Baseline lines are
'<config>\\t<class>\\t<count>', sorted; '#' lines are comments.

A structural problem (a log missing, a source that did not parse, or a log whose source count
does not match its list) fails both subcommands before any baseline comparison or write, so
`write` never records a baseline over a broken run.

<logdir>\\extra.log, when present (a TEST_CLANG_EXTRA run), is read too: its warning classes
count toward DEV's, its sources are not counted against coresrc.txt, and a source of its that
did not parse is a problem. `write` refuses outright while extra.log exists, so a
TEST_CLANG_EXTRA run never writes the baseline.

Usage:
  clang_warnings.py check --logdir DIR --baseline FILE
  clang_warnings.py write --logdir DIR --baseline FILE
check prints 'clang_check: DEV <ok>/<n> PROD <ok>/<n> profiler <ok>/<n> clean, <w> warnings in
<c> classes, none above baseline' and exits 0; otherwise one line per class above its baseline
('clang_check: <config> <class> <count> > <baseline>'), per failed source, or per list whose
entry count differs from the log's, then 'clang_check: FAILED (<n> problem(s))', exit 1.
write prints 'clang_check: baseline written: <c> classes, <w> warnings -> <FILE>' and exits 0.
"""
import argparse, collections, os, re, sys

sys.dont_write_bytecode = True

CONFIGS = (('DEV', 'dev', os.path.join('tools', 'build', 'coresrc.txt')),
           ('PROD', 'prod', os.path.join('tools', 'build', 'coresrc.txt')),
           ('profiler', 'prof', os.path.join('tools', 'build', 'profsrc.txt')))
HEADER_RE = re.compile(r'^=== (\S+)  rc=(-?\d+)')
WARNING_RE = re.compile(r': warning:')
CLASS_RE = re.compile(r'\[(-W[^\],]+)[^\]]*\]\s*$')


def parse_log(path):
    """(sources, sources with rc 0, Counter of class -> warning lines)."""
    with open(path, 'rb') as f:
        text = f.read().decode('mbcs', 'replace')
    sources = []
    ok = []
    counts = collections.Counter()
    for line in text.split('\n'):
        m = HEADER_RE.match(line)
        if m:
            source, rc = m.group(1), int(m.group(2))
            sources.append(source)
            if rc == 0:
                ok.append(source)
            continue
        if WARNING_RE.search(line):
            cm = CLASS_RE.search(line)
            counts[cm.group(1) if cm else '(none)'] += 1
    return sources, ok, counts


def list_count(path):
    """Non-blank, non-'#' lines of a source list."""
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r\n', '\n')
    return len([line for line in text.split('\n') if line.strip() and not line.strip().startswith('#')])


def load_baseline(path):
    """{(config, class): count}."""
    baseline = {}
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            config, cls, count = line.split('\t')
            baseline[(config, cls)] = int(count)
    return baseline


def scan(logdir):
    """(per-config (ok, expected, Counter), structural problem lines) over CONFIGS."""
    data = {}
    problems = []
    for label, key, list_path in CONFIGS:
        log_path = os.path.join(logdir, key + '.log')
        expected = list_count(list_path)
        if not os.path.isfile(log_path):
            problems.append('clang_check: %s log missing: %s' % (label, log_path))
            data[label] = (0, expected, collections.Counter())
            continue
        sources, ok, counts = parse_log(log_path)
        if len(sources) != expected:
            problems.append('clang_check: %s log has %d source(s), %s has %d'
                            % (label, len(sources), list_path, expected))
        for s in sources:
            if s not in ok:
                problems.append('clang_check: %s %s FAILED to parse' % (label, s))
        data[label] = (len(ok), expected, counts)

    extra_path = os.path.join(logdir, 'extra.log')
    if os.path.isfile(extra_path):
        sources, ok, counts = parse_log(extra_path)
        for s in sources:
            if s not in ok:
                problems.append('clang_check: DEV extra %s FAILED to parse' % s)
        ok_dev, expected_dev, counts_dev = data['DEV']
        merged = collections.Counter(counts_dev)
        merged.update(counts)
        data['DEV'] = (ok_dev, expected_dev, merged)
    return data, problems


def main(argv=None):
    """The two subcommands above; returns the exit code."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    for name in ('check', 'write'):
        p = sub.add_parser(name)
        p.add_argument('--logdir', required=True)
        p.add_argument('--baseline', required=True)
    args = parser.parse_args(argv)

    if args.cmd == 'write' and os.path.isfile(os.path.join(args.logdir, 'extra.log')):
        print('clang_check: extra.log present: a TEST_CLANG_EXTRA run never writes the baseline')
        print('clang_check: FAILED (1 problem(s))')
        return 1

    data, problems = scan(args.logdir)
    if problems:
        for p in problems:
            print(p)
        print('clang_check: FAILED (%d problem(s))' % len(problems))
        return 1

    combined = collections.Counter()
    for label, _, _ in CONFIGS:
        _, _, counts = data[label]
        for cls, n in counts.items():
            combined[(label, cls)] += n
    total_warnings = sum(combined.values())
    total_classes = len(combined)

    if args.cmd == 'write':
        with open(args.baseline, 'w', encoding='utf-8', newline='\n') as f:
            for config, cls in sorted(combined):
                f.write('%s\t%s\t%d\n' % (config, cls, combined[(config, cls)]))
        print('clang_check: baseline written: %d classes, %d warnings -> %s'
              % (total_classes, total_warnings, args.baseline))
        return 0

    baseline = load_baseline(args.baseline)
    over = []
    for (config, cls), count in sorted(combined.items()):
        base = baseline.get((config, cls), 0)
        if count > base:
            over.append('clang_check: %s %s %d > %d' % (config, cls, count, base))
    if over:
        for p in over:
            print(p)
        print('clang_check: FAILED (%d problem(s))' % len(over))
        return 1

    ok_dev, n_dev, _ = data['DEV']
    ok_prod, n_prod, _ = data['PROD']
    ok_prof, n_prof, _ = data['profiler']
    print('clang_check: DEV %d/%d PROD %d/%d profiler %d/%d clean, %d warnings in %d classes, '
          'none above baseline' % (ok_dev, n_dev, ok_prod, n_prod, ok_prof, n_prof,
                                   total_warnings, total_classes))
    return 0


if __name__ == '__main__':
    sys.exit(main())

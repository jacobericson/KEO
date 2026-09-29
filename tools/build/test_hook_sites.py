"""Structural test of the hook install sites (unittest, standard library only).

Every manifest row that startPlugin, a module installer or the first navmesh
dispatch installs must be named by exactly one HookInstall( or HookInstallRow(
call under src/ (or by one entry of one array ending `Rows` that is passed to
such a call); no call may name an id the manifest lacks, or a render or gui
row, which install outside the manifest. InstallHooks' step list must name
each step once. The one NmPoolDecide( call must read, through
HookRowInstalled(, exactly the rows whose caps carry HOOK_CAP_WORKER_POOL.

Every row use, and every entry of a passed `Rows` array, must sit under
conditions that imply its row's `#if`/`#ifdef` block; the one `NmPoolDecide(`
call must sit under no open directive. The step list is still read as text, so
a step under the wrong compile gate is not seen.
"""
import collections, os, re, subprocess, sys, unittest
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ROWS_PATH = 'src/plugin/hook_manifest_rows.inc'
STEPS_PATH = 'src/plugin/hook_manifest.cpp'
POOL_PATH = 'src/navmesh/jobs/nm_dispatch.cpp'
POOL_CAP = 'HOOK_CAP_WORKER_POOL'

INSTALLED_BY_MANIFEST = ('HOOK_BY_STARTUP', 'HOOK_BY_MODULE', 'HOOK_BY_LAZY')
OUTSIDE_MANIFEST = ('HOOK_BY_RENDER', 'HOOK_BY_GUI')


def strip_comments(text, blank_literals=False):
    """Removes // and /* */ comments and keeps every newline in place. String
    and character literals stay whole, or with blank_literals become empty."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c if blank_literals else text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def parse_rows(text):
    """The HOOK_ROW(id, "name", rva, kind, installer, want, caps, ...) rows, a
    row's continuation lines joined to it, in list order."""
    rows = []
    pending = None
    for line in strip_comments(text).split('\n'):
        if pending is None:
            if not re.match(r'\s*HOOK_ROW\s*\(', line):
                continue
            pending = line
        else:
            pending += ' ' + line.strip()
        if pending.count('(') > pending.count(')'):
            continue
        m = re.match(r'\s*HOOK_ROW\s*\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
                     r'\s*(\w+)\s*,\s*(\w+)\s*,\s*([\w\s|]+?)\s*,', pending)
        if not m:
            raise ValueError('unparsed row: ' + pending.strip())
        rows.append({'id': m.group(1), 'name': m.group(2), 'rva': m.group(3),
                     'kind': m.group(4), 'installer': m.group(5), 'want': m.group(6),
                     'caps': m.group(7)})
        pending = None
    if pending is not None:
        raise ValueError('unterminated row: ' + pending.strip())
    return rows


def find_calls(files):
    """(id, path) for the first argument of every HookInstall(/HookInstallRow(
    call, and for every HOOK_ id in the initialiser of an array ending `Rows`
    that a call is passed. files maps a path to its text."""
    calls = []
    for path in sorted(files):
        text = strip_comments(files[path], blank_literals=True)
        for m in re.finditer(r'\bHookInstall(?:Row)?\s*\(\s*(HOOK_\w+)', text):
            calls.append((m.group(1), path))
        passed = set(re.findall(r'\bHookInstall(?:Row)?\s*\(\s*(\w+Rows)\s*\[', text))
        for m in re.finditer(r'\b(\w+Rows)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}', text):
            if m.group(1) in passed:
                for rid in re.findall(r'\bHOOK_\w+', m.group(2)):
                    calls.append((rid, path))
    return calls


def find_steps(text):
    """The names in kInstallSteps[]'s initialiser, preprocessor lines skipped."""
    text = strip_comments(text, blank_literals=True)
    m = re.search(r'\bkInstallSteps\s*\[\s*\]\s*\)\s*\([^)]*\)\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        m = re.search(r'\bkInstallSteps\s*\[\s*\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        return []
    body = '\n'.join(l for l in m.group(1).split('\n') if not l.strip().startswith('#'))
    return [s.strip().lstrip('&').strip() for s in body.split(',') if s.strip()]


def check(rows, calls, steps=()):
    """The problem lines; none when every rule holds."""
    problems = []
    by_id = dict((r['id'], r) for r in rows)
    counts = collections.Counter(rid for rid, _ in calls)
    for r in rows:
        if r['installer'] not in INSTALLED_BY_MANIFEST:
            continue
        n = counts.get(r['id'], 0)
        if n == 0:
            problems.append('test_hook_sites: %s installed by no call' % r['id'])
        elif n > 1:
            problems.append('test_hook_sites: %s installed by %d calls' % (r['id'], n))
    for rid in sorted(counts):
        if rid not in by_id:
            problems.append('test_hook_sites: %s names no manifest row' % rid)
        elif by_id[rid]['installer'] in OUTSIDE_MANIFEST:
            problems.append('test_hook_sites: %s is a render or gui row, installed outside the manifest'
                            % rid)
    step_counts = collections.Counter(steps)
    for name in sorted(step_counts):
        if step_counts[name] > 1:
            problems.append('test_hook_sites: step %s listed %d times' % (name, step_counts[name]))
    return problems


def check_pool_inputs(rows, text):
    """The problem lines for the pool decision's inputs: the HookRowInstalled(
    ids in the one NmPoolDecide( call's argument list must be exactly the rows
    whose caps carry HOOK_CAP_WORKER_POOL, and the call itself must sit under
    no open preprocessor directive."""
    text = strip_comments(text, blank_literals=True)
    starts = [m.end() for m in re.finditer(r'\bNmPoolDecide\s*\(', text)]
    if len(starts) != 1:
        return ['test_hook_sites: %d NmPoolDecide calls' % len(starts)]
    problems = []
    call_line = text.count('\n', 0, starts[0]) + 1
    if directive_conditions(text)[call_line - 1]:
        problems.append('test_hook_sites: NmPoolDecide called under an open preprocessor directive (line %d)'
                        % call_line)
    i, depth = starts[0], 1
    while i < len(text) and depth:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
        i += 1
    args = text[starts[0]:i - 1]
    read_ids = set(re.findall(r'\bHookRowInstalled\s*\(\s*(HOOK_\w+)', args))
    cap_ids = set(r['id'] for r in rows
                  if POOL_CAP in [c.strip() for c in r['caps'].split('|')])
    for rid in sorted(read_ids - cap_ids):
        problems.append('test_hook_sites: NmPoolDecide reads %s, which carries no %s' % (rid, POOL_CAP))
    for rid in sorted(cap_ids - read_ids):
        problems.append('test_hook_sites: %s row %s not read by NmPoolDecide' % (POOL_CAP, rid))
    return problems


def _normalize_cond(expr):
    """An #if/#elif expression, whitespace collapsed; a defined(X)/defined X
    spelling becomes defined(X)."""
    expr = expr.strip()
    m = re.match(r'defined\s*\(\s*(\w+)\s*\)\s*$', expr)
    if not m:
        m = re.match(r'defined\s+(\w+)\s*$', expr)
    if m:
        return 'defined(%s)' % m.group(1)
    expr = re.sub(r'\s+', '', expr)
    while _outer_parens(expr):
        expr = expr[1:-1]
    return expr


def _outer_parens(expr):
    """Whether expr is one parenthesised group: (A), but not (A)&&(B)."""
    if not (expr.startswith('(') and expr.endswith(')')):
        return False
    depth = 0
    for i, c in enumerate(expr):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if depth == 0 and i < len(expr) - 1:
            return False
    return True


def directive_conditions(text):
    """The list of #if/#ifdef/#ifndef conditions open at each 1-based line: an
    #ifdef/#ifndef/#if defined(...) normalise to one spelling, an #elif
    replaces its frame's condition and an #else negates it (defined(X) and
    !defined(X) swap). A directive continued with a trailing
    backslash is read whole at its first line; its continuation lines carry
    the conditions the whole directive leaves open."""
    stack = []
    result = []
    lines = strip_comments(text, blank_literals=True).split('\n')
    i = 0
    while i < len(lines):
        body = lines[i].strip()
        extra = 0
        if body.startswith('#'):
            while body.endswith('\\') and i + extra + 1 < len(lines):
                extra += 1
                body = body[:-1] + ' ' + lines[i + extra].strip()
            body = body[1:].strip()
            m = re.match(r'ifdef\s+(\w+)', body)
            if m:
                stack.append('defined(%s)' % m.group(1))
            else:
                m = re.match(r'ifndef\s+(\w+)', body)
                if m:
                    stack.append('!defined(%s)' % m.group(1))
                else:
                    m = re.match(r'if\b\s*(.*)', body)
                    if m:
                        stack.append(_normalize_cond(m.group(1)))
                    else:
                        m = re.match(r'elif\b\s*(.*)', body)
                        if m:
                            if stack:
                                stack[-1] = _normalize_cond(m.group(1))
                        elif re.match(r'else\b', body):
                            if stack:
                                top = stack[-1]
                                if re.match(r'!defined\(\w+\)$', top):
                                    stack[-1] = top[1:]
                                elif re.match(r'defined\(\w+\)$', top):
                                    stack[-1] = '!' + top
                                else:
                                    stack[-1] = '!(%s)' % top
                        elif re.match(r'endif\b', body):
                            if stack:
                                stack.pop()
        for _ in range(extra + 1):
            result.append(list(stack))
        i += extra + 1
    return result


def row_conditions(text):
    """{id: conditions} for every HOOK_ROW( in a manifest .inc, taken at the
    row's first line."""
    conds = directive_conditions(text)
    result = {}
    pending = None
    start = None
    for i, line in enumerate(strip_comments(text).split('\n')):
        if pending is None:
            if not re.match(r'\s*HOOK_ROW\s*\(', line):
                continue
            pending = line
            start = i
        else:
            pending += ' ' + line.strip()
        if pending.count('(') > pending.count(')'):
            continue
        m = re.match(r'\s*HOOK_ROW\s*\(\s*(\w+)\s*,', pending)
        if m:
            result[m.group(1)] = conds[start]
        pending = None
    return result


def find_uses(files):
    """(id, path, line, conditions) for every HookInstall(/HookInstallRow(/
    HookRowWanted(/HookRowInstalled( call naming a HOOK_ id, and for every
    HOOK_ id in the initialiser of an array ending `Rows` that a call is passed
    (the same rule find_calls uses), taken at that id's own line."""
    uses = []
    for path in sorted(files):
        raw = files[path]
        text = strip_comments(raw, blank_literals=True)
        conds = directive_conditions(raw)
        for m in re.finditer(r'\b(?:HookInstall|HookInstallRow|HookRowWanted|HookRowInstalled)\s*\(\s*(HOOK_\w+)', text):
            line = text.count('\n', 0, m.start()) + 1
            uses.append((m.group(1), path, line, conds[line - 1]))
        passed = set(re.findall(r'\bHookInstall(?:Row)?\s*\(\s*(\w+Rows)\s*\[', text))
        for m in re.finditer(r'\b(\w+Rows)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}', text):
            if m.group(1) not in passed:
                continue
            for rm in re.finditer(r'\bHOOK_\w+', m.group(2)):
                idx = m.start(2) + rm.start()
                line = text.count('\n', 0, idx) + 1
                uses.append((rm.group(0), path, line, conds[line - 1]))
    return uses


def _cond_implies(use_cond, row_cond):
    """Whether a use's open condition satisfies one of its row's conditions."""
    if use_cond == row_cond:
        return True
    mu = re.match(r'^(\w+)>=(\d+)$', use_cond)
    mr = re.match(r'^(\w+)>=(\d+)$', row_cond)
    if mu and mr and mu.group(1) == mr.group(1):
        return int(mu.group(2)) >= int(mr.group(2))
    return False


def check_conditions(row_conds, uses):
    """The problem lines for a use whose site does not sit under every one of
    its row's #if/#ifdef conditions. An id with no manifest row is skipped:
    check() already reports it for an install call."""
    problems = []
    for rid, path, line, use_conds in uses:
        conds = row_conds.get(rid)
        if conds is None:
            continue
        if not all(any(_cond_implies(uc, c) for uc in use_conds) for c in conds):
            problems.append("test_hook_sites: %s used at %s:%d outside its row's #if block"
                            % (rid, path, line))
    return problems


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def real_tree_problems():
    out = subprocess.check_output(['git', 'ls-files', 'src'], cwd=ROOT, universal_newlines=True)
    paths = [p for p in out.split('\n') if p.endswith(('.cpp', '.h'))]
    files = dict((p, read(p)) for p in paths)
    rows = parse_rows(read(ROWS_PATH))
    calls = find_calls(files)
    steps = find_steps(read(STEPS_PATH))
    problems = []
    if not rows:
        problems.append('test_hook_sites: no HOOK_ROW in %s' % ROWS_PATH)
    if not steps:
        problems.append('test_hook_sites: no kInstallSteps initialiser in %s' % STEPS_PATH)
    problems += check(rows, calls, steps)
    problems += check_pool_inputs(rows, read(POOL_PATH))
    problems += check_conditions(row_conditions(read(ROWS_PATH)), find_uses(files))
    return problems


ROWS = '''
	// --- a section ---
	HOOK_ROW(HOOK_A, "a", RVA_A, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
	HOOK_ROW(HOOK_B, "b", RVA_B, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#ifdef ZONEOPT_DEBUG
	HOOK_ROW(HOOK_P1, "p1", RVA_P1, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
	HOOK_ROW(HOOK_P2, "p2", RVA_P2, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#endif
	HOOK_ROW(HOOK_L, "l", RVA_L, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
	HOOK_ROW(HOOK_R, "r", RVA_R, HOOK_DIAGNOSTIC, HOOK_BY_RENDER, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
	HOOK_ROW(HOOK_G, "g", RVA_G, HOOK_DIAGNOSTIC, HOOK_BY_GUI, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
'''

HELPER = '''
// The helper names the call in its comment: HookInstallRow(HOOK_R, ...).
const char* HookInstallRow(HookRowId id, void* detour, void** orig, int* installed,
                           bool reverify)
{
	return NULL;
}
'''

STARTUP = '''
static void InstallA(int* installed, int*)
{
	if (HookInstallRow(HOOK_A, hook_a, (void**)&orig_a, installed, false) == NULL)
		LogMsg("a installed");
}
'''

MODULE = '''
static const HookRowId kPushRows[2] =
{
	HOOK_P1, HOOK_P2
};
void InstallB(int* installed, int*)
{
	const char* why = HookInstallRow(HOOK_B, hook_b, (void**)&orig_b, installed, true);
	for (int k = 0; k < 2; ++k)
		HookInstallRow(kPushRows[k], kPushDetours[k], (void**)&s_orig[k], installed, true);
	/* a comment: HookInstallRow(HOOK_B, ...) */
	LogMsg("HookInstallRow(HOOK_B) in a string is not a call");
}
'''

LAZY = '''
static void InstallLazy()
{
	if (HookInstallRow(HOOK_L, (void*)hook_l, (void**)&orig_l, NULL, true) == NULL)
		LogMsgDeferrable("l: installed");
}
'''

STEPS = '''
static void (*const kInstallSteps[])(int*, int*) =
{
	InstallA,
	// InstallA in a comment is not an entry
#ifdef ZONEOPT_DEBUG
	InstallB,
#endif
	StepC,
};
'''


TYPED = '''
static const HookRowId kRows[2] = { HOOK_P1, HOOK_P2 };
void InstallTyped(int* i)
{
	HookInstall(HOOK_A, h, &o, i, true);
	for (int k = 0; k < 2; ++k)
		HookInstall(kRows[k], kDetours[k], &s_orig[k], i, true);
	/* a comment: HookInstall(HOOK_B, h, &o, i, true) */
	LogMsg("HookInstall(HOOK_B) in a string is not a call");
}
'''

FORWARDER = '''
template <typename Fn>
inline const char* HookInstall(HookRowId id, Fn detour, Fn* orig, int* installed, bool reverify)
{
	return HookInstallRow(id, (void*)detour, (void**)orig, installed, reverify);
}
'''


def clean_files():
    return {'helper.cpp': HELPER, 'startup.cpp': STARTUP, 'module.cpp': MODULE, 'lazy.cpp': LAZY}


class ParseTests(unittest.TestCase):
    def test_rows_joined_and_read(self):
        rows = parse_rows(ROWS)
        self.assertEqual([r['id'] for r in rows],
                         ['HOOK_A', 'HOOK_B', 'HOOK_P1', 'HOOK_P2', 'HOOK_L', 'HOOK_R', 'HOOK_G'])
        self.assertEqual(rows[0]['name'], 'a')
        self.assertEqual(rows[1]['installer'], 'HOOK_BY_MODULE')
        self.assertEqual(rows[4]['want'], 'HOOK_WANT_UNCOUNTED')
        self.assertEqual(rows[5]['caps'], 'HOOK_CAP_NONE')

    def test_calls_skip_comments_strings_and_the_helper(self):
        calls = find_calls(clean_files())
        self.assertEqual(sorted(rid for rid, _ in calls),
                         ['HOOK_A', 'HOOK_B', 'HOOK_L', 'HOOK_P1', 'HOOK_P2'])

    def test_rows_array_counts_only_when_passed(self):
        files = {'m.cpp': 'static const HookRowId kOtherRows[1] = { HOOK_A };\n'}
        self.assertEqual(find_calls(files), [])

    def test_steps_skip_preprocessor_and_comments(self):
        self.assertEqual(find_steps(STEPS), ['InstallA', 'InstallB', 'StepC'])

    def test_typed_install_calls_counted(self):
        self.assertEqual(sorted(rid for rid, _ in find_calls({'typed.cpp': TYPED})),
                         ['HOOK_A', 'HOOK_P1', 'HOOK_P2'])

    def test_forwarder_names_no_row(self):
        files = {'hook_manifest.h': FORWARDER}
        self.assertEqual(find_calls(files), [])
        self.assertEqual(find_uses(files), [])


class CheckTests(unittest.TestCase):
    def problems(self, files, steps=STEPS):
        return check(parse_rows(ROWS), find_calls(files), find_steps(steps))

    def test_clean_set_passes(self):
        self.assertEqual(self.problems(clean_files()), [])

    def test_missing_call_fails(self):
        files = clean_files()
        files['lazy.cpp'] = 'static void InstallLazy() {}\n'
        self.assertEqual(self.problems(files), ['test_hook_sites: HOOK_L installed by no call'])

    def test_missing_array_entry_fails(self):
        files = clean_files()
        files['module.cpp'] = MODULE.replace('HOOK_P1, HOOK_P2', 'HOOK_P1')
        self.assertEqual(self.problems(files), ['test_hook_sites: HOOK_P2 installed by no call'])

    def test_doubled_call_fails(self):
        files = clean_files()
        files['second.cpp'] = STARTUP
        self.assertEqual(self.problems(files), ['test_hook_sites: HOOK_A installed by 2 calls'])

    def test_unknown_id_fails(self):
        files = clean_files()
        files['extra.cpp'] = 'void X(int* i) { HookInstallRow(HOOK_NOPE, h, (void**)&o, i, true); }\n'
        self.assertEqual(self.problems(files), ['test_hook_sites: HOOK_NOPE names no manifest row'])

    def test_render_row_named_by_a_call_fails(self):
        files = clean_files()
        files['render.cpp'] = 'void X(int* i) { HookInstallRow(HOOK_R, h, (void**)&o, i, true); }\n'
        self.assertEqual(self.problems(files),
                         ['test_hook_sites: HOOK_R is a render or gui row, installed outside the manifest'])

    def test_gui_row_named_by_a_call_fails(self):
        files = clean_files()
        files['gui.cpp'] = 'void X(int* i) { HookInstallRow(HOOK_G, h, (void**)&o, i, true); }\n'
        self.assertEqual(self.problems(files),
                         ['test_hook_sites: HOOK_G is a render or gui row, installed outside the manifest'])

    def test_typed_spelling_passes(self):
        files = clean_files()
        files['startup.cpp'] = STARTUP.replace('HookInstallRow(HOOK_A, hook_a, (void**)&orig_a',
                                               'HookInstall(HOOK_A, hook_a, &orig_a')
        self.assertEqual(self.problems(files), [])

    def test_call_doubled_across_spellings_fails(self):
        files = clean_files()
        files['typed.cpp'] = 'void X(int* i) { HookInstall(HOOK_A, h, &o, i, true); }\n'
        self.assertEqual(self.problems(files), ['test_hook_sites: HOOK_A installed by 2 calls'])

    def test_step_listed_twice_fails(self):
        steps = STEPS.replace('\tStepC,', '\tStepC,\n\tInstallA,')
        self.assertEqual(self.problems(clean_files(), steps),
                         ['test_hook_sites: step InstallA listed 2 times'])


POOL_ROWS = ROWS.replace(
    'HOOK_A, "a", RVA_A, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE',
    'HOOK_A, "a", RVA_A, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_WORKER_POOL').replace(
    'HOOK_L, "l", RVA_L, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE',
    'HOOK_L, "l", RVA_L, HOOK_FATAL, HOOK_BY_LAZY, HOOK_WANT_UNCOUNTED, HOOK_CAP_NONE | HOOK_CAP_WORKER_POOL')

POOL_SITE = '''
	// NmPoolDecide(a, HookRowInstalled(HOOK_B), b) in a comment is not a call
	NmPoolDecision d = NmPoolDecide(
		InterlockedCompareExchange(&done, 0, 0) == 2,
		HookRowInstalled(HOOK_A),
		HookRowInstalled(HOOK_L));
	LogMsg("NmPoolDecide(HookRowInstalled(HOOK_B)) in a string is not a call");
'''


class PoolInputTests(unittest.TestCase):
    def problems(self, site):
        return check_pool_inputs(parse_rows(POOL_ROWS), site)

    def test_caps_read(self):
        caps = dict((r['id'], r['caps']) for r in parse_rows(POOL_ROWS))
        self.assertEqual(caps['HOOK_A'], 'HOOK_CAP_WORKER_POOL')
        self.assertEqual(caps['HOOK_L'], 'HOOK_CAP_NONE | HOOK_CAP_WORKER_POOL')

    def test_clean_pair_passes(self):
        self.assertEqual(self.problems(POOL_SITE), [])

    def test_row_without_the_bit_fails(self):
        site = POOL_SITE.replace('HookRowInstalled(HOOK_A)', 'HookRowInstalled(HOOK_B)')
        self.assertEqual(self.problems(site),
                         ['test_hook_sites: NmPoolDecide reads HOOK_B, which carries no HOOK_CAP_WORKER_POOL',
                          'test_hook_sites: HOOK_CAP_WORKER_POOL row HOOK_A not read by NmPoolDecide'])

    def test_capability_row_not_read_fails(self):
        site = POOL_SITE.replace('HookRowInstalled(HOOK_L)', 'true')
        self.assertEqual(self.problems(site),
                         ['test_hook_sites: HOOK_CAP_WORKER_POOL row HOOK_L not read by NmPoolDecide'])

    def test_two_calls_fail(self):
        self.assertEqual(self.problems(POOL_SITE + POOL_SITE),
                         ['test_hook_sites: 2 NmPoolDecide calls'])

    def test_no_call_fails(self):
        self.assertEqual(self.problems('int x;\n'), ['test_hook_sites: 0 NmPoolDecide calls'])

    def test_call_under_open_directive_fails(self):
        site = '#ifdef X\n' + POOL_SITE + '#endif\n'
        call_line = site.split('\n').index('\tNmPoolDecision d = NmPoolDecide(') + 1
        self.assertEqual(self.problems(site),
                         ['test_hook_sites: NmPoolDecide called under an open preprocessor directive (line %d)'
                          % call_line])


COND_ROWS = '''
	HOOK_ROW(HOOK_A, "a", RVA_A, HOOK_FATAL, HOOK_BY_STARTUP, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#if ZONEHAND_STEP >= 2
	HOOK_ROW(HOOK_S2, "s2", RVA_S2, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#endif
#ifdef ZONEOPT_DEBUG
	HOOK_ROW(HOOK_D, "d", RVA_D, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#endif
'''


class ConditionTests(unittest.TestCase):
    def row_conds(self):
        return row_conditions(COND_ROWS)

    def uses(self, site, path='site.cpp'):
        return find_uses({path: site})

    def check_site(self, site):
        return check_conditions(self.row_conds(), self.uses(site))

    def test_row_conditions(self):
        rc = self.row_conds()
        self.assertEqual(rc['HOOK_A'], [])
        self.assertEqual(rc['HOOK_S2'], ['ZONEHAND_STEP>=2'])
        self.assertEqual(rc['HOOK_D'], ['defined(ZONEOPT_DEBUG)'])

    def test_clean_set_passes(self):
        site = '''#if ZONEHAND_STEP >= 2
	HookRowWanted(HOOK_S2);
#endif
#if ZONEHAND_STEP >= 3
	HookRowWanted(HOOK_S2);
#endif
#if defined(ZONEOPT_DEBUG)
	HookRowWanted(HOOK_D);
#endif
HookRowWanted(HOOK_A);
'''
        self.assertEqual(self.check_site(site), [])

    def test_c1_shape_fails(self):
        site = 'HookRowWanted(HOOK_S2);\n'
        self.assertEqual(self.check_site(site),
                         ["test_hook_sites: HOOK_S2 used at site.cpp:1 outside its row's #if block"])

    def test_use_in_else_arm_fails(self):
        site = '''#if ZONEHAND_STEP >= 2
#else
HookInstallRow(HOOK_S2, hook_s2, (void**)&orig_s2, installed, true);
#endif
'''
        self.assertEqual(self.check_site(site),
                         ["test_hook_sites: HOOK_S2 used at site.cpp:3 outside its row's #if block"])

    def test_typed_use_in_else_arm_fails(self):
        site = '''#if ZONEHAND_STEP >= 2
#else
HookInstall(HOOK_S2, hook_s2, &orig_s2, installed, true);
#endif
'''
        self.assertEqual(self.check_site(site),
                         ["test_hook_sites: HOOK_S2 used at site.cpp:3 outside its row's #if block"])

    def test_use_under_lower_step_fails(self):
        site = '''#if ZONEHAND_STEP >= 1
HookRowWanted(HOOK_S2);
#endif
'''
        self.assertEqual(self.check_site(site),
                         ["test_hook_sites: HOOK_S2 used at site.cpp:2 outside its row's #if block"])

    def test_use_outside_ifdef_fails(self):
        site = 'HookRowInstalled(HOOK_D);\n'
        self.assertEqual(self.check_site(site),
                         ["test_hook_sites: HOOK_D used at site.cpp:1 outside its row's #if block"])

    def test_array_path_outside_ifdef_fails(self):
        problems = check_conditions(row_conditions(ROWS), find_uses(clean_files()))
        push_line = MODULE.split('\n').index('\tHOOK_P1, HOOK_P2') + 1
        self.assertEqual(sorted(problems), sorted([
            "test_hook_sites: HOOK_P1 used at module.cpp:%d outside its row's #if block" % push_line,
            "test_hook_sites: HOOK_P2 used at module.cpp:%d outside its row's #if block" % push_line,
        ]))

    def test_use_in_elif_arm_passes(self):
        site = '''#if defined(SOMETHING_ELSE)
#elif ZONEHAND_STEP >= 2
HookRowWanted(HOOK_S2);
#endif
'''
        self.assertEqual(self.check_site(site), [])

    def test_use_under_ifndef_passes(self):
        rows = COND_ROWS + '''#ifndef ZONEOPT_DEBUG
	HOOK_ROW(HOOK_P, "p", RVA_P, HOOK_DIAGNOSTIC, HOOK_BY_MODULE, HOOK_WANT_ALWAYS, HOOK_CAP_NONE,
	         0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF)
#endif
'''
        site = '''#ifndef ZONEOPT_DEBUG
HookRowWanted(HOOK_P);
#endif
#ifdef ZONEOPT_DEBUG
#else
HookRowWanted(HOOK_P);
#endif
#if !defined(ZONEOPT_DEBUG)
HookRowWanted(HOOK_P);
#endif
'''
        self.assertEqual(check_conditions(row_conditions(rows), self.uses(site)), [])

    def test_use_in_else_of_ifndef_passes(self):
        site = '''#ifndef ZONEOPT_DEBUG
#else
HookRowWanted(HOOK_D);
#endif
'''
        self.assertEqual(self.check_site(site), [])

    def test_array_entry_under_its_block_passes(self):
        site = '''static const HookRowId kModRows[] = {
#ifdef ZONEOPT_DEBUG
	HOOK_D,
#endif
#if ZONEHAND_STEP >= 3
	HOOK_S2,
#endif
	HOOK_A };
void Install(int i) { HookInstallRow(kModRows[i], h, (void**)&o, installed, true); }
'''
        uses = self.uses(site)
        self.assertEqual(sorted(u[0] for u in uses), ['HOOK_A', 'HOOK_D', 'HOOK_S2'])
        self.assertEqual(self.check_site(site), [])

    def test_parenthesised_directives_pass_and_keep_the_stack(self):
        site = '''#if(ZONEHAND_STEP >= 2)
HookRowWanted(HOOK_S2);
#endif
#if defined(ZONEOPT_DEBUG)
#if(ZONEHAND_STEP >= 1)
#elif(ZONEHAND_STEP >= 2)
HookRowWanted(HOOK_S2);
#endif
HookRowWanted(HOOK_D);
#endif
'''
        self.assertEqual(self.check_site(site), [])

    def test_parenthesised_if_endif_keeps_the_enclosing_frame(self):
        site = '''#ifdef ZONEOPT_DEBUG
#if!defined(SOMETHING_ELSE)
#endif
HookRowWanted(HOOK_D);
#endif
'''
        self.assertEqual(self.check_site(site), [])

    def test_continued_directive_read_whole(self):
        site = '''#if ZONEHAND_STEP \\
    >= 2
HookRowWanted(HOOK_S2);
#endif
HookRowWanted(HOOK_A);
'''
        conds = directive_conditions(site)
        self.assertEqual(len(conds), len(site.split('\n')))
        self.assertEqual(conds[2], ['ZONEHAND_STEP>=2'])
        self.assertEqual(conds[4], [])
        self.assertEqual(self.check_site(site), [])

    def test_unknown_id_gives_no_condition_problem(self):
        site = 'HookInstallRow(HOOK_NOPE, h, (void**)&o, i, true);\n'
        self.assertEqual(self.check_site(site), [])


class RealTreeTest(unittest.TestCase):
    def test_real_tree(self):
        problems = real_tree_problems()
        if problems:
            self.fail('\n' + '\n'.join(problems))


if __name__ == '__main__':
    unittest.main()

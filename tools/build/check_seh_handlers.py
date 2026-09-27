"""Check that every function holding a __try carries its exception handler in
a built DLL's unwind table (Python 3, stdlib only, plus llvm-readobj and
llvm-pdbutil from LLVM).

RtlDispatchException finds a function's handler only through its
RUNTIME_FUNCTION's UNWIND_INFO, and only when the flags carry
UNW_FLAG_EHANDLER (for __except) or UNW_FLAG_UHANDLER (for __finally); the
handler then consults the function's scope table, which lists the address
ranges each __try covers. Under /EHsc clang does not treat a load or store
as able to fault. A __try whose body makes no call gets no handler at all
(in a leaf function, no unwind entry either). A __try whose body also makes
a call does get __C_specific_handler, but its scope covers only the call, so
a fault on the guarded load still escapes. Only /EHa (-fasync-exceptions)
makes the scope cover every guarded instruction; that is the guarantee, and
tools\\build\\clang_compile.py refuses any unit that would build without it.

What this check proves is narrower: every __try function carries the
handler with the right flag, read from what the linker actually wrote. It
does not decode the scope tables, so it does not prove that each guarded
instruction lies inside a scope. It catches the no-handler case under /EHsc,
not the call-plus-load case.

The PDB must be this DLL's: the GUID and age in the DLL's CodeView (RSDS)
record must equal the PDB's, or nothing is judged and the check fails.

Three views, cross-checked:
  1. Unwind table (llvm-readobj --unwind DLL): every RUNTIME_FUNCTION, its
     flags and its handler, the handler named from the PDB's publics.
  2. PDB (llvm-pdbutil dump --symbols/--publics): every procedure, its
     address and size, its S_FRAMEPROC flags ("has seh" marks a function the
     compiler built with SEH, including a __try inlined into it) and the
     functions inlined into it.
  3. Sources: every function whose body holds a __try, found by a brace scan
     of the comment- and string-stripped text, with whether it uses __except,
     __finally or both.
A source function is matched to the PDB by name within its own module (the
object named after its source), either as a procedure of its own or as an
inlinee of procedures in that module; each of those must carry
__C_specific_handler with the flag its __except/__finally needs. Every
"has seh" procedure must carry it too, whether or not a source maps to it.
A source __try function found neither as a procedure nor as an inlinee in
its module fails the check unless --allow-absent names it (code this build
leaves out, such as DEV-only functions in PROD), and so does a module that
holds __try but has no procedure in the PDB at all (built without /Z7, or
not linked).

Usage:
  python tools\\build\\check_seh_handlers.py <dll> [--pdb <pdb>]
         [--sources <list.txt>] [--src <file> ...] [--expect <function> ...]
         [--allow-absent <function> ...] [--list]
    --pdb      default: the DLL's path with .pdb
    --sources  a source list, one path per line (default
               tools\\build\\coresrc.txt when no --src is given)
    --src      a single source (repeatable), added to the list
    --expect   a function that must be found and carry its handler
               (repeatable); a spot check by name
    --allow-absent  a __try function this build is known to leave out
               (repeatable); naming one no source defines fails the check
    --list     print every source function holding __try, with its status
Environment: LLVM_BIN (default C:\\Program Files\\LLVM\\bin).

Exit 0 when nothing is flagged and every --expect passes; 1 otherwise.
"""
import argparse
import os
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kenshilib'))
from check_imports import PE  # noqa: E402

SEH_HANDLER = '__C_specific_handler'
UNW_EHANDLER = 0x1
UNW_UHANDLER = 0x2
NOT_FUNCTION_NAMES = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', '__declspec',
                      '__except', '__try', '__finally', 'decltype', 'alignof', '__alignof', 'throw',
                      'static_assert', 'defined', '__pragma'}


def llvm_tool(name):
    return os.path.join(os.environ.get('LLVM_BIN') or r'C:\Program Files\LLVM\bin', name)


def run(tool, args):
    proc = subprocess.run([llvm_tool(tool)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError('%s %s failed: %s' % (tool, ' '.join(args), proc.stderr.decode('mbcs', 'replace')[:500]))
    return proc.stdout.decode('utf-8', 'replace').splitlines()


# ---------------------------------------------------------------- unwind table

def read_unwind(dll, image_base):
    """{start rva: (end rva, flags, handler rva or None)}"""
    entries = {}
    cur = None
    for line in run('llvm-readobj.exe', ['--unwind', dll]):
        s = line.strip()
        if s == 'RuntimeFunction {':
            cur = {'start': None, 'end': None, 'flags': 0, 'handler': None}
            continue
        if cur is None:
            continue
        m = re.match(r'StartAddress: \((0x[0-9A-Fa-f]+)\)', s)
        if m and cur['start'] is None:
            cur['start'] = int(m.group(1), 16) - image_base
            continue
        m = re.match(r'EndAddress: \((0x[0-9A-Fa-f]+)\)', s)
        if m and cur['end'] is None:
            cur['end'] = int(m.group(1), 16) - image_base
            continue
        m = re.match(r'Flags \[ \((0x[0-9A-Fa-f]+)\)', s)
        if m:
            cur['flags'] = int(m.group(1), 16)
            continue
        m = re.match(r'Handler: \((0x[0-9A-Fa-f]+)\)', s)
        if m:
            cur['handler'] = int(m.group(1), 16) - image_base
            continue
        if s.startswith('UnwindInfoAddress') or s.startswith('UnwindInfo {'):
            continue
        if s == '}' and cur['start'] is not None and cur['end'] is not None and cur['start'] not in entries:
            entries[cur['start']] = (cur['end'], cur['flags'], cur['handler'])
    return entries


# ------------------------------------------------------------------------ PDB

def dll_pdb_identity(pe):
    """(GUID, age) from the DLL's CodeView (RSDS) debug record, GUID in the
    {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} form llvm-pdbutil prints."""
    rva, size = pe.directories[6]
    if not rva:
        raise ValueError('the DLL has no debug directory')
    base = pe.offset(rva)
    for i in range(size // 28):
        dtype, dsize, _, raw = struct.unpack_from('<IIII', pe.data, base + i * 28 + 12)
        if dtype != 2 or pe.data[raw:raw + 4] != b'RSDS':
            continue
        d1, d2, d3 = struct.unpack_from('<IHH', pe.data, raw + 4)
        d4 = pe.data[raw + 12:raw + 20]
        age = struct.unpack_from('<I', pe.data, raw + 20)[0]
        guid = '{%08X-%04X-%04X-%s-%s}' % (d1, d2, d3, d4[:2].hex().upper(), d4[2:].hex().upper())
        return guid, age
    raise ValueError('the DLL has no CodeView (RSDS) debug record')


def pdb_identity(pdb):
    guid = age = None
    for line in run('llvm-pdbutil.exe', ['dump', '--summary', pdb]):
        m = re.match(r'\s*GUID: (\{[0-9A-Fa-f-]+\})\s*$', line)
        if m:
            guid = m.group(1).upper()
        m = re.match(r'\s*Age: (\d+)\s*$', line)
        if m:
            age = int(m.group(1))
    if guid is None or age is None:
        raise ValueError('no GUID/Age in the summary of %s' % pdb)
    return guid, age


def section_rva(pe, addr):
    sec, off = addr.split(':')
    sec = int(sec)
    if sec < 1 or sec > len(pe.sections):
        return None
    return pe.sections[sec - 1][0] + int(off)


def read_publics(pdb, pe):
    names = {}
    pending = None
    for line in run('llvm-pdbutil.exe', ['dump', '--publics', pdb]):
        m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`$', line)
        if m:
            pending = m.group(1)
            continue
        m = re.search(r'flags = ([^,]*), addr = (\d+:\d+)', line)
        if m and pending is not None:
            if 'function' in m.group(1):
                rva = section_rva(pe, m.group(2))
                if rva is not None:
                    names.setdefault(rva, pending)
            pending = None
    return names


def short_name(name):
    name = re.sub(r'<.*>', '', name)
    return name.split('::')[-1].strip('`\'')


def read_procs(pdb, pe):
    """Every procedure: name, module stem, rva, size, S_FRAMEPROC flags, inlinee names."""
    procs = []
    module = ''
    cur = None
    in_frameproc = False
    lines = run('llvm-pdbutil.exe', ['dump', '--symbols', pdb])
    for i, line in enumerate(lines):
        m = re.match(r'\s*Mod \d+ \| `(.*)`:', line)
        if m:
            module = os.path.splitext(os.path.basename(m.group(1)))[0].lower()
            cur = None
            continue
        m = re.search(r'\| S_(G|L)PROC32 \[size = \d+\] `(.*)`$', line)
        if m:
            cur = {'name': m.group(2), 'module': module, 'rva': None, 'size': 0,
                   'frame': set(), 'inlinees': set()}
            procs.append(cur)
            in_frameproc = False
            if i + 1 < len(lines):
                a = re.search(r'addr = (\d+:\d+), code size = (\d+)', lines[i + 1])
                if a:
                    cur['rva'] = section_rva(pe, a.group(1))
                    cur['size'] = int(a.group(2))
            continue
        if cur is None:
            continue
        if re.search(r'\| S_FRAMEPROC ', line):
            in_frameproc = True
            continue
        if in_frameproc:
            m = re.match(r'\s+flags = (.*)$', line)
            if m:
                cur['frame'] = set(f.strip() for f in m.group(1).split('|'))
                in_frameproc = False
                continue
            if re.search(r'\| S_', line):
                in_frameproc = False
        # S_INLINEES ("inlinee: 0x.. (Name)") lists direct inlinees only;
        # S_INLINESITE ("inlinee = 0x.. (Name)") also covers nested ones.
        for m in re.finditer(r'inlinee\s*[:=]\s*0x[0-9A-Fa-f]+ \(([^)]*)\)', line):
            cur['inlinees'].add(short_name(m.group(1)))
    return procs


# -------------------------------------------------------------------- sources

def strip_code(text):
    """Comments, string and character literals and preprocessor lines blanked,
    newlines kept, so offsets and brace structure survive."""
    out = []
    i, n = 0, len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start and c in ' \t':
            out.append(c)
            i += 1
            continue
        if at_line_start and c == '#':
            while i < n:
                if text[i] == '\n' and (i == 0 or text[i - 1] != '\\'):
                    break
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            continue
        at_line_start = False
        if c == '\n':
            out.append(c)
            at_line_start = True
            i += 1
        elif text.startswith('//', i):
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * max(0, j - i - 2) + (c if j - i >= 2 else ''))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def function_name(header):
    """The name of a function whose definition header this is, or None."""
    h = ' '.join(header.split())
    if not h or h.endswith('=') or ')' not in h:
        return None
    if re.match(r'^(namespace|extern|class|struct|union|enum)\b', h) and '(' not in h:
        return None
    for m in re.finditer(r'(~?[A-Za-z_][\w]*(?:\s*::\s*~?[A-Za-z_]\w*)*)\s*\(', h):
        name = re.sub(r'\s+', '', m.group(1))
        if name.split('::')[-1] in NOT_FUNCTION_NAMES:
            continue
        return name
    return None


def functions_with_try(path):
    """[(name, uses __except, uses __finally)] for each function holding a __try."""
    with open(path, 'rb') as f:
        text = strip_code(f.read().decode('latin-1'))
    stack = []            # 'fn:<name>', 'scope' or 'block'
    fn_start = None
    fn_name = None
    last_boundary = 0
    found = []
    for i, c in enumerate(text):
        if c == '{':
            in_fn = any(k.startswith('fn:') for k in stack)
            if in_fn:
                stack.append('block')
            else:
                name = function_name(text[last_boundary:i])
                if name:
                    stack.append('fn:' + name)
                    fn_start, fn_name = i, name
                else:
                    stack.append('scope')
            last_boundary = i + 1
        elif c == '}':
            if stack:
                kind = stack.pop()
                if kind.startswith('fn:'):
                    body = text[fn_start:i]
                    if re.search(r'\b__try\b', body):
                        found.append((fn_name, bool(re.search(r'\b__except\b', body)),
                                      bool(re.search(r'\b__finally\b', body))))
            last_boundary = i + 1
        elif c == ';':
            last_boundary = i + 1
    return found


def read_source_list(path):
    with open(path, 'rb') as f:
        lines = f.read().decode('latin-1').replace('\r\n', '\n').split('\n')
    return [l.strip() for l in lines if l.strip() and not l.strip().startswith('#')]


# ---------------------------------------------------------------------- check

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dll')
    parser.add_argument('--pdb')
    parser.add_argument('--sources')
    parser.add_argument('--src', action='append', default=[])
    parser.add_argument('--expect', action='append', default=[])
    parser.add_argument('--allow-absent', action='append', default=[])
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    pdb = args.pdb or os.path.splitext(args.dll)[0] + '.pdb'
    sources = []
    if args.sources or not args.src:
        sources += read_source_list(args.sources or r'tools\build\coresrc.txt')
    sources += args.src

    try:
        pe = PE(args.dll)
        dll_id = dll_pdb_identity(pe)
        pdb_id = pdb_identity(pdb)
        if dll_id != pdb_id:
            print('check_seh_handlers: FAILED: %s is not this DLL\'s PDB (DLL expects GUID %s age %d, '
                  'PDB is GUID %s age %d)' % (pdb, dll_id[0], dll_id[1], pdb_id[0], pdb_id[1]))
            return 1
        print('%s: PDB matches the DLL (GUID %s, age %d)' % (pdb, dll_id[0], dll_id[1]))
        unwind = read_unwind(args.dll, pe.image_base)
        publics = read_publics(pdb, pe)
        procs = read_procs(pdb, pe)
    except (OSError, ValueError, RuntimeError) as error:
        print('check_seh_handlers: ERROR: %s' % error)
        return 1

    def handler_name(rva):
        return publics.get(rva, 'rva 0x%X' % rva) if rva is not None else None

    def verdict(proc, need_eh, need_uh):
        """(ok, text) for one procedure's unwind entry."""
        if proc['rva'] is None:
            return False, 'no address'
        entry = unwind.get(proc['rva'])
        if entry is None:
            return False, 'no RUNTIME_FUNCTION at 0x%X' % proc['rva']
        end, flags, handler = entry
        name = handler_name(handler)
        missing = []
        if need_eh and not flags & UNW_EHANDLER:
            missing.append('EHANDLER')
        if need_uh and not flags & UNW_UHANDLER:
            missing.append('UHANDLER')
        if not flags & (UNW_EHANDLER | UNW_UHANDLER):
            return False, 'flags 0x%X, no handler' % flags
        if missing or name != SEH_HANDLER:
            return False, 'flags 0x%X, handler %s%s' % (flags, name, (', missing ' + '+'.join(missing)) if missing else '')
        return True, 'flags 0x%X, handler %s' % (flags, name)

    # 1. The unwind table on its own.
    with_handler = [e for e in unwind.values() if e[1] & (UNW_EHANDLER | UNW_UHANDLER) and e[2] is not None]
    by_handler = {}
    for e in with_handler:
        by_handler[handler_name(e[2])] = by_handler.get(handler_name(e[2]), 0) + 1
    print('%s: %d RUNTIME_FUNCTION entries, %d with an exception handler (%s)'
          % (args.dll, len(unwind), len(with_handler),
             ', '.join('%s %d' % (k, v) for k, v in sorted(by_handler.items(), key=lambda kv: -kv[1]))))

    # 2. Every procedure the compiler marked "has seh".
    flagged = []
    seh_procs = [p for p in procs if 'has seh' in p['frame']]
    seh_ok = 0
    for p in seh_procs:
        ok, text = verdict(p, False, False)
        if ok:
            seh_ok += 1
        else:
            flagged.append('has-seh procedure %s (%s): %s' % (p['name'], p['module'], text))
    print('PDB: %d procedures, %d marked "has seh", %d of them with %s'
          % (len(procs), len(seh_procs), seh_ok, SEH_HANDLER))

    # 3. Every source function holding a __try, matched by name in its module.
    by_module = {}
    for p in procs:
        by_module.setdefault(p['module'], []).append(p)
    results = {}
    allow_absent = set(short_name(n) for n in args.allow_absent)
    defined = set()
    total = files = standalone = inlined_only = absent = allowed = 0
    for src in sources:
        if not os.path.isfile(src):
            print('check_seh_handlers: ERROR: source not found: %s' % src)
            return 1
        fns = functions_with_try(src)
        if not fns:
            continue
        files += 1
        stem = os.path.splitext(os.path.basename(src))[0].lower()
        mod_procs = by_module.get(stem, [])
        if not mod_procs and any(short_name(n) not in allow_absent for n, _, _ in fns):
            flagged.append('%s: module %s holds __try but has no procedure in the PDB '
                           '(built without debug information, or not linked)' % (src, stem))
        for name, uses_except, uses_finally in fns:
            total += 1
            short = short_name(name)
            defined.add(short)
            own = [p for p in mod_procs if short_name(p['name']) == short]
            parents = [p for p in mod_procs if short in p['inlinees'] and p not in own]
            lines = []
            bad = False
            for p in own + parents:
                ok, text = verdict(p, uses_except, uses_finally)
                role = 'own' if p in own else 'inlined into %s' % p['name']
                lines.append('%s: %s%s' % (role, text, '' if ok else '  <-- FLAGGED'))
                if not ok:
                    bad = True
                    flagged.append('%s:%s %s: %s' % (src, name, role, text))
            if own:
                standalone += 1
            elif parents:
                inlined_only += 1
            else:
                absent += 1
            if own or parents:
                status = 'FLAGGED' if bad else 'ok'
            elif short in allow_absent:
                allowed += 1
                status = 'absent (allowed)'
            else:
                status = 'ABSENT'
                flagged.append('%s:%s is neither a procedure nor inlined anywhere in module %s' % (src, name, stem))
            results.setdefault(short, []).append((src, name, status, lines))
    print('Sources: %d function(s) holding __try in %d file(s); %d present as their own procedure, '
          '%d only inlined, %d absent from this image (%d allowed by --allow-absent)'
          % (total, files, standalone, inlined_only, absent, allowed))
    stale = sorted(n for n in allow_absent if n not in defined)
    if stale:
        flagged.append('--allow-absent names no __try function in the sources: ' + ', '.join(stale))
    if args.list:
        for short in sorted(results, key=str.lower):
            for src, full, status, lines in results[short]:
                print('  %-45s %-40s %s%s' % (src, full, status,
                      '' if not lines else ' (%d procedure(s))' % len(lines)))

    # 4. Spot checks by name.
    expect_failed = []
    for name in args.expect:
        hits = results.get(short_name(name), [])
        if not hits:
            expect_failed.append(name)
            print('  expect %s: NOT FOUND among the __try functions' % name)
            continue
        for src, full, status, lines in hits:
            print('  expect %s (%s): %s' % (full, src, status))
            for l in lines[:6]:
                print('      ' + l)
            if len(lines) > 6:
                print('      ... %d more' % (len(lines) - 6))
            if status != 'ok':
                expect_failed.append(name)

    if flagged:
        print('FLAGGED (%d):' % len(flagged))
        for f in flagged:
            print('  ' + f)
    if flagged or expect_failed:
        print('check_seh_handlers: FAILED')
        return 1
    print('check_seh_handlers: OK, every __try function and every "has seh" procedure carries %s' % SEH_HANDLER)
    return 0


if __name__ == '__main__':
    sys.exit(main())

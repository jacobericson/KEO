"""CRT import gate for a DLL built against the VS 2010 contract (Python 3,
stdlib only).

KenshiLib's API passes std::string across the DLL boundary, so a plugin must
use VS 2010's runtime: it imports MSVCR100.dll (and MSVCP100.dll when it uses
the C++ library) and nothing from any other Microsoft C or C++ runtime. A
compiler other than cl.exe 16 can pick up a later runtime without failing
to build (a stray include or library path, a helper the old runtime lacks),
so the clang build scripts run this after every link.

Usage: python tools\\build\\check_crt_imports.py <dll>
Prints the runtime DLLs imported and exits 0, or names the offending import
and exits 1.
"""
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kenshilib'))
from check_imports import PE  # noqa: E402

REQUIRED = 'msvcr100.dll'
ALLOWED = {'msvcr100.dll', 'msvcp100.dll'}
# Any Microsoft C or C++ runtime DLL: msvcrt, msvcrNNN/msvcpNNN with any
# suffix (msvcp140_1, msvcp140_atomic_wait, msvcr100d), vcruntime, the UCRT,
# ConcRT and OpenMP.
RUNTIME_RE = re.compile(r'^(msvc(rt|[rp]\d+(_\w+)?[a-z]?)|vcruntime.*|ucrtbase.*|api-ms-win-crt-.*|concrt.*|vcomp.*)\.dll$')


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    try:
        dlls = sorted(set(dll.lower() for dll, _ in PE(path).imports()))
    except (OSError, ValueError) as error:
        print('CRT import check FAILED: %s: %s' % (path, error))
        return 1
    runtimes = [d for d in dlls if RUNTIME_RE.match(d)]
    wrong = [d for d in runtimes if d not in ALLOWED]
    if wrong:
        print('CRT import check FAILED: %s imports a runtime other than VS 2010\'s: %s' % (path, ', '.join(wrong)))
        return 1
    if REQUIRED not in runtimes:
        print('CRT import check FAILED: %s does not import %s' % (path, REQUIRED.upper()))
        return 1
    print('%s: CRT imports OK: %s (all imports: %s)' % (path, ', '.join(d.upper() for d in runtimes), ', '.join(dlls)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

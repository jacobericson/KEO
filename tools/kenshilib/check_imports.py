"""PE import compatibility gate (Python 3, no third-party packages)."""
import argparse
import json
import struct
import sys
from pathlib import Path


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        if d[:2] != b'MZ':
            raise ValueError('not a PE image: ' + str(path))
        pe = self.u32(0x3c)
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('invalid PE signature')
        self.machine = self.u16(pe + 4)
        opt = pe + 24
        if self.u16(opt) != 0x20b:
            raise ValueError('expected PE32+ (x64)')
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.directories = [struct.unpack_from('<II', d, opt + 112 + i * 8) for i in range(16)]
        self.sections = []
        section = opt + self.u16(pe + 20)
        for i in range(self.u16(pe + 6)):
            size, rva, raw_size, raw = struct.unpack_from('<IIII', d, section + i * 40 + 8)
            self.sections.append((rva, max(size, raw_size), raw, raw_size))

    def u16(self, off):
        return struct.unpack_from('<H', self.data, off)[0]

    def u32(self, off):
        return struct.unpack_from('<I', self.data, off)[0]

    def offset(self, rva):
        for va, size, raw, raw_size in self.sections:
            if va <= rva < va + size and rva - va < raw_size:
                return raw + rva - va
        raise ValueError('unmapped RVA 0x%x' % rva)

    def string(self, rva):
        off = self.offset(rva)
        return self.data[off:self.data.index(b'\0', off)].decode('ascii')

    def exports(self):
        rva, size = self.directories[0]
        if not rva:
            return set()
        off = self.offset(rva)
        count = self.u32(off + 24)
        names = self.offset(self.u32(off + 32))
        return {self.string(self.u32(names + i * 4)) for i in range(count)}

    def imports(self):
        result = []
        for directory, width in ((1, 20), (13, 32)):
            rva, size = self.directories[directory]
            if not rva:
                continue
            pos = self.offset(rva)
            for index in range(size // width):
                fields = struct.unpack_from('<%dI' % (width // 4), self.data, pos + width * index)
                if not any(fields):
                    break
                if directory == 1:
                    table, name = fields[0] or fields[4], fields[3]
                else:
                    if not fields[0] & 1:
                        raise ValueError('VA-based delay imports unsupported')
                    table, name = fields[4], fields[1]
                dll = self.string(name)
                off = self.offset(table)
                while True:
                    thunk = struct.unpack_from('<Q', self.data, off)[0]
                    if not thunk:
                        break
                    symbol = '#%d' % (thunk & 0xffff) if thunk & (1 << 63) else self.string(thunk + 2)
                    result.append((dll, symbol))
                    off += 8
            else:
                raise ValueError('unterminated import directory')
        return result


def check(plugin, baseline):
    pe = PE(plugin)
    if pe.machine != 0x8664:
        raise ValueError('plugin must be x64')
    if '?startPlugin@@YAXXZ' not in pe.exports():
        raise ValueError('missing required C++ export ?startPlugin@@YAXXZ')
    imports = {symbol for dll, symbol in pe.imports() if dll.lower() == 'kenshilib.dll'}
    if not imports:
        raise ValueError('no KenshiLib imports found; refusing an empty compatibility check')
    missing = imports - set(baseline['exports'])
    if missing:
        raise ValueError('imports unavailable in KenshiLib 0.5.0:\n  ' + '\n  '.join(sorted(missing)))
    print('%s: x64, startPlugin export OK; %d KenshiLib imports compatible with 0.5.0' % (plugin, len(imports)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plugin', type=Path)
    parser.add_argument('--baseline', type=Path, default=Path(__file__).with_name('exports-0.5.0.json'))
    args = parser.parse_args()
    try:
        baseline = json.loads(args.baseline.read_text(encoding='utf-8'))
        if baseline['version'] != '0.5.0' or not baseline['exports']:
            raise ValueError('invalid 0.5.0 export baseline')
        check(args.plugin, baseline)
    except (OSError, ValueError, KeyError, struct.error) as error:
        print('KenshiLib compatibility FAILED: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

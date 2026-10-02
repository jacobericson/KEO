"""Write the clang checking tier's response files and compile_commands.json (Python 3, stdlib).

Run after tools\\build\\clang_env.bat (tools\\build\\clang_check.bat does): reads CLANG_CL,
CLANG_COMMON, KENSHILIB_HEADERS and BOOST_ROOT from the environment and refuses to run
without them; ZONEHAND_STEP comes from the environment, else 3. Configurations:
  dev   CLANG_COMMON /DZONEHAND_STEP=<n> /DKEO_DEBUG, the optimizer /I set, coresrc.txt
  prod  CLANG_COMMON /Gy /DZONEHAND_STEP=<n>, the optimizer /I set, coresrc.txt
  prof  CLANG_COMMON, the profiler /I set, profsrc.txt
The optimizer /I set is KENSHILIB_HEADERS\\Include, \\Include\\ogre, BOOST_ROOT, then src
(variant_clang.bat's order); the profiler's is the same (build_clang.bat's). Every
configuration adds -Wmicrosoft-include, which flags an include found only through an
includer's folder.

Usage: compile_commands.py --rsp-dir DIR [--json FILE] [--json-config dev|prod]
Prints 'compile_commands: <cfg> <n> source(s) -> <DIR>\\<cfg>.rsp' for dev, prod and prof,
and with --json 'compile_commands: <m> entries (<cfg> <n> + prof <k>) -> <FILE>' (entries
{directory, file, arguments} for --json-config, default dev, then the profiler). Exit 0; 1 when
a variable is unset or a list is missing or empty.
"""
import argparse, json, os, sys

sys.dont_write_bytecode = True

CORESRC = os.path.join('tools', 'build', 'coresrc.txt')
PROFSRC = os.path.join('tools', 'build', 'profsrc.txt')
REQUIRED = ('CLANG_CL', 'CLANG_COMMON', 'KENSHILIB_HEADERS', 'BOOST_ROOT')


def read_list(path):
    """Non-blank, non-'#' lines, stripped; ValueError when there are none."""
    with open(path, 'rb') as f:
        text = f.read().decode('latin-1').replace('\r\n', '\n')
    lines = [line.strip() for line in text.split('\n') if line.strip() and not line.strip().startswith('#')]
    if not lines:
        raise ValueError('%s lists no sources' % path)
    return lines


def configurations(env):
    """{'dev': (args, sources), 'prod': ..., 'prof': ...}: args is CLANG_COMMON split on
    whitespace, then the configuration's flags, the /I arguments as '/I' + path, and
    '-Wmicrosoft-include'."""
    common = env['CLANG_COMMON'].split()
    headers = env['KENSHILIB_HEADERS']
    boost = env['BOOST_ROOT']
    step = env.get('ZONEHAND_STEP') or '3'
    warn = ['-Wmicrosoft-include']

    i_headers = os.path.join(headers, 'Include')
    i_ogre = os.path.join(headers, 'Include', 'ogre')
    i_opt = ['/I' + i_headers, '/I' + i_ogre, '/I' + boost, '/Isrc']
    i_prof = ['/I' + i_headers, '/I' + i_ogre, '/I' + boost, '/Isrc']

    core_sources = read_list(CORESRC)
    prof_sources = read_list(PROFSRC)

    dev_args = common + ['/DZONEHAND_STEP=%s' % step, '/DKEO_DEBUG'] + i_opt + warn
    prod_args = common + ['/Gy', '/DZONEHAND_STEP=%s' % step] + i_opt + warn
    prof_args = common + i_prof + warn

    return {
        'dev': (dev_args, core_sources),
        'prod': (prod_args, core_sources),
        'prof': (prof_args, prof_sources),
    }


def write_rsp(path, args):
    """One argument per line, an /I path written /I"<path>"."""
    with open(path, 'w', encoding='mbcs') as f:
        for a in args:
            if a.startswith('/I') and len(a) > 2:
                f.write('/I"%s"\n' % a[2:])
            else:
                f.write(a + '\n')


def main(argv=None):
    """Parse, check REQUIRED, write the three .rsp files and the JSON."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--rsp-dir', required=True)
    parser.add_argument('--json')
    parser.add_argument('--json-config', choices=('dev', 'prod'), default='dev')
    args = parser.parse_args(argv)

    missing = [v for v in REQUIRED if not os.environ.get(v)]
    if missing:
        print('compile_commands: ERROR: environment variable(s) not set: %s (run tools\\build\\clang_env.bat first)'
              % ', '.join(missing))
        return 1

    try:
        configs = configurations(os.environ)
    except (OSError, ValueError) as error:
        print('compile_commands: ERROR: %s' % error)
        return 1

    os.makedirs(args.rsp_dir, exist_ok=True)
    for name in ('dev', 'prod', 'prof'):
        cfg_args, sources = configs[name]
        write_rsp(os.path.join(args.rsp_dir, name + '.rsp'), cfg_args)
        print('compile_commands: %s %d source(s) -> %s\\%s.rsp' % (name, len(sources), args.rsp_dir, name))

    if args.json:
        cwd = os.getcwd()
        clang = os.environ['CLANG_CL']
        entries = []
        for name in (args.json_config, 'prof'):
            cfg_args, sources = configs[name]
            command = [clang] + cfg_args
            for s in sources:
                entries.append({'directory': cwd, 'file': s, 'arguments': command + ['/c', s]})
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(entries, f, indent=2)
        cfg_n = len(configs[args.json_config][1])
        prof_n = len(configs['prof'][1])
        print('compile_commands: %d entries (%s %d + prof %d) -> %s'
              % (len(entries), args.json_config, cfg_n, prof_n, args.json))
    return 0


if __name__ == '__main__':
    sys.exit(main())

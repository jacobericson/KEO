"""Tests of tools\\build\\run_variants.py's pure parts (unittest, standard library only): the compile
flags token for token, the include-note filter, the command-line warning filter, and every refusal
of the object manifest. No compiler runs; each manifest test works in its own temporary folder.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_variants  # noqa: E402

ENV = {'KENSHILIB': r'K:\kl', 'BOOST_ROOT': r'K:\kl\..\..\boost_1_60_0'}

# The compile lines the batch scripts ran before the runner held the flags, as cl received them
# (quotes removed, /MP, the response file and /Fo left out):
#   variant.bat: cl /nologo /EHsc /O2 %B_GL% %B_OPTFLAGS% /MD /W3 /DNDEBUG /DWIN32_LEAN_AND_MEAN
#     /DBOOST_ALL_NO_LIB /DBOOST_ERROR_CODE_HEADER_ONLY /DBOOST_SYSTEM_NO_DEPRECATED %B_DEFINES%
#     %B_MP% /I"%KENSHILIB%\Include" /I"%KENSHILIB%\Include\ogre" /I"%BOOST_ROOT%" /Isrc /c ...
#   build.bat: the same without defines, with /GL and no /Gy.
COMMON_HEAD = ['/nologo', '/EHsc', '/O2']
COMMON_MID = ['/MD', '/W3', '/DNDEBUG', '/DWIN32_LEAN_AND_MEAN', '/DBOOST_ALL_NO_LIB',
              '/DBOOST_ERROR_CODE_HEADER_ONLY', '/DBOOST_SYSTEM_NO_DEPRECATED']
INCLUDES = [r'/IK:\kl\Include', r'/IK:\kl\Include\ogre', r'/IK:\kl\..\..\boost_1_60_0', '/Isrc']
STEP4_DEV = COMMON_HEAD + ['/GL'] + COMMON_MID + ['/DZONEHAND_STEP=3', '/DKEO_DEBUG'] + INCLUDES
STEP4_PROD = COMMON_HEAD + ['/GL', '/Gy'] + COMMON_MID + ['/DZONEHAND_STEP=3'] + INCLUDES
PROFILER = COMMON_HEAD + ['/GL'] + COMMON_MID + INCLUDES


class ClArgsTest(unittest.TestCase):

    def test_flags_match_the_old_compile_lines(self):
        self.assertEqual(run_variants.cl_args('opt', 'dev', '/DZONEHAND_STEP=3 /DKEO_DEBUG', ENV), STEP4_DEV)
        self.assertEqual(run_variants.cl_args('opt', 'prod', '/DZONEHAND_STEP=3', ENV), STEP4_PROD)
        self.assertEqual(run_variants.cl_args('prof', 'prof', '', ENV), PROFILER)

    def test_unknown_pairs_and_missing_roots_are_refused(self):
        for kind, flavour in (('prof', 'dev'), ('opt', 'prof'), ('opt', 'x')):
            with self.assertRaises(ValueError):
                run_variants.cl_args(kind, flavour, '', ENV)
        for missing in ('KENSHILIB', 'BOOST_ROOT'):
            env = dict(ENV)
            del env[missing]
            with self.assertRaises(ValueError):
                run_variants.cl_args('opt', 'dev', '', env)

    def test_command_sha_covers_every_token(self):
        self.assertNotEqual(run_variants.cmd_sha256(STEP4_DEV), run_variants.cmd_sha256(STEP4_DEV + ['/DX']))
        self.assertEqual(run_variants.cmd_sha256(list(STEP4_DEV)), run_variants.cmd_sha256(STEP4_DEV))


class OutputFilterTest(unittest.TestCase):

    def test_notes_are_dropped_and_collected_once_in_order(self):
        cwd = os.getcwd()
        out = (b'one.cpp\r\n'
               b'Note: including file: src\\plugin/two.h\r\n'
               b'Note: including file:  C:\\SDK\\Include\\windows.h\r\n'
               b'src\\one.cpp(3) : warning C4101: \'x\' : unreferenced local variable\r\n'
               b'Note: including file: src\\plugin/two.h\r\n'
               b'tail without newline')
        text, includes = run_variants.split_notes(out)
        self.assertEqual(text, b'one.cpp\r\nsrc\\one.cpp(3) : warning C4101: \'x\' : unreferenced local '
                               b'variable\r\ntail without newline\r\n')
        self.assertEqual(includes, [os.path.normcase(os.path.join(cwd, 'src', 'plugin', 'two.h')),
                                    os.path.normcase(r'C:\SDK\Include\windows.h')])

    def test_no_output_stays_empty(self):
        self.assertEqual(run_variants.split_notes(b''), (b'', []))

    def test_command_line_warnings_are_kept_once_per_variant(self):
        d9 = b"cl : Command line warning D9025 : overriding '/O2' with '/O1'\r\n"
        other = b"cl : Command line warning D9002 : ignoring unknown option '/Q'\r\n"
        outputs = [d9 + b'a.cpp\r\n', d9 + other + b'b.cpp\r\n', b'c.cpp\r\n' + d9]
        self.assertEqual(run_variants.drop_repeated_command_line_warnings(outputs),
                         [d9 + b'a.cpp\r\n', other + b'b.cpp\r\n', b'c.cpp\r\n'])
        warn = b"a.cpp(1) : warning C4101: 'x' : unreferenced local variable\r\n"
        self.assertEqual(run_variants.drop_repeated_command_line_warnings([warn, warn]), [warn, warn])


class ManifestTest(unittest.TestCase):

    def setUp(self):
        self.cwd = os.getcwd()
        self.tmp = tempfile.mkdtemp(prefix='keo_run_variants_test_')
        os.chdir(self.tmp)
        self.saved = dict((k, os.environ.get(k)) for k in ENV)
        os.environ.update(ENV)
        os.makedirs('list')
        self.list_path = os.path.join('list', 'src.txt')
        self.write_list(['src\\a.cpp', 'src\\b.cpp'])
        self.v = self.variant()
        os.makedirs(self.v.objdir)
        for i, source in enumerate(self.v.sources):
            with open(os.path.join(self.v.objdir, run_variants.obj_name(source)), 'wb') as f:
                f.write(b'object %d' % i)
            self.v.results[i] = (0, b'', ['c:\\inc\\%d.h' % i])
        run_variants.write_manifest(self.v)

    def tearDown(self):
        os.chdir(self.cwd)
        for k, value in self.saved.items():
            if value is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = value
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_list(self, sources):
        with open(self.list_path, 'w') as f:
            f.write('# comment\n\n' + '\n'.join(sources) + '\n')

    def variant(self, build_id='build-1', outdir=os.path.join('build', 'x_dev')):
        v = run_variants.Variant(0, 'opt', outdir, os.path.join('build', 'obj_x_dev'), '/DZONEHAND_STEP=3',
                                 '', self.list_path, False, build_id)
        v.sources = run_variants.load_sources(self.list_path)
        v.args = run_variants.cl_args(v.kind, v.flavour, v.defines)
        return v

    def problems(self, v=None):
        return run_variants.validate_manifest(v or self.v)

    def edit_manifest(self, change):
        with open(self.v.manifest, 'r', encoding='utf-8') as f:
            data = json.load(f)
        change(data)
        with open(self.v.manifest, 'w', encoding='utf-8') as f:
            json.dump(data, f)

    def assertRefused(self, problems, fragment):
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(fragment, problems[0])

    def test_clean_manifest_passes_and_records_each_object(self):
        self.assertEqual(self.problems(), [])
        with open(self.v.manifest, 'r', encoding='utf-8') as f:
            data = json.load(f)
        self.assertEqual([e['src'] for e in data['sources']], ['src\\a.cpp', 'src\\b.cpp'])
        self.assertEqual([e['how'] for e in data['sources']], ['compiled', 'compiled'])
        self.assertEqual(data['sources'][1]['includes'], ['c:\\inc\\1.h'])

    def test_another_runs_manifest_is_refused(self):
        self.assertRefused(self.problems(self.variant(build_id='build-2')), 'is not this run\'s')

    def test_another_flavour_is_refused(self):
        prod = self.variant(outdir=os.path.join('build', 'x_prod'))
        problems = self.problems(prod)
        self.assertTrue(any('kind opt/dev, expected opt/prod' in p for p in problems), problems)
        self.assertTrue(any('compile command\'s sha256 differs' in p for p in problems), problems)

    def test_another_command_is_refused(self):
        self.v.args = self.v.args + ['/DEXTRA']
        self.assertRefused(self.problems(), 'compile command\'s sha256 differs')

    def test_a_changed_list_is_refused(self):
        self.write_list(['src\\b.cpp', 'src\\a.cpp'])
        self.assertRefused(self.problems(), 'the source list differs')
        self.write_list(['src\\a.cpp', 'src\\b.cpp', 'src\\c.cpp'])
        self.assertRefused(self.problems(), 'the source list differs')
        os.remove(self.list_path)
        self.assertRefused(self.problems(), 'cannot re-read the source list')

    def test_a_missing_or_rewritten_object_is_refused(self):
        b = os.path.join(self.v.objdir, 'b.obj')
        with open(b, 'r+b') as f:
            f.write(b'X')
        self.assertRefused(self.problems(), 'b.obj does not match its recorded sha256')
        os.remove(b)
        self.assertRefused(self.problems(), 'b.obj is missing')

    def test_an_object_outside_this_folder_is_refused(self):
        def move(data):
            data['sources'][0]['obj'] = os.path.join('elsewhere', 'a.obj')
        self.edit_manifest(move)
        self.assertRefused(self.problems(), 'is not this folder\'s')

    def test_a_missing_or_unreadable_manifest_is_refused(self):
        with open(self.v.manifest, 'w') as f:
            f.write('{not json')
        self.assertRefused(self.problems(), 'unreadable')
        os.remove(self.v.manifest)
        self.assertRefused(self.problems(), 'unreadable')

    def test_sources_sharing_an_object_name_are_refused(self):
        self.write_list(['a\\one.cpp', 'c\\One.cpp'])
        with self.assertRaises(ValueError):
            run_variants.load_sources(self.list_path)

    def test_fresh_objects_only(self):
        start = os.stat(os.path.join(self.v.objdir, 'a.obj')).st_mtime + 10
        problems = run_variants.check_objects_fresh(self.v.objdir, self.v.sources, start)
        self.assertEqual(len(problems), 2)
        self.assertIn('predates this compile', problems[0])
        missing = run_variants.check_objects_fresh(self.v.objdir, ['src\\%s.cpp' % uuid.uuid4().hex], 0)
        self.assertIn('no object was produced', missing[0])


if __name__ == '__main__':
    unittest.main()

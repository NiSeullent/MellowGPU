# SPDX-License-Identifier: MIT
"""Exercise the real Clang runner; no native GPU operation is available."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'Tools/run-backend-interface-tests.py'
CLANG = shutil.which('clang++')


@unittest.skipUnless(CLANG, 'Real Clang++ is required for driver-override regressions')
class BackendInterfaceRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.out = Path(self.temporary.name) / 'result'

    def tearDown(self):
        self.temporary.cleanup()

    def run_runner(self, override=None, project=ROOT, environment=None, out=None):
        environment = dict(os.environ if environment is None else environment)
        environment.pop('CCC_OVERRIDE_OPTIONS', None)
        if override is not None:
            environment['CCC_OVERRIDE_OPTIONS'] = override
        return subprocess.run(
            [sys.executable, str(project / 'Tools/run-backend-interface-tests.py'),
             '--cxx', CLANG, '--sanitize', '--out', str(self.out if out is None else out)],
            cwd=project, env=environment,
            capture_output=True, text=True, timeout=120)

    def fixture_project(self, source):
        # Compile the production runner's actual commands in an isolated project;
        # only its test translation unit is generated, never the canonical fixture.
        project = Path(tempfile.mkdtemp(prefix='project-', dir=self.temporary.name))
        for name in ('Tools/run-backend-interface-tests.py', 'Tools/sanitizer_policy.py',
                     'Drivers/Core/BackendInterfaces.hpp'):
            target = project / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
        (project / 'tests').mkdir()
        (project / 'tests/backend_interface_tests.cpp').write_text(source)
        return project

    def assert_override_rejected(self, override):
        result = self.run_runner(override)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('CCC_OVERRIDE_OPTIONS', result.stderr)
        self.assertFalse(self.out.exists(), 'Rejected override must create no output or success report')

    def test_driver_override_cannot_disable_requested_sanitizers(self):
        # Actual Clang appends this flag after the runner's sanitizer flags.
        # Before the guard, the runner reported sanitizers=true for an
        # uninstrumented binary and returned success.
        self.assert_override_rejected('+-fno-sanitize=all')

    def test_even_empty_driver_override_is_rejected_before_output(self):
        self.assert_override_rejected('')

    def test_clean_sanitized_run_executes_real_adapter_and_records_sources(self):
        environment = dict(os.environ, ASAN_OPTIONS='abort_on_error=0:exitcode=0',
                           UBSAN_OPTIONS='halt_on_error=0:exitcode=0')
        result = self.run_runner(environment=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('native GPU operations unavailable', result.stdout)
        report = json.loads((self.out / 'backend-interface-tests.json').read_text())
        self.assertTrue(report['passed'])
        self.assertTrue(report['sanitizers'])
        self.assertIn('actual_checks', report)
        self.assertGreater(report['actual_checks'], 0)
        self.assertIn('sanitizer_runtime_policy', report)
        scope = report['sanitizer_runtime_policy']
        self.assertEqual(scope['host_platform'], sys.platform)
        self.assertIs(scope['address_sanitizer_enabled'], True)
        self.assertIs(scope['undefined_behavior_sanitizer_enabled'], True)
        if sys.platform == 'darwin':
            self.assertIs(scope['leak_detection_enabled'], False)
            self.assertEqual(scope['asan_options'], 'detect_leaks=0:halt_on_error=1')
            self.assertIn('not verified', scope['leak_detection_scope'])
        elif sys.platform.startswith('linux'):
            self.assertIs(scope['leak_detection_enabled'], True)
            self.assertEqual(scope['asan_options'], 'detect_leaks=1:halt_on_error=1')
        self.assertEqual(scope['ubsan_options'], 'halt_on_error=1:print_stacktrace=1')
        self.assertIn('Tools/sanitizer_policy.py', report['source_sha256'])
        self.assertEqual([item['exit_code'] for item in report['commands']], [0, 0, 0])
        for name, digest in report['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), digest)
        for key in ('physical_gpu_executed', 'native_target_executed',
                    'metal3_verified', 'windowserver_verified'):
            self.assertIs(report[key], False)
        nm = shutil.which('nm')
        self.assertIsNotNone(nm, 'Native symbol inspector required to verify instrumentation')
        symbols = subprocess.run([nm, str(self.out / 'backend-interface-tests')],
                                 capture_output=True, text=True, timeout=20)
        self.assertEqual(symbols.returncode, 0, symbols.stderr)
        self.assertIn('__asan_init', symbols.stdout)

    def test_caller_zero_exit_options_cannot_hide_actual_sanitizer_findings(self):
        # Removing the runner's runtime policy must turn these into false passes:
        # both real instrumented binaries print a valid summary before the fault,
        # so summary validation alone cannot protect this boundary.
        faults = {
            'address': ('char *p = static_cast<char *>(std::malloc(8));\n'
                        '    if (!p) return 2;\n'
                        '    volatile int index = argc + 7;\n'
                        '    p[index] = 42;\n'
                        '    std::free(p);', 'AddressSanitizer'),
            'undefined': ('volatile int limit = INT_MAX;\n'
                          '    volatile int increment = argc;\n'
                          '    volatile int result = limit + increment;\n'
                          '    (void)result;', 'runtime error: signed integer overflow'),
        }
        for fault, (operation, diagnostic) in faults.items():
            with self.subTest(sanitizer=fault):
                source = ('#include <cstdio>\n#include <cstdlib>\n#include <climits>\n'
                          'int main(int argc, char **) {\n'
                          '    std::puts("PASS 1 internal three-vendor interface checks; '
                          'native GPU operations unavailable");\n'
                          '    std::fflush(stdout);\n    ' + operation + '\n    return 0;\n}\n')
                project = self.fixture_project(source)
                out = Path(self.temporary.name) / fault
                environment = dict(os.environ, ASAN_OPTIONS='abort_on_error=0:exitcode=0',
                                   UBSAN_OPTIONS='halt_on_error=0:exitcode=0')
                result = self.run_runner(project=project, environment=environment, out=out)
                report = json.loads((out / 'backend-interface-tests.json').read_text())
                self.assertIn(diagnostic, (out / '1.stderr').read_text())
                self.assertIn('PASS 1 internal', (out / '1.stdout').read_text())
                self.assertEqual(result.returncode, 1, json.dumps(report) + result.stdout + result.stderr)
                self.assertIs(report['passed'], False)
                self.assertNotEqual(report['commands'][1]['exit_code'], 0)
                shutil.rmtree(project)

    def test_zero_exit_without_a_valid_positive_summary_cannot_pass(self):
        summaries = ['', 'PASS 0 internal three-vendor interface checks; native GPU operations unavailable',
                     'PASS 1000000000 internal three-vendor interface checks; native GPU operations unavailable',
                     'PASS 1 internal three-vendor interface checks; native GPU operations unavailable\\nextra']
        for index, summary in enumerate(summaries):
            with self.subTest(summary=summary):
                project = self.fixture_project('#include <cstdio>\nint main() { std::puts("' + summary + '"); }\n')
                out = Path(self.temporary.name) / str(index)
                result = self.run_runner(project=project, out=out)
                report = json.loads((out / 'backend-interface-tests.json').read_text())
                self.assertEqual(result.returncode, 1, json.dumps(report))
                self.assertIs(report['passed'], False)
                self.assertEqual(report['commands'][1]['exit_code'], 0)
                shutil.rmtree(project)

    def test_actual_positive_summary_count_is_recorded_without_fixed_fixture_count(self):
        project = self.fixture_project(
            '#include <cstdio>\nint main() { std::puts("PASS 3 internal three-vendor interface checks; '
            'native GPU operations unavailable"); }\n')
        result = self.run_runner(project=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((self.out / 'backend-interface-tests.json').read_text())
        self.assertIs(report['passed'], True)
        self.assertIn('actual_checks', report)
        self.assertEqual(report['actual_checks'], 3)


if __name__ == '__main__':
    unittest.main()

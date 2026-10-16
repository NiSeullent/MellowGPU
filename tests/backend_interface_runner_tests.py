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

    def run_runner(self, override=None):
        environment = dict(os.environ)
        environment.pop('CCC_OVERRIDE_OPTIONS', None)
        if override is not None:
            environment['CCC_OVERRIDE_OPTIONS'] = override
        return subprocess.run(
            [sys.executable, str(RUNNER), '--cxx', CLANG, '--sanitize',
             '--out', str(self.out)], cwd=ROOT, env=environment,
            capture_output=True, text=True, timeout=120)

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
        result = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('native GPU operations unavailable', result.stdout)
        report = json.loads((self.out / 'backend-interface-tests.json').read_text())
        self.assertTrue(report['passed'])
        self.assertTrue(report['sanitizers'])
        self.assertEqual([item['exit_code'] for item in report['commands']], [0, 0, 0])
        for name, digest in report['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), digest)
        for key in ('physical_gpu_executed', 'native_target_executed',
                    'metal3_verified', 'windowserver_verified'):
            self.assertIs(report[key], False)
        nm = shutil.which('nm')
        self.assertIsNotNone(nm, 'Native symbol inspector required to verify instrumentation')
        symbols = subprocess.run([nm, '-u', str(self.out / 'backend-interface-tests')],
                                 capture_output=True, text=True, timeout=20)
        self.assertEqual(symbols.returncode, 0, symbols.stderr)
        self.assertIn('__asan_init', symbols.stdout)


if __name__ == '__main__':
    unittest.main()

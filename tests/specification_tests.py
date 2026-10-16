# SPDX-License-Identifier: MIT
"""Real checker regressions; mutation fixtures never qualify physical hardware."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'Tools/mellowgpu-spec.py'


class SpecificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'project'
        shutil.copytree(ROOT, self.root, ignore=shutil.ignore_patterns(
            '.git', '.claude', 'build', '__pycache__'))

    def tearDown(self):
        self.temp.cleanup()

    def run_check(self):
        return subprocess.run([sys.executable, str(TOOL), 'check', '--root', str(self.root)],
                              capture_output=True, text=True, timeout=30)

    def mutate(self, path, change):
        file = self.root / path
        value = json.loads(file.read_text())
        change(value)
        file.write_text(json.dumps(value))

    def rejected(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('FAIL', result.stderr)

    def test_actual_project_contract_is_consistent(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report['hardware_qualified'])
        self.assertFalse(report['gpu_executed'])

    def test_qualified_profile_cannot_be_added(self):
        self.mutate('spec/v1/support-matrix.json', lambda x: x['qualified_profiles'].append('15igl7'))
        self.rejected()

    def test_native_capability_cannot_be_promoted(self):
        self.mutate('spec/v1/capabilities.json', lambda x: x.update(native_gpu_submission=True))
        self.rejected()

    def test_wire_size_drift_rejected(self):
        self.mutate('spec/v1/abi.json', lambda x: x['control'].update(request_bytes=128))
        self.rejected()

    def test_cpu_and_gpu_vendor_cannot_be_confused(self):
        self.mutate('spec/v1/support-matrix.json', lambda x: x['development_targets'][0].update(gpu_vendor='nvidia'))
        self.rejected()

    def test_dependency_pin_or_source_hash_drift_rejected(self):
        self.mutate('dependencies.lock', lambda x: x['components'][0].update(commit='0' * 40))
        self.rejected()

    def test_dependency_source_digest_drift_rejected(self):
        file = self.root / 'porting/upstreams.lock.json'
        file.write_text(file.read_text() + '\n')
        self.rejected()

    def test_dependency_reverse_tracking_matches_forward_requirements(self):
        index = json.loads((self.root / 'dependencies.lock').read_text())
        tracking = {item['id']: item['requirements'] for item in index['tracking']}
        requirements = json.loads((self.root / 'spec/v1/requirements.json').read_text())
        for requirement in requirements['requirements']:
            for source in requirement['source_refs']:
                if source in tracking:
                    self.assertIn(requirement['id'], tracking[source], source)

    def test_duplicate_json_key_rejected(self):
        file = self.root / 'spec/v1/capabilities.json'
        file.write_text(file.read_text().replace('{', '{"schema_version":99,', 1))
        self.rejected()

    def test_missing_requirement_implementation_rejected(self):
        self.mutate('spec/v1/requirements.json', lambda x: x['requirements'][0]['implementation'].append('missing/source.hpp'))
        self.rejected()

    def test_duplicate_requirement_id_rejected(self):
        self.mutate('spec/v1/requirements.json', lambda x: x['requirements'].append(x['requirements'][0]))
        self.rejected()

    def test_source_link_removed_rejected(self):
        self.mutate('spec/v1/requirements.json', lambda x: x['requirements'][0].update(source_refs=['missing-source']))
        self.rejected()

    def test_schema_unknown_key_rejected(self):
        self.mutate('spec/v1/abi.json', lambda x: x.update(qualification_override=True))
        self.rejected()

    def test_symlink_implementation_rejected(self):
        target = self.root / 'Drivers/Core/BackendInterfaces.hpp'
        copy = Path(self.temp.name) / 'outside.hpp'
        target.rename(copy)
        target.symlink_to(copy)
        self.rejected()

    def test_document_requirement_link_must_exist(self):
        path = self.root / 'docs/specification.md'
        path.write_text(path.read_text().replace('MG-CORE-ABI-001', 'REMOVED-ABI-ID'))
        self.rejected()

    def test_stale_comment_cannot_hide_actual_abi_drift(self):
        path = self.root / 'Drivers/Core/MellowGPUABI.h'
        body = path.read_text().replace('#define MELLOWGPU_CONTROL_ABI_MAJOR 1U',
                                        '#define MELLOWGPU_CONTROL_ABI_MAJOR 2U')
        path.write_text(body + '\n// prior ABI: #define MELLOWGPU_CONTROL_ABI_MAJOR 1U\n')
        self.rejected()

    def test_unused_schema_branch_must_be_checked(self):
        self.mutate('spec/v1/abi.schema.json', lambda x: x['properties'].update(
            unused={'$ref': 'not-supported.json'}))
        self.rejected()

    def test_schema_object_additional_properties_is_not_silently_ignored(self):
        self.mutate('spec/v1/abi.schema.json', lambda x: x.update(
            additionalProperties={'type': 'integer'}))
        self.rejected()

    def test_boolean_cannot_replace_integer_in_nested_constant(self):
        self.mutate('spec/v1/capabilities.json', lambda x: x.update(control_bits=[True, 2, 4]))
        self.rejected()

    def test_integer_cannot_replace_boolean_in_nested_constant(self):
        self.mutate('spec/v1/support-matrix.json', lambda x: x['research_platforms'][0].update(
            kernel_implementation=0))
        self.rejected()

    def test_exact_target_revision_subsystem_and_os_are_bound(self):
        for key, value in [('pci_revision', 'FF'), ('pci_subsystem', 'ffff:ffff'),
                           ('os_version', '26.2'), ('cpu_vendor', 'AMD')]:
            path = self.root / 'spec/v1/support-matrix.json'
            original = path.read_text()
            self.mutate('spec/v1/support-matrix.json', lambda x: x['development_targets'][0].update(
                {key: value}))
            self.rejected()
            path.write_text(original)

    def test_observation_cannot_gain_gpu_execution(self):
        self.mutate('spec/evidence/15igl7-metal-runtime-metadata.json',
                    lambda x: x.update(gpu_commands_submitted=True))
        self.rejected()

    def test_observation_receipt_source_hash_is_bound(self):
        path = self.root / 'Tools/metal-runtime-metadata.mm'
        path.write_text(path.read_text() + '\n// changed helper bytes\n')
        self.rejected()

    def test_observation_receipt_os_matches_actual_tuple(self):
        self.mutate('spec/evidence/15igl7-metal-runtime-metadata.receipt.json',
                    lambda x: x['operating_system_identity'].update(os_build='24H99'))
        self.rejected()

    def test_profile_native_control_cannot_promote_gpu_capabilities(self):
        path = self.root / 'porting/metal3-profile.json'
        original = path.read_bytes()
        for key, value in [('gpu_submission_implemented', True),
                           ('gpu_virtual_memory_implemented', True),
                           ('physical_runtime_verified', True), ('mode', 'gpu')]:
            with self.subTest(key=key):
                self.mutate(str(path.relative_to(self.root)),
                            lambda x: x['native_control'].update({key: value}))
                self.rejected()
                path.write_bytes(original)

    def test_profile_cpu_or_policy_results_cannot_qualify_hardware(self):
        path = self.root / 'porting/metal3-profile.json'
        original = path.read_bytes()
        for key in ('cpu_rasterization_qualifies', 'cpu_pixel_transfers_qualify',
                    'policy_unit_tests_are_hardware_evidence'):
            with self.subTest(key=key):
                self.mutate(str(path.relative_to(self.root)), lambda x: x.update({key: True}))
                self.rejected()
                path.write_bytes(original)

    def test_profile_full_product_gates_cannot_be_removed(self):
        self.mutate('porting/metal3-profile.json', lambda x: x['required_gates'].remove('windowserver'))
        self.rejected()

    def test_receipt_helper_architecture_and_scope_cannot_drift(self):
        path = self.root / 'spec/evidence/15igl7-metal-runtime-metadata.receipt.json'
        original = path.read_bytes()
        for location, key, value in [('helper', 'native_executed_architecture', 'arm64'),
                                     ('helper', 'public_device_enumeration_called', True),
                                     (None, 'private_initializers_invoked_directly', True),
                                     (None, 'user_designated_test_device', '255U')]:
            with self.subTest(key=key):
                self.mutate(str(path.relative_to(self.root)),
                            lambda x: (x[location] if location else x).update({key: value}))
                self.rejected()
                path.write_bytes(original)

    def test_actual_glk_cannot_invent_apple_gpu_family(self):
        self.mutate('spec/v1/support-matrix.json', lambda x: x['development_targets'][0].update(
            apple_gpu_family='Apple9'))
        self.rejected()


if __name__ == '__main__':
    unittest.main()

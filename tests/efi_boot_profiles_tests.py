#!/usr/bin/env python3
"""Check exact profile deltas so first-boot comparisons keep one variable at a time."""

import copy
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'efi_boot_profiles', ROOT / 'Tools/efi_boot_profiles.py')
PROFILES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILES)


def differences(left, right, path=()):
    if isinstance(left, dict) and isinstance(right, dict):
        if left.keys() != right.keys():
            return {path + ('<keys>',)}
        return set().union(*(differences(left[key], right[key], path + (key,))
                             for key in left))
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            return {path + ('<length>',)}
        return set().union(*(differences(a, b, path + (index,))
                             for index, (a, b) in enumerate(zip(left, right))))
    return {path} if left != right else set()


class BootProfileTests(unittest.TestCase):
    def setUp(self):
        self.guid = PROFILES.APPLE_BOOT_GUID
        self.default = {
            'ACPI': {'Add': [{'Path': 'SSDT-PLUG-255U.aml', 'Enabled': True}]},
            'Booter': {'Quirks': {
                'DevirtualiseMmio': True, 'EnableWriteUnprotector': False,
                'RebuildAppleMemoryMap': True, 'SyncRuntimePermissions': True,
                'SetupVirtualMap': True}},
            'Kernel': {'Add': [
                {'BundlePath': 'Lilu.kext', 'Enabled': True},
                {'BundlePath': 'MellowGPU.kext', 'Enabled': True}],
                'Emulate': {'Cpuid1Data': b'\xea\x06\x09\x00'}},
            'NVRAM': {'Add': {self.guid: {
                'boot-args': '-v keepsyms=1 debug=0x100 -mellowdiag'}}},
            'PlatformInfo': {'Generic': {'SystemProductName': 'MacBookPro16,2'}},
        }

    def test_baseline_changes_only_mellow_and_its_argument(self):
        original = copy.deepcopy(self.default)
        profiles = PROFILES.build_profiles(self.default)
        baseline = profiles['config-boot-baseline.plist']
        self.assertEqual(self.default, original)
        self.assertEqual(differences(self.default, baseline), {
            ('Kernel', 'Add', 1, 'Enabled'),
            ('NVRAM', 'Add', self.guid, 'boot-args'),
        })
        self.assertIs(baseline['Kernel']['Add'][1]['Enabled'], False)
        self.assertEqual(baseline['NVRAM']['Add'][self.guid]['boot-args'],
                         '-v keepsyms=1 debug=0x100 -mellowoff')

    def test_existing_rescue_and_memory_map_variants_are_preserved(self):
        profiles = PROFILES.build_profiles(self.default)
        rescue = profiles['config-rescue.plist']
        legacy = profiles['config-legacy-memory-map.plist']
        self.assertEqual(differences(self.default, rescue), {
            ('Kernel', 'Add', 1, 'Enabled'),
            ('NVRAM', 'Add', self.guid, 'boot-args'),
        })
        self.assertEqual(rescue['NVRAM']['Add'][self.guid]['boot-args'],
                         '-v keepsyms=1 debug=0x100 -mellowoff cpus=1')
        self.assertEqual(differences(self.default, legacy), {
            ('Booter', 'Quirks', 'DevirtualiseMmio'),
            ('Booter', 'Quirks', 'EnableWriteUnprotector'),
            ('Booter', 'Quirks', 'RebuildAppleMemoryMap'),
            ('Booter', 'Quirks', 'SyncRuntimePermissions'),
        })

    def test_ambiguous_mellow_or_boot_arguments_are_rejected(self):
        for malformed in ('duplicate', 'disabled', 'missing-argument', 'duplicate-argument'):
            with self.subTest(malformed=malformed):
                config = copy.deepcopy(self.default)
                if malformed == 'duplicate':
                    config['Kernel']['Add'].append(
                        {'BundlePath': 'MellowGPU.kext', 'Enabled': True})
                elif malformed == 'disabled':
                    config['Kernel']['Add'][1]['Enabled'] = False
                elif malformed == 'missing-argument':
                    config['NVRAM']['Add'][self.guid]['boot-args'] = '-v'
                else:
                    config['NVRAM']['Add'][self.guid]['boot-args'] += ' -mellowdiag'
                with self.assertRaises(ValueError):
                    PROFILES.build_profiles(config)


if __name__ == '__main__':
    unittest.main(verbosity=2)

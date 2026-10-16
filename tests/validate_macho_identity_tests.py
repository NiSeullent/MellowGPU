#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression tests: validate-macho keeps strict MellowGPU defaults while
explicit identity parameters check a standalone diagnostic kext genuinely."""
import importlib.util
import inspect
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools'))
spec = importlib.util.spec_from_file_location('validate_macho', ROOT / 'Tools/validate-macho.py')
V = importlib.util.module_from_spec(spec)
spec.loader.exec_module(V)
from product_identity import BUNDLE_ID, VERSION

PROBE_ID, PROBE_VERSION = 'com.NiSeullent.MellowGPU.Probe', '0.1.0'
PROBE_ENTRYPOINTS = ('_MellowProbe_start', '_MellowProbe_stop')
MELLOW_ENTRYPOINTS = ('_MellowGPU_kern_start', '_MellowGPU_kern_stop')


def section(name, segment, addr, size, offset):
    return struct.pack('<16s16sQQ8I', name.encode(), segment.encode(), addr, size, offset, 0, 0, 0, 0, 0, 0, 0)


def kext(identifier, version, defined, undefined=(), filetype=11, lifecycle=True):
    """Thin x86_64 image: one __DATA segment holding kmod_info and lifecycle sections."""
    sections = [('__data', 0x1000, 0x100)]
    if lifecycle:
        sections += [('__mod_init_func', 0x1100, 8), ('__mod_term_func', 0x1108, 8)]
    seg_size = 72 + 80 * len(sections)
    commands_size = seg_size + 24
    data_off = 0x1000
    seg = struct.pack('<II16s4Q4I', 0x19, seg_size, b'__DATA', 0x1000, 0x1000, data_off, 0x1000, 3, 3, len(sections), 0)
    seg += b''.join(section(n, '__DATA', a, s, data_off + a - 0x1000) for n, a, s in sections)
    kmod = bytearray(0x100)
    kmod[16:16 + len(identifier)] = identifier.encode()
    kmod[80:80 + len(version)] = version.encode()
    strings = b'\0'
    symbols = b''
    for name, value, external in [('_kmod_info', 0x1000, True)] + [(n, 0x1010, False) for n in defined]:
        symbols += struct.pack('<IBBHQ', len(strings), 0x0E | (1 if external else 0), 1, 0, value)
        strings += name.encode() + b'\0'
    for name in undefined:
        symbols += struct.pack('<IBBHQ', len(strings), 0x01, 0, 0, 0)
        strings += name.encode() + b'\0'
    symoff = data_off + 0x1000
    stroff = symoff + len(symbols)
    symtab = struct.pack('<6I', 0x2, 24, symoff, len(symbols) // 16, stroff, len(strings))
    header = struct.pack('<8I', 0xFEEDFACF, 0x01000007, 3, filetype, 2, commands_size, 0, 0)
    image = bytearray(header + seg + symtab)
    image += bytes(data_off - len(image))
    image += kmod + bytes(0x1000 - len(kmod))
    image += symbols + strings
    return bytes(image)


class ValidateMachoIdentity(unittest.TestCase):
    def inspect(self, data, **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'binary'
            path.write_bytes(data)
            return V.inspect_binary(path, **kwargs)

    def test_defaults_remain_strict_mellowgpu_identity(self):
        parameters = inspect.signature(V.inspect_binary).parameters
        self.assertEqual(parameters['expected_bundle_id'].default, BUNDLE_ID)
        self.assertEqual(parameters['expected_version'].default, VERSION)
        self.assertEqual(tuple(parameters['required_entrypoints'].default), MELLOW_ENTRYPOINTS)
        for name in ('expected_bundle_id', 'expected_version', 'required_entrypoints'):
            self.assertEqual(parameters[name].kind, inspect.Parameter.KEYWORD_ONLY)

    def test_default_accepts_current_mellowgpu_product(self):
        report = self.inspect(kext(BUNDLE_ID, VERSION, MELLOW_ENTRYPOINTS))
        self.assertTrue(report['structural_validation_passed'], report['errors'])

    def test_default_rejects_standalone_probe(self):
        report = self.inspect(kext(PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS))
        self.assertFalse(report['structural_validation_passed'])
        self.assertIn('KMOD identity is not the current MellowGPU development product', report['errors'])
        self.assertIn('Missing module definition _MellowGPU_kern_start', report['errors'])

    def test_explicit_identity_accepts_matching_probe(self):
        report = self.inspect(kext(PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS), expected_bundle_id=PROBE_ID,
                              expected_version=PROBE_VERSION, required_entrypoints=PROBE_ENTRYPOINTS)
        self.assertTrue(report['structural_validation_passed'], report['errors'])
        self.assertEqual((report['kmod_identifier'], report['kmod_version']), (PROBE_ID, PROBE_VERSION))

    def test_explicit_identity_is_still_checked(self):
        for identifier, version, defined in [(PROBE_ID, '0.1.1', PROBE_ENTRYPOINTS),
                                             (BUNDLE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS),
                                             (PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS[:1])]:
            report = self.inspect(kext(identifier, version, defined), expected_bundle_id=PROBE_ID,
                                  expected_version=PROBE_VERSION, required_entrypoints=PROBE_ENTRYPOINTS)
            self.assertFalse(report['structural_validation_passed'], (identifier, version, defined))

    def test_explicit_identity_keeps_kernel_image_rules(self):
        probe = dict(expected_bundle_id=PROBE_ID, expected_version=PROBE_VERSION, required_entrypoints=PROBE_ENTRYPOINTS)
        cases = {'user import': kext(PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS, undefined=('_main',)),
                 'not kext bundle': kext(PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS, filetype=2),
                 'no lifecycle sections': kext(PROBE_ID, PROBE_VERSION, PROBE_ENTRYPOINTS, lifecycle=False),
                 'no kmod_info entrypoints': kext(PROBE_ID, PROBE_VERSION, ())}
        for label, data in cases.items():
            self.assertFalse(self.inspect(data, **probe)['structural_validation_passed'], label)


if __name__ == '__main__':
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""Read-only EFI structure, binary identity and exact manifest checks; never a boot test."""
import argparse
import hashlib
import json
import plistlib
import re
import struct
from pathlib import Path, PurePosixPath
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from product_identity import BUNDLE, BUNDLE_ID, LEGACY_BUNDLE, validate_configured_product


FONT_SUFFIXES = {'.ttf', '.otf', '.woff', '.woff2', '.pfb', '.pfm', '.pf2', '.fon', '.fnt'}
UNVERIFIED = (
    'target_boot_verified', 'target_kernel_load_verified',
    'native_gpu_submission_implemented', 'apple_metal_abi_implemented',
    'full_metal_acceleration_implemented', 'windowserver_acceleration_verified',
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative_path(base, name, directory=False):
    """Reject traversal and platform-dependent spelling, even if the result exists."""
    if not isinstance(name, str) or not name or any(c in name for c in ('\\', ':', '\n', '\r', '\x00')):
        raise ValueError('Invalid relative path')
    parts = name.split('/')
    if any(p in ('', '.', '..') for p in parts) or PurePosixPath(name).is_absolute():
        raise ValueError('Unsafe relative path: ' + name)
    target = (base / name).resolve()
    if not target.is_relative_to(base.resolve()):
        raise ValueError('Path escapes its component: ' + name)
    if not (target.is_dir() if directory else target.is_file()):
        raise ValueError('Missing package component: ' + name)
    return target


def macho_x64(data, expected_kind, fat_allowed=True):
    """Validate the x86_64 slice and bounded load-command table, not runtime linkage."""
    if fat_allowed and data[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        if len(data) < 8:
            raise ValueError('Truncated universal Mach-O')
        is64 = data[:4] == b'\xca\xfe\xba\xbf'
        count = struct.unpack_from('>I', data, 4)[0]
        entry_size = 32 if is64 else 20
        if not 0 < count <= 16 or 8 + count * entry_size > len(data):
            raise ValueError('Invalid universal Mach-O table')
        slices = []
        for i in range(count):
            entry = struct.unpack_from('>IIQQII' if is64 else '>IIIII', data, 8 + i * entry_size)
            cpu, _, offset, size = entry[:4]
            if offset < 8 + count * entry_size or size < 32 or offset + size > len(data):
                raise ValueError('Invalid universal Mach-O slice')
            if cpu == 0x01000007:
                slices.append(data[offset:offset + size])
        if len(slices) != 1:
            raise ValueError('Expected exactly one x86_64 slice')
        return macho_x64(slices[0], expected_kind, False)
    if len(data) < 32:
        raise ValueError('Truncated Mach-O executable')
    magic, cpu, _, kind, count, commands, _, _ = struct.unpack_from('<8I', data)
    if magic != 0xFEEDFACF or cpu != 0x01000007 or kind != expected_kind:
        raise ValueError('Incorrect x86_64 Mach-O type')
    if not 0 < count <= 4096 or 32 + commands > len(data):
        raise ValueError('Invalid Mach-O load-command region')
    offset = 32
    for _ in range(count):
        if offset + 8 > 32 + commands:
            raise ValueError('Truncated Mach-O load command')
        size = struct.unpack_from('<I', data, offset + 4)[0]
        if size < 8 or size % 8 or offset + size > 32 + commands:
            raise ValueError('Invalid Mach-O load-command size')
        offset += size
    if offset != 32 + commands:
        raise ValueError('Mach-O load-command size mismatch')


def pe_x64(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Invalid EFI DOS header: ' + path.name)
    offset = struct.unpack_from('<I', data, 0x3C)[0]
    if offset + 24 + 70 > len(data) or data[offset:offset + 4] != b'PE\0\0':
        raise ValueError('Invalid EFI PE header: ' + path.name)
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    optional_size = struct.unpack_from('<H', data, offset + 20)[0]
    magic = struct.unpack_from('<H', data, offset + 24)[0]
    subsystem = struct.unpack_from('<H', data, offset + 24 + 68)[0]
    if machine != 0x8664 or magic != 0x20B or optional_size < 70 or offset + 24 + optional_size > len(data) or subsystem not in (10, 11, 12):
        raise ValueError('EFI image is not bounded x86_64 PE32+: ' + path.name)


def verify_manifest(root):
    entries = {}
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        if '  ' not in line:
            raise ValueError('Malformed SHA256 manifest')
        digest, name = line.split('  ', 1)
        if not re.fullmatch(r'[0-9a-f]{64}', digest) or name in entries or name == 'SHA256SUMS':
            raise ValueError('Invalid or duplicate manifest entry: ' + name)
        path = relative_path(root, name)
        if sha(path) != digest:
            raise ValueError('Checksum mismatch: ' + name)
        entries[name] = digest
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() and p != root / 'SHA256SUMS'}
    if set(entries) != actual:
        raise ValueError('Manifest does not cover the exact original package file set')
    return len(entries)


def validate(root, verify_sha=False):
    root = Path(root).resolve()
    oc = root / 'EFI/OC'
    status = json.loads((root / 'STATUS.json').read_text())
    if status.get('status') != 'PARTIAL_EXPERIMENTAL_NOT_FULL_METAL':
        raise ValueError('Unexpected release readiness classification')
    for key in UNVERIFIED:
        if status.get(key) is not False:
            raise ValueError('Unexpected hardware/Metal success claim: ' + key)
    for path in root.rglob('*'):
        if path.name == LEGACY_BUNDLE:
            raise ValueError('Legacy Mellow bundle cannot be included in active MellowGPU package')
        if path.is_symlink() or path.suffix.lower() in FONT_SUFFIXES:
            raise ValueError('Unexpected package file: ' + str(path))
    profiles = []
    binary_checks = set()
    for path in [oc / 'config.plist', *sorted((root / 'Profiles').glob('*.plist'))]:
        config = plistlib.loads(path.read_bytes())
        for entry in config['ACPI']['Add']:
            table = relative_path(oc / 'ACPI', entry['Path'])
            data = table.read_bytes()
            if len(data) < 36 or data[:4] != b'SSDT' or struct.unpack_from('<I', data, 4)[0] != len(data) or sum(data) % 256:
                raise ValueError('Invalid generated SSDT: ' + table.name)
        loaded = set()
        configured_products = []
        for entry in config['Kernel']['Add']:
            bundle = relative_path(oc / 'Kexts', entry['BundlePath'], directory=True)
            plist = relative_path(bundle, entry['PlistPath'])
            info = plistlib.loads(plist.read_bytes())
            configured_products.append((entry['BundlePath'], info))
            executable = info.get('CFBundleExecutable')
            if executable:
                expected = 'Contents/MacOS/' + executable
                if entry['ExecutablePath'] != expected:
                    raise ValueError('ExecutablePath does not match bundle metadata')
                binary = relative_path(bundle, expected)
                if binary not in binary_checks:
                    macho_x64(binary.read_bytes(), 11)
                    binary_checks.add(binary)
            elif entry['ExecutablePath']:
                raise ValueError('Unexpected executable in codeless kext')
            if entry['MinKernel'] != '24.0.0' or entry['MaxKernel'] != '24.99.99':
                raise ValueError('Wrong Darwin injection range')
            if entry['Enabled']:
                identifier = info['CFBundleIdentifier']
                if identifier in loaded:
                    raise ValueError('Duplicate loaded bundle identifier')
                for dependency in info.get('OSBundleLibraries', {}):
                    if not dependency.startswith('com.apple.') and dependency not in loaded:
                        raise ValueError('Unresolved or out-of-order dependency: ' + dependency)
                loaded.add(identifier)
        validate_configured_product(configured_products)
        for entry in config['UEFI']['Drivers']:
            pe_x64(relative_path(oc / 'Drivers', entry['Path']))
        for entry in config['Misc']['Tools']:
            pe_x64(relative_path(oc / 'Tools', entry['Path']))
        if config['DeviceProperties']['Add']:
            raise ValueError('Unexpected identity injection')
        profiles.append({'profile': path.relative_to(root).as_posix(), 'passed': True})
    for required in (root / 'EFI/BOOT/BOOTx64.efi', oc / 'OpenCore.efi'):
        pe_x64(required)
    built = json.loads((root / 'Verification/macho-validation.json').read_text())
    mellow_binary = oc / 'Kexts/MellowGPU.kext/Contents/MacOS/MellowGPU'
    if not built.get('structural_validation_passed') or built['sha256'] != sha(mellow_binary):
        raise ValueError('Mellow executable does not match its native build receipt')
    for name in ('sequoia-probe', 'tahoe-diag-client', 'metal-probe'):
        macho_x64(relative_path(root / 'Tools', name).read_bytes(), 2)
    manifest_count = verify_manifest(root) if verify_sha else None
    return {
        'passed': True, 'scope': 'static package files and native-build identity only; no runtime execution',
        'profiles': profiles, 'x86_64_kext_binaries_checked': len(binary_checks),
        'checksums_verified': verify_sha, 'manifest_files_verified': manifest_count,
        'target_boot_verified': False, 'full_metal_acceleration_verified': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--verify-sha', action='store_true')
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.package, args.verify_sha), indent=2))
        return 0
    except (ValueError, KeyError, OSError, struct.error) as error:
        print(json.dumps({'passed': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

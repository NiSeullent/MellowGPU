#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the passive GLK PCI observer as a separate native diagnostic kext.

The bundle contains only Mellow/GlkPassiveProbeIOKit.cpp plus the standard
KMOD start/stop wrappers that libkmod.a calls (KERN_SUCCESS boilerplate; the
IOService personality does the matching). It is not MellowGPU, not an
accelerator and not GPU support. This tool never installs, loads or signs with
an identity; it builds, ad-hoc signs and audits in a fresh output directory.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUNDLE_ID = 'com.NiSeullent.MellowGPU.Probe'
VERSION = '0.1.0'
EXECUTABLE = 'MellowGPUProbe'
ENTRYPOINTS = ('_MellowProbe_start', '_MellowProbe_stop')
SOURCES = ('Mellow/GlkPassiveProbeIOKit.cpp', 'Mellow/GlkPassiveProbeIOKit.hpp',
           'Mellow/GlkPassiveProbeIOKit.personality.plist', 'Tools/build-glk-passive-probe.py',
           'Tools/validate-macho.py', 'Tools/tahoe-abi.py', 'Tools/product_identity.py')
LIBRARIES = {'com.apple.iokit.IOPCIFamily': '1.0.0b1', 'com.apple.kpi.iokit': '10.0.0',
             'com.apple.kpi.libkern': '10.0.0', 'com.apple.kpi.mach': '10.0.0'}
# Kernel code model: no red zone, no SIMD/FP register use, freestanding headers.
COMMON_FLAGS = ['-target', 'x86_64-apple-macos13', '-mkernel', '-nostdinc', '-DKERNEL', '-DKERNEL_PRIVATE',
                '-fno-exceptions', '-fno-builtin', '-fno-common', '-fno-asynchronous-unwind-tables',
                '-fvisibility=hidden', '-mno-red-zone', '-mno-sse', '-mno-sse2', '-mno-mmx', '-msoft-float',
                '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations']
CXX_FLAGS = ['-std=c++17', '-fapple-kext', '-fno-rtti', '-fno-use-cxa-atexit',
             '-fno-register-global-dtors-with-atexit']
MODULE_INFO = '''#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
kern_return_t MellowProbe_start(kmod_info_t *info, void *data) { return KERN_SUCCESS; }
kern_return_t MellowProbe_stop(kmod_info_t *info, void *data) { return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(com.NiSeullent.MellowGPU.Probe, "0.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = MellowProbe_start;
__private_extern__ kmod_stop_func_t *_antimain = MellowProbe_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
'''
FORBIDDEN_IMPORTS = ('_main', '_atexit', '___cxa_atexit', 'dyld_stub_binder', '_dyld_stub_binder', '___dso_handle')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, cwd=None):
    result = subprocess.run([str(x) for x in argv], cwd=cwd, capture_output=True, text=True, timeout=300)
    record = {'command': [str(x) for x in argv], 'exit_code': result.returncode,
              'stdout': result.stdout, 'stderr': result.stderr}
    if result.returncode:
        raise RuntimeError(json.dumps(record))
    return record


def load_tool(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'Tools' / filename)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / 'Tools'))
    spec.loader.exec_module(module)
    return module


def git_binding():
    head = run(['git', 'rev-parse', 'HEAD'], ROOT)['stdout'].strip()
    status = run(['git', 'status', '--porcelain', '--', *SOURCES], ROOT)['stdout']
    blobs = {}
    for name in SOURCES:
        tracked = subprocess.run(['git', 'rev-parse', 'HEAD:' + name], cwd=ROOT, capture_output=True, text=True)
        actual = run(['git', 'hash-object', name], ROOT)['stdout'].strip()
        blobs[name] = {'head_blob': tracked.stdout.strip() if tracked.returncode == 0 else None, 'worktree_blob': actual}
    clean = not status and all(v['head_blob'] == v['worktree_blob'] for v in blobs.values())
    return {'commit': head, 'inputs_match_commit': clean, 'git_blobs': blobs}


def sdk_record(sdk):
    sdk = sdk.resolve()
    settings = sdk / 'SDKSettings.json'
    headers = sdk / 'System/Library/Frameworks/Kernel.framework/Headers'
    libkmod = sdk / 'usr/lib/libkmod.a'
    for path in (settings, headers / 'IOKit/pci/IOPCIDevice.h', headers / 'mach/kmod.h', libkmod):
        if not path.exists():
            raise ValueError('SDK lacks kernel build input: ' + str(path))
    version = json.loads(settings.read_text()).get('Version')
    return sdk, headers, libkmod, {'path': str(sdk), 'version': version, 'settings_sha256': sha(settings),
                                   'libkmod_sha256': sha(libkmod)}


def build(args):
    output = args.output.resolve()
    if output.exists():
        raise ValueError('Output must be a fresh directory: ' + str(output))
    if output.is_relative_to(ROOT) and not output.is_relative_to(ROOT / 'build'):
        raise ValueError('Output inside the repository must be under build/')
    sdk, headers, libkmod, sdk_info = sdk_record(args.sdk)
    if args.require_sdk_version and sdk_info['version'] != args.require_sdk_version:
        raise ValueError('SDK version %s is not %s' % (sdk_info['version'], args.require_sdk_version))
    if bool(args.boot_kc) != bool(args.system_kc):
        raise ValueError('--boot-kc and --system-kc must be supplied as a pair from one boot')
    before = {name: sha(ROOT / name) for name in SOURCES}
    binding = git_binding()
    output.mkdir(parents=True)
    work = output / 'objects'
    work.mkdir()
    clang = Path(run(['xcrun', '--find', 'clang'])['stdout'].strip())
    ld = Path(run(['xcrun', '--find', 'ld'])['stdout'].strip())
    resource = Path(run([clang, '-print-resource-dir'])['stdout'].strip())
    flags = [*COMMON_FLAGS[:4], '-isystem', resource / 'include', '-isystem', headers, *COMMON_FLAGS[4:]]
    commands = [run([clang, '--version']), run(['xcrun', 'ld', '-v'])]
    commands.append(run([clang, *flags, *CXX_FLAGS, '-c', ROOT / 'Mellow/GlkPassiveProbeIOKit.cpp',
                         '-o', work / 'probe.o']))
    (work / 'module_info.c').write_text(MODULE_INFO)
    commands.append(run([clang, *flags, '-c', work / 'module_info.c', '-o', work / 'module_info.o']))
    bundle = output / (EXECUTABLE + '.kext')
    binary = bundle / 'Contents/MacOS' / EXECUTABLE
    binary.parent.mkdir(parents=True)
    commands.append(run([ld, '-arch', 'x86_64', '-kext', '-static', '-undefined', 'dynamic_lookup', '-o', binary,
                         work / 'probe.o', work / 'module_info.o', libkmod]))
    personality = plistlib.loads((ROOT / 'Mellow/GlkPassiveProbeIOKit.personality.plist').read_bytes())
    if set(personality) != {'MellowGlkPassiveProbe'}:
        raise ValueError('Unexpected passive personality set')
    personality['MellowGlkPassiveProbe']['CFBundleIdentifier'] = BUNDLE_ID
    info = dict(CFBundleIdentifier=BUNDLE_ID, CFBundleExecutable=EXECUTABLE, CFBundleName=EXECUTABLE,
                CFBundlePackageType='KEXT', CFBundleVersion=VERSION, CFBundleShortVersionString=VERSION,
                CFBundleInfoDictionaryVersion='6.0', OSBundleCompatibleVersion=VERSION,
                IOKitPersonalities=personality, OSBundleLibraries=dict(LIBRARIES),
                NSHumanReadableCopyright='Mellow contributors; passive diagnostic only')
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    commands.append(run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', bundle]))
    commands.append(run(['/usr/bin/codesign', '--verify', '--strict', '--verbose=2', bundle]))

    validator = load_tool('mellow_validate_macho', 'validate-macho.py')
    structural = validator.inspect_binary(binary, expected_bundle_id=BUNDLE_ID, expected_version=VERSION,
                                          required_entrypoints=ENTRYPOINTS)
    errors = list(structural['errors'])
    if structural['filetype'] != 11 or structural['cpu_type'] != hex(0x01000007):
        errors.append('not a thin x86_64 MH_KEXT_BUNDLE')
    for name in FORBIDDEN_IMPORTS:
        if name in structural['undefined_symbols']:
            errors.append('user-space import ' + name)
    if any('lilu' in name.lower() for name in structural['undefined_symbols']) or 'as.vit9696.Lilu' in info['OSBundleLibraries']:
        errors.append('passive probe must not depend on Lilu')
    abi = load_tool('mellow_tahoe_abi', 'tahoe-abi.py')
    image = abi.macho(binary.read_bytes())
    exports = None
    if args.boot_kc:
        collections = [abi.read_kc(args.boot_kc.resolve()), abi.read_kc(args.system_kc.resolve())]
        exports = abi.resolve_imports(collections, {'definitions': {}}, {'CFBundleIdentifier': 'unused'}, image, info)
        exports['collections'] = [{'path': c['path'], 'sha256': c['sha256'], 'uuid': c['uuid']} for c in collections]
        if exports['unmatched'] or not exports['all_imports_statically_eligible']:
            errors.append('imports not statically eligible against supplied kernel collections')
    after = {name: sha(ROOT / name) for name in SOURCES}
    if after != before:
        errors.append('build inputs changed during build')
    files = {p.relative_to(bundle).as_posix(): sha(p) for p in sorted(bundle.rglob('*')) if p.is_file()}
    receipt = {
        'schema': 'mellowgpu.glk-passive-probe-build/1',
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'status': 'BUILT_AND_AUDITED' if not errors else 'FAILED_AUDIT', 'errors': errors,
        'bundle_identifier': BUNDLE_ID, 'version': VERSION, 'bundle': str(bundle),
        'binary_sha256': sha(binary), 'uuid': image.get('uuid'), 'bundle_file_sha256': files,
        'source_sha256': before, 'source_binding': binding,
        'install_eligible_source': binding['inputs_match_commit'],
        'sdk': sdk_info, 'toolchain': {'clang': str(clang), 'clang_sha256': sha(clang), 'ld': str(ld), 'ld_sha256': sha(ld)},
        'compile_flags': [str(x) for x in flags], 'cxx_flags': CXX_FLAGS, 'module_info_c': MODULE_INFO,
        'import_count': len(set(structural['undefined_symbols'])), 'imports': sorted(set(structural['undefined_symbols'])),
        'structural': {k: v for k, v in structural.items() if k not in ('defined_symbols', 'undefined_symbols')},
        'kernel_collection_exports': exports,
        'signature': 'ad-hoc, codesign --verify --strict passed',
        'scope': 'Static build/signature/structure/import eligibility only; not installed, loaded or GPU-executed',
        'loaded': False, 'gpu_execution': False, 'accelerator': False, 'qualified_profiles': 0,
        'commands': [{k: c[k] for k in ('command', 'exit_code')} for c in commands],
    }
    (output / 'build-receipt.json').write_text(json.dumps(receipt, indent=2, default=str) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Fresh output directory (never reused)')
    parser.add_argument('--sdk', type=Path, required=True, help='macOS SDK with Kernel.framework headers and libkmod.a')
    parser.add_argument('--require-sdk-version', default='15.5', help="Required SDKSettings Version ('' to accept any)")
    parser.add_argument('--boot-kc', type=Path, help='Actual BootKernelExtensions.kc paired with --system-kc')
    parser.add_argument('--system-kc', type=Path, help='Actual SystemKernelExtensions.kc from the same boot')
    parser.add_argument('--expect-binary-sha256', help='Fail unless the rebuilt executable matches this digest')
    args = parser.parse_args()
    receipt = build(args)
    if args.expect_binary_sha256:
        receipt['expected_binary_sha256'] = args.expect_binary_sha256
        receipt['reproduced_expected_binary'] = receipt['binary_sha256'] == args.expect_binary_sha256
        (args.output.resolve() / 'build-receipt.json').write_text(json.dumps(receipt, indent=2, default=str) + '\n')
    exports = receipt['kernel_collection_exports']
    print(json.dumps({'status': receipt['status'], 'errors': receipt['errors'], 'binary_sha256': receipt['binary_sha256'],
                      'uuid': receipt['uuid'], 'imports': receipt['import_count'],
                      'eligible_imports': exports['matched_count'] if exports else None,
                      'reproduced_expected_binary': receipt.get('reproduced_expected_binary'),
                      'install_eligible_source': receipt['install_eligible_source']}, indent=2))
    ok = receipt['status'] == 'BUILT_AND_AUDITED' and receipt.get('reproduced_expected_binary', True)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())

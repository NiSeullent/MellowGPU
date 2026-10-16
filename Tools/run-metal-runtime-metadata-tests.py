#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test the original metadata helper on test classes; never enumerate or submit a GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out, sdk = args.out.resolve(), args.sdk.resolve()
    architecture = platform.machine()
    if sys.platform != 'darwin' or architecture not in ('arm64', 'x86_64'):
        parser.error('Native macOS arm64 or x86_64 runtime is required; no cross-architecture execution')
    translation = subprocess.run(['/usr/sbin/sysctl', '-in', 'sysctl.proc_translated'],
                                 capture_output=True, text=True, timeout=10)
    if translation.returncode or translation.stdout.strip() not in ('', '0', '1'):
        parser.error('Cannot establish native process execution')
    if translation.stdout.strip() == '1':
        parser.error('Rosetta translation is not native architecture evidence')
    if 'CCC_OVERRIDE_OPTIONS' in os.environ:
        parser.error('CCC_OVERRIDE_OPTIONS must be unset')
    if not args.cxx or out.exists() or out == root or out in root.parents:
        parser.error('Clang++ and fresh scratch output are required')
    if not (sdk/'SDKSettings.plist').is_file():
        parser.error('An explicit local Apple SDK is required')
    files = ['Tools/metal-runtime-metadata.mm', 'tests/metal_runtime_metadata_fixture.mm',
             'Tools/run-metal-runtime-metadata-tests.py']
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {name: digest(root/name) for name in files}
    out.mkdir(parents=True)
    binary = out/'metal-runtime-metadata-fixture'
    compile_argv = [args.cxx, '-std=c++17', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                    '-arch', architecture, '-mmacosx-version-min=13.0', '-isysroot', str(sdk),
                    str(root/files[1]), '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)]
    if args.sanitize:
        compile_argv += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    report = {'schema':'mellowgpu.original-metal-metadata-fixture/1', 'passed':False,
              'source_sha256':before, 'architecture':architecture, 'sanitizers':args.sanitize,
              'process_translation_detected':False,
              'sdk_settings_sha256':digest(sdk/'SDKSettings.plist'),
              'scope':'Actual helper bodies, original test-only classes and public Objective-C runtime; not a Metal device behavior test',
              'public_device_enumeration_called':False, 'gpu_commands_submitted':False,
              'private_driver_subclasses_instantiated_directly':False,
              'metal3_verified':False, 'windowserver_verified':False, 'commands':[]}
    try:
        compiler = subprocess.run([args.cxx, '--version'], capture_output=True, text=True, timeout=15)
        if compiler.returncode:
            raise ValueError('Compiler version query failed')
        report['compiler_version'] = compiler.stdout
        for index, argv in enumerate([compile_argv, ['/usr/bin/arch', '-'+architecture, str(binary)]]):
            result = subprocess.run(argv, cwd=root, capture_output=True, text=True, timeout=60 if index == 0 else 20)
            (out/f'{index}.stdout').write_text(result.stdout)
            (out/f'{index}.stderr').write_text(result.stderr)
            report['commands'].append({'argv':argv, 'returncode':result.returncode})
            if result.returncode:
                raise ValueError('Metadata fixture compile/run failed: '+result.stdout+result.stderr)
        expected = 'PASS 7 original runtime metadata cases; no fixture initialization or method invocation\n'
        if (out/'1.stdout').read_text() != expected:
            raise ValueError('Unexpected metadata fixture output')
        report['source_unchanged'] = before == {name:digest(root/name) for name in files}
        if not report['source_unchanged']:
            raise ValueError('Metadata helper or committed fixture changed during execution')
        report.update(passed=True, actual_cases=7, binary_sha256=digest(binary))
        print(expected, end='')
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
        print(error, file=sys.stderr)
    (out/'metadata-fixture-result.json').write_text(json.dumps(report, indent=2, sort_keys=True)+'\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

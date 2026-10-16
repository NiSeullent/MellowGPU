#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify actual metadata adapter code; this never executes a GPU backend."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    if not args.cxx or out.exists() or out == root or out in root.parents:
        parser.error('compiler and fresh scratch directory required')
    names = ['Drivers/Core/BackendInterfaces.hpp', 'tests/backend_interface_tests.cpp',
             'Tools/run-backend-interface-tests.py']
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    hashes = {name: digest(root / name) for name in names}
    out.mkdir(parents=True)
    binary = out / 'backend-interface-tests'
    common = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic']
    build = common + [str(root / names[1]), '-o', str(binary)]
    if args.sanitize:
        build += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-g']
    header = out / 'freestanding.cpp'
    header.write_text('#include "' + str(root / names[0]) + '"\n' +
        'template class MellowGPUCore::Backend::MetadataInterfaceV1<0x8086>;\n' +
        'template class MellowGPUCore::Backend::MetadataInterfaceV1<0x1002>;\n' +
        'template class MellowGPUCore::Backend::MetadataInterfaceV1<0x10de>;\n')
    commands = [build, [str(binary)], common + ['-ffreestanding', '-fno-exceptions',
                '-fno-rtti', '-fsyntax-only', str(header)]]
    report = {'schema': 'mellowgpu.backend-interface-tests/1', 'passed': False,
              'source_sha256': hashes, 'sanitizers': args.sanitize, 'commands': [],
              'host_platform': platform.platform(), 'process_architecture': platform.machine(),
              'compiler_version': subprocess.run([args.cxx, '--version'], capture_output=True,
                                                  text=True, check=True).stdout,
              'sdk_selection': 'Toolchain default; no explicit SDK or kernel ABI claim',
              'native_target_executed': False,
              'scope': 'Real internal metadata interfaces; no native GPU implementation',
              'physical_gpu_executed': False, 'metal3_verified': False,
              'windowserver_verified': False}
    try:
        for i, command in enumerate(commands):
            result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
            (out / f'{i}.stdout').write_text(result.stdout)
            (out / f'{i}.stderr').write_text(result.stderr)
            report['commands'].append({'argv': command, 'exit_code': result.returncode})
            if result.returncode:
                raise ValueError(result.stderr or result.stdout)
        if hashes != {name: digest(root / name) for name in names}:
            raise ValueError('source changed during tests')
        report.update(passed=True, binary_sha256=digest(binary))
        print((out / '1.stdout').read_text().strip())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
        print(error, file=sys.stderr)
    (out / 'backend-interface-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

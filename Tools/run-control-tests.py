#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute control-wire validation and compile its C/freestanding declarations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    parser.add_argument('--cc', default=shutil.which('clang') or shutil.which('gcc'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not args.cxx or not args.cc:
        parser.error('C and C++ compilers are required')
    root = Path(__file__).resolve().parents[1]
    inputs = ['Drivers/Core/MellowGPUABI.h', 'Drivers/Core/ControlProtocol.hpp',
              'tests/control_protocol_tests.cpp', 'Tools/run-control-tests.py']
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs}
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out.resolve() / 'control-tests'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if args.sanitize else []
    commands = [
        [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-x', 'c',
         '-fsyntax-only', '-include', str(root / inputs[0]), '/dev/null'],
        [args.cxx, '-std=c++17', '-ffreestanding', '-nostdinc++', '-fno-exceptions', '-fno-rtti',
         '-Wall', '-Wextra', '-Werror', '-pedantic', '-x', 'c++', '-fsyntax-only',
         '-include', str(root / inputs[1]), '/dev/null'],
        [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', *flags,
         str(root / inputs[2]), '-o', str(binary)],
        [str(binary)],
    ]
    results = []
    for index, command in enumerate(commands):
        done = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
        (args.out / f'{index}.stdout').write_text(done.stdout)
        (args.out / f'{index}.stderr').write_text(done.stderr)
        results.append({'command': command, 'exit_code': done.returncode})
        if done.returncode:
            print(done.stdout + done.stderr)
            break
    unchanged = hashes == {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs}
    passed = unchanged and len(results) == len(commands) and all(item['exit_code'] == 0 for item in results)
    report = {'passed': passed, 'source_sha256': hashes, 'source_unchanged': unchanged,
              'commands': results, 'sanitizers': args.sanitize, 'gpu_executed': False,
              'native_iokit_executed': False, 'scope': 'Actual request/reply wire validation; C ABI and freestanding compile only'}
    (args.out / 'control-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    print((args.out / '3.stdout').read_text().strip() if passed else 'FAIL control-wire checks')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

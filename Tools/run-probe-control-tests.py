#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the real userspace probe against injected public IOKit responses."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    if not args.cxx or out.exists() or out == root or out in root.parents:
        parser.error('compiler and fresh scratch output are required')
    inputs = ['Userspace/mellowgpu-control.cpp', 'Drivers/Core/MellowGPUABI.h',
              'Drivers/Core/ControlProtocol.hpp', 'tests/probe_control_tests.cpp',
              'Tools/run-probe-control-tests.py']
    inputs += [p.relative_to(root).as_posix() for p in sorted((root / 'tests/probe_control_shim').rglob('*'))
               if p.is_file()]
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    hashes = {name: digest(root / name) for name in inputs}
    out.mkdir(parents=True)
    binary = out / 'probe-control-tests'
    command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-I' + str(root / 'tests/probe_control_shim'), str(root / 'tests/probe_control_tests.cpp'),
               '-o', str(binary)]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    report = {'schema': 'mellowgpu.actual-probe-test/1', 'passed': False,
              'scope': 'Actual probe source; injected public IOKit and test-process CPU RAM only',
              'source_sha256': hashes, 'sanitizers': args.sanitize,
              'native_iokit_executed': False, 'physical_gpu_executed': False,
              'metal3_verified': False, 'windowserver_verified': False, 'commands': []}
    try:
        for index, argv in enumerate([command, [str(binary)]]):
            done = subprocess.run(argv, cwd=root, capture_output=True, text=True, timeout=120 if not index else 30)
            (out / f'{index}.stdout').write_text(done.stdout)
            (out / f'{index}.stderr').write_text(done.stderr)
            report['commands'].append({'argv': argv, 'exit_code': done.returncode})
            if done.returncode:
                raise ValueError('probe regression compile/run failed: ' + done.stdout + done.stderr)
        lines = (out / '1.stdout').read_text().splitlines()
        results = [json.loads(line) for line in lines[:-1]]
        if len(results) != 7 or [x['status'] for x in results] != ['failed'] * 6 + ['passed']:
            raise ValueError('actual probe JSON outcomes did not match the seven scenarios')
        if any(x['gpu_executed'] or x['metal3_verified'] or x['windowserver_verified'] for x in results):
            raise ValueError('CPU probe must never claim acceleration qualification')
        report['source_unchanged'] = hashes == {name: digest(root / name) for name in inputs}
        if not report['source_unchanged']:
            raise ValueError('source changed during regression execution')
        report.update(passed=True, actual_probe_cases=7, binary_sha256=digest(binary))
        print(lines[-1])
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
        print(error, file=sys.stderr)
    (out / 'probe-control-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

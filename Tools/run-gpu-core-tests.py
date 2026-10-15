#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build/run actual lifecycle policy bodies. No native or GPU operations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


def execute(compiler, root, output, sanitize):
    output.mkdir(parents=True, exist_ok=True)
    binary = output / ('gpu-lifecycle-tests.exe' if os.name == 'nt' else 'gpu-lifecycle-tests')
    sources = ('Drivers/Core/DeviceLifecycle.hpp', 'tests/gpu_lifecycle_tests.cpp',
               'Tools/run-gpu-core-tests.py')
    before = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in sources}
    report = {'schema_version': 1, 'evidence_scope': 'lifecycle-policy-only-no-native-adapter',
              'physical_gpu_executed': False, 'native_driver_executed': False,
              'metal_conformance_tested': False, 'windowserver_tested': False,
              'hardware_support_qualified': False, 'compiler': compiler,
              'source_sha256': before, 'status': 'FAIL', 'build': None, 'test': None,
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'sanitizers': ['address', 'undefined'] if sanitize else []}
    try:
        version = subprocess.run([compiler, '--version'], capture_output=True,
                                 text=True, check=False, timeout=20)
        report['compiler_version'] = version.stdout.strip()
        if version.returncode:
            raise RuntimeError('compiler version query failed: ' + version.stderr)
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-fno-exceptions', '-fno-rtti',
                   str(root / 'tests/gpu_lifecycle_tests.cpp'), '-o', str(binary)]
        if sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        report['compile_command'] = command
        build = subprocess.run(command, capture_output=True, text=True,
                               check=False, timeout=120)
        report['build'] = {'exit_code': build.returncode, 'stdout': build.stdout,
                           'stderr': build.stderr}
        if build.returncode:
            print(build.stdout + build.stderr, file=sys.stderr)
            return build.returncode
        report['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        test = subprocess.run([str(binary)], capture_output=True, text=True,
                              check=False, timeout=60)
        report['test'] = {'exit_code': test.returncode, 'stdout': test.stdout,
                          'stderr': test.stderr}
        print(test.stdout, end='')
        print(test.stderr, end='', file=sys.stderr)
        if test.returncode == 0:
            report['status'] = 'PASS_LIFECYCLE_POLICY_ONLY'
        return test.returncode
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        report['error'] = str(error)
        print(str(error), file=sys.stderr)
        return 1
    finally:
        try:
            after = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in sources}
            report['source_unchanged'] = after == before
        except OSError as error:
            report['source_unchanged'] = False
            report['source_read_error'] = str(error)
        if not report['source_unchanged']:
            report['status'] = 'FAIL_SOURCE_CHANGED'
        (output / 'gpu-core-tests.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        if not report['source_unchanged']:
            raise RuntimeError('source changed while lifecycle tests ran')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', '--compiler', dest='compiler', default=os.environ.get('CXX', ''))
    parser.add_argument('--out', type=Path, help='Retain binary and honest policy report here')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    compiler = args.compiler or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        parser.error('C++17 compiler required')
    root = Path(__file__).resolve().parents[1]
    if args.out:
        return execute(compiler, root, args.out.resolve(), args.sanitize)
    with tempfile.TemporaryDirectory(prefix='mellowgpu-core-') as temp:
        return execute(compiler, root, Path(temp), args.sanitize)


if __name__ == '__main__':
    raise SystemExit(main())

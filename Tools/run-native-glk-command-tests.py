#!/usr/bin/env python3
"""Build and run the HOSTED NativeGlk command-encoding regression, and compile
the encoder as Darwin kernel C++ against MacKernelSDK. Never touches a GPU,
loads a kext or runs a generated command stream on hardware."""
import os
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys
from sanitizer_policy import sanitizer_environment

SOURCES = ['Drivers/NativeGlk/Gen9CommandEncoding.cpp', 'tests/native_glk_command_tests.cpp']
HEADERS = ['Drivers/NativeGlk/CommandEncoding.hpp']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-pedantic']


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, **kwargs)
    return dict(command=[str(part) for part in command], exit=result.returncode,
                output=(result.stdout + result.stderr)[-20000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--out', type=Path, required=True, help='Scratch build/report directory')
    parser.add_argument('--sdk', default=None, help='Host SDK for the hosted builds (default: xcrun)')
    args = parser.parse_args()
    if os.environ.get("CCC_OVERRIDE_OPTIONS"):
        raise SystemExit("CCC_OVERRIDE_OPTIONS changes compiler arguments; refusing evidence generation")
    root = Path(__file__).resolve().parent.parent
    out = args.out.resolve()
    if out == root or root in out.parents:
        parser.error('Build output must be outside the source tree')
    if not args.cxx:
        parser.error('Specify --cxx with an installed clang++')
    out.mkdir(parents=True, exist_ok=True)
    sdk = args.sdk
    if sdk is None and shutil.which('xcrun'):
        sdk = run(['xcrun', '--show-sdk-path'])['output'].strip()
    sysroot = ['-isysroot', sdk] if sdk else []
    inputs = [root / path for path in SOURCES + HEADERS + ['Tools/run-native-glk-command-tests.py', 'Tools/sanitizer_policy.py']]
    runtime_env, runtime_scope = sanitizer_environment()
    report = dict(scope='HOSTED encoding regression and kernel-flag compile only; no GPU, MMIO, '
                        'kext load, submission or hardware execution',
                  gpu_execution='NOT_RUN', hardware_admission=False,
                  generated=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  host=platform.platform(), host_sdk=sdk, compiler=run([args.cxx, '--version'])['output'].splitlines()[0],
                  source_sha256={path.relative_to(root).as_posix(): sha256(path) for path in inputs},
                  sanitizer_runtime=runtime_scope, steps=[])
    sources = [root / path for path in SOURCES]
    builds = [('host', ['-O2'], out / 'native_glk_command_tests'),
              ('asan-ubsan', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                              '-fno-omit-frame-pointer'], out / 'native_glk_command_tests_sanitized')]
    for name, flags, binary in builds:
        step = dict(name=name, compile=run([args.cxx, *sysroot, '-std=c++17', *flags, *WARNINGS, '-I', root,
                                            *sources, '-o', binary]))
        if step['compile']['exit'] == 0:
            if name == 'asan-ubsan':
                step['instrumentation_symbols'] = run(['nm', '-u', binary])
                symbols = step['instrumentation_symbols']['output']
                step['instrumentation_verified'] = '__asan_' in symbols and '__ubsan_' in symbols
            step['run'] = run([binary], timeout=900, env=runtime_env)
        report['steps'].append(step)

    # Darwin kernel C++ flags of Tools/cross-build.py, compile only (no link or load).
    clang = Path(args.cxx).with_name('clang')
    resource = run([clang, '-print-resource-dir'])['output'].strip()
    version = plistlib.loads((root / 'Mellow/Info.plist').read_bytes())['CFBundleVersion']
    kernel_object = out / 'CommandEncoding.kernel.o'
    kernel_flags = ['-target', 'x86_64-apple-macos13', '-std=c++17', '-mkernel', '-fapple-kext',
                    '-nostdinc', '-isystem', Path(resource) / 'include', '-I', root / 'MacKernelSDK/Headers',
                    '-I', root / 'Lilu.kext/Contents/Resources', '-DKERNEL', '-DKERNEL_PRIVATE',
                    '-DPRODUCT_NAME=MellowGPU', f'-DMODULE_VERSION={version}',
                    '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                    '-fno-asynchronous-unwind-tables', '-fvisibility=hidden',
                    '-fno-register-global-dtors-with-atexit', '-fno-use-cxa-atexit',
                    '-mllvm', '-disable-atexit-based-global-dtor-lowering',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unknown-warning-option',
                    '-Wno-ossharedptr-misuse', '-Wno-vla', '-O3']
    kernel = dict(name='kernel-compile', compile=run([clang, *kernel_flags, '-c',
                                                      root / 'Drivers/NativeGlk/Gen9CommandEncoding.cpp',
                                                      '-o', kernel_object]))
    if kernel['compile']['exit'] == 0:
        kernel['undefined_symbols'] = run(['nm', '-u', kernel_object])['output'].split()
        kernel['object_sha256'] = sha256(kernel_object)
    report['steps'].append(kernel)

    report['source_inputs_unchanged'] = all(sha256(path) == report['source_sha256'][path.relative_to(root).as_posix()] for path in inputs)
    passed = report['source_inputs_unchanged'] and all(step['compile']['exit'] == 0 and step.get('run', {'exit': 0})['exit'] == 0
                 and step.get('instrumentation_verified', True)
                 for step in report['steps'])
    report['all_passed'] = passed
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for step in report['steps']:
        status = step['compile']['exit'] or step.get('run', {'exit': 0})['exit']
        print(f"{step['name']}: {'PASS' if status == 0 else 'FAIL'}")
        if step['compile']['exit']:
            print(step['compile']['output'])
        elif 'run' in step:
            print(step['run']['output'].strip())
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())

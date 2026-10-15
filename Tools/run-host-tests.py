#!/usr/bin/env python3
"""Build and run portable driver contract tests; never loads a GPU driver."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('g++') or shutil.which('clang++'))
    parser.add_argument('--out', type=Path, required=True, help='Scratch build/report directory')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Specify --cxx with an installed C++ compiler')
    root = Path(__file__).resolve().parent.parent
    args.out.mkdir(parents=True, exist_ok=True)
    compiler = subprocess.run([args.cxx, '--version'], capture_output=True, text=True, check=True)
    results = []
    # accel_contracts_harness.cpp requires production function extraction and
    # has its own runner; it must not compile as an independent test program.
    suites = [
        ('tests/HardwareAccessTests.cpp', [], []),
        ('tests/modelTests.cpp', [], []),
        ('tests/patcher_policy_tests.cpp', [], []),
        ('tests/full_system_qualification_tests.cpp', [], []),
        ('tests/xe_guc_tests.cpp', ['Mellow/XeGuCTransport.cpp'], [root / 'Mellow']),
        ('tests/xe_guc_region_owner_tests.cpp',
         ['Mellow/XeGuCRegionOwner.cpp', 'Mellow/XeGgtt.cpp', 'Drivers/PortedXe/XePageTable.cpp',
          'Mellow/XeGuCFirmware.cpp', 'Mellow/XeFirmware.cpp', 'Mellow/XeGuCTransport.cpp'], [root]),
        ('tests/xe_guc_region_owner_tests.cpp',
         ['Mellow/XeGuCRegionOwner.cpp', 'Mellow/XeGgtt.cpp', 'Drivers/PortedXe/XePageTable.cpp',
          'Mellow/XeGuCFirmware.cpp', 'Mellow/XeFirmware.cpp', 'Mellow/XeGuCTransport.cpp',
          'Mellow/XeMemoryIOKit.cpp', 'Mellow/XeMemory.cpp'], [root / 'tests/native_memory_shim', root],
         ['-DKERNEL', '-DXE_REGION_OWNER_IOKIT_SHIM_TEST'], [], [], 'iokit-factory'),
        ('tests/ported_nvidia_test.cpp', ['Drivers/PortedNvidia/CommandEncoding.cpp'], [root]),
        ('tests/ported_nvidia_gsp_radix3_test.cpp', ['Drivers/PortedNvidiaGsp/Radix3.cpp'], [root]),
        ('tests/ported_nvidia_gsp_firmware_test.cpp', ['Drivers/PortedNvidiaGsp/FirmwareImage.cpp'], [root]),
        ('tests/native_nvidia_probe_tests.cpp', ['Drivers/NativeNvidia/Probe.cpp'], [root]),
        ('tests/native_gpu_memory_tests.cpp', ['Drivers/NativeGpu/MemoryOwner.cpp'], [root]),
        ('tests/native_nvidia_queue_tests.cpp',
         ['Drivers/NativeGpu/MemoryOwner.cpp', 'Drivers/NativeNvidia/GpFifoQueue.cpp',
          'Drivers/PortedNvidia/CommandEncoding.cpp'], [root]),
        ('tests/native_nvidia_iokit_tests.cpp',
         ['Drivers/NativeNvidia/Probe.cpp', 'Mellow/NvidiaMmioIOKit.cpp'],
         [root / 'tests/native_nvidia_shim', root]),
        ('tests/native_memory_iokit_tests.cpp',
         ['Drivers/NativeGpu/MemoryOwner.cpp', 'Mellow/NativeMemoryIOKit.cpp'],
         [root / 'tests/native_memory_shim', root]),
        ('tests/xe_dma_completion_tests.cpp',
         ['Mellow/XeMemoryIOKit.cpp', 'Mellow/XeMemory.cpp', 'Drivers/PortedXe/XePageTable.cpp'],
         [root / 'tests/native_memory_shim', root]),
        ('tests/native_nvidia_copy_pushbuffer_tests.cpp',
         ['Drivers/NativeNvidia/CopyPushbuffer.cpp', 'Drivers/NativeNvidia/GpFifoQueue.cpp',
          'Drivers/NativeGpu/MemoryOwner.cpp', 'Drivers/PortedNvidia/CommandEncoding.cpp'], [root]),
        ('Userspace/WindowServer/SurfaceSnapshotTests.cpp',
         ['Userspace/WindowServer/SurfaceSnapshot.cpp'], [root]),
        ('Userspace/WindowServer/RenderFixtureOracleTests.cpp', [], [root]),
        ('tests/opencl_runtime_regression.cpp',
         ['Runtime/PlatformRuntime.cpp', 'Runtime/OpenCLProvider.cpp'], [root],
         ['-DMELLOW_OPENCL_TESTING'], [] if sys.platform in ('win32', 'darwin') else ['-ldl']),
        ('tests/xe_context_execution_tests.cpp',
         ['Mellow/XeContextExecution.cpp', 'Mellow/XeContext.cpp', 'Mellow/XeDispatch.cpp',
          'Mellow/XeMemory.cpp', 'Mellow/XeZebin.cpp', 'Mellow/XeGuCTransport.cpp',
          'Mellow/XeFence.cpp', 'Drivers/PortedXe/XePageTable.cpp'], [root / 'Mellow'],
         [], [], ['compiler-evidence/mellow_evidence_mtl.bin']),
    ]
    for suite in suites:
        name, production_inputs, include_dirs = suite[:3]
        compile_flags = suite[3] if len(suite) > 3 else []
        link_flags = suite[4] if len(suite) > 4 else []
        runtime_inputs = suite[5] if len(suite) > 5 else []
        variant = suite[6] if len(suite) > 6 else None
        source = root / name
        inputs = [root / path for path in production_inputs] + [source]
        compile_inputs = [path.relative_to(root).as_posix() for path in inputs]
        source_sha256 = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in inputs}
        # Ask the compiler for each translation unit's actual non-system
        # dependencies, including IOKit shims and inline command encoders.
        # Snapshot before compilation and require unchanged content and an
        # unchanged dependency set afterward. A single -MF during multi-input
        # compilation would overwrite earlier translation units' dependencies.
        dependency_command = [args.cxx, '-std=c++17', *compile_flags,
                              *['-I' + str(path) for path in include_dirs],
                              '-MM', '-MT', 'mellow_test', *map(str, inputs)]

        def dependencies(result):
            if result.returncode != 0:
                return None
            body = result.stdout.replace('\\\n', '').replace('mellow_test:', '')
            paths = sorted({Path(name).resolve() for name in shlex.split(body)})
            if not paths or any(not path.is_file() or not path.is_relative_to(root) for path in paths):
                return None
            return paths

        dependency_before = subprocess.run(dependency_command, cwd=root, capture_output=True, text=True)
        dependency_paths = dependencies(dependency_before)
        dependency_sha256 = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in dependency_paths or []}
        runtime_sha256 = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                          for name in runtime_inputs}
        binary = args.out / (source.stem + ('-' + variant if variant else '') +
                             ('.exe' if sys.platform == 'win32' else ''))
        command = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   *compile_flags, *['-I' + str(path) for path in include_dirs],
                   *map(str, inputs), *link_flags, '-o', str(binary)]
        compile_result = subprocess.run(command, capture_output=True, text=True)
        test = dict(source=source.relative_to(root).as_posix(), variant=variant,
                    compile_exit=compile_result.returncode,
                    compile_output=compile_result.stdout + compile_result.stderr,
                    production_inputs=production_inputs, compile_inputs=compile_inputs,
                    compile_flags=compile_flags, link_flags=link_flags,
                    runtime_inputs=runtime_inputs,
                    runtime_input_sha256=runtime_sha256,
                    dependency_command=dependency_command,
                    dependency_before_exit=dependency_before.returncode,
                    dependency_before_output=dependency_before.stdout + dependency_before.stderr,
                    dependency_sha256=dependency_sha256,
                    source_sha256=source_sha256, compile_command=command)
        if compile_result.returncode == 0:
            try:
                run = subprocess.run([str(binary.resolve()), *[str(root / name) for name in runtime_inputs]],
                                     cwd=root, capture_output=True, text=True, timeout=30)
                test.update(run_exit=run.returncode, output=run.stdout + run.stderr)
            except subprocess.TimeoutExpired:
                test.update(run_exit=None, output='TIMEOUT after 30 seconds')
        dependency_after = subprocess.run(dependency_command, cwd=root, capture_output=True, text=True)
        test.update(dependency_after_exit=dependency_after.returncode,
                    dependency_set_unchanged=dependency_paths is not None and
                    dependency_paths == dependencies(dependency_after))
        test['inputs_unchanged'] = test['dependency_set_unchanged'] and all(
            path.is_file() and dependency_sha256[path.relative_to(root).as_posix()] ==
            hashlib.sha256(path.read_bytes()).hexdigest() for path in dependency_paths or []) and all(
            path.is_file() and source_sha256[path.relative_to(root).as_posix()] ==
            hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs) and all(
            runtime_sha256[name] == hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in runtime_inputs)
        if compile_result.returncode == 0:
            test['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        results.append(test)
        print(test['source'] + (' [' + variant + ']' if variant else ''),
              'PASS' if test.get('run_exit') == 0 and test['inputs_unchanged'] else 'FAIL')
        if test.get('output'): print(test['output'].strip())
        if compile_result.returncode: print(test['compile_output'])
    passed = bool(results) and all(x.get('run_exit') == 0 and x['inputs_unchanged'] for x in results)
    report = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        host=platform.platform(), compiler=compiler.stdout.splitlines()[0],
        scope='Portable driver helpers and contracts; no IOKit loader, firmware, GPU or Metal execution',
        source_hash_scope='Explicit compile inputs, compiler-reported local non-system dependencies and runtime data; system compiler headers/libraries are not hashed',
        hardware_execution=False, passed=passed, results=results)
    (args.out / 'host-tests.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 0 if passed else 1

if __name__ == '__main__':
    raise SystemExit(main())

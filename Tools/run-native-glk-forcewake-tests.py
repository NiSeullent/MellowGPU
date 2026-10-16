#!/usr/bin/env python3
"""Build/run the GLK Gen9 forcewake regressions; never loads a driver or touches hardware.

Host tests drive production protocol bodies over an emulated register file
(hosted injected transport, not hardware proof). Mutation checks copy the
production source into --out, apply one protocol deviation each and require
the suite to fail. Kernel units are compile-only against MacKernelSDK.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

CORE = 'Drivers/NativeGlk/Forcewake.cpp'
ADAPTER = 'Mellow/GlkForcewakeIOKit.cpp'
TEST = 'tests/native_glk_forcewake_tests.cpp'
IOKIT_TEST = 'tests/native_glk_forcewake_iokit_tests.cpp'
SOURCES = ['Drivers/NativeGlk/Forcewake.hpp', CORE, 'Mellow/GlkForcewakeIOKit.hpp', ADAPTER,
           'Mellow/HardwareAccess.hpp', TEST, IOKIT_TEST, 'tests/glk_forcewake_iokit_shim.hpp',
           'Drivers/NativeGlk/LICENSE.MIT', 'Drivers/NativeGlk/provenance.json',
           'Tools/run-native-glk-forcewake-tests.py']
HOST_FLAGS = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wno-sign-conversion',
              '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
KERNEL_FLAGS = ['-std=c++17', '-mkernel', '-fapple-kext', '-nostdinc', '-DKERNEL', '-DKERNEL_PRIVATE',
                '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
                '-fno-asynchronous-unwind-tables', '-fvisibility=hidden',
                '-fno-register-global-dtors-with-atexit', '-fno-use-cxa-atexit', '-mllvm',
                '-disable-atexit-based-global-dtor-lowering', '-Wall', '-Wextra',
                '-Wno-unknown-warning-option', '-Wno-ossharedptr-misuse', '-Wno-vla', '-O3',
                '-Werror', '-Wno-macro-redefined']
# (name, exact original text, replacement); each must occur exactly once.
MUTATIONS = [
    ('reset-uses-gen12-0xefff', 'write(domain.set, maskedDisable(ResetClearBitsGen9))',
     'write(domain.set, maskedDisable(0xefffU))'),
    ('fallback-9-passes', 'pass++ < FallbackMaxPasses', 'pass++ < FallbackMaxPasses - 1'),
    ('all-ones-ack-accepted', 'return value == AllOnes ? Status::MmioUnreliable : Status::Ok;',
     'return Status::Ok;'),
    ('blind-request-without-ack-clear', 'status = waitKernelAck(Gen9Domains[i], false, deadlineUs);',
     'status = Status::Ok;'),
    ('no-fallback-workaround', 'status = ackWithFallback(domain, set, deadlineUs, detected);',
     'detected = false;'),
    ('no-final-wait-after-fallback', '\n    status = waitAck(domain, ForcewakeKernel, value, deadlineUs, matched);',
     '\n    matched = false;'),
    ('no-rollback', 'if ((requested & bit(i)) &&', 'if (false && (requested & bit(i)) &&'),
    ('fallback-bit-left-set', 'const Status cleared = write(domain.set, maskedDisable(ForcewakeKernelFallback));',
     'const Status cleared = Status::Ok;'),
    ('constant-relax-delay', 'FallbackRelaxStepUs * pass', 'FallbackRelaxStepUs'),
    ('gen9-table-typo', '{0x8130, 0x813f, M}', '{0x8130, 0x813f, G}'),
    ('shadow-table-typo', '{0xa008, 0xa00c}', '{0xa008, 0xa008}'),
    ('no-reference-count', '--wakeCount_[i] == 0', '(--wakeCount_[i], true)'),
    ('init-without-mmio-sanity', 'if (value != AllOnes) break;', 'break;'),
    ('timeout-reported-success', 'return set ? Status::AckSetTimeout : Status::AckClearTimeout;',
     'return Status::Ok;'),
    ('half-ack-timeout', 'const uint64_t budget = limited ? remaining : ForcewakeAckTimeoutUs;',
     'const uint64_t budget = limited ? remaining : ForcewakeAckTimeoutUs / 2;'),
    ('deadline-cut-as-hw-timeout', 'return limited ? Status::DeadlineExceeded : Status::Ok;',
     'return Status::Ok;'),
    ('owner-open-not-required', 'if (!actual.providerOpen) return Status::Ownership;', ''),
    ('lease-generation-ignored', 'if (!slot.live || slot.generation != lease.generation)',
     'if (!slot.live)'),
]
ADAPTER_MUTATIONS = [
    ('open-skips-prevalidation', 'if (status != Status::Ok) return status;\n    window_ = true;',
     'window_ = true;'),
    ('close-ignores-lost-authority', 'if (status == Status::Ok) return result;', 'return result;'),
    ('bar-bdf-not-checked', '(tag & 0x00ffff00U) != addressSpaceBdf(pci) || ', ''),
    ('tunnel-ignored', 'out.tunneled = tunnel != nullptr;', 'out.tunneled = false;'),
    ('udelay-dropped', 'IODelay(microseconds);', '(void)microseconds;'),
    # Removing just the early physical check is equivalent: the post-map
    # authority snapshot checks it again. Test an actual unsafe write instead.
    ('mmio-write-offset-corrupted', 'MellowHardware::write32(self.base_, self.length_, offset, value)',
     'MellowHardware::write32(self.base_, self.length_, offset + 4, value)'),
    ('owner-retain-omitted', '    owner_->retain();\n', ''),
]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=300, **kwargs)
    return {'command': [str(c) for c in command], 'exit': result.returncode,
            'stdout': result.stdout[-4000:], 'stderr': result.stderr[-4000:]}


def primary_oracle(args, root, out, sysroot):
    manifest = json.loads((root / 'Drivers/NativeGlk/provenance.json').read_text())
    if manifest['revision'] != '0d9ff90a5422cc7509258aaaba1e7481df4d332a':
        raise ValueError('Unexpected primary revision')
    texts = {}
    for entry in manifest['upstream_files']:
        path = (args.linux_root / entry['path']).resolve()
        if not path.is_relative_to(args.linux_root.resolve()):
            raise ValueError('Primary path escapes cache')
        data = path.read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if len(data) != entry['bytes'] or sha256(path) != entry['sha256'] or blob != entry['git_blob']:
            raise ValueError('Primary cache mismatch: ' + entry['path'])
        texts[entry['path']] = data.decode()
    uncore = texts['drivers/gpu/drm/i915/intel_uncore.c']
    registers = texts['drivers/gpu/drm/i915/gt/intel_gt_regs.h']
    header = texts['drivers/gpu/drm/i915/intel_uncore.h']
    if not re.search(r'FW_DOMAIN_ID_RENDER = 0,\s*FW_DOMAIN_ID_GT,\s*/\*.*?\*/\s*FW_DOMAIN_ID_MEDIA,', header):
        raise ValueError('Unexpected domain numbering')
    body = re.search(r'__gen9_fw_ranges\[\] = \{(.*?)\n\};', uncore, re.S).group(1)
    rows = re.findall(r'GEN_FW_RANGE\((0x[0-9a-f]+), (0x[0-9a-f]+), ([\w |]+)\)', body)
    if len(rows) != 32:
        raise ValueError('Incomplete Gen9 table')
    domains = {'0': 0, 'FORCEWAKE_RENDER': 1, 'FORCEWAKE_GT': 2, 'FORCEWAKE_MEDIA': 4}
    ranges = [(int(a, 16), int(b, 16), sum(domains[x.strip()] for x in d.split('|'))) for a, b, d in rows]
    shadow_body = re.search(r'gen8_shadowed_regs\[\] = \{(.*?)\n\};', uncore, re.S).group(1)
    shadows = [(int(a, 16), int(b, 16)) for a, b in re.findall(
        r'\.start =\s*(0x[0-9a-fA-F]+), \.end =\s*(0x[0-9a-fA-F]+)', shadow_body)]
    if len(shadows) != 5 or '__reg < 0x40000 || __reg >= 0x116000;' not in uncore:
        raise ValueError('Unexpected shadow table or domain predicate')
    constants = []
    for native, upstream in [('ForcewakeRenderGen9', 'FORCEWAKE_RENDER_GEN9'),
            ('ForcewakeGtGen9', 'FORCEWAKE_GT_GEN9'), ('ForcewakeMediaGen9', 'FORCEWAKE_MEDIA_GEN9'),
            ('ForcewakeAckRenderGen9', 'FORCEWAKE_ACK_RENDER_GEN9'),
            ('ForcewakeAckGtGen9', 'FORCEWAKE_ACK_GT_GEN9'), ('ForcewakeAckMediaGen9', 'FORCEWAKE_ACK_MEDIA_GEN9')]:
        value = re.search(r'^#define\s+' + upstream + r'\s+_MMIO\((0x[0-9a-f]+)\)', registers, re.M).group(1)
        constants.append('static_assert(%s == %s, "pinned register");' % (native, value))
    ack_ms = int(re.search(r'^#define FORCEWAKE_ACK_TIMEOUT_MS (\d+)', uncore, re.M).group(1))
    passes = int(re.search(r'pass\+\+ < (\d+)', uncore).group(1))
    delay = int(re.search(r'udelay\((\d+) \* pass\)', uncore).group(1))
    constants += ['static_assert(ForcewakeAckTimeoutUs == %d, "pinned timeout");' % (ack_ms * 1000),
                  'static_assert(FallbackMaxPasses == %d, "pinned passes");' % passes,
                  'static_assert(FallbackRelaxStepUs == %d, "pinned delay");' % delay]
    source = '\n'.join(['// Generated numeric oracle from verified MIT primary files; host-only.',
        '#include "Forcewake.hpp"', '#include <cstdio>', 'using namespace MellowNativeGlk;', *constants,
        'struct Range { unsigned first,last,domains; };',
        'static const Range ranges[] = {' + ','.join('{%d,%d,%d}' % row for row in ranges) + '};',
        'static const Range shadows[] = {' + ','.join('{%d,%d,0}' % row for row in shadows) + '};',
        'int main(){ unsigned checks=0, failures=0;',
        'for(unsigned reg=0;reg<0x200000;++reg){ unsigned expected=0;',
        'for(const auto&r:ranges)if(reg>=r.first&&reg<=r.last)expected=r.domains;',
        '++checks; if(gen9ReadDomains(reg)!=expected)++failures;',
        'for(const auto&r:shadows)if(reg>=r.first&&reg<=r.last)expected=0;',
        '++checks; if(gen9WriteDomains(reg)!=expected)++failures;}',
        'std::printf("pinned_forcewake_oracle: %u checks, %u failures\\n",checks,failures);',
        'return failures?1:0;}'])
    path = out / 'pinned-forcewake-oracle.cpp'
    path.write_text(source)
    binary = out / 'pinned-forcewake-oracle'
    result = {'verified_primary_files': len(texts), 'gen9_ranges': len(ranges),
              'shadow_ranges': len(shadows), 'source_sha256': sha256(path),
              'compile': run([args.cxx, *sysroot, *HOST_FLAGS, '-I', root / 'Drivers/NativeGlk',
                              path, root / CORE, '-o', binary])}
    if result['compile']['exit'] == 0:
        result['execution'] = run([binary])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++'))
    parser.add_argument('--sysroot', help='Host SDK for test binaries (xcrun --show-sdk-path)')
    parser.add_argument('--out', type=Path, required=True, help='Scratch build/report directory')
    parser.add_argument('--skip-mutations', action='store_true')
    parser.add_argument('--linux-root', type=Path, required=True,
                        help='Verified primary Linux cache matching provenance.json')
    args = parser.parse_args()
    if 'CCC_OVERRIDE_OPTIONS' in os.environ:
        parser.error('CCC_OVERRIDE_OPTIONS must be absent, including empty values')
    if not args.cxx:
        parser.error('Specify --cxx')
    root = Path(__file__).resolve().parent.parent
    out = args.out.resolve()
    if out == root or root in out.parents or out in root.parents:
        parser.error('--out must be outside the source tree')
    if args.out.is_symlink() or (out.exists() and (not out.is_dir() or any(out.iterdir()))):
        parser.error('--out must be new or empty')
    out.mkdir(parents=True, exist_ok=True)
    source_names = SOURCES + sorted(p.relative_to(root).as_posix()
        for p in (root / 'tests/glk_forcewake_shim').rglob('*') if p.is_file())
    sysroot = ['-isysroot', args.sysroot] if args.sysroot else []
    report = {
        'scope': 'Hosted injected-transport protocol tests plus compile-only kernel units; '
                 'no driver load, no hardware access, no qualification',
        'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'compiler': run([args.cxx, '--version'])['stdout'].splitlines()[:2],
        'sources': {name: sha256(root / name) for name in source_names},
    }
    binary = out / 'native_glk_forcewake_tests'
    build = run([args.cxx, *sysroot, *HOST_FLAGS, '-I', root, root / TEST, root / CORE, '-o', binary])
    report['host_build'] = build
    report['host_run'] = run([binary]) if build['exit'] == 0 else {'exit': None, 'stdout': 'NOT_RUN'}
    report['host_sanitizer_imports'] = run(['nm', '-u', binary]) if build['exit'] == 0 else {'exit': None}

    shim_binary = out / 'native_glk_forcewake_iokit_tests'
    shim_include = ['-I', root / 'tests/glk_forcewake_shim', '-I', root / 'Mellow', '-I', root]
    shim_build = run([args.cxx, *sysroot, *HOST_FLAGS, *shim_include,
                      root / IOKIT_TEST, root / CORE, root / ADAPTER, '-o', shim_binary])
    report['iokit_host_build'] = shim_build
    report['iokit_host_run'] = run([shim_binary]) if shim_build['exit'] == 0 else {'exit': None, 'stdout': 'NOT_RUN'}
    report['iokit_sanitizer_imports'] = run(['nm', '-u', shim_binary]) if shim_build['exit'] == 0 else {'exit': None}
    report['primary_oracle'] = primary_oracle(args, root, out, sysroot)

    mutants = []
    if not args.skip_mutations:
        original = (root / CORE).read_text()
        for name, before, after in MUTATIONS:
            entry = {'name': name, 'occurrences': original.count(before)}
            if entry['occurrences'] != 1:
                entry['result'] = 'INVALID_MUTATION'
                mutants.append(entry)
                continue
            directory = out / 'mutants' / name
            directory.mkdir(parents=True, exist_ok=True)
            source = directory / 'Forcewake.cpp'
            source.write_text(original.replace(before, after))
            shutil.copy(root / 'Drivers/NativeGlk/Forcewake.hpp', directory / 'Forcewake.hpp')
            program = directory / 'mutant_tests'
            compiled = run([args.cxx, *sysroot, *HOST_FLAGS, '-Wno-unused-parameter', '-Wno-unused-variable',
                            '-Wno-unused-function', '-Wno-unused-const-variable', '-Wno-unreachable-code',
                            '-I', root, root / TEST, source, '-o', program])
            if compiled['exit'] != 0:
                entry['result'] = 'BUILD_FAILED'
                entry['stderr'] = compiled['stderr'][-800:]
            else:
                executed = run([program])
                entry['exit'] = executed['exit']
                entry['summary'] = (executed['stdout'].strip().splitlines() or [''])[-1]
                entry['result'] = 'KILLED' if executed['exit'] != 0 else 'SURVIVED'
            mutants.append(entry)
    report['mutations'] = mutants

    adapter_mutants = []
    if not args.skip_mutations:
        original = (root / ADAPTER).read_text()
        for name, before, after in ADAPTER_MUTATIONS:
            entry = {'name': name, 'occurrences': original.count(before)}
            if entry['occurrences'] != 1:
                entry['result'] = 'INVALID_MUTATION'
                adapter_mutants.append(entry)
                continue
            directory = out / 'adapter-mutants' / name
            directory.mkdir(parents=True)
            source = directory / 'GlkForcewakeIOKit.cpp'
            source.write_text(original.replace(before, after))
            program = directory / 'mutant_tests'
            compiled = run([args.cxx, *sysroot, *HOST_FLAGS, '-Wno-unused-parameter', '-Wno-unused-variable',
                            '-Wno-unused-function', *shim_include, root / IOKIT_TEST, root / CORE, source, '-o', program])
            entry['compile'] = compiled
            if compiled['exit'] != 0:
                entry['result'] = 'BUILD_FAILED'
            else:
                entry['execution'] = run([program])
                entry['result'] = 'KILLED' if entry['execution']['exit'] != 0 else 'SURVIVED'
            adapter_mutants.append(entry)
    report['adapter_mutations'] = adapter_mutants

    resource = run([args.cxx, '-print-resource-dir'])['stdout'].strip()
    kernel = []
    for target in ['x86_64-apple-macos13', 'x86_64-apple-macos15']:
        for unit in [CORE, ADAPTER]:
            obj = out / 'kernel' / target / (Path(unit).stem + '.o')
            obj.parent.mkdir(parents=True, exist_ok=True)
            entry = run([args.cxx, '-target', target, *KERNEL_FLAGS, '-isystem', Path(resource) / 'include',
                         '-I', root / 'MacKernelSDK/Headers', '-c', root / unit, '-o', obj])
            entry.update({'unit': unit, 'target': target,
                          'object_sha256': sha256(obj) if entry['exit'] == 0 else None})
            if entry['exit'] == 0:
                symbols = run(['nm', '-u', obj])['stdout'].split()
                entry['undefined_symbols'] = sorted(s for s in symbols if s not in ('U',))
            kernel.append(entry)
    report['kernel_compile'] = kernel
    report['sources_after'] = {name: sha256(root / name) for name in source_names}
    report['sources_unchanged'] = report['sources'] == report['sources_after']

    killed = sum(1 for m in mutants if m.get('result') == 'KILLED')
    report['summary'] = {
        'host_tests_passed': report['host_run'].get('exit') == 0,
        'host_output': report['host_run'].get('stdout', '').strip(),
        'iokit_tests_passed': report['iokit_host_run'].get('exit') == 0,
        'iokit_output': report['iokit_host_run'].get('stdout', '').strip(),
        'primary_oracle_passed': report['primary_oracle'].get('execution', {}).get('exit') == 0,
        'adapter_mutants_killed': sum(m.get('result') == 'KILLED' for m in adapter_mutants),
        'adapter_mutants_total': len(adapter_mutants),
        'mutants_killed': killed, 'mutants_total': len(mutants),
        'kernel_units_compiled': sum(1 for k in kernel if k['exit'] == 0), 'kernel_units_total': len(kernel),
        'hardware': 'NOT_RUN (no hardware operation authorized)',
    }
    ok = report['summary']['host_tests_passed'] and killed == len(mutants) and \
        report['summary']['kernel_units_compiled'] == len(kernel) and \
        report['summary']['iokit_tests_passed'] and report['summary']['primary_oracle_passed'] and \
        all(m.get('result') == 'KILLED' for m in adapter_mutants) and report['sources_unchanged']
    for name in ('host_sanitizer_imports', 'iokit_sanitizer_imports'):
        symbols = report[name].get('stdout', '')
        ok = ok and report[name]['exit'] == 0 and '__asan_init' in symbols and '__ubsan_handle' in symbols
    report['status'] = 'PASS_HOSTED_AND_KERNEL_COMPILE_ONLY' if ok else 'FAIL'
    (out / 'native-glk-forcewake-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

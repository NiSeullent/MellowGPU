#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test the GLK BCS0 execlists engine and native IOKit adapter bodies.

HOSTED INJECTED TRANSPORT and compile-only native objects: no driver load,
MMIO, DMA, interrupt, PCI access, reset or GPU execution happens.
"""
import argparse, datetime, hashlib, json, os, re, shutil, subprocess
from pathlib import Path

SOURCES = ['Drivers/NativeGlk/BcsExecution.hpp', 'Drivers/NativeGlk/BcsExecution.cpp',
           'Drivers/NativeGlk/BcsExecution.provenance.json', 'Drivers/NativeGlk/BcsExecution.LICENSE.MIT',
           'Mellow/GlkBcsIOKit.hpp', 'Mellow/GlkBcsIOKit.cpp', 'tests/native_glk_bcs_tests.cpp',
           'tests/native_glk_bcs_iokit_tests.cpp', 'tests/native_glk_bcs_iokit_shim.hpp',
           'tests/native_glk_bcs_shim/IOKit/IOLib.h', 'tests/native_glk_bcs_shim/IOKit/IOMemoryDescriptor.h',
           'tests/native_glk_bcs_shim/IOKit/pci/IOPCIDevice.h', 'Tools/run-native-glk-bcs-tests.py']
# Canonical GLK command body is Gen9CommandEncoding.cpp (distinct from NVIDIA CommandEncoding.o in Xcode).
SEEDED = ['Drivers/NativeGlk/Gen9CommandEncoding.cpp', 'Drivers/NativeGlk/CommandEncoding.hpp',
          'Drivers/NativeGlk/LrcImage.cpp', 'Drivers/NativeGlk/LrcImage.hpp',
          'Drivers/NativeGpu/MemoryOwner.cpp', 'Drivers/NativeGpu/MemoryOwner.hpp']
ENGINE = ['Drivers/NativeGlk/BcsExecution.cpp', 'Drivers/NativeGlk/LrcImage.cpp', 'Drivers/NativeGlk/Gen9CommandEncoding.cpp']
# (upstream file, regex with one value group, C++ constant it must equal)
ORACLES = [
 ('gt/intel_engine_regs.h', r'#define RING_ELSP\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::Elsp - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_CONTEXT_STATUS_PTR\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::ContextStatusPtr - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_HWS_PGA\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::HwsPga - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_HWSTAM\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::Hwstam - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_MI_MODE\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::MiMode - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_MODE_GEN7\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::ModeGen7 - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_EIR\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::Eir - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_EMR\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::Emr - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_ESR\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::Esr - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_RESET_CTL\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::ResetCtl - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_HEAD\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::RingHead - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define RING_TAIL\(base\)\s+_MMIO\(\(base\) \+ (0x[0-9a-f]+)\)', 'B::Reg::RingTail - B::MmioBase'),
 ('gt/intel_engine_regs.h', r'#define\s+HEAD_ADDR\s+(0x[0-9A-F]+)', 'B::Bit::HeadAddr'),
 ('gt/intel_engine_regs.h', r'#define\s+TAIL_ADDR\s+(0x[0-9A-F]+)', 'B::Bit::TailAddr'),
 ('gt/intel_engine_regs.h', r'#define\s+MODE_IDLE\s+REG_BIT\((\d+)\)', '__builtin_ctz(B::Bit::ModeIdle)'),
 ('gt/intel_engine_regs.h', r'#define\s+STOP_RING\s+REG_BIT\((\d+)\)', '__builtin_ctz(B::Bit::StopRing)'),
 ('gt/intel_engine_regs.h', r'#define\s+GFX_RUN_LIST_ENABLE\s+\(1 << (\d+)\)', '__builtin_ctz(B::Bit::GfxRunListEnable)'),
 ('gt/intel_engine_regs.h', r'#define\s+RESET_CTL_CAT_ERROR\s+REG_BIT\((\d+)\)', '__builtin_ctz(B::Bit::ResetCatError)'),
 ('gt/intel_engine_regs.h', r'#define\s+RESET_CTL_READY_TO_RESET\s+REG_BIT\((\d+)\)', '__builtin_ctz(B::Bit::ResetReady)'),
 ('gt/intel_engine_regs.h', r'#define\s+RESET_CTL_REQUEST_RESET\s+REG_BIT\((\d+)\)', '__builtin_ctz(B::Bit::ResetRequest)'),
 ('gt/intel_lrc_reg.h', r'#define GEN8_EXECLISTS_STATUS_BUF (0x[0-9a-f]+)', 'B::Reg::ExeclistsStatusBuf - B::MmioBase'),
 ('gt/intel_lrc_reg.h', r'#define CTX_RING_HEAD\s+\((0x[0-9a-f]+) \+ 1\)', 'B::ContextRingHeadDword - 1'),
 ('gt/intel_engine.h', r'#define I915_HWS_CSB_BUF0_INDEX\s+(0x[0-9a-f]+)', 'B::HwspCsbBufferDword'),
 ('gt/intel_engine.h', r'#define I915_HWS_CSB_WRITE_INDEX\s+(0x[0-9a-f]+)', 'B::HwspCsbWriteDword'),
 ('gt/intel_engine.h', r'#define I915_GEM_HWS_PREEMPT\s+(0x[0-9a-f]+)', 'B::HwspPreemptDword'),
 ('gt/intel_engine.h', r'#define I915_GEM_HWS_SEQNO\s+(0x[0-9a-f]+)', 'B::HwspSeqnoDword'),
 ('i915_reg.h', r'#define BLT_RING_BASE\s+(0x[0-9a-f]+)', 'B::MmioBase'),
 ('i915_reg.h', r'#define\s+I915_ERROR_INSTRUCTION\s+\(1 << (\d+)\)', '__builtin_ctz(B::Bit::ErrorInstruction)'),
]
ORDER_FACTS = [  # exact upstream statements whose order the bodies rely on
 ('gt/intel_execlists_submission.c', r'\} else \{\s*writel\(upper_32_bits\(desc\), execlists->submit_reg\);\s*writel\(lower_32_bits\(desc\), execlists->submit_reg\);'),
 ('gt/intel_execlists_submission.c', r'for \(n = execlists_num_ports\(execlists\); n--; \)'),
 ('gt/intel_execlists_submission.c', r'WRITE_ONCE\(\*csb, -1\);'),
 ('gt/intel_execlists_submission.c', r'execlists->csb_head = tail;[\s\S]{0,900}?rmb\(\);'),
 ('gt/intel_execlists_submission.c', r'REG_MASKED_FIELD_ENABLE\(GFX_RUN_LIST_ENABLE\)'),
 ('gt/intel_execlists_submission.c', r'ENGINE_WRITE\(engine, RING_EMR, ~I915_ERROR_INSTRUCTION\);'),
 ('gt/intel_engine_cs.c', r'intel_uncore_write_fw\(uncore, mode, REG_MASKED_FIELD_ENABLE\(STOP_RING\)\);'),
 ('gt/intel_reset.c', r'__intel_wait_for_register_fw\(uncore, reg, mask, ack,\s*700, 0, NULL\);'),
]
MUTANTS = [  # RED: each must make the engine test fail
 ('ELSP port 0 before port 1', 'for (const Lrc::MmioWrite &w : plan.writes) write(w.offset, w.value);',
  'for (uint32_t m = 4; m--;) write(plan.writes[m].offset, plan.writes[m].value);'),
 ('nonce not required', 'e.breadcrumb && e.nonce && e.savedHead', 'e.breadcrumb && e.savedHead'),
 ('context ID not checked', 'if (identity != Lrc::Status::Ok) return fault(Status::StaleIdentity);', '(void)identity;'),
 ('CSB entry not consumed', 'setHwspQword(dword, ~0ULL);', '(void)dword;'),
 ('no wmb before ELSP', 'fence(Fence::Store); // wmb(): context image', '// removed'),
 ('breadcrumb not required', 'e.promoted && e.csbCompleted && e.breadcrumb', 'e.promoted && e.csbCompleted'),
 ('all-ones accepted', 'if (value == AllOnes) { ++readFailures_; return false; }', ''),
]

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(argv, timeout=180, env=None):
    env = dict(os.environ if env is None else env); env.pop('CCC_OVERRIDE_OPTIONS', None)
    try:
        r = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=timeout, env=env)
        return {'argv': list(map(str, argv)), 'exit_code': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr}
    except subprocess.TimeoutExpired as e:
        return {'argv': list(map(str, argv)), 'exit_code': None, 'timeout': timeout, 'stdout': str(e.stdout or ''), 'stderr': str(e.stderr or '')}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cxx', default=shutil.which('clang++'))
    p.add_argument('--linux-i915', type=Path, required=True, help='pinned cache of drivers/gpu/drm/i915')
    p.add_argument('--seed-manifest', type=Path)
    p.add_argument('--sdk', type=Path, help='exact MacOSX15.5.sdk for native x86_64 kernel object compile')
    a = p.parse_args(); root = Path(__file__).resolve().parents[1]; out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False); os.chdir(root)
    report = {'schema': 'mellowgpu.glk-bcs-tests/1', 'status': 'FAIL', 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'scope': 'hosted injected transport + native SDK object compile; no hardware',
              'hardware_executed': False, 'mmio_executed': False, 'dma_executed': False, 'gpu_executed': False,
              'native_driver_loaded': False, 'hardware_support_qualified': False, 'metal_qualified': False,
              'windowserver_qualified': False, 'sanitizers': ['address', 'undefined'], 'fatal_sanitizers': True}
    before = {n: sha(root / n) for n in SOURCES + SEEDED}
    try:
        if a.seed_manifest:
            seeds = json.loads(a.seed_manifest.read_text())
            drift = [n for n, h in seeds.items() if sha(root / n) != h]
            report['seed_inputs'] = {'checked': len(seeds), 'drift': drift}
            if drift: raise ValueError('seeded inputs changed')
        prov = json.loads((root / SOURCES[2]).read_text())
        cache = [f for f in prov['files'] if sha(f['cache_path']) != f['sha256']]
        report['provenance_files'] = len(prov['files'])
        if cache: raise ValueError('pinned upstream cache drift: %s' % [f['path'] for f in cache])
        lines, facts = [], []
        for rel, rx, expr in ORACLES:
            m = re.search(rx, (a.linux_i915 / rel).read_text())
            if not m: raise ValueError('oracle missing: ' + rx)
            label = re.sub(r'[^A-Za-z0-9_/. ]', '', rel + ' ' + rx)[:60]
            lines.append(f'static_assert(uint64_t({expr}) == uint64_t({m.group(1)}), "{label}");')
        for rel, rx in ORDER_FACTS:
            if not re.search(rx, (a.linux_i915 / rel).read_text()): raise ValueError('ordering fact missing: ' + rx)
            facts.append(rel + ': ' + rx)
        oracle = out / 'upstream_oracle.cpp'
        oracle.write_text('#include "Drivers/NativeGlk/BcsExecution.hpp"\nnamespace B = MellowNativeGlk::Bcs;\n' + '\n'.join(lines) + '\n')
        report['upstream_oracle'] = run([a.cxx, '-std=c++17', '-fsyntax-only', '-I', root, oracle])
        report['upstream_oracle']['constants'] = len(lines); report['ordering_facts'] = facts
        if report['upstream_oracle']['exit_code'] != 0: raise ValueError('header constants differ from pinned upstream')
        common = [a.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', root]
        san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:halt_on_error=1:detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        suites = {'engine': (['tests/native_glk_bcs_tests.cpp', *ENGINE], [], 'native_glk_bcs_tests'),
                  'iokit': (['tests/native_glk_bcs_iokit_tests.cpp', 'Mellow/GlkBcsIOKit.cpp', *ENGINE, 'Drivers/NativeGpu/MemoryOwner.cpp'],
                            ['-DMELLOW_GLK_BCS_HOSTED', '-Itests/native_glk_bcs_shim'], 'native_glk_bcs_iokit_tests')}
        report['suites'] = {}
        for name, (inputs, extra, label) in suites.items():
            binary = out / label; s = {}
            s['build'] = run([*common, *extra, *san, *inputs, '-o', binary])
            if s['build']['exit_code'] != 0: raise ValueError(name + ' build failed')
            s['imports'] = run(['xcrun', 'nm', '-u', binary])
            if '___asan_init' not in s['imports']['stdout'] or '___ubsan_handle_' not in s['imports']['stdout']:
                raise ValueError(name + ': sanitizer instrumentation missing')
            s['imports']['stdout'] = '(asan/ubsan imports verified)'
            s['binary_sha256'] = sha(binary); s['test'] = run([binary], 60, env)
            m = re.fullmatch(label + r': ([0-9]+) checks passed\n', s['test']['stdout'])
            if s['test']['exit_code'] != 0 or s['test']['stderr'] or not m: raise ValueError(name + ' tests failed')
            s['checks'] = int(m.group(1)); report['suites'][name] = s
        report['mutants'] = []
        source = (root / ENGINE[0]).read_text()
        for title, old, new in MUTANTS:
            if source.count(old) != 1: raise ValueError('mutant anchor not unique: ' + title)
            mroot = out / ('mutant-%d' % len(report['mutants'])); (mroot / 'Drivers/NativeGlk').mkdir(parents=True)
            for h in ['BcsExecution.hpp', 'CommandEncoding.hpp', 'LrcImage.hpp']:
                shutil.copy(root / 'Drivers/NativeGlk' / h, mroot / 'Drivers/NativeGlk' / h)
            (mroot / 'Drivers/NativeGlk/BcsExecution.cpp').write_text(source.replace(old, new))
            binary = mroot / 'mutant'
            build = run([a.cxx, '-std=c++17', '-O1', '-w', *san, '-I', mroot, 'tests/native_glk_bcs_tests.cpp',
                         mroot / 'Drivers/NativeGlk/BcsExecution.cpp', ENGINE[1], ENGINE[2], '-o', binary])
            test = run([binary], 60, env) if build['exit_code'] == 0 else None
            killed = build['exit_code'] == 0 and test['exit_code'] != 0
            report['mutants'].append({'mutant': title, 'built': build['exit_code'] == 0, 'killed': killed,
                                      'failure': (test['stderr'].splitlines() or [''])[0] if test else build['stderr'][-300:]})
            if not killed: raise ValueError('mutant survived: ' + title)
        if a.sdk:
            sdk = a.sdk.resolve(); settings = json.loads((sdk / 'SDKSettings.json').read_text())
            if settings.get('Version') != '15.5': raise ValueError('native compile requires exact SDK15.5')
            res = run([a.cxx, '-print-resource-dir'])['stdout'].strip()
            flags = [a.cxx, '-target', 'x86_64-apple-macos15.5', '-std=c++17', '-mkernel', '-fapple-kext', '-nostdinc',
                     '-isystem', res + '/include', '-isystem', sdk / 'System/Library/Frameworks/Kernel.framework/Headers',
                     '-isysroot', sdk, '-DKERNEL', '-DKERNEL_PRIVATE', '-fno-exceptions', '-fno-rtti', '-fno-builtin',
                     '-fno-common', '-fno-asynchronous-unwind-tables', '-fvisibility=hidden', '-fno-use-cxa-atexit',
                     '-fno-threadsafe-statics', '-Wall', '-Wextra', '-Werror', '-Wno-unknown-warning-option',
                     '-Wno-ossharedptr-misuse', '-Wno-deprecated-declarations', '-I', root]
            native = {'sdk_settings_sha256': sha(sdk / 'SDKSettings.json'), 'objects': {},
                      'scope': 'x86_64 kernel object compile only; not linked, loaded or executed',
                      'warning_exception': 'public SDK marks IOPCIDevice deprecated in favor of PCIDriverKit'}
            for src in ['Mellow/GlkBcsIOKit.cpp', *ENGINE]:
                obj = out / (Path(src).stem + '.sdk155.o'); r = run([*flags, '-c', src, '-o', obj])
                if r['exit_code'] != 0 or r['stderr']: raise ValueError('native compile failed: ' + src + r['stderr'][-400:])
                native['objects'][src] = {'object_sha256': sha(obj), 'undefined': run(['xcrun', 'nm', '-u', obj])['stdout'].split()}
            report['native'] = native
        report['status'] = 'PASS_HOSTED_AND_NATIVE_OBJECT' if a.sdk else 'PASS_HOSTED_ONLY'
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as e:
        report['error'] = str(e)
    finally:
        after = {n: sha(root / n) for n in SOURCES + SEEDED}
        report['source_sha256'] = before; report['source_unchanged'] = after == before
        if not report['source_unchanged']: report['status'] = 'FAIL_SOURCE_CHANGED'
        (out / 'result.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'status': report['status'], 'checks': {k: v.get('checks') for k, v in report.get('suites', {}).items()},
                      'mutants_killed': sum(m['killed'] for m in report.get('mutants', [])), 'error': report.get('error'),
                      'hardware_support_qualified': False}))
    return 0 if report['status'].startswith('PASS_') else 1

if __name__ == '__main__': raise SystemExit(main())

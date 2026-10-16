#!/usr/bin/env python3
# Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
"""Build and run the GLK panel power sequencer checks.

hosted: injected-MMIO software model (not hardware evidence), built with
-Werror and again with ASan/UBSan. kernel: compile-only of PanelPower.cpp with
Darwin kext flags against this repo's MacKernelSDK; no link, load or run.
mutants: source mutations that the hosted suite must reject.
Every artifact goes under --out; nothing touches hardware.
"""
import os
import argparse, hashlib, json, pathlib, shutil, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "Tools"))
from sanitizer_policy import sanitizer_environment
RUNTIME_ENV, RUNTIME_SCOPE = sanitizer_environment()

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / 'Drivers/NativeGlk/PanelPower.cpp'
HDR = ROOT / 'Drivers/NativeGlk/PanelPower.hpp'
TEST = ROOT / 'tests/native_glk_panel_power_tests.cpp'
MUTANTS = [
    ('legacy PPS base', 'PchPpsBase - LegacyPpsBase', 'LegacyPpsBase - LegacyPpsBase'),
    ('cycle field without +1', '(powerCycleUnits + 999) / 1000 + 1', '(powerCycleUnits + 999) / 1000'),
    ('hardware cycle decode', '(field - 1) * 1000', 'field * 1000'),
    ('ms rounding down', '(units + 9) / 10', 'units / 10'),
    ('no spec fallback', 'const PpsDelays spec = specificationDelays();', 'const PpsDelays spec {};'),
    ('backlight registers not forced to 1', 't.registers.backlightOn = 1', 't.registers.backlightOn = chosen[BacklightOn]'),
    ('missing PANEL_POWER_RESET', 'PanelPowerOn | PanelPowerReset;', 'PanelPowerOn;'),
    ('panel off keeps VDD', 'PanelPowerOn | PanelPowerReset | EdpForceVdd | EdpBlcEnable', 'PanelPowerOn | PanelPowerReset | EdpBlcEnable'),
    ('no T11+T12 wait', 'waitRemaining(state_.panelOffKnown, state_.panelOffUs, state_.timing.waits.powerCycle)', 'PpsStatus::Ok'),
    ('no T8 wait', 'waitRemaining(state_.powerOnKnown, state_.powerOnUs, state_.timing.waits.backlightOn)', 'PpsStatus::Ok'),
    ('IDLE_ON mask ignores state', 'pollStatus(IdleOnMask, IdleOnValue', 'pollStatus(PpOn, PpOn'),
    ('no poll read bound', 'reads < StatusPollMaxReads', 'true'),
    ('cancel releases AUX after write', 'state_.obligations |= ObligationIndeterminate;', 'state_.obligations = 0;'),
    ('AUX released before IDLE_OFF', '// The AUX reference stays held until IDLE_OFF is observed.', 'dropAux();'),
    ('VBT controller ignored', 'controller = facts.backlightController;', 'controller = 0;'),
    ('unknown VBT accepted', 'facts.state != FactState::Observed', 'false'),
    ('no controller conflict check', 'otherLit && !selectedLit', 'false'),
    ('no readback verification', '!sameDelays(hw, state_.timing.registers)', 'false'),
    ('delay range unchecked', 't.registers.powerUp > PanelDelayFieldMax', 'false'),
]

def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def run(cmd, log):
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=RUNTIME_ENV, timeout=10)
    except subprocess.TimeoutExpired:
        p = subprocess.CompletedProcess(cmd, 124, '', 'TEST_TIMEOUT: bounded harness terminated process')
    log.write_text('$ ' + ' '.join(map(str, cmd)) + '\n' + p.stdout + p.stderr + f'\nexit {p.returncode}\n')
    return p

def hosted(cxx, out, name, sources, extra=()):
    exe = out / name
    b = run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', *extra, *sources, '-o', exe], out / f'{name}.build.log')
    if b.returncode:
        return {'built': False, 'passed': False, 'log': str(out / f'{name}.build.log')}
    r = run([exe], out / f'{name}.run.log')
    return {'built': True, 'passed': r.returncode == 0, 'output': (r.stdout + r.stderr).strip()[-400:]}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--cxx', default=shutil.which('clang++'))
    ap.add_argument('--skip-mutations', action='store_true')
    ap.add_argument('--phase', choices=['red', 'green'], default='green')
    a = ap.parse_args()
    if os.environ.get("CCC_OVERRIDE_OPTIONS"):
        raise SystemExit("CCC_OVERRIDE_OPTIONS changes compiler arguments; refusing evidence generation")
    out = pathlib.Path(a.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    if ROOT in out.parents or out == ROOT:
        sys.exit('--out must be outside the source tree')
    report = {'scope': 'hosted injected-MMIO model and kernel compile only; no hardware access',
              'hardware_run': 'NOT_RUN', 'sanitizer_runtime': RUNTIME_SCOPE, 'sources': {str(p.relative_to(ROOT)): sha(p) for p in (HDR, TEST, SRC, pathlib.Path(__file__), ROOT / 'Tools/sanitizer_policy.py') if p.exists()}}
    if a.phase == 'red':
        report['red'] = hosted(a.cxx, out, 'red', [TEST])
        (out / 'red-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report['red'], indent=2)); return 0 if not report['red']['passed'] else 1
    report['hosted'] = hosted(a.cxx, out, 'hosted', [TEST, SRC])
    report['sanitized'] = hosted(a.cxx, out, 'sanitized', [TEST, SRC],
                                 ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])
    clang = pathlib.Path(subprocess.run(['xcrun', '--find', 'clang'], capture_output=True, text=True).stdout.strip())
    resource = subprocess.run([clang, '-print-resource-dir'], capture_output=True, text=True).stdout.strip()
    kcmd = [clang, '-x', 'c++', '-target', 'x86_64-apple-macos13', '-std=c++17', '-mkernel', '-fapple-kext',
            '-nostdinc', '-isystem', f'{resource}/include', '-I', ROOT / 'MacKernelSDK/Headers',
            '-DKERNEL', '-DKERNEL_PRIVATE', '-fno-exceptions', '-fno-rtti', '-fno-builtin', '-fno-common',
            '-fno-asynchronous-unwind-tables', '-fvisibility=hidden', '-fno-use-cxa-atexit',
            '-Wall', '-Wextra', '-Werror', '-O2', '-c', SRC, '-o', out / 'PanelPower.kernel.o']
    k = run(kcmd, out / 'kernel-compile.log')
    report['kernel_compile'] = {'compiled': k.returncode == 0, 'linked': False, 'loaded': False,
                                'command': [str(c) for c in kcmd]}
    if k.returncode == 0:
        nm = subprocess.run(['nm', '-u', out / 'PanelPower.kernel.o'], capture_output=True, text=True).stdout.split()
        report['kernel_compile']['undefined_symbols'] = nm
    nm = subprocess.run(['nm', '-u', out / 'sanitized'], capture_output=True, text=True)
    report['sanitizer_symbols_verified'] = nm.returncode == 0 and '__asan_' in nm.stdout and '__ubsan_' in nm.stdout
    results = []
    base = SRC.read_text()
    for name, old, new in ([] if a.skip_mutations else MUTANTS):
        if base.count(old) < 1:
            results.append({'mutant': name, 'status': 'NOT_APPLICABLE'}); continue
        mdir = out / 'mutants' / name.replace(' ', '_'); mdir.mkdir(parents=True, exist_ok=True)
        msrc = mdir / 'PanelPower.cpp'
        msrc.write_text(base.replace(old, new))
        b = run([a.cxx, '-std=c++17', '-w', '-O1', '-I', ROOT / 'Drivers/NativeGlk', '-include', HDR, TEST, msrc,
                 '-o', mdir / 'test'], mdir / 'build.log')
        if b.returncode:
            results.append({'mutant': name, 'status': 'BUILD_FAILED'}); continue
        r = run([mdir / 'test'], mdir / 'run.log')
        results.append({'mutant': name, 'status': ('KILLED_TIMEOUT' if r.returncode == 124 else 'KILLED') if r.returncode else 'SURVIVED'})
    report['inputs_unchanged'] = all(sha(ROOT / name) == value for name, value in report['sources'].items())
    report['mutants'] = results
    (out / 'green-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'sources'}, indent=2))
    ok = report['hosted']['passed'] and report['sanitized']['passed'] and report['kernel_compile']['compiled']
    ok = ok and report['inputs_unchanged'] and report['sanitizer_symbols_verified'] and all(m['status'] in ('KILLED', 'KILLED_TIMEOUT') for m in results)
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())

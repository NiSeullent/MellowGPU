#!/usr/bin/env python3
"""Run the NativeGlkCompiler host regression built by build-glk-shader-compiler.py.

Checks that the recorded inputs still match the build manifest, runs the
adapter test binary with Intel/NIR/Mesa debug variables scrubbed, then
re-verifies its results from the files it wrote:

  * every positive case is real upstream brw ISA: nonempty, gen_decode and
    gen_validate clean, the gen_print disassembly re-assembles byte for byte
    to the shipped program (hex column vs .isa.bin), and each SIMD variant
    ends in exactly one EOT;
  * binding metadata matches the ISA: every send's bti(N) is in the resource
    table (BTI 254 = Gen9 SLM, only with shared memory), writes carry a data
    payload and reads return data;
  * input constants reach the ISA as immediates; identical input gives
    identical ISA, different input different ISA;
  * every SPIR-V module the test wrote is replayed through the mglk-compile
    executable, which must reproduce the library result byte for byte, plus
    executable-only rejections (unaligned/empty file, wrong device, missing
    binding, bad usage);
  * diagnostics for every malformed or unsupported input.

Finally writes <work>/source-freeze.json: product source hashes, Mesa pin,
patches, exact flags/commands, artifact hashes and the explicit list of code
paths that are not implemented or not exercised.

The ISA has NOT executed anywhere: no GPU, RCS submission, kernel upload or
Metal qualification happens here.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
COMPONENT = ROOT / 'Userspace' / 'NativeGlkCompiler'
PRODUCT_TOOLS = ['Tools/build-glk-shader-compiler.py', 'Tools/run-glk-shader-compiler-tests.py']
POSITIVE = [
    'nir_ssbo_write_m3_a7_wg64', 'nir_ssbo_write_m3_a7_wg64_repeat', 'nir_ssbo_write_m5_a11_wg64',
    'nir_ssbo_write_m3_a7_wg8', 'nir_ssbo_write_m3_a7_wg64_bti5', 'nir_ssbo_copy_scale',
    'nir_shared_barrier_wg64', 'spirv_ssbo_write_m3_a7_wg64', 'spirv_ssbo_write_m5_a11_wg64',
    'spirv_ssbo_write_m3_a7_wg16', 'spirv_ubo_mul_a7_wg64',
]
NEGATIVE = {
    'device_8086_5a85': 'MGLK_E_UNSUPPORTED_DEVICE', 'device_8086_1912': 'MGLK_E_UNSUPPORTED_DEVICE',
    'device_8086_9a49': 'MGLK_E_UNSUPPORTED_DEVICE', 'device_8086_0000': 'MGLK_E_UNSUPPORTED_DEVICE',
    'nir_foreign_options': 'MGLK_E_INVALID_ARGUMENT', 'nir_vertex_stage': 'MGLK_E_UNSUPPORTED_INPUT',
    'nir_workgroup_dim_zero': 'MGLK_E_LIMIT_EXCEEDED', 'nir_workgroup_2048': 'MGLK_E_LIMIT_EXCEEDED',
    'nir_bti_250': 'MGLK_E_UNSUPPORTED_INPUT', 'nir_null': 'MGLK_E_INVALID_ARGUMENT',
    'spirv_null_words': 'MGLK_E_INVALID_ARGUMENT', 'spirv_header_only_3_words': 'MGLK_E_MALFORMED_INPUT',
    'spirv_byteswapped_magic': 'MGLK_E_MALFORMED_INPUT', 'spirv_truncated': 'MGLK_E_MALFORMED_INPUT',
    'spirv_zero_word_count': 'MGLK_E_MALFORMED_INPUT', 'spirv_missing_entry': 'MGLK_E_UNSUPPORTED_INPUT',
    'spirv_vertex_model': 'MGLK_E_UNSUPPORTED_INPUT', 'spirv_out_of_bounds_id': 'MGLK_E_FRONTEND_REJECTED',
    'spirv_unmapped_binding': 'MGLK_E_UNSUPPORTED_INPUT', 'spirv_kind_mismatch': 'MGLK_E_UNSUPPORTED_INPUT',
    'spirv_duplicate_bti': 'MGLK_E_INVALID_ARGUMENT', 'spirv_bti_240': 'MGLK_E_LIMIT_EXCEEDED',
    'spirv_word_limit': 'MGLK_E_LIMIT_EXCEEDED', 'spirv_null_bindings': 'MGLK_E_INVALID_ARGUMENT',
    'spirv_push_constant_storage': 'MGLK_E_UNSUPPORTED_INPUT',
    'spirv_uniform_constant_storage': 'MGLK_E_UNSUPPORTED_INPUT',
}
# Library-API misuse the executable cannot express (it always passes a valid map).
API_ONLY = {'spirv_null_bindings'}
SAME = [('nir_ssbo_write_m3_a7_wg64', 'nir_ssbo_write_m3_a7_wg64_repeat')]
DIFFERENT = [('nir_ssbo_write_m3_a7_wg64', 'nir_ssbo_write_m5_a11_wg64'),
             ('nir_ssbo_write_m3_a7_wg64', 'nir_ssbo_write_m3_a7_wg64_bti5'),
             ('spirv_ssbo_write_m3_a7_wg64', 'spirv_ssbo_write_m5_a11_wg64'),
             ('spirv_ssbo_write_m3_a7_wg64', 'spirv_ubo_mul_a7_wg64')]
EXPECTED_RESOURCES = {
    'nir_ssbo_write_m3_a7_wg64_bti5': [{'bti': 5, 'kind': 'ssbo', 'access': 2}],
    'nir_ssbo_copy_scale': [{'bti': 0, 'kind': 'ssbo', 'access': 2}, {'bti': 1, 'kind': 'ssbo', 'access': 1}],
    'spirv_ssbo_write_m3_a7_wg64': [{'bti': 3, 'kind': 'ssbo', 'access': 2}],
    'spirv_ubo_mul_a7_wg64': [{'bti': 3, 'kind': 'ssbo', 'access': 2}, {'bti': 4, 'kind': 'ubo', 'access': 1}],
}
SLM_BTI = 254  # Gen7+ shared local memory binding table index used by brw
ACCESS_READ, ACCESS_WRITE = 1, 2
VARIANT = re.compile(r'^# SIMD variant @0x([0-9a-f]+) \((\d+) bytes\)$')
INSTRUCTION = re.compile(r'^0x([0-9a-f]{8}): ((?:[0-9a-f]{2} )+)\s*(.*)$')
SEND = re.compile(r'\bsends?c?(?:\.\w+)?\s+\(')
OUTSTANDING = [
    dict(path='GPU execution of produced ISA', state='NOT_RUN',
         note='nothing here uploads, submits or executes a kernel; host is Darwin arm64'),
    dict(path='Kernel upload, instruction base / KSP, MEDIA_VFE_STATE, GPGPU_WALKER, RCS submission',
         state='NOT_IMPLEMENTED', note='owned by a future GLK command-submission component'),
    dict(path='Binding table and RENDER_SURFACE_STATE for the reported BTIs', state='NOT_IMPLEMENTED',
         note='metadata names BTI, kind and access only; SLM BTI 254 needs no surface state'),
    dict(path='CURBE / per-thread push upload (subgroup id at subgroup_id_push_dword)', state='NOT_IMPLEMENTED'),
    dict(path='Scratch allocation', state='NOT_EXERCISED', note='every test kernel reports total_scratch=0'),
    dict(path='Relocations', state='NOT_EXERCISED',
         note='every test kernel has zero relocs; consumers must still apply mglk_kernel_relocs'),
    dict(path='SIMD32 and multi-variant programs', state='NOT_EXERCISED',
         note='upstream selected one SIMD8 or SIMD16 variant for every test kernel'),
    dict(path='SSBO atomics', state='NOT_EXERCISED', note='scanned as ATOMIC|READ|WRITE; no positive case'),
    dict(path='SPIR-V images, samplers, push constants, physical storage buffers, other storage classes',
         state='REJECTED', note='scan_spirv refuses OpVariable outside Input/Uniform/Workgroup/Private/'
                                'Function/StorageBuffer with MGLK_E_UNSUPPORTED_INPUT'),
    dict(path='Descriptor arrays/reindex, variable workgroup size, non-GLCompute models', state='REJECTED'),
    dict(path='SPIR-V capabilities beyond Shader', state='NOT_ENABLED',
         note='spirv_to_nir runs with capabilities={Shader}; others are refused by upstream vtn'),
    dict(path='Independent SPIR-V validation', state='NOT_AVAILABLE',
         note='no spirv-val/SPIRV-Tools on this host; inputs are only accepted by upstream vtn'),
    dict(path='Runtime fused EU/subslice topology', state='UNKNOWN', note='static PCI table only'),
    dict(path='Metal/AIR front end into this compiler', state='NOT_IMPLEMENTED',
         note='no Metal qualification; qualified_profiles=[]'),
    dict(path='Concurrent use of one mglk_compiler', state='NOT_SUPPORTED', note='single-threaded per handle'),
]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_disassembly(text):
    variants = []
    for line in text.splitlines():
        match = VARIANT.match(line)
        if match:
            variants.append(dict(base=int(match.group(1), 16), size=int(match.group(2)), insts=[]))
            continue
        match = INSTRUCTION.match(line)
        if match and variants:
            variants[-1]['insts'].append(dict(offset=int(match.group(1), 16), raw=bytes.fromhex(match.group(2)),
                                              text=match.group(3)))
    return variants


def mnemonic(text):
    tokens = [token for token in text.split() if not token.startswith('(')]
    return tokens[0].split('.')[0] if tokens else ''


def sends(variants):
    found = []
    for variant in variants:
        for inst in variant['insts']:
            text = inst['text']
            if not SEND.search(text):
                continue
            bti = re.search(r'\bbti\((\d+)\)|Surface = (\d+)', text)
            payload = re.search(r'\bwr:\w+\+(\d+)', text)
            response = re.search(r'\brd:(\d+)', text)
            simd = re.search(r'\bsimd(\d+)\b', text)
            found.append(dict(text=text, bti=int(next(g for g in bti.groups() if g)) if bti else None,
                              payload=int(payload.group(1)) if payload else None,
                              response=int(response.group(1)) if response else None,
                              simd=int(simd.group(1)) if simd else None))
    return found


def has_immediate(variants, mnemonics, value):
    pattern = re.compile(rf'(?<![\w.]){value}:u?[wd]\b|\b0x{value:08x}\b')
    return any(mnemonic(inst['text']) in mnemonics and pattern.search(inst['text'])
               for variant in variants for inst in variant['insts'])


def expected_constants(name):
    match = re.search(r'_m(\d+)_a(\d+)_', name)
    if match:
        return int(match.group(1)), int(match.group(2))
    match = re.search(r'_ubo_mul_a(\d+)_', name)
    if match:
        return None, int(match.group(1))
    return (9, None) if name == 'nir_ssbo_copy_scale' else (None, None)


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True).stdout


def product_sources():
    paths = sorted([path for path in COMPONENT.rglob('*') if path.is_file()] + [ROOT / name for name in PRODUCT_TOOLS])
    records = []
    for path in paths:
        rel = str(path.relative_to(ROOT))
        head = path.read_text(errors='replace')[:2000]
        spdx = re.search(r'SPDX-License-Identifier:\s*(\S+)', head)
        records.append(dict(path=rel, sha256=sha256(path), size_bytes=path.stat().st_size,
                            git_blob=git('hash-object', '--', rel).strip(),
                            spdx=spdx.group(1) if spdx else None))
    listing = ''.join(f'{record["sha256"]}  {record["path"]}\n' for record in records)
    return records, hashlib.sha256(listing.encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, required=True, help='Work directory used by build-glk-shader-compiler.py')
    parser.add_argument('--out', type=Path, help='Report directory (default: <work>/test-run)')
    parser.add_argument('--timeout', type=int, default=900)
    args = parser.parse_args()
    work = args.work.resolve()
    out = (args.out or work / 'test-run').resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('--out must be outside the source checkout')
    manifest = json.loads((work / 'build-manifest.json').read_text())
    checks = []

    def check(name, ok, detail=''):
        checks.append(dict(check=name, ok=bool(ok), detail=detail))
        return ok

    for rel, digest in manifest['inputs'].items():
        check(f'input-unchanged:{rel}', sha256(ROOT / rel) == digest, 'rebuild if this fails')
    binary, cli = Path(manifest['adapter']['binary']), Path(manifest['adapter']['cli'])
    for label, path, digest in (('binary', binary, manifest['adapter']['binary_sha256']),
                                ('cli', cli, manifest['adapter']['cli_sha256'])):
        if not check(f'{label}-matches-manifest', path.exists() and sha256(path) == digest):
            raise SystemExit(f'{path} missing or differs from build manifest; rerun the build')

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('INTEL_', 'NIR_', 'MESA_', 'BRW_', 'GALLIUM_', 'LIBGL_'))}
    scrubbed = sorted(set(os.environ) - set(env))
    started = datetime.datetime.now(datetime.timezone.utc)
    result = subprocess.run([str(binary), str(out)], env=env, capture_output=True, text=True, timeout=args.timeout)
    (out / 'stdout.txt').write_text(result.stdout)
    (out / 'stderr.txt').write_text(result.stderr)
    check('process-exit-0', result.returncode == 0, f'exit {result.returncode}: {result.stderr[-2000:]}')

    data = json.loads((out / 'cases.json').read_text())
    cases = {case['name']: case for case in data['cases']}
    check('mesa-commit', data['mesa_commit'] == manifest['source']['commit'])
    check('abi-version-2', data['abi_version'] == 2)
    check('test-binary-expectations', data['failures'] == 0, f'{data["failures"]} failure(s)')
    for name, case in cases.items():
        check(f'status:{name}', case['status'] == case['expected'], f'{case["status"]}: {case["message"]}')
    unexpected = sorted(set(cases) - set(POSITIVE) - set(NEGATIVE) - {'device_8086_3185', 'device_8086_3184',
                                                                      'device_8086_3185_desc', 'device_8086_3184_desc'})
    check('no-unlisted-cases', not unexpected, ', '.join(unexpected))

    isa = {}
    for name in POSITIVE:
        case = cases.get(name)
        if not check(f'present:{name}', case is not None and case['status'] == 'MGLK_OK'):
            continue
        info, resources = case['info'], case['resources']
        program = (out / 'cases' / f'{name}.isa.bin').read_bytes()
        variants = parse_disassembly((out / 'cases' / f'{name}.disasm.txt').read_text())
        isa[name] = program
        case['isa_sha256'] = hashlib.sha256(program).hexdigest()
        code_size = info['const_data_offset'] if info['const_data_size'] else info['program_size']
        check(f'isa-nonempty:{name}', len(program) > 0 and len(program) == info['program_size'],
              f'{len(program)} bytes')
        check(f'gen-decode:{name}', info['decoded_instructions'] > 0, f'{info["decoded_instructions"]} instructions')
        check(f'gen-validate:{name}', info['validation_errors'] == 0)
        check(f'simd-single-variant:{name}', info['simd_mask'] in (1, 2, 4), f'mask {info["simd_mask"]}')
        check(f'variants-match-mask:{name}', len(variants) == bin(info['simd_mask']).count('1'))
        reassembled = bytearray()
        contiguous = True
        for variant in sorted(variants, key=lambda item: item['base']):
            position = variant['base']
            for inst in variant['insts']:
                contiguous &= variant['base'] + inst['offset'] == position
                contiguous &= program[position:position + len(inst['raw'])] == inst['raw']
                position += len(inst['raw'])
                reassembled += inst['raw']
            contiguous &= position == variant['base'] + variant['size']
        check(f'disasm-reassembles-to-isa:{name}', contiguous and len(reassembled) == code_size,
              f'{len(reassembled)} of {code_size} code bytes')
        check(f'disasm-instruction-count:{name}',
              sum(len(variant['insts']) for variant in variants) == info['decoded_instructions'])
        # brw may pad after the EOT send with a compacted nop; nothing else may follow it.
        eot_ok = bool(variants)
        for variant in variants:
            eots = [index for index, inst in enumerate(variant['insts']) if 'EOT' in inst['text']]
            eot_ok &= len(eots) == 1 and all(mnemonic(inst['text']) == 'nop'
                                             for inst in variant['insts'][eots[0] + 1:])
        check(f'single-eot-then-padding:{name}', eot_ok)
        found = sends(variants)
        btis = {send['bti'] for send in found if send['bti'] is not None}
        table = {resource['bti']: resource for resource in resources}
        check(f'isa-btis-equal-resource-table:{name}', btis - {SLM_BTI} == set(table),
              f'isa {sorted(btis)} table {sorted(table)}')
        check(f'slm-bti-iff-shared:{name}', (SLM_BTI in btis) == (info['total_shared'] > 0))
        for bti, resource in table.items():
            at = [send for send in found if send['bti'] == bti]
            if resource['access'] & ACCESS_WRITE:
                check(f'isa-write-bti{bti}:{name}', any(send['payload'] and send['response'] == 0 for send in at))
            if resource['access'] & ACCESS_READ:
                check(f'isa-read-bti{bti}:{name}', any(send['response'] for send in at))
        simd = {send['simd'] for send in found if send['simd']}
        check(f'send-simd-matches-dispatch:{name}', simd <= {info['dispatch_simd_size']},
              f'{sorted(simd)} vs {info["dispatch_simd_size"]}')
        multiplier, addend = expected_constants(name)
        if multiplier is not None:
            check(f'isa-immediate-mul-{multiplier}:{name}', has_immediate(variants, {'mul', 'mad'}, multiplier))
        if addend is not None:
            check(f'isa-immediate-add-{addend}:{name}', has_immediate(variants, {'add', 'mad'}, addend))
        check(f'subgroup-id-push:{name}', (info['push_per_thread_dwords'] > 0) ==
              (info['subgroup_id_push_dword'] is not None))
        if name in EXPECTED_RESOURCES:
            check(f'resource-table:{name}', resources == EXPECTED_RESOURCES[name], json.dumps(resources))
    for a, b in SAME:
        check(f'deterministic:{a}=={b}', a in isa and isa.get(a) == isa.get(b))
    for a, b in DIFFERENT:
        check(f'input-sensitive:{a}!={b}', a in isa and b in isa and isa[a] != isa[b])
    if 'nir_ssbo_write_m3_a7_wg8' in cases and 'nir_ssbo_write_m3_a7_wg64' in cases:
        check('local-size-metadata', cases['nir_ssbo_write_m3_a7_wg8']['info']['local_size'] == [8, 1, 1] and
              cases['nir_ssbo_write_m3_a7_wg64']['info']['local_size'] == [64, 1, 1])
    ubo = cases.get('spirv_ubo_mul_a7_wg64', {}).get('info', {})
    check('ubo-pull-metadata', ubo.get('uses_ubo_pull') == 1, json.dumps(ubo.get('uses_ubo_pull')))
    shared = cases.get('nir_shared_barrier_wg64', {})
    shared_text = (out / 'cases' / 'nir_shared_barrier_wg64.disasm.txt').read_text() if shared.get('info') else ''
    check('shared-barrier-metadata', shared.get('info', {}).get('uses_barrier') == 1 and
          shared.get('info', {}).get('total_shared', 0) >= 256)
    check('shared-barrier-isa', all(token in shared_text for token in ('barrier_msg', 'memory_fence', 'wait')))
    desc = cases.get('device_8086_3185_desc', {}).get('device', {})
    check('glk-device-desc', desc.get('verx10') == 90 and desc.get('platform') == 'GLK' and
          desc.get('runtime_fused_topology_known') == 0, json.dumps(desc))
    for name, status in NEGATIVE.items():
        case = cases.get(name)
        check(f'negative:{name}', case is not None and case['status'] == status and case['message'] != '')
    vtn = cases.get('spirv_out_of_bounds_id', {}).get('message', '')
    check('upstream-vtn-diagnostic', 'SPIR-V id 99 is out-of-bounds' in vtn, vtn[:300])

    # Replay every recorded SPIR-V module through the offline compiler executable.
    cli_dir = out / 'cli'
    cli_dir.mkdir()
    cli_runs = []

    def run_cli(label, arguments):
        result = subprocess.run([str(cli), *arguments], env=env, capture_output=True, text=True,
                                timeout=args.timeout)
        report = cli_dir / f'{label}.json'
        parsed = json.loads(report.read_text()) if report.exists() else None
        cli_runs.append(dict(label=label, argv=['mglk-compile', *arguments], exit=result.returncode,
                             stderr=result.stderr[-1000:], status=parsed and parsed['status']))
        return result.returncode, parsed

    def binding_args(bindings):
        return [item for b in bindings for item in ('--binding', f'{b["set"]}:{b["binding"]}:{b["bti"]}:{b["kind"]}')]

    replayed = 0
    for name, case in sorted(cases.items()):
        if case['input'] != 'spirv' or not case.get('spv') or name in API_ONLY:
            continue
        replayed += 1
        code, parsed = run_cli(name, ['--entry', case['entry'], *binding_args(case['bindings']),
                                      '--out', str(cli_dir / name), str(out / 'cases' / f'{name}.spv')])
        if case['status'] == 'MGLK_OK':
            same = (code == 0 and parsed and parsed['status'] == 'MGLK_OK' and
                    (cli_dir / f'{name}.isa.bin').read_bytes() == isa.get(name) and
                    (cli_dir / f'{name}.disasm.txt').read_text() ==
                    (out / 'cases' / f'{name}.disasm.txt').read_text() and
                    parsed['info'] == case['info'] and parsed['resources'] == case['resources'] and
                    parsed['relocs'] == case['relocs'])
            check(f'cli-reproduces-library:{name}', same, f'exit {code}')
        else:
            check(f'cli-rejects:{name}', code == 1 and parsed and parsed['status'] == case['status'] and
                  parsed['message'] == case['message'], f'exit {code} {parsed and parsed["status"]}')
    check('cli-replayed-all-recorded-spirv', replayed >= 17, f'{replayed} module(s)')

    good = out / 'cases' / 'spirv_ssbo_write_m3_a7_wg64.spv'
    good_map = ['--binding', '0:0:3:ssbo']
    inputs = out / 'cli-inputs'
    inputs.mkdir()
    (inputs / 'unaligned.spv').write_bytes(good.read_bytes()[:-2])
    (inputs / 'empty.spv').write_bytes(b'')
    code, parsed = run_cli('repeat', [*good_map, '--out', str(cli_dir / 'repeat'), str(good)])
    check('cli-deterministic-repeat', code == 0 and (cli_dir / 'repeat.isa.bin').read_bytes() ==
          isa.get('spirv_ssbo_write_m3_a7_wg64'))
    code, parsed = run_cli('device_3184', ['--device', '0x3184', *good_map, '--out', str(cli_dir / 'device_3184'),
                                           str(good)])
    check('cli-glk-3184-compiles', code == 0 and parsed['device']['pci_device_id'] == 0x3184 and
          parsed['device']['platform'] == 'GLK' and parsed['info']['validation_errors'] == 0)
    for label, arguments, status in (
            ('unaligned', [*good_map, str(inputs / 'unaligned.spv')], 'MGLK_E_MALFORMED_INPUT'),
            ('empty', [*good_map, str(inputs / 'empty.spv')], 'MGLK_E_MALFORMED_INPUT'),
            ('missing_file', [*good_map, str(inputs / 'absent.spv')], 'MGLK_E_INVALID_ARGUMENT'),
            ('device_apl_5a85', ['--device', '0x5a85', *good_map, str(good)], 'MGLK_E_UNSUPPORTED_DEVICE'),
            ('device_tgl_9a49', ['--device', '0x9a49', *good_map, str(good)], 'MGLK_E_UNSUPPORTED_DEVICE'),
            ('no_binding_map', [str(good)], 'MGLK_E_UNSUPPORTED_INPUT')):
        code, parsed = run_cli(label, ['--out', str(cli_dir / label), *arguments])
        check(f'cli-rejects:{label}', code == 1 and parsed and parsed['status'] == status and
              not (cli_dir / f'{label}.isa.bin').exists(), f'exit {code} {parsed and parsed["status"]}')
    for label, arguments in (('usage_no_input', []), ('usage_bad_kind', ['--binding', '0:0:3:image', str(good)]),
                             ('usage_bad_device', ['--device', '0x13185', str(good)]),
                             ('usage_no_out', [*good_map, str(good)])):
        code, _ = run_cli(label, [*arguments, *(['--out', str(cli_dir / label)] if label != 'usage_no_out' else [])])
        check(f'cli-usage-error:{label}', code == 2 and not (cli_dir / f'{label}.json').exists(), f'exit {code}')

    failed = [item for item in checks if not item['ok']]
    report = dict(
        scope='Host-only compile of NIR/SPIR-V compute kernels to GLK Gen9 EU ISA through pinned upstream Mesa '
              'brw, via the adapter library and the mglk-compile executable; the ISA has not executed. Not GPU, '
              'RCS, upload or Metal qualification.',
        verdict='PASS' if not failed else 'FAIL',
        gpu_execution='NOT_RUN', rcs_submission='NOT_IMPLEMENTED', kernel_upload='NOT_IMPLEMENTED',
        hardware_admission=False, metal_qualification=False, qualified_profiles=[],
        generated=started.isoformat(), host=platform.platform(), scrubbed_environment=scrubbed,
        mesa=manifest['source'], patches=manifest['patches'], build_manifest_sha256=sha256(work / 'build-manifest.json'),
        runner_sha256=sha256(Path(__file__).resolve()),
        binary_sha256=manifest['adapter']['binary_sha256'], cli_sha256=manifest['adapter']['cli_sha256'],
        adapter_library_sha256=manifest['adapter']['library_sha256'],
        cases=data['cases'], cli_runs=cli_runs, checks=checks, failed_checks=failed)
    report_path = out / 'report.json'
    report_path.write_text(json.dumps(report, indent=1) + '\n')

    sources, source_set = product_sources()
    status = [line for line in git('status', '--porcelain=v1', '--untracked-files=all').splitlines() if line]
    owned = ('Userspace/NativeGlkCompiler/', *PRODUCT_TOOLS)
    adapter = manifest['adapter']
    stable = dict(
        source_set_sha256=source_set, lock_sha256=manifest['lock']['sha256'], mesa=manifest['source'],
        archive_sha256=manifest['archive'].get('sha256'), patches=manifest['patches'],
        meson_flags=manifest['meson']['flags'], adapter_flags=adapter['flags'], adapter_warnings=adapter['warnings'],
        static_libraries=manifest['static_libraries'], objects=adapter['objects'],
        adapter_library_sha256=adapter['library_sha256'], test_binary_sha256=adapter['binary_sha256'],
        cli_sha256=adapter['cli_sha256'], compiled_sources_sha256=manifest['inventory']['sha256'],
        positive_isa_sha256={name: cases[name].get('isa_sha256') for name in POSITIVE if name in cases})
    freeze = dict(
        schema='mellow-native-glk-compiler-source-freeze/1',
        scope=report['scope'], verdict=report['verdict'], gpu_execution='NOT_RUN', hardware_admission=False,
        metal_qualification=False, qualified_profiles=[], generated=started.isoformat(),
        freeze_digest=hashlib.sha256(json.dumps(stable, sort_keys=True).encode()).hexdigest(),
        freeze_digest_covers='sha256 of canonical JSON of "stable" (no timestamps)', stable=stable,
        product_sources=sources,
        worktree=dict(head=git('rev-parse', 'HEAD').strip(), status=status,
                      tracked_modifications=[line for line in status if not line.startswith('??')],
                      untracked_outside_product=[line for line in status if line.startswith('??') and
                                                 not line[3:].startswith(owned)],
                      committed=False),
        upstream=dict(mesa=manifest['source'], archive=manifest['archive'], lock=manifest['lock'],
                      license_files=manifest['inventory']['license_files'],
                      compiled_files=manifest['inventory']['compiled_files'],
                      compiled_licenses=manifest['inventory']['licenses'],
                      compiled_sources_inventory=manifest['inventory']['path'],
                      boundary='Mesa source, venv, wheels, build tree and binaries live only under the work '
                               'directory; the product tree holds adapter sources, patches and the lock'),
        build=dict(commands=adapter['commands'], environment=adapter['environment'], tools=manifest['tools'],
                   python=manifest['python'], rebuild_comparison=manifest.get('rebuild_comparison'),
                   kernel_interface_imports=adapter['kernel_interface_imports'],
                   cli_undefined_symbols=adapter['cli_undefined_symbols']),
        tests=dict(report=str(report_path), report_sha256=sha256(report_path), checks=len(checks),
                   failed=len(failed), cases=len(data['cases']), cli_runs=len(cli_runs)),
        outstanding=OUTSTANDING,
        parent_reported_target_observation=dict(
            verified_by_this_run=False,
            report='parent loaded a passive probe on the 15IGL7 physical GPU and observed NoMapper, D0, PCI '
                   'command 0x7 and firmware framebuffer 1, with no Metal device',
            relevance='context only; no output of this host-only compiler has been run on that machine'))
    (work / 'source-freeze.json').write_text(json.dumps(freeze, indent=1) + '\n')

    print(f'{report["verdict"]}: {len(checks) - len(failed)}/{len(checks)} checks; report {report_path}\n'
          f'source freeze {work / "source-freeze.json"} digest {freeze["freeze_digest"]}')
    for item in failed:
        print(f'  FAILED {item["check"]}: {item["detail"][:300]}')
    raise SystemExit(0 if not failed else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mellow contributors.
"""Test the GLK RCS0 render-workaround producers against an oracle generated
from pinned Linux source text. Hosted codec tests and a freestanding x86_64
kernel-style compile only: no device, register, DMA or GPU access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess

COMMIT = '0d9ff90a5422cc7509258aaaba1e7481df4d332a'
PINNED = {
 'drivers/gpu/drm/i915/gt/intel_lrc.c': 'b10f6affc652967c619a846982bb9731f24ec8ef3b8d46daf428adb76f5b3843',
 'drivers/gpu/drm/i915/gt/intel_lrc.h': 'e71534746b6f6720c978c0096316c51873f5afc2974d94cd4979303384a6f5b3',
 'drivers/gpu/drm/i915/gt/intel_lrc_reg.h': '671b2adfb2825597425eea515b892cba1365f658d552f9a90439182f0f7f21c0',
 'drivers/gpu/drm/i915/gt/intel_gpu_commands.h': 'd8d14b3edefb858418eb52506f5f7598ee2cd6bd8affa659e43faaf11c85e77c',
 'drivers/gpu/drm/i915/gt/intel_gt_regs.h': 'ccc7fb4c51e94ce99dc2695f9351284e00dddef7fded90bf0be169c6539d77ac',
 'drivers/gpu/drm/i915/gt/intel_engine_regs.h': 'e6ae20bc2946a252a948628df6a84522c19dd45d537f5efa0e5c55d08e82961a',
 'drivers/gpu/drm/i915/gt/intel_workarounds.c': '1c03b70783a52440cb5852da81da80976d2ad085e7fc95a743aee77bc0309b66',
 'drivers/gpu/drm/i915/gt/intel_engine_types.h': '47b77f01148818361cf0f0c02bb40fd6420b06941d6e06516e9447f3146b6a62',
 'drivers/gpu/drm/i915/gt/intel_engine_cs.c': '3b832bd7e1d5225cb336749f5ccd17a08fa6ddbe1d62387972f1606b144375e4',
 'drivers/gpu/drm/i915/gt/intel_execlists_submission.c': '44c53b014628fdd409d089d15209020a79a0098bf2b6f4c477d89df73ddf5cf2',
 'drivers/gpu/drm/i915/gt/intel_gt.c': '4d1441c4a15bb07200e8f89d545395a2e6b02955ff982af4c186fd5e206ffead',
 'drivers/gpu/drm/i915/intel_uncore.c': '357d32c4980bd619d73aaea4f35136bed08762a1c2f718fedcf1c40a500ea47a',
 'drivers/gpu/drm/i915/i915_pci.c': 'faf891f17e233c24c183338e798c8867a4e0f7afced17c7f342e988fbc38dbf1',
 'include/drm/intel/pciids.h': '102d8f821c9464ad17c0160535a4e3811f49609f199f1b6d7268050d71d8e8f0',
 'drivers/gpu/drm/i915/gt/intel_sseu.c': '809036e4325fda1c112d3f784853c23e319f00f822e166c7eb7a5a5e27d47132',
 'drivers/gpu/drm/i915/gt/intel_sseu.h': '0986f94c4b3cce30e07887aa8f274d224487cfd30102faf811d0e15a76d43246',
 'drivers/gpu/drm/i915/gt/intel_gt_types.h': 'd5057f6a2819959c15b8f36e4e94c98d0bf4311c75502d168873748c8a9b6349',
 'drivers/gpu/drm/i915/gt/intel_gt.h': '74be24390462209952217f05d61be0793a2b2d6aad1d4867a84ee40711c3c855',
 'drivers/gpu/drm/i915/gt/gen8_engine_cs.c': 'd605b5e4b74d76e29be1db9fb22b2f16d52f8ed96ad2c5746f850665f10f2c72',
 'drivers/gpu/drm/i915/gt/gen8_engine_cs.h': '5d3def4da1e5469c10f4be1471574e01510d3353de3aae891fdfb4d2343c8d26',
 'drivers/gpu/drm/i915/gt/intel_engine.h': 'd07100bbdd00a2874b19851134c56b56c448df0219be67acb01e89ab3ed996cc',
 'drivers/gpu/drm/i915/gt/intel_workarounds_types.h': 'da7c9dfd56f5266a1bea13498b06fc43237240a6da4ca9c4e108f625a5430d72',
 'drivers/gpu/drm/i915/intel_step.c': '67f07f0bab8760e89d0b953bb26a4e8a9113e07ebc5660553829419a0322670d',
 'drivers/gpu/drm/i915/i915_drv.h': '9474f2666449dc78bb97c4b22a7e14279c3182b49c90d4a73df5a9b4c6b71620',
 'include/drm/intel/reg_bits.h': '0b6499233882efd4720dbcba1e887811a7e9dc2b9ab233c1bf625c3bdde4cec5',
}
SOURCES = ['Drivers/NativeGlk/RenderWorkarounds.hpp', 'Drivers/NativeGlk/RenderWorkarounds.cpp',
           'tests/native_glk_render_wa_tests.cpp', 'Tools/run-native-glk-render-wa-tests.py',
           'Mellow/GlkTopologyIOKit.hpp', 'Mellow/GlkTopologyIOKit.cpp', 'tests/native_glk_topology_iokit_tests.cpp',
           'tests/native_glk_render_wa_shim/IOKit/pci/IOPCIDevice.h']
LRC = ['Drivers/NativeGlk/LrcImage.hpp', 'Drivers/NativeGlk/LrcImage.cpp']
U32 = 0xffffffff


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def blob(path):
    data = Path(path).read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
def bounded():
    resource.setrlimit(resource.RLIMIT_CPU, (60, 60))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
def run(command, timeout=120, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout,
                       env=env, preexec_fn=bounded)
    return {'command': list(map(str, command)), 'returncode': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr}
def uncomment(text): return re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)
def norm(text): return ' '.join(text.split())


class Source:
    """Pinned text plus #define and enum values, evaluated on demand."""
    def __init__(self, texts):
        self.texts = texts
        self.defs, self.enums = {}, {}
        for text in texts.values():
            flat = uncomment(text).replace('\\\n', ' ')
            for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\(([^)]*)\))?(.*)$', flat, re.M):
                params = [p.strip() for p in m.group(3).split(',')] if m.group(2) else None
                self.defs.setdefault(m.group(1), (params, m.group(4).strip()))
            for m in re.finditer(r'enum\s+\w+\s*\{(.*?)\}', flat, re.S):
                for e in re.finditer(r'([A-Z_][A-Z0-9_]*)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', m.group(1)):
                    self.enums[e.group(1)] = int(e.group(2), 0)
        self.builtins = {'_MMIO': lambda r: r, 'MCR_REG': lambda r: r, 'BIT': lambda n: 1 << n,
                         'REG_BIT': lambda n: 1 << n, 'i915_mmio_reg_offset': lambda r: r,
                         'REG_GENMASK': lambda h, l: ((1 << (h - l + 1)) - 1) << l,
                         'REG_MASKED_FIELD': lambda m, v: (m << 16 | v) & U32,
                         'REG_MASKED_FIELD_ENABLE': lambda a: (a << 16 | a) & U32,
                         'REG_MASKED_FIELD_DISABLE': lambda a: (a << 16) & U32}
        masked = norm(self.defs['REG_MASKED_FIELD'][1])
        if not masked.endswith('((mask) << 16 | (value)))') or \
           'REG_MASKED_FIELD(_a, _a)' not in self.defs['REG_MASKED_FIELD_ENABLE'][1] or \
           norm(self.defs['REG_MASKED_FIELD_DISABLE'][1]) != '(REG_MASKED_FIELD((a), 0))':
            raise ValueError('reg_bits.h masked-field macros differ from the oracle model')

    def text(self, name): return self.texts[name]

    def expand(self, expr, depth=0):
        if depth > 32: raise ValueError('macro recursion in ' + expr)
        out, i = [], 0
        while i < len(expr):
            n = re.compile(r'(0x[0-9a-fA-F]+|[0-9]+)[uUlL]*').match(expr, i)
            if n:
                out.append(n.group(1)); i = n.end(); continue
            m = re.compile(r'[A-Za-z_]\w*').match(expr, i)
            if not m:
                out.append(expr[i]); i += 1; continue
            name, i = m.group(0), m.end()
            if name in self.builtins or (name in self.defs and self.defs[name][0] is not None):
                j = i
                while j < len(expr) and expr[j].isspace(): j += 1
                if j >= len(expr) or expr[j] != '(': raise ValueError('macro without arguments: ' + name)
                args, i = split_args(expr, j)
                args = [self.expand(a, depth + 1) for a in args]
                if name in self.builtins:
                    out.append(name + '(' + ','.join(args) + ')')
                else:
                    params, body = self.defs[name]
                    for p, a in zip(params, args): body = re.sub(r'\b' + re.escape(p) + r'\b', '(' + a + ')', body)
                    out.append('(' + self.expand(body, depth + 1) + ')')
            elif name in self.defs:
                out.append('(' + self.expand(self.defs[name][1], depth + 1) + ')')
            elif name in self.enums:
                out.append(str(self.enums[name]))
            else:
                out.append(name)
        return ''.join(out)

    def value(self, expr, env=None):
        py = re.sub(r'\b(0x[0-9a-fA-F]+|[0-9]+)[uUlL]+\b', r'\1', self.expand(norm(expr)))
        py = re.sub(r'\bsizeof\s*\(\s*u32\s*\)', '4', py)  # u32 is four bytes
        names = dict(self.builtins, **(env or {}))
        for name in re.findall(r'\b[A-Za-z_]\w*', py):
            if name not in names: raise ValueError('unresolved identifier %s in %s' % (name, expr))
        return eval(py, {'__builtins__': {}}, names) & U32  # noqa: S307 - names are checked above


def split_args(expr, open_index):
    depth, start, args, i = 0, open_index + 1, [], open_index
    while True:
        c = expr[i]
        if c in '([{': depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(expr[start:i].strip()); return [a for a in args if a], i + 1
        elif c == ',' and depth == 1:
            args.append(expr[start:i].strip()); start = i + 1
        i += 1


def function_body(text, name):
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^;{)]*\)\s*\{', text)
    if not m: raise ValueError('missing function ' + name)
    depth, j = 1, m.end()
    while depth:
        depth += {'{': 1, '}': -1}.get(text[j], 0); j += 1
    return uncomment(text[m.end():j - 1])


def parse(s, i=0, single=False):
    nodes = []
    while True:
        while i < len(s) and s[i].isspace(): i += 1
        if i >= len(s) or s[i] == '}':
            return nodes, i
        if s[i] == '{':
            sub, i = parse(s, i + 1); nodes.append(('block', sub)); i += 1
        elif re.match(r'(if|while)\s*\(', s[i:]):
            kw = s[i:i + 5].split('(')[0].strip()
            args, i = split_args(s, s.index('(', i))
            body, i = parse(s, i, True)
            rest = s[i:].lstrip()
            if rest.startswith('else') and not re.match(r'else\w', rest): raise ValueError('else is not modelled')
            nodes.append((kw, norm(','.join(args)), body))
        else:
            depth, j = 0, i
            while s[j] != ';' or depth:
                depth += {'(': 1, '{': 1, '[': 1, ')': -1, '}': -1, ']': -1}.get(s[j], 0); j += 1
            nodes.append(('stmt', norm(s[i:j]))); i = j + 1
        if single: return nodes, i


def interpret(nodes, conditions, statement, env):
    for node in nodes:
        if node[0] == 'block':
            interpret(node[1], conditions, statement, env)
        elif node[0] in ('if', 'while'):
            if node[1] not in conditions: raise ValueError('unmodelled condition: ' + node[1])
            guard = 0
            while conditions[node[1]](env):
                interpret(node[2], conditions, statement, env)
                guard += 1
                if node[0] == 'if': break
                if guard > 64: raise ValueError('unbounded loop')
        elif not statement(node[1], env):
            raise ValueError('unmodelled statement: ' + node[1])


def pipe_control(src, flags, offset):
    # __gen8_emit_pipe_control(): six zeroed dwords, then header|group0, group1, offset.
    return [src.value('GFX_OP_PIPE_CONTROL(6)'), flags, offset, 0, 0, 0]


def check_helpers(src):
    body = norm(function_body(src.text('drivers/gpu/drm/i915/gt/gen8_engine_cs.h'), '__gen8_emit_pipe_control'))
    want = ['memset(batch, 0, 6 * sizeof(u32));', 'batch[0] = GFX_OP_PIPE_CONTROL(6) | bit_group_0;',
            'batch[1] = bit_group_1;', 'batch[2] = offset;', 'return batch + 6;']
    if [w for w in want if w not in body]: raise ValueError('__gen8_emit_pipe_control differs')
    body = norm(function_body(src.text('drivers/gpu/drm/i915/gt/gen8_engine_cs.h'), 'gen8_emit_pipe_control'))
    if 'return __gen8_emit_pipe_control(batch, 0, bit_group_1, offset);' not in body:
        raise ValueError('gen8_emit_pipe_control differs')
    body = norm(function_body(src.text('drivers/gpu/drm/i915/gt/intel_lrc.c'), 'emit_lri'))
    want = ['*batch++ = MI_LOAD_REGISTER_IMM(count);', '*batch++ = i915_mmio_reg_offset(lri->reg);',
            '*batch++ = lri->value;', '} while (lri++, --count);', '*batch++ = MI_NOOP;']
    if [w for w in want if w not in body]: raise ValueError('emit_lri differs')


def indirect_ctx(src, pooled, scratch):
    lrc = src.text('drivers/gpu/drm/i915/gt/intel_lrc.c')
    out, lri = [], []
    env = {'pooled': pooled}
    def value(expr):
        expr = re.sub(r'intel_gt_scratch_offset\(\s*engine->gt\s*,', '_SCRATCH(', expr)
        return src.value(expr, {'_SCRATCH': lambda f: scratch + f})
    def statement(stmt, _):
        m = re.fullmatch(r'static const struct lri lri\[\] = \{(.*)\}', stmt)
        if m:
            for entry in re.findall(r'\{([^{}]*)\}', m.group(1)):
                reg, val = [p.strip() for p in entry.split(',') if p.strip()][:2]
                lri.append((src.value(reg), src.value(val)))
            return True
        m = re.fullmatch(r'\*batch\+\+ = (.+)', stmt)
        if m: out.append(value(m.group(1))); return True
        if stmt == 'batch = gen8_emit_flush_coherentl3_wa(engine, batch)':
            interpret(parse(function_body(lrc, 'gen8_emit_flush_coherentl3_wa'))[0], conditions, statement, env)
            return True
        m = re.fullmatch(r'batch = gen8_emit_pipe_control\((.*)\)', stmt)
        if m:
            args, _ = split_args('(' + m.group(1) + ')', 0)
            if args[0] != 'batch': return False
            out.extend(pipe_control(src, value(args[1]), value(args[2]))); return True
        if stmt == 'batch = emit_lri(batch, lri, ARRAY_SIZE(lri))':
            if not 0 < len(lri) <= 63: raise ValueError('emit_lri count')
            out.append(src.value('MI_LOAD_REGISTER_IMM(%d)' % len(lri)))
            for reg, val in lri: out.extend([reg, val])
            out.append(src.value('MI_NOOP')); return True
        return stmt == 'return batch'
    conditions = {'HAS_POOLED_EU(engine->i915)': lambda e: e['pooled'],
                  '(unsigned long)batch % CACHELINE_BYTES': lambda e: (4 * len(out)) % src.value('CACHELINE_BYTES') != 0}
    interpret(parse(function_body(lrc, 'gen9_init_indirectctx_bb'))[0], conditions, statement, env)
    return out


def flush_rcs(src):
    """gen8_emit_flush_rcs(rq, EMIT_BARRIER) for graphics version 9, not Kabylake."""
    if norm(src.defs['EMIT_BARRIER'][1]) != '(EMIT_INVALIDATE | EMIT_FLUSH)': raise ValueError('EMIT_BARRIER differs')
    env = {'flags': 0, 'vf_flush_wa': False, 'dc_flush_wa': False, 'len': None, 'begin': None, 'cs': []}
    def statement(stmt, e):
        if stmt in ('bool vf_flush_wa = false, dc_flush_wa = false', 'u32 *cs, flags = 0', 'int len', 'return 0'):
            return True
        m = re.fullmatch(r'flags \|= (\w+)', stmt)
        if m: e['flags'] |= src.value(m.group(1)); return True
        m = re.fullmatch(r'(vf_flush_wa|dc_flush_wa) = true', stmt)
        if m: e[m.group(1)] = True; return True
        m = re.fullmatch(r'len (\+?=) ([0-9]+)', stmt)
        if m: e['len'] = (e['len'] if m.group(1) == '+=' else 0) + int(m.group(2)); return True
        if stmt == 'cs = intel_ring_begin(rq, len)': e['begin'] = e['len']; return True
        if stmt == 'return PTR_ERR(cs)': return True
        m = re.fullmatch(r'cs = gen8_emit_pipe_control\(cs, (.+), (.+)\)', stmt)
        if m:
            flags = e['flags'] if m.group(1) == 'flags' else src.value(m.group(1))
            e['cs'].extend(pipe_control(src, flags, src.value(m.group(2)))); return True
        if stmt == 'intel_ring_advance(rq, cs)':
            if len(e['cs']) != e['begin']: raise ValueError('flush length mismatch')
            return True
        return False
    conditions = {'mode & EMIT_FLUSH': lambda e: True, 'mode & EMIT_INVALIDATE': lambda e: True,
                  'GRAPHICS_VER(rq->i915) == 9': lambda e: True,
                  'IS_KABYLAKE(rq->i915) && IS_GRAPHICS_STEP(rq->i915, 0, STEP_C0)': lambda e: False,
                  'IS_ERR(cs)': lambda e: False, 'vf_flush_wa': lambda e: e['vf_flush_wa'],
                  'dc_flush_wa': lambda e: e['dc_flush_wa']}
    interpret(parse(function_body(src.text('drivers/gpu/drm/i915/gt/gen8_engine_cs.c'), 'gen8_emit_flush_rcs'))[0],
              conditions, statement, env)
    return env['cs']


def ctx_workarounds(src):
    wa = src.text('drivers/gpu/drm/i915/gt/intel_workarounds.c')
    helper = {'wa_masked_en': 'wa_add(wal, reg, 0, REG_MASKED_FIELD_ENABLE(val), val, true);',
              'wa_mcr_masked_en': 'wa_mcr_add(wal, reg, 0, REG_MASKED_FIELD_ENABLE(val), val, true);',
              'wa_masked_dis': 'wa_add(wal, reg, 0, REG_MASKED_FIELD_DISABLE(val), val, true);',
              'wa_mcr_masked_dis': 'wa_mcr_add(wal, reg, 0, REG_MASKED_FIELD_DISABLE(val), val, true);',
              'wa_masked_field_set': 'wa_add(wal, reg, 0, REG_MASKED_FIELD(mask, val), mask, true);'}
    for name, body in helper.items():
        if norm(function_body(wa, name)) != body: raise ValueError(name + ' differs')
    add = norm(function_body(wa, '_wa_add'))
    for anchor in ['wa_->set |= wa->set;', 'wa_->clr |= wa->clr;', 'wa_->read |= wa->read;',
                   'if (i915_mmio_reg_offset(wal->list[mid].reg) < addr) {']:
        if anchor not in add: raise ValueError('_wa_add differs')
    dispatch = norm(function_body(wa, '__intel_engine_init_ctx_wa'))
    if 'else if (IS_GEMINILAKE(i915)) glk_ctx_workarounds_init(engine, wal);' not in dispatch:
        raise ValueError('GLK ctx workaround dispatch differs')
    gen9lp = re.search(r'#define GEN9_LP_FEATURES(.*?)\n\n', src.text('drivers/gpu/drm/i915/i915_pci.c'), re.S).group(1)
    if 'has_llc' in gen9lp: raise ValueError('GEN9_LP_FEATURES sets has_llc')
    entries = {}
    def add_wa(reg, set_, read, mcr):
        e = entries.setdefault(reg, {'set': 0, 'read': 0, 'mcr': mcr, 'clr': 0})
        if e['mcr'] != mcr: raise ValueError('mixed MCR entry')
        e['set'] |= set_; e['read'] |= read
    def statement(stmt, _):
        if stmt == 'struct drm_i915_private *i915 = engine->i915': return True
        if stmt == 'gen9_ctx_workarounds_init(engine, wal)':
            interpret(parse(function_body(wa, 'gen9_ctx_workarounds_init'))[0], conditions, statement, None)
            return True
        m = re.fullmatch(r'wa_(mcr_)?masked_(en|dis)\(wal, (\w+), (.+)\)', stmt)
        if m:
            bits = src.value(m.group(4))
            set_ = src.value(('REG_MASKED_FIELD_ENABLE(%d)' if m.group(2) == 'en' else 'REG_MASKED_FIELD_DISABLE(%d)') % bits)
            add_wa(src.value(m.group(3)), set_, bits, bool(m.group(1))); return True
        m = re.fullmatch(r'wa_masked_field_set\(wal, (\w+), (\w+), (\w+)\)', stmt)
        if m:
            mask, val = src.value(m.group(2)), src.value(m.group(3))
            add_wa(src.value(m.group(1)), src.value('REG_MASKED_FIELD(%d, %d)' % (mask, val)), mask, False); return True
        return False
    conditions = {'HAS_LLC(i915)': lambda e: False, 'IS_GEN9_LP(i915)': lambda e: True,
                  'IS_SKYLAKE(i915) || IS_KABYLAKE(i915) || IS_COFFEELAKE(i915) || IS_COMETLAKE(i915)': lambda e: False}
    interpret(parse(function_body(wa, 'glk_ctx_workarounds_init'))[0], conditions, statement, None)
    emit = norm(function_body(wa, 'intel_engine_emit_ctx_wa'))
    for anchor in ['*cs++ = MI_LOAD_REGISTER_IMM(wal->count);', 'if (wa->masked_reg || (wa->clr | wa->set) == U32_MAX) { val = wa->set;',
                   '*cs++ = i915_mmio_reg_offset(wa->reg); *cs++ = val; } *cs++ = MI_NOOP;']:
        if anchor not in emit: raise ValueError('intel_engine_emit_ctx_wa differs')
    if emit.count('rq->engine->emit_flush(rq, EMIT_BARRIER)') != 2: raise ValueError('ctx wa barriers differ')
    if 'engine->emit_flush = gen8_emit_flush_rcs;' not in src.text('drivers/gpu/drm/i915/gt/intel_execlists_submission.c'):
        raise ValueError('RCS emit_flush binding differs')
    if 'err = intel_engine_emit_ctx_wa(rq);' not in src.text('drivers/gpu/drm/i915/gt/intel_gt.c'):
        raise ValueError('default-state ctx wa emission differs')
    if 'IS_ERR(cs)' not in emit: raise ValueError('emit_ctx_wa structure differs')
    listing = [dict(offset=reg, **{k: v for k, v in e.items() if k != 'clr'}) for reg, e in sorted(entries.items())]
    flush = flush_rcs(src)
    lri = [src.value('MI_LOAD_REGISTER_IMM(%d)' % len(listing))]
    for e in listing: lri.extend([e['offset'], e['set']])
    lri.append(src.value('MI_NOOP'))
    return listing, flush + lri + flush


def sseu(src, fuse2, eu_disable):
    """gen9_sseu_info_init() for IS_GEN9_LP and intel_sseu_make_rpcs() for version 9."""
    text = norm(function_body(src.text('drivers/gpu/drm/i915/gt/intel_sseu.c'), 'gen9_sseu_info_init'))
    for anchor in ['sseu->slice_mask = REG_FIELD_GET(GEN8_F2_S_ENA_MASK, fuse2);',
                   'intel_sseu_set_info(sseu, IS_GEN9_LP(i915) ? 1 : 3, IS_GEN9_LP(i915) ? 3 : 4, 8);',
                   'subslice_mask &= ~REG_FIELD_GET(GEN9_F2_SS_DIS_MASK, fuse2);',
                   'eu_disabled_mask = (eu_disable >> (ss * 8)) & eu_mask;', 'if (eu_per_ss == 7)',
                   'sseu->has_subslice_pg = IS_GEN9_LP(i915) && intel_sseu_subslice_total(sseu) > 1;',
                   'sseu->has_eu_pg = sseu->eu_per_subslice > 2;',
                   'RUNTIME_INFO(i915)->has_pooled_eu = hweight8(sseu->subslice_mask.hsw[0]) == 3;']:
        if anchor not in text: raise ValueError('gen9_sseu_info_init differs: ' + anchor)
    get = lambda mask, v: (v & mask) >> ((mask & -mask).bit_length() - 1)
    pop = lambda v: bin(v).count('1')
    slice_mask = get(src.value('GEN8_F2_S_ENA_MASK'), fuse2)
    ss_mask = ((1 << 3) - 1) & ~get(src.value('GEN9_F2_SS_DIS_MASK'), fuse2) & 0xff
    hsw0, eu, ss7 = 0, [0, 0, 0], 0
    if slice_mask & 1:
        hsw0 = ss_mask
        for ss in range(3):
            if hsw0 & (1 << ss):
                disabled = (eu_disable >> (ss * 8)) & 0xff
                eu[ss] = ~disabled & 0xff
                if 8 - pop(disabled) == 7: ss7 |= 1 << ss
    total = sum(map(pop, eu)); sst = pop(hsw0)
    per = -(-total // sst) if sst else 0
    t = dict(sliceMask=slice_mask, subsliceMask=hsw0, euMask=eu, subslice7Eu=ss7, euTotal=total, euPerSubslice=per,
             hasSlicePg=False, hasSubslicePg=sst > 1, hasEuPg=per > 2, hasPooledEu=pop(hsw0) == 3, minEuInPool=0)
    if t['hasPooledEu']:
        t['minEuInPool'] = 3 if not hsw0 & 4 or not hsw0 & 1 else 6 if not hsw0 & 2 else 9
    rpcs = 0
    if t['hasSubslicePg']:
        v = pop(hsw0) << src.value('GEN8_RPCS_SS_CNT_SHIFT')
        if v & ~src.value('GEN8_RPCS_SS_CNT_MASK'): raise ValueError('GEM_BUG_ON subslice count')
        rpcs |= src.value('GEN8_RPCS_ENABLE') | src.value('GEN8_RPCS_SS_CNT_ENABLE') | v
    if t['hasEuPg']:
        rpcs |= (8 << src.value('GEN8_RPCS_EU_MIN_SHIFT')) & src.value('GEN8_RPCS_EU_MIN_MASK')
        rpcs |= (8 << src.value('GEN8_RPCS_EU_MAX_SHIFT')) & src.value('GEN8_RPCS_EU_MAX_MASK')
        rpcs |= src.value('GEN8_RPCS_ENABLE')
    t['rpcs'] = rpcs
    return t


def steps(src):
    text = src.text('drivers/gpu/drm/i915/intel_step.c')
    table = re.search(r'glk_revids\[\] = \{(.*?)\};', text, re.S).group(1)
    known = {int(i): s for i, s in re.findall(r'\[(\d+)\] = \{ COMMON_STEP\((\w+)\) \}', table)}
    if known != {3: 'B0'}: raise ValueError('glk_revids differs')
    body = norm(function_body(text, 'intel_step_init'))
    for anchor in ['if (revid < size && revids[revid].graphics_step != STEP_NONE) {',
                   'while (revid < size && revids[revid].graphics_step == STEP_NONE) revid++;',
                   'step.graphics_step = STEP_FUTURE;']:
        if anchor not in body: raise ValueError('intel_step_init differs')
    size, cases = max(known) + 1, []
    for rev in [0, 1, 2, 3, 4, 6, 255]:
        r = rev
        if r < size and r in known: cases.append((rev, False, True)); continue
        while r < size and r not in known: r += 1
        cases.append((rev, r >= size, False))
    return cases


def oracle(src):
    check_helpers(src)
    ids = re.search(r'#define INTEL_GLK_IDS\(MACRO__, \.\.\.\)(.*?)\n\n', src.text('include/drm/intel/pciids.h'), re.S).group(1)
    glk_ids = [int(x, 16) for x in re.findall(r'MACRO__\((0x[0-9a-fA-F]+)', ids)]
    ranges = re.search(r'__gen9_fw_ranges\[\] = \{(.*?)\};', src.text('drivers/gpu/drm/i915/intel_uncore.c'), re.S).group(1)
    fw = [(int(a, 16), int(b, 16), d.strip()) for a, b, d in re.findall(r'GEN_FW_RANGE\((0x[0-9a-f]+), (0x[0-9a-f]+), ([^)]*)\)', ranges)]
    fuse_domains = {hex(r): [d for a, b, d in fw if a <= r <= b] for r in (src.value('GEN8_FUSE2'), src.value('GEN9_EU_DISABLE(0)'))}
    lrc = norm(function_body(src.text('drivers/gpu/drm/i915/gt/intel_lrc.c'), 'lrc_init_wa_ctx'))
    for anchor in ['if (GRAPHICS_VER(engine->i915) == 9) { wa_bb_fn[0] = gen9_init_indirectctx_bb; wa_bb_fn[1] = NULL; }',
                   'err = i915_ggtt_pin(wa_ctx->vma, &ww, 0, PIN_HIGH);', 'wa_bb[i]->offset = batch_ptr - batch;']:
        if anchor not in lrc: raise ValueError('lrc_init_wa_ctx differs')
    if norm(src.defs['CTX_WA_BB_SIZE'][1]) != '(PAGE_SIZE)': raise ValueError('CTX_WA_BB_SIZE differs')
    cases = [('3x6 pooled', 1 << 25, 0x00c0c0c0), ('2x6 ss2 fused', 1 << 25 | 4 << 20, 0x00c0c0c0),
             ('1x8 single subslice', 1 << 25 | 6 << 20, 0), ('ss1 fused 2x8', 1 << 25 | 2 << 20, 0),
             ('unbalanced 7/8/7', 1 << 25, 0x00800080), ('2 EU per subslice', 1 << 25, 0x00fcfcfc),
             ('ignored SS_DIS bit 3 and EU byte 3', 1 << 25 | 8 << 20, 0xff000000), ('3x3', 1 << 25, 0x00f8f8f8)]
    topo = [(n, f, e, sseu(src, f, e)) for n, f, e in cases]
    placements = [(0, 0x00ffe000, 0x00fff000), (1, 0x00ffe000, 0x00fff000), (0, 0xfffff000, 0x00001000),
                  (2, 0x00010000, 0xfffff000)]
    batches = []
    for ti, wa, scratch in placements:
        words = indirect_ctx(src, topo[ti][3]['hasPooledEu'], scratch)
        batches.append((ti, wa, scratch, words, topo[ti][3]['hasPooledEu']))
    listing, ring = ctx_workarounds(src)
    b = lambda v: 'true' if v else 'false'
    h = lambda v: '0x%08xU' % v
    lines = ['// Generated by Tools/run-native-glk-render-wa-tests.py from Linux ' + COMMIT + '.',
             '#pragma once', '#include <stdint.h>', 'namespace Oracle {',
             'struct StepCase { uint8_t revision; bool future; bool inTable; };',
             'const StepCase stepCases[] = {' + ', '.join('{%d, %s, %s}' % (r, b(f), b(k)) for r, f, k in steps(src)) + '};',
             'constexpr bool targetDeviceInGlkIds = %s;' % b(0x3185 in glk_ids),
             'constexpr uint32_t indirectCtxOffsetDefault = %s;' % h(src.value('GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT')),
             'struct TopologyCase { const char *name; uint32_t fuse2, euDisable0; uint8_t sliceMask, subsliceMask, euMask[3],'
             ' subslice7Eu, euTotal, euPerSubslice; bool hasSlicePg, hasSubslicePg, hasEuPg, hasPooledEu;'
             ' uint8_t minEuInPool; uint32_t rpcs; };', 'const TopologyCase topologyCases[] = {']
    for n, f, e, t in topo:
        lines.append('  {"%s", %s, %s, %d, %d, {%d, %d, %d}, %d, %d, %d, %s, %s, %s, %s, %d, %s},' % (
            n, h(f), h(e), t['sliceMask'], t['subsliceMask'], *t['euMask'], t['subslice7Eu'], t['euTotal'],
            t['euPerSubslice'], b(t['hasSlicePg']), b(t['hasSubslicePg']), b(t['hasEuPg']), b(t['hasPooledEu']),
            t['minEuInPool'], h(t['rpcs'])))
    lines += ['};', 'struct BatchCase { const char *name; unsigned topology; uint32_t waContextGgtt, scratchGgtt;'
              ' unsigned dwordCount; uint32_t indirectBytes; bool mediaPool; uint32_t dwords[64]; };',
              'const BatchCase batchCases[] = {']
    for ti, wa, scratch, words, pooled in batches:
        lines.append('  {"%s @%08x scratch %08x", %d, %s, %s, %d, %d, %s, {%s}},' % (
            topo[ti][0], wa, scratch, ti, h(wa), h(scratch), len(words), 4 * len(words), b(pooled), ', '.join(map(h, words))))
    lines += ['};', 'struct ContextWorkaround { uint32_t offset, set, read; bool mcr; };',
              'const ContextWorkaround contextWorkarounds[] = {' +
              ', '.join('{%s, %s, %s, %s}' % (h(e['offset']), h(e['set']), h(e['read']), b(e['mcr'])) for e in listing) + '};',
              'const uint32_t contextWorkaroundRing[] = {' + ', '.join(map(h, ring)) + '};', '} // namespace Oracle', '']
    summary = {'glk_pci_ids': [hex(x) for x in glk_ids], 'fuse_forcewake_domains': fuse_domains,
               'step_cases': steps(src), 'topology_cases': [(n, hex(f), hex(e), t) for n, f, e, t in topo],
               'indirect_ctx_dwords': {('pooled' if p else 'unpooled') + '@' + hex(s): [hex(w) for w in words]
                                       for _, _, s, words, p in batches},
               'context_workarounds': [{k: (hex(v) if isinstance(v, int) and not isinstance(v, bool) else v)
                                        for k, v in e.items()} for e in listing],
               'context_workaround_ring': [hex(w) for w in ring]}
    return '\n'.join(lines), summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=shutil.which('clang++') or '')
    p.add_argument('--linux-root', required=True, type=Path, action='append',
                   help='Pinned Linux tree(s); every pinned file must exist in one of them')
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--kernel-sdk', type=Path, help='Genuine macOS SDK for the x86_64 kernel-style compile')
    p.add_argument('--implementation', type=Path, help='Alternative RenderWorkarounds.cpp (RED runs only)')
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]; out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    impl = (args.implementation or root / SOURCES[1]).resolve()
    before = {n: (sha(root / n) if (root / n).is_file() else None) for n in SOURCES + LRC}
    report = {'schema': 'mellowgpu.glk-render-wa-tests/1', 'linux_commit': COMMIT, 'status': 'FAIL',
              'source_sha256': before, 'implementation': str(impl), 'implementation_sha256': sha(impl),
              'physical_gpu_executed': False, 'native_driver_executed': False, 'mmio_or_dma_executed': False,
              'hardware_support_qualified': False, 'metal_conformance_tested': False, 'windowserver_tested': False,
              'fuse_values': 'synthetic test inputs; no reading of any physical part',
              'sanitizers': ['address', 'undefined'], 'fatal_sanitizers': True}
    result = 1
    try:
        texts, pins = {}, {}
        for name, digest in PINNED.items():
            path = next((r / name for r in args.linux_root if (r / name).is_file()), None)
            if path is None or sha(path) != digest: raise ValueError('Pinned Linux source missing or drifted: ' + name)
            texts[name] = path.read_text(); pins[name] = {'sha256': digest, 'git_blob': blob(path), 'path': str(path)}
        report['linux_sources'] = pins
        include, report['oracle'] = oracle(Source(texts))
        (out / 'native_glk_render_wa_oracle.inc').write_text(include)
        report['oracle_include_sha256'] = sha(out / 'native_glk_render_wa_oracle.inc')
        report['compiler'] = run([args.cxx, '--version'], 20)
        common = [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fno-exceptions',
                  '-fno-rtti', '-I', root]
        san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        objects, report['compile'] = [], []
        for name, src in [('RenderWorkarounds', impl), ('LrcImage', root / LRC[1]),
                          ('tests', root / 'tests/native_glk_render_wa_tests.cpp')]:
            obj = out / (name + '.san.o')
            report['compile'].append(run([*common, *san, '-I', out, '-c', src, '-o', obj]))
            if report['compile'][-1]['returncode']: raise ValueError('Sanitized compile failed: ' + name)
            objects.append(obj)
        nm = run(['nm', '-u', objects[0]], 20)['stdout']
        report['production_object_sanitizer_symbols'] = sorted({s for s in nm.split() if '__asan_' in s or '__ubsan_' in s})
        if not args.implementation and (not any('__asan_' in s for s in report['production_object_sanitizer_symbols']) or
                                        not any('__ubsan_' in s for s in report['production_object_sanitizer_symbols'])):
            raise ValueError('Production object lacks ASan/UBSan instrumentation')
        binary = out / 'native-glk-render-wa-tests'
        report['link'] = run([*common, *san, *objects, '-o', binary])
        if report['link']['returncode']: raise ValueError('Sanitized link failed')
        report['binary_sha256'] = sha(binary)
        env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:halt_on_error=1:detect_leaks=0',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        report['test'] = run([binary], 60, env)
        m = re.fullmatch(r'native_glk_render_wa_tests: ([0-9]+) checks passed\n', report['test']['stdout'])
        if report['test']['returncode'] or report['test']['stderr'] or not m:
            raise ValueError('Hosted producer tests or fatal sanitizer checks failed')
        report['checks'] = int(m.group(1))
        # Topology read adapter, hosted over the configuration-space shim.
        shim, adapter_objects = root / 'tests/native_glk_render_wa_shim', []
        for name, src in [('GlkTopologyIOKit', root / 'Mellow/GlkTopologyIOKit.cpp'),
                          ('topology_tests', root / 'tests/native_glk_topology_iokit_tests.cpp')]:
            obj = out / (name + '.san.o')
            report['compile'].append(run([*common, *san, '-I', shim, '-c', src, '-o', obj]))
            if report['compile'][-1]['returncode']: raise ValueError('Sanitized compile failed: ' + name)
            adapter_objects.append(obj)
        nm = run(['nm', '-u', adapter_objects[0]], 20)['stdout']
        report['adapter_object_sanitizer_symbols'] = sorted({s for s in nm.split() if '__asan_' in s or '__ubsan_' in s})
        if not any('__asan_' in s for s in report['adapter_object_sanitizer_symbols']) or \
           not any('__ubsan_' in s for s in report['adapter_object_sanitizer_symbols']):
            raise ValueError('Adapter object lacks ASan/UBSan instrumentation')
        adapter_binary = out / 'native-glk-topology-iokit-tests'
        report['adapter_link'] = run([*common, *san, *adapter_objects, objects[0], '-o', adapter_binary])
        if report['adapter_link']['returncode']: raise ValueError('Adapter sanitized link failed')
        report['adapter_binary_sha256'] = sha(adapter_binary)
        report['adapter_test'] = run([adapter_binary], 60, env)
        m = re.fullmatch(r'native_glk_topology_iokit_tests: ([0-9]+) checks passed\n', report['adapter_test']['stdout'])
        if report['adapter_test']['returncode'] or report['adapter_test']['stderr'] or not m:
            raise ValueError('Hosted topology adapter tests or fatal sanitizer checks failed')
        report['adapter_checks'] = int(m.group(1))
        if args.kernel_sdk:
            sdk = args.kernel_sdk.resolve()
            kernel_headers = sdk / 'System/Library/Frameworks/Kernel.framework/Headers'
            if not (sdk / 'SDKSettings.json').is_file() or not kernel_headers.is_dir():
                raise ValueError('Kernel SDK missing')
            resource_dir = run([args.cxx, '-print-resource-dir'], 20)['stdout'].strip()
            kflags = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                      '-target', 'x86_64-apple-macos15.8.1', '-isysroot', sdk, '-nostdinc', '-I', kernel_headers,
                      '-isystem', Path(resource_dir) / 'include', '-mkernel', '-fapple-kext', '-ffreestanding',
                      '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-DKERNEL', '-I', root]
            obj = out / 'RenderWorkarounds.kernel-x86_64.o'
            report['kernel_style_compile'] = run([*kflags, '-c', impl, '-o', obj])
            if report['kernel_style_compile']['returncode'] or report['kernel_style_compile']['stderr']:
                raise ValueError('x86_64 kernel-style compile failed')
            headers = run([*kflags, '-fsyntax-only', '-H', impl], 60)['stderr']
            report['kernel_style_headers'] = sorted({l.lstrip('. ').strip() for l in headers.splitlines() if l.startswith('.')})
            if not any(h.startswith(str(kernel_headers)) for h in report['kernel_style_headers']):
                raise ValueError('Kernel compile did not use Kernel.framework headers')
            report['kernel_style_undefined_symbols'] = run(['nm', '-u', obj], 20)['stdout'].split()
            report['kernel_style_object_sha256'] = sha(obj)
            report['kernel_sdk'] = {'path': str(sdk), 'SDKSettings.json_sha256': sha(sdk / 'SDKSettings.json')}
            akflags = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-target', 'x86_64-apple-macos15.8.1',
                       '-isysroot', sdk, '-nostdinc', '-isystem', Path(resource_dir) / 'include', '-isystem', kernel_headers,
                       '-mkernel', '-fapple-kext', '-fno-exceptions', '-fno-rtti', '-DKERNEL', '-DKERNEL_PRIVATE',
                       '-fno-builtin', '-fno-common', '-fno-asynchronous-unwind-tables', '-fvisibility=hidden',
                       '-fno-use-cxa-atexit', '-fno-threadsafe-statics', '-Wno-deprecated-declarations', '-I', root]
            report['adapter_kernel_style_warning_exception'] = ('-Wno-deprecated-declarations: the SDK marks IOPCIDevice '
                                                                '__kpi_deprecated("Use PCIDriverKit"); same exception as Tools/run-glk-dma-tests.py')
            adapter = root / 'Mellow/GlkTopologyIOKit.cpp'
            aobj = out / 'GlkTopologyIOKit.kernel-x86_64.o'
            report['adapter_kernel_style_compile'] = run([*akflags, '-c', adapter, '-o', aobj])
            if report['adapter_kernel_style_compile']['returncode'] or report['adapter_kernel_style_compile']['stderr']:
                raise ValueError('Adapter x86_64 kernel-style compile failed')
            aheaders = run([*akflags, '-fsyntax-only', '-H', adapter], 60)['stderr']
            report['adapter_kernel_style_pci_header'] = sorted({l.lstrip('. ').strip() for l in aheaders.splitlines()
                                                            if l.startswith('.') and l.rstrip().endswith('IOKit/pci/IOPCIDevice.h')})
            if not report['adapter_kernel_style_pci_header'] or not all(
                    h.startswith(str(kernel_headers)) for h in report['adapter_kernel_style_pci_header']):
                raise ValueError('Adapter kernel compile did not use the SDK IOPCIDevice.h')
            report['adapter_kernel_style_pci_header_sha256'] = sha(report['adapter_kernel_style_pci_header'][0])
            report['adapter_kernel_style_undefined_symbols'] = run(['nm', '-u', aobj], 20)['stdout'].split()
            report['adapter_kernel_style_object_sha256'] = sha(aobj)
            report['kernel_style_scope'] = 'Freestanding x86_64 object compile only; no KDK/KPI link, kext load or native execution'
        result = 0; report['status'] = 'PASS_HOSTED_PRODUCER_ONLY'
    except (OSError, ValueError, KeyError, AttributeError, subprocess.TimeoutExpired) as e:
        report['error'] = '%s: %s' % (type(e).__name__, e)
    finally:
        after = {n: (sha(root / n) if (root / n).is_file() else None) for n in SOURCES + LRC}
        report['source_unchanged'] = after == before
        if not report['source_unchanged']: result = 1; report['status'] = 'FAIL_SOURCE_CHANGED'
        (out / 'result.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'status': report['status'], 'checks': report.get('checks'), 'error': report.get('error'),
                      'hardware_support_qualified': False}))
    return result


if __name__ == '__main__':
    raise SystemExit(main())

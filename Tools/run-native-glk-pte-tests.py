#!/usr/bin/env python3
"""Hosted value tests for Drivers/NativeGlk/PageTableEncoding against pinned Linux.

Verifies the pinned torvalds/linux sources, generates a test-only oracle header
from them (verbatim MIT fragments plus numeric facts parsed from other files),
builds and runs tests/native_glk_pte_tests.cpp with and without sanitizers,
compiles the production unit with the repository's kernel flags, and checks
that seeded mutants of the production source are rejected. No GPU, MMIO, DMA,
kext loading or hardware access of any kind takes place.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UNIT = ROOT / "Drivers/NativeGlk"
HPP = UNIT / "PageTableEncoding.hpp"
CPP = UNIT / "PageTableEncoding.cpp"
PROVENANCE = UNIT / "PageTableEncoding.provenance.json"
TEST = ROOT / "tests/native_glk_pte_tests.cpp"
REVISION = "0d9ff90a5422cc7509258aaaba1e7481df4d332a"
PASS = "PASS_NATIVE_GLK_PTE_HOSTED_VALUES"

# (symbol, file) fragments compiled verbatim into the oracle, in emission order.
FUNCTIONS = [
    ("i915_pde_index", "drivers/gpu/drm/i915/gt/intel_gtt.h"),
    ("i915_vm_is_4lvl", "drivers/gpu/drm/i915/gt/intel_gtt.h"),
    ("gen8_pde_encode", "drivers/gpu/drm/i915/gt/gen8_ppgtt.c"),
    ("gen8_pte_encode", "drivers/gpu/drm/i915/gt/gen8_ppgtt.c"),
    ("gen12_pte_encode", "drivers/gpu/drm/i915/gt/gen8_ppgtt.c"),
    ("needs_wc_ggtt_mapping", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("mtl_ggtt_pte_encode", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("gen8_ggtt_pte_encode", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("gen8_ggtt_pte_decode", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("gen8_get_total_gtt_size", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("gen6_gttmmadr_size", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("gen6_gttadr_offset", "drivers/gpu/drm/i915/gt/intel_ggtt.c"),
    ("intel_ggtt_update_needs_vtd_wa", "drivers/gpu/drm/i915/gt/intel_gtt.c"),
    ("intel_vm_no_concurrent_access_wa", "drivers/gpu/drm/i915/gt/intel_gtt.c"),
    ("bdw_setup_private_ppat", "drivers/gpu/drm/i915/gt/intel_gtt.c"),
    ("chv_setup_private_ppat", "drivers/gpu/drm/i915/gt/intel_gtt.c"),
    ("setup_private_pat", "drivers/gpu/drm/i915/gt/intel_gtt.c"),
]
DEFINES = [
    ("drivers/gpu/drm/i915/gt/intel_gtt.h", [
        "I915_GTT_PAGE_SIZE_4K", "I915_GTT_PAGE_SIZE_64K", "I915_GTT_PAGE_SIZE_2M", "I915_GTT_PAGE_SIZE",
        "I915_PDES", "I915_PDE_MASK", "MTL_PPGTT_PTE_PAT3", "GEN12_PPGTT_PTE_LM", "GEN12_PPGTT_PTE_PAT2",
        "GEN12_PPGTT_PTE_PAT1", "GEN12_PPGTT_PTE_PAT0", "GEN12_GGTT_PTE_LM", "MTL_GGTT_PTE_PAT0",
        "MTL_GGTT_PTE_PAT1", "GEN12_GGTT_PTE_ADDR_MASK", "PPAT_UNCACHED", "PPAT_CACHED_PDE", "PPAT_CACHED",
        "PPAT_DISPLAY_ELLC", "CHV_PPAT_SNOOP", "GEN8_PPAT_AGE", "GEN8_PPAT_LLCeLLC", "GEN8_PPAT_LLCELLC",
        "GEN8_PPAT_LLC", "GEN8_PPAT_WB", "GEN8_PPAT_WT", "GEN8_PPAT_WC", "GEN8_PPAT_UC",
        "GEN8_PPAT_ELLC_OVERRIDE", "GEN8_PPAT", "GEN8_PAGE_PRESENT", "GEN8_PAGE_RW", "GEN8_PDE_IPS_64K",
        "GEN8_PDE_PS_2M", "PTE_READ_ONLY", "PTE_LM"]),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", [
        "GEN8_PAGE_SIZE", "GEN8_PTE_SHIFT", "GEN8_PDES", "gen8_pd_shift", "gen8_pd_index",
        "__gen8_pte_shift", "__gen8_pte_index"]),
    ("drivers/gpu/drm/i915/gt/intel_gt_regs.h", ["GEN8_PRIVATE_PAT_LO", "GEN8_PRIVATE_PAT_HI"]),
    ("include/drm/intel/i915_drm.h", ["BDW_GMCH_GGMS_SHIFT", "BDW_GMCH_GGMS_MASK"]),
    ("drivers/gpu/drm/i915/i915_drv.h", ["IS_GEN9_LP"]),
]
# Exact upstream statements whose presence fixes the GLK path through Linux.
ANCHORS = [
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\tppgtt->vm.top = i915_vm_is_4lvl(&ppgtt->vm) ? 3 : 2;\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\tppgtt->vm.has_read_only = !IS_GRAPHICS_VER(gt->i915, 11, 12);\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\tif (GRAPHICS_VER(gt->i915) >= 12)\n\t\tppgtt->vm.pte_encode = gen12_pte_encode;\n\telse\n\t\tppgtt->vm.pte_encode = gen8_pte_encode;\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\tpte_flags = vm->has_read_only;\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\t\tobj->encode = gen8_pde_encode(px_dma(obj), I915_CACHE_NONE);\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\t\t\tvaddr[index++] = encode | iter->dma;\n"),
    ("drivers/gpu/drm/i915/gt/gen8_ppgtt.c", "\t\t\tvaddr[maybe_64K] |= GEN8_PDE_IPS_64K;\n"),
    ("drivers/gpu/drm/i915/gt/intel_ppgtt.c", "\tppgtt->vm.total = BIT_ULL(RUNTIME_INFO(i915)->ppgtt_size);\n"),
    ("drivers/gpu/drm/i915/gt/intel_ppgtt.c", "\twrite_dma_entry(px_base(pd), idx, encode(px_dma(to), I915_CACHE_LLC));\n"),
    ("drivers/gpu/drm/i915/gt/intel_ppgtt.c", "\tvaddr[idx] = encoded_entry;\n\tdrm_clflush_virt_range(&vaddr[idx], sizeof(u64));\n"),
    ("drivers/gpu/drm/i915/gt/intel_ggtt.c", "\tif (IS_CHERRYVIEW(i915))\n\t\tsize = chv_get_total_gtt_size(snb_gmch_ctl);\n\telse\n\t\tsize = gen8_get_total_gtt_size(snb_gmch_ctl);\n"),
    ("drivers/gpu/drm/i915/gt/intel_ggtt.c", "\tggtt->vm.total = (size / sizeof(gen8_pte_t)) * I915_GTT_PAGE_SIZE;\n"),
    ("drivers/gpu/drm/i915/gt/intel_ggtt.c", "\tif (GRAPHICS_VER_FULL(i915) >= IP_VER(12, 70))\n\t\tggtt->vm.pte_encode = mtl_ggtt_pte_encode;\n\telse\n\t\tggtt->vm.pte_encode = gen8_ggtt_pte_encode;\n"),
    ("drivers/gpu/drm/i915/gt/intel_ggtt.c", "\t\t(gen8_pte_t __iomem *)ggtt->gsm + offset / I915_GTT_PAGE_SIZE;\n\n\tgen8_set_pte(pte, ggtt->vm.pte_encode(addr, pat_index, flags));\n"),
    ("drivers/gpu/drm/i915/gt/intel_ggtt.c", "\tif (needs_wc_ggtt_mapping(i915))\n\t\tggtt->gsm = ioremap_wc(phys_addr, size);\n\telse\n\t\tggtt->gsm = ioremap(phys_addr, size);\n"),
    ("drivers/gpu/drm/i915/gt/intel_gt.c", "\tif (HAS_LLC(gt->i915) || always_coherent)\n\t\treturn I915_MAP_WB;\n"),
    ("drivers/gpu/drm/i915/gt/intel_gtt.c", "int map_pt_dma(struct i915_address_space *vm, struct drm_i915_gem_object *obj)\n{\n\tenum i915_map_type type;\n\tvoid *vaddr;\n\n\ttype = intel_gt_coherent_map_type(vm->gt, obj, true);\n"),
    ("drivers/gpu/drm/i915/i915_pci.c", "static const struct intel_device_info glk_info = {\n\tGEN9_LP_FEATURES,\n\tPLATFORM(INTEL_GEMINILAKE),\n};\n"),
]
# Seeded production faults; each must make the oracle-enabled suite fail.
MUTANTS = [
    ("dma_mask_40_bits", "hpp", "constexpr uint32_t kDmaAddressBits = 39;", "constexpr uint32_t kDmaAddressBits = 40;"),
    ("ppgtt_va_47_bits", "hpp", "constexpr uint32_t kPpgttVirtualAddressBits = 48;", "constexpr uint32_t kPpgttVirtualAddressBits = 47;"),
    ("pat_bit_large_page_position", "hpp", "kPtePat = uint64_t(1) << 7;", "kPtePat = uint64_t(1) << 12;"),
    ("xe_pde_64k_bit", "hpp", "kPdeIps64K = uint64_t(1) << 11;", "kPdeIps64K = uint64_t(1) << 6;"),
    ("read_only_ignored", "cpp", "    if (pteFlags & kPteReadOnly) pte &= ~kEntryWritable;\n", ""),
    ("wt_maps_to_cached", "cpp", "case GlkCacheLevel::WriteThrough: bits = kPpatDisplayEllc;", "case GlkCacheLevel::WriteThrough: bits = kPpatCached;"),
    ("pat0_not_snooped", "cpp", "ppatEntry(0, Glk::kChvPpatSnoop) | ppatEntry(1, 0)", "ppatEntry(0, 0) | ppatEntry(1, 0)"),
    ("snb_ggms_shift", "cpp", "(gmchControl >> 6) & 0x3", "(gmchControl >> 8) & 0x3"),
    ("pde_always_uncached", "cpp", "pde |= caching == DirectoryCaching::Cached ? kPpatCachedPde : kPpatUncached;", "pde |= kPpatUncached;"),
    ("big_endian_image", "cpp", "out[i] = uint8_t(value >> (8 * i));", "out[i] = uint8_t(value >> (56 - 8 * i));"),
    ("dma_limit_off_by_one", "cpp", "if (dma >= Glk::kDmaAddressLimit)", "if (dma > Glk::kDmaAddressLimit)"),
    ("va_limit_off_by_one", "cpp", "if (virtualAddress >= Glk::kPpgttVirtualAddressLimit)", "if (virtualAddress > Glk::kPpgttVirtualAddressLimit)"),
    ("ggtt_accepts_xe_bits", "cpp", "    if (raw & ~kGgttAllowed) return PteStatus::ReservedBitsSet;\n", ""),
    ("leaf_any_pat_selector", "cpp", "        if (selector != 2 && selector != 3 && selector != 4) return PteStatus::InvalidPatIndex;\n", ""),
    ("64k_group_not_sequential", "cpp", "out[i] = encode | (dma + uint64_t(i) * Glk::kPageSize4K);", "out[i] = encode | dma;"),
    ("ggtt_entry_4_bytes", "cpp", "Glk::kGsmBar0Offset + index * Glk::kGgttEntryBytes;", "Glk::kGsmBar0Offset + index * 4;"),
    ("directory_ps_not_rejected_as_size", "cpp", "        if (raw & kPdePageSize)\n", "        if (false)\n"),
    ("ggtt_read_only_accepted", "cpp", "PteStatus status = checkPteFlags(pteFlags, false);", "PteStatus status = checkPteFlags(pteFlags & ~kPteReadOnly, false);"),
]


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def git_blob(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def verify_sources(linux, manifest):
    if manifest.get("revision") != REVISION:
        raise ValueError("unexpected upstream revision in provenance")
    texts = {}
    root = linux.resolve()
    for entry in manifest["upstream_files"]:
        path = (root / entry["path"]).resolve()
        if not path.is_relative_to(root) or path.is_symlink():
            raise ValueError("invalid upstream path " + entry["path"])
        data = path.read_bytes()
        if len(data) != entry["bytes"] or sha256_bytes(data) != entry["sha256"] or git_blob(data) != entry["git_blob"]:
            raise ValueError("pinned source mismatch: " + entry["path"])
        texts[entry["path"]] = data.decode("utf-8")
    return texts


def extract_function(text, name):
    lines = text.split("\n")
    pattern = re.compile(r"^(?:[A-Za-z_][\w \*]*[\s\*])?" + re.escape(name) + r"\(")
    starts = [i for i, line in enumerate(lines) if pattern.match(line) and not line.rstrip().endswith(";")]
    definitions = []
    for start in starts:
        end = start
        while end < len(lines) and not lines[end].rstrip().endswith(")") and "{" not in lines[end]:
            end += 1
        if end + 1 < len(lines) and (lines[end + 1] == "{" or "{" in lines[end]):
            definitions.append(start)
    if len(definitions) != 1:
        raise ValueError("expected one definition of %s, found %d" % (name, len(definitions)))
    start = definitions[0]
    if lines[start].startswith(name + "("):
        start -= 1  # return type on its own line
    end = start
    while lines[end] != "}":
        end += 1
    return "\n".join(lines[start:end + 1]), start + 1, end + 1


def extract_define(text, name):
    matches = [(i, line) for i, line in enumerate(text.split("\n"))
               if re.match(r"^#define\s+" + re.escape(name) + r"(?:\(|\s)", line)]
    if len(matches) != 1 or matches[0][1].rstrip().endswith("\\"):
        raise ValueError("expected one single-line #define " + name)
    return matches[0][1], matches[0][0] + 1


def macro_block(text, name):
    match = re.search(r"^#define " + re.escape(name) + r" \\\n((?:.*\\\n)*.*\n)", text, re.M)
    if not match:
        raise ValueError("macro block not found: " + name)
    return match.group(1)


def parse_glk_info(pci):
    block = macro_block(pci, "GEN9_LP_FEATURES")
    items = [item.strip() for item in block.replace("\\\n", "\n").replace("\n", " ").split(",") if item.strip()]
    fields = {}
    included = []
    for item in items:
        if "=" in item:
            key, value = (part.strip() for part in item.split("=", 1))
            fields[key] = value
        else:
            included.append(item)
    guarded = (".dma_mask_size", ".__runtime.ppgtt_size", ".max_pat_index", ".has_snoop",
               ".has_coherent_ggtt", ".has_llc", ".__runtime.page_sizes")
    for macro in included:
        name = macro.split("(")[0]
        if name in ("GEN", "PLATFORM"):
            continue
        body = macro_block(pci, name)
        hits = [key for key in guarded if key in body]
        if name == "GEN9_DEFAULT_PAGE_SIZES":
            if hits != [".__runtime.page_sizes"]:
                raise ValueError("unexpected GEN9_DEFAULT_PAGE_SIZES body")
            fields[".__runtime.page_sizes"] = re.sub(r"\s+", " ", body.split("=", 1)[1].replace("\\", " ")).strip()
        elif hits:
            raise ValueError("%s overrides %s" % (name, hits))
    if "GEN9_DEFAULT_PAGE_SIZES" not in included or ".has_llc" in fields:
        raise ValueError("unexpected GEN9_LP_FEATURES structure")
    required = {key: fields[key] for key in guarded if key in fields}
    numeric = {".dma_mask_size", ".__runtime.ppgtt_size", ".max_pat_index"}
    for key in numeric:
        if not re.fullmatch(r"\d+", required.get(key, "")):
            raise ValueError("missing numeric field " + key)
    for key in (".has_snoop", ".has_coherent_ggtt"):
        if required.get(key) not in ("true", "false"):
            raise ValueError("missing boolean field " + key)
    return required, included


def parse_x86_bits(pgtable):
    bits = {}
    for name in ("PRESENT", "RW", "PWT", "PCD", "PAT", "PSE"):
        found = re.findall(r"^#define _PAGE_BIT_" + name + r"\s+(\d+)\b", pgtable, re.M)
        if len(found) != 1:
            raise ValueError("x86 bit not found: " + name)
        bits[name] = int(found[0])
    return bits


def parse_cache_levels(objtypes):
    body = re.search(r"^enum i915_cache_level \{\n(.*?)^\};", objtypes, re.M | re.S)
    if not body:
        raise ValueError("enum i915_cache_level not found")
    code = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
    names = re.findall(r"^\s*(I915_CACHE_\w+)(?:\s*=\s*(\d+))?\s*,", code, re.M)
    if [n for n, _ in names] != ["I915_CACHE_NONE", "I915_CACHE_LLC", "I915_CACHE_L3_LLC", "I915_CACHE_WT"] or names[0][1] != "0":
        raise ValueError("unexpected i915_cache_level enumerators")
    return [n for n, _ in names]


def generate_oracle(texts):
    fragments = []
    permission = texts["LICENSES/preferred/MIT"].split("License-Text:\n", 1)[1]
    out = ["/* GENERATED TEST-ONLY ORACLE from torvalds/linux %s; not production code. */" % REVISION,
           "/* Copyright 2003 Tungsten Graphics, Inc.; Copyright 2016, 2020, 2022 Intel Corporation.\n" +
           permission + "\n*/", "#pragma once", "#include <stdint.h>",
           "#pragma clang diagnostic push",
           *('#pragma clang diagnostic ignored "%s"' % w for w in (
               "-Wunused-function", "-Wunused-parameter", "-Wunused-variable", "-Wsign-compare",
               "-Wconversion", "-Wshorten-64-to-32", "-Wimplicit-int-conversion", "-Wshadow",
               "-Wmissing-prototypes", "-Wunused-macros")),
           "typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;",
           "typedef u64 dma_addr_t; typedef u64 gen8_pte_t; typedef unsigned long pteval_t;",
           "#define BIT(nr) (1UL << (nr))", "#define BIT_ULL(nr) (1ULL << (nr))",
           "#define REG_BIT(n) ((u32)(1U << (n)))",
           "#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))",
           "#define _AT(T, X) ((T)(X))", "#define unlikely(x) (x)", "#define WARN_ON_ONCE(x) ((void)(x))",
           "#define GEM_BUG_ON(x) do { if (x) __builtin_trap(); } while (0)",
           "#define SZ_4K 0x00001000", "#define SZ_4M 0x00400000", "#define SZ_16M 0x01000000",
           "#define lower_32_bits(n) ((u32)(n))", "#define upper_32_bits(n) ((u32)((n) >> 32))",
           "static constexpr unsigned oracle_ilog2(unsigned long long v) { return v > 1 ? 1 + oracle_ilog2(v >> 1) : 0; }",
           "#define ilog2(n) oracle_ilog2(n)",
           "struct i915_reg_t { u32 reg; };", "#define _MMIO(r) (i915_reg_t{(u32)(r)})"]
    bits = parse_x86_bits(texts["arch/x86/include/asm/pgtable_types.h"])
    out.append("/* numeric facts parsed from arch/x86/include/asm/pgtable_types.h (GPL-2.0; no text copied) */")
    for name in ("PRESENT", "RW", "PWT", "PCD", "PAT", "PSE"):
        out.append("#define _PAGE_%s (_AT(pteval_t, 1) << %d)" % (name, bits[name]))
    levels = parse_cache_levels(texts["drivers/gpu/drm/i915/gem/i915_gem_object_types.h"])
    out.append("enum i915_cache_level { %s = 0, %s };" % (levels[0], ", ".join(levels[1:])))
    out += ["struct drm_i915_private { int graphics_ver; };",
            "struct intel_uncore { struct drm_i915_private *i915; unsigned count; u32 offsets[8]; u32 values[8]; };",
            "enum intel_gt_type { GT_PRIMARY, GT_TILE, GT_MEDIA };",
            "struct intel_gt { struct intel_uncore *uncore; struct drm_i915_private *i915; enum intel_gt_type type; };",
            "struct i915_address_space { u64 total; };",
            "static struct drm_i915_private oracle_glk_i915 = { 9 };",
            "static struct intel_gt oracle_glk_gt = { nullptr, &oracle_glk_i915, GT_PRIMARY };",
            "/* GLK identity: i915_pci.c GEN(9) + PLATFORM(INTEL_GEMINILAKE); worst case VT-d active. */",
            "#define GRAPHICS_VER(i915) 9", "#define IP_VER(ver, rel) ((ver) << 8 | (rel))",
            "#define GRAPHICS_VER_FULL(i915) IP_VER(9, 0)",
            "#define IS_GRAPHICS_VER(i915, from, until) (GRAPHICS_VER(i915) >= (from) && GRAPHICS_VER(i915) <= (until))",
            "#define IS_GEMINILAKE(i915) 1", "#define IS_BROXTON(i915) 0", "#define IS_CHERRYVIEW(i915) 0",
            "#define i915_vtd_active(i915) 1",
            "static inline void oracle_uncore_write(struct intel_uncore *u, i915_reg_t r, u32 v) {",
            "    if (u->count < 8) { u->offsets[u->count] = r.reg; u->values[u->count] = v; } ++u->count; }",
            "#define intel_uncore_write(u, r, v) oracle_uncore_write((u), (r), (v))",
            "enum { ORACLE_PPAT_NONE, ORACLE_PPAT_CHV, ORACLE_PPAT_BDW, ORACLE_PPAT_OTHER };",
            "static int oracle_selected_ppat = ORACLE_PPAT_NONE;"]
    for stub, arg in (("xelpmp", "intel_uncore"), ("xelpg", "intel_gt"), ("xehp", "intel_gt"),
                      ("tgl", "intel_uncore"), ("icl", "intel_uncore")):
        out.append("static void %s_setup_private_ppat(struct %s *) { oracle_selected_ppat = ORACLE_PPAT_OTHER; }" % (stub, arg))
    out.append("/* ---- verbatim pinned MIT fragments ---- */")
    for path, names in DEFINES:
        for name in names:
            line, number = extract_define(texts[path], name)
            out.append(line)
            fragments.append({"symbol": name, "kind": "define", "file": path, "line": number,
                              "sha256": sha256_bytes(line.encode())})
    for name, path in FUNCTIONS:
        if name == "setup_private_pat":
            out.append("#define chv_setup_private_ppat(u) (oracle_selected_ppat = ORACLE_PPAT_CHV, chv_setup_private_ppat(u))")
            out.append("#define bdw_setup_private_ppat(u) (oracle_selected_ppat = ORACLE_PPAT_BDW, bdw_setup_private_ppat(u))")
        body, first, last = extract_function(texts[path], name)
        out.append(body)
        fragments.append({"symbol": name, "kind": "function", "file": path, "start_line": first,
                          "end_line": last, "sha256": sha256_bytes(body.encode())})
    info, included = parse_glk_info(texts["drivers/gpu/drm/i915/i915_pci.c"])
    out += ["/* parsed from i915_pci.c GEN9_LP_FEATURES (glk_info) */",
            "#define ORACLE_GLK_DMA_MASK_SIZE %s" % info[".dma_mask_size"],
            "#define ORACLE_GLK_PPGTT_SIZE %s" % info[".__runtime.ppgtt_size"],
            "#define ORACLE_GLK_MAX_PAT_INDEX %s" % info[".max_pat_index"],
            "#define ORACLE_GLK_PAGE_SIZES (%s)" % info[".__runtime.page_sizes"],
            "#define ORACLE_GLK_HAS_SNOOP %s" % info[".has_snoop"],
            "#define ORACLE_GLK_HAS_COHERENT_GGTT %s" % info[".has_coherent_ggtt"],
            "#define ORACLE_GLK_HAS_LLC 0",
            "static inline bool oracle_glk_has_read_only() { struct intel_gt *gt = &oracle_glk_gt; (void)gt;",
            "    return !IS_GRAPHICS_VER(gt->i915, 11, 12); }",
            "#define ORACLE_GLK_PPGTT_HAS_READ_ONLY (oracle_glk_has_read_only())",
            "static inline unsigned oracle_glk_top() { struct i915_address_space vm = { BIT_ULL(ORACLE_GLK_PPGTT_SIZE) };",
            "    return i915_vm_is_4lvl(&vm) ? 3 : 2; }",
            "#define ORACLE_GLK_PPGTT_TOP (oracle_glk_top())",
            "#pragma clang diagnostic pop"]
    for path, anchor in ANCHORS:
        if texts[path].count(anchor) != 1:
            raise ValueError("pinned anchor missing in %s: %r" % (path, anchor[:60]))
    return "\n".join(out) + "\n", fragments, {"glk_info": info, "included_macros": included}


def run(command, cwd, timeout=300):
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)
    return {"command": [str(c) for c in command], "returncode": result.returncode,
            "stdout": result.stdout[-4000:], "stderr": result.stderr[-4000:]}


def last_json(text):
    for line in reversed(text.strip().splitlines()):
        try:
            return json.loads(line)
        except ValueError:
            continue
    return {}


def host_suite(cxx, out, name, sources, extra, run_prefix=()):
    binary = out / name
    command = [cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
               "-DMELLOW_GLK_PTE_ORACLE=1", "-I", str(out), *extra, *map(str, sources), "-o", str(binary)]
    record = {"compile": run(command, out)}
    if record["compile"]["returncode"] == 0:
        record["execution"] = run([*run_prefix, str(binary)], out, timeout=600)
        record["result"] = last_json(record["execution"]["stdout"])
        if any(flag.startswith("-fsanitize=") for flag in extra):
            symbols = run(["nm", "-u", str(binary)], out)
            record["sanitizer_symbol_audit"] = symbols
            record["sanitizer_instrumentation_verified"] = (symbols["returncode"] == 0 and
                "__asan_init" in symbols["stdout"] and "__ubsan_handle" in symbols["stdout"])
    return record


def kernel_compile(cxx, out, variant, extra):
    resource = subprocess.run([cxx, "-print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    obj = out / ("PageTableEncoding-kernel-%s.o" % variant)
    # Flags mirror Tools/cross-build.py; -Werror added for this unit.
    command = [cxx, "-target", "x86_64-apple-macos13", "-std=c++17", "-mkernel", "-fapple-kext", "-nostdinc",
               "-isystem", resource + "/include", "-I", str(ROOT / "MacKernelSDK/Headers"),
               "-DKERNEL", "-DKERNEL_PRIVATE", "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-common",
               "-fno-asynchronous-unwind-tables", "-fvisibility=hidden", "-Wall", "-Wextra", "-Werror",
               *extra, "-c", str(CPP), "-o", str(obj)]
    record = {"variant": variant, "compile": run(command, out)}
    if record["compile"]["returncode"] == 0:
        undefined = run(["nm", "-u", str(obj)], out)
        record["undefined_symbols"] = [s for s in undefined["stdout"].split() if s]
        record["object_sha256"] = sha256_bytes(obj.read_bytes())
    return record


def mutants(cxx, out):
    results = []
    for name, target, old, new in MUTANTS:
        directory = out / "mutants" / name
        (directory / "Drivers/NativeGlk").mkdir(parents=True)
        (directory / "tests").mkdir()
        hpp, cpp = HPP.read_text(encoding="utf-8"), CPP.read_text(encoding="utf-8")
        source = hpp if target == "hpp" else cpp
        if source.count(old) != 1:
            results.append({"mutant": name, "status": "ERROR_TARGET_NOT_UNIQUE"})
            continue
        if target == "hpp":
            hpp = hpp.replace(old, new)
        else:
            cpp = cpp.replace(old, new)
        (directory / "Drivers/NativeGlk/PageTableEncoding.hpp").write_text(hpp, encoding="utf-8")
        (directory / "Drivers/NativeGlk/PageTableEncoding.cpp").write_text(cpp, encoding="utf-8")
        shutil.copy(TEST, directory / "tests/native_glk_pte_tests.cpp")
        shutil.copy(out / "native_glk_pte_pinned_oracle.h", directory / "tests")
        record = host_suite(cxx, directory, "mutant-tests",
                            [directory / "Drivers/NativeGlk/PageTableEncoding.cpp",
                             directory / "tests/native_glk_pte_tests.cpp"], ["-Wno-error", "-I", str(directory / "tests")])
        failed_compile = record["compile"]["returncode"] != 0
        failed_run = not failed_compile and record["execution"]["returncode"] != 0
        status = "KILLED_COMPILE" if failed_compile else ("KILLED" if failed_run else "SURVIVED")
        results.append({"mutant": name, "file": target, "status": status,
                        "failures": record.get("result", {}).get("failures")})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-root", type=Path, required=True, help="Directory holding the pinned Linux files")
    parser.add_argument("--out", type=Path, required=True, help="New empty scratch directory outside the repository")
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--skip-mutants", action="store_true")
    args = parser.parse_args()
    if "CCC_OVERRIDE_OPTIONS" in os.environ:
        parser.error("CCC_OVERRIDE_OPTIONS must be absent (including an empty value)")
    out = args.out.resolve()
    if out.is_relative_to(ROOT) or ROOT.is_relative_to(out):
        parser.error("scratch output must not overlap the repository")
    if args.out.is_symlink() or (out.exists() and (not out.is_dir() or any(out.iterdir()))):
        parser.error("scratch output must be new or empty")
    if not args.cxx:
        parser.error("clang++ unavailable")
    out.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "status": "FAIL", "scope": "hosted value tests of a pure encoder",
              "hardware_execution": False, "mmio_or_dma": False, "kext_loaded": False, "metal_tested": False,
              "qualifies_acceleration": False}
    try:
        inputs = (HPP, CPP, PROVENANCE, TEST, Path(__file__).resolve())
        def source_hashes():
            return {p.relative_to(ROOT).as_posix(): sha256_bytes(p.read_bytes()) for p in inputs}
        before = source_hashes()
        manifest = json.loads(PROVENANCE.read_text(encoding="utf-8"))
        texts = verify_sources(args.linux_root, manifest)
        report["upstream_files_verified"] = len(texts)
        oracle, fragments, parsed = generate_oracle(texts)
        recorded = {(f["symbol"], f["file"]): f["sha256"] for f in manifest.get("oracle_fragments", [])}
        mismatched = [f["symbol"] for f in fragments if recorded.get((f["symbol"], f["file"])) != f["sha256"]]
        report["oracle_fragments"] = fragments
        report["oracle_fragment_mismatches"] = mismatched
        report["parsed_device_info"] = parsed
        (out / "native_glk_pte_pinned_oracle.h").write_text(oracle, encoding="utf-8")
        report["oracle_header_sha256"] = sha256_bytes(oracle.encode())
        report["source_sha256"] = before
        sources = [CPP, TEST]
        report["host"] = host_suite(args.cxx, out, "native-glk-pte-tests", sources, [])
        report["host_sanitized"] = host_suite(args.cxx, out, "native-glk-pte-tests-asan-ubsan", sources,
                                              ["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"])
        rosetta = run(["arch", "-x86_64", "/usr/bin/true"], out)
        if rosetta["returncode"] == 0:
            report["host_x86_64"] = host_suite(args.cxx, out, "native-glk-pte-tests-x86_64", sources,
                                               ["-arch", "x86_64"], ("arch", "-x86_64"))
        else:
            report["host_x86_64"] = {"status": "NOT_RUN", "reason": rosetta["stderr"].strip()[:200]}
        report["kernel_objects"] = [
            kernel_compile(args.cxx, out, "release-O3", ["-O3"]),
            kernel_compile(args.cxx, out, "debug-O0", ["-O0", "-DDEBUG=1", "-DAPPLE_KEXT_ASSERTIONS=1"]),
            kernel_compile(args.cxx, out, "xcode-other-cflags", ["-O3", "-mno-mmx", "-mno-sse", "-mno-sse2", "-mno-sse3",
                                                                 "-mno-red-zone", "-fno-slp-vectorize", "-fno-vectorize"])]
        if not args.skip_mutants:
            report["mutants"] = mutants(args.cxx, out)
        suites = [report["host"], report["host_sanitized"]]
        if "result" in report["host_x86_64"]:
            suites.append(report["host_x86_64"])
        ok = all(s.get("execution", {}).get("returncode") == 0 and
                 s.get("result", {}).get("status") == PASS and s["result"].get("pinned_linux_oracle") is True
                 and s["result"].get("hardware_execution") is False and
                 s["result"].get("hosted_value_tests_only") is True and
                 type(s["result"].get("checks")) is int and s["result"]["checks"] > 0 and
                 type(s["result"].get("oracle_vectors")) is int and s["result"]["oracle_vectors"] > 0 and
                 s["result"].get("failures") == 0 for s in suites)
        ok = ok and report["host_sanitized"].get("sanitizer_instrumentation_verified") is True
        report["source_sha256_after"] = source_hashes()
        report["sources_unchanged"] = before == report["source_sha256_after"]
        verify_sources(args.linux_root, manifest)
        ok = ok and report["sources_unchanged"]
        ok = ok and not mismatched
        # -mkernel enables compiler auto-initialization in Debug. Its sole
        # libc primitive is admitted only with the pinned 24H32 exported-symbol
        # witness; optimized variants must still be completely import-free.
        debug_import = manifest.get("debug_kernel_import", {})
        debug_eligible = (debug_import.get("symbol") == "_memset" and
            debug_import.get("eligibility", {}).get("static_declared_provider_match") is True)
        ok = ok and all(k["compile"]["returncode"] == 0 and
            (k.get("undefined_symbols") == [] or
             (k["variant"] == "debug-O0" and k.get("undefined_symbols") == ["_memset"] and debug_eligible))
            for k in report["kernel_objects"])
        report["debug_kernel_import_witness"] = debug_import
        ok = ok and all(m["status"].startswith("KILLED") for m in report.get("mutants", []))
        if ok:
            report["status"] = PASS
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = "%s: %s" % (type(error).__name__, error)
    (out / "native-glk-pte-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    summary = {"status": report["status"], "checks": report.get("host", {}).get("result", {}).get("checks", 0),
               "oracle_vectors": report.get("host", {}).get("result", {}).get("oracle_vectors", 0),
               "mutants_killed": sum(m["status"].startswith("KILLED") for m in report.get("mutants", [])),
               "mutants_total": len(report.get("mutants", [])), "hardware_execution": False,
               "report": str(out / "native-glk-pte-report.json")}
    print(json.dumps(summary))
    return 0 if report["status"] == PASS else 2


if __name__ == "__main__":
    raise SystemExit(main())

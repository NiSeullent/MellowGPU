// SPDX-License-Identifier: MIT
/*
 * Gemini Lake (Gen9 LP, i915/execlists) GGTT and PPGTT entry encoding.
 *
 * Constants and encoding rules are re-expressed (no verbatim code) from
 * torvalds/linux 0d9ff90a5422cc7509258aaaba1e7481df4d332a:
 *   drivers/gpu/drm/i915/gt/gen8_ppgtt.c     Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_ggtt.c     Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_gtt.c      Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_gtt.h      Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_ppgtt.c    Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_gt_regs.h  Copyright © 2022 Intel Corporation
 *   drivers/gpu/drm/i915/i915_pci.c          Copyright © 2016 Intel Corporation
 *   include/drm/intel/i915_drm.h             Copyright 2003 Tungsten Graphics, Inc.
 * Copyright (c) 2026 Mellow contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * See PageTableEncoding.provenance.json for blob ids, SHA-256 and symbols.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure value encoding only. Nothing here maps, allocates, flushes, writes MMIO,
// programs PAT registers or invalidates TLBs; it is not evidence that any GLK
// page table has ever been walked by hardware. Every encoder leaves its output
// untouched unless it returns PteStatus::Ok.
namespace Mellow { namespace NativeGlk {

enum class PteStatus : uint8_t {
    Ok,
    InvalidArgument,          // null buffer, bad count/length, unknown flag bit
    UnalignedDmaAddress,
    DmaAddressOutOfRange,     // at or above the GLK 39-bit DMA mask
    UnalignedVirtualAddress,
    VirtualAddressOutOfRange, // at or above 2^48 (PPGTT) or the GGTT size
    RangeOverflow,            // start + length wraps 64 bits
    InvalidPatIndex,          // > max_pat_index, or PAT bits Linux never emits
    InvalidLevel,
    UnsupportedPageSize,      // 2M/1G leaves are not enabled for GLK
    UnsupportedFlag,          // PTE_LM, GGTT read-only, read-only directory
    NotPresent,
    ReservedBitsSet,          // bits outside the GLK Gen8 entry format
    InvalidGgttSize,          // GMCH GGMS field selects no GTT
};

namespace Glk {
// i915_pci.c GEN9_LP_FEATURES (glk_info): DMA addresses placed in any entry
// are bounded by dma_mask_size; PPGTT virtual addresses by ppgtt_size.
// The two widths are distinct and must never be interchanged.
constexpr uint32_t kDmaAddressBits = 39;
constexpr uint32_t kPpgttVirtualAddressBits = 48;
constexpr uint64_t kDmaAddressLimit = uint64_t(1) << kDmaAddressBits;
constexpr uint64_t kPpgttVirtualAddressLimit = uint64_t(1) << kPpgttVirtualAddressBits;
constexpr uint32_t kMaxPatIndex = 3;             // .max_pat_index
constexpr uint64_t kPageSize4K = uint64_t(1) << 12;
constexpr uint64_t kPageSize64K = uint64_t(1) << 16;
constexpr uint64_t kPageSize2M = uint64_t(1) << 21;
constexpr uint64_t kSupportedPageSizes = kPageSize4K | kPageSize64K; // GEN9_DEFAULT_PAGE_SIZES
constexpr bool kHasLlc = false;                  // GEN9_LP_FEATURES sets no has_llc
constexpr bool kHasSnoop = true;
constexpr bool kHasCoherentGgtt = false;
// gen8_ppgtt_create: has_read_only = !IS_GRAPHICS_VER(11, 12); GLK is ver 9.
constexpr bool kPpgttReadOnlySupported = true;

// gen8_ppgtt.c: 4K tables of 512 u64 entries; ppgtt_size 48 makes the VM
// 4-level (vm.top = 3): PML4E 47:39 | PDPE 38:30 | PDE 29:21 | PTE 20:12.
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kLevelShift = 9;
constexpr uint32_t kEntriesPerTable = 512;
constexpr uint32_t kTopLevel = 3;
constexpr uint32_t kEntriesPer64KPage = 16;      // every 16th PTE in IPS mode

// chv_setup_private_ppat (used for IS_GEN9_LP): only the snoop bit is
// meaningful. The hardware never snoops PPGTT page-table walks nor CPU
// GMADR->GGTT accesses; GGTT has no PAT selector and always uses entry 0.
constexpr uint32_t kPrivatePatLoRegister = 0x40e0; // GEN8_PRIVATE_PAT_LO
constexpr uint32_t kPrivatePatHiRegister = 0x40e4; // GEN8_PRIVATE_PAT_HI
constexpr uint8_t kChvPpatSnoop = 1u << 6;         // CHV_PPAT_SNOOP
constexpr bool kPageWalkSnooped = false;
constexpr bool kCpuApertureSnooped = false;

// intel_ggtt.c: gen8 GTTMMADR (BAR0) is 16 MiB with the GSM at +8 MiB.
// needs_wc_ggtt_mapping() is false for GEN9_LP: writes wider than 64 bits to
// the GSM are dropped, so the GSM must be mapped uncached and each entry stored
// as one 64-bit write; gen8_ggtt_invalidate then performs no GFX_FLSH_CNTL
// write. The BXT VT-d stop_machine serialization does not apply to GLK.
constexpr uint64_t kGttMmadrBytes = uint64_t(16) << 20;
constexpr uint64_t kGsmBar0Offset = uint64_t(8) << 20;
constexpr uint32_t kGgttEntryBytes = 8;
constexpr bool kGsmMappingMustBeUncached = true;
constexpr bool kGgttInvalidateWritesGfxFlush = false;
constexpr bool kGgttUpdateNeedsVtdSerialization = false;
} // namespace Glk

// Linux PTE flag values (intel_gtt.h PTE_READ_ONLY / PTE_LM).
constexpr uint32_t kPteReadOnly = 1u << 0;
constexpr uint32_t kPteLocalMemory = 1u << 1;   // always rejected: GLK has no LMEM

// Gen8 entry bits used by GLK. Bit 7 is the PAT bit in a 4K PTE but the
// page-size bit in a directory entry; Linux never sets it in a GLK PDE.
constexpr uint64_t kEntryPresent = uint64_t(1) << 0; // GEN8_PAGE_PRESENT
constexpr uint64_t kEntryWritable = uint64_t(1) << 1; // GEN8_PAGE_RW
constexpr uint64_t kEntryPwt = uint64_t(1) << 3;     // _PAGE_PWT
constexpr uint64_t kEntryPcd = uint64_t(1) << 4;     // _PAGE_PCD
constexpr uint64_t kPtePat = uint64_t(1) << 7;       // _PAGE_PAT (4K PTE)
constexpr uint64_t kPdePageSize = uint64_t(1) << 7;  // GEN8_PDE_PS_2M / 1G
constexpr uint64_t kPdeIps64K = uint64_t(1) << 11;   // GEN8_PDE_IPS_64K
constexpr uint64_t kDmaAddressMask =
    (Glk::kDmaAddressLimit - 1) & ~(Glk::kPageSize4K - 1); // bits 38:12

// gen8_pte_encode PAT selection for pre-gen12 (pat_index == cache level).
enum class GlkCacheLevel : uint8_t { None = 0, Llc = 1, L3Llc = 2, WriteThrough = 3 };

// Directory caching as passed to gen8_pde_encode: Cached is what
// __set_pd_entry uses (I915_CACHE_LLC); Uncached is the scratch directory form.
enum class DirectoryCaching : uint8_t { Cached, Uncached };

struct PpgttEntryView {
    uint64_t dmaAddress;
    uint8_t level;
    uint8_t patSelector;     // (PAT << 2) | (PCD << 1) | PWT into GEN8_PRIVATE_PAT
    bool writable;
    bool ips64K;             // level-1 entry selecting a 64K-mode page table
    bool gpuAccessSnooped;   // leaf: CHV PPAT snoop; directory: false (walk)
};

struct GgttEntryView {
    uint64_t dmaAddress;
    bool gpuAccessSnooped;   // PAT entry 0 carries CHV_PPAT_SNOOP
    bool cpuApertureSnooped; // always false on GLK
};

// Address and range validation.
PteStatus checkDmaAddress(uint64_t dma, uint64_t alignment);
PteStatus ppgttIndex(uint64_t virtualAddress, uint32_t level, uint32_t &index);
PteStatus checkPpgttRange(uint64_t virtualAddress, uint64_t length, uint64_t pageSize);

// PAT semantics.
uint64_t chvPrivatePatValue();
bool chvPatSelectorSnoops(uint8_t selector);
PteStatus ppgttPatBits(uint32_t patIndex, uint64_t &bits);

// PPGTT encoders (gen8_pte_encode / gen8_pde_encode with GLK checks).
PteStatus encodePpgttPte(uint64_t dma, uint32_t patIndex, uint32_t pteFlags, uint64_t &out);
PteStatus encodePpgttDirectoryEntry(uint32_t level, uint64_t tableDma, DirectoryCaching caching,
                                    bool ips64K, uint64_t &out);
// gen8_init_scratch: read-only uncached scratch PTE, uncached scratch PDEs.
PteStatus encodePpgttScratchPte(uint64_t scratchDma, uint64_t &out);
PteStatus encodePpgttScratchDirectoryEntry(uint32_t level, uint64_t scratchTableDma, uint64_t &out);
// gen8_ppgtt_insert_huge 64K path: all sixteen 4K PTEs of one 64K page.
PteStatus encodePpgtt64KPage(uint64_t dma, uint32_t patIndex, uint32_t pteFlags,
                             uint64_t (&out)[Glk::kEntriesPer64KPage]);
// A page table may be marked IPS only if every 16th PTE is present and
// points at a 64K-aligned region (setup_scratch_page requirement).
PteStatus check64KPageTable(const uint64_t *entries, size_t count);
PteStatus decodePpgttEntry(uint64_t raw, uint32_t level, PpgttEntryView &out);

// GGTT (gen8_ggtt_pte_encode: no RW, no PAT bits on GLK).
PteStatus encodeGgttPte(uint64_t dma, uint32_t pteFlags, uint64_t &out);
PteStatus decodeGgttPte(uint64_t raw, GgttEntryView &out);
PteStatus ggttTotalFromGmchControl(uint16_t gmchControl, uint64_t &totalBytes);
PteStatus checkGgttRange(uint64_t offset, uint64_t length, uint64_t ggttTotal);
PteStatus ggttEntryLocation(uint64_t offset, uint64_t ggttTotal, uint64_t &entryIndex,
                            uint64_t &bar0ByteOffset);

// Explicit little-endian entry images, independent of host byte order.
void storeLe64(uint64_t value, uint8_t (&out)[8]);
uint64_t loadLe64(const uint8_t (&in)[8]);
PteStatus serializeEntriesLe(const uint64_t *entries, size_t count, uint8_t *out, size_t outBytes);
PteStatus deserializeEntriesLe(const uint8_t *in, size_t inBytes, uint64_t *entries, size_t count);

} } // namespace Mellow::NativeGlk

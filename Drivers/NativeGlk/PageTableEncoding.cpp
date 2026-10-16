// SPDX-License-Identifier: MIT
/*
 * Copyright © 2016, 2020, 2022 Intel Corporation
 * Copyright 2003 Tungsten Graphics, Inc.
 * Copyright (c) 2026 Mellow contributors
 * Full permission notice and per-file upstream notices: PageTableEncoding.hpp.
 * Exact upstream blobs, SHA-256 and symbols: PageTableEncoding.provenance.json.
 */
#include "PageTableEncoding.hpp"

namespace Mellow { namespace NativeGlk {
namespace {
constexpr uint64_t kOneGiB = uint64_t(1) << 30;
constexpr uint32_t kKnownPteFlags = kPteReadOnly | kPteLocalMemory;

// intel_gtt.h PPAT selections, expressed as PWT/PCD/PAT bit combinations.
constexpr uint64_t kPpatUncached = kEntryPwt | kEntryPcd;  // PPAT_UNCACHED
constexpr uint64_t kPpatCachedPde = 0;                      // PPAT_CACHED_PDE
constexpr uint64_t kPpatCached = kPtePat;                   // PPAT_CACHED
constexpr uint64_t kPpatDisplayEllc = kEntryPcd;            // PPAT_DISPLAY_ELLC

// Every bit a GLK entry may carry; Linux emits nothing else on Gen9 LP.
constexpr uint64_t kLeafAllowed =
    kEntryPresent | kEntryWritable | kEntryPwt | kEntryPcd | kPtePat | kDmaAddressMask;
constexpr uint64_t kDirectoryAllowed =
    kEntryPresent | kEntryWritable | kEntryPwt | kEntryPcd | kDmaAddressMask;
constexpr uint64_t kGgttAllowed = kEntryPresent | kDmaAddressMask;

// GEN8_PPAT(i, x): one byte per PAT selector.
constexpr uint64_t ppatEntry(uint32_t selector, uint64_t value) {
    return value << (selector * 8);
}

PteStatus checkPteFlags(uint32_t flags, bool readOnlyRepresentable) {
    if (flags & ~kKnownPteFlags) return PteStatus::InvalidArgument;
    if (flags & kPteLocalMemory) return PteStatus::UnsupportedFlag;
    if ((flags & kPteReadOnly) && !readOnlyRepresentable) return PteStatus::UnsupportedFlag;
    return PteStatus::Ok;
}

uint8_t leafSelector(uint64_t entry) {
    return uint8_t(((entry & kPtePat) ? 4 : 0) | ((entry & kEntryPcd) ? 2 : 0) |
                   ((entry & kEntryPwt) ? 1 : 0));
}

// gen8_get_total_gtt_size can only yield 1, 2 or 4 GiB of GGTT on x86_64.
bool validGgttTotal(uint64_t total) {
    return total == kOneGiB || total == 2 * kOneGiB || total == 4 * kOneGiB;
}

PteStatus checkPageSize(uint64_t pageSize) {
    if (pageSize == Glk::kPageSize2M || pageSize == kOneGiB) return PteStatus::UnsupportedPageSize;
    if (pageSize != Glk::kPageSize4K && pageSize != Glk::kPageSize64K) return PteStatus::InvalidArgument;
    return PteStatus::Ok;
}

void putLe64(uint64_t value, uint8_t *out) {
    for (unsigned i = 0; i < 8; ++i) out[i] = uint8_t(value >> (8 * i));
}

uint64_t getLe64(const uint8_t *in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(in[i]) << (8 * i);
    return value;
}
} // namespace

PteStatus checkDmaAddress(uint64_t dma, uint64_t alignment) {
    const PteStatus size = checkPageSize(alignment);
    if (size != PteStatus::Ok) return size;
    if (dma & (alignment - 1)) return PteStatus::UnalignedDmaAddress;
    if (dma >= Glk::kDmaAddressLimit) return PteStatus::DmaAddressOutOfRange;
    return PteStatus::Ok;
}

PteStatus ppgttIndex(uint64_t virtualAddress, uint32_t level, uint32_t &index) {
    if (level > Glk::kTopLevel) return PteStatus::InvalidLevel;
    if (virtualAddress >= Glk::kPpgttVirtualAddressLimit) return PteStatus::VirtualAddressOutOfRange;
    // __gen8_pte_index(a, lvl): (a >> (12 + 9 * lvl)) & (I915_PDES - 1).
    index = uint32_t((virtualAddress >> (Glk::kPageShift + Glk::kLevelShift * level)) &
                     (Glk::kEntriesPerTable - 1));
    return PteStatus::Ok;
}

PteStatus checkPpgttRange(uint64_t virtualAddress, uint64_t length, uint64_t pageSize) {
    const PteStatus size = checkPageSize(pageSize);
    if (size != PteStatus::Ok) return size;
    if (!length || (length & (pageSize - 1))) return PteStatus::InvalidArgument;
    if (virtualAddress & (pageSize - 1)) return PteStatus::UnalignedVirtualAddress;
    if (virtualAddress > ~uint64_t(0) - length) return PteStatus::RangeOverflow;
    if (virtualAddress + length > Glk::kPpgttVirtualAddressLimit) return PteStatus::VirtualAddressOutOfRange;
    return PteStatus::Ok;
}

uint64_t chvPrivatePatValue() {
    // chv_setup_private_ppat: snoop on selectors 0 and 4-7, nothing else.
    return ppatEntry(0, Glk::kChvPpatSnoop) | ppatEntry(1, 0) | ppatEntry(2, 0) | ppatEntry(3, 0) |
           ppatEntry(4, Glk::kChvPpatSnoop) | ppatEntry(5, Glk::kChvPpatSnoop) |
           ppatEntry(6, Glk::kChvPpatSnoop) | ppatEntry(7, Glk::kChvPpatSnoop);
}

bool chvPatSelectorSnoops(uint8_t selector) {
    return selector < 8 && ((chvPrivatePatValue() >> (selector * 8)) & Glk::kChvPpatSnoop) != 0;
}

PteStatus ppgttPatBits(uint32_t patIndex, uint64_t &bits) {
    if (patIndex > Glk::kMaxPatIndex) return PteStatus::InvalidPatIndex;
    // LEGACY_CACHELEVEL: pre-gen12 pat_index is the i915_cache_level value.
    switch (static_cast<GlkCacheLevel>(patIndex)) {
    case GlkCacheLevel::None: bits = kPpatUncached; break;
    case GlkCacheLevel::WriteThrough: bits = kPpatDisplayEllc; break;
    case GlkCacheLevel::Llc:
    case GlkCacheLevel::L3Llc: bits = kPpatCached; break;
    }
    return PteStatus::Ok;
}

PteStatus encodePpgttPte(uint64_t dma, uint32_t patIndex, uint32_t pteFlags, uint64_t &out) {
    PteStatus status = checkPteFlags(pteFlags, Glk::kPpgttReadOnlySupported);
    if (status != PteStatus::Ok) return status;
    uint64_t patBits = 0;
    status = ppgttPatBits(patIndex, patBits);
    if (status != PteStatus::Ok) return status;
    status = checkDmaAddress(dma, Glk::kPageSize4K);
    if (status != PteStatus::Ok) return status;
    uint64_t pte = dma | kEntryPresent | kEntryWritable;
    if (pteFlags & kPteReadOnly) pte &= ~kEntryWritable;
    out = pte | patBits;
    return PteStatus::Ok;
}

PteStatus encodePpgttDirectoryEntry(uint32_t level, uint64_t tableDma, DirectoryCaching caching,
                                    bool ips64K, uint64_t &out) {
    if (level < 1 || level > Glk::kTopLevel) return PteStatus::InvalidLevel;
    if (caching != DirectoryCaching::Cached && caching != DirectoryCaching::Uncached)
        return PteStatus::InvalidArgument;
    // gen8_ppgtt_insert_huge sets IPS only on the level-1 entry that
    // __set_pd_entry wrote (cached form) for a fully 64K-capable page table.
    if (ips64K && (level != 1 || caching != DirectoryCaching::Cached)) return PteStatus::UnsupportedFlag;
    const PteStatus status = checkDmaAddress(tableDma, Glk::kPageSize4K);
    if (status != PteStatus::Ok) return status;
    uint64_t pde = tableDma | kEntryPresent | kEntryWritable;
    pde |= caching == DirectoryCaching::Cached ? kPpatCachedPde : kPpatUncached;
    if (ips64K) pde |= kPdeIps64K;
    out = pde;
    return PteStatus::Ok;
}

PteStatus encodePpgttScratchPte(uint64_t scratchDma, uint64_t &out) {
    // gen8_init_scratch: pte_flags = vm->has_read_only, cache level NONE.
    return encodePpgttPte(scratchDma, uint32_t(GlkCacheLevel::None), kPteReadOnly, out);
}

PteStatus encodePpgttScratchDirectoryEntry(uint32_t level, uint64_t scratchTableDma, uint64_t &out) {
    return encodePpgttDirectoryEntry(level, scratchTableDma, DirectoryCaching::Uncached, false, out);
}

PteStatus encodePpgtt64KPage(uint64_t dma, uint32_t patIndex, uint32_t pteFlags,
                             uint64_t (&out)[Glk::kEntriesPer64KPage]) {
    uint64_t encode = 0;
    PteStatus status = encodePpgttPte(0, patIndex, pteFlags, encode);
    if (status != PteStatus::Ok) return status;
    status = checkDmaAddress(dma, Glk::kPageSize64K);
    if (status != PteStatus::Ok) return status;
    // A 64K-aligned dma below 2^39 keeps all sixteen 4K pages below 2^39.
    for (uint32_t i = 0; i < Glk::kEntriesPer64KPage; ++i)
        out[i] = encode | (dma + uint64_t(i) * Glk::kPageSize4K);
    return PteStatus::Ok;
}

PteStatus check64KPageTable(const uint64_t *entries, size_t count) {
    if (!entries || count != Glk::kEntriesPerTable) return PteStatus::InvalidArgument;
    for (size_t i = 0; i < count; i += Glk::kEntriesPer64KPage) {
        PpgttEntryView view {};
        const PteStatus status = decodePpgttEntry(entries[i], 0, view);
        if (status != PteStatus::Ok) return status;
        if (view.dmaAddress & (Glk::kPageSize64K - 1)) return PteStatus::UnalignedDmaAddress;
    }
    return PteStatus::Ok;
}

PteStatus decodePpgttEntry(uint64_t raw, uint32_t level, PpgttEntryView &out) {
    if (level > Glk::kTopLevel) return PteStatus::InvalidLevel;
    if (!(raw & kEntryPresent)) return PteStatus::NotPresent;
    PpgttEntryView view {};
    view.dmaAddress = raw & kDmaAddressMask;
    view.level = uint8_t(level);
    view.writable = (raw & kEntryWritable) != 0;
    if (level == 0) {
        if (raw & ~kLeafAllowed) return PteStatus::ReservedBitsSet;
        const uint8_t selector = leafSelector(raw);
        // gen8_pte_encode emits only PPAT_UNCACHED, PPAT_DISPLAY_ELLC, PPAT_CACHED.
        if (selector != 2 && selector != 3 && selector != 4) return PteStatus::InvalidPatIndex;
        view.patSelector = selector;
        view.gpuAccessSnooped = chvPatSelectorSnoops(selector);
    } else {
        if (raw & kPdePageSize)
            return level == Glk::kTopLevel ? PteStatus::ReservedBitsSet : PteStatus::UnsupportedPageSize;
        const uint64_t allowed = kDirectoryAllowed | (level == 1 ? kPdeIps64K : 0);
        if (raw & ~allowed) return PteStatus::ReservedBitsSet;
        if (!view.writable) return PteStatus::UnsupportedFlag;
        const uint8_t selector = uint8_t(((raw & kEntryPcd) ? 2 : 0) | ((raw & kEntryPwt) ? 1 : 0));
        if (selector != 0 && selector != 3) return PteStatus::InvalidPatIndex;
        view.ips64K = (raw & kPdeIps64K) != 0;
        if (view.ips64K && selector != 0) return PteStatus::UnsupportedFlag;
        view.patSelector = selector;
        view.gpuAccessSnooped = Glk::kPageWalkSnooped;
    }
    out = view;
    return PteStatus::Ok;
}

PteStatus encodeGgttPte(uint64_t dma, uint32_t pteFlags, uint64_t &out) {
    // gen8_ggtt_insert_entries ignores PTE_READ_ONLY; refuse it rather than
    // silently granting write access through the GGTT.
    PteStatus status = checkPteFlags(pteFlags, false);
    if (status != PteStatus::Ok) return status;
    status = checkDmaAddress(dma, Glk::kPageSize4K);
    if (status != PteStatus::Ok) return status;
    out = dma | kEntryPresent;
    return PteStatus::Ok;
}

PteStatus decodeGgttPte(uint64_t raw, GgttEntryView &out) {
    if (!(raw & kEntryPresent)) return PteStatus::NotPresent;
    if (raw & ~kGgttAllowed) return PteStatus::ReservedBitsSet;
    GgttEntryView view {};
    view.dmaAddress = raw & kDmaAddressMask;
    view.gpuAccessSnooped = chvPatSelectorSnoops(0);
    view.cpuApertureSnooped = Glk::kCpuApertureSnooped;
    out = view;
    return PteStatus::Ok;
}

PteStatus ggttTotalFromGmchControl(uint16_t gmchControl, uint64_t &totalBytes) {
    // gen8_get_total_gtt_size (GLK is not CHV): BDW_GMCH_GGMS_SHIFT 6, MASK 0x3.
    const uint32_t ggms = (gmchControl >> 6) & 0x3;
    if (!ggms) return PteStatus::InvalidGgttSize;
    const uint64_t gsmBytes = (uint64_t(1) << ggms) << 20;
    totalBytes = gsmBytes / Glk::kGgttEntryBytes * Glk::kPageSize4K;
    return PteStatus::Ok;
}

PteStatus checkGgttRange(uint64_t offset, uint64_t length, uint64_t ggttTotal) {
    if (!validGgttTotal(ggttTotal)) return PteStatus::InvalidGgttSize;
    if (!length || (length & (Glk::kPageSize4K - 1))) return PteStatus::InvalidArgument;
    if (offset & (Glk::kPageSize4K - 1)) return PteStatus::UnalignedVirtualAddress;
    if (offset > ~uint64_t(0) - length) return PteStatus::RangeOverflow;
    if (offset + length > ggttTotal) return PteStatus::VirtualAddressOutOfRange;
    return PteStatus::Ok;
}

PteStatus ggttEntryLocation(uint64_t offset, uint64_t ggttTotal, uint64_t &entryIndex,
                            uint64_t &bar0ByteOffset) {
    if (!validGgttTotal(ggttTotal)) return PteStatus::InvalidGgttSize;
    if (offset & (Glk::kPageSize4K - 1)) return PteStatus::UnalignedVirtualAddress;
    if (offset >= ggttTotal) return PteStatus::VirtualAddressOutOfRange;
    // gen8_ggtt_insert_page: gsm + offset / I915_GTT_PAGE_SIZE, 8-byte entries.
    const uint64_t index = offset / Glk::kPageSize4K;
    const uint64_t byteOffset = Glk::kGsmBar0Offset + index * Glk::kGgttEntryBytes;
    if (byteOffset + Glk::kGgttEntryBytes > Glk::kGttMmadrBytes) return PteStatus::VirtualAddressOutOfRange;
    entryIndex = index;
    bar0ByteOffset = byteOffset;
    return PteStatus::Ok;
}

void storeLe64(uint64_t value, uint8_t (&out)[8]) { putLe64(value, out); }

uint64_t loadLe64(const uint8_t (&in)[8]) { return getLe64(in); }

PteStatus serializeEntriesLe(const uint64_t *entries, size_t count, uint8_t *out, size_t outBytes) {
    if (!entries || !out || !count || count > ~size_t(0) / 8 || outBytes != count * 8)
        return PteStatus::InvalidArgument;
    for (size_t i = 0; i < count; ++i) putLe64(entries[i], out + i * 8);
    return PteStatus::Ok;
}

PteStatus deserializeEntriesLe(const uint8_t *in, size_t inBytes, uint64_t *entries, size_t count) {
    if (!in || !entries || !count || count > ~size_t(0) / 8 || inBytes != count * 8)
        return PteStatus::InvalidArgument;
    for (size_t i = 0; i < count; ++i) entries[i] = getLe64(in + i * 8);
    return PteStatus::Ok;
}

} } // namespace Mellow::NativeGlk

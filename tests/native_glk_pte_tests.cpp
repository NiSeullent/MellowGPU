// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
//
// Hosted regression for Drivers/NativeGlk/PageTableEncoding. Pure value tests:
// no GPU, MMIO, DMA or kernel is touched, so a pass is NOT hardware evidence.
// With MELLOW_GLK_PTE_ORACLE the runner (Tools/run-native-glk-pte-tests.py)
// provides native_glk_pte_pinned_oracle.h, generated at test time from the
// SHA-256-pinned Linux sources; production outputs are compared against the
// unmodified Linux encoders compiled from those files.
#include "../Drivers/NativeGlk/PageTableEncoding.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

using namespace Mellow::NativeGlk;

namespace {
unsigned checks = 0;
unsigned failures = 0;
unsigned oracleVectors = 0;

void expect(bool condition, const char *what, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        if (failures <= 40) fprintf(stderr, "FAIL line %d: %s\n", line, what);
    }
}
#define EXPECT(condition) expect((condition), #condition, __LINE__)
#define EXPECT_STATUS(expression, status) expect((expression) == (status), #expression " == " #status, __LINE__)

uint64_t splitmix(uint64_t &state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

constexpr uint64_t kSentinel = 0xa5a5a5a5a5a5a5a5ull;
constexpr uint64_t kTopDma = Glk::kDmaAddressLimit - Glk::kPageSize4K; // 0x7ffffff000

void testWidthsAreDistinct() {
    EXPECT(Glk::kDmaAddressLimit == 0x8000000000ull);
    EXPECT(Glk::kPpgttVirtualAddressLimit == 0x1000000000000ull);
    EXPECT(kDmaAddressMask == 0x7ffffff000ull);
    // A VA that is valid for the 48-bit PPGTT is rejected as a 39-bit DMA address.
    uint32_t index = 0;
    EXPECT_STATUS(ppgttIndex(uint64_t(1) << 39, 3, index), PteStatus::Ok);
    EXPECT(index == 1);
    EXPECT_STATUS(checkDmaAddress(uint64_t(1) << 39, Glk::kPageSize4K), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(checkDmaAddress(kTopDma, Glk::kPageSize4K), PteStatus::Ok);
    EXPECT_STATUS(checkDmaAddress(0x12345001, Glk::kPageSize4K), PteStatus::UnalignedDmaAddress);
    EXPECT_STATUS(checkDmaAddress(0x11000, Glk::kPageSize64K), PteStatus::UnalignedDmaAddress);
    EXPECT_STATUS(checkDmaAddress(0x10000, Glk::kPageSize64K), PteStatus::Ok);
    EXPECT_STATUS(checkDmaAddress(0x10000, 3), PteStatus::InvalidArgument);
    EXPECT_STATUS(checkDmaAddress(0, Glk::kPageSize2M), PteStatus::UnsupportedPageSize);
}

void testPpgttPteFixtures() {
    // Values computed by hand from intel_gtt.h: P=0x1 RW=0x2 PWT=0x8 PCD=0x10 PAT=0x80.
    uint64_t out = kSentinel;
    EXPECT_STATUS(encodePpgttPte(0x12345000, 0, 0, out), PteStatus::Ok);
    EXPECT(out == 0x1234501bull);   // I915_CACHE_NONE -> PPAT_UNCACHED (selector 3)
    EXPECT_STATUS(encodePpgttPte(0x12345000, 1, 0, out), PteStatus::Ok);
    EXPECT(out == 0x12345083ull);   // I915_CACHE_LLC -> PPAT_CACHED (selector 4)
    EXPECT_STATUS(encodePpgttPte(0x12345000, 2, 0, out), PteStatus::Ok);
    EXPECT(out == 0x12345083ull);   // I915_CACHE_L3_LLC -> PPAT_CACHED
    EXPECT_STATUS(encodePpgttPte(0x12345000, 3, 0, out), PteStatus::Ok);
    EXPECT(out == 0x12345013ull);   // I915_CACHE_WT -> PPAT_DISPLAY_ELLC (selector 2)
    EXPECT_STATUS(encodePpgttPte(0x12345000, 0, kPteReadOnly, out), PteStatus::Ok);
    EXPECT(out == 0x12345019ull);
    EXPECT_STATUS(encodePpgttPte(kTopDma, 1, kPteReadOnly, out), PteStatus::Ok);
    EXPECT(out == 0x7ffffff081ull);
    EXPECT_STATUS(encodePpgttPte(0, 1, 0, out), PteStatus::Ok);
    EXPECT(out == 0x83ull);

    uint64_t untouched = kSentinel;
    EXPECT_STATUS(encodePpgttPte(uint64_t(1) << 39, 1, 0, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodePpgttPte(uint64_t(1) << 47, 1, 0, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodePpgttPte(~uint64_t(0) & ~uint64_t(0xfff), 1, 0, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodePpgttPte(0x12345800, 1, 0, untouched), PteStatus::UnalignedDmaAddress);
    EXPECT_STATUS(encodePpgttPte(0x12345000, 4, 0, untouched), PteStatus::InvalidPatIndex);
    EXPECT_STATUS(encodePpgttPte(0x12345000, 0xffffffffu, 0, untouched), PteStatus::InvalidPatIndex);
    EXPECT_STATUS(encodePpgttPte(0x12345000, 1, kPteLocalMemory, untouched), PteStatus::UnsupportedFlag);
    EXPECT_STATUS(encodePpgttPte(0x12345000, 1, 1u << 2, untouched), PteStatus::InvalidArgument);
    EXPECT(untouched == kSentinel);

    uint64_t bits = kSentinel;
    EXPECT_STATUS(ppgttPatBits(0, bits), PteStatus::Ok); EXPECT(bits == 0x18);
    EXPECT_STATUS(ppgttPatBits(1, bits), PteStatus::Ok); EXPECT(bits == 0x80);
    EXPECT_STATUS(ppgttPatBits(2, bits), PteStatus::Ok); EXPECT(bits == 0x80);
    EXPECT_STATUS(ppgttPatBits(3, bits), PteStatus::Ok); EXPECT(bits == 0x10);
    bits = kSentinel;
    EXPECT_STATUS(ppgttPatBits(4, bits), PteStatus::InvalidPatIndex); EXPECT(bits == kSentinel);
}

void testDirectoryFixtures() {
    uint64_t out = kSentinel;
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, 0xabcde000, DirectoryCaching::Cached, false, out), PteStatus::Ok);
    EXPECT(out == 0xabcde003ull);   // PPAT_CACHED_PDE == 0
    EXPECT_STATUS(encodePpgttDirectoryEntry(2, 0xabcde000, DirectoryCaching::Uncached, false, out), PteStatus::Ok);
    EXPECT(out == 0xabcde01bull);
    EXPECT_STATUS(encodePpgttDirectoryEntry(3, kTopDma, DirectoryCaching::Cached, false, out), PteStatus::Ok);
    EXPECT(out == 0x7ffffff003ull);
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, 0xabcde000, DirectoryCaching::Cached, true, out), PteStatus::Ok);
    EXPECT(out == 0xabcde803ull);   // GEN8_PDE_IPS_64K
    EXPECT_STATUS(encodePpgttScratchPte(0x5000, out), PteStatus::Ok);
    EXPECT(out == 0x5019ull);       // read-only, uncached scratch PTE
    EXPECT_STATUS(encodePpgttScratchDirectoryEntry(2, 0x6000, out), PteStatus::Ok);
    EXPECT(out == 0x601bull);

    uint64_t untouched = kSentinel;
    EXPECT_STATUS(encodePpgttDirectoryEntry(0, 0x1000, DirectoryCaching::Cached, false, untouched), PteStatus::InvalidLevel);
    EXPECT_STATUS(encodePpgttDirectoryEntry(4, 0x1000, DirectoryCaching::Cached, false, untouched), PteStatus::InvalidLevel);
    EXPECT_STATUS(encodePpgttDirectoryEntry(2, 0x1000, DirectoryCaching::Cached, true, untouched), PteStatus::UnsupportedFlag);
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, 0x1000, DirectoryCaching::Uncached, true, untouched), PteStatus::UnsupportedFlag);
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, 0x1001, DirectoryCaching::Cached, false, untouched), PteStatus::UnalignedDmaAddress);
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, uint64_t(1) << 39, DirectoryCaching::Cached, false, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodePpgttDirectoryEntry(1, 0x1000, static_cast<DirectoryCaching>(7), false, untouched), PteStatus::InvalidArgument);
    EXPECT_STATUS(encodePpgttScratchDirectoryEntry(0, 0x1000, untouched), PteStatus::InvalidLevel);
    EXPECT_STATUS(encodePpgttScratchPte(0x1080, untouched), PteStatus::UnalignedDmaAddress);
    EXPECT(untouched == kSentinel);
}

void testDecodeFixtures() {
    PpgttEntryView view {};
    EXPECT_STATUS(decodePpgttEntry(0x7ffffff083ull, 0, view), PteStatus::Ok);
    EXPECT(view.dmaAddress == kTopDma && view.writable && view.patSelector == 4 && view.gpuAccessSnooped && !view.ips64K);
    EXPECT_STATUS(decodePpgttEntry(0x1234501bull, 0, view), PteStatus::Ok);
    EXPECT(view.patSelector == 3 && !view.gpuAccessSnooped && view.writable);
    EXPECT_STATUS(decodePpgttEntry(0x12345011ull, 0, view), PteStatus::Ok);
    EXPECT(view.patSelector == 2 && !view.gpuAccessSnooped && !view.writable);
    EXPECT_STATUS(decodePpgttEntry(0xabcde803ull, 1, view), PteStatus::Ok);
    EXPECT(view.ips64K && view.patSelector == 0 && !view.gpuAccessSnooped && view.level == 1);
    EXPECT_STATUS(decodePpgttEntry(0xabcde01bull, 3, view), PteStatus::Ok);
    EXPECT(view.patSelector == 3 && !view.ips64K && view.level == 3);

    PpgttEntryView untouched {};
    untouched.dmaAddress = kSentinel;
    EXPECT_STATUS(decodePpgttEntry(0, 0, untouched), PteStatus::NotPresent);
    EXPECT_STATUS(decodePpgttEntry(0x12345082ull, 0, untouched), PteStatus::NotPresent);
    EXPECT_STATUS(decodePpgttEntry(0x12345003ull, 0, untouched), PteStatus::InvalidPatIndex); // selector 0
    EXPECT_STATUS(decodePpgttEntry(0x1234508bull, 0, untouched), PteStatus::InvalidPatIndex); // selector 5
    EXPECT_STATUS(decodePpgttEntry(0x12345087ull, 0, untouched), PteStatus::ReservedBitsSet); // U/S bit 2
    EXPECT_STATUS(decodePpgttEntry(0x12345883ull, 0, untouched), PteStatus::ReservedBitsSet); // gen12 PTE_LM bit 11
    EXPECT_STATUS(decodePpgttEntry((uint64_t(1) << 62) | 0x12345083ull, 0, untouched), PteStatus::ReservedBitsSet); // MTL PAT3
    EXPECT_STATUS(decodePpgttEntry((uint64_t(1) << 39) | 0x83ull, 0, untouched), PteStatus::ReservedBitsSet); // beyond DMA39
    EXPECT_STATUS(decodePpgttEntry((uint64_t(1) << 63) | 0x83ull, 0, untouched), PteStatus::ReservedBitsSet);
    EXPECT_STATUS(decodePpgttEntry(0xabcde083ull, 1, untouched), PteStatus::UnsupportedPageSize); // 2M PS
    EXPECT_STATUS(decodePpgttEntry(0xabcde083ull, 2, untouched), PteStatus::UnsupportedPageSize); // 1G PS
    EXPECT_STATUS(decodePpgttEntry(0xabcde083ull, 3, untouched), PteStatus::ReservedBitsSet);
    EXPECT_STATUS(decodePpgttEntry(0xabcde801ull | 0x2ull, 2, untouched), PteStatus::ReservedBitsSet); // IPS above level 1
    EXPECT_STATUS(decodePpgttEntry(0xabcde001ull, 1, untouched), PteStatus::UnsupportedFlag); // read-only directory
    EXPECT_STATUS(decodePpgttEntry(0xabcde013ull, 1, untouched), PteStatus::InvalidPatIndex); // PCD only
    EXPECT_STATUS(decodePpgttEntry(0xabcde81bull, 1, untouched), PteStatus::UnsupportedFlag); // IPS + uncached
    EXPECT_STATUS(decodePpgttEntry(0x83ull, 4, untouched), PteStatus::InvalidLevel);
    EXPECT(untouched.dmaAddress == kSentinel);
}

void testPatSemantics() {
    EXPECT(chvPrivatePatValue() == 0x4040404000000040ull);
    EXPECT(uint32_t(chvPrivatePatValue()) == 0x00000040u);
    EXPECT(uint32_t(chvPrivatePatValue() >> 32) == 0x40404040u);
    EXPECT(Glk::kPrivatePatLoRegister == 0x40e0 && Glk::kPrivatePatHiRegister == 0x40e4);
    const bool snoop[8] = {true, false, false, false, true, true, true, true};
    for (uint8_t selector = 0; selector < 8; ++selector)
        EXPECT(chvPatSelectorSnoops(selector) == snoop[selector]);
    EXPECT(!chvPatSelectorSnoops(8));
    EXPECT(!Glk::kHasLlc && Glk::kHasSnoop && !Glk::kHasCoherentGgtt);
    EXPECT(!Glk::kPageWalkSnooped && !Glk::kCpuApertureSnooped);
    EXPECT(Glk::kGsmMappingMustBeUncached && !Glk::kGgttInvalidateWritesGfxFlush);
    EXPECT(!Glk::kGgttUpdateNeedsVtdSerialization);
}

void testGgttFixtures() {
    uint64_t out = kSentinel;
    EXPECT_STATUS(encodeGgttPte(kTopDma, 0, out), PteStatus::Ok);
    EXPECT(out == 0x7ffffff001ull);
    EXPECT_STATUS(encodeGgttPte(0, 0, out), PteStatus::Ok);
    EXPECT(out == 0x1ull);
    uint64_t untouched = kSentinel;
    EXPECT_STATUS(encodeGgttPte(0x1000, kPteReadOnly, untouched), PteStatus::UnsupportedFlag);
    EXPECT_STATUS(encodeGgttPte(0x1000, kPteLocalMemory, untouched), PteStatus::UnsupportedFlag);
    EXPECT_STATUS(encodeGgttPte(0x1000, 1u << 5, untouched), PteStatus::InvalidArgument);
    EXPECT_STATUS(encodeGgttPte(uint64_t(1) << 39, 0, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodeGgttPte(0x1004, 0, untouched), PteStatus::UnalignedDmaAddress);
    EXPECT(untouched == kSentinel);

    GgttEntryView view {};
    EXPECT_STATUS(decodeGgttPte(0x7ffffff001ull, view), PteStatus::Ok);
    EXPECT(view.dmaAddress == kTopDma && view.gpuAccessSnooped && !view.cpuApertureSnooped);
    GgttEntryView unchanged {};
    unchanged.dmaAddress = kSentinel;
    EXPECT_STATUS(decodeGgttPte(0x7ffffff000ull, unchanged), PteStatus::NotPresent);
    EXPECT_STATUS(decodeGgttPte(0x1003ull, unchanged), PteStatus::ReservedBitsSet);   // GEN12_GGTT_PTE_LM
    EXPECT_STATUS(decodeGgttPte((uint64_t(1) << 52) | 0x1001ull, unchanged), PteStatus::ReservedBitsSet); // MTL PAT0
    EXPECT_STATUS(decodeGgttPte((uint64_t(1) << 53) | 0x1001ull, unchanged), PteStatus::ReservedBitsSet); // MTL PAT1
    EXPECT_STATUS(decodeGgttPte((uint64_t(1) << 40) | 0x1001ull, unchanged), PteStatus::ReservedBitsSet);
    EXPECT_STATUS(decodeGgttPte(0x1019ull, unchanged), PteStatus::ReservedBitsSet);   // PPGTT PAT bits
    EXPECT(unchanged.dmaAddress == kSentinel);

    uint64_t total = kSentinel;
    EXPECT_STATUS(ggttTotalFromGmchControl(3u << 6, total), PteStatus::Ok);
    EXPECT(total == (uint64_t(4) << 30));
    EXPECT_STATUS(ggttTotalFromGmchControl(2u << 6, total), PteStatus::Ok);
    EXPECT(total == (uint64_t(2) << 30));
    EXPECT_STATUS(ggttTotalFromGmchControl(1u << 6, total), PteStatus::Ok);
    EXPECT(total == (uint64_t(1) << 30));
    EXPECT_STATUS(ggttTotalFromGmchControl(0xffff, total), PteStatus::Ok);
    EXPECT(total == (uint64_t(4) << 30));
    total = kSentinel;
    EXPECT_STATUS(ggttTotalFromGmchControl(0xff3f, total), PteStatus::InvalidGgttSize);
    EXPECT(total == kSentinel);

    const uint64_t four = uint64_t(4) << 30;
    uint64_t index = kSentinel, offset = kSentinel;
    EXPECT_STATUS(ggttEntryLocation(0x1000, four, index, offset), PteStatus::Ok);
    EXPECT(index == 1 && offset == 0x800008ull);
    EXPECT_STATUS(ggttEntryLocation(four - 0x1000, four, index, offset), PteStatus::Ok);
    EXPECT(index == 0xfffff && offset == 0xfffff8ull && offset + 8 <= Glk::kGttMmadrBytes);
    index = offset = kSentinel;
    EXPECT_STATUS(ggttEntryLocation(four, four, index, offset), PteStatus::VirtualAddressOutOfRange);
    EXPECT_STATUS(ggttEntryLocation(0x1800, four, index, offset), PteStatus::UnalignedVirtualAddress);
    EXPECT_STATUS(ggttEntryLocation(0, four + 0x1000, index, offset), PteStatus::InvalidGgttSize);
    EXPECT_STATUS(ggttEntryLocation(0, 3u << 20, index, offset), PteStatus::InvalidGgttSize);
    EXPECT(index == kSentinel && offset == kSentinel);

    EXPECT_STATUS(checkGgttRange(0, four, four), PteStatus::Ok);
    EXPECT_STATUS(checkGgttRange(0x1000, four, four), PteStatus::VirtualAddressOutOfRange);
    EXPECT_STATUS(checkGgttRange(0x1000, 0, four), PteStatus::InvalidArgument);
    EXPECT_STATUS(checkGgttRange(0x1000, 0x1800, four), PteStatus::InvalidArgument);
    EXPECT_STATUS(checkGgttRange(~uint64_t(0xfff), 0x2000, four), PteStatus::RangeOverflow);
}

void testPpgttAddressing() {
    uint32_t index = 0xffffffffu;
    const uint64_t top = Glk::kPpgttVirtualAddressLimit - Glk::kPageSize4K;
    for (uint32_t level = 0; level <= 2; ++level) {
        EXPECT_STATUS(ppgttIndex(top, level, index), PteStatus::Ok);
        EXPECT(index == 511);
    }
    EXPECT_STATUS(ppgttIndex(top, 3, index), PteStatus::Ok);
    EXPECT(index == 511);
    EXPECT_STATUS(ppgttIndex(0x0000123456789000ull, 0, index), PteStatus::Ok);
    EXPECT(index == 0x189);
    EXPECT_STATUS(ppgttIndex(0x0000123456789000ull, 1, index), PteStatus::Ok);
    EXPECT(index == 0xb3);
    EXPECT_STATUS(ppgttIndex(0x0000123456789000ull, 2, index), PteStatus::Ok);
    EXPECT(index == 0xd1);
    EXPECT_STATUS(ppgttIndex(0x0000123456789000ull, 3, index), PteStatus::Ok);
    EXPECT(index == 0x24);
    index = 77;
    EXPECT_STATUS(ppgttIndex(Glk::kPpgttVirtualAddressLimit, 0, index), PteStatus::VirtualAddressOutOfRange);
    EXPECT_STATUS(ppgttIndex(0xffff800000000000ull, 3, index), PteStatus::VirtualAddressOutOfRange); // canonical high half
    EXPECT_STATUS(ppgttIndex(0x1000, 4, index), PteStatus::InvalidLevel);
    EXPECT(index == 77);

    EXPECT_STATUS(checkPpgttRange(0, Glk::kPpgttVirtualAddressLimit, Glk::kPageSize4K), PteStatus::Ok);
    EXPECT_STATUS(checkPpgttRange(0x1000, Glk::kPpgttVirtualAddressLimit, Glk::kPageSize4K), PteStatus::VirtualAddressOutOfRange);
    EXPECT_STATUS(checkPpgttRange(0xfffffffffffff000ull, 0x2000, Glk::kPageSize4K), PteStatus::RangeOverflow);
    EXPECT_STATUS(checkPpgttRange(0x1000, 0, Glk::kPageSize4K), PteStatus::InvalidArgument);
    EXPECT_STATUS(checkPpgttRange(0x1000, 0x10000, Glk::kPageSize64K), PteStatus::UnalignedVirtualAddress);
    EXPECT_STATUS(checkPpgttRange(0x10000, 0x1000, Glk::kPageSize64K), PteStatus::InvalidArgument);
    EXPECT_STATUS(checkPpgttRange(0x10000, 0x20000, Glk::kPageSize64K), PteStatus::Ok);
    EXPECT_STATUS(checkPpgttRange(0, Glk::kPageSize2M, Glk::kPageSize2M), PteStatus::UnsupportedPageSize);
    EXPECT_STATUS(checkPpgttRange(0, uint64_t(1) << 30, uint64_t(1) << 30), PteStatus::UnsupportedPageSize);
}

void test64KPages() {
    uint64_t group[Glk::kEntriesPer64KPage];
    for (auto &entry : group) entry = kSentinel;
    EXPECT_STATUS(encodePpgtt64KPage(0x10000, 1, 0, group), PteStatus::Ok);
    bool sequential = true;
    for (uint32_t i = 0; i < Glk::kEntriesPer64KPage; ++i)
        sequential = sequential && group[i] == ((0x10000ull + i * 0x1000ull) | 0x83ull);
    EXPECT(sequential);
    uint64_t untouched[Glk::kEntriesPer64KPage];
    for (auto &entry : untouched) entry = kSentinel;
    EXPECT_STATUS(encodePpgtt64KPage(0x11000, 1, 0, untouched), PteStatus::UnalignedDmaAddress);
    EXPECT_STATUS(encodePpgtt64KPage(Glk::kDmaAddressLimit, 1, 0, untouched), PteStatus::DmaAddressOutOfRange);
    EXPECT_STATUS(encodePpgtt64KPage(0x10000, 9, 0, untouched), PteStatus::InvalidPatIndex);
    EXPECT_STATUS(encodePpgtt64KPage(0x10000, 1, kPteLocalMemory, untouched), PteStatus::UnsupportedFlag);
    EXPECT(untouched[0] == kSentinel && untouched[15] == kSentinel);
    EXPECT_STATUS(encodePpgtt64KPage(Glk::kDmaAddressLimit - Glk::kPageSize64K, 3, kPteReadOnly, group), PteStatus::Ok);
    EXPECT(group[15] == (kTopDma | 0x11ull));

    static uint64_t table[Glk::kEntriesPerTable];
    for (uint32_t g = 0; g < Glk::kEntriesPerTable / Glk::kEntriesPer64KPage; ++g) {
        uint64_t page[Glk::kEntriesPer64KPage];
        EXPECT_STATUS(encodePpgtt64KPage(0x40000000ull + g * Glk::kPageSize64K, 1, 0, page), PteStatus::Ok);
        for (uint32_t i = 0; i < Glk::kEntriesPer64KPage; ++i) table[g * Glk::kEntriesPer64KPage + i] = page[i];
    }
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::Ok);
    uint64_t scratch = 0;
    EXPECT_STATUS(encodePpgttScratchPte(0x7000000000ull, scratch), PteStatus::Ok);  // 64K-aligned scratch
    for (uint32_t i = 256; i < Glk::kEntriesPerTable; ++i) table[i] = scratch;
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::Ok);
    const uint64_t saved = table[32];
    table[32] = 0;
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::NotPresent);
    table[32] = saved + 0x1000;
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::UnalignedDmaAddress);
    table[32] = saved | (uint64_t(1) << 62);
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::ReservedBitsSet);
    table[32] = saved;
    table[33] = 0;   // a non-head entry is ignored by hardware in IPS mode
    EXPECT_STATUS(check64KPageTable(table, Glk::kEntriesPerTable), PteStatus::Ok);
    EXPECT_STATUS(check64KPageTable(table, 511), PteStatus::InvalidArgument);
    EXPECT_STATUS(check64KPageTable(nullptr, Glk::kEntriesPerTable), PteStatus::InvalidArgument);
}

void testSerialization() {
    uint8_t bytes[8] = {};
    storeLe64(0x0102030405060708ull, bytes);
    const uint8_t expected[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    EXPECT(memcmp(bytes, expected, 8) == 0);
    EXPECT(loadLe64(bytes) == 0x0102030405060708ull);

    uint64_t entries[3] = {0x7ffffff083ull, 0xabcde803ull, 0x1ull};
    uint8_t image[24];
    memset(image, 0xcc, sizeof(image));
    EXPECT_STATUS(serializeEntriesLe(entries, 3, image, sizeof(image)), PteStatus::Ok);
    const uint8_t first[8] = {0x83, 0xf0, 0xff, 0xff, 0x7f, 0, 0, 0};
    EXPECT(memcmp(image, first, 8) == 0);
    EXPECT(image[8] == 0x03 && image[9] == 0xe8 && image[10] == 0xcd && image[11] == 0xab);
    EXPECT(image[16] == 0x01 && image[23] == 0x00);
    uint64_t back[3] = {};
    EXPECT_STATUS(deserializeEntriesLe(image, sizeof(image), back, 3), PteStatus::Ok);
    EXPECT(back[0] == entries[0] && back[1] == entries[1] && back[2] == entries[2]);

    uint8_t small[23];
    memset(small, 0xcc, sizeof(small));
    EXPECT_STATUS(serializeEntriesLe(entries, 3, small, sizeof(small)), PteStatus::InvalidArgument);
    EXPECT(small[0] == 0xcc);
    EXPECT_STATUS(serializeEntriesLe(nullptr, 3, image, sizeof(image)), PteStatus::InvalidArgument);
    EXPECT_STATUS(serializeEntriesLe(entries, 0, image, sizeof(image)), PteStatus::InvalidArgument);
    EXPECT_STATUS(serializeEntriesLe(entries, SIZE_MAX / 4, image, sizeof(image)), PteStatus::InvalidArgument);
    uint64_t keep[3] = {kSentinel, kSentinel, kSentinel};
    EXPECT_STATUS(deserializeEntriesLe(image, 16, keep, 3), PteStatus::InvalidArgument);
    EXPECT_STATUS(deserializeEntriesLe(nullptr, 24, keep, 3), PteStatus::InvalidArgument);
    EXPECT(keep[0] == kSentinel);
}

void testRoundTrips() {
    uint64_t state = 0x6c6b2d707465ull;
    for (int i = 0; i < 4096; ++i) {
        const uint64_t dma = splitmix(state) & kDmaAddressMask;
        const uint32_t pat = uint32_t(splitmix(state) & 3);
        const uint32_t flags = (splitmix(state) & 1) ? kPteReadOnly : 0;
        uint64_t pte = 0;
        PpgttEntryView view {};
        EXPECT_STATUS(encodePpgttPte(dma, pat, flags, pte), PteStatus::Ok);
        EXPECT_STATUS(decodePpgttEntry(pte, 0, view), PteStatus::Ok);
        EXPECT(view.dmaAddress == dma && view.writable == !flags &&
               view.gpuAccessSnooped == (pat == 1 || pat == 2));
        const uint32_t level = 1 + uint32_t(splitmix(state) % 3);
        uint64_t pde = 0;
        EXPECT_STATUS(encodePpgttDirectoryEntry(level, dma, DirectoryCaching::Cached, level == 1, pde), PteStatus::Ok);
        EXPECT_STATUS(decodePpgttEntry(pde, level, view), PteStatus::Ok);
        EXPECT(view.dmaAddress == dma && view.ips64K == (level == 1) && !view.gpuAccessSnooped);
        uint64_t gte = 0;
        GgttEntryView ggtt {};
        EXPECT_STATUS(encodeGgttPte(dma, 0, gte), PteStatus::Ok);
        EXPECT_STATUS(decodeGgttPte(gte, ggtt), PteStatus::Ok);
        EXPECT(ggtt.dmaAddress == dma);
    }
}

#ifdef MELLOW_GLK_PTE_ORACLE
} // namespace
#include "native_glk_pte_pinned_oracle.h"
namespace {

void testAgainstPinnedLinux() {
    // Device facts parsed from the pinned i915_pci.c / gen8_ppgtt.c / intel_ggtt.c.
    EXPECT(Glk::kDmaAddressBits == ORACLE_GLK_DMA_MASK_SIZE);
    EXPECT(Glk::kPpgttVirtualAddressBits == ORACLE_GLK_PPGTT_SIZE);
    EXPECT(Glk::kMaxPatIndex == ORACLE_GLK_MAX_PAT_INDEX);
    EXPECT(Glk::kSupportedPageSizes == uint64_t(ORACLE_GLK_PAGE_SIZES));
    EXPECT(Glk::kHasSnoop == bool(ORACLE_GLK_HAS_SNOOP));
    EXPECT(Glk::kHasCoherentGgtt == bool(ORACLE_GLK_HAS_COHERENT_GGTT));
    EXPECT(Glk::kHasLlc == bool(ORACLE_GLK_HAS_LLC));
    EXPECT(Glk::kPpgttReadOnlySupported == bool(ORACLE_GLK_PPGTT_HAS_READ_ONLY));
    EXPECT(Glk::kTopLevel == ORACLE_GLK_PPGTT_TOP);
    EXPECT(Glk::kEntriesPerTable == I915_PDES && Glk::kEntriesPerTable == GEN8_PDES);
    EXPECT(Glk::kPageShift == GEN8_PTE_SHIFT);
    EXPECT(Glk::kPageSize4K == I915_GTT_PAGE_SIZE_4K && Glk::kPageSize64K == I915_GTT_PAGE_SIZE_64K &&
           Glk::kPageSize2M == I915_GTT_PAGE_SIZE_2M);
    EXPECT(kPteReadOnly == PTE_READ_ONLY && kPteLocalMemory == PTE_LM);
    EXPECT(kEntryPresent == GEN8_PAGE_PRESENT && kEntryWritable == GEN8_PAGE_RW);
    EXPECT(kEntryPwt == _PAGE_PWT && kEntryPcd == _PAGE_PCD && kPtePat == _PAGE_PAT);
    EXPECT(kPdePageSize == GEN8_PDE_PS_2M && kPdeIps64K == GEN8_PDE_IPS_64K);
    EXPECT(Glk::kChvPpatSnoop == CHV_PPAT_SNOOP);
    EXPECT(Glk::kPrivatePatLoRegister == GEN8_PRIVATE_PAT_LO.reg && Glk::kPrivatePatHiRegister == GEN8_PRIVATE_PAT_HI.reg);
    EXPECT(Glk::kGsmBar0Offset == gen6_gttadr_offset(&oracle_glk_i915));
    EXPECT(Glk::kGttMmadrBytes == gen6_gttmmadr_size(&oracle_glk_i915));
    EXPECT(Glk::kGsmMappingMustBeUncached == !needs_wc_ggtt_mapping(&oracle_glk_i915));
    EXPECT(Glk::kGgttInvalidateWritesGfxFlush == needs_wc_ggtt_mapping(&oracle_glk_i915));
    EXPECT(Glk::kGgttUpdateNeedsVtdSerialization == intel_vm_no_concurrent_access_wa(&oracle_glk_i915));

    // GEN8_PRIVATE_PAT programming selected by setup_private_pat for GEN9_LP.
    struct intel_uncore uncore {};
    struct intel_gt gt {};
    gt.uncore = &uncore;
    gt.i915 = &oracle_glk_i915;
    gt.type = GT_PRIMARY;
    setup_private_pat(&gt);
    EXPECT(oracle_selected_ppat == ORACLE_PPAT_CHV);
    EXPECT(uncore.count == 2 && uncore.offsets[0] == Glk::kPrivatePatLoRegister &&
           uncore.offsets[1] == Glk::kPrivatePatHiRegister);
    const uint64_t pat = (uint64_t(uncore.values[1]) << 32) | uncore.values[0];
    EXPECT(pat == chvPrivatePatValue());
    for (uint8_t selector = 0; selector < 8; ++selector)
        EXPECT(chvPatSelectorSnoops(selector) == (((pat >> (selector * 8)) & CHV_PPAT_SNOOP) != 0));

    uint64_t state = 0x676c6b3a6f72636cull;
    const uint64_t edges[] = {0, Glk::kPageSize4K, Glk::kPageSize64K, 0xfffff000ull, kTopDma};
    for (int i = 0; i < 20000; ++i) {
        const uint64_t dma = i < 5 ? edges[i] : (splitmix(state) & kDmaAddressMask);
        for (uint32_t patIndex = 0; patIndex <= Glk::kMaxPatIndex; ++patIndex) {
            for (uint32_t flags = 0; flags <= kPteReadOnly; ++flags) {
                uint64_t ours = 0;
                EXPECT_STATUS(encodePpgttPte(dma, patIndex, flags, ours), PteStatus::Ok);
                EXPECT(ours == gen8_pte_encode(dma, patIndex, flags));
                ++oracleVectors;
            }
            uint64_t ggtt = 0;
            EXPECT_STATUS(encodeGgttPte(dma, 0, ggtt), PteStatus::Ok);
            EXPECT(ggtt == gen8_ggtt_pte_encode(dma, patIndex, 0));
            bool present = false, local = true;
            GgttEntryView gview {};
            EXPECT_STATUS(decodeGgttPte(ggtt, gview), PteStatus::Ok);
            EXPECT(gview.dmaAddress == gen8_ggtt_pte_decode(ggtt, &present, &local) && present && !local);
            ++oracleVectors;
        }
        for (uint32_t level = 1; level <= Glk::kTopLevel; ++level) {
            uint64_t cached = 0, uncached = 0;
            EXPECT_STATUS(encodePpgttDirectoryEntry(level, dma, DirectoryCaching::Cached, false, cached), PteStatus::Ok);
            EXPECT_STATUS(encodePpgttDirectoryEntry(level, dma, DirectoryCaching::Uncached, false, uncached), PteStatus::Ok);
            EXPECT(cached == gen8_pde_encode(dma, I915_CACHE_LLC));   // __set_pd_entry
            EXPECT(uncached == gen8_pde_encode(dma, I915_CACHE_NONE)); // gen8_init_scratch
            EXPECT_STATUS(encodePpgttScratchDirectoryEntry(level, dma, uncached), PteStatus::Ok);
            EXPECT(uncached == gen8_pde_encode(dma, I915_CACHE_NONE));
            oracleVectors += 3;
        }
        uint64_t ips = 0, scratch = 0;
        EXPECT_STATUS(encodePpgttDirectoryEntry(1, dma, DirectoryCaching::Cached, true, ips), PteStatus::Ok);
        EXPECT(ips == (gen8_pde_encode(dma, I915_CACHE_LLC) | GEN8_PDE_IPS_64K));
        EXPECT_STATUS(encodePpgttScratchPte(dma, scratch), PteStatus::Ok);
        EXPECT(scratch == gen8_pte_encode(dma, I915_CACHE_NONE, PTE_READ_ONLY)); // has_read_only scratch
        oracleVectors += 2;

        const uint64_t va = splitmix(state) & (Glk::kPpgttVirtualAddressLimit - 1);
        for (uint32_t level = 0; level <= Glk::kTopLevel; ++level) {
            uint32_t index = 0;
            EXPECT_STATUS(ppgttIndex(va, level, index), PteStatus::Ok);
            EXPECT(index == __gen8_pte_index(va, level));
            ++oracleVectors;
        }

        if ((dma & (Glk::kPageSize64K - 1)) == 0 || i % 7 == 0) {
            const uint64_t base = dma & ~(Glk::kPageSize64K - 1);
            const uint32_t patIndex = uint32_t(splitmix(state) & 3);
            const uint32_t flags = uint32_t(splitmix(state) & 1);
            uint64_t page[Glk::kEntriesPer64KPage];
            EXPECT_STATUS(encodePpgtt64KPage(base, patIndex, flags, page), PteStatus::Ok);
            const uint64_t encode = gen8_pte_encode(0, patIndex, flags);   // gen8_ppgtt_insert_huge
            for (uint32_t k = 0; k < Glk::kEntriesPer64KPage; ++k)
                EXPECT(page[k] == (encode | (base + k * I915_GTT_PAGE_SIZE)));
            ++oracleVectors;
        }

        // Linux would encode these out-of-contract inputs; GLK must refuse them
        // and the resulting Linux words must not decode as GLK entries.
        const uint64_t wide = dma | (uint64_t(1) << (39 + splitmix(state) % 25));
        uint64_t refused = kSentinel;
        PpgttEntryView pview {};
        GgttEntryView gview {};
        EXPECT_STATUS(encodePpgttPte(wide, 1, 0, refused), PteStatus::DmaAddressOutOfRange);
        EXPECT(decodePpgttEntry(gen8_pte_encode(wide, 1, 0), 0, pview) == PteStatus::ReservedBitsSet);
        EXPECT_STATUS(encodeGgttPte(wide, 0, refused), PteStatus::DmaAddressOutOfRange);
        EXPECT(decodeGgttPte(gen8_ggtt_pte_encode(wide, 0, 0), gview) == PteStatus::ReservedBitsSet);
        EXPECT_STATUS(encodePpgttPte(dma, 4 + uint32_t(splitmix(state) % 60), 0, refused), PteStatus::InvalidPatIndex);
        EXPECT_STATUS(encodePpgttPte(dma, 1, PTE_LM, refused), PteStatus::UnsupportedFlag);
        EXPECT(refused == kSentinel);
        // Xe-LP/Xe-LPG fields must never be accepted on GLK.
        EXPECT(decodePpgttEntry(gen12_pte_encode(dma, 8 | (uint32_t(i) & 3), 0), 0, pview) == PteStatus::ReservedBitsSet);
        EXPECT(decodePpgttEntry(gen12_pte_encode(dma, 4, PTE_LM), 0, pview) == PteStatus::ReservedBitsSet);
        EXPECT(decodeGgttPte(mtl_ggtt_pte_encode(dma, 1 + uint32_t(i % 3), 0), gview) == PteStatus::ReservedBitsSet);
        EXPECT(decodeGgttPte(gen8_ggtt_pte_encode(dma, 0, PTE_LM), gview) == PteStatus::ReservedBitsSet);
        oracleVectors += 8;
    }

    // gen8_gmch_probe (non-CHV): total = gen8_get_total_gtt_size / 8 * 4K.
    for (uint32_t gmch = 0; gmch <= 0xffff; ++gmch) {
        const unsigned size = gen8_get_total_gtt_size(uint16_t(gmch));
        uint64_t total = kSentinel;
        const PteStatus status = ggttTotalFromGmchControl(uint16_t(gmch), total);
        if (size == 0) {
            EXPECT(status == PteStatus::InvalidGgttSize && total == kSentinel);
        } else {
            EXPECT(status == PteStatus::Ok && total == (uint64_t(size) / sizeof(gen8_pte_t)) * I915_GTT_PAGE_SIZE);
        }
        ++oracleVectors;
    }
}
#endif
} // namespace

int main() {
    testWidthsAreDistinct();
    testPpgttPteFixtures();
    testDirectoryFixtures();
    testDecodeFixtures();
    testPatSemantics();
    testGgttFixtures();
    testPpgttAddressing();
    test64KPages();
    testSerialization();
    testRoundTrips();
#ifdef MELLOW_GLK_PTE_ORACLE
    testAgainstPinnedLinux();
    const bool oracle = true;
#else
    const bool oracle = false;
#endif
    const bool pass = failures == 0 && checks > 0;
    printf("{\"status\":\"%s\",\"checks\":%u,\"failures\":%u,\"pinned_linux_oracle\":%s,"
           "\"oracle_vectors\":%u,\"hardware_execution\":false,\"hosted_value_tests_only\":true}\n",
           pass ? "PASS_NATIVE_GLK_PTE_HOSTED_VALUES" : "FAIL", checks, failures,
           oracle ? "true" : "false", oracleVectors);
    return pass ? 0 : 1;
}

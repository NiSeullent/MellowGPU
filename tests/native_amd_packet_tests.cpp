// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
#include "Drivers/NativeAmd/PacketEncoding.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>

using namespace MellowNativeAmd;
namespace {
unsigned checks = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", description);
        exit(1);
    }
}
constexpr Architecture gfx9 = Architecture::Gfx9;
constexpr Architecture badArchitectures[] = {
    static_cast<Architecture>(0), static_cast<Architecture>(8),
    static_cast<Architecture>(10), static_cast<Architecture>(0xFF)
};

// Oracle: little-endian bytes and single-bit reads, independent of the
// encoder's masks. Positions follow the kfd_pm4_headers_ai.h bitfields.
uint32_t field(uint32_t word, unsigned first, unsigned width) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word % 0x100), static_cast<uint8_t>(word / 0x100 % 0x100),
        static_cast<uint8_t>(word / 0x10000 % 0x100), static_cast<uint8_t>(word / 0x1000000)
    };
    uint64_t result = 0, weight = 1;
    for (unsigned bit = first; bit < first + width; ++bit, weight *= 2)
        if ((bytes[bit / 8] & (1U << (bit % 8))) != 0) result += weight;
    return static_cast<uint32_t>(result);
}
uint32_t headerWord(uint32_t type, uint32_t count, uint32_t opcode, uint32_t low) {
    return type * 0x40000000U + count * 0x10000U + opcode * 0x100U + low;
}

struct HeaderVector { uint32_t opcode, body; bool compute; uint32_t word; };
// Hand-evaluated soc15d.h PACKET3(op, body - 1) and PACKET3_COMPUTE.
constexpr HeaderVector headerVectors[] = {
    {0x10, 0, false, 0xFFFF1000},      // gfx_v9_0.c ring->funcs->nop
    {0x10, 1, false, 0xC0001000},
    {0x10, 0x3FFF, false, 0xFFFE1000}, // gfx_v9_0.c NOP count cap 0x3FFE
    {0x49, 7, false, 0xC0064900},      // PACKET3(PACKET3_RELEASE_MEM, 6)
    {0x49, 7, true, 0xC0064902},
    {0x00, 1, false, 0xC0000000},
    {0xFF, 1, true, 0xC000FF02},
    {0x00, 0x3FFF, false, 0xFFFE0000},
};
void headerVectorsMatch() {
    for (const HeaderVector &vector : headerVectors) {
        uint32_t word = 0;
        check(encodeType3Header(gfx9, vector.opcode, vector.body, vector.compute, word) == Status::Ok &&
              word == vector.word, "type-3 header matches hand-evaluated PACKET3 vector");
        Type3Header header;
        check(decodeType3Header(gfx9, vector.word, header) == Status::Ok &&
              header.opcode == vector.opcode && header.compute == vector.compute &&
              header.bodyDwords == vector.body && header.packetDwords == vector.body + 1,
              "type-3 vector decodes to its fields");
    }
}

void unchangedHeader(Status expected, Architecture architecture, uint32_t opcode,
                     uint32_t body, bool compute) {
    uint32_t word = 0xDEADBEEF;
    check(encodeType3Header(architecture, opcode, body, compute, word) == expected,
          "invalid type-3 header request classified");
    check(word == 0xDEADBEEF, "type-3 header failure preserves output");
}
void headerEncodeLimits() {
    for (Architecture architecture : badArchitectures)
        unchangedHeader(Status::UnsupportedArchitecture, architecture, OpcodeNop, 1, false);
    // No 8-bit opcode truncation, e.g. 0x149 must not alias RELEASE_MEM.
    for (uint32_t opcode : {0x100U, 0x149U, 0xFFFFFFFFU})
        unchangedHeader(Status::InvalidOpcode, gfx9, opcode, 7, false);
    for (uint32_t opcode : {0x00U, 0x49U, 0xFFU})
        unchangedHeader(Status::InvalidCount, gfx9, opcode, 0, false);
    // Only the plain 0xFFFF1000 one-dword NOP is established.
    unchangedHeader(Status::UnsupportedCount, gfx9, OpcodeNop, 0, true);
    // Count 0x3FFF on NOP is the one-dword form, so a 0x4000 body is impossible;
    // for other opcodes the meaning of count 0x3FFF is not established.
    unchangedHeader(Status::InvalidCount, gfx9, OpcodeNop, 0x4000, false);
    unchangedHeader(Status::UnsupportedCount, gfx9, OpcodeReleaseMem, 0x4000, false);
    unchangedHeader(Status::UnsupportedCount, gfx9, 0x00, 0x4000, true);
    // No 14-bit count truncation: 0x4007 and 0x10007 would alias a count of 6.
    for (uint32_t body : {0x4001U, 0x4007U, 0x10007U, 0xFFFFFFFFU}) {
        unchangedHeader(Status::InvalidCount, gfx9, OpcodeNop, body, false);
        unchangedHeader(Status::InvalidCount, gfx9, OpcodeReleaseMem, body, true);
    }
}

void headerDecodeSweep() {
    constexpr uint32_t counts[] = {0, 1, 6, 0x1FFF, 0x3FFE, 0x3FFF};
    unsigned long decoded = 0, mismatches = 0;
    for (uint32_t type = 0; type < 4; ++type)
    for (uint32_t low = 0; low < 0x100; ++low)
    for (uint32_t opcode = 0; opcode < 0x100; ++opcode)
    for (uint32_t count : counts) {
        const uint32_t word = headerWord(type, count, opcode, low);
        Status expected = Status::Ok;
        uint32_t body = count + 1;
        if (type != 3) expected = Status::NotType3;
        else if (low != 0 && low != 2) expected = Status::ReservedBits;
        else if (count == 0x3FFF) {
            if (opcode == 0x10 && low == 0) body = 0;
            else expected = Status::UnsupportedCount;
        }
        Type3Header header {0xAA, true, 0xBB, 0xCC};
        bool ok = decodeType3Header(gfx9, word, header) == expected;
        if (expected == Status::Ok) {
            uint32_t again = 0;
            ok = ok && header.opcode == opcode && header.compute == (low == 2) &&
                 header.bodyDwords == body && header.packetDwords == body + 1 &&
                 encodeType3Header(gfx9, opcode, body, low == 2, again) == Status::Ok &&
                 again == word;
            ++decoded;
        } else {
            ok = ok && header.opcode == 0xAA && header.compute && header.bodyDwords == 0xBB &&
                 header.packetDwords == 0xCC;
        }
        if (!ok && mismatches++ == 0) fprintf(stderr, "header 0x%08X misclassified\n", word);
    }
    check(mismatches == 0, "1572864 swept header words classified, round-tripped, unchanged on failure");
    check(decoded == 2 * 256 * 5 + 1, "exactly the documented type-3 header forms decode");
    for (Architecture architecture : badArchitectures) {
        Type3Header header {0xAA, true, 0xBB, 0xCC};
        check(decodeType3Header(architecture, 0xC0064900, header) == Status::UnsupportedArchitecture &&
              header.opcode == 0xAA && header.packetDwords == 0xCC,
              "uninspected architecture header decode rejected without output mutation");
    }
}

void streamMeasurement() {
    // One-dword NOP, NOP with a two-dword body, then an eight-dword RELEASE_MEM.
    const uint32_t stream[] = {0xFFFF1000, 0xC0011000, 0xFFFF1000, 0xFFFF1000,
                               0xC0064900, 0x00238514, 0x42000000, 0x1000, 0, 1, 0, 0};
    constexpr size_t streamDwords = sizeof(stream) / sizeof(stream[0]);
    constexpr uint32_t expectedSizes[] = {1, 3, 8};
    size_t offset = 0;
    unsigned packets = 0;
    while (offset < streamDwords && packets < 3) {
        Type3Header header;
        check(measureType3Packet(gfx9, stream + offset, streamDwords - offset, header) == Status::Ok &&
              header.packetDwords == expectedSizes[packets], "stream packet measured");
        offset += header.packetDwords;
        ++packets;
    }
    check(offset == streamDwords && packets == 3, "stream walk consumes exactly three packets");

    Type3Header header {7, true, 7, 7};
    check(measureType3Packet(gfx9, stream + 4, 7, header) == Status::InvalidLength &&
          header.opcode == 7 && header.packetDwords == 7, "truncated RELEASE_MEM rejected unchanged");
    check(measureType3Packet(gfx9, stream + 1, 2, header) == Status::InvalidLength,
          "NOP body beyond the buffer rejected");
    check(measureType3Packet(gfx9, nullptr, 8, header) == Status::InvalidLength &&
          measureType3Packet(gfx9, stream, 0, header) == Status::InvalidLength,
          "missing or empty buffer rejected");
    check(measureType3Packet(gfx9, stream + 4, 8, header) == Status::Ok &&
          header.opcode == OpcodeReleaseMem && header.packetDwords == 8, "exact-fit RELEASE_MEM accepted");
    // Only words[0] is read; `available` exercises the length arithmetic.
    const uint32_t largest = 0xFFFE1000; // count 0x3FFE: 0x4000 dwords in total
    check(measureType3Packet(gfx9, &largest, 0x3FFF, header) == Status::InvalidLength,
          "largest body one dword short rejected");
    check(measureType3Packet(gfx9, &largest, 0x4000, header) == Status::Ok &&
          header.packetDwords == 0x4000, "largest representable packet measured");
    check(measureType3Packet(gfx9, stream, SIZE_MAX, header) == Status::Ok && header.packetDwords == 1,
          "maximum available length does not overflow");
    const uint32_t foreign[] = {0x80000000, 0xC0064901, 0xFFFF4900};
    check(measureType3Packet(gfx9, foreign, 3, header) == Status::NotType3 &&
          measureType3Packet(gfx9, foreign + 1, 2, header) == Status::ReservedBits &&
          measureType3Packet(gfx9, foreign + 2, 1, header) == Status::UnsupportedCount,
          "type-2, reserved-bit and ambiguous-count headers rejected in a stream");
    for (Architecture architecture : badArchitectures)
        check(measureType3Packet(architecture, stream, streamDwords, header) ==
              Status::UnsupportedArchitecture, "uninspected architecture stream rejected");
}

ReleaseMemFence makeFence(FenceCache cache, FenceData data, FenceInterrupt interrupt,
                          uint64_t address, uint64_t sequence) {
    return ReleaseMemFence {gfx9, FenceEvent::CacheFlushAndInvTsEvent, cache,
                            FenceDestination::MemoryController, data, interrupt, address, sequence};
}
bool sameFence(const ReleaseMemFence &a, const ReleaseMemFence &b) {
    return a.architecture == b.architecture && a.event == b.event && a.cache == b.cache &&
           a.destination == b.destination && a.data == b.data && a.interrupt == b.interrupt &&
           a.address == b.address && a.sequence == b.sequence;
}
bool samePacket(const ReleaseMemPacket &a, const ReleaseMemPacket &b) {
    for (uint32_t i = 0; i < ReleaseMemDwords; ++i)
        if (a.dwords[i] != b.dwords[i]) return false;
    return true;
}
constexpr FenceCache Default = FenceCache::DefaultActions, WbOnly = FenceCache::WritebackOnly;
constexpr FenceData Low32 = FenceData::Low32, Full64 = FenceData::Full64;
constexpr FenceInterrupt NoInt = FenceInterrupt::None, Int = FenceInterrupt::AfterWriteConfirm;

struct FenceVector {
    FenceCache cache; FenceData data; FenceInterrupt interrupt;
    uint64_t address, sequence; ReleaseMemPacket packet;
};
// gfx_v9_0_ring_emit_fence evaluated by hand from soc15d.h bit numbers:
// dword 1 = event 0x14 | EVENT_INDEX(5) 0x500 | EOP_TC_WB_ACTION_EN 0x8000 plus
// TCL1 0x10000 | TC 0x20000 | TC_MD 0x200000, or TC_NC 0x80000 for TC_WB_ONLY.
// dword 2 = DATA_SEL(1) 0x20000000 or DATA_SEL(2) 0x40000000, | INT_SEL(2) 0x02000000.
constexpr FenceVector fenceVectors[] = {
    {Default, Full64, Int, 0x00007FFFFFFFFFF8ULL, 0x0123456789ABCDEFULL,
     {{0xC0064900, 0x00238514, 0x42000000, 0xFFFFFFF8, 0x00007FFF, 0x89ABCDEF, 0x01234567, 0}}},
    {Default, Full64, NoInt, 0x0000000100000008ULL, 0xFFFFFFFFFFFFFFFFULL,
     {{0xC0064900, 0x00238514, 0x40000000, 0x00000008, 0x00000001, 0xFFFFFFFF, 0xFFFFFFFF, 0}}},
    {Default, Low32, Int, 0x0000400000000004ULL, 0xFFFFFFFFULL,
     {{0xC0064900, 0x00238514, 0x22000000, 0x00000004, 0x00004000, 0xFFFFFFFF, 0, 0}}},
    {Default, Low32, NoInt, 0x1000, 0,
     {{0xC0064900, 0x00238514, 0x20000000, 0x00001000, 0, 0, 0, 0}}},
    {WbOnly, Full64, Int, 0x000012345678ABC0ULL, 0x0000000100000000ULL,
     {{0xC0064900, 0x00088514, 0x42000000, 0x5678ABC0, 0x00001234, 0, 1, 0}}},
    {WbOnly, Full64, NoInt, 0, 1,
     {{0xC0064900, 0x00088514, 0x40000000, 0, 0, 1, 0, 0}}},
    {WbOnly, Low32, Int, 0x00007FFFFFFFFFFCULL, 0x80000000ULL,
     {{0xC0064900, 0x00088514, 0x22000000, 0xFFFFFFFC, 0x00007FFF, 0x80000000, 0, 0}}},
    {WbOnly, Low32, NoInt, 0xDEADBEECULL, 0x12345678ULL,
     {{0xC0064900, 0x00088514, 0x20000000, 0xDEADBEEC, 0, 0x12345678, 0, 0}}},
};

void kfdLayout(const ReleaseMemPacket &packet, const FenceVector &vector) {
    const uint32_t header = packet.dwords[0], control = packet.dwords[1], select = packet.dwords[2];
    const uint32_t wbOnly = vector.cache == WbOnly ? 1 : 0, fullActions = 1 - wbOnly;
    check(field(header, 30, 2) == 3 && field(header, 16, 14) == 6 && field(header, 8, 8) == 0x49 &&
          field(header, 0, 8) == 0, "PM4_MES_TYPE_3_HEADER: type 3, count 6, opcode RELEASE_MEM");
    check(field(control, 0, 6) == 0x14 && field(control, 6, 2) == 0 && field(control, 8, 4) == 5,
          "event_type CACHE_FLUSH_AND_INV_TS_EVENT with event_index end_of_pipe");
    check(field(control, 12, 1) == 0 && field(control, 13, 1) == 0 && field(control, 14, 1) == 0 &&
          field(control, 15, 1) == 1 && field(control, 18, 1) == 0 && field(control, 20, 1) == 0,
          "volatile and write-combine actions clear, tc_wb_action_ena set");
    check(field(control, 16, 1) == fullActions && field(control, 17, 1) == fullActions &&
          field(control, 21, 1) == fullActions && field(control, 19, 1) == wbOnly,
          "tcl1/tc/tc_md actions or tc_nc action selected by the fence flag");
    check(field(control, 22, 3) == 0 && field(control, 25, 2) == 0 && field(control, 27, 2) == 0 &&
          field(control, 29, 3) == 0, "cache_policy LRU; EOP_EXEC and pq_exe_status clear");
    check(field(select, 0, 16) == 0 && field(select, 16, 2) == 0 && field(select, 18, 6) == 0 &&
          field(select, 24, 3) == (vector.interrupt == Int ? 2U : 0U) && field(select, 27, 2) == 0 &&
          field(select, 29, 3) == (vector.data == Full64 ? 2U : 1U),
          "dst_sel memory_controller, int_sel and data_sel");
    if (vector.data == Full64)
        check(field(packet.dwords[3], 0, 3) == 0 &&
              field(packet.dwords[3], 3, 29) == vector.address / 8 % 0x20000000,
              "address_lo_64b holds address bits 31:3");
    else
        check(field(packet.dwords[3], 0, 2) == 0 &&
              field(packet.dwords[3], 2, 30) == vector.address / 4 % 0x40000000,
              "address_lo_32b holds address bits 31:2");
    check(field(packet.dwords[4], 0, 32) == vector.address / 0x100000000ULL &&
          field(packet.dwords[5], 0, 32) == vector.sequence % 0x100000000ULL &&
          field(packet.dwords[6], 0, 32) == vector.sequence / 0x100000000ULL &&
          field(packet.dwords[7], 0, 32) == 0, "address_hi, data_lo, data_hi and zero int_ctxid");
}
void fenceVectorsMatch() {
    for (const FenceVector &vector : fenceVectors) {
        const ReleaseMemFence request = makeFence(vector.cache, vector.data, vector.interrupt,
                                                  vector.address, vector.sequence);
        ReleaseMemPacket packet {};
        check(encodeReleaseMemFence(request, packet) == Status::Ok && samePacket(packet, vector.packet),
              "RELEASE_MEM matches the hand-evaluated gfx_v9_0 fence vector");
        kfdLayout(packet, vector);
        ReleaseMemFence decoded {};
        check(decodeReleaseMemFence(gfx9, vector.packet, decoded) == Status::Ok && sameFence(decoded, request),
              "RELEASE_MEM vector decodes to its request");
    }
}

void unchangedFence(Status expected, const ReleaseMemFence &request, const char *description) {
    ReleaseMemPacket packet;
    for (uint32_t i = 0; i < ReleaseMemDwords; ++i) packet.dwords[i] = 0xA5A5A500U + i;
    check(encodeReleaseMemFence(request, packet) == expected, description);
    bool unchanged = true;
    for (uint32_t i = 0; i < ReleaseMemDwords; ++i) unchanged = unchanged && packet.dwords[i] == 0xA5A5A500U + i;
    check(unchanged, "RELEASE_MEM failure preserves all eight output dwords");
}
void fenceEncodeLimits() {
    unchangedFence(Status::UnsupportedArchitecture, ReleaseMemFence {}, "zero-initialized request rejected");
    for (Architecture architecture : badArchitectures) {
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.architecture = architecture;
        unchangedFence(Status::UnsupportedArchitecture, request, "uninspected architecture fence rejected");
    }
    // CACHE_FLUSH_TS 0x04 and BOTTOM_OF_PIPE_TS 0x28 exist but are not emitted by this path.
    for (unsigned event : {0x00U, 0x04U, 0x28U, 0x3FU}) {
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.event = static_cast<FenceEvent>(event);
        unchangedFence(Status::UnsupportedField, request, "unestablished event rejected");
    }
    for (unsigned cache : {0U, 3U, 0xFFU}) {
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.cache = static_cast<FenceCache>(cache);
        unchangedFence(Status::UnsupportedField, request, "unestablished cache action rejected");
    }
    for (unsigned destination : {1U, 2U, 3U}) { // 1 is DST_SEL TC/L2
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.destination = static_cast<FenceDestination>(destination);
        unchangedFence(Status::UnsupportedField, request, "unestablished destination rejected");
    }
    for (unsigned data : {0U, 3U, 4U, 5U, 7U}) { // discard, GPU/system counters, GDS
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.data = static_cast<FenceData>(data);
        unchangedFence(Status::UnsupportedField, request, "unestablished data selector rejected");
    }
    for (unsigned interrupt : {1U, 3U, 4U, 5U, 6U, 7U}) { // 1 requires DATA_SEL 0
        ReleaseMemFence request = makeFence(Default, Full64, Int, 0x1000, 1);
        request.interrupt = static_cast<FenceInterrupt>(interrupt);
        unchangedFence(Status::UnsupportedField, request, "unestablished interrupt selector rejected");
    }
    for (uint64_t misalign = 1; misalign < 8; ++misalign)
        unchangedFence(Status::InvalidAddress, makeFence(Default, Full64, Int, 0x1000 + misalign, 1),
                       "64-bit data requires qword alignment");
    for (uint64_t misalign = 1; misalign < 4; ++misalign)
        unchangedFence(Status::InvalidAddress, makeFence(WbOnly, Low32, NoInt, 0x1004 + misalign, 1),
                       "32-bit data requires dword alignment");
    for (uint64_t address : {0x0000800000000000ULL, 0x0000FFFFFFFFFFF8ULL, 0x0001000000000000ULL,
                             0xFFFF800000000000ULL, 0xFFFFFFFFFFFFFFF8ULL}) {
        unchangedFence(Status::UnsupportedAddress, makeFence(Default, Full64, Int, address, 1),
                       "address at or above the 48-bit GMC hole start rejected");
        unchangedFence(Status::UnsupportedAddress, makeFence(Default, Low32, Int, address + 4, 1),
                       "32-bit fence address at or above the GMC hole start rejected");
    }
    unchangedFence(Status::InvalidAddress, makeFence(Default, Full64, Int, UINT64_MAX, 1),
                   "misaligned top address rejected");
    for (uint64_t sequence : {0x100000000ULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL})
        unchangedFence(Status::InvalidSequence, makeFence(Default, Low32, Int, 0x1000, sequence),
                       "sequence wider than a 32-bit write rejected, not truncated");
    ReleaseMemPacket packet {};
    check(encodeReleaseMemFence(makeFence(Default, Full64, Int, 0x00007FFFFFFFFFF8ULL, UINT64_MAX), packet) ==
          Status::Ok && packet.dwords[4] == 0x7FFF && packet.dwords[6] == 0xFFFFFFFF,
          "highest qword below the hole with a full 64-bit sequence accepted");
    check(encodeReleaseMemFence(makeFence(WbOnly, Low32, NoInt, 0x00007FFFFFFFFFFCULL, 0xFFFFFFFF), packet) ==
          Status::Ok && packet.dwords[3] == 0xFFFFFFFC && packet.dwords[6] == 0,
          "highest dword below the hole with a full 32-bit sequence accepted");
}

void unchangedDecode(Status expected, const ReleaseMemPacket &packet, const char *description,
                     Architecture architecture = gfx9) {
    const ReleaseMemFence sentinel = makeFence(WbOnly, Low32, NoInt, 0x4444, 0x55);
    ReleaseMemFence decoded = sentinel;
    check(decodeReleaseMemFence(architecture, packet, decoded) == expected, description);
    check(sameFence(decoded, sentinel), "RELEASE_MEM decode failure preserves output");
}
void fenceDecodeLimits() {
    const ReleaseMemPacket base = fenceVectors[0].packet; // Full64 + interrupt
    const ReleaseMemPacket low = fenceVectors[7].packet;  // Low32
    struct Mutation { uint32_t dword, value; Status expected; const char *description; };
    const Mutation mutations[] = {
        {0, 0xC0064902, Status::UnsupportedField, "compute-bit RELEASE_MEM header rejected"},
        {0, 0xC0064901, Status::ReservedBits, "reserved header bit rejected"},
        {0, 0x80000000, Status::NotType3, "type-2 header rejected"},
        {0, 0xC0061000, Status::WrongPacket, "NOP header is not a fence"},
        {0, 0xC0054900, Status::WrongPacket, "short RELEASE_MEM count rejected"},
        {0, 0xC0074900, Status::WrongPacket, "long RELEASE_MEM count rejected"},
        {1, 0x10238514, Status::UnsupportedField, "EOP_EXEC trailing-fence bit rejected"},
        {1, 0x00239514, Status::UnsupportedField, "TCL1_VOL action rejected"},
        {1, 0x0023A514, Status::UnsupportedField, "TC_VOL action rejected"},
        {1, 0x002B8514, Status::UnsupportedField, "mixed cache actions rejected"},
        {1, 0x00238528, Status::UnsupportedField, "BOTTOM_OF_PIPE_TS event rejected"},
        {1, 0x00238614, Status::UnsupportedField, "shader_done event index rejected"},
        {1, 0x02238514, Status::UnsupportedField, "STREAM cache policy rejected"},
        {2, 0x42010000, Status::UnsupportedField, "DST_SEL TC/L2 rejected"},
        {2, 0x41000000, Status::UnsupportedField, "interrupt-only INT_SEL rejected"},
        {2, 0x62000000, Status::UnsupportedField, "GPU clock DATA_SEL rejected"},
        {2, 0x02000000, Status::UnsupportedField, "discard DATA_SEL rejected"},
        {2, 0x42000001, Status::UnsupportedField, "reserved select bit rejected"},
        {3, 0xFFFFFFFC, Status::InvalidAddress, "dword-only alignment rejected for 64-bit data"},
        {4, 0x00008000, Status::UnsupportedAddress, "address at the hole start rejected"},
        {4, 0xFFFF8000, Status::UnsupportedAddress, "sign-extended upper-half address rejected"},
        {7, 1, Status::UnsupportedField, "nonzero int_ctxid rejected"},
    };
    for (const Mutation &mutation : mutations) {
        ReleaseMemPacket packet = base;
        packet.dwords[mutation.dword] = mutation.value;
        unchangedDecode(mutation.expected, packet, mutation.description);
    }
    ReleaseMemPacket wide = low;
    wide.dwords[6] = 1;
    unchangedDecode(Status::InvalidSequence, wide, "32-bit fence with nonzero data_hi rejected");
    for (Architecture architecture : badArchitectures)
        unchangedDecode(Status::UnsupportedArchitecture, base, "uninspected architecture decode rejected",
                        architecture);
}

void decoderIsExactInverse() {
    // A single-bit flip is accepted only when it yields another encodable fence.
    // Full64 at an 8-aligned VA: address bits 3..46 (44), sequence (64), INT_SEL 2->0.
    // Low32 at a 4-aligned VA: address bits 2..46 (45), data_lo (32), INT_SEL 0->2.
    struct Base { ReleaseMemFence fence; unsigned accepted; };
    const Base bases[] = {
        {makeFence(Default, Full64, Int, 0x0000123456789AB8ULL, 0x0123456789ABCDEFULL), 44 + 64 + 1},
        {makeFence(WbOnly, Low32, NoInt, 0x1004, 0x12345678), 45 + 32 + 1},
    };
    for (const Base &item : bases) {
        ReleaseMemPacket packet {};
        check(encodeReleaseMemFence(item.fence, packet) == Status::Ok, "strictness base encodes");
        unsigned accepted = 0, violations = 0;
        for (uint32_t dword = 0; dword < ReleaseMemDwords; ++dword)
            for (uint32_t bit = 0; bit < 32; ++bit) {
                ReleaseMemPacket mutated = packet;
                mutated.dwords[dword] ^= 1U << bit;
                ReleaseMemFence decoded = item.fence;
                if (decodeReleaseMemFence(gfx9, mutated, decoded) != Status::Ok) {
                    if (!sameFence(decoded, item.fence)) ++violations;
                    continue;
                }
                ++accepted;
                ReleaseMemPacket again {};
                if (encodeReleaseMemFence(decoded, again) != Status::Ok || !samePacket(again, mutated))
                    ++violations;
            }
        check(violations == 0, "every accepted bit flip re-encodes to the identical packet");
        check(accepted == item.accepted, "decoder accepts exactly the encodable single-bit neighbours");
    }
}
} // namespace

int main() {
    headerVectorsMatch();
    headerEncodeLimits();
    headerDecodeSweep();
    streamMeasurement();
    fenceVectorsMatch();
    fenceEncodeLimits();
    fenceDecodeLimits();
    decoderIsExactInverse();
    printf("NativeAmd PM4 packet construction: %u checks passed; GPU execution NOT_RUN.\n", checks);
}

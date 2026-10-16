// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// HOSTED encoding regression for Drivers/NativeGlk/CommandEncoding. No GPU,
// ring, GTT, MMIO or submission is involved; GPU execution is NOT_RUN.
#include "Drivers/NativeGlk/CommandEncoding.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

using namespace MellowNativeGlk;
namespace {
unsigned checks = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", description);
        exit(1);
    }
}
constexpr Generation gen9 = Generation::Gen9;
constexpr Engine bcs = Engine::Bcs0;
constexpr Site ring = Site::Ring, batch = Site::PpgttBatch;
constexpr AddressSpace ggtt = AddressSpace::Ggtt, ppgtt = AddressSpace::Ppgtt;

// Oracle, independent of the encoder's constants: MI_INSTR(opcode, flags) of
// intel_gpu_commands.h places client 0 in [31:29] and opcode in [28:23].
constexpr uint32_t mi(uint32_t opcode, uint32_t flags) { return opcode * 0x800000U + flags; }
constexpr uint32_t bit(unsigned n) { return n == 0 ? 1U : 2U * bit(n - 1); }
static_assert(mi(0x00, 0) == 0x00000000, "MI_NOOP");
static_assert(mi(0x02, 0) == 0x01000000, "MI_USER_INTERRUPT");
static_assert(mi(0x05, 0) == 0x02800000, "MI_ARB_CHECK");
static_assert(mi(0x08, 0) + 1 == 0x04000001, "MI_ARB_ON_OFF | MI_ARB_ENABLE");
static_assert(mi(0x0a, 0) == 0x05000000, "MI_BATCH_BUFFER_END");
static_assert(mi(0x20, 2) + bit(22) == 0x10400002, "MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT");
static_assert(mi(0x26, 1) + 1 + bit(14) == 0x13004002, "(MI_FLUSH_DW + 1) | OP_STOREDW");
static_assert(mi(0x26, 1) + 1 + bit(21) + bit(14) == 0x13204002, "| STORE_INDEX");
static_assert(mi(0x26, 1) + 1 + bit(21) + bit(18) + bit(14) == 0x13244002, "| INVALIDATE_TLB");
static_assert(mi(0x31, 1) + bit(8) == 0x18800101, "MI_BATCH_BUFFER_START_GEN8 | BIT(8)");
static_assert(0x34 * 4 == 0xD0 && PphwspScratchIndex == 0xD0, "LRC_PPHWSP_SCRATCH_ADDR");

void toBytes(const uint32_t *words, size_t count, uint8_t *out) {
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < 4; ++k) {
            uint32_t word = words[i];
            for (size_t shift = 0; shift < k; ++shift) word /= 0x100;
            out[i * 4 + k] = static_cast<uint8_t>(word % 0x100);
        }
}
Command make(CommandKind kind) { Command c; c.kind = kind; return c; }
Command store(AddressSpace space, uint64_t address, uint32_t value) {
    Command c = make(CommandKind::StoreDwordImm);
    c.space = space; c.address = address; c.value = value;
    return c;
}
Command flushScratch(bool invalidate) {
    Command c = make(CommandKind::FlushDw);
    c.postSync = FlushPostSync::ScratchIndex; c.invalidateTlb = invalidate;
    return c;
}
Command flushGgtt(uint64_t address, uint32_t value) {
    Command c = make(CommandKind::FlushDw);
    c.postSync = FlushPostSync::GgttStoreDword; c.space = ggtt; c.address = address; c.value = value;
    return c;
}
Command batchStart(AddressSpace space, uint64_t address) {
    Command c = make(CommandKind::BatchBufferStart);
    c.space = space; c.address = address;
    return c;
}
Command arb(bool enable) { Command c = make(CommandKind::ArbOnOff); c.enable = enable; return c; }
bool same(const Command &a, const Command &b) {
    return a.kind == b.kind && a.space == b.space && a.address == b.address && a.value == b.value &&
           a.postSync == b.postSync && a.invalidateTlb == b.invalidateTlb && a.enable == b.enable;
}
Status decodeWords(Site site, const uint32_t *words, size_t count, Command &out, uint32_t &dwords) {
    uint8_t bytes[4 * MaxCommandDwords] = {};
    toBytes(words, count, bytes);
    return decodeCommand(gen9, bcs, site, bytes, count * 4, out, dwords);
}

struct Vector { Site site; Command command; uint32_t count; uint32_t words[4]; };
const Vector vectors[] = {
    {ring, make(CommandKind::Noop), 1, {0x00000000}},
    {batch, make(CommandKind::Noop), 1, {0x00000000}},
    {ring, make(CommandKind::UserInterrupt), 1, {0x01000000}},
    {ring, make(CommandKind::ArbCheck), 1, {0x02800000}},
    {ring, arb(true), 1, {0x04000001}},
    {ring, arb(false), 1, {0x04000000}},
    {batch, make(CommandKind::BatchBufferEnd), 1, {0x05000000}},
    {ring, store(ggtt, 0x00FED000, 0xC0DE1234), 4, {0x10400002, 0x00FED000, 0, 0xC0DE1234}},
    {ring, store(ggtt, 0xFFFFFFFC, 0), 4, {0x10400002, 0xFFFFFFFC, 0, 0}},
    {batch, store(ppgtt, 0x00007FFFFFFFFFFCULL, 0xFFFFFFFF), 4, {0x10000002, 0xFFFFFFFC, 0x7FFF, 0xFFFFFFFF}},
    {batch, store(ppgtt, 0, 1), 4, {0x10000002, 0, 0, 1}},
    {ring, flushScratch(false), 4, {0x13204002, 0xD0, 0, 0}},
    {ring, flushScratch(true), 4, {0x13244002, 0xD0, 0, 0}},
    {ring, flushGgtt(0x00001000, 0x12345678), 4, {0x13004002, 0x00001004, 0, 0x12345678}},
    {ring, flushGgtt(0xFFFFFFD8, 7), 4, {0x13004002, 0xFFFFFFDC, 0, 7}},
    {ring, batchStart(ggtt, 0x00200000), 3, {0x18800001, 0x00200000, 0}},
    {ring, batchStart(ppgtt, 0x00007FFFFFFFF000ULL), 3, {0x18800101, 0xFFFFF000, 0x7FFF}},
    {ring, batchStart(ppgtt, 0x0000000100000008ULL), 3, {0x18800101, 8, 1}},
};

void vectorsMatch() {
    for (const Vector &v : vectors) {
        EncodedCommand encoded;
        check(encodeCommand(gen9, bcs, v.site, v.command, encoded) == Status::Ok &&
              encoded.count == v.count && memcmp(encoded.dwords, v.words, v.count * 4) == 0,
              "encoding matches the hand-evaluated i915 DWORDs");
        Command decoded; uint32_t dwords = 0;
        check(decodeWords(v.site, v.words, v.count, decoded, dwords) == Status::Ok &&
              dwords == v.count && same(decoded, v.command), "decoder is the exact inverse");
    }
}

void byteOrder() {
    const uint32_t words[2] = {0x10400002, 0x00FED000};
    const uint8_t expected[8] = {0x02, 0x00, 0x40, 0x10, 0x00, 0xD0, 0xFE, 0x00};
    uint8_t bytes[9];
    memset(bytes, 0xA5, sizeof bytes);
    check(storeLittleEndian(words, 2, bytes, 8) == Status::Ok && memcmp(bytes, expected, 8) == 0 &&
          bytes[8] == 0xA5, "little-endian bytes are explicit and bounded");
    check(loadLittleEndian(expected) == 0x10400002 && loadLittleEndian(expected + 4) == 0x00FED000,
          "little-endian load");
    memset(bytes, 0xA5, sizeof bytes);
    check(storeLittleEndian(words, 2, bytes, 7) == Status::InsufficientSpace && bytes[0] == 0xA5,
          "short capacity writes nothing");
    check(storeLittleEndian(nullptr, 1, bytes, 8) == Status::InvalidArgument &&
          storeLittleEndian(words, 1, nullptr, 8) == Status::InvalidArgument &&
          storeLittleEndian(nullptr, 0, nullptr, 0) == Status::Ok, "null buffers");
}

void profileRejections() {
    const Generation generations[] = {static_cast<Generation>(0), static_cast<Generation>(8),
                                      static_cast<Generation>(10), static_cast<Generation>(12)};
    const Engine engines[] = {Engine::Rcs0, Engine::Vcs0, Engine::Vecs0, static_cast<Engine>(0),
                              static_cast<Engine>(0xFF)};
    const Site sites[] = {static_cast<Site>(0), static_cast<Site>(3), static_cast<Site>(0xFF)};
    for (const Vector &v : vectors) {
        EncodedCommand encoded;
        memset(&encoded, 0xA5, sizeof encoded);
        uint8_t bytes[16] = {};
        toBytes(v.words, v.count, bytes);
        Command decoded = make(CommandKind::Noop); uint32_t dwords = 99;
        for (Generation g : generations) {
            check(encodeCommand(g, bcs, v.site, v.command, encoded) == Status::UnsupportedGeneration &&
                  encoded.count == 0xA5A5A5A5, "only the Gen9 command format is represented");
            check(decodeCommand(g, bcs, v.site, bytes, v.count * 4, decoded, dwords) ==
                  Status::UnsupportedGeneration && dwords == 99, "decode generation");
        }
        // RCS0 uses PIPE_CONTROL flushes and breadcrumbs; no other engine is represented.
        for (Engine e : engines) {
            check(encodeCommand(gen9, e, v.site, v.command, encoded) == Status::UnsupportedEngine &&
                  encoded.count == 0xA5A5A5A5, "only BCS0 command forms are encoded");
            check(decodeCommand(gen9, e, v.site, bytes, v.count * 4, decoded, dwords) ==
                  Status::UnsupportedEngine && dwords == 99, "decode engine");
        }
        for (Site s : sites)
            check(encodeCommand(gen9, bcs, s, v.command, encoded) == Status::InvalidSite &&
                  decodeCommand(gen9, bcs, s, bytes, v.count * 4, decoded, dwords) == Status::InvalidSite,
                  "unknown site");
        check(decoded.kind == CommandKind::Noop && dwords == 99, "outputs unchanged on failure");
    }
}

void siteRules() {
    struct Case { Site site; Command command; Status status; };
    const Case cases[] = {
        {ring, store(ppgtt, 0x1000, 1), Status::UnsupportedAddressSpace},
        {batch, store(ggtt, 0x1000, 1), Status::UnsupportedAddressSpace},
        {ring, make(CommandKind::BatchBufferEnd), Status::UnsupportedAtSite},
        {batch, make(CommandKind::UserInterrupt), Status::UnsupportedAtSite},
        {batch, make(CommandKind::ArbCheck), Status::UnsupportedAtSite},
        {batch, arb(true), Status::UnsupportedAtSite},
        {batch, flushScratch(true), Status::UnsupportedAtSite},
        {batch, flushGgtt(0x1000, 1), Status::UnsupportedAtSite},
        {batch, batchStart(ppgtt, 0x1000), Status::UnsupportedAtSite},
        {batch, batchStart(ggtt, 0x1000), Status::UnsupportedAtSite},
    };
    for (const Case &c : cases) {
        EncodedCommand encoded;
        memset(&encoded, 0xA5, sizeof encoded);
        check(encodeCommand(gen9, bcs, c.site, c.command, encoded) == c.status &&
              encoded.count == 0xA5A5A5A5, "site rule on encode");
        // The same words, produced for the other site, are refused by the decoder.
        const Site other = c.site == ring ? batch : ring;
        check(encodeCommand(gen9, bcs, other, c.command, encoded) == Status::Ok, "other site encodes");
        Command decoded; uint32_t dwords = 0;
        check(decodeWords(c.site, encoded.dwords, encoded.count, decoded, dwords) == c.status,
              "site rule on decode");
    }
}

void fieldRules() {
    struct Case { Site site; Command command; Status status; };
    Command c[16];
    for (Command &x : c) x = make(CommandKind::Noop);
    c[0].kind = static_cast<CommandKind>(0);
    c[1].kind = static_cast<CommandKind>(9);
    c[2].value = 1;
    c[3].enable = true;
    c[4] = arb(true); c[4].address = 8;
    c[5] = store(ggtt, 0x1000, 1); c[5].postSync = FlushPostSync::ScratchIndex;
    c[6] = store(ggtt, 0x1000, 1); c[6].invalidateTlb = true;
    c[7] = flushScratch(false); c[7].postSync = static_cast<FlushPostSync>(0);
    c[8] = flushScratch(false); c[8].postSync = static_cast<FlushPostSync>(3);
    c[9] = flushScratch(false); c[9].space = ggtt;
    c[10] = flushScratch(false); c[10].address = PphwspScratchIndex;
    c[11] = flushScratch(false); c[11].value = 1;
    c[12] = flushGgtt(0x1000, 1); c[12].invalidateTlb = true;
    c[13] = batchStart(ggtt, 0x1000); c[13].value = 1;
    c[14] = make(CommandKind::BatchBufferEnd); c[14].space = ppgtt;
    c[15] = make(CommandKind::UserInterrupt); c[15].postSync = FlushPostSync::GgttStoreDword;
    for (const Command &command : c) {
        EncodedCommand encoded;
        const Site site = command.kind == CommandKind::BatchBufferEnd ? batch : ring;
        check(encodeCommand(gen9, bcs, site, command, encoded) == Status::InvalidCommand,
              "unknown kinds and stray fields are rejected, never dropped");
    }
    const Case spaces[] = {
        {ring, flushGgtt(0x1000, 1), Status::Ok},
        {ring, store(static_cast<AddressSpace>(0), 0x1000, 1), Status::InvalidAddressSpace},
        {ring, store(static_cast<AddressSpace>(3), 0x1000, 1), Status::InvalidAddressSpace},
        {ring, batchStart(static_cast<AddressSpace>(0), 0x1000), Status::InvalidAddressSpace},
    };
    for (const Case &s : spaces) {
        EncodedCommand encoded;
        check(encodeCommand(gen9, bcs, s.site, s.command, encoded) == s.status, "address space selector");
    }
    Command ppgttFlush = flushGgtt(0x1000, 1);
    ppgttFlush.space = ppgtt;
    EncodedCommand encoded;
    check(encodeCommand(gen9, bcs, ring, ppgttFlush, encoded) == Status::UnsupportedAddressSpace,
          "post-sync PPGTT store (MI_FLUSH_DW_USE_PPGTT) is not emitted for BCS");
}

void addressRules() {
    struct Case { Site site; Command command; Status status; };
    const Case cases[] = {
        {ring, store(ggtt, 0x1002, 1), Status::MisalignedAddress},
        {ring, store(ggtt, 0x100000000ULL, 1), Status::AddressOutOfRange},
        {batch, store(ppgtt, 0x7FFFFFFFFFFEULL, 1), Status::MisalignedAddress},
        {batch, store(ppgtt, PpgttAddressLimit, 1), Status::AddressOutOfRange},
        {batch, store(ppgtt, 0xFFFF800000000000ULL, 1), Status::AddressOutOfRange},
        {ring, flushGgtt(0x1004, 1), Status::MisalignedAddress},
        {ring, flushGgtt(0x1020, 1), Status::ReservedAddressBit},
        {ring, flushGgtt(0xFFFFFFE0, 1), Status::ReservedAddressBit},
        {ring, flushGgtt(0x100000000ULL, 1), Status::AddressOutOfRange},
        {ring, batchStart(ggtt, 0x1004), Status::MisalignedAddress},
        {ring, batchStart(ggtt, 0x100000000ULL), Status::AddressOutOfRange},
        {ring, batchStart(ppgtt, PpgttAddressLimit), Status::AddressOutOfRange},
        {ring, batchStart(ppgtt, 0xFFFFFFFFFFFFF000ULL), Status::AddressOutOfRange},
    };
    for (const Case &c : cases) {
        EncodedCommand encoded;
        memset(&encoded, 0xA5, sizeof encoded);
        check(encodeCommand(gen9, bcs, c.site, c.command, encoded) == c.status &&
              encoded.count == 0xA5A5A5A5, "address alignment, range and reserved bits");
    }
}

void decodeRejections() {
    struct Case { Site site; uint32_t count; uint32_t words[4]; Status status; };
    const Case cases[] = {
        // Foreign MI opcodes, including ones gen9_blt_cmds lets user batches use.
        {ring, 3, {mi(0x22, 1), 0x22000, 0}, Status::UnrecognizedCommand},     // MI_LOAD_REGISTER_IMM(1)
        {ring, 4, {mi(0x1c, 2), 0, 0, 0}, Status::UnrecognizedCommand},        // MI_SEMAPHORE_WAIT
        {ring, 4, {mi(0x24, 2), 0x22000, 0, 0}, Status::UnrecognizedCommand},  // MI_STORE_REGISTER_MEM_GEN8
        {ring, 1, {mi(0x04, 0)}, Status::UnrecognizedCommand},                 // MI_FLUSH
        {ring, 1, {mi(0x03, 0)}, Status::UnrecognizedCommand},                 // MI_WAIT_FOR_EVENT
        {ring, 4, {mi(0x2f, 1), 0, 0, 0}, Status::UnrecognizedCommand},        // MI_ATOMIC
        {ring, 3, {mi(0x2A, 1), 0x22000, 0x22004}, Status::UnrecognizedCommand}, // MI_LOAD_REGISTER_REG
        // Non-MI clients: COLOR_BLT_CMD, a 2D XY_ header and RCS PIPE_CONTROL(6).
        {ring, 4, {0x50000003, 0, 0, 0}, Status::UnrecognizedCommand},
        {batch, 4, {0x54C00006, 0, 0, 0}, Status::UnrecognizedCommand},
        {ring, 4, {0x7A000004, 0, 0, 0}, Status::UnrecognizedCommand},
        {ring, 1, {0x20000000}, Status::UnrecognizedCommand},
        {ring, 1, {0xE0000000}, Status::UnrecognizedCommand},
        // Length fields that are not the represented Gen8+ forms.
        {ring, 4, {0x10400003, 0x1000, 0, 1}, Status::InvalidLength},
        {batch, 4, {0x10200003, 0x1000, 0, 1}, Status::InvalidLength},      // MI_STORE_QWORD_IMM_GEN8
        {ring, 3, {0x10400001, 0x1000, 1}, Status::InvalidLength},          // MI_STORE_DWORD_IMM
        {ring, 3, {mi(0x26, 1), 0xD0, 0}, Status::InvalidLength},           // gen6 MI_FLUSH_DW
        {ring, 4, {0x13004003, 0x1004, 0, 1}, Status::InvalidLength},
        {ring, 3, {mi(0x31, 0), 0x1000, 0}, Status::InvalidLength},         // MI_BATCH_BUFFER_START
        {ring, 4, {0x18800102, 0x1000, 0, 0}, Status::InvalidLength},
        {ring, 3, {0x18800081, 0x1000, 0}, Status::InvalidLength},          // MI_BATCH_GTT in length
        // Header bits not emitted on Gen9 BCS.
        {ring, 1, {bit(22)}, Status::UnsupportedField},
        {batch, 1, {1}, Status::UnsupportedField},
        {ring, 1, {0x01000001}, Status::UnsupportedField},
        {ring, 1, {0x02800100}, Status::UnsupportedField},                  // gen12 preparser_disable
        {ring, 1, {0x04000002}, Status::UnsupportedField},
        {batch, 1, {0x05000001}, Status::UnsupportedField},
        {batch, 4, {0x10200002, 0x1000, 0, 1}, Status::UnsupportedField},
        {ring, 4, {0x10400402, 0x1000, 0, 1}, Status::UnsupportedField},
        {ring, 4, {0x13004002 | bit(7), 0x1004, 0, 1}, Status::UnsupportedField},  // MI_INVALIDATE_BSD
        {ring, 4, {0x13004002 | bit(8), 0x1004, 0, 1}, Status::UnsupportedField},  // MI_FLUSH_DW_NOTIFY
        {ring, 4, {0x13004002 | bit(9), 0x1004, 0, 1}, Status::UnsupportedField},  // MI_FLUSH_DW_LLC
        {ring, 4, {0x13004002 | bit(16), 0x1004, 0, 1}, Status::UnsupportedField}, // MI_FLUSH_DW_CCS
        {ring, 4, {0x13004002 | bit(22), 0x1004, 0, 1}, Status::UnsupportedField}, // PROTECTED_MEM_EN
        {ring, 4, {0x13044002, 0x1004, 0, 1}, Status::UnsupportedField},
        {ring, 4, {0x13000002, 0x1004, 0, 1}, Status::UnsupportedField},
        {ring, 4, {0x13200002, 0xD0, 0, 0}, Status::UnsupportedField},
        {ring, 4, {0x13008002, 0x1004, 0, 1}, Status::UnsupportedField},
        {ring, 4, {0x1300C002, 0x1004, 0, 1}, Status::UnsupportedField},
        {ring, 3, {0x18800001 | bit(22), 0x1000, 0}, Status::UnsupportedField},     // second level
        {ring, 3, {0x18800101 | bit(15), 0x1000, 0}, Status::UnsupportedField},     // predicate
        {ring, 3, {0x18800101 | bit(10), 0x1000, 0}, Status::UnsupportedField},     // resource streamer
        // Operands.
        {ring, 4, {0x13204002, 0xD4, 0, 0}, Status::UnsupportedField},
        {ring, 4, {0x13204002, 0xD0, 1, 0}, Status::UnsupportedField},
        {ring, 4, {0x13244002, 0xD0, 0, 5}, Status::UnsupportedField},
        {ring, 4, {0x13004002, 0x1000, 0, 1}, Status::UnsupportedAddressSpace},
        {ring, 4, {0x13004002, 0x1006, 0, 1}, Status::MisalignedAddress},
        {ring, 4, {0x13004002, 0x1024, 0, 1}, Status::ReservedAddressBit},
        {ring, 4, {0x13004002, 0x1004, 1, 1}, Status::AddressOutOfRange},
        {ring, 4, {0x10400002, 0x1001, 0, 1}, Status::MisalignedAddress},
        {ring, 4, {0x10400002, 0x1000, 1, 1}, Status::AddressOutOfRange},
        {batch, 4, {0x10000002, 0x1000, 0x8000, 1}, Status::AddressOutOfRange},
        {ring, 3, {0x18800001, 0x1004, 0}, Status::MisalignedAddress},
        {ring, 3, {0x18800001, 0x1000, 1}, Status::AddressOutOfRange},
        {ring, 3, {0x18800101, 0x1000, 0xFFFF}, Status::AddressOutOfRange},
        {ring, 3, {0x18800101, 0x1000, 0xFFFFFFFF}, Status::AddressOutOfRange},
    };
    for (const Case &c : cases) {
        Command decoded = make(CommandKind::ArbCheck); uint32_t dwords = 77;
        check(decodeWords(c.site, c.words, c.count, decoded, dwords) == c.status &&
              decoded.kind == CommandKind::ArbCheck && dwords == 77, "decoder rejection, outputs unchanged");
    }
    // Every proper prefix of every represented command is truncated.
    for (const Vector &v : vectors) {
        uint8_t bytes[16];
        toBytes(v.words, v.count, bytes);
        for (size_t length = 0; length < v.count * 4; ++length) {
            Command decoded; uint32_t dwords = 77;
            check(decodeCommand(gen9, bcs, v.site, bytes, length, decoded, dwords) == Status::Truncated &&
                  dwords == 77, "truncated packet");
        }
    }
    Command decoded; uint32_t dwords = 0;
    check(decodeCommand(gen9, bcs, ring, nullptr, 0, decoded, dwords) == Status::Truncated &&
          decodeCommand(gen9, bcs, ring, nullptr, 4, decoded, dwords) == Status::InvalidArgument,
          "null input");
}

// A decoder that accepted a word the encoder would not produce could hide a
// foreign command. Any accepted image must re-encode byte for byte.
void acceptedImpliesExactReencode(Site site, const uint32_t *words, unsigned &accepted) {
    Command decoded; uint32_t dwords = 0;
    if (decodeWords(site, words, MaxCommandDwords, decoded, dwords) != Status::Ok) return;
    ++accepted;
    EncodedCommand encoded;
    check(encodeCommand(gen9, bcs, site, decoded, encoded) == Status::Ok && encoded.count == dwords &&
          memcmp(encoded.dwords, words, dwords * 4) == 0, "accepted words re-encode exactly");
}
bool representedHeader(uint32_t header, Site site) {
    const uint32_t ringHeaders[] = {0x00000000, 0x01000000, 0x02800000, 0x04000000, 0x04000001,
                                    0x10400002, 0x13004002, 0x13204002, 0x13244002, 0x18800001, 0x18800101};
    const uint32_t batchHeaders[] = {0x00000000, 0x05000000, 0x10000002};
    const uint32_t *list = site == ring ? ringHeaders : batchHeaders;
    const size_t count = site == ring ? sizeof ringHeaders / 4 : sizeof batchHeaders / 4;
    for (size_t i = 0; i < count; ++i) if (list[i] == header) return true;
    return false;
}
void strictInverseSweep() {
    // Single-bit flips of each represented header and operand dword.
    for (const Vector &v : vectors)
        for (uint32_t word = 0; word < v.count; ++word)
            for (unsigned b = 0; b < 32; ++b) {
                uint32_t words[MaxCommandDwords] = {};
                memcpy(words, v.words, v.count * 4);
                words[word] ^= bit(b);
                unsigned accepted = 0;
                acceptedImpliesExactReencode(ring, words, accepted);
                acceptedImpliesExactReencode(batch, words, accepted);
            }
    // Every client/opcode pair with the low-bit patterns of every MI form.
    const uint32_t lows[] = {0, 1, 2, 3, 0x101, 0x4002, 0x204002, 0x244002, 0x400002, 0x200003};
    const uint32_t operands[3] = {0x1004, 0, 0};
    for (uint32_t top = 0; top < 512; ++top)
        for (uint32_t low : lows)
            for (Site site : {ring, batch}) {
                const uint32_t header = top * 0x800000U + low;
                if (header / 0x800000U != top) continue;
                const uint32_t words[4] = {header, operands[0], operands[1], operands[2]};
                Command decoded; uint32_t dwords = 0;
                const Status status = decodeWords(site, words, 4, decoded, dwords);
                // Operands 0x1004/0/0 suit only the post-sync flush and the ring store/start
                // headers' alignment rules; the header itself must be in the represented set.
                if (status == Status::Ok)
                    check(representedHeader(header, site), "only represented headers decode");
                if (!representedHeader(header, site))
                    check(status != Status::Ok, "foreign header rejected");
            }
    // Pseudo-random words: acceptance always means an exact encoder image.
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    unsigned accepted = 0;
    for (unsigned i = 0; i < 2000000; ++i) {
        uint32_t words[MaxCommandDwords];
        for (uint32_t &w : words) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            w = static_cast<uint32_t>(state >> 16);
        }
        if (i % 4 == 1) {
            words[0] = vectors[i / 4 % (sizeof vectors / sizeof vectors[0])].words[0];
            if (i % 8 == 1) { words[1] &= ~3U; words[2] = 0; }
        }
        acceptedImpliesExactReencode(i % 2 ? ring : batch, words, accepted);
    }
    check(accepted > 1000, "sweep exercised accepted encoder images");
}

void writerTransactions() {
    CommandWriter writer;
    uint8_t bytes[64];
    check(writer.append(make(CommandKind::Noop)) == Status::InvalidArgument &&
          writer.padToQword() == Status::InvalidArgument && writer.finishBatch() == Status::InvalidArgument,
          "writer refuses use before begin");
    check(writer.begin(static_cast<Generation>(8), bcs, batch, bytes, 64) == Status::UnsupportedGeneration &&
          writer.begin(gen9, Engine::Rcs0, batch, bytes, 64) == Status::UnsupportedEngine &&
          writer.begin(gen9, bcs, static_cast<Site>(0), bytes, 64) == Status::InvalidSite &&
          writer.begin(gen9, bcs, batch, nullptr, 64) == Status::InvalidArgument &&
          writer.append(make(CommandKind::Noop)) == Status::InvalidArgument, "writer begin rejections");

    // igt_emit_store_dw for Gen8+: two PPGTT stores one page apart, then MI_BATCH_BUFFER_END.
    memset(bytes, 0xA5, sizeof bytes);
    check(writer.begin(gen9, bcs, batch, bytes, 40) == Status::Ok, "batch writer");
    check(writer.append(store(ppgtt, 0x0000123456789000ULL, 0xDEADBEEF)) == Status::Ok &&
          writer.append(store(ppgtt, 0x000012345678A000ULL, 0xDEADBEEF)) == Status::Ok &&
          writer.bytesWritten() == 32, "batch stores");
    check(writer.append(flushScratch(false)) == Status::UnsupportedAtSite && writer.bytesWritten() == 32 &&
          bytes[32] == 0xA5, "failed append leaves storage unchanged");
    check(writer.finishBatch() == Status::Ok && writer.bytesWritten() == 40, "end plus qword pad");
    const uint32_t expected[10] = {0x10000002, 0x56789000, 0x1234, 0xDEADBEEF,
                                   0x10000002, 0x5678A000, 0x1234, 0xDEADBEEF, 0x05000000, 0x00000000};
    uint8_t image[40];
    toBytes(expected, 10, image);
    check(memcmp(bytes, image, 40) == 0 && bytes[40] == 0xA5, "batch image bytes");
    check(writer.append(make(CommandKind::Noop)) == Status::TrailingData &&
          writer.finishBatch() == Status::TrailingData && writer.padToQword() == Status::Ok &&
          writer.bytesWritten() == 40, "nothing follows the batch end");
    size_t commands = 0;
    check(checkCommandImage(gen9, bcs, batch, bytes, 40, commands) == Status::Ok && commands == 4,
          "writer output passes the image check");

    // finishBatch is transactional over the end and its pad.
    memset(bytes, 0xA5, sizeof bytes);
    check(writer.begin(gen9, bcs, batch, bytes, 20) == Status::Ok &&
          writer.append(store(ppgtt, 0x1000, 1)) == Status::Ok, "short batch");
    check(writer.finishBatch() == Status::InsufficientSpace && writer.bytesWritten() == 16 &&
          bytes[16] == 0xA5 && bytes[19] == 0xA5, "end without room for its pad writes nothing");
    check(writer.begin(gen9, bcs, batch, bytes, 8) == Status::Ok && writer.finishBatch() == Status::Ok &&
          writer.bytesWritten() == 8 && loadLittleEndian(bytes) == 0x05000000 && loadLittleEndian(bytes + 4) == 0,
          "empty batch is end plus pad");

    // Ring writer: no batch end; explicit qword padding.
    memset(bytes, 0xA5, sizeof bytes);
    check(writer.begin(gen9, bcs, ring, bytes, 12) == Status::Ok, "ring writer");
    check(writer.finishBatch() == Status::UnsupportedAtSite &&
          writer.append(make(CommandKind::BatchBufferEnd)) == Status::UnsupportedAtSite, "no BBE in ring");
    check(writer.append(make(CommandKind::UserInterrupt)) == Status::Ok && writer.padToQword() == Status::Ok &&
          writer.bytesWritten() == 8 && writer.padToQword() == Status::Ok && writer.bytesWritten() == 8,
          "pad only when odd");
    check(writer.append(store(ggtt, 0x2000, 3)) == Status::InsufficientSpace && writer.bytesWritten() == 8 &&
          bytes[8] == 0xA5 && bytes[11] == 0xA5, "partial packet never written");
    check(writer.append(make(CommandKind::ArbCheck)) == Status::Ok &&
          writer.padToQword() == Status::InsufficientSpace && writer.bytesWritten() == 12, "pad needs room");
}

void imageChecks() {
    struct Case { Site site; uint32_t count; uint32_t words[8]; Status status; size_t commands; };
    const Case cases[] = {
        {ring, 0, {}, Status::Ok, 0},
        {ring, 2, {0x01000000, 0}, Status::Ok, 2},
        {ring, 1, {0x01000000}, Status::UnalignedStream, 0},
        {ring, 2, {0x10400002, 0x1000}, Status::Truncated, 0},
        {ring, 4, {0x10400002, 0x1000, 0, 0x7A000004}, Status::Ok, 1},
        {ring, 4, {0x7A000004, 0, 0, 0}, Status::UnrecognizedCommand, 0},
        {ring, 2, {0x05000000, 0}, Status::UnsupportedAtSite, 0},
        {batch, 0, {}, Status::MissingBatchEnd, 0},
        {batch, 2, {0, 0}, Status::MissingBatchEnd, 0},
        {batch, 2, {0x05000000, 0}, Status::Ok, 2},
        {batch, 2, {0, 0x05000000}, Status::Ok, 2},
        {batch, 4, {0x05000000, 0, 0, 0}, Status::TrailingData, 0},
        {batch, 4, {0, 0x05000000, 0, 0}, Status::TrailingData, 0},
        {batch, 2, {0x05000000, 0x01000000}, Status::TrailingData, 0},
        {batch, 6, {0x05000000, 0x10000002, 0x1000, 0, 1, 0}, Status::TrailingData, 0},
        {batch, 6, {0x10000002, 0x1000, 0, 1, 0x05000000, 0}, Status::Ok, 3},
        {batch, 6, {0x10000002, 0x1000, 0, 1, 0x05000000, 0x02800000}, Status::TrailingData, 0},
        {batch, 6, {0x10400002, 0x1000, 0, 1, 0x05000000, 0}, Status::UnsupportedAddressSpace, 0},
    };
    for (const Case &c : cases) {
        uint8_t bytes[32];
        toBytes(c.words, c.count, bytes);
        size_t commands = 1234;
        const Status status = checkCommandImage(gen9, bcs, c.site, bytes, c.count * 4, commands);
        check(status == c.status && commands == (status == Status::Ok ? c.commands : 1234),
              "image allowlist and qword/end rules");
    }
    const uint8_t bytes[6] = {};
    size_t commands = 0;
    check(checkCommandImage(gen9, bcs, ring, bytes, 6, commands) == Status::UnalignedStream &&
          checkCommandImage(gen9, bcs, ring, nullptr, 8, commands) == Status::InvalidArgument &&
          checkCommandImage(gen9, Engine::Rcs0, ring, bytes, 0, commands) == Status::UnsupportedEngine,
          "image arguments");
}

StoreProbeRing goodProbe() {
    StoreProbeRing p;
    p.probe.target = 0x00FED000;
    p.probe.nonce = 0x6C6B4731;
    p.probe.cpuInitial = 0x00000000;
    p.ring.space = ggtt;
    p.ring.base = 0x00100000;
    p.ring.bytes = 0x4000;
    p.breadcrumb = 0x00200080;
    p.breadcrumbValue = 0x2A;
    return p;
}
void probeRing() {
    const StoreProbeRing p = goodProbe();
    check(checkStoreProbeRing(p) == Status::Ok, "probe accepted");
    uint8_t bytes[72];
    memset(bytes, 0xA5, sizeof bytes);
    CommandWriter writer;
    check(writer.begin(gen9, bcs, ring, bytes, 64) == Status::Ok && writer.appendStoreProbeRing(p) == Status::Ok &&
          writer.bytesWritten() == StoreProbeRingDwords * 4, "probe ring written");
    const uint32_t expected[StoreProbeRingDwords] = {
        0x13244002, 0x000000D0, 0x00000000, 0x00000000, // gen8_emit_flush_xcs(EMIT_INVALIDATE)
        0x10400002, 0x00FED000, 0x00000000, 0x6C6B4731, // ring GGTT store of the nonce
        0x13004002, 0x00200084, 0x00000000, 0x0000002A, // gen8_emit_ggtt_write breadcrumb
        0x01000000, 0x04000001,                         // MI_USER_INTERRUPT, MI_ARB_ENABLE
        0x02800000, 0x00000000,                         // gen8_emit_wa_tail
    };
    uint8_t image[64];
    toBytes(expected, StoreProbeRingDwords, image);
    check(memcmp(bytes, image, 64) == 0 && bytes[64] == 0xA5, "probe ring bytes");
    check(StoreProbeRingTailBytes == 14 * 4 && StoreProbeRingWaTailBytes == 16 * 4 &&
          StoreProbeRingTailBytes % 8 == 0, "rq->tail and rq->wa_tail are qword offsets");
    size_t commands = 0;
    check(checkCommandImage(gen9, bcs, ring, bytes, 64, commands) == Status::Ok && commands == 7,
          "probe ring passes the allowlist");

    struct Case { StoreProbeRing probe; Status status; };
    Case cases[24];
    for (Case &c : cases) c = {p, Status::Ok};
    cases[0].probe.probe.nonce = p.probe.cpuInitial;           cases[0].status = Status::InvalidProbe;
    cases[1].probe.probe.cpuInitial = 5; cases[1].probe.probe.nonce = 0;
                                                                cases[1].status = Status::InvalidProbe;
    cases[2].probe.probe.nonce = 0xFFFFFFFF;                    cases[2].status = Status::InvalidProbe;
    cases[3].probe.ring.space = ppgtt;                          cases[3].status = Status::UnsupportedAddressSpace;
    cases[4].probe.ring.space = static_cast<AddressSpace>(0);  cases[4].status = Status::InvalidAddressSpace;
    cases[5].probe.ring.bytes = 0;                              cases[5].status = Status::InvalidProbe;
    cases[6].probe.ring.base = 0xFFFFF000; cases[6].probe.ring.bytes = 0x2000;
                                                                cases[6].status = Status::AddressOutOfRange;
    cases[7].probe.ring.base = 0xFFFFFFFFFFFFF000ULL;           cases[7].status = Status::AddressOutOfRange;
    cases[8].probe.probe.target = 0x00FED002;                   cases[8].status = Status::MisalignedAddress;
    cases[9].probe.probe.target = 0x100000000ULL;               cases[9].status = Status::AddressOutOfRange;
    cases[10].probe.breadcrumb = 0x00200084;                    cases[10].status = Status::MisalignedAddress;
    cases[11].probe.breadcrumb = 0x002000A0;                    cases[11].status = Status::ReservedAddressBit;
    // The nonce dword of the emitted image lies inside the ring: reading it
    // back would show CPU-written command bytes, not a GPU store.
    cases[12].probe.probe.target = p.ring.base + 7 * 4;         cases[12].status = Status::ProbeAliasesCommands;
    cases[13].probe.probe.target = p.ring.base;                 cases[13].status = Status::ProbeAliasesCommands;
    cases[14].probe.probe.target = p.ring.base + p.ring.bytes - 4;
                                                                cases[14].status = Status::ProbeAliasesCommands;
    cases[15].probe.probe.target = p.ring.base - 4;             cases[15].status = Status::Ok;
    cases[16].probe.probe.target = p.ring.base + p.ring.bytes;  cases[16].status = Status::Ok;
    cases[17].probe.breadcrumb = p.ring.base + 0x100;           cases[17].status = Status::ProbeAliasesCommands;
    cases[18].probe.probe.target = p.breadcrumb;                cases[18].status = Status::ProbeAliasesBreadcrumb;
    cases[19].probe.probe.target = p.breadcrumb + 4;            cases[19].status = Status::Ok;
    cases[20].probe.probe.target = p.breadcrumb - 4;            cases[20].status = Status::Ok;
    cases[21].probe.breadcrumb = 0x100000000ULL;                cases[21].status = Status::AddressOutOfRange;
    cases[22].probe.ring.base = 0xFFFFC000;                     cases[22].status = Status::Ok;
    cases[23].probe.breadcrumb = p.ring.base + p.ring.bytes - 0x40;
                                                                cases[23].status = Status::ProbeAliasesCommands;
    for (const Case &c : cases) {
        check(checkStoreProbeRing(c.probe) == c.status, "probe rule");
        uint8_t out[64];
        memset(out, 0xA5, sizeof out);
        CommandWriter w;
        check(w.begin(gen9, bcs, ring, out, 64) == Status::Ok && w.appendStoreProbeRing(c.probe) == c.status,
              "writer applies the probe rule");
        if (c.status != Status::Ok)
            check(w.bytesWritten() == 0 && out[0] == 0xA5 && out[63] == 0xA5, "rejected probe writes nothing");
    }

    uint8_t out[80];
    memset(out, 0xA5, sizeof out);
    check(writer.begin(gen9, bcs, batch, out, 80) == Status::Ok &&
          writer.appendStoreProbeRing(p) == Status::UnsupportedAtSite && writer.bytesWritten() == 0,
          "probe ring only in a ring");
    check(writer.begin(gen9, bcs, ring, out, 80) == Status::Ok && writer.append(make(CommandKind::Noop)) == Status::Ok &&
          writer.appendStoreProbeRing(p) == Status::UnalignedStream && writer.bytesWritten() == 4, "qword start");
    check(writer.begin(gen9, bcs, ring, out, 60) == Status::Ok &&
          writer.appendStoreProbeRing(p) == Status::InsufficientSpace && writer.bytesWritten() == 0 &&
          out[4] == 0xA5, "probe ring is all or nothing");
    CommandWriter unused;
    check(unused.appendStoreProbeRing(p) == Status::InvalidArgument, "probe before begin");

    uint8_t reading[4];
    toBytes(&p.probe.nonce, 1, reading);
    check(classifyProbeReading(p.probe, reading) == ProbeReading::NonceObserved, "nonce observed");
    check(classifyProbeReading(p.probe, nullptr) == ProbeReading::Invalid, "missing observation rejected");
    for (uint32_t nonce : {p.probe.cpuInitial, 0U, 0xFFFFFFFFU}) {
        StoreProbe invalid = p.probe;
        invalid.nonce = nonce;
        check(classifyProbeReading(invalid, reading) == ProbeReading::Invalid,
              "invalid nonce cannot be observed as GPU write");
    }
    toBytes(&p.probe.cpuInitial, 1, reading);
    check(classifyProbeReading(p.probe, reading) == ProbeReading::Unchanged, "initial fill");
    const uint8_t swapped[4] = {0x6C, 0x6B, 0x47, 0x31};
    const uint8_t other[4] = {0x31, 0x47, 0x6B, 0x6D};
    check(classifyProbeReading(p.probe, swapped) == ProbeReading::Foreign &&
          classifyProbeReading(p.probe, other) == ProbeReading::Foreign, "byte order and foreign values");
}
} // namespace

int main() {
    vectorsMatch();
    byteOrder();
    profileRejections();
    siteRules();
    fieldRules();
    addressRules();
    decodeRejections();
    strictInverseSweep();
    writerTransactions();
    imageChecks();
    probeRing();
    printf("NativeGlk Gen9 BCS0 MI command construction (HOSTED): %u checks passed; GPU execution NOT_RUN.\n",
           checks);
}

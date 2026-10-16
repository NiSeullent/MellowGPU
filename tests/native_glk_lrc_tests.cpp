// SPDX-License-Identifier: MIT
// Copyright © 2014 Intel Corporation
// Copyright (c) 2026 Mellow contributors.
// The gen9 offset tables below are verbatim from Linux intel_lrc.c at
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a (MIT); see
// retained permission notice in Drivers/NativeGlk/LrcImage.cpp.
//
// Hosted regression tests of the pure GLK LRC builder. They run on the build
// machine only; nothing here touches a GPU, MMIO or a kernel.
#include "Drivers/NativeGlk/LrcImage.hpp"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

using namespace MellowNativeGlk::Lrc;

namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const char *description) {
    ++checks;
    if (!value) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", description);
    }
}

// ---- Independent oracles ---------------------------------------------------
// Byte-wise little-endian reads, independent of the production serializer.
uint32_t le32(const uint8_t *image, size_t byteOffset) {
    return image[byteOffset] + image[byteOffset + 1] * 0x100U +
           image[byteOffset + 2] * 0x10000U + image[byteOffset + 3] * 0x1000000U;
}
uint32_t reg(const uint8_t *image, unsigned dword) { return le32(image, 4096 + 4 * dword); }
void putReg(uint8_t *image, unsigned dword, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) image[4096 + 4 * dword + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint8_t pattern(size_t i) { return static_cast<uint8_t>(i * 31 + 7); }

// intel_lrc_reg.h register-state dword indices (value slots).
constexpr unsigned CTX_CONTEXT_CONTROL = 0x03, CTX_RING_HEAD = 0x05, CTX_RING_TAIL = 0x07,
                   CTX_RING_START = 0x09, CTX_RING_CTL = 0x0b, CTX_TIMESTAMP = 0x23,
                   CTX_PDP3_UDW = 0x25, CTX_PDP3_LDW = 0x27, CTX_PDP2_UDW = 0x29,
                   CTX_PDP2_LDW = 0x2b, CTX_PDP1_UDW = 0x2d, CTX_PDP1_LDW = 0x2f,
                   CTX_PDP0_UDW = 0x31, CTX_PDP0_LDW = 0x33, CTX_R_PWR_CLK_STATE = 0x43;
// Gen9 lrc_ring_*() indices plus one (value slots).
constexpr unsigned MI_MODE_VALUE = 0x55, BB_OFFSET_VALUE = 0x65, PER_CTX_VALUE = 0x19,
                   INDIRECT_CTX_VALUE = 0x1b, INDIRECT_OFFSET_VALUE = 0x1d;

// Verbatim intel_lrc.c tables with the set_offsets() macro encodings, minus
// their BUILD_BUG_ON_ZERO guards.
#define NOP(x) (0x80 | (x))
#define LRI(count, flags) ((flags) << 6 | (count))
#define POSTED 1
#define REG(x) ((x) >> 2)
#define REG16(x) (((x) >> 9) | 0x80), (((x) >> 2) & 0x7f)
#define END 0
const uint8_t gen9_xcs_offsets[] = {
    NOP(1), LRI(14, POSTED), REG16(0x244), REG(0x034), REG(0x030), REG(0x038), REG(0x03c),
    REG(0x168), REG(0x140), REG(0x110), REG(0x11c), REG(0x114), REG(0x118), REG(0x1c0),
    REG(0x1c4), REG(0x1c8),
    NOP(3), LRI(9, POSTED), REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284),
    REG16(0x280), REG16(0x27c), REG16(0x278), REG16(0x274), REG16(0x270),
    NOP(13), LRI(1, POSTED), REG16(0x200),
    NOP(13), LRI(44, POSTED), REG(0x028), REG(0x09c), REG(0x0c0), REG(0x178), REG(0x17c),
    REG16(0x358), REG(0x170), REG(0x150), REG(0x154), REG(0x158), REG16(0x41c),
    REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c), REG16(0x610), REG16(0x614),
    REG16(0x618), REG16(0x61c), REG16(0x620), REG16(0x624), REG16(0x628), REG16(0x62c),
    REG16(0x630), REG16(0x634), REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644),
    REG16(0x648), REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c),
    REG16(0x660), REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674),
    REG16(0x678), REG16(0x67c), REG(0x068),
    END
};
const uint8_t gen9_rcs_offsets[] = {
    NOP(1), LRI(14, POSTED), REG16(0x244), REG(0x34), REG(0x30), REG(0x38), REG(0x3c),
    REG(0x168), REG(0x140), REG(0x110), REG(0x11c), REG(0x114), REG(0x118), REG(0x1c0),
    REG(0x1c4), REG(0x1c8),
    NOP(3), LRI(9, POSTED), REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284),
    REG16(0x280), REG16(0x27c), REG16(0x278), REG16(0x274), REG16(0x270),
    NOP(13), LRI(1, 0), REG(0xc8),
    NOP(13), LRI(44, POSTED), REG(0x28), REG(0x9c), REG(0xc0), REG(0x178), REG(0x17c),
    REG16(0x358), REG(0x170), REG(0x150), REG(0x154), REG(0x158), REG16(0x41c),
    REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c), REG16(0x610), REG16(0x614),
    REG16(0x618), REG16(0x61c), REG16(0x620), REG16(0x624), REG16(0x628), REG16(0x62c),
    REG16(0x630), REG16(0x634), REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644),
    REG16(0x648), REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c),
    REG16(0x660), REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674),
    REG16(0x678), REG16(0x67c), REG(0x68),
    END
};
#undef END
#undef REG16
#undef REG
#undef POSTED
#undef LRI
#undef NOP

// Test-local decode of the u8 encoding as documented above set_offsets():
// MI_LOAD_REGISTER_IMM(n) = 0x22 << 23 | (2n - 1), FORCE_POSTED bit 12,
// MI_BATCH_BUFFER_END = 0x0a << 23 (intel_gpu_commands.h, graphics ver 9).
struct Layout {
    uint32_t dword[1024];
    bool command[1024]; // header, register offset or batch end
    unsigned end;       // index after the last register pair
};
void decodeTable(const uint8_t *data, uint32_t base, Layout &layout) {
    for (unsigned i = 0; i < 1024; ++i) { layout.dword[i] = 0; layout.command[i] = false; }
    unsigned i = 0;
    while (*data) {
        if (*data & 0x80) { i += *data++ & 0x7f; continue; }
        unsigned count = *data & 0x3f;
        const bool posted = (*data >> 6) & 1;
        ++data;
        layout.dword[i] = 0x11000000U + 2 * count - 1 + (posted ? 0x1000U : 0);
        layout.command[i++] = true;
        while (count--) {
            uint32_t offset = 0;
            uint8_t v;
            do { v = *data++; offset = offset << 7 | (v & 0x7f); } while (v & 0x80);
            layout.dword[i] = base + offset * 4;
            layout.command[i] = true;
            i += 2;
        }
    }
    layout.end = i;
}

constexpr size_t RcsBytes = 22 * 4096, XcsBytes = 2 * 4096;
uint8_t imageA[RcsBytes], imageB[RcsBytes], defaultState[RcsBytes], snapshot[RcsBytes];
uint8_t overlap[2 * XcsBytes];

void fill(uint8_t *buffer, size_t bytes, bool usePattern) {
    for (size_t i = 0; i < bytes; ++i) buffer[i] = usePattern ? pattern(i) : 0xA5;
}
void snap(const uint8_t *image, size_t bytes) { for (size_t i = 0; i < bytes; ++i) snapshot[i] = image[i]; }
bool unchanged(const uint8_t *image, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (image[i] != snapshot[i]) return false;
    return true;
}
bool zeroRange(const uint8_t *image, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) if (image[i]) return false;
    return true;
}
bool patternRange(const uint8_t *image, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) if (image[i] != pattern(i)) return false;
    return true;
}

struct EngineCase { Engine engine; uint32_t base; size_t bytes; bool render; const uint8_t *table; };
const EngineCase engines[] = {
    {Engine::Rcs0, 0x02000, RcsBytes, true, gen9_rcs_offsets},   // RENDER_RING_BASE
    {Engine::Vcs0, 0x12000, XcsBytes, false, gen9_xcs_offsets},  // GEN6_BSD_RING_BASE
    {Engine::Bcs0, 0x22000, XcsBytes, false, gen9_xcs_offsets},  // BLT_RING_BASE
    {Engine::Vecs0, 0x1a000, XcsBytes, false, gen9_xcs_offsets}, // VEBOX_RING_BASE
};

constexpr uint64_t Pml4 = 0x0000001234567000ULL;
constexpr uint32_t ContextGgtt = 0x00100000, RingGgtt = 0x00200000, RingBytes = 0x4000;
const RenderPrerequisites render = {0x00300000, 0xC0, 0x80000B88};

ContextImageSpec specFor(const EngineCase &e) {
    ContextImageSpec spec;
    spec.engine = e.engine;
    spec.pml4Dma = Pml4;
    spec.contextTimestamp = 0x12345678;
    spec.render = e.render ? &render : nullptr;
    return spec;
}

// Expected dwords written by the init_*() helpers (hand-evaluated):
// CONTEXT_CONTROL = masked enable INHIBIT_SYN_CTX_SWITCH (bit 3) | masked
// disable RESTORE_INHIBIT, SAVE_INHIBIT, RS_CTX_ENABLE (bits 0-2) | restore
// inhibit bit 0 when fresh.
constexpr uint32_t FreshControl = 0x000F0009, DefaultControl = 0x000F0008;

void engineTable() {
    for (const EngineCase &e : engines) {
        EngineInfo info;
        check(describeEngine(e.engine, info) == Status::Ok && info.mmioBase == e.base &&
              info.contextBytes == e.bytes && info.renderState == e.render,
              "engine base, GEN9/GEN8 LR context size and render state");
    }
    EngineInfo info;
    info.mmioBase = 7;
    check(describeEngine(static_cast<Engine>(4), info) == Status::UnsupportedEngine &&
          info.mmioBase == 7, "unknown engine rejected without output");
}

void freshImageLayout() {
    for (const EngineCase &e : engines) {
        fill(imageA, e.bytes, true);
        const ContextImageSpec spec = specFor(e);
        check(initContextImage(spec, nullptr, 0, imageA, e.bytes) == Status::Ok, "fresh image builds");
        Layout layout;
        decodeTable(e.table, e.base, layout);
        check(layout.end == 0xaa, "gen9 tables end at dword 0xaa");
        bool commandsMatch = true;
        for (unsigned i = 0; i < 1024; ++i)
            if (layout.command[i] && reg(imageA, i) != layout.dword[i]) commandsMatch = false;
        check(commandsMatch, "every LRI header and register offset matches the upstream table");
        check(reg(imageA, 0xaa) == 0x05000000, "fresh list closed by MI_BATCH_BUFFER_END");
        // Hand-evaluated header spot checks.
        check(reg(imageA, 0x01) == 0x1100101B && reg(imageA, 0x21) == 0x11001011 &&
              reg(imageA, 0x51) == 0x11001057, "LRI(14/9/44, POSTED) headers");
        check(reg(imageA, 0x41) == (e.render ? 0x11000001U : 0x11001001U),
              "gen9 RCS LRI(1, 0) vs XCS LRI(1, POSTED)");
        check(reg(imageA, 0x42) == e.base + (e.render ? 0xc8U : 0x200U), "slot 0x42 register");
        check(reg(imageA, 0x02) == e.base + 0x244 && reg(imageA, 0x06) == e.base + 0x30 &&
              reg(imageA, 0x54) == e.base + 0x9c && reg(imageA, 0x64) == e.base + 0x158 &&
              reg(imageA, 0xa8) == e.base + 0x68, "register offsets carry the engine base");
        // Values.
        check(reg(imageA, CTX_CONTEXT_CONTROL) == FreshControl, "fresh CONTEXT_CONTROL");
        check(reg(imageA, CTX_TIMESTAMP) == 0x12345678, "CTX_TIMESTAMP from runtime.last");
        check(reg(imageA, CTX_PDP0_UDW) == 0x12 && reg(imageA, CTX_PDP0_LDW) == 0x34567000,
              "PML4 in PDP0 (ASSIGN_CTX_PML4)");
        check(reg(imageA, CTX_PDP1_UDW) == 0 && reg(imageA, CTX_PDP1_LDW) == 0 &&
              reg(imageA, CTX_PDP2_UDW) == 0 && reg(imageA, CTX_PDP2_LDW) == 0 &&
              reg(imageA, CTX_PDP3_UDW) == 0 && reg(imageA, CTX_PDP3_LDW) == 0,
              "PDP1-3 untouched for a 4-level PPGTT");
        check(reg(imageA, MI_MODE_VALUE) == 0x01000000, "STOP_RING masked-disabled");
        check(reg(imageA, BB_OFFSET_VALUE) == 0, "BB_OFFSET cleared");
        check(reg(imageA, PER_CTX_VALUE) == 0, "no Gen9 per-context batch");
        check(reg(imageA, INDIRECT_CTX_VALUE) == (e.render ? 0x00300003U : 0U),
              "INDIRECT_CTX = address | cachelines (RCS only)");
        check(reg(imageA, INDIRECT_OFFSET_VALUE) == (e.render ? 0x980U : 0U),
              "INDIRECT_CTX_OFFSET = 0x26 << 6 (RCS only)");
        check(reg(imageA, CTX_RING_HEAD) == 0 && reg(imageA, CTX_RING_TAIL) == 0 &&
              reg(imageA, CTX_RING_START) == 0 && reg(imageA, CTX_RING_CTL) == 0 &&
              reg(imageA, CTX_R_PWR_CLK_STATE) == 0, "ring and RPCS wait for lrc_update_regs");
        // Everything else zero: PPHWSP, unlisted register dwords, later pages.
        bool restZero = zeroRange(imageA, 0, 4096) && zeroRange(imageA, 8192, e.bytes);
        const unsigned values[] = {CTX_CONTEXT_CONTROL, CTX_TIMESTAMP, CTX_PDP0_UDW, CTX_PDP0_LDW,
                                   MI_MODE_VALUE, INDIRECT_CTX_VALUE, INDIRECT_OFFSET_VALUE};
        for (unsigned i = 0; i < 1024; ++i) {
            bool known = layout.command[i] || i == 0xaa;
            for (unsigned v : values) known = known || v == i;
            if (!known && reg(imageA, i) != 0) restZero = false;
        }
        check(restZero, "all other image bytes zero");
    }
}

void updateAndDescriptor() {
    for (const EngineCase &e : engines) {
        const ContextImageSpec spec = specFor(e);
        check(initContextImage(spec, nullptr, 0, imageA, e.bytes) == Status::Ok, "init before update");
        uint32_t lrca = 0;
        const RingSpec ring = {RingGgtt, RingBytes, 0x40, 0x80};
        check(updateContextRegisters(spec, ring, ContextGgtt, imageA, e.bytes, lrca) == Status::Ok,
              "lrc_update_regs succeeds");
        check(lrca == 0x0010011D, "LRCA | LEGACY_64B << 3 | VALID | PRIVILEGE | FORCE_RESTORE");
        check(reg(imageA, CTX_RING_START) == RingGgtt && reg(imageA, CTX_RING_HEAD) == 0x40 &&
              reg(imageA, CTX_RING_TAIL) == 0x80 && reg(imageA, CTX_RING_CTL) == 0x3001,
              "RING_START/HEAD/TAIL and RING_CTL_SIZE | RING_VALID");
        check(reg(imageA, CTX_R_PWR_CLK_STATE) == (e.render ? 0x80000B88U : 0U), "RPCS on RCS only");
        RegisterState state;
        check(decodeRegisterState(e.engine, imageA, e.bytes, state) == Status::Ok &&
              state.contextControl == FreshControl && state.ringHead == 0x40 &&
              state.ringTail == 0x80 && state.ringStart == RingGgtt && state.ringCtl == 0x3001 &&
              state.timestamp == 0x12345678 && state.pdpUdw[0] == 0x12 &&
              state.pdpLdw[0] == 0x34567000 && state.miMode == 0x01000000 && state.listClosed &&
              state.indirectCtx == (e.render ? 0x00300003U : 0U),
              "decoder returns the encoded values");
        check(checkRegisterState(state, RingGgtt, RingBytes) == Status::Ok, "lrc_check_regs clean");
        RegisterState waiting = state;
        waiting.ringCtl |= 0xC00; // RING_WAIT | RING_WAIT_SEMAPHORE ignored
        check(checkRegisterState(waiting, RingGgtt, RingBytes) == Status::Ok, "wait bits ignored");
        check(checkRegisterState(state, RingGgtt + 4096, RingBytes) == Status::RingMismatch,
              "RING_START mismatch");
        check(checkRegisterState(state, RingGgtt, RingBytes * 2) == Status::RingMismatch,
              "RING_CTL size mismatch");
        RegisterState stopped = state;
        stopped.miMode = 0x01000100;
        check(checkRegisterState(stopped, RingGgtt, RingBytes) == Status::StopRingSet, "STOP_RING");
        stopped.miMode = 0x00000100; // value without its mask bit is not enabled
        check(checkRegisterState(stopped, RingGgtt, RingBytes) == Status::Ok, "masked STOP_RING");
    }
    // Largest ring: RING_CTL_SIZE(2 MiB) fills RING_NR_PAGES.
    const EngineCase &v = engines[1];
    const ContextImageSpec spec = specFor(v);
    initContextImage(spec, nullptr, 0, imageA, v.bytes);
    uint32_t lrca = 0;
    check(updateContextRegisters(spec, {RingGgtt, 0x200000, 0, 0x1FFFF8}, 0xFFFFE000, imageA,
                                 v.bytes, lrca) == Status::Ok &&
          reg(imageA, CTX_RING_CTL) == 0x1FF001 && lrca == 0xFFFFE11D,
          "2 MiB ring and a context ending at 4 GiB");
}

void updateRejects() {
    const EngineCase &v = engines[1];
    const ContextImageSpec spec = specFor(v);
    initContextImage(spec, nullptr, 0, imageA, v.bytes);
    struct RingCase { RingSpec ring; uint32_t ggtt; Status status; const char *what; };
    const RingCase cases[] = {
        {{RingGgtt, 0x3000, 0, 0}, ContextGgtt, Status::InvalidRing, "ring size not a power of two"},
        {{RingGgtt, 0x800, 0, 0}, ContextGgtt, Status::InvalidRing, "ring below one page"},
        {{RingGgtt, 0x400000, 0, 0}, ContextGgtt, Status::InvalidRing, "ring beyond RING_NR_PAGES"},
        {{0, RingBytes, 0, 0}, ContextGgtt, Status::InvalidRing, "ring at GGTT 0"},
        {{RingGgtt + 0x800, RingBytes, 0, 0}, ContextGgtt, Status::InvalidRing, "unaligned ring"},
        {{0xFFFFF000, RingBytes, 0, 0}, ContextGgtt, Status::InvalidRing, "ring past 4 GiB"},
        {{RingGgtt, RingBytes, 0x44, 0}, ContextGgtt, Status::InvalidRingOffset, "head not qword"},
        {{RingGgtt, RingBytes, RingBytes, 0}, ContextGgtt, Status::InvalidRingOffset, "head == size"},
        {{RingGgtt, RingBytes, 0, 0x4008}, ContextGgtt, Status::InvalidRingOffset, "tail outside"},
        {{RingGgtt, RingBytes, 0, 0x7C}, ContextGgtt, Status::InvalidRingOffset, "tail not qword"},
        {{RingGgtt, RingBytes, 0, 0}, 0, Status::InvalidContextAddress, "context at GGTT 0"},
        {{RingGgtt, RingBytes, 0, 0}, 0x100800, Status::InvalidContextAddress, "unaligned LRCA"},
        {{RingGgtt, RingBytes, 0, 0}, 0xFFFFF000, Status::InvalidContextAddress, "context past 4 GiB"},
    };
    for (const RingCase &c : cases) {
        snap(imageA, v.bytes);
        uint32_t lrca = 0xDEAD;
        check(updateContextRegisters(spec, c.ring, c.ggtt, imageA, v.bytes, lrca) == c.status &&
              lrca == 0xDEAD && unchanged(imageA, v.bytes), c.what);
    }
    uint32_t lrca = 0xDEAD;
    uint8_t *zero = imageB;
    fill(zero, v.bytes, false);
    for (size_t i = 0; i < v.bytes; ++i) zero[i] = 0;
    check(updateContextRegisters(spec, {RingGgtt, RingBytes, 0, 0}, ContextGgtt, zero, v.bytes,
                                 lrca) == Status::LayoutMismatch && lrca == 0xDEAD &&
          zeroRange(zero, 0, v.bytes), "update refuses an image without the ring slots");
    ContextImageSpec rcs = specFor(engines[0]);
    initContextImage(rcs, nullptr, 0, imageA, RcsBytes);
    rcs.render = nullptr;
    snap(imageA, RcsBytes);
    check(updateContextRegisters(rcs, {RingGgtt, RingBytes, 0, 0}, ContextGgtt, imageA, RcsBytes,
                                 lrca) == Status::MissingRenderPrerequisites &&
          unchanged(imageA, RcsBytes), "RCS update needs the RPCS value");
}

void initRejects() {
    ContextImageSpec rcs = specFor(engines[0]);
    fill(imageA, RcsBytes, true);
    snap(imageA, RcsBytes);
    rcs.render = nullptr;
    check(initContextImage(rcs, nullptr, 0, imageA, RcsBytes) == Status::MissingRenderPrerequisites &&
          unchanged(imageA, RcsBytes), "RCS refuses without workaround batch and RPCS");
    ContextImageSpec vcs = specFor(engines[1]);
    vcs.render = &render;
    check(initContextImage(vcs, nullptr, 0, imageA, XcsBytes) == Status::UnexpectedRenderPrerequisites &&
          unchanged(imageA, RcsBytes), "render inputs rejected for VCS");
    vcs.render = nullptr;
    const uint64_t badPml4[] = {0, Pml4 + 8, Pml4 + 0x800, 1ULL << 39, 0xFFFFFFFFFFFFF000ULL};
    for (uint64_t pml4 : badPml4) {
        vcs.pml4Dma = pml4;
        check(initContextImage(vcs, nullptr, 0, imageA, XcsBytes) == Status::InvalidAddressSpace &&
              unchanged(imageA, RcsBytes), "PML4 must be a nonzero 4 KiB page below 2^39");
    }
    vcs.pml4Dma = (1ULL << 39) - 4096;
    check(initContextImage(vcs, nullptr, 0, imageA, XcsBytes) == Status::Ok, "highest DMA page accepted");
    fill(imageA, RcsBytes, true);
    snap(imageA, RcsBytes);
    vcs.pml4Dma = Pml4;
    check(initContextImage(vcs, nullptr, 0, imageA, XcsBytes - 1) == Status::InvalidBuffer &&
          initContextImage(vcs, nullptr, 0, imageA, XcsBytes + 4096) == Status::InvalidBuffer &&
          initContextImage(vcs, nullptr, 0, nullptr, XcsBytes) == Status::InvalidBuffer &&
          initContextImage(vcs, nullptr, XcsBytes, imageA, XcsBytes) == Status::InvalidBuffer &&
          initContextImage(vcs, defaultState, 0, imageA, XcsBytes) == Status::InvalidBuffer &&
          initContextImage(vcs, defaultState, XcsBytes - 4, imageA, XcsBytes) == Status::InvalidBuffer &&
          unchanged(imageA, RcsBytes), "image and default-state sizes must equal context_size");
    for (size_t i = 0; i < sizeof overlap; ++i) overlap[i] = pattern(i);
    check(initContextImage(vcs, overlap + 4096, XcsBytes, overlap, XcsBytes) == Status::InvalidBuffer &&
          patternRange(overlap, 0, sizeof overlap), "partially aliased default state rejected");
    vcs.engine = static_cast<Engine>(9);
    check(initContextImage(vcs, nullptr, 0, imageA, XcsBytes) == Status::UnsupportedEngine &&
          unchanged(imageA, RcsBytes), "unknown engine");
    struct WaCase { RenderPrerequisites wa; Status status; const char *what; };
    const WaCase waCases[] = {
        {{0x00300000, 0, 0}, Status::InvalidWorkaroundBatch, "empty indirect batch"},
        {{0x00300000, 0x20, 0}, Status::InvalidWorkaroundBatch, "batch not whole cachelines"},
        {{0x00300000, 64 * 64, 0}, Status::InvalidWorkaroundBatch, "64 cachelines overflow the field"},
        {{0x00300020, 0x40, 0}, Status::InvalidWorkaroundBatch, "batch not cacheline aligned"},
        {{0, 0x40, 0}, Status::InvalidWorkaroundBatch, "batch at GGTT 0"},
        {{0xFFFFFFC0, 0x80, 0}, Status::InvalidWorkaroundBatch, "batch past 4 GiB"},
        {{0x00300000, 0x40, 1U << 12}, Status::InvalidPowerClockState, "RPCS bit 12 reserved on Gen9"},
        {{0x00300000, 0x40, 1U << 19}, Status::InvalidPowerClockState, "RPCS bit 19 reserved"},
        {{0x00300000, 0x40, 1U << 30}, Status::InvalidPowerClockState, "RPCS bit 30 reserved"},
    };
    rcs = specFor(engines[0]);
    for (const WaCase &c : waCases) {
        rcs.render = &c.wa;
        check(initContextImage(rcs, nullptr, 0, imageA, RcsBytes) == c.status &&
              unchanged(imageA, RcsBytes), c.what);
    }
    const RenderPrerequisites widest = {0xFFFFF000, 63 * 64, 0x8007FFFF & ~0x7000U};
    rcs.render = &widest;
    check(initContextImage(rcs, nullptr, 0, imageA, RcsBytes) == Status::Ok &&
          reg(imageA, INDIRECT_CTX_VALUE) == 0xFFFFF03F, "63-cacheline batch at the GGTT top");
}

void defaultStateImage() {
    for (const EngineCase &e : engines) {
        for (size_t i = 0; i < e.bytes; ++i) defaultState[i] = pattern(i);
        fill(imageA, e.bytes, false);
        const ContextImageSpec spec = specFor(e);
        const uint32_t oldMode = reg(defaultState, MI_MODE_VALUE);
        check(initContextImage(spec, defaultState, e.bytes, imageA, e.bytes) == Status::Ok,
              "default-state image builds");
        Layout layout;
        decodeTable(e.table, e.base, layout);
        bool commandsMatch = true;
        for (unsigned i = 0; i < 1024; ++i)
            if (layout.command[i] && reg(imageA, i) != layout.dword[i]) commandsMatch = false;
        check(commandsMatch, "set_offsets() rewrites headers over the default state");
        check(zeroRange(imageA, 0, 4096), "PPHWSP cleared");
        check(reg(imageA, CTX_CONTEXT_CONTROL) == DefaultControl, "restore not inhibited");
        check(reg(imageA, 0xaa) == reg(defaultState, 0xaa), "list not closed");
        check(reg(imageA, MI_MODE_VALUE) == ((oldMode & ~0x100U) | 0x01000000U),
              "__reset_stop_ring read-modify-write");
        check(reg(imageA, CTX_PDP1_LDW) == reg(defaultState, CTX_PDP1_LDW) &&
              reg(imageA, CTX_PDP3_UDW) == reg(defaultState, CTX_PDP3_UDW) &&
              reg(imageA, CTX_RING_TAIL) == reg(defaultState, CTX_RING_TAIL) &&
              reg(imageA, CTX_R_PWR_CLK_STATE) == reg(defaultState, CTX_R_PWR_CLK_STATE) &&
              reg(imageA, PER_CTX_VALUE) == reg(defaultState, PER_CTX_VALUE) &&
              reg(imageA, 0) == reg(defaultState, 0),
              "unwritten register dwords keep the default state");
        check(reg(imageA, CTX_PDP0_LDW) == 0x34567000 && reg(imageA, BB_OFFSET_VALUE) == 0 &&
              reg(imageA, CTX_TIMESTAMP) == 0x12345678, "init values written");
        check(patternRange(imageA, 8192, e.bytes), "later pages copied");
        // In place: the image already holds the default state.
        for (size_t i = 0; i < e.bytes; ++i) imageB[i] = pattern(i);
        check(initContextImage(spec, imageB, e.bytes, imageB, e.bytes) == Status::Ok &&
              reg(imageB, CTX_CONTEXT_CONTROL) == DefaultControl && zeroRange(imageB, 0, 4096),
              "in-place default state");
    }
}

void scrubAndStopRing() {
    for (const EngineCase &e : engines) {
        for (size_t i = 0; i < e.bytes; ++i) imageA[i] = pattern(i);
        const ContextImageSpec spec = specFor(e);
        check(scrubRegisterState(spec, imageA, e.bytes) == Status::Ok, "scrub");
        check(patternRange(imageA, 0, 4096) && patternRange(imageA, 8192, e.bytes),
              "scrub touches only the register-state page");
        check(reg(imageA, CTX_CONTEXT_CONTROL) == FreshControl && reg(imageA, 0xaa) == 0x05000000 &&
              reg(imageA, CTX_RING_TAIL) == 0 && reg(imageA, MI_MODE_VALUE) == 0x01000000,
              "scrubbed page is a fresh page");
        putReg(imageA, MI_MODE_VALUE, 0x12340100);
        check(resetStopRing(e.engine, imageA, e.bytes) == Status::Ok &&
              reg(imageA, MI_MODE_VALUE) == 0x13340000, "lrc_reset_regs clears STOP_RING");
        putReg(imageA, MI_MODE_VALUE - 1, 0);
        snap(imageA, e.bytes);
        check(resetStopRing(e.engine, imageA, e.bytes) == Status::LayoutMismatch &&
              unchanged(imageA, e.bytes), "reset refuses a missing MI_MODE slot");
        check(scrubRegisterState(spec, imageA, e.bytes - 4096) == Status::InvalidBuffer &&
              unchanged(imageA, e.bytes), "scrub size checked");
    }
}

void decoderRejects() {
    const EngineCase &e = engines[2];
    const ContextImageSpec spec = specFor(e);
    initContextImage(spec, nullptr, 0, imageA, e.bytes);
    RegisterState state;
    state.ringHead = 0xBEEF;
    const unsigned corrupt[] = {0x01, 0x02, 0x21, 0x32, 0x41, 0x42, 0x51, 0xa8};
    for (unsigned index : corrupt) {
        const uint32_t saved = reg(imageA, index);
        putReg(imageA, index, saved ^ 0x1000);
        check(decodeRegisterState(e.engine, imageA, e.bytes, state) == Status::LayoutMismatch &&
              state.ringHead == 0xBEEF, "corrupted header or offset detected");
        putReg(imageA, index, saved);
    }
    check(decodeRegisterState(Engine::Vcs0, imageA, e.bytes, state) == Status::LayoutMismatch,
          "BCS image is not a VCS image");
    check(decodeRegisterState(Engine::Rcs0, imageA, e.bytes, state) == Status::InvalidBuffer,
          "XCS-sized image is not an RCS image");
    putReg(imageA, 0xaa, 0);
    check(decodeRegisterState(e.engine, imageA, e.bytes, state) == Status::Ok && !state.listClosed,
          "open list reported");
}

void descriptors() {
    DescriptorFields fields;
    check(decodeDescriptor(Engine::Vcs0, 0x000000200010011DULL, fields) == Status::Ok &&
          fields.lrca == 0x00100000 && fields.contextId == 0x20 && fields.forceRestore &&
          !fields.hasPriority, "descriptor decode");
    check(decodeDescriptor(Engine::Rcs0, 0x0000002000100519ULL, fields) == Status::Ok &&
          fields.hasPriority && fields.priority == Priority::High && !fields.forceRestore,
          "RCS priority field");
    check(decodeDescriptor(Engine::Rcs0, 0x0000002000100119ULL, fields) == Status::Ok &&
          fields.hasPriority && fields.priority == Priority::Low, "RCS low priority is zero");
    struct Bad { Engine engine; uint64_t value; Status status; const char *what; };
    const Bad bad[] = {
        {Engine::Vcs0, 0x0000002000100118ULL, Status::InvalidDescriptor, "VALID clear"},
        {Engine::Vcs0, 0x000000200010011BULL, Status::InvalidDescriptor, "FORCE_PD_RESTORE"},
        {Engine::Vcs0, 0x0000002000100109ULL, Status::InvalidDescriptor, "legacy 32-bit mode"},
        {Engine::Vcs0, 0x0000002000100101ULL, Status::InvalidDescriptor, "advanced mode"},
        {Engine::Vcs0, 0x0000002000100111ULL, Status::InvalidDescriptor, "advanced AD mode"},
        {Engine::Vcs0, 0x0000002000100139ULL, Status::InvalidDescriptor, "L3LLC on Gen9"},
        {Engine::Vcs0, 0x0000002000100159ULL, Status::InvalidDescriptor, "bit 6"},
        {Engine::Vcs0, 0x0000002000100199ULL, Status::InvalidDescriptor, "bit 7"},
        {Engine::Vcs0, 0x0000002000100019ULL, Status::InvalidDescriptor, "PRIVILEGE clear"},
        {Engine::Vcs0, 0x0000002000100319ULL, Status::InvalidDescriptor, "priority on VCS"},
        {Engine::Rcs0, 0x0000002000100719ULL, Status::InvalidDescriptor, "priority 3"},
        {Engine::Vcs0, 0x0000002000100919ULL, Status::InvalidDescriptor, "bit 11"},
        {Engine::Vcs0, 0x0000002000000119ULL, Status::InvalidContextAddress, "LRCA 0"},
        {Engine::Vcs0, 0x0000000000100119ULL, Status::InvalidContextId, "context ID 0"},
        {Engine::Vcs0, 0x0020000000100119ULL, Status::InvalidDescriptor, "MBZ bit 53"},
        {Engine::Vcs0, 0x0040000000100119ULL, Status::InvalidDescriptor, "MBZ bit 54"},
        {Engine::Vcs0, 0x8000002000100119ULL, Status::InvalidDescriptor, "group ID"},
        {static_cast<Engine>(5), 0x0000002000100119ULL, Status::UnsupportedEngine, "engine"},
    };
    for (const Bad &b : bad) {
        fields.lrca = 0x77;
        check(decodeDescriptor(b.engine, b.value, fields) == b.status && fields.lrca == 0x77, b.what);
    }
    check(decodeDescriptor(Engine::Vcs0, 0x001FFFFF00100119ULL, fields) == Status::Ok &&
          fields.contextId == 0x1FFFFF, "21-bit context ID");
    uint32_t ccid = 0;
    check(contextIdForTag(0, ccid) == Status::Ok && ccid == 0x20, "tag 0 -> 1 << 5");
    check(contextIdForTag(1, ccid) == Status::Ok && ccid == 0x40, "tag 1 -> 2 << 5");
    check(contextIdForTag(62, ccid) == Status::Ok && ccid == 0x7E0, "tag 62 -> 63 << 5");
    ccid = 9;
    check(contextIdForTag(63, ccid) == Status::InvalidContextId && ccid == 9, "tag 63 is not allocatable");
}

void submissions() {
    const EngineCase &v = engines[1];
    const ContextImageSpec spec = specFor(v);
    initContextImage(spec, nullptr, 0, imageA, v.bytes);
    uint32_t lrca = 0;
    updateContextRegisters(spec, {RingGgtt, RingBytes, 0x40, 0x80}, ContextGgtt, imageA, v.bytes, lrca);
    SubmitState state = {lrca, 0x20};
    uint64_t desc = 0;
    struct Step { uint32_t head, tail; uint64_t expected; const char *what; };
    // intel_ring_direction(): (next - prev) << (32 - log2(0x4000)), signed.
    const Step steps[] = {
        {0x40, 0xC0, 0x000000200010011DULL, "first submission keeps the pin FORCE_RESTORE"},
        {0x40, 0xC0, 0x000000200010011DULL, "same tail forces restore"},
        {0x40, 0x100, 0x0000002000100119ULL, "forward tail is a lite restore"},
        {0x40, 0x80, 0x000000200010011DULL, "backward tail forces restore"},
        {0x00, 0x2078, 0x0000002000100119ULL, "forward by less than half the ring"},
    };
    for (const Step &s : steps) {
        check(prepareSubmission(v.engine, RingBytes, s.head, s.tail, Priority::Normal, state,
                                imageA, v.bytes, desc) == Status::Ok && desc == s.expected &&
              reg(imageA, CTX_RING_TAIL) == s.tail && state.lrca == 0x00100119, s.what);
    }
    check(prepareSubmission(v.engine, RingBytes, 0, 0x3FF8, Priority::Normal, state, imageA,
                            v.bytes, desc) == Status::Ok && desc == 0x0000002000100119ULL,
          "forward to the ring end");
    check(prepareSubmission(v.engine, RingBytes, 0x40, 0x10, Priority::Normal, state, imageA,
                            v.bytes, desc) == Status::Ok && desc == 0x0000002000100119ULL,
          "forward across the wrap");
    check(prepareSubmission(v.engine, RingBytes, 0, 0x2010, Priority::Normal, state, imageA,
                            v.bytes, desc) == Status::Ok && desc == 0x000000200010011DULL,
          "exactly half the ring counts as backward");
    // Rejections leave image, state and descriptor untouched.
    struct Bad { uint32_t ringBytes, head, tail; Status status; const char *what; };
    const Bad bad[] = {
        {RingBytes, 0x1C8, 0x1C0, Status::TailInHeadCacheline, "tail behind head in its cacheline"},
        {RingBytes, 0, 0x4000, Status::InvalidRingOffset, "tail == ring size"},
        {RingBytes, 0, 0x104, Status::InvalidRingOffset, "tail not qword aligned"},
        {RingBytes, 0x4000, 0x100, Status::InvalidRingOffset, "head outside ring"},
        {0x3000, 0, 0x100, Status::InvalidRing, "ring size not a power of two"},
        {0x1000, 0, 0x100, Status::InvalidRingOffset, "image tail outside a smaller ring"},
    };
    for (const Bad &b : bad) {
        snap(imageA, v.bytes);
        const SubmitState before = state;
        desc = 0x55;
        check(prepareSubmission(v.engine, b.ringBytes, b.head, b.tail, Priority::Normal, state,
                                imageA, v.bytes, desc) == b.status && desc == 0x55 &&
              unchanged(imageA, v.bytes) && state.lrca == before.lrca &&
              state.contextId == before.contextId, b.what);
    }
    check(prepareSubmission(v.engine, RingBytes, 0x108, 0x130, Priority::Normal, state, imageA,
                            v.bytes, desc) == Status::Ok, "tail ahead of head in one cacheline");
    SubmitState broken = {0x00100118, 0x20};
    snap(imageA, v.bytes);
    check(prepareSubmission(v.engine, RingBytes, 0, 0x200, Priority::Normal, broken, imageA,
                            v.bytes, desc) == Status::InvalidDescriptor && unchanged(imageA, v.bytes),
          "invalid persistent descriptor refused");
    broken = {0x00100319, 0x20};
    check(prepareSubmission(Engine::Rcs0, RingBytes, 0, 0x200, Priority::Normal, broken, imageA,
                            v.bytes, desc) == Status::InvalidBuffer,
          "XCS-sized image refused for RCS");
    // RCS priority mapping on GLK.
    const ContextImageSpec rcs = specFor(engines[0]);
    initContextImage(rcs, nullptr, 0, imageB, RcsBytes);
    updateContextRegisters(rcs, {RingGgtt, RingBytes, 0, 0x80}, 0x00400000, imageB, RcsBytes, lrca);
    SubmitState r = {lrca | 0x200, 0x40};
    snap(imageB, RcsBytes);
    check(prepareSubmission(Engine::Rcs0, RingBytes, 0, 0x100, Priority::Normal, r, imageB,
                            RcsBytes, desc) == Status::InvalidDescriptor && unchanged(imageB, RcsBytes),
          "priority is per submission, never in the persistent descriptor");
    r.lrca = lrca;
    check(prepareSubmission(Engine::Rcs0, RingBytes, 0, 0x100, Priority::Normal, r, imageB,
                            RcsBytes, desc) == Status::Ok && desc == 0x000000400040031DULL,
          "RCS normal priority sets bit 9");
    check(prepareSubmission(Engine::Rcs0, RingBytes, 0, 0x180, Priority::High, r, imageB,
                            RcsBytes, desc) == Status::Ok && desc == 0x0000004000400519ULL,
          "RCS high priority sets bit 10");
    check(prepareSubmission(Engine::Rcs0, RingBytes, 0, 0x200, Priority::Low, r, imageB,
                            RcsBytes, desc) == Status::Ok && desc == 0x0000004000400119ULL,
          "RCS low priority leaves the field zero");
    check(prepareSubmission(v.engine, RingBytes, 0, 0x300, Priority::High, state, imageA,
                            v.bytes, desc) == Status::Ok && (desc & 0x600) == 0,
          "priority ignored without I915_ENGINE_HAS_EU_PRIORITY");
    putReg(imageA, CTX_RING_TAIL - 1, 0);
    snap(imageA, v.bytes);
    check(prepareSubmission(v.engine, RingBytes, 0, 0x400, Priority::Normal, state, imageA,
                            v.bytes, desc) == Status::LayoutMismatch && unchanged(imageA, v.bytes),
          "submission refuses a missing RING_TAIL slot");
}

void unwind() {
    SubmitState s = {0x00100119, 0x20};
    check(markUnwind(RingBytes, 0x80, 0x100, s) == Status::Ok && s.lrca == 0x00100119,
          "short rollback keeps lite restore");
    check(markUnwind(RingBytes, 0x1000, 0x100, s) == Status::Ok && s.lrca == 0x0010011D,
          "rollback that wraps forces restore");
    s.lrca = 0x00100119;
    check(markUnwind(RingBytes, 0x10, 0x3FF8, s) == Status::Ok && s.lrca == 0x0010011D,
          "ring->tail + 8 may equal the ring size");
    s.lrca = 0x00100119;
    check(markUnwind(RingBytes, 0x84, 0x100, s) == Status::InvalidRingOffset &&
          markUnwind(RingBytes, 0x80, 0x4000, s) == Status::InvalidRingOffset &&
          markUnwind(0x3000, 0x80, 0x100, s) == Status::InvalidRing && s.lrca == 0x00100119,
          "unwind offsets validated");
}

void submissionRejectsRingControlDrift() {
    const EngineCase &e = engines[2];
    const auto spec = specFor(e);
    check(initContextImage(spec, nullptr, 0, imageA, e.bytes) == Status::Ok,
          "initialize ring-control drift fixture");
    uint32_t lrca = 0;
    check(updateContextRegisters(spec, {RingGgtt, RingBytes, 0, 0x80}, ContextGgtt,
                                 imageA, e.bytes, lrca) == Status::Ok, "initialize actual ring size");
    SubmitState state = {lrca, 0x20};
    uint64_t descriptor = 0xaabbccddeeff0011ULL;
    snap(imageA, e.bytes);
    // The image still programs 16KiB. Merely accepting a caller's 32KiB bound
    // would publish a tail using a different wrap/size than this context owns.
    check(prepareSubmission(e.engine, RingBytes * 2, 0, 0x100, Priority::Normal,
                            state, imageA, e.bytes, descriptor) == Status::RingMismatch &&
          unchanged(imageA, e.bytes) && state.lrca == lrca &&
          descriptor == 0xaabbccddeeff0011ULL, "caller/image ring size mismatch is atomic refusal");
}

void elsp() {
    ElspPlan plan;
    check(planElspSubmission(Engine::Vcs0, 0x0000002000100119ULL, 0, plan) == Status::Ok &&
          plan.writes[0].offset == 0x12230 && plan.writes[0].value == 0 &&
          plan.writes[1].offset == 0x12230 && plan.writes[1].value == 0 &&
          plan.writes[2].offset == 0x12230 && plan.writes[2].value == 0x20 &&
          plan.writes[3].offset == 0x12230 && plan.writes[3].value == 0x00100119,
          "single port: empty port 1 then port 0, upper dword first");
    check(planElspSubmission(Engine::Rcs0, 0x000000200010031DULL, 0x0000004000500519ULL, plan) ==
              Status::Ok && plan.writes[0].offset == 0x2230 && plan.writes[0].value == 0x40 &&
          plan.writes[1].value == 0x00500519 && plan.writes[2].value == 0x20 &&
          plan.writes[3].value == 0x0010031D, "two ports in reverse order");
    plan.writes[0].value = 0x99;
    check(planElspSubmission(Engine::Vcs0, 0, 0x0000002000100119ULL, plan) == Status::EmptyPort0 &&
          planElspSubmission(Engine::Vcs0, 0x0000002000100119ULL, 0x0000004000100119ULL, plan) ==
              Status::DuplicateContext &&
          planElspSubmission(Engine::Vcs0, 0x0000002000100119ULL, 0x0000002000500119ULL, plan) ==
              Status::DuplicateContext &&
          planElspSubmission(Engine::Vcs0, 0x0000002000100119ULL, 0x0000004000500118ULL, plan) ==
              Status::InvalidDescriptor &&
          planElspSubmission(Engine::Vcs0, 0x0000002000100519ULL, 0, plan) == Status::InvalidDescriptor &&
          planElspSubmission(static_cast<Engine>(4), 0x0000002000100119ULL, 0, plan) ==
              Status::UnsupportedEngine && plan.writes[0].value == 0x99,
          "assert_pending_valid() rejections leave the plan unchanged");
}

void csb() {
    CsbReset reset;
    check(planCsbReset(Engine::Vecs0, reset) == Status::Ok && reset.statusPointer.offset == 0x1a3a0 &&
          reset.statusPointer.value == 0xFFFF0505 && reset.head == 5 && reset.writePointerDword == 5 &&
          reset.entryFill == 0xFFFFFFFFFFFFFFFFULL, "reset_csb_pointers() plan");
    struct WalkCase { uint8_t head; uint32_t pointer; uint8_t count; uint8_t first; uint8_t last; };
    const WalkCase walks[] = {
        {5, 5, 0, 0, 0}, {5, 0, 1, 0, 0}, {5, 2, 3, 0, 2}, {3, 2, 5, 4, 2}, {2, 1, 5, 3, 1},
        {0, 5, 5, 1, 5}, {5, 0x105, 0, 0, 0}, {4, 0xFFFFFF00, 2, 5, 0},
    };
    for (const WalkCase &w : walks) {
        CsbWalk walk;
        check(walkCsb(w.head, w.pointer, walk) == Status::Ok && walk.count == w.count &&
              walk.newHead == static_cast<uint8_t>(w.pointer) &&
              (w.count == 0 || (walk.indices[0] == w.first && walk.indices[w.count - 1] == w.last)),
              "process_csb() walk order");
    }
    CsbWalk walk;
    check(walkCsb(3, 2, walk) == Status::Ok && walk.indices[0] == 4 && walk.indices[1] == 5 &&
          walk.indices[2] == 0 && walk.indices[3] == 1 && walk.indices[4] == 2, "wrap at 6 entries");
    walk.newHead = 9;
    check(walkCsb(0, 6, walk) == Status::InvalidCsbPointer &&
          walkCsb(0, 7, walk) == Status::InvalidCsbPointer &&
          walkCsb(6, 0, walk) == Status::InvalidCsbPointer && walk.newHead == 9,
          "reserved pointer values 6 and 7");
    CsbEvent event;
    check(decodeCsbEntry(0x0000002000000001ULL, event) == Status::Ok && event.promote &&
          event.idleToActive && !event.complete && event.contextId == 0x20 && event.status == 1,
          "idle-to-active promotes");
    check(decodeCsbEntry(0x0000004000000012ULL, event) == Status::Ok && event.promote &&
          event.preempted && event.complete && !event.idleToActive, "preemption promotes");
    check(decodeCsbEntry(0x0000004000000014ULL, event) == Status::Ok && !event.promote &&
          event.complete && event.elementSwitch, "element switch completes port 0");
    check(decodeCsbEntry(0x0000002000000018ULL, event) == Status::Ok && !event.promote &&
          event.activeToIdle && event.complete, "active-to-idle");
    check(decodeCsbEntry(0x0000002000008000ULL, event) == Status::Ok && event.liteRestore &&
          !event.promote, "lite restore bit");
    check(decodeCsbEntry(0xFFFFFFFFFFFFFFFEULL, event) == Status::Ok && event.contextId == 0xFFFFFFFF,
          "only all-ones is the unwritten marker");
    event.contextId = 3;
    check(decodeCsbEntry(0xFFFFFFFFFFFFFFFFULL, event) == Status::CsbEntryNotWritten &&
          event.contextId == 3, "all-ones entry is not decoded");
    uint32_t offset = 0;
    check(csbEntryMmioOffset(Engine::Rcs0, 0, offset) == Status::Ok && offset == 0x2370 &&
          csbEntryMmioOffset(Engine::Bcs0, 5, offset) == Status::Ok && offset == 0x22398,
          "GEN8_EXECLISTS_STATUS_BUF + 8 * index");
    check(csbEntryMmioOffset(Engine::Rcs0, 6, offset) == Status::InvalidCsbPointer && offset == 0x22398,
          "no Gen11 second status buffer");
}

void csbAssignmentIdentity() {
    uint32_t assigned = 0;
    check(contextIdForTag(0, assigned) == Status::Ok && assigned == 0x20,
          "encode the caller-retained live assignment");
    CsbEvent event;
    check(decodeCsbEntry((uint64_t(assigned) << 32) | 0x18, event) == Status::Ok &&
          checkCsbContext(event, assigned) == Status::Ok, "matching assigned CSB identity");
    check(checkCsbContext(event, 0x40) == Status::CsbIdentityMismatch,
          "foreign live context is not this submission");
    check(checkCsbContext(event, 0) == Status::InvalidContextId &&
          checkCsbContext(event, 0x200000) == Status::InvalidContextId,
          "invalid expected assignment cannot authorize an event");
    const uint32_t invalidIds[] = {0U, 0x200000U, 0xffffffffU};
    for (uint32_t invalid : invalidIds) {
        event.contextId = invalid;
        check(checkCsbContext(event, assigned) == Status::InvalidContextId,
              "zero and out-of-range CSB tags are not assignments");
    }
}
} // namespace

int main() {
    engineTable();
    freshImageLayout();
    updateAndDescriptor();
    updateRejects();
    initRejects();
    defaultStateImage();
    scrubAndStopRing();
    decoderRejects();
    descriptors();
    submissions();
    unwind();
    submissionRejectsRingControlDrift();
    elsp();
    csb();
    csbAssignmentIdentity();
    if (failures) {
        fprintf(stderr, "native_glk_lrc_tests: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("native_glk_lrc_tests: %u checks passed\n", checks);
    return 0;
}

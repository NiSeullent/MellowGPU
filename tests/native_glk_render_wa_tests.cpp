// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Hosted tests for Drivers/NativeGlk/RenderWorkarounds.cpp. Expected words and
// topology values come from native_glk_render_wa_oracle.inc, which
// Tools/run-native-glk-render-wa-tests.py generates from the pinned Linux
// source text. Fuse values are synthetic test inputs, not readings of any part.
// No device, register, DMA or GPU access happens here.
#include "Drivers/NativeGlk/RenderWorkarounds.hpp"
#include "native_glk_render_wa_oracle.inc"
#include <cstdio>
#include <cstring>

using namespace MellowNativeGlk;
using RenderWa::Status;

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr uint8_t Fill = 0xa5;

RenderWa::PciIdentity target(uint8_t revision = 6) {
    RenderWa::PciIdentity id;
    id.present = true; id.vendor = 0x8086; id.device = 0x3185; id.revision = revision;
    id.subsystemVendor = 0x17aa; id.subsystem = 0x3e77;
    return id;
}
RenderWa::FuseReadings fuses(uint32_t fuse2, uint32_t euDisable0) {
    RenderWa::FuseReadings f; f.present = true; f.fuse2 = fuse2; f.euDisable0 = euDisable0; return f;
}
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool filled(const uint8_t *p, size_t bytes, uint8_t value) {
    for (size_t i = 0; i < bytes; ++i) if (p[i] != value) return false;
    return true;
}
template <typename T> bool sameBytes(const T &a, const T &b) { return !std::memcmp(&a, &b, sizeof(T)); }
template <typename T> T filledStruct() { T value; std::memset(&value, Fill, sizeof value); return value; }
// Field comparison: independently produced records may differ in padding.
bool sameWa(const RenderWa::WaContext &a, const RenderWa::WaContext &b) {
    return a.indirectOffset == b.indirectOffset && a.indirectBytes == b.indirectBytes &&
           a.perCtxOffset == b.perCtxOffset && a.perCtxBytes == b.perCtxBytes &&
           a.mediaPoolState == b.mediaPoolState && a.render.indirectCtxGgtt == b.render.indirectCtxGgtt &&
           a.render.indirectCtxBytes == b.render.indirectCtxBytes &&
           a.render.powerClockState == b.render.powerClockState;
}

RenderWa::Topology decoded(uint32_t fuse2, uint32_t euDisable0, uint8_t revision = 6) {
    RenderWa::Topology t;
    CHECK(RenderWa::decodeTopology(target(revision), fuses(fuse2, euDisable0), t) == Status::Ok);
    return t;
}

void testIdentity() {
    for (const auto &c : Oracle::stepCases) {
        RenderWa::Platform p;
        CHECK(RenderWa::identifyPlatform(target(c.revision), p) == Status::Ok);
        CHECK(p.valid);
        CHECK((p.step == RenderWa::GraphicsStep::Future) == c.future);
        CHECK(p.revisionInTable == c.inTable);
    }
    CHECK(Oracle::targetDeviceInGlkIds);
    RenderWa::Platform keep = filledStruct<RenderWa::Platform>();
    const RenderWa::Platform before = keep;
    RenderWa::PciIdentity id = target(); id.present = false;
    CHECK(RenderWa::identifyPlatform(id, keep) == Status::MissingIdentity);
    id = target(); id.device = 0x3184; // GLK in Linux, not the designated target
    CHECK(RenderWa::identifyPlatform(id, keep) == Status::UnsupportedDevice);
    id = target(); id.device = 0x5a85;
    CHECK(RenderWa::identifyPlatform(id, keep) == Status::UnsupportedDevice);
    id = target(); id.vendor = 0x8087;
    CHECK(RenderWa::identifyPlatform(id, keep) == Status::UnsupportedDevice);
    CHECK(sameBytes(keep, before));
}

void testTopologyOracle() {
    for (const auto &c : Oracle::topologyCases) {
        RenderWa::Topology t;
        CHECK(RenderWa::decodeTopology(target(), fuses(c.fuse2, c.euDisable0), t) == Status::Ok);
        CHECK(RenderWa::checkTopology(t) == Status::Ok);
        CHECK(t.maxSlices == 1 && t.maxSubslices == 3 && t.maxEusPerSubslice == 8);
        CHECK(t.sliceMask == c.sliceMask);
        CHECK(t.subsliceMask == c.subsliceMask);
        for (unsigned ss = 0; ss < 3; ++ss) CHECK(t.euMask[ss] == c.euMask[ss]);
        CHECK(t.subslice7Eu == c.subslice7Eu);
        CHECK(t.euTotal == c.euTotal);
        CHECK(t.euPerSubslice == c.euPerSubslice);
        CHECK(t.hasSlicePg == c.hasSlicePg);
        CHECK(t.hasSubslicePg == c.hasSubslicePg);
        CHECK(t.hasEuPg == c.hasEuPg);
        CHECK(t.hasPooledEu == c.hasPooledEu);
        CHECK(t.minEuInPool == c.minEuInPool);
        RenderWa::SseuRequest r;
        CHECK(RenderWa::defaultSseu(t, r) == Status::Ok);
        CHECK(r.sliceMask == c.sliceMask && r.subsliceMask == c.subsliceMask);
        CHECK(r.minEusPerSubslice == 8 && r.maxEusPerSubslice == 8);
        uint32_t rpcs = 0;
        CHECK(RenderWa::makeRpcs(t, rpcs) == Status::Ok);
        CHECK(rpcs == c.rpcs);
        if (rpcs != c.rpcs) std::fprintf(stderr, "  case %s: rpcs %08x expected %08x\n", c.name, rpcs, c.rpcs);
        // Values Lrc::initContextImage() accepts for R_PWR_CLK_STATE.
        CHECK(!(rpcs & ~0x80078fffU));
    }
}

void testTopologyRefusals() {
    struct Case { const char *name; RenderWa::PciIdentity id; RenderWa::FuseReadings f; Status expected; };
    RenderWa::FuseReadings absent = fuses(1u << 25, 0); absent.present = false;
    RenderWa::PciIdentity noId = target(); noId.present = false;
    RenderWa::PciIdentity otherGlk = target(); otherGlk.device = 0x3184;
    const Case cases[] = {
        {"identity absent", noId, fuses(1u << 25, 0), Status::MissingIdentity},
        {"other GLK id", otherGlk, fuses(1u << 25, 0), Status::UnsupportedDevice},
        {"fuses absent", target(), absent, Status::MissingFuses},
        {"all-zero FUSE2", target(), fuses(0, 0), Status::NoRenderSlice},
        {"all-ones FUSE2", target(), fuses(0xffffffffU, 0), Status::InconsistentFuses},
        {"slice 1 only", target(), fuses(1u << 26, 0), Status::InconsistentFuses},
        {"slices 0 and 2", target(), fuses(5u << 25, 0), Status::InconsistentFuses},
        {"all subslices fused", target(), fuses(1u << 25 | 7u << 20, 0), Status::NoRenderSubslice},
        {"all EUs fused", target(), fuses(1u << 25, 0x00ffffffU), Status::NoRenderEu},
        {"all-ones EU_DISABLE", target(), fuses(1u << 25, 0xffffffffU), Status::NoRenderEu},
        {"enabled subslice EUs fused", target(), fuses(1u << 25 | 6u << 20, 0x000000ffU), Status::NoRenderEu},
    };
    for (const auto &c : cases) {
        RenderWa::Topology t = filledStruct<RenderWa::Topology>();
        const RenderWa::Topology before = t;
        const Status s = RenderWa::decodeTopology(c.id, c.f, t);
        CHECK(s == c.expected);
        if (s != c.expected) std::fprintf(stderr, "  case %s: status %u\n", c.name, unsigned(s));
        CHECK(sameBytes(t, before));
    }
    // Disabled-subslice EU bits are ignored, as Linux skips those subslices.
    const RenderWa::Topology t = decoded(1u << 25 | 4u << 20, 0x00ff0000U);
    CHECK(t.subsliceMask == 3 && t.euTotal == 16 && t.euMask[2] == 0);
}

void testTamperedTopology() {
    const RenderWa::Topology good = decoded(1u << 25, 0x00c0c0c0U);
    RenderWa::Topology t;
    uint32_t rpcs = 0x12345678;
    RenderWa::SseuRequest r = filledStruct<RenderWa::SseuRequest>();
    const RenderWa::SseuRequest rBefore = r;
    // A full GPU claimed over a 2x6 fuse reading is refused by every consumer.
    t = decoded(1u << 25 | 4u << 20, 0x00c0c0c0U); t.subsliceMask = 7; t.hasPooledEu = true;
    CHECK(RenderWa::checkTopology(t) == Status::InvalidTopology);
    CHECK(RenderWa::makeRpcs(t, rpcs) == Status::InvalidTopology && rpcs == 0x12345678);
    CHECK(RenderWa::defaultSseu(t, r) == Status::InvalidTopology && sameBytes(r, rBefore));
    t = good; t.hasPooledEu = false;
    CHECK(RenderWa::checkTopology(t) == Status::InvalidTopology);
    t = good; t.fuses.fuse2 |= 4u << 20;
    CHECK(RenderWa::checkTopology(t) == Status::InvalidTopology);
    t = good; t.identity.device = 0x3184;
    CHECK(RenderWa::checkTopology(t) == Status::UnsupportedDevice);
    t = good; t.platform.step = RenderWa::GraphicsStep::B0;
    CHECK(RenderWa::checkTopology(t) == Status::InvalidTopology);
    t = good; t.fuses.present = false;
    CHECK(RenderWa::checkTopology(t) == Status::MissingFuses);
    CHECK(RenderWa::checkTopology(RenderWa::Topology {}) == Status::MissingIdentity);
    t = good; t.minEuInPool = 3;
    CHECK(RenderWa::checkTopology(t) == Status::InvalidTopology);
}

void testWaContextOracle() {
    static uint8_t page[RenderWa::WaContextBytes];
    for (const auto &c : Oracle::batchCases) {
        const auto &tc = Oracle::topologyCases[c.topology];
        const RenderWa::Topology t = decoded(tc.fuse2, tc.euDisable0);
        std::memset(page, Fill, sizeof page);
        RenderWa::WaContext wa;
        const RenderWa::WaPlacement placement {c.waContextGgtt, c.scratchGgtt};
        CHECK(RenderWa::buildWaContext(t, placement, page, sizeof page, wa) == Status::Ok);
        for (unsigned i = 0; i < c.dwordCount; ++i) {
            CHECK(le32(page + 4*i) == c.dwords[i]);
            if (le32(page + 4*i) != c.dwords[i])
                std::fprintf(stderr, "  case %s dword %u: %08x expected %08x\n", c.name, i, le32(page + 4*i), c.dwords[i]);
        }
        CHECK(filled(page + 4*c.dwordCount, sizeof page - 4*c.dwordCount, 0));
        CHECK(wa.indirectOffset == 0 && wa.indirectBytes == c.indirectBytes);
        CHECK(wa.perCtxOffset == c.indirectBytes && wa.perCtxBytes == 0);
        CHECK(wa.indirectBytes % 64 == 0 && 4*c.dwordCount == wa.indirectBytes);
        CHECK(wa.mediaPoolState == c.mediaPool);
        CHECK(wa.render.indirectCtxGgtt == c.waContextGgtt);
        CHECK(wa.render.indirectCtxBytes == c.indirectBytes);
        CHECK(wa.render.powerClockState == tc.rpcs);
        // Revision B0 (in table) and the target's rev 06 (STEP_FUTURE) give
        // identical bytes: no GLK producer here is stepping gated.
        static uint8_t again[RenderWa::WaContextBytes];
        RenderWa::WaContext wa2;
        CHECK(RenderWa::buildWaContext(decoded(tc.fuse2, tc.euDisable0, 3), placement, again,
                                       sizeof again, wa2) == Status::Ok);
        CHECK(!std::memcmp(page, again, sizeof page) && sameWa(wa, wa2));
    }
}

void testWaContextRefusals() {
    static uint8_t page[RenderWa::WaContextBytes + 8];
    const RenderWa::Topology t = decoded(1u << 25, 0x00c0c0c0U);
    struct Case { const char *name; RenderWa::WaPlacement p; size_t bytes; Status expected; };
    const Case cases[] = {
        {"wa_ctx at 0", {0, 0x2000}, RenderWa::WaContextBytes, Status::InvalidWaContextAddress},
        {"wa_ctx unaligned", {0x1040, 0x8000}, RenderWa::WaContextBytes, Status::InvalidWaContextAddress},
        {"scratch at 0", {0x1000, 0}, RenderWa::WaContextBytes, Status::InvalidScratchAddress},
        {"scratch unaligned", {0x1000, 0x2100}, RenderWa::WaContextBytes, Status::InvalidScratchAddress},
        {"shared page", {0x7000, 0x7000}, RenderWa::WaContextBytes, Status::AddressOverlap},
        {"short object", {0x1000, 0x2000}, RenderWa::WaContextBytes - 4, Status::InvalidBuffer},
        {"long object", {0x1000, 0x2000}, RenderWa::WaContextBytes + 8, Status::InvalidBuffer},
    };
    for (const auto &c : cases) {
        std::memset(page, Fill, sizeof page);
        RenderWa::WaContext wa = filledStruct<RenderWa::WaContext>();
        const RenderWa::WaContext before = wa;
        CHECK(RenderWa::buildWaContext(t, c.p, page, c.bytes, wa) == c.expected);
        CHECK(filled(page, sizeof page, Fill) && sameBytes(wa, before));
    }
    RenderWa::WaContext wa = filledStruct<RenderWa::WaContext>();
    const RenderWa::WaContext before = wa;
    CHECK(RenderWa::buildWaContext(t, {0x1000, 0x2000}, nullptr, RenderWa::WaContextBytes, wa) == Status::InvalidBuffer);
    RenderWa::Topology bad = t; bad.hasPooledEu = false;
    CHECK(RenderWa::buildWaContext(bad, {0x1000, 0x2000}, page, RenderWa::WaContextBytes, wa) == Status::InvalidTopology);
    CHECK(RenderWa::buildWaContext(RenderWa::Topology {}, {0x1000, 0x2000}, page, RenderWa::WaContextBytes, wa) ==
          Status::MissingIdentity);
    CHECK(filled(page, sizeof page, Fill) && sameBytes(wa, before));
    // Output record inside the destination object is refused before writing.
    alignas(16) static uint8_t object[RenderWa::WaContextBytes];
    std::memset(object, Fill, sizeof object);
    auto *inside = reinterpret_cast<RenderWa::WaContext *>(object + 1024);
    CHECK(RenderWa::buildWaContext(t, {0x1000, 0x2000}, object, sizeof object, *inside) == Status::InvalidBuffer);
    CHECK(filled(object, sizeof object, Fill));
    // Topology stored inside the destination is staged before the page is cleared.
    alignas(16) static uint8_t staged[RenderWa::WaContextBytes];
    std::memset(staged, 0, sizeof staged);
    auto *inPage = reinterpret_cast<RenderWa::Topology *>(staged + 2048);
    *inPage = t;
    RenderWa::WaContext fromInside;
    CHECK(RenderWa::buildWaContext(*inPage, {0x1000, 0x2000}, staged, sizeof staged, fromInside) == Status::Ok);
    CHECK(fromInside.render.powerClockState == Oracle::topologyCases[0].rpcs);
    // Highest page of the 32-bit GGTT is a valid placement.
    RenderWa::WaContext top;
    std::memset(page, Fill, sizeof page);
    CHECK(RenderWa::buildWaContext(t, {0xfffff000U, 0x1000}, page, RenderWa::WaContextBytes, top) == Status::Ok);
}

void testLrcIntegration() {
    static uint8_t image[22 * Lrc::PageBytes];
    static uint8_t page[RenderWa::WaContextBytes];
    for (const auto &tc : Oracle::topologyCases) {
        const RenderWa::Topology t = decoded(tc.fuse2, tc.euDisable0);
        RenderWa::WaContext wa;
        CHECK(RenderWa::buildWaContext(t, {0x00ffe000U, 0x00fff000U}, page, sizeof page, wa) == Status::Ok);
        Lrc::ContextImageSpec spec;
        spec.engine = Lrc::Engine::Rcs0; spec.pml4Dma = 0x7654000; spec.render = &wa.render;
        CHECK(Lrc::initContextImage(spec, nullptr, 0, image, sizeof image) == Lrc::Status::Ok);
        Lrc::RingSpec ring; ring.ggtt = 0x00800000U; ring.bytes = 0x4000;
        uint32_t lrca = 0;
        CHECK(Lrc::updateContextRegisters(spec, ring, 0x00400000U, image, sizeof image, lrca) == Lrc::Status::Ok);
        Lrc::RegisterState regs;
        CHECK(Lrc::decodeRegisterState(Lrc::Engine::Rcs0, image, sizeof image, regs) == Lrc::Status::Ok);
        CHECK(regs.indirectCtx == (0x00ffe000U | (wa.indirectBytes / 64)));
        CHECK(regs.indirectCtxOffset == Oracle::indirectCtxOffsetDefault << 6);
        CHECK(regs.bbPerCtxPtr == 0); // per_ctx.size == 0: init_wa_bb_regs() leaves it
        CHECK(regs.slot43 == tc.rpcs);
    }
}

void testContextWorkarounds() {
    RenderWa::Platform platform;
    CHECK(RenderWa::identifyPlatform(target(), platform) == Status::Ok);
    RenderWa::ContextWorkaround list[RenderWa::ContextWorkaroundCount];
    CHECK(RenderWa::contextWorkarounds(platform, list) == Status::Ok);
    static_assert(sizeof(Oracle::contextWorkarounds) / sizeof(*Oracle::contextWorkarounds) ==
                  RenderWa::ContextWorkaroundCount, "oracle list length");
    for (unsigned i = 0; i < RenderWa::ContextWorkaroundCount; ++i) {
        CHECK(list[i].offset == Oracle::contextWorkarounds[i].offset);
        CHECK(list[i].set == Oracle::contextWorkarounds[i].set);
        CHECK(list[i].read == Oracle::contextWorkarounds[i].read);
        CHECK(list[i].mcr == Oracle::contextWorkarounds[i].mcr);
        if (i) CHECK(list[i - 1].offset < list[i].offset);
    }
    static_assert(sizeof(Oracle::contextWorkaroundRing) / 4 == RenderWa::ContextWorkaroundRingDwords,
                  "oracle ring length");
    uint8_t ring[RenderWa::ContextWorkaroundRingDwords * 4 + 16];
    std::memset(ring, Fill, sizeof ring);
    size_t written = 0;
    CHECK(RenderWa::emitContextWorkarounds(platform, ring, sizeof ring, written) == Status::Ok);
    CHECK(written == sizeof(Oracle::contextWorkaroundRing) && written % 8 == 0);
    for (unsigned i = 0; i < RenderWa::ContextWorkaroundRingDwords; ++i) {
        CHECK(le32(ring + 4*i) == Oracle::contextWorkaroundRing[i]);
        if (le32(ring + 4*i) != Oracle::contextWorkaroundRing[i])
            std::fprintf(stderr, "  ring dword %u: %08x expected %08x\n", i, le32(ring + 4*i), Oracle::contextWorkaroundRing[i]);
    }
    CHECK(filled(ring + written, sizeof ring - written, Fill));
    RenderWa::Platform b0;
    CHECK(RenderWa::identifyPlatform(target(3), b0) == Status::Ok);
    uint8_t ringB0[sizeof ring];
    std::memset(ringB0, Fill, sizeof ringB0);
    CHECK(RenderWa::emitContextWorkarounds(b0, ringB0, sizeof ringB0, written) == Status::Ok);
    CHECK(!std::memcmp(ring, ringB0, sizeof ring));
    // Refusals leave the destination and outputs unchanged.
    std::memset(ring, Fill, sizeof ring);
    written = 77;
    CHECK(RenderWa::emitContextWorkarounds(platform, ring, RenderWa::ContextWorkaroundRingDwords * 4 - 4, written) ==
          Status::InvalidBuffer);
    CHECK(RenderWa::emitContextWorkarounds(platform, nullptr, sizeof ring, written) == Status::InvalidBuffer);
    CHECK(RenderWa::emitContextWorkarounds(RenderWa::Platform {}, ring, sizeof ring, written) == Status::MissingIdentity);
    RenderWa::ContextWorkaround keep[RenderWa::ContextWorkaroundCount];
    std::memset(keep, Fill, sizeof keep);
    CHECK(RenderWa::contextWorkarounds(RenderWa::Platform {}, keep) == Status::MissingIdentity);
    CHECK(filled(reinterpret_cast<uint8_t *>(keep), sizeof keep, Fill));
    CHECK(filled(ring, sizeof ring, Fill) && written == 77);
}
} // namespace

int main() {
    testIdentity();
    testTopologyOracle();
    testTopologyRefusals();
    testTamperedTopology();
    testWaContextOracle();
    testWaContextRefusals();
    testLrcIntegration();
    testContextWorkarounds();
    if (failures) {
        std::fprintf(stderr, "native_glk_render_wa_tests: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    std::printf("native_glk_render_wa_tests: %u checks passed\n", checks);
    return 0;
}

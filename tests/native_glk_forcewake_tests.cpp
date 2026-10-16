// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. Expected Gen9 values transcribed from
// torvalds/linux 0d9ff90a intel_uncore.c / intel_gt_regs.h; see
// Drivers/NativeGlk/LICENSE.MIT and provenance.json.
//
// HOSTED INJECTED-TRANSPORT TEST. The register file below is an emulation that
// executes the production protocol bodies. It is never evidence that GLK
// hardware acknowledges forcewake, nor of any GPU acceleration.
#include "../Drivers/NativeGlk/Forcewake.hpp"
#include <cstdio>
#include <vector>

using namespace MellowNativeGlk;
using Status = ForcewakeStatus;

static int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); } } while (false)

namespace {
struct Access {
    char kind; // R read, W write, D delay
    uint32_t offset, value;
};
bool operator==(const Access &a, const Access &b) {
    return a.kind == b.kind && a.offset == b.offset && a.value == b.value;
}

// Emulated Gen9 forcewake register file: request registers have masked-write
// semantics and acknowledge registers mirror request bits unless a fault is
// injected. Simulated time advances on reads and delays only.
struct Emulated {
    uint16_t request[DomainCount] {};
    uint64_t time {1000}, readCostUs {1};
    bool logReads {};
    std::vector<Access> log;
    unsigned reads {}, relaxes {}, violations {};
    unsigned pulses[DomainCount] {}, withholdKernelUntilPulses[DomainCount] {};
    bool kernelSetNeverAcks[DomainCount] {}, kernelClearStuck[DomainCount] {};
    int allOnesAfterAckReads[DomainCount] {-1, -1, -1};
    unsigned ackReads[DomainCount] {};
    bool allOnesGtRequest {}, failReads {}, failWrites {}, failClock {}, regressClock {};

    static int domainForSet(uint32_t offset) {
        for (unsigned i = 0; i < DomainCount; ++i) if (Gen9Domains[i].set == offset) return int(i);
        return -1;
    }
    static int domainForAck(uint32_t offset) {
        for (unsigned i = 0; i < DomainCount; ++i) if (Gen9Domains[i].ack == offset) return int(i);
        return -1;
    }
    bool read(uint32_t offset, uint32_t &value) {
        ++reads; time += readCostUs;
        if (failReads) return false;
        if (logReads) log.push_back({'R', offset, 0});
        if (const int d = domainForAck(offset); d >= 0) {
            const unsigned i = unsigned(d);
            const unsigned readIndex = ackReads[i]++;
            if (allOnesAfterAckReads[i] >= 0 && readIndex >= unsigned(allOnesAfterAckReads[i])) {
                value = 0xffffffffU; return true;
            }
            uint32_t ack = request[i];
            if (kernelSetNeverAcks[i] || pulses[i] < withholdKernelUntilPulses[i]) ack &= ~ForcewakeKernel;
            if (kernelClearStuck[i]) ack |= ForcewakeKernel;
            value = ack; return true;
        }
        if (const int d = domainForSet(offset); d >= 0) {
            value = allOnesGtRequest && offset == ForcewakeGtGen9 ? 0xffffffffU : request[d];
            return true;
        }
        ++violations; value = 0; return true;
    }
    bool write(uint32_t offset, uint32_t value) {
        if (failWrites) return false;
        log.push_back({'W', offset, value});
        const int d = domainForSet(offset);
        const uint32_t mask = value >> 16, bits = value & 0xffffU;
        if (d < 0 || (bits & ~mask)) { ++violations; return true; }
        if ((mask & ForcewakeKernelFallback) && (bits & ForcewakeKernelFallback)) ++pulses[d];
        request[d] = uint16_t((request[d] & ~mask) | (bits & mask));
        return true;
    }
    ForcewakeIo io() {
        return {this,
            [](void *c, uint32_t o, uint32_t &v) { return static_cast<Emulated *>(c)->read(o, v); },
            [](void *c, uint32_t o, uint32_t v) { return static_cast<Emulated *>(c)->write(o, v); },
            [](void *c, uint64_t &now) {
                auto &e = *static_cast<Emulated *>(c);
                if (e.failClock) return false;
                now = e.regressClock ? e.time - 5 : e.time;
                return true;
            },
            [](void *c, uint32_t us) {
                auto &e = *static_cast<Emulated *>(c);
                e.log.push_back({'D', 0, us}); e.time += us;
            },
            [](void *c) { ++static_cast<Emulated *>(c)->relaxes; }};
    }
    std::vector<Access> writesOnly() const {
        std::vector<Access> out;
        for (const auto &a : log) if (a.kind == 'W') out.push_back(a);
        return out;
    }
    std::vector<uint32_t> delays() const {
        std::vector<uint32_t> out;
        for (const auto &a : log) if (a.kind == 'D') out.push_back(a.value);
        return out;
    }
};

constexpr uint32_t Set = 0x00010001, Clear = 0x00010000, FbSet = 0x80008000,
    FbClear = 0x80000000, Reset = 0xffff0000;
constexpr uint64_t Far = 1ULL << 40;

void ready(Emulated &e, Gen9Forcewake &fw) {
    CHECK(fw.bind(e.io()) == Status::Ok);
    CHECK(fw.initialize(e.time + Far) == Status::Ok);
    e.log.clear(); e.reads = 0;
}

void testEncodingsAndRegisters() {
    static_assert(maskedEnable(ForcewakeKernel) == 0x00010001U, "REG_MASKED_FIELD_ENABLE(BIT(0))");
    static_assert(maskedDisable(ForcewakeKernel) == 0x00010000U, "REG_MASKED_FIELD_DISABLE(BIT(0))");
    static_assert(maskedEnable(ForcewakeKernelFallback) == 0x80008000U, "fallback set");
    static_assert(maskedDisable(ForcewakeKernelFallback) == 0x80000000U, "fallback clear");
    static_assert(maskedDisable(ResetClearBitsGen9) == 0xffff0000U, "Gen9 fw_domain_reset");
    CHECK(maskedFieldValid(0xffff, 0x0001));
    CHECK(!maskedFieldValid(0x10000, 0));
    CHECK(!maskedFieldValid(0x0001, 0x0002));
    CHECK(Gen9Domains[0].set == 0xa278 && Gen9Domains[0].ack == 0x0d84);
    CHECK(Gen9Domains[1].set == 0xa188 && Gen9Domains[1].ack == 0x130044);
    CHECK(Gen9Domains[2].set == 0xa270 && Gen9Domains[2].ack == 0x0d88);
    CHECK(ForcewakeAckTimeoutUs == 50000 && FallbackMaxPasses == 10 && FallbackRelaxStepUs == 10);
    for (uint32_t offset : {0xa278U, 0xa188U, 0xa270U, 0x0d84U, 0x130044U, 0x0d88U})
        CHECK(isForcewakeRegister(offset));
    for (uint32_t offset : {0U, 0xa18cU, 0xa274U, 0x0d80U, 0x130040U, 0x130048U, 0x2030U})
        CHECK(!isForcewakeRegister(offset));
    CHECK(isGlkDeviceId(0x3184) && isGlkDeviceId(0x3185));
    CHECK(!isGlkDeviceId(0x5a84) && !isGlkDeviceId(0x3183) && !isGlkDeviceId(0x9a49));
}

// Independent transcription of __gen9_fw_ranges (intel_uncore.c @0d9ff90a).
struct Range { uint32_t start, end; uint8_t domains; };
constexpr uint8_t R = RenderDomain, G = GtDomain, M = MediaDomain;
const Range gen9Table[] = {
    {0x0, 0xaff, G}, {0xb00, 0x1fff, 0}, {0x2000, 0x26ff, R}, {0x2700, 0x2fff, G},
    {0x3000, 0x3fff, R}, {0x4000, 0x51ff, G}, {0x5200, 0x7fff, R}, {0x8000, 0x812f, G},
    {0x8130, 0x813f, M}, {0x8140, 0x815f, R}, {0x8160, 0x82ff, G}, {0x8300, 0x84ff, R},
    {0x8500, 0x87ff, G}, {0x8800, 0x89ff, M}, {0x8a00, 0x8bff, G}, {0x8c00, 0x8cff, R},
    {0x8d00, 0x93ff, G}, {0x9400, 0x97ff, R | M}, {0x9800, 0xafff, G}, {0xb000, 0xb47f, R},
    {0xb480, 0xcfff, G}, {0xd000, 0xd7ff, M}, {0xd800, 0xdfff, G}, {0xe000, 0xe8ff, R},
    {0xe900, 0x11fff, G}, {0x12000, 0x13fff, M}, {0x14000, 0x19fff, G}, {0x1a000, 0x1e9ff, M},
    {0x1ea00, 0x243ff, G}, {0x24400, 0x247ff, R}, {0x24800, 0x2ffff, G}, {0x30000, 0x3ffff, M},
};

void testDomainTables() {
    uint32_t next = 0;
    for (const auto &range : gen9Table) {
        CHECK(range.start == next);
        next = range.end + 1;
        for (uint32_t offset : {range.start, range.start + 4, range.end - 3, range.end}) {
            CHECK(gen9ReadDomains(offset) == range.domains);
        }
    }
    CHECK(next == 0x40000);
    // NEEDS_FORCE_WAKE false, and >= 0x116000 beyond the Gen9 table.
    for (uint32_t offset : {0x40000U, 0x100000U, 0x115ffcU, 0x116000U, 0x130044U, 0x1fffffU})
        CHECK(gen9ReadDomains(offset) == 0 && gen9WriteDomains(offset) == 0);
    // gen8_shadowed_regs: writes need no wake; reads still do.
    for (uint32_t offset : {0x2030U, 0xa008U, 0xa00cU, 0x12030U, 0x1a030U, 0x22030U})
        CHECK(gen9WriteDomains(offset) == 0 && gen9ReadDomains(offset) != 0);
    CHECK(gen9ReadDomains(0x2030) == R && gen9ReadDomains(0xa008) == G);
    CHECK(gen9ReadDomains(0x12030) == M && gen9ReadDomains(0x1a030) == M && gen9ReadDomains(0x22030) == G);
    CHECK(gen9WriteDomains(0x2034) == R && gen9WriteDomains(0xa004) == G && gen9WriteDomains(0xa010) == G);
    CHECK(gen9WriteDomains(0x9400) == (R | M));
}

void testBindAndPreconditions() {
    Emulated e; Gen9Forcewake fw; ForcewakeLease lease;
    ForcewakeIo missing = e.io(); missing.write32 = nullptr;
    CHECK(fw.bind(missing) == Status::Invalid);
    CHECK(fw.state() == Gen9Forcewake::State::Unbound);
    CHECK(fw.get(AllDomains, e.time + Far, lease) == Status::Unavailable);
    CHECK(fw.bind(e.io()) == Status::Ok);
    CHECK(fw.bind(e.io()) == Status::Busy);
    CHECK(fw.get(AllDomains, e.time + Far, lease) == Status::Unavailable);
    CHECK(e.reads == 0 && e.log.empty());
    CHECK(fw.initialize(e.time + Far) == Status::Ok);
    e.log.clear(); e.reads = 0;
    CHECK(fw.get(0, e.time + Far, lease) == Status::Invalid);
    CHECK(fw.get(0x08, e.time + Far, lease) == Status::Invalid);
    CHECK(fw.get(AllDomains, e.time, lease) == Status::DeadlineExceeded);
    CHECK(e.reads == 0 && e.log.empty() && lease.slot == UINT32_MAX);
    CHECK(fw.initialize(e.time + Far) == Status::Busy);
}

void testInitializeWritesExactResetSequence() {
    Emulated e; Gen9Forcewake fw;
    e.logReads = true;
    e.request[0] = 0x0001; e.request[1] = 0x8003; e.request[2] = 0x0002; // stale BIOS/owner bits
    CHECK(fw.bind(e.io()) == Status::Ok);
    CHECK(fw.initialize(e.time + Far) == Status::Ok);
    const std::vector<Access> expected {
        {'R', ForcewakeGtGen9, 0},
        {'W', 0xa278, Reset}, {'W', 0xa188, Reset}, {'W', 0xa270, Reset},
        {'W', 0xa278, Reset}, {'W', 0xa188, Reset}, {'W', 0xa270, Reset},
    };
    CHECK(e.log == expected);
    CHECK(e.request[0] == 0 && e.request[1] == 0 && e.request[2] == 0);
    CHECK(fw.state() == Gen9Forcewake::State::Ready && e.violations == 0);
}

void testInitializeRefusesDeadMmioBeforeWrites() {
    Emulated e; Gen9Forcewake fw;
    e.allOnesGtRequest = true;
    CHECK(fw.bind(e.io()) == Status::Ok);
    const uint64_t start = e.time;
    CHECK(fw.initialize(start + Far) == Status::MmioUnreliable);
    CHECK(e.writesOnly().empty());
    CHECK(e.time - start >= MmioSanityTimeoutUs && e.time - start < MmioSanityTimeoutUs + 5000);
    CHECK(fw.state() == Gen9Forcewake::State::Bound);
    Emulated e2; Gen9Forcewake fw2;
    e2.allOnesGtRequest = true;
    CHECK(fw2.bind(e2.io()) == Status::Ok);
    CHECK(fw2.initialize(e2.time + 3000) == Status::DeadlineExceeded);
    CHECK(e2.writesOnly().empty());
    Emulated e3; Gen9Forcewake fw3;
    e3.failReads = true;
    CHECK(fw3.bind(e3.io()) == Status::Ok);
    CHECK(fw3.initialize(e3.time + Far) == Status::IoFailure && e3.writesOnly().empty());
}

void testGetAllExactProtocolAndPutOrder() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.logReads = true;
    ForcewakeLease all;
    CHECK(fw.get(AllDomains, e.time + Far, all) == Status::Ok);
    const std::vector<Access> expected {
        {'R', 0x0d84, 0}, {'W', 0xa278, Set},
        {'R', 0x130044, 0}, {'W', 0xa188, Set},
        {'R', 0x0d88, 0}, {'W', 0xa270, Set},
        {'R', 0x0d84, 0}, {'R', 0x130044, 0}, {'R', 0x0d88, 0},
    };
    CHECK(e.log == expected);
    CHECK(fw.activeDomains() == AllDomains && all.domains == AllDomains && all.slot < Gen9Forcewake::LeaseCapacity);
    CHECK(fw.wakeCount(Domain::Render) == 1 && fw.wakeCount(Domain::Gt) == 1 && fw.wakeCount(Domain::Media) == 1);
    e.log.clear();
    ForcewakeLease render;
    CHECK(fw.get(RenderDomain, e.time + Far, render) == Status::Ok);
    CHECK(e.log.empty() && fw.wakeCount(Domain::Render) == 2 && fw.liveLeases() == 2);
    const ForcewakeLease copy = all;
    CHECK(fw.put(all) == Status::Ok);
    CHECK(all.slot == UINT32_MAX);
    CHECK((e.log == std::vector<Access> {{'W', 0xa188, Clear}, {'W', 0xa270, Clear}}));
    CHECK(fw.activeDomains() == RenderDomain);
    e.log.clear();
    ForcewakeLease stale = copy;
    CHECK(fw.put(stale) == Status::StaleLease);
    CHECK(fw.put(all) == Status::Invalid);
    ForcewakeLease forged = render; forged.domains = AllDomains;
    CHECK(fw.put(forged) == Status::Ownership);
    CHECK(e.log.empty());
    CHECK(fw.put(render) == Status::Ok);
    CHECK((e.log == std::vector<Access> {{'W', 0xa278, Clear}}));
    CHECK(fw.activeDomains() == 0 && fw.liveLeases() == 0 && e.violations == 0);
    CHECK(e.request[0] == 0 && e.request[1] == 0 && e.request[2] == 0);
}

void testFallbackRecoversDroppedAck() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.withholdKernelUntilPulses[1] = 3;
    ForcewakeLease gt;
    const uint64_t start = e.time;
    CHECK(fw.get(GtDomain, e.time + Far, gt) == Status::Ok);
    CHECK((e.writesOnly() == std::vector<Access> {
        {'W', 0xa188, Set}, {'W', 0xa188, FbSet}, {'W', 0xa188, FbClear},
        {'W', 0xa188, FbSet}, {'W', 0xa188, FbClear}, {'W', 0xa188, FbSet}, {'W', 0xa188, FbClear}}));
    CHECK((e.delays() == std::vector<uint32_t> {10, 20, 30}));
    CHECK(e.time - start >= ForcewakeAckTimeoutUs && e.time - start < 2 * ForcewakeAckTimeoutUs);
    CHECK(fw.diagnostics().fallbackInvocations == 1 && fw.diagnostics().lastFallbackPasses == 3);
    CHECK(fw.activeDomains() == GtDomain && e.relaxes > 0 && e.violations == 0);
    CHECK((e.request[1] & ForcewakeKernelFallback) == 0);
}

void testFallbackExhaustionFailsClosed() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.kernelSetNeverAcks[2] = true;
    ForcewakeLease lease;
    const uint64_t start = e.time;
    CHECK(fw.get(RenderDomain | MediaDomain, e.time + Far, lease) == Status::AckSetTimeout);
    std::vector<Access> expected {{'W', 0xa278, Set}, {'W', 0xa270, Set}};
    for (unsigned pass = 1; pass <= FallbackMaxPasses; ++pass) {
        expected.push_back({'W', 0xa270, FbSet}); expected.push_back({'W', 0xa270, FbClear});
    }
    expected.push_back({'W', 0xa278, Clear}); expected.push_back({'W', 0xa270, Clear});
    CHECK(e.writesOnly() == expected);
    CHECK((e.delays() == std::vector<uint32_t> {10, 20, 30, 40, 50, 60, 70, 80, 90, 100}));
    CHECK(e.time - start >= 2 * ForcewakeAckTimeoutUs && e.time - start < 3 * ForcewakeAckTimeoutUs);
    CHECK(fw.state() == Gen9Forcewake::State::Faulted && fw.activeDomains() == 0);
    CHECK(fw.wakeCount(Domain::Render) == 0 && fw.liveLeases() == 0 && lease.slot == UINT32_MAX);
    CHECK(fw.diagnostics().lastFailure == Status::AckSetTimeout);
    CHECK(fw.diagnostics().failedDomainMask == MediaDomain && fw.diagnostics().lastFallbackPasses == 10);
    CHECK(e.request[0] == 0 && e.request[2] == 0);
    e.log.clear(); e.reads = 0;
    ForcewakeLease again;
    CHECK(fw.get(RenderDomain, e.time + Far, again) == Status::Faulted);
    CHECK(e.reads == 0 && e.log.empty());
    uint8_t lost = 0xff;
    CHECK(fw.reset(lost) == Status::Ok && lost == 0);
    CHECK((e.writesOnly() == std::vector<Access> {{'W', 0xa278, Reset}, {'W', 0xa188, Reset}, {'W', 0xa270, Reset}}));
    CHECK(fw.state() == Gen9Forcewake::State::Bound);
    e.kernelSetNeverAcks[2] = false;
    CHECK(fw.initialize(e.time + Far) == Status::Ok);
    CHECK(fw.get(MediaDomain, e.time + Far, again) == Status::Ok && fw.put(again) == Status::Ok);
    CHECK(e.violations == 0);
}

void testStuckClearNeverRequestsDomain() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.kernelClearStuck[1] = true;
    ForcewakeLease lease;
    CHECK(fw.get(AllDomains, e.time + Far, lease) == Status::AckClearTimeout);
    std::vector<Access> expected {{'W', 0xa278, Set}};
    for (unsigned pass = 1; pass <= FallbackMaxPasses; ++pass) {
        expected.push_back({'W', 0xa188, FbSet}); expected.push_back({'W', 0xa188, FbClear});
    }
    expected.push_back({'W', 0xa278, Clear});
    CHECK(e.writesOnly() == expected);
    CHECK(fw.diagnostics().failedDomainMask == GtDomain && fw.state() == Gen9Forcewake::State::Faulted);
    CHECK(e.request[0] == 0 && e.request[1] == 0 && e.request[2] == 0);
}

void testAllOnesAckRefusedImmediately() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.allOnesAfterAckReads[0] = 0;
    ForcewakeLease lease;
    const uint64_t start = e.time;
    CHECK(fw.get(RenderDomain, e.time + Far, lease) == Status::MmioUnreliable);
    CHECK(e.writesOnly().empty() && e.time - start < 100);
    // Linux would accept ~0 as a set ack ((~0 & BIT(0)) == BIT(0)); refuse it.
    Emulated e2; Gen9Forcewake fw2; ready(e2, fw2);
    e2.allOnesAfterAckReads[0] = 1;
    CHECK(fw2.get(RenderDomain, e2.time + Far, lease) == Status::MmioUnreliable);
    CHECK((e2.writesOnly() == std::vector<Access> {{'W', 0xa278, Set}, {'W', 0xa278, Clear}}));
    CHECK(fw2.activeDomains() == 0 && fw2.state() == Gen9Forcewake::State::Faulted);
    CHECK(fw2.diagnostics().lastAck == 0xffffffffU);
}

void testDeadlineClockAndIoFailures() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    e.kernelSetNeverAcks[0] = true;
    ForcewakeLease lease;
    const uint64_t start = e.time;
    CHECK(fw.get(RenderDomain, start + 1000, lease) == Status::DeadlineExceeded);
    CHECK((e.writesOnly() == std::vector<Access> {{'W', 0xa278, Set}, {'W', 0xa278, Clear}}));
    CHECK(e.time - start < 1100 && e.request[0] == 0);

    Emulated c; Gen9Forcewake clock; ready(c, clock);
    c.regressClock = true;
    CHECK(clock.get(RenderDomain, c.time + Far, lease) == Status::ClockFailure);
    CHECK(c.log.empty() && c.reads == 0);
    Emulated f; Gen9Forcewake failedClock; ready(f, failedClock);
    f.failClock = true;
    CHECK(failedClock.get(RenderDomain, f.time + Far, lease) == Status::ClockFailure && f.reads == 0);

    Emulated w; Gen9Forcewake writes; ready(w, writes);
    w.failWrites = true;
    CHECK(writes.get(RenderDomain, w.time + Far, lease) == Status::IoFailure);
    CHECK(writes.diagnostics().rollbackFailures == 1 && writes.state() == Gen9Forcewake::State::Faulted);
    Emulated r; Gen9Forcewake readsFail; ready(r, readsFail);
    r.failReads = true;
    CHECK(readsFail.get(RenderDomain, r.time + Far, lease) == Status::IoFailure && r.writesOnly().empty());
}

void testResetUnbindAndCapacity() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    ForcewakeLease a, b;
    CHECK(fw.get(RenderDomain | GtDomain, e.time + Far, a) == Status::Ok);
    CHECK(fw.unbind(false) == Status::Busy && fw.state() == Gen9Forcewake::State::Ready);
    e.log.clear();
    uint8_t lost = 0;
    CHECK(fw.reset(lost) == Status::Ok && lost == (RenderDomain | GtDomain));
    CHECK((e.writesOnly() == std::vector<Access> {{'W', 0xa278, Clear}, {'W', 0xa188, Clear},
        {'W', 0xa278, Reset}, {'W', 0xa188, Reset}, {'W', 0xa270, Reset}}));
    e.log.clear();
    CHECK(fw.put(a) == Status::StaleLease && e.log.empty());
    CHECK(fw.get(RenderDomain, e.time + Far, b) == Status::Unavailable);
    CHECK(fw.initialize(e.time + Far) == Status::Ok);
    e.log.clear();
    ForcewakeLease leases[Gen9Forcewake::LeaseCapacity];
    for (auto &lease : leases) CHECK(fw.get(RenderDomain, e.time + Far, lease) == Status::Ok);
    CHECK(e.writesOnly().size() == 1 && fw.wakeCount(Domain::Render) == Gen9Forcewake::LeaseCapacity);
    e.log.clear(); e.reads = 0;
    CHECK(fw.get(RenderDomain, e.time + Far, b) == Status::Busy && e.reads == 0 && e.log.empty());
    CHECK(fw.unbind(false) == Status::Busy);
    CHECK(fw.unbind(true) == Status::Ok && fw.state() == Gen9Forcewake::State::Unbound);
    CHECK(fw.put(leases[0]) == Status::Unavailable && e.log.empty());
}

void testLeaseSlotReuseRejectsStaleCopy() {
    Emulated e; Gen9Forcewake fw; ready(e, fw);
    ForcewakeLease first, second;
    CHECK(fw.get(GtDomain, e.time + Far, first) == Status::Ok);
    ForcewakeLease copy = first;
    CHECK(fw.put(first) == Status::Ok);
    CHECK(fw.get(GtDomain, e.time + Far, second) == Status::Ok);
    CHECK(second.slot == copy.slot && second.generation != copy.generation);
    e.log.clear();
    CHECK(fw.put(copy) == Status::StaleLease);
    CHECK(e.log.empty() && fw.wakeCount(Domain::Gt) == 1 && fw.activeDomains() == GtDomain);
    CHECK(fw.put(second) == Status::Ok && fw.activeDomains() == 0);
}

GlkPciExpectation goodExpectation() {
    GlkPciExpectation x;
    x.device = 0x3185; x.revision = 0x06; x.subsystemVendor = 0x17aa; x.subsystemDevice = 0x3e77;
    x.bus = 0; x.slot = 2; x.function = 0; x.registryId = 0x100000123; x.epoch = 7;
    return x;
}
GlkAuthoritySnapshot goodSnapshot() {
    GlkAuthoritySnapshot s;
    s.vendor = 0x8086; s.device = 0x3185; s.classRevision = 0x03000006;
    s.subsystemVendor = 0x17aa; s.subsystemDevice = 0x3e77; s.command = 0x0006;
    s.headerType = 0; s.bus = 0; s.slot = 2; s.function = 0; s.registryId = 0x100000123; s.epoch = 7;
    s.pmCapability = true; s.pmcsr = 0x0008; s.bar0Low = 0xa0000004; s.bar0High = 0;
    s.mappedPhysical = 0xa0000000; s.mappedLength = 16 * 1024 * 1024;
    s.providerOpen = true; s.inactive = false; s.powerStateD0 = true; s.tunneled = false;
    return s;
}

void testAuthorityValidation() {
    const auto x = goodExpectation();
    CHECK(validateAuthority(x, goodSnapshot()) == Status::Ok);
    uint64_t physical = 0;
    CHECK(glkBar0Physical(goodSnapshot(), physical) && physical == 0xa0000000);
    struct Case { void (*mutate)(GlkAuthoritySnapshot &); Status expected; };
    const Case cases[] = {
        {[](GlkAuthoritySnapshot &s) { s.vendor = 0x10de; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.device = 0x3184; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.classRevision = 0x03000003; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.classRevision = 0x04800006; }, Status::Unsupported},
        {[](GlkAuthoritySnapshot &s) { s.subsystemDevice = 0x3e78; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.registryId = 1; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.epoch = 8; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.slot = 3; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.headerType = 1; }, Status::Unsupported},
        {[](GlkAuthoritySnapshot &s) { s.providerOpen = false; }, Status::Ownership},
        {[](GlkAuthoritySnapshot &s) { s.inactive = true; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.powerStateD0 = false; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.pmCapability = false; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.pmcsr = 0x0003; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.pmcsr = 0xffff; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.command = 0x0004; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.command = 0xffff; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.tunneled = true; }, Status::Unsupported},
        {[](GlkAuthoritySnapshot &s) { s.bar0Low = 0xa0000001; }, Status::Unsupported},
        {[](GlkAuthoritySnapshot &s) { s.bar0Low = 0xa0000000; }, Status::Unsupported},
        {[](GlkAuthoritySnapshot &s) { s.bar0Low = 0x00000004; s.mappedPhysical = 0; }, Status::Unavailable},
        {[](GlkAuthoritySnapshot &s) { s.mappedPhysical = 0xb0000000; }, Status::IdentityChanged},
        {[](GlkAuthoritySnapshot &s) { s.mappedLength = RegisterWindowBytes - 4; }, Status::Unavailable},
    };
    for (const auto &c : cases) {
        auto s = goodSnapshot(); c.mutate(s);
        CHECK(validateAuthority(x, s) == c.expected);
    }
    auto zeroRegistry = x;
    auto zeroSnapshot = goodSnapshot();
    zeroRegistry.registryId = zeroSnapshot.registryId = 0;
    CHECK(validateAuthority(zeroRegistry, zeroSnapshot) == Status::Invalid);
    auto zeroEpoch = x;
    zeroSnapshot = goodSnapshot();
    zeroEpoch.epoch = zeroSnapshot.epoch = 0;
    CHECK(validateAuthority(zeroEpoch, zeroSnapshot) == Status::Invalid);
    auto invalidBdf = x;
    zeroSnapshot = goodSnapshot();
    invalidBdf.slot = zeroSnapshot.slot = 32;
    CHECK(validateAuthority(invalidBdf, zeroSnapshot) == Status::Invalid);
    invalidBdf = x;
    zeroSnapshot = goodSnapshot();
    invalidBdf.function = zeroSnapshot.function = 8;
    CHECK(validateAuthority(invalidBdf, zeroSnapshot) == Status::Invalid);
    auto notGlk = goodExpectation(); notGlk.device = 0x5917;
    auto s = goodSnapshot(); s.device = 0x5917;
    CHECK(validateAuthority(notGlk, s) == Status::Unsupported);
}
} // namespace

int main() {
    testEncodingsAndRegisters();
    testDomainTables();
    testBindAndPreconditions();
    testInitializeWritesExactResetSequence();
    testInitializeRefusesDeadMmioBeforeWrites();
    testGetAllExactProtocolAndPutOrder();
    testFallbackRecoversDroppedAck();
    testFallbackExhaustionFailsClosed();
    testStuckClearNeverRequestsDomain();
    testAllOnesAckRefusedImmediately();
    testDeadlineClockAndIoFailures();
    testResetUnbindAndCapacity();
    testLeaseSlotReuseRejectsStaleCopy();
    testAuthorityValidation();
    std::printf("native_glk_forcewake_tests: %d checks, %d failures "
                "(hosted injected transport; not hardware proof)\n", checks, failures);
    return failures ? 1 : 0;
}

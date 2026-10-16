// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Hosted tests for Mellow/GlkTopologyIOKit.cpp over the configuration-space
// shim in tests/native_glk_render_wa_shim and injected binding, forcewake and
// register operations. Register values are synthetic; no device is accessed.
#include "Mellow/GlkTopologyIOKit.hpp"
#include <cstdio>
#include <cstring>

using namespace MellowNative;
namespace RenderWa = MellowNativeGlk::RenderWa;

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); } } while (0)

struct Fake {
    IOPCIDevice *device {};
    bool bound = true, forcewake = true, readOk = true, unbindOnRead = false;
    bool access = true, awake = true, unbindOnAcquire = false;
    bool releaseKnown = true, quarantined = false, unbindOnRelease = false;
    bool closeClientOnRead = false, clientAlive = true;
    unsigned revokeAfterConfig = 0;
    unsigned begins=0, ends=0, accessDepth=0, outsideAccess=0, cleanupOrderFailures=0;
    uint32_t fuse2 = 1u << 25, euDisable0 = 0x00c0c0c0U;
    unsigned boundCalls = 0, acquires = 0, releases = 0, depth = 0, reads = 0, readsOutside = 0;
    uint32_t order[4] {};
};
Fake &fake(void *c) { return *static_cast<Fake *>(c); }
void afterConfigRead(void *c) {
    Fake &f=fake(c);
    if (!f.accessDepth) ++f.outsideAccess;
    if (f.device->configReads==f.revokeAfterConfig) f.bound=false;
}
bool begin(void *c, IOPCIDevice *d) {
    Fake &f=fake(c); ++f.begins;
    if (!f.access || !f.awake || d!=f.device) return false;
    ++f.accessDepth; return true;
}
void end(void *c, IOPCIDevice *, GlkTopologyCleanup cleanup) {
    Fake &f=fake(c); ++f.ends;
    if (f.accessDepth!=1) ++f.cleanupOrderFailures;
    if (cleanup==GlkTopologyCleanup::ForcewakeUnknown) {
        f.quarantined=true; // native owner retains this same operation/PM hold
        if (f.depth!=1) ++f.cleanupOrderFailures;
    } else {
        if (f.depth) ++f.cleanupOrderFailures;
        --f.accessDepth;
    }
}
bool bound(void *c, IOPCIDevice *d) {
    Fake &f=fake(c); ++f.boundCalls;
    if (!f.accessDepth) ++f.outsideAccess;
    return f.bound && f.awake && d==f.device;
}
bool acquire(void *c) {
    ++fake(c).acquires;
    if (!fake(c).accessDepth) ++fake(c).outsideAccess;
    if (fake(c).unbindOnAcquire) fake(c).bound=false;
    if (!fake(c).forcewake) return false;
    ++fake(c).depth;
    return true;
}
bool release(void *c) {
    Fake &f=fake(c); if (!f.accessDepth) ++f.outsideAccess; ++f.releases;
    if (f.unbindOnRelease) f.bound=false;
    if (!f.releaseKnown) return false;
    --f.depth; return true;
}
bool readRegister(void *c, uint32_t offset, uint32_t &value) {
    Fake &f = fake(c);
    if (!f.depth) ++f.readsOutside;
    if (!f.accessDepth) ++f.outsideAccess;
    if (f.closeClientOnRead) f.clientAlive=false;
    if (f.reads < 4) f.order[f.reads] = offset;
    ++f.reads;
    if (f.unbindOnRead) f.bound = false;
    if (!f.readOk) return false;
    value = offset == RenderWa::Fuse2Offset ? f.fuse2 : offset == RenderWa::EuDisable0Offset ? f.euDisable0 : 0xdeadbeefU;
    return true;
}

void put16(IOPCIDevice &d, unsigned offset, uint16_t v) {
    d.config[offset] = static_cast<UInt8>(v); d.config[offset + 1] = static_cast<UInt8>(v >> 8);
}
void glk(IOPCIDevice &d, uint16_t device = 0x3185) {
    put16(d, 0x00, 0x8086); put16(d, 0x02, device); d.config[0x08] = 6;
    put16(d, 0x2c, 0x17aa); put16(d, 0x2e, 0x3e77);
}
GlkTopologyReadOps ops(Fake &f) {
    GlkTopologyReadOps o;
    o.context = &f; o.beginAccess=begin; o.endAccess=end; o.bound = bound; o.acquireGtForcewake = acquire;
    o.releaseGtForcewake = release; o.readRegister = readRegister;
    return o;
}
RenderWa::Topology poisoned() {
    RenderWa::Topology t; std::memset(&t, 0xa5, sizeof t); return t;
}
bool unchanged(const RenderWa::Topology &t) {
    const RenderWa::Topology p = poisoned();
    return !std::memcmp(&t, &p, sizeof t);
}

void testSuccess() {
    IOPCIDevice d; glk(d);
    Fake f; f.device = &d;
    RenderWa::Topology t = poisoned();
    const GlkTopologyReadResult r = glkReadTopology(&d, ops(f), t);
    CHECK(r.status == GlkTopologyReadStatus::Ok && r.decode == RenderWa::Status::Ok);
    CHECK(RenderWa::checkTopology(t) == RenderWa::Status::Ok);
    CHECK(t.identity.device == 0x3185 && t.identity.revision == 6 && t.identity.subsystemVendor == 0x17aa &&
          t.identity.subsystem == 0x3e77);
    CHECK(t.platform.step == RenderWa::GraphicsStep::Future && !t.platform.revisionInTable);
    CHECK(t.subsliceMask == 7 && t.euTotal == 18 && t.hasPooledEu);
    RenderWa::Topology direct;
    RenderWa::FuseReadings fuses; fuses.present = true; fuses.fuse2 = f.fuse2; fuses.euDisable0 = f.euDisable0;
    CHECK(RenderWa::decodeTopology(t.identity, fuses, direct) == RenderWa::Status::Ok);
    uint32_t a = 0, b = 1;
    CHECK(RenderWa::makeRpcs(t, a) == RenderWa::Status::Ok && RenderWa::makeRpcs(direct, b) == RenderWa::Status::Ok && a == b);
    CHECK(f.acquires == 1 && f.releases == 1 && f.depth == 0 && f.readsOutside == 0);
    CHECK(f.reads == 2 && f.order[0] == 0x9120 && f.order[1] == 0x9134);
    CHECK(d.configReads == 5 && f.boundCalls == 11);
    CHECK(f.begins==1 && f.ends==1 && f.accessDepth==0 && f.outsideAccess==0 && f.cleanupOrderFailures==0);
}

void testLifetimeAndPm() {
    for (unsigned config=1;config<=5;++config) {
        IOPCIDevice d; glk(d); Fake f; f.device=&d; f.revokeAfterConfig=config;
        d.configContext=&f; d.afterConfigRead=afterConfigRead;
        RenderWa::Topology t=poisoned();
        auto r=glkReadTopology(&d,ops(f),t);
        CHECK(r.status==GlkTopologyReadStatus::NotBound && d.configReads==config);
        CHECK(f.begins==1 && f.ends==1 && f.accessDepth==0 && f.acquires==0 && f.reads==0);
        CHECK(f.outsideAccess==0 && f.cleanupOrderFailures==0 && unchanged(t));
    }
    for (unsigned fault=0; fault<7; ++fault) {
        IOPCIDevice d; glk(d); Fake f; f.device=&d;
        RenderWa::Topology t=poisoned();
        if (fault==0) f.access=false;
        if (fault==1) f.awake=false;
        if (fault==2) f.unbindOnAcquire=true;
        if (fault==3) f.unbindOnRead=true;
        if (fault==4) f.readOk=false;
        if (fault==5) f.forcewake=false;
        if (fault==6) f.closeClientOnRead=true;
        auto r=glkReadTopology(&d,ops(f),t);
        CHECK(f.begins==1 && f.accessDepth==0 && f.depth==0);
        CHECK(f.outsideAccess==0 && f.cleanupOrderFailures==0);
        CHECK(f.ends==(fault<2?0u:1u));
        if (fault<2) CHECK(r.status==GlkTopologyReadStatus::AccessUnavailable && d.configReads==0 && f.reads==0);
        if (fault==2) CHECK(r.status==GlkTopologyReadStatus::NotBound && f.reads==0 && f.releases==1);
        if (fault==3) CHECK(r.status==GlkTopologyReadStatus::NotBound && f.reads==1 && f.releases==1);
        if (fault==4) CHECK(r.status==GlkTopologyReadStatus::RegisterReadFailed && f.releases==1);
        if (fault==5) CHECK(r.status==GlkTopologyReadStatus::ForcewakeUnavailable && f.reads==0 && f.releases==0);
        if (fault==6) CHECK(r.status==GlkTopologyReadStatus::Ok && !f.clientAlive && f.reads==2);
        if (fault!=6) CHECK(unchanged(t));
    }
    IOPCIDevice d; glk(d); Fake f; f.device=&d; auto o=ops(f);
    RenderWa::Topology t=poisoned(); o.beginAccess=nullptr;
    CHECK(glkReadTopology(&d,o,t).status==GlkTopologyReadStatus::MissingOps);
    o=ops(f); o.endAccess=nullptr;
    CHECK(glkReadTopology(&d,o,t).status==GlkTopologyReadStatus::MissingOps);
    CHECK(f.begins==0 && d.configReads==0 && unchanged(t));
}

void testReleaseUnknown() {
    for (unsigned failRead=0;failRead<2;++failRead) {
        IOPCIDevice d; glk(d); Fake f; f.device=&d;
        f.releaseKnown=false; f.readOk=!failRead;
        RenderWa::Topology t=poisoned();
        const auto r=glkReadTopology(&d,ops(f),t);
        CHECK(r.status==GlkTopologyReadStatus::ForcewakeReleaseUnknown);
        // Even a successful fuse decode cannot escape as a valid old output.
        const auto *bytes=reinterpret_cast<const uint8_t *>(&t);
        bool zero=true; for (size_t i=0;i<sizeof t;++i) zero=zero && bytes[i]==0;
        CHECK(zero);
        CHECK(f.quarantined && f.accessDepth==1 && f.depth==1);
        CHECK(f.begins==1 && f.ends==1 && f.releases==1 && f.cleanupOrderFailures==0 && f.outsideAccess==0);
        // Only this injected native-authoritative recovery resolves quarantine;
        // the producer never drops an unknown forcewake/PM operation itself.
        f.releaseKnown=true; CHECK(release(&f)); f.quarantined=false;
        end(&f,&d,GlkTopologyCleanup::Complete);
        CHECK(f.accessDepth==0 && f.depth==0 && f.cleanupOrderFailures==0);
    }
    IOPCIDevice d; glk(d); Fake f; f.device=&d; f.unbindOnRelease=true;
    RenderWa::Topology t=poisoned();
    CHECK(glkReadTopology(&d,ops(f),t).status==GlkTopologyReadStatus::NotBound);
    CHECK(unchanged(t) && f.ends==1 && f.accessDepth==0 && !f.quarantined);
}

void testRefusals() {
    {   // No device, no operations, not bound: nothing is accessed.
        IOPCIDevice d; glk(d); Fake f; f.device = &d;
        RenderWa::Topology t = poisoned();
        CHECK(glkReadTopology(nullptr, ops(f), t).status == GlkTopologyReadStatus::MissingDevice);
        GlkTopologyReadOps partial = ops(f); partial.readRegister = nullptr;
        CHECK(glkReadTopology(&d, partial, t).status == GlkTopologyReadStatus::MissingOps);
        partial = ops(f); partial.releaseGtForcewake = nullptr;
        CHECK(glkReadTopology(&d, partial, t).status == GlkTopologyReadStatus::MissingOps);
        CHECK(glkReadTopology(&d, GlkTopologyReadOps {}, t).status == GlkTopologyReadStatus::MissingOps);
        f.bound = false;
        CHECK(glkReadTopology(&d, ops(f), t).status == GlkTopologyReadStatus::NotBound);
        IOPCIDevice other; glk(other); f.bound = true;
        CHECK(glkReadTopology(&other, ops(f), t).status == GlkTopologyReadStatus::AccessUnavailable);
        CHECK(d.configReads == 0 && other.configReads == 0 && f.acquires == 0 && f.reads == 0 && unchanged(t));
    }
    {   // Identity refused before forcewake or MMIO.
        IOPCIDevice d; glk(d, 0x3184); Fake f; f.device = &d;
        RenderWa::Topology t = poisoned();
        const GlkTopologyReadResult r = glkReadTopology(&d, ops(f), t);
        CHECK(r.status == GlkTopologyReadStatus::IdentityRefused && r.decode == RenderWa::Status::UnsupportedDevice);
        put16(d, 0x00, 0xffff); put16(d, 0x02, 0xffff); // absent function reads all-ones
        CHECK(glkReadTopology(&d, ops(f), t).decode == RenderWa::Status::UnsupportedDevice);
        CHECK(f.acquires == 0 && f.reads == 0 && unchanged(t));
    }
    {   // Forcewake unavailable: no register read, nothing to release.
        IOPCIDevice d; glk(d); Fake f; f.device = &d; f.forcewake = false;
        RenderWa::Topology t = poisoned();
        CHECK(glkReadTopology(&d, ops(f), t).status == GlkTopologyReadStatus::ForcewakeUnavailable);
        CHECK(f.reads == 0 && f.releases == 0 && unchanged(t));
    }
    {   // Failed read still releases forcewake.
        IOPCIDevice d; glk(d); Fake f; f.device = &d; f.readOk = false;
        RenderWa::Topology t = poisoned();
        CHECK(glkReadTopology(&d, ops(f), t).status == GlkTopologyReadStatus::RegisterReadFailed);
        CHECK(f.acquires == 1 && f.releases == 1 && f.depth == 0 && f.reads == 1 && unchanged(t));
    }
    {   // Binding lost during the window: values are not attributed.
        IOPCIDevice d; glk(d); Fake f; f.device = &d; f.unbindOnRead = true;
        RenderWa::Topology t = poisoned();
        CHECK(glkReadTopology(&d, ops(f), t).status == GlkTopologyReadStatus::NotBound);
        CHECK(f.releases == 1 && f.depth == 0 && unchanged(t));
        CHECK(f.reads == 1); // revoke on FUSE2 must prevent EU_DISABLE read
    }
    {   // Slice 0 fused: EU_DISABLE(0) is not read and the topology is refused.
        IOPCIDevice d; glk(d); Fake f; f.device = &d; f.fuse2 = 0;
        RenderWa::Topology t = poisoned();
        const GlkTopologyReadResult r = glkReadTopology(&d, ops(f), t);
        CHECK(r.status == GlkTopologyReadStatus::TopologyRefused && r.decode == RenderWa::Status::NoRenderSlice);
        CHECK(f.reads == 1 && f.order[0] == 0x9120 && f.releases == 1 && unchanged(t));
    }
    {   // All-ones registers, as from an unpowered or unmapped read.
        IOPCIDevice d; glk(d); Fake f; f.device = &d; f.fuse2 = 0xffffffffU; f.euDisable0 = 0xffffffffU;
        RenderWa::Topology t = poisoned();
        const GlkTopologyReadResult r = glkReadTopology(&d, ops(f), t);
        CHECK(r.status == GlkTopologyReadStatus::TopologyRefused && r.decode == RenderWa::Status::InconsistentFuses);
        CHECK(f.reads == 2 && unchanged(t));
    }
}
} // namespace

int main() {
    testSuccess();
    testRefusals();
    testLifetimeAndPm();
    testReleaseUnknown();
    if (failures) {
        std::fprintf(stderr, "native_glk_topology_iokit_tests: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    std::printf("native_glk_topology_iokit_tests: %u checks passed\n", checks);
    return 0;
}

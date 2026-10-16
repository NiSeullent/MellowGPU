// HOSTED INJECTED TRANSPORT: unchanged GlkPhysicalOwnerIOKit.cpp, seeded
// GlkDmaMemoryIOKit.cpp and MemoryOwner.cpp against the explicit boundary in
// native_glk_owner_iokit_shim.hpp. No PCI configuration access, power
// transition, DMA, MMIO, interrupt or GPU operation happens; host threads
// only exercise the gate, command sleep and asynchronous drain logic.
#include "native_glk_owner_iokit_shim.hpp"
#include "../Mellow/GlkPhysicalOwnerIOKit.hpp"
#include <cstdio>
using namespace MellowNative;
namespace Shim = GlkOwnerShim;
using Owner = MellowGlkPhysicalOwner;
using S = GlkOwnerStatus;
using Q = GlkQuarantine;
static std::atomic<unsigned> checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)

namespace MellowNative {
// Hosted fence boundary for the seeded adapter: records, executes nothing.
void glkCpuFence(GlkCpuFence) { Shim::event("fence"); }
}

struct Reset { Reset() { Shim::reset(); IOMapper::gSystem = nullptr; } };
struct World : Reset {
    IOPCIDevice *device {new IOPCIDevice};
    Owner *owner {new Owner};
    ~World() { Shim::disposeAll(); }
    uint64_t on() {
        CHECK(owner->setPowerState(Owner::PowerOn, owner) == kIOPMAckImplied);
        return owner->snapshot().liveEpoch;
    }
    IOWorkLoop *loop() const { return owner->getWorkLoop(); }
    template <class T> T *source() const {
        for (auto *s : loop()->sources) if (auto *t = dynamic_cast<T *>(s)) return t;
        return nullptr;
    }
    // XNU actionWillTerminate/actionDidTerminate order for a terminated provider.
    void terminateProvider() {
        device->inactive = true; device->termPhase3 = true;
        CHECK(owner->willTerminate(device, 0));
        CHECK(owner->message(kIOMessageServiceIsTerminated, device) == kIOReturnSuccess);
        bool defer = false;
        CHECK(owner->didTerminate(device, 0, &defer) && !defer);
    }
    // XNU actionStop: stop(), then close if the client still holds it open.
    void stopLikeIOKit() {
        owner->stop(device);
        if (device->isOpen(owner)) device->close(owner);
    }
    void cleanFree() {
        owner->release(); device->release();
        CHECK(Shim::objects.empty() && Shim::allocations.empty());
    }
};

struct Probe { int runs {}; GlkPhysicalBinding seen {}; bool inGate {}; };
static S probe(void *p, const GlkPhysicalBinding &b) {
    auto &x = *static_cast<Probe *>(p);
    ++x.runs; x.seen = b; x.inGate = b.workLoop && b.workLoop->inGate();
    return S::Ok;
}
struct Producer {
    static inline int sequence {};
    std::vector<uint64_t> quiesced, resets;
    bool stopOk {true}, resetOk {true};
    int stamp {};
    Owner *reenterOff {};
    IOPCIDevice *reenterDevice {};
};
static bool producerQuiesce(void *p, uint64_t epoch) {
    auto &x = *static_cast<Producer *>(p);
    x.quiesced.push_back(epoch); x.stamp = ++Producer::sequence;
    if (x.reenterOff) {
        Owner *owner = x.reenterOff; x.reenterOff = nullptr;
        CHECK(owner->message(kIOMessageServiceIsTerminated, x.reenterDevice) == kIOReturnSuccess);
    }
    return x.stopOk;
}
static bool producerReset(void *p, uint64_t epoch) {
    auto &x = *static_cast<Producer *>(p);
    x.resets.push_back(epoch);
    return x.resetOk;
}
struct Registration { Owner *owner; uint64_t epoch; GlkProducer kind; GlkClientOps ops; };
static S registerAction(void *p, const GlkPhysicalBinding &) {
    auto &r = *static_cast<Registration *>(p);
    return r.owner->registerClient(r.epoch, r.kind, r.ops);
}
static S reg(World &w, uint64_t epoch, GlkProducer kind, Producer &p, bool withReset = false) {
    Registration r {w.owner, epoch, kind, {&p, producerQuiesce, withReset ? producerReset : nullptr, w.owner}};
    return w.owner->runLeased(epoch, registerAction, &r);
}
static bool started(World &w) { return w.owner->start(w.device); }
static std::vector<uint64_t> one(uint64_t e) { return {e}; }

static void startRefusals() {
    {
        Reset r;
        auto *plain = new IOService;
        auto *owner = new Owner;
        CHECK(!owner->start(plain) && !plain->openClient && owner->pmInits == 0);
        owner->release(); plain->release();
        CHECK(Shim::objects.empty());
    }
    {
        World w; version_major = 25;
        CHECK(!started(w) && !w.device->openClient && w.device->configReads == 0 && w.owner->pmInits == 0);
        w.cleanFree();
    }
    {
        // Ownership precedes every configuration access.
        World w; auto *other = new IOService;
        CHECK(w.device->open(other) && !started(w));
        CHECK(w.device->openClient == other && w.device->configReads == 0 && w.owner->pmInits == 0);
        w.device->close(other); other->release(); w.cleanFree();
    }
    struct Mutation { const char *name; void (*apply)(IOPCIDevice &); };
    const Mutation mutations[] = {
        {"revision", [](IOPCIDevice &d) { d.put32(0x08, 0x03000003); }},
        {"sibling 3184", [](IOPCIDevice &d) { d.put32(0x00, 0x31848086); }},
        {"subsystem", [](IOPCIDevice &d) { d.put32(0x2c, 0x3e7817aa); }},
        {"subsystem vendor", [](IOPCIDevice &d) { d.put32(0x2c, 0x3e771028); }},
        {"base class", [](IOPCIDevice &d) { d.put32(0x08, 0x04000006); }},
        {"function 1", [](IOPCIDevice &d) { d.function = 1; }},
        {"bridge header", [](IOPCIDevice &d) { d.config[0x0e] = 1; }},
        {"absent", [](IOPCIDevice &d) { std::memset(d.config, 0xff, sizeof d.config); }},
        {"registry 0", [](IOPCIDevice &d) { d.registryId = 0; }},
        {"no capability list", [](IOPCIDevice &d) { d.put16(0x06, 0); }},
        {"no PM capability", [](IOPCIDevice &d) { d.put16(0xd0, 0x0010); }},
        {"capability loop", [](IOPCIDevice &d) { d.put16(0xac, 0x4005); }},
    };
    for (const auto &m : mutations) {
        World w; m.apply(*w.device);
        const bool ok = started(w);
        if (ok) std::fprintf(stderr, "mutation accepted: %s\n", m.name);
        CHECK(!ok && !w.device->openClient && w.device->closes == 1);
        CHECK(w.owner->pmInits == 0 && w.device->configWrites == 0);
        w.cleanFree();
    }
    for (int fault = 0; fault < 5; ++fault) {
        World w;
        if (fault == 0) Shim::faults.workLoopFactory = true;
        if (fault == 1) Shim::faults.gateFactory = true;
        if (fault == 2) Shim::faults.timerFactory = true;
        if (fault == 3) Shim::faults.addSourceFailAt = 2;
        if (fault == 4) Shim::faults.registerPower = kIOReturnError;
        CHECK(!started(w) && !w.device->openClient && w.device->configWrites == 0);
        CHECK(fault == 4 ? w.owner->pmInits == 1 && w.owner->pmStops == 1 && w.owner->pmStopsInGate == 0
                         : w.owner->pmInits == 0 && w.owner->pmStops == 0);
        w.cleanFree();
    }
}

static void startAndPublish() {
    World w;
    CHECK(started(w) && !started(w));
    CHECK(w.device->openClient == w.owner && w.owner->pmInits == 1 && w.device->joinedDriver == w.owner);
    CHECK(w.owner->powerDriver == w.owner && w.owner->stateCount == 2 && w.owner->desired == 1);
    const auto &off = w.owner->states[0], &on = w.owner->states[1];
    CHECK(off.version == kIOPMPowerStateVersion1 && !off.capabilityFlags && !off.outputPowerCharacter &&
          !off.inputPowerRequirement);
    CHECK(on.version == kIOPMPowerStateVersion1 && on.capabilityFlags == (kIOPMPowerOn | kIOPMDeviceUsable) &&
          on.outputPowerCharacter == kIOPMPowerOn && on.inputPowerRequirement == kIOPMPowerOn);
    CHECK(Shim::count("pm.init") == 1 && Shim::count("pm.join") == 1 && Shim::count("pm.register") == 1);
    CHECK(w.loop() && w.loop()->sources.size() == 2 && w.source<IOCommandGate>() && w.source<IOTimerEventSource>());
    auto s = w.owner->snapshot();
    CHECK(s.started && s.providerOpen && !s.powered && !s.liveEpoch && s.missing == GlkAllProducers);
    // No lease exists until PM itself delivers On.
    GlkLeaseHold hold; Probe p;
    CHECK(w.owner->acquireHold(1, hold) == S::PowerDown && hold.slot == UINT32_MAX);
    CHECK(w.owner->runLeased(1, probe, &p) == S::PowerDown && p.runs == 0 && w.device->configWrites == 0);
    CHECK(w.owner->setPowerState(Owner::PowerOn, w.device) == kIOPMAckImplied);
    CHECK(w.owner->setPowerState(2, w.owner) == kIOPMAckImplied && !w.owner->snapshot().liveEpoch);
    const uint64_t e1 = w.on();
    CHECK(e1 && w.on() == e1);
    CHECK(w.device->busLeadCalls == 1 && w.device->configWrites == 1 && (w.device->get16(4) & kIOPCICommandBusLead));
    CHECK(w.owner->snapshot().busLeadByOwner && w.owner->snapshot().powered);
    CHECK(w.owner->acquireHold(e1 + 1, hold) == S::StaleEpoch && w.owner->acquireHold(0, hold) == S::StaleEpoch);
    CHECK(w.owner->runLeased(e1, probe, &p) == S::Ok && p.runs == 1 && p.inGate && !w.loop()->inGate());
    CHECK(p.seen.device == w.device && p.seen.owner == w.owner && p.seen.workLoop == w.loop() &&
          p.seen.gate == w.source<IOCommandGate>());
    const auto &id = p.seen.identity;
    CHECK(id.device.vendor == 0x8086 && id.device.device == 0x3185 && id.device.registryId == 4294967813ULL &&
          id.device.epoch == e1 && id.revision == 0x06 && id.subsystemVendor == 0x17aa && id.subsystem == 0x3e77);
    CHECK(w.owner->runLeased(e1, nullptr, nullptr) == S::Invalid);
    Probe serial;
    CHECK(w.owner->runSerialized(probe, &serial) == S::Ok && serial.inGate && serial.seen.identity.device.epoch == 0);
    w.stopLikeIOKit();
    CHECK(!w.device->openClient && w.device->closes == 1 && w.owner->pmStops == 1 && w.owner->pmStopsInGate == 0);
    CHECK(w.loop()->sources.empty() && !(w.device->get16(4) & kIOPCICommandBusLead) && w.device->busLeadCalls == 2);
    w.cleanFree();
}

static void publicationEvidence() {
    {
        World w; CHECK(started(w));
        w.device->put16(0xd4, 0x0003); // D3hot: a delivery alone publishes nothing
        CHECK(w.on() == 0 && w.owner->snapshot().quarantine == Q::None && w.owner->snapshot().powered);
        w.stopLikeIOKit(); w.cleanFree();
    }
    {
        World w; CHECK(started(w));
        w.device->put16(0x04, 0);
        CHECK(w.on() == 0 && w.device->busLeadCalls == 0);
        w.stopLikeIOKit(); w.cleanFree();
    }
    {
        World w; CHECK(started(w));
        w.device->busLeadIgnored = true;
        CHECK(w.on() == 0 && w.device->busLeadCalls == 1);
        w.stopLikeIOKit();
        CHECK(w.device->busLeadCalls == 2);
        w.cleanFree();
    }
    {
        // Firmware-established bus lead is observed and never altered.
        World w; w.device->put16(0x04, kIOPCICommandMemorySpace | kIOPCICommandBusLead);
        CHECK(started(w) && w.on() != 0 && w.device->configWrites == 0);
        w.stopLikeIOKit();
        CHECK(w.device->configWrites == 0 && (w.device->get16(4) & kIOPCICommandBusLead));
        w.cleanFree();
    }
    {
        World w; CHECK(started(w));
        w.device->put32(0x08, 0x03000007);
        const unsigned refs = w.owner->refs;
        CHECK(w.on() == 0 && w.owner->snapshot().quarantine == Q::IdentityChanged && w.owner->refs == refs + 1);
    }
}

static void leaseEntryRechecks() {
    GlkLeaseHold h; Probe p;
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        w.device->put16(0xd4, 0x0003);
        CHECK(w.owner->runLeased(e1, probe, &p) == S::Quarantined && p.runs == 0);
        CHECK(w.owner->snapshot().quarantine == Q::PowerStateMismatch && !w.owner->snapshot().liveEpoch);
        CHECK(w.owner->acquireHold(e1, h) == S::Quarantined);
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        w.device->put32(0x2c, 0x3e7817aa);
        CHECK(w.owner->acquireHold(e1, h) == S::Quarantined && w.owner->snapshot().quarantine == Q::IdentityChanged);
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        w.device->registryId = 99;
        CHECK(w.owner->acquireHold(e1, h) == S::Quarantined);
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        w.device->inactive = true;
        CHECK(w.owner->acquireHold(e1, h) == S::Ownership && w.owner->snapshot().quarantine == Q::None);
        w.device->inactive = false;
        w.stopLikeIOKit(); w.cleanFree();
    }
}

static void holdTable() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    GlkLeaseHold holds[Owner::MaxHolds], extra;
    for (auto &h : holds) CHECK(w.owner->acquireHold(e1, h) == S::Ok && h.epoch == e1);
    CHECK(w.owner->acquireHold(e1, extra) == S::Busy && extra.slot == UINT32_MAX);
    GlkLeaseHold forged = holds[3]; forged.generation += 100;
    GlkLeaseHold wrongEpoch = holds[4]; ++wrongEpoch.epoch;
    GlkLeaseHold replay = holds[5];
    CHECK(w.owner->releaseHold(forged) == S::Invalid && w.owner->releaseHold(wrongEpoch) == S::Invalid);
    CHECK(w.owner->releaseHold(holds[5]) == S::Ok && holds[5].slot == UINT32_MAX && w.owner->releaseHold(replay) == S::Invalid);
    for (auto &h : holds) if (h.slot != UINT32_MAX) CHECK(w.owner->releaseHold(h) == S::Ok);
    CHECK(w.owner->snapshot().holds == 0);
    w.stopLikeIOKit(); w.cleanFree();
}

static void producersAndPowerCycle() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw, irq, extra; Producer::sequence = 0;
    const GlkClientOps ungated {&fw, producerQuiesce, nullptr, w.owner};
    CHECK(w.owner->registerClient(e1, GlkProducerForcewake, ungated) == S::Ownership);
    CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok && reg(w, e1, GlkProducerInterrupt, irq) == S::Ok);
    CHECK(reg(w, e1, GlkProducerForcewake, extra) == S::Busy);
    CHECK(reg(w, e1, GlkProducerGpuReset, extra) == S::Invalid && reg(w, e1, GlkProducerGpuVm, extra, true) == S::Invalid);
    CHECK(reg(w, e1, GlkProducer(3), extra) == S::Invalid && reg(w, e1 + 1, GlkProducerGpuVm, extra) == S::StaleEpoch);
    auto s = w.owner->snapshot();
    CHECK(s.registered == (GlkProducerForcewake | GlkProducerInterrupt) &&
          s.missing == (GlkProducerGpuVm | GlkProducerGpuReset));
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied);
    CHECK(fw.quiesced == one(e1) && irq.quiesced == one(e1) && irq.stamp < fw.stamp && extra.quiesced.empty());
    s = w.owner->snapshot();
    CHECK(!s.liveEpoch && s.revokedEpoch == e1 && !s.revoking && !s.powered && !s.registered &&
          s.missing == GlkAllProducers && w.owner->acks == 0);
    GlkLeaseHold h;
    CHECK(w.owner->acquireHold(e1, h) == S::PowerDown);
    const uint64_t e2 = w.on();
    CHECK(e2 > e1 && w.owner->acquireHold(e1, h) == S::StaleEpoch);
    CHECK(w.owner->acquireHold(e2, h) == S::Ok && w.owner->releaseHold(h) == S::Ok);
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied && fw.quiesced.size() == 1);
    w.stopLikeIOKit(); w.cleanFree();
}

struct OwnedProducer : OSObject {
    unsigned *destroyed, *quiesced;
    OwnedProducer(unsigned &d, unsigned &q) : destroyed(&d), quiesced(&q) {}
    ~OwnedProducer() override { ++*destroyed; }
    static bool quiesce(void *p, uint64_t) { ++*static_cast<OwnedProducer *>(p)->quiesced; return true; }
};
static void retainedProducer() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    unsigned destroyed = 0, quiesced = 0;
    auto *p = new OwnedProducer(destroyed, quiesced);
    Registration r {w.owner, e1, GlkProducerForcewake, {p, OwnedProducer::quiesce, nullptr, p}};
    CHECK(w.owner->runLeased(e1, registerAction, &r) == S::Ok);
    CHECK(p->refs == 2); // owner must own the callback anchor before publishing it.
    p->release();
    CHECK(destroyed == 0);
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied);
    CHECK(quiesced == 1 && destroyed == 1);
    w.stopLikeIOKit(); w.cleanFree();
}

static void recursiveRevocation() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw; fw.reenterOff = w.owner; fw.reenterDevice = w.device;
    CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied);
    CHECK(fw.quiesced == one(e1)); // Reentrant PM delivery must not run producer twice.
    CHECK(!w.owner->snapshot().revoking && !w.owner->snapshot().liveEpoch);
    w.stopLikeIOKit(); w.cleanFree();
}

struct ActionClose {
    Owner *owner;
    IOPCIDevice *device;
    Producer *producer;
    bool openInside {}, stoppedInside {};
};
static S closeDuringAction(void *p, const GlkPhysicalBinding &) {
    auto &c = *static_cast<ActionClose *>(p);
    CHECK(c.owner->message(kIOMessageServiceIsRequestingClose, c.device) == kIOReturnSuccess);
    c.openInside = c.device->isOpen(c.owner);
    c.stoppedInside = !c.producer->quiesced.empty();
    return S::Ok;
}
static void actionDrainsBeforeClose() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw;
    CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
    ActionClose c {w.owner, w.device, &fw};
    CHECK(w.owner->runLeased(e1, closeDuringAction, &c) == S::Ok);
    CHECK(c.openInside && !c.stoppedInside); // native action still owns its binding
    CHECK(fw.quiesced == one(e1) && !w.device->isOpen(w.owner));
    w.stopLikeIOKit(); w.cleanFree();
}

static void terminateSupersedesReset() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer reset; reset.reenterOff = w.owner; reset.reenterDevice = w.device;
    CHECK(reg(w, e1, GlkProducerGpuReset, reset, true) == S::Ok);
    CHECK(w.owner->requestReset(e1) == S::Ok);
    CHECK(reset.quiesced == one(e1) && reset.resets.empty());
    CHECK(w.owner->snapshot().terminating && !w.owner->snapshot().liveEpoch);
    w.stopLikeIOKit(); w.cleanFree();
}

static void asyncPowerDrain() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw; CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
    GlkLeaseHold h, h2; Probe p;
    CHECK(w.owner->acquireHold(e1, h) == S::Ok);
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == IOReturn(Owner::PowerAckBoundUs));
    auto s = w.owner->snapshot();
    CHECK(s.revoking && s.powerAckPending && !s.liveEpoch && s.holds == 1 && fw.quiesced.empty());
    auto *timer = w.source<IOTimerEventSource>();
    CHECK(timer->armed && timer->armedUs == Owner::DrainBudgetUs);
    CHECK(w.owner->acquireHold(e1, h2) == S::Revoking && w.owner->runLeased(e1, probe, &p) == S::Revoking && !p.runs);
    std::thread releaser([&] { CHECK(w.owner->releaseHold(h) == S::Ok); });
    releaser.join();
    CHECK(w.owner->acks == 1 && w.owner->acksInGate == 0 && fw.quiesced == one(e1) && !timer->armed);
    s = w.owner->snapshot();
    CHECK(!s.revoking && !s.powerAckPending && s.quarantine == Q::None && !timer->fire());
    CHECK(w.on() > e1);
    w.stopLikeIOKit(); w.cleanFree();
}

static void drainTimeout() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw; CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
    GlkLeaseHold h, h2;
    CHECK(w.owner->acquireHold(e1, h) == S::Ok);
    const unsigned refs = w.owner->refs;
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == IOReturn(Owner::PowerAckBoundUs));
    CHECK(w.source<IOTimerEventSource>()->fire());
    auto s = w.owner->snapshot();
    CHECK(s.quarantine == Q::DrainTimeout && !s.revoking && !s.powerAckPending && w.owner->acks == 1);
    CHECK(fw.quiesced.empty() && w.owner->refs == refs + 1 && s.registered == GlkProducerForcewake);
    CHECK(w.owner->releaseHold(h) == S::Ok && w.owner->acks == 1 && fw.quiesced.empty());
    CHECK(w.on() == 0 && w.owner->acquireHold(0, h2) == S::Quarantined);
    w.terminateProvider();
    CHECK(w.device->openClient == w.owner && w.owner->stopsScheduled == 0 && w.device->closes == 0);
}

static void quiesceUnknown() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    Producer fw, irq; irq.stopOk = false;
    CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok && reg(w, e1, GlkProducerInterrupt, irq) == S::Ok);
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied);
    const auto s = w.owner->snapshot();
    CHECK(fw.quiesced == one(e1) && irq.quiesced == one(e1) && s.quarantine == Q::ClientQuiesceUnknown);
    CHECK(s.registered == (GlkProducerForcewake | GlkProducerInterrupt) && w.on() == 0);
}

struct Nested { Owner *owner; S result; };
static S nestedWait(void *p, const GlkPhysicalBinding &) {
    auto &n = *static_cast<Nested *>(p);
    n.result = n.owner->waitRevocation(1000);
    return S::Ok;
}

static void resetPaths() {
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        CHECK(w.owner->requestReset(e1) == S::MissingProducer && w.owner->requestReset(e1 + 1) == S::StaleEpoch);
        const auto s = w.owner->snapshot();
        CHECK(s.liveEpoch == e1 && !s.revoking && !s.revocation && w.device->configWrites == 1);
        w.stopLikeIOKit(); w.cleanFree();
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        Producer fw, rst;
        CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok && reg(w, e1, GlkProducerGpuReset, rst, true) == S::Ok);
        GlkLeaseHold h;
        CHECK(w.owner->acquireHold(e1, h) == S::Ok && w.owner->requestReset(e1) == S::Ok);
        auto s = w.owner->snapshot();
        CHECK(s.revoking && !s.liveEpoch && (s.revocation & GlkRevokeReset) && rst.resets.empty());
        CHECK(w.owner->waitRevocation(1000) == S::Timeout && w.owner->waitRevocation(0) == S::Busy);
        Nested nested {w.owner, S::Ok};
        CHECK(w.owner->runSerialized(nestedWait, &nested) == S::Ok && nested.result == S::Busy);
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(w.owner->releaseHold(h) == S::Ok);
        });
        CHECK(w.owner->waitRevocation(2000000) == S::Ok);
        releaser.join();
        CHECK(rst.quiesced == one(e1) && fw.quiesced == one(e1) && rst.resets == one(e1) && fw.stamp > rst.stamp);
        s = w.owner->snapshot();
        CHECK(s.liveEpoch > e1 && s.quarantine == Q::None && !s.registered && w.source<IOCommandGate>()->sleeps >= 2);
        w.stopLikeIOKit(); w.cleanFree();
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        Producer rst; rst.resetOk = false;
        CHECK(reg(w, e1, GlkProducerGpuReset, rst, true) == S::Ok && w.owner->requestReset(e1) == S::Ok);
        CHECK(w.owner->snapshot().quarantine == Q::ResetUnproven && !w.owner->snapshot().liveEpoch);
        CHECK(w.owner->waitRevocation(1000) == S::Quarantined);
    }
}

static void termination() {
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        Producer fw; CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
        w.terminateProvider();
        CHECK(fw.quiesced == one(e1) && !w.device->openClient && w.device->closes == 1 && w.owner->stopsScheduled >= 1);
        CHECK(!(w.device->get16(4) & kIOPCICommandBusLead) && w.device->busLeadCalls == 2);
        const auto s = w.owner->snapshot();
        CHECK(s.terminating && !s.providerOpen && s.closeRequested);
        GlkLeaseHold h;
        CHECK(w.owner->acquireHold(e1, h) == S::Terminating && w.on() == 0);
        w.stopLikeIOKit();
        CHECK(w.owner->pmStops == 1 && w.owner->pmStopsInGate == 0 && w.loop()->sources.empty());
        w.cleanFree();
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        Producer fw; CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
        GlkLeaseHold h, h2;
        CHECK(w.owner->acquireHold(e1, h) == S::Ok);
        w.terminateProvider();
        CHECK(w.device->openClient == w.owner && w.owner->stopsScheduled == 0 && fw.quiesced.empty());
        CHECK(w.owner->acquireHold(e1, h2) == S::Terminating);
        std::thread releaser([&] { CHECK(w.owner->releaseHold(h) == S::Ok); });
        releaser.join();
        CHECK(!w.device->openClient && fw.quiesced == one(e1) && w.owner->stopsScheduled == 1);
        w.stopLikeIOKit(); w.cleanFree();
    }
}

static void ownStop() {
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        Producer fw; CHECK(reg(w, e1, GlkProducerForcewake, fw) == S::Ok);
        GlkLeaseHold h; CHECK(w.owner->acquireHold(e1, h) == S::Ok);
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(w.owner->releaseHold(h) == S::Ok);
        });
        w.stopLikeIOKit();
        releaser.join();
        CHECK(fw.quiesced == one(e1) && !w.device->openClient && w.owner->snapshot().quarantine == Q::None);
        w.cleanFree();
    }
    {
        World w; CHECK(started(w)); const uint64_t e1 = w.on();
        GlkLeaseHold h; CHECK(w.owner->acquireHold(e1, h) == S::Ok);
        const auto t0 = std::chrono::steady_clock::now();
        w.owner->stop(w.device);
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(900));
        CHECK(w.owner->snapshot().quarantine == Q::DrainTimeout && w.device->openClient == w.owner);
        CHECK(w.loop()->sources.size() == 2 && w.owner->acks == 0);
        w.device->close(w.owner); // IOKit closes after stop regardless; state stays retained
    }
}

struct Vm {
    unsigned maps {}, unmaps {}, quiesces {};
    uint64_t epoch {};
    std::vector<uint64_t> pages;
};
static MemoryStatus vmMap(void *p, const DeviceIdentity &id, uint64_t, const MemoryHandle &handle,
        const DmaPin &pin, bool writable, GpuMapping &out) {
    auto &v = *static_cast<Vm *>(p);
    ++v.maps; v.epoch = id.epoch; v.pages.assign(pin.pages, pin.pages + pin.pageCount);
    out = {&v, 0x100000 + uint64_t(handle.slot) * 0x10000, pin.bytes, writable};
    return MemoryStatus::Ok;
}
static MemoryStatus vmUnmap(void *p, const DeviceIdentity &, uint64_t, const MemoryHandle &,
        const DmaPin &, GpuMapping &) { ++static_cast<Vm *>(p)->unmaps; return MemoryStatus::Ok; }
static bool vmRetired(void *, const MemoryView &) { return true; }
static bool vmQuiesce(void *p, const DeviceIdentity &id) {
    auto &v = *static_cast<Vm *>(p); ++v.quiesces; v.epoch = id.epoch; return true;
}
struct DmaCtx {
    Owner *owner; uint64_t epoch; GlkDmaMemoryIOKit *adapter; MemoryOwner *memory;
    MemoryLimits limits; GlkVmProducer vm;
    MemoryHandle handle {};
    MemoryStatus ms[8] {};
    S detach {S::Unavailable};
};
static S attachAction(void *p, const GlkPhysicalBinding &) {
    auto &c = *static_cast<DmaCtx *>(p);
    return c.owner->attachDmaMemory(c.epoch, *c.adapter, c.limits, c.vm);
}
static S useAction(void *p, const GlkPhysicalBinding &b) {
    auto &c = *static_cast<DmaCtx *>(p);
    c.ms[0] = c.memory->initialize(b.identity.device, c.limits, c.adapter->backend());
    c.ms[1] = c.memory->allocate(42, 8192, c.handle);
    c.ms[2] = c.memory->map(42, c.handle, true);
    c.ms[3] = c.memory->sync(42, c.handle, CacheDirection::ForDevice);
    return S::Ok;
}
static S teardownAction(void *p, const GlkPhysicalBinding &) {
    auto &c = *static_cast<DmaCtx *>(p);
    c.ms[4] = c.memory->sync(42, c.handle, CacheDirection::ForDevice);
    c.ms[5] = c.memory->quiesce();
    c.ms[6] = c.memory->retire(42, c.handle);
    c.ms[7] = c.memory->close();
    c.detach = c.owner->detachDmaMemory(*c.adapter);
    return S::Ok;
}

struct OwnedVm : OSObject {
    Vm value;
    unsigned *destroyed;
    explicit OwnedVm(unsigned &d) : destroyed(&d) {}
    ~OwnedVm() override { ++*destroyed; }
};
static S detachOnly(void *p, const GlkPhysicalBinding &) {
    auto &c = *static_cast<DmaCtx *>(p);
    return c.owner->detachDmaMemory(*c.adapter);
}
static void retainedVmBinding() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    unsigned destroyed = 0;
    auto *v = new OwnedVm(destroyed);
    GlkDmaMemoryIOKit adapter; MemoryOwner memory;
    DmaCtx c {w.owner, e1, &adapter, &memory, {39,48,8192,16384},
        {&v->value, vmMap, vmUnmap, vmRetired, vmQuiesce, v}};
    CHECK(w.owner->runLeased(e1, attachAction, &c) == S::Ok && v->refs == 3);
    v->release(); // registration and persistent binding retain it separately.
    CHECK(w.owner->setPowerState(Owner::PowerOff, w.owner) == kIOPMAckImplied);
    CHECK(destroyed == 0 && v->value.quiesces == 1 && v->refs == 1);
    CHECK(w.owner->runSerialized(detachOnly, &c) == S::Ok && destroyed == 1);
    w.stopLikeIOKit(); w.cleanFree();
}

static void dmaFactory() {
    World w; CHECK(started(w)); const uint64_t e1 = w.on();
    GlkDmaMemoryIOKit adapter; MemoryOwner memory; Vm vm;
    DmaCtx c {w.owner, e1, &adapter, &memory, {39, 48, 8192, 16384}, {&vm, vmMap, vmUnmap, vmRetired, vmQuiesce, w.owner}};
    CHECK(w.owner->attachDmaMemory(e1, adapter, c.limits, c.vm) == S::Ownership && !adapter.attached());
    DmaCtx missing = c; missing.vm.retired = nullptr;
    CHECK(w.owner->runLeased(e1, attachAction, &missing) == S::MissingProducer && !adapter.attached());
    CHECK(w.owner->snapshot().missing & GlkProducerGpuVm);
    CHECK(w.owner->runLeased(e1, attachAction, &c) == S::Ok && adapter.attached() &&
          adapter.route() == GlkDmaRoute::DirectPhysical && w.owner->snapshot().registered == GlkProducerGpuVm);
    CHECK(w.owner->runLeased(e1, attachAction, &c) == S::Busy);
    CHECK(w.owner->runLeased(e1, useAction, &c) == S::Ok);
    CHECK(c.ms[0] == MemoryStatus::Ok && c.ms[1] == MemoryStatus::Ok && c.ms[2] == MemoryStatus::Ok &&
          c.ms[3] == MemoryStatus::Ok);
    CHECK(vm.maps == 1 && vm.epoch == e1 && vm.pages.size() == 2 && vm.pages[1] == vm.pages[0] + 4096);
    CHECK(memory.chargedBytes() == 8192 && adapter.pinnedBytes() == 8192);
    // Outside the shared gate the seeded adapter is never admitted.
    const auto ungated = memory.sync(42, c.handle, CacheDirection::ForDevice);
    CHECK(ungated == MemoryStatus::Ownership);
    // Provider termination: the VM producer stops DMA for e1; the provider
    // stays open while the adapter still references its binding.
    w.terminateProvider();
    CHECK(vm.quiesces == 1 && vm.epoch == e1 && w.device->openClient == w.owner && w.owner->stopsScheduled == 0);
    CHECK(w.owner->runSerialized(teardownAction, &c) == S::Ok);
    std::printf("dma teardown statuses: ungated=%d sync=%d quiesce=%d retire=%d close=%d detach=%d\n",
        int(ungated), int(c.ms[4]), int(c.ms[5]), int(c.ms[6]), int(c.ms[7]), int(c.detach));
    CHECK(c.ms[4] == MemoryStatus::Ownership && c.ms[5] == MemoryStatus::Ok && c.ms[6] == MemoryStatus::Ok &&
          c.ms[7] == MemoryStatus::Ok && c.detach == S::Ok);
    CHECK(!adapter.attached() && vm.unmaps == 1 && vm.quiesces == 2 && adapter.pinnedBytes() == 0);
    CHECK(!w.device->openClient && w.owner->stopsScheduled == 1);
    w.stopLikeIOKit(); w.cleanFree();
}

int main() {
    startRefusals();
    startAndPublish();
    publicationEvidence();
    leaseEntryRechecks();
    holdTable();
    producersAndPowerCycle();
    retainedProducer();
    recursiveRevocation();
    actionDrainsBeforeClose();
    terminateSupersedesReset();
    asyncPowerDrain();
    drainTimeout();
    quiesceUnknown();
    resetPaths();
    termination();
    ownStop();
    retainedVmBinding();
    dmaFactory();
    std::printf("glk physical owner boundary: %u checks; hosted injected transport; "
                "no hardware, PCI, power, DMA or GPU operation\n", checks.load());
    return 0;
}

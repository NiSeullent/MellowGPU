// HOSTED TESTS ONLY. Decoder tests drive the production decoder against an
// in-memory GLK interrupt register model; adapter tests run the production
// IOKit adapter against tests/native_glk_interrupt_shim. Neither touches a
// GPU, MMIO, IOKit or a kernel; passing proves no hardware behavior.
#include "Drivers/NativeGlk/InterruptDecode.hpp"
#include "Mellow/GlkInterruptIOKit.hpp"
#include <functional>
#include <utility>

using namespace MellowNativeGlk::Interrupt;
using MellowNativeGlk::Handling;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {
struct FakeGlk {
    uint32_t master {}, extraLevels {};
    uint32_t imr[GtBanks] {}, ier[GtBanks] {}, iir[GtBanks] {};
    uint32_t dePipe[DePipes] {}, dePort {}, deMisc {}, pcu {};
    bool admitted {true}, gone {}, ignoreMasterWrite {}, badAccess {}, deWrite {}, inHook {};
    uint32_t failRead {}, failWrite {};
    unsigned reads {};
    uint64_t epoch {77};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::function<void(uint32_t)> afterRead;
    std::function<void(uint32_t, uint32_t)> beforeWrite;
    uint32_t levels() const {
        uint32_t level = extraLevels;
        const uint32_t p0 = iir[0] & ier[0], p1 = iir[1] & ier[1], p2 = iir[2] & ier[2], p3 = iir[3] & ier[3];
        if (p0 & 0xffffU) level |= MasterGtRcs;
        if (p0 >> 16) level |= MasterGtBcs;
        if (p1 & 0xffffU) level |= MasterGtVcs0;
        if (p1 >> 16) level |= MasterGtVcs1;
        if (p2 & 0xffffU) level |= MasterGtPm;
        if (p2 >> 16) level |= MasterGtGuc;
        if (p3 & 0xffffU) level |= MasterGtVecs;
        return level;
    }
    void raise(unsigned bank, uint32_t bits) { iir[bank] |= bits & ~imr[bank]; }
    uint32_t *gt(uint32_t offset, unsigned &kind) {
        for (unsigned b = 0; b < GtBanks; ++b) {
            if (offset == gtImr(b)) { kind = 1; return &imr[b]; }
            if (offset == gtIer(b)) { kind = 2; return &ier[b]; }
            if (offset == gtIir(b)) { kind = 3; return &iir[b]; }
        }
        return nullptr;
    }
    static bool read(void *opaque, uint32_t offset, uint32_t &value) {
        auto &s = *static_cast<FakeGlk *>(opaque);
        if (!s.admitted || offset == s.failRead) return false;
        ++s.reads;
        unsigned kind = 0;
        if (s.gone) value = 0xffffffffU;
        else if (offset == MasterIrq) value = (s.master & MasterControl) | s.levels();
        else if (uint32_t *reg = s.gt(offset, kind)) value = *reg;
        else if (offset == dePipeIer(0) || offset == dePipeIer(1) || offset == dePipeIer(2))
            value = s.dePipe[(offset - dePipeIer(0)) / 0x10];
        else if (offset == DePortIer) value = s.dePort;
        else if (offset == DeMiscIer) value = s.deMisc;
        else if (offset == PcuIer) value = s.pcu;
        else { s.badAccess = true; return false; }
        if (s.afterRead && !s.inHook) { s.inHook = true; s.afterRead(offset); s.inHook = false; }
        return true;
    }
    static bool write(void *opaque, uint32_t offset, uint32_t value) {
        auto &s = *static_cast<FakeGlk *>(opaque);
        if (!s.admitted || offset == s.failWrite) return false;
        if (s.beforeWrite && !s.inHook) { s.inHook = true; s.beforeWrite(offset, value); s.inHook = false; }
        s.writes.push_back({offset, value});
        char text[48];
        snprintf(text, sizeof text, "mmio.w %x=%x", offset, value);
        shim::note(text);
        if (s.gone) return true;
        unsigned kind = 0;
        if (offset == MasterIrq) { if (!s.ignoreMasterWrite) s.master = value & MasterControl; }
        else if (uint32_t *reg = s.gt(offset, kind)) *reg = kind == 3 ? (*reg & ~value) : value;
        else if (offset >= dePipeIer(0) && offset <= PcuIer) s.deWrite = true;
        else { s.badAccess = true; return false; }
        return true;
    }
    static bool admit(void *opaque, uint64_t epoch) {
        auto &self = *static_cast<FakeGlk *>(opaque);
        return epoch == self.epoch && self.admitted;
    }
    RegisterOps ops() { return {this, admit, read, write}; }
    size_t count(uint32_t offset) const { size_t n = 0; for (auto &w : writes) n += w.first == offset; return n; }
};

Configuration config(uint8_t engines = engineBit(Engine::Rcs0) | engineBit(Engine::Bcs0)) {
    Configuration c {};
    c.epoch = 77; c.identity = FirstHardware; c.engines = engines;
    return c;
}

void constantsMatchPinnedSources() {
    CHECK(MasterIrq == 0x44200 && MasterControl == 0x80000000U && MasterGtIrqs == 0x7f);
    CHECK(gtIsr(0) == 0x44300 && gtImr(0) == 0x44304 && gtIir(0) == 0x44308 && gtIer(0) == 0x4430c);
    CHECK(gtIir(3) == 0x44338 && gtIer(2) == 0x4432c);
    CHECK(dePipeIer(2) == 0x4442c && DePortIer == 0x4444c && DeMiscIer == 0x4446c && PcuIer == 0x444ec);
    CHECK(CsExeclistsIrqs == 0x0909);
    CHECK(engineRoute(Engine::Bcs0).bank == 0 && engineRoute(Engine::Bcs0).shift == 16);
    CHECK(engineRoute(Engine::Vcs0).bank == 1 && engineRoute(Engine::Vecs0).bank == 3);
    CHECK(gtBankMaster(2) == (MasterGtPm | MasterGtGuc));
    // NEEDS_FORCE_WAKE() is false for every offset this decoder touches.
    CHECK(FirstRegister >= 0x40000 && LastRegister < 0x116000);
}

void configureRejectsBeforeEffects() {
    struct Case { Configuration c; Status expected; };
    Configuration bad[8];
    for (auto &c : bad) c = config();
    bad[0].epoch = 0;
    bad[1].identity.device = 0x3184;
    bad[2].identity.revision = 0x03;
    bad[3].identity.subsystem = 0x3e78;
    bad[4].engines = 0;
    bad[5].engines = 0x10;           // no VCS1 or other engines on GLK
    bad[6].csIrqs = CsExeclistsIrqs | (1U << 4);
    bad[7].csIrqs = 0;
    for (auto &c : bad) {
        FakeGlk hw; Decoder d;
        CHECK(d.configure(c, hw.ops()) != Status::Ok);
        CHECK(hw.writes.empty() && d.state() == Decoder::Unconfigured);
    }
    FakeGlk hw; Decoder d;
    RegisterOps missing = hw.ops(); missing.write32 = nullptr;
    CHECK(d.configure(config(), missing) == Status::Invalid);
    hw.admitted = false;
    CHECK(d.configure(config(), hw.ops()) == Status::Unavailable && hw.writes.empty());
}

void unownedDomainsRejectedBeforeEffects() {
    for (unsigned which = 0; which < 6; ++which) {
        FakeGlk hw; Decoder d;
        if (which < 3) hw.dePipe[which] = 1; else if (which == 3) hw.dePort = 8;
        else if (which == 4) hw.deMisc = 1U << 22; else hw.pcu = 1;
        CHECK(d.configure(config(), hw.ops()) == Status::Unowned);
        CHECK(hw.writes.empty() && !hw.deWrite && d.state() == Decoder::Unconfigured);
    }
    FakeGlk hw; Decoder d; hw.gone = true;
    CHECK(d.configure(config(), hw.ops()) == Status::IoFailure && hw.writes.empty());
}

void configureFollowsGen8ResetAndInit() {
    FakeGlk hw; Decoder d;
    hw.master = MasterControl; hw.iir[0] = 0x101; hw.imr[2] = 0; hw.ier[2] = 0x30; hw.iir[2] = 0x10;
    CHECK(d.configure(config(), hw.ops()) == Status::Ok);
    std::vector<std::pair<uint32_t, uint32_t>> expected {{MasterIrq, 0}};
    for (unsigned b = 0; b < GtBanks; ++b) {
        expected.push_back({gtImr(b), 0xffffffffU}); expected.push_back({gtIer(b), 0});
        expected.push_back({gtIir(b), 0xffffffffU}); expected.push_back({gtIir(b), 0xffffffffU});
    }
    const uint32_t on[GtBanks] {0x09090909U, 0, 0, 0};
    for (unsigned b = 0; b < GtBanks; ++b) { expected.push_back({gtIer(b), on[b]}); expected.push_back({gtImr(b), ~on[b]}); }
    CHECK(hw.writes == expected);
    CHECK(!hw.deWrite && !hw.badAccess && !(hw.master & MasterControl));
    CHECK(hw.ier[0] == 0x09090909U && hw.imr[0] == 0xf6f6f6f6U && hw.ier[2] == 0 && hw.imr[2] == 0xffffffffU);
    CHECK(hw.iir[0] == 0 && hw.iir[2] == 0);
    CHECK(d.state() == Decoder::Configured && d.ownedMaster() == (MasterGtRcs | MasterGtBcs));
    CHECK(d.configure(config(), hw.ops()) == Status::Invalid); // no double configure
}

void configureFailuresFailClosed() {
    FakeGlk stuck; Decoder a; stuck.master = MasterControl; stuck.ignoreMasterWrite = true;
    CHECK(a.configure(config(), stuck.ops()) == Status::IoFailure && a.state() == Decoder::Fault);
    CHECK(stuck.count(gtIer(0)) == 0); // never programs enables behind a live master
    FakeGlk lost; Decoder b; lost.failWrite = gtIer(1);
    CHECK(b.configure(config(), lost.ops()) == Status::IoFailure && b.state() == Decoder::Fault);
    CHECK(!b.filter());
}

struct Armed {
    FakeGlk hw; Decoder d;
    explicit Armed(uint8_t engines = engineBit(Engine::Rcs0) | engineBit(Engine::Bcs0)) {
        CHECK(d.configure(config(engines), hw.ops()) == Status::Ok);
        CHECK(d.arm() == Status::Ok && (hw.master & MasterControl));
        hw.writes.clear(); hw.reads = 0;
    }
};

void filterBoundedAndNotOurs() {
    Armed t;
    CHECK(!t.d.filter());
    CHECK(t.hw.writes.empty() && t.hw.reads == 1); // one read, no write, no re-enable
    t.hw.admitted = false; t.hw.raise(0, CsUserInterrupt); t.hw.reads = 0;
    CHECK(!t.d.filter() && t.hw.reads == 0 && t.hw.writes.empty());
    Decoder idle; CHECK(!idle.filter());
    Decoder unarmed; FakeGlk hw; CHECK(unarmed.configure(config(), hw.ops()) == Status::Ok);
    hw.raise(0, CsUserInterrupt); hw.reads = 0;
    CHECK(!unarmed.filter() && hw.reads == 0);
}

void claimSampleAckExactRoute() {
    Armed t;
    t.hw.raise(0, CsUserInterrupt | CsContextSwitch);
    CHECK(t.d.filter());
    CHECK(t.hw.writes.size() == 1 && t.hw.writes[0] == std::make_pair(MasterIrq, 0U));
    CHECK(t.d.state() == Decoder::Pending && !(t.hw.master & MasterControl));
    const unsigned reads = t.hw.reads;
    CHECK(!t.d.filter() && t.hw.reads == reads); // pending: inert, no MMIO
    Batch b;
    CHECK(t.d.collect(b) == Status::Ok);
    CHECK(b.master == MasterGtRcs && b.iir[0] == 0x101 && b.events[0] == 0x101 && b.events[1] == 0);
    CHECK(t.hw.count(gtIir(0)) == 1 && t.hw.writes.back() == std::make_pair(gtIir(0), 0x101U));
    CHECK(!(t.hw.master & MasterControl) && t.d.state() == Decoder::Servicing); // deferred work runs masked
    CHECK(t.d.rearm() == Status::Ok && (t.hw.master & MasterControl) && t.d.state() == Decoder::Armed);
    Batch none;
    CHECK(t.d.collect(none) == Status::Empty && !none.any());
}

void lateBitSurvivesExactAck() {
    Armed t;
    t.hw.raise(0, CsContextSwitch);
    bool injected = false;
    t.hw.afterRead = [&](uint32_t offset) {
        if (offset == gtIir(0) && !injected) { injected = true; t.hw.raise(0, uint32_t(CsContextSwitch) << 16); }
    };
    CHECK(t.d.filter());
    Batch b;
    CHECK(t.d.collect(b) == Status::Ok && b.iir[0] == CsContextSwitch && b.events[1] == 0);
    CHECK(t.hw.iir[0] == uint32_t(CsContextSwitch) << 16); // sampled-bit W1C kept the late BCS bit
    CHECK(t.d.rearm() == Status::Ok);
    CHECK(t.d.filter());
    Batch next;
    CHECK(t.d.collect(next) == Status::Ok && next.events[1] == CsContextSwitch && next.master == MasterGtBcs);
}

void unownedPendingFailsClosedWithoutAck() {
    const uint32_t foreign[] {MasterDePipeA, MasterDePort, MasterPcu, MasterGtPm, MasterGtGuc, MasterGtVcs1};
    for (uint32_t level : foreign) {
        Armed t(engineBit(Engine::Rcs0));
        t.hw.raise(0, CsUserInterrupt); t.hw.extraLevels = level;
        CHECK(t.d.filter());
        Batch b;
        CHECK(t.d.collect(b) == Status::Unowned && t.d.unownedSample() == level);
        CHECK(t.hw.count(gtIir(0)) == 0 && t.hw.iir[0] == CsUserInterrupt);
        CHECK(!(t.hw.master & MasterControl) && t.d.state() == Decoder::Fault && !b.any());
        CHECK(t.d.rearm() != Status::Ok && !(t.hw.master & MasterControl) && !t.d.filter());
    }
    Armed bcs(engineBit(Engine::Rcs0)); // BCS shares bank 0 but is not configured
    bcs.hw.ier[0] |= uint32_t(CsUserInterrupt) << 16; bcs.hw.iir[0] |= uint32_t(CsUserInterrupt) << 16;
    CHECK(bcs.d.filter());
    Batch b;
    CHECK(bcs.d.collect(b) == Status::Unowned && bcs.hw.count(gtIir(0)) == 0);
}

void unexpectedIirBitsNotAcknowledged() {
    Armed t(engineBit(Engine::Rcs0) | engineBit(Engine::Vecs0));
    t.hw.raise(3, CsUserInterrupt);
    t.hw.raise(0, CsUserInterrupt); t.hw.iir[0] |= 1U << 4; // never-enabled PIPE_CONTROL notify
    CHECK(t.d.filter());
    Batch b;
    CHECK(t.d.collect(b) == Status::UnexpectedBits && t.d.unexpectedIir(0) == 0x11);
    CHECK(t.hw.count(gtIir(0)) == 0 && t.hw.count(gtIir(3)) == 0 && t.hw.iir[3] == CsUserInterrupt);
    CHECK(t.d.state() == Decoder::Fault && !(t.hw.master & MasterControl));
}

void deviceLossAndIoFailures() {
    Armed firstRead; firstRead.hw.failRead = MasterIrq;
    CHECK(firstRead.d.filter() && firstRead.d.state() == Decoder::Fault && firstRead.hw.writes.empty());
    Batch failed; CHECK(firstRead.d.collect(failed) == Status::IoFailure);
    Armed gone; gone.hw.raise(0, CsUserInterrupt); gone.hw.gone = true;
    CHECK(gone.d.filter() && gone.hw.writes.empty() && gone.d.state() == Decoder::Fault);
    Batch b;
    CHECK(gone.d.collect(b) == Status::IoFailure);
    Armed iir; iir.hw.raise(0, CsUserInterrupt);
    CHECK(iir.d.filter());
    iir.hw.failRead = gtIir(0);
    CHECK(iir.d.collect(b) == Status::IoFailure && iir.hw.count(gtIir(0)) == 0);
    Armed power; power.hw.raise(0, CsUserInterrupt);
    CHECK(power.d.filter());
    power.hw.admitted = false;
    const size_t writes = power.hw.writes.size();
    CHECK(power.d.collect(b) == Status::Unavailable && power.hw.writes.size() == writes);
    Armed mask; mask.hw.raise(0, CsUserInterrupt); mask.hw.failWrite = MasterIrq;
    CHECK(mask.d.filter() && mask.d.state() == Decoder::Fault); // claim but cannot mask: fault
}

void stormDetected() {
    Armed t(engineBit(Engine::Rcs0));
    Status last = Status::Ok;
    unsigned rounds = 0;
    for (; rounds < 2 * Decoder::StormLimit && last == Status::Ok; ++rounds) {
        t.hw.extraLevels = MasterGtRcs; // master reports RCS, IIR empty ("master lied")
        if (!t.d.filter()) break;
        Batch b; last = t.d.collect(b);
        if (last == Status::Ok) { CHECK(!b.any() && t.hw.count(gtIir(0)) == 0); last = t.d.rearm(); }
    }
    CHECK(last == Status::Storm && rounds == Decoder::StormLimit);
    CHECK(!(t.hw.master & MasterControl));
}

void rearmRaceRemasks() {
    Armed t;
    t.hw.raise(0, CsUserInterrupt);
    CHECK(t.d.filter());
    Batch b; CHECK(t.d.collect(b) == Status::Ok);
    t.hw.raise(0, CsContextSwitch); // new work while masked
    bool claimed = false;
    t.hw.beforeWrite = [&](uint32_t offset, uint32_t value) {
        if (offset == MasterIrq && value == MasterControl && !claimed) claimed = t.d.filter();
    };
    CHECK(t.d.rearm() == Status::Ok);
    CHECK(claimed && t.d.state() == Decoder::Pending && !(t.hw.master & MasterControl));
    t.hw.beforeWrite = nullptr;
    CHECK(t.d.collect(b) == Status::Ok && b.events[0] == CsContextSwitch);
}

void maskEngineAndStop() {
    Armed t;
    t.hw.raise(0, uint32_t(CsMasterError) << 16);
    CHECK(t.d.filter());
    Batch b; CHECK(t.d.collect(b) == Status::Ok && b.events[1] == CsMasterError);
    CHECK(t.d.maskEngine(Engine::Bcs0) == Status::Ok);
    CHECK(t.hw.imr[0] == 0xfffff6f6U && t.hw.ier[0] == 0x00000909U && t.d.enabled(0) == 0x909);
    CHECK(t.d.ownedMaster() == MasterGtRcs && t.d.engines() == engineBit(Engine::Rcs0));
    CHECK(t.d.maskEngine(Engine::Vcs0) == Status::Invalid); // not configured
    CHECK(t.d.rearm() == Status::Ok);
    t.hw.raise(0, uint32_t(CsMasterError) << 16);
    CHECK(!t.d.filter()); // masked engine cannot latch
    t.hw.writes.clear(); t.hw.reads = 0;
    CHECK(t.d.finishStop() == Status::Invalid && t.hw.writes.empty()); // must begin first
    CHECK(t.d.beginStop() == Decoder::Armed);
    t.hw.raise(0, CsUserInterrupt);
    CHECK(!t.d.filter() && t.hw.reads == 0 && t.hw.writes.empty()); // inert during drain window
    CHECK(t.d.finishStop() == Status::Ok && t.hw.writes.front() == std::make_pair(MasterIrq, 0U));
    for (unsigned bank = 0; bank < GtBanks; ++bank) CHECK(t.hw.ier[bank] == 0 && t.hw.imr[bank] == 0xffffffffU && t.hw.iir[bank] == 0);
    CHECK(t.d.state() == Decoder::Unconfigured && !(t.hw.master & MasterControl) && !t.hw.deWrite);
    CHECK(t.d.finishStop() == Status::Ok); // already proven masked
    Armed unpowered; unpowered.d.beginStop(); unpowered.hw.admitted = false;
    CHECK(unpowered.d.finishStop() == Status::Unavailable && unpowered.d.state() == Decoder::Stopping);
}

// ---------------- HOSTED IOKit shim adapter tests ----------------
struct Rig {
    FakeGlk hw;
    IOPCIDevice *device {new IOPCIDevice};
    IOService *owner {new IOService};
    IOWorkLoop *loop {new IOWorkLoop};
    MellowGlkInterrupt *irq {MellowGlkInterrupt::create()};
    std::vector<std::pair<Engine, uint16_t>> calls;
    std::vector<std::pair<Status, uint8_t>> faults;
    Handling reply {Handling::Done};
    unsigned more {}; bool maskedDuringHandler {true};
    Engine failEngine {Engine::Vecs0};
    bool callerOwns {true};
    bool reenterDetach {}, loseAdmission {};
    Status reentrantStatus {Status::Ok};
    Rig() {
        device->set16(kIOPCIConfigVendorID, 0x8086); device->set16(kIOPCIConfigDeviceID, 0x3185);
        device->config[kIOPCIConfigRevisionID] = 0x06;
        device->set16(kIOPCIConfigSubSystemVendorID, 0x17aa); device->set16(kIOPCIConfigSubSystemID, 0x3e77);
        device->set16(kIOPCIConfigCommand, 0x0006);
        device->interruptCount = 2;
        device->interruptTypes[0] = kIOInterruptTypeLevel; device->interruptTypes[1] = kIOInterruptTypePCIMessaged;
        owner->provider = device; device->opener = owner;
        shim::log().clear();
    }
    void releaseCaller() { CHECK(irq && callerOwns); callerOwns = false; irq->release(); }
    ~Rig() {
        CHECK(!irq || !irq->attached());
        if (irq && callerOwns) irq->release();
        if (device) device->release();
        if (owner) owner->release();
        if (loop) loop->release();
    }
    static Handling engine(void *opaque, uint64_t epoch, Engine e, uint16_t bits) {
        auto &r = *static_cast<Rig *>(opaque);
        r.calls.push_back({e, bits});
        if (r.reenterDetach) r.reentrantStatus = r.irq->detach();
        if (r.loseAdmission) r.hw.admitted = false;
        r.maskedDuringHandler &= epoch == 77 && !(r.hw.master & MasterControl);
        if (r.more) { --r.more; return Handling::More; }
        return e == r.failEngine ? Handling::Failed : r.reply;
    }
    static void fault(void *opaque, uint64_t, Status status, uint8_t engines) {
        static_cast<Rig *>(opaque)->faults.push_back({status, engines});
    }
    MellowNativeGlk::InterruptHandlers handlers() { return {this, engine, fault}; }
    Status attach(int index = 1) { return irq->attach(device, owner, loop, index, config(), hw.ops(), handlers()); }
    size_t at(const std::string &event) const {
        for (size_t i = 0; i < shim::log().size(); ++i) if (shim::log()[i] == event) return i;
        return SIZE_MAX;
    }
};

void adapterRejectsWithoutIdentity() {
    for (unsigned which = 0; which < 9; ++which) {
        Rig r;
        const int deviceRefs = r.device->retainCount(), ownerRefs = r.owner->retainCount();
        int index = 1;
        auto handlers = r.handlers();
        if (which == 0) r.loop->gate = false;
        else if (which == 1) r.device->opener = nullptr;
        else if (which == 2) r.device->set16(kIOPCIConfigDeviceID, 0x3184);
        else if (which == 3) r.device->powerState = kIOPCIDeviceDozeState;
        else if (which == 4) r.device->set16(kIOPCIConfigCommand, 0x0002);
        else if (which == 5) index = 0; // legacy INTx is not accepted
        else if (which == 6) handlers.fault = nullptr;
        else if (which == 7) r.device->slot = 3;
        else r.owner->inactive = true;
        CHECK(r.irq->attach(r.device, r.owner, r.loop, index, config(), r.hw.ops(), handlers) != Status::Ok);
        CHECK(r.hw.writes.empty() && r.at("os.register") == SIZE_MAX && !r.irq->attached());
        CHECK(r.device->retainCount() == deviceRefs && r.owner->retainCount() == ownerRefs && r.irq->retainCount() == 1);
    }
    Rig r; IOFilterInterruptEventSource::failCreate() = true;
    CHECK(r.attach() == Status::Unavailable && !r.irq->attached() && !(r.hw.master & MasterControl));
    IOFilterInterruptEventSource::failCreate() = false;
    CHECK(r.device->retainCount() == 1 && r.irq->retainCount() == 1);
}

void adapterLifecycle() {
    const int liveBefore = shim::liveObjects(), freedBefore = shim::freedObjects();
    {
        Rig r;
        CHECK(MellowGlkInterrupt::messagedIndex(r.device) == 1);
        CHECK(r.attach() == Status::Ok && r.irq->attached());
        CHECK(r.at("mmio.w 44200=0") < r.at("os.register") && r.at("os.register") < r.at("os.disable+drain"));
        CHECK(r.device->retainCount() == 2 && r.owner->retainCount() == 2 && r.loop->retainCount() == 2 && r.irq->retainCount() == 2);
        CHECK(!r.device->vectorEnabled[1] && !(r.hw.master & MasterControl));
        CHECK(r.attach() == Status::Invalid);
        CHECK(r.irq->arm() == Status::Ok && r.at("os.enable") < r.at("mmio.w 44200=80000000"));
        // Caller drops its reference while attached: internal reference keeps it alive.
        r.releaseCaller();
        CHECK(r.irq->retainCount() == 1 && shim::freedObjects() == freedBefore);
        r.hw.raise(0, CsUserInterrupt);
        CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
        CHECK(r.loop->run() == 1 && r.calls.size() == 1 && r.calls[0] == std::make_pair(Engine::Rcs0, CsUserInterrupt));
        CHECK(r.maskedDuringHandler && (r.hw.master & MasterControl) && r.irq->deliveries() == 1);
        CHECK(!IOFilterInterruptEventSource::deliver(r.device, 1)); // nothing pending: not claimed
        // Power proof lost: detach refuses unproven cleanup and keeps everything.
        r.device->powerState = kIOPCIDeviceOffState;
        shim::log().clear();
        CHECK(r.irq->detach() == Status::Unavailable);
        CHECK(r.device->registered[1] != nullptr && r.at("os.unregister") == SIZE_MAX && r.at("os.disable+drain") != SIZE_MAX);
        CHECK(r.device->retainCount() == 2 && r.irq->retainCount() == 1 && r.irq->attached());
        r.device->powerState = kIOPCIDeviceOnState;
        shim::log().clear();
        CHECK(r.irq->detach() == Status::Ok);
        CHECK(r.at("mmio.w 44200=0") < r.at("os.unregister") && r.device->registered[1] == nullptr);
        CHECK(r.device->retainCount() == 1 && r.owner->retainCount() == 1 && r.loop->retainCount() == 1);
        CHECK(shim::freedObjects() == freedBefore + 2); // source and adapter freed only after proven detach
        r.irq = nullptr;
        r.device->release(); r.owner->release(); r.loop->release();
        r.device = nullptr; r.owner = nullptr; r.loop = nullptr;
    }
    CHECK(shim::liveObjects() == liveBefore);
}

void adapterDeferredRouting() {
    Rig r;
    CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
    r.more = 2;
    r.hw.raise(0, CsContextSwitch | (uint32_t(CsContextSwitch) << 16));
    CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
    r.loop->run();
    CHECK(r.calls.size() == 4 && r.calls[0] == r.calls[3] && r.calls[1] == r.calls[2]);
    CHECK(r.maskedDuringHandler && (r.hw.master & MasterControl) && r.faults.empty());
    r.calls.clear(); r.failEngine = Engine::Bcs0;
    r.hw.raise(0, uint32_t(CsMasterError) << 16);
    CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
    r.loop->run();
    CHECK(r.faults.size() == 1 && r.faults[0].first == Status::Faulted && r.faults[0].second == engineBit(Engine::Bcs0));
    CHECK((r.hw.imr[0] & 0x09090000U) == 0x09090000U && !(r.hw.ier[0] & 0xffff0000U));
    CHECK((r.hw.master & MasterControl) && !r.irq->faulted()); // RCS still served
    r.calls.clear(); r.failEngine = Engine::Vecs0;
    r.hw.raise(0, CsUserInterrupt);
    CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
    r.loop->run();
    CHECK(r.calls.size() == 1 && r.calls[0].first == Engine::Rcs0);
    r.more = 2 * MellowGlkInterrupt::MorePassLimit; // endless More is bounded and fails closed
    r.hw.raise(0, CsUserInterrupt);
    CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
    r.loop->run(8 * MellowGlkInterrupt::MorePassLimit);
    CHECK(r.irq->faulted() && !r.device->vectorEnabled[1] && !(r.hw.master & MasterControl));
    CHECK(r.irq->detach() == Status::Ok);
    r.releaseCaller();
    r.irq = nullptr;
}

void adapterFailsClosed() {
    {
        Rig r;
        CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
        r.hw.extraLevels = MasterDePipeA; r.hw.raise(0, CsUserInterrupt);
        CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
        r.loop->run();
        CHECK(r.calls.empty() && r.faults.size() == 1 && r.faults[0] == std::make_pair(Status::Unowned, uint8_t(0xff)));
        CHECK(r.irq->faulted() && !r.device->vectorEnabled[1] && !(r.hw.master & MasterControl));
        CHECK(r.irq->arm() != Status::Ok && !(r.hw.master & MasterControl));
        CHECK(r.irq->detach() == Status::Ok);
        r.releaseCaller();
        r.irq = nullptr;
    }
    {
        Rig r;
        CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
        r.hw.raise(0, CsUserInterrupt);
        CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
        r.device->powerState = kIOPCIDeviceOffState; r.hw.admitted = false; // power lost before workloop
        const size_t writes = r.hw.writes.size();
        r.loop->run();
        CHECK(r.calls.empty() && r.faults.size() == 1 && r.faults[0].first == Status::Unavailable);
        CHECK(r.hw.writes.size() == writes && !r.device->vectorEnabled[1]);
        CHECK(r.irq->detach() == Status::Unavailable && r.irq->attached());
        r.device->powerState = kIOPCIDeviceOnState; r.hw.admitted = true;
        CHECK(r.irq->detach() == Status::Ok);
        r.releaseCaller();
        r.irq = nullptr;
    }
    {
        Rig r;
        CHECK(r.attach() == Status::Ok);
        r.device->registryId = 0x2000; // a different registry entry now answers
        const size_t writes = r.hw.writes.size();
        CHECK(r.irq->arm() == Status::Unavailable && r.hw.writes.size() == writes && !r.device->vectorEnabled[1]);
        r.device->registryId = 0x1000;
        CHECK(r.irq->quiesce() == Status::Ok && r.irq->arm() == Status::Invalid);
        Configuration next = config(); next.epoch = 78;
        CHECK(r.irq->resume(next, r.hw.ops()) == Status::Unavailable); // epoch 78 not admitted by owner
        CHECK(r.irq->resume(config(), r.hw.ops()) == Status::Invalid); // stale epoch cannot reopen
        r.hw.epoch = 78;
        CHECK(r.irq->resume(next, r.hw.ops()) == Status::Ok && r.irq->arm() == Status::Ok);
        CHECK(r.irq->detach() == Status::Ok);
        r.releaseCaller();
        r.irq = nullptr;
    }
}

void adapterPartialAcquisitionAndDrainFailures() {
    { Rig r; r.hw.failWrite = gtIer(1);
      CHECK(r.attach() == Status::IoFailure && r.irq->attached());
      CHECK(r.device->retainCount() == 2 && r.irq->retainCount() == 2);
      CHECK(!r.device->registered[1]); // native refs retained even before source creation
      r.hw.failWrite = 0; CHECK(r.irq->detach() == Status::Ok); }
    { Rig r; r.loop->addResult = kIOReturnError;
      CHECK(r.attach() == Status::IoFailure && !r.irq->attached());
      CHECK(r.at("os.register") < r.at("os.unregister"));
      CHECK(!r.device->registered[1] && r.device->retainCount() == 1); }
    { Rig r; r.device->disableResult = kIOReturnError;
      CHECK(r.attach() == Status::IoFailure && r.irq->attached() && r.device->registered[1]);
      CHECK(r.irq->detach() == Status::IoFailure && r.device->retainCount() == 2);
      r.device->disableResult = kIOReturnSuccess; CHECK(r.irq->detach() == Status::Ok); }
    { Rig r; CHECK(r.attach() == Status::Ok); r.device->enableResult = kIOReturnError;
      CHECK(r.irq->arm() == Status::IoFailure && r.irq->faulted());
      CHECK(!r.device->vectorEnabled[1] && !(r.hw.master & MasterControl));
      CHECK(r.faults.size() == 1 && r.faults[0].second == 0xff);
      CHECK(r.irq->detach() == Status::Ok); }
    { Rig r; CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
      r.hw.raise(0, CsUserInterrupt); CHECK(IOFilterInterruptEventSource::deliver(r.device, 1));
      r.loop->removeResult = kIOReturnError;
      CHECK(r.irq->detach() == Status::IoFailure && r.irq->attached());
      CHECK(r.device->registered[1] && !r.device->vectorEnabled[1]);
      CHECK(r.loop->run() == 1 && r.calls.empty()); // queued stale action cannot touch hardware
      r.loop->removeResult = kIOReturnSuccess; CHECK(r.irq->detach() == Status::Ok); }
}

void adapterCallbacksCannotUnbindActiveAction() {
    { Rig r; CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
      r.reenterDetach = true; r.hw.raise(0, CsUserInterrupt);
      CHECK(IOFilterInterruptEventSource::deliver(r.device, 1)); r.loop->run();
      CHECK(r.reentrantStatus == Status::Invalid && r.irq->attached());
      CHECK(r.device->registered[1] && (r.hw.master & MasterControl));
      CHECK(r.irq->detach() == Status::Ok); }
    { Rig r; CHECK(r.attach() == Status::Ok && r.irq->arm() == Status::Ok);
      r.loseAdmission = true; r.hw.raise(0, CsUserInterrupt | (uint32_t(CsUserInterrupt) << 16));
      CHECK(IOFilterInterruptEventSource::deliver(r.device, 1)); r.loop->run();
      CHECK(r.calls.size() == 1 && r.irq->faulted() && !r.device->vectorEnabled[1]);
      CHECK(r.faults.size() == 1 && r.faults[0].first == Status::Unavailable);
      CHECK(r.irq->detach() == Status::Unavailable && r.irq->attached());
      r.hw.admitted = true; CHECK(r.irq->detach() == Status::Ok); }
}
}

int main() {
    constantsMatchPinnedSources();
    configureRejectsBeforeEffects();
    unownedDomainsRejectedBeforeEffects();
    configureFollowsGen8ResetAndInit();
    configureFailuresFailClosed();
    filterBoundedAndNotOurs();
    claimSampleAckExactRoute();
    lateBitSurvivesExactAck();
    unownedPendingFailsClosedWithoutAck();
    unexpectedIirBitsNotAcknowledged();
    deviceLossAndIoFailures();
    stormDetected();
    rearmRaceRemasks();
    maskEngineAndStop();
    adapterRejectsWithoutIdentity();
    adapterLifecycle();
    adapterDeferredRouting();
    adapterFailsClosed();
    adapterPartialAcquisitionAndDrainFailures();
    adapterCallbacksCannotUnbindActiveAction();
    CHECK(shim::liveObjects() == 0);
    printf("native_glk_interrupt_tests: %d checks, %d failures (HOSTED register model + IOKit shim; no hardware)\n",
           checks, failures);
    return failures ? 1 : 0;
}

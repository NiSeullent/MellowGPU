// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// HOSTED INJECTED TRANSPORT. FakeBcs is a CPU model of the BCS0 register file
// and of an execlists engine that interprets ring bytes with the seeded
// decodeCommand(). It only drives the production Engine bodies: no GPU, MMIO,
// DMA, interrupt or PCI access happens, and no pass here is hardware evidence.
#include "Drivers/NativeGlk/BcsExecution.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace B = MellowNativeGlk::Bcs;
namespace Lrc = MellowNativeGlk::Lrc;
using Engine = MellowNativeGlk::Engine;
using MellowNativeGlk::Command;
using MellowNativeGlk::CommandKind;
using MellowNativeGlk::FlushPostSync;
using MellowNativeGlk::Generation;
using MellowNativeGlk::PphwspScratchIndex;
using MellowNativeGlk::ProbeReading;
using MellowNativeGlk::Site;
using MellowNativeGlk::checkCommandImage;
using MellowNativeGlk::decodeCommand;
using Status = B::Status;

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {
constexpr uint64_t Epoch = 7;
enum class Gpu { Normal, Deferred, NoCsb, NoNonce, ForeignNonce, StaleBreadcrumb, ForeignBreadcrumb,
                 WrongCcid, MmioFallback, MmioAllOnes, PromoteZeroCcid, NoSavedHead,
                 DoubleComplete, CompleteWithoutPromote };
struct Buffer {
    B::LeaseKind kind {};
    uint8_t *cpu {};
    uint64_t ggtt {}, bytes {}, generation {};
    bool live {true}, retainFails {};
    int holds {};
};
struct World {
    Buffer hwsp, context, ring, probe, probe2;
    uint64_t rootDma {0x7654000}, rootGeneration {3};
    bool rootLive {true};
    int rootHolds {};
    uint32_t forcewake {B::ForcewakeAllGen9};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::vector<std::string> events;
    uint32_t regs[B::WindowBytes / 4] {};
    Gpu mode {Gpu::Normal};
    uint32_t elsp[4] {}, elspCount {}, hwWrite {5}, userInterrupts {}, delays {}, resets {};
    uint64_t mmioCsb[6] {};
    bool deviceGone {}, stopRequested {}, idle {true}, resetRequested {}, catError {}, resetOk {true};
    uint32_t esr {}, readyAfter {}, resetPolls {}, ringHeadReg {}, ringTailReg {};
    uint64_t pendingDescriptor {};
};
World *w;

uint8_t *page(size_t bytes) {
    auto *p = static_cast<uint8_t *>(std::aligned_alloc(4096, bytes));
    if (!p) std::abort();
    std::memset(p, 0x5a, bytes);
    return p;
}
uint32_t ld(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void st(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
Buffer *byGgtt(uint64_t address) {
    for (Buffer *b : {&w->hwsp, &w->context, &w->ring, &w->probe, &w->probe2})
        if (address >= b->ggtt && address + 4 <= b->ggtt + b->bytes) return b;
    return nullptr;
}
void gpuStore(uint64_t address, uint32_t value) {
    Buffer *b = byGgtt(address);
    if (!b) std::abort();
    st(b->cpu + (address - b->ggtt), value);
}
void csbPush(uint32_t status, uint32_t ccid) {
    w->hwWrite = (w->hwWrite + 1) % 6;
    const uint64_t entry = uint64_t(ccid) << 32 | status;
    uint8_t *slot = w->hwsp.cpu + (B::HwspCsbBufferDword + 2 * w->hwWrite) * 4;
    w->mmioCsb[w->hwWrite] = entry;
    if (w->mode == Gpu::MmioFallback) { st(slot, ~0u); st(slot + 4, ~0u); }
    else if (w->mode == Gpu::MmioAllOnes) { st(slot, ~0u); st(slot + 4, ~0u); w->mmioCsb[w->hwWrite] = ~0ULL; }
    else { st(slot, uint32_t(entry)); st(slot + 4, uint32_t(entry >> 32)); }
    st(w->hwsp.cpu + B::HwspCsbWriteDword * 4, w->hwWrite);
}

// The CPU engine model: context restore, ring interpretation, context save, CSB.
void execute(uint64_t descriptor) {
    Lrc::DescriptorFields d;
    if (Lrc::decodeDescriptor(Lrc::Engine::Bcs0, descriptor, d) != Lrc::Status::Ok) std::abort();
    Buffer *ctx = byGgtt(d.lrca);
    if (ctx != &w->context) std::abort();
    Lrc::RegisterState regs;
    if (Lrc::decodeRegisterState(Lrc::Engine::Bcs0, ctx->cpu, ctx->bytes, regs) != Lrc::Status::Ok) std::abort();
    if (regs.ringStart != w->ring.ggtt) std::abort();
    uint32_t pos = regs.ringHead & B::Bit::HeadAddr;
    const uint32_t tail = regs.ringTail & B::Bit::TailAddr, size = uint32_t(w->ring.bytes);
    while (pos != tail) {
        Command c; uint32_t n = 0;
        if (decodeCommand(Generation::Gen9, Engine::Bcs0, Site::Ring, w->ring.cpu + pos, size - pos, c, n) != MellowNativeGlk::Status::Ok) std::abort();
        if (c.kind == CommandKind::StoreDwordImm) {
            if (w->mode == Gpu::ForeignNonce) gpuStore(c.address, c.value ^ 0x100);
            else if (w->mode != Gpu::NoNonce) gpuStore(c.address, c.value);
        } else if (c.kind == CommandKind::FlushDw && c.postSync == FlushPostSync::GgttStoreDword) {
            if (w->mode == Gpu::ForeignBreadcrumb) gpuStore(c.address, c.value + 7);
            else if (w->mode != Gpu::StaleBreadcrumb) gpuStore(c.address, c.value);
        } else if (c.kind == CommandKind::FlushDw) {
            st(ctx->cpu + PphwspScratchIndex, 0);
        } else if (c.kind == CommandKind::UserInterrupt) {
            ++w->userInterrupts;
        }
        pos = (pos + n * 4) & (size - 1);
    }
    if (w->mode != Gpu::NoSavedHead)
        st(ctx->cpu + Lrc::StateOffset + B::ContextRingHeadDword * 4, tail | (regs.ringHead & ~B::Bit::HeadAddr));
    if (w->mode == Gpu::NoCsb) return;
    if (w->mode != Gpu::CompleteWithoutPromote) csbPush(0x1, w->mode == Gpu::PromoteZeroCcid ? 0 : d.contextId);
    csbPush(0x18, w->mode == Gpu::WrongCcid ? d.contextId + 32 : d.contextId);
    if (w->mode == Gpu::DoubleComplete) csbPush(0x18, d.contextId);
}

uint32_t &reg(uint32_t offset) { return w->regs[(offset - B::MmioBase) / 4]; }
uint32_t mmioRead(void *, uint32_t offset) {
    if (!B::inWindow(offset)) std::abort();
    w->events.push_back("r");
    if (w->deviceGone) return ~0u;
    if (offset == B::Reg::MiMode)
        return (w->stopRequested ? B::Bit::StopRing : 0) | (w->idle ? B::Bit::ModeIdle : 0);
    if (offset == B::Reg::ResetCtl) {
        uint32_t v = w->catError ? B::Bit::ResetCatError : 0;
        if (w->resetRequested && ++w->resetPolls > w->readyAfter) v |= B::Bit::ResetReady;
        return v;
    }
    if (offset == B::Reg::Esr) return w->esr;
    if (offset == B::Reg::RingHead) return w->ringHeadReg;
    if (offset == B::Reg::RingTail) return w->ringTailReg;
    if (offset >= B::Reg::ExeclistsStatusBuf && offset < B::Reg::ExeclistsStatusBuf + 48) {
        const uint32_t i = (offset - B::Reg::ExeclistsStatusBuf) / 8;
        return (offset & 4) ? uint32_t(w->mmioCsb[i] >> 32) : uint32_t(w->mmioCsb[i]);
    }
    return reg(offset);
}
void mmioWrite(void *, uint32_t offset, uint32_t value) {
    if (!B::inWindow(offset)) std::abort();
    w->writes.push_back({offset, value});
    w->events.push_back("w");
    if (offset == B::Reg::MiMode && (value >> 16) & B::Bit::StopRing)
        w->stopRequested = value & B::Bit::StopRing;
    if (offset == B::Reg::ResetCtl) {
        if ((value >> 16) & B::Bit::ResetRequest) w->resetRequested = value & B::Bit::ResetRequest;
        if ((value >> 16) & B::Bit::ResetCatError && value & B::Bit::ResetCatError) w->catError = false;
    }
    reg(offset) = value;
    if (offset == B::Reg::Elsp) {
        w->elsp[w->elspCount++ % 4] = value;
        if (w->elspCount % 4 == 0) {
            if (w->elsp[0] || w->elsp[1]) std::abort(); // port 1 must be empty
            const uint64_t port0 = uint64_t(w->elsp[2]) << 32 | w->elsp[3];
            if (w->mode == Gpu::Deferred) w->pendingDescriptor = port0; else execute(port0);
        }
    }
}
void mmioDelay(void *, uint32_t) { ++w->delays; }
void mmioFence(void *, B::Fence f) { w->events.push_back(f == B::Fence::Store ? "wmb" : f == B::Fence::Load ? "rmb" : "mb"); }
bool fwHeld(void *, uint64_t epoch, uint32_t domains) { return epoch == Epoch && (domains & ~w->forcewake) == 0; }
bool resetOk(void *, uint64_t epoch) { ++w->resets; w->hwWrite = 5; return epoch == Epoch && w->resetOk; }

Buffer *find(const B::Lease &l) {
    for (Buffer *b : {&w->hwsp, &w->context, &w->ring, &w->probe, &w->probe2})
        if (b->kind == l.kind && b->ggtt == l.address && b->cpu == l.cpu && b->generation == l.generation && b->bytes == l.bytes) return b;
    return nullptr;
}
bool holds(void *, const B::Lease &l) {
    if (l.deviceEpoch != Epoch) return false;
    if (l.kind == B::LeaseKind::PpgttRoot) return w->rootLive && l.address == w->rootDma && l.generation == w->rootGeneration;
    Buffer *b = find(l);
    return b && b->live;
}
bool retain(void *c, const B::Lease &l) {
    if (!holds(c, l)) return false;
    if (l.kind == B::LeaseKind::PpgttRoot) { ++w->rootHolds; return true; }
    Buffer *b = find(l);
    if (b->retainFails) return false;
    ++b->holds; w->events.push_back("retain"); return true;
}
bool release(void *, const B::Lease &l) {
    if (l.kind == B::LeaseKind::PpgttRoot) { if (!w->rootHolds) return false; --w->rootHolds; return true; }
    Buffer *b = find(l);
    if (!b || !b->holds) return false;
    --b->holds; return true;
}
bool syncLease(void *c, const B::Lease &l, B::CacheOp op) {
    if (!holds(c, l)) return false;
    w->events.push_back(op == B::CacheOp::ForDevice ? "dev" : "cpu");
    return true;
}

B::Lease leaseOf(const Buffer &b) {
    B::Lease l; l.kind = b.kind; l.owner = 1; l.slot = 0; l.generation = b.generation;
    l.deviceEpoch = Epoch; l.address = b.ggtt; l.bytes = b.bytes; l.cpu = b.cpu; return l;
}
B::Platform platform() {
    B::Platform p; p.deviceEpoch = Epoch;
    p.mmio = {nullptr, mmioRead, mmioWrite, mmioDelay, mmioFence};
    p.forcewake = {nullptr, fwHeld};
    p.reset = {nullptr, resetOk};
    p.leases = {nullptr, holds, retain, release, syncLease};
    return p;
}
B::Bindings bindings() {
    B::Bindings b; b.statusPage = leaseOf(w->hwsp); b.context = leaseOf(w->context); b.ring = leaseOf(w->ring);
    b.ppgttRoot.kind = B::LeaseKind::PpgttRoot; b.ppgttRoot.owner = 1; b.ppgttRoot.generation = w->rootGeneration;
    b.ppgttRoot.deviceEpoch = Epoch; b.ppgttRoot.address = w->rootDma; b.ppgttRoot.bytes = 4096;
    return b;
}
void fresh(Gpu mode = Gpu::Normal, uint32_t ringBytes = 4096) {
    static World storage;
    for (Buffer *b : {&storage.hwsp, &storage.context, &storage.ring, &storage.probe, &storage.probe2}) std::free(b->cpu);
    storage = World {};
    w = &storage;
    w->mode = mode;
    w->hwsp = {B::LeaseKind::StatusPage, page(4096), 0x10000, 4096, 11};
    w->context = {B::LeaseKind::Context, page(8192), 0x20000, 8192, 12};
    w->ring = {B::LeaseKind::Ring, page(ringBytes), 0x40000, ringBytes, 13};
    w->probe = {B::LeaseKind::ProbeTarget, page(4096), 0x30000, 4096, 14};
    w->probe2 = {B::LeaseKind::ProbeTarget, page(4096), 0x31000, 4096, 15};
}
size_t eventIndex(const char *name, size_t from = 0) {
    for (size_t i = from; i < w->events.size(); ++i) if (w->events[i] == name) return i;
    return w->events.size();
}
void ready(B::Engine &e, uint32_t seqno = 100) {
    CHECK(e.attach(platform(), bindings(), seqno) == Status::Ok);
    CHECK(e.initialize() == Status::Ok);
    CHECK(e.state() == B::EngineState::Ready);
}

void testAttach() {
    fresh();
    B::Engine e;
    B::Platform p = platform(); p.leases.retain = nullptr;
    CHECK(e.attach(p, bindings(), 1) == Status::Unavailable);
    p = platform(); p.forcewake.held = nullptr;
    CHECK(e.attach(p, bindings(), 1) == Status::Unavailable);
    B::Bindings b = bindings(); b.statusPage.bytes = 8192;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.context.bytes = 4096;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.ring.bytes = 4096 * 3;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.ring.address = 0xFFFFF000;  b.ring.bytes = 8192;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.context.address = b.statusPage.address;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.ring.cpu = b.context.cpu + 4096;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.ppgttRoot.address = 1ULL << 39;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    b = bindings(); b.ring.deviceEpoch = Epoch + 1;
    CHECK(e.attach(platform(), b, 1) == Status::InvalidArgument);
    CHECK(e.attach(platform(), bindings(), ~0u) == Status::InvalidArgument);
    w->rootLive = false;
    CHECK(e.attach(platform(), bindings(), 1) == Status::Ownership);
    w->rootLive = true;
    CHECK(e.state() == B::EngineState::Detached && w->writes.empty() && w->events.empty());
    CHECK(e.attach(platform(), bindings(), 1) == Status::Ok);
    CHECK(e.attach(platform(), bindings(), 1) == Status::InvalidState);
    CHECK(w->writes.empty()); // attach has no register effect
}

void testInitialize() {
    fresh();
    B::Engine e;
    CHECK(e.attach(platform(), bindings(), 100) == Status::Ok);
    w->forcewake = B::ForcewakeGt;
    CHECK(e.initialize() == Status::ForcewakeNotHeld && w->writes.empty() && w->hwsp.holds == 0);
    w->forcewake = B::ForcewakeAllGen9;
    CHECK(e.initialize() == Status::Ok);
    const std::vector<std::pair<uint32_t, uint32_t>> expect = {
        {B::Reg::ContextStatusPtr, 0xffff0505}, {B::Reg::ContextStatusPtr, 0xffff0505},
        {B::Reg::Hwstam, ~0u}, {B::Reg::ModeGen7, 0x80008000}, {B::Reg::MiMode, 0x01000000},
        {B::Reg::HwsPga, 0x10000}, {B::Reg::Emr, ~0u}, {B::Reg::Eir, ~0u}, {B::Reg::Emr, 0xFFFFFFFE}};
    CHECK(w->writes == expect);
    CHECK(ld(w->hwsp.cpu + B::HwspCsbWriteDword * 4) == 5 && e.csbHead() == 5);
    for (int i = 0; i < 12; ++i) CHECK(ld(w->hwsp.cpu + (B::HwspCsbBufferDword + i) * 4) == ~0u);
    CHECK(ld(w->hwsp.cpu + B::HwspSeqnoDword * 4) == 100 && ld(w->hwsp.cpu + B::HwspPreemptDword * 4) == 0);
    Lrc::RegisterState regs;
    CHECK(Lrc::decodeRegisterState(Lrc::Engine::Bcs0, w->context.cpu, 8192, regs) == Lrc::Status::Ok);
    CHECK(regs.ringStart == 0x40000 && regs.ringHead == 0 && regs.ringTail == 0 && regs.listClosed);
    CHECK(regs.pdpLdw[0] == uint32_t(w->rootDma) && regs.pdpUdw[0] == 0);
    CHECK(w->hwsp.holds == 1 && w->ring.holds == 0);
    // wmb between the HWSP write pointer and the all-ones CSB fill.
    CHECK(eventIndex("wmb") < eventIndex("dev"));

    fresh(); w->esr = 0x10;
    B::Engine bad;
    CHECK(bad.attach(platform(), bindings(), 1) == Status::Ok);
    CHECK(bad.initialize() == Status::EngineInError && bad.state() == B::EngineState::Faulted);
    CHECK(bad.submitStore(leaseOf(w->probe), 0xC0FFEE01, 0x11111111) == Status::InvalidState);
    fresh(); w->deviceGone = true;
    B::Engine lost;
    CHECK(lost.attach(platform(), bindings(), 1) == Status::Ok);
    CHECK(lost.initialize() == Status::DeviceLost && lost.state() == B::EngineState::DeviceLost);
    CHECK(lost.detach() == Status::Busy); // HWSP stays pinned: writes cannot be proven stopped
}

void testFirstStore() {
    fresh();
    B::Engine e; ready(e);
    w->writes.clear(); w->events.clear();
    CHECK(e.submitStore(leaseOf(w->probe), 0xC0FFEE01, 0x11111111) == Status::Ok);
    const B::RequestRecord r = e.request();
    // execlists_submit_ports(): exactly four ELSP writes, port 1 then port 0.
    CHECK(w->writes.size() == 4);
    for (auto &x : w->writes) CHECK(x.first == B::Reg::Elsp);
    CHECK(w->writes[0].second == 0 && w->writes[1].second == 0);
    CHECK(w->writes[2].second == uint32_t(r.descriptor >> 32) && w->writes[3].second == uint32_t(r.descriptor));
    Lrc::DescriptorFields d;
    CHECK(Lrc::decodeDescriptor(Lrc::Engine::Bcs0, r.descriptor, d) == Lrc::Status::Ok);
    CHECK(d.lrca == 0x20000 && d.contextId == r.contextId && r.contextId == 32 && d.forceRestore);
    // Holds before sealing bytes; cache cleaning, then wmb, then the first ELSP write.
    const size_t firstWrite = eventIndex("w");
    CHECK(eventIndex("retain") < eventIndex("dev") && eventIndex("wmb") < firstWrite);
    CHECK(eventIndex("dev", eventIndex("retain")) < eventIndex("wmb"));
    size_t n = 0;
    CHECK(checkCommandImage(Generation::Gen9, Engine::Bcs0, Site::Ring, w->ring.cpu, 64, n) == MellowNativeGlk::Status::Ok && n > 0);
    CHECK(r.seqno == 101 && r.tail == 56 && r.waTail == 64 && e.ringEmit() == 64);
    CHECK(w->ring.holds == 1 && w->context.holds == 1 && w->probe.holds == 1 && w->rootHolds == 1);
    CHECK(w->userInterrupts == 1 && e.interrupts() == 0); // the model's MI_USER_INTERRUPT is not consumed
    CHECK(e.poll() == Status::Ok);
    const B::RequestRecord &done = e.request();
    CHECK(done.phase == B::RequestPhase::Completed);
    CHECK(done.evidence.promoted && done.evidence.csbCompleted && done.evidence.breadcrumb &&
          done.evidence.nonce && done.evidence.savedHead && done.evidence.leasesHeld);
    CHECK(done.evidence.completionEntry == (uint64_t(32) << 32 | 0x18) && done.evidence.csbEvents == 2);
    CHECK(eventIndex("rmb") < w->events.size());
    // Consumed CSB entries were overwritten with the all-ones marker.
    CHECK(ld(w->hwsp.cpu + (B::HwspCsbBufferDword + 0) * 4) == ~0u && ld(w->hwsp.cpu + (B::HwspCsbBufferDword + 2) * 4) == ~0u);
    CHECK(e.submitStore(leaseOf(w->probe2), 0xC0FFEE02, 0x22222222) == Status::Busy); // retire first
    CHECK(e.retireCompleted() == Status::Ok);
    CHECK(w->ring.holds == 0 && w->context.holds == 0 && w->probe.holds == 0 && w->rootHolds == 0 && w->hwsp.holds == 1);
    CHECK(e.seqno() == 101 && e.ringHead() == 56 && e.request().phase == B::RequestPhase::None);
    // Second request: lite restore without FORCE_RESTORE, tag reused after retirement.
    CHECK(e.submitStore(leaseOf(w->probe2), 0xC0FFEE02, 0x22222222) == Status::Ok);
    CHECK(Lrc::decodeDescriptor(Lrc::Engine::Bcs0, e.request().descriptor, d) == Lrc::Status::Ok && !d.forceRestore);
    CHECK(e.request().ringOffset == 64 && e.request().tail == 120);
    CHECK(e.poll() == Status::Ok && e.retireCompleted() == Status::Ok && e.seqno() == 102);
}

void testIrqAndMemoryAloneNeverComplete() {
    fresh(Gpu::Deferred);
    B::Engine e; ready(e);
    CHECK(e.submitStore(leaseOf(w->probe), 0xA5A50001, 0x0BADF00D) == Status::Ok);
    e.noteUserInterrupt(); e.noteUserInterrupt();
    CHECK(e.poll() == Status::Pending && e.request().phase == B::RequestPhase::Pending);
    // CPU-written matching values (no CSB) are not evidence.
    st(w->probe.cpu, 0xA5A50001); st(w->hwsp.cpu + B::HwspSeqnoDword * 4, 101);
    CHECK(e.poll() == Status::Pending);
    const uint32_t before = w->delays;
    CHECK(e.drain(5, 10) == Status::TimedOut && w->delays == before + 5);
    CHECK(e.request().phase == B::RequestPhase::Pending && e.interrupts() == 2);
    CHECK(e.abandon() == Status::Ok && e.unknownCount() == 1);
    CHECK(e.unknown(0).phase == B::RequestPhase::Unknown && w->probe.holds == 1 && w->ring.holds == 1);
    CHECK(e.state() == B::EngineState::Faulted && e.seqno() == 101);
    CHECK(e.submitStore(leaseOf(w->probe2), 0xA5A50002, 0) == Status::InvalidState);
    CHECK(e.detach() == Status::Busy);
}

void testEvidenceRules() {
    { // breadcrumb not yet visible: awaits, then completes once the GPU value lands
        fresh(Gpu::StaleBreadcrumb);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::Pending && e.request().evidence.csbCompleted && !e.request().evidence.breadcrumb);
        CHECK(e.poll() == Status::Pending);
        st(w->hwsp.cpu + B::HwspSeqnoDword * 4, e.request().seqno);
        CHECK(e.poll() == Status::Ok && e.request().phase == B::RequestPhase::Completed);
    }
    { // foreign breadcrumb contradicts: Unknown, pinned
        fresh(Gpu::ForeignBreadcrumb);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::EvidenceMismatch && e.unknownCount() == 1 && w->probe.holds == 1);
    }
    { // nonce missing: CSB + breadcrumb + saved head alone do not complete
        fresh(Gpu::NoNonce);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::Pending && e.request().evidence.breadcrumb && !e.request().evidence.nonce);
        CHECK(e.request().evidence.probe == ProbeReading::Unchanged);
        CHECK(e.drain(3, 1) == Status::TimedOut && e.abandon() == Status::Ok && w->probe.holds == 1);
    }
    { // foreign nonce
        fresh(Gpu::ForeignNonce);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::EvidenceMismatch && e.unknownCount() == 1);
    }
    { // no context save of RING_HEAD
        fresh(Gpu::NoSavedHead);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::Pending && !e.request().evidence.savedHead && e.request().evidence.nonce);
    }
    { // completion carrying another context ID
        fresh(Gpu::WrongCcid);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::StaleIdentity && e.unknownCount() == 1 && e.state() == B::EngineState::Faulted);
        CHECK(w->probe.holds == 1 && w->context.holds == 1);
    }
    { // promote identity is not interpreted, as in process_csb()
        fresh(Gpu::PromoteZeroCcid);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::Ok);
    }
    { // ERROR_CSB: a second completion with no active port
        fresh(Gpu::DoubleComplete);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::CsbFault && e.unknownCount() == 1);
    }
    { // completion without a preceding promote
        fresh(Gpu::CompleteWithoutPromote);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        CHECK(e.poll() == Status::CsbFault && e.unknownCount() == 1);
    }
    { // lease revoked while in flight
        fresh(Gpu::Deferred);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x51515151, 0x0) == Status::Ok);
        w->probe.live = false;
        CHECK(e.poll() == Status::Ownership && e.unknownCount() == 1 && w->probe.holds == 1);
    }
}

void testCsbReadPaths() {
    { // HWSP entry still all-ones: wa_csb_read MMIO fallback
        fresh(Gpu::MmioFallback);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x77777777, 0x1) == Status::Ok);
        w->events.clear();
        CHECK(e.poll() == Status::Ok && e.request().phase == B::RequestPhase::Completed);
        CHECK(w->delays >= 20); // two entries, ten re-reads each before MMIO
    }
    { // all-ones from MMIO with a live device: unknown CSB, pinned
        fresh(Gpu::MmioAllOnes);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x77777777, 0x1) == Status::Ok);
        CHECK(e.poll() == Status::CsbFault && e.unknownCount() == 1 && e.state() == B::EngineState::Faulted);
    }
    { // device gone during CSB processing
        fresh(Gpu::MmioFallback);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x77777777, 0x1) == Status::Ok);
        w->deviceGone = true;
        CHECK(e.poll() == Status::DeviceLost && e.state() == B::EngineState::DeviceLost && e.unknownCount() == 1);
        CHECK(e.detach() == Status::Busy && w->probe.holds == 1);
    }
    { // write pointer in the reserved range
        fresh(Gpu::Deferred);
        B::Engine e; ready(e);
        CHECK(e.submitStore(leaseOf(w->probe), 0x77777777, 0x1) == Status::Ok);
        st(w->hwsp.cpu + B::HwspCsbWriteDword * 4, 6);
        CHECK(e.poll() == Status::CsbFault && e.unknownCount() == 1);
    }
}

void testWraps() {
    fresh(Gpu::Normal, 4096);
    B::Engine e; ready(e, 0xFFFFFFFC);
    uint32_t heads = 0, wrapsSeen = 0, previousEmit = 0;
    for (uint32_t i = 0; i < 70; ++i) {
        Buffer &p = (i & 1) ? w->probe2 : w->probe;
        CHECK(e.submitStore(leaseOf(p), 0x9000 + i * 2 + 1, 0x9000 + i * 2) == Status::Ok);
        const uint32_t seqno = e.request().seqno;
        CHECK(seqno != 0 && seqno != ~0u);
        if (e.request().ringOffset < previousEmit) ++wrapsSeen;
        previousEmit = e.request().ringOffset;
        CHECK(e.poll() == Status::Ok);
        CHECK(e.retireCompleted() == Status::Ok);
        heads |= 1u << e.csbHead();
    }
    // Two events per request: the cached head rests on 1, 3, 5 and wraps 5 -> 1,
    // so every one of the six entries is consumed. 70 x 64 bytes wraps the ring once.
    CHECK(heads == 0x2A && wrapsSeen == 1);
    for (int i = 0; i < 12; ++i) CHECK(ld(w->hwsp.cpu + (B::HwspCsbBufferDword + i) * 4) == ~0u);
    CHECK(e.seqno() == 68u); // 0xFFFFFFFD, 0xFFFFFFFE, then 1..68: 0 and ~0 skipped
    CHECK(w->ring.holds == 0 && w->probe.holds == 0 && w->probe2.holds == 0);
}

void testSubmitRefusals() {
    fresh(Gpu::Deferred);
    B::Engine e; ready(e);
    w->writes.clear();
    w->forcewake = 0;
    CHECK(e.submitStore(leaseOf(w->probe), 1, 2) == Status::ForcewakeNotHeld);
    w->forcewake = B::ForcewakeAllGen9;
    w->probe.retainFails = true;
    CHECK(e.submitStore(leaseOf(w->probe), 0x12345678, 2) == Status::Ownership);
    CHECK(w->ring.holds == 0 && w->context.holds == 0 && w->rootHolds == 0 && w->writes.empty());
    w->probe.retainFails = false;
    CHECK(e.submitStore(leaseOf(w->probe), 0x12345678, 0x12345678) == Status::Encoding); // nonce == cpuInitial
    CHECK(e.submitStore(leaseOf(w->probe), 0, 2) == Status::Encoding);
    CHECK(e.submitStore(leaseOf(w->probe), ~0u, 2) == Status::Encoding);
    B::Lease alias = leaseOf(w->probe); alias.address = w->ring.ggtt;
    CHECK(e.submitStore(alias, 0x12345678, 2) == Status::InvalidArgument);
    CHECK(w->writes.empty() && w->ring.holds == 0);
    st(w->hwsp.cpu + B::HwspSeqnoDword * 4, 555); // foreign writer before submit
    CHECK(e.submitStore(leaseOf(w->probe), 0x12345678, 2) == Status::EvidenceMismatch);
    CHECK(e.state() == B::EngineState::Faulted && w->writes.empty());
}

void testStopAndReset() {
    fresh(Gpu::Deferred);
    B::Engine e; ready(e);
    w->writes.clear();
    CHECK(e.stop() == Status::Ok && e.state() == B::EngineState::Stopped);
    CHECK(w->writes.size() == 1 && w->writes[0] == std::make_pair(B::Reg::MiMode, 0x01000100u));
    CHECK(e.cancelStop() == Status::Ok && w->writes.back() == std::make_pair(B::Reg::MiMode, 0x01000000u));
    w->idle = false; w->ringHeadReg = 0x40; w->ringTailReg = 0x40;
    CHECK(e.stop() == Status::Ok); // idle flag missing but ring empty
    CHECK(e.cancelStop() == Status::Ok);
    w->ringTailReg = 0x80;
    const uint32_t delays = w->delays;
    CHECK(e.stop() == Status::TimedOut && e.state() == B::EngineState::Stopped && e.lastStopTail() == 0x80);
    CHECK(w->delays - delays == 1000 + 100);
    w->idle = true;
    CHECK(e.cancelStop() == Status::Ok);

    // Reset with a request in flight that never produced evidence.
    CHECK(e.submitStore(leaseOf(w->probe), 0x31313131, 0x0) == Status::Ok);
    const uint32_t oldCcid = e.request().contextId;
    w->readyAfter = 3; w->writes.clear();
    CHECK(e.prepareReset() == Status::Ok && e.state() == B::EngineState::ResetPrepared);
    CHECK(ld(w->hwsp.cpu + B::HwspPreemptDword * 4) == 1);
    CHECK(w->writes[0] == std::make_pair(B::Reg::MiMode, 0x01000100u));
    CHECK(w->writes.back() == std::make_pair(B::Reg::ResetCtl, 0x00010001u));
    B::Platform noReset = platform(); (void)noReset;
    CHECK(e.resetEngine() == Status::Ok && w->resets == 1);
    CHECK(w->writes.back() == std::make_pair(B::Reg::ResetCtl, 0x00010000u));
    CHECK(e.state() == B::EngineState::Attached && e.engineEpoch() == 2 && e.unknownCount() == 1);
    CHECK(w->probe.holds == 1 && e.unknown(0).contextId == oldCcid);
    CHECK(e.initialize() == Status::Ok);
    CHECK(ld(w->hwsp.cpu + B::HwspSeqnoDword * 4) == 101 && ld(w->hwsp.cpu + B::HwspPreemptDword * 4) == 0);
    Lrc::RegisterState regs;
    CHECK(Lrc::decodeRegisterState(Lrc::Engine::Bcs0, w->context.cpu, 8192, regs) == Lrc::Status::Ok);
    CHECK(regs.ringHead == 64 && regs.ringTail == 64); // no replay of unproven commands
    // New work gets a fresh identity; the stale identity is recognized.
    w->mode = Gpu::Deferred;
    CHECK(e.submitStore(leaseOf(w->probe2), 0x32323232, 0x0) == Status::Ok);
    CHECK(e.request().contextId != oldCcid && e.request().seqno == 102);
    csbPush(0x18, oldCcid);
    CHECK(e.poll() == Status::StaleIdentity && e.unknownCount() == 2);
    CHECK(e.detach() == Status::Busy);

    // CAT_ERROR handshake and a not-ready engine.
    fresh();
    B::Engine c; ready(c);
    w->catError = true; w->writes.clear();
    CHECK(c.prepareReset() == Status::Ok);
    CHECK(w->writes.back() == std::make_pair(B::Reg::ResetCtl, 0x00040004u));
    CHECK(c.cancelReset() == Status::Ok && c.state() == B::EngineState::Stopped);
    fresh();
    B::Engine n; ready(n);
    w->readyAfter = 100000; w->writes.clear();
    CHECK(n.prepareReset() == Status::ResetNotReady && n.state() == B::EngineState::Stopped);
    CHECK(w->writes.back() == std::make_pair(B::Reg::ResetCtl, 0x00010000u));
    // Missing reset authority is reported, never assumed.
    fresh();
    B::Engine m;
    B::Platform p = platform(); p.reset = {};
    CHECK(m.attach(p, bindings(), 5) == Status::Ok && m.initialize() == Status::Ok);
    CHECK(m.prepareReset() == Status::Ok && m.resetEngine() == Status::Unavailable);
    CHECK(m.state() == B::EngineState::ResetPrepared);
}

void testCompletedBeforeResetIsKept() {
    fresh(Gpu::Normal);
    B::Engine e; ready(e);
    CHECK(e.submitStore(leaseOf(w->probe), 0x41414141, 0x0) == Status::Ok);
    // reset_csb(): events written before the reset are processed and proven.
    CHECK(e.prepareReset() == Status::Ok);
    CHECK(e.request().phase == B::RequestPhase::Completed);
    CHECK(e.resetEngine() == Status::Ok && e.unknownCount() == 0);
    CHECK(e.retireCompleted() == Status::Ok && w->probe.holds == 0);
}

void testUnprovenStopCannotDetach() {
    fresh(Gpu::Deferred);
    B::Engine e; ready(e);
    w->idle = false; w->ringHeadReg = 0x40; w->ringTailReg = 0x80;
    CHECK(e.stop() == Status::TimedOut);
    CHECK(e.stop() == Status::TimedOut); // Stopped is requested, not proven.
    const int retained = w->hwsp.holds;
    CHECK(e.detach() == Status::Busy && w->hwsp.holds == retained);
    w->idle = true;
    CHECK(e.stop() == Status::Ok && e.detach() == Status::Ok && w->hwsp.holds == 0);
}

void testDetach() {
    fresh();
    B::Engine e; ready(e);
    CHECK(e.detach() == Status::Busy); // must stop first
    CHECK(e.stop() == Status::Ok);
    w->writes.clear();
    CHECK(e.detach() == Status::Ok && e.state() == B::EngineState::Detached);
    CHECK(w->writes.size() == 1 && w->writes[0] == std::make_pair(B::Reg::Hwstam, ~0u));
    CHECK(w->hwsp.holds == 0);
    CHECK(e.attach(platform(), bindings(), 9) == Status::Ok); // reusable after detach
}
} // namespace

int main() {
    static_assert(B::Reg::Elsp == 0x22230 && B::Reg::ContextStatusPtr == 0x223a0 &&
                  B::Reg::HwsPga == 0x22080 && B::Reg::ModeGen7 == 0x2229c && B::Reg::MiMode == 0x2209c,
                  "BCS0 register offsets");
    static_assert(B::maskedEnable(B::Bit::GfxRunListEnable) == 0x80008000u &&
                  B::maskedDisable(B::Bit::StopRing) == 0x01000000u, "masked writes");
    CHECK(!B::inWindow(0x21ffc) && !B::inWindow(0x23000) && !B::inWindow(0x22002) && B::inWindow(0x22ffc));
    CHECK(!B::inWindow(0x70180) && !B::inWindow(0x44300) && !B::inWindow(0xa188) && !B::inWindow(0x941c));
    testAttach();
    testInitialize();
    testFirstStore();
    testIrqAndMemoryAloneNeverComplete();
    testEvidenceRules();
    testCsbReadPaths();
    testWraps();
    testSubmitRefusals();
    testStopAndReset();
    testCompletedBeforeResetIsKept();
    testUnprovenStopCannotDetach();
    testDetach();
    std::printf("native_glk_bcs_tests: %u checks passed\n", checks);
    return 0;
}

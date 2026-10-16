// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// HOSTED INJECTED TRANSPORT for the unchanged GlkBcsIOKit adapter: shim
// IOKit headers, a host-memory "BAR", the real MemoryOwner with a host
// backend, and fixture owner callbacks. No PCI, MMIO, DMA, barrier or GPU
// operation is performed; nothing here is evidence of hardware execution.
#include "Mellow/GlkBcsIOKit.hpp"
#include <cstdio>
#include <vector>

namespace B = MellowNativeGlk::Bcs;
using MellowNative::GlkBcsIOKit;
using Result = GlkBcsIOKit::Result;
using namespace MellowNative;

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

static std::vector<B::Fence> fences;
namespace MellowNative { void glkBcsCpuFence(B::Fence kind) { fences.push_back(kind); } }

namespace {
constexpr uint64_t Epoch = 21, Owner = 0x5151;
struct HostBackend {
    uint64_t nextGgtt {0x100000};
    unsigned syncs {}, maps {};
    std::vector<uint64_t> ggtt; // the fixture GGTT owner's published offsets
    std::vector<void *> live;
};
HostBackend host;
bool admitted(void *, const DeviceIdentity &) { return true; }
MemoryStatus pin(void *, const DeviceIdentity &d, uint64_t, const MemoryHandle &, uint64_t bytes, DmaPin &out) {
    auto *cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, bytes));
    auto *pages = static_cast<uint64_t *>(std::malloc(sizeof(uint64_t) * (bytes / 4096)));
    if (!cpu || !pages) std::abort();
    std::memset(cpu, 0, bytes);
    static uint64_t nextPage = 0x200000;
    for (uint64_t i = 0; i < bytes / 4096; ++i) { pages[i] = nextPage; nextPage += 4096; }
    out.cookie = cpu; out.cpu = cpu; out.pages = pages; out.pageCount = bytes / 4096;
    out.bytes = bytes; out.deviceRegistryId = d.registryId;
    return MemoryStatus::Ok;
}
MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, DmaPin &p) {
    std::free(const_cast<uint64_t *>(p.pages)); std::free(p.cpu); p = {}; return MemoryStatus::Ok;
}
MemoryStatus synchronize(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, CacheDirection) {
    ++host.syncs; return MemoryStatus::Ok;
}
MemoryStatus mapGpu(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &p, bool writable, GpuMapping &m) {
    m.cookie = reinterpret_cast<void *>(uintptr_t(host.nextGgtt)); m.address = host.nextGgtt;
    m.bytes = p.bytes; m.writable = writable; host.ggtt.push_back(host.nextGgtt);
    host.nextGgtt += p.bytes + 4096; ++host.maps; return MemoryStatus::Ok;
}
MemoryStatus unmapGpu(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, GpuMapping &m) {
    m = {}; return MemoryStatus::Ok;
}
bool retired(void *, const MemoryView &) { return true; }
bool quiesce(void *, const DeviceIdentity &) { return true; }

struct Owners {
    uint32_t forcewake {B::ForcewakeAllGen9};
    bool ggttHonest {true}, rootLive {true};
    int rootHolds {}, resets {};
} owners;
bool fwHeld(void *, uint64_t epoch, uint32_t domains) { return epoch == Epoch && (domains & ~owners.forcewake) == 0; }
bool ggttBound(void *, const MemoryView &view, uint64_t ggtt) {
    if (!owners.ggttHonest) return false;
    for (uint64_t a : host.ggtt) if (a == ggtt && view.mapping.address == ggtt) return true;
    return false;
}
bool rootLive(void *, uint64_t epoch, uint64_t generation, uint64_t dma) {
    return owners.rootLive && epoch == Epoch && generation == 9 && dma == 0x7700000;
}
bool rootHold(void *, uint64_t, uint64_t) { ++owners.rootHolds; return true; }
bool rootDrop(void *, uint64_t, uint64_t) { if (!owners.rootHolds) return false; --owners.rootHolds; return true; }
bool resetBlitter(void *, uint64_t epoch) { ++owners.resets; return epoch == Epoch; }

GlkBcsAuthorities authorities() {
    GlkBcsAuthorities a;
    a.forcewake = {nullptr, fwHeld};
    a.reset = {nullptr, resetBlitter};
    a.ggtt = {nullptr, ggttBound};
    a.ppgtt = {nullptr, rootLive, rootHold, rootDrop};
    return a;
}
GlkBcsIdentity identity() {
    GlkBcsIdentity id;
    id.vendor = 0x8086; id.device = 0x3185; id.revision = 0x06;
    id.subsystemVendor = 0x17aa; id.subsystem = 0x3e77; id.registryId = 4294967813ULL; id.epoch = Epoch;
    return id;
}
struct Fixture {
    IOPCIDevice device;
    IOService owner;
    MemoryOwner memory;
    MemoryHandle hwsp, context, ring, probe;
    Fixture() {
        owner.provider = &device; device.openOwner = &owner;
        MemoryLimits limits; limits.dmaAddressBits = 39;
        MemoryBackend backend {nullptr, admitted, pin, unpin, synchronize, mapGpu, unmapGpu, retired, quiesce};
        CHECK(memory.initialize({0x8086, 0x3185, 4294967813ULL, Epoch}, limits, backend) == MemoryStatus::Ok);
        for (auto [h, bytes] : {std::pair<MemoryHandle *, uint64_t>{&hwsp, 4096}, {&context, 8192}, {&ring, 4096}, {&probe, 4096}}) {
            CHECK(memory.allocate(Owner, bytes, *h) == MemoryStatus::Ok);
            CHECK(memory.map(Owner, *h, true) == MemoryStatus::Ok);
        }
    }
    GlkBcsMemory bindings() const { return {hwsp, context, ring, 0x7700000, 9}; }
    uint32_t holds(MemoryHandle h) const { MemoryView v; CHECK(memory.inspect(Owner, h, v) == MemoryStatus::Ok); return v.jobHolds; }
};
// The adapter's own mapping (the latest one created), read directly.
uint32_t bar(GlkBcsIOKit &, uint32_t offset) {
    if (!GlkBcsShim::liveMaps.count(GlkBcsShim::lastMap)) std::abort();
    return *reinterpret_cast<uint32_t *>(static_cast<IOMemoryMap *>(GlkBcsShim::lastMap)->bytes + offset);
}
bool barClean(uint32_t from, uint32_t to) {
    if (!GlkBcsShim::liveMaps.count(GlkBcsShim::lastMap)) std::abort();
    const auto *bytes = static_cast<IOMemoryMap *>(GlkBcsShim::lastMap)->bytes;
    for (uint32_t i = from; i < to; ++i) if (bytes[i]) return false;
    return true;
}

void testAttach() {
    Fixture f;
    GlkBcsIOKit a;
    CHECK(a.attach(nullptr, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Invalid);
    GlkBcsIdentity id = identity(); id.device = 0x3184;
    CHECK(a.attach(&f.device, &f.owner, id, &f.memory, Owner, authorities()) == Result::Invalid);
    id = identity(); id.epoch = Epoch + 1;
    CHECK(a.attach(&f.device, &f.owner, id, &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.openOwner = nullptr;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.openOwner = &f.owner; f.owner.provider = nullptr;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.owner.provider = &f.device; f.device.inactive = true;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.inactive = false; f.device.revision = 0x03;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.revision = 0x06; f.device.subsystemId = 0x12341234;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.subsystemId = 0x3e7717aa; f.device.command = 0x0002; // bus mastering off: never enabled here
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.command = 0x0004; // memory space off
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ownership);
    f.device.command = 0x0006;
    CHECK(GlkBcsShim::maps == 0 && f.device.command == 0x0006);
    GlkBcsShim::mapFails = true;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Unavailable);
    GlkBcsShim::mapFails = false; GlkBcsShim::mapLength = 1024 * 1024;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Unavailable);
    CHECK(GlkBcsShim::liveMaps.empty()); // short map released
    GlkBcsShim::mapLength = 2 * 1024 * 1024;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ok);
    CHECK(GlkBcsShim::lastMapRegister == kIOPCIConfigBaseAddress0 && GlkBcsShim::lastMapOptions == kIOMapInhibitCache);
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Busy);
    CHECK(a.detach() == Result::Ok && GlkBcsShim::liveMaps.empty());
}

void testPendingWiringAndWindow() {
    Fixture f;
    for (int missing = 0; missing < 3; ++missing) {
        GlkBcsAuthorities ops = authorities();
        if (missing == 0) ops.forcewake = {};
        if (missing == 1) ops.ggtt = {};
        if (missing == 2) ops.ppgtt.hold = nullptr;
        GlkBcsIOKit a;
        CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, ops) == Result::Ok);
        CHECK(a.bind(f.bindings(), 1) == Result::Unavailable);
        CHECK(a.engine().state() == B::EngineState::Detached && a.detach() == Result::Ok);
    }
    GlkBcsIOKit a;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ok);
    owners.ggttHonest = false;
    CHECK(a.bind(f.bindings(), 1) == Result::Ownership);
    owners.ggttHonest = true;
    MemoryHandle unmapped;
    CHECK(f.memory.allocate(Owner, 4096, unmapped) == MemoryStatus::Ok);
    GlkBcsMemory m = f.bindings(); m.ring = unmapped;
    CHECK(a.bind(m, 1) == Result::Ownership);
    m = f.bindings(); m.pml4Generation = 10;
    CHECK(a.bind(m, 1) == Result::Engine && a.lastEngineStatus() == B::Status::Ownership);
    // The MMIO port never reaches display, interrupt, forcewake or GDRST registers.
    B::MmioPort port = a.mmioPort();
    for (uint32_t offset : {0x70180u, 0x44300u, 0xa188u, 0x941cu, 0x21ffcu, 0x23000u, 0x22002u}) {
        port.write32(port.context, offset, 0xdeadbeef);
        CHECK(port.read32(port.context, offset) == 0xFFFFFFFFu);
    }
    CHECK(a.windowViolations() == 14 && barClean(0, 2 * 1024 * 1024));
    port.write32(port.context, B::Reg::Hwstam, 0x12345678);
    CHECK(bar(a, B::Reg::Hwstam) == 0x12345678 && port.read32(port.context, B::Reg::Hwstam) == 0x12345678);
    CHECK(a.detach() == Result::Ok);
}

void testEngineThroughAdapter() {
    Fixture f;
    GlkBcsIOKit a;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ok);
    CHECK(a.bind(f.bindings(), 40) == Result::Ok);
    B::Engine &e = a.engine();
    owners.forcewake = B::ForcewakeGt;
    CHECK(e.initialize() == B::Status::ForcewakeNotHeld && barClean(0, 2 * 1024 * 1024));
    owners.forcewake = B::ForcewakeAllGen9;
    CHECK(e.initialize() == B::Status::Ok && e.state() == B::EngineState::Ready);
    MemoryView hv; CHECK(f.memory.inspect(Owner, f.hwsp, hv) == MemoryStatus::Ok);
    CHECK(bar(a, B::Reg::HwsPga) == hv.mapping.address && bar(a, B::Reg::ModeGen7) == 0x80008000u);
    CHECK(bar(a, B::Reg::MiMode) == 0x01000000u && bar(a, B::Reg::Emr) == 0xFFFFFFFEu);
    CHECK(bar(a, B::Reg::ContextStatusPtr) == 0xffff0505u && f.holds(f.hwsp) == 1);
    CHECK(barClean(0, B::MmioBase) && barClean(B::MmioBase + B::WindowBytes, 2 * 1024 * 1024));
    B::Lease probe;
    CHECK(a.leaseProbe(f.probe, probe) == Result::Ok && probe.kind == B::LeaseKind::ProbeTarget);
    fences.clear();
    CHECK(e.submitStore(probe, 0x600DF00D, 0x0) == B::Status::Ok);
    CHECK(!fences.empty() && fences.back() == B::Fence::Store);
    CHECK(bar(a, B::Reg::Elsp) == uint32_t(e.request().descriptor)); // last write: port 0 lower
    CHECK(f.holds(f.ring) == 1 && f.holds(f.context) == 1 && f.holds(f.probe) == 1 && owners.rootHolds == 1);
    // Plain BAR memory never produces CSB events: no completion can appear.
    e.noteUserInterrupt();
    CHECK(e.poll() == B::Status::Pending && e.drain(3, 5) == B::Status::TimedOut);
    CHECK(e.abandon() == B::Status::Ok && e.unknownCount() == 1);
    CHECK(e.detach() == B::Status::Busy && a.detach() == Result::Busy);
    CHECK(f.holds(f.probe) == 1 && f.memory.retire(Owner, f.probe) != MemoryStatus::Ok); // stays pinned
}

void testCleanLifecycle() {
    Fixture f;
    GlkBcsIOKit a;
    CHECK(a.attach(&f.device, &f.owner, identity(), &f.memory, Owner, authorities()) == Result::Ok);
    CHECK(a.bind(f.bindings(), 1) == Result::Ok);
    B::Engine &e = a.engine();
    CHECK(e.initialize() == B::Status::Ok);
    const uint64_t delays = GlkBcsShim::delayCalls;
    CHECK(e.stop() == B::Status::Ok); // ring empty although MODE_IDLE never reads set
    CHECK(GlkBcsShim::delayCalls - delays == 1100);
    CHECK(e.prepareReset() == B::Status::ResetNotReady); // plain memory never acks RESET_CTL
    CHECK(e.detach() == B::Status::Ok && f.holds(f.hwsp) == 0 && bar(a, B::Reg::Hwstam) == ~0u);
    void *own = GlkBcsShim::lastMap;
    CHECK(a.detach() == Result::Ok && !GlkBcsShim::liveMaps.count(own));
    // The previous test's adapter still pins its map: an Unknown request never unmaps.
    CHECK(GlkBcsShim::liveMaps.size() == 1);
    CHECK(GlkBcsShim::unmaps >= 1 && owners.resets == 0);
}
} // namespace

int main() {
    testAttach();
    testPendingWiringAndWindow();
    testEngineThroughAdapter();
    testCleanLifecycle();
    std::printf("native_glk_bcs_iokit_tests: %u checks passed\n", checks);
    return 0;
}

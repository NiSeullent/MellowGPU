// HOSTED INJECTED TRANSPORT: unchanged GlkDmaMemoryIOKit.cpp + MemoryOwner.cpp
// against the explicit fake OS boundary in native_glk_dma_iokit_shim.hpp.
// No hardware, DMA, IOMMU, cache line, barrier or GPU operation happens here;
// passing proves adapter logic only. Host cleanup at the end is not recovery.
#include "native_glk_dma_iokit_shim.hpp"
#include "../Mellow/GlkDmaMemoryIOKit.hpp"
#include <cstdio>
#include <cstdlib>
using namespace MellowNative;
namespace Mock = GlkDmaShim;
static unsigned checks;
static IOMapper *transitionMapper;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)

namespace MellowNative {
// Hosted fence boundary: records the requested x86 ordering, executes nothing.
void glkCpuFence(GlkCpuFence kind) {
    Mock::event(kind == GlkCpuFence::Full ? "fence.full" :
        kind == GlkCpuFence::Store ? "fence.store" : "fence.load");
}
}

static bool sameId(const DeviceIdentity &a, const DeviceIdentity &b) {
    return a.vendor == b.vendor && a.device == b.device && a.registryId == b.registryId && a.epoch == b.epoch;
}
struct Vm {
    IOPCIDevice *device {};
    IOService *owner {};
    DeviceIdentity identity {};
    bool admitted {true}, retired {true}, quiesced {true};
    MemoryStatus mapStatus {MemoryStatus::Ok}, unmapStatus {MemoryStatus::Ok};
    bool partialMap {};
    unsigned maps {}, unmaps {}, stops {};
    static bool admission(void *p, IOPCIDevice *device, IOService *owner, const DeviceIdentity &id) {
        auto &v = *static_cast<Vm *>(p);
        return v.admitted && device == v.device && owner == v.owner && sameId(id, v.identity);
    }
    static MemoryStatus map(void *p, const DeviceIdentity &, uint64_t owner,
            const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &out) {
        auto &v = *static_cast<Vm *>(p); ++v.maps;
        CHECK(owner == 42 && pin.bytes != 0);
        if (v.mapStatus == MemoryStatus::Ok || v.partialMap)
            out = {reinterpret_cast<void *>(uintptr_t(handle.slot + 1)),
                0x1000000 + uint64_t(handle.slot) * 0x10000, pin.bytes, writable};
        return v.mapStatus;
    }
    static MemoryStatus unmap(void *p, const DeviceIdentity &, uint64_t owner,
            const MemoryHandle &, const DmaPin &, GpuMapping &) {
        auto &v = *static_cast<Vm *>(p); ++v.unmaps; CHECK(owner == 42); return v.unmapStatus;
    }
    static bool retirement(void *p, const MemoryView &) { return static_cast<Vm *>(p)->retired; }
    static bool stop(void *p, const DeviceIdentity &) { auto &v = *static_cast<Vm *>(p); ++v.stops; return v.quiesced; }
    GlkPhysicalVmOps ops(bool = false) {
        return {this, admission, map, unmap, retirement, stop};
    }
};
struct Fixture {
    IOPCIDevice *device {new IOPCIDevice};
    IOService *owner {new IOService};
    IOMapper *mapper {new IOMapper};
    GlkDmaIdentity identity {{0x8086, 0x3185, 4294967813ULL, 7}, 0x06, 0x17aa, 0x3e77};
    MemoryLimits limits {39, 48, 8192, 16384};
    Vm vm;
    GlkDmaMemoryIOKit adapter;
    MemoryBackend backend {};
    bool iommu, quarantine {};
    explicit Fixture(bool withIommu = true) : iommu(withIommu) {
        Mock::reset(); IOMapper::gSystem = nullptr;
        owner->provider = device; device->openOwner = owner;
        if (iommu) device->fixtureProperty("iommu-parent", mapper);
        vm.device = device; vm.owner = owner; vm.identity = identity.device;
    }
    ~Fixture() {
        const auto status = adapter.detach();
        CHECK(status == (quarantine ? MemoryStatus::Busy : MemoryStatus::Ok));
        CHECK(Mock::unsafeClearCalls == 0 && Mock::forbiddenMapperLookups == 0 &&
            Mock::systemMapperFallbacks == 0 && Mock::prematureDestroy == 0);
        IOMapper::gSystem = nullptr;
        device->release(); owner->release(); mapper->release();
    }
    MemoryStatus tryAttach() { return adapter.attach(device, owner, identity, limits, vm.ops(!iommu)); }
    void attach() {
        CHECK(tryAttach() == MemoryStatus::Ok);
        CHECK(adapter.route() == (iommu ? GlkDmaRoute::IommuMapped : GlkDmaRoute::DirectPhysical));
        CHECK(device->refs == 2 && owner->refs == 2 && mapper->refs == (iommu ? 3u : 1u));
        backend = adapter.backend();
    }
    // Nothing retained, observed or allocated: a clean pre-acquisition refusal.
    void untouched(unsigned configReads = 0) const {
        CHECK(!adapter.attached() && adapter.route() == GlkDmaRoute::Unavailable);
        CHECK(device->refs == 1 && owner->refs == 1 && mapper->refs == (iommu ? 2u : 1u));
        CHECK(device->configReads == configReads && Mock::bufferFactories == 0 &&
            Mock::specCalls == 0 && Mock::faults.mallocCalls == 0);
    }
    MemoryHandle handle(uint32_t slot = 0) const { return {slot, 77 + slot, 4294967813ULL, 7}; }
    MemoryStatus pin(DmaPin &out, uint32_t slot = 0, uint64_t bytes = 8192) {
        return backend.pin(backend.context, identity.device, 42, handle(slot), bytes, out);
    }
    MemoryStatus unpin(DmaPin &pin, uint32_t slot = 0) {
        return backend.unpin(backend.context, identity.device, 42, handle(slot), pin);
    }
    MemoryStatus sync(const DmaPin &pin, CacheDirection direction, uint32_t slot = 0) {
        return backend.synchronize(backend.context, identity.device, 42, handle(slot), pin, direction);
    }
};
static void cleanPinFailure(Fixture &f) {
    f.attach(); DmaPin pin {};
    CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie != nullptr);
    CHECK(f.adapter.pinnedBytes() == 8192 && f.adapter.detach() == MemoryStatus::Busy);
    CHECK(f.unpin(pin) == MemoryStatus::Ok && pin.cookie == nullptr && f.adapter.pinnedBytes() == 0);
    CHECK(f.device->refs == 2 && f.owner->refs == 2 && f.mapper->refs == (f.iommu ? 3u : 1u));
}
static void positivePinLifetime(Fixture &f) {
    f.attach(); DmaPin pin {};
    CHECK(f.pin(pin) == MemoryStatus::Ok && pin.pageCount == 2 && pin.bytes == 8192);
    CHECK(pin.deviceRegistryId == 4294967813ULL && pin.cpu == Mock::lastBuffer->cpu);
    for (size_t i = 0; i < pin.bytes; ++i) CHECK(pin.cpu[i] == 0);
    CHECK(Mock::lastMask == GlkDmaPhysicalMask && GlkDmaPhysicalMask == 0x7ffffff000ULL);
    CHECK(Mock::lastCommand && Mock::lastCommand->bits == 39 && Mock::lastCommand->capacity == 8192);
    if (f.iommu) {
        CHECK(Mock::lastMappingOption == IODMACommand::kMapped && Mock::lastSpecMapper == f.mapper);
        CHECK(pin.pages[0] == 0x800000 && pin.pages[1] == 0x801000 && Mock::ordered("dma.segment", "mapper.translate"));
    } else {
        CHECK(Mock::lastMappingOption == IODMACommand::kBypassed && Mock::lastSpecMapper == nullptr);
        CHECK(pin.pages[0] == Mock::physical(0) && pin.pages[1] == Mock::physical(1));
        CHECK(Mock::first("mapper.translate") == Mock::events.size());
    }
    // Zeroes are cleaned out of CPU caches after both prepares and before Ok.
    CHECK(Mock::stores == 1 && Mock::flushes == 0 && Mock::ordered("dma.prepare", "md.store"));
    CHECK(Mock::sequence({"md.store", "fence.full"}) && Mock::events.back() == "fence.full");
    CHECK(f.adapter.detach() == MemoryStatus::Busy);
    size_t mark = Mock::events.size();
    CHECK(f.sync(pin, CacheDirection::ForDevice) == MemoryStatus::Ok);
    CHECK(Mock::sequence({"dma.sync.out", "md.store", "fence.full"}, mark) && Mock::events.size() == mark + 3);
    mark = Mock::events.size();
    CHECK(f.sync(pin, CacheDirection::ForCpu) == MemoryStatus::Ok);
    CHECK(Mock::sequence({"fence.full", "dma.sync.in", "md.flush", "fence.full"}, mark) && Mock::events.size() == mark + 4);
    CHECK(Mock::stores == 2 && Mock::flushes == 1);
    CHECK(f.unpin(pin) == MemoryStatus::Ok && f.adapter.pinnedBytes() == 0);
    CHECK(Mock::ordered("dma.complete", "dma.clear") && Mock::ordered("dma.clear", "dma.destroy") &&
        Mock::ordered("dma.destroy", "memory.complete") && Mock::ordered("memory.complete", "memory.destroy"));
    CHECK(f.device->refs == 2 && f.owner->refs == 2 && f.mapper->refs == (f.iommu ? 3u : 1u));
}
static void retainUnknown(Fixture &f, DmaPin &pin, bool descriptorFailure = false) {
    const auto cookie = pin.cookie; uint8_t *const cpu = pin.cpu;
    CHECK(f.unpin(pin) == MemoryStatus::IoFailure);
    CHECK(pin.cookie == cookie && f.adapter.pinnedBytes() == 8192);
    const auto commandCalls = Mock::commandCompleteCalls, descriptorCalls = Mock::descriptorCompleteCalls;
    Mock::faults.commandComplete = false; Mock::faults.descriptorComplete = false;
    CHECK(f.unpin(pin) == MemoryStatus::Quarantined);
    CHECK(pin.cookie == cookie && f.adapter.pinnedBytes() == 8192);
    CHECK(Mock::commandCompleteCalls == commandCalls && Mock::descriptorCompleteCalls == descriptorCalls);
    const auto syncCalls = Mock::syncCalls, stores = Mock::stores, flushes = Mock::flushes;
    CHECK(f.sync(pin, CacheDirection::ForCpu) == MemoryStatus::Ownership);
    GpuMapping mapping {};
    CHECK(f.backend.map(f.backend.context, f.identity.device, 42, f.handle(), pin, true, mapping) == MemoryStatus::Ownership);
    CHECK(Mock::syncCalls == syncCalls && Mock::stores == stores && Mock::flushes == flushes);
    CHECK(f.vm.maps == 0 && mapping.cookie == nullptr && f.adapter.detach() == MemoryStatus::Busy);
    // Native backing outlives the unknown completion: buffer and CPU view stay.
    CHECK(Mock::lastBuffer && (!cpu || Mock::lastBuffer->cpu == cpu));
    if (!descriptorFailure) CHECK(Mock::lastCommand);
    else CHECK(!Mock::lastCommand);
    f.quarantine = true;
}
int main(int argc, char **argv) {
    // Codex regression: actual native API mode, no independent DMAR callback.
    // These two focused paths first failed against the preserved Claude body.
    if (argc == 2 && std::strcmp(argv[1], "--native-direct-no-dmar") == 0) {
        Fixture f(false);
        CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops(false)) == MemoryStatus::Ok);
        CHECK(f.adapter.route() == GlkDmaRoute::DirectPhysical);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--native-bypassed") == 0) {
        Fixture f(false); f.attach(); DmaPin pin {};
        CHECK(f.pin(pin) == MemoryStatus::Ok);
        CHECK(Mock::lastMappingOption == IODMACommand::kBypassed);
        CHECK(f.unpin(pin) == MemoryStatus::Ok);
        return 0;
    }
    // Both legitimate routes, full lifetime and cache/order sequencing.
    { Fixture f; positivePinLifetime(f); }
    { Fixture f(false); positivePinLifetime(f); CHECK(Mock::systemMapperChecks == 1); }

    // Native kNoMapper is a positive platform state, not the observed empty
    // ioreg snapshot. No independent GPU-owned DMAR/PMR proof is required.
    { Fixture f(false); CHECK(f.tryAttach() == MemoryStatus::Ok); }
    // Pending and live native system state are rejected before allocation.
    for (uintptr_t state : {uintptr_t(1), uintptr_t(2), uintptr_t(0)}) {
        Fixture f(false); auto *live = new IOMapper;
        IOMapper::gSystem = state ? reinterpret_cast<IOMapper *>(state) : live;
        CHECK(f.tryAttach() == MemoryStatus::Unavailable); f.untouched();
        IOMapper::gSystem = nullptr; live->release();
    }
    // A numeric/foreign or inactive mapper is never looked up, waited for or bypassed.
    { Fixture f(false); auto *numeric = new OSObject; f.device->fixtureProperty("iommu-parent", numeric); numeric->release();
      CHECK(f.tryAttach() == MemoryStatus::Unavailable); f.untouched(); CHECK(f.device->refs == 1); }
    { Fixture f; f.mapper->inactive = true; CHECK(f.tryAttach() == MemoryStatus::Unavailable); f.untouched(); }
    { Fixture f; CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, f.vm.ops(true)) == MemoryStatus::Ok);
      CHECK(f.adapter.route() == GlkDmaRoute::IommuMapped); }

    // Argument, callback and ownership validation.
    for (unsigned bad = 0; bad < 9; ++bad) {
        Fixture f;
        switch (bad) {
            case 0: f.identity.device.device = 0x3184; break; case 1: f.identity.device.vendor = 0x8087; break;
            case 2: f.identity.device.registryId = 0; break; case 3: f.identity.device.epoch = 0; break;
            case 4: f.limits.dmaAddressBits = 46; break; case 5: f.limits.dmaAddressBits = 38; break;
            case 6: f.limits.maxAllocationBytes = 6000; break; case 7: f.identity.subsystemVendor = 0xffff; break;
            case 8: f.limits.maxTotalBytes = 4096; break;
        }
        CHECK(f.tryAttach() == MemoryStatus::Invalid); f.untouched();
    }
    for (unsigned missing = 0; missing < 6; ++missing) {
        Fixture f; auto ops = f.vm.ops(false);
        switch (missing) {
            case 0: ops.context = nullptr; break; case 1: ops.admitted = nullptr; break;
            case 2: ops.map = nullptr; break; case 3: ops.unmap = nullptr; break;
            case 4: ops.retired = nullptr; break; case 5: ops.quiesce = nullptr; break;
        }
        CHECK(f.adapter.attach(f.device, f.owner, f.identity, f.limits, ops) == MemoryStatus::Unavailable); f.untouched();
    }
    { Fixture f; f.vm.admitted = false; CHECK(f.tryAttach() == MemoryStatus::Ownership); f.untouched(); }
    { Fixture f; f.owner->provider = nullptr; CHECK(f.tryAttach() == MemoryStatus::Ownership); f.untouched(); }
    { Fixture f; f.device->openOwner = nullptr; CHECK(f.tryAttach() == MemoryStatus::Ownership); f.untouched(); }
    { Fixture f; f.device->inactive = true; CHECK(f.tryAttach() == MemoryStatus::Ownership); f.untouched(); }
    for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
        Fixture f(mismatch & 1);
        switch (mismatch) {
            case 0: f.device->physicalId = 0x31848086; break; case 1: f.device->revision = 0x03; break;
            case 2: f.device->subsystemId = 0x3e7817aa; break; case 3: ++f.device->registryId; break;
        }
        CHECK(f.tryAttach() == MemoryStatus::Ownership);
        CHECK(!f.adapter.attached() && f.device->refs == 1 && f.owner->refs == 1 && f.device->configReads > 0);
        CHECK(Mock::bufferFactories == 0 && Mock::specCalls == 0);
    }
    { Fixture f; f.attach(); CHECK(f.tryAttach() == MemoryStatus::Busy); }

    // Authority must still hold at every pin; refusal precedes any allocation.
    for (unsigned change = 0; change < 7; ++change) {
        Fixture f(change >= 3); f.attach(); IOMapper *other = nullptr;
        switch (change) {
            case 0: other = new IOMapper; IOMapper::gSystem = other; break;
            case 1: IOMapper::gSystem = reinterpret_cast<IOMapper *>(uintptr_t(2)); break;
            case 2: other = new IOMapper; f.device->fixtureProperty("iommu-parent", other); break;
            case 3: other = new IOMapper; f.device->fixtureProperty("iommu-parent", other); break;
            case 4: f.device->fixtureProperty("iommu-parent", nullptr); break;
            case 5: f.mapper->inactive = true; break;
            case 6: f.vm.admitted = false; break;
        }
        const auto reads = f.device->configReads; DmaPin pin {};
        CHECK(f.pin(pin) == MemoryStatus::Ownership && !pin.cookie && f.adapter.pinnedBytes() == 0);
        CHECK(Mock::bufferFactories == 0 && Mock::specCalls == 0 && Mock::faults.mallocCalls == 0);
        if (change == 6) CHECK(f.device->configReads == reads);
        IOMapper::gSystem = nullptr; if (other) other->release();
        f.device->fixtureProperty("iommu-parent", f.iommu ? f.mapper : nullptr); IOMapper::gSystem = nullptr;
        f.mapper->inactive = false;
    }

    // Acquisition failures always return a cleanup token that unpins cleanly.
    // Native state changed at real IOKit call boundaries. Pending initialization
    // is refused before a waiting constructor; post-constructor loss cannot
    // proceed to descriptor attachment/prepare/segment publication.
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        Fixture f(false); f.attach(); DmaPin pin {};
        auto pending = [] { IOMapper::gSystem = reinterpret_cast<IOMapper *>(uintptr_t(2)); };
        if (boundary == 0) Mock::faults.afterDescriptorPrepare = pending;
        if (boundary == 1) Mock::faults.afterSpecification = pending;
        if (boundary == 2) Mock::faults.afterSetDescriptor = pending;
        if (boundary == 3) Mock::faults.afterCommandPrepare = pending;
        CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie);
        CHECK(Mock::pendingMapperWaits == 0 && Mock::stores == 0);
        CHECK(Mock::specCalls == (boundary == 0 ? 0u : 1u));
        CHECK(Mock::descriptorSetCalls == (boundary < 2 ? 0u : 1u));
        CHECK(Mock::commandPrepareCalls == (boundary < 3 ? 0u : 1u));
        CHECK(f.unpin(pin) == MemoryStatus::Ok && !pin.cookie && f.adapter.pinnedBytes() == 0);
        IOMapper::gSystem = nullptr;
    }
    // If a real mapper appears within native selection, kBypassed itself
    // rejects it. Forced kUnmapped would silently accept this boundary.
    { Fixture f(false); f.attach(); DmaPin pin {}; transitionMapper = f.mapper;
      Mock::faults.beforeSpecification = [] { IOMapper::gSystem = transitionMapper; };
      CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie);
      CHECK(Mock::nativeBypassRejections == 1 && Mock::pendingMapperWaits == 0);
      CHECK(Mock::descriptorSetCalls == 0 && Mock::commandPrepareCalls == 0);
      CHECK(Mock::lastCommand == nullptr && f.mapper->refs == 1);
      CHECK(f.unpin(pin) == MemoryStatus::Ok && !pin.cookie);
      IOMapper::gSystem = nullptr; transitionMapper = nullptr; }
    { Fixture f; f.attach(); DmaPin pin {}; Mock::faults.mallocFailureCall = 1;
      CHECK(f.pin(pin) == MemoryStatus::Capacity && !pin.cookie && f.adapter.pinnedBytes() == 0); }
    for (bool iommu : {true, false}) {
        for (unsigned fault = 0; fault < 10; ++fault) {
            Fixture f(iommu);
            switch (fault) {
                case 0: Mock::faults.mallocFailureCall = 2; break; case 1: Mock::faults.bufferFactory = true; break;
                case 2: Mock::faults.noCpu = true; break; case 3: Mock::faults.descriptorPrepare = true; break;
                case 4: Mock::faults.commandFactory = true; break; case 5: Mock::faults.setDescriptor = true; break;
                case 6: Mock::faults.setDescriptor = true; Mock::faults.setDescriptorAcquires = false; break;
                case 7: Mock::faults.commandPrepare = true; break; case 8: Mock::faults.store = true; break;
                case 9: Mock::faults.copyDescriptor = true; break;
            }
            cleanPinFailure(f);
        }
        for (unsigned fault = 0; fault < 9; ++fault) {
            Fixture f(iommu);
            switch (fault) {
                case 0: Mock::faults.segmentError = 1; break; case 1: Mock::faults.segmentCount = 1; break;
                case 2: Mock::faults.segmentLength = 1; break; case 3: Mock::faults.segmentOffset = 1; break;
                case 4: Mock::faults.segmentAlignment = 1; break; case 5: Mock::faults.segmentWidth = 1; break;
                case 6: Mock::faults.physicalShort = 1; break; case 7: Mock::faults.physicalHigh = 1; break;
                case 8: Mock::faults.translatedBounce = 1; break;
            }
            cleanPinFailure(f);
            CHECK(Mock::stores == 0); // Rejected pages are never cleaned or published.
        }
    }
    // Authority lost after the segment proof, before publication: cleanup, no Ok.
    for (unsigned lost = 0; lost < 3; ++lost) {
        Fixture f(lost != 2); f.attach(); DmaPin pin {};
        Mock::faults.syncContext = &f.vm;
        if (lost == 0) Mock::faults.afterStore = [](void *v) { static_cast<Vm *>(v)->admitted = false; };
        if (lost == 1) Mock::faults.afterStore = [](void *v) { static_cast<Vm *>(v)->device->fixtureProperty("iommu-parent", nullptr); };
        if (lost == 2) Mock::faults.afterStore = [](void *) { IOMapper::gSystem = reinterpret_cast<IOMapper *>(uintptr_t(2)); };
        CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie && Mock::stores == 1);
        CHECK(f.unpin(pin) == MemoryStatus::Ok && f.adapter.pinnedBytes() == 0);
    }

    // Slot, capacity and forged-token handling.
    { Fixture f; f.attach(); DmaPin pin {}, second {}, third {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      CHECK(f.pin(second) == MemoryStatus::Busy && !second.cookie);
      CHECK(f.pin(second, 31) == MemoryStatus::Ok && f.adapter.pinnedBytes() == 16384);
      CHECK(f.pin(third, 1) == MemoryStatus::Capacity && !third.cookie);
      CHECK(f.pin(third, 32) == MemoryStatus::Invalid && f.pin(third, 1, 4097) == MemoryStatus::Invalid);
      CHECK(f.unpin(second, 31) == MemoryStatus::Ok && f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f(false); f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      auto bad = f.handle(); ++bad.generation;
      CHECK(f.backend.unpin(f.backend.context, f.identity.device, 42, bad, pin) == MemoryStatus::Ownership);
      CHECK(f.backend.unpin(f.backend.context, f.identity.device, 43, f.handle(), pin) == MemoryStatus::Ownership);
      DmaPin forged = pin; forged.cookie = reinterpret_cast<void *>(uintptr_t(1));
      CHECK(f.backend.unpin(f.backend.context, f.identity.device, 42, f.handle(), forged) == MemoryStatus::Ownership);
      forged = pin; ++forged.pageCount;
      CHECK(f.sync(forged, CacheDirection::ForCpu) == MemoryStatus::Ownership);
      forged = pin; forged.cpu += 4096;
      CHECK(f.sync(forged, CacheDirection::ForDevice) == MemoryStatus::Ownership);
      CHECK(f.sync(pin, static_cast<CacheDirection>(99)) == MemoryStatus::Invalid);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }

    // Cache maintenance failures are reported, never treated as completed.
    for (unsigned fault = 0; fault < 4; ++fault) {
        Fixture f(fault & 1); f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
        const auto stores = Mock::stores, flushes = Mock::flushes;
        if (fault < 2) {
            Mock::faults.synchronize = true;
            CHECK(f.sync(pin, CacheDirection::ForDevice) == MemoryStatus::IoFailure && Mock::stores == stores);
            CHECK(f.sync(pin, CacheDirection::ForCpu) == MemoryStatus::IoFailure && Mock::flushes == flushes);
        } else {
            Mock::faults.store = true; Mock::faults.flush = true;
            CHECK(f.sync(pin, CacheDirection::ForDevice) == MemoryStatus::IoFailure && Mock::stores == stores + 1);
            CHECK(f.sync(pin, CacheDirection::ForCpu) == MemoryStatus::IoFailure && Mock::flushes == flushes + 1);
        }
        Mock::faults = {}; CHECK(f.unpin(pin) == MemoryStatus::Ok);
    }
    // After quiescence only CPU-side invalidation remains available.
    { Fixture f(false); f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      CHECK(f.backend.quiesce(f.backend.context, f.identity.device) && f.vm.stops == 1);
      f.vm.admitted = false;
      CHECK(f.sync(pin, CacheDirection::ForDevice) == MemoryStatus::Ownership);
      size_t mark = Mock::events.size();
      CHECK(f.sync(pin, CacheDirection::ForCpu) == MemoryStatus::Ok);
      CHECK(Mock::sequence({"fence.full", "dma.sync.in", "md.flush", "fence.full"}, mark));
      DmaPin second {}; CHECK(f.pin(second, 1, 4096) == MemoryStatus::Ownership && !second.cookie);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); f.vm.quiesced = false;
      CHECK(!f.backend.quiesce(f.backend.context, f.identity.device)); DmaPin pin {};
      CHECK(f.pin(pin) == MemoryStatus::Ok && f.unpin(pin) == MemoryStatus::Ok); }

    // Portable MemoryOwner integration over both routes.
    for (bool iommu : {true, false}) {
        Fixture f(iommu); f.attach(); MemoryOwner memory; MemoryHandle handle {};
        CHECK(memory.initialize(f.identity.device, f.limits, f.backend) == MemoryStatus::Ok);
        CHECK(memory.allocate(42, 8192, handle) == MemoryStatus::Ok && memory.map(42, handle, true) == MemoryStatus::Ok);
        CHECK(memory.retain(42, handle) == MemoryStatus::Ok && memory.synchronizeRetained(42, handle, CacheDirection::ForCpu) == MemoryStatus::Ok);
        CHECK(memory.retire(42, handle) == MemoryStatus::Busy && memory.close() == MemoryStatus::Busy);
        CHECK(f.vm.stops == 1 && f.adapter.pinnedBytes() == 8192);
        CHECK(memory.release(42, handle) == MemoryStatus::Ok && memory.close() == MemoryStatus::Ok);
        CHECK(f.vm.unmaps == 1 && f.adapter.pinnedBytes() == 0 && memory.allocations() == 0);
        CHECK(Mock::flushes >= 1 && Mock::stores >= 2);
    }
    { Fixture f; f.attach(); MemoryOwner memory; MemoryHandle handle {};
      CHECK(memory.initialize(f.identity.device, f.limits, f.backend) == MemoryStatus::Ok);
      CHECK(memory.allocate(42, 8192, handle) == MemoryStatus::Ok);
      Mock::faults.syncContext = &f.vm;
      Mock::faults.afterSync = [](void *context, IOOptionBits direction) {
          if (direction == kIODirectionOut) static_cast<Vm *>(context)->admitted = false;
      };
      // Admission disappears after the device-direction sync, before VM map.
      CHECK(memory.map(42, handle, true) == MemoryStatus::Quarantined);
      MemoryView view {};
      CHECK(memory.inspect(42, handle, view) == MemoryStatus::Ok && view.state == MemoryState::Quarantined);
      CHECK(f.vm.maps == 0 && f.adapter.pinnedBytes() == 8192);
      CHECK(memory.close() == MemoryStatus::Ok && f.vm.stops == 1 && f.vm.maps == 0 && f.vm.unmaps == 0);
      CHECK(f.adapter.pinnedBytes() == 0 && memory.allocations() == 0); }

    // VM publication ownership is delegated, never synthesized.
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      GpuMapping forged {reinterpret_cast<void *>(uintptr_t(1)), 0x1000000, 8192, true};
      CHECK(f.backend.unmap(f.backend.context, f.identity.device, 42, f.handle(), pin, forged) == MemoryStatus::Ownership);
      GpuMapping empty {};
      CHECK(f.backend.unmap(f.backend.context, f.identity.device, 42, f.handle(), pin, empty) == MemoryStatus::Ok);
      CHECK(f.vm.unmaps == 0 && f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f(false); f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok); GpuMapping mapping {};
      f.vm.mapStatus = MemoryStatus::Unavailable;
      CHECK(f.backend.map(f.backend.context, f.identity.device, 42, f.handle(), pin, true, mapping) == MemoryStatus::Unavailable);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok); GpuMapping mapping {};
      f.vm.mapStatus = MemoryStatus::IoFailure; f.vm.partialMap = true;
      CHECK(f.backend.map(f.backend.context, f.identity.device, 42, f.handle(), pin, true, mapping) == MemoryStatus::IoFailure);
      CHECK(f.unpin(pin) == MemoryStatus::Busy && f.adapter.pinnedBytes() == 8192);
      f.vm.unmapStatus = MemoryStatus::IoFailure;
      CHECK(f.backend.unmap(f.backend.context, f.identity.device, 42, f.handle(), pin, mapping) == MemoryStatus::IoFailure);
      CHECK(f.unpin(pin) == MemoryStatus::Busy);
      f.vm.unmapStatus = MemoryStatus::Ok;
      CHECK(f.backend.unmap(f.backend.context, f.identity.device, 42, f.handle(), pin, mapping) == MemoryStatus::Ok && !mapping.cookie);
      CHECK(f.unpin(pin) == MemoryStatus::Ok); }
    CHECK(Mock::objects.empty() && Mock::allocations.empty());

    // Permanent UNKNOWN-completion quarantine runs after leak-free cases.
    { Fixture f; f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      Mock::faults.commandComplete = true; retainUnknown(f, pin); }
    { Fixture f(false); f.attach(); DmaPin pin {}; CHECK(f.pin(pin) == MemoryStatus::Ok);
      Mock::faults.descriptorComplete = true; retainUnknown(f, pin, true); }
    { Fixture f(false); f.attach(); DmaPin pin {}; Mock::faults.commandPrepare = true; Mock::faults.commandPrepareAcquires = false;
      CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie); retainUnknown(f, pin); }
    { Fixture f; f.attach(); DmaPin pin {}; Mock::faults.commandPrepare = true;
      CHECK(f.pin(pin) == MemoryStatus::IoFailure && pin.cookie); Mock::faults.commandComplete = true; retainUnknown(f, pin); }
    CHECK(!Mock::objects.empty() && !Mock::allocations.empty());
    Mock::disposeQuarantinedHostWorld();
    CHECK(Mock::objects.empty() && Mock::allocations.empty());
    std::printf("PASS native GLK DMA IOKit boundary: %u checks; HOSTED injected OS, no hardware/DMA/cache/GPU\n", checks);
}

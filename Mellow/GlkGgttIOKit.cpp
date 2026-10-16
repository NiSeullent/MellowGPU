// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// GGTT sequencing is re-expressed from Linux i915 (MIT); notices, blobs and
// symbols: Drivers/NativeGlk/GgttPublication.hpp and its provenance record.
#include "GlkGgttIOKit.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace MellowNative {
namespace {
namespace Ggtt = Mellow::NativeGlk;
namespace Gen9Lp = Mellow::NativeGlk::Gen9LpGgtt;

bool same(DeviceIdentity a, DeviceIdentity b) {
    return a.vendor == b.vendor && a.device == b.device &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool same(MemoryHandle a, MemoryHandle b) {
    return a.slot == b.slot && a.generation == b.generation &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool plausibleId(uint16_t id) { return id && id != UINT16_MAX; }

// Live configuration identity; the same tuple GlkDmaMemoryIOKit admits.
bool identityHolds(IOPCIDevice *device, const GlkDmaIdentity &identity) {
    const uint32_t id = device->configRead32(kIOPCIConfigVendorID);
    const uint32_t subsystem = device->configRead32(kIOPCIConfigSubSystemVendorID);
    return static_cast<uint16_t>(id) == identity.device.vendor &&
        static_cast<uint16_t>(id >> 16) == identity.device.device &&
        device->configRead8(kIOPCIConfigRevisionID) == identity.revision &&
        static_cast<uint16_t>(subsystem) == identity.subsystemVendor &&
        static_cast<uint16_t>(subsystem >> 16) == identity.subsystem &&
        device->getRegistryEntryID() == identity.device.registryId;
}

MemoryStatus fromGgtt(Ggtt::GgttStatus status) {
    switch (status) {
    case Ggtt::GgttStatus::Ok: return MemoryStatus::Ok;
    case Ggtt::GgttStatus::InvalidArgument:
    case Ggtt::GgttStatus::Overlap: return MemoryStatus::Invalid;
    case Ggtt::GgttStatus::Ownership: return MemoryStatus::Ownership;
    case Ggtt::GgttStatus::Busy: return MemoryStatus::Busy;
    case Ggtt::GgttStatus::Capacity: return MemoryStatus::Capacity;
    case Ggtt::GgttStatus::Quarantined: return MemoryStatus::Quarantined;
    case Ggtt::GgttStatus::ClockRegression: return MemoryStatus::ClockRegression;
    case Ggtt::GgttStatus::ReadbackMismatch:
    case Ggtt::GgttStatus::InvalidateTimeout:
    case Ggtt::GgttStatus::TransportFailure: return MemoryStatus::IoFailure;
    case Ggtt::GgttStatus::Unavailable:
    case Ggtt::GgttStatus::EncodingRejected: return MemoryStatus::Unavailable;
    }
    return MemoryStatus::IoFailure;
}
} // namespace

MemoryStatus GlkGgttIOKit::attach(IOPCIDevice *device, IOService *owner, GlkDmaIdentity identity,
                                  GlkGgttAuthority authority) {
    if (device_) return MemoryStatus::Busy;
    const auto &id = identity.device;
    if (!device || !owner || id.vendor != GlkDmaVendor || id.device != GlkDmaDevice ||
        !id.registryId || !id.epoch || !plausibleId(identity.subsystemVendor) ||
        !plausibleId(identity.subsystem)) return MemoryStatus::Invalid;
    if (!authority.context || !authority.admitted || !authority.handover || !authority.acquireGt ||
        !authority.releaseGt || !authority.retired || !authority.quiesce || !authority.awakeEngines)
        return MemoryStatus::Unavailable;
    if (owner->getProvider() != device || !device->isOpen(owner) || device->isInactive() ||
        owner->isInactive()) return MemoryStatus::Ownership;
    // The owner's lease precedes every configuration read and BAR mapping.
    if (!authority.admitted(authority.context, device, owner, id)) return MemoryStatus::Ownership;
    if (!identityHolds(device, identity)) return MemoryStatus::Ownership;
    const uint32_t barLow = device->configRead32(kIOPCIConfigBaseAddress0);
    const uint32_t barHigh = device->configRead32(kIOPCIConfigBaseAddress1);
    const uint16_t command = device->configRead16(kIOPCIConfigCommand);
    const uint16_t gmch = device->configRead16(Gen9Lp::kGmchControlRegister);
    uint64_t base = 0;
    if (Ggtt::decodeGttMmadrBar(barLow, barHigh, command, base) != Ggtt::GgttStatus::Ok)
        return MemoryStatus::Unavailable;
    // The IOKit resource must be the very BAR the configuration space names.
    IODeviceMemory *bar = device->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar || bar->getPhysicalAddress() != base || bar->getLength() != Gen9Lp::kGttMmadrBytes)
        return MemoryStatus::Unavailable;
    Ggtt::GgttGeometry geometry {};
    if (Ggtt::deriveGgttGeometry(gmch, bar->getLength(), geometry) != Ggtt::GgttStatus::Ok)
        return MemoryStatus::Unavailable;

    device_ = device; owner_ = owner;
    device_->retain(); owner_->retain();
    identity_ = identity; authority_ = authority; geometry_ = geometry;
    barBase_ = base; barLow_ = barLow; barHigh_ = barHigh; gmchControl_ = gmch; stopped_ = false;
    // ggtt_probe_common: ioremap (uncached), never ioremap_wc, on GEN9_LP.
    gsmMap_ = bar->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache,
        geometry.gsmBarOffset, geometry.gsmBytes);
    if (!mappingHolds(gsmMap_, base + geometry.gsmBarOffset, geometry.gsmBytes)) {
        unwind(); return MemoryStatus::Unavailable;
    }
    registerMap_ = bar->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache,
        Gen9Lp::kRegisterWindowOffset, Gen9Lp::kRegisterWindowBytes);
    if (!mappingHolds(registerMap_, base + Gen9Lp::kRegisterWindowOffset, Gen9Lp::kRegisterWindowBytes)) {
        unwind(); return MemoryStatus::Unavailable;
    }
    gsm_ = reinterpret_cast<volatile uint64_t *>(gsmMap_->getVirtualAddress());
    registers_ = reinterpret_cast<volatile uint32_t *>(registerMap_->getVirtualAddress());

    Ggtt::GgttHandover handover {};
    if (!authority.handover(authority.context, id, geometry.total, handover) ||
        handover.registryId != id.registryId || handover.epoch != id.epoch) {
        unwind(); return MemoryStatus::Unavailable;
    }
    const Ggtt::GgttIo io {this, writeEntry, readEntry, writeRegister, readRegister, fence, now, pause};
    const Ggtt::GgttGtAccess gt {this, acquireGt, releaseGt, awakeEngines};
    const auto status = publisher_.initialize(geometry, handover, io, gt);
    if (status != Ggtt::GgttStatus::Ok) {
        unwind(); return status == Ggtt::GgttStatus::Unavailable ? MemoryStatus::Unavailable
                                                                   : MemoryStatus::Invalid;
    }
    if (!observe()) { (void)publisher_.shutdown(); unwind(); return MemoryStatus::Ownership; }
    return MemoryStatus::Ok;
}

bool GlkGgttIOKit::mappingHolds(IOMemoryMap *map, uint64_t physical, uint64_t bytes) {
    if (!map) return false;
    const IOVirtualAddress address = map->getVirtualAddress();
    return address && !(address & (MemoryOwner::PageBytes - 1)) && map->getLength() == bytes &&
        map->getPhysicalAddress() == physical &&
        (map->getMapOptions() & kIOMapCacheMask) == kIOMapInhibitCache;
}

void GlkGgttIOKit::unwind() {
    gsm_ = nullptr; registers_ = nullptr;
    // Releasing the owner can destroy a service embedding this adapter.
    // Close every field before any callback, then use captured locals only.
    IOMemoryMap *registerMap = registerMap_;
    IOMemoryMap *gsmMap = gsmMap_;
    IOPCIDevice *device = device_;
    IOService *owner = owner_;
    registerMap_ = nullptr; gsmMap_ = nullptr;
    owner_ = nullptr; device_ = nullptr;
    identity_ = {}; authority_ = {}; geometry_ = {};
    barBase_ = 0; barLow_ = barHigh_ = 0; gmchControl_ = 0; stopped_ = false;
    // Last mapping references remove the kernel mappings. Owner goes last;
    // no access to this is permitted after that release.
    if (registerMap) registerMap->release();
    if (gsmMap) gsmMap->release();
    if (device) device->release();
    if (owner) owner->release();
}

bool GlkGgttIOKit::observe() const {
    if (!device_ || !owner_ || !gsm_ || !registers_ || owner_->getProvider() != device_ ||
        !device_->isOpen(owner_) || device_->isInactive() || owner_->isInactive()) return false;
    if (!authority_.admitted(authority_.context, device_, owner_, identity_.device)) return false;
    if (!identityHolds(device_, identity_)) return false;
    // A moved, resized or decode-disabled BAR, or a changed GSM size, means
    // the mappings no longer name this GGTT; nothing may be written.
    uint64_t base = 0;
    if (device_->configRead32(kIOPCIConfigBaseAddress0) != barLow_ ||
        device_->configRead32(kIOPCIConfigBaseAddress1) != barHigh_ ||
        device_->configRead16(Gen9Lp::kGmchControlRegister) != gmchControl_ ||
        Ggtt::decodeGttMmadrBar(barLow_, barHigh_, device_->configRead16(kIOPCIConfigCommand), base) !=
            Ggtt::GgttStatus::Ok || base != barBase_) return false;
    IODeviceMemory *bar = device_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    return bar && bar->getPhysicalAddress() == barBase_ && bar->getLength() == Gen9Lp::kGttMmadrBytes;
}

bool GlkGgttIOKit::engineControl(uint32_t offset) {
    for (uint32_t control : Gen9Lp::kEngineTlbControls) if (offset == control) return true;
    return false;
}

bool GlkGgttIOKit::writeEntry(void *opaque, uint64_t index, const uint8_t (&le)[8]) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    if (!self.gsm_ || index >= self.geometry_.entryCount) return false;
    glkGgttStore64(self.gsm_ + index, Ggtt::loadLe64(le));
    return true;
}

bool GlkGgttIOKit::readEntry(void *opaque, uint64_t index, uint8_t (&le)[8]) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    if (!self.gsm_ || index >= self.geometry_.entryCount) return false;
    Ggtt::storeLe64(glkGgttLoad64(self.gsm_ + index), le);
    return true;
}

bool GlkGgttIOKit::writeRegister(void *opaque, uint32_t offset, uint32_t value) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    if (!self.registers_ || !engineControl(offset)) return false;
    glkGgttStore32(self.registers_ + (offset - Gen9Lp::kRegisterWindowOffset) / 4, value);
    return true;
}

bool GlkGgttIOKit::readRegister(void *opaque, uint32_t offset, uint32_t &value) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    if (!self.registers_ || !engineControl(offset)) return false;
    value = glkGgttLoad32(self.registers_ + (offset - Gen9Lp::kRegisterWindowOffset) / 4);
    return true;
}

void GlkGgttIOKit::fence(void *, Ggtt::GgttFence kind) {
    glkCpuFence(kind == Ggtt::GgttFence::Store ? GlkCpuFence::Store
              : kind == Ggtt::GgttFence::Load ? GlkCpuFence::Load : GlkCpuFence::Full);
}

bool GlkGgttIOKit::now(void *, uint64_t &nanoseconds) {
    uint64_t absolute = 0;
    clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute, &nanoseconds);
    return true;
}

void GlkGgttIOKit::pause(void *, uint32_t microseconds) {
    // Never shorter than requested; the slow phase may sleep (owner lock is
    // sleepable), the tight phase spins.
    if (microseconds >= 1000) IOSleep((microseconds + 999) / 1000);
    else IODelay(microseconds);
}

bool GlkGgttIOKit::acquireGt(void *opaque, uint64_t &token) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    // Re-prove the live BAR and lease immediately before any effect.
    if (!self.observe()) return false;
    return self.authority_.acquireGt(self.authority_.context, self.identity_.device, token);
}

bool GlkGgttIOKit::releaseGt(void *opaque, uint64_t token) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    return self.authority_.releaseGt(self.authority_.context, self.identity_.device, token);
}

bool GlkGgttIOKit::awakeEngines(void *opaque, uint64_t token, uint32_t &mask) {
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    mask = 0;
    if (!token || !self.observe()) return false;
    uint32_t observed = 0;
    if (!self.authority_.awakeEngines(self.authority_.context, self.identity_.device, token, observed) ||
        !self.observe()) return false;
    mask = observed;
    return true;
}

GlkPhysicalVmOps GlkGgttIOKit::vmOps() {
    return {this, admitted, map, unmap, retired, quiesce};
}

uint32_t GlkGgttIOKit::liveBindings() const {
    uint32_t count = 0;
    for (const auto &entry : bindings_) count += entry.used;
    return count;
}

MemoryStatus GlkGgttIOKit::detach() {
    for (const auto &entry : bindings_) {
        if (entry.used) return entry.quarantined ? MemoryStatus::Quarantined : MemoryStatus::Busy;
    }
    const auto status = publisher_.shutdown();
    if (status != Ggtt::GgttStatus::Ok) return fromGgtt(status);
    unwind();
    return MemoryStatus::Ok;
}

bool GlkGgttIOKit::admitted(void *opaque, IOPCIDevice *device, IOService *owner, const DeviceIdentity &id) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    return self.device_ && device == self.device_ && owner == self.owner_ &&
        same(id, self.identity_.device) && self.observe();
}

GlkGgttIOKit::Binding *GlkGgttIOKit::binding(const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, const GpuMapping &mapping) {
    if (!device_ || !same(device, identity_.device) || handle.slot >= MemoryOwner::MaxAllocations)
        return nullptr;
    Binding &entry = bindings_[handle.slot];
    // Compare the opaque cookie to the table slot BEFORE trusting anything else.
    if (mapping.cookie != &entry || !entry.used || entry.owner != owner || !same(entry.handle, handle) ||
        !same(entry.device, device) || entry.pinCookie != pin.cookie || entry.pages != pin.pages ||
        entry.pageCount != pin.pageCount || entry.bytes != pin.bytes ||
        mapping.address != entry.extent.start || mapping.bytes != entry.extent.bytes ||
        !mapping.writable) return nullptr;
    return &entry;
}

MemoryStatus GlkGgttIOKit::map(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &mapping) {
    mapping = {};
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    // Every refusal below is clean: Unavailable with an empty mapping.
    if (!self.device_ || self.stopped_ || !same(device, self.identity_.device) || !owner ||
        handle.slot >= MemoryOwner::MaxAllocations || !handle.generation ||
        handle.registryId != device.registryId || handle.epoch != device.epoch)
        return MemoryStatus::Unavailable;
    // Gen8 GGTT entries carry no RW bit; read-only cannot be expressed.
    if (!writable) return MemoryStatus::Unavailable;
    if (!pin.cookie || !pin.pages || !pin.pageCount || pin.deviceRegistryId != device.registryId ||
        pin.pageCount > MemoryOwner::MaxAllocationBytes / MemoryOwner::PageBytes ||
        pin.bytes != uint64_t(pin.pageCount) * MemoryOwner::PageBytes) return MemoryStatus::Unavailable;
    Binding &entry = self.bindings_[handle.slot];
    if (entry.used || !self.observe()) return MemoryStatus::Unavailable;
    Ggtt::GgttExtent extent {};
    if (self.publisher_.reserve(owner, pin.bytes, extent) != Ggtt::GgttStatus::Ok)
        return MemoryStatus::Unavailable;
    const size_t captureBytes = pin.pageCount * sizeof(uint64_t);
    auto *capture = static_cast<uint64_t *>(IOMalloc(captureBytes));
    if (!capture) {
        (void)self.publisher_.release(owner, extent);
        return MemoryStatus::Unavailable;
    }
    const auto status = self.publisher_.publish(owner, extent, pin.pages, pin.pageCount,
                                                capture, pin.pageCount);
    if (status != Ggtt::GgttStatus::Ok &&
        self.publisher_.state(extent) == Ggtt::GgttExtentState::Reserved) {
        // Refused before the first entry store: nothing to undo.
        (void)self.publisher_.release(owner, extent);
        IOFree(capture, captureBytes);
        return MemoryStatus::Unavailable;
    }
    entry = {};
    entry.used = true;
    entry.quarantined = status != Ggtt::GgttStatus::Ok;
    entry.device = device; entry.owner = owner; entry.handle = handle;
    entry.pinCookie = pin.cookie; entry.pages = pin.pages; entry.pageCount = pin.pageCount;
    entry.bytes = pin.bytes; entry.extent = extent; entry.capture = capture;
    // Even an uncertain publication returns its cleanup mapping.
    mapping = {&entry, extent.start, extent.bytes, true};
    if (status != Ggtt::GgttStatus::Ok) return MemoryStatus::IoFailure;
    return MemoryStatus::Ok;
}

MemoryStatus GlkGgttIOKit::unmap(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, GpuMapping &mapping) {
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    Binding *entry = self.binding(device, owner, handle, pin, mapping);
    if (!entry) return MemoryStatus::Ownership;
    if (entry->quarantined) return MemoryStatus::Quarantined;
    // Epoch quiescence covers every consumer; otherwise ask the queue owner.
    const GlkGgttConsumerView view {device, owner, handle, entry->extent.start, entry->extent.bytes};
    const bool retiredNow = self.stopped_ || self.authority_.retired(self.authority_.context, view);
    if (!retiredNow) return MemoryStatus::Busy;
    const auto status = self.publisher_.unpublish(owner, entry->extent, true);
    if (status == Ggtt::GgttStatus::Ok &&
        self.publisher_.release(owner, entry->extent) == Ggtt::GgttStatus::Ok) {
        IOFree(entry->capture, entry->pageCount * sizeof(uint64_t));
        *entry = {};
        mapping = {};
        return MemoryStatus::Ok;
    }
    if (self.publisher_.state(entry->extent) == Ggtt::GgttExtentState::Published)
        return status == Ggtt::GgttStatus::Unavailable ? MemoryStatus::Busy : fromGgtt(status);
    // Restore or invalidation uncertain: keep extent, capture and pages.
    entry->quarantined = true;
    return status == Ggtt::GgttStatus::Ok ? MemoryStatus::Quarantined : fromGgtt(status);
}

bool GlkGgttIOKit::retired(void *opaque, const MemoryView &view) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    Binding *entry = self.binding(view.device, view.owner, view.handle, view.pin, view.mapping);
    if (!entry || entry->quarantined || !self.observe()) return false;
    const GlkGgttConsumerView consumer {view.device, view.owner, view.handle,
                                        entry->extent.start, entry->extent.bytes};
    return self.authority_.retired(self.authority_.context, consumer);
}

bool GlkGgttIOKit::quiesce(void *opaque, const DeviceIdentity &device) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkGgttIOKit *>(opaque);
    if (!self.device_ || !same(device, self.identity_.device)) return false;
    if (self.stopped_) return true;
    if (!self.authority_.quiesce(self.authority_.context, device)) return false;
    self.stopped_ = true;
    return true;
}

} // namespace MellowNative

// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "GlkDmaMemoryIOKit.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace MellowNative {
namespace {
bool same(DeviceIdentity a, DeviceIdentity b) {
    return a.vendor == b.vendor && a.device == b.device &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool same(MemoryHandle a, MemoryHandle b) {
    return a.slot == b.slot && a.generation == b.generation &&
        a.registryId == b.registryId && a.epoch == b.epoch;
}
bool empty(const GpuMapping &m) {
    return !m.cookie && !m.address && !m.bytes && !m.writable;
}
bool plausibleId(uint16_t id) { return id && id != UINT16_MAX; }
}
struct GlkDmaMemoryIOKit::Resource {
    GlkDmaMemoryIOKit *adapter {};
    DeviceIdentity device {};
    MemoryHandle handle {};
    uint64_t owner {}, bytes {};
    size_t pageCount {};
    IOBufferMemoryDescriptor *memory {};
    IODMACommand *command {};
    IOMapper *mapper {};
    uint64_t *pages {};
    GlkDmaRoute route {GlkDmaRoute::Unavailable};
    bool descriptorAttempted {}, descriptorPrepared {}, commandAttempted {}, commandPrepared {};
    bool commandCompleteUncertain {}, descriptorCompleteUncertain {};
    bool ready {}, mapAttempted {};
};

MemoryStatus GlkDmaMemoryIOKit::attach(IOPCIDevice *device, IOService *owner,
        GlkDmaIdentity identity, MemoryLimits limits, GlkPhysicalVmOps vm) {
    if (device_) return MemoryStatus::Busy;
    const auto &id = identity.device;
    if (!device || !owner || id.vendor != GlkDmaVendor || id.device != GlkDmaDevice ||
        !id.registryId || !id.epoch || !plausibleId(identity.subsystemVendor) ||
        !plausibleId(identity.subsystem) || limits.dmaAddressBits != GlkDmaAddressBits ||
        limits.gpuAddressBits < 39 || limits.gpuAddressBits > 48 ||
        !limits.maxAllocationBytes || limits.maxAllocationBytes > MemoryOwner::MaxAllocationBytes ||
        (limits.maxAllocationBytes & (MemoryOwner::PageBytes - 1)) ||
        limits.maxTotalBytes < limits.maxAllocationBytes ||
        limits.maxTotalBytes > MemoryOwner::MaxAllocationBytes * MemoryOwner::MaxAllocations)
        return MemoryStatus::Invalid;
    if (!vm.context || !vm.admitted || !vm.map || !vm.unmap || !vm.retired || !vm.quiesce)
        return MemoryStatus::Unavailable;
    if (owner->getProvider() != device || !device->isOpen(owner) ||
        device->isInactive() || owner->isInactive()) return MemoryStatus::Ownership;
    // The owner's lease precedes route proof and every configuration read.
    if (!vm.admitted(vm.context, device, owner, id)) return MemoryStatus::Ownership;
    IOMapper *mapper = nullptr;
    const auto route = selectRoute(device, mapper);
    if (route == GlkDmaRoute::Unavailable) return MemoryStatus::Unavailable;
    device_ = device; owner_ = owner; mapper_ = mapper; // property owns the mapper retain.
    device_->retain(); owner_->retain();
    identity_ = identity; limits_ = limits; vm_ = vm; route_ = route; stopped_ = false;
    if (!observe()) { detach(); return MemoryStatus::Ownership; }
    return MemoryStatus::Ok;
}
GlkDmaRoute GlkDmaMemoryIOKit::selectRoute(IOPCIDevice *device, IOMapper *&mapper) {
    mapper = nullptr;
    auto *property = device->copyProperty("iommu-parent");
    if (property) {
        // A numeric ID still names a remapping unit. Resolving it would need
        // copyMapperForDevice lookup/wait, and it contradicts direct addressing.
        auto *candidate = OSDynamicCast(IOMapper, property);
        if (!candidate || candidate->isInactive()) {
            property->release();
            return GlkDmaRoute::Unavailable;
        }
        mapper = candidate;
        return GlkDmaRoute::IommuMapped;
    }
    // Read once, never waited on. Only kNoMapper (null) is compatible with
    // OS-authorized no-system-mapper DMA; pending or live state vetoes it.
    // The actual kBypassed native constructor/prepare and each original page
    // are still checked before a usable pin is published.
    if (IOMapper::gSystem) return GlkDmaRoute::Unavailable;
    return GlkDmaRoute::DirectPhysical;
}
bool GlkDmaMemoryIOKit::routeHolds() const {
    auto *property = device_->copyProperty("iommu-parent");
    bool holds = false;
    if (route_ == GlkDmaRoute::IommuMapped)
        holds = mapper_ && property == mapper_ && !mapper_->isInactive();
    else if (route_ == GlkDmaRoute::DirectPhysical)
        holds = !property && !mapper_ && !IOMapper::gSystem;
    if (property) property->release();
    return holds;
}
bool GlkDmaMemoryIOKit::observe() const {
    if (!device_ || !owner_ || stopped_ || route_ == GlkDmaRoute::Unavailable ||
        owner_->getProvider() != device_ || !device_->isOpen(owner_) ||
        device_->isInactive() || owner_->isInactive()) return false;
    if (!vm_.admitted(vm_.context, device_, owner_, identity_.device)) return false;
    const uint32_t id = device_->configRead32(kIOPCIConfigVendorID);
    const uint32_t subsystem = device_->configRead32(kIOPCIConfigSubSystemVendorID);
    if (static_cast<uint16_t>(id) != identity_.device.vendor ||
        static_cast<uint16_t>(id >> 16) != identity_.device.device ||
        device_->configRead8(kIOPCIConfigRevisionID) != identity_.revision ||
        static_cast<uint16_t>(subsystem) != identity_.subsystemVendor ||
        static_cast<uint16_t>(subsystem >> 16) != identity_.subsystem ||
        device_->getRegistryEntryID() != identity_.device.registryId) return false;
    return routeHolds();
}
MemoryBackend GlkDmaMemoryIOKit::backend() {
    return {this, admitted, pin, unpin, synchronize, map, unmap, retired, quiesce};
}
MemoryStatus GlkDmaMemoryIOKit::detach() {
    for (auto *resource : resources_) if (resource) return MemoryStatus::Busy;
    if (pinnedBytes_) return MemoryStatus::Quarantined;
    if (mapper_) { mapper_->release(); mapper_ = nullptr; }
    if (owner_) { owner_->release(); owner_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    identity_ = {}; limits_ = {}; vm_ = {}; route_ = GlkDmaRoute::Unavailable; stopped_ = false;
    return MemoryStatus::Ok;
}
bool GlkDmaMemoryIOKit::admitted(void *opaque, const DeviceIdentity &device) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    return same(device, self.identity_.device) && self.observe();
}
void GlkDmaMemoryIOKit::describe(Resource &r, DmaPin &out) {
    out = {&r, r.memory ? static_cast<uint8_t *>(r.memory->getBytesNoCopy()) : nullptr,
        r.pages, r.pageCount, r.bytes, r.device.registryId};
}
GlkDmaMemoryIOKit::Resource *GlkDmaMemoryIOKit::checked(const DeviceIdentity &device,
        uint64_t owner, const MemoryHandle &handle, const DmaPin &pin, bool complete) const {
    if (!device_ || !same(device, identity_.device) || !owner ||
        handle.slot >= MemoryOwner::MaxAllocations ||
        handle.registryId != identity_.device.registryId || handle.epoch != identity_.device.epoch)
        return nullptr;
    auto *r = resources_[handle.slot];
    // Compare the opaque pointer to retained table metadata BEFORE dereferencing.
    if (!r || pin.cookie != r || r->adapter != this || r->owner != owner ||
        !same(r->device, device) || !same(r->handle, handle) ||
        pin.pages != r->pages || pin.pageCount != r->pageCount || pin.bytes != r->bytes ||
        pin.deviceRegistryId != r->device.registryId) return nullptr;
    if (complete && (r->commandCompleteUncertain || r->descriptorCompleteUncertain ||
        !r->descriptorPrepared || !r->commandPrepared || !r->ready || r->route != route_ ||
        !r->memory || !r->command || !r->pages ||
        (r->route == GlkDmaRoute::IommuMapped) != (r->mapper != nullptr) ||
        pin.cpu != r->memory->getBytesNoCopy())) return nullptr;
    return r;
}
MemoryStatus GlkDmaMemoryIOKit::pin(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, uint64_t bytes, DmaPin &out) {
    out = {};
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    if (!owner || !bytes || (bytes & (MemoryOwner::PageBytes - 1)) || bytes > SIZE_MAX ||
        bytes > self.limits_.maxAllocationBytes || handle.slot >= MemoryOwner::MaxAllocations ||
        !handle.generation || handle.registryId != device.registryId || handle.epoch != device.epoch)
        return MemoryStatus::Invalid;
    // Route authority is re-proven before any allocation or DMA preparation.
    if (!same(device, self.identity_.device) || !self.observe()) return MemoryStatus::Ownership;
    if (self.resources_[handle.slot]) return MemoryStatus::Busy;
    if (self.pinnedBytes_ > self.limits_.maxTotalBytes ||
        bytes > self.limits_.maxTotalBytes - self.pinnedBytes_) return MemoryStatus::Capacity;
    auto *r = static_cast<Resource *>(IOMalloc(sizeof(Resource)));
    if (!r) return MemoryStatus::Capacity;
    *r = {};
    r->adapter = &self; r->device = device; r->handle = handle; r->owner = owner; r->bytes = bytes;
    r->pageCount = static_cast<size_t>(bytes / MemoryOwner::PageBytes);
    r->route = self.route_;
    const bool mapped = r->route == GlkDmaRoute::IommuMapped;
    if (mapped) { r->mapper = self.mapper_; r->mapper->retain(); }
    self.resources_[handle.slot] = r; self.pinnedBytes_ += bytes;
    // Failures after acquisition ALWAYS return this retained cleanup resource.
    r->pages = static_cast<uint64_t *>(IOMalloc(r->pageCount * sizeof(uint64_t)));
    if (!r->pages) { describe(*r, out); return MemoryStatus::IoFailure; }
    bzero(r->pages, r->pageCount * sizeof(uint64_t));
    // Wired kernel pages below 2^39: neither route can need a bounce page.
    r->memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut,
        bytes, GlkDmaPhysicalMask);
    if (!r->memory || !r->memory->getBytesNoCopy()) { describe(*r, out); return MemoryStatus::IoFailure; }
    bzero(r->memory->getBytesNoCopy(), static_cast<size_t>(bytes));
    r->descriptorAttempted = true;
    if (r->memory->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    r->descriptorPrepared = true;
    // Never force kUnmapped: it skips native system-mapper eligibility. The
    // normal initialized platform contract makes kNoMapper terminal for this
    // lease. Recheck after descriptor preparation before entering a native
    // constructor that could wait if global initialization were pending.
    if (!self.observe()) { describe(*r, out); return MemoryStatus::IoFailure; }
    r->command = IODMACommand::withSpecification(IODMACommand::OutputHost64, GlkDmaAddressBits,
        MemoryOwner::PageBytes, mapped ? IODMACommand::kMapped : IODMACommand::kBypassed,
        bytes, MemoryOwner::PageBytes, mapped ? r->mapper : nullptr);
    // Successful kBypassed proves native selection found no mapper. A changed
    // route, including a new pending state, cannot be used to prepare DMA.
    if (!r->command || !self.observe() ||
        r->command->setMemoryDescriptor(r->memory, false) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    if (!self.observe()) { describe(*r, out); return MemoryStatus::IoFailure; }
    r->commandAttempted = true;
    if (r->command->prepare(0, bytes) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    r->commandPrepared = true;
    if (!self.observe()) { describe(*r, out); return MemoryStatus::IoFailure; }
    UInt64 offset = 0;
    const uint64_t limit = 1ULL << GlkDmaAddressBits;
    for (size_t i = 0; i < r->pageCount; ++i) {
        IODMACommand::Segment64 segment {};
        UInt32 count = 1; const UInt64 before = offset;
        if (r->command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
            count != 1 || segment.fLength != MemoryOwner::PageBytes ||
            offset != before + MemoryOwner::PageBytes ||
            (segment.fIOVMAddr & (MemoryOwner::PageBytes - 1)) ||
            segment.fIOVMAddr > limit - MemoryOwner::PageBytes) {
            describe(*r, out); return MemoryStatus::IoFailure;
        }
        IOByteCount length = 0;
        const uint64_t original = r->memory->getPhysicalSegment(before, &length, kIOMemoryMapperNone);
        // Every device page must be the original descriptor page: an IOVA
        // translating to it, or that exact physical page. Anything else is a
        // bounce/copy page and is rejected rather than synchronized.
        if (length < MemoryOwner::PageBytes || !original ||
            (original & (MemoryOwner::PageBytes - 1)) || original > limit - MemoryOwner::PageBytes ||
            r->command->getIOMemoryDescriptor() != r->memory ||
            (mapped ? r->mapper->mapToPhysicalAddress(segment.fIOVMAddr) != original
                    : segment.fIOVMAddr != original)) {
            describe(*r, out); return MemoryStatus::IoFailure;
        }
        r->pages[i] = segment.fIOVMAddr;
    }
    if (offset != bytes) { describe(*r, out); return MemoryStatus::IoFailure; }
    // The zero fill may still sit in CPU caches; an unsnooped read must not
    // see older memory contents. Clean every line before publishing the pin.
    if (r->memory->performOperation(kIOMemoryIncoherentIOStore, 0, bytes) != kIOReturnSuccess) {
        describe(*r, out); return MemoryStatus::IoFailure;
    }
    glkCpuFence(GlkCpuFence::Full);
    describe(*r, out);
    if (!self.observe()) return MemoryStatus::IoFailure;
    r->ready = true;
    return MemoryStatus::Ok;
}
MemoryStatus GlkDmaMemoryIOKit::releaseResource(Resource &r) {
    if (r.mapAttempted) return MemoryStatus::Busy;
    // Unknown inverse DMA outcome: keep backing, mapper and charge forever.
    // Neither NotReady, a later success, GPU reset nor repetition proves the
    // translation or wiring was reclaimed.
    if (r.commandCompleteUncertain || r.descriptorCompleteUncertain)
        return MemoryStatus::Quarantined;
    if (r.command) {
        if (r.commandAttempted) {
            if (r.command->complete(true, true) != kIOReturnSuccess) {
                r.commandCompleteUncertain = true;
                return MemoryStatus::IoFailure;
            }
            r.commandAttempted = false; r.commandPrepared = false;
        }
        // Never clearMemoryDescriptor(true): it discards completion errors.
        if (r.command->getMemoryDescriptor()) {
            if (r.command->clearMemoryDescriptor(false) != kIOReturnSuccess ||
                r.command->getMemoryDescriptor()) return MemoryStatus::IoFailure;
        }
        r.command->release(); r.command = nullptr;
    }
    if (r.descriptorAttempted) {
        if (!r.memory || r.memory->complete(kIODirectionInOut) != kIOReturnSuccess) {
            r.descriptorCompleteUncertain = true;
            return MemoryStatus::IoFailure;
        }
        r.descriptorAttempted = false; r.descriptorPrepared = false;
    }
    if (r.memory) { r.memory->release(); r.memory = nullptr; }
    if (r.mapper) { r.mapper->release(); r.mapper = nullptr; }
    if (r.pages) { IOFree(r.pages, r.pageCount * sizeof(uint64_t)); r.pages = nullptr; }
    pinnedBytes_ -= r.bytes; resources_[r.handle.slot] = nullptr;
    IOFree(&r, sizeof(Resource));
    return MemoryStatus::Ok;
}
MemoryStatus GlkDmaMemoryIOKit::unpin(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, DmaPin &pin) {
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, false);
    if (!r) return MemoryStatus::Ownership;
    const auto result = self.releaseResource(*r);
    if (result == MemoryStatus::Ok) pin = {};
    else describe(*r, pin);
    return result;
}
MemoryStatus GlkDmaMemoryIOKit::synchronize(void *opaque, const DeviceIdentity &device,
        uint64_t owner, const MemoryHandle &handle, const DmaPin &pin, CacheDirection direction) {
    if (!opaque) return MemoryStatus::Unavailable;
    if (direction != CacheDirection::ForDevice && direction != CacheDirection::ForCpu)
        return MemoryStatus::Invalid;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r) return MemoryStatus::Ownership;
    if (self.stopped_) {
        if (direction != CacheDirection::ForCpu) return MemoryStatus::Ownership;
    } else if (!self.observe()) return MemoryStatus::Ownership;
    // IODMACommand::synchronize copies only bounce pages (proven absent) and
    // never maintains caches for kMapped/kBypassed, so clflush explicitly.
    if (direction == CacheDirection::ForDevice) {
        if (r->command->synchronize(kIODirectionOut) != kIOReturnSuccess ||
            r->memory->performOperation(kIOMemoryIncoherentIOStore, 0, r->bytes) != kIOReturnSuccess)
            return MemoryStatus::IoFailure;
        glkCpuFence(GlkCpuFence::Full);
        return MemoryStatus::Ok;
    }
    // Order the caller's completion-proof read before discarding lines the
    // CPU may have speculatively refilled while the device owned the pages.
    glkCpuFence(GlkCpuFence::Full);
    if (r->command->synchronize(kIODirectionIn) != kIOReturnSuccess ||
        r->memory->performOperation(kIOMemoryIncoherentIOFlush, 0, r->bytes) != kIOReturnSuccess)
        return MemoryStatus::IoFailure;
    glkCpuFence(GlkCpuFence::Full);
    return MemoryStatus::Ok;
}
MemoryStatus GlkDmaMemoryIOKit::map(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &mapping) {
    mapping = {};
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r || !self.observe()) return MemoryStatus::Ownership;
    if (r->mapAttempted) return MemoryStatus::Busy;
    r->mapAttempted = true;
    const auto result = self.vm_.map(self.vm_.context, device, owner, handle, pin, writable, mapping);
    if (result == MemoryStatus::Unavailable && empty(mapping)) r->mapAttempted = false;
    return result;
}
MemoryStatus GlkDmaMemoryIOKit::unmap(void *opaque, const DeviceIdentity &device, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, GpuMapping &mapping) {
    if (!opaque) return MemoryStatus::Unavailable;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    auto *r = self.checked(device, owner, handle, pin, true);
    if (!r) return MemoryStatus::Ownership;
    // Recorded no-publication, not a fabricated TLB acknowledgement.
    if (!r->mapAttempted) return empty(mapping) ? MemoryStatus::Ok : MemoryStatus::Ownership;
    const auto result = self.vm_.unmap(self.vm_.context, device, owner, handle, pin, mapping);
    if (result == MemoryStatus::Ok) { r->mapAttempted = false; mapping = {}; }
    return result;
}
bool GlkDmaMemoryIOKit::retired(void *opaque, const MemoryView &view) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    return self.checked(view.device, view.owner, view.handle, view.pin, true) &&
        self.observe() && self.vm_.retired(self.vm_.context, view);
}
bool GlkDmaMemoryIOKit::quiesce(void *opaque, const DeviceIdentity &device) {
    if (!opaque) return false;
    auto &self = *static_cast<GlkDmaMemoryIOKit *>(opaque);
    if (!self.device_ || !same(device, self.identity_.device)) return false;
    if (self.stopped_) return true;
    if (!self.vm_.quiesce(self.vm_.context, device)) return false;
    self.stopped_ = true;
    return true;
}

} // namespace MellowNative

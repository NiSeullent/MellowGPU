// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "GlkBcsIOKit.hpp"

namespace MellowNative {
namespace Bcs = MellowNativeGlk::Bcs;
namespace {
constexpr uint64_t GgttLimit = 1ULL << 32;
constexpr uint16_t CommandMemorySpace = 0x0002, CommandBusLead = 0x0004; // IOPCIFamilyDefinitions.h
// Device-memory MMIO, the readl()/writel() shape: one aligned 32-bit access
// with compiler barriers so it is neither merged, split nor reordered.
inline uint32_t mmioRead(volatile uint8_t *base, uint32_t offset) {
    __asm__ volatile("" ::: "memory");
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("" ::: "memory");
    return value;
}
inline void mmioWrite(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    __asm__ volatile("" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("" ::: "memory");
}
} // namespace

bool GlkBcsIOKit::deviceAdmitted() const {
    if (!device_ || !owner_ || owner_->getProvider() != device_ || !device_->isOpen(owner_) ||
        device_->isInactive() || owner_->isInactive() ||
        device_->getRegistryEntryID() != identity_.registryId)
        return false;
    const uint32_t id = device_->configRead32(kIOPCIConfigVendorID);
    const uint32_t subsystem = device_->configRead32(kIOPCIConfigSubSystemVendorID);
    const uint8_t revision = device_->configRead8(kIOPCIConfigRevisionID);
    const uint16_t command = device_->configRead16(kIOPCIConfigCommand);
    return id == (uint32_t(identity_.device) << 16 | identity_.vendor) &&
           subsystem == (uint32_t(identity_.subsystem) << 16 | identity_.subsystemVendor) &&
           revision == identity_.revision && (command & CommandMemorySpace) &&
           (command & CommandBusLead);
}

GlkBcsIOKit::Result GlkBcsIOKit::attach(IOPCIDevice *device, IOService *owner,
        const GlkBcsIdentity &identity, MemoryOwner *memory, uint64_t memoryOwner,
        const GlkBcsAuthorities &ops) {
    if (device_) return Result::Busy;
    if (!device || !owner || !memory || !memoryOwner || identity.vendor != GlkBcsVendor ||
        identity.device != GlkBcsDevice || !identity.registryId || !identity.epoch)
        return Result::Invalid;
    const DeviceIdentity owned = memory->device();
    if (owned.vendor != identity.vendor || owned.device != identity.device ||
        owned.registryId != identity.registryId || owned.epoch != identity.epoch)
        return Result::Ownership;
    device_ = device; owner_ = owner; identity_ = identity;
    if (!deviceAdmitted()) { device_ = nullptr; owner_ = nullptr; identity_ = {}; return Result::Ownership; }
    IOMemoryMap *map = device->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!map) { device_ = nullptr; owner_ = nullptr; identity_ = {}; return Result::Unavailable; }
    const uint64_t address = map->getVirtualAddress();
    if (!address || (address & 0xfff) || map->getLength() < GlkRegisterWindowBytes) {
        map->release();
        device_ = nullptr; owner_ = nullptr; identity_ = {};
        return Result::Unavailable;
    }
    map_ = map;
    mmio_ = reinterpret_cast<volatile uint8_t *>(static_cast<uintptr_t>(address));
    memory_ = memory; memoryOwner_ = memoryOwner; ops_ = ops; violations_ = 0;
    return Result::Ok;
}

bool GlkBcsIOKit::lease(MemoryHandle memory, Bcs::LeaseKind kind, Bcs::Lease &out) const {
    if (!memory_ || !ops_.ggtt.bound) return false;
    MemoryView view;
    if (memory_->inspect(memoryOwner_, memory, view) != MemoryStatus::Ok) return false;
    const bool gpuWrites = kind != Bcs::LeaseKind::Ring; // HWSP, context save, probe store
    if (view.state != MemoryState::Mapped || view.device.registryId != identity_.registryId ||
        view.device.epoch != identity_.epoch || !view.pin.cpu || view.mapping.bytes != view.bytes ||
        !view.mapping.address || view.mapping.address >= GgttLimit ||
        view.mapping.bytes > GgttLimit - view.mapping.address || (gpuWrites && !view.mapping.writable))
        return false;
    if (!ops_.ggtt.bound(ops_.ggtt.context, view, view.mapping.address)) return false;
    Bcs::Lease result;
    result.kind = kind;
    result.owner = memoryOwner_;
    result.slot = memory.slot;
    result.generation = memory.generation;
    result.deviceEpoch = identity_.epoch;
    result.address = view.mapping.address;
    result.bytes = view.mapping.bytes;
    result.cpu = view.pin.cpu;
    out = result;
    return true;
}

MemoryHandle GlkBcsIOKit::handle(const Bcs::Lease &lease) const {
    MemoryHandle memory;
    memory.slot = lease.slot <= UINT32_MAX ? uint32_t(lease.slot) : UINT32_MAX;
    memory.generation = lease.generation;
    memory.registryId = identity_.registryId;
    memory.epoch = identity_.epoch;
    return memory;
}

GlkBcsIOKit::Result GlkBcsIOKit::bind(const GlkBcsMemory &memory, uint32_t initialSeqno) {
    if (!device_) return Result::Invalid;
    if (engine_.state() != Bcs::EngineState::Detached) return Result::Busy;
    // Pending wiring stays visible: without these owners nothing is bound.
    if (!ops_.forcewake.held || !ops_.ggtt.bound || !ops_.ppgtt.live || !ops_.ppgtt.hold ||
        !ops_.ppgtt.drop)
        return Result::Unavailable;
    if (!deviceAdmitted()) return Result::Ownership;
    Bcs::Bindings bindings;
    if (!lease(memory.statusPage, Bcs::LeaseKind::StatusPage, bindings.statusPage) ||
        !lease(memory.context, Bcs::LeaseKind::Context, bindings.context) ||
        !lease(memory.ring, Bcs::LeaseKind::Ring, bindings.ring))
        return Result::Ownership;
    bindings.ppgttRoot.kind = Bcs::LeaseKind::PpgttRoot;
    bindings.ppgttRoot.owner = memoryOwner_;
    bindings.ppgttRoot.generation = memory.pml4Generation;
    bindings.ppgttRoot.deviceEpoch = identity_.epoch;
    bindings.ppgttRoot.address = memory.pml4Dma;
    bindings.ppgttRoot.bytes = MemoryOwner::PageBytes;
    Bcs::Platform platform;
    platform.deviceEpoch = identity_.epoch;
    platform.mmio = mmioPort();
    platform.forcewake = {this, forcewakeHeld};
    platform.reset = {this, ops_.reset.resetBlitter ? resetEngine : nullptr};
    platform.leases = {this, holds, retain, release, synchronize};
    lastEngine_ = engine_.attach(platform, bindings, initialSeqno);
    return lastEngine_ == Bcs::Status::Ok ? Result::Ok : Result::Engine;
}

GlkBcsIOKit::Result GlkBcsIOKit::leaseProbe(MemoryHandle memory, Bcs::Lease &out) const {
    if (!device_) return Result::Invalid;
    return lease(memory, Bcs::LeaseKind::ProbeTarget, out) ? Result::Ok : Result::Ownership;
}

GlkBcsIOKit::Result GlkBcsIOKit::detach() {
    if (!device_) return Result::Invalid;
    if (engine_.state() != Bcs::EngineState::Detached) return Result::Busy;
    map_->release();
    map_ = nullptr; mmio_ = nullptr; device_ = nullptr; owner_ = nullptr;
    memory_ = nullptr; memoryOwner_ = 0; identity_ = {}; ops_ = {};
    return Result::Ok;
}

uint32_t GlkBcsIOKit::read32(void *context, uint32_t offset) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (!self->mmio_ || !Bcs::inWindow(offset)) { ++self->violations_; return 0xFFFFFFFFu; }
    return mmioRead(self->mmio_, offset);
}

void GlkBcsIOKit::write32(void *context, uint32_t offset, uint32_t value) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (!self->mmio_ || !Bcs::inWindow(offset)) { ++self->violations_; return; }
    mmioWrite(self->mmio_, offset, value);
}

void GlkBcsIOKit::delay(void *, uint32_t microseconds) { IODelay(microseconds); }

void GlkBcsIOKit::fence(void *, Bcs::Fence kind) { glkBcsCpuFence(kind); }

bool GlkBcsIOKit::forcewakeHeld(void *context, uint64_t epoch, uint32_t domains) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    return epoch == self->identity_.epoch && self->ops_.forcewake.held &&
           self->ops_.forcewake.held(self->ops_.forcewake.context, epoch, domains);
}

bool GlkBcsIOKit::resetEngine(void *context, uint64_t epoch) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    return epoch == self->identity_.epoch && self->ops_.reset.resetBlitter &&
           self->ops_.reset.resetBlitter(self->ops_.reset.context, epoch);
}

bool GlkBcsIOKit::holds(void *context, const Bcs::Lease &lease) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (lease.owner != self->memoryOwner_ || lease.deviceEpoch != self->identity_.epoch) return false;
    if (lease.kind == Bcs::LeaseKind::PpgttRoot)
        return self->ops_.ppgtt.live &&
               self->ops_.ppgtt.live(self->ops_.ppgtt.context, lease.deviceEpoch,
                                     lease.generation, lease.address);
    Bcs::Lease current;
    return self->lease(self->handle(lease), lease.kind, current) &&
           current.address == lease.address && current.bytes == lease.bytes &&
           current.cpu == lease.cpu && current.generation == lease.generation;
}

bool GlkBcsIOKit::retain(void *context, const Bcs::Lease &lease) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (!holds(context, lease)) return false;
    if (lease.kind == Bcs::LeaseKind::PpgttRoot)
        return self->ops_.ppgtt.hold(self->ops_.ppgtt.context, lease.deviceEpoch, lease.generation);
    return self->memory_->retain(self->memoryOwner_, self->handle(lease)) == MemoryStatus::Ok;
}

bool GlkBcsIOKit::release(void *context, const Bcs::Lease &lease) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (lease.owner != self->memoryOwner_ || lease.deviceEpoch != self->identity_.epoch) return false;
    if (lease.kind == Bcs::LeaseKind::PpgttRoot)
        return self->ops_.ppgtt.drop(self->ops_.ppgtt.context, lease.deviceEpoch, lease.generation);
    return self->memory_->release(self->memoryOwner_, self->handle(lease)) == MemoryStatus::Ok;
}

bool GlkBcsIOKit::synchronize(void *context, const Bcs::Lease &lease, Bcs::CacheOp op) {
    auto *self = static_cast<GlkBcsIOKit *>(context);
    if (lease.kind == Bcs::LeaseKind::PpgttRoot || !holds(context, lease)) return false;
    const MemoryHandle memory = self->handle(lease);
    MemoryView view;
    if (self->memory_->inspect(self->memoryOwner_, memory, view) != MemoryStatus::Ok) return false;
    const CacheDirection direction =
        op == Bcs::CacheOp::ForDevice ? CacheDirection::ForDevice : CacheDirection::ForCpu;
    // Retained (published or about to be) backing uses the queue path; the
    // owner's sync() refuses job-held memory by contract.
    const MemoryStatus status = view.jobHolds
        ? self->memory_->synchronizeRetained(self->memoryOwner_, memory, direction)
        : self->memory_->sync(self->memoryOwner_, memory, direction);
    return status == MemoryStatus::Ok;
}

} // namespace MellowNative

// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// OpRegion/VBT rules are re-expressed from Linux i915 (MIT); notices, blobs and
// symbols: Drivers/NativeGlk/OpRegionSnapshot.hpp and its provenance record.
#include "GlkOpRegionIOKit.hpp"
#include <IOKit/IOLib.h>

#ifndef MELLOW_GLK_OPREGION_HOSTED_IO
// The kext target's MacKernelSDK IOLib.h predates the macOS 12 data-heap KPI.
// Same declarations as the SDK 15.5 IOLib.h; com.apple.kpi.iokit exports both.
extern "C" {
void *IOMallocData(vm_size_t size);
void IOFreeData(void *address, vm_size_t size);
}
#endif

namespace MellowNative {
namespace NG = Mellow::NativeGlk;

namespace {
using Status = GlkOpRegionStatus;
enum class Op : uint8_t { Capture, With, Copy, Discard, Info };
// x86_64 XNU pages are 4 KiB; a physical mapping keeps the in-page offset
// (IOMemoryDescriptor.cpp memoryReferenceMap adds pageOffset back).
constexpr uint64_t kPageMask = 0xfff;

bool same(const uint8_t *a, const uint8_t *b, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) if (a[i] != b[i]) return false;
    return true;
}
bool leasedOp(Op op) { return op == Op::Capture || op == Op::With || op == Op::Copy; }
}

struct GlkOpRegionIOKit::Call {
    Op op;
    GlkOpRegionIOKit *self {};
    MellowGlkPhysicalOwner *owner {};
    uint64_t epoch {};
    GlkVbtConsumer consumer {};
    void *context {};
    uint8_t *out {};
    uint32_t capacity {};
    uint32_t *bytes {};
    GlkOpRegionInfo *info {};
    Status status {Status::Owner};
    bool ran {};
};

GlkOpRegionIOKit::~GlkOpRegionIOKit() {
    // Owned data only: firmware windows never outlive a capture, and the
    // objects of a failed unmap stay retained on purpose.
    unpublish();
}

GlkOpRegionStatus GlkOpRegionIOKit::capture(MellowGlkPhysicalOwner &owner, uint64_t epoch,
        GlkOwnerStatus *ownerStatus) {
    Call call {Op::Capture};
    call.epoch = epoch;
    return run(owner, call, true, ownerStatus);
}

GlkOpRegionStatus GlkOpRegionIOKit::withVbt(MellowGlkPhysicalOwner &owner, uint64_t epoch,
        GlkVbtConsumer consumer, void *context, GlkOwnerStatus *ownerStatus) {
    if (!consumer) return Status::Invalid;
    Call call {Op::With};
    call.epoch = epoch;
    call.consumer = consumer;
    call.context = context;
    return run(owner, call, true, ownerStatus);
}

GlkOpRegionStatus GlkOpRegionIOKit::copyVbt(MellowGlkPhysicalOwner &owner, uint64_t epoch, uint8_t *out,
        uint32_t capacity, uint32_t &bytes, GlkOwnerStatus *ownerStatus) {
    bytes = 0;
    if (!out || !capacity) return Status::Invalid;
    Call call {Op::Copy};
    call.epoch = epoch;
    call.out = out;
    call.capacity = capacity;
    call.bytes = &bytes;
    return run(owner, call, true, ownerStatus);
}

GlkOpRegionStatus GlkOpRegionIOKit::discard(MellowGlkPhysicalOwner &owner, GlkOwnerStatus *ownerStatus) {
    Call call {Op::Discard};
    return run(owner, call, false, ownerStatus);
}

GlkOpRegionStatus GlkOpRegionIOKit::info(MellowGlkPhysicalOwner &owner, GlkOpRegionInfo &out,
        GlkOwnerStatus *ownerStatus) {
    out = {};
    Call call {Op::Info};
    call.info = &out;
    return run(owner, call, false, ownerStatus);
}

GlkOpRegionStatus GlkOpRegionIOKit::run(MellowGlkPhysicalOwner &owner, Call &call, bool leased,
        GlkOwnerStatus *ownerStatus) {
    call.self = this;
    call.owner = &owner;
    // The owner's gate is the only serialization: runLeased admits the live
    // PM epoch only, runSerialized delivers a binding with epoch zero.
    const GlkOwnerStatus result = leased ? owner.runLeased(call.epoch, &gated, &call)
                                         : owner.runSerialized(&gated, &call);
    if (ownerStatus) *ownerStatus = result;
    return call.ran ? call.status : Status::Owner;
}

GlkOwnerStatus GlkOpRegionIOKit::gated(void *context, const GlkPhysicalBinding &binding) {
    auto *call = static_cast<Call *>(context);
    if (!call || !call->self) return GlkOwnerStatus::Invalid;
    call->ran = true;
    GlkOpRegionIOKit &self = *call->self;
    if (!self.bindingHolds(*call, binding)) {
        call->status = Status::Ownership;
        return GlkOwnerStatus::Ok;
    }
    if (self.inConsumer_) {
        // A consumer runs on this gate; nothing may change under its view.
        call->status = call->op == Op::Info ? Status::Ok : Status::Busy;
        if (call->op == Op::Info) *call->info = self.info_;
        return GlkOwnerStatus::Ok;
    }
    switch (call->op) {
    case Op::Capture:
        call->status = self.quarantined_ ? Status::Quarantined : self.captureGated(binding);
        break;
    case Op::With:
    case Op::Copy:
        if (!self.vbt_) { call->status = Status::NotCaptured; break; }
        // Published bytes are tied to the epoch they were captured in.
        if (self.info_.epoch != binding.identity.device.epoch) { call->status = Status::StaleEpoch; break; }
        if (call->op == Op::With) {
            self.inConsumer_ = true;
            call->consumer(call->context, self.vbt_, self.vbtBytes_, self.info_);
            self.inConsumer_ = false;
            call->status = Status::Ok;
            break;
        }
        *call->bytes = self.vbtBytes_;
        if (call->capacity < self.vbtBytes_) { call->status = Status::Invalid; break; }
        for (uint32_t i = 0; i < self.vbtBytes_; ++i) call->out[i] = self.vbt_[i];
        call->status = Status::Ok;
        break;
    case Op::Discard:
        self.unpublish();
        call->status = Status::Ok;
        break;
    case Op::Info:
        *call->info = self.info_;
        call->status = Status::Ok;
        break;
    }
    return GlkOwnerStatus::Ok;
}

bool GlkOpRegionIOKit::bindingHolds(const Call &call, const GlkPhysicalBinding &b) const {
    // Only the frozen physical owner class, as the instance the caller named,
    // inside its own gate, on its own provider and work loop.
    auto *owner = OSDynamicCast(MellowGlkPhysicalOwner, b.owner);
    if (!owner || owner != call.owner || !b.device || !b.workLoop || !b.gate) return false;
    if (!b.workLoop->inGate() || owner->getWorkLoop() != b.workLoop || owner->getProvider() != b.device)
        return false;
    const auto &p = GlkLenovo15IGL7;
    const auto &id = b.identity;
    if (id.device.vendor != p.vendor || id.device.device != p.device || id.revision != p.revision ||
        id.subsystemVendor != p.subsystemVendor || id.subsystem != p.subsystem) return false;
    if (!id.device.registryId || id.device.registryId != b.device->getRegistryEntryID()) return false;
    // Once bound, exactly one owner instance and registry entry are served.
    if (owner_ && (owner_ != owner || registryId_ != id.device.registryId)) return false;
    if (!leasedOp(call.op)) return id.device.epoch == 0;
    if (!id.device.epoch || id.device.epoch != call.epoch) return false;
    // The owner's own gated state must agree with the delivered epoch.
    const GlkOwnerSnapshot state = owner->snapshot();
    if (state.liveEpoch != call.epoch || state.quarantine != GlkQuarantine::None || !state.powered ||
        state.revoking || state.terminating || !state.providerOpen) return false;
    return b.device->isOpen(owner) && !b.device->isInactive() && identityHolds(b.device);
}

bool GlkOpRegionIOKit::identityHolds(IOPCIDevice *device) {
    // The admitted tuple, function 0, read live (an absent function reads all-ones).
    const auto &p = GlkLenovo15IGL7;
    if (device->getFunctionNumber() != 0) return false;
    const uint32_t id = device->configRead32(kIOPCIConfigVendorID);
    const uint32_t classRevision = device->configRead32(kIOPCIConfigRevisionID);
    const uint32_t subsystem = device->configRead32(kIOPCIConfigSubSystemVendorID);
    return id == (uint32_t(p.device) << 16 | p.vendor) && (classRevision & 0xff) == p.revision &&
        (classRevision >> 24) == p.baseClass && subsystem == (uint32_t(p.subsystem) << 16 | p.subsystemVendor);
}

bool GlkOpRegionIOKit::observe(IOPCIDevice *device, GlkIgdObservation &o) {
    o = {};
    o.asls = device->configRead32(NG::OpRegion::kPciAsls);
    o.ggc = device->configRead16(NG::OpRegion::kPciGgc);
    o.bdsm = device->configRead32(NG::OpRegion::kPciBdsm);
    // BDW_GMCH_GGMS_SHIFT 6 / MASK 0x3 and BDW_GMCH_GMS_SHIFT 8 / MASK 0xff.
    o.ggms = uint8_t((o.ggc >> 6) & 0x3);
    o.gms = uint8_t(o.ggc >> 8);
    o.gsmBytes = NG::gsmBytesFromGgc(o.ggc);
    o.bdsmBase = o.bdsm & NG::OpRegion::kBdsmBaseMask;
    return o.asls != UINT32_MAX && o.ggc != UINT16_MAX && o.bdsm != UINT32_MAX;
}

GlkOpRegionStatus GlkOpRegionIOKit::commit(GlkOpRegionInfo &next, GlkOpRegionStatus status) {
    next.last = status;
    next.quarantined = quarantined_;
    info_ = next;
    return status;
}

GlkOpRegionStatus GlkOpRegionIOKit::captureGated(const GlkPhysicalBinding &b) {
    const uint64_t epoch = b.identity.device.epoch;
    // Per-capture fields restart; the published snapshot's fields carry over.
    GlkOpRegionInfo next = info_;
    ++next.captures;
    next.igd = {};
    next.header = {};
    next.source = NG::VbtSource::None;
    next.rvdaVbt = next.mailboxVbt = NG::VbtStatus::NotTried;
    next.firmware = NG::OpRegionStatus::Ok;

    GlkIgdObservation first {};
    if (!observe(b.device, first)) {
        next.firmware = NG::OpRegionStatus::Unavailable;
        return commit(next, Status::Firmware);
    }
    next.igd = first;
    next.firmware = NG::checkAsls(first.asls);
    if (next.firmware != NG::OpRegionStatus::Ok) return commit(next, Status::Firmware);

    // The only physical addresses ever mapped: ASLS here, and the RVDA the
    // pure layer decodes from this OpRegion's validated header.
    Window region {};
    if (!mapFirmware(first.asls, NG::OpRegion::kBytes, region)) {
        next.firmware = NG::OpRegionStatus::Unavailable;
        return commit(next, unmapFirmware(region) ? Status::Firmware : Status::Quarantined);
    }
    const NG::FirmwareWindow window {&region, copyWindow, region.bytes};
    const NG::OpRegionPlatform platform {this, mapRvda, unmapRvda, allocate, release, observeAsls};
    NG::OpRegionSnapshot snap {};
    device_ = b.device;
    next.firmware = NG::captureOpRegion(first.asls, window, platform, snap);
    device_ = nullptr;
    const bool unmapped = unmapFirmware(region);
    next.header = snap.plan.header;
    next.source = snap.source;
    next.rvdaVbt = snap.rvdaVbt;
    next.mailboxVbt = snap.mailboxVbt;
    if (!unmapped || quarantined_ || next.firmware == NG::OpRegionStatus::Uncertain) {
        // A kernel mapping of firmware RAM may still exist: nothing new is
        // published and no later capture runs in this instance.
        NG::releaseSnapshot(platform, snap);
        quarantined_ = true;
        return commit(next, Status::Quarantined);
    }
    if (next.firmware != NG::OpRegionStatus::Ok) return commit(next, Status::Firmware);

    // GGC, BDSM and ASLS once more, after every window is gone.
    GlkIgdObservation again {};
    const bool observed = observe(b.device, again);
    if (!observed || again.asls != first.asls || again.ggc != first.ggc || again.bdsm != first.bdsm) {
        NG::releaseSnapshot(platform, snap);
        next.firmware = observed ? NG::OpRegionStatus::Incoherent : NG::OpRegionStatus::Unavailable;
        return commit(next, Status::Firmware);
    }
    if (vbt_) {
        const bool equal = vbtBytes_ == snap.vbtBytes && same(vbt_, snap.vbt, snap.vbtBytes);
        NG::releaseSnapshot(platform, snap);
        if (!equal) {
            // Firmware VBT bytes must not change within a boot; neither copy
            // is trusted and nothing stays published.
            unpublish();
            next.published = false;
            next.epoch = next.digest = 0;
            next.vbtBytes = 0;
            return commit(next, Status::Changed);
        }
        // Identical bytes: the existing buffer is rebound to this epoch.
    } else {
        vbt_ = snap.vbt;
        vbtBytes_ = snap.vbtBytes;
        next.digest = snap.digest;
        snap.vbt = nullptr;
    }
    owner_ = OSDynamicCast(MellowGlkPhysicalOwner, b.owner);
    registryId_ = b.identity.device.registryId;
    next.published = true;
    next.epoch = epoch;
    next.registryId = registryId_;
    next.vbtBytes = vbtBytes_;
    ++next.generation;
    return commit(next, Status::Ok);
}

bool GlkOpRegionIOKit::mapFirmware(uint64_t physical, uint32_t bytes, Window &w) {
    w = {};
    if (!bytes || physical > UINT64_MAX - bytes) return false;
    // TASK_NULL selects kIOMemoryTypePhysical64 (withAddressRanges); the
    // memory is only read (kIODirectionOut) and no system mapper is involved.
    w.descriptor = IOMemoryDescriptor::withAddressRange(physical, bytes,
        kIODirectionOut | kIOMemoryMapperNone, TASK_NULL);
    if (!w.descriptor) return false;
    // kIOMapDefaultCache: memoryReferenceCreate takes the physical page's own
    // mode (IODefaultCacheBits); nothing here asserts a cacheability.
    w.map = w.descriptor->createMappingInTask(kernel_task, 0,
        kIOMapAnywhere | kIOMapReadOnly | kIOMapDefaultCache, 0, bytes);
    if (!w.map) return false;
    const IOVirtualAddress address = w.map->getVirtualAddress();
    if (!address || w.map->getLength() != bytes || w.map->getPhysicalAddress() != physical ||
        !(w.map->getMapOptions() & kIOMapReadOnly) || ((address ^ physical) & kPageMask)) return false;
    w.base = reinterpret_cast<const volatile uint8_t *>(address);
    w.bytes = bytes;
    return true;
}

bool GlkOpRegionIOKit::unmapFirmware(Window &w) {
    w.base = nullptr;
    if (w.map && w.map->unmap() != kIOReturnSuccess) {
        // The kernel mapping may still exist. Both objects are kept, never
        // released and never used again; the instance is quarantined.
        for (auto &slot : retained_) {
            if (slot.map || slot.descriptor) continue;
            slot = w;
            break;
        }
        w = {};
        quarantined_ = true;
        return false;
    }
    // Releasing the map drops its descriptor reference; the descriptor last.
    if (w.map) w.map->release();
    if (w.descriptor) w.descriptor->release();
    w = {};
    return true;
}

void GlkOpRegionIOKit::unpublish() {
    if (vbt_) IOFreeData(vbt_, vbtBytes_);
    vbt_ = nullptr;
    vbtBytes_ = 0;
    info_.published = false;
    info_.epoch = info_.digest = 0;
    info_.vbtBytes = 0;
}

bool GlkOpRegionIOKit::copyWindow(void *context, uint32_t offset, uint8_t *out, uint32_t bytes) {
    auto *w = static_cast<Window *>(context);
    if (!w || !w->base || !out || offset > w->bytes || bytes > w->bytes - offset) return false;
    const volatile uint8_t *from = w->base + offset;
    for (uint32_t i = 0; i < bytes; ++i) out[i] = glkFirmwareLoad8(from + i);
    return true;
}

bool GlkOpRegionIOKit::mapRvda(void *context, uint64_t physical, uint32_t bytes, NG::FirmwareWindow &out) {
    auto *self = static_cast<GlkOpRegionIOKit *>(context);
    out = {};
    // One RVDA window, only while a capture holds the bound device.
    if (!self || !self->device_ || self->rvda_.map || self->rvda_.descriptor) return false;
    if (!mapFirmware(physical, bytes, self->rvda_)) {
        (void)self->unmapFirmware(self->rvda_);
        return false;
    }
    out = {&self->rvda_, copyWindow, self->rvda_.bytes};
    return true;
}

bool GlkOpRegionIOKit::unmapRvda(void *context) {
    auto *self = static_cast<GlkOpRegionIOKit *>(context);
    return self && self->unmapFirmware(self->rvda_);
}

uint8_t *GlkOpRegionIOKit::allocate(void *, uint32_t bytes) {
    // Data-only kernel heap: the copy holds firmware bytes, never pointers.
    return bytes ? static_cast<uint8_t *>(IOMallocData(bytes)) : nullptr;
}

void GlkOpRegionIOKit::release(void *, uint8_t *at, uint32_t bytes) {
    if (at) IOFreeData(at, bytes);
}

bool GlkOpRegionIOKit::observeAsls(void *context, uint32_t &asls) {
    auto *self = static_cast<GlkOpRegionIOKit *>(context);
    if (!self || !self->device_) return false;
    asls = self->device_->configRead32(NG::OpRegion::kPciAsls);
    return asls != UINT32_MAX;
}

} // namespace MellowNative

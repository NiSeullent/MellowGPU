// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// Native IOKit wiring; register decoding remains in InterruptDecode.cpp.
#include "GlkInterruptIOKit.hpp"

using namespace MellowNativeGlk;
using namespace MellowNativeGlk::Interrupt;
OSDefineMetaClassAndStructors(MellowGlkInterrupt, OSObject);

MellowGlkInterrupt *MellowGlkInterrupt::create() {
    auto *result = new MellowGlkInterrupt;
    if (result && !result->init()) { result->release(); return nullptr; }
    return result;
}

int MellowGlkInterrupt::messagedIndex(IOPCIDevice *device) {
    if (!device || device->isInactive()) return -1;
    // Bounded provider-index scan; no allocation, waiting or vector creation.
    for (int i = 0; i < 32; ++i) {
        int type = 0;
        if (device->getInterruptType(i, &type) != kIOReturnSuccess) return -1;
        if (type & kIOInterruptTypePCIMessaged) return i;
    }
    return -1;
}

bool MellowGlkInterrupt::proven() const {
    if (!device_ || !owner_ || !loop_ || !loop_->inGate() || !epoch_ ||
        !ops_.opaque || !ops_.admitted || !ops_.admitted(ops_.opaque, epoch_) ||
        device_->isInactive() || owner_->isInactive() || owner_->getProvider() != device_ ||
        !device_->isOpen(owner_) || device_->getRegistryEntryID() != registryId_ ||
        device_->getPowerState() != kIOPCIDeviceOnState) return false;
    // Native owner lease precedes configuration access. Never called in IRQ.
    if (device_->getBusNumber() != 0 || device_->getDeviceNumber() != 2 || device_->getFunctionNumber() != 0)
        return false;
    return device_->configRead16(kIOPCIConfigVendorID) == FirstHardware.vendor &&
        device_->configRead16(kIOPCIConfigDeviceID) == FirstHardware.device &&
        device_->configRead8(kIOPCIConfigRevisionID) == FirstHardware.revision &&
        device_->configRead16(kIOPCIConfigSubSystemVendorID) == FirstHardware.subsystemVendor &&
        device_->configRead16(kIOPCIConfigSubSystemID) == FirstHardware.subsystem &&
        (device_->configRead16(kIOPCIConfigCommand) & 6U) == 6U;
}

MellowGlkInterrupt::Status MellowGlkInterrupt::attach(IOPCIDevice *device, IOService *owner,
        IOWorkLoop *loop, int index, const Configuration &config, const RegisterOps &ops,
        const InterruptHandlers &handlers) {
    if (attached() || handling_) return Status::Invalid;
    if (!device || !owner || !loop || !loop->inGate() || index < 0 || index >= 32 ||
        !config.epoch || !sameIdentity(config.identity, FirstHardware) ||
        !ops.opaque || !ops.admitted || !ops.read32 || !ops.write32 ||
        !handlers.opaque || !handlers.engine || !handlers.fault) return Status::Invalid;
    device_ = device; owner_ = owner; loop_ = loop; index_ = index;
    registryId_ = device->getRegistryEntryID(); epoch_ = config.epoch; ops_ = ops;
    // Retain before the native source constructor: XNU registers its primary
    // handler during init(), not only when added to a workloop.
    device_->retain(); owner_->retain(); loop_->retain(); retain(); selfRetained_ = true;
    if (!proven()) { dropReferences(); return Status::Unavailable; }
    int type = 0;
    if (device_->getInterruptType(index_, &type) != kIOReturnSuccess || !(type & kIOInterruptTypePCIMessaged)) {
        dropReferences(); return Status::Unsupported;
    }
    handlers_ = handlers;
    last_ = decoder_.configure(config, ops);
    if (last_ != Status::Ok) {
        const Status original = last_;
        if (mask() == Status::Ok) dropReferences();
        return original;
    }
    source_ = IOFilterInterruptEventSource::filterInterruptEventSource(this, action, filter, device_, index_);
    if (!source_) {
        last_ = Status::Unavailable;
        if (mask() == Status::Ok) dropReferences();
        return Status::Unavailable;
    }
    // Framework enable/disable methods return void; the explicit provider
    // call verifies the idempotent disable/drain result before cleanup.
    source_->disable();
    if (device_->disableInterrupt(index_) != kIOReturnSuccess) { last_ = Status::IoFailure; return last_; }
    if (loop_->addEventSource(source_) != kIOReturnSuccess) {
        last_ = Status::IoFailure;
        if (mask() == Status::Ok) dropReferences();
        return Status::IoFailure;
    }
    sourceAdded_ = true; enabled_ = false; resume_ = false; faulted_ = false;
    return last_ = Status::Ok;
}

MellowGlkInterrupt::Status MellowGlkInterrupt::arm() {
    if (handling_ || !source_ || !sourceAdded_ || enabled_ || resume_ || faulted_ ||
        decoder_.state() != Decoder::Configured) return Status::Invalid;
    if (!proven()) return last_ = Status::Unavailable;
    source_->enable();
    if (device_->enableInterrupt(index_) != kIOReturnSuccess) {
        failClosed(Status::IoFailure); return last_;
    }
    enabled_ = true;
    last_ = decoder_.arm();
    if (last_ != Status::Ok) failClosed(last_);
    return last_;
}

MellowGlkInterrupt::Status MellowGlkInterrupt::mask() {
    decoder_.beginStop(); // primary filter inert before OS drain
    enabled_ = false;
    if (source_) {
        source_->disable();
        if (device_->disableInterrupt(index_) != kIOReturnSuccess) return Status::IoFailure;
    }
    if (decoder_.state() == Decoder::Unconfigured) return Status::Ok;
    if (!proven()) return Status::Unavailable;
    return decoder_.finishStop();
}

MellowGlkInterrupt::Status MellowGlkInterrupt::quiesce() {
    if (handling_ || !device_ || !loop_ || !loop_->inGate()) return Status::Invalid;
    last_ = mask();
    if (last_ == Status::Ok) { resume_ = true; batch_ = {}; morePasses_ = 0; }
    return last_;
}

MellowGlkInterrupt::Status MellowGlkInterrupt::resume(const Configuration &config, const RegisterOps &ops) {
    if (handling_ || !source_ || !sourceAdded_ || !loop_->inGate() || !resume_ || enabled_ ||
        decoder_.state() != Decoder::Unconfigured || config.epoch <= epoch_ ||
        !sameIdentity(config.identity, FirstHardware) || !ops.opaque || !ops.admitted ||
        !ops.read32 || !ops.write32) return Status::Invalid;
    if (!ops.admitted(ops.opaque, config.epoch)) return last_ = Status::Unavailable;
    const auto oldOps = ops_; const uint64_t oldEpoch = epoch_;
    ops_ = ops; epoch_ = config.epoch;
    if (!proven()) { ops_ = oldOps; epoch_ = oldEpoch; return last_ = Status::Unavailable; }
    // An admitted native reconfiguration attempt consumes this epoch. Failure
    // retains the disabled registration; quiesce then a fresh epoch can retry.
    last_ = decoder_.configure(config, ops);
    if (last_ == Status::Ok) { resume_ = false; faulted_ = false; batch_ = {}; morePasses_ = 0; }
    return last_;
}

MellowGlkInterrupt::Status MellowGlkInterrupt::detach() {
    if (handling_ || !device_ || !loop_ || !loop_->inGate()) return Status::Invalid;
    last_ = mask();
    if (last_ != Status::Ok) return last_; // retained disabled registration + owner
    if (!proven()) return last_ = Status::Unavailable;
    if (sourceAdded_) {
        if (loop_->removeEventSource(source_) != kIOReturnSuccess) return last_ = Status::IoFailure;
        sourceAdded_ = false;
    }
    dropReferences(); // may destroy this if caller already dropped its ref
    return Status::Ok;
}

void MellowGlkInterrupt::dropReferences() {
    if (source_) { source_->release(); source_ = nullptr; }
    if (loop_) { loop_->release(); loop_ = nullptr; }
    if (owner_) { owner_->release(); owner_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    ops_ = {}; handlers_ = {}; batch_ = {}; registryId_ = 0; epoch_ = 0; index_ = -1;
    enabled_ = resume_ = faulted_ = false;
    if (selfRetained_) { selfRetained_ = false; release(); }
}

void MellowGlkInterrupt::failClosed(Status status) {
    faulted_ = true; last_ = status;
    (void)mask(); // failure preserves all native ownership
    batch_ = {};
    const bool wasHandling = handling_; handling_ = true;
    handlers_.fault(handlers_.opaque, epoch_, status, 0xff);
    handling_ = wasHandling;
}

bool MellowGlkInterrupt::filter(OSObject *object, IOFilterInterruptEventSource *sender) {
    // No generic IOKit/config calls in primary context. The source constructor
    // may register before source_ is assigned; Configured decoder is inert.
    return object && sender && static_cast<MellowGlkInterrupt *>(object)->decoder_.filter();
}

void MellowGlkInterrupt::action(OSObject *object, IOInterruptEventSource *sender, int count) {
    if (!object || !sender || count <= 0) return;
    auto &self = *static_cast<MellowGlkInterrupt *>(object);
    if (sender != self.source_ || !self.loop_ || !self.loop_->inGate() || self.handling_) return;
    self.handling_ = true;
    if (!self.enabled_) { self.handling_ = false; return; } // stale work after stop
    if (!self.proven()) { self.failClosed(Status::Unavailable); self.handling_ = false; return; }
    if (self.decoder_.state() == Decoder::Pending || self.decoder_.state() == Decoder::Fault) {
        ++self.deliveries_; self.morePasses_ = 0;
        const Status status = self.decoder_.collect(self.batch_);
        if (status != Status::Ok) { self.failClosed(status); self.handling_ = false; return; }
    }
    if (self.decoder_.state() != Decoder::Servicing) { self.handling_ = false; return; }
    for (unsigned n = 0; n < EngineCount; ++n) {
        const unsigned i = (self.nextEngine_ + n) % EngineCount;
        if (!self.batch_.events[i]) continue;
        const auto engine = static_cast<Engine>(i);
        const Handling done = self.handlers_.engine(self.handlers_.opaque, self.epoch_, engine, self.batch_.events[i]);
        if (!self.proven()) { self.failClosed(Status::Unavailable); self.handling_ = false; return; }
        if (done == Handling::More) continue;
        self.batch_.events[i] = 0;
        if (done != Handling::Done) {
            const Status masked = self.decoder_.maskEngine(engine);
            if (masked != Status::Ok) { self.failClosed(masked); self.handling_ = false; return; }
            self.handlers_.fault(self.handlers_.opaque, self.epoch_, Status::Faulted, engineBit(engine));
            if (!self.proven()) { self.failClosed(Status::Unavailable); self.handling_ = false; return; }
        }
    }
    self.nextEngine_ = static_cast<uint8_t>((self.nextEngine_ + 1) % EngineCount);
    if (!self.decoder_.engines()) self.failClosed(Status::Faulted);
    else if (self.batch_.any()) {
        if (++self.morePasses_ >= MorePassLimit) self.failClosed(Status::Storm);
        else self.source_->signalInterrupt(); // actual workloop re-schedule, GT remains masked
    } else {
        const Status status = self.decoder_.rearm();
        if (status != Status::Ok) self.failClosed(status);
    }
    self.handling_ = false;
}

void MellowGlkInterrupt::free() {
    // Internal retain prevents normal finalfree while native resources live.
    // Never implicitly detach/unregister an uncertain interrupt from free().
    if (source_ || device_ || owner_ || loop_ || selfRetained_) return;
    OSObject::free();
}

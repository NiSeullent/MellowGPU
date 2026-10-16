// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "GlkPhysicalOwnerIOKit.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <kern/clock.h>
#include <libkern/version.h>

#define super IOService

namespace MellowNative {
OSDefineMetaClassAndStructors(MellowGlkPhysicalOwner, IOService);

namespace {
// Register layout, torvalds/linux@0d9ff90a include/uapi/linux/pci_regs.h.
constexpr uint8_t PciHeaderType = 0x0e, PciHeaderLayoutMask = 0x7f;
constexpr uint8_t PciCapabilityList = 0x34, PciCapIdPm = 0x01, PciPmControl = 4;
constexpr uint16_t PciStatusCapList = 0x10, PciPmStateMask = 0x0003;
// Bound for the read-only capability walk; a longer chain is malformed.
constexpr unsigned PciCapabilityTtl = 48;
// kIOPCICommandBusLead in IOPCIFamilyDefinitions.h; older kernel SDK headers
// (the kext target's MacKernelSDK) name the same bit kIOPCICommandBusMaster.
constexpr uint16_t PciCommandBusLead = 0x0004;
// Prefer non-virtual setBusLeadEnable where declared; otherwise use the
// virtual setBusMasterEnable that older headers expose for the same bit.
template <typename Device>
auto setBusLead(Device *device, bool enable, int) -> decltype(device->setBusLeadEnable(enable)) {
    return device->setBusLeadEnable(enable);
}
template <typename Device>
bool setBusLead(Device *device, bool enable, long) { return device->setBusMasterEnable(enable); }
using Status = GlkOwnerStatus;
enum class Op : uint8_t {
    Power, Acquire, Release, RunLeased, RunSerialized, Reset, Wait,
    Terminate, CloseRequest, Stop, Snapshot
};
}

struct MellowGlkPhysicalOwner::GateCall {
    Op op;
    unsigned long ordinal {};
    uint64_t epoch {};
    uint32_t timeoutUs {};
    GlkLeaseHold *hold {};
    GlkGatedAction action {};
    void *context {};
    GlkOwnerSnapshot *snapshot {};
    Status status {Status::Unavailable};
    IOReturn power {kIOPMAckImplied};
    // Effects performed by the caller after the gate is opened again.
    bool acknowledge {}, close {};
};

bool MellowGlkPhysicalOwner::start(IOService *provider) {
    auto *device = OSDynamicCast(IOPCIDevice, provider);
    // An x86_64 kext never runs translated; the kernel itself must be the
    // Darwin release whose contracts this code was written against.
    if (!device || started_ || device_ || version_major != GlkKernelMajor) return false;
    if (!super::start(provider)) return false;
    // Exclusive native ownership precedes every configuration access.
    if (!device->open(this)) {
        super::stop(provider);
        return false;
    }
    device_ = device;
    device_->retain();
    providerOpen_ = true;
    registryId_ = device_->getRegistryEntryID();
    bool ok = registryId_ && identityHolds() && findPowerCapability();
    if (ok) {
        workLoop_ = IOWorkLoop::workLoop();
        gate_ = workLoop_ ? IOCommandGate::commandGate(this) : nullptr;
        timer_ = workLoop_ ? IOTimerEventSource::timerEventSource(this, &drainExpired) : nullptr;
        ok = gate_ && timer_ && workLoop_->addEventSource(gate_) == kIOReturnSuccess;
        if (ok && workLoop_->addEventSource(timer_) != kIOReturnSuccess) {
            workLoop_->removeEventSource(gate_);
            ok = false;
        }
        sourcesAdded_ = ok;
    }
    if (ok) {
        powerStates_[PowerOff] = {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        powerStates_[PowerOn] = {kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable,
            kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0};
        PMinit();
        pmInitialized_ = true;
        device_->joinPMtree(this);
        ok = registerPowerDriver(this, powerStates_, PowerStateCount) == kIOReturnSuccess;
    }
    if (!ok) {
        // Nothing was published: undo in reverse and close the provider.
        if (pmInitialized_) { PMstop(); pmInitialized_ = false; }
        teardownSources();
        providerOpen_ = false;
        device_->close(this);
        super::stop(provider);
        return false;
    }
    started_ = true;
    // Driver desire only; system sleep still lowers this node through PM, and
    // an epoch appears only when PM delivers setPowerState(On).
    changePowerStateTo(PowerOn);
    return true;
}

void MellowGlkPhysicalOwner::stop(IOService *provider) {
    // PMstop waits for in-flight PM driver calls, which take the gate.
    if (pmInitialized_) { PMstop(); pmInitialized_ = false; }
    if (started_ && provider == device_) {
        // Own termination: actionStop calls stop() first and then closes the
        // provider itself, so the drain here is synchronous and bounded.
        GateCall call {Op::Stop};
        if (enter(call)) finishPost(call);
    }
    if (quarantine_ == GlkQuarantine::None) teardownSources();
    started_ = false;
    super::stop(provider);
}

void MellowGlkPhysicalOwner::free() {
    // Never reached while quarantined: that state retains this instance.
    teardownSources();
    if (timer_) { timer_->release(); timer_ = nullptr; }
    if (gate_) { gate_->release(); gate_ = nullptr; }
    if (workLoop_) { workLoop_->release(); workLoop_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    super::free();
}

IOWorkLoop *MellowGlkPhysicalOwner::getWorkLoop() const { return workLoop_; }

void MellowGlkPhysicalOwner::teardownSources() {
    if (!sourcesAdded_) return;
    timer_->cancelTimeout();
    timerArmed_ = false;
    workLoop_->removeEventSource(timer_);
    workLoop_->removeEventSource(gate_);
    sourcesAdded_ = false;
}

bool MellowGlkPhysicalOwner::enter(GateCall &call) {
    return gate_ && gate_->runAction(&MellowGlkPhysicalOwner::gated, &call) == kIOReturnSuccess;
}

IOReturn MellowGlkPhysicalOwner::gated(OSObject *target, void *call, void *, void *, void *) {
    auto *self = OSDynamicCast(MellowGlkPhysicalOwner, target);
    if (!self || !call) return kIOReturnBadArgument;
    self->dispatch(*static_cast<GateCall *>(call));
    return kIOReturnSuccess;
}

void MellowGlkPhysicalOwner::finishPost(GateCall &call) {
    // acknowledgeSetPowerState only queues a PM request (IOServicePM.cpp);
    // close() takes the provider's arbitration lock, so it runs ungated.
    if (call.acknowledge) acknowledgeSetPowerState();
    if (call.close) device_->close(this);
}

IOReturn MellowGlkPhysicalOwner::setPowerState(unsigned long ordinal, IOService *whatDevice) {
    if (whatDevice != this || ordinal >= PowerStateCount) return kIOPMAckImplied;
    GateCall call {Op::Power};
    call.ordinal = ordinal;
    if (!enter(call)) return kIOPMAckImplied;
    finishPost(call);
    return call.power;
}

IOReturn MellowGlkPhysicalOwner::message(UInt32 type, IOService *provider, void *argument) {
    if (provider && provider == device_ &&
        (type == kIOMessageServiceIsTerminated || type == kIOMessageServiceIsRequestingClose)) {
        GateCall call {type == kIOMessageServiceIsRequestingClose ? Op::CloseRequest : Op::Terminate};
        if (enter(call)) finishPost(call);
        return kIOReturnSuccess;
    }
    return super::message(type, provider, argument);
}

bool MellowGlkPhysicalOwner::willTerminate(IOService *provider, IOOptionBits options) {
    if (provider && provider == device_) {
        GateCall call {Op::Terminate};
        if (enter(call)) finishPost(call);
    }
    return super::willTerminate(provider, options);
}

bool MellowGlkPhysicalOwner::didTerminate(IOService *provider, IOOptionBits options, bool *defer) {
    // defer stays false: while this client keeps the provider open the base
    // implementation requests close, and close() schedules stop only once
    // revocation has completed cleanly. Quarantine never closes.
    return super::didTerminate(provider, options, defer);
}

GlkOwnerStatus MellowGlkPhysicalOwner::acquireHold(uint64_t epoch, GlkLeaseHold &hold) {
    hold = {};
    GateCall call {Op::Acquire};
    call.epoch = epoch;
    call.hold = &hold;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerStatus MellowGlkPhysicalOwner::releaseHold(GlkLeaseHold &hold) {
    GateCall call {Op::Release};
    call.hold = &hold;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerStatus MellowGlkPhysicalOwner::runLeased(uint64_t epoch, GlkGatedAction action, void *context) {
    if (!action) return Status::Invalid;
    GateCall call {Op::RunLeased};
    call.epoch = epoch;
    call.action = action;
    call.context = context;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerStatus MellowGlkPhysicalOwner::runSerialized(GlkGatedAction action, void *context) {
    if (!action) return Status::Invalid;
    GateCall call {Op::RunSerialized};
    call.action = action;
    call.context = context;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerStatus MellowGlkPhysicalOwner::requestReset(uint64_t epoch) {
    GateCall call {Op::Reset};
    call.epoch = epoch;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerStatus MellowGlkPhysicalOwner::waitRevocation(uint32_t timeoutUs) {
    // commandSleep must own the gate exactly once; nested callers would
    // sleep holding the outer acquisition.
    if (!timeoutUs || !workLoop_ || workLoop_->inGate()) return Status::Busy;
    GateCall call {Op::Wait};
    call.timeoutUs = timeoutUs;
    if (!enter(call)) return Status::Unavailable;
    finishPost(call);
    return call.status;
}

GlkOwnerSnapshot MellowGlkPhysicalOwner::snapshot() {
    GlkOwnerSnapshot out {};
    GateCall call {Op::Snapshot};
    call.snapshot = &out;
    if (enter(call)) finishPost(call);
    return out;
}

void MellowGlkPhysicalOwner::dispatch(GateCall &call) {
    switch (call.op) {
    case Op::Power:
        call.status = Status::Ok;
        if (call.ordinal == PowerOn) {
            powered_ = true;
            if (quarantine_ == GlkQuarantine::None && !terminating_ && !revoking_ &&
                !liveEpoch_ && providerOpen_) (void)publish();
            break;
        }
        powered_ = false;
        beginRevocation(GlkRevokePower);
        finishRevocation(call);
        if (revoking_) {
            // Holds outstanding: acknowledge when the last one is released,
            // or at the drain deadline with quarantine latched.
            powerAckPending_ = true;
            call.power = PowerAckBoundUs;
        }
        break;
    case Op::Acquire:
        if (!enterLease(call.epoch, call.status)) break;
        call.status = Status::Busy;
        for (uint32_t i = 0; i < MaxHolds; ++i) {
            if (holds_[i].active) continue;
            holds_[i] = {call.epoch, ++holdGeneration_, true};
            ++holdCount_;
            *call.hold = {call.epoch, holds_[i].generation, i};
            call.status = Status::Ok;
            break;
        }
        break;
    case Op::Release:
        call.status = gatedRelease(*call.hold, call);
        break;
    case Op::RunLeased:
        if (!enterLease(call.epoch, call.status)) break;
        ++activeActions_;
        call.status = call.action(call.context, binding(call.epoch));
        --activeActions_;
        // Recursive notifications revoke admission immediately, but cannot
        // quiesce or close while this native callback still owns the binding.
        if (revoking_ && !activeActions_) finishRevocation(call);
        closeIfRequested(call);
        break;
    case Op::RunSerialized:
        call.status = call.action(call.context, binding(0));
        closeIfRequested(call);
        break;
    case Op::Reset:
        if (!enterLease(call.epoch, call.status)) break;
        // Without a real reset producer a reset request has no effect.
        if (!(registeredProducers() & GlkProducerGpuReset)) {
            call.status = Status::MissingProducer;
            break;
        }
        beginRevocation(GlkRevokeReset);
        finishRevocation(call);
        call.status = Status::Ok;
        break;
    case Op::Wait: {
        AbsoluteTime deadline {};
        clock_interval_to_deadline(call.timeoutUs, kMicrosecondScale, &deadline);
        while (revoking_ && gate_->commandSleep(&revoking_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
        call.status = revoking_ ? Status::Timeout :
            quarantine_ != GlkQuarantine::None ? Status::Quarantined : Status::Ok;
        break;
    }
    case Op::Terminate:
        terminating_ = true;
        beginRevocation(GlkRevokeTerminate);
        finishRevocation(call);
        call.status = Status::Ok;
        break;
    case Op::CloseRequest:
        terminating_ = true;
        closeRequested_ = true;
        beginRevocation(GlkRevokeTerminate);
        finishRevocation(call);
        closeIfRequested(call);
        call.status = call.close ? Status::Ok : Status::Busy;
        break;
    case Op::Stop: {
        terminating_ = true;
        beginRevocation(GlkRevokeTerminate);
        finishRevocation(call);
        if (revoking_) {
            AbsoluteTime deadline {};
            clock_interval_to_deadline(DrainBudgetUs, kMicrosecondScale, &deadline);
            while (revoking_ && gate_->commandSleep(&revoking_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
            if (revoking_) expireDrain(call);
        }
        // PMstop already ran: no PM transition waits for an acknowledgement.
        powerAckPending_ = false;
        call.acknowledge = false;
        if (timerArmed_) { timer_->cancelTimeout(); timerArmed_ = false; }
        // IOKit closes the provider after stop() returns. Adapters that still
        // reference binding contexts keep this instance alive instead.
        if (dmaInUse()) latchQuarantine(GlkQuarantine::BindingsOutstanding);
        closeRequested_ = true;
        closeIfRequested(call);
        call.status = quarantine_ == GlkQuarantine::None ? Status::Ok : Status::Quarantined;
        break;
    }
    case Op::Snapshot: {
        auto &s = *call.snapshot;
        s.liveEpoch = liveEpoch_; s.revokedEpoch = revokedEpoch_; s.holds = holdCount_;
        s.revocation = revocation_; s.registered = registeredProducers();
        s.missing = static_cast<uint8_t>(GlkAllProducers & ~s.registered);
        s.quarantine = quarantine_; s.started = started_; s.powered = powered_;
        s.revoking = revoking_; s.terminating = terminating_; s.providerOpen = providerOpen_;
        s.closeRequested = closeRequested_; s.powerAckPending = powerAckPending_;
        s.busLeadByOwner = busLeadByOwner_;
        call.status = Status::Ok;
        break;
    }
    }
}

bool MellowGlkPhysicalOwner::identityHolds() const {
    const auto &p = GlkLenovo15IGL7;
    if (!device_ || !registryId_ || device_->getRegistryEntryID() != registryId_ ||
        device_->getFunctionNumber() != 0) return false;
    const uint32_t id = device_->configRead32(kIOPCIConfigVendorID);
    const uint32_t classRevision = device_->configRead32(kIOPCIConfigRevisionID);
    const uint32_t subsystem = device_->configRead32(kIOPCIConfigSubSystemVendorID);
    const uint8_t header = device_->configRead8(PciHeaderType);
    return id == (uint32_t(p.device) << 16 | p.vendor) &&
        (classRevision & 0xff) == p.revision && (classRevision >> 24) == p.baseClass &&
        subsystem == (uint32_t(p.subsystem) << 16 | p.subsystemVendor) &&
        !(header & PciHeaderLayoutMask);
}

bool MellowGlkPhysicalOwner::findPowerCapability() {
    // Read-only walk; the capability list is fixed for the device lifetime.
    pmCapability_ = 0;
    const uint16_t status = device_->configRead16(kIOPCIConfigStatus);
    if (status == UINT16_MAX || !(status & PciStatusCapList)) return false;
    uint8_t offset = device_->configRead8(PciCapabilityList) & 0xfc;
    for (unsigned ttl = 0; ttl < PciCapabilityTtl && offset >= 0x40; ++ttl) {
        const uint16_t header = device_->configRead16(offset);
        if ((header & 0xff) == PciCapIdPm) {
            pmCapability_ = offset;
            return true;
        }
        if ((header & 0xff) == 0xff) return false;
        offset = static_cast<uint8_t>(header >> 8) & 0xfc;
    }
    return false;
}

bool MellowGlkPhysicalOwner::publish() {
    if (!identityHolds()) {
        latchQuarantine(GlkQuarantine::IdentityChanged);
        return false;
    }
    if (!device_->isOpen(this) || device_->isInactive() || isInactive()) return false;
    // PM's delivery is the lease; PMCSR D0 and decode only corroborate it.
    const uint16_t pmcsr = device_->configRead16(static_cast<UInt8>(pmCapability_ + PciPmControl));
    if (pmcsr == UINT16_MAX || (pmcsr & PciPmStateMask)) return false;
    uint16_t command = device_->configRead16(kIOPCIConfigCommand);
    if (command == UINT16_MAX || !(command & kIOPCICommandMemorySpace)) return false;
    if (!(command & PciCommandBusLead)) {
        // i915_driver.c i915_driver_hw_probe: pci_set_master(). The return
        // value is not a success indication; the register is re-read.
        (void)setBusLead(device_, true, 0);
        busLeadByOwner_ = true;
        command = device_->configRead16(kIOPCIConfigCommand);
        if (command == UINT16_MAX || !(command & PciCommandBusLead)) return false;
    }
    if (!++epochCounter_) ++epochCounter_;
    liveEpoch_ = epochCounter_;
    revocation_ = 0;
    return true;
}

bool MellowGlkPhysicalOwner::leaseValid(uint64_t epoch, GlkOwnerStatus &status) const {
    if (quarantine_ != GlkQuarantine::None) status = Status::Quarantined;
    else if (terminating_) status = Status::Terminating;
    else if (revoking_) status = Status::Revoking;
    else if (!powered_ || !liveEpoch_) status = Status::PowerDown;
    else if (epoch != liveEpoch_) status = Status::StaleEpoch;
    else if (!providerOpen_ || !device_->isOpen(this) || device_->isInactive() || isInactive())
        status = Status::Ownership;
    else {
        status = Status::Ok;
        return true;
    }
    return false;
}

bool MellowGlkPhysicalOwner::enterLease(uint64_t epoch, GlkOwnerStatus &status) {
    if (!leaseValid(epoch, status)) return false;
    // A device that changed identity or left D0 behind PM's back is never
    // trusted again in this instance.
    if (!identityHolds()) {
        latchQuarantine(GlkQuarantine::IdentityChanged);
        status = Status::Quarantined;
        return false;
    }
    const uint16_t pmcsr = device_->configRead16(static_cast<UInt8>(pmCapability_ + PciPmControl));
    if (pmcsr == UINT16_MAX || (pmcsr & PciPmStateMask)) {
        latchQuarantine(GlkQuarantine::PowerStateMismatch);
        status = Status::Quarantined;
        return false;
    }
    return true;
}

bool MellowGlkPhysicalOwner::admits(IOPCIDevice *device, IOService *service, const DeviceIdentity &id) const {
    GlkOwnerStatus status;
    return workLoop_ && workLoop_->inGate() && device && device == device_ && service == this &&
        id.vendor == GlkLenovo15IGL7.vendor && id.device == GlkLenovo15IGL7.device &&
        id.registryId == registryId_ && leaseValid(id.epoch, status);
}

GlkPhysicalBinding MellowGlkPhysicalOwner::binding(uint64_t epoch) {
    const auto &p = GlkLenovo15IGL7;
    return {device_, this, workLoop_, gate_,
        {{p.vendor, p.device, registryId_, epoch}, p.revision, p.subsystemVendor, p.subsystem}};
}

void MellowGlkPhysicalOwner::beginRevocation(uint8_t cause) {
    revocation_ |= cause;
    if (!liveEpoch_) return;
    // Admission ends here, before any wait: every later check sees no epoch.
    revokedEpoch_ = liveEpoch_;
    liveEpoch_ = 0;
    revoking_ = true;
}

void MellowGlkPhysicalOwner::finishRevocation(GateCall &call) {
    if (!revoking_ || finishingRevocation_) return;
    if (holdCount_ || activeActions_) {
        if (!timerArmed_) {
            if (timer_ && timer_->setTimeoutUS(DrainBudgetUs) == kIOReturnSuccess) timerArmed_ = true;
            else expireDrain(call); // no bounded wait exists: never wait unbounded
        }
        return;
    }
    if (timerArmed_) { timer_->cancelTimeout(); timerArmed_ = false; }
    finishingRevocation_ = true; // IOCommandGate is recursive; callbacks may revoke again.
    bool stopped = true;
    // Every producer gets its stop attempt, last registered first.
    for (uint32_t i = clientCount_; i-- > 0;)
        if (!clients_[i].ops.quiesce(clients_[i].ops.context, revokedEpoch_)) stopped = false;
    // Power loss and termination supersede reset, including a reentrant delivery.
    const bool reset = (revocation_ & GlkRevokeReset) &&
        !(revocation_ & (GlkRevokePower | GlkRevokeTerminate));
    if (!stopped) {
        latchQuarantine(GlkQuarantine::ClientQuiesceUnknown);
    } else if (reset) {
        bool completed = false;
        for (uint32_t i = 0; i < clientCount_; ++i)
            if (clients_[i].kind == GlkProducerGpuReset)
                completed = clients_[i].ops.reset(clients_[i].ops.context, revokedEpoch_);
        if (!completed) latchQuarantine(GlkQuarantine::ResetUnproven);
    }
    if (quarantine_ == GlkQuarantine::None) {
        // Producers are epoch-bound and must register again in a new epoch.
        const uint32_t count = clientCount_;
        clientCount_ = 0;
        for (uint32_t i = 0; i < count; ++i) {
            OSObject *lifetime = clients_[i].ops.lifetime;
            clients_[i] = {};
            lifetime->release();
        }
    }
    finishingRevocation_ = false;
    revoking_ = false;
    if (powerAckPending_) {
        powerAckPending_ = false;
        call.acknowledge = true;
    }
    if (reset && quarantine_ == GlkQuarantine::None && powered_ && !terminating_) (void)publish();
    closeIfRequested(call);
    gate_->commandWakeup(&revoking_);
}

void MellowGlkPhysicalOwner::expireDrain(GateCall &call) {
    if (!revoking_) return;
    // Holds never drained: in-flight work is of unknown state. Producers are
    // not asked to stop under it; everything stays retained.
    latchQuarantine(GlkQuarantine::DrainTimeout);
    revoking_ = false;
    if (powerAckPending_) {
        powerAckPending_ = false;
        call.acknowledge = true;
    }
    gate_->commandWakeup(&revoking_);
}

void MellowGlkPhysicalOwner::drainExpired(OSObject *target, IOTimerEventSource *) {
    // Timer actions run on the work loop with the gate already closed.
    auto *self = OSDynamicCast(MellowGlkPhysicalOwner, target);
    if (!self) return;
    self->timerArmed_ = false;
    if (!self->revoking_ || (!self->holdCount_ && !self->activeActions_)) return;
    GateCall call {Op::Power};
    self->expireDrain(call);
    if (call.acknowledge) self->acknowledgeSetPowerState();
}

void MellowGlkPhysicalOwner::closeIfRequested(GateCall &call) {
    if (!closeRequested_ || revoking_ || quarantine_ != GlkQuarantine::None || !providerOpen_ ||
        liveEpoch_ || holdCount_ || activeActions_ || dmaInUse()) return;
    if (busLeadByOwner_) {
        // Restore only what this owner changed; firmware scanout may need it.
        (void)setBusLead(device_, false, 0);
        busLeadByOwner_ = false;
    }
    providerOpen_ = false;
    call.close = true;
}

void MellowGlkPhysicalOwner::latchQuarantine(GlkQuarantine reason) {
    if (quarantine_ != GlkQuarantine::None) return;
    quarantine_ = reason;
    if (liveEpoch_) {
        revokedEpoch_ = liveEpoch_;
        liveEpoch_ = 0;
    }
    // Deliberate: the instance, its kext and every binding context stay
    // resident so no adapter or in-flight work can reach freed state.
    if (!selfRetained_) {
        retain();
        selfRetained_ = true;
    }
}

GlkOwnerStatus MellowGlkPhysicalOwner::gatedRelease(GlkLeaseHold &hold, GateCall &call) {
    if (hold.slot >= MaxHolds) return Status::Invalid;
    auto &h = holds_[hold.slot];
    if (!h.active || h.generation != hold.generation || h.epoch != hold.epoch) return Status::Invalid;
    h = {};
    --holdCount_;
    hold = {};
    if (revoking_ && !holdCount_) finishRevocation(call);
    return Status::Ok;
}

uint8_t MellowGlkPhysicalOwner::registeredProducers() const {
    uint8_t kinds = 0;
    for (uint32_t i = 0; i < clientCount_; ++i) kinds |= clients_[i].kind;
    return kinds;
}

bool MellowGlkPhysicalOwner::dmaInUse() const {
    for (const auto &b : dma_) if (b.used) return true;
    return false;
}

GlkOwnerStatus MellowGlkPhysicalOwner::registerClient(uint64_t epoch, GlkProducer kind,
        const GlkClientOps &ops) {
    if (!workLoop_ || !workLoop_->inGate()) return Status::Ownership;
    GlkOwnerStatus status;
    if (!leaseValid(epoch, status)) return status;
    if (!kind || (kind & (kind - 1)) || (kind & ~GlkAllProducers) || !ops.context || !ops.quiesce || !ops.lifetime ||
        ((kind == GlkProducerGpuReset) != (ops.reset != nullptr))) return Status::Invalid;
    if ((registeredProducers() & kind) || clientCount_ >= MaxClients) return Status::Busy;
    ops.lifetime->retain();
    clients_[clientCount_++] = {kind, ops, epoch};
    return Status::Ok;
}

GlkOwnerStatus MellowGlkPhysicalOwner::attachDmaMemory(uint64_t epoch, GlkDmaMemoryIOKit &adapter,
        MemoryLimits limits, const GlkVmProducer &vm) {
    if (!workLoop_ || !workLoop_->inGate()) return Status::Ownership;
    GlkOwnerStatus status;
    if (!leaseValid(epoch, status)) return status;
    if (!vm.context || !vm.map || !vm.unmap || !vm.retired || !vm.quiesce || !vm.lifetime) return Status::MissingProducer;
    if (adapter.attached()) return Status::Busy;
    DmaBinding *slot = nullptr;
    for (auto &b : dma_) {
        if (b.used && b.adapter == &adapter) return Status::Busy;
        if (!b.used && !slot) slot = &b;
    }
    if (!slot) return Status::Busy;
    vm.lifetime->retain(); // binding outlives its epoch registration until detach
    *slot = {this, &adapter, vm, epoch, true};
    // The VM producer stops DMA for this epoch on every revocation.
    status = registerClient(epoch, GlkProducerGpuVm, {slot, vmClientQuiesce, nullptr, vm.lifetime});
    if (status != Status::Ok) {
        vm.lifetime->release();
        *slot = {};
        return status;
    }
    const GlkPhysicalVmOps ops {slot, vmAdmitted, vmMap, vmUnmap, vmRetired, vmQuiesce};
    const auto result = adapter.attach(device_, this, binding(epoch).identity, limits, ops);
    if (result == MemoryStatus::Ok) return Status::Ok;
    clients_[--clientCount_] = {};
    vm.lifetime->release(); // registration
    *slot = {};
    vm.lifetime->release(); // binding
    if (result == MemoryStatus::Invalid) return Status::Invalid;
    if (result == MemoryStatus::Ownership) return Status::Ownership;
    if (result == MemoryStatus::Busy) return Status::Busy;
    return Status::Unavailable;
}

GlkOwnerStatus MellowGlkPhysicalOwner::detachDmaMemory(GlkDmaMemoryIOKit &adapter) {
    if (!workLoop_ || !workLoop_->inGate()) return Status::Ownership;
    if (finishingRevocation_) return Status::Busy; // callback currently uses the binding
    for (auto &b : dma_) {
        if (!b.used || b.adapter != &adapter) continue;
        // Requires MemoryOwner close Ok first; retained pins keep it Busy.
        const auto result = adapter.detach();
        if (result == MemoryStatus::Busy) return Status::Busy;
        if (result != MemoryStatus::Ok) return Status::Quarantined;
        for (uint32_t i = 0; i < clientCount_; ++i) {
            if (clients_[i].ops.context != &b) continue;
            for (uint32_t j = i + 1; j < clientCount_; ++j) clients_[j - 1] = clients_[j];
            clients_[--clientCount_] = {};
            b.vm.lifetime->release(); // registration
            break;
        }
        OSObject *lifetime = b.vm.lifetime;
        b = {};
        lifetime->release(); // no callback can use the detached binding now
        return Status::Ok;
    }
    return Status::Invalid;
}

bool MellowGlkPhysicalOwner::vmAdmitted(void *context, IOPCIDevice *device, IOService *service,
        const DeviceIdentity &id) {
    auto *b = static_cast<DmaBinding *>(context);
    return b && b->used && b->owner && id.epoch == b->epoch && b->owner->admits(device, service, id);
}

MemoryStatus MellowGlkPhysicalOwner::vmMap(void *context, const DeviceIdentity &id, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, bool writable, GpuMapping &mapping) {
    auto *b = static_cast<DmaBinding *>(context);
    // Unavailable with an untouched mapping: nothing was attempted.
    if (!b || !b->used || id.epoch != b->epoch || !b->owner->admits(b->owner->device_, b->owner, id))
        return MemoryStatus::Unavailable;
    return b->vm.map(b->vm.context, id, owner, handle, pin, writable, mapping);
}

MemoryStatus MellowGlkPhysicalOwner::vmUnmap(void *context, const DeviceIdentity &id, uint64_t owner,
        const MemoryHandle &handle, const DmaPin &pin, GpuMapping &mapping) {
    auto *b = static_cast<DmaBinding *>(context);
    // Teardown after revocation stays possible, but only serialized.
    if (!b || !b->used || id.epoch != b->epoch || !b->owner->workLoop_->inGate())
        return MemoryStatus::Ownership;
    return b->vm.unmap(b->vm.context, id, owner, handle, pin, mapping);
}

bool MellowGlkPhysicalOwner::vmRetired(void *context, const MemoryView &view) {
    auto *b = static_cast<DmaBinding *>(context);
    return b && b->used && view.device.epoch == b->epoch &&
        b->owner->admits(b->owner->device_, b->owner, view.device) &&
        b->vm.retired(b->vm.context, view);
}

bool MellowGlkPhysicalOwner::vmQuiesce(void *context, const DeviceIdentity &id) {
    auto *b = static_cast<DmaBinding *>(context);
    return b && b->used && id.epoch == b->epoch && b->owner->workLoop_->inGate() &&
        b->vm.quiesce(b->vm.context, id);
}

bool MellowGlkPhysicalOwner::vmClientQuiesce(void *context, uint64_t epoch) {
    auto *b = static_cast<DmaBinding *>(context);
    if (!b || !b->used || epoch != b->epoch) return false;
    return b->vm.quiesce(b->vm.context, b->owner->binding(epoch).identity.device);
}

} // namespace MellowNative

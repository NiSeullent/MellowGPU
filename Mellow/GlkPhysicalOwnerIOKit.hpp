// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "GlkDmaMemoryIOKit.hpp"
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>

#if !defined(__x86_64__) && !defined(MELLOW_GLK_OWNER_HOSTED)
#error "The GLK physical owner is an x86_64 macOS kernel component"
#endif

namespace MellowNative {

// The single admitted physical profile: user-designated Lenovo 15IGL7,
// Intel N4120 Gemini Lake IGD. torvalds/linux@0d9ff90a pciids.h lists 0x3185
// in INTEL_GLK_IDS and INTEL_VGA_DEVICE matches display base class 0x03;
// i915_pci.c i915_pci_probe binds PCI function 0 only. Revision and
// subsystem pin the board. The registry entry ID is bound at start and is
// valid only until reboot, so it is never a compile-time constant.
struct GlkPhysicalProfile {
    uint16_t vendor, device;
    uint8_t revision, baseClass;
    uint16_t subsystemVendor, subsystem;
};
constexpr GlkPhysicalProfile GlkLenovo15IGL7 {0x8086, 0x3185, 0x06, 0x03, 0x17aa, 0x3e77};
// Darwin 24 is macOS 15 (target 15.8.1, 24H32). Native contracts were read
// from SDK 15.5 and xnu-11417.140.69 (43a90889); other kernels are refused.
constexpr int GlkKernelMajor = 24;

enum class GlkOwnerStatus : uint8_t {
    Ok, Invalid, Unavailable, Ownership, StaleEpoch, PowerDown, Revoking,
    Terminating, Quarantined, Busy, MissingProducer, Timeout
};
// Revocation causes; several may accumulate in one serialized revocation.
enum GlkRevocation : uint8_t {
    GlkRevokePower = 1, GlkRevokeReset = 2, GlkRevokeTerminate = 4
};
// Latched, terminal for this instance: no epoch is published again, the
// provider stays open, the instance retains itself, nothing is reclaimed.
enum class GlkQuarantine : uint8_t {
    None, IdentityChanged, PowerStateMismatch, DrainTimeout,
    ClientQuiesceUnknown, ResetUnproven, BindingsOutstanding
};
// Producers this owner cannot supply. Each is registered only by its real
// implementation, for one live epoch, and is dropped when that epoch ends.
enum GlkProducer : uint8_t {
    GlkProducerGpuVm = 1,      // GGTT/PPGTT publication, retirement, DMA stop
    GlkProducerForcewake = 2,  // Gen9 forcewake before any GT register access
    GlkProducerInterrupt = 4,  // interrupt source disable and drain
    GlkProducerGpuReset = 8,   // engine/GT reset with completion proof
};
constexpr uint8_t GlkAllProducers = 0x0f;

struct GlkClientOps {
    void *context {};
    // Gated, after every hold of the epoch drained and before power, reset
    // or close proceeds. True only when this producer's hardware activity
    // for that epoch is proven stopped. Any false latches quarantine.
    bool (*quiesce)(void *, uint64_t epoch) {};
    // GlkProducerGpuReset only, after every quiesce returned true: perform
    // the reset; true only with completion proof.
    bool (*reset)(void *, uint64_t epoch) {};
    // Strong lifetime anchor owned by this registration until quiesce finishes.
    // Its owner must own context and all callback state; never a user pointer.
    OSObject *lifetime {};
};

// GPU VM producer for the seeded GlkDmaMemoryIOKit: GlkPhysicalVmOps
// without admission, which only this owner supplies.
struct GlkVmProducer {
    void *context {};
    MemoryStatus (*map)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, bool writable, GpuMapping &) {};
    MemoryStatus (*unmap)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, GpuMapping &) {};
    bool (*retired)(void *, const MemoryView &) {};
    bool (*quiesce)(void *, const DeviceIdentity &) {};
    // Owns context through adapter detach, independently of epoch registration.
    OSObject *lifetime {};
};

// Valid only inside the gated action that received it. identity.device.epoch
// is the live epoch for runLeased and zero (no admission) for runSerialized.
struct GlkPhysicalBinding {
    IOPCIDevice *device {};
    IOService *owner {};
    IOWorkLoop *workLoop {};
    IOCommandGate *gate {};
    GlkDmaIdentity identity {};
};
using GlkGatedAction = GlkOwnerStatus (*)(void *, const GlkPhysicalBinding &);

// An outstanding reference to one epoch, e.g. submitted GPU work. Holds
// block revocation completion; only releaseHold() ends one.
struct GlkLeaseHold {
    uint64_t epoch {}, generation {};
    uint32_t slot {UINT32_MAX};
};

struct GlkOwnerSnapshot {
    uint64_t liveEpoch {}, revokedEpoch {};
    uint32_t holds {};
    uint8_t revocation {}, registered {}, missing {GlkAllProducers};
    GlkQuarantine quarantine {GlkQuarantine::None};
    bool started {}, powered {}, revoking {}, terminating {}, providerOpen {};
    bool closeRequested {}, powerAckPending {}, busLeadByOwner {};
};

// IOService client of the GLK IOPCIDevice that owns its native PCI lifetime.
//
// Ownership: start() requires IOPCIDevice::open(this) before any config
// read, then the full tuple, function 0, a type-0 header and the PCI PM
// capability; failure closes and refuses. Power: PMinit, joinPMtree on the
// provider and registerPowerDriver with an Off/On table make this object a
// power-plane child. An epoch is published only from PM's setPowerState(On)
// delivery, with re-verified identity, PMCSR D0 and memory decode. The
// provider's getPowerState() is not used: SDK 15.5 IOService.h documents
// that a parent reports Off while its children power on during wake.
//
// Serialization: every state change runs in this object's IOCommandGate on
// its own IOWorkLoop; the gate is the shared sleepable lock the seeded DMA
// adapter requires, and admission demands IOWorkLoop::inGate(). Revocation
// (setPowerState(Off), reset, provider termination, own stop) zeroes the live
// epoch at once, waits for holds, then calls each registered producer's
// quiesce in reverse order. setPowerState(Off) with holds returns a bounded
// time and acknowledges later; at DrainBudgetUs the owner latches
// DrainTimeout. Release macOS treats a tardy third-party setPowerState as
// acked after logging (xnu IOServicePM.cpp ackTimerTick), so the late
// acknowledgement only ends PM's wait; quarantine records the outcome.
// Termination: the provider stays open until revocation completes cleanly,
// which natively holds back stop(); quarantine never closes it.
//
// Effects: the only configuration write is IOPCIDevice::setBusLeadEnable
// (enable when firmware left it clear, restored only if this object set it).
// No BAR mapping, MMIO, interrupt, DMA, reset or system/IOMMU policy change.
// No personality references this class; nothing instantiates it here.
class MellowGlkPhysicalOwner : public IOService {
    OSDeclareDefaultStructors(MellowGlkPhysicalOwner);
public:
    static constexpr unsigned long PowerOff = 0, PowerOn = 1, PowerStateCount = 2;
    static constexpr uint32_t MaxHolds = 16, MaxClients = 4, MaxDmaBindings = 2;
    // PM is given margin over the owner's own drain deadline; XNU clamps
    // driver-requested time at kMaxTimeRequested (30 s).
    static constexpr uint32_t DrainBudgetUs = 1000000, PowerAckBoundUs = 2000000;

    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOWorkLoop *getWorkLoop() const override;
    IOReturn setPowerState(unsigned long ordinal, IOService *device) override;
    IOReturn message(UInt32 type, IOService *provider, void *argument = nullptr) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) override;

    // Sleepable thread context only: never interrupt, filter or PM context.
    GlkOwnerStatus acquireHold(uint64_t epoch, GlkLeaseHold &);
    GlkOwnerStatus releaseHold(GlkLeaseHold &);
    // Runs action inside the gate only while epoch is the live epoch.
    // Reentrant revocation closes admission, but native quiesce/close waits
    // for every admitted action to return (also for recursive actions).
    GlkOwnerStatus runLeased(uint64_t epoch, GlkGatedAction, void *context);
    // Runs action inside the gate without admission, for teardown.
    GlkOwnerStatus runSerialized(GlkGatedAction, void *context);
    // Refused without effect unless a GpuReset producer is registered.
    // Registered callback/VM anchors are retained; producer context and the
    // plain C++ DMA adapter must remain owned by that anchor until detach.
    GlkOwnerStatus requestReset(uint64_t epoch);
    GlkOwnerStatus waitRevocation(uint32_t timeoutUs);
    GlkOwnerSnapshot snapshot();

    // Gated calls (inside runLeased or runSerialized as stated).
    GlkOwnerStatus registerClient(uint64_t epoch, GlkProducer, const GlkClientOps &);  // runLeased
    GlkOwnerStatus attachDmaMemory(uint64_t epoch, GlkDmaMemoryIOKit &, MemoryLimits,
                                   const GlkVmProducer &);                             // runLeased
    GlkOwnerStatus detachDmaMemory(GlkDmaMemoryIOKit &);                              // either

private:
    struct Hold { uint64_t epoch, generation; bool active; };
    struct Client { GlkProducer kind; GlkClientOps ops; uint64_t epoch; };
    struct DmaBinding {
        MellowGlkPhysicalOwner *owner;
        GlkDmaMemoryIOKit *adapter;
        GlkVmProducer vm;
        uint64_t epoch;
        bool used;
    };
    struct GateCall;

    IOPCIDevice *device_ {};
    IOWorkLoop *workLoop_ {};
    IOCommandGate *gate_ {};
    IOTimerEventSource *timer_ {};
    IOPMPowerState powerStates_[PowerStateCount] {};
    Hold holds_[MaxHolds] {};
    Client clients_[MaxClients] {};
    DmaBinding dma_[MaxDmaBindings] {};
    uint64_t registryId_ {}, liveEpoch_ {}, revokedEpoch_ {}, epochCounter_ {}, holdGeneration_ {};
    uint32_t holdCount_ {}, clientCount_ {}, activeActions_ {};
    uint8_t pmCapability_ {}, revocation_ {};
    GlkQuarantine quarantine_ {GlkQuarantine::None};
    bool started_ {}, pmInitialized_ {}, sourcesAdded_ {}, providerOpen_ {}, powered_ {};
    bool revoking_ {}, terminating_ {}, closeRequested_ {}, powerAckPending_ {};
    bool timerArmed_ {}, busLeadByOwner_ {}, selfRetained_ {}, finishingRevocation_ {};

    static IOReturn gated(OSObject *, void *, void *, void *, void *);
    static void drainExpired(OSObject *, IOTimerEventSource *);
    bool enter(GateCall &);
    void dispatch(GateCall &);
    void finishPost(GateCall &);
    void teardownSources();
    bool identityHolds() const;
    bool findPowerCapability();
    bool publish();
    bool leaseValid(uint64_t epoch, GlkOwnerStatus &) const;
    bool enterLease(uint64_t epoch, GlkOwnerStatus &);
    bool admits(IOPCIDevice *, IOService *, const DeviceIdentity &) const;
    void beginRevocation(uint8_t cause);
    void finishRevocation(GateCall &);
    void expireDrain(GateCall &);
    void closeIfRequested(GateCall &);
    void latchQuarantine(GlkQuarantine);
    GlkPhysicalBinding binding(uint64_t epoch);
    GlkOwnerStatus gatedRelease(GlkLeaseHold &, GateCall &);
    uint8_t registeredProducers() const;
    bool dmaInUse() const;
    static bool vmAdmitted(void *, IOPCIDevice *, IOService *, const DeviceIdentity &);
    static MemoryStatus vmMap(void *, const DeviceIdentity &, uint64_t,
        const MemoryHandle &, const DmaPin &, bool, GpuMapping &);
    static MemoryStatus vmUnmap(void *, const DeviceIdentity &, uint64_t,
        const MemoryHandle &, const DmaPin &, GpuMapping &);
    static bool vmRetired(void *, const MemoryView &);
    static bool vmQuiesce(void *, const DeviceIdentity &);
    static bool vmClientQuiesce(void *, uint64_t);
};

} // namespace MellowNative

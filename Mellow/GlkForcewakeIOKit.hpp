// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// The Gen9 protocol it drives is MIT (Drivers/NativeGlk/LICENSE.MIT).
#pragma once
#include "../Drivers/NativeGlk/Forcewake.hpp"
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNativeGlk {
// Real x86-64 IOKit mapped-BAR forcewake adapter for one Gemini Lake GPU.
// Authority contract: the provider driver has already opened this exact
// IOPCIDevice, keeps it in D0, and holds its sleepable power/reset lock across
// EVERY call, including detach. ownerEpoch is the owner's current reset/power
// epoch; any change from the attach epoch refuses access and requires a fresh
// attach. attach() retains both provider and owner until explicit detach(),
// maps BAR0 but performs no MMIO. Each operation re-samples
// identity, PM state and ownership before AND after its register accesses,
// and MMIO is limited to the six Gen9 forcewake offsets inside that window.
// No component starts this automatically; no qualification is implied.
class IOKitForcewake {
public:
    IOKitForcewake() = default;
    IOKitForcewake(const IOKitForcewake &) = delete;
    IOKitForcewake &operator=(const IOKitForcewake &) = delete;
    // Releases IOKit objects without MMIO if detach() was not called.
    ~IOKitForcewake() { abandon(); }

    ForcewakeStatus attach(IOPCIDevice *, IOService *owner, const GlkPciExpectation &);
    // Writes request registers; see Gen9Forcewake::initialize().
    ForcewakeStatus initialize(uint64_t ownerEpoch, uint64_t timeoutUs);
    ForcewakeStatus get(uint64_t ownerEpoch, uint8_t domains, uint64_t timeoutUs, ForcewakeLease &);
    ForcewakeStatus put(uint64_t ownerEpoch, ForcewakeLease &);
    ForcewakeStatus reset(uint64_t ownerEpoch, uint8_t &lostDomains);
    // intel_uncore_fini_mmio(): resets held domains when authority still
    // validates, then unmaps. Without authority it abandons without MMIO.
    void detach(uint64_t ownerEpoch);
    const Gen9Forcewake &engine() const { return engine_; }

private:
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMemoryDescriptor *bar0_ {};
    IOMemoryMap *map_ {};
    volatile uint32_t *base_ {};
    uint64_t length_ {};
    GlkPciExpectation expected_ {};
    Gen9Forcewake engine_ {};
    bool window_ {};

    bool snapshot(uint64_t ownerEpoch, GlkAuthoritySnapshot &) const;
    bool findBar0(const GlkAuthoritySnapshot &, uint64_t physical);
    ForcewakeStatus open(uint64_t ownerEpoch);
    ForcewakeStatus close(uint64_t ownerEpoch, ForcewakeStatus result);
    ForcewakeStatus deadline(uint64_t timeoutUs, uint64_t &deadlineUs) const;
    void abandon();
    static bool read32(void *, uint32_t, uint32_t &);
    static bool write32(void *, uint32_t, uint32_t);
    static bool nowUs(void *, uint64_t &);
    static void delayUs(void *, uint32_t);
    static void relax(void *);
};
} // namespace MellowNativeGlk

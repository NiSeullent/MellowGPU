// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// OpRegion/VBT rules are re-expressed from Linux i915 (MIT); notices, blobs and
// symbols: Drivers/NativeGlk/OpRegionSnapshot.hpp and its provenance record.
#pragma once
#include "GlkPhysicalOwnerIOKit.hpp"
#include "../Drivers/NativeGlk/OpRegionSnapshot.hpp"
#include <IOKit/IOMemoryDescriptor.h>

namespace MellowNative {

// Exactly one byte load per call from a read-only firmware mapping, never a
// memcpy: the region is shared with ACPI AML and firmware. The hosted
// regression build routes loads to an injected model instead.
#if defined(MELLOW_GLK_OPREGION_HOSTED_IO)
uint8_t glkFirmwareLoad8(const volatile uint8_t *);
#elif defined(__x86_64__)
inline uint8_t glkFirmwareLoad8(const volatile uint8_t *at) { return *at; }
#else
#error "The GLK OpRegion snapshot is an x86_64 macOS kernel component"
#endif

enum class GlkOpRegionStatus : uint8_t {
    Ok,
    Invalid,      // argument misuse, or copy capacity below the VBT size
    Owner,        // the physical owner refused the lease (see GlkOpRegionInfo::owner)
    Ownership,    // the binding the owner delivered failed this adapter's checks
    Firmware,     // the OpRegion/VBT was refused (see GlkOpRegionInfo::firmware)
    Changed,      // a later capture saw different VBT bytes; nothing is published
    NotCaptured,
    StaleEpoch,   // the published bytes belong to another owner epoch
    Busy,         // re-entered from inside a VBT consumer
    Quarantined,  // a firmware unmap failed; its objects are retained forever
};

// Read-only configuration observations of the bound IGD function, all taken
// inside one leased capture and re-read before publication.
struct GlkIgdObservation {
    uint32_t asls {};        // pci_config.h ASLS 0xfc
    uint16_t ggc {};         // i915_drm.h SNB_GMCH_CTRL 0x50
    uint8_t ggms {}, gms {}; // BDW_GMCH_GGMS (7:6) and BDW_GMCH_GMS (15:8), raw
    uint32_t gsmBytes {};    // intel_ggtt.c gen8_get_total_gtt_size; 0 = none
    uint32_t bdsm {};        // i915_drm.h INTEL_BSM 0x5c, raw
    uint32_t bdsmBase {};    // bdsm & INTEL_BSM_MASK
};

// Metadata only: no kernel pointer and no firmware bytes.
struct GlkOpRegionInfo {
    GlkIgdObservation igd {};
    Mellow::NativeGlk::OpRegionHeader header {};
    Mellow::NativeGlk::VbtSource source {Mellow::NativeGlk::VbtSource::None};
    Mellow::NativeGlk::VbtStatus rvdaVbt {Mellow::NativeGlk::VbtStatus::NotTried};
    Mellow::NativeGlk::VbtStatus mailboxVbt {Mellow::NativeGlk::VbtStatus::NotTried};
    Mellow::NativeGlk::OpRegionStatus firmware {Mellow::NativeGlk::OpRegionStatus::Ok};
    GlkOpRegionStatus last {GlkOpRegionStatus::NotCaptured};
    uint64_t epoch {}, registryId {}, digest {}, generation {};
    uint32_t vbtBytes {}, captures {};
    bool published {}, quarantined {};
};

// Runs inside the owner's gate during withVbt; the view ends with the call
// and must not be retained, published or handed to user space.
using GlkVbtConsumer = void (*)(void *, const uint8_t *vbt, uint32_t bytes, const GlkOpRegionInfo &);

// Read-only snapshot of the GLK IGD OpRegion and its VBT.
//
// Every operation runs as a gated action of the frozen MellowGlkPhysicalOwner:
// capture, withVbt and copyVbt through runLeased(epoch), so only the live PM
// epoch of the exact 8086:3185 rev 06 17aa:3e77 function is admitted, and the
// delivered binding is checked again here (owner class and instance, provider,
// work loop and gate, open state, registry ID, epoch and live configuration
// identity). discard and info run serialized. There is no caller-supplied
// ownership flag and no caller-supplied physical address: ASLS is read from
// the bound function, RVDA only from the validated OpRegion header.
//
// Mappings: IOMemoryDescriptor::withAddressRange over the physical range with
// kIODirectionOut | kIOMemoryMapperNone (no IOMMU mapping), mapped in
// kernel_task with kIOMapReadOnly and kIOMapDefaultCache (XNU chooses the
// platform cache mode for the physical page), verified by length, physical
// address, page offset and read-only option, and unmapped before capture
// returns. A failed unmap latches quarantine and keeps both objects forever.
// Only an owned IOMallocData copy of exactly vbt_size bytes outlives a capture.
//
// Effects: configuration reads only. No configuration write, BAR mapping,
// MMIO, ASLE/SWSCI handshake, ACPI method, display or panel action. Sleepable
// thread context only, outside the owner's gate.
class GlkOpRegionIOKit {
public:
    GlkOpRegionIOKit() = default;
    ~GlkOpRegionIOKit();
    GlkOpRegionIOKit(const GlkOpRegionIOKit &) = delete;
    GlkOpRegionIOKit &operator=(const GlkOpRegionIOKit &) = delete;

    // Owner refuses the lease: Owner, with the owner's status in *owner.
    GlkOpRegionStatus capture(MellowGlkPhysicalOwner &, uint64_t epoch, GlkOwnerStatus *owner = nullptr);
    GlkOpRegionStatus withVbt(MellowGlkPhysicalOwner &, uint64_t epoch, GlkVbtConsumer, void *context,
                              GlkOwnerStatus *owner = nullptr);
    // All or nothing: capacity below the VBT size copies nothing.
    GlkOpRegionStatus copyVbt(MellowGlkPhysicalOwner &, uint64_t epoch, uint8_t *out,
                              uint32_t capacity, uint32_t &bytes, GlkOwnerStatus *owner = nullptr);
    GlkOpRegionStatus discard(MellowGlkPhysicalOwner &, GlkOwnerStatus *owner = nullptr);
    GlkOpRegionStatus info(MellowGlkPhysicalOwner &, GlkOpRegionInfo &, GlkOwnerStatus *owner = nullptr);

private:
    struct Window {
        IOMemoryDescriptor *descriptor {};
        IOMemoryMap *map {};
        const volatile uint8_t *base {};
        uint32_t bytes {};
    };
    struct Call;

    // All state below changes only inside the owner's gate.
    MellowGlkPhysicalOwner *owner_ {};  // identity only, never dereferenced outside a call
    uint64_t registryId_ {};
    uint8_t *vbt_ {};
    uint32_t vbtBytes_ {};
    GlkOpRegionInfo info_ {};
    Window rvda_ {};
    Window retained_[2] {};             // failed unmaps: never released
    IOPCIDevice *device_ {};            // valid only inside the current gated capture
    bool inConsumer_ {}, quarantined_ {};

    GlkOpRegionStatus run(MellowGlkPhysicalOwner &, Call &, bool leased, GlkOwnerStatus *owner);
    static GlkOwnerStatus gated(void *, const GlkPhysicalBinding &);
    bool bindingHolds(const Call &, const GlkPhysicalBinding &) const;
    static bool identityHolds(IOPCIDevice *);
    GlkOpRegionStatus captureGated(const GlkPhysicalBinding &);
    static bool observe(IOPCIDevice *, GlkIgdObservation &);
    static bool mapFirmware(uint64_t physical, uint32_t bytes, Window &);
    bool unmapFirmware(Window &);
    GlkOpRegionStatus commit(GlkOpRegionInfo &, GlkOpRegionStatus);
    void unpublish();
    static bool copyWindow(void *, uint32_t offset, uint8_t *out, uint32_t bytes);
    static bool mapRvda(void *, uint64_t physical, uint32_t bytes, Mellow::NativeGlk::FirmwareWindow &);
    static bool unmapRvda(void *);
    static uint8_t *allocate(void *, uint32_t bytes);
    static void release(void *, uint8_t *, uint32_t bytes);
    static bool observeAsls(void *, uint32_t &);
};

} // namespace MellowNative

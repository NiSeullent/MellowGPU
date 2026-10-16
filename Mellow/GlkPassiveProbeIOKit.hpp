// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNative {

// The single user-designated first physical target: Lenovo 15IGL7, Celeron
// N4120, Gemini Lake UHD 600 at 00:02.0, native x86_64 macOS 15.8.1 (Darwin 24,
// i915/execlists-class hardware; no Xe/GuC). Every field is required. Neither a
// near match (0x3184, another revision, another subsystem or BDF) nor a match
// on registry properties alone is admitted.
struct GlkPassiveTarget {
    static constexpr uint16_t Vendor = 0x8086, Device = 0x3185;
    static constexpr uint8_t Revision = 0x06;
    static constexpr uint16_t SubsystemVendor = 0x17aa, Subsystem = 0x3e77;
    static constexpr uint32_t ClassCode = 0x030000; // VGA-compatible display, prog-if 0.
    static constexpr uint8_t Bus = 0, Slot = 2, Function = 0;
    static constexpr int DarwinMajor = 24;
};

// IOMapper::gSystem as defined by XNU 43a90889 iokit/IOKit/IOMapper.h:66-71
// and iokit/Kernel/IOMapper.cpp:64,120,144-152: Unknown (1) at boot,
// HasMapper (2) when the platform says a mapper will start, NoMapper (null)
// when it says none will, otherwise the started system mapper. Values with a
// low bit set other than 1 and 2 are not defined there and stay Unrecognized.
// Classification uses only the loaded value; the pointer is never followed.
enum class GlkSystemMapperState : uint8_t { Unrecognized, NoMapper, Unknown, HasMapper, Live };
// Type of a provider property. A mapper object is split by its own liveness.
enum class GlkPropertyKind : uint8_t { Absent, MapperActive, MapperInactive, Number, Data, Other };
enum class GlkPmCapability : uint8_t { NoCapabilityList, NotFound, Malformed, Found };
enum class GlkProbeVerdict : uint8_t {
    Target, NotPci, Inactive, WrongKernel, RegistryMismatch, WrongLocation,
    ConfigUnreadable, ConfigMismatch, OpenedBySelf, Unstable
};

struct GlkPciIdentity {
    uint16_t vendor {}, device {};
    uint8_t revision {};
    uint32_t classCode {};
    uint16_t subsystemVendor {}, subsystem {};
};

// One bounded, read-only sample. No field holds or encodes a kernel address.
struct GlkPassiveSnapshot {
    uint64_t providerRegistryId {};
    GlkPciIdentity registry {}, config {};
    bool registryComplete {};
    uint8_t bus {}, slot {}, function {}, headerType {};
    uint16_t command {}, status {};
    GlkPmCapability pm {GlkPmCapability::NoCapabilityList};
    uint8_t pmOffset {}, capabilitiesVisited {};
    uint16_t pmc {}, pmcsr {};
    GlkSystemMapperState systemMapper {GlkSystemMapperState::Unrecognized};
    GlkPropertyKind iommuParent {GlkPropertyKind::Absent};
    uint32_t iommuParentDataBytes {};
    GlkPropertyKind descriptorOptions {GlkPropertyKind::Absent};
    uint64_t descriptorOptionsValue {};
    uint32_t descriptorOptionsBits {};
    bool providerInactive {}, providerOpen {}, providerOpenBySelf {}, clientsTruncated {};
    uint32_t providerBusy {}, clients {}, framebufferClients {};
    int darwinMajor {};
};

// Bounded passive sampler shared by probe and start; exposed for regression.
// Reads registry properties, identity/command/status/capability configuration
// dwords and PMCSR, provider state and the gSystem word. It writes nothing,
// opens nothing, joins no power tree, never waits for or resolves a mapper and
// maps no BAR. A Target verdict only means this device is the designated one.
GlkProbeVerdict glkPassiveSample(IOService *provider, const IOService *self,
                                 GlkPassiveSnapshot &out);
bool glkSameDevice(const GlkPassiveSnapshot &, const GlkPassiveSnapshot &);
bool glkSameObservation(const GlkPassiveSnapshot &, const GlkPassiveSnapshot &);
const char *glkVerdictName(GlkProbeVerdict);
const char *glkSystemMapperName(GlkSystemMapperState);
const char *glkPropertyKindName(GlkPropertyKind);
const char *glkPmCapabilityName(GlkPmCapability);

} // namespace MellowNative

// Passive native snapshot service for the designated GLK GPU, matched under
// its own IOMatchCategory so the firmware framebuffer keeps its provider. The
// published properties are observations, NOT a durable power, reset, DMA or
// mapper lease, and NOT GPU, Metal or WindowServer support. It is no
// accelerator and owns no device state: IOKit serializes probe/start/stop/free
// for one instance and nothing else (no timer, interrupt, work loop, user
// client, notifier or power-tree membership) can run concurrently, so there is
// no lock. Nothing retained outlives start, so stop and free release nothing.
class MellowGlkPassiveProbe : public IOService {
    OSDeclareDefaultStructors(MellowGlkPassiveProbe)
public:
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
private:
    MellowNative::GlkPassiveSnapshot probed_ {};
    bool probedTarget_ {}, published_ {};
    bool publish(const MellowNative::GlkPassiveSnapshot &, bool matchesProbe);
};

// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "GlkPassiveProbeIOKit.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/version.h>

#if !defined(__x86_64__) && !defined(MELLOW_GLK_PASSIVE_HOSTED)
#error "MellowGlkPassiveProbe is defined only for the native x86_64 GLK target"
#endif

OSDefineMetaClassAndStructors(MellowGlkPassiveProbe, IOService)
#define super IOService

namespace MellowNative {
namespace {
// Type-0 configuration header and PCI PM capability layout (PCI Local Bus
// 3.0 / PCI PM 1.2), the same values Linux torvalds/linux@0d9ff90a uses in
// include/uapi/linux/pci_regs.h. Every access is an aligned read below 0x100.
constexpr UInt8 ConfigId = 0x00, ConfigCommandStatus = 0x04, ConfigClassRevision = 0x08,
    ConfigHeaderDword = 0x0c, ConfigSubsystem = 0x2c, ConfigCapabilityPointer = 0x34;
constexpr uint16_t StatusCapabilityList = 0x0010;
constexpr uint8_t CapabilityPm = 0x01, PmCapabilities = 2, PmControlStatus = 4;
// drivers/pci/pci.h PCI_FIND_CAP_TTL and PCI_STD_HEADER_SIZEOF: at most 48
// list entries, none inside the 64-byte standard header. A PM structure is 8
// bytes, so its offset may not exceed 0xf8.
constexpr unsigned CapabilityTtl = 48;
constexpr uint8_t CapabilityFloor = 0x40, PmCeiling = 0xf8;
constexpr uint32_t MaxClients = 32;
constexpr const char *ObservationKey = "MellowGlkPassiveObservation";

bool registryWord(IOService *provider, const char *key, uint32_t limit, uint32_t &value) {
    OSObject *object = provider->copyProperty(key);
    auto *data = OSDynamicCast(OSData, object);
    // IOPCIFamily IOPCIBridge::checkProperties reads these as one uint32.
    bool ok = data && data->getLength() == sizeof(uint32_t);
    if (ok) {
        const auto *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
        ok = bytes != nullptr;
        if (ok) value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
            uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
        ok = ok && value <= limit;
    }
    if (object) object->release();
    return ok;
}
bool isTarget(const GlkPciIdentity &id) {
    return id.vendor == GlkPassiveTarget::Vendor && id.device == GlkPassiveTarget::Device &&
        id.revision == GlkPassiveTarget::Revision && id.classCode == GlkPassiveTarget::ClassCode &&
        id.subsystemVendor == GlkPassiveTarget::SubsystemVendor &&
        id.subsystem == GlkPassiveTarget::Subsystem;
}
bool sameIdentity(const GlkPciIdentity &a, const GlkPciIdentity &b) {
    return a.vendor == b.vendor && a.device == b.device && a.revision == b.revision &&
        a.classCode == b.classCode && a.subsystemVendor == b.subsystemVendor &&
        a.subsystem == b.subsystem;
}
GlkSystemMapperState classifySystemMapper(uintptr_t word) {
    if (!word) return GlkSystemMapperState::NoMapper;
    if (word == 1) return GlkSystemMapperState::Unknown;
    if (word == 2) return GlkSystemMapperState::HasMapper;
    // kWaitMask is 3: any other value with those bits is not a mapper.
    if (word & 3) return GlkSystemMapperState::Unrecognized;
    return GlkSystemMapperState::Live;
}
GlkPropertyKind propertyKind(IOService *provider, const char *key, uint32_t &dataBytes,
                             uint64_t &value, uint32_t &bits) {
    dataBytes = 0; value = 0; bits = 0;
    OSObject *object = provider->copyProperty(key);
    if (!object) return GlkPropertyKind::Absent;
    auto kind = GlkPropertyKind::Other;
    if (auto *mapper = OSDynamicCast(IOMapper, object)) {
        kind = mapper->isInactive() ? GlkPropertyKind::MapperInactive : GlkPropertyKind::MapperActive;
    } else if (auto *number = OSDynamicCast(OSNumber, object)) {
        kind = GlkPropertyKind::Number;
        value = number->unsigned64BitValue(); bits = number->numberOfBits();
    } else if (auto *data = OSDynamicCast(OSData, object)) {
        kind = GlkPropertyKind::Data;
        dataBytes = data->getLength();
    }
    object->release();
    return kind;
}
// Bounded walk with the Linux list rules plus loop rejection: an entry seen
// twice, a nonzero pointer into the standard header or an all-ones ID is
// Malformed instead of silently ending the list.
GlkPmCapability findPm(IOPCIDevice *pci, uint16_t status, uint8_t &offset, uint8_t &visited) {
    offset = 0; visited = 0;
    if (!(status & StatusCapabilityList)) return GlkPmCapability::NoCapabilityList;
    uint8_t position = static_cast<uint8_t>(pci->configRead32(ConfigCapabilityPointer));
    uint64_t seen = 0;
    for (unsigned ttl = CapabilityTtl; ttl; --ttl) {
        if (!position) return GlkPmCapability::NotFound;
        if (position < CapabilityFloor) return GlkPmCapability::Malformed;
        position &= 0xfc;
        const uint64_t bit = 1ULL << ((position - CapabilityFloor) / 4);
        if (seen & bit) return GlkPmCapability::Malformed;
        seen |= bit; ++visited;
        const uint16_t header = pci->configRead16(position);
        const uint8_t id = static_cast<uint8_t>(header);
        if (id == 0xff) return GlkPmCapability::Malformed;
        if (id == CapabilityPm) {
            if (position > PmCeiling) return GlkPmCapability::Malformed;
            offset = position;
            return GlkPmCapability::Found;
        }
        position = static_cast<uint8_t>(header >> 8);
    }
    // 48 entries fill every slot from 0x40 to 0xfc: only a terminator is left.
    return position ? GlkPmCapability::Malformed : GlkPmCapability::NotFound;
}
}

GlkProbeVerdict glkPassiveSample(IOService *provider, const IOService *self, GlkPassiveSnapshot &out) {
    out = {};
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return GlkProbeVerdict::NotPci;
    out.providerRegistryId = pci->getRegistryEntryID();
    out.providerInactive = pci->isInactive();
    if (out.providerInactive) return GlkProbeVerdict::Inactive;
    out.darwinMajor = version_major;
    if (out.darwinMajor != GlkPassiveTarget::DarwinMajor) return GlkProbeVerdict::WrongKernel;

    // Registry identity and nub address first: a different device is refused
    // before any configuration access is issued to it.
    uint32_t vendor = 0, device = 0, revision = 0, classCode = 0, subVendor = 0, sub = 0;
    out.registryComplete = registryWord(pci, "vendor-id", 0xffff, vendor) &&
        registryWord(pci, "device-id", 0xffff, device) &&
        registryWord(pci, "revision-id", 0xff, revision) &&
        registryWord(pci, "class-code", 0xffffff, classCode) &&
        registryWord(pci, "subsystem-vendor-id", 0xffff, subVendor) &&
        registryWord(pci, "subsystem-id", 0xffff, sub);
    out.registry = {static_cast<uint16_t>(vendor), static_cast<uint16_t>(device),
        static_cast<uint8_t>(revision), classCode, static_cast<uint16_t>(subVendor),
        static_cast<uint16_t>(sub)};
    if (!out.registryComplete || !isTarget(out.registry)) return GlkProbeVerdict::RegistryMismatch;
    out.bus = pci->getBusNumber(); out.slot = pci->getDeviceNumber(); out.function = pci->getFunctionNumber();
    if (out.bus != GlkPassiveTarget::Bus || out.slot != GlkPassiveTarget::Slot ||
        out.function != GlkPassiveTarget::Function) return GlkProbeVerdict::WrongLocation;

    // Physical identity. The public configRead32/16 forward to IOPCIFamily
    // extendedConfigRead32/16, which return all-ones when configuration access
    // is refused. Registry properties can be injected, so both must match.
    const UInt32 id = pci->configRead32(ConfigId);
    if (id == 0xffffffffU || (id & 0xffff) == 0xffff) return GlkProbeVerdict::ConfigUnreadable;
    const UInt32 classRevision = pci->configRead32(ConfigClassRevision);
    const UInt32 subsystem = pci->configRead32(ConfigSubsystem);
    out.headerType = static_cast<uint8_t>(pci->configRead32(ConfigHeaderDword) >> 16);
    out.config = {static_cast<uint16_t>(id), static_cast<uint16_t>(id >> 16),
        static_cast<uint8_t>(classRevision), classRevision >> 8,
        static_cast<uint16_t>(subsystem), static_cast<uint16_t>(subsystem >> 16)};
    if (!isTarget(out.config) || (out.headerType & 0x7f)) return GlkProbeVerdict::ConfigMismatch;

    const UInt32 commandStatus = pci->configRead32(ConfigCommandStatus);
    out.command = static_cast<uint16_t>(commandStatus);
    out.status = static_cast<uint16_t>(commandStatus >> 16);
    out.pm = findPm(pci, out.status, out.pmOffset, out.capabilitiesVisited);
    if (out.pm == GlkPmCapability::Found) {
        out.pmc = pci->configRead16(static_cast<UInt8>(out.pmOffset + PmCapabilities));
        out.pmcsr = pci->configRead16(static_cast<UInt8>(out.pmOffset + PmControlStatus));
    }

    out.providerOpen = pci->isOpen();
    out.providerOpenBySelf = self && pci->isOpen(self);
    out.providerBusy = pci->getBusyState();
    if (OSIterator *clients = pci->getClientIterator()) {
        // Counts include this probe. metaCast by name imports no graphics class.
        while (OSObject *client = clients->getNextObject()) {
            if (out.clients == MaxClients) { out.clientsTruncated = true; break; }
            ++out.clients;
            if (client->metaCast("IOFramebuffer")) ++out.framebufferClients;
        }
        clients->release();
    }

    uint64_t unusedValue = 0; uint32_t unusedBits = 0, unusedBytes = 0;
    out.iommuParent = propertyKind(pci, "iommu-parent", out.iommuParentDataBytes, unusedValue, unusedBits);
    out.descriptorOptions = propertyKind(pci, "IOMemoryDescriptorOptions", unusedBytes,
        out.descriptorOptionsValue, out.descriptorOptionsBits);
    // One plain load. checkForSystemMapper/waitForSystemMapper and
    // copyMapperForDevice can sleep without a timeout and are never called.
    out.systemMapper = classifySystemMapper(
        reinterpret_cast<uintptr_t>(__atomic_load_n(&IOMapper::gSystem, __ATOMIC_RELAXED)));
    if (out.providerOpenBySelf) return GlkProbeVerdict::OpenedBySelf;
    return GlkProbeVerdict::Target;
}

bool glkSameDevice(const GlkPassiveSnapshot &a, const GlkPassiveSnapshot &b) {
    return a.providerRegistryId == b.providerRegistryId && a.registryComplete == b.registryComplete &&
        sameIdentity(a.registry, b.registry) && sameIdentity(a.config, b.config) &&
        a.bus == b.bus && a.slot == b.slot && a.function == b.function &&
        a.headerType == b.headerType && a.darwinMajor == b.darwinMajor;
}
bool glkSameObservation(const GlkPassiveSnapshot &a, const GlkPassiveSnapshot &b) {
    return glkSameDevice(a, b) && a.command == b.command && a.status == b.status &&
        a.pm == b.pm && a.pmOffset == b.pmOffset && a.capabilitiesVisited == b.capabilitiesVisited &&
        a.pmc == b.pmc && a.pmcsr == b.pmcsr && a.systemMapper == b.systemMapper &&
        a.iommuParent == b.iommuParent && a.iommuParentDataBytes == b.iommuParentDataBytes &&
        a.descriptorOptions == b.descriptorOptions &&
        a.descriptorOptionsValue == b.descriptorOptionsValue &&
        a.descriptorOptionsBits == b.descriptorOptionsBits && a.providerOpen == b.providerOpen;
}
const char *glkVerdictName(GlkProbeVerdict verdict) {
    switch (verdict) {
    case GlkProbeVerdict::Target: return "Target";
    case GlkProbeVerdict::NotPci: return "NotPci";
    case GlkProbeVerdict::Inactive: return "Inactive";
    case GlkProbeVerdict::WrongKernel: return "WrongKernel";
    case GlkProbeVerdict::RegistryMismatch: return "RegistryMismatch";
    case GlkProbeVerdict::WrongLocation: return "WrongLocation";
    case GlkProbeVerdict::ConfigUnreadable: return "ConfigUnreadable";
    case GlkProbeVerdict::ConfigMismatch: return "ConfigMismatch";
    case GlkProbeVerdict::OpenedBySelf: return "OpenedBySelf";
    case GlkProbeVerdict::Unstable: return "Unstable";
    }
    return "Invalid";
}
const char *glkSystemMapperName(GlkSystemMapperState state) {
    switch (state) {
    case GlkSystemMapperState::NoMapper: return "NoMapper";
    case GlkSystemMapperState::Unknown: return "Unknown";
    case GlkSystemMapperState::HasMapper: return "HasMapper";
    case GlkSystemMapperState::Live: return "Live";
    case GlkSystemMapperState::Unrecognized: return "Unrecognized";
    }
    return "Invalid";
}
const char *glkPropertyKindName(GlkPropertyKind kind) {
    switch (kind) {
    case GlkPropertyKind::Absent: return "Absent";
    case GlkPropertyKind::MapperActive: return "IOMapperActive";
    case GlkPropertyKind::MapperInactive: return "IOMapperInactive";
    case GlkPropertyKind::Number: return "OSNumber";
    case GlkPropertyKind::Data: return "OSData";
    case GlkPropertyKind::Other: return "Other";
    }
    return "Invalid";
}
const char *glkPmCapabilityName(GlkPmCapability state) {
    switch (state) {
    case GlkPmCapability::NoCapabilityList: return "NoCapabilityList";
    case GlkPmCapability::NotFound: return "NotFound";
    case GlkPmCapability::Malformed: return "Malformed";
    case GlkPmCapability::Found: return "Found";
    }
    return "Invalid";
}

} // namespace MellowNative

using namespace MellowNative;

IOService *MellowGlkPassiveProbe::probe(IOService *provider, SInt32 *score) {
    probedTarget_ = false; probed_ = {};
    if (!super::probe(provider, score)) return nullptr;
    GlkPassiveSnapshot sample {};
    const auto verdict = glkPassiveSample(provider, this, sample);
    if (verdict != GlkProbeVerdict::Target) {
        IOLog("MellowGlkPassiveProbe: refused provider: %s\n", glkVerdictName(verdict));
        return nullptr;
    }
    probed_ = sample; probedTarget_ = true;
    return this;
}

bool MellowGlkPassiveProbe::start(IOService *provider) {
    published_ = false;
    if (!probedTarget_ || !super::start(provider)) return false;
    // A fresh sample, not the probe result, is what gets published, and only
    // for the same physical device and registry entry that probe admitted.
    GlkPassiveSnapshot sample {};
    auto verdict = glkPassiveSample(provider, this, sample);
    if (verdict == GlkProbeVerdict::Target && !glkSameDevice(probed_, sample))
        verdict = GlkProbeVerdict::Unstable;
    if (verdict != GlkProbeVerdict::Target || !publish(sample, glkSameObservation(probed_, sample))) {
        IOLog("MellowGlkPassiveProbe: not started: %s\n",
              verdict == GlkProbeVerdict::Target ? "PublishFailed" : glkVerdictName(verdict));
        removeProperty(ObservationKey);
        super::stop(provider);
        return false;
    }
    published_ = true;
    registerService();
    return true;
}

void MellowGlkPassiveProbe::stop(IOService *provider) {
    if (published_) removeProperty(ObservationKey);
    published_ = false; probedTarget_ = false; probed_ = {};
    super::stop(provider);
}

void MellowGlkPassiveProbe::free() {
    // No retained object, lock, mapping, lease or provider open to undo.
    super::free();
}

bool MellowGlkPassiveProbe::publish(const GlkPassiveSnapshot &s, bool matchesProbe) {
    OSDictionary *d = OSDictionary::withCapacity(48);
    if (!d) return false;
    bool ok = true;
    auto number = [&](const char *key, uint64_t value, unsigned bits) {
        OSNumber *object = OSNumber::withNumber(static_cast<unsigned long long>(value), bits);
        ok = ok && object && d->setObject(key, object);
        if (object) object->release();
    };
    auto string = [&](const char *key, const char *value) {
        OSString *object = OSString::withCString(value);
        ok = ok && object && d->setObject(key, object);
        if (object) object->release();
    };
    auto flag = [&](const char *key, bool value) {
        ok = ok && d->setObject(key, value ? kOSBooleanTrue : kOSBooleanFalse);
    };
    string("Schema", "mellowgpu.glk-passive-probe/1");
    string("Scope", "passive observation; not a power, reset, DMA or mapper lease; not GPU support");
    number("ProviderRegistryEntryID", s.providerRegistryId, 64);
    number("VendorID", s.config.vendor, 16); number("DeviceID", s.config.device, 16);
    number("RevisionID", s.config.revision, 8); number("ClassCode", s.config.classCode, 32);
    number("SubsystemVendorID", s.config.subsystemVendor, 16);
    number("SubsystemID", s.config.subsystem, 16);
    flag("RegistryIdentityMatchesConfig", s.registryComplete && sameIdentity(s.registry, s.config));
    number("Bus", s.bus, 8); number("Device", s.slot, 8); number("Function", s.function, 8);
    number("HeaderType", s.headerType, 8);
    number("Command", s.command, 16); number("Status", s.status, 16);
    flag("IOSpaceEnabled", s.command & 0x1); flag("MemorySpaceEnabled", s.command & 0x2);
    flag("BusMasterEnabled", s.command & 0x4);
    string("PMCapability", glkPmCapabilityName(s.pm));
    number("CapabilitiesVisited", s.capabilitiesVisited, 8);
    if (s.pm == GlkPmCapability::Found) {
        static const char *const states[] = {"D0", "D1", "D2", "D3hot"};
        number("PMCapabilityOffset", s.pmOffset, 8);
        number("PMC", s.pmc, 16); number("PMCSR", s.pmcsr, 16);
        string("PowerState", states[s.pmcsr & 0x3]);
        flag("NoSoftReset", s.pmcsr & 0x0008); flag("PMEEnable", s.pmcsr & 0x0100);
        flag("PMEStatus", s.pmcsr & 0x8000);
    }
    string("SystemMapper", glkSystemMapperName(s.systemMapper));
    string("IOMMUParent", glkPropertyKindName(s.iommuParent));
    if (s.iommuParent == GlkPropertyKind::Data) number("IOMMUParentDataBytes", s.iommuParentDataBytes, 32);
    string("IOMemoryDescriptorOptions", glkPropertyKindName(s.descriptorOptions));
    if (s.descriptorOptions == GlkPropertyKind::Number) {
        // Values wider than 32 bits are withheld: only option masks are expected.
        const bool narrow = s.descriptorOptionsValue <= 0xffffffffULL;
        number("IOMemoryDescriptorOptionsBits", s.descriptorOptionsBits, 32);
        if (narrow) number("IOMemoryDescriptorOptionsValue", s.descriptorOptionsValue, 32);
        flag("IOMemoryDescriptorOptionsWithheld", !narrow);
        flag("IOMemoryDescriptorOptionsMapperNone", s.descriptorOptionsValue == kIOMemoryMapperNone);
    }
    flag("ProviderOpen", s.providerOpen);
    number("ProviderBusyState", s.providerBusy, 32);
    number("ProviderClients", s.clients, 32);
    number("FramebufferClients", s.framebufferClients, 32);
    flag("ProviderClientsTruncated", s.clientsTruncated);
    number("DarwinMajor", static_cast<uint32_t>(s.darwinMajor), 32);
    flag("SamplesAgree", matchesProbe);
    flag("DurablePowerLease", false); flag("DMALease", false); flag("MapperLease", false);
    flag("GPUSupport", false); flag("Acceleration", false);
    ok = ok && setProperty(ObservationKey, d);
    d->release();
    return ok;
}

// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// The Gen9 protocol it drives is MIT (Drivers/NativeGlk/LICENSE.MIT).
#include "GlkForcewakeIOKit.hpp"
#include "HardwareAccess.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>

namespace MellowNativeGlk {
namespace {
using Status = ForcewakeStatus;
constexpr uint8_t ConfigHeaderType = 0x0e, ConfigCapabilityPointer = 0x34;
constexpr uint16_t StatusCapabilityList = 0x10;

uint32_t addressSpaceBdf(const GlkAuthoritySnapshot &pci) {
    return uint32_t(pci.bus) << 16 | uint32_t(pci.slot) << 11 | uint32_t(pci.function) << 8;
}
} // namespace

bool IOKitForcewake::snapshot(uint64_t ownerEpoch, GlkAuthoritySnapshot &out) const {
    out = {};
    if (!device_ || !owner_) return false;
    IOPCIDevice *device = device_;
    const uint32_t id = device->configRead32(kIOPCIConfigVendorID);
    out.vendor = static_cast<uint16_t>(id);
    out.device = static_cast<uint16_t>(id >> 16);
    out.classRevision = device->configRead32(kIOPCIConfigRevisionID);
    out.subsystemVendor = device->configRead16(kIOPCIConfigSubSystemVendorID);
    out.subsystemDevice = device->configRead16(kIOPCIConfigSubSystemID);
    out.command = device->configRead16(kIOPCIConfigCommand);
    out.headerType = device->configRead8(ConfigHeaderType);
    out.bus = device->getBusNumber();
    out.slot = device->getDeviceNumber();
    out.function = device->getFunctionNumber();
    out.registryId = device->getRegistryEntryID();
    out.epoch = ownerEpoch;
    out.bar0Low = device->configRead32(kIOPCIConfigBaseAddress0);
    out.bar0High = (out.bar0Low & 6U) == 4U ? device->configRead32(kIOPCIConfigBaseAddress1) : 0;
    // Bounded read-only capability walk; findPCICapability() may change
    // config access state in Apple's implementation, so it is not used.
    const uint16_t status = device->configRead16(kIOPCIConfigStatus);
    uint8_t offset = status != 0xffff && (status & StatusCapabilityList) && !(out.headerType & 0x7fU)
        ? device->configRead8(ConfigCapabilityPointer) : 0;
    uint64_t visited = 0;
    for (unsigned count = 0; offset && count < 48; ++count) {
        if (offset < 0x40 || offset > 0xf8 || (offset & 3U)) break;
        const uint64_t mark = uint64_t(1) << ((offset - 0x40) / 4);
        if (visited & mark) break;
        visited |= mark;
        const uint32_t capability = device->configRead32(offset);
        if (capability == UINT32_MAX) break;
        if ((capability & 0xffU) == kIOPCIPowerManagementCapability) {
            out.pmCapability = true;
            out.pmcsr = device->configRead16(static_cast<uint8_t>(offset + 4));
            break;
        }
        offset = static_cast<uint8_t>(capability >> 8);
    }
    OSObject *tunnel = device->copyProperty(kIOPCITunnelledKey);
    out.tunneled = tunnel != nullptr;
    if (tunnel) tunnel->release();
    out.providerOpen = device->isOpen(owner_) && owner_->getProvider() == device;
    out.inactive = device->isInactive() || owner_->isInactive();
    out.powerStateD0 = device->getPowerState() == kIOPCIDeviceOnState;
    if (map_) {
        out.mappedPhysical = map_->getPhysicalAddress();
        out.mappedLength = map_->getLength();
    }
    return true;
}

bool IOKitForcewake::findBar0(const GlkAuthoritySnapshot &pci, uint64_t physical) {
    OSObject *property = device_->copyProperty(gIODeviceMemoryKey);
    OSArray *array = OSDynamicCast(OSArray, property);
    bool ambiguous = false;
    if (array) for (unsigned i = 0; i < array->getCount(); ++i) {
        auto *descriptor = OSDynamicCast(IOMemoryDescriptor, array->getObject(i));
        if (!descriptor) continue;
        const uint32_t tag = descriptor->getTag();
        if ((tag & 0xffU) != kIOPCIConfigBaseAddress0) continue;
        IOByteCount contiguous = 0;
        const uint64_t segment = descriptor->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        if (bar0_ || ((tag >> 24) & 3U) != kIOPCI64BitMemorySpace ||
            (tag & 0x00ffff00U) != addressSpaceBdf(pci) || segment != physical ||
            descriptor->getLength() < RegisterWindowBytes || contiguous < descriptor->getLength() ||
            descriptor->getLength() > UINT64_MAX - physical) {
            ambiguous = true;
            break;
        }
        descriptor->retain();
        bar0_ = descriptor;
    }
    if (property) property->release();
    return bar0_ && !ambiguous;
}

ForcewakeStatus IOKitForcewake::attach(IOPCIDevice *device, IOService *owner,
                                       const GlkPciExpectation &expected) {
    if (device_ || map_) return Status::Busy;
    if (!device || !owner || owner->getProvider() != device) return Status::Ownership;
    if (!isGlkDeviceId(expected.device)) return Status::Unsupported;
    device_ = device;
    owner_ = owner;
    expected_ = expected;
    device_->retain();
    owner_->retain();
    GlkAuthoritySnapshot pci {};
    if (!snapshot(expected.epoch, pci)) { abandon(); return Status::IoFailure; }
    // Validate all authority facts before mapping, using the BAR as the
    // would-be mapping; the real mapping is validated again below.
    uint64_t physical = 0;
    if (glkBar0Physical(pci, physical)) {
        pci.mappedPhysical = physical;
        pci.mappedLength = RegisterWindowBytes;
    }
    Status status = validateAuthority(expected_, pci);
    if (status != Status::Ok) { abandon(); return status; }
    if (!findBar0(pci, physical)) { abandon(); return Status::Unavailable; }
    map_ = bar0_->map(kIOMapInhibitCache);
    if (!map_ || !map_->getVirtualAddress() || (map_->getVirtualAddress() & 3U)) {
        abandon();
        return Status::IoFailure;
    }
    if (map_->getPhysicalAddress() != physical || map_->getLength() != bar0_->getLength()) {
        abandon();
        return Status::IdentityChanged;
    }
    base_ = reinterpret_cast<volatile uint32_t *>(map_->getVirtualAddress());
    length_ = map_->getLength();
    if (!snapshot(expected.epoch, pci)) { abandon(); return Status::IoFailure; }
    if ((status = validateAuthority(expected_, pci)) != Status::Ok) { abandon(); return status; }
    ForcewakeIo io {this, read32, write32, nowUs, delayUs, relax};
    if ((status = engine_.bind(io)) != Status::Ok) { abandon(); return status; }
    return Status::Ok;
}

ForcewakeStatus IOKitForcewake::open(uint64_t ownerEpoch) {
    if (!device_ || !map_ || engine_.state() == Gen9Forcewake::State::Unbound) return Status::Unavailable;
    if (window_) return Status::Busy;
    GlkAuthoritySnapshot pci {};
    if (!snapshot(ownerEpoch, pci)) return Status::IoFailure;
    const Status status = validateAuthority(expected_, pci);
    if (status != Status::Ok) return status;
    window_ = true;
    return Status::Ok;
}

// Authority lost during an operation invalidates every result: the engine is
// abandoned (no further MMIO) and the caller must detach and re-attach.
ForcewakeStatus IOKitForcewake::close(uint64_t ownerEpoch, ForcewakeStatus result) {
    window_ = false;
    GlkAuthoritySnapshot pci {};
    Status status = snapshot(ownerEpoch, pci) ? validateAuthority(expected_, pci) : Status::IoFailure;
    if (status == Status::Ok) return result;
    engine_.unbind(true);
    return status;
}

ForcewakeStatus IOKitForcewake::deadline(uint64_t timeoutUs, uint64_t &deadlineUs) const {
    uint64_t start = 0;
    if (!timeoutUs || !nowUs(nullptr, start)) return Status::Invalid;
    if (timeoutUs > UINT64_MAX - start) return Status::Invalid;
    deadlineUs = start + timeoutUs;
    return Status::Ok;
}

ForcewakeStatus IOKitForcewake::initialize(uint64_t ownerEpoch, uint64_t timeoutUs) {
    uint64_t deadlineUs = 0;
    Status status = deadline(timeoutUs, deadlineUs);
    if (status != Status::Ok) return status;
    if ((status = open(ownerEpoch)) != Status::Ok) return status;
    return close(ownerEpoch, engine_.initialize(deadlineUs));
}

ForcewakeStatus IOKitForcewake::get(uint64_t ownerEpoch, uint8_t domains, uint64_t timeoutUs,
                                    ForcewakeLease &lease) {
    lease = {};
    uint64_t deadlineUs = 0;
    Status status = deadline(timeoutUs, deadlineUs);
    if (status != Status::Ok) return status;
    if ((status = open(ownerEpoch)) != Status::Ok) return status;
    ForcewakeLease acquired {};
    status = close(ownerEpoch, engine_.get(domains, deadlineUs, acquired));
    if (status == Status::Ok) lease = acquired;
    return status;
}

ForcewakeStatus IOKitForcewake::put(uint64_t ownerEpoch, ForcewakeLease &lease) {
    const Status status = open(ownerEpoch);
    if (status != Status::Ok) return status;
    return close(ownerEpoch, engine_.put(lease));
}

ForcewakeStatus IOKitForcewake::reset(uint64_t ownerEpoch, uint8_t &lostDomains) {
    lostDomains = 0;
    const Status status = open(ownerEpoch);
    if (status != Status::Ok) return status;
    return close(ownerEpoch, engine_.reset(lostDomains));
}

void IOKitForcewake::detach(uint64_t ownerEpoch) {
    if (engine_.state() != Gen9Forcewake::State::Unbound &&
        (engine_.activeDomains() || engine_.liveLeases()) && open(ownerEpoch) == Status::Ok) {
        uint8_t lost = 0;
        close(ownerEpoch, engine_.reset(lost));
    }
    abandon();
}

void IOKitForcewake::abandon() {
    engine_.unbind(true);
    window_ = false;
    base_ = nullptr;
    length_ = 0;
    // Release callbacks may destroy an owner that embeds this adapter. Close
    // all fields first, then use captured locals only; owner is released last.
    IOMemoryMap *map = map_;
    IOMemoryDescriptor *bar = bar0_;
    IOPCIDevice *device = device_;
    IOService *owner = owner_;
    map_ = nullptr;
    bar0_ = nullptr;
    device_ = nullptr;
    owner_ = nullptr;
    expected_ = {};
    if (map) map->release();
    if (bar) bar->release();
    if (device) device->release();
    if (owner) owner->release();
}

bool IOKitForcewake::read32(void *context, uint32_t offset, uint32_t &value) {
    auto &self = *static_cast<IOKitForcewake *>(context);
    if (!self.window_ || !isForcewakeRegister(offset)) return false;
    return MellowHardware::read32(self.base_, self.length_, offset, value);
}

bool IOKitForcewake::write32(void *context, uint32_t offset, uint32_t value) {
    auto &self = *static_cast<IOKitForcewake *>(context);
    if (!self.window_ || !isForcewakeRegister(offset)) return false;
    return MellowHardware::write32(self.base_, self.length_, offset, value);
}

bool IOKitForcewake::nowUs(void *, uint64_t &value) {
    uint64_t absolute = 0, nanoseconds = 0;
    clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute, &nanoseconds);
    value = nanoseconds / 1000;
    return true;
}

void IOKitForcewake::delayUs(void *, uint32_t microseconds) {
    IODelay(microseconds);
}

void IOKitForcewake::relax(void *) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}
} // namespace MellowNativeGlk

// Host-only OS boundary for the unchanged GLK BCS adapter.
// HOSTED INJECTED TRANSPORT: the "BAR" is ordinary host memory. No PCI
// configuration access, MMIO, DMA, barrier, delay or GPU operation happens,
// and passing tests here are never evidence of hardware execution.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOVirtualAddress = uint64_t;
constexpr unsigned kIOPCIConfigVendorID = 0x00, kIOPCIConfigCommand = 0x04,
    kIOPCIConfigRevisionID = 0x08, kIOPCIConfigBaseAddress0 = 0x10,
    kIOPCIConfigSubSystemVendorID = 0x2c;
constexpr IOOptionBits kIOMapInhibitCache = 0x100; // shim value; SDK value is used natively

namespace GlkBcsShim {
inline unsigned delayCalls, maps, unmaps, configReads, lastMapRegister {~0u};
inline uint64_t delayMicroseconds;
inline IOOptionBits lastMapOptions {~0u};
inline bool mapFails;
inline uint64_t mapLength {2ULL * 1024 * 1024};
inline std::set<void *> liveMaps;
inline void *lastMap;
}

inline void IODelay(unsigned microseconds) {
    ++GlkBcsShim::delayCalls; GlkBcsShim::delayMicroseconds += microseconds;
}

class OSObject {
public:
    unsigned refs {1};
    virtual ~OSObject() = default;
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};
class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    bool isInactive() const { return inactive; }
    IOService *getProvider() const { return provider; }
};
class IOMemoryMap : public OSObject {
public:
    uint8_t *bytes {};
    uint64_t length {};
    explicit IOMemoryMap(uint64_t n) : length(n) {
        bytes = static_cast<uint8_t *>(std::aligned_alloc(4096, static_cast<size_t>(n)));
        if (!bytes) std::abort();
        std::memset(bytes, 0, static_cast<size_t>(n));
        GlkBcsShim::liveMaps.insert(this);
    }
    ~IOMemoryMap() override { ++GlkBcsShim::unmaps; GlkBcsShim::liveMaps.erase(this); std::free(bytes); }
    IOVirtualAddress getVirtualAddress() { return reinterpret_cast<uintptr_t>(bytes); }
    IOByteCount getLength() { return length; }
};
class IOPCIDevice : public IOService {
public:
    // Lenovo 15IGL7 identity tuple as plain fixture data, not a hardware read.
    uint32_t physicalId {0x31858086}, subsystemId {0x3e7717aa};
    uint8_t revision {0x06};
    uint16_t command {0x0006}; // memory space + bus lead, enabled by the PCI owner
    uint64_t registryId {4294967813ULL};
    IOService *openOwner {};
    uint32_t configRead32(IOByteCount offset) {
        ++GlkBcsShim::configReads;
        if (offset == kIOPCIConfigVendorID) return physicalId;
        if (offset == kIOPCIConfigSubSystemVendorID) return subsystemId;
        std::abort();
    }
    uint16_t configRead16(IOByteCount offset) {
        ++GlkBcsShim::configReads; if (offset != kIOPCIConfigCommand) std::abort(); return command;
    }
    uint8_t configRead8(IOByteCount offset) {
        ++GlkBcsShim::configReads; if (offset != kIOPCIConfigRevisionID) std::abort(); return revision;
    }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
    IOMemoryMap *mapDeviceMemoryWithRegister(UInt8 reg, IOOptionBits options) {
        ++GlkBcsShim::maps; GlkBcsShim::lastMapRegister = reg; GlkBcsShim::lastMapOptions = options;
        if (GlkBcsShim::mapFails) return nullptr;
        auto *map = new IOMemoryMap(GlkBcsShim::mapLength);
        GlkBcsShim::lastMap = map;
        return map;
    }
};

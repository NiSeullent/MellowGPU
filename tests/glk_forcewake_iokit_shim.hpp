// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// Explicit HOST-ONLY IOKit boundary for Mellow/GlkForcewakeIOKit.cpp, which is
// compiled unchanged against these types. BAR memory here is plain host RAM:
// it has no forcewake behavior, so it can never produce a hardware ack.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
enum : uint32_t {
    kIOPCIConfigVendorID = 0x00, kIOPCIConfigCommand = 0x04, kIOPCIConfigStatus = 0x06,
    kIOPCIConfigRevisionID = 0x08, kIOPCIConfigBaseAddress0 = 0x10, kIOPCIConfigBaseAddress1 = 0x14,
    kIOPCIConfigSubSystemVendorID = 0x2c, kIOPCIConfigSubSystemID = 0x2e
};
constexpr unsigned kIOPCIPowerManagementCapability = 1, kIOPCIDeviceOnState = 2,
    kIOPCI64BitMemorySpace = 3;
constexpr IOOptionBits kIOMapInhibitCache = 0x200, kIOMemoryMapperNone = 0x200000;
inline constexpr const char *gIODeviceMemoryKey = "IODeviceMemory";
#define kIOPCITunnelledKey "IOPCITunnelled"

namespace GlkShim {
inline unsigned alive = 0, delays = 0, clockCalls = 0;
inline uint64_t clockNs = 1000000, clockStepNs = 100000;
inline void (*clockHook)(unsigned call) = nullptr; // test-only state change injection
}
inline void IODelay(unsigned microseconds) { ++GlkShim::delays; GlkShim::clockNs += microseconds * 1000ULL; }
inline void clock_get_uptime(uint64_t *result) {
    if (GlkShim::clockHook) GlkShim::clockHook(++GlkShim::clockCalls);
    else ++GlkShim::clockCalls;
    GlkShim::clockNs += GlkShim::clockStepNs;
    *result = GlkShim::clockNs;
}
inline void absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *result) { *result = absolute; }

class OSObject {
public:
    unsigned refs {1};
    OSObject() { ++GlkShim::alive; }
    virtual ~OSObject() { --GlkShim::alive; }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)

class OSArray : public OSObject {
public:
    std::vector<OSObject *> objects;
    ~OSArray() override { for (auto *object : objects) object->release(); }
    void add(OSObject *object) { object->retain(); objects.push_back(object); }
    unsigned getCount() const { return static_cast<unsigned>(objects.size()); }
    OSObject *getObject(unsigned index) const { return index < objects.size() ? objects[index] : nullptr; }
};

class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    unsigned powerState {kIOPCIDeviceOnState};
    std::map<std::string, OSObject *> properties;
    ~IOService() override { for (auto &item : properties) item.second->release(); }
    IOService *getProvider() const { return provider; }
    bool isInactive() const { return inactive; }
    unsigned getPowerState() const { return powerState; }
    OSObject *copyProperty(const char *name) {
        auto found = properties.find(name);
        if (found == properties.end()) return nullptr;
        found->second->retain();
        return found->second;
    }
    void fixtureProperty(const char *name, OSObject *value) {
        auto found = properties.find(name);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
        if (value) { value->retain(); properties.emplace(name, value); }
    }
};

class IOMemoryDescriptor;
class IOMemoryMap : public OSObject {
public:
    IOMemoryDescriptor *descriptor;
    uintptr_t virtualAddress;
    uint64_t physical, length;
    IOMemoryMap(IOMemoryDescriptor *, uintptr_t, uint64_t, uint64_t);
    ~IOMemoryMap() override;
    uintptr_t getVirtualAddress() const { return virtualAddress; }
    uint64_t getPhysicalAddress() const { return physical; }
    uint64_t getLength() const { return length; }
};
class IOMemoryDescriptor : public OSObject {
public:
    std::vector<uint32_t> registers = std::vector<uint32_t>(2 * 1024 * 1024 / 4);
    uint64_t physical {0xa0000000}, length {2 * 1024 * 1024};
    uint32_t tag {(kIOPCI64BitMemorySpace << 24) | (2U << 11) | kIOPCIConfigBaseAddress0};
    uint64_t mapPhysicalOverride {};
    unsigned maps {};
    uint32_t getTag() const { return tag; }
    uint64_t getLength() const { return length; }
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *bytes, IOOptionBits options) {
        if (options != kIOMemoryMapperNone) std::abort();
        if (bytes) *bytes = offset < length ? length - offset : 0;
        return offset < length ? physical + offset : 0;
    }
    IOMemoryMap *map(IOOptionBits options) {
        if (options != kIOMapInhibitCache) std::abort();
        ++maps;
        return new IOMemoryMap(this, reinterpret_cast<uintptr_t>(registers.data()),
                               mapPhysicalOverride ? mapPhysicalOverride : physical, length);
    }
    uint32_t at(uint32_t offset) const { return registers.at(offset / 4); }
    bool untouched() const {
        for (uint32_t value : registers) if (value) return false;
        return true;
    }
};
inline IOMemoryMap::IOMemoryMap(IOMemoryDescriptor *d, uintptr_t v, uint64_t p, uint64_t n)
    : descriptor(d), virtualAddress(v), physical(p), length(n) { descriptor->retain(); }
inline IOMemoryMap::~IOMemoryMap() { descriptor->release(); }

class IOPCIDevice : public IOService {
public:
    uint8_t config[256] {};
    uint64_t registryId {0x100000123};
    uint8_t bus {0}, slot {2}, function {0};
    IOService *openOwner {};
    uint64_t getRegistryEntryID() const { return registryId; }
    uint8_t getBusNumber() const { return bus; }
    uint8_t getDeviceNumber() const { return slot; }
    uint8_t getFunctionNumber() const { return function; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
    void fixture8(unsigned offset, uint8_t value) { config[offset] = value; }
    void fixture16(unsigned offset, uint16_t value) {
        fixture8(offset, uint8_t(value)); fixture8(offset + 1, uint8_t(value >> 8));
    }
    void fixture32(unsigned offset, uint32_t value) {
        fixture16(offset, uint16_t(value)); fixture16(offset + 2, uint16_t(value >> 16));
    }
    uint8_t configRead8(IOByteCount offset) { return offset < 256 ? config[offset] : 0xff; }
    uint16_t configRead16(IOByteCount offset) {
        return offset <= 254 ? uint16_t(config[offset] | config[offset + 1] << 8) : 0xffff;
    }
    uint32_t configRead32(IOByteCount offset) {
        return offset <= 252 ? uint32_t(configRead16(offset)) | uint32_t(configRead16(offset + 2)) << 16
                             : 0xffffffffU;
    }
};

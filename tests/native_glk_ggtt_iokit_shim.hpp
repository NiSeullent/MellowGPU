// Host-only OS/device boundary for the unchanged GLK GGTT adapter.
// HOSTED INJECTED TRANSPORT: no PCI configuration access, BAR mapping, MMIO,
// cache attribute, CPU barrier, forcewake, TLB or GPU operation is performed.
// "BAR0" is a host buffer, mappings are windows into it and the engine TLB
// controls are a scripted model. Passing is never evidence of GPU behaviour.
// There is deliberately no configWrite: the adapter cannot write config space.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOVirtualAddress = uintptr_t;
using IOPhysicalAddress = uint64_t;
using mach_vm_address_t = uint64_t;
using mach_vm_size_t = uint64_t;
using task_t = void *;
inline task_t kernel_task = reinterpret_cast<void *>(uintptr_t(1));
// Values as in the SDK15.5 IOKit/IOMapTypes.h and IOPCIDevice.h.
constexpr IOOptionBits kIOMapAnywhere = 0x1, kIOMapCacheMask = 0xf00, kIOMapDefaultCache = 0,
    kIOMapInhibitCache = 0x100, kIOMapWriteCombineCache = 0x400;
constexpr unsigned kIOPCIConfigVendorID = 0x00, kIOPCIConfigCommand = 0x04,
    kIOPCIConfigRevisionID = 0x08, kIOPCIConfigBaseAddress0 = 0x10,
    kIOPCIConfigBaseAddress1 = 0x14, kIOPCIConfigSubSystemVendorID = 0x2c;

class OSObject;
namespace GlkGgttShim {
constexpr uint64_t BarBytes = 16ULL << 20, GsmOffset = 8ULL << 20;
// 'M' map, 'U' unmap, 'r'/'w' GSM entry, 'Q'/'W' register, 'F'/'S'/'L' fence,
// 'A'/'R' GT hold, 'P' pause (value in microseconds).
struct Ev { char op; uint64_t at, value; };
struct Window { uintptr_t begin, end; uint64_t barOffset; bool live; };
struct Faults {
    long failMapCall {-1};
    IOOptionBits forceCache {~0u};
    uint64_t skewPhysical {};
    unsigned mallocFailureCall {};
    long dropEntryIndex {-1};
    std::set<uint32_t> stuck;
    unsigned clearAfter {2};
    bool frozenClock {};
};
inline Faults faults;
inline std::set<OSObject *> objects;
inline std::map<void *, size_t> allocations;
inline std::vector<Ev> events;
inline std::vector<Window> windows;
inline std::map<uint32_t, unsigned> pending;
inline uint8_t *bar0 {};
inline uint64_t clockNs {5000000000ULL};
inline unsigned mapCalls {}, mallocCalls {}, configReads {}, liveMaps {};
inline IOOptionBits lastRequestedOptions {};

inline void reset() {
    faults = {}; events.clear(); windows.clear(); pending.clear();
    clockNs = 5000000000ULL; mapCalls = mallocCalls = configReads = liveMaps = 0;
    lastRequestedOptions = 0;
    if (!bar0) {
        bar0 = static_cast<uint8_t *>(std::aligned_alloc(4096, BarBytes));
        if (!bar0) std::abort();
    }
    std::memset(bar0, 0, BarBytes);
}
inline unsigned count(char op) {
    unsigned n = 0;
    for (const Ev &e : events) n += e.op == op;
    return n;
}
// The adapter may only touch bytes inside a live mapping it created.
inline uint64_t barOffset(const volatile void *at, size_t width) {
    const auto address = reinterpret_cast<uintptr_t>(at);
    for (const Window &w : windows) {
        if (w.live && address >= w.begin && address + width <= w.end && !(address % width))
            return w.barOffset + (address - w.begin);
    }
    std::abort();
}
void disposeWorld();
} // namespace GlkGgttShim

class OSObject {
public:
    unsigned refs {1};
    OSObject() { GlkGgttShim::objects.insert(this); }
    virtual ~OSObject() { GlkGgttShim::objects.erase(this); }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};

class IOMemoryMap : public OSObject {
public:
    uintptr_t va {};
    uint64_t physical {}, length {};
    IOOptionBits options {};
    size_t window {};
    ~IOMemoryMap() override {
        GlkGgttShim::windows[window].live = false;
        --GlkGgttShim::liveMaps;
        GlkGgttShim::events.push_back({'U', physical, length});
    }
    IOVirtualAddress getVirtualAddress() { return va; }
    IOPhysicalAddress getPhysicalAddress() { return physical; }
    IOByteCount getLength() { return length; }
    IOOptionBits getMapOptions() { return options; }
};

class IOMemoryDescriptor : public OSObject {};
class IODeviceMemory : public IOMemoryDescriptor {
public:
    uint64_t physical {}, length {};
    IODeviceMemory(uint64_t p, uint64_t l) : physical(p), length(l) {}
    IOPhysicalAddress getPhysicalAddress() { return physical; }
    IOByteCount getLength() const { return length; }
    IOMemoryMap *createMappingInTask(task_t task, mach_vm_address_t address, IOOptionBits options,
                                     mach_vm_size_t offset, mach_vm_size_t bytes) {
        using namespace GlkGgttShim;
        if (task != kernel_task || address || !(options & kIOMapAnywhere) || !bytes) std::abort();
        lastRequestedOptions = options;
        if (long(mapCalls++) == faults.failMapCall) return nullptr;
        if (offset > BarBytes || bytes > BarBytes - offset || offset + bytes > length) return nullptr;
        auto *map = new IOMemoryMap;
        map->va = reinterpret_cast<uintptr_t>(bar0) + uintptr_t(offset);
        map->physical = physical + offset + faults.skewPhysical;
        map->length = bytes;
        map->options = faults.forceCache == ~0u ? options
                                                : (options & ~kIOMapCacheMask) | faults.forceCache;
        map->window = windows.size();
        windows.push_back({map->va, map->va + uintptr_t(bytes), offset, true});
        ++liveMaps;
        events.push_back({'M', offset, bytes});
        return map;
    }
};

class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    bool isInactive() const { return inactive; }
    IOService *getProvider() const { return provider; }
};
class IOMapper : public IOService {};

class IOPCIDevice : public IOService {
public:
    // Lenovo 15IGL7 identity and plausible BAR/GMCH values as plain fixture
    // data, not hardware reads. GMCH GGMS=3 (8 MiB GSM, 4 GiB GGTT).
    uint32_t physicalId {0x31858086}, subsystemId {0x3e7717aa};
    uint8_t revision {0x06};
    uint32_t bar0Low {0xa0000004}, bar0High {0};
    uint16_t command {0x0007}, gmch {0x02c1};
    uint64_t registryId {4294967813ULL};
    IOService *openOwner {};
    IODeviceMemory *barMemory {};
    ~IOPCIDevice() override { if (barMemory) barMemory->release(); }
    uint32_t configRead32(IOByteCount offset) {
        ++GlkGgttShim::configReads;
        if (offset == kIOPCIConfigVendorID) return physicalId;
        if (offset == kIOPCIConfigSubSystemVendorID) return subsystemId;
        if (offset == kIOPCIConfigBaseAddress0) return bar0Low;
        if (offset == kIOPCIConfigBaseAddress1) return bar0High;
        std::abort();
    }
    uint16_t configRead16(IOByteCount offset) {
        ++GlkGgttShim::configReads;
        if (offset == kIOPCIConfigCommand) return command;
        if (offset == 0x50) return gmch;
        std::abort();
    }
    uint8_t configRead8(IOByteCount offset) {
        ++GlkGgttShim::configReads;
        if (offset != kIOPCIConfigRevisionID) std::abort();
        return revision;
    }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
    IODeviceMemory *getDeviceMemoryWithRegister(UInt8 reg) {
        return reg == kIOPCIConfigBaseAddress0 ? barMemory : nullptr;
    }
};

inline void *IOMalloc(size_t size) {
    if (++GlkGgttShim::mallocCalls == GlkGgttShim::faults.mallocFailureCall) return nullptr;
    void *result = std::malloc(size);
    if (result) GlkGgttShim::allocations.emplace(result, size);
    return result;
}
inline void IOFree(void *address, size_t size) {
    auto found = GlkGgttShim::allocations.find(address);
    if (found == GlkGgttShim::allocations.end() || found->second != size) std::abort();
    GlkGgttShim::allocations.erase(found);
    std::free(address);
}
inline void IODelay(unsigned microseconds) {
    GlkGgttShim::events.push_back({'P', 0, microseconds});
    if (!GlkGgttShim::faults.frozenClock) GlkGgttShim::clockNs += uint64_t(microseconds) * 1000;
}
inline void IOSleep(unsigned milliseconds) {
    GlkGgttShim::events.push_back({'P', 0, uint64_t(milliseconds) * 1000});
    if (!GlkGgttShim::faults.frozenClock) GlkGgttShim::clockNs += uint64_t(milliseconds) * 1000000;
}
// Absolute time is nanoseconds in this model.
inline void clock_get_uptime(uint64_t *result) { *result = GlkGgttShim::clockNs; }
inline void absolutetime_to_nanoseconds(uint64_t absolute, uint64_t *result) { *result = absolute; }

namespace GlkGgttShim {
inline void disposeWorld() {
    // Test teardown only, after all assertions; implies no hardware recovery.
    // Maps first, then devices (which own their BAR descriptor), then the rest.
    for (int pass = 0; pass < 3; ++pass) {
        for (;;) {
            OSObject *next = nullptr;
            for (OSObject *object : objects) {
                if ((pass == 0 && dynamic_cast<IOMemoryMap *>(object)) ||
                    (pass == 1 && dynamic_cast<IOPCIDevice *>(object)) || pass == 2) { next = object; break; }
            }
            if (!next) break;
            delete next;
        }
    }
    for (auto &allocation : allocations) std::free(allocation.first);
    allocations.clear();
}
} // namespace GlkGgttShim

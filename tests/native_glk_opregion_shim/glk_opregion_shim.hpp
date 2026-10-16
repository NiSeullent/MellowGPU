// Host-only OS boundary for GlkOpRegionIOKit and the unchanged physical owner.
// HOSTED INJECTED MODEL: no PCI configuration cycle, physical memory, page
// table, PM transition, ACPI or firmware access occurs. "Physical" ranges are
// host buffers. Passing tests here are never evidence about the 15IGL7.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
using IOVirtualAddress = uintptr_t;
using IOPhysicalAddress = uint64_t;
using mach_vm_address_t = uint64_t;
using mach_vm_size_t = uint64_t;
using AbsoluteTime = uint64_t;
using IOPMPowerFlags = unsigned long;
using task_t = void *;
#define TASK_NULL (static_cast<task_t>(nullptr))
inline task_t kernel_task = reinterpret_cast<task_t>(uintptr_t(0x4b45524e));
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 0x2bc, kIOReturnBadArgument = 0x2c2,
    kIOReturnNotPermitted = 0x2e2;
constexpr IOOptionBits kIODirectionIn = 1, kIODirectionOut = 2, kIOMemoryMapperNone = 0x800;
constexpr IOOptionBits kIOMapAnywhere = 0x1, kIOMapCacheMask = 0xf00, kIOMapDefaultCache = 0,
    kIOMapInhibitCache = 0x100, kIOMapCopybackCache = 0x300, kIOMapReadOnly = 0x1000;
constexpr uint8_t kIOPCIConfigVendorID = 0x00, kIOPCIConfigCommand = 0x04, kIOPCIConfigStatus = 0x06,
    kIOPCIConfigRevisionID = 0x08, kIOPCIConfigSubSystemVendorID = 0x2c;
constexpr uint16_t kIOPCICommandMemorySpace = 0x2, kIOPCICommandBusLead = 0x4;
constexpr unsigned long kIOPMPowerStateVersion1 = 1, kIOPMPowerOn = 0x2, kIOPMDeviceUsable = 0x8000;
constexpr IOReturn kIOPMAckImplied = 0;
constexpr UInt32 kIOMessageServiceIsTerminated = 0xe0000010, kIOMessageServiceIsRequestingClose = 0xe0000100;
constexpr int THREAD_UNINT = 0, THREAD_TIMED_OUT = 1;
constexpr uint32_t kMicrosecondScale = 1000;
inline int version_major = 24;
inline void clock_interval_to_deadline(uint32_t, uint32_t, AbsoluteTime *deadline) { *deadline = 1; }

class IOPCIDevice;
class IOMemoryDescriptor;
class IOMemoryMap;
namespace OpRegionShim {
struct DescriptorCall { uint64_t address, length; IOOptionBits options; task_t task; };
struct MappingCall { task_t task; uint64_t at; IOOptionBits options; uint64_t offset, length; };
struct Ram { uint64_t base; uint64_t size; uint8_t *host; };
struct Faults {
    int descriptorAt {-1}, mappingAt {-1}, unmapAt {-1}, mallocAt {-1};
    int64_t lengthSkew {}, physicalSkew {};
    bool dropReadOnly {}, misalignVirtual {};
};
struct State {
    Faults faults {};
    std::vector<DescriptorCall> descriptors;
    std::vector<MappingCall> mappings;
    std::vector<Ram> ram;
    std::map<void *, size_t> blocks;
    int liveObjects {}, liveDescriptors {}, liveMaps {}, mappedMaps {};
    int descriptorCalls {}, mappingCalls {}, unmapCalls {}, mallocCalls {}, freeCalls {};
    int releasedWhileMapped {};
    uint64_t loads {};
    void (*onLoad)(uint64_t count) {};
    void (*onConfigRead)(IOPCIDevice *, uint8_t offset) {};
};
inline State state;
inline uint8_t *ramAt(uint64_t physical, uint64_t length) {
    for (auto &r : state.ram)
        if (physical >= r.base && length <= r.size && physical - r.base <= r.size - length)
            return r.host + (physical - r.base);
    return nullptr;
}
// Firmware RAM model: page-aligned host backing so in-page offsets match.
inline uint8_t *addRam(uint64_t base, uint64_t size) {
    const uint64_t rounded = (size + 4095) & ~uint64_t(4095);
    auto *host = static_cast<uint8_t *>(std::aligned_alloc(4096, rounded));
    std::memset(host, 0, rounded);
    state.ram.push_back({base, rounded, host});
    return host;
}
inline void clearRam() {
    for (auto &r : state.ram) std::free(r.host);
    state.ram.clear();
}
}

class OSObject {
public:
    int refs {1};
    OSObject() { ++OpRegionShim::state.liveObjects; }
    virtual ~OSObject() { --OpRegionShim::state.liveObjects; }
    void retain() { ++refs; }
    void release() {
        if (refs <= 0) std::abort();
        if (!--refs) free();
    }
    virtual void free() { delete this; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSDeclareDefaultStructors(name) static_assert(true, "")
#define OSDefineMetaClassAndStructors(name, base) static_assert(true, "")

class IOWorkLoop;
class IOEventSource : public OSObject {
public:
    IOWorkLoop *workLoop {};
    OSObject *owner {};
};
class IOWorkLoop : public OSObject {
public:
    int gateDepth {};
    std::vector<IOEventSource *> sources;
    static IOWorkLoop *workLoop() { return new IOWorkLoop; }
    IOReturn addEventSource(IOEventSource *s) {
        s->retain(); s->workLoop = this; sources.push_back(s); return kIOReturnSuccess;
    }
    IOReturn removeEventSource(IOEventSource *s) {
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sources[i] != s) continue;
            sources.erase(sources.begin() + long(i)); s->workLoop = nullptr; s->release();
            return kIOReturnSuccess;
        }
        return kIOReturnBadArgument;
    }
    bool inGate() const { return gateDepth > 0; }
};
class IOCommandGate : public IOEventSource {
public:
    typedef IOReturn (*Action)(OSObject *, void *, void *, void *, void *);
    static IOCommandGate *commandGate(OSObject *owner, Action = nullptr) {
        auto *g = new IOCommandGate; g->owner = owner; return g;
    }
    // Recursive closed gate on one thread, as IOWorkLoop's recursive lock.
    IOReturn runAction(Action action, void *a0 = nullptr, void *a1 = nullptr, void *a2 = nullptr,
                       void *a3 = nullptr) {
        if (!action) return kIOReturnBadArgument;
        if (!workLoop) return kIOReturnNotPermitted;
        IOWorkLoop *loop = workLoop;
        ++loop->gateDepth;
        const IOReturn r = action(owner, a0, a1, a2, a3);
        --loop->gateDepth;
        return r;
    }
    int commandSleep(void *, AbsoluteTime, UInt32) { return THREAD_TIMED_OUT; }
    void commandWakeup(void *, bool = false) {}
};
class IOTimerEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *, IOTimerEventSource *);
    Action action {};
    bool armed {};
    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action) {
        auto *t = new IOTimerEventSource; t->owner = owner; t->action = action; return t;
    }
    IOReturn setTimeoutUS(UInt32) { armed = true; return kIOReturnSuccess; }
    void cancelTimeout() { armed = false; }
};

struct IOPMPowerState {
    unsigned long version;
    IOPMPowerFlags capabilityFlags, outputPowerCharacter, inputPowerRequirement;
    unsigned long staticPower, stateOrder, powerToAttain, timeToAttain, settleUpTime, timeToLower,
        settleDownTime, powerDomainBudget;
};

class IOService : public OSObject {
public:
    IOService *provider {};
    IOService *openedBy {};
    bool inactive {};
    uint64_t registryId {};
    virtual bool start(IOService *p) { provider = p; return true; }
    virtual void stop(IOService *) {}
    virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
    virtual IOReturn setPowerState(unsigned long, IOService *) { return kIOPMAckImplied; }
    virtual IOReturn message(UInt32, IOService *, void * = nullptr) { return kIOReturnSuccess; }
    virtual bool willTerminate(IOService *, IOOptionBits) { return true; }
    virtual bool didTerminate(IOService *, IOOptionBits, bool *) { return true; }
    virtual bool open(IOService *client, IOOptionBits = 0, void * = nullptr) {
        if (openedBy && openedBy != client) return false;
        openedBy = client; return true;
    }
    virtual void close(IOService *client, IOOptionBits = 0) { if (openedBy == client) openedBy = nullptr; }
    virtual bool isOpen(const IOService *client = nullptr) const {
        return client ? openedBy == client : openedBy != nullptr;
    }
    IOService *getProvider() const { return provider; }
    bool isInactive() const { return inactive; }
    uint64_t getRegistryEntryID() const { return registryId; }
    void PMinit() {}
    void PMstop() {}
    void joinPMtree(IOService *) {}
    IOReturn registerPowerDriver(IOService *, IOPMPowerState *, unsigned long) { return kIOReturnSuccess; }
    IOReturn changePowerStateTo(unsigned long) { return kIOReturnSuccess; }
    IOReturn acknowledgeSetPowerState() { return kIOReturnSuccess; }
};

class IOMapper : public OSObject {};

class IOPCIDevice : public IOService {
public:
    uint8_t config[256] {};
    uint8_t function {};
    unsigned configWrites {}, busLeadCalls {};
    unsigned reads[256] {};
    UInt32 configRead32(UInt8 o) {
        const UInt32 v = UInt32(config[o]) | UInt32(config[o + 1]) << 8 | UInt32(config[o + 2]) << 16 |
            UInt32(config[o + 3]) << 24;
        touched(o);
        return v;
    }
    UInt16 configRead16(UInt8 o) {
        const UInt16 v = UInt16(config[o] | config[o + 1] << 8);
        touched(o);
        return v;
    }
    UInt8 configRead8(UInt8 o) { const UInt8 v = config[o]; touched(o); return v; }
    UInt8 getFunctionNumber() { return function; }
    bool setBusLeadEnable(bool enable) {
        ++busLeadCalls;
        if (enable) config[4] |= kIOPCICommandBusLead; else config[4] &= uint8_t(~kIOPCICommandBusLead);
        return true;
    }
    void put32(uint8_t o, uint32_t v) { for (int i = 0; i < 4; ++i) config[o + i] = uint8_t(v >> (8 * i)); }
    void put16(uint8_t o, uint16_t v) { config[o] = uint8_t(v); config[o + 1] = uint8_t(v >> 8); }
private:
    void touched(UInt8 o) {
        ++reads[o];
        if (OpRegionShim::state.onConfigRead) OpRegionShim::state.onConfigRead(this, o);
    }
};

class IOMemoryMap : public OSObject {
public:
    IOMemoryDescriptor *memory {};
    uint64_t physical {}, length {};
    IOOptionBits options {};
    IOVirtualAddress address {};
    uint8_t *scratch {};
    bool mapped {};
    IOMemoryMap() { ++OpRegionShim::state.liveMaps; }
    ~IOMemoryMap() override { --OpRegionShim::state.liveMaps; }
    IOVirtualAddress getVirtualAddress() { return address; }
    IOByteCount getLength() { return IOByteCount(int64_t(length) + OpRegionShim::state.faults.lengthSkew); }
    IOPhysicalAddress getPhysicalAddress() {
        return IOPhysicalAddress(int64_t(physical) + OpRegionShim::state.faults.physicalSkew);
    }
    IOOptionBits getMapOptions() {
        return OpRegionShim::state.faults.dropReadOnly ? options & ~kIOMapReadOnly : options;
    }
    IOReturn unmap() {
        auto &s = OpRegionShim::state;
        if (s.unmapCalls++ == s.faults.unmapAt) return kIOReturnError;  // mapping stays
        if (mapped) { mapped = false; --s.mappedMaps; }
        address = 0;
        return kIOReturnSuccess;
    }
    void free() override;
};

class IOMemoryDescriptor : public OSObject {
public:
    uint64_t address {}, length {};
    IOOptionBits options {};
    IOMemoryDescriptor() { ++OpRegionShim::state.liveDescriptors; }
    ~IOMemoryDescriptor() override { --OpRegionShim::state.liveDescriptors; }
    static IOMemoryDescriptor *withAddressRange(mach_vm_address_t address, mach_vm_size_t length,
                                                IOOptionBits options, task_t task) {
        auto &s = OpRegionShim::state;
        s.descriptors.push_back({address, length, options, task});
        if (s.descriptorCalls++ == s.faults.descriptorAt) return nullptr;
        auto *d = new IOMemoryDescriptor;
        d->address = address; d->length = length; d->options = options;
        return d;
    }
    IOMemoryMap *createMappingInTask(task_t task, mach_vm_address_t at, IOOptionBits options,
                                     mach_vm_size_t offset = 0, mach_vm_size_t length = 0) {
        auto &s = OpRegionShim::state;
        s.mappings.push_back({task, at, options, offset, length});
        if (s.mappingCalls++ == s.faults.mappingAt) return nullptr;
        if (!length) length = this->length;
        if (offset > this->length || length > this->length - offset) return nullptr;
        auto *m = new IOMemoryMap;
        m->memory = this; retain();
        m->physical = address + offset; m->length = length; m->options = options;
        uint8_t *host = OpRegionShim::ramAt(m->physical, length);
        if (!host) {
            // Unbacked physical range: reads return all-ones, as a master abort.
            const uint64_t page = m->physical & 0xfff;
            m->scratch = static_cast<uint8_t *>(std::aligned_alloc(4096, (page + length + 4095) & ~uint64_t(4095)));
            std::memset(m->scratch, 0xff, (page + length + 4095) & ~uint64_t(4095));
            host = m->scratch + page;
        }
        m->address = reinterpret_cast<IOVirtualAddress>(host) + (s.faults.misalignVirtual ? 1 : 0);
        m->mapped = true; ++s.mappedMaps;
        return m;
    }
};

inline void IOMemoryMap::free() {
    auto &s = OpRegionShim::state;
    // IOMemoryMap::free unmaps a live mapping itself (IOMemoryDescriptor.cpp).
    if (mapped) { ++s.releasedWhileMapped; mapped = false; --s.mappedMaps; }
    if (scratch) std::free(scratch);
    if (memory) memory->release();
    OSObject::free();
}

inline void *IOMallocData(size_t size) {
    auto &s = OpRegionShim::state;
    if (s.mallocCalls++ == s.faults.mallocAt) return nullptr;
    void *p = std::malloc(size ? size : 1);
    std::memset(p, 0xa5, size);
    s.blocks[p] = size;
    return p;
}
inline void IOFreeData(void *p, size_t size) {
    auto &s = OpRegionShim::state;
    auto found = s.blocks.find(p);
    // IOFreeData requires the exact allocation size.
    if (found == s.blocks.end() || found->second != size) std::abort();
    s.blocks.erase(found);
    ++s.freeCalls;
    std::free(p);
}

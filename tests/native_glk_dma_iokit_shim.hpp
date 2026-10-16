// Host-only OS lifetime boundary for the unchanged GLK DMA adapter.
// HOSTED INJECTED TRANSPORT: no physical DMA, IOMMU, cache line, barrier,
// provider lock, PCI configuration access or GPU operation is performed or
// modeled. Passing tests here are never evidence of hardware acceleration.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#if defined(_WIN32)
#include <malloc.h>
inline void bzero(void *address, size_t bytes) { std::memset(address, 0, bytes); }
#else
#include <strings.h>
#endif
#include <vector>
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using mach_vm_size_t = uint64_t;
using mach_vm_address_t = uint64_t;
using IOReturn = int;
using IODirection = unsigned;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnError = -2,
    kIOReturnUnderrun = -3;
constexpr IODirection kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
constexpr IOOptionBits kIOMemoryMapperNone = 0x800;
constexpr IOOptionBits kIOMemoryIncoherentIOFlush = 1, kIOMemoryIncoherentIOStore = 2;
constexpr unsigned kIOPCIConfigVendorID = 0x00, kIOPCIConfigRevisionID = 0x08,
    kIOPCIConfigSubSystemVendorID = 0x2c;
inline task_t kernel_task = reinterpret_cast<void *>(uintptr_t(1));

class OSObject;
class IOMapper;
class IOBufferMemoryDescriptor;
class IODMACommand;
namespace GlkDmaShim {
enum class Kind { Service, Mapper, Buffer, Command, Other };
struct Faults {
    unsigned mallocFailureCall {}, mallocCalls {};
    bool bufferFactory {}, noCpu {}, descriptorPrepare {}, descriptorPrepareAcquires {true};
    bool descriptorComplete {}, descriptorCompleteConsumes {true};
    bool commandFactory {}, setDescriptor {}, setDescriptorAcquires {true};
    bool commandPrepare {}, commandPrepareAcquires {true};
    bool commandComplete {}, commandCompleteConsumes {true}, clearDescriptor {}, synchronize {};
    bool store {}, flush {};
    void *syncContext {};
    void (*afterSync)(void *, IOOptionBits) {};
    void (*afterStore)(void *) {};
    void (*afterDescriptorPrepare)() {};
    void (*beforeSpecification)() {};
    void (*afterSpecification)() {};
    void (*afterSetDescriptor)() {};
    void (*afterCommandPrepare)() {};
    int segmentError {-1}, segmentCount {-1}, segmentLength {-1}, segmentOffset {-1},
        segmentAlignment {-1}, segmentWidth {-1}, physicalShort {-1}, physicalHigh {-1},
        translatedBounce {-1};
    bool copyDescriptor {};
};
inline Faults faults;
inline std::set<OSObject *> objects;
inline std::map<void *, size_t> allocations;
inline std::vector<std::string> events;
inline bool forcingCleanup {};
inline unsigned unsafeClearCalls {}, forbiddenMapperLookups {}, systemMapperFallbacks {},
    commandCompleteCalls {}, descriptorCompleteCalls {}, syncCalls {}, specCalls {},
    stores {}, flushes {}, bufferFactories {}, prematureDestroy {};
inline unsigned systemMapperChecks {}, pendingMapperWaits {}, nativeBypassRejections {},
    commandPrepareCalls {}, descriptorSetCalls {};
inline uint64_t lastMask {};
inline unsigned lastMappingOption {~0u};
inline IOMapper *lastSpecMapper {};
inline IOBufferMemoryDescriptor *lastBuffer {};
inline IODMACommand *lastCommand {};
inline void reset() {
    faults = {}; events.clear();
    unsafeClearCalls = forbiddenMapperLookups = systemMapperFallbacks = 0;
    commandCompleteCalls = descriptorCompleteCalls = syncCalls = specCalls = 0;
    stores = flushes = bufferFactories = prematureDestroy = 0;
    systemMapperChecks = pendingMapperWaits = nativeBypassRejections = 0;
    commandPrepareCalls = descriptorSetCalls = 0;
    lastMask = 0; lastMappingOption = ~0u; lastSpecMapper = nullptr;
    lastBuffer = nullptr; lastCommand = nullptr;
}
inline void event(const char *name) { events.emplace_back(name); }
inline size_t first(const char *name, size_t from = 0) {
    for (size_t i = from; i < events.size(); ++i) if (events[i] == name) return i;
    return events.size();
}
inline bool ordered(const char *a, const char *b) {
    const size_t x = first(a), y = first(b);
    return x < y && y < events.size();
}
// Exact contiguous event subsequence starting at the first occurrence of the
// first name at or after index from.
inline bool sequence(std::initializer_list<const char *> names, size_t from = 0) {
    size_t at = first(*names.begin(), from);
    for (const char *name : names) {
        if (at >= events.size() || events[at] != name) return false;
        ++at;
    }
    return true;
}
inline uint64_t physical(int index) {
    if (index == faults.physicalHigh) return (1ULL << 39) + uint64_t(index) * 8192;
    return 0x100000 + uint64_t(index) * 8192;
}
void disposeQuarantinedHostWorld();
}
class OSObject {
public:
    unsigned refs {1};
    GlkDmaShim::Kind kind;
    explicit OSObject(GlkDmaShim::Kind k = GlkDmaShim::Kind::Other) : kind(k) {
        GlkDmaShim::objects.insert(this);
    }
    virtual ~OSObject() { GlkDmaShim::objects.erase(this); }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) delete this; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
class IOService : public OSObject {
public:
    IOService *provider {};
    bool inactive {};
    std::map<std::string, OSObject *> properties;
    IOService() : OSObject(GlkDmaShim::Kind::Service) {}
    ~IOService() override { for (auto &entry : properties) entry.second->release(); }
    bool isInactive() const { return inactive; }
    IOService *getProvider() const { return provider; }
    OSObject *copyProperty(const char *key) {
        auto found = properties.find(key);
        if (found == properties.end()) return nullptr;
        found->second->retain(); return found->second;
    }
    void fixtureProperty(const char *key, OSObject *value) {
        auto found = properties.find(key);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
        if (value) { value->retain(); properties.emplace(key, value); }
    }
};
class IOPCIDevice : public IOService {
public:
    // Lenovo 15IGL7 identity tuple as plain fixture data, not a hardware read.
    uint32_t physicalId {0x31858086}, subsystemId {0x3e7717aa};
    uint8_t revision {0x06};
    uint64_t registryId {4294967813ULL};
    IOService *openOwner {};
    unsigned configReads {};
    uint32_t configRead32(IOByteCount offset) {
        ++configReads;
        if (offset == kIOPCIConfigVendorID) return physicalId;
        if (offset == kIOPCIConfigSubSystemVendorID) return subsystemId;
        std::abort();
    }
    uint8_t configRead8(IOByteCount offset) {
        ++configReads; if (offset != kIOPCIConfigRevisionID) std::abort(); return revision;
    }
    uint64_t getRegistryEntryID() const { return registryId; }
    bool isOpen(const IOService *owner) const { return owner && owner == openOwner; }
};
class IOMapper : public IOService {
public:
    static IOMapper *gSystem;
    std::map<uint64_t, uint64_t> translations;
    IOMapper() { kind = GlkDmaShim::Kind::Mapper; }
    uint64_t mapToPhysicalAddress(uint64_t address) {
        GlkDmaShim::event("mapper.translate");
        auto found = translations.find(address);
        if (found == translations.end()) return 0;
        const int index = static_cast<int>((address - 0x800000) / 4096);
        return found->second + (index == GlkDmaShim::faults.translatedBounce ? 4096 : 0);
    }
    static IOMapper *copyMapperForDevice(IOService *) {
        ++GlkDmaShim::forbiddenMapperLookups; return nullptr;
    }
};
inline IOMapper *IOMapper::gSystem = nullptr;
class IOMemoryDescriptor : public OSObject {
public:
    IOMemoryDescriptor() : OSObject(GlkDmaShim::Kind::Buffer) {}
    virtual uint64_t getPhysicalSegment(IOByteCount, IOByteCount *, IOOptionBits) = 0;
    virtual IOReturn performOperation(IOOptionBits, IOByteCount, IOByteCount) = 0;
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    uint8_t *cpu {};
    uint64_t bytes {};
    bool prepared {};
    explicit IOBufferMemoryDescriptor(uint64_t length) : bytes(length) {
#if defined(_WIN32)
        cpu = static_cast<uint8_t *>(_aligned_malloc(static_cast<size_t>(bytes), 4096));
#else
        cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, static_cast<size_t>(bytes)));
#endif
        if (!cpu) std::abort();
        std::memset(cpu, 0xa5, static_cast<size_t>(bytes));
    }
    ~IOBufferMemoryDescriptor() override {
        if (prepared && !GlkDmaShim::forcingCleanup) ++GlkDmaShim::prematureDestroy;
        if (GlkDmaShim::lastBuffer == this) GlkDmaShim::lastBuffer = nullptr;
        GlkDmaShim::event("memory.destroy");
#if defined(_WIN32)
        _aligned_free(cpu);
#else
        std::free(cpu);
#endif
    }
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t task, IOOptionBits options,
            mach_vm_size_t length, mach_vm_address_t mask) {
        if (task != kernel_task || options != kIODirectionInOut || !length || (length & 4095)) std::abort();
        ++GlkDmaShim::bufferFactories; GlkDmaShim::lastMask = mask;
        GlkDmaShim::event("memory.factory");
        if (GlkDmaShim::faults.bufferFactory) return nullptr;
        auto *memory = new IOBufferMemoryDescriptor(length);
        GlkDmaShim::lastBuffer = memory; return memory;
    }
    void *getBytesNoCopy() const { return GlkDmaShim::faults.noCpu ? nullptr : cpu; }
    uint64_t getLength() const { return bytes; }
    IOReturn prepare(IODirection direction) {
        if (direction != kIODirectionInOut) std::abort();
        GlkDmaShim::event("memory.prepare");
        prepared = !GlkDmaShim::faults.descriptorPrepare || GlkDmaShim::faults.descriptorPrepareAcquires;
        if (GlkDmaShim::faults.afterDescriptorPrepare) GlkDmaShim::faults.afterDescriptorPrepare();
        return GlkDmaShim::faults.descriptorPrepare ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn complete(IODirection direction) {
        if (direction != kIODirectionInOut) std::abort();
        ++GlkDmaShim::descriptorCompleteCalls; GlkDmaShim::event("memory.complete");
        if (!prepared) return kIOReturnNotReady;
        if (GlkDmaShim::faults.descriptorComplete) {
            if (GlkDmaShim::faults.descriptorCompleteConsumes) prepared = false;
            return kIOReturnError;
        }
        prepared = false; return kIOReturnSuccess;
    }
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, IOOptionBits options) override {
        if (options != kIOMemoryMapperNone || !prepared || offset >= bytes) return 0;
        const auto index = static_cast<int>(offset / 4096);
        if (length) *length = index == GlkDmaShim::faults.physicalShort ? 2048 : 4096;
        return GlkDmaShim::physical(index);
    }
    // Records the requested whole-range cache operation; touches no cache.
    IOReturn performOperation(IOOptionBits options, IOByteCount offset, IOByteCount length) override {
        if (offset || length != bytes) std::abort();
        if (options == kIOMemoryIncoherentIOStore) {
            ++GlkDmaShim::stores; GlkDmaShim::event("md.store");
            if (GlkDmaShim::faults.afterStore) GlkDmaShim::faults.afterStore(GlkDmaShim::faults.syncContext);
            return prepared && !GlkDmaShim::faults.store ? kIOReturnSuccess : kIOReturnUnderrun;
        }
        if (options == kIOMemoryIncoherentIOFlush) {
            ++GlkDmaShim::flushes; GlkDmaShim::event("md.flush");
            return prepared && !GlkDmaShim::faults.flush ? kIOReturnSuccess : kIOReturnUnderrun;
        }
        std::abort();
    }
};
class IODMACommand : public OSObject {
public:
    struct Segment64 { uint64_t fIOVMAddr {}, fLength {}; };
    enum MappingOptions { kMapped = 0, kBypassed = 1, kNonCoherent = 2, kUnmapped = 3 };
    static constexpr int OutputHost64 = 64;
    IOMapper *mapper {};
    IOBufferMemoryDescriptor *memory {};
    bool active {};
    uint8_t bits {};
    uint64_t capacity {};
    explicit IODMACommand(IOMapper *m, uint8_t b, uint64_t n)
        : OSObject(GlkDmaShim::Kind::Command), mapper(m), bits(b), capacity(n) {
        if (mapper) mapper->retain();
    }
    ~IODMACommand() override {
        if ((active || memory) && !GlkDmaShim::forcingCleanup) ++GlkDmaShim::prematureDestroy;
        if (memory) memory->release();
        if (mapper) mapper->release();
        GlkDmaShim::event("dma.destroy");
        if (GlkDmaShim::lastCommand == this) GlkDmaShim::lastCommand = nullptr;
    }
    static IODMACommand *withSpecification(int output, UInt8 bits, UInt64 segmentSize,
            MappingOptions options, UInt64 bytes, UInt32 alignment, IOMapper *mapper) {
        ++GlkDmaShim::specCalls; GlkDmaShim::event("dma.factory");
        GlkDmaShim::lastMappingOption = options; GlkDmaShim::lastSpecMapper = mapper;
        if (output != OutputHost64 || segmentSize != 4096 || alignment != 4096 ||
            (options != kMapped && options != kUnmapped && options != kBypassed)) std::abort();
        if (GlkDmaShim::faults.beforeSpecification) GlkDmaShim::faults.beforeSpecification();
        // Exact XNU43a908 IODMACommand::setSpecification native semantics:
        // kUnmapped skips selection. Every other null-mapper request checks
        // pending system state, then selects gSystem; kBypassed rejects a
        // selected mapper. The pending branch records, never really waits.
        if (!mapper && options != kUnmapped) {
            ++GlkDmaShim::systemMapperChecks;
            if (reinterpret_cast<uintptr_t>(IOMapper::gSystem) & 3) {
                ++GlkDmaShim::pendingMapperWaits; return nullptr;
            }
            mapper = IOMapper::gSystem;
            if (mapper && options != kBypassed) ++GlkDmaShim::systemMapperFallbacks;
        }
        if (options == kBypassed && mapper) {
            ++GlkDmaShim::nativeBypassRejections; return nullptr;
        }
        if (GlkDmaShim::faults.commandFactory) return nullptr;
        auto *command = new IODMACommand(mapper, bits, bytes);
        GlkDmaShim::lastCommand = command;
        if (GlkDmaShim::faults.afterSpecification) GlkDmaShim::faults.afterSpecification();
        return command;
    }
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *descriptor, bool autoPrepare) {
        if (autoPrepare || memory || active) std::abort();
        ++GlkDmaShim::descriptorSetCalls;
        GlkDmaShim::event("dma.set");
        if (!GlkDmaShim::faults.setDescriptor || GlkDmaShim::faults.setDescriptorAcquires) {
            memory = dynamic_cast<IOBufferMemoryDescriptor *>(const_cast<IOMemoryDescriptor *>(descriptor));
            if (!memory) std::abort();
            memory->retain();
        }
        if (GlkDmaShim::faults.afterSetDescriptor) GlkDmaShim::faults.afterSetDescriptor();
        return GlkDmaShim::faults.setDescriptor ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn prepare(UInt64 offset, UInt64 length) {
        GlkDmaShim::event("dma.prepare");
        ++GlkDmaShim::commandPrepareCalls;
        if (!memory || !memory->prepared || offset || length != capacity) std::abort();
        active = !GlkDmaShim::faults.commandPrepare || GlkDmaShim::faults.commandPrepareAcquires;
        if (GlkDmaShim::faults.afterCommandPrepare) GlkDmaShim::faults.afterCommandPrepare();
        return GlkDmaShim::faults.commandPrepare ? kIOReturnError : kIOReturnSuccess;
    }
    IOReturn complete(bool invalidate, bool synchronize) {
        if (!invalidate || !synchronize) std::abort();
        ++GlkDmaShim::commandCompleteCalls; GlkDmaShim::event("dma.complete");
        if (!active) return kIOReturnNotReady;
        if (GlkDmaShim::faults.commandComplete) {
            if (GlkDmaShim::faults.commandCompleteConsumes) active = false;
            return kIOReturnError;
        }
        active = false; return kIOReturnSuccess;
    }
    IOReturn clearMemoryDescriptor(bool autoComplete) {
        GlkDmaShim::event("dma.clear");
        if (autoComplete) {
            ++GlkDmaShim::unsafeClearCalls;
            if (active) (void)complete(true, true); // XNU discards this result.
        } else if (active) return kIOReturnNotReady;
        if (GlkDmaShim::faults.clearDescriptor) return kIOReturnError;
        if (memory) { memory->release(); memory = nullptr; }
        return kIOReturnSuccess;
    }
    const IOMemoryDescriptor *getMemoryDescriptor() const { return memory; }
    IOMemoryDescriptor *getIOMemoryDescriptor() const {
        return GlkDmaShim::faults.copyDescriptor ? reinterpret_cast<IOMemoryDescriptor *>(uintptr_t(1)) : memory;
    }
    IOReturn synchronize(IOOptionBits direction) {
        ++GlkDmaShim::syncCalls;
        GlkDmaShim::event(direction == kIODirectionOut ? "dma.sync.out" : "dma.sync.in");
        if (direction != kIODirectionIn && direction != kIODirectionOut) std::abort();
        if (!active || GlkDmaShim::faults.synchronize) return kIOReturnError;
        if (GlkDmaShim::faults.afterSync)
            GlkDmaShim::faults.afterSync(GlkDmaShim::faults.syncContext, direction);
        return kIOReturnSuccess;
    }
    IOReturn gen64IOVMSegments(UInt64 *offset, Segment64 *segment, UInt32 *count) {
        if (!active || !offset || !segment || !count || *count != 1) std::abort();
        GlkDmaShim::event("dma.segment");
        const int index = static_cast<int>(*offset / 4096);
        if (index == GlkDmaShim::faults.segmentError) return kIOReturnError;
        const uint64_t original = GlkDmaShim::physical(index);
        if (mapper) {
            segment->fIOVMAddr = 0x800000 + uint64_t(index) * 4096;
        } else {
            // Unmapped: physical page, or a different page for a modeled bounce.
            segment->fIOVMAddr = index == GlkDmaShim::faults.translatedBounce
                ? 0x7000000 + uint64_t(index) * 4096 : original;
        }
        segment->fLength = index == GlkDmaShim::faults.segmentLength ? 2048 : 4096;
        *count = index == GlkDmaShim::faults.segmentCount ? 0 : 1;
        *offset += index == GlkDmaShim::faults.segmentOffset ? 2048 : 4096;
        if (index == GlkDmaShim::faults.segmentAlignment) ++segment->fIOVMAddr;
        if (index == GlkDmaShim::faults.segmentWidth) segment->fIOVMAddr = 1ULL << bits;
        if (mapper) mapper->translations[segment->fIOVMAddr] = original;
        return kIOReturnSuccess;
    }
};
inline void *IOMalloc(size_t size) {
    ++GlkDmaShim::faults.mallocCalls;
    if (GlkDmaShim::faults.mallocFailureCall == GlkDmaShim::faults.mallocCalls) return nullptr;
    void *result = std::malloc(size); if (result) GlkDmaShim::allocations.emplace(result, size);
    return result;
}
inline void IOFree(void *address, size_t size) {
    auto found = GlkDmaShim::allocations.find(address);
    if (found == GlkDmaShim::allocations.end() || found->second != size) std::abort();
    GlkDmaShim::allocations.erase(found); std::free(address);
}
namespace GlkDmaShim {
inline void disposeQuarantinedHostWorld() {
    // Test teardown only, after all assertions and adapter lifetimes. This
    // neither invokes production recovery nor implies DMA/IOMMU retirement.
    forcingCleanup = true;
    for (Kind kind : {Kind::Command, Kind::Buffer, Kind::Service, Kind::Mapper, Kind::Other}) {
        for (;;) {
            OSObject *next = nullptr;
            for (auto *object : objects) if (object->kind == kind) { next = object; break; }
            if (!next) break;
            delete next;
        }
    }
    for (auto &allocation : allocations) std::free(allocation.first);
    allocations.clear(); forcingCleanup = false;
}
}

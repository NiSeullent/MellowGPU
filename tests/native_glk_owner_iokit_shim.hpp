// Host-only OS boundary for the unchanged GLK physical owner and the seeded
// GlkDmaMemoryIOKit/MemoryOwner it composes.
// HOSTED INJECTED TRANSPORT: no PCI configuration access, power transition,
// DMA, cache line, MMIO, interrupt or GPU operation happens or is modeled
// beyond the plain fixture fields below. The gate, command sleep and wakeup
// use real host threads and locks so serialization and asynchronous drain
// run for real; XNU semantics reproduced here are cited in the provenance
// file and are not Apple code. Passing tests are never hardware evidence.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

#if !defined(MELLOW_GLK_OWNER_HOSTED)
#error "hosted shim only"
#endif

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
using IODirection = unsigned;
using AbsoluteTime = uint64_t;
using IOPMPowerFlags = unsigned long;
using task_t = void *;
using mach_vm_size_t = uint64_t;
using mach_vm_address_t = uint64_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnError = -2,
    kIOReturnBadArgument = -3, kIOReturnNotPermitted = -4, kIOReturnUnsupported = -5,
    kIOReturnNoResources = -6;
constexpr IOReturn kIOPMAckImplied = 0;
constexpr unsigned long kIOPMPowerOn = 0x2, kIOPMDeviceUsable = 0x8000, kIOPMPowerStateVersion1 = 1;
constexpr UInt32 kIOMessageServiceIsTerminated = 0xe0000010, kIOMessageServiceIsRequestingClose = 0xe0000100;
constexpr int THREAD_AWAKENED = 0, THREAD_TIMED_OUT = 1;
constexpr UInt32 THREAD_UNINT = 0, kMicrosecondScale = 1000;
constexpr unsigned kIOPCIConfigVendorID = 0x00, kIOPCIConfigCommand = 0x04, kIOPCIConfigStatus = 0x06,
    kIOPCIConfigRevisionID = 0x08, kIOPCIConfigSubSystemVendorID = 0x2c;
constexpr uint16_t kIOPCICommandMemorySpace = 0x2, kIOPCICommandBusLead = 0x4;
constexpr IODirection kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
constexpr IOOptionBits kIOMemoryMapperNone = 0x800, kIOMemoryIncoherentIOFlush = 1, kIOMemoryIncoherentIOStore = 2;
inline task_t kernel_task = reinterpret_cast<void *>(uintptr_t(1));
// The kernel exports this read-only; tests change it to model other kernels.
inline int version_major = 24;

struct IOPMPowerState {
    unsigned long version;
    IOPMPowerFlags capabilityFlags, outputPowerCharacter, inputPowerRequirement;
    unsigned long staticPower, stateOrder, powerToAttain, timeToAttain, settleUpTime,
        timeToLower, settleDownTime, powerDomainBudget;
};

class OSObject;
namespace GlkOwnerShim {
struct Faults {
    bool openFails {}, workLoopFactory {}, gateFactory {}, timerFactory {}, timerArmFails {};
    unsigned addSourceFailAt {};
    IOReturn registerPower {kIOReturnSuccess};
};
inline Faults faults;
inline std::mutex eventLock;
inline std::vector<std::string> events;
inline std::set<OSObject *> objects;
inline std::map<void *, size_t> allocations;
inline unsigned addSourceCalls {};
inline uint64_t nextPhysical {0x100000};
inline thread_local unsigned gateDepth {};
inline bool disposing {};
inline void event(const std::string &name) {
    std::lock_guard<std::mutex> hold(eventLock);
    events.push_back(name);
}
inline size_t count(const std::string &name) {
    std::lock_guard<std::mutex> hold(eventLock);
    size_t n = 0;
    for (auto &e : events) n += e == name;
    return n;
}
inline uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void reset() {
    faults = {}; addSourceCalls = 0; nextPhysical = 0x100000; version_major = 24;
    std::lock_guard<std::mutex> hold(eventLock);
    events.clear();
}
void disposeAll();
}

class OSObject {
public:
    std::atomic<unsigned> refs {1};
    OSObject() { GlkOwnerShim::objects.insert(this); }
    virtual ~OSObject() { GlkOwnerShim::objects.erase(this); }
    void retain() { ++refs; }
    void release() { if (!refs) std::abort(); if (!--refs) free(); }
    virtual void free() { delete this; }
};
class OSDictionary;
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSDeclareDefaultStructors(className) public: className(); protected: ~className() override
#define OSDefineMetaClassAndStructors(className, superName) \
    className::className() {} className::~className() {}

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *result) {
    *result = GlkOwnerShim::nowNs() + uint64_t(interval) * scale;
}

class IOWorkLoop;
class IOService : public OSObject {
public:
    static inline uint64_t nextRegistryId {0x100000100ULL};
    uint64_t registryId {nextRegistryId++};
    IOService *provider {}, *openClient {};
    bool inactive {}, termPhase3 {};
    std::map<std::string, OSObject *> properties;
    // Power-management bookkeeping (PMinit .. acknowledgeSetPowerState).
    unsigned pmInits {}, pmStops {}, pmStopsInGate {}, stopsScheduled {}, closes {};
    std::atomic<unsigned> acks {}, acksInGate {};
    IOService *joinedDriver {}, *powerDriver {};
    IOPMPowerState states[4] {};
    unsigned long stateCount {};
    long desired {-1};
    ~IOService() override { if (!GlkOwnerShim::disposing) for (auto &entry : properties) entry.second->release(); }
    virtual bool start(IOService *p) { provider = p; return true; }
    virtual void stop(IOService *) { GlkOwnerShim::event("service.stop"); }
    virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
    virtual IOReturn setPowerState(unsigned long, IOService *) { return kIOPMAckImplied; }
    virtual IOReturn message(UInt32, IOService *, void * = nullptr) { return kIOReturnUnsupported; }
    virtual bool willTerminate(IOService *, IOOptionBits) { return true; }
    // XNU 43a908 IOService::didTerminate semantics (IOService.cpp:2958).
    virtual bool didTerminate(IOService *p, IOOptionBits options, bool *defer) {
        if (!*defer) {
            if (!p->handleIsOpen(this)) scheduleStop(p);
            else {
                message(kIOMessageServiceIsRequestingClose, p, reinterpret_cast<void *>(uintptr_t(options)));
                if (!p->handleIsOpen(this)) scheduleStop(p);
            }
        }
        return true;
    }
    virtual bool open(IOService *client, IOOptionBits = 0, void * = nullptr) {
        if (inactive || GlkOwnerShim::faults.openFails || (openClient && openClient != client)) return false;
        openClient = client;
        return true;
    }
    // XNU 43a908 IOService::close semantics (IOService.cpp:3561): after
    // termination phase 3 the last close schedules the client's stop.
    virtual void close(IOService *client, IOOptionBits = 0) {
        const bool was = handleIsOpen(client);
        if (was) { openClient = nullptr; ++closes; GlkOwnerShim::event("provider.close"); }
        if (was && termPhase3) client->scheduleStop(this);
    }
    virtual bool isOpen(const IOService *client = nullptr) const {
        return client ? openClient == client : openClient != nullptr;
    }
    virtual bool handleIsOpen(const IOService *client) const { return client && openClient == client; }
    virtual void joinPMtree(IOService *driver) { joinedDriver = driver; GlkOwnerShim::event("pm.join"); }
    bool isInactive() const { return inactive; }
    IOService *getProvider() const { return provider; }
    uint64_t getRegistryEntryID() const { return registryId; }
    OSObject *copyProperty(const char *key) {
        auto found = properties.find(key);
        if (found == properties.end()) return nullptr;
        found->second->retain();
        return found->second;
    }
    void scheduleStop(IOService *) { ++stopsScheduled; GlkOwnerShim::event("stop.scheduled"); }
    void PMinit() { ++pmInits; GlkOwnerShim::event("pm.init"); }
    void PMstop() { ++pmStops; if (GlkOwnerShim::gateDepth) ++pmStopsInGate; GlkOwnerShim::event("pm.stop"); }
    IOReturn registerPowerDriver(IOService *driver, IOPMPowerState *table, unsigned long n) {
        GlkOwnerShim::event("pm.register");
        powerDriver = driver; stateCount = n;
        for (unsigned long i = 0; i < n && i < 4; ++i) states[i] = table[i];
        return GlkOwnerShim::faults.registerPower;
    }
    IOReturn changePowerStateTo(unsigned long ordinal) { desired = long(ordinal); return kIOReturnSuccess; }
    IOReturn acknowledgeSetPowerState() {
        ++acks; if (GlkOwnerShim::gateDepth) ++acksInGate;
        GlkOwnerShim::event("pm.ack");
        return kIOReturnSuccess;
    }
};

// Configuration space is a plain array; values are fixture data only.
class IOPCIDevice : public IOService {
public:
    uint8_t config[256] {};
    uint8_t function {};
    unsigned configReads {}, configWrites {}, busLeadCalls {};
    bool busLeadIgnored {};
    IOPCIDevice() {
        registryId = 4294967813ULL; // 15IGL7 observation; per-boot in reality
        put32(0x00, 0x31858086); put16(0x04, kIOPCICommandMemorySpace); put16(0x06, 0x0010);
        put32(0x08, 0x03000006); config[0x0e] = 0; put32(0x2c, 0x3e7717aa); config[0x34] = 0x40;
        put16(0x40, 0xac09); put16(0xac, 0xd005); put16(0xd0, 0x0001); put16(0xd4, 0x0000);
    }
    void put16(unsigned at, uint16_t v) { config[at] = uint8_t(v); config[at + 1] = uint8_t(v >> 8); }
    void put32(unsigned at, uint32_t v) { put16(at, uint16_t(v)); put16(at + 2, uint16_t(v >> 16)); }
    uint16_t get16(unsigned at) const { return uint16_t(config[at] | config[at + 1] << 8); }
    UInt32 configRead32(UInt8 at) {
        ++configReads; if (at & 3) std::abort();
        return uint32_t(get16(at)) | uint32_t(get16(at + 2u)) << 16;
    }
    UInt16 configRead16(UInt8 at) { ++configReads; if (at & 1) std::abort(); return get16(at); }
    UInt8 configRead8(UInt8 at) { ++configReads; return config[at]; }
    UInt8 getFunctionNumber() { return function; }
    bool setBusLeadEnable(bool enable) {
        ++busLeadCalls; ++configWrites;
        GlkOwnerShim::event(enable ? "pci.buslead.on" : "pci.buslead.off");
        const bool was = get16(0x04) & kIOPCICommandBusLead;
        if (!busLeadIgnored)
            put16(0x04, uint16_t(enable ? get16(0x04) | kIOPCICommandBusLead : get16(0x04) & ~kIOPCICommandBusLead));
        return was;
    }
};

class IOEventSource : public OSObject {
public:
    IOWorkLoop *workLoop {};
    OSObject *owner {};
};
// One recursive gate per work loop, as IOWorkLoop's IORecursiveLock.
class IOWorkLoop : public OSObject {
public:
    std::recursive_mutex gate;
    std::atomic<std::thread::id> holder {};
    std::atomic<unsigned> depth {};
    std::condition_variable_any wake;
    std::map<void *, uint64_t> generations;
    std::vector<IOEventSource *> sources;
    static IOWorkLoop *workLoop() { return GlkOwnerShim::faults.workLoopFactory ? nullptr : new IOWorkLoop; }
    ~IOWorkLoop() override { if (!GlkOwnerShim::disposing) for (auto *s : sources) s->release(); }
    IOReturn addEventSource(IOEventSource *s) {
        if (++GlkOwnerShim::addSourceCalls == GlkOwnerShim::faults.addSourceFailAt) return kIOReturnError;
        s->retain(); s->workLoop = this; sources.push_back(s);
        return kIOReturnSuccess;
    }
    IOReturn removeEventSource(IOEventSource *s) {
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            if (*it != s) continue;
            sources.erase(it); s->workLoop = nullptr; s->release();
            return kIOReturnSuccess;
        }
        return kIOReturnBadArgument;
    }
    bool inGate() const { return depth && holder.load() == std::this_thread::get_id(); }
    void closeGate() { gate.lock(); holder = std::this_thread::get_id(); ++depth; ++GlkOwnerShim::gateDepth; }
    void openGate() { --GlkOwnerShim::gateDepth; if (!--depth) holder = std::thread::id(); gate.unlock(); }
};
class IOCommandGate : public IOEventSource {
public:
    typedef IOReturn (*Action)(OSObject *, void *, void *, void *, void *);
    unsigned sleeps {}, timeouts {};
    static IOCommandGate *commandGate(OSObject *o, Action = nullptr) {
        if (GlkOwnerShim::faults.gateFactory) return nullptr;
        auto *g = new IOCommandGate; g->owner = o; return g;
    }
    IOReturn runAction(Action action, void *a0 = nullptr, void *a1 = nullptr, void *a2 = nullptr, void *a3 = nullptr) {
        if (!action) return kIOReturnBadArgument;
        if (!workLoop) return kIOReturnNotPermitted;
        IOWorkLoop *loop = workLoop;
        loop->closeGate();
        const IOReturn result = action(owner, a0, a1, a2, a3);
        loop->openGate();
        return result;
    }
    // Releases the gate while sleeping, as IOWorkLoop::sleepGate. A nested
    // hold would sleep with the outer acquisition kept; that aborts here.
    int commandSleep(void *event, AbsoluteTime deadline, UInt32) {
        IOWorkLoop *loop = workLoop;
        if (!loop || !loop->inGate()) return kIOReturnNotPermitted;
        if (loop->depth != 1) std::abort();
        ++sleeps;
        const uint64_t seen = loop->generations[event];
        loop->holder = std::thread::id(); loop->depth = 0; --GlkOwnerShim::gateDepth;
        std::unique_lock<std::recursive_mutex> lock(loop->gate, std::adopt_lock);
        const auto until = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline));
        const bool woke = loop->wake.wait_until(lock, until, [&] { return loop->generations[event] != seen; });
        lock.release();
        loop->holder = std::this_thread::get_id(); loop->depth = 1; ++GlkOwnerShim::gateDepth;
        if (!woke) ++timeouts;
        return woke ? THREAD_AWAKENED : THREAD_TIMED_OUT;
    }
    void commandWakeup(void *event, bool = false) {
        if (!workLoop) return;
        ++workLoop->generations[event];
        workLoop->wake.notify_all();
    }
};
class IOTimerEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *, IOTimerEventSource *);
    Action action {};
    std::atomic<bool> armed {};
    uint32_t armedUs {};
    unsigned sets {}, cancels {};
    static IOTimerEventSource *timerEventSource(OSObject *o, Action a = nullptr) {
        if (GlkOwnerShim::faults.timerFactory) return nullptr;
        auto *t = new IOTimerEventSource; t->owner = o; t->action = a; return t;
    }
    IOReturn setTimeoutUS(UInt32 us) {
        if (!workLoop || GlkOwnerShim::faults.timerArmFails) return kIOReturnNoResources;
        armed = true; armedUs = us; ++sets;
        return kIOReturnSuccess;
    }
    void cancelTimeout() { armed = false; ++cancels; }
    // Test hook: the work loop thread delivering an expiry, gate closed.
    bool fire() {
        if (!armed || !workLoop || !action) return false;
        IOWorkLoop *loop = workLoop;
        loop->closeGate(); armed = false; action(owner, this); loop->openGate();
        return true;
    }
};

// Minimal DMA objects for the seeded adapter's direct no-system-mapper route.
class IOMapper : public IOService {
public:
    static inline IOMapper *gSystem {};
    uint64_t mapToPhysicalAddress(uint64_t address) { return address; }
};
class IOMemoryDescriptor : public OSObject {};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    uint8_t *cpu {};
    uint64_t bytes {}, physical {};
    bool prepared {};
    explicit IOBufferMemoryDescriptor(uint64_t n) : bytes(n), physical(GlkOwnerShim::nextPhysical) {
        GlkOwnerShim::nextPhysical += n;
        cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, size_t(n)));
        if (!cpu) std::abort();
        std::memset(cpu, 0xa5, size_t(n));
    }
    ~IOBufferMemoryDescriptor() override { std::free(cpu); }
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t task, IOOptionBits options,
            mach_vm_size_t length, mach_vm_address_t mask) {
        if (task != kernel_task || options != kIODirectionInOut || !length || (length & 4095) || mask >> 39) std::abort();
        return new IOBufferMemoryDescriptor(length);
    }
    void *getBytesNoCopy() const { return cpu; }
    IOReturn prepare(IODirection) { prepared = true; return kIOReturnSuccess; }
    IOReturn complete(IODirection) { if (!prepared) return kIOReturnNotReady; prepared = false; return kIOReturnSuccess; }
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, IOOptionBits options) {
        if (options != kIOMemoryMapperNone || !prepared || offset >= bytes) return 0;
        if (length) *length = 4096;
        return physical + offset;
    }
    IOReturn performOperation(IOOptionBits, IOByteCount offset, IOByteCount length) {
        if (offset || length != bytes) std::abort();
        GlkOwnerShim::event("md.cache");
        return prepared ? kIOReturnSuccess : kIOReturnNotReady;
    }
};
class IODMACommand : public OSObject {
public:
    struct Segment64 { uint64_t fIOVMAddr {}, fLength {}; };
    enum MappingOptions { kMapped = 0, kBypassed = 1, kNonCoherent = 2, kUnmapped = 3 };
    static constexpr int OutputHost64 = 64;
    IOBufferMemoryDescriptor *memory {};
    bool active {};
    ~IODMACommand() override { if (memory && !GlkOwnerShim::disposing) memory->release(); }
    static IODMACommand *withSpecification(int output, UInt8 bits, UInt64 segment, MappingOptions options,
            UInt64, UInt32 alignment, IOMapper *mapper) {
        if (output != OutputHost64 || bits != 39 || segment != 4096 || alignment != 4096) std::abort();
        // kBypassed rejects a selected system mapper (IODMACommand.cpp:326).
        if (options == kBypassed && (mapper || IOMapper::gSystem)) return nullptr;
        return new IODMACommand;
    }
    IOReturn setMemoryDescriptor(const IOMemoryDescriptor *d, bool autoPrepare) {
        if (autoPrepare || memory) std::abort();
        memory = dynamic_cast<IOBufferMemoryDescriptor *>(const_cast<IOMemoryDescriptor *>(d));
        if (!memory) std::abort();
        memory->retain();
        return kIOReturnSuccess;
    }
    IOReturn prepare(UInt64 offset, UInt64 length) {
        if (!memory || !memory->prepared || offset || length != memory->bytes) std::abort();
        active = true; return kIOReturnSuccess;
    }
    IOReturn complete(bool, bool) { if (!active) return kIOReturnNotReady; active = false; return kIOReturnSuccess; }
    IOReturn clearMemoryDescriptor(bool autoComplete) {
        if (autoComplete) std::abort();
        if (active) return kIOReturnNotReady;
        if (memory) { memory->release(); memory = nullptr; }
        return kIOReturnSuccess;
    }
    const IOMemoryDescriptor *getMemoryDescriptor() const { return memory; }
    IOMemoryDescriptor *getIOMemoryDescriptor() const { return memory; }
    IOReturn synchronize(IOOptionBits) { return active ? kIOReturnSuccess : kIOReturnError; }
    IOReturn gen64IOVMSegments(UInt64 *offset, Segment64 *segment, UInt32 *count) {
        if (!active || !offset || !segment || !count || *count != 1 || *offset >= memory->bytes) std::abort();
        segment->fIOVMAddr = memory->physical + *offset; segment->fLength = 4096; *offset += 4096;
        return kIOReturnSuccess;
    }
};
inline void *IOMalloc(size_t size) {
    void *p = std::malloc(size);
    if (p) GlkOwnerShim::allocations.emplace(p, size);
    return p;
}
inline void IOFree(void *p, size_t size) {
    auto found = GlkOwnerShim::allocations.find(p);
    if (found == GlkOwnerShim::allocations.end() || found->second != size) std::abort();
    GlkOwnerShim::allocations.erase(found);
    std::free(p);
}

namespace GlkOwnerShim {
// Test teardown only, after every assertion: frees deliberately retained
// (quarantined) host objects. Not production recovery.
inline void disposeAll() {
    disposing = true;
    while (!objects.empty()) {
        OSObject *next = *objects.begin();
        delete next;
    }
    for (auto &a : allocations) std::free(a.first);
    allocations.clear();
    disposing = false;
}
}

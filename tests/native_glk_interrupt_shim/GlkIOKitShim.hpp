// HOSTED SHIM ONLY: a minimal single-threaded model of the IOKit objects used
// by Mellow/GlkInterruptIOKit.cpp. It models the XNU behaviors the adapter
// relies on (xnu-11417.140.69 IOInterruptEventSource/IOFilterInterruptEvent-
// Source/IOInterruptController/IOWorkLoop): registration on addEventSource,
// unregistration on removeEventSource, disable() as the drain point, filter
// true => producerCount++, and checkForWork capturing producerCount before the
// action. It is not IOKit, not a kernel, and proves nothing about hardware.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

typedef int IOReturn;
typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnError = static_cast<IOReturn>(0xe00002bc);
constexpr IOReturn kIOReturnNoInterrupt = static_cast<IOReturn>(0xe00002e9);
#define kIOInterruptTypeEdge (0)
#define kIOInterruptTypeLevel (1)
enum { kIOInterruptTypePCIMessaged = 0x00010000, kIOInterruptTypePCIMessagedX = 0x00020000 };
enum { kIOPCIConfigVendorID = 0x00, kIOPCIConfigDeviceID = 0x02, kIOPCIConfigCommand = 0x04,
       kIOPCIConfigRevisionID = 0x08, kIOPCIConfigSubSystemVendorID = 0x2c, kIOPCIConfigSubSystemID = 0x2e };
enum { kIOPCIDeviceOffState = 0, kIOPCIDeviceDozeState = 1, kIOPCIDeviceOnState = 2 };

namespace shim {
inline std::vector<std::string> &log() { static std::vector<std::string> events; return events; }
inline void note(const std::string &event) { log().push_back(event); }
inline int &liveObjects() { static int count = 0; return count; }
inline int &freedObjects() { static int count = 0; return count; }
}

class OSObject {
public:
    OSObject() { ++shim::liveObjects(); }
    virtual ~OSObject() { --shim::liveObjects(); }
    virtual bool init() { return true; }
    virtual void free() { ++shim::freedObjects(); delete this; }
    void retain() const { ++refs_; }
    void release() const { if (--refs_ == 0) const_cast<OSObject *>(this)->free(); }
    int retainCount() const { return refs_; }
private:
    mutable int refs_ {1};
};
#define OSDeclareDefaultStructors(name) \
    public: name() = default; protected: ~name() override = default; private: static_assert(true, "")
#define OSDefineMetaClassAndStructors(name, super) static_assert(true, "")
#define OSTypeAlloc(type) (new type)

class IOEventSource;
class IOFilterInterruptEventSource;
class IOService : public OSObject {
public:
    IOService *provider {}, *opener {};
    bool inactive {};
    uint64_t registryId {0x1000};
    unsigned long powerState {kIOPCIDeviceOnState};
    int interruptTypes[4] {};
    int interruptCount {};
    // Interrupt controller vector state per index.
    IOFilterInterruptEventSource *registered[4] {};
    bool vectorEnabled[4] {};
    IOReturn disableResult {kIOReturnSuccess}, enableResult {kIOReturnSuccess};
    IOService *getProvider() const { return provider; }
    bool isOpen(const IOService *client = nullptr) const { return client ? opener == client : opener != nullptr; }
    bool isInactive() const { return inactive; }
    uint64_t getRegistryEntryID() { return registryId; }
    unsigned long getPowerState() { return powerState; }
    IOReturn getInterruptType(int source, int *type) {
        if (source < 0 || source >= interruptCount || !type) return kIOReturnNoInterrupt;
        *type = interruptTypes[source];
        return kIOReturnSuccess;
    }
    IOReturn disableInterrupt(int index) {
        shim::note("provider.disable+drain");
        if (disableResult != kIOReturnSuccess) return disableResult;
        vectorEnabled[index] = false; return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int index) {
        shim::note("provider.enable");
        if (enableResult != kIOReturnSuccess) return enableResult;
        vectorEnabled[index] = true; return kIOReturnSuccess;
    }
};

class IOPCIDevice : public IOService {
public:
    uint8_t config[256] {};
    uint8_t bus {0}, slot {2}, function {0};
    UInt8 configRead8(UInt8 offset) { return config[offset]; }
    UInt16 configRead16(UInt8 offset) { return static_cast<UInt16>(config[offset] | config[offset + 1] << 8); }
    UInt8 getBusNumber() { return bus; }
    UInt8 getDeviceNumber() { return slot; }
    UInt8 getFunctionNumber() { return function; }
    void set16(uint8_t offset, uint16_t value) { config[offset] = uint8_t(value); config[offset + 1] = uint8_t(value >> 8); }
};

class IOWorkLoop;
class IOEventSource : public OSObject {
public:
    IOWorkLoop *workLoop {};
    virtual void setWorkLoop(IOWorkLoop *loop) { workLoop = loop; }
};

class IOInterruptEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *, IOInterruptEventSource *, int);
    OSObject *owner {};
    Action action {};
    IOService *provider {};
    int intIndex {-1};
    unsigned producerCount {}, consumerCount {};
    virtual void enable() {
        shim::note("os.enable");
        if (provider && intIndex >= 0) (void)provider->enableInterrupt(intIndex);
    }
    virtual void disable() {
        // XNU IOInterruptController::disableInterrupt: soft-disable, then spin
        // while the vector's primary handler is active (the drain point).
        shim::note("os.disable+drain");
        if (provider && intIndex >= 0) (void)provider->disableInterrupt(intIndex);
    }
    bool checkForWork() {
        const unsigned cached = producerCount;
        const int count = static_cast<int>(cached - consumerCount);
        if (count > 0) { action(owner, this, count); consumerCount = cached; }
        return producerCount != consumerCount;
    }
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    typedef bool (*Filter)(OSObject *, IOFilterInterruptEventSource *);
    Filter filterAction {};
    static bool &failCreate() { static bool fail = false; return fail; }
    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *owner, Action action,
            Filter filter, IOService *provider, int index = 0) {
        if (failCreate()) return nullptr;
        auto *source = new IOFilterInterruptEventSource;
        source->owner = owner; source->action = action; source->filterAction = filter;
        source->provider = provider; source->intIndex = index;
        // Exact XNU source registers during IOInterruptEventSource::init.
        provider->registered[index] = source; provider->vectorEnabled[index] = false;
        shim::note("os.register");
        return source;
    }
    void free() override {
        if (provider && intIndex >= 0) {
            provider->registered[intIndex] = nullptr; provider->vectorEnabled[intIndex] = false;
            shim::note("os.unregister");
        }
        OSObject::free();
    }
    void setWorkLoop(IOWorkLoop *loop) override {
        if (loop && intIndex < 0) {
            intIndex = ~intIndex;
            provider->registered[intIndex] = this; provider->vectorEnabled[intIndex] = false;
            shim::note("os.register");
        } else if (!loop && intIndex >= 0) {
            provider->vectorEnabled[intIndex] = false; provider->registered[intIndex] = nullptr;
            shim::note("os.unregister");
            intIndex = ~intIndex;
        }
        IOEventSource::setWorkLoop(loop);
    }
    virtual void signalInterrupt() { ++producerCount; }
    // Primary interrupt on this vector (hosted model of the platform dispatch).
    static bool deliver(IOService *provider, int index) {
        auto *source = provider->registered[index];
        if (!source || !provider->vectorEnabled[index]) return false;
        if (source->filterAction(source->owner, source)) { source->signalInterrupt(); return true; }
        return false;
    }
};

class IOWorkLoop : public OSObject {
public:
    bool gate {true};
    IOReturn addResult {kIOReturnSuccess};
    IOReturn removeResult {kIOReturnSuccess};
    std::vector<IOEventSource *> sources;
    bool inGate() const { return gate; }
    IOReturn addEventSource(IOEventSource *source) {
        if (addResult != kIOReturnSuccess) return addResult;
        source->retain(); sources.push_back(source); source->setWorkLoop(this);
        return kIOReturnSuccess;
    }
    IOReturn removeEventSource(IOEventSource *source) {
        if (removeResult != kIOReturnSuccess) return removeResult;
        for (size_t i = 0; i < sources.size(); ++i) if (sources[i] == source) {
            sources.erase(sources.begin() + long(i)); source->setWorkLoop(nullptr); source->release();
            return kIOReturnSuccess;
        }
        return kIOReturnError;
    }
    // Serialized workloop thread: bounded passes over pending interrupt work.
    unsigned run(unsigned limit = 4096) {
        unsigned passes = 0;
        for (bool more = true; more && passes < limit;) {
            more = false;
            for (size_t i = 0; i < sources.size(); ++i) {
                auto *source = static_cast<IOInterruptEventSource *>(sources[i]);
                if (source->producerCount != source->consumerCount) { ++passes; more |= source->checkForWork(); }
            }
        }
        return passes;
    }
};

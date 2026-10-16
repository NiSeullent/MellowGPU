// Host-only OS boundary for the unchanged MellowGlkPassiveProbe source.
// HOSTED INJECTED TRANSPORT: no kernel, IORegistry, PCI configuration space,
// IOMMU, power state or GPU exists here. Configuration "reads" return fixture
// words; gSystem is a fixture word that is never a real mapper. Passing tests
// are never evidence that the probe ran on hardware or that a GPU works.
//
// Deliberately ABSENT so any production use fails to compile: configWrite*,
// configRead8, IOService::open/close, PMinit/joinPMtree/changePowerStateTo,
// setMemoryEnable/setBusMasterEnable/mapDeviceMemory*, IOMapper
// checkForSystemMapper/waitForSystemMapper/copyMapperForDevice*.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using SInt32 = int32_t;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
#define APPLE_KEXT_OVERRIDE override
// SDK15.5 Kernel.framework IOKit/IOMemoryDescriptor.h:104.
enum { kIOMemoryMapperNone = 0x00000800 };
inline int version_major = 24;

class OSObject;
class IOService;
class IOPCIDevice;
namespace GlkPassiveShim {
inline std::set<OSObject *> objects;
inline std::vector<std::string> logs;
inline std::vector<std::string> propertyReads;
inline unsigned registerCalls {}, superProbeCalls {}, superStartCalls {}, superStopCalls {},
    serviceFrees {}, isOpenCalls {}, setObjectCalls {}, setPropertyCalls {}, creations {};
inline bool superProbeFails {}, superStartFails {}, setPropertyFails {};
// 1-based ordinal of the setObject/creation call that fails; 0 never fails.
inline unsigned failSetObjectAt {}, failCreationAt {};
inline void (*afterConfigRead)(IOPCIDevice *, UInt8) {};
inline void reset() {
    logs.clear(); propertyReads.clear();
    registerCalls = superProbeCalls = superStartCalls = superStopCalls = serviceFrees = 0;
    isOpenCalls = setObjectCalls = setPropertyCalls = creations = 0;
    superProbeFails = superStartFails = setPropertyFails = false;
    failSetObjectAt = failCreationAt = 0; afterConfigRead = nullptr;
}
inline bool creationFails() { return ++creations == failCreationAt; }
}

class OSObject {
public:
    unsigned refs {1};
    bool immortal {};
    std::set<std::string> names {"OSObject"};
    OSObject() { GlkPassiveShim::objects.insert(this); }
    virtual ~OSObject() { GlkPassiveShim::objects.erase(this); }
    void retain() const { if (!immortal) ++const_cast<OSObject *>(this)->refs; }
    void release() const {
        if (immortal) return;
        auto *self = const_cast<OSObject *>(this);
        if (!self->refs) std::abort();
        if (!--self->refs) self->free();
    }
    virtual void free() { delete this; }
    OSObject *metaCast(const char *name) const {
        return names.count(name) ? const_cast<OSObject *>(this) : nullptr;
    }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSDeclareDefaultStructors(className) \
    public: className(); protected: ~className() override; private:
#define OSDefineMetaClassAndStructors(className, superName) \
    className::className() { names.insert(#className); } className::~className() {}

class OSBoolean : public OSObject {
public:
    const bool value;
    explicit OSBoolean(bool v) : value(v) {
        immortal = true; GlkPassiveShim::objects.erase(this);
    }
};
inline OSBoolean shimTrue {true}, shimFalse {false};
inline OSBoolean *const kOSBooleanTrue = &shimTrue;
inline OSBoolean *const kOSBooleanFalse = &shimFalse;

class OSNumber : public OSObject {
public:
    unsigned long long value {};
    unsigned bits {};
    static OSNumber *withNumber(unsigned long long v, unsigned n) {
        if (GlkPassiveShim::creationFails()) return nullptr;
        if (!n || n > 64) std::abort();
        auto *number = new OSNumber;
        number->bits = n; number->value = n == 64 ? v : (v & ((1ULL << n) - 1));
        return number;
    }
    unsigned long long unsigned64BitValue() const { return value; }
    unsigned numberOfBits() const { return bits; }
};
class OSString : public OSObject {
public:
    std::string text;
    static OSString *withCString(const char *s) {
        if (GlkPassiveShim::creationFails()) return nullptr;
        if (!s) std::abort();
        auto *string = new OSString; string->text = s; return string;
    }
    const char *getCStringNoCopy() const { return text.c_str(); }
};
class OSData : public OSObject {
public:
    std::vector<uint8_t> bytes;
    static OSData *withWord(uint32_t word, unsigned length = 4) {
        auto *data = new OSData;
        for (unsigned i = 0; i < length; ++i) data->bytes.push_back(uint8_t(i < 4 ? word >> (8 * i) : 0));
        return data;
    }
    unsigned getLength() const { return static_cast<unsigned>(bytes.size()); }
    const void *getBytesNoCopy() const { return bytes.empty() ? nullptr : bytes.data(); }
};
class OSDictionary : public OSObject {
public:
    std::map<std::string, const OSObject *> entries;
    static OSDictionary *withCapacity(unsigned) {
        if (GlkPassiveShim::creationFails()) return nullptr;
        return new OSDictionary;
    }
    ~OSDictionary() override { for (auto &entry : entries) entry.second->release(); }
    bool setObject(const char *key, const OSObject *value) {
        if (!key || !value) std::abort();
        if (++GlkPassiveShim::setObjectCalls == GlkPassiveShim::failSetObjectAt) return false;
        value->retain();
        auto found = entries.find(key);
        if (found != entries.end()) { found->second->release(); entries.erase(found); }
        entries.emplace(key, value);
        return true;
    }
    const OSObject *getObject(const char *key) const {
        auto found = entries.find(key);
        return found == entries.end() ? nullptr : found->second;
    }
};
class OSIterator : public OSObject {
public:
    std::vector<OSObject *> items;
    size_t next {};
    ~OSIterator() override { for (auto *item : items) item->release(); }
    OSObject *getNextObject() { return next < items.size() ? items[next++] : nullptr; }
};

class IOService : public OSObject {
public:
    IOService *provider {};
    std::vector<IOService *> clientList;
    std::set<const IOService *> openers;
    std::map<std::string, OSObject *> properties;
    bool inactive {}, registered {};
    uint32_t busy {};
    uint64_t registryId {};
    IOService() { names.insert("IOService"); }
    ~IOService() override { for (auto &entry : properties) entry.second->release(); }
    void free() override { ++GlkPassiveShim::serviceFrees; OSObject::free(); }
    virtual IOService *probe(IOService *, SInt32 *) {
        ++GlkPassiveShim::superProbeCalls;
        return GlkPassiveShim::superProbeFails ? nullptr : this;
    }
    virtual bool start(IOService *candidate) {
        ++GlkPassiveShim::superStartCalls;
        if (candidate != provider) std::abort();
        return !GlkPassiveShim::superStartFails;
    }
    virtual void stop(IOService *candidate) {
        ++GlkPassiveShim::superStopCalls;
        if (candidate != provider) std::abort();
    }
    void registerService(IOOptionBits = 0) { ++GlkPassiveShim::registerCalls; registered = true; }
    bool setProperty(const char *key, OSObject *value) {
        ++GlkPassiveShim::setPropertyCalls;
        if (!key || !value) std::abort();
        if (GlkPassiveShim::setPropertyFails) return false;
        fixture(key, value); return true;
    }
    void removeProperty(const char *key) { fixture(key, nullptr); }
    OSObject *copyProperty(const char *key) const {
        GlkPassiveShim::propertyReads.emplace_back(key);
        auto found = properties.find(key);
        if (found == properties.end()) return nullptr;
        found->second->retain(); return found->second;
    }
    // Fixture setter: stores value retained; caller keeps its own reference.
    void fixture(const char *key, OSObject *value) {
        auto found = properties.find(key);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
        if (value) { value->retain(); properties.emplace(key, value); }
    }
    bool isInactive() const { return inactive; }
    bool isOpen(const IOService *client = nullptr) const {
        ++GlkPassiveShim::isOpenCalls;
        return client ? openers.count(client) != 0 : !openers.empty();
    }
    uint32_t getBusyState() { return busy; }
    uint64_t getRegistryEntryID() const { return registryId; }
    IOService *getProvider() const { return provider; }
    OSIterator *getClientIterator() const {
        if (GlkPassiveShim::creationFails()) return nullptr;
        auto *iterator = new OSIterator;
        for (auto *client : clientList) { client->retain(); iterator->items.push_back(client); }
        return iterator;
    }
};

class IOPCIDevice : public IOService {
public:
    uint32_t config[64] {};
    bool configRefused {};
    UInt8 bus {}, slot {}, function {};
    std::vector<std::pair<unsigned, UInt8>> reads; // (width, offset)
    IOPCIDevice() { names.insert("IOPCIDevice"); }
    // The public SDK15.5 surface (IOPCIDevice.h:490,498): non-virtual
    // configRead32/16(IOByteCount) forwarding to IOPCIFamily-681.140.3
    // extendedConfigRead32/16, which return all-ones when configAccess fails.
    // Offsets at or above 0x100 abort here: the probe uses no extended space.
    UInt32 configRead32(IOByteCount offset) {
        if ((offset & 3) || offset >= 0x100) std::abort();
        reads.emplace_back(32, static_cast<UInt8>(offset));
        const UInt32 value = configRefused ? 0xffffffffU : config[offset / 4];
        if (GlkPassiveShim::afterConfigRead) GlkPassiveShim::afterConfigRead(this, static_cast<UInt8>(offset));
        return value;
    }
    UInt16 configRead16(IOByteCount offset) {
        if ((offset & 1) || offset >= 0x100) std::abort();
        reads.emplace_back(16, static_cast<UInt8>(offset));
        const UInt16 value = configRefused ? 0xffff :
            static_cast<UInt16>(config[offset / 4] >> ((offset & 2) * 8));
        if (GlkPassiveShim::afterConfigRead) GlkPassiveShim::afterConfigRead(this, static_cast<UInt8>(offset));
        return value;
    }
    UInt8 getBusNumber() { return bus; }
    UInt8 getDeviceNumber() { return slot; }
    UInt8 getFunctionNumber() { return function; }
    void setWord16(UInt8 offset, UInt16 value) {
        auto &word = config[offset / 4];
        const unsigned shift = (offset & 2) * 8;
        word = (word & ~(0xffffU << shift)) | (uint32_t(value) << shift);
    }
};

class IOMapper : public IOService {
public:
    static IOMapper *gSystem;
    IOMapper() { names.insert("IOMapper"); }
};
inline IOMapper *IOMapper::gSystem = reinterpret_cast<IOMapper *>(uintptr_t(1));

inline void IOLog(const char *format, ...) {
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    GlkPassiveShim::logs.emplace_back(line);
}

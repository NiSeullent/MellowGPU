// HOSTED INJECTED TRANSPORT: unchanged Mellow/GlkPassiveProbeIOKit.cpp against
// the explicit fake OS boundary in native_glk_passive_probe_shim.hpp. No
// kernel, IORegistry, PCI configuration space, IOMMU, power state or GPU is
// touched; passing proves sampler/lifecycle logic only, never hardware.
#include "native_glk_passive_probe_shim.hpp"
#include "../Mellow/GlkPassiveProbeIOKit.hpp"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace MellowNative;
namespace Shim = GlkPassiveShim;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (0)

namespace {
constexpr const char *Key = "MellowGlkPassiveObservation";
// Aligned and inside the host __PAGEZERO: any dereference faults under ASan.
constexpr uintptr_t LiveWord = 0xdead0000;
IOMapper *mapperWord(uintptr_t value) { return reinterpret_cast<IOMapper *>(value); }
uintptr_t address(const void *p) { return reinterpret_cast<uintptr_t>(p); }

struct World {
    IOPCIDevice *gpu {new IOPCIDevice};
    IOService *framebuffer {new IOService};
    World() {
        gpu->registryId = 4294967814ULL; gpu->slot = 2;
        auto &c = gpu->config;
        c[0x00 / 4] = 0x31858086; c[0x04 / 4] = 0x00100007; c[0x08 / 4] = 0x03000006;
        c[0x0c / 4] = 0; c[0x2c / 4] = 0x3e7717aa; c[0x34 / 4] = 0x40;
        // GLK-like list shape: vendor 0x40, PCIe 0x70, MSI 0xac, PM 0xd0.
        gpu->setWord16(0x40, 0x7009); gpu->setWord16(0x70, 0xac10);
        gpu->setWord16(0xac, 0xd005); gpu->setWord16(0xd0, 0x0001);
        gpu->setWord16(0xd2, 0x0022); gpu->setWord16(0xd4, 0x0000);
        registry("vendor-id", 0x8086); registry("device-id", 0x3185); registry("revision-id", 0x06);
        registry("class-code", 0x030000); registry("subsystem-vendor-id", 0x17aa);
        registry("subsystem-id", 0x3e77);
        framebuffer->names.insert("IOFramebuffer"); framebuffer->provider = gpu;
        framebuffer->retain(); gpu->clientList.push_back(framebuffer);
        gpu->openers.insert(framebuffer);
    }
    ~World() {
        for (auto *client : gpu->clientList) client->release();
        gpu->clientList.clear(); framebuffer->release(); gpu->release();
    }
    void registry(const char *key, uint32_t value, unsigned length = 4) {
        auto *data = OSData::withWord(value, length); gpu->fixture(key, data); data->release();
    }
    void property(const char *key, OSObject *value) { gpu->fixture(key, value); value->release(); }
};
struct Attached {
    World &w;
    MellowGlkPassiveProbe *probe {new MellowGlkPassiveProbe};
    explicit Attached(World &world) : w(world) {
        probe->provider = w.gpu; probe->retain(); w.gpu->clientList.push_back(probe);
    }
    ~Attached() { probe->release(); }
    bool matched() { SInt32 score = 0; return probe->probe(w.gpu, &score) == probe; }
    bool started() { return matched() && probe->start(w.gpu); }
    const OSDictionary *observation() const {
        auto found = probe->properties.find(Key);
        return found == probe->properties.end() ? nullptr : dynamic_cast<const OSDictionary *>(found->second);
    }
};
const OSObject *field(const OSDictionary *d, const char *key) { return d ? d->getObject(key) : nullptr; }
uint64_t num(const OSDictionary *d, const char *key) {
    auto *n = dynamic_cast<const OSNumber *>(field(d, key)); CHECK(n); return n ? n->value : ~0ULL;
}
unsigned width(const OSDictionary *d, const char *key) {
    auto *n = dynamic_cast<const OSNumber *>(field(d, key)); CHECK(n); return n ? n->bits : 0;
}
std::string str(const OSDictionary *d, const char *key) {
    auto *s = dynamic_cast<const OSString *>(field(d, key)); CHECK(s); return s ? s->text : "";
}
bool flag(const OSDictionary *d, const char *key) {
    auto *b = dynamic_cast<const OSBoolean *>(field(d, key)); CHECK(b); return b && b->value;
}
// Only numbers, strings and booleans are published, and none of them carries
// an object address or the gSystem word, raw or with its wait bits cleared.
void noPointers(const OSDictionary *d, std::initializer_list<uintptr_t> pointers) {
    CHECK(d);
    for (auto &entry : d->entries) {
        if (auto *n = dynamic_cast<const OSNumber *>(entry.second)) {
            for (auto p : pointers) CHECK(n->value != p && n->value != (p & ~uintptr_t(3)));
        } else if (auto *s = dynamic_cast<const OSString *>(entry.second)) {
            for (auto p : pointers) {
                char hex[32]; std::snprintf(hex, sizeof(hex), "%" PRIxPTR, p);
                CHECK(s->text.find(hex) == std::string::npos);
            }
        } else {
            CHECK(dynamic_cast<const OSBoolean *>(entry.second) != nullptr);
        }
    }
}
// Bounded read set: identity/command/class/header/subsystem/capability-pointer
// dwords, list headers, and the PM PMC/PMCSR words only.
bool readsWithin(const IOPCIDevice *gpu, std::initializer_list<unsigned> words16) {
    for (auto &read : gpu->reads) {
        bool ok = false;
        if (read.first == 32) for (unsigned o : {0x00u, 0x04u, 0x08u, 0x0cu, 0x2cu, 0x34u}) ok |= read.second == o;
        if (read.first == 16) for (unsigned o : words16) ok |= read.second == o;
        if (!ok) return false;
    }
    return true;
}
const std::string &lastLog() { static const std::string none; return Shim::logs.empty() ? none : Shim::logs.back(); }

void expectRefused(World &w, const char *verdict, size_t configReads) {
    Attached a(w);
    CHECK(!a.matched());
    CHECK(lastLog() == std::string("MellowGlkPassiveProbe: refused provider: ") + verdict + "\n");
    CHECK(w.gpu->reads.size() == configReads);
    // A refused probe never starts, publishes or registers anything.
    CHECK(!a.probe->start(w.gpu) && Shim::superStartCalls == 0 && Shim::registerCalls == 0);
    CHECK(!a.observation() && a.probe->properties.empty() && !a.probe->registered);
    CHECK(!w.gpu->openers.count(a.probe));
}
const OSDictionary *startTarget(Attached &a) {
    CHECK(a.started());
    CHECK(Shim::registerCalls == 1 && a.probe->registered);
    return a.observation();
}
}

int main() {
    // Exact target, boot-default Unknown system mapper and a framebuffer client.
    { Shim::reset(); IOMapper::gSystem = mapperWord(1); World w;
      const auto registryBefore = w.gpu->properties;
      uint32_t configBefore[64]; std::memcpy(configBefore, w.gpu->config, sizeof(configBefore));
      { Attached a(w);
        CHECK(a.matched() && w.gpu->reads.size() == 12);
        const unsigned objectsBefore = Shim::setObjectCalls;
        CHECK(a.probe->start(w.gpu) && Shim::registerCalls == 1 && a.probe->registered);
        const auto *d = a.observation();
        CHECK(w.gpu->reads.size() == 24 && Shim::setObjectCalls > objectsBefore);
        CHECK(str(d, "Schema") == "mellowgpu.glk-passive-probe/1");
        CHECK(num(d, "VendorID") == 0x8086 && num(d, "DeviceID") == 0x3185 && num(d, "RevisionID") == 6);
        CHECK(num(d, "ClassCode") == 0x030000 && num(d, "SubsystemVendorID") == 0x17aa);
        CHECK(num(d, "SubsystemID") == 0x3e77 && flag(d, "RegistryIdentityMatchesConfig"));
        CHECK(width(d, "VendorID") == 16 && width(d, "ClassCode") == 32 && width(d, "ProviderRegistryEntryID") == 64);
        CHECK(num(d, "Bus") == 0 && num(d, "Device") == 2 && num(d, "Function") == 0 && num(d, "HeaderType") == 0);
        CHECK(num(d, "ProviderRegistryEntryID") == 4294967814ULL);
        CHECK(num(d, "Command") == 7 && num(d, "Status") == 0x10);
        CHECK(flag(d, "IOSpaceEnabled") && flag(d, "MemorySpaceEnabled") && flag(d, "BusMasterEnabled"));
        CHECK(str(d, "PMCapability") == "Found" && num(d, "PMCapabilityOffset") == 0xd0);
        CHECK(num(d, "CapabilitiesVisited") == 4 && num(d, "PMC") == 0x22 && num(d, "PMCSR") == 0);
        CHECK(str(d, "PowerState") == "D0" && !flag(d, "NoSoftReset") && !flag(d, "PMEEnable") && !flag(d, "PMEStatus"));
        CHECK(str(d, "SystemMapper") == "Unknown" && str(d, "IOMMUParent") == "Absent");
        CHECK(str(d, "IOMemoryDescriptorOptions") == "Absent");
        CHECK(!field(d, "IOMMUParentDataBytes") && !field(d, "IOMemoryDescriptorOptionsValue"));
        CHECK(flag(d, "ProviderOpen") && num(d, "ProviderBusyState") == 0);
        CHECK(num(d, "ProviderClients") == 2 && num(d, "FramebufferClients") == 1 && !flag(d, "ProviderClientsTruncated"));
        CHECK(num(d, "DarwinMajor") == 24 && flag(d, "SamplesAgree"));
        for (const char *claim : {"DurablePowerLease", "DMALease", "MapperLease", "GPUSupport", "Acceleration"})
            CHECK(!flag(d, claim));
        noPointers(d, {address(w.gpu), address(a.probe), address(w.framebuffer)});
        CHECK(readsWithin(w.gpu, {0x40, 0x70, 0xac, 0xd0, 0xd2, 0xd4}));
        CHECK(!w.gpu->openers.count(a.probe) && w.gpu->openers.count(w.framebuffer));
        CHECK(Shim::superProbeCalls == 1 && Shim::superStartCalls == 1 && Shim::superStopCalls == 0);
        CHECK(Shim::logs.empty() && a.probe->properties.size() == 1);
        a.probe->stop(w.gpu);
        CHECK(!a.observation() && Shim::superStopCalls == 1);
      }
      CHECK(std::memcmp(configBefore, w.gpu->config, sizeof(configBefore)) == 0);
      CHECK(w.gpu->properties == registryBefore); }
    CHECK(Shim::objects.empty());

    // gSystem classification: value only, never followed, never published.
    for (auto expected : std::initializer_list<std::pair<uintptr_t, const char *>> {
            {0, "NoMapper"}, {1, "Unknown"}, {2, "HasMapper"}, {3, "Unrecognized"},
            {LiveWord, "Live"}, {LiveWord | 1, "Unrecognized"}, {LiveWord | 2, "Unrecognized"},
            {LiveWord | 3, "Unrecognized"}}) {
        Shim::reset(); IOMapper::gSystem = mapperWord(expected.first); World w;
        { Attached a(w); const auto *d = startTarget(a);
          CHECK(str(d, "SystemMapper") == expected.second);
          noPointers(d, {address(w.gpu), address(a.probe)});
          // Sentinels 0..3 are legitimately small numbers; addresses are not.
          if (expected.first >= LiveWord) noPointers(d, {expected.first}); }
    }
    IOMapper::gSystem = mapperWord(1);
    CHECK(Shim::objects.empty());

    // iommu-parent property type, with mapper liveness but no value.
    { Shim::reset(); World w; auto *mapper = new IOMapper; mapper->retain(); w.property("iommu-parent", mapper);
      { Attached a(w); CHECK(str(startTarget(a), "IOMMUParent") == "IOMapperActive"); }
      mapper->inactive = true;
      { Attached a(w); Shim::registerCalls = 0;
        const auto *d = startTarget(a);
        CHECK(str(d, "IOMMUParent") == "IOMapperInactive");
        noPointers(d, {address(mapper)}); }
      mapper->release(); }
    { Shim::reset(); World w; w.property("iommu-parent", OSNumber::withNumber(5, 32));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "IOMMUParent") == "OSNumber" && !field(d, "IOMMUParentDataBytes")); }
    { Shim::reset(); World w; w.property("iommu-parent", OSData::withWord(1, 4));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "IOMMUParent") == "OSData" && num(d, "IOMMUParentDataBytes") == 4); }
    { Shim::reset(); World w; w.property("iommu-parent", OSString::withCString("unit"));
      Attached a(w); CHECK(str(startTarget(a), "IOMMUParent") == "Other"); }
    CHECK(Shim::objects.empty());

    // IOMemoryDescriptorOptions: AppleVTD publishes kIOMemoryMapperNone as a
    // 32-bit OSNumber; other values and types are reported, never synthesized.
    { Shim::reset(); World w; w.property("IOMemoryDescriptorOptions", OSNumber::withNumber(kIOMemoryMapperNone, 32));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "IOMemoryDescriptorOptions") == "OSNumber" && num(d, "IOMemoryDescriptorOptionsValue") == 0x800);
      CHECK(num(d, "IOMemoryDescriptorOptionsBits") == 32 && flag(d, "IOMemoryDescriptorOptionsMapperNone"));
      CHECK(!flag(d, "IOMemoryDescriptorOptionsWithheld")); }
    { Shim::reset(); World w; w.property("IOMemoryDescriptorOptions", OSNumber::withNumber(1, 32));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(num(d, "IOMemoryDescriptorOptionsValue") == 1 && !flag(d, "IOMemoryDescriptorOptionsMapperNone")); }
    { Shim::reset(); World w; const uint64_t wide = 0xffffff8012345678ULL;
      w.property("IOMemoryDescriptorOptions", OSNumber::withNumber(wide, 64));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(flag(d, "IOMemoryDescriptorOptionsWithheld") && !field(d, "IOMemoryDescriptorOptionsValue"));
      CHECK(num(d, "IOMemoryDescriptorOptionsBits") == 64 && !flag(d, "IOMemoryDescriptorOptionsMapperNone"));
      noPointers(d, {static_cast<uintptr_t>(wide)}); }
    { Shim::reset(); World w; w.property("IOMemoryDescriptorOptions", OSData::withWord(0x800, 4));
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "IOMemoryDescriptorOptions") == "OSData" && !field(d, "IOMemoryDescriptorOptionsMapperNone")); }
    CHECK(Shim::objects.empty());

    // Refusals. Registry identity, kernel and nub address are checked before
    // any configuration access, so a different device sees zero reads.
    { Shim::reset(); auto *probe = new MellowGlkPassiveProbe; auto *other = new IOService;
      probe->provider = other; SInt32 score = 0;
      CHECK(probe->probe(other, &score) == nullptr && lastLog() == "MellowGlkPassiveProbe: refused provider: NotPci\n");
      CHECK(!probe->start(other) && Shim::superStartCalls == 0);
      probe->release(); other->release(); }
    { Shim::reset(); World w; w.gpu->inactive = true; expectRefused(w, "Inactive", 0); }
    for (int major : {23, 25}) {
        Shim::reset(); version_major = major; World w; expectRefused(w, "WrongKernel", 0);
    }
    version_major = 24;
    for (auto wrong : std::initializer_list<std::pair<const char *, uint32_t>> {
            {"vendor-id", 0x8087}, {"vendor-id", 0x00018086}, {"device-id", 0x3184},
            {"revision-id", 0x03}, {"revision-id", 0x106}, {"class-code", 0x038000},
            {"class-code", 0x030001}, {"class-code", 0x01030000}, {"subsystem-vendor-id", 0x17ab},
            {"subsystem-id", 0x3e78}}) {
        Shim::reset(); World w; w.registry(wrong.first, wrong.second);
        expectRefused(w, "RegistryMismatch", 0);
        for (auto &read : Shim::propertyReads) CHECK(read != "iommu-parent" && read != "IOMemoryDescriptorOptions");
    }
    for (const char *key : {"vendor-id", "device-id", "revision-id", "class-code", "subsystem-vendor-id", "subsystem-id"}) {
        { Shim::reset(); World w; w.gpu->fixture(key, nullptr); expectRefused(w, "RegistryMismatch", 0); }
        { Shim::reset(); World w; w.registry(key, 0x06, 2); expectRefused(w, "RegistryMismatch", 0); }
        { Shim::reset(); World w; w.registry(key, 0x06, 8); expectRefused(w, "RegistryMismatch", 0); }
        { Shim::reset(); World w; w.property(key, OSNumber::withNumber(0x8086, 32)); expectRefused(w, "RegistryMismatch", 0); }
    }
    { Shim::reset(); World w; w.gpu->bus = 1; expectRefused(w, "WrongLocation", 0); }
    { Shim::reset(); World w; w.gpu->slot = 3; expectRefused(w, "WrongLocation", 0); }
    { Shim::reset(); World w; w.gpu->function = 1; expectRefused(w, "WrongLocation", 0); }
    { Shim::reset(); World w; w.gpu->configRefused = true; expectRefused(w, "ConfigUnreadable", 1); }
    { Shim::reset(); World w; w.gpu->config[0] = 0x3185ffff; expectRefused(w, "ConfigUnreadable", 1); }
    for (auto wrong : std::initializer_list<std::pair<unsigned, uint32_t>> {
            {0x00, 0x31848086}, {0x00, 0x31858087}, {0x08, 0x03000003}, {0x08, 0x03800006},
            {0x08, 0x03000106}, {0x2c, 0x3e7817aa}, {0x2c, 0x3e7717ab}, {0x2c, 0xffffffff},
            {0x0c, 0x00010000}, {0x0c, 0x00820000}}) {
        Shim::reset(); World w; w.gpu->config[wrong.first / 4] = wrong.second;
        expectRefused(w, "ConfigMismatch", 4);
    }
    { Shim::reset(); World w; Attached a(w); w.gpu->openers.insert(a.probe);
      CHECK(!a.matched() && lastLog() == "MellowGlkPassiveProbe: refused provider: OpenedBySelf\n");
      CHECK(!a.probe->start(w.gpu) && !a.observation() && Shim::registerCalls == 0); }
    { Shim::reset(); World w; Shim::superProbeFails = true; Attached a(w);
      CHECK(!a.matched() && w.gpu->reads.empty() && Shim::logs.empty()); }
    // Multi-function bit with a type-0 layout is still this device.
    { Shim::reset(); World w; w.gpu->config[0x0c / 4] = 0x00800000;
      Attached a(w); CHECK(num(startTarget(a), "HeaderType") == 0x80); }
    CHECK(Shim::objects.empty());

    // PM capability walk: bounded, loop-safe, never wraps past 0xff.
    { Shim::reset(); World w; w.gpu->config[0x04 / 4] = 0x00000007;
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PMCapability") == "NoCapabilityList" && num(d, "CapabilitiesVisited") == 0);
      CHECK(!field(d, "PMCapabilityOffset") && !field(d, "PowerState") && readsWithin(w.gpu, {})); }
    { Shim::reset(); World w; w.gpu->config[0x34 / 4] = 0;
      Attached a(w); CHECK(str(startTarget(a), "PMCapability") == "NotFound"); }
    { Shim::reset(); World w; w.gpu->config[0x34 / 4] = 0x20;
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PMCapability") == "Malformed" && num(d, "CapabilitiesVisited") == 0); }
    { Shim::reset(); World w; w.gpu->setWord16(0x70, 0x4010);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PMCapability") == "Malformed" && num(d, "CapabilitiesVisited") == 2); }
    { Shim::reset(); World w; w.gpu->setWord16(0x70, 0xacff);
      Attached a(w); CHECK(str(startTarget(a), "PMCapability") == "Malformed"); }
    { Shim::reset(); World w; w.gpu->config[0x34 / 4] = 0xfc; w.gpu->setWord16(0xfc, 0x0001);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PMCapability") == "Malformed" && readsWithin(w.gpu, {0xfc})); }
    { Shim::reset(); World w; w.gpu->config[0x34 / 4] = 0xf8; w.gpu->setWord16(0xf8, 0x0001);
      w.gpu->setWord16(0xfa, 0x0003); w.gpu->setWord16(0xfc, 0x0003);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(num(d, "PMCapabilityOffset") == 0xf8 && str(d, "PowerState") == "D3hot");
      CHECK(readsWithin(w.gpu, {0xf8, 0xfa, 0xfc})); }
    { Shim::reset(); World w; w.gpu->config[0x34 / 4] = 0x43; w.gpu->setWord16(0xac, 0xd305);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PMCapability") == "Found" && num(d, "PMCapabilityOffset") == 0xd0); }
    for (bool loop : {false, true}) {
        Shim::reset(); World w;
        for (unsigned i = 0; i < 48; ++i) {
            const unsigned position = 0x40 + 4 * i;
            const unsigned next = i < 47 ? position + 4 : (loop ? 0x40 : 0);
            w.gpu->setWord16(static_cast<UInt8>(position), static_cast<UInt16>(next << 8 | 0x09));
        }
        Attached a(w); const auto *d = startTarget(a);
        CHECK(str(d, "PMCapability") == (loop ? "Malformed" : "NotFound"));
        CHECK(num(d, "CapabilitiesVisited") == 48 && w.gpu->reads.size() == 2 * (6 + 48));
    }
    { Shim::reset(); World w; w.gpu->setWord16(0xd4, 0x8103);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PowerState") == "D3hot" && flag(d, "PMEEnable") && flag(d, "PMEStatus") && !flag(d, "NoSoftReset")); }
    { Shim::reset(); World w; w.gpu->setWord16(0xd4, 0x0009);
      Attached a(w); const auto *d = startTarget(a);
      CHECK(str(d, "PowerState") == "D1" && flag(d, "NoSoftReset")); }
    CHECK(Shim::objects.empty());

    // Start re-samples. A different registry entry or physical identity is
    // refused and undone; changed observations publish SamplesAgree=false.
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); w.gpu->registryId++;
      CHECK(!a.probe->start(w.gpu) && lastLog() == "MellowGlkPassiveProbe: not started: Unstable\n");
      CHECK(Shim::superStartCalls == 1 && Shim::superStopCalls == 1 && !a.observation() && Shim::registerCalls == 0); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); w.gpu->config[0] = 0x31848086;
      CHECK(!a.probe->start(w.gpu) && lastLog() == "MellowGlkPassiveProbe: not started: ConfigMismatch\n");
      CHECK(Shim::superStopCalls == 1 && !a.observation()); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); w.gpu->inactive = true;
      CHECK(!a.probe->start(w.gpu) && lastLog() == "MellowGlkPassiveProbe: not started: Inactive\n"); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); IOMapper::gSystem = mapperWord(0);
      CHECK(a.probe->start(w.gpu)); const auto *d = a.observation();
      CHECK(!flag(d, "SamplesAgree") && str(d, "SystemMapper") == "NoMapper"); IOMapper::gSystem = mapperWord(1); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); w.gpu->setWord16(0xd4, 0x0003);
      CHECK(a.probe->start(w.gpu)); const auto *d = a.observation();
      CHECK(!flag(d, "SamplesAgree") && str(d, "PowerState") == "D3hot"); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); w.gpu->busy = 3;
      CHECK(a.probe->start(w.gpu)); const auto *d = a.observation();
      CHECK(flag(d, "SamplesAgree") && num(d, "ProviderBusyState") == 3); }
    { Shim::reset(); World w; Attached a(w);
      CHECK(!a.probe->start(w.gpu) && Shim::superStartCalls == 0 && w.gpu->reads.empty()); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); Shim::superStartFails = true;
      CHECK(!a.probe->start(w.gpu) && Shim::superStopCalls == 0 && !a.observation() && w.gpu->reads.size() == 12); }
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched()); Shim::setPropertyFails = true;
      CHECK(!a.probe->start(w.gpu) && lastLog() == "MellowGlkPassiveProbe: not started: PublishFailed\n");
      CHECK(Shim::superStopCalls == 1 && !a.observation() && Shim::registerCalls == 0); }
    // Clients beyond the bound are not walked.
    { Shim::reset(); World w;
      for (int i = 0; i < 40; ++i) { auto *client = new IOService; client->provider = w.gpu; w.gpu->clientList.push_back(client); }
      Attached a(w); const auto *d = startTarget(a);
      CHECK(num(d, "ProviderClients") == 32 && flag(d, "ProviderClientsTruncated") && num(d, "FramebufferClients") == 1); }
    CHECK(Shim::objects.empty());

    // Every individual publication allocation or insertion failure.
    unsigned insertions = 0, creations = 0;
    { Shim::reset(); World w; Attached a(w); CHECK(a.matched());
      const unsigned objects = Shim::setObjectCalls, made = Shim::creations;
      CHECK(a.probe->start(w.gpu));
      insertions = Shim::setObjectCalls - objects; creations = Shim::creations - made; }
    CHECK(insertions >= 40 && creations >= 30);
    for (unsigned n = 1; n <= insertions; ++n) {
        Shim::reset(); World w; Attached a(w); CHECK(a.matched());
        Shim::failSetObjectAt = Shim::setObjectCalls + n;
        CHECK(!a.probe->start(w.gpu) && lastLog() == "MellowGlkPassiveProbe: not started: PublishFailed\n");
        CHECK(!a.observation() && Shim::superStopCalls == 1 && Shim::registerCalls == 0);
    }
    unsigned iteratorOnly = 0;
    for (unsigned n = 1; n <= creations; ++n) {
        Shim::reset(); World w; Attached a(w); CHECK(a.matched());
        Shim::failCreationAt = Shim::creations + n;
        if (a.probe->start(w.gpu)) {
            // Only the client iterator is optional: no clients are counted.
            const auto *d = a.observation(); ++iteratorOnly;
            CHECK(n == 1 && num(d, "ProviderClients") == 0 && num(d, "FramebufferClients") == 0);
        } else {
            CHECK(lastLog() == "MellowGlkPassiveProbe: not started: PublishFailed\n");
            CHECK(!a.observation() && Shim::superStopCalls == 1 && Shim::registerCalls == 0);
        }
    }
    CHECK(iteratorOnly == 1);
    CHECK(Shim::objects.empty());

    // Lifecycle: stop removes only this probe's observation; release frees it.
    { Shim::reset(); World w; unsigned freesBefore = 0;
      { Attached a(w); startTarget(a); a.probe->stop(w.gpu);
        CHECK(a.probe->properties.empty() && Shim::superStopCalls == 1);
        freesBefore = Shim::serviceFrees; }
      CHECK(Shim::serviceFrees == freesBefore); }
    CHECK(Shim::objects.empty() && Shim::serviceFrees >= 2);
    std::printf("PASS native GLK passive probe boundary: %u checks; HOSTED injected OS, no kernel/PCI/IOMMU/GPU access\n", checks);
    return 0;
}

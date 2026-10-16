// Actual GLK GGTT IOKit adapter against hosted OS/device boundaries.
// HOSTED INJECTED TRANSPORT (native_glk_ggtt_iokit_shim.hpp): no PCI access,
// BAR mapping, MMIO, barrier, forcewake, TLB or GPU operation happens. The
// forcewake/reset, handover, retirement and quiescence producers are test
// stand-ins for native owners that do not exist yet.
#include "../Mellow/GlkGgttIOKit.hpp"
#include <cstdio>

using namespace MellowNative;
namespace Ggtt = Mellow::NativeGlk;
namespace L = Mellow::NativeGlk::Gen9LpGgtt;
using GlkGgttShim::events;
using GlkGgttShim::faults;

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); std::abort(); } } while (0)

namespace MellowNative {
void glkCpuFence(GlkCpuFence kind) {
    events.push_back({kind == GlkCpuFence::Full ? 'F' : kind == GlkCpuFence::Store ? 'S' : 'L', 0, 0});
}
void glkGgttStore64(volatile uint64_t *at, uint64_t value) {
    const uint64_t offset = GlkGgttShim::barOffset(at, 8);
    if (offset < GlkGgttShim::GsmOffset) std::abort();
    const uint64_t index = (offset - GlkGgttShim::GsmOffset) / 8;
    events.push_back({'w', index, value});
    if (long(index) == faults.dropEntryIndex) return;
    uint8_t image[8];
    Ggtt::storeLe64(value, image);
    std::memcpy(GlkGgttShim::bar0 + offset, image, 8);
}
uint64_t glkGgttLoad64(const volatile uint64_t *at) {
    const uint64_t offset = GlkGgttShim::barOffset(at, 8);
    if (offset < GlkGgttShim::GsmOffset) std::abort();
    uint8_t image[8];
    std::memcpy(image, GlkGgttShim::bar0 + offset, 8);
    const uint64_t value = Ggtt::loadLe64(image);
    events.push_back({'r', (offset - GlkGgttShim::GsmOffset) / 8, value});
    return value;
}
void glkGgttStore32(volatile uint32_t *at, uint32_t value) {
    const uint64_t offset = GlkGgttShim::barOffset(at, 4);
    if (offset < L::kRegisterWindowOffset || offset >= L::kRegisterWindowOffset + L::kRegisterWindowBytes)
        std::abort();
    events.push_back({'W', offset, value});
    if (value != L::kTlbInvalidateBit) std::abort();
    GlkGgttShim::pending[uint32_t(offset)] = faults.clearAfter;
}
uint32_t glkGgttLoad32(const volatile uint32_t *at) {
    const uint64_t offset = GlkGgttShim::barOffset(at, 4);
    unsigned &left = GlkGgttShim::pending[uint32_t(offset)];
    const uint32_t value = faults.stuck.count(uint32_t(offset)) || left ? L::kTlbInvalidateBit : 0;
    if (left) --left;
    events.push_back({'Q', offset, value});
    return value;
}
} // namespace MellowNative

namespace {
constexpr uint64_t MiB = 1ULL << 20, Page = 4096;
constexpr uint64_t PoolFirst = 17 * MiB / Page;   // pool [16, 48) MiB minus stolen [16, 17) MiB

struct Authority {
    bool admit {true}, handoverReady {true}, gtReady {true}, gtReleaseOk {true};
    bool retiredOk {true}, quiesceOk {true};
    uint64_t handoverEpoch {9};
    bool awakeKnown {true};
    uint32_t awakeMask {15};
    unsigned gtAcquires {}, gtReleases {}, retiredCalls {}, quiesceCalls {};
    GlkGgttConsumerView last {};
    Ggtt::GgttRetainedRange retained[2] {{0, 8 * MiB, Ggtt::GgttRangeKind::Display},
                                         {16 * MiB, 1 * MiB, Ggtt::GgttRangeKind::Stolen}};
};
bool authAdmitted(void *c, IOPCIDevice *, IOService *, const DeviceIdentity &) {
    return static_cast<Authority *>(c)->admit;
}
bool authHandover(void *c, const DeviceIdentity &id, uint64_t total, Ggtt::GgttHandover &out) {
    auto &a = *static_cast<Authority *>(c);
    if (!a.handoverReady) return false;
    out = {};
    out.registryId = id.registryId; out.epoch = a.handoverEpoch; out.ggttTotal = total;
    out.pool = {16 * MiB, 32 * MiB};
    out.retained = a.retained; out.retainedCount = 2; out.complete = true;
    return true;
}
bool authAcquire(void *c, const DeviceIdentity &, uint64_t &token) {
    auto &a = *static_cast<Authority *>(c);
    if (!a.gtReady) return false;
    ++a.gtAcquires;
    events.push_back({'A', 0, 0});
    token = 0xf00 + a.gtAcquires;
    return true;
}
bool authRelease(void *c, const DeviceIdentity &, uint64_t token) {
    auto &a = *static_cast<Authority *>(c);
    ++a.gtReleases;
    events.push_back({'R', 0, token});
    return a.gtReleaseOk && token == 0xf00 + a.gtAcquires;
}
bool authAwake(void *c, const DeviceIdentity &, uint64_t token, uint32_t &mask) {
    auto &a = *static_cast<Authority *>(c);
    CHECK(token == 0xf00 + a.gtAcquires && a.gtAcquires == a.gtReleases + 1);
    if (!a.awakeKnown) return false;
    mask = a.awakeMask;
    return true;
}
bool authRetired(void *c, const GlkGgttConsumerView &view) {
    auto &a = *static_cast<Authority *>(c);
    ++a.retiredCalls; a.last = view;
    return a.retiredOk;
}
bool authQuiesce(void *c, const DeviceIdentity &) {
    auto &a = *static_cast<Authority *>(c);
    ++a.quiesceCalls;
    return a.quiesceOk;
}

bool empty(const GpuMapping &m) { return !m.cookie && !m.address && !m.bytes && !m.writable; }

struct World {
    IOPCIDevice *device {};
    IOService *owner {};
    Authority authority {};
    GlkDmaIdentity identity {};
    GlkGgttIOKit adapter;
    uint64_t pages[4] {0x1000000, 0x2000000, 0x7ffffff000ULL, 0x3000};
    int cookie {};
    DmaPin pin {};
    MemoryHandle handle {};
    World() {
        GlkGgttShim::reset();
        device = new IOPCIDevice;
        device->barMemory = new IODeviceMemory(0xa0000000ULL, GlkGgttShim::BarBytes);
        owner = new IOService;
        owner->provider = device;
        device->openOwner = owner;
        identity.device = {0x8086, 0x3185, device->registryId, 9};
        identity.revision = 6; identity.subsystemVendor = 0x17aa; identity.subsystem = 0x3e77;
        pin = {&cookie, nullptr, pages, 4, 4 * Page, device->registryId};
        handle = {3, 11, device->registryId, 9};
    }
    ~World() { GlkGgttShim::disposeWorld(); }
    GlkGgttAuthority auth() {
        return {&authority, authAdmitted, authHandover, authAcquire, authRelease, authRetired, authQuiesce, authAwake};
    }
    MemoryStatus attach() { return adapter.attach(device, owner, identity, auth()); }
    static uint64_t entry(uint64_t index) {
        uint8_t image[8];
        std::memcpy(image, GlkGgttShim::bar0 + GlkGgttShim::GsmOffset + index * 8, 8);
        return Ggtt::loadLe64(image);
    }
    static void seed(uint64_t index, uint64_t value) {
        uint8_t image[8];
        Ggtt::storeLe64(value, image);
        std::memcpy(GlkGgttShim::bar0 + GlkGgttShim::GsmOffset + index * 8, image, 8);
    }
};

struct Cursor {
    size_t at {};
    void expect(char op, uint64_t where, uint64_t value, bool checkValue = true) {
        CHECK(at < events.size());
        CHECK(events[at].op == op && events[at].at == where);
        if (checkValue) CHECK(events[at].value == value);
        ++at;
    }
    void invalidation() {
        for (uint32_t control : L::kEngineTlbControls) expect('W', control, 1);
        for (uint32_t control : L::kEngineTlbControls) {
            expect('Q', control, 1); expect('P', 0, 1);
            expect('Q', control, 1); expect('P', 0, 1);
            expect('Q', control, 0);
        }
    }
};

unsigned hardwareEvents() {
    return GlkGgttShim::count('r') + GlkGgttShim::count('w') + GlkGgttShim::count('Q') +
           GlkGgttShim::count('W') + GlkGgttShim::count('A');
}

void testAttach() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    CHECK(w.adapter.attached() && w.adapter.barBase() == 0xa0000000ULL);
    const auto &g = w.adapter.geometry();
    CHECK(g.total == 4ULL << 30 && g.gsmBytes == 8 * MiB && g.entryCount == 1ULL << 20);
    CHECK(GlkGgttShim::liveMaps == 2 && events.size() == 2);
    CHECK(events[0].op == 'M' && events[0].at == 8 * MiB && events[0].value == 8 * MiB);
    CHECK(events[1].op == 'M' && events[1].at == L::kRegisterWindowOffset && events[1].value == L::kRegisterWindowBytes);
    CHECK(GlkGgttShim::lastRequestedOptions == (kIOMapAnywhere | kIOMapInhibitCache));
    CHECK(w.device->refs == 2 && w.owner->refs == 2);
    CHECK(w.adapter.attach(w.device, w.owner, w.identity, w.auth()) == MemoryStatus::Busy);
    auto vm = w.adapter.vmOps();
    CHECK(vm.context == &w.adapter && vm.admitted(vm.context, w.device, w.owner, w.identity.device));
    CHECK(!vm.admitted(vm.context, w.device, w.device, w.identity.device));
    CHECK(w.adapter.detach() == MemoryStatus::Ok);
    CHECK(GlkGgttShim::liveMaps == 0 && GlkGgttShim::count('U') == 2);
    CHECK(w.device->refs == 1 && w.owner->refs == 1 && !w.adapter.attached());
    CHECK(!vm.admitted(vm.context, w.device, w.owner, w.identity.device));
}

// The adapter retains its owner. Its last release may delete an IOService
// that embeds the adapter itself; teardown must not touch this afterward.
void testFinalOwnerReleaseMayDestroyAdapter() {
    struct EmbeddedOwner final : IOService { GlkGgttIOKit embedded; };
    World w;
    w.owner->release();
    auto *owner = new EmbeddedOwner;
    w.owner = owner;
    owner->provider = w.device;
    w.device->openOwner = owner;
    CHECK(owner->embedded.attach(w.device, owner, w.identity, w.auth()) == MemoryStatus::Ok);
    owner->release(); // only the adapter's retained owner reference remains
    CHECK(owner->embedded.detach() == MemoryStatus::Ok); // may delete owner/adapter
    CHECK(GlkGgttShim::liveMaps == 0 && w.device->refs == 1);
    w.owner = nullptr;
    w.device->openOwner = nullptr;
}

void testNativeAwakeProducerRequired() {
    World w;
    auto missing = w.auth(); missing.awakeEngines = nullptr;
    CHECK(w.adapter.attach(w.device, w.owner, w.identity, missing) == MemoryStatus::Unavailable);
    CHECK(GlkGgttShim::mapCalls == 0 && hardwareEvents() == 0);
    CHECK(w.attach() == MemoryStatus::Ok);
    w.authority.awakeKnown = false;
    auto vm = w.adapter.vmOps(); GpuMapping mapping;
    CHECK(vm.map(vm.context, w.identity.device, 7, w.handle, w.pin, true, mapping) == MemoryStatus::Unavailable);
    CHECK(empty(mapping) && hardwareEvents() == 1); // GT acquisition only; no PTE or TLB operation
    w.authority.awakeKnown = true;
    w.authority.awakeMask = 2; // native fixture has only BCS runtime-PM awake
    events.clear();
    CHECK(vm.map(vm.context, w.identity.device, 7, w.handle, w.pin, true, mapping) == MemoryStatus::Ok);
    CHECK(GlkGgttShim::count('W') == 1);
    for (const auto &event : events) if (event.op == 'W' || event.op == 'Q') CHECK(event.at == L::kBtcr);
    CHECK(vm.unmap(vm.context, w.identity.device, 7, w.handle, w.pin, mapping) == MemoryStatus::Ok);
    CHECK(w.adapter.detach() == MemoryStatus::Ok);
}

struct AttachCase { const char *name; void (*arm)(World &); MemoryStatus expected; };

void testAttachRefusals() {
    const AttachCase cases[] = {
        {"revision", [](World &w) { w.device->revision = 7; }, MemoryStatus::Ownership},
        {"subsystem", [](World &w) { w.device->subsystemId = 0x3e7817aa; }, MemoryStatus::Ownership},
        {"lease", [](World &w) { w.authority.admit = false; }, MemoryStatus::Ownership},
        {"not open", [](World &w) { w.device->openOwner = nullptr; }, MemoryStatus::Ownership},
        {"inactive", [](World &w) { w.device->inactive = true; }, MemoryStatus::Ownership},
        {"prefetchable BAR", [](World &w) { w.device->bar0Low = 0xa000000c; }, MemoryStatus::Unavailable},
        {"32-bit BAR", [](World &w) { w.device->bar0Low = 0xa0000000; }, MemoryStatus::Unavailable},
        {"decode disabled", [](World &w) { w.device->command = 0x0004; }, MemoryStatus::Unavailable},
        {"descriptor base", [](World &w) { w.device->barMemory->physical = 0xb0000000; }, MemoryStatus::Unavailable},
        {"descriptor length", [](World &w) { w.device->barMemory->length = 32 * MiB; }, MemoryStatus::Unavailable},
        {"no descriptor", [](World &w) { w.device->barMemory->release(); w.device->barMemory = nullptr; },
         MemoryStatus::Unavailable},
        {"GGMS 0", [](World &w) { w.device->gmch = 0x0201; }, MemoryStatus::Unavailable},
        {"write-combined map", [](World &) { faults.forceCache = kIOMapWriteCombineCache; }, MemoryStatus::Unavailable},
        {"default-cache map", [](World &) { faults.forceCache = kIOMapDefaultCache; }, MemoryStatus::Unavailable},
        {"map physical skew", [](World &) { faults.skewPhysical = Page; }, MemoryStatus::Unavailable},
        {"GSM map failed", [](World &) { faults.failMapCall = 0; }, MemoryStatus::Unavailable},
        {"register map failed", [](World &) { faults.failMapCall = 1; }, MemoryStatus::Unavailable},
        {"no handover", [](World &w) { w.authority.handoverReady = false; }, MemoryStatus::Unavailable},
        {"stale handover", [](World &w) { w.authority.handoverEpoch = 8; }, MemoryStatus::Unavailable},
        {"other function", [](World &w) { w.identity.device.device = 0x3184; }, MemoryStatus::Invalid},
    };
    for (const AttachCase &c : cases) {
        World w;
        c.arm(w);
        const MemoryStatus status = w.attach();
        if (status != c.expected) std::fprintf(stderr, "attach case %s -> %d\n", c.name, int(status));
        CHECK(status == c.expected);
        CHECK(!w.adapter.attached() && GlkGgttShim::liveMaps == 0);
        CHECK(w.device->refs == 1 && w.owner->refs == 1);
        CHECK(hardwareEvents() == 0);
        if (c.arm == cases[12].arm) CHECK(GlkGgttShim::lastRequestedOptions == (kIOMapAnywhere | kIOMapInhibitCache));
    }
    World w;
    GlkGgttAuthority partial = w.auth();
    partial.retired = nullptr;
    CHECK(w.adapter.attach(w.device, w.owner, w.identity, partial) == MemoryStatus::Unavailable);
    CHECK(GlkGgttShim::configReads == 0 && GlkGgttShim::mapCalls == 0);
}

void testPublishRoundTrip() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    const uint64_t prior[4] = {0x5550000001ULL, 0, 0x5552000001ULL, 0x5553000001ULL};
    for (uint64_t i = 0; i < 4; ++i) World::seed(PoolFirst + i, prior[i]);
    World::seed(PoolFirst - 1, 0x0dead001); World::seed(PoolFirst + 4, 0x0beef001);
    events.clear();
    auto vm = w.adapter.vmOps();
    GpuMapping mapping {};
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, mapping) == MemoryStatus::Ok);
    CHECK(mapping.cookie && mapping.address == 17 * MiB && mapping.bytes == 4 * Page && mapping.writable);
    for (uint64_t i = 0; i < 4; ++i) CHECK(World::entry(PoolFirst + i) == (w.pages[i] | 1));
    CHECK(GlkGgttShim::bar0[GlkGgttShim::GsmOffset + (PoolFirst + 2) * 8 + 4] == 0x7f);
    CHECK(World::entry(PoolFirst - 1) == 0x0dead001 && World::entry(PoolFirst + 4) == 0x0beef001);
    Cursor c;
    c.expect('A', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) c.expect('r', PoolFirst + i, prior[i]);
    c.expect('F', 0, 0);
    for (uint64_t i = 0; i < 4; ++i) c.expect('w', PoolFirst + i, w.pages[i] | 1);
    c.expect('F', 0, 0);
    for (uint64_t i = 0; i < 4; ++i) c.expect('r', PoolFirst + i, w.pages[i] | 1);
    c.invalidation();
    c.expect('R', 0, 0xf01);
    CHECK(c.at == events.size());
    CHECK(GlkGgttShim::allocations.size() == 1 && w.adapter.liveBindings() == 1);
    CHECK(w.adapter.detach() == MemoryStatus::Busy);

    events.clear();
    w.authority.retiredOk = false;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, mapping) == MemoryStatus::Busy);
    CHECK(events.empty() && !empty(mapping));
    w.authority.retiredOk = true; w.authority.gtReady = false;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, mapping) == MemoryStatus::Busy);
    CHECK(hardwareEvents() == 0 && !empty(mapping));
    w.authority.gtReady = true;
    GpuMapping forged = mapping; forged.cookie = &forged;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, forged) == MemoryStatus::Ownership);
    DmaPin otherPin = w.pin; otherPin.pageCount = 3;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, otherPin, mapping) == MemoryStatus::Ownership);
    CHECK(vm.unmap(vm.context, w.identity.device, 78, w.handle, w.pin, mapping) == MemoryStatus::Ownership);
    MemoryHandle otherGeneration = w.handle; ++otherGeneration.generation;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, otherGeneration, w.pin, mapping) == MemoryStatus::Ownership);
    events.clear();
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, mapping) == MemoryStatus::Ok);
    CHECK(empty(mapping));
    for (uint64_t i = 0; i < 4; ++i) CHECK(World::entry(PoolFirst + i) == prior[i]);
    CHECK(World::entry(PoolFirst - 1) == 0x0dead001 && World::entry(PoolFirst + 4) == 0x0beef001);
    c = {};
    c.expect('A', 0, 0, false);
    c.expect('F', 0, 0);
    for (uint64_t i = 0; i < 4; ++i) c.expect('w', PoolFirst + i, prior[i]);
    c.expect('F', 0, 0);
    for (uint64_t i = 0; i < 4; ++i) c.expect('r', PoolFirst + i, prior[i]);
    c.invalidation();
    c.expect('R', 0, 0, false);
    CHECK(c.at == events.size());
    CHECK(w.authority.last.ggttOffset == 17 * MiB && w.authority.last.bytes == 4 * Page &&
          w.authority.last.owner == 77 && w.authority.last.handle.slot == 3);
    CHECK(GlkGgttShim::allocations.empty() && w.adapter.liveBindings() == 0);
    CHECK(w.adapter.publisher().extentsInUse() == 0);
    CHECK(w.adapter.detach() == MemoryStatus::Ok && GlkGgttShim::liveMaps == 0);
}

void testMapRefusals() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    auto vm = w.adapter.vmOps();
    GpuMapping m {};
    events.clear();
    // Gen8 GGTT entries cannot express read-only.
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, false, m) == MemoryStatus::Unavailable && empty(m));
    uint64_t highPages[4] = {0x1000, 0x2000, 1ULL << 39, 0x4000};
    DmaPin high = w.pin; high.pages = highPages;
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, high, true, m) == MemoryStatus::Unavailable && empty(m));
    DmaPin odd = w.pin; odd.bytes = 3 * Page;
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, odd, true, m) == MemoryStatus::Unavailable);
    MemoryHandle stale = w.handle; stale.epoch = 8;
    CHECK(vm.map(vm.context, w.identity.device, 77, stale, w.pin, true, m) == MemoryStatus::Unavailable);
    DeviceIdentity other = w.identity.device; ++other.registryId;
    CHECK(vm.map(vm.context, other, 77, w.handle, w.pin, true, m) == MemoryStatus::Unavailable);
    CHECK(vm.map(vm.context, w.identity.device, 0, w.handle, w.pin, true, m) == MemoryStatus::Unavailable);
    CHECK(hardwareEvents() == 0);
    w.authority.gtReady = false;
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::Unavailable && empty(m));
    w.authority.gtReady = true;
    faults.mallocFailureCall = GlkGgttShim::mallocCalls + 1;
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::Unavailable && empty(m));
    CHECK(hardwareEvents() == 0);
    CHECK(w.adapter.publisher().extentsInUse() == 0 && GlkGgttShim::allocations.empty());
    CHECK(w.adapter.liveBindings() == 0);
    // A moved BAR, changed GSM size or disabled decode refuses before effects.
    struct Drift { void (*arm)(World &); void (*undo)(World &); };
    const Drift drifts[] = {
        {[](World &x) { x.device->bar0Low = 0xb0000004; x.device->barMemory->physical = 0xb0000000; },
         [](World &x) { x.device->bar0Low = 0xa0000004; x.device->barMemory->physical = 0xa0000000; }},
        {[](World &x) { x.device->gmch = 0x0281; }, [](World &x) { x.device->gmch = 0x02c1; }},
        {[](World &x) { x.device->command = 0x0004; }, [](World &x) { x.device->command = 0x0007; }},
        {[](World &x) { x.device->revision = 0x07; }, [](World &x) { x.device->revision = 0x06; }},
        {[](World &x) { x.authority.admit = false; }, [](World &x) { x.authority.admit = true; }},
    };
    for (const Drift &drift : drifts) {
        drift.arm(w);
        events.clear();
        CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::Unavailable);
        CHECK(events.empty() && empty(m));
        CHECK(!vm.admitted(vm.context, w.device, w.owner, w.identity.device));
        drift.undo(w);
        CHECK(vm.admitted(vm.context, w.device, w.owner, w.identity.device));
    }
    CHECK(w.adapter.detach() == MemoryStatus::Ok);
}

void testUncertainPublication() {
    {
        World w;
        CHECK(w.attach() == MemoryStatus::Ok);
        auto vm = w.adapter.vmOps();
        faults.dropEntryIndex = long(PoolFirst + 1);
        GpuMapping m {};
        CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::IoFailure);
        CHECK(m.cookie && m.address == 17 * MiB && m.bytes == 4 * Page && m.writable);
        CHECK(w.authority.gtAcquires == 1 && w.authority.gtReleases == 1);
        CHECK(GlkGgttShim::count('W') == 0);            // no invalidation claimed
        CHECK(w.adapter.publisher().faulted());
        CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, m) == MemoryStatus::Quarantined);
        MemoryView view {};
        view.device = w.identity.device; view.owner = 77; view.handle = w.handle; view.pin = w.pin; view.mapping = m;
        CHECK(!vm.retired(vm.context, view));
        CHECK(w.adapter.detach() == MemoryStatus::Quarantined && w.adapter.attached());
        CHECK(GlkGgttShim::allocations.size() == 1);     // capture held with the extent
        MemoryHandle next = w.handle; next.slot = 4;
        GpuMapping m2 {};
        events.clear();
        CHECK(vm.map(vm.context, w.identity.device, 77, next, w.pin, true, m2) == MemoryStatus::Unavailable);
        CHECK(hardwareEvents() == 0 && empty(m2));
    }
    {
        World w;
        CHECK(w.attach() == MemoryStatus::Ok);
        auto vm = w.adapter.vmOps();
        faults.stuck.insert(L::kM1tcr);
        const uint64_t start = GlkGgttShim::clockNs;
        GpuMapping m {};
        CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::IoFailure);
        const uint64_t elapsed = GlkGgttShim::clockNs - start;
        CHECK(elapsed >= 4100000 && elapsed <= 6500000);
        CHECK(!empty(m) && w.authority.gtReleases == 1 && w.adapter.publisher().faulted());
        for (uint64_t i = 0; i < 4; ++i) CHECK(World::entry(PoolFirst + i) == (w.pages[i] | 1));
        CHECK(w.adapter.detach() == MemoryStatus::Quarantined);
    }
}

void testUncertainInverse() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    auto vm = w.adapter.vmOps();
    GpuMapping m {};
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::Ok);
    faults.dropEntryIndex = long(PoolFirst);
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, m) == MemoryStatus::IoFailure);
    CHECK(!empty(m) && w.adapter.publisher().faulted());
    CHECK(World::entry(PoolFirst) == (w.pages[0] | 1));  // page still referenced: backing must stay
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, m) == MemoryStatus::Quarantined);
    CHECK(GlkGgttShim::allocations.size() == 1 && w.adapter.liveBindings() == 1);
    CHECK(w.adapter.detach() == MemoryStatus::Quarantined);
}

void testQuiesce() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    auto vm = w.adapter.vmOps();
    GpuMapping m {};
    CHECK(vm.map(vm.context, w.identity.device, 77, w.handle, w.pin, true, m) == MemoryStatus::Ok);
    w.authority.quiesceOk = false;
    CHECK(!vm.quiesce(vm.context, w.identity.device));
    w.authority.quiesceOk = true;
    CHECK(vm.quiesce(vm.context, w.identity.device) && vm.quiesce(vm.context, w.identity.device));
    CHECK(w.authority.quiesceCalls == 2);
    MemoryHandle next = w.handle; next.slot = 5;
    GpuMapping m2 {};
    CHECK(vm.map(vm.context, w.identity.device, 77, next, w.pin, true, m2) == MemoryStatus::Unavailable);
    // Epoch quiescence covers every consumer; restoration still needs GT access.
    w.authority.retiredOk = false;
    w.authority.gtReady = false;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, m) == MemoryStatus::Busy);
    w.authority.gtReady = true;
    CHECK(vm.unmap(vm.context, w.identity.device, 77, w.handle, w.pin, m) == MemoryStatus::Ok);
    CHECK(w.authority.retiredCalls == 0 && empty(m));
    CHECK(w.adapter.detach() == MemoryStatus::Ok);
}

// MemoryOwner -> MemoryBackend -> the real GlkPhysicalVmOps. The pin side is a
// fixture standing in for GlkDmaMemoryIOKit (tested by its own suite).
struct Backend {
    GlkPhysicalVmOps vm {};
    IOPCIDevice *device {};
    IOService *owner {};
    uint64_t pages[MemoryOwner::MaxAllocations][4] {};
    int cookies[MemoryOwner::MaxAllocations] {};
    uint8_t *cpu {};
    unsigned pins {}, unpins {};
};
bool bAdmitted(void *c, const DeviceIdentity &d) {
    auto &b = *static_cast<Backend *>(c);
    return b.vm.admitted(b.vm.context, b.device, b.owner, d);
}
MemoryStatus bPin(void *c, const DeviceIdentity &d, uint64_t, const MemoryHandle &h, uint64_t bytes, DmaPin &out) {
    auto &b = *static_cast<Backend *>(c);
    if (bytes > 4 * Page || h.slot >= MemoryOwner::MaxAllocations) return MemoryStatus::Invalid;
    for (uint64_t i = 0; i < bytes / Page; ++i) b.pages[h.slot][i] = 0x40000000 + h.slot * MiB + i * Page;
    out = {&b.cookies[h.slot], b.cpu + h.slot * 4 * Page, b.pages[h.slot], size_t(bytes / Page), bytes, d.registryId};
    ++b.pins;
    return MemoryStatus::Ok;
}
MemoryStatus bUnpin(void *c, const DeviceIdentity &, uint64_t, const MemoryHandle &, DmaPin &pin) {
    ++static_cast<Backend *>(c)->unpins;
    pin = {};
    return MemoryStatus::Ok;
}
MemoryStatus bSync(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, CacheDirection) {
    return MemoryStatus::Ok;
}
MemoryStatus bMap(void *c, const DeviceIdentity &d, uint64_t o, const MemoryHandle &h, const DmaPin &p,
                  bool writable, GpuMapping &m) {
    auto &b = *static_cast<Backend *>(c);
    return b.vm.map(b.vm.context, d, o, h, p, writable, m);
}
MemoryStatus bUnmap(void *c, const DeviceIdentity &d, uint64_t o, const MemoryHandle &h, const DmaPin &p,
                    GpuMapping &m) {
    auto &b = *static_cast<Backend *>(c);
    return b.vm.unmap(b.vm.context, d, o, h, p, m);
}
bool bRetired(void *c, const MemoryView &v) {
    auto &b = *static_cast<Backend *>(c);
    return b.vm.retired(b.vm.context, v);
}
bool bQuiesce(void *c, const DeviceIdentity &d) {
    auto &b = *static_cast<Backend *>(c);
    return b.vm.quiesce(b.vm.context, d);
}

void testMemoryOwnerEndToEnd() {
    World w;
    CHECK(w.attach() == MemoryStatus::Ok);
    Backend b;
    b.vm = w.adapter.vmOps(); b.device = w.device; b.owner = w.owner;
    b.cpu = static_cast<uint8_t *>(std::aligned_alloc(4096, MemoryOwner::MaxAllocations * 4 * Page));
    CHECK(b.cpu != nullptr);
    MemoryOwner owner;
    MemoryLimits limits {};
    limits.dmaAddressBits = 39; limits.gpuAddressBits = 39;
    limits.maxAllocationBytes = 4 * Page; limits.maxTotalBytes = 64 * Page;
    const MemoryBackend backend {&b, bAdmitted, bPin, bUnpin, bSync, bMap, bUnmap, bRetired, bQuiesce};
    CHECK(owner.initialize(w.identity.device, limits, backend) == MemoryStatus::Ok);
    MemoryHandle h {};
    CHECK(owner.allocate(5, 2 * Page, h) == MemoryStatus::Ok);
    CHECK(owner.map(5, h, true) == MemoryStatus::Ok);
    MemoryView view {};
    CHECK(owner.inspect(5, h, view) == MemoryStatus::Ok && view.state == MemoryState::Mapped);
    CHECK(view.mapping.address == 17 * MiB && view.mapping.bytes == 2 * Page);
    CHECK(World::entry(PoolFirst) == (b.pages[h.slot][0] | 1) && World::entry(PoolFirst + 1) == (b.pages[h.slot][1] | 1));
    CHECK(owner.retire(5, h) == MemoryStatus::Ok);
    CHECK(World::entry(PoolFirst) == 0 && World::entry(PoolFirst + 1) == 0 && b.unpins == 1);
    // An unacknowledged invalidation quarantines the slot and keeps the pin,
    // and later epoch quiescence does not turn it into a release.
    faults.stuck.insert(L::kRtcr);
    MemoryHandle h2 {};
    CHECK(owner.allocate(5, Page, h2) == MemoryStatus::Ok);
    CHECK(owner.map(5, h2, true) == MemoryStatus::Quarantined);
    CHECK(owner.inspect(5, h2, view) == MemoryStatus::Ok && view.state == MemoryState::Quarantined);
    CHECK(!empty(view.mapping));
    CHECK(owner.retire(5, h2) == MemoryStatus::Quarantined && b.unpins == 1);
    CHECK(owner.close() == MemoryStatus::Quarantined && b.unpins == 1);
    CHECK(w.authority.quiesceCalls == 1 && w.adapter.liveBindings() == 1);
    CHECK(w.adapter.detach() == MemoryStatus::Quarantined);
    std::free(b.cpu);
}
} // namespace

int main() {
    testAttach();
    testAttachRefusals();
    testFinalOwnerReleaseMayDestroyAdapter();
    testNativeAwakeProducerRequired();
    testPublishRoundTrip();
    testMapRefusals();
    testUncertainPublication();
    testUncertainInverse();
    testQuiesce();
    testMemoryOwnerEndToEnd();
    std::free(GlkGgttShim::bar0);
    GlkGgttShim::bar0 = nullptr;
    std::printf("GLK GGTT IOKit adapter (hosted injected transport): %u checks; no hardware touched\n", checks);
    return 0;
}

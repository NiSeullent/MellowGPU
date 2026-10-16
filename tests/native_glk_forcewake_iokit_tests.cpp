// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// HOSTED SHIM TEST of Mellow/GlkForcewakeIOKit.cpp compiled unchanged against
// tests/glk_forcewake_iokit_shim.hpp. BAR0 is plain host RAM with no forcewake
// behavior: it proves authority gating and exact mapped writes, and that the
// adapter fails closed without a hardware ack. Never hardware evidence.
#include "../Mellow/GlkForcewakeIOKit.hpp"
#include <cstdio>

using namespace MellowNativeGlk;
using Status = ForcewakeStatus;

static int failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); } } while (false)

namespace {
constexpr uint64_t Epoch = 7, Timeout = 5 * 1000 * 1000;

struct Fixture {
    IOPCIDevice *device = new IOPCIDevice;
    IOService *owner = new IOService;
    IOMemoryDescriptor *bar = new IOMemoryDescriptor;
    OSArray *memory = new OSArray;
    Fixture() {
        owner->provider = device;
        device->openOwner = owner;
        device->fixture32(0x00, 0x31858086);
        device->fixture16(0x04, 0x0006);
        device->fixture16(0x06, 0x0010);
        device->fixture32(0x08, 0x03000006);
        device->fixture32(0x10, 0xa0000004);
        device->fixture32(0x14, 0);
        device->fixture16(0x2c, 0x17aa);
        device->fixture16(0x2e, 0x3e77);
        device->fixture8(0x34, 0x40);
        device->fixture32(0x40, 0x0000d009); // vendor capability -> 0xd0
        device->fixture32(0xd0, 0x00000001); // PCI PM capability, end of list
        device->fixture16(0xd4, 0x0008);     // PMCSR: D0, No_Soft_Reset
        memory->add(bar);
        device->fixtureProperty(gIODeviceMemoryKey, memory);
    }
    ~Fixture() { memory->release(); bar->release(); owner->release(); device->release(); }
    static GlkPciExpectation expectation() {
        GlkPciExpectation x;
        x.device = 0x3185; x.revision = 0x06; x.subsystemVendor = 0x17aa; x.subsystemDevice = 0x3e77;
        x.bus = 0; x.slot = 2; x.function = 0; x.registryId = 0x100000123; x.epoch = Epoch;
        return x;
    }
};

void testInitializeWritesMappedBarAndGetFailsClosed() {
    Fixture f;
    IOKitForcewake fw;
    CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == Status::Ok);
    CHECK(f.bar->maps == 1 && f.bar->untouched());
    CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == Status::Busy);
    CHECK(fw.initialize(Epoch, Timeout) == Status::Ok);
    for (const auto &domain : Gen9Domains) {
        CHECK(f.bar->at(domain.set) == 0xffff0000U);
        CHECK(f.bar->at(domain.ack) == 0);
    }
    ForcewakeLease lease;
    const unsigned delays = GlkShim::delays;
    CHECK(fw.get(Epoch, RenderDomain, Timeout, lease) == Status::AckSetTimeout);
    CHECK(lease.slot == UINT32_MAX && fw.engine().state() == Gen9Forcewake::State::Faulted);
    CHECK(fw.engine().diagnostics().lastFallbackPasses == FallbackMaxPasses);
    CHECK(GlkShim::delays - delays == FallbackMaxPasses);
    CHECK(f.bar->at(0xa278) == 0x00010000U); // rollback was the final write
    CHECK(f.bar->at(0xa188) == 0xffff0000U && f.bar->at(0xa270) == 0xffff0000U);
    fw.detach(Epoch);
    CHECK(fw.engine().state() == Gen9Forcewake::State::Unbound);
}

void testRetainsOwnerThroughMappedLifetime() {
    Fixture f;
    IOKitForcewake fw;
    CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == Status::Ok);
    CHECK(f.owner->refs == 2 && f.device->refs == 2);
    fw.detach(Epoch);
    CHECK(f.owner->refs == 1 && f.device->refs == 1);
}

void testEpochAndPowerChangesRefuseBeforeMmio() {
    Fixture f;
    IOKitForcewake fw;
    CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == Status::Ok);
    CHECK(fw.initialize(Epoch + 1, Timeout) == Status::IdentityChanged && f.bar->untouched());
    f.device->powerState = 1;
    CHECK(fw.initialize(Epoch, Timeout) == Status::Unavailable && f.bar->untouched());
    f.device->powerState = kIOPCIDeviceOnState;
    f.device->fixture16(0xd4, 0x0003);
    CHECK(fw.initialize(Epoch, Timeout) == Status::Unavailable && f.bar->untouched());
    f.device->fixture16(0xd4, 0x0000);
    f.device->openOwner = nullptr;
    CHECK(fw.initialize(Epoch, Timeout) == Status::Ownership && f.bar->untouched());
    f.device->openOwner = f.owner;
    CHECK(fw.initialize(Epoch, 0) == Status::Invalid && f.bar->untouched());
    CHECK(fw.initialize(Epoch, Timeout) == Status::Ok && f.bar->at(0xa278) == 0xffff0000U);
    fw.detach(Epoch);
    ForcewakeLease lease;
    CHECK(fw.get(Epoch, RenderDomain, Timeout, lease) == Status::Unavailable);
}

IOPCIDevice *hookDevice = nullptr;
unsigned hookFrom = 0;

void testAuthorityLostMidOperationInvalidatesResult() {
    Fixture f;
    IOKitForcewake fw;
    CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == Status::Ok);
    hookDevice = f.device;
    hookFrom = GlkShim::clockCalls + 3; // deadline, engine base, then first poll sample
    GlkShim::clockHook = [](unsigned call) { if (call >= hookFrom) hookDevice->powerState = 1; };
    CHECK(fw.initialize(Epoch, Timeout) == Status::Unavailable);
    GlkShim::clockHook = nullptr;
    CHECK(f.bar->at(0xa278) == 0xffff0000U); // writes preceded the observed loss
    CHECK(fw.engine().state() == Gen9Forcewake::State::Unbound);
    f.device->powerState = kIOPCIDeviceOnState;
    ForcewakeLease lease;
    CHECK(fw.get(Epoch, RenderDomain, Timeout, lease) == Status::Unavailable);
    fw.detach(Epoch);
}

void testAttachRefusals() {
    struct Case { void (*mutate)(Fixture &); Status expected; };
    const Case cases[] = {
        {[](Fixture &f) { f.device->openOwner = nullptr; }, Status::Ownership},
        {[](Fixture &f) { f.owner->provider = nullptr; }, Status::Ownership},
        {[](Fixture &f) { f.device->fixture16(0xd4, 0x0003); }, Status::Unavailable},
        {[](Fixture &f) { f.device->fixture8(0x34, 0); }, Status::Unavailable},
        {[](Fixture &f) { f.device->fixture16(0x04, 0x0004); }, Status::Unavailable},
        {[](Fixture &f) { f.device->inactive = true; }, Status::Unavailable},
        {[](Fixture &f) { f.device->fixture32(0x00, 0x31848086); }, Status::IdentityChanged},
        {[](Fixture &f) { f.device->fixture16(0x2e, 0x3e78); }, Status::IdentityChanged},
        {[](Fixture &f) { f.device->registryId = 1; }, Status::IdentityChanged},
        {[](Fixture &f) { f.device->fixture32(0x10, 0xa0000000); }, Status::Unsupported},
        {[](Fixture &f) { auto *o = new OSObject; f.device->fixtureProperty(kIOPCITunnelledKey, o); o->release(); },
         Status::Unsupported},
        {[](Fixture &f) { f.bar->tag ^= 1U << 11; }, Status::Unavailable},
        {[](Fixture &f) { f.bar->physical = 0xb0000000; }, Status::Unavailable},
        {[](Fixture &f) { f.bar->length = RegisterWindowBytes / 2; }, Status::Unavailable},
        {[](Fixture &f) { f.device->fixtureProperty(gIODeviceMemoryKey, nullptr); }, Status::Unavailable},
        {[](Fixture &f) { f.bar->mapPhysicalOverride = 0xc0000000; }, Status::IdentityChanged},
    };
    for (const auto &c : cases) {
        const unsigned before = GlkShim::alive;
        {
            Fixture f;
            c.mutate(f);
            IOKitForcewake fw;
            CHECK(fw.attach(f.device, f.owner, Fixture::expectation()) == c.expected);
            CHECK(f.bar->untouched() && f.device->refs == 1 && f.bar->refs == 2);
            CHECK(fw.engine().state() == Gen9Forcewake::State::Unbound);
        }
        CHECK(GlkShim::alive == before);
    }
    Fixture f;
    IOKitForcewake fw;
    auto notGlk = Fixture::expectation(); notGlk.device = 0x5917;
    CHECK(fw.attach(f.device, f.owner, notGlk) == Status::Unsupported && f.bar->maps == 0);
    CHECK(fw.attach(nullptr, f.owner, Fixture::expectation()) == Status::Ownership);
}
} // namespace

int main() {
    const unsigned baseline = GlkShim::alive;
    testInitializeWritesMappedBarAndGetFailsClosed();
    testRetainsOwnerThroughMappedLifetime();
    testEpochAndPowerChangesRefuseBeforeMmio();
    testAuthorityLostMidOperationInvalidatesResult();
    testAttachRefusals();
    CHECK(GlkShim::alive == baseline);
    std::printf("native_glk_forcewake_iokit_tests: %d checks, %d failures "
                "(hosted IOKit shim over plain RAM; not hardware proof)\n", checks, failures);
    return failures ? 1 : 0;
}

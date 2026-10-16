// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// HOSTED INJECTED-MMIO TESTS. A software model of the two GLK PPS instances,
// a simulated clock and fake power references stand in for hardware. Passing
// proves protocol logic only; it is not evidence of panel control on a device.
#include "../Drivers/NativeGlk/PanelPower.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>
using namespace MellowNativeGlk;
static_assert(!std::is_copy_constructible<GlkPanelPowerSequencer>::value, "power obligations have one owner");
static_assert(!std::is_move_constructible<GlkPanelPowerSequencer>::value, "power obligations cannot be moved implicitly");
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); abort(); } } while (0)

struct LoggedWrite { uint32_t offset, value; uint64_t t; };
struct Fake {
    enum Phase : uint8_t { OffIdle, PowerUp, OnIdle, PowerDown, Cycle };
    uint32_t control[2] {}, on[2] {}, off[2] {};
    Phase phase[2] {OffIdle, OffIdle};
    uint64_t phaseEnd[2] {};
    uint64_t t {1000000}, upUs {200000}, downUs {300000}, lastAuxReleaseT {};
    bool stuck {}, clockStuck {}, clockFail {}, ignoreDelayWrites {}, coreFail {}, auxFail {};
    bool cancelFlag {}, allOnes {}, ownerValid {true};
    uint64_t previousNow {};
    unsigned reads {}, writesCount {}, sleeps {}, stray {}, noCore {}, statusReads {};
    unsigned coreAcq {}, coreRel {}, auxAcq {}, auxRel {}, controlWrites {}, nowCalls {};
    unsigned earlyPowerOn {}, earlyVdd {};
    int failReadAt {-1}, failWriteAt {-1}, cancelAtSleep {-1}, cancelAfterControlWrites {-1};
    int regressAtNowCall {-1};
    int loseOwnerAtRead {-1}, loseOwnerAtWrite {-1};
    int64_t statusOverride {-1};
    int overrideAfterControlWrites {-1};
    bool reenterAtCore {};
    int coreHeld {};
    LoggedWrite log[64] {};
    unsigned logCount {};
    GlkPanelPowerSequencer *reenter {};
    PpsStatus reenterStatus {PpsStatus::Ok};
    bool reentered {};

    void bios() {
        for (unsigned i = 0; i < 2; ++i) {
            on[i] = (1500U << 16) | 1; off[i] = (3000U << 16) | 1; control[i] = 7U << 4;
        }
    }
    void litPanel(unsigned i) {
        control[i] = PanelPowerOn | PanelPowerReset | EdpBlcEnable | (7U << 4); phase[i] = OnIdle;
    }
    void dc9Reset() {
        for (unsigned i = 0; i < 2; ++i) { control[i] = on[i] = off[i] = 0; phase[i] = OffIdle; }
    }
    static bool decode(uint32_t offset, unsigned &i, uint32_t &reg) {
        if (offset < 0xC7200 || offset > 0xC730C) return false;
        i = (offset - 0xC7200) / 0x100; reg = (offset - 0xC7200) % 0x100;
        return i < 2 && (reg == 0 || reg == 4 || reg == 8 || reg == 0xC);
    }
    void advance(unsigned i) {
        if (stuck) return;
        if (phase[i] == PowerUp && t >= phaseEnd[i]) phase[i] = OnIdle;
        if (phase[i] == PowerDown && t >= phaseEnd[i]) {
            const uint32_t field = (control[i] >> 4) & 0x1f;
            phase[i] = Cycle; phaseEnd[i] += field ? (field - 1) * 100000ULL : 0;
        }
        if (phase[i] == Cycle && t >= phaseEnd[i]) phase[i] = OffIdle;
    }
    uint32_t statusOf(unsigned i) {
        advance(i);
        switch (phase[i]) {
            case PowerUp: return PpSequencePowerUp | 0x9U;
            case OnIdle: return PpOn | PpSequenceStateOnIdle;
            case PowerDown: return PpOn | PpSequencePowerDown | 0x1U;
            case Cycle: return PpCycleDelayActive;
            default: return 0;
        }
    }
    static bool read(void *p, uint32_t offset, uint32_t &value) {
        auto &f = *static_cast<Fake *>(p); ++f.reads;
        unsigned i = 0; uint32_t reg = 0;
        if (!decode(offset, i, reg)) { ++f.stray; return false; }
        if (f.coreHeld <= 0) ++f.noCore;
        if (f.failReadAt >= 0 && f.reads == unsigned(f.failReadAt)) return false;
        if (f.allOnes) { value = 0xffffffffU; return true; }
        if (reg == 0) ++f.statusReads;
        value = reg == 0 ? f.statusOf(i) : reg == 4 ? f.control[i] : reg == 8 ? f.on[i] : f.off[i];
        if (reg == 0 && f.statusOverride >= 0 && int(f.controlWrites) >= f.overrideAfterControlWrites)
            value = uint32_t(f.statusOverride);
        if (f.loseOwnerAtRead == int(f.reads)) f.ownerValid = false;
        return true;
    }
    static bool write(void *p, uint32_t offset, uint32_t value) {
        auto &f = *static_cast<Fake *>(p); ++f.writesCount;
        unsigned i = 0; uint32_t reg = 0;
        if (!decode(offset, i, reg) || reg == 0) { ++f.stray; return false; }
        if (f.coreHeld <= 0) ++f.noCore;
        if (f.failWriteAt >= 0 && f.writesCount == unsigned(f.failWriteAt)) return false;
        if (f.logCount < 64) f.log[f.logCount++] = {offset, value, f.t};
        if (reg == 4) {
            ++f.controlWrites; f.advance(i);
            const bool was = f.control[i] & PanelPowerOn, now = value & PanelPowerOn;
            if (!(f.control[i] & EdpForceVdd) && (value & EdpForceVdd) && f.phase[i] == Cycle) ++f.earlyVdd;
            f.control[i] = value;
            if (!was && now) {
                if (f.phase[i] != OffIdle) ++f.earlyPowerOn;
                f.phase[i] = PowerUp; f.phaseEnd[i] = f.t + f.upUs;
            }
            if (was && !now) { f.phase[i] = PowerDown; f.phaseEnd[i] = f.t + f.downUs; }
        } else if (!f.ignoreDelayWrites) {
            (reg == 8 ? f.on[i] : f.off[i]) = value;
        }
        if (f.loseOwnerAtWrite == int(f.writesCount)) f.ownerValid = false;
        return true;
    }
    static bool now(void *p, uint64_t &value) {
        auto &f = *static_cast<Fake *>(p); ++f.nowCalls;
        if (f.clockFail) return false;
        value = f.t;
        if (f.regressAtNowCall >= 0 && f.nowCalls == unsigned(f.regressAtNowCall)) value = f.previousNow - 1;
        f.previousNow = value;
        return true;
    }
    static bool sleep(void *p, uint64_t us) {
        auto &f = *static_cast<Fake *>(p); ++f.sleeps;
        if (f.reenter && !f.reentered) { f.reentered = true; f.reenterStatus = f.reenter->vddOff().status; }
        if ((f.cancelAtSleep >= 0 && f.sleeps == unsigned(f.cancelAtSleep)) ||
            (f.cancelAfterControlWrites >= 0 && f.controlWrites >= unsigned(f.cancelAfterControlWrites))) {
            f.cancelFlag = true; f.cancelAtSleep = f.cancelAfterControlWrites = -1; return false;
        }
        if (!f.clockStuck) f.t += us;
        return true;
    }
    static bool cancelled(void *p) { return static_cast<Fake *>(p)->cancelFlag; }
    static bool coreGet(void *p) {
        auto &f = *static_cast<Fake *>(p); if (f.coreFail) return false;
        if (f.reenterAtCore && f.reenter && !f.reentered) {
            f.reentered = true; f.reenterStatus = f.reenter->vddOff().status;
        }
        ++f.coreAcq; ++f.coreHeld; return true;
    }
    static void corePut(void *p) { auto &f = *static_cast<Fake *>(p); ++f.coreRel; --f.coreHeld; }
    static bool auxGet(void *p) {
        auto &f = *static_cast<Fake *>(p); if (f.auxFail) return false;
        ++f.auxAcq; return true;
    }
    static void auxPut(void *p) { auto &f = *static_cast<Fake *>(p); ++f.auxRel; f.lastAuxReleaseT = f.t; }
    static bool ownerCurrent(void *p, const DeviceIdentity &d) {
        return static_cast<Fake *>(p)->ownerValid && d.vendor == 0x8086 && d.device == 0x3185 &&
            d.registryId == 0x1234 && d.epoch == 7;
    }
    PanelPowerAccess access() {
        return {this, read, write, now, sleep, cancelled, coreGet, corePut, auxGet, auxPut, ownerCurrent};
    }
    void clean() const { CHECK(stray == 0 && noCore == 0 && coreHeld == 0 && coreAcq == coreRel); }
};

// Synthetic VBT facts: not values from any captured 15IGL7 VBT.
static PanelPowerConfig config(unsigned controller = 0) {
    PanelPowerConfig c {};
    c.device = {0x8086, 0x3185, 0x1234, 7};
    c.displayOwnershipGranted = true;
    c.vbt.state = FactState::Observed;
    c.vbt.bdbVersion = 228;
    c.vbt.backlightEntryValid = c.vbt.pwmBacklight = true;
    c.vbt.backlightController = uint8_t(controller);
    c.vbt.edpBlock = true;
    c.vbt.edpPowerSequence = {1200, 600, 300, 4000, 4500};
    return c;
}
static bool sameTiming(const PpsTiming &a, const PpsTiming &b) { return memcmp(&a, &b, sizeof a) == 0; }
static void expectLog(const Fake &f, unsigned from, const LoggedWrite *expected, unsigned count) {
    CHECK(f.logCount == from + count);
    for (unsigned i = 0; i < count; ++i)
        CHECK(f.log[from + i].offset == expected[i].offset && f.log[from + i].value == expected[i].value);
}

static void pureTests() {
    uint32_t offset = 0;
    CHECK(ppsRegisterOffset(0, PpStatusReg, offset) && offset == 0xC7200);
    CHECK(ppsRegisterOffset(0, PpControlReg, offset) && offset == 0xC7204);
    CHECK(ppsRegisterOffset(0, PpOnDelaysReg, offset) && offset == 0xC7208);
    CHECK(ppsRegisterOffset(0, PpOffDelaysReg, offset) && offset == 0xC720C);
    CHECK(ppsRegisterOffset(1, PpStatusReg, offset) && offset == 0xC7300);
    CHECK(ppsRegisterOffset(1, PpControlReg, offset) && offset == 0xC7304);
    CHECK(ppsRegisterOffset(1, PpOffDelaysReg, offset) && offset == 0xC730C);
    offset = 99; CHECK(!ppsRegisterOffset(2, PpStatusReg, offset) && offset == 0);
    offset = 99; CHECK(!ppsRegisterOffset(0, 0x61210, offset) && offset == 0); // no PP_DIVISOR on GLK
    CHECK(!ppsRegisterOffset(0, 0x61201, offset) && !ppsRegisterOffset(0, 0xC7200, offset));
    CHECK(glkDevice(0x8086, 0x3184) && glkDevice(0x8086, 0x3185) && !glkDevice(0x10de, 0x3185));
    for (uint32_t d = 0; d <= 0xffff; ++d)
        if (d != 0x3184 && d != 0x3185) CHECK(!glkDevice(0x8086, uint16_t(d)));

    for (uint32_t field = 0; field < 32; ++field) {
        const PpsDelays d = decodeHardwareDelays(0, 0, (field << 4) | 0xFFFFFE0FU);
        CHECK(d.powerCycle == (field ? (field - 1) * 1000 : 0));
        CHECK(!d.powerUp && !d.backlightOn && !d.backlightOff && !d.powerDown);
    }
    const PpsDelays d = decodeHardwareDelays(0xC0000000U | (0x1FFFU << 16) | 0xF234U,
        0xE0000000U | (0x0ABCU << 16) | 0xE123U, 0);
    CHECK(d.powerUp == 0x1FFF && d.backlightOn == 0x1234 && d.powerDown == 0x0ABC &&
        d.backlightOff == 0x0123 && d.powerCycle == 0);
    for (uint32_t units = 0; units <= 70000; ++units) {
        uint32_t field = 77; const uint32_t expect = (units + 999) / 1000 + 1;
        const bool ok = encodePowerCycleField(units, field);
        CHECK(ok == (expect <= 31) && field == (ok ? expect : 77U));
    }

    const PpsDelays spec = specificationDelays();
    CHECK(spec.powerUp == 2100 && spec.backlightOn == 500 && spec.backlightOff == 500 &&
        spec.powerDown == 5000 && spec.powerCycle == 5100);
    CHECK(!delaysPresent({}) && delaysPresent({0, 0, 0, 0, 1}) && delaysPresent({1, 0, 0, 0, 0}) &&
        delaysPresent({0, 1, 0, 0, 0}) && delaysPresent({0, 0, 1, 0, 0}) && delaysPresent({0, 0, 0, 1, 0}));

    PpsTiming t {};
    CHECK(computeTiming(decodeHardwareDelays(0x05DC0001, 0x0BB80001, 7U << 4),
        {1200, 600, 300, 4000, 4500}, t) == PpsStatus::Ok);
    CHECK(t.registers.powerUp == 1500 && t.registers.backlightOn == 1 && t.registers.backlightOff == 1 &&
        t.registers.powerDown == 4000 && t.registers.powerCycle == 6000);
    CHECK(t.waits.powerUp == 150 && t.waits.backlightOn == 60 && t.waits.backlightOff == 30 &&
        t.waits.powerDown == 400 && t.waits.powerCycle == 600);
    CHECK(t.source[PowerUp] == DelaySource::Hardware && t.source[BacklightOn] == DelaySource::Vbt &&
        t.source[BacklightOff] == DelaySource::Vbt && t.source[PowerDown] == DelaySource::Vbt &&
        t.source[PowerCycle] == DelaySource::Hardware);
    CHECK(t.ppOn == 0x05DC0001 && t.ppOff == 0x0FA00001 && t.cycleField == 7);
    // Both sources zero: eDP 1.3 limits. The wait uses 510 ms; the register is
    // rounded to the hardware's 100 ms granularity (6000 units, field 7).
    CHECK(computeTiming({}, {}, t) == PpsStatus::Ok);
    CHECK(t.registers.powerUp == 2100 && t.registers.backlightOn == 1 && t.registers.backlightOff == 1 &&
        t.registers.powerDown == 5000 && t.registers.powerCycle == 6000 && t.cycleField == 7);
    CHECK(t.waits.powerUp == 210 && t.waits.backlightOn == 50 && t.waits.backlightOff == 50 &&
        t.waits.powerDown == 500 && t.waits.powerCycle == 510);
    for (unsigned i = 0; i < DelayFieldCount; ++i) CHECK(t.source[i] == DelaySource::Specification);
    CHECK(computeTiming({}, {2101, 0, 0, 0, 1}, t) == PpsStatus::Ok);
    CHECK(t.waits.powerUp == 211 && t.registers.powerCycle == 1000 && t.cycleField == 2 && t.waits.powerCycle == 1);
    CHECK(t.source[PowerUp] == DelaySource::Vbt && t.source[PowerDown] == DelaySource::Specification);
    CHECK(computeTiming({}, {0, 65535, 65535, 0, 30000}, t) == PpsStatus::Ok);
    CHECK(t.waits.backlightOn == 6554 && t.registers.backlightOn == 1 && t.cycleField == 31);
    const PpsTiming sentinel = t;
    const PpsDelays bad[] = {{0x2000, 0, 0, 0, 0}, {0, 0, 0, 0x2000, 0}, {0, 0, 0, 0, 30001}, {0, 0, 0, 0, 65535}};
    for (const auto &b : bad) {
        CHECK(computeTiming({}, b, t) == PpsStatus::DelayOutOfRange && sameTiming(t, sentinel));
        CHECK(computeTiming(b, {}, t) == PpsStatus::DelayOutOfRange && sameTiming(t, sentinel));
    }

    VbtPanelPowerFacts v = config().vbt;
    unsigned controller = 9; ControllerSource source = ControllerSource::Vbt;
    CHECK(resolveController(v, controller, source) == PpsStatus::Ok && controller == 0 && source == ControllerSource::Vbt);
    v.backlightController = 1; v.bdbVersion = 191;
    CHECK(resolveController(v, controller, source) == PpsStatus::Ok && controller == 1 && source == ControllerSource::Vbt);
    v.bdbVersion = 190;
    CHECK(resolveController(v, controller, source) == PpsStatus::Ok && controller == 0 &&
        source == ControllerSource::VbtBeforeVersion191);
    v.bdbVersion = 191;
    for (unsigned c = 2; c < 256; ++c) {
        v.backlightController = uint8_t(c); controller = 9; source = ControllerSource::Vbt;
        CHECK(resolveController(v, controller, source) == PpsStatus::Invalid &&
            controller == 0 && source == ControllerSource::Unknown);
    }
    VbtPanelPowerFacts missing[3] = {config().vbt, config().vbt, config().vbt};
    missing[0].state = FactState::Unknown; missing[1].backlightEntryValid = false; missing[2].pwmBacklight = false;
    for (const auto &m : missing) {
        controller = 9; source = ControllerSource::Vbt;
        CHECK(resolveController(m, controller, source) == PpsStatus::PrerequisiteUnknown &&
            controller == 0 && source == ControllerSource::Unknown);
    }
}

static void expectInitFailure(Fake &f, const PanelPowerConfig &c, PpsStatus expected, bool noMmio) {
    GlkPanelPowerSequencer s;
    const PpsResult r = s.initialize(c, f.access());
    CHECK(r.status == expected && r.writes == 0 && r.obligations == 0 && f.writesCount == 0);
    if (noMmio) CHECK(f.reads == 0);
    CHECK(!s.state().initialized && s.state().obligations == 0 && f.auxAcq == f.auxRel);
    CHECK(s.vddOn().status == PpsStatus::Invalid && s.panelOn().status == PpsStatus::Invalid &&
        s.reconcile().status == PpsStatus::Invalid && f.writesCount == 0);
    CHECK(f.stray == 0 && f.coreAcq == f.coreRel);
}

static void initializeTests() {
    for (unsigned missing = 0; missing < 10; ++missing) {
        Fake f; f.bios(); PanelPowerAccess a = f.access();
        switch (missing) {
            case 0: a.context = nullptr; break; case 1: a.read32 = nullptr; break;
            case 2: a.write32 = nullptr; break; case 3: a.nowUs = nullptr; break;
            case 4: a.sleepUs = nullptr; break; case 5: a.cancelled = nullptr; break;
            case 6: a.acquireDisplayCorePower = nullptr; break; case 7: a.releaseDisplayCorePower = nullptr; break;
            case 8: a.acquireAuxPower = nullptr; break; default: a.releaseAuxPower = nullptr; break;
        }
        GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), a).status == PpsStatus::Unavailable && !s.state().initialized);
        CHECK(f.reads == 0 && f.writesCount == 0 && f.coreAcq == 0 && f.sleeps == 0);
    }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.device.vendor = 0x10de; expectInitFailure(f, c, PpsStatus::Unsupported, true); CHECK(f.coreAcq == 0); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.device.device = 0x3e9b; expectInitFailure(f, c, PpsStatus::Unsupported, true); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.device.registryId = 0; expectInitFailure(f, c, PpsStatus::Invalid, true); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.device.epoch = 0; expectInitFailure(f, c, PpsStatus::Invalid, true); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.displayOwnershipGranted = false; expectInitFailure(f, c, PpsStatus::Ownership, true); CHECK(f.coreAcq == 0); }
    // The real 15IGL7 has no VBT capture: unknown facts stop before any access.
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.vbt = {}; expectInitFailure(f, c, PpsStatus::PrerequisiteUnknown, true); CHECK(f.coreAcq == 0); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.vbt.pwmBacklight = false; expectInitFailure(f, c, PpsStatus::PrerequisiteUnknown, true); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.vbt.backlightController = 2; expectInitFailure(f, c, PpsStatus::Invalid, true); }
    { Fake f; f.bios(); f.coreFail = true; expectInitFailure(f, config(), PpsStatus::Unavailable, true); }
    { Fake f; f.bios(); f.allOnes = true; expectInitFailure(f, config(), PpsStatus::Unavailable, false); CHECK(f.coreAcq == 1); }
    for (int at = 1; at <= 6; ++at) { Fake f; f.bios(); f.failReadAt = at; expectInitFailure(f, config(), PpsStatus::IoFailure, false); }
    { Fake f; f.bios(); f.stuck = true; f.phase[0] = Fake::PowerUp; expectInitFailure(f, config(), PpsStatus::Busy, false); }
    { Fake f; f.bios(); f.stuck = true; f.phase[0] = Fake::PowerDown; expectInitFailure(f, config(), PpsStatus::Busy, false); }
    // VBT names PPS 0 but only PPS 1 is lit or has VDD forced.
    { Fake f; f.bios(); f.litPanel(1); expectInitFailure(f, config(0), PpsStatus::ControllerConflict, false); }
    { Fake f; f.bios(); f.control[1] |= EdpForceVdd; expectInitFailure(f, config(0), PpsStatus::ControllerConflict, false); }
    { Fake f; f.bios(); PanelPowerConfig c = config(); c.vbt.edpPowerSequence.powerUp = 0x2000; expectInitFailure(f, c, PpsStatus::DelayOutOfRange, false); }
    { Fake f; f.bios(); f.on[0] = 0; f.off[0] = 0; f.control[0] = 0x1FU << 4 | 0; PanelPowerConfig c = config(); c.vbt.edpPowerSequence.powerCycle = 30001; expectInitFailure(f, c, PpsStatus::DelayOutOfRange, false); }
    { Fake f; f.bios(); f.control[0] |= EdpForceVdd; f.auxFail = true; expectInitFailure(f, config(), PpsStatus::Unavailable, false); CHECK(f.auxAcq == 0); }

    Fake f; f.bios(); GlkPanelPowerSequencer s;
    const PpsResult r = s.initialize(config(), f.access());
    CHECK(r.status == PpsStatus::Ok && r.writes == 0 && f.writesCount == 0 && f.reads == 6 && f.sleeps == 0);
    CHECK(s.state().initialized && !s.state().registersProgrammed && s.state().controller == 0 &&
        s.state().controllerSource == ControllerSource::Vbt && s.state().obligations == 0);
    CHECK(s.state().bios.powerUp == 1500 && s.state().bios.powerDown == 3000 && s.state().bios.powerCycle == 6000);
    CHECK(s.state().timing.ppOn == 0x05DC0001 && s.state().timing.ppOff == 0x0FA00001 && s.state().timing.cycleField == 7);
    CHECK(!s.state().panelOffKnown && !s.state().powerOnKnown && !s.state().backlightOffKnown);
    CHECK(s.initialize(config(), f.access()).status == PpsStatus::Invalid && f.writesCount == 0);
    f.clean();
}

static void fullSequenceTest() {
    Fake f; f.bios(); GlkPanelPowerSequencer s;
    CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
    const uint64_t start = f.t;
    PpsResult r = s.vddOn();
    CHECK(r.status == PpsStatus::Ok && r.writes == 4);
    const LoggedWrite vdd[] = {{0xC7208, 0x05DC0001, 0}, {0xC720C, 0x0FA00001, 0}, {0xC7204, 0x70, 0}, {0xC7204, 0x78, 0}};
    expectLog(f, 0, vdd, 4);
    // Off instant unknown: the full T11+T12 (600 ms) plus the IDLE_CYCLE poll
    // precede VDD, then the 150 ms power-up settle follows it.
    CHECK(f.log[3].t >= start + 600000 + StatusPollIntervalUs && f.t >= f.log[3].t + 150000);
    CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced) && f.auxAcq == 1 && s.state().vddSettled);
    CHECK(s.vddOn().status == PpsStatus::Ok && f.logCount == 4 && f.auxAcq == 1); // already forced

    r = s.panelOn();
    const LoggedWrite on[] = {{0xC7204, 0x7B, 0}};
    CHECK(r.status == PpsStatus::Ok && r.writes == 1 && (r.ppStatus & IdleOnMask) == IdleOnValue);
    expectLog(f, 4, on, 1);
    CHECK(s.state().powerOnKnown && s.state().powerOnUs >= f.log[4].t + f.upUs);
    CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced | ObligationPanelPower));
    CHECK(s.panelOn().status == PpsStatus::StateMismatch && f.logCount == 5);

    r = s.vddOff();
    const LoggedWrite vddOff[] = {{0xC7204, 0x73, 0}};
    CHECK(r.status == PpsStatus::Ok && r.writes == 1 && r.obligations == ObligationPanelPower && f.auxRel == 1);
    expectLog(f, 5, vddOff, 1);
    CHECK(s.vddOff().status == PpsStatus::Ok && f.logCount == 6); // nothing to drop
    CHECK(s.panelOff().status == PpsStatus::StateMismatch && f.logCount == 6); // needs forced VDD

    r = s.backlightOn();
    const LoggedWrite blcOn[] = {{0xC7204, 0x77, 0}};
    CHECK(r.status == PpsStatus::Ok && r.writes == 1 && (r.obligations & ObligationBacklight));
    expectLog(f, 6, blcOn, 1);
    CHECK(f.log[6].t >= s.state().powerOnUs + 60000);
    CHECK(s.backlightOn().status == PpsStatus::StateMismatch && f.logCount == 7);

    r = s.backlightOff();
    const LoggedWrite blcOff[] = {{0xC7204, 0x73, 0}};
    CHECK(r.status == PpsStatus::Ok && r.writes == 1 && r.obligations == ObligationPanelPower);
    expectLog(f, 7, blcOff, 1);
    CHECK(f.t >= f.log[7].t + 30000 && s.state().backlightOffKnown);

    CHECK(s.vddOn().status == PpsStatus::Ok && f.auxAcq == 2);
    const LoggedWrite vdd2[] = {{0xC7204, 0x7B, 0}};
    expectLog(f, 8, vdd2, 1);
    r = s.panelOff();
    const LoggedWrite off[] = {{0xC7204, 0x70, 0}};
    CHECK(r.status == PpsStatus::Ok && r.writes == 1 && r.obligations == 0 && (r.ppStatus & IdleOffMask) == IdleOffValue);
    expectLog(f, 9, off, 1);
    CHECK(f.auxRel == 2 && f.lastAuxReleaseT >= f.log[9].t + f.downUs);
    CHECK(s.state().panelOffKnown && !s.state().wantVdd && s.state().pending == PendingWait::None);

    // Measured T11+T12 from the recorded off instant.
    CHECK(s.vddOn().status == PpsStatus::Ok);
    CHECK(f.log[10].value == 0x78 && f.log[10].t >= s.state().panelOffUs + 600000);
    CHECK(s.panelOn().status == PpsStatus::Ok && s.vddOff().status == PpsStatus::Ok);
    CHECK(s.vddOn().status == PpsStatus::Ok && s.panelOff().status == PpsStatus::Ok);
    CHECK(s.state().obligations == 0 && f.auxAcq == f.auxRel && f.earlyPowerOn == 0 && f.earlyVdd == 0);
    f.clean();
}

static void controllerOneTest() {
    Fake f; f.bios(); GlkPanelPowerSequencer s;
    CHECK(s.initialize(config(1), f.access()).status == PpsStatus::Ok && s.state().controller == 1);
    CHECK(s.vddOn().status == PpsStatus::Ok && s.panelOn().status == PpsStatus::Ok);
    CHECK(s.backlightOn().status == PpsStatus::Ok && s.backlightOff().status == PpsStatus::Ok);
    CHECK(s.panelOff().status == PpsStatus::Ok && s.state().obligations == 0);
    for (unsigned i = 0; i < f.logCount; ++i) CHECK(f.log[i].offset >= 0xC7300 && f.log[i].offset <= 0xC730C);
    CHECK(f.control[0] == 0x70 && f.on[0] == 0x05DC0001 && f.off[0] == 0x0BB80001 && f.phase[0] == Fake::OffIdle);
    f.clean();
}

static void timeoutAndClockTests() {
    {   // PP_ON alone cannot prove that the sequencer reached IDLE_ON.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.statusOverride = PpOn;
        f.overrideAfterControlWrites = int(f.controlWrites) + 1;
        CHECK(s.panelOn().status == PpsStatus::Timeout);
        CHECK((s.state().obligations & ObligationIndeterminate) && f.auxRel == 0);
        f.clean();
    }
    {
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.stuck = true;
        const PpsResult r = s.panelOn();
        const uint64_t elapsed = f.t - f.log[f.logCount - 1].t;
        CHECK(r.status == PpsStatus::Timeout && elapsed >= StatusPollTimeoutUs &&
            elapsed <= StatusPollTimeoutUs + 2 * StatusPollIntervalUs);
        CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced | ObligationPanelPower | ObligationIndeterminate));
        CHECK(s.state().pending == PendingWait::PanelOn && f.control[0] == 0x7B); // no rollback write
        const unsigned logged = f.logCount;
        CHECK(s.vddOff().status == PpsStatus::ObligationPending && s.panelOff().status == PpsStatus::ObligationPending &&
            s.backlightOn().status == PpsStatus::ObligationPending && f.logCount == logged && f.auxRel == 0);
        CHECK(s.reconcile().status == PpsStatus::Timeout && f.logCount == logged); // still stuck, still owed
        f.stuck = false;
        const PpsResult q = s.reconcile();
        CHECK(q.status == PpsStatus::Ok && q.writes == 0 && f.logCount == logged);
        CHECK(q.obligations == (ObligationAuxPower | ObligationVddForced | ObligationPanelPower) && s.state().powerOnKnown);
        CHECK(s.reconcile().status == PpsStatus::Ok && s.panelOff().status == PpsStatus::Ok && s.state().obligations == 0);
        f.clean();
    }
    {   // A stopped clock cannot hold the poll open forever.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.stuck = f.clockStuck = true;
        const unsigned before = f.statusReads;
        CHECK(s.panelOn().status == PpsStatus::Timeout);
        CHECK(f.statusReads - before >= StatusPollMaxReads && f.statusReads - before <= StatusPollMaxReads + 2);
        f.clean();
    }
    {   // Clock failure or regression before any write leaves no new effect.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        f.clockFail = true;
        CHECK(s.vddOn().status == PpsStatus::ClockFault && f.auxAcq == f.auxRel && s.state().obligations == 0);
        f.clockFail = false; f.regressAtNowCall = int(f.nowCalls) + 2;
        CHECK(s.vddOn().status == PpsStatus::ClockFault && f.auxAcq == f.auxRel && s.state().obligations == 0);
        CHECK(f.control[0] == 0x70 && !(f.control[0] & EdpForceVdd));
        f.clean();
    }
}

static void cancellationTests() {
    {   // During the T11+T12 wait before VDD: no VDD write, own AUX reference returned.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        CHECK(s.vddOn().status == PpsStatus::Ok && s.vddOff().status == PpsStatus::Ok);
        const unsigned logged = f.logCount;
        f.cancelAtSleep = int(f.sleeps) + 1;
        const PpsResult r = s.vddOn();
        CHECK(r.status == PpsStatus::Cancelled && r.writes == 0 && f.logCount == logged);
        CHECK(r.obligations == 0 && f.auxAcq == f.auxRel && !(f.control[0] & EdpForceVdd));
        CHECK(s.vddOn().status == PpsStatus::Cancelled && f.logCount == logged); // still cancelled
        f.cancelFlag = false;
        CHECK(s.vddOn().status == PpsStatus::Ok);
        f.clean();
    }
    {   // During the VDD settle: VDD and AUX stay owed until settle completes.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        f.cancelAfterControlWrites = 2; // programming RMW, then FORCE_VDD
        const PpsResult r = s.vddOn();
        CHECK(r.status == PpsStatus::Cancelled && r.writes == 4 && f.control[0] == 0x78);
        CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced) && s.state().pending == PendingWait::VddSettle);
        f.cancelFlag = false;
        CHECK(s.panelOn().status == PpsStatus::ObligationPending && s.backlightOn().status == PpsStatus::ObligationPending);
        const uint64_t vddAt = f.log[3].t;
        CHECK(s.reconcile().status == PpsStatus::Ok && f.t >= vddAt + 150000 && s.state().vddSettled);
        CHECK(s.panelOn().status == PpsStatus::Ok);
        f.clean();
    }
    {   // During IDLE_ON: POWER_ON stays requested; reconcile observes completion.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.cancelAfterControlWrites = int(f.controlWrites) + 1;
        const PpsResult r = s.panelOn();
        CHECK(r.status == PpsStatus::Cancelled && f.control[0] == 0x7B);
        CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced | ObligationPanelPower | ObligationIndeterminate));
        f.cancelFlag = false;
        CHECK(s.vddOff().status == PpsStatus::ObligationPending);
        CHECK(s.reconcile().status == PpsStatus::Ok && s.state().pending == PendingWait::None && s.state().powerOnKnown);
        CHECK(s.state().obligations == (ObligationAuxPower | ObligationVddForced | ObligationPanelPower));
        f.clean();
    }
    {   // During IDLE_OFF: AUX power is not released while the panel may still sequence.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok &&
            s.panelOn().status == PpsStatus::Ok);
        f.cancelAfterControlWrites = int(f.controlWrites) + 1;
        const PpsResult r = s.panelOff();
        CHECK(r.status == PpsStatus::Cancelled && f.control[0] == 0x70 && f.auxRel == 0);
        CHECK(r.obligations == (ObligationAuxPower | ObligationPanelPower | ObligationIndeterminate));
        CHECK(!s.state().panelOffKnown && s.state().pending == PendingWait::PanelOff);
        f.cancelFlag = false;
        CHECK(s.vddOn().status == PpsStatus::ObligationPending && f.auxAcq == 1);
        const PpsResult q = s.reconcile();
        CHECK(q.status == PpsStatus::Ok && q.obligations == 0 && f.auxRel == 1 && s.state().panelOffKnown);
        f.clean();
    }
    {   // During T9: BLC is off; the later panel-off write still waits out T9.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok &&
            s.panelOn().status == PpsStatus::Ok && s.backlightOn().status == PpsStatus::Ok);
        f.cancelAfterControlWrites = int(f.controlWrites) + 1;
        CHECK(s.backlightOff().status == PpsStatus::Cancelled && !(f.control[0] & EdpBlcEnable));
        CHECK(!(s.state().obligations & ObligationBacklight));
        const uint64_t blcOffAt = f.log[f.logCount - 1].t;
        f.cancelFlag = false;
        CHECK(s.panelOff().status == PpsStatus::Ok && f.log[f.logCount - 1].t >= blcOffAt + 30000);
        f.clean();
    }
    {   // Cancel requested before the POWER_ON write: nothing written.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.cancelFlag = true; const unsigned logged = f.logCount;
        CHECK(s.panelOn().status == PpsStatus::Cancelled && f.logCount == logged);
        CHECK(s.state().obligations == (ObligationAuxPower | ObligationVddForced));
        f.clean();
    }
}

static void faultTests() {
    {   // Failed programming write: unverified registers, no VDD write, retry clears it.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        f.failWriteAt = 1;
        const PpsResult r = s.vddOn();
        CHECK(r.status == PpsStatus::IoFailure && r.obligations == ObligationRegistersUnverified);
        CHECK(f.auxAcq == f.auxRel && !(f.control[0] & EdpForceVdd) && !s.state().registersProgrammed);
        const unsigned logged = f.logCount;
        CHECK(s.vddOn().status == PpsStatus::Ok && f.logCount == logged + 4);
        CHECK(s.state().obligations == (ObligationAuxPower | ObligationVddForced));
        f.clean();
    }
    {   // Failed POWER_ON write: outcome unknown until reconcile reads PP_CONTROL.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        f.failWriteAt = int(f.writesCount) + 1;
        const PpsResult r = s.panelOn();
        CHECK(r.status == PpsStatus::IoFailure && (r.obligations & (ObligationPanelPower | ObligationIndeterminate)) ==
            (ObligationPanelPower | ObligationIndeterminate));
        const PpsResult q = s.reconcile();
        CHECK(q.status == PpsStatus::Ok && q.obligations == (ObligationAuxPower | ObligationVddForced));
        CHECK(s.panelOn().status == PpsStatus::Ok);
        f.clean();
    }
    {   // Delay registers that do not hold the programmed values stop the sequence.
        Fake f; f.bios(); f.ignoreDelayWrites = true; GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        const PpsResult r = s.vddOn();
        CHECK(r.status == PpsStatus::StateMismatch && r.obligations == ObligationRegistersUnverified);
        CHECK(!(f.control[0] & EdpForceVdd) && f.auxAcq == f.auxRel);
        f.clean();
    }
    {   // AUX power unavailable: no VDD write.
        Fake f; f.bios(); f.auxFail = true; GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        CHECK(s.vddOn().status == PpsStatus::Unavailable && f.writesCount == 0 && s.state().obligations == 0);
        f.clean();
    }
    {   // Display-core power unavailable: no MMIO at all.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        f.coreFail = true; const unsigned reads = f.reads;
        CHECK(s.vddOn().status == PpsStatus::Unavailable && f.reads == reads && f.writesCount == 0);
        f.clean();
    }
    {   // Nested call from a transport callback.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
        f.reenter = &s;
        CHECK(s.vddOn().status == PpsStatus::Ok && f.reentered && f.reenterStatus == PpsStatus::Busy);
        f.clean();
    }
}

static void handoverAndResetTests() {
    {   // Firmware left VDD forced: adopted with an AUX reference, then dropped.
        Fake f; f.bios(); f.control[0] |= EdpForceVdd; GlkPanelPowerSequencer s;
        const PpsResult r = s.initialize(config(), f.access());
        CHECK(r.status == PpsStatus::Ok && r.writes == 0 && f.auxAcq == 1);
        CHECK(r.obligations == (ObligationAuxPower | ObligationVddForced) && !s.state().wantVdd && !s.state().vddSettled);
        CHECK(s.panelOn().status == PpsStatus::ObligationPending);
        const PpsResult q = s.vddOff();
        const LoggedWrite expected[] = {{0xC7208, 0x05DC0001, 0}, {0xC720C, 0x0FA00001, 0}, {0xC7204, 0x78, 0}, {0xC7204, 0x70, 0}};
        CHECK(q.status == PpsStatus::Ok && q.obligations == 0 && f.auxRel == 1 && s.state().panelOffKnown);
        expectLog(f, 0, expected, 4);
        f.clean();
    }
    {   // Firmware-lit panel: adopted as owed panel power and backlight.
        Fake f; f.bios(); f.litPanel(0); GlkPanelPowerSequencer s;
        const PpsResult r = s.initialize(config(), f.access());
        CHECK(r.status == PpsStatus::Ok && r.writes == 0 && r.obligations == (ObligationPanelPower | ObligationBacklight));
        CHECK(!s.state().powerOnKnown);
        CHECK(s.panelOn().status == PpsStatus::StateMismatch && s.backlightOn().status == PpsStatus::StateMismatch);
        CHECK(f.writesCount == 0);
        CHECK(s.backlightOff().status == PpsStatus::Ok && s.state().obligations == ObligationPanelPower);
        CHECK(s.vddOn().status == PpsStatus::Ok && f.control[0] == 0x7B);
        CHECK(s.panelOff().status == PpsStatus::Ok && s.state().obligations == 0 && f.auxAcq == f.auxRel);
        f.clean();
    }
    {   // DC9 with nothing owed: the next access reprograms the reset registers.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok &&
            s.vddOff().status == PpsStatus::Ok);
        CHECK(s.noteDisplayPowerReset().status == PpsStatus::Ok && !s.state().registersProgrammed);
        f.dc9Reset(); const unsigned logged = f.logCount;
        CHECK(s.vddOn().status == PpsStatus::Ok);
        const LoggedWrite expected[] = {{0xC7208, 0x05DC0001, 0}, {0xC720C, 0x0FA00001, 0}, {0xC7204, 0x70, 0}, {0xC7204, 0x78, 0}};
        expectLog(f, logged, expected, 4);
        f.clean();
    }
    {   // DC9 while VDD was owed: power was lost unsequenced; reconcile from hardware.
        Fake f; f.bios(); GlkPanelPowerSequencer s;
        CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok && s.vddOn().status == PpsStatus::Ok);
        const PpsResult r = s.noteDisplayPowerReset();
        CHECK(r.status == PpsStatus::ObligationPending && (r.obligations & ObligationIndeterminate) &&
            s.state().pending == PendingWait::PowerLost);
        f.dc9Reset(); const unsigned logged = f.logCount;
        CHECK(s.vddOff().status == PpsStatus::ObligationPending && f.logCount == logged);
        const PpsResult q = s.reconcile();
        CHECK(q.status == PpsStatus::Ok && q.writes == 0 && q.obligations == 0 && f.auxAcq == f.auxRel);
        CHECK(!s.state().panelOffKnown);
        const uint64_t start = f.t;
        CHECK(s.vddOn().status == PpsStatus::Ok && f.logCount == logged + 4);
        CHECK(f.log[logged + 3].t >= start + 600000);
        f.clean();
    }
}

static void observeTests() {
    Fake f; f.bios(); f.litPanel(1); PpsObservation o {};
    CHECK(observePanelPower(config().device, f.access(), o) == PpsStatus::Ok);
    CHECK(f.reads == 8 && f.writesCount == 0 && f.sleeps == 0);
    CHECK(o.instance[0].status == 0 && o.instance[0].control == 0x70 && o.instance[0].onDelays == 0x05DC0001 &&
        o.instance[0].offDelays == 0x0BB80001);
    CHECK((o.instance[1].status & IdleOnMask) == IdleOnValue && o.instance[1].control == 0x77);
    f.clean();
    const PpsObservation zero {};
    { Fake g; g.bios(); g.failReadAt = 5; o.instance[0].control = 1;
      CHECK(observePanelPower(config().device, g.access(), o) == PpsStatus::IoFailure && !memcmp(&o, &zero, sizeof o)); g.clean(); }
    { Fake g; g.bios(); PanelPowerAccess a = g.access(); a.read32 = nullptr; o.instance[0].control = 1;
      CHECK(observePanelPower(config().device, a, o) == PpsStatus::Unavailable && !memcmp(&o, &zero, sizeof o) && g.coreAcq == 0); }
    { Fake g; g.bios(); DeviceIdentity d = config().device; d.device = 0x5917; o.instance[0].control = 1;
      CHECK(observePanelPower(d, g.access(), o) == PpsStatus::Unsupported && !memcmp(&o, &zero, sizeof o) && g.reads == 0); }
    { Fake g; g.bios(); g.coreFail = true;
      CHECK(observePanelPower(config().device, g.access(), o) == PpsStatus::Unavailable && g.reads == 0); }
}

static void lostOwnerTest() {
    Fake f; f.bios(); GlkPanelPowerSequencer s;
    CHECK(s.initialize(config(), f.access()).status == PpsStatus::Ok);
    const unsigned reads = f.reads, writes = f.writesCount;
    f.ownerValid = false;
    CHECK(s.vddOn().status == PpsStatus::Ownership);
    CHECK(f.reads == reads && f.writesCount == writes && f.auxAcq == 0);
    f.ownerValid = true;
    CHECK(s.reconcile().status == PpsStatus::Ownership && s.vddOn().status == PpsStatus::Ownership);
    CHECK(f.reads == reads && f.writesCount == writes);
    f.clean();
    {
        Fake g; g.bios(); GlkPanelPowerSequencer q;
        PanelPowerAccess a = g.access(); a.ownerCurrent = nullptr;
        CHECK(q.initialize(config(), a).status == PpsStatus::Unavailable && g.reads == 0 && g.coreAcq == 0);
    }
    {
        Fake g; g.bios(); GlkPanelPowerSequencer q;
        CHECK(q.initialize(config(), g.access()).status == PpsStatus::Ok && q.vddOn().status == PpsStatus::Ok);
        g.loseOwnerAtWrite = int(g.writesCount) + 1;
        const PpsResult r = q.panelOn();
        CHECK(r.status == PpsStatus::Ownership && r.writes == 1);
        CHECK((r.obligations & (ObligationAuxPower | ObligationIndeterminate | ObligationPanelPower)) ==
              (ObligationAuxPower | ObligationIndeterminate | ObligationPanelPower));
        CHECK(g.auxRel == 0);
        const unsigned oldReads = g.reads, oldWrites = g.writesCount;
        g.ownerValid = true;
        CHECK(q.reconcile().status == PpsStatus::Ownership && q.vddOff().status == PpsStatus::Ownership);
        CHECK(g.reads == oldReads && g.writesCount == oldWrites && g.auxRel == 0);
        g.clean();
    }
    {
        Fake g; g.bios(); PpsObservation observation {};
        g.loseOwnerAtRead = 1;
        CHECK(observePanelPower(config().device, g.access(), observation) == PpsStatus::Ownership);
        const PpsObservation zero {};
        CHECK(g.reads == 1 && g.writesCount == 0 && !memcmp(&observation, &zero, sizeof zero));
        g.clean();
    }
    {
        Fake g; g.bios(); GlkPanelPowerSequencer q;
        CHECK(q.initialize(config(), g.access()).status == PpsStatus::Ok);
        g.reenter = &q; g.reenterAtCore = true;
        CHECK(q.vddOn().status == PpsStatus::Ok && g.reenterStatus == PpsStatus::Busy);
        CHECK(q.vddOff().status == PpsStatus::Ok && g.auxAcq == g.auxRel);
        g.clean();
    }
}

int main() {
    lostOwnerTest();
    pureTests();
    initializeTests();
    fullSequenceTest();
    controllerOneTest();
    timeoutAndClockTests();
    cancellationTests();
    faultTests();
    handoverAndResetTests();
    observeTests();
    printf("native_glk_panel_power_tests: %u checks passed (hosted injected MMIO; not hardware evidence)\n", checks);
    return 0;
}

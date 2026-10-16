// SPDX-License-Identifier: MIT
// Copyright © 2020, 2023 Intel Corporation
// Copyright (c) 2026 Mellow contributors
// GLK panel power sequencing. Behavior follows i915 display/intel_pps.c at
// torvalds/linux@0d9ff90a5422cc7509258aaaba1e7481df4d332a; deliberate
// fail-closed differences are listed in PanelPower.NOTES.md.
#include "PanelPower.hpp"

namespace MellowNativeGlk {
namespace {
constexpr uint32_t AllOnes = 0xffffffffU;
uint32_t unitsToMs(uint32_t units) { return (units + 9) / 10; } // pps_units_to_msecs
bool accessComplete(const PanelPowerAccess &a) {
    return a.context && a.read32 && a.write32 && a.nowUs && a.sleepUs && a.cancelled &&
        a.acquireDisplayCorePower && a.releaseDisplayCorePower && a.acquireAuxPower && a.releaseAuxPower && a.ownerCurrent;
}
bool identityBound(const DeviceIdentity &d) { return d.registryId && d.epoch; }
PpsStatus readReg(const PanelPowerAccess &a, const DeviceIdentity &device, unsigned controller, uint32_t legacyReg, uint32_t &value) {
    uint32_t offset = 0;
    value = 0;
    if (!ppsRegisterOffset(controller, legacyReg, offset)) return PpsStatus::Invalid;
    if (!a.ownerCurrent(a.context, device)) return PpsStatus::Ownership;
    if (!a.read32(a.context, offset, value)) return PpsStatus::IoFailure;
    if (!a.ownerCurrent(a.context, device)) { value = 0; return PpsStatus::Ownership; }
    // No PPS register is all-ones; the function or display power is gone.
    return value == AllOnes ? PpsStatus::Unavailable : PpsStatus::Ok;
}
bool sameDelays(const PpsDelays &a, const PpsDelays &b) {
    return a.powerUp == b.powerUp && a.backlightOn == b.backlightOn && a.backlightOff == b.backlightOff &&
        a.powerDown == b.powerDown && a.powerCycle == b.powerCycle;
}
bool lit(uint32_t status, uint32_t control) {
    return (status & PpOn) || (control & (PanelPowerOn | EdpForceVdd));
}
}

bool glkDevice(uint16_t vendor, uint16_t device) {
    return vendor == 0x8086 && (device == 0x3184 || device == 0x3185);
}

bool ppsRegisterOffset(unsigned controller, uint32_t legacyReg, uint32_t &offset) {
    offset = 0;
    if (controller >= GlkPpsCount) return false;
    if (legacyReg != PpStatusReg && legacyReg != PpControlReg &&
        legacyReg != PpOnDelaysReg && legacyReg != PpOffDelaysReg) return false;
    offset = PchPpsBase - LegacyPpsBase + legacyReg + controller * PpsInstanceStride;
    return true;
}

PpsDelays decodeHardwareDelays(uint32_t ppOn, uint32_t ppOff, uint32_t ppControl) {
    PpsDelays d {};
    d.powerUp = uint16_t((ppOn & PanelPowerUpDelayMask) >> PanelDelayShift);
    d.backlightOn = uint16_t(ppOn & PanelLightOnDelayMask);
    d.backlightOff = uint16_t(ppOff & PanelLightOffDelayMask);
    d.powerDown = uint16_t((ppOff & PanelPowerDownDelayMask) >> PanelDelayShift);
    // "hardware wants <delay>+1 in 100ms units"
    const uint32_t field = (ppControl & BxtPowerCycleDelayMask) >> BxtPowerCycleDelayShift;
    d.powerCycle = uint16_t(field ? (field - 1) * 1000 : 0);
    return d;
}

bool delaysPresent(const PpsDelays &d) {
    return d.powerUp || d.backlightOn || d.backlightOff || d.powerDown || d.powerCycle;
}

PpsDelays specificationDelays() {
    // T1+T3, T8 (T7 limit), T9 (symmetric with T8), T10, T11+T12 in 100 us units.
    return {(10 + 200) * 10, 50 * 10, 50 * 10, 500 * 10, (10 + 500) * 10};
}

bool encodePowerCycleField(uint32_t powerCycleUnits, uint32_t &field) {
    if (powerCycleUnits > (BxtPowerCycleFieldMax - 1) * 1000) return false;
    const uint32_t value = (powerCycleUnits + 999) / 1000 + 1;
    if (value > BxtPowerCycleFieldMax) return false;
    field = value;
    return true;
}

PpsStatus computeTiming(const PpsDelays &bios, const PpsDelays &vbt, PpsTiming &out) {
    const PpsDelays spec = specificationDelays();
    const uint16_t cur[DelayFieldCount] = {bios.powerUp, bios.backlightOn, bios.backlightOff,
        bios.powerDown, bios.powerCycle};
    const uint16_t fromVbt[DelayFieldCount] = {vbt.powerUp, vbt.backlightOn, vbt.backlightOff,
        vbt.powerDown, vbt.powerCycle};
    const uint16_t limit[DelayFieldCount] = {spec.powerUp, spec.backlightOn, spec.backlightOff,
        spec.powerDown, spec.powerCycle};
    PpsTiming t {};
    uint16_t chosen[DelayFieldCount] {};
    for (unsigned i = 0; i < DelayFieldCount; ++i) {
        const uint16_t larger = cur[i] > fromVbt[i] ? cur[i] : fromVbt[i];
        chosen[i] = larger ? larger : limit[i];
        t.source[i] = !larger ? DelaySource::Specification :
            cur[i] >= fromVbt[i] ? DelaySource::Hardware : DelaySource::Vbt;
    }
    t.waits = {unitsToMs(chosen[PowerUp]), unitsToMs(chosen[BacklightOn]), unitsToMs(chosen[BacklightOff]),
        unitsToMs(chosen[PowerDown]), unitsToMs(chosen[PowerCycle])};
    t.registers.powerUp = chosen[PowerUp];
    t.registers.powerDown = chosen[PowerDown];
    // T8/T9 are waited in software, so the hardware backlight delays are 1.
    t.registers.backlightOn = 1; t.registers.backlightOff = 1;
    // The hardware has 100 ms power-cycle granularity: round up.
    const uint32_t cycle = (uint32_t(chosen[PowerCycle]) + 999) / 1000 * 1000;
    if (t.registers.powerUp > PanelDelayFieldMax || t.registers.powerDown > PanelDelayFieldMax ||
        !encodePowerCycleField(cycle, t.cycleField)) return PpsStatus::DelayOutOfRange;
    t.registers.powerCycle = uint16_t(cycle);
    t.ppOn = (uint32_t(t.registers.powerUp) << PanelDelayShift) | t.registers.backlightOn;
    t.ppOff = (uint32_t(t.registers.powerDown) << PanelDelayShift) | t.registers.backlightOff;
    out = t;
    return PpsStatus::Ok;
}

PpsStatus resolveController(const VbtPanelPowerFacts &facts, unsigned &controller, ControllerSource &source) {
    controller = 0;
    source = ControllerSource::Unknown;
    // i915 falls back to PPS 0 or a PP_ON/VDD scan here; without an observed
    // PWM backlight entry there is no VBT choice to follow.
    if (facts.state != FactState::Observed || !facts.backlightEntryValid || !facts.pwmBacklight)
        return PpsStatus::PrerequisiteUnknown;
    if (facts.bdbVersion < 191) { source = ControllerSource::VbtBeforeVersion191; return PpsStatus::Ok; }
    if (facts.backlightController >= GlkPpsCount) return PpsStatus::Invalid;
    controller = facts.backlightController;
    source = ControllerSource::Vbt;
    return PpsStatus::Ok;
}

PpsStatus observePanelPower(const DeviceIdentity &device, PanelPowerAccess a, PpsObservation &out) {
    out = {};
    if (!a.context || !a.read32 || !a.acquireDisplayCorePower || !a.releaseDisplayCorePower || !a.ownerCurrent)
        return PpsStatus::Unavailable;
    if (!glkDevice(device.vendor, device.device)) return PpsStatus::Unsupported;
    if (!identityBound(device)) return PpsStatus::Invalid;
    if (!a.ownerCurrent(a.context, device)) return PpsStatus::Ownership;
    if (!a.acquireDisplayCorePower(a.context)) return PpsStatus::Unavailable;
    PpsObservation o {};
    PpsStatus status = PpsStatus::Ok;
    for (unsigned c = 0; c < GlkPpsCount && status == PpsStatus::Ok; ++c) {
        PpsInstanceSnapshot &i = o.instance[c];
        if ((status = readReg(a, device, c, PpStatusReg, i.status)) != PpsStatus::Ok ||
            (status = readReg(a, device, c, PpControlReg, i.control)) != PpsStatus::Ok ||
            (status = readReg(a, device, c, PpOnDelaysReg, i.onDelays)) != PpsStatus::Ok)
            break;
        status = readReg(a, device, c, PpOffDelaysReg, i.offDelays);
    }
    a.releaseDisplayCorePower(a.context);
    if (status == PpsStatus::Ok) out = o;
    return status;
}

PpsResult GlkPanelPowerSequencer::initialize(const PanelPowerConfig &config, PanelPowerAccess access) {
    PpsResult r {};
    if (busy_) { r.status = PpsStatus::Busy; r.obligations = state_.obligations; return r; }
    if (state_.initialized) { r.status = PpsStatus::Invalid; r.obligations = state_.obligations; return r; }
    if (!accessComplete(access)) { r.status = PpsStatus::Unavailable; return r; }
    if (!glkDevice(config.device.vendor, config.device.device)) { r.status = PpsStatus::Unsupported; return r; }
    if (!identityBound(config.device)) { r.status = PpsStatus::Invalid; return r; }
    if (!config.displayOwnershipGranted) { r.status = PpsStatus::Ownership; return r; }
    if (!access.ownerCurrent(access.context, config.device)) { r.status = PpsStatus::Ownership; return r; }
    unsigned controller = 0;
    ControllerSource source = ControllerSource::Unknown;
    if ((r.status = resolveController(config.vbt, controller, source)) != PpsStatus::Ok) return r;
    busy_ = true;
    if (!access.acquireDisplayCorePower(access.context)) { busy_ = false; r.status = PpsStatus::Unavailable; return r; }
    access_ = access;
    device_ = config.device;
    authorityLost_ = false;
    state_ = {};
    state_.controller = controller;
    state_.controllerSource = source;
    lastNowUs_ = 0;
    r.status = adopt(config, r);
    if (ownership() != PpsStatus::Ok) r.status = PpsStatus::Ownership;
    access_.releaseDisplayCorePower(access_.context);
    busy_ = false;
    if (r.status != PpsStatus::Ok && !(authorityLost_ && state_.initialized)) {
        if (state_.obligations & ObligationAuxPower) access_.releaseAuxPower(access_.context);
        state_ = {};
        access_ = {};
    }
    r.obligations = state_.obligations;
    return r;
}

PpsStatus GlkPanelPowerSequencer::adopt(const PanelPowerConfig &config, PpsResult &r) {
    uint64_t t = 0;
    PpsStatus s = now(t);
    if (s != PpsStatus::Ok) return s;
    uint32_t status[GlkPpsCount] {}, control[GlkPpsCount] {}, on = 0, off = 0;
    for (unsigned c = 0; c < GlkPpsCount; ++c)
        if ((s = readReg(access_, device_, c, PpStatusReg, status[c])) != PpsStatus::Ok ||
            (s = readReg(access_, device_, c, PpControlReg, control[c])) != PpsStatus::Ok) return s;
    const unsigned sel = state_.controller, other = sel ^ 1U;
    if ((s = read(PpOnDelaysReg, on)) != PpsStatus::Ok || (s = read(PpOffDelaysReg, off)) != PpsStatus::Ok)
        return s;
    r.ppStatus = status[sel]; r.ppControl = control[sel];
    for (unsigned c = 0; c < GlkPpsCount; ++c)
        if ((status[c] & PpSequenceMask) != PpSequenceNone) return PpsStatus::Busy;
    // pps_init_late() keeps the VBT choice over the initial scan. A lit
    // instance that is not the VBT's means the facts disagree with hardware.
    const bool selectedLit = lit(status[sel], control[sel]), otherLit = lit(status[other], control[other]);
    if (otherLit && !selectedLit) return PpsStatus::ControllerConflict;
    state_.bios = decodeHardwareDelays(on, off, control[sel]);
    PpsTiming timing {};
    const PpsDelays vbt = config.vbt.edpBlock ? config.vbt.edpPowerSequence : PpsDelays {};
    if ((s = computeTiming(state_.bios, vbt, timing)) != PpsStatus::Ok) return s;
    state_.timing = timing;
    // pps_vdd_init(): VDD left on by firmware needs the AUX reference.
    if (control[sel] & EdpForceVdd) {
        if (!access_.acquireAuxPower(access_.context)) return PpsStatus::Unavailable;
        state_.obligations |= ObligationAuxPower | ObligationVddForced;
        state_.vddOnUs = t;
        state_.vddSettled = (status[sel] & PpOn) != 0;
        if (!state_.vddSettled) state_.pending = PendingWait::VddSettle;
    }
    if ((status[sel] & PpOn) || (control[sel] & PanelPowerOn)) state_.obligations |= ObligationPanelPower;
    if (control[sel] & EdpBlcEnable) state_.obligations |= ObligationBacklight;
    state_.initialized = true;
    return PpsStatus::Ok;
}

PpsResult GlkPanelPowerSequencer::vddOn() { return run(Op::VddOn); }
PpsResult GlkPanelPowerSequencer::vddOff() { return run(Op::VddOff); }
PpsResult GlkPanelPowerSequencer::panelOn() { return run(Op::PanelOn); }
PpsResult GlkPanelPowerSequencer::panelOff() { return run(Op::PanelOff); }
PpsResult GlkPanelPowerSequencer::backlightOn() { return run(Op::BacklightOn); }
PpsResult GlkPanelPowerSequencer::backlightOff() { return run(Op::BacklightOff); }
PpsResult GlkPanelPowerSequencer::reconcile() { return run(Op::Reconcile); }

PpsResult GlkPanelPowerSequencer::run(Op op) {
    PpsResult r {};
    r.obligations = state_.obligations;
    if (busy_) { r.status = PpsStatus::Busy; return r; }
    if (!state_.initialized) { r.status = PpsStatus::Invalid; return r; }
    if (ownership() != PpsStatus::Ok) { r.status = PpsStatus::Ownership; r.obligations = state_.obligations; return r; }
    // Only reconcile resolves an unobserved outcome; only reconcile or vddOff
    // may proceed while VDD is still settling.
    const bool blocked = (state_.obligations & ObligationIndeterminate) ||
        (state_.pending == PendingWait::VddSettle && op != Op::VddOff);
    if (blocked && op != Op::Reconcile) { r.status = PpsStatus::ObligationPending; return r; }
    // intel_pps_lock(): display-core power around every access.
    busy_ = true;
    if (!access_.acquireDisplayCorePower(access_.context)) { busy_ = false; r.status = PpsStatus::Unavailable; return r; }
    switch (op) {
        case Op::VddOn: r.status = doVddOn(r); break;
        case Op::VddOff: r.status = doVddOff(r); break;
        case Op::PanelOn: r.status = doPanelOn(r); break;
        case Op::PanelOff: r.status = doPanelOff(r); break;
        case Op::BacklightOn: r.status = doBacklightOn(r); break;
        case Op::BacklightOff: r.status = doBacklightOff(r); break;
        case Op::Reconcile: r.status = doReconcile(r); break;
    }
    if (ownership() != PpsStatus::Ok) r.status = PpsStatus::Ownership;
    access_.releaseDisplayCorePower(access_.context);
    busy_ = false;
    r.obligations = state_.obligations;
    return r;
}

PpsStatus GlkPanelPowerSequencer::read(uint32_t legacyReg, uint32_t &value) {
    if (ownership() != PpsStatus::Ok) { value = 0; return PpsStatus::Ownership; }
    const PpsStatus status = readReg(access_, device_, state_.controller, legacyReg, value);
    if (status == PpsStatus::Ownership) authorityLost_ = true;
    return status;
}

PpsStatus GlkPanelPowerSequencer::ownership() {
    if (!authorityLost_ && access_.ownerCurrent(access_.context, device_)) return PpsStatus::Ok;
    authorityLost_ = true;
    if (state_.initialized) unverified(0, PendingWait::PowerLost);
    return PpsStatus::Ownership;
}

PpsStatus GlkPanelPowerSequencer::write(uint32_t legacyReg, uint32_t value, PpsResult &r) {
    uint32_t offset = 0;
    if (!ppsRegisterOffset(state_.controller, legacyReg, offset)) return PpsStatus::Invalid;
    if (ownership() != PpsStatus::Ok) return PpsStatus::Ownership;
    ++r.writes; // issued, whether or not the transport confirms it
    const bool written = access_.write32(access_.context, offset, value);
    if (ownership() != PpsStatus::Ok) return PpsStatus::Ownership;
    return written ? PpsStatus::Ok : PpsStatus::IoFailure;
}

PpsStatus GlkPanelPowerSequencer::now(uint64_t &value) {
    value = 0;
    if (!access_.nowUs(access_.context, value) || value < lastNowUs_) return PpsStatus::ClockFault;
    lastNowUs_ = value;
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::sleepFor(uint64_t us) {
    if (access_.cancelled(access_.context)) return PpsStatus::Cancelled;
    if (!us) return PpsStatus::Ok;
    return access_.sleepUs(access_.context, us) ? PpsStatus::Ok : PpsStatus::Cancelled;
}

PpsStatus GlkPanelPowerSequencer::waitRemaining(bool known, uint64_t sinceUs, uint32_t delayMs) {
    // wait_remaining_ms_from_jiffies(); an unknown start waits the full delay.
    const uint64_t delayUs = uint64_t(delayMs) * 1000;
    uint64_t t = 0;
    const PpsStatus s = now(t);
    if (s != PpsStatus::Ok) return s;
    uint64_t remaining = delayUs;
    if (known) remaining = t >= sinceUs + delayUs ? 0 : sinceUs + delayUs - t;
    return sleepFor(remaining);
}

PpsStatus GlkPanelPowerSequencer::pollStatus(uint32_t mask, uint32_t value, PpsResult &r) {
    // poll_timeout_us(read, cond, 10 ms, 5 s, sleep_before_read): the expiry
    // is sampled before each read, so one read always follows the deadline.
    uint64_t start = 0, t = 0;
    PpsStatus s = now(start);
    if (s != PpsStatus::Ok) return s;
    const uint64_t deadline = start + StatusPollTimeoutUs;
    if ((s = sleepFor(StatusPollIntervalUs)) != PpsStatus::Ok) return s;
    for (uint32_t reads = 0; reads < StatusPollMaxReads; ++reads) {
        if ((s = now(t)) != PpsStatus::Ok) return s;
        const bool expired = t > deadline;
        uint32_t status = 0;
        if ((s = read(PpStatusReg, status)) != PpsStatus::Ok) return s;
        r.ppStatus = status;
        if ((status & mask) == value) return PpsStatus::Ok;
        if (expired) return PpsStatus::Timeout;
        if ((s = sleepFor(StatusPollIntervalUs)) != PpsStatus::Ok) return s;
    }
    return PpsStatus::Timeout;
}

PpsStatus GlkPanelPowerSequencer::ensureProgrammed(PpsResult &r) {
    PpsStatus s = PpsStatus::Ok;
    uint32_t on = 0, off = 0, control = 0;
    if (!state_.registersProgrammed) {
        // pps_init_registers(intel_dp, false) for GLK: no port select, the
        // cycle delay is a PP_CONTROL read-modify-write. Also the DC9 path of
        // bxt_power_sequencer_idx().
        if ((s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
        state_.obligations |= ObligationRegistersUnverified;
        if ((s = write(PpOnDelaysReg, state_.timing.ppOn, r)) != PpsStatus::Ok ||
            (s = write(PpOffDelaysReg, state_.timing.ppOff, r)) != PpsStatus::Ok ||
            (s = write(PpControlReg, (control & ~BxtPowerCycleDelayMask) |
                (state_.timing.cycleField << BxtPowerCycleDelayShift), r)) != PpsStatus::Ok)
            return s;
    }
    // intel_pps_verify_state(), done before any sequence write rather than
    // logged during the wait.
    if ((s = read(PpOnDelaysReg, on)) != PpsStatus::Ok || (s = read(PpOffDelaysReg, off)) != PpsStatus::Ok ||
        (s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    r.ppControl = control;
    const PpsDelays hw = decodeHardwareDelays(on, off, control);
    if (!sameDelays(hw, state_.timing.registers)) {
        state_.registersProgrammed = false;
        state_.obligations |= ObligationRegistersUnverified;
        return PpsStatus::StateMismatch;
    }
    state_.registersProgrammed = true;
    state_.obligations &= ~uint32_t(ObligationRegistersUnverified);
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::waitPowerCycle(PpsResult &r) {
    // wait_panel_power_cycle(): software T11+T12 remainder, then IDLE_CYCLE.
    PpsStatus s = waitRemaining(state_.panelOffKnown, state_.panelOffUs, state_.timing.waits.powerCycle);
    if (s != PpsStatus::Ok || (s = pollStatus(IdleCycleMask, IdleCycleValue, r)) != PpsStatus::Ok) return s;
    if (!state_.panelOffKnown) {
        // A full cycle delay has now elapsed since the unknown off instant.
        uint64_t t = 0;
        if ((s = now(t)) != PpsStatus::Ok) return s;
        const uint64_t cycleUs = uint64_t(state_.timing.waits.powerCycle) * 1000;
        state_.panelOffUs = t > cycleUs ? t - cycleUs : 0;
        state_.panelOffKnown = true;
    }
    return PpsStatus::Ok;
}

void GlkPanelPowerSequencer::unverified(uint32_t obligation, PendingWait pending) {
    state_.obligations |= obligation;
    state_.obligations |= ObligationIndeterminate;
    state_.pending = pending;
}

void GlkPanelPowerSequencer::dropAux() {
    if (!(state_.obligations & ObligationAuxPower) || authorityLost_) return;
    access_.releaseAuxPower(access_.context);
    state_.obligations &= ~uint32_t(ObligationAuxPower);
}

PpsStatus GlkPanelPowerSequencer::doVddOn(PpsResult &r) {
    uint32_t control = 0, status = 0;
    PpsStatus s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) return s;
    r.ppControl = control;
    const bool owed = state_.obligations & ObligationVddForced;
    if (control & EdpForceVdd) {
        if (!owed) return PpsStatus::StateMismatch;
        state_.wantVdd = true;
        return PpsStatus::Ok;
    }
    if (owed) return PpsStatus::StateMismatch; // cleared outside this owner
    if (!access_.acquireAuxPower(access_.context)) return PpsStatus::Unavailable;
    state_.obligations |= ObligationAuxPower;
    bool panelPowered = false;
    if ((s = ensureProgrammed(r)) == PpsStatus::Ok && (s = read(PpStatusReg, status)) == PpsStatus::Ok) {
        r.ppStatus = status;
        panelPowered = status & PpOn;
        if (!panelPowered) s = waitPowerCycle(r);
    }
    if (s == PpsStatus::Ok && access_.cancelled(access_.context)) s = PpsStatus::Cancelled;
    if (s == PpsStatus::Ok) s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) { dropAux(); return s; } // nothing written for VDD
    if ((s = write(PpControlReg, control | EdpForceVdd, r)) != PpsStatus::Ok) {
        unverified(ObligationVddForced, PendingWait::Unverified);
        return s;
    }
    state_.obligations |= ObligationVddForced;
    state_.wantVdd = true;
    state_.panelOffKnown = false;
    uint64_t t = 0;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok || (s = now(t)) != PpsStatus::Ok) {
        unverified(0, PendingWait::Unverified);
        return s;
    }
    r.ppControl = control;
    if (!(control & EdpForceVdd)) { unverified(0, PendingWait::Unverified); return PpsStatus::StateMismatch; }
    state_.vddOnUs = t;
    if (panelPowered) { state_.vddSettled = true; return PpsStatus::Ok; }
    // "If the panel wasn't on, delay before accessing aux channel"
    state_.vddSettled = false;
    state_.pending = PendingWait::VddSettle;
    if ((s = sleepFor(uint64_t(state_.timing.waits.powerUp) * 1000)) != PpsStatus::Ok) return s;
    state_.vddSettled = true;
    state_.pending = PendingWait::None;
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::doVddOff(PpsResult &r) {
    uint32_t control = 0;
    PpsStatus s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) return s;
    r.ppControl = control;
    const bool owed = state_.obligations & ObligationVddForced;
    if (!(control & EdpForceVdd)) return owed ? PpsStatus::StateMismatch : PpsStatus::Ok;
    if (!owed) return PpsStatus::StateMismatch;
    if ((s = ensureProgrammed(r)) != PpsStatus::Ok || (s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    const uint32_t value = control & ~EdpForceVdd;
    if ((s = write(PpControlReg, value, r)) != PpsStatus::Ok) { unverified(0, PendingWait::Unverified); return s; }
    uint64_t t = 0;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok || (s = now(t)) != PpsStatus::Ok) {
        unverified(0, PendingWait::Unverified);
        return s;
    }
    r.ppControl = control;
    if (control & EdpForceVdd) { unverified(0, PendingWait::Unverified); return PpsStatus::StateMismatch; }
    state_.obligations &= ~uint32_t(ObligationVddForced);
    state_.wantVdd = false;
    state_.vddSettled = false;
    if (state_.pending == PendingWait::VddSettle) state_.pending = PendingWait::None;
    if (!(value & PanelPowerOn)) { state_.panelOffUs = t; state_.panelOffKnown = true; }
    dropAux();
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::doPanelOn(PpsResult &r) {
    uint32_t status = 0, control = 0;
    PpsStatus s = read(PpStatusReg, status);
    if (s != PpsStatus::Ok) return s;
    r.ppStatus = status;
    if ((status & PpOn) || (state_.obligations & ObligationPanelPower)) return PpsStatus::StateMismatch;
    if ((s = ensureProgrammed(r)) != PpsStatus::Ok || (s = waitPowerCycle(r)) != PpsStatus::Ok) return s;
    if (access_.cancelled(access_.context)) return PpsStatus::Cancelled;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    const uint32_t request = control | PanelPowerOn | PanelPowerReset;
    if ((s = write(PpControlReg, request, r)) != PpsStatus::Ok) {
        unverified(ObligationPanelPower, PendingWait::PanelOn);
        return s;
    }
    state_.obligations |= ObligationPanelPower;
    state_.pending = PendingWait::PanelOn;
    state_.panelOffKnown = false;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok ||
        (r.ppControl = control, s = pollStatus(IdleOnMask, IdleOnValue, r)) != PpsStatus::Ok) {
        state_.obligations |= ObligationIndeterminate;
        return s;
    }
    uint64_t t = 0;
    if ((s = now(t)) != PpsStatus::Ok) { state_.obligations |= ObligationIndeterminate; return s; }
    state_.powerOnUs = t;
    state_.powerOnKnown = true;
    state_.pending = PendingWait::None;
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::doPanelOff(PpsResult &r) {
    uint32_t control = 0;
    PpsStatus s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) return s;
    r.ppControl = control;
    // "need VDD to turn off panel"; the backlight goes first so T9 is explicit.
    if (!(control & EdpForceVdd) || !(state_.obligations & ObligationVddForced) ||
        (control & EdpBlcEnable) || (state_.obligations & ObligationBacklight)) return PpsStatus::StateMismatch;
    if ((s = waitRemaining(state_.backlightOffKnown, state_.backlightOffUs, state_.timing.waits.backlightOff)) != PpsStatus::Ok ||
        (s = ensureProgrammed(r)) != PpsStatus::Ok) return s;
    if (access_.cancelled(access_.context)) return PpsStatus::Cancelled;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    // Switch off panel power and forced VDD together.
    const uint32_t value = control & ~(PanelPowerOn | PanelPowerReset | EdpForceVdd | EdpBlcEnable);
    if ((s = write(PpControlReg, value, r)) != PpsStatus::Ok) {
        unverified(0, PendingWait::PanelOff);
        return s;
    }
    state_.wantVdd = false;
    state_.pending = PendingWait::PanelOff;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) { state_.obligations |= ObligationIndeterminate; return s; }
    r.ppControl = control;
    if (!(control & EdpForceVdd)) state_.obligations &= ~uint32_t(ObligationVddForced);
    // The AUX reference stays held until IDLE_OFF is observed.
    if ((s = pollStatus(IdleOffMask, IdleOffValue, r)) != PpsStatus::Ok) {
        state_.obligations |= ObligationIndeterminate;
        return s;
    }
    uint64_t t = 0;
    if ((s = now(t)) != PpsStatus::Ok) { state_.obligations |= ObligationIndeterminate; return s; }
    state_.panelOffUs = t;
    state_.panelOffKnown = true;
    state_.powerOnKnown = false;
    state_.vddSettled = false;
    state_.obligations &= ~uint32_t(ObligationPanelPower | ObligationVddForced);
    state_.pending = PendingWait::None;
    dropAux();
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::doBacklightOn(PpsResult &r) {
    uint32_t status = 0, control = 0;
    PpsStatus s = read(PpStatusReg, status);
    if (s != PpsStatus::Ok || (s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    r.ppStatus = status; r.ppControl = control;
    if ((status & IdleOnMask) != IdleOnValue || !(state_.obligations & ObligationPanelPower) ||
        (control & EdpBlcEnable) || (state_.obligations & ObligationBacklight)) return PpsStatus::StateMismatch;
    // wait_backlight_on(): T8 from the observed power-on, or in full.
    if ((s = waitRemaining(state_.powerOnKnown, state_.powerOnUs, state_.timing.waits.backlightOn)) != PpsStatus::Ok ||
        (s = ensureProgrammed(r)) != PpsStatus::Ok) return s;
    if (access_.cancelled(access_.context)) return PpsStatus::Cancelled;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    if ((s = write(PpControlReg, control | EdpBlcEnable, r)) != PpsStatus::Ok) {
        unverified(ObligationBacklight, PendingWait::Unverified);
        return s;
    }
    state_.obligations |= ObligationBacklight;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) { unverified(0, PendingWait::Unverified); return s; }
    r.ppControl = control;
    if (!(control & EdpBlcEnable)) { unverified(0, PendingWait::Unverified); return PpsStatus::StateMismatch; }
    return PpsStatus::Ok;
}

PpsStatus GlkPanelPowerSequencer::doBacklightOff(PpsResult &r) {
    uint32_t control = 0;
    PpsStatus s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) return s;
    r.ppControl = control;
    const bool owed = state_.obligations & ObligationBacklight;
    if (!(control & EdpBlcEnable)) return owed ? PpsStatus::StateMismatch : PpsStatus::Ok;
    if (!owed) return PpsStatus::StateMismatch;
    if ((s = ensureProgrammed(r)) != PpsStatus::Ok) return s;
    if (access_.cancelled(access_.context)) return PpsStatus::Cancelled;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok) return s;
    if ((s = write(PpControlReg, control & ~EdpBlcEnable, r)) != PpsStatus::Ok) {
        unverified(0, PendingWait::Unverified);
        return s;
    }
    uint64_t t = 0;
    if ((s = read(PpControlReg, control)) != PpsStatus::Ok || (s = now(t)) != PpsStatus::Ok) {
        unverified(0, PendingWait::Unverified);
        return s;
    }
    r.ppControl = control;
    if (control & EdpBlcEnable) { unverified(0, PendingWait::Unverified); return PpsStatus::StateMismatch; }
    state_.obligations &= ~uint32_t(ObligationBacklight);
    state_.backlightOffUs = t;
    state_.backlightOffKnown = true;
    // edp_wait_backlight_off(): an interrupted T9 is finished by panelOff().
    return sleepFor(uint64_t(state_.timing.waits.backlightOff) * 1000);
}

PpsStatus GlkPanelPowerSequencer::doReconcile(PpsResult &r) {
    if (!(state_.obligations & ObligationIndeterminate) && state_.pending == PendingWait::None)
        return PpsStatus::Ok;
    uint32_t control = 0;
    PpsStatus s = read(PpControlReg, control);
    if (s != PpsStatus::Ok) return s;
    r.ppControl = control;
    // Wait for the sequencer to settle in the state PP_CONTROL requests; no write.
    const bool requestOn = control & PanelPowerOn;
    if ((s = pollStatus(requestOn ? IdleOnMask : IdleOffMask, requestOn ? IdleOnValue : IdleOffValue, r)) !=
        PpsStatus::Ok) return s;
    uint64_t t = 0;
    if ((s = now(t)) != PpsStatus::Ok) return s;
    const uint32_t before = state_.obligations;
    const PendingWait was = state_.pending;
    const bool vdd = control & EdpForceVdd;
    uint32_t next = before & ~uint32_t(ObligationVddForced | ObligationPanelPower | ObligationBacklight |
        ObligationIndeterminate);
    if (vdd) next |= ObligationVddForced;
    if (requestOn) next |= ObligationPanelPower;
    if (control & EdpBlcEnable) next |= ObligationBacklight;
    state_.obligations = next;
    state_.wantVdd = vdd;
    state_.pending = PendingWait::None;
    if (requestOn) {
        if (was == PendingWait::PanelOn) { state_.powerOnUs = t; state_.powerOnKnown = true; }
        else if (!(before & ObligationPanelPower)) state_.powerOnKnown = false;
    } else if (!vdd) {
        if (was == PendingWait::PanelOff) { state_.panelOffUs = t; state_.panelOffKnown = true; }
        else if (before & (ObligationPanelPower | ObligationVddForced)) state_.panelOffKnown = false;
        state_.vddSettled = false;
    }
    if (!vdd) dropAux();
    if (vdd && !(state_.obligations & ObligationAuxPower)) {
        // Forced VDD without our AUX reference: take it as pps_vdd_init() does.
        if (!access_.acquireAuxPower(access_.context)) {
            unverified(0, PendingWait::Unverified);
            return PpsStatus::Unavailable;
        }
        state_.obligations |= ObligationAuxPower;
    }
    if (vdd && !requestOn && !state_.vddSettled) {
        state_.pending = PendingWait::VddSettle;
        if ((s = waitRemaining(true, state_.vddOnUs, state_.timing.waits.powerUp)) != PpsStatus::Ok) return s;
        state_.vddSettled = true;
        state_.pending = PendingWait::None;
    }
    if (vdd && requestOn) state_.vddSettled = true;
    return PpsStatus::Ok;
}

PpsResult GlkPanelPowerSequencer::noteDisplayPowerReset() {
    PpsResult r {};
    r.obligations = state_.obligations;
    if (busy_) { r.status = PpsStatus::Busy; return r; }
    if (!state_.initialized) { r.status = PpsStatus::Invalid; return r; }
    state_.registersProgrammed = false;
    if (state_.obligations & (ObligationVddForced | ObligationPanelPower | ObligationBacklight)) {
        unverified(0, PendingWait::PowerLost);
        r.status = PpsStatus::ObligationPending;
    } else {
        r.status = PpsStatus::Ok;
    }
    r.obligations = state_.obligations;
    return r;
}
}

// SPDX-License-Identifier: MIT
// Copyright © 2020, 2023 Intel Corporation
// Copyright (c) 2026 Mellow contributors
// Gemini Lake eDP panel power sequencer (PPS). Register layout and sequencing
// rules follow Linux i915 display/intel_pps.c and display/intel_pps_regs.h at
// torvalds/linux@0d9ff90a5422cc7509258aaaba1e7481df4d332a; permission notice in
// PanelPower.LICENSE.MIT, sources and hashes in PanelPower.provenance.json. Original C++: no upstream
// function body is copied. Nothing here starts automatically, and an injected
// transport passing these checks is not evidence of real panel control.
#pragma once
#include "../NativeGpu/MemoryOwner.hpp"

namespace MellowNativeGlk {
using MellowNative::DeviceIdentity;

// intel_pps_setup(): geminilake uses PCH_PPS_BASE. _MMIO_PPS() addresses
// mmio_base - PPS_BASE + reg + pps_idx * 0x100. 0x61200 is only the legacy
// register name base and is never accessed on GLK.
constexpr uint32_t LegacyPpsBase = 0x61200, PchPpsBase = 0xC7200, PpsInstanceStride = 0x100;
constexpr uint32_t PpStatusReg = 0x61200, PpControlReg = 0x61204,
    PpOnDelaysReg = 0x61208, PpOffDelaysReg = 0x6120C;
constexpr unsigned GlkPpsCount = 2; // intel_num_pps(): geminilake/broxton

// PP_STATUS
constexpr uint32_t PpOn = 1U << 31, PpReady = 1U << 30, PpSequenceMask = 3U << 28,
    PpSequenceNone = 0, PpSequencePowerUp = 1U << 28, PpSequencePowerDown = 2U << 28,
    PpCycleDelayActive = 1U << 27, PpSequenceStateMask = 0xFU,
    PpSequenceStateOffIdle = 0x0U, PpSequenceStateOnIdle = 0x8U, PpSequenceStateReset = 0xFU;
// PP_CONTROL. GLK has DDI, so there is no PANEL_UNLOCK_REGS key and no
// PP_DIVISOR; the T11+T12 field lives in BXT_POWER_CYCLE_DELAY_MASK (8:4).
constexpr uint32_t PanelUnlockMask = 0xFFFF0000U, BxtPowerCycleDelayMask = 0x1F0U,
    BxtPowerCycleDelayShift = 4, BxtPowerCycleFieldMax = 0x1FU,
    EdpForceVdd = 1U << 3, EdpBlcEnable = 1U << 2,
    PanelPowerReset = 1U << 1, PanelPowerOn = 1U << 0;
// PP_ON_DELAYS / PP_OFF_DELAYS, 13-bit fields in 100 us units. GLK programs
// no port select (pps_init_registers selects one only for VLV/CHV/IBX/CPT).
constexpr uint32_t PanelPortSelectMask = 3U << 30, PanelPowerUpDelayMask = 0x1FFF0000U,
    PanelLightOnDelayMask = 0x1FFFU, PanelPowerDownDelayMask = 0x1FFF0000U,
    PanelLightOffDelayMask = 0x1FFFU, PanelDelayShift = 16, PanelDelayFieldMax = 0x1FFFU;
// wait_panel_on / wait_panel_off / wait_panel_power_cycle conditions.
constexpr uint32_t IdleOnMask = PpOn | PpSequenceMask | PpSequenceStateMask,
    IdleOnValue = PpOn | PpSequenceNone | PpSequenceStateOnIdle,
    IdleOffMask = PpOn | PpSequenceMask, IdleOffValue = 0,
    IdleCycleMask = PpOn | PpSequenceMask | PpCycleDelayActive | PpSequenceStateMask,
    IdleCycleValue = PpSequenceStateOffIdle;
// wait_panel_status(): poll_timeout_us(..., 10 ms, 5000 ms, sleep_before_read).
constexpr uint64_t StatusPollIntervalUs = 10000, StatusPollTimeoutUs = 5000000;
// One read after expiry plus the sleep before the first read; this bound also
// ends the loop if an injected clock stops advancing.
constexpr uint32_t StatusPollMaxReads = uint32_t(StatusPollTimeoutUs / StatusPollIntervalUs) + 2;

enum class PpsStatus : uint8_t {
    Ok, Invalid, Unsupported, Unavailable, Ownership, PrerequisiteUnknown,
    ControllerConflict, DelayOutOfRange, IoFailure, ClockFault, Timeout, Cancelled,
    ObligationPending, StateMismatch, Busy
};
// Hardware or reference obligations this owner holds. They are only cleared
// after the corresponding effect is observed; cancellation never clears them.
enum PpsObligation : uint32_t {
    ObligationAuxPower = 1U << 0,          // AUX power-domain reference held for VDD
    ObligationVddForced = 1U << 1,         // EDP_FORCE_VDD set
    ObligationPanelPower = 1U << 2,        // PANEL_POWER_ON requested or found on
    ObligationBacklight = 1U << 3,         // EDP_BLC_ENABLE set
    ObligationIndeterminate = 1U << 4,     // a write or wait did not complete
    ObligationRegistersUnverified = 1U << 5 // delay registers may hold partial values
};
// VddSettle: VDD forced with the panel off, AUX not yet usable. The others
// accompany ObligationIndeterminate: an outcome that has not been observed.
enum class PendingWait : uint8_t { None, VddSettle, PanelOn, PanelOff, Unverified, PowerLost };

// struct intel_pps_delays: all fields in 100 us PPS units.
struct PpsDelays {
    uint16_t powerUp {}, backlightOn {}, backlightOff {}, powerDown {}, powerCycle {};
};
// Software waits in milliseconds: pps_units_to_msecs() = DIV_ROUND_UP(units, 10).
struct PpsWaitsMs {
    uint32_t powerUp {}, backlightOn {}, backlightOff {}, powerDown {}, powerCycle {};
};
enum class DelaySource : uint8_t { Hardware, Vbt, Specification };
enum DelayField : uint8_t { PowerUp, BacklightOn, BacklightOff, PowerDown, PowerCycle, DelayFieldCount };
struct PpsTiming {
    PpsDelays registers {};   // pps_delays: what PP_ON/PP_OFF/PP_CONTROL must hold
    PpsWaitsMs waits {};
    DelaySource source[DelayFieldCount] {};
    uint32_t ppOn {}, ppOff {}, cycleField {};
};

// Facts a VBT parser extracted (intel_bios.c parse_lfp_backlight/parse_edp)
// for the already-chosen panel_type. This module never parses VBT bytes and
// never substitutes a default controller for a missing fact.
enum class FactState : uint8_t { Unknown, Observed };
struct VbtPanelPowerFacts {
    FactState state {FactState::Unknown};
    uint16_t bdbVersion {};
    bool backlightEntryValid {};   // BDB_LFP_BACKLIGHT present, entry_size matched
    bool pwmBacklight {};          // data[panel_type].type == BDB_BACKLIGHT_TYPE_PWM
    uint8_t backlightController {}; // backlight_control[panel_type].controller (191+)
    bool edpBlock {};              // BDB_EDP present
    PpsDelays edpPowerSequence {}; // power_seqs[panel_type] t1_t3,t8,t9,t10,t11_t12
};
enum class ControllerSource : uint8_t { Unknown, Vbt, VbtBeforeVersion191 };

struct PanelPowerConfig {
    DeviceIdentity device {};
    // The firmware console/framebuffer released the eDP panel to this owner.
    bool displayOwnershipGranted {};
    VbtPanelPowerFacts vbt {};
};

// Injected transport. Every callback is required; a missing one is reported as
// Unavailable before any access. Offsets are GTTMMADR byte offsets. sleepUs
// sleeps at least the requested time and returns false when interrupted.
// Display-core power is held around every access sequence (intel_pps_lock);
// AUX power is held while VDD is forced (vdd_wakeref).
struct PanelPowerAccess {
    void *context {};
    bool (*read32)(void *, uint32_t offset, uint32_t &value) {};
    bool (*write32)(void *, uint32_t offset, uint32_t value) {};
    bool (*nowUs)(void *, uint64_t &now) {};
    bool (*sleepUs)(void *, uint64_t us) {};
    bool (*cancelled)(void *) {};
    bool (*acquireDisplayCorePower)(void *) {};
    void (*releaseDisplayCorePower)(void *) {};
    bool (*acquireAuxPower)(void *) {};
    void (*releaseAuxPower)(void *) {};
    // Native owner checks physical registry identity and reset/PM epoch under
    // its display lock. False revokes this sequencer permanently; power/MMIO
    // must not be accessed through a replacement device or epoch.
    bool (*ownerCurrent)(void *, const DeviceIdentity &) {};
};

struct PpsInstanceSnapshot { uint32_t status {}, control {}, onDelays {}, offDelays {}; };
struct PpsObservation { PpsInstanceSnapshot instance[GlkPpsCount] {}; };

struct PanelPowerState {
    bool initialized {}, registersProgrammed {}, wantVdd {}, vddSettled {};
    unsigned controller {};
    ControllerSource controllerSource {ControllerSource::Unknown};
    PpsDelays bios {};
    PpsTiming timing {};
    uint32_t obligations {};
    PendingWait pending {PendingWait::None};
    // Monotonic microseconds. A false *Known flag means the instant is unknown
    // and the full delay is waited from now.
    bool panelOffKnown {}, powerOnKnown {}, backlightOffKnown {};
    uint64_t panelOffUs {}, powerOnUs {}, backlightOffUs {}, vddOnUs {};
};
struct PpsResult {
    PpsStatus status {PpsStatus::Invalid};
    uint32_t obligations {}, writes {};
    uint32_t ppStatus {}, ppControl {};
};

bool glkDevice(uint16_t vendor, uint16_t device); // INTEL_GLK_IDS
bool ppsRegisterOffset(unsigned controller, uint32_t legacyReg, uint32_t &offset);
// intel_pps_readout_hw_state() for GLK: cycle = field ? (field - 1) * 1000 : 0.
PpsDelays decodeHardwareDelays(uint32_t ppOn, uint32_t ppOff, uint32_t ppControl);
bool delaysPresent(const PpsDelays &); // pps_delays_valid()
PpsDelays specificationDelays();       // pps_init_delays_spec(), eDP 1.3 limits
// DIV_ROUND_UP(power_cycle, 1000) + 1, rejecting values the 5-bit field
// cannot hold instead of masking them as REG_FIELD_PREP would.
bool encodePowerCycleField(uint32_t powerCycleUnits, uint32_t &field);
// pps_init_delays(): max(BIOS, VBT), else the spec limit; backlight registers
// forced to 1 for manual waits; power_cycle rounded up to 100 ms. Output is
// unchanged on error.
PpsStatus computeTiming(const PpsDelays &bios, const PpsDelays &vbt, PpsTiming &out);
// pps_initial_setup()/pps_init_late() VBT choice without the PP_ON/VDD scan.
PpsStatus resolveController(const VbtPanelPowerFacts &, unsigned &controller, ControllerSource &);
// Read-only capture of both instances under display-core power. Output is
// zero on every failure. Requires read32, nowUs is not used.
PpsStatus observePanelPower(const DeviceIdentity &, PanelPowerAccess, PpsObservation &);

// Caller serializes all calls (the display owner's sleepable lock); nested
// calls from transport callbacks return Busy. The access callbacks must stay
// valid until the sequencer is discarded with no obligations.
class GlkPanelPowerSequencer {
public:
    GlkPanelPowerSequencer() = default;
    GlkPanelPowerSequencer(const GlkPanelPowerSequencer &) = delete;
    GlkPanelPowerSequencer &operator=(const GlkPanelPowerSequencer &) = delete;
    // Read-only: validates prerequisites, reads both instances, computes the
    // timing and adopts any firmware-left VDD/panel/backlight as obligations.
    PpsResult initialize(const PanelPowerConfig &, PanelPowerAccess);
    PpsResult vddOn();        // intel_pps_vdd_on_unlocked
    PpsResult vddOff();       // intel_pps_vdd_off_sync_unlocked
    PpsResult panelOn();      // intel_pps_on_unlocked
    PpsResult panelOff();     // intel_pps_off_unlocked; requires VDD forced
    PpsResult backlightOn();  // intel_pps_backlight_on
    PpsResult backlightOff(); // intel_pps_backlight_off
    // Completes an interrupted VDD settle or status wait, or re-reads hardware
    // after a power loss. Never writes.
    PpsResult reconcile();
    // bxt_pps_reset_all(): DC9 may reset PPS registers. Registers are
    // reprogrammed before the next access; live power obligations at this
    // point mean power was lost without sequencing.
    PpsResult noteDisplayPowerReset();
    const PanelPowerState &state() const { return state_; }

private:
    enum class Op : uint8_t { VddOn, VddOff, PanelOn, PanelOff, BacklightOn, BacklightOff, Reconcile };
    PpsResult run(Op);
    PpsStatus adopt(const PanelPowerConfig &, PpsResult &);
    PpsStatus doVddOn(PpsResult &);
    PpsStatus doVddOff(PpsResult &);
    PpsStatus doPanelOn(PpsResult &);
    PpsStatus doPanelOff(PpsResult &);
    PpsStatus doBacklightOn(PpsResult &);
    PpsStatus doBacklightOff(PpsResult &);
    PpsStatus doReconcile(PpsResult &);
    PpsStatus read(uint32_t legacyReg, uint32_t &value);
    PpsStatus ownership();
    PpsStatus write(uint32_t legacyReg, uint32_t value, PpsResult &);
    PpsStatus now(uint64_t &);
    PpsStatus sleepFor(uint64_t us);
    PpsStatus waitRemaining(bool known, uint64_t sinceUs, uint32_t delayMs);
    PpsStatus pollStatus(uint32_t mask, uint32_t value, PpsResult &);
    PpsStatus ensureProgrammed(PpsResult &);
    PpsStatus waitPowerCycle(PpsResult &);
    void unverified(uint32_t obligation, PendingWait);
    void dropAux();

    PanelPowerAccess access_ {};
    PanelPowerState state_ {};
    uint64_t lastNowUs_ {};
    bool busy_ {};
    DeviceIdentity device_ {};
    bool authorityLost_ {};
};
}

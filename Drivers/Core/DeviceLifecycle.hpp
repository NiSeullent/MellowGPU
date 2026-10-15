// SPDX-License-Identifier: MIT
// Original MellowGPU common policy code. No upstream driver source is copied.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace MellowGPUCore {
enum class LeaseKind : uint8_t { NativeOperation, Pin, Submission, Publication, Display };
enum class Retirement : uint8_t { Unknown, KnownRetired };
enum class NativeQuiescence : uint8_t { Unknown, KnownQuiescent, KnownNotQuiescent };
enum class TransitionGoal : uint8_t { None, Reset, Stop };
enum class DevicePhase : uint8_t { Invalid, Running, ResetPending, Lost, StopPending, Quiescing, QuiescentRetained, Stopped };
enum class LifecycleStatus : uint8_t {
    Success, InvalidInput, InvalidIdentity, AdmissionClosed, NoSlot, CounterExhausted,
    WrongDevice, StaleGeneration, InvalidToken, StaleToken, AlreadyReleased,
    UnknownFlightRetained, Busy, PlanActive, NoPlan, Superseded, InvalidGeneration,
    AlreadyQuiescent, QuiescenceRequired, NativeAttemptFailed
};
struct LeaseToken {
    uint64_t device {}, generation {}, serial {};
    uint32_t slot {};
    LeaseKind kind {LeaseKind::NativeOperation};
    uint8_t reserved[3] {};
};
struct QuiescencePlan {
    uint64_t device {}, generation {}, serial {};
    TransitionGoal goal {TransitionGoal::None};
    uint8_t reserved[7] {};
};
static_assert(sizeof(LeaseToken)==32,"fixed lease record");
static_assert(sizeof(QuiescencePlan)==32,"fixed plan record");
struct LifecycleLimits {
    // An adapter may impose a smaller lifetime token budget; never wrap it.
    uint64_t leaseSerialCeiling {UINT64_MAX};
    uint64_t planSerialCeiling {UINT64_MAX};
};
struct DeviceSnapshot {
    uint64_t device {}, generation {};
    DevicePhase phase {DevicePhase::Invalid};
    TransitionGoal goal {TransitionGoal::None};
    uint32_t leases {}, unknownLeases {}, leasesByKind[5] {};
    bool admissionOpen {}, quiescenceActive {}, nativeQuiescenceAcknowledged {};
};

// Externally serialized policy state, not a native synchronization primitive or
// hardware attestation. The adapter owns/retains this object and native backing
// while any lease or quiescence plan exists. No callback is invoked by this core.
class DeviceLifecycle {
public:
    static constexpr uint32_t MaxLeases=64;
    DeviceLifecycle(uint64_t device,uint64_t generation,LifecycleLimits limits={})
        : device_(device),generation_(generation),limits_(limits) {
        if(device && generation && limits.leaseSerialCeiling && limits.planSerialCeiling)
            phase_=DevicePhase::Running;
    }
    DeviceLifecycle(const DeviceLifecycle&)=delete;
    DeviceLifecycle& operator=(const DeviceLifecycle&)=delete;
    LifecycleStatus acquire(LeaseKind kind,LeaseToken *out) {
        if(!out)return LifecycleStatus::InvalidInput;
        *out={};
        if(!validKind(kind))return LifecycleStatus::InvalidInput;
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(phase_!=DevicePhase::Running)return LifecycleStatus::AdmissionClosed;
        if(leaseSerial_==limits_.leaseSerialCeiling)return LifecycleStatus::CounterExhausted;
        for(uint32_t i=0;i<MaxLeases;++i) {
            Slot &s=slots_[i];
            if(s.live)continue;
            s.serial=++leaseSerial_;s.kind=kind;s.live=true;s.unknown=false;
            *out={device_,generation_,s.serial,i,kind,{0,0,0}};
            return LifecycleStatus::Success;
        }
        return LifecycleStatus::NoSlot;
    }
    LifecycleStatus release(const LeaseToken &token,Retirement retirement) {
        if(retirement!=Retirement::Unknown && retirement!=Retirement::KnownRetired)
            return LifecycleStatus::InvalidInput;
        const LifecycleStatus valid=validateLease(token);
        if(valid!=LifecycleStatus::Success)return valid;
        Slot &s=slots_[token.slot];
        if(retirement==Retirement::Unknown) {
            s.unknown=true;return LifecycleStatus::UnknownFlightRetained;
        }
        s.live=false;s.unknown=false;
        return LifecycleStatus::Success;
    }
    LifecycleStatus requestReset() {
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(phase_==DevicePhase::Stopped || goal_==TransitionGoal::Stop)
            return LifecycleStatus::AdmissionClosed;
        if(goal_==TransitionGoal::Reset)return LifecycleStatus::Success;
        goal_=TransitionGoal::Reset;phase_=DevicePhase::ResetPending;
        markNativeFlightUnknown();return LifecycleStatus::Success;
    }
    LifecycleStatus requestStop() {
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(phase_==DevicePhase::Stopped)return LifecycleStatus::Success;
        if(goal_==TransitionGoal::Stop)return LifecycleStatus::Success;
        quiescenceAcknowledged_=false;
        goal_=TransitionGoal::Stop;phase_=DevicePhase::StopPending;
        return LifecycleStatus::Success;
    }
    LifecycleStatus markDeviceLost() {
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(phase_==DevicePhase::Stopped)return LifecycleStatus::AdmissionClosed;
        lost_=true;
        quiescenceAcknowledged_=false;
        if(planActive_)planSuperseded_=true;
        if(goal_==TransitionGoal::None)goal_=TransitionGoal::Reset;
        phase_=DevicePhase::Lost;markNativeFlightUnknown();
        return LifecycleStatus::Success;
    }
    LifecycleStatus prepareQuiescence(QuiescencePlan *out) {
        if(!out)return LifecycleStatus::InvalidInput;
        *out={};
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(planActive_)return LifecycleStatus::PlanActive;
        if(goal_==TransitionGoal::None || phase_==DevicePhase::Stopped)
            return LifecycleStatus::AdmissionClosed;
        if(quiescenceAcknowledged_)return LifecycleStatus::AlreadyQuiescent;
        // Pins and GPU/display obligations must remain owned while reset stops
        // unknown DMA. Waiting for them before reset would deadlock recovery.
        for(const Slot &s:slots_)if(s.live &&
            (s.kind==LeaseKind::NativeOperation || s.kind==LeaseKind::Publication))
            return LifecycleStatus::Busy;
        if(planSerial_==limits_.planSerialCeiling)return LifecycleStatus::CounterExhausted;
        ++planSerial_;planActive_=true;planGoal_=goal_;planLost_=lost_;planSuperseded_=false;
        *out={device_,generation_,planSerial_,planGoal_,{0,0,0,0,0,0,0}};
        return LifecycleStatus::Success;
    }
    LifecycleStatus finishQuiescence(const QuiescencePlan &plan,NativeQuiescence outcome) {
        if(outcome!=NativeQuiescence::Unknown && outcome!=NativeQuiescence::KnownQuiescent &&
           outcome!=NativeQuiescence::KnownNotQuiescent)
            return LifecycleStatus::InvalidInput;
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(plan.device!=device_)return LifecycleStatus::WrongDevice;
        if(plan.generation!=generation_)return LifecycleStatus::StaleGeneration;
        if(!plan.serial || (plan.goal!=TransitionGoal::Reset && plan.goal!=TransitionGoal::Stop))
            return LifecycleStatus::InvalidToken;
        for(uint8_t b:plan.reserved)if(b)return LifecycleStatus::InvalidToken;
        if(!planActive_)return LifecycleStatus::NoPlan;
        if(plan.serial!=planSerial_)return LifecycleStatus::StaleToken;
        if(plan.goal!=planGoal_)return LifecycleStatus::InvalidToken;
        if(outcome==NativeQuiescence::Unknown)return LifecycleStatus::UnknownFlightRetained;
        if(outcome==NativeQuiescence::KnownNotQuiescent) {
            // The adapter guarantees this control attempt returned with no
            // asynchronous control operation still in flight. GPU/display DMA
            // can remain unknown: retain every lease and keep admission closed.
            planActive_=false;quiescenceAcknowledged_=false;
            return LifecycleStatus::NativeAttemptFailed;
        }
        // A native callback can close/escalate state while the adapter's lock
        // is released. Finish that old boundary, but do not reopen or destroy
        // anything using its superseded reset result.
        if(planSuperseded_ || goal_!=planGoal_ || lost_!=planLost_) {
            planActive_=false;return LifecycleStatus::Superseded;
        }
        // This only acknowledges the adapter's native barrier. Existing pins,
        // submissions and display references remain live until their separate
        // explicit retirement; neither stop nor generation changes here.
        planActive_=false;quiescenceAcknowledged_=true;
        phase_=DevicePhase::QuiescentRetained;
        return LifecycleStatus::Success;
    }
    LifecycleStatus completeTransition(uint64_t nextGeneration=0) {
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(goal_==TransitionGoal::None || phase_==DevicePhase::Stopped)
            return LifecycleStatus::AdmissionClosed;
        if(planActive_)return LifecycleStatus::PlanActive;
        if(!quiescenceAcknowledged_)return LifecycleStatus::QuiescenceRequired;
        for(const Slot &s:slots_)if(s.live)return LifecycleStatus::Busy;
        if(goal_==TransitionGoal::Stop) {
            if(nextGeneration)return LifecycleStatus::InvalidGeneration;
            quiescenceAcknowledged_=false;phase_=DevicePhase::Stopped;
            return LifecycleStatus::Success;
        }
        if(!nextGeneration || nextGeneration<=generation_)
            return LifecycleStatus::InvalidGeneration;
        generation_=nextGeneration;quiescenceAcknowledged_=false;goal_=TransitionGoal::None;
        lost_=false;phase_=DevicePhase::Running;
        return LifecycleStatus::Success;
    }
    DeviceSnapshot snapshot()const {
        DeviceSnapshot out;
        out.device=device_;out.generation=generation_;
        out.phase=planActive_?DevicePhase::Quiescing:phase_;out.goal=goal_;
        out.admissionOpen=phase_==DevicePhase::Running;out.quiescenceActive=planActive_;
        out.nativeQuiescenceAcknowledged=quiescenceAcknowledged_;
        for(const Slot &s:slots_)if(s.live) {
            ++out.leases;++out.leasesByKind[static_cast<uint8_t>(s.kind)];
            if(s.unknown)++out.unknownLeases;
        }
        return out;
    }
private:
    struct Slot {
        uint64_t serial {};
        LeaseKind kind {LeaseKind::NativeOperation};
        bool live {},unknown {};
    };
    static bool validKind(LeaseKind kind) {
        return static_cast<uint8_t>(kind)<=static_cast<uint8_t>(LeaseKind::Display);
    }
    LifecycleStatus validateLease(const LeaseToken &t)const {
        if(phase_==DevicePhase::Invalid)return LifecycleStatus::InvalidIdentity;
        if(t.device!=device_)return LifecycleStatus::WrongDevice;
        if(t.generation!=generation_)return LifecycleStatus::StaleGeneration;
        if(!t.serial || t.slot>=MaxLeases || !validKind(t.kind))return LifecycleStatus::InvalidToken;
        for(uint8_t b:t.reserved)if(b)return LifecycleStatus::InvalidToken;
        const Slot &s=slots_[t.slot];
        if(t.serial!=s.serial)return LifecycleStatus::StaleToken;
        if(t.kind!=s.kind)return LifecycleStatus::InvalidToken;
        return s.live?LifecycleStatus::Success:LifecycleStatus::AlreadyReleased;
    }
    void markNativeFlightUnknown() {
        for(Slot &s:slots_)if(s.live && s.kind!=LeaseKind::Pin)s.unknown=true;
    }
    uint64_t device_,generation_;
    LifecycleLimits limits_;
    Slot slots_[MaxLeases] {};
    uint64_t leaseSerial_ {},planSerial_ {};
    DevicePhase phase_ {DevicePhase::Invalid};
    TransitionGoal goal_ {TransitionGoal::None},planGoal_ {TransitionGoal::None};
    bool lost_ {},planLost_ {},planActive_ {},planSuperseded_ {},quiescenceAcknowledged_ {};
};
} // namespace MellowGPUCore

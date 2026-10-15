// SPDX-License-Identifier: MIT
#include "../Drivers/Core/DeviceLifecycle.hpp"
#include <stdio.h>
#include <stdint.h>
using namespace MellowGPUCore;
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x);return false;}}while(false)
using S=LifecycleStatus;

// A client ending does not return a device-owned native lease. A copied token
// represents the native owner, independently of the request/client stack.
static bool native_work_outlives_client_and_teardown() {
    DeviceLifecycle device(10,1);LeaseToken native;
    { LeaseToken clientRequest;
      REQUIRE(device.acquire(LeaseKind::Submission,&clientRequest)==S::Success);native=clientRequest; }
    REQUIRE(device.requestStop()==S::Success);LeaseToken refused;
    REQUIRE(device.acquire(LeaseKind::Pin,&refused)==S::AdmissionClosed);
    REQUIRE(device.completeTransition()==S::QuiescenceRequired);QuiescencePlan plan;
    REQUIRE(device.prepareQuiescence(&plan)==S::Success);
    REQUIRE(device.snapshot().leases==1 && device.snapshot().quiescenceActive);
    REQUIRE(device.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(device.snapshot().leases==1 && device.snapshot().nativeQuiescenceAcknowledged);
    REQUIRE(device.completeTransition()==S::Busy);
    REQUIRE(device.release(native,Retirement::KnownRetired)==S::Success);
    REQUIRE(device.completeTransition()==S::Success);
    REQUIRE(device.snapshot().phase==DevicePhase::Stopped);
    REQUIRE(device.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::NoPlan);
    return true;
}
static bool every_typed_owner_blocks_teardown() {
    const LeaseKind kinds[]={LeaseKind::NativeOperation,LeaseKind::Pin,LeaseKind::Submission,LeaseKind::Publication,LeaseKind::Display};
    for(unsigned i=0;i<5;++i) {
        DeviceLifecycle d(40+i,7);LeaseToken lease;QuiescencePlan p;
        REQUIRE(d.acquire(kinds[i],&lease)==S::Success);
        REQUIRE(d.snapshot().leases==1 && d.snapshot().leasesByKind[i]==1);
        REQUIRE(d.requestStop()==S::Success);
        if(i==0 || i==3) {
            REQUIRE(d.prepareQuiescence(&p)==S::Busy);
            REQUIRE(d.completeTransition()==S::QuiescenceRequired);
            REQUIRE(d.release(lease,Retirement::KnownRetired)==S::Success);
            REQUIRE(d.prepareQuiescence(&p)==S::Success);
            REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
        } else {
            REQUIRE(d.prepareQuiescence(&p)==S::Success);
            REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
            REQUIRE(d.completeTransition()==S::Busy);
            REQUIRE(d.release(lease,Retirement::KnownRetired)==S::Success);
        }
        REQUIRE(d.completeTransition()==S::Success);
    }
    return true;
}
static bool timeout_and_device_loss_retain_unknown_flight() {
    DeviceLifecycle d(12,4);LeaseToken submission,pin;
    REQUIRE(d.acquire(LeaseKind::Submission,&submission)==S::Success);
    REQUIRE(d.acquire(LeaseKind::Pin,&pin)==S::Success);
    REQUIRE(d.release(submission,Retirement::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.snapshot().leases==2 && d.snapshot().unknownLeases==1);
    REQUIRE(d.markDeviceLost()==S::Success);REQUIRE(d.requestReset()==S::Success);QuiescencePlan p;
    REQUIRE(d.prepareQuiescence(&p)==S::Success);
    REQUIRE(d.finishQuiescence(p,NativeQuiescence::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.snapshot().generation==4 && d.snapshot().quiescenceActive && d.snapshot().leases==2);
    REQUIRE(d.completeTransition(5)==S::PlanActive);
    REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.snapshot().leases==2 && d.snapshot().unknownLeases==1 && !d.snapshot().admissionOpen);
    REQUIRE(d.prepareQuiescence(&p)==S::AlreadyQuiescent);
    REQUIRE(d.completeTransition(5)==S::Busy);
    REQUIRE(d.release(pin,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.release(submission,Retirement::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.completeTransition(5)==S::Busy);
    REQUIRE(d.release(submission,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.completeTransition(5)==S::Success);
    REQUIRE(d.snapshot().generation==5 && d.snapshot().admissionOpen);
    REQUIRE(d.release(submission,Retirement::KnownRetired)==S::StaleGeneration);
    return true;
}
// Regression: native reset must be reachable while a hung GPU still owns pinned
// work, but acknowledging the native barrier must not erase those obligations.
static bool gpu_hang_allows_quiesce_without_freeing_pins() {
    DeviceLifecycle d(30,1);LeaseToken submit,pin,display,operation,publication;
    REQUIRE(d.acquire(LeaseKind::Submission,&submit)==S::Success);
    REQUIRE(d.acquire(LeaseKind::Pin,&pin)==S::Success);
    REQUIRE(d.acquire(LeaseKind::Display,&display)==S::Success);
    REQUIRE(d.acquire(LeaseKind::NativeOperation,&operation)==S::Success);
    REQUIRE(d.acquire(LeaseKind::Publication,&publication)==S::Success);
    REQUIRE(d.release(submit,Retirement::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.markDeviceLost()==S::Success);QuiescencePlan plan;
    REQUIRE(d.prepareQuiescence(&plan)==S::Busy);
    REQUIRE(d.release(operation,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.prepareQuiescence(&plan)==S::Busy);
    REQUIRE(d.release(publication,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.prepareQuiescence(&plan)==S::Success);
    REQUIRE(d.snapshot().leases==3 && d.snapshot().generation==1);
    REQUIRE(d.completeTransition(2)==S::PlanActive);
    REQUIRE(d.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.snapshot().leases==3 && d.snapshot().unknownLeases==2 && d.snapshot().generation==1);
    REQUIRE(d.completeTransition(2)==S::Busy);
    REQUIRE(d.release(submit,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.completeTransition(2)==S::Busy);
    REQUIRE(d.release(display,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.completeTransition(2)==S::Busy);
    REQUIRE(d.release(pin,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.completeTransition(2)==S::Success && d.snapshot().generation==2);
    return true;
}
static bool exact_identity_prevents_aba_and_double_release() {
    DeviceLifecycle d(13,3),other(14,3);LeaseToken a,b;
    REQUIRE(d.acquire(LeaseKind::Pin,&a)==S::Success);
    REQUIRE(other.release(a,Retirement::KnownRetired)==S::WrongDevice);
    LeaseToken wrong=a;wrong.kind=LeaseKind::Display;
    REQUIRE(d.release(wrong,Retirement::KnownRetired)==S::InvalidToken);
    wrong=a;wrong.reserved[2]=1;REQUIRE(d.release(wrong,Retirement::KnownRetired)==S::InvalidToken);
    REQUIRE(d.release(a,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.release(a,Retirement::KnownRetired)==S::AlreadyReleased);
    REQUIRE(d.acquire(LeaseKind::Pin,&b)==S::Success);
    REQUIRE(a.slot==b.slot && a.serial!=b.serial);
    REQUIRE(d.release(a,Retirement::KnownRetired)==S::StaleToken);
    REQUIRE(d.snapshot().leases==1);
    REQUIRE(d.release(b,Retirement::KnownRetired)==S::Success);
    return true;
}
static bool capacity_and_counter_exhaustion_fail_without_wrapping() {
    DeviceLifecycle d(15,1);LeaseToken tokens[DeviceLifecycle::MaxLeases],extra;
    for(unsigned i=0;i<DeviceLifecycle::MaxLeases;++i)REQUIRE(d.acquire(LeaseKind::NativeOperation,&tokens[i])==S::Success);
    REQUIRE(d.acquire(LeaseKind::NativeOperation,&extra)==S::NoSlot && !extra.serial);
    REQUIRE(d.snapshot().leases==64);
    for(const auto&t:tokens)REQUIRE(d.release(t,Retirement::KnownRetired)==S::Success);
    DeviceLifecycle bounded(16,1,{2,1});LeaseToken a,b;
    REQUIRE(bounded.acquire(LeaseKind::Pin,&a)==S::Success);
    REQUIRE(bounded.release(a,Retirement::KnownRetired)==S::Success);
    REQUIRE(bounded.acquire(LeaseKind::Pin,&b)==S::Success);
    REQUIRE(b.serial==2 && b.serial!=a.serial);
    REQUIRE(bounded.acquire(LeaseKind::Pin,&extra)==S::CounterExhausted && !extra.serial);
    REQUIRE(bounded.release(b,Retirement::KnownRetired)==S::Success);
    REQUIRE(bounded.requestReset()==S::Success);QuiescencePlan p;
    REQUIRE(bounded.prepareQuiescence(&p)==S::Success);
    REQUIRE(bounded.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(bounded.completeTransition(2)==S::Success);
    REQUIRE(bounded.acquire(LeaseKind::Pin,&extra)==S::CounterExhausted);
    REQUIRE(bounded.requestStop()==S::Success);
    REQUIRE(bounded.prepareQuiescence(&p)==S::CounterExhausted);
    REQUIRE(bounded.snapshot().phase!=DevicePhase::Stopped && !bounded.snapshot().admissionOpen);
    return true;
}
static bool generation_changes_only_after_final_native_quiescence() {
    DeviceLifecycle d(17,8);QuiescencePlan p,other;
    REQUIRE(d.prepareQuiescence(&p)==S::AdmissionClosed);
    REQUIRE(d.completeTransition(9)==S::AdmissionClosed);
    REQUIRE(d.requestReset()==S::Success);
    REQUIRE(d.completeTransition(9)==S::QuiescenceRequired);
    REQUIRE(d.prepareQuiescence(&p)==S::Success);
    REQUIRE(d.prepareQuiescence(&other)==S::PlanActive);
    other=p;other.serial++;
    REQUIRE(d.finishQuiescence(other,NativeQuiescence::KnownQuiescent)==S::StaleToken);
    REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.snapshot().generation==8 && !d.snapshot().admissionOpen);
    REQUIRE(d.completeTransition(8)==S::InvalidGeneration);
    REQUIRE(d.completeTransition(0)==S::InvalidGeneration);
    REQUIRE(d.snapshot().generation==8 && d.snapshot().nativeQuiescenceAcknowledged);
    REQUIRE(d.completeTransition(9)==S::Success);
    REQUIRE(d.snapshot().generation==9 && d.snapshot().admissionOpen);
    REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::StaleGeneration);
    DeviceLifecycle last(18,UINT64_MAX);REQUIRE(last.requestReset()==S::Success);
    REQUIRE(last.prepareQuiescence(&p)==S::Success);
    REQUIRE(last.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(last.completeTransition(1)==S::InvalidGeneration);
    REQUIRE(last.snapshot().generation==UINT64_MAX && !last.snapshot().admissionOpen);
    return true;
}
// Models the adapter dropping its external lock around a native callback. The
// callback may request termination/loss, but cannot start another boundary or
// make the old reset plan reopen admission after its native operation returns.
static bool callback_reentrancy_supersedes_completion_plan() {
    DeviceLifecycle d(19,1);QuiescencePlan reset,second;
    REQUIRE(d.requestReset()==S::Success);
    REQUIRE(d.prepareQuiescence(&reset)==S::Success);
    REQUIRE(d.requestStop()==S::Success);
    REQUIRE(d.prepareQuiescence(&second)==S::PlanActive);
    REQUIRE(d.finishQuiescence(reset,NativeQuiescence::KnownQuiescent)==S::Superseded);
    REQUIRE(d.snapshot().generation==1 && !d.snapshot().admissionOpen);
    REQUIRE(d.prepareQuiescence(&second)==S::Success);
    REQUIRE(d.finishQuiescence(reset,NativeQuiescence::KnownQuiescent)==S::StaleToken);
    REQUIRE(d.finishQuiescence(second,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.completeTransition()==S::Success);
    REQUIRE(d.snapshot().phase==DevicePhase::Stopped);
    DeviceLifecycle lost(20,1);
    REQUIRE(lost.requestReset()==S::Success);REQUIRE(lost.prepareQuiescence(&reset)==S::Success);
    REQUIRE(lost.markDeviceLost()==S::Success);
    REQUIRE(lost.finishQuiescence(reset,NativeQuiescence::KnownQuiescent)==S::Superseded);
    REQUIRE(lost.snapshot().generation==1 && !lost.snapshot().admissionOpen);
    REQUIRE(lost.prepareQuiescence(&second)==S::Success);
    // A further loss during a second native boundary must not be mistaken for
    // the already latched loss that began recovery.
    REQUIRE(lost.markDeviceLost()==S::Success);
    REQUIRE(lost.finishQuiescence(second,NativeQuiescence::KnownQuiescent)==S::Superseded);
    REQUIRE(lost.prepareQuiescence(&second)==S::Success);
    REQUIRE(lost.finishQuiescence(second,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(lost.completeTransition(2)==S::Success);
    return true;
}
static bool late_loss_and_stop_invalidate_an_acknowledged_barrier() {
    DeviceLifecycle d(31,1);LeaseToken pin;QuiescencePlan plan;
    REQUIRE(d.acquire(LeaseKind::Pin,&pin)==S::Success);
    REQUIRE(d.requestReset()==S::Success);
    REQUIRE(d.prepareQuiescence(&plan)==S::Success);
    REQUIRE(d.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.snapshot().nativeQuiescenceAcknowledged);
    REQUIRE(d.markDeviceLost()==S::Success);
    REQUIRE(!d.snapshot().nativeQuiescenceAcknowledged && d.snapshot().leases==1);
    REQUIRE(d.completeTransition(2)==S::QuiescenceRequired);
    REQUIRE(d.prepareQuiescence(&plan)==S::Success);
    REQUIRE(d.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.release(pin,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.requestStop()==S::Success);
    REQUIRE(!d.snapshot().nativeQuiescenceAcknowledged);
    REQUIRE(d.completeTransition()==S::QuiescenceRequired);
    REQUIRE(d.prepareQuiescence(&plan)==S::Success);
    REQUIRE(d.finishQuiescence(plan,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.requestStop()==S::Success); // same intent is idempotent
    REQUIRE(d.completeTransition(2)==S::InvalidGeneration);
    REQUIRE(d.completeTransition()==S::Success);
    REQUIRE(d.snapshot().generation==1 && d.snapshot().phase==DevicePhase::Stopped);
    return true;
}
static bool completed_native_failure_allows_bounded_retry_without_retirement() {
    DeviceLifecycle d(32,9,{UINT64_MAX,2});LeaseToken pin,submission,refused;
    REQUIRE(d.acquire(LeaseKind::Pin,&pin)==S::Success);
    REQUIRE(d.acquire(LeaseKind::Submission,&submission)==S::Success);
    REQUIRE(d.release(submission,Retirement::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.markDeviceLost()==S::Success);QuiescencePlan first,retry,third;
    REQUIRE(d.prepareQuiescence(&first)==S::Success);
    REQUIRE(d.finishQuiescence(first,NativeQuiescence::KnownNotQuiescent)==S::NativeAttemptFailed);
    REQUIRE(d.snapshot().leases==2 && d.snapshot().unknownLeases==1 && d.snapshot().generation==9);
    REQUIRE(!d.snapshot().quiescenceActive && !d.snapshot().nativeQuiescenceAcknowledged && !d.snapshot().admissionOpen);
    REQUIRE(d.completeTransition(10)==S::QuiescenceRequired);
    REQUIRE(d.acquire(LeaseKind::NativeOperation,&refused)==S::AdmissionClosed);
    REQUIRE(d.prepareQuiescence(&retry)==S::Success && retry.serial!=first.serial);
    REQUIRE(d.finishQuiescence(first,NativeQuiescence::KnownQuiescent)==S::StaleToken);
    REQUIRE(d.finishQuiescence(retry,NativeQuiescence::Unknown)==S::UnknownFlightRetained);
    REQUIRE(d.prepareQuiescence(&third)==S::PlanActive);
    REQUIRE(d.finishQuiescence(retry,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.completeTransition(10)==S::Busy);
    REQUIRE(d.release(pin,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.release(submission,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.completeTransition(10)==S::Success);
    REQUIRE(d.requestStop()==S::Success);
    REQUIRE(d.prepareQuiescence(&third)==S::CounterExhausted);
    REQUIRE(d.snapshot().phase!=DevicePhase::Stopped && !d.snapshot().admissionOpen);
    return true;
}
static bool malformed_records_cannot_mutate_live_state() {
    DeviceLifecycle invalid(0,1),badGeneration(1,0),badLimits(1,1,{0,1});LeaseToken t;
    REQUIRE(invalid.acquire(LeaseKind::Pin,&t)==S::InvalidIdentity);
    REQUIRE(badGeneration.requestReset()==S::InvalidIdentity);
    REQUIRE(badLimits.requestStop()==S::InvalidIdentity);
    DeviceLifecycle d(21,1);
    REQUIRE(d.acquire(static_cast<LeaseKind>(255),&t)==S::InvalidInput);
    REQUIRE(d.acquire(LeaseKind::Pin,nullptr)==S::InvalidInput);
    REQUIRE(d.acquire(LeaseKind::Pin,&t)==S::Success);
    REQUIRE(d.release(t,static_cast<Retirement>(255))==S::InvalidInput);
    LeaseToken wrong=t;wrong.slot=64;REQUIRE(d.release(wrong,Retirement::KnownRetired)==S::InvalidToken);
    REQUIRE(d.snapshot().leases==1);
    REQUIRE(d.release(t,Retirement::KnownRetired)==S::Success);
    REQUIRE(d.requestStop()==S::Success);QuiescencePlan p;
    REQUIRE(d.prepareQuiescence(nullptr)==S::InvalidInput);
    REQUIRE(d.prepareQuiescence(&p)==S::Success);
    auto wrongPlan=p;wrongPlan.reserved[6]=1;
    REQUIRE(d.finishQuiescence(wrongPlan,NativeQuiescence::KnownQuiescent)==S::InvalidToken);
    REQUIRE(d.finishQuiescence(p,static_cast<NativeQuiescence>(255))==S::InvalidInput);
    REQUIRE(d.snapshot().quiescenceActive);
    REQUIRE(d.finishQuiescence(p,NativeQuiescence::KnownQuiescent)==S::Success);
    REQUIRE(d.completeTransition()==S::Success);
    REQUIRE(d.requestReset()==S::AdmissionClosed);
    REQUIRE(d.markDeviceLost()==S::AdmissionClosed);
    return true;
}
int main() {
    struct Test{const char *name;bool(*run)();};
    const Test tests[]={
        {"native work outlives client; teardown waits",native_work_outlives_client_and_teardown},
        {"every typed lease blocks teardown",every_typed_owner_blocks_teardown},
        {"GPU hang reaches native quiesce without freeing pins",gpu_hang_allows_quiesce_without_freeing_pins},
        {"timeouts and loss retain unknown flight",timeout_and_device_loss_retain_unknown_flight},
        {"identity prevents ABA and double release",exact_identity_prevents_aba_and_double_release},
        {"capacity and serial exhaustion cannot wrap",capacity_and_counter_exhaustion_fail_without_wrapping},
        {"native quiescence precedes generation change",generation_changes_only_after_final_native_quiescence},
        {"reentrant callback supersedes completion plan",callback_reentrancy_supersedes_completion_plan},
        {"late loss or stop invalidates acknowledged barrier",late_loss_and_stop_invalidate_an_acknowledged_barrier},
        {"completed native failure permits bounded retained retry",completed_native_failure_allows_bounded_retry_without_retirement},
        {"malformed records preserve live state",malformed_records_cannot_mutate_live_state}};
    for(const auto&t:tests){if(!t.run())return 42;printf("PASS %s\n",t.name);}
    puts("PASS 11 lifecycle policy groups; no native or GPU proof");return 0;
}

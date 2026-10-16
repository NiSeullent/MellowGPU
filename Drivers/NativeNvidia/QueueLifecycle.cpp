// SPDX-License-Identifier: MIT
// Original MellowGPU NVIDIA queue lifecycle adapter. No upstream driver source is copied.
#include "QueueLifecycle.hpp"

namespace MellowNativeNvidia {
using MellowGPUCore::DevicePhase;
using MellowGPUCore::LeaseKind;
using MellowGPUCore::LeaseToken;
using MellowGPUCore::LifecycleStatus;
using MellowGPUCore::NativeQuiescence;
using MellowGPUCore::Retirement;
using MellowGPUCore::TransitionGoal;
using Status = QueueLifecycleStatus;

namespace {
bool sameChannel(const ChannelBinding &a, const ChannelBinding &b)
{
    return a.device.vendor == b.device.vendor && a.device.device == b.device.device &&
        a.device.registryId == b.device.registryId && a.device.epoch == b.device.epoch &&
        a.owner == b.owner && a.vm == b.vm && a.channel == b.channel;
}
QueueLifecycleResult outcome(Status status, LifecycleStatus lifecycle = LifecycleStatus::Success)
{
    QueueLifecycleResult result;
    result.status = status;
    result.lifecycle = lifecycle;
    return result;
}
QueueLifecycleResult policy(LifecycleStatus lifecycle)
{
    return outcome(lifecycle == LifecycleStatus::AdmissionClosed ? Status::AdmissionClosed : Status::Policy, lifecycle);
}
}

QueueLifecycle::QueueLifecycle(MellowGPUCore::DeviceLifecycle &device, GpFifoQueue &queue, DeviceQuiescence authority)
    : device_(device), queue_(queue), authority_(authority) {}

uint32_t QueueLifecycle::jobLeases() const
{
    uint32_t count = 0;
    for (const auto &job : jobs_)
        count += job.live;
    return count;
}

LifecycleStatus QueueLifecycle::admission() const
{
    const auto snapshot = device_.snapshot();
    if (snapshot.phase == DevicePhase::Invalid)
        return LifecycleStatus::InvalidIdentity;
    return snapshot.admissionOpen ? LifecycleStatus::Success : LifecycleStatus::AdmissionClosed;
}

// A live lease of this lifecycle cannot fail validation: its generation only
// advances once no lease is live. Failure keeps the token so it stays visible.
bool QueueLifecycle::retireLease(LeaseToken &lease)
{
    if (device_.release(lease, Retirement::KnownRetired) != LifecycleStatus::Success)
        return false;
    lease = {};
    return true;
}

// Native work may still consume the resources: keep every lease, close admission.
void QueueLifecycle::faulted(QueueLifecycleResult &result, Status status)
{
    result.status = status;
    result.lifecycle = device_.requestReset();
}

// Ends only obligations a queue boundary proved retired. A job lease ends when
// its record reports no holds after GPU completion or a proven channel stop
// (JobState::Reset is only written after quiesce). A tokenless rollback and the
// channel lease end only when reset() returned Ok: binding detached, claim
// released and every job record's holds gone.
void QueueLifecycle::collect(bool resetProven)
{
    for (auto &job : jobs_) {
        if (!job.live)
            continue;
        bool retired = resetProven;
        if (!retired && job.fence.generation) {
            JobResult record;
            retired = queue_.query(job.fence, record) == QueueStatus::Ok && !record.resourcesHeld &&
                (record.state == JobState::Completed || record.state == JobState::Reset);
        }
        if (retired && retireLease(job.lease))
            job = {};
    }
    if (resetProven && channelHeld_ && retireLease(channel_)) {
        channelHeld_ = false;
        releasedGeneration_ = binding_.channelGeneration;
        releasedValid_ = true;
        binding_ = {};
    }
}

QueueLifecycleResult QueueLifecycle::observe(QueueStatus status)
{
    QueueLifecycleResult result;
    result.queueCalled = true;
    result.queue = status;
    switch (status) {
    case QueueStatus::Ok:
        result.status = Status::Ok;
        break;
    case QueueStatus::ClockRegression:
        result.status = Status::Rejected; // Refused before any native read.
        break;
    case QueueStatus::Busy:
        result.status = Status::Busy;
        break;
    case QueueStatus::TimedOut:
        faulted(result, Status::TimedOut); // A deadline is neither completion nor quiescence.
        break;
    default:
        faulted(result, Status::QueueFault);
        break;
    }
    return result;
}

QueueLifecycleResult QueueLifecycle::attach(const ChannelBinding &binding)
{
    if (!authority_.context || !authority_.quiesce)
        return outcome(Status::Unavailable);
    // The lifecycle instance is the physical registry ID. bind() then requires
    // this identity to match the MemoryOwner's admitted ring/fence and the claim.
    if (binding.device.registryId != device_.snapshot().device)
        return outcome(Status::WrongDevice, LifecycleStatus::WrongDevice);
    const LifecycleStatus open = admission();
    if (open != LifecycleStatus::Success)
        return policy(open);
    if (identityFixed_ && !sameChannel(binding, identity_))
        return outcome(Status::BindingIdentityMismatch);
    if (releasedValid_ && binding.channelGeneration <= releasedGeneration_)
        return outcome(Status::StaleBinding);
    if (channelHeld_ || queue_.state() != QueueState::Detached)
        return outcome(Status::Busy);
    LeaseToken operation, channel;
    LifecycleStatus status = device_.acquire(LeaseKind::NativeOperation, &operation);
    if (status != LifecycleStatus::Success)
        return policy(status);
    status = device_.acquire(LeaseKind::Submission, &channel);
    if (status != LifecycleStatus::Success) {
        retireLease(operation); // No native call was made.
        return policy(status);
    }
    QueueLifecycleResult result;
    result.queueCalled = true;
    result.queue = queue_.bind(binding);
    retireLease(operation); // The synchronous bind call has returned.
    if (queue_.state() == QueueState::Detached) {
        // bind() only stays Detached with no claim and no ring/fence hold.
        retireLease(channel);
        result.status = Status::Rejected;
        return result;
    }
    channel_ = channel;
    binding_ = binding;
    channelHeld_ = true;
    if (!identityFixed_) {
        identity_ = binding;
        identityFixed_ = true;
    }
    if (result.queue == QueueStatus::Ok && queue_.state() == QueueState::Ready) {
        result.status = Status::Ok;
        return result;
    }
    // Unknown claim or retained ring/fence: the lease stays until reset() Ok.
    faulted(result, result.queue == QueueStatus::PublicationUnknown ? Status::PublicationUnknown : Status::QueueFault);
    return result;
}

QueueLifecycleResult QueueLifecycle::submit(const SubmitRequest &request, uint64_t now, FenceToken &token)
{
    token = {};
    const LifecycleStatus open = admission();
    if (open != LifecycleStatus::Success)
        return policy(open);
    if (!channelHeld_ || queue_.state() != QueueState::Ready)
        return outcome(Status::Busy);
    Job *slot = nullptr;
    for (auto &job : jobs_)
        if (!job.live) {
            slot = &job;
            break;
        }
    if (!slot)
        return outcome(Status::Busy);
    LeaseToken work, publication;
    LifecycleStatus status = device_.acquire(LeaseKind::Submission, &work);
    if (status != LifecycleStatus::Success)
        return policy(status);
    status = device_.acquire(LeaseKind::Publication, &publication);
    if (status != LifecycleStatus::Success) {
        retireLease(work); // Nothing was published.
        return policy(status);
    }
    QueueLifecycleResult result;
    result.queueCalled = true;
    result.queue = queue_.submit(request, now, token);
    retireLease(publication); // The synchronous publication call has returned.
    if (result.queue == QueueStatus::Ok) {
        *slot = {work, token, true};
        result.status = Status::Ok;
        return result;
    }
    if (result.queue == QueueStatus::PublicationUnknown) {
        // GP_PUT may have exposed the job; a later rejection cannot unpublish it.
        *slot = {work, token, true};
        faulted(result, Status::PublicationUnknown);
        return result;
    }
    if (queue_.state() == QueueState::Ready) {
        // Still Ready: refused or rolled back before GP_PUT with holds released.
        retireLease(work);
        result.status = Status::Rejected;
        return result;
    }
    // No token: the failed rollback may still hold memory (record unaddressable).
    *slot = {work, {}, true};
    faulted(result, Status::QueueFault);
    return result;
}

QueueLifecycleResult QueueLifecycle::poll(uint64_t now)
{
    if (!channelHeld_)
        return outcome(Status::Busy);
    const QueueStatus status = queue_.poll(now);
    collect(false);
    return observe(status);
}

QueueLifecycleResult QueueLifecycle::expire(uint64_t now)
{
    if (!channelHeld_)
        return outcome(Status::Busy);
    return observe(queue_.expire(now));
}

QueueLifecycleResult QueueLifecycle::recover(uint64_t nextGeneration)
{
    return transition(TransitionGoal::Reset, nextGeneration);
}

QueueLifecycleResult QueueLifecycle::close()
{
    return transition(TransitionGoal::Stop, 0);
}

QueueLifecycleResult QueueLifecycle::transition(TransitionGoal goal, uint64_t nextGeneration)
{
    if (!authority_.context || !authority_.quiesce)
        return outcome(Status::Unavailable);
    auto snapshot = device_.snapshot();
    if (snapshot.phase == DevicePhase::Invalid)
        return policy(LifecycleStatus::InvalidIdentity);
    LifecycleStatus status = LifecycleStatus::Success;
    if (goal == TransitionGoal::Reset) {
        // Validated before admission closes or any native state changes.
        if (nextGeneration <= snapshot.generation)
            return policy(LifecycleStatus::InvalidGeneration);
        status = device_.requestReset(); // A pending stop dominates: AdmissionClosed.
    } else {
        if (snapshot.phase == DevicePhase::Stopped)
            return outcome(Status::Ok); // Stopped already required every lease retired.
        status = device_.requestStop();
    }
    if (status != LifecycleStatus::Success)
        return policy(status);
    snapshot = device_.snapshot();
    if (planActive_ && !snapshot.quiescenceActive)
        planActive_ = false;
    if (!planActive_) {
        status = device_.prepareQuiescence(&plan_);
        if (status == LifecycleStatus::Success)
            planActive_ = true;
        else if (status != LifecycleStatus::AlreadyQuiescent)
            // A foreign in-flight native call or plan; refused before native calls.
            return outcome(status == LifecycleStatus::Busy || status == LifecycleStatus::PlanActive ?
                Status::Busy : Status::Policy, status);
    }
    QueueLifecycleResult result;
    result.queueCalled = true;
    result.queue = queue_.reset(); // Stops THIS channel, then releases its holds.
    const bool resetProven = result.queue == QueueStatus::Ok;
    collect(resetProven);
    if (result.queue == QueueStatus::QuiesceFailed) {
        // Channel stop unproven: every lease and hold stays; the plan stays open.
        result.status = Status::QuiescenceUnknown;
        result.lifecycle = planActive_ ? device_.finishQuiescence(plan_, NativeQuiescence::Unknown) :
            LifecycleStatus::AlreadyQuiescent;
        return result;
    }
    if (planActive_) {
        // The channel stop covers one channel. KnownQuiescent additionally needs
        // the physical owner's proof for every consumer of the whole device.
        const bool quiescent = authority_.quiesce(authority_.context, plan_);
        result.lifecycle = device_.finishQuiescence(plan_,
            quiescent ? NativeQuiescence::KnownQuiescent : NativeQuiescence::Unknown);
        if (result.lifecycle == LifecycleStatus::UnknownFlightRetained) {
            result.status = Status::QuiescenceUnknown;
            return result;
        }
        planActive_ = false;
        if (result.lifecycle == LifecycleStatus::Superseded) {
            result.status = Status::Superseded; // Loss/stop arrived meanwhile; never reopen on it.
            return result;
        }
        if (result.lifecycle != LifecycleStatus::Success) {
            result.status = Status::Policy;
            return result;
        }
    }
    if (!resetProven) {
        // Channel stopped but not released: its lease still holds the device.
        result.status = Status::CleanupPending;
        return result;
    }
    result.lifecycle = device_.completeTransition(goal == TransitionGoal::Reset ? nextGeneration : 0);
    if (result.lifecycle == LifecycleStatus::Success)
        result.status = Status::Ok;
    else if (result.lifecycle == LifecycleStatus::Busy)
        result.status = channelHeld_ || jobLeases() ? Status::CleanupPending : Status::ForeignObligations;
    else if (result.lifecycle == LifecycleStatus::AdmissionClosed)
        result.status = Status::AdmissionClosed;
    else
        result.status = Status::Policy;
    return result;
}

QueueLifecycleResult QueueLifecycle::retire(FenceToken token)
{
    for (const auto &job : jobs_)
        if (job.live && job.fence.generation == token.generation && job.fence.sequence == token.sequence)
            return outcome(Status::Busy); // A job record never outlives its lease.
    QueueLifecycleResult result;
    result.queueCalled = true;
    result.queue = queue_.retire(token);
    result.status = result.queue == QueueStatus::Ok ? Status::Ok :
        result.queue == QueueStatus::Busy ? Status::Busy : Status::Rejected;
    return result;
}

} // namespace MellowNativeNvidia

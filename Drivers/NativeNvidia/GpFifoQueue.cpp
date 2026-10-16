// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#include "GpFifoQueue.hpp"

namespace MellowNativeNvidia {
namespace {
bool sameDevice(MellowNative::DeviceIdentity a, MellowNative::DeviceIdentity b)
{
    return a.vendor == b.vendor && a.device == b.device && a.registryId == b.registryId && a.epoch == b.epoch;
}
bool sameHandle(MellowNative::MemoryHandle a, MellowNative::MemoryHandle b)
{
    return a.slot == b.slot && a.generation == b.generation && a.registryId == b.registryId && a.epoch == b.epoch;
}
bool span(uint64_t offset, uint64_t bytes, uint64_t limit)
{
    return bytes && offset <= limit && bytes <= limit - offset;
}
bool gpuSpan(const MellowNative::MemoryView &view, uint64_t offset, uint64_t bytes)
{
    return span(offset, bytes, view.bytes) && span(offset, bytes, view.mapping.bytes) &&
        view.mapping.address <= UINT64_MAX - offset &&
        bytes - 1 <= UINT64_MAX - (view.mapping.address + offset);
}
}

GpFifoQueue::GpFifoQueue(MellowNative::MemoryOwner &memory, QueueTransport transport)
    : memory_(memory), transport_(transport) {}

bool GpFifoQueue::completeTransport() const
{
    return transport_.context && transport_.claim && transport_.sealPushbuffer && transport_.beforePut && transport_.writePut &&
        transport_.afterPut && transport_.readCurrentToken && transport_.ringDoorbell &&
        transport_.readCompletedFence && transport_.quiesce && transport_.releaseChannel;
}

bool GpFifoQueue::tick(uint64_t now)
{
    if (clockSet_ && now < lastTime_)
        return false;
    clockSet_ = true;
    lastTime_ = now;
    return true;
}

QueueStatus GpFifoQueue::view(MellowNative::MemoryHandle handle, MellowNative::MemoryView &out) const
{
    if (memory_.inspect(binding_.owner, handle, out) != MellowNative::MemoryStatus::Ok)
        return QueueStatus::Ownership;
    if (!sameDevice(out.device, binding_.device) || !sameHandle(out.handle, handle) ||
        out.owner != binding_.owner || out.state != MellowNative::MemoryState::Mapped ||
        !out.pin.cookie || !out.pin.cpu || !out.mapping.cookie || !out.bytes ||
        out.pin.bytes < out.bytes || out.mapping.bytes < out.bytes)
        return QueueStatus::Ownership;
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::releaseBindingHolds()
{
    if (fenceHeld_) {
        if (memory_.release(binding_.owner, binding_.fence) != MellowNative::MemoryStatus::Ok)
            return QueueStatus::MemoryFailure;
        fenceHeld_ = false;
    }
    if (ringHeld_) {
        if (memory_.release(binding_.owner, binding_.ring) != MellowNative::MemoryStatus::Ok)
            return QueueStatus::MemoryFailure;
        ringHeld_ = false;
    }
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::bind(const ChannelBinding &binding)
{
    if (state_ != QueueState::Detached || ringHeld_ || fenceHeld_ || claimed_)
        return QueueStatus::Busy;
    if (!completeTransport())
        return QueueStatus::Unavailable;
    if (generation_ == UINT64_MAX)
        return QueueStatus::SequenceExhausted;
    if (binding.channelClass != uint32_t(Mellow::PortedNvidia::ChannelClass::AmpereA))
        return QueueStatus::UnsupportedClass;
    if (binding.device.vendor != 0x10de || !binding.device.device || binding.device.device == UINT16_MAX ||
        !binding.device.registryId ||
        !binding.device.epoch || !binding.owner || !binding.vm || !binding.channel || !binding.channelGeneration ||
        binding.ringEntries < 2 || binding.ringEntries > MaxRingEntries ||
        (binding.ringEntries & (binding.ringEntries - 1)) || binding.ringOffset % 8 || binding.fenceOffset % 4 ||
        sameHandle(binding.ring, binding.fence))
        return QueueStatus::Invalid;
    binding_ = binding;
    QueueStatus status = view(binding.ring, ring_);
    if (status == QueueStatus::Ok)
        status = view(binding.fence, fence_);
    if (status == QueueStatus::Ok && (ring_.jobHolds || fence_.jobHolds ||
        !ring_.mapping.writable || !fence_.mapping.writable ||
        !gpuSpan(ring_, binding.ringOffset, uint64_t(binding.ringEntries) * 8) ||
        !gpuSpan(fence_, binding.fenceOffset, 4)))
        status = QueueStatus::Invalid;
    if (status != QueueStatus::Ok) {
        binding_ = {};
        ring_ = {};
        fence_ = {};
        return status;
    }
    if (memory_.retain(binding.owner, binding.ring) != MellowNative::MemoryStatus::Ok) {
        binding_ = {};
        ring_ = {};
        fence_ = {};
        return QueueStatus::MemoryFailure;
    }
    ringHeld_ = true;
    if (memory_.retain(binding.owner, binding.fence) != MellowNative::MemoryStatus::Ok) {
        status = releaseBindingHolds();
        if (status != QueueStatus::Ok) {
            state_ = QueueState::NeedsReset;
            stopped_ = true; // claim has not run: nothing exposed to this channel.
            return status;
        }
        binding_ = {};
        ring_ = {};
        fence_ = {};
        return QueueStatus::MemoryFailure;
    }
    fenceHeld_ = true;
    if (view(binding_.ring, ring_) != QueueStatus::Ok || view(binding_.fence, fence_) != QueueStatus::Ok) {
        state_ = QueueState::NeedsReset;
        stopped_ = true;
        return QueueStatus::Ownership;
    }
    ChannelObservation observation {};
    const Operation result = transport_.claim(transport_.context, binding_, ring_, fence_, observation);
    if (result == Operation::Rejected) {
        status = releaseBindingHolds();
        if (status != QueueStatus::Ok) {
            state_ = QueueState::NeedsReset;
            stopped_ = true;
            return status;
        }
        binding_ = {};
        ring_ = {};
        fence_ = {};
        return QueueStatus::TransportFailure;
    }
    claimed_ = true; // Unknown may have acquired ownership; never discard it.
    stopped_ = false;
    if (result != Operation::Done) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::PublicationUnknown;
    }
    if (!sameDevice(observation.device, binding_.device) || observation.owner != binding_.owner ||
        observation.vm != binding_.vm || observation.channel != binding_.channel ||
        observation.channelGeneration != binding_.channelGeneration ||
        observation.channelClass != binding_.channelClass || observation.ringEntries != binding_.ringEntries ||
        observation.gpPut >= binding_.ringEntries || observation.gpGet != observation.gpPut ||
        observation.completedFence != observation.lastQueuedFence) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::Ownership;
    }
    if (observation.mode != ChannelMode::Ordinary || observation.ringDomain != RingDomain::CoherentSystem) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::UnsupportedChannel;
    }
    put_ = observation.gpPut;
    pending_ = 0;
    completed_ = nextSequence_ = observation.completedFence;
    bindingVerified_ = true;
    state_ = QueueState::Ready;
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::releaseJob(Job &job)
{
    while (job.heldCount) {
        if (memory_.release(binding_.owner, job.held[job.heldCount - 1]) != MellowNative::MemoryStatus::Ok)
            return QueueStatus::MemoryFailure;
        --job.heldCount;
    }
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::uncertain(Job &job, uint32_t newPut)
{
    job.state = JobState::PublicationUnknown;
    job.ringPending = true;
    put_ = newPut;
    ++pending_;
    nextSequence_ = job.token.sequence;
    state_ = QueueState::NeedsReset;
    return QueueStatus::PublicationUnknown;
}

QueueStatus GpFifoQueue::submit(const SubmitRequest &request, uint64_t now, FenceToken &token)
{
    token = {};
    if (state_ != QueueState::Ready)
        return QueueStatus::Busy;
    if (!tick(now))
        return QueueStatus::ClockRegression;
    if (!request.range.commandBytes || request.range.commandBytes % 4 || request.range.offset % 4 ||
        request.range.capacityBytes % 4 || request.range.commandBytes > request.range.capacityBytes ||
        request.range.capacityBytes > Mellow::PortedNvidia::GpfifoBytesMax || request.deadline <= now ||
        request.resourceCount > MaxDataResources || (request.resourceCount && !request.resources) ||
        sameHandle(request.commands, binding_.ring) || sameHandle(request.commands, binding_.fence))
        return QueueStatus::Invalid;
    if (nextSequence_ == UINT32_MAX)
        return QueueStatus::SequenceExhausted;
    if (pending_ >= binding_.ringEntries - 1)
        return QueueStatus::Capacity; // Keep one sentinel ring slot unavailable.
    Job *job = nullptr;
    for (auto &candidate : jobs_)
        if (candidate.state == JobState::Free) {
            job = &candidate;
            break;
        }
    if (!job)
        return QueueStatus::Capacity;
    MellowNative::MemoryView commands {}, resources[MaxDataResources] {};
    QueueStatus status = view(request.commands, commands);
    if (status != QueueStatus::Ok)
        return status;
    // A sealed command allocation cannot be rewritten by another live job.
    if (commands.jobHolds || !gpuSpan(commands, request.range.offset, request.range.capacityBytes))
        return QueueStatus::Ownership;
    for (uint32_t i = 0; i < request.resourceCount; ++i) {
        const auto handle = request.resources[i].handle;
        if (sameHandle(handle, request.commands) || sameHandle(handle, binding_.ring) || sameHandle(handle, binding_.fence))
            return QueueStatus::Ownership;
        for (uint32_t j = 0; j < i; ++j)
            if (sameHandle(handle, request.resources[j].handle))
                return QueueStatus::Ownership;
        for (const auto &liveJob : jobs_)
            if (liveJob.heldCount && sameHandle(handle, liveJob.held[0]))
                return QueueStatus::Ownership; // A live private PB is never data.
        status = view(handle, resources[i]);
        if (status != QueueStatus::Ok)
            return status;
        if (request.resources[i].gpuWrites && !resources[i].mapping.writable)
            return QueueStatus::Ownership;
    }
    *job = {};
    job->token = {generation_, nextSequence_ + 1};
    job->deadline = request.deadline;
    // Before PUT, rollback is safe because no GPU work has been published.
    auto unpublished = [&](QueueStatus failure) {
        if (releaseJob(*job) != QueueStatus::Ok) {
            job->state = JobState::PublicationUnknown;
            state_ = QueueState::NeedsReset;
            return QueueStatus::MemoryFailure;
        }
        *job = {};
        if (failure == QueueStatus::MemoryFailure)
            state_ = QueueState::NeedsReset;
        return failure;
    };
    if (memory_.retain(binding_.owner, request.commands) != MellowNative::MemoryStatus::Ok)
        return unpublished(QueueStatus::MemoryFailure);
    job->held[job->heldCount++] = request.commands;
    for (uint32_t i = 0; i < request.resourceCount; ++i) {
        if (memory_.retain(binding_.owner, request.resources[i].handle) != MellowNative::MemoryStatus::Ok)
            return unpublished(QueueStatus::MemoryFailure);
        job->held[job->heldCount++] = request.resources[i].handle;
    }
    status = view(request.commands, commands);
    for (uint32_t i = 0; status == QueueStatus::Ok && i < request.resourceCount; ++i)
        status = view(request.resources[i].handle, resources[i]);
    if (status != QueueStatus::Ok)
        return unpublished(status);
    uint64_t submittedBytes = 0;
    status = transport_.sealPushbuffer(transport_.context, binding_, commands, request.range, resources,
        request.resourceCount, fence_.mapping.address + binding_.fenceOffset, job->token.sequence, submittedBytes);
    if (status != QueueStatus::Ok)
        return unpublished(status);
    if (submittedBytes < request.range.commandBytes || submittedBytes > request.range.capacityBytes ||
        !gpuSpan(commands, request.range.offset, submittedBytes))
        return unpublished(QueueStatus::Invalid);
    Mellow::PortedNvidia::GpfifoEntry entry {};
    if (Mellow::PortedNvidia::encodeGpfifoPushbuffer(Mellow::PortedNvidia::ChannelClass::AmpereA,
        commands.mapping.address + request.range.offset, submittedBytes, Mellow::PortedNvidia::Sync::Proceed,
        entry) != Mellow::PortedNvidia::Status::Ok)
        return unpublished(QueueStatus::Invalid);
    for (uint32_t i = 0; i < job->heldCount; ++i)
        if (memory_.synchronizeRetained(binding_.owner, job->held[i], MellowNative::CacheDirection::ForDevice) !=
            MellowNative::MemoryStatus::Ok)
            return unpublished(QueueStatus::MemoryFailure);
    uint8_t bytes[8] {};
    Mellow::PortedNvidia::writeGpfifoLittleEndian(entry, bytes);
    auto *destination = ring_.pin.cpu + binding_.ringOffset + uint64_t(put_) * 8;
    for (uint32_t i = 0; i < 8; ++i)
        destination[i] = bytes[i];
    if (memory_.synchronizeRetained(binding_.owner, binding_.ring, MellowNative::CacheDirection::ForDevice) !=
        MellowNative::MemoryStatus::Ok)
        return unpublished(QueueStatus::MemoryFailure);
    status = transport_.beforePut(transport_.context, binding_);
    if (status != QueueStatus::Ok)
        return unpublished(status);
    const uint32_t newPut = (put_ + 1) % binding_.ringEntries;
    const Operation putResult = transport_.writePut(transport_.context, binding_, newPut);
    if (putResult == Operation::Rejected)
        return unpublished(QueueStatus::TransportFailure);
    // GP_PUT alone can expose work. Later rejection cannot unpublish consumed
    // commands; preserve holds even if the doorbell reports rejection.
    if (putResult != Operation::Done) {
        job->tokenIssued = true;
        token = job->token;
        return uncertain(*job, newPut);
    }
    status = transport_.afterPut(transport_.context, binding_);
    uint32_t currentToken = 0;
    if (status == QueueStatus::Ok)
        status = transport_.readCurrentToken(transport_.context, binding_, currentToken);
    if (status != QueueStatus::Ok || transport_.ringDoorbell(transport_.context, binding_, currentToken) != Operation::Done) {
        job->tokenIssued = true;
        token = job->token;
        return uncertain(*job, newPut);
    }
    job->state = JobState::Submitted;
    job->ringPending = true;
    put_ = newPut;
    ++pending_;
    nextSequence_ = job->token.sequence;
    job->tokenIssued = true;
    token = job->token;
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::poll(uint64_t now)
{
    if (state_ == QueueState::Detached || !claimed_ || stopped_ || !bindingVerified_)
        return QueueStatus::Busy;
    if (!tick(now))
        return QueueStatus::ClockRegression;
    if (memory_.synchronizeRetained(binding_.owner, binding_.fence, MellowNative::CacheDirection::ForCpu) !=
        MellowNative::MemoryStatus::Ok) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::MemoryFailure;
    }
    uint32_t observed = 0;
    const QueueStatus status = transport_.readCompletedFence(transport_.context, binding_, fence_, observed);
    if (status != QueueStatus::Ok) {
        state_ = QueueState::NeedsReset;
        return status;
    }
    if (observed < completed_ || observed > nextSequence_) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::InvalidFence;
    }
    completed_ = observed;
    for (auto &job : jobs_)
        if (job.ringPending && job.token.generation == generation_ && job.token.sequence <= observed) {
            job.ringPending = false;
            --pending_;
            job.state = JobState::Completed;
            if (releaseJob(job) != QueueStatus::Ok) {
                state_ = QueueState::NeedsReset;
                return QueueStatus::MemoryFailure;
            }
        }
    return expire(now);
}

QueueStatus GpFifoQueue::expire(uint64_t now)
{
    if (state_ == QueueState::Detached)
        return QueueStatus::Busy;
    if (!tick(now))
        return QueueStatus::ClockRegression;
    bool expired = false;
    for (auto &job : jobs_)
        if (job.state == JobState::Submitted && job.deadline <= now) {
            job.state = JobState::TimedOut;
            expired = true;
        }
    if (expired) {
        state_ = QueueState::NeedsReset;
        return QueueStatus::TimedOut;
    }
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::reset()
{
    if (state_ == QueueState::Detached && !ringHeld_ && !fenceHeld_ && !claimed_)
        return QueueStatus::Ok;
    state_ = QueueState::NeedsReset;
    bindingVerified_ = false;
    if (!stopped_) {
        if (!transport_.quiesce || !transport_.quiesce(transport_.context, binding_))
            return QueueStatus::QuiesceFailed;
        stopped_ = true;
    }
    for (auto &job : jobs_)
        if (job.state != JobState::Free) {
            if (job.state != JobState::Completed)
                job.state = JobState::Reset;
            if (job.ringPending) {
                job.ringPending = false;
                --pending_;
            }
            if (releaseJob(job) != QueueStatus::Ok)
                return QueueStatus::MemoryFailure;
        }
    if (claimed_) {
        if (!transport_.releaseChannel || transport_.releaseChannel(transport_.context, binding_) != Operation::Done)
            return QueueStatus::TransportFailure;
        claimed_ = false;
    }
    const QueueStatus status = releaseBindingHolds();
    if (status != QueueStatus::Ok)
        return status;
    // No native channel, ring/fence lease or job hold remains. A pre-PUT
    // rollback failure returned no token, so its caller cannot acknowledge the
    // internal record. Reclaim only those records after complete recovery;
    // every issued Completed/Reset token still requires explicit retire().
    for (auto &job : jobs_)
        if (job.state != JobState::Free && !job.tokenIssued)
            job = {};
    binding_ = {};
    ring_ = {};
    fence_ = {};
    put_ = pending_ = nextSequence_ = completed_ = 0;
    clockSet_ = false;
    lastTime_ = 0;
    if (generation_ != UINT64_MAX)
        ++generation_;
    state_ = QueueState::Detached;
    return QueueStatus::Ok;
}

GpFifoQueue::Job *GpFifoQueue::find(FenceToken token)
{
    for (auto &job : jobs_)
        if (job.state != JobState::Free && job.token.generation == token.generation && job.token.sequence == token.sequence)
            return &job;
    return nullptr;
}

const GpFifoQueue::Job *GpFifoQueue::find(FenceToken token) const
{
    for (const auto &job : jobs_)
        if (job.state != JobState::Free && job.token.generation == token.generation && job.token.sequence == token.sequence)
            return &job;
    return nullptr;
}

QueueStatus GpFifoQueue::query(FenceToken token, JobResult &result) const
{
    result = {};
    const Job *job = find(token);
    if (!job)
        return QueueStatus::StaleToken;
    result = {job->state, job->heldCount != 0, job->token};
    return QueueStatus::Ok;
}

QueueStatus GpFifoQueue::retire(FenceToken token)
{
    Job *job = find(token);
    if (!job)
        return QueueStatus::StaleToken;
    if (job->heldCount || job->ringPending || (job->state != JobState::Completed && job->state != JobState::Reset))
        return QueueStatus::Busy;
    *job = {};
    return QueueStatus::Ok;
}

} // namespace MellowNativeNvidia

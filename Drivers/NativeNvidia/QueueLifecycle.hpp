// SPDX-License-Identifier: MIT
// Original MellowGPU NVIDIA queue lifecycle adapter. No upstream driver source is copied.
#pragma once
#include "../Core/DeviceLifecycle.hpp"
#include "GpFifoQueue.hpp"

namespace MellowNativeNvidia {

enum class QueueLifecycleStatus : uint8_t {
    Ok, Unavailable, WrongDevice, AdmissionClosed, Busy, StaleBinding, Rejected,
    PublicationUnknown, TimedOut, QueueFault, QuiescenceUnknown, CleanupPending,
    Superseded, ForeignObligations, Policy, BindingIdentityMismatch
};
struct QueueLifecycleResult {
    QueueLifecycleStatus status {QueueLifecycleStatus::Busy};
    MellowGPUCore::LifecycleStatus lifecycle {MellowGPUCore::LifecycleStatus::Success};
    QueueStatus queue {QueueStatus::Unavailable}; // Meaningful only when queueCalled.
    bool queueCalled {};
};

// Trusted physical-owner boundary for the WHOLE lifecycle device instance,
// never a user-client callback. True proves every consumer of plan.device
// (all channels and engines, including ones holding no lease, display scanout,
// firmware and IRQ/workers) stopped with DMA idle, and excludes new work until
// the transition completes. A timeout, an idle-looking ring or one channel's
// QueueTransport::quiesce cannot return true. False keeps the plan active.
struct DeviceQuiescence {
    void *context {};
    bool (*quiesce)(void *, const MellowGPUCore::QuiescencePlan &) {};
};

// Joins one GpFifoQueue to the device-wide MellowGPUCore::DeviceLifecycle.
// Lease mapping: the bind() call is a NativeOperation and the submit() call a
// Publication; each ends when that synchronous call returns. The claimed
// channel (ring/fence holds) and every job (command/data holds) are Submission
// leases: they execute on the GPU, outlive reset requests and block transition.
//
// Identity: the lifecycle device is the physical registry ID. attach() refuses
// a binding whose device.registryId differs, before any lease or native call;
// GpFifoQueue::bind then requires that identity to equal the MemoryOwner's
// admitted ring/fence identity and the claim's negotiated channel identity.
// The first bind retaining a channel or backing fixes this adapter's device
// vendor/device/registry/epoch, owner, VM and channel identity. Different
// identities return BindingIdentityMismatch before any lease or native call;
// they require another adapter. A cleanly refused bind fixes no identity.
//
// KnownRetired is asserted only at a proven queue boundary: GPU tracking
// semaphore completion with every hold released, rollback before GP_PUT
// exposure, or after GpFifoQueue::reset() proved this channel stopped. A job
// lease ends when its record reports no holds; a tokenless failed rollback and
// the channel lease end only when reset() returns Ok (binding detached).
// GP_GET==GP_PUT, IRQ arrival, CPU-written semaphores, deadlines and doorbell
// rejection after GP_PUT are never retirement; they keep the lease and request
// reset. The channel stop covers THIS channel only: KnownQuiescent is reported
// to the lifecycle only after DeviceQuiescence also proves the whole device.
//
// Externally serialized with the queue, MemoryOwner and lifecycle. Native
// callbacks may only request stop/reset/loss on the lifecycle. The owner keeps
// all objects alive until close() returns Ok; no destructor releases.
// Lifecycle generation, FenceToken generation, channelGeneration and the
// MemoryOwner epoch stay independent; none is derived from another. These are
// MellowGPU native objects, not Nouveau/NVK or NVIDIA RM handles or channels.
class QueueLifecycle {
public:
    QueueLifecycle(MellowGPUCore::DeviceLifecycle &, GpFifoQueue &, DeviceQuiescence);
    QueueLifecycle(const QueueLifecycle &) = delete;
    QueueLifecycle &operator=(const QueueLifecycle &) = delete;
    // Claims through GpFifoQueue::bind. Only the fixed identity can reattach,
    // with a channel generation newer than its last released generation.
    QueueLifecycleResult attach(const ChannelBinding &);
    QueueLifecycleResult submit(const SubmitRequest &, uint64_t now, FenceToken &);
    // Fence-only retirement. Allowed while admission is closed, for draining.
    QueueLifecycleResult poll(uint64_t now);
    QueueLifecycleResult expire(uint64_t now);
    // Requests reset, stops and releases this channel through GpFifoQueue::reset(),
    // proves device-wide quiescence, then installs nextGeneration once no lease
    // remains. Repeat after QuiescenceUnknown, CleanupPending, Superseded or
    // ForeignObligations (other consumers still retiring); nothing is guessed.
    QueueLifecycleResult recover(uint64_t nextGeneration);
    // The same sequence ending in the terminal Stopped phase.
    QueueLifecycleResult close();
    // Client acknowledgement of a queue job record whose lease has ended.
    QueueLifecycleResult retire(FenceToken);
    bool channelHeld() const { return channelHeld_; }
    uint32_t jobLeases() const;
private:
    struct Job {
        MellowGPUCore::LeaseToken lease {};
        FenceToken fence {}; // Empty: a failed rollback may still hold memory.
        bool live {};
    };
    MellowGPUCore::DeviceLifecycle &device_;
    GpFifoQueue &queue_;
    DeviceQuiescence authority_ {};
    MellowGPUCore::LeaseToken channel_ {};
    MellowGPUCore::QuiescencePlan plan_ {};
    ChannelBinding binding_ {}, identity_ {};
    uint64_t releasedGeneration_ {};
    Job jobs_[GpFifoQueue::MaxJobs] {};
    bool channelHeld_ {}, planActive_ {}, identityFixed_ {}, releasedValid_ {};
    MellowGPUCore::LifecycleStatus admission() const;
    bool retireLease(MellowGPUCore::LeaseToken &);
    void faulted(QueueLifecycleResult &, QueueLifecycleStatus);
    void collect(bool resetProven);
    QueueLifecycleResult observe(QueueStatus);
    QueueLifecycleResult transition(MellowGPUCore::TransitionGoal, uint64_t nextGeneration);
};

} // namespace MellowNativeNvidia

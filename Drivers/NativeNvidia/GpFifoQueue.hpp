// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "../NativeGpu/MemoryOwner.hpp"
#include "../PortedNvidia/CommandEncoding.hpp"

namespace MellowNativeNvidia {

enum class QueueStatus : uint8_t {
    Ok, Unavailable, Invalid, UnsupportedClass, UnsupportedChannel, Ownership,
    MemoryFailure, Capacity, Busy, TransportFailure, PublicationUnknown,
    StaleToken, InvalidFence, SequenceExhausted, TimedOut, ClockRegression,
    QuiesceFailed
};
enum class Operation : uint8_t { Done, Rejected, Unknown };
enum class ChannelMode : uint8_t { Ordinary, Proxy, Confidential, WorkLaunch, Unsupported };
enum class RingDomain : uint8_t { CoherentSystem, NoncoherentSystem, Video, Unsupported };
enum class QueueState : uint8_t { Detached, Ready, NeedsReset };
enum class JobState : uint8_t { Free, Submitted, PublicationUnknown, Completed, TimedOut, Reset };
struct FenceToken { uint64_t generation {}; uint32_t sequence {}; };
struct ChannelBinding {
    // Values come from the physical/channel owner, never PCI/class spoofing.
    MellowNative::DeviceIdentity device {};
    uint64_t owner {}, vm {}, channel {}, channelGeneration {};
    uint32_t channelClass {}, ringEntries {};
    MellowNative::MemoryHandle ring {}, fence {};
    uint64_t ringOffset {}, fenceOffset {};
};
struct ChannelObservation {
    // claim returns the actually negotiated identity and empty, exclusively
    // owned channel. GP_GET == GP_PUT alone is NOT an idle/completion proof.
    MellowNative::DeviceIdentity device {};
    uint64_t owner {}, vm {}, channel {}, channelGeneration {};
    uint32_t channelClass {}, ringEntries {}, gpGet {}, gpPut {};
    uint32_t completedFence {}, lastQueuedFence {};
    ChannelMode mode {ChannelMode::Unsupported};
    RingDomain ringDomain {RingDomain::Unsupported};
};
struct PushbufferRange {
    uint64_t offset {}, commandBytes {}, capacityBytes {};
};
struct DataResource {
    MellowNative::MemoryHandle handle {};
    bool gpuWrites {};
};
struct SubmitRequest {
    MellowNative::MemoryHandle commands {};
    PushbufferRange range {};
    const DataResource *resources {};
    uint32_t resourceCount {};
    uint64_t deadline {};
};
struct JobResult {
    JobState state {JobState::Free};
    bool resourcesHeld {};
    FenceToken token {};
};

// Physical/channel boundary. Every callback is mandatory. Calls are serialized
// with MemoryOwner, CPU writers, reset, IRQ work and channel/MMIO lease teardown.
// This is trusted kernel input, never an unvalidated user-client callback table.
struct QueueTransport {
    void *context {};
    // Validates exact live device/VM/channel, real ring/fence mappings, ordinary
    // non-confidential mode and exclusive ownership; observes an idle tracking
    // fence with no queued work. Done holds that exclusive lease until release.
    // Rejected proves no lease acquired; Unknown requires quiescence/release.
    Operation (*claim)(void *, const ChannelBinding &, const MellowNative::MemoryView &ring,
        const MellowNative::MemoryView &fence, ChannelObservation &) {};
    // Validate actual bound engine commands, resolve every GPU memory access
    // against these retained views, and append a system-ordered GPU completion
    // release to this exact private pushbuffer. Reject unsafe/privileged/nested
    // commands. No GPU publication here. Return exact submitted bytes, including
    // the release. CPU/user writes are prohibited until all job holds are gone.
    // The fence is dedicated to this channel and NEVER completed by the CPU.
    QueueStatus (*sealPushbuffer)(void *, const ChannelBinding &,
        const MellowNative::MemoryView &commands, PushbufferRange,
        const MellowNative::MemoryView *resources, uint32_t resourceCount,
        uint64_t fenceGpuAddress, uint32_t fencePayload, uint64_t &submittedBytes) {};
    // Full CPU memory ordering after command/ring DMA synchronization. Includes
    // any adapter-required BAR write flush before exposing GP_PUT. Missing or
    // failed ordering is an error; a compiler/release fence alone is insufficient.
    QueueStatus (*beforePut)(void *, const ChannelBinding &) {};
    // Write to the measured channel GP_PUT binding; never a guessed register.
    // Rejected proves no GPU-visible PUT change. Unknown retains all resources.
    Operation (*writePut)(void *, const ChannelBinding &, uint32_t newPut) {};
    // System write ordering for PUT before reading the CURRENT work token.
    QueueStatus (*afterPut)(void *, const ChannelBinding &) {};
    QueueStatus (*readCurrentToken)(void *, const ChannelBinding &, uint32_t &) {};
    Operation (*ringDoorbell)(void *, const ChannelBinding &, uint32_t currentToken) {};
    // Actual dedicated GPU tracking semaphore, after acquire/system visibility.
    // Must reject channel error/identity loss and CPU-written/IRQ/GP_GET values.
    QueueStatus (*readCompletedFence)(void *, const ChannelBinding &,
        const MellowNative::MemoryView &fence, uint32_t &payload) {};
    // Proves channel stopped, all prior DMA idle and no future submissions/IRQ
    // work; keeps this condition through release under the same owner lock.
    bool (*quiesce)(void *, const ChannelBinding &) {};
    // Done proves channel and its register/memory leases no longer consume ring
    // or fence. Failure keeps both holds; retry after authoritative quiescence.
    Operation (*releaseChannel)(void *, const ChannelBinding &) {};
};

// Actual bounded GPFIFO publication and lifetime owner. Initial reviewed slice
// is negotiated AmpereA (0xC56F), ordinary coherent sysmem only, 40-bit fetch.
// Other classes/modes fail explicitly. This does not allocate firmware/channel,
// create GPU page tables, provide engine command sealing or advertise Metal.
// NVIDIA sources pinned at e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb:
// kernel-open/nvidia-uvm/uvm_channel.c (sentinel, publication, tracking fence),
// uvm_turing_host.c (inherited PUT ordering), uvm_hal.c (Ampere inheritance),
// kernel-open/common/inc/nv_uvm_types.h (dynamic work token after PUT/fence).
// Encoder retains separate source notices/provenance in ../PortedNvidia.
// No destructor releases live backing. Keep all owners alive until reset Ok.
class GpFifoQueue {
public:
    static constexpr uint32_t MaxJobs = 32, MaxDataResources = 8, MaxRingEntries = 256;
    GpFifoQueue(MellowNative::MemoryOwner &, QueueTransport);
    GpFifoQueue(const GpFifoQueue &) = delete;
    GpFifoQueue &operator=(const GpFifoQueue &) = delete;
    QueueStatus bind(const ChannelBinding &);
    QueueStatus submit(const SubmitRequest &, uint64_t now, FenceToken &);
    QueueStatus poll(uint64_t now);
    QueueStatus expire(uint64_t now);
    // Stops admission before quiescing. Success invalidates this binding; a new
    // authoritative channel/epoch binding is required. Never resets GPU by guess.
    // Returned tokens stay queryable until explicit retire(). Internal rollback
    // records whose token never escaped are reclaimed only after full reset.
    QueueStatus reset();
    QueueStatus query(FenceToken, JobResult &) const;
    QueueStatus retire(FenceToken);
    QueueState state() const { return state_; }
    uint32_t pending() const { return pending_; }
    uint32_t put() const { return put_; }
private:
    struct Job {
        FenceToken token {};
        JobState state {JobState::Free};
        uint64_t deadline {};
        MellowNative::MemoryHandle held[MaxDataResources + 1] {};
        uint32_t heldCount {};
        bool ringPending {};
        bool tokenIssued {};
    };
    MellowNative::MemoryOwner &memory_;
    QueueTransport transport_ {};
    ChannelBinding binding_ {};
    MellowNative::MemoryView ring_ {}, fence_ {};
    Job jobs_[MaxJobs] {};
    QueueState state_ {QueueState::Detached};
    uint64_t generation_ {1}, lastTime_ {};
    uint32_t put_ {}, pending_ {}, nextSequence_ {}, completed_ {};
    bool ringHeld_ {}, fenceHeld_ {}, claimed_ {}, stopped_ {}, clockSet_ {}, bindingVerified_ {};
    bool completeTransport() const;
    bool tick(uint64_t);
    QueueStatus view(MellowNative::MemoryHandle, MellowNative::MemoryView &) const;
    QueueStatus releaseJob(Job &);
    QueueStatus releaseBindingHolds();
    QueueStatus uncertain(Job &, uint32_t newPut);
    Job *find(FenceToken);
    const Job *find(FenceToken) const;
};

} // namespace MellowNativeNvidia

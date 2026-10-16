// SPDX-License-Identifier: MIT
// Host regression evidence only. Production MellowGPUCore::DeviceLifecycle,
// MemoryOwner, GpFifoQueue, sealCopyPushbuffer and QueueLifecycle execute against
// an injected SIMULATED physical transport and SIMULATED whole-device quiescence
// owner. The test device model performs only the semaphore release encoded in
// the actual sealed pushbuffer. No IOKit,
// firmware, physical GPU, Metal or WindowServer execution occurs or is implied.
#include "../Drivers/NativeNvidia/QueueLifecycle.hpp"
#include "../Drivers/NativeNvidia/CopyPushbuffer.hpp"
#include "../Drivers/PortedNvidiaCopy/NvidiaCopyEncoder.hpp"
#include <initializer_list>
#include <stdio.h>
#include <string.h>

using namespace MellowNative;
using namespace MellowNativeNvidia;
using MellowGPUCore::DeviceLifecycle;
using MellowGPUCore::DevicePhase;
using MellowGPUCore::LeaseKind;
using MellowGPUCore::LeaseToken;
using MellowGPUCore::LifecycleStatus;
using MellowGPUCore::Retirement;
using MellowGPUCore::TransitionGoal;
using S = QueueLifecycleStatus;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); return false; } } while (false)

namespace {
constexpr uint64_t Owner = 31, OtherDevice = 0x4e56, GpuBase = 0x01000000;
constexpr uint32_t Submission = uint32_t(LeaseKind::Submission), Publication = uint32_t(LeaseKind::Publication);
const DeviceIdentity Device {0x10de, 0x2206, 0x88776655, 9};
enum class Hook : uint8_t { None, LoseDevice, RequestStop };

uint32_t le(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
void writeLe(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i)
        bytes[i] = uint8_t(value >> (i * 8));
}
bool sameBinding(const ChannelBinding &a, const ChannelBinding &b)
{
    return a.device.registryId == b.device.registryId && a.device.epoch == b.device.epoch && a.owner == b.owner &&
        a.vm == b.vm && a.channel == b.channel && a.channelGeneration == b.channelGeneration;
}

// Simulated physical owner: DMA backend, channel registers and a test device
// model. Injected transport for host execution, never NVIDIA hardware evidence.
struct Physical {
    alignas(4096) uint8_t pages[MemoryOwner::MaxAllocations][4096] {};
    uint64_t dma[MemoryOwner::MaxAllocations] {};
    uint32_t mapCookies[MemoryOwner::MaxAllocations] {};
    DeviceLifecycle *lifecycle {};
    CopySealerContext sealer {};
    ChannelBinding claimed {};
    Operation claimResult {Operation::Done}, putResult {Operation::Done}, bellResult {Operation::Done};
    Operation releaseResult {Operation::Done};
    QueueStatus afterResult {QueueStatus::Ok}, tokenResult {QueueStatus::Ok};
    Hook onPut {Hook::None}, onQuiesce {Hook::None};
    bool channelClaimed {}, channelStopped {}, quiesceResult {true}, syncFails {}, otherConsumerActive {};
    uint32_t gpPut {}, gpGet {}, executed {}, workToken {0x5150}, rung {}, released {}, violations {};
    char trace[256] {};
    uint32_t length {};
    void record(char event)
    {
        if (length + 1 < sizeof(trace))
            trace[length++] = event;
        else
            ++violations;
    }
    bool since(uint32_t mark, const char *expected) const
    {
        const size_t count = strlen(expected);
        return length - mark == count && !memcmp(trace + mark, expected, count);
    }
};
Physical &physical(void *opaque) { return *static_cast<Physical *>(opaque); }

bool admitted(void *, const DeviceIdentity &device)
{
    return device.vendor == Device.vendor && device.device == Device.device &&
        device.registryId == Device.registryId && device.epoch == Device.epoch;
}
MemoryStatus pin(void *opaque, const DeviceIdentity &device, uint64_t, const MemoryHandle &handle,
    uint64_t bytes, DmaPin &out)
{
    auto &p = physical(opaque);
    if (handle.slot >= MemoryOwner::MaxAllocations || bytes != 4096)
        return MemoryStatus::Invalid;
    p.dma[handle.slot] = 0x100000 + uint64_t(handle.slot) * 4096;
    out = {p.pages[handle.slot], p.pages[handle.slot], &p.dma[handle.slot], 1, bytes, device.registryId};
    return MemoryStatus::Ok;
}
MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, DmaPin &) { return MemoryStatus::Ok; }
MemoryStatus sync(void *opaque, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, CacheDirection)
{
    return physical(opaque).syncFails ? MemoryStatus::IoFailure : MemoryStatus::Ok;
}
MemoryStatus map(void *opaque, const DeviceIdentity &, uint64_t, const MemoryHandle &handle,
    const DmaPin &pin, bool writable, GpuMapping &out)
{
    auto &p = physical(opaque);
    out = {&p.mapCookies[handle.slot], GpuBase + uint64_t(handle.slot) * 4096, pin.bytes, writable};
    return MemoryStatus::Ok;
}
MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, GpuMapping &)
{
    return MemoryStatus::Ok;
}
bool retired(void *, const MemoryView &) { return true; }
bool stopMemory(void *, const DeviceIdentity &) { return true; }
MemoryBackend backend(Physical &p) { return {&p, admitted, pin, unpin, sync, map, unmap, retired, stopMemory}; }

// Simulated VM/engine owner for the production copy sealer.
QueueStatus authority(void *opaque, const ChannelBinding &binding, const MemoryView &, const MemoryView *, uint32_t,
    const MemoryView &, CopyEngineObservation &out)
{
    auto &p = physical(opaque);
    if (!p.channelClaimed || p.channelStopped || !sameBinding(binding, p.claimed))
        return QueueStatus::Ownership;
    out = {binding.device, binding.owner, binding.vm, binding.channel, binding.channelGeneration,
        binding.channelClass, 0xC6B5, 4, ChannelMode::Ordinary, RingDomain::CoherentSystem};
    return QueueStatus::Ok;
}
Operation claim(void *opaque, const ChannelBinding &binding, const MemoryView &ring, const MemoryView &fence,
    ChannelObservation &out)
{
    auto &p = physical(opaque);
    p.record('C');
    if (p.channelClaimed || ring.jobHolds != 1 || fence.jobHolds != 1)
        ++p.violations;
    const uint32_t completed = le(fence.pin.cpu + binding.fenceOffset);
    out = {binding.device, binding.owner, binding.vm, binding.channel, binding.channelGeneration, binding.channelClass,
        binding.ringEntries, p.gpPut, p.gpPut, completed, completed, ChannelMode::Ordinary, RingDomain::CoherentSystem};
    if (p.claimResult != Operation::Rejected) { // Unknown may have taken the channel.
        p.channelClaimed = true;
        p.channelStopped = false;
        p.claimed = binding;
    }
    return p.claimResult;
}
QueueStatus seal(void *opaque, const ChannelBinding &binding, const MemoryView &commands, PushbufferRange range,
    const MemoryView *resources, uint32_t count, uint64_t fenceAddress, uint32_t payload, uint64_t &bytes)
{
    auto &p = physical(opaque);
    p.record('S');
    if (!p.channelClaimed || p.channelStopped)
        ++p.violations;
    return sealCopyPushbuffer(&p.sealer, binding, commands, range, resources, count, fenceAddress, payload, bytes);
}
QueueStatus beforePut(void *opaque, const ChannelBinding &)
{
    physical(opaque).record('B');
    return QueueStatus::Ok;
}
Operation writePut(void *opaque, const ChannelBinding &, uint32_t value)
{
    auto &p = physical(opaque);
    p.record('P');
    if (!p.channelClaimed || p.channelStopped)
        ++p.violations;
    if (p.putResult != Operation::Rejected) { // Unknown may have reached GP_PUT.
        p.gpPut = value;
        ++p.workToken;
    }
    if (p.onPut == Hook::LoseDevice)
        (void)p.lifecycle->markDeviceLost(); // Native loss callback during publication.
    return p.putResult;
}
QueueStatus afterPut(void *opaque, const ChannelBinding &)
{
    auto &p = physical(opaque);
    p.record('A');
    return p.afterResult;
}
QueueStatus currentToken(void *opaque, const ChannelBinding &, uint32_t &out)
{
    auto &p = physical(opaque);
    p.record('T');
    out = p.workToken;
    return p.tokenResult;
}
Operation doorbell(void *opaque, const ChannelBinding &, uint32_t token)
{
    auto &p = physical(opaque);
    p.record('D');
    p.rung = token;
    return p.bellResult;
}
// Simulated acquire read of the dedicated semaphore. The modeled adapter
// rejects any value its GPU did not release (a CPU store), as the contract requires.
QueueStatus readFence(void *opaque, const ChannelBinding &binding, const MemoryView &fence, uint32_t &out)
{
    auto &p = physical(opaque);
    p.record('F');
    if (!p.channelClaimed || p.channelStopped)
        ++p.violations;
    const uint32_t value = le(fence.pin.cpu + binding.fenceOffset);
    if (value != p.released)
        return QueueStatus::InvalidFence;
    out = value;
    return QueueStatus::Ok;
}
bool quiesce(void *opaque, const ChannelBinding &)
{
    auto &p = physical(opaque);
    p.record('Q');
    if (p.onQuiesce == Hook::LoseDevice)
        (void)p.lifecycle->markDeviceLost();
    if (p.onQuiesce == Hook::RequestStop)
        (void)p.lifecycle->requestStop();
    if (p.quiesceResult)
        p.channelStopped = true;
    return p.quiesceResult;
}
Operation releaseChannel(void *opaque, const ChannelBinding &)
{
    auto &p = physical(opaque);
    p.record('R');
    if (!p.channelStopped)
        ++p.violations; // Release before proven stop.
    if (p.releaseResult == Operation::Done) {
        p.channelClaimed = p.channelStopped = false;
        p.gpPut = p.gpGet = p.executed = 0;
    }
    return p.releaseResult;
}
QueueTransport transport(Physical &p)
{
    return {&p, claim, seal, beforePut, writePut, afterPut, currentToken, doorbell, readFence, quiesce, releaseChannel};
}
// Simulated whole-device stop owner. It covers consumers that hold no lifecycle
// lease (otherConsumerActive, e.g. scanout DMA) and only its own registry ID.
bool quiesceDevice(void *opaque, const MellowGPUCore::QuiescencePlan &plan)
{
    auto &p = physical(opaque);
    p.record('X');
    if (p.channelClaimed && !p.channelStopped)
        ++p.violations; // The adapter must stop its own channel first.
    return plan.device == Device.registryId && !p.otherConsumerActive;
}
DeviceQuiescence deviceAuthority(Physical &p) { return {&p, quiesceDevice}; }
uint8_t *resolve(Physical &p, uint64_t address, uint64_t bytes)
{
    if (address < GpuBase)
        return nullptr;
    const uint64_t slot = (address - GpuBase) / 4096, offset = (address - GpuBase) % 4096;
    if (slot >= MemoryOwner::MaxAllocations || bytes > 4096 - offset)
        return nullptr;
    return p.pages[slot] + offset;
}
// TEST DEVICE MODEL. Fetch advances GP_GET only. Execution decodes each fetched
// GPFIFO entry and performs only the CE semaphore release in its sealed tail.
void fetch(Physical &p)
{
    if (p.channelClaimed && !p.channelStopped)
        p.gpGet = p.gpPut;
}
bool execute(Physical &p, uint32_t entries)
{
    uint32_t semaphore = 0, launch = 0;
    if (Mellow::PortedNvidia::encodeMethodHeader(Mellow::PortedNvidia::ChannelClass::AmpereA,
            Mellow::PortedNvidia::MethodMode::Incrementing, 0x240, 4, 3, semaphore) != Mellow::PortedNvidia::Status::Ok ||
        Mellow::PortedNvidia::encodeMethodHeader(Mellow::PortedNvidia::ChannelClass::AmpereA,
            Mellow::PortedNvidia::MethodMode::Incrementing, 0x300, 4, 1, launch) != Mellow::PortedNvidia::Status::Ok)
        return false;
    for (; entries; --entries) {
        if (!p.channelClaimed || p.channelStopped || p.executed == p.gpGet)
            return false;
        const uint8_t *entry = p.pages[p.claimed.ring.slot] + p.claimed.ringOffset + uint64_t(p.executed) * 8;
        const uint32_t lower = le(entry), upper = le(entry + 4), words = (upper >> 10) & 0x1fffff;
        const uint8_t *pushbuffer = resolve(p, lower | uint64_t(upper & 0xff) << 32, uint64_t(words) * 4);
        if (!pushbuffer || words < 6)
            return false;
        auto tail = [&](uint32_t fromEnd) { return le(pushbuffer + uint64_t(words - fromEnd) * 4); };
        if (tail(6) != semaphore || tail(2) != launch || tail(1) != ((1U << 2) | (1U << 3)))
            return false;
        uint8_t *target = resolve(p, uint64_t(tail(5)) << 32 | tail(4), 4);
        if (!target)
            return false;
        writeLe(target, tail(3)); // The modeled GPU semaphore write.
        p.released = tail(3);
        p.executed = (p.executed + 1) % p.claimed.ringEntries;
    }
    return true;
}

struct Fixture {
    Physical physical;
    MemoryOwner memory;
    DeviceLifecycle lifecycle;
    GpFifoQueue queue;
    QueueLifecycle adapter;
    ChannelBinding binding {};
    MemoryHandle ring {}, fence {}, source {}, destination {}, commands[3] {};
    DataResource data[2] {};
    explicit Fixture(uint64_t generation = 1, MellowGPUCore::LifecycleLimits limits = {},
        uint64_t device = Device.registryId)
        : lifecycle(device, generation, limits), queue(memory, transport(physical)),
          adapter(lifecycle, queue, deviceAuthority(physical)) {}
    bool allocate(MemoryHandle &handle)
    {
        return memory.allocate(Owner, 4096, handle) == MemoryStatus::Ok &&
            memory.map(Owner, handle, true) == MemoryStatus::Ok;
    }
    bool init()
    {
        physical.lifecycle = &lifecycle;
        physical.sealer = {&memory, &physical, authority};
        if (memory.initialize(Device, {}, backend(physical)) != MemoryStatus::Ok || !allocate(ring) ||
            !allocate(fence) || !allocate(source) || !allocate(destination))
            return false;
        for (auto &handle : commands)
            if (!allocate(handle))
                return false;
        binding = {Device, Owner, 47, 73, 11, 0xC56F, 4, ring, fence, 16, 8};
        data[0] = {source, false};
        data[1] = {destination, true};
        MemoryView from {}, to {};
        if (memory.inspect(Owner, source, from) != MemoryStatus::Ok || memory.inspect(Owner, destination, to) != MemoryStatus::Ok)
            return false;
        // Client commands: the reviewed 44-byte virtual copy, before any GPU hold.
        const MellowNvidia::CopyChannelLease channel {Owner, binding.vm, Device.epoch, 0xC6B5, 4, true, true, true};
        const MellowNvidia::CopyBufferLease src {Owner, binding.vm, Device.epoch, from.handle.generation,
            from.mapping.address, from.bytes, true, true, from.mapping.writable};
        const MellowNvidia::CopyBufferLease dst {Owner, binding.vm, Device.epoch, to.handle.generation,
            to.mapping.address, to.bytes, true, true, to.mapping.writable};
        const MellowNvidia::VirtualLinearCopy copy {channel, src, dst, 32, 64, 256};
        uint32_t words[CopyInputWords] {};
        size_t count = 0;
        if (MellowNvidia::encodeVirtualLinearCopy(copy, words, CopyInputWords, count) != MellowNvidia::CopyStatus::Ok ||
            count != CopyInputWords)
            return false;
        for (auto handle : commands)
            for (uint32_t i = 0; i < CopyInputWords; ++i)
                writeLe(physical.pages[handle.slot] + uint64_t(i) * 4, words[i]);
        return true;
    }
    SubmitRequest request(uint32_t index, uint64_t deadline = 100)
    {
        return {commands[index], {0, CopyInputBytes, 128}, data, 2, deadline};
    }
    uint32_t holds(MemoryHandle handle)
    {
        MemoryView view {};
        return memory.inspect(Owner, handle, view) == MemoryStatus::Ok ? view.jobHolds : UINT32_MAX;
    }
    uint8_t *page(MemoryHandle handle) { return physical.pages[handle.slot]; }
    uint32_t fenceWord() { return le(page(fence) + binding.fenceOffset); }
    bool run(uint32_t entries)
    {
        fetch(physical);
        return execute(physical, entries);
    }
    JobState job(FenceToken token, bool &held)
    {
        JobResult result;
        held = true;
        if (queue.query(token, result) != QueueStatus::Ok)
            return JobState::Free;
        held = result.resourcesHeld;
        return result.state;
    }
    // Ends the device through the adapter, then retires memory. Ok only if
    // every lease, hold and simulated contract check is clean.
    bool finish()
    {
        physical.quiesceResult = true;
        physical.releaseResult = Operation::Done;
        physical.onQuiesce = Hook::None;
        physical.syncFails = false;
        physical.otherConsumerActive = false;
        const auto snapshot = lifecycle.snapshot();
        return adapter.close().status == S::Ok && lifecycle.snapshot().phase == DevicePhase::Stopped &&
            !lifecycle.snapshot().leases && !adapter.channelHeld() && !adapter.jobLeases() &&
            !holds(ring) && !holds(fence) && snapshot.device == Device.registryId && !physical.violations &&
            memory.quiesce() == MemoryStatus::Ok && memory.close() == MemoryStatus::Ok;
    }
};

bool published_job_retires_only_on_gpu_semaphore()
{
    Fixture f;
    REQUIRE(f.init());
    auto r = f.adapter.attach(f.binding);
    REQUIRE(r.status == S::Ok && r.queueCalled && r.queue == QueueStatus::Ok && f.physical.since(0, "C"));
    REQUIRE(f.adapter.channelHeld() && f.lifecycle.snapshot().leases == 1);
    REQUIRE(f.lifecycle.snapshot().leasesByKind[Submission] == 1);
    REQUIRE(f.holds(f.ring) == 1 && f.holds(f.fence) == 1);
    uint8_t fenceBefore[4096];
    memcpy(fenceBefore, f.page(f.fence), sizeof(fenceBefore));
    FenceToken job;
    r = f.adapter.submit(f.request(0), 1, job);
    REQUIRE(r.status == S::Ok && r.queue == QueueStatus::Ok && job.generation == 1 && job.sequence == 1);
    REQUIRE(f.physical.since(1, "SBPATD") && f.physical.rung == 0x5151);
    const auto live = f.lifecycle.snapshot();
    REQUIRE(live.leases == 2 && live.leasesByKind[Submission] == 2 && !live.leasesByKind[Publication]);
    REQUIRE(f.holds(f.commands[0]) == 1 && f.holds(f.source) == 1 && f.holds(f.destination) == 1);
    r = f.adapter.retire(job);
    REQUIRE(r.status == S::Busy && !r.queueCalled); // Record cannot outlive its lease.
    r = f.adapter.poll(0);
    REQUIRE(r.status == S::Rejected && r.queue == QueueStatus::ClockRegression && f.lifecycle.snapshot().admissionOpen);
    // GP_GET == GP_PUT and interrupt-driven polling are fetch progress only.
    fetch(f.physical);
    REQUIRE(f.physical.gpGet == f.physical.gpPut && f.physical.gpPut == 1);
    for (uint64_t now = 2; now < 4; ++now) {
        r = f.adapter.poll(now);
        REQUIRE(r.status == S::Ok && r.queueCalled && f.lifecycle.snapshot().leases == 2);
    }
    bool held = false;
    REQUIRE(f.job(job, held) == JobState::Submitted && held && f.holds(f.commands[0]) == 1);
    REQUIRE(!memcmp(fenceBefore, f.page(f.fence), sizeof(fenceBefore))); // No CPU semaphore store.
    REQUIRE(f.run(1) && f.fenceWord() == job.sequence);
    r = f.adapter.poll(4);
    REQUIRE(r.status == S::Ok && f.lifecycle.snapshot().leases == 1 && !f.adapter.jobLeases());
    REQUIRE(f.job(job, held) == JobState::Completed && !held);
    REQUIRE(!f.holds(f.commands[0]) && !f.holds(f.source) && !f.holds(f.destination));
    REQUIRE(f.adapter.channelHeld() && f.holds(f.ring) == 1 && f.holds(f.fence) == 1);
    REQUIRE(f.adapter.retire(job).status == S::Ok);
    // Job completion did not release the channel: it still holds the device.
    f.physical.quiesceResult = false;
    uint32_t mark = f.physical.length;
    r = f.adapter.close();
    REQUIRE(r.status == S::QuiescenceUnknown && r.queue == QueueStatus::QuiesceFailed && f.physical.since(mark, "Q"));
    REQUIRE(r.lifecycle == LifecycleStatus::UnknownFlightRetained);
    auto s = f.lifecycle.snapshot();
    REQUIRE(s.phase == DevicePhase::Quiescing && s.quiescenceActive && s.leases == 1 && !s.admissionOpen);
    REQUIRE(f.holds(f.ring) == 1 && f.holds(f.fence) == 1);
    f.physical.quiesceResult = true;
    mark = f.physical.length;
    r = f.adapter.close();
    REQUIRE(r.status == S::Ok && f.physical.since(mark, "QRX"));
    s = f.lifecycle.snapshot();
    REQUIRE(s.phase == DevicePhase::Stopped && !s.leases && !f.adapter.channelHeld());
    REQUIRE(!f.holds(f.ring) && !f.holds(f.fence));
    mark = f.physical.length;
    r = f.adapter.close();
    REQUIRE(r.status == S::Ok && !r.queueCalled && f.physical.since(mark, ""));
    REQUIRE(f.adapter.attach(f.binding).status == S::AdmissionClosed && f.physical.since(mark, ""));
    return f.finish();
}

bool rejected_put_differs_from_ambiguous_publication()
{
    {
        Fixture f;
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        f.physical.putResult = Operation::Rejected;
        FenceToken token {7, 7};
        auto r = f.adapter.submit(f.request(0), 1, token);
        REQUIRE(r.status == S::Rejected && r.queue == QueueStatus::TransportFailure && !token.generation && !token.sequence);
        REQUIRE(f.physical.since(1, "SBP"));
        const auto s = f.lifecycle.snapshot();
        REQUIRE(s.leases == 1 && !s.unknownLeases && s.admissionOpen && s.phase == DevicePhase::Running);
        REQUIRE(!f.holds(f.commands[0]) && !f.holds(f.source) && !f.holds(f.destination));
        f.physical.putResult = Operation::Done;
        r = f.adapter.submit(f.request(0), 2, token);
        REQUIRE(r.status == S::Ok && token.sequence == 1 && f.lifecycle.snapshot().leases == 2);
        REQUIRE(f.finish());
    }
    const char *const expected[] {"SBP", "SBPA", "SBPAT", "SBPATD", "SBPATD"};
    for (uint32_t stage = 0; stage < 5; ++stage) {
        Fixture f;
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        if (stage == 0) f.physical.putResult = Operation::Unknown;
        if (stage == 1) f.physical.afterResult = QueueStatus::TransportFailure;
        if (stage == 2) f.physical.tokenResult = QueueStatus::TransportFailure;
        if (stage == 3) f.physical.bellResult = Operation::Rejected; // GP_PUT already exposed work.
        if (stage == 4) f.physical.bellResult = Operation::Unknown;
        FenceToken token;
        auto r = f.adapter.submit(f.request(0), 1, token);
        REQUIRE(r.status == S::PublicationUnknown && r.queue == QueueStatus::PublicationUnknown);
        REQUIRE(token.generation == 1 && token.sequence == 1 && f.physical.since(1, expected[stage]));
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.leases == 2 && s.unknownLeases == 2 && !s.admissionOpen && s.phase == DevicePhase::ResetPending);
        REQUIRE(s.generation == 1 && f.adapter.jobLeases() == 1);
        bool held = false;
        REQUIRE(f.job(token, held) == JobState::PublicationUnknown && held);
        REQUIRE(f.holds(f.commands[0]) == 1 && f.holds(f.source) == 1 && f.holds(f.destination) == 1);
        const uint32_t mark = f.physical.length;
        FenceToken refused {5, 5};
        r = f.adapter.submit(f.request(1), 2, refused);
        REQUIRE(r.status == S::AdmissionClosed && !r.queueCalled && !refused.generation && f.physical.since(mark, ""));
        REQUIRE(f.adapter.retire(token).status == S::Busy && f.physical.since(mark, ""));
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && r.queue == QueueStatus::Ok && f.physical.since(mark, "QRX"));
        s = f.lifecycle.snapshot();
        REQUIRE(s.generation == 2 && s.admissionOpen && !s.leases && s.phase == DevicePhase::Running);
        REQUIRE(f.job(token, held) == JobState::Reset && !held); // Reset, never Completed.
        REQUIRE(!f.holds(f.commands[0]) && !f.holds(f.source) && !f.holds(f.ring) && !f.holds(f.fence));
        REQUIRE(f.adapter.retire(token).status == S::Ok);
        REQUIRE(f.finish());
    }
    return true;
}

bool timeout_is_neither_completion_nor_quiescence()
{
    {
        Fixture f;
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        FenceToken job;
        REQUIRE(f.adapter.submit(f.request(0, 100), 1, job).status == S::Ok);
        REQUIRE(f.adapter.expire(50).status == S::Ok && f.lifecycle.snapshot().admissionOpen);
        auto r = f.adapter.expire(100);
        REQUIRE(r.status == S::TimedOut && r.queue == QueueStatus::TimedOut);
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.leases == 2 && s.unknownLeases == 2 && s.phase == DevicePhase::ResetPending && s.generation == 1);
        bool held = false;
        REQUIRE(f.job(job, held) == JobState::TimedOut && held && f.holds(f.commands[0]) == 1);
        REQUIRE(f.adapter.poll(101).status == S::Ok && f.lifecycle.snapshot().leases == 2);
        // A late GPU release after the deadline is still authoritative completion.
        REQUIRE(f.run(1));
        REQUIRE(f.adapter.poll(102).status == S::Ok && f.lifecycle.snapshot().leases == 1);
        REQUIRE(f.job(job, held) == JobState::Completed && !held && !f.holds(f.commands[0]));
        // The deadline never stopped the channel: recovery needs proven quiescence.
        f.physical.quiesceResult = false;
        const uint32_t mark = f.physical.length;
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            r = f.adapter.recover(2);
            REQUIRE(r.status == S::QuiescenceUnknown && r.lifecycle == LifecycleStatus::UnknownFlightRetained);
            s = f.lifecycle.snapshot();
            REQUIRE(s.quiescenceActive && s.generation == 1 && s.leases == 1 && f.holds(f.ring) == 1);
        }
        f.physical.quiesceResult = true;
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && f.physical.since(mark, "QQQRX") && f.lifecycle.snapshot().generation == 2);
        REQUIRE(f.job(job, held) == JobState::Completed && !held); // Channel release did not rewrite it.
        REQUIRE(f.finish());
    }
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken job;
    REQUIRE(f.adapter.submit(f.request(0, 100), 1, job).status == S::Ok);
    REQUIRE(f.adapter.expire(100).status == S::TimedOut);
    REQUIRE(f.adapter.recover(5).status == S::Ok);
    bool held = false;
    REQUIRE(f.job(job, held) == JobState::Reset && !held && f.lifecycle.snapshot().generation == 5);
    return f.finish();
}

bool forged_cpu_semaphore_is_not_completion()
{
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken job;
    REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
    writeLe(f.page(f.fence) + f.binding.fenceOffset, job.sequence); // Forbidden CPU store.
    auto r = f.adapter.poll(2);
    REQUIRE(r.status == S::QueueFault && r.queue == QueueStatus::InvalidFence);
    const auto s = f.lifecycle.snapshot();
    REQUIRE(s.leases == 2 && s.unknownLeases == 2 && s.phase == DevicePhase::ResetPending);
    bool held = false;
    REQUIRE(f.job(job, held) == JobState::Submitted && held && f.holds(f.commands[0]) == 1);
    REQUIRE(f.adapter.recover(2).status == S::Ok && !f.lifecycle.snapshot().leases);
    REQUIRE(f.job(job, held) == JobState::Reset && !held);
    return f.finish();
}

bool incomplete_cleanup_keeps_the_device_held()
{
    {
        Fixture f; // Channel release Unknown after a proven stop.
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        FenceToken job;
        REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
        f.physical.releaseResult = Operation::Unknown;
        uint32_t mark = f.physical.length;
        auto r = f.adapter.recover(2);
        REQUIRE(r.status == S::CleanupPending && r.queue == QueueStatus::TransportFailure && f.physical.since(mark, "QRX"));
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.generation == 1 && !s.admissionOpen && s.nativeQuiescenceAcknowledged && s.leases == 1);
        REQUIRE(s.phase == DevicePhase::QuiescentRetained && f.adapter.channelHeld() && !f.adapter.jobLeases());
        bool held = false;
        REQUIRE(f.job(job, held) == JobState::Reset && !held && !f.holds(f.commands[0]));
        REQUIRE(f.holds(f.ring) == 1 && f.holds(f.fence) == 1);
        f.physical.releaseResult = Operation::Done;
        mark = f.physical.length;
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && f.physical.since(mark, "R")); // Cleanup only; no second stop.
        s = f.lifecycle.snapshot();
        REQUIRE(s.generation == 2 && !s.leases && !f.holds(f.ring) && !f.holds(f.fence));
        REQUIRE(f.finish());
    }
    {
        Fixture f; // Unknown claim: channel may be owned; ring/fence stay held.
        REQUIRE(f.init());
        f.physical.claimResult = Operation::Unknown;
        auto r = f.adapter.attach(f.binding);
        REQUIRE(r.status == S::PublicationUnknown && r.queue == QueueStatus::PublicationUnknown);
        auto s = f.lifecycle.snapshot();
        REQUIRE(f.adapter.channelHeld() && s.leases == 1 && s.unknownLeases == 1 && s.phase == DevicePhase::ResetPending);
        REQUIRE(f.holds(f.ring) == 1 && f.holds(f.fence) == 1);
        uint32_t mark = f.physical.length;
        r = f.adapter.poll(1);
        REQUIRE(r.status == S::Busy && f.physical.since(mark, "")); // No fence read on unverified claim.
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && f.physical.since(mark, "QRX") && !f.lifecycle.snapshot().leases);
        REQUIRE(!f.holds(f.ring) && !f.holds(f.fence));
        f.physical.claimResult = Operation::Done;
        mark = f.physical.length;
        // An unknown claim retained backing, so it fixes identity even though
        // bind did not return Ok. A different channel cannot erase its history.
        ChannelBinding other = f.binding;
        ++other.channel;
        ++other.channelGeneration;
        auto refused = f.adapter.attach(other);
        REQUIRE(refused.status == S::BindingIdentityMismatch && !refused.queueCalled && f.physical.since(mark, ""));
        ChannelBinding older = f.binding;
        --older.channelGeneration;
        REQUIRE(f.adapter.attach(f.binding).status == S::StaleBinding && f.physical.since(mark, ""));
        REQUIRE(f.adapter.attach(older).status == S::StaleBinding && f.physical.since(mark, ""));
        ChannelBinding newer = f.binding;
        ++newer.channelGeneration;
        REQUIRE(f.adapter.attach(newer).status == S::Ok && f.physical.since(mark, "C"));
        REQUIRE(f.finish());
    }
    {
        Fixture f; // Rejected claim proves nothing was acquired.
        REQUIRE(f.init());
        f.physical.claimResult = Operation::Rejected;
        auto r = f.adapter.attach(f.binding);
        REQUIRE(r.status == S::Rejected && r.queue == QueueStatus::TransportFailure && !f.adapter.channelHeld());
        const auto s = f.lifecycle.snapshot();
        REQUIRE(!s.leases && s.admissionOpen && s.phase == DevicePhase::Running);
        REQUIRE(!f.holds(f.ring) && !f.holds(f.fence));
        f.physical.claimResult = Operation::Done;
        // A clean refusal owns no channel/backing and must not permanently
        // bind the adapter to the refused identity.
        ChannelBinding other = f.binding;
        ++other.channel;
        REQUIRE(f.adapter.attach(other).status == S::Ok);
        REQUIRE(f.finish());
    }
    Fixture f; // Failed rollback: no token, so only reset() Ok proves release.
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    f.physical.syncFails = true;
    FenceToken token {3, 3};
    auto r = f.adapter.submit(f.request(0), 1, token);
    REQUIRE(r.status == S::QueueFault && r.queue == QueueStatus::MemoryFailure && !token.generation && !token.sequence);
    REQUIRE(f.adapter.jobLeases() == 1 && f.lifecycle.snapshot().leases == 2 && f.memory.draining());
    REQUIRE(f.lifecycle.snapshot().phase == DevicePhase::ResetPending);
    // This rollback did drop its holds, but a failed hold release returns the
    // same status with no record or token; the adapter cannot tell them apart.
    REQUIRE(!f.holds(f.commands[0]) && !f.queue.pending());
    f.physical.syncFails = false;
    r = f.adapter.poll(2);
    REQUIRE(r.status == S::QueueFault && r.queue == QueueStatus::MemoryFailure && f.adapter.jobLeases() == 1);
    f.physical.releaseResult = Operation::Unknown;
    uint32_t mark = f.physical.length;
    r = f.adapter.recover(2);
    REQUIRE(r.status == S::CleanupPending && r.queue == QueueStatus::TransportFailure && f.physical.since(mark, "QRX"));
    REQUIRE(f.adapter.jobLeases() == 1 && f.adapter.channelHeld() && f.lifecycle.snapshot().leases == 2);
    f.physical.releaseResult = Operation::Done;
    mark = f.physical.length;
    r = f.adapter.recover(2);
    REQUIRE(r.status == S::Ok && f.physical.since(mark, "R"));
    REQUIRE(!f.adapter.jobLeases() && !f.lifecycle.snapshot().leases && f.lifecycle.snapshot().generation == 2);
    return f.finish();
}

bool completed_job_with_held_memory_keeps_its_lease()
{
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken job;
    REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
    // Fault injection on the real owner: an errant holder drops one hold early,
    // so the queue's ordered release (destination, source, commands) stops midway.
    REQUIRE(f.memory.release(Owner, f.source) == MemoryStatus::Ok);
    REQUIRE(f.run(1) && f.fenceWord() == job.sequence);
    auto r = f.adapter.poll(2);
    REQUIRE(r.status == S::QueueFault && r.queue == QueueStatus::MemoryFailure);
    bool held = false;
    REQUIRE(f.job(job, held) == JobState::Completed && held); // Completed is not release.
    REQUIRE(f.holds(f.commands[0]) == 1 && !f.holds(f.source) && !f.holds(f.destination));
    auto s = f.lifecycle.snapshot();
    REQUIRE(f.adapter.jobLeases() == 1 && s.leases == 2 && s.unknownLeases == 2 && s.phase == DevicePhase::ResetPending);
    r = f.adapter.poll(3); // The authoritative fence is unchanged; holds still remain.
    REQUIRE(r.status == S::Ok && f.adapter.jobLeases() == 1 && f.lifecycle.snapshot().leases == 2);
    REQUIRE(f.adapter.retire(job).status == S::Busy);
    REQUIRE(f.memory.retain(Owner, f.source) == MemoryStatus::Ok); // Restore the owner invariant.
    const uint32_t mark = f.physical.length;
    r = f.adapter.recover(2);
    REQUIRE(r.status == S::Ok && f.physical.since(mark, "QRX") && !f.lifecycle.snapshot().leases);
    REQUIRE(f.job(job, held) == JobState::Completed && !held && !f.holds(f.commands[0]) && !f.holds(f.source));
    REQUIRE(f.adapter.retire(job).status == S::Ok);
    return f.finish();
}

bool channel_quiesce_is_not_device_quiescence()
{
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken job;
    REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
    f.physical.otherConsumerActive = true; // A device consumer that holds no lease.
    uint32_t mark = f.physical.length;
    auto r = f.adapter.close();
    REQUIRE(r.status == S::QuiescenceUnknown && r.queue == QueueStatus::Ok);
    REQUIRE(r.lifecycle == LifecycleStatus::UnknownFlightRetained && f.physical.since(mark, "QRX"));
    // This channel is proven stopped and released, so its own leases retired,
    // but the device barrier is not acknowledged and nothing reopens.
    auto s = f.lifecycle.snapshot();
    REQUIRE(s.phase == DevicePhase::Quiescing && s.quiescenceActive && !s.nativeQuiescenceAcknowledged && !s.leases);
    REQUIRE(!f.adapter.channelHeld() && !f.adapter.jobLeases() && !f.holds(f.ring) && !f.holds(f.fence));
    bool held = false;
    REQUIRE(f.job(job, held) == JobState::Reset && !held);
    mark = f.physical.length;
    REQUIRE(f.adapter.close().status == S::QuiescenceUnknown && f.physical.since(mark, "X"));
    f.physical.otherConsumerActive = false;
    mark = f.physical.length;
    r = f.adapter.close();
    REQUIRE(r.status == S::Ok && f.physical.since(mark, "X"));
    s = f.lifecycle.snapshot();
    REQUIRE(s.phase == DevicePhase::Stopped && s.generation == 1 && !s.leases);
    REQUIRE(f.adapter.retire(job).status == S::Ok);
    return f.finish();
}

bool lifecycle_device_is_the_bound_physical_device()
{
    {
        Fixture f(1, {}, OtherDevice); // Lifecycle of a different device instance.
        REQUIRE(f.init());
        auto r = f.adapter.attach(f.binding);
        REQUIRE(r.status == S::WrongDevice && r.lifecycle == LifecycleStatus::WrongDevice && !r.queueCalled);
        REQUIRE(f.physical.since(0, "") && !f.lifecycle.snapshot().leases && !f.adapter.channelHeld());
        REQUIRE(!f.holds(f.ring) && !f.holds(f.fence) && f.queue.state() == QueueState::Detached);
        // Naming the lifecycle's device in the binding does not pass either: the
        // queue checks the MemoryOwner's admitted ring/fence identity before claim.
        ChannelBinding forged = f.binding;
        forged.device.registryId = OtherDevice;
        r = f.adapter.attach(forged);
        REQUIRE(r.status == S::Rejected && r.queueCalled && r.queue == QueueStatus::Ownership);
        REQUIRE(f.physical.since(0, "") && !f.lifecycle.snapshot().leases && !f.holds(f.ring));
        // The physical owner attests stops only for its own registry ID.
        r = f.adapter.close();
        REQUIRE(r.status == S::QuiescenceUnknown && f.physical.since(0, "X"));
        REQUIRE(f.lifecycle.snapshot().quiescenceActive && !f.lifecycle.snapshot().nativeQuiescenceAcknowledged);
        REQUIRE(!f.physical.violations && f.memory.quiesce() == MemoryStatus::Ok && f.memory.close() == MemoryStatus::Ok);
    }
    Fixture f;
    REQUIRE(f.init());
    ChannelBinding stale = f.binding;
    ++stale.device.epoch; // Same registry ID, not the admitted memory epoch.
    auto r = f.adapter.attach(stale);
    REQUIRE(r.status == S::Rejected && r.queue == QueueStatus::Ownership && f.physical.since(0, ""));
    REQUIRE(!f.lifecycle.snapshot().leases && f.lifecycle.snapshot().admissionOpen);
    QueueLifecycle incomplete(f.lifecycle, f.queue, {}); // No whole-device authority.
    r = incomplete.attach(f.binding);
    REQUIRE(r.status == S::Unavailable && !r.queueCalled && f.physical.since(0, ""));
    r = incomplete.recover(2);
    REQUIRE(r.status == S::Unavailable && !r.queueCalled && f.lifecycle.snapshot().admissionOpen);
    REQUIRE(!f.lifecycle.snapshot().leases && f.lifecycle.snapshot().generation == 1);
    REQUIRE(f.adapter.attach(f.binding).status == S::Ok && f.physical.since(0, "C"));
    return f.finish();
}

bool reentrant_loss_and_stop_supersede_the_boundary()
{
    {
        Fixture f; // Loss notification while GP_PUT is being published.
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        f.physical.onPut = Hook::LoseDevice;
        FenceToken job;
        auto r = f.adapter.submit(f.request(0), 1, job);
        REQUIRE(r.status == S::Ok && job.sequence == 1);
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.phase == DevicePhase::Lost && !s.admissionOpen && s.leases == 2 && s.unknownLeases == 2);
        REQUIRE(!s.leasesByKind[Publication]);
        f.physical.onPut = Hook::None;
        const uint32_t mark = f.physical.length;
        FenceToken refused;
        REQUIRE(f.adapter.submit(f.request(1), 2, refused).status == S::AdmissionClosed && f.physical.since(mark, ""));
        REQUIRE(f.adapter.recover(2).status == S::Ok && f.physical.since(mark, "QRX"));
        bool held = false;
        REQUIRE(f.job(job, held) == JobState::Reset && !held && f.lifecycle.snapshot().generation == 2);
        REQUIRE(f.finish());
    }
    {
        Fixture f; // Loss notification while the channel stop is in progress.
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        FenceToken job;
        REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
        f.physical.onQuiesce = Hook::LoseDevice;
        auto r = f.adapter.recover(2);
        REQUIRE(r.status == S::Superseded && r.queue == QueueStatus::Ok && r.lifecycle == LifecycleStatus::Superseded);
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.generation == 1 && !s.admissionOpen && !s.quiescenceActive && !s.nativeQuiescenceAcknowledged);
        REQUIRE(!s.leases && !f.adapter.channelHeld()); // Released natively; barrier not acknowledged.
        f.physical.onQuiesce = Hook::None;
        const uint32_t mark = f.physical.length;
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && f.physical.since(mark, "X")); // Fresh device barrier; channel released.
        REQUIRE(f.lifecycle.snapshot().generation == 2 && f.lifecycle.snapshot().admissionOpen);
        REQUIRE(f.finish());
    }
    Fixture f; // Stop escalation during a reset stop: stop dominates.
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken job;
    REQUIRE(f.adapter.submit(f.request(0), 1, job).status == S::Ok);
    f.physical.onQuiesce = Hook::RequestStop;
    REQUIRE(f.adapter.recover(2).status == S::Superseded);
    f.physical.onQuiesce = Hook::None;
    const uint32_t mark = f.physical.length;
    auto r = f.adapter.recover(2);
    REQUIRE(r.status == S::AdmissionClosed && !r.queueCalled && f.physical.since(mark, ""));
    REQUIRE(f.adapter.close().status == S::Ok && f.lifecycle.snapshot().phase == DevicePhase::Stopped);
    REQUIRE(f.lifecycle.snapshot().generation == 1 && f.physical.since(mark, "X"));
    ChannelBinding newer = f.binding;
    ++newer.channelGeneration;
    REQUIRE(f.adapter.attach(newer).status == S::AdmissionClosed && f.physical.since(mark, "X"));
    return f.finish();
}

bool independent_identities_and_generations()
{
    Fixture f(7);
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken first;
    REQUIRE(f.adapter.submit(f.request(0), 1, first).status == S::Ok);
    REQUIRE(first.generation == 1 && f.lifecycle.snapshot().generation == 7);
    REQUIRE(f.binding.channelGeneration == 11 && f.memory.device().epoch == 9);
    uint32_t mark = f.physical.length;
    for (uint64_t invalid : {uint64_t(0), uint64_t(7)}) {
        const auto r = f.adapter.recover(invalid);
        REQUIRE(r.status == S::Policy && r.lifecycle == LifecycleStatus::InvalidGeneration && !r.queueCalled);
        REQUIRE(f.lifecycle.snapshot().admissionOpen && f.physical.since(mark, ""));
    }
    REQUIRE(f.adapter.recover(8).status == S::Ok && f.physical.since(mark, "QRX"));
    mark = f.physical.length;
    REQUIRE(f.adapter.attach(f.binding).status == S::StaleBinding && f.physical.since(mark, ""));
    ChannelBinding newer = f.binding;
    ++newer.channelGeneration;
    REQUIRE(f.adapter.attach(newer).status == S::Ok && f.physical.since(mark, "C"));
    FenceToken second;
    REQUIRE(f.adapter.submit(f.request(1), 2, second).status == S::Ok);
    REQUIRE(second.generation == 2 && second.sequence == first.sequence);
    REQUIRE(f.lifecycle.snapshot().generation == 8 && f.memory.device().epoch == 9);
    bool held = false;
    REQUIRE(f.job(first, held) == JobState::Reset && !held && f.job(second, held) == JobState::Submitted && held);
    REQUIRE(f.adapter.retire(first).status == S::Ok);
    const auto r = f.adapter.retire(first);
    REQUIRE(r.status == S::Rejected && r.queue == QueueStatus::StaleToken);
    return f.finish();
}

bool foreign_obligations_stay_owned_through_the_stop()
{
    {
        Fixture f;
        REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
        FenceToken job;
        REQUIRE(f.adapter.submit(f.request(1), 1, job).status == S::Ok);
        LeaseToken display;
        REQUIRE(f.lifecycle.acquire(LeaseKind::Display, &display) == LifecycleStatus::Success);
        // A display obligation stays owned while the stop runs; waiting for it
        // first would deadlock recovery. Only the final transition waits for it.
        uint32_t mark = f.physical.length;
        auto r = f.adapter.recover(2);
        REQUIRE(r.status == S::ForeignObligations && r.queue == QueueStatus::Ok && r.lifecycle == LifecycleStatus::Busy);
        REQUIRE(f.physical.since(mark, "QRX") && !f.adapter.channelHeld() && !f.adapter.jobLeases());
        auto s = f.lifecycle.snapshot();
        REQUIRE(s.phase == DevicePhase::QuiescentRetained && s.nativeQuiescenceAcknowledged && s.generation == 1);
        REQUIRE(s.leases == 1 && s.leasesByKind[uint32_t(LeaseKind::Display)] == 1 && !s.admissionOpen);
        mark = f.physical.length;
        REQUIRE(f.adapter.recover(2).status == S::ForeignObligations && f.physical.since(mark, ""));
        REQUIRE(f.lifecycle.release(display, Retirement::KnownRetired) == LifecycleStatus::Success);
        r = f.adapter.recover(2);
        REQUIRE(r.status == S::Ok && f.physical.since(mark, "") && f.lifecycle.snapshot().generation == 2);
        ChannelBinding newer = f.binding;
        ++newer.channelGeneration;
        REQUIRE(f.adapter.attach(newer).status == S::Ok);
        // A foreign in-flight native call refuses the plan before any native call.
        LeaseToken operation;
        REQUIRE(f.lifecycle.acquire(LeaseKind::NativeOperation, &operation) == LifecycleStatus::Success);
        mark = f.physical.length;
        r = f.adapter.recover(3);
        REQUIRE(r.status == S::Busy && r.lifecycle == LifecycleStatus::Busy && !r.queueCalled && f.physical.since(mark, ""));
        REQUIRE(f.adapter.channelHeld() && !f.lifecycle.snapshot().quiescenceActive && !f.lifecycle.snapshot().admissionOpen);
        REQUIRE(f.lifecycle.release(operation, Retirement::KnownRetired) == LifecycleStatus::Success);
        REQUIRE(f.adapter.recover(3).status == S::Ok && f.physical.since(mark, "QRX"));
        ++newer.channelGeneration;
        REQUIRE(f.adapter.attach(newer).status == S::Ok);
        LeaseToken foreign[62];
        for (auto &lease : foreign)
            REQUIRE(f.lifecycle.acquire(LeaseKind::Pin, &lease) == LifecycleStatus::Success);
        mark = f.physical.length;
        FenceToken token;
        r = f.adapter.submit(f.request(0), 1, token);
        REQUIRE(r.status == S::Policy && r.lifecycle == LifecycleStatus::NoSlot && !r.queueCalled);
        REQUIRE(f.physical.since(mark, "") && f.lifecycle.snapshot().leases == 63 && !f.holds(f.commands[0]));
        for (const auto &lease : foreign)
            REQUIRE(f.lifecycle.release(lease, Retirement::KnownRetired) == LifecycleStatus::Success);
        REQUIRE(f.adapter.submit(f.request(0), 1, token).status == S::Ok && token.sequence == 1);
        REQUIRE(f.finish());
    }
    Fixture f(1, {3, UINT64_MAX}); // Serial budget: attach takes 2, submit needs 2.
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    const uint32_t mark = f.physical.length;
    FenceToken token;
    const auto r = f.adapter.submit(f.request(0), 1, token);
    REQUIRE(r.status == S::Policy && r.lifecycle == LifecycleStatus::CounterExhausted && !r.queueCalled);
    REQUIRE(f.physical.since(mark, "") && f.lifecycle.snapshot().leases == 1 && !f.holds(f.commands[0]));
    return f.finish();
}

// Switching A -> B -> A must not evict A's released generation. Each adapter
// belongs to the first retained identity; a different identity needs a new owner.
bool channel_identity_cannot_evict_released_generation()
{
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    REQUIRE(f.adapter.recover(2).status == S::Ok);
    const uint32_t mark = f.physical.length;
    const auto before = f.lifecycle.snapshot();
    for (unsigned field = 0; field < 6; ++field) {
        ChannelBinding other = f.binding;
        // The first case is the actual A -> B eviction counterexample.
        if (field == 0) ++other.channel;
        if (field == 1) ++other.owner;
        if (field == 2) ++other.vm;
        if (field == 3) ++other.device.epoch;
        if (field == 4) ++other.device.device;
        if (field == 5) other.device.vendor = 0x8086;
        ++other.channelGeneration;
        const auto rejected = f.adapter.attach(other);
        REQUIRE(rejected.status == S::BindingIdentityMismatch && !rejected.queueCalled);
        REQUIRE(f.physical.since(mark, "") && f.queue.state() == QueueState::Detached);
        const auto after = f.lifecycle.snapshot();
        REQUIRE(after.leases == before.leases && after.generation == before.generation && after.admissionOpen);
        REQUIRE(!f.adapter.channelHeld() && !f.adapter.jobLeases());
        // Every refused replacement leaves the original high-water mark intact.
        REQUIRE(f.adapter.attach(f.binding).status == S::StaleBinding && f.physical.since(mark, ""));
    }
    ChannelBinding foreignDevice = f.binding;
    ++foreignDevice.device.registryId;
    const auto wrongDevice = f.adapter.attach(foreignDevice);
    REQUIRE(wrongDevice.status == S::WrongDevice && !wrongDevice.queueCalled && f.physical.since(mark, ""));
    ChannelBinding next = f.binding;
    ++next.channelGeneration;
    REQUIRE(f.adapter.attach(next).status == S::Ok && f.physical.since(mark, "C"));
    REQUIRE(f.adapter.recover(3).status == S::Ok);
    const uint32_t finalMark = f.physical.length;
    REQUIRE(f.adapter.attach(f.binding).status == S::StaleBinding);
    REQUIRE(f.adapter.attach(next).status == S::StaleBinding && f.physical.since(finalMark, ""));
    ++next.channelGeneration;
    REQUIRE(f.adapter.attach(next).status == S::Ok && f.physical.since(finalMark, "C"));
    return f.finish();
}

bool draining_stop_keeps_completion_distinct_from_release()
{
    Fixture f;
    REQUIRE(f.init() && f.adapter.attach(f.binding).status == S::Ok);
    FenceToken jobs[2];
    REQUIRE(f.adapter.submit(f.request(0), 1, jobs[0]).status == S::Ok);
    REQUIRE(f.adapter.submit(f.request(1), 2, jobs[1]).status == S::Ok && f.queue.pending() == 2);
    REQUIRE(f.lifecycle.requestStop() == LifecycleStatus::Success); // Owner begins termination.
    uint32_t mark = f.physical.length;
    FenceToken refused;
    REQUIRE(f.adapter.submit(f.request(2), 3, refused).status == S::AdmissionClosed && f.physical.since(mark, ""));
    REQUIRE(f.run(2) && f.fenceWord() == jobs[1].sequence);
    REQUIRE(f.adapter.poll(4).status == S::Ok && f.physical.since(mark, "F"));
    REQUIRE(!f.adapter.jobLeases() && f.lifecycle.snapshot().leases == 1 && f.adapter.channelHeld());
    REQUIRE(!f.holds(f.commands[0]) && !f.holds(f.commands[1]) && !f.holds(f.source));
    mark = f.physical.length;
    REQUIRE(f.adapter.close().status == S::Ok && f.physical.since(mark, "QRX"));
    bool held = false;
    for (auto job : jobs) {
        REQUIRE(f.job(job, held) == JobState::Completed && !held); // Completed, not Reset.
        REQUIRE(f.adapter.retire(job).status == S::Ok);
    }
    return f.finish();
}
} // namespace

int main()
{
    struct Test { const char *name; bool (*run)(); };
    const Test tests[] {
        {"published job retires only on its GPU semaphore", published_job_retires_only_on_gpu_semaphore},
        {"rejected PUT differs from ambiguous publication", rejected_put_differs_from_ambiguous_publication},
        {"timeout is neither completion nor quiescence", timeout_is_neither_completion_nor_quiescence},
        {"forged CPU semaphore is not completion", forged_cpu_semaphore_is_not_completion},
        {"completed job with held memory keeps its lease", completed_job_with_held_memory_keeps_its_lease},
        {"incomplete cleanup keeps the device held", incomplete_cleanup_keeps_the_device_held},
        {"channel quiesce is not device quiescence", channel_quiesce_is_not_device_quiescence},
        {"lifecycle device is the bound physical device", lifecycle_device_is_the_bound_physical_device},
        {"reentrant loss and stop supersede the boundary", reentrant_loss_and_stop_supersede_the_boundary},
        {"identities and generations stay independent", independent_identities_and_generations},
        {"foreign obligations stay owned through the stop", foreign_obligations_stay_owned_through_the_stop},
        {"channel identity cannot evict released generation", channel_identity_cannot_evict_released_generation},
        {"draining stop keeps completion distinct from release", draining_stop_keeps_completion_distinct_from_release}};
    unsigned failed = 0;
    for (const auto &test : tests) {
        const bool passed = test.run();
        failed += !passed;
        printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
    }
    if (failed) {
        printf("FAIL %u of %zu NVIDIA queue lifecycle groups\n", failed, sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    printf("PASS %zu NVIDIA queue lifecycle groups; simulated transport, no GPU or hardware qualification\n",
        sizeof(tests) / sizeof(tests[0]));
    return 0;
}

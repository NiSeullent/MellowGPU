// Host regression evidence only: adapters below simulate device operations.
// No IOKit, firmware, physical GPU or Metal execution is performed.
#include "../Drivers/NativeNvidia/GpFifoQueue.hpp"
#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

using namespace MellowNative;
using namespace MellowNativeNvidia;
namespace {
constexpr uint64_t Owner = 31;
const DeviceIdentity Device {0x10de, 0x2206, 0x88776655, 9};
struct Mock {
    alignas(4096) uint8_t pages[MemoryOwner::MaxAllocations][4096] {};
    uint64_t dma[MemoryOwner::MaxAllocations] {};
    uint32_t mapCookies[MemoryOwner::MaxAllocations] {};
    std::vector<std::string> events;
    uint64_t gpuBase {0x01000000};
    Operation claimResult {Operation::Done}, putResult {Operation::Done}, bellResult {Operation::Done};
    Operation releaseResult {Operation::Done};
    QueueStatus sealResult {QueueStatus::Ok}, beforeResult {QueueStatus::Ok}, afterResult {QueueStatus::Ok};
    QueueStatus tokenResult {QueueStatus::Ok}, fenceResult {QueueStatus::Ok};
    ChannelMode mode {ChannelMode::Ordinary};
    RingDomain domain {RingDomain::CoherentSystem};
    bool live {true}, stop {true}, wrongIdentity {}, incomplete {}, syncFails {};
    MemoryOwner *faultMemory {};
    bool dropCommandHoldBeforeReject {};
    uint32_t initialPut {}, baseline {}, completed {}, hardwareGet {}, workToken {0x12345678}, rungToken {}, puts {};
    uint64_t sealedBytes {64}, sealedFenceAddress {};
    uint32_t sealedPayload {};
};
bool admitted(void *context, const DeviceIdentity &device)
{
    const auto &mock = *static_cast<Mock *>(context);
    return mock.live && device.vendor == Device.vendor && device.device == Device.device &&
        device.registryId == Device.registryId && device.epoch == Device.epoch;
}
MemoryStatus pin(void *context, const DeviceIdentity &device, uint64_t, const MemoryHandle &handle,
    uint64_t bytes, DmaPin &out)
{
    auto &mock = *static_cast<Mock *>(context);
    assert(bytes == 4096);
    mock.dma[handle.slot] = 0x100000 + uint64_t(handle.slot) * 4096;
    out = {mock.pages[handle.slot], mock.pages[handle.slot], &mock.dma[handle.slot], 1, bytes, device.registryId};
    return MemoryStatus::Ok;
}
MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, DmaPin &)
{
    return MemoryStatus::Ok;
}
MemoryStatus sync(void *context, const DeviceIdentity &, uint64_t, const MemoryHandle &handle,
    const DmaPin &, CacheDirection direction)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.push_back(std::string(direction == CacheDirection::ForDevice ? "device:" : "cpu:") +
        std::to_string(handle.slot));
    return mock.syncFails ? MemoryStatus::IoFailure : MemoryStatus::Ok;
}
MemoryStatus map(void *context, const DeviceIdentity &, uint64_t, const MemoryHandle &handle,
    const DmaPin &pin, bool writable, GpuMapping &out)
{
    auto &mock = *static_cast<Mock *>(context);
    out = {&mock.mapCookies[handle.slot], mock.gpuBase + uint64_t(handle.slot) * 4096, pin.bytes, writable};
    return MemoryStatus::Ok;
}
MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &, const DmaPin &, GpuMapping &)
{
    return MemoryStatus::Ok;
}
bool retired(void *, const MemoryView &) { return true; }
bool stopMemory(void *context, const DeviceIdentity &) { return static_cast<Mock *>(context)->stop; }
MemoryBackend memoryBackend(Mock &mock)
{
    return {&mock, admitted, pin, unpin, sync, map, unmap, retired, stopMemory};
}
Operation claim(void *context, const ChannelBinding &binding, const MemoryView &ring,
    const MemoryView &fence, ChannelObservation &out)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("claim");
    assert(ring.jobHolds == 1 && fence.jobHolds == 1);
    out = {binding.device, binding.owner, binding.vm, binding.channel, binding.channelGeneration,
        binding.channelClass, binding.ringEntries, mock.initialPut, mock.initialPut,
        mock.baseline, mock.baseline + uint32_t(mock.incomplete), mock.mode, mock.domain};
    if (mock.wrongIdentity)
        ++out.device.registryId;
    return mock.claimResult;
}
QueueStatus seal(void *context, const ChannelBinding &, const MemoryView &commands, PushbufferRange,
    const MemoryView *resources, uint32_t count, uint64_t fenceAddress, uint32_t payload, uint64_t &bytes)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("seal");
    assert(commands.jobHolds == 1);
    for (uint32_t i = 0; i < count; ++i)
        assert(resources[i].jobHolds);
    mock.sealedFenceAddress = fenceAddress;
    mock.sealedPayload = payload;
    bytes = mock.sealedBytes;
    // Deliberate ownership-invariant fault: simulate an errant holder dropping
    // the queue's retain before a pre-PUT rejection. Ordinary transport errors
    // do not cause release() to fail. The real rollback path must keep its
    // unreturned record until authoritative recovery completes.
    if (mock.dropCommandHoldBeforeReject) {
        assert(mock.faultMemory);
        assert(mock.faultMemory->release(Owner, commands.handle) == MemoryStatus::Ok);
        return QueueStatus::Invalid;
    }
    return mock.sealResult; // Simulation, not an engine command validator.
}
QueueStatus beforePut(void *context, const ChannelBinding &)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("before");
    return mock.beforeResult;
}
Operation writePut(void *context, const ChannelBinding &, uint32_t)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("put");
    ++mock.puts;
    ++mock.workToken; // A stale token read before PUT would be detected.
    return mock.putResult;
}
QueueStatus afterPut(void *context, const ChannelBinding &)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("after");
    return mock.afterResult;
}
QueueStatus token(void *context, const ChannelBinding &, uint32_t &out)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("token");
    out = mock.workToken;
    return mock.tokenResult;
}
Operation bell(void *context, const ChannelBinding &, uint32_t token)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("bell");
    mock.rungToken = token;
    return mock.bellResult;
}
QueueStatus fence(void *context, const ChannelBinding &, const MemoryView &, uint32_t &out)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("fence");
    out = mock.completed;
    return mock.fenceResult;
}
bool quiesce(void *context, const ChannelBinding &)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("stop");
    return mock.stop;
}
Operation releaseChannel(void *context, const ChannelBinding &)
{
    auto &mock = *static_cast<Mock *>(context);
    mock.events.emplace_back("release-channel");
    return mock.releaseResult;
}
QueueTransport transport(Mock &mock)
{
    return {&mock, claim, seal, beforePut, writePut, afterPut, token, bell, fence, quiesce, releaseChannel};
}
struct Fixture {
    Mock mock;
    MemoryOwner memory;
    GpFifoQueue queue {memory, transport(mock)};
    ChannelBinding binding;
    MemoryHandle commands {}, data {};
    Fixture()
    {
        mock.faultMemory = &memory;
        assert(memory.initialize(Device, {}, memoryBackend(mock)) == MemoryStatus::Ok);
        binding = {Device, Owner, 47, 73, 11, 0xC56F, 4, allocation(), allocation(), 16, 4};
        commands = allocation();
        data = allocation();
        mock.events.clear();
    }
    MemoryHandle allocation(bool writable = true, uint64_t owner = Owner)
    {
        MemoryHandle result;
        assert(memory.allocate(owner, 4096, result) == MemoryStatus::Ok);
        assert(memory.map(owner, result, writable) == MemoryStatus::Ok);
        return result;
    }
    SubmitRequest request(MemoryHandle command, const DataResource *resources = nullptr, uint32_t count = 0)
    {
        return {command, {0, 32, 128}, resources, count, 100};
    }
    uint32_t holds(MemoryHandle handle, uint64_t owner = Owner)
    {
        MemoryView view;
        assert(memory.inspect(owner, handle, view) == MemoryStatus::Ok);
        return view.jobHolds;
    }
    uint8_t *cpu(MemoryHandle handle)
    {
        MemoryView view;
        assert(memory.inspect(Owner, handle, view) == MemoryStatus::Ok);
        return view.pin.cpu;
    }
    uint64_t address(MemoryHandle handle)
    {
        MemoryView view;
        assert(memory.inspect(Owner, handle, view) == MemoryStatus::Ok);
        return view.mapping.address;
    }
    void finish()
    {
        mock.stop = true;
        mock.releaseResult = Operation::Done;
        mock.syncFails = false;
        assert(queue.reset() == QueueStatus::Ok);
        assert(memory.quiesce() == MemoryStatus::Ok);
        assert(memory.close() == MemoryStatus::Ok);
    }
};
void publicationAndCompletion()
{
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    assert(f.holds(f.binding.ring) == 1 && f.holds(f.binding.fence) == 1);
    f.mock.events.clear();
    const DataResource data {f.data, true};
    FenceToken job;
    assert(f.queue.submit(f.request(f.commands, &data, 1), 1, job) == QueueStatus::Ok);
    const std::vector<std::string> expected {"seal", "device:2", "device:3", "device:0",
        "before", "put", "after", "token", "bell"};
    assert(f.mock.events == expected);
    assert(f.mock.rungToken == 0x12345679 && f.mock.sealedPayload == job.sequence);
    assert(f.mock.sealedFenceAddress == f.address(f.binding.fence) + f.binding.fenceOffset);
    Mellow::PortedNvidia::GpfifoEntry packet;
    assert(Mellow::PortedNvidia::encodeGpfifoPushbuffer(Mellow::PortedNvidia::ChannelClass::AmpereA,
        f.address(f.commands), 64, Mellow::PortedNvidia::Sync::Proceed, packet) == Mellow::PortedNvidia::Status::Ok);
    uint8_t expectedBytes[8];
    Mellow::PortedNvidia::writeGpfifoLittleEndian(packet, expectedBytes);
    for (uint32_t i = 0; i < 8; ++i)
        assert(f.cpu(f.binding.ring)[f.binding.ringOffset + i] == expectedBytes[i]);
    assert(f.holds(f.commands) == 1 && f.holds(f.data) == 1 && f.queue.pending() == 1);
    assert(f.memory.retire(Owner, f.commands) == MemoryStatus::Busy);
    f.mock.hardwareGet = 1; // Fetch/IRQ progress is deliberately not completion.
    assert(f.queue.poll(2) == QueueStatus::Ok);
    assert(f.holds(f.commands) == 1 && f.holds(f.data) == 1);
    f.mock.completed = job.sequence;
    assert(f.queue.poll(3) == QueueStatus::Ok);
    JobResult result;
    assert(f.queue.query(job, result) == QueueStatus::Ok && result.state == JobState::Completed && !result.resourcesHeld);
    assert(f.holds(f.commands) == 0 && f.holds(f.data) == 0 && f.queue.pending() == 0);
    assert(f.holds(f.binding.ring) == 1 && f.holds(f.binding.fence) == 1);
    assert(f.queue.retire(job) == QueueStatus::Ok && f.queue.query(job, result) == QueueStatus::StaleToken);
    f.finish();
}
void sentinelAndWrap()
{
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    MemoryHandle commands[4] {f.commands, f.allocation(), f.allocation(), f.allocation()};
    FenceToken jobs[4];
    const DataResource data {f.data, true};
    for (uint32_t i = 0; i < 3; ++i)
        assert(f.queue.submit(f.request(commands[i], &data, 1), i + 1, jobs[i]) == QueueStatus::Ok);
    assert(f.queue.pending() == 3 && f.queue.put() == 3 && f.holds(f.data) == 3);
    assert(f.queue.submit(f.request(commands[3]), 4, jobs[3]) == QueueStatus::Capacity);
    assert(f.holds(commands[3]) == 0);
    f.mock.completed = jobs[0].sequence;
    assert(f.queue.poll(5) == QueueStatus::Ok && f.queue.pending() == 2);
    assert(f.holds(commands[0]) == 0 && f.holds(commands[1]) == 1 && f.holds(f.data) == 2);
    assert(f.queue.submit(f.request(commands[3], &data, 1), 6, jobs[3]) == QueueStatus::Ok);
    assert(f.queue.put() == 0 && f.queue.pending() == 3 && f.holds(f.data) == 3);
    f.mock.completed = jobs[3].sequence;
    assert(f.queue.poll(7) == QueueStatus::Ok && !f.queue.pending() && !f.holds(f.data));
    for (auto job : jobs)
        assert(f.queue.retire(job) == QueueStatus::Ok);
    f.finish();
}
void ownershipFailures()
{
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    FenceToken token {99, 99};
    auto stale = f.commands;
    ++stale.epoch;
    assert(f.queue.submit(f.request(stale), 1, token) == QueueStatus::Ownership);
    assert(!token.generation && !token.sequence && !f.mock.puts);
    const auto otherOwner = f.allocation(true, Owner + 1);
    const DataResource foreign {otherOwner, false};
    assert(f.queue.submit(f.request(f.commands, &foreign, 1), 2, token) == QueueStatus::Ownership);
    const auto readOnly = f.allocation(false);
    const DataResource writeReadOnly {readOnly, true};
    assert(f.queue.submit(f.request(f.commands, &writeReadOnly, 1), 3, token) == QueueStatus::Ownership);
    const DataResource duplicate[2] {{f.data, false}, {f.data, true}};
    assert(f.queue.submit(f.request(f.commands, duplicate, 2), 4, token) == QueueStatus::Ownership);
    const DataResource ring {f.binding.ring, false};
    assert(f.queue.submit(f.request(f.commands, &ring, 1), 5, token) == QueueStatus::Ownership);
    assert(f.queue.submit(f.request(f.commands), 6, token) == QueueStatus::Ok);
    FenceToken untouched {8, 8};
    assert(f.queue.submit(f.request(f.commands), 7, untouched) == QueueStatus::Ownership);
    const auto anotherCommand = f.allocation();
    const DataResource liveCommands {f.commands, true};
    assert(f.queue.submit(f.request(anotherCommand, &liveCommands, 1), 8, untouched) == QueueStatus::Ownership);
    assert(!untouched.generation && !untouched.sequence && f.mock.puts == 1 && f.holds(anotherCommand) == 0);
    f.finish();
}
void uncertaintyAndReset()
{
    for (uint32_t stage = 0; stage < 4; ++stage) {
        Fixture f;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        if (stage == 0) f.mock.putResult = Operation::Unknown;
        if (stage == 1) f.mock.afterResult = QueueStatus::TransportFailure;
        if (stage == 2) f.mock.tokenResult = QueueStatus::TransportFailure;
        if (stage == 3) f.mock.bellResult = Operation::Rejected;
        const DataResource data {f.data, true};
        FenceToken token;
        assert(f.queue.submit(f.request(f.commands, &data, 1), 1, token) == QueueStatus::PublicationUnknown);
        assert(f.queue.state() == QueueState::NeedsReset && f.queue.pending() == 1);
        assert(f.holds(f.commands) == 1 && f.holds(f.data) == 1);
        JobResult result;
        assert(f.queue.query(token, result) == QueueStatus::Ok && result.resourcesHeld &&
            result.state == JobState::PublicationUnknown);
        assert(f.queue.retire(token) == QueueStatus::Busy);
        FenceToken unchanged {19, 19};
        assert(f.queue.submit(f.request(f.commands), 2, unchanged) == QueueStatus::Busy);
        f.mock.stop = false;
        assert(f.queue.reset() == QueueStatus::QuiesceFailed);
        assert(f.holds(f.commands) == 1 && f.holds(f.binding.ring) == 1);
        f.mock.stop = true;
        f.mock.releaseResult = Operation::Unknown;
        assert(f.queue.reset() == QueueStatus::TransportFailure);
        assert(f.holds(f.commands) == 0 && f.holds(f.binding.ring) == 1 && f.holds(f.binding.fence) == 1);
        f.mock.releaseResult = Operation::Done;
        assert(f.queue.reset() == QueueStatus::Ok);
        assert(f.queue.query(token, result) == QueueStatus::Ok && result.state == JobState::Reset && !result.resourcesHeld);
        assert(f.queue.retire(token) == QueueStatus::Ok);
        assert(!f.holds(f.binding.ring) && !f.holds(f.binding.fence));
        f.finish();
    }
}
void unpublishedFailures()
{
    for (uint32_t stage = 0; stage < 4; ++stage) {
        Fixture f;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        if (stage == 0) f.mock.sealResult = QueueStatus::Unavailable;
        if (stage == 1) f.mock.sealedBytes = 33;
        if (stage == 2) f.mock.beforeResult = QueueStatus::TransportFailure;
        if (stage == 3) f.mock.putResult = Operation::Rejected;
        const DataResource data {f.data, true};
        FenceToken token {77, 77};
        const QueueStatus status = f.queue.submit(f.request(f.commands, &data, 1), 1, token);
        assert(status != QueueStatus::Ok && status != QueueStatus::PublicationUnknown);
        assert(f.queue.state() == QueueState::Ready && !f.queue.pending());
        assert(!f.holds(f.commands) && !f.holds(f.data) && f.holds(f.binding.ring) == 1);
        assert(!token.generation && !token.sequence);
        assert(f.mock.puts == uint32_t(stage == 3));
        f.finish();
    }
    Fixture high;
    high.mock.gpuBase = 1ULL << 40;
    const auto unencodable = high.allocation();
    assert(high.queue.bind(high.binding) == QueueStatus::Ok);
    FenceToken token;
    assert(high.queue.submit(high.request(unencodable), 1, token) == QueueStatus::Invalid);
    assert(!high.holds(unencodable) && !high.mock.puts);
    high.finish();
}
void tokenlessRollbackRecoveryDoesNotExhaustJobs()
{
    Fixture f;
    f.mock.dropCommandHoldBeforeReject = true;
    const DataResource data {f.data, true};
    for (uint32_t cycle = 0; cycle < GpFifoQueue::MaxJobs + 1; ++cycle) {
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        FenceToken token {77, 77};
        const auto status = f.queue.submit(f.request(f.commands, &data, 1), 1, token);
        if (status != QueueStatus::MemoryFailure)
            fprintf(stderr, "tokenless rollback cycle %u: status %u, expected MemoryFailure %u\n",
                cycle + 1, unsigned(status), unsigned(QueueStatus::MemoryFailure));
        assert(status == QueueStatus::MemoryFailure);
        assert(!token.generation && !token.sequence && !f.mock.puts && !f.queue.pending());
        assert(f.queue.state() == QueueState::NeedsReset);
        assert(!f.holds(f.commands) && !f.holds(f.data));
        // Restore the actual retain invariant before asking recovery to release
        // the queue's still-recorded hold. This is fault injection, not normal
        // MemoryOwner or transport behaviour.
        assert(f.memory.retain(Owner, f.commands) == MemoryStatus::Ok);
        assert(f.queue.reset() == QueueStatus::Ok);
        assert(f.queue.state() == QueueState::Detached && !f.holds(f.commands));
        assert(!f.holds(f.binding.ring) && !f.holds(f.binding.fence));
        ++f.binding.channelGeneration;
    }
    f.finish();
}

void tokenlessRecoveryNeedsCompleteNativeRelease()
{
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    f.mock.dropCommandHoldBeforeReject = true;
    FenceToken returned {77, 77};
    assert(f.queue.submit(f.request(f.commands), 1, returned) == QueueStatus::MemoryFailure);
    assert(!returned.generation && !returned.sequence && !f.mock.puts);
    // Diagnostic knowledge only: no caller received this internal token.
    const FenceToken internal {1, 1};
    JobResult result;
    assert(f.queue.query(internal, result) == QueueStatus::Ok && result.resourcesHeld);
    f.mock.stop = false;
    assert(f.queue.reset() == QueueStatus::QuiesceFailed);
    assert(f.queue.query(internal, result) == QueueStatus::Ok && result.resourcesHeld);
    f.mock.stop = true;
    assert(f.queue.reset() == QueueStatus::MemoryFailure);
    assert(f.queue.query(internal, result) == QueueStatus::Ok && result.resourcesHeld);
    assert(f.memory.retain(Owner, f.commands) == MemoryStatus::Ok);
    f.mock.releaseResult = Operation::Unknown;
    assert(f.queue.reset() == QueueStatus::TransportFailure);
    assert(f.queue.query(internal, result) == QueueStatus::Ok && !result.resourcesHeld &&
        result.state == JobState::Reset);
    assert(f.holds(f.binding.ring) == 1 && f.holds(f.binding.fence) == 1);
    f.mock.releaseResult = Operation::Done;
    // Even a successful native channel release is insufficient if a retained
    // binding allocation has not yet been released.
    assert(f.memory.release(Owner, f.binding.fence) == MemoryStatus::Ok);
    assert(f.queue.reset() == QueueStatus::MemoryFailure);
    assert(f.queue.query(internal, result) == QueueStatus::Ok && !result.resourcesHeld);
    assert(f.memory.retain(Owner, f.binding.fence) == MemoryStatus::Ok);
    assert(f.queue.reset() == QueueStatus::Ok);
    assert(f.queue.query(internal, result) == QueueStatus::StaleToken);
    f.finish();
}

void issuedCompletionSurvivesResetUntilAcknowledged()
{
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    FenceToken completed;
    assert(f.queue.submit(f.request(f.commands), 1, completed) == QueueStatus::Ok);
    f.mock.completed = completed.sequence;
    assert(f.queue.poll(2) == QueueStatus::Ok && !f.holds(f.commands));
    assert(f.queue.reset() == QueueStatus::Ok);
    JobResult result;
    assert(f.queue.query(completed, result) == QueueStatus::Ok &&
        result.state == JobState::Completed && !result.resourcesHeld);
    ++f.binding.channelGeneration;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    FenceToken reset;
    assert(f.queue.submit(f.request(f.commands), 1, reset) == QueueStatus::Ok);
    assert(f.queue.reset() == QueueStatus::Ok);
    assert(f.queue.query(completed, result) == QueueStatus::Ok && result.state == JobState::Completed);
    assert(f.queue.query(reset, result) == QueueStatus::Ok && result.state == JobState::Reset);
    assert(f.queue.retire(completed) == QueueStatus::Ok);
    assert(f.queue.retire(reset) == QueueStatus::Ok);
    assert(f.queue.query(completed, result) == QueueStatus::StaleToken);
    assert(f.queue.query(reset, result) == QueueStatus::StaleToken);
    f.finish();
}

void bindingFailures()
{
    {
        Fixture f;
        auto incompleteTransport = transport(f.mock);
        incompleteTransport.sealPushbuffer = nullptr;
        GpFifoQueue absent(f.memory, incompleteTransport);
        assert(absent.bind(f.binding) == QueueStatus::Unavailable);
        assert(!f.holds(f.binding.ring) && f.mock.events.empty());
        auto unsupported = f.binding;
        unsupported.channelClass = 0xB06F;
        assert(f.queue.bind(unsupported) == QueueStatus::UnsupportedClass);
        auto stale = f.binding;
        ++stale.device.epoch;
        assert(f.queue.bind(stale) == QueueStatus::Ownership);
        auto range = f.binding;
        range.ringOffset = 4096;
        assert(f.queue.bind(range) == QueueStatus::Invalid);
        assert(!f.holds(f.binding.ring) && !f.holds(f.binding.fence));
        f.finish();
    }
    for (uint32_t scenario = 0; scenario < 6; ++scenario) {
        Fixture f;
        if (scenario == 0) f.mock.mode = ChannelMode::WorkLaunch;
        if (scenario == 1) f.mock.domain = RingDomain::NoncoherentSystem;
        if (scenario == 2) f.mock.wrongIdentity = true;
        if (scenario == 3) f.mock.incomplete = true;
        if (scenario == 4) f.mock.claimResult = Operation::Unknown;
        if (scenario == 5) f.mock.claimResult = Operation::Rejected;
        assert(f.queue.bind(f.binding) != QueueStatus::Ok);
        assert(f.holds(f.binding.ring) == uint32_t(scenario != 5));
        assert(f.holds(f.binding.fence) == uint32_t(scenario != 5));
        f.mock.events.clear();
        assert(f.queue.poll(1) == QueueStatus::Busy);
        assert(f.mock.events.empty()); // No DMA sync or fence callback on an unverified channel.
        f.finish();
    }
}
void fenceAndDeadlineFailures()
{
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        Fixture f;
        f.mock.baseline = f.mock.completed = 10;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        FenceToken token;
        assert(f.queue.submit(f.request(f.commands), 1, token) == QueueStatus::Ok && token.sequence == 11);
        if (scenario == 0) f.mock.completed = 9;
        if (scenario == 1) f.mock.completed = 12;
        if (scenario == 2) f.mock.fenceResult = QueueStatus::InvalidFence; // CPU/IRQ/non-acquire source rejected.
        assert(f.queue.poll(2) == QueueStatus::InvalidFence);
        assert(f.queue.state() == QueueState::NeedsReset && f.holds(f.commands) == 1);
        f.finish();
    }
    Fixture f;
    assert(f.queue.bind(f.binding) == QueueStatus::Ok);
    FenceToken token;
    assert(f.queue.submit(f.request(f.commands), 10, token) == QueueStatus::Ok);
    assert(f.queue.poll(9) == QueueStatus::ClockRegression && f.holds(f.commands) == 1);
    assert(f.queue.expire(100) == QueueStatus::TimedOut && f.holds(f.commands) == 1);
    JobResult result;
    assert(f.queue.query(token, result) == QueueStatus::Ok && result.state == JobState::TimedOut && result.resourcesHeld);
    f.mock.stop = false;
    assert(f.queue.reset() == QueueStatus::QuiesceFailed && f.holds(f.commands) == 1);
    f.finish();
}
void exhaustionSynchronizationAndRebind()
{
    {
        Fixture f;
        f.mock.baseline = f.mock.completed = UINT32_MAX;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        FenceToken token {8, 8};
        assert(f.queue.submit(f.request(f.commands), 1, token) == QueueStatus::SequenceExhausted);
        assert(!token.generation && !token.sequence && !f.mock.puts && !f.holds(f.commands));
        f.finish();
    }
    {
        Fixture f;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        f.mock.syncFails = true;
        FenceToken token;
        assert(f.queue.submit(f.request(f.commands), 1, token) == QueueStatus::MemoryFailure);
        assert(f.queue.state() == QueueState::NeedsReset && !f.mock.puts && !f.holds(f.commands));
        assert(f.holds(f.binding.ring) == 1 && f.memory.draining());
        f.finish();
    }
    {
        Fixture f;
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        FenceToken old;
        assert(f.queue.submit(f.request(f.commands), 1, old) == QueueStatus::Ok);
        assert(f.queue.reset() == QueueStatus::Ok);
        ++f.binding.channelGeneration; // New measured owner/channel lease.
        assert(f.queue.bind(f.binding) == QueueStatus::Ok);
        FenceToken current;
        assert(f.queue.submit(f.request(f.commands), 2, current) == QueueStatus::Ok);
        assert(old.generation != current.generation && old.sequence == current.sequence);
        JobResult result;
        assert(f.queue.query(old, result) == QueueStatus::Ok && result.state == JobState::Reset);
        assert(f.queue.retire(old) == QueueStatus::Ok);
        assert(f.queue.query(old, result) == QueueStatus::StaleToken && result.state == JobState::Free && !result.token.generation);
        assert(f.queue.query(current, result) == QueueStatus::Ok && result.resourcesHeld && result.state == JobState::Submitted);
        f.finish();
    }
}
} // namespace

int main()
{
    publicationAndCompletion();
    sentinelAndWrap();
    ownershipFailures();
    uncertaintyAndReset();
    unpublishedFailures();
    tokenlessRollbackRecoveryDoesNotExhaustJobs();
    tokenlessRecoveryNeedsCompleteNativeRelease();
    issuedCompletionSurvivesResetUntilAcknowledged();
    bindingFailures();
    fenceAndDeadlineFailures();
    exhaustionSynchronizationAndRebind();
    puts("Native NVIDIA GPFIFO queue host regressions passed; no physical GPU execution.");
}

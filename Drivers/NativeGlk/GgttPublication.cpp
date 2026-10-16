// SPDX-License-Identifier: MIT
/*
 * Copyright © 2013, 2016, 2019, 2020, 2022, 2023, 2025 Intel Corporation
 * Copyright 2003 Tungsten Graphics, Inc.
 * Copyright (c) 2026 Mellow contributors
 * Full permission notice and per-file upstream notices: GgttPublication.hpp.
 * Exact upstream blobs, SHA-256 and symbols: GgttPublication.provenance.json.
 */
#include "GgttPublication.hpp"

namespace Mellow { namespace NativeGlk {
namespace {
constexpr uint64_t kPage = Glk::kPageSize4K;
constexpr uint64_t kOneGiB = uint64_t(1) << 30;

bool pageAligned(uint64_t value) { return !(value & (kPage - 1)); }

// Callers have already proven neither range wraps.
bool intersects(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes) {
    return a < b + bBytes && b < a + aBytes;
}

bool validKind(GgttRangeKind kind) {
    return kind == GgttRangeKind::Firmware || kind == GgttRangeKind::Display ||
           kind == GgttRangeKind::Stolen || kind == GgttRangeKind::Platform;
}

bool sameGeometry(const GgttGeometry &a, const GgttGeometry &b) {
    return a.total == b.total && a.entryCount == b.entryCount && a.gsmBytes == b.gsmBytes &&
           a.gsmBarOffset == b.gsmBarOffset && a.firstUsable == b.firstUsable &&
           a.endUsable == b.endUsable;
}

bool sameImage(const uint8_t (&a)[8], const uint8_t (&b)[8]) {
    uint8_t diff = 0;
    for (unsigned i = 0; i < 8; ++i) diff |= uint8_t(a[i] ^ b[i]);
    return !diff;
}
} // namespace

GgttStatus deriveGgttGeometry(uint16_t gmchControl, uint64_t gttMmadrBytes, GgttGeometry &out) {
    // ggtt_probe_common only warns on a wrong GTTMMADR length; a native owner
    // cannot place the GSM inside a resource of unknown shape, so refuse.
    if (gttMmadrBytes != Gen9LpGgtt::kGttMmadrBytes) return GgttStatus::InvalidArgument;
    uint64_t total = 0;
    // GGMS 0 selects no GSM at all: there is no GGTT to publish into.
    if (ggttTotalFromGmchControl(gmchControl, total) != PteStatus::Ok) return GgttStatus::Unavailable;
    GgttGeometry geometry {};
    geometry.total = total;
    geometry.entryCount = total / kPage;
    geometry.gsmBytes = geometry.entryCount * Glk::kGgttEntryBytes;
    geometry.gsmBarOffset = Gen9LpGgtt::kGsmBarOffset;
    if (geometry.gsmBytes > gttMmadrBytes - geometry.gsmBarOffset) return GgttStatus::InvalidArgument;
    geometry.firstUsable = Gen9LpGgtt::kPinBiasBytes;
    geometry.endUsable = total - Gen9LpGgtt::kTopGuardBytes;
    out = geometry;
    return GgttStatus::Ok;
}

GgttStatus decodeGttMmadrBar(uint32_t barLow, uint32_t barHigh, uint16_t command, uint64_t &base) {
    using namespace Gen9LpGgtt;
    // All-ones (absent function) fails the I/O-space test first.
    if (barLow & kBarIoSpace) return GgttStatus::InvalidArgument;
    if ((barLow & kBarTypeMask) != kBarType64) return GgttStatus::InvalidArgument;
    if (barLow & kBarPrefetchable) return GgttStatus::InvalidArgument;
    // Memory decode is never enabled here; a disabled BAR is not usable.
    if (!(command & kCommandMemorySpace)) return GgttStatus::Unavailable;
    const uint64_t value = (uint64_t(barHigh) << 32) | (barLow & ~uint32_t(0xf));
    if (!value || (value & (kGttMmadrBytes - 1))) return GgttStatus::InvalidArgument;
    base = value;
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::initialize(const GgttGeometry &geometry, const GgttHandover &handover,
                                     GgttIo io, GgttGtAccess gt) {
    if (initialized_ || faulted_) return GgttStatus::Busy;
    if (geometry.total != kOneGiB && geometry.total != 2 * kOneGiB && geometry.total != 4 * kOneGiB)
        return GgttStatus::InvalidArgument;
    GgttGeometry expected {};
    expected.total = geometry.total;
    expected.entryCount = geometry.total / kPage;
    expected.gsmBytes = expected.entryCount * Glk::kGgttEntryBytes;
    expected.gsmBarOffset = Gen9LpGgtt::kGsmBarOffset;
    expected.firstUsable = Gen9LpGgtt::kPinBiasBytes;
    expected.endUsable = geometry.total - Gen9LpGgtt::kTopGuardBytes;
    if (!sameGeometry(geometry, expected)) return GgttStatus::InvalidArgument;
    if (!io.context || !io.writeEntry || !io.readEntry || !io.writeRegister || !io.readRegister ||
        !io.fence || !io.now || !io.pause) return GgttStatus::Unavailable;
    if (!gt.context || !gt.acquire || !gt.release || !gt.awakeEngines) return GgttStatus::Unavailable;
    // Without the complete handover no range can be proven native-owned.
    if (!handover.complete || !handover.registryId || !handover.epoch) return GgttStatus::Unavailable;
    if (handover.ggttTotal != geometry.total) return GgttStatus::Ownership;
    if (handover.retainedCount > MaxRetained || (handover.retainedCount && !handover.retained))
        return GgttStatus::InvalidArgument;
    const GgttRange pool = handover.pool;
    if (!pool.bytes || !pageAligned(pool.start) || !pageAligned(pool.bytes) ||
        pool.start > UINT64_MAX - pool.bytes || pool.start < expected.firstUsable ||
        pool.start + pool.bytes > expected.endUsable) return GgttStatus::InvalidArgument;
    for (uint32_t i = 0; i < handover.retainedCount; ++i) {
        const GgttRetainedRange &range = handover.retained[i];
        if (!validKind(range.kind) || !range.bytes || !pageAligned(range.start) ||
            !pageAligned(range.bytes) || range.start > UINT64_MAX - range.bytes ||
            range.start + range.bytes > geometry.total) return GgttStatus::InvalidArgument;
        // Overlapping retained ranges mean the producer's record is inconsistent.
        for (uint32_t j = 0; j < i; ++j) {
            if (intersects(range.start, range.bytes, handover.retained[j].start, handover.retained[j].bytes))
                return GgttStatus::Overlap;
        }
    }
    for (uint32_t i = 0; i < handover.retainedCount; ++i) retained_[i] = handover.retained[i];
    retainedCount_ = handover.retainedCount;
    geometry_ = geometry;
    pool_ = pool;
    io_ = io;
    gt_ = gt;
    initialized_ = true;
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::checkPlacement(uint64_t start, uint64_t bytes) const {
    if (!bytes || !pageAligned(start) || !pageAligned(bytes) || start > UINT64_MAX - bytes)
        return GgttStatus::InvalidArgument;
    // Only the handed-over pool is native-owned; the pool already excludes
    // the pin-bias page and the top guard page.
    if (start < pool_.start || start + bytes > pool_.start + pool_.bytes) return GgttStatus::Ownership;
    for (uint32_t i = 0; i < retainedCount_; ++i) {
        if (intersects(start, bytes, retained_[i].start, retained_[i].bytes)) return GgttStatus::Overlap;
    }
    for (const Slot &slot : slots_) {
        if (slot.state != GgttExtentState::Free && intersects(start, bytes, slot.start, slot.bytes))
            return GgttStatus::Overlap;
    }
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::claim(uint64_t owner, uint64_t start, uint64_t bytes, GgttExtent &out) {
    const GgttStatus placement = checkPlacement(start, bytes);
    if (placement != GgttStatus::Ok) return placement;
    if (generation_ == UINT64_MAX) return GgttStatus::Capacity;
    for (uint32_t i = 0; i < MaxExtents; ++i) {
        Slot &slot = slots_[i];
        if (slot.state != GgttExtentState::Free) continue;
        slot = {};
        slot.state = GgttExtentState::Reserved;
        slot.owner = owner;
        slot.generation = generation_++;
        slot.start = start;
        slot.bytes = bytes;
        out = {i, slot.generation, start, bytes};
        return GgttStatus::Ok;
    }
    return GgttStatus::Capacity;
}

GgttStatus GgttPublisher::reserve(uint64_t owner, uint64_t bytes, GgttExtent &out) {
    out = {};
    if (!initialized_) return GgttStatus::Unavailable;
    if (faulted_) return GgttStatus::Quarantined;
    if (!owner || !bytes || !pageAligned(bytes)) return GgttStatus::InvalidArgument;
    if (bytes > pool_.bytes) return GgttStatus::Capacity;
    const uint64_t lastStart = pool_.start + pool_.bytes - bytes;
    uint64_t candidate = pool_.start;
    // First fit. Each step moves past at least one blocking range for good,
    // so the walk is bounded by the number of ranges that can block.
    for (uint32_t step = 0; step <= MaxRetained + MaxExtents; ++step) {
        if (candidate > lastStart) return GgttStatus::Capacity;
        uint64_t blockedUntil = 0;
        for (uint32_t i = 0; i < retainedCount_; ++i) {
            const GgttRetainedRange &range = retained_[i];
            if (intersects(candidate, bytes, range.start, range.bytes) &&
                range.start + range.bytes > blockedUntil) blockedUntil = range.start + range.bytes;
        }
        for (const Slot &slot : slots_) {
            if (slot.state != GgttExtentState::Free && intersects(candidate, bytes, slot.start, slot.bytes) &&
                slot.start + slot.bytes > blockedUntil) blockedUntil = slot.start + slot.bytes;
        }
        if (!blockedUntil) return claim(owner, candidate, bytes, out);
        candidate = blockedUntil;
    }
    return GgttStatus::Capacity;
}

GgttStatus GgttPublisher::reserveAt(uint64_t owner, uint64_t start, uint64_t bytes, GgttExtent &out) {
    out = {};
    if (!initialized_) return GgttStatus::Unavailable;
    if (faulted_) return GgttStatus::Quarantined;
    if (!owner) return GgttStatus::InvalidArgument;
    return claim(owner, start, bytes, out);
}

GgttPublisher::Slot *GgttPublisher::lookup(uint64_t owner, const GgttExtent &extent) {
    if (extent.slot >= MaxExtents) return nullptr;
    Slot &slot = slots_[extent.slot];
    if (slot.state == GgttExtentState::Free || !owner || slot.owner != owner ||
        slot.generation != extent.generation || slot.start != extent.start || slot.bytes != extent.bytes)
        return nullptr;
    return &slot;
}

const GgttPublisher::Slot *GgttPublisher::lookup(const GgttExtent &extent) const {
    if (extent.slot >= MaxExtents) return nullptr;
    const Slot &slot = slots_[extent.slot];
    if (slot.state == GgttExtentState::Free || slot.generation != extent.generation ||
        slot.start != extent.start || slot.bytes != extent.bytes) return nullptr;
    return &slot;
}

GgttStatus GgttPublisher::release(uint64_t owner, const GgttExtent &extent) {
    if (!initialized_) return GgttStatus::Unavailable;
    Slot *slot = lookup(owner, extent);
    if (!slot) return GgttStatus::Ownership;
    if (slot->state == GgttExtentState::Quarantined) return GgttStatus::Quarantined;
    if (slot->state != GgttExtentState::Reserved) return GgttStatus::Busy;
    *slot = {};
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::quarantine(Slot &slot, GgttStatus cause) {
    // Entries may reference the caller's pages and TLBs may hold them: the
    // extent, its capture and the backing stay held; nothing reuses them.
    slot.state = GgttExtentState::Quarantined;
    faulted_ = true;
    return cause;
}

GgttStatus GgttPublisher::readEntries(const Slot &slot, uint64_t *into) {
    const uint64_t first = slot.start / kPage;
    for (size_t i = 0; i < slot.pageCount; ++i) {
        uint8_t image[8] {};
        if (!io_.readEntry(io_.context, first + i, image)) return GgttStatus::TransportFailure;
        into[i] = loadLe64(image);
    }
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::writeEntries(const Slot &slot, const uint64_t *entries) {
    const uint64_t first = slot.start / kPage;
    for (size_t i = 0; i < slot.pageCount; ++i) {
        uint8_t image[8] {};
        storeLe64(entries[i], image);
        // gen8_set_pte: exactly one 64-bit store per entry.
        if (!io_.writeEntry(io_.context, first + i, image)) return GgttStatus::TransportFailure;
    }
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::verifyEntries(const Slot &slot, const uint64_t *entries) {
    // Non-posted reads complete only after the preceding posted writes to the
    // same function, so this both drains and checks every entry. It is the
    // readback check that catches dropped (e.g. wider than 64-bit) writes.
    const uint64_t first = slot.start / kPage;
    for (size_t i = 0; i < slot.pageCount; ++i) {
        uint8_t expected[8] {}, actual[8] {};
        storeLe64(entries[i], expected);
        if (!io_.readEntry(io_.context, first + i, actual)) return GgttStatus::TransportFailure;
        if (!sameImage(expected, actual)) return GgttStatus::ReadbackMismatch;
    }
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::waitTlbControlClear(uint32_t offset) {
    using namespace Gen9LpGgtt;
    // __intel_wait_for_register_fw(reg, done, 0, 100 us, 4 ms): a tight phase
    // then a sleeping phase with backoff and one check after expiry. The poll
    // cap keeps the wait bounded even if the clock stops advancing.
    uint32_t polls = 0;
    uint64_t start = 0, now = 0;
    uint32_t value = 0;
    if (!io_.now(io_.context, start)) return GgttStatus::TransportFailure;
    for (;;) {
        if (!io_.now(io_.context, now)) return GgttStatus::TransportFailure;
        if (now < start) return GgttStatus::ClockRegression;
        if (++polls > kMaxTlbPolls) return GgttStatus::InvalidateTimeout;
        if (!io_.readRegister(io_.context, offset, value)) return GgttStatus::TransportFailure;
        if (!(value & kTlbInvalidateBit)) return GgttStatus::Ok;
        if (now - start >= uint64_t(kTlbFastTimeoutUs) * 1000) break;
        io_.pause(io_.context, 1);
    }
    start = now;
    uint32_t wait = kSlowWaitMinUs;
    for (;;) {
        if (!io_.now(io_.context, now)) return GgttStatus::TransportFailure;
        if (now < start) return GgttStatus::ClockRegression;
        const bool expired = now - start > uint64_t(kTlbSlowTimeoutMs) * 1000000;
        if (++polls > kMaxTlbPolls) return GgttStatus::InvalidateTimeout;
        if (!io_.readRegister(io_.context, offset, value)) return GgttStatus::TransportFailure;
        if (!(value & kTlbInvalidateBit)) return GgttStatus::Ok;
        if (expired) return GgttStatus::InvalidateTimeout;
        io_.pause(io_.context, wait);
        if (wait < kSlowWaitMaxUs) wait <<= 1;
    }
}

GgttStatus GgttPublisher::invalidateEngineTlbs(uint32_t awake) {
    using namespace Gen9LpGgtt;
    // gen8_ggtt_invalidate on GEN9_LP: the GSM is mapped uncached, so there
    // is no write-combining buffer to flush and no GFX_FLSH_CNTL write.
    // mmio_invalidate_full skips engines whose runtime-PM wakeref is not
    // active. The native owner sampled this mask under this same held token;
    // it serializes every wake/resume invalidation until the token release.
    for (uint32_t i = 0; i < kEngineCount; ++i) {
        if (!(awake & (1U << i))) continue;
        if (!io_.writeRegister(io_.context, kEngineTlbControls[i], kTlbInvalidateBit))
            return GgttStatus::TransportFailure;
    }
    for (uint32_t i = 0; i < kEngineCount; ++i) {
        if (!(awake & (1U << i))) continue;
        const GgttStatus status = waitTlbControlClear(kEngineTlbControls[i]);
        if (status != GgttStatus::Ok) return status;
    }
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::publish(uint64_t owner, const GgttExtent &extent, const uint64_t *dmaPages,
                                  size_t pageCount, uint64_t *capture, size_t captureCount) {
    if (!initialized_) return GgttStatus::Unavailable;
    Slot *slot = lookup(owner, extent);
    if (!slot) return GgttStatus::Ownership;
    if (faulted_ || slot->state == GgttExtentState::Quarantined) return GgttStatus::Quarantined;
    if (slot->state != GgttExtentState::Reserved) return GgttStatus::Busy;
    if (!dmaPages || !capture || !pageCount || captureCount < pageCount ||
        pageCount != slot->bytes / kPage) return GgttStatus::InvalidArgument;
    const uintptr_t pagesBegin = reinterpret_cast<uintptr_t>(dmaPages);
    const uintptr_t captureBegin = reinterpret_cast<uintptr_t>(capture);
    if (captureBegin < pagesBegin + pageCount * sizeof(uint64_t) &&
        pagesBegin < captureBegin + captureCount * sizeof(uint64_t)) return GgttStatus::InvalidArgument;
    // Every page must encode (4 KiB aligned, below 2^39) before any effect.
    for (size_t i = 0; i < pageCount; ++i) {
        uint64_t entry = 0;
        if (encodeGgttPte(dmaPages[i], 0, entry) != PteStatus::Ok) return GgttStatus::EncodingRejected;
    }
    uint64_t token = 0;
    if (!gt_.acquire(gt_.context, token)) return GgttStatus::Unavailable;
    uint32_t awake = 0;
    if (!token || !gt_.awakeEngines(gt_.context, token, awake) ||
        (awake & ~((1U << Gen9LpGgtt::kEngineCount) - 1))) {
        // No entry was touched. Unknown native PM state is not permission to
        // invalidate all engines, nor to skip invalidation for an awake one.
        if (!gt_.release(gt_.context, token)) return quarantine(*slot, GgttStatus::TransportFailure);
        return GgttStatus::Unavailable;
    }
    slot->pageCount = pageCount;
    // Capture is read-only: a failure here changed no entry.
    if (readEntries(*slot, capture) != GgttStatus::Ok) {
        slot->pageCount = 0;
        if (!gt_.release(gt_.context, token)) return quarantine(*slot, GgttStatus::TransportFailure);
        return GgttStatus::Unavailable;
    }
    slot->capture = capture;
    // Order the caller's page contents and cache maintenance before the
    // first uncached entry store, then the stores before the readback.
    io_.fence(io_.context, GgttFence::Full);
    GgttStatus status = GgttStatus::Ok;
    const uint64_t first = slot->start / kPage;
    for (size_t i = 0; i < pageCount && status == GgttStatus::Ok; ++i) {
        uint64_t entry = 0;
        uint8_t image[8] {};
        (void)encodeGgttPte(dmaPages[i], 0, entry);
        storeLe64(entry, image);
        if (!io_.writeEntry(io_.context, first + i, image)) status = GgttStatus::TransportFailure;
    }
    io_.fence(io_.context, GgttFence::Full);
    for (size_t i = 0; i < pageCount && status == GgttStatus::Ok; ++i) {
        uint64_t entry = 0;
        uint8_t expected[8] {}, actual[8] {};
        (void)encodeGgttPte(dmaPages[i], 0, entry);
        storeLe64(entry, expected);
        if (!io_.readEntry(io_.context, first + i, actual)) status = GgttStatus::TransportFailure;
        else if (!sameImage(expected, actual)) status = GgttStatus::ReadbackMismatch;
    }
    if (status == GgttStatus::Ok) status = invalidateEngineTlbs(awake);
    const bool released = gt_.release(gt_.context, token);
    if (status != GgttStatus::Ok) return quarantine(*slot, status);
    if (!released) return quarantine(*slot, GgttStatus::TransportFailure);
    slot->state = GgttExtentState::Published;
    return GgttStatus::Ok;
}

GgttStatus GgttPublisher::unpublish(uint64_t owner, const GgttExtent &extent, bool consumersRetired) {
    if (!initialized_) return GgttStatus::Unavailable;
    Slot *slot = lookup(owner, extent);
    if (!slot) return GgttStatus::Ownership;
    if (faulted_ || slot->state == GgttExtentState::Quarantined) return GgttStatus::Quarantined;
    if (slot->state != GgttExtentState::Published) return GgttStatus::InvalidArgument;
    // A completed TLB invalidation does not retire in-flight consumers.
    if (!consumersRetired) return GgttStatus::Busy;
    if (!slot->capture || slot->pageCount != slot->bytes / kPage)
        return quarantine(*slot, GgttStatus::TransportFailure);
    uint64_t token = 0;
    if (!gt_.acquire(gt_.context, token)) return GgttStatus::Unavailable;
    uint32_t awake = 0;
    if (!token || !gt_.awakeEngines(gt_.context, token, awake) ||
        (awake & ~((1U << Gen9LpGgtt::kEngineCount) - 1))) {
        // No entry was touched. Unknown native PM state is not permission to
        // invalidate all engines, nor to skip invalidation for an awake one.
        if (!gt_.release(gt_.context, token)) return quarantine(*slot, GgttStatus::TransportFailure);
        return GgttStatus::Unavailable;
    }
    io_.fence(io_.context, GgttFence::Full);
    GgttStatus status = writeEntries(*slot, slot->capture);
    io_.fence(io_.context, GgttFence::Full);
    if (status == GgttStatus::Ok) status = verifyEntries(*slot, slot->capture);
    if (status == GgttStatus::Ok) status = invalidateEngineTlbs(awake);
    const bool released = gt_.release(gt_.context, token);
    if (status != GgttStatus::Ok) return quarantine(*slot, status);
    if (!released) return quarantine(*slot, GgttStatus::TransportFailure);
    slot->state = GgttExtentState::Reserved;
    slot->capture = nullptr;
    slot->pageCount = 0;
    return GgttStatus::Ok;
}

GgttExtentState GgttPublisher::state(const GgttExtent &extent) const {
    const Slot *slot = lookup(extent);
    return slot ? slot->state : GgttExtentState::Free;
}

const uint64_t *GgttPublisher::captured(const GgttExtent &extent) const {
    const Slot *slot = lookup(extent);
    return slot ? slot->capture : nullptr;
}

uint32_t GgttPublisher::extentsInUse() const {
    uint32_t count = 0;
    for (const Slot &slot : slots_) count += slot.state != GgttExtentState::Free;
    return count;
}

GgttStatus GgttPublisher::shutdown() {
    if (!initialized_) return GgttStatus::Ok;
    for (const Slot &slot : slots_) {
        if (slot.state == GgttExtentState::Quarantined) return GgttStatus::Quarantined;
        if (slot.state != GgttExtentState::Free) return GgttStatus::Busy;
    }
    for (Slot &slot : slots_) slot = {};
    for (GgttRetainedRange &range : retained_) range = {};
    retainedCount_ = 0;
    geometry_ = {};
    pool_ = {};
    io_ = {};
    gt_ = {};
    initialized_ = false;
    return GgttStatus::Ok;
}

} } // namespace Mellow::NativeGlk

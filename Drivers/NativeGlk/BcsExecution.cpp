// SPDX-License-Identifier: MIT
// Copyright © 2014 Intel Corporation      (gt/intel_execlists_submission.c, gt/gen8_engine_cs.c)
// Copyright © 2014-2018 Intel Corporation (gt/intel_lrc_reg.h)
// Copyright © 2016 Intel Corporation      (gt/intel_engine_cs.c)
// Copyright © 2008-2018 Intel Corporation (gt/intel_reset.c)
// Copyright © 2019 Intel Corporation      (gt/intel_ring.h)
// Copyright © 2022 Intel Corporation      (gt/intel_engine_regs.h)
// Copyright © 2017 Intel Corporation      (intel_uncore.h)
// Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas. (i915_reg.h)
// Copyright (c) 2026 Mellow contributors.
// Verbatim upstream notices and permission text: BcsExecution.LICENSE.MIT.
#include "BcsExecution.hpp"

namespace MellowNativeGlk::Bcs {
namespace {

constexpr uint32_t PageBytes = Lrc::PageBytes;
constexpr uint64_t GgttLimit = 1ULL << 32;
constexpr uint32_t RequestBytes = StoreProbeRingDwords * 4;
constexpr uint32_t AllOnes = 0xFFFFFFFFu;
constexpr uint32_t RingMaxBytes = 0x001FF000 + PageBytes; // RING_NR_PAGES + RING_CTL_SIZE
// __intel_engine_stop_cs(engine, 1000, stop_timeout(engine)): a 1000 us
// atomic phase, then a bounded slow phase. The 100 ms slow bound is this
// unit's policy (props.stop_timeout_ms is a Kconfig value not pinned here).
constexpr uint32_t StopFastPolls = 1000, StopSlowPolls = 100;
constexpr uint32_t ResetAckPolls = 700; // gen8_engine_reset_prepare: 700 us
constexpr uint32_t CsbRetryPolls = 10;  // wa_csb_read: wait_for_atomic_us(.., 10)

uint32_t loadDword(const uint8_t *bytes) {
    const volatile uint8_t *v = bytes; // READ_ONCE of device-written memory
    return uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 | uint32_t(v[3]) << 24;
}
void storeDword(uint8_t *bytes, uint32_t value) {
    volatile uint8_t *v = bytes; // WRITE_ONCE
    v[0] = uint8_t(value); v[1] = uint8_t(value >> 8);
    v[2] = uint8_t(value >> 16); v[3] = uint8_t(value >> 24);
}
bool powerOfTwo(uint64_t value) { return value && !(value & (value - 1)); }
bool overlaps(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes) {
    return a < b + bBytes && b < a + aBytes;
}
bool cpuOverlaps(const Lease &a, const Lease &b) {
    if (!a.cpu || !b.cpu) return false;
    return overlaps(reinterpret_cast<uintptr_t>(a.cpu), a.bytes,
                    reinterpret_cast<uintptr_t>(b.cpu), b.bytes);
}
// __intel_ring_space(): head must not share the tail's cacheline.
uint32_t ringSpace(uint32_t head, uint32_t tail, uint32_t size) {
    return (head - tail - Lrc::CachelineBytes) & (size - 1);
}
bool ggttLease(const Lease &lease, LeaseKind kind, uint64_t epoch) {
    return lease.kind == kind && lease.deviceEpoch == epoch && lease.cpu && lease.address &&
           !(lease.address & (PageBytes - 1)) && lease.bytes && !(lease.bytes & (PageBytes - 1)) &&
           lease.address < GgttLimit && lease.bytes <= GgttLimit - lease.address &&
           reinterpret_cast<uintptr_t>(lease.cpu) <= UINTPTR_MAX - lease.bytes;
}

} // namespace

bool Engine::forcewake(uint32_t domains) const {
    return platform_.forcewake.held &&
           platform_.forcewake.held(platform_.forcewake.context, platform_.deviceEpoch, domains);
}

bool Engine::read(uint32_t offset, uint32_t &value) {
    // Offsets are formed only from Reg/Lrc constants; this keeps the window.
    if (!inWindow(offset)) { ++readFailures_; value = AllOnes; return false; }
    value = platform_.mmio.read32(platform_.mmio.context, offset);
    // No register read by this unit can legitimately be all-ones.
    if (value == AllOnes) { ++readFailures_; return false; }
    return true;
}

void Engine::write(uint32_t offset, uint32_t value) {
    if (!inWindow(offset)) return;
    platform_.mmio.write32(platform_.mmio.context, offset, value);
}

void Engine::fence(Fence kind) const { platform_.mmio.fence(platform_.mmio.context, kind); }
void Engine::delay(uint32_t microseconds) const {
    platform_.mmio.delayMicroseconds(platform_.mmio.context, microseconds);
}

bool Engine::holds(const Lease &lease) const {
    return lease.deviceEpoch == platform_.deviceEpoch &&
           platform_.leases.holds(platform_.leases.context, lease);
}
bool Engine::sync(const Lease &lease, CacheOp op) const {
    return platform_.leases.synchronize(platform_.leases.context, lease, op);
}
bool Engine::staticLeasesHeld() const {
    return holds(bindings_.statusPage) && holds(bindings_.context) && holds(bindings_.ring) &&
           holds(bindings_.ppgttRoot);
}

uint32_t Engine::hwspDword(uint32_t index) const {
    return loadDword(bindings_.statusPage.cpu + index * 4);
}
void Engine::setHwspDword(uint32_t index, uint32_t value) {
    storeDword(bindings_.statusPage.cpu + index * 4, value);
}
uint64_t Engine::hwspQword(uint32_t dwordIndex) const {
    return uint64_t(hwspDword(dwordIndex)) | uint64_t(hwspDword(dwordIndex + 1)) << 32;
}
void Engine::setHwspQword(uint32_t dwordIndex, uint64_t value) {
    setHwspDword(dwordIndex, uint32_t(value));
    setHwspDword(dwordIndex + 1, uint32_t(value >> 32));
}

Status Engine::attach(const Platform &platform, const Bindings &bindings, uint32_t initialSeqno) {
    if (state_ != EngineState::Detached) return Status::InvalidState;
    const MmioPort &mmio = platform.mmio;
    const LeaseAuthority &leases = platform.leases;
    if (!mmio.read32 || !mmio.write32 || !mmio.delayMicroseconds || !mmio.fence ||
        !platform.forcewake.held || !leases.holds || !leases.retain || !leases.release ||
        !leases.synchronize)
        return Status::Unavailable;
    if (!platform.deviceEpoch || initialSeqno == AllOnes) return Status::InvalidArgument;
    Lrc::EngineInfo info;
    if (Lrc::describeEngine(Lrc::Engine::Bcs0, info) != Lrc::Status::Ok ||
        info.mmioBase != MmioBase || info.renderState)
        return Status::Encoding;
    const uint64_t epoch = platform.deviceEpoch;
    const Lease &hwsp = bindings.statusPage, &context = bindings.context, &ring = bindings.ring,
                &root = bindings.ppgttRoot;
    if (!ggttLease(hwsp, LeaseKind::StatusPage, epoch) || hwsp.bytes != PageBytes ||
        !ggttLease(context, LeaseKind::Context, epoch) || context.bytes != info.contextBytes ||
        !ggttLease(ring, LeaseKind::Ring, epoch) || !powerOfTwo(ring.bytes) ||
        ring.bytes > RingMaxBytes)
        return Status::InvalidArgument;
    // The PML4 is a DMA address below the GLK 39-bit limit (px_dma(ppgtt->pd)).
    if (root.kind != LeaseKind::PpgttRoot || root.deviceEpoch != epoch || !root.address ||
        (root.address & (PageBytes - 1)) || root.bytes != PageBytes ||
        root.address > Lrc::DmaAddressLimit - PageBytes)
        return Status::InvalidArgument;
    if (overlaps(hwsp.address, hwsp.bytes, context.address, context.bytes) ||
        overlaps(hwsp.address, hwsp.bytes, ring.address, ring.bytes) ||
        overlaps(context.address, context.bytes, ring.address, ring.bytes) ||
        cpuOverlaps(hwsp, context) || cpuOverlaps(hwsp, ring) || cpuOverlaps(context, ring) ||
        cpuOverlaps(root, hwsp) || cpuOverlaps(root, context) || cpuOverlaps(root, ring))
        return Status::InvalidArgument;
    platform_ = platform;
    bindings_ = bindings;
    if (!staticLeasesHeld()) { platform_ = {}; bindings_ = {}; return Status::Ownership; }
    state_ = EngineState::Attached;
    engineEpoch_ = 1;
    seqno_ = initialSeqno;
    emit_ = head_ = 0;
    csbHead_ = Lrc::CsbEntries - 1;
    return Status::Ok;
}

// reset_csb_pointers()
Status Engine::resetCsbPointers() {
    Lrc::CsbReset plan;
    if (Lrc::planCsbReset(Lrc::Engine::Bcs0, plan) != Lrc::Status::Ok ||
        plan.statusPointer.offset != Reg::ContextStatusPtr)
        return Status::Encoding;
    setHwspDword(HwspPreemptDword, 0); // ring_set_paused(engine, 0)
    uint32_t posting;
    write(plan.statusPointer.offset, plan.statusPointer.value);
    if (!read(Reg::ContextStatusPtr, posting)) return deviceLost();
    csbHead_ = plan.head;
    setHwspDword(HwspCsbWriteDword, plan.writePointerDword);
    fence(Fence::Store); // wmb()
    for (uint32_t i = 0; i < Lrc::CsbEntries; ++i)
        setHwspQword(HwspCsbBufferDword + 2 * i, plan.entryFill);
    if (!sync(bindings_.statusPage, CacheOp::ForDevice)) return Status::Ownership;
    write(plan.statusPointer.offset, plan.statusPointer.value);
    if (!read(Reg::ContextStatusPtr, posting)) return deviceLost();
    if (hwspDword(HwspCsbWriteDword) != plan.writePointerDword) return Status::EvidenceMismatch;
    return Status::Ok;
}

// lrc_pin(): lrc_init_state() for a fresh context, then lrc_update_regs().
Status Engine::pinContext() {
    const Lease &context = bindings_.context, &ring = bindings_.ring;
    const Lrc::ContextImageSpec spec {Lrc::Engine::Bcs0, bindings_.ppgttRoot.address, 0, nullptr};
    if (Lrc::initContextImage(spec, nullptr, 0, context.cpu, context.bytes) != Lrc::Status::Ok)
        return Status::Encoding;
    const Lrc::RingSpec rings {uint32_t(ring.address), uint32_t(ring.bytes), emit_, emit_};
    uint32_t lrca = 0;
    if (Lrc::updateContextRegisters(spec, rings, uint32_t(context.address), context.cpu,
                                    context.bytes, lrca) != Lrc::Status::Ok)
        return Status::Encoding;
    if (!sync(context, CacheOp::ForDevice)) return Status::Ownership;
    submit_ = {lrca, 0};
    head_ = emit_;
    contextPinned_ = true;
    return Status::Ok;
}

// enable_execlists() and enable_error_interrupt().
Status Engine::enableExeclists() {
    if (!forcewake(ForcewakeAllGen9)) return Status::ForcewakeNotHeld;
    const uint32_t hwsp = uint32_t(bindings_.statusPage.address);
    write(Reg::Hwstam, ~0u); // intel_engine_set_hwsp_writemask(engine, ~0u)
    write(Reg::ModeGen7, maskedEnable(Bit::GfxRunListEnable)); // GRAPHICS_VER < 11
    write(Reg::MiMode, maskedDisable(Bit::StopRing));
    write(Reg::HwsPga, hwsp);
    uint32_t pga, esr, mode, mi;
    if (!read(Reg::HwsPga, pga)) return deviceLost(); // ENGINE_POSTING_READ
    write(Reg::Emr, ~0u);
    write(Reg::Eir, ~0u); // clear all existing errors
    if (!read(Reg::Esr, esr)) return deviceLost();
    // Upstream resets the engine here; this unit refuses and requires reset.
    if (esr) { faulted_ = true; state_ = EngineState::Faulted; return Status::EngineInError; }
    write(Reg::Emr, ~Bit::ErrorInstruction);
    if (!read(Reg::ModeGen7, mode) || !read(Reg::MiMode, mi)) return deviceLost();
    if (pga != hwsp || !(mode & Bit::GfxRunListEnable) || (mi & Bit::StopRing)) {
        faulted_ = true; state_ = EngineState::Faulted; return Status::RegisterMismatch;
    }
    return Status::Ok;
}

Status Engine::initialize() {
    if (state_ != EngineState::Attached) return Status::InvalidState;
    if (!forcewake(ForcewakeAllGen9)) return Status::ForcewakeNotHeld;
    if (!staticLeasesHeld()) return Status::Ownership;
    if (!hwspRetained_) {
        // HWS_PGA makes the page a GPU write target until detach.
        if (!platform_.leases.retain(platform_.leases.context, bindings_.statusPage))
            return Status::Ownership;
        hwspRetained_ = true;
    }
    stopProven_ = false; // initialization may enable hardware again
    Status status = resetCsbPointers(); // execlists_sanitize()
    if (status != Status::Ok) return status;
    setHwspDword(HwspSeqnoDword, seqno_); // sanitize_hwsp(): intel_timeline_reset_seqno()
    if (!sync(bindings_.statusPage, CacheOp::ForDevice)) return Status::Ownership;
    if ((status = pinContext()) != Status::Ok) return status;
    if ((status = enableExeclists()) != Status::Ok) return status; // execlists_resume()
    faulted_ = false;
    state_ = EngineState::Ready;
    return Status::Ok;
}

Status Engine::allocateTag(uint32_t &tag) {
    for (uint32_t i = 0; i < MaxTags; ++i) {
        if (!(tagsInUse_ & (1ULL << i))) { tagsInUse_ |= 1ULL << i; tag = i; return Status::Ok; }
    }
    return Status::NoContextTag;
}
void Engine::freeTag(uint32_t tag) { if (tag < MaxTags) tagsInUse_ &= ~(1ULL << tag); }

bool Engine::retainRequestLeases(const Lease &probe) {
    const Lease *leases[] = {&bindings_.ring, &bindings_.context, &bindings_.ppgttRoot, &probe};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!platform_.leases.retain(platform_.leases.context, *leases[i])) {
            // Never published: releasing these holds is an authoritative rejection.
            while (i--) (void)platform_.leases.release(platform_.leases.context, *leases[i]);
            return false;
        }
    }
    return true;
}
void Engine::releaseRequestLeases(const Lease &probe) {
    const Lease *leases[] = {&bindings_.ring, &bindings_.context, &bindings_.ppgttRoot, &probe};
    for (uint32_t i = 0; i < 4; ++i) {
        if (released_ & (1u << i)) continue;
        if (platform_.leases.release(platform_.leases.context, *leases[i])) released_ |= 1u << i;
    }
}

Status Engine::submitStore(const Lease &probe, uint32_t nonce, uint32_t cpuInitial) {
    if (state_ != EngineState::Ready || faulted_ || !contextPinned_) return Status::InvalidState;
    if (request_.phase != RequestPhase::None || unknownCount_ >= MaxUnknown) return Status::Busy;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    const Lease &hwsp = bindings_.statusPage, &context = bindings_.context, &ring = bindings_.ring;
    if (!ggttLease(probe, LeaseKind::ProbeTarget, platform_.deviceEpoch) ||
        overlaps(probe.address, probe.bytes, hwsp.address, hwsp.bytes) ||
        overlaps(probe.address, probe.bytes, context.address, context.bytes) ||
        overlaps(probe.address, probe.bytes, ring.address, ring.bytes) ||
        cpuOverlaps(probe, hwsp) || cpuOverlaps(probe, context) || cpuOverlaps(probe, ring) ||
        cpuOverlaps(probe, bindings_.ppgttRoot))
        return Status::InvalidArgument;
    if (!staticLeasesHeld() || !holds(probe)) return Status::Ownership;

    // Breadcrumb seqno: u32 wrap, never 0 (zeroed page) or all-ones (failed read).
    uint32_t seqno = seqno_ + 1;
    if (seqno == AllOnes) ++seqno;
    if (seqno == 0) seqno = 1;
    const uint32_t size = uint32_t(ring.bytes);
    // intel_ring_begin(): wrap with MI_NOOP padding when the request does not fit.
    uint32_t at = emit_, padding = 0;
    if (size - at < RequestBytes) { padding = size - at; at = 0; }
    if (padding + RequestBytes > ringSpace(head_, emit_, size)) return Status::RingFull;
    const StoreProbeRing shape {{probe.address, nonce, cpuInitial},
                                {AddressSpace::Ggtt, ring.address, ring.bytes},
                                hwsp.address + HwspSeqnoDword * 4, seqno};
    if (checkStoreProbeRing(shape) != MellowNativeGlk::Status::Ok) return Status::Encoding;

    // The previous breadcrumb must still be what the last retirement saw.
    if (!sync(hwsp, CacheOp::ForCpu)) return Status::Ownership;
    if (hwspDword(HwspSeqnoDword) != seqno_) {
        faulted_ = true; state_ = EngineState::Faulted; return Status::EvidenceMismatch;
    }
    uint32_t tag = 0, ccid = 0;
    Status status = allocateTag(tag);
    if (status != Status::Ok) return status;
    if (Lrc::contextIdForTag(tag, ccid) != Lrc::Status::Ok) { freeTag(tag); return Status::Encoding; }
    if (!retainRequestLeases(probe)) { freeTag(tag); return Status::Ownership; }
    released_ = 0;
    auto reject = [&](Status why) {
        releaseRequestLeases(probe);
        released_ = 0;
        freeTag(tag);
        return why;
    };

    // Seal producer-owned bytes while every consumer lease is held.
    storeDword(probe.cpu, cpuInitial);
    for (uint32_t i = 0; i < padding; ++i) ring.cpu[emit_ + i] = 0;
    CommandWriter writer;
    if (writer.begin(Generation::Gen9, MellowNativeGlk::Engine::Bcs0, Site::Ring, ring.cpu + at,
                     RequestBytes) != MellowNativeGlk::Status::Ok ||
        writer.appendStoreProbeRing(shape) != MellowNativeGlk::Status::Ok ||
        writer.bytesWritten() != RequestBytes)
        return reject(Status::Encoding);
    const uint32_t tail = at + StoreProbeRingTailBytes;                   // rq->tail
    const uint32_t waTail = (at + StoreProbeRingWaTailBytes) & (size - 1); // rq->wa_tail

    // execlists_update_context()
    Lrc::SubmitState state {submit_.lrca, ccid};
    uint64_t descriptor = 0;
    if (Lrc::prepareSubmission(Lrc::Engine::Bcs0, size, head_, tail, Lrc::Priority::Normal, state,
                               context.cpu, context.bytes, descriptor) != Lrc::Status::Ok)
        return reject(Status::Encoding);
    if (!sync(probe, CacheOp::ForDevice) || !sync(ring, CacheOp::ForDevice) ||
        !sync(context, CacheOp::ForDevice)) {
        faulted_ = true; state_ = EngineState::Faulted;
        return reject(Status::Ownership);
    }
    fence(Fence::Store); // wmb(): context image complete before the UC ELSP write
    submit_.lrca = state.lrca;
    Lrc::ElspPlan plan;
    if (Lrc::planElspSubmission(Lrc::Engine::Bcs0, descriptor, 0, plan) != Lrc::Status::Ok)
        return reject(Status::Encoding);
    if (!forcewake(ForcewakeGt) || !staticLeasesHeld() || !holds(probe))
        return reject(Status::Ownership);

    request_ = {};
    request_.phase = RequestPhase::Pending;
    request_.sequence = ++sequence_;
    request_.deviceEpoch = platform_.deviceEpoch;
    request_.engineEpoch = engineEpoch_;
    request_.descriptor = descriptor;
    request_.contextId = ccid;
    request_.tag = tag;
    request_.seqno = seqno;
    request_.previousSeqno = seqno_;
    request_.nonce = nonce;
    request_.cpuInitial = cpuInitial;
    request_.ringOffset = at;
    request_.tail = tail;
    request_.waTail = waTail;
    request_.probe = probe;
    emit_ = waTail;
    // execlists_submit_ports(): port 1 (empty) then port 0, upper then lower.
    for (const Lrc::MmioWrite &w : plan.writes) write(w.offset, w.value);
    return Status::Ok;
}

bool Engine::unknownContext(uint32_t ccid) const {
    for (uint32_t i = 0; i < unknownCount_; ++i)
        if (unknown_[i].contextId == ccid) return true;
    return false;
}

void Engine::makeUnknown() {
    if (request_.phase != RequestPhase::Pending && request_.phase != RequestPhase::Active) return;
    // Submissions stop before MaxUnknown, so the in-flight record always fits.
    if (unknownCount_ < MaxUnknown) {
        unknown_[unknownCount_] = request_;
        unknown_[unknownCount_].phase = RequestPhase::Unknown;
        ++unknownCount_;
    }
    // Job holds and the context tag stay retained. The breadcrumb advances so
    // a late write of this seqno can never match a later request.
    seqno_ = request_.seqno;
    request_ = {};
}

Status Engine::deviceLost() {
    makeUnknown();
    state_ = EngineState::DeviceLost;
    return Status::DeviceLost;
}

// csb_read() and wa_csb_read(): an all-ones HWSP entry is re-read for 10 us,
// then the GEN8_EXECLISTS_STATUS_BUF MMIO copy is used. The entry is
// consumed by writing all-ones back.
Status Engine::readCsbEntry(uint32_t index, uint64_t &entry) {
    const uint32_t dword = HwspCsbBufferDword + 2 * index;
    uint64_t value = hwspQword(dword);
    for (uint32_t n = 0; value == ~0ULL && n < CsbRetryPolls; ++n) {
        delay(1);
        if (!sync(bindings_.statusPage, CacheOp::ForCpu)) return Status::Ownership;
        value = hwspQword(dword);
    }
    if (value == ~0ULL) {
        uint32_t offset = 0, low = 0, high = 0, sentinel = 0;
        if (Lrc::csbEntryMmioOffset(Lrc::Engine::Bcs0, index, offset) != Lrc::Status::Ok)
            return Status::Encoding;
        if (!read(offset, low) || !read(offset + 4, high)) {
            if (!read(Reg::MiMode, sentinel)) return deviceLost();
            makeUnknown();
            faulted_ = true; state_ = EngineState::Faulted;
            return Status::CsbFault;
        }
        value = uint64_t(high) << 32 | low;
    }
    setHwspQword(dword, ~0ULL);
    entry = value;
    return Status::Ok;
}

// process_csb() for GRAPHICS_VER < 11 with one context in port 0.
Status Engine::processCsb(bool &progress) {
    progress = false;
    auto fault = [&](Status why) {
        makeUnknown();
        faulted_ = true;
        if (state_ == EngineState::Ready) state_ = EngineState::Faulted;
        return why;
    };
    if (!staticLeasesHeld() || !holds(request_.probe)) return fault(Status::Ownership);
    if (!sync(bindings_.statusPage, CacheOp::ForCpu)) return fault(Status::Ownership);
    // READ_ONCE(*csb_write) into a u8, as process_csb() does.
    const uint32_t writePointer = hwspDword(HwspCsbWriteDword);
    if (uint8_t(writePointer) == csbHead_) return Status::Ok;
    Lrc::CsbWalk walk;
    if (Lrc::walkCsb(uint8_t(csbHead_), writePointer, walk) != Lrc::Status::Ok)
        return fault(Status::CsbFault);
    csbHead_ = walk.newHead;
    fence(Fence::Load); // rmb(): write pointer before entries
    for (uint32_t i = 0; i < walk.count; ++i) {
        uint64_t entry = 0;
        const Status entryStatus = readCsbEntry(walk.indices[i], entry);
        if (entryStatus != Status::Ok) return entryStatus;
        Lrc::CsbEvent event;
        if (Lrc::decodeCsbEntry(entry, event) != Lrc::Status::Ok) return fault(Status::CsbFault);
        progress = true;
        ++request_.evidence.csbEvents;
        if (event.contextId && unknownContext(event.contextId)) return fault(Status::StaleIdentity);
        if (event.promote) {
            if (request_.phase != RequestPhase::Pending) return fault(Status::CsbFault);
            setHwspDword(HwspPreemptDword, 0); // ring_set_paused(engine, 0)
            request_.phase = RequestPhase::Active;
            request_.evidence.promoted = true;
            uint32_t posting;
            if (!read(Reg::ContextStatusPtr, posting)) return deviceLost();
        } else {
            if (request_.phase != RequestPhase::Active || request_.evidence.csbCompleted)
                return fault(Status::CsbFault);
            const Lrc::Status identity = Lrc::checkCsbContext(event, request_.contextId);
            if (identity != Lrc::Status::Ok) return fault(Status::StaleIdentity);
            request_.evidence.csbCompleted = true;
            request_.evidence.completionEntry = entry;
        }
    }
    // drm_clflush_virt_range(): publish the consumed all-ones markers.
    if (!sync(bindings_.statusPage, CacheOp::ForDevice)) return fault(Status::Ownership);
    return Status::Ok;
}

Status Engine::correlate() {
    RequestRecord &r = request_;
    auto fault = [&](Status why) {
        makeUnknown();
        faulted_ = true;
        if (state_ == EngineState::Ready) state_ = EngineState::Faulted;
        return why;
    };
    if (r.engineEpoch != engineEpoch_ || r.deviceEpoch != platform_.deviceEpoch)
        return fault(Status::StaleIdentity);
    if (!sync(bindings_.statusPage, CacheOp::ForCpu) || !sync(r.probe, CacheOp::ForCpu) ||
        !sync(bindings_.context, CacheOp::ForCpu))
        return fault(Status::Ownership);
    Evidence &e = r.evidence;
    e.breadcrumbRead = hwspDword(HwspSeqnoDword);
    e.breadcrumb = e.breadcrumbRead == r.seqno;
    uint8_t observed[4];
    for (uint32_t i = 0; i < 4; ++i) observed[i] = static_cast<const volatile uint8_t *>(r.probe.cpu)[i];
    e.probe = classifyProbeReading({r.probe.address, r.nonce, r.cpuInitial}, observed);
    e.nonce = e.probe == ProbeReading::NonceObserved;
    e.savedHeadRead = loadDword(bindings_.context.cpu + Lrc::StateOffset + ContextRingHeadDword * 4);
    e.savedHead = (e.savedHeadRead & Bit::HeadAddr) == r.tail;
    e.leasesHeld = staticLeasesHeld() && holds(r.probe);
    if (!e.leasesHeld) return fault(Status::Ownership);
    if (e.breadcrumbRead != r.seqno && e.breadcrumbRead != r.previousSeqno)
        return fault(Status::EvidenceMismatch);
    if (e.probe == ProbeReading::Foreign || e.probe == ProbeReading::Invalid)
        return fault(Status::EvidenceMismatch);
    if (e.promoted && e.csbCompleted && e.breadcrumb && e.nonce && e.savedHead) {
        r.phase = RequestPhase::Completed;
        return Status::Ok;
    }
    return Status::Pending; // awaiting visibility; the caller bounds polls
}

Status Engine::poll() {
    if (request_.phase == RequestPhase::Completed) return Status::Ok;
    if (request_.phase != RequestPhase::Pending && request_.phase != RequestPhase::Active)
        return Status::InvalidState;
    if (state_ != EngineState::Ready && state_ != EngineState::Stopped) return Status::InvalidState;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    bool progress = false;
    const Status status = processCsb(progress);
    if (status != Status::Ok) return status;
    return request_.evidence.csbCompleted ? correlate() : Status::Pending;
}

Status Engine::drain(uint32_t polls, uint32_t delayMicroseconds) {
    for (uint32_t i = 0; i < polls; ++i) {
        const Status status = poll();
        if (status != Status::Pending) return status;
        delay(delayMicroseconds);
    }
    return Status::TimedOut;
}

Status Engine::retireCompleted() {
    if (request_.phase != RequestPhase::Completed) return Status::InvalidState;
    releaseRequestLeases(request_.probe);
    if (released_ != 0xF) return Status::Ownership; // retryable; nothing else changes
    head_ = request_.tail; // the saved RING_HEAD proved consumption up to tail
    seqno_ = request_.seqno;
    freeTag(request_.tag);
    request_ = {};
    released_ = 0;
    return Status::Ok;
}

Status Engine::abandon() {
    if (request_.phase != RequestPhase::Pending && request_.phase != RequestPhase::Active)
        return Status::InvalidState;
    makeUnknown();
    faulted_ = true;
    if (state_ == EngineState::Ready) state_ = EngineState::Faulted;
    return Status::Ok;
}

// __intel_engine_stop_cs() and intel_engine_stop_cs().
Status Engine::stopCs() {
    write(Reg::MiMode, maskedEnable(Bit::StopRing));
    uint32_t mode = 0;
    bool idle = false;
    for (uint32_t n = 0; !idle && n < StopFastPolls; ++n) {
        if (!read(Reg::MiMode, mode)) return deviceLost();
        if (mode & Bit::ModeIdle) idle = true; else delay(1);
    }
    for (uint32_t n = 0; !idle && n < StopSlowPolls; ++n) {
        delay(1000);
        if (!read(Reg::MiMode, mode)) return deviceLost();
        idle = mode & Bit::ModeIdle;
    }
    if (!read(Reg::MiMode, mode)) return deviceLost(); // final posting read
    if (idle) return Status::Ok;
    uint32_t head = 0, tail = 0;
    if (!read(Reg::RingHead, head) || !read(Reg::RingTail, tail)) return deviceLost();
    stopTail_ = tail & Bit::TailAddr;
    // "Sometimes we observe that the idle flag is not set even though the
    // ring is empty."
    return (head & Bit::HeadAddr) == (tail & Bit::TailAddr) ? Status::Ok : Status::TimedOut;
}

Status Engine::stop() {
    if (state_ == EngineState::Stopped && stopProven_) return Status::Ok;
    if (state_ != EngineState::Ready && state_ != EngineState::Faulted &&
        state_ != EngineState::Stopped && !(state_ == EngineState::Attached && hwspRetained_))
        return Status::InvalidState;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    const Status status = stopCs();
    if (status == Status::DeviceLost) return status;
    stopProven_ = status == Status::Ok;
    state_ = EngineState::Stopped; // STOP_RING stays requested even on timeout
    return status;
}

Status Engine::cancelStop() {
    if (state_ != EngineState::Stopped || faulted_ || !contextPinned_) return Status::InvalidState;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    write(Reg::MiMode, maskedDisable(Bit::StopRing)); // intel_engine_cancel_stop_cs()
    stopProven_ = false;
    state_ = EngineState::Ready;
    return Status::Ok;
}

// execlists_reset_prepare() + reset_csb() + gen8_engine_reset_prepare().
Status Engine::prepareReset() {
    if (state_ != EngineState::Ready && state_ != EngineState::Stopped &&
        state_ != EngineState::Faulted)
        return Status::InvalidState;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    setHwspDword(HwspPreemptDword, 1); // ring_set_paused(engine, 1)
    fence(Fence::Store);
    if (!sync(bindings_.statusPage, CacheOp::ForDevice)) return Status::Ownership;
    Status status = stopCs(); // upstream continues after a stop timeout
    if (status == Status::DeviceLost) return status;
    stopProven_ = status == Status::Ok;
    state_ = EngineState::Stopped;
    if (request_.phase == RequestPhase::Pending || request_.phase == RequestPhase::Active) {
        bool progress = false;
        status = processCsb(progress); // events written before the reset still count
        if (status == Status::DeviceLost) return status;
        if (status == Status::Ok && request_.evidence.csbCompleted) {
            status = correlate();
            if (status == Status::DeviceLost) return status;
        }
    }
    uint32_t ack = 0, request = 0, mask = 0, expect = 0;
    if (!read(Reg::ResetCtl, ack)) return deviceLost();
    if (ack & Bit::ResetCatError) {
        request = mask = Bit::ResetCatError;
        expect = 0;
    } else if (!(ack & Bit::ResetReady)) {
        request = Bit::ResetRequest;
        mask = expect = Bit::ResetReady;
    } else {
        state_ = EngineState::ResetPrepared;
        return Status::Ok;
    }
    write(Reg::ResetCtl, maskedEnable(request));
    for (uint32_t n = 0; n < ResetAckPolls; ++n) {
        if (!read(Reg::ResetCtl, ack)) return deviceLost();
        if ((ack & mask) == expect) { state_ = EngineState::ResetPrepared; return Status::Ok; }
        delay(1);
    }
    write(Reg::ResetCtl, maskedDisable(Bit::ResetRequest)); // gen8_engine_reset_cancel()
    return Status::ResetNotReady;
}

Status Engine::resetEngine() {
    if (state_ != EngineState::ResetPrepared) return Status::InvalidState;
    if (!platform_.reset.resetEngine) return Status::Unavailable;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    if (!platform_.reset.resetEngine(platform_.reset.context, platform_.deviceEpoch))
        return Status::Unavailable;
    write(Reg::ResetCtl, maskedDisable(Bit::ResetRequest)); // gen8_engine_reset_cancel()
    makeUnknown(); // unproven work keeps every pin; nothing is replayed
    stopProven_ = true; // native reset completion, not just RESET_READY
    ++engineEpoch_;
    contextPinned_ = false; // the register state is rebuilt by initialize()
    head_ = emit_;
    state_ = EngineState::Attached;
    return Status::Ok;
}

Status Engine::cancelReset() {
    if (state_ != EngineState::ResetPrepared) return Status::InvalidState;
    if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
    write(Reg::ResetCtl, maskedDisable(Bit::ResetRequest));
    state_ = EngineState::Stopped;
    return Status::Ok;
}

Status Engine::detach() {
    if (state_ == EngineState::Detached) return Status::InvalidState;
    if (request_.phase != RequestPhase::None || unknownCount_ || state_ == EngineState::DeviceLost)
        return Status::Busy;
    if (state_ == EngineState::Ready || state_ == EngineState::Faulted) return Status::Busy;
    if (hwspRetained_) {
        if (!stopProven_) return Status::Busy;
        if (!forcewake(ForcewakeGt)) return Status::ForcewakeNotHeld;
        write(Reg::Hwstam, ~0u); // cleanup_status_page()
        if (!platform_.leases.release(platform_.leases.context, bindings_.statusPage))
            return Status::Ownership;
        hwspRetained_ = false;
    }
    platform_ = {};
    bindings_ = {};
    submit_ = {};
    request_ = {};
    state_ = EngineState::Detached;
    engineEpoch_ = sequence_ = tagsInUse_ = 0;
    seqno_ = emit_ = head_ = csbHead_ = interrupts_ = stopTail_ = readFailures_ = released_ = 0;
    contextPinned_ = faulted_ = stopProven_ = false;
    return Status::Ok;
}

} // namespace MellowNativeGlk::Bcs

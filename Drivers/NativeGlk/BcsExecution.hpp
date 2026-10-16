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
// Exact commit/path/blob/sha256/symbol references: BcsExecution.provenance.json.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "CommandEncoding.hpp"
#include "LrcImage.hpp"

namespace MellowNativeGlk::Bcs {

// Gemini Lake (Gen9 LP) BCS0 execlists engine bodies transcribed from Linux
// torvalds/linux@0d9ff90a5422cc7509258aaaba1e7481df4d332a:
//   initialize    execlists_sanitize/reset_csb_pointers/sanitize_hwsp, lrc pin
//                 (lrc_init_state + lrc_update_regs), execlists_resume ->
//                 enable_execlists + enable_error_interrupt
//   submitStore   intel_ring_begin wrap, the CommandEncoding store probe
//                 request, execlists_update_context, execlists_submit_ports
//                 (no ELSQ: port 1 then port 0, upper then lower dword)
//   poll          process_csb/csb_read/wa_csb_read/gen8_csb_parse, then an
//                 original Mellow completion correlation (below)
//   stop          __intel_engine_stop_cs/intel_engine_stop_cs/cancel_stop_cs
//   reset         execlists_reset_prepare (ring_set_paused, stop_cs, reset_csb),
//                 gen8_engine_reset_prepare/cancel; GEN6_GDRST itself belongs
//                 to the GT reset owner and is only reached via ResetAuthority.
//
// Every register effect goes through the injected MmioPort, restricted here to
// the BCS0 engine window [0x22000, 0x23000). No display, GT interrupt, GGTT PTE,
// forcewake or GDRST register is formed. Nothing in this unit enables bus
// mastering, maps a BAR, allocates or pins memory, or starts automatically.
//
// Completion (an original Mellow rule, stricter than i915): a request is
// Completed only when ALL of these hold for one submission, sampled after the
// lease owner's ForCpu synchronization:
//   1. a promote CSB event moved it from pending to active, then a non-promote
//      CSB event retired port 0 carrying its assigned context ID;
//   2. the engine status page seqno dword (I915_GEM_HWS_SEQNO) equals the
//      request's breadcrumb, written by its MI_FLUSH_DW post-sync;
//   3. the probe target holds the GPU-only nonce (never in CPU-written bytes);
//   4. the saved context image CTX_RING_HEAD equals the submitted tail;
//   5. every lease is still held for the same device epoch, and no reset
//      changed the engine epoch since submission.
// An interrupt, a timeout or any single matching memory value never completes
// a request. Contradicting evidence makes it Unknown: its leases, job holds and
// context tag stay retained until the device owner quarantines them.

// --- Registers: absolute GTTMMADR offsets (intel_engine_regs.h, i915_reg.h,
// intel_lrc_reg.h). BLT_RING_BASE is __engine_mmio_base() for GRAPHICS_VER 9.
constexpr uint32_t MmioBase = 0x22000, WindowBytes = 0x1000;
namespace Reg {
constexpr uint32_t RingTail = MmioBase + 0x30, RingHead = MmioBase + 0x34,
    RingStart = MmioBase + 0x38, RingCtl = MmioBase + 0x3c, ActhdUdw = MmioBase + 0x5c,
    Acthd = MmioBase + 0x74, HwsPga = MmioBase + 0x80, Hwstam = MmioBase + 0x98,
    MiMode = MmioBase + 0x9c, Eir = MmioBase + 0xb0, Emr = MmioBase + 0xb4,
    Esr = MmioBase + 0xb8, ResetCtl = MmioBase + 0xd0, Elsp = MmioBase + 0x230,
    ExeclistStatusLo = MmioBase + 0x234, ExeclistStatusHi = MmioBase + 0x238,
    ModeGen7 = MmioBase + 0x29c, ExeclistsStatusBuf = MmioBase + 0x370,
    ContextStatusPtr = MmioBase + 0x3a0;
}
namespace Bit {
constexpr uint32_t HeadAddr = 0x001FFFFC, TailAddr = 0x001FFFF8, ModeIdle = 1u << 9,
    StopRing = 1u << 8, GfxRunListEnable = 1u << 15, ErrorInstruction = 1u << 0,
    ResetRequest = 1u << 0, ResetReady = 1u << 1, ResetCatError = 1u << 2;
}
// Masked register writes: the upper half selects the bits written.
constexpr uint32_t maskedEnable(uint32_t bits) { return bits << 16 | bits; }
constexpr uint32_t maskedDisable(uint32_t bits) { return bits << 16; }
constexpr bool inWindow(uint32_t offset) {
    return offset >= MmioBase && offset < MmioBase + WindowBytes && !(offset & 3);
}

// Engine status page dwords (intel_engine.h).
constexpr uint32_t HwspCsbBufferDword = 0x10, HwspCsbWriteDword = 0x1f,
    HwspPreemptDword = 0x32, HwspSeqnoDword = 0x40;
// intel_lrc_reg.h CTX_RING_HEAD, a dword index into the register state page.
constexpr uint32_t ContextRingHeadDword = 0x04 + 1;

enum class Status : uint8_t {
    Ok,
    Pending,          // nothing proven yet; never a completion
    Unavailable,      // a native authority/binding prerequisite is missing
    InvalidArgument,
    InvalidState,
    Ownership,        // a lease is not (or no longer) held by its owner
    ForcewakeNotHeld,
    DeviceLost,       // an all-ones read from a register that cannot read so
    RegisterMismatch, // readback after enable differs from what was written
    EngineInError,    // RING_ESR nonzero after enable_error_interrupt
    TimedOut,         // bounded wait expired; proves nothing
    Busy,
    CsbFault,         // event without pending/active port (ERROR_CSB)
    StaleIdentity,    // CSB context ID is not the request's assignment
    EvidenceMismatch, // contradicting GPU-visible evidence
    Encoding,         // CommandEncoding/LrcImage refused the inputs
    RingFull,
    NoContextTag,
    ResetNotReady
};

enum class Fence : uint8_t { Full, Store, Load };

// Native BAR owner: 32-bit uncached register access inside the mapped
// register window, a busy-wait delay and x86 CPU ordering (wmb = sfence,
// rmb = lfence, mb = mfence).
struct MmioPort {
    void *context {};
    uint32_t (*read32)(void *, uint32_t offset) {};
    void (*write32)(void *, uint32_t offset, uint32_t value) {};
    void (*delayMicroseconds)(void *, uint32_t) {};
    void (*fence)(void *, Fence) {};
};

// intel_uncore.h forcewake_domain_id bits initialized for GRAPHICS_VER 9.
// BCS0 registers lie in __gen9_fw_ranges 0x1ea00-0x243ff (FORCEWAKE_GT);
// enable_execlists() asserts FORCEWAKE_ALL. Upstream's ELSP path (no ELSQ,
// engine->fw_domain 0) takes no reference; this unit requires the GT domain
// for every BCS0 register access, which is stricter.
enum : uint32_t { ForcewakeRender = 1u << 0, ForcewakeGt = 1u << 1, ForcewakeMedia = 1u << 2 };
constexpr uint32_t ForcewakeAllGen9 = ForcewakeRender | ForcewakeGt | ForcewakeMedia;
struct ForcewakeAuthority {
    void *context {};
    // True only while the forcewake owner holds acked references on every
    // requested domain for this device epoch.
    bool (*held)(void *, uint64_t deviceEpoch, uint32_t domains) {};
};

// GT reset owner (GEN6_GDRST, outside this unit's window). True only when
// the owner observed completion of a BCS0 engine reset for this epoch.
struct ResetAuthority {
    void *context {};
    bool (*resetEngine)(void *, uint64_t deviceEpoch) {};
};

// Typed bindings from native owners. Address is a GGTT offset for every kind
// except PpgttRoot, whose address is the PML4 DMA address. The engine never
// trusts these fields alone: every effect re-asks the authority.
enum class LeaseKind : uint8_t { StatusPage = 1, Context, Ring, ProbeTarget, PpgttRoot };
enum class CacheOp : uint8_t { ForDevice, ForCpu };
struct Lease {
    LeaseKind kind {};
    uint64_t owner {}, slot {}, generation {}, deviceEpoch {};
    uint64_t address {}, bytes {};
    uint8_t *cpu {};
};
struct LeaseAuthority {
    void *context {};
    bool (*holds)(void *, const Lease &) {};   // exact live binding, same epoch
    bool (*retain)(void *, const Lease &) {};  // job hold before publication
    bool (*release)(void *, const Lease &) {}; // only after proven completion
    bool (*synchronize)(void *, const Lease &, CacheOp) {};
};

struct Platform {
    uint64_t deviceEpoch {};
    MmioPort mmio {};
    ForcewakeAuthority forcewake {};
    ResetAuthority reset {};
    LeaseAuthority leases {};
};
struct Bindings {
    Lease statusPage {}, context {}, ring {}, ppgttRoot {};
};

enum class EngineState : uint8_t {
    Detached, Attached, Ready, Stopped, ResetPrepared, Faulted, DeviceLost
};
enum class RequestPhase : uint8_t { None, Pending, Active, Completed, Unknown };

struct Evidence {
    bool promoted {}, csbCompleted {}, breadcrumb {}, nonce {}, savedHead {}, leasesHeld {};
    uint64_t completionEntry {};
    uint32_t breadcrumbRead {}, savedHeadRead {}, csbEvents {};
    ProbeReading probe {ProbeReading::Invalid};
};
struct RequestRecord {
    RequestPhase phase {RequestPhase::None};
    uint64_t sequence {}, deviceEpoch {}, engineEpoch {};
    uint64_t descriptor {};
    uint32_t contextId {}, tag {}, seqno {}, previousSeqno {}, nonce {}, cpuInitial {};
    uint32_t ringOffset {}, tail {}, waTail {};
    Lease probe {};
    Evidence evidence {};
};

class Engine {
public:
    static constexpr uint32_t MaxUnknown = 8;
    static constexpr uint32_t MaxTags = 63; // Lrc::contextIdForTag
    Engine() = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Validates authorities and lease geometry; no register or memory effect.
    Status attach(const Platform &, const Bindings &, uint32_t initialSeqno);
    // Requires FORCEWAKE_ALL. Retains the status page for the engine lifetime.
    Status initialize();
    // Builds, seals and publishes one BCS0 GGTT store request on port 0.
    Status submitStore(const Lease &probeTarget, uint32_t nonce, uint32_t cpuInitial);
    // Processes CSB events and correlates evidence. Ok only when Completed.
    Status poll();
    // Bounded poll loop; expiry returns TimedOut and proves nothing.
    Status drain(uint32_t polls, uint32_t delayMicroseconds);
    // Interrupt delivery only counts; it never advances a request.
    void noteUserInterrupt() { ++interrupts_; }
    // Releases the job holds of a Completed request.
    Status retireCompleted();
    // Moves a request that cannot be proven to Unknown, keeping it pinned.
    Status abandon();
    Status stop();
    Status cancelStop();
    Status prepareReset();
    Status resetEngine();
    Status cancelReset();
    // Requires no in-flight or Unknown request; masks HWSP writes first.
    Status detach();

    EngineState state() const { return state_; }
    const RequestRecord &request() const { return request_; }
    uint32_t unknownCount() const { return unknownCount_; }
    const RequestRecord &unknown(uint32_t index) const { return unknown_[index < MaxUnknown ? index : 0]; }
    uint64_t engineEpoch() const { return engineEpoch_; }
    uint32_t seqno() const { return seqno_; }
    uint32_t csbHead() const { return csbHead_; }
    uint32_t ringEmit() const { return emit_; }
    uint32_t ringHead() const { return head_; }
    uint32_t interrupts() const { return interrupts_; }
    uint32_t lastStopTail() const { return stopTail_; }
    uint32_t readFailures() const { return readFailures_; }

private:
    Platform platform_ {};
    Bindings bindings_ {};
    RequestRecord request_ {};
    RequestRecord unknown_[MaxUnknown] {};
    EngineState state_ {EngineState::Detached};
    Lrc::SubmitState submit_ {};
    uint64_t engineEpoch_ {}, sequence_ {};
    uint64_t tagsInUse_ {};
    uint32_t unknownCount_ {}, seqno_ {}, emit_ {}, head_ {}, csbHead_ {};
    uint32_t interrupts_ {}, stopTail_ {}, readFailures_ {}, released_ {};
    bool hwspRetained_ {}, contextPinned_ {}, faulted_ {}, stopProven_ {};

    bool forcewake(uint32_t domains) const;
    bool read(uint32_t offset, uint32_t &value);
    void write(uint32_t offset, uint32_t value);
    void fence(Fence) const;
    void delay(uint32_t microseconds) const;
    bool holds(const Lease &) const;
    bool sync(const Lease &, CacheOp) const;
    bool staticLeasesHeld() const;
    uint32_t hwspDword(uint32_t index) const;
    void setHwspDword(uint32_t index, uint32_t value);
    uint64_t hwspQword(uint32_t dwordIndex) const;
    void setHwspQword(uint32_t dwordIndex, uint64_t value);
    Status resetCsbPointers();
    Status pinContext();
    Status enableExeclists();
    Status allocateTag(uint32_t &tag);
    void freeTag(uint32_t tag);
    bool retainRequestLeases(const Lease &probe);
    void releaseRequestLeases(const Lease &probe);
    Status processCsb(bool &progress);
    Status readCsbEntry(uint32_t index, uint64_t &entry);
    Status correlate();
    bool unknownContext(uint32_t contextId) const;
    void makeUnknown();
    Status deviceLost();
    Status stopCs();
};

} // namespace MellowNativeGlk::Bcs

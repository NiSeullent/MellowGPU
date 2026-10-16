// SPDX-License-Identifier: MIT
// Copyright © 2014 Intel Corporation
// Copyright © 2014-2018 Intel Corporation
// Copyright © 2016 Intel Corporation
// Copyright © 2019 Intel Corporation
// Copyright © 2020 Intel Corporation
// Copyright © 2022 Intel Corporation
// Copyright © 2003-2018 Intel Corporation
// Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas. All Rights Reserved.
// Copyright (c) 2026 Mellow contributors.
// Permission text is retained in LrcImage.cpp. Exact source provenance,
// limitations and Claude/Codex contribution scope: LRC-IMPLEMENTATION-REPORT.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeGlk::Lrc {

// Gemini Lake (graphics version 9, Gen9 LP) execlists logical ring context
// encoding transcribed from Linux i915 at torvalds/linux
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a: the context image of
// lrc_init_state()/lrc_update_regs(), the context descriptor, the two-port
// ELSP write order and the Gen8 context-status-buffer (CSB) format.
//
// Pure builders and decoders: nothing here maps memory, allocates, reads or
// writes MMIO, owns a GPU, or shows that hardware executed anything. Images
// are serialized as explicit little-endian dwords. Every function leaves its
// outputs and the image unchanged when it returns a status other than Ok.
//
// Only the GLK configuration of i915_pci.c GEN9_LP_FEATURES is represented:
// no ELSQ, two ELSP ports, a 48-bit (4-level) full PPGTT, a 39-bit DMA mask
// and the four platform engines below. Runtime engine fusing, GGTT size,
// GuC WOPCM pin bias and forcewake are not known to this unit.

enum class Status : uint8_t {
    Ok,
    UnsupportedEngine,
    MissingRenderPrerequisites,    // RCS0 without the inputs below
    UnexpectedRenderPrerequisites, // render inputs for a non-render engine
    InvalidBuffer,                 // null, wrong size or partially aliased
    InvalidContextAddress,
    InvalidAddressSpace,
    InvalidRing,
    InvalidRingOffset,
    TailInHeadCacheline,
    InvalidWorkaroundBatch,
    InvalidPowerClockState,
    InvalidContextId,
    InvalidDescriptor,
    LayoutMismatch,
    RingMismatch,
    StopRingSet,
    EmptyPort0,
    DuplicateContext,
    CsbEntryNotWritten,
    InvalidCsbPointer,
    CsbIdentityMismatch
};

// GEN9_LP_FEATURES platform_engine_mask. Whether a given part fuses off an
// engine is not determined here.
enum class Engine : uint8_t { Rcs0, Vcs0, Bcs0, Vecs0 };

struct EngineInfo {
    uint32_t mmioBase {};     // __engine_mmio_base() for graphics version 9
    uint32_t contextBytes {}; // intel_engine_context_size(): PPHWSP + state
    bool renderState {};      // I915_ENGINE_HAS_RCS_REG_STATE and _EU_PRIORITY
};
Status describeEngine(Engine, EngineInfo &out);

constexpr uint32_t PageBytes = 4096;           // PAGE_SIZE, I915_GTT_PAGE_SIZE
constexpr uint32_t StateOffset = PageBytes;    // LRC_STATE_OFFSET
constexpr uint32_t CachelineBytes = 64;        // CACHELINE_BYTES
constexpr uint64_t DmaAddressLimit = 1ULL << 39; // GLK dma_mask_size 39

// RCS0 values this unit cannot derive. For GLK, Linux points INDIRECT_CTX at
// the gen9_init_indirectctx_bb() workaround batch built by lrc_init_wa_ctx()
// and loads R_PWR_CLK_STATE from intel_sseu_make_rpcs() over the fused
// slice/subslice/EU topology. Neither producer is implemented here, so an
// RCS0 image is refused rather than built without them. Only the encoding of
// the supplied values is checked, never their correctness for a part.
struct RenderPrerequisites {
    uint32_t indirectCtxGgtt {};  // ggtt(wa_ctx->vma) + indirect_ctx.offset
    uint32_t indirectCtxBytes {}; // indirect_ctx.size
    uint32_t powerClockState {};  // GEN8_R_PWR_CLK_STATE value
};

struct ContextImageSpec {
    Engine engine {};
    uint64_t pml4Dma {};          // px_dma(ppgtt->pd) of the 4-level PPGTT
    uint32_t contextTimestamp {}; // ce->stats.runtime.last, 0 when new
    const RenderPrerequisites *render {}; // RCS0 only
};

struct RingSpec {
    uint32_t ggtt {};  // i915_ggtt_offset(ring->vma)
    uint32_t bytes {}; // ring->size
    uint32_t head {};  // head argument of lrc_update_regs()
    uint32_t tail {};  // ring->tail
};

// First-pin lrc_init_state(). Image must be exactly contextBytes.
// Without a default state (nullptr, 0) the result is a fresh image with
// restore inhibited: all bytes zero except the register-state page, which
// holds the gen9_rcs_offsets/gen9_xcs_offsets MI_LOAD_REGISTER_IMM headers
// and register offsets closed by MI_BATCH_BUFFER_END, plus the values of
// init_common_regs(), init_ppgtt_regs(), init_wa_bb_regs() and
// __reset_stop_ring(). With a default state of exactly contextBytes it is
// copied, the PPHWSP page is cleared and the same dwords are rewritten
// without closing the list or inhibiting restore. Ring registers are left
// for updateContextRegisters(), as in lrc_pin().
Status initContextImage(const ContextImageSpec &, const uint8_t *defaultState,
                        size_t defaultStateBytes, uint8_t *image, size_t imageBytes);

// lrc_init_regs(ce, engine, true), the reset_active()/lrc_reset() scrub:
// rewrites only the register-state page as a fresh restore-inhibited page.
Status scrubRegisterState(const ContextImageSpec &, uint8_t *image, size_t imageBytes);

// lrc_reset_regs(): clears STOP_RING in RING_MI_MODE and sets its mask bit.
Status resetStopRing(Engine, uint8_t *image, size_t imageBytes);

// lrc_update_regs(): RING_START, RING_HEAD, RING_TAIL, RING_CTL and, for
// RCS0, R_PWR_CLK_STATE. OA state is not written; i915_oa_init_reg_state()
// writes it only while an exclusive perf stream exists. lrcaOut receives
// lrc_descriptor() | CTX_DESC_FORCE_RESTORE, the low descriptor dword.
Status updateContextRegisters(const ContextImageSpec &, const RingSpec &,
                              uint32_t contextGgtt, uint8_t *image,
                              size_t imageBytes, uint32_t &lrcaOut);

// Descriptor bits, intel_lrc.h and the lrc_descriptor() comment: [0] VALID,
// [1] FORCE_PD_RESTORE, [2] FORCE_RESTORE, [4:3] addressing mode, [5]
// L3LLC_COHERENT (Gen8 only), [8] PRIVILEGE, [10:9] GEN12_CTX_PRIORITY,
// [31:12] LRCA, [52:32] context ID, [54:53] MBZ, [63:55] group ID (unused).
// execlists_update_context() also ORs the priority field into RCS0
// descriptors on GLK because every RENDER_CLASS engine carries
// I915_ENGINE_HAS_EU_PRIORITY; what Gen9 hardware does with it is not
// established by these sources. Accepted descriptors are exactly those this
// unit produces: VALID, PRIVILEGE, legacy 64-bit mode, no PD restore,
// priority only on RCS0 and never 3, nonzero LRCA and context ID.
enum class Priority : uint8_t { Low, Normal, High }; // vs I915_PRIORITY_NORMAL
struct DescriptorFields {
    uint32_t lrca {};
    uint32_t contextId {};
    bool forceRestore {};
    bool hasPriority {};
    Priority priority {};
};
Status decodeDescriptor(Engine, uint64_t descriptor, DescriptorFields &out);

// __execlists_schedule_in() below IP 12.55 without a fixed OA tag:
// ccid = (1 + tag) << (GEN11_SW_CTX_ID_SHIFT - 32), with tag a free bit of
// GENMASK(BITS_PER_LONG - 2, 0) on a 64-bit kernel. Gen9 adds no engine bits.
// This encodes a tag; it does NOT allocate/reserve a live tag. The native owner
// must retain the assignment until context retirement and prevent concurrent reuse.
Status contextIdForTag(unsigned tag, uint32_t &ccid);

// ce->lrc: lrca from updateContextRegisters() (FORCE_RESTORE pending until the
// next submission) and the context ID from contextIdForTag().
struct SubmitState {
    uint32_t lrca {};
    uint32_t contextId {};
};

// execlists_update_context(). The previous tail is CTX_RING_TAIL of the image
// (Linux asserts it equals ring->tail). requestTail must satisfy
// assert_ring_tail_valid() against ringHead, the last known RING_HEAD.
// FORCE_RESTORE is added when intel_ring_direction(tail, prev) <= 0, the
// tail is stored, FORCE_RESTORE is cleared from state, and the 64-bit ELSP
// descriptor is returned. Priority affects RCS0 only, as in Linux.
// This codec additionally refuses caller/image ring-size drift, invalid ring
// backing and enabled STOP_RING before mutating the image or output.
Status prepareSubmission(Engine, uint32_t ringBytes, uint32_t ringHead,
                         uint32_t requestTail, Priority, SubmitState &state,
                         uint8_t *image, size_t imageBytes, uint64_t &descriptorOut);

// __unwind_incomplete_requests(): sets FORCE_RESTORE in state when
// intel_ring_direction(requestTail, ringTail + 8) > 0.
Status markUnwind(uint32_t ringBytes, uint32_t requestTail, uint32_t ringTail,
                  SubmitState &state);

struct MmioWrite {
    uint32_t offset {};
    uint32_t value {};
};

// execlists_submit_ports() without ELSQ: ports 1 then 0, each as upper then
// lower dword to RING_ELSP; an empty port 1 is the zero descriptor. Port
// checks follow assert_pending_valid(): port 0 present, no repeated context
// (LRCA) or context ID. The writes are only planned.
struct ElspPlan {
    MmioWrite writes[4] {};
};
Status planElspSubmission(Engine, uint64_t port0, uint64_t port1, ElspPlan &out);

// Gen8 CSB (GRAPHICS_VER < 11): six qword entries at engine status page dword
// I915_HWS_CSB_BUF0_INDEX, write pointer at dword I915_HWS_CSB_WRITE_INDEX.
// Pointer values 6 and 7 are reserved (intel_lrc_reg.h).
constexpr unsigned CsbEntries = 6;
constexpr unsigned HwspCsbBufferDword = 0x10;
constexpr unsigned HwspCsbWriteDword = 0x1f;

// reset_csb_pointers(): RING_CONTEXT_STATUS_PTR masked write, the cached head
// and status-page write pointer set to csb_size - 1, and entries filled with
// all-ones so stale entries are detectable.
struct CsbReset {
    MmioWrite statusPointer {};
    uint8_t head {};
    uint32_t writePointerDword {};
    uint64_t entryFill {};
};
Status planCsbReset(Engine, CsbReset &out);

// process_csb() walk: from the cached head to the status-page write pointer
// (only its low byte, as Linux reads it into a u8). Linux has no range check;
// a reserved pointer would never match a wrapped head, so it is rejected.
struct CsbWalk {
    uint8_t indices[CsbEntries] {};
    uint8_t count {};
    uint8_t newHead {};
};
Status walkCsb(uint8_t head, uint32_t writePointerDword, CsbWalk &out);

// Low dword is status, high dword the context ID. gen8_csb_parse() promotes
// pending to active on IDLE_ACTIVE or PREEMPTED; otherwise port 0 completed.
// All-ones is the unwritten marker (csb_read() falls back to MMIO and then
// writes all-ones back after consuming an entry).
struct CsbEvent {
    uint32_t status {};
    uint32_t contextId {};
    bool promote {};
    bool idleToActive {};
    bool preempted {};
    bool elementSwitch {};
    bool activeToIdle {};
    bool complete {};
    bool liteRestore {};
};
Status decodeCsbEntry(uint64_t entry, CsbEvent &out);

// An original Mellow identity check after raw decode, against the caller's
// retained native assignment. Decode accepts uninterpreted status/reserved
// bits; its Ok is NOT event validity or GPU completion. This helper only
// checks a nonzero 21-bit context identity; even a match does not validate
// pending/active event ordering, producer epoch, job fence or retirement.
Status checkCsbContext(const CsbEvent &, uint32_t assignedContextId);

// wa_csb_read() fallback: GEN8_EXECLISTS_STATUS_BUF + 8 * index (index < 6).
Status csbEntryMmioOffset(Engine, unsigned index, uint32_t &offset);

// Register-state values by meaning. slot43 is R_PWR_CLK_STATE on RCS0 and the
// base + 0x200 register on other engines (never written by this unit there).
struct RegisterState {
    uint32_t contextControl {};
    uint32_t ringHead {};
    uint32_t ringTail {};
    uint32_t ringStart {};
    uint32_t ringCtl {};
    uint32_t bbState {};
    uint32_t timestamp {};
    uint32_t pdpUdw[4] {}; // index n is PDPn
    uint32_t pdpLdw[4] {};
    uint32_t slot43 {};
    uint32_t miMode {};
    uint32_t bbOffset {};
    uint32_t bbPerCtxPtr {};
    uint32_t indirectCtx {};
    uint32_t indirectCtxOffset {};
    bool listClosed {}; // MI_BATCH_BUFFER_END after the last register pair
};
// Requires the exact set_offsets() headers and register offsets. Whether GLK
// hardware saves them unchanged is not established here.
Status decodeRegisterState(Engine, const uint8_t *image, size_t imageBytes,
                           RegisterState &out);

// lrc_check_regs() without its repairs: RING_START, RING_CTL ignoring
// RING_WAIT and RING_WAIT_SEMAPHORE, and an enabled STOP_RING.
Status checkRegisterState(const RegisterState &, uint32_t ringGgtt, uint32_t ringBytes);

} // namespace MellowNativeGlk::Lrc

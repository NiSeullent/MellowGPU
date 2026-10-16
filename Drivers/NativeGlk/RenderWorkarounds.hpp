// SPDX-License-Identifier: MIT
// Copyright © 2014 Intel Corporation
// Copyright © 2014-2018 Intel Corporation
// Copyright © 2003-2018 Intel Corporation
// Copyright © 2019 Intel Corporation
// Copyright © 2020,2021 Intel Corporation
// Copyright © 2022 Intel Corporation
// Copyright © 2016 Intel Corporation
// Copyright 2013 Intel Corporation
// Copyright (c) 2026 Mellow contributors.
// Permission text is retained in RenderWorkarounds.cpp and
// RenderWorkarounds.LICENSE.MIT. Exact source commit, paths, blobs, hashes and
// symbols: RenderWorkarounds.provenance.json.
#pragma once
#include "LrcImage.hpp"
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeGlk::RenderWa {

// Gemini Lake (graphics version 9, IS_GEN9_LP, no LLC) RCS0 render-context
// producers transcribed from Linux i915 at torvalds/linux
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a:
//  - intel_step_init() revision handling for IS_GEMINILAKE,
//  - gen9_sseu_info_init() topology decode from the fuse registers,
//  - intel_sseu_from_device_info() and intel_sseu_make_rpcs(),
//  - lrc_init_wa_ctx() with gen9_init_indirectctx_bb(),
//    gen8_emit_flush_coherentl3_wa(), gen8_emit_pipe_control() and emit_lri(),
//  - glk_ctx_workarounds_init() and intel_engine_emit_ctx_wa() with
//    gen8_emit_flush_rcs(EMIT_BARRIER).
//
// Pure producers: nothing here reads registers, maps or pins memory, owns a
// GPU, takes forcewake or shows that hardware executed anything. Outputs are
// little-endian dwords. A function returning other than Ok leaves every output
// and destination buffer unchanged.

enum class Status : uint8_t {
    Ok,
    MissingIdentity,       // PCI identity not supplied by a native reader
    UnsupportedDevice,     // not 8086:3185, the designated GLK target
    MissingFuses,          // GEN8_FUSE2/GEN9_EU_DISABLE(0) not natively read
    InconsistentFuses,     // slice bits other than slice 0 in a GEN9_LP part
    NoRenderSlice,         // slice 0 fused off
    NoRenderSubslice,      // every subslice fused off
    NoRenderEu,            // every EU of the enabled subslices fused off
    InvalidTopology,       // not the decode of its recorded identity/fuses
    InvalidWaContextAddress,
    InvalidScratchAddress,
    AddressOverlap,        // wa_ctx and scratch objects share a GGTT page
    InvalidBuffer          // null, wrong size or partially aliased
};

// PCI configuration-space identity read by the native owner of the bound
// device. present must only be set by such a reader; the values cannot be
// authenticated here.
struct PciIdentity {
    bool present {};
    uint16_t vendor {}, device {};
    uint8_t revision {};
    uint16_t subsystemVendor {}, subsystem {};
};

constexpr uint16_t GlkVendor = 0x8086;
constexpr uint16_t GlkDevice = 0x3185; // INTEL_GLK_IDS member; 0x3184 is refused

// intel_step_init() for IS_GEMINILAKE: glk_revids[] = { [3] = B0 }. Revisions
// 0..2 hit table gaps and take the next entry (B0); revisions above 3 are
// "Unknown revid" and use STEP_FUTURE. Linux continues in both cases and no
// GLK RCS0 producer below depends on the step.
enum class GraphicsStep : uint8_t { B0, Future };
struct Platform {
    GraphicsStep step {};
    bool revisionInTable {}; // false where Linux logs "Unknown revid"
    bool valid {};           // set only by identifyPlatform()
};
Status identifyPlatform(const PciIdentity &, Platform &out);

// Raw register values in the order gen9_sseu_info_init() reads them. Both
// offsets lie in GEN_FW_RANGE(0x8d00, 0x93ff, FORCEWAKE_GT) of __gen9_fw_ranges
// (intel_uncore.c), so a meaningful read needs the GT forcewake domain held.
// present must only be set by a native reader that held it.
constexpr uint32_t Fuse2Offset = 0x9120;      // GEN8_FUSE2
constexpr uint32_t EuDisable0Offset = 0x9134; // GEN9_EU_DISABLE(0)
struct FuseReadings {
    bool present {};
    uint32_t fuse2 {};
    uint32_t euDisable0 {};
};

// struct sseu_dev_info for IS_GEN9_LP: intel_sseu_set_info(sseu, 1, 3, 8).
// Linux accepts any fuse value; this decoder additionally refuses a reading
// whose slice field names a slice other than 0 (the "single slice" GEN9_LP
// model the decode relies on) or that leaves no render slice, subslice or EU.
// An all-zero or all-ones reading, typical of an unpowered or unmapped read,
// is therefore refused instead of being treated as a full GPU.
struct Topology {
    PciIdentity identity {};
    FuseReadings fuses {};
    Platform platform {};
    uint8_t maxSlices {}, maxSubslices {}, maxEusPerSubslice {};
    uint8_t sliceMask {};    // REG_FIELD_GET(GEN8_F2_S_ENA_MASK, fuse2)
    uint8_t subsliceMask {}; // subslice_mask.hsw[0]
    uint8_t euMask[3] {};    // eu_mask.hsw[0][ss], zero for fused subslices
    uint8_t subslice7Eu {};  // subslice_7eu[0]
    uint8_t euTotal {};
    uint8_t euPerSubslice {};
    bool hasSlicePg {}, hasSubslicePg {}, hasEuPg {};
    bool hasPooledEu {};     // RUNTIME_INFO()->has_pooled_eu
    uint8_t minEuInPool {};
};
Status decodeTopology(const PciIdentity &, const FuseReadings &, Topology &out);

// Recomputes the decode from the recorded identity and fuses and compares
// every derived field; every consumer below starts with this check.
Status checkTopology(const Topology &);

// struct intel_sseu. Only the request Linux uses for GLK RCS0 contexts is
// produced: engine->sseu = intel_sseu_from_device_info() (intel_engine_cs.c).
// No user context SSEU or i915/perf pinned request is modelled.
struct SseuRequest {
    uint8_t sliceMask {}, subsliceMask {};
    uint8_t minEusPerSubslice {}, maxEusPerSubslice {};
};
Status defaultSseu(const Topology &, SseuRequest &out);

// intel_sseu_make_rpcs() for graphics version 9 with that request: the
// GEN8_R_PWR_CLK_STATE value lrc_update_regs() stores in CTX_R_PWR_CLK_STATE.
Status makeRpcs(const Topology &, uint32_t &powerClockState);

// lrc_init_wa_ctx(): one CTX_WA_BB_SIZE object pinned in the GGTT, the
// indirect-context batch at offset 0 and an empty per-context batch after it.
constexpr uint32_t WaContextBytes = 4096;          // CTX_WA_BB_SIZE (PAGE_SIZE)
constexpr uint32_t ScratchBytes = 4096;            // intel_gt_init_scratch(): SZ_4K
constexpr uint32_t CoherentL3ScratchField = 256;   // INTEL_GT_SCRATCH_FIELD_COHERENTL3_WA
constexpr uint32_t PphwspScratchAddress = 0x34*4;  // LRC_PPHWSP_SCRATCH_ADDR
constexpr uint32_t IndirectCtxOffsetDefault = 0x26; // GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT

// GGTT offsets of the two objects, assigned by the native GGTT owner. Both
// must be page aligned, nonzero and inside the 32-bit GGTT.
struct WaPlacement {
    uint32_t waContextGgtt {}; // i915_ggtt_offset(engine->wa_ctx.vma)
    uint32_t scratchGgtt {};   // i915_ggtt_offset(gt->scratch)
};

struct WaContext {
    uint32_t indirectOffset {}, indirectBytes {}; // wa_ctx->indirect_ctx
    uint32_t perCtxOffset {}, perCtxBytes {};     // wa_ctx->per_ctx (empty on Gen9)
    bool mediaPoolState {};                       // WaMediaPoolStateCmdInWABB emitted
    Lrc::RenderPrerequisites render {};           // input of Lrc::initContextImage
};

// Writes the whole object: gen9_init_indirectctx_bb() from offset 0, padded
// with MI_NOOP to a cacheline and with no MI_BATCH_BUFFER_END, then zeros as
// in a fresh shmem object. The GEN9_MEDIA_POOL_STATE packet is emitted only
// when the decoded topology has pooled EUs (three enabled subslices).
// render.indirectCtxGgtt = waContextGgtt + indirect offset, render.
// indirectCtxBytes = indirect size and render.powerClockState = makeRpcs().
// The caller must make the bytes visible to the GPU before use
// (__i915_gem_object_flush_map()) and keep scratch + 256 private to this batch.
Status buildWaContext(const Topology &, const WaPlacement &, uint8_t *object,
                      size_t objectBytes, WaContext &out);

// Context workaround list of __intel_engine_init_ctx_wa() for GLK RCS0:
// gen9_ctx_workarounds_init() without the HAS_LLC entries plus
// glk_ctx_workarounds_init(), merged and sorted by offset as _wa_add() does.
// Every entry is a masked register, so intel_engine_emit_ctx_wa() loads
// wa->set without reading the register.
struct ContextWorkaround {
    uint32_t offset {};
    uint32_t set {};  // value loaded by MI_LOAD_REGISTER_IMM
    uint32_t read {}; // bits wa_verify() would compare
    bool mcr {};      // multicast register, emitted at its plain offset
};
constexpr unsigned ContextWorkaroundCount = 9;
Status contextWorkarounds(const Platform &, ContextWorkaround (&out)[ContextWorkaroundCount]);

// intel_engine_emit_ctx_wa() for GLK RCS0, as __engines_record_defaults()
// emits it into the request that records the default context image:
// gen8_emit_flush_rcs(EMIT_BARRIER) (with the Gen9 null PIPE_CONTROL before
// VF invalidation), MI_LOAD_REGISTER_IMM of the list, MI_NOOP, and the same
// flush again. Every block is an even number of dwords; the request owner
// places them at an 8-byte aligned ring offset (intel_ring_begin()).
constexpr unsigned ContextWorkaroundRingDwords = 12 + 2*ContextWorkaroundCount + 2 + 12;
Status emitContextWorkarounds(const Platform &, uint8_t *ring, size_t ringBytes,
                              size_t &writtenBytes);

} // namespace MellowNativeGlk::RenderWa

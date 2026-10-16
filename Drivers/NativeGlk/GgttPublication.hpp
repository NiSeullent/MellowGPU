// SPDX-License-Identifier: MIT
/*
 * Gemini Lake (Gen9 LP, i915/execlists) GGTT publication and invalidation.
 *
 * Sequences and constants are re-expressed (no verbatim code) from
 * torvalds/linux 0d9ff90a5422cc7509258aaaba1e7481df4d332a:
 *   drivers/gpu/drm/i915/gt/intel_ggtt.c          Copyright © 2020 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_tlb.c           Copyright © 2023 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_engine_cs.c     Copyright © 2016 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_engine_types.h  Copyright © 2019 Intel Corporation
 *   drivers/gpu/drm/i915/gt/intel_gt_regs.h       Copyright © 2022 Intel Corporation
 *   drivers/gpu/drm/i915/intel_uncore.c           Copyright © 2013 Intel Corporation
 *   drivers/gpu/drm/i915/i915_wait_util.h         Copyright © 2025 Intel Corporation
 *   drivers/gpu/drm/i915/i915_pci.c               Copyright © 2016 Intel Corporation
 *   include/drm/intel/i915_drm.h                  Copyright 2003 Tungsten Graphics, Inc.
 * Copyright (c) 2026 Mellow contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * See GgttPublication.provenance.json for blob ids, SHA-256 and symbols.
 */
#pragma once
#include "PageTableEncoding.hpp"

// Portable sequencing over an injected register/GSM transport. This file
// owns ordering, ownership and failure semantics; it never maps a BAR or
// touches hardware itself. Mellow/GlkGgttIOKit supplies the native transport.
namespace Mellow { namespace NativeGlk {

enum class GgttStatus : uint8_t {
    Ok,
    InvalidArgument,
    Unavailable,       // a required producer/authority is absent: no effect
    Ownership,         // owner/extent/generation mismatch: no effect
    Busy,              // consumers not proven retired: no effect
    Capacity,
    Overlap,           // extent intersects retained, guard or owned range
    EncodingRejected,  // PageTableEncoding refused a page: no effect
    ReadbackMismatch,  // entry writes issued, readback differs: quarantined
    InvalidateTimeout, // TLB invalidation not acknowledged in time: quarantined
    TransportFailure,  // BAR access refused after writes began: quarantined
    ClockRegression,   // monotonic clock went backwards mid-wait: quarantined
    Quarantined,       // extent or publisher permanently held
};

namespace Gen9LpGgtt {
// gen8_gmch_probe/ggtt_probe_common: GEN4_GTTMMADR_BAR (config 0x10) is 16 MiB
// (gen6_gttmmadr_size), the GSM starts at gen6_gttadr_offset = size / 2, and
// SNB_GMCH_CTRL (config 0x50) selects the GSM size through BDW_GMCH_GGMS.
constexpr uint8_t kGttMmadrBarRegister = 0x10;
constexpr uint8_t kGmchControlRegister = 0x50;
constexpr uint64_t kGttMmadrBytes = Glk::kGttMmadrBytes;
constexpr uint64_t kGsmBarOffset = Glk::kGttMmadrBytes / 2;
static_assert(kGsmBarOffset == Glk::kGsmBar0Offset, "gen6_gttadr_offset");
// init_ggtt: pin_bias = max(I915_GTT_PAGE_SIZE, intel_wopcm_guc_size); GLK
// runs without GuC so the bias is one page. The last page (vm.total -
// PAGE_SIZE) stays bound to scratch against hardware prefetch past the end.
constexpr uint64_t kPinBiasBytes = Glk::kPageSize4K;
constexpr uint64_t kTopGuardBytes = Glk::kPageSize4K;

// intel_engine_init_tlb_invalidation gen8_regs (GRAPHICS_VER 8..11). Every
// GLK engine (i915_pci.c GEN9_LP_FEATURES: RCS0|VCS0|BCS0|VECS0) is instance
// 0, so request == done == BIT(0), written unmasked below Gen12.
constexpr uint32_t kRtcr = 0x4260;   // GEN8_RTCR, RENDER_CLASS
constexpr uint32_t kM1tcr = 0x4264;  // GEN8_M1TCR, VIDEO_DECODE_CLASS
constexpr uint32_t kBtcr = 0x426c;   // GEN8_BTCR, COPY_ENGINE_CLASS
constexpr uint32_t kVtcr = 0x4270;   // GEN8_VTCR, VIDEO_ENHANCEMENT_CLASS
constexpr uint32_t kTlbInvalidateBit = 1u << 0;
constexpr uint32_t kEngineCount = 4;
// for_each_engine order: enum intel_engine_id RCS0, BCS0, VCS0, VECS0.
constexpr uint32_t kEngineTlbControls[kEngineCount] = {kRtcr, kBtcr, kM1tcr, kVtcr};
// intel_tlb.c TLB_INVAL_TIMEOUT_US / _MS for __intel_wait_for_register_fw.
constexpr uint32_t kTlbFastTimeoutUs = 100;
constexpr uint32_t kTlbSlowTimeoutMs = 4;
// i915_wait_util.h wait_for(COND, MS) = _wait_for(COND, MS*1000, 10, 1000):
// usleep_range(wait, 2*wait), doubling while wait < 1000 us, and one check
// after expiry. _wait_for_atomic spins with cpu_relax; this port pauses
// 1 us per tight poll so the poll count stays bounded.
constexpr uint32_t kSlowWaitMinUs = 10, kSlowWaitMaxUs = 1000;
// Upper bound on register polls independent of the clock source.
constexpr uint32_t kMaxTlbPolls = 4096;
// The only register page the native adapter maps besides the GSM.
constexpr uint32_t kRegisterWindowOffset = 0x4000;
constexpr uint32_t kRegisterWindowBytes = 0x1000;
// GFX_FLSH_CNTL_GEN6: gen8_ggtt_invalidate writes it only for WC GSM
// mappings. needs_wc_ggtt_mapping() is false for IS_GEN9_LP: never written.
constexpr uint32_t kGfxFlushControlGen6 = 0x101008;

// PCI BAR register low bits and command register memory decode enable.
constexpr uint32_t kBarIoSpace = 1u << 0;
constexpr uint32_t kBarTypeMask = 3u << 1, kBarType64 = 2u << 1;
constexpr uint32_t kBarPrefetchable = 1u << 3;
constexpr uint16_t kCommandMemorySpace = 1u << 1;
} // namespace Gen9LpGgtt

struct GgttGeometry {
    uint64_t total {};        // GGTT address space bytes: 1, 2 or 4 GiB
    uint64_t entryCount {};   // total / 4 KiB
    uint64_t gsmBytes {};     // entryCount * 8: 2, 4 or 8 MiB
    uint64_t gsmBarOffset {}; // within GTTMMADR
    uint64_t firstUsable {};  // pin bias
    uint64_t endUsable {};    // total - top guard page
};

// gen8_get_total_gtt_size + ggtt_probe_common BAR checks. The BAR length is
// the live resource length; anything but 16 MiB is refused, not warned.
GgttStatus deriveGgttGeometry(uint16_t gmchControl, uint64_t gttMmadrBytes, GgttGeometry &out);
// Exact GTTMMADR identity: 64-bit, non-prefetchable memory BAR with decode
// enabled and a 16 MiB aligned nonzero base. Prefetchable or I/O BARs cannot
// carry registers with read side effects and are refused.
GgttStatus decodeGttMmadrBar(uint32_t barLow, uint32_t barHigh, uint16_t command, uint64_t &base);

enum class GgttRangeKind : uint8_t { Firmware = 1, Display, Stolen, Platform };
struct GgttRange { uint64_t start {}, bytes {}; };
struct GgttRetainedRange { uint64_t start {}, bytes {}; GgttRangeKind kind {}; };

// Sealed handover from the native display/firmware/stolen owner. It names
// every GGTT range whose entries must survive (boot framebuffer, firmware and
// stolen-memory bindings) and the pool the native owner may allocate from.
// A missing or incomplete record means no reservation can be proven: zero or
// scratch PTEs are never treated as evidence that a range is free.
struct GgttHandover {
    uint64_t registryId {}, epoch {};
    uint64_t ggttTotal {};
    GgttRange pool {};
    const GgttRetainedRange *retained {};
    uint32_t retainedCount {};
    bool complete {};
};

struct GgttExtent {
    uint32_t slot {UINT32_MAX};
    uint64_t generation {}, start {}, bytes {};
};
enum class GgttExtentState : uint8_t { Free, Reserved, Published, Quarantined };

enum class GgttFence : uint8_t { Full, Store, Load };

// Native transport. Entry images are explicit little-endian bytes; the
// adapter must issue each as ONE 64-bit uncached access (writes wider than
// 64 bits to the Gen9 LP GSM are dropped). Registers are 32-bit uncached.
// A false return means the access was not issued within the bounded mapping.
struct GgttIo {
    void *context {};
    bool (*writeEntry)(void *, uint64_t index, const uint8_t (&le)[8]) {};
    bool (*readEntry)(void *, uint64_t index, uint8_t (&le)[8]) {};
    bool (*writeRegister)(void *, uint32_t offset, uint32_t value) {};
    bool (*readRegister)(void *, uint32_t offset, uint32_t &value) {};
    void (*fence)(void *, GgttFence) {};
    bool (*now)(void *, uint64_t &nanoseconds) {};
    void (*pause)(void *, uint32_t microseconds) {};
};

// mmio_invalidate_full holds FORCEWAKE_ALL and serialises with GT reset.
// acquire returns true only with a real native GT runtime-PM awake reference,
// every forcewake domain acknowledged, and GT reset excluded until release. This is the pending native
// forcewake/reset owner; it is not implemented here.
struct GgttGtAccess {
    void *context {};
    bool (*acquire)(void *, uint64_t &token) {};
    bool (*release)(void *, uint64_t token) {};
    // Trusted native owner observes engine runtime-PM wakerefs while token
    // holds forcewake/reset exclusion. Bits follow kEngineTlbControls order.
    // It must serialize engine wake/resume invalidation for the full lease.
    // No user/caller-provided mask may be used as authority. Missing/unknown
    // producer refuses publication before entry writes.
    bool (*awakeEngines)(void *, uint64_t token, uint32_t &mask) {};
};

// Publication protocol (all under the caller's serializing owner lock):
//   acquire GT access -> capture owned entries (non-posted reads) -> mfence ->
//   one 64-bit LE store per entry -> mfence -> readback every entry ->
//   gen8_ggtt_invalidate (GEN9_LP: no register write) -> engine TLB
//   invalidation (write BIT(0) to each control, wait each to read 0) ->
//   release GT access.
// Unpublication restores exactly the captured entries in the same order and
// demands the same completed invalidation before the caller may release the
// backing pages. Linux's GEN9_LP bind performs no invalidation write; the
// engine invalidation is the pinned mechanism Linux runs before releasing
// pages, applied to both directions so "completed" is observed, not inferred.
// It is NOT engine quiescence: retirement of every consumer is a separate
// proof the caller must supply before unpublication.
class GgttPublisher {
public:
    static constexpr uint32_t MaxExtents = 32, MaxRetained = 16;
    GgttPublisher() = default;
    GgttPublisher(const GgttPublisher &) = delete;
    GgttPublisher &operator=(const GgttPublisher &) = delete;

    GgttStatus initialize(const GgttGeometry &, const GgttHandover &, GgttIo, GgttGtAccess);
    // Native-owned, page-aligned, nonoverlapping extents inside the handover
    // pool, never intersecting a retained range, the pin bias or the guard.
    GgttStatus reserve(uint64_t owner, uint64_t bytes, GgttExtent &out);
    GgttStatus reserveAt(uint64_t owner, uint64_t start, uint64_t bytes, GgttExtent &out);
    // Bookkeeping only; Published and Quarantined extents are never released.
    GgttStatus release(uint64_t owner, const GgttExtent &);
    // dmaPages are device-visible addresses of the original private pages.
    // capture (captureCount >= pageCount) receives the prior entries and stays
    // owned by the publisher until unpublish Ok or release of the extent.
    GgttStatus publish(uint64_t owner, const GgttExtent &, const uint64_t *dmaPages,
                       size_t pageCount, uint64_t *capture, size_t captureCount);
    // consumersRetired is the caller's retirement proof for this extent.
    GgttStatus unpublish(uint64_t owner, const GgttExtent &, bool consumersRetired);
    GgttExtentState state(const GgttExtent &) const;
    const uint64_t *captured(const GgttExtent &) const;
    bool initialized() const { return initialized_; }
    bool faulted() const { return faulted_; }
    uint32_t extentsInUse() const;
    // Forget configuration once every extent is Free; never with live state.
    GgttStatus shutdown();

private:
    struct Slot {
        GgttExtentState state {GgttExtentState::Free};
        uint64_t owner {}, generation {}, start {}, bytes {};
        uint64_t *capture {};
        size_t pageCount {};
    };
    Slot slots_[MaxExtents] {};
    GgttRetainedRange retained_[MaxRetained] {};
    uint32_t retainedCount_ {};
    GgttGeometry geometry_ {};
    GgttRange pool_ {};
    GgttIo io_ {};
    GgttGtAccess gt_ {};
    uint64_t generation_ {1};
    bool initialized_ {}, faulted_ {};

    Slot *lookup(uint64_t owner, const GgttExtent &);
    const Slot *lookup(const GgttExtent &) const;
    GgttStatus checkPlacement(uint64_t start, uint64_t bytes) const;
    GgttStatus claim(uint64_t owner, uint64_t start, uint64_t bytes, GgttExtent &out);
    GgttStatus quarantine(Slot &, GgttStatus cause);
    GgttStatus readEntries(const Slot &, uint64_t *into);
    GgttStatus writeEntries(const Slot &, const uint64_t *entries);
    GgttStatus verifyEntries(const Slot &, const uint64_t *entries);
    GgttStatus invalidateEngineTlbs(uint32_t awake);
    GgttStatus waitTlbControlClear(uint32_t offset);
};

} } // namespace Mellow::NativeGlk

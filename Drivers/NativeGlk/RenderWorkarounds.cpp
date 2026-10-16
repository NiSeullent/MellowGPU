// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Command, register and workaround encodings derived from Intel i915 files at
// Linux 0d9ff90a5422cc7509258aaaba1e7481df4d332a, copyright Intel Corporation
// (see RenderWorkarounds.hpp and RenderWorkarounds.provenance.json for the
// exact files, blobs and notices).
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "RenderWorkarounds.hpp"

namespace MellowNativeGlk::RenderWa {
namespace {
// intel_gpu_commands.h
constexpr uint32_t miInstr(uint32_t opcode, uint32_t flags) { return opcode << 23 | flags; }
constexpr uint32_t MiNoop = miInstr(0, 0);
constexpr uint32_t MiArbOnOff = miInstr(0x08, 0), MiArbEnable = 1, MiArbDisable = 0;
constexpr uint32_t miLoadRegisterImm(uint32_t count) { return miInstr(0x22, 2*count - 1); }
constexpr uint32_t MiStoreRegisterMemGen8 = miInstr(0x24, 2);
constexpr uint32_t MiLoadRegisterMemGen8 = miInstr(0x29, 2);
constexpr uint32_t MiSrmLrmGlobalGtt = 1U << 22;
constexpr uint32_t GfxOpPipeControl6 = 3U << 29 | 3U << 27 | 2U << 24 | (6 - 2);
constexpr uint32_t Gen9MediaPoolState = 3U << 29 | 2U << 27 | 5U << 16 | 4;
constexpr uint32_t Gen9MediaPoolEnable = 1U << 31;
constexpr uint32_t PcFlushL3 = 1U << 27, PcStoreDataIndex = 1U << 21, PcCsStall = 1U << 20,
                   PcTlbInvalidate = 1U << 18, PcQwWrite = 1U << 14,
                   PcRenderTargetCacheFlush = 1U << 12, PcInstructionCacheInvalidate = 1U << 11,
                   PcTextureCacheInvalidate = 1U << 10, PcFlushEnable = 1U << 7,
                   PcDcFlushEnable = 1U << 5, PcVfCacheInvalidate = 1U << 4,
                   PcConstCacheInvalidate = 1U << 3, PcStateCacheInvalidate = 1U << 2,
                   PcDepthCacheFlush = 1U << 0;
constexpr uint32_t CachelineBytes = 64; // CACHELINE_BYTES, intel_engine.h

// reg_bits.h REG_MASKED_FIELD{,_ENABLE,_DISABLE}
constexpr uint32_t maskedField(uint32_t mask, uint32_t value) { return mask << 16 | value; }
constexpr uint32_t maskedEnable(uint32_t bits) { return maskedField(bits, bits); }
constexpr uint32_t maskedDisable(uint32_t bits) { return maskedField(bits, 0); }

// intel_gt_regs.h
constexpr uint32_t Gen8L3sqcreg4 = 0xb118, Gen8LqscFlushCoherentLines = 1U << 21;
constexpr uint32_t CommonSliceChicken2 = 0x7014, Gen9DisableGatherAtSetShaderCommonSlice = 1U << 12,
                   Gen8SbeDisableReplayBufOptimization = 1U << 8;
constexpr uint32_t FfSliceChicken = 0x2088, FfSliceChickenClProvokingVertexFix = 1U << 1;
constexpr uint32_t ThreeDChicken3 = 0x2090, ThreeDChickenSfProvokingVertexFix = 1U << 12;
constexpr uint32_t Gen8F2SEnaShift = 25, Gen8F2SEnaBits = 0x7;  // REG_GENMASK(27, 25)
constexpr uint32_t Gen9F2SsDisShift = 20, Gen9F2SsDisBits = 0xf; // REG_GENMASK(23, 20)
constexpr uint32_t Gen8CsChicken1 = 0x2580, Gen9Preempt3dObjectLevel = 1U << 0;
constexpr uint32_t gen9PreemptGpgpuLevel(uint32_t hi, uint32_t lo) { return hi << 2 | lo << 1; }
constexpr uint32_t Gen9WmChicken3 = 0x5588, Gen9FactorInClrValHiz = 1U << 9;
constexpr uint32_t CacheMode1 = 0x7004, Gen8StcOptimizationDisable4x4 = 1U << 6,
                   Gen9PartialResolveInVcDisable = 1U << 1;
constexpr uint32_t HdcChicken0 = 0x7300, HdcForceCsrNonCoherentOvrDisable = 1U << 15,
                   HdcForceContextSaveRestoreNonCoherent = 1U << 5, HdcForceNonCoherent = 1U << 4;
constexpr uint32_t HalfSliceChicken2 = 0xe180, Gen8StPoDisable = 1U << 13;           // MCR
constexpr uint32_t Gen9HalfSliceChicken5 = 0xe188, Gen9CcsTlbPrefetchEnable = 1U << 3; // MCR
constexpr uint32_t Gen9HalfSliceChicken7 = 0xe194, Gen9EnableYv12Bugfix = 1U << 4,     // MCR
                   Gen9EnableGpgpuPreemption = 1U << 2;
constexpr uint32_t Gen8RowChicken = 0xe4f0, FlowControlEnable = 1U << 15,              // MCR
                   PartialInstructionShootdownDisable = 1U << 8;

// intel_engine_regs.h GEN8_R_PWR_CLK_STATE fields
constexpr uint32_t RpcsEnable = 1U << 31, RpcsSCntEnable = 1U << 18, RpcsSCntShift = 15,
                   RpcsSCntMask = 7U << 15, RpcsSsCntEnable = 1U << 11, RpcsSsCntShift = 8,
                   RpcsSsCntMask = 7U << 8, RpcsEuMaxShift = 4, RpcsEuMaxMask = 0xfU << 4,
                   RpcsEuMinShift = 0, RpcsEuMinMask = 0xfU;

// glk_revids[] = { [3] = { COMMON_STEP(B0) } } in intel_step.c.
constexpr unsigned GlkRevidTableSize = 4, GlkB0Revid = 3;

constexpr unsigned IndirectCtxMaxDwords = 64; // the GLK batch is 48 dwords

unsigned popcount8(uint32_t value) {
    unsigned n = 0;
    for (value &= 0xff; value; value &= value - 1) ++n;
    return n;
}
void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8*i));
}
bool overlaps(const void *a, size_t aBytes, const void *b, size_t bBytes) {
    const uintptr_t x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x < y + bBytes && y < x + aBytes;
}
// A page-aligned 4 KiB object at a nonzero offset inside the 32-bit GGTT.
bool ggttPage(uint32_t offset) {
    return offset && !(offset & (WaContextBytes - 1));
}

// Bounded dword stream: excess dwords are counted, never stored.
class Words {
public:
    Words(uint32_t *storage, unsigned capacity) : storage_(storage), capacity_(capacity) {}
    void put(uint32_t value) {
        if (count_ < capacity_) storage_[count_] = value;
        ++count_;
    }
    // __gen8_emit_pipe_control(batch, 0, flags, offset), gen8_engine_cs.h.
    void pipeControl(uint32_t flags, uint32_t offset) {
        put(GfxOpPipeControl6); put(flags); put(offset); put(0); put(0); put(0);
    }
    unsigned size() const { return count_; }
    bool overflow() const { return count_ > capacity_; }
    const uint32_t *data() const { return storage_; }

private:
    uint32_t *storage_;
    unsigned capacity_;
    unsigned count_ = 0;
};

struct Lri {
    uint32_t reg;
    uint32_t value;
};

// emit_lri(): one MI_LOAD_REGISTER_IMM, the pairs, then MI_NOOP.
void emitLri(Words &batch, const Lri *lri, unsigned count) {
    batch.put(miLoadRegisterImm(count));
    for (unsigned i = 0; i < count; ++i) { batch.put(lri[i].reg); batch.put(lri[i].value); }
    batch.put(MiNoop);
}

// gen8_emit_flush_coherentl3_wa(): saves GEN8_L3SQCREG4 to scratch + 256,
// loads the constant with GEN8_LQSC_FLUSH_COHERENT_LINES, flushes and
// restores the saved value.
void flushCoherentL3(Words &batch, uint32_t scratchGgtt) {
    const uint32_t slot = scratchGgtt + CoherentL3ScratchField;
    batch.put(MiStoreRegisterMemGen8 | MiSrmLrmGlobalGtt);
    batch.put(Gen8L3sqcreg4); batch.put(slot); batch.put(0);
    batch.put(miLoadRegisterImm(1));
    batch.put(Gen8L3sqcreg4); batch.put(0x40400000U | Gen8LqscFlushCoherentLines);
    batch.pipeControl(PcCsStall | PcDcFlushEnable, 0);
    batch.put(MiLoadRegisterMemGen8 | MiSrmLrmGlobalGtt);
    batch.put(Gen8L3sqcreg4); batch.put(slot); batch.put(0);
}

// gen9_init_indirectctx_bb(), starting at a cacheline-aligned offset.
void indirectContextBatch(Words &batch, uint32_t scratchGgtt, bool pooledEu) {
    static constexpr Lri lri[] = {
        // WaDisableGatherAtSetShaderCommonSlice:skl,bxt,kbl,glk
        {CommonSliceChicken2, maskedDisable(Gen9DisableGatherAtSetShaderCommonSlice)},
        // BSpec: 11391
        {FfSliceChicken, maskedEnable(FfSliceChickenClProvokingVertexFix)},
        // BSpec: 11299
        {ThreeDChicken3, maskedEnable(ThreeDChickenSfProvokingVertexFix)},
    };
    batch.put(MiArbOnOff | MiArbDisable);
    // WaFlushCoherentL3CacheLinesAtContextSwitch:skl,bxt,glk
    flushCoherentL3(batch, scratchGgtt);
    // WaClearSlmSpaceAtContextSwitch:skl,bxt,kbl,glk,cfl
    batch.pipeControl(PcFlushL3 | PcStoreDataIndex | PcCsStall | PcQwWrite, PphwspScratchAddress);
    emitLri(batch, lri, sizeof(lri) / sizeof(*lri));
    // WaMediaPoolStateCmdInWABB:bxt,glk loads the 3x6 default pool
    // configuration; hardware ignores bits of a fused subslice.
    if (pooledEu) {
        batch.put(Gen9MediaPoolState); batch.put(Gen9MediaPoolEnable);
        batch.put(0x00777000); batch.put(0); batch.put(0); batch.put(0);
    }
    batch.put(MiArbOnOff | MiArbEnable);
    // Pad to the end of the cacheline; INDIRECT_CTX carries the length in
    // cachelines, so no MI_BATCH_BUFFER_END follows.
    while ((4*batch.size()) % CachelineBytes) batch.put(MiNoop);
}

// gen8_emit_flush_rcs(rq, EMIT_BARRIER) for graphics version 9 parts other
// than Kabylake (no WaForGAMHang).
void flushRcsBarrier(Words &ring) {
    uint32_t flags = PcCsStall;
    // EMIT_FLUSH
    flags |= PcRenderTargetCacheFlush | PcDepthCacheFlush | PcDcFlushEnable | PcFlushEnable;
    // EMIT_INVALIDATE
    flags |= PcTlbInvalidate | PcInstructionCacheInvalidate | PcTextureCacheInvalidate |
             PcVfCacheInvalidate | PcConstCacheInvalidate | PcStateCacheInvalidate |
             PcQwWrite | PcStoreDataIndex;
    // On Gen9 a null PIPE_CONTROL must precede VF_CACHE_INVALIDATE.
    ring.pipeControl(0, 0);
    ring.pipeControl(flags, PphwspScratchAddress);
}

// i915_wa_list built with _wa_add(): sorted by offset, a repeated offset
// merges set and read bits. Masked additions carry clr == 0, so the
// "Discarding overwritten w/a" path never applies.
class WorkaroundList {
public:
    void maskedEnable(uint32_t reg, uint32_t bits, bool mcr) {
        add(reg, RenderWa::maskedEnable(bits), bits, mcr);
    }
    void maskedDisable(uint32_t reg, uint32_t bits, bool mcr) {
        add(reg, RenderWa::maskedDisable(bits), bits, mcr);
    }
    void maskedFieldSet(uint32_t reg, uint32_t mask, uint32_t value) {
        add(reg, maskedField(mask, value), mask, false);
    }
    unsigned count() const { return count_; }
    bool failed() const { return failed_; }
    const ContextWorkaround &operator[](unsigned i) const { return list_[i]; }

private:
    void add(uint32_t reg, uint32_t set, uint32_t read, bool mcr) {
        unsigned start = 0, end = count_;
        while (start < end) {
            const unsigned mid = start + (end - start)/2;
            if (list_[mid].offset < reg) start = mid + 1;
            else if (list_[mid].offset > reg) end = mid;
            else {
                if (list_[mid].mcr != mcr) failed_ = true;
                list_[mid].set |= set; list_[mid].read |= read;
                return;
            }
        }
        if (count_ == ContextWorkaroundCount) { failed_ = true; return; }
        for (unsigned i = count_; i > start; --i) list_[i] = list_[i - 1];
        list_[start] = {reg, set, read, mcr};
        ++count_;
    }
    ContextWorkaround list_[ContextWorkaroundCount] {};
    unsigned count_ = 0;
    bool failed_ = false;
};

// glk_ctx_workarounds_init(): gen9_ctx_workarounds_init() with HAS_LLC()
// false (GEN9_LP_FEATURES), IS_GEN9_LP() true and not SKL/KBL/CFL/CML.
void glkContextWorkarounds(WorkaroundList &wal) {
    // WaClearFlowControlGpgpuContextSave, WaDisablePartialInstShootdown
    wal.maskedEnable(Gen8RowChicken, FlowControlEnable | PartialInstructionShootdownDisable, true);
    // WaEnableYV12BugFixInHalfSliceChicken7, WaEnableSamplerGPGPUPreemptionSupport
    wal.maskedEnable(Gen9HalfSliceChicken7, Gen9EnableYv12Bugfix | Gen9EnableGpgpuPreemption, true);
    // Wa4x4STCOptimizationDisable, WaDisablePartialResolveInVc
    wal.maskedEnable(CacheMode1, Gen8StcOptimizationDisable4x4 | Gen9PartialResolveInVcDisable, false);
    // WaCcsTlbPrefetchDisable
    wal.maskedDisable(Gen9HalfSliceChicken5, Gen9CcsTlbPrefetchEnable, true);
    // WaForceContextSaveRestoreNonCoherent
    wal.maskedEnable(HdcChicken0, HdcForceContextSaveRestoreNonCoherent |
                                  HdcForceCsrNonCoherentOvrDisable, false);
    // WaForceEnableNonCoherent
    wal.maskedEnable(HdcChicken0, HdcForceNonCoherent, false);
    // WaDisableSTUnitPowerOptimization
    wal.maskedEnable(HalfSliceChicken2, Gen8StPoDisable, true);
    // WaDisable3DMidCmdPreemption
    wal.maskedDisable(Gen8CsChicken1, Gen9Preempt3dObjectLevel, false);
    // WaDisableGPGPUMidCmdPreemption
    wal.maskedFieldSet(Gen8CsChicken1, gen9PreemptGpgpuLevel(1, 1), gen9PreemptGpgpuLevel(1, 0));
    // WaClearHIZ_WM_CHICKEN3:bxt,glk
    wal.maskedEnable(Gen9WmChicken3, Gen9FactorInClrValHiz, false);
    // WaToEnableHwFixForPushConstHWBug:glk
    wal.maskedEnable(CommonSliceChicken2, Gen8SbeDisableReplayBufOptimization, false);
}

bool validPlatform(const Platform &p) {
    return p.valid && (p.step == GraphicsStep::B0 || p.step == GraphicsStep::Future);
}

bool sameTopology(const Topology &a, const Topology &b) {
    const PciIdentity &x = a.identity, &y = b.identity;
    if (x.present != y.present || x.vendor != y.vendor || x.device != y.device ||
        x.revision != y.revision || x.subsystemVendor != y.subsystemVendor ||
        x.subsystem != y.subsystem)
        return false;
    if (a.fuses.present != b.fuses.present || a.fuses.fuse2 != b.fuses.fuse2 ||
        a.fuses.euDisable0 != b.fuses.euDisable0)
        return false;
    if (a.platform.step != b.platform.step || a.platform.revisionInTable != b.platform.revisionInTable ||
        a.platform.valid != b.platform.valid)
        return false;
    for (unsigned ss = 0; ss < 3; ++ss)
        if (a.euMask[ss] != b.euMask[ss]) return false;
    return a.maxSlices == b.maxSlices && a.maxSubslices == b.maxSubslices &&
           a.maxEusPerSubslice == b.maxEusPerSubslice && a.sliceMask == b.sliceMask &&
           a.subsliceMask == b.subsliceMask && a.subslice7Eu == b.subslice7Eu &&
           a.euTotal == b.euTotal && a.euPerSubslice == b.euPerSubslice &&
           a.hasSlicePg == b.hasSlicePg && a.hasSubslicePg == b.hasSubslicePg &&
           a.hasEuPg == b.hasEuPg && a.hasPooledEu == b.hasPooledEu &&
           a.minEuInPool == b.minEuInPool;
}
} // namespace

Status identifyPlatform(const PciIdentity &id, Platform &out) {
    if (!id.present) return Status::MissingIdentity;
    if (id.vendor != GlkVendor || id.device != GlkDevice) return Status::UnsupportedDevice;
    // intel_step_init(): an exact entry, else the next entry after a gap,
    // else STEP_FUTURE. Only revid 3 has an entry.
    Platform platform;
    platform.revisionInTable = id.revision == GlkB0Revid;
    platform.step = id.revision < GlkRevidTableSize ? GraphicsStep::B0 : GraphicsStep::Future;
    platform.valid = true;
    out = platform;
    return Status::Ok;
}

Status decodeTopology(const PciIdentity &id, const FuseReadings &fuses, Topology &out) {
    Topology t;
    Status status = identifyPlatform(id, t.platform);
    if (status != Status::Ok) return status;
    if (!fuses.present) return Status::MissingFuses;
    t.identity = id;
    t.fuses = fuses;
    // intel_sseu_set_info(sseu, 1, 3, 8): "BXT has a single slice and at
    // most 3 subslices".
    t.maxSlices = 1; t.maxSubslices = 3; t.maxEusPerSubslice = 8;
    t.sliceMask = static_cast<uint8_t>((fuses.fuse2 >> Gen8F2SEnaShift) & Gen8F2SEnaBits);
    if (t.sliceMask & ~1U) return Status::InconsistentFuses;
    if (!(t.sliceMask & 1)) return Status::NoRenderSlice;
    // The subslice disable field applies to every enabled slice.
    t.subsliceMask = static_cast<uint8_t>(((1U << t.maxSubslices) - 1) &
                                          ~((fuses.fuse2 >> Gen9F2SsDisShift) & Gen9F2SsDisBits));
    unsigned euTotal = 0;
    for (unsigned ss = 0; ss < t.maxSubslices; ++ss) {
        if (!(t.subsliceMask & (1U << ss))) continue;
        const uint32_t disabled = (fuses.euDisable0 >> (8*ss)) & 0xff;
        t.euMask[ss] = static_cast<uint8_t>(~disabled & 0xff);
        if (t.maxEusPerSubslice - popcount8(disabled) == 7) t.subslice7Eu |= static_cast<uint8_t>(1U << ss);
        euTotal += popcount8(t.euMask[ss]);
    }
    const unsigned subsliceTotal = popcount8(t.subsliceMask);
    if (!subsliceTotal) return Status::NoRenderSubslice;
    if (!euTotal) return Status::NoRenderEu;
    t.euTotal = static_cast<uint8_t>(euTotal);
    t.euPerSubslice = static_cast<uint8_t>((euTotal + subsliceTotal - 1) / subsliceTotal);
    t.hasSlicePg = false; // !IS_GEN9_LP() only
    t.hasSubslicePg = subsliceTotal > 1;
    t.hasEuPg = t.euPerSubslice > 2;
    t.hasPooledEu = subsliceTotal == 3;
    if (t.hasPooledEu) {
        const auto disabled = [&](unsigned ss) { return !(t.subsliceMask & (1U << ss)); };
        t.minEuInPool = disabled(2) || disabled(0) ? 3 : disabled(1) ? 6 : 9;
    }
    out = t;
    return Status::Ok;
}

Status checkTopology(const Topology &topology) {
    Topology fresh;
    const Status status = decodeTopology(topology.identity, topology.fuses, fresh);
    if (status != Status::Ok) return status;
    return sameTopology(fresh, topology) ? Status::Ok : Status::InvalidTopology;
}

Status defaultSseu(const Topology &topology, SseuRequest &out) {
    const Status status = checkTopology(topology);
    if (status != Status::Ok) return status;
    SseuRequest request;
    request.sliceMask = topology.sliceMask;
    request.subsliceMask = topology.subsliceMask;
    request.minEusPerSubslice = topology.maxEusPerSubslice;
    request.maxEusPerSubslice = topology.maxEusPerSubslice;
    out = request;
    return Status::Ok;
}

Status makeRpcs(const Topology &topology, uint32_t &powerClockState) {
    SseuRequest request;
    const Status status = defaultSseu(topology, request);
    if (status != Status::Ok) return status;
    uint32_t rpcs = 0;
    if (topology.hasSlicePg) {
        const uint32_t value = popcount8(request.sliceMask) << RpcsSCntShift;
        if (value & ~RpcsSCntMask) return Status::InvalidTopology;
        rpcs |= RpcsEnable | RpcsSCntEnable | value;
    }
    if (topology.hasSubslicePg) {
        const uint32_t value = popcount8(request.subsliceMask) << RpcsSsCntShift;
        if (value & ~RpcsSsCntMask) return Status::InvalidTopology;
        rpcs |= RpcsEnable | RpcsSsCntEnable | value;
    }
    if (topology.hasEuPg) {
        const uint32_t low = uint32_t(request.minEusPerSubslice) << RpcsEuMinShift;
        const uint32_t high = uint32_t(request.maxEusPerSubslice) << RpcsEuMaxShift;
        if ((low & ~RpcsEuMinMask) || (high & ~RpcsEuMaxMask)) return Status::InvalidTopology;
        rpcs |= low | high | RpcsEnable;
    }
    powerClockState = rpcs;
    return Status::Ok;
}

Status buildWaContext(const Topology &topology, const WaPlacement &placement, uint8_t *object,
                      size_t objectBytes, WaContext &out) {
    // Every input is consumed into locals before the object is written, so
    // inputs stored inside the destination stay valid.
    uint32_t rpcs = 0;
    Status status = makeRpcs(topology, rpcs);
    if (status != Status::Ok) return status;
    const bool pooledEu = topology.hasPooledEu;
    if (!object || objectBytes != WaContextBytes || overlaps(object, objectBytes, &out, sizeof out))
        return Status::InvalidBuffer;
    const uint32_t waGgtt = placement.waContextGgtt, scratchGgtt = placement.scratchGgtt;
    if (!ggttPage(waGgtt)) return Status::InvalidWaContextAddress;
    if (!ggttPage(scratchGgtt)) return Status::InvalidScratchAddress;
    if (waGgtt == scratchGgtt) return Status::AddressOverlap;

    uint32_t storage[IndirectCtxMaxDwords];
    Words batch(storage, IndirectCtxMaxDwords);
    indirectContextBatch(batch, scratchGgtt, pooledEu);
    const uint32_t indirectBytes = 4*batch.size();
    // lrc_init_wa_ctx() GEM_BUG_ON()s and the INDIRECT_CTX size field limit.
    if (batch.overflow() || indirectBytes % CachelineBytes || indirectBytes > 63*CachelineBytes)
        return Status::InvalidBuffer;

    WaContext result;
    result.indirectOffset = 0;
    result.indirectBytes = indirectBytes;
    result.perCtxOffset = indirectBytes; // per_ctx starts where indirect_ctx ends
    result.perCtxBytes = 0;              // wa_bb_fn[1] == NULL on Gen9
    result.mediaPoolState = pooledEu;
    result.render.indirectCtxGgtt = waGgtt + result.indirectOffset;
    result.render.indirectCtxBytes = indirectBytes;
    result.render.powerClockState = rpcs;

    for (unsigned i = 0; i < batch.size(); ++i) write32(object + 4*i, batch.data()[i]);
    for (size_t i = indirectBytes; i < objectBytes; ++i) object[i] = 0;
    out = result;
    return Status::Ok;
}

Status contextWorkarounds(const Platform &platform, ContextWorkaround (&out)[ContextWorkaroundCount]) {
    if (!validPlatform(platform)) return Status::MissingIdentity;
    WorkaroundList wal;
    glkContextWorkarounds(wal);
    if (wal.failed() || wal.count() != ContextWorkaroundCount) return Status::InvalidBuffer;
    for (unsigned i = 0; i < ContextWorkaroundCount; ++i) out[i] = wal[i];
    return Status::Ok;
}

Status emitContextWorkarounds(const Platform &platform, uint8_t *ring, size_t ringBytes,
                              size_t &writtenBytes) {
    if (!validPlatform(platform)) return Status::MissingIdentity;
    constexpr size_t Bytes = 4*ContextWorkaroundRingDwords;
    if (!ring || ringBytes < Bytes || overlaps(ring, Bytes, &writtenBytes, sizeof writtenBytes))
        return Status::InvalidBuffer;
    WorkaroundList wal;
    glkContextWorkarounds(wal);
    if (wal.failed() || wal.count() != ContextWorkaroundCount) return Status::InvalidBuffer;

    uint32_t storage[ContextWorkaroundRingDwords];
    Words words(storage, ContextWorkaroundRingDwords);
    flushRcsBarrier(words);
    // Masked entries load wa->set without reading the register.
    words.put(miLoadRegisterImm(wal.count()));
    for (unsigned i = 0; i < wal.count(); ++i) { words.put(wal[i].offset); words.put(wal[i].set); }
    words.put(MiNoop);
    flushRcsBarrier(words);
    if (words.overflow() || words.size() != ContextWorkaroundRingDwords) return Status::InvalidBuffer;

    for (unsigned i = 0; i < words.size(); ++i) write32(ring + 4*i, storage[i]);
    writtenBytes = Bytes;
    return Status::Ok;
}

} // namespace MellowNativeGlk::RenderWa

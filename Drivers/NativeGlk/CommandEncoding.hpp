// SPDX-License-Identifier: MIT
// Copyright © 2003-2018 Intel Corporation
// Copyright © 2008,2010 Intel Corporation
// Copyright © 2013 Intel Corporation
// Copyright © 2014 Intel Corporation
// Copyright © 2016 Intel Corporation
// Copyright © 2018 Intel Corporation
// Copyright © 2019 Intel Corporation
// Copyright (c) 2026 Mellow contributors. See CommandEncoding.LICENSE.MIT and
// CommandEncoding.provenance.json.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeGlk {

// Gen9 BCS0 MI command construction for the first non-display GPU write on
// Gemini Lake. EncodedCommand holds logical CPU-order DWORDs; CommandWriter,
// decodeCommand and checkCommandImage use explicit little-endian bytes. No
// ring, context, GTT mapping, MMIO, interrupt or submission is owned or
// performed, and no result of this unit shows that a GPU executed anything.

// Command-format identifiers, not device admission. Gen9 is i915
// GRAPHICS_VER 9, the format GEN9_LP_FEATURES (i915_pci.c) selects for GLK.
// Only BCS0 forms are represented. RCS0 flushes and breadcrumbs are
// PIPE_CONTROL (gen8_emit_flush_rcs, gen8_emit_fini_breadcrumb_rcs) and VCS
// flushes add MI_INVALIDATE_BSD, so every engine except BCS0 is rejected.
enum class Generation : uint8_t { Gen9 = 9 };
enum class Engine : uint8_t { Rcs0 = 1, Bcs0 = 2, Vcs0 = 3, Vecs0 = 4 };

// Where the words execute. Ring is the logical ring context's ring buffer.
// PpgttBatch is a first-level batch entered by a non-secure
// MI_BATCH_BUFFER_START_GEN8 (bit 8). Privileged GGTT batch payloads are not
// built: no inspected Gen8+ source emits a store inside one.
enum class Site : uint8_t { Ring = 1, PpgttBatch = 2 };
enum class AddressSpace : uint8_t { Ggtt = 1, Ppgtt = 2 };

enum class Status : uint8_t {
    Ok, UnsupportedGeneration, UnsupportedEngine, InvalidSite, InvalidArgument,
    InvalidCommand,          // unknown kind/selector, or a field the kind does not use is set
    UnsupportedAtSite,       // the inspected sources do not emit this command here
    InvalidAddressSpace, UnsupportedAddressSpace,
    MisalignedAddress, AddressOutOfRange, ReservedAddressBit,
    UnrecognizedCommand,     // header is not a represented MI command
    InvalidLength,           // header length field differs from the represented form
    UnsupportedField,        // other header or operand bits not emitted by the sources
    Truncated, InsufficientSpace, UnalignedStream, MissingBatchEnd, TrailingData,
    InvalidProbe, ProbeAliasesCommands, ProbeAliasesBreadcrumb
};

// Exact i915 Gen8+ BCS emissions, from intel_gpu_commands.h, gen8_engine_cs.c/.h
// and the i915_cmd_parser.c gen9_blt_cmds lengths:
//   Noop            MI_NOOP 0x00000000
//   UserInterrupt   MI_USER_INTERRUPT 0x01000000 (fini breadcrumb tail)
//   ArbCheck        MI_ARB_CHECK 0x02800000 (gen8_emit_wa_tail)
//   ArbOnOff        MI_ARB_ON_OFF | MI_ARB_ENABLE/DISABLE 0x04000001/0x04000000
//   BatchBufferEnd  MI_BATCH_BUFFER_END 0x05000000
//   StoreDwordImm   MI_STORE_DWORD_IMM_GEN4 [| MI_USE_GGTT], addr lo, addr hi, value
//   FlushDw         (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW | ..., addr, 0, value
//   BatchBufferStart MI_BATCH_BUFFER_START_GEN8 [| BIT(8) PPGTT], addr lo, addr hi
enum class CommandKind : uint8_t {
    Noop = 1, UserInterrupt, ArbCheck, ArbOnOff, BatchBufferEnd,
    StoreDwordImm, FlushDw, BatchBufferStart
};
enum class FlushPostSync : uint8_t {
    // gen8_emit_flush_xcs: STORE_INDEX | OP_STOREDW of 0 to the per-context
    // HWSP index LRC_PPHWSP_SCRATCH_ADDR; invalidateTlb adds
    // MI_INVALIDATE_TLB (EMIT_INVALIDATE). space and address stay zero.
    ScratchIndex = 1,
    // gen8_emit_ggtt_write: OP_STOREDW of value to address | MI_FLUSH_DW_USE_GTT.
    // space must be Ggtt; address 8-byte aligned with bit 5 clear (w/a).
    GgttStoreDword = 2
};

// Fields a kind does not use must stay zero; they are never silently dropped.
struct Command {
    CommandKind kind {};
    AddressSpace space {};     // StoreDwordImm, BatchBufferStart, FlushDw GgttStoreDword
    uint64_t address {};       // same kinds
    uint32_t value {};         // StoreDwordImm, FlushDw GgttStoreDword
    FlushPostSync postSync {}; // FlushDw
    bool invalidateTlb {};     // FlushDw ScratchIndex
    bool enable {};            // ArbOnOff
};

constexpr uint32_t PphwspScratchIndex = 0x34 * 4; // LRC_PPHWSP_SCRATCH_ADDR
// GGTT command offsets are u32 with a zero upper dword in every inspected
// emitter. PPGTT addresses stay in the lower half of the 48-bit GLK PPGTT,
// where i915's canonical (gen8_canonical_addr) and raw offsets coincide.
constexpr uint64_t GgttAddressLimit = 1ULL << 32;
constexpr uint64_t PpgttAddressLimit = 1ULL << 47;

// Site rules (anything else is UnsupportedAtSite/UnsupportedAddressSpace):
//   Ring: Noop, UserInterrupt, ArbCheck, ArbOnOff, FlushDw, BatchBufferStart
//         (Ggtt = I915_DISPATCH_SECURE, Ppgtt = non-secure), StoreDwordImm Ggtt.
//   PpgttBatch: Noop, BatchBufferEnd, StoreDwordImm Ppgtt (igt_emit_store_dw).
// StoreDwordImm needs 4-byte alignment (alignof_dword); BatchBufferStart needs
// 8 (execbuffer's batch_start_offset rule; the hardware minimum is not
// established here).
constexpr uint32_t MaxCommandDwords = 4;
struct EncodedCommand {
    uint32_t dwords[MaxCommandDwords];
    uint32_t count;
};
// Output is unchanged on failure.
Status encodeCommand(Generation, Engine, Site, const Command &, EncodedCommand &out);

// Strict inverse of encodeCommand on little-endian bytes: accepts only words
// encodeCommand would produce for this site. Every foreign MI opcode, every
// non-MI client (XY_* blits included) and every unrepresented bit pattern is
// rejected. Outputs are unchanged on failure.
Status decodeCommand(Generation, Engine, Site, const uint8_t *bytes, size_t availableBytes,
                     Command &out, uint32_t &dwords);

// Byte 4i+k receives bits 8k+7:8k of words[i], independent of host order.
Status storeLittleEndian(const uint32_t *words, size_t count, uint8_t *bytes, size_t capacityBytes);
uint32_t loadLittleEndian(const uint8_t bytes[4]);

// Allowlist re-check of a complete image built by this unit. Images must be
// qword sized (intel_ring_begin, intel_ring_offset_valid; execbuffer
// batch_len). A batch must end with MI_BATCH_BUFFER_END, followed only by the
// single MI_NOOP that completes a qword. This is not i915_cmd_parser and not a
// security validator for userspace batches. `commands` counts every decoded
// command, padding included, and is written only on success.
Status checkCommandImage(Generation, Engine, Site, const uint8_t *bytes, size_t sizeBytes,
                         size_t &commands);

// First-write probe. The CPU fills target with cpuInitial; only the ring
// command carries nonce. nonce must differ from cpuInitial, 0 (zero-filled
// pages) and 0xFFFFFFFF (failed reads), and the target and breadcrumb
// dwords must not overlap the ring range or each other, so a nonce read back
// cannot come from CPU-written command bytes. Physical aliasing between
// distinct GGTT pages is the memory owner's precondition and not checked.
struct GpuRange {
    AddressSpace space {};
    uint64_t base {}, bytes {};
};
struct StoreProbe {
    uint64_t target {}; // GGTT
    uint32_t nonce {}, cpuInitial {};
};
struct StoreProbeRing {
    StoreProbe probe;
    GpuRange ring;          // GGTT range of the whole ring buffer
    uint64_t breadcrumb {}; // GGTT dword the post-sync flush writes (HWSP seqno)
    uint32_t breadcrumbValue {};
};
Status checkStoreProbeRing(const StoreProbeRing &);

// Appends little-endian commands to caller storage, such as a CPU mapping of
// ring or batch backing. Nothing is published to hardware. Each call is
// transactional: on failure no byte changes and bytesWritten() is unchanged.
class CommandWriter {
public:
    Status begin(Generation, Engine, Site, uint8_t *bytes, size_t capacityBytes);
    Status append(const Command &);
    Status padToQword();  // one MI_NOOP when the dword count is odd
    Status finishBatch(); // PpgttBatch: MI_BATCH_BUFFER_END, then padToQword
    size_t bytesWritten() const { return used_; }

    // Appends the StoreProbeRingDwords sequence below after checkStoreProbeRing.
    Status appendStoreProbeRing(const StoreProbeRing &);

private:
    Status put(const EncodedCommand *, size_t count);
    Generation generation_ {};
    Engine engine_ {};
    Site site_ {};
    uint8_t *bytes_ {};
    size_t capacity_ {}, used_ {};
    bool ready_ {}, ended_ {};
};

// The i915 BCS request shape, without the init breadcrumb and the semaphore
// preempt busywait (not represented here):
//   MI_FLUSH_DW scratch + MI_INVALIDATE_TLB    execlists_request_alloc
//   MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT      target, 0, nonce
//   MI_FLUSH_DW post-sync GGTT store           breadcrumb, 0, breadcrumbValue
//   MI_USER_INTERRUPT, MI_ARB_ON_OFF | MI_ARB_ENABLE
//   MI_ARB_CHECK, MI_NOOP                      gen8_emit_wa_tail
// Requires a Ring writer at a qword offset. Tail is rq->tail (the first
// RING_TAIL), WaTail is rq->wa_tail (WaIdleLiteRestore), relative to the start.
constexpr uint32_t StoreProbeRingDwords = 16;
constexpr uint32_t StoreProbeRingTailBytes = 56, StoreProbeRingWaTailBytes = 64;

// A matching value alone does not show GPU execution: completion still needs
// the breadcrumb and execlists context-switch evidence of the submission owner.
enum class ProbeReading : uint8_t { Invalid = 0, Unchanged = 1, NonceObserved, Foreign };
ProbeReading classifyProbeReading(const StoreProbe &, const uint8_t observed[4]);

} // namespace MellowNativeGlk

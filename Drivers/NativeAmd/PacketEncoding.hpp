// SPDX-License-Identifier: MIT
// Copyright 2014 Advanced Micro Devices, Inc.
// Copyright 2016 Advanced Micro Devices, Inc.
// Copyright 2016-2022 Advanced Micro Devices, Inc.
// Copyright (C) 2017  Advanced Micro Devices, Inc.
// Copyright 2018 Advanced Micro Devices, Inc. All Rights Reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeAmd {

// Outputs are logical CPU-order DWORD values. A future ring writer must
// explicitly serialize each DWORD little-endian; copying their host memory
// image is valid only with a separately established little-endian contract.
// No ring writer or native GPU ownership is supplied by this unit.

// Packet-format identifier, not PCI device admission or GPU support. Gfx9 is
// the SOC15 PM4 format of Linux soc15d.h as emitted by gfx_v9_0.c on GFX and
// compute rings. GFX10/11/12 formats were not inspected and have no enumerator.
enum class Architecture : uint8_t { Gfx9 = 9 };
enum class Status : uint8_t {
    Ok, UnsupportedArchitecture, InvalidOpcode, InvalidCount, UnsupportedCount,
    NotType3, ReservedBits, InvalidLength, WrongPacket, UnsupportedField,
    InvalidAddress, UnsupportedAddress, InvalidSequence
};

constexpr uint32_t OpcodeNop = 0x10;        // PACKET3_NOP
constexpr uint32_t OpcodeReleaseMem = 0x49; // PACKET3_RELEASE_MEM
constexpr uint32_t Type3BodyDwordsMax = 0x3FFF;

struct Type3Header {
    uint32_t opcode {};
    bool compute {};          // PACKET3_COMPUTE, header bit 1
    uint32_t bodyDwords {};   // zero only for the one-dword NOP
    uint32_t packetDwords {}; // header plus body
};

// Type-3 header: type [31:30] = 3, count [29:16] = body dwords - 1, opcode
// [15:8]; bits [7:0] are zero except bit 1 set by PACKET3_COMPUTE. The only
// header-only packet is the one-dword NOP 0xFFFF1000 (count 0x3FFF) that
// gfx_v9_0.c writes as ring->funcs->nop; request it with NOP and bodyDwords 0.
// Count 0x3FFF is otherwise neither emitted nor accepted: for NOP it selects
// the one-dword form, and gfx_v9_0.c caps NOP bodies below it, so its meaning
// for other opcodes is not established. Opcode bodies are not validated.
// Output is unchanged on failure.
Status encodeType3Header(Architecture, uint32_t opcode, uint32_t bodyDwords,
                         bool compute, uint32_t &out);
Status decodeType3Header(Architecture, uint32_t header, Type3Header &out);
// Decodes words[0] and requires the complete packet within `available` dwords.
Status measureType3Packet(Architecture, const uint32_t *words, size_t available,
                          Type3Header &out);

// RELEASE_MEM end-of-pipe completion write, limited to the variants that
// gfx_v9_0_ring_emit_fence emits for amdgpu fence flags 64BIT, INT and
// TC_WB_ONLY. EOP_EXEC (trailing fence), DST_SEL TC/L2, other events, cache
// policies, data and interrupt selectors are rejected as not established.
// KIQ fences use WRITE_DATA instead and are not represented.
enum class FenceEvent : uint8_t { CacheFlushAndInvTsEvent = 0x14 }; // EVENT_INDEX 5
enum class FenceCache : uint8_t {
    DefaultActions = 1, // EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN | EOP_TC_MD_ACTION_EN
    WritebackOnly = 2   // EOP_TC_NC_ACTION_EN (AMDGPU_FENCE_FLAG_TC_WB_ONLY)
};                      // Both also set EOP_TC_WB_ACTION_EN.
enum class FenceDestination : uint8_t { MemoryController = 0 };         // DST_SEL
enum class FenceData : uint8_t { Low32 = 1, Full64 = 2 };               // DATA_SEL
enum class FenceInterrupt : uint8_t { None = 0, AfterWriteConfirm = 2 }; // INT_SEL

// Zero is not a valid architecture, event, cache or data value, so a
// zero-initialized request is rejected.
struct ReleaseMemFence {
    Architecture architecture {};
    FenceEvent event {};
    FenceCache cache {};
    FenceDestination destination {};
    FenceData data {};
    FenceInterrupt interrupt {};
    uint64_t address {};  // GPU VA: 8-byte aligned for Full64, 4-byte for Low32
    uint64_t sequence {}; // Low32 requires a value that fits 32 bits
};
constexpr uint32_t ReleaseMemDwords = 8;
struct ReleaseMemPacket { uint32_t dwords[ReleaseMemDwords]; };

// Addresses must lie below AMDGPU_GMC_HOLE_START of the 48-bit GMC layout,
// where the sign-extended and hole-masked forms coincide. Upper-half addresses
// are rejected: which form the CP consumes here was not established.
constexpr uint64_t FenceAddressLimit = 0x0000800000000000ULL;

// No allocation, VM mapping, ring publication, MMIO, interrupt routing or GPU
// execution occurs. Output is unchanged on failure.
Status encodeReleaseMemFence(const ReleaseMemFence &, ReleaseMemPacket &out);
// Strict inverse of encodeReleaseMemFence; every other bit pattern is rejected.
Status decodeReleaseMemFence(Architecture, const ReleaseMemPacket &,
                             ReleaseMemFence &out);

} // namespace MellowNativeAmd

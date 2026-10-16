// SPDX-License-Identifier: MIT
// Copyright 2014 Advanced Micro Devices, Inc.
// Copyright 2016 Advanced Micro Devices, Inc.
// Copyright 2016-2022 Advanced Micro Devices, Inc.
// Copyright (C) 2017  Advanced Micro Devices, Inc.
// Copyright 2018 Advanced Micro Devices, Inc. All Rights Reserved.
// Copyright (c) 2026 Mellow contributors. See LICENSE.MIT and provenance.json.
#include "PacketEncoding.hpp"

namespace MellowNativeAmd {
namespace {
// soc15d.h PM4 header: PACKET_TYPE3 in [31:30], count [29:16], opcode [15:8].
constexpr uint32_t PacketType3 = 3;
constexpr uint32_t CountMask = 0x3FFF;
constexpr uint32_t OpcodeMask = 0xFF;
constexpr uint32_t ComputeBit = 1U << 1;
constexpr uint32_t OneDwordNopCount = 0x3FFF;

// RELEASE_MEM: vega10_enum.h CACHE_FLUSH_AND_INV_TS_EVENT, the
// kfd_pm4_headers_ai.h end_of_pipe event index, and soc15d.h EOP_* bits.
constexpr uint32_t EndOfPipeEventIndex = 5;
constexpr uint32_t EopTcWbActionEn = 1U << 15;
constexpr uint32_t EopTcl1ActionEn = 1U << 16;
constexpr uint32_t EopTcActionEn = 1U << 17;
constexpr uint32_t EopTcNcActionEn = 1U << 19;
constexpr uint32_t EopTcMdActionEn = 1U << 21;
constexpr uint32_t DstSelShift = 16, IntSelShift = 24, DataSelShift = 29;

uint32_t packHeader(uint32_t opcode, uint32_t count, bool compute) {
    return (PacketType3 << 30) | (count << 16) | (opcode << 8) | (compute ? ComputeBit : 0);
}
bool cacheActions(FenceCache cache, uint32_t &bits) {
    switch (cache) {
        case FenceCache::DefaultActions:
            bits = EopTcl1ActionEn | EopTcActionEn | EopTcMdActionEn;
            return true;
        case FenceCache::WritebackOnly:
            bits = EopTcNcActionEn;
            return true;
    }
    return false;
}
bool knownData(FenceData data) {
    return data == FenceData::Low32 || data == FenceData::Full64;
}
bool knownInterrupt(FenceInterrupt interrupt) {
    return interrupt == FenceInterrupt::None || interrupt == FenceInterrupt::AfterWriteConfirm;
}
uint32_t eventControl(uint32_t cacheBits) {
    return static_cast<uint32_t>(FenceEvent::CacheFlushAndInvTsEvent) |
           (EndOfPipeEventIndex << 8) | EopTcWbActionEn | cacheBits;
}
uint32_t selectControl(FenceData data, FenceInterrupt interrupt) {
    return (static_cast<uint32_t>(data) << DataSelShift) |
           (static_cast<uint32_t>(interrupt) << IntSelShift) |
           (static_cast<uint32_t>(FenceDestination::MemoryController) << DstSelShift);
}
} // namespace

Status encodeType3Header(Architecture architecture, uint32_t opcode, uint32_t bodyDwords,
                         bool compute, uint32_t &out) {
    if (architecture != Architecture::Gfx9) return Status::UnsupportedArchitecture;
    if (opcode > OpcodeMask) return Status::InvalidOpcode;
    uint32_t count = 0;
    if (bodyDwords == 0) {
        // Type-3 packets carry at least one body dword except the plain NOP
        // gfx_v9_0.c uses as its one-dword ring NOP.
        if (opcode != OpcodeNop) return Status::InvalidCount;
        if (compute) return Status::UnsupportedCount;
        count = OneDwordNopCount;
    } else if (bodyDwords <= Type3BodyDwordsMax) {
        count = bodyDwords - 1;
    } else if (bodyDwords == Type3BodyDwordsMax + 1 && opcode != OpcodeNop) {
        return Status::UnsupportedCount;
    } else {
        return Status::InvalidCount;
    }
    out = packHeader(opcode, count, compute);
    return Status::Ok;
}

Status decodeType3Header(Architecture architecture, uint32_t header, Type3Header &out) {
    if (architecture != Architecture::Gfx9) return Status::UnsupportedArchitecture;
    if ((header >> 30) != PacketType3) return Status::NotType3;
    if ((header & OpcodeMask & ~ComputeBit) != 0) return Status::ReservedBits;
    Type3Header decoded;
    decoded.opcode = (header >> 8) & OpcodeMask;
    decoded.compute = (header & ComputeBit) != 0;
    const uint32_t count = (header >> 16) & CountMask;
    if (count == OneDwordNopCount) {
        if (decoded.opcode != OpcodeNop || decoded.compute) return Status::UnsupportedCount;
        decoded.bodyDwords = 0;
    } else {
        decoded.bodyDwords = count + 1;
    }
    decoded.packetDwords = decoded.bodyDwords + 1;
    out = decoded;
    return Status::Ok;
}

Status measureType3Packet(Architecture architecture, const uint32_t *words, size_t available,
                          Type3Header &out) {
    if (architecture != Architecture::Gfx9) return Status::UnsupportedArchitecture;
    if (words == nullptr || available == 0) return Status::InvalidLength;
    Type3Header decoded;
    const Status status = decodeType3Header(architecture, words[0], decoded);
    if (status != Status::Ok) return status;
    if (decoded.packetDwords > available) return Status::InvalidLength;
    out = decoded;
    return Status::Ok;
}

Status encodeReleaseMemFence(const ReleaseMemFence &fence, ReleaseMemPacket &out) {
    if (fence.architecture != Architecture::Gfx9) return Status::UnsupportedArchitecture;
    uint32_t cacheBits = 0;
    if (fence.event != FenceEvent::CacheFlushAndInvTsEvent || !cacheActions(fence.cache, cacheBits) ||
        fence.destination != FenceDestination::MemoryController || !knownData(fence.data) ||
        !knownInterrupt(fence.interrupt))
        return Status::UnsupportedField;
    // gfx_v9_0.c: qword-aligned for 64-bit data, dword-aligned for low 32 bits.
    const uint64_t alignment = fence.data == FenceData::Full64 ? 8 : 4;
    if ((fence.address & (alignment - 1)) != 0) return Status::InvalidAddress;
    if (fence.address >= FenceAddressLimit) return Status::UnsupportedAddress;
    if (fence.data == FenceData::Low32 && fence.sequence > 0xFFFFFFFFULL)
        return Status::InvalidSequence;

    ReleaseMemPacket encoded;
    encoded.dwords[0] = packHeader(OpcodeReleaseMem, ReleaseMemDwords - 2, false);
    encoded.dwords[1] = eventControl(cacheBits);
    encoded.dwords[2] = selectControl(fence.data, fence.interrupt);
    encoded.dwords[3] = static_cast<uint32_t>(fence.address);
    encoded.dwords[4] = static_cast<uint32_t>(fence.address >> 32);
    encoded.dwords[5] = static_cast<uint32_t>(fence.sequence);
    encoded.dwords[6] = static_cast<uint32_t>(fence.sequence >> 32);
    encoded.dwords[7] = 0; // INT_CTXID; gfx_v9_0.c writes zero.
    out = encoded;
    return Status::Ok;
}

Status decodeReleaseMemFence(Architecture architecture, const ReleaseMemPacket &packet,
                             ReleaseMemFence &out) {
    Type3Header header;
    const Status headerStatus = decodeType3Header(architecture, packet.dwords[0], header);
    if (headerStatus != Status::Ok) return headerStatus;
    if (header.opcode != OpcodeReleaseMem || header.bodyDwords != ReleaseMemDwords - 1)
        return Status::WrongPacket;
    if (header.compute) return Status::UnsupportedField;

    ReleaseMemFence decoded {architecture, FenceEvent::CacheFlushAndInvTsEvent,
                             FenceCache::DefaultActions, FenceDestination::MemoryController,
                             FenceData::Full64, FenceInterrupt::None, 0, 0};
    constexpr FenceCache caches[] = {FenceCache::DefaultActions, FenceCache::WritebackOnly};
    constexpr FenceData widths[] = {FenceData::Low32, FenceData::Full64};
    constexpr FenceInterrupt interrupts[] = {FenceInterrupt::None, FenceInterrupt::AfterWriteConfirm};
    bool controlKnown = false, selectKnown = false;
    for (FenceCache cache : caches) {
        uint32_t cacheBits = 0;
        if (cacheActions(cache, cacheBits) && packet.dwords[1] == eventControl(cacheBits)) {
            decoded.cache = cache;
            controlKnown = true;
        }
    }
    for (FenceData data : widths)
        for (FenceInterrupt interrupt : interrupts)
            if (packet.dwords[2] == selectControl(data, interrupt)) {
                decoded.data = data;
                decoded.interrupt = interrupt;
                selectKnown = true;
            }
    if (!controlKnown || !selectKnown || packet.dwords[7] != 0) return Status::UnsupportedField;
    decoded.address = packet.dwords[3] | (static_cast<uint64_t>(packet.dwords[4]) << 32);
    decoded.sequence = packet.dwords[5] | (static_cast<uint64_t>(packet.dwords[6]) << 32);

    // The remaining words are raw values; re-encoding applies the same address,
    // alignment and sequence-width rules, and every field is now accounted for.
    ReleaseMemPacket image;
    const Status status = encodeReleaseMemFence(decoded, image);
    if (status != Status::Ok) return status;
    out = decoded;
    return Status::Ok;
}

} // namespace MellowNativeAmd

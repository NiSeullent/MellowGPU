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
#include "CommandEncoding.hpp"

namespace MellowNativeGlk {
namespace {
// intel_gpu_commands.h: MI_INSTR(opcode, flags) puts INSTR_MI_CLIENT (0) in
// [31:29] and the opcode in [28:23].
constexpr unsigned MiOpcodeShift = 23;
constexpr uint32_t miInstr(uint32_t opcode, uint32_t flags) { return opcode << MiOpcodeShift | flags; }
constexpr uint32_t MiNoop = miInstr(0x00, 0);
constexpr uint32_t MiUserInterrupt = miInstr(0x02, 0);
constexpr uint32_t MiArbCheck = miInstr(0x05, 0);
constexpr uint32_t MiArbOnOff = miInstr(0x08, 0), MiArbEnable = 1U << 0;
constexpr uint32_t MiBatchBufferEnd = miInstr(0x0a, 0);
constexpr uint32_t MiStoreDwordImmGen4 = miInstr(0x20, 2), MiUseGgtt = 1U << 22;
// gen8_engine_cs.c/.h emit MI_FLUSH_DW + 1, the four-dword Gen8+ form.
constexpr uint32_t MiFlushDwGen8 = miInstr(0x26, 1) + 1;
constexpr uint32_t FlushDwStoreIndex = 1U << 21, MiInvalidateTlb = 1U << 18;
constexpr uint32_t FlushDwOpStoreDw = 1U << 14, FlushDwUseGtt = 1U << 2;
constexpr uint32_t FlushDwReservedAddressBit = 1U << 5; // gen8_emit_ggtt_write w/a
// gen8_emit_bb_start: BIT(8) unless I915_DISPATCH_SECURE (MI_BATCH_PPGTT_HSW).
constexpr uint32_t MiBatchBufferStartGen8 = miInstr(0x31, 1), BatchPpgtt = 1U << 8;
// i915_cmd_parser.c gen9_blt_cmds length masks; length = field + LENGTH_BIAS.
constexpr uint32_t StoreDwordLengthMask = 0x3FF, FlushDwLengthMask = 0x3F, BatchStartLengthMask = 0xFF;
constexpr uint64_t DwordAlign = 4, QwordAlign = 8; // alignof_dword, alignof_qword

enum : unsigned { UsesSpace = 1, UsesAddress = 2, UsesValue = 4, UsesPostSync = 8, UsesTlb = 16, UsesEnable = 32 };
bool onlyUses(const Command &c, unsigned used) {
    return ((used & UsesSpace) || c.space == AddressSpace {}) && ((used & UsesAddress) || c.address == 0) &&
           ((used & UsesValue) || c.value == 0) && ((used & UsesPostSync) || c.postSync == FlushPostSync {}) &&
           ((used & UsesTlb) || !c.invalidateTlb) && ((used & UsesEnable) || !c.enable);
}
bool knownSpace(AddressSpace space) { return space == AddressSpace::Ggtt || space == AddressSpace::Ppgtt; }

Status checkProfile(Generation generation, Engine engine, Site site) {
    if (generation != Generation::Gen9) return Status::UnsupportedGeneration;
    if (engine != Engine::Bcs0) return Status::UnsupportedEngine;
    if (site != Site::Ring && site != Site::PpgttBatch) return Status::InvalidSite;
    return Status::Ok;
}
Status checkAddress(AddressSpace space, uint64_t address, uint64_t alignment) {
    if (address % alignment != 0) return Status::MisalignedAddress;
    if (address >= (space == AddressSpace::Ggtt ? GgttAddressLimit : PpgttAddressLimit))
        return Status::AddressOutOfRange;
    return Status::Ok;
}
// The address gen8_emit_ggtt_write accepts: GGTT, 8-byte aligned, bit 5 clear.
Status checkFlushTarget(uint64_t address) {
    const Status status = checkAddress(AddressSpace::Ggtt, address, QwordAlign);
    if (status == Status::Ok && (address & FlushDwReservedAddressBit) != 0) return Status::ReservedAddressBit;
    return status;
}

// Shared by encode and decode, so the decoder accepts no rule the encoder lacks.
Status validate(Site site, const Command &c) {
    const bool ring = site == Site::Ring;
    switch (c.kind) {
    case CommandKind::Noop:
        return onlyUses(c, 0) ? Status::Ok : Status::InvalidCommand;
    case CommandKind::UserInterrupt:
    case CommandKind::ArbCheck:
        if (!onlyUses(c, 0)) return Status::InvalidCommand;
        return ring ? Status::Ok : Status::UnsupportedAtSite;
    case CommandKind::ArbOnOff:
        if (!onlyUses(c, UsesEnable)) return Status::InvalidCommand;
        return ring ? Status::Ok : Status::UnsupportedAtSite;
    case CommandKind::BatchBufferEnd:
        if (!onlyUses(c, 0)) return Status::InvalidCommand;
        return ring ? Status::UnsupportedAtSite : Status::Ok;
    case CommandKind::StoreDwordImm:
        if (!onlyUses(c, UsesSpace | UsesAddress | UsesValue)) return Status::InvalidCommand;
        if (!knownSpace(c.space)) return Status::InvalidAddressSpace;
        // Ring: GGTT (gen8_emit_init_breadcrumb). Batch: PPGTT (igt_emit_store_dw).
        if ((c.space == AddressSpace::Ggtt) != ring) return Status::UnsupportedAddressSpace;
        return checkAddress(c.space, c.address, DwordAlign);
    case CommandKind::FlushDw:
        if (c.postSync == FlushPostSync::ScratchIndex) {
            if (!onlyUses(c, UsesPostSync | UsesTlb)) return Status::InvalidCommand;
        } else if (c.postSync == FlushPostSync::GgttStoreDword) {
            if (!onlyUses(c, UsesPostSync | UsesSpace | UsesAddress | UsesValue)) return Status::InvalidCommand;
        } else {
            return Status::InvalidCommand;
        }
        if (!ring) return Status::UnsupportedAtSite;
        if (c.postSync == FlushPostSync::ScratchIndex) return Status::Ok;
        if (!knownSpace(c.space)) return Status::InvalidAddressSpace;
        if (c.space != AddressSpace::Ggtt) return Status::UnsupportedAddressSpace;
        return checkFlushTarget(c.address);
    case CommandKind::BatchBufferStart:
        if (!onlyUses(c, UsesSpace | UsesAddress)) return Status::InvalidCommand;
        if (!ring) return Status::UnsupportedAtSite;
        if (!knownSpace(c.space)) return Status::InvalidAddressSpace;
        return checkAddress(c.space, c.address, QwordAlign);
    }
    return Status::InvalidCommand;
}

uint32_t low32(uint64_t value) { return static_cast<uint32_t>(value); }
uint32_t high32(uint64_t value) { return static_cast<uint32_t>(value >> 32); }
// Requires a command that passed validate().
EncodedCommand build(const Command &c) {
    EncodedCommand out {};
    switch (c.kind) {
    case CommandKind::Noop: out = {{MiNoop}, 1}; break;
    case CommandKind::UserInterrupt: out = {{MiUserInterrupt}, 1}; break;
    case CommandKind::ArbCheck: out = {{MiArbCheck}, 1}; break;
    case CommandKind::ArbOnOff: out = {{MiArbOnOff | (c.enable ? MiArbEnable : 0)}, 1}; break;
    case CommandKind::BatchBufferEnd: out = {{MiBatchBufferEnd}, 1}; break;
    case CommandKind::StoreDwordImm:
        out = {{MiStoreDwordImmGen4 | (c.space == AddressSpace::Ggtt ? MiUseGgtt : 0),
                low32(c.address), high32(c.address), c.value}, 4};
        break;
    case CommandKind::FlushDw:
        if (c.postSync == FlushPostSync::ScratchIndex)
            out = {{MiFlushDwGen8 | FlushDwStoreIndex | FlushDwOpStoreDw | (c.invalidateTlb ? MiInvalidateTlb : 0),
                    PphwspScratchIndex, 0, 0}, 4};
        else
            out = {{MiFlushDwGen8 | FlushDwOpStoreDw, low32(c.address) | FlushDwUseGtt, 0, c.value}, 4};
        break;
    case CommandKind::BatchBufferStart:
        out = {{MiBatchBufferStartGen8 | (c.space == AddressSpace::Ppgtt ? BatchPpgtt : 0),
                low32(c.address), high32(c.address)}, 3};
        break;
    }
    return out;
}

// Header to kind, length and selector bits. Fixed one-dword commands have no
// length field: every bit besides the exact i915 value is unrepresented.
Status parseHeader(uint32_t header, Command &c, uint32_t &dwords) {
    if (header >> 29 != 0) return Status::UnrecognizedCommand; // not INSTR_MI_CLIENT
    dwords = 1;
    switch (header >> MiOpcodeShift) {
    case MiNoop >> MiOpcodeShift:
        c.kind = CommandKind::Noop;
        return header == MiNoop ? Status::Ok : Status::UnsupportedField;
    case MiUserInterrupt >> MiOpcodeShift:
        c.kind = CommandKind::UserInterrupt;
        return header == MiUserInterrupt ? Status::Ok : Status::UnsupportedField;
    case MiArbCheck >> MiOpcodeShift:
        c.kind = CommandKind::ArbCheck;
        return header == MiArbCheck ? Status::Ok : Status::UnsupportedField;
    case MiArbOnOff >> MiOpcodeShift:
        c.kind = CommandKind::ArbOnOff;
        c.enable = header == (MiArbOnOff | MiArbEnable);
        return header == MiArbOnOff || c.enable ? Status::Ok : Status::UnsupportedField;
    case MiBatchBufferEnd >> MiOpcodeShift:
        c.kind = CommandKind::BatchBufferEnd;
        return header == MiBatchBufferEnd ? Status::Ok : Status::UnsupportedField;
    case MiStoreDwordImmGen4 >> MiOpcodeShift:
        c.kind = CommandKind::StoreDwordImm;
        dwords = 4;
        if ((header & StoreDwordLengthMask) != (MiStoreDwordImmGen4 & StoreDwordLengthMask))
            return Status::InvalidLength;
        if (header != MiStoreDwordImmGen4 && header != (MiStoreDwordImmGen4 | MiUseGgtt))
            return Status::UnsupportedField;
        c.space = header & MiUseGgtt ? AddressSpace::Ggtt : AddressSpace::Ppgtt;
        return Status::Ok;
    case MiFlushDwGen8 >> MiOpcodeShift:
        c.kind = CommandKind::FlushDw;
        dwords = 4;
        if ((header & FlushDwLengthMask) != (MiFlushDwGen8 & FlushDwLengthMask)) return Status::InvalidLength;
        if (header == (MiFlushDwGen8 | FlushDwOpStoreDw)) {
            c.postSync = FlushPostSync::GgttStoreDword;
            c.space = AddressSpace::Ggtt;
            return Status::Ok;
        }
        if ((header & ~MiInvalidateTlb) != (MiFlushDwGen8 | FlushDwStoreIndex | FlushDwOpStoreDw))
            return Status::UnsupportedField;
        c.postSync = FlushPostSync::ScratchIndex;
        c.invalidateTlb = (header & MiInvalidateTlb) != 0;
        return Status::Ok;
    case MiBatchBufferStartGen8 >> MiOpcodeShift:
        c.kind = CommandKind::BatchBufferStart;
        dwords = 3;
        if ((header & BatchStartLengthMask) != (MiBatchBufferStartGen8 & BatchStartLengthMask))
            return Status::InvalidLength;
        if (header != MiBatchBufferStartGen8 && header != (MiBatchBufferStartGen8 | BatchPpgtt))
            return Status::UnsupportedField;
        c.space = header & BatchPpgtt ? AddressSpace::Ppgtt : AddressSpace::Ggtt;
        return Status::Ok;
    }
    return Status::UnrecognizedCommand;
}
// Reads only the dwords of the parsed kind; the caller checked availability.
Status parseOperands(const uint8_t *bytes, Command &c) {
    switch (c.kind) {
    case CommandKind::StoreDwordImm:
        c.address = loadLittleEndian(bytes + 4) | static_cast<uint64_t>(loadLittleEndian(bytes + 8)) << 32;
        c.value = loadLittleEndian(bytes + 12);
        return Status::Ok;
    case CommandKind::BatchBufferStart:
        c.address = loadLittleEndian(bytes + 4) | static_cast<uint64_t>(loadLittleEndian(bytes + 8)) << 32;
        return Status::Ok;
    case CommandKind::FlushDw: {
        const uint32_t address = loadLittleEndian(bytes + 4), upper = loadLittleEndian(bytes + 8);
        const uint32_t value = loadLittleEndian(bytes + 12);
        if (c.postSync == FlushPostSync::ScratchIndex)
            return address == PphwspScratchIndex && upper == 0 && value == 0 ? Status::Ok : Status::UnsupportedField;
        if ((address & FlushDwUseGtt) == 0) return Status::UnsupportedAddressSpace; // MI_FLUSH_DW_USE_PPGTT
        c.address = (address & ~FlushDwUseGtt) | static_cast<uint64_t>(upper) << 32;
        c.value = value;
        return Status::Ok;
    }
    case CommandKind::Noop:
    case CommandKind::UserInterrupt:
    case CommandKind::ArbCheck:
    case CommandKind::ArbOnOff:
    case CommandKind::BatchBufferEnd:
        return Status::Ok;
    }
    return Status::UnrecognizedCommand;
}

bool overlaps(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes) { return a < b + bBytes && b < a + aBytes; }
Command make(CommandKind kind) {
    Command c;
    c.kind = kind;
    return c;
}
} // namespace

Status encodeCommand(Generation generation, Engine engine, Site site, const Command &command,
                     EncodedCommand &out) {
    Status status = checkProfile(generation, engine, site);
    if (status == Status::Ok) status = validate(site, command);
    if (status == Status::Ok) out = build(command);
    return status;
}

Status decodeCommand(Generation generation, Engine engine, Site site, const uint8_t *bytes,
                     size_t availableBytes, Command &out, uint32_t &dwords) {
    Status status = checkProfile(generation, engine, site);
    if (status != Status::Ok) return status;
    if (!bytes && availableBytes) return Status::InvalidArgument;
    if (availableBytes < 4) return Status::Truncated;
    Command command;
    uint32_t count = 0;
    status = parseHeader(loadLittleEndian(bytes), command, count);
    if (status != Status::Ok) return status;
    if (availableBytes / 4 < count) return Status::Truncated;
    status = parseOperands(bytes, command);
    if (status == Status::Ok) status = validate(site, command);
    if (status != Status::Ok) return status;
    // Defense in depth: accepted words must be the encoder's exact image.
    const EncodedCommand image = build(command);
    if (image.count != count) return Status::UnsupportedField;
    for (uint32_t i = 0; i < count; ++i)
        if (image.dwords[i] != loadLittleEndian(bytes + 4 * i)) return Status::UnsupportedField;
    out = command;
    dwords = count;
    return Status::Ok;
}

Status storeLittleEndian(const uint32_t *words, size_t count, uint8_t *bytes, size_t capacityBytes) {
    if (count == 0) return Status::Ok;
    if (!words || !bytes) return Status::InvalidArgument;
    if (capacityBytes / 4 < count) return Status::InsufficientSpace;
    for (size_t i = 0; i < count; ++i)
        for (unsigned k = 0; k < 4; ++k) bytes[4 * i + k] = static_cast<uint8_t>(words[i] >> (8 * k));
    return Status::Ok;
}
uint32_t loadLittleEndian(const uint8_t bytes[4]) {
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

Status checkCommandImage(Generation generation, Engine engine, Site site, const uint8_t *bytes,
                         size_t sizeBytes, size_t &commands) {
    Status status = checkProfile(generation, engine, site);
    if (status != Status::Ok) return status;
    if (!bytes && sizeBytes) return Status::InvalidArgument;
    if (sizeBytes % QwordAlign != 0) return Status::UnalignedStream;
    size_t offset = 0, count = 0;
    bool ended = false;
    while (offset < sizeBytes && !ended) {
        Command command;
        uint32_t dwords = 0;
        status = decodeCommand(generation, engine, site, bytes + offset, sizeBytes - offset, command, dwords);
        if (status != Status::Ok) return status;
        offset += dwords * 4;
        ++count;
        ended = command.kind == CommandKind::BatchBufferEnd;
    }
    if (site == Site::PpgttBatch) {
        if (!ended) return Status::MissingBatchEnd;
        // The image is qword sized, so at most the one completing MI_NOOP may follow.
        const size_t rest = sizeBytes - offset;
        if (rest != 0 && (rest != 4 || loadLittleEndian(bytes + offset) != MiNoop)) return Status::TrailingData;
        if (rest != 0) ++count;
    }
    commands = count;
    return Status::Ok;
}

Status checkStoreProbeRing(const StoreProbeRing &p) {
    const StoreProbe &probe = p.probe;
    if (probe.nonce == probe.cpuInitial || probe.nonce == 0 || probe.nonce == 0xFFFFFFFF)
        return Status::InvalidProbe;
    if (!knownSpace(p.ring.space)) return Status::InvalidAddressSpace;
    if (p.ring.space != AddressSpace::Ggtt) return Status::UnsupportedAddressSpace;
    if (p.ring.bytes == 0) return Status::InvalidProbe;
    if (p.ring.base >= GgttAddressLimit || p.ring.bytes > GgttAddressLimit - p.ring.base)
        return Status::AddressOutOfRange;
    Status status = checkAddress(AddressSpace::Ggtt, probe.target, DwordAlign);
    if (status == Status::Ok) status = checkFlushTarget(p.breadcrumb);
    if (status != Status::Ok) return status;
    if (overlaps(probe.target, 4, p.ring.base, p.ring.bytes) || overlaps(p.breadcrumb, 4, p.ring.base, p.ring.bytes))
        return Status::ProbeAliasesCommands;
    if (overlaps(probe.target, 4, p.breadcrumb, 4)) return Status::ProbeAliasesBreadcrumb;
    return Status::Ok;
}

ProbeReading classifyProbeReading(const StoreProbe &probe, const uint8_t observed[4]) {
    if (!observed || probe.nonce == probe.cpuInitial || probe.nonce == 0 || probe.nonce == 0xFFFFFFFF)
        return ProbeReading::Invalid;
    const uint32_t value = loadLittleEndian(observed);
    if (value == probe.nonce) return ProbeReading::NonceObserved;
    return value == probe.cpuInitial ? ProbeReading::Unchanged : ProbeReading::Foreign;
}

Status CommandWriter::begin(Generation generation, Engine engine, Site site, uint8_t *bytes,
                            size_t capacityBytes) {
    ready_ = false;
    const Status status = checkProfile(generation, engine, site);
    if (status != Status::Ok) return status;
    if (!bytes && capacityBytes) return Status::InvalidArgument;
    generation_ = generation;
    engine_ = engine;
    site_ = site;
    bytes_ = bytes;
    capacity_ = capacityBytes;
    used_ = 0;
    ended_ = false;
    ready_ = true;
    return Status::Ok;
}

Status CommandWriter::put(const EncodedCommand *commands, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) total += commands[i].count * 4;
    if (capacity_ - used_ < total) return Status::InsufficientSpace;
    for (size_t i = 0; i < count; ++i) {
        storeLittleEndian(commands[i].dwords, commands[i].count, bytes_ + used_, capacity_ - used_);
        used_ += commands[i].count * 4;
    }
    return Status::Ok;
}

Status CommandWriter::append(const Command &command) {
    if (!ready_) return Status::InvalidArgument;
    if (ended_) return Status::TrailingData;
    EncodedCommand encoded;
    Status status = encodeCommand(generation_, engine_, site_, command, encoded);
    if (status == Status::Ok) status = put(&encoded, 1);
    if (status == Status::Ok && command.kind == CommandKind::BatchBufferEnd) ended_ = true;
    return status;
}

Status CommandWriter::padToQword() {
    if (!ready_) return Status::InvalidArgument;
    if (used_ % QwordAlign == 0) return Status::Ok;
    const EncodedCommand noop = {{MiNoop}, 1};
    return put(&noop, 1);
}

Status CommandWriter::finishBatch() {
    if (!ready_) return Status::InvalidArgument;
    if (site_ != Site::PpgttBatch) return Status::UnsupportedAtSite;
    if (ended_) return Status::TrailingData;
    const EncodedCommand image[2] = {{{MiBatchBufferEnd}, 1}, {{MiNoop}, 1}};
    // The end makes an even dword count odd; only then is the pad needed.
    const Status status = put(image, used_ % QwordAlign == 0 ? 2 : 1);
    if (status == Status::Ok) ended_ = true;
    return status;
}

Status CommandWriter::appendStoreProbeRing(const StoreProbeRing &p) {
    if (!ready_) return Status::InvalidArgument;
    if (site_ != Site::Ring) return Status::UnsupportedAtSite;
    if (used_ % QwordAlign != 0) return Status::UnalignedStream;
    Status status = checkStoreProbeRing(p);
    if (status != Status::Ok) return status;
    Command sequence[7] = {make(CommandKind::FlushDw), make(CommandKind::StoreDwordImm),
                           make(CommandKind::FlushDw), make(CommandKind::UserInterrupt),
                           make(CommandKind::ArbOnOff), make(CommandKind::ArbCheck), make(CommandKind::Noop)};
    sequence[0].postSync = FlushPostSync::ScratchIndex;
    sequence[0].invalidateTlb = true;
    sequence[1].space = AddressSpace::Ggtt;
    sequence[1].address = p.probe.target;
    sequence[1].value = p.probe.nonce;
    sequence[2].postSync = FlushPostSync::GgttStoreDword;
    sequence[2].space = AddressSpace::Ggtt;
    sequence[2].address = p.breadcrumb;
    sequence[2].value = p.breadcrumbValue;
    sequence[4].enable = true;
    EncodedCommand image[7];
    for (size_t i = 0; i < 7 && status == Status::Ok; ++i)
        status = encodeCommand(generation_, engine_, site_, sequence[i], image[i]);
    return status == Status::Ok ? put(image, 7) : status;
}

} // namespace MellowNativeGlk

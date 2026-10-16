// SPDX-License-Identifier: MIT
/*
 * Gemini Lake IGD OpRegion and VBT read-only snapshot: layout, selection,
 * validity and coherence over injected firmware windows.
 *
 * Layout and rules are re-expressed (no verbatim code) from torvalds/linux
 * 0d9ff90a5422cc7509258aaaba1e7481df4d332a:
 *   drivers/gpu/drm/i915/display/intel_opregion.c  Copyright 2008 Intel Corporation <hong.liu@intel.com>
 *                                                  Copyright 2008 Red Hat <mjg@redhat.com>
 *   drivers/gpu/drm/i915/display/intel_bios.c      Copyright © 2006 Intel Corporation
 *   drivers/gpu/drm/i915/display/intel_vbt_defs.h  Copyright © 2006-2016 Intel Corporation
 *   include/drm/intel/pci_config.h                 Copyright © 2022 Intel Corporation
 *   include/drm/intel/i915_drm.h                   Copyright 2003 Tungsten Graphics, Inc.
 *   drivers/gpu/drm/i915/gt/intel_ggtt.c           Copyright © 2020 Intel Corporation
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
 * See OpRegionSnapshot.provenance.json for blob ids, SHA-256 and symbols.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// Portable rules over injected read-only windows. This file never maps
// memory or touches configuration space; Mellow/GlkOpRegionIOKit supplies
// the native windows. Every decision is taken on owned copies, never on live
// firmware memory, and a second pass re-reads every byte that decided the
// selection or is published.
namespace Mellow { namespace NativeGlk {

namespace OpRegion {
// intel_opregion.c: OPREGION_SIZE is mapped at ASLS; mailbox offsets.
constexpr uint32_t kBytes = 8 * 1024;
constexpr uint32_t kHeaderBytes = 0x100;            // BUILD_BUG_ON(opregion_header)
constexpr uint32_t kAsleOffset = 0x300, kVbtOffset = 0x400, kAsleExtOffset = 0x1c00;
constexpr uint32_t kMboxAcpi = 1u << 0, kMboxSwsci = 1u << 1, kMboxAsle = 1u << 2,
    kMboxAsleExt = 1u << 4, kMboxBacklight = 1u << 5;
// Packed struct opregion_header byte offsets.
constexpr uint32_t kHeaderSize = 0x10, kHeaderRevision = 0x15, kHeaderMinor = 0x16,
    kHeaderMajor = 0x17, kHeaderMailboxes = 0x58, kHeaderDriverModel = 0x5c, kHeaderPcon = 0x60;
constexpr uint32_t kSignatureBytes = 16;            // OPREGION_SIGNATURE "IntelGraphicsMem"
// Packed struct opregion_asle: u64 rvda at 0xba, u32 rvds at 0xc2.
constexpr uint32_t kRvdaOffset = kAsleOffset + 0xba, kRvdsOffset = kAsleOffset + 0xc2;
constexpr uint32_t kRvdBytes = 12;                  // rvda and rvds, one span
// The header SIZE field has no unit at the pin; the IGD OpRegion convention
// is KiB. Requiring at least 8 keeps the 8 KiB mapping inside the declared
// region whether the field is read as KiB or as bytes.
constexpr uint32_t kMinDeclaredSize = kBytes / 1024;
// pci_config.h ASLS and i915_drm.h SNB_GMCH_CTRL / INTEL_BSM, IGD function.
constexpr uint8_t kPciAsls = 0xfc, kPciGgc = 0x50, kPciBdsm = 0x5c;
constexpr uint32_t kBdsmBaseMask = ~((1u << 20) - 1); // INTEL_BSM_MASK
} // namespace OpRegion

namespace Vbt {
// intel_vbt_defs.h packed struct vbt_header and struct bdb_header.
constexpr uint32_t kHeaderBytes = 48, kSizeOffset = 0x18, kChecksumOffset = 0x1a,
    kBdbOffsetOffset = 0x1c;
constexpr uint32_t kBdbHeaderBytes = 22, kBdbSizeOffset = 0x14;
constexpr uint32_t kSignatureBytes = 4;             // intel_bios.c vbt_signature "$VBT"
constexpr uint32_t kBdbSignatureBytes = 15;         // "BIOS_DATA_BLOCK" (vbt_defs comment)
// vbt_size is a u16, so no valid VBT exceeds 0xffff bytes; an RVDS beyond
// 64 KiB can never be required and is refused instead of mapped.
constexpr uint32_t kMaxRvdsBytes = 0x10000;
} // namespace Vbt

enum class VbtStatus : uint8_t {
    NotTried, Ok,
    // intel_bios_is_valid_vbt refusals: selection falls through as in Linux.
    Truncated, Signature, SizeOverflow, BdbHeaderIncomplete, BdbIncomplete,
    // Stricter refusals of a VBT Linux would accept. These never redirect the
    // selection to another VBT; they refuse the capture.
    HeaderIncomplete, BdbSignature, Checksum,
};
constexpr bool linuxRejects(VbtStatus s) {
    return s == VbtStatus::Truncated || s == VbtStatus::Signature || s == VbtStatus::SizeOverflow ||
        s == VbtStatus::BdbHeaderIncomplete || s == VbtStatus::BdbIncomplete;
}

enum class OpRegionStatus : uint8_t {
    Ok,
    Invalid,      // contract misuse: missing callbacks, window geometry
    NotPresent,   // ASLS is zero: firmware declares no OpRegion
    Unavailable,  // configuration all-ones, copy, mapping or allocation failed
    Signature,    // header is not "IntelGraphicsMem"
    HeaderSize,   // declared size smaller than the 8 KiB that is mapped
    Version,      // not OpRegion 2.0 or 2.1
    RvdaRange,    // 2.1 RVDA inside the OpRegion, RVDS bounds or address overflow
    NoVbt,        // neither RVDA nor mailbox #4 holds a Linux-valid VBT
    VbtRefused,   // the VBT Linux would select fails a stricter check
    Incoherent,   // a repeated observation differs from the first
    Uncertain,    // an RVDA unmap failed: mapping state unknown
};

enum class VbtSource : uint8_t { None, RvdaAbsolute, RvdaRelative, Mailbox4 };

struct OpRegionHeader {
    uint32_t declaredSize {}, mailboxes {}, driverModel {}, pcon {};
    uint8_t major {}, minor {}, revision {};
};

struct OpRegionPlan {
    OpRegionHeader header {};
    uint64_t rvda {}, rvdaPhysical {};
    uint32_t rvds {};
    bool rvdaCandidate {}, rvdaRelative {};
    uint32_t mailboxLimit {};   // end of the mailbox #4 window within the OpRegion
};

// One read-only window; copy() is only called inside [0, length).
struct FirmwareWindow {
    void *context {};
    bool (*copy)(void *, uint32_t offset, uint8_t *out, uint32_t bytes) {};
    uint32_t length {};
};

struct OpRegionPlatform {
    void *context {};
    // Maps exactly [physical, physical + bytes) read-only. Called at most once
    // per capture and only with the RVDA decoded from this capture's header.
    bool (*mapRvda)(void *, uint64_t physical, uint32_t bytes, FirmwareWindow &) {};
    // Called exactly once after a successful mapRvda. False: state unknown.
    bool (*unmapRvda)(void *) {};
    // Owned kernel buffer for the published bytes, exactly `bytes` long.
    uint8_t *(*allocate)(void *, uint32_t bytes) {};
    void (*release)(void *, uint8_t *, uint32_t bytes) {};
    // Re-reads ASLS from configuration space for the coherence pass.
    bool (*observeAsls)(void *, uint32_t &) {};
};

struct OpRegionSnapshot {
    OpRegionPlan plan {};
    uint32_t asls {};
    VbtSource source {VbtSource::None};
    VbtStatus rvdaVbt {VbtStatus::NotTried}, mailboxVbt {VbtStatus::NotTried};
    uint8_t *vbt {};            // owned through OpRegionPlatform::allocate
    uint32_t vbtBytes {};
    uint64_t digest {};         // FNV-1a 64 of the VBT bytes, change detection only
};

// ASLS admission before anything is mapped (intel_opregion_setup: 0 = none).
OpRegionStatus checkAsls(uint32_t asls);
OpRegionStatus decodeHeader(const uint8_t (&header)[OpRegion::kHeaderBytes], OpRegionHeader &);
// Version, size, mailbox and RVDA rules; resolves RVDA absolute (2.0) or
// ASLS-relative (2.1) without mapping anything.
OpRegionStatus planOpRegion(uint32_t asls, const uint8_t (&header)[OpRegion::kHeaderBytes],
                            const uint8_t (&rvd)[OpRegion::kRvdBytes], OpRegionPlan &);
// intel_bios_is_valid_vbt plus the stricter checks, on an owned copy.
VbtStatus validateVbt(const uint8_t *bytes, uint32_t windowBytes, uint32_t &vbtBytes);
uint64_t vbtDigest(const uint8_t *bytes, uint32_t length);
// GTT stolen memory (GSM) bytes selected by GGC GGMS; 0 when none.
uint32_t gsmBytesFromGgc(uint16_t ggc);

// Pass one copies, validates and selects exactly as intel_opregion_setup
// does; pass two re-reads the header, RVDA/RVDS, every published VBT byte and
// ASLS. Only Ok leaves out.vbt owned by the caller.
OpRegionStatus captureOpRegion(uint32_t asls, const FirmwareWindow &opRegion,
                               const OpRegionPlatform &, OpRegionSnapshot &out);
void releaseSnapshot(const OpRegionPlatform &, OpRegionSnapshot &);

} } // namespace Mellow::NativeGlk

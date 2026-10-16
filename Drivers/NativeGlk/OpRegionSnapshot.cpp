// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. Re-expressed from Linux i915 (MIT);
// notices, blob ids and symbols: OpRegionSnapshot.hpp and its provenance record.
#include "OpRegionSnapshot.hpp"

namespace Mellow { namespace NativeGlk {
namespace {
constexpr uint8_t kOpRegionSignature[OpRegion::kSignatureBytes] =
    {'I', 'n', 't', 'e', 'l', 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 'M', 'e', 'm'};
constexpr uint8_t kVbtSignature[Vbt::kSignatureBytes] = {'$', 'V', 'B', 'T'};
constexpr uint8_t kBdbSignature[Vbt::kBdbSignatureBytes] =
    {'B', 'I', 'O', 'S', '_', 'D', 'A', 'T', 'A', '_', 'B', 'L', 'O', 'C', 'K'};
// Coherence pass granularity: bounded stack use, no second VBT allocation.
constexpr uint32_t kCompareChunk = 64;

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

bool same(const uint8_t *a, const uint8_t *b, uint32_t bytes) {
    uint8_t difference = 0;
    for (uint32_t i = 0; i < bytes; ++i) difference |= uint8_t(a[i] ^ b[i]);
    return !difference;
}

// The window contract is enforced here, before any callback runs.
bool windowCopy(const FirmwareWindow &w, uint32_t offset, uint8_t *out, uint32_t bytes) {
    if (!w.copy || !out || offset > w.length || bytes > w.length - offset) return false;
    return w.copy(w.context, offset, out, bytes);
}

// Mailbox #4 is a sub-window of the OpRegion mapping, never a new mapping.
struct SubWindow { const FirmwareWindow *parent; uint32_t base; };
bool subCopy(void *context, uint32_t offset, uint8_t *out, uint32_t bytes) {
    auto *sub = static_cast<SubWindow *>(context);
    return windowCopy(*sub->parent, sub->base + offset, out, bytes);
}

OpRegionStatus compareWindow(const FirmwareWindow &w, const uint8_t *expected, uint32_t bytes) {
    uint8_t chunk[kCompareChunk];
    for (uint32_t at = 0; at < bytes; at += kCompareChunk) {
        const uint32_t n = bytes - at < kCompareChunk ? bytes - at : kCompareChunk;
        if (!windowCopy(w, at, chunk, n)) return OpRegionStatus::Unavailable;
        if (!same(chunk, expected + at, n)) return OpRegionStatus::Incoherent;
    }
    return OpRegionStatus::Ok;
}

// Copies one VBT out of the window and judges the copy. A non-Ok verdict is
// returned with OpRegionStatus::Ok; a non-Ok status means no verdict exists.
// On return nothing is owned unless verdict is Ok.
OpRegionStatus loadVbt(const FirmwareWindow &w, const OpRegionPlatform &p, OpRegionSnapshot &s,
                       VbtStatus &verdict) {
    verdict = VbtStatus::Truncated;
    if (w.length < Vbt::kHeaderBytes) return OpRegionStatus::Ok;
    uint8_t head[Vbt::kHeaderBytes];
    if (!windowCopy(w, 0, head, sizeof head)) return OpRegionStatus::Unavailable;
    uint32_t checked = 0;
    if (!same(head, kVbtSignature, Vbt::kSignatureBytes)) {
        verdict = VbtStatus::Signature;
        return OpRegionStatus::Ok;
    }
    const uint32_t size = le16(head + Vbt::kSizeOffset);
    if (size > w.length) {
        verdict = VbtStatus::SizeOverflow;
        return OpRegionStatus::Ok;
    }
    if (size < Vbt::kHeaderBytes) {
        // Everything Linux would read lies inside head; at best HeaderIncomplete.
        verdict = validateVbt(head, Vbt::kHeaderBytes, checked);
        return OpRegionStatus::Ok;
    }
    uint8_t *copy = p.allocate(p.context, size);
    if (!copy) return OpRegionStatus::Unavailable;
    if (!windowCopy(w, 0, copy, size)) {
        p.release(p.context, copy, size);
        return OpRegionStatus::Unavailable;
    }
    // A header that moved between the two copies is a concurrent change.
    if (!same(copy, head, Vbt::kHeaderBytes)) {
        p.release(p.context, copy, size);
        return OpRegionStatus::Incoherent;
    }
    verdict = validateVbt(copy, size, checked);
    if (verdict != VbtStatus::Ok) {
        p.release(p.context, copy, size);
        return OpRegionStatus::Ok;
    }
    s.vbt = copy;
    s.vbtBytes = size;
    return OpRegionStatus::Ok;
}

void dropVbt(const OpRegionPlatform &p, OpRegionSnapshot &s) {
    if (s.vbt) p.release(p.context, s.vbt, s.vbtBytes);
    s.vbt = nullptr;
    s.vbtBytes = 0;
    s.source = VbtSource::None;
}
} // namespace

OpRegionStatus checkAsls(uint32_t asls) {
    if (!asls) return OpRegionStatus::NotPresent;
    // An all-ones read is a failed configuration cycle, not an address.
    if (asls == UINT32_MAX) return OpRegionStatus::Unavailable;
    // ASLS is a 32-bit physical address; the 8 KiB window may not wrap.
    if (asls > UINT32_MAX - (OpRegion::kBytes - 1)) return OpRegionStatus::Invalid;
    return OpRegionStatus::Ok;
}

OpRegionStatus decodeHeader(const uint8_t (&h)[OpRegion::kHeaderBytes], OpRegionHeader &out) {
    out = {};
    if (!same(h, kOpRegionSignature, OpRegion::kSignatureBytes)) return OpRegionStatus::Signature;
    out.declaredSize = le32(h + OpRegion::kHeaderSize);
    out.revision = h[OpRegion::kHeaderRevision];
    out.minor = h[OpRegion::kHeaderMinor];
    out.major = h[OpRegion::kHeaderMajor];
    out.mailboxes = le32(h + OpRegion::kHeaderMailboxes);
    out.driverModel = le32(h + OpRegion::kHeaderDriverModel);
    out.pcon = le32(h + OpRegion::kHeaderPcon);
    if (out.declaredSize < OpRegion::kMinDeclaredSize) return OpRegionStatus::HeaderSize;
    // Only the two layouts whose RVDA meaning is known are admitted: 2.0
    // (physical) and 2.1 (ASLS-relative). Anything else is refused, not guessed.
    if (out.major != 2 || out.minor > 1) return OpRegionStatus::Version;
    return OpRegionStatus::Ok;
}

OpRegionStatus planOpRegion(uint32_t asls, const uint8_t (&header)[OpRegion::kHeaderBytes],
                            const uint8_t (&rvd)[OpRegion::kRvdBytes], OpRegionPlan &plan) {
    plan = {};
    OpRegionStatus status = checkAsls(asls);
    if (status != OpRegionStatus::Ok) return status;
    status = decodeHeader(header, plan.header);
    if (status != OpRegionStatus::Ok) return status;
    const uint32_t mailboxes = plan.header.mailboxes;
    // The VBT may run into the ASLE extension area only when that mailbox is absent.
    plan.mailboxLimit = (mailboxes & OpRegion::kMboxAsleExt) ? OpRegion::kAsleExtOffset : OpRegion::kBytes;
    // Without the ASLE mailbox RVDA/RVDS do not exist.
    if (!(mailboxes & OpRegion::kMboxAsle)) return OpRegionStatus::Ok;
    plan.rvda = le64(rvd);
    plan.rvds = le32(rvd + 8);
    if (!plan.rvda || !plan.rvds) return OpRegionStatus::Ok;
    plan.rvdaRelative = plan.header.major > 2 || plan.header.minor >= 1;
    if (plan.rvds > Vbt::kMaxRvdsBytes) return OpRegionStatus::RvdaRange;
    uint64_t physical = plan.rvda;
    if (plan.rvdaRelative) {
        // 2.1+: unsigned offset from ASLS that never points into the OpRegion.
        // Linux only warns and maps it anyway; here it refuses.
        if (plan.rvda < OpRegion::kBytes || plan.rvda > UINT64_MAX - asls) return OpRegionStatus::RvdaRange;
        physical += asls;
    }
    if (physical > UINT64_MAX - plan.rvds) return OpRegionStatus::RvdaRange;
    plan.rvdaPhysical = physical;
    plan.rvdaCandidate = true;
    return OpRegionStatus::Ok;
}

VbtStatus validateVbt(const uint8_t *vbt, uint32_t window, uint32_t &vbtBytes) {
    vbtBytes = 0;
    // intel_bios_is_valid_vbt, in its order.
    if (!vbt || window < Vbt::kHeaderBytes) return VbtStatus::Truncated;
    if (!same(vbt, kVbtSignature, Vbt::kSignatureBytes)) return VbtStatus::Signature;
    const uint32_t size = le16(vbt + Vbt::kSizeOffset);
    if (size > window) return VbtStatus::SizeOverflow;
    const uint32_t bdb = le32(vbt + Vbt::kBdbOffsetOffset);
    // range_overflows_t(bdb_offset, sizeof(struct bdb_header), vbt_size)
    if (bdb >= size || Vbt::kBdbHeaderBytes > size - bdb) return VbtStatus::BdbHeaderIncomplete;
    // range_overflows_t(bdb_offset, bdb_size, vbt_size)
    if (le16(vbt + bdb + Vbt::kBdbSizeOffset) > size - bdb) return VbtStatus::BdbIncomplete;
    // Stricter: the published bytes must hold the whole VBT header, the BDB
    // must carry its signature, and every byte must sum to zero modulo 256.
    if (size < Vbt::kHeaderBytes) return VbtStatus::HeaderIncomplete;
    if (!same(vbt + bdb, kBdbSignature, Vbt::kBdbSignatureBytes)) return VbtStatus::BdbSignature;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < size; ++i) sum = uint8_t(sum + vbt[i]);
    if (sum) return VbtStatus::Checksum;
    vbtBytes = size;
    return VbtStatus::Ok;
}

uint64_t vbtDigest(const uint8_t *bytes, uint32_t length) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint32_t gsmBytesFromGgc(uint16_t ggc) {
    // gen8_get_total_gtt_size: BDW_GMCH_GGMS_SHIFT 6, BDW_GMCH_GGMS_MASK 0x3.
    const uint32_t ggms = (ggc >> 6) & 0x3;
    return ggms ? (uint32_t(1) << ggms) << 20 : 0;
}

OpRegionStatus captureOpRegion(uint32_t asls, const FirmwareWindow &region,
                               const OpRegionPlatform &p, OpRegionSnapshot &s) {
    s = {};
    if (!region.copy || region.length != OpRegion::kBytes || !p.mapRvda || !p.unmapRvda ||
        !p.allocate || !p.release || !p.observeAsls) return OpRegionStatus::Invalid;
    OpRegionStatus status = checkAsls(asls);
    if (status != OpRegionStatus::Ok) return status;
    s.asls = asls;

    // Pass one: decide on copies.
    uint8_t header[OpRegion::kHeaderBytes], rvd[OpRegion::kRvdBytes];
    if (!windowCopy(region, 0, header, sizeof header) ||
        !windowCopy(region, OpRegion::kRvdaOffset, rvd, sizeof rvd)) return OpRegionStatus::Unavailable;
    status = planOpRegion(asls, header, rvd, s.plan);
    if (status != OpRegionStatus::Ok) return status;

    FirmwareWindow rvdaWindow {};
    const FirmwareWindow *selected = nullptr;
    bool mapped = false;
    if (s.plan.rvdaCandidate && s.plan.rvds < Vbt::kHeaderBytes) {
        // Linux maps and rejects it as truncated; the verdict needs no mapping.
        s.rvdaVbt = VbtStatus::Truncated;
    } else if (s.plan.rvdaCandidate) {
        if (!p.mapRvda(p.context, s.plan.rvdaPhysical, s.plan.rvds, rvdaWindow))
            return OpRegionStatus::Unavailable;
        mapped = true;
        if (rvdaWindow.length != s.plan.rvds || !rvdaWindow.copy) status = OpRegionStatus::Invalid;
        else status = loadVbt(rvdaWindow, p, s, s.rvdaVbt);
        if (status == OpRegionStatus::Ok && s.rvdaVbt == VbtStatus::Ok) {
            selected = &rvdaWindow;
            s.source = s.plan.rvdaRelative ? VbtSource::RvdaRelative : VbtSource::RvdaAbsolute;
        } else {
            if (status == OpRegionStatus::Ok && !linuxRejects(s.rvdaVbt)) status = OpRegionStatus::VbtRefused;
            // intel_opregion_setup unmaps an invalid RVDA before falling back.
            mapped = false;
            if (!p.unmapRvda(p.context)) status = OpRegionStatus::Uncertain;
            if (status != OpRegionStatus::Ok) return status;
        }
    }

    SubWindow mailbox {&region, OpRegion::kVbtOffset};
    const FirmwareWindow mailboxWindow {&mailbox, subCopy, s.plan.mailboxLimit - OpRegion::kVbtOffset};
    if (!selected) {
        status = loadVbt(mailboxWindow, p, s, s.mailboxVbt);
        if (status != OpRegionStatus::Ok) return status;
        if (s.mailboxVbt != VbtStatus::Ok)
            return linuxRejects(s.mailboxVbt) ? OpRegionStatus::NoVbt : OpRegionStatus::VbtRefused;
        selected = &mailboxWindow;
        s.source = VbtSource::Mailbox4;
    }

    // Pass two: every byte that decided the selection or is published.
    uint8_t headerAgain[OpRegion::kHeaderBytes], rvdAgain[OpRegion::kRvdBytes];
    uint32_t aslsAgain = 0;
    if (!windowCopy(region, 0, headerAgain, sizeof headerAgain) ||
        !windowCopy(region, OpRegion::kRvdaOffset, rvdAgain, sizeof rvdAgain))
        status = OpRegionStatus::Unavailable;
    else if (!same(headerAgain, header, sizeof header) ||
             ((s.plan.header.mailboxes & OpRegion::kMboxAsle) && !same(rvdAgain, rvd, sizeof rvd)))
        status = OpRegionStatus::Incoherent;
    else
        status = compareWindow(*selected, s.vbt, s.vbtBytes);
    if (status == OpRegionStatus::Ok) {
        if (!p.observeAsls(p.context, aslsAgain)) status = OpRegionStatus::Unavailable;
        else if (aslsAgain != asls) status = OpRegionStatus::Incoherent;
    }
    if (mapped && !p.unmapRvda(p.context)) status = OpRegionStatus::Uncertain;
    if (status != OpRegionStatus::Ok) {
        dropVbt(p, s);
        return status;
    }
    s.digest = vbtDigest(s.vbt, s.vbtBytes);
    return OpRegionStatus::Ok;
}

void releaseSnapshot(const OpRegionPlatform &p, OpRegionSnapshot &s) {
    if (s.vbt && p.release) p.release(p.context, s.vbt, s.vbtBytes);
    s = {};
}

} } // namespace Mellow::NativeGlk

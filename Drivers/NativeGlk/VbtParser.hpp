// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
//
// Wire layouts and decode rules below are translated from Linux
// drivers/gpu/drm/i915/display/intel_vbt_defs.h and intel_bios.c at
// 0d9ff90a5422cc7509258aaaba1e7481df4d332a; PPS unit rules come from
// intel_pps.c (SPDX MIT, Copyright © 2020 Intel Corporation). The two VBT
// files carry this notice:
//
//   Copyright © 2006-2016 Intel Corporation   (intel_vbt_defs.h)
//   Copyright © 2006 Intel Corporation        (intel_bios.c)
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice (including the next
//   paragraph) shall be included in all copies or substantial portions of the
//   Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// See VbtParser.provenance.json for blob/SHA256 pins and the narrow scope.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Mellow { namespace NativeGlk {

// Byte offsets of the packed little-endian VBT structures. Bit positions
// follow the LSB-first allocation of the upstream x86 bitfields. Untrusted
// bytes are only ever read through bounded byte loads at these offsets.
namespace VbtLayout {
// struct vbt_header
constexpr uint32_t kVbtHeaderBytes = 48;
constexpr uint32_t kVbtSignatureBytes = 20;
constexpr uint32_t kVbtVersion = 20;
constexpr uint32_t kVbtHeaderSize = 22;
constexpr uint32_t kVbtSize = 24;
constexpr uint32_t kVbtChecksum = 26;
constexpr uint32_t kVbtBdbOffset = 28;
// struct bdb_header; each block then has a u8 id and u16 size
constexpr uint32_t kBdbHeaderBytes = 22;
constexpr uint32_t kBdbSignatureBytes = 16;
constexpr uint32_t kBdbVersion = 16;
constexpr uint32_t kBdbHeaderSize = 18;
constexpr uint32_t kBdbSize = 20;
constexpr uint32_t kBlockHeaderBytes = 3;

// Block 1, struct bdb_general_features
constexpr uint32_t kGeneralFeaturesBytes = 7;
constexpr uint32_t kGfBits2 = 1;
constexpr uint8_t kGfEnableSscBit = 1;
constexpr uint8_t kGfSscFreqBit = 2;
constexpr uint8_t kGfDisplayClockModeBit = 6;
constexpr uint32_t kGfBits3 = 2;
constexpr uint8_t kGfRotate180Bit = 2;
constexpr uint32_t kGfBits5 = 4;
constexpr uint8_t kGfIntCrtSupportBit = 0;

// Block 2, struct bdb_general_definitions followed by child_device_config[]
constexpr uint32_t kGeneralDefinitionsBytes = 5;
constexpr uint32_t kGdChildDevSize = 4;
constexpr uint32_t kChildHandle = 0;
constexpr uint32_t kChildDeviceType = 2;
constexpr uint32_t kChildByte7 = 7;
constexpr uint8_t kChildHdmiLevelShifterMask = 0x1f;
constexpr uint32_t kChildByte10 = 10;
constexpr uint8_t kChildGangedEdpBit = 3;
constexpr uint32_t kChildDvoPort = 16;
constexpr uint32_t kChildDdcPin = 19;
constexpr uint32_t kChildByte23 = 23;
constexpr uint8_t kChildLaneReversalBit = 1;
constexpr uint8_t kChildLspconBit = 2;
constexpr uint8_t kChildHpdInvertBit = 4;
constexpr uint8_t kChildUseVbtVswingBit = 5;
constexpr uint8_t kChildDpMaxLaneCountShift = 6;
constexpr uint32_t kChildAuxChannel = 25;
constexpr uint32_t kChildByte38 = 38;
constexpr uint8_t kChildDpMaxLinkRateMask = 0x07;
constexpr uint32_t kChildEdpDataRateOverride = 40;
constexpr uint32_t kChildEdpDataRateOverrideMask = 0x0fff;
constexpr uint32_t kChildLegacyBytes = 33;   // LEGACY_CHILD_DEVICE_CONFIG_SIZE
constexpr uint32_t kChildKnownBytes = 44;    // sizeof(struct child_device_config)

// Block 12, struct bdb_driver_features
constexpr uint32_t kDriverFeaturesBytes = 19;
constexpr uint32_t kDfExtendedBits1 = 7;     // u16, lvds_config in bits 11-12
constexpr uint8_t kDfLvdsConfigShift = 11;

// Block 27, struct bdb_edp
constexpr uint32_t kEdpBytes = 850;
constexpr uint32_t kEdpPowerSeqs = 0;
constexpr uint32_t kEdpPowerSeqStride = 10;  // struct edp_power_seq, five u16
constexpr uint32_t kEdpColorDepth = 160;
constexpr uint32_t kEdpFastLinkParams = 164; // struct edp_fast_link_params[16]
constexpr uint32_t kEdpFastLinkStride = 2;
constexpr uint32_t kEdpSdrrsMsaTimingDelay = 196;
constexpr uint32_t kEdpVswingPreemph = 204;  // u64, 173+
constexpr uint32_t kEdpFastLinkTrainingRate = 748;  // u16[16], 224+
constexpr uint32_t kEdpMaxPortLinkRate = 780;       // u16[16], 244+

// Block 40, struct bdb_lfp_options
constexpr uint32_t kLfpOptionsBytes = 34;
constexpr uint32_t kLoPanelType = 0;
constexpr uint32_t kLoPanelType2 = 1;
constexpr uint32_t kLoFlags = 2;
constexpr uint8_t kLoPixelDitherBit = 5;
constexpr uint32_t kLoDpsPanelTypeBits = 16;

// Block 41, struct bdb_lfp_data_ptrs; each table is u16 offset + u8 size
constexpr uint32_t kLfpPtrsBytes = 148;
constexpr uint32_t kLpNumEntries = 0;
constexpr uint32_t kLpEntries = 1;
constexpr uint32_t kLpEntryStride = 9;
constexpr uint32_t kLpFpTiming = 0;
constexpr uint32_t kLpDvoTiming = 3;
constexpr uint32_t kLpPanelPnpId = 6;
constexpr uint32_t kLpPanelName = 145;
constexpr uint32_t kLpTableSize = 2;

// Block 42 tables
constexpr uint32_t kFpXres = 0;
constexpr uint32_t kFpYres = 2;
constexpr uint32_t kFpLvdsRegVal = 8;
constexpr uint32_t kFpMinTableBytes = 32;
constexpr uint32_t kFpUnaccountedBytes = 6;
constexpr uint32_t kFpGeneratedBytes = 38;
constexpr uint32_t kDtdBytes = 18;           // struct bdb_edid_dtd
constexpr uint32_t kPnpIdBytes = 10;         // struct bdb_edid_pnp_id
constexpr uint32_t kPanelNameBytes = 13;     // struct bdb_edid_product_name
constexpr uint32_t kTailSeamlessDrrsMinRefresh = 210;  // bdb_lfp_data_tail, 188+
// struct bdb_edid_dtd packed bytes
constexpr uint32_t kDtdClock = 0;
constexpr uint32_t kDtdHactiveLo = 2;
constexpr uint32_t kDtdHblankLo = 3;
constexpr uint32_t kDtdHHi = 4;              // hblank_hi:4 low, hactive_hi:4 high
constexpr uint32_t kDtdVactiveLo = 5;
constexpr uint32_t kDtdVblankLo = 6;
constexpr uint32_t kDtdVHi = 7;              // vblank_hi:4 low, vactive_hi:4 high
constexpr uint32_t kDtdHsyncOffLo = 8;
constexpr uint32_t kDtdHsyncWidthLo = 9;
constexpr uint32_t kDtdVsyncLo = 10;         // vsync_pulse_width_lo:4 low, vsync_off_lo:4 high
constexpr uint32_t kDtdSyncHi = 11;          // vsw_hi b0-1, vso_hi b2-3, hsw_hi b4-5, hso_hi b6-7
constexpr uint32_t kDtdHimageLo = 12;
constexpr uint32_t kDtdVimageLo = 13;
constexpr uint32_t kDtdImageHi = 14;         // vimage_hi:4 low, himage_hi:4 high
constexpr uint32_t kDtdFlags = 17;
constexpr uint8_t kDtdVsyncPositiveBit = 5;
constexpr uint8_t kDtdHsyncPositiveBit = 6;

// Block 43, struct bdb_lfp_backlight
constexpr uint32_t kBacklightBytes = 305;
constexpr uint32_t kBlEntrySize = 0;
constexpr uint32_t kBlData = 1;
constexpr uint32_t kBlEntryBytes = 6;        // struct lfp_backlight_data_entry
constexpr uint8_t kBlTypeMask = 0x03;
constexpr uint8_t kBlActiveLowPwmBit = 2;
constexpr uint32_t kBlEntryPwmFreqHz = 1;
constexpr uint32_t kBlEntryMinBrightness = 3;
constexpr uint32_t kBlLevel = 97;
constexpr uint32_t kBlControl = 113;         // type:4 low, controller:4 high, 191+
constexpr uint32_t kBlBrightnessLevel = 129; // struct lfp_brightness_level[16], 234+
constexpr uint32_t kBlBrightnessMinLevel = 193;
constexpr uint32_t kBlLevelStride = 4;
constexpr uint32_t kBlPrecisionBits = 257;   // 236+

// Block 58, struct bdb_generic_dtd
constexpr uint32_t kGdtdHeaderBytes = 2;
constexpr uint32_t kGdtdEntryBytes = 28;     // struct generic_dtd_entry
constexpr uint32_t kGdtdPixelClock = 0;
constexpr uint32_t kGdtdHactive = 4;
constexpr uint32_t kGdtdHblank = 6;
constexpr uint32_t kGdtdHfrontPorch = 8;
constexpr uint32_t kGdtdHsync = 10;
constexpr uint32_t kGdtdVactive = 12;
constexpr uint32_t kGdtdVblank = 14;
constexpr uint32_t kGdtdVfrontPorch = 16;
constexpr uint32_t kGdtdVsync = 18;
constexpr uint32_t kGdtdWidthMm = 20;
constexpr uint32_t kGdtdHeightMm = 22;
constexpr uint32_t kGdtdFlags = 24;
constexpr uint8_t kGdtdVsyncPositiveBit = 6;
constexpr uint8_t kGdtdHsyncPositiveBit = 7;
} // namespace VbtLayout

namespace VbtBlockId {
constexpr uint8_t kGeneralFeatures = 1;
constexpr uint8_t kGeneralDefinitions = 2;
constexpr uint8_t kDriverFeatures = 12;
constexpr uint8_t kEdp = 27;
constexpr uint8_t kLfpOptions = 40;
constexpr uint8_t kLfpDataPtrs = 41;
constexpr uint8_t kLfpData = 42;
constexpr uint8_t kLfpBacklight = 43;
constexpr uint8_t kMipiSequence = 53;
constexpr uint8_t kGenericDtd = 58;
}

// Device-type bits and handles used for eDP selection (intel_vbt_defs.h).
constexpr uint16_t kDeviceTypeInternalConnector = 1u << 12;
constexpr uint16_t kDeviceTypeNotHdmiOutput = 1u << 11;
constexpr uint16_t kDeviceTypeMipiOutput = 1u << 10;
constexpr uint16_t kDeviceTypeTmdsDviSignaling = 1u << 4;
constexpr uint16_t kDeviceTypeDisplayPortOutput = 1u << 2;
constexpr uint16_t kDeviceTypeAnalogOutput = 1u << 0;
constexpr uint16_t kDeviceHandleLfp1 = 0x0008;
constexpr uint16_t kDeviceHandleLfp2 = 0x0080;

// BDB versions whose child-device layout the pinned source documents.
// Bounds of this parser/profile's reviewed format policy, not a GLK firmware
// version range or proof that an actual panel supports every decoded version.
constexpr uint16_t kMinGlkBdbVersion = 155;
constexpr uint16_t kMaxKnownBdbVersion = 264; // last version in pinned source table

constexpr uint32_t kVbtMaxPanels = 16;
constexpr uint32_t kVbtMaxBlocks = 128;
constexpr uint32_t kVbtMaxChildren = 32;

enum class VbtError : uint8_t {
    None,
    InvalidArgument,
    VbtHeaderIncomplete,
    VbtSignatureInvalid,
    VbtHeaderSizeInvalid,
    VbtSizeExceedsBuffer,
    BdbOverlapsVbtHeader,
    BdbHeaderIncomplete,
    BdbSignatureInvalid,
    BdbHeaderSizeInvalid,
    BdbSizeExceedsVbt,
    BlockHeaderTruncated,
    BlockOverflow,
    BlockTableFull,
    DuplicateDecodedBlock,
};

enum class BlockDisposition : uint8_t {
    Unknown,        // id not defined by the pinned source; retained only
    KnownRetained,  // defined upstream but not decoded by this parser
    Decoded,
    Rejected,       // decoded block failed validation; contributes nothing
    Duplicate,      // later copy of a non-decoded id; upstream uses the first
};

struct VbtBlockRecord {
    uint8_t id;
    BlockDisposition disposition;
    bool mipiSequenceV3Size;  // size came from the v3+ u32 field
    uint32_t dataOffset;      // from BDB start, just past the 3-byte header
    uint32_t size;
};

enum class GlkPort : uint8_t { None, A, B, C };
enum class AuxChannel : uint8_t { None, A, B, C, D, E, F, G, H, I };

enum class ChildIssue : uint8_t {
    None, BlockAbsent, BlockTooSmall, ChildSizeTooSmall, TooManyChildren
};

struct VbtChildDevice {
    uint16_t slot;               // index within block 2
    uint16_t handle;
    uint16_t rawDeviceType;
    uint16_t deviceType;         // after i915 sanitize_device_type
    uint8_t dvoPort;
    uint8_t ddcPin;
    uint8_t auxChannelRaw;
    uint8_t i915Port;            // enum port (0 = A), 0xff = PORT_NONE
    GlkPort port;                // None unless the port exists on GLK (A-C)
    AuxChannel aux;              // None if 0 or not in direct_aux_ch_map
    bool sharesPort;             // another child maps to the same DDI port
    bool laneReversal, hpdInvert, gangedEdp, lspcon, useVbtVswing;
    uint8_t hdmiLevelShifter;    // 158+
    uint32_t dpMaxLinkRate;      // i915 units (162000 = RBR); 0 = none/unknown
    uint8_t dpMaxLaneCount;      // 244+; 0 = unknown
    bool edpDataRateOverrideKnown;
    uint16_t edpDataRateOverride;
    bool supportsDp, supportsEdp, supportsDvi, supportsHdmi, supportsDsi, supportsCrt;
};

struct VbtGeneralFeatures {
    bool present;
    bool enableSsc, sscFreq, displayClockMode, intCrtSupport;
    bool rotate180Known;         // 181+
    bool rotate180;
};

struct VbtDriverFeatures {
    bool present;
    bool lvdsConfigKnown;
    uint8_t lvdsConfig;          // 3 = BDB_DRIVER_FEATURE_INT_SDVO_LVDS, i915 eDP
};

struct VbtLfpOptions {
    bool present;
    uint32_t blockSize;
    bool panelTypeKnown;
    uint8_t panelType;
    bool panelType2Known;
    uint8_t panelType2;
    bool pixelDitherKnown;
    bool pixelDither;
    bool dpsPanelTypeBitsKnown;
    uint32_t dpsPanelTypeBits;
};

// Detailed timing after i915 fill_detail_timing_data/parse_generic_dtd.
struct VbtDtd {
    uint32_t clockKhz;
    uint32_t hdisplay, hsyncStart, hsyncEnd, htotal;
    uint32_t vdisplay, vsyncStart, vsyncEnd, vtotal;
    uint32_t widthMm, heightMm;
    bool hsyncPositive, vsyncPositive;
    bool hsyncEndClamped, vsyncEndClamped;  // i915 bogus h/vsync_end fixup
};

enum class LfpIssue : uint8_t {
    None, DataBlockAbsent, PointersAbsent, PointersTooShort, DataBlockEmpty,
    EntryCountInvalid, TableSizeInvalid, PanelNameSizeInvalid, EntryStrideInvalid,
    LayoutInvalid, OffsetBelowDataBlock, TableOutsideBlock, TerminatorMissing
};

struct VbtLfpEntry {
    VbtDtd dtd;
    uint8_t pnpId[VbtLayout::kPnpIdBytes];
    uint16_t fpXres, fpYres;
    uint32_t fpLvdsRegVal;
    bool nameKnown;
    char name[VbtLayout::kPanelNameBytes + 1];   // raw bytes, NUL appended
    bool seamlessDrrsMinRefreshKnown;
    uint8_t seamlessDrrsMinRefresh;
};

struct VbtLfpTables {
    bool present;
    bool pointersGenerated;      // modern VBT without block 41
    LfpIssue issue;
    uint8_t fpTimingSize;        // effective, including the 6 unaccounted bytes
    uint32_t entryStride;
    VbtLfpEntry entries[kVbtMaxPanels];
};

enum class GenericDtdIssue : uint8_t {
    None, BlockAbsent, IgnoredBelowVersion229, BlockTooSmall, EntrySizeTooSmall,
    EntrySizeUnexpected
};

struct VbtGenericDtds {
    bool present;
    GenericDtdIssue issue;
    uint16_t entrySize;
    uint32_t count;              // entries in the block
    uint8_t decodedCount;        // min(count, 16); panel types are < 16
    VbtDtd entries[kVbtMaxPanels];
};

// eDP T1+T3, T8, T9, T10 and T11+T12 in PPS units of 100 microseconds.
struct VbtEdpPowerSeq { uint16_t t1t3, t8, t9, t10, t11t12; };

struct VbtEdpPanel {
    bool powerSeqKnown;
    VbtEdpPowerSeq powerSeq;
    bool colorDepthKnown;
    uint8_t colorDepthCode;      // EDP_18BPP/24BPP/30BPP
    bool fastLinkKnown;
    uint8_t rateCode, lanesCode, preemphasisCode, vswingCode;
    bool msaTimingDelayKnown;
    uint8_t msaTimingDelay;
    bool vswingPreemphKnown;     // 173+
    uint8_t vswingPreemph;
    bool fastLinkTrainingRateKnown;   // 224+
    uint16_t fastLinkTrainingRate;
    bool maxPortLinkRateKnown;        // 244+
    uint16_t maxPortLinkRate;
};

struct VbtEdp {
    bool present;
    uint32_t blockSize;
    VbtEdpPanel panels[kVbtMaxPanels];
};

enum class BacklightIssue : uint8_t { None, BlockAbsent, BlockTooSmall, EntrySizeUnsupported };

struct VbtBacklightPanel {
    bool entryKnown;
    uint8_t type;                // 2 = BDB_BACKLIGHT_TYPE_PWM
    bool activeLowPwm;
    uint16_t pwmFreqHz;
    uint8_t minBrightness;
    bool levelKnown;
    uint8_t level;
    bool controlKnown;           // 191+
    uint8_t controlType;
    uint8_t controller;
    bool brightnessKnown;        // 234+
    uint16_t brightnessLevel, brightnessMinLevel;
    bool precisionBitsKnown;     // 236+
    uint8_t precisionBits;
};

struct VbtBacklight {
    bool present;
    BacklightIssue issue;
    VbtBacklightPanel panels[kVbtMaxPanels];
};

// Self-contained decode of a caller-owned VBT copy; holds no pointer into it.
// It is several KiB: allocate it in heap/static storage, not on a kernel stack.
struct VbtDocument {
    VbtError error;
    uint8_t vbtSignature[VbtLayout::kVbtSignatureBytes];
    uint16_t vbtVersion, vbtHeaderSize, vbtSize;
    uint8_t vbtChecksum;
    uint8_t byteSum;             // 8-bit sum of vbtSize bytes; recorded only
    uint32_t bdbOffset;
    uint16_t bdbVersion, bdbHeaderSize, bdbSize;
    uint8_t bdbTrailingBytes;    // < 4 bytes the upstream walk never reads
    uint16_t blockCount;
    VbtBlockRecord blocks[kVbtMaxBlocks];
    ChildIssue childIssue;
    uint8_t childDevSize;
    int16_t childDevSizeExpected;     // -1: version newer than the source table
    bool childDevSizeMismatch;
    uint16_t childSlots;
    uint8_t childCount;
    VbtChildDevice children[kVbtMaxChildren];
    VbtGeneralFeatures general;
    VbtDriverFeatures driver;
    VbtLfpOptions lfpOptions;
    VbtLfpTables lfp;
    VbtGenericDtds genericDtd;
    VbtEdp edp;
    VbtBacklight backlight;
};

// Validates and decodes a VBT image that the caller already copied out of
// firmware. Any structural error clears the document and leaves only error.
// The buffer is not retained and no firmware or device memory is touched.
VbtError parseVbt(const uint8_t *bytes, size_t length, VbtDocument &out);

// Inputs i915 consults besides the VBT. Each must come from a real read;
// absent inputs stay unavailable and are never replaced with defaults.
struct GlkPanelRequest {
    int16_t opregionPanelType = -1;      // OpRegion panel index 0-15, -1 unavailable
    bool edidProductIdValid = false;
    uint8_t edidProductId[VbtLayout::kPnpIdBytes] = {};  // EDID base block bytes 8..17
    uint16_t childHandle = 0;            // 0: the VBT must define exactly one eDP child
};

enum class PanelTypeSource : uint8_t { None, OpRegion, Vbt, PnpId };
enum class TimingSource : uint8_t { None, GenericDtd, LfpData };
// A legacy fallback is NOT an observed controller byte. The native panel
// sequencer must still validate actual two-PPS state/ownership before writes.
enum class PpsControllerSource : uint8_t { Unknown, VbtControlByte, LegacyBefore191 };

namespace GlkPanelGap {
constexpr uint32_t kDocumentInvalid = 1u << 0;
constexpr uint32_t kInvalidRequest = 1u << 1;
constexpr uint32_t kBdbVersionTooOld = 1u << 2;
constexpr uint32_t kBdbVersionTooNew = 1u << 3;
constexpr uint32_t kNoEdpChild = 1u << 4;
constexpr uint32_t kAmbiguousEdpChild = 1u << 5;
constexpr uint32_t kRequestedChildMissing = 1u << 6;
constexpr uint32_t kRequestedChildNotEdp = 1u << 7;
constexpr uint32_t kEdpPortNotOnGlk = 1u << 8;
constexpr uint32_t kEdpPortShared = 1u << 9;
constexpr uint32_t kAuxUnspecified = 1u << 10;
constexpr uint32_t kAuxNotOnGlk = 1u << 11;
constexpr uint32_t kNoLfpOptions = 1u << 12;
constexpr uint32_t kPanelTypeInvalid = 1u << 13;
constexpr uint32_t kPanelTypeNeedsEdid = 1u << 14;
constexpr uint32_t kPanelTypeNoPnpMatch = 1u << 15;
constexpr uint32_t kPanelType2Missing = 1u << 16;
constexpr uint32_t kNoTiming = 1u << 17;
constexpr uint32_t kTimingInvalid = 1u << 18;
constexpr uint32_t kNoEdpBlock = 1u << 19;
constexpr uint32_t kPpsMissing = 1u << 20;
constexpr uint32_t kPpsAllZero = 1u << 21;
constexpr uint32_t kPpsInstanceUnknown = 1u << 22;
constexpr uint32_t kPpsInstanceInvalid = 1u << 23;
constexpr uint32_t kChecksumInvalid = 1u << 24;
}

// VBT-derived eDP panel description for GLK. Fields are diagnostic unless
// ready; ready requires a zero VBT byte sum and every required item to come
// from VBT/request data or the explicit pinned pre-191 PWM fallback. This is
// configuration-data readiness within the reviewed format bounds only, not native
// panel discovery, timing/connector compatibility, or permission to write PPS.
struct GlkEdpPanelProfile {
    bool ready = false;
    uint32_t gaps = 0;
    PanelTypeSource panelTypeSource = PanelTypeSource::None;
    int16_t panelType = -1;
    int16_t childIndex = -1;           // index into VbtDocument::children
    uint16_t childHandle = 0;
    GlkPort port = GlkPort::None;
    AuxChannel aux = AuxChannel::None;
    TimingSource timingSource = TimingSource::None;
    VbtDtd mode = {};
    uint32_t refreshMilliHz = 0;
    VbtEdpPowerSeq pps = {};           // 100 us units, as stored in the VBT
    uint32_t t1t3Us = 0, t8Us = 0, t9Us = 0, t10Us = 0, t11t12Us = 0;
    uint32_t t1t3Ms = 0, t8Ms = 0, t9Ms = 0, t10Ms = 0, t11t12Ms = 0;  // rounded up
    int8_t ppsInstance = -1;           // GLK PPS 0/1, interpreted with source below
    PpsControllerSource ppsControllerSource = PpsControllerSource::Unknown;
    // Optional VBT link data; zero/-1 means the VBT did not provide it.
    uint8_t bpp = 0;
    uint32_t linkRate = 0;             // i915 units: 162000 = 1.62 Gbit/s
    uint8_t lanes = 0;
    int8_t preemphasisLevel = -1, vswingLevel = -1;
    bool lowVswingKnown = false, lowVswing = false;
    uint32_t maxLinkRate = 0;
    bool backlightPwm = false, activeLowPwm = false;
    uint16_t pwmFreqHz = 0;
    bool minBrightnessKnown = false;
    uint8_t minBrightness = 0;
    bool orientationKnown = false, rotate180 = false;
    bool pnpIdKnown = false;
    uint8_t pnpId[VbtLayout::kPnpIdBytes] = {};
};

// Panel selection follows i915 get_panel_type without its index-0 fallback;
// an unresolved panel stays unresolved. Pure function of its inputs.
void buildGlkEdpPanelProfile(const VbtDocument &doc, const GlkPanelRequest &request,
                             GlkEdpPanelProfile &out);

// Exposed decode rules (pure, bounded).
int childDeviceExpectedSize(uint16_t bdbVersion);   // -1 when unknown (> 264)
bool decodeLfpDtd(const uint8_t *bytes, size_t length, VbtDtd &out);
bool decodeGenericDtd(const uint8_t *bytes, size_t length, VbtDtd &out);
bool glkModeValid(const VbtDtd &mode);
uint32_t modeRefreshMilliHz(const VbtDtd &mode);    // 0 for an invalid mode
inline uint32_t ppsUnitsToMicroseconds(uint16_t units) { return uint32_t(units) * 100u; }
inline uint32_t ppsUnitsToMillisecondsCeil(uint16_t units) { return (uint32_t(units) + 9u) / 10u; }
const char *vbtErrorName(VbtError error);

} } // namespace Mellow::NativeGlk

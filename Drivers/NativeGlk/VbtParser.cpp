// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
// Original bounded translation of Linux i915 0d9ff90a5422cc7509258aaaba1e7481df4d332a.
// The Intel copyright and MIT permission notice are retained in VbtParser.hpp.
#include "VbtParser.hpp"

namespace Mellow { namespace NativeGlk {
namespace {
template<class T> void clear(T &v) {
    auto *p = reinterpret_cast<uint8_t *>(&v);
    for (size_t i = 0; i < sizeof(T); ++i) p[i] = 0;
}
void copy(uint8_t *d, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
bool equal(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}
struct Slice {
    const uint8_t *p; uint32_t n;
    bool has(uint32_t off, uint32_t bytes) const { return off <= n && bytes <= n - off; }
    uint8_t u8(uint32_t off) const { return p[off]; }
    uint16_t u16(uint32_t off) const { return uint16_t(p[off]) | uint16_t(uint16_t(p[off+1]) << 8); }
    uint32_t u32(uint32_t off) const { return uint32_t(u16(off)) | (uint32_t(u16(off+2)) << 16); }
    uint64_t u64(uint32_t off) const { return uint64_t(u32(off)) | (uint64_t(u32(off+4)) << 32); }
};
VbtError fail(VbtDocument &out, VbtError error) { clear(out); out.error = error; return error; }
bool decodedId(uint8_t id) {
    return id == 1 || id == 2 || id == 12 || id == 27 || id == 40 || id == 41 ||
           id == 42 || id == 43 || id == 58;
}
bool knownId(uint8_t id) {
    return (id >= 1 && id <= 32) || (id >= 40 && id <= 46) ||
           (id >= 50 && id <= 58) || id >= 252;
}
VbtBlockRecord *find(VbtDocument &d, uint8_t id) {
    for (uint16_t i = 0; i < d.blockCount; ++i) if (d.blocks[i].id == id) return &d.blocks[i];
    return nullptr;
}
uint8_t ddiPort(uint8_t raw) {
    switch (raw) {
        case 0: case 10: return 0;
        case 1: case 7: return 1;
        case 2: case 8: return 2;
        case 3: case 9: return 3;
        case 6: case 11: case 12: return 4;
        case 13: case 14: return 5;
        case 15: case 16: return 6;
        case 17: case 18: return 7;
        case 19: case 20: return 8;
        default: return 0xff;
    }
}
AuxChannel aux(uint8_t raw) {
    switch (raw) {
        case 0x40: return AuxChannel::A; case 0x10: return AuxChannel::B;
        case 0x20: return AuxChannel::C; case 0x30: return AuxChannel::D;
        case 0x50: return AuxChannel::E; case 0x60: return AuxChannel::F;
        case 0x70: return AuxChannel::G; case 0x80: return AuxChannel::H;
        case 0x90: return AuxChannel::I; default: return AuxChannel::None;
    }
}
void children(Slice b, VbtDocument &d, VbtBlockRecord &r) {
    d.childIssue = ChildIssue::None;
    if (!b.has(0, 5)) { d.childIssue = ChildIssue::BlockTooSmall; r.disposition = BlockDisposition::Rejected; return; }
    d.childDevSize = b.u8(4);
    d.childDevSizeExpected = int16_t(childDeviceExpectedSize(d.bdbVersion));
    d.childDevSizeMismatch = d.childDevSize != (d.childDevSizeExpected < 0 ? 44 : d.childDevSizeExpected);
    if (d.childDevSize < 33) { d.childIssue = ChildIssue::ChildSizeTooSmall; r.disposition = BlockDisposition::Rejected; return; }
    d.childSlots = uint16_t((b.n - 5) / d.childDevSize);
    unsigned count = 0;
    for (uint16_t i = 0; i < d.childSlots; ++i) if (b.u16(5 + uint32_t(i) * d.childDevSize + 2) != 0) ++count;
    if (count > kVbtMaxChildren) { d.childIssue = ChildIssue::TooManyChildren; r.disposition = BlockDisposition::Rejected; return; }
    for (uint16_t i = 0; i < d.childSlots; ++i) {
        Slice c{b.p + 5 + uint32_t(i) * d.childDevSize, d.childDevSize};
        if (c.u16(2) == 0) continue;
        auto &o = d.children[d.childCount++];
        o.slot = i; o.handle = c.u16(0); o.rawDeviceType = c.u16(2); o.deviceType = o.rawDeviceType;
        o.dvoPort = c.u8(16); o.ddcPin = c.u8(19); o.auxChannelRaw = c.u8(25);
        o.i915Port = ddiPort(o.dvoPort);
        o.port = o.i915Port < 3 ? GlkPort(uint8_t(GlkPort::A) + o.i915Port) : GlkPort::None;
        o.aux = aux(o.auxChannelRaw);
        if (o.i915Port == 0 && (o.deviceType & kDeviceTypeTmdsDviSignaling)) {
            o.deviceType &= uint16_t(~kDeviceTypeTmdsDviSignaling); o.deviceType |= kDeviceTypeNotHdmiOutput;
        }
        o.supportsDp = (o.deviceType & kDeviceTypeDisplayPortOutput) != 0;
        o.supportsEdp = o.supportsDp && (o.deviceType & kDeviceTypeInternalConnector) != 0;
        o.supportsDvi = (o.deviceType & kDeviceTypeTmdsDviSignaling) != 0;
        o.supportsHdmi = o.supportsDvi && (o.deviceType & kDeviceTypeNotHdmiOutput) == 0;
        o.supportsDsi = (o.deviceType & kDeviceTypeMipiOutput) != 0;
        o.supportsCrt = (o.deviceType & kDeviceTypeAnalogOutput) != 0;
        o.gangedEdp = d.bdbVersion >= 202 && (c.u8(10) & 8) != 0;
        o.laneReversal = d.bdbVersion >= 184 && (c.u8(23) & 2) != 0;
        o.lspcon = d.bdbVersion >= 192 && (c.u8(23) & 4) != 0;
        o.hpdInvert = d.bdbVersion >= 196 && (c.u8(23) & 16) != 0;
        o.useVbtVswing = d.bdbVersion >= 218 && (c.u8(23) & 32) != 0;
        if (d.bdbVersion >= 158) o.hdmiLevelShifter = c.u8(7) & 31;
        if (d.bdbVersion >= 216 && c.has(38, 1)) {
            const uint8_t code = c.u8(38) & 7;
            if (d.bdbVersion < 230) {
                constexpr uint32_t rates[] = {810000,540000,270000,162000};
                o.dpMaxLinkRate = code < 4 ? rates[code] : 810000;
            } else {
                constexpr uint32_t rates[] = {0,162000,270000,540000,810000,1000000,1350000,2000000};
                o.dpMaxLinkRate = rates[code];
            }
        }
        if (d.bdbVersion >= 244) o.dpMaxLaneCount = uint8_t((c.u8(23) >> 6) + 1);
        if (d.bdbVersion >= 263 && c.has(40, 4)) {
            o.edpDataRateOverrideKnown = true; o.edpDataRateOverride = uint16_t(c.u32(40) & 0xfff);
        }
    }
    for (uint8_t i = 0; i < d.childCount; ++i) for (uint8_t j = uint8_t(i+1); j < d.childCount; ++j)
        if (d.children[i].i915Port != 0xff && d.children[i].i915Port == d.children[j].i915Port)
            d.children[i].sharesPort = d.children[j].sharesPort = true;
}
void edp(Slice b, uint16_t version, VbtEdp &d) {
    d.present = true; d.blockSize = b.n;
    for (uint32_t i = 0; i < 16; ++i) {
        auto &p = d.panels[i]; const uint32_t off = i * 10;
        if (b.has(off, 10)) { p.powerSeqKnown = true; p.powerSeq = {b.u16(off),b.u16(off+2),b.u16(off+4),b.u16(off+6),b.u16(off+8)}; }
        if (b.has(160,4)) { p.colorDepthKnown = true; p.colorDepthCode = uint8_t((b.u32(160) >> (i*2)) & 3); }
        if (b.has(164+i*2,2)) { p.fastLinkKnown = true; p.rateCode = b.u8(164+i*2)&15; p.lanesCode = b.u8(164+i*2)>>4; p.preemphasisCode=b.u8(165+i*2)&15; p.vswingCode=b.u8(165+i*2)>>4; }
        if (b.has(196,4)) { p.msaTimingDelayKnown=true; p.msaTimingDelay=uint8_t((b.u32(196)>>(i*2))&3); }
        if (version>=173 && b.has(204,8)) { p.vswingPreemphKnown=true; p.vswingPreemph=uint8_t((b.u64(204)>>(i*4))&15); }
        if (version>=224 && b.has(748+i*2,2)) { p.fastLinkTrainingRateKnown=true; p.fastLinkTrainingRate=b.u16(748+i*2); }
        if (version>=244 && b.has(780+i*2,2)) { p.maxPortLinkRateKnown=true; p.maxPortLinkRate=b.u16(780+i*2); }
    }
}
void backlight(Slice b, uint16_t v, VbtBacklight &d, VbtBlockRecord &r) {
    if (!b.has(0,1)) { d.issue=BacklightIssue::BlockTooSmall; r.disposition=BlockDisposition::Rejected; return; }
    if (b.u8(0)!=6) { d.issue=BacklightIssue::EntrySizeUnsupported; r.disposition=BlockDisposition::Rejected; return; }
    d.present=true; d.issue=BacklightIssue::None;
    for (uint32_t i=0;i<16;++i) {
        auto &p=d.panels[i]; uint32_t e=1+i*6;
        if (b.has(e,6)) { p.entryKnown=true; p.type=b.u8(e)&3; p.activeLowPwm=(b.u8(e)&4)!=0; p.pwmFreqHz=b.u16(e+1); p.minBrightness=b.u8(e+3); }
        if (b.has(97+i,1)) { p.levelKnown=true; p.level=b.u8(97+i); }
        if (v>=191 && b.has(113+i,1)) { p.controlKnown=true; p.controlType=b.u8(113+i)&15; p.controller=b.u8(113+i)>>4; }
        if (v>=234 && b.has(129+i*4,2) && b.has(193+i*4,2)) { p.brightnessKnown=true; p.brightnessLevel=b.u16(129+i*4); p.brightnessMinLevel=b.u16(193+i*4); }
        if (v>=236 && b.has(257+i,1)) { p.precisionBitsKnown=true; p.precisionBits=b.u8(257+i); }
    }
}
LfpIssue lfp(Slice data, const Slice *ptrs, uint32_t dataOffset, uint16_t v, VbtLfpTables &out) {
    if (!data.n) return LfpIssue::DataBlockEmpty;
    uint32_t fp=38, stride=66, name=16*66; bool names=false;
    if (!ptrs) {
        if (v<155 || data.n<16*66) return LfpIssue::PointersAbsent;
        names=data.has(name,16*13); out.pointersGenerated=true;
    } else {
        const Slice p=*ptrs;
        if (p.n<145) return LfpIssue::PointersTooShort;
        if (p.u8(0)!=3) return LfpIssue::EntryCountInvalid;
        fp=p.u8(3);
        if (fp<32 || p.u8(6)!=18 || p.u8(9)!=10) return LfpIssue::TableSizeInvalid;
        const uint8_t nameSize=p.has(145,3)?p.u8(147):0;
        if (nameSize!=0 && nameSize!=13) return LfpIssue::PanelNameSizeInvalid;
        // Check all absolute offsets before subtracting unsigned BDB positions.
        for (uint32_t i=0;i<16;++i) for (uint32_t t=0;t<3;++t)
            if (p.u16(1+i*9+t*3)<dataOffset) return LfpIssue::OffsetBelowDataBlock;
        if (nameSize && p.u16(145)<dataOffset) return LfpIssue::OffsetBelowDataBlock;
        const uint32_t first=p.u16(1)-dataOffset;
        const uint32_t second=p.u16(10)-dataOffset;
        if (second<first) return LfpIssue::EntryStrideInvalid;
        stride=second-first;
        for (uint32_t i=0;i<16;++i) {
            const uint32_t e=1+i*9;
            if (p.u8(e+2)!=fp || p.u8(e+5)!=18 || p.u8(e+8)!=10) return LfpIssue::EntryStrideInvalid;
            for (uint32_t t=0;t<3;++t)
                if (uint32_t(p.u16(e+t*3)) != uint32_t(p.u16(1+t*3))+i*stride) return LfpIssue::EntryStrideInvalid;
        }
        if (fp+6+28==stride) fp+=6;
        if (fp+28!=stride || first!=0 || uint32_t(p.u16(4))-dataOffset!=fp ||
            uint32_t(p.u16(7))-dataOffset!=fp+18) return LfpIssue::LayoutInvalid;
        if (fp>255 || stride==0) return LfpIssue::LayoutInvalid;
        names=nameSize!=0; if (names) name=p.u16(145)-dataOffset;
    }
    if (!data.has(0,16*stride) || (names && !data.has(name,16*13))) return LfpIssue::TableOutsideBlock;
    for (uint32_t i=0;i<16;++i) if (data.u16(i*stride+fp-2)!=0xffff) return LfpIssue::TerminatorMissing;
    // No partial decoded entries can escape validation failure above.
    out.fpTimingSize=uint8_t(fp); out.entryStride=stride;
    for (uint32_t i=0;i<16;++i) {
        auto &e=out.entries[i]; uint32_t off=i*stride;
        e.fpXres=data.u16(off); e.fpYres=data.u16(off+2); e.fpLvdsRegVal=data.u32(off+8);
        decodeLfpDtd(data.p+off+fp,18,e.dtd); copy(e.pnpId,data.p+off+fp+18,10);
        if (names) {
            e.nameKnown=true; for (uint32_t j=0;j<13;++j) e.name[j]=char(data.u8(name+i*13+j)); e.name[13]=0;
            if (v>=188 && data.has(name+210+i,1)) { e.seamlessDrrsMinRefreshKnown=true; e.seamlessDrrsMinRefresh=data.u8(name+210+i); }
        }
    }
    out.present=true; return LfpIssue::None;
}
} // namespace

int childDeviceExpectedSize(uint16_t v) {
    if (v>264) return -1; if (v>=263) return 44; if (v>=256) return 40;
    if (v>=216) return 39; if (v>=196) return 38; if (v>=195) return 37;
    if (v>=111) return 33; if (v>=106) return 27; return 22;
}

bool decodeLfpDtd(const uint8_t *p, size_t n, VbtDtd &o) {
    clear(o); if (!p || n<18) return false; Slice b{p,18};
    o.clockKhz=uint32_t(b.u16(0))*10;
    o.hdisplay=b.u8(2)+(uint32_t(b.u8(4)>>4)<<8); o.htotal=o.hdisplay+b.u8(3)+(uint32_t(b.u8(4)&15)<<8);
    o.vdisplay=b.u8(5)+(uint32_t(b.u8(7)>>4)<<8); o.vtotal=o.vdisplay+b.u8(6)+(uint32_t(b.u8(7)&15)<<8);
    o.hsyncStart=o.hdisplay+b.u8(8)+(uint32_t(b.u8(11)>>6)<<8);
    o.hsyncEnd=o.hsyncStart+b.u8(9)+(uint32_t((b.u8(11)>>4)&3)<<8);
    o.vsyncStart=o.vdisplay+(b.u8(10)>>4)+(uint32_t((b.u8(11)>>2)&3)<<4);
    o.vsyncEnd=o.vsyncStart+(b.u8(10)&15)+(uint32_t(b.u8(11)&3)<<4);
    if (o.hsyncEnd>o.htotal) { o.hsyncEnd=o.htotal; o.hsyncEndClamped=true; }
    if (o.vsyncEnd>o.vtotal) { o.vsyncEnd=o.vtotal; o.vsyncEndClamped=true; }
    o.widthMm=b.u8(12)+(uint32_t(b.u8(14)>>4)<<8); o.heightMm=b.u8(13)+(uint32_t(b.u8(14)&15)<<8);
    o.hsyncPositive=(b.u8(17)&64)!=0; o.vsyncPositive=(b.u8(17)&32)!=0; return true;
}
bool decodeGenericDtd(const uint8_t *p,size_t n,VbtDtd &o) {
    clear(o); if (!p || n<28) return false; Slice b{p,28};
    o.clockKhz=b.u32(0); o.hdisplay=b.u16(4); o.htotal=o.hdisplay+b.u16(6);
    o.hsyncStart=o.hdisplay+b.u16(8); o.hsyncEnd=o.hsyncStart+b.u16(10);
    o.vdisplay=b.u16(12); o.vtotal=o.vdisplay+b.u16(14);
    o.vsyncStart=o.vdisplay+b.u16(16); o.vsyncEnd=o.vsyncStart+b.u16(18);
    o.widthMm=b.u16(20); o.heightMm=b.u16(22); o.hsyncPositive=(b.u8(24)&128)!=0; o.vsyncPositive=(b.u8(24)&64)!=0; return true;
}
bool glkModeValid(const VbtDtd &m) {
    return m.clockKhz && m.hdisplay && m.vdisplay && m.htotal>m.hdisplay && m.vtotal>m.vdisplay &&
        m.hsyncStart>=m.hdisplay && m.hsyncEnd>m.hsyncStart && m.hsyncEnd<=m.htotal &&
        m.vsyncStart>=m.vdisplay && m.vsyncEnd>m.vsyncStart && m.vsyncEnd<=m.vtotal;
}
uint32_t modeRefreshMilliHz(const VbtDtd &m) {
    if (!glkModeValid(m)) return 0;
    const uint64_t denominator=uint64_t(m.htotal)*m.vtotal;
    const uint64_t answer=(uint64_t(m.clockKhz)*1000000+denominator/2)/denominator;
    return answer>UINT32_MAX?0:uint32_t(answer);
}

VbtError parseVbt(const uint8_t *bytes,size_t length,VbtDocument &d) {
    clear(d);
    if (!bytes) return fail(d,VbtError::InvalidArgument);
    if (length<48) return fail(d,VbtError::VbtHeaderIncomplete);
    Slice v{bytes,48};
    const uint8_t sig[]={'$','V','B','T'};
    if (!equal(bytes,sig,4)) return fail(d,VbtError::VbtSignatureInvalid);
    const uint16_t hs=v.u16(22), size=v.u16(24);
    if (hs<48 || hs>size) return fail(d,VbtError::VbtHeaderSizeInvalid);
    if (size>length) return fail(d,VbtError::VbtSizeExceedsBuffer);
    const uint32_t bo=v.u32(28);
    if (bo<hs) return fail(d,VbtError::BdbOverlapsVbtHeader);
    if (bo>size || uint32_t(size)-bo<22) return fail(d,VbtError::BdbHeaderIncomplete);
    Slice b{bytes+bo,uint32_t(size)-bo};
    const uint8_t bsig[]={'B','I','O','S','_','D','A','T','A','_','B','L','O','C','K',' '};
    if (!equal(b.p,bsig,16)) return fail(d,VbtError::BdbSignatureInvalid);
    const uint16_t bhs=b.u16(18), bsize=b.u16(20);
    if (bhs<22 || bhs>bsize) return fail(d,VbtError::BdbHeaderSizeInvalid);
    if (bsize>b.n) return fail(d,VbtError::BdbSizeExceedsVbt);
    copy(d.vbtSignature,bytes,20); d.vbtVersion=v.u16(20); d.vbtHeaderSize=hs; d.vbtSize=size; d.vbtChecksum=v.u8(26);
    for (uint32_t i=0;i<size;++i) d.byteSum=uint8_t(d.byteSum+bytes[i]);
    d.bdbOffset=bo; d.bdbVersion=b.u16(16); d.bdbHeaderSize=bhs; d.bdbSize=bsize;
    d.childIssue=ChildIssue::BlockAbsent; d.lfp.issue=LfpIssue::DataBlockAbsent;
    d.genericDtd.issue=GenericDtdIssue::BlockAbsent; d.backlight.issue=BacklightIssue::BlockAbsent;
    uint32_t at=bhs;
    while (uint32_t(bsize)-at>3) {
        if (d.blockCount==kVbtMaxBlocks) return fail(d,VbtError::BlockTableFull);
        const uint8_t id=b.u8(at); uint32_t n=b.u16(at+1); bool mipi=false;
        if (id==53 && b.u8(at+3)>=3) {
            if (uint32_t(bsize)-at<8) return fail(d,VbtError::BlockHeaderTruncated);
            n=b.u32(at+4); mipi=true;
        }
        if (n>uint32_t(bsize)-at-3) return fail(d,VbtError::BlockOverflow);
        const bool duplicate=find(d,id)!=nullptr;
        if (duplicate && decodedId(id)) return fail(d,VbtError::DuplicateDecodedBlock);
        auto &r=d.blocks[d.blockCount++]; r.id=id; r.dataOffset=at+3; r.size=n; r.mipiSequenceV3Size=mipi;
        r.disposition=duplicate?BlockDisposition::Duplicate:(decodedId(id)?BlockDisposition::Decoded:(knownId(id)?BlockDisposition::KnownRetained:BlockDisposition::Unknown));
        at+=3+n;
    }
    d.bdbTrailingBytes=uint8_t(uint32_t(bsize)-at);
    for (uint16_t i=0;i<d.blockCount;++i) {
        auto &r=d.blocks[i]; if (r.disposition!=BlockDisposition::Decoded) continue;
        Slice s{b.p+r.dataOffset,r.size};
        switch (r.id) {
            case 1:
                if (s.n<7) { r.disposition=BlockDisposition::Rejected; break; }
                d.general.present=true; d.general.enableSsc=(s.u8(1)&2)!=0; d.general.sscFreq=(s.u8(1)&4)!=0;
                d.general.displayClockMode=(s.u8(1)&64)!=0; d.general.intCrtSupport=(s.u8(4)&1)!=0;
                if (d.bdbVersion>=181) { d.general.rotate180Known=true; d.general.rotate180=(s.u8(2)&4)!=0; } break;
            case 2: children(s,d,r); break;
            case 12:
                if (s.n<19) { r.disposition=BlockDisposition::Rejected; break; }
                d.driver.present=true; d.driver.lvdsConfigKnown=true; d.driver.lvdsConfig=uint8_t((s.u16(7)>>11)&3); break;
            case 27: edp(s,d.bdbVersion,d.edp); break;
            case 40:
                d.lfpOptions.present=true; d.lfpOptions.blockSize=s.n;
                if (s.has(0,1)) { d.lfpOptions.panelTypeKnown=true; d.lfpOptions.panelType=s.u8(0); }
                if (s.has(1,1)) { d.lfpOptions.panelType2Known=true; d.lfpOptions.panelType2=s.u8(1); }
                if (s.has(2,1)) { d.lfpOptions.pixelDitherKnown=true; d.lfpOptions.pixelDither=(s.u8(2)&32)!=0; }
                if (s.has(16,4)) { d.lfpOptions.dpsPanelTypeBitsKnown=true; d.lfpOptions.dpsPanelTypeBits=s.u32(16); } break;
            case 43: backlight(s,d.bdbVersion,d.backlight,r); break;
            case 58:
                if (d.bdbVersion<229) { d.genericDtd.issue=GenericDtdIssue::IgnoredBelowVersion229; r.disposition=BlockDisposition::KnownRetained; break; }
                if (s.n<2) { d.genericDtd.issue=GenericDtdIssue::BlockTooSmall; r.disposition=BlockDisposition::Rejected; break; }
                d.genericDtd.entrySize=s.u16(0);
                if (s.u16(0)!=28) { d.genericDtd.issue=s.u16(0)<28?GenericDtdIssue::EntrySizeTooSmall:GenericDtdIssue::EntrySizeUnexpected; r.disposition=BlockDisposition::Rejected; break; }
                d.genericDtd.present=true; d.genericDtd.issue=GenericDtdIssue::None;
                d.genericDtd.count=(s.n-2)/28; d.genericDtd.decodedCount=uint8_t(d.genericDtd.count>16?16:d.genericDtd.count);
                for (uint32_t j=0;j<d.genericDtd.decodedCount;++j) decodeGenericDtd(s.p+2+j*28,28,d.genericDtd.entries[j]); break;
            default: break;
        }
    }
    auto *data=find(d,42); auto *ptr=find(d,41);
    if (data) {
        Slice ds{b.p+data->dataOffset,data->size}; Slice ps{ptr?b.p+ptr->dataOffset:nullptr,ptr?ptr->size:0};
        d.lfp.issue=lfp(ds,ptr?&ps:nullptr,data->dataOffset,d.bdbVersion,d.lfp);
        if (d.lfp.issue!=LfpIssue::None) { const auto issue=d.lfp.issue; clear(d.lfp); d.lfp.issue=issue; data->disposition=BlockDisposition::Rejected; if (ptr) ptr->disposition=BlockDisposition::Rejected; }
    } else if (ptr) ptr->disposition=BlockDisposition::Rejected;
    return VbtError::None;
}

void buildGlkEdpPanelProfile(const VbtDocument &d,const GlkPanelRequest &r,GlkEdpPanelProfile &o) {
    // Reset directly in caller storage: no compiler-emitted memcpy of a
    // default-initialized aggregate and no large temporary on a kernel stack.
    clear(o); o.panelType=-1; o.childIndex=-1; o.ppsInstance=-1;
    o.preemphasisLevel=-1; o.vswingLevel=-1;
    namespace G=GlkPanelGap;
    if (d.error!=VbtError::None) { o.gaps=G::kDocumentInvalid; return; }
    if (r.opregionPanelType<-1 || r.opregionPanelType>=16) { o.gaps=G::kInvalidRequest; return; }
    if (d.byteSum) o.gaps|=G::kChecksumInvalid;
    if (d.bdbVersion<kMinGlkBdbVersion) o.gaps|=G::kBdbVersionTooOld;
    if (d.bdbVersion>kMaxKnownBdbVersion) o.gaps|=G::kBdbVersionTooNew;
    int selected=-1;
    for (uint8_t i=0;i<d.childCount;++i) {
        const auto &c=d.children[i];
        if (r.childHandle) { if (c.handle==r.childHandle) { if (!c.supportsEdp) { o.gaps|=G::kRequestedChildNotEdp; return; } if (selected>=0) { o.gaps|=G::kAmbiguousEdpChild; return; } selected=i; } }
        else if (c.supportsEdp) { if (selected>=0) { o.gaps|=G::kAmbiguousEdpChild; return; } selected=i; }
    }
    if (selected<0) { o.gaps|=r.childHandle?G::kRequestedChildMissing:G::kNoEdpChild; return; }
    const auto &c=d.children[selected]; o.childIndex=int16_t(selected); o.childHandle=c.handle; o.port=c.port; o.aux=c.aux;
    if (c.port==GlkPort::None) { o.gaps|=G::kEdpPortNotOnGlk; return; }
    if (c.sharesPort) { o.gaps|=G::kEdpPortShared; return; }
    if (!c.auxChannelRaw) { o.gaps|=G::kAuxUnspecified; return; }
    if (c.aux!=AuxChannel::A && c.aux!=AuxChannel::B && c.aux!=AuxChannel::C) { o.gaps|=G::kAuxNotOnGlk; return; }
    if (r.opregionPanelType>=0) { o.panelType=r.opregionPanelType; o.panelTypeSource=PanelTypeSource::OpRegion; }
    else if (!d.lfpOptions.present) o.gaps|=G::kNoLfpOptions;
    else {
        const bool second=c.handle==kDeviceHandleLfp2;
        const bool known=second?d.lfpOptions.panelType2Known:d.lfpOptions.panelTypeKnown;
        const uint8_t p=second?d.lfpOptions.panelType2:d.lfpOptions.panelType;
        if (!known) o.gaps|=second?G::kPanelType2Missing:G::kPanelTypeInvalid;
        else if (p<16) { o.panelType=p; o.panelTypeSource=PanelTypeSource::Vbt; }
        else if (p!=255) o.gaps|=G::kPanelTypeInvalid;
        else if (!r.edidProductIdValid) o.gaps|=G::kPanelTypeNeedsEdid;
        else {
            int match=-1;
            if (d.lfp.present) {
                for (int i=0;i<16;++i) if (equal(d.lfp.entries[i].pnpId,r.edidProductId,10)) { match=i; break; }
                if (match<0) for (int i=0;i<16;++i) if (d.lfp.entries[i].pnpId[8]==0 && d.lfp.entries[i].pnpId[9]==0 && equal(d.lfp.entries[i].pnpId,r.edidProductId,8)) { match=i; break; }
            }
            if (match<0) o.gaps|=G::kPanelTypeNoPnpMatch;
            else { o.panelType=int16_t(match); o.panelTypeSource=PanelTypeSource::PnpId; }
        }
    }
    if (o.panelType<0) { o.gaps|=G::kNoTiming; return; }
    const unsigned p=unsigned(o.panelType);
    if (d.genericDtd.present && p<d.genericDtd.decodedCount) { o.mode=d.genericDtd.entries[p]; o.timingSource=TimingSource::GenericDtd; }
    else if (d.lfp.present) { o.mode=d.lfp.entries[p].dtd; o.timingSource=TimingSource::LfpData; }
    else o.gaps|=G::kNoTiming;
    if (o.timingSource!=TimingSource::None && !glkModeValid(o.mode)) o.gaps|=G::kTimingInvalid;
    o.refreshMilliHz=modeRefreshMilliHz(o.mode);
    if (d.lfp.present) { o.pnpIdKnown=true; copy(o.pnpId,d.lfp.entries[p].pnpId,10); }
    if (!d.edp.present) o.gaps|=G::kNoEdpBlock;
    else {
        const auto &e=d.edp.panels[p];
        if (!e.powerSeqKnown) o.gaps|=G::kPpsMissing;
        else {
            o.pps=e.powerSeq;
            if (!(o.pps.t1t3|o.pps.t8|o.pps.t9|o.pps.t10|o.pps.t11t12)) o.gaps|=G::kPpsAllZero;
            o.t1t3Us=ppsUnitsToMicroseconds(o.pps.t1t3); o.t8Us=ppsUnitsToMicroseconds(o.pps.t8); o.t9Us=ppsUnitsToMicroseconds(o.pps.t9);
            o.t10Us=ppsUnitsToMicroseconds(o.pps.t10); o.t11t12Us=ppsUnitsToMicroseconds(o.pps.t11t12);
            o.t1t3Ms=ppsUnitsToMillisecondsCeil(o.pps.t1t3); o.t8Ms=ppsUnitsToMillisecondsCeil(o.pps.t8); o.t9Ms=ppsUnitsToMillisecondsCeil(o.pps.t9);
            o.t10Ms=ppsUnitsToMillisecondsCeil(o.pps.t10); o.t11t12Ms=ppsUnitsToMillisecondsCeil(o.pps.t11t12);
        }
        if (e.colorDepthKnown && e.colorDepthCode<3) o.bpp=uint8_t(18+6*e.colorDepthCode);
        if (e.fastLinkKnown) {
            if (d.bdbVersion<224) { constexpr uint32_t rates[]={162000,270000,540000}; if (e.rateCode<3) o.linkRate=rates[e.rateCode]; }
            o.lanes=e.lanesCode==0?1:(e.lanesCode==1?2:(e.lanesCode==3?4:0));
            if (e.preemphasisCode<4) o.preemphasisLevel=int8_t(e.preemphasisCode);
            if (e.vswingCode<4) o.vswingLevel=int8_t(e.vswingCode);
        }
        if (e.fastLinkTrainingRateKnown) o.linkRate=uint32_t(e.fastLinkTrainingRate)*20;
        if (e.vswingPreemphKnown) { o.lowVswingKnown=true; o.lowVswing=e.vswingPreemph==0; }
        if (e.maxPortLinkRateKnown) o.maxLinkRate=uint32_t(e.maxPortLinkRate)*20;
    }
    const auto &bl=d.backlight.panels[p];
    if (d.backlight.present && bl.entryKnown && bl.type==2) {
        o.backlightPwm=true; o.activeLowPwm=bl.activeLowPwm; o.pwmFreqHz=bl.pwmFreqHz;
        if (d.bdbVersion<191) {
            // parse_lfp_backlight assigns controller 0 before the versioned
            // control byte exists. Preserve its distinct origin; this is not
            // hardware readback or display ownership admission.
            o.ppsInstance=0;
            o.ppsControllerSource=PpsControllerSource::LegacyBefore191;
        } else if (!bl.controlKnown) o.gaps|=G::kPpsInstanceUnknown;
        else if (bl.controller>1) o.gaps|=G::kPpsInstanceInvalid;
        else {
            o.ppsInstance=int8_t(bl.controller);
            o.ppsControllerSource=PpsControllerSource::VbtControlByte;
        }
        if (d.bdbVersion<234) { o.minBrightnessKnown=true; o.minBrightness=bl.minBrightness; }
        else if (bl.brightnessKnown && (d.bdbVersion<236 || bl.precisionBitsKnown)) {
            uint32_t min=bl.brightnessMinLevel;
            const bool scale=d.bdbVersion>=236?(bl.precisionBitsKnown && bl.precisionBits==16):bl.brightnessLevel>255;
            if (scale) min/=255;
            if (min<=255) { o.minBrightnessKnown=true; o.minBrightness=uint8_t(min); }
        }
    } else o.gaps|=G::kPpsInstanceUnknown;
    o.orientationKnown=d.general.rotate180Known; o.rotate180=d.general.rotate180;
    o.ready=o.gaps==0;
}

const char *vbtErrorName(VbtError e) {
    switch (e) {
#define N(x) case VbtError::x: return #x
        N(None); N(InvalidArgument); N(VbtHeaderIncomplete); N(VbtSignatureInvalid);
        N(VbtHeaderSizeInvalid); N(VbtSizeExceedsBuffer); N(BdbOverlapsVbtHeader);
        N(BdbHeaderIncomplete); N(BdbSignatureInvalid); N(BdbHeaderSizeInvalid);
        N(BdbSizeExceedsVbt); N(BlockHeaderTruncated); N(BlockOverflow);
        N(BlockTableFull); N(DuplicateDecodedBlock);
#undef N
        default: return "UnknownVbtError";
    }
}
} } // namespace Mellow::NativeGlk

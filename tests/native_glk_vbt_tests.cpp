// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors.
//
// HOSTED TESTS OVER SYNTHETIC STRUCTURAL FIXTURES. No VBT was captured from the
// Lenovo 15IGL7 (or any machine). Every byte below is generated here from the
// pinned i915 layout, so passing results are parser evidence, never hardware
// evidence and never a qualified display/panel profile.
#include "../Drivers/NativeGlk/VbtParser.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Mellow::NativeGlk;
namespace Gap = GlkPanelGap;

static int gChecks = 0;
static int gFailures = 0;

#define CHECK(cond) do { ++gChecks; if (!(cond)) { ++gFailures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++gChecks; const long long va_ = (long long)(a); \
    const long long vb_ = (long long)(b); if (va_ != vb_) { ++gFailures; \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } } while (0)

using Bytes = std::vector<uint8_t>;

static void put8(Bytes &b, size_t off, uint8_t v) { if (b.size() < off + 1) b.resize(off + 1); b[off] = v; }
static void put16(Bytes &b, size_t off, uint16_t v) { put8(b, off, uint8_t(v)); put8(b, off + 1, uint8_t(v >> 8)); }
static void put32(Bytes &b, size_t off, uint32_t v) { put16(b, off, uint16_t(v)); put16(b, off + 2, uint16_t(v >> 16)); }
static void put64(Bytes &b, size_t off, uint64_t v) { put32(b, off, uint32_t(v)); put32(b, off + 4, uint32_t(v >> 32)); }
static uint16_t get16(const Bytes &b, size_t off) { return uint16_t(b[off] | (b[off + 1] << 8)); }

// ---------------------------------------------------------------- encoders --
struct ModeSpec {
    uint16_t clock10k, hactive, hblank, hso, hsw, vactive, vblank, vso, vsw, himage, vimage;
    bool hpos, vpos;
};

// struct bdb_edid_dtd, bit positions written independently of the parser.
static Bytes encodeDtd(const ModeSpec &m) {
    Bytes d(18, 0);
    put16(d, 0, m.clock10k);
    d[2] = uint8_t(m.hactive); d[3] = uint8_t(m.hblank);
    d[4] = uint8_t(((m.hactive >> 8) & 0xf) << 4 | ((m.hblank >> 8) & 0xf));
    d[5] = uint8_t(m.vactive); d[6] = uint8_t(m.vblank);
    d[7] = uint8_t(((m.vactive >> 8) & 0xf) << 4 | ((m.vblank >> 8) & 0xf));
    d[8] = uint8_t(m.hso); d[9] = uint8_t(m.hsw);
    d[10] = uint8_t((m.vso & 0xf) << 4 | (m.vsw & 0xf));
    d[11] = uint8_t(((m.hso >> 8) & 3) << 6 | ((m.hsw >> 8) & 3) << 4 | ((m.vso >> 4) & 3) << 2 | ((m.vsw >> 4) & 3));
    d[12] = uint8_t(m.himage); d[13] = uint8_t(m.vimage);
    d[14] = uint8_t(((m.himage >> 8) & 0xf) << 4 | ((m.vimage >> 8) & 0xf));
    d[17] = uint8_t((m.hpos ? 0x40 : 0) | (m.vpos ? 0x20 : 0) | 0x18 | 0x80);
    return d;
}

struct GenericSpec {
    uint32_t clockKhz; uint16_t hactive, hblank, hfp, hsync, vactive, vblank, vfp, vsync, wmm, hmm;
    bool hpos, vpos;
};

static Bytes encodeGeneric(const GenericSpec &g) {
    Bytes d(28, 0);
    put32(d, 0, g.clockKhz);
    put16(d, 4, g.hactive); put16(d, 6, g.hblank); put16(d, 8, g.hfp); put16(d, 10, g.hsync);
    put16(d, 12, g.vactive); put16(d, 14, g.vblank); put16(d, 16, g.vfp); put16(d, 18, g.vsync);
    put16(d, 20, g.wmm); put16(d, 22, g.hmm);
    d[24] = uint8_t((g.hpos ? 0x80 : 0) | (g.vpos ? 0x40 : 0) | 0x15);
    return d;
}

static const ModeSpec kMode1080 = {13850, 1920, 160, 48, 32, 1080, 31, 3, 5, 344, 194, true, false};
static ModeSpec modeXga(int p) {
    return ModeSpec{uint16_t(6500 + p), 1024, 320, 24, 136, 768, 38, 3, 6, 300, 200, false, true};
}

static void pnpFor(int p, uint8_t out[10]) {
    const uint8_t base[10] = {0x11, 0x22, uint8_t(0x30 + p), 0x00, 0xa0, 0xa1, 0xa2, 0xa3, 0x05, 0x1e};
    std::memcpy(out, base, 10);
    if (p == 7) {
        const uint8_t undated[10] = {0x11, 0x22, 0x77, 0x00, 0xb0, 0xb1, 0xb2, 0xb3, 0x00, 0x00};
        std::memcpy(out, undated, 10);
    }
}

struct ChildSpec {
    uint16_t handle, type; uint8_t dvo, aux;
    uint8_t byte23 = 0, byte38 = 0, byte10 = 0, byte7 = 0;
    uint32_t rateOverride = 0;
};

static Bytes generalDefinitions(uint8_t childSize, const std::vector<ChildSpec> &children, size_t emptySlots) {
    Bytes b(5, 0);
    b[0] = 0x02; b[4] = childSize;
    for (const ChildSpec &c : children) {
        const size_t base = b.size();
        b.resize(base + childSize, 0);
        Bytes child(44, 0);
        put16(child, 0, c.handle); put16(child, 2, c.type);
        child[7] = c.byte7; child[10] = c.byte10; child[16] = c.dvo; child[19] = 0x05;
        child[23] = c.byte23; child[24] = 0x03; child[25] = c.aux; child[38] = c.byte38;
        put32(child, 40, c.rateOverride);
        for (size_t i = 0; i < childSize && i < child.size(); ++i) b[base + i] = child[i];
    }
    b.resize(b.size() + emptySlots * childSize, 0);
    return b;
}

struct LfpLayoutSpec { uint8_t fpDeclared = 32; uint8_t hidden = 6; bool names = true; };

static uint32_t strideOf(const LfpLayoutSpec &l) { return uint32_t(l.fpDeclared) + l.hidden + 18 + 10; }

static Bytes lfpData(const LfpLayoutSpec &l, const std::vector<ModeSpec> &modes, bool tail) {
    const uint32_t stride = strideOf(l), fp = uint32_t(l.fpDeclared) + l.hidden;
    Bytes b(16 * stride, 0);
    for (int p = 0; p < 16; ++p) {
        const uint32_t e = uint32_t(p) * stride;
        put16(b, e + 0, modes[p].hactive); put16(b, e + 2, modes[p].vactive);
        put32(b, e + 8, 0x80300000u + uint32_t(p));
        put16(b, e + fp - 2, 0xffff);
        const Bytes dtd = encodeDtd(modes[p]);
        std::memcpy(&b[e + fp], dtd.data(), 18);
        uint8_t pnp[10]; pnpFor(p, pnp);
        std::memcpy(&b[e + fp + 18], pnp, 10);
    }
    if (tail) {
        const size_t t = b.size();
        b.resize(t + 310, 0);
        for (int p = 0; p < 16; ++p) {
            char name[14];
            std::snprintf(name, sizeof name, "SYNTH-PANEL%02d", p);
            std::memcpy(&b[t + size_t(p) * 13], name, 13);
            b[t + 210 + size_t(p)] = uint8_t(40 + p);
        }
    }
    return b;
}

static Bytes lfpPtrs(uint32_t dataOffset, const LfpLayoutSpec &l) {
    const uint32_t stride = strideOf(l), fp = uint32_t(l.fpDeclared) + l.hidden;
    Bytes b(148, 0);
    b[0] = 3;
    for (uint32_t p = 0; p < 16; ++p) {
        const size_t e = 1 + p * 9;
        put16(b, e + 0, uint16_t(dataOffset + p * stride)); b[e + 2] = l.fpDeclared;
        put16(b, e + 3, uint16_t(dataOffset + p * stride + fp)); b[e + 5] = 18;
        put16(b, e + 6, uint16_t(dataOffset + p * stride + fp + 18)); b[e + 8] = 10;
    }
    if (l.names) { put16(b, 145, uint16_t(dataOffset + 16 * stride)); b[147] = 13; }
    return b;
}

struct EdpPanelSpec { uint16_t seq[5]; uint8_t depth, rate, lanes, preemph, vswing, vswingNibble; uint16_t trainingRate, maxPortRate; };

static Bytes edpBlock(const EdpPanelSpec (&p)[16], size_t size) {
    Bytes b(850, 0);
    uint32_t depth = 0; uint64_t vsw = 0;
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 5; ++k) put16(b, size_t(i) * 10 + size_t(k) * 2, p[i].seq[k]);
        depth |= uint32_t(p[i].depth & 3) << (2 * i);
        b[164 + size_t(i) * 2] = uint8_t((p[i].lanes << 4) | (p[i].rate & 0xf));
        b[165 + size_t(i) * 2] = uint8_t((p[i].vswing << 4) | (p[i].preemph & 0xf));
        vsw |= uint64_t(p[i].vswingNibble & 0xf) << (4 * i);
        put16(b, 748 + size_t(i) * 2, p[i].trainingRate);
        put16(b, 780 + size_t(i) * 2, p[i].maxPortRate);
    }
    put32(b, 160, depth);
    put64(b, 204, vsw);
    b.resize(size);
    return b;
}

struct BacklightSpec { uint8_t type; bool activeLow; uint16_t freq; uint8_t minB, level, control; uint16_t bLevel, bMin; uint8_t precision; };

static Bytes backlightBlock(const BacklightSpec (&p)[16], uint8_t entrySize = 6) {
    Bytes b(305, 0);
    b[0] = entrySize;
    for (int i = 0; i < 16; ++i) {
        const size_t e = 1 + size_t(i) * 6;
        b[e] = uint8_t((p[i].type & 3) | (p[i].activeLow ? 4 : 0));
        put16(b, e + 1, p[i].freq); b[e + 3] = p[i].minB;
        b[97 + size_t(i)] = p[i].level; b[113 + size_t(i)] = p[i].control;
        put16(b, 129 + size_t(i) * 4, p[i].bLevel); put16(b, 193 + size_t(i) * 4, p[i].bMin);
        b[257 + size_t(i)] = p[i].precision;
    }
    return b;
}

// --------------------------------------------------------------- assembler --
struct Block {
    uint8_t id; Bytes data;
    int32_t sizeField = -1;           // override for the u16 size
    bool lateLfpPtrs = false;         // block 41 bytes depend on block 42 position
    LfpLayoutSpec layout;
};

struct VbtOptions {
    uint16_t bdbVersion = 228;
    std::string vbtSignature = "$VBT SYNTHETIC-GLK  ";
    std::string bdbSignature = "BIOS_DATA_BLOCK ";
    size_t trailingBdb = 0;
    bool fixChecksum = true;
};

static void fixChecksum(Bytes &v) {
    const size_t size = get16(v, 24);
    v[26] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < size && i < v.size(); ++i) sum = uint8_t(sum + v[i]);
    v[26] = uint8_t(0x100 - sum);
}

static Bytes assemble(std::vector<Block> blocks, const VbtOptions &o) {
    std::vector<uint32_t> dataOffset;
    uint32_t pos = 22;
    for (const Block &b : blocks) { dataOffset.push_back(pos + 3); pos += 3 + uint32_t(b.data.size()); }
    uint32_t lfpDataOffset = 0;
    for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i].id == 42) { lfpDataOffset = dataOffset[i]; break; }
    for (Block &b : blocks) if (b.lateLfpPtrs) b.data = lfpPtrs(lfpDataOffset, b.layout);
    Bytes bdb(22, 0);
    std::memcpy(bdb.data(), o.bdbSignature.data(), o.bdbSignature.size() < 16 ? o.bdbSignature.size() : 16);
    put16(bdb, 16, o.bdbVersion); put16(bdb, 18, 22);
    for (const Block &b : blocks) {
        const uint16_t size = b.sizeField >= 0 ? uint16_t(b.sizeField) : uint16_t(b.data.size());
        bdb.push_back(b.id); bdb.push_back(uint8_t(size)); bdb.push_back(uint8_t(size >> 8));
        bdb.insert(bdb.end(), b.data.begin(), b.data.end());
    }
    bdb.resize(bdb.size() + o.trailingBdb, 0);
    put16(bdb, 20, uint16_t(bdb.size()));
    Bytes v(48, 0);
    std::memcpy(v.data(), o.vbtSignature.data(), o.vbtSignature.size() < 20 ? o.vbtSignature.size() : 20);
    put16(v, 20, 100); put16(v, 22, 48); put32(v, 28, 48);
    v.insert(v.end(), bdb.begin(), bdb.end());
    put16(v, 24, uint16_t(v.size()));
    if (o.fixChecksum) fixChecksum(v);
    return v;
}

static size_t findBlock(const std::vector<Block> &blocks, uint8_t id) {
    for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i].id == id) return i;
    return blocks.size();
}

// ---------------------------------------------------------------- fixtures --
// Fixture A: synthetic BDB 228 shaped like a GLK laptop VBT, panel type 2.
static std::vector<Block> fixtureA(VbtOptions &o) {
    o = VbtOptions{};
    std::vector<Block> blocks;
    Bytes general(7, 0); general[1] = 0x02; general[4] = 0x01;
    blocks.push_back({1, general});
    blocks.push_back({2, generalDefinitions(39, {
        {kDeviceHandleLfp1, 0x78c6, 10, 0x40, 0, 1},
        {0x0004, 0x60d2, 1, 0x10},
        {0x0040, 0x68c6, 8, 0x20}}, 1)});
    Bytes driver(19, 0); put16(driver, 7, uint16_t(3u << 11));
    blocks.push_back({12, driver});
    EdpPanelSpec edp[16];
    for (int p = 0; p < 16; ++p)
        edp[p] = EdpPanelSpec{{uint16_t(100 + p), uint16_t(10 + p), uint16_t(20 + p), uint16_t(300 + p), uint16_t(400 + p)},
                              2, 0, 1, 1, 0, 1, 8100, 8100};
    edp[2] = EdpPanelSpec{{2100, 500, 500, 5000, 5100}, 1, 2, 3, 0, 1, 0, 13500, 27000};
    blocks.push_back({27, edpBlock(edp, 850)});
    Bytes options(34, 0); options[0] = 2; options[1] = 3; options[2] = 0x20; put32(options, 16, 0x20);
    blocks.push_back({40, options});
    Block ptrs{41, Bytes(148, 0)}; ptrs.lateLfpPtrs = true;
    blocks.push_back(ptrs);
    std::vector<ModeSpec> modes;
    for (int p = 0; p < 16; ++p) modes.push_back(p == 2 ? kMode1080 : modeXga(p));
    blocks.push_back({42, lfpData(LfpLayoutSpec{}, modes, true)});
    BacklightSpec bl[16];
    for (int p = 0; p < 16; ++p) bl[p] = BacklightSpec{2, true, uint16_t(1000 + p), 5, 200, 0x02, 0, 0, 0};
    bl[2] = BacklightSpec{2, false, 200, 10, 255, 0x12, 0, 0, 0};
    blocks.push_back({43, backlightBlock(bl)});
    blocks.push_back({44, Bytes(10, 0)});
    blocks.push_back({0xee, Bytes{1, 2, 3, 4}});
    blocks.push_back({254, Bytes{0, 0}});
    return blocks;
}

static Bytes buildA() { VbtOptions o; auto b = fixtureA(o); return assemble(b, o); }

struct Parsed { std::unique_ptr<VbtDocument> doc{new VbtDocument}; VbtError error{}; };

static Parsed parse(const Bytes &v, size_t length = SIZE_MAX) {
    Parsed r;
    r.error = parseVbt(v.data(), length == SIZE_MAX ? v.size() : length, *r.doc);
    return r;
}

static GlkEdpPanelProfile profileOf(const VbtDocument &doc, const GlkPanelRequest &request = {}) {
    GlkEdpPanelProfile profile;
    buildGlkEdpPanelProfile(doc, request, profile);
    return profile;
}

static const VbtBlockRecord *record(const VbtDocument &d, uint8_t id, int nth = 0) {
    for (uint16_t i = 0; i < d.blockCount && i < kVbtMaxBlocks; ++i)
        if (d.blocks[i].id == id && nth-- == 0) return &d.blocks[i];
    return nullptr;
}

// ------------------------------------------------------------------- tests --
static void testFixtureAStructure() {
    const Bytes v = buildA();
    Parsed r = parse(v);
    const VbtDocument &d = *r.doc;
    CHECK_EQ(r.error, VbtError::None);
    CHECK_EQ(d.error, VbtError::None);
    CHECK(std::memcmp(d.vbtSignature, "$VBT SYNTHETIC-GLK  ", 20) == 0);
    CHECK_EQ(d.vbtVersion, 100);
    CHECK_EQ(d.vbtHeaderSize, 48);
    CHECK_EQ(d.vbtSize, v.size());
    CHECK_EQ(d.byteSum, 0);
    CHECK_EQ(d.bdbOffset, 48);
    CHECK_EQ(d.bdbVersion, 228);
    CHECK_EQ(d.bdbHeaderSize, 22);
    CHECK_EQ(d.bdbSize, v.size() - 48);
    CHECK_EQ(d.blockCount, 11); // actual encoded block list has 11 members
    CHECK_EQ(d.bdbTrailingBytes, 0);
    const uint8_t decoded[] = {1, 2, 12, 27, 40, 41, 42, 43};
    for (uint8_t id : decoded) {
        const VbtBlockRecord *rec = record(d, id);
        CHECK(rec && rec->disposition == BlockDisposition::Decoded);
    }
    CHECK(record(d, 44) && record(d, 44)->disposition == BlockDisposition::KnownRetained);
    CHECK(record(d, 254) && record(d, 254)->disposition == BlockDisposition::KnownRetained);
    CHECK(record(d, 0xee) && record(d, 0xee)->disposition == BlockDisposition::Unknown);
    CHECK(record(d, 0xee) && record(d, 0xee)->size == 4);
    CHECK_EQ(record(d, 1)->dataOffset, 25u);
    // Children.
    CHECK_EQ(d.childIssue, ChildIssue::None);
    CHECK_EQ(d.childDevSize, 39);
    CHECK_EQ(d.childDevSizeExpected, 39);
    CHECK(!d.childDevSizeMismatch);
    CHECK_EQ(d.childSlots, 4);
    CHECK_EQ(d.childCount, 3);
    const VbtChildDevice &c0 = d.children[0];
    CHECK_EQ(c0.handle, kDeviceHandleLfp1);
    CHECK_EQ(c0.deviceType, 0x78c6);
    CHECK_EQ(c0.dvoPort, 10);
    CHECK_EQ(c0.i915Port, 0);
    CHECK_EQ(c0.port, GlkPort::A);
    CHECK_EQ(c0.aux, AuxChannel::A);
    CHECK(c0.supportsDp && c0.supportsEdp && !c0.supportsHdmi && !c0.supportsDvi);
    CHECK_EQ(c0.dpMaxLinkRate, 540000u);  // BDB 216-229 table code 1 = HBR2
    CHECK_EQ(c0.dpMaxLaneCount, 0);       // 244+ only
    CHECK(!c0.sharesPort);
    CHECK_EQ(d.children[1].port, GlkPort::B);
    CHECK_EQ(d.children[1].aux, AuxChannel::B);
    CHECK(d.children[1].supportsHdmi && !d.children[1].supportsEdp);
    CHECK_EQ(d.children[2].port, GlkPort::C);
    CHECK(d.children[2].supportsDp && !d.children[2].supportsEdp);
    CHECK_EQ(d.children[2].slot, 2);
    // Other decoded blocks.
    CHECK(d.general.present && d.general.enableSsc && d.general.intCrtSupport);
    CHECK(d.general.rotate180Known && !d.general.rotate180);
    CHECK(d.driver.present && d.driver.lvdsConfigKnown);
    CHECK_EQ(d.driver.lvdsConfig, 3);
    CHECK(d.lfpOptions.present && d.lfpOptions.panelTypeKnown && d.lfpOptions.panelType2Known);
    CHECK_EQ(d.lfpOptions.panelType, 2);
    CHECK_EQ(d.lfpOptions.panelType2, 3);
    CHECK(d.lfpOptions.pixelDitherKnown && d.lfpOptions.pixelDither);
    CHECK(d.lfpOptions.dpsPanelTypeBitsKnown);
    CHECK_EQ(d.lfpOptions.dpsPanelTypeBits, 0x20u);
    CHECK(d.lfp.present && !d.lfp.pointersGenerated);
    CHECK_EQ(d.lfp.issue, LfpIssue::None);
    CHECK_EQ(d.lfp.fpTimingSize, 38);
    CHECK_EQ(d.lfp.entryStride, 66u);
    const VbtLfpEntry &e2 = d.lfp.entries[2];
    CHECK_EQ(e2.dtd.hdisplay, 1920u);
    CHECK_EQ(e2.fpXres, 1920);
    CHECK_EQ(e2.fpYres, 1080);
    CHECK_EQ(e2.fpLvdsRegVal, 0x80300002u);
    CHECK(e2.nameKnown && std::strcmp(e2.name, "SYNTH-PANEL02") == 0);
    CHECK(e2.seamlessDrrsMinRefreshKnown);
    CHECK_EQ(e2.seamlessDrrsMinRefresh, 42);
    uint8_t pnp9[10]; pnpFor(9, pnp9);
    CHECK(std::memcmp(d.lfp.entries[9].pnpId, pnp9, 10) == 0);
    CHECK_EQ(d.lfp.entries[15].dtd.clockKhz, 65150u);
    CHECK(d.edp.present);
    CHECK_EQ(d.edp.blockSize, 850u);
    CHECK(d.edp.panels[2].powerSeqKnown);
    CHECK_EQ(d.edp.panels[2].powerSeq.t11t12, 5100);
    CHECK_EQ(d.edp.panels[2].rateCode, 2);
    CHECK_EQ(d.edp.panels[2].lanesCode, 3);
    CHECK(d.edp.panels[2].fastLinkTrainingRateKnown);
    CHECK(!d.edp.panels[2].maxPortLinkRateKnown);   // 244+
    CHECK(d.backlight.present);
    CHECK_EQ(d.backlight.issue, BacklightIssue::None);
    CHECK(d.backlight.panels[2].controlKnown);
    CHECK_EQ(d.backlight.panels[2].controller, 1);
    CHECK(!d.backlight.panels[2].brightnessKnown);  // 234+
    CHECK_EQ(d.genericDtd.issue, GenericDtdIssue::BlockAbsent);
}

static void testFixtureAProfile() {
    Parsed r = parse(buildA());
    const GlkEdpPanelProfile p = profileOf(*r.doc);
    CHECK(p.ready);
    CHECK_EQ(p.gaps, 0u);
    CHECK_EQ(p.panelTypeSource, PanelTypeSource::Vbt);
    CHECK_EQ(p.panelType, 2);
    CHECK_EQ(p.childIndex, 0);
    CHECK_EQ(p.childHandle, kDeviceHandleLfp1);
    CHECK_EQ(p.port, GlkPort::A);
    CHECK_EQ(p.aux, AuxChannel::A);
    CHECK_EQ(p.timingSource, TimingSource::LfpData);
    CHECK_EQ(p.mode.clockKhz, 138500u);
    CHECK_EQ(p.mode.hdisplay, 1920u);
    CHECK_EQ(p.mode.hsyncStart, 1968u);
    CHECK_EQ(p.mode.hsyncEnd, 2000u);
    CHECK_EQ(p.mode.htotal, 2080u);
    CHECK_EQ(p.mode.vdisplay, 1080u);
    CHECK_EQ(p.mode.vsyncStart, 1083u);
    CHECK_EQ(p.mode.vsyncEnd, 1088u);
    CHECK_EQ(p.mode.vtotal, 1111u);
    CHECK_EQ(p.mode.widthMm, 344u);
    CHECK_EQ(p.mode.heightMm, 194u);
    CHECK(p.mode.hsyncPositive && !p.mode.vsyncPositive);
    CHECK(!p.mode.hsyncEndClamped && !p.mode.vsyncEndClamped);
    // 138500 kHz / (2080 * 1111) = 59.93388 Hz, rounded to nearest mHz.
    CHECK_EQ(p.refreshMilliHz, 59934u);
    CHECK_EQ(p.pps.t1t3, 2100); CHECK_EQ(p.pps.t8, 500); CHECK_EQ(p.pps.t9, 500);
    CHECK_EQ(p.pps.t10, 5000); CHECK_EQ(p.pps.t11t12, 5100);
    CHECK_EQ(p.t1t3Us, 210000u); CHECK_EQ(p.t8Us, 50000u); CHECK_EQ(p.t9Us, 50000u);
    CHECK_EQ(p.t10Us, 500000u); CHECK_EQ(p.t11t12Us, 510000u);
    CHECK_EQ(p.t1t3Ms, 210u); CHECK_EQ(p.t8Ms, 50u); CHECK_EQ(p.t9Ms, 50u);
    CHECK_EQ(p.t10Ms, 500u); CHECK_EQ(p.t11t12Ms, 510u);
    CHECK_EQ(p.ppsInstance, 1);           // PPS1 from VBT, not a PPS0 default
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::VbtControlByte);
    CHECK_EQ(p.bpp, 24);
    CHECK_EQ(p.linkRate, 270000u);        // 224+: 13500 * 20, not rate code 2 (5.4)
    CHECK_EQ(p.lanes, 4);
    CHECK_EQ(p.preemphasisLevel, 0);
    CHECK_EQ(p.vswingLevel, 1);
    CHECK(p.lowVswingKnown && p.lowVswing);
    CHECK_EQ(p.maxLinkRate, 0u);          // 244+ only
    CHECK(p.backlightPwm && !p.activeLowPwm);
    CHECK_EQ(p.pwmFreqHz, 200);
    CHECK(p.minBrightnessKnown);
    CHECK_EQ(p.minBrightness, 10);
    CHECK(p.orientationKnown && !p.rotate180);
    uint8_t pnp2[10]; pnpFor(2, pnp2);
    CHECK(p.pnpIdKnown && std::memcmp(p.pnpId, pnp2, 10) == 0);
}

// Fixture B: synthetic BDB 240 with a generic DTD block (229+ path).
static std::vector<Block> fixtureB(VbtOptions &o, uint16_t gdtdSize = 28, uint8_t panelType = 1) {
    std::vector<Block> blocks = fixtureA(o);
    o.bdbVersion = 240;
    blocks[findBlock(blocks, 40)].data[0] = panelType;
    Bytes g(2, 0); put16(g, 0, gdtdSize);
    const GenericSpec entries[3] = {
        {50000, 800, 100, 10, 10, 600, 10, 1, 1, 0, 0, false, false},
        {72300, 1366, 160, 48, 32, 768, 22, 3, 5, 344, 194, true, false},
        {60000, 1280, 160, 48, 32, 800, 23, 3, 6, 0, 0, true, true}};
    for (const GenericSpec &e : entries) {
        Bytes enc = encodeGeneric(e);
        enc.resize(gdtdSize > 28 ? gdtdSize : 28, 0);
        g.insert(g.end(), enc.begin(), enc.end());
    }
    blocks.push_back({58, g});
    EdpPanelSpec edp[16];
    for (int p = 0; p < 16; ++p) edp[p] = EdpPanelSpec{{2000, 600, 700, 4000, 5000}, 1, 0, 1, 0, 0, 1, 8100, 13500};
    blocks[findBlock(blocks, 27)].data = edpBlock(edp, 850);
    BacklightSpec bl[16];
    for (int p = 0; p < 16; ++p) bl[p] = BacklightSpec{2, false, 1000, 0, 0, 0x02, 65535, 2550, 16};
    blocks[findBlock(blocks, 43)].data = backlightBlock(bl);
    return blocks;
}

static void testGenericDtdPath() {
    VbtOptions o;
    Parsed r = parse(assemble(fixtureB(o), o));
    CHECK_EQ(r.error, VbtError::None);
    const VbtDocument &d = *r.doc;
    CHECK(d.genericDtd.present);
    CHECK_EQ(d.genericDtd.issue, GenericDtdIssue::None);
    CHECK_EQ(d.genericDtd.entrySize, 28);
    CHECK_EQ(d.genericDtd.count, 3u);
    CHECK_EQ(d.genericDtd.decodedCount, 3);
    const GlkEdpPanelProfile p = profileOf(d);
    CHECK(p.ready);
    CHECK_EQ(p.panelType, 1);
    CHECK_EQ(p.timingSource, TimingSource::GenericDtd);
    CHECK_EQ(p.mode.clockKhz, 72300u);
    CHECK_EQ(p.mode.hdisplay, 1366u);
    CHECK_EQ(p.mode.hsyncStart, 1414u);
    CHECK_EQ(p.mode.hsyncEnd, 1446u);
    CHECK_EQ(p.mode.htotal, 1526u);
    CHECK_EQ(p.mode.vdisplay, 768u);
    CHECK_EQ(p.mode.vsyncStart, 771u);
    CHECK_EQ(p.mode.vsyncEnd, 776u);
    CHECK_EQ(p.mode.vtotal, 790u);
    CHECK(p.mode.hsyncPositive && !p.mode.vsyncPositive);
    CHECK_EQ(p.refreshMilliHz, 59973u);   // 72300 kHz / (1526 * 790) = 59.97312 Hz
    CHECK_EQ(p.ppsInstance, 0);
    CHECK_EQ(p.linkRate, 162000u);
    CHECK_EQ(p.lanes, 2);
    CHECK_EQ(p.maxLinkRate, 0u);          // 240 < 244
    CHECK(p.minBrightnessKnown);
    CHECK_EQ(p.minBrightness, 10);        // 236+: 16-bit precision, 2550 / 255

    // Generic block rejected or not covering the panel: i915 falls back to block 42.
    VbtOptions o2; Parsed small = parse(assemble(fixtureB(o2, 27), o2));
    CHECK_EQ(small.doc->genericDtd.issue, GenericDtdIssue::EntrySizeTooSmall);
    CHECK(record(*small.doc, 58)->disposition == BlockDisposition::Rejected);
    GlkEdpPanelProfile ps = profileOf(*small.doc);
    CHECK(ps.ready);
    CHECK_EQ(ps.timingSource, TimingSource::LfpData);
    CHECK_EQ(ps.mode.hdisplay, 1024u);
    VbtOptions o3; Parsed large = parse(assemble(fixtureB(o3, 32), o3));
    CHECK_EQ(large.doc->genericDtd.issue, GenericDtdIssue::EntrySizeUnexpected);
    CHECK_EQ(profileOf(*large.doc).timingSource, TimingSource::LfpData);
    VbtOptions o4; Parsed beyond = parse(assemble(fixtureB(o4, 28, 5), o4));
    const GlkEdpPanelProfile pb = profileOf(*beyond.doc);
    CHECK_EQ(pb.panelType, 5);
    CHECK_EQ(pb.timingSource, TimingSource::LfpData);
    // Below BDB 229 the block is retained but ignored, as upstream does.
    VbtOptions o5; auto blocks = fixtureB(o5); o5.bdbVersion = 228;
    Parsed old = parse(assemble(blocks, o5));
    CHECK_EQ(old.doc->genericDtd.issue, GenericDtdIssue::IgnoredBelowVersion229);
    CHECK(record(*old.doc, 58)->disposition == BlockDisposition::KnownRetained);
    CHECK_EQ(profileOf(*old.doc).timingSource, TimingSource::LfpData);
    // 244+ exposes the max port link rate.
    VbtOptions o6; auto b244 = fixtureB(o6); o6.bdbVersion = 244;
    CHECK_EQ(profileOf(*parse(assemble(b244, o6)).doc).maxLinkRate, 270000u);
}

// Fixture C: BDB 155 without block 41; pointers are generated like i915 does.
static std::vector<Block> fixtureC(VbtOptions &o) {
    o = VbtOptions{}; o.bdbVersion = 155;
    std::vector<Block> blocks;
    blocks.push_back({2, generalDefinitions(33, {{kDeviceHandleLfp1, 0x78c6, 10, 0x40}}, 0)});
    EdpPanelSpec edp[16];
    for (int p = 0; p < 16; ++p) edp[p] = EdpPanelSpec{{2100, 500, 500, 5000, 5100}, 1, 0, 1, 0, 0, 0, 0, 0};
    blocks.push_back({27, edpBlock(edp, 200)});
    Bytes options(34, 0); options[0] = 0;
    blocks.push_back({40, options});
    std::vector<ModeSpec> modes;
    for (int p = 0; p < 16; ++p) modes.push_back(kMode1080);
    LfpLayoutSpec gen; gen.fpDeclared = 38; gen.hidden = 0;
    Bytes data = lfpData(gen, modes, true);
    data.resize(16 * 66 + 208);           // names only, enough for generation
    blocks.push_back({42, data});
    BacklightSpec bl[16];
    for (int p = 0; p < 16; ++p) bl[p] = BacklightSpec{2, false, 200, 3, 100, 0x10, 0, 0, 0};
    blocks.push_back({43, backlightBlock(bl)});
    return blocks;
}

static void testGeneratedPointersAndNoDefaultController() {
    VbtOptions o;
    Parsed r = parse(assemble(fixtureC(o), o));
    const VbtDocument &d = *r.doc;
    CHECK_EQ(r.error, VbtError::None);
    CHECK(d.lfp.present && d.lfp.pointersGenerated);
    CHECK_EQ(d.lfp.fpTimingSize, 38);
    CHECK_EQ(d.lfp.entryStride, 66u);
    CHECK(d.lfp.entries[0].nameKnown);
    CHECK(!d.lfp.entries[0].seamlessDrrsMinRefreshKnown);
    CHECK_EQ(d.childDevSizeExpected, 33);
    CHECK(!d.general.present);
    CHECK(!d.edp.panels[0].vswingPreemphKnown);
    const GlkEdpPanelProfile p = profileOf(d);
    // Pinned parse_lfp_backlight defaults a selected valid PWM entry to
    // controller 0 before 191. This is a source fallback, not a captured
    // controller byte or permission to program the actual PPS instance.
    CHECK(p.ready);
    CHECK_EQ(p.gaps, 0u);
    CHECK_EQ(p.ppsInstance, 0);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::LegacyBefore191);
    CHECK_EQ(p.panelType, 0);
    CHECK_EQ(p.panelTypeSource, PanelTypeSource::Vbt);
    CHECK_EQ(p.mode.hdisplay, 1920u);
    CHECK_EQ(p.linkRate, 162000u);         // pre-224 rate code EDP_RATE_1_62
    CHECK_EQ(p.lanes, 2);
    CHECK(!p.lowVswingKnown);
    CHECK(!p.orientationKnown);
    CHECK(p.minBrightnessKnown);
    CHECK_EQ(p.minBrightness, 3);

    // Before BDB 155 no pointers are generated, so there is no timing at all.
    VbtOptions o2; auto blocks = fixtureC(o2); o2.bdbVersion = 154;
    Parsed old = parse(assemble(blocks, o2));
    CHECK(!old.doc->lfp.present);
    CHECK_EQ(old.doc->lfp.issue, LfpIssue::PointersAbsent);
    const GlkEdpPanelProfile po = profileOf(*old.doc);
    CHECK((po.gaps & Gap::kBdbVersionTooOld) != 0);
    CHECK((po.gaps & Gap::kNoTiming) != 0);
    // Data block too small for 16 generated entries.
    VbtOptions o3; auto b3 = fixtureC(o3);
    b3[findBlock(b3, 42)].data.resize(16 * 66 - 1);
    CHECK_EQ(parse(assemble(b3, o3)).doc->lfp.issue, LfpIssue::PointersAbsent);
}

static void testLegacyControllerRequiresSelectedPwm() {
    VbtOptions o; auto b = fixtureC(o);
    b[findBlock(b, 40)].data[0] = 1;
    auto &bl = b[findBlock(b, 43)].data;
    bl[1] = 0; // unselected entry 0 cannot defeat selected valid PWM entry 1
    Parsed selected = parse(assemble(b, o));
    auto p = profileOf(*selected.doc);
    CHECK(p.ready);
    CHECK_EQ(p.panelType, 1);
    CHECK_EQ(p.ppsInstance, 0);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::LegacyBefore191);
    bl[1 + 6] = 0; // selected entry 1 has no valid PWM source fallback
    p = profileOf(*parse(assemble(b, o)).doc);
    CHECK(!p.ready);
    CHECK_EQ(p.gaps, Gap::kPpsInstanceUnknown);
    CHECK_EQ(p.ppsInstance, -1);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::Unknown);
    bl[1 + 6] = 2;
    bl.resize(1 + 6); // selected entry 1 bytes were not captured
    p = profileOf(*parse(assemble(b, o)).doc);
    CHECK(!p.ready);
    CHECK_EQ(p.gaps, Gap::kPpsInstanceUnknown);
    CHECK_EQ(p.ppsInstance, -1);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::Unknown);
    b.erase(b.begin() + findBlock(b, 43));
    p = profileOf(*parse(assemble(b, o)).doc);
    CHECK(!p.ready);
    CHECK_EQ(p.gaps, Gap::kPpsInstanceUnknown);
    CHECK_EQ(p.ppsInstance, -1);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::Unknown);
    b = fixtureC(o); o.bdbVersion = 191;
    b[findBlock(b, 43)].data.resize(113); // no modern control[0] byte
    p = profileOf(*parse(assemble(b, o)).doc);
    CHECK(!p.ready);
    CHECK_EQ(p.gaps, Gap::kPpsInstanceUnknown);
    CHECK_EQ(p.ppsInstance, -1);
    CHECK_EQ(p.ppsControllerSource, PpsControllerSource::Unknown);
}

static void testPanelTypeSelection() {
    // 0xff means "match the EDID PnP ID"; without an EDID nothing is chosen.
    VbtOptions o; auto blocks = fixtureA(o);
    blocks[findBlock(blocks, 40)].data[0] = 0xff;
    Parsed r = parse(assemble(blocks, o));
    GlkEdpPanelProfile none = profileOf(*r.doc);
    CHECK(!none.ready);
    CHECK_EQ(none.panelType, -1);
    CHECK_EQ(none.panelTypeSource, PanelTypeSource::None);
    CHECK((none.gaps & Gap::kPanelTypeNeedsEdid) != 0);
    CHECK((none.gaps & Gap::kNoTiming) != 0);
    GlkPanelRequest req;
    req.edidProductIdValid = true; pnpFor(4, req.edidProductId);
    GlkEdpPanelProfile full = profileOf(*r.doc, req);
    CHECK(full.ready);
    CHECK_EQ(full.panelTypeSource, PanelTypeSource::PnpId);
    CHECK_EQ(full.panelType, 4);
    CHECK_EQ(full.mode.clockKhz, 65040u);
    pnpFor(7, req.edidProductId); req.edidProductId[8] = 9; req.edidProductId[9] = 0x20;
    GlkEdpPanelProfile undated = profileOf(*r.doc, req);
    CHECK_EQ(undated.panelType, 7);        // date-less VBT entry, upstream best match
    pnpFor(4, req.edidProductId); req.edidProductId[8] = 6;
    GlkEdpPanelProfile miss = profileOf(*r.doc, req);
    CHECK_EQ(miss.panelType, -1);
    CHECK((miss.gaps & Gap::kPanelTypeNoPnpMatch) != 0);
    // A valid OpRegion panel index takes precedence over the VBT.
    Parsed a = parse(buildA());
    GlkPanelRequest op; op.opregionPanelType = 9;
    GlkEdpPanelProfile opr = profileOf(*a.doc, op);
    CHECK(opr.ready);
    CHECK_EQ(opr.panelTypeSource, PanelTypeSource::OpRegion);
    CHECK_EQ(opr.panelType, 9);
    op.opregionPanelType = 16;
    GlkEdpPanelProfile bad = profileOf(*a.doc, op);
    CHECK(!bad.ready);
    CHECK_EQ(bad.gaps, Gap::kInvalidRequest);
    // Invalid VBT panel type: no fallback to panel 0.
    VbtOptions o2; auto b2 = fixtureA(o2);
    b2[findBlock(b2, 40)].data[0] = 0x20;
    GlkEdpPanelProfile inv = profileOf(*parse(assemble(b2, o2)).doc);
    CHECK(!inv.ready);
    CHECK_EQ(inv.panelType, -1);
    CHECK((inv.gaps & Gap::kPanelTypeInvalid) != 0);
    // LFP2 children use panel_type2, which must exist in the block.
    VbtOptions o3; auto b3 = fixtureA(o3);
    b3[findBlock(b3, 2)].data = generalDefinitions(39, {{kDeviceHandleLfp2, 0x78c6, 10, 0x40}}, 0);
    GlkEdpPanelProfile lfp2 = profileOf(*parse(assemble(b3, o3)).doc);
    CHECK(lfp2.ready);
    CHECK_EQ(lfp2.panelType, 3);
    CHECK_EQ(lfp2.ppsInstance, 0);
    b3[findBlock(b3, 40)].data.resize(1);
    GlkEdpPanelProfile shortOpt = profileOf(*parse(assemble(b3, o3)).doc);
    CHECK_EQ(shortOpt.panelType, -1);
    CHECK((shortOpt.gaps & Gap::kPanelType2Missing) != 0);
    // No LFP options block at all.
    VbtOptions o4; auto b4 = fixtureA(o4);
    b4.erase(b4.begin() + long(findBlock(b4, 40)));
    GlkEdpPanelProfile noOpt = profileOf(*parse(assemble(b4, o4)).doc);
    CHECK_EQ(noOpt.panelType, -1);
    CHECK((noOpt.gaps & Gap::kNoLfpOptions) != 0);
}

static void testChildSelection() {
    VbtOptions o; auto base = fixtureA(o);
    auto withChildren = [&](const std::vector<ChildSpec> &children, uint8_t size = 39) {
        auto b = base; b[findBlock(b, 2)].data = generalDefinitions(size, children, 0);
        return parse(assemble(b, o));
    };
    const ChildSpec edpA{kDeviceHandleLfp1, 0x78c6, 10, 0x40};
    const ChildSpec edpB{kDeviceHandleLfp2, 0x78c6, 7, 0x10};
    const ChildSpec dpC{0x0040, 0x68c6, 8, 0x20};
    Parsed none = withChildren({dpC});
    CHECK_EQ(profileOf(*none.doc).gaps, Gap::kNoEdpChild);
    Parsed two = withChildren({edpA, edpB, dpC});
    GlkEdpPanelProfile amb = profileOf(*two.doc);
    CHECK(!amb.ready);
    CHECK((amb.gaps & Gap::kAmbiguousEdpChild) != 0);
    CHECK_EQ(amb.childIndex, -1);
    GlkPanelRequest req; req.childHandle = kDeviceHandleLfp2;
    GlkEdpPanelProfile picked = profileOf(*two.doc, req);
    CHECK(picked.ready);
    CHECK_EQ(picked.childIndex, 1);
    CHECK_EQ(picked.port, GlkPort::B);
    CHECK_EQ(picked.aux, AuxChannel::B);
    CHECK_EQ(picked.panelType, 3);
    req.childHandle = 0x0040;
    CHECK((profileOf(*two.doc, req).gaps & Gap::kRequestedChildNotEdp) != 0);
    req.childHandle = 0x0100;
    CHECK((profileOf(*two.doc, req).gaps & Gap::kRequestedChildMissing) != 0);
    Parsed portD = withChildren({{kDeviceHandleLfp1, 0x78c6, 9, 0x40}});
    CHECK_EQ(portD.doc->children[0].i915Port, 3);
    CHECK_EQ(portD.doc->children[0].port, GlkPort::None);
    CHECK_EQ(profileOf(*portD.doc).gaps, Gap::kEdpPortNotOnGlk);
    Parsed noAux = withChildren({{kDeviceHandleLfp1, 0x78c6, 10, 0}});
    CHECK_EQ(profileOf(*noAux.doc).gaps, Gap::kAuxUnspecified);
    Parsed auxD = withChildren({{kDeviceHandleLfp1, 0x78c6, 10, 0x30}});
    CHECK_EQ(auxD.doc->children[0].aux, AuxChannel::D);
    CHECK_EQ(profileOf(*auxD.doc).gaps, Gap::kAuxNotOnGlk);
    Parsed auxBad = withChildren({{kDeviceHandleLfp1, 0x78c6, 10, 0x41}});
    CHECK_EQ(auxBad.doc->children[0].aux, AuxChannel::None);
    CHECK_EQ(profileOf(*auxBad.doc).gaps, Gap::kAuxNotOnGlk);
    Parsed shared = withChildren({edpA, {0x0004, 0x60d2, 0, 0x10}});
    CHECK(shared.doc->children[0].sharesPort && shared.doc->children[1].sharesPort);
    CHECK_EQ(profileOf(*shared.doc).gaps, Gap::kEdpPortShared);
    // i915 sanitize_device_type: DVI/HDMI claimed on port A becomes not-HDMI.
    CHECK_EQ(shared.doc->children[1].rawDeviceType, 0x60d2);
    CHECK_EQ(shared.doc->children[1].deviceType, 0x68c2);
    CHECK(!shared.doc->children[1].supportsHdmi && !shared.doc->children[1].supportsDvi);
    // MIPI DSI children have no DDI port on display version 10.
    Parsed dsi = withChildren({{kDeviceHandleLfp1, 0x1400, 21, 0}});
    CHECK(dsi.doc->children[0].supportsDsi);
    CHECK_EQ(dsi.doc->children[0].i915Port, 0xff);
    CHECK_EQ(profileOf(*dsi.doc).gaps, Gap::kNoEdpChild);
    // Child size rules from child_device_size_valid.
    Parsed tiny = withChildren({edpA}, 32);
    CHECK_EQ(tiny.doc->childIssue, ChildIssue::ChildSizeTooSmall);
    CHECK_EQ(tiny.doc->childCount, 0);
    CHECK(record(*tiny.doc, 2)->disposition == BlockDisposition::Rejected);
    CHECK_EQ(profileOf(*tiny.doc).gaps, Gap::kNoEdpChild);
    Parsed mismatch = withChildren({edpA}, 38);
    CHECK(mismatch.doc->childDevSizeMismatch);
    CHECK_EQ(mismatch.doc->childCount, 1);
    CHECK(profileOf(*mismatch.doc).ready);
    auto truncatedDefs = base; truncatedDefs[findBlock(truncatedDefs, 2)].data.resize(4);
    CHECK_EQ(parse(assemble(truncatedDefs, o)).doc->childIssue, ChildIssue::BlockTooSmall);
    std::vector<ChildSpec> many(33, dpC);
    Parsed crowd = withChildren(many);
    CHECK_EQ(crowd.doc->childIssue, ChildIssue::TooManyChildren);
    CHECK_EQ(crowd.doc->childCount, 0);
    // Version-gated child fields.
    VbtOptions o230 = o; o230.bdbVersion = 244;
    auto b230 = base;
    b230[findBlock(b230, 2)].data = generalDefinitions(39, {{kDeviceHandleLfp1, 0x78c6, 10, 0x40, 0xc0 | 0x12, 3}}, 0);
    Parsed v244 = parse(assemble(b230, o230));
    CHECK_EQ(v244.doc->children[0].dpMaxLinkRate, 540000u);   // 230+ table code 3 = HBR2
    CHECK_EQ(v244.doc->children[0].dpMaxLaneCount, 4);
    CHECK(v244.doc->children[0].laneReversal && v244.doc->children[0].hpdInvert);
    CHECK(v244.doc->childDevSizeMismatch == false);
    VbtOptions o263 = o; o263.bdbVersion = 263;
    auto b263 = base;
    b263[findBlock(b263, 2)].data = generalDefinitions(44, {{kDeviceHandleLfp1, 0x78c6, 10, 0x40, 0, 0, 0, 0, 0xfffff048u}}, 0);
    Parsed v263 = parse(assemble(b263, o263));
    CHECK(v263.doc->children[0].edpDataRateOverrideKnown);
    CHECK_EQ(v263.doc->children[0].edpDataRateOverride, 0x048);
}

static void testVersionedChildBits() {
    // These reserved bytes acquire meaning only at their declared BDB version.
    // Wire bits deliberately remain set while the version crosses each gate.
    const struct { uint16_t v; bool ganged, lane, lspcon, hpd, vswing; } rows[] = {
        {155,false,false,false,false,false}, {183,false,false,false,false,false},
        {184,false,true,false,false,false}, {191,false,true,false,false,false},
        {192,false,true,true,false,false}, {195,false,true,true,false,false},
        {196,false,true,true,true,false}, {201,false,true,true,true,false},
        {202,true,true,true,true,false}, {217,true,true,true,true,false},
        {218,true,true,true,true,true}};
    for (const auto &row: rows) {
        VbtOptions o; auto b=fixtureA(o); o.bdbVersion=row.v;
        b[findBlock(b,2)].data=generalDefinitions(39, {{kDeviceHandleLfp1,0x78c6,10,0x40,0x36,0,8}},0);
        Parsed r=parse(assemble(b,o)); const auto &c=r.doc->children[0];
        CHECK_EQ(c.gangedEdp,row.ganged); CHECK_EQ(c.laneReversal,row.lane);
        CHECK_EQ(c.lspcon,row.lspcon); CHECK_EQ(c.hpdInvert,row.hpd);
        CHECK_EQ(c.useVbtVswing,row.vswing);
    }
}

static void testVersionLimits() {
    VbtOptions o; auto b = fixtureA(o);
    o.bdbVersion = 264;
    Parsed latestReviewed = parse(assemble(b, o));
    CHECK(profileOf(*latestReviewed.doc).ready); // data-only format policy
    CHECK_EQ(kMaxKnownBdbVersion, 264); // not a measured GLK firmware limit
    o.bdbVersion = 265;
    Parsed newer = parse(assemble(b, o));
    CHECK_EQ(newer.error, VbtError::None);
    CHECK_EQ(newer.doc->childDevSizeExpected, -1);
    CHECK(newer.doc->childDevSizeMismatch);  // 39 vs sizeof(child_device_config)
    GlkEdpPanelProfile p = profileOf(*newer.doc);
    CHECK(!p.ready);
    CHECK_EQ(p.gaps, Gap::kBdbVersionTooNew);
    CHECK_EQ(childDeviceExpectedSize(264), 44);
    CHECK_EQ(childDeviceExpectedSize(263), 44);
    CHECK_EQ(childDeviceExpectedSize(262), 40);
    CHECK_EQ(childDeviceExpectedSize(256), 40);
    CHECK_EQ(childDeviceExpectedSize(255), 39);
    CHECK_EQ(childDeviceExpectedSize(216), 39);
    CHECK_EQ(childDeviceExpectedSize(215), 38);
    CHECK_EQ(childDeviceExpectedSize(196), 38);
    CHECK_EQ(childDeviceExpectedSize(195), 37);
    CHECK_EQ(childDeviceExpectedSize(194), 33);
    CHECK_EQ(childDeviceExpectedSize(111), 33);
    CHECK_EQ(childDeviceExpectedSize(110), 27);
    CHECK_EQ(childDeviceExpectedSize(106), 27);
    CHECK_EQ(childDeviceExpectedSize(105), 22);
    CHECK_EQ(childDeviceExpectedSize(0), 22);
    CHECK_EQ(childDeviceExpectedSize(265), -1);
    Parsed none{}; none.doc->error = VbtError::BlockOverflow;
    CHECK_EQ(profileOf(*none.doc).gaps, Gap::kDocumentInvalid);
}

static void expectError(const Bytes &v, VbtError expected, size_t length = SIZE_MAX) {
    Parsed r = parse(v, length);
    CHECK_EQ(r.error, expected);
    CHECK_EQ(r.doc->error, expected);
    CHECK_EQ(r.doc->blockCount, 0);
    CHECK(!r.doc->edp.present && !r.doc->lfp.present && !r.doc->lfpOptions.present);
    CHECK_EQ(r.doc->childCount, 0);
    CHECK(!profileOf(*r.doc).ready);
}

static void testHeaderMalformations() {
    const Bytes good = buildA();
    VbtDocument doc;
    CHECK_EQ(parseVbt(nullptr, 100, doc), VbtError::InvalidArgument);
    expectError(good, VbtError::VbtHeaderIncomplete, 47);
    Bytes sig = good; sig[3] = 'X'; expectError(sig, VbtError::VbtSignatureInvalid);
    Bytes hs = good; put16(hs, 22, 47); expectError(hs, VbtError::VbtHeaderSizeInvalid);
    Bytes hb = good; put16(hb, 22, uint16_t(good.size() + 1)); expectError(hb, VbtError::VbtHeaderSizeInvalid);
    Bytes vs = good; put16(vs, 24, uint16_t(good.size() + 1)); expectError(vs, VbtError::VbtSizeExceedsBuffer);
    Bytes overlap = good; put32(overlap, 28, 40); expectError(overlap, VbtError::BdbOverlapsVbtHeader);
    Bytes late = good; put32(late, 28, uint32_t(good.size() - 21)); expectError(late, VbtError::BdbHeaderIncomplete);
    Bytes huge = good; put32(huge, 28, 0xfffffff0u); expectError(huge, VbtError::BdbHeaderIncomplete);
    Bytes bsig = good; bsig[48 + 14] = 'X'; expectError(bsig, VbtError::BdbSignatureInvalid);
    Bytes bhs = good; put16(bhs, 48 + 18, 21); expectError(bhs, VbtError::BdbHeaderSizeInvalid);
    Bytes bhb = good; put16(bhb, 48 + 18, uint16_t(get16(good, 48 + 20) + 1)); expectError(bhb, VbtError::BdbHeaderSizeInvalid);
    Bytes bsz = good; put16(bsz, 48 + 20, uint16_t(good.size() - 48 + 1)); expectError(bsz, VbtError::BdbSizeExceedsVbt);
    // Truncation sweep: every proper prefix of a valid image must be rejected.
    int accepted = 0;
    for (size_t n = 0; n < good.size(); ++n) {
        Parsed r = parse(good, n);
        if (r.error == VbtError::None) ++accepted;
        if (n < 48) CHECK_EQ(r.error, VbtError::VbtHeaderIncomplete);
    }
    CHECK_EQ(accepted, 0);
    // Extra buffer bytes past vbt_size are ignored; checksum is recorded only.
    Bytes padded = good; padded.resize(good.size() + 100, 0xaa);
    CHECK_EQ(parse(padded).error, VbtError::None);
    Bytes sum = good; sum[26] = uint8_t(sum[26] + 1);
    Parsed rs = parse(sum);
    CHECK_EQ(rs.error, VbtError::None);
    CHECK_EQ(rs.doc->byteSum, 1);
    CHECK(!profileOf(*rs.doc).ready); // diagnostic parse is not native-ready data
    CHECK_EQ(profileOf(*rs.doc).gaps, Gap::kChecksumInvalid);
}

static void testBlockWalk() {
    VbtOptions o; auto base = fixtureA(o);
    // A block running past bdb_size invalidates the whole image.
    auto over = base; over.back().sizeField = int32_t(over.back().data.size() + 1);
    expectError(assemble(over, o), VbtError::BlockOverflow);
    // MIPI sequence v3+: the walk uses the u32 size after the version byte.
    auto mipi = base;
    Bytes seq{3, 12, 0, 0, 0, 9, 9, 9, 9, 9, 9, 9};
    Block m{53, seq}; m.sizeField = 0;
    mipi.insert(mipi.begin() + 1, m);
    Parsed rm = parse(assemble(mipi, o));
    CHECK_EQ(rm.error, VbtError::None);
    CHECK(record(*rm.doc, 53) && record(*rm.doc, 53)->mipiSequenceV3Size);
    CHECK_EQ(record(*rm.doc, 53)->size, 12u);
    CHECK(profileOf(*rm.doc).ready);
    auto mipi2 = base;                    // v2 keeps the u16 size
    Block m2{53, Bytes{2, 1, 1}};
    mipi2.insert(mipi2.begin() + 1, m2);
    Parsed rm2 = parse(assemble(mipi2, o));
    CHECK(record(*rm2.doc, 53) && !record(*rm2.doc, 53)->mipiSequenceV3Size);
    CHECK(profileOf(*rm2.doc).ready);
    auto mipiCut = base; mipiCut.push_back({53, Bytes{3}});
    expectError(assemble(mipiCut, o), VbtError::BlockHeaderTruncated);
    auto mipiLong = base; Block ml{53, Bytes{3, 0xff, 0xff, 0, 0, 1}}; mipiLong.push_back(ml);
    expectError(assemble(mipiLong, o), VbtError::BlockOverflow);
    // Duplicates: ambiguous decoded blocks are fatal, others are retained.
    auto dupEdp = base; dupEdp.push_back(dupEdp[findBlock(dupEdp, 27)]);
    expectError(assemble(dupEdp, o), VbtError::DuplicateDecodedBlock);
    auto dupUnknown = base; dupUnknown.push_back({0xee, Bytes{5}});
    Parsed du = parse(assemble(dupUnknown, o));
    CHECK_EQ(du.error, VbtError::None);
    CHECK(record(*du.doc, 0xee, 1) && record(*du.doc, 0xee, 1)->disposition == BlockDisposition::Duplicate);
    CHECK(profileOf(*du.doc).ready);
    auto full = base; for (int i = 0; i < 120; ++i) full.push_back({0xef, Bytes{}});
    expectError(assemble(full, o), VbtError::BlockTableFull);
    // Fewer than 4 trailing bytes are never read; 4 zero bytes form a block 0.
    VbtOptions t3 = o; t3.trailingBdb = 3;
    Parsed r3 = parse(assemble(base, t3));
    CHECK_EQ(r3.error, VbtError::None);
    CHECK_EQ(r3.doc->bdbTrailingBytes, 3);
    VbtOptions t4 = o; t4.trailingBdb = 4;
    Parsed r4 = parse(assemble(base, t4));
    CHECK_EQ(r4.error, VbtError::None);
    CHECK_EQ(r4.doc->bdbTrailingBytes, 1);
    CHECK(record(*r4.doc, 0) && record(*r4.doc, 0)->disposition == BlockDisposition::Unknown);
}

static void testLfpPointerValidation() {
    VbtOptions o; auto base = fixtureA(o);
    const size_t i41 = findBlock(base, 41), i42 = findBlock(base, 42);
    // Materialize block 41 so individual bytes can be corrupted.
    uint32_t dataOffset = 22;
    for (size_t i = 0; i < i42; ++i) dataOffset += 3 + uint32_t(base[i].data.size());
    dataOffset += 3;
    const Bytes ptrs = lfpPtrs(dataOffset, LfpLayoutSpec{});
    auto expectIssue = [&](const Bytes &p, LfpIssue issue, const Bytes *data = nullptr) {
        auto b = base; b[i41].lateLfpPtrs = false; b[i41].data = p;
        if (data) b[i42].data = *data;
        Parsed r = parse(assemble(b, o));
        CHECK_EQ(r.error, VbtError::None);
        CHECK(!r.doc->lfp.present);
        CHECK_EQ(r.doc->lfp.issue, issue);
        CHECK(record(*r.doc, 41)->disposition == BlockDisposition::Rejected);
        GlkEdpPanelProfile p2 = profileOf(*r.doc);
        CHECK(!p2.ready);
        CHECK((p2.gaps & Gap::kNoTiming) != 0);
    };
    {
        auto b = base; b[i41].lateLfpPtrs = false; b[i41].data = ptrs;
        CHECK(profileOf(*parse(assemble(b, o)).doc).ready);   // materialized copy is valid
    }
    Bytes p = ptrs; p[0] = 2; expectIssue(p, LfpIssue::EntryCountInvalid);
    p = ptrs; for (int e = 0; e < 16; ++e) p[1 + e * 9 + 2] = 31; expectIssue(p, LfpIssue::TableSizeInvalid);
    p = ptrs; for (int e = 0; e < 16; ++e) p[1 + e * 9 + 5] = 17; expectIssue(p, LfpIssue::TableSizeInvalid);
    p = ptrs; for (int e = 0; e < 16; ++e) p[1 + e * 9 + 8] = 11; expectIssue(p, LfpIssue::TableSizeInvalid);
    p = ptrs; p[147] = 12; expectIssue(p, LfpIssue::PanelNameSizeInvalid);
    p = ptrs; put16(p, 1 + 7 * 9 + 3, uint16_t(get16(p, 1 + 7 * 9 + 3) + 1)); expectIssue(p, LfpIssue::EntryStrideInvalid);
    p = ptrs; p[1 + 9 * 9 + 2] = 33; expectIssue(p, LfpIssue::EntryStrideInvalid);
    p = ptrs; for (int e = 0; e < 16; ++e) p[1 + e * 9 + 2] = 40; expectIssue(p, LfpIssue::LayoutInvalid);
    p = ptrs; put16(p, 1, uint16_t(dataOffset - 1)); expectIssue(p, LfpIssue::OffsetBelowDataBlock);
    p = ptrs; put16(p, 145, uint16_t(dataOffset + 2000)); expectIssue(p, LfpIssue::TableOutsideBlock);
    Bytes shortPtrs = ptrs; shortPtrs.resize(144); expectIssue(shortPtrs, LfpIssue::PointersTooShort);
    std::vector<ModeSpec> modes(16, kMode1080);
    Bytes noTerm = lfpData(LfpLayoutSpec{}, modes, true); noTerm[4 * 66 + 36] = 0;
    expectIssue(ptrs, LfpIssue::TerminatorMissing, &noTerm);
    Bytes cut = lfpData(LfpLayoutSpec{}, modes, false); cut.resize(16 * 66 - 1);
    expectIssue(ptrs, LfpIssue::TableOutsideBlock, &cut);
    Bytes empty; expectIssue(ptrs, LfpIssue::DataBlockEmpty, &empty);
    auto noData = base; noData[i41].lateLfpPtrs = false; noData[i41].data = ptrs;
    noData.erase(noData.begin() + long(i42));
    Parsed nd = parse(assemble(noData, o));
    CHECK_EQ(nd.doc->lfp.issue, LfpIssue::DataBlockAbsent);
    // Short block 41 without the panel-name table is still valid (upstream zero-fill).
    Bytes noNames = ptrs; noNames.resize(145);
    // Block 41 precedes 42: removing its 3-byte optional name pointer moves
    // every BDB-absolute table address back by 3 bytes.
    for (size_t e = 0; e < 16; ++e) for (size_t t = 0; t < 3; ++t) {
        const size_t off = 1 + e * 9 + t * 3;
        put16(noNames, off, uint16_t(get16(noNames, off) - 3));
    }
    auto b = base; b[i41].lateLfpPtrs = false; b[i41].data = noNames;
    Parsed nn = parse(assemble(b, o));
    CHECK(nn.doc->lfp.present && !nn.doc->lfp.entries[2].nameKnown);
    CHECK(profileOf(*nn.doc).ready);
}

static void testEdpAndBacklight() {
    VbtOptions o; auto base = fixtureA(o);
    const size_t i27 = findBlock(base, 27), i43 = findBlock(base, 43);
    auto edpCut = base; edpCut[i27].data.resize(29);
    GlkEdpPanelProfile cut = profileOf(*parse(assemble(edpCut, o)).doc);
    CHECK_EQ(cut.gaps, Gap::kPpsMissing);
    auto edpShort = base; edpShort[i27].data.resize(30);   // panel 2 sequence fits exactly
    GlkEdpPanelProfile fits = profileOf(*parse(assemble(edpShort, o)).doc);
    CHECK(fits.ready);
    CHECK_EQ(fits.bpp, 0);                // color depth not in the block
    CHECK_EQ(fits.linkRate, 0u);
    CHECK_EQ(fits.lanes, 0);
    auto zero = base; for (int k = 0; k < 10; ++k) zero[i27].data[20 + size_t(k)] = 0;
    CHECK_EQ(profileOf(*parse(assemble(zero, o)).doc).gaps, Gap::kPpsAllZero);
    auto partial = base; for (int k = 0; k < 8; ++k) partial[i27].data[20 + size_t(k)] = 0;
    GlkEdpPanelProfile pz = profileOf(*parse(assemble(partial, o)).doc);
    CHECK(pz.ready);                      // only T11+T12 present; i915 merges per field
    CHECK_EQ(pz.t11t12Ms, 510u);
    auto noEdp = base; noEdp.erase(noEdp.begin() + long(i27));
    CHECK_EQ(profileOf(*parse(assemble(noEdp, o)).doc).gaps, Gap::kNoEdpBlock);
    auto badDepth = base; put32(badDepth[i27].data, 160, 3u << 4);
    CHECK_EQ(profileOf(*parse(assemble(badDepth, o)).doc).bpp, 0);
    // Backlight-derived PPS instance.
    BacklightSpec bl[16];
    for (int p = 0; p < 16; ++p) bl[p] = BacklightSpec{2, false, 200, 10, 255, 0x22, 0, 0, 0};
    auto ctl2 = base; ctl2[i43].data = backlightBlock(bl);
    GlkEdpPanelProfile c2 = profileOf(*parse(assemble(ctl2, o)).doc);
    CHECK_EQ(c2.gaps, Gap::kPpsInstanceInvalid);
    CHECK_EQ(c2.ppsInstance, -1);
    for (int p = 0; p < 16; ++p) bl[p].type = 0;
    auto noPwm = base; noPwm[i43].data = backlightBlock(bl);
    GlkEdpPanelProfile np = profileOf(*parse(assemble(noPwm, o)).doc);
    CHECK_EQ(np.gaps, Gap::kPpsInstanceUnknown);
    CHECK(!np.backlightPwm);
    auto entry5 = base; entry5[i43].data[0] = 5;
    Parsed e5 = parse(assemble(entry5, o));
    CHECK_EQ(e5.doc->backlight.issue, BacklightIssue::EntrySizeUnsupported);
    CHECK(record(*e5.doc, 43)->disposition == BlockDisposition::Rejected);
    CHECK_EQ(profileOf(*e5.doc).gaps, Gap::kPpsInstanceUnknown);
    auto blShort = base; blShort[i43].data.resize(114);   // control[0] only
    CHECK_EQ(profileOf(*parse(assemble(blShort, o)).doc).gaps, Gap::kPpsInstanceUnknown);
    auto noBl = base; noBl.erase(noBl.begin() + long(i43));
    Parsed nb = parse(assemble(noBl, o));
    CHECK_EQ(nb.doc->backlight.issue, BacklightIssue::BlockAbsent);
    CHECK_EQ(profileOf(*nb.doc).gaps, Gap::kPpsInstanceUnknown);
    // 234+ minimum brightness above 255 is reported unknown, never truncated.
    VbtOptions o2 = o; o2.bdbVersion = 235;
    for (int p = 0; p < 16; ++p) bl[p] = BacklightSpec{2, false, 200, 10, 255, 0x02, 200, 300, 0};
    auto hi = base; hi[i43].data = backlightBlock(bl);
    GlkEdpPanelProfile ph = profileOf(*parse(assemble(hi, o2)).doc);
    CHECK(ph.ready);
    CHECK(!ph.minBrightnessKnown);
    for (int p = 0; p < 16; ++p) bl[p].bLevel = 1000;    // > 255 selects scaling before 236
    hi[i43].data = backlightBlock(bl);
    GlkEdpPanelProfile ps = profileOf(*parse(assemble(hi, o2)).doc);
    CHECK(ps.minBrightnessKnown);
    CHECK_EQ(ps.minBrightness, 1);
    // From version236 precision decides the scale. A truncated precision
    // byte must not turn an ambiguous raw minimum into a known 8-bit value.
    o2.bdbVersion=236;
    for (int p=0;p<16;++p) bl[p]=BacklightSpec{2,false,200,10,255,0x02,65535,255,16};
    hi[i43].data=backlightBlock(bl);
    CHECK_EQ(profileOf(*parse(assemble(hi,o2)).doc).minBrightness,1);
    hi[i43].data.resize(257); // full level arrays, no precision bytes
    CHECK(!profileOf(*parse(assemble(hi,o2)).doc).minBrightnessKnown);
}

static void testTimingDecode() {
    ModeSpec m{65535, 4095, 4095, 1023, 1023, 4095, 4095, 63, 63, 4095, 4095, true, true};
    const Bytes enc = encodeDtd(m);
    VbtDtd d{};
    CHECK(decodeLfpDtd(enc.data(), enc.size(), d));
    CHECK_EQ(d.clockKhz, 655350u);
    CHECK_EQ(d.hdisplay, 4095u);
    CHECK_EQ(d.hsyncStart, 5118u);
    CHECK_EQ(d.htotal, 8190u);
    CHECK_EQ(d.hsyncEnd, 6141u);
    CHECK_EQ(d.vsyncStart, 4158u);
    CHECK_EQ(d.vsyncEnd, 4221u);
    CHECK_EQ(d.vtotal, 8190u);
    CHECK_EQ(d.widthMm, 4095u);
    CHECK_EQ(d.heightMm, 4095u);
    CHECK(d.hsyncPositive && d.vsyncPositive);
    CHECK(!decodeLfpDtd(enc.data(), 17, d));
    CHECK(!decodeLfpDtd(nullptr, 18, d));
    // Bogus sync end beyond total is clamped exactly like i915.
    ModeSpec clamp{6500, 1024, 100, 90, 50, 768, 10, 8, 15, 0, 0, false, false};
    const Bytes ce = encodeDtd(clamp);
    CHECK(decodeLfpDtd(ce.data(), ce.size(), d));
    CHECK_EQ(d.hsyncEnd, 1124u);
    CHECK(d.hsyncEndClamped && d.vsyncEndClamped);
    CHECK_EQ(d.vsyncEnd, 778u);
    CHECK(glkModeValid(d));
    // Single-bit positions of the packed DTD bytes.
    const struct { size_t byte; uint8_t value; uint32_t VbtDtd::*field; uint32_t expected; } bits[] = {
        {4, 0x10, &VbtDtd::hdisplay, 256}, {4, 0x01, &VbtDtd::htotal, 256},
        {7, 0x10, &VbtDtd::vdisplay, 256}, {7, 0x01, &VbtDtd::vtotal, 256},
        {11, 0x40, &VbtDtd::hsyncStart, 256}, {11, 0x10, &VbtDtd::hsyncEnd, 256},
        {11, 0x04, &VbtDtd::vsyncStart, 16}, {11, 0x01, &VbtDtd::vsyncEnd, 16},
        {10, 0x10, &VbtDtd::vsyncStart, 1}, {10, 0x01, &VbtDtd::vsyncEnd, 1},
        {14, 0x10, &VbtDtd::widthMm, 256}, {14, 0x01, &VbtDtd::heightMm, 256}};
    for (const auto &b : bits) {
        Bytes z(18, 0); z[b.byte] = b.value;
        // Linux always clamps sync end to total. Supply a sufficient total
        // when isolating an end-field bit, rather than testing a zero total.
        if (b.field == &VbtDtd::hsyncEnd) z[4] = 0x0f;
        if (b.field == &VbtDtd::vsyncEnd) z[7] = 0x0f;
        VbtDtd t{};
        CHECK(decodeLfpDtd(z.data(), z.size(), t));
        CHECK_EQ(t.*b.field, b.expected);
    }
    Bytes flags(18, 0); flags[17] = 0x40;
    CHECK(decodeLfpDtd(flags.data(), 18, d) && d.hsyncPositive && !d.vsyncPositive);
    flags[17] = 0x20;
    CHECK(decodeLfpDtd(flags.data(), 18, d) && !d.hsyncPositive && d.vsyncPositive);
    flags[17] = 0x06;                     // EDID-style bits are not i915's layout
    CHECK(decodeLfpDtd(flags.data(), 18, d) && !d.hsyncPositive && !d.vsyncPositive);
    // Generic DTD: no clamp, kHz clock, u16 fields.
    const Bytes g = encodeGeneric({200000, 3840, 160, 48, 32, 2160, 62, 3, 5, 600, 340, false, true});
    CHECK(decodeGenericDtd(g.data(), g.size(), d));
    CHECK_EQ(d.clockKhz, 200000u);
    CHECK_EQ(d.htotal, 4000u);
    CHECK_EQ(d.vtotal, 2222u);
    CHECK(!d.hsyncPositive && d.vsyncPositive);
    CHECK(!decodeGenericDtd(g.data(), 27, d));
    const Bytes gb = encodeGeneric({1000, 100, 10, 8, 5, 100, 10, 1, 1, 0, 0, false, false});
    CHECK(decodeGenericDtd(gb.data(), gb.size(), d));
    CHECK(!d.hsyncEndClamped);
    CHECK_EQ(d.hsyncEnd, 113u);
    CHECK(!glkModeValid(d));              // hsync_end past htotal
    CHECK_EQ(modeRefreshMilliHz(d), 0u);
    // Mode validity and refresh rounding.
    VbtDtd v{};
    CHECK(!glkModeValid(v));
    v = VbtDtd{138500, 1920, 1968, 2000, 2080, 1080, 1083, 1088, 1111, 0, 0, false, false, false, false};
    CHECK(glkModeValid(v));
    CHECK_EQ(modeRefreshMilliHz(v), 59934u);
    VbtDtd noClock = v; noClock.clockKhz = 0; CHECK(!glkModeValid(noClock));
    VbtDtd noBlank = v; noBlank.htotal = 1920; noBlank.hsyncStart = 1920; noBlank.hsyncEnd = 1920;
    CHECK(!glkModeValid(noBlank));
    VbtDtd vBad = v; vBad.vsyncStart = 1112; vBad.vsyncEnd = 1112; CHECK(!glkModeValid(vBad));
    VbtDtd wide = v; wide.clockKhz = 0xffffffffu;
    CHECK_EQ(modeRefreshMilliHz(wide), uint32_t((0xffffffffull * 1000000ull + 2310880ull / 2) / 2310880ull));
    // PPS unit conversions (100 us units, ceil to ms).
    CHECK_EQ(ppsUnitsToMicroseconds(0), 0u);
    CHECK_EQ(ppsUnitsToMicroseconds(1), 100u);
    CHECK_EQ(ppsUnitsToMicroseconds(65535), 6553500u);
    CHECK_EQ(ppsUnitsToMillisecondsCeil(0), 0u);
    CHECK_EQ(ppsUnitsToMillisecondsCeil(1), 1u);
    CHECK_EQ(ppsUnitsToMillisecondsCeil(10), 1u);
    CHECK_EQ(ppsUnitsToMillisecondsCeil(11), 2u);
    CHECK_EQ(ppsUnitsToMillisecondsCeil(65535), 6554u);
    CHECK(std::strcmp(vbtErrorName(VbtError::BlockOverflow), "BlockOverflow") == 0);
    CHECK(std::strcmp(vbtErrorName(VbtError(200)), "UnknownVbtError") == 0);
}

// Deterministic mutation sweep. Only memory safety (with sanitizers) and the
// ready-profile invariants are asserted; mutated bytes may still be valid.
static void testMutations() {
    const Bytes good = buildA();
    uint64_t state = 0x9e3779b97f4a7c15ull;
    auto next = [&]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    int ready = 0, rejected = 0, invariantFailures = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        Bytes v = good;
        const int flips = 1 + int(next() % 8);
        for (int f = 0; f < flips; ++f) v[size_t(next() % v.size())] = uint8_t(next());
        if (next() % 4 == 0) fixChecksum(v);
        size_t length = v.size();
        if (next() % 5 == 0) length = size_t(next() % (v.size() + 1));
        Parsed r = parse(v, length);
        GlkPanelRequest req;
        if (next() % 3 == 0) { req.edidProductIdValid = true; pnpFor(int(next() % 16), req.edidProductId); }
        const GlkEdpPanelProfile p = profileOf(*r.doc, req);
        if (r.error != VbtError::None) {
            ++rejected;
            if (r.doc->blockCount != 0 || p.ready || r.doc->childCount != 0) ++invariantFailures;
            continue;
        }
        if (r.doc->blockCount > kVbtMaxBlocks || r.doc->childCount > kVbtMaxChildren) ++invariantFailures;
        if (!p.ready) continue;
        ++ready;
        const bool ok = p.gaps == 0 && p.panelType >= 0 && p.panelType < 16 && glkModeValid(p.mode) &&
            p.port != GlkPort::None && (p.aux == AuxChannel::A || p.aux == AuxChannel::B || p.aux == AuxChannel::C) &&
            (p.ppsInstance == 0 || p.ppsInstance == 1) &&
            (p.pps.t1t3 | p.pps.t8 | p.pps.t9 | p.pps.t10 | p.pps.t11t12) != 0 &&
            p.timingSource != TimingSource::None && p.panelTypeSource != PanelTypeSource::None &&
            r.doc->bdbVersion >= kMinGlkBdbVersion && r.doc->bdbVersion <= kMaxKnownBdbVersion && r.doc->byteSum == 0;
        if (!ok) ++invariantFailures;
    }
    CHECK_EQ(invariantFailures, 0);
    CHECK(ready > 0);
    CHECK(rejected > 0);
    std::printf("mutation sweep: 4000 cases, %d ready, %d rejected\n", ready, rejected);
}

int main() {
    testFixtureAStructure();
    testFixtureAProfile();
    testGenericDtdPath();
    testGeneratedPointersAndNoDefaultController();
    testLegacyControllerRequiresSelectedPwm();
    testPanelTypeSelection();
    testChildSelection();
    testVersionedChildBits();
    testVersionLimits();
    testHeaderMalformations();
    testBlockWalk();
    testLfpPointerValidation();
    testEdpAndBacklight();
    testTimingDecode();
    testMutations();
    std::printf("native_glk_vbt_tests: %d checks, %d failures (synthetic fixtures, no hardware)\n",
                gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

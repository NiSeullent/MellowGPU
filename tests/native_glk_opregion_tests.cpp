// Hosted regression for Drivers/NativeGlk/OpRegionSnapshot and
// Mellow/GlkOpRegionIOKit, driven through the unchanged MellowGlkPhysicalOwner.
// Synthetic OpRegion/VBT images only; no firmware bytes are shipped or read.
// HOSTED INJECTED MODEL: never evidence about the physical 15IGL7.
// Every CHECK is fatal (abort), so sanitizer runs stop at the first failure.
#include "../Mellow/GlkOpRegionIOKit.hpp"
#include "../Drivers/NativeGlk/PageTableEncoding.hpp"
#include <cstddef>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace MellowNative;
namespace NG = Mellow::NativeGlk;
using OS = NG::OpRegionStatus;
using VS = NG::VbtStatus;
using GS = GlkOpRegionStatus;

#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "FATAL %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// Link boundary for the frozen owner: DMA attachment is never exercised here.
namespace MellowNative {
MemoryStatus GlkDmaMemoryIOKit::attach(IOPCIDevice *, IOService *, GlkDmaIdentity, MemoryLimits,
                                       GlkPhysicalVmOps) { std::abort(); }
MemoryStatus GlkDmaMemoryIOKit::detach() { std::abort(); }
uint8_t glkFirmwareLoad8(const volatile uint8_t *at) {
    auto &s = OpRegionShim::state;
    const uint8_t value = *at;
    ++s.loads;
    if (s.onLoad) s.onLoad(s.loads);
    return value;
}
}

// Packed mirrors of the pinned Linux definitions; the constants must agree.
namespace layout {
struct __attribute__((packed)) Over { uint8_t rsvd, revision, minor, major; };
struct __attribute__((packed)) OpRegionHeader {
    uint8_t signature[16]; uint32_t size; Over over; uint8_t bios_ver[32], vbios_ver[16], driver_ver[16];
    uint32_t mboxes, driver_model, pcon; uint8_t dver[32], rsvd[124];
};
struct __attribute__((packed)) Asle {
    uint32_t ardy, aslc, tche, alsi, bclp, pfit, cblv; uint16_t bclm[20]; uint32_t cpfm, epfm;
    uint8_t plut[74]; uint32_t pfmb, cddv, pcft, srot, iuer; uint64_t fdss; uint32_t fdsp, stat;
    uint64_t rvda; uint32_t rvds; uint8_t rsvd[58];
};
struct __attribute__((packed)) VbtHeader {
    uint8_t signature[20]; uint16_t version, header_size, vbt_size; uint8_t vbt_checksum, reserved0;
    uint32_t bdb_offset, aim_offset[4];
};
struct __attribute__((packed)) BdbHeader { uint8_t signature[16]; uint16_t version, header_size, bdb_size; };
static_assert(sizeof(OpRegionHeader) == NG::OpRegion::kHeaderBytes, "opregion_header");
static_assert(offsetof(OpRegionHeader, size) == NG::OpRegion::kHeaderSize, "size");
static_assert(offsetof(OpRegionHeader, over) + 1 == NG::OpRegion::kHeaderRevision, "revision");
static_assert(offsetof(OpRegionHeader, over) + 2 == NG::OpRegion::kHeaderMinor, "minor");
static_assert(offsetof(OpRegionHeader, over) + 3 == NG::OpRegion::kHeaderMajor, "major");
static_assert(offsetof(OpRegionHeader, mboxes) == NG::OpRegion::kHeaderMailboxes, "mboxes");
static_assert(offsetof(OpRegionHeader, driver_model) == NG::OpRegion::kHeaderDriverModel, "model");
static_assert(offsetof(OpRegionHeader, pcon) == NG::OpRegion::kHeaderPcon, "pcon");
static_assert(sizeof(Asle) == 0x100, "opregion_asle");
static_assert(NG::OpRegion::kAsleOffset + offsetof(Asle, rvda) == NG::OpRegion::kRvdaOffset, "rvda");
static_assert(NG::OpRegion::kAsleOffset + offsetof(Asle, rvds) == NG::OpRegion::kRvdsOffset, "rvds");
static_assert(NG::OpRegion::kRvdsOffset + 4 - NG::OpRegion::kRvdaOffset == NG::OpRegion::kRvdBytes, "span");
static_assert(sizeof(VbtHeader) == NG::Vbt::kHeaderBytes, "vbt_header");
static_assert(offsetof(VbtHeader, vbt_size) == NG::Vbt::kSizeOffset, "vbt_size");
static_assert(offsetof(VbtHeader, vbt_checksum) == NG::Vbt::kChecksumOffset, "vbt_checksum");
static_assert(offsetof(VbtHeader, bdb_offset) == NG::Vbt::kBdbOffsetOffset, "bdb_offset");
static_assert(sizeof(BdbHeader) == NG::Vbt::kBdbHeaderBytes, "bdb_header");
static_assert(offsetof(BdbHeader, bdb_size) == NG::Vbt::kBdbSizeOffset, "bdb_size");
}

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kAsls = 0x7a5c2018, kRamBase = 0x7a5c2000, kRamBytes = 0x8000;
unsigned tests;

void le(Bytes &b, size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i)); }
void fixChecksum(Bytes &v, uint32_t size) {
    v[NG::Vbt::kChecksumOffset] = 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < size; ++i) sum = uint8_t(sum + v[i]);
    v[NG::Vbt::kChecksumOffset] = uint8_t(-sum);
}
// Synthetic VBT: header, BDB header, patterned payload, zero-sum checksum.
Bytes makeVbt(uint32_t size, uint8_t seed = 3, uint32_t bdb = 48) {
    Bytes v(size < 48 ? 48 : size, 0);
    std::memcpy(v.data(), "$VBT MELLOW SYNTH   ", 20);
    le(v, 0x14, 0x0100, 2); le(v, 0x16, 48, 2); le(v, 0x18, size, 2); le(v, 0x1c, bdb, 4);
    if (bdb + 22 <= v.size()) {
        std::memcpy(&v[bdb], "BIOS_DATA_BLOCK ", 16);
        le(v, bdb + 0x10, 0xf8, 2); le(v, bdb + 0x12, 22, 2); le(v, bdb + 0x14, size - bdb, 2);
    }
    for (size_t i = bdb + 22; i < size; ++i) v[i] = uint8_t(i * 7 + seed);
    if (size >= 48) fixChecksum(v, size);
    return v;
}
Bytes makeOpRegion(uint8_t major, uint8_t minor, uint32_t mboxes, uint64_t rvda = 0, uint32_t rvds = 0,
                   uint32_t declared = 8) {
    Bytes r(NG::OpRegion::kBytes, 0);
    std::memcpy(r.data(), "IntelGraphicsMem", 16);
    le(r, 0x10, declared, 4); r[0x16] = minor; r[0x17] = major; le(r, 0x58, mboxes, 4);
    le(r, NG::OpRegion::kRvdaOffset, rvda, 8); le(r, NG::OpRegion::kRvdsOffset, rvds, 4);
    return r;
}
void place(Bytes &region, const Bytes &vbt, size_t at = NG::OpRegion::kVbtOffset) {
    std::memcpy(&region[at], vbt.data(), vbt.size());
}
constexpr uint32_t kAsle = NG::OpRegion::kMboxAcpi | NG::OpRegion::kMboxAsle;

// ---- pure layer host -------------------------------------------------------
struct Host {
    Bytes region, rvdaBytes;
    uint64_t expectRvda {};
    uint32_t asls {kAsls}, aslsAgain {kAsls};
    int mapCalls {}, unmapCalls {}, regionCopies {}, rvdaCopies {}, mutateRegionAfter {-1},
        mutateRvdaAfter {-1}, failRegionCopy {-1};
    size_t mutateAt {};
    bool failMap {}, failUnmap {}, failAlloc {}, failObserve {};
    std::vector<std::pair<uint8_t *, uint32_t>> live;
    uint32_t lastAlloc {};
};
bool hostRegion(void *c, uint32_t off, uint8_t *out, uint32_t n) {
    auto *h = static_cast<Host *>(c);
    CHECK(off <= h->region.size() && n <= h->region.size() - off);
    if (h->regionCopies++ == h->failRegionCopy) return false;
    std::memcpy(out, &h->region[off], n);
    if (h->regionCopies == h->mutateRegionAfter) h->region[h->mutateAt] ^= 0x5a;
    return true;
}
bool hostRvda(void *c, uint32_t off, uint8_t *out, uint32_t n) {
    auto *h = static_cast<Host *>(c);
    CHECK(off <= h->rvdaBytes.size() && n <= h->rvdaBytes.size() - off);
    std::memcpy(out, &h->rvdaBytes[off], n);
    if (++h->rvdaCopies == h->mutateRvdaAfter) h->rvdaBytes[h->mutateAt] ^= 0x5a;
    return true;
}
bool hostMap(void *c, uint64_t physical, uint32_t bytes, NG::FirmwareWindow &w) {
    auto *h = static_cast<Host *>(c);
    ++h->mapCalls;
    // Only the RVDA decoded from this capture's header is ever requested.
    CHECK(physical == h->expectRvda && h->mapCalls == 1);
    if (h->failMap) return false;
    h->rvdaBytes.resize(bytes, 0xff);
    w = {h, hostRvda, bytes};
    return true;
}
bool hostUnmap(void *c) { auto *h = static_cast<Host *>(c); ++h->unmapCalls; return !h->failUnmap; }
uint8_t *hostAlloc(void *c, uint32_t bytes) {
    auto *h = static_cast<Host *>(c);
    if (h->failAlloc) return nullptr;
    auto *p = new uint8_t[bytes];
    h->live.push_back({p, bytes});
    h->lastAlloc = bytes;
    return p;
}
void hostRelease(void *c, uint8_t *p, uint32_t bytes) {
    auto *h = static_cast<Host *>(c);
    for (size_t i = 0; i < h->live.size(); ++i) {
        if (h->live[i].first != p) continue;
        CHECK(h->live[i].second == bytes);
        h->live.erase(h->live.begin() + long(i));
        delete[] p;
        return;
    }
    CHECK(!"release of an unknown block");
}
bool hostObserve(void *c, uint32_t &asls) {
    auto *h = static_cast<Host *>(c);
    asls = h->aslsAgain;
    return !h->failObserve;
}
NG::OpRegionPlatform platformOf(Host &h) { return {&h, hostMap, hostUnmap, hostAlloc, hostRelease, hostObserve}; }
OS runPure(Host &h, NG::OpRegionSnapshot &s) {
    const NG::FirmwareWindow w {&h, hostRegion, uint32_t(h.region.size())};
    return NG::captureOpRegion(h.asls, w, platformOf(h), s);
}
// Every path: nothing owned unless Ok, every successful map unmapped once.
void settle(Host &h, NG::OpRegionSnapshot &s, OS status) {
    if (status != OS::Ok) CHECK(!s.vbt && !s.vbtBytes);
    if (h.mapCalls && !h.failMap) CHECK(h.unmapCalls == 1);
    if (!h.mapCalls || h.failMap) CHECK(h.unmapCalls == 0);
    NG::releaseSnapshot(platformOf(h), s);
    CHECK(h.live.empty());
    ++tests;
}

void pureAdmission() {
    CHECK(NG::checkAsls(0) == OS::NotPresent && NG::checkAsls(UINT32_MAX) == OS::Unavailable);
    CHECK(NG::checkAsls(0xffffe001) == OS::Invalid && NG::checkAsls(0xffffe000) == OS::Ok);
    uint8_t h[256] {}, rvd[12] {};
    NG::OpRegionPlan plan;
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Signature);
    auto r = makeOpRegion(2, 0, kAsle, 0, 0, 7);
    std::memcpy(h, r.data(), 256);
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::HeaderSize);
    for (auto [major, minor, ok] : {std::tuple{1, 0, false}, {2, 2, false}, {3, 0, false}, {3, 1, false},
                                    {2, 0, true}, {2, 1, true}}) {
        r = makeOpRegion(uint8_t(major), uint8_t(minor), kAsle);
        std::memcpy(h, r.data(), 256);
        CHECK((NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok) == ok);
        if (!ok) CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Version);
    }
    ++tests;
}

void purePlan() {
    uint8_t h[256], rvd[12];
    NG::OpRegionPlan plan;
    auto load = [&](const Bytes &r) {
        std::memcpy(h, r.data(), 256); std::memcpy(rvd, &r[NG::OpRegion::kRvdaOffset], 12);
    };
    load(makeOpRegion(2, 0, NG::OpRegion::kMboxAcpi, 0x7a600000, 0x2000));   // no ASLE mailbox
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && !plan.rvdaCandidate && !plan.rvda);
    CHECK(plan.mailboxLimit == NG::OpRegion::kBytes);
    load(makeOpRegion(2, 0, kAsle | NG::OpRegion::kMboxAsleExt, 0, 0x2000));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && !plan.rvdaCandidate);
    CHECK(plan.mailboxLimit == NG::OpRegion::kAsleExtOffset);
    load(makeOpRegion(2, 0, kAsle, 0x7a600000, 0));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && !plan.rvdaCandidate);
    load(makeOpRegion(2, 0, kAsle, 0x7a600000, 0x2000));                    // 2.0: physical
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && plan.rvdaCandidate && !plan.rvdaRelative);
    CHECK(plan.rvdaPhysical == 0x7a600000 && plan.rvds == 0x2000);
    load(makeOpRegion(2, 1, kAsle, 0x10000, 0x2000));                       // 2.1: ASLS-relative
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && plan.rvdaRelative);
    CHECK(plan.rvdaPhysical == uint64_t(kAsls) + 0x10000);
    load(makeOpRegion(2, 1, kAsle, NG::OpRegion::kBytes, 0x2000));          // boundary: just past
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok && plan.rvdaPhysical == uint64_t(kAsls) + 0x2000);
    load(makeOpRegion(2, 1, kAsle, NG::OpRegion::kBytes - 1, 0x2000));      // inside the OpRegion
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::RvdaRange);
    load(makeOpRegion(2, 1, kAsle, 0x400, 0x1000));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::RvdaRange);
    load(makeOpRegion(2, 0, kAsle, 0x7a600000, NG::Vbt::kMaxRvdsBytes));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok);
    load(makeOpRegion(2, 0, kAsle, 0x7a600000, NG::Vbt::kMaxRvdsBytes + 1));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::RvdaRange);
    load(makeOpRegion(2, 1, kAsle, UINT64_MAX - kAsls + 1, 0x100));         // relative overflow
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::RvdaRange);
    load(makeOpRegion(2, 0, kAsle, UINT64_MAX - 0xff, 0x100));              // range overflow
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::RvdaRange);
    load(makeOpRegion(2, 0, kAsle, UINT64_MAX - 0x100, 0x100));
    CHECK(NG::planOpRegion(kAsls, h, rvd, plan) == OS::Ok);
    ++tests;
}

void pureVbtVerdicts() {
    uint32_t n = 0;
    auto v = makeVbt(0x200);
    CHECK(NG::validateVbt(v.data(), 0x200, n) == VS::Ok && n == 0x200);
    CHECK(NG::validateVbt(v.data(), 0x1000, n) == VS::Ok && n == 0x200);
    CHECK(NG::validateVbt(v.data(), 0x1ff, n) == VS::SizeOverflow && n == 0);
    CHECK(NG::validateVbt(v.data(), 47, n) == VS::Truncated && NG::validateVbt(nullptr, 64, n) == VS::Truncated);
    auto bad = v; bad[1] = 'X';
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::Signature);
    bad = v; le(bad, 0x1c, 0x200 - 21, 4); fixChecksum(bad, 0x200);              // BDB header 1 short
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::BdbHeaderIncomplete);
    bad = v; le(bad, 0x1c, 0x200, 4); fixChecksum(bad, 0x200);
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::BdbHeaderIncomplete);
    bad = v; le(bad, 48 + 0x14, 0x200 - 48 + 1, 2); fixChecksum(bad, 0x200);       // bdb_size 1 over
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::BdbIncomplete);
    bad = v; le(bad, 48 + 0x14, 0x200 - 48, 2); fixChecksum(bad, 0x200);           // exactly fits
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::Ok);
    bad = v; bad[0x1a] ^= 1;
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::Checksum && !NG::linuxRejects(VS::Checksum));
    bad = v; bad[48 + 14] = 'x'; fixChecksum(bad, 0x200);
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::BdbSignature);
    bad = v; bad[48 + 15] = '\0'; fixChecksum(bad, 0x200);                          // 16th byte not judged
    CHECK(NG::validateVbt(bad.data(), 0x200, n) == VS::Ok);
    // Linux-valid but unsafe: vbt_size 46 < header, BDB at 0 with bdb_size 16.
    Bytes tiny(48, 0);
    std::memcpy(tiny.data(), "$VBT", 4); le(tiny, 0x14, 16, 2); le(tiny, 0x18, 46, 2);
    CHECK(NG::validateVbt(tiny.data(), 48, n) == VS::HeaderIncomplete && !NG::linuxRejects(VS::HeaderIncomplete));
    CHECK(NG::linuxRejects(VS::Signature) && NG::linuxRejects(VS::BdbIncomplete) && !NG::linuxRejects(VS::Ok));
    CHECK(NG::vbtDigest(v.data(), 0x200) != NG::vbtDigest(bad.data(), 0x200));
    CHECK(NG::vbtDigest(nullptr, 0) == 0xcbf29ce484222325ULL);
    // GSM bytes agree with the seeded GGTT geometry for every GGMS value.
    for (uint16_t ggms = 0; ggms < 4; ++ggms) {
        const uint16_t ggc = uint16_t(ggms << 6 | 0x2a00);
        uint64_t total = 0;
        const auto pte = NG::ggttTotalFromGmchControl(ggc, total);
        if (!ggms) CHECK(NG::gsmBytesFromGgc(ggc) == 0 && pte == NG::PteStatus::InvalidGgttSize);
        else CHECK(pte == NG::PteStatus::Ok && NG::gsmBytesFromGgc(ggc) == total / 4096 * 8 &&
                   NG::gsmBytesFromGgc(ggc) == (2u << 20) << (ggms - 1));
    }
    ++tests;
}

void pureCaptures() {
    const auto vbt = makeVbt(0x1200), other = makeVbt(0x1980, 9);
    {   // mailbox #4, no RVDA: nothing mapped
        Host h; h.region = makeOpRegion(2, 0, kAsle); place(h.region, vbt);
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Ok && s.source == NG::VbtSource::Mailbox4 && h.mapCalls == 0 && h.lastAlloc == 0x1200);
        CHECK(s.vbtBytes == 0x1200 && Bytes(s.vbt, s.vbt + s.vbtBytes) == vbt && s.rvdaVbt == VS::NotTried);
        CHECK(s.digest == NG::vbtDigest(vbt.data(), 0x1200) && s.asls == kAsls);
        settle(h, s, st);
    }
    for (int minor = 0; minor < 2; ++minor) {   // RVDA preferred over mailbox #4
        Host h;
        const uint64_t rvda = minor ? 0x10000 : 0x7a600000;
        h.expectRvda = minor ? kAsls + rvda : rvda;
        h.region = makeOpRegion(2, uint8_t(minor), kAsle, rvda, 0x1a00); place(h.region, vbt);
        h.rvdaBytes = other; h.rvdaBytes.resize(0x1a00, 0);
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Ok && h.mapCalls == 1 && Bytes(s.vbt, s.vbt + s.vbtBytes) == other);
        CHECK(s.source == (minor ? NG::VbtSource::RvdaRelative : NG::VbtSource::RvdaAbsolute));
        CHECK(s.rvdaVbt == VS::Ok && s.mailboxVbt == VS::NotTried);
        settle(h, s, st);
    }
    {   // Linux-invalid RVDA VBT: unmapped, then mailbox #4 as intel_opregion_setup
        Host h; h.expectRvda = 0x7a600000;
        h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 0x1a00); place(h.region, vbt);
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);   // RVDA window reads all-ones
        CHECK(st == OS::Ok && s.rvdaVbt == VS::Signature && s.source == NG::VbtSource::Mailbox4);
        CHECK(Bytes(s.vbt, s.vbt + s.vbtBytes) == vbt);
        settle(h, s, st);
    }
    {   // stricter refusal at RVDA never redirects to mailbox #4
        Host h; h.expectRvda = 0x7a600000;
        h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 0x1a00); place(h.region, vbt);
        h.rvdaBytes = other; h.rvdaBytes[0x1a] ^= 1;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::VbtRefused && s.rvdaVbt == VS::Checksum && s.mailboxVbt == VS::NotTried);
        settle(h, s, st);
    }
    {   // RVDS below a VBT header: Linux's truncated verdict without any mapping
        Host h; h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 40); place(h.region, vbt);
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Ok && h.mapCalls == 0 && s.rvdaVbt == VS::Truncated && s.source == NG::VbtSource::Mailbox4);
        settle(h, s, st);
    }
    {   // mailbox VBT window honours ASLE_EXT (0x1800) or not (0x1c00, the CHT allowance)
        const auto large = makeVbt(0x1900);
        Host h; h.region = makeOpRegion(2, 0, kAsle | NG::OpRegion::kMboxAsleExt); place(h.region, large);
        NG::OpRegionSnapshot s; OS st = runPure(h, s);
        CHECK(st == OS::NoVbt && s.mailboxVbt == VS::SizeOverflow);
        settle(h, s, st);
        Host g; g.region = makeOpRegion(2, 0, kAsle); place(g.region, large);
        st = runPure(g, s);
        CHECK(st == OS::Ok && s.vbtBytes == 0x1900);
        settle(g, s, st);
    }
    {   // nothing valid anywhere; stricter refusal in mailbox #4
        Host h; h.region = makeOpRegion(2, 0, kAsle);
        NG::OpRegionSnapshot s; OS st = runPure(h, s);
        CHECK(st == OS::NoVbt && s.mailboxVbt == VS::Signature);
        settle(h, s, st);
        auto broken = vbt; broken[100] ^= 1;
        Host g; g.region = makeOpRegion(2, 1, kAsle); place(g.region, broken);
        st = runPure(g, s);
        CHECK(st == OS::VbtRefused && s.mailboxVbt == VS::Checksum);
        settle(g, s, st);
    }
    ++tests;
}

void pureCoherenceAndFaults() {
    const auto vbt = makeVbt(0x1200), other = makeVbt(0x1980, 9);
    auto mailboxHost = [&](Host &h) { h.region = makeOpRegion(2, 0, kAsle); place(h.region, vbt); };
    struct Case { int after; size_t at; };
    // Region copies: 1 header, 2 rvd, 3 VBT head, 4 VBT body | pass two from 5.
    for (Case c : {Case{4, 0x20}, Case{4, 0x400 + 0x900}, Case{4, NG::OpRegion::kRvdaOffset + 2},
                   Case{3, 0x400 + 5}}) {
        Host h; mailboxHost(h); h.mutateRegionAfter = c.after; h.mutateAt = c.at;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Incoherent);
        settle(h, s, st);
    }
    {   // RVDA bytes moving between passes
        Host h; h.expectRvda = 0x7a600000;
        h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 0x1a00); h.rvdaBytes = other;
        h.mutateRvdaAfter = 2; h.mutateAt = 0x1000;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Incoherent && h.unmapCalls == 1);
        settle(h, s, st);
    }
    {   Host h; mailboxHost(h); h.aslsAgain = kAsls + 0x1000;
        NG::OpRegionSnapshot s; OS st = runPure(h, s); CHECK(st == OS::Incoherent); settle(h, s, st);
        Host g; mailboxHost(g); g.failObserve = true;
        st = runPure(g, s); CHECK(st == OS::Unavailable); settle(g, s, st);
    }
    for (int at : {0, 1, 2, 3, 4, 5, 6}) {
        Host h; mailboxHost(h); h.failRegionCopy = at;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Unavailable);
        settle(h, s, st);
    }
    {   Host h; mailboxHost(h); h.failAlloc = true;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s); CHECK(st == OS::Unavailable); settle(h, s, st);
    }
    {   Host h; h.expectRvda = 0x7a600000; h.failMap = true;
        h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 0x1a00); place(h.region, vbt);
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Unavailable && h.unmapCalls == 0);   // map failure never falls back
        settle(h, s, st);
    }
    for (bool selected : {true, false}) {   // unmap failure after selection or on fallback
        Host h; h.expectRvda = 0x7a600000; h.failUnmap = true;
        h.region = makeOpRegion(2, 0, kAsle, 0x7a600000, 0x1a00); place(h.region, vbt);
        if (selected) h.rvdaBytes = other;
        NG::OpRegionSnapshot s; const OS st = runPure(h, s);
        CHECK(st == OS::Uncertain && h.unmapCalls == 1);
        settle(h, s, st);
    }
    {   // contract misuse and ASLS admission happen before any copy
        Host h; mailboxHost(h);
        NG::OpRegionSnapshot s;
        NG::FirmwareWindow shortWindow {&h, hostRegion, 4096};
        CHECK(NG::captureOpRegion(kAsls, shortWindow, platformOf(h), s) == OS::Invalid);
        auto p = platformOf(h); p.observeAsls = nullptr;
        NG::FirmwareWindow w {&h, hostRegion, NG::OpRegion::kBytes};
        CHECK(NG::captureOpRegion(kAsls, w, p, s) == OS::Invalid);
        CHECK(NG::captureOpRegion(0, w, platformOf(h), s) == OS::NotPresent && h.regionCopies == 0);
        settle(h, s, OS::Invalid);
    }
    ++tests;
}

// ---- IOKit adapter through the real physical owner ------------------------
struct World {
    IOPCIDevice *device {};
    MellowGlkPhysicalOwner *owner {};
    uint64_t epoch {};
    uint8_t *ram {};
};
World bringUp(const Bytes &region, uint32_t asls = kAsls, uint64_t registry = 0x1000002a5) {
    OpRegionShim::clearRam();
    OpRegionShim::state = {};
    World w;
    auto *d = w.device = new IOPCIDevice;
    d->registryId = registry;
    d->put32(0x00, 0x31858086); d->put16(0x04, 0x0006); d->put16(0x06, 0x0010);
    d->put32(0x08, 0x03000006); d->put32(0x2c, 0x3e7717aa); d->config[0x34] = 0x40;
    d->put16(0x40, 0x0001); d->put16(0x44, 0x0000);                  // PM capability, D0
    d->put16(0x50, 0x02c0); d->put32(0x5c, 0x7b000001); d->put32(0xfc, asls);
    uint8_t *host = OpRegionShim::addRam(kRamBase, kRamBytes);
    if (asls >= kRamBase && asls - kRamBase + region.size() <= kRamBytes) {
        w.ram = host + (asls - kRamBase);
        std::memcpy(w.ram, region.data(), region.size());
    }
    w.owner = new MellowGlkPhysicalOwner;
    CHECK(w.owner->start(d));
    CHECK(w.owner->setPowerState(MellowGlkPhysicalOwner::PowerOn, w.owner) == kIOPMAckImplied);
    w.epoch = w.owner->snapshot().liveEpoch;
    CHECK(w.epoch && d->busLeadCalls == 0);
    return w;
}
void tearDown(World &w, bool quarantined = false) {
    w.owner->stop(w.device);
    CHECK(!w.device->isOpen());
    w.owner->release();
    w.device->release();
    auto &s = OpRegionShim::state;
    CHECK(s.blocks.empty() && s.releasedWhileMapped == 0);
    if (!quarantined) CHECK(s.liveMaps == 0 && s.liveDescriptors == 0 && s.mappedMaps == 0);
    ++tests;
}
void expectOpRegionMapping(size_t at, uint64_t address, uint64_t length) {
    auto &s = OpRegionShim::state;
    CHECK(s.descriptors.size() > at && s.mappings.size() > at);
    const auto &d = s.descriptors[at];
    CHECK(d.address == address && d.length == length && d.task == TASK_NULL);
    CHECK(d.options == (kIODirectionOut | kIOMemoryMapperNone));
    const auto &m = s.mappings[at];
    CHECK(m.task == kernel_task && m.at == 0 && m.offset == 0 && m.length == length);
    CHECK(m.options == (kIOMapAnywhere | kIOMapReadOnly | kIOMapDefaultCache));
}
struct Seen {
    Bytes bytes;
    GlkOpRegionInfo info;
    GlkOpRegionIOKit *adapter {};
    MellowGlkPhysicalOwner *owner {};
    uint64_t epoch {};
    GS nestedCapture {GS::Ok}, nestedDiscard {GS::Ok}, nestedInfo {GS::Invalid};
    int calls {};
};
void collect(void *c, const uint8_t *p, uint32_t n, const GlkOpRegionInfo &info) {
    auto *seen = static_cast<Seen *>(c);
    seen->bytes.assign(p, p + n);
    seen->info = info;
    ++seen->calls;
    if (seen->adapter) {
        GlkOpRegionInfo nested;
        seen->nestedCapture = seen->adapter->capture(*seen->owner, seen->epoch);
        seen->nestedDiscard = seen->adapter->discard(*seen->owner);
        seen->nestedInfo = seen->adapter->info(*seen->owner, nested);
    }
}
Bytes published(GlkOpRegionIOKit &a, World &w, GS expect = GS::Ok) {
    Seen seen;
    CHECK(a.withVbt(*w.owner, w.epoch, collect, &seen) == expect);
    return seen.bytes;
}

void adapterMailbox() {
    auto region = makeOpRegion(2, 0, kAsle);
    const auto vbt = makeVbt(0x1200);
    place(region, vbt);
    World w = bringUp(region);
    {
        GlkOpRegionIOKit a;
        GlkOwnerStatus os {GlkOwnerStatus::Invalid};
        CHECK(a.capture(*w.owner, w.epoch, &os) == GS::Ok && os == GlkOwnerStatus::Ok);
        auto &s = OpRegionShim::state;
        CHECK(s.descriptors.size() == 1);
        expectOpRegionMapping(0, kAsls, NG::OpRegion::kBytes);
        CHECK(s.liveMaps == 0 && s.liveDescriptors == 0 && s.unmapCalls == 1);
        CHECK(s.blocks.size() == 1 && s.blocks.begin()->second == 0x1200);
        CHECK(w.device->configWrites == 0 && w.device->busLeadCalls == 0);
        CHECK(w.device->reads[0xfc] == 3 && w.device->reads[0x50] == 2 && w.device->reads[0x5c] == 2);
        CHECK(published(a, w) == vbt);
        GlkOpRegionInfo i;
        CHECK(a.info(*w.owner, i) == GS::Ok && i.published && i.epoch == w.epoch && i.vbtBytes == 0x1200);
        CHECK(i.source == NG::VbtSource::Mailbox4 && i.header.major == 2 && i.header.minor == 0);
        CHECK(i.igd.asls == kAsls && i.igd.ggc == 0x02c0 && i.igd.ggms == 3 && i.igd.gms == 2);
        CHECK(i.igd.gsmBytes == (8u << 20) && i.igd.bdsm == 0x7b000001 && i.igd.bdsmBase == 0x7b000000);
        CHECK(i.digest == NG::vbtDigest(vbt.data(), 0x1200) && i.generation == 1 && i.captures == 1);
        CHECK(i.registryId == 0x1000002a5 && i.last == GS::Ok && !i.quarantined);
        Bytes small(0x11ff, 0xcc), exact(0x1200);
        uint32_t n = 7;
        CHECK(a.copyVbt(*w.owner, w.epoch, small.data(), 0x11ff, n) == GS::Invalid && n == 0x1200);
        for (uint8_t b : small) CHECK(b == 0xcc);
        CHECK(a.copyVbt(*w.owner, w.epoch, exact.data(), 0x1200, n) == GS::Ok && exact == vbt);
        CHECK(a.copyVbt(*w.owner, w.epoch, nullptr, 0x1200, n) == GS::Invalid && n == 0);
        CHECK(a.withVbt(*w.owner, w.epoch, nullptr, nullptr) == GS::Invalid);
        // Consumers run inside the gate and cannot change what they see.
        Seen nested; nested.adapter = &a; nested.owner = w.owner; nested.epoch = w.epoch;
        CHECK(a.withVbt(*w.owner, w.epoch, collect, &nested) == GS::Ok && nested.calls == 1);
        CHECK(nested.nestedCapture == GS::Busy && nested.nestedDiscard == GS::Busy && nested.nestedInfo == GS::Ok);
        CHECK(published(a, w) == vbt && s.descriptors.size() == 1);
    }
    tearDown(w);   // destructor released the owned copy
}

void adapterRvda() {
    const auto mailbox = makeVbt(0x1200), firmware = makeVbt(0x1980, 9);
    for (int minor = 0; minor < 3; ++minor) {
        // 0: 2.0 absolute, 1: 2.1 relative, 2: 2.0 pointing at unbacked memory
        const uint64_t rvda = minor == 1 ? 0x2000 : minor == 0 ? 0x7a5c4800 : 0x9f000000;
        const uint64_t physical = minor == 1 ? kAsls + rvda : rvda;
        auto region = makeOpRegion(2, minor == 1 ? 1 : 0, kAsle, rvda, 0x1a00);
        place(region, mailbox);
        World w = bringUp(region);
        uint8_t *ramBase = w.ram - (kAsls - kRamBase);
        if (minor < 2) std::memcpy(ramBase + (physical - kRamBase), firmware.data(), firmware.size());
        {
            GlkOpRegionIOKit a;
            CHECK(a.capture(*w.owner, w.epoch) == GS::Ok);
            auto &s = OpRegionShim::state;
            CHECK(s.descriptors.size() == 2);
            expectOpRegionMapping(0, kAsls, NG::OpRegion::kBytes);
            expectOpRegionMapping(1, physical, 0x1a00);
            CHECK(s.liveMaps == 0 && s.liveDescriptors == 0 && s.unmapCalls == 2);
            GlkOpRegionInfo i;
            CHECK(a.info(*w.owner, i) == GS::Ok);
            if (minor < 2) {
                CHECK(published(a, w) == firmware && i.rvdaVbt == VS::Ok);
                CHECK(i.source == (minor ? NG::VbtSource::RvdaRelative : NG::VbtSource::RvdaAbsolute));
            } else {
                CHECK(published(a, w) == mailbox && i.rvdaVbt == VS::Signature && i.source == NG::VbtSource::Mailbox4);
            }
        }
        tearDown(w);
    }
    {   // version outside 2.0/2.1: refused after the header, nothing else mapped
        auto region = makeOpRegion(2, 2, kAsle, 0x2000, 0x1a00);
        place(region, mailbox);
        World w = bringUp(region);
        {
            GlkOpRegionIOKit a;
            CHECK(a.capture(*w.owner, w.epoch) == GS::Firmware);
            GlkOpRegionInfo i;
            CHECK(a.info(*w.owner, i) == GS::Ok && i.firmware == OS::Version && !i.published);
            CHECK(OpRegionShim::state.descriptors.size() == 1);
            CHECK(published(a, w, GS::NotCaptured).empty());
        }
        tearDown(w);
    }
}

unsigned flipAfter;
void flipSubsystem(IOPCIDevice *d, uint8_t offset) {
    if (offset == 0x2c && d->reads[0x2c] == flipAfter) d->put32(0x2c, 0xffffffff);
}

void adapterBinding() {
    auto region = makeOpRegion(2, 0, kAsle);
    const auto vbt = makeVbt(0x1200);
    place(region, vbt);
    World w = bringUp(region);
    {
        GlkOpRegionIOKit a;
        GlkOwnerStatus os {};
        // No live lease, no capture: the owner refuses before any mapping.
        CHECK(a.capture(*w.owner, w.epoch + 1, &os) == GS::Owner && os == GlkOwnerStatus::StaleEpoch);
        CHECK(a.capture(*w.owner, 0, &os) == GS::Owner && os == GlkOwnerStatus::StaleEpoch);
        CHECK(OpRegionShim::state.descriptors.empty());
        // Identity lost inside the lease (after the owner's own check).
        flipAfter = w.device->reads[0x2c] + 1;
        OpRegionShim::state.onConfigRead = flipSubsystem;
        CHECK(a.capture(*w.owner, w.epoch, &os) == GS::Ownership && os == GlkOwnerStatus::Ok);
        OpRegionShim::state.onConfigRead = nullptr;
        w.device->put32(0x2c, 0x3e7717aa);
        CHECK(OpRegionShim::state.descriptors.empty());
        CHECK(a.capture(*w.owner, w.epoch) == GS::Ok);
        // A second owner on another registry entry is refused once bound.
        auto *other = new IOPCIDevice;
        std::memcpy(other->config, w.device->config, sizeof other->config);
        other->registryId = 0x1000002b0;
        auto *second = new MellowGlkPhysicalOwner;
        CHECK(second->start(other) && second->setPowerState(1, second) == kIOPMAckImplied);
        const uint64_t e2 = second->snapshot().liveEpoch;
        CHECK(a.capture(*second, e2) == GS::Ownership);
        CHECK(a.withVbt(*second, e2, collect, nullptr) == GS::Ownership);
        CHECK(a.discard(*second) == GS::Ownership);
        second->stop(other); second->release(); other->release();
        CHECK(published(a, w) == vbt);
    }
    tearDown(w);
}

void mutateFirmwareVbt(World &w) {
    w.ram[NG::OpRegion::kVbtOffset + 0x300] ^= 0x11;
    Bytes copy(w.ram + NG::OpRegion::kVbtOffset, w.ram + NG::OpRegion::kVbtOffset + 0x1200);
    fixChecksum(copy, 0x1200);
    std::memcpy(w.ram + NG::OpRegion::kVbtOffset, copy.data(), copy.size());
}

void adapterEpochs() {
    auto region = makeOpRegion(2, 1, kAsle);
    const auto vbt = makeVbt(0x1200);
    place(region, vbt);
    World w = bringUp(region);
    {
        GlkOpRegionIOKit a;
        GlkOwnerStatus os {};
        CHECK(a.capture(*w.owner, w.epoch) == GS::Ok);
        const uint64_t first = w.epoch;
        CHECK(w.owner->setPowerState(MellowGlkPhysicalOwner::PowerOff, w.owner) == kIOPMAckImplied);
        CHECK(a.withVbt(*w.owner, first, collect, nullptr, &os) == GS::Owner && os == GlkOwnerStatus::PowerDown);
        CHECK(a.capture(*w.owner, first, &os) == GS::Owner && os == GlkOwnerStatus::PowerDown);
        CHECK(w.owner->setPowerState(MellowGlkPhysicalOwner::PowerOn, w.owner) == kIOPMAckImplied);
        w.epoch = w.owner->snapshot().liveEpoch;
        CHECK(w.epoch && w.epoch != first);
        uint32_t n = 0;
        Bytes out(0x2000);
        CHECK(published(a, w, GS::StaleEpoch).empty());
        CHECK(a.copyVbt(*w.owner, w.epoch, out.data(), 0x2000, n) == GS::StaleEpoch);
        // Recapture in the new epoch: identical bytes rebind the same buffer.
        CHECK(a.capture(*w.owner, w.epoch) == GS::Ok && published(a, w) == vbt);
        GlkOpRegionInfo i;
        CHECK(a.info(*w.owner, i) == GS::Ok && i.generation == 2 && i.captures == 2 && i.epoch == w.epoch);
        CHECK(OpRegionShim::state.blocks.size() == 1);
        // Firmware bytes that change within a boot are never published.
        mutateFirmwareVbt(w);
        CHECK(a.capture(*w.owner, w.epoch) == GS::Changed);
        CHECK(published(a, w, GS::NotCaptured).empty() && OpRegionShim::state.blocks.empty());
        CHECK(a.info(*w.owner, i) == GS::Ok && !i.published && i.last == GS::Changed && i.vbtBytes == 0);
        CHECK(a.capture(*w.owner, w.epoch) == GS::Ok && published(a, w).size() == 0x1200);
        CHECK(a.discard(*w.owner) == GS::Ok && OpRegionShim::state.blocks.empty());
        CHECK(published(a, w, GS::NotCaptured).empty() && a.discard(*w.owner) == GS::Ok);
    }
    tearDown(w);
}

unsigned aslsReads;
void moveAsls(IOPCIDevice *d, uint8_t offset) {
    if (offset == 0xfc && d->reads[0xfc] == aslsReads) d->put32(0xfc, kAsls + 0x1000);
}
void moveGgc(IOPCIDevice *d, uint8_t offset) {
    if (offset == 0x50 && d->reads[0x50] == 1) d->put16(0x50, 0x0280);
}
World *loadWorld;
uint64_t loadAt;
void mutateOnLoad(uint64_t count) {
    if (count == loadAt) loadWorld->ram[NG::OpRegion::kVbtOffset + 0x700] ^= 0x40;
}

void adapterFaults() {
    auto region = makeOpRegion(2, 0, kAsle);
    place(region, makeVbt(0x1200));
    enum Fault { Descriptor, Mapping, Length, Physical, ReadOnly, Offset, Malloc, AslsZero, AslsOnes,
                 GgcOnes, AslsMoves, AslsMovesLate, GgcMoves, BytesMove };
    for (int f = Descriptor; f <= BytesMove; ++f) {
        const uint32_t asls = f == AslsZero ? 0 : f == AslsOnes ? UINT32_MAX : kAsls;
        World w = bringUp(region, asls);
        auto &s = OpRegionShim::state;
        if (f == Descriptor) s.faults.descriptorAt = 0;
        if (f == Mapping) s.faults.mappingAt = 0;
        if (f == Length) s.faults.lengthSkew = -1;
        if (f == Physical) s.faults.physicalSkew = 0x1000;
        if (f == ReadOnly) s.faults.dropReadOnly = true;
        if (f == Offset) s.faults.misalignVirtual = true;
        if (f == Malloc) s.faults.mallocAt = 0;
        if (f == GgcOnes) w.device->put16(0x50, 0xffff);
        if (f == AslsMoves || f == AslsMovesLate) { aslsReads = f == AslsMoves ? 1 : 2; s.onConfigRead = moveAsls; }
        if (f == GgcMoves) s.onConfigRead = moveGgc;
        if (f == BytesMove) { loadWorld = &w; loadAt = 256 + 12 + 48 + 0x1200; s.onLoad = mutateOnLoad; }
        {
            GlkOpRegionIOKit a;
            CHECK(a.capture(*w.owner, w.epoch) == GS::Firmware);
            s.onConfigRead = nullptr; s.onLoad = nullptr;
            GlkOpRegionInfo i;
            CHECK(a.info(*w.owner, i) == GS::Ok && !i.published && i.last == GS::Firmware);
            const OS expect = f == AslsZero ? OS::NotPresent :
                (f == AslsMoves || f == AslsMovesLate || f == GgcMoves || f == BytesMove) ? OS::Incoherent :
                OS::Unavailable;
            CHECK(i.firmware == expect);
            if (f == AslsZero || f == AslsOnes || f == GgcOnes) CHECK(s.descriptors.empty());
            CHECK(s.liveMaps == 0 && s.liveDescriptors == 0 && s.blocks.empty());
            CHECK(published(a, w, GS::NotCaptured).empty());
        }
        tearDown(w);
    }
}

void adapterQuarantine() {
    const auto vbt = makeVbt(0x1200);
    for (int which = 0; which < 2; ++which) {
        // 0: the OpRegion unmap fails; 1: the RVDA unmap (first unmap) fails.
        auto region = which ? makeOpRegion(2, 1, kAsle, 0x2000, 0x1a00) : makeOpRegion(2, 0, kAsle);
        place(region, vbt);
        World w = bringUp(region);
        if (which) std::memcpy(w.ram + 0x2000, makeVbt(0x1980, 9).data(), 0x1980);
        auto &s = OpRegionShim::state;
        s.faults.unmapAt = 0;
        {
            GlkOpRegionIOKit a;
            CHECK(a.capture(*w.owner, w.epoch) == GS::Quarantined);
            // The failed window's map and descriptor stay retained forever.
            CHECK(s.liveMaps == 1 && s.liveDescriptors == 1 && s.mappedMaps == 1 && s.blocks.empty());
            const size_t before = s.descriptors.size();
            CHECK(a.capture(*w.owner, w.epoch) == GS::Quarantined && s.descriptors.size() == before);
            GlkOpRegionInfo i;
            CHECK(a.info(*w.owner, i) == GS::Ok && i.quarantined && !i.published);
            if (which) CHECK(i.firmware == OS::Uncertain);
            CHECK(published(a, w, GS::NotCaptured).empty());
        }
        CHECK(s.liveMaps == 1 && s.liveDescriptors == 1);   // the destructor kept them too
        tearDown(w, true);
    }
}
} // namespace

int main() {
    pureAdmission();
    purePlan();
    pureVbtVerdicts();
    pureCaptures();
    pureCoherenceAndFaults();
    adapterMailbox();
    adapterRvda();
    adapterBinding();
    adapterEpochs();
    adapterFaults();
    adapterQuarantine();
    OpRegionShim::clearRam();
    std::printf("native_glk_opregion_tests: %u groups passed (HOSTED INJECTED MODEL, not hardware)\n", tests);
    return 0;
}

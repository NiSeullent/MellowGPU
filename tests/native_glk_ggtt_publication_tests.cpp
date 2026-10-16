// Portable GGTT publication core over an injected transport.
// HOSTED INJECTED TRANSPORT: the "GSM" is a host byte array and the TLB
// controls are a scripted model. No BAR, MMIO, forcewake, TLB or GPU is
// touched or modeled as hardware; passing is not evidence of GPU behaviour.
#include "../Drivers/NativeGlk/GgttPublication.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace Mellow::NativeGlk;
namespace L = Mellow::NativeGlk::Gen9LpGgtt;

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); std::abort(); } } while (0)

namespace {
constexpr uint64_t MiB = uint64_t(1) << 20, GiB = uint64_t(1) << 30, Page = 4096;

struct Ev { char op; uint64_t at, value; };

// Scripted device model. 'r'/'w' entry read/write, 'Q'/'W' register read/write,
// 'F'/'S'/'L' fences, 'A'/'R' GT acquire/release, 'P' pause.
struct Fake {
    std::vector<uint8_t> gsm;
    std::vector<Ev> log;
    std::map<uint32_t, unsigned> pending;
    std::set<uint32_t> stuck;
    unsigned clearAfter {2};
    uint64_t clock {1000000};
    bool frozen {};
    long regressAtCall {-1};
    unsigned nowCalls {};
    long failEntryWrite {-1}, failEntryRead {-1}, failRegWrite {-1}, failRegRead {-1};
    long dropIndex {-1};
    unsigned entryWrites {}, entryReads {}, regWrites {}, regReads {};
    bool gtAvailable {true}, gtReleaseOk {true};
    unsigned acquires {}, releases {}, refusedAcquires {};
    uint64_t heldToken {};
    bool awakeKnown {true};
    uint32_t awakeMask {15};
    std::vector<uint32_t> pauses;

    explicit Fake(uint64_t entries) : gsm(entries * 8, 0) {}
    uint64_t entry(uint64_t index) const {
        uint8_t image[8];
        std::memcpy(image, &gsm[index * 8], 8);
        return loadLe64(image);
    }
    void seed(uint64_t index, uint64_t value) {
        uint8_t image[8];
        storeLe64(value, image);
        std::memcpy(&gsm[index * 8], image, 8);
    }
    unsigned count(char op) const {
        unsigned n = 0;
        for (const Ev &e : log) n += e.op == op;
        return n;
    }
};

bool writeEntry(void *c, uint64_t index, const uint8_t (&le)[8]) {
    Fake &f = *static_cast<Fake *>(c);
    if (long(f.entryWrites++) == f.failEntryWrite || index * 8 + 8 > f.gsm.size()) return false;
    f.log.push_back({'w', index, loadLe64(le)});
    if (long(index) != f.dropIndex) std::memcpy(&f.gsm[index * 8], le, 8);
    return true;
}
bool readEntry(void *c, uint64_t index, uint8_t (&le)[8]) {
    Fake &f = *static_cast<Fake *>(c);
    if (long(f.entryReads++) == f.failEntryRead || index * 8 + 8 > f.gsm.size()) return false;
    std::memcpy(le, &f.gsm[index * 8], 8);
    f.log.push_back({'r', index, loadLe64(le)});
    return true;
}
bool writeRegister(void *c, uint32_t offset, uint32_t value) {
    Fake &f = *static_cast<Fake *>(c);
    if (long(f.regWrites++) == f.failRegWrite) return false;
    f.log.push_back({'W', offset, value});
    if (value == L::kTlbInvalidateBit) f.pending[offset] = f.clearAfter;
    return true;
}
bool readRegister(void *c, uint32_t offset, uint32_t &value) {
    Fake &f = *static_cast<Fake *>(c);
    if (long(f.regReads++) == f.failRegRead) return false;
    unsigned &left = f.pending[offset];
    value = f.stuck.count(offset) || left ? L::kTlbInvalidateBit : 0;
    if (left) --left;
    f.log.push_back({'Q', offset, value});
    return true;
}
void fence(void *c, GgttFence kind) {
    static_cast<Fake *>(c)->log.push_back({kind == GgttFence::Full ? 'F' : kind == GgttFence::Store ? 'S' : 'L', 0, 0});
}
bool now(void *c, uint64_t &ns) {
    Fake &f = *static_cast<Fake *>(c);
    if (long(++f.nowCalls) == f.regressAtCall) f.clock -= 1000000;
    ns = f.clock;
    return true;
}
void pause(void *c, uint32_t us) {
    Fake &f = *static_cast<Fake *>(c);
    f.pauses.push_back(us);
    f.log.push_back({'P', 0, us});
    if (!f.frozen) f.clock += uint64_t(us) * 1000;
}
bool acquire(void *c, uint64_t &token) {
    Fake &f = *static_cast<Fake *>(c);
    if (!f.gtAvailable) { ++f.refusedAcquires; return false; }
    ++f.acquires;
    f.log.push_back({'A', 0, 0});
    token = f.heldToken = 0x51 + f.acquires;
    return true;
}
bool release(void *c, uint64_t token) {
    Fake &f = *static_cast<Fake *>(c);
    ++f.releases;
    f.log.push_back({'R', 0, token});
    return f.gtReleaseOk && token == f.heldToken;
}
GgttIo io(Fake &f) { return {&f, writeEntry, readEntry, writeRegister, readRegister, fence, now, pause}; }
bool awakeEngines(void *c, uint64_t token, uint32_t &mask) {
    auto &f = *static_cast<Fake *>(c);
    CHECK(token == f.heldToken && f.acquires == f.releases + 1);
    if (!f.awakeKnown) return false;
    mask = f.awakeMask;
    return true;
}
GgttGtAccess gt(Fake &f) { return {&f, acquire, release, awakeEngines}; }

// GGMS = 1: 2 MiB GSM, 1 GiB GGTT, keeps host memory small.
GgttGeometry geometry1G() {
    GgttGeometry g {};
    CHECK(deriveGgttGeometry(uint16_t(1u << 6), 16 * MiB, g) == GgttStatus::Ok);
    return g;
}
const GgttRetainedRange kRetained[] = {
    {64 * MiB, 8 * MiB, GgttRangeKind::Display},  // boot framebuffer binding
    {80 * MiB, 1 * MiB, GgttRangeKind::Stolen},
};
GgttHandover handover(uint64_t total = GiB) {
    GgttHandover h {};
    h.registryId = 0x100000205; h.epoch = 7; h.ggttTotal = total;
    h.pool = {64 * MiB, 64 * MiB};
    h.retained = kRetained; h.retainedCount = 2; h.complete = true;
    return h;
}

void testGeometryAndBar() {
    for (uint16_t ggms = 1; ggms <= 3; ++ggms) {
        GgttGeometry g {};
        // GMS (stolen size) and lock bits must not influence the GSM size.
        CHECK(deriveGgttGeometry(uint16_t((0xa5u << 8) | (ggms << 6) | 0x3), 16 * MiB, g) == GgttStatus::Ok);
        CHECK(g.total == (GiB << (ggms - 1)));
        CHECK(g.gsmBytes == (MiB << ggms));
        CHECK(g.entryCount == g.total / Page);
        CHECK(g.gsmBarOffset == 8 * MiB && g.gsmBarOffset + g.gsmBytes <= 16 * MiB);
        CHECK(g.firstUsable == Page && g.endUsable == g.total - Page);
    }
    GgttGeometry g {};
    CHECK(deriveGgttGeometry(0x0200, 16 * MiB, g) == GgttStatus::Unavailable);
    CHECK(deriveGgttGeometry(0x00c0, 32 * MiB, g) == GgttStatus::InvalidArgument);
    CHECK(deriveGgttGeometry(0x00c0, 4 * MiB, g) == GgttStatus::InvalidArgument);
    CHECK(g.total == 0);

    uint64_t base = 0;
    CHECK(decodeGttMmadrBar(0xa0000004, 0, 0x0007, base) == GgttStatus::Ok && base == 0xa0000000);
    CHECK(decodeGttMmadrBar(0x01000004, 0x60, 0x0002, base) == GgttStatus::Ok && base == 0x6001000000);
    base = 0x1234;
    CHECK(decodeGttMmadrBar(0xa000000c, 0, 0x7, base) == GgttStatus::InvalidArgument); // prefetchable
    CHECK(decodeGttMmadrBar(0xa0000000, 0, 0x7, base) == GgttStatus::InvalidArgument); // 32-bit
    CHECK(decodeGttMmadrBar(0xa0000002, 0, 0x7, base) == GgttStatus::InvalidArgument); // below 1 MiB type
    CHECK(decodeGttMmadrBar(0x0000f001, 0, 0x7, base) == GgttStatus::InvalidArgument); // I/O space
    CHECK(decodeGttMmadrBar(0xffffffff, 0xffffffff, 0xffff, base) == GgttStatus::InvalidArgument);
    CHECK(decodeGttMmadrBar(0xa0800004, 0, 0x7, base) == GgttStatus::InvalidArgument); // not 16 MiB aligned
    CHECK(decodeGttMmadrBar(0x00000004, 0, 0x7, base) == GgttStatus::InvalidArgument); // zero base
    CHECK(decodeGttMmadrBar(0xa0000004, 0, 0x0005, base) == GgttStatus::Unavailable);  // decode off
    CHECK(base == 0x1234);
    // Pinned constants: the four Gen8 engine controls in for_each_engine order.
    CHECK(L::kEngineTlbControls[0] == 0x4260 && L::kEngineTlbControls[1] == 0x426c &&
          L::kEngineTlbControls[2] == 0x4264 && L::kEngineTlbControls[3] == 0x4270);
    CHECK(L::kTlbFastTimeoutUs == 100 && L::kTlbSlowTimeoutMs == 4 && L::kGsmBarOffset == 8 * MiB);
}

void testInitializeRefusals() {
    const GgttGeometry g = geometry1G();
    Fake f(g.entryCount);
    {
        GgttPublisher p;
        GgttHandover h = handover(); h.complete = false;
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::Unavailable);
        h = handover(2 * GiB);
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::Ownership);
        h = handover(); h.pool = {0, 4 * MiB};               // includes the pin-bias page
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        h = handover(); h.pool = {GiB - 4 * MiB, 4 * MiB};   // includes the top guard page
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        h = handover(); h.pool = {64 * MiB + 512, 4 * MiB};
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        const GgttRetainedRange overlapping[] = {{64 * MiB, 2 * MiB, GgttRangeKind::Display},
                                                 {65 * MiB, 1 * MiB, GgttRangeKind::Firmware}};
        h = handover(); h.retained = overlapping;
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::Overlap);
        const GgttRetainedRange beyond[] = {{GiB - Page, 2 * Page, GgttRangeKind::Platform}};
        h = handover(); h.retained = beyond; h.retainedCount = 1;
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        const GgttRetainedRange badKind[] = {{8 * MiB, Page, GgttRangeKind(0)}};
        h = handover(); h.retained = badKind; h.retainedCount = 1;
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        h = handover(); h.retainedCount = GgttPublisher::MaxRetained + 1;
        CHECK(p.initialize(g, h, io(f), gt(f)) == GgttStatus::InvalidArgument);
        GgttIo partial = io(f); partial.pause = nullptr;
        CHECK(p.initialize(g, handover(), partial, gt(f)) == GgttStatus::Unavailable);
        GgttGtAccess noGt = gt(f); noGt.release = nullptr;
        CHECK(p.initialize(g, handover(), io(f), noGt) == GgttStatus::Unavailable);
        GgttGeometry forged = g; forged.gsmBytes *= 2;
        CHECK(p.initialize(forged, handover(), io(f), gt(f)) == GgttStatus::InvalidArgument);
        forged = g; forged.firstUsable = 0;
        CHECK(p.initialize(forged, handover(), io(f), gt(f)) == GgttStatus::InvalidArgument);
        CHECK(!p.initialized());
        GgttExtent e {};
        CHECK(p.reserve(1, Page, e) == GgttStatus::Unavailable);
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Busy);
    }
    CHECK(f.log.empty() && f.acquires == 0);
}

void testAwakeEngineAuthority() {
    const auto g = geometry1G();
    {
        Fake f(g.entryCount); GgttPublisher p;
        auto absent = gt(f); absent.awakeEngines = nullptr;
        CHECK(p.initialize(g, handover(), io(f), absent) == GgttStatus::Unavailable);
        CHECK(f.log.empty());
    }
    for (uint32_t mask : {0U, 1U, 2U, 4U, 8U, 5U, 15U}) {
        Fake f(g.entryCount); GgttPublisher p;
        f.awakeMask = mask;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e; CHECK(p.reserve(7, Page, e) == GgttStatus::Ok);
        uint64_t dma = 0x12345000, saved = 0;
        CHECK(p.publish(7, e, &dma, 1, &saved, 1) == GgttStatus::Ok);
        unsigned selected = 0;
        for (unsigned i = 0; i < L::kEngineCount; ++i) selected += (mask >> i) & 1U;
        CHECK(f.regWrites == selected);
        for (const auto &event : f.log) if (event.op == 'W' || event.op == 'Q') {
            bool permitted = false;
            for (unsigned i = 0; i < L::kEngineCount; ++i)
                permitted |= ((mask >> i) & 1U) && event.at == L::kEngineTlbControls[i];
            CHECK(permitted);
        }
        // Re-observe on the next native lease; a prior awake snapshot cannot
        // authorize a later inverse operation.
        f.log.clear(); f.regWrites = 0; f.awakeMask = mask ^ 15U;
        CHECK(p.unpublish(7, e, true) == GgttStatus::Ok);
        CHECK(f.regWrites == L::kEngineCount - selected);
        for (const auto &event : f.log) if (event.op == 'W' || event.op == 'Q') {
            bool permitted = false;
            for (unsigned i = 0; i < L::kEngineCount; ++i)
                permitted |= ((f.awakeMask >> i) & 1U) && event.at == L::kEngineTlbControls[i];
            CHECK(permitted);
        }
        CHECK(p.release(7, e) == GgttStatus::Ok);
    }
    {
        Fake f(g.entryCount); GgttPublisher p;
        f.awakeKnown = false; f.gtReleaseOk = false;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e; CHECK(p.reserve(7, Page, e) == GgttStatus::Ok);
        uint64_t dma = 0x12345000, saved = 0;
        CHECK(p.publish(7, e, &dma, 1, &saved, 1) == GgttStatus::TransportFailure);
        CHECK(f.entryWrites == 0 && f.regWrites == 0);
        CHECK(p.state(e) == GgttExtentState::Quarantined && p.faulted());
        CHECK(p.release(7, e) == GgttStatus::Quarantined);
    }
    for (bool invalidMask : {false, true}) {
        Fake f(g.entryCount); GgttPublisher p;
        f.awakeKnown = invalidMask;
        f.awakeMask = 16;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e; CHECK(p.reserve(7, Page, e) == GgttStatus::Ok);
        uint64_t dma = 0x12345000, saved = 0;
        CHECK(p.publish(7, e, &dma, 1, &saved, 1) == GgttStatus::Unavailable);
        CHECK(f.entryWrites == 0 && f.regWrites == 0 && f.acquires == f.releases);
        CHECK(p.state(e) == GgttExtentState::Reserved);
        CHECK(p.release(7, e) == GgttStatus::Ok);
    }
}

void testReservations() {
    const GgttGeometry g = geometry1G();
    Fake f(g.entryCount);
    GgttPublisher p;
    CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
    GgttExtent a {}, b {}, c {}, d {};
    CHECK(p.reserve(1, 4 * Page, a) == GgttStatus::Ok);
    CHECK(a.start == 72 * MiB && a.bytes == 4 * Page);            // first fit after the framebuffer
    CHECK(p.reserve(1, 8 * MiB, b) == GgttStatus::Ok);
    CHECK(b.start == 81 * MiB);                                    // skips extent a and stolen range
    CHECK(p.reserveAt(2, 80 * MiB, Page, c) == GgttStatus::Overlap);
    CHECK(p.reserveAt(2, 70 * MiB, Page, c) == GgttStatus::Overlap);
    CHECK(p.reserveAt(2, 72 * MiB + Page, Page, c) == GgttStatus::Overlap);
    CHECK(p.reserveAt(2, 32 * MiB, Page, c) == GgttStatus::Ownership); // outside the pool
    CHECK(p.reserveAt(2, 127 * MiB + Page, 2 * Page, c) == GgttStatus::Ok);
    CHECK(p.reserveAt(2, 100 * MiB + 8, Page, d) == GgttStatus::InvalidArgument);
    CHECK(p.reserveAt(0, 100 * MiB, Page, d) == GgttStatus::InvalidArgument);
    CHECK(p.reserve(2, 64 * MiB, d) == GgttStatus::Capacity && d.slot == UINT32_MAX);
    CHECK(p.reserve(2, 128 * MiB, d) == GgttStatus::Capacity);
    CHECK(p.release(2, a) == GgttStatus::Ownership);                // wrong owner
    CHECK(p.release(1, a) == GgttStatus::Ok);
    CHECK(p.release(1, a) == GgttStatus::Ownership);                // stale generation
    CHECK(p.reserve(3, 4 * Page, d) == GgttStatus::Ok && d.start == 72 * MiB && d.generation != a.generation);
    CHECK(p.state(a) == GgttExtentState::Free && p.state(d) == GgttExtentState::Reserved);
    CHECK(p.shutdown() == GgttStatus::Busy);
    CHECK(p.release(3, d) == GgttStatus::Ok && p.release(1, b) == GgttStatus::Ok && p.release(2, c) == GgttStatus::Ok);
    // Slot exhaustion is a bounded capacity failure.
    GgttExtent many[GgttPublisher::MaxExtents] {};
    for (uint32_t i = 0; i < GgttPublisher::MaxExtents; ++i) CHECK(p.reserve(9, Page, many[i]) == GgttStatus::Ok);
    CHECK(p.reserve(9, Page, d) == GgttStatus::Capacity);
    for (auto &extent : many) CHECK(p.release(9, extent) == GgttStatus::Ok);
    CHECK(p.extentsInUse() == 0 && p.shutdown() == GgttStatus::Ok && !p.initialized());
    CHECK(f.log.empty());   // reservation never touches the GSM: no PTE scan as ownership proof
}

// Exact publication protocol: acquire, capture, fence, stores, fence,
// readback, four invalidation requests, four waits, release.
void testPublishSequenceAndRestore() {
    const GgttGeometry g = geometry1G();
    Fake f(g.entryCount);
    GgttPublisher p;
    CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
    GgttExtent e {};
    CHECK(p.reserve(5, 4 * Page, e) == GgttStatus::Ok);
    const uint64_t first = e.start / Page;
    const uint64_t prior[4] = {0x0000000abc000001ULL, 0, 0x0000001234567000ULL, 0x0000007ffffff001ULL};
    for (uint64_t i = 0; i < 4; ++i) f.seed(first + i, prior[i]);
    f.seed(first - 1, 0x1111000001ULL); f.seed(first + 4, 0x2222000001ULL);
    const uint64_t pages[4] = {0x1000000, 0x2000000, 0x7ffffff000ULL, 0x3000};
    uint64_t capture[4] = {};
    CHECK(p.publish(5, e, pages, 4, capture, 4) == GgttStatus::Ok);
    CHECK(p.state(e) == GgttExtentState::Published && p.captured(e) == capture);
    for (int i = 0; i < 4; ++i) {
        CHECK(capture[i] == prior[i]);
        CHECK(f.entry(first + i) == (pages[i] | 1));               // gen8_ggtt_pte_encode: addr | PRESENT
        CHECK(f.gsm[(first + i) * 8] == uint8_t((pages[i] | 1) & 0xff)); // little-endian byte 0
    }
    CHECK(f.gsm[(first + 2) * 8 + 4] == 0x7f);                     // byte 4 of 0x7ffffff001
    CHECK(f.entry(first - 1) == 0x1111000001ULL && f.entry(first + 4) == 0x2222000001ULL);
    size_t at = 0;
    auto expect = [&](char op, uint64_t where, uint64_t value, bool checkValue) {
        CHECK(at < f.log.size());
        CHECK(f.log[at].op == op && f.log[at].at == where);
        if (checkValue) CHECK(f.log[at].value == value);
        ++at;
    };
    expect('A', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) expect('r', first + i, prior[i], true);
    expect('F', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) expect('w', first + i, pages[i] | 1, true);
    expect('F', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) expect('r', first + i, pages[i] | 1, true);
    for (uint32_t control : L::kEngineTlbControls) expect('W', control, 1, true);
    for (uint32_t control : L::kEngineTlbControls) {
        expect('Q', control, 1, true); expect('P', 0, 1, true);
        expect('Q', control, 1, true); expect('P', 0, 1, true);
        expect('Q', control, 0, true);
    }
    expect('R', 0, 0, false);
    CHECK(at == f.log.size());
    for (const Ev &ev : f.log) if (ev.op == 'W') CHECK(ev.at != L::kGfxFlushControlGen6);

    CHECK(p.publish(5, e, pages, 4, capture, 4) == GgttStatus::Busy);     // already published
    CHECK(p.release(5, e) == GgttStatus::Busy);
    CHECK(p.unpublish(6, e, true) == GgttStatus::Ownership);
    const size_t before = f.log.size();
    CHECK(p.unpublish(5, e, false) == GgttStatus::Busy);                 // no retirement proof
    f.gtAvailable = false;
    CHECK(p.unpublish(5, e, true) == GgttStatus::Unavailable);           // no forcewake/reset hold
    CHECK(f.log.size() == before && p.state(e) == GgttExtentState::Published);
    f.gtAvailable = true;
    CHECK(p.unpublish(5, e, true) == GgttStatus::Ok);
    CHECK(p.state(e) == GgttExtentState::Reserved && p.captured(e) == nullptr);
    for (uint64_t i = 0; i < 4; ++i) CHECK(f.entry(first + i) == prior[i]);
    CHECK(f.entry(first - 1) == 0x1111000001ULL && f.entry(first + 4) == 0x2222000001ULL);
    // Inverse: fence, restore exactly the owned entries, fence, readback, invalidate.
    at = before;
    expect('A', 0, 0, false);
    expect('F', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) expect('w', first + i, prior[i], true);
    expect('F', 0, 0, false);
    for (uint64_t i = 0; i < 4; ++i) expect('r', first + i, prior[i], true);
    for (uint32_t control : L::kEngineTlbControls) expect('W', control, 1, true);
    for (const Ev &ev : f.log) if (ev.op == 'w') CHECK(ev.at >= first && ev.at < first + 4);
    CHECK(f.acquires == f.releases);
    CHECK(p.release(5, e) == GgttStatus::Ok && p.shutdown() == GgttStatus::Ok);
}

void testCleanRefusals() {
    const GgttGeometry g = geometry1G();
    Fake f(g.entryCount);
    GgttPublisher p;
    CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
    GgttExtent e {};
    CHECK(p.reserve(5, 2 * Page, e) == GgttStatus::Ok);
    uint64_t capture[2] = {};
    const uint64_t high[2] = {0x1000, uint64_t(1) << 39};
    const uint64_t unaligned[2] = {0x1000, 0x2008};
    const uint64_t good[2] = {0x1000, 0x2000};
    CHECK(p.publish(5, e, high, 2, capture, 2) == GgttStatus::EncodingRejected);
    CHECK(p.publish(5, e, unaligned, 2, capture, 2) == GgttStatus::EncodingRejected);
    CHECK(p.publish(5, e, good, 1, capture, 2) == GgttStatus::InvalidArgument);
    CHECK(p.publish(5, e, good, 2, capture, 1) == GgttStatus::InvalidArgument);
    uint64_t overlap[3] = {0x1000, 0x2000, 0};
    CHECK(p.publish(5, e, overlap, 2, overlap + 1, 2) == GgttStatus::InvalidArgument);
    CHECK(p.publish(4, e, good, 2, capture, 2) == GgttStatus::Ownership);
    CHECK(f.log.empty() && f.acquires == 0);
    f.gtAvailable = false;
    CHECK(p.publish(5, e, good, 2, capture, 2) == GgttStatus::Unavailable);
    CHECK(f.log.empty());
    f.gtAvailable = true;
    f.failEntryRead = 1;                                   // capture read refused: reads only
    CHECK(p.publish(5, e, good, 2, capture, 2) == GgttStatus::Unavailable);
    CHECK(f.count('w') == 0 && f.count('W') == 0 && f.acquires == f.releases);
    CHECK(p.state(e) == GgttExtentState::Reserved && !p.faulted());
    CHECK(p.unpublish(5, e, true) == GgttStatus::InvalidArgument);
    CHECK(p.release(5, e) == GgttStatus::Ok);
}

struct FaultCase { const char *name; void (*arm)(Fake &); GgttStatus expected; };

void testUncertainPublication() {
    const FaultCase cases[] = {
        {"entry store refused", [](Fake &f) { f.failEntryWrite = 1; }, GgttStatus::TransportFailure},
        {"dropped store", [](Fake &f) { f.dropIndex = long(72 * MiB / Page + 1); }, GgttStatus::ReadbackMismatch},
        {"readback refused", [](Fake &f) { f.failEntryRead = 3; }, GgttStatus::TransportFailure},
        {"invalidate request refused", [](Fake &f) { f.failRegWrite = 2; }, GgttStatus::TransportFailure},
        {"invalidate poll refused", [](Fake &f) { f.failRegRead = 3; }, GgttStatus::TransportFailure},
        {"stuck VCS control", [](Fake &f) { f.stuck.insert(L::kM1tcr); }, GgttStatus::InvalidateTimeout},
        {"clock regression", [](Fake &f) { f.regressAtCall = 3; }, GgttStatus::ClockRegression},
        {"hold release failed", [](Fake &f) { f.gtReleaseOk = false; }, GgttStatus::TransportFailure},
    };
    for (const FaultCase &fault : cases) {
        const GgttGeometry g = geometry1G();
        Fake f(g.entryCount);
        GgttPublisher p;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e {}, other {};
        CHECK(p.reserve(5, 2 * Page, e) == GgttStatus::Ok);
        CHECK(p.reserve(5, Page, other) == GgttStatus::Ok);
        fault.arm(f);
        const uint64_t pages[2] = {0x10000, 0x20000};
        uint64_t capture[2] = {};
        const GgttStatus status = p.publish(5, e, pages, 2, capture, 2);
        if (status != fault.expected) std::fprintf(stderr, "case %s -> %d\n", fault.name, int(status));
        CHECK(status == fault.expected);
        CHECK(p.state(e) == GgttExtentState::Quarantined && p.faulted());
        CHECK(p.captured(e) == capture);                    // capture held with the extent
        CHECK(f.acquires == 1 && f.releases == 1);          // hold always handed back once
        CHECK(p.release(5, e) == GgttStatus::Quarantined);
        CHECK(p.unpublish(5, e, true) == GgttStatus::Quarantined);
        const size_t logged = f.log.size();
        CHECK(p.publish(5, other, pages, 1, capture, 1) == GgttStatus::Quarantined);
        CHECK(p.reserve(5, Page, other) == GgttStatus::Quarantined);
        CHECK(f.log.size() == logged);                      // latched: no further effects
        CHECK(p.shutdown() == GgttStatus::Quarantined && p.initialized());
    }
}

void testInvalidationTiming() {
    const GgttGeometry g = geometry1G();
    {   // Stuck control: 100 us tight phase, then 10..1280 us backoff past 4 ms.
        Fake f(g.entryCount);
        f.stuck.insert(L::kRtcr);
        GgttPublisher p;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e {};
        CHECK(p.reserve(5, Page, e) == GgttStatus::Ok);
        const uint64_t page = 0x40000;
        uint64_t capture = 0;
        const uint64_t start = f.clock;
        CHECK(p.publish(5, e, &page, 1, &capture, 1) == GgttStatus::InvalidateTimeout);
        const uint64_t elapsed = f.clock - start;
        CHECK(elapsed >= 4100000 && elapsed <= 6500000);
        unsigned tight = 0;
        std::vector<uint32_t> slow;
        for (uint32_t us : f.pauses) { if (us == 1) ++tight; else slow.push_back(us); }
        CHECK(tight == 100);
        const std::vector<uint32_t> backoff = {10, 20, 40, 80, 160, 320, 640, 1280, 1280, 1280};
        CHECK(slow == backoff);
        CHECK(f.count('Q') == 101 + 11);
        // Only RCS was waited on: the remaining controls were requested, not polled.
        for (const Ev &ev : f.log) if (ev.op == 'Q') CHECK(ev.at == L::kRtcr);
        CHECK(f.count('W') == 4);
    }
    {   // A clock that never advances is still bounded by the poll cap.
        Fake f(g.entryCount);
        f.stuck.insert(L::kVtcr);
        f.frozen = true;
        GgttPublisher p;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e {};
        CHECK(p.reserve(5, Page, e) == GgttStatus::Ok);
        const uint64_t page = 0x40000;
        uint64_t capture = 0;
        CHECK(p.publish(5, e, &page, 1, &capture, 1) == GgttStatus::InvalidateTimeout);
        unsigned vtcrPolls = 0;
        for (const Ev &ev : f.log) vtcrPolls += ev.op == 'Q' && ev.at == L::kVtcr;
        CHECK(f.count('Q') <= L::kMaxTlbPolls * L::kEngineCount);
        CHECK(vtcrPolls == L::kMaxTlbPolls);
    }
    {   // Late but in-window completion in the sleeping phase is success.
        Fake f(g.entryCount);
        f.clearAfter = 105;
        GgttPublisher p;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e {};
        CHECK(p.reserve(5, Page, e) == GgttStatus::Ok);
        const uint64_t page = 0x40000;
        uint64_t capture = 0;
        CHECK(p.publish(5, e, &page, 1, &capture, 1) == GgttStatus::Ok);
        CHECK(p.state(e) == GgttExtentState::Published);
    }
}

void testUncertainInverse() {
    const GgttGeometry g = geometry1G();
    const FaultCase cases[] = {
        {"restore store refused", [](Fake &f) { f.failEntryWrite = long(f.entryWrites) + 1; }, GgttStatus::TransportFailure},
        {"restore dropped", [](Fake &f) { f.dropIndex = long(72 * MiB / Page); }, GgttStatus::ReadbackMismatch},
        {"inverse invalidation stuck", [](Fake &f) { f.stuck.insert(L::kBtcr); }, GgttStatus::InvalidateTimeout},
        {"inverse hold release failed", [](Fake &f) { f.gtReleaseOk = false; }, GgttStatus::TransportFailure},
    };
    for (const FaultCase &fault : cases) {
        Fake f(g.entryCount);
        GgttPublisher p;
        CHECK(p.initialize(g, handover(), io(f), gt(f)) == GgttStatus::Ok);
        GgttExtent e {};
        CHECK(p.reserve(5, 2 * Page, e) == GgttStatus::Ok);
        const uint64_t pages[2] = {0x10000, 0x20000};
        uint64_t capture[2] = {};
        CHECK(p.publish(5, e, pages, 2, capture, 2) == GgttStatus::Ok);
        fault.arm(f);
        CHECK(p.unpublish(5, e, true) == fault.expected);
        CHECK(p.state(e) == GgttExtentState::Quarantined && p.faulted() && p.captured(e) == capture);
        CHECK(p.release(5, e) == GgttStatus::Quarantined);
        CHECK(f.acquires == f.releases);
    }
}
} // namespace

int main() {
    testGeometryAndBar();
    testInitializeRefusals();
    testReservations();
    testAwakeEngineAuthority();
    testPublishSequenceAndRestore();
    testCleanRefusals();
    testUncertainPublication();
    testInvalidationTiming();
    testUncertainInverse();
    std::printf("GLK GGTT publication core (hosted injected transport): %u checks; no hardware touched\n", checks);
    return 0;
}

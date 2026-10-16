// SPDX-License-Identifier: MIT
// Upstream notices and the narrow Linux import scope are in InterruptDecode.hpp.
#include "InterruptDecode.hpp"

namespace MellowNativeGlk {
namespace Interrupt {
namespace {
constexpr uint32_t AllOnes = 0xffffffffU;

uint32_t bankEnable(uint8_t engines, uint16_t cs, unsigned bank) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < EngineCount; ++i) {
        const auto engine = static_cast<Engine>(i);
        const EngineRoute route = engineRoute(engine);
        if ((engines & engineBit(engine)) && route.bank == bank) bits |= uint32_t(cs) << route.shift;
    }
    return bits;
}
}

bool Decoder::admitted() const { return ops_.admitted && ops_.admitted(ops_.opaque, config_.epoch); }

bool Decoder::read(uint32_t reg, uint32_t &value) const {
    value = 0;
    return admitted() && ops_.read32(ops_.opaque, reg, value);
}

bool Decoder::write(uint32_t reg, uint32_t value) const {
    return admitted() && ops_.write32(ops_.opaque, reg, value);
}

void Decoder::recompute() {
    owned_ = 0;
    for (unsigned bank = 0; bank < GtBanks; ++bank) enabled_[bank] = bankEnable(engines_, config_.csIrqs, bank);
    for (unsigned i = 0; i < EngineCount; ++i) {
        const auto engine = static_cast<Engine>(i);
        if (engines_ & engineBit(engine)) owned_ |= engineRoute(engine).master;
    }
}

Status Decoder::fail(Status status) {
    fault_ = status;
    __atomic_store_n(&state_, uint32_t(Fault), __ATOMIC_RELEASE);
    // Delivery is left masked; only finishStop() proves the mask.
    if (admitted()) ops_.write32(ops_.opaque, MasterIrq, 0);
    return status;
}

// gen8_master_intr_disable() plus a posting read that must show the enable clear.
Status Decoder::disableMaster() {
    uint32_t posted = 0;
    if (!write(MasterIrq, 0) || !read(MasterIrq, posted) || posted == AllOnes || (posted & MasterControl))
        return Status::IoFailure;
    return Status::Ok;
}

// gen2_irq_reset(): IMR all ones (posted), IER zero, IIR cleared twice.
bool Decoder::resetBank(unsigned bank) const {
    uint32_t posted = 0;
    return write(gtImr(bank), AllOnes) && read(gtImr(bank), posted) && write(gtIer(bank), 0) &&
        write(gtIir(bank), AllOnes) && read(gtIir(bank), posted) &&
        write(gtIir(bank), AllOnes) && read(gtIir(bank), posted);
}

// gen2_irq_init() with gen2_assert_iir_is_zero(); afterwards the bank must
// enable exactly the configured CS bits and nothing else.
bool Decoder::initBank(unsigned bank) const {
    uint32_t value = 0;
    if (!read(gtIir(bank), value) || value == AllOnes) return false;
    if (value && (!write(gtIir(bank), AllOnes) || !read(gtIir(bank), value) ||
                  !write(gtIir(bank), AllOnes) || !read(gtIir(bank), value) || value)) return false;
    const uint32_t on = enabled_[bank];
    uint32_t ier = 0;
    return write(gtIer(bank), on) && write(gtImr(bank), ~on) && read(gtImr(bank), value) &&
        !(value & on) && read(gtIer(bank), ier) && ier == on;
}

Status Decoder::configure(const Configuration &c, const RegisterOps &o) {
    if (state() != Unconfigured) return Status::Invalid;
    if (!c.epoch || !c.engines || (c.engines & ~GlkEngines) || !c.csIrqs || (c.csIrqs & ~CsExeclistsIrqs) ||
        !o.opaque || !o.admitted || !o.read32 || !o.write32) return Status::Invalid;
    if (!sameIdentity(c.identity, FirstHardware)) return Status::Unsupported;
    config_ = c; ops_ = o; engines_ = c.engines; fault_ = Status::Ok; unowned_ = 0; empty_ = 0;
    for (auto &bits : unexpected_) bits = 0;
    __atomic_store_n(&sampled_, 0U, __ATOMIC_RELEASE);
    recompute();
    const auto reject = [this](Status status) {
        config_ = {}; ops_ = {}; engines_ = 0; recompute();
        return status;
    };
    if (!admitted()) return reject(Status::Unavailable);
    // Read-only proof before any effect: the device answers and no display or
    // PCU source (not owned here) can be delivered through the shared master.
    uint32_t value = 0;
    if (!read(MasterIrq, value) || value == AllOnes) return reject(Status::IoFailure);
    const uint32_t foreign[] {dePipeIer(0), dePipeIer(1), dePipeIer(2), DePortIer, DeMiscIer, PcuIer};
    for (uint32_t reg : foreign) {
        if (!read(reg, value) || value == AllOnes) return reject(Status::IoFailure);
        if (value) { unowned_ = value; return reject(Status::Unowned); }
    }
    // gen8_irq_reset(): master first, then every GT bank (PM/GuC included).
    if (disableMaster() != Status::Ok) return fail(Status::IoFailure);
    for (unsigned bank = 0; bank < GtBanks; ++bank) if (!resetBank(bank)) return fail(Status::IoFailure);
    // gen8_gt_irq_postinstall() restricted to configured engines; bank 2
    // (RPS/GuC) stays fully masked because neither is implemented on GLK here.
    for (unsigned bank = 0; bank < GtBanks; ++bank) if (!initBank(bank)) return fail(Status::IoFailure);
    __atomic_store_n(&state_, uint32_t(Configured), __ATOMIC_RELEASE);
    return Status::Ok;
}

Status Decoder::enableMaster() {
    __atomic_store_n(&state_, uint32_t(Armed), __ATOMIC_RELEASE);
    if (!write(MasterIrq, MasterControl)) return fail(Status::IoFailure);
    // A primary interrupt may claim a level between the state store and this
    // enable; its mask write can land first. Never leave delivery enabled
    // while a claim is pending.
    const uint32_t after = state();
    if (after != Armed && !write(MasterIrq, 0)) return fail(Status::IoFailure);
    return after == Fault ? fault_ : Status::Ok;
}

Status Decoder::arm() {
    if (state() != Configured) return Status::Invalid;
    if (!admitted()) return Status::Unavailable;
    return enableMaster();
}

bool Decoder::filter() {
    // Primary interrupt context: bounded MMIO and atomics only.
    if (__atomic_load_n(&state_, __ATOMIC_ACQUIRE) != Armed || !admitted()) return false;
    uint32_t level = 0;
    uint32_t expected = Armed;
    if (!ops_.read32(ops_.opaque, MasterIrq, level)) {
        fault_ = Status::IoFailure;
        return __atomic_compare_exchange_n(&state_, &expected, uint32_t(Fault), false,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (level == AllOnes) { // device gone/D3: no write, report through the workloop
        fault_ = Status::IoFailure;
        return __atomic_compare_exchange_n(&state_, &expected, uint32_t(Fault), false,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (!(level & ~MasterControl)) return false; // nothing pending here; no write, no re-enable
    if (!__atomic_compare_exchange_n(&state_, &expected, uint32_t(Pending), false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    // gen8_master_intr_disable(): mask, then sample the level bits while masked.
    if (!ops_.write32(ops_.opaque, MasterIrq, 0) || !ops_.read32(ops_.opaque, MasterIrq, level) ||
        level == AllOnes || (level & MasterControl)) {
        fault_ = Status::IoFailure;
        __atomic_store_n(&state_, uint32_t(Fault), __ATOMIC_RELEASE);
        return true;
    }
    __atomic_fetch_or(&sampled_, level & ~MasterControl, __ATOMIC_ACQ_REL);
    return true;
}

Status Decoder::collect(Batch &out) {
    out = Batch {};
    const uint32_t current = state();
    if (current == Fault) return fault_;
    if (current != Pending) return Status::Empty;
    if (!admitted()) return fail(Status::Unavailable);
    __atomic_store_n(&state_, uint32_t(Servicing), __ATOMIC_RELEASE);
    const uint32_t master = __atomic_exchange_n(&sampled_, 0U, __ATOMIC_ACQ_REL);
    out.master = master;
    // Display, PCU, RPS/GuC and unconfigured engines are not owned: their
    // IIRs are never read or acknowledged, and delivery stays masked.
    if (master & ~owned_) { unowned_ = master & ~owned_; return fail(Status::Unowned); }
    uint32_t iir[GtBanks] {};
    for (unsigned bank = 0; bank < GtBanks; ++bank) {
        if (!(master & gtBankMaster(bank))) continue;
        if (!read(gtIir(bank), iir[bank]) || iir[bank] == AllOnes) return fail(Status::IoFailure);
        if (iir[bank] & ~enabled_[bank]) { unexpected_[bank] = iir[bank]; return fail(Status::UnexpectedBits); }
    }
    // gen8_gt_irq_handler(): write back exactly the sampled IIR (W1C) so an
    // event latched after the sample stays pending for the next delivery.
    for (unsigned bank = 0; bank < GtBanks; ++bank)
        if (iir[bank] && !write(gtIir(bank), iir[bank])) return fail(Status::IoFailure);
    for (unsigned i = 0; i < EngineCount; ++i) {
        const auto engine = static_cast<Engine>(i);
        if (!(engines_ & engineBit(engine))) continue;
        const EngineRoute route = engineRoute(engine);
        out.events[i] = static_cast<uint16_t>((iir[route.bank] >> route.shift) & config_.csIrqs);
    }
    for (unsigned bank = 0; bank < GtBanks; ++bank) out.iir[bank] = iir[bank];
    if (out.any()) empty_ = 0;
    else if (++empty_ >= StormLimit) return fail(Status::Storm);
    return Status::Ok;
}

Status Decoder::maskEngine(Engine engine) {
    const uint32_t current = state();
    if (current != Servicing && current != Configured) return Status::Invalid;
    if (static_cast<unsigned>(engine) >= EngineCount || !(engines_ & engineBit(engine))) return Status::Invalid;
    if (!admitted()) return fail(Status::Unavailable);
    const EngineRoute route = engineRoute(engine);
    const uint32_t removed = uint32_t(config_.csIrqs) << route.shift;
    engines_ = static_cast<uint8_t>(engines_ & ~engineBit(engine));
    recompute();
    const uint32_t on = enabled_[route.bank];
    uint32_t imr = 0, ier = 0, iir = 0;
    // Stop latching, then propagation; prove both, then acknowledge exactly
    // the masked engine's latched bits so its master level cannot persist.
    if (!write(gtImr(route.bank), ~on) || !write(gtIer(route.bank), on) ||
        !read(gtImr(route.bank), imr) || (imr & removed) != removed ||
        !read(gtIer(route.bank), ier) || ier != on ||
        !read(gtIir(route.bank), iir) || iir == AllOnes ||
        ((iir & removed) && !write(gtIir(route.bank), iir & removed))) return fail(Status::IoFailure);
    return Status::Ok;
}

Status Decoder::rearm() {
    const uint32_t current = state();
    if (current == Fault) return fault_;
    if (current != Servicing) return Status::Invalid;
    if (!admitted()) return fail(Status::Unavailable);
    return enableMaster();
}

uint32_t Decoder::beginStop() {
    uint32_t previous = state();
    while (previous != Unconfigured &&
           !__atomic_compare_exchange_n(&state_, &previous, uint32_t(Stopping), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    return previous;
}

Status Decoder::finishStop() {
    const uint32_t current = state();
    if (current == Unconfigured) return Status::Ok;
    if (current != Stopping) return Status::Invalid;
    if (!admitted()) return Status::Unavailable; // no stale-epoch MMIO; caller keeps registration
    if (disableMaster() != Status::Ok) return Status::IoFailure;
    for (unsigned bank = 0; bank < GtBanks; ++bank) if (!resetBank(bank)) return Status::IoFailure;
    for (unsigned bank = 0; bank < GtBanks; ++bank) {
        uint32_t ier = AllOnes;
        if (!read(gtIer(bank), ier) || ier) return Status::IoFailure;
    }
    __atomic_store_n(&sampled_, 0U, __ATOMIC_RELEASE);
    config_ = {}; ops_ = {}; engines_ = 0; recompute();
    __atomic_store_n(&state_, uint32_t(Unconfigured), __ATOMIC_RELEASE);
    return Status::Ok;
}
}
}

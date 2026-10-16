// SPDX-License-Identifier: MIT
/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2022 Intel Corporation
 * Copyright (c) 2026 Mellow contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Adapted from torvalds/linux 0d9ff90a5422cc7509258aaaba1e7481df4d332a
 * drivers/gpu/drm/i915/intel_uncore.c. See LICENSE.MIT and provenance.json.
 */
#include "Forcewake.hpp"

namespace MellowNativeGlk {
namespace {
using Status = ForcewakeStatus;
constexpr uint32_t AllOnes = 0xffffffffU;
constexpr uint8_t bit(unsigned index) { return static_cast<uint8_t>(1U << index); }

struct FwRange {
    uint32_t start, end;
    uint8_t domains;
};
constexpr uint8_t R = RenderDomain, G = GtDomain, M = MediaDomain;
// __gen9_fw_ranges: sorted and watertight over 0x0-0x3ffff.
constexpr FwRange Gen9FwRanges[] = {
    {0x0, 0xaff, G}, {0xb00, 0x1fff, 0}, /* uncore range */
    {0x2000, 0x26ff, R}, {0x2700, 0x2fff, G}, {0x3000, 0x3fff, R}, {0x4000, 0x51ff, G},
    {0x5200, 0x7fff, R}, {0x8000, 0x812f, G}, {0x8130, 0x813f, M}, {0x8140, 0x815f, R},
    {0x8160, 0x82ff, G}, {0x8300, 0x84ff, R}, {0x8500, 0x87ff, G}, {0x8800, 0x89ff, M},
    {0x8a00, 0x8bff, G}, {0x8c00, 0x8cff, R}, {0x8d00, 0x93ff, G}, {0x9400, 0x97ff, R | M},
    {0x9800, 0xafff, G}, {0xb000, 0xb47f, R}, {0xb480, 0xcfff, G}, {0xd000, 0xd7ff, M},
    {0xd800, 0xdfff, G}, {0xe000, 0xe8ff, R}, {0xe900, 0x11fff, G}, {0x12000, 0x13fff, M},
    {0x14000, 0x19fff, G}, {0x1a000, 0x1e9ff, M}, {0x1ea00, 0x243ff, G}, {0x24400, 0x247ff, R},
    {0x24800, 0x2ffff, G}, {0x30000, 0x3ffff, M},
};
struct MmioRange {
    uint32_t start, end;
};
// gen8_shadowed_regs: write-only shadows that need no wake (reads still do).
constexpr MmioRange Gen8ShadowedRegs[] = {
    {0x2030, 0x2030}, {0xa008, 0xa00c}, {0x12030, 0x12030}, {0x1a030, 0x1a030}, {0x22030, 0x22030},
};

// NEEDS_FORCE_WAKE()
constexpr bool needsForceWake(uint32_t offset) { return offset < 0x40000 || offset >= 0x116000; }

// find_fw_domain(): BSEARCH over the sorted table; no entry means no domain.
uint8_t findFwDomain(uint32_t offset) {
    size_t low = 0, high = sizeof(Gen9FwRanges) / sizeof(Gen9FwRanges[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const FwRange &entry = Gen9FwRanges[middle];
        if (offset < entry.start) high = middle;
        else if (offset > entry.end) low = middle + 1;
        else return entry.domains;
    }
    return 0;
}

bool isShadowed(uint32_t offset) {
    for (const MmioRange &range : Gen8ShadowedRegs)
        if (offset >= range.start && offset <= range.end) return true;
    return false;
}
} // namespace

bool isForcewakeRegister(uint32_t offset) {
    for (const DomainRegisters &domain : Gen9Domains)
        if (offset == domain.set || offset == domain.ack) return true;
    return false;
}

uint8_t gen9ReadDomains(uint32_t offset) {
    return needsForceWake(offset) ? findFwDomain(offset) : 0;
}

uint8_t gen9WriteDomains(uint32_t offset) {
    return needsForceWake(offset) && !isShadowed(offset) ? findFwDomain(offset) : 0;
}

ForcewakeStatus Gen9Forcewake::bind(const ForcewakeIo &io) {
    if (state_ != State::Unbound) return Status::Busy;
    if (!io.read32 || !io.write32 || !io.nowUs || !io.delayUs) return Status::Invalid;
    io_ = io;
    active_ = 0;
    for (uint32_t &count : wakeCount_) count = 0;
    invalidateLeases();
    lastNowUs_ = 0;
    diagnostics_ = {};
    state_ = State::Bound;
    return Status::Ok;
}

ForcewakeStatus Gen9Forcewake::now(uint64_t &value) {
    uint64_t sample = 0;
    if (!io_.nowUs(io_.context, sample) || sample < lastNowUs_) return Status::ClockFailure;
    lastNowUs_ = value = sample;
    return Status::Ok;
}

ForcewakeStatus Gen9Forcewake::write(uint32_t offset, uint32_t value) {
    return io_.write32(io_.context, offset, value) ? Status::Ok : Status::IoFailure;
}

// fw_ack(). intel_uncore.c treats an all-ones ack as "MMIO unreliable" when an
// ack-clear wait fails; it is refused immediately here, because the ack-set
// condition (~0 & FORCEWAKE_KERNEL) would otherwise be satisfied by a dead BAR.
ForcewakeStatus Gen9Forcewake::readAck(const DomainRegisters &domain, uint32_t &value) {
    if (!io_.read32(io_.context, domain.ack, value)) return Status::IoFailure;
    diagnostics_.lastAck = value;
    return value == AllOnes ? Status::MmioUnreliable : Status::Ok;
}

// __wait_for_ack(): wait_for_atomic((fw_ack(d) & ack) == value, 50 ms). Time is
// sampled before each condition check, so the condition is always evaluated
// once after expiry. A caller deadline may only shorten the bound, and then
// reports DeadlineExceeded instead of a hardware timeout.
ForcewakeStatus Gen9Forcewake::waitAck(const DomainRegisters &domain, uint32_t ackBit,
                                       uint32_t value, uint64_t deadlineUs, bool &matched) {
    matched = false;
    uint64_t base = 0;
    Status status = now(base);
    if (status != Status::Ok) return status;
    const uint64_t remaining = deadlineUs > base ? deadlineUs - base : 0;
    const bool limited = remaining < ForcewakeAckTimeoutUs;
    const uint64_t budget = limited ? remaining : ForcewakeAckTimeoutUs;
    for (;;) {
        uint64_t current = 0;
        if ((status = now(current)) != Status::Ok) return status;
        uint32_t ack = 0;
        if ((status = readAck(domain, ack)) != Status::Ok) return status;
        if ((ack & ackBit) == value) {
            matched = true;
            return Status::Ok;
        }
        if (current - base >= budget) return limited ? Status::DeadlineExceeded : Status::Ok;
        if (io_.relax) io_.relax(io_.context);
    }
}

// fw_domain_wait_ack_with_fallback(): WaRsForcewakeAddDelayForAck
// (skl,bxt,kbl,glk,cfl,cnl,icl), HSDES #1604254524. Toggle the fallback request
// to kick the power state machine so a lost kernel ack is delivered. The two
// fallback ack waits are best effort (their timeouts are ignored, as upstream),
// and the fallback bit is cleared at the end of every pass.
ForcewakeStatus Gen9Forcewake::ackWithFallback(const DomainRegisters &domain, bool set,
                                               uint64_t deadlineUs, bool &detected) {
    const uint32_t value = set ? ForcewakeKernel : 0;
    ++diagnostics_.fallbackInvocations;
    detected = false;
    unsigned pass = 1;
    do {
        bool ignored = false;
        Status status = waitAck(domain, ForcewakeKernelFallback, 0, deadlineUs, ignored);
        if (status != Status::Ok) return status;
        status = write(domain.set, maskedEnable(ForcewakeKernelFallback));
        if (status == Status::Ok) {
            // Give gt some time to relax before the polling frenzy.
            io_.delayUs(io_.context, FallbackRelaxStepUs * pass);
            status = waitAck(domain, ForcewakeKernelFallback, ForcewakeKernelFallback,
                             deadlineUs, ignored);
        }
        uint32_t ack = 0;
        if (status == Status::Ok) status = readAck(domain, ack);
        if (status == Status::Ok) detected = (ack & ForcewakeKernel) == value;
        const Status cleared = write(domain.set, maskedDisable(ForcewakeKernelFallback));
        diagnostics_.lastFallbackPasses = pass;
        if (status != Status::Ok) return status;
        if (cleared != Status::Ok) return cleared;
    } while (!detected && pass++ < FallbackMaxPasses);
    return Status::Ok;
}

// fw_domain_wait_ack_{clear,set}_fallback(), including the final
// fw_domain_wait_ack_{clear,set}() wait. Upstream logs and proceeds after that
// last timeout; this driver reports it and the caller fails closed.
ForcewakeStatus Gen9Forcewake::waitKernelAck(const DomainRegisters &domain, bool set,
                                             uint64_t deadlineUs) {
    const uint32_t value = set ? ForcewakeKernel : 0;
    bool matched = false;
    Status status = waitAck(domain, ForcewakeKernel, value, deadlineUs, matched);
    if (status != Status::Ok || matched) return status;
    bool detected = false;
    status = ackWithFallback(domain, set, deadlineUs, detected);
    if (status != Status::Ok || detected) return status;
    status = waitAck(domain, ForcewakeKernel, value, deadlineUs, matched);
    if (status != Status::Ok || matched) return status;
    return set ? Status::AckSetTimeout : Status::AckClearTimeout;
}

// fw_domains_reset() / fw_domain_reset(): WaRsClearFWBitsAtReset clears all 16
// request bits before graphics version 12. No ack waits, as upstream.
ForcewakeStatus Gen9Forcewake::resetRequests() {
    Status status = Status::Ok;
    for (const DomainRegisters &domain : Gen9Domains)
        if (write(domain.set, maskedDisable(ResetClearBitsGen9)) != Status::Ok) status = Status::IoFailure;
    return status;
}

ForcewakeStatus Gen9Forcewake::fail(ForcewakeStatus status, uint8_t domainMask) {
    diagnostics_.lastFailure = status;
    diagnostics_.failedDomainMask = domainMask;
    state_ = State::Faulted;
    return status;
}

void Gen9Forcewake::invalidateLeases() {
    for (LeaseSlot &slot : leases_) {
        ++slot.generation;
        slot.domains = 0;
        slot.live = false;
    }
}

ForcewakeStatus Gen9Forcewake::initialize(uint64_t deadlineUs) {
    if (state_ == State::Unbound) return Status::Unavailable;
    if (state_ == State::Ready) return Status::Busy;
    if (state_ == State::Faulted) return Status::Faulted;
    uint64_t base = 0;
    Status status = now(base);
    if (status != Status::Ok) return status;
    if (base >= deadlineUs) return Status::DeadlineExceeded;
    // sanity_check_mmio_access(): FORCEWAKE_MT (== FORCEWAKE_GT_GEN9) is masked,
    // so its upper half never reads back as ones while the BAR decodes. This
    // read-only wait_for(COND, 2000) with 10..1000 us backoff precedes writes.
    const uint64_t remaining = deadlineUs - base;
    const bool limited = remaining < MmioSanityTimeoutUs;
    const uint64_t budget = limited ? remaining : MmioSanityTimeoutUs;
    uint32_t wait = 10;
    for (;;) {
        uint64_t current = 0;
        if ((status = now(current)) != Status::Ok) return status;
        uint32_t value = 0;
        if (!io_.read32(io_.context, ForcewakeGtGen9, value)) return Status::IoFailure;
        if (value != AllOnes) break;
        if (current - base >= budget) return limited ? Status::DeadlineExceeded : Status::MmioUnreliable;
        io_.delayUs(io_.context, wait);
        if (wait < 1000) wait <<= 1;
    }
    // __fw_domain_init() for render, gt, media resets each domain; then
    // forcewake_early_sanitize(uncore, 0) runs intel_uncore_forcewake_reset(),
    // which has no active domain to put and resets every domain again.
    for (unsigned round = 0; round < 2; ++round)
        if ((status = resetRequests()) != Status::Ok) return fail(status, AllDomains);
    active_ = 0;
    state_ = State::Ready;
    return Status::Ok;
}

ForcewakeStatus Gen9Forcewake::get(uint8_t domains, uint64_t deadlineUs, ForcewakeLease &lease) {
    lease = {};
    if (state_ == State::Unbound || state_ == State::Bound) return Status::Unavailable;
    if (state_ == State::Faulted) return Status::Faulted;
    if (!domains || (domains & ~AllDomains)) return Status::Invalid;
    unsigned slot = LeaseCapacity;
    for (unsigned i = 0; i < LeaseCapacity; ++i)
        if (!leases_[i].live) { slot = i; break; }
    if (slot == LeaseCapacity) return Status::Busy;
    uint64_t start = 0;
    Status status = now(start);
    if (status != Status::Ok) return status;
    if (start >= deadlineUs) return Status::DeadlineExceeded;

    // __intel_uncore_forcewake_get(): only domains without a reference wake.
    uint8_t wake = 0;
    for (unsigned i = 0; i < DomainCount; ++i)
        if ((domains & bit(i)) && !wakeCount_[i]) wake |= bit(i);
    // fw_domains_get_with_fallback(): per domain wait for the previous ack to
    // clear, then request; afterwards wait for every new request's ack.
    uint8_t requested = 0, failed = 0;
    for (unsigned i = 0; status == Status::Ok && i < DomainCount; ++i) {
        if (!(wake & bit(i))) continue;
        status = waitKernelAck(Gen9Domains[i], false, deadlineUs);
        if (status == Status::Ok) {
            requested |= bit(i);
            status = write(Gen9Domains[i].set, maskedEnable(ForcewakeKernel)); // fw_domain_get()
        }
        if (status != Status::Ok) failed = bit(i);
    }
    for (unsigned i = 0; status == Status::Ok && i < DomainCount; ++i) {
        if (!(wake & bit(i))) continue;
        status = waitKernelAck(Gen9Domains[i], true, deadlineUs);
        if (status != Status::Ok) failed = bit(i);
    }
    if (status != Status::Ok) {
        // Withdraw this call's requests (fw_domain_put()); held domains stay.
        for (unsigned i = 0; i < DomainCount; ++i)
            if ((requested & bit(i)) &&
                write(Gen9Domains[i].set, maskedDisable(ForcewakeKernel)) != Status::Ok)
                ++diagnostics_.rollbackFailures;
        return fail(status, failed);
    }
    active_ |= wake;
    for (unsigned i = 0; i < DomainCount; ++i)
        if (domains & bit(i)) ++wakeCount_[i];
    leases_[slot].live = true;
    leases_[slot].domains = domains;
    lease = {slot, leases_[slot].generation, domains};
    return Status::Ok;
}

ForcewakeStatus Gen9Forcewake::put(ForcewakeLease &lease) {
    if (state_ == State::Unbound) return Status::Unavailable;
    if (lease.slot >= LeaseCapacity) return Status::Invalid;
    LeaseSlot &slot = leases_[lease.slot];
    if (!slot.live || slot.generation != lease.generation) return Status::StaleLease;
    if (slot.domains != lease.domains) return Status::Ownership;
    // __intel_uncore_forcewake_put(delayed = false): release at count zero.
    uint8_t released = 0;
    for (unsigned i = 0; i < DomainCount; ++i)
        if ((lease.domains & bit(i)) && --wakeCount_[i] == 0) released |= bit(i);
    Status status = Status::Ok;
    for (unsigned i = 0; i < DomainCount; ++i) {
        if (!(released & bit(i))) continue;
        if (write(Gen9Domains[i].set, maskedDisable(ForcewakeKernel)) != Status::Ok)
            status = Status::IoFailure;
        active_ &= static_cast<uint8_t>(~bit(i));
    }
    slot.live = false;
    slot.domains = 0;
    ++slot.generation;
    lease = {};
    return status == Status::Ok ? status : fail(status, released);
}

ForcewakeStatus Gen9Forcewake::reset(uint8_t &lostDomains) {
    lostDomains = 0;
    if (state_ == State::Unbound) return Status::Unavailable;
    const uint8_t held = active_;
    Status status = Status::Ok;
    // intel_uncore_forcewake_reset(): fw_domains_put(active), then reset all.
    for (unsigned i = 0; i < DomainCount; ++i)
        if ((held & bit(i)) &&
            write(Gen9Domains[i].set, maskedDisable(ForcewakeKernel)) != Status::Ok)
            status = Status::IoFailure;
    if (resetRequests() != Status::Ok) status = Status::IoFailure;
    active_ = 0;
    for (uint32_t &count : wakeCount_) count = 0;
    invalidateLeases();
    lostDomains = held;
    if (status != Status::Ok) return fail(status, held);
    state_ = State::Bound;
    return Status::Ok;
}

ForcewakeStatus Gen9Forcewake::unbind(bool abandonHardwareState) {
    if (state_ == State::Unbound) return Status::Ok;
    if ((active_ || liveLeases()) && !abandonHardwareState) return Status::Busy;
    io_ = {};
    active_ = 0;
    for (uint32_t &count : wakeCount_) count = 0;
    invalidateLeases();
    state_ = State::Unbound;
    return Status::Ok;
}

uint32_t Gen9Forcewake::wakeCount(Domain domain) const {
    const unsigned index = static_cast<unsigned>(domain);
    return index < DomainCount ? wakeCount_[index] : 0;
}

unsigned Gen9Forcewake::liveLeases() const {
    unsigned live = 0;
    for (const LeaseSlot &slot : leases_) live += slot.live ? 1U : 0U;
    return live;
}

bool isGlkDeviceId(uint16_t device) {
    return device == 0x3184 || device == 0x3185;
}

bool glkBar0Physical(const GlkAuthoritySnapshot &snapshot, uint64_t &physical) {
    physical = 0;
    // Memory space (bit 0 clear) and 64-bit type (bits 2:1 = 10b) only.
    if ((snapshot.bar0Low & 1U) || (snapshot.bar0Low & 6U) != 4U) return false;
    physical = uint64_t(snapshot.bar0High) << 32 | (snapshot.bar0Low & ~0xfU);
    return physical != 0;
}

ForcewakeStatus validateAuthority(const GlkPciExpectation &expected, const GlkAuthoritySnapshot &actual) {
    if (!isGlkDeviceId(expected.device)) return Status::Unsupported;
    if (!expected.registryId || !expected.epoch || expected.slot > 31 || expected.function > 7)
        return Status::Invalid;
    if (actual.vendor != 0x8086 || actual.device != expected.device) return Status::IdentityChanged;
    if ((actual.classRevision >> 24) != 0x03) return Status::Unsupported; // display controller
    if ((actual.classRevision & 0xffU) != expected.revision ||
        actual.subsystemVendor != expected.subsystemVendor ||
        actual.subsystemDevice != expected.subsystemDevice ||
        actual.registryId != expected.registryId || actual.epoch != expected.epoch ||
        actual.bus != expected.bus || actual.slot != expected.slot ||
        actual.function != expected.function) return Status::IdentityChanged;
    if ((actual.headerType & 0x7fU) || actual.tunneled) return Status::Unsupported;
    if (!actual.providerOpen) return Status::Ownership;
    if (actual.inactive || !actual.powerStateD0) return Status::Unavailable;
    // PCI PM capability present and PMCSR PowerState (bits 1:0) == D0.
    if (!actual.pmCapability || actual.pmcsr == 0xffff || (actual.pmcsr & 3U)) return Status::Unavailable;
    // Memory Space Enable is required for BAR decode.
    if (actual.command == 0xffff || !(actual.command & 2U)) return Status::Unavailable;
    if ((actual.bar0Low & 1U) || (actual.bar0Low & 6U) != 4U) return Status::Unsupported;
    uint64_t physical = 0;
    if (!glkBar0Physical(actual, physical)) return Status::Unavailable;
    if (actual.mappedPhysical != physical) return Status::IdentityChanged;
    if (actual.mappedLength < RegisterWindowBytes) return Status::Unavailable;
    return Status::Ok;
}

} // namespace MellowNativeGlk

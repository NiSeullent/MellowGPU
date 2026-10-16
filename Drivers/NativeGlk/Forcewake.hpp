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
 * Gen9 (Gemini Lake) forcewake protocol adapted from torvalds/linux
 * 0d9ff90a5422cc7509258aaaba1e7481df4d332a drivers/gpu/drm/i915/intel_uncore.c
 * and gt/intel_gt_regs.h. See LICENSE.MIT and provenance.json for exact blobs.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeGlk {

// gt/intel_gt_regs.h: Gen9 request (masked) and acknowledge registers.
constexpr uint32_t ForcewakeRenderGen9 = 0xa278, ForcewakeGtGen9 = 0xa188,
    ForcewakeMediaGen9 = 0xa270;
constexpr uint32_t ForcewakeAckRenderGen9 = 0x0d84, ForcewakeAckGtGen9 = 0x130044,
    ForcewakeAckMediaGen9 = 0x0d88;
constexpr uint32_t ForcewakeKernel = 1U << 0, ForcewakeUser = 1U << 1,
    ForcewakeKernelFallback = 1U << 15;

// intel_uncore.c timing and workaround bounds.
constexpr uint64_t ForcewakeAckTimeoutUs = 50 * 1000;  // FORCEWAKE_ACK_TIMEOUT_MS
constexpr unsigned FallbackMaxPasses = 10;              // `pass++ < 10`
constexpr uint32_t FallbackRelaxStepUs = 10;            // udelay(10 * pass)
constexpr uint32_t ResetClearBitsGen9 = 0xffff;         // WaRsClearFWBitsAtReset, ver < 12
constexpr uint64_t MmioSanityTimeoutUs = 2000 * 1000;   // sanity_check_mmio_access()
constexpr uint64_t RegisterWindowBytes = 2 * 1024 * 1024; // intel_uncore_setup_mmio()

// include/drm/intel/reg_bits.h REG_MASKED_FIELD: bits 31:16 select which of
// bits 15:0 a write updates. Values outside that contract are rejected.
constexpr bool maskedFieldValid(uint32_t mask, uint32_t value) {
    return !(mask & 0xffff0000U) && !(value & ~mask);
}
constexpr uint32_t maskedField(uint32_t mask, uint32_t value) { return mask << 16 | value; }
constexpr uint32_t maskedEnable(uint32_t bits) { return maskedField(bits, bits); }
constexpr uint32_t maskedDisable(uint32_t bits) { return maskedField(bits, 0); }

// FW_DOMAIN_ID_RENDER/GT/MEDIA and FORCEWAKE_RENDER/GT/MEDIA = BIT(id).
enum class Domain : uint8_t { Render = 0, Gt = 1, Media = 2 };
constexpr unsigned DomainCount = 3;
constexpr uint8_t RenderDomain = 1U << 0, GtDomain = 1U << 1, MediaDomain = 1U << 2,
    AllDomains = RenderDomain | GtDomain | MediaDomain;

struct DomainRegisters {
    Domain id;
    uint32_t set, ack;
};
// intel_uncore_fw_domains_init() IS_GRAPHICS_VER(9, 10) branch, in init order.
// Iteration (for_each_fw_domain_masked) is ascending domain id, as here.
constexpr DomainRegisters Gen9Domains[DomainCount] = {
    {Domain::Render, ForcewakeRenderGen9, ForcewakeAckRenderGen9},
    {Domain::Gt, ForcewakeGtGen9, ForcewakeAckGtGen9},
    {Domain::Media, ForcewakeMediaGen9, ForcewakeAckMediaGen9},
};

// The only BAR offsets this component may touch.
bool isForcewakeRegister(uint32_t offset);
// __gen9_fw_ranges with NEEDS_FORCE_WAKE; writes also exclude gen8_shadowed_regs.
// Zero means no forcewake domain is required for that access on Gen9.
uint8_t gen9ReadDomains(uint32_t offset);
uint8_t gen9WriteDomains(uint32_t offset);

enum class ForcewakeStatus : uint8_t {
    Ok, Invalid, Unavailable, Unsupported, Ownership, StaleLease, Busy, Faulted,
    IoFailure, MmioUnreliable, ClockFailure, DeadlineExceeded,
    AckClearTimeout, AckSetTimeout, IdentityChanged
};

// Injected register/time boundary. The production implementation is the
// IOKit mapped-BAR adapter under owner authority; host tests inject emulated
// register files that prove protocol bodies only, never hardware behavior.
struct ForcewakeIo {
    void *context {};
    bool (*read32)(void *, uint32_t offset, uint32_t &value) {};
    bool (*write32)(void *, uint32_t offset, uint32_t value) {};
    bool (*nowUs)(void *, uint64_t &monotonicUs) {};
    void (*delayUs)(void *, uint32_t) {};  // udelay(): busy wait, never sleeps
    void (*relax)(void *) {};              // cpu_relax() between polls; optional
};

// Ownership of a wake reference on one or more domains. Consumed by put().
struct ForcewakeLease {
    uint32_t slot {UINT32_MAX};
    uint64_t generation {};
    uint8_t domains {};
};

struct ForcewakeDiagnostics {
    ForcewakeStatus lastFailure {ForcewakeStatus::Ok};
    uint8_t failedDomainMask {};
    uint32_t lastAck {};
    uint32_t fallbackInvocations {}, lastFallbackPasses {};
    uint32_t rollbackFailures {};
};

// Gen9 three-domain forcewake: explicit synchronous get/put with reference
// counted leases (intel_uncore_forcewake_get/put without the delayed
// auto-release timer), early sanitize and reset. Every method must be called
// under the owner's exclusive power/reset lock. Not thread-safe by itself.
class Gen9Forcewake {
public:
    static constexpr unsigned LeaseCapacity = 32;
    enum class State : uint8_t { Unbound, Bound, Ready, Faulted };

    Gen9Forcewake() = default;
    Gen9Forcewake(const Gen9Forcewake &) = delete;
    Gen9Forcewake &operator=(const Gen9Forcewake &) = delete;

    // No register access.
    ForcewakeStatus bind(const ForcewakeIo &);
    // intel_uncore_init_mmio() subset: read-only sanity_check_mmio_access(),
    // then __fw_domain_init() x3 and forcewake_early_sanitize(uncore, 0).
    // WRITES masked-disable 0xffff to every request register twice; this is
    // hardware initialization, never a probe.
    ForcewakeStatus initialize(uint64_t deadlineUs);
    // fw_domains_get_with_fallback() for domains whose count is zero. Returns
    // Ok only after every newly requested domain acknowledged FORCEWAKE_KERNEL.
    // Failure releases this call's requests and faults the instance.
    ForcewakeStatus get(uint8_t domains, uint64_t deadlineUs, ForcewakeLease &);
    // __intel_uncore_forcewake_put(delayed = false). Consumes the lease.
    ForcewakeStatus put(ForcewakeLease &);
    // intel_uncore_forcewake_reset(): put active domains, reset all request
    // bits and invalidate every lease. initialize() is required afterwards.
    ForcewakeStatus reset(uint8_t &lostDomains);
    // Refuses (Busy) while hardware domains are requested unless abandoning a
    // device that is already gone. No register access.
    ForcewakeStatus unbind(bool abandonHardwareState);

    State state() const { return state_; }
    uint8_t activeDomains() const { return active_; }
    uint32_t wakeCount(Domain domain) const;
    unsigned liveLeases() const;
    const ForcewakeDiagnostics &diagnostics() const { return diagnostics_; }

private:
    struct LeaseSlot {
        uint64_t generation {1};
        uint8_t domains {};
        bool live {};
    };
    ForcewakeIo io_ {};
    State state_ {State::Unbound};
    uint8_t active_ {};
    uint32_t wakeCount_[DomainCount] {};
    LeaseSlot leases_[LeaseCapacity] {};
    uint64_t lastNowUs_ {};
    ForcewakeDiagnostics diagnostics_ {};

    ForcewakeStatus now(uint64_t &);
    ForcewakeStatus write(uint32_t offset, uint32_t value);
    ForcewakeStatus readAck(const DomainRegisters &, uint32_t &);
    ForcewakeStatus waitAck(const DomainRegisters &, uint32_t bit, uint32_t value,
                            uint64_t deadlineUs, bool &matched);
    ForcewakeStatus ackWithFallback(const DomainRegisters &, bool set,
                                    uint64_t deadlineUs, bool &detected);
    ForcewakeStatus waitKernelAck(const DomainRegisters &, bool set, uint64_t deadlineUs);
    ForcewakeStatus resetRequests();
    ForcewakeStatus fail(ForcewakeStatus, uint8_t domainMask);
    void invalidateLeases();
};

// Device/PM/exclusive-owner authority facts sampled by a native adapter.
struct GlkPciExpectation {
    uint16_t device {};
    uint8_t revision {};
    uint16_t subsystemVendor {}, subsystemDevice {};
    uint8_t bus {}, slot {}, function {};
    uint64_t registryId {}, epoch {};
};
struct GlkAuthoritySnapshot {
    uint16_t vendor {}, device {};
    uint32_t classRevision {};
    uint16_t subsystemVendor {}, subsystemDevice {};
    uint16_t command {};
    uint8_t headerType {}, bus {}, slot {}, function {};
    uint64_t registryId {}, epoch {};
    bool pmCapability {};
    uint16_t pmcsr {0xffff};
    uint32_t bar0Low {}, bar0High {};
    uint64_t mappedPhysical {}, mappedLength {};
    bool providerOpen {}, inactive {true}, powerStateD0 {}, tunneled {};
};
// include/drm/intel/pciids.h INTEL_GLK_IDS.
bool isGlkDeviceId(uint16_t);
bool glkBar0Physical(const GlkAuthoritySnapshot &, uint64_t &);
// Ok only for an expected GLK display-class device in D0 with memory decode,
// opened by its owner, untunneled, with a 64-bit BAR0 mapping covering the
// Gen9 register window at the BAR's physical address. No side effects.
ForcewakeStatus validateAuthority(const GlkPciExpectation &, const GlkAuthoritySnapshot &);

} // namespace MellowNativeGlk

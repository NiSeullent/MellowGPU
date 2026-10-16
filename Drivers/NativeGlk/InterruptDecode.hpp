// SPDX-License-Identifier: MIT
// Mellow native Gemini Lake (Gen9 LP, i915 execlists, no GuC) GT interrupt
// decode/acknowledge/route. Pure register-level logic; the MMIO accessors and
// the IOKit primary-interrupt/workloop binding are supplied by the caller
// (Mellow/GlkInterruptIOKit.*). Nothing here proves GPU execution.
//
// Narrow import scope from torvalds/linux@0d9ff90a5422cc7509258aaaba1e7481df4d332a
// (see Drivers/NativeGlk/interrupt-provenance.json for SHA-256/blob ids):
//   include/drm/intel/intel_gmd_interrupt_regs.h  GEN8_MASTER_IRQ and level bits
//   drivers/gpu/drm/i915/i915_reg.h               GEN8_GT_{ISR,IMR,IIR,IER}, GEN8_*_IRQ_SHIFT,
//                                                 GT_* CS bits, GEN8_PCU_IER
//   drivers/gpu/drm/i915/display/intel_display_regs.h  GEN8_DE_{PIPE,PORT,MISC}_IER (read-only check)
//   drivers/gpu/drm/i915/i915_irq.c               gen8_master_intr_disable/enable, gen8_irq_handler,
//                                                 gen2_irq_reset/gen2_irq_init/gen2_assert_iir_is_zero order
//   drivers/gpu/drm/i915/gt/intel_gt_irq.c        gen8_gt_irq_handler bank routing + exact IIR write-back,
//                                                 gen8_gt_irq_reset, gen8_gt_irq_postinstall enable set
//   drivers/gpu/drm/i915/intel_uncore.c           NEEDS_FORCE_WAKE(): 0x40000..0x115fff needs no forcewake
//   drivers/gpu/drm/i915/i915_pci.c               glk_info/GEN9_LP_FEATURES engine mask RCS0|VCS0|BCS0|VECS0
// Only constants and sequencing were reimplemented; no Linux function text is copied.
//
// Upstream notices preserved for the files above:
//
// i915_irq.c, i915_reg.h:
//  Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
//  All Rights Reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the
//  "Software"), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sub license, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice (including the
//  next paragraph) shall be included in all copies or substantial portions
//  of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
//  IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
//  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// intel_uncore.c: Copyright © 2013 Intel Corporation
// i915_pci.c:     Copyright © 2016 Intel Corporation
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice (including the next
//  paragraph) shall be included in all copies or substantial portions of the
//  Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
//  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
//  IN THE SOFTWARE.
//
// intel_gt_irq.c:              SPDX-License-Identifier: MIT, Copyright © 2019 Intel Corporation
// intel_display_regs.h:        SPDX-License-Identifier: MIT, Copyright © 2025 Intel Corporation
// intel_gmd_interrupt_regs.h:  SPDX-License-Identifier: MIT, Copyright © 2026 Intel Corporation
//  License text: LICENSES/preferred/MIT at the same Linux revision.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace MellowNativeGlk {
namespace Interrupt {
// GEN8_MASTER_IRQ: one register gates GT, display (DE) and PCU delivery.
constexpr uint32_t MasterIrq = 0x44200;
constexpr uint32_t MasterControl = 1U << 31;
constexpr uint32_t MasterPcu = 1U << 30;
constexpr uint32_t MasterDePch = 1U << 23, MasterDeMisc = 1U << 22, MasterDePort = 1U << 20;
constexpr uint32_t MasterDePipeA = 1U << 16, MasterDePipeB = 1U << 17, MasterDePipeC = 1U << 18;
constexpr uint32_t MasterGtVecs = 1U << 6, MasterGtGuc = 1U << 5, MasterGtPm = 1U << 4;
constexpr uint32_t MasterGtVcs1 = 1U << 3, MasterGtVcs0 = 1U << 2;
constexpr uint32_t MasterGtBcs = 1U << 1, MasterGtRcs = 1U << 0;
constexpr uint32_t MasterGtIrqs = 0x7f; // GEN8_GT_IRQS

// GEN8_GT_*(which): bank 0 RCS|BCS, 1 VCS0|VCS1, 2 PM|GuC, 3 VECS.
constexpr unsigned GtBanks = 4;
constexpr uint32_t gtIsr(unsigned bank) { return 0x44300 + 0x10 * bank; }
constexpr uint32_t gtImr(unsigned bank) { return 0x44304 + 0x10 * bank; }
constexpr uint32_t gtIir(unsigned bank) { return 0x44308 + 0x10 * bank; }
constexpr uint32_t gtIer(unsigned bank) { return 0x4430c + 0x10 * bank; }
constexpr uint32_t gtBankMaster(unsigned bank) {
    return bank == 0 ? (MasterGtRcs | MasterGtBcs) : bank == 1 ? (MasterGtVcs0 | MasterGtVcs1) :
        bank == 2 ? (MasterGtPm | MasterGtGuc) : bank == 3 ? MasterGtVecs : 0;
}
// Display/PCU enables are not owned by this driver. They are only read to
// prove the shared master cannot deliver an unowned source; never written.
constexpr unsigned DePipes = 3; // GLK pipes A..C
constexpr uint32_t dePipeIer(unsigned pipe) { return 0x4440c + 0x10 * pipe; }
constexpr uint32_t DePortIer = 0x4444c, DeMiscIer = 0x4446c, PcuIer = 0x444ec;
// Every offset touched here is below 0x116000 and at/above 0x40000, so no
// forcewake is needed (NEEDS_FORCE_WAKE); raw accessors are legal in hard IRQ.
constexpr uint32_t FirstRegister = MasterIrq, LastRegister = PcuIer;

// Per-engine command-streamer bits, shifted by the engine's bank shift.
constexpr uint16_t CsUserInterrupt = 1U << 0;  // GT_RENDER_USER_INTERRUPT
constexpr uint16_t CsMasterError = 1U << 3;    // GT_CS_MASTER_ERROR_INTERRUPT
constexpr uint16_t CsContextSwitch = 1U << 8;  // GT_CONTEXT_SWITCH_INTERRUPT
constexpr uint16_t CsWaitSemaphore = 1U << 11; // GT_WAIT_SEMAPHORE_INTERRUPT
constexpr uint16_t CsExeclistsIrqs = CsUserInterrupt | CsMasterError | CsContextSwitch | CsWaitSemaphore;

// GLK engines (glk_info). VCS1 does not exist on GLK and is never enabled.
enum class Engine : uint8_t { Rcs0, Bcs0, Vcs0, Vecs0 };
constexpr unsigned EngineCount = 4;
constexpr uint8_t engineBit(Engine engine) { return static_cast<uint8_t>(1U << static_cast<unsigned>(engine)); }
constexpr uint8_t GlkEngines = 0x0f;
struct EngineRoute { uint8_t bank, shift; uint32_t master; };
constexpr EngineRoute engineRoute(Engine engine) {
    return engine == Engine::Rcs0 ? EngineRoute {0, 0, MasterGtRcs} :
        engine == Engine::Bcs0 ? EngineRoute {0, 16, MasterGtBcs} :
        engine == Engine::Vcs0 ? EngineRoute {1, 0, MasterGtVcs0} : EngineRoute {3, 0, MasterGtVecs};
}

enum class Status : uint8_t {
    Ok, Empty, Invalid, Unavailable, IoFailure, Unsupported,
    Unowned,        // a non-owned domain (DE/PCU/PM/GuC/unconfigured engine) is enabled or pending
    UnexpectedBits, // a sampled IIR holds bits that were never enabled; nothing acknowledged
    Storm,          // repeated deliveries with nothing to acknowledge
    Faulted
};

// The initial target identity (Lenovo 15IGL7), not a hardware qualification.
// Other GLK SKUs,
// including 8086:3184, have zero qualification and are rejected.
struct DeviceIdentity { uint16_t vendor, device; uint8_t revision; uint16_t subsystemVendor, subsystem; };
constexpr DeviceIdentity FirstHardware {0x8086, 0x3185, 0x06, 0x17aa, 0x3e77};
constexpr bool sameIdentity(const DeviceIdentity &a, const DeviceIdentity &b) {
    return a.vendor == b.vendor && a.device == b.device && a.revision == b.revision &&
        a.subsystemVendor == b.subsystemVendor && a.subsystem == b.subsystem;
}

// Raw BAR0 accessors owned by the PCI/MMIO owner. Each call must be bounded,
// never sleep, allocate, block or take forcewake, and must fail (return false)
// unless the owner epoch still holds D0 and a valid mapping. admitted() is the
// owner's hard-IRQ-safe atomic proof of the same.
struct RegisterOps {
    void *opaque {};
    bool (*admitted)(void *, uint64_t epoch) {};
    bool (*read32)(void *, uint32_t offset, uint32_t &value) {};
    bool (*write32)(void *, uint32_t offset, uint32_t value) {};
};

struct Configuration {
    uint64_t epoch {};
    DeviceIdentity identity {};
    uint8_t engines {};                   // engineBit() set, subset of GlkEngines
    uint16_t csIrqs {CsExeclistsIrqs};    // per-engine enable set, subset of CsExeclistsIrqs
};

// One service pass. events[] are work hints for the serialized workloop:
// user interrupt => re-read the breadcrumb/seqno in coherent memory; context
// switch => process the CSB; wait semaphore / master error => engine registers
// under forcewake. None of them is a completion by itself.
struct Batch {
    uint32_t master {};                   // sampled level bits, control bit clear
    uint32_t iir[GtBanks] {};             // exact sampled value written back (W1C)
    uint16_t events[EngineCount] {};
    bool any() const {
        for (unsigned i = 0; i < EngineCount; ++i) if (events[i]) return true;
        return false;
    }
};

// filter() runs in primary interrupt context: at most two MMIO reads, one
// write and atomics; it never re-enables delivery. Every other method runs on
// one serialized workloop/gate. Stop is two-phase: beginStop() makes filter()
// inert, the caller drains the OS interrupt (IOInterruptEventSource::disable),
// then finishStop() masks hardware with posting-read proof.
class Decoder {
public:
    enum State : uint32_t { Unconfigured, Configured, Armed, Pending, Servicing, Stopping, Fault };
    static constexpr uint32_t StormLimit = 64;
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    Status configure(const Configuration &, const RegisterOps &);
    Status arm();
    bool filter();
    Status collect(Batch &);
    Status maskEngine(Engine);
    Status rearm();
    uint32_t beginStop();
    Status finishStop();

    uint32_t state() const { return __atomic_load_n(&state_, __ATOMIC_ACQUIRE); }
    Status fault() const { return fault_; }
    uint64_t epoch() const { return config_.epoch; }
    uint8_t engines() const { return engines_; }
    uint32_t enabled(unsigned bank) const { return bank < GtBanks ? enabled_[bank] : 0; }
    uint32_t ownedMaster() const { return owned_; }
    uint32_t unownedSample() const { return unowned_; }
    uint32_t unexpectedIir(unsigned bank) const { return bank < GtBanks ? unexpected_[bank] : 0; }
private:
    Configuration config_ {};
    RegisterOps ops_ {};
    uint32_t state_ {Unconfigured};
    uint32_t sampled_ {};                 // filter -> workloop, atomic
    uint32_t enabled_[GtBanks] {};
    uint32_t owned_ {}, unowned_ {}, unexpected_[GtBanks] {}, empty_ {};
    uint8_t engines_ {};
    Status fault_ {Status::Ok};
    bool admitted() const;
    bool read(uint32_t reg, uint32_t &value) const;
    bool write(uint32_t reg, uint32_t value) const;
    Status fail(Status);
    Status enableMaster();
    Status disableMaster();
    bool resetBank(unsigned bank) const;
    bool initBank(unsigned bank) const;
    void recompute();
};
}
}

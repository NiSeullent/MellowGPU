// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// Engine bodies and their Intel notices: Drivers/NativeGlk/BcsExecution.*.
#pragma once
#include "../Drivers/NativeGlk/BcsExecution.hpp"
#include "../Drivers/NativeGpu/MemoryOwner.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNative {

// First target only: Lenovo 15IGL7, Intel N4120 (Gemini Lake) 8086:3185.
constexpr uint16_t GlkBcsVendor = 0x8086, GlkBcsDevice = 0x3185;
// intel_uncore_setup_mmio(): integrated parts below IP 12.70 map a 2 MiB
// register window at the start of GTTMMADR (BAR 0); the GGTT follows later in
// the same BAR and is never touched here.
constexpr uint64_t GlkRegisterWindowBytes = 2ULL * 1024 * 1024;

#if defined(MELLOW_GLK_BCS_HOSTED)
// Hosted regression boundary only: records ordering, executes no barrier.
void glkBcsCpuFence(MellowNativeGlk::Bcs::Fence);
#elif defined(__x86_64__)
inline void glkBcsCpuFence(MellowNativeGlk::Bcs::Fence kind) {
    if (kind == MellowNativeGlk::Bcs::Fence::Store) __asm__ volatile("sfence" ::: "memory");
    else if (kind == MellowNativeGlk::Bcs::Fence::Load) __asm__ volatile("lfence" ::: "memory");
    else __asm__ volatile("mfence" ::: "memory");
}
#else
#error "GLK BCS register ordering is implemented for x86_64 kernels only"
#endif

// Values from the trusted physical owner's admitted profile, compared with
// live configuration space; never from user input.
struct GlkBcsIdentity {
    uint16_t vendor {}, device {};
    uint8_t revision {};
    uint16_t subsystemVendor {}, subsystem {};
    uint64_t registryId {}, epoch {};
};

// Native owners this adapter depends on. Each is a separate component that
// is not part of this unit; while one is absent the dependent effect refuses.
//  - forcewake: acked FORCEWAKE_GEN9 domain references (glk forcewake owner)
//  - reset: GEN6_GDRST BCS engine reset (GT reset owner)
//  - ggtt: proof that a MemoryOwner mapping is a GGTT binding of exactly the
//    pinned pages at that offset (GGTT PTE owner)
//  - ppgtt: the 4-level PPGTT root lifetime (PPGTT owner)
struct GlkForcewakeOps {
    void *context {};
    bool (*held)(void *, uint64_t epoch, uint32_t domains) {};
};
struct GlkEngineResetOps {
    void *context {};
    bool (*resetBlitter)(void *, uint64_t epoch) {};
};
struct GlkGgttBindingOps {
    void *context {};
    bool (*bound)(void *, const MemoryView &, uint64_t ggtt) {};
};
struct GlkPpgttRootOps {
    void *context {};
    bool (*live)(void *, uint64_t epoch, uint64_t generation, uint64_t pml4Dma) {};
    bool (*hold)(void *, uint64_t epoch, uint64_t generation) {};
    bool (*drop)(void *, uint64_t epoch, uint64_t generation) {};
};
struct GlkBcsAuthorities {
    GlkForcewakeOps forcewake {};
    GlkEngineResetOps reset {};
    GlkGgttBindingOps ggtt {};
    GlkPpgttRootOps ppgtt {};
};

// MemoryOwner allocations (MemoryOwner::map Ok) plus the PPGTT root identity.
struct GlkBcsMemory {
    MemoryHandle statusPage {}, context {}, ring {};
    uint64_t pml4Dma {}, pml4Generation {};
};

// Native BCS0 adapter. attach() admits only an opened, active, identity-
// matched 8086:3185 provider whose memory space and bus mastering the PCI
// owner already enabled (this adapter enables neither), then maps BAR 0
// uncached. Its MMIO port reaches only the BCS0 window [0x22000, 0x23000);
// any other offset is dropped/read as all-ones and counted, so display,
// interrupt, forcewake, GDRST and GGTT registers are unreachable through it.
// Leases are rebuilt from MemoryOwner records and owner callbacks on every
// check. Nothing starts automatically: callers serialize all methods under
// the device owner's sleepable lock, outside interrupt context.
class GlkBcsIOKit {
public:
    enum class Result : uint8_t { Ok, Unavailable, Invalid, Ownership, Busy, Engine };
    GlkBcsIOKit() = default;
    GlkBcsIOKit(const GlkBcsIOKit &) = delete;
    GlkBcsIOKit &operator=(const GlkBcsIOKit &) = delete;

    Result attach(IOPCIDevice *, IOService *owner, const GlkBcsIdentity &, MemoryOwner *,
                  uint64_t memoryOwner, const GlkBcsAuthorities &);
    // Builds typed static leases and attaches the engine. No register effect.
    Result bind(const GlkBcsMemory &, uint32_t initialSeqno);
    Result leaseProbe(MemoryHandle, MellowNativeGlk::Bcs::Lease &out) const;
    MellowNativeGlk::Bcs::Engine &engine() { return engine_; }
    MellowNativeGlk::Bcs::MmioPort mmioPort() { return {this, read32, write32, delay, fence}; }
    // Requires a Detached engine (Engine::detach Ok); releases the BAR map.
    Result detach();
    bool attached() const { return device_ != nullptr; }
    uint32_t windowViolations() const { return violations_; }
    MellowNativeGlk::Bcs::Status lastEngineStatus() const { return lastEngine_; }

private:
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMemoryMap *map_ {};
    volatile uint8_t *mmio_ {};
    GlkBcsIdentity identity_ {};
    MemoryOwner *memory_ {};
    uint64_t memoryOwner_ {};
    GlkBcsAuthorities ops_ {};
    MellowNativeGlk::Bcs::Engine engine_ {};
    MellowNativeGlk::Bcs::Status lastEngine_ {MellowNativeGlk::Bcs::Status::Ok};
    uint32_t violations_ {};

    bool deviceAdmitted() const;
    bool lease(MemoryHandle, MellowNativeGlk::Bcs::LeaseKind, MellowNativeGlk::Bcs::Lease &) const;
    MemoryHandle handle(const MellowNativeGlk::Bcs::Lease &) const;
    static uint32_t read32(void *, uint32_t);
    static void write32(void *, uint32_t, uint32_t);
    static void delay(void *, uint32_t);
    static void fence(void *, MellowNativeGlk::Bcs::Fence);
    static bool forcewakeHeld(void *, uint64_t, uint32_t);
    static bool resetEngine(void *, uint64_t);
    static bool holds(void *, const MellowNativeGlk::Bcs::Lease &);
    static bool retain(void *, const MellowNativeGlk::Bcs::Lease &);
    static bool release(void *, const MellowNativeGlk::Bcs::Lease &);
    static bool synchronize(void *, const MellowNativeGlk::Bcs::Lease &, MellowNativeGlk::Bcs::CacheOp);
};

} // namespace MellowNative

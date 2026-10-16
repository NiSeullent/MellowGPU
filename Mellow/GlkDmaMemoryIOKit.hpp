// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
#pragma once
#include "../Drivers/NativeGpu/MemoryOwner.hpp"
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNative {

// Gemini Lake facts, Linux torvalds/linux@0d9ff90a i915_pci.c GEN9_LP_FEATURES:
// dma_mask_size 39, no LLC, has_snoop, no coherent GGTT. Only the first target
// function ID is admitted here; 0x3184 and other Gen9 LP parts are not.
constexpr uint16_t GlkDmaVendor = 0x8086, GlkDmaDevice = 0x3185;
constexpr uint8_t GlkDmaAddressBits = 39;
// Backing pages are allocated below 2^39 so the device limit never needs a
// bounce page. Page alignment comes from the mask's clear low bits.
constexpr uint64_t GlkDmaPhysicalMask =
    ((1ULL << GlkDmaAddressBits) - 1) & ~(MemoryOwner::PageBytes - 1);

// x86 CPU ordering. OSSynchronizeIO() is an empty function on x86_64 and is
// never used as a barrier. Full = mfence, Store = sfence, Load = lfence, the
// same instructions Linux uses for mb()/wmb()/rmb() on x86_64.
enum class GlkCpuFence : uint8_t { Full, Store, Load };
#if defined(MELLOW_GLK_DMA_HOSTED_FENCE)
// Hosted regression boundary only: records ordering, executes no barrier.
void glkCpuFence(GlkCpuFence);
#elif defined(__x86_64__)
inline void glkCpuFence(GlkCpuFence kind) {
    if (kind == GlkCpuFence::Store) __asm__ volatile("sfence" ::: "memory");
    else if (kind == GlkCpuFence::Load) __asm__ volatile("lfence" ::: "memory");
    else __asm__ volatile("mfence" ::: "memory");
}
#else
#error "GLK DMA CPU ordering is implemented for x86_64 kernels only"
#endif

// How device addresses relate to host pages. Unavailable is the default and
// the result whenever the needed authority is missing or contradicted.
enum class GlkDmaRoute : uint8_t { Unavailable, IommuMapped, DirectPhysical };

// Values come from the trusted physical owner's admitted profile and are
// compared against live configuration space; never from user input.
struct GlkDmaIdentity {
    DeviceIdentity device {};
    uint8_t revision {};
    uint16_t subsystemVendor {}, subsystem {};
};

// Trusted physical owner callbacks, all bound to one nonnull context and the
// owner's shared sleepable lock. Admission includes completed native platform
// initialization and the device's PM/reset/termination lease. This adapter
// explicitly maintains CPU caches instead of assuming coherent CPU backing.
// The real GPU VM/submission owner still supplies correct PTE/cache policy,
// GPU cache flushes and globally visible writes before completion is observed.
struct GlkPhysicalVmOps {
    void *context {};
    bool (*admitted)(void *, IOPCIDevice *, IOService *, const DeviceIdentity &) {};
    MemoryStatus (*map)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, bool writable, GpuMapping &) {};
    MemoryStatus (*unmap)(void *, const DeviceIdentity &, uint64_t owner,
        const MemoryHandle &, const DmaPin &, GpuMapping &) {};
    bool (*retired)(void *, const MemoryView &) {};
    bool (*quiesce)(void *, const DeviceIdentity &) {};
};

// MemoryOwner backend for GLK private pages. Each allocation is a wired,
// zeroed, page-aligned kernel buffer below 2^39 behind one IODMACommand with
// a 39-bit, 4 KiB-segment specification.
//
// Route selection happens before any DMA resource exists:
// - IommuMapped: "iommu-parent" is a live IOMapper object; kMapped with that
//   exact mapper. Each IOVA must translate back to its original page.
// - DirectPhysical: no "iommu-parent" at all, IOMapper::gSystem reads exactly
//   kNoMapper (null, read only; any other value vetoes). Native kBypassed with
//   a null explicit mapper must actually succeed and prepare; unlike forced
//   kUnmapped this mode rejects a selected system mapper. Every segment must
//   equal its original physical page. No independent DMAR/PMR register access
//   is needed or authorized: native IOKit owns that platform policy.
// Anything else, including numeric mapper IDs, returns Unavailable. There is
// no lookup/wait, guessed identity address, global mapper, or bounce fallback;
// a segment that is not the original page fails and is cleaned up.
// The native state is rechecked before/after command acquisition/preparation.
// There is no atomic public gSystem+constructor snapshot API. The initialized
// PCI/platform owner must not reinitialize the system mapper behind an active
// DMA lease; arbitrary concurrent kernel reinitialization is not made bounded
// by these checks. Pending/changed state is rejected, never waited on directly.
//
// GLK has no LLC; i915 maps system pages uncached unless snooping is chosen,
// so caches are maintained explicitly with XNU performOperation (x86: mfence,
// clflush per line through the physmap, mfence) plus explicit mfences:
// pin cleans the zeroed pages before returning, ForDevice cleans CPU writes,
// ForCpu invalidates stale lines after the caller's completion-proof read.
// None of these is a GPU cache flush, TLB invalidation or job completion.
//
// Same lifetime rules as NativeMemoryIOKit: callers serialize on the owner's
// sleepable lock (IODMACommand and cache maintenance may block, no IRQ use),
// failed pins return a cleanup token, failed or unknown DMA completion latches
// permanent quarantine, and nothing frees live backing in a destructor. This
// adapter opens no device, enables no bus mastering, touches no register
// beyond identity configuration reads and claims no GPU acceleration.
class GlkDmaMemoryIOKit {
public:
    GlkDmaMemoryIOKit() = default;
    GlkDmaMemoryIOKit(const GlkDmaMemoryIOKit &) = delete;
    GlkDmaMemoryIOKit &operator=(const GlkDmaMemoryIOKit &) = delete;
    MemoryStatus attach(IOPCIDevice *, IOService *owner, GlkDmaIdentity, MemoryLimits,
                        GlkPhysicalVmOps);
    MemoryBackend backend();
    // Requires MemoryOwner.close Ok first. Retained pins make detach Busy.
    MemoryStatus detach();
    GlkDmaRoute route() const { return route_; }
    uint64_t pinnedBytes() const { return pinnedBytes_; }
    bool attached() const { return device_ != nullptr; }
private:
    struct Resource;
    Resource *resources_[MemoryOwner::MaxAllocations] {};
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMapper *mapper_ {};
    GlkDmaIdentity identity_ {};
    MemoryLimits limits_ {};
    GlkPhysicalVmOps vm_ {};
    GlkDmaRoute route_ {GlkDmaRoute::Unavailable};
    uint64_t pinnedBytes_ {};
    bool stopped_ {};
    static GlkDmaRoute selectRoute(IOPCIDevice *, IOMapper *&);
    bool routeHolds() const;
    bool observe() const;
    Resource *checked(const DeviceIdentity &, uint64_t, const MemoryHandle &,
                      const DmaPin &, bool complete) const;
    static void describe(Resource &, DmaPin &);
    MemoryStatus releaseResource(Resource &);
    static bool admitted(void *, const DeviceIdentity &);
    static MemoryStatus pin(void *, const DeviceIdentity &, uint64_t,
                            const MemoryHandle &, uint64_t, DmaPin &);
    static MemoryStatus unpin(void *, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &, DmaPin &);
    static MemoryStatus synchronize(void *, const DeviceIdentity &, uint64_t,
                                    const MemoryHandle &, const DmaPin &, CacheDirection);
    static MemoryStatus map(void *, const DeviceIdentity &, uint64_t,
                            const MemoryHandle &, const DmaPin &, bool, GpuMapping &);
    static MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t,
                              const MemoryHandle &, const DmaPin &, GpuMapping &);
    static bool retired(void *, const MemoryView &);
    static bool quiesce(void *, const DeviceIdentity &);
};

} // namespace MellowNative

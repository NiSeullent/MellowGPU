// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// GGTT sequencing is re-expressed from Linux i915 (MIT); notices, blobs and
// symbols: Drivers/NativeGlk/GgttPublication.hpp and its provenance record.
#pragma once
#include "GlkDmaMemoryIOKit.hpp"
#include "../Drivers/NativeGlk/GgttPublication.hpp"
#include <IOKit/IOMemoryDescriptor.h>

namespace MellowNative {

// Exactly one uncached access per call. x86_64 kernels emit a single
// movq/movl (gen8_set_pte writeq, intel_uncore 32-bit _fw accessors); the
// hosted regression build routes them to an injected device model instead.
#if defined(MELLOW_GLK_GGTT_HOSTED_IO)
void glkGgttStore64(volatile uint64_t *, uint64_t);
uint64_t glkGgttLoad64(const volatile uint64_t *);
void glkGgttStore32(volatile uint32_t *, uint32_t);
uint32_t glkGgttLoad32(const volatile uint32_t *);
#elif defined(__x86_64__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "GSM entries are little-endian");
inline void glkGgttStore64(volatile uint64_t *at, uint64_t value) {
    __asm__ volatile("movq %1, %0" : "=m"(*at) : "r"(value) : "memory");
}
inline uint64_t glkGgttLoad64(const volatile uint64_t *at) {
    uint64_t value;
    __asm__ volatile("movq %1, %0" : "=r"(value) : "m"(*at) : "memory");
    return value;
}
inline void glkGgttStore32(volatile uint32_t *at, uint32_t value) {
    __asm__ volatile("movl %1, %0" : "=m"(*at) : "r"(value) : "memory");
}
inline uint32_t glkGgttLoad32(const volatile uint32_t *at) {
    uint32_t value;
    __asm__ volatile("movl %1, %0" : "=r"(value) : "m"(*at) : "memory");
    return value;
}
#else
#error "GLK GGTT MMIO is implemented for x86_64 kernels only"
#endif

struct GlkGgttConsumerView {
    DeviceIdentity device {};
    uint64_t owner {};
    MemoryHandle handle {};
    uint64_t ggttOffset {}, bytes {};
};

// Native producers this adapter consumes and does NOT implement. Each is
// bound to one nonnull context and the owner's shared sleepable lock.
// - admitted: the physical owner's lease (platform init, PM/reset/termination).
// - handover: the display/firmware/stolen owner's complete record of retained
//   GGTT ranges plus the pool handed to the native owner, for this epoch.
// - acquireGt/releaseGt: FORCEWAKE_ALL acknowledged and GT reset excluded
//   (pending forcewake owner; never emulated by a register write here).
// - awakeEngines: actual engine runtime-PM wakerefs observed under the held
//   GT token. Owner serializes engine wake/resume invalidation until release;
//   missing observation is Unavailable, never an assumed all/zero mask.
// - retired: the queue/engine owner proves no current or future consumer.
// - quiesce: actual engine stop/reset/DMA idle for the epoch.
struct GlkGgttAuthority {
    void *context {};
    bool (*admitted)(void *, IOPCIDevice *, IOService *, const DeviceIdentity &) {};
    bool (*handover)(void *, const DeviceIdentity &, uint64_t ggttTotal,
                     Mellow::NativeGlk::GgttHandover &) {};
    bool (*acquireGt)(void *, const DeviceIdentity &, uint64_t &token) {};
    bool (*releaseGt)(void *, const DeviceIdentity &, uint64_t token) {};
    bool (*retired)(void *, const GlkGgttConsumerView &) {};
    bool (*quiesce)(void *, const DeviceIdentity &) {};
    // Native runtime-PM observation bound to the actual acquired GT token;
    // analogous to intel_engine_pm_is_awake, not a caller mask/boolean.
    bool (*awakeEngines)(void *, const DeviceIdentity &, uint64_t token, uint32_t &mask) {};
};

// Native GGTT publisher for the GLK 8086:3185 function. attach proves the
// tagged GTTMMADR identity (config BAR0 64-bit non-prefetchable memory BAR
// with decode enabled, equal to the IOPCIDevice BAR0 IODeviceMemory base and
// exactly 16 MiB), derives the GSM from SNB_GMCH_CTRL, and maps exactly two
// uncached windows: the GSM (BAR0 + 8 MiB, 2/4/8 MiB) and the 4 KiB register
// page holding the four engine TLB controls. Never write-combined: Gen9 LP
// drops GSM writes wider than 64 bits. Register access is limited to those
// four controls; nothing else in the BAR is reachable through this adapter.
//
// vmOps() supplies GlkDmaMemoryIOKit's GlkPhysicalVmOps: map reserves a
// native-owned extent and publishes the pin's original DMA pages through
// GgttPublisher; unmap needs retirement (or epoch quiescence) and restores
// the captured entries with a completed invalidation. Unavailable from map
// guarantees no entry changed; uncertain results keep the extent, capture
// and binding forever and report IoFailure/Quarantined, so the backing pages
// stay pinned. Missing producers refuse effects; nothing is emulated.
//
// No autoattach, no BAR enable, no bus mastering, no PCI configuration write.
// Callers serialize on the owner's sleepable lock; no IRQ context use.
class GlkGgttIOKit {
public:
    GlkGgttIOKit() = default;
    GlkGgttIOKit(const GlkGgttIOKit &) = delete;
    GlkGgttIOKit &operator=(const GlkGgttIOKit &) = delete;
    MemoryStatus attach(IOPCIDevice *, IOService *owner, GlkDmaIdentity, GlkGgttAuthority);
    GlkPhysicalVmOps vmOps();
    // Busy with live bindings, Quarantined while any extent is held.
    MemoryStatus detach();
    bool attached() const { return device_ != nullptr; }
    uint64_t barBase() const { return barBase_; }
    const Mellow::NativeGlk::GgttGeometry &geometry() const { return geometry_; }
    const Mellow::NativeGlk::GgttPublisher &publisher() const { return publisher_; }
    uint32_t liveBindings() const;

private:
    struct Binding {
        bool used {}, quarantined {};
        DeviceIdentity device {};
        uint64_t owner {};
        MemoryHandle handle {};
        void *pinCookie {};
        const uint64_t *pages {};
        size_t pageCount {};
        uint64_t bytes {};
        Mellow::NativeGlk::GgttExtent extent {};
        uint64_t *capture {};
    };
    Binding bindings_[MemoryOwner::MaxAllocations] {};
    Mellow::NativeGlk::GgttPublisher publisher_ {};
    Mellow::NativeGlk::GgttGeometry geometry_ {};
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOMemoryMap *gsmMap_ {}, *registerMap_ {};
    volatile uint64_t *gsm_ {};
    volatile uint32_t *registers_ {};
    GlkDmaIdentity identity_ {};
    GlkGgttAuthority authority_ {};
    uint64_t barBase_ {};
    uint32_t barLow_ {}, barHigh_ {};
    uint16_t gmchControl_ {};
    bool stopped_ {};

    bool observe() const;
    void unwind();
    Binding *binding(const DeviceIdentity &, uint64_t owner, const MemoryHandle &,
                     const DmaPin &, const GpuMapping &);
    static bool mappingHolds(IOMemoryMap *, uint64_t physical, uint64_t bytes);
    static bool engineControl(uint32_t offset);
    static bool writeEntry(void *, uint64_t, const uint8_t (&)[8]);
    static bool readEntry(void *, uint64_t, uint8_t (&)[8]);
    static bool writeRegister(void *, uint32_t, uint32_t);
    static bool readRegister(void *, uint32_t, uint32_t &);
    static void fence(void *, Mellow::NativeGlk::GgttFence);
    static bool now(void *, uint64_t &);
    static void pause(void *, uint32_t);
    static bool acquireGt(void *, uint64_t &);
    static bool releaseGt(void *, uint64_t);
    static bool awakeEngines(void *, uint64_t token, uint32_t &mask);
    static bool admitted(void *, IOPCIDevice *, IOService *, const DeviceIdentity &);
    static MemoryStatus map(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &,
                            const DmaPin &, bool, GpuMapping &);
    static MemoryStatus unmap(void *, const DeviceIdentity &, uint64_t, const MemoryHandle &,
                              const DmaPin &, GpuMapping &);
    static bool retired(void *, const MemoryView &);
    static bool quiesce(void *, const DeviceIdentity &);
};

} // namespace MellowNative

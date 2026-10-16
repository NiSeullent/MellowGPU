// Copyright (c) 2026 Mellow contributors. Repository LICENSE applies.
// GLK GT interrupt delivery: IOFilterInterruptEventSource on the GLK owner's
// IOWorkLoop, bound to Drivers/NativeGlk/InterruptDecode. This object is only
// constructed by the native GLK owner; nothing here loads, matches, activates
// or powers a driver, and an interrupt is never reported as GPU completion.
#pragma once
#include "../Drivers/NativeGlk/InterruptDecode.hpp"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowNativeGlk {
enum class Handling : uint8_t { Done, More, Failed };
struct InterruptHandlers {
    void *opaque {};
    // Workloop (gate held), hardware delivery still masked. bits are
    // Interrupt::Cs* hints; completion requires the caller's memory checks.
    // More: same bits are offered again on a later workloop pass.
    // Failed: that engine's sources are masked and fault() reports it.
    // Callbacks may request asynchronous owner stop but must not synchronously
    // mutate this registration: arm/quiesce/resume/detach reject reentrancy.
    Handling (*engine)(void *, uint64_t epoch, Interrupt::Engine, uint16_t bits) {};
    // Workloop. engines == 0xff: device delivery is masked/fail-closed.
    void (*fault)(void *, uint64_t epoch, Interrupt::Status, uint8_t engines) {};
};
}

class MellowGlkInterrupt : public OSObject {
    OSDeclareDefaultStructors(MellowGlkInterrupt);
public:
    using Status = MellowNativeGlk::Interrupt::Status;
    static constexpr unsigned MorePassLimit = 1024;
    static MellowGlkInterrupt *create();
    // First provider interrupt index reporting a PCI-messaged (MSI) type, or -1.
    static int messagedIndex(IOPCIDevice *);

    // Every call: the owner's workloop gate is held, the owner is the opened
    // client of this exact 8086:3185 rev 06 17aa:3e77 00:02.0 device in D0
    // with memory decode and bus mastering enabled. attach() masks and
    // programs the GT banks before registering the source, which stays
    // disabled until arm(). A successful attach holds references on the
    // device, owner, workloop and this object until a proven detach().
    Status attach(IOPCIDevice *, IOService *owner, IOWorkLoop *, int index,
                  const MellowNativeGlk::Interrupt::Configuration &,
                  const MellowNativeGlk::Interrupt::RegisterOps &,
                  const MellowNativeGlk::InterruptHandlers &);
    Status arm();
    // Filter made inert, OS interrupt disabled (IOInterruptController drains
    // an in-flight primary handler), then hardware masked with posting-read
    // proof. Registration and references are kept. Required before D3/reset.
    Status quiesce();
    // After quiesce() and a fresh owner epoch (e.g. back in D0).
    Status resume(const MellowNativeGlk::Interrupt::Configuration &,
                  const MellowNativeGlk::Interrupt::RegisterOps &);
    // quiesce() + removeEventSource (unregisters the vector) + release. If
    // the hardware mask cannot be proven, nothing is released: the disabled
    // source stays registered and every reference stays held for a retry.
    Status detach();

    Status lastStatus() const { return last_; }
    // Includes a partial native acquisition retained after uncertain masking.
    bool attached() const { return device_ != nullptr; }
    bool faulted() const { return faulted_; }
    uint64_t deliveries() const { return deliveries_; }
    const MellowNativeGlk::Interrupt::Decoder &decoder() const { return decoder_; }
    void free() override;
private:
    IOPCIDevice *device_ {};
    IOService *owner_ {};
    IOWorkLoop *loop_ {};
    IOFilterInterruptEventSource *source_ {};
    MellowNativeGlk::Interrupt::Decoder decoder_ {};
    MellowNativeGlk::InterruptHandlers handlers_ {};
    MellowNativeGlk::Interrupt::Batch batch_ {};
    MellowNativeGlk::Interrupt::RegisterOps ops_ {};
    uint64_t registryId_ {}, epoch_ {}, deliveries_ {};
    unsigned morePasses_ {};
    int index_ {-1};
    uint8_t nextEngine_ {};
    bool selfRetained_ {}, enabled_ {}, resume_ {}, faulted_ {}, sourceAdded_ {}, handling_ {};
    Status last_ {Status::Empty};
    bool proven() const;
    void dropReferences();
    void failClosed(Status);
    Status mask();
    static bool filter(OSObject *, IOFilterInterruptEventSource *);
    static void action(OSObject *, IOInterruptEventSource *, int);
};

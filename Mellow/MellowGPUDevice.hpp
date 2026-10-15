// SPDX-License-Identifier: MIT
#pragma once
#include "../Drivers/Core/MellowGPUABI.h"
#include "../Drivers/Core/DeviceLifecycle.hpp"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace MellowGPUCore {
struct ControlNativeState;
class MellowGPUDevice : public IOService {
    OSDeclareDefaultStructors(MellowGPUDevice)
public:
    bool init(OSDictionary * = nullptr) override;
    IOService *probe(IOService *, SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
    bool beginOperation(LeaseToken &);
    void endOperation(const LeaseToken &);
    bool openSession(uint64_t &, uint64_t &);
    void closeSession();
    MellowGPUControlStatus reserveBacking(uint64_t);
    void returnBacking(uint64_t);
    void describe(MellowGPUControlReply &);
    bool controlActive();
private:
    IOLock *lock_ {};
    ControlNativeState *native_ {};
    uint64_t registry_ {}, epoch_ {}, nextSession_ {1}, backingBytes_ {};
    uint32_t vendor_ {}, device_ {}, classRevision_ {}, subsystem_ {}, bdf_ {}, clients_ {};
    bool superStarted_ {}, stopping_ {}, ready_ {};
    bool failStart(IOService *);
};

class MellowGPUClient : public IOUserClient {
    OSDeclareDefaultStructors(MellowGPUClient)
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **) override;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch * = nullptr,
                           OSObject * = nullptr, void * = nullptr) override;
private:
    struct Slot {
        // These descriptors/maps never escape this private client. All mappings
        // must acknowledge unmap before descriptor release and quota credit.
        IOBufferMemoryDescriptor *memory {};
        IOMemoryMap *map {};
        uint64_t object {}, bytes {};
        uint32_t state {MellowGPUStorageEmpty};
    };
    IOLock *lock_ {};
    MellowGPUDevice *owner_ {};
    task_t task_ {};
    uint64_t session_ {}, epoch_ {}, lastRequest_ {}, nextObject_ {1}, liveBytes_ {};
    Slot slots_[MELLOWGPU_CONTROL_MAX_BUFFERS] {};
    bool superStarted_ {}, closing_ {}, resourceHold_ {};
    bool authorized() const;
    Slot *find(uint64_t);
    MellowGPUControlStatus perform(const MellowGPUControlRequest &, MellowGPUControlReply &);
    MellowGPUControlStatus unmap(Slot &);
    bool cleanup();
    bool empty() const;
    bool dropResourceHold();
};
}

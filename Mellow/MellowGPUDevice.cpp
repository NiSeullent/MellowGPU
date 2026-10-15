// SPDX-License-Identifier: MIT
// Passive public IOKit control/storage boundary. No GPU VM, DMA or display owner.
#include "MellowGPUDevice.hpp"
#include "../Drivers/Core/ControlProtocol.hpp"
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <libkern/libkern.h>

namespace MellowGPUCore {
OSDefineMetaClassAndStructors(MellowGPUDevice, IOService)
OSDefineMetaClassAndStructors(MellowGPUClient, IOUserClient)

struct ControlNativeState {
    DeviceLifecycle lifecycle;
    ControlNativeState(uint64_t registry, uint64_t epoch) : lifecycle(registry, epoch) {}
    static void *operator new(size_t size) noexcept { return IOMalloc(size); }
    static void operator delete(void *value) noexcept { IOFree(value, sizeof(ControlNativeState)); }
};
namespace {
bool selected() {
    return static_cast<unsigned>(getKernelVersion()) == 24 &&
        checkKernelArgument("-mellowgpudev") && !checkKernelArgument("-mellowoff") &&
        !checkKernelArgument("-mellowdiag") && !checkKernelArgument("-mellownativexe") &&
        !checkKernelArgument("-mellowtahoe") && !checkKernelArgument("-mellowtglwithgfx");
}
bool identity(IOPCIDevice *pci, uint32_t &id, uint32_t &revision) {
    if (!pci || pci->isInactive()) return false;
    id = pci->configRead32(kIOPCIConfigVendorID);
    revision = pci->configRead32(kIOPCIConfigRevisionID);
    return Control::validPciIdentity(id & 0xffffU, id >> 16, revision, pci->getRegistryEntryID());
}
}
bool MellowGPUDevice::init(OSDictionary *properties) {
    if (!IOService::init(properties)) return false;
    // Framework initialization precedes attachment/publication. This lock stays
    // at one address through failed start, stop and the final owner reference.
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}
IOService *MellowGPUDevice::probe(IOService *provider, SInt32 *score) {
    uint32_t id = 0, revision = 0;
    if (!selected() || !identity(OSDynamicCast(IOPCIDevice, provider), id, revision)) return nullptr;
    return IOService::probe(provider, score);
}
bool MellowGPUDevice::start(IOService *provider) {
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool fresh = !superStarted_ && !native_ && !stopping_;
    IOLockUnlock(lock_);
    if (!fresh) return false;
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    uint32_t id = 0, revision = 0;
    if (!selected() || !identity(pci, id, revision) || !IOService::start(provider)) return false;
    const auto registry = pci->getRegistryEntryID();
    const auto epoch = getRegistryEntryID();
    const auto subsystem = pci->configRead32(kIOPCIConfigSubSystemVendorID);
    const auto bdf = (static_cast<uint32_t>(pci->getBusNumber()) << 8) |
        (static_cast<uint32_t>(pci->getDeviceNumber()) << 3) |
        static_cast<uint32_t>(pci->getFunctionNumber());
    auto *native = epoch ? new ControlNativeState(registry, epoch) : nullptr;
    IOLockLock(lock_);
    superStarted_ = true;
    registry_ = registry; epoch_ = epoch; vendor_ = id & 0xffffU; device_ = id >> 16;
    classRevision_ = revision; subsystem_ = subsystem; bdf_ = bdf;
    native_ = native;
    const bool initialized = native_ && !stopping_ && !isInactive();
    IOLockUnlock(lock_);
    if (!initialized) return failStart(provider);
    const bool published = setProperty("MellowGPUQualification", "unqualified") &&
        setProperty("MellowGPUQualificationProfileVersion", static_cast<uint64_t>(1), 32) &&
        setProperty("MellowGPUControlABI", static_cast<uint64_t>(MELLOWGPU_CONTROL_ABI_MAJOR), 32) &&
        setProperty("MellowGPUControlMode", "cpu-storage-only") &&
        setProperty("MellowGPUControlCapabilities", static_cast<uint64_t>(7), 32) &&
        setProperty("MellowGPUPhysicalRegistryID", registry_, 64) &&
        setProperty("MellowGPUPhysicalVendorID", static_cast<uint64_t>(vendor_), 32) &&
        setProperty("MellowGPUPhysicalDeviceID", static_cast<uint64_t>(device_), 32);
    if (!published) return failStart(provider);
    IOLockLock(lock_);
    const bool ready = !stopping_ && !isInactive();
    if (ready) ready_ = true;
    IOLockUnlock(lock_);
    if (!ready) return failStart(provider);
    registerService(kIOServiceAsynchronous);
    return true;
}
bool MellowGPUDevice::failStart(IOService *provider) {
    // A pre-ready reader can still own a reference to this attached service.
    // Close admission but retain its synchronization/state until final free.
    IOLockLock(lock_); stopping_ = true; ready_ = false;
    if (native_) native_->lifecycle.requestStop();
    const bool stopSuper = superStarted_; superStarted_ = false;
    IOLockUnlock(lock_);
    if (stopSuper) IOService::stop(provider);
    return false;
}
void MellowGPUDevice::stop(IOService *provider) {
    bool stopSuper = false;
    if (lock_) {
        IOLockLock(lock_); stopping_ = true; ready_ = false;
        if (native_) native_->lifecycle.requestStop();
        stopSuper = superStarted_; superStarted_ = false;
        IOLockUnlock(lock_);
    }
    // Already admitted CPU calls retain this owner and their native objects.
    // This closes control admission, not another driver's GPU/DMA activity.
    if (stopSuper) IOService::stop(provider);
}
void MellowGPUDevice::free() {
    if (native_) { delete native_; native_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOService::free();
}
bool MellowGPUDevice::controlActive() {
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool active = native_ && ready_ && !stopping_ && !isInactive();
    IOLockUnlock(lock_); return active;
}
bool MellowGPUDevice::beginOperation(LeaseToken &token) {
    token = {};
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool admitted = native_ && ready_ && !stopping_ && !isInactive() &&
        native_->lifecycle.acquire(LeaseKind::NativeOperation, &token) == LifecycleStatus::Success;
    if (admitted) retain();
    IOLockUnlock(lock_); return admitted;
}
void MellowGPUDevice::endOperation(const LeaseToken &token) {
    IOLockLock(lock_);
    const auto result = native_->lifecycle.release(token, Retirement::KnownRetired);
    IOLockUnlock(lock_);
    if (result != LifecycleStatus::Success) {
        // Do not release an owner reference when its private lease is unproven.
        IOLog("MellowGPUControl: invalid operation retirement; owner retained\n");
        return;
    }
    release();
}
bool MellowGPUDevice::openSession(uint64_t &session, uint64_t &epoch) {
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool admitted = native_ && ready_ && !stopping_ && !isInactive() &&
        clients_ < MELLOWGPU_CONTROL_MAX_CLIENTS && nextSession_ != UINT64_MAX;
    if (admitted) { ++clients_; session = nextSession_++; epoch = epoch_; }
    IOLockUnlock(lock_); return admitted;
}
void MellowGPUDevice::closeSession() {
    IOLockLock(lock_); if (clients_) --clients_; IOLockUnlock(lock_);
}
MellowGPUControlStatus MellowGPUDevice::reserveBacking(uint64_t bytes) {
    IOLockLock(lock_);
    MellowGPUControlStatus status = MellowGPUControlOk;
    if (!ready_ || stopping_ || isInactive()) status = MellowGPUControlNotReady;
    else if (bytes > MELLOWGPU_CONTROL_MAX_DEVICE_BYTES - backingBytes_) status = MellowGPUControlCapacity;
    else { backingBytes_ += bytes; retain(); }
    IOLockUnlock(lock_); return status;
}
void MellowGPUDevice::returnBacking(uint64_t bytes) {
    IOLockLock(lock_);
    const bool valid = bytes && bytes <= backingBytes_;
    if (valid) backingBytes_ -= bytes;
    IOLockUnlock(lock_);
    if (valid) release();
    else IOLog("MellowGPUControl: invalid backing credit; owner retained\n");
}
void MellowGPUDevice::describe(MellowGPUControlReply &reply) {
    IOLockLock(lock_);
    reply.registry_id = registry_; reply.vendor_id = vendor_; reply.device_id = device_;
    reply.class_revision = classRevision_; reply.subsystem_id = subsystem_; reply.pci_bdf = bdf_;
    reply.device_backing_bytes = backingBytes_;
    IOLockUnlock(lock_);
}
IOReturn MellowGPUDevice::newUserClient(task_t task, void *security, UInt32 type,
        OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (!task || task != current_task() || type != MELLOWGPU_CONTROL_CONNECT_TYPE ||
        IOUserClient::clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!controlActive()) return kIOReturnNotReady;
    auto *client = new MellowGPUClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) { client->release(); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnNotReady; }
    *handler = client; return kIOReturnSuccess;
}
bool MellowGPUClient::initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) {
    if (!task || task != current_task() || type != MELLOWGPU_CONTROL_CONNECT_TYPE ||
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess ||
        !IOUserClient::initWithTask(task, security, type, properties)) return false;
    task_ = task; task_reference(task_);
    lock_ = IOLockAlloc(); return lock_ != nullptr;
}
bool MellowGPUClient::start(IOService *provider) {
    auto *owner = OSDynamicCast(MellowGPUDevice, provider);
    if (!owner || !IOUserClient::start(provider)) return false;
    superStarted_ = true;
    if (!owner->openSession(session_, epoch_)) { superStarted_ = false; IOUserClient::stop(provider); return false; }
    owner_ = owner; owner_->retain(); return true;
}
bool MellowGPUClient::authorized() const {
    return task_ && current_task() == task_ &&
        clientHasPrivilege(task_, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
}
MellowGPUClient::Slot *MellowGPUClient::find(uint64_t object) {
    for (auto &slot : slots_) if (slot.memory && slot.object == object) return &slot;
    return nullptr;
}
bool MellowGPUClient::empty() const {
    for (const auto &slot : slots_) if (slot.memory || slot.map) return false;
    return true;
}
bool MellowGPUClient::dropResourceHold() {
    if (!resourceHold_ || !empty()) return false;
    resourceHold_ = false; return true;
}
MellowGPUControlStatus MellowGPUClient::unmap(Slot &slot) {
    if (slot.state == MellowGPUStorageQuarantined) return MellowGPUControlQuarantined;
    if (!slot.map) return MellowGPUControlInvalid;
    const IOReturn result = slot.map->unmap();
    if (result != kIOReturnSuccess) {
        // XNU consumes fAddressMap even on error. Never retry a failed unmap and
        // mistake its now-empty second success for actual retirement.
        slot.state = MellowGPUStorageQuarantined; return MellowGPUControlQuarantined;
    }
    slot.map->release(); slot.map = nullptr; slot.state = MellowGPUStorageAllocated;
    return MellowGPUControlOk;
}
MellowGPUControlStatus MellowGPUClient::perform(const MellowGPUControlRequest &request,
        MellowGPUControlReply &reply) {
    if (request.operation == MellowGPUDeviceQuery) return MellowGPUControlOk;
    if (request.operation == MellowGPUStorageCreate) {
        if (request.bytes > MELLOWGPU_CONTROL_MAX_CLIENT_BYTES - liveBytes_ || nextObject_ > UINT32_MAX)
            return MellowGPUControlCapacity;
        Slot *slot = nullptr;
        for (auto &candidate : slots_) if (!candidate.memory) { slot = &candidate; break; }
        if (!slot) return MellowGPUControlCapacity;
        const auto charged = owner_->reserveBacking(request.bytes);
        if (charged != MellowGPUControlOk) return charged;
        auto *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut | kIOMemoryKernelUserShared, request.bytes, MELLOWGPU_CONTROL_PAGE_BYTES);
        if (!memory || !memory->getBytesNoCopy() || memory->getLength() != request.bytes) {
            if (memory) memory->release();
            owner_->returnBacking(request.bytes); return MellowGPUControlNoMemory;
        }
        bzero(memory->getBytesNoCopy(), static_cast<size_t>(request.bytes));
        if (!owner_->controlActive()) {
            memory->release(); owner_->returnBacking(request.bytes); return MellowGPUControlNotReady;
        }
        if (!resourceHold_) { retain(); resourceHold_ = true; }
        slot->memory = memory; slot->bytes = request.bytes; slot->object = nextObject_++;
        slot->state = MellowGPUStorageAllocated; liveBytes_ += slot->bytes;
        reply.object = slot->object; reply.bytes = slot->bytes; reply.storage_state = slot->state;
        return MellowGPUControlOk;
    }
    Slot *slot = find(request.object);
    if (!slot) return MellowGPUControlNotFound;
    reply.object = slot->object;
    if (slot->state == MellowGPUStorageQuarantined) {
        reply.bytes = slot->bytes; reply.storage_state = slot->state;
        return MellowGPUControlQuarantined;
    }
    if (request.operation == MellowGPUStorageClose) {
        if (slot->map) return MellowGPUControlBusy;
        auto *memory = slot->memory; const auto bytes = slot->bytes;
        liveBytes_ -= bytes; *slot = {};
        memory->release(); owner_->returnBacking(bytes); return MellowGPUControlOk;
    }
    if (request.operation == MellowGPUStorageMap) {
        if (slot->map) return MellowGPUControlBusy;
        auto *map = slot->memory->createMappingInTask(task_, 0, kIOMapAnywhere | kIOMapUnique);
        if (!map) return MellowGPUControlNoMemory;
        slot->map = map;
        const bool valid = map->getMemoryDescriptor() == slot->memory &&
            map->getAddressTask() == task_ && map->getLength() == slot->bytes &&
            map->getAddress() && !(map->getAddress() & (MELLOWGPU_CONTROL_PAGE_BYTES - 1));
        if (!valid || !owner_->controlActive()) {
            const auto retired = unmap(*slot);
            if (retired != MellowGPUControlOk) { reply.bytes = slot->bytes; reply.storage_state = slot->state; return retired; }
            return MellowGPUControlNotReady;
        }
        slot->state = MellowGPUStorageMapped;
        reply.object = slot->object; reply.bytes = slot->bytes; reply.storage_state = slot->state;
        reply.cpu_address = map->getAddress(); return MellowGPUControlOk;
    }
    const auto result = unmap(*slot);
    reply.bytes = slot->bytes; reply.storage_state = slot->state;
    return result;
}
IOReturn MellowGPUClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *, OSObject *, void *) {
    if (!authorized()) return kIOReturnNotPrivileged;
    if (!args || args->version != kIOExternalMethodArgumentsCurrentVersion ||
        args->scalarInputCount || args->scalarOutputCount || args->asyncWakePort ||
        args->asyncReferenceCount || args->structureInputDescriptor || args->structureOutputDescriptor ||
        args->structureVariableOutputData || !args->structureInput || !args->structureOutput ||
        args->structureInputSize != sizeof(MellowGPUControlRequest) ||
        args->structureOutputSize != sizeof(MellowGPUControlReply)) return kIOReturnBadArgument;
    MellowGPUControlRequest request {}; MellowGPUControlReply reply {};
    memcpy(&request, args->structureInput, sizeof(request));
    reply.abi_major = MELLOWGPU_CONTROL_ABI_MAJOR; reply.abi_minor = MELLOWGPU_CONTROL_ABI_MINOR;
    reply.struct_size = sizeof(reply); reply.request_id = request.request_id;
    IOLockLock(lock_);
    reply.session = session_; reply.device_epoch = epoch_;
    reply.capabilities = MellowGPUCapDeviceQuery | MellowGPUCapCPUStorage | MellowGPUCapCPUMap;
    reply.mode = MellowGPUControlCPUStorageOnly;
    auto status = Control::validateRequest(request, selector, session_, epoch_, lastRequest_);
    LeaseToken operation {};
    if (status == MellowGPUControlOk) {
        lastRequest_ = request.request_id;
        if (closing_ || !owner_ || !owner_->beginOperation(operation)) status = MellowGPUControlNotReady;
        else { status = perform(request, reply); owner_->endOperation(operation); }
    }
    reply.status = status; reply.client_live_bytes = liveBytes_;
    if (owner_) owner_->describe(reply);
    const bool drop = dropResourceHold();
    IOLockUnlock(lock_);
    memcpy(args->structureOutput, &reply, sizeof(reply)); args->structureOutputSize = sizeof(reply);
    if (drop) release();
    return kIOReturnSuccess;
}
bool MellowGPUClient::cleanup() {
    bool retired = true;
    for (auto &slot : slots_) {
        if (!slot.memory) continue;
        if (slot.state == MellowGPUStorageQuarantined ||
            (slot.map && unmap(slot) != MellowGPUControlOk)) { retired = false; continue; }
        auto *memory = slot.memory; const auto bytes = slot.bytes;
        liveBytes_ -= bytes; slot = {};
        memory->release(); owner_->returnBacking(bytes);
    }
    return retired;
}
IOReturn MellowGPUClient::clientClose() {
    if (!lock_) { terminate(); return kIOReturnSuccess; }
    IOLockLock(lock_); closing_ = true;
    const bool retired = cleanup(); const bool drop = dropResourceHold();
    IOLockUnlock(lock_);
    terminate();
    if (drop) release();
    return retired ? kIOReturnSuccess : kIOReturnError;
}
IOReturn MellowGPUClient::clientDied() { return clientClose(); }
void MellowGPUClient::stop(IOService *provider) {
    bool drop = false;
    if (lock_) { IOLockLock(lock_); closing_ = true; cleanup(); drop = dropResourceHold(); IOLockUnlock(lock_); }
    if (superStarted_) { superStarted_ = false; IOUserClient::stop(provider); }
    if (drop) release();
}
void MellowGPUClient::free() {
    // Live storage acquired a self-hold before publication, so an unknown map
    // cannot arrive here. Its client/task/device/charge stay retained instead.
    if (owner_) { owner_->closeSession(); owner_->release(); owner_ = nullptr; }
    if (task_) { task_deallocate(task_); task_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    IOUserClient::free();
}
IOReturn MellowGPUClient::clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **memory) {
    if (memory) *memory = nullptr;
    return authorized() ? kIOReturnUnsupported : kIOReturnNotPrivileged;
}
}

// SPDX-License-Identifier: MIT
// Executes the actual userspace probe. Public IOKit responses are injected;
// mmap below allocates ordinary test-process RAM only. No GPU is accessed.
#include <IOKit/IOKitLib.h>
#include "../Drivers/Core/MellowGPUABI.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

namespace {
enum class Case { ParentRegistry, Session, Epoch, FirstMapBytes, Registry,
                  SecondMapBytes, HappyStorage };
Case scenario;
unsigned iterations, calls, createCalls, mapCalls, unmapCalls, storageCloses,
         connectionCloses, parentReleases, serviceReleases, iteratorReleases;
void *inaccessible;
alignas(4096) unsigned char storage[MELLOWGPU_CONTROL_PAGE_BYTES];
constexpr uint64_t session = 0x55, epoch = 0x66, registry = 0x1234, object = 0x99;
constexpr io_object_t service = 1, parent = 2, iterator = 3, connection = 4;
bool isHappyMap() {
    return scenario == Case::HappyStorage || (scenario == Case::SecondMapBytes && !mapCalls);
}
}

task_port_t mach_task_self() { return 7; }
CFMutableDictionaryRef IOServiceMatching(const char *name) {
    assert(!strcmp(name, MELLOWGPU_CONTROL_SERVICE));
    return reinterpret_cast<CFMutableDictionaryRef>(1);
}
IOReturn IOServiceGetMatchingServices(mach_port_t port, CFDictionaryRef matching,
                                     io_iterator_t *out) {
    assert(port == kIOMainPortDefault && matching && out);
    *out = iterator; return kIOReturnSuccess;
}
io_object_t IOIteratorNext(io_iterator_t value) {
    assert(value == iterator); return iterations++ ? IO_OBJECT_NULL : service;
}
IOReturn IORegistryEntryGetParentEntry(io_registry_entry_t value, const io_name_t plane,
                                      io_registry_entry_t *out) {
    assert(value == service && !strcmp(plane, kIOServicePlane));
    *out = parent; return kIOReturnSuccess;
}
boolean_t IOObjectConformsTo(io_object_t value, const io_name_t name) {
    assert(value == parent && !strcmp(name, "IOPCIDevice")); return true;
}
IOReturn IORegistryEntryGetRegistryEntryID(io_registry_entry_t value, uint64_t *out) {
    assert(value == parent || value == service);
    *out = value == parent ? (scenario == Case::ParentRegistry ? registry + 1 : registry) : 0x7777;
    return kIOReturnSuccess;
}
IOReturn IOObjectRelease(io_object_t value) {
    if(value == parent) ++parentReleases;
    else if(value == service) ++serviceReleases;
    else { assert(value == iterator); ++iteratorReleases; }
    return kIOReturnSuccess;
}
IOReturn IOServiceOpen(io_service_t value, task_port_t task, uint32_t type, io_connect_t *out) {
    assert(value == service && task == mach_task_self() && type == MELLOWGPU_CONTROL_CONNECT_TYPE);
    *out = connection; return kIOReturnSuccess;
}
IOReturn IOServiceClose(io_connect_t value) {
    assert(value == connection); ++connectionCloses; return kIOReturnSuccess;
}
kern_return_t IOConnectCallStructMethod(mach_port_t value, uint32_t operation,
        const void *input, size_t inputSize, void *output, size_t *outputSize) {
    assert(value == connection && inputSize == sizeof(MellowGPUControlRequest));
    assert(output && outputSize && *outputSize == sizeof(MellowGPUControlReply));
    const auto &q = *static_cast<const MellowGPUControlRequest *>(input);
    assert(q.request_id == ++calls && q.operation == operation && !q.flags);
    assert(q.abi_major == MELLOWGPU_CONTROL_ABI_MAJOR && q.abi_minor == MELLOWGPU_CONTROL_ABI_MINOR);
    assert(q.struct_size == sizeof(q));
    if(operation == MellowGPUDeviceQuery) assert(!q.session && !q.device_epoch && !q.object && !q.bytes);
    else assert(q.session == session && q.device_epoch == epoch);
    auto &r = *static_cast<MellowGPUControlReply *>(output); r = {};
    r.abi_major = MELLOWGPU_CONTROL_ABI_MAJOR; r.abi_minor = MELLOWGPU_CONTROL_ABI_MINOR;
    r.struct_size = sizeof(r); r.status = MellowGPUControlOk; r.request_id = q.request_id;
    r.session = session; r.device_epoch = epoch; r.registry_id = registry;
    r.vendor_id = 0x8086; r.device_id = 0x3185; r.class_revision = 0x03000000;
    r.capabilities = MellowGPUCapDeviceQuery | MellowGPUCapCPUStorage | MellowGPUCapCPUMap;
    r.mode = MellowGPUControlCPUStorageOnly;
    if(operation == MellowGPUStorageCreate) {
        assert(!q.object && q.bytes == sizeof(storage)); ++createCalls;
        r.object = object; r.bytes = sizeof(storage); r.storage_state = MellowGPUStorageAllocated;
        r.client_live_bytes = r.device_backing_bytes = sizeof(storage);
    } else if(operation == MellowGPUStorageMap) {
        assert(q.object == object && !q.bytes);
        const bool happyMap = isHappyMap(); ++mapCalls;
        r.object = object; r.bytes = sizeof(storage); r.storage_state = MellowGPUStorageMapped;
        r.cpu_address = reinterpret_cast<uintptr_t>(happyMap ? storage : inaccessible);
        r.client_live_bytes = r.device_backing_bytes = sizeof(storage);
        if(scenario == Case::Session) ++r.session;
        if(scenario == Case::Epoch) ++r.device_epoch;
        if(scenario == Case::FirstMapBytes || (scenario == Case::SecondMapBytes && mapCalls == 2))
            r.bytes = r.client_live_bytes = r.device_backing_bytes = sizeof(storage) * 2;
        if(scenario == Case::Registry) ++r.registry_id;
    } else if(operation == MellowGPUStorageUnmap) {
        assert(q.object == object && !q.bytes); ++unmapCalls;
        r.object = object; r.bytes = sizeof(storage); r.storage_state = MellowGPUStorageAllocated;
        r.client_live_bytes = r.device_backing_bytes = sizeof(storage);
    } else if(operation == MellowGPUStorageClose) {
        assert(q.object == object && !q.bytes); ++storageCloses; r.object = object;
    } else assert(operation == MellowGPUDeviceQuery);
    *outputSize = sizeof(r); return KERN_SUCCESS;
}

// Include the production main, Client and StorageTest bodies without copying
// their validation. Only the public OS-call declarations above are substituted.
#define main actual_probe_main
#include "../Userspace/mellowgpu-control.cpp"
#undef main

int main() {
    inaccessible = mmap(nullptr, sizeof(storage), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    assert(!(reinterpret_cast<uintptr_t>(inaccessible) & (MELLOWGPU_CONTROL_PAGE_BYTES - 1)));
    char name[] = "probe-control-tests", flag[] = "--selftest";
    char *args[] = {name, flag, nullptr};
    const Case cases[] = {Case::ParentRegistry, Case::Session, Case::Epoch,
                          Case::FirstMapBytes, Case::Registry, Case::SecondMapBytes, Case::HappyStorage};
    for(const auto current : cases) {
        scenario = current; iterations = calls = createCalls = mapCalls = unmapCalls = storageCloses = 0;
        connectionCloses = parentReleases = serviceReleases = iteratorReleases = 0;
        memset(storage, 0, sizeof(storage));
        const bool happy = current == Case::HappyStorage;
        assert(actual_probe_main(2, args) == (happy ? 0 : 1));
        assert(connectionCloses == 1 && parentReleases == 1 && serviceReleases == 1 && iteratorReleases == 1);
        assert(createCalls == (current == Case::ParentRegistry ? 0U : 1U));
        const unsigned maps = happy || current == Case::SecondMapBytes ? 2U : current == Case::ParentRegistry ? 0U : 1U;
        assert(mapCalls == maps);
        assert(unmapCalls == (happy ? 2U : current == Case::SecondMapBytes ? 1U : 0U));
        assert(storageCloses == (happy ? 1U : 0U));
        if(happy) for(unsigned i = 0; i < sizeof(storage); ++i)
            assert(storage[i] == static_cast<unsigned char>((i * 17U) ^ 0x5aU));
    }
    assert(munmap(inaccessible, sizeof(storage)) == 0);
    puts("PASS 7 actual-probe cases; simulated IOKit and test-process CPU RAM only");
}

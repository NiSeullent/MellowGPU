// SPDX-License-Identifier: MIT
// Native control/CPU-storage probe. It never submits GPU work or renders pixels.
#include "../Drivers/Core/ControlProtocol.hpp"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

namespace {
using MellowGPUCore::Control::validateSuccessReply;

struct Client {
    io_connect_t connection {};
    uint64_t sequence {}, session {}, epoch {};
    MellowGPUControlReply identity {};
    IOReturn kernelReturn {kIOReturnSuccess};
    uint32_t nativeStatus {}, wireError {};

    bool call(uint32_t operation,uint64_t object,uint64_t bytes,MellowGPUControlReply &reply) {
        if(sequence==UINT64_MAX) { wireError=1; return false; }
        const MellowGPUControlRequest request {MELLOWGPU_CONTROL_ABI_MAJOR,
            MELLOWGPU_CONTROL_ABI_MINOR,sizeof(MellowGPUControlRequest),operation,
            ++sequence,operation==MellowGPUDeviceQuery?0:session,
            operation==MellowGPUDeviceQuery?0:epoch,object,bytes,0};
        reply={};size_t size=sizeof(reply);
        kernelReturn=IOConnectCallStructMethod(connection,operation,&request,sizeof(request),&reply,&size);
        if(kernelReturn!=kIOReturnSuccess) return false;
        if(size!=sizeof(reply)) { wireError=2; return false; }
        nativeStatus=reply.status;
        if(reply.status!=MellowGPUControlOk) return false;
        if(!validateSuccessReply(request,reply)) { wireError=3; return false; }
        if(operation==MellowGPUDeviceQuery) {
            session=reply.session;epoch=reply.device_epoch;identity=reply;
        } else if(reply.registry_id!=identity.registry_id || reply.vendor_id!=identity.vendor_id ||
                  reply.device_id!=identity.device_id || reply.class_revision!=identity.class_revision ||
                  reply.subsystem_id!=identity.subsystem_id || reply.pci_bdf!=identity.pci_bdf ||
                  reply.capabilities!=identity.capabilities || reply.mode!=identity.mode) {
            wireError=4;return false;
        }
        return true;
    }
};

struct StorageTest {
    bool created {}, firstMap {}, zeroed {}, written {}, firstUnmap {},
         secondMap {}, persisted {}, secondUnmap {}, closed {};
    bool run(Client &client) {
        MellowGPUControlReply reply {};
        constexpr uint64_t bytes=MELLOWGPU_CONTROL_PAGE_BYTES;
        if(!(created=client.call(MellowGPUStorageCreate,0,bytes,reply))) return false;
        const uint64_t object=reply.object;
        if(!(firstMap=client.call(MellowGPUStorageMap,object,0,reply))) return false;
        if(reply.bytes!=bytes) { client.wireError=5;return false; }
        auto *memory=reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(reply.cpu_address));
        zeroed=true;
        for(uint64_t i=0;i<bytes;++i) if(memory[i]) zeroed=false;
        for(uint64_t i=0;i<bytes;++i) memory[i]=static_cast<unsigned char>((i*17U)^0x5aU);
        written=true;
        for(uint64_t i=0;i<bytes;++i)
            if(memory[i]!=static_cast<unsigned char>((i*17U)^0x5aU)) written=false;
        if(!(firstUnmap=client.call(MellowGPUStorageUnmap,object,0,reply))) return false;
        // A new native CPU mapping must observe the same retained object.
        if(!(secondMap=client.call(MellowGPUStorageMap,object,0,reply))) return false;
        if(reply.bytes!=bytes) { client.wireError=5;return false; }
        memory=reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(reply.cpu_address));
        persisted=true;
        for(uint64_t i=0;i<bytes;++i)
            if(memory[i]!=static_cast<unsigned char>((i*17U)^0x5aU)) persisted=false;
        if(!(secondUnmap=client.call(MellowGPUStorageUnmap,object,0,reply))) return false;
        if(!(closed=client.call(MellowGPUStorageClose,object,0,reply))) return false;
        return zeroed && written && persisted;
    }
};

const char *boolean(bool value) { return value?"true":"false"; }
}

int main(int argc,char **argv) {
    bool selftest=false;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--selftest")) selftest=true;
        else if(!strcmp(argv[i],"--help")) {
            puts("Usage: mellowgpu-control [--selftest]\n"
                 "Query MellowGPUDevice services; --selftest tests 4096 bytes of native CPU shared storage.\n"
                 "Requires administrator access when services are present. No GPU submission or rendering.");
            return 0;
        } else { fputs("Unknown option; use --help\n",stderr); return 64; }
    }
    io_iterator_t iterator=IO_OBJECT_NULL;
    const IOReturn matching=IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching(MELLOWGPU_CONTROL_SERVICE),&iterator);
    if(matching!=kIOReturnSuccess) {
        printf("{\"schema\":\"mellowgpu.native-control-probe/1\",\"status\":\"matching_failed\","
               "\"kernel_return\":%u,\"gpu_executed\":false}\n",static_cast<unsigned>(matching));
        return 1;
    }
    unsigned count=0,passed=0;
    printf("{\"schema\":\"mellowgpu.native-control-probe/1\","
           "\"scope\":\"native PCI query and CPU shared storage only\","
           "\"gpu_executed\":false,\"metal3_verified\":false,\"windowserver_verified\":false,\"devices\":[");
    io_service_t service;
    while((service=IOIteratorNext(iterator))) {
        if(count++) putchar(',');
        uint64_t serviceId=0,physicalId=0;
        IORegistryEntryGetRegistryEntryID(service,&serviceId);
        io_registry_entry_t parent=IO_OBJECT_NULL;
        bool physicalParent=false;
        if(IORegistryEntryGetParentEntry(service,kIOServicePlane,&parent)==kIOReturnSuccess) {
            physicalParent=IOObjectConformsTo(parent,"IOPCIDevice") &&
                IORegistryEntryGetRegistryEntryID(parent,&physicalId)==kIOReturnSuccess;
            IOObjectRelease(parent);
        }
        Client client;
        const IOReturn opened=IOServiceOpen(service,mach_task_self(),MELLOWGPU_CONTROL_CONNECT_TYPE,&client.connection);
        MellowGPUControlReply query {};
        const bool queried=opened==kIOReturnSuccess && client.call(MellowGPUDeviceQuery,0,0,query);
        const bool identityMatches=queried && physicalParent && physicalId==query.registry_id;
        StorageTest storage;
        const bool tested=selftest && identityMatches && storage.run(client);
        // Client close owns error cleanup. Never repeat a failed native unmap;
        // its apparent later success cannot prove that the first map retired.
        const IOReturn closed=client.connection?IOServiceClose(client.connection):kIOReturnSuccess;
        const bool ok=identityMatches && (!selftest || tested) && closed==kIOReturnSuccess;
        if(ok) ++passed;
        printf("{\"service_registry_id\":\"0x%llx\",\"pci_registry_id\":\"0x%llx\","
               "\"vendor_id\":%u,\"device_id\":%u,\"class_revision\":%u,\"pci_bdf\":%u,"
               "\"session\":\"0x%llx\",\"device_epoch\":\"0x%llx\",\"capabilities\":%u,"
               "\"query_passed\":%s,\"physical_parent_matches\":%s,"
               "\"storage_selftest_requested\":%s,\"storage_selftest_passed\":%s,"
               "\"storage_steps\":{\"created\":%s,\"first_map\":%s,\"zeroed\":%s,\"written\":%s,"
               "\"first_unmap\":%s,\"second_map\":%s,\"persisted\":%s,\"second_unmap\":%s,\"closed\":%s},"
               "\"open_return\":%u,\"last_kernel_return\":%u,\"last_native_status\":%u,"
               "\"wire_error\":%u,\"close_return\":%u,\"passed\":%s}",
               static_cast<unsigned long long>(serviceId),static_cast<unsigned long long>(physicalId),
               query.vendor_id,query.device_id,query.class_revision,query.pci_bdf,
               static_cast<unsigned long long>(query.session),static_cast<unsigned long long>(query.device_epoch),
               query.capabilities,boolean(queried),boolean(identityMatches),boolean(selftest),boolean(tested),
               boolean(storage.created),boolean(storage.firstMap),boolean(storage.zeroed),boolean(storage.written),
               boolean(storage.firstUnmap),boolean(storage.secondMap),boolean(storage.persisted),
               boolean(storage.secondUnmap),boolean(storage.closed),static_cast<unsigned>(opened),
               static_cast<unsigned>(client.kernelReturn),client.nativeStatus,client.wireError,
               static_cast<unsigned>(closed),boolean(ok));
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    printf("],\"device_count\":%u,\"passed_device_count\":%u,\"status\":\"%s\"}\n",
           count,passed,!count?"no_native_control_service":(passed==count?"passed":"failed"));
    return !count?2:(passed==count?0:1);
}

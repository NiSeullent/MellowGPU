// SPDX-License-Identifier: MIT
// Actual wire validation tests. Native/GPU behavior is not exercised here.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if __has_include("../Drivers/Core/ControlProtocol.hpp")
#include "../Drivers/Core/ControlProtocol.hpp"
using namespace MellowGPUCore::Control;
static unsigned checks;
#define EXPECT(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); exit(42); } } while(0)
static MellowGPUControlRequest query(uint64_t id=1) {
    return {MELLOWGPU_CONTROL_ABI_MAJOR,MELLOWGPU_CONTROL_ABI_MINOR,
        sizeof(MellowGPUControlRequest),MellowGPUDeviceQuery,id,0,0,0,0,0};
}
static MellowGPUControlRequest create() {
    auto r=query(); r.operation=MellowGPUStorageCreate; r.session=20;
    r.device_epoch=30;r.bytes=MELLOWGPU_CONTROL_PAGE_BYTES;return r;
}
static void status(const MellowGPUControlRequest &r,uint32_t selector,MellowGPUControlStatus expected,
                   uint64_t last=0,uint64_t session=20,uint64_t epoch=30) {
    EXPECT(validateRequest(r,selector,session,epoch,last)==expected);
}
int main() {
    EXPECT(sizeof(MellowGPUControlRequest)==64);
    EXPECT(sizeof(MellowGPUControlReply)==128);
    auto r=query();status(r,MellowGPUDeviceQuery,MellowGPUControlOk);
    r.abi_major=0;status(r,0,MellowGPUControlUnsupported);
    r=query();r.abi_minor=1;status(r,0,MellowGPUControlUnsupported);
    r=query();r.struct_size=63;status(r,0,MellowGPUControlInvalid);
    r=query();r.request_id=0;status(r,0,MellowGPUControlInvalid);
    r=query();r.flags=1;status(r,0,MellowGPUControlInvalid);
    r=query();status(r,1,MellowGPUControlInvalid);
    r=query();r.operation=99;status(r,99,MellowGPUControlUnsupported);
    r=query();status(r,0,MellowGPUControlReplay,1);
    r=query(UINT64_MAX);status(r,0,MellowGPUControlOk,UINT64_MAX-1);
    status(r,0,MellowGPUControlReplay,UINT64_MAX);
    r=query();r.session=20;status(r,0,MellowGPUControlInvalid);
    r=query();r.device_epoch=30;status(r,0,MellowGPUControlInvalid);
    r=query();r.object=1;status(r,0,MellowGPUControlInvalid);
    r=query();r.bytes=4096;status(r,0,MellowGPUControlInvalid);
    r=create();status(r,1,MellowGPUControlOk);
    const uint64_t invalidBytes[] = {0,1,4095,4097,UINT64_MAX};
    for (uint64_t value : invalidBytes) {
        r=create();r.bytes=value;status(r,1,MellowGPUControlInvalid);
    }
    r=create();r.bytes=MELLOWGPU_CONTROL_MAX_BUFFER_BYTES;status(r,1,MellowGPUControlOk);
    r.bytes+=4096;status(r,1,MellowGPUControlInvalid);
    r=create();r.object=1;status(r,1,MellowGPUControlInvalid);
    r=create();r.session=21;status(r,1,MellowGPUControlStaleSession);
    r=create();r.session=0;status(r,1,MellowGPUControlStaleSession);
    r=create();status(r,1,MellowGPUControlStaleSession,0,0);
    r=create();r.device_epoch=31;status(r,1,MellowGPUControlStaleEpoch);
    r=create();r.device_epoch=0;status(r,1,MellowGPUControlStaleEpoch);
    r=create();status(r,1,MellowGPUControlStaleEpoch,0,20,0);
    for(uint32_t operation=MellowGPUStorageClose;operation<=MellowGPUStorageUnmap;++operation) {
        r=create();r.operation=operation;r.object=8;r.bytes=0;
        status(r,operation,MellowGPUControlOk);
        r.object=0;status(r,operation,MellowGPUControlInvalid);
        r.object=8;r.bytes=4096;status(r,operation,MellowGPUControlInvalid);
        r.bytes=0;r.flags=2;status(r,operation,MellowGPUControlInvalid);
        r.flags=0;r.session=19;status(r,operation,MellowGPUControlStaleSession);
        r.session=20;r.device_epoch=29;status(r,operation,MellowGPUControlStaleEpoch);
    }
    EXPECT(validPciIdentity(0x8086,0x3185,0x03000001,50));
    EXPECT(validPciIdentity(0x1002,0x7550,0x030000c1,51));
    EXPECT(validPciIdentity(0x10de,0x2684,0x030200a1,52));
    EXPECT(!validPciIdentity(0x1234,0x3185,0x03000001,50));
    EXPECT(!validPciIdentity(0x18086,0x3185,0x03000001,50));
    EXPECT(!validPciIdentity(0x8086,0,0x03000001,50));
    EXPECT(!validPciIdentity(0x8086,0xffff,0x03000001,50));
    EXPECT(!validPciIdentity(0x8086,0x13185,0x03000001,50));
    EXPECT(!validPciIdentity(0x8086,0x3185,0x02000001,50));
    EXPECT(!validPciIdentity(0x8086,0x3185,0xffffffff,50));
    EXPECT(!validPciIdentity(0x8086,0x3185,0x03000001,0));
    // Raw unaligned user bytes are copied by IOKit before validation. Test every
    // request byte against the canonical request without dereferencing a wire pointer.
    const auto original=query();
    for(size_t index=0;index<sizeof(original);++index) {
        r=original; reinterpret_cast<unsigned char *>(&r)[index]^=0x80;
        const auto result=validateRequest(r,0,20,30,0);
        if(index>=16 && index<24) EXPECT(result==MellowGPUControlOk);
        else EXPECT(result!=MellowGPUControlOk);
    }
#if defined(MELLOWGPU_CONTROL_REPLY_VALIDATION)
    auto replyFor=[](const MellowGPUControlRequest &request) {
        MellowGPUControlReply p {};
        p.abi_major=1;p.struct_size=sizeof(p);p.request_id=request.request_id;
        p.session=20;p.device_epoch=30;p.registry_id=50;p.vendor_id=0x8086;
        p.device_id=0x3185;p.class_revision=0x03000001;p.pci_bdf=0x10;
        p.mode=MellowGPUControlCPUStorageOnly;p.capabilities=7;
        p.client_live_bytes=p.device_backing_bytes=4096;
        if(request.operation==MellowGPUStorageCreate) {
            p.object=3;p.bytes=request.bytes;p.storage_state=MellowGPUStorageAllocated;
        } else if(request.operation!=MellowGPUDeviceQuery) {
            p.object=request.object;
            if(request.operation!=MellowGPUStorageClose) {
                p.bytes=4096;p.storage_state=MellowGPUStorageAllocated;
            }
            if(request.operation==MellowGPUStorageMap) {
                p.storage_state=MellowGPUStorageMapped;p.cpu_address=0x100000;
            }
        }
        return p;
    };
    r=query();auto p=replyFor(r);EXPECT(validateSuccessReply(r,p));
#define BAD_REPLY(field,value) do { p=replyFor(r);p.field=(value);EXPECT(!validateSuccessReply(r,p)); } while(0)
    BAD_REPLY(abi_major,2);BAD_REPLY(abi_minor,1);BAD_REPLY(struct_size,127);
    BAD_REPLY(status,MellowGPUControlUnsupported);BAD_REPLY(request_id,2);
    BAD_REPLY(session,0);BAD_REPLY(device_epoch,0);BAD_REPLY(registry_id,0);
    BAD_REPLY(vendor_id,0x1234);BAD_REPLY(class_revision,0x02000001);
    BAD_REPLY(device_id,0);BAD_REPLY(pci_bdf,0x10000);BAD_REPLY(mode,0);
    BAD_REPLY(capabilities,8);BAD_REPLY(capabilities,0);BAD_REPLY(reserved,1);
    BAD_REPLY(client_live_bytes,MELLOWGPU_CONTROL_MAX_CLIENT_BYTES+1);
    BAD_REPLY(device_backing_bytes,MELLOWGPU_CONTROL_MAX_DEVICE_BYTES+1);
    BAD_REPLY(device_backing_bytes,0);
    BAD_REPLY(object,1);BAD_REPLY(bytes,4096);BAD_REPLY(cpu_address,0x100000);
    BAD_REPLY(storage_state,MellowGPUStorageAllocated);
    r=create();p=replyFor(r);EXPECT(validateSuccessReply(r,p));
    BAD_REPLY(session,21);BAD_REPLY(device_epoch,31);BAD_REPLY(object,0);
    BAD_REPLY(bytes,8192);BAD_REPLY(storage_state,MellowGPUStorageMapped);
    BAD_REPLY(cpu_address,0x100000);BAD_REPLY(capabilities,MellowGPUCapDeviceQuery);
    BAD_REPLY(client_live_bytes,0);
    for(uint32_t op=MellowGPUStorageClose;op<=MellowGPUStorageUnmap;++op) {
        r=create();r.operation=op;r.bytes=0;r.object=3;
        p=replyFor(r);EXPECT(validateSuccessReply(r,p));
        BAD_REPLY(object,4);BAD_REPLY(session,21);BAD_REPLY(device_epoch,31);
        if(op==MellowGPUStorageClose) {
            BAD_REPLY(bytes,4096);BAD_REPLY(storage_state,MellowGPUStorageAllocated);
            BAD_REPLY(cpu_address,0x100000);
        } else if(op==MellowGPUStorageMap) {
            BAD_REPLY(cpu_address,0);BAD_REPLY(cpu_address,0x100001);
            BAD_REPLY(cpu_address,0xfffffffffffff000ULL);
            BAD_REPLY(cpu_address,0x800000000000ULL);
            BAD_REPLY(storage_state,MellowGPUStorageAllocated);BAD_REPLY(bytes,4095);
            BAD_REPLY(capabilities,MellowGPUCapDeviceQuery|MellowGPUCapCPUStorage);
        } else {
            BAD_REPLY(cpu_address,0x100000);BAD_REPLY(bytes,0);
            BAD_REPLY(storage_state,MellowGPUStorageMapped);
        }
    }
#undef BAD_REPLY
#else
    EXPECT(!"success reply validation is missing");
#endif
    printf("PASS %u control-wire checks; no native or GPU execution\n",checks);
}
#else
int main() { fputs("FAIL: native control wire validator is missing\n",stderr); return 42; }
#endif

/* SPDX-License-Identifier: MIT
 * Original MellowGPU control ABI. No upstream driver ABI is copied.
 * CPU shared storage does not imply GPU mappings or accelerated rendering.
 */
#ifndef MELLOWGPU_CONTROL_ABI_H
#define MELLOWGPU_CONTROL_ABI_H
#include <stdint.h>
#include <stddef.h>

#define MELLOWGPU_CONTROL_ABI_MAJOR 1U
#define MELLOWGPU_CONTROL_ABI_MINOR 0U
#define MELLOWGPU_CONTROL_CONNECT_TYPE 0x4d470100U
#define MELLOWGPU_CONTROL_SERVICE "MellowGPUDevice"
#define MELLOWGPU_CONTROL_PAGE_BYTES 4096ULL
#define MELLOWGPU_CONTROL_MAX_BUFFER_BYTES (16ULL * 1024ULL * 1024ULL)
#define MELLOWGPU_CONTROL_MAX_CLIENT_BYTES (64ULL * 1024ULL * 1024ULL)
#define MELLOWGPU_CONTROL_MAX_DEVICE_BYTES (256ULL * 1024ULL * 1024ULL)
#define MELLOWGPU_CONTROL_MAX_CLIENTS 16U
#define MELLOWGPU_CONTROL_MAX_BUFFERS 16U

enum MellowGPUControlOperation {
    MellowGPUDeviceQuery=0, MellowGPUStorageCreate=1, MellowGPUStorageClose=2,
    MellowGPUStorageMap=3, MellowGPUStorageUnmap=4
};
enum MellowGPUControlStatus {
    MellowGPUControlOk=0, MellowGPUControlInvalid=1, MellowGPUControlUnsupported=2,
    MellowGPUControlStaleSession=3, MellowGPUControlStaleEpoch=4,
    MellowGPUControlReplay=5, MellowGPUControlNotReady=6,
    MellowGPUControlCapacity=7, MellowGPUControlNoMemory=8,
    MellowGPUControlNotFound=9, MellowGPUControlForbidden=10,
    MellowGPUControlBusy=11, MellowGPUControlQuarantined=12
};
enum MellowGPUControlCapability {
    MellowGPUCapDeviceQuery=1, MellowGPUCapCPUStorage=2, MellowGPUCapCPUMap=4
};
enum MellowGPUControlStorageState {
    MellowGPUStorageEmpty=0, MellowGPUStorageAllocated=1,
    MellowGPUStorageMapped=2, MellowGPUStorageQuarantined=3
};
enum MellowGPUControlMode { MellowGPUControlCPUStorageOnly=1 };

typedef struct MellowGPUControlRequest {
    uint32_t abi_major,abi_minor,struct_size,operation;
    uint64_t request_id,session,device_epoch,object,bytes,flags;
} MellowGPUControlRequest;

typedef struct MellowGPUControlReply {
    uint32_t abi_major,abi_minor,struct_size,status;
    uint64_t request_id,session,device_epoch,object,bytes;
    uint32_t storage_state,capabilities;
    uint64_t registry_id,client_live_bytes,device_backing_bytes;
    uint32_t vendor_id,device_id,class_revision,subsystem_id,pci_bdf,mode;
    uint64_t cpu_address,reserved;
} MellowGPUControlReply;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error MellowGPU revision 1 control ABI requires a little-endian host
#endif
#if defined(__cplusplus)
#define MELLOWGPU_ABI_ASSERT(c,m) static_assert(c,m)
#else
#define MELLOWGPU_ABI_ASSERT(c,m) _Static_assert(c,m)
#endif
MELLOWGPU_ABI_ASSERT(sizeof(MellowGPUControlRequest)==64,"control request size");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlRequest,request_id)==16,"request id offset");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlRequest,flags)==56,"flags offset");
MELLOWGPU_ABI_ASSERT(sizeof(MellowGPUControlReply)==128,"control reply size");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlReply,object)==40,"reply object offset");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlReply,registry_id)==64,"PCI registry offset");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlReply,cpu_address)==112,"CPU address offset");
MELLOWGPU_ABI_ASSERT(offsetof(MellowGPUControlReply,reserved)==120,"reserved offset");
#undef MELLOWGPU_ABI_ASSERT
#endif

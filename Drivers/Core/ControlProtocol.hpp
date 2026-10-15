// SPDX-License-Identifier: MIT
// Original wire validation. Native task authorization is enforced by IOKit.
#pragma once
#include "MellowGPUABI.h"

namespace MellowGPUCore { namespace Control {

inline bool validPciIdentity(uint32_t vendor,uint32_t device,uint32_t classRevision,
                             uint64_t registryId) {
    return (vendor==0x8086U || vendor==0x1002U || vendor==0x10deU) &&
        device && device<0xffffU && (classRevision>>24)==0x03U && registryId;
}

// The adapter copies untrusted wire bytes into an aligned local first. This
// function does not authenticate a task, consume request IDs or call a backend.
inline MellowGPUControlStatus validateRequest(const MellowGPUControlRequest &r,
        uint32_t selector,uint64_t expectedSession,uint64_t expectedEpoch,
        uint64_t lastRequestId) {
    if(r.abi_major!=MELLOWGPU_CONTROL_ABI_MAJOR ||
       r.abi_minor!=MELLOWGPU_CONTROL_ABI_MINOR) return MellowGPUControlUnsupported;
    if(r.struct_size!=sizeof(r) || !r.request_id || r.flags ||
       r.operation!=selector) return MellowGPUControlInvalid;
    if(selector>MellowGPUStorageUnmap) return MellowGPUControlUnsupported;
    if(r.request_id<=lastRequestId) return MellowGPUControlReplay;
    if(selector==MellowGPUDeviceQuery)
        return (!r.session && !r.device_epoch && !r.object && !r.bytes)
            ? MellowGPUControlOk : MellowGPUControlInvalid;
    if(!expectedSession || r.session!=expectedSession) return MellowGPUControlStaleSession;
    if(!expectedEpoch || r.device_epoch!=expectedEpoch) return MellowGPUControlStaleEpoch;
    if(selector==MellowGPUStorageCreate) {
        if(r.object || !r.bytes || r.bytes>MELLOWGPU_CONTROL_MAX_BUFFER_BYTES ||
           (r.bytes&(MELLOWGPU_CONTROL_PAGE_BYTES-1))) return MellowGPUControlInvalid;
    } else if(!r.object || r.bytes) return MellowGPUControlInvalid;
    return MellowGPUControlOk;
}

// A userspace probe must validate a successful native reply before touching the
// returned CPU address. This checks protocol correlation and bounds only; it
// cannot authenticate a GPU, qualify Metal or establish kernel loadability.
inline bool validateSuccessReply(const MellowGPUControlRequest &request,
                                 const MellowGPUControlReply &r) {
    if(validateRequest(request,request.operation,request.session,request.device_epoch,0)
       !=MellowGPUControlOk) return false;
    if(r.abi_major!=MELLOWGPU_CONTROL_ABI_MAJOR || r.abi_minor!=MELLOWGPU_CONTROL_ABI_MINOR ||
       r.struct_size!=sizeof(r) || r.status!=MellowGPUControlOk ||
       r.request_id!=request.request_id || !r.session || !r.device_epoch || r.reserved ||
       r.mode!=MellowGPUControlCPUStorageOnly || !validPciIdentity(r.vendor_id,r.device_id,
       r.class_revision,r.registry_id) || r.pci_bdf>0xffffU ||
       !(r.capabilities&MellowGPUCapDeviceQuery) || (r.capabilities&~7U) ||
       r.client_live_bytes>MELLOWGPU_CONTROL_MAX_CLIENT_BYTES ||
       r.device_backing_bytes>MELLOWGPU_CONTROL_MAX_DEVICE_BYTES ||
       r.client_live_bytes>r.device_backing_bytes) return false;
    if(request.operation==MellowGPUDeviceQuery)
        return !r.object && !r.bytes && !r.cpu_address && r.storage_state==MellowGPUStorageEmpty;
    if(r.session!=request.session || r.device_epoch!=request.device_epoch ||
       !(r.capabilities&MellowGPUCapCPUStorage)) return false;
    if(request.operation==MellowGPUStorageCreate)
        return r.object && r.bytes==request.bytes && r.client_live_bytes>=r.bytes &&
            r.storage_state==MellowGPUStorageAllocated && !r.cpu_address;
    if(r.object!=request.object) return false;
    if(request.operation==MellowGPUStorageClose)
        return !r.bytes && !r.cpu_address && r.storage_state==MellowGPUStorageEmpty;
    if(!(r.capabilities&MellowGPUCapCPUMap) || !r.bytes ||
       r.bytes>MELLOWGPU_CONTROL_MAX_BUFFER_BYTES ||
       (r.bytes&(MELLOWGPU_CONTROL_PAGE_BYTES-1)) || r.client_live_bytes<r.bytes) return false;
    if(request.operation==MellowGPUStorageMap) {
        constexpr uint64_t maxUserAddress=0x00007fffffffffffULL;
        return r.storage_state==MellowGPUStorageMapped &&
            r.cpu_address>=MELLOWGPU_CONTROL_PAGE_BYTES &&
            !(r.cpu_address&(MELLOWGPU_CONTROL_PAGE_BYTES-1)) &&
            r.cpu_address<=maxUserAddress && r.bytes-1<=maxUserAddress-r.cpu_address;
    }
    return request.operation==MellowGPUStorageUnmap &&
        r.storage_state==MellowGPUStorageAllocated && !r.cpu_address;
}

}} // namespace MellowGPUCore::Control
#define MELLOWGPU_CONTROL_REPLY_VALIDATION 1

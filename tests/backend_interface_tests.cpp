// SPDX-License-Identifier: MIT
// Real internal adapter contract tests. No native backend or GPU executes.
#include "../Drivers/Core/BackendInterfaces.hpp"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>
using namespace MellowGPUCore::Backend;
static unsigned checks;
static void check(bool value) { ++checks; if(!value) { fprintf(stderr,"FAIL check %u\n",checks); abort(); } }
static_assert(!std::is_copy_constructible<IntelInterfaceV1>::value,"binding must not be copied");
static_assert(!std::is_copy_assignable<IntelInterfaceV1>::value,"binding must not be overwritten");
static_assert(!std::is_move_constructible<IntelInterfaceV1>::value,"binding has stable single owner");
static void poison(Output &out) {
    out.identity={0x8086,0x3185,6,77,88,KernelArchitecture::X86_64};
    out.capabilities=~0ULL;out.object=99;out.nativeInitialized=true;
}
static void empty(const Output &out) {
    check(!out.identity.vendor && !out.identity.device && !out.identity.revision &&
          !out.identity.registry && !out.identity.epoch && !out.capabilities &&
          !out.object && !out.nativeInitialized);
}
template<class Adapter> static void exercise(uint16_t vendor) {
    Adapter adapter;
    Identity identity {vendor,0x3185,6,0x100000205ULL,1,KernelArchitecture::X86_64};
    Output out; out.capabilities=~0ULL;out.object=99;
    Request request {1,0,Operation::Describe,identity.registry,identity.epoch};
    check(adapter.call(request,out)==Status::NotBound && !out.capabilities && !out.object);
    auto bad=identity;bad.vendor=vendor==0x8086 ? 0x1002 : 0x8086;
    check(adapter.bindIdentity(bad)==Status::WrongVendor);
    bad=identity;bad.registry=0;check(adapter.bindIdentity(bad)==Status::InvalidIdentity);
    bad=identity;bad.epoch=0;check(adapter.bindIdentity(bad)==Status::InvalidIdentity);
    bad=identity;bad.device=0;check(adapter.bindIdentity(bad)==Status::InvalidIdentity);
    bad=identity;bad.device=0xffff;check(adapter.bindIdentity(bad)==Status::InvalidIdentity);
    bad=identity;bad.architecture={};check(adapter.bindIdentity(bad)==Status::UnknownArchitecture);
    bad=identity;bad.architecture=static_cast<KernelArchitecture>(255);
    check(adapter.bindIdentity(bad)==Status::UnknownArchitecture);
    check(adapter.bindIdentity(identity)==Status::IdentityBound);
    check(adapter.bindIdentity(identity)==Status::AlreadyBound);
    bad=identity;bad.registry++;check(adapter.bindIdentity(bad)==Status::BindingConflict);
    check(adapter.call(request,out)==Status::MetadataOnly);
    check(out.identity.vendor==vendor && out.identity.registry==identity.registry);
    check(!out.capabilities && !out.nativeInitialized && !out.object);
    poison(out);request.major=2;check(adapter.call(request,out)==Status::UnsupportedABI);empty(out);
    poison(out);request.major=1;request.minor=1;check(adapter.call(request,out)==Status::UnsupportedABI);empty(out);
    poison(out);request.minor=0;request.registry++;check(adapter.call(request,out)==Status::WrongDevice);empty(out);
    poison(out);request.registry=identity.registry;request.epoch++;check(adapter.call(request,out)==Status::StaleEpoch);empty(out);
    request.epoch=identity.epoch;
    const Operation missing[]={Operation::CreateResource,Operation::CreateQueue,Operation::Submit,
        Operation::ObserveFence,Operation::Recover,Operation::Present,Operation::VendorExtension};
    for(auto operation:missing) {
        request.operation=operation;out.capabilities=~0ULL;out.object=777;out.nativeInitialized=true;
        check(adapter.call(request,out)==Status::Unsupported);
        check(!out.capabilities && !out.object && !out.nativeInitialized && !out.identity.registry);
    }
    poison(out);request.operation=static_cast<Operation>(255);
    check(adapter.call(request,out)==Status::UnsupportedOperation);empty(out);
}
int main() {
    exercise<IntelInterfaceV1>(0x8086);exercise<AmdInterfaceV1>(0x1002);
    exercise<NvidiaInterfaceV1>(0x10de);
    const KernelArchitecture architectures[]={KernelArchitecture::Arm64,KernelArchitecture::Arm64e};
    for(auto architecture:architectures) {
        IntelInterfaceV1 adapter;Identity identity{0x8086,0x3185,6,1,1,architecture};
        check(adapter.bindIdentity(identity)==Status::IdentityBound);
        Output out;Request request{1,0,Operation::Describe,1,1};
        check(adapter.call(request,out)==Status::MetadataOnly);
        check(out.identity.architecture==architecture && !out.nativeInitialized && !out.capabilities);
        request.operation=Operation::Submit;check(adapter.call(request,out)==Status::Unsupported);
    }
    printf("PASS %u internal three-vendor interface checks; native GPU operations unavailable\n",checks);
}

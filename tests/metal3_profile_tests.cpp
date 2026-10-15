#ifdef MELLOWGPU_PROFILE_BASELINE
#include "metal3_profile_baseline.hpp"
#else
#include "../Drivers/Core/Metal3Profile.hpp"
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace MellowGPUCore::Conformance;
static unsigned checks;
static void check(bool condition){++checks;assert(condition);}
static Digest digest(uint8_t value){Digest d;d.bytes[0]=value;return d;}
static Contract contract(uint16_t vendor=0x8086)
{
 Contract c;c.device={vendor,0x1234,1,81,7};c.compatibility=digest(1);c.inventory=digest(2);c.inventoryComplete=true;
 for(size_t i=0;i<GateCount;i++)c.requiredCases[i]=static_cast<uint32_t>(100+i);
 return c;
}
static Profile profile(const Contract&c,Route route=Route::IntelXeAnv)
{
 Profile p;p.device=c.device;p.compatibility=c.compatibility;p.inventory=c.inventory;p.route=route;
 p.metal3Reported=true;p.pixelTransferCountersKnown=true;
 for(size_t i=0;i<GateCount;i++){
  auto&r=p.records[i];r.gate=static_cast<Gate>(i);r.device=c.device;r.compatibility=c.compatibility;r.inventory=c.inventory;
  r.receipt=digest(static_cast<uint8_t>(10+i));r.source=Source::NativeSystem;r.status=RecordStatus::Passed;
  r.expectedCases=r.passedCases=c.requiredCases[i];
 }
 return p;
}
int main()
{
 auto c=contract();auto p=profile(c);auto a=assess(c,p);
 check(a.status==Status::ReadyForSupportReview&&a.completeProfile&&!a.hardwareAttested);
 const Route intelRoutes[]={Route::IntelI915Anv,Route::IntelXeAnv};
 for(auto route:intelRoutes){p=profile(c,route);check(assess(c,p).completeProfile);}
 auto amd=contract(0x1002);check(assess(amd,profile(amd,Route::AmdgpuRadv)).completeProfile);
 auto nv=contract(0x10de);check(assess(nv,profile(nv,Route::NouveauNvk)).completeProfile);
 for(size_t i=0;i<GateCount;i++){
  p=profile(c);p.records[i].status=RecordStatus::Missing;check(assess(c,p).status==Status::MissingRecord);
  p=profile(c);p.records[i].status=RecordStatus::Failed;check(assess(c,p).status==Status::FailedRecord);
  p=profile(c);p.records[i].source=Source::HostModel;check(assess(c,p).status==Status::NonHardwareRecord);
  p=profile(c);p.records[i].source=Source::SoftwareRenderer;check(assess(c,p).status==Status::NonHardwareRecord);
  p=profile(c);p.records[i].device.epoch++;check(assess(c,p).status==Status::StaleRecord);
  p=profile(c);p.records[i].compatibility=digest(99);check(assess(c,p).status==Status::StaleRecord);
  p=profile(c);p.records[i].inventory=digest(99);check(assess(c,p).status==Status::StaleRecord);
  p=profile(c);p.records[i].passedCases--;check(assess(c,p).status==Status::IncompleteCases);
  p=profile(c);p.records[i].skippedCases=1;check(assess(c,p).status==Status::IncompleteCases);
  p=profile(c);p.records[i].failedCases=1;check(assess(c,p).status==Status::IncompleteCases);
  p=profile(c);p.records[i].expectedCases--;check(assess(c,p).status==Status::IncompleteCases);
  p=profile(c);p.records[i].receipt={};check(assess(c,p).status==Status::MissingRecord);
  p=profile(c);p.records[i].gate=Gate::Count;check(assess(c,p).status==Status::MissingRecord);
 }
 p=profile(c);p.records[static_cast<size_t>(Gate::WindowServer)].source=Source::NativeGpu;
 check(assess(c,p).status==Status::NonHardwareRecord);
 p=profile(c);p.records[1].receipt=p.records[0].receipt;check(assess(c,p).status==Status::ReusedRecord);
 p=profile(c);p.route=Route::AmdgpuRadv;check(assess(c,p).status==Status::UnsupportedRoute);
 p=profile(nv,Route::NvidiaRmUnpaired);check(assess(nv,p).status==Status::UnsupportedRoute);
 p=profile(c);p.metal3Reported=false;check(assess(c,p).status==Status::Metal3Unavailable);
 p=profile(c);p.softwarePixelExecution=true;check(assess(c,p).status==Status::SoftwarePath);
 p=profile(c);p.pixelTransferCountersKnown=false;check(assess(c,p).status==Status::MissingCounters);
 p=profile(c);p.cpuPixelTransfers=1;check(assess(c,p).status==Status::CpuPixelTransfer);
 p=profile(c);p.device.instance++;check(assess(c,p).status==Status::WrongDevice);
 p=profile(c);p.compatibility=digest(3);check(assess(c,p).status==Status::StaleRecord);
 p=profile(c);p.inventory=digest(3);check(assess(c,p).status==Status::IncompleteInventory);
 auto bad=c;bad.inventoryComplete=false;check(assess(bad,profile(bad)).status==Status::IncompleteInventory);
 bad=c;bad.compatibility={};check(assess(bad,profile(bad)).status==Status::MalformedContract);
 bad=c;bad.requiredCases[0]=0;check(assess(bad,profile(bad)).status==Status::MalformedContract);
 bad=c;bad.device.epoch=0;check(assess(bad,profile(bad)).status==Status::MalformedContract);
 printf("PASS: %u full-profile policy checks; synthetic records only; hardware not executed or attested\n",checks);
}

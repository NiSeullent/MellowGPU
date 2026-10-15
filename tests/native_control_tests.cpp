// SPDX-License-Identifier: MIT
// Executes actual native adapter bodies. All OS/task/PCI boundaries are simulated.
#include "native_control_shim.hpp"
#include "../Mellow/MellowGPUDevice.hpp"
#include <assert.h>
#include <stdio.h>
using namespace MellowGPUCore;
namespace Mock=ControlShim;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Fixture {
 Task task;IOPCIDevice*pci{};MellowGPUDevice*device{};std::vector<MellowGPUClient*>clients;bool quarantined{};
 Fixture(uint16_t vendor=0x8086,unsigned bus=0,unsigned slot=2,unsigned function=0){Mock::reset();Mock::current=&task;Mock::arguments.insert("-mellowgpudev");pci=new IOPCIDevice;pci->id=(0x3185U<<16)|vendor;pci->bus=bus;pci->slot=slot;pci->function=function;device=new MellowGPUDevice;CHECK(device->init());CHECK(device->probe(pci,nullptr)==device);CHECK(device->start(pci));}
 MellowGPUClient*client(){IOUserClient*c=nullptr;CHECK(device->newUserClient(&task,nullptr,MELLOWGPU_CONTROL_CONNECT_TYPE,nullptr,&c)==kIOReturnSuccess);auto*n=dynamic_cast<MellowGPUClient*>(c);CHECK(n);clients.push_back(n);return n;}
 ~Fixture(){
  for(auto*c:clients){c->clientClose();c->stop(device);c->detach(device);c->release();}
  device->stop(pci);device->release();pci->release();
  if(quarantined){CHECK(!Mock::objects.empty());Mock::discardQuarantinedWorld();task.refs=1;}
  CHECK(Mock::objects.empty());CHECK(Mock::allocations.empty());CHECK(task.refs==1);CHECK(Mock::forbiddenCalls==0);
 }
};
static MellowGPUControlRequest request(uint32_t op,uint64_t id,uint64_t session=0,uint64_t epoch=0,uint64_t object=0,uint64_t bytes=0){return{1,0,64,op,id,session,epoch,object,bytes,0};}
static MellowGPUControlReply call(MellowGPUClient*c,MellowGPUControlRequest r){
 MellowGPUControlReply reply;std::memset(&reply,0xa5,sizeof(reply));IOExternalMethodArguments a;a.structureInput=&r;a.structureOutput=&reply;a.structureInputSize=64;a.structureOutputSize=128;
 CHECK(c->externalMethod(r.operation,&a)==kIOReturnSuccess);CHECK(reply.abi_major==1&&!reply.abi_minor&&reply.struct_size==128&&reply.request_id==r.request_id&&!reply.reserved);CHECK(reply.capabilities==7&&reply.mode==1);CHECK(reply.device_backing_bytes<=MELLOWGPU_CONTROL_MAX_DEVICE_BYTES&&reply.client_live_bytes<=MELLOWGPU_CONTROL_MAX_CLIENT_BYTES);return reply;
}
static void admission(){
 {Mock::reset();Mock::faults.lock=true;auto*d=new MellowGPUDevice;CHECK(!d->init());d->release();CHECK(Mock::objects.empty()&&Mock::allocations.empty());}
 for(auto vendor:{0x8086,0x1002,0x10de}){Fixture f(static_cast<uint16_t>(vendor));CHECK(f.device->numbers["MellowGPUPhysicalVendorID"]==static_cast<unsigned>(vendor));CHECK(f.device->strings["MellowGPUQualification"]=="unqualified");}
 for(unsigned bad=0;bad<11;++bad){Mock::reset();auto*p=new IOPCIDevice;auto*d=new MellowGPUDevice;CHECK(d->init());
  Mock::arguments.insert("-mellowgpudev");
  switch(bad){case 0:Mock::arguments.clear();break;case 1:Mock::darwin=25;break;case 2:Mock::arguments.insert("-mellowoff");break;case 3:Mock::arguments.insert("-mellowdiag");break;case 4:Mock::arguments.insert("-mellownativexe");break;case 5:Mock::arguments.insert("-mellowtahoe");break;case 6:Mock::arguments.insert("-mellowtglwithgfx");break;case 7:p->id=0x12341234;break;case 8:p->id=0xffff8086;break;case 9:p->revision=0x02000001;break;case 10:p->registry=0;break;}
  CHECK(!d->probe(p,nullptr));CHECK(!d->start(p));CHECK(!Mock::registers);d->release();p->release();CHECK(Mock::objects.empty());
 }
 for(unsigned fault=0;fault<3;++fault){Mock::reset();Mock::arguments.insert("-mellowgpudev");auto*p=new IOPCIDevice;auto*d=new MellowGPUDevice;CHECK(d->init());
  if(fault==0)Mock::faults.malloc=true;if(fault==1)Mock::faults.property=true;if(fault==2)Mock::faults.serviceStart=true;
  CHECK(!d->start(p));CHECK(Mock::superStarts==Mock::superStops);d->release();p->release();CHECK(Mock::objects.empty()&&Mock::allocations.empty());
 }
}
static void requestsAndAuthorization(){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));CHECK(q.status==0&&q.session&&q.device_epoch&&q.registry_id==99&&!q.object&&!q.bytes&&!q.cpu_address);
 CHECK(call(c,request(0,1)).status==MellowGPUControlReplay);
 auto bad=request(1,2,q.session,q.device_epoch,0,4096);bad.flags=1;CHECK(call(c,bad).status==MellowGPUControlInvalid);
 CHECK(call(c,request(1,2,q.session+1,q.device_epoch,0,4096)).status==MellowGPUControlStaleSession);
 CHECK(call(c,request(1,2,q.session,q.device_epoch+1,0,4096)).status==MellowGPUControlStaleEpoch);
 CHECK(call(c,request(9,2)).status==MellowGPUControlUnsupported);
 MellowGPUControlReply reply{};auto r=request(0,2);IOExternalMethodArguments a;a.structureInput=&r;a.structureOutput=&reply;a.structureInputSize=64;a.structureOutputSize=128;
 for(unsigned i=0;i<7;i++){auto b=a;switch(i){case 0:b.scalarInputCount=1;break;case 1:b.asyncWakePort=1;break;case 2:b.structureInputSize=63;break;case 3:b.structureOutputSize=127;break;case 4:b.structureInputDescriptor=&r;break;case 5:b.structureVariableOutputData=&r;break;case 6:b.version=0;break;}CHECK(c->externalMethod(0,&b)==kIOReturnBadArgument);}
 Task other;Mock::current=&other;CHECK(c->externalMethod(0,&a)==kIOReturnNotPrivileged);IOMemoryDescriptor*m=reinterpret_cast<IOMemoryDescriptor*>(1);IOOptionBits options=0;CHECK(c->clientMemoryForType(1,&options,&m)==kIOReturnNotPrivileged&&!m);
 Mock::current=&f.task;f.task.admin=false;CHECK(c->externalMethod(0,&a)==kIOReturnNotPrivileged);f.task.admin=true;CHECK(c->clientMemoryForType(1,&options,&m)==kIOReturnUnsupported&&!m);CHECK(Mock::mapCalls==0);
}
static void realCpuStorage(){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto create=call(c,request(1,2,q.session,q.device_epoch,0,4096));CHECK(!create.status&&create.object&&create.bytes==4096&&create.storage_state==1&&!create.cpu_address&&create.client_live_bytes==4096&&create.device_backing_bytes==4096);
 auto*memory=Mock::lastMemory;for(unsigned i=0;i<4096;++i)CHECK(static_cast<uint8_t*>(memory->getBytesNoCopy())[i]==0);
 auto map=call(c,request(3,3,q.session,q.device_epoch,create.object));CHECK(!map.status&&map.storage_state==2&&map.cpu_address==reinterpret_cast<uintptr_t>(memory->getBytesNoCopy()));auto*cpu=reinterpret_cast<uint8_t*>(map.cpu_address);cpu[200]=0x7b;CHECK(static_cast<uint8_t*>(memory->getBytesNoCopy())[200]==0x7b);
 CHECK(call(c,request(2,4,q.session,q.device_epoch,create.object)).status==MellowGPUControlBusy);
 auto unmapped=call(c,request(4,5,q.session,q.device_epoch,create.object));CHECK(!unmapped.status&&unmapped.bytes==4096&&unmapped.storage_state==1&&!unmapped.cpu_address);
 auto closed=call(c,request(2,6,q.session,q.device_epoch,create.object));CHECK(!closed.status&&closed.object==create.object&&!closed.bytes&&!closed.storage_state&&!closed.cpu_address&&!closed.client_live_bytes&&!closed.device_backing_bytes);
 CHECK(call(c,request(0,7)).device_backing_bytes==0);CHECK(call(c,request(2,8,q.session,q.device_epoch,create.object)).status==MellowGPUControlNotFound);
 auto again=call(c,request(1,9,q.session,q.device_epoch,0,4096));CHECK(!again.status&&again.object!=create.object);
}
static void failuresAndRequestConsumption(){
 for(unsigned fault=0;fault<3;++fault){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));if(fault==0)Mock::faults.memoryInit=true;if(fault==1)Mock::faults.noCpu=true;if(fault==2)Mock::faults.shortLength=true;
  auto bad=call(c,request(1,2,q.session,q.device_epoch,0,4096));CHECK(bad.status==MellowGPUControlNoMemory&&!bad.device_backing_bytes&&!bad.client_live_bytes);Mock::faults={};CHECK(call(c,request(1,2,q.session,q.device_epoch,0,4096)).status==MellowGPUControlReplay);CHECK(call(c,request(1,3,q.session,q.device_epoch,0,4096)).status==0);
 }
 for(unsigned fault=0;fault<4;++fault){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));if(fault==0)Mock::faults.map=true;if(fault==1)Mock::faults.badMapTask=true;if(fault==2)Mock::faults.badMapMemory=true;if(fault==3)Mock::faults.badMapLength=true;
  auto bad=call(c,request(3,3,q.session,q.device_epoch,a.object));CHECK(bad.status!=0&&!bad.cpu_address&&bad.client_live_bytes==4096&&bad.device_backing_bytes==4096);Mock::faults={};CHECK(call(c,request(3,4,q.session,q.device_epoch,a.object)).status==0);
 }
}
static void quarantine(){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));CHECK(call(c,request(3,3,q.session,q.device_epoch,a.object)).status==0);
 Mock::faults.unmap=true;auto bad=call(c,request(4,4,q.session,q.device_epoch,a.object));CHECK(bad.status==MellowGPUControlQuarantined&&bad.storage_state==3&&bad.device_backing_bytes==4096&&!bad.cpu_address);const auto attempts=Mock::unmapCalls;CHECK(Mock::lastMap->getAddress()==0);
 Mock::faults.unmap=false;CHECK(call(c,request(4,5,q.session,q.device_epoch,a.object)).status==MellowGPUControlQuarantined);CHECK(Mock::unmapCalls==attempts);CHECK(call(c,request(2,6,q.session,q.device_epoch,a.object)).status==MellowGPUControlQuarantined);
 CHECK(c->clientClose()==kIOReturnError);CHECK(f.task.refs==2&&Mock::unmapCalls==attempts);f.quarantined=true;
}
static MellowGPUDevice*stopOwner;static IOPCIDevice*stopPci;
static void stopDuringNative(){stopOwner->stop(stopPci);}
static void stopLeaseAndEpoch(){
 {Fixture f;LeaseToken token;const auto refs=f.device->refs;CHECK(f.device->beginOperation(token));CHECK(token.device==99&&token.generation==f.device->getRegistryEntryID()&&f.device->refs==refs+1);f.device->stop(f.pci);LeaseToken denied;CHECK(!f.device->beginOperation(denied));f.device->endOperation(token);CHECK(f.device->refs==refs);}
 {Fixture f;auto*c=f.client();auto q=call(c,request(0,1));stopOwner=f.device;stopPci=f.pci;Mock::faults.onAllocate=stopDuringNative;auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));CHECK(a.status==MellowGPUControlNotReady&&!a.device_backing_bytes&&!a.client_live_bytes);Mock::faults.onAllocate=nullptr;}
 {Fixture f;auto*c=f.client();auto old=call(c,request(0,1));auto*second=new MellowGPUDevice;CHECK(second->init());CHECK(second->start(f.pci));IOUserClient*raw=nullptr;CHECK(second->newUserClient(&f.task,nullptr,MELLOWGPU_CONTROL_CONNECT_TYPE,nullptr,&raw)==0);auto*other=dynamic_cast<MellowGPUClient*>(raw);auto fresh=call(other,request(0,1));CHECK(fresh.session==old.session&&fresh.device_epoch!=old.device_epoch);CHECK(call(other,request(1,2,old.session,old.device_epoch,0,4096)).status==MellowGPUControlStaleEpoch);other->clientClose();other->stop(second);other->detach(second);other->release();second->stop(f.pci);second->release();}
}
static void limits(){
 {Fixture f;for(unsigned i=0;i<16;i++)CHECK(f.device->reserveBacking(16ULL*1024*1024)==0);CHECK(f.device->reserveBacking(4096)==MellowGPUControlCapacity);for(unsigned i=0;i<16;i++)f.device->returnBacking(16ULL*1024*1024);}
 {Fixture f;auto*c=f.client();auto q=call(c,request(0,1));for(unsigned i=0;i<4;i++)CHECK(call(c,request(1,i+2,q.session,q.device_epoch,0,16ULL*1024*1024)).status==0);CHECK(call(c,request(1,6,q.session,q.device_epoch,0,4096)).status==MellowGPUControlCapacity);}
 {Fixture f;for(unsigned i=0;i<16;i++)f.client();IOUserClient*denied=nullptr;CHECK(f.device->newUserClient(&f.task,nullptr,MELLOWGPU_CONTROL_CONNECT_TYPE,nullptr,&denied)==kIOReturnNotReady&&!denied);}
 {Fixture f;auto*c=f.client();auto q=call(c,request(0,1));for(unsigned i=0;i<16;i++)CHECK(call(c,request(1,i+2,q.session,q.device_epoch,0,4096)).status==0);CHECK(call(c,request(1,18,q.session,q.device_epoch,0,4096)).status==MellowGPUControlCapacity);}
}
static void bdf(){
 {Fixture f;CHECK(call(f.client(),request(0,1)).pci_bdf==0x0010);}
 {Fixture f(0x1002,5,31,7);CHECK(call(f.client(),request(0,1)).pci_bdf==0x05ff);}
}
static MellowGPUDevice*startingOwner;static IOReturn earlyOpenResult;
static void openDuringProperty(){Mock::faults.onProperty=nullptr;IOUserClient*c=nullptr;earlyOpenResult=startingOwner->newUserClient(Mock::current,nullptr,MELLOWGPU_CONTROL_CONNECT_TYPE,nullptr,&c);if(c){c->clientClose();c->stop(startingOwner);c->detach(startingOwner);c->release();}}
static void provisionalAdmission(){Mock::reset();Mock::arguments.insert("-mellowgpudev");auto*p=new IOPCIDevice;auto*d=new MellowGPUDevice;CHECK(d->init());startingOwner=d;earlyOpenResult=kIOReturnError;Mock::faults.onProperty=openDuringProperty;CHECK(d->start(p));CHECK(earlyOpenResult==kIOReturnNotReady);d->stop(p);d->release();p->release();CHECK(Mock::objects.empty()&&Mock::allocations.empty());}
static void stopDuringProperty(){Mock::faults.onProperty=nullptr;startingOwner->stop(stopPci);}
static void stoppedProvisionalAdmission(){Mock::reset();Mock::arguments.insert("-mellowgpudev");auto*p=new IOPCIDevice;auto*d=new MellowGPUDevice;CHECK(d->init());startingOwner=d;stopPci=p;Mock::faults.onProperty=stopDuringProperty;CHECK(!d->start(p));CHECK(!Mock::registers);CHECK(!d->controlActive());CHECK(Mock::superStarts==Mock::superStops);d->release();p->release();CHECK(Mock::objects.empty()&&Mock::allocations.empty());}
static void twoPrivateMappings(){Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));auto b=call(c,request(1,3,q.session,q.device_epoch,0,4096));auto ma=call(c,request(3,4,q.session,q.device_epoch,a.object));auto mb=call(c,request(3,5,q.session,q.device_epoch,b.object));CHECK(ma.cpu_address!=mb.cpu_address&&mb.device_backing_bytes==8192);CHECK(call(c,request(2,6,q.session,q.device_epoch,a.object)).status==MellowGPUControlBusy);CHECK(call(c,request(4,7,q.session,q.device_epoch,a.object)).device_backing_bytes==8192);auto closed=call(c,request(2,8,q.session,q.device_epoch,a.object));CHECK(closed.status==0&&closed.device_backing_bytes==4096&&closed.client_live_bytes==4096);CHECK(Mock::bufferDestroys==1);CHECK(c->clientDied()==0);CHECK(Mock::bufferDestroys==2);MellowGPUControlReply r{};f.device->describe(r);CHECK(r.device_backing_bytes==0);}
static void clientConstructionFailures(){for(unsigned fault=0;fault<4;++fault){Fixture f;const auto refs=f.device->refs;IOUserClient*c=reinterpret_cast<IOUserClient*>(1);if(fault==0)Mock::faults.initClient=true;if(fault==1)Mock::faults.lock=true;if(fault==2)Mock::faults.attach=true;if(fault==3)Mock::faults.clientStart=true;CHECK(f.device->newUserClient(&f.task,nullptr,MELLOWGPU_CONTROL_CONNECT_TYPE,nullptr,&c)!=0&&!c);Mock::faults={};CHECK(f.task.refs==1&&f.device->refs==refs);f.client();}}
static void mappingExitBoundaries(){
 {Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));stopOwner=f.device;stopPci=f.pci;Mock::faults.onMap=stopDuringNative;auto mapped=call(c,request(3,3,q.session,q.device_epoch,a.object));CHECK(mapped.status==MellowGPUControlNotReady&&!mapped.cpu_address&&mapped.client_live_bytes==4096&&mapped.device_backing_bytes==4096);Mock::faults={};CHECK(c->clientClose()==0);MellowGPUControlReply r{};f.device->describe(r);CHECK(!r.device_backing_bytes);}
 {Fixture f;auto*c=f.client();auto q=call(c,request(0,1));auto a=call(c,request(1,2,q.session,q.device_epoch,0,4096));Mock::faults.badMapTask=true;Mock::faults.unmap=true;auto mapped=call(c,request(3,3,q.session,q.device_epoch,a.object));CHECK(mapped.status==MellowGPUControlQuarantined&&mapped.storage_state==3&&mapped.device_backing_bytes==4096&&!mapped.cpu_address);Mock::faults={};CHECK(c->clientClose()!=0);f.quarantined=true;}
}
static std::thread provisionalReader;static bool readerActive;
static void pauseProvisionalReader(){Mock::faults.onProperty=nullptr;provisionalReader=std::thread([]{Mock::pauseNextLock=true;readerActive=startingOwner->controlActive();});while(!Mock::pausedLockReached.load())std::this_thread::yield();}
static void failedStartReaderLifetime(){Mock::reset();Mock::arguments.insert("-mellowgpudev");Mock::pausedLockReached=false;Mock::allowPausedLock=false;auto*p=new IOPCIDevice;auto*d=new MellowGPUDevice;CHECK(d->init());startingOwner=d;Mock::faults.property=true;Mock::faults.onProperty=pauseProvisionalReader;CHECK(!d->start(p));CHECK(!Mock::allocations.empty());Mock::allowPausedLock=true;provisionalReader.join();CHECK(!readerActive);CHECK(!d->controlActive());CHECK(!d->start(p));d->release();p->release();CHECK(Mock::objects.empty()&&Mock::allocations.empty());}
int main(){failedStartReaderLifetime();clientConstructionFailures();mappingExitBoundaries();stoppedProvisionalAdmission();twoPrivateMappings();provisionalAdmission();bdf();admission();requestsAndAuthorization();realCpuStorage();failuresAndRequestConsumption();quarantine();stopLeaseAndEpoch();limits();printf("PASS %u native-control checks; actual adapter with simulated IOKit only\n",checks);}

// SPDX-License-Identifier: MIT
// Test-only public IOKit boundary model. No real task mappings, PCI, DMA or GPU.
#pragma once
#include <assert.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <new>
#include <atomic>
#include <thread>
using UInt32=uint32_t;using UInt8=uint8_t;using SInt32=int32_t;using IOReturn=int;
using IOOptionBits=uint32_t;using vm_size_t=size_t;using mach_vm_size_t=uint64_t;using mach_vm_address_t=uint64_t;using IOVirtualAddress=uint64_t;using IOByteCount=uint64_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnError=-1,kIOReturnNotReady=-2,kIOReturnBadArgument=-3,kIOReturnNotPrivileged=-4,kIOReturnNoMemory=-5,kIOReturnUnsupported=-6;
constexpr IOOptionBits kIODirectionInOut=3,kIOMemoryKernelUserShared=0x1000,kIOMapAnywhere=1,kIOMapUnique=2,kIOServiceAsynchronous=8;
constexpr unsigned kIOPCIConfigVendorID=0,kIOPCIConfigRevisionID=8,kIOPCIConfigSubSystemVendorID=44;
constexpr unsigned kIOExternalMethodArgumentsCurrentVersion=2;
inline const char *kIOClientPrivilegeAdministrator="admin";
struct Task {unsigned refs{1};bool admin{true};};using task_t=Task*;
inline Task kernelTask;inline task_t kernel_task=&kernelTask;
class OSObject;class IOBufferMemoryDescriptor;class IOMemoryMap;
namespace ControlShim {
struct Faults {
 bool lock{},malloc{},serviceStart{},clientStart{},initClient{},attach{},property{},memoryInit{},noCpu{},shortLength{},map{},unmap{},unmapConsumes{true},badMapTask{},badMapMemory{},badMapLength{};
 void (*onAllocate)(){};void (*onMap)(){};void (*onProperty)(){};
};
inline std::atomic<bool> pausedLockReached{},allowPausedLock{};inline thread_local bool pauseNextLock{};
inline Faults faults;inline unsigned darwin=24;inline std::set<std::string> arguments;
inline Task defaultTask;inline task_t current=&defaultTask;
inline std::set<OSObject*> objects;inline std::map<void*,size_t> allocations;
inline unsigned configReads{},registers{},superStarts{},superStops{},mapCalls{},unmapCalls{},bufferDestroys{},forbiddenCalls{};
inline IOBufferMemoryDescriptor *lastMemory{};inline IOMemoryMap *lastMap{};
inline uint64_t nextRegistry=1000;
inline void reset(){faults={};darwin=24;arguments.clear();current=&defaultTask;defaultTask.admin=true;configReads=registers=superStarts=superStops=mapCalls=unmapCalls=bufferDestroys=forbiddenCalls=0;lastMemory=nullptr;lastMap=nullptr;}
}
inline task_t current_task(){return ControlShim::current;}
inline void task_reference(task_t t){assert(t);++t->refs;}
inline void task_deallocate(task_t t){assert(t&&t->refs>1);--t->refs;}
inline unsigned getKernelVersion(){return ControlShim::darwin;}
inline bool checkKernelArgument(const char*n){return ControlShim::arguments.count(n);}
inline void IOLog(const char*,...){ }
inline void *IOMalloc(size_t bytes){if(ControlShim::faults.malloc)return nullptr;void*p=std::malloc(bytes);ControlShim::allocations[p]=bytes;return p;}
inline void IOFree(void*p,size_t bytes){assert(ControlShim::allocations.at(p)==bytes);ControlShim::allocations.erase(p);std::free(p);}
struct IOLock{std::mutex mutex;};
inline IOLock *IOLockAlloc(){return ControlShim::faults.lock?nullptr:new IOLock;}
inline void IOLockFree(IOLock*p){delete p;}
inline void IOLockLock(IOLock*p){assert(p);if(ControlShim::pauseNextLock){ControlShim::pauseNextLock=false;ControlShim::pausedLockReached=true;while(!ControlShim::allowPausedLock.load())std::this_thread::yield();}p->mutex.lock();}
inline void IOLockUnlock(IOLock*p){assert(p);p->mutex.unlock();}
class OSObject {
public:
 unsigned refs{1};OSObject(){ControlShim::objects.insert(this);}virtual~OSObject(){ControlShim::objects.erase(this);}
 void retain(){assert(refs);++refs;}void release(){assert(refs);if(!--refs)free();}virtual void free(){delete this;}
};
#define OSDeclareDefaultStructors(n) public:n()=default;~n() override=default;
#define OSDefineMetaClassAndStructors(n,b)
#define OSDynamicCast(t,o) dynamic_cast<t*>(o)
class OSDictionary:public OSObject{};
class IOService:public OSObject {
public:
 IOService *provider{};bool inactive{};uint64_t serviceRegistry{++ControlShim::nextRegistry};std::map<std::string,std::string> strings;std::map<std::string,uint64_t> numbers;
 virtual bool init(OSDictionary* =nullptr){return true;}
 virtual IOService *probe(IOService*,SInt32*){return this;}
 virtual bool start(IOService*p){if(ControlShim::faults.serviceStart)return false;provider=p;++ControlShim::superStarts;return true;}
 virtual void stop(IOService*){++ControlShim::superStops;inactive=true;}
 bool isInactive()const{return inactive;}
 virtual bool attach(IOService*p){if(ControlShim::faults.attach)return false;provider=p;return true;}
 virtual void detach(IOService*){provider=nullptr;}
 bool setProperty(const char*k,const char*v){strings[k]=v;if(ControlShim::faults.onProperty)ControlShim::faults.onProperty();return !ControlShim::faults.property;}
 bool setProperty(const char*k,uint64_t v,unsigned){numbers[k]=v;return !ControlShim::faults.property;}
 virtual void registerService(IOOptionBits options=0){assert(options==kIOServiceAsynchronous);++ControlShim::registers;}
 virtual bool terminate(IOOptionBits=0){inactive=true;return true;}
 uint64_t getRegistryEntryID(){return serviceRegistry;}
 virtual IOReturn newUserClient(task_t,void*,UInt32,OSDictionary*,class IOUserClient**){return kIOReturnUnsupported;}
};
class IOPCIDevice:public IOService {
public:
 uint32_t id{0x31858086},revision{0x03000001},subsystem{0x12348086};uint64_t registry{99};unsigned bus{},slot{2},function{};
 uint32_t configRead32(unsigned offset){++ControlShim::configReads;assert(offset==0||offset==8||offset==44);return offset==0?id:offset==8?revision:subsystem;}
 uint64_t getRegistryEntryID(){return registry;}
 unsigned getBusNumber(){return bus;}unsigned getDeviceNumber(){return slot;}unsigned getFunctionNumber(){return function;}
 // Any accidental active hardware operation fails immediately in actual tests.
 bool open(IOService*){++ControlShim::forbiddenCalls;assert(false);return false;}
 void setBusMasterEnable(bool){++ControlShim::forbiddenCalls;assert(false);}
 void configWrite32(unsigned,uint32_t){++ControlShim::forbiddenCalls;assert(false);}
};
struct IOExternalMethodDispatch{};
struct IOExternalMethodArguments {
 uint32_t version{kIOExternalMethodArgumentsCurrentVersion};uint32_t scalarInputCount{},scalarOutputCount{},asyncWakePort{},asyncReferenceCount{};
 void*structureInputDescriptor{};void*structureOutputDescriptor{};void*structureVariableOutputData{};
 const void*structureInput{};void*structureOutput{};uint32_t structureInputSize{},structureOutputSize{};
};
class IOMemoryDescriptor:public OSObject {
public:
 virtual void*getBytesNoCopy()const=0;virtual uint64_t getLength()const=0;
 virtual IOMemoryMap*createMappingInTask(task_t,uint64_t,IOOptionBits)=0;
};
class IOBufferMemoryDescriptor:public IOMemoryDescriptor {
public:
 uint8_t*cpu{};uint64_t bytes{};
 static IOBufferMemoryDescriptor*inTaskWithOptions(task_t task,IOOptionBits options,uint64_t size,uint64_t alignment){auto*m=new IOBufferMemoryDescriptor;if(!m->initWithPhysicalMask(task,options,size,alignment,0)){m->release();return nullptr;}return m;}
 ~IOBufferMemoryDescriptor()override{if(cpu){std::free(cpu);++ControlShim::bufferDestroys;}}
 virtual bool initWithPhysicalMask(task_t task,IOOptionBits options,uint64_t size,uint64_t alignment,uint64_t physicalMask){
  assert(task==kernel_task&&options==(kIODirectionInOut|kIOMemoryKernelUserShared)&&alignment==4096&&!physicalMask);
  if(ControlShim::faults.memoryInit)return false;
  cpu=static_cast<uint8_t*>(std::aligned_alloc(4096,size));assert(cpu);bytes=size;std::memset(cpu,0xa5,size);ControlShim::lastMemory=this;
  if(ControlShim::faults.onAllocate)ControlShim::faults.onAllocate();return true;
 }
 void*getBytesNoCopy()const override{return ControlShim::faults.noCpu?nullptr:cpu;}
 uint64_t getLength()const override{return bytes-(ControlShim::faults.shortLength?1:0);}
 IOMemoryMap*createMappingInTask(task_t,uint64_t,IOOptionBits)override;
};
class IOMemoryMap:public OSObject {
public:
 IOMemoryDescriptor*memory;task_t task;uint64_t address,bytes;
 IOMemoryMap(IOMemoryDescriptor*m,task_t t):memory(m),task(t),address(reinterpret_cast<uintptr_t>(m->getBytesNoCopy())),bytes(m->getLength()){memory->retain();}
 ~IOMemoryMap()override{memory->release();}
 IOReturn unmap(){++ControlShim::unmapCalls;if(ControlShim::faults.unmap){if(ControlShim::faults.unmapConsumes)address=0;return kIOReturnError;}address=0;return kIOReturnSuccess;}
 IOMemoryDescriptor*getMemoryDescriptor(){return ControlShim::faults.badMapMemory?nullptr:memory;}
 task_t getAddressTask(){return ControlShim::faults.badMapTask?kernel_task:task;}
 uint64_t getAddress(){return address;}uint64_t getLength(){return bytes-(ControlShim::faults.badMapLength?1:0);}
};
inline IOMemoryMap*IOBufferMemoryDescriptor::createMappingInTask(task_t task,uint64_t address,IOOptionBits options){
 ++ControlShim::mapCalls;assert(!address&&options==(kIOMapAnywhere|kIOMapUnique));if(ControlShim::faults.map)return nullptr;
 auto*m=new IOMemoryMap(this,task);ControlShim::lastMap=m;if(ControlShim::faults.onMap)ControlShim::faults.onMap();return m;
}
class IOUserClient:public IOService {
public:
 virtual bool initWithTask(task_t,void*,UInt32,OSDictionary*){return !ControlShim::faults.initClient;}
 bool start(IOService*p)override{if(ControlShim::faults.clientStart)return false;return IOService::start(p);}
 static IOReturn clientHasPrivilege(void*t,const char*){return t&&static_cast<Task*>(t)->admin?kIOReturnSuccess:kIOReturnNotPrivileged;}
 virtual IOReturn clientClose(){return kIOReturnSuccess;}virtual IOReturn clientDied(){return clientClose();}
 virtual IOReturn clientMemoryForType(UInt32,IOOptionBits*,IOMemoryDescriptor**){return kIOReturnUnsupported;}
 virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments*,IOExternalMethodDispatch* =nullptr,OSObject* =nullptr,void* =nullptr){return kIOReturnUnsupported;}
};
namespace ControlShim {
// Only for a deliberately retained quarantined host world. This discards the
// simulation after assertions; it is NOT a production recovery/unmap algorithm.
inline void discardQuarantinedWorld(){
 for(;;){IOMemoryMap*map=nullptr;for(auto*o:objects)if((map=dynamic_cast<IOMemoryMap*>(o)))break;if(!map)break;delete map;}
 while(!objects.empty())delete *objects.begin();
 for(auto&a:allocations)std::free(a.first);allocations.clear();
}
}

// SPDX-License-Identifier: MIT
// Original qualification policy; existing combined-product licenses remain.
#pragma once

#include <stdint.h>
#include <stddef.h>

// Internal qualification-record policy. The native runner must authenticate the
// measurements before constructing these records; app/ioctl input is not trusted.
// This code performs no GPU operation and cannot attest hardware execution.
namespace MellowGPUCore { namespace Conformance {

struct Digest { uint8_t bytes[32] {}; };
struct Identity { uint16_t vendor {}, device {}; uint32_t revision {}; uint64_t instance {}, epoch {}; };
enum class Route : uint8_t { Unknown, IntelI915Anv, IntelXeAnv, AmdgpuRadv, NouveauNvk, NvidiaRmUnpaired };
enum class Gate : uint8_t { Metal3Api, Shaders, Resources, Synchronization, WindowServer, Scanout, Applications, Games, Lifecycle, Performance, Count };
constexpr size_t GateCount=static_cast<size_t>(Gate::Count);
enum class Source : uint8_t { Unknown, HostModel, SoftwareRenderer, NativeGpu, NativeSystem };
enum class RecordStatus : uint8_t { Missing, Failed, Passed };
struct Contract {
 Identity device {}; Digest compatibility {}, inventory {};
 bool inventoryComplete {}; uint32_t requiredCases[GateCount] {};
};
struct Record {
 Gate gate {Gate::Count}; Identity device {}; Digest compatibility {}, inventory {}, receipt {};
 Source source {Source::Unknown}; RecordStatus status {RecordStatus::Missing};
 uint32_t expectedCases {}, passedCases {}, failedCases {}, skippedCases {};
};
struct Profile {
 Identity device {}; Digest compatibility {}, inventory {}; Route route {Route::Unknown};
 bool metal3Reported {}, softwarePixelExecution {}, pixelTransferCountersKnown {};
 uint64_t cpuPixelTransfers {}; Record records[GateCount] {};
};
enum class Status : uint8_t {
 ReadyForSupportReview, MalformedContract, WrongDevice, UnsupportedRoute,
 Metal3Unavailable, IncompleteInventory, SoftwarePath, MissingCounters, CpuPixelTransfer,
 MissingRecord, FailedRecord, NonHardwareRecord, StaleRecord, IncompleteCases, ReusedRecord
};
struct Assessment {Status status {Status::MalformedContract}; Gate gate {Gate::Count}; bool completeProfile {}; bool hardwareAttested {};};

inline bool sameDigest(const Digest&a,const Digest&b)
{for(size_t i=0;i<32;i++)if(a.bytes[i]!=b.bytes[i])return false;return true;}
inline bool present(const Digest&d)
{for(auto b:d.bytes)if(b)return true;return false;}
inline bool sameIdentity(const Identity&a,const Identity&b)
{return a.vendor==b.vendor&&a.device==b.device&&a.revision==b.revision&&a.instance==b.instance&&a.epoch==b.epoch;}
inline bool validIdentity(const Identity&i)
{return i.vendor&&i.vendor!=0xffff&&i.device&&i.device!=0xffff&&i.instance&&i.epoch;}
inline bool routeMatches(uint16_t vendor,Route route)
{
 switch(route){
  case Route::IntelI915Anv:case Route::IntelXeAnv:return vendor==0x8086;
  case Route::AmdgpuRadv:return vendor==0x1002;
  case Route::NouveauNvk:return vendor==0x10de;
  default:return false;
 }
}

inline Assessment assess(const Contract&contract,const Profile&profile)
{
 if(!validIdentity(contract.device)||!present(contract.compatibility)||!present(contract.inventory))
  return {Status::MalformedContract,Gate::Count,false,false};
 for(auto count:contract.requiredCases)if(!count)
  return {Status::MalformedContract,Gate::Count,false,false};
 if(!sameIdentity(contract.device,profile.device))return {Status::WrongDevice,Gate::Count,false,false};
 if(!routeMatches(profile.device.vendor,profile.route))return {Status::UnsupportedRoute,Gate::Count,false,false};
 if(!profile.metal3Reported)return {Status::Metal3Unavailable,Gate::Metal3Api,false,false};
 if(!contract.inventoryComplete||!sameDigest(contract.inventory,profile.inventory))
  return {Status::IncompleteInventory,Gate::Metal3Api,false,false};
 if(!sameDigest(contract.compatibility,profile.compatibility))return {Status::StaleRecord,Gate::Count,false,false};
 if(profile.softwarePixelExecution)return {Status::SoftwarePath,Gate::Count,false,false};
 if(!profile.pixelTransferCountersKnown)return {Status::MissingCounters,Gate::Resources,false,false};
 if(profile.cpuPixelTransfers)return {Status::CpuPixelTransfer,Gate::Resources,false,false};
 for(size_t i=0;i<GateCount;i++){
  const auto gate=static_cast<Gate>(i);const auto&r=profile.records[i];
  if(r.gate!=gate||r.status==RecordStatus::Missing||!present(r.receipt))return {Status::MissingRecord,gate,false,false};
  if(r.status!=RecordStatus::Passed)return {Status::FailedRecord,gate,false,false};
  const bool native=r.source==Source::NativeGpu||r.source==Source::NativeSystem;
  const bool system=i>=static_cast<size_t>(Gate::WindowServer);
  if(!native||(system&&r.source!=Source::NativeSystem))return {Status::NonHardwareRecord,gate,false,false};
  if(!sameIdentity(contract.device,r.device)||!sameDigest(contract.compatibility,r.compatibility)||!sameDigest(contract.inventory,r.inventory))
   return {Status::StaleRecord,gate,false,false};
  if(r.expectedCases!=contract.requiredCases[i]||r.passedCases!=r.expectedCases||r.failedCases||r.skippedCases)
   return {Status::IncompleteCases,gate,false,false};
  for(size_t previous=0;previous<i;previous++)if(sameDigest(r.receipt,profile.records[previous].receipt))
   return {Status::ReusedRecord,gate,false,false};
 }
 // Complete internal records are eligible for release review only. Authority
 // to publish a device remains with the separate native qualification service.
 return {Status::ReadyForSupportReview,Gate::Count,true,false};
}

}}

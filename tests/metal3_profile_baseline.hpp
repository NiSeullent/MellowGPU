// Test-only absent implementation used to record the initial failing behavior.
#pragma once
#include <stdint.h>
#include <stddef.h>
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
inline Assessment assess(const Contract &,const Profile &) {return {Status::MissingRecord,Gate::Metal3Api,false,false};}
}}

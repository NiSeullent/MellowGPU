# ADR 0002: Metal semantic frontend and vendor execution routes

Date: 2026-10-16. Status: selected development direction, subject to measured
conformance and performance. System driver registration remains unresolved.
Requirements: `MG-CORE-ABI-001`, `MG-CORE-SYNC-001`,
`MG-GPU-INTEL-001`, `MG-GPU-AMD-001`, `MG-GPU-NVIDIA-001`, `MG-DISPLAY-001`.

## Context

Metal object semantics, AIR/MSL compilation, GPU execution and integration with
Apple's device/compositor stack are different contracts. The actual 24H32
runtime exposes four observed private selector encodings; that observation did
not invoke a private initializer or submit work. No inference from those names
may enable a partial system Metal object.
[Exact metadata and scope](../platform-support.md#observed-metal-metadata).

## Alternatives and tradeoffs

| Route | Correctness and compatibility | Development cost | Performance hypothesis to measure |
| --- | --- | --- | --- |
| Metal semantic frontend -> Vulkan -> ANV/RADV/NVK | Reuses mature shader/code-generation and Vulkan execution work, but Metal storage/hazards/argument buffers/formats/presentation require explicit translation; a Vulkan device does not register a Metal driver | Matching native winsys/UAPI, compiler dialect and resource/fence/display adapters remain substantial | Lower initial compiler cost; translation, descriptor churn, barriers and shader-cache overhead may dominate some workloads |
| Metal frontend -> direct per-generation commands/compiler | Can represent vendor-specific requirements and avoid an intermediate API where justified | Largest implementation/validation burden; VM, schedulers, shader generation and each required API must be maintained | Potential lower submit/translation cost, not an established advantage |
| Historical Metal-like facade -> host GL/CL | Useful limited execution oracle on an already accelerated host | Lower bounded experimentation cost; cannot add a missing native GPU or full Metal semantics | May incur copies/serialization and compiler work; results do not qualify the new driver |
| Private Apple runtime calls/interposition as the product | Could reuse parts of a particular installed stack | Exact per-build ABI, object ownership, signing and system integration are unproven; selector lookup is insufficient | UNKNOWN; no product route selected from a symbol list |

Select a MellowGPU-owned semantic frontend with a versioned execution boundary.
Start by evaluating Mesa vendor routes against the complete required contract;
keep a separately reviewed direct backend option for measured gaps or benefits.
This is a design selection, not a claim that a production Metal-to-Vulkan layer
or a complete Metal driver already exists. The choice does not depend on
mixing NVIDIA RM handles/channels with Nouveau/NVK objects.

## Compiler and resource contract

Pin each AIR/compiler adapter to tool/SDK build, target, dialect schema and
input corpus. Validate containers, integer ranges, address spaces, intrinsics,
metadata, resource layouts and relocation types before lowering. Unknown inputs
are compilation errors. Runtime MSL compilation has an explicit implementation
and license gate; private framework discovery is not a compiler API contract.
Bind cached outputs to semantic flags, compiler/backend revisions, GPU family,
firmware ABI and the exact input, and invalidate incompatible cache entries.

Metal feature eligibility is a reviewed programming-model/family/limit inventory,
not a version string or a count of methods. [Apple's feature tables](https://developer.apple.com/metal/capabilities/)
are an input to that review. Required resource, render, compute, blit and shader
cases need semantic tests, native completions and correct outputs. Every optional
feature has an accurate physical applicability predicate and unsupported path.

A shared surface additionally needs producer authority and a native fence.
Matching pages or completing a CPU wait cannot create a general cross-process
Metal/WindowServer synchronization contract. Undocumented descriptor equality
and one-field runtime probes cannot establish validation of all attachment slots
or future fields. Limited historical facades stay explicitly outside full
protocol/family and system-device advertisement.

## Promotion and measurement

Before choosing a product route, compare on the same GPU/OS/power/quality tuple:
CPU encoding/submit cost, actual GPU time, bandwidth and copy count, cold compile
and cache-hit latency, present latency, throughput, frame-time p95/p99 and
stutter under sustained load. Faster-than-NullMoth is a hypothesis until this
paired experiment exists. CPU rendering or hidden frame staging cannot qualify
the native route; explicit validation readbacks are labeled and measured.

GPU correctness and per-process isolation precede global device discovery.
System publication additionally requires the exact native Apple integration,
IOSurface/WindowServer, application/game and display/recovery gates. No loaded
experimental object may return a supported family or masquerade as the system
GPU while those gates remain incomplete. Unknown command acceptance is never
blindly retried. The supported tuple list remains empty until all mandatory
records are independently reviewed.

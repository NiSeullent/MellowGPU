# Metal 3 and desktop conformance

Revision 1, 2026-10-16. This document defines the evidence needed by
[the product specification](MELLOWGPU-SPEC.md), with the source observations in
[source-analysis.md](source-analysis.md). It supplements the existing
[SDK inventory](MELLOWGPU-METAL-API.md), [native control boundary](MELLOWGPU-NATIVE-CONTROL.md)
and [lifecycle contract](MELLOWGPU-LIFECYCLE.md). It grants no device capability.

**There are zero MellowGPU-qualified profiles.** The current phase is research,
architecture, specification and initial implementation. Complete selected Metal
3 behavior, GPU WindowServer composition, native presentation, and a published
app/game corpus are the final support bar. A device unable to meet the mandatory
profile is excluded from supported releases; a development experiment is not a
reduced definition of support.

## Normative sources and unresolved family review

| Source | Selected revision/content identity | What it establishes |
| --- | --- | --- |
| [Apple Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf), also reached through [capabilities](https://developer.apple.com/metal/capabilities/) | **May 21, 2026**, 18 pages; SHA256 `9f31df15dd6827545702c5a0845f6e36e1889878cd0e534123bd70211e5c00a8` | A reviewed feature/limit/format reference, not a qualified PCI device |
| [Apple `MTLGPUFamily.metal3`](https://developer.apple.com/documentation/metal/mtlgpufamily/metal3) / [primary symbol data](https://developer.apple.com/tutorials/data/documentation/metal/mtlgpufamily/metal3.json) | Observed 2026-10-16; SHA256 `aacd65ddd8380c528ed58bb89f707a02f1bdfd2c42a48a5d3936cc9f098108dc` | Public symbol/platform documentation; not proof that a replacement driver implements the family |
| Local Apple SDK declaration captures | SDK15.5 and SDK26.5, both explicitly targeting x86_64/macOS **15.8.1**; digests in [the review record](../porting/metal-api-review.json) | Source-level declarations and spelled availability only |

The selected Apple table distinguishes the Metal programming model from hardware
families and separates Metal 4-only facilities. It describes the Metal 3 model
as the combined Metal 1, 2 and 3 API surface. Family rows, per-device runtime
queries, formats, limits and footnotes must be reviewed together. The current
table's hardware columns cover Apple2–Apple10; they are not an automatic mapping
for Intel, AMD or NVIDIA PCI devices. Examples of separately conditional features
include ray tracing, mesh shading, sparse resources and compressed formats.
Metal 4 command allocators, argument tables and its decoupled queue model are
not mandatory merely because a newer SDK contains them.

**Selected family for 15IGL7/8086:3185: UNKNOWN.** No `Mac2`, `Apple7`,
`MTLGPUFamilyMetal3`, ray-tracing or mesh capability is inferred from its product
name, architecture, a Vulkan extension or the table. The profile review must
select applicable official family/limit material and justify every mandatory
and conditional item. Missing review blocks support; it does not default a
feature to false and then declare an artificially smaller complete profile.

The SDK15.5 capture contains **2,604 declarations** and the SDK26.5 comparison
contains **3,364**. Neither count is the number of mandatory Metal 3 APIs.
SDK15.5 predates the exact target OS, so later target additions remain a review
gap. The comparison explicitly classifies newer availability and Metal 4 names;
unresolved availability stays unresolved. Method-name matches, inherited methods
and historical facade bodies do not verify signatures or semantics. The
inventory's completeness and hardware flags remain false, and verified
implementation count remains zero.

## Requirement and feature records

The frozen root requirement IDs below are shared with the specification. A
feature ID is a stable review key beneath a requirement, not a new support claim.
Every reviewed item needs: exact requirement/feature ID; mandatory/conditional
obligation and applicability predicate; source document digest, page/row or
symbol; source commit/path/symbol and implementation reference; positive and
negative correctness-test identities; native result/raw-log digests; numeric
performance/stability criteria; and behavior when unavailable. A blank, stale,
conflicting or unreviewed item has status `UNKNOWN` and cannot count as passed.

| Requirement | Initial review feature IDs | Mandatory acceptance domain |
| --- | --- | --- |
| `MG-METAL-001` | `metal3.device`, `metal3.api.render`, `metal3.api.compute`, `metal3.api.blit`, `metal3.resources`, `metal3.limits` | Complete reviewed API behavior and accurate capability/limit reporting, native resources and genuine GPU results |
| `MG-SHADER-001` | `metal3.shader.air`, `metal3.shader.msl`, `metal3.shader.codegen`, `metal3.shader.cache` | Accepted compiler/AIR versions, MSL semantics, vendor code generation and content/ABI-bound cache correctness |
| `MG-WS-001` | `desktop.windowserver`, `desktop.coreanimation`, `desktop.app-corpus`, `desktop.game-corpus` | Actual GPU composition, selected process/device identity and published application/game compatibility results |
| `MG-SURFACE-001` | `surface.import`, `surface.cross-process`, `surface.producer-fence`, `surface.retirement` | Authenticated IOSurface rights/allocation identity, native ordering and lifetime across processes |
| `MG-DISPLAY-001` | `display.commit`, `display.present`, `display.vblank`, `display.hotplug`, `display.sleep-reset` | Native modes/outputs, real completion/latch/retirement, declared hotplug and recovery behavior |
| `MG-PERF-001` | `performance.submit`, `performance.shader`, `performance.copy`, `performance.frames`, `performance.soak` | Reproducible paired baseline, declared budgets, distributions, copy accounting and sustained stability |
| `MG-PROVENANCE-001` | `provenance.source`, `provenance.license`, `provenance.firmware`, `provenance.native-abi` | Exact source/build/runtime/firmware/configuration identities and reviewed rights/closure |
| `MG-UPSTREAM-001` | `update.impact`, `update.qualification`, `update.promotion` | Candidate-only discovery, affected requirement invalidation and independent reviewed promotion |

For example, `metal3.optional.raytracing`, `metal3.optional.mesh`,
`metal3.optional.sparse`, `display.optional.hdr` and `display.optional.vrr` need
their own physical-generation, OS, selected-family, format and runtime-feature
predicates. If applicable or advertised, their complete reviewed tests become
mandatory. If legitimately inapplicable, the profile records the reason and the
API's documented unsupported response. Neither “all optional features on” nor
“all optional features off” is an acceptable substitute for that review.

## Semantic and native boundaries

Device discovery must identify a real GPU and matching driver tuple. A supported
family response is produced only from reviewed immutable native capability
records; applications cannot authorize it through an environment variable,
caller-supplied boolean, copied name or user-client argument. Error-returning
stubs must not be reported as implemented APIs.

Metal-to-Vulkan is a candidate implementation strategy, not a conformance
equivalence. For every required Metal operation, the adapter must either show
matching Vulkan/resource/shader/synchronization semantics or implement the
missing native behavior. Vulkan extension names, successful device creation,
shader compilation alone or a noop ICD cannot discharge the requirement. A
direct frontend has the same acceptance obligations. OpenGL/OpenCL, DXVK or
other translation paths need their own versioned workload records; passing one
path does not qualify another graphics API or all games.

Shader correctness includes stage interfaces, argument buffers, resource
indexing, threadgroup/SIMD behavior, memory ordering, atomics, precision and
specialization for the applicable profile. Cache keys bind the source/IR,
compiler/translator/backend versions, GPU/ABI, options and specialization inputs.
Unsupported AIR/MSL/feature combinations must fail explicitly. CPU compilation
and test-oracle arithmetic are allowed; hidden CPU rasterization or replacing a
GPU computation with its expected answer cannot pass.

Cross-process sharing must resolve actual task rights and IOSurface allocation,
retain prepared memory and DMA ownership, validate format/modifier/pitch/size,
and authenticate the producer's native queue/fence. The writer's fence must be
published with the BO ownership before another process can consume it. CPU
timestamps, IOSurface seeds, API return codes or incremented counters alone are
not fences. An old surface remains retained until the actual replacement is
latched and hardware references retire. Device loss invalidates generations;
unknown-flight memory remains quarantined until authoritative quiescence and
cleanup. See [the lifetime state model](MELLOWGPU-LIFECYCLE.md).

WindowServer acceptance requires the real process to select the intended native
device, perform GPU composition and present through the qualified display owner.
App-only/offscreen rendering, wallpaper visibility, a framebuffer name,
`system_profiler`, software VBL, screenshot pixels or an accelerator property
are insufficient individually or together. Require correlated native work,
actual fence completion, composition/present records, display IRQ/latch and
retirement, plus pixel correctness and CPU-transfer accounting. A qualified
GPU copy path must report its copy cost; claim zero-copy only when measurement
proves no relevant copies. CPU frame transfers cannot satisfy the final product
support contract.

## Measurable acceptance corpus

This is the initial **test design**, not a record of completed tests. Each row
must become a versioned corpus entry with source/executable/content digests,
launch parameters, exact OS/GPU tuple, duration, expected results/tolerances,
negative cases, timing budget, raw evidence and known limitations. The current
native conformance result for every row is **NOT_RUN**; required corpus and
threshold review is **UNKNOWN**. Thresholds must be frozen before a run, not
adjusted afterward to make it pass.

| Corpus area | Required positive and negative measurements |
| --- | --- |
| Device/API | Enumerate authentic device; compare all reviewed capability/limit responses against native behavior; refuse unsupported enums, options, foreign handles and resource bounds without fake success |
| Render/compute/blit | Deterministic multi-stage workloads, depth/stencil/MSAA/format combinations as applicable, compute data dependencies, copies/resolves; verify real GPU output and relevant barriers; inject validation and allocation failures |
| Shaders | Versioned AIR/MSL stage/binding/precision/atomic/control-flow corpus; invalid and unsupported input rejection; cold/warm compile latency and cache-key invalidation; compare outputs against defined tolerances |
| Resources/surfaces | Shared/managed/private semantics as applicable, heaps/aliasing/hazards, multi-plane/stride formats, cross-process producer/consumer chains; client exit while native work lives, denied rights, stale generation and malformed size cases |
| WindowServer/apps | OS-matched AppKit/CoreAnimation multi-window and fullscreen scenes; pinned TextEdit, Preview, Photos and browser builds/workloads; resizing, occlusion, animations, video/image surfaces and transitions with selected GPU, frame/pixel/latency/error evidence |
| Games | An explicitly selected set of real game/engine builds and deterministic scenes covering native Metal and any separately claimed translation route; executable/content/version and graphics settings pinned, with known visual and compatibility limits published |
| Display | Every declared output/mode/refresh/color path; EDID/modeset, repeated presents and retirement; monitor disconnect/reconnect and multiple displays only where claimed; no synthetic IRQ substitute |
| Recovery/soak | Memory pressure, malformed submissions, queue timeout, device loss, reset, process termination, sleep/wake and sustained desktop/game load; bounded outcomes with retained unknown-flight resources and no stale-generation reuse |
| Performance | Matched GPU/OS/power/quality baseline, CPU submit cost, GPU duration, bandwidth/copy bytes, shader cold/cache latency, throughput and p50/p95/p99/max frame/present times; stutter, thermal/power state, memory use and failure counts |

The app names identify proposed corpus targets, not measured compatibility. No
game build/content has yet been selected and pinned for this profile; this is
an explicit support blocker. Publish the actual tested list and issue history.
No finite corpus warrants the statement “all applications and games work.”
Likewise, performance and stability exceeding NullMoth are engineering goals
until a same-hardware paired experiment provides results and uncertainty.

## Exact tuple, platforms and result handling

A native result binds CPU architecture; PCI vendor/device/revision/subsystem;
GPU generation; firmware; macOS build, kernel/KC and relevant image UUIDs;
driver/userspace/compiler/shader/patch/configuration hashes; API and corpus
revision; and actual device/display identity. A result for another tuple cannot
be relabeled current. Build-only and host-policy records remain separate.

The current designated initial device is **15IGL7**, N4120, PCI8086:3185 rev06,
subsystem17AA:3E77, x86_64/macOS15.8.1/24H32. Its metadata-only observation of four
Metal superclass encodings is documented in [source analysis](source-analysis.md).
It establishes no private initializer behavior, complete IOAccelerator ABI,
Metal registration, surface ownership or WindowServer acceleration. Historical
NullMoth-derived experimental results and simulations are not a MellowGPU
qualification record. This document changes no boot or security policy.

ARM64 userspace builds, ARM64 XNU loading, ARM64e pointer authentication and
external-vendor GPU access require separate feasibility and native evidence.
Apple's [eGPU support article](https://support.apple.com/en-gb/102363), published
January 22, 2024, requires an Intel Mac for its supported eGPU route; it does not
establish an official Apple Silicon external Intel/AMD/NVIDIA route. Apple
integrated GPU support is a different backend scope. An arm64 helper run or
universal binary does not bridge those boundaries.

Results retain `UNKNOWN`, `NOT_RUN`, failed, unsupported, timeout or device-lost
outcomes distinctly. A timeout never becomes fence success; an interrupted or
ambiguous submission is not automatically resent. Missing raw output, incomplete
corpus review, a mandatory failure, incorrect capability reporting, hidden CPU
fallback or missing system presentation evidence blocks support. Successful
policy checks, compilation and packaging remain initial implementation evidence.
Only the reviewed, exact-tuple full acceptance record may promote a profile.

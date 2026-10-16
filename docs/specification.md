# MellowGPU specification — initial v1

Status: research, architecture and initial implementation, 2026-10-16. **No hardware support profile is qualified.** The product bar is native Metal 3 execution, real GPU WindowServer composition and a published application/game suite on an exact hardware/OS tuple. Firmware framebuffer output, a signed kext, CPU shared storage, hosted models and a universal userspace binary do not satisfy that bar.

This document indexes the normative initial contracts in [`spec/v1`](../spec/v1/). `Tools/mellowgpu-spec.py check` checks their consistency, exact dependency index, implementation/test paths, requirement IDs, current wire constants and development support gates. It does not run those tests, validate the complete Metal feature universe or attest hardware. Schemas are JSON Schema 2020-12; the bundled stdlib checker accepts only their explicitly supported keyword subset and fails on unsupported schema keywords.

## Scope and platform selection

Intel **x86_64 CPU execution is the first host priority**. Intel, AMD and NVIDIA GPU implementations remain separate generation-specific backends. CPU vendor is never the GPU routing key. A future support claim binds physical PCI vendor/device/revision/subsystem, GPU IP/firmware, CPU/kernel architecture, macOS version/build, driver/compiler hashes and required feature profile. Unknown or incomplete combinations return unsupported and remain experimental.

The user's designated first target is Lenovo 15IGL7: Celeron N4120, physical IOPCIDevice `8086:3185`, revision `06`, subsystem `17AA:3E77`, native x86_64 macOS 15.8.1 / 24H32. Its investigation route is **GLK i915/ANV**, separate from the existing Xe models. Spoofed MacBookPro SMBIOS and Apple's built-in IOKitRegistryCompatibility AMD/AGX entries are not physical GPU evidence. Native PCI identification and four Objective-C type encodings have been measured; no MellowGPU GPU command or accelerated WindowServer path has run.

ARM64 and ARM64e have distinct feasibility tracks. ARM64e kernel ABI/PAC/loading cannot be inferred from ARM64 userspace compilation. Apple Silicon external PC GPU access, native DMA/IRQ ownership, Metal registration and display integration remain research questions. Apple integrated GPU implementation is a separate scope. See [platform support](platform-support.md).

## Contract and ownership

Layer A owns IDs, capabilities, quotas and transitions; B owns native XNU/platform mechanisms; C owns vendor/generation hardware; D owns Metal and shaders; E owns display and desktop integration. Details and alternatives are in [architecture](architecture.md) and [ADRs](adr/). Linux objects and APIs are source contracts to translate, not macOS runtime services.

The existing control wire ABI is little-endian **1.0**, a 64-byte request and 128-byte reply, connection type `0x4d470100`. Its only advertised capabilities are query `1`, CPU storage `2` and CPU mapping `4`; it is not a GPU submission ABI. Kernel pointers never become resource handles. The new internal C++ backend interface 1.0 is a separate, caller-serialized metadata contract. Intel/AMD/NVIDIA bind a single nonzero device identity and epoch; Describe returns MetadataOnly, all native GPU operations return Unsupported and clear output. Binding copied metadata is not native device ownership.

Device/context/queue/resource identities include device and generation. Model leases distinguish native operation, pin, submission, publication and display ownership. A submission that may have reached hardware is UNKNOWN until a native authority proves retirement; it is never automatically resubmitted. Queue progress alone cannot retire every resource or prove whole-device quiescence. Resource import, command validation, queue publication, execution completion, display retirement and backing release are separate boundaries.

Policy transition: Running → ResetPending/StopPending/Lost → Quiescing → QuiescentRetained → Running with a strictly newer generation, or Stopped. NativeOperation/Publication must finish before preparing quiescence. A confirmed native barrier leaves pin/submission/display leases retained until individually retired. UNKNOWN leaves admission closed and obligations retained; a superseded plan cannot reopen a lost or stopping device. `Drivers/Core/DeviceLifecycle.hpp` is externally serialized policy, not an interrupt handler, native lock, DMA barrier or hardware reset implementation.

## Frozen requirement groups

The machine-readable [requirements](../spec/v1/requirements.json) contain applicability, source IDs, actual implementation/test paths, acceptance criteria, unsupported behavior and a separate hardware result. Each group below still needs a complete per-feature/per-generation test inventory before qualification. Referenced tests are not test receipts.

| ID | Contract | Initial scope |
|---|---|---|
| MG-CORE-ABI-001 | Version negotiation, output clearing and unavailable operations | Wire and internal metadata contracts |
| MG-CORE-LIFETIME-001 | Device/context/queue/resource ownership and quotas | Typed lease/storage models |
| MG-CORE-SYNC-001 | Publication, completion, native retirement | Partial queue models |
| MG-OS-XNU-001 | PCI/MMIO/IRQ/DMA/IOMMU/platform boundary | Native CPU storage only |
| MG-CPU-X64-001 | Actual x86_64 first target | Identity and metadata measured |
| MG-CPU-ARM64-001 | ARM64 feasibility | Research only |
| MG-CPU-ARM64E-001 | ARM64e/PAC/kernel feasibility | Research only |
| MG-GPU-INTEL-001 | i915/ANV versus Xe/ANV generations | Partial source/model work |
| MG-GPU-AMD-001 | amdgpu/RADV generations | Initial interface/research |
| MG-GPU-NVIDIA-001 | Nouveau/NVK versus open RM contracts | Partial queue/source models |
| MG-METAL-001 | Applicable mandatory Metal 3 semantics | Declaration inventory, semantic coverage incomplete |
| MG-SHADER-001 | AIR versions, IR legality, ISA and cache identity | Research implementation only |
| MG-WS-001 | Actual GPU WindowServer composition | Not implemented/NOT_RUN |
| MG-DISPLAY-001 | Panel, modeset, present, vblank and power | Not qualified/NOT_RUN |
| MG-SURFACE-001 | IOSurface ownership and process synchronization | CPU snapshot oracle only |
| MG-RECOVERY-001 | Timeout, loss, reset and stop | Policy models; native GPU recovery missing |
| MG-PERF-001 | Reproducible GPU performance and stability | No comparative GPU result |
| MG-UPSTREAM-001 | Discovery, impact analysis and reviewable updates | Intake/staging tools; later stages unavailable |
| MG-PROVENANCE-001 | Source, licenses, firmware and ABI receipts | Selected-file provenance; complete closure UNKNOWN |

## Metal, display and support admission

The selected Apple feature-table bytes and SDK declaration observations are pinned separately from implementation. No Apple family is assigned to the GLK target from a product name or PCI ID. API declarations, selector presence and observed method encodings are not shader semantics or private initializer contracts. The target's actual Apple family and applicable complete required-case inventory remain unresolved. See [Metal 3 conformance](metal3-conformance.md).

Admission requires all applicable mandatory features and negative cases, exact native resource/VM isolation, CPU/GPU memory visibility and fences, IOSurface producer/consumer/display retirement, actual WindowServer attribution, physical scanout, recovery, stability, performance and declared app/game suites. A missing required feature, incorrect output, hidden CPU fallback or inflated capability fails admission. HDR, VRR, particular connectors and optional features each need explicit applicability and evidence. There is no guarantee for all games or applications.

## Source and change control

[`dependencies.lock`](../dependencies.lock) is an exact generated index of [`porting/upstreams.lock.json`](../porting/upstreams.lock.json), with requirement/test links and explicit unresolved source/firmware closures. A selected MIT file does not admit its dependency closure. NullMoth and Cobalt remain reference-only. Firmware binaries are not implicitly redistributable. Existing repository/per-file notices remain in effect. See [source analysis](source-analysis.md) and [upstream policy](upstream-update-policy.md).

Generate an index only after reviewing a canonical lock change with `python3 Tools/mellowgpu-spec.py write-lock`. A changed source, patch, license, ABI or firmware observation creates a candidate and invalidates dependent evidence; it cannot promote support. The full automated adapter update/build/hardware/performance/PR sequence is a roadmap item. Repository discovery CI exists; no external-service schedule is created by this work.

## Initial implementation and remaining work

The initial deliverable supplies metadata-only three-vendor interfaces, existing real policy/resource/queue models, native CPU control isolation, contract/negative tests, pinned source intake and candidate impact tooling, versioned schemas, and exact target selection. Actual kernel GPU VM/IRQ/submission/firmware/power ownership, a complete AIR-to-vendor compiler, Metal device registration, GPU IOSurface sharing, display ownership and WindowServer execution remain unfinished.

Development-Mac EFI/boot/SIP/AMFI/Secure Boot/kernel policy must remain unchanged. Loading and hardware tests require the designated disposable environment and a verified recovery path. The current safe target metadata query required neither reboot nor driver replacement. Planned native vertical execution and recovery acceptance are in [validation](validation-plan.md), [security](security-and-recovery.md) and [roadmap](roadmap.md).

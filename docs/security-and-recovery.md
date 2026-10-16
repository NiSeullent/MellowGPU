# Security, ownership and recovery — initial v1

Scope: MG-CORE-LIFETIME-001, MG-CORE-SYNC-001, MG-OS-XNU-001, MG-RECOVERY-001 and MG-PROVENANCE-001. This is an implementable contract and statement of current limits. No real GPU reset, process GPU isolation or native interrupt execution is certified.

## Boundaries and identity

Userspace supplies versioned fixed-size requests, never kernel object pointers. Validate the entire structure, reserved fields, operation, ranges/overflow, sequence, session, device epoch and object ownership before native calls or output mapping. Return cleared output on rejected internal backend operations. Native PCI ownership must come from the retained IOPCIDevice provider, not a caller-supplied vendor value or compatibility dictionary. Internal metadata adapters intentionally do not establish such ownership.

Contexts and queues must bind one device and generation. GPU virtual address spaces, mappings and command buffers need per-process access checks, resource residency validation and explicit limits before native GPU submission can be enabled. These GPU facilities are **not provided by the CPU control service**. Commands from untrusted applications must not become arbitrary MMIO, page-table writes or DMA addresses. Linux GEM/TTM/dma-buf/dma-fence/IRQ/workqueue behavior must receive separate native implementations and cannot be assumed from matching names.

## Current CPU control implementation

`Mellow/MellowGPUDevice.cpp` allocates its lock before IOService attachment and publishes initialized state under that lock. Failed start closes admission while retaining the state/lock through final object destruction. Requests require ready state. Concrete private IOBufferMemoryDescriptor objects provide bounded CPU backing; no vtable subclass interposition is used. Mapping is authorized for the original task only and is not exported to arbitrary clients. Limits bound tracked kernel-owned private backing and records, not all RAM, every Mach alias or eventual userspace retention.

Unknown unmap outcomes retain/quarantine the affected backing rather than reusing a consumed mapping state. Tests exercise actual adapter code with injected IOKit faults; they do not reproduce XNU VM aliasing, kernel scheduling or concurrent native races. NativeOperation leases here protect private CPU helper operations only; they do not acknowledge GPU DMA quiescence. The development guard remains explicit and passive. No change here grants general accelerator loading or bypasses platform security.

## Serialization and lock responsibilities

The common DeviceLifecycle and BackendInterfaces objects require one external owner/serialization boundary. Copying metadata bindings is not transfer of native ownership. Native adapters must retain backing before dropping a lock for potentially reentrant OS callbacks. Never hold a callback-sensitive global policy lock over reset/close callbacks or an unbounded fence wait. Admission must close synchronously when reset, stop or device loss is requested. Reentrant callbacks may escalate the goal; returning from an old reset must not reopen a stopping/lost device.

The current native CPU adapter has concrete locking; future GPU/IRQ/VM/display lock ordering is unresolved. Before enabling a native backend, publish an actual lock graph, interrupt context restrictions and timeout/cancellation behavior, then test races under native execution. Hosted single-threaded policy tests cannot establish native lock correctness.

## Submission uncertainty and retirement

| Boundary | Authority required | If unknown |
|---|---|---|
| Command published | Native queue/doorbell path with precise visibility | Retain command storage and publication owner |
| Execution completed | Exact queue/context fence, identity and reset generation | Do not report completion or retry implicitly |
| GPU mapping no longer referenced | Queue/VM/native teardown authority | Retain mapping, pins and DMA backing |
| Scanout no longer references surface | Display flip/disable retirement authority | Retain surface and display lease |
| Native control operation returned | The matching synchronous/asynchronous callback boundary | Keep NativeOperation lease |
| Entire device quiescent | Backend-specific DMA/IRQ/engine/display barrier | Close admission, retain all unresolved obligations |

GET==PUT, a semaphore value or a transport ACK alone is not a receipt for all these boundaries. An UNKNOWN publication must not be resubmitted because it might execute twice. Confirmed completion does not automatically release held native references. Timeout is an error classification, not proof that DMA stopped.

## Recovery state contract

`Drivers/Core/DeviceLifecycle.hpp` owns a 64-slot typed-lease policy. Running admits leases. ResetPending, StopPending and Lost close admission. Preparing quiescence requires NativeOperation/Publication owners to retire first, but permits pins/submission/display owners to remain held while native reset stops unknown traffic. A serial/device/generation-specific QuiescencePlan protects completion correlation.

UNKNOWN keeps the plan and backing retained. KnownNotQuiescent only ends that control attempt, keeps admission closed and does not retire GPU obligations. KnownQuiescent acknowledges the matching native boundary but leaves each separate lease alive. Superseded goals cannot be completed using an earlier reset result. Only after every owner retires may completeTransition stop permanently or reopen with a strictly newer generation. Counter exhaustion fails closed; serials/epochs never wrap into a valid stale handle.

The core does not invoke reset or interrupts. Vendor adapters must supply native authority; the initial three-vendor metadata interfaces return Unsupported for recovery. New Claude lifecycle/packet proposals are reviewed and tested independently before integration, and remain hosted model evidence until actual native callbacks exist.

## Source, build and test environment

Untrusted upstream paths and patches are inspected through bounded regular-file and descriptor-relative no-follow readers. Candidate Git provenance validates commit/tree/blob links and hashes; source/license/ABI observations must remain distinct from full-closure approval. Known restricted references stay reference-only; alias detection does not prove universal fork ancestry. Candidate staging does not automatically execute upstream hooks or build scripts.

No development-Mac EFI, boot disk, SIP, AMFI, Secure Boot or kernel policy changes are permitted. Physical loading requires a designated disposable target, exact baseline/recovery image, a verified route back to macOS and explicit artifact/OS identity. The current 15IGL7 metadata-only inspection changed none of those settings. Hardware recovery, invalid DMA, interrupt storms, sleep/wake and device-loss tests remain NOT_RUN; see [validation](validation-plan.md).

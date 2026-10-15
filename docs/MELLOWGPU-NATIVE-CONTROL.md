# MellowGPU native control boundary

Revision 1, 2026-10-16. Extends [the platform specification](MELLOWGPU-SPEC.md).
This is an implementation step toward native GPU memory, winsys and Metal
integration. CPU shared storage is a control/resource primitive; it does not
render pixels, publish GPU page tables or satisfy the acceleration profile.

## Native composition

The existing Intel-specific diagnostic user client remains a separate research
interface. A new `MellowGPUDevice` IOService discovers physical Intel, AMD and
NVIDIA display-class PCI devices. An explicit `-mellowgpudev` boot selection on
Darwin 24 selects this development path; `-mellowoff` and conflicting legacy
diagnostic/native modes reject it. The Lilu entry also skips legacy patch
registration in this mode. A separate matching category permits discovery
without replacing the framebuffer. There are no BAR accesses, PCI writes,
bus-master changes, display commits or engine submissions in this boundary.

The service owns the physical identity and lifecycle admission. Its clients
hold native-operation leases around blocking allocation/mapping calls. Device
stop closes admission before later calls. The service lock is never held across
descriptor allocation, mapping, unmapping or descriptor destruction. Client
state has its own sleepable lock; the lock order is client then service.
Stopping this passive service does not prove that another driver's GPU engines
are quiescent. No GPU quiescence receipt is synthesized.

## Versioned wire contract

`Drivers/Core/MellowGPUABI.h` is an original, fixed-width C ABI. Requests are
64 bytes and replies are 128 bytes on the selected little-endian x86_64 ABI.
Every request carries exact major/minor/size, operation, a nonzero increasing
request ID, a server-owned session and device epoch. Query bootstraps its session
with zero identity fields. All other operations require the exact current
session and epoch. Flags and reserved fields are zero in revision 1.

The control epoch is the native control IOService's unique registry-entry ID;
the PCI registry ID identifies its physical parent. A fresh service instance
therefore cannot reuse the previous service's serialized session/object tuple.
This passive epoch does not observe another driver's GPU reset and cannot
authorize GPU work. A future GPU owner must connect actual reset generation
and native resource invalidation before enabling those operations.

| Selector | Operation | Native effect |
| --- | --- | --- |
| 0 | DeviceQuery | Return the physical PCI identity, assigned session and current implementation capabilities |
| 1 | StorageCreate | Allocate and zero page-aligned kernel/user shared RAM, returning an opaque object |
| 2 | StorageClose | Revoke an unmapped object and release its owner reference |
| 3 | StorageMap | Map that object's descriptor into the retained original task only |
| 4 | StorageUnmap | Retire the owned native CPU mapping before releasing its reference |

The reply's CPU address is a user virtual address. It is never a DMA address or
GPU virtual address. GPU mapping, submissions, Metal and WindowServer support
remain absent from the capability mask. Unsupported operations return a
distinct error. The pure validator checks the wire shape and correlation; native
task authorization and actual memory operations are enforced by the user client.
Accepted valid request IDs are consumed before the native operation, including
native failure, so a retry cannot accidentally repeat an ambiguous operation.

`Tools/run-probe-control-tests.py --sanitize --out FRESH` executes the actual
userspace probe against injected IOKit replies. Mismatched physical parent,
session, epoch, identity and mapping length must be rejected before an
inaccessible CPU page can be touched. A normal create/map/remap/close case
checks pattern persistence and one connection close. This executes test-process
RAM and probe code; it does not call native IOKit or submit GPU work.

The client retains its original task and verifies the current task and
administrator privilege for each call. Mapping is an explicit external method
that fixes the destination to this original task. The generic
`clientMemoryForType` mapping path stays unavailable; the caller cannot select
another task through `IOConnectMapMemory`.

## Storage ownership

Allocation and quota reservation are separate from native publication. Every
descriptor is zeroed, page aligned and limited to 16 MiB. Clients have at most
16 objects and 64 MiB; the device has at most 16 clients and 256 MiB charged
to private owned backing records. These are control-object quotas, not a bound
on global physical RAM or arbitrary Mach VM aliases created by the owning task.
Opaque object IDs never wrap or alias a previous allocation in the same
session. No kernel pointer or physical page address enters the reply.

Native mappings retain their descriptors. A private client slot owns the
concrete descriptor, its explicit native map and the device backing charge.
There is no descriptor export, native getter or generic user-client mapping
interface through which an untracked descriptor reference can escape. Closing
requires successful unmapping, release of the tracked map, then release of the
private descriptor before the device backing charge is credited. Destruction
runs outside the service lock. A retained task may create other VM aliases;
this private descriptor rule does not establish retirement of those aliases.
Future GPU, IOSurface or kernel consumers must
add explicit persistent resource leases before sharing this backing; this
closed CPU-storage ownership rule cannot authorize an exported resource.

Mapped objects require successful native unmapping before close. An unmap error
preserves the mapping, descriptor, backing charge and client cleanup owner.
It cannot be repaired by calling unmap again and observing success: the selected
XNU implementation clears internal mapping state even when the first unmap
returns an error. Device/service removal and client exit do not turn unknown
retirement into success. This slice intentionally has no quarantine recovery API.

## Implementation and evidence plan

1. Parent: C wire layout, portable request validation and actual C/C++ negative
   tests before implementation; ASan/UBSan and freestanding compilation.
2. Native adapter worker: the actual IOService, IOUserClient, tracked descriptor,
   task-specific CPU mapping, Xcode/personality integration and tests compiling
   the real adapter against fault-injected IOKit boundaries. No source-shape-only
   assertion substitutes for execution of the adapter's code.
3. Metal inventory worker: SDK/Clang-derived API and availability inventory tied
   to actual header digests; unknown/unreviewed requirements remain unqualified.
4. Upstream worker: immutable candidate source/patch/config materialization with
   strict application and provenance, extending source discovery without
   production installation or hardware promotion.
5. Parent: public userspace control probe, fresh native compilation, independent
   review, applicable regressions, exact commit and GitHub publication. Runtime
   PCI/storage results require the target machine and remain separately recorded.

The full goal still requires actual vendor GPU VM/queue/fence/display owners,
matched Mesa winsys integration, the native Metal frontend, and the complete
hardware/system acceptance corpus. This boundary cannot qualify a device.

## Primary contracts

- [Apple IOUserClient declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/IOKit/IOUserClient.h).
- [Apple descriptor allocation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IOBufferMemoryDescriptor.cpp).
- [Apple memory mapping and unmapping](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IOMemoryDescriptor.cpp).

The repository's retained SDK declarations and the selected OS export inventory
must also match the compiled native boundary. Public declaration names alone do
not prove loadability on a different macOS build.

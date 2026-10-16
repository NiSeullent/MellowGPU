# ADR 0001: Common core and native platform boundaries

Date: 2026-10-16. Status: selected design; native GPU implementation and product
qualification pending. Requirements: `MG-CORE-ABI-001`,
`MG-CORE-LIFETIME-001`, `MG-CORE-SYNC-001`, `MG-OS-XNU-001`,
`MG-CPU-X64-001`, `MG-CPU-ARM64-001`, `MG-CPU-ARM64E-001`,
`MG-GPU-INTEL-001`, `MG-GPU-AMD-001`, `MG-GPU-NVIDIA-001`.

## Context

The same resource obligations must survive client exit, native callbacks, reset
and display use on different vendors. Linux algorithms depend on kernel services
whose semantics are not supplied by XNU header compatibility. CPU architecture
and GPU vendor are independent. Existing policy/model code is useful, but a
passing callback simulation cannot prove native DMA retirement.

## Alternatives

| Alternative | Benefit | Cost or rejected assumption |
| --- | --- | --- |
| Continue per-card Apple-driver patches | Reuses an existing integrated stack when the exact binaries exist | Private layouts/build assumptions spread across product logic; no general unsupported-GPU or vendor path |
| Expose the whole Linux kernel API as one universal compatibility layer | Potential upstream source reuse | Enormous semantic surface; no-op locks/preemption/fences or fake tasks can compile yet violate lifetime and ordering |
| Make a DriverKit-only universal GPU stack | Userspace fault isolation and public PCI transport | Complete Metal/WindowServer provider path and all required device operations are not established by PCI access alone |
| Freestanding policy + narrow C contracts + explicit native and generation adapters | Testable ownership/error model; independent vendor/CPU work; bounded source upgrades | More explicit native work and adapter tests; no automatic hardware support from code reuse |

Select the last option. DriverKit remains an evaluated native adapter candidate,
not an excluded transport and not an assumed complete graphics provider.
[Platform feasibility and primary Apple sources](../platform-support.md#driverkit-and-native-iokit-feasibility).

## Decision and contracts

Use freestanding C++ policy where it removes OS-dependent state from validation,
and fixed-width C-compatible records at native boundaries. Keep kernel and
userspace pointers, C++ ABI, locks, MMIO, task/thread objects and vendor command
payloads out of the shared wire ABI. The existing x86_64 kext is compiled with its
supported kernel toolchain/language subset; a portable-core build must not force
unsupported C++ runtime facilities into it. C++ language version and CPU ABI are
separate constraints.

The platform adapter owns actual native references and serialization. It
acquires separate operation/pin/submission/publication/display obligations and
never drops them on timeout alone. Quiescence is a native barrier followed by
resource retirement and a final state transition, not a single boolean reset
success. Externally supplied records never act as hardware attestation.

The current native control ABI implements only verified identity/session and
private CPU-storage operations. Its incarnation epoch is distinct from future
GPU reset epochs. GPU operations remain unsupported until an actual owner,
permissions, isolation, native mapping/fence semantics and tests exist. Per-vendor
extensions are allowed with negotiated applicability; the common API does not
force every backend into a lowest-common-denominator execution model.

Implement x86_64 first while keeping Intel, AMD and NVIDIA GPU lanes parallel.
ARM64 userspace tooling is independently useful. ARM64e kernel/PAC, native
interrupts, memory ordering and device access have separate acceptance gates;
universal-build success cannot satisfy them.

## Consequences and acceptance

An upstream import must identify its use mode, file/license/commit/patch hashes,
needed native services and tests. Unsupported services fail explicitly before
hardware publication. Every target tuple needs native compile/export checks,
lifetime fault/race tests and real resource/queue/fence execution. No callback
runs under a shared policy lock; blocking native transitions retain their owner
and use deferred completion where necessary.

Current tests prove parts of validation and policy; native GPU VM, submission,
quiescence and system graphics remain incomplete. This ADR does not authorize
loading code or changing the development Mac's security/boot settings.

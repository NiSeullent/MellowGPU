# Platform support and feasibility

Revision 1, 2026-10-16. This is a support contract and evidence inventory, not a
list of compatible products. **Qualified GPU/CPU/macOS tuples: none.** The
acceptance bar is the complete selected Metal 3 profile, real GPU WindowServer
composition, and the published application/game corpus. Experimental status
never permits advertising an unsupported system Metal device.

## Independent dimensions

A support record binds physical PCI identity and GPU generation, CPU/kernel ABI,
macOS product/build and kernel UUID, kernel-collection hashes, actual loaded
module UUIDs, userspace/runtime images, firmware, source/patch/configuration and
compiler revisions, and the requirement/test-corpus digests. A vendor name, CPU
family or macOS major version is insufficient. Intel as a CPU vendor and Intel
as a GPU vendor are separate dimensions.

| Host/ABI | Permitted current use | What remains required | Status |
| --- | --- | --- | --- |
| Intel x86_64, macOS 15.8.1/24H32 | First native target; current passive PCI/session/CPU-storage implementation and local build/export checks | Actual target execution of each native operation, GPU VM/submission, native fences, Metal/frontend/system display and full conformance | Experimental; `MG-CPU-X64-001`, `MG-OS-XNU-001` |
| x86_64, other macOS builds | Host logic/build work where tools are available | New exact KPI/family ABI, firmware/configuration and native hardware records | UNKNOWN / unqualified |
| arm64 userspace on Apple silicon | Tools, common-core tests and explicitly compiled userspace components | A separate native OS/hardware adapter and target evidence | Development host; `MG-CPU-ARM64-001` is not a GPU-support claim |
| arm64e Apple-silicon kernel | Feasibility design only | arm64e binary/ABI/PAC correctness, approved load policy, device access, DMA/IRQ/power, Metal and display integration | UNKNOWN; `MG-CPU-ARM64E-001` |
| Apple-silicon host with external Intel/AMD/NVIDIA GPU | Isolated future feasibility research | Every gate below; no official external-GPU route assumed | Not supported or qualified by MellowGPU |
| Apple integrated GPU backend | Separate proposed backend, not implied by arm64 compilation | Its own hardware/firmware/VM/display/compiler and licensing contracts | Out of the three external-vendor implementation lanes |

Apple documents arm64e as the Apple-silicon kext architecture. A universal
x86_64+arm64 userspace executable is useful build evidence, but contains neither
the required kernel architecture proof nor device/driver integration. A
universal kext must still have the correct target slice and satisfy all native
conditions. Rosetta does not turn an x86_64 kext into an arm64e kernel module.
[Apple kernel-extension architecture requirements](https://developer.apple.com/documentation/kernel/implementing_drivers_system_extensions_and_kexts),
[PCIe kext architecture guidance](https://developer.apple.com/documentation/kernel/hardware_families/pci/implementing_a_pcie_kext_for_a_thunderbolt_device).

PAC affects more than pointer width: function-pointer types, virtual calls,
C++/Objective-C++ ownership declarations, stored callbacks and return-address
conventions require exact compatible declarations. Serialized handles remain
fixed-width values, never signed pointers. No backend may strip PAC or call a
guessed vtable slot to bypass an ABI mismatch.
[Apple pointer-authentication guidance](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication).

## First physical target: 15IGL7

| Field | Observed/selected value |
| --- | --- |
| System | Lenovo 15IGL7 |
| CPU architecture / processor | x86_64 / Celeron N4120 |
| Physical display PCI vendor/device | Intel `8086:3185` |
| Revision / subsystem | `06` / `17AA:3E77` |
| GPU family route | Gemini Lake (GLK), i915 + ANV |
| macOS | 15.8.1, build `24H32`, Darwin 24.6.0 |
| Exact kernel UUID used by existing local collection evidence | `B7B3E8AD-9CA6-382C-BD56-8D52139342A4` |
| Current operational observation | Firmware `IONDRVFramebuffer`; `-nvoff`; no active NMX GPU/accelerator instance in the captured registry |
| MellowGPU product qualification | None; the old loaded NMX binaries are not a new MellowGPU runtime |

The machine is selected because an actual recoverable test environment and
physical PCI identity are available, and the GLK i915/ANV route is a concrete
source investigation. This does not establish that the N4120 can satisfy every
mandatory Metal 3 feature. A missing mandatory feature or system test leaves
this tuple experimental/ineligible even if it is useful for initial bring-up.
Intel `7D41` and the retained Xe modules belong to a different future lane;
they must not select code, firmware or conformance limits for `3185`.

Current module inventory can list mapped NMXGPU/NMXAccel binaries while their
IOService instance counts are zero. Mapped code, started owner, successful attach,
selected Metal device and completed GPU work are distinct observations. Capture
boot identity and both module/registry state before interpreting a result.

### Apple compatibility entries are not physical GPUs

The current `IOServiceCompatibility` display/IOFB/IOAccelerator dictionaries
exactly match the Apple BootKC personality of
`com.apple.iokit.IOKitRegistryCompatibility`, version 1, UUID
`09C98BCE-6440-33FD-B698-000E42330B3F`. Its default entries include AMD
`1002:6810`, model `Unknown Unknown`, `AGXMetalA12` and synthetic VRAM values.
These fields are compatibility-view metadata, not evidence of an AMD device,
AGX hardware or a third-party spoof. Physical admission must resolve the real
`IOPCIDevice` and native provider, not these nested properties.

Local comparison record (outside the shipped source):
`work/15igl7-compatibility-provenance/provenance.json`, SHA-256
`b630f3ed99e74c1874a218cf7a8aeb75b7b761809916462cd92354324f9e4eb0`.
Its source BootKC SHA-256 is
`cd013c14a7f21dbb8181fb06c44e079de8646b6a674abe21f6e47f42e8979af3`.
Apple XNU release `xnu-11417.140.69` resolves to commit
`43a90889846e00bfb5cf1d255cdc0a701a1e05a4`.
In [`iokit/Kernel/IOService.cpp`](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/iokit/Kernel/IOService.cpp#L113),
`OSDefineMetaClassAndStructors(IOServiceCompatibility, IOService)` defines its
native class metadata; [`IOService::copyExistingServices()`](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/iokit/Kernel/IOService.cpp#L6097)
enumerates compatibility instances for `gIOCompatibilityMatchKey`, and
[`IOService::matchInternal()`](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/iokit/Kernel/IOService.cpp#L7642)
uses `gIOCompatibilityPropertiesKey` for compatibility matching.
In [`iokit/Kernel/IOUserClient.cpp`](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/iokit/Kernel/IOUserClient.cpp#L422),
the static predicate `IOTaskRegistryCompatibility(task_t)` returns false;
`IOTaskRegistryCompatibilityMatching()` tests it before enabling compatibility
matching. Shipping per-task policy remains UNKNOWN from this published stub.
No removal or boot-setting change follows from this observation.

## Observed Metal metadata

A read-only x86_64 process recorded these **instance** methods of
`MTLIOAccelDevice` on the actual target. They are private runtime metadata, not
an authorized or implemented driver interface:

| Selector | Actual encoding | Observed return / extra argument |
| --- | --- | --- |
| `initWithAcceleratorPort:` | `@20@0:8I16` | Object / one unsigned-int argument |
| `registryID` | `Q16@0:8` | Unsigned 64-bit integer |
| `sharedRef` | `^{__IOAccelShared=}16@0:8` | Opaque `__IOAccelShared` pointer |
| `deviceRef` | `^{__IOAccelDevice=}16@0:8` | Opaque `__IOAccelDevice` pointer |

The declaring/implementation image is Metal.framework UUID
`C3EFBABE-6B06-33AB-BC45-43FE86297AA1`; IOAccelerator.framework UUID is
`E2BAC781-B505-38FC-A912-FC0B204F4802`. `_MTLDevice` also has the observed
initializer, while `AGXMetalDevice` was not found in that process. The record did
not call public device enumeration, invoke private initializers, instantiate a
private driver subclass, read device values, create command buffers or submit
GPU work. Its `darwin24_x86_64_target_abi_verified` flag is false. In particular,
its empty `public_devices` array is not an enumeration result because enumeration
was not called.

Local record: `work/15igl7-current-metal-runtime-metadata.json`, SHA-256
`fb33772998988b17a5ab88cb4011b60bb29137b1214504de8fde9abda7d99536`.
These x86_64 encodings are not ARM64/ARM64e declarations. Behavior, ownership,
error results, teardown, virtual interface layout, entitlement expectations and
system registration must still be proved independently for an exact runtime.

## DriverKit and native IOKit feasibility

| Route | Established basis | Unresolved MellowGPU obligations |
| --- | --- | --- |
| PCIDriverKit dext | Public PCI device/configuration/memory access and an entitlement-based userspace driver model on Intel and Apple silicon | Actual entitlement approval, required DMA/interrupt/power coverage, usable GPU family/Metal registration, IOSurface/fence integration and WindowServer/display ownership |
| Native IOKit kext | Existing x86_64 service/client/CPU-storage code and native build/KPI-export checks | GPU owner, VM/IRQ/firmware/PM/display implementation, exact private-family ABI where needed, appropriate signing/approval, target execution and conformance |
| Codeless matching | Match already supported hardware to an existing system driver | Cannot add missing GPU execution/compiler/display behavior |
| Mixed dext + narrow kext | Potential isolation of compiler/control policy from privileged hardware ownership | Versioned authenticated transport, shared resource lifetime, native callbacks and a complete independently qualified graphics route |

Apple's PCI DriverKit availability does not certify external GPU support or a
third-party replacement for its graphics stack. The native-kext route is a
conditional research choice, not an assurance of ordinary deployment: exact
exports, class ABIs, OS build, signature/approval and target boot policy must all
permit it. User-approved loading/security requirements vary with platform;
Apple silicon additionally uses LocalPolicy/AuxKC approval. This document does
not request or perform any policy change.
[PCIDriverKit](https://developer.apple.com/documentation/pcidriverkit),
[DriverKit signing/entitlement requirements](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development),
[Apple kernel-extension security model](https://support.apple.com/guide/security/kernel-extensions-sec8e454101b/web).

Before any hardware path, verify exact strong/weak imports and declared
providers against the target kernel collections and actual dependency modules;
compile success cannot supply a missing export. Validate native IOKit object
lifetime and public method signatures, signed bundle/Info/KMOD coherence and
firmware/license inputs. An ad hoc strict signature only verifies integrity;
it supplies no Apple certificate, approval, GPU execution or system support.

## Apple-silicon external GPU feasibility gate

Apple's published eGPU support requires an Intel-based Mac. Therefore this
project does not assume an officially supported external Intel/AMD/NVIDIA GPU
route on Apple silicon.
[Apple eGPU requirements](https://support.apple.com/en-gb/102363).
PCIDriverKit availability and Thunderbolt enumeration are separate facts.

Each proposed Apple-silicon experiment must resolve, in order:

1. A designated recoverable lab machine and authorization distinct from the
   development Mac; actual device link and PCIe/Thunderbolt enumeration.
2. Its permissible driver architecture, signing, entitlements and loading policy.
3. Native BAR/config access, IRQ delivery, DMA/IOMMU domain/coherency, address
   width, isolation and invalidation; reset and power ownership.
4. Firmware, kernel/backend/winsys/compiler and CPU/PAC compatibility.
5. Actual Metal device registration and resource/fence interoperability, followed
   by WindowServer compositor selection and native display retirement.

An UNKNOWN or failed prerequisite stops that experiment and leaves support
unqualified. This gate must not block the independently buildable x86_64 lane.
An Apple integrated GPU backend would need its own requirements and source
review, not an alias of the external-vendor adapters.

## Development-machine and evidence boundaries

The development Mac is for source inspection, builds, host tests, metadata reads
and packaging within the authorized scope. Its EFI, boot disk, SIP, AMFI, Secure
Boot and kernel policies are not changed. Driver loading and hardware tests
belong only to an explicitly designated disposable/recoverable environment with
an independent recovery path. Nothing in this document authorizes installation,
NVRAM changes or a new external automation schedule.

Host policy/model tests, native compile/link/export checks, task-specific native
CPU storage, genuine offscreen GPU work, cross-process surfaces, compositor
composition and scanout/flip are separate evidence stages. Results unavailable
on actual hardware remain NOT_RUN or UNKNOWN. Firmware output, the Apple
compatibility phantom and legacy research results cannot promote the Metal 3
or WindowServer gates.

# Source analysis and provenance boundaries

Observation date: **2026-10-16**. This is the research companion to
[the specification](MELLOWGPU-SPEC.md), [Metal conformance](metal3-conformance.md),
[source intake](MELLOWGPU-UPSTREAMS.md), [candidate materialization](MELLOWGPU-CANDIDATES.md)
and [licensing review](LICENSING.md). It does not change a source pin, license,
native admission policy or supported-device list. No upstream source is copied
into MellowGPU by this analysis.

## Evidence vocabulary and local baseline

`README-CLAIM` means an upstream author's published statement. `SOURCE-OBSERVED`
means the named code or contract was inspected at the linked commit.
`TEST-OBSERVED` requires the exact test command and output; source containing a
test is not a test result. `HARDWARE-OBSERVED` requires the exact physical tuple
and raw result. `UNKNOWN` is neither a successful result nor proof of absence.
`NOT_RUN` describes an experiment that this analysis did not execute.

MellowGPU existed before this work. The inspected repository HEAD was
`1247ea16e1fe780f9d3bcf8847ff4315d30a3d3b` (2026-10-16; explicit Apple-target AST
fixtures), with concurrent uncommitted inventory/intake changes kept separate.
Existing `Drivers/`, `Runtime/`, `Userspace/` and license history are retained.
[Native control](MELLOWGPU-NATIVE-CONTROL.md) implements a limited IOKit/CPU-storage
boundary; [the lifecycle core](MELLOWGPU-LIFECYCLE.md) is externally serialized
policy. Neither implements a complete native GPU backend or certifies Metal.

## Rechecked repositories: discovery is not adoption

The following primary repository and default-branch metadata was fetched on
2026-10-15 at 23:58 UTC. These are observed branch heads, not promises that a
remote will remain unchanged. GitHub reported all four GitHub repositories
public, enabled and not archived. Mesa's public GitLab response identified
`mesa/mesa` and `main`; its returned metadata omitted an archive flag, so that
flag is `UNKNOWN` rather than inferred false.

| Project | Observed default branch HEAD and date | Selected analysis/build-reference pin |
| --- | --- | --- |
| [NullMoth](https://github.com/nullmoth/nvidia-macos-driver) | `d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb`, 2026-10-15, “Driver and Mac app 2.0.2” | Same v2.0.2; reference-only |
| [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) | `d1f7bad8408a0a2c016b067c477422975a59de44`, 2026-10-15 | Same; reference-only |
| [Linux](https://github.com/torvalds/linux) | `2f2f7eeef8b0a1a9b9ca998231419873ccae8d4e`, 2026-10-15 | Existing selected subsets remain `0d9ff90a5422cc7509258aaaba1e7481df4d332a` |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) | `8f491f606a1e5b8d492151c2b1e4d702e264f0bf`, 2026-10-15 | Existing selected subset and Cobalt patch base `f5cb8ee032adabef599ae892ec4e42d796b84da9` |
| [NVIDIA open modules](https://github.com/NVIDIA/open-gpu-kernel-modules) | `ca2d03e6fafd4d4b56b383497d931c25b283bfcf`, 2026-10-07, 615.78.08 | Existing research pin `e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb`, 610.57.04 |

Reproduction endpoints are each GitHub repository's `/repos/OWNER/REPO` and
`/repos/OWNER/REPO/commits?per_page=1` under `https://api.github.com`, and Mesa's
`https://gitlab.freedesktop.org/api/v4/projects/mesa%2Fmesa` plus
`/repository/commits?ref_name=main&per_page=1`. Record the response digest and
observation time, then verify an immutable commit and selected blobs with the
existing intake tools. A changed head is an update candidate only (`MG-UPSTREAM-001`).

The exact selected subset is recorded in [the existing lock](../porting/upstreams.lock.json).
It is not a complete kernel, userspace, generated-header or firmware dependency
closure. Newer discovered heads in this table have not been substituted into it.

## NullMoth v2.0.2: two kernel routes and a Metal-on-Vulkan frontend

All links in this section are pinned to
[`d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb`](https://github.com/nullmoth/nvidia-macos-driver/tree/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb).
Its main README describes a NVIDIA Turing-and-newer product developed primarily
on one RTX 5060; those are **README-CLAIMs**, not a MellowGPU support list.
The additional AMD/Intel lane is separately described in
[`gpu-port/README.md`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/README.md).

| Boundary | SOURCE-OBSERVED evidence | Consequence for MellowGPU |
| --- | --- | --- |
| Kernel/userspace | [`gpu-port/xkpi/xnu/nmx_kext.cpp`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/xkpi/xnu/nmx_kext.cpp): `NMXGPU::probe`, `start`, `attachNow`; Apple-header C++ calls the C `nmx_port_*` boundary. NVIDIA has a separate `kexts/NVRM`/`NVAccel` route. | Preserve vendor-native owners and a small typed OS boundary. The RM transport and DRM transport are different ABIs. |
| Initialization/firmware | `opted_in` honors `-nvoff` and per-vendor opt-in. `attachNow` waits for the firmware root, uses a persistent attach marker, calls `nmx_port_attach`, then publishes the claim and framebuffer. [`bsdcompat/src/nmx_firmware.c`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/xkpi/bsdcompat/src/nmx_firmware.c) adapts firmware access. | Firmware protocol, bytes, location and admission are release inputs. Successful module loading is not successful GPU attachment. Render attachment may already alter display state before IOFramebuffer publication. |
| Memory/mapping | [`xnu/nmx_mm.cpp`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/xkpi/xnu/nmx_mm.cpp): `nmx_xnu_mm_note_task`, `nmx_xnu_mm_copy` use current-task copy or prepared IOMemoryDescriptor copies. [`patches/libdrm-macos.patch`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/patches/libdrm-macos.patch): `drmNmxMmap`, `drmNmxMunmap`, private map/unmap ioctls. | Linux mmap offsets are opaque handles, not host pointers. Pin, file, task, BO, partial-unmap and cache-policy lifetime need explicit contracts and native tests. A copy descriptor is not automatically a persistent DMA import. |
| Metal/shaders | [`plugin/NVMTLDevice.m`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/plugin/NVMTLDevice.m): `NVMTLDevice`, `initWithAcceleratorPort:`, `supportsFamily:`. [`plugin/nvmtl_vk.c`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/plugin/nvmtl_vk.c) creates Vulkan resources/devices and submits queues; `translator/` converts AIR toward SPIR-V; NVK/ANV/RADV supply their own vendor compiler paths. | Method/feature declarations are not conformance. AIR version, translation semantics, Vulkan features, vendor codegen and native completion must be qualified together (`MG-METAL-001`, `MG-SHADER-001`). |
| IOSurface/WindowServer | [`plugin/NVMTLObjects.m`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/plugin/NVMTLObjects.m): `nvmtlSurfaceOut` has a VRAM dirty/export branch and a locked IOSurface image-readback branch. `nvmtl_vk_headless` treats AMD/Intel surfaces as copy-out paths. | Do not generalize NVIDIA sharing to Intel/AMD or call all paths zero-copy. Native sharing needs authenticated producer fences and actual allocation identity, not IOSurface seed alone (`MG-SURFACE-001`). |
| Display | [`xnu/nmx_fb.cpp`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/gpu-port/xkpi/xnu/nmx_fb.cpp): `NMXGPUFB`, framebuffer registration, a timer-generated VBL interface and bridge calls. | An IOFramebuffer and synthetic VBL do not prove real flip IRQ/retirement or GPU WindowServer composition. Preserve hardware display owners and measure the actual present path (`MG-WS-001`, `MG-DISPLAY-001`). |
| Synchronization/recovery | `nvmtl_vk_zero_dummies` sets its done/layout bookkeeping before actual fence completion and destroys its fence before checking a failed wait. `NMXGPU::stop` polls attach state for a finite interval before detach; PM directly calls `nmx_port_power`. | Original MellowGPU code must retain unknown-flight resources, publish success only after real completion, distinguish native quiescence from final reference drain, and avoid timer/timeout-based ownership release. These bounded source observations are not a whole-project audit. |
| Private ABI/build | The Metal superclass, IOAccelerator interfaces, XNU scheduler bridges, build configuration and external FreeBSD/Linux roots are OS/build-sensitive. GPU README's Mesa/libdrm labels include abbreviated revisions and external object roots. | Pin complete source/config/compiler/KDK/runtime identity. Do not adopt an experimental object cache as a reproducible source build. |

`gpu-port/README.md` reports a 41-chip compute-pipeline matrix using Mesa noop
DRM shims. Its scope is simulated device/compiler coverage; this analysis did
not rerun it, and it supplies no physical display or game qualification.
Repository-wide correctness, latency, recovery and complete Metal conformance
are **UNKNOWN** from those claims. Potential bottlenecks to measure include
IOSurface readback/copy, CPU synchronization, queue serialization, shader
translation/cache misses and per-frame allocation. No relative performance gain
is established here.

## Cobalt64: generation-specific AMD bring-up with a private RADV transport

All links in this section use
[`d1f7bad8408a0a2c016b067c477422975a59de44`](https://github.com/Almosst-DEV/Cobalt64/tree/d1f7bad8408a0a2c016b067c477422975a59de44).
The [README](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/README.md)
claims a GPU-composited three-monitor desktop and selected apps on one RX 9070 XT
PC, macOS 26.6.2. It also calls the project a preview and identifies incomplete
display modes, frame pacing and app coverage. This analysis did not reproduce
that hardware result: independent hardware qualification is **UNKNOWN**.

| Boundary | SOURCE-OBSERVED evidence | Reuse or redesign decision |
| --- | --- | --- |
| Kernel/init/firmware | [`src/navi48-bringup/src/Navi48Bringup.cpp`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/src/navi48-bringup/src/Navi48Bringup.cpp): `Navi48Bringup::start`, `stop`, PSP/MES and native-display operations. [`BUILDING.md`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/BUILDING.md) names ten AMD firmware inputs and a separately obtained MacKernelSDK. | Treat Navi48/GFX12/DCN4.1/PSP/SMU/MES as generation-specific code. Neither firmware names nor display register assumptions apply to all AMD GPUs. Reproducible firmware/configuration closure is still required. |
| UAPI/memory/submission | [`Navi48NativeABI.h`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/src/navi48-bringup/src/Navi48NativeABI.h): `N48N_UC_TYPE`, major 1/minor 12, `N48N_SEL_BOCREATE`, `GEMVA`, `SUBMIT`, `WAITSEQ`, `BO_IMPORT_HOST`. [`Navi48NativeClient.cpp`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/src/navi48-bringup/src/Navi48NativeClient.cpp): `externalMethod`, exact selector shapes and caller-role restrictions, `clientClose`. | Its private IOKit handle/session/relative-wait contract is not generic DRM or ANV. Study bounded validation, quotas and task ownership; define original device/generation-scoped MellowGPU contracts and verify native lifetime separately. |
| Userspace backend | [`mesa-patches/README.txt`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/mesa-patches/README.txt) pins nine ordered RADV Darwin patches to Mesa `f5cb8ee…`; patch 0004 routes host import through `N48N` rather than a Linux DRM node. | A winsys alternative is feasible in source. Do not combine N48N handles with Linux libdrm/ioctl assumptions. README's minimum ABI “1.11 or newer” is not exact equality with native 1.12. |
| Metal/compiler | [`Navi48Device.m`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/tools/native/navi48metal/Navi48Device.m): `Navi48Device : MTLIOAccelDevice`, Vulkan resource/queue operations. [`n48_xlate.h`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/tools/native/navi48metal/n48_xlate.h): translation/cache identity, environment checks, bounded input sizes, worker timeout states and a distinct WindowServer path. | Reuse the lesson that compiler identity, OS build, overrides and failures belong in cache keys. No cached shader, selector count or fallback shader is a complete Metal implementation. Translation outputs must retain their licensing and semantics. |
| Surfaces/display/fences | [`n48_ioalias.h`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/tools/native/navi48metal/n48_ioalias.h) tracks aliases; [`n48_dispflip.h`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/tools/native/navi48metal/n48_dispflip.h): `n48df_complete_seq` accepts a successful, ordered GPU completion before requesting present. It is explicitly a D-copy present state model. Native scanout selectors are separately versioned. | Separate render completion, copy completion, display latch and old-surface retirement. A GPU copy is different from a CPU frame copy, but neither is proof of zero-copy. Tests of the state model do not prove IRQ/DMA/cache behavior. |
| Recovery/private ABI | Native user-client close/disarm paths and display restore/watchdog logic exist. [`tools/ioaccel-layout/README.md`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/tools/ioaccel-layout/README.md) documents per-KC layout extraction, unknown return types and validation only on 26.6.2 x86_64. | A measured vtable slot is not a verified method signature or ownership rule. Require exact build/architecture/class/export/return-type proof; never default an unknown return type to a callable production ABI. Native stop/reset/sleep correctness remains a hardware gate. |

The tree contains host tests, fault/“planted break” tests and device-side tools.
Their existence is SOURCE-OBSERVED; this analysis executed none of Cobalt's tests
or hardware tooling. Its publication and render paths must not be relabeled
MellowGPU implementations. Translation, pipeline compilation, serial completion
queues, import caching, descriptor reuse and display copies are measurable
optimization candidates, not an established performance comparison.

## Linux, Mesa and NVIDIA: explicit import roles

| Source family / exact contract | Selected role | Native boundary still required |
| --- | --- | --- |
| Linux i915 `i915_driver_probe` in [`i915_driver.c` at `0d9ff90…`](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/i915/i915_driver.c) | Proposed per-file port/behavior reference for the GLK lane; no complete imported backend claimed | PCI/BAR, memory/cache/DMA, IRQ, GEM, native work/RCU, firmware, reset, PM and Intel display |
| Linux Xe `xe_device_probe` in [`xe_device.c` at the same pin](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/xe/xe_device.c) | Existing selected algorithms/host-test oracles; separate generation lane | GuC/VM/bind/scheduler/firmware and display contracts; not a substitute for GLK i915 |
| Linux amdgpu `amdgpu_device_init` / recovery in [`amdgpu_device.c`](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c) | Per-generation source/patch-set candidate | TTM/GPUVM, PSP/SMU/GFX/SDMA/MES differences, DC/display, reset and PM |
| Mesa [`anv_CreateDevice`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/f5cb8ee032adabef599ae892ec4e42d796b84da9/src/intel/vulkan/anv_device.c), [`radv_CreateDevice`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/f5cb8ee032adabef599ae892ec4e42d796b84da9/src/amd/vulkan/radv_device.c), [`nvk_CreateDevice`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/f5cb8ee032adabef599ae892ec4e42d796b84da9/src/nouveau/vulkan/nvk_device.c) | Proposed userspace dependencies plus reviewed Darwin winsys patches; selected lock files alone are not whole builds | Matching UAPI, actual device features/compiler, BO mapping/import, timestamp/errno and native sync semantics |
| Nouveau/NVK/NAK; Mesa [`nak_compiler_create` / `nak_compile_shader`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/f5cb8ee032adabef599ae892ec4e42d796b84da9/src/nouveau/compiler/nak/api.rs) | Kernel/userspace/compiler source lane and test oracle where applicable | Matching Nouveau channel/VM/fence/firmware interface; support varies by generation |
| NVIDIA open RM at selected `e4a5faa…`; latest [`README` at `ca2d03e…`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/ca2d03e6fafd4d4b56b383497d931c25b283bfcf/README.md) | Alternate research dependency, separate from Nouveau | NVIDIA requires matching release userspace/GSP; its Linux x86_64/aarch64 build support is not macOS/ARM64e support. NVK cannot use RM objects merely because both address NVIDIA hardware. |
| DRM GEM/TTM, dma-buf, dma-fence, syncobj, scheduler | Behavioral contracts and separately reviewed source subsets | Native ownership, prepared memory, DMA/IOMMU, reservations, fence signaling/waits, cancellation and reset; no no-op emulation of execution guarantees |

The [DRM memory](https://docs.kernel.org/gpu/drm-mm.html) and
[dma-buf synchronization](https://docs.kernel.org/driver-api/dma-buf.html)
documentation distinguishes shared allocation lifetime from execution ordering.
Linux objects, IRQ/workqueue/RCU context and kernel clocks do not exist in XNU
by naming a wrapper after them. Every imported call needs a complete native
implementation or an explicit unsupported boundary; a source parser is not one.

## License and redistribution observations

`MG-PROVENANCE-001` requires review of the exact compiled closure, not just the
repository metadata's license label. GitHub returned `NOASSERTION` for several
mixed-license repositories; that is not a grant and not evidence of no license.

| Source | Rechecked controlling evidence | Current treatment |
| --- | --- | --- |
| NullMoth | [`LICENSE`](https://github.com/nullmoth/nvidia-macos-driver/blob/d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb/LICENSE), `NOTICE`, `translator/LICENSE`; PolyForm Noncommercial for its main components, LGPL-3.0-or-later translator, separately licensed Mesa/libdrm modifications and firmware | Reference-only; no source copied/relicensed by this analysis |
| Cobalt64 | [`LICENSE`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/LICENSE) MIT, [`src/navi48-bringup/LICENSE`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/src/navi48-bringup/LICENSE) BSD-3-Clause, per-file notices, [`translator NOTICE`](https://github.com/Almosst-DEV/Cobalt64/blob/d1f7bad8408a0a2c016b067c477422975a59de44/third-party/metal2vulkan-n48/NOTICE) LGPL derivative at `43c46ac8…` | Reference-only in the current lock; do not flatten the tree to one MIT label |
| Linux | [`COPYING`](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/COPYING), per-file SPDX/notices and dependency closure | GPL-2.0/syscall-note context plus mixed per-file terms; permissive leaf files do not settle the linked closure. A syscall exception is not permission to link arbitrary kernel code under another license. |
| Mesa | [`docs/license.rst`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/f5cb8ee032adabef599ae892ec4e42d796b84da9/docs/license.rst) directs review to individual component/file licenses | Selected MIT/other notices remain intact; no blanket compatibility or formal Vulkan conformance claim |
| NVIDIA | [`COPYING`](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/ca2d03e6fafd4d4b56b383497d931c25b283bfcf/COPYING) distinguishes MIT source and dual MIT/GPLv2 material | Review selected branch, headers and complete artifact; GSP/userspace/firmware terms are separate |

Firmware records must include vendor source, exact bytes, protocol, notices and
the permitted distribution mode. Existing MellowGPU policy is fetch-and-verify,
not bundled firmware ([LICENSING](LICENSING.md)); it is not a universal legal
judgment about every blob. Original Mellow TSNPL, Lilu BSD and SDK/APSL notices
remain controlling. Process/module separation alone does not resolve license
compatibility; unclear rights stay reference-only.

## First physical lane and the unresolved native ABI

The user-designated connected **15IGL7** was observed as Intel N4120, x86_64,
macOS **15.8.1 / 24H32**, PCI **8086:3185 rev 06**, subsystem **17AA:3E77**.
That identifies the initial i915/ANV GLK investigation lane. It does not make
the device eligible for full Metal 3; its complete required-feature/family
review remains **UNKNOWN**. Intel CPU priority is independent of GPU vendor.
Other vendors' backends continue separately. A Xe/7D41 test or simulation is
not evidence about this PCI tuple.

The exact Linux source identification is `INTEL_GLK_IDS` containing `0x3185` in
[`include/drm/intel/pciids.h` at `0d9ff90…`](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/include/drm/intel/pciids.h),
used with `glk_info` by `drivers/gpu/drm/i915/i915_pci.c`. The header's Git blob is
`dff389b56eb3bf0ebcf9d8a81c2e76722a0ea54f`, SHA256
`102d8f821c9464ad17c0160535a4e3811f49609f199f1b6d7268050d71d8e8f0`.
This is a source-family match, not evidence of native functionality.

An original, metadata-only helper was actually executed natively on this target.
The observed Metal image UUID was `C3EFBABE-6B06-33AB-BC45-43FE86297AA1`.
`MTLIOAccelDevice → _MTLDevice → NSObject` was found; the four directly declared
instance encodings were `@20@0:8I16` (`initWithAcceleratorPort:`), `Q16@0:8`
(`registryID`), `^{__IOAccelShared=}16@0:8` (`sharedRef`) and
`^{__IOAccelDevice=}16@0:8` (`deviceRef`). The output SHA256 was
`fb33772998988b17a5ab88cb4011b60bb29137b1214504de8fde9abda7d99536`;
helper source `3ffd32bae6cbdead34a9ccb3c51f1e88d5fd996fbbe380db584233b151c3ba91`.

This proves those metadata observations only. No private initializer, return
value, device registration, GPU submission, Metal 3 capability, IOSurface fence
or WindowServer execution was tested by that helper. The current safe boot's
firmware framebuffer and loaded modules are not GPU acceleration. Separate
arm64/macOS 26.2 measurements remain supplementary; a universal executable and
similar encoding strings do not prove ARM64e pointer authentication, C++ layout,
kernel loading or cross-build private ABI compatibility.

The original helper is retained at
[`Tools/metal-runtime-metadata.mm`](../Tools/metal-runtime-metadata.mm), with the
[sanitized target output](../spec/evidence/15igl7-metal-runtime-metadata.json)
and [bounded receipt](../spec/evidence/15igl7-metal-runtime-metadata.receipt.json).
The receipt records OS/build and a digest of those identity fields; no OS binary
hash was measured, and its `os_binary_sha256` is explicitly null. The helper's
default mode performs no device enumeration. Its separate explicit
`--enumerate-public-devices` mode only calls the public factory for Apple's
existing devices; neither mode invokes a recorded private selector.

The committed original-class fixture is never regenerated by its runner:

```sh
python3 Tools/run-metal-runtime-metadata-tests.py \
  --sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk \
  --sanitize --out build/metal-runtime-metadata-tests
```

It checks the actual helper's declaring-owner/type/absence logic and that fixture
initialization and methods are not invoked. It does not enumerate a device or
test a native Metal implementation. The runner requires a fresh scratch output
and a native macOS architecture; no cross-architecture execution is implied.

## Resulting engineering decisions and open evidence

Keep upstream algorithms vendor/generation specific; implement original shared
identity, leases, clock/error semantics and support policy. Compare a reviewed
Metal-to-Vulkan frontend against a direct native frontend using the same semantic
corpus, native ABI gates and measurements. The former can reuse compiler/resource
work but adds winsys/translation/feature-mapping boundaries; the latter avoids a
translation layer but requires complete native compiler/runtime ownership.
Neither route bypasses WindowServer, display or private ABI requirements.

The first vertical path is authenticated device discovery → native allocation
and mapping → actual command submission/fence → genuine shader result → native
IOSurface producer/consumer ordering → independent display commit/retirement.
Each step must preserve the previous safe display and report unsupported before
an unimplemented boundary. Full conformance, app/game coverage, zero CPU frame
copies, sleep/wake/reset robustness and comparative performance are **NOT_RUN
for MellowGPU** on this tuple. Current supported-profile count remains **zero**.

# Validation plan and actual evidence classes

All support remains experimental; qualified profile count is zero. MG-* contracts live in [requirements.json](../spec/v1/requirements.json). Every actual receipt must bind the command, complete relevant source/dependency hashes, compiler/SDK, OS/build, CPU/process/kernel architecture, exact PCI GPU/firmware tuple, output and failures. A filename containing “result” or a referenced test is not an execution receipt.

## Evidence levels

| Level | What it proves | What it cannot prove |
|---|---|---|
| Source/specification observation | Exact source bytes, paths, symbols, declarations or runtime metadata | Correct execution or permitted complete dependency closure |
| Hosted contract/negative tests | Actual production policy code under injected dependencies | Native IRQ/VM/DMA, physical GPU or display correctness |
| Native compile/sign/export inspection | Artifact structure and target symbol availability | Loading, initialization, acceleration or performance |
| Native safe metadata query | Exact OS class/image/type metadata and PCI identity | Private initializer semantics, Metal registration or GPU commands |
| Native GPU vertical test | One exact GPU path with correct output and native retirement | Complete Metal 3, WindowServer or a supported device |
| Full system qualification | All applicable mandatory cases and declared system/application suites | Unlisted apps/games, other GPUs/OS builds or optional features |

## Repeatable local and CI commands

Run from the repository root, with fresh scratch output directories:

```sh
python3 Tools/mellowgpu-spec.py check
python3 -m unittest discover -s tests -p specification_tests.py -v
python3 Tools/run-backend-interface-tests.py --sanitize --out build/backend-interface-validation
python3 Tools/run-probe-control-tests.py --sanitize --out build/probe-validation
python3 Tools/run-host-tests.py --out build/host-validation
python3 -m unittest discover -s tests -p metal_api_inventory_tests.py -v
TMPDIR=/private/tmp python3 -m unittest discover -s tests -p 'test_mellow_*.py' -v
python3 Tools/mellow-upstream.py validate
```

On Linux use its native canonical temporary directory; `/private/tmp` is a macOS-specific firmlink avoidance for the repository's existing no-symlink test policy. Actual Linux CI and macOS local outputs are separate evidence. Inventory tests invoke real Clang fixtures but execute no GPU. The specification checker runs no referenced test and does not validate every Metal semantic case. Fresh report directories and input hashes avoid reusing old passing receipts after source changes.

## Actual first target and completed safe observation

15IGL7 was selected because the user connected and designated it, USB SSH is available without repeated passwords, and actual native IOPCIDevice/CPU/OS identity can be inspected. N4120/GLK `8086:3185` rev06/subsystem17AA:3E77 is the relevant physical tuple; firmware framebuffer output and a spoofed Apple model are recorded separately. This target does not substitute for AMD/NVIDIA hardware or newer Intel Xe.

Native x86_64 macOS15.8.1/24H32 inspection found MTLIOAccelDevice → _MTLDevice → NSObject and four method type encodings: initWithAcceleratorPort `@20@0:8I16`, registryID `Q16@0:8`, sharedRef `^{__IOAccelShared=}16@0:8`, deviceRef `^{__IOAccelDevice=}16@0:8`. The helper did not enumerate public devices, call private initializers, create command buffers or submit GPU work. Metal image UUID is C3EFBABE-6B06-33AB-BC45-43FE86297AA1. The exact measurement belongs only to that build/architecture; it is not a verified complete IOAccelerator ABI.

The observed compatibility AMD1002:6810/AGX entries matched Apple's built-in IOKitRegistryCompatibility metadata. They are not another installed physical GPU and must not be removed as a supposed third-party spoof. An actual GPU-owner service instance and native MellowGPU GPU execution were absent. Current old-stack module loading and panel output are not MellowGPU qualification.

## First planned native vertical path — NOT_RUN

The first real GLK milestone is a bounded, non-display render/compute or GPU copy operation using the exact i915/ANV-generation contract, native GPU-owned memory/VM, validated command storage and a native fence. Seed input in a CPU-visible staging resource, submit one immutable bounded job, await correlated hardware completion, verify result and prove native retirement before release. CPU writes/readback are allowed as test setup/verification, never counted as GPU computation. Native ownership, IRQ/DMA/VM, firmware requirements and recovery must be implemented and reviewed first.

It remains NOT_RUN: the CPU control service offers no GPU address space, native GLK queue or GPU fence; using it to upload bytes would not execute this milestone. Existing Xe/GuC and NVIDIA hosted tests cannot stand in for GLK hardware. Keep display/panel ownership unchanged while developing the first non-display test. PPS0 diagnosis needs actual panel/connector/VBT/timing/register evidence before any corrective modeset/MMIO is considered.

## Required qualification campaigns

1. **Native admission/ownership:** exact provider identity, exclusive/coordinated device ownership, process access/quotas, VM/DMA bounds, page alignment and overflow, stale handles and epoch wrap refusal. Invalid commands must not affect another context or system memory.
2. **Execution and shader correctness:** pinned AIR/compiler versions; applicable render/compute/blit/resource semantics, formats, atomics, barriers, sampling and edge cases; robust negative inputs. Compare known references and read back independently. Inventory completeness is a prerequisite, not a pass count inferred from API declarations.
3. **Sharing/display/system:** actual same-page IOSurface import with measured copy accounting; multiple producer/consumer processes and independent fences; present/vblank/flip retirement; WindowServer attributed to the new GPU path; multi-window/fullscreen, resolution changes, EDID/hotplug and declared monitor combinations. Test optional HDR/VRR/color separately.
4. **Failure and lifetime:** allocation/map/unmap/publish/IRQ failures, timeout, unknown submission, reentrant stop, GPU reset/device loss, process exit while in flight, memory pressure and recovery generations. Include kernel-native race/fuzz/fault tests beyond hosted ASan/UBSan.
5. **Applications/games:** publish exact app/game/version/API/settings and pass/fail/known limitation records. No universal compatibility claim. Launch success alone is insufficient; verify correct frames, synchronization and sustained real GPU use.
6. **Stability/performance:** repeated reset and sleep/wake campaigns, at least an 8-hour steady desktop/load soak and a separate 30-minute thermal steady workload as proposed acceptance workloads. Freeze baseline/thresholds before measurement, not after observing favorable results.

## Performance protocol

Bind same GPU/OS/build/power/clock policy/resolution/quality and workload revision. Measure CPU submission time, GPU timestamps, bandwidth, bytes copied, shader cold/warm latency, throughput and frame-time distribution including p95/p99, stutter and present latency. Report raw samples, run counts, clock/thermal state and uncertainty. A preliminary candidate regression budget is median ≤ baseline×1.05 and p99 ≤ baseline×1.10; these are proposed review thresholds, not measured achievements or universal guarantees.

“Faster/more stable than NullMoth” remains a goal requiring an equivalent NVIDIA setup and reproducible baseline. GLK cannot supply that same-GPU comparison. Any zero-copy claim requires measured same-page ownership and absence of hidden CPU/pixel transfers. No current performance or system acceptance campaign has run.

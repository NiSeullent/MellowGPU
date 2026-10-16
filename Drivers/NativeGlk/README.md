# Native Gemini Lake implementation

These units implement actual Gen9/GLK formats and native memory boundaries for
the first 15IGL7/N4120 target, physical Intel `8086:3185` revision `06`, subsystem
`17aa:3e77`. GLK uses i915/execlists; the separate experimental Xe/GuC path does
not provide its backend.

| Unit | Implemented code | Remaining native connection |
| --- | --- | --- |
| `PageTableEncoding` | Checked GGTT/PPGTT entries, PAT fields, geometry and little-endian serialization | Actual retained page tables, publication and TLB/cache ownership |
| `CommandEncoding` / `Gen9CommandEncoding.cpp` | BCS MI packet encoder/decoder and bounded first-write ring image | Native ring/context submission and independently correlated completion |
| `LrcImage` | Gen9 register-state image, assigned context IDs, ELSP write plan and CSB decoding | Native identity allocation, live backing, engine/reset/fence owner |
| `PanelPower` | Two GLK PCH PPS banks, timing selection and VDD/panel/backlight sequencing | Captured target VBT, firmware display handover, native display/AUX power and MMIO owner |
| `Forcewake` / `Mellow/GlkForcewakeIOKit` | Gen9 render/media forcewake domains, ack polling, reference counts and a native MMIO adapter | Actual GT/PM producer wiring; D0 or ack bits are not engine-awake proof |
| `InterruptDecode` / `Mellow/GlkInterruptIOKit` | Gen8+ master/GT IIR decode and an `IOFilterInterruptEventSource`/work-loop lifetime adapter | Actual interrupt producer, MSI choice and engine/reset consumers |
| `GgttPublication` / `Mellow/GlkGgttIOKit` | GGTT entry write/readback, invalidation and quarantine with a mandatory held awake-engines callback | Firmware/display/stolen handover, zero-PTE-free proof and a real engine-PM observer |
| `BcsExecution` / `Mellow/GlkBcsIOKit` | BCS0 execlists engine: ring/context setup, ELSP submission, CSB tracking, unproven-stop handling | Real forcewake/interrupt/GGTT/owner producers and correlated GPU completion |
| `Mellow/GlkPhysicalOwnerIOKit` | PCI/PM physical owner: epochs, producer registration, required OSObject lifetime anchors, revocation drain and quarantine | Actual GT forcewake/VM/reset/interrupt producers; live epoch alone is not readiness |
| `RenderWorkarounds` / `Mellow/GlkTopologyIOKit` | RCS0 context/indirect workaround producers and a held-operation topology (fuse) read adapter | Native owner callbacks, actual fuse reads, render default state and execution |
| `OpRegionSnapshot` / `Mellow/GlkOpRegionIOKit` | Bounded ACPI OpRegion header/mailbox snapshot and VBT extraction behind the physical owner | Actual OpRegion capture on the target and display-owner admission |
| `VbtParser` | Bounded VBT/BDB parser and GLK eDP panel profile, including the pinned pre-191 PPS0 fallback | Actual VBT capture, a typed bridge to `PanelPower`, connector/EDID and display-owner admission |
| `Mellow/GlkDmaMemoryIOKit` | Native private IOBuffer/IODMACommand backing, 39-bit original pages, mapped/bypassed routes and cache operations | Actual admitted physical/PM/GPU-VM/quiescence owner |
| `Mellow/GlkPassiveProbeIOKit` | Read-only PCI/PM observer (passive IOService) | Diagnostic only; not a power, reset, DMA or mapper lease |

All production units (twenty translation units) are in the real Xcode kext target, so their bodies
compile and link into `MellowGPU.kext`. Nothing starts them: the main
`Info.plist` has no GLK personality, and qualified GPU profiles remain zero.
Native compilation, source-derived values and hosted injected-OS tests do not
demonstrate GPU execution. Complete applicable Metal3, GPU IOSurface/WindowServer,
scanout, apps/games and performance/recovery testing remain required.

The passive observer is also built separately by
`Tools/build-glk-passive-probe.py` as `com.NiSeullent.MellowGPU.Probe` 0.1.0.
That tool reproduces the bundle installed on the target byte-for-byte. Both
bundles define the `MellowGlkPassiveProbe` class, so the main development kext
must not be loaded while the standalone probe is loaded.

Actual target testing currently observes `IONDRVFramebuffer`, no IOAccelerator,
and zero public Metal devices (enumeration and device-lab bench exit 3). This
is a failing baseline rather than an acceleration result.

The offline Mesa-based Gen9 compute compiler lives in `Userspace/NativeGlkCompiler`;
its ISA has never been uploaded or executed and is not GPU evidence.

Source provenance and retained notices accompany each component, or, for LRC,
`LRC-IMPLEMENTATION-REPORT.md` at the repository root. Per-file authorship
(Claude Code original code; Codex corrections and completions), canonical
adaptations and hashes are recorded in `porting/native-glk-implementation.json`.
The GLK command body has a distinct source basename to avoid colliding with
the existing NVIDIA `CommandEncoding.o` in Xcode.

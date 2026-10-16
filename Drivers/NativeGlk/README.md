# Native Gemini Lake implementation

These units implement actual Gen9/GLK formats and native memory boundaries for
the first 15IGL7/N4120 target, physical Intel `8086:3185` revision `06`, subsystem
`17aa:3e77`. GLK uses i915/execlists; the separate experimental Xe/GuC path does
not provide its backend.

| Unit | Implemented code | Remaining native connection |
| --- | --- | --- |
| `PageTableEncoding` | Checked GGTT/PPGTT entries, PAT fields, geometry and little-endian serialization | Actual retained page tables, publication and TLB/cache ownership |
| `CommandEncoding` / `Gen9CommandEncoding.cpp` | BCS MI packet encoder/decoder and bounded first-write ring image | Native ring/context submission and independently correlated completion |
| `LrcImage` | Gen9 register-state image, assigned context IDs, ELSP write plan and CSB decoding | Native identity allocation, live backing, render workarounds/topology, engine/reset/fence owner |
| `PanelPower` | Two GLK PCH PPS banks, timing selection and VDD/panel/backlight sequencing | Captured target VBT, firmware display handover, native display/AUX power and MMIO owner |
| `Mellow/GlkDmaMemoryIOKit` | Native private IOBuffer/IODMACommand backing, 39-bit original pages, mapped/bypassed routes and cache operations | Actual admitted physical/PM/GPU-VM/quiescence owner |

All five production units are in the real Xcode kext target. They do not start
themselves and are not yet connected as a GLK accelerator. Native compilation,
source-derived packet values and hosted injected-OS tests do not demonstrate
GPU execution. Qualified GPU profiles remain zero; complete applicable Metal3,
GPU IOSurface/WindowServer, scanout, apps/games and performance/recovery testing
remain required.

Actual target testing currently observes `IONDRVFramebuffer`, no IOAccelerator,
and three native public-Metal enumeration failures with no Metal device. This
is a failing baseline rather than an acceleration result. Native submission,
physical ownership, forcewake/interrupt integration, VBT and device-lab tools
are being implemented separately.

Source provenance and retained notices accompany each component or, for LRC,
`LRC-IMPLEMENTATION-REPORT.md` at the repository root. Claude Code authored the
original PTE/command/panel/DMA code and the LRC header/tests; Codex completed
LRC bodies and corrected/verified the other units. Per-file adoption and
canonical adaptations are recorded in `porting/native-glk-implementation.json`.
The GLK command body has a distinct source basename to avoid colliding with
the existing NVIDIA `CommandEncoding.o` in Xcode.

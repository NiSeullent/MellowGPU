# GLK device-lab boundaries

Public-API tools for the designated physical GLK GPU: Lenovo 15IGL7, PCI `8086:3185` rev `06`
subsystem `17aa:3e77` at `00:02.0` (GFX0@2 under AppleACPIPCI), native x86_64 macOS 15.8.1 (24H32).

## Current target reality
The passive preflight (`target-passive-preflight.json`) shows no IOAccelerator/Metal device:
the panel runs on `IONDRVFramebuffer` only, and a newly staged AuxKC needs a reboot before
any driver can load. Until a Metal 3 device bound to the physical GLK GPU exists,
`glk-metal-bench` and `glk-framebuffer-probe` exit 3 ("no Metal devices"). That is the
correct result, not a defect. Native GPU, fence and scanout evidence remain unavailable.

## Hard gates (non-zero exit with evidence; never a skip, never a CPU fallback)
| exit | condition |
|---|---|
| 22 | Rosetta-translated or unknown translation state (host gate, before any device call) |
| 26 | process is not x86_64, or the host is Apple silicon (host gate) |
| 27 | macOS is not 15.8.1 / build is not 24H32 (host gate) |
| 3 | no Metal device present |
| 11 / 12 | zero / more than one Metal device bound to the physical GPU |
| 23 | selected device lacks the Metal 3 family |
| 24 | no WindowServer session (probe) |
| 7 | command, present or whole-process watchdog expired (`clean_cleanup=false`) |
| 9 / 14 | GPU output differs from the CPU oracle or the known image |
| 19 | missing or zero-length GPU timestamps |
| 28 | a frame was not confirmed presented with a real `presentedTime` (probe) |

Physical ownership requires all of the following. The `registryID` must resolve through
IOService ancestors to an `IOPCIDevice` (exact class) whose parent is an `IOPCIBridge`. `reg`
must decode to 00:02.0, and both `reg` and `assigned-addresses` must be present. The five IDs,
read as IOPCIFamily OSData, must match exactly. No synthetic/compatibility identity keys may
be present.

## What a PASS does not prove (these flags stay false)
`native_driver_fence_evidence`, `full_gpu_execution_attested`,
`windowserver_presentation_verified`, `scanout_screencapture_verified` (needs Screen
Recording/TCC, never requested), `compositor_ownership_attested`,
`game_compatibility_attested`, `hardware_qualification`. The orchestrator rejects any report
that sets one of them true.

## Safety scope
No tool loads a kext, touches PCI/MMIO/DMA, changes EFI/boot/SIP/AMFI/IOMMU/TCC, installs
to a system path, uses ssh, or reboots. Staging only assembles a bundle. `install.sh` and
`rollback.sh` run separately on the target behind the fingerprint guard, and write only
under the validated install root and the bundle.

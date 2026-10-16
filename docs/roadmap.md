# MellowGPU roadmap

This is a dependency-ordered engineering plan, not a completion date or a promise that every GPU can implement the final profile. Full Metal 3 and actual GPU WindowServer/apps/games remain the release criterion. Current qualified tuples: **zero**.

| Stage | Required output and exit condition | Current status |
|---|---|---|
| 0. Source and target truth | Exact repo/source/license observations, CPU/GPU/OS distinction, safe connected-target identity | Initial selected observations and actual 15IGL7 metadata available; full closures UNKNOWN |
| 1. Shared contracts | Versioned schemas/ABI, three vendor interfaces, lifetime/queue/recovery policy and negative tests | Initial implementation; GPU methods explicitly unavailable |
| 2. Native platform owner | Retained PCI provider, access control, MMIO/DMA/IOMMU/IRQ, ordering, power and recoverable ownership on x86_64 | CPU control storage implemented; native GPU owner incomplete |
| 3. First GPU vertical | Real GLK GPU job → native fence → independent result → proven retirement, preserving panel ownership | NOT_RUN; GLK i915/ANV path required, not Xe substitution |
| 4. Vendor generations | Intel GLK then separately chosen Xe; AMD chosen IP; NVIDIA chosen Nouveau/NVK or separately coherent RM stack | Research and partial hosted units; no admitted physical tuple |
| 5. Metal/shaders | Complete applicable required inventory, AIR/IR/vendor compiler, resources/queues/sync, native Metal registration | Partial research; declaration census not semantic completion |
| 6. Surface/display/desktop | Same-page sharing, display retirement, modeset/panel, actual WindowServer/CoreAnimation | Native path incomplete; PPS0 root cause unproven |
| 7. Qualification | Exact apps/games, recovery/fault/race/soak, display/sleep/wake, reproducible performance | NOT_RUN |
| 8. Maintained releases | Evidence-bound support matrix and reviewed upstream adapter/spec/ABI updates | Selected-source intake and CI exist; automatic native update/PR/promotion unavailable |

Work may proceed concurrently where contracts are fixed. Intel, AMD and NVIDIA hardware code remains separate; no lowest-common abstraction should erase generation-specific memory/display/power requirements. Shared interface changes have one owner and versioned consumers. Optional features and vendor extensions need independent applicability and performance measurement.

## The designated 15IGL7 path

Physical device8086:3185 belongs on the GLK investigation route. Complete pinned i915 PCI/panel/GT/VM/submit/IRQ/firmware analysis; document native alternatives to Linux facilities. Implement a bounded native owner and no-display GPU test with recovery. The four measured private Metal type encodings narrow an ABI research task but do not supply native command selectors, shared-memory layouts, initialization semantics or WindowServer contracts.

Do not “fix” PPS0 by guessing timings or repeatedly loading a display owner. First correlate actual connector, VBT/EDID, panel sequencing, ownership and hardware state. The old firmware display path remains the current visible baseline; blank-screen and panic histories are failure evidence requiring a recoverable test environment. No development-Mac boot or security configuration change is part of this work.

## Parallel work and integration

Actual Claude Code Opus5.5/max sessions provide isolated implementations and reviews for Intel lifecycle, AMD packet encoding, NVIDIA lifecycle, Metal/compiler, native control, wire/probe and source intake. A second eight-session wave investigates GLK/native translation and independently reviews bounded units and provenance. Process init model records, source hashes, terminal status and actual commands distinguish real sessions from requested labels.

Proposals are not automatically integrated. In particular, an undocumented NSObject equality-based render-pass sampling proposal was rejected because a one-field self-test does not establish every attachment/index semantic or public array capacity. Source reviews without execution remain static reviews. A completed hosted lifecycle adapter still needs concrete native callbacks and cannot qualify GPU support.

## ARM64/ARM64e research gate

Keep this work independent of the x86_64 vertical path. Establish actual external-device enumeration, PCIe/Thunderbolt ownership, native DMA/IOMMU/IRQ, loading/signing/PAC policy and Metal/WindowServer registration on an explicitly designated target before implementing support claims. An Apple Silicon compiler host or existing Apple GPU Metal device does not implement an external Intel/AMD/NVIDIA backend. Apple integrated GPU development is separate scope.

## Update automation

Discovery and diff/license/ABI impact reporting feed isolated staged candidates. Next implement reproducible adapter build manifests, exact required-case inventory and hardware runner receipts, then reviewable branch/PR generation. Automatic updates must stop at unresolved Apple private ABI, firmware/legal closure, hardware or performance requirements. No new external scheduler is created. See [upstream-update-policy.md](upstream-update-policy.md).

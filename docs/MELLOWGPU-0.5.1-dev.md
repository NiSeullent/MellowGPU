# MellowGPU 0.5.1-dev

This development release adds a real public IOKit control boundary for Intel,
AMD and NVIDIA display-class PCI devices. The explicit Darwin 24 development
mode exposes physical identity and original-task shared-memory storage through
a versioned wire ABI. It does not submit GPU commands. Its storage mapping and
quota records are CPU resource primitives, not a software renderer or evidence
of GPU memory ownership.

The native implementation holds operation and resource ownership through
allocation, mapping, removal and client cleanup. Unknown unmap retirement stays
quarantined. Initialization and failed-start publication use a stable lock and
defer state destruction until the final reference. The universal diagnostic
client validates the physical parent and every reply before using a CPU address.

Clang-derived Metal declaration inventories distinguish SDK availability,
unreviewed requirements and observed source method bodies. These inventories
are not an assertion that the methods are implemented or that Metal 3 works.
Immutable upstream candidates preserve exact Git sources, licenses, project
inputs and patch application results without executing an upstream build or
promoting support.

The actual adapter is exercised against fault-injected IOKit boundaries, and
the kext is compiled for x86_64 and checked for structure and signature. Those
checks do not replace native kernel loading, actual task mapping, GPU execution
or system compositor testing on the target hardware.

**No GPU/OS profile is qualified.** Native vendor GPU VM, submission, fences,
display ownership, the complete Metal frontend and WindowServer/apps/games
acceptance remain required. CPU rasterization and CPU pixel transfers cannot
qualify. The package contains no installer and performs no EFI, NVRAM or kernel
collection changes.

See the [native control contract](MELLOWGPU-NATIVE-CONTROL.md),
[Metal inventory scope](MELLOWGPU-METAL-API.md),
[candidate contract](MELLOWGPU-CANDIDATES.md),
and [complete system qualification profile](../porting/metal3-profile.json).

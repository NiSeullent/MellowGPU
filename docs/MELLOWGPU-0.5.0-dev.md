# MellowGPU 0.5.0-dev

This development release remakes the existing Mellow product as MellowGPU.
The kernel bundle, executable and bundle identifier are now `MellowGPU.kext`,
`MellowGPU` and `com.NiSeullent.MellowGPU`. Original history, licenses and
hardware evidence retain their original scope and identity.

The remake introduces a vendor-neutral native lifecycle core, an exact-tuple
full Metal 3/system support-profile policy, and immutable source/specification
update tooling integrated with the existing source-intake engine. Intel i915/xe,
AMD amdgpu and NVIDIA Nouveau are separate porting lanes with matched ANV, RADV
and NVK contracts. NVIDIA open RM/GSP remains a separate ABI research lane.

The qualification profile requires Metal 3, genuine GPU resources and fences,
WindowServer, native scanout, application/game tests, recovery and measured
performance. CPU rasterization and CPU pixel copies cannot qualify. The active
native product cannot promote legacy bring-up evidence to full Metal support.

**No GPU/OS profile is qualified by this release.** Shared-core and profile
tests exercise actual software with synthetic native boundaries. Native kernel
compilation, Mach-O/import/signature checks are development build checks; they
do not establish loadability or physical acceleration. Full native Metal API,
system compositor and three-vendor hardware bring-up remain active work.

The package contains the actual development kext, source/build identity,
checksums, architecture/specification, component provenance and this scope.
It has no installer and does not modify EFI, NVRAM, kernel collections or
the currently running laptop. Replace the old Mellow product when explicitly
testing this development product; do not configure both together.

See [the specification](MELLOWGPU-SPEC.md),
[the implementation plan](MELLOWGPU-IMPLEMENTATION.md),
and [the actual qualification profile](../porting/metal3-profile.json).

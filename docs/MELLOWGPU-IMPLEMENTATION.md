# MellowGPU remake implementation plan

> For agentic workers: execute independent tasks with subagent-driven development;
> use actual failing and passing tests, then integrate and verify the whole change.

**Goal:** Rename the existing product, implement enforceable shared contracts and
source-update tooling, and publish a reproducible development release while
preserving the full Metal 3/system qualification requirement.

**Architecture:** Extend the existing runtime and source-intake assets with a
vendor-neutral lease core and complete-profile admission policy. Keep hardware
adapters and immutable source tuples explicit. Preserve licenses and history.

**Tech stack:** C++17 common code, fixed-width C native boundaries, Python 3
source tooling, existing Xcode kernel target and GitHub Actions.

**Spec:** [MELLOWGPU-SPEC.md](MELLOWGPU-SPEC.md).

## Global constraints

No source/CI/model result grants GPU support. No CPU pixel route qualifies. No
blind upstream or system installation. Preserve existing notices and historical
evidence. Empty qualified device lists remain empty until hardware acceptance.

## Review focus

Stale tuples/epochs and wrong vendor ABI pairs must fail admission. Unknown-flight
leases cannot be released by a timeout. Git membership must be verified rather
than supplied as a revision string. Active bundle/executable/module identities
must agree after rename. Development packaging must disclose its actual scope.

## Task 1: Product migration

Files: active Xcode product, Info.plist, cross-build/EFI/packaging tools and their
tests; existing native/EFI workflows. Owner: framebuffer_review.

- [x] Add rename regression tests and observe the old product fail.
- [x] Build `MellowGPU.kext` / `MellowGPU` / `com.NiSeullent.MellowGPU` consistently.
- [x] Preserve historical evidence and source interfaces; reject simultaneous
  old/new loading in active migration tools.
- [x] Execute native cross-build and Mach-O/product validation.

## Task 2: Shared native lifecycle

Files: `Drivers/Core/DeviceLifecycle.hpp`, `tests/gpu_lifecycle_tests.cpp`,
`Tools/run-gpu-core-tests.py`, `docs/MELLOWGPU-LIFECYCLE.md`.
Owner: driver_research.

Interface: fixed-slot typed lease tokens; stop/reset/device-loss close admission;
two-phase native drain plan; separate adapter observation of real quiescence.

- [x] Test old/absent behavior, stale/double tokens, unknown work, reset, teardown,
  capacity and reentrant plan completion.
- [x] Implement freestanding C++17 code, without native callbacks under a lock.
- [x] Execute actual C++ tests with ASan/UBSan and document native adapter duties.

## Task 3: Full Metal 3 support-profile policy

Files: `Drivers/Core/Metal3Profile.hpp`, `tests/metal3_profile_tests.cpp`,
`Tools/run-metal3-profile-tests.py`, `porting/metal3-profile.json`.
Owner: parent.

Interface: assess internal qualification records against an exact compatibility
digest and device generation. Required API inventory, native resources/sync,
WindowServer/scanout, apps, games, recovery and performance gates all apply.

- [x] Test missing gates, software evidence, CPU transfers, incomplete inventory,
  wrong ABI pair, stale tuple/epoch, reused records and successful policy fixtures.
- [x] Implement explicit rejection statuses; never call the policy an attestation.
- [x] Keep real device support empty; execute host tests with sanitizers.

## Task 4: Immutable upstream integration

Files: `Tools/mellow-upstream.py`, `Tools/mellow_port/upstreams.py`,
`tests/test_mellow_upstreams.py`, `porting/upstreams.lock.json`,
`.github/workflows/upstreams.yml`. Owner: local_context.

- [x] Write actual temporary-Git tests for revision/blob/content mismatch,
  traversal, dirty worktrees, role changes, malformed metadata and ABI drift.
- [x] Implement strict lock validation, membership verification and candidate
  discovery using existing intake; never execute fetched scripts.
- [x] Pin primary references/dependencies/specs and preserve component licenses.
- [x] Run offline tests and live read-only discovery; retain candidate reports.

## Task 5: Integration and publication

Files: README, attribution addition, remake CI, release notes and package manifest.
Owner: parent.

The clean-source native build and development packager are implemented with
regression tests. Publication receipts and release asset checksums record the
exact released commit; they are generated after the source commit, rather than
stamping a previous binary with a new revision.

- [ ] Independently review agent changes and run appropriate existing host,
  platform, intake, new contract tests and native bundle checks.
- [ ] Package exact source/build identities, checksums and explicit support scope.
- [ ] Commit the remake, rename the authorized GitHub repository to MellowGPU,
  push reviewed commits and publish a development prerelease with actual assets.
- [ ] Verify remote commit, workflow and release asset identities. Continue native
  vendor bring-up against the new contracts; do not claim full acceleration here.

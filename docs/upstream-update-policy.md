# Upstream update and provenance policy

Status: initial implementation; no automatic production promotion.

This policy addresses **MG-UPSTREAM-001** (trace upstream changes without
unverified product adoption) and **MG-PROVENANCE-001** (bind selected inputs and
evidence to immutable source identities). The final acceptance requirement
remains complete applicable Metal 3 hardware acceleration, actual WindowServer
composition and the declared application/game corpus for an exact
PCI ID/GPU generation/macOS build/CPU architecture tuple. Source intake, a
successful build and simulated transport tests cannot satisfy that requirement.

## Authoritative inputs and evidence

`porting/upstreams.lock.json` is the existing machine-readable source/specification
lock. It records canonical repository URLs, full Git commit and selected blob
identities, file SHA256/size, per-file declared licenses, source URIs, usage roles,
consumers, local patch/configuration digests and declared exact ABI checks.
`dependencies.lock` is the project-level generated index maintained by the
specification tooling; it must agree with the canonical source-lock digest.
Neither file is a hardware support certificate.

The source lock describes selected files, not a complete build dependency
closure. Firmware identity, firmware redistribution permission, transitive
licenses, build configurations, compiler/native ABI compatibility and hardware
acceptance remain **UNKNOWN/unapproved** wherever matching evidence is absent.
An empty firmware list means firmware evidence is missing or inapplicable only
when separately established; it does not prove that a GPU needs no firmware.

Usage is explicit: build dependency, port/patch input, behavior reference or test
oracle must be distinguished by the consuming adapter specification. Current
tooling admits `reference-only` and `build-dependency` roles; the latter still
does not approve building or distributing the complete backend.

Licensing is evaluated per source/file and selected closure. A repository root
license does not establish the license of every source, firmware or binary.
`darwin-permissive` and `linux-native` are separate intake profiles; accepting a
declared SPDX expression in a profile is a source-evidence check, not a legal
compatibility determination. Malformed/conflicting SPDX markers require review,
and pre-SPDX MIT sources require the exact pinned complete leading grant/notice.
The complete selected dependency/firmware distribution review remains mandatory.

NullMoth and Cobalt64 remain reference-only. Known canonical URL aliases and the
curated restricted commit/blob/content identities cannot be promoted by changing
an ID or URL. Comparison with a supplied prior lock also detects such reference
identities under renamed components. This finite policy cannot identify every
fork, modified copy or previously unseen ancestry. Unknown provenance remains
unapproved; the absence of a known-reference match is not license clearance.

## Implemented pipeline

| Boundary | Current implementation | Meaning of success |
| --- | --- | --- |
| Lock validation | `Tools/mellow-upstream.py validate` | Schema, roles, immutable identity syntax, selected license/profile and URI consistency passed; source bytes are not yet verified. |
| Change detection | `discover` queries the configured branch/tag and specification digest. Annotated tags retain the object and peeled commit. | A remote candidate was observed. Its source, ABI and hardware behavior remain unverified. Fetch/timeouts are recorded as errors. |
| Declared impact report | `report --candidate-lock` compares selected file/blob/hash records, ABI-header/contract assertions, notice/license review, patches/configuration, specification digests and consumers. | Reviewable metadata drift is identified. This is not whole-program semantic ABI analysis or complete firmware/license closure. |
| Immutable source verification | `verify-source` checks actual HEAD, recomputes the full commit → every selected nested tree → regular blob object hashes, and matches selected worktree SHA256/size/license and exact ABI assertions. | These selected source bytes belong to the immutable revision. No fetched script is executed. |
| Isolated evidence materialization | `Tools/mellow-candidate.py stage` copies verified sources and exact tracked project patch/configuration inputs into a fresh private directory. | The selected data and strict text patches were captured; all build/qualification/promotion flags remain false. |
| Repository CI | `.github/workflows/upstreams.yml` runs local intake regressions and lock validation; scheduled/manual discovery uploads review-only reports with read-only repository permissions. | CI has evidence for its stated host/source scope. It does not install a driver or qualify a GPU. |

Discovery never rewrites an active lock. A changed revision requires new explicit
immutable source records and an actual matching checkout before materialization.
The report preserves unchanged blob content across a changed commit without
inventing an actual header/license-content change. Unknown or changed native
Apple ABI is never inferred compatible from a name, OS version or public header.

Candidate materialization does not create a Git update branch. It compares
existing directory identities to reject source/project overlap, including macOS
Data-volume and case aliases. Held file/directory descriptors prevent symlink
following during captured reads/writes and permission changes. Unreadable,
unlisted, empty or special-file additions cannot silently pass inventory.

The patch subset begins with `diff --git`, uses strict UTF-8/LF unified hunks,
and permits selected regular files or regular additions. Before actual Git
application, an independent byte-level evaluator checks complete preimages and
declared old/new line positions. The actual patched tree must match those bytes;
offsets, fuzz, hidden preambles and unsupported metadata fail. Fetched or pinned
scripts remain data and are never executed by intake. See
[candidate intake](MELLOWGPU-CANDIDATES.md) for the exact subset and bounds.

Candidate receipts always retain:

- `status=evidence_intake_unapproved`;
- `ready=false`, `build_eligible=false`, `license_review_accepted=false`;
- `hardware_qualified=false`, `production_promotion_available=false`.

Post-patch UTF-8 license observations include scripts, assembly, shaders, build
files and notices; changed notice bytes and changed/added observations are
listed. Binary/undecodable data stays unclassified. No observation authenticates
legal review or clears the missing complete closure.

## Required continuation, not yet automated

The following stages are **NOT_IMPLEMENTED as an integrated automatic update
pipeline**. Existing development build/package commands or host tests do not
implicitly connect them to a discovered candidate.

1. Create an isolated candidate Git branch/worktree from a fixed project commit,
   preserving the previous accepted lock and source tree.
2. Establish complete selected source/configuration/patch/toolchain and firmware
   closures; review per-file obligations and unknown redistribution rights.
3. Update the affected adapter and requirement/specification records with exact
   commit/path/symbol evidence. Reject unsupported APIs and ambiguous changes.
4. Build/link each affected native architecture and OS target, audit exact native
   imports/ABI/signatures, and run correctness, negative, recovery and performance
   regressions. Universal compilation does not prove ARM64e ABI or GPU support.
5. Run the required hardware corpus on the explicitly designated disposable test
   system with a reviewed recovery route. Require exact device/OS/build identity,
   real GPU submission/fence/readback, IOSurface/interprocess ordering, display,
   WindowServer, app/game, memory-pressure, reset and declared sleep/wake evidence.
6. Generate a reviewable pull request containing source/specification/lock changes,
   impact analysis, unknowns, commands and hashed results. No automatic PR generator
   or production-lock replacement is currently supplied by these intake tools.
7. Promote only after the complete required review and measured acceptance corpus
   passes. Unresolved license/firmware, missing required Metal features, hidden CPU
   rendering/copy fallback, false capabilities or missing hardware results block
   support. Preserve exact previous accepted artifacts and rollback evidence.

No external scheduler, notification service or unattended hardware executor is
registered by this work. Repository CI discovery remains a source-reporting
activity. Driver loading, EFI/boot changes and kernel/platform policy changes are
outside the updater and require the separately designated test environment.

## Failure, consumption and traceability

Invalid schema, duplicate JSON keys, mismatched commit/tree/blob/SHA, dirty selected
worktree, unapproved role change, unexpected output or failed exact patch produces
no successful intake record. Existing output is never overwritten. After private
reservation, failure retains an error marker when storage allows; unknown state
does not authorize a blind retry in the same output directory.

Exclusive/fsynced files are not a crash-atomic publication mechanism. A killed
process may leave partial or truncated evidence. A future consumer must parse
strictly, reject failure/missing/truncated records, rehash the complete expected
file set and immutable provenance tuple, and enforce its own source/build/native/
hardware gates. There is currently no candidate build/promotion consumer.

Each proposed adoption must link requirement ID → source commit/path/symbol →
implementation → exact test → actual result. Source-only, host simulation, native
compile, runtime metadata and hardware results remain distinct evidence kinds.
The current intake tests use temporary Git/filesystem fixtures; fixture device
IDs and CPU storage results are not evidence for the connected 15IGL7 or another
physical machine. Missing hardware measurements are **NOT_RUN**, not PASS.

Verification commands for this implemented slice:

```sh
python3 Tools/mellow-upstream.py validate
python3 -m unittest discover -s tests -p test_mellow_upstreams.py -v
python3 -m unittest discover -s tests -p test_mellow_candidate.py -v
python3 -m unittest discover -s tests -p test_mellow_intake_integrity.py -v
```

On macOS, use a canonical non-symlink temporary directory for the existing legacy
intake fixture suite, for example `TMPDIR=/private/tmp`. The dedicated path-alias
regressions exercise real macOS aliases separately and do not relax the policy.

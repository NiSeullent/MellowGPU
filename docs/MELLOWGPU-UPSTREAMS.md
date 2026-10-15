# Pinned upstream sources and specification updates

The upstream tool extends the existing `Tools/mellow_port` intake engine. It
validates immutable source and document records, verifies selected source files
against real Git objects and the checked-out worktree, and reports update
candidates. It never installs a driver, executes fetched scripts, changes the
active lock, or grants Metal/WindowServer support.

## Commands

From the repository root, using Python 3.12 or newer and Git:

```sh
python3 Tools/mellow-upstream.py validate
python3 Tools/mellow-upstream.py report
python3 Tools/mellow-upstream.py discover --output candidates.json
python3 Tools/mellow-upstream.py verify-source \
  --component linux-xe --source-root /absolute/path/to/locked-linux-checkout
python3 Tools/mellow-upstream.py report \
  --candidate-lock /absolute/path/to/reviewed-candidate.lock.json
```

`--lock` selects a different lock; the default is `porting/upstreams.lock.json`.
`--output` writes one new exclusive report. It refuses an existing destination,
including the active lock. `verify-source --project-root` selects the root used
for declared local patches/configuration; it defaults to this checkout. Source
and output paths must be actual paths without symlink components. On macOS use
an actual `/private/...` temporary path instead of its `/var/...` alias.

`validate` checks schema, immutable IDs, paths, source URIs, profile/license
markers and reference-only policy. It does not claim that source files have been
fetched or verified. `verify-source` additionally requires HEAD at the exact
locked commit, regular Git blob modes, correct blob object ID, SHA256 and size,
matching unchanged worktree bytes, actual SPDX or explicitly pinned legacy MIT
notice, local patch/configuration digests, and configured exact ABI checks.
A repository with a different HEAD or modified selected file is rejected.
Verification reads only the listed files; it does not certify the entire tree.

`discover` queries canonical HTTPS remotes for the declared tracking ref and
its peeled tag, and fetches declared HTTPS specification documents to compare
content digests. These are metadata/content observations; candidate commit and
source membership remain unverified until source verification. Each query has a
timeout, and failures stay visible in the report. There is no automatic retry or
fallback to an unpinned branch. Discovery does not execute upstream code.

`report --candidate-lock` reports source, ABI header/contract, notice/license,
patch, configuration and specification drift and affected component consumers.
A new commit with identical header blobs does not falsely report changed header
content. Reference-only entries cannot be promoted to linked inputs through an
update. Every tool result keeps hardware qualification and production promotion
false. Updates require the remaining build, native ABI, license-closure and full
hardware conformance stages in [the specification](MELLOWGPU-SPEC.md).

## Initial source records

The lock contains selected source subsets and reference documents, not complete
vendor backends or a verified build dependency closure. All component license
reviews are initially `unreviewed`. Consumers identify the affected recipe or
specification; they do not cause the source to be compiled. Each selected file
records canonical `source_uri`, Git blob ID, SHA256, byte count and license
observation. Original notices and existing Mellow licenses remain controlling.

| Component ID | Role | Full immutable commit | ABI/source boundary |
| --- | --- | --- | --- |
| `nullmoth-reference` | reference-only | `d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb` | v2.0.2 architecture/license documents; no copied implementation |
| `cobalt64-reference` | reference-only | `d1f7bad8408a0a2c016b067c477422975a59de44` | Cobalt private N48N interface reference; not DRM compatibility |
| `linux-xe` | build-dependency, subset | `0d9ff90a5422cc7509258aaaba1e7481df4d332a` | Existing Xe GGTT register source pin |
| `linux-nouveau` | build-dependency, subset | `0d9ff90a5422cc7509258aaaba1e7481df4d332a` | Nouveau device source; native NVK backend still unimplemented |
| `mesa-vulkan` | build-dependency, subset | `f5cb8ee032adabef599ae892ec4e42d796b84da9` | Selected ANV source; not a complete ANV/RADV/NVK build |
| `libdrm` | build-dependency, subset | `b97cbde15c5c3abfe44d78e8f57139e50f612fec` | Selected userspace DRM transport source |
| `nvidia-open-rm` | build-dependency, research subset | `e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb` | RM610.57.04 research contract, separate from Nouveau/NVK |

Canonical source remotes are respectively
[NullMoth](https://github.com/nullmoth/nvidia-macos-driver),
[Cobalt64](https://github.com/Almosst-DEV/Cobalt64),
[Linux](https://github.com/torvalds/linux),
[Mesa](https://gitlab.freedesktop.org/mesa/mesa),
[libdrm](https://gitlab.freedesktop.org/mesa/libdrm) and
[NVIDIA open modules](https://github.com/NVIDIA/open-gpu-kernel-modules).
The libdrm repository's canonical path is `mesa/libdrm`; older `mesa/drm` links
redirect. Full commits and file blobs were measured from primary Git/Git-host
responses; abbreviated source revision labels were not retained as active pins.

Apple's [Metal 3 model data](https://developer.apple.com/tutorials/data/documentation/metal/mtlgpufamily/metal3.json)
and [feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
are separately content-pinned with source URI, observation revision and consumers.
These records detect document drift. They are not yet a complete feature/API
inventory and cannot qualify Metal 3.

## License, ABI and update boundaries

`darwin-permissive` accepts a bounded set of explicit permissive/dual-license
expressions; `linux-native` separately admits selected GPL-family expressions.
Unknown, conflicting and incompatible markers fail validation. This mechanical
profile check is not legal clearance: the selected build's complete header,
generated-code, patch, firmware and linked dependency closure still needs review.
An old MIT source without SPDX needs the exact leading notice SHA and matching
grant/attribution/warranty markers. Other unmarked source remains blocked from
build-dependency verification. Reference-only source notices can be recorded for
inspection without granting linkage eligibility. NullMoth's PolyForm and
Mellow's retained TSNPL are not relabeled MIT.

ABI checks assert only explicitly configured exact integer relationships among
a selected header macro, one document marker and the locked expected value.
They do not infer structure layout or semantic compatibility from text. A
consumer minimum such as Cobalt's “1.11 or newer” is not treated as exact equality
with native ABI1.12. Initial records leave such assertions unset, while header
content changes are still reported. Native UAPI layout/import/behavior tests
remain separate mandatory gates.

The weekly/manual GitHub workflow uploads candidate and gap reports with
read-only repository permissions. Push/PR runs execute actual temporary-Git
negative tests and existing intake regressions. Actions are commit-pinned.
No workflow writes a production lock, opens a promotion PR, installs a driver,
or changes a supported-device matrix. The first implementation deliberately
leaves automatic patch application, dependency-closure proof, native builds and
trusted hardware promotion as explicit remaining stages.

## Verification

```sh
python3 -m unittest discover -s tests -p test_mellow_upstreams.py -v
python3 -m unittest discover -s tests -p test_mellow_port.py -v
```

Tests create real temporary Git histories and reject changed worktrees, wrong
commits/blobs/content hashes, traversal/symlinks, role promotion (including a
canonical-remote spelling bypass), source license mismatch, altered notices,
ABI document/header mismatch, changed configuration and output overwrites.
Discovery fixtures verify reporting without network or lock mutation; separate
read-only live metadata probes are observations rather than unit-test evidence.

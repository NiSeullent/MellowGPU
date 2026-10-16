# Immutable candidate source intake

`Tools/mellow-candidate.py` materializes a selected upstream component and its
pinned local patch/configuration series in a fresh, private evidence directory.
It extends [the upstream verifier](MELLOWGPU-UPSTREAMS.md); it is not an automatic
Linux-to-Darwin driver translator, builder, installer or qualification authority.

```sh
python3 Tools/mellow-candidate.py stage \
  --lock porting/upstreams.lock.json \
  --component libdrm \
  --source-root /absolute/path/to/pinned-libdrm-checkout \
  --project-root /absolute/path/to/MellowGPU \
  --out /absolute/path/to/new-candidate-intake
```

A macOS or Linux host, Python 3.12 or newer and Git are required. Use actual paths without symlink
components. `--project-root` defaults to this repository. Output must not exist,
its parent must already exist, and it must be outside both source and project
roots. The source HEAD must equal the selected full lock commit. A sparse source
checkout can be used: only explicitly selected, verified Git files are copied.
Unlisted files are not a dependency-closure or build-completeness claim.
Overlap checks compare existing directory device/inode ancestry, including
macOS Data-volume firmlinks and case aliases, rather than path spellings alone.

Reference-only entries, including NullMoth and Cobalt64, are rejected. Even if a
caller changes a candidate lock's review flags or supplies a different revision,
this tool cannot grant build eligibility or hardware support. A different source
revision requires an independently updated immutable file/blob/hash lock and a
matching actual checkout. Source-lock generation/discovery remains a separate
operation; this command never rewrites an active or last-known-good lock.
Known GitHub host aliases and the curated reference commit/blob/content pins
cannot become build dependencies under a renamed component or fork URL. Drift
comparison also preserves supplied prior reference identities across renames.
Verified commit headers also reject an immediate parent from the curated
restricted-reference commit set. This checks only that direct parent; it does
not walk a fork graph or establish unseen ancestor or root-tree provenance.
This bounded policy cannot identify arbitrary forks, modified copies or unseen
license ancestry. Unknown content still requires complete provenance/license
review and never becomes build eligible through this tool. Known source file
suffixes and build files cannot evade source-license checks by being relabelled
as specifications.

## Verification and output

Before creating output, the tool reuses `verify_source` for selected Git commit,
every nested tree and blob, SHA256, size, actual source license, configuration and declared exact ABI
checks. Patch and configuration files must additionally be regular Git blobs in
the project HEAD, with matching worktree bytes and locked digests. A changed
worktree plus an edited SHA in the input lock does not bypass project membership.
Commit, tree and blob Git object hashes are recomputed from their complete raw
bytes; Git's successful path lookup alone is not accepted as membership proof.
The original lock is parsed strictly, copied byte for byte, hashed and reparsed.
All original inputs and both source/project HEADs are checked again before the
completion record. No original tree is checked out, patched or otherwise changed.

A successful evidence directory contains:

- `inputs.lock.json`: exact captured lock bytes.
- `source-raw/`: exact selected upstream source bytes.
- `source-patched/`: those bytes with the strict pinned patch series applied.
- `project-inputs/`: exactly the listed tracked patch/configuration files.
- `intake-intent.json` and `intake-result.json`: immutable input/output digests,
  project commit/blob records, ordered patch before/after manifests, actual
  post-patch license observations and declared ABI-check status.

Captured inputs and JSON records are created exclusively and fsynced. Files
are nonexecutable, and completed source files are read-only; their content hashes, not a permission bit, identify the
candidate. Unlisted bundle files, symlinks and special files cause rejection.
Tree enumeration fails on unreadable or unexpected empty directories. Directory
creation, captured writes, reads and read-only conversion use held directory/file
descriptors without following symlinks; permission changes do not follow a
replaced source path into a foreign file.
Git-ignored project extras are not copied into the bundle. The tool uses a
private temporary Git metadata directory for application; it does not reuse or
modify the source/project index or execute hooks or fetched scripts.
Git's check and actual apply use a separate temporary working tree, never an
output bundle pathname. Every result is compared byte for byte in memory before
the no-follow writer creates `source-patched/`. Swapping an output pathname
therefore cannot redirect Git into the original source checkout. Exact known
restricted-reference bytes are rejected in patch/configuration inputs and in
every computed patched file, including newly added files; this finite content
policy does not establish the origin of altered or unknown content.

The record always has `status=evidence_intake_unapproved`, `ready=false`,
`build_eligible=false`, `hardware_qualified=false` and
`production_promotion_available=false`. The existing 0.5.0 lock has unreviewed
complete dependency/license closures; its permitted source inputs can therefore
be captured as evidence, not approved for building. Declared review metadata is
preserved but is not authenticated or accepted as authority by this tool.
Post-patch ABI mismatch is explicitly reported as a blocking gap. Added or
changed source notices are observations requiring full dependency/license
review; a successful mechanical patch cannot clear that review.
Post-patch license observations include all UTF-8 files, including scripts,
assembly, shaders and build files; undecodable data is explicitly unclassified.
Changed notice-file bytes and changed/added license observations are listed in
`post_patch_license_changed_paths`. A malformed SPDX marker cannot be silently
dropped to preserve an otherwise permissive expression.
License observations retain digests of leading license comments and lexical
license/prose lines, so rewriting an old permission grant or adding a GPL prose
notice is visible even without a new canonical SPDX marker. Notice path checks
include `LICENCE`, `COPYRIGHT` and children of `LICENSES/`. Non-LF vertical
separators inside SPDX payloads are malformed, noncanonical tag case is treated
conservatively as malformed, and GPL review detection is case insensitive.
These are bounded lexical observations, not a complete license parser or legal
clearance. Ordinary source code changes do not by themselves imply a license
content change.

## Strict patch subset

Patch bytes must be pinned tracked project files and start immediately with
`diff --git`; preambles, including hidden context/normal diffs, are rejected.
Supported input is UTF-8 Git
unified text diff data with LF lines and a final LF, changing existing selected
files or adding regular mode-100644 files. Paths must be canonical relative
ASCII paths. Binary patches, renames/copies, deletions, mode changes, symlinks,
Git metadata paths, quoted/escaping paths and missing-final-newline forms are
rejected. Email-format trailers and other unsupported metadata also fail closed.
This intentionally bounded subset does not promise to apply arbitrary upstream
patch series; unsupported patches must be reviewed or supported deliberately.

Before invoking Git, an independent byte-level hunk check uses the declared
original and new line positions, complete context/removal bytes and exact hunk
counts. It cannot search for an offset or drop context. Then `git apply --check`
and actual `git apply` run with whitespace errors rejected. The resulting tree
must equal the independently calculated tree exactly. Thus a patch that Git
could relocate to another line is rejected even when Git's own check succeeds.
Patch/configuration scripts are always data; none is sourced or executed.

The aggregate source bound is128MiB, pinned patch/configuration input32MiB,
patched tree160MiB and individual file16MiB. These are intake limits, not GPU
resource limits. A complete driver still requires selected-build include and
license closure, exact configuration/firmware/toolchain, real native linking,
and the full Metal3/WindowServer/application/game acceptance pipeline.

## Failure and evidence consumption

Validation failures before reservation leave no output. Once a directory is
reserved, failures retain `failed-intake.json` when storage permits and never
publish a successful completion record. An incomplete file created by a failed
write is removed only if it is still the file created by that write; unexpected
replacement files are preserved. An existing output directory is never reset,
reused or overwritten. An intent, partial directory, error-free exit alone or a
caller-created JSON record cannot authorize a build.
Exclusive/fsynced records are not a crash-atomic publication protocol: abrupt
termination may leave a truncated record. Such a record is invalid evidence;
the tool has no consumer that accepts it or promotes any candidate.

Any future consumer must require a complete matching receipt, absence of the
failure marker, and reverify every captured input/output digest and original
source/provenance tuple before its own separately implemented gates. This
initial tool has no build/promotion consumer. No source directory is registered
with a live driver and no production candidate/report/lock is replaced.

## Tests

```sh
python3 -m unittest discover -s tests -p test_mellow_candidate.py -v
python3 -m unittest discover -s tests -p test_mellow_intake_integrity.py -v
python3 -m unittest discover -s tests -p test_mellow_intake_boundaries.py -v
```

Tests create real temporary Git repositories and run actual Git application.
They cover correct source/patch/config capture, zero-patch intake, dirty worktree
and forged lock hashes, duplicate JSON, reference-role changes, copied-lock
reparse, unexpected output files, exact line/context rejection, path/mode/link
attacks, script nonexecution, partial writes, source/lock/project changes during
staging, post-patch ABI gaps and preserving existing output. Fault injection is
limited to the relevant write/apply boundary; Git and the production staging
logic run for real.
Integrity cases additionally exercise actual macOS path aliases, unreadable
directories, a post-inventory symlink replacement, ignored patch preambles,
forged loose commit/nested-tree objects, SHA256-format Git repositories,
malformed SPDX markers and renamed reference identities. These are local
filesystem/Git records, never target GPU or hardware qualification evidence.

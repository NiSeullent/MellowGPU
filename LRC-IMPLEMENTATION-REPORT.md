# GLK LRC image and descriptor codec completion

This isolated candidate now contains actual C++17 implementations for every
declared LRC image, register-state, descriptor, ELSP-plan and CSB-codec function.
It has no MMIO, IOKit, allocation, DMA or GPU execution path. It is an encoding
component for a future native owner, not a native execution or Metal claim.

## Contribution and source provenance

Claude `glk_lrc_author` left `LrcImage.hpp` (267 lines) and the initial test file
(721 lines), but no `.cpp`, NOTICE or provenance JSON. Exact untouched before
copies and hashes are in `completion-evidence/before.json`:

* Header: `a514e5d41f37b229835d87a3f25b0704ecdfaafef595c07574328185ccc47fce`.
* Tests: `71edd0001f27a7f2cde1c1e4fa6784c5239cc663613b0d35ecf3de7876473db6`.

Codex `/root/driver_research` wrote `LrcImage.cpp`, the focused runner, the
additional ring-consistency/assigned-CSB tests and this report. The header was
extended with the separate CSB-identity checker and scope comments; references
to nonexistent notice/provenance files were corrected. Intel notices remain in
the header/tests, and the MIT permission text is retained in `LrcImage.cpp`.
The production offset arrays are expanded register data; the initial tests
contain the upstream compressed tables. No Cobalt/NullMoth code was used.

The exact source is Linux `0d9ff90a5422cc7509258aaaba1e7481df4d332a`:

| Source | Used facts / symbols | SHA256 |
| --- | --- | --- |
| `gt/intel_lrc.c` | `gen9_{r,x}cs_offsets`, `lrc_init_state`, `__lrc_init_regs`, `lrc_update_regs`, `lrc_descriptor`, stop-ring reset | `b10f6affc652967c619a846982bb9731f24ec8ef3b8d46daf428adb76f5b3843` |
| `gt/intel_lrc_reg.h` | register-state value slots, FORCE_RESTORE, six-entry CSB | `671b2adfb2825597425eea515b892cba1365f658d552f9a90439182f0f7f21c0` |
| `gt/intel_engine_regs.h` | ring control/MI_MODE, RPCS fields, ELSP/CSB offsets | `e6ae20bc2946a252a948628df6a84522c19dd45d537f5efa0e5c55d08e82961a` |
| `gt/intel_engine_cs.c` | actual engine bases/context sizes and render/EU-priority flag selection | `3b832bd7e1d5225cb336749f5ccd17a08fa6ddbe1d62387972f1606b144375e4` |
| `gt/intel_context_types.h` | low LRCA / upper live ccid descriptor union | `999dc3bf65b9f73ed08dd8210b41787c135922f3bbddbc6461e774cb755320f4` |
| `gt/intel_execlists_submission.c` | assigned-tag encoding, tail direction, priority, reverse-port/upper-first ELSP, CSB parsing/reset | `44c53b014628fdd409d089d15209020a79a0098bf2b6f4c477d89df73ddf5cf2` |
| `gt/intel_ring.h` | qword offsets, cacheline head/tail restriction, modular direction | `2057ac91d51ef486292595390de04e7e3435acafc2fa35a237de3bfd7b23c035` |

These i915 files carry MIT SPDX identifiers; the `i915_reg.h` base definitions
carry the legacy MIT notice. The independently verified source cache and
`completion-evidence/intel_ring.source.json` retain Git blob identities.
The runner verifies nine cache files plus `intel_ring.h` and compares the full
test table token sequences to the actual `gen9_{r,x}cs_offsets` tables.
This is selected-file provenance, not approval of the entire Linux closure.

## Implemented behavior

Images use explicit byte-wise LE32 encoding, so an unaligned caller buffer
does not require a host-aligned `uint32_t*`. Fresh and default-state initialization
follow the pinned register lists; partial source/destination overlap is refused,
while exact in-place default-state initialization is supported. Every error
returns before modifying the image/state/output. Non-render images are two
pages; Gen9 render images are 22 pages. The provided image extent must match.

PML4 DMA is nonzero/page-aligned and below the GLK 39-bit bus-address limit.
GGTT context/ring extents are validated against the 32-bit LRCA/ring addressing
fields. Ring sizes are page-aligned powers of two, bounded by the actual
RING_NR_PAGES encoding. RCS inputs require explicit indirect-workaround-batch
address/size and RPCS data; unsupported reserved bits and missing inputs fail.
These checks validate encoding, not the caller's physical ownership or fuses.

Full 64-bit descriptors contain a nonzero 21-bit context ID, valid lower flags
and engine-appropriate priority fields. `contextIdForTag` implements the pinned
64-bit-kernel tag encoding; it does not allocate or reserve a live ID.
Publication preparation preserves/restores FORCE_RESTORE on repeated, backward
or wrapped tails and retains only the persistent lower flags afterward.
It additionally rejects a caller/image ring-size mismatch and enabled STOP_RING.
The ELSP plan describes port1 before port0, upper dword before lower; it performs
no writes, barrier or publication.

The CSB codec handles six-entry wrapping, reserved pointers, allones unwritten
entries, status bits and context ID extraction. Raw decode is deliberately not
event validation: unknown status bits remain visible. The separate original
`checkCsbContext` refuses zero/out-of-range or foreign assigned IDs. A match
does not validate epoch, pending/active sequence, a GPU-written fence or retirement.

## Actual verification

The initial missing implementation produces unresolved symbols (saved baseline
log). The added ring-size/image counterexample was first observed RED:
`1 of 320 checks failed`; the corrected body refuses it atomically. The new
assigned-CSB API was also linked only after its missing-body RED was recorded.

The final focused run passed **327 actual C++ checks** with address/undefined
sanitizers and `-fno-sanitize-recover=all`; runtime sanitizer settings halt/abort
on errors. Whole-table source-oracle checks passed. Two compiled actual-body
mutants (dropping descriptor context ID, accepting a foreign CSB identity) each
failed the tests; the mutation results are kept separately.

The same production `.cpp` compiled as a freestanding x86_64 macOS15.8.1
kernel-style object with explicit SDK15.5, `-mkernel`, no exceptions/RTTI,
no C++ standard library and no builtins. `nm -u` reported no unresolved symbols.
This is an object/source check, not a KDK/KPI link or execution in an x86 kernel.
Hosted tests ran on the local build host, not on 15IGL7.

```sh
python3 -B Tools/run-native-glk-lrc-tests.py \
  --linux-root /absolute/path/to/verified/linux \
  --ring-header /absolute/path/to/verified/intel_ring.h \
  --kernel-sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk \
  --out /absolute/fresh/result-directory
```

The runner's `completion-evidence/final/result.json` binds the actual source
bytes, compile commands, test output and kernel-style object. It records all
hardware/Metal/WindowServer qualification fields false. The final freeze binds
this report and all deliverable files. No independent peer is claimed here;
the framebuffer peer was busy with the forcewake implementation.

## Native prerequisites still absent

The native owner must supply authentic retained DMA/GGTT/PPGTT mappings, actual
forcewake and PM exclusion, authoritative GGTT reservation, GLK cache maintenance,
and safe complete engine initialization/reset. It must retain live context IDs
until retirement and serialize image changes with submission and reset.

For RCS, this unit does not create the `gen9_init_indirectctx_bb` workaround batch,
derive RPCS from actual fused SSEU topology, validate a golden/default native
context, or implement OA/performance streams. Supplying a numerically valid
address or RPCS value is not evidence that those prerequisites exist.

The owner must implement actual publication barriers, request/CSB acquire reads,
GPU-written completion fences, expected event ordering, stale-epoch rejection,
and retirement after completed invalidation/quiescence. Timeout, raw CSB COMPLETE,
successful encoding or a matching ID must never become execution/retirement
proof. This candidate does not expose a driver user client, admit hardware,
initialize a GPU, submit a batch or qualify Metal3/WindowServer.

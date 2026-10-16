# Metal API declaration inventory and implementation review

This inventory targets **macOS 15.8.1, x86_64, Metal 3**. It is declaration
evidence from a local Apple SDK, not an implementation, semantic conformance,
GPU capability, or WindowServer qualification. The product's complete required
API inventory and all hardware support remain unqualified.

Apple describes the Metal 3 programming model as the combined Metal 1, 2 and 3
API surface. A hardware feature additionally depends on the GPU family and
runtime capability queries; an OS availability annotation alone cannot make a
feature mandatory or supported. Metal 4 command allocators, argument tables and
its queue model are separate. See Apple's [feature tables, May 21, 2026](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
and [Metal 3 family documentation](https://developer.apple.com/documentation/metal/mtlgpufamily/metal3).
Their observed document digests are recorded in `porting/upstreams.lock.json`;
the current tables include Apple families and do not qualify Intel, AMD or NVIDIA
PCI devices for MellowGPU.

## Reproduce declaration evidence

Use a local, legally obtained Apple SDK. The tool reads headers without copying
their text into the report. Generated JSON contains names, types, selectors,
source spans, availability metadata and digests. Apple's headers and notices
remain under their existing terms; no SDK headers are redistributed here.

```sh
python3 Tools/metal-api-inventory.py capture \
  --sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk \
  --target-macos 15.8.1 --out build/metal-api-sdk15.5.json
python3 Tools/metal-api-inventory.py verify \
  --sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk \
  --inventory build/metal-api-sdk15.5.json
python3 tests/metal_api_inventory_tests.py
```

Output paths must be fresh. Select `--clang` explicitly when the compiler is not
the executable found on `PATH`. The SDK 15.5 report is the baseline available in
this environment. It is older than macOS 15.8.1, so it cannot prove that no API
was added between those revisions. The SDK 26.5 report is a supplementary
comparison with the **same 15.8.1 target**, not a Metal 4 requirement list.

The invoked compiler can be a shim such as `/usr/bin/clang`. Its hash is retained
separately from the selected frontend. The explicit driver's
`-print-prog-name=clang` response must agree with exactly one `-cc1` command in its
`-###` dry run. The actual frontend's binary digest and version, requested and
effective target, and requested SDK are bound before and after capture and
checked again during verification. A custom compiler is queried directly; the
tool never replaces it with an `xcrun` selection. Ambiguous frontend reports or
a changed target fail closed. In addition to this selection probe, separate
`compiler_commands` records retain the normalized token list and digest for
the exact AST and dependency requests. Each reported frontend, effective target
and SDK must agree with the selected identity. A source observation records its
own Objective-C++ AST request too. Frontend, temporary source, SDK, compiler
resource and debug/coverage working directories use fixed labels, including
joined `-fdebug-compilation-dir=...` options; remaining absolute paths use
digests. The raw command paths are not serialized.
[`CCC_OVERRIDE_OPTIONS`](https://github.com/llvm/llvm-project/blob/main/clang/tools/driver/driver.cpp),
which rewrites Clang's explicit command line, and the `CPATH`, `C_INCLUDE_PATH`,
`CPLUS_INCLUDE_PATH`, `OBJC_INCLUDE_PATH` and `OBJCPLUS_INCLUDE_PATH` overrides
must be unset, including empty values. Compiler subprocesses inherit only
explicit executable/toolchain selection, temporary-directory/system-root
settings and a C locale. In particular `PWD`, SDK/deployment overrides and
semantic driver options are not inherited. The explicitly selected compiler
remains a trusted input: a dishonest custom driver can misreport `-###`.
Reported argv and binary digests are not execution or hardware attestation.

The extractor runs the real Clang Objective-C frontend without modules and
reads the complete AST. Filtering only names beginning with `MTL` would miss
anonymous Objective-C class extensions and Metal-owned `NSProcessInfo` APIs.
Definitions physically originating in `Metal.framework/Headers` are selected;
forward declarations and implicit property accessors are excluded, while
properties and their mutability are recorded. Function redeclarations retain
their additional source sites. Interface/protocol ancestry is recorded, rather
than pretending that every inherited method is a new implementation.

Clang's JSON `AvailabilityAttr` omits version values. The extractor follows its
actual macro expansion source span and reads recognized `API_AVAILABLE`,
`API_DEPRECATED`, `API_DEPRECATED_WITH_REPLACEMENT`, `API_UNAVAILABLE`, or native
`availability(...)` spellings. These are **spelled availability observations**;
unrecognized wrappers or nonnumeric versions remain unresolved. Source macro
definitions and other preprocessing inputs are pinned by Clang's dependency
list, in addition to all Metal header hashes, SDK settings, and compiler bytes.
Both `verify` and `observe-source` perform a fresh complete capture and require
exact equality of declarations, classification counts, compiler commands,
headers, SDK settings and dependency membership/bytes. A newly added
`__has_include` support header, deleted declaration or forged count therefore
cannot pass because old bytes remain unchanged. Source observation additionally
rechecks SDK inputs after its translation units. Clang's elided file names are
expanded in full document order, including inherited attributes, before any
declaration filtering. These are reproducible source-level census checks, not
independent certification of annotation semantics or implementation behavior.
Both `verify` and `observe-source` reject duplicate JSON object keys at every
nesting level, including equal-valued duplicates. An inventory must have one
unambiguous interpretation before its input bindings are checked.

The schema remains `mellowgpu.metal-api-inventory/1`: the additive provenance
fields do not change the declaration meaning. Readers require the exact current
generator digest and tell users to recapture older reports. They do not silently
migrate an earlier report or accept a partial provenance object. Counts require
integer types; boolean or floating-point zero cannot stand in for integer zero.

| Classification | Interpretation |
| --- | --- |
| `target_api_candidate` | Parsed macOS introduction is no later than 15.8.1; hardware/requirement review is still pending |
| `unspecified_macos_availability_candidate` | No effective parsed macOS introduction; no default assumption of a required API |
| `newer_than_target` | Parsed introduction is later than the exact target |
| `unavailable_on_macos` | Explicitly unavailable or obsoleted by the target |
| `metal4_namespace_excluded` | Name/owner explicitly references Metal 4, including references added to older Metal classes |
| `availability_unresolved` | At least one applicable spelling could not be interpreted |

In particular, new GPU-family enum constants may inherit an old enum's OS
availability. Their presence does not imply that old operating systems or a
physical GPU implement that family. All declarations retain `requirement`,
`implementation_status` and `hardware_feature_condition` as `unreviewed`.
`required_inventory_complete`, `implementation_coverage_complete`, and
`hardware_qualified` remain false; verified implementation count remains zero.

## Current runtime observations

`Runtime/MetalObjects.*` is a limited compute object layer over OpenCL.
`Runtime/RenderObjects.*` is a limited render layer over the existing GL route.
The MMA/MRA Objective-C facades deliberately do not declare complete Apple Metal
protocol conformance, and explicitly reject parts of the API. For example, the
render facade's compute and blit encoder entrypoints return unsupported errors.
These retained experiments are not the complete native frontend in the
MellowGPU specification.

`observe-source` runs the real Objective-C++ frontend on these two facade
translation units and records explicit method bodies under documented class
role aliases. A matching selector is only a source observation: a body returning
an unsupported error counts in the observation list too. It does not verify a
signature match, inherited selectors, semantics, shaders, native resources,
native completion, display, or hardware execution.

```sh
python3 Tools/metal-api-inventory.py observe-source \
  --sdk /path/to/MacOSX.sdk --repo . \
  --inventory build/metal-api.json --out build/metal-source-observations.json
```

The unchanged render facade currently fails actual SDK 15.5 and 26.5 syntax checks at
its use of `MTLMaxRenderPassSampleBuffers` in
`Userspace/AppleMetal/MellowAppleRenderMetal.mm`. No substitute macro is injected
by this tool. The recorded `--facade compute` observation contains 56 actual
method bodies and explicitly lists the render facade as unobserved. It records
zero semantically verified implementations. The default `--facade all` fails
when either translation unit fails; selecting one facade cannot make the other
one disappear from the report's unobserved list.

## Required next review and implementation boundary

The generated records are an auditable starting set, not the final requirements
manifest. A reviewed profile must classify every candidate against the selected
Metal 3 programming model, hardware-feature predicates and limits. It must also
add requirements not represented by the Metal framework Objective-C headers:
MSL semantics and compiler behavior, MetalFX where required by the selected
profile, QuartzCore/CAMetalLayer, cross-process IOSurface and synchronization,
and the actual WindowServer/IOAccelerator/display ABI. Public header extraction
cannot resolve private WindowServer contracts.

Each required operation then needs an implementation reference and negative and
positive semantic tests, followed by native hardware records for the exact
vendor/GPU/OS/kernel/userspace/firmware/compiler tuple. Method-name matches, CPU
policy tests, parser success, API stubs and copied pixels cannot satisfy those
gates. A supported profile additionally needs real WindowServer composition,
scanout/flip/retirement, application and game workloads, recovery, and performance
evidence as required by `docs/MELLOWGPU-SPEC.md`. The inventory tool has no code
path that promotes a device or changes the qualification profile.

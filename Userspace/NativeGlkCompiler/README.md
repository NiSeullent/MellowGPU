# Native GLK compute compiler (host-only)

An offline compiler for Intel Gemini Lake (Gen9 LP, PCI `8086:3184` and
`8086:3185`) compute kernels. It is a small adapter over the pinned upstream
Mesa `f5cb8ee032adabef599ae892ec4e42d796b84da9` SPIR-V front end (`spirv_to_nir`),
NIR, and `brw` backend (`brw_compile` → `brw_compile_cs`). Every instruction it
returns comes from upstream `brw`. The adapter then checks the program again
with upstream `gen_decode`/`gen_validate` and disassembles it with `gen_print`.
Nothing here is CPU shader emulation, hard-coded ISA, or a stub compiler.

**Evidence boundary:** compilation runs only on the build host (Darwin arm64).
The produced ISA has **not executed**: `gpu_execution=NOT_RUN`. There is no kernel
upload, RCS submission, binding-table/surface-state construction, CURBE upload,
GPU qualification or Metal qualification (`qualified_profiles=[]`).

## Layout

| Path | Contents |
| --- | --- |
| `include/mellow_glk_compiler.h` | C ABI (`MGLK_ABI_VERSION 2`), ownership rules, bounds |
| `src/mellow_glk_compiler.c` | Adapter: input bounding, iris-order lowering, binding map, metadata, ISA checks |
| `tools/mglk_compile.c` | `mglk-compile` executable: SPIR-V file → ISA, disassembly, log, JSON |
| `tests/glk_compiler_tests.c` | Host regression over typed NIR and SPIR-V, positive and negative |
| `patches/` | The only two Mesa changes, both to the meson build (no compiler sources) |
| `provenance/mesa-glk-compiler.lock.json` | Mesa commit/tree, archive hash, patches, wheel hashes, meson options, targets |

Mesa source, wheels, venv, build tree and binaries stay in an out-of-tree work
directory and are never copied into this tree.

## Build and test

```sh
python3 -I Tools/build-glk-shader-compiler.py --work <dir-outside-checkout> [--fetch]
python3 -I Tools/run-glk-shader-compiler-tests.py --work <same-dir>
```

The build tool:

- verifies the Mesa remote, commit, tree and version, and applies or verifies
  the locked patches;
- builds a venv from hash-pinned wheels and configures Mesa's meson recipe with
  the locked options (`b_ndebug=false`);
- builds the locked static libraries with at most two jobs;
- compiles the adapter with the exact upstream flags of `brw_nir.c`;
- links `glk_compiler_tests` and `mglk-compile` with `ZERO_AR_DATE=1`.

`<work>/build-manifest.json` records every command, flag, tool version and
hash, and compares the artifacts to the previous build. It also lists the
linked upstream objects that import `_ioctl`; the adapter never calls them.
`<work>/provenance/compiled-sources.json` lists every compiled upstream or
generated file with its git blob, sha256 and license notice.

The runner checks:

- the disassembly re-assembles byte for byte to the shipped program;
- send `bti(N)` matches the resource table;
- input constants appear as ISA immediates;
- every recorded SPIR-V module replayed through `mglk-compile` reproduces the
  library output byte for byte.

It writes `<work>/test-run/report.json` and `<work>/source-freeze.json`. The
freeze holds product source hashes, the stable freeze digest, the exact flags,
and the explicit `outstanding` list of paths that are not implemented, not
exercised, or rejected.

## `mglk-compile`

```sh
mglk-compile [--device 0x3185] [--entry main] \
             [--binding SET:BINDING:BTI:ssbo|ubo]... --out PREFIX input.spv
```

Exit status: `0` means compiled, `1` means rejected or failed (diagnostic on
stderr and in `PREFIX.json`), `2` means a usage or output error. On success it
writes `PREFIX.isa.bin` (EU instructions followed by constant data),
`PREFIX.disasm.txt`, `PREFIX.log.txt` and `PREFIX.json`.

Metadata a future GLK submission owner must honour:

- Every SPIR-V `(set, binding)` the module uses must be mapped to a Gen9 binding
  table index 0..239, and the map must not contain duplicates. The resource
  table reports BTI, kind and observed access. It is a superset of what the ISA
  addresses.
- Sends to BTI 254 are Gen9 shared local memory (`total_shared` bytes) and need
  no surface state.
- `push_per_thread_dwords` > 0 means per-thread push dword
  `subgroup_id_push_dword` must hold each hardware thread's subgroup id. The
  layout follows upstream `brw_cs_fill_push_const_info`, as iris uses it.
- `simd_mask`/`prog_offset`, `dispatch_*`, `local_size`, `total_scratch`, and the
  relocation list must be applied as reported.

Only the `Input`, `Uniform`, `Workgroup`, `Private`, `Function` and
`StorageBuffer` storage classes are accepted. Push constants, images,
samplers, physical storage buffers, descriptor arrays and variable workgroup
sizes are rejected with a diagnostic. Runtime EU fusing is unknown; topology
comes from the static PCI table.

## License boundary

Adapter, tool and test sources are MIT (`SPDX-License-Identifier: MIT`). Mesa
is not vendored. The patches modify only Mesa build files. Upstream notices
(`docs/license.rst`, `licenses/*`) are hashed in the build manifest.

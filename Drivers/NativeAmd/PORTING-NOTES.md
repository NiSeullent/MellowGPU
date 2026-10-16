# AMD GFX9 PM4 packet encoding

This directory starts the AMD lane with two packet units:

- PM4 type-3 header encoding, decoding and stream measurement for the SOC15
  (GFX9) format.
- The GFX9 `RELEASE_MEM` end-of-pipe completion fence. It covers exactly the
  variants that Linux `gfx_v9_0_ring_emit_fence` emits.

It builds packets only. It does not admit a device, own a ring, publish a write
pointer, touch MMIO, route interrupts, or run on a GPU. `Architecture::Gfx9`
names a packet format, not a detected device or a hardware support claim.

The immutable upstream revision is torvalds/linux
`0d9ff90a5422cc7509258aaaba1e7481df4d332a`. These AMD MIT sources were
inspected. Full-file SHA-256 hashes are in `provenance.json`.

- [soc15d.h](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdgpu/soc15d.h):
  `PACKET3`, `PACKET3_COMPUTE`, `CP_PACKET_GET_*`, `PACKET3_NOP` 0x10,
  `PACKET3_RELEASE_MEM` 0x49, `EVENT_TYPE`/`EVENT_INDEX`, `EOP_*`,
  `DATA_SEL`/`INT_SEL`/`DST_SEL` and their value comments.
- [kfd_pm4_headers_ai.h](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdkfd/kfd_pm4_headers_ai.h)
  (GPL-2.0 OR MIT, used under MIT): the count is body dwords minus one, and
  header bits 7:0 are reserved. It gives the 8-dword `pm4_mec_release_mem`
  layout, `end_of_pipe` event index 5, and `CACHE_FLUSH_AND_INV_TS_EVENT` 0x14.
- [vega10_enum.h](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/include/vega10_enum.h):
  `VGT_EVENT_TYPE` `CACHE_FLUSH_AND_INV_TS_EVENT` = 0x14.
- [gfx_v9_0.c](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c):
  the generation semantics.
  - `gfx_v9_0_ring_emit_fence` emits `PACKET3(PACKET3_RELEASE_MEM, 6)` on GFX
    and compute rings. Its cache actions depend on `TC_WB_ONLY`. It sets
    `DATA_SEL` 1/2, `INT_SEL` 0/2 and qword/dword alignment, and writes a zero
    final dword.
  - `.nop = PACKET3(PACKET3_NOP, 0x3FFF)` serves as a one-dword NOP.
  - `gfx_v9_ring_insert_nop` caps NOP counts at 0x3FFE.
  - KIQ fences use `WRITE_DATA` instead.
- [amdgpu_ring.h](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.h):
  `AMDGPU_FENCE_FLAG_64BIT`, `_INT`, `_TC_WB_ONLY` and `_EXEC`.
- [amdgpu_gmc.h](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.h):
  the 48-bit VA hole, with `AMDGPU_GMC_HOLE_START` 0x0000800000000000.

The C++ is original field packing, written from these published definitions.
No upstream function body was copied. The AMD notices are kept in the sources
and in `LICENSE.MIT`.

## Adapter policy

These rules are policy, not hardware discoveries:

- Count 0x3FFF is emitted only as the plain one-dword NOP 0xFFFF1000.
  - For NOP, a 0x4000-dword body is impossible, so it is `InvalidCount`.
  - For other opcodes, and for a compute-bit NOP, its meaning is not
    established, so it is `UnsupportedCount`.
  - Header bits 7:0 other than the `PACKET3_COMPUTE` bit are rejected.
- Opcode and count are never masked, so out-of-range values cannot alias
  another packet.
- The fence requires 64-bit data to be 8-byte aligned and low-32-bit data to be
  4-byte aligned. A 32-bit fence needs a sequence that fits 32 bits; it is not
  silently truncated.
- Fence addresses must lie below `AMDGPU_GMC_HOLE_START` of the 48-bit layout.
  There, the sign-extended and hole-masked forms are identical.
- The decoder is a strict inverse of the encoder. Any other bit pattern is
  rejected, and outputs are unchanged on every error.

## Not supported, and the missing facts

- **GFX10, GFX11 and GFX12:** `nvd.h` and `gfx_v10_0.c`/`gfx_v11_0.c`/
  `gfx_v12_0.c` were not inspected. No compatibility is claimed.
- **Device mapping:** which PCI devices or GC IP versions use this format was
  not checked (`amdgpu_discovery.c` not inspected).
- **Addresses at or above the hole:** it is not established whether the CP's
  `RELEASE_MEM` takes the `amdgpu_gmc_sign_extend` canonical form or the
  `AMDGPU_GMC_HOLE_MASK` linear form there. The same gap applies to the 57-bit
  (`max_level == 4`) layout.
- **`EOP_EXEC` (bit 28):** `soc15d.h` names it "For Trailing Fence", and
  `gfx_v9_0.c` sets it for `AMDGPU_FENCE_FLAG_EXEC`. The KFD MEC layout marks
  the bit reserved, so its meaning per engine is not established.
- **Other documented fields:** `DST_SEL` TC/L2, `INT_SEL` 1 and 3-6,
  `DATA_SEL` 0 and 3-5, other events, the `STREAM` cache policy, and the
  volatile or write-combine actions are all documented. The inspected GFX9
  fence path never emits them, and their combined semantics are not
  established.
- **Interrupts and other fence forms:** EOP interrupt routing, IH decoding,
  `int_ctxid` meaning and KIQ `WRITE_DATA` fences are not covered.

## Tests

The host tests use hand-evaluated header and fence vectors and an independent
bit-level layout check keyed to the KFD struct. They also cover:

- an exhaustive sweep of header type, reserved bits and opcodes,
- stream-length and overflow limits,
- truncation and alias rejection,
- unsupported fields, and unchanged outputs on error,
- a single-bit-flip proof that the decoder accepts exactly the encodable
  neighbours of a packet.

A separate local check did the following and found no mismatch on 2026-10-16:

- compiled the pinned upstream `soc15d.h`, `kfd_pm4_headers_ai.h` and
  `vega10_enum.h` against this encoder,
- compared `PACKET3`, `PACKET3_COMPUTE`, `CP_PACKET_GET_*` and
  `pm4_mec_release_mem` bitfields,
- compared the Linux fence flag mapping, restated with the upstream macros.

Those headers are not vendored. None of this proves GPU execution.

Direct host build (run from the repository root):

```sh
c++ -std=c++17 -Wall -Wextra -Wconversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I. Drivers/NativeAmd/PacketEncoding.cpp tests/native_amd_packet_tests.cpp -o build/native-amd-packet-tests
build/native-amd-packet-tests
```

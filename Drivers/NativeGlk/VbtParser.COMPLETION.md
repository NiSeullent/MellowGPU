# GLK VBT parser completion

This isolated candidate completes actual `VbtParser.cpp` bodies. Claude's interrupted role left the 538-line header and 1,116-line synthetic fixture suite, without a `.cpp`. Their exact before copies and SHA256 values are under `completion-evidence/`; the missing implementation produced an actual link failure. Codex completed the implementation, corrected source-disproved fixture assumptions, and added regressions. No canonical tree, target computer, firmware, PCI configuration, MMIO, or boot state was modified.

The five integration files are `Drivers/NativeGlk/VbtParser.{hpp,cpp,provenance.json}`, `tests/native_glk_vbt_tests.cpp`, and `Tools/run-native-glk-vbt-tests.py`. They use namespace `Mellow::NativeGlk`. The existing panel sequencer uses `MellowNativeGlk`, so a reviewed typed bridge is still required; a namespace cast is not a bridge.

## Behavior and evidence

`parseVbt` checks VBT/BDB bounds/signatures and block extents before decoding, uses explicit little-endian bounded loads, preserves unknown block records, rejects duplicated decoded blocks, and clears the complete document on structural failure. No input pointer survives. Child fields follow version gates. LFP tables require valid BDB-relative addresses, uniform strides/sizes, contained extents, and all 16 FP terminators. Partial eDP/backlight blocks expose only fields whose bytes actually exist. Unsupported or missing panel/controller choices remain unknown.

`buildGlkEdpPanelProfile` follows explicit OpRegion/VBT/PnP selection without an index-0 fallback. Its `ready` flag means complete parsed configuration data only. It also requires a zero VBT byte sum; diagnostically parsing a checksum mismatch does not make it ready. It does not validate connector topology, EDID authenticity, panel capability, native ownership, current firmware state, or PPS programming safety. Checksum is an integrity check, not authentication. The document occupies several KiB and must be caller-owned heap/static storage, not a kernel stack.

Frozen final runner evidence is `completion-evidence/final/result.json`:

- 711 actual parser/selection checks, fatal AddressSanitizer and UndefinedBehaviorSanitizer; 0 failures.
- 4,000 deterministic mutated images plus a sweep of every proper prefix of a valid image. Synthetic `ready` counts are test bookkeeping, never hardware qualification.
- 26 complete packed declarations extracted from exact Linux `intel_vbt_defs.h`, compiled against parser constants: structure size/offset assertions plus 814 bitfield-byte checks. The same offset oracle also compiles for x86_64.
- Explicit macOS SDK 15.5, target x86_64-apple-macos15.8.1, `-mkernel -ffreestanding -nostdinc++ -fno-builtin`, warning-as-error build. `-Wframe-larger-than=1024` passes; `nm -u` is empty. Object SHA256 is in the report.
- This is source/object compilation, not a KDK/KPI link, kext load, target execution, actual VBT capture, panel or Metal/WindowServer qualification.

Reproduce in a fresh output directory:

```sh
python3 -B Tools/run-native-glk-vbt-tests.py \
  --linux-root /absolute/path/to/verified/linux \
  --kernel-sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk \
  --out /absolute/fresh/output
```

The runner refuses source pin drift and existing output directories, records full compiler commands/results, and checks source files remain unchanged. Primary commit is `0d9ff90a5422cc7509258aaaba1e7481df4d332a`; exact Git blob SHA1, byte length, SHA256, license notice, and URLs are in `VbtParser.provenance.json`.

The first actual implementation run had six fixture failures: the encoded fixture contains 11 blocks, not 12; shortening block 41 by three bytes moves subsequent block-42 BDB addresses by three; and three single-bit DTD tests asserted unclamped ends with a zero total even though Linux `fill_detail_timing_data` always clamps them. Before evidence remains intact. Additional actual RED→GREEN cases reject checksum-mismatched data-ready profiles (2 checks), reserved child flags below their introduction versions (30 checks), and unknown version-236 brightness precision (1 check). The raw versioned fields are still retained when present, without claiming absent bytes were observed.

## Exact VBT-to-panel conversion obligations

For a selected validated `profile.panelType` and `profile.childIndex`, the semantic PPS mapping is:

| Parser value | PanelPower `PpsDelays` field | Unit |
| --- | --- | --- |
| `pps.t1t3` | `powerUp` | 100 microseconds |
| `pps.t8` | `backlightOn` | 100 microseconds |
| `pps.t9` | `backlightOff` | 100 microseconds |
| `pps.t10` | `powerDown` | 100 microseconds |
| `pps.t11t12` | `powerCycle` | 100 microseconds |

Copy the raw 16-bit units into `PpsDelays`; do **not** feed `t*Ms` or `t*Us` into that structure. The microsecond/millisecond fields are derived diagnostic conversions (`units*100`, `ceil(units/10)`). A single zero delay remains zero; the parser supplies no specification substitute. Before programming, the actual sequencer's `computeTiming(observedHardware, vbt, out)` must perform the hardware/VBT/specification merge, T8/T9 software-wait handling, 100 ms cycle encoding, and field-range checks. A parsed profile is insufficient to call it with invented hardware observations.

Use `ppsInstance` only with its explicit `ppsControllerSource` and a selected valid PWM type-2 entry. At BDB 191+, `VbtControlByte` requires the actual selected `controlKnown` byte and controller 0/1. Below 191, `LegacyBefore191` names the pinned `parse_lfp_backlight` rule assigning PPS0 after a valid PWM entry; it does **not** claim that a controller byte or hardware PPS selection was observed. Missing/invalid entries never obtain this fallback. A typed bridge must preserve the origin and exact version, then retain `PanelPower::resolveController` and its native two-PPS read/conflict, display ownership and power admission gates. It must never fabricate `FactState::Observed` from a zero-initialized document: the selected PWM/VBT fields need captured bytes/provenance. Port/AUX enum values must be converted explicitly, not cast between unrelated modules.

The profile's reviewed BDB format policy is 155..264, with 264 taken from the pinned source's last known child-size version. This is not an observed or supported GLK firmware-version range, panel/connector compatibility claim, or caller authority to accept a future firmware image. Versions beyond 264 remain diagnostic-only; a native caller must independently bind the captured VBT, selected connector and live hardware to its narrower supported platform contract before programming.

Native prerequisites still missing include a real 15IGL7 VBT capture with owned immutable lifetime/provenance, real OpRegion/EDID inputs, selected physical connector confirmation, display-power/native-owner leases, and firmware display ownership transfer. No actual VBT or panel was available for this implementation. There is no MMIO writer, PPS0 guess, hardware success stub, or support qualification in this candidate.

A later independent bounded Codex peer reviewed the production parser against pinned Linux and the existing panel sequencer. It reproduced the source-unsupported pre-191 PWM refusal (5 failing checks), then corrected it with an explicitly sourced legacy PPS0 selection. The new profile `PpsControllerSource::LegacyBefore191` is distinct from `VbtControlByte`; neither is hardware write permission. Missing/invalid selected PWM entries and missing modern control bytes still refuse a ready profile. The peer final result is `completion-evidence/peer-legacy-fix/final/result.json`: 735 fatal-ASan/UBSan parser checks, the same 4,000 mutation cases and 814 primary-layout byte checks, plus fresh SDK 15.5 x86 kernel-style compile with no undefined imports. Original 711-check freeze remains historical, with its six source files preserved under `peer-legacy-fix/before/`. No actual VBT/panel capture or native panel bridge was added.

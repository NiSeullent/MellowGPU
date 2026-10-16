#!/usr/bin/env python3
"""Build and test the GLK GGTT publication core and native IOKit adapter.

Hosted injected-transport suites under fatal ASan/UBSan in bounded child
processes, source-mutation RED checks, and an optional exact-SDK15.5 x86_64
kernel object compile with disassembly and import checks. No driver load,
PCI access, MMIO, DMA, TLB operation or GPU work happens: nothing produced
here is hardware evidence.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = [
    "Drivers/NativeGlk/GgttPublication.hpp", "Drivers/NativeGlk/GgttPublication.cpp",
    "Drivers/NativeGlk/GgttPublication.provenance.json", "Drivers/NativeGlk/GgttPublication.LICENSE.MIT",
    "Mellow/GlkGgttIOKit.hpp", "Mellow/GlkGgttIOKit.cpp",
    "tests/native_glk_ggtt_publication_tests.cpp", "tests/native_glk_ggtt_iokit_tests.cpp",
    "tests/native_glk_ggtt_iokit_shim.hpp", "Tools/run-native-glk-ggtt-tests.py",
]
SEEDED = [
    "Drivers/NativeGlk/PageTableEncoding.hpp", "Drivers/NativeGlk/PageTableEncoding.cpp",
    "Drivers/NativeGpu/MemoryOwner.hpp", "Drivers/NativeGpu/MemoryOwner.cpp",
    "Mellow/GlkDmaMemoryIOKit.hpp",
]
SUITES = {
    "core": ["tests/native_glk_ggtt_publication_tests.cpp", "Drivers/NativeGlk/GgttPublication.cpp",
             "Drivers/NativeGlk/PageTableEncoding.cpp"],
    "adapter": ["tests/native_glk_ggtt_iokit_tests.cpp", "Mellow/GlkGgttIOKit.cpp",
                "Drivers/NativeGlk/GgttPublication.cpp", "Drivers/NativeGlk/PageTableEncoding.cpp",
                "Drivers/NativeGpu/MemoryOwner.cpp"],
}
HOSTED = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DMELLOW_GLK_DMA_HOSTED_FENCE",
          "-DMELLOW_GLK_GGTT_HOSTED_IO", "-Itests/native_glk_ggtt_shim", "-I."]
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
NATIVE_SOURCES = ["Drivers/NativeGlk/GgttPublication.cpp", "Drivers/NativeGlk/PageTableEncoding.cpp",
                  "Mellow/GlkGgttIOKit.cpp"]
# Each mutant must compile and be killed by at least one suite.
MUTATIONS = [
    ("skip-entry-readback", "Drivers/NativeGlk/GgttPublication.cpp",
     "else if (!sameImage(expected, actual)) status = GgttStatus::ReadbackMismatch;",
     "else if (false) status = GgttStatus::ReadbackMismatch;"),
    ("skip-publish-invalidation", "Drivers/NativeGlk/GgttPublication.cpp",
     "    if (status == GgttStatus::Ok) status = invalidateEngineTlbs(awake);\n    const bool released = gt_.release(gt_.context, token);\n    if (status != GgttStatus::Ok) return quarantine(*slot, status);\n    if (!released) return quarantine(*slot, GgttStatus::TransportFailure);\n    slot->state = GgttExtentState::Published;",
     "    const bool released = gt_.release(gt_.context, token);\n    if (status != GgttStatus::Ok) return quarantine(*slot, status);\n    if (!released) return quarantine(*slot, GgttStatus::TransportFailure);\n    slot->state = GgttExtentState::Published;"),
    ("asleep-engine-invalidated", "Drivers/NativeGlk/GgttPublication.cpp",
     "        if (!(awake & (1U << i))) continue;\n        if (!io_.writeRegister(io_.context, kEngineTlbControls[i], kTlbInvalidateBit))",
     "        if (false) continue;\n        if (!io_.writeRegister(io_.context, kEngineTlbControls[i], kTlbInvalidateBit))"),
    ("missing-awake-producer-accepted", "Drivers/NativeGlk/GgttPublication.cpp",
     "!gt.context || !gt.acquire || !gt.release || !gt.awakeEngines",
     "!gt.context || !gt.acquire || !gt.release"),
    ("engine-order", "Drivers/NativeGlk/GgttPublication.hpp",
     "kEngineTlbControls[kEngineCount] = {kRtcr, kBtcr, kM1tcr, kVtcr};",
     "kEngineTlbControls[kEngineCount] = {kRtcr, kM1tcr, kBtcr, kVtcr};"),
    ("quarantine-does-not-latch", "Drivers/NativeGlk/GgttPublication.cpp",
     "    slot.state = GgttExtentState::Quarantined;\n    faulted_ = true;",
     "    slot.state = GgttExtentState::Quarantined;\n    faulted_ = false;"),
    ("retained-ranges-ignored", "Drivers/NativeGlk/GgttPublication.cpp",
     "        if (intersects(start, bytes, retained_[i].start, retained_[i].bytes)) return GgttStatus::Overlap;",
     "        (void)i;"),
    ("short-slow-timeout", "Drivers/NativeGlk/GgttPublication.cpp",
     "now - start > uint64_t(kTlbSlowTimeoutMs) * 1000000;",
     "now - start > uint64_t(kTlbSlowTimeoutMs) * 100000;"),
    ("prefetchable-bar-accepted", "Drivers/NativeGlk/GgttPublication.cpp",
     "    if (barLow & kBarPrefetchable) return GgttStatus::InvalidArgument;",
     "    if (false) return GgttStatus::InvalidArgument;"),
    ("write-combined-gsm", "Mellow/GlkGgttIOKit.cpp",
     "gsmMap_ = bar->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache,",
     "gsmMap_ = bar->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache,"),
    ("unmap-without-retirement", "Mellow/GlkGgttIOKit.cpp",
     "    if (!retiredNow) return MemoryStatus::Busy;",
     "    (void)retiredNow;"),
    ("register-map-leaked", "Mellow/GlkGgttIOKit.cpp",
     "    if (registerMap) registerMap->release();",
     "    (void)registerMap;"),
    ("uncertain-map-reported-clean", "Mellow/GlkGgttIOKit.cpp",
     "    if (status != Ggtt::GgttStatus::Ok) return MemoryStatus::IoFailure;\n    return MemoryStatus::Ok;",
     "    if (status != Ggtt::GgttStatus::Ok) { mapping = {}; return MemoryStatus::Unavailable; }\n    return MemoryStatus::Ok;"),
]
FORBIDDEN_IMPORTS = re.compile(r"^(__Znw|__Zna|__Zdl|__Zda|___cxa_|___gxx_|__ZSt|_objc_)")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, *, cwd=ROOT, environment=None, timeout=180, keep=20000):
    environment = dict(os.environ if environment is None else environment)
    environment.pop("CCC_OVERRIDE_OPTIONS", None)
    try:
        result = subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True, text=True,
                                env=environment, check=False, timeout=timeout)
        return {"argv": [str(part) for part in command], "exit_code": result.returncode,
                "stdout": result.stdout[-keep:] if keep else result.stdout,
                "stderr": result.stderr[-keep:] if keep else result.stderr}
    except subprocess.TimeoutExpired as error:
        return {"argv": [str(part) for part in command], "exit_code": None, "timeout": timeout,
                "stdout": str(error.stdout or "")[-4000:], "stderr": str(error.stderr or "")[-4000:]}


def dependencies(command, cwd=ROOT):
    result = run(command, cwd=cwd, keep=None)
    if result["exit_code"] != 0:
        raise RuntimeError("dependency discovery failed: " + result["stderr"][-2000:])
    text = result["stdout"].replace("\\\n", " ")
    files = sorted({(cwd / value).resolve() for value in shlex.split(text.split(":", 1)[1])})
    if not files or any(not path.is_file() for path in files):
        raise ValueError("invalid compiler dependencies")
    return {str(path): digest(path) for path in files}


def test_environment():
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=0"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return environment


def functions(disassembly):
    blocks, name = {}, None
    for line in disassembly.splitlines():
        header = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if header:
            name = header.group(1)
            blocks[name] = []
        elif name and line.strip():
            blocks[name].append(line.strip())
    return blocks


def memory_stores(instructions):
    stores = []
    for line in instructions:
        match = re.search(r"\b(mov[bwlq]?)\s+[%$][^,]+,\s*(-?0x[0-9a-f]+)?\((%r\w+)", line)
        if match and match.group(3) not in ("%rsp", "%rbp"):
            stores.append(match.group(1))
    return stores


def verify_provenance(provenance, roots):
    record = {"files_verified": 0, "files_unlocated": [], "mismatches": [], "fragments_verified": 0}
    texts = {}
    for entry in provenance["upstream_files"]:
        relative = entry["path"]
        candidates = [Path(root) / relative for root in roots] + [Path(root) / Path(relative).name for root in roots]
        found = next((path for path in candidates if path.is_file() and digest(path) == entry["sha256"]), None)
        if not found:
            located = [str(path) for path in candidates if path.is_file()]
            (record["mismatches"] if located else record["files_unlocated"]).append(relative)
            continue
        data = found.read_bytes()
        if hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() != entry["git_blob"]:
            record["mismatches"].append(relative + " (git blob)")
            continue
        record["files_verified"] += 1
        texts[relative] = data.decode("utf-8").split("\n")
    for fragment in provenance["symbol_fragments"]:
        lines = texts.get(fragment["file"])
        if lines is None:
            continue
        line = lines[fragment["line"] - 1] if 0 < fragment["line"] <= len(lines) else ""
        if hashlib.sha256(line.encode()).hexdigest() != fragment["sha256"]:
            record["mismatches"].append("fragment " + fragment["symbol"])
        else:
            record["fragments_verified"] += 1
    return record


def build_suite(cxx, suite, output, *, cwd=ROOT, sanitize=True):
    binary = output / f"native-glk-ggtt-{suite}-tests"
    command = [cxx, *HOSTED, *(SANITIZE if sanitize else []), *SUITES[suite], "-o", binary]
    return binary, run(command, cwd=cwd)


def mutation_checks(cxx, output):
    results = []
    files = set(COMPONENT + SEEDED)
    files.update(str(path.relative_to(ROOT)) for path in (ROOT / "tests/native_glk_ggtt_shim").rglob("*.h"))
    for name, relative, old, new in MUTATIONS:
        workspace = output / "mutants" / name
        for file in files:
            target = workspace / file
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / file, target)
        source = (workspace / relative).read_text()
        entry = {"name": name, "file": relative, "applied": source.count(old) == 1}
        if entry["applied"]:
            (workspace / relative).write_text(source.replace(old, new))
            entry["suites"] = {}
            for suite in SUITES:
                binary, build = build_suite(cxx, suite, workspace, cwd=workspace, sanitize=False)
                outcome = {"build_exit": build["exit_code"]}
                if build["exit_code"] == 0:
                    test = run([binary], cwd=workspace, environment=test_environment(), timeout=120)
                    outcome["test_exit"] = test["exit_code"]
                    outcome["failure"] = (test["stderr"].strip().splitlines() or [""])[-1][:300]
                entry["suites"][suite] = outcome
            built = all(item["build_exit"] == 0 for item in entry["suites"].values())
            entry["killed"] = built and any(item.get("test_exit") not in (0, None) or item.get("test_exit") is None
                                            for item in entry["suites"].values())
        else:
            entry["killed"] = False
        results.append(entry)
    return results


def native_checks(cxx, sdk, output, all_dependencies):
    settings = json.loads((sdk / "SDKSettings.json").read_text())
    if settings.get("Version") != "15.5":
        raise ValueError("native compile requires exact SDK15.5")
    resource = run([cxx, "-print-resource-dir"])
    if resource["exit_code"] != 0:
        raise ValueError("compiler resource discovery failed")
    flags = [cxx, "-target", "x86_64-apple-macos15.5", "-std=c++17", "-mkernel", "-fapple-kext", "-Os",
             "-nostdinc", "-isystem", str(Path(resource["stdout"].strip()) / "include"),
             "-isystem", str(sdk / "System/Library/Frameworks/Kernel.framework/Headers"),
             "-isysroot", str(sdk), "-DKERNEL", "-DKERNEL_PRIVATE", "-fno-exceptions", "-fno-rtti",
             "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables", "-fvisibility=hidden",
             "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-Wall", "-Wextra", "-Werror",
             "-Wno-unknown-warning-option", "-Wno-ossharedptr-misuse", "-Wno-deprecated-declarations"]
    native = {"sdk": str(sdk), "sdk_settings_sha256": digest(sdk / "SDKSettings.json"),
              "scope": "x86_64 kernel object compile, relocatable link and static inspection only; "
                       "not a kext link, load or runtime ABI admission", "objects": {}}
    objects = []
    for source in NATIVE_SOURCES:
        all_dependencies.update(dependencies([*flags, "-M", "-MT", "glk", source]))
        obj = output / (Path(source).stem + ".sdk155.o")
        result = run([*flags, "-c", source, "-o", obj])
        native["objects"][source] = {"exit_code": result["exit_code"], "stderr": result["stderr"][-4000:]}
        if result["exit_code"] != 0:
            raise RuntimeError("native SDK kernel compile failed: " + source)
        native["objects"][source]["sha256"] = digest(obj)
        objects.append(obj)
    combined = output / "GlkGgtt.sdk155.relocatable.o"
    link = run(["xcrun", "ld", "-r", "-arch", "x86_64", "-o", combined, *objects])
    native["relocatable_link"] = {"exit_code": link["exit_code"], "stderr": link["stderr"][-4000:]}
    if link["exit_code"] != 0:
        raise RuntimeError("relocatable link failed")
    imports = run(["xcrun", "nm", "-u", combined], keep=None)
    names = sorted({line.strip() for line in imports["stdout"].splitlines() if line.strip()})
    native["imports"] = names
    native["forbidden_imports"] = [name for name in names if FORBIDDEN_IMPORTS.match(name)]
    disassembly = run(["xcrun", "objdump", "-d", "--no-show-raw-insn", output / "GlkGgttIOKit.sdk155.o"], keep=None)
    if disassembly["exit_code"] != 0:
        raise RuntimeError("disassembly failed")
    blocks = functions(disassembly["stdout"])

    def block(fragment):
        matches = [body for name, body in blocks.items() if fragment in name]
        return matches[0] if len(matches) == 1 else None

    write_entry, write_register, attach = (block("GlkGgttIOKit10writeEntry"), block("GlkGgttIOKit13writeRegister"),
                                           block("GlkGgttIOKit6attach"))
    vector = re.compile(r"\b(v?movup|v?movap|v?movdq|movnt|vmov)")
    checks = {
        "writeEntry_single_64bit_store": bool(write_entry) and memory_stores(write_entry) == ["movq"],
        "writeEntry_no_vector_store": bool(write_entry) and not any(vector.search(line) for line in write_entry),
        "writeRegister_single_32bit_store": bool(write_register) and memory_stores(write_register) == ["movl"],
        "attach_requests_uncached_map": bool(attach) and any("$0x101," in line for line in attach),
        "attach_never_requests_write_combine": bool(attach) and not any("$0x401," in line for line in attach),
        "gfx_flush_control_never_referenced": "0x101008" not in disassembly["stdout"],
        "no_cxx_runtime_imports": not native["forbidden_imports"],
    }
    native["static_checks"] = checks
    native["writeEntry"] = write_entry
    native["writeRegister"] = write_register
    if not all(checks.values()):
        raise RuntimeError("native static checks failed: " + ", ".join(k for k, v in checks.items() if not v))
    return native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--sdk", type=Path, help="exact macOS15.5 SDK for the native kernel object compile")
    parser.add_argument("--upstream", type=Path, action="append", default=[],
                        help="pinned upstream cache root used to re-verify provenance (repeatable)")
    parser.add_argument("--seed-manifest", type=Path, help="seed-source-hashes.json that must still match")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("clang++ is required")
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    os.chdir(ROOT)
    shim = sorted(str(path.relative_to(ROOT)) for path in (ROOT / "tests/native_glk_ggtt_shim").rglob("*.h"))
    inputs = COMPONENT + shim + SEEDED
    report = {"schema": "mellowgpu.glk-ggtt-tests/1", "status": "FAIL",
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "actual GGTT core/adapter on hosted injected transport, mutation RED checks, "
                       "optional exact-SDK x86_64 kernel object compile",
              "hardware_executed": False, "native_driver_loaded": False, "mmio_executed": False,
              "dma_executed": False, "tlb_invalidation_executed": False, "gpu_executed": False,
              "metal_qualified": False, "windowserver_qualified": False,
              "sanitizers": ["address", "undefined"],
              "leak_sanitizer": "disabled: unsupported on this macOS runtime; allocations checked by fixtures"}
    before = {name: digest(ROOT / name) for name in inputs}
    all_dependencies = {}
    try:
        report["compiler"] = run([args.cxx, "--version"])
        report["compiler_sha256"] = digest(Path(args.cxx).resolve())
        if args.seed_manifest:
            manifest = json.loads(args.seed_manifest.read_text())
            changed = [name for name, value in manifest.items() if digest(ROOT / name) != value]
            report["seed_manifest"] = {"path": str(args.seed_manifest), "entries": len(manifest), "changed": changed}
            if changed:
                raise RuntimeError("seeded inputs changed")
        report["suites"] = {}
        for suite, sources in SUITES.items():
            for source in sources:
                all_dependencies.update(dependencies([args.cxx, *HOSTED, "-MM", "-MT", "glk", source]))
            binary, build = build_suite(args.cxx, suite, output)
            entry = {"build": build}
            report["suites"][suite] = entry
            if build["exit_code"] != 0:
                raise RuntimeError(suite + " hosted build failed")
            symbols = run(["xcrun", "nm", "-u", binary])
            entry["sanitizer_symbols"] = sorted({s for s in symbols["stdout"].split() if "asan_init" in s or
                                                 s.startswith("___ubsan_handle_")})[:12]
            if "asan_init" not in symbols["stdout"] or "ubsan_handle_" not in symbols["stdout"]:
                raise RuntimeError(suite + ": actual sanitizer instrumentation is missing")
            entry["binary_sha256"] = digest(binary)
            entry["test"] = run([binary], environment=test_environment(), timeout=120)
            match = re.search(r": ([0-9]+) checks;", entry["test"]["stdout"])
            entry["checks"] = int(match.group(1)) if match else None
            if entry["test"]["exit_code"] != 0 or entry["checks"] is None:
                raise RuntimeError(suite + " hosted test failed")
        report["mutations"] = mutation_checks(args.cxx, output)
        survivors = [m["name"] for m in report["mutations"] if not m["killed"]]
        report["mutants_killed"] = len(report["mutations"]) - len(survivors)
        if survivors:
            raise RuntimeError("mutants survived or failed to build: " + ", ".join(survivors))
        provenance = json.loads((ROOT / "Drivers/NativeGlk/GgttPublication.provenance.json").read_text())
        report["provenance"] = verify_provenance(provenance, args.upstream)
        if report["provenance"]["mismatches"] or report["provenance"]["files_unlocated"]:
            raise RuntimeError("provenance missing or mismatched")
        if args.sdk:
            report["native"] = native_checks(args.cxx, args.sdk.resolve(), output, all_dependencies)
        report["status"] = "PASS_HOSTED_MUTATION_AND_NATIVE_OBJECT" if args.sdk else "PASS_HOSTED_AND_MUTATION_ONLY"
    except (OSError, ValueError, RuntimeError, IndexError, KeyError) as error:
        report["error"] = str(error)
    finally:
        report["source_sha256"] = before
        report["dependency_sha256"] = all_dependencies
        report["source_unchanged"] = all((ROOT / n).is_file() and digest(ROOT / n) == v for n, v in before.items())
        report["dependencies_unchanged"] = all(Path(p).is_file() and digest(p) == v for p, v in all_dependencies.items())
        if not report["source_unchanged"] or not report["dependencies_unchanged"]:
            report["status"] = "FAIL_INPUT_CHANGED"
        (output / "glk-ggtt-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    summary = {key: report.get(key) for key in ("status", "mutants_killed", "source_unchanged",
                                                "dependencies_unchanged", "error")}
    summary["checks"] = {suite: entry.get("checks") for suite, entry in report.get("suites", {}).items()}
    print(json.dumps(summary))
    return 0 if report["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())

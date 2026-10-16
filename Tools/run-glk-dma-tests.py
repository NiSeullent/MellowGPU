#!/usr/bin/env python3
"""Compile the real GLK DMA adapter against injected OS boundaries and SDK.

No driver load, hardware operation, DMA, MMIO, queue or setting change.
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
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, *, environment=None, timeout=90):
    environment = dict(os.environ if environment is None else environment)
    # Clang's driver override can silently remove explicit sanitizer flags.
    environment.pop("CCC_OVERRIDE_OPTIONS", None)
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                env=environment, check=False, timeout=timeout)
        return {"argv": list(map(str, command)), "exit_code": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired as error:
        return {"argv": list(map(str, command)), "exit_code": None,
                "timeout": timeout, "stdout": str(error.stdout or ""),
                "stderr": str(error.stderr or "")}


def dependencies(command):
    result = run(command)
    if result["exit_code"] != 0:
        raise RuntimeError("dependency discovery failed: " + result["stderr"])
    text = result["stdout"].replace("\\\n", " ")
    files = sorted({Path(value).resolve() for value in shlex.split(text.split(":", 1)[1])})
    if not files or any(not path.is_file() for path in files):
        raise ValueError("invalid compiler dependencies")
    return {str(path): digest(path) for path in files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--sdk", type=Path, help="Optional native macOS15.5 SDK kernel compile")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("clang++ is required")
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    os.chdir(root)
    report = {"schema": "mellowgpu.glk-dma-tests/1", "status": "FAIL",
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "actual adapter hosted OS boundaries and optional native SDK object compile",
              "hardware_executed": False, "native_driver_executed": False,
              "dma_executed": False, "gpu_executed": False,
              "metal_qualified": False, "windowserver_qualified": False,
              "sanitizers": ["address", "undefined"] if args.sanitize else [],
              "leak_sanitizer": "disabled: unsupported on this macOS runtime; host allocations checked by fixture",
              "native_linked": False, "native_loaded": False, "native": None}
    source_names = ["Mellow/GlkDmaMemoryIOKit.cpp", "Mellow/GlkDmaMemoryIOKit.hpp",
                    "Drivers/NativeGpu/MemoryOwner.cpp", "Drivers/NativeGpu/MemoryOwner.hpp",
                    "tests/native_glk_dma_iokit_tests.cpp", "tests/native_glk_dma_iokit_shim.hpp",
                    "tests/native_glk_dma_iokit_provenance.json", "Tools/run-glk-dma-tests.py"]
    source_names += [str(path.relative_to(root)) for path in sorted((root / "tests/native_glk_dma_shim").rglob("*.h"))]
    before = {name: digest(root / name) for name in source_names}
    all_dependencies = {}
    try:
        report["compiler"] = run([args.cxx, "--version"])
        report["compiler_invocation_sha256"] = digest(Path(args.cxx))
        base = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-DMELLOW_GLK_DMA_HOSTED_FENCE", "-Itests/native_glk_dma_shim", "-I."]
        inputs = ["tests/native_glk_dma_iokit_tests.cpp", "Mellow/GlkDmaMemoryIOKit.cpp",
                  "Drivers/NativeGpu/MemoryOwner.cpp"]
        for source in inputs:
            all_dependencies.update(dependencies([*base, "-MM", "-MT", "glk", source]))
        binary = output / "native-glk-dma-tests"
        options = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"] if args.sanitize else []
        report["build"] = run([*base, *options, *inputs, "-o", str(binary)])
        if report["build"]["exit_code"] != 0:
            raise RuntimeError("hosted adapter compile failed")
        if args.sanitize:
            report["sanitizer_imports"] = run(["xcrun", "nm", "-u", str(binary)])
            symbols = report["sanitizer_imports"]
            if symbols["exit_code"] != 0 or "asan_init" not in symbols["stdout"] or "ubsan_handle_" not in symbols["stdout"]:
                raise RuntimeError("actual sanitizer instrumentation is missing")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=0"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1"
        report["tests"] = [run([str(binary), *option], environment=environment, timeout=30)
                           for option in [[], ["--native-direct-no-dmar"], ["--native-bypassed"]]]
        if any(test["exit_code"] != 0 for test in report["tests"]):
            raise RuntimeError("actual hosted adapter test failed")
        match = re.search(r"boundary: ([0-9]+) checks;", report["tests"][0]["stdout"])
        if not match:
            raise RuntimeError("missing actual test count")
        report["checks"] = int(match.group(1))
        report["binary_sha256"] = digest(binary)
        if args.sdk:
            sdk = args.sdk.resolve()
            settings = json.loads((sdk / "SDKSettings.json").read_text())
            if settings.get("Version") != "15.5":
                raise ValueError("native compile requires exact SDK15.5")
            resource = run([args.cxx, "-print-resource-dir"])
            if resource["exit_code"] != 0:
                raise ValueError("compiler resource discovery failed")
            flags = [args.cxx, "-target", "x86_64-apple-macos15.5", "-std=c++17", "-mkernel", "-fapple-kext",
                     "-nostdinc", "-isystem", str(Path(resource["stdout"].strip()) / "include"),
                     "-isystem", str(sdk / "System/Library/Frameworks/Kernel.framework/Headers"),
                     "-isysroot", str(sdk), "-DKERNEL", "-DKERNEL_PRIVATE", "-fno-exceptions", "-fno-rtti",
                     "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables", "-fvisibility=hidden",
                     "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unknown-warning-option", "-Wno-ossharedptr-misuse", "-Wno-deprecated-declarations"]
            source = "Mellow/GlkDmaMemoryIOKit.cpp"
            native_dependencies = dependencies([*flags, "-M", "-MT", "glk", source])
            all_dependencies.update(native_dependencies)
            obj = output / "GlkDmaMemoryIOKit.sdk155.o"
            native = run([*flags, "-c", source, "-o", str(obj)])
            native["sdk_settings_sha256"] = digest(sdk / "SDKSettings.json")
            native["scope"] = "x86_64 native kernel object compile only; not linked or runtime ABI admission"
            native["warning_exception"] = "public SDK marks required IOPCIDevice deprecated in favor of PCIDriverKit"
            report["native"] = native
            if native["exit_code"] != 0:
                raise RuntimeError("native SDK kernel object compile failed")
            native["object_sha256"] = digest(obj)
            native["imports"] = run(["xcrun", "nm", "-u", str(obj)])
            if native["imports"]["exit_code"] != 0:
                raise RuntimeError("native object import inspection failed")
        report["status"] = "PASS_HOSTED_AND_NATIVE_OBJECT" if args.sdk else "PASS_HOSTED_ONLY"
    except (OSError, ValueError, RuntimeError, IndexError) as error:
        report["error"] = str(error)
    finally:
        report["source_sha256"] = before
        report["dependency_sha256"] = all_dependencies
        report["source_unchanged"] = all((root / name).is_file() and digest(root / name) == value for name, value in before.items())
        report["dependencies_unchanged"] = all(Path(path).is_file() and digest(Path(path)) == value for path, value in all_dependencies.items())
        if not report["source_unchanged"] or not report["dependencies_unchanged"]:
            report["status"] = "FAIL_INPUT_CHANGED"
        (output / "glk-dma-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report.get(key) for key in ("status", "checks", "source_unchanged", "dependencies_unchanged", "error")}))
    return 0 if report["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())

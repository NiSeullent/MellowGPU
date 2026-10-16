#!/usr/bin/env python3
"""Actual GLK IRQ bodies, hosted OS boundary and native SDK objects; no load/MMIO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def call(argv, timeout=60):
    env = os.environ.copy()
    env.pop("CCC_OVERRIDE_OPTIONS", None)
    env.update(ASAN_OPTIONS="abort_on_error=1:detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env)
        return {"argv": list(map(str, argv)), "exit_code": r.returncode, "stdout": r.stdout, "stderr": r.stderr}
    except subprocess.TimeoutExpired:
        return {"argv": list(map(str, argv)), "exit_code": None, "timeout": timeout}


def deps(argv):
    result = call(argv)
    if result["exit_code"] != 0:
        raise RuntimeError("dependency compile failed: " + result.get("stderr", "timeout"))
    body = result["stdout"].replace("\\\n", " ").split(":", 1)[1]
    paths = sorted({Path(p).resolve() for p in shlex.split(body)})
    if not paths or any(not p.is_file() for p in paths):
        raise ValueError("invalid actual compiler dependency set")
    return {str(p): sha(p) for p in paths}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cxx", default=shutil.which("clang++"))
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--sdk", type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error("clang++ required")
    root = Path(__file__).resolve().parents[1]
    os.chdir(root)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = ["Drivers/NativeGlk/InterruptDecode.cpp", "Mellow/GlkInterruptIOKit.cpp"]
    names = inputs + ["Drivers/NativeGlk/InterruptDecode.hpp", "Mellow/GlkInterruptIOKit.hpp",
                      "Drivers/NativeGlk/interrupt-provenance.json", "tests/native_glk_interrupt_tests.cpp",
                      "Tools/run-glk-interrupt-tests.py"]
    names += [str(f) for f in sorted(Path("tests/native_glk_interrupt_shim").rglob("*")) if f.is_file()]
    before = {name: sha(Path(name)) for name in names}
    dependencies = {}
    report = {"schema": "mellowgpu.glk-interrupt-tests/1", "status": "FAIL", "source_sha256": before,
              "scope": "actual decoder/native wiring under hosted register/OS boundaries; SDK object compile only",
              "hardware_executed": False, "native_driver_executed": False, "mmio_executed": False,
              "irq_delivered_on_hardware": False, "gpu_completion_proven": False, "gpu_executed": False,
              "metal_qualified": False, "windowserver_qualified": False, "native_linked": False,
              "sanitizers": ["address", "undefined"] if a.sanitize else [],
              "leak_sanitizer": "disabled on this macOS runtime; fixture asserts zero live OS objects"}
    try:
        report["compiler"] = call([a.cxx, "--version"])
        report["compiler_sha256"] = sha(Path(a.cxx))
        host = [a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-Itests/native_glk_interrupt_shim", "-I."]
        units = ["tests/native_glk_interrupt_tests.cpp", *inputs]
        for f in units:
            dependencies.update(deps([*host, "-MM", "-MT", "irq", f]))
        binary = out / "native-glk-interrupt-tests"
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"] if a.sanitize else []
        report["build"] = call([*host, *flags, *units, "-o", str(binary)])
        if report["build"]["exit_code"] != 0:
            raise RuntimeError("actual hosted native-body compile failed")
        if a.sanitize:
            report["sanitizer_imports"] = call(["xcrun", "nm", "-u", str(binary)])
            text = report["sanitizer_imports"].get("stdout", "")
            if "asan_init" not in text or "ubsan_handle_" not in text:
                raise RuntimeError("actual sanitizer instrumentation missing")
        report["test"] = call([str(binary)], timeout=30)
        if report["test"]["exit_code"] != 0:
            raise RuntimeError("actual hosted native-body tests failed")
        match = re.search(r"([0-9]+) checks, 0 failures", report["test"].get("stdout", ""))
        if not match:
            raise RuntimeError("actual test count missing")
        report["checks"] = int(match.group(1))
        report["binary_sha256"] = sha(binary)
        if a.sdk:
            sdk = a.sdk.resolve()
            if json.loads((sdk / "SDKSettings.json").read_text()).get("Version") != "15.5":
                raise ValueError("native object compilation requires exact SDK15.5")
            resource = call([a.cxx, "-print-resource-dir"])
            if resource["exit_code"] != 0:
                raise RuntimeError("clang resource discovery failed")
            native = [a.cxx, "-target", "x86_64-apple-macos15.5", "-std=c++17", "-mkernel", "-fapple-kext",
                      "-nostdinc", "-isystem", str(Path(resource["stdout"].strip()) / "include"),
                      "-isystem", str(sdk / "System/Library/Frameworks/Kernel.framework/Headers"), "-isysroot", str(sdk),
                      "-DKERNEL", "-DKERNEL_PRIVATE", "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-common",
                      "-fno-asynchronous-unwind-tables", "-fvisibility=hidden", "-fno-use-cxa-atexit", "-fno-threadsafe-statics",
                      "-Wall", "-Wextra", "-Werror", "-Wno-unknown-warning-option", "-Wno-ossharedptr-misuse",
                      "-Wno-deprecated-declarations"]
            report["native"] = []
            report["native_warning_exception"] = "SDK IOPCIDevice deprecation only"
            for f in inputs:
                dependencies.update(deps([*native, "-M", "-MT", "irq", f]))
                obj = out / (Path(f).stem + ".sdk155.o")
                result = call([*native, "-c", f, "-o", str(obj)])
                report["native"].append(result)
                if result["exit_code"] != 0:
                    raise RuntimeError("native SDK object compile failed")
                result["object_sha256"] = sha(obj)
                result["imports"] = call(["xcrun", "nm", "-u", str(obj)])
            report["sdk_settings_sha256"] = sha(sdk / "SDKSettings.json")
        report["status"] = "PASS_HOSTED_AND_NATIVE_OBJECTS" if a.sdk else "PASS_HOSTED_ONLY"
    except (OSError, ValueError, RuntimeError, IndexError) as error:
        report["error"] = str(error)
    finally:
        report["dependency_sha256"] = dependencies
        report["source_unchanged"] = all(Path(n).is_file() and sha(Path(n)) == value for n, value in before.items())
        report["dependencies_unchanged"] = all(Path(n).is_file() and sha(Path(n)) == value for n, value in dependencies.items())
        if not report["source_unchanged"] or not report["dependencies_unchanged"]:
            report["status"] = "FAIL_INPUT_CHANGED"
        (out / "glk-interrupt-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report.get(k) for k in ("status", "checks", "source_unchanged", "dependencies_unchanged", "error")}))
    return 0 if report["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run the GLK physical owner against injected OS boundaries and the SDK.

Hosted: the unchanged owner, the seeded GLK DMA adapter and MemoryOwner are
built with fatal ASan/UBSan against tests/native_glk_owner_shim. Mutants are
compiled from temporary copies and must be killed. Native: optional x86_64
kernel object compile against the exact macOS 15.5 SDK. No driver load,
hardware, PCI configuration, power, DMA, MMIO or setting change happens.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, *, environment=None, timeout=120):
    environment = dict(os.environ if environment is None else environment)
    # Clang's driver override can silently remove explicit sanitizer flags.
    environment.pop("CCC_OVERRIDE_OPTIONS", None)
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                env=environment, check=False, timeout=timeout)
        return {"argv": list(map(str, command)), "exit_code": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired as error:
        return {"argv": list(map(str, command)), "exit_code": None, "timeout": timeout,
                "stdout": str(error.stdout or ""), "stderr": str(error.stderr or "")}


def dependencies(command):
    result = run(command)
    if result["exit_code"] != 0:
        raise RuntimeError("dependency discovery failed: " + result["stderr"])
    text = result["stdout"].replace("\\\n", " ")
    files = sorted({Path(value).resolve() for value in shlex.split(text.split(":", 1)[1])})
    if not files or any(not path.is_file() for path in files):
        raise ValueError("invalid compiler dependencies")
    return {str(path): digest(path) for path in files}


OWNER = "Mellow/GlkPhysicalOwnerIOKit.cpp"
# Each mutant replaces exactly one occurrence in a temporary copy.
MUTANTS = {
    "recursive-quiesce-reentered": ("if (!revoking_ || finishingRevocation_) return;", "if (!revoking_) return;"),
    "active-action-not-drained": ("if (holdCount_ || activeActions_) {", "if (holdCount_) {"),
    "callback-anchor-not-retained": ("    ops.lifetime->retain();", "    (void)ops.lifetime;"),
    "vm-binding-anchor-not-retained": ("    vm.lifetime->retain(); // binding", "    (void)vm.lifetime; // binding"),
    "termination-does-not-supersede-reset": ("!(revocation_ & (GlkRevokePower | GlkRevokeTerminate));", "true;"),
    "config-before-open": ("    if (!device->open(this)) {", "    if (false) {"),
    "kernel-version-unchecked": (" || version_major != GlkKernelMajor) return false;", ") return false;"),
    "function-unchecked": (" ||\n        device_->getFunctionNumber() != 0) return false;", ") return false;"),
    "revision-unchecked": ("(classRevision & 0xff) == p.revision && ", ""),
    "publish-ignores-pmcsr": ("    if (pmcsr == UINT16_MAX || (pmcsr & PciPmStateMask)) return false;", "    (void)pmcsr;"),
    "epoch-reused": ("    if (!++epochCounter_) ++epochCounter_;\n    liveEpoch_ = epochCounter_;",
                     "    liveEpoch_ = epochCounter_ ? epochCounter_ : ++epochCounter_;"),
    "power-off-keeps-epoch": ("        powered_ = false;\n        beginRevocation(GlkRevokePower);", "        powered_ = false;"),
    "ack-implied-while-draining": ("            powerAckPending_ = true;\n            call.power = PowerAckBoundUs;",
                                   "            call.power = kIOPMAckImplied;"),
    "drain-timeout-not-quarantined": ("    latchQuarantine(GlkQuarantine::DrainTimeout);\n    revoking_ = false;", "    revoking_ = false;"),
    "quiesce-forward-order": ("    for (uint32_t i = clientCount_; i-- > 0;)\n        if (!clients_[i].ops.quiesce",
                              "    for (uint32_t i = 0; i < clientCount_; ++i)\n        if (!clients_[i].ops.quiesce"),
    "close-before-drain": ("    if (!closeRequested_ || revoking_ || quarantine_ != GlkQuarantine::None || !providerOpen_ ||\n"
                           "        liveEpoch_ || holdCount_ || activeActions_ || dmaInUse()) return;",
                           "    if (!closeRequested_ || quarantine_ != GlkQuarantine::None || !providerOpen_ ||\n"
                           "        liveEpoch_ || dmaInUse()) return;"),
    "firmware-bus-lead-cleared": ("    if (busLeadByOwner_) {\n        // Restore only", "    if (true) {\n        // Restore only"),
    "reset-without-producer": ("        if (!(registeredProducers() & GlkProducerGpuReset)) {", "        if (false) {"),
    "stop-without-drain-wait": ("            while (revoking_ && gate_->commandSleep(&revoking_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}\n"
                                "            if (revoking_) expireDrain(call);", "            if (revoking_) expireDrain(call);"),
    "admission-without-gate": ("    return workLoop_ && workLoop_->inGate() && device && device == device_ && service == this &&",
                               "    return workLoop_ && device && device == device_ && service == this &&"),
    "vm-producer-unregistered": ("    status = registerClient(epoch, GlkProducerGpuVm, {slot, vmClientQuiesce, nullptr, vm.lifetime});",
                                 "    status = Status::Ok;"),
}
EXPECTED_IMPORTS = ["PMinit", "PMstop", "joinPMtree", "registerPowerDriver", "changePowerStateTo",
                    "acknowledgeSetPowerState", "commandGate", "timerEventSource", "workLoop",
                    "setBusLeadEnable", "_version_major", "clock_interval_to_deadline", "attach"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--sdk", type=Path, help="Exact macOS 15.5 SDK for the native kernel object compile")
    parser.add_argument("--seed-hashes", type=Path, help="Seeded input snapshot hashes that must be unchanged")
    parser.add_argument("--skip-mutants", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("clang++ is required")
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    os.chdir(root)
    translated = run(["sysctl", "-n", "sysctl.proc_translated"])
    report = {"schema": "mellowgpu.glk-physical-owner-tests/1", "status": "FAIL",
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "actual owner + seeded DMA adapter on hosted injected IOKit boundary; optional SDK15.5 x86_64 kernel object compile",
              "host": {"machine": platform.machine(), "proc_translated": translated["stdout"].strip()},
              "hardware_executed": False, "native_driver_loaded": False, "pci_config_accessed": False,
              "power_transition_executed": False, "dma_executed": False, "gpu_executed": False,
              "metal_qualified": False, "windowserver_qualified": False,
              "sanitizers": ["address", "undefined"],
              "leak_sanitizer": "disabled: unsupported on this macOS runtime; host objects/allocations checked by fixture",
              "native": None, "mutants": None}
    sources = [OWNER, "Mellow/GlkPhysicalOwnerIOKit.hpp", "Mellow/GlkDmaMemoryIOKit.cpp",
               "Mellow/GlkDmaMemoryIOKit.hpp", "Drivers/NativeGpu/MemoryOwner.cpp", "Drivers/NativeGpu/MemoryOwner.hpp",
               "tests/native_glk_owner_iokit_tests.cpp", "tests/native_glk_owner_iokit_shim.hpp",
               "tests/native_glk_owner_iokit_provenance.json", "Tools/run-native-glk-owner-tests.py"]
    sources += [str(p.relative_to(root)) for p in sorted((root / "tests/native_glk_owner_shim").rglob("*.h"))]
    before = {name: digest(root / name) for name in sources}
    all_dependencies = {}
    try:
        if args.seed_hashes:
            seeds = json.loads(args.seed_hashes.read_text())
            report["seed_inputs"] = {name: {"expected": value, "actual": digest(root / name) if (root / name).is_file() else None}
                                     for name, value in seeds.items()}
            report["seed_inputs_unchanged"] = all(v["expected"] == v["actual"] for v in report["seed_inputs"].values())
            if not report["seed_inputs_unchanged"]:
                raise RuntimeError("seeded input snapshot changed")
        report["compiler"] = run([args.cxx, "--version"])
        base = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DMELLOW_GLK_OWNER_HOSTED",
                "-DMELLOW_GLK_DMA_HOSTED_FENCE", "-Itests/native_glk_owner_shim", "-I."]
        inputs = ["tests/native_glk_owner_iokit_tests.cpp", OWNER, "Mellow/GlkDmaMemoryIOKit.cpp",
                  "Drivers/NativeGpu/MemoryOwner.cpp"]
        for source in inputs:
            all_dependencies.update(dependencies([*base, "-MM", "-MT", "glk", source]))
        binary = output / "native-glk-owner-tests"
        sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
        report["build"] = run([*base, *sanitize, *inputs, "-o", str(binary)])
        if report["build"]["exit_code"] != 0:
            raise RuntimeError("hosted owner compile failed")
        symbols = run(["xcrun", "nm", "-u", str(binary)])
        report["sanitizer_imports_present"] = {"asan_init": "asan_init" in symbols["stdout"],
                                               "ubsan_handle": "ubsan_handle_" in symbols["stdout"]}
        if symbols["exit_code"] != 0 or not all(report["sanitizer_imports_present"].values()):
            raise RuntimeError("actual sanitizer instrumentation is missing")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=0"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        report["test"] = run([str(binary)], environment=environment, timeout=120)
        if report["test"]["exit_code"] != 0:
            raise RuntimeError("hosted owner test failed")
        match = re.search(r"glk physical owner boundary: ([0-9]+) checks;", report["test"]["stdout"])
        if not match:
            raise RuntimeError("missing actual test count")
        report["checks"] = int(match.group(1))
        report["binary_sha256"] = digest(binary)
        if not args.skip_mutants:
            text = (root / OWNER).read_text()
            objects = output / "mutant-objects"
            objects.mkdir()
            shared = []
            for source in inputs:
                if source == OWNER:
                    continue
                obj = objects / (Path(source).stem + ".o")
                result = run([*base, "-c", source, "-o", str(obj)])
                if result["exit_code"] != 0:
                    raise RuntimeError("mutant base compile failed: " + result["stderr"])
                shared.append(str(obj))
            results = {}
            for name, (old, new) in MUTANTS.items():
                if text.count(old) != 1:
                    raise RuntimeError("mutant anchor is not unique: " + name)
                folder = output / "mutants" / name
                folder.mkdir(parents=True)
                mutated = folder / "GlkPhysicalOwnerIOKit.cpp"
                mutated.write_text(text.replace(old, new))
                exe = folder / "tests"
                build = run([*base, "-IMellow", str(mutated), *shared, "-o", str(exe)])
                outcome = {"source_sha256": digest(mutated), "build_exit": build["exit_code"]}
                if build["exit_code"] == 0:
                    test = run([str(exe)], timeout=60)
                    outcome["test_exit"] = test["exit_code"]
                    outcome["failure"] = (test["stderr"].strip().splitlines() or [""])[-1][:300]
                    outcome["killed"] = test["exit_code"] != 0
                else:
                    # A compile failure proves nothing about the tests.
                    outcome["killed"] = False
                    outcome["failure"] = "mutant did not compile: " + build["stderr"][-300:]
                results[name] = outcome
            report["mutants"] = results
            report["mutants_killed"] = sum(1 for v in results.values() if v["killed"])
            if report["mutants_killed"] != len(results):
                raise RuntimeError("surviving mutant: " + ", ".join(k for k, v in results.items() if not v["killed"]))
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
            native_dependencies = dependencies([*flags, "-M", "-MT", "glk", OWNER])
            all_dependencies.update(native_dependencies)
            obj = output / "GlkPhysicalOwnerIOKit.sdk155.o"
            native = run([*flags, "-c", OWNER, "-o", str(obj)])
            native["sdk_settings_sha256"] = digest(sdk / "SDKSettings.json")
            native["scope"] = "x86_64 native kernel object compile only; not linked, loaded or runtime-admitted"
            native["warning_exception"] = "public SDK marks IOPCIDevice APIs deprecated in favor of PCIDriverKit"
            native["sdk_headers"] = {k: v for k, v in native_dependencies.items() if "/Kernel.framework/" in k}
            report["native"] = native
            if native["exit_code"] != 0:
                raise RuntimeError("native SDK kernel object compile failed")
            native["object_sha256"] = digest(obj)
            imports = run(["xcrun", "nm", "-u", str(obj)])
            native["undefined_symbols"] = imports["stdout"].split()
            native["expected_imports_present"] = {name: name in imports["stdout"] for name in EXPECTED_IMPORTS}
            if imports["exit_code"] != 0 or not all(native["expected_imports_present"].values()):
                raise RuntimeError("native object import inspection failed")
        report["status"] = "PASS_HOSTED_MUTANTS_AND_NATIVE_OBJECT" if args.sdk and not args.skip_mutants else \
            "PASS_HOSTED_AND_NATIVE_OBJECT" if args.sdk else "PASS_HOSTED_ONLY"
    except (OSError, ValueError, RuntimeError, IndexError, KeyError) as error:
        report["error"] = str(error)
    finally:
        report["source_sha256"] = before
        report["dependency_sha256"] = all_dependencies
        report["source_unchanged"] = all((root / n).is_file() and digest(root / n) == v for n, v in before.items())
        report["dependencies_unchanged"] = all(Path(p).is_file() and digest(Path(p)) == v for p, v in all_dependencies.items())
        if not report["source_unchanged"] or not report["dependencies_unchanged"]:
            report["status"] = "FAIL_INPUT_CHANGED"
        (output / "native-glk-owner-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report.get(k) for k in ("status", "checks", "mutants_killed", "seed_inputs_unchanged",
                                                  "source_unchanged", "dependencies_unchanged", "error")}))
    return 0 if report["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regress the real MellowGlkPassiveProbe source against injected OS boundaries.

Builds and runs the hosted tests (optionally under fatal ASan/UBSan), kills
source mutants, compiles native x86_64 kernel objects against SDK15.5 and the
repository MacKernelSDK, audits imports and direct call targets, and checks
the personality fragment with IOPCIFamily matching rules. No driver load,
registry, PCI, IOMMU, DMA, MMIO, GPU or setting access happens here.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import subprocess

PRODUCTION = "Mellow/GlkPassiveProbeIOKit.cpp"
HEADER = "Mellow/GlkPassiveProbeIOKit.hpp"
PERSONALITY = "Mellow/GlkPassiveProbeIOKit.personality.plist"
TEST = "tests/native_glk_passive_probe_tests.cpp"
SHIM = "tests/native_glk_passive_probe_shim.hpp"
SHIM_DIR = "tests/native_glk_passive_probe_shim"
PROVENANCE = "tests/native_glk_passive_probe_provenance.json"
RUNNER = "Tools/run-glk-passive-probe-tests.py"
SCHEMA = "mellowgpu.glk-passive-probe-tests/1"
# Each mutant must make the unchanged hosted tests fail.
MUTANTS = [
    ("registry check skipped", "if (!out.registryComplete || !isTarget(out.registry)) return GlkProbeVerdict::RegistryMismatch;", ""),
    ("header type ignored", "if (!isTarget(out.config) || (out.headerType & 0x7f))", "if (!isTarget(out.config))"),
    ("revision ignored", "id.revision == GlkPassiveTarget::Revision &&", ""),
    ("subsystem ignored", "id.subsystem == GlkPassiveTarget::Subsystem;", "true;"),
    ("function ignored", "out.function != GlkPassiveTarget::Function", "false"),
    ("kernel ignored", "if (out.darwinMajor != GlkPassiveTarget::DarwinMajor) return GlkProbeVerdict::WrongKernel;", ""),
    ("HasMapper as Unknown", "if (word == 2) return GlkSystemMapperState::HasMapper;", "if (word == 2) return GlkSystemMapperState::Unknown;"),
    ("wait bits ignored", "if (word & 3) return GlkSystemMapperState::Unrecognized;", ""),
    ("loop detection removed", "if (seen & bit) return GlkPmCapability::Malformed;", ""),
    ("PM ceiling removed", "if (position > PmCeiling) return GlkPmCapability::Malformed;", ""),
    ("all-ones accepted", "if (id == 0xffffffffU || (id & 0xffff) == 0xffff) return GlkProbeVerdict::ConfigUnreadable;", ""),
    ("self open ignored", "if (out.providerOpenBySelf) return GlkProbeVerdict::OpenedBySelf;", ""),
    ("identity drift accepted", "verdict = GlkProbeVerdict::Unstable;", "verdict = GlkProbeVerdict::Target;"),
    ("wide option published", "if (narrow) number(", "if (true) number("),
    ("publish failure ignored", "ok = ok && setProperty(ObservationKey, d);", "setProperty(ObservationKey, d);"),
    ("stop keeps observation", "if (published_) removeProperty(ObservationKey);", ""),
]
# Direct (non-virtual) call/jump targets the native object may contain.
ALLOWED_CALLS = {
    "_IOLog", "_memcpy", "_memset",
    "__ZN11IOPCIDevice20extendedConfigRead16Ey", "__ZN11IOPCIDevice20extendedConfigRead32Ey",
    "__ZN12OSDictionary12withCapacityEj", "__ZN8OSNumber10withNumberEyj", "__ZN8OSString11withCStringEPKc",
    "__ZN15IORegistryEntry18getRegistryEntryIDEv",
    "__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass",
    "__ZN9IOService5probeEPS_Pi", "__ZN9IOService5startEPS_", "__ZN9IOService4stopEPS_", "__ZN9IOService4freeEv",
    "__ZN11OSMetaClassC2EPKcPKS_j", "__ZN11OSMetaClassD2Ev", "__ZNK11OSMetaClass19instanceConstructedEv",
    "__ZN9IOServiceC2EPK11OSMetaClass", "__ZN9IOServiceD2Ev", "__ZN8OSObjectnwEm", "__ZN8OSObjectdlEPvm",
    "__ZN11OSMetaClassdlEPvm", "__ZNK9IOService10isInactiveEv", "__ZNK15OSMetaClassBase8metaCastEPKc",
}
# Vtable padding slots of this class's own ancestry. Not in the pinned XNU
# export lists; resolution by kext vtable patching is a target-link gap.
PADDING = re.compile(r"^__ZN\d+(OSMetaClassBase|OSMetaClass|OSObject|IORegistryEntry|IOService)\d+_RESERVED\1\d+Ev$")
FORBIDDEN_IMPORT = re.compile(r"configWrite|extendedConfigWrite|copyMapperForDevice|waitForSystemMapper|"
                              r"checkForSystemMapper|setBusMasterEnable|setBusLeadEnable|setMemoryEnable|"
                              r"setIOEnable|setPowerState.*IOPCIDevice|IOPCIDevice.*mapDeviceMemory|hasPCIPowerManagement|"
                              r"enablePCIPowerManagement|IOPCIDevice.*setConfigHandler")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, *, environment=None, timeout=120, cwd=None):
    environment = dict(os.environ if environment is None else environment)
    # Clang's driver override can silently remove explicit sanitizer flags.
    environment.pop("CCC_OVERRIDE_OPTIONS", None)
    try:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                                env=environment, check=False, timeout=timeout, cwd=cwd)
        return {"argv": list(map(str, command)), "exit_code": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired as error:
        return {"argv": list(map(str, command)), "exit_code": None, "timeout": timeout,
                "stdout": str(error.stdout or ""), "stderr": str(error.stderr or "")}


def dependencies(command):
    result = run(command)
    if result["exit_code"] != 0:
        raise RuntimeError("dependency discovery failed: " + result["stderr"][-2000:])
    text = result["stdout"].replace("\\\n", " ")
    files = sorted({Path(value).resolve() for value in shlex.split(text.split(":", 1)[1])})
    if not files or any(not path.is_file() for path in files):
        raise ValueError("invalid compiler dependencies")
    return {str(path): digest(path) for path in files}


def sanitized_environment():
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=0"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return environment


def iopci_match(table, config):
    """IOPCIFamily-681.140.3 IOPCIBridge::pciMatchNub/matchKeys over saved config words."""
    keys = [("IOPCIMatch", (0x00, 0x2c), 0xffffffff), ("IOPCIPrimaryMatch", (0x00,), 0xffffffff),
            ("IOPCISecondaryMatch", (0x2c,), 0xffffffff), ("IOPCIClassMatch", (0x08,), 0xffffff00)]
    for name, registers, default in keys:
        text = table.get(name)
        if text is None:
            continue
        found = False
        for register in registers:
            for item in re.finditer(r"0x([0-9a-fA-F]+)\s*(?:&\s*0x([0-9a-fA-F]+))?", text):
                value, mask = int(item.group(1), 16), int(item.group(2), 16) if item.group(2) else default
                if (value & mask) == (config[register] & mask):
                    found = True
            if found:
                break
        if not found:
            return False
    return True


def check_personality(root):
    data = plistlib.loads((root / PERSONALITY).read_bytes())
    if list(data) != ["MellowGlkPassiveProbe"]:
        raise ValueError("personality fragment must contain exactly MellowGlkPassiveProbe")
    table = data["MellowGlkPassiveProbe"]
    required = {"CFBundleIdentifier": "com.NiSeullent.MellowGPU", "IOClass": "MellowGlkPassiveProbe",
                "IOProviderClass": "IOPCIDevice", "IOMatchCategory": "MellowGlkPassiveProbe",
                "IOPCIPrimaryMatch": "0x31858086", "IOPCISecondaryMatch": "0x3e7717aa",
                "IOPCIClassMatch": "0x03000006&0xffffffff", "MellowGPUQualification": "unqualified"}
    for key, value in required.items():
        if table.get(key) != value:
            raise ValueError(f"personality {key} must be {value!r}")
    for forbidden in ("IOUserClientClass", "IOPCIMatch", "IONameMatch", "IOPropertyMatch", "IOResourceMatch"):
        if forbidden in table:
            raise ValueError(f"personality must not contain {forbidden}")
    target = {0x00: 0x31858086, 0x08: 0x03000006, 0x2c: 0x3e7717aa}
    cases = {"target": (target, True)}
    for name, register, value in [("device 3184", 0x00, 0x31848086), ("revision 03", 0x08, 0x03000003),
                                  ("class 0380", 0x08, 0x03800006), ("prog-if 01", 0x08, 0x03000106),
                                  ("subsystem vendor", 0x2c, 0x3e7717ab), ("subsystem", 0x2c, 0x3e7817aa),
                                  ("vendor", 0x00, 0x31851002)]:
        cases[name] = ({**target, register: value}, False)
    results = {name: iopci_match(table, config) for name, (config, _) in cases.items()}
    if any(results[name] != expected for name, (_, expected) in cases.items()):
        raise ValueError("personality matching model mismatch: " + json.dumps(results))
    return {"sha256": digest(root / PERSONALITY), "matching_model": results,
            "framebuffer_category_distinct": table["IOMatchCategory"] != "IOFramebuffer"}


def call_targets(obj):
    listing = run(["xcrun", "otool", "-tV", obj])
    if listing["exit_code"] != 0:
        raise RuntimeError("otool disassembly failed")
    targets = set()
    for line in listing["stdout"].splitlines():
        match = re.search(r"\s(?:callq|call|jmpq|jmp)\s+(_\S+)$", line)
        if match:
            targets.add(match.group(1))
    return sorted(targets)


def export_sets(directory):
    names = ["IOKit.exports", "IOKit.x86_64.exports", "Libkern.exports", "Libkern.x86_64.exports"]
    exports, hashes = {}, {}
    for name in names:
        path = directory / name
        hashes[name] = digest(path)
        for line in path.read_text().splitlines():
            # "exported:implementation" lines export the name before the colon.
            symbol = line.strip().split(":", 1)[0]
            if symbol and not symbol.startswith("#"):
                exports.setdefault(symbol, name)
    return exports, hashes


def native_compile(cxx, root, output, name, target_flags, exports):
    resource = run([cxx, "-print-resource-dir"])
    if resource["exit_code"] != 0:
        raise ValueError("compiler resource discovery failed")
    flags = [cxx, *target_flags, "-std=c++17", "-mkernel", "-fapple-kext", "-nostdinc",
             "-isystem", str(Path(resource["stdout"].strip()) / "include"), "-DKERNEL", "-DKERNEL_PRIVATE",
             "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables",
             "-fvisibility=hidden", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-Wall", "-Wextra",
             "-Werror", "-Wno-unknown-warning-option", "-Wno-ossharedptr-misuse", "-Wno-deprecated-declarations"]
    native_dependencies = dependencies([*flags, "-M", "-MT", "glk", PRODUCTION])
    obj = output / f"GlkPassiveProbeIOKit.{name}.o"
    native = run([*flags, "-c", PRODUCTION, "-o", obj])
    native["scope"] = "x86_64 native kernel object compile only; not linked, signed, loaded or runtime admitted"
    if native["exit_code"] != 0:
        raise RuntimeError(f"native {name} kernel object compile failed")
    native["object_sha256"] = digest(obj)
    imports = run(["xcrun", "nm", "-u", obj])
    if imports["exit_code"] != 0:
        raise RuntimeError("native object import inspection failed")
    symbols = sorted(set(imports["stdout"].split()))
    forbidden = [symbol for symbol in symbols if FORBIDDEN_IMPORT.search(symbol)]
    rest = [s for s in symbols if s not in exports]
    native["imports"] = {"count": len(symbols), "forbidden": forbidden,
                         "xnu_declared_kpi": {s: exports[s] for s in symbols if s in exports},
                         "iopcifamily_owned": [s for s in rest if s.startswith("__ZN11IOPCIDevice")],
                         "vtable_padding_unverified": [s for s in rest if PADDING.match(s)],
                         "unresolved": [s for s in rest if not s.startswith("__ZN11IOPCIDevice") and not PADDING.match(s)]}
    # Only imported targets: helpers, lambdas and inline SDK wrappers are local.
    calls = [call for call in call_targets(obj) if call in symbols]
    native["imported_direct_calls"] = calls
    native["unexpected_direct_calls"] = [call for call in calls if call not in ALLOWED_CALLS]
    problems = []
    if forbidden or native["imports"]["unresolved"] or native["unexpected_direct_calls"]:
        problems.append(f"native {name} import/call audit failed")
    for required in ("__ZN8IOMapper7gSystemE", "_version_major", "__ZN11IOPCIDevice20extendedConfigRead32Ey"):
        if required not in symbols:
            problems.append(f"native {name} object does not import {required}")
    return native, native_dependencies, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--mutants", action="store_true")
    parser.add_argument("--sdk", type=Path, help="Exact macOS15.5 SDK for a native kernel object compile")
    parser.add_argument("--mackernelsdk", action="store_true", help="Also compile with repository MacKernelSDK")
    parser.add_argument("--exports", type=Path, help="Pinned XNU config/ directory with *.exports files")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("clang++ is required")
    if (args.sdk or args.mackernelsdk) and not args.exports:
        parser.error("native compiles require --exports for the import audit")
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    os.chdir(root)
    report = {"schema": SCHEMA, "status": "FAIL",
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "actual probe source against hosted OS boundaries; optional native object compiles",
              "hardware_executed": False, "native_driver_loaded": False, "pci_config_accessed": False,
              "iommu_accessed": False, "gpu_executed": False, "metal_qualified": False,
              "windowserver_qualified": False, "qualified_profiles": 0,
              "sanitizers": ["address", "undefined"] if args.sanitize else [],
              "leak_sanitizer": "disabled: unsupported on this macOS runtime; host objects checked by fixture",
              "native": {}}
    names = [PRODUCTION, HEADER, PERSONALITY, TEST, SHIM, PROVENANCE, RUNNER]
    names += [str(path.relative_to(root)) for path in sorted((root / SHIM_DIR).rglob("*.h"))]
    before = {name: digest(root / name) for name in names}
    all_dependencies = {}
    try:
        report["compiler"] = run([args.cxx, "--version"])
        report["compiler_invocation_sha256"] = digest(Path(args.cxx))
        report["personality"] = check_personality(root)
        base = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-DMELLOW_GLK_PASSIVE_HOSTED", f"-I{SHIM_DIR}", "-I."]
        sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer"] if args.sanitize else []
        for source in (TEST, PRODUCTION):
            all_dependencies.update(dependencies([*base, "-MM", "-MT", "glk", source]))
        binary = output / "native-glk-passive-probe-tests"
        report["build"] = run([*base, *sanitize, TEST, PRODUCTION, "-o", binary], timeout=300)
        if report["build"]["exit_code"] != 0:
            raise RuntimeError("hosted probe compile failed")
        if args.sanitize:
            imports = run(["xcrun", "nm", "-u", binary])
            report["sanitizer_imports"] = {"exit_code": imports["exit_code"],
                "asan_init": "asan_init" in imports["stdout"],
                "ubsan_handlers": sorted(set(re.findall(r"___ubsan_handle_\w+", imports["stdout"])))}
            if imports["exit_code"] != 0 or not report["sanitizer_imports"]["asan_init"] or \
                    not report["sanitizer_imports"]["ubsan_handlers"]:
                raise RuntimeError("actual sanitizer instrumentation is missing")
            canary = output / "sanitizer-canary.cpp"
            canary.write_text("#include <climits>\nint main(int argc, char **) { int v = INT_MAX - 1 + argc; "
                              "return v + 1 > 0 ? 0 : 3; }\n")
            canary_binary = output / "sanitizer-canary"
            built = run([args.cxx, "-std=c++17", *sanitize, canary, "-o", canary_binary])
            fired = run([canary_binary], environment=sanitized_environment(), timeout=30)
            report["sanitizer_canary"] = {"build_exit": built["exit_code"], "exit_code": fired["exit_code"],
                                          "ubsan_reported": "signed integer overflow" in fired["stderr"]}
            if built["exit_code"] != 0 or fired["exit_code"] in (0, None) or \
                    not report["sanitizer_canary"]["ubsan_reported"]:
                raise RuntimeError("sanitizer failure is not fatal")
        report["test"] = run([binary], environment=sanitized_environment(), timeout=120)
        if report["test"]["exit_code"] != 0:
            raise RuntimeError("actual hosted probe test failed")
        match = re.search(r"boundary: ([0-9]+) checks;", report["test"]["stdout"])
        if not match:
            raise RuntimeError("missing actual test count")
        report["checks"] = int(match.group(1))
        report["binary_sha256"] = digest(binary)
        if args.mutants:
            text = (root / PRODUCTION).read_text()
            report["mutants"] = []
            for index, (label, old, new) in enumerate(MUTANTS):
                if text.count(old) != 1:
                    raise RuntimeError(f"mutant anchor not unique: {label}")
                folder = output / f"mutant-{index:02d}"
                folder.mkdir()
                shutil.copy(root / HEADER, folder / "GlkPassiveProbeIOKit.hpp")
                (folder / "GlkPassiveProbeIOKit.cpp").write_text(text.replace(old, new))
                mutant = folder / "test"
                # Warnings stay visible but not fatal: a kill must be a test
                # failure, never an unused-variable compile error.
                lenient = [flag for flag in base if flag != "-Werror"]
                built = run([*lenient, TEST, folder / "GlkPassiveProbeIOKit.cpp", "-o", mutant], timeout=300)
                if built["exit_code"] != 0:
                    raise RuntimeError(f"mutant did not compile: {label}")
                outcome = run([mutant], timeout=60)
                killed = outcome["exit_code"] not in (0, None)
                report["mutants"].append({"label": label, "test_exit": outcome["exit_code"],
                    "failure": outcome["stderr"].strip().splitlines()[-1:] if killed else [],
                    "killed": killed})
            report["mutants_killed"] = sum(item["killed"] for item in report["mutants"])
            if report["mutants_killed"] != len(MUTANTS):
                raise RuntimeError("surviving source mutant")
        exports = None
        if args.exports:
            exports, report["export_lists_sha256"] = export_sets(args.exports.resolve())
        if args.sdk:
            sdk = args.sdk.resolve()
            settings = json.loads((sdk / "SDKSettings.json").read_text())
            if settings.get("Version") != "15.5":
                raise ValueError("native compile requires exact SDK15.5")
            native, found, problems = native_compile(args.cxx, root, output, "sdk155",
                ["-target", "x86_64-apple-macos15.5", "-isystem",
                 str(sdk / "System/Library/Frameworks/Kernel.framework/Headers"), "-isysroot", str(sdk)], exports)
            native["sdk_settings_sha256"] = digest(sdk / "SDKSettings.json")
            native["warning_exception"] = "public SDK marks IOPCIDevice deprecated in favor of PCIDriverKit"
            report["native"]["sdk155"] = native
            all_dependencies.update(found)
            if problems:
                raise RuntimeError("; ".join(problems))
        if args.mackernelsdk:
            native, found, problems = native_compile(args.cxx, root, output, "mackernelsdk",
                ["-target", "x86_64-apple-macos13", "-I", str(root / "MacKernelSDK/Headers"),
                 "-Wno-macro-redefined"], exports)
            native["warning_exception"] = ("MacKernelSDK TargetConditionals.h redefines clang builtin "
                                           "TARGET_OS_* macros; -Wno-macro-redefined only")
            report["native"]["mackernelsdk"] = native
            all_dependencies.update(found)
            if problems:
                raise RuntimeError("; ".join(problems))
        native_names = sorted(report["native"])
        report["status"] = "PASS_HOSTED_AND_NATIVE_OBJECTS[" + ",".join(native_names) + "]" \
            if native_names else "PASS_HOSTED_ONLY"
    except (OSError, ValueError, RuntimeError, IndexError, KeyError) as error:
        report["error"] = str(error)
    finally:
        report["source_sha256"] = before
        report["dependency_sha256"] = all_dependencies
        report["source_unchanged"] = all((root / n).is_file() and digest(root / n) == v for n, v in before.items())
        report["dependencies_unchanged"] = all(Path(p).is_file() and digest(p) == v for p, v in all_dependencies.items())
        if not report["source_unchanged"] or not report["dependencies_unchanged"]:
            report["status"] = "FAIL_INPUT_CHANGED"
        (output / "glk-passive-probe-tests.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report.get(key) for key in
                      ("status", "checks", "mutants_killed", "source_unchanged", "dependencies_unchanged", "error")}))
    return 0 if report["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())

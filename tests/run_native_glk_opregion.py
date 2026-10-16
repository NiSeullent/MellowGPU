#!/usr/bin/env python3 -I
"""Bounded runner for the GLK OpRegion snapshot unit.

Hosted: fatal ASan/UBSan regression against the injected shim (two repeat
runs), then source mutants that must each be killed. Kernel: genuine SDK 15.5
x86_64 kext-mode compile of the new units (plus the frozen owner for symbol
closure), undefined-symbol classification against the pinned XNU exports and
a forbidden-effect audit. Every child has a timeout. No device, GPU, MMIO,
DMA or target operation is performed.

usage: run_native_glk_opregion.py <worktree> <results-dir> <xnu-config-dir>
"""
import hashlib, json, os, re, shutil, subprocess, sys, time

W, R, XCONF = (os.path.abspath(a) for a in sys.argv[1:4])
TC = "/Applications/Xcode-26.6.0.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
CXX = TC + "/clang++"
HOST_SDK = "/Applications/Xcode-26.6.0.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk"
KSDK = "/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk"
KHDR = KSDK + "/System/Library/Frameworks/Kernel.framework/Headers"
SOURCES = {
    "snapshot": "Drivers/NativeGlk/OpRegionSnapshot.cpp",
    "adapter": "Mellow/GlkOpRegionIOKit.cpp",
}
HOSTED = ["tests/native_glk_opregion_tests.cpp", SOURCES["snapshot"], SOURCES["adapter"],
          "Mellow/GlkPhysicalOwnerIOKit.cpp", "Drivers/NativeGlk/PageTableEncoding.cpp"]
HOSTED_FLAGS = ["-isysroot", HOST_SDK, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DMELLOW_GLK_OWNER_HOSTED", "-DMELLOW_GLK_DMA_HOSTED_FENCE",
                "-DMELLOW_GLK_OPREGION_HOSTED_IO", "-I", W + "/tests/native_glk_opregion_shim"]
ENV = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1:"
           "strict_string_checks=1:check_initialization_order=1",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

# (name, file key, exact snippet, replacement): each mutant must be killed.
MUTANTS = [
    ("opregion-signature-unchecked", "snapshot",
     "if (!same(h, kOpRegionSignature,", "if (false && !same(h, kOpRegionSignature,"),
    ("declared-size-unchecked", "snapshot",
     "if (out.declaredSize < OpRegion::kMinDeclaredSize) return OpRegionStatus::HeaderSize;", ""),
    ("version-2.2-admitted", "snapshot", "out.minor > 1", "out.minor > 2"),
    ("rvda-2.0-treated-relative", "snapshot",
     "plan.rvdaRelative = plan.header.major > 2 || plan.header.minor >= 1;", "plan.rvdaRelative = true;"),
    ("rvda-2.1-treated-absolute", "snapshot",
     "plan.rvdaRelative = plan.header.major > 2 || plan.header.minor >= 1;", "plan.rvdaRelative = false;"),
    ("rvda-inside-opregion-admitted", "snapshot", "plan.rvda < OpRegion::kBytes || ", ""),
    ("rvds-unbounded", "snapshot",
     "if (plan.rvds > Vbt::kMaxRvdsBytes) return OpRegionStatus::RvdaRange;", ""),
    ("rvda-without-asle-mailbox", "snapshot",
     "if (!(mailboxes & OpRegion::kMboxAsle)) return OpRegionStatus::Ok;", ""),
    ("asle-ext-window-ignored", "snapshot",
     "(mailboxes & OpRegion::kMboxAsleExt) ? OpRegion::kAsleExtOffset : OpRegion::kBytes",
     "OpRegion::kBytes"),
    ("bdb-size-off-by-one", "snapshot",
     "if (le16(vbt + bdb + Vbt::kBdbSizeOffset) > size - bdb)",
     "if (le16(vbt + bdb + Vbt::kBdbSizeOffset) > size - bdb + 1)"),
    ("checksum-unchecked", "snapshot", "if (sum) return VbtStatus::Checksum;", ""),
    ("bdb-signature-unchecked", "snapshot",
     "if (!same(vbt + bdb, kBdbSignature,", "if (false && !same(vbt + bdb, kBdbSignature,"),
    ("ggms-decoded-from-gms-bits", "snapshot", "const uint32_t ggms = (ggc >> 6) & 0x3;",
     "const uint32_t ggms = (ggc >> 8) & 0x3;"),
    ("header-incomplete-published", "snapshot",
     "if (size < Vbt::kHeaderBytes) return VbtStatus::HeaderIncomplete;", ""),
    ("strict-refusal-redirects", "snapshot",
     "if (status == OpRegionStatus::Ok && !linuxRejects(s.rvdaVbt)) status = OpRegionStatus::VbtRefused;", ""),
    ("rvda-map-failure-falls-back", "snapshot",
     "            return OpRegionStatus::Unavailable;\n        mapped = true;",
     "            s.rvdaVbt = VbtStatus::Signature;\n        else mapped = true;"),
    ("fallback-keeps-rvda-mapped", "snapshot",
     "            mapped = false;\n            if (!p.unmapRvda(p.context)) status = OpRegionStatus::Uncertain;", ""),
    ("pass-two-vbt-not-compared", "snapshot",
     "        status = compareWindow(*selected, s.vbt, s.vbtBytes);",
     "        status = ((void)compareWindow, OpRegionStatus::Ok);"),
    ("pass-two-asls-not-observed", "snapshot",
     "else if (aslsAgain != asls) status = OpRegionStatus::Incoherent;", ""),
    ("head-body-race-ignored", "snapshot",
     "if (!same(copy, head, Vbt::kHeaderBytes)) {", "if (false) {"),
    ("rvda-unmap-failure-ignored", "snapshot",
     "if (mapped && !p.unmapRvda(p.context)) status = OpRegionStatus::Uncertain;",
     "if (mapped) (void)p.unmapRvda(p.context);"),
    # Not listed, equivalent under the frozen owner: "serialized binding accepts
    # a nonzero epoch" (runSerialized always delivers binding(0)).
    ("bdsm-base-unmasked", "adapter", "o.bdsmBase = o.bdsm & NG::OpRegion::kBdsmBaseMask;", "o.bdsmBase = o.bdsm;"),
    ("owner-instance-unchecked", "adapter", "if (owner_ && (owner_ != owner || registryId_ != id.device.registryId)) return false;", ""),
    ("live-identity-unchecked", "adapter", " && identityHolds(b.device);", ";"),
    ("mapper-not-excluded", "adapter", "kIODirectionOut | kIOMemoryMapperNone, TASK_NULL", "kIODirectionOut, TASK_NULL"),
    ("mapping-writable", "adapter", "kIOMapAnywhere | kIOMapReadOnly | kIOMapDefaultCache", "kIOMapAnywhere | kIOMapDefaultCache"),
    ("mapping-length-unverified", "adapter", "w.map->getLength() != bytes || ", ""),
    ("mapping-physical-unverified", "adapter", "w.map->getPhysicalAddress() != physical ||", ""),
    ("unmap-failure-releases", "adapter",
     "    if (w.map && w.map->unmap() != kIOReturnSuccess) {", "    if (w.map && w.map->unmap() != kIOReturnSuccess && false) {"),
    ("stale-epoch-served", "adapter",
     "if (self.info_.epoch != binding.identity.device.epoch) { call->status = Status::StaleEpoch; break; }", ""),
    ("partial-copy-out", "adapter",
     "if (call->capacity < self.vbtBytes_) { call->status = Status::Invalid; break; }",
     "if (call->capacity < self.vbtBytes_) self.vbtBytes_ = call->capacity;"),
    ("changed-vbt-accepted", "adapter", "        if (!equal) {", "        if (!equal && false) {"),
    ("final-observation-skipped", "adapter",
     "if (!observed || again.asls != first.asls || again.ggc != first.ggc || again.bdsm != first.bdsm) {",
     "if (!observed) {"),
    ("consumer-reentry-allowed", "adapter", "    if (self.inConsumer_) {", "    if (false) {"),
    ("quarantine-not-latched", "adapter",
     "        call->status = self.quarantined_ ? Status::Quarantined : self.captureGated(binding);",
     "        call->status = self.captureGated(binding);"),
]

FORBIDDEN = re.compile(r"configWrite|extendedConfigWrite|mapDeviceMemory|setMemoryEnable|setBusLeadEnable|"
                       r"setIOEnable|setBusMasterEnable|evaluateObject|evaluateInteger|withPhysicalAddress|"
                       r"IOLog|setProperty|copyout|_IOMapPages|_ml_phys|_pmap_")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def run(cmd, timeout, log, env=None, cwd=None):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                           env=env, cwd=cwd or R)
        rc, out = p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        rc, out = "timeout", (e.stdout or b"").decode("utf-8", "replace")
    open(log, "w").write(out)
    return {"rc": rc, "seconds": round(time.time() - t0, 2), "log": os.path.relpath(log, R),
            "tail": out.strip().splitlines()[-3:]}


def hosted_build(out, overrides=None, log=None):
    # The green build compiles every unit; a mutant recompiles only its
    # mutated copy (quoted includes resolve to the worktree) and relinks.
    os.makedirs(R + "/bin/obj", exist_ok=True)
    objs = []
    for rel in HOSTED:
        name = os.path.basename(rel).replace(".cpp", ".o")
        if overrides and rel in overrides:
            obj = os.path.dirname(out) + "/" + name
            c = run([CXX] + HOSTED_FLAGS + ["-iquote", W + "/Drivers/NativeGlk", "-iquote", W + "/Mellow",
                                            "-c", overrides[rel], "-o", obj], 300, obj + ".log")
        else:
            obj = R + "/bin/obj/" + name
            c = {"rc": 0} if overrides else run([CXX] + HOSTED_FLAGS + ["-c", W + "/" + rel, "-o", obj],
                                                300, obj + ".log")
        if c["rc"] != 0:
            return c
        objs.append(obj)
    return run([CXX] + HOSTED_FLAGS + objs + ["-o", out], 300, log)


def main():
    for d in ("bin", "logs", "kernel", "mutants"):
        shutil.rmtree(R + "/" + d, ignore_errors=True)
        os.makedirs(R + "/" + d)
    res = {"schema": "mellowgpu.glk-opregion-native-run/1", "worktree": W,
           "classification": "HOSTED INJECTED MODEL + NATIVE COMPILE ONLY; no hardware evidence",
           "tools": {"clang": subprocess.run([CXX, "--version"], stdout=subprocess.PIPE).stdout.decode().splitlines()[0],
                     "kernel_sdk": KSDK, "host_sdk": HOST_SDK},
           "sources": {rel: sha(W + "/" + rel) for rel in HOSTED + [
               "Drivers/NativeGlk/OpRegionSnapshot.hpp", "Mellow/GlkOpRegionIOKit.hpp",
               "tests/native_glk_opregion_shim/glk_opregion_shim.hpp", "tests/run_native_glk_opregion.py"]}}
    # Hosted green build and two fatal sanitizer runs.
    binary = R + "/bin/native_glk_opregion_tests"
    res["hosted_build"] = hosted_build(binary, log=R + "/logs/hosted-build.log")
    res["hosted_runs"] = []
    if res["hosted_build"]["rc"] == 0:
        for i in (1, 2):
            res["hosted_runs"].append(run([binary], 120, R + f"/logs/hosted-run{i}.log", env=ENV))
    green = res["hosted_build"]["rc"] == 0 and len(res["hosted_runs"]) == 2 and \
        all(r["rc"] == 0 for r in res["hosted_runs"])
    # Mutants: each must build and then fail the fatal suite.
    res["mutants"] = []
    for name, key, old, new in (MUTANTS if green else []):
        rel = SOURCES[key]
        text = open(W + "/" + rel).read()
        entry = {"name": name, "file": rel, "occurrences": text.count(old)}
        if text.count(old) != 1:
            entry["verdict"] = "not-applicable"
            res["mutants"].append(entry)
            continue
        mdir = R + "/mutants/" + name
        os.makedirs(mdir)
        mpath = mdir + "/" + os.path.basename(rel)
        open(mpath, "w").write(text.replace(old, new))
        b = hosted_build(mdir + "/tests", {rel: mpath}, mdir + "/build.log")
        entry["build_rc"] = b["rc"]
        if b["rc"] != 0:
            entry["verdict"] = "build-failed"
        else:
            r = run([mdir + "/tests"], 120, mdir + "/run.log", env=ENV)
            entry["run_rc"] = r["rc"]
            entry["first_failure"] = next((l for l in open(mdir + "/run.log").read().splitlines()
                                           if "FATAL" in l or "ERROR: " in l or "runtime error" in l), "")
            entry["verdict"] = "killed" if r["rc"] != 0 else "SURVIVED"
        res["mutants"].append(entry)
    # Genuine SDK 15.5 x86_64 kernel-mode compile.
    resource = subprocess.run([CXX, "-print-resource-dir"], stdout=subprocess.PIPE).stdout.decode().strip()
    kflags = ["-target", "x86_64-apple-macos15.0", "-std=c++17", "-mkernel", "-fapple-kext", "-nostdinc",
              "-isystem", resource + "/include", "-isystem", KHDR, "-DKERNEL", "-fno-exceptions", "-fno-rtti",
              "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables", "-fvisibility=hidden",
              "-Wall", "-Wextra", "-Werror", "-Wno-ossharedptr-misuse",
              # IOPCIDevice is __kpi_deprecated("Use PCIDriverKit") in SDK 15.5 but still exported.
              "-Wno-deprecated-declarations", "-O2"]
    res["kernel"] = {"flags": kflags, "objects": {}}
    objects = {}
    for rel in [SOURCES["snapshot"], SOURCES["adapter"], "Mellow/GlkPhysicalOwnerIOKit.cpp"]:
        obj = R + "/kernel/" + os.path.basename(rel).replace(".cpp", ".o")
        c = run([CXX] + kflags + ["-c", W + "/" + rel, "-o", obj], 300, obj + ".log")
        entry = {"rc": c["rc"], "source_sha256": sha(W + "/" + rel), "frozen_input": "PhysicalOwner" in rel}
        if c["rc"] == 0:
            entry["object_sha256"] = sha(obj)
            entry["macho"] = subprocess.run([TC + "/otool", "-hv", obj], stdout=subprocess.PIPE).stdout.decode().split("\n")[3].split()[:3]
            objects[rel] = obj
        res["kernel"]["objects"][rel] = entry
    exports = set()
    for f in sorted(os.listdir(XCONF)):
        if f.endswith(".exports"):
            for line in open(XCONF + "/" + f):
                sym = line.split(":")[0].split("#")[0].strip()
                if sym:
                    exports.add(sym)
    defined = set()
    for obj in objects.values():
        defined |= set(subprocess.run([TC + "/nm", "-gUj", obj], stdout=subprocess.PIPE).stdout.decode().split())
    pci_header = open(KHDR + "/IOKit/pci/IOPCIDevice.h").read()
    audit = {}
    for rel in (SOURCES["snapshot"], SOURCES["adapter"]):
        if rel not in objects:
            continue
        undefined = subprocess.run([TC + "/nm", "-uj", objects[rel]], stdout=subprocess.PIPE).stdout.decode().split()
        cls = {"xnu_exports": [], "kext_objects": [], "iopcifamily_kpi": [], "unresolved": []}
        for sym in undefined:
            if sym in defined:
                cls["kext_objects"].append(sym)
            elif sym in exports:
                cls["xnu_exports"].append(sym)
            elif sym.startswith("__ZN11IOPCIDevice") and re.sub(r"^__ZN11IOPCIDevice\d+", "", sym) is not None \
                    and re.search(r"__ZN11IOPCIDevice\d+(\w+?)E", sym) and \
                    re.search(r"__ZN11IOPCIDevice\d+(\w+?)E", sym).group(1) in pci_header:
                cls["iopcifamily_kpi"].append(sym)
            else:
                cls["unresolved"].append(sym)
        cls["forbidden"] = [s for s in undefined if FORBIDDEN.search(s)]
        audit[rel] = cls
    res["kernel"]["symbols"] = audit
    adapter_obj = objects.get(SOURCES["adapter"])
    if adapter_obj:
        dis = subprocess.run([TC + "/otool", "-tvV", adapter_obj], stdout=subprocess.PIPE).stdout.decode()
        open(R + "/kernel/GlkOpRegionIOKit.disasm.txt", "w").write(dis)
        # kIOMapAnywhere|kIOMapReadOnly|kIOMapDefaultCache = 0x1001; descriptor options 0x802.
        res["kernel"]["immediates"] = {"map_options_0x1001": len(re.findall(r"\$0x1001\b", dis)),
                                       "descriptor_options_0x802": len(re.findall(r"\$0x802\b", dis)),
                                       "createMappingInTask_calls": dis.count("createMappingInTask"),
                                       "withAddressRange_calls": dis.count("withAddressRange")}
    killed = sum(1 for m in res["mutants"] if m.get("verdict") == "killed")
    res["summary"] = {
        "hosted_green": green,
        "mutants_total": len(res["mutants"]), "mutants_killed": killed,
        "mutants_other": [m["name"] + ":" + m["verdict"] for m in res["mutants"] if m.get("verdict") != "killed"],
        "kernel_compiled": all(e["rc"] == 0 for e in res["kernel"]["objects"].values()),
        "unresolved_symbols": sum(len(a["unresolved"]) for a in audit.values()),
        "forbidden_symbols": sum(len(a["forbidden"]) for a in audit.values()),
    }
    json.dump(res, open(R + "/run-results.json", "w"), indent=1, sort_keys=True)
    print(json.dumps(res["summary"], indent=1))
    ok = green and killed == len(MUTANTS) and res["summary"]["kernel_compiled"] and \
        not res["summary"]["unresolved_symbols"] and not res["summary"]["forbidden_symbols"]
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

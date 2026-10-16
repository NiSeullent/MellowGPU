#!/usr/bin/env python3
"""Host, CPU-only integration tests for the GLK device lab.

Never touches a GPU, Metal device, WindowServer session, kext or system path. Every tool
binary run here either uses a CPU-only mode (--selfcheck / --oracle-digest) or must stop
at the host gate before any device call (asserted from its event stream). On the
physical target itself the gate-refusal checks are NOT_RUN (they would reach device paths).

  selfchecks         CPU-only self-checks (fixed oracle vector + 21 ownership/host-gate
                     known-answer cases) of the x86_64 target artifacts and host variants.
  gate_refusals      The real tools on a non-target host fail with the exact gate code
                     and emit no device/WindowServer event.
  oracle_crosscheck  Swift (target + host builds) and C++ (plain + fatal ASan/UBSan)
                     digests agree on fixed, edge and random vectors incl. the default
                     benchmark workload size.
  orchestrator       selfcheck pipeline; parent watchdog kills hanging process groups and
                     detects a descendant escaping its group; deadline gaps; zero runs;
                     device-mode refusal off-target; acceptance-logic unit cases.
  staging            build validation and tamper refusal; destination validation; static
                     script safety incl. injected negatives; install guards refuse with
                     zero writes; transactional install, re-install refusal, rollback and
                     tamper-protected rollback inside a sandbox root.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True  # never leave __pycache__ in the source tree

DEFAULT_BUILD = Path("/Users/skyhype/Documents/Codex/2026-10-15/sp/work/claude-swarm-20261016"
                     "/wave7-results/glk_device_lab_author/build")
DEVICE_LAB = Path(__file__).resolve().parents[1]
PINNED_PATH_LINE = "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
DEVICE_STAGES = {"host_gate_passed", "device_enumeration", "device", "device_selected",
                 "windowserver_probe", "msl_compile_started", "headless_check_completed"}
SAN_ENV = {"ASAN_OPTIONS": "halt_on_error=1:abort_on_error=1:detect_leaks=0",
           "UBSAN_OPTIONS": "halt_on_error=1:abort_on_error=1:print_stacktrace=1"}
TOOLS = ("glk-metal-bench", "glk-framebuffer-probe")


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, DEVICE_LAB / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ORCH = load("glk_orchestrator", "orchestrate/run-glk-device-lab.py")
STAGE = load("glk_stage", "staging/stage-glk-device-lab.py")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, env=None, timeout=300):
    proc = subprocess.run([str(c) for c in command], capture_output=True, text=True, timeout=timeout, env=env)
    return proc.returncode, proc.stdout, proc.stderr


def last_line_json(text):
    events = ORCH.parse_jsonl(text)
    return events[-1] if events else {}


def write_exec(path: Path, text: str):
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


class Checks:
    def __init__(self):
        self.items = []
        self.not_run = []

    def check(self, name, ok, **detail):
        self.items.append(dict(name=name, ok=bool(ok), **detail))
        return bool(ok)

    def section(self):
        status = "PASS" if self.items and all(i["ok"] for i in self.items) else "FAIL"
        return {"status": status, "passed": sum(i["ok"] for i in self.items), "total": len(self.items),
                "failed": [i["name"] for i in self.items if not i["ok"]], "not_run": self.not_run,
                "checks": self.items}


def host_facts():
    facts, violations = ORCH.host_gate()
    hardware = "arm64" if facts["hw_optional_arm64"] == 1 else facts["machine"]
    return {"facts": facts, "violations": violations, "hardware_arch": hardware, "is_target": not violations}


def expected_gate_code(binary_arch, host):
    facts = host["facts"]
    if host["hardware_arch"] == "arm64" and binary_arch == "x86_64":
        return 22  # runs under Rosetta
    if binary_arch != "x86_64" or host["hardware_arch"] == "arm64":
        return 26
    if facts["os_build"] != "24H32" or facts["os_product"] != "15.8.1":
        return 27
    return 0


def binary_sets(build_dir: Path, host):
    sets = {"target_x86_64": (build_dir, "x86_64")}
    if (build_dir / "host").is_dir():
        sets["host_" + host["hardware_arch"]] = (build_dir / "host", host["hardware_arch"])
    return sets


# ---------------------------------------------------------------- 1. selfchecks
def test_selfchecks(build_dir, host):
    c = Checks()
    for label, (directory, _) in binary_sets(build_dir, host).items():
        for tool in TOOLS:
            rc, out, _ = run([directory / tool, "--selfcheck"])
            events = ORCH.parse_jsonl(out)
            final = events[-1] if events else {}
            started = next((e for e in events if e.get("stage") == "selfcheck_started"), {})
            translated = (started.get("host") or {}).get("proc_translated")
            cases = final.get("identity_cases") or []
            c.check("%s/%s --selfcheck" % (label, tool),
                    rc == 0 and final.get("stage") == "selfcheck_completed" and final.get("passed") is True
                    and final.get("metal_called") is False and final.get("identity_failures") == 0
                    and len(cases) >= 21 and not ({e.get("stage") for e in events} & DEVICE_STAGES),
                    returncode=rc, identity_cases=len(cases), proc_translated=translated,
                    execution="rosetta-translated, CPU-only" if translated == 1 else "native, CPU-only")
    return c.section()


# ---------------------------------------------------------------- 2. gate refusals
def test_gate_refusals(build_dir, host):
    c = Checks()
    if host["is_target"]:
        c.not_run.append("this host passes the host gate (physical target); refusal runs would reach device paths")
        return dict(c.section(), status="NOT_RUN")
    invocations = [("glk-metal-bench", []), ("glk-metal-bench", ["--enumerate-only"]),
                   ("glk-metal-bench", ["--registry-id", "0x1"]), ("glk-framebuffer-probe", []),
                   ("glk-framebuffer-probe", ["--headless-check"])]
    for label, (directory, arch) in binary_sets(build_dir, host).items():
        want = expected_gate_code(arch, host)
        for tool, extra in invocations:
            rc, out, _ = run([directory / tool, *extra], timeout=60)
            events = ORCH.parse_jsonl(out)
            stages = [e.get("stage") for e in events]
            final = events[-1] if events else {}
            c.check("%s/%s %s refused at gate %d" % (label, tool, " ".join(extra) or "(no args)", want),
                    rc == want and final.get("stage") == "failed" and final.get("code") == want
                    and final.get("metal_called") is False and not (set(stages) & DEVICE_STAGES),
                    returncode=rc, stages=stages, error=final.get("error"))
    return c.section()


# ---------------------------------------------------------------- 3. oracle cross-check
def test_oracle_crosscheck(build_dir, host):
    c = Checks()
    cpp = {k: build_dir / ("glk_oracle_tests_%s" % k) for k in ("plain", "asan_ubsan")}
    swift = {label: directory / "glk-metal-bench" for label, (directory, _) in binary_sets(build_dir, host).items()}
    env = dict(os.environ, **SAN_ENV)

    _, out, _ = run([build_dir / "glk-metal-bench", "--selfcheck"])
    fixed_swift = last_line_json(out)
    rc, out, _ = run([cpp["asan_ubsan"], "--nonce", "0x12345678", "--rounds", "3", "--count", "8"], env=env)
    fixed_cpp = last_line_json(out)
    c.check("fixed known-answer vector: Swift --selfcheck == C++ (ASan/UBSan, known-answer checks on)",
            rc == 0 and fixed_cpp.get("status") == "PASS_GLK_ORACLE"
            and fixed_swift.get("expected_first") == fixed_cpp.get("expected_first")
            and fixed_swift.get("checksum") == fixed_cpp.get("checksum")
            and fixed_swift.get("fnv1a64") == fixed_cpp.get("fnv1a64"),
            swift=fixed_swift.get("fnv1a64"), cpp=fixed_cpp.get("fnv1a64"))

    rng = random.Random(0x474C4B)
    vectors = [(0x12345678, 3, 8), (0, 1, 1024), (0xFFFFFFFF, 1, 4097), (0xFFFFFFFF, 65536, 1024),
               (0x80000000, 7, 1)]
    vectors += [(rng.getrandbits(32), rng.randint(1, 96), rng.randint(1024, 1 << 16)) for _ in range(4)]
    vectors.append((rng.getrandbits(32), 64, 1 << 20))  # the benchmark's default compute workload
    for nonce, rounds, count in vectors:
        width = min(4, count)
        results = {}
        for label, binary in swift.items():
            rc, out, _ = run([binary, "--oracle-digest", hex(nonce), str(rounds), str(count)])
            e = last_line_json(out)
            results["swift_" + label] = (rc == 0 and e.get("stage") == "oracle_digest_completed"
                                         and e.get("metal_called") is False, e.get("fnv1a64"), e.get("checksum"),
                                         tuple((e.get("expected_first") or [])[:width]))
        for label, binary in cpp.items():
            rc, out, _ = run([binary, "--nonce", hex(nonce), "--rounds", str(rounds), "--count", str(count),
                              "--emit-only"], env=env)
            e = last_line_json(out)
            results["cpp_" + label] = (rc == 0 and e.get("status") == "PASS_GLK_ORACLE", e.get("fnv1a64"),
                                       e.get("checksum"), tuple((e.get("expected_first") or [])[:width]))
        agree = all(v[0] for v in results.values()) and all(len({v[i] for v in results.values()}) == 1
                                                             for i in (1, 2, 3))
        c.check("nonce=%#x rounds=%d count=%d: %d implementations agree" % (nonce, rounds, count, len(results)),
                agree and None not in {v[1] for v in results.values()},
                fnv1a64=sorted({str(v[1]) for v in results.values()}), implementations=sorted(results))
    return c.section()


# ---------------------------------------------------------------- 4. orchestrator
def genuine_events(kind):
    host = {"proc_translated": 0, "process_arch": "x86_64", "kern_osversion": "24H32",
            "kern_osproductversion": "15.8.1"}
    pci = {"vendor_id": 0x8086, "device_id": 0x3185, "revision_id": 6, "subsystem_vendor_id": 0x17AA,
           "subsystem_id": 0x3E77, "pci_location": "00:02.0", "parent_is_pci_bridge": True,
           "synthetic_identity_keys": []}
    rid = "4294968000"
    events = [{"stage": "started"}, {"stage": "host_gate_passed"}, {"stage": "device_enumeration"},
              {"stage": "device_selected", "registry_id": rid}]
    final = {"stage": "completed", "registry_id": rid, "pci": pci, "host": host}
    final.update({k: True for k in ORCH.MUST_BE_TRUE[kind]})
    final.update({k: False for k in ORCH.MUST_BE_FALSE[kind]})
    if kind == "bench":
        for section, stage in (("compute", "compute_iteration"), ("render", "render_iteration"),
                               ("blit", "blit_iteration")):
            final[section] = {"iterations": 2, "passed": True, "gpu_seconds": {"count": 2, "min": 0.0012}}
            events += [{"stage": stage, "registry_id": rid, "mismatches": 0} for _ in range(2)]
    else:
        final.update({"frames_requested": 3, "frames_presented": 3, "drawable_readback_validated_frames": 3,
                      "presentation_timestamps_captured": 3, "present_callback_timeouts": 0,
                      "dropped_presents": 0})
    return events + [final]


def _first(events, stage):
    return next(e for e in events if e.get("stage") == stage)


CLASSIFY_CASES = [
    ("genuine bench report", "bench", None, 0, "PASS"),
    ("genuine probe report", "probe", None, 0, "PASS"),
    ("nonzero exit", "bench", None, 1, "FAIL"),
    ("wrong revision", "bench", lambda e: e[-1]["pci"].update(revision_id=3), 0, "FAIL"),
    ("wrong subsystem", "bench", lambda e: e[-1]["pci"].update(subsystem_id=0), 0, "FAIL"),
    ("wrong PCI location", "bench", lambda e: e[-1]["pci"].update(pci_location="00:01.0"), 0, "FAIL"),
    ("parent not a bridge", "bench", lambda e: e[-1]["pci"].update(parent_is_pci_bridge=False), 0, "FAIL"),
    ("synthetic identity", "bench", lambda e: e[-1]["pci"].update(synthetic_identity_keys=["X"]), 0, "FAIL"),
    ("Rosetta process", "bench", lambda e: e[-1]["host"].update(proc_translated=1), 0, "FAIL"),
    ("arm64 process", "bench", lambda e: e[-1]["host"].update(process_arch="arm64"), 0, "FAIL"),
    ("other macOS build", "bench", lambda e: e[-1]["host"].update(kern_osversion="24G90"), 0, "FAIL"),
    ("CPU fallback claimed", "bench", lambda e: e[-1].update(cpu_fallback_used=True), 0, "FAIL"),
    ("fence evidence overclaim", "bench", lambda e: e[-1].update(native_driver_fence_evidence=True), 0, "FAIL"),
    ("full GPU execution overclaim", "bench", lambda e: e[-1].update(full_gpu_execution_attested=True), 0, "FAIL"),
    ("honesty flag missing", "bench", lambda e: e[-1].pop("hardware_qualification"), 0, "FAIL"),
    ("ownership not attested", "bench", lambda e: e[-1].update(glk_ownership_attested=False), 0, "FAIL"),
    ("iteration event missing", "bench", lambda e: e.remove(_first(e, "render_iteration")), 0, "FAIL"),
    ("iteration on another device", "bench", lambda e: _first(e, "blit_iteration").update(registry_id="1"), 0, "FAIL"),
    ("zero GPU time", "bench", lambda e: e[-1]["compute"]["gpu_seconds"].update(min=0), 0, "FAIL"),
    ("host gate event missing", "bench", lambda e: e.remove(_first(e, "host_gate_passed")), 0, "FAIL"),
    ("failed event before completed", "bench", lambda e: e.insert(-1, {"stage": "failed", "code": 7}), 0, "FAIL"),
    ("diagnostic-only stage", "bench", lambda e: e.__setitem__(-1, {"stage": "enumerate_only_completed"}), 0, "FAIL"),
    ("no terminal event", "bench", lambda e: e.pop(), 0, "FAIL"),
    ("probe scanout overclaim", "probe", lambda e: e[-1].update(scanout_screencapture_verified=True), 0, "FAIL"),
    ("probe dropped present", "probe", lambda e: e[-1].update(dropped_presents=1, frames_presented=2), 0, "FAIL"),
    ("probe readback incomplete", "probe", lambda e: e[-1].update(drawable_readback_validated_frames=2), 0, "FAIL"),
]


def test_orchestrator(build_dir, out, host):
    c = Checks()
    orchestrator = DEVICE_LAB / "orchestrate" / "run-glk-device-lab.py"

    def orchestrate(args, scratch, timeout=180):
        rc, _, err = run([sys.executable, "-B", orchestrator, *args, "--out", scratch], timeout=timeout)
        path = scratch / "orchestrator-report.json"
        return rc, (json.loads(path.read_text()) if path.is_file() else {}), err

    rc, rep, err = orchestrate(["--selfcheck-only", "--bench-runs", "2", "--probe-runs", "1",
                                "--build-dir", str(build_dir)], out / "o-selfcheck")
    runs = rep.get("runs", {}).get("bench", []) + rep.get("runs", {}).get("probe", [])
    c.check("selfcheck pipeline over the real tools passes (no Metal)",
            rc == 0 and rep.get("status") == "PASS" and rep.get("metal_tested") is False and len(runs) == 3
            and rep.get("clean_cleanup") is True and all(r.get("process_group_reaped") for r in runs),
            returncode=rc, stderr=err[-300:])

    hang_dir = out / "fake-hang"
    hang_dir.mkdir()
    write_exec(hang_dir / "glk-metal-bench",
               '#!/bin/sh\necho $$ > "%s/leader.pid"\nsleep 300 &\necho $! > "%s/grandchild.pid"\n'
               'echo \'{"stage":"started","tool":"fake-hanging-tool"}\'\nwait\n' % (hang_dir, hang_dir))
    started = time.monotonic()
    rc, rep, _ = orchestrate(["--selfcheck-only", "--bench-runs", "1", "--run-timeout", "2",
                              "--total-timeout", "60", "--build-dir", str(hang_dir)], out / "o-watchdog")
    elapsed = time.monotonic() - started
    leader = int((hang_dir / "leader.pid").read_text())
    grandchild = int((hang_dir / "grandchild.pid").read_text())
    run0 = (rep.get("runs", {}).get("bench") or [{}])[0]
    c.check("hanging tool: TIMEOUT, whole process group SIGKILLed and reaped, run FAILS",
            rc == 2 and rep.get("status") == "FAIL" and run0.get("status") == "TIMEOUT"
            and run0.get("killed_process_group") is True and run0.get("process_group_reaped") is True
            and rep.get("clean_cleanup") is False and not pid_alive(leader) and not pid_alive(grandchild)
            and elapsed < 30, returncode=rc, elapsed_seconds=round(elapsed, 2),
            leader_alive=pid_alive(leader), grandchild_alive=pid_alive(grandchild))

    escape_dir = out / "fake-escape"
    escape_dir.mkdir()
    write_exec(escape_dir / "glk-metal-bench",
               "#!%s\nimport os, time\nif os.fork() == 0:\n    os.setsid()\n"
               "    open(%r, 'w').write(str(os.getpid()))\n    time.sleep(120)\n    os._exit(0)\n"
               "print('{\"stage\":\"started\",\"tool\":\"fake-escaping-tool\"}', flush=True)\ntime.sleep(120)\n"
               % (sys.executable, str(escape_dir / "escaped.pid")))
    started = time.monotonic()
    rc, rep, _ = orchestrate(["--selfcheck-only", "--bench-runs", "1", "--run-timeout", "2",
                              "--total-timeout", "60", "--build-dir", str(escape_dir)], out / "o-escape")
    elapsed = time.monotonic() - started
    escaped_pid = int((escape_dir / "escaped.pid").read_text()) if (escape_dir / "escaped.pid").is_file() else 0
    escaped_alive = escaped_pid > 0 and pid_alive(escaped_pid)
    if escaped_alive:
        os.kill(escaped_pid, signal.SIGKILL)  # test cleanup of the deliberately escaped process
    run0 = (rep.get("runs", {}).get("bench") or [{}])[0]
    c.check("descendant escaping its process group is detected; parent does not hang",
            rc == 2 and run0.get("status") == "TIMEOUT" and run0.get("escaped_descendants") is True
            and rep.get("clean_cleanup") is False and escaped_alive and elapsed < 30,
            returncode=rc, elapsed_seconds=round(elapsed, 2), escaped_process_was_alive=escaped_alive)

    rc, rep, _ = orchestrate(["--selfcheck-only", "--bench-runs", "3", "--run-timeout", "2",
                              "--total-timeout", "3", "--build-dir", str(hang_dir)], out / "o-deadline")
    c.check("parent total deadline leaves NOT_RUN gaps and FAILS",
            rc == 2 and rep.get("status") == "FAIL"
            and any(g.get("reason") == "parent deadline reached before start" for g in rep.get("not_run_gaps", [])),
            returncode=rc, gaps=rep.get("not_run_gaps"))

    rc, rep, _ = orchestrate(["--selfcheck-only", "--bench-runs", "0", "--probe-runs", "0",
                              "--build-dir", str(build_dir)], out / "o-zero")
    c.check("zero requested runs is a FAIL, not a pass", rc == 2 and rep.get("status") == "FAIL", returncode=rc)

    if host["is_target"]:
        c.not_run.append("device-mode refusal: this host is the physical target")
    else:
        marker_dir = out / "fake-marker"
        marker_dir.mkdir()
        write_exec(marker_dir / "glk-metal-bench", '#!/bin/sh\ntouch "%s/EXECUTED"\n' % marker_dir)
        rc, rep, _ = orchestrate(["--bench-runs", "1", "--build-dir", str(marker_dir)], out / "o-device-refusal")
        c.check("device mode refuses off-target before starting any tool",
                rc == 2 and rep.get("status") == "FAIL" and rep.get("host_gate", {}).get("passed") is False
                and not (marker_dir / "EXECUTED").exists() and rep.get("runs", {}).get("bench") == [],
                returncode=rc, violations=rep.get("host_gate", {}).get("violations"))

    for name, kind, mutate, rc, expected in CLASSIFY_CASES:
        events = copy.deepcopy(genuine_events(kind))
        if mutate:
            mutate(events)
        status, reasons = ORCH.classify_run(kind, rc, False, events, False)
        c.check("classify: %s -> %s" % (name, expected), status == expected, got=status, reasons=reasons[:3])
    status, _ = ORCH.classify_run("bench", None, True, genuine_events("bench"), False)
    c.check("classify: timed out -> TIMEOUT", status == "TIMEOUT", got=status)
    ok_selfcheck = [{"stage": "selfcheck_completed", "passed": True, "metal_called": False}]
    bad_selfcheck = [{"stage": "selfcheck_completed", "passed": True, "metal_called": True}]
    c.check("classify: clean selfcheck -> PASS", ORCH.classify_run("bench", 0, False, ok_selfcheck, True)[0] == "PASS")
    c.check("classify: selfcheck that touched Metal -> FAIL",
            ORCH.classify_run("bench", 0, False, bad_selfcheck, True)[0] == "FAIL")
    return c.section()


# ---------------------------------------------------------------- 5. staging
SHIMS = {
    "uname": '#!/bin/sh\nif [ "${1:-}" = "-m" ]; then echo "${GLK_SHIM_ARCH:-x86_64}"; '
             'else exec /usr/bin/uname "$@"; fi\n',
    "sysctl": ('#!/bin/sh\ncase "$*" in\n'
               '  *sysctl.proc_translated*) echo "${GLK_SHIM_TRANSLATED:-0}" ;;\n'
               '  *hw.optional.arm64*) if [ -n "${GLK_SHIM_ARM64:-}" ]; then echo "$GLK_SHIM_ARM64"; else exit 1; fi ;;\n'
               '  *machdep.cpu.brand_string*) echo "${GLK_SHIM_CPU:-Intel(R) Celeron(R) N4120 CPU @ 1.10GHz}" ;;\n'
               '  *) exec /usr/sbin/sysctl "$@" ;;\nesac\n'),
    "sw_vers": ('#!/bin/sh\ncase "${1:-}" in\n  -buildVersion) echo "${GLK_SHIM_BUILD:-24H32}" ;;\n'
                '  -productVersion) echo "${GLK_SHIM_PRODUCT:-15.8.1}" ;;\n  *) exec /usr/bin/sw_vers "$@" ;;\nesac\n'),
    "ioreg": ('#!/bin/sh\nnode() {\n'
              '  printf \'+-o %s  <class IOPCIDevice, id 0x1000002%s, registered, matched, active, busy 0 (0 ms), retain 9>\\n\' "$1" "$2"\n'
              '  printf \'    {\\n      "vendor-id" = <%s>\\n      "device-id" = <%s>\\n\' "$3" "$4"\n'
              '  printf \'      "revision-id" = <%s>\\n      "subsystem-vendor-id" = <aa170000>\\n\' "$5"\n'
              '  printf \'      "subsystem-id" = <773e0000>\\n    }\\n\'\n}\n'
              'node PXSX@0 10 86800000 a8310000 06000000\n'
              'node GFX0@2 06 86800000 85310000 "${GLK_SHIM_REV:-06}000000"\n'
              'if [ -n "${GLK_SHIM_DUP:-}" ]; then node GFX1@3 07 86800000 85310000 06000000; fi\n'),
}


def make_shims(shim_dir: Path):
    shim_dir.mkdir(parents=True, exist_ok=True)
    for name, body in SHIMS.items():
        write_exec(shim_dir / name, body)


def make_shimmed_scripts(bundle: Path, shim_dir: Path):
    """Test doubles: copies of the generated scripts whose single pinned PATH line also
    searches the shim dir (the target fingerprint emulation). Everything else is identical."""
    for name in ("install.sh", "rollback.sh"):
        text = (bundle / name).read_text()
        if text.splitlines().count(PINNED_PATH_LINE) != 1:
            raise AssertionError("%s does not pin PATH exactly once" % name)
        (bundle / ("shim-" + name)).write_text(
            text.replace(PINNED_PATH_LINE, "PATH=%s:/usr/bin:/bin:/usr/sbin:/sbin" % shim_dir, 1))


def test_staging(build_dir, out, host):
    c = Checks()
    stage_tool = DEVICE_LAB / "staging" / "stage-glk-device-lab.py"
    sim_root = out / "sim-target-root"
    install_root = str(sim_root / "glk-device-lab")
    bundle = out / "bundle"

    def stage(args, timeout=180):
        rc, so, se = run([sys.executable, "-B", stage_tool, *map(str, args)], timeout=timeout)
        try:
            return rc, json.loads(so), se
        except json.JSONDecodeError:
            return rc, {}, se

    rc, rep, err = stage(["--build-dir", build_dir, "--out", bundle, "--install-root", install_root])
    c.check("staging validates the build and assembles a PASS bundle",
            rc == 0 and rep.get("status") == "PASS" and rep.get("payload_consistent") is True
            and rep.get("sh_syntax_ok") is True and not any((rep.get("script_safety") or {}).values())
            and rep.get("build_validation", {}).get("problems") == [], returncode=rc, error=rep.get("error"),
            stderr=err[-300:])
    if rc != 0:
        return c.section()
    manifest = json.loads((bundle / "manifest.json").read_text())
    want = {f["name"]: f["sha256"] for f in manifest["files"]}
    c.check("SHA256SUMS verifies every bundle file",
            subprocess.run(["shasum", "-a", "256", "-c", "SHA256SUMS"], cwd=bundle, capture_output=True).returncode == 0)
    c.check("manifest pins the generated scripts' sha256",
            all(manifest["scripts"][n] == sha256_file(bundle / n) for n in ("install.sh", "rollback.sh")))
    c.check("payload = build artifacts (x86_64) + orchestrator",
            set(want) == {"glk-metal-bench", "glk-framebuffer-probe", "glk-device-lab-run"}
            and want["glk-metal-bench"] == sha256_file(build_dir / "glk-metal-bench"))

    # Destination validation and static script safety (unit level).
    for bad in ("/System/Library/Extensions/x", "/Library/Extensions/x", "/Users/a b/x", "/Users/x/$(id)",
                "/Users/../System/x", "relative/path", "/usr/bin/x", "/private/var/x", "/Users/x;rm",
                "/Users", "/Users/x/", "/Users//x", "/Volumes/EFI/x"):
        try:
            STAGE.validate_dest(bad)
            rejected = False
        except ValueError:
            rejected = True
        c.check("validate_dest rejects %r" % bad, rejected)
    clean = (bundle / "install.sh").read_text()
    c.check("generated install.sh/rollback.sh have zero static-safety violations",
            STAGE.scan_script_safety(clean, install_root) == []
            and STAGE.scan_script_safety((bundle / "rollback.sh").read_text(), install_root) == [])
    for injected in ("sudo true", "ssh target true", "kmutil load -p x", "nvram boot-args=x", "csrutil disable",
                     "rm -rf /tmp/x", 'cp x "/System/Library/Extensions/x"', "launchctl load x",
                     "python3 -c 1", "eval x", "tccutil reset All", "bless --setBoot", "reboot"):
        c.check("static scan flags injected %r" % injected,
                STAGE.scan_script_safety(clean + "\n" + injected + "\n", install_root) != [])
    c.check("static scan flags an unpinned PATH",
            STAGE.scan_script_safety(clean.replace(PINNED_PATH_LINE, ""), install_root) != [])

    # Staging must refuse a tampered build and an unsafe install root (writing nothing).
    tampered = out / "tampered-build"
    tampered.mkdir()
    for name in ("glk-metal-bench", "glk-framebuffer-probe", "build-report.json"):
        shutil.copyfile(build_dir / name, tampered / name)
    data = bytearray((tampered / "glk-metal-bench").read_bytes())
    data[-1] ^= 0xFF
    (tampered / "glk-metal-bench").write_bytes(bytes(data))
    rc, rep, _ = stage(["--build-dir", tampered, "--out", out / "bundle-tampered", "--install-root", install_root])
    c.check("staging refuses an artifact whose bytes differ from the build report",
            rc == 2 and "sha256" in (rep.get("error") or "") and not (out / "bundle-tampered" / "install.sh").exists(),
            error=(rep.get("error") or "")[:200])
    rc, rep, _ = stage(["--build-dir", build_dir, "--out", out / "bundle-badroot",
                        "--install-root", "/System/Library/Extensions/glk"])
    c.check("staging refuses a system install root and writes nothing",
            rc == 2 and not (out / "bundle-badroot").exists(), error=(rep.get("error") or "")[:200])

    # Install guards: every refusal must leave zero writes.
    shim_dir = out / "shim"
    make_shims(shim_dir)
    make_shimmed_scripts(bundle, shim_dir)
    state, receipt = bundle / "install-state.tsv", bundle / "install-receipt.json"

    def nothing_written():
        return not sim_root.exists() and not state.exists() and not receipt.exists() \
            and sorted(p.name for p in (bundle / "backups").iterdir()) == ["README.txt"]

    def sh(script, args=(), **overrides):
        return run(["/bin/sh", bundle / script, *args], env=dict(os.environ, **overrides), timeout=120)

    for label, script, args, overrides in (
            ("real install.sh, this non-target host, dry", "install.sh", (), {}),
            ("real install.sh, this non-target host, --confirm", "install.sh", ("--confirm",), {}),
            ("revision 03", "shim-install.sh", ("--confirm",), {"GLK_SHIM_REV": "03"}),
            ("two matching GPUs", "shim-install.sh", ("--confirm",), {"GLK_SHIM_DUP": "1"}),
            ("other macOS build", "shim-install.sh", ("--confirm",), {"GLK_SHIM_BUILD": "24G90"}),
            ("other macOS version", "shim-install.sh", ("--confirm",), {"GLK_SHIM_PRODUCT": "15.7"}),
            ("Rosetta-translated", "shim-install.sh", ("--confirm",), {"GLK_SHIM_TRANSLATED": "1"}),
            ("Apple silicon", "shim-install.sh", ("--confirm",), {"GLK_SHIM_ARM64": "1"}),
            ("arm64 arch", "shim-install.sh", ("--confirm",), {"GLK_SHIM_ARCH": "arm64"}),
            ("other CPU", "shim-install.sh", ("--confirm",), {"GLK_SHIM_CPU": "Apple M2"}),
            ("target fingerprint, no --confirm (dry preview)", "shim-install.sh", (), {})):
        rc, so, se = sh(script, args, **overrides)
        c.check("install refuses: %s (zero writes)" % label, rc != 0 and nothing_written(),
                returncode=rc, message=(se.strip().splitlines() or so.strip().splitlines() or [""])[-1][:160])

    tamper_bundle = out / "bundle-payload-tamper"
    shutil.copytree(bundle, tamper_bundle)
    data = bytearray((tamper_bundle / "payload" / "glk-metal-bench").read_bytes())
    data[100] ^= 0x01
    (tamper_bundle / "payload" / "glk-metal-bench").write_bytes(bytes(data))
    make_shimmed_scripts(tamper_bundle, shim_dir)
    rc, _, se = run(["/bin/sh", tamper_bundle / "shim-install.sh", "--confirm"], timeout=120)
    c.check("install refuses a tampered payload before writing anything",
            rc != 0 and "sha256 mismatch" in se and not sim_root.exists()
            and not (tamper_bundle / "install-state.tsv").exists(), message=se.strip()[-160:])

    # Transactional install / re-install refusal / rollback in the sandbox root.
    bench_dest = sim_root / "glk-device-lab" / "bin" / "glk-metal-bench"
    probe_dest = bench_dest.parent / "glk-framebuffer-probe"
    run_dest = bench_dest.parent / "glk-device-lab-run"
    bench_dest.parent.mkdir(parents=True)
    old_bytes = b"ORIGINAL-TARGET-BYTES-v0\n"
    bench_dest.write_bytes(old_bytes)
    rc, so, se = sh("shim-install.sh", ("--confirm",))
    receipt_json = json.loads(receipt.read_text()) if receipt.is_file() else {}
    backup = bundle / "backups" / "glk-metal-bench.orig"
    c.check("install: all destinations hold the exact payload bytes",
            rc == 0 and all(p.is_file() and sha256_file(p) == want[p.name] for p in (bench_dest, probe_dest, run_dest)),
            returncode=rc, stderr=se[-300:])
    c.check("install: verified original-byte backup + receipt + state",
            backup.is_file() and backup.read_bytes() == old_bytes
            and (bundle / "backups" / "glk-metal-bench.orig.sha256").read_text().strip()
            == hashlib.sha256(old_bytes).hexdigest()
            and len(receipt_json.get("installed", [])) == 3 and len(state.read_text().splitlines()) == 3)
    c.check("install: installed modes are 0755",
            all(stat_mode(p) == 0o755 for p in (bench_dest, probe_dest, run_dest)))
    rc, _, se = sh("shim-install.sh", ("--confirm",))
    c.check("re-install refused while install-state exists (backup slot never overwritten)",
            rc != 0 and backup.read_bytes() == old_bytes and sha256_file(probe_dest) == want[probe_dest.name],
            message=se.strip()[-160:])
    before = {p: p.read_bytes() for p in (bench_dest, probe_dest, run_dest)}
    rc, so, _ = sh("shim-rollback.sh")
    c.check("rollback without --confirm is a dry preview (nothing changed)",
            rc != 0 and all(p.read_bytes() == b for p, b in before.items()) and state.exists())
    rc, so, se = sh("shim-rollback.sh", ("--confirm",))
    c.check("rollback restores original bytes and removes newly installed files",
            rc == 0 and bench_dest.read_bytes() == old_bytes and not probe_dest.exists() and not run_dest.exists()
            and not state.exists() and (bundle / "install-state.tsv.rolledback").exists(),
            returncode=rc, stdout=so[-300:], stderr=se[-300:])

    rc, _, se = sh("shim-install.sh", ("--confirm",))
    reinstalled = rc == 0 and sha256_file(probe_dest) == want[probe_dest.name]
    with open(probe_dest, "ab") as handle:
        handle.write(b"locally modified after install")
    modified = probe_dest.read_bytes()
    rc, so, se = sh("shim-rollback.sh", ("--confirm",))
    c.check("rollback never clobbers a file changed after install (and reports failure)",
            reinstalled and rc != 0 and probe_dest.read_bytes() == modified and bench_dest.read_bytes() == old_bytes
            and not run_dest.exists() and state.exists(), returncode=rc, stderr=se.strip()[-200:])
    return c.section()


def stat_mode(path: Path) -> int:
    return path.stat().st_mode & 0o777


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--out", type=Path, required=True, help="New/empty scratch dir")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error("--out must be new or empty")
    if not str(out).startswith(STAGE.ALLOWED_DEST_ROOTS):
        parser.error("--out must be under %s: the sandbox install root inside it has to pass the staging "
                     "tool's destination validation" % (STAGE.ALLOWED_DEST_ROOTS,))
    out.mkdir(parents=True, exist_ok=True)
    build_dir = args.build_dir.resolve()

    host = host_facts()
    report = {"schema_version": 2, "status": "FAIL", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "build_dir": str(build_dir), "host": host, "hardware_execution": False, "metal_tested": False,
              "gpu_touched": False, "sections": {}}
    sections = (("selfchecks", lambda: test_selfchecks(build_dir, host)),
                ("gate_refusals", lambda: test_gate_refusals(build_dir, host)),
                ("oracle_crosscheck", lambda: test_oracle_crosscheck(build_dir, host)),
                ("orchestrator", lambda: test_orchestrator(build_dir, out, host)),
                ("staging", lambda: test_staging(build_dir, out, host)))
    for name, fn in sections:
        started = time.monotonic()
        try:
            report["sections"][name] = fn()
        except Exception as error:  # a harness defect is recorded as a failure, never skipped
            report["sections"][name] = {"status": "FAIL", "error": "%s: %s" % (type(error).__name__, error)}
        report["sections"][name]["seconds"] = round(time.monotonic() - started, 2)
    statuses = {k: v["status"] for k, v in report["sections"].items()}
    if all(s == "PASS" for s in statuses.values()):
        report["status"] = "PASS"
    elif all(s in ("PASS", "NOT_RUN") for s in statuses.values()):
        report["status"] = "INCOMPLETE"
    (out / "host-test-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "sections": {
        k: {"status": v["status"], "passed": v.get("passed"), "total": v.get("total"), "failed": v.get("failed"),
            "error": v.get("error")} for k, v in report["sections"].items()},
        "report": str(out / "host-test-report.json")}, indent=2))
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())

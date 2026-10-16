#!/usr/bin/env python3
"""Repeat-test orchestrator for the MellowGPU GLK device lab.

Runs glk-metal-bench and (optionally) glk-framebuffer-probe repeatedly against the
designated physical GLK GPU as bounded child processes, each in its own process group
under a parent watchdog (per-run and total deadlines; the whole group is SIGKILLed on
timeout). Captures read-only OS/kernel/boot/device identity and artifact hashes,
validates every reported success against the exact physical identity and honesty flags
(a "completed" event is never trusted on its own), aggregates throughput and frame
pacing only from validated runs, and records every NOT_RUN gap. It never fabricates
metrics and never reports a timed-out, unvalidated or unknown run as a success.

Device mode refuses to start any tool unless this host is the target: untranslated
x86_64, not Apple silicon, macOS 15.8.1 (24H32). --selfcheck-only runs only the tools'
CPU-only --selfcheck (no Metal/GPU access), e.g. on the build machine. No privileged
action, kext load, PCI/MMIO/DMA access or system change is performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import time

TOOL_DIR = Path(__file__).resolve().parent
TARGET_ARCH = "x86_64"
TARGET_BUILD = "24H32"
TARGET_PRODUCT = "15.8.1"
EXPECTED_PCI = {"vendor_id": 0x8086, "device_id": 0x3185, "revision_id": 0x06,
                "subsystem_vendor_id": 0x17AA, "subsystem_id": 0x3E77}
EXPECTED_LOCATION = "00:02.0"
TERMINAL_STAGES = ("completed", "failed", "selfcheck_completed",
                   "enumerate_only_completed", "headless_check_completed")
MUST_BE_TRUE = {
    "bench": ("glk_ownership_attested", "metal_tested", "metal3_family", "public_metal_only"),
    "probe": ("glk_ownership_attested", "metal_tested", "metal3_family", "public_metal_only",
              "windowserver_session", "window_presented", "drawable_readback_validated"),
}
# A public-API tool claiming any of these is overclaiming: they must be present and false.
MUST_BE_FALSE = {
    "bench": ("cpu_fallback_used", "native_driver_fence_evidence", "full_gpu_execution_attested",
              "windowserver_presentation_verified", "game_compatibility_attested", "hardware_qualification"),
    "probe": ("cpu_fallback_used", "native_driver_fence_evidence", "compositor_ownership_attested",
              "scanout_screencapture_verified", "game_compatibility_attested", "hardware_qualification"),
}
EVIDENCE_NOT_AVAILABLE = [
    "native_driver_fence_evidence: public Metal cannot attest the native driver's fences",
    "full_gpu_execution_attestation: public API success is not full third-party GPU execution",
    "scanout_screencapture: needs Screen Recording (TCC) permission, never requested; NOT_RUN",
    "game_compatibility / hardware_qualification: out of scope; qualified profiles remain 0",
]

# Read-only identity probes. None mutate state; each is bounded.
IDENTITY_COMMANDS = {
    "os_product": ["sw_vers", "-productVersion"],
    "os_build": ["sw_vers", "-buildVersion"],
    "uname": ["uname", "-a"],
    "sysctl": ["sysctl", "-n", "kern.osversion", "kern.osproductversion", "kern.bootsessionuuid",
               "machdep.cpu.brand_string", "hw.model"],
    "accelerator": ["ioreg", "-rc", "IOAccelerator", "-d", "1"],
    "pci_gpu": ["ioreg", "-r", "-d", "1", "-c", "IOPCIDevice", "-l", "-w", "0"],
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def command_text(command, timeout=20):
    try:
        proc = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except (OSError, subprocess.SubprocessError) as error:
        return None, "", str(error)


def sysctl_int(name):
    rc, out, _ = command_text(["sysctl", "-n", name])
    out = out.strip()
    return int(out) if rc == 0 and out.lstrip("-").isdigit() else None


def host_gate():
    """Device mode only runs on the physical target; returns (facts, violations)."""
    facts = {
        "machine": platform.machine(), "proc_translated": sysctl_int("sysctl.proc_translated"),
        "hw_optional_arm64": sysctl_int("hw.optional.arm64"),
        "os_build": command_text(["sw_vers", "-buildVersion"])[1].strip(),
        "os_product": command_text(["sw_vers", "-productVersion"])[1].strip(),
    }
    violations = []
    if facts["proc_translated"] not in (None, 0):
        violations.append("translated process (sysctl.proc_translated=%s)" % facts["proc_translated"])
    if facts["hw_optional_arm64"] == 1:
        violations.append("Apple silicon host (hw.optional.arm64=1)")
    if facts["machine"] != TARGET_ARCH:
        violations.append("machine %s is not %s" % (facts["machine"], TARGET_ARCH))
    if facts["os_build"] != TARGET_BUILD:
        violations.append("macOS build %r is not %s" % (facts["os_build"], TARGET_BUILD))
    if facts["os_product"] != TARGET_PRODUCT:
        violations.append("macOS %r is not %s" % (facts["os_product"], TARGET_PRODUCT))
    return facts, violations


def collect_identity():
    identity = {}
    for name, command in IDENTITY_COMMANDS.items():
        rc, out, err = command_text(command)
        identity[name] = {"command": command, "returncode": rc, "stdout": out[-4000:], "stderr": err[-500:],
                          "stdout_sha256": sha256_bytes(out.encode())}
    accel = identity["accelerator"]
    identity["accelerator_present"] = accel["returncode"] == 0 and bool(accel["stdout"].strip())
    return identity


def _signal_group(pgid, sig):
    try:
        os.killpg(pgid, sig)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def _group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def run_child(command, timeout):
    """Run one tool in its own process group under the parent watchdog. On timeout the
    whole group is SIGKILLed; a descendant that escaped the group and still holds the
    output pipes is detected and reported (never waited on forever)."""
    started = time.monotonic()
    proc = subprocess.Popen([str(c) for c in command], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, start_new_session=True)
    pgid = proc.pid
    timed_out = escaped = False
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        _signal_group(pgid, signal.SIGKILL)
        try:
            out, err = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired as late:
            escaped = True
            out, err = late.stdout or b"", late.stderr or b""
            for stream in (proc.stdout, proc.stderr):
                stream.close()
            proc.kill()
            proc.wait()
    stragglers = _signal_group(pgid, signal.SIGKILL)  # nothing from a finished run may linger
    deadline = time.monotonic() + 3
    while _group_alive(pgid) and time.monotonic() < deadline:
        time.sleep(0.05)
    return {
        "returncode": None if timed_out else proc.returncode, "exit_status": proc.returncode,
        "timed_out": timed_out, "elapsed_seconds": round(time.monotonic() - started, 3),
        "pid": proc.pid, "killed_process_group": timed_out or stragglers,
        "stragglers_killed": stragglers and not timed_out, "process_group_reaped": not _group_alive(pgid),
        "escaped_descendants": escaped,
        "stdout": out.decode("utf-8", "replace"), "stderr": err.decode("utf-8", "replace"),
    }


def parse_jsonl(text):
    events = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            events.append(obj)
    return events


def terminal_event(events):
    for event in reversed(events):
        if event.get("stage") in TERMINAL_STAGES:
            return event
    return None


def validate_completed(kind, final, events):
    """Reasons a 'completed' report is NOT acceptable evidence (empty list = acceptable)."""
    reasons = []
    pci = final.get("pci") or {}
    for key, want in EXPECTED_PCI.items():
        if pci.get(key) != want:
            reasons.append("pci %s=%r is not %#x" % (key, pci.get(key), want))
    if pci.get("pci_location") != EXPECTED_LOCATION:
        reasons.append("pci location %r is not %s" % (pci.get("pci_location"), EXPECTED_LOCATION))
    if pci.get("parent_is_pci_bridge") is not True:
        reasons.append("pci parent is not an IOPCIBridge")
    if pci.get("synthetic_identity_keys"):
        reasons.append("synthetic identity keys present: %s" % pci.get("synthetic_identity_keys"))
    host = final.get("host") or {}
    if host.get("proc_translated") != 0:
        reasons.append("process was translated or unknown (proc_translated=%r)" % host.get("proc_translated"))
    if host.get("process_arch") != TARGET_ARCH:
        reasons.append("process arch %r is not %s" % (host.get("process_arch"), TARGET_ARCH))
    if host.get("kern_osversion") != TARGET_BUILD:
        reasons.append("macOS build %r is not %s" % (host.get("kern_osversion"), TARGET_BUILD))
    if host.get("kern_osproductversion") != TARGET_PRODUCT:
        reasons.append("macOS %r is not %s" % (host.get("kern_osproductversion"), TARGET_PRODUCT))
    for key in MUST_BE_TRUE[kind]:
        if final.get(key) is not True:
            reasons.append("%s is not true" % key)
    for key in MUST_BE_FALSE[kind]:
        if final.get(key) is not False:
            reasons.append("%s must be present and false (got %r)" % (key, final.get(key)))
    registry_id = final.get("registry_id")
    selected = [e for e in events if e.get("stage") == "device_selected"]
    if len(selected) != 1 or selected[0].get("registry_id") != registry_id:
        reasons.append("device_selected missing or for another registry id")
    if not any(e.get("stage") == "host_gate_passed" for e in events):
        reasons.append("host gate not recorded as passed")
    if kind == "bench":
        for section, stage in (("compute", "compute_iteration"), ("render", "render_iteration"),
                               ("blit", "blit_iteration")):
            data = final.get(section) or {}
            n = data.get("iterations")
            iterations = [e for e in events if e.get("stage") == stage]
            gpu = data.get("gpu_seconds") or {}
            if data.get("passed") is not True or not isinstance(n, int) or n < 1:
                reasons.append("%s did not pass" % section)
            elif len(iterations) != n:
                reasons.append("%s: %d iteration events for %d iterations" % (section, len(iterations), n))
            elif any(e.get("registry_id") != registry_id for e in iterations):
                reasons.append("%s iteration ran on another registry id" % section)
            elif any(e.get("mismatches", 0) != 0 for e in iterations):
                reasons.append("%s iteration reported mismatches" % section)
            elif gpu.get("count") != n or not isinstance(gpu.get("min"), (int, float)) or gpu["min"] <= 0:
                reasons.append("%s GPU timestamps missing or non-positive" % section)
    else:
        frames = final.get("frames_requested")
        if not isinstance(frames, int) or frames < 1 or final.get("frames_presented") != frames:
            reasons.append("frames_presented %r != frames_requested %r" % (final.get("frames_presented"), frames))
        if final.get("drawable_readback_validated_frames") != frames:
            reasons.append("not every presented drawable was read back and validated")
        if not isinstance(final.get("presentation_timestamps_captured"), int) \
                or final["presentation_timestamps_captured"] < (frames or 1):
            reasons.append("presentation timestamps missing")
        if final.get("present_callback_timeouts") != 0 or final.get("dropped_presents") != 0:
            reasons.append("present callbacks timed out or presents were dropped")
    return reasons


def classify_run(kind, returncode, timed_out, events, selfcheck_only):
    """Pure decision: (status, reasons) for one child run."""
    if timed_out:
        return "TIMEOUT", ["child exceeded the parent watchdog; process group killed"]
    final = terminal_event(events)
    stage = final.get("stage") if final else None
    if any(e.get("stage") == "failed" for e in events) and stage != "failed":
        return "FAIL", ["a failed event precedes the terminal event"]
    if selfcheck_only:
        if (stage == "selfcheck_completed" and final.get("passed") is True
                and final.get("metal_called") is False and returncode == 0):
            return "PASS", []
        return "FAIL", ["selfcheck did not pass cleanly (stage=%r exit=%r)" % (stage, returncode)]
    if stage == "completed":
        reasons = validate_completed(kind, final, events)
        if returncode != 0:
            reasons.insert(0, "exit code %r" % returncode)
        return ("PASS" if not reasons else "FAIL"), reasons
    if stage == "failed":
        return "FAIL", ["tool failed: code=%s %s" % (final.get("code"), final.get("error"))]
    if stage in ("enumerate_only_completed", "headless_check_completed"):
        return "FAIL", ["diagnostic-only stage %s is not benchmark evidence" % stage]
    return "FAIL", ["no terminal event (stage=%r exit=%r)" % (stage, returncode)]


def summarize_samples(values):
    values = [v for v in values if isinstance(v, (int, float))]
    if not values:
        return {"count": 0}
    s = sorted(values)
    n = len(s)

    def pct(p):
        if n == 1:
            return s[0]
        rank = p / 100.0 * (n - 1)
        lo, hi = int(rank), min(int(rank) + 1, n - 1)
        return s[lo] + (s[hi] - s[lo]) * (rank - lo)
    return {"count": n, "min": s[0], "max": s[-1], "mean": sum(s) / n, "median": pct(50), "p95": pct(95)}


def extract_sample(kind, final):
    if kind == "bench":
        return {
            "device": final.get("device_name"), "registry_id": final.get("registry_id"),
            "compute_elems_per_s": ((final.get("compute") or {}).get("elements_per_second") or {}).get("median"),
            "render_mpix_per_s": ((final.get("render") or {}).get("megapixels_per_second") or {}).get("median"),
            "blit_gb_per_s": ((final.get("blit") or {}).get("gigabytes_per_second") or {}).get("median"),
        }
    return {
        "device": final.get("device_name"), "frames_presented": final.get("frames_presented"),
        "present_interval_ms_median": (final.get("present_interval_ms") or {}).get("median"),
        "present_interval_ms_p95": (final.get("present_interval_ms") or {}).get("p95"),
    }


def aggregate(runs):
    result = {}
    bench = [r for r in runs["bench"] if r.get("status") == "PASS" and "sample" in r]
    if bench:
        for key in ("compute_elems_per_s", "render_mpix_per_s", "blit_gb_per_s"):
            result[key] = summarize_samples([r["sample"].get(key) for r in bench])
    probe = [r for r in runs["probe"] if r.get("status") == "PASS" and "sample" in r]
    if probe:
        result["present_interval_ms_median"] = summarize_samples(
            [r["sample"].get("present_interval_ms_median") for r in probe])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=TOOL_DIR,
                        help="Directory holding glk-metal-bench / glk-framebuffer-probe (default: this script's dir)")
    parser.add_argument("--out", type=Path, required=True, help="New/empty output directory for the report")
    parser.add_argument("--bench-runs", type=int, default=3)
    parser.add_argument("--probe-runs", type=int, default=0, help="GUI probe runs (needs the console GUI session)")
    parser.add_argument("--run-timeout", type=float, default=150.0,
                        help="Per-run parent watchdog (s); above the tools' own watchdogs (bench 100 s, probe 90 s)")
    parser.add_argument("--total-timeout", type=float, default=1800.0, help="Parent deadline for all runs (s)")
    parser.add_argument("--registry-id", default=None, help="Explicit Metal registry ID for the tools")
    parser.add_argument("--bench-args", default="", help="Extra args for glk-metal-bench (space separated)")
    parser.add_argument("--probe-args", default="", help="Extra args for glk-framebuffer-probe (space separated)")
    parser.add_argument("--selfcheck-only", action="store_true",
                        help="Only run each tool's CPU-only --selfcheck (no Metal/GPU access)")
    args = parser.parse_args()

    if args.bench_runs < 0 or args.probe_runs < 0:
        parser.error("run counts must be non-negative")
    if not 1 <= args.run_timeout <= 3600 or not 1 <= args.total_timeout <= 86400:
        parser.error("timeouts out of range")
    out = args.out.resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error("--out must be new or empty")
    out.mkdir(parents=True, exist_ok=True)

    bench = args.build_dir / "glk-metal-bench"
    probe = args.build_dir / "glk-framebuffer-probe"
    report = {
        "schema_version": 2, "status": "FAIL", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "orchestrator_host": platform.platform(), "mode": "selfcheck_only" if args.selfcheck_only else "device",
        "build_dir": str(args.build_dir), "run_timeout": args.run_timeout, "total_timeout": args.total_timeout,
        "hardware_execution": False, "metal_tested": False, "metal_attempted": False, "clean_cleanup": True,
        "not_run_gaps": [], "failures": [], "runs": {"bench": [], "probe": []},
        "evidence_not_available": EVIDENCE_NOT_AVAILABLE,
    }
    report["artifacts"] = {label: ({"path": str(p), "sha256": sha256_file(p)} if p.is_file()
                                   else {"path": str(p), "present": False})
                           for label, p in (("glk-metal-bench", bench), ("glk-framebuffer-probe", probe))}

    def finish():
        report_path = out / "orchestrator-report.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        runs = report["runs"]
        print(json.dumps({
            "status": report["status"], "mode": report["mode"], "report": str(report_path),
            "bench": {"total": len(runs["bench"]), "pass": sum(r.get("status") == "PASS" for r in runs["bench"])},
            "probe": {"total": len(runs["probe"]), "pass": sum(r.get("status") == "PASS" for r in runs["probe"])},
            "not_run_gaps": len(report["not_run_gaps"]), "clean_cleanup": report["clean_cleanup"],
            "metal_tested": report["metal_tested"], "failures": report["failures"][:10],
        }, indent=2))
        return 0 if report["status"] == "PASS" else 2

    if args.selfcheck_only:
        report["host_gate"] = {"skipped": "selfcheck-only runs CPU-only checks; no device access"}
        report["identity"] = {"skipped": "selfcheck-only keeps the build machine build-only"}
    else:
        facts, violations = host_gate()
        report["host_gate"] = {"facts": facts, "violations": violations, "passed": not violations}
        if violations:
            report["failures"].append("not the physical target; no tool was started: " + "; ".join(violations))
            return finish()
        report["identity"] = collect_identity()

    if args.bench_runs + args.probe_runs == 0:
        report["failures"].append("no runs requested; nothing executed is not a pass")
        return finish()

    deadline = time.monotonic() + args.total_timeout

    def execute(kind, binary, extra_args, index):
        remaining = deadline - time.monotonic()
        if remaining <= 1 or not binary.is_file():
            reason = "parent deadline reached before start" if remaining <= 1 else "binary not found: %s" % binary
            report["not_run_gaps"].append({"kind": kind, "index": index, "reason": reason})
            return {"kind": kind, "index": index, "status": "NOT_RUN", "reasons": [reason]}
        command = [binary, *extra_args]
        if args.selfcheck_only:
            command.append("--selfcheck")
        elif args.registry_id:
            command += ["--registry-id", args.registry_id]
        child = run_child(command, min(args.run_timeout, remaining))
        (out / ("%s-%d.jsonl" % (kind, index))).write_text(child["stdout"], encoding="utf-8")
        (out / ("%s-%d.stderr" % (kind, index))).write_text(child["stderr"], encoding="utf-8")
        events = parse_jsonl(child["stdout"])
        final = terminal_event(events)
        status, reasons = classify_run(kind, child["returncode"], child["timed_out"], events, args.selfcheck_only)
        entry = {key: child[key] for key in ("returncode", "exit_status", "timed_out", "elapsed_seconds", "pid",
                                              "killed_process_group", "stragglers_killed",
                                              "process_group_reaped", "escaped_descendants")}
        entry.update({"kind": kind, "index": index, "command": [str(c) for c in command], "status": status,
                      "reasons": reasons, "final_stage": final.get("stage") if final else None,
                      "metal_attempted": any(e.get("stage") == "device_enumeration" for e in events),
                      "stdout_sha256": sha256_bytes(child["stdout"].encode()),
                      "stderr_tail": child["stderr"][-500:]})
        if child["timed_out"] or child["escaped_descendants"] or (final and final.get("clean_cleanup") is False):
            report["clean_cleanup"] = False  # killing a host process does NOT cancel in-flight GPU work
        if child["timed_out"]:
            report["not_run_gaps"].append({"kind": kind, "index": index, "reason": "child timeout"})
        if status == "PASS" and not args.selfcheck_only:
            entry["sample"] = extract_sample(kind, final)
        if status != "PASS":
            report["failures"].append("%s-%d %s: %s" % (kind, index, status, "; ".join(reasons)[:300]))
        return entry

    for i in range(args.bench_runs):
        report["runs"]["bench"].append(execute("bench", bench, args.bench_args.split(), i))
    for i in range(args.probe_runs):
        report["runs"]["probe"].append(execute("probe", probe, args.probe_args.split(), i))

    all_runs = report["runs"]["bench"] + report["runs"]["probe"]
    report["aggregate"] = aggregate(report["runs"])
    report["metal_attempted"] = any(r.get("metal_attempted") for r in all_runs)
    report["metal_tested"] = (not args.selfcheck_only) and any(r.get("status") == "PASS" for r in all_runs)
    report["hardware_execution"] = report["metal_tested"]
    if (all(r.get("status") == "PASS" for r in all_runs) and report["clean_cleanup"]
            and not report["not_run_gaps"]):
        report["status"] = "PASS"
    return finish()


if __name__ == "__main__":
    raise SystemExit(main())

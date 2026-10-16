#!/usr/bin/env python3
"""Prepare a reviewable, target-only staging bundle for the GLK device-lab tools.

Validates the build first (build-report.json must be PASS, every artifact's bytes must
match the report, be x86_64-only with a macOS minos <= 15.8.1, and Tools/device-lab must
be byte-identical to the sources the build recorded), then assembles: payload, full
source copy, build report, manifest, generated POSIX-sh install.sh / rollback.sh, and a
SHA256SUMS over every bundle file. The generated scripts are statically scanned (no
privileged/network/kext/boot/TCC commands, no paths outside the install root, pinned
PATH, set -eu) and syntax-checked before the bundle is declared PASS.

This tool NEVER deploys, runs ssh, installs on the developer Mac, touches
system/boot/kext/SIP/AMFI/TCC, or reboots. install.sh refuses to run anywhere but the
designated target (untranslated x86_64, Celeron N4120, macOS 15.8.1 / 24H32, exactly one
physical IOPCIDevice 8086:3185 rev 06 subsystem 17aa:3e77) and writes only under the
validated, non-system install root. Installation and rollback remain a separate,
manual, reviewed step on the target.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import time

DEVICE_LAB = Path(__file__).resolve().parents[1]
RESULTS = Path("/Users/skyhype/Documents/Codex/2026-10-15/sp/work/claude-swarm-20261016"
               "/wave7-results/glk_device_lab_author")
DEFAULT_BUILD = RESULTS / "build"
DEFAULT_PREFLIGHT = RESULTS / "target-passive-preflight.json"
# Derived from target-passive-preflight.json (console user "nyase").
DEFAULT_INSTALL_ROOT = "/Users/nyase/glk-device-lab"
TARGET = {"arch": "x86_64", "cpu": "N4120", "build": "24H32", "product": "15.8.1", "minos_max": (15, 8, 1),
          # ioreg prints IOPCIFamily OSData little-endian.
          "pci_hex": {"VID": "86800000", "DID": "85310000", "RID": "06000000",
                      "SVID": "aa170000", "SSID": "773e0000"}}
ALLOWED_DEST_ROOTS = ("/Users/", "/opt/", "/usr/local/", "/var/root/")
FORBIDDEN_DEST_PREFIXES = ("/System", "/Library", "/private", "/bin", "/sbin", "/usr/bin", "/usr/sbin",
                           "/usr/lib", "/usr/libexec", "/etc", "/var/db", "/boot", "/efi", "/EFI", "/cores",
                           "/Applications", "/Volumes")
SAFE_PATH = re.compile(r"^/[A-Za-z0-9._/-]+$")
BUILT_TOOLS = ("glk-metal-bench", "glk-framebuffer-probe")
ORCHESTRATOR = ("glk-device-lab-run", DEVICE_LAB / "orchestrate" / "run-glk-device-lab.py")
PINNED_PATH_LINE = "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
FORBIDDEN_COMMANDS = re.compile(
    r"(?<![\w.-])(sudo|su|ssh|scp|sftp|rsync|curl|wget|nc|kextload|kextunload|kextutil|kmutil|kextcache|"
    r"nvram|csrutil|bless|reboot|shutdown|halt|tccutil|spctl|launchctl|diskutil|systemsetup|pmset|"
    r"defaults|eval|exec|source|python|python3|perl|ruby|osascript|chown|chflags|xattr|codesign|"
    r"installer|softwareupdate|dd)(?![\w.-])")
RECURSIVE_RM = re.compile(r"\brm\s+-[A-Za-z]*[rR]")
ABSOLUTE_PATH = re.compile(r"(?:(?<=[\s\"'=:(>])|^)(/[A-Za-z0-9._/-]+)", re.M)
ALLOWED_SCRIPT_PATHS = ("/usr/bin", "/bin", "/usr/sbin", "/sbin", "/dev/null")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version_tuple(text):
    return tuple(int(p) for p in re.findall(r"\d+", text or "")[:3])


def tree_hashes(root: Path = DEVICE_LAB) -> dict:
    """Same rule as build-glk-device-lab.py: every file, bytecode caches excluded."""
    hashes = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc" and p.name != ".DS_Store":
            hashes[p.relative_to(root).as_posix()] = sha256_file(p)
    return hashes


def validate_dest(path: str):
    if not SAFE_PATH.match(path):
        raise ValueError("install path must be absolute and use only [A-Za-z0-9._/-]: %r" % path)
    if ".." in path.split("/") or "//" in path or path.endswith("/"):
        raise ValueError("install path must be normalized (no '..', '//' or trailing '/'): %s" % path)
    if not path.startswith(ALLOWED_DEST_ROOTS):
        raise ValueError("install path %s is not under an allowed non-system root %s" % (path, ALLOWED_DEST_ROOTS))
    if path.startswith(FORBIDDEN_DEST_PREFIXES):
        raise ValueError("install path targets a forbidden system/boot path: %s" % path)
    if len([p for p in path.split("/") if p]) < 2:
        raise ValueError("install path must be a subdirectory of an allowed root: %s" % path)


def capture(args):
    proc = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def independent_macho_check(path: Path):
    problems = []
    _, archs, _ = capture(["lipo", "-archs", path])
    if archs.split() != [TARGET["arch"]]:
        problems.append("%s arches %s are not exactly x86_64" % (path.name, archs.split()))
    _, loads, _ = capture(["otool", "-l", path])
    minos = re.findall(r"^\s*minos\s+(\S+)", loads, re.M)
    if len(minos) != 1 or not version_tuple(minos[0]) or version_tuple(minos[0]) > TARGET["minos_max"]:
        problems.append("%s minos %s does not run on macOS 15.8.1" % (path.name, minos))
    return problems


def validate_build(build_dir: Path, report_path: Path):
    if not report_path.is_file():
        return None, ["missing build report %s (run build-glk-device-lab.py first)" % report_path]
    build = json.loads(report_path.read_text())
    problems = []
    if build.get("status") != "PASS":
        problems.append("build report status is %r" % build.get("status"))
    for name in BUILT_TOOLS:
        path = build_dir / name
        art = (build.get("artifacts") or {}).get(name) or {}
        if not path.is_file():
            problems.append("missing artifact %s" % path)
            continue
        actual = sha256_file(path)
        if art.get("sha256") != actual:
            problems.append("%s sha256 %s does not match build report %s" % (name, actual, art.get("sha256")))
        if art.get("target_ok") is not True:
            problems.append("%s target metadata not validated by the build" % name)
        problems += independent_macho_check(path)
    recorded = build.get("source_tree_sha256") or {}
    current = tree_hashes()
    drift = sorted(k for k in set(recorded) | set(current) if recorded.get(k) != current.get(k))
    if not recorded:
        problems.append("build report lacks source_tree_sha256")
    elif drift:
        problems.append("Tools/device-lab changed since the build (rebuild first): %s" % drift)
    return build, problems


def code_only(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def scan_script_safety(text: str, install_root: str):
    code = code_only(text)
    violations = ["forbidden command token: %s" % m.group(1) for m in FORBIDDEN_COMMANDS.finditer(code)]
    if RECURSIVE_RM.search(code):
        violations.append("recursive rm")
    for m in ABSOLUTE_PATH.finditer(code):
        p = m.group(1)
        if p in ALLOWED_SCRIPT_PATHS or p == install_root or p.startswith(install_root + "/"):
            continue
        violations.append("absolute path outside the install root: %s" % p)
    if "set -eu" not in code.splitlines():
        violations.append("missing 'set -eu'")
    if code.splitlines().count(PINNED_PATH_LINE) != 1:
        violations.append("PATH is not pinned exactly once")
    return sorted(set(violations))


GUARD = r"""# --- Target guard (nothing is written before it passes): untranslated x86_64 Intel @CPU@,
# --- macOS @PRODUCT@ (@BUILD@), exactly one physical IOPCIDevice 8086:3185 rev 06 subsys 17aa:3e77.
[ "$(uname -m)" = "x86_64" ] || fail "arch $(uname -m) is not x86_64 (not the target)"
TRANSLATED="$(sysctl -n sysctl.proc_translated 2>/dev/null || true)"
case "$TRANSLATED" in ""|0) : ;; *) fail "translated process (sysctl.proc_translated=$TRANSLATED)" ;; esac
ARM64="$(sysctl -n hw.optional.arm64 2>/dev/null || true)"
case "$ARM64" in ""|0) : ;; *) fail "Apple silicon host (hw.optional.arm64=$ARM64)" ;; esac
CPU="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
case "$CPU" in *"@CPU@"*) : ;; *) fail "CPU '$CPU' lacks fingerprint @CPU@ (not the target)" ;; esac
OS_BUILD="$(sw_vers -buildVersion 2>/dev/null || echo unknown)"
[ "$OS_BUILD" = "@BUILD@" ] || fail "macOS build $OS_BUILD is not @BUILD@"
OS_PRODUCT="$(sw_vers -productVersion 2>/dev/null || echo unknown)"
[ "$OS_PRODUCT" = "@PRODUCT@" ] || fail "macOS $OS_PRODUCT is not @PRODUCT@"
ioreg -r -d 1 -c IOPCIDevice -l -w 0 | awk '
  /\+-o / { if (v && d && r && sv && s) n++; v = 0; d = 0; r = 0; sv = 0; s = 0 }
  /"vendor-id" = <@VID@>/ { v = 1 }
  /"device-id" = <@DID@>/ { d = 1 }
  /"revision-id" = <@RID@>/ { r = 1 }
  /"subsystem-vendor-id" = <@SVID@>/ { sv = 1 }
  /"subsystem-id" = <@SSID@>/ { s = 1 }
  END { if (v && d && r && sv && s) n++; exit (n == 1) ? 0 : 1 }' \
  || fail "exactly one physical IOPCIDevice 8086:3185 rev 06 subsystem 17aa:3e77 is required"
"""

INSTALL_TEMPLATE = r"""#!/bin/sh
# GENERATED by stage-glk-device-lab.py; review before running. Installs the GLK device-lab
# tools on the physical target only, with verified original-byte backups and atomic
# same-directory renames. Writes only under @ROOT@ and this bundle.
# Usage: sh install.sh            verify target + payload and print the plan (writes nothing)
#        sh install.sh --confirm  install
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 022
BUNDLE="$(cd "$(dirname "$0")" && pwd -P)"
STATE="$BUNDLE/install-state.tsv"
RECEIPT="$BUNDLE/install-receipt.json"
TAB="$(printf '\t')"

fail() { echo "install: $1" >&2; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }

@GUARD@
# --- Verify everything before writing anything. ---
[ ! -e "$STATE" ] || fail "install-state.tsv exists (installed or interrupted); run rollback.sh first"
[ ! -e "$RECEIPT" ] || fail "install-receipt.json exists (already installed); run rollback.sh first"
[ ! -L "@ROOT@" ] || fail "@ROOT@ is a symlink; refusing"
[ ! -L "@ROOT@/bin" ] || fail "@ROOT@/bin is a symlink; refusing"
@VERIFY@
if [ "${1:-}" != "--confirm" ]; then
  echo "install: target and payload verified; dry preview only (nothing written). Plan:"
@PLAN@
  echo "install: re-run with --confirm to install."
  exit 1
fi

# --- Install: verified backups first, then atomic same-directory renames. ---
: > "$STATE"
@INSTALL@
{
  echo "{"
  echo "  \"schema\": \"mellowgpu.glk-device-lab-install-receipt/1\","
  echo "  \"utc\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo "  \"cpu\": \"$CPU\", \"os_build\": \"$OS_BUILD\", \"os_product\": \"$OS_PRODUCT\","
  echo "  \"installed\": ["
  SEP=""
  while IFS="$TAB" read -r N D I B BS; do
    printf '%s    {"name": "%s", "dest": "%s", "installed_sha256": "%s", "backup": "%s", "backup_sha256": "%s"}' \
      "$SEP" "$N" "$D" "$I" "$B" "$BS"
    SEP=",
"
  done < "$STATE"
  echo ""
  echo "  ]"
  echo "}"
} > "$RECEIPT"
echo "install: complete; receipt $RECEIPT. Revert with: sh $BUNDLE/rollback.sh --confirm"
"""

VERIFY_ITEM = r"""# @NAME@
[ -f "$BUNDLE/payload/@NAME@" ] || fail "missing payload @NAME@"
[ "$(sha "$BUNDLE/payload/@NAME@")" = "@SHA@" ] || fail "payload @NAME@ sha256 mismatch"
[ ! -L "@DEST@" ] || fail "@DEST@ is a symlink; refusing"
[ ! -d "@DEST@" ] || fail "@DEST@ is a directory; refusing"
[ ! -e "$BUNDLE/backups/@NAME@.orig" ] || fail "backups/@NAME@.orig is in use; refusing to overwrite an original"
"""

PLAN_ITEM = """  echo "  @NAME@ (sha256 @SHA@, mode @MODE@) -> @DEST@"
"""

INSTALL_ITEM = r"""# @NAME@
mkdir -p "@DIR@"
BACKUP=none
BACKUP_SHA=none
if [ -e "@DEST@" ]; then
  BACKUP="$BUNDLE/backups/@NAME@.orig"
  cp -p "@DEST@" "$BACKUP"
  BACKUP_SHA="$(sha "$BACKUP")"
  [ "$BACKUP_SHA" = "$(sha "@DEST@")" ] || fail "backup of @DEST@ does not match the original bytes"
  echo "$BACKUP_SHA" > "$BACKUP.sha256"
fi
printf '%s\t%s\t%s\t%s\t%s\n' "@NAME@" "@DEST@" "@SHA@" "$BACKUP" "$BACKUP_SHA" >> "$STATE"
TMP="$(mktemp "@DIR@/.@NAME@.XXXXXX")"
cp "$BUNDLE/payload/@NAME@" "$TMP"
chmod @MODE@ "$TMP"
[ "$(sha "$TMP")" = "@SHA@" ] || { rm -f "$TMP"; fail "staged copy of @NAME@ does not match the payload"; }
mv -f "$TMP" "@DEST@"
[ "$(sha "@DEST@")" = "@SHA@" ] || fail "post-install sha256 mismatch for @DEST@"
echo "install: @NAME@ -> @DEST@ (backup: $BACKUP)"
"""

ROLLBACK_TEMPLATE = r"""#!/bin/sh
# GENERATED by stage-glk-device-lab.py; guarded rollback for the GLK device-lab install.
# Restores verified original bytes from backups/, or removes a file this bundle installed,
# only while that file still has the exact installed bytes (later changes are never
# clobbered). Writes only under @ROOT@ and this bundle.
# Usage: sh rollback.sh            dry preview (writes nothing)
#        sh rollback.sh --confirm  roll back
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 022
BUNDLE="$(cd "$(dirname "$0")" && pwd -P)"
STATE="$BUNDLE/install-state.tsv"
RECEIPT="$BUNDLE/install-receipt.json"
REFUSED=0

fail() { echo "rollback: $1" >&2; exit 1; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }

@GUARD@
[ -f "$STATE" ] || { echo "rollback: no install-state.tsv; nothing to roll back"; exit 0; }
if [ "${1:-}" != "--confirm" ]; then
  echo "rollback: dry preview only (nothing written). Recorded install entries:"
  cat "$STATE"
  echo "rollback: re-run with --confirm to roll back."
  exit 1
fi

revert() { # name dest installed-sha256
  awk -F '\t' -v n="$1" '$1 == n { found = 1 } END { exit found ? 0 : 1 }' "$STATE" || return 0
  BACKUP="$BUNDLE/backups/$1.orig"
  if [ -e "$2" ] && [ "$(sha "$2")" != "$3" ]; then
    echo "rollback: $2 changed since install; left untouched" >&2
    REFUSED=$((REFUSED + 1))
    return 0
  fi
  if [ -f "$BACKUP" ]; then
    WANT="$(cat "$BACKUP.sha256" 2>/dev/null || echo missing)"
    if [ "$(sha "$BACKUP")" != "$WANT" ]; then
      echo "rollback: backup of $2 fails its recorded sha256; left untouched" >&2
      REFUSED=$((REFUSED + 1))
      return 0
    fi
    TMP="$(mktemp "$(dirname "$2")/.$1.rollback.XXXXXX")"
    cp -p "$BACKUP" "$TMP"
    mv -f "$TMP" "$2"
    [ "$(sha "$2")" = "$WANT" ] || fail "restored $2 does not match its backup"
    mv -f "$BACKUP" "$BACKUP.restored"
    mv -f "$BACKUP.sha256" "$BACKUP.sha256.restored"
    echo "rollback: restored original bytes of $2"
  elif [ -e "$2" ]; then
    rm -f "$2"
    echo "rollback: removed installed $2"
  fi
}

@CALLS@
[ "$REFUSED" -eq 0 ] || fail "$REFUSED file(s) left untouched; install-state.tsv kept for review"
mv -f "$STATE" "$STATE.rolledback"
[ ! -e "$RECEIPT" ] || mv -f "$RECEIPT" "$RECEIPT.rolledback"
echo "rollback: complete."
"""


def fill(template: str, values: dict) -> str:
    for key, value in values.items():
        template = template.replace("@%s@" % key, value)
    if re.search(r"@[A-Z]+@", template):
        raise ValueError("unfilled template token: %s" % re.search(r"@[A-Z]+@", template).group(0))
    return template


def generate_scripts(files, install_root):
    guard = fill(GUARD, dict(TARGET["pci_hex"], CPU=TARGET["cpu"], BUILD=TARGET["build"],
                             PRODUCT=TARGET["product"]))
    verify = plan = install = calls = ""
    for f in files:
        item = {"NAME": f["name"], "DEST": f["dest"], "DIR": f["dest"].rsplit("/", 1)[0],
                "SHA": f["sha256"], "MODE": f["mode"]}
        verify += fill(VERIFY_ITEM, item)
        plan += fill(PLAN_ITEM, item)
        install += fill(INSTALL_ITEM, item)
        calls += 'revert "%s" "%s" "%s"\n' % (f["name"], f["dest"], f["sha256"])
    install_sh = fill(INSTALL_TEMPLATE, {"GUARD": guard, "ROOT": install_root, "VERIFY": verify,
                                         "PLAN": plan.rstrip("\n"), "INSTALL": install})
    rollback_sh = fill(ROLLBACK_TEMPLATE, {"GUARD": guard, "ROOT": install_root, "CALLS": calls})
    return install_sh, rollback_sh


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--build-report", type=Path, default=None, help="default: <build-dir>/build-report.json")
    parser.add_argument("--out", type=Path, required=True, help="New/empty bundle output directory")
    parser.add_argument("--install-root", default=DEFAULT_INSTALL_ROOT,
                        help="Target-only, non-system directory the tools install into")
    parser.add_argument("--preflight", type=Path, default=DEFAULT_PREFLIGHT)
    args = parser.parse_args()

    report = {"schema_version": 2, "status": "FAIL", "deployed": False, "ssh": False,
              "touched_system_paths": False, "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    out = args.out.resolve()
    owns_out = False
    try:
        install_root = args.install_root
        validate_dest(install_root)
        if out.exists() and (not out.is_dir() or any(out.iterdir())):
            raise ValueError("--out must be new or empty: %s" % out)
        owns_out = True
        build_report_path = args.build_report or (args.build_dir / "build-report.json")
        build, problems = validate_build(args.build_dir, build_report_path)
        report["build_validation"] = {"build_report": str(build_report_path), "problems": problems}
        if problems:
            raise ValueError("build validation failed: " + "; ".join(problems))

        sources = [(name, args.build_dir / name) for name in BUILT_TOOLS] + [ORCHESTRATOR]
        files = []
        for name, src in sources:
            dest = "%s/bin/%s" % (install_root, name)
            validate_dest(dest)
            files.append({"name": name, "source": str(src), "dest": dest, "sha256": sha256_file(src),
                          "mode": "0755"})
        install_sh, rollback_sh = generate_scripts(files, install_root)
        safety = {name: scan_script_safety(text, install_root)
                  for name, text in (("install.sh", install_sh), ("rollback.sh", rollback_sh))}
        if any(safety.values()):
            raise ValueError("generated script safety violations: %s" % safety)

        for sub in ("payload", "backups", "source"):
            (out / sub).mkdir(parents=True, exist_ok=True)
        for f in files:
            shutil.copyfile(f["source"], out / "payload" / f["name"])
            (out / "payload" / f["name"]).chmod(0o755)
        for p in sorted(DEVICE_LAB.rglob("*")):
            if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc" and p.name != ".DS_Store":
                target = out / "source" / p.relative_to(DEVICE_LAB)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, target)
        shutil.copyfile(build_report_path, out / "build-report.json")
        (out / "backups" / "README.txt").write_text(
            "install.sh backs up any pre-existing destination file here (NAME.orig + NAME.orig.sha256),\n"
            "verified against the original bytes, before overwriting it. rollback.sh restores from here.\n",
            encoding="utf-8")
        for name, text in (("install.sh", install_sh), ("rollback.sh", rollback_sh)):
            path = out / name
            path.write_text(text, encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        syntax = {name: capture(["/bin/sh", "-n", out / name]) for name in ("install.sh", "rollback.sh")}
        syntax_ok = all(rc == 0 for rc, _, _ in syntax.values())

        consistent = all(sha256_file(out / "payload" / f["name"]) == f["sha256"] for f in files)
        manifest = {
            "schema": "mellowgpu.glk-device-lab-staging/2", "utc": report["utc"], "deployed": False,
            "note": "candidate bundle; install/rollback is a separate reviewed step on the target",
            "target": {"expected_pci": "8086:3185 rev 06 subsystem 17aa:3e77 at 00:02.0",
                       "expected_arch": TARGET["arch"], "expected_build": TARGET["build"],
                       "expected_product": TARGET["product"], "cpu_fingerprint": TARGET["cpu"],
                       "preflight_source": str(args.preflight),
                       "preflight_sha256": sha256_file(args.preflight) if args.preflight.is_file() else None},
            "install_root": install_root, "allowed_dest_roots": list(ALLOWED_DEST_ROOTS),
            "files": [{k: f[k] for k in ("name", "dest", "sha256", "mode")} for f in files],
            "build_report_sha256": sha256_file(build_report_path),
            "build_source_tree_digest": build.get("source_tree_digest"),
            "artifact_metadata": {name: {k: build["artifacts"][name].get(k)
                                         for k in ("sha256", "arches", "uuids", "cdhash", "build_versions",
                                                   "codesign")}
                                  for name in BUILT_TOOLS},
            "scripts": {name: sha256_file(out / name) for name in ("install.sh", "rollback.sh")},
            "script_safety": {"violations": safety, "sh_syntax_ok": syntax_ok},
            "guards": [
                "install.sh/rollback.sh refuse unless untranslated x86_64, not Apple silicon, CPU N4120, "
                "macOS 15.8.1 (24H32) and exactly one IOPCIDevice 8086:3185 rev 06 subsys 17aa:3e77",
                "all payload hashes verified before any write; refuses symlink/directory destinations",
                "verified original-byte backups before overwrite; atomic same-directory renames",
                "re-install refused while install-state exists; backup slots are never overwritten",
                "rollback restores verified backups or removes only files still byte-identical to the payload",
                "PATH pinned; no sudo/ssh/network/kext/nvram/csrutil/bless/reboot/tccutil/launchctl commands",
            ],
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        report.update({"bundle": str(out), "files": [f["name"] for f in files], "payload_consistent": consistent,
                       "install_root": install_root, "script_safety": safety, "sh_syntax_ok": syntax_ok,
                       "build_report_sha256": manifest["build_report_sha256"],
                       "build_source_tree_digest": manifest["build_source_tree_digest"]})
        if not syntax_ok:
            raise ValueError("generated scripts fail sh -n: %s" % {k: v[2] for k, v in syntax.items()})
        if consistent:
            report["status"] = "PASS"
    except (ValueError, OSError, json.JSONDecodeError) as error:
        report["error"] = "%s: %s" % (type(error).__name__, error)
        for name in ("install.sh", "rollback.sh"):  # never leave runnable scripts in a failed bundle
            if owns_out and (out / name).is_file():
                (out / name).unlink()

    if report.get("bundle"):
        (out / "staging-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if report["status"] == "PASS":
            sums = "".join("%s  %s\n" % (sha256_file(p), p.relative_to(out).as_posix())
                           for p in sorted(out.rglob("*")) if p.is_file())
            (out / "SHA256SUMS").write_text(sums, encoding="utf-8")
            report["sha256sums_sha256"] = sha256_file(out / "SHA256SUMS")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())

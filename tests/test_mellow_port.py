"""Artifact and rejection tests; no compiler/GPU success is inferred."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stderr

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Tools"))
from mellow_port import PortError, available_targets, prepare
from mellow_port.core import FAMILIES_PATH, RECIPES_PATH, inventory, lexical

REVISION = "4d7d9486c04d917265f64c55bd23b2cc4fe7749c"
FILE = "drivers/gpu/drm/xe/regs/test_regs.h"
SOURCE = """/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Fixture Authors */
#include <linux/types.h>
#define TEST_REG 0x1234U
#define CONDITIONAL_REG (BASE + 4)
#define BAD_OCTAL 012
#define FUNCTION(x) ((x) + 1)
#define TOO_WIDE 0x10000000000000000
#define BAD_SUFFIX 0x123uLl
static int submit(void) { return linux_submit(); }
"""


class PortTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "linux"
        target = self.source / FILE
        target.parent.mkdir(parents=True)
        target.write_text(SOURCE, encoding="utf-8")

    def run_port(self, output="result", **kwargs):
        arguments = {"command": "generate", "source_root": self.source, "target": "xe", "revision": REVISION, "files": [FILE], "output": self.root / output}
        arguments.update(kwargs)
        return prepare(**arguments)

    def test_generation_is_deterministic_and_bounded(self):
        self.assertEqual(self.run_port("one"), self.run_port("two"))
        trees = [{p.relative_to(self.root / folder).as_posix(): p.read_bytes() for p in (self.root / folder).rglob("*") if p.is_file()} for folder in ("one", "two")]
        self.assertEqual(*trees)
        tree = trees[0]
        header = next(v.decode() for k, v in tree.items() if k.endswith(".h"))
        for expected in ("0x1234U", "SPDX-License-Identifier: MIT", "Copyright 2026 Fixture Authors"):
            self.assertIn(expected, header)
        for forbidden in ("linux_submit", "BAD_OCTAL", "CONDITIONAL_REG", "TOO_WIDE", "BAD_SUFFIX"):
            self.assertNotIn(forbidden, header)
        self.assertIn("FATAL_ERROR", tree["CMakeLists.txt"].decode())
        manifest = json.loads(tree["source-manifest.json"])
        self.assertEqual(manifest["files"][0]["sha256"], hashlib.sha256((self.source / FILE).read_bytes()).hexdigest())
        self.assertFalse(manifest["revision_membership_verified"])
        inventory = json.loads(tree["inventory.json"])["files"][0]
        self.assertIn("linux/types.h", inventory["includes_lexical"])
        self.assertIn("linux_submit", inventory["call_tokens_approximate"])
        gaps = json.loads(tree["gap-report.json"])
        self.assertFalse(gaps["driver_ready"])
        self.assertTrue(any(item["id"].startswith("functions:") for item in gaps["gaps"]))

    def test_ready_gate_fails_after_emitting_review_artifacts(self):
        result = self.run_port(require_ready=True)
        self.assertEqual(result["exit_code"], 2)
        self.assertFalse(result["compile_performed"])
        self.assertFalse(result["hardware_test_performed"])
        self.assertTrue((self.root / "result/gap-report.json").exists())

    def test_comments_preserve_token_boundaries_and_source_lines(self):
        source = "#define JOINED 1/**/2\n/* first\nsecond */\n#define REAL 0x20U\n#define WITH_NOTE 7 /* note */\n"
        data = inventory(FILE, source)
        self.assertEqual([(item["name"], item["literal"], item["line"]) for item in data["simple_integer_defines"]], [("REAL", "0x20U", 4), ("WITH_NOTE", "7", 5)])
        self.assertIn("JOINED", [item["name"] for item in data["unconverted_macros"]])
        tokens = lexical("one/**/two\n/* a\nb */ three()")
        self.assertIn("one two", tokens)
        self.assertEqual(tokens.count("\n"), 2)

    def test_equal_named_identical_headers_keep_distinct_provenance(self):
        other = "drivers/gpu/drm/xe/abi/test_regs.h"
        path = self.source / other
        path.parent.mkdir(parents=True)
        path.write_bytes((self.source / FILE).read_bytes())
        self.run_port(files=[FILE, other])
        backend = json.loads((self.root / "result/backend.json").read_text())
        exports = backend["review_headers"]
        self.assertEqual(len(exports), 2)
        self.assertEqual(len({item["path"] for item in exports}), 2)
        self.assertEqual({item["source"] for item in exports}, {FILE, other})
        for item in exports:
            content = (self.root / "result" / item["path"]).read_text()
            self.assertIn(" * Source: " + item["source"] + "\n", content)

    def test_reject_binary_and_path_traversal(self):
        for path in ("../escape.h", "/absolute.h", "C:/outside.h", FILE + ":stream.h", "drivers/gpu/drm/xe/driver.ko", "drivers/gpu/drm/xe/driver.run"):
            with self.subTest(path=path), self.assertRaises(PortError):
                self.run_port(files=[path])
        (self.source / FILE).write_bytes(b"\x7fELF\0binary")
        with self.assertRaises(PortError):
            self.run_port()

    def test_recipe_requires_explicit_backend_source(self):
        with self.assertRaises(PortError):
            self.run_port(target="amdgpu")
        with self.assertRaises(PortError):
            self.run_port(target="rx9070")
        with self.assertRaises(PortError):
            self.run_port(revision="main")
        for target, relative in (("amdgpu", "drivers/gpu/drm/amd/amdgpu/fixture.h"), ("nvidia-open", "kernel-open/nvidia/fixture.h")):
            path = self.source / relative
            path.parent.mkdir(parents=True)
            path.write_text(SOURCE, encoding="utf-8")
            self.run_port(target, target=target, files=[relative])
            self.assertEqual(json.loads((self.root / target / "backend.json").read_text())["pci_device_ids"], [])

    def make_source(self, relative):
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(SOURCE, encoding="utf-8")
        return relative

    def test_target_choices_follow_recipe_registry(self):
        recipes = json.loads(RECIPES_PATH.read_bytes())["recipes"]
        self.assertEqual(set(available_targets()), set(recipes))
        self.assertTrue({"xe", "amdgpu", "nvidia-open", "i915", "nouveau"} <= set(recipes))

    def test_glk_reference_routes_to_i915_without_runtime_admission(self):
        families = json.loads(FAMILIES_PATH.read_bytes())["families"]
        recipes = json.loads(RECIPES_PATH.read_bytes())["recipes"]
        profile = families["intel-glk"]
        self.assertEqual(profile["source_targets"], ["i915"])
        self.assertFalse(profile["runtime_device_admission"])
        self.assertFalse(profile["physical_gpu_verified"])
        self.assertIn("intel-glk", recipes["i915"]["architectures"])
        self.assertNotIn("intel-glk", recipes["xe"]["architectures"])
        relative = self.make_source("drivers/gpu/drm/i915/fixture.h")
        result = self.run_port("glk-reference", target="i915", files=[relative],
                               gpu_family="intel-glk", require_ready=True)
        self.assertEqual(result["exit_code"], 2)
        self.assertFalse(result["driver_ready"])
        self.assertFalse(result["compile_performed"])
        self.assertFalse(result["hardware_test_performed"])
        backend = json.loads((self.root / "glk-reference/backend.json").read_bytes())
        self.assertTrue(backend["adapter_contract"]["source_profile_compatible"])
        self.assertFalse(backend["adapter_contract"]["runtime_device_admitted"])
        self.assertFalse(backend["adapter_contract"]["physical_gpu_verified"])
        self.assertEqual(backend["pci_device_ids"], [])
        self.assertEqual(backend["implemented_entry_points"], [])
        with self.assertRaises(PortError):
            self.run_port("glk-xe", target="xe", gpu_family="intel-glk")
        self.assertFalse((self.root / "glk-xe").exists())

    def test_i915_and_nak_observations_do_not_admit_a_build_closure(self):
        lock = json.loads((REPO / "porting/upstreams.lock.json").read_bytes())
        components = {item["id"]: item for item in lock["components"]}
        expected = {
            "linux-i915": ("0d9ff90a5422cc7509258aaaba1e7481df4d332a", {
                "drivers/gpu/drm/i915/i915_driver.c",
                "drivers/gpu/drm/i915/i915_pci.c", "include/drm/intel/pciids.h"}),
            "mesa-nak": ("f5cb8ee032adabef599ae892ec4e42d796b84da9", {
                "src/nouveau/compiler/nak/lib.rs", "src/nouveau/compiler/nak/api.rs"}),
        }
        for name, (commit, paths) in expected.items():
            with self.subTest(component=name):
                component = components[name]
                self.assertEqual(component["commit"], commit)
                self.assertEqual({item["path"] for item in component["files"]}, paths)
                self.assertEqual(component["role"], "reference-only")
                self.assertEqual(component["profile"], "reference-only")
                self.assertTrue(all(path.startswith(("docs/", "porting/"))
                                    for path in component["consumers"]))
                self.assertEqual(component["license_review"], {
                    "status": "unreviewed", "evidence_sha256": None})
                self.assertEqual(component["configuration"], [])
                self.assertEqual(component["patches"], [])
                self.assertEqual(component["abi"]["checks"], [])
        self.assertIn("include/drm/intel/pciids.h", components["linux-i915"]["abi"]["headers"])

    def test_all_family_source_pairs_keep_hardware_admission_closed(self):
        profiles = json.loads(FAMILIES_PATH.read_bytes())["families"]
        source_paths = {
            "i915": "drivers/gpu/drm/i915/fixture.h",
            "xe": FILE,
            "nouveau": "drivers/gpu/drm/nouveau/fixture.h",
            "nvidia-open": "kernel-open/nvidia/fixture.h",
        }
        for family, profile in profiles.items():
            for target in profile["source_targets"]:
                with self.subTest(family=family, target=target):
                    relative = self.make_source(source_paths[target])
                    output = family + "-" + target
                    result = self.run_port(output, target=target, files=[relative], gpu_family=family, require_ready=True)
                    self.assertEqual(result["exit_code"], 2)
                    self.assertFalse(result["driver_ready"])
                    self.assertFalse(result["hardware_test_performed"])
                    manifest = json.loads((self.root / output / "source-manifest.json").read_bytes())
                    contract = manifest["adapter_contract"]
                    self.assertEqual(contract["gpu_family"], family)
                    self.assertEqual(contract["registry_sha256"], hashlib.sha256(FAMILIES_PATH.read_bytes()).hexdigest())
                    self.assertTrue(contract["source_profile_compatible"])
                    self.assertFalse(contract["runtime_device_admitted"])
                    self.assertFalse(contract["physical_gpu_verified"])
                    backend = json.loads((self.root / output / "backend.json").read_bytes())
                    self.assertEqual(backend["adapter_contract"], contract)
                    self.assertEqual(backend["implemented_entry_points"], [])
                    self.assertEqual(backend["pci_device_ids"], [])
                    gaps = json.loads((self.root / output / "gap-report.json").read_bytes())["gaps"]
                    self.assertTrue({"adapter-firmware", "adapter-userspace", "adapter-source-provenance", "family-admission"} <= {gap["id"] for gap in gaps})

    def test_wrong_vendor_and_pre_turing_rm_rejected_before_output(self):
        pairs = [("nvidia-open", family) for family in ("nvidia-maxwell", "nvidia-pascal", "nvidia-volta")]
        pairs += [("xe", "nvidia-ampere"), ("nouveau", "intel-tgl"), ("xe", "intel-icl"), ("i915", "intel-lnl"), ("xe", "invented-family")]
        for target, family in pairs:
            with self.subTest(target=target, family=family):
                destination = self.root / (target + "-" + family)
                with self.assertRaises(PortError):
                    self.run_port(destination, target=target, gpu_family=family)
                self.assertFalse(destination.exists())

    def test_rm_and_nouveau_sources_cannot_be_mixed(self):
        rm = self.make_source("kernel-open/nvidia/fixture.h")
        nouveau = self.make_source("drivers/gpu/drm/nouveau/fixture.h")
        for target in ("nvidia-open", "nouveau"):
            with self.subTest(target=target), self.assertRaises(PortError):
                self.run_port(target, target=target, files=[rm, nouveau], gpu_family="nvidia-ampere")
            self.assertFalse((self.root / target).exists())

    def test_family_plans_are_deterministic_and_do_not_manufacture_verification(self):
        relative = self.make_source("drivers/gpu/drm/nouveau/fixture.h")
        for output in ("family-one", "family-two"):
            self.run_port(output, target="nouveau", files=[relative], gpu_family="nvidia-maxwell")
        files = lambda folder: {p.relative_to(folder).as_posix(): p.read_bytes() for p in folder.rglob("*") if p.is_file()}
        self.assertEqual(files(self.root / "family-one"), files(self.root / "family-two"))
        manifest = json.loads((self.root / "family-one/source-manifest.json").read_bytes())
        self.assertFalse(manifest["revision_membership_verified"])
        self.assertFalse(manifest["adapter_contract"]["physical_gpu_verified"])

    def test_unspecified_family_is_not_inferred_from_filename_or_source(self):
        relative = self.make_source("kernel-open/nvidia/maxwell.h")
        self.run_port("unspecified", target="nvidia-open", files=[relative])
        contract = json.loads((self.root / "unspecified/plan.json").read_bytes())["adapter_contract"]
        self.assertIsNone(contract["gpu_family"])
        self.assertIsNone(contract["source_profile_compatible"])
        self.assertFalse(contract["runtime_device_admitted"])

    def test_cli_accepts_new_target_and_preserves_not_ready_exit(self):
        relative = self.make_source("drivers/gpu/drm/nouveau/fixture.h")
        result = subprocess.run([sys.executable, str(REPO / "Tools/mellow-port.py"), "plan", "--source-root", str(self.source), "--target", "nouveau", "--gpu-family", "nvidia-maxwell", "--revision", REVISION, "--file", relative, "--output", str(self.root / "nouveau-cli"), "--require-ready"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(json.loads(result.stdout)["driver_ready"])

    def test_malformed_family_registry_never_emits_compatible_output(self):
        mutations = [
            ("schema_version", True),
            ("source_targets", "nouveau-extra"),
            ("source_targets", None),
            ("source_targets", [None]),
            ("runtime_device_admission", True),
            ("physical_gpu_verified", True),
            ("vendor", "Intel"),
        ]
        relative = self.make_source("drivers/gpu/drm/nouveau/fixture.h")
        for index, (field, value) in enumerate(mutations):
            with self.subTest(field=field, value=value):
                document = json.loads(FAMILIES_PATH.read_bytes())
                (document if field == "schema_version" else document["families"]["nvidia-maxwell"])[field] = value
                registry_file = self.root / "family-registry.json"
                registry_file.write_text(json.dumps(document), encoding="utf-8")
                output = self.root / ("malformed-family-" + str(index))
                with patch("mellow_port.core.FAMILIES_PATH", registry_file), self.assertRaises(PortError):
                    self.run_port(output, target="nouveau", files=[relative], gpu_family="nvidia-maxwell")
                self.assertFalse(output.exists())

    def test_invalid_recipe_registries_reject_without_cli_traceback(self):
        spec = importlib.util.spec_from_file_location("mellow_port_cli_tests", REPO / "Tools/mellow-port.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        valid = json.loads(RECIPES_PATH.read_bytes())
        documents = [None, {"schema_version": True, "recipes": valid["recipes"]}, {"schema_version": 1, "recipes": None}]
        invalid_requirements = json.loads(RECIPES_PATH.read_bytes())
        invalid_requirements["recipes"]["xe"]["requirements"]["firmware"] = "not a list"
        documents.append(invalid_requirements)
        for index, content in enumerate(["{invalid-json", *[json.dumps(document) for document in documents], None]):
            with self.subTest(index=index):
                registry_file = self.root / ("recipes-" + str(index) + ".json")
                if content is not None:
                    registry_file.write_text(content, encoding="utf-8")
                stderr = io.StringIO()
                with patch("mellow_port.core.RECIPES_PATH", registry_file), redirect_stderr(stderr):
                    self.assertEqual(cli.main(["--help"]), 1)
                rejection = json.loads(stderr.getvalue())
                self.assertFalse(rejection["artifacts_generated"])
                self.assertFalse(rejection["driver_ready"])
                self.assertIn("error", rejection)

    def test_duplicate_registry_keys_rejected_before_output(self):
        recipes = RECIPES_PATH.read_text(encoding="utf-8")
        families = FAMILIES_PATH.read_text(encoding="utf-8")
        cases = [
            ("RECIPES_PATH", recipes.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1', 1)),
            ("RECIPES_PATH", recipes.replace('"vendor": "Intel"', '"vendor": "Intel", "vendor": "Intel"', 1)),
            ("FAMILIES_PATH", families.replace('"physical_gpu_verified": false',
                '"physical_gpu_verified": true, "physical_gpu_verified": false', 1)),
        ]
        for index, (field, raw) in enumerate(cases):
            with self.subTest(field=field, index=index):
                registry_file = self.root / ("duplicate-" + str(index) + ".json")
                registry_file.write_text(raw, encoding="utf-8")
                output = self.root / ("duplicate-output-" + str(index))
                with patch("mellow_port.core." + field, registry_file), self.assertRaisesRegex(PortError, "Duplicate JSON"):
                    self.run_port(output, gpu_family="intel-tgl")
                self.assertFalse(output.exists())

    def test_nonfinite_registry_metadata_rejected_before_output(self):
        for field, original, collection, entry in (
                ("RECIPES_PATH", RECIPES_PATH, "recipes", "xe"),
                ("FAMILIES_PATH", FAMILIES_PATH, "families", "intel-tgl")):
            document = json.loads(original.read_bytes())
            document[collection][entry]["unreviewed_metadata"] = 0
            valid = json.dumps(document)
            for index, literal in enumerate(("NaN", "Infinity", "-Infinity", "1e999", "-1e999")):
                with self.subTest(field=field, literal=literal):
                    registry_file = self.root / (field + "-nonfinite-" + str(index) + ".json")
                    registry_file.write_text(valid.replace('"unreviewed_metadata": 0',
                        '"unreviewed_metadata": ' + literal, 1), encoding="utf-8")
                    output = self.root / (field + "-nonfinite-output-" + str(index))
                    with patch("mellow_port.core." + field, registry_file), self.assertRaisesRegex(PortError, "Nonfinite JSON"):
                        self.run_port(output, gpu_family="intel-tgl")
                    self.assertFalse(output.exists())
            # Ordinary finite numbers and these words inside strings remain
            # valid JSON metadata; rejecting numeric tokens is not text filtering.
            document[collection][entry]["unreviewed_metadata"] = {"finite": 0.125, "text": "NaN Infinity"}
            registry_file = self.root / (field + "-finite.json")
            registry_file.write_text(json.dumps(document), encoding="utf-8")
            with patch("mellow_port.core." + field, registry_file):
                self.assertFalse(self.run_port(field + "-finite-output", gpu_family="intel-tgl")["driver_ready"])

    def test_recipe_prefixes_require_canonical_relative_directories(self):
        malformed = ("", "drivers/gpu/drm/xe", "/drivers/gpu/drm/xe/", "C:/drivers/",
                     "../drivers/", "drivers/../xe/", "drivers/./xe/", "drivers//xe/",
                     "drivers\\gpu\\xe\\", "./drivers/", "drivers/gpu/drm/xe//")
        for field in ("source_prefixes", "admission_prefixes"):
            for index, prefix in enumerate(malformed):
                with self.subTest(field=field, prefix=prefix):
                    document = json.loads(RECIPES_PATH.read_bytes())
                    document["recipes"]["xe"][field] = [prefix]
                    registry_file = self.root / (field + "-prefix-" + str(index) + ".json")
                    registry_file.write_text(json.dumps(document), encoding="utf-8")
                    output = self.root / (field + "-prefix-output-" + str(index))
                    with patch("mellow_port.core.RECIPES_PATH", registry_file), self.assertRaises(PortError):
                        self.run_port(output)
                    self.assertFalse(output.exists())

    def test_admission_prefixes_must_stay_in_source_directories(self):
        for index, prefix in enumerate(("drivers/gpu/drm/xe-evil/", "drivers/gpu/drm/", "kernel-open/", "include/drm-evil/")):
            with self.subTest(prefix=prefix):
                document = json.loads(RECIPES_PATH.read_bytes())
                document["recipes"]["xe"]["admission_prefixes"] = [prefix]
                registry_file = self.root / ("outside-admission-" + str(index) + ".json")
                registry_file.write_text(json.dumps(document), encoding="utf-8")
                output = self.root / ("outside-admission-output-" + str(index))
                with patch("mellow_port.core.RECIPES_PATH", registry_file), self.assertRaisesRegex(PortError, "Admission prefix"):
                    self.run_port(output)
                self.assertFalse(output.exists())
        document = json.loads(RECIPES_PATH.read_bytes())
        document["recipes"]["xe"]["admission_prefixes"] = ["drivers/gpu/drm/xe/regs/"]
        registry_file = self.root / "nested-admission.json"
        registry_file.write_text(json.dumps(document), encoding="utf-8")
        with patch("mellow_port.core.RECIPES_PATH", registry_file):
            self.assertFalse(self.run_port("nested-admission-output")["driver_ready"])
        sibling = self.make_source("drivers/gpu/drm/xe-evil/fixture.h")
        with self.assertRaises(PortError):
            self.run_port("sibling-source-output", files=[sibling])
        self.assertFalse((self.root / "sibling-source-output").exists())

    def test_output_never_overwrites_or_modifies_source(self):
        before = (self.source / FILE).read_bytes()
        with self.assertRaises(PortError):
            self.run_port(output=self.source / "generated")
        self.run_port()
        with self.assertRaises(PortError):
            self.run_port()
        self.assertEqual((self.source / FILE).read_bytes(), before)

    def test_symlink_not_admitted(self):
        link = self.source / "drivers/gpu/drm/xe/linked.h"
        try:
            link.symlink_to(self.source / FILE)
        except (OSError, NotImplementedError):
            self.skipTest("OS does not permit creating test symlink")
        with self.assertRaises(PortError):
            self.run_port(files=["drivers/gpu/drm/xe/linked.h"])

    def test_missing_and_gpl_license_are_not_approved(self):
        (self.source / FILE).write_text(SOURCE.replace("SPDX-License-Identifier: MIT", "SPDX-License-Identifier: GPL-2.0-only OR MIT"), encoding="utf-8")
        self.run_port("gpl")
        facts = json.loads((self.root / "gpl/source-manifest.json").read_text())["files"][0]["license"]
        self.assertTrue(facts["gpl_component_review_required"])
        self.assertFalse(facts["license_compatibility_determined"])
        (self.source / FILE).write_text("#define REG 0x10\n", encoding="utf-8")
        self.run_port("unknown")
        self.assertFalse((self.root / "unknown/generated").exists())

    def test_cli_exit_and_does_not_execute_source(self):
        (self.source / "RUN-ME.sh").write_text("touch SHOULD-NOT-EXIST\n", encoding="utf-8")
        result = subprocess.run([sys.executable, str(REPO / "Tools/mellow-port.py"), "plan", "--source-root", str(self.source), "--target", "xe", "--revision", REVISION, "--file", FILE, "--output", str(self.root / "cli"), "--require-ready"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(json.loads(result.stdout)["artifacts_generated"])
        self.assertFalse((self.source / "SHOULD-NOT-EXIST").exists())

    def test_optional_real_pinned_xe_subset(self):
        capture = REPO.parent / "xe-submission-primary/drivers_gpu_drm_xe_abi_guc_klvs_abi.h"
        if not capture.exists():
            self.skipTest("local upstream capture unavailable")
        data = capture.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), "b52b164a997275d3a9e99189844fea6d25913c060260a21ee8b6252615c86613")
        relative = "drivers/gpu/drm/xe/abi/guc_klvs_abi.h"
        path = self.source / relative
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        self.run_port("upstream", files=[relative])
        manifest = json.loads((self.root / "upstream/source-manifest.json").read_text())
        self.assertEqual(manifest["files"][0]["license"]["single_expression"], "MIT")
        self.assertTrue(list((self.root / "upstream/generated").glob("*.h")))


if __name__ == "__main__":
    unittest.main()

"""Real Git/source/patch candidate staging and failure-boundary regressions."""
# SPDX-License-Identifier: MIT
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools'))
try:
    from mellow_port import candidate as c
except ImportError:
    c = None


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(c, 'RED: immutable candidate staging is not implemented')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.src = self.root / 'upstream'; self.src.mkdir()
        self.project = self.root / 'project'; self.project.mkdir()
        for repo in (self.src, self.project): self.git(repo, 'init', '-q')
        self.name = 'drivers/test.c'
        self.original = b'/* SPDX-License-Identifier: MIT */\n#define ABI_VERSION 3u\nint value(void) { return 1; }\n'
        self.put(self.src / self.name, self.original)
        self.put(self.src / 'README.md', b'ABI version: 3\n')
        self.commit(self.src)
        self.revision = self.git(self.src, 'rev-parse', 'HEAD').strip()
        self.put(self.src / self.name, self.original.replace(b'return 1', b'return 2'))
        self.patch_bytes = self.git(self.src, 'diff', '--full-index').encode()
        self.git(self.src, 'checkout', '--', self.name)
        self.patch_path = 'patches/0001-test.patch'
        self.put(self.project / self.patch_path, self.patch_bytes)
        self.put(self.project / 'config/options.json', b'{"feature":true}\n')
        self.commit(self.project)
        self.lock = {'schema_version': 1, 'specifications': [], 'components': [{
            'id': 'fixture', 'role': 'build-dependency', 'remote': 'https://github.com/example/fixture',
            'commit': self.revision, 'tracking_ref': 'refs/heads/main', 'profile': 'darwin-permissive',
            'consumers': ['Drivers/Core/Test.cpp'], 'files': [self.record(self.name, 'source'), self.record('README.md', 'specification')],
            'patches': [{'path': self.patch_path, 'sha256': self.sha(self.patch_bytes)}],
            'configuration': [{'path': 'config/options.json', 'sha256': self.sha((self.project / 'config/options.json').read_bytes())}],
            'license_review': {'status': 'unreviewed', 'evidence_sha256': None},
            'abi': {'family': 'fixture-v3', 'headers': [self.name], 'checks': [
                {'header': self.name, 'symbol': 'ABI_VERSION', 'document': 'README.md', 'marker': 'ABI version:', 'expected': 3}]}}]}
        self.lock_path = self.root / 'input.lock.json'
        self.save_lock()

    @staticmethod
    def sha(data): return hashlib.sha256(data).hexdigest()

    @staticmethod
    def put(path, data):
        path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)

    def git(self, repo, *args):
        env = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                   GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
                   GIT_AUTHOR_DATE='2026-01-01T00:00:00+0000', GIT_COMMITTER_DATE='2026-01-01T00:00:00+0000')
        return subprocess.run(['git', '-C', str(repo), *args], env=env, check=True, capture_output=True, text=True).stdout

    def commit(self, repo):
        self.git(repo, 'add', '.'); self.git(repo, 'commit', '-qm', 'fixture')

    def record(self, path, kind):
        data = (self.src / path).read_bytes()
        return {'path': path, 'kind': kind, 'git_blob': self.git(self.src, 'rev-parse', self.revision + ':' + path).strip(),
                'sha256': self.sha(data), 'size_bytes': len(data), 'spdx': 'MIT',
                'source_uri': 'https://github.com/example/fixture/blob/' + self.revision + '/' + path}

    def save_lock(self): self.lock_path.write_text(json.dumps(self.lock))

    def stage(self, name='candidate'):
        return c.stage_candidate(self.lock_path, 'fixture', self.src, self.root / name, self.project)

    def set_patch(self, data):
        self.put(self.project / self.patch_path, data); self.commit(self.project)
        self.lock['components'][0]['patches'][0]['sha256'] = self.sha(data); self.save_lock()

    def test_real_exact_patch_bundle_keeps_originals_and_is_never_build_ready(self):
        lock_before = self.lock_path.read_bytes(); result = self.stage()
        out = self.root / 'candidate'
        self.assertEqual((out / 'source-raw' / self.name).read_bytes(), self.original)
        self.assertEqual((out / 'source-patched' / self.name).read_bytes(), self.original.replace(b'return 1', b'return 2'))
        self.assertEqual((self.src / self.name).read_bytes(), self.original)
        self.assertEqual((out / 'project-inputs' / self.patch_path).read_bytes(), self.patch_bytes)
        self.assertEqual((out / 'project-inputs/config/options.json').read_bytes(), b'{"feature":true}\n')
        self.assertEqual(self.lock_path.read_bytes(), lock_before)
        self.assertTrue(result['patches_applied_exactly'])
        self.assertFalse(result['build_eligible']); self.assertFalse(result['ready'])
        self.assertFalse(result['hardware_qualified']); self.assertFalse(result['production_promotion_available'])
        self.assertEqual(result['license_review']['status'], 'unreviewed')
        self.assertTrue((out / 'intake-result.json').is_file())
        self.assertFalse((out / 'failed-intake.json').exists())

    def test_zero_patch_and_config_current_lock_shape_is_evidence_only(self):
        self.lock['components'][0]['patches'] = []; self.lock['components'][0]['configuration'] = []; self.save_lock()
        result = self.stage()
        self.assertEqual(result['patches'], [])
        self.assertFalse(result['build_eligible'])
        self.assertEqual((self.root / 'candidate/source-patched' / self.name).read_bytes(), self.original)

    def test_reference_role_and_review_flag_cannot_grant_build_eligibility(self):
        entry = self.lock['components'][0]; entry['role'] = 'reference-only'; entry['profile'] = 'reference-only'; entry['consumers'] = ['docs/REFERENCE.md']; self.save_lock()
        with self.assertRaisesRegex(c.PortError, 'reference-only'): self.stage()
        self.assertFalse((self.root / 'candidate').exists())
        entry['role'] = 'build-dependency'; entry['profile'] = 'darwin-permissive'; entry['license_review'] = {'status': 'reviewed', 'evidence_sha256': 'a' * 64}; self.save_lock()
        self.assertFalse(self.stage()['build_eligible'])

    def test_git_ignored_extra_inputs_are_not_copied(self):
        self.put(self.project / '.gitignore', b'config/foreign.txt\n'); self.commit(self.project)
        self.put(self.project / 'config/foreign.txt', b'private ignored data')
        self.stage()
        self.assertFalse((self.root / 'candidate/project-inputs/config/foreign.txt').exists())

    def test_patch_and_config_must_match_project_head_not_only_updated_lock_hash(self):
        for field, path, data in [('patches', self.patch_path, self.patch_bytes.replace(b'return 2', b'return 9')),
                                  ('configuration', 'config/options.json', b'{"feature":false}\n')]:
            with self.subTest(field=field):
                original = (self.project / path).read_bytes()
                self.put(self.project / path, data); self.lock['components'][0][field][0]['sha256'] = self.sha(data); self.save_lock()
                with self.assertRaisesRegex(c.PortError, 'project|Project'): self.stage(field)
                self.assertFalse((self.root / field).exists())
                self.put(self.project / path, original); self.lock['components'][0][field][0]['sha256'] = self.sha(original); self.save_lock()

    def test_changed_upstream_and_duplicate_lock_rejected_before_output(self):
        self.put(self.src / self.name, self.original + b'changed\n')
        with self.assertRaises(c.PortError): self.stage()
        self.assertFalse((self.root / 'candidate').exists())
        self.put(self.src / self.name, self.original)
        self.lock_path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaises(c.PortError): self.stage()

    def test_line_offset_is_rejected_even_when_git_apply_would_succeed(self):
        data = self.patch_bytes.replace(b'@@ -1,3 +1,3 @@', b'@@ -2,3 +2,3 @@')
        self.assertNotEqual(data, self.patch_bytes); self.set_patch(data)
        self.git(self.src, 'apply', '--check', str(self.project / self.patch_path))
        with self.assertRaisesRegex(c.PortError, 'line|context|position'): self.stage()
        out = self.root / 'candidate'; self.assertFalse((out / 'intake-result.json').exists())
        self.assertTrue((out / 'failed-intake.json').is_file())
        self.assertEqual((self.src / self.name).read_bytes(), self.original)

    def test_traversal_mode_symlink_binary_rename_delete_and_git_metadata_rejected(self):
        attacks = [self.patch_bytes.replace(b'a/drivers/test.c', b'a/../escape.c').replace(b'b/drivers/test.c', b'b/../escape.c'),
                   self.patch_bytes.replace(b'index ', b'old mode 100644\nnew mode 100755\nindex ', 1),
                   b'diff --git a/link b/link\nnew file mode 120000\n--- /dev/null\n+++ b/link\n@@ -0,0 +1 @@\n+../outside\n',
                   b'diff --git a/a b/a\nGIT binary patch\nliteral 1\nA\n',
                   b'diff --git a/a b/b\nrename from a\nrename to b\n',
                   b'diff --git a/drivers/test.c b/drivers/test.c\ndeleted file mode 100644\n--- a/drivers/test.c\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n',
                   b'diff --git a/.git/config b/.git/config\nnew file mode 100644\n--- /dev/null\n+++ b/.git/config\n@@ -0,0 +1 @@\n+[alias]\n']
        for i, data in enumerate(attacks):
            with self.subTest(index=i):
                self.set_patch(data)
                with self.assertRaises(c.PortError): self.stage('attack' + str(i))
                self.assertFalse((self.root / ('attack' + str(i)) / 'intake-result.json').exists())
        self.assertFalse((self.root / 'escape.c').exists())

    def test_new_script_can_be_recorded_but_is_never_executed(self):
        marker = self.root / 'SCRIPT-RAN'
        data = ('diff --git a/helper.sh b/helper.sh\nnew file mode 100644\n--- /dev/null\n+++ b/helper.sh\n@@ -0,0 +1,3 @@\n+#!/bin/sh\n+# SPDX-License-Identifier: MIT\n+touch ' + str(marker) + '\n').encode()
        self.set_patch(data); self.stage()
        self.assertFalse(marker.exists())
        mode = (self.root / 'candidate/source-patched/helper.sh').stat().st_mode
        self.assertFalse(mode & 0o111)

    def test_partial_apply_exception_cannot_publish_success_and_blocks_retry(self):
        with patch.object(c, '_apply_patch', side_effect=RuntimeError('injected apply failure')):
            with self.assertRaises(RuntimeError): self.stage()
        out = self.root / 'candidate'; self.assertTrue((out / 'failed-intake.json').is_file())
        self.assertFalse((out / 'intake-result.json').exists())
        with self.assertRaises(c.PortError): self.stage()
        self.assertEqual((self.src / self.name).read_bytes(), self.original)

    def test_lock_and_source_changes_during_materialization_prevent_success(self):
        real = c._apply_patch
        for mode in ('lock', 'source', 'project'):
            with self.subTest(mode=mode):
                def changed(*args, **kwargs):
                    value = real(*args, **kwargs)
                    if mode == 'lock': self.lock_path.write_bytes(self.lock_path.read_bytes() + b' ')
                    elif mode == 'source': self.put(self.src / self.name, self.original + b'changed\n')
                    else:
                        self.put(self.project / 'other.txt', b'new project HEAD\n'); self.commit(self.project)
                    return value
                with patch.object(c, '_apply_patch', side_effect=changed):
                    with self.assertRaises(c.PortError): self.stage('changed-' + mode)
                self.assertFalse((self.root / ('changed-' + mode) / 'intake-result.json').exists())
                self.put(self.src / self.name, self.original); self.save_lock()

    def test_existing_overlap_and_symlink_output_rejected(self):
        out = self.root / 'candidate'; out.mkdir(); (out / 'foreign').write_text('keep')
        with self.assertRaises(c.PortError): self.stage()
        self.assertEqual((out / 'foreign').read_text(), 'keep')
        for output in (self.src / 'new', self.project / 'new', self.root):
            with self.assertRaises(c.PortError): c.stage_candidate(self.lock_path, 'fixture', self.src, output, self.project)
        link = self.root / 'linked'; link.symlink_to(self.src, target_is_directory=True)
        with self.assertRaises(c.PortError): c.stage_candidate(self.lock_path, 'fixture', self.src, link / 'new', self.project)

    def test_captured_lock_reparse_and_unlisted_bundle_files_cannot_pass(self):
        real = c._apply_patch
        for mode in ('captured-lock', 'unlisted-file'):
            with self.subTest(mode=mode):
                def altered(*args, **kwargs):
                    value = real(*args, **kwargs); out = args[0].parent
                    if mode == 'captured-lock':
                        p = out / 'inputs.lock.json'; p.chmod(0o644); p.write_bytes(p.read_bytes() + b' ')
                    else: (out / 'foreign.txt').write_bytes(b'unlisted data')
                    return value
                with patch.object(c, '_apply_patch', side_effect=altered):
                    with self.assertRaises(c.PortError): self.stage(mode)
                out = self.root / mode
                self.assertFalse((out / 'intake-result.json').exists())
                self.assertTrue((out / 'failed-intake.json').is_file())
                if mode == 'unlisted-file': self.assertEqual((out / 'foreign.txt').read_bytes(), b'unlisted data')

    def test_partial_completion_write_is_removed_and_failure_remains(self):
        real_write = os.write
        def failed(fd, data):
            if b'mellowgpu.candidate-intake/1' in bytes(data):
                real_write(fd, bytes(data)[:12]); raise OSError('injected partial completion write')
            return real_write(fd, data)
        with patch.object(c.os, 'write', side_effect=failed):
            with self.assertRaises(OSError): self.stage()
        out = self.root / 'candidate'
        self.assertFalse((out / 'intake-result.json').exists())
        self.assertTrue((out / 'failed-intake.json').exists())
        self.assertEqual((self.src / self.name).read_bytes(), self.original)

    def test_post_patch_abi_mismatch_is_recorded_as_blocked_not_silently_accepted(self):
        data = self.patch_bytes.replace(b'+int value(void) { return 2; }', b'+int value(void) { return 2; }')
        # A real Git diff changing a declared ABI value, with an unchanged consumer document.
        self.put(self.src / self.name, self.original.replace(b'ABI_VERSION 3u', b'ABI_VERSION 4u'))
        data = self.git(self.src, 'diff', '--full-index').encode(); self.git(self.src, 'checkout', '--', self.name)
        self.set_patch(data); result = self.stage()
        self.assertFalse(result['patched_abi_checks_match'])
        self.assertTrue(any('ABI' in x for x in result['blocking_gaps']))
        self.assertFalse(result['ready']); self.assertFalse(result['build_eligible'])

    def test_cli_rejects_before_writing_or_calling_any_script(self):
        p = subprocess.run([sys.executable, str(ROOT / 'Tools/mellow-candidate.py'), 'stage', '--lock', str(self.lock_path), '--component', 'missing', '--source-root', str(self.src), '--project-root', str(self.project), '--out', str(self.root / 'candidate')], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0); self.assertFalse((self.root / 'candidate').exists())
        self.assertIn('production_promotion_available', p.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)

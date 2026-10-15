"""Actual temporary-Git upstream membership/drift tests; no GPU or installation."""
# SPDX-License-Identifier: MIT
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'Tools'))
try:
    from mellow_port import upstreams as u
except ImportError:
    u = None


class UpstreamTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(u, 'RED: strict upstream manager is not implemented')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.src = self.root / 'source'
        self.src.mkdir()
        self.git('init', '-q')
        self.write('include/abi.h', '/* SPDX-License-Identifier: MIT */\n#define GPU_ABI_VERSION 3u\n')
        self.write('README.md', 'GPU ABI version: 3\n')
        self.write('LICENSE', 'Fixture MIT notice\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD').strip()
        self.lock = self.make_lock()

    def git(self, *args):
        env = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                   GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
                   GIT_AUTHOR_DATE='2026-01-01T00:00:00+0000', GIT_COMMITTER_DATE='2026-01-01T00:00:00+0000')
        return subprocess.run(['git', '-C', str(self.src), *args], env=env, check=True,
                              capture_output=True, text=True).stdout

    def write(self, name, text):
        p = self.src / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def make_lock(self):
        files = []
        for name, kind in [('include/abi.h', 'source'), ('README.md', 'specification'), ('LICENSE', 'notice')]:
            data = (self.src / name).read_bytes()
            files.append({'path': name, 'kind': kind, 'git_blob': self.git('rev-parse', self.commit + ':' + name).strip(),
                          'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': len(data), 'spdx': 'MIT',
                          'source_uri': 'https://raw.githubusercontent.com/example/fixture/' + self.commit + '/' + name})
        return {'schema_version': 1, 'components': [{
            'id': 'fixture', 'role': 'build-dependency', 'remote': 'https://github.com/example/fixture',
            'commit': self.commit, 'tracking_ref': 'refs/heads/main', 'profile': 'darwin-permissive',
            'consumers': ['Drivers/Core/Test.cpp'], 'files': files, 'patches': [], 'configuration': [],
            'license_review': {'status': 'unreviewed', 'evidence_sha256': None},
            'abi': {'family': 'fixture-drm-v1', 'headers': ['include/abi.h'], 'checks': [
                {'header': 'include/abi.h', 'symbol': 'GPU_ABI_VERSION', 'document': 'README.md',
                 'marker': 'GPU ABI version:', 'expected': 3}]}}],
            'specifications': [{'id': 'metal-model', 'uri': 'https://developer.apple.com/metal/resources/',
                                'sha256': 'a' * 64, 'revision': 'fixture-only',
                                'consumers': ['docs/MELLOWGPU-SPEC.md']}]}

    def verify(self, lock=None):
        return u.verify_source(self.lock if lock is None else lock, 'fixture', self.src)

    def test_exact_git_blob_and_worktree_membership(self):
        result = self.verify()
        self.assertTrue(result['revision_membership_verified'])
        self.assertEqual(result['files_verified'], 3)
        self.assertFalse(result['build_eligible'])
        self.assertFalse(result['hardware_qualified'])
        self.assertFalse(result['production_promotion_available'])

    def test_changed_worktree_cannot_keep_commit_label(self):
        self.write('include/abi.h', '/* SPDX-License-Identifier: MIT */\n#define GPU_ABI_VERSION 4u\n')
        with self.assertRaisesRegex(u.PortError, 'worktree'):
            self.verify()

    def test_other_checked_out_commit_rejected(self):
        self.write('extra.txt', 'new commit\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'other')
        with self.assertRaisesRegex(u.PortError, 'HEAD'):
            self.verify()

    def test_forged_blob_and_content_digest_rejected(self):
        for field, value in [('git_blob', '1' * 40), ('sha256', '2' * 64), ('size_bytes', 12)]:
            with self.subTest(field=field):
                lock = copy.deepcopy(self.lock)
                lock['components'][0]['files'][0][field] = value
                with self.assertRaises(u.PortError):
                    self.verify(lock)

    def test_path_traversal_and_uri_revision_rejected(self):
        for path in ['../secret.h', '/secret.h', 'include//abi.h', 'include/./abi.h', 'include/a:abi.h', 'C:/abi.h', 'include\\abi.h']:
            with self.subTest(path=path):
                lock = copy.deepcopy(self.lock)
                lock['components'][0]['files'][0]['path'] = path
                with self.assertRaises(u.PortError): u.validate_lock(lock)
        lock = copy.deepcopy(self.lock)
        lock['components'][0]['files'][0]['source_uri'] = 'https://raw.githubusercontent.com/example/fixture/main/include/abi.h'
        with self.assertRaises(u.PortError): u.validate_lock(lock)

    def test_symlink_and_git_symlink_mode_rejected(self):
        p = self.src / 'include/abi.h'
        p.unlink(); p.symlink_to('../LICENSE')
        with self.assertRaises(u.PortError): self.verify()
        self.git('add', '.'); self.git('commit', '-qm', 'symlink')
        self.commit = self.git('rev-parse', 'HEAD').strip(); lock = self.make_lock()
        with self.assertRaises(u.PortError): self.verify(lock)

    def test_reference_only_cannot_promote_to_linked_dependency(self):
        for remote, identifier in [('https://github.com/nullmoth/nvidia-macos-driver', 'nullmoth'),
                                    ('https://github.com/Almosst-DEV/Cobalt64', 'cobalt64')]:
            lock = copy.deepcopy(self.lock); c = lock['components'][0]
            c['remote'] = remote; c['id'] = identifier
            for f in c['files']: f['source_uri'] = remote + '/blob/' + c['commit'] + '/' + f['path']
            with self.assertRaisesRegex(u.PortError, 'reference-only'): u.validate_lock(lock)
        before = copy.deepcopy(self.lock); before['components'][0]['role'] = 'reference-only'
        before['components'][0]['profile'] = 'reference-only'; before['components'][0]['consumers'] = ['docs/REFERENCE.md']
        with self.assertRaisesRegex(u.PortError, 'promotion'): u.report(before, self.lock)

    def test_license_profile_and_actual_source_notice_mismatch(self):
        lock = copy.deepcopy(self.lock); lock['components'][0]['files'][0]['spdx'] = 'GPL-2.0-only'
        with self.assertRaisesRegex(u.PortError, 'license'): u.validate_lock(lock)
        lock['components'][0]['profile'] = 'linux-native'
        u.validate_lock(lock)
        with self.assertRaisesRegex(u.PortError, 'SPDX'): self.verify(lock)

    def test_actual_abi_header_document_and_expected_must_agree(self):
        for name, text in [('README.md', 'GPU ABI version: 4\n'),
                           ('include/abi.h', '/* SPDX-License-Identifier: MIT */\n#define GPU_ABI_VERSION 4u\n')]:
            with self.subTest(name=name):
                original = (self.src / name).read_text()
                self.write(name, text); self.git('add', '.'); self.git('commit', '-qm', 'ABI mismatch')
                self.commit = self.git('rev-parse', 'HEAD').strip(); lock = self.make_lock()
                with self.assertRaisesRegex(u.PortError, 'ABI'): self.verify(lock)
                self.write(name, original); self.git('add', '.'); self.git('commit', '-qm', 'restore')
                self.commit = self.git('rev-parse', 'HEAD').strip()

    def test_report_names_real_header_spec_and_configuration_drift(self):
        after = copy.deepcopy(self.lock)
        after['components'][0]['files'][0]['sha256'] = 'f' * 64
        after['components'][0]['configuration'] = [{'path': 'porting/config.json', 'sha256': 'b' * 64}]
        after['specifications'][0]['sha256'] = 'c' * 64
        result = u.report(self.lock, after)
        change = result['components'][0]
        self.assertEqual(change['abi_headers_changed'], ['include/abi.h'])
        self.assertTrue(change['configuration_changed'])
        self.assertEqual(result['specifications_changed'], ['metal-model'])
        self.assertTrue(result['qualification_invalidated'])
        self.assertFalse(result['production_promotion_available'])

    def test_discovery_returns_candidates_without_changing_lock(self):
        before = json.dumps(self.lock, sort_keys=True)
        calls = []
        def query(remote, ref):
            calls.append((remote, ref)); return {'object': '7' * 40, 'commit': '8' * 40}
        result = u.discover(self.lock, query=query, spec_query=lambda uri: "b" * 64)
        self.assertEqual(calls, [('https://github.com/example/fixture', 'refs/heads/main')])
        self.assertEqual(result['candidates'][0]['commit'], '8' * 40)
        self.assertEqual(result['specification_candidates'][0]['sha256'], 'b' * 64)
        self.assertTrue(result['specification_candidates'][0]['changed'])
        self.assertFalse(result['lock_modified'])
        self.assertFalse(result['production_promotion_available'])
        self.assertEqual(before, json.dumps(self.lock, sort_keys=True))

    def test_duplicate_json_and_unknown_schema_fields_rejected(self):
        path = self.root / 'lock.json'; path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaises(u.PortError): u.load_lock(path)
        lock = copy.deepcopy(self.lock); lock['supported'] = True
        with self.assertRaises(u.PortError): u.validate_lock(lock)
        lock = copy.deepcopy(self.lock); lock['components'][0]['files'].append(copy.deepcopy(lock['components'][0]['files'][0]))
        with self.assertRaises(u.PortError): u.validate_lock(lock)

    def test_selected_configuration_and_patch_bytes_verified(self):
        project = self.root / 'project'; (project / 'porting').mkdir(parents=True)
        config = project / 'porting/config.json'; config.write_text('{"config":1}\n')
        lock = copy.deepcopy(self.lock)
        lock['components'][0]['configuration'] = [{'path': 'porting/config.json', 'sha256': hashlib.sha256(config.read_bytes()).hexdigest()}]
        self.assertTrue(u.verify_source(lock, 'fixture', self.src, project)['revision_membership_verified'])
        config.write_text('{"config":2}\n')
        with self.assertRaisesRegex(u.PortError, 'configuration'): u.verify_source(lock, 'fixture', self.src, project)

    def test_cli_failure_never_overwrites_report_or_lock(self):
        lockpath = self.root / 'lock.json'; lockpath.write_text(json.dumps(self.lock))
        output = self.root / 'result.json'; output.write_text('existing')
        p = subprocess.run([sys.executable, str(REPO / 'Tools/mellow-upstream.py'), 'report', '--lock', str(lockpath), '--output', str(output)], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(output.read_text(), 'existing')
        self.assertEqual(json.loads(lockpath.read_text()), self.lock)

    def test_malformed_nested_types_fail_as_port_errors(self):
        for target, field, value in [('component', 'consumers', [{}]), ('abi', 'headers', [{}]),
                                     ('component', 'remote', 'https://github.com:bad/example/repo')]:
            lock = copy.deepcopy(self.lock)
            obj = lock['components'][0]['abi'] if target == 'abi' else lock['components'][0]
            obj[field] = value
            with self.assertRaises(u.PortError): u.validate_lock(lock)

    def test_same_blob_new_commit_does_not_claim_actual_header_drift(self):
        after = copy.deepcopy(self.lock); c = after['components'][0]
        c['commit'] = '9' * 40
        for f in c['files']:
            f['source_uri'] = 'https://raw.githubusercontent.com/example/fixture/' + c['commit'] + '/' + f['path']
        result = u.report(self.lock, after)
        self.assertTrue(result['components'][0]['changed'])
        self.assertEqual(result['components'][0]['abi_headers_changed'], [])
        self.assertFalse(result['components'][0]['license_changed'])
        self.assertTrue(result['qualification_invalidated'])

    def test_legacy_full_mit_notice_requires_exact_pinned_comment(self):
        notice = ('/* Permission is hereby granted, free of charge, to any person obtaining a copy of this software '
                  'and associated documentation files (the "Software"), to deal in the Software without restriction, '
                  'including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, '
                  'and/or sell copies of the Software. The above copyright notice and this permission notice shall '
                  'be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", '
                  'WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED. */')
        self.write('include/abi.h', notice + '\n#define GPU_ABI_VERSION 3u\n')
        self.git('add', '.'); self.git('commit', '-qm', 'legacy notice')
        self.commit = self.git('rev-parse', 'HEAD').strip(); lock = self.make_lock()
        with self.assertRaisesRegex(u.PortError, 'SPDX'): self.verify(lock)
        lock['components'][0]['files'][0]['license_notice_sha256'] = hashlib.sha256(notice.encode()).hexdigest()
        self.assertTrue(self.verify(lock)['revision_membership_verified'])
        lock['components'][0]['files'][0]['license_notice_sha256'] = 'a' * 64
        with self.assertRaises(u.PortError): self.verify(lock)

    def test_explicit_default_port_cannot_bypass_reference_ceiling(self):
        lock = copy.deepcopy(self.lock); c = lock['components'][0]
        c['remote'] = 'https://github.com:443/nullmoth/nvidia-macos-driver'
        for f in c['files']: f['source_uri'] = c['remote'] + '/blob/' + c['commit'] + '/' + f['path']
        with self.assertRaises(u.PortError): u.validate_lock(lock)

    def test_actual_license_notice_and_notice_evidence_drift_reported(self):
        after = copy.deepcopy(self.lock)
        after['components'][0]['files'][2]['sha256'] = 'a' * 64
        self.assertTrue(u.report(self.lock, after)['components'][0]['license_changed'])
        after = copy.deepcopy(self.lock)
        after['components'][0]['files'][0]['license_notice_sha256'] = 'b' * 64
        self.assertTrue(u.report(self.lock, after)['components'][0]['license_changed'])

    def test_checked_in_lock_is_strict_and_all_runtime_support_stays_unqualified(self):
        lock = u.load_lock(REPO / 'porting/upstreams.lock.json')
        result = u.report(lock)
        self.assertFalse(result['hardware_qualified'])
        self.assertFalse(result['production_promotion_available'])
        roles = {c['id']: c['role'] for c in lock['components']}
        self.assertEqual(roles['nullmoth-reference'], 'reference-only')
        self.assertEqual(roles['cobalt64-reference'], 'reference-only')


if __name__ == '__main__': unittest.main(verbosity=2)

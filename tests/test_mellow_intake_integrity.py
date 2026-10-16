"""Actual Git/filesystem counterexamples for immutable, unapproved intake."""
# SPDX-License-Identifier: MIT
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import unittest
from unittest.mock import patch
import zlib

import test_mellow_candidate as candidate_fixture
import test_mellow_upstreams as upstream_fixture

c = candidate_fixture.c
u = upstream_fixture.u


class CandidateIntegrityTests(unittest.TestCase):
    setUp = candidate_fixture.CandidateTests.setUp
    sha = staticmethod(candidate_fixture.CandidateTests.sha)
    put = staticmethod(candidate_fixture.CandidateTests.put)
    git = candidate_fixture.CandidateTests.git
    commit = candidate_fixture.CandidateTests.commit
    record = candidate_fixture.CandidateTests.record
    save_lock = candidate_fixture.CandidateTests.save_lock
    stage = candidate_fixture.CandidateTests.stage
    set_patch = candidate_fixture.CandidateTests.set_patch

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS Data firmlink counterexample')
    def test_data_volume_alias_cannot_place_output_in_protected_checkout(self):
        for source in (self.src, self.project):
            alias = Path('/System/Volumes/Data' + str(source))
            if not alias.is_dir() or not alias.samefile(source):
                self.skipTest('This host does not expose the Data-volume alias')
            output = alias / 'unexpected-intake'
            with self.assertRaisesRegex(c.PortError, 'overlap'):
                c.stage_candidate(self.lock_path, 'fixture', self.src, output, self.project)
            self.assertFalse((source / 'unexpected-intake').exists())

    def test_case_alias_cannot_place_output_in_protected_checkout(self):
        for source in (self.src, self.project):
            alias = source.with_name(source.name.upper())
            if not alias.is_dir() or not alias.samefile(source):
                self.skipTest('Case-sensitive host filesystem')
            with self.assertRaisesRegex(c.PortError, 'overlap'):
                c.stage_candidate(self.lock_path, 'fixture', self.src, alias / 'unexpected-intake', self.project)
            self.assertFalse((source / 'unexpected-intake').exists())

    def test_unparsed_pre_diff_context_patch_bytes_are_rejected(self):
        hidden = (b'*** foreign.txt\t2026-01-01\n--- foreign.txt\t2026-01-02\n'
                  b'***************\n*** 1 ****\n- old\n--- 1 ----\n+ hidden\n')
        self.set_patch(hidden + self.patch_bytes)
        # Git accepts the Git diff while ignoring this context-diff preamble.
        self.git(self.src, 'apply', '--check', str(self.project / self.patch_path))
        with self.assertRaisesRegex(c.PortError, 'preamble|start|header'):
            self.stage()
        self.assertFalse((self.root / 'candidate/intake-result.json').exists())

    def test_unreadable_or_empty_unlisted_directories_never_complete(self):
        real = c._apply_patch
        for mode in ('unreadable', 'empty'):
            foreign = self.root / mode / 'foreign'
            def altered(*args, **kwargs):
                result = real(*args, **kwargs)
                foreign.mkdir()
                if mode == 'unreadable':
                    child = foreign / 'unlisted.sh'; child.write_text('#!/bin/sh\nexit 0\n')
                    child.chmod(0o755); foreign.chmod(0)
                return result
            try:
                with patch.object(c, '_apply_patch', side_effect=altered):
                    with self.assertRaises(c.PortError): self.stage(mode)
                self.assertFalse((self.root / mode / 'intake-result.json').exists())
            finally:
                if foreign.exists(): foreign.chmod(0o700)

    def test_post_inventory_symlink_cannot_chmod_foreign_file(self):
        foreign = self.root / 'foreign.txt'; foreign.write_bytes(self.original)
        foreign.chmod(0o640); before = stat.S_IMODE(foreign.stat().st_mode)
        real = c._tree; changed = False
        def swapped(root, *args, **kwargs):
            nonlocal changed
            result = real(root, *args, **kwargs)
            if Path(root) == self.root / 'candidate' and not changed:
                changed = True
                file = Path(root) / 'source-patched' / self.name
                file.unlink(); file.symlink_to(foreign)
            return result
        with patch.object(c, '_tree', side_effect=swapped):
            with self.assertRaises((c.PortError, OSError)): self.stage()
        self.assertTrue(changed)
        self.assertEqual(stat.S_IMODE(foreign.stat().st_mode), before)
        self.assertFalse((self.root / 'candidate/intake-result.json').exists())

    def test_special_file_read_rejects_without_waiting_for_a_writer(self):
        os.mkfifo(self.root / 'pipe')
        code = ('import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);'
                'from mellow_port.upstreams import read_file,PortError\n'
                'try: read_file(Path(sys.argv[2]),"pipe")\n'
                'except PortError: sys.exit(0)\n'
                'sys.exit(1)\n')
        done = subprocess.run([sys.executable, '-c', code, str(candidate_fixture.ROOT / 'Tools'), str(self.root)],
                              capture_output=True, text=True, timeout=1)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_writer_never_follows_a_parent_directory_symlink(self):
        private = self.root / 'private'; private.mkdir()
        foreign = self.root / 'foreign'; foreign.mkdir()
        (private / 'link').symlink_to(foreign, target_is_directory=True)
        with self.assertRaises((c.PortError, OSError)):
            c._write(private / 'link' / 'nested' / 'new.txt', b'not allowed')
        self.assertFalse((foreign / 'nested').exists())

    def test_project_nested_tree_membership_is_rehashed(self):
        old_blob = self.git(self.project, 'rev-parse', 'HEAD:config/options.json').strip()
        tree = self.git(self.project, 'rev-parse', 'HEAD:config').strip()
        changed = b'{"feature":false}\n'; self.put(self.project / 'config/options.json', changed)
        new_blob = self.git(self.project, 'hash-object', '-w', 'config/options.json').strip()
        treefile = self.project / '.git' / 'objects' / tree[:2] / tree[2:]
        raw = zlib.decompress(treefile.read_bytes())
        treefile.chmod(0o600)
        treefile.write_bytes(zlib.compress(raw.replace(bytes.fromhex(old_blob), bytes.fromhex(new_blob))))
        entry = self.lock['components'][0]
        entry['configuration'][0]['sha256'] = self.sha(changed)
        with self.assertRaisesRegex(c.PortError, 'object|tree|digest'):
            c._project_inputs(entry, self.project)

    def test_added_script_assembly_shader_and_makefile_notices_are_observed(self):
        data = self.patch_bytes
        for name in ('extra.py', 'assembly.S', 'shader.metal', 'Makefile'):
            data += ('diff --git a/' + name + ' b/' + name + '\nnew file mode 100644\n'
                     '--- /dev/null\n+++ b/' + name + '\n@@ -0,0 +1 @@\n'
                     '+# SPDX-License-Identifier: MIT\n').encode()
        self.set_patch(data); result = self.stage()
        for name in ('extra.py', 'assembly.S', 'shader.metal', 'Makefile'):
            self.assertEqual(result['post_patch_license_observations'].get(name, {}).get('single_expression'), 'MIT')
            self.assertIn(name, result['post_patch_license_changed_paths'])


class UpstreamIntegrityTests(unittest.TestCase):
    setUp = upstream_fixture.UpstreamTests.setUp
    git = upstream_fixture.UpstreamTests.git
    write = upstream_fixture.UpstreamTests.write
    make_lock = upstream_fixture.UpstreamTests.make_lock
    verify = upstream_fixture.UpstreamTests.verify

    def test_known_github_reference_host_alias_cannot_promote(self):
        for host in ('www.github.com', 'GITHUB.COM', 'github.com.'):
            lock = copy.deepcopy(self.lock); entry = lock['components'][0]
            entry['remote'] = 'https://' + host + '/nullmoth/nvidia-macos-driver'
            entry['id'] = 'renamed-reference'
            for item in entry['files']:
                item['source_uri'] = entry['remote'] + '/blob/' + entry['commit'] + '/' + item['path']
            with self.assertRaises(u.PortError): u.validate_lock(lock)

    def test_known_reference_commit_cannot_be_relabelled_as_a_fork(self):
        lock = copy.deepcopy(self.lock); entry = lock['components'][0]
        entry['remote'] = 'https://github.com/example/renamed-project'
        entry['commit'] = 'd96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb'
        for item in entry['files']:
            item['source_uri'] = entry['remote'] + '/blob/' + entry['commit'] + '/' + item['path']
        with self.assertRaisesRegex(u.PortError, 'reference-only'): u.validate_lock(lock)

    def test_known_reference_blob_or_bytes_cannot_be_relabelled(self):
        for field, value in [('git_blob', '0f80732aaa0f51a3928835503aaa2168b46de3f9'),
                             ('sha256', 'b999a335eabb62c8b5394660aa8e3cc16bebb8483aace53abf9a16991addff42')]:
            lock = copy.deepcopy(self.lock); lock['components'][0]['files'][1][field] = value
            with self.assertRaisesRegex(u.PortError, 'reference-only'): u.validate_lock(lock)

    def test_report_does_not_lose_reference_role_when_id_is_changed(self):
        before = copy.deepcopy(self.lock); entry = before['components'][0]
        entry['role'] = entry['profile'] = 'reference-only'; entry['consumers'] = ['docs/REFERENCE.md']
        after = copy.deepcopy(self.lock); after['components'][0]['id'] = 'renamed-component'
        with self.assertRaisesRegex(u.PortError, 'promotion'): u.report(before, after)

    def test_source_suffix_cannot_skip_license_verification_by_kind(self):
        for name in ('include/abi.h', 'build.py', 'shader.metal', 'assembly.S', 'Makefile'):
            lock = copy.deepcopy(self.lock); entry = lock['components'][0]; item = entry['files'][0]
            entry['abi'] = {'family': 'fixture', 'headers': [], 'checks': []}
            item['path'] = name; item['kind'] = 'specification'
            item['source_uri'] = entry['remote'] + '/blob/' + entry['commit'] + '/' + name
            with self.assertRaisesRegex(u.PortError, 'source'): u.validate_lock(lock)

    def test_malformed_extra_spdx_is_not_silently_dropped(self):
        self.write('include/abi.h', '/* SPDX-License-Identifier: MIT */\n'
                   '/* SPDX-License-Identifier: GPL-2.0-only */ /* see COPYING */\n#define GPU_ABI_VERSION 3u\n')
        self.git('add', '.'); self.git('commit', '-qm', 'ambiguous SPDX')
        self.commit = self.git('rev-parse', 'HEAD').strip(); lock = self.make_lock()
        with self.assertRaisesRegex(u.PortError, 'SPDX'): self.verify(lock)

    def test_forged_nested_tree_is_rejected_even_if_git_and_worktree_agree(self):
        old_blob = self.git('rev-parse', self.commit + ':include/abi.h').strip()
        tree = self.git('rev-parse', self.commit + ':include').strip()
        data = b'/* SPDX-License-Identifier: MIT */\n#define GPU_ABI_VERSION 3u\n/* substituted tree bytes */\n'
        self.write('include/abi.h', data.decode())
        new_blob = self.git('hash-object', '-w', 'include/abi.h').strip()
        treefile = self.src / '.git' / 'objects' / tree[:2] / tree[2:]
        raw = zlib.decompress(treefile.read_bytes())
        substituted = raw.replace(bytes.fromhex(old_blob), bytes.fromhex(new_blob))
        self.assertNotEqual(substituted, raw)
        treefile.chmod(0o600)
        treefile.write_bytes(zlib.compress(substituted))
        # Actual Git follows the tampered nested tree under the original commit.
        self.assertEqual(self.git('rev-parse', self.commit + ':include/abi.h').strip(), new_blob)
        lock = self.make_lock()
        with self.assertRaisesRegex(u.PortError, 'object|tree|digest'): self.verify(lock)

    def test_tampered_commit_bytes_cannot_keep_the_original_revision(self):
        objectfile = self.src / '.git' / 'objects' / self.commit[:2] / self.commit[2:]
        raw = zlib.decompress(objectfile.read_bytes()); objectfile.chmod(0o600)
        objectfile.write_bytes(zlib.compress(raw[:-1] + b'x'))
        self.assertEqual(self.git('rev-parse', 'HEAD').strip(), self.commit)
        with self.assertRaisesRegex(u.PortError, 'object|digest'): self.verify()

    def test_sha256_git_object_chain_uses_the_repository_hash_width(self):
        other = self.root / 'sha256'; other.mkdir(); self.src = other
        self.git('init', '-q', '--object-format=sha256')
        self.write('nested/test.h', '/* SPDX-License-Identifier: MIT */\n')
        self.git('add', '.'); self.git('commit', '-qm', 'SHA256 object fixture')
        commit = self.git('rev-parse', 'HEAD').strip()
        mode, blob, data = u.verified_git_blob(other, commit, 'nested/test.h')
        self.assertEqual(len(commit), 64); self.assertEqual(len(blob), 64)
        self.assertEqual(mode, '100644'); self.assertEqual(data, (other / 'nested/test.h').read_bytes())


if __name__ == '__main__': unittest.main(verbosity=2)

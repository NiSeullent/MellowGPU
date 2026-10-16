"""Actual Git and filesystem regressions for additional source-intake boundaries."""
# SPDX-License-Identifier: MIT
import copy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import test_mellow_candidate as cf
import test_mellow_upstreams as uf
from mellow_port import core

c, u = cf.c, uf.u


class CandidateBoundaryTests(unittest.TestCase):
    setUp = cf.CandidateTests.setUp
    sha = staticmethod(cf.CandidateTests.sha)
    put = staticmethod(cf.CandidateTests.put)
    git = cf.CandidateTests.git
    commit = cf.CandidateTests.commit
    record = cf.CandidateTests.record
    save_lock = cf.CandidateTests.save_lock
    stage = cf.CandidateTests.stage
    set_patch = cf.CandidateTests.set_patch

    def test_git_apply_never_writes_into_published_candidate_tree(self):
        real = c._expected_patch
        exposed = []
        def race(contents, data):
            result = real(contents, data)
            tree = self.root / 'candidate/source-patched'
            exposed.append(tree.exists())
            if tree.exists():
                tree.rename(self.root / 'candidate/aside')
                tree.symlink_to(self.src, target_is_directory=True)
            return result
        try:
            with patch.object(c, '_expected_patch', side_effect=race):
                self.stage()
        except c.PortError:
            pass
        self.assertEqual((self.src / self.name).read_bytes(), self.original)
        self.assertEqual(self.git(self.src, 'status', '--porcelain'), '')
        self.assertEqual(exposed, [False])

    def test_known_reference_configuration_and_patch_bytes_rejected_before_output(self):
        for kind, name in [('configuration', 'config/options.json'), ('patches', self.patch_path)]:
            data = (self.project / name).read_bytes()
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            with self.subTest(kind=kind), patch.object(u, 'REFERENCE_BLOBS', u.REFERENCE_BLOBS | {blob}):
                with self.assertRaisesRegex(c.PortError, 'reference-only'):
                    self.stage(kind)
                self.assertFalse((self.root / kind).exists())

    def test_known_reference_patch_result_rejected_without_publishing_source(self):
        restricted = b'Known restricted bytes; fixture, not upstream code.\n'
        data = self.patch_bytes + (b'diff --git a/copied.txt b/copied.txt\nnew file mode 100644\n'
            b'--- /dev/null\n+++ b/copied.txt\n@@ -0,0 +1 @@\n+' + restricted)
        self.set_patch(data)
        with patch.object(u, 'REFERENCE_DIGESTS', u.REFERENCE_DIGESTS | {self.sha(restricted)}):
            with self.assertRaisesRegex(c.PortError, 'reference-only'): self.stage()
        self.assertEqual((self.src / self.name).read_bytes(), self.original)
        self.assertFalse((self.root / 'candidate/source-patched').exists())
        self.assertFalse((self.root / 'candidate/intake-result.json').exists())

    def test_legacy_grant_and_added_prose_changes_are_observed(self):
        legacy = (b'/* Copyright 2026 Fixture Authors.\n'
            b' * Permission is hereby granted, free of charge, to any person obtaining a copy\n'
            b' * to deal in the Software without restriction.\n'
            b' * This permission notice shall be included in all copies or substantial portions.\n'
            b' * THE SOFTWARE IS PROVIDED WITHOUT WARRANTY OF ANY KIND.\n */\n')
        original = legacy + b'#define ABI_VERSION 3u\nint value(void) { return 1; }\n'
        self.put(self.src / self.name, original); self.commit(self.src)
        self.revision = self.git(self.src, 'rev-parse', 'HEAD').strip()
        entry = self.lock['components'][0]; entry['commit'] = self.revision
        entry['files'] = [self.record(self.name, 'source'), self.record('README.md', 'specification')]
        entry['files'][0]['license_notice_sha256'] = self.sha(legacy.rstrip(b'\n'))
        self.original = original
        for label, after in [('legacy-grant', original.replace(b'Permission is hereby granted, free of charge',
                                     b'Permission requires the GNU General Public License')),
                             ('prose-added', original + b'/* Licensed under the GNU General Public License. */\n')]:
            with self.subTest(label=label):
                self.put(self.src / self.name, after)
                diff = self.git(self.src, 'diff', '--full-index').encode()
                self.git(self.src, 'checkout', '--', self.name)
                self.set_patch(diff)
                result = self.stage(label)
                self.assertIn(self.name, result['post_patch_license_changed_paths'])
                self.assertFalse(result['build_eligible'])

    def test_notice_path_variants_are_observed_without_kind_relabelling_bypass(self):
        names = ['LICENCE', 'LICENSES/preferred/MIT', 'COPYRIGHT']
        for name in names: self.put(self.src / name, b'Original notice bytes.\n')
        self.commit(self.src); self.revision = self.git(self.src, 'rev-parse', 'HEAD').strip()
        entry = self.lock['components'][0]; entry['commit'] = self.revision
        entry['files'] = [self.record(self.name, 'source'), self.record('README.md', 'specification')] + [
            self.record(name, 'specification') for name in names]
        for name in names: self.put(self.src / name, b'Changed notice bytes.\n')
        diff = self.git(self.src, 'diff', '--full-index').encode()
        self.git(self.src, 'checkout', '--', *names); self.set_patch(diff)
        result = self.stage()
        self.assertTrue(set(names) <= set(result['post_patch_license_changed_paths']))


class UpstreamBoundaryTests(unittest.TestCase):
    setUp = uf.UpstreamTests.setUp
    git = uf.UpstreamTests.git
    write = uf.UpstreamTests.write
    make_lock = uf.UpstreamTests.make_lock
    verify = uf.UpstreamTests.verify

    def test_known_reference_direct_parent_is_rejected_without_ancestry_walk(self):
        known = self.commit
        self.git('commit', '-q', '--allow-empty', '-m', 'child of known reference')
        self.commit = self.git('rev-parse', 'HEAD').strip(); lock = self.make_lock()
        with patch.object(u, 'REFERENCE_COMMITS', u.REFERENCE_COMMITS | {known}):
            with self.assertRaisesRegex(u.PortError, 'reference-only'): self.verify(lock)

    def test_unchanged_mixed_reference_and_build_observations_are_not_a_promotion(self):
        reference = copy.deepcopy(self.lock['components'][0])
        reference.update(id='same-origin-reference', role='reference-only', profile='reference-only',
                         consumers=['docs/REFERENCE.md'])
        lock = copy.deepcopy(self.lock); lock['components'].append(reference)
        result = u.report(lock)
        self.assertFalse(result['qualification_invalidated'])
        self.assertFalse(result['build_eligible'])
        promoted = copy.deepcopy(lock); promoted['components'][0]['id'] = 'new-copy'
        with self.assertRaisesRegex(u.PortError, 'promotion'): u.report(lock, promoted)

    def test_non_lf_spdx_separators_and_marker_case_fail_closed(self):
        for label, head in [('ff', '/* SPDX-License-Identifier: MIT\f GPL-2.0-only */\n'),
                            ('unicode-line', '/* SPDX-License-Identifier: MIT\u2028 GPL-2.0-only */\n'),
                            ('lowercase-tag', '/* SPDX-License-Identifier: MIT */\n/* spdx-license-identifier: GPL-2.0-only */\n')]:
            with self.subTest(label=label):
                self.write('include/abi.h', head + '#define GPU_ABI_VERSION 3u\n')
                self.git('add', '.'); self.git('commit', '-qm', label)
                self.commit = self.git('rev-parse', 'HEAD').strip()
                facts = core.license_facts((self.src / 'include/abi.h').read_text())
                self.assertIsNone(facts['single_expression'])
                self.assertTrue(facts['gpl_component_review_required'])
                with self.assertRaisesRegex(u.PortError, 'SPDX'): self.verify(self.make_lock())

    def test_lowercase_gpl_always_requires_review(self):
        facts = core.license_facts('/* SPDX-License-Identifier: gpl-2.0-only */\n')
        self.assertTrue(facts['gpl_component_review_required'])
        name = 'drivers/gpu/drm/xe/lowercase.h'
        self.write(name, '/* SPDX-License-Identifier: gpl-2.0-only */\n#define REG 0x10\n')
        out = self.root / 'lowercase-review'
        result = core.prepare('generate', self.src, 'xe', self.commit, [name], out)
        gaps = json.loads((out / 'gap-report.json').read_bytes())['gaps']
        self.assertTrue(any(item['id'] == 'license:' + name for item in gaps))
        self.assertFalse(result['driver_ready'])

    def test_trailing_vertical_separator_is_not_stripped_into_permissive_spdx(self):
        for separator in ('\v', '\f', '\x1c', '\x1d', '\x1e', '\x85', '\u2028', '\u2029'):
            with self.subTest(separator=repr(separator)):
                facts = core.license_facts('/* SPDX-License-Identifier: MIT' + separator + ' */\n')
                self.assertIsNone(facts['single_expression'])
                self.assertTrue(facts['malformed_spdx_markers_observed'])
        self.assertEqual(core.license_facts('/* SPDX-License-Identifier: MIT */\r\n')['single_expression'], 'MIT')


if __name__ == '__main__': unittest.main(verbosity=2)

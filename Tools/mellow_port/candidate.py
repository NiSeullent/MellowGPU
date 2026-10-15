"""Verified source/patch/config evidence bundles; never build or promote a driver.

SPDX-License-Identifier: MIT
Original MellowGPU tooling; existing repository/component licenses are unchanged.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile

from . import upstreams as u
from .core import PortError, canonical, digest, license_facts, no_links, reject_json_constant, unique_json_object

MAX_SOURCE_BYTES = 128 * 1024 * 1024
MAX_LOCAL_BYTES = 32 * 1024 * 1024
MAX_PATCHED_BYTES = MAX_SOURCE_BYTES + MAX_LOCAL_BYTES
MAX_BUNDLE_BYTES = 2 * MAX_SOURCE_BYTES + 2 * MAX_LOCAL_BYTES + u.MAX_BYTES + 4096
FORBIDDEN = {'.git', '.gitattributes', '.gitmodules', '.gitignore'}


def _safe_path(name):
    u.path(name)
    u.require(not any(p.lower() in FORBIDDEN for p in name.split('/')), 'Git metadata paths are not candidate source')
    return name


def _parse_lock(raw):
    try:
        return u.validate_lock(json.loads(raw, object_pairs_hook=unique_json_object, parse_constant=reject_json_constant))
    except (ValueError, UnicodeError) as error:
        raise PortError('Invalid captured lock: ' + str(error)) from error


def _snapshot_lock(path):
    path = no_links(path)
    lock = u.load_lock(path)
    raw = u.read_file(path.parent, path.name)
    u.require(_parse_lock(raw) == lock, 'Lock changed between parse and capture')
    return lock, raw


def _write(path, data, mode=0o444):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    identity = os.fstat(fd)
    try:
        view = memoryview(data)
        while view:
            size = os.write(fd, view)
            u.require(size > 0, 'Incomplete candidate write')
            view = view[size:]
        os.fsync(fd)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)
    except BaseException:
        # Remove only this call's incomplete file; preserve an unexpected replacement.
        try:
            now = path.lstat()
            if (now.st_dev, now.st_ino) == (identity.st_dev, identity.st_ino): path.unlink()
        except OSError: pass
        raise
    finally:
        os.close(fd)


def _json(path, value):
    _write(path, canonical(value).encode())


def _project_inputs(component, root):
    head = u.git(['rev-parse', '--verify', 'HEAD'], root).decode().strip()
    u.object_id(head)
    u.require(u.git(['cat-file', '-t', head], root).strip() == b'commit', 'Project HEAD must be a commit')
    records, data, total = [], {}, 0
    for kind in ('patches', 'configuration'):
        for item in component[kind]:
            name = _safe_path(item['path'])
            row = u.git(['ls-tree', '-z', head, '--', name], root)
            try:
                header, filename = row.rstrip(b'\0').split(b'\t')
                mode, object_type, oid = header.decode().split()
            except ValueError as error:
                raise PortError('Project input is not a tracked Git blob') from error
            u.require(filename.decode() == name and mode in ('100644', '100755') and object_type == 'blob', 'Project input mode/membership mismatch')
            size = u.git(['cat-file', '-s', head + ':' + name], root).strip()
            u.require(size.isdigit() and int(size) <= u.MAX_BYTES, 'Project input exceeds bound')
            blob = u.git(['cat-file', 'blob', head + ':' + name], root)
            hasher = hashlib.sha1 if len(oid) == 40 else hashlib.sha256
            u.require(hasher(b'blob ' + str(len(blob)).encode() + b'\0' + blob).hexdigest() == oid and digest(blob) == item['sha256'], 'Project input Git blob/digest mismatch')
            u.require(u.read_file(root, name) == blob, 'Project input worktree differs from HEAD')
            total += len(blob)
            u.require(total <= MAX_LOCAL_BYTES, 'Project input aggregate bound exceeded')
            if name in data: u.require(data[name] == blob, 'Conflicting project input')
            data[name] = blob
            records.append({'path': name, 'kind': kind, 'git_blob': oid, 'sha256': digest(blob), 'size_bytes': len(blob), 'git_mode': mode})
    return head, records, data


def _tree(root, maximum=MAX_PATCHED_BYTES):
    result = {}
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories + files:
            file = Path(current) / name
            info = file.lstat()
            relative = file.relative_to(root).as_posix()
            _safe_path(relative)
            u.require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode), 'Candidate contains a link or special file')
            if stat.S_ISREG(info.st_mode):
                u.require(not info.st_mode & 0o111 and info.st_size <= u.MAX_BYTES, 'Candidate file mode/size rejected')
                result[relative] = u.read_file(root, relative)
    u.require(sum(len(data) for data in result.values()) <= maximum, 'Candidate aggregate bound exceeded')
    return result


def _manifest(contents):
    return {name: {'sha256': digest(data), 'size_bytes': len(data)} for name, data in sorted(contents.items())}


def _lines(data):
    u.require(not data or data.endswith(b'\n'), 'Strict patch subset requires a final LF')
    return [line + b'\n' for line in data.split(b'\n')[:-1]] if data else []


def _expected_patch(contents, patch):
    """Apply byte-exact unified hunks in memory at declared line numbers.

    Git may find an offset even with --check; this independent preimage/position
    check forbids offsets, dropped context, fuzz and overlapping hunks.
    """
    u.require(patch and len(patch) <= u.MAX_BYTES and b'\0' not in patch and b'\r' not in patch, 'Strict text patch required')
    try: patch.decode('utf-8')
    except UnicodeError as error: raise PortError('Patch must be UTF-8') from error
    lines = _lines(patch)
    starts = [i for i, line in enumerate(lines) if line.startswith(b'diff --git ')]
    u.require(starts, 'Git text diff headers required')
    result = dict(contents); changed = set()
    for index, start in enumerate(starts):
        block = lines[start:(starts[index + 1] if index + 1 < len(starts) else len(lines))]
        match = re.fullmatch(rb'diff --git a/([A-Za-z0-9_./+-]+) b/([A-Za-z0-9_./+-]+)\n', block[0])
        u.require(match is not None and match[1] == match[2], 'Rename/copy or noncanonical patch path rejected')
        name = _safe_path(match[1].decode()); u.require(name not in changed, 'Repeated patch target rejected'); changed.add(name)
        position = 1; addition = False; seen_index = False
        while position < len(block) and not block[position].startswith(b'--- '):
            line = block[position]
            if line == b'new file mode 100644\n' and not addition: addition = True
            elif re.fullmatch(rb'index [0-9a-f]{4,64}\.\.[0-9a-f]{4,64}(?: 100644)?\n', line) and not seen_index: seen_index = True
            else: raise PortError('Unsupported patch mode/binary/rename/delete metadata')
            position += 1
        u.require(position + 2 < len(block), 'Incomplete text patch headers')
        old = b'--- /dev/null\n' if addition else b'--- a/' + name.encode() + b'\n'
        u.require(block[position] == old and block[position + 1] == b'+++ b/' + name.encode() + b'\n', 'Patch headers do not match exact target')
        u.require(name not in result if addition else name in result, 'Patch target is not a selected or added source')
        original = b'' if addition else result[name]
        u.require(b'\0' not in original and b'\r' not in original, 'Patched source must use strict LF text')
        try: original.decode('utf-8')
        except UnicodeError as error: raise PortError('Patched source must be UTF-8') from error
        before = _lines(original); cursor = 0; output = []; hunks = 0; position += 2
        while position < len(block):
            header = re.fullmatch(rb'@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@[^\n]*\n', block[position])
            u.require(header is not None, 'Unsupported hunk/no-newline/trailing patch metadata')
            old_start = int(header[1]); old_count = int(header[2] or b'1')
            new_start = int(header[3]); new_count = int(header[4] or b'1')
            old_at = old_start - 1 if old_count else old_start
            new_at = new_start - 1 if new_count else new_start
            u.require(0 <= cursor <= old_at <= len(before), 'Hunk line position/overlap rejected')
            output.extend(before[cursor:old_at]); cursor = old_at
            u.require(new_at == len(output), 'Hunk new line position rejected')
            position += 1; removed = added = 0
            while position < len(block) and not block[position].startswith(b'@@ '):
                line = block[position]
                u.require(line[:1] in (b' ', b'-', b'+'), 'Unsupported hunk line')
                prefix, payload = line[:1], line[1:]
                if prefix in (b' ', b'-'):
                    u.require(cursor < len(before) and before[cursor] == payload, 'Hunk exact line/context mismatch')
                    cursor += 1; removed += 1
                if prefix in (b' ', b'+'): output.append(payload); added += 1
                position += 1
            u.require(removed == old_count and added == new_count, 'Hunk line count mismatch')
            hunks += 1
        u.require(hunks > 0, 'Patch has no hunks')
        output.extend(before[cursor:]); after = b''.join(output)
        u.require(len(after) <= u.MAX_BYTES, 'Patched file exceeded bound')
        result[name] = after
    u.require(sum(map(len, result.values())) <= MAX_PATCHED_BYTES, 'Patched source aggregate bound exceeded')
    return result


def _apply_patch(staged, patch_file, contents, git_dir):
    u.require(_tree(staged) == contents, 'Staged source changed before apply')
    patch_bytes = u.read_file(patch_file.parent, patch_file.name)
    expected = _expected_patch(contents, patch_bytes)
    prefix = ['--git-dir=' + str(git_dir), '--work-tree=' + str(staged), 'apply']
    u.git(prefix + ['--check', '--whitespace=error-all', str(patch_file)], staged)
    u.git(prefix + ['--whitespace=error-all', str(patch_file)], staged)
    actual = _tree(staged)
    u.require(actual == expected, 'Actual Git result differs from exact line/context result')
    return actual, {'patch_sha256': digest(patch_bytes), 'check_exit_code': 0, 'apply_exit_code': 0,
                    'exact_line_context_verified': True, 'before': _manifest(contents), 'after': _manifest(actual)}


def stage_candidate(lock_path, component_id, source_root, output, project_root=None):
    source = no_links(source_root); project = no_links(project_root or Path(__file__).resolve().parents[2])
    target = no_links(output); lock_path = no_links(lock_path)
    u.require(not target.exists() and target.parent.is_dir(), 'Candidate output must be fresh with an existing parent')
    for root in (source, project):
        u.require(not target.is_relative_to(root) and not root.is_relative_to(target), 'Candidate output/source/project overlap rejected')
    lock, lock_raw = _snapshot_lock(lock_path)
    entries = [entry for entry in lock['components'] if entry['id'] == component_id]
    u.require(len(entries) == 1, 'Unknown candidate component')
    entry = entries[0]
    u.require(entry['role'] == 'build-dependency', 'reference-only entries cannot become build candidates')
    u.require(sum(item['size_bytes'] for item in entry['files']) <= MAX_SOURCE_BYTES, 'Selected source aggregate bound exceeded')
    verification = u.verify_source(lock, component_id, source, project)
    project_head, project_records, local = _project_inputs(entry, project)
    raw = {}
    for item in entry['files']:
        name = _safe_path(item['path']); data = u.read_file(source, name)
        u.require(digest(data) == item['sha256'], 'Source changed before materialization'); raw[name] = data
    target.mkdir(mode=0o700)
    try:
        intent = {'schema': 'mellowgpu.candidate-intent/1', 'component': component_id,
            'commit': entry['commit'], 'lock_sha256': digest(lock_raw), 'ready': False, 'build_eligible': False,
            'hardware_qualified': False, 'production_promotion_available': False}
        _json(target / 'intake-intent.json', intent)
        _write(target / 'inputs.lock.json', lock_raw)
        for name, data in raw.items():
            _write(target / 'source-raw' / name, data)
            _write(target / 'source-patched' / name, data, 0o644)
        for name, data in local.items(): _write(target / 'project-inputs' / name, data)
        apply_records = []; current = dict(raw)
        with tempfile.TemporaryDirectory(prefix='mellow-candidate-git-') as temporary:
            git_dir = Path(temporary).resolve() / 'metadata'
            u.git(['init', '--bare', '--template=', str(git_dir)])
            for patch in entry['patches']:
                current, record = _apply_patch(target / 'source-patched', target / 'project-inputs' / patch['path'], current, git_dir)
                record['path'] = patch['path']; apply_records.append(record)
        # Revalidate all originals, project membership, captured inputs and staged outputs.
        u.require(_snapshot_lock(lock_path) == (lock, lock_raw), 'Lock changed during candidate staging')
        u.require(u.read_file(target, 'inputs.lock.json') == lock_raw and u.load_lock(target / 'inputs.lock.json') == lock, 'Captured lock bytes/reparse mismatch')
        u.require(u.verify_source(lock, component_id, source, project) == verification, 'Source verification changed')
        u.require(_project_inputs(entry, project) == (project_head, project_records, local), 'Project HEAD/input changed during staging')
        u.require(_tree(target / 'source-raw') == raw and _tree(target / 'source-patched') == current, 'Materialized source changed')
        if local: u.require(_tree(target / 'project-inputs') == local, 'Captured project inputs changed')
        expected_bundle = {'intake-intent.json': canonical(intent).encode(), 'inputs.lock.json': lock_raw}
        for prefix, contents in (('source-raw/', raw), ('source-patched/', current), ('project-inputs/', local)):
            expected_bundle.update({prefix + name: data for name, data in contents.items()})
        u.require(_tree(target, MAX_BUNDLE_BYTES) == expected_bundle, 'Unexpected or altered candidate bundle files')
        facts = {}
        for name, data in current.items():
            if name.endswith(('.c', '.h', '.cc', '.cpp', '.hpp', '.inc', '.sh')):
                facts[name] = license_facts(data.decode('utf-8'))
        try:
            u.check_abi(entry, current); abi_matches = True
        except PortError:
            abi_matches = False
        for name in current: (target / 'source-patched' / name).chmod(0o444)
        result = {'schema': 'mellowgpu.candidate-intake/1', 'status': 'evidence_intake_unapproved',
            'component': component_id, 'remote': entry['remote'], 'commit': entry['commit'], 'profile': entry['profile'],
            'lock_sha256': digest(lock_raw), 'project_commit': project_head, 'project_inputs': project_records,
            'source_verification': verification, 'raw_sources': _manifest(raw), 'patched_sources': _manifest(current),
            'patches': apply_records, 'patches_applied_exactly': True, 'patched_abi_checks_match': abi_matches,
            'post_patch_license_observations': facts, 'license_review': entry['license_review'], 'license_review_accepted': False,
            'blocking_gaps': ['Unapproved complete dependency/license closure', 'Build and native imports not executed',
                              'Full Metal3/WindowServer/apps/games hardware conformance unavailable'] +
                             ([] if abi_matches else ['Patched source ABI does not match locked exact assertions']),
            'ready': False, 'build_eligible': False, 'hardware_qualified': False, 'production_promotion_available': False,
            'source_or_lock_modified': False, 'scripts_executed': False,
            'scope': 'Selected immutable source/project inputs and exact text-patch evidence only; not a complete or approved backend.'}
        _json(target / 'intake-result.json', result)
        return result
    except BaseException as error:
        try:
            _json(target / 'failed-intake.json', {'schema': 'mellowgpu.candidate-failure/1', 'status': 'failed',
                'component': component_id, 'error_type': type(error).__name__, 'error': str(error)[:4096],
                'ready': False, 'build_eligible': False, 'hardware_qualified': False, 'production_promotion_available': False,
                'scope': 'Partial private output only; no successful candidate was published or promoted.'})
        except BaseException:
            pass  # The durable intent still cannot authorize a candidate/build.
        raise

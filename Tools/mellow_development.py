#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared read-only identity/provenance checks for development builds and packages."""
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import stat
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from product_identity import BUNDLE, BUNDLE_ID, DEVELOPMENT_LABEL, validate_product_info


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, cwd=None, env=None):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env,
                            capture_output=True, text=True, timeout=1800)
    if result.returncode:
        raise ValueError('Command failed: ' + ' '.join(map(str, argv)) + '\n' + result.stdout + result.stderr)
    return result


def source_snapshot(repo):
    repo = Path(repo).resolve()
    if run(['git', 'status', '--porcelain', '--untracked-files=all'], repo).stdout.strip():
        raise ValueError('Development artifacts require a clean, fully committed Git HEAD')
    commit = run(['git', 'rev-parse', 'HEAD'], repo).stdout.strip()
    tree = run(['git', 'rev-parse', 'HEAD^{tree}'], repo).stdout.strip()
    algorithm = run(['git', 'rev-parse', '--show-object-format'], repo).stdout.strip()
    entries = subprocess.check_output(['git', 'ls-tree', '-rz', '--full-tree', 'HEAD'], cwd=repo)
    files = {}
    for entry in entries.split(b'\0'):
        if not entry:
            continue
        header, rawname = entry.split(b'\t', 1)
        mode, kind, blob = header.decode().split()
        name = rawname.decode()
        path = repo / name
        if mode not in ('100644', '100755') or kind != 'blob' or path.is_symlink() or not path.is_file():
            raise ValueError('Source HEAD must contain regular files only: ' + name)
        data = path.read_bytes()
        digest = hashlib.new(algorithm, b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest != blob:
            raise ValueError('Source bytes differ from Git HEAD: ' + name)
        files[name] = hashlib.sha256(data).hexdigest()
    if not files:
        raise ValueError('Empty source tree')
    if run(['git', 'status', '--porcelain', '--untracked-files=all'], repo).stdout.strip():
        raise ValueError('Source changed while checking HEAD')
    return {'commit': commit, 'tree': tree, 'files': files}


def new_output(path, repo):
    path = Path(path).absolute()
    if path.exists() or path.is_symlink():
        raise ValueError('Output already exists; choose a fresh output directory')
    resolved = path.resolve()
    if resolved == repo or repo in resolved.parents:
        relative = resolved.relative_to(repo)
        result = subprocess.run(['git', 'check-ignore', str(relative)], cwd=repo, capture_output=True)
        if result.returncode:
            raise ValueError('Output inside source checkout must be Git-ignored')
    return resolved


def tree_manifest(root):
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError('Bundle/tree must be a real directory, not a symlink')
    result = {}
    for path in sorted(root.rglob('*')):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode) or not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise ValueError('Symlink or special file in bundle/tree: ' + str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = {
                'sha256': sha(path), 'bytes': path.stat().st_size,
                'executable': bool(mode & 0o111)}
    return result


def validator(repo):
    spec = importlib.util.spec_from_file_location('mellowgpu_structural_validator', repo / 'Tools/validate-macho.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_bundle(repo, bundle, report=None):
    bundle = Path(bundle)
    manifest = tree_manifest(bundle)
    if bundle.name != BUNDLE:
        raise ValueError('Wrong product bundle name')
    info_path = bundle / 'Contents/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    validate_product_info(info)
    # An additional active/legacy kext nested in this product is a mixed tuple.
    for path in bundle.rglob('*.kext'):
        raise ValueError('Nested kext in single-product development bundle: ' + str(path))
    binary = bundle / 'Contents/MacOS' / info['CFBundleExecutable']
    actual = validator(repo).inspect_binary(binary)
    if not actual['structural_validation_passed']:
        raise ValueError('Actual Mach-O structural validation failed')
    if report is not None:
        claimed = json.loads(Path(report).read_text())
        if any(claimed.get(key) != value for key, value in actual.items()):
            raise ValueError('Fresh structural report does not match the actual binary')
        if claimed.get('bundle_identifier') != BUNDLE_ID or claimed.get('bundle_version') != info['CFBundleVersion']:
            raise ValueError('Structural report belongs to different bundle metadata')
    return manifest, binary, info_path


def verify_signature(bundle):
    verified = run(['/usr/bin/codesign', '--verify', '--strict', '--verbose=2', bundle])
    display = run(['/usr/bin/codesign', '--display', '--verbose=4', bundle])
    if 'Signature=adhoc' not in (display.stdout + display.stderr).splitlines():
        raise ValueError('Expected the explicit ad-hoc development signature')
    return {'verification': verified.stdout + verified.stderr,
            'description': display.stdout + display.stderr,
            'scope': 'ad-hoc integrity only; no Apple signing or loadability claim'}


def file_uuid(binary):
    result = run(['xcrun', 'dwarfdump', '--uuid', binary])
    rows = result.stdout.strip().splitlines()
    match = re.fullmatch(r'UUID: ([0-9A-Fa-f-]{36}) \(x86_64\) .+', rows[0]) if len(rows) == 1 else None
    if not match:
        raise ValueError('Expected one actual x86_64 Mach-O UUID')
    return match.group(1).upper()


def write_json(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def check_snapshot(before, after):
    if before != after:
        raise ValueError('Source HEAD or bytes changed during development artifact creation')

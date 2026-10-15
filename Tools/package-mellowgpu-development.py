#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package an exact clean-source native development build. Never installs or qualifies hardware."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mellow_development as D
from product_identity import BUNDLE, DEVELOPMENT_LABEL

DOCUMENTS = ('docs/MELLOWGPU-SPEC.md', 'docs/MELLOWGPU-IMPLEMENTATION.md',
             'docs/MELLOWGPU-LIFECYCLE.md', 'docs/MELLOWGPU-0.5.0-dev.md',
             'porting/metal3-profile.json', 'porting/upstreams.lock.json', 'LICENSE', 'NOTICE')


def qualification_profile(repo):
    profile = json.loads((repo / 'porting/metal3-profile.json').read_text())
    if (profile.get('profile') != 'mellowgpu-metal3-system-v1' or
            profile.get('qualified_devices') != [] or
            profile.get('api_inventory_complete') is not False or
            profile.get('hardware_attestation_implemented') is not False or
            profile.get('cpu_rasterization_qualifies') is not False or
            profile.get('cpu_pixel_transfers_qualify') is not False):
        raise ValueError('Current development packager requires the unqualified profile')
    return profile


def verify_source_archive(path, snapshot):
    """Git attributes can omit/substitute files without changing a clean HEAD."""
    found = {}
    with tarfile.open(path, 'r:gz') as archive:
        for member in archive.getmembers():
            parts = Path(member.name).parts
            if not parts or parts[0] != 'MellowGPU' or '..' in parts:
                raise ValueError('Source archive has an unexpected member path')
            if member.isdir():
                continue
            name = Path(*parts[1:]).as_posix()
            if not member.isfile() or name not in snapshot['files'] or name in found:
                raise ValueError('Source archive has an unexpected, linked or duplicate member')
            with archive.extractfile(member) as stream:
                found[name] = hashlib.sha256(stream.read()).hexdigest()
    if found != snapshot['files']:
        raise ValueError('Source archive membership or bytes differ from verified Git HEAD')


def build_evidence(repo, snapshot, bundle, structural, path):
    path = Path(path)
    receipt_bytes = path.read_bytes()
    receipt = json.loads(receipt_bytes)
    if (receipt.get('schema') != 'mellowgpu.native-build/1' or
            receipt.get('status') != 'built_signed_development_only' or
            receipt.get('hardware_tested') is not False or
            receipt.get('runtime_loaded') is not False or
            receipt.get('production_installable') is not False or
            receipt.get('native_build', {}).get('exit_code') != 0 or
            not receipt.get('native_build', {}).get('command') or not receipt.get('toolchain')):
        raise ValueError('Not a completed native development build receipt')
    if (receipt.get('source_commit') != snapshot['commit'] or
            receipt.get('source_tree') != snapshot['tree'] or
            receipt.get('source_sha256') != snapshot['files']):
        raise ValueError('Native build source tuple differs from current clean HEAD')
    manifest, binary, info = D.verify_bundle(repo, bundle, structural)
    if (manifest != receipt.get('bundle_files') or D.sha(binary) != receipt.get('binary_sha256') or
            D.sha(info) != receipt.get('info_sha256') or receipt.get('architecture') != 'x86_64' or
            D.file_uuid(binary) != receipt.get('uuid')):
        raise ValueError('Bundle differs from the native build tuple')
    evidence = receipt.get('evidence_files', {})
    if not evidence or evidence.get('structural_report', {}).get('sha256') != D.sha(structural):
        raise ValueError('Fresh structural report is not bound to native build receipt')
    validated = {}
    for label, item in evidence.items():
        rel = item.get('path')
        if not isinstance(rel, str) or not rel or Path(rel).is_absolute() or '..' in Path(rel).parts:
            raise ValueError('Unsafe build evidence path')
        source = path.parent / rel
        if source.is_symlink() or not source.is_file() or D.sha(source) != item.get('sha256'):
            raise ValueError('Build evidence bytes mismatch: ' + label)
        if not source.resolve().is_relative_to(path.parent.resolve()):
            raise ValueError('Build evidence escapes native output')
        validated[label] = source
    D.verify_signature(bundle)
    return receipt, validated, receipt_bytes


def package(repo, bundle, structural, native_receipt, out):
    repo = Path(repo).resolve()
    output = D.new_output(out, repo)
    snapshot = D.source_snapshot(repo)
    qualification_profile(repo)
    receipt, evidence, receipt_bytes = build_evidence(repo, snapshot, Path(bundle), Path(structural), Path(native_receipt))
    for name in DOCUMENTS:
        if name not in snapshot['files']:
            raise ValueError('Required package document is not in source HEAD: ' + name)
    if not any(name.startswith('LICENSES/') for name in snapshot['files']):
        raise ValueError('Missing committed component licenses')
    license_files = D.tree_manifest(repo / 'LICENSES')
    if {('LICENSES/' + name): item['sha256'] for name, item in license_files.items()} != {
            name: digest for name, digest in snapshot['files'].items() if name.startswith('LICENSES/')}:
        raise ValueError('Component licenses differ from exact source HEAD')
    output.mkdir(parents=True)
    stage = output / 'package'
    stage.mkdir()
    shutil.copytree(bundle, stage / BUNDLE)
    for name in DOCUMENTS:
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / name, target)
    shutil.copytree(repo / 'LICENSES', stage / 'LICENSES')
    D.write_json(stage / 'SOURCE.json', snapshot)
    (stage / 'BUILD.json').write_bytes(receipt_bytes)
    evdir = stage / 'Evidence'
    evdir.mkdir()
    for label, source in evidence.items():
        # Labels are receipt keys, not caller-selected filesystem paths.
        if not label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in label):
            raise ValueError('Unsafe evidence label')
        shutil.copy2(source, evdir / (label + source.suffix))
        if D.sha(evdir / (label + source.suffix)) != receipt['evidence_files'][label]['sha256']:
            raise ValueError('Native build evidence changed while packaging')
    source_dir = stage / 'Sources'
    source_dir.mkdir()
    with (source_dir / 'MellowGPU-source.tar.gz').open('xb') as stream:
        subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=MellowGPU/', snapshot['commit']],
                       cwd=repo, stdout=stream, check=True)
    verify_source_archive(source_dir / 'MellowGPU-source.tar.gz', snapshot)
    status = {'schema': 'mellowgpu.development-package/1', 'product': 'MellowGPU',
              'development_label': DEVELOPMENT_LABEL, 'source_commit': snapshot['commit'],
              'source_tree': snapshot['tree'], 'support_status': 'unqualified',
              'qualified_devices': [], 'api_inventory_complete': False,
              'production_installable': False, 'installer_included': False,
              'hardware_tested': False, 'runtime_loaded': False,
              'metal3_verified': False, 'windowserver_verified': False,
              'scope': 'Native build, integrity and static identity evidence only; no GPU support qualification'}
    D.write_json(stage / 'STATUS.json', status)
    D.verify_bundle(repo, stage / BUNDLE, structural)
    D.verify_signature(stage / BUNDLE)
    if D.tree_manifest(stage / BUNDLE) != receipt['bundle_files']:
        raise ValueError('Copied bundle differs from exact native build')
    D.check_snapshot(snapshot, D.source_snapshot(repo))
    manifest = D.tree_manifest(stage)
    with (stage / 'SHA256SUMS').open('x') as stream:
        stream.writelines(item['sha256'] + '  ' + name + '\n' for name, item in sorted(manifest.items()))
    archive = output / ('MellowGPU-' + DEVELOPMENT_LABEL + '.zip')
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for path in sorted(stage.rglob('*')):
            if not path.is_file():
                continue
            rel = Path('MellowGPU-' + DEVELOPMENT_LABEL) / path.relative_to(stage)
            item = zipfile.ZipInfo(rel.as_posix(), date_time=(2026, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = (0o100755 if path.stat().st_mode & 0o111 else 0o100644) << 16
            data = path.read_bytes()
            relative = path.relative_to(stage).as_posix()
            if relative != 'SHA256SUMS' and hashlib.sha256(data).hexdigest() != manifest[relative]['sha256']:
                raise ValueError('Package bytes changed during archive creation')
            zipped.writestr(item, data)
    digest = D.sha(archive)
    (output / (archive.name + '.sha256')).write_text(digest + '  ' + archive.name + '\n')
    D.check_snapshot(snapshot, D.source_snapshot(repo))
    result = {'status': 'development_package_created', 'archive': str(archive),
              'archive_sha256': digest, 'binary_sha256': receipt['binary_sha256'],
              'source_commit': snapshot['commit'], 'source_tree': snapshot['tree'],
              'support_status': 'unqualified', 'production_installable': False,
              'hardware_tested': False, 'installer_executed': False,
              'drivers_loaded': False, 'kernel_collection_rebuilt': False}
    D.write_json(output / 'package-result.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--structural-report', type=Path, required=True)
    parser.add_argument('--build-evidence', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.repo, args.bundle, args.structural_report, args.build_evidence, args.out), indent=2))


if __name__ == '__main__':
    main()

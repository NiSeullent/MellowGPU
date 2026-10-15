#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build/sign an exact clean-source x86_64 development artifact; never installs or loads it."""
import argparse
import json
import os
import platform
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mellow_development as D
from product_identity import BUNDLE


def capture(output, label, argv, cwd=None, env=None):
    result = D.run(argv, cwd, env)
    (output / (label + '.stdout')).write_text(result.stdout)
    (output / (label + '.stderr')).write_text(result.stderr)
    return {'command': list(map(str, argv)), 'exit_code': result.returncode,
            'stdout': label + '.stdout', 'stderr': label + '.stderr'}


def toolchain(output):
    commands = [capture(output, 'xcode-version', ['xcodebuild', '-version']),
                capture(output, 'compiler-version', ['xcrun', 'clang', '--version']),
                capture(output, 'host-version', ['/usr/bin/sw_vers'])]
    files = {}
    for name in ('clang', 'ld'):
        path = Path(D.run(['xcrun', '--find', name]).stdout.strip()).resolve()
        files[name] = {'path': str(path), 'sha256': D.sha(path)}
    codesign = Path('/usr/bin/codesign')
    files['codesign'] = {'path': str(codesign), 'sha256': D.sha(codesign)}
    sdk = Path(D.run(['xcrun', '--show-sdk-path']).stdout.strip()).resolve()
    settings = sdk / 'SDKSettings.plist'
    if not settings.is_file():
        raise ValueError('Selected native SDK has no SDKSettings.plist')
    files['sdk_settings'] = {'path': str(settings), 'sha256': D.sha(settings)}
    return {'host': platform.platform(), 'selected_sdk': str(sdk), 'files': files}, commands


def native_build(repo, output):
    env = os.environ.copy()
    env['MELLOW_BUILD_DIR'] = str(output / 'native')
    command = ['bash', str(repo / 'Tools/cross-build-native.sh'), 'Release']
    record = capture(output, 'native-build', command, repo, env)
    settings = json.loads((output / 'native/build-settings.json').read_text())
    matches = [item['buildSettings'] for item in settings if item.get('target') == 'Mellow']
    if len(matches) != 1:
        raise ValueError('Native output must describe the exact Mellow internal target')
    selected = matches[0]
    bundle = Path(selected['TARGET_BUILD_DIR']) / selected['FULL_PRODUCT_NAME']
    if bundle.name != BUNDLE or not bundle.resolve().is_relative_to((output / 'native').resolve()):
        raise ValueError('Unexpected native product location or name')
    D.verify_bundle(repo, bundle)
    artifact = output / 'artifact' / BUNDLE
    artifact.parent.mkdir()
    shutil.copytree(bundle, artifact)
    return artifact, record


def build(repo, out):
    repo = Path(repo).resolve()
    output = D.new_output(out, repo)
    before = D.source_snapshot(repo)
    if platform.system() != 'Darwin':
        raise ValueError('Native development build requires macOS and full Xcode')
    output.mkdir(parents=True)
    try:
        tools, commands = toolchain(output)
        bundle, native = native_build(repo, output)
        commands.append(native)
        commands.append(capture(output, 'adhoc-sign', ['/usr/bin/codesign', '--force', '--sign', '-', bundle]))
        signature = D.verify_signature(bundle)
        D.write_json(output / 'signature-verification.json', signature)
        structural = output / 'structural-report.json'
        commands.append(capture(output, 'structural', [sys.executable, str(repo / 'Tools/validate-macho.py'), bundle, '--output', structural], repo))
        manifest, binary, info = D.verify_bundle(repo, bundle, structural)
        uuid = D.file_uuid(binary)
        D.check_snapshot(before, D.source_snapshot(repo))
        # Toolchain paths/settings are part of the external build-input record.
        for item in tools['files'].values():
            if D.sha(Path(item['path'])) != item['sha256']:
                raise ValueError('Native toolchain changed during build')
        evidence = {}
        for path in sorted(output.rglob('*')):
            if not path.is_file() or path.is_symlink():
                continue
            relative = path.relative_to(output).as_posix()
            # Keep logs/settings and static reports, not DerivedData object files.
            if '/DerivedData/' in relative or relative.startswith('artifact/'):
                continue
            label = 'structural_report' if path == structural else relative.replace('/', '_').replace('.', '_').replace('-', '_')
            if label in evidence:
                raise ValueError('Duplicate native evidence label')
            evidence[label] = {'path': relative, 'sha256': D.sha(path)}
        receipt = {'schema': 'mellowgpu.native-build/1', 'status': 'built_signed_development_only',
                   'source_commit': before['commit'], 'source_tree': before['tree'],
                   'source_sha256': before['files'], 'source_hash_scope': 'all tracked regular files, checked against Git HEAD before and after native compilation',
                   'bundle_path': str(bundle), 'bundle_files': manifest,
                   'binary_sha256': D.sha(binary), 'info_sha256': D.sha(info),
                   'architecture': 'x86_64', 'uuid': uuid, 'native_build': native,
                   'toolchain': tools, 'commands': commands, 'evidence_files': evidence,
                   'hardware_tested': False, 'runtime_loaded': False,
                   'production_installable': False,
                   'scope': 'Actual native compile/link and ad-hoc integrity only; not loadability, Metal 3 or WindowServer qualification'}
        D.write_json(output / 'build-result.json', receipt)
        return receipt
    except Exception as error:
        D.write_json(output / 'failed-build.json', {'status': 'failed', 'error': str(error),
            'source_commit': before['commit'], 'hardware_tested': False, 'runtime_loaded': False})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True, help='Fresh directory outside source or under a Git-ignored build directory')
    args = parser.parse_args()
    result = build(args.repo, args.out)
    print(json.dumps({key: result[key] for key in ('status', 'source_commit', 'source_tree', 'bundle_path', 'binary_sha256', 'uuid', 'hardware_tested')}, indent=2))


if __name__ == '__main__':
    main()

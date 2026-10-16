#!/usr/bin/env python3
"""Build the NativeGlkCompiler adapter against the pinned upstream Mesa.

Verifies (or, with --fetch, fetches) Mesa f5cb8ee0 from the official remote by
commit and tree id, applies only the recorded patches, configures Mesa's own
meson recipe with the locked options inside a private venv built from
hash-pinned wheels, builds the Intel brw/NIR/vtn/ISL/devinfo static libraries
with at most two parallel jobs, then compiles and links the adapter, its host
test and the mglk-compile offline compiler executable.  Archive and link steps
run with ZERO_AR_DATE=1 so a rebuild of unchanged inputs is byte-identical;
when a previous manifest exists its artifact hashes are compared.  Writes a
manifest with every command, flag, tool version and hash.

Host build only: no GPU, kext, SSH, driver load or install.  A successful
build is not GPU or Metal qualification.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
COMPONENT = ROOT / 'Userspace' / 'NativeGlkCompiler'
LOCK = COMPONENT / 'provenance' / 'mesa-glk-compiler.lock.json'
ADAPTER_SOURCES = ['src/mellow_glk_compiler.c']
TEST_SOURCES = ['tests/glk_compiler_tests.c']
CLI_SOURCES = ['tools/mglk_compile.c']
HEADERS = ['include/mellow_glk_compiler.h']
# Kernel-interface imports that must only come from upstream device-query code
# the adapter never calls (it resolves devices from the static PCI table).
KERNEL_INTERFACE_SYMBOLS = ('_ioctl',)
MAX_JOBS = 2
ADAPTER_WARNINGS = ['-Wall', '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
                    '-Werror=int-conversion', '-Werror=return-type', '-Werror=missing-prototypes',
                    '-Werror=format', '-Wno-unused-function']
DEBUG_ENV = ('INTEL_DEBUG', 'NIR_DEBUG', 'INTEL_SIMD_DEBUG', 'MESA_SPIRV_DEBUG', 'MESA_SHADER_CACHE_DISABLE',
             'BRW_DEBUG', 'CCC_OVERRIDE_OPTIONS')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


class Builder:
    def __init__(self, args, lock):
        self.args = args
        self.lock = lock
        self.work = args.work
        self.mesa = self.work / 'src' / 'mesa'
        self.build = self.work / 'build' / 'mesa'
        self.venv = self.work / 'venv'
        self.wheels = self.work / 'wheels'
        self.out = self.work / 'adapter'
        self.steps = []

    def run(self, command, cwd=None, env=None, check=True, quiet=False):
        command = [str(part) for part in command]
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
        output = (result.stdout + result.stderr)
        self.steps.append(dict(command=command, cwd=str(cwd) if cwd else None, exit=result.returncode,
                               output_tail=output[-4000:]))
        if check and result.returncode != 0:
            raise SystemExit(f'command failed ({result.returncode}): {shlex.join(command)}\n{output[-8000:]}')
        if not quiet:
            print(f'[{result.returncode}] {shlex.join(command)[:200]}', flush=True)
        return result.stdout

    def tool_env(self):
        ninja_dir = os.path.dirname(self.args.ninja)
        env = {key: value for key, value in os.environ.items() if key not in DEBUG_ENV}
        env['PATH'] = os.pathsep.join([str(self.venv / 'bin'), '/usr/bin', '/bin', '/usr/sbin', '/sbin', ninja_dir])
        env.pop('PYTHONPATH', None)
        env.pop('PYTHONHOME', None)
        return env

    def source(self):
        pin = self.lock['mesa']
        if not (self.mesa / '.git').exists():
            if not self.args.fetch:
                raise SystemExit(f'{self.mesa} missing; rerun with --fetch to fetch {pin["commit"]} from {pin["remote"]}')
            self.mesa.mkdir(parents=True, exist_ok=True)
            self.run(['git', '-C', self.mesa, 'init', '-q'])
            self.run(['git', '-C', self.mesa, 'remote', 'add', 'origin', pin['remote']])
            self.run(['git', '-C', self.mesa, 'fetch', '-q', '--depth', '1', 'origin', pin['commit']])
            self.run(['git', '-C', self.mesa, 'checkout', '-q', 'FETCH_HEAD'])
        remote = self.run(['git', '-C', self.mesa, 'remote', 'get-url', 'origin'], quiet=True).strip()
        head = self.run(['git', '-C', self.mesa, 'rev-parse', 'HEAD'], quiet=True).strip()
        tree = self.run(['git', '-C', self.mesa, 'rev-parse', 'HEAD^{tree}'], quiet=True).strip()
        commit = self.run(['git', '-C', self.mesa, 'cat-file', '-p', 'HEAD'], quiet=True)
        parent = re.search(r'^parent ([0-9a-f]{40})$', commit, re.M)
        version = (self.mesa / 'VERSION').read_text().strip()
        if remote.removesuffix('.git') != pin['remote'].removesuffix('.git'):
            raise SystemExit(f'unexpected Mesa remote {remote}')
        if (head, tree, parent and parent.group(1), version) != (pin['commit'], pin['tree'], pin['parent'],
                                                                  pin['version_file']):
            raise SystemExit(f'Mesa checkout is {head} tree {tree} version {version}; lock requires '
                             f'{pin["commit"]} tree {pin["tree"]}')
        return dict(remote=remote, commit=head, tree=tree, parent=parent.group(1), version=version)

    def archive(self):
        pin = self.lock['mesa']['archive']
        path = self.work / 'archive' / pin['file']
        if not path.exists() and self.args.fetch:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.run(['curl', '-sS', '-L', '--fail', '--max-time', '600', '-o', path, pin['url']])
        if not path.exists():
            return dict(present=False, expected_sha256=pin['sha256'])
        actual = sha256(path)
        if actual != pin['sha256']:
            raise SystemExit(f'archive {path} sha256 {actual} != locked {pin["sha256"]}')
        return dict(present=True, path=str(path), sha256=actual, size_bytes=path.stat().st_size)

    def patches(self):
        applied = []
        for patch in self.lock['patches']:
            path = COMPONENT / patch['path']
            digest = sha256(path)
            if digest != patch['sha256']:
                raise SystemExit(f'{path} sha256 {digest} != locked {patch["sha256"]}')
            forward = subprocess.run(['git', '-C', str(self.mesa), 'apply', '--check', str(path)],
                                     capture_output=True)
            if forward.returncode == 0:
                self.run(['git', '-C', self.mesa, 'apply', path])
                state = 'applied-now'
            elif subprocess.run(['git', '-C', str(self.mesa), 'apply', '--check', '-R', str(path)],
                                capture_output=True).returncode == 0:
                state = 'already-applied'
            else:
                raise SystemExit(f'{path} neither applies nor is already applied to the pinned tree')
            applied.append(dict(path=patch['path'], sha256=digest, state=state))
        status = self.run(['git', '-C', self.mesa, 'status', '--porcelain'], quiet=True).split('\n')
        dirty = sorted(line[3:] for line in status if line.strip())
        allowed = sorted({name for patch in self.lock['patches'] for name in patch['files']})
        if dirty != allowed:
            raise SystemExit(f'Mesa working tree differs beyond recorded patches: {dirty}')
        diff = self.run(['git', '-C', self.mesa, 'diff', '--no-color'], quiet=True)
        return dict(patches=applied, modified_files=dirty,
                    worktree_diff_sha256=hashlib.sha256(diff.encode()).hexdigest())

    def python_env(self):
        pins = self.lock['python']['wheels']
        self.wheels.mkdir(parents=True, exist_ok=True)
        python = shutil.which(self.args.python) or self.args.python
        for wheel in pins:
            path = self.wheels / wheel['file']
            if not path.exists():
                if not self.args.fetch:
                    raise SystemExit(f'{path} missing; rerun with --fetch')
                self.run([python, '-m', 'pip', 'download', '--no-deps', '--only-binary=:all:', '-d', self.wheels,
                          wheel['requirement']])
            digest = sha256(path)
            if digest != wheel['sha256']:
                raise SystemExit(f'{path} sha256 {digest} != locked {wheel["sha256"]}')
        if not (self.venv / 'bin' / 'python').exists():
            self.run([python, '-m', 'venv', self.venv])
            self.run([self.venv / 'bin' / 'python', '-m', 'pip', 'install', '-q', '--no-index', '--no-deps',
                      *[self.wheels / wheel['file'] for wheel in pins]])
        probe = self.run([self.venv / 'bin' / 'python', '-I', '-c',
                          'import sys, mako, yaml, packaging, mesonbuild.coredata as c;'
                          'print(sys.version.split()[0], mako.__version__, yaml.__version__, '
                          'packaging.__version__, c.version)'], quiet=True).split()
        expected = [wheel['requirement'].split('==')[1] for wheel in pins]
        if [probe[1], probe[3], probe[4]] != [expected[0], expected[3], expected[4]] or probe[2] != expected[2]:
            raise SystemExit(f'venv module versions {probe} do not match lock {expected}')
        return dict(venv=str(self.venv), python=probe[0], mako=probe[1], pyyaml=probe[2], packaging=probe[3],
                    meson=probe[4], wheels={wheel['file']: wheel['sha256'] for wheel in pins})

    def configure(self):
        options = self.lock['meson_options']
        flags = [f'-D{key}={value}' for key, value in options.items() if key not in ('wrap_mode', 'buildtype')]
        flags = [f'--wrap-mode={options["wrap_mode"]}', f'--buildtype={options["buildtype"]}', *flags]
        meson = self.venv / 'bin' / 'meson'
        env = self.tool_env()
        if not (self.build / 'build.ninja').exists():
            self.build.parent.mkdir(parents=True, exist_ok=True)
            self.run([meson, 'setup', self.build, self.mesa, *flags], env=env)
        current = json.loads(self.run([meson, 'introspect', '--buildoptions', self.build], env=env, quiet=True))
        values = {item['name']: item['value'] for item in current}
        mismatched = []
        for key, value in options.items():
            have = values.get(key)
            have = ','.join(have) if isinstance(have, list) else str(have).lower() if isinstance(have, bool) else have
            if str(have) != value:
                mismatched.append((key, have, value))
        if mismatched:
            self.run([meson, 'setup', '--reconfigure', self.build, self.mesa, *flags], env=env)
        return dict(build_dir=str(self.build), flags=flags, reconfigured_for=mismatched)

    def compile_mesa(self):
        jobs = min(self.args.jobs, MAX_JOBS)
        self.run([self.args.ninja, '-C', self.build, f'-j{jobs}', *self.lock['ninja_targets']], env=self.tool_env())
        return {target: sha256(self.build / target) for target in self.lock['ninja_targets']}

    def upstream_flags(self):
        commands = json.loads((self.build / 'compile_commands.json').read_text())
        wanted = self.lock['adapter_flags_from']
        entry = next((item for item in commands if item['file'].endswith(wanted)), None)
        if entry is None:
            raise SystemExit(f'{wanted} not found in compile_commands.json')
        tokens = shlex.split(entry['command'])
        flags = []
        for token in tokens:
            if token.startswith('-I'):
                flags.append('-I' + str((self.build / token[2:]).resolve()))
            elif token.startswith('-D') or token.startswith('-std=') or token in ('-fno-math-errno',
                                                                                  '-fno-trapping-math',
                                                                                  '-fno-common'):
                flags.append(token)
        if any(flag == '-DNDEBUG' for flag in flags):
            raise SystemExit('upstream objects were built with NDEBUG; lock requires assert-enabled build')
        return flags, entry

    def compile_adapter(self):
        flags, entry = self.upstream_flags()
        obj = self.out / 'obj'
        obj.mkdir(parents=True, exist_ok=True)
        (self.out / 'lib').mkdir(exist_ok=True)
        (self.out / 'bin').mkdir(exist_ok=True)
        # ld64/ar honour ZERO_AR_DATE: no member or debug-map timestamps.
        env = {key: value for key, value in os.environ.items() if key not in DEBUG_ENV}
        env['ZERO_AR_DATE'] = '1'
        common = [self.args.cc, '-c', '-O2', '-g', *flags, '-I', COMPONENT / 'include', *ADAPTER_WARNINGS]
        commands, objects = [], {}
        for source in ADAPTER_SOURCES + TEST_SOURCES + CLI_SOURCES:
            target = obj / (Path(source).stem + '.o')
            commands.append([*common, COMPONENT / source, '-o', target])
            self.run(commands[-1], env=env)
            objects[source] = target
        library = self.out / 'lib' / 'libmellow_glk_compiler.a'
        library.unlink(missing_ok=True)
        commands.append(['ar', 'rcs', library, *[objects[source] for source in ADAPTER_SOURCES]])
        self.run(commands[-1], env=env)
        libraries = [self.build / target for target in self.lock['ninja_targets']]
        binaries = {}
        for name, source in (('glk_compiler_tests', TEST_SOURCES[0]), ('mglk-compile', CLI_SOURCES[0])):
            binary = self.out / 'bin' / name
            commands.append([self.args.cxx, '-o', binary, objects[source], library, *libraries, '-lz', '-lm',
                             '-lpthread'])
            self.run(commands[-1], env=env)
            undefined = sorted(set(self.run(['nm', '-u', binary], quiet=True).split()))
            # The -g debug map names every archive member the linker pulled in.
            linked = {line.rsplit(' OSO ', 1)[1] for line in self.run(['nm', '-ap', binary], quiet=True).splitlines()
                      if ' OSO ' in line}
            binaries[name] = dict(path=str(binary), sha256=sha256(binary), undefined_symbols=undefined,
                                  linked_members=linked)
        imported = sorted({symbol for item in binaries.values() for symbol in item['undefined_symbols']} &
                          set(KERNEL_INTERFACE_SYMBOLS))
        linked = set().union(*(item.pop('linked_members') for item in binaries.values()))
        referencing = []
        for target in self.lock['ninja_targets']:
            listing = self.run(['nm', '-A', self.build / target], check=False, quiet=True)
            members = {line.split(':')[1] for line in listing.splitlines()
                       if line.split()[-2:] in (['U', symbol] for symbol in imported)}
            referencing += sorted(f'{target}({member})' for member in members
                                  if f'{self.build / target}({member})' in linked)
        return dict(flags=flags, flags_source=entry['file'], warnings=ADAPTER_WARNINGS,
                    environment=dict(ZERO_AR_DATE='1'), commands=[[str(part) for part in command]
                                                                  for command in commands],
                    objects={source: sha256(path) for source, path in objects.items()},
                    library=str(library), library_sha256=sha256(library),
                    binary=binaries['glk_compiler_tests']['path'],
                    binary_sha256=binaries['glk_compiler_tests']['sha256'],
                    binary_undefined_symbols=binaries['glk_compiler_tests']['undefined_symbols'],
                    cli=binaries['mglk-compile']['path'], cli_sha256=binaries['mglk-compile']['sha256'],
                    cli_undefined_symbols=binaries['mglk-compile']['undefined_symbols'],
                    kernel_interface_imports=dict(
                        symbols=imported, linked_members_referencing=referencing,
                        note='linked upstream device-info/engine query objects; the adapter only calls '
                             'intel_get_device_info_from_pci_id and never opens a DRM fd or issues ioctl'))

    def inventory(self):
        commands = json.loads((self.build / 'compile_commands.json').read_text())
        prefixes = tuple(target + '.p/' for target in self.lock['ninja_targets'])
        files = sorted({str((self.build / item['file']).resolve()) for item in commands
                        if item.get('output', '').startswith(prefixes)})
        mesa_root = str(self.mesa.resolve()) + '/'
        build_root = str(self.build.resolve()) + '/'
        tracked = [path[len(mesa_root):] for path in files if path.startswith(mesa_root)]
        blobs = {}
        if tracked:
            listing = self.run(['git', '-C', self.mesa, 'ls-files', '-s', '--', *tracked], quiet=True)
            for line in listing.splitlines():
                meta, name = line.split('\t', 1)
                blobs[name] = meta.split()[1]
        records, licenses, directory_licenses = [], {}, {}
        for path in files:
            if path.startswith(mesa_root):
                rel, origin = path[len(mesa_root):], 'upstream'
            else:
                rel, origin = path[len(build_root):] if path.startswith(build_root) else path, 'generated'
            text = Path(path).read_text(errors='replace')[:6000]
            spdx = re.search(r'SPDX-License-Identifier:\s*([^\s*]+(?:\s+(?:OR|AND|WITH)\s+[^\s*]+)*)', text)
            sibling = next((Path(path).parent / name for name in ('LICENSE', 'LICENSE.txt', 'COPYING')
                            if (Path(path).parent / name).is_file()), None)
            if spdx:
                license_id = spdx.group(1).strip()
            elif 'Permission is hereby granted, free of charge' in text:
                license_id = 'MIT (license text)'
            elif 'Boost Software License' in text:
                license_id = 'BSL-1.0 (license text)'
            elif 'Apache License' in text:
                license_id = 'Apache-2.0 (license text)'
            elif 'License for Berkeley SoftFloat' in text:
                license_id = 'BSD-3-Clause (Berkeley SoftFloat 3e license text)'
            elif 'public domain' in text.lower() or 'CC0' in text:
                license_id = 'public-domain/CC0 (text)'
            elif sibling and str(sibling).startswith(mesa_root):
                notice = str(sibling)[len(mesa_root):]
                directory_licenses[notice] = sha256(sibling)
                license_id = f'NO-HEADER (directory notice {notice})'
            elif origin == 'generated':
                license_id = 'NO-HEADER (build-generated by a Mesa generator script)'
            else:
                license_id = 'NO-HEADER (no per-file notice; see docs/license.rst)'
            licenses[license_id] = licenses.get(license_id, 0) + 1
            records.append(dict(path=rel, origin=origin, git_blob=blobs.get(rel), sha256=sha256(path),
                                license=license_id))
        notices = {name: dict(sha256=sha256(self.mesa / name),
                              git_blob=self.run(['git', '-C', self.mesa, 'rev-parse', f'HEAD:{name}'],
                                                quiet=True).strip())
                   for name in self.lock['mesa']['license_files']}
        destination = self.work / 'provenance'
        destination.mkdir(exist_ok=True)
        path = destination / 'compiled-sources.json'
        path.write_text(json.dumps(dict(mesa_commit=self.lock['mesa']['commit'], license_files=notices,
                                        directory_licenses=directory_licenses, files=records), indent=1) + '\n')
        return dict(path=str(path), sha256=sha256(path), compiled_files=len(records),
                    generated_files=sum(1 for record in records if record['origin'] == 'generated'),
                    licenses=licenses, license_files=notices, directory_licenses=directory_licenses)


def tool_versions(args, builder):
    def first(command):
        try:
            result = subprocess.run(command, capture_output=True, text=True)
            return (result.stdout + result.stderr).strip().splitlines()[0]
        except (OSError, IndexError):
            return None
    return dict(cc=first([args.cc, '--version']), cxx=first([args.cxx, '--version']),
                ninja=first([args.ninja, '--version']), git=first(['git', '--version']),
                sdk=first(['xcrun', '--show-sdk-path']), host=platform.platform(), machine=platform.machine())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, required=True,
                        help='Out-of-tree directory holding Mesa source, venv, build and outputs')
    parser.add_argument('--fetch', action='store_true',
                        help='Allow fetching missing pinned source/archive/wheels from their official origins')
    parser.add_argument('--jobs', type=int, default=MAX_JOBS, help=f'Native compile jobs (capped at {MAX_JOBS})')
    parser.add_argument('--python', default='python3.13', help='Interpreter used to create the private venv')
    parser.add_argument('--cc', default='/usr/bin/clang')
    parser.add_argument('--cxx', default='/usr/bin/clang++')
    parser.add_argument('--ninja', default=shutil.which('ninja') or '/opt/homebrew/bin/ninja')
    args = parser.parse_args()
    args.work = args.work.resolve()
    if args.work == ROOT or ROOT in args.work.parents:
        parser.error('--work must be outside the source checkout')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    present = [key for key in DEBUG_ENV if os.environ.get(key)]
    if 'CCC_OVERRIDE_OPTIONS' in present:
        raise SystemExit('CCC_OVERRIDE_OPTIONS changes compiler arguments; refusing evidence generation')
    lock = json.loads(LOCK.read_text())
    builder = Builder(args, lock)
    manifest = dict(
        scope='Host (Darwin arm64) build of pinned upstream Mesa Intel compiler libraries and the '
              'NativeGlkCompiler adapter. No GPU, MMIO, RCS submission, kernel upload or hardware execution. '
              'Not GPU or Metal qualification.',
        gpu_execution='NOT_RUN', hardware_admission=False, metal_qualification=False, qualified_profiles=[],
        generated=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        lock=dict(path=str(LOCK.relative_to(ROOT)), sha256=sha256(LOCK)),
        inputs={str(path.relative_to(ROOT)): sha256(path) for path in
                [COMPONENT / name for name in ADAPTER_SOURCES + TEST_SOURCES + CLI_SOURCES + HEADERS] +
                [Path(__file__).resolve(), LOCK] + [COMPONENT / patch['path'] for patch in lock['patches']]},
        debug_environment_ignored=present,
        max_jobs=min(args.jobs, MAX_JOBS))
    manifest['source'] = builder.source()
    manifest['archive'] = builder.archive()
    manifest['patches'] = builder.patches()
    manifest['python'] = builder.python_env()
    manifest['meson'] = builder.configure()
    manifest['static_libraries'] = builder.compile_mesa()
    manifest['adapter'] = builder.compile_adapter()
    manifest['inventory'] = builder.inventory()
    manifest['tools'] = tool_versions(args, builder)
    manifest['steps'] = builder.steps
    path = args.work / 'build-manifest.json'
    manifest['rebuild_comparison'] = compare_previous(path, manifest)
    path.write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'build manifest: {path}\nadapter test binary: {manifest["adapter"]["binary"]}\n'
          f'offline compiler: {manifest["adapter"]["cli"]}\n'
          f'rebuild comparison: {manifest["rebuild_comparison"]["verdict"]}')


def compare_previous(path, manifest):
    """Compares artifact hashes with the previous manifest when inputs match."""
    if not path.exists():
        return dict(verdict='NO_PREVIOUS_BUILD')
    previous = json.loads(path.read_text())
    if previous.get('inputs') != manifest['inputs'] or previous.get('source') != manifest['source']:
        return dict(verdict='INPUTS_CHANGED', previous_generated=previous.get('generated'))
    def artifacts(item):
        adapter = item.get('adapter', {})
        found = dict(item.get('static_libraries', {}))
        found.update({f'adapter:{key}': adapter.get(key) for key in ('library_sha256', 'binary_sha256',
                                                                      'cli_sha256')})
        found.update({f'object:{key}': value for key, value in adapter.get('objects', {}).items()})
        return found
    old, new = artifacts(previous), artifacts(manifest)
    differing = sorted(key for key in new if old.get(key) != new[key])
    return dict(verdict='IDENTICAL' if not differing else 'DIFFERENT', previous_generated=previous.get('generated'),
                compared=len(new), differing=differing)


if __name__ == '__main__':
    main()

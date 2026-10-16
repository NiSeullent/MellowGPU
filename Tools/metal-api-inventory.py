#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract declaration evidence from a local Apple SDK; never qualifies an API/GPU."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import tempfile

KINDS = {'ObjCProtocolDecl', 'ObjCInterfaceDecl', 'ObjCCategoryDecl',
         'ObjCMethodDecl', 'ObjCPropertyDecl', 'FunctionDecl', 'EnumDecl',
         'EnumConstantDecl', 'TypedefDecl', 'RecordDecl', 'VarDecl'}
OWNERS = {'ObjCProtocolDecl', 'ObjCInterfaceDecl', 'ObjCCategoryDecl', 'EnumDecl', 'RecordDecl'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compiler_environment_guard():
    # These documented driver/include inputs must not redefine the SDK census.
    for name in ('CCC_OVERRIDE_OPTIONS', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
                 'OBJC_INCLUDE_PATH', 'OBJCPLUS_INCLUDE_PATH'):
        if name in os.environ:
            raise ValueError('Compiler environment requires ' + name + ' to be unset')


def compiler_environment():
    compiler_environment_guard()
    # Preserve explicit tool selection and temporary-file operation only. In
    # particular PWD, HOME/user config, SDKROOT, RC_DEBUG_OPTIONS and date/macro
    # overrides are not inherited by the compiler evidence subprocess.
    result = {key: os.environ[key] for key in ('PATH', 'DEVELOPER_DIR', 'TMPDIR',
              'TMP', 'TEMP', 'SYSTEMROOT', 'WINDIR') if key in os.environ}
    return dict(result, LC_ALL='C', LANG='C')


def read_inventory(path):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON key: ' + key)
            result[key] = value
        return result
    result = json.loads(Path(path).read_text(), object_pairs_hook=unique_object)
    if not isinstance(result, dict):
        raise ValueError('Inventory must be one JSON object')
    return result


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d{1,3}(?:\.\d{1,3}){0,2}', value):
        raise ValueError('Expected a numeric macOS version')
    return tuple(map(int, value.split('.'))) + (0,) * (3 - len(value.split('.')))


def parse_ast_stream(text):
    """Clang's name-filtered JSON is a sequence of top-level JSON objects."""
    decoder, offset, roots = json.JSONDecoder(), 0, []
    try:
        while offset < len(text):
            while offset < len(text) and text[offset].isspace():
                offset += 1
            if offset == len(text):
                break
            node, offset = decoder.raw_decode(text, offset)
            if not isinstance(node, dict) or not isinstance(node.get('kind'), str):
                raise ValueError('Invalid Clang AST object')
            roots.append(node)
    except (json.JSONDecodeError, TypeError) as error:
        raise ValueError('Invalid/truncated Clang AST stream') from error
    if not roots:
        raise ValueError('Empty Clang AST stream')
    materialize_location_files(roots)
    return roots


def materialize_location_files(roots):
    """Expand Clang's document-order file elision before filtering any nodes."""
    last = None
    def walk(value):
        nonlocal last
        if isinstance(value, list):
            for child in value: walk(child)
        elif isinstance(value, dict):
            # includedFrom.file describes include ancestry, not LastLocFilename.
            if 'offset' in value and 'col' in value:
                if isinstance(value.get('file'), str): last = value['file']
                elif last is not None: value['file'] = last
            for key, child in list(value.items()):
                if key != 'includedFrom': walk(child)
    walk(roots)


def command_evidence(clang, command, sdk, source):
    """Record the reported cc1 argv for this exact request, not a generic probe."""
    result = subprocess.run([str(clang), '-###', *command[1:]], capture_output=True,
                            text=True, timeout=30, env=compiler_environment())
    if result.returncode:
        raise ValueError('Compiler command trace failed')
    commands = []
    for line in (result.stdout + '\n' + result.stderr).splitlines():
        if not line.lstrip().startswith('"'): continue
        try: tokens = shlex.split(line)
        except ValueError as error: raise ValueError('Malformed compiler command trace') from error
        if '-cc1' in tokens: commands.append(tokens)
    if len(commands) != 1 or not Path(commands[0][0]).is_absolute():
        raise ValueError('Compiler request must report exactly one absolute frontend')
    tokens = commands[0]
    frontend = Path(tokens[0]).resolve()
    if not frontend.is_file(): raise ValueError('Compiler request frontend is not a regular file')
    sdk = str(Path(sdk).resolve())
    sources = {str(Path(source)), str(Path(source).resolve())}
    def option(argv, name):
        if argv.count(name) != 1 or argv.index(name) + 1 >= len(argv):
            raise ValueError('Compiler request lacks one ' + name)
        return argv[argv.index(name) + 1]
    requested = re.fullmatch(r'x86_64-apple-macos(\d+(?:\.\d+){0,2})', option(command, '-target'))
    effective = re.fullmatch(r'x86_64-apple-macosx?(\d+(?:\.\d+){0,2})', option(tokens, '-triple'))
    if (not requested or not effective or version(requested.group(1)) != version(effective.group(1)) or
        Path(option(tokens, '-isysroot')).resolve() != Path(sdk)):
        raise ValueError('Compiler request effective target/SDK differs')
    resource = None
    if tokens.count('-resource-dir') == 1 and tokens.index('-resource-dir') + 1 < len(tokens):
        resource = tokens[tokens.index('-resource-dir') + 1]
    normalized = []
    for index, token in enumerate(tokens):
        if index == 0: token = '<FRONTEND>'
        elif token in sources: token = '<SOURCE>'
        elif tokens[index - 1] in ('-fdebug-compilation-dir', '-fcoverage-compilation-dir'):
            token = '<WORKING_DIRECTORY>'
        elif token.startswith(('-fdebug-compilation-dir=', '-fcoverage-compilation-dir=')):
            token = token.split('=', 1)[0] + '=<WORKING_DIRECTORY>'
        elif token == sdk or token.startswith(sdk + os.sep): token = '<SDK>' + token[len(sdk):]
        elif resource and (token == resource or token.startswith(resource + os.sep)):
            token = '<COMPILER_RESOURCE>' + token[len(resource):]
        elif token.startswith('/'):
            token = '<ABSOLUTE_PATH_SHA256:' + hashlib.sha256(token.encode()).hexdigest() + '>'
        elif '=/' in token:
            prefix, private_path = token.split('=', 1)
            token = prefix + '=<ABSOLUTE_PATH_SHA256:' + hashlib.sha256(private_path.encode()).hexdigest() + '>'
        normalized.append(token)
    return {'frontend_sha256': sha(frontend), 'normalized_tokens': normalized,
            'normalized_sha256': hashlib.sha256(json.dumps(normalized, separators=(',', ':')).encode()).hexdigest(),
            'scope': 'Reported -### command for this exact request; selected compiler is trusted input, not execution attestation'}


def record_command(evidence, key, clang, command, sdk, source):
    if evidence is None: return
    current = command_evidence(clang, command, sdk, source)
    if key in evidence and evidence[key] != current:
        raise ValueError('Compiler command changed during capture')
    evidence[key] = current


def clang_ast(clang, source, arguments, name_filter='MTL', language='objective-c', evidence=None, sdk=None):
    compiler_environment_guard()
    command = [str(clang), '-x', language, '-fsyntax-only', '-fno-modules',
               '-Wno-availability', '-Xclang', '-ast-dump=json']
    if name_filter is not None:
        command += ['-Xclang', '-ast-dump-filter', '-Xclang', name_filter]
    command += [*map(str, arguments), str(source)]
    record_command(evidence, 'ast', clang, command, sdk, source)
    result = subprocess.run(command, capture_output=True, text=True, timeout=180, env=compiler_environment())
    if result.returncode:
        raise ValueError('Clang AST extraction failed:\n' + result.stderr[-12000:])
    return parse_ast_stream(result.stdout)


def compiler_identity(clang, sdk, target, language='objective-c'):
    """Bind the explicit invocation and its reported frontend, without xcrun substitution."""
    compiler_environment_guard()
    version(target)
    invocation = Path(clang).resolve()
    invocation_before = sha(invocation)

    def query(argv):
        result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=30, env=compiler_environment())
        if result.returncode:
            raise ValueError('Compiler identity query failed')
        return result

    reported = query([clang, '-print-prog-name=clang']).stdout.strip()
    if '\n' in reported or not Path(reported).is_absolute():
        raise ValueError('Compiler frontend report must identify one absolute file')
    frontend = Path(reported).resolve()
    if not frontend.is_file():
        raise ValueError('Compiler frontend is not a regular file')
    frontend_before = sha(frontend)
    with tempfile.TemporaryDirectory(prefix='mellow-compiler-identity-') as temporary:
        source = Path(temporary) / 'identity.m'
        source.write_text('/* syntax-only driver selection probe */\n')
        trace = query([clang, '-###', '-x', language, '-fsyntax-only', '-fno-modules',
                       '-target', 'x86_64-apple-macos' + target, '-isysroot', Path(sdk).resolve(), source])
    commands = []
    for line in (trace.stdout + '\n' + trace.stderr).splitlines():
        if not line.lstrip().startswith('"'):
            continue
        try: tokens = shlex.split(line)
        except ValueError as error: raise ValueError('Malformed compiler driver trace') from error
        if '-cc1' in tokens:
            commands.append(tokens)
    if len(commands) != 1:
        raise ValueError('Compiler driver must select exactly one frontend command')
    command = commands[0]
    if not Path(command[0]).is_absolute() or Path(command[0]).resolve() != frontend:
        raise ValueError('Compiler reported frontend and driver trace disagree')

    def single_option(name):
        if command.count(name) != 1 or command.index(name) + 1 >= len(command):
            raise ValueError('Compiler driver trace lacks one ' + name)
        return command[command.index(name) + 1]

    triple = single_option('-triple')
    match = re.fullmatch(r'x86_64-apple-macosx?(\d+(?:\.\d+){0,2})', triple)
    if not match or version(match.group(1)) != version(target):
        raise ValueError('Compiler effective target differs from requested target')
    if Path(single_option('-isysroot')).resolve() != Path(sdk).resolve():
        raise ValueError('Compiler effective SDK differs from requested SDK')
    normalized = []
    for index, token in enumerate(command):
        if index == 0:
            token = '<FRONTEND>'
        elif token == str(source):
            token = '<PROBE_SOURCE>'
        elif command[index - 1] in ('-fdebug-compilation-dir', '-fcoverage-compilation-dir'):
            token = '<WORKING_DIRECTORY>'
        elif token.startswith(('-fdebug-compilation-dir=', '-fcoverage-compilation-dir=')):
            token = token.split('=', 1)[0] + '=<WORKING_DIRECTORY>'
        elif token == str(Path(sdk).resolve()) or token.startswith(str(Path(sdk).resolve()) + os.sep):
            token = '<SDK>' + token[len(str(Path(sdk).resolve())):]
        normalized.append(token)
    command_sha = hashlib.sha256(json.dumps(normalized, separators=(',', ':')).encode()).hexdigest()
    driver_version = query([clang, '--version']).stdout.splitlines()
    frontend_version = query([frontend, '--version']).stdout.splitlines()
    if not driver_version or not frontend_version:
        raise ValueError('Compiler version query is empty')
    if sha(invocation) != invocation_before or sha(frontend) != frontend_before:
        raise ValueError('Compiler binary changed during identity query')
    return {'invocation': {'name': Path(clang).name, 'sha256': invocation_before, 'version': driver_version[0]},
            'frontend': {'name': frontend.name, 'sha256': frontend_before, 'version': frontend_version[0]},
            'requested_target': 'x86_64-apple-macos' + target, 'effective_target': triple,
            'frontend_command': {'normalized_sha256': command_sha,
                'normalization': 'Frontend executable, temporary probe source, SDK root and debug/coverage working directories replaced with fixed labels; other tokens unchanged'},
            'execution_environment_policy': 'minimal-v1: PATH/DEVELOPER_DIR/temp/system-root only; C locale; driver/include overrides rejected',
            'language': language, 'selection_evidence': 'driver -print-prog-name=clang and single -### -cc1 command agree',
            'scope': 'Reported compiler selection and exact binary bytes; no process/hardware attestation'}


def verify_compiler_identity(expected, clang, sdk, target, language='objective-c'):
    if compiler_identity(clang, sdk, target, language) != expected:
        raise ValueError('Compiler invocation/frontend/version/target identity drift')


def header_manifest(root):
    root = Path(root).resolve()
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('Linked SDK header tree member')
        if path.is_file() and path.suffix == '.h':
            result[path.relative_to(root).as_posix()] = {'sha256': sha(path), 'bytes': path.stat().st_size}
    if not result:
        raise ValueError('No SDK headers')
    return result


def dependency_manifest(clang, source, arguments, sdk, evidence=None):
    """Pin macro-only SDK support inputs as well as declarations' own headers."""
    compiler_environment_guard()
    sdk, source = Path(sdk).resolve(), Path(source).resolve()
    resource_result = subprocess.run([str(clang), '-print-resource-dir'], capture_output=True, text=True, check=True, timeout=15, env=compiler_environment())
    resource = Path(resource_result.stdout.strip()).resolve()
    command = [str(clang), '-x', 'objective-c', '-fno-modules', '-M', '-MT', 'inventory', *map(str, arguments), str(source)]
    record_command(evidence, 'dependencies', clang, command, sdk, source)
    result = subprocess.run(command, capture_output=True, text=True, timeout=180, env=compiler_environment())
    if result.returncode or not result.stdout.startswith('inventory:'):
        raise ValueError('Clang dependency extraction failed')
    files = {'sdk': {}, 'compiler_resource': {}}
    for name in shlex.split(result.stdout.split(':', 1)[1].replace('\\\n', ' ')):
        file = Path(name).resolve()
        if file == source: continue
        group = 'sdk' if file.is_relative_to(sdk) else 'compiler_resource' if file.is_relative_to(resource) else None
        if group is None:
            raise ValueError('Clang consumed a header outside the selected SDK/compiler')
        relative = file.relative_to(sdk if group == 'sdk' else resource).as_posix()
        files[group][relative] = {'sha256': sha(file), 'bytes': file.stat().st_size}
    return files


def verify_dependency_manifest(expected, sdk, clang, target):
    arguments = ['-target', 'x86_64-apple-macos' + target, '-isysroot', str(Path(sdk).resolve())]
    with tempfile.TemporaryDirectory(prefix='mellow-metal-dependency-verify-') as temporary:
        source = Path(temporary) / 'inventory.m'
        source.write_text('#import <Metal/Metal.h>\n')
        actual = dependency_manifest(clang, source, arguments, sdk)
    if actual != expected:
        raise ValueError('Current SDK/compiler dependency membership or bytes differ')


def verify_headers(expected, root):
    if expected != header_manifest(root):
        raise ValueError('SDK header membership or bytes changed')


def point(location):
    if 'expansionLoc' not in location:
        return location
    expanded = dict(location['expansionLoc'])
    # Clang elides the file on a macro argument's expansion location, even
    # across filtered top-level JSON roots. Its spelling names the same file.
    if expanded.get('isMacroArgExpansion') and 'file' not in expanded:
        if 'file' in location.get('spellingLoc', {}):
            expanded['file'] = location['spellingLoc']['file']
    return expanded


def call_at(data, offset):
    """Read one balanced macro/attribute invocation; strings never become syntax."""
    match = re.match(rb'[A-Za-z_][A-Za-z_0-9]*\s*\(', data[offset:])
    if not match:
        return '', ''
    name = data[offset:offset + match.end()].split(b'(')[0].strip().decode()
    pos, depth, quote, escaped = offset + match.end(), 1, None, False
    start = pos
    while pos < len(data) and pos - offset <= 16384:
        char = data[pos]
        if quote:
            if escaped: escaped = False
            elif char == 92: escaped = True
            elif char == quote: quote = None
        elif char in (34, 39): quote = char
        elif char == 40: depth += 1
        elif char == 41:
            depth -= 1
            if depth == 0:
                body = data[start:pos].decode('utf-8')
                # Deprecation messages can contain text such as "macos(99.0)".
                body = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', body)
                return name, body
        pos += 1
    return name, ''


def own_availability(node, file, root, cache):
    macos, unknown, seen = {}, False, set()
    for attr in node.get('inner', []):
        if attr.get('kind') not in ('AvailabilityAttr', 'UnavailableAttr', 'DeprecatedAttr'):
            continue
        loc = point(attr.get('range', {}).get('begin', {}))
        candidate = Path(loc.get('file', file)).resolve()
        offset = loc.get('offset')
        if not candidate.is_relative_to(root) or not isinstance(offset, int):
            unknown = True
            continue
        key = (candidate, offset)
        if key in seen:
            continue
        seen.add(key)
        data = cache.setdefault(candidate, candidate.read_bytes())
        name, body = call_at(data, offset)
        if name in ('API_AVAILABLE', 'API_DEPRECATED', 'API_DEPRECATED_WITH_REPLACEMENT'):
            match = re.search(r'\bmacos\s*\(\s*(\d+(?:\.\d+){0,2})(?:\s*,\s*(\d+(?:\.\d+){0,2}))?\s*\)', body)
            if match:
                macos['introduced'] = match.group(1)
                if match.group(2): macos['deprecated'] = match.group(2)
            elif not body or re.search(r'\bmacos\b', body):
                unknown = True
        elif name == 'API_UNAVAILABLE':
            if re.search(r'\bmacos\b', body): macos['unavailable'] = True
            elif not body: unknown = True
        elif name == 'availability':
            # Native Clang attribute in real fixtures, not inferred from API names.
            platform = body.split(',', 1)[0].strip()
            if platform == 'macos':
                for field in ('introduced', 'deprecated', 'obsoleted'):
                    match = re.search(r'\b' + field + r'\s*=\s*(\d+(?:\.\d+){0,2})', body)
                    if match: macos[field] = match.group(1)
                if re.search(r'\bunavailable\b', body): macos['unavailable'] = True
        else:
            unknown = True
    return {'macos': macos, 'unresolved': unknown}


def merge_availability(parent, own):
    merged = dict(parent.get('macos', {}))
    for field, value in own['macos'].items():
        if field == 'introduced' and field in merged:
            merged[field] = max((merged[field], value), key=version)
        elif field == 'obsoleted' and field in merged:
            merged[field] = min((merged[field], value), key=version)
        else:
            merged[field] = value
    return {'macos': merged, 'unresolved': parent.get('unresolved', False) or own['unresolved']}


def classification(name, owner, availability, target):
    os = availability['macos']
    if 'mtl4' in name.lower() or 'mtl4' in owner.lower():
        return 'metal4_namespace_excluded'
    if os.get('unavailable') or ('obsoleted' in os and version(os['obsoleted']) <= version(target)):
        return 'unavailable_on_macos'
    if availability['unresolved']:
        return 'availability_unresolved'
    if 'introduced' in os and version(os['introduced']) > version(target):
        return 'newer_than_target'
    return 'target_api_candidate' if 'introduced' in os else 'unspecified_macos_availability_candidate'


def extract_declarations(roots, header_root, target):
    root, records, cache, seen_ids = Path(header_root).resolve(), {}, {}, set()
    declaration_ids = {}
    version(target)
    last_file = None
    roots = [child for node in roots for child in (node.get('inner', []) if node.get('kind') == 'TranslationUnitDecl' else [node])]

    def visit(node, file, owner='', inherited=None):
        inherited = inherited or {'macos': {}, 'unresolved': False}
        loc = point(node.get('loc', {}))
        file = Path(loc.get('file', file)).resolve() if loc.get('file', file) else None
        if not file or not file.is_relative_to(root):
            return
        kind, name = node.get('kind'), node.get('name', '')
        if kind == 'ObjCCategoryDecl':
            interface = node.get('interface', {}).get('name', '')
            name = interface + '(' + name + ')'
        if node.get('isImplicit'):
            return
        if kind == 'EnumDecl' and not any(p.get('kind') == 'EnumConstantDecl' for p in node.get('inner', [])):
            return
        if kind == 'RecordDecl' and not node.get('completeDefinition'):
            return
        data = cache.setdefault(file, file.read_bytes())
        begin = point(node.get('range', {}).get('begin', {})).get('offset', loc.get('offset'))
        endloc = point(node.get('range', {}).get('end', {}))
        end = endloc.get('offset', begin)
        end = end + endloc.get('tokLen', 0) if isinstance(end, int) else None
        # Protocol/class forward declarations are references, not API definitions.
        if kind in ('ObjCProtocolDecl', 'ObjCInterfaceDecl', 'ObjCCategoryDecl'):
            if not isinstance(begin, int) or end is None or b'@end' not in data[begin:end]:
                return
        availability = merge_availability(inherited, own_availability(node, file, root, cache))
        # Include every named declaration physically defined by Metal headers,
        # including NSProcessInfo categories and anonymous class extensions.
        in_scope = bool(name)
        if kind in KINDS and in_scope:
            offset = loc.get('offset')
            if not isinstance(offset, int) or not 0 <= offset < len(data):
                raise ValueError('Invalid Clang source span: ' + str((kind, name, str(file), offset)))
            discriminator = ('-' if node.get('instance') else '+') if kind == 'ObjCMethodDecl' else ''
            identity = kind + ':' + owner + ':' + discriminator + name
            item = {'id': identity, 'kind': kind, 'name': name, 'owner': owner,
                    'source': {'path': file.relative_to(root).as_posix(), 'line': data[:offset].count(b'\n') + 1,
                               'offset': offset, 'span_begin': begin, 'span_end': end},
                    'availability': availability,
                    'target_classification': classification(name, owner, availability, target),
                    'requirement': 'unreviewed', 'implementation_status': 'unreviewed',
                    'hardware_feature_condition': 'unreviewed', 'hardware_verified': False}
            if 'type' in node: item['type'] = node['type'].get('qualType', '')
            if kind == 'ObjCPropertyDecl':
                item['property_attributes'] = {key: node[key] for key in ('readonly', 'readwrite', 'atomic', 'nonatomic', 'copy', 'strong', 'weak', 'assign', 'class') if key in node}
            if kind == 'ObjCMethodDecl':
                item.update(instance=bool(node.get('instance')), return_type=node.get('returnType', {}).get('qualType', ''),
                            parameter_types=[p.get('type', {}).get('qualType', '') for p in node.get('inner', []) if p.get('kind') == 'ParmVarDecl'])
            if kind in OWNERS:
                item['inherited_protocols'] = [p['name'] for p in node.get('protocols', [])]
                if node.get('super', {}).get('name'): item['superclass'] = node['super']['name']
            if identity in records and records[identity] != item:
                old = records[identity]
                if (kind != 'FunctionDecl' or declaration_ids.get(node.get('previousDecl')) != identity or
                    old.get('type') != item.get('type')):
                    raise ValueError('Ambiguous duplicate API identity: ' + identity)
                old.setdefault('redeclaration_sources', []).append(item['source'])
                old['availability'] = merge_availability(old['availability'], availability)
                old['target_classification'] = classification(name, owner, old['availability'], target)
            else:
                records[identity] = item
            declaration_ids[node.get('id')] = identity
        if kind in OWNERS and name:
            owner = name
        for child in node.get('inner', []):
            if child.get('kind') in KINDS:
                visit(child, file, owner, availability)

    for node in roots:
        loc = point(node.get('loc', {}))
        if loc.get('file'): last_file = loc['file']
        if node.get('id') in seen_ids:
            continue
        seen_ids.add(node.get('id'))
        visit(node, last_file)
    if not records:
        raise ValueError('No Metal API declarations extracted')
    return [records[key] for key in sorted(records)]


def inventory_report(records, target):
    result = {'schema': 'mellowgpu.metal-api-inventory/1', 'target_os': 'macOS', 'target_version': target,
              'generator': {'path': 'Tools/metal-api-inventory.py', 'sha256': sha(Path(__file__))},
              'programming_model': 'Metal3', 'required_inventory_complete': False,
              'implementation_coverage_complete': False, 'hardware_qualified': False,
              'verified_implementation_count': 0,
              'classification_counts': dict(sorted(collections.Counter(r['target_classification'] for r in records).items())),
              'declarations': records,
              'scope': 'SDK declarations only; candidates require semantic and hardware-feature review. No MSL, MetalFX, QuartzCore or private WindowServer ABI completeness claim.'}
    validate_inventory(result)
    return result


def validate_inventory(report):
    if (report.get('schema') != 'mellowgpu.metal-api-inventory/1' or
        any(report.get(key) is not False for key in ('required_inventory_complete', 'implementation_coverage_complete', 'hardware_qualified')) or
        type(report.get('verified_implementation_count')) is not int or
        report.get('verified_implementation_count') != 0):
        raise ValueError('Declaration inventory cannot promote conformance or hardware coverage')
    version(report.get('target_version'))
    seen = set()
    for item in report.get('declarations', []):
        identity = item.get('id')
        if not isinstance(identity, str) or identity in seen:
            raise ValueError('Missing/duplicate declaration identity')
        seen.add(identity)
        if (item.get('requirement') != 'unreviewed' or item.get('implementation_status') != 'unreviewed' or
            item.get('hardware_feature_condition') != 'unreviewed' or item.get('hardware_verified') is not False):
            raise ValueError('Declaration existence is not implementation evidence')
    if not seen:
        raise ValueError('Empty declaration inventory')
    counts = report.get('classification_counts')
    if (not isinstance(counts, dict) or any(type(value) is not int or value < 0 for value in counts.values()) or
        counts != dict(collections.Counter(item['target_classification'] for item in report['declarations']))):
        raise ValueError('Declaration classification counts differ from records')


def source_method_observations(roots, source_root, roles):
    """A concrete method body is observation evidence, including unsupported bodies."""
    root, observed, last_file = Path(source_root).resolve(), [], None
    for node in roots:
        loc = point(node.get('loc', {}))
        if loc.get('file'): last_file = loc['file']
        name = node.get('name')
        if node.get('kind') != 'ObjCImplementationDecl' or name not in roles or not last_file:
            continue
        file = Path(last_file).resolve()
        if not file.is_relative_to(root):
            raise ValueError('Implementation source escapes repository')
        data = file.read_bytes()
        for method in node.get('inner', []):
            if method.get('kind') != 'ObjCMethodDecl' or not any(n.get('kind') == 'CompoundStmt' for n in method.get('inner', [])):
                continue
            offset = point(method['loc'])['offset']
            observed.append({'class': name, 'sdk_role': roles[name], 'selector': method['name'],
                             'instance': bool(method.get('instance')),
                             'source': {'path': file.relative_to(root).as_posix(),
                                        'line': data[:offset].count(b'\n') + 1, 'offset': offset},
                             'semantic_implementation_verified': False, 'hardware_verified': False})
    return sorted(observed, key=lambda item: (item['class'], item['selector'], item['instance']))


def observe_repository(repo, sdk, clang, inventory, facade='all'):
    verify_inventory(inventory, sdk, clang)
    if facade not in ('all', 'compute', 'render'):
        raise ValueError('Unknown source facade')
    repo = Path(repo).resolve()
    compiler_before = compiler_identity(clang, sdk, inventory['target_version'], 'objective-c++')
    units = {
        'Userspace/AppleMetal/MellowAppleMetal.mm': ('MMA', {
            'MMADevice': 'MTLDevice', 'MMABuffer': 'MTLBuffer', 'MMALibrary': 'MTLLibrary',
            'MMAFunction': 'MTLFunction', 'MMAPipeline': 'MTLComputePipelineState',
            'MMAQueue': 'MTLCommandQueue', 'MMACommand': 'MTLCommandBuffer', 'MMAEncoder': 'MTLComputeCommandEncoder'}),
        'Userspace/AppleMetal/MellowAppleRenderMetal.mm': ('MRA', {
            'MRADevice': 'MTLDevice', 'MRATexture': 'MTLTexture', 'MRALibrary': 'MTLLibrary',
            'MRAFunction': 'MTLFunction', 'MRAPipeline': 'MTLRenderPipelineState',
            'MRAQueue': 'MTLCommandQueue', 'MRACommand': 'MTLCommandBuffer', 'MRAEncoder': 'MTLRenderCommandEncoder'})}
    files = list(units) + ['Userspace/AppleMetal/MellowAppleMetal.h', 'Userspace/AppleMetal/MellowAppleRenderMetal.h',
                          'Runtime/MetalObjects.hpp', 'Runtime/MetalObjects.cpp',
                          'Runtime/RenderObjects.hpp', 'Runtime/RenderObjects.cpp']
    before = {name: sha(repo / name) for name in files}
    observed, commands = [], {}
    for source, (prefix, roles) in units.items():
        if facade != 'all' and facade != ('compute' if prefix == 'MMA' else 'render'):
            continue
        arguments = ['-std=c++17', '-fobjc-arc', '-target', 'x86_64-apple-macos' + inventory['target_version'], '-isysroot', str(Path(sdk).resolve())]
        commands[source] = {}
        ast = clang_ast(clang, repo / source, arguments, prefix, 'objective-c++', commands[source], sdk)
        observed.extend(source_method_observations(ast, repo, roles))
    if before != {name: sha(repo / name) for name in files}:
        raise ValueError('Runtime source changed during observation')
    verify_compiler_identity(compiler_before, clang, sdk, inventory['target_version'], 'objective-c++')
    if any(value['ast']['frontend_sha256'] != compiler_before['frontend']['sha256'] for value in commands.values()):
        raise ValueError('Source request frontend differs from compiler identity')
    verify_headers(inventory['sdk']['headers'], Path(sdk).resolve() / inventory['sdk']['headers_root'])
    if sha(Path(sdk).resolve() / 'SDKSettings.plist') != inventory['sdk']['settings_sha256']:
        raise ValueError('SDK settings changed during source observation')
    verify_dependency_manifest(inventory['preprocessor_dependencies'], sdk, clang, inventory['target_version'])
    sdk_methods = {(r['owner'], r['name'], r.get('instance')) for r in inventory['declarations'] if r['kind'] == 'ObjCMethodDecl'}
    for item in observed:
        item['matches_direct_sdk_method_declaration'] = (item['sdk_role'], item['selector'], item['instance']) in sdk_methods
    return {'schema': 'mellowgpu.metal-source-observations/1', 'target_version': inventory['target_version'],
            'generator': {'path': 'Tools/metal-api-inventory.py', 'sha256': sha(Path(__file__))},
            'compiler': compiler_before,
            'compiler_commands': commands,
            'observed_facades': ['compute', 'render'] if facade == 'all' else [facade],
            'unobserved_facades': [] if facade == 'all' else ['render' if facade == 'compute' else 'compute'],
            'source_files': before, 'observed_method_body_count': len(observed), 'methods': observed,
            'verified_implementation_count': 0, 'required_inventory_complete': False,
            'hardware_qualified': False,
            'scope': 'Explicit historical facade role aliases; direct method bodies only. Unsupported/error-return bodies also count. No inherited-selector, signature, semantics, complete-API or GPU qualification inference.'}


def capture(sdk, clang, target):
    sdk = Path(sdk).resolve()
    compiler_before = compiler_identity(clang, sdk, target)
    headers = sdk / 'System/Library/Frameworks/Metal.framework/Headers'
    before = header_manifest(headers)
    settings = sdk / 'SDKSettings.plist'
    sdk_version = str(plistlib.loads(settings.read_bytes()).get('Version', ''))
    arguments = ['-target', 'x86_64-apple-macos' + target, '-isysroot', str(sdk)]
    commands = {}
    with tempfile.TemporaryDirectory(prefix='mellow-metal-api-') as temporary:
        source = Path(temporary) / 'inventory.m'
        source.write_text('#import <Metal/Metal.h>\n')
        dependencies_before = dependency_manifest(clang, source, arguments, sdk, commands)
        ast = clang_ast(clang, source, arguments, name_filter=None, evidence=commands, sdk=sdk)
        if dependencies_before != dependency_manifest(clang, source, arguments, sdk, commands):
            raise ValueError('SDK/compiler dependencies changed during AST capture')
    records = extract_declarations(ast, headers, target)
    verify_headers(before, headers)
    verify_compiler_identity(compiler_before, clang, sdk, target)
    if any(value['frontend_sha256'] != compiler_before['frontend']['sha256'] for value in commands.values()):
        raise ValueError('Capture request frontend differs from compiler identity')
    result = inventory_report(records, target)
    result['sdk'] = {'name': sdk.name, 'version': sdk_version, 'settings_sha256': sha(settings),
                     'headers_root': 'System/Library/Frameworks/Metal.framework/Headers', 'headers': before,
                     'older_than_target': version(sdk_version) < version(target)}
    result['compiler'] = compiler_before
    result['compiler_commands'] = commands
    result['ast_arguments'] = ['-x', 'objective-c', '-fsyntax-only', '-fno-modules',
                              '-target', 'x86_64-apple-macos' + target, '-isysroot', '<SDK>',
                              '-Xclang', '-ast-dump=json']
    result['preprocessor_dependencies'] = dependencies_before
    return result


def verify_inventory(report, sdk, clang):
    """Re-extract the complete source-level census, not semantic conformance."""
    validate_inventory(report)
    if report.get('generator') != {'path': 'Tools/metal-api-inventory.py', 'sha256': sha(Path(__file__))}:
        raise ValueError('Inventory generator source drift; recapture with this tool')
    current = capture(sdk, clang, report['target_version'])
    # Canonical JSON comparison also distinguishes bool/float from integer.
    if json.dumps(report, sort_keys=True, allow_nan=False) != json.dumps(current, sort_keys=True, allow_nan=False):
        raise ValueError('Inventory declaration/compiler/SDK/dependency census differs from fresh capture')
    return current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    c = commands.add_parser('capture')
    c.add_argument('--sdk', required=True, type=Path)
    c.add_argument('--clang', default='clang')
    c.add_argument('--target-macos', default='15.8.1')
    c.add_argument('--out', type=Path, required=True)
    v = commands.add_parser('verify')
    v.add_argument('--sdk', required=True, type=Path)
    v.add_argument('--inventory', required=True, type=Path)
    v.add_argument('--clang', default='clang')
    o = commands.add_parser('observe-source')
    o.add_argument('--sdk', required=True, type=Path)
    o.add_argument('--clang', default='clang')
    o.add_argument('--repo', required=True, type=Path)
    o.add_argument('--inventory', required=True, type=Path)
    o.add_argument('--out', required=True, type=Path)
    o.add_argument('--facade', choices=('all', 'compute', 'render'), default='all')
    args = parser.parse_args()
    if args.action == 'capture':
        import shutil
        clang = shutil.which(args.clang)
        if not clang: raise ValueError('Clang not found')
        result = capture(args.sdk, clang, args.target_macos)
        with args.out.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2, sort_keys=True); stream.write('\n')
        print(json.dumps({'declarations': len(result['declarations']), 'classifications': result['classification_counts'],
                          'required_inventory_complete': False, 'hardware_qualified': False}))
    elif args.action == 'verify':
        import shutil
        report = read_inventory(args.inventory); validate_inventory(report)
        clang = shutil.which(args.clang)
        if not clang: raise ValueError('Compiler not found')
        verify_inventory(report, args.sdk, clang)
        print(json.dumps({'verified_header_bytes': True, 'verified_declaration_census': True, 'hardware_qualified': False}))
    else:
        import shutil
        clang = shutil.which(args.clang)
        if not clang: raise ValueError('Clang not found')
        report = read_inventory(args.inventory)
        result = observe_repository(args.repo, args.sdk, clang, report, args.facade)
        result['inventory_sha256'] = sha(args.inventory)
        result['sdk_settings_sha256'] = sha(args.sdk.resolve() / 'SDKSettings.plist')
        with args.out.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2, sort_keys=True); stream.write('\n')
        print(json.dumps({'observed_method_bodies': result['observed_method_body_count'], 'verified_implementation_count': 0}))


if __name__ == '__main__':
    main()

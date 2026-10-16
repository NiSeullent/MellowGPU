"""Immutable source/spec intake and candidate reports; never driver promotion.

SPDX-License-Identifier: MIT
Original MellowGPU tooling. Existing repository/component licenses are unchanged.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from .core import PortError, canonical, digest, inventory, license_facts, no_links, unique_json_object, reject_json_constant

HASH = re.compile(r'[0-9a-f]{64}\Z')
OBJECT = re.compile(r'(?:[0-9a-f]{40}|[0-9a-f]{64})\Z')
MAX_BYTES = 16 * 1024 * 1024
REFERENCE_REMOTES = {'https://github.com/nullmoth/nvidia-macos-driver', 'https://github.com/almosst-dev/cobalt64'}
REFERENCE_IDS = {'nullmoth-reference', 'cobalt64-reference'}
# Exact known restricted-reference identities from the curated initial lock.
# This detects known aliases/copies, not arbitrary fork ancestry or new content.
REFERENCE_COMMITS = {'d96eaca3f47e748ce5b1f5fa3913a85e48f1f5fb', 'd1f7bad8408a0a2c016b067c477422975a59de44'}
REFERENCE_BLOBS = {'f2575a258d86240add7334ca0d5c6fc961ea9a60', '0f80732aaa0f51a3928835503aaa2168b46de3f9',
                   '38591a90d0b801a959a13c71fdfd554bab50533c', '028dcd775f0e013edc18fcb99d4a64ed7557a95d',
                   '6b835383a605b345187e69e1bd25a29dd59d7cdb'}
REFERENCE_DIGESTS = {'1b5c7a92697863d38fd2779af76732acfb8f521b758781eab057ad966701b0b7',
                     'b999a335eabb62c8b5394660aa8e3cc16bebb8483aace53abf9a16991addff42',
                     'ec3822b70df32a509380bcf9a5884384f54a8a622780fca4c5340e31bbe77d79',
                     'b47ba580fe38885f36367e61634b2e694ceddb5fb64060e13329eb338397d576',
                     'be272ec1dc42d0bb5f7bf285884a8dadf5f9b0748787ae9f89b9e3ec7d1deb0d'}
SOURCE_SUFFIXES = {'.c', '.h', '.cc', '.cpp', '.cxx', '.hpp', '.hxx', '.inc', '.s', '.asm',
                   '.m', '.mm', '.metal', '.sh', '.py', '.rs', '.go', '.mk', '.cmake'}
PERMISSIVE = {'MIT', 'BSD-2-Clause', 'BSD-3-Clause', 'ISC', 'Zlib', 'Apache-2.0', '0BSD',
              'GPL-2.0 OR MIT', 'GPL-2.0-only OR MIT', '(GPL-2.0-only OR MIT)',
              'GPL-2.0-or-later OR MIT', '(GPL-2.0 OR MIT)'}
LINUX = PERMISSIVE | {'GPL-2.0', 'GPL-2.0-only', 'GPL-2.0-or-later', 'LGPL-2.1-only',
                       'LGPL-2.1-or-later', 'LGPL-3.0-only', 'LGPL-3.0-or-later',
                       'GPL-2.0 WITH Linux-syscall-note', 'GPL-2.0-only WITH Linux-syscall-note'}


def require(ok, message):
    if not ok:
        raise PortError(message)


def reject_reference_bytes(data):
    """Reject only exact curated reference bytes; this is no ancestry detector."""
    framed = b'blob ' + str(len(data)).encode() + b'\0' + data
    require(digest(data) not in REFERENCE_DIGESTS and
            hashlib.sha1(framed).hexdigest() not in REFERENCE_BLOBS and
            hashlib.sha256(framed).hexdigest() not in REFERENCE_BLOBS,
            'Known reference-only content cannot become candidate source or project input')


def keys(obj, required, optional=()):
    require(isinstance(obj, dict) and set(required) <= set(obj) <= set(required) | set(optional), 'Unexpected/missing schema fields')


def path(value):
    require(isinstance(value, str) and bool(re.fullmatch(r'[A-Za-z0-9_./+-]+', value)) and
            not value.startswith('/') and all(p not in ('', '.', '..') for p in value.split('/')), 'Noncanonical source path')
    return value


def text(value, label):
    require(isinstance(value, str) and 0 < len(value) <= 512 and '\x00' not in value and '\n' not in value, 'Invalid ' + label)


def sha(value):
    require(isinstance(value, str) and bool(HASH.fullmatch(value)), 'Invalid SHA256')


def object_id(value):
    require(isinstance(value, str) and bool(OBJECT.fullmatch(value)), 'Full immutable Git object required')


def https(value):
    text(value, 'HTTPS URI')
    try:
        p = urlsplit(value)
        port = p.port
    except ValueError as e:
        raise PortError('Invalid HTTPS URI') from e
    require(p.scheme == 'https' and p.hostname and not p.username and not p.password and not p.fragment and
            not p.query and port in (None, 443), 'Canonical HTTPS URI required')
    return p


def remote(value):
    p = https(value)
    require(p.port is None and not p.hostname.endswith('.') and not value.endswith('/') and not value.endswith('.git') and
            bool(re.fullmatch(r'/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)+', p.path)) and
            all(x not in ('.', '..') for x in p.path.split('/')[1:]), 'Canonical remote required (without .git)')
    return value


def remote_identity(value):
    parsed = urlsplit(value)
    host = parsed.hostname.lower()
    if host == 'www.github.com': host = 'github.com'
    return 'https://' + host + parsed.path.lower()


def source_path(name):
    item = PurePosixPath(name)
    return item.suffix.lower() in SOURCE_SUFFIXES or item.name.lower() in ('makefile', 'kconfig', 'cmakelists.txt')


def source_uris(c, filename):
    base, revision = c['remote'], c['commit']
    uris = {base + '/blob/' + revision + '/' + filename, base + '/-/blob/' + revision + '/' + filename,
            base + '/-/raw/' + revision + '/' + filename}
    if urlsplit(base).hostname == 'github.com':
        uris.add('https://raw.githubusercontent.com/' + urlsplit(base).path.lstrip('/') + '/' + revision + '/' + filename)
    return uris


def consumers(items):
    require(isinstance(items, list) and 0 < len(items) <= 128 and all(isinstance(x, str) for x in items) and len(set(items)) == len(items), 'Invalid consumers')
    for item in items:
        path(item)


def validate_lock(lock):
    keys(lock, {'schema_version', 'components', 'specifications'})
    require(type(lock['schema_version']) is int and lock['schema_version'] == 1, 'Unsupported upstream schema')
    require(isinstance(lock['components'], list) and 0 < len(lock['components']) <= 64, 'Invalid components')
    ids = set()
    for c in lock['components']:
        keys(c, {'id', 'role', 'remote', 'commit', 'tracking_ref', 'profile', 'consumers', 'files',
                 'patches', 'configuration', 'license_review', 'abi'})
        require(isinstance(c['id'], str) and bool(re.fullmatch(r'[a-z][a-z0-9-]{0,63}', c['id'])) and c['id'] not in ids, 'Invalid/duplicate component id')
        ids.add(c['id']); remote(c['remote']); object_id(c['commit']); consumers(c['consumers'])
        require(c['role'] in ('reference-only', 'build-dependency'), 'Invalid component role')
        reference = c['role'] == 'reference-only'
        require((remote_identity(c['remote']) not in REFERENCE_REMOTES and c['id'] not in REFERENCE_IDS and
                 c['commit'] not in REFERENCE_COMMITS) or reference,
                'NullMoth/Cobalt64 must remain reference-only')
        require(c['profile'] == 'reference-only' if reference else c['profile'] in ('darwin-permissive', 'linux-native'), 'Role/profile mismatch')
        require(not reference or all(x.startswith(('docs/', 'porting/')) for x in c['consumers']), 'reference-only consumer cannot be linked code')
        ref = c['tracking_ref']
        require(isinstance(ref, str) and bool(re.fullmatch(r'refs/(?:heads|tags)/[A-Za-z0-9_./+-]+', ref)) and
                all(x not in ('', '.', '..') for x in ref.split('/')) and '..' not in ref and not ref.endswith('/'), 'Invalid tracking ref')
        keys(c['license_review'], {'status', 'evidence_sha256'})
        review = c['license_review']
        require(review['status'] in ('unreviewed', 'reviewed'), 'Invalid license review')
        if review['status'] == 'reviewed': sha(review['evidence_sha256'])
        else: require(review['evidence_sha256'] is None, 'Unreviewed license cannot claim review evidence')
        require(isinstance(c['files'], list) and 0 < len(c['files']) <= 4096, 'Invalid selected files')
        selected = set()
        for f in c['files']:
            keys(f, {'path', 'kind', 'git_blob', 'sha256', 'size_bytes', 'spdx', 'source_uri'}, {'license_notice_sha256'})
            path(f['path']); require(f['path'] not in selected, 'Duplicate selected path'); selected.add(f['path'])
            require(f['kind'] in ('source', 'notice', 'specification'), 'Invalid selected file kind')
            require(not source_path(f['path']) or f['kind'] == 'source', 'Known source path must retain source kind/license checks')
            object_id(f['git_blob']); sha(f['sha256'])
            require(reference or (f['git_blob'] not in REFERENCE_BLOBS and f['sha256'] not in REFERENCE_DIGESTS),
                    'Known NullMoth/Cobalt64 content must remain reference-only')
            require(type(f['size_bytes']) is int and 0 <= f['size_bytes'] <= MAX_BYTES, 'Invalid file size')
            text(f['spdx'], 'SPDX'); https(f['source_uri'])
            if 'license_notice_sha256' in f:
                sha(f['license_notice_sha256'])
                require(f['kind'] == 'source' and f['spdx'] == 'MIT', 'Legacy notice requires explicit MIT source review')
            require(f['source_uri'] in source_uris(c, f['path']), 'Source URI must pin canonical remote/commit/path')
            if not reference:
                admitted = PERMISSIVE if c['profile'] == 'darwin-permissive' else LINUX
                require(f['spdx'] in admitted, 'Source license is outside selected profile; review required')
        for label in ('patches', 'configuration'):
            require(isinstance(c[label], list) and len(c[label]) <= 256, 'Invalid ' + label)
            seen = set()
            for item in c[label]:
                keys(item, {'path', 'sha256'}); path(item['path']); sha(item['sha256'])
                require(reference or item['sha256'] not in REFERENCE_DIGESTS,
                        'Known reference-only content cannot become a patch/configuration input')
                require(item['path'] not in seen, 'Duplicate ' + label + ' path'); seen.add(item['path'])
        a = c['abi']; keys(a, {'family', 'headers', 'checks'}); text(a['family'], 'ABI family')
        require(isinstance(a['headers'], list) and all(isinstance(x, str) for x in a['headers']) and len(set(a['headers'])) == len(a['headers']) and
                all(x in selected for x in a['headers']), 'ABI headers must be selected files')
        require(isinstance(a['checks'], list) and len(a['checks']) <= 128, 'Invalid ABI checks')
        for check in a['checks']:
            keys(check, {'header', 'symbol', 'document', 'marker', 'expected'})
            require(check['header'] in a['headers'] and check['document'] in selected, 'ABI check source missing')
            require(isinstance(check['symbol'], str) and bool(re.fullmatch(r'[A-Za-z_]\w*', check['symbol'])), 'Invalid ABI symbol')
            text(check['marker'], 'ABI document marker')
            require(type(check['expected']) is int and 0 <= check['expected'] <= 0xffffffffffffffff, 'Invalid ABI value')
    require(isinstance(lock['specifications'], list) and len(lock['specifications']) <= 128, 'Invalid specifications')
    specs = set()
    for s in lock['specifications']:
        keys(s, {'id', 'uri', 'sha256', 'revision', 'consumers'})
        text(s['id'], 'specification id'); require(s['id'] not in specs, 'Duplicate specification'); specs.add(s['id'])
        https(s['uri']); sha(s['sha256']); text(s['revision'], 'specification revision'); consumers(s['consumers'])
    return lock


def load_lock(filename):
    p = no_links(filename)
    require(p.is_file() and p.stat().st_size <= MAX_BYTES, 'Lock must be a bounded regular file')
    try:
        value = json.loads(p.read_bytes(), object_pairs_hook=unique_json_object, parse_constant=reject_json_constant)
    except (ValueError, UnicodeError) as e:
        raise PortError('Invalid upstream JSON: ' + str(e)) from e
    return validate_lock(value)


def open_directory(directory, create=False):
    """Hold every absolute path component; never follow an ancestor symlink."""
    directory = Path(os.path.abspath(directory))
    fd = os.open(directory.anchor, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for part in directory.parts[1:]:
            if create:
                try: os.mkdir(part, mode=0o700, dir_fd=fd)
                except FileExistsError: pass
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd); fd = child
        return fd
    except OSError as error:
        os.close(fd)
        raise PortError('Cannot hold non-symlink directory: ' + str(error)) from error


def read_regular(fd):
    before = os.fstat(fd)
    require(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_BYTES, 'Input must be bounded regular file')
    chunks = []; total = 0
    while True:
        block = os.read(fd, min(65536, MAX_BYTES + 1 - total))
        if not block: break
        chunks.append(block); total += len(block)
        require(total <= MAX_BYTES, 'Input exceeded file bound')
    after = os.fstat(fd)
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'Input changed during read')
    return b''.join(chunks)


def read_file(root, name):
    """Open each component relative to held directory FDs without following links."""
    path(name); root = no_links(root)
    fd = open_directory(root)
    try:
        parts = name.split('/')
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd); fd = child
        child = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        try:
            return read_regular(child)
        finally:
            os.close(child)
    except OSError as e:
        raise PortError('Cannot read selected regular file: ' + str(e)) from e
    finally:
        os.close(fd)


def git(args, root=None, limit=MAX_BYTES):
    env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0',
               GIT_NO_REPLACE_OBJECTS='1', GIT_NO_LAZY_FETCH='1', GIT_OPTIONAL_LOCKS='0')
    argv = ['git', '-c', 'core.hooksPath=' + os.devnull, '-c', 'core.fsmonitor=false',
            '-c', 'protocol.allow=never', '-c', 'protocol.https.allow=always']
    if root is not None: argv += ['-C', str(root)]
    try:
        p = subprocess.run(argv + list(args), env=env, capture_output=True, timeout=25)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise PortError('Bounded Git query failed: ' + str(e)) from e
    require(p.returncode == 0 and len(p.stdout) <= limit and len(p.stderr) <= 65536, 'Git query failed or exceeded bound: ' + p.stderr[:512].decode(errors='replace'))
    return p.stdout


def verified_git_object(root, oid, kind, cache):
    """Recompute the named object's full Git hash, including commit/tree bytes."""
    object_id(oid)
    key = kind, oid
    if key in cache: return cache[key]
    require(git(['cat-file', '-t', oid], root).strip() == kind.encode(), 'Git object type mismatch')
    size = git(['cat-file', '-s', oid], root).strip()
    require(size.isdigit() and int(size) <= MAX_BYTES, 'Git object exceeds intake bound')
    data = git(['cat-file', kind, oid], root)
    hasher = hashlib.sha1 if len(oid) == 40 else hashlib.sha256
    actual = hasher(kind.encode() + b' ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    require(len(data) == int(size) and actual == oid, 'Git object content hash mismatch: ' + kind)
    if kind != 'blob': cache[key] = data
    return data


def verified_git_blob(root, commit, name, cache=None):
    """Verify commit -> every selected nested tree -> regular blob membership."""
    path(name); cache = {} if cache is None else cache
    record = verified_git_object(root, commit, 'commit', cache)
    first = record.split(b'\n', 1)[0]
    match = re.fullmatch(rb'tree ([0-9a-f]{40}|[0-9a-f]{64})', first)
    require(match is not None and len(match[1]) == len(commit), 'Malformed commit root tree')
    oid = match[1].decode(); parts = name.split('/'); oid_bytes = len(commit) // 2
    for index, part in enumerate(parts):
        tree = verified_git_object(root, oid, 'tree', cache)
        entries = []; offset = 0
        while offset < len(tree):
            space = tree.find(b' ', offset); zero = tree.find(b'\0', space + 1)
            require(space > offset and zero > space + 1 and zero + 1 + oid_bytes <= len(tree), 'Malformed Git tree object')
            mode, filename = tree[offset:space], tree[space + 1:zero]
            child = tree[zero + 1:zero + 1 + oid_bytes].hex()
            if filename == part.encode(): entries.append((mode, child))
            offset = zero + 1 + oid_bytes
        require(len(entries) == 1, 'Missing/ambiguous Git tree membership')
        mode, oid = entries[0]
        if index + 1 != len(parts):
            require(mode == b'40000', 'Selected path ancestor is not a Git tree')
        else:
            require(mode in (b'100644', b'100755'), 'Selected Git blob mode rejected')
            return mode.decode(), oid, verified_git_object(root, oid, 'blob', cache)
    raise PortError('Missing selected Git blob')


def check_abi(c, contents):
    for check in c['abi']['checks']:
        try:
            header = contents[check['header']].decode('utf-8')
            document = contents[check['document']].decode('utf-8')
        except UnicodeError as e:
            raise PortError('ABI input must be UTF-8') from e
        constants = [x['unsigned_value'] for x in inventory(check['header'], header)['simple_integer_defines'] if x['name'] == check['symbol']]
        # Exact assertions only. A minimum-version requirement must not be
        # represented as an equality assertion; leave it unasserted until modeled.
        values = re.findall(re.escape(check['marker']) + r'\s*(0[xX][0-9a-fA-F]+|[0-9]+)(?![A-Za-z0-9_])', document)
        require(len(constants) == 1 and len(values) == 1 and constants[0] == int(values[0], 0 if values[0].lower().startswith('0x') else 10) == check['expected'], 'ABI header/document/lock mismatch')


def verify_source(lock, component_id, source_root, project_root=None):
    validate_lock(lock)
    selected = [c for c in lock['components'] if c['id'] == component_id]
    require(len(selected) == 1, 'Unknown component')
    c = selected[0]; root = no_links(source_root)
    require(root.is_dir(), 'Source root must exist')
    head = git(['rev-parse', '--verify', 'HEAD'], root).decode().strip()
    require(head == c['commit'], 'Source HEAD must equal locked commit')
    contents = {}; objects = {}
    commit_bytes = verified_git_object(root, head, 'commit', objects)
    if c['role'] != 'reference-only':
        headers = commit_bytes.split(b'\n\n', 1)[0].split(b'\n')
        parents = [line[7:].decode('ascii') for line in headers if line.startswith(b'parent ')]
        require(not (set(parents) & REFERENCE_COMMITS),
                'Known reference-only direct parent requires separate reference review')
    for f in c['files']:
        mode, oid, blob = verified_git_blob(root, c['commit'], f['path'], objects)
        require(oid == f['git_blob'], 'Git blob membership/mode mismatch')
        require(len(blob) == f['size_bytes'] and digest(blob) == f['sha256'], 'Git blob content digest mismatch')
        work = read_file(root, f['path'])
        require(work == blob, 'Selected worktree differs from locked Git blob')
        if f['kind'] == 'source':
            try: facts = license_facts(work.decode('utf-8'))
            except UnicodeError as e: raise PortError('Source must be UTF-8') from e
            if c['role'] != 'reference-only' and facts['single_expression'] != f['spdx']:
                # Older MIT files predate SPDX. Pin the actual leading license
                # comment, including full grant/attribution/warranty markers.
                # This records source evidence, never clears license closure.
                lead = re.match(rb'\s*(/\*.*?\*/)', work, re.S)
                comment = lead[1] if lead else b''
                words = re.sub(rb'\s+', b' ', comment.replace(b'*', b' ')).lower()
                require(facts['single_expression'] is None and not facts['spdx_expressions_observed'] and
                        not facts['malformed_spdx_markers_observed'] and
                        f['spdx'] == 'MIT' and f.get('license_notice_sha256') == digest(comment) and
                        all(x in words for x in (b'permission is hereby granted, free of charge',
                            b'to deal in the software without restriction', b'this permission notice',
                            b'all copies or substantial portions', b'without warranty of any kind')) and
                        b'gnu general public license' not in words and b'lesser general public license' not in words,
                        'Actual SPDX/legacy MIT notice differs from locked license')
        contents[f['path']] = work
    check_abi(c, contents)
    project = no_links(project_root or Path(__file__).resolve().parents[2])
    local = {}
    for label in ('patches', 'configuration'):
        for item in c[label]:
            data = read_file(project, item['path'])
            require(digest(data) == item['sha256'], 'Selected ' + label + ' digest mismatch')
            local[item['path']] = data
    for name, data in contents.items():
        require(read_file(root, name) == data, 'Selected worktree changed during verification')
    for name, data in local.items():
        require(read_file(project, name) == data, 'Selected local inputs changed during verification')
    require(git(['rev-parse', '--verify', 'HEAD'], root).decode().strip() == head, 'Source HEAD changed during verification')
    return {'schema_version': 1, 'component': component_id, 'commit': head,
            'role': c['role'], 'profile': c['profile'], 'revision_membership_verified': True,
            'files_verified': len(contents), 'abi_checks_verified': len(c['abi']['checks']),
            'build_eligible': False, 'hardware_qualified': False, 'production_promotion_available': False,
            'reference_ancestry_scope': 'Exact curated commit/direct-parent/content identities only; other ancestry UNKNOWN',
            'remaining_gates': ['Full selected-build dependency/license closure', 'Exact patch application and build',
                                'Target native imports/ABI', 'Complete Metal3/WindowServer/apps/games hardware conformance'],
            'scope': 'Selected Git blobs/worktree/local input and declared exact ABI checks only; no scripts executed.'}


def query_ref(remote_url, ref):
    output = git(['ls-remote', '--exit-code', remote_url, ref, ref + '^{}'], limit=65536).decode()
    values = {}
    for line in output.splitlines():
        parts = line.split('\t'); require(len(parts) == 2, 'Malformed remote ref response')
        oid, name = parts; object_id(oid)
        require(name in (ref, ref + '^{}') and name not in values, 'Unexpected/duplicate remote ref')
        values[name] = oid
    require(ref in values, 'Tracking ref absent')
    return {'object': values[ref], 'commit': values.get(ref + '^{}', values[ref])}


def query_spec(uri):
    https(uri)
    try:
        with urlopen(Request(uri, headers={'User-Agent': 'MellowGPU-upstream-intake/1'}), timeout=15) as response:
            final = https(response.geturl())
            require(final.hostname == urlsplit(uri).hostname, 'Specification redirect changed origin')
            data = response.read(MAX_BYTES + 1)
            require(len(data) <= MAX_BYTES, 'Specification exceeded intake bound')
            return digest(data)
    except (OSError, ValueError) as e:
        raise PortError('Specification metadata fetch failed: ' + str(e)) from e


def discover(lock, query=None, spec_query=None):
    validate_lock(lock); query = query or query_ref; spec_query = spec_query or query_spec
    candidates = []
    for c in lock['components']:
        try:
            found = query(c['remote'], c['tracking_ref'])
            keys(found, {'object', 'commit'}); object_id(found['object']); object_id(found['commit'])
            candidates.append({'id': c['id'], 'remote': c['remote'], 'tracking_ref': c['tracking_ref'],
                               'role': c['role'], 'object': found['object'], 'commit': found['commit'],
                               'locked_commit': c['commit'], 'changed': found['commit'] != c['commit'],
                               'membership_verified': False, 'source_verified': False})
        except (PortError, OSError) as e:
            candidates.append({'id': c['id'], 'role': c['role'], 'error': str(e), 'source_verified': False})
    specifications = []
    for spec in lock['specifications']:
        try:
            measured = spec_query(spec['uri']); sha(measured)
            specifications.append({'id': spec['id'], 'uri': spec['uri'], 'sha256': measured,
                                   'locked_sha256': spec['sha256'], 'changed': measured != spec['sha256'],
                                   'consumers': spec['consumers']})
        except (PortError, OSError) as e:
            specifications.append({'id': spec['id'], 'error': str(e)})
    return {'schema_version': 1, 'candidates': candidates, 'specification_candidates': specifications, 'lock_modified': False,
            'hardware_qualified': False, 'production_promotion_available': False,
            'scope': 'Remote metadata only; candidates require immutable source and full conformance review.'}


def report(before, after=None):
    validate_lock(before)
    if after is None: after = before
    validate_lock(after)
    existing = {c['id']: c for c in before['components']}
    references = [c for c in before['components'] if c['role'] == 'reference-only']
    for component in after['components']:
        if component['role'] == 'reference-only': continue
        # A mixed-role selected-source index is not itself a transition. Keep
        # unchanged pre-existing records distinct from actual role/copy changes.
        if existing.get(component['id']) == component: continue
        for reference in references:
            copied = (remote_identity(component['remote']) == remote_identity(reference['remote']) or
                      component['commit'] == reference['commit'] or
                      bool({f['git_blob'] for f in component['files']} & {f['git_blob'] for f in reference['files']}) or
                      bool({f['sha256'] for f in component['files']} & {f['sha256'] for f in reference['files']}))
            require(not copied, 'Reference-only role promotion under renamed identity is forbidden')
    old = {c['id']: c for c in before['components']}; new = {c['id']: c for c in after['components']}
    changes = []
    for key in sorted(old.keys() | new.keys()):
        a, b = old.get(key), new.get(key)
        require(not (a and a['role'] == 'reference-only' and b and b['role'] != 'reference-only'), 'Reference-only role promotion is forbidden')
        af = {f['path']: f for f in a['files']} if a else {}
        bf = {f['path']: f for f in b['files']} if b else {}
        paths = sorted(p for p in af.keys() | bf.keys() if af.get(p) != bf.get(p))
        headers = set((a or {}).get('abi', {}).get('headers', [])) | set((b or {}).get('abi', {}).get('headers', []))
        changes.append({'id': key, 'changed': a != b, 'role': (b or a)['role'], 'source_files_changed': paths,
                        'abi_headers_changed': sorted(p for p in headers if
                            tuple((af.get(p) or {}).get(k) for k in ('git_blob', 'sha256', 'size_bytes')) !=
                            tuple((bf.get(p) or {}).get(k) for k in ('git_blob', 'sha256', 'size_bytes'))), 'abi_contract_changed': (a or {}).get('abi') != (b or {}).get('abi'),
                        'patches_changed': (a or {}).get('patches') != (b or {}).get('patches'),
                        'configuration_changed': (a or {}).get('configuration') != (b or {}).get('configuration'),
                        'license_changed': (a or {}).get('license_review') != (b or {}).get('license_review') or
                            any((af.get(p) or {}).get('spdx') != (bf.get(p) or {}).get('spdx') or
                                (af.get(p) or {}).get('license_notice_sha256') != (bf.get(p) or {}).get('license_notice_sha256') or
                                ('notice' in ((af.get(p) or {}).get('kind'), (bf.get(p) or {}).get('kind')) and
                                    tuple((af.get(p) or {}).get(k) for k in ('kind', 'git_blob', 'sha256', 'size_bytes')) !=
                                    tuple((bf.get(p) or {}).get(k) for k in ('kind', 'git_blob', 'sha256', 'size_bytes')))
                                for p in af.keys() | bf.keys()),
                        'affected_consumers': sorted(set((a or {}).get('consumers', [])) | set((b or {}).get('consumers', [])))})
    ospec = {s['id']: s for s in before['specifications']}; nspec = {s['id']: s for s in after['specifications']}
    spec_changed = sorted(k for k in ospec.keys() | nspec.keys() if ospec.get(k) != nspec.get(k))
    return {'schema_version': 1, 'lock_sha256': digest(canonical(after).encode()), 'components': changes,
            'specifications_changed': spec_changed, 'qualification_invalidated': bool(spec_changed or any(x['changed'] for x in changes)),
            'build_eligible': False, 'hardware_qualified': False, 'production_promotion_available': False,
            'scope': 'Selected source/spec/config/ABI drift only; license closure and native conformance remain separate gates.'}

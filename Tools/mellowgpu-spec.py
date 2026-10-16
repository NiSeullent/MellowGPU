#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check initial v1 specification consistency. Never attests GPU support.

The stdlib schema engine implements ONLY the keyword subset used by the committed
v1 schemas. Unsupported keywords fail; this is not a general JSON Schema engine.
Source reads use the existing descriptor-relative no-follow intake reader.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from mellow_port import upstreams as upstream
from mellow_port.core import PortError, unique_json_object, reject_json_constant

DOCS = ['specification', 'source-analysis', 'architecture', 'platform-support',
        'metal3-conformance', 'security-and-recovery', 'upstream-update-policy',
        'validation-plan', 'roadmap']
ARTIFACTS = ['abi', 'capabilities', 'support-matrix', 'requirements']
REQUIREMENTS = {'MG-' + suffix for suffix in (
    'CORE-ABI-001', 'CORE-LIFETIME-001', 'CORE-SYNC-001', 'OS-XNU-001',
    'CPU-X64-001', 'CPU-ARM64-001', 'CPU-ARM64E-001', 'GPU-INTEL-001',
    'GPU-AMD-001', 'GPU-NVIDIA-001', 'METAL-001', 'SHADER-001', 'WS-001',
    'DISPLAY-001', 'SURFACE-001', 'RECOVERY-001', 'PERF-001', 'UPSTREAM-001',
    'PROVENANCE-001')}
KEYWORDS = {'$schema', '$id', 'title', 'description', 'type', 'properties',
            'required', 'additionalProperties', 'items', 'minItems', 'maxItems',
            'uniqueItems', 'const', 'enum', 'pattern', 'minimum', 'maximum',
            'minLength', 'maxLength'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def parse(data):
    return json.loads(data, object_pairs_hook=unique_json_object,
                      parse_constant=reject_json_constant)


def strict_equal(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(strict_equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(strict_equal(a, b) for a, b in zip(left, right))
    return left == right


def schema_shape(schema, location='$'):
    require(isinstance(schema, dict) and set(schema) <= KEYWORDS,
            location + ': unsupported schema keyword/type')
    if 'type' in schema:
        require(schema['type'] in ('object', 'array', 'string', 'integer', 'boolean', 'null'),
                location + ': unsupported schema type')
    for key in ('additionalProperties', 'uniqueItems'):
        if key in schema:
            require(type(schema[key]) is bool, location + ': unsupported non-boolean ' + key)
    for key in ('minItems', 'maxItems', 'minLength', 'maxLength', 'minimum', 'maximum'):
        if key in schema:
            require(type(schema[key]) is int, location + ': invalid integer keyword ' + key)
    if 'pattern' in schema:
        require(type(schema['pattern']) is str, location + ': pattern must be a string')
        try:
            re.compile(schema['pattern'])
        except re.error as error:
            raise ValueError(location + ': invalid schema pattern') from error
    if 'enum' in schema:
        require(type(schema['enum']) is list and bool(schema['enum']), location + ': invalid enum')
    if 'properties' in schema:
        require(type(schema['properties']) is dict, location + ': properties must be an object')
        for key, child in schema['properties'].items():
            schema_shape(child, location + '.properties.' + key)
    if 'required' in schema:
        required = schema['required']
        require(type(required) is list and all(type(key) is str for key in required) and
                len(set(required)) == len(required) and set(required) <= set(schema.get('properties', {})),
                location + ': invalid required keys')
    if 'items' in schema:
        schema_shape(schema['items'], location + '.items')


def schema_check(value, schema, location='$'):
    types = {'object': lambda x: type(x) is dict, 'array': lambda x: type(x) is list,
             'string': lambda x: type(x) is str, 'integer': lambda x: type(x) is int,
             'boolean': lambda x: type(x) is bool, 'null': lambda x: x is None}
    if 'type' in schema:
        require(schema['type'] in types and types[schema['type']](value), location + ': wrong type')
    if 'const' in schema:
        require(strict_equal(value, schema['const']), location + ': constant mismatch')
    if 'enum' in schema:
        require(any(strict_equal(value, item) for item in schema['enum']), location + ': invalid value')
    if isinstance(value, dict):
        props = schema.get('properties', {})
        require(set(schema.get('required', [])) <= set(value), location + ': missing keys')
        if schema.get('additionalProperties') is False:
            require(set(value) <= set(props), location + ': unknown keys')
        for key, item in value.items():
            if key in props:
                schema_check(item, props[key], location + '.' + key)
    if isinstance(value, list):
        require(len(value) >= schema.get('minItems', 0) and
                len(value) <= schema.get('maxItems', len(value)), location + ': array bound')
        if schema.get('uniqueItems'):
            require(len({json.dumps(x, sort_keys=True) for x in value}) == len(value), location + ': duplicates')
        for index, item in enumerate(value):
            if 'items' in schema:
                schema_check(item, schema['items'], location + '[' + str(index) + ']')
    if type(value) is str:
        require(schema.get('minLength', 0) <= len(value) <= schema.get('maxLength', len(value)), location + ': string bound')
        if 'pattern' in schema:
            require(re.search(schema['pattern'], value) is not None, location + ': invalid string')
    if type(value) is int:
        require(schema.get('minimum', value) <= value <= schema.get('maximum', value), location + ': number bound')


def dependency_index(root, read):
    data = read('porting/upstreams.lock.json')
    lock = upstream.validate_lock(parse(data))
    requirements = parse(read('spec/v1/requirements.json'))['requirements']
    annotations = []
    for component in lock['components']:
        # Invert the actual forward links: component IDs are opaque identifiers,
        # not a reliable vendor or impact classification (e.g. ANV=mesa-vulkan).
        impacted = {'MG-PROVENANCE-001', 'MG-UPSTREAM-001'}
        impacted.update(item['id'] for item in requirements
                        if component['id'] in item['source_refs'])
        annotations.append({'id': component['id'], 'requirements': sorted(impacted),
                            'adapter': 'Drivers/Core/BackendInterfaces.hpp',
                            'tests': ['tests/specification_tests.py'],
                            'source_closure': 'UNKNOWN', 'firmware': 'UNKNOWN',
                            'firmware_hashes': [], 'build_admitted': False,
                            'support_tuples': [], 'hardware_qualified': False})
    return {'schema_version': 1, 'kind': 'mellowgpu.dependencies-index',
            'generated_from': {'path': 'porting/upstreams.lock.json',
                               'sha256': hashlib.sha256(data).hexdigest()},
            'components': lock['components'], 'specifications': lock['specifications'],
            'tracking': annotations,
            'scope': 'Selected source observations only; no complete build or firmware closure admitted'}


def check(root):
    checked = {}

    def read(name):
        data = upstream.read_file(root, name)
        checked[name] = hashlib.sha256(data).hexdigest()
        return data

    def document(name):
        return read(name).decode('utf-8')

    artifacts = {}
    for name in ARTIFACTS:
        schema = parse(read('spec/v1/' + name + '.schema.json'))
        schema_shape(schema, name)
        require(schema.get('$schema') == 'https://json-schema.org/draft/2020-12/schema', 'schema revision missing')
        value = parse(read('spec/v1/' + name + '.json'))
        schema_check(value, schema, name)
        artifacts[name] = value
    dependencies = parse(read('dependencies.lock'))
    dependency_schema = parse(read('spec/v1/dependencies.schema.json'))
    schema_shape(dependency_schema, 'dependencies')
    schema_check(dependencies, dependency_schema, 'dependencies')
    require(strict_equal(dependencies, dependency_index(root, read)), 'dependencies.lock differs from exact canonical source/index')

    docs = {name: document('docs/' + name + '.md') for name in DOCS}
    for name in ('0001-core-and-platform-boundaries', '0002-metal-frontend-route', '0003-display-ownership'):
        document('docs/adr/' + name + '.md')
    sources = {item['id'] for item in dependencies['components'] + dependencies['specifications']}
    sources.add('mellowgpu-original-contract')
    ids = set()
    for requirement in artifacts['requirements']['requirements']:
        identity = requirement['id']
        require(identity not in ids, 'duplicate requirement ID')
        ids.add(identity)
        require(identity in docs['specification'], 'requirement absent from human specification: ' + identity)
        require(set(requirement['source_refs']) <= sources, 'unknown source reference for ' + identity)
        for name in requirement['implementation'] + requirement['tests']:
            read(name)
        require(requirement['hardware_result'] == 'NOT_RUN', 'v1 has no hardware qualification authority')
        require(requirement['status'] != 'hardware-qualified', 'hosted results cannot qualify hardware')
    require(ids == REQUIREMENTS, 'initial requirement set differs from frozen nineteen contracts')

    abi = artifacts['abi']
    require(abi['control'] == {'major': 1, 'minor': 0, 'request_bytes': 64, 'reply_bytes': 128,
                              'connect_type': 0x4d470100, 'mode': 'cpu-storage-only'}, 'wire contract mismatch')
    read('Drivers/Core/MellowGPUABI.h')
    # Real C11 preprocessing/layout, not a regex that can match stale comments,
    # disabled branches or an obsolete declaration hidden beside the real one.
    compiler = shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
    require(compiler is not None, 'C compiler required for actual wire/layout assertions')
    probe = '''#include "MellowGPUABI.h"
#undef _Static_assert
_Static_assert(MELLOWGPU_CONTROL_ABI_MAJOR == 1U, "major");
_Static_assert(MELLOWGPU_CONTROL_ABI_MINOR == 0U, "minor");
_Static_assert(MELLOWGPU_CONTROL_CONNECT_TYPE == 0x4d470100U, "connection");
_Static_assert(sizeof(MellowGPUControlRequest) == 64, "request");
_Static_assert(sizeof(MellowGPUControlReply) == 128, "reply");
_Static_assert(MellowGPUCapDeviceQuery == 1 && MellowGPUCapCPUStorage == 2 && MellowGPUCapCPUMap == 4, "caps");
'''
    command = [compiler, '-std=c11', '-ffreestanding', '-fsyntax-only', '-x', 'c',
               '-I' + str(root / 'Drivers/Core'), '-']
    environment = {key: value for key, value in os.environ.items()
                   if key not in ('CCC_OVERRIDE_OPTIONS', 'CPATH', 'C_INCLUDE_PATH',
                                  'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH')}
    compiled = subprocess.run(command, input=probe, capture_output=True, text=True,
                              timeout=30, env=environment)
    require(compiled.returncode == 0, 'native wire/layout compile failed: ' + compiled.stderr[:2048])
    profile = parse(read('porting/metal3-profile.json'))
    require(profile['qualified_devices'] == [] and profile['hardware_attestation_implemented'] is False and
            profile['api_inventory_complete'] is False, 'profile evidence gate differs from initial v1')
    require(profile['required_programming_model'] == 'Metal3' and strict_equal(profile['required_gates'], [
        'metal3_api_inventory', 'shader_semantics', 'native_resources', 'native_synchronization',
        'windowserver', 'native_scanout', 'applications', 'games', 'lifecycle_recovery', 'performance']),
        'initial v1 cannot weaken the full product qualification gates')
    require(strict_equal(profile['native_control'], {
        'specification': 'docs/MELLOWGPU-NATIVE-CONTROL.md', 'mode': 'cpu-storage-only',
        'gpu_virtual_memory_implemented': False, 'gpu_submission_implemented': False,
        'physical_runtime_verified': False}), 'profile native control differs from CPU-only v1 authority')
    for key in ('cpu_rasterization_qualifies', 'cpu_pixel_transfers_qualify',
                'policy_unit_tests_are_hardware_evidence'):
        require(profile[key] is False, 'profile cannot qualify hardware from ' + key)
    for inventory in profile['api_declaration_inventories']:
        require(hashlib.sha256(read(inventory['path'])).hexdigest() == inventory['sha256'], 'declaration inventory hash drift')

    matrix = artifacts['support-matrix']
    require(matrix['qualified_profiles'] == [], 'no qualification authority exists in v1')
    vendors = {'intel': '8086', 'amd': '1002', 'nvidia': '10de'}
    targets = matrix['development_targets']
    require(len({item['id'] for item in targets}) == len(targets), 'duplicate target ID')
    for target in targets:
        require(vendors.get(target['gpu_vendor']) == target['pci_vendor'], 'CPU/GPU vendor identity confusion')
        require(target['hardware_qualified'] is False, 'development tuple cannot be qualified')
        if target['id'] == '15igl7-glk-24h32':
            require(target['pci_device'] == '3185' and target['gpu_family'] == 'intel-glk' and
                    target['cpu_architecture'] == 'x86_64' and target['os_build'] == '24H32' and
                    target['pci_revision'] == '06' and target['pci_subsystem'] == '17aa:3e77' and
                    target['os_version'] == '15.8.1' and target['cpu_vendor'] == 'Intel' and
                    target['cpu_model'] == 'Celeron N4120' and target['route'] == 'i915/ANV' and
                    target['apple_gpu_family'] == 'UNKNOWN',
                    '15IGL7 must not be substituted with Xe, another tuple or a spoofed SMBIOS')
    require(any(item['id'] == '15igl7-glk-24h32' for item in targets), 'actual first target missing')
    receipt = parse(read('spec/evidence/15igl7-metal-runtime-metadata.receipt.json'))
    observed_bytes = read('spec/evidence/15igl7-metal-runtime-metadata.json')
    observation = parse(observed_bytes)
    require(receipt['schema'] == 'mellowgpu.metal-runtime-metadata-receipt/1' and
            receipt['observation']['path'] == 'spec/evidence/15igl7-metal-runtime-metadata.json' and
            hashlib.sha256(observed_bytes).hexdigest() == receipt['observation']['sha256'],
            'actual metadata observation/receipt digest mismatch')
    helper = receipt['helper']
    require(helper['source_path'] == 'Tools/metal-runtime-metadata.mm' and
            hashlib.sha256(read(helper['source_path'])).hexdigest() == helper['source_sha256'],
            'measured metadata helper source differs')
    require(helper['native_executed_architecture'] == 'x86_64' and
            helper['public_device_enumeration_called'] is False and
            receipt['private_initializers_invoked_directly'] is False and
            receipt['user_designated_test_device'] == '15IGL7', 'actual metadata helper execution scope drift')
    os_identity = {key: observation[key] for key in ('host_machine', 'kernel_release', 'os_build',
                                                   'os_product_version', 'process_architecture')}
    require(strict_equal(os_identity, receipt['operating_system_identity']) and
            os_identity['host_machine'] == 'x86_64' and os_identity['process_architecture'] == 'x86_64' and
            os_identity['os_build'] == '24H32' and os_identity['os_product_version'] == '15.8.1',
            'actual metadata receipt OS/architecture tuple mismatch')
    identity_digest = hashlib.sha256(json.dumps(os_identity, sort_keys=True,
                                                separators=(',', ':')).encode()).hexdigest()
    require(identity_digest == receipt['os_identity_fields_sha256'], 'OS field digest mismatch')
    require(receipt['recorded_pci_identity']['vendor'] == '8086' and
            receipt['recorded_pci_identity']['device'] == '3185' and
            receipt['recorded_pci_identity']['revision'] == '06' and
            receipt['recorded_pci_identity']['subsystem_vendor'] == '17AA' and
            receipt['recorded_pci_identity']['subsystem_device'] == '3E77', 'actual PCI observation tuple mismatch')
    for key in ('public_device_enumeration_called', 'private_initializers_invoked_directly',
                'private_driver_subclasses_instantiated_directly', 'command_buffers_created',
                'gpu_commands_submitted', 'darwin24_x86_64_target_abi_verified'):
        require(observation[key] is False, 'metadata-only evidence cannot promote ' + key)
    for key in ('complete_native_abi_verified', 'gpu_commands_submitted', 'metal3_verified',
                'windowserver_verified', 'performed_boot_or_display_change', 'performed_kernel_extension_load'):
        require(receipt[key] is False, 'metadata receipt scope drift: ' + key)
    require(receipt['os_binary_sha256'] is None, 'OS field digest is not an OS binary attestation')
    for file in ('Tools/mellowgpu-spec.py', 'Tools/mellow_port/core.py', 'Tools/mellow_port/upstreams.py'):
        read(file)
    return {'schema': 'mellowgpu.specification-check/1', 'passed': True,
            'requirements_checked': len(ids), 'source_sha256': checked,
            'schema_engine': 'Exact committed v1 keyword subset; not a general JSON Schema implementation',
            'wire_compile': {'argv': command, 'exit_code': compiled.returncode},
            'hardware_qualified': False, 'gpu_executed': False,
            'scope': 'Document/schema/source/contract consistency only'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['check', 'write-lock'])
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    try:
        if args.command == 'write-lock':
            target = root / 'dependencies.lock'
            require(not target.is_symlink(), 'dependency lock must not be a symlink')
            value = dependency_index(root, lambda name: upstream.read_file(root, name))
            target.write_text(json.dumps(value, indent=2) + '\n')
            print('Generated selected-source index; no dependency or hardware admission')
        else:
            print(json.dumps(check(root), indent=2))
        return 0
    except (ValueError, KeyError, TypeError, OSError, PortError, subprocess.SubprocessError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

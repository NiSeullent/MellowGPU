#!/usr/bin/env python3
"""Real Clang Objective-C AST fixtures; no GPU or runtime execution."""
import copy
import importlib.util
import json
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / 'Tools/metal-api-inventory.py'

HEADER = r'''
#define API_AVAILABLE(...) __attribute__((availability(macos,introduced=10.11)))
@interface NSObject @end
__attribute__((availability(macos,introduced=10.11)))
@protocol MTLBase
@property(readonly) unsigned long label;
- (void)commit;
@end
@protocol MTLDevice;
__attribute__((availability(macos,introduced=10.11)))
@protocol MTLDevice <MTLBase>
- (void)oldMethod __attribute__((availability(macos,introduced=10.12,deprecated=14.0)));
- (void)newMethod __attribute__((availability(macos,introduced=26.0)));
- (void)iosOnly __attribute__((availability(macos,unavailable)));
- (void)iosUnavailable __attribute__((availability(ios,unavailable)));
+ (void)factory;
- (void)useMTL4CommandQueue;
@property(readonly) id mtl4CommandQueue;
@end
__attribute__((availability(macos,introduced=26.0)))
@interface MTL4CommandQueue : NSObject
- (void)commit;
@end
typedef struct MTLSize { unsigned long width; } MTLSize;
enum MTLMode : int;
typedef enum MTLMode : int { MTLModeA = 1, MTLModeB = 2 } MTLMode;
extern int MTLCreateSystemDefaultDevice(void);
static inline int MTLHelper(int a);
static inline int MTLHelper(int a) { return a; }
'''


class MetalInventory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.is_file():
            raise AssertionError('Actual SDK AST inventory implementation missing')
        spec = importlib.util.spec_from_file_location('metal_api_inventory', TOOL)
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.clang = shutil.which('clang')
        if not cls.clang:
            raise unittest.SkipTest('Real Clang is required for AST fixtures')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='metal-api-ast-')
        self.root = pathlib.Path(self.temp.name).resolve()
        self.header = self.root / 'MTLFixture.h'
        self.header.write_text(HEADER)
        self.ast = self.m.clang_ast(self.clang, self.header, [])
        self.records = self.m.extract_declarations(self.ast, self.root, '15.0')

    def tearDown(self):
        self.temp.cleanup()

    def record(self, owner, name, kind='ObjCMethodDecl'):
        return next(r for r in self.records if r['owner'] == owner and r['name'] == name and r['kind'] == kind)

    def test_actual_ast_preserves_owner_selectors_types_and_no_forward_duplicates(self):
        self.assertEqual(self.record('MTLDevice', 'factory')['instance'], False)
        self.assertEqual(self.record('MTLBase', 'commit')['return_type'], 'void')
        self.assertEqual(len([r for r in self.records if r['kind'] == 'ObjCProtocolDecl' and r['name'] == 'MTLDevice']), 1)
        self.assertTrue(any(r['name'] == 'MTLCreateSystemDefaultDevice' and r['kind'] == 'FunctionDecl' for r in self.records))
        self.assertTrue(any(r['name'] == 'MTLModeA' for r in self.records))
        self.assertEqual(len([r for r in self.records if r['kind'] == 'EnumDecl' and r['name'] == 'MTLMode']), 1)
        self.assertEqual(len([r for r in self.records if r['kind'] == 'FunctionDecl' and r['name'] == 'MTLHelper']), 1)
        self.assertTrue(any(r['name'] == 'MTLSize' and r['kind'] == 'RecordDecl' for r in self.records))
        self.assertEqual(self.record('MTLBase', 'label', 'ObjCPropertyDecl')['type'], 'unsigned long')
        self.assertFalse(any(r.get('implicit') for r in self.records))

    def test_macos_15_and_metal4_are_separate_from_gpu_feature_requirements(self):
        self.assertEqual(self.record('MTLDevice', 'oldMethod')['target_classification'], 'target_api_candidate')
        self.assertEqual(self.record('MTLDevice', 'newMethod')['target_classification'], 'newer_than_target')
        self.assertEqual(self.record('MTLDevice', 'iosOnly')['target_classification'], 'unavailable_on_macos')
        self.assertEqual(self.record('MTLDevice', 'iosUnavailable')['target_classification'], 'target_api_candidate')
        self.assertEqual(self.record('MTL4CommandQueue', 'commit')['target_classification'], 'metal4_namespace_excluded')
        self.assertEqual(self.record('MTLDevice', 'useMTL4CommandQueue')['target_classification'], 'metal4_namespace_excluded')
        self.assertEqual(self.record('MTLDevice', 'mtl4CommandQueue', 'ObjCPropertyDecl')['target_classification'], 'metal4_namespace_excluded')
        self.assertEqual(self.record('MTLDevice', 'oldMethod')['availability']['macos']['deprecated'], '14.0')
        self.assertTrue(all(r['requirement'] == 'unreviewed' and r['implementation_status'] == 'unreviewed' for r in self.records))
        self.assertTrue(all(r['hardware_verified'] is False for r in self.records))

    def test_availability_macro_expansion_uses_actual_source_not_json_omitted_values(self):
        self.header.write_text(HEADER + '\nAPI_AVAILABLE(macos(10.11))\n@interface MTLExtra : NSObject @end\n')
        records = self.m.extract_declarations(self.m.clang_ast(self.clang, self.header, []), self.root, '15.0')
        extra = next(r for r in records if r['name'] == 'MTLExtra')
        # Fixture macro semantic version and spelled argument agree; source is pinned.
        self.assertEqual(extra['availability']['macos']['introduced'], '10.11')
        self.assertEqual(extra['target_classification'], 'target_api_candidate')

    def test_unknown_availability_never_becomes_required(self):
        self.header.write_text(HEADER + '\n#define OPAQUE __attribute__((availability(macos,introduced=26.0)))\nOPAQUE\n@interface MTLOpaque : NSObject @end\n')
        records = self.m.extract_declarations(self.m.clang_ast(self.clang, self.header, []), self.root, '15.0')
        opaque = next(r for r in records if r['name'] == 'MTLOpaque')
        self.assertEqual(opaque['target_classification'], 'availability_unresolved')
        self.assertEqual(opaque['requirement'], 'unreviewed')
        self.header.write_text(HEADER + '\nAPI_AVAILABLE(macos(OPAQUE_VERSION))\n@interface MTLVersionMacro : NSObject @end\n')
        records = self.m.extract_declarations(self.m.clang_ast(self.clang, self.header, []), self.root, '15.0')
        self.assertEqual(next(r for r in records if r['name']=='MTLVersionMacro')['target_classification'], 'availability_unresolved')

    def test_macro_argument_locations_do_not_inherit_an_unrelated_header(self):
        other = self.root / 'MTLOther.h'
        other.write_text('// padding\n' * 100 + '\n#define ENUM(T,N) enum N:T N; enum N:T\ntypedef ENUM(int, MTLIndirect) { MTLIndirectA=1 };\n')
        self.header.write_text(HEADER + '\n#include "MTLOther.h"\n')
        records = self.m.extract_declarations(self.m.clang_ast(self.clang, self.header, []), self.root, '15.0')
        item = next(r for r in records if r['kind'] == 'TypedefDecl' and r['name'] == 'MTLIndirect')
        self.assertEqual(item['source']['path'], 'MTLOther.h')

    def test_source_locations_and_header_drift_are_bound_to_bytes(self):
        manifest = self.m.header_manifest(self.root)
        self.assertEqual(manifest['MTLFixture.h']['sha256'], self.m.sha(self.header))
        self.m.verify_headers(manifest, self.root)
        self.header.write_text(HEADER + '\n// drift\n')
        with self.assertRaisesRegex(ValueError, 'header'):
            self.m.verify_headers(manifest, self.root)
        self.assertTrue(all(r['source']['path'] == 'MTLFixture.h' and r['source']['line'] > 0 for r in self.records))

    def test_manifest_rejects_extra_and_linked_headers(self):
        manifest = self.m.header_manifest(self.root)
        (self.root / 'Extra.h').write_text('// extra')
        with self.assertRaises(ValueError):
            self.m.verify_headers(manifest, self.root)
        (self.root / 'Extra.h').unlink()
        (self.root / 'Link.h').symlink_to(self.header)
        with self.assertRaises(ValueError):
            self.m.header_manifest(self.root)

    def test_failed_clang_never_emits_a_partial_inventory(self):
        self.header.write_text('@protocol MTLBroken this is invalid')
        with self.assertRaisesRegex(ValueError, 'Clang'):
            self.m.clang_ast(self.clang, self.header, [])

    def test_inventory_cannot_self_promote_to_complete_or_implemented(self):
        report = self.m.inventory_report(self.records, '15.0')
        self.assertFalse(report['required_inventory_complete'])
        self.assertFalse(report['hardware_qualified'])
        self.assertEqual(report['verified_implementation_count'], 0)
        self.m.validate_inventory(report)
        for mutate in (lambda r: r.update(required_inventory_complete=True),
                       lambda r: r.update(hardware_qualified=True),
                       lambda r: r['declarations'][0].update(implementation_status='implemented'),
                       lambda r: r['declarations'].append(copy.deepcopy(r['declarations'][0]))):
            changed = copy.deepcopy(report); mutate(changed)
            with self.assertRaises(ValueError): self.m.validate_inventory(changed)

    def test_json_stream_rejects_truncation_and_non_ast_objects(self):
        with self.assertRaises(ValueError): self.m.parse_ast_stream('{"kind":')
        with self.assertRaises(ValueError): self.m.parse_ast_stream('{}')
        with self.assertRaises(ValueError): self.m.parse_ast_stream('[]')

    def test_real_source_method_bodies_remain_unreviewed_even_when_names_match(self):
        self.header.write_text(HEADER + '\n@interface MMADevice : NSObject\n- (void)commit;\n@end\n@implementation MMADevice\n- (void)commit {}\n- (void)unsupported { /* never implementation proof */ }\n@end\n')
        ast = self.m.clang_ast(self.clang, self.header, [], name_filter='MMA')
        observed = self.m.source_method_observations(ast, self.root, {'MMADevice': 'MTLBase'})
        self.assertEqual({r['selector'] for r in observed}, {'commit', 'unsupported'})
        self.assertTrue(all(r['semantic_implementation_verified'] is False for r in observed))
        self.assertEqual(next(r for r in observed if r['selector']=='commit')['sdk_role'], 'MTLBase')
        self.assertTrue(all(r['source']['path']=='MTLFixture.h' for r in observed))

    def test_unfiltered_ast_preserves_categories_extensions_and_non_mtl_names(self):
        self.header.write_text(HEADER + '\n@interface MTLExtra : NSObject\n@property(readonly) int count;\n@end\n@interface MTLExtra()\n@property(readwrite) int count;\n@end\n@interface NSObject (DeviceCertification)\n- (void)certify;\n@end\nextern int NSDeviceCertification;\n')
        records = self.m.extract_declarations(self.m.clang_ast(self.clang, self.header, [], name_filter=None), self.root, '15.0')
        self.assertTrue(any(r['name']=='NSDeviceCertification' for r in records))
        self.assertEqual(next(r for r in records if r['owner']=='MTLExtra()' and r['name']=='count')['property_attributes']['readwrite'], True)
        self.assertTrue(any(r['owner']=='NSObject(DeviceCertification)' and r['name']=='certify' for r in records))

    def test_actual_preprocessor_dependencies_pin_macro_only_support_headers(self):
        support = self.root / 'Support.h'; support.write_text('#define HELPER_VERSION 15\n')
        self.header.write_text('#include "Support.h"\n' + HEADER)
        dependencies = self.m.dependency_manifest(self.clang, self.header, [], self.root)
        self.assertEqual(dependencies['sdk']['Support.h']['sha256'], self.m.sha(support))
        self.assertNotIn('MTLFixture.h', dependencies['sdk'])
        self.assertTrue(all(not pathlib.Path(name).is_absolute() for name in dependencies['sdk']))

    def test_explicit_compute_only_observation_cannot_hide_unobserved_render(self):
        names = ['Userspace/AppleMetal/MellowAppleMetal.mm', 'Userspace/AppleMetal/MellowAppleRenderMetal.mm',
                 'Userspace/AppleMetal/MellowAppleMetal.h', 'Userspace/AppleMetal/MellowAppleRenderMetal.h',
                 'Runtime/MetalObjects.hpp', 'Runtime/MetalObjects.cpp', 'Runtime/RenderObjects.hpp', 'Runtime/RenderObjects.cpp']
        for name in names:
            file = self.root / name; file.parent.mkdir(parents=True, exist_ok=True); file.write_text('// fixture\n')
        (self.root/names[0]).write_text('@interface NSObject @end\n@interface MMADevice:NSObject @end\n@implementation MMADevice\n- (void)commit {}\n@end\n')
        (self.root/names[1]).write_text('invalid render translation unit')
        result = self.m.observe_repository(self.root, self.root, self.clang, self.m.inventory_report(self.records,'15.0'), facade='compute')
        self.assertEqual(result['unobserved_facades'], ['render'])
        self.assertEqual(result['observed_method_body_count'], 1)
        self.assertEqual(result['verified_implementation_count'], 0)
        self.assertFalse(result['required_inventory_complete'])

    def test_new_optional_sdk_support_input_is_detected_without_old_byte_drift(self):
        headers = self.root/'System/Library/Frameworks/Metal.framework/Headers'; headers.mkdir(parents=True)
        (headers/'Metal.h').write_text('#if __has_include(<NewSupport.h>)\n#include <NewSupport.h>\n#endif\n@interface MTLTest @end\n')
        (self.root/'usr/include').mkdir(parents=True)
        (self.root/'SDKSettings.plist').write_bytes(plistlib.dumps({'Version':'15.5'}))
        report = self.m.capture(self.root, self.clang, '15.8.1')
        inventory_path = self.root/'inventory.json'; inventory_path.write_text(json.dumps(report))
        self.m.verify_dependency_manifest(report['preprocessor_dependencies'], self.root, self.clang, '15.8.1')
        old_headers = copy.deepcopy(report['sdk']['headers'])
        (self.root/'usr/include/NewSupport.h').write_text('#define NEW_SUPPORT_PRESENT 1\n')
        self.m.verify_headers(old_headers, headers)  # All formerly recorded Metal bytes are unchanged.
        with self.assertRaisesRegex(ValueError, 'dependency'):
            self.m.verify_dependency_manifest(report['preprocessor_dependencies'], self.root, self.clang, '15.8.1')
        process = subprocess.run([sys.executable, str(TOOL), 'verify', '--sdk', str(self.root), '--clang', self.clang, '--inventory', str(inventory_path)], capture_output=True, text=True)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn('dependency', process.stderr)

    def compiler_wrapper(self, transform=''):
        wrapper = self.root/'custom-clang'
        wrapper.write_text('#!' + sys.executable + '\nimport os,sys\na=sys.argv[1:]\n' + transform + '\nos.execv(' + repr(self.clang) + ',[' + repr(self.clang) + ',*a])\n')
        wrapper.chmod(0o755)
        return wrapper

    def test_actual_frontend_hash_and_effective_target_are_not_the_driver_shim(self):
        wrapper = self.compiler_wrapper()
        identity = self.m.compiler_identity(wrapper, self.root, '15.8.1')
        reported = subprocess.run([str(wrapper), '-print-prog-name=clang'], capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(identity['invocation']['sha256'], self.m.sha(wrapper))
        self.assertEqual(identity['frontend']['sha256'], self.m.sha(pathlib.Path(reported).resolve()))
        self.assertNotEqual(identity['invocation']['sha256'], identity['frontend']['sha256'])
        self.assertEqual(identity['requested_target'], 'x86_64-apple-macos15.8.1')
        self.assertRegex(identity['effective_target'], r'^x86_64-apple-macosx?15\.8\.1$')
        self.assertTrue(identity['frontend']['version'])
        self.m.verify_compiler_identity(identity, wrapper, self.root, '15.8.1')
        forged = copy.deepcopy(identity); forged['frontend']['sha256'] = '0'*64
        with self.assertRaisesRegex(ValueError, 'Compiler'):
            self.m.verify_compiler_identity(forged, wrapper, self.root, '15.8.1')

    def test_explicit_compiler_target_override_is_rejected_without_xcrun_fallback(self):
        wrapper = self.compiler_wrapper("if '-target' in a: a[a.index('-target')+1]='arm64-apple-macos15.8.1'")
        with self.assertRaisesRegex(ValueError, 'target'):
            self.m.compiler_identity(wrapper, self.root, '15.8.1')

    def test_unresolved_custom_frontend_report_is_rejected_without_guessing(self):
        wrapper = self.compiler_wrapper("if a==['-print-prog-name=clang']:\n print('clang'); sys.exit(0)")
        with self.assertRaisesRegex(ValueError, 'frontend'):
            self.m.compiler_identity(wrapper, self.root, '15.8.1')

    def test_unchanged_apple_driver_cannot_hide_a_different_selected_frontend(self):
        if not pathlib.Path('/Library/Developer/CommandLineTools/usr/bin/clang').is_file():
            self.skipTest('Second actual Apple toolchain not installed')
        driver = '/usr/bin/clang'
        before = self.m.compiler_identity(driver, self.root, '15.8.1')
        with patch.dict(os.environ, {'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}):
            after = self.m.compiler_identity(driver, self.root, '15.8.1')
            if before['frontend']['sha256'] == after['frontend']['sha256']:
                self.skipTest('Default frontend already is the alternate toolchain')
            self.assertEqual(before['invocation']['sha256'], after['invocation']['sha256'])
            with self.assertRaisesRegex(ValueError, 'Compiler'):
                self.m.verify_compiler_identity(before, driver, self.root, '15.8.1')


if __name__ == '__main__':
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""Active product/EFI migration contract; no historical evidence or hardware mutation."""
import copy,importlib.util,plistlib,re,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,ROOT/path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class ProductMigrationTests(unittest.TestCase):
 def test_info_and_every_active_xcode_configuration(self):
  p=plistlib.loads((ROOT/'Mellow/Info.plist').read_bytes())
  self.assertEqual((p['CFBundleIdentifier'],p['CFBundleExecutable'],p['CFBundleName']),('com.NiSeullent.MellowGPU','MellowGPU','MellowGPU'))
  self.assertEqual(p['CFBundleVersion'],'0.5.1');self.assertEqual(p['CFBundleShortVersionString'],'0.5.1');self.assertEqual(p['MellowGPUDevelopmentLabel'],'0.5.1-dev')
  self.assertTrue(all(x['CFBundleIdentifier'] in ('com.NiSeullent.MellowGPU','$(PRODUCT_BUNDLE_IDENTIFIER)')for x in p['IOKitPersonalities'].values()))
  proj=(ROOT/'Mellow.xcodeproj/project.pbxproj').read_text()
  self.assertEqual(re.findall(r'PRODUCT_NAME = ([^;]+);',proj),['MellowGPU']*3)
  self.assertEqual(re.findall(r'PRODUCT_BUNDLE_IDENTIFIER = ([^;]+);',proj),['com.NiSeullent.MellowGPU']*3)
  self.assertEqual(re.findall(r'MODULE_NAME = ([^;]+);',proj),['com.NiSeullent.MellowGPU']*3)
  self.assertEqual(re.findall(r'MODULE_VERSION = ([^;]+);',proj),['0.5.1']*3)
  self.assertNotIn('Mellow.kext',proj)
  # Keep the internal target/source-folder stable for legacy include paths.
  self.assertIn('name = Mellow;',proj);self.assertIn('path = Mellow;',proj)
 def test_profiles_reject_legacy_and_mixed_loading(self):
  m=load('profiles','Tools/efi_boot_profiles.py');g=m.APPLE_BOOT_GUID
  c={'Kernel':{'Add':[{'BundlePath':'MellowGPU.kext','Enabled':True}]},'NVRAM':{'Add':{g:{'boot-args':'-v -mellowdiag'}}},'Booter':{'Quirks':{}}}
  self.assertFalse(m.build_profiles(c)['config-boot-baseline.plist']['Kernel']['Add'][0]['Enabled'])
  for entries in ([{'BundlePath':'Mellow.kext','Enabled':True}],c['Kernel']['Add']+[{'BundlePath':'Mellow.kext','Enabled':True}],c['Kernel']['Add']*2):
   with self.subTest(entries=entries),self.assertRaises(ValueError):m.build_profiles({**c,'Kernel':{'Add':entries}})
 def test_bundle_guard_catches_renamed_legacy_metadata(self):
  m=load('product_identity','Tools/product_identity.py')
  good=plistlib.loads((ROOT/'Mellow/Info.plist').read_bytes())
  m.validate_configured_product([('MellowGPU.kext',good)])
  for items in ([('renamed.kext',{'CFBundleIdentifier':m.LEGACY_BUNDLE_ID,'CFBundleExecutable':'Mellow'})],[('MellowGPU.kext',good),('old-renamed.kext',{'CFBundleIdentifier':m.LEGACY_BUNDLE_ID})],[('MellowGPU.kext',{**good,'CFBundleExecutable':'Mellow'})],[('renamed.kext',good)]):
   with self.subTest(items=items),self.assertRaises(ValueError):m.validate_configured_product(items)
 def test_full_product_metadata_and_public_qualification(self):
  m=load('product_identity','Tools/product_identity.py');p=plistlib.loads((ROOT/'Mellow/Info.plist').read_bytes())
  m.validate_product_info(p)
  for key,bad in [('CFBundleName','Mellow'),('CFBundleShortVersionString','0.4.4'),('MellowGPUDevelopmentLabel','0.5.1-qualified')]:
   with self.subTest(key=key),self.assertRaises(ValueError):m.validate_product_info({**p,key:bad})
  for personality in p['IOKitPersonalities'].values():
   self.assertEqual(personality['MellowGPUQualification'],'unqualified')
   self.assertEqual(personality['MellowGPUQualificationProfileVersion'],1)
  active=(ROOT/'Mellow/kern_mellow.cpp').read_text()
  self.assertIn('Qualification::evaluate(nativeReadiness)',active)
  self.assertIn('static_cast<uint64_t>(qualification.mayAdvertiseMetal)',active)
  self.assertNotIn('static_cast<uint64_t>(nativeReadiness.mayAdvertiseMetal)',active)
 def test_active_native_and_efi_artifacts_are_named_consistently(self):
  for f in ('Tools/cross-build.py','Tools/build-255u-efi.py','.github/workflows/build.yml','.github/workflows/sequoia-efi.yml'):
   s=(ROOT/f).read_text();self.assertNotIn('Mellow.kext',s,f);self.assertNotIn('Contents/MacOS/Mellow"',s,f)
  schemes=list((ROOT/'Mellow.xcodeproj/xcshareddata/xcschemes').glob('*.xcscheme'))
  self.assertTrue(schemes)
  for f in schemes:self.assertNotIn('Mellow.kext',f.read_text())
if __name__=='__main__':unittest.main(verbosity=2)

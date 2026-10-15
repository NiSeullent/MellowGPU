#!/usr/bin/env python3
"""Actual signed-binary packaging with a small Git fixture; native build boundary is mocked."""
import argparse,copy,hashlib,importlib.util,json,os,plistlib,shutil,subprocess,sys,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
args,other=argparse.ArgumentParser(add_help=False).parse_known_args()
BUNDLE=None
if '--bundle' in other:
 i=other.index('--bundle');BUNDLE=Path(other[i+1]).resolve();del other[i:i+2]
sys.argv=[sys.argv[0],*other]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class FeaturePresence(unittest.TestCase):
 def test_actual_development_entry_points_exist(self):
  self.assertTrue((ROOT/'Tools/package-mellowgpu-development.py').is_file(),'development packager missing')
  self.assertTrue((ROOT/'Tools/build-mellowgpu-development.py').is_file(),'clean native build wrapper missing')
class DevelopmentPackages(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not BUNDLE:raise unittest.SkipTest('Supply --bundle actual/MellowGPU.kext for real-artifact boundary tests')
  cls.p=load('dev_package',ROOT/'Tools/package-mellowgpu-development.py')
  cls.b=load('dev_build',ROOT/'Tools/build-mellowgpu-development.py')
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='mellowgpu-package-fixture-');self.base=Path(self.tmp.name);self.repo=self.base/'source';self.repo.mkdir()
  for name in ('.gitignore','Tools/product_identity.py','Tools/validate-macho.py','Tools/cross-build-native.sh','Tools/mellow_development.py','Tools/package-mellowgpu-development.py','Tools/build-mellowgpu-development.py','Mellow/Info.plist','Mellow.xcodeproj/project.pbxproj','docs/MELLOWGPU-SPEC.md','docs/MELLOWGPU-IMPLEMENTATION.md','docs/MELLOWGPU-LIFECYCLE.md','docs/MELLOWGPU-'+self.p.DEVELOPMENT_LABEL+'.md','docs/MELLOWGPU-NATIVE-CONTROL.md','docs/MELLOWGPU-METAL-API.md','docs/MELLOWGPU-CANDIDATES.md','porting/metal-api-review.json','porting/metal-api-sdk15.5-macos15.8.1.json','porting/metal-api-sdk26.5-macos15.8.1.json','porting/metal-api-source-observations.json','porting/metal3-profile.json','porting/upstreams.lock.json','LICENSE','NOTICE'):
   dest=self.repo/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,dest)
  shutil.copytree(ROOT/'LICENSES',self.repo/'LICENSES')
  self.git('init','-q');self.git('config','user.name','Development packaging test');self.git('config','user.email','fixture@example.invalid');self.git('add','.');self.git('commit','-qm','Actual source metadata fixture, not a native source build')
  self.native=self.base/'native';self.native.mkdir();self.bundle=self.native/'artifact/MellowGPU.kext';self.bundle.parent.mkdir();shutil.copytree(BUNDLE,self.bundle)
  subprocess.run(['/usr/bin/codesign','--force','--sign','-',str(self.bundle)],check=True,capture_output=True)
  self.binary=self.bundle/'Contents/MacOS/MellowGPU';self.report=self.native/'structural-report.json'
  subprocess.run([sys.executable,str(ROOT/'Tools/validate-macho.py'),str(self.bundle),'--output',str(self.report)],check=True,capture_output=True)
  snap=self.p.D.source_snapshot(self.repo)
  self.receipt={'schema':'mellowgpu.native-build/1','status':'built_signed_development_only','source_commit':snap['commit'],'source_tree':snap['tree'],'source_sha256':snap['files'],'bundle_path':str(self.bundle),'bundle_files':self.p.D.tree_manifest(self.bundle),'binary_sha256':self.p.D.sha(self.binary),'info_sha256':self.p.D.sha(self.bundle/'Contents/Info.plist'),'architecture':'x86_64','uuid':self.p.D.file_uuid(self.binary),'native_build':{'command':['bash','Tools/cross-build-native.sh','Release'],'exit_code':0},'toolchain':{'scope':'TEST native boundary; real signed binary, not a native build of this Git fixture'},'evidence_files':{'structural_report':{'path':'structural-report.json','sha256':self.p.D.sha(self.report)}},'hardware_tested':False,'runtime_loaded':False,'production_installable':False}
  self.receipt_path=self.native/'build-result.json';self.write_receipt()
 def tearDown(self):self.tmp.cleanup()
 def git(self,*a):return subprocess.check_output(['git',*a],cwd=self.repo,text=True).strip()
 def write_receipt(self):self.receipt_path.write_text(json.dumps(self.receipt))
 def package(self,out=None):return self.p.package(self.repo,self.bundle,self.report,self.receipt_path,out or self.base/'release')
 def test_real_artifact_package_checksums_and_honest_scope(self):
  r=self.package();root=self.base/'release/package';s=json.loads((root/'STATUS.json').read_text());self.assertFalse(s['production_installable']);self.assertEqual(s['qualified_devices'],[]);self.assertEqual(s['support_status'],'unqualified')
  self.assertEqual(json.loads((root/'SOURCE.json').read_text())['commit'],self.git('rev-parse','HEAD'))
  for line in (root/'SHA256SUMS').read_text().splitlines():
   h,p=line.split('  ',1);self.assertEqual(h,self.p.D.sha(root/p))
  self.assertEqual(r['archive_sha256'],self.p.D.sha(Path(r['archive'])))
  with zipfile.ZipFile(r['archive'])as z:self.assertIn('MellowGPU-'+self.p.DEVELOPMENT_LABEL+'/MellowGPU.kext/Contents/MacOS/MellowGPU',z.namelist())
  self.assertTrue((root/'Sources/MellowGPU-source.tar.gz').is_file())
 def test_dirty_git_and_source_receipt_mismatch_rejected_before_output(self):
  (self.repo/'NOTICE').write_text('changed')
  with self.assertRaisesRegex(ValueError,'clean|HEAD'):self.package()
  self.assertFalse((self.base/'release').exists())
  self.git('checkout','--','NOTICE');self.receipt['source_sha256'].pop('NOTICE');self.write_receipt()
  with self.assertRaisesRegex(ValueError,'source'):self.package()
 def test_package_carries_native_and_api_inventory_scope_with_exact_bytes(self):
  r=self.package();root=self.base/'release/package'
  for name in ('docs/MELLOWGPU-NATIVE-CONTROL.md','docs/MELLOWGPU-METAL-API.md','docs/MELLOWGPU-CANDIDATES.md','porting/metal-api-review.json','porting/metal-api-sdk15.5-macos15.8.1.json','porting/metal-api-sdk26.5-macos15.8.1.json','porting/metal-api-source-observations.json'):
   with self.subTest(document=name):
    self.assertTrue((root/name).is_file(), 'Missing native control / inventory scope: '+name)
    self.assertEqual((root/name).read_bytes(),(self.repo/name).read_bytes())
    with zipfile.ZipFile(r['archive']) as archive:
     self.assertEqual(archive.read('MellowGPU-'+self.p.DEVELOPMENT_LABEL+'/'+name),(self.repo/name).read_bytes())
 def test_old_identity_and_symlink_bundle_rejected(self):
  linked=self.base/'MellowGPU.kext';linked.symlink_to(self.bundle,target_is_directory=True)
  with self.assertRaises(ValueError):self.p.package(self.repo,linked,self.report,self.receipt_path,self.base/'linkout')
  info=self.bundle/'Contents/Info.plist';p=plistlib.loads(info.read_bytes());p['CFBundleIdentifier']='com.NiSeullent.Mellow';info.write_bytes(plistlib.dumps(p))
  with self.assertRaises(ValueError):self.package()
 def test_stale_structural_binary_and_mixed_build_tuple_rejected(self):
  p=json.loads(self.report.read_text());p['sha256']='0'*64;self.report.write_text(json.dumps(p));self.receipt['evidence_files']['structural_report']['sha256']=self.p.D.sha(self.report);self.write_receipt()
  with self.assertRaisesRegex(ValueError,'structural'):self.package()
  self.receipt['source_commit']='1'*40;self.write_receipt()
  with self.assertRaisesRegex(ValueError,'source'):self.package()
 def test_profile_cannot_promote_development_to_support(self):
  p=self.repo/'porting/metal3-profile.json';data=json.loads(p.read_text());data['api_inventory_complete']=True;p.write_text(json.dumps(data));self.git('add','.');self.git('commit','-qm','Test forbidden support promotion');snap=self.p.D.source_snapshot(self.repo);self.receipt.update(source_commit=snap['commit'],source_tree=snap['tree'],source_sha256=snap['files']);self.write_receipt()
  with self.assertRaisesRegex(ValueError,'profile'):self.package()
 def test_overwrite_and_bundle_nested_symlink_rejected(self):
  out=self.base/'existing';out.mkdir();sentinel=out/'keep';sentinel.write_text('untouched')
  with self.assertRaises(ValueError):self.package(out)
  self.assertEqual(sentinel.read_text(),'untouched')
  (self.bundle/'Contents/alias').symlink_to('Info.plist')
  with self.assertRaises(ValueError):self.package()
 def test_signature_failure_and_extra_bundle_member_rejected(self):
  with patch.object(self.p.D,'verify_signature',side_effect=ValueError('strict signature failure')):
   with self.assertRaisesRegex(ValueError,'signature'):self.package()
  (self.bundle/'Contents/extra').write_text('not in native tuple')
  with self.assertRaisesRegex(ValueError,'[Bb]undle'):self.package()
 def test_native_wrapper_dirty_or_existing_output_never_calls_toolchain(self):
  with patch.object(self.b,'native_build',side_effect=AssertionError('must not build')):
   (self.repo/'NOTICE').write_text('dirty')
   with self.assertRaises(ValueError):self.b.build(self.repo,self.base/'newbuild')
   self.git('checkout','--','NOTICE');out=self.base/'existingbuild';out.mkdir()
   with self.assertRaises(ValueError):self.b.build(self.repo,out)
 def test_native_wrapper_records_actual_signing_and_fresh_structural_report(self):
  def native(repo,out):
   bundle=out/'artifact/MellowGPU.kext';bundle.parent.mkdir();shutil.copytree(self.bundle,bundle)
   return bundle,{'command':['mock-native-boundary-using-actual-MachO'], 'exit_code':0}
  tools={'scope':'TEST mocked native compile boundary only','files':{'codesign':{'path':'/usr/bin/codesign','sha256':self.p.D.sha(Path('/usr/bin/codesign'))}}}
  with patch.object(self.b,'native_build',side_effect=native),patch.object(self.b,'toolchain',return_value=(tools,[])):
   result=self.b.build(self.repo,self.base/'freshbuild')
  self.assertEqual(result['source_commit'],self.git('rev-parse','HEAD'))
  self.assertEqual(result['binary_sha256'],self.p.D.sha(Path(result['bundle_path'])/'Contents/MacOS/MellowGPU'))
  self.assertFalse(result['hardware_tested']);self.assertIn('structural_report',result['evidence_files'])
 def test_incomplete_native_build_or_promoted_runtime_claim_rejected(self):
  for key,value in [('status','failed'),('hardware_tested',True),('runtime_loaded',True),('production_installable',True)]:
   original=copy.deepcopy(self.receipt);self.receipt[key]=value;self.write_receipt()
   with self.subTest(key=key),self.assertRaises(ValueError):self.package()
   self.receipt=original
  self.write_receipt()
 def test_ignored_uncommitted_license_cannot_enter_package(self):
  extra=self.repo/'LICENSES/__pycache__';extra.mkdir();(extra/'uncommitted.txt').write_text('not in HEAD')
  with self.assertRaisesRegex(ValueError,'license|LICENSE'):self.package()
 def test_git_export_ignore_cannot_silently_omit_committed_source_licenses(self):
  (self.repo/'.git/info/attributes').write_text('LICENSE export-ignore\nNOTICE export-ignore\n')
  self.assertEqual(self.p.D.source_snapshot(self.repo)['files'],self.receipt['source_sha256'])
  with self.assertRaisesRegex(ValueError,'archive'):self.package()
  self.assertFalse((self.base/'release/package-result.json').exists())
 def test_git_export_substitution_cannot_rewrite_committed_source_bytes(self):
  (self.repo/'format.txt').write_text('$Format:%H$\n');self.git('add','format.txt');self.git('commit','-qm','Source substitution regression')
  snap=self.p.D.source_snapshot(self.repo);self.receipt.update(source_commit=snap['commit'],source_tree=snap['tree'],source_sha256=snap['files']);self.write_receipt()
  (self.repo/'.git/info/attributes').write_text('format.txt export-subst\n')
  with self.assertRaisesRegex(ValueError,'archive'):self.package()
if __name__=='__main__':unittest.main(verbosity=2)

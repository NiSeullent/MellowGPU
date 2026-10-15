#!/usr/bin/env python3
"""Independent static release controls and deliberate corruptions; no hardware claims."""
import importlib.util,json,plistlib,re,shutil,struct,sys,tarfile,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PACKAGE=Path(sys.argv.pop(1)).resolve()
spec=importlib.util.spec_from_file_location('package_validator',ROOT/'Tools/validate-255u-package.py')
V=importlib.util.module_from_spec(spec);spec.loader.exec_module(V)

def x64_kext(data):
    if data[:4]==bytes.fromhex('cafebabe'):
        count=struct.unpack_from('>I',data,4)[0]
        assert 0<count<=16 and 8+20*count<=len(data)
        for i in range(count):
            cpu,sub,off,size,align=struct.unpack_from('>5I',data,8+20*i)
            if cpu==0x1000007:
                assert off+size<=len(data)
                return x64_kext(data[off:off+size])
        raise AssertionError('No x86_64 executable slice')
    assert len(data)>=32
    magic,cpu,sub,kind,n,commands,flags,reserved=struct.unpack_from('<8I',data)
    assert magic==0xfeedfacf and cpu==0x1000007 and kind==11
    assert n>0 and 32+commands<=len(data)

class PackageControls(unittest.TestCase):
    def test_original_hashes_and_truthful_scope(self):
        self.assertTrue(V.validate(PACKAGE,True)['passed'])
        status=json.loads((PACKAGE/'STATUS.json').read_text())
        for key in ('target_boot_verified','target_kernel_load_verified','native_gpu_submission_implemented','apple_metal_abi_implemented','full_metal_acceleration_implemented'):
            self.assertIs(status[key],False)

    def test_real_kexts_and_dependency_order(self):
        oc=PACKAGE/'EFI/OC'; cfg=plistlib.loads((oc/'config.plist').read_bytes()); loaded=set()
        for entry in cfg['Kernel']['Add']:
            bundle=oc/'Kexts'/entry['BundlePath']
            info=plistlib.loads((bundle/entry['PlistPath']).read_bytes())
            x64_kext((bundle/entry['ExecutablePath']).read_bytes())
            if not entry['Enabled']: continue
            for dep in info.get('OSBundleLibraries',{}):
                if not dep.startswith('com.apple.'): self.assertIn(dep,loaded)
            self.assertNotIn(info['CFBundleIdentifier'],loaded)
            loaded.add(info['CFBundleIdentifier'])
        self.assertIn('com.NiSeullent.MellowGPU',loaded)
        info=plistlib.loads((oc/'Kexts/MellowGPU.kext/Contents/Info.plist').read_bytes())
        self.assertEqual(info['CFBundleVersion'],'0.5.0')
        self.assertEqual(info['IOKitPersonalities']['MellowTahoeDiagnostic']['IOPCIPrimaryMatch'].lower(),'0x7d418086')

    def test_acpi_uid_and_headers(self):
        hardware=json.loads((PACKAGE/'hardware.json').read_text())
        dsl=(PACKAGE/'Sources/ACPI/SSDT-PLUG-255U.dsl').read_text()
        uids=[int(x,16) for x in re.findall(r'Processor \(CP[0-9A-F]{2}, 0x([0-9A-F]{2}),',dsl)]
        self.assertEqual(uids,sorted(row['uid'] for row in hardware['enabled_madt']))
        self.assertEqual(len(uids),14)
        for path in (PACKAGE/'EFI/OC/ACPI').glob('*.aml'):
            data=path.read_bytes(); self.assertEqual(data[:4],b'SSDT');self.assertEqual(sum(data)%256,0)
            self.assertEqual(struct.unpack_from('<I',data,4)[0],len(data));self.assertEqual(data[10:16],b'MELLOW')

    def test_source_archives_exclude_private_input_and_fonts(self):
        for source in (PACKAGE/'Sources').glob('*.tar.gz'):
            with tarfile.open(source) as archive:
                for item in archive.getmembers():
                    name=Path(item.name)
                    self.assertNotIn(name.name.lower(),('sysreport.zip','msdm.aml','dsdt.aml','task-cgl.txt','task-ggtt.txt'))
                    self.assertNotIn(name.suffix.lower(),('.ttf','.otf','.woff','.woff2','.pf2','.fon','.fnt'))

    def test_exact_manifest_rejects_unlisted_files_and_duplicate_entries(self):
        with tempfile.TemporaryDirectory(prefix='mellow-manifest-controls-') as temp:
            root=Path(temp)/'package';shutil.copytree(PACKAGE,root)
            extra=root/'unexpected.txt';extra.write_text('not in the source release')
            with self.assertRaises(ValueError): V.validate(root,True)
            extra.unlink()
            manifest=root/'SHA256SUMS';original=manifest.read_text()
            manifest.write_text(original+original.splitlines()[0]+'\n')
            with self.assertRaises(ValueError): V.validate(root,True)

    def test_uefi_traversal_and_wrong_executable_rejected(self):
        with tempfile.TemporaryDirectory(prefix='mellow-path-controls-') as temp:
            root=Path(temp)/'package';shutil.copytree(PACKAGE,root)
            config=root/'EFI/OC/config.plist';original=config.read_bytes()
            cfg=plistlib.loads(original)
            cfg['UEFI']['Drivers'][0]['Path']='../OpenCore.efi'
            config.write_bytes(plistlib.dumps(cfg))
            with self.assertRaises(ValueError): V.validate(root)
            config.write_bytes(original)
            binary=root/'EFI/OC/Kexts/MellowGPU.kext/Contents/MacOS/MellowGPU'
            data=bytearray(binary.read_bytes());data[-1]^=1;binary.write_bytes(data)
            with self.assertRaises(ValueError): V.validate(root)

    def test_corruptions_rejected(self):
        with tempfile.TemporaryDirectory(prefix='mellow-negative-package-') as temp:
            root=Path(temp)/'package';shutil.copytree(PACKAGE,root)
            file=root/'STATUS.json'; original=file.read_bytes(); status=json.loads(original)
            status['full_metal_acceleration_implemented']=True;file.write_text(json.dumps(status))
            with self.assertRaises(ValueError): V.validate(root)
            file.write_bytes(original)
            config=root/'EFI/OC/config.plist'; original_config=config.read_bytes(); cfg=plistlib.loads(original_config)
            cfg['ACPI']['Add'][0]['Path']='../../../../not-an-acpi-table.aml';config.write_bytes(plistlib.dumps(cfg))
            with self.assertRaises(ValueError): V.validate(root)
            config.write_bytes(original_config)
            image=root/'EFI/OC/OpenCore.efi'; image.write_bytes(image.read_bytes()+b'altered')
            with self.assertRaises(ValueError): V.validate(root,True)

if __name__=='__main__': unittest.main(verbosity=2)

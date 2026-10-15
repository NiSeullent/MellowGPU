#!/usr/bin/env python3
"""Assemble a pinned experimental Sequoia EFI. Does not install, flash, or claim Metal support."""
import argparse, copy, hashlib, json, os, plistlib, re, shutil, stat, struct, subprocess, tarfile, tempfile, urllib.request, zipfile
from pathlib import Path, PurePosixPath
from efi_boot_profiles import build_profiles
from product_identity import BUNDLE, LEGACY_BUNDLE, VERSION, DEVELOPMENT_LABEL
ROOT=Path(__file__).resolve().parents[1]
FONT_SUFFIXES={'.ttf','.otf','.woff','.woff2','.pfb','.pfm','.fon','.fnt'}
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def fetch(item,cache):
    target=cache/item['asset']
    if not target.exists():
        req=urllib.request.Request(item['url'],headers={'User-Agent':'Mellow-Sequoia-Reproducible-Build'})
        with urllib.request.urlopen(req,timeout=120) as response: data=response.read(100*1024*1024+1)
        if len(data)>100*1024*1024: raise ValueError('Asset exceeds download bound')
        target.write_bytes(data)
    if sha(target)!=item['sha256']: raise ValueError('Pinned digest mismatch: '+item['asset'])
    if 'bytes' in item and target.stat().st_size!=item['bytes']: raise ValueError('Pinned size mismatch')
    return target

def unzip(path,dest):
    with zipfile.ZipFile(path) as z:
        if sum(i.file_size for i in z.infolist())>500*1024*1024: raise ValueError('Expanded archive too large')
        for item in z.infolist():
            rel=PurePosixPath(item.filename)
            if rel.is_absolute() or '..' in rel.parts or '\\' in item.filename: raise ValueError('Unsafe ZIP path')
            if stat.S_ISLNK(item.external_attr>>16): raise ValueError('Unexpected ZIP symlink')
            if item.is_dir(): continue
            target=dest/str(rel); target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(z.read(item)); target.chmod(0o755 if item.external_attr>>16 & 0o111 else 0o644)

def treecopy(src,dst):
    if not src.is_dir(): raise FileNotFoundError(src)
    shutil.copytree(src,dst)

def pe_header(path):
    data=path.read_bytes()
    if len(data)<64 or data[:2]!=b'MZ': raise ValueError('Not PE: '+str(path))
    offset=struct.unpack_from('<I',data,0x3c)[0]
    if offset+94>len(data) or data[offset:offset+4]!=b'PE\0\0': raise ValueError('Invalid PE header')
    machine=struct.unpack_from('<H',data,offset+4)[0]
    subsystem=struct.unpack_from('<H',data,offset+24+68)[0]
    if machine!=0x8664 or subsystem not in (10,11,12): raise ValueError('Not an x86_64 UEFI image')
    return {'machine':hex(machine),'subsystem':subsystem,'sha256':sha(path)}

def configuration(sample,oc):
    p=copy.deepcopy(sample)
    for category,names in {'ACPI':['Add','Delete','Patch'],'Booter':['MmioWhitelist','Patch'],
                           'Kernel':['Add','Block','Force','Patch'],'Misc':['BlessOverride','Entries','Tools'],
                           'UEFI':['Drivers','ReservedMemory']}.items():
        for name in names: p[category][name]=[]
    p['DeviceProperties']={'Add':{},'Delete':{}}
    for filename in sorted((oc/'ACPI').glob('*.aml')):
        p['ACPI']['Add'].append({'Comment':'255U reviewed additive '+filename.stem,'Enabled':True,'Path':filename.name})
    p['Booter']['Quirks'].update(AvoidRuntimeDefrag=True,DevirtualiseMmio=True,EnableSafeModeSlide=True,
        EnableWriteUnprotector=False,ProtectUefiServices=True,ProvideCustomSlide=True,
        RebuildAppleMemoryMap=True,SetupVirtualMap=True,SyncRuntimePermissions=True,ResizeAppleGpuBars=-1)
    # Kernel-model experiment only. GPU identity and CPUID feature registers are untouched.
    p['Kernel']['Emulate'].update(Cpuid1Data=struct.pack('<4I',0x000906ea,0,0,0),
        Cpuid1Mask=struct.pack('<4I',0xffffffff,0,0,0),DummyPowerManagement=False,
        MinKernel='24.0.0',MaxKernel='24.99.99')
    p['Kernel']['Quirks'].update(AppleXcpmCfgLock=True,AppleXcpmExtraMsrs=True,
        AppleXcpmForceBoost=False,DisableIoMapper=True,DisableLinkeditJettison=True,
        PanicNoKextDump=True,ProvideCurrentCpuInfo=True,XhciPortLimit=False)
    p['Kernel']['Scheme'].update(KernelArch='x86_64',KernelCache='Auto')
    order=['Lilu.kext','VirtualSMC.kext','SMCBatteryManager.kext','NVMeFix.kext',
           'VoodooPS2Controller.kext','VoodooPS2Controller.kext/Contents/PlugIns/VoodooPS2Keyboard.kext',
           'RealtekRTL8111.kext','MellowGPU.kext']
    for rel in order:
        info=plistlib.loads((oc/'Kexts'/rel/'Contents/Info.plist').read_bytes())
        exe=info.get('CFBundleExecutable')
        p['Kernel']['Add'].append({'Arch':'x86_64','BundlePath':rel,
          'Comment':f'MellowGPU {DEVELOPMENT_LABEL} diagnostic only; native Metal missing' if rel=='MellowGPU.kext' else info['CFBundleIdentifier'],
          'Enabled':True,'ExecutablePath':'Contents/MacOS/'+exe if exe else '',
          'MaxKernel':'24.99.99','MinKernel':'24.0.0','PlistPath':'Contents/Info.plist'})
    p['Misc']['Boot'].update(HideAuxiliary=True,LauncherOption='Disabled',PickerMode='Builtin',
        ShowPicker=True,Timeout=5,PickerAudioAssist=False,PollAppleHotKeys=False)
    p['Misc']['Security'].update(AllowSetDefault=False,DmgLoading='Signed',ScanPolicy=0,
        SecureBootModel='Disabled',Vault='Optional',ExposeSensitiveData=6)
    p['Misc']['Debug'].update(AppleDebug=True,ApplePanic=True,DisableWatchDog=True,Target=67,DisplayLevel=2147483650)
    t=copy.deepcopy(sample['Misc']['Tools'][0]); t.update(Arguments='',Auxiliary=True,Comment='UEFI shell: read-only inspection is recommended',
        Enabled=True,Name='OpenShell',Path='OpenShell.efi',RealPath=False,TextMode=False)
    p['Misc']['Tools']=[t]
    p['NVRAM']['Add']={'7C436110-AB2A-4BBB-A880-FE41995C9F82':{
        'boot-args':'-v keepsyms=1 debug=0x100 -mellowdiag','csr-active-config':bytes(4)}}
    p['NVRAM']['Delete']={'7C436110-AB2A-4BBB-A880-FE41995C9F82':['boot-args','csr-active-config']}
    p['NVRAM']['LegacySchema']={}
    p['NVRAM'].update(LegacyOverwrite=False,WriteFlash=False)
    p['PlatformInfo'].update(Automatic=True,UpdateDataHub=True,UpdateNVRAM=True,UpdateSMBIOS=True,UpdateSMBIOSMode='Create')
    # Public laboratory placeholders. Offline personalization is required before real use.
    p['PlatformInfo']['Generic'].update(MLB='M0000000000000001',ROM=bytes.fromhex('020000002550'),
        SystemProductName='MacBookPro16,2',SystemSerialNumber='W00000000001',
        SystemUUID='00000000-0000-4000-8000-000000000255',SpoofVendor=True,ProcessorType=0)
    p['UEFI']['ConnectDrivers']=True
    for name in ('OpenRuntime.efi','OpenHfsPlus.efi'):
        entry=copy.deepcopy(sample['UEFI']['Drivers'][0]); entry.update(Arguments='',Comment=name,Enabled=True,LoadEarly=False,Path=name)
        p['UEFI']['Drivers'].append(entry)
    p['UEFI']['Input'].update(KeySupport=True,KeySupportMode='Auto',PointerSupport=False)
    p['UEFI']['Output'].update(ProvideConsoleGop=True,Resolution='Max',TextRenderer='BuiltinGraphics',UIScale=1)
    p['UEFI']['Quirks'].update(ResizeGpuBars=-1,RequestBootVarRouting=True,ForceOcWriteFlash=False)
    return p

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--native',type=Path,required=True,help='Completed matching native CI artifact directory')
    ap.add_argument('--out',type=Path,required=True,help='New output directory; never an ESP')
    ap.add_argument('--iasl',default='iasl'); ap.add_argument('--cache',type=Path,default=ROOT/'build/dependencies')
    args=ap.parse_args(); out=args.out.resolve(); native=args.native.resolve()
    if out.exists(): raise SystemExit('Output directory already exists; choose a new empty output path')
    if subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],cwd=ROOT,text=True).strip():
        raise ValueError('Package assembly requires a clean, fully committed source checkout')
    source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    if (native/'source-commit.txt').read_text().strip()!=source_commit: raise ValueError('Native binary belongs to a different source commit')
    if not json.loads((native/'kernel-simd-audit.json').read_text())['passed']: raise ValueError('Kernel SIMD audit failed')
    if not json.loads((native/'host-tests/report.json').read_text())['passed']: raise ValueError('Native host tests failed')
    lock=json.loads((ROOT/'EFI-255U/dependencies.lock.json').read_text()); args.cache.mkdir(parents=True,exist_ok=True)
    out.mkdir(parents=True); stage=out/'package'; stage.mkdir(); oc=stage/'EFI/OC'
    (oc/'ACPI').mkdir(parents=True); (oc/'Drivers').mkdir(); (oc/'Kexts').mkdir(); (oc/'Tools').mkdir(); (stage/'EFI/BOOT').mkdir()
    verification=stage/'Verification'; verification.mkdir(); tools=stage/'Tools'; tools.mkdir()
    sources=stage/'Sources'; sources.mkdir(); licenses=stage/'Licenses'; licenses.mkdir()
    with tempfile.TemporaryDirectory(prefix='mellow-efi-components-') as temporary:
        unpack=Path(temporary); assets={}
        for item in lock['assets']:
            path=fetch(item,args.cache); folder=unpack/item['name']; folder.mkdir(); unzip(path,folder); assets[item['name']]=folder
            if item.get('corresponding_source'):
                source=fetch(item['corresponding_source'],args.cache)
                with tarfile.open(source) as t:
                    if any(Path(m.name).suffix.lower() in FONT_SUFFIXES for m in t.getmembers()): raise ValueError('Font redistribution excluded')
                shutil.copy2(source,sources/source.name)
        base=assets['OpenCore']; x64=base/'X64/EFI'
        shutil.copy2(x64/'BOOT/BOOTx64.efi',stage/'EFI/BOOT/BOOTx64.efi'); shutil.copy2(x64/'OC/OpenCore.efi',oc/'OpenCore.efi')
        for filename in ('OpenRuntime.efi','OpenHfsPlus.efi'): shutil.copy2(x64/'OC/Drivers'/filename,oc/'Drivers'/filename)
        shutil.copy2(x64/'OC/Tools/OpenShell.efi',oc/'Tools/OpenShell.efi')
        for tool in ('ocvalidate','macserial'):
            (tools/tool).mkdir()
            for file in (base/'Utilities'/tool).iterdir():
                if file.is_file() and file.suffix.lower() not in FONT_SUFFIXES:
                    shutil.copy2(file,tools/tool/file.name)
                    if not file.suffix or file.suffix=='.linux': (tools/tool/file.name).chmod(0o755)
        locations=[('Lilu','Lilu.kext'),('VirtualSMC','Kexts/VirtualSMC.kext'),('VirtualSMC','Kexts/SMCBatteryManager.kext'),
                   ('NVMeFix','NVMeFix.kext'),('VoodooPS2','VoodooPS2Controller.kext'),
                   ('RealtekRTL8111','RealtekRTL8111-V3.0.0/Release/RealtekRTL8111.kext')]
        for asset,rel in locations: treecopy(assets[asset]/rel,oc/'Kexts'/Path(rel).name)
        plugins=oc/'Kexts/VoodooPS2Controller.kext/Contents/PlugIns'
        for child in plugins.iterdir():
            if child.name!='VoodooPS2Keyboard.kext': shutil.rmtree(child)
        treecopy(native/'MellowGPU.kext',oc/'Kexts/MellowGPU.kext')
        # Preserve original guarded personalities and the exact built Info.plist.
        # Preserve the exact built Info.plist; no post-link identity or Mach-O patching occurs.
        acpi_reports=[]
        for dsl in sorted((ROOT/'EFI-255U/ACPI').glob('*.dsl')):
            prefix=unpack/dsl.stem
            cmd=[args.iasl,'-p',str(prefix),str(dsl)]
            result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
            acpi_reports.append({'file':dsl.name,'command':cmd,'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr,'source_sha256':sha(dsl)})
            if result.returncode: raise RuntimeError(result.stdout+result.stderr)
            aml=prefix.with_suffix('.aml'); data=aml.read_bytes()
            if data[:4]!=b'SSDT' or len(data)!=struct.unpack_from('<I',data,4)[0] or sum(data)%256: raise ValueError('Invalid generated SSDT')
            shutil.copy2(aml,oc/'ACPI'/aml.name)
        (verification/'acpi-compile.json').write_text(json.dumps(acpi_reports,indent=2)+'\n')
        config=configuration(plistlib.loads((base/'Docs/Sample.plist').read_bytes()),oc)
        (oc/'config.plist').write_bytes(plistlib.dumps(config,sort_keys=False))
        profiles=stage/'Profiles'; profiles.mkdir()
        for name, profile in build_profiles(config).items():
            (profiles/name).write_bytes(plistlib.dumps(profile,sort_keys=False))
        tests=[]
        validator=tools/'ocvalidate/ocvalidate.linux'
        import platform
        if platform.system()=='Darwin': validator=tools/'ocvalidate/ocvalidate'
        for p in [oc/'config.plist',*sorted(profiles.glob('*.plist'))]:
            r=subprocess.run([str(validator),str(p)],capture_output=True,text=True,timeout=30)
            tests.append({'profile':str(p.relative_to(stage)),'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
            if r.returncode: raise RuntimeError('OpenCore validator failed: '+r.stdout+r.stderr)
        (verification/'ocvalidate.json').write_text(json.dumps(tests,indent=2)+'\n')
    for p in (ROOT/'EFI-255U/Licenses').iterdir(): shutil.copy2(p,licenses/p.name)
    shutil.copy2(ROOT/'LICENSE',licenses/'Mellow-LICENSE'); shutil.copy2(ROOT/'NOTICE',licenses/'Mellow-NOTICE')
    treecopy(ROOT/'LICENSES',licenses/'Mellow-component-licenses')
    for name in ('sequoia-probe','tahoe-diag-client','metal-probe'):
        shutil.copy2(native/name,tools/name); (tools/name).chmod(0o755)
    for name in ('personalize-255u.py','product_identity.py','validate-255u-package.py','metal-run.py','metal-probe.swift','collect-255u-logs.sh'):
        shutil.copy2(ROOT/'Tools'/name,tools/name)
    for name in ('macho-validation.json','kernel-simd-audit.json','source-commit.txt','xcode-version.txt','build-host-version.txt','cgl-attempt-exit-code.txt','VALIDATION-SCOPE.txt'):
        if (native/name).exists(): shutil.copy2(native/name,verification/name)
    for name in ('host-tests','render-shader','cgl-build','msl-render-build','cgl-attempt'):
        folder=native/name
        if folder.exists():
            target=verification/name; target.mkdir()
            for file in folder.glob('*.json'): shutil.copy2(file,target/file.name)
    shutil.copy2(ROOT/'EFI-255U/hardware.json',stage/'hardware.json'); shutil.copy2(ROOT/'EFI-255U/dependencies.lock.json',stage/'dependencies.lock.json')
    # Preserve the historical source document. Adapt the generated delivery's
    # active product references, rather than shipping instructions for old bytes.
    delivery_readme=(ROOT/'EFI-255U/README-FIRST.ko.md').read_text()
    delivery_readme=delivery_readme.replace(LEGACY_BUNDLE,BUNDLE).replace('Mellow 0.4.4','MellowGPU '+DEVELOPMENT_LABEL)
    (stage/'README-FIRST.ko.md').write_text(delivery_readme)
    treecopy(ROOT/'EFI-255U/ACPI',sources/'ACPI')
    with open(sources/'Mellow-source.tar.gz','wb') as f:
        subprocess.run(['git','archive','--format=tar.gz','--prefix=Mellow/',source_commit],cwd=ROOT,stdout=f,check=True)
    for file in stage.rglob('*'):
        if file.suffix.lower() in FONT_SUFFIXES: raise ValueError('No font files are distributed')
        if file.name.lower() in ('msdm.aml','sysreport.zip','report.json') and 'Verification' not in file.parts:
            raise ValueError('Raw user data must not be published')
    pe={str(p.relative_to(stage)):pe_header(p) for p in (stage/'EFI').rglob('*.efi')}
    (verification/'uefi-pe-images.json').write_text(json.dumps(pe,indent=2)+'\n')
    status={'schema':'mellow.efi-delivery/1','status':'PARTIAL_EXPERIMENTAL_NOT_FULL_METAL',
      'target':'SAMSUNG NT751XHD-KR735 / Core Ultra 7 255U / physical 8086:7D41',
      'os_target':'macOS Sequoia / Darwin 24','source_repository':'https://github.com/NiSeullent/MellowGPU',
      'source_branch':os.environ.get('GITHUB_REF_NAME') or subprocess.check_output(
          ['git','branch','--show-current'],cwd=ROOT,text=True).strip() or 'detached',
      'source_commit':source_commit,
      'ci_url':os.environ.get('GITHUB_SERVER_URL','https://github.com')+'/'+os.environ.get('GITHUB_REPOSITORY','NiSeullent/Mellow')+'/actions/runs/'+os.environ.get('GITHUB_RUN_ID','local'),
      'mellow_version':VERSION,'product':'MellowGPU','development_label':DEVELOPMENT_LABEL,'opencore_version':'1.0.7',
      'target_boot_verified':False,'target_kernel_load_verified':False,'native_gpu_submission_implemented':False,
      'apple_metal_abi_implemented':False,'full_metal_acceleration_implemented':False,'windowserver_acceleration_verified':False,
      'real_kernel_binary_built':True,'read_only_physical_probe_implemented':True,
      'ggtt_manager_implemented':True,'ggtt_physical_adapter_integrated':False,
      'cgl_host_provider_implemented':True,'cgl_supplies_7d41_driver':False,
      'smbios_personalization_required':True,'raw_hardware_report_published':False,
      'miro_diagram_contents_retrieved':False,'required_next_work':['physical native GPU ownership and firmware/submission integration','Apple Metal ABI/user-mode driver','target boot/load/GPU readback testing']}
    (stage/'STATUS.json').write_text(json.dumps(status,indent=2)+'\n')
    result=subprocess.run(['python3',str(ROOT/'Tools/validate-255u-package.py'),str(stage)],capture_output=True,text=True,timeout=90)
    (verification/'package-validation.json').write_text(result.stdout)
    if result.returncode: raise RuntimeError(result.stdout+result.stderr)
    hashes=[]
    for file in sorted(stage.rglob('*')):
        if file.is_file(): hashes.append(sha(file)+'  '+file.relative_to(stage).as_posix())
    (stage/'SHA256SUMS').write_text('\n'.join(hashes)+'\n')
    archive=out/'EFI.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for file in sorted(stage.rglob('*')):
            if not file.is_file(): continue
            info=zipfile.ZipInfo(file.relative_to(stage).as_posix(),date_time=(2026,9,25,0,0,0))
            info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=(stat.S_IFREG | (0o755 if file.stat().st_mode & 0o111 else 0o644))<<16
            z.writestr(info,file.read_bytes())
    (out/'EFI.zip.sha256').write_text(sha(archive)+'  EFI.zip\n')
    print(json.dumps({'archive':str(archive),'sha256':sha(archive),'bytes':archive.stat().st_size,'status':status['status']}))
if __name__=='__main__': main()

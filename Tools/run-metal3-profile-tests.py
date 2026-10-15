#!/usr/bin/env python3
"""Execute actual Metal3 support-profile policy tests, with no hardware claim."""
import argparse,datetime,hashlib,json,pathlib,shutil,subprocess,sys,tempfile
def execute(root,out,cxx,sanitize,baseline):
    out.mkdir(parents=True,exist_ok=True)
    paths=['tests/metal3_profile_tests.cpp','Tools/run-metal3-profile-tests.py',
           'tests/metal3_profile_baseline.hpp' if baseline else 'Drivers/Core/Metal3Profile.hpp']
    record={'schema_version':1,'status':'FAIL','scope':'synthetic-profile-policy-only',
            'physical_gpu_executed':False,'metal3_conformance_tested':False,'hardware_attested':False,
            'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in paths},
            'at':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    command=[cxx,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-fno-rtti','-fno-exceptions',
             str(root/'tests/metal3_profile_tests.cpp'),'-o',str(out/'metal3-profile-tests')]
    if sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    if baseline:command+=['-DMELLOWGPU_PROFILE_BASELINE']
    try:
        record['command']=command
        r=subprocess.run(command,capture_output=True,text=True,timeout=120)
        record['build']={'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        if r.returncode:print(r.stderr,file=sys.stderr);return r.returncode
        r=subprocess.run([str(out/'metal3-profile-tests')],capture_output=True,text=True,timeout=30)
        record['test']={'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        print(r.stdout+r.stderr,end='')
        if r.returncode==0:record['status']='PASS_SOFTWARE_POLICY_ONLY'
        return r.returncode
    finally:(out/'metal3-profile-tests.json').write_text(json.dumps(record,indent=2)+'\n')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cxx',default=shutil.which('clang++')or shutil.which('g++'))
    p.add_argument('--out',type=pathlib.Path);p.add_argument('--sanitize',action='store_true');p.add_argument('--baseline',action='store_true');a=p.parse_args()
    if not a.cxx:p.error('C++17 compiler required')
    root=pathlib.Path(__file__).resolve().parents[1]
    if a.out:return execute(root,a.out.resolve(),a.cxx,a.sanitize,a.baseline)
    with tempfile.TemporaryDirectory(prefix='mellowgpu-profile-')as d:return execute(root,pathlib.Path(d),a.cxx,a.sanitize,a.baseline)
if __name__=='__main__':raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual IOService/user-client bodies with fault-injected public IOKit shims."""
import argparse,hashlib,json,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cxx',default=shutil.which('clang++')or shutil.which('g++'));p.add_argument('--out',type=Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
 out=a.out.resolve()
 if not a.cxx or out.exists() or out==ROOT or out in ROOT.parents:p.error('compiler and fresh scratch output required')
 paths=[ROOT/n for n in ('Mellow/MellowGPUDevice.hpp','Mellow/MellowGPUDevice.cpp','Drivers/Core/MellowGPUABI.h','Drivers/Core/ControlProtocol.hpp','Drivers/Core/DeviceLifecycle.hpp','tests/native_control_tests.cpp','tests/native_control_shim.hpp','Tools/run-native-control-tests.py')]
 paths+=sorted(x for x in (ROOT/'tests/native_control_shim').rglob('*')if x.is_file())
 digest=lambda x:hashlib.sha256(x.read_bytes()).hexdigest();before={x.relative_to(ROOT).as_posix():digest(x)for x in paths}
 out.mkdir(parents=True);binary=out/'native-control-tests'
 command=[a.cxx,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-fno-exceptions','-I'+str(ROOT/'tests'),'-I'+str(ROOT/'tests/native_control_shim'),str(ROOT/'tests/native_control_tests.cpp'),str(ROOT/'Mellow/MellowGPUDevice.cpp'),'-o',str(binary)]
 if a.sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 report={'status':'FAIL','scope':'actual native adapter bodies with simulated public IOKit/task/PCI/memory boundaries','native_iokit_executed':False,'physical_gpu_executed':False,'metal_tested':False,'source_sha256':before,'command':command}
 try:
  r=subprocess.run(command,capture_output=True,text=True,timeout=120);report['build']={'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
  if r.returncode:raise ValueError('native-control shim compile failed: '+r.stderr)
  r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=60);report['test']={'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr};print(r.stdout,end='')
  if r.returncode:raise ValueError('native-control shim execution failed: '+r.stderr)
  report['binary_sha256']=digest(binary);report['source_unchanged']=before=={x.relative_to(ROOT).as_posix():digest(x)for x in paths}
  if not report['source_unchanged']:raise ValueError('source changed during native-control tests')
  report['status']='PASS_NATIVE_CONTROL_SHIMS_ONLY'
 except (ValueError,OSError,subprocess.SubprocessError)as error:report['error']=str(error);print(error,file=sys.stderr)
 (out/'native-control-tests.json').write_text(json.dumps(report,indent=2)+'\n');return 0 if report['status']=='PASS_NATIVE_CONTROL_SHIMS_ONLY' else 1
if __name__=='__main__':raise SystemExit(main())

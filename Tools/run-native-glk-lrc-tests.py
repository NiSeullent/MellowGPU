#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test actual GLK LRC codec bodies; no device access or hardware execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from sanitizer_policy import sanitizer_environment

PINNED = {
 'gt/intel_lrc.c':'b10f6affc652967c619a846982bb9731f24ec8ef3b8d46daf428adb76f5b3843',
 'gt/intel_lrc.h':'e71534746b6f6720c978c0096316c51873f5afc2974d94cd4979303384a6f5b3',
 'gt/intel_lrc_reg.h':'671b2adfb2825597425eea515b892cba1365f658d552f9a90439182f0f7f21c0',
 'gt/intel_engine_cs.c':'3b832bd7e1d5225cb336749f5ccd17a08fa6ddbe1d62387972f1606b144375e4',
 'gt/intel_engine_regs.h':'e6ae20bc2946a252a948628df6a84522c19dd45d537f5efa0e5c55d08e82961a',
 'gt/intel_context_types.h':'999dc3bf65b9f73ed08dd8210b41787c135922f3bbddbc6461e774cb755320f4',
 'gt/intel_execlists_submission.c':'44c53b014628fdd409d089d15209020a79a0098bf2b6f4c477d89df73ddf5cf2',
 'gt/intel_gpu_commands.h':'d8d14b3edefb858418eb52506f5f7598ee2cd6bd8affa659e43faaf11c85e77c',
 'i915_reg.h':'4378b821119b67119ed3b7d218c3563d481271e1a16dcd59c1fd258306140a85',
}
RING_SHA = '2057ac91d51ef486292595390de04e7e3435acafc2fa35a237de3bfd7b23c035'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def run(command, timeout=120, env=None):
    result = subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=timeout,env=env)
    return {'command':list(map(str,command)), 'returncode':result.returncode,
            'stdout':result.stdout,'stderr':result.stderr}
def table(text, name):
    match=re.search(r'\b'+re.escape(name)+r'\[\]\s*=\s*\{(.*?)\};',text,re.S)
    if not match: raise ValueError('Missing source oracle '+name)
    source=re.sub(r'/\*.*?\*/|//[^\n]*','',match.group(1),flags=re.S)
    return [str(int(token,0)) if re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+',token) else token
            for token in re.findall(r'0x[0-9a-fA-F]+|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|[^\s]',source)]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx',default=shutil.which('clang++') or '')
    p.add_argument('--linux-root',required=True,type=Path)
    p.add_argument('--ring-header',required=True,type=Path)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--kernel-sdk',type=Path,help='Optional explicit macOS SDK for x86_64 kernel-style compile only')
    args=p.parse_args();root=Path(__file__).resolve().parents[1];out=args.out.resolve()
    if os.environ.get('CCC_OVERRIDE_OPTIONS'): raise ValueError('Compiler argument override refused')
    out.mkdir(parents=True,exist_ok=False)
    names=['Drivers/NativeGlk/LrcImage.hpp','Drivers/NativeGlk/LrcImage.cpp',
           'tests/native_glk_lrc_tests.cpp','Tools/run-native-glk-lrc-tests.py','Tools/sanitizer_policy.py']
    before={name:sha(root/name) for name in names}
    report={'schema':'mellowgpu.glk-lrc-host-codec-tests/1','source_sha256':before,
            'linux_commit':'0d9ff90a5422cc7509258aaaba1e7481df4d332a','status':'FAIL',
            'physical_gpu_executed':False,'native_driver_executed':False,
            'hardware_support_qualified':False,'metal_conformance_tested':False,
            'windowserver_tested':False,'sanitizers':['address','undefined'],'fatal_sanitizers':True}
    result=1
    try:
        linux=args.linux_root/'drivers/gpu/drm/i915'
        actual={name:sha(linux/name) for name in PINNED}
        if actual!=PINNED or sha(args.ring_header)!=RING_SHA:
            raise ValueError('Pinned Linux source oracle drift')
        upstream=(linux/'gt/intel_lrc.c').read_text();tests=(root/names[2]).read_text()
        for name in ['gen9_xcs_offsets','gen9_rcs_offsets']:
            if table(upstream,name)!=table(tests,name):
                raise ValueError('Test register table differs from actual upstream '+name)
        report['source_oracles']={'sha256':actual,'ring_header_sha256':RING_SHA,
                                  'whole_gen9_table_token_comparison':True}
        report['compiler']=run([args.cxx,'--version'],20)
        if report['compiler']['returncode']: raise ValueError('Compiler version query failed')
        common=[args.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror','-pedantic',
                '-fno-exceptions','-fno-rtti','-I',root]
        binary=out/'native-glk-lrc-tests'
        report['compile']=run([*common,'-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer',root/names[1],root/names[2],'-o',binary])
        if report['compile']['returncode']: raise ValueError('Host codec build failed')
        report['binary_sha256']=sha(binary)
        nm=run(['nm','-u',binary],20)
        if nm['returncode'] or '__asan_' not in nm['stdout'] or '__ubsan_' not in nm['stdout']:
            raise ValueError('Actual sanitizer instrumentation missing')
        report['sanitizer_symbols_verified']=True
        env,report['sanitizer_runtime']=sanitizer_environment()
        report['test']=run([binary],30,env)
        match=re.fullmatch(r'native_glk_lrc_tests: ([0-9]+) checks passed\n',report['test']['stdout'])
        if report['test']['returncode'] or report['test']['stderr'] or not match:
            raise ValueError('Host codec tests or fatal sanitizer checks failed')
        report['checks']=int(match.group(1))
        if args.kernel_sdk:
            if not (args.kernel_sdk/'SDKSettings.plist').is_file(): raise ValueError('Explicit SDK missing')
            obj=out/'LrcImage.kernel-x86.o'
            report['kernel_style_compile']=run([*common,'-target','x86_64-apple-macos15.8.1',
                '-isysroot',args.kernel_sdk,'-mkernel','-ffreestanding','-fno-builtin','-fno-stack-protector',
                '-nostdinc++','-DKERNEL','-DKERNEL_PRIVATE','-c',root/names[1],'-o',obj])
            if report['kernel_style_compile']['returncode'] or report['kernel_style_compile']['stderr']:
                raise ValueError('x86 kernel-style compile failed')
            report['kernel_style_object_sha256']=sha(obj)
            report['kernel_sdk_settings_sha256']=sha(args.kernel_sdk/'SDKSettings.plist')
            report['kernel_style_scope']='Freestanding source/object compile only; no KDK/KPI link, kext loading or native execution'
        result=0;report['status']='PASS_HOST_CODEC_ONLY'
    except (OSError,ValueError,subprocess.TimeoutExpired) as e:
        report['error']=str(e)
    finally:
        after={name:sha(root/name) for name in names}
        report['source_unchanged']=after==before
        if not report['source_unchanged']:result=1;report['status']='FAIL_SOURCE_CHANGED'
        (out/'result.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'status':report['status'],'checks':report.get('checks'),
                      'error':report.get('error'),'hardware_support_qualified':False}))
    return result

if __name__=='__main__': raise SystemExit(main())

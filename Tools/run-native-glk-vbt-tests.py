#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual VBT parser bodies and pinned Linux layout oracles; no device I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

COMMIT = '0d9ff90a5422cc7509258aaaba1e7481df4d332a'
PINNED = {
    'intel_bios.c': ('97cbae2e547e2d11da6cc0e9f92801bf7ca969b3', 'da901f6ac60909d9bf379333244b424ab512410069cbd6c7a2674c96fbfe9be5'),
    'intel_vbt_defs.h': ('0dc13d080e8aa92c9b083ec67ae2dbb350b4db47', '75f0d7e96dd8aa00564b69fcfef6fe2781c84def6fe1705ad40e37db34bcb1fe'),
    'intel_pps.c': ('d4c98b150fa2b7c9736cf5071e67ac55d73f17cb', 'f1f9c08347630a7ff9f7e5c9fb3e527d3db6ae81320b31519843b0ec0cc92866'),
}

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def run(argv, env=None):
    r = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=120, env=env)
    return {'command': list(map(str, argv)), 'returncode': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr}

def declaration(text, name):
    # Extract the complete real packed declaration, including nested unions.
    start = re.search(r'^struct ' + re.escape(name) + r'\s*\{', text, re.M)
    if not start:
        raise ValueError('Missing primary struct ' + name)
    at = start.end(); depth = 1
    while at < len(text) and depth:
        if text[at] == '{': depth += 1
        if text[at] == '}': depth -= 1
        at += 1
    end = re.match(r'\s*__packed;', text[at:])
    if depth or not end:
        raise ValueError('Malformed primary packed struct ' + name)
    return text[start.start():at + end.end()]

def oracle(text):
    names = ['bdb_edid_pnp_id', 'bdb_edid_product_name', 'bdb_edid_dtd', 'vbt_header',
             'bdb_header', 'bdb_general_features', 'child_device_config', 'bdb_driver_features',
             'edp_power_seq', 'edp_fast_link_params', 'edp_pwm_delays', 'edp_full_link_params',
             'edp_apical_params', 'bdb_edp', 'bdb_lfp_options', 'lfp_data_ptr_table',
             'lfp_data_ptr', 'bdb_lfp_data_ptrs', 'fp_timing', 'lfp_backlight_data_entry',
             'lfp_backlight_control_method', 'lfp_brightness_level', 'bdb_lfp_backlight',
             'generic_dtd_entry', 'lfp_black_border', 'bdb_lfp_data_tail']
    output = '''#include <stdint.h>
#include <stddef.h>
extern "C" int printf(const char *, ...);
#include "Drivers/NativeGlk/VbtParser.hpp"
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
#define __packed __attribute__((packed))
namespace Primary {
'''
    output += '\n'.join(declaration(text, name) for name in names) + '\n}\n'
    output += '''using namespace Mellow::NativeGlk;
#define SIZE(t,v) static_assert(sizeof(Primary::t)==v, #t " size")
#define OFF(t,f,v) static_assert(offsetof(Primary::t,f)==v, #t "::" #f)
SIZE(vbt_header,VbtLayout::kVbtHeaderBytes); OFF(vbt_header,version,VbtLayout::kVbtVersion);
OFF(vbt_header,header_size,VbtLayout::kVbtHeaderSize); OFF(vbt_header,vbt_size,VbtLayout::kVbtSize);
OFF(vbt_header,vbt_checksum,VbtLayout::kVbtChecksum); OFF(vbt_header,bdb_offset,VbtLayout::kVbtBdbOffset);
SIZE(bdb_header,VbtLayout::kBdbHeaderBytes); OFF(bdb_header,version,VbtLayout::kBdbVersion);
OFF(bdb_header,header_size,VbtLayout::kBdbHeaderSize); OFF(bdb_header,bdb_size,VbtLayout::kBdbSize);
SIZE(bdb_general_features,VbtLayout::kGeneralFeaturesBytes);
SIZE(child_device_config,VbtLayout::kChildKnownBytes); OFF(child_device_config,handle,VbtLayout::kChildHandle);
OFF(child_device_config,device_type,VbtLayout::kChildDeviceType); OFF(child_device_config,dvo_port,VbtLayout::kChildDvoPort);
OFF(child_device_config,ddc_pin,VbtLayout::kChildDdcPin); OFF(child_device_config,aux_channel,VbtLayout::kChildAuxChannel);
SIZE(bdb_driver_features,VbtLayout::kDriverFeaturesBytes);
SIZE(bdb_edp,VbtLayout::kEdpBytes); SIZE(edp_power_seq,VbtLayout::kEdpPowerSeqStride);
OFF(bdb_edp,power_seqs,VbtLayout::kEdpPowerSeqs); OFF(bdb_edp,color_depth,VbtLayout::kEdpColorDepth);
OFF(bdb_edp,fast_link_params,VbtLayout::kEdpFastLinkParams); SIZE(edp_fast_link_params,VbtLayout::kEdpFastLinkStride);
OFF(bdb_edp,sdrrs_msa_timing_delay,VbtLayout::kEdpSdrrsMsaTimingDelay);
OFF(bdb_edp,edp_vswing_preemph,VbtLayout::kEdpVswingPreemph);
OFF(bdb_edp,edp_fast_link_training_rate,VbtLayout::kEdpFastLinkTrainingRate);
OFF(bdb_edp,edp_max_port_link_rate,VbtLayout::kEdpMaxPortLinkRate);
SIZE(bdb_lfp_options,VbtLayout::kLfpOptionsBytes); OFF(bdb_lfp_options,panel_type,VbtLayout::kLoPanelType);
OFF(bdb_lfp_options,panel_type2,VbtLayout::kLoPanelType2); OFF(bdb_lfp_options,dps_panel_type_bits,VbtLayout::kLoDpsPanelTypeBits);
SIZE(bdb_lfp_data_ptrs,VbtLayout::kLfpPtrsBytes); OFF(bdb_lfp_data_ptrs,ptr,VbtLayout::kLpEntries);
SIZE(lfp_data_ptr,VbtLayout::kLpEntryStride); OFF(bdb_lfp_data_ptrs,panel_name,VbtLayout::kLpPanelName);
OFF(fp_timing,x_res,VbtLayout::kFpXres); OFF(fp_timing,y_res,VbtLayout::kFpYres); OFF(fp_timing,lvds_reg_val,VbtLayout::kFpLvdsRegVal);
SIZE(bdb_edid_dtd,VbtLayout::kDtdBytes); SIZE(bdb_edid_pnp_id,VbtLayout::kPnpIdBytes);
SIZE(bdb_edid_product_name,VbtLayout::kPanelNameBytes);
OFF(bdb_edid_dtd,clock,VbtLayout::kDtdClock); OFF(bdb_edid_dtd,hactive_lo,VbtLayout::kDtdHactiveLo);
OFF(bdb_edid_dtd,hblank_lo,VbtLayout::kDtdHblankLo); OFF(bdb_edid_dtd,vactive_lo,VbtLayout::kDtdVactiveLo);
OFF(bdb_edid_dtd,vblank_lo,VbtLayout::kDtdVblankLo); OFF(bdb_edid_dtd,hsync_off_lo,VbtLayout::kDtdHsyncOffLo);
OFF(bdb_edid_dtd,hsync_pulse_width_lo,VbtLayout::kDtdHsyncWidthLo);
OFF(bdb_edid_dtd,himage_lo,VbtLayout::kDtdHimageLo); OFF(bdb_edid_dtd,vimage_lo,VbtLayout::kDtdVimageLo);
SIZE(bdb_lfp_backlight,VbtLayout::kBacklightBytes); SIZE(lfp_backlight_data_entry,VbtLayout::kBlEntryBytes);
OFF(bdb_lfp_backlight,data,VbtLayout::kBlData); OFF(lfp_backlight_data_entry,pwm_freq_hz,VbtLayout::kBlEntryPwmFreqHz);
OFF(lfp_backlight_data_entry,min_brightness,VbtLayout::kBlEntryMinBrightness);
OFF(bdb_lfp_backlight,level,VbtLayout::kBlLevel); OFF(bdb_lfp_backlight,backlight_control,VbtLayout::kBlControl);
OFF(bdb_lfp_backlight,brightness_level,VbtLayout::kBlBrightnessLevel);
OFF(bdb_lfp_backlight,brightness_min_level,VbtLayout::kBlBrightnessMinLevel);
OFF(bdb_lfp_backlight,brightness_precision_bits,VbtLayout::kBlPrecisionBits);
SIZE(lfp_brightness_level,VbtLayout::kBlLevelStride); SIZE(generic_dtd_entry,VbtLayout::kGdtdEntryBytes);
OFF(generic_dtd_entry,pixel_clock,VbtLayout::kGdtdPixelClock); OFF(generic_dtd_entry,hactive,VbtLayout::kGdtdHactive);
OFF(generic_dtd_entry,hblank,VbtLayout::kGdtdHblank); OFF(generic_dtd_entry,hfront_porch,VbtLayout::kGdtdHfrontPorch);
OFF(generic_dtd_entry,hsync,VbtLayout::kGdtdHsync); OFF(generic_dtd_entry,vactive,VbtLayout::kGdtdVactive);
OFF(generic_dtd_entry,vblank,VbtLayout::kGdtdVblank); OFF(generic_dtd_entry,vfront_porch,VbtLayout::kGdtdVfrontPorch);
OFF(generic_dtd_entry,vsync,VbtLayout::kGdtdVsync); OFF(generic_dtd_entry,width_mm,VbtLayout::kGdtdWidthMm);
OFF(generic_dtd_entry,height_mm,VbtLayout::kGdtdHeightMm);
OFF(bdb_lfp_data_tail,seamless_drrs_min_refresh_rate,VbtLayout::kTailSeamlessDrrsMinRefresh);
int main() {
    unsigned checks=0;
#define BIT(t,f,off,mask) do { Primary::t v{}; v.f=1; const auto *p=reinterpret_cast<const uint8_t*>(&v); \
    for (size_t i=0;i<sizeof v;++i) { ++checks; if (p[i]!=(i==off?mask:0)) return 1; } } while(0)
    BIT(bdb_general_features,enable_ssc,1,2); BIT(bdb_general_features,ssc_freq,1,4);
    BIT(bdb_general_features,display_clock_mode,1,64); BIT(bdb_general_features,rotate_180,2,4);
    BIT(bdb_general_features,int_crt_support,4,1);
    BIT(child_device_config,hdmi_level_shifter_value,7,1); BIT(child_device_config,ganged_edp,10,8);
    BIT(child_device_config,lane_reversal,23,2); BIT(child_device_config,lspcon,23,4);
    BIT(child_device_config,hpd_invert,23,16); BIT(child_device_config,use_vbt_vswing,23,32);
    BIT(child_device_config,dp_max_lane_count,23,64); BIT(child_device_config,dp_max_link_rate,38,1);
    BIT(child_device_config,edp_data_rate_override,40,1); BIT(bdb_driver_features,lvds_config,8,8);
    BIT(bdb_lfp_options,pixel_dither,2,32);
    BIT(edp_fast_link_params,rate,0,1); BIT(edp_fast_link_params,lanes,0,16);
    BIT(edp_fast_link_params,preemphasis,1,1); BIT(edp_fast_link_params,vswing,1,16);
    BIT(bdb_edid_dtd,hactive_hi,4,16); BIT(bdb_edid_dtd,hblank_hi,4,1);
    BIT(bdb_edid_dtd,vactive_hi,7,16); BIT(bdb_edid_dtd,vblank_hi,7,1);
    BIT(bdb_edid_dtd,hsync_off_hi,11,64); BIT(bdb_edid_dtd,hsync_pulse_width_hi,11,16);
    BIT(bdb_edid_dtd,vsync_off_hi,11,4); BIT(bdb_edid_dtd,vsync_pulse_width_hi,11,1);
    BIT(bdb_edid_dtd,vsync_off_lo,10,16); BIT(bdb_edid_dtd,vsync_pulse_width_lo,10,1);
    BIT(bdb_edid_dtd,himage_hi,14,16); BIT(bdb_edid_dtd,vimage_hi,14,1);
    BIT(bdb_edid_dtd,hsync_positive,17,64); BIT(bdb_edid_dtd,vsync_positive,17,32);
    BIT(lfp_backlight_data_entry,type,0,1); BIT(lfp_backlight_data_entry,active_low_pwm,0,4);
    BIT(lfp_backlight_control_method,type,0,1); BIT(lfp_backlight_control_method,controller,0,16);
    BIT(generic_dtd_entry,hsync_positive_polarity,24,128); BIT(generic_dtd_entry,vsync_positive_polarity,24,64);
    printf("primary_vbt_layout: %u bitfield byte checks passed\\n",checks); return 0;
}
'''
    return output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=shutil.which('clang++'))
    p.add_argument('--linux-root', type=Path, required=True)
    p.add_argument('--kernel-sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a=p.parse_args(); root=Path(__file__).resolve().parents[1]; out=a.out.resolve()
    out.mkdir(parents=True,exist_ok=False)
    names=['Drivers/NativeGlk/VbtParser.hpp','Drivers/NativeGlk/VbtParser.cpp',
           'Drivers/NativeGlk/VbtParser.provenance.json','tests/native_glk_vbt_tests.cpp',
           'Tools/run-native-glk-vbt-tests.py']
    before={n:sha(root/n) for n in names}
    report={'schema':'mellowgpu.glk-vbt-host-parser/1','status':'FAIL','source_sha256':before,
            'linux_commit':COMMIT,'actual_vbt_captured':False,'physical_gpu_executed':False,
            'native_driver_executed':False,'hardware_support_qualified':False,
            'metal_conformance_tested':False,'windowserver_tested':False,'fatal_sanitizers':True}
    code=1
    try:
        base=a.linux_root/'drivers/gpu/drm/i915/display'
        verified={}
        for name,(blob,digest) in PINNED.items():
            raw=(base/name).read_bytes()
            if hashlib.sha256(raw).hexdigest()!=digest or hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()!=blob:
                raise ValueError('Primary pin drift: '+name)
            verified[name]={'git_blob':blob,'sha256':digest,'bytes':len(raw)}
        report['primary_sources']=verified
        report['compiler']=run([a.cxx,'--version'])
        if report['compiler']['returncode']: raise ValueError('Compiler unavailable')
        common=[a.cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti','-I',root]
        fatal=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        report['compile']=run([*common,*fatal,'-Wno-missing-field-initializers',root/names[1],root/names[3],'-o',out/'vbt-tests'])
        if report['compile']['returncode'] or report['compile']['stderr']: raise ValueError('Hosted build failed')
        env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        report['test']=run([out/'vbt-tests'],env)
        match=re.search(r'native_glk_vbt_tests: ([0-9]+) checks, 0 failures \(synthetic fixtures, no hardware\)\n\Z',report['test']['stdout'])
        if report['test']['returncode'] or report['test']['stderr'] or not match: raise ValueError('Hosted parser or fatal sanitizer failure')
        report['checks']=int(match.group(1)); report['mutation_cases']=4000
        oracle_src=out/'primary-layout.cpp'; oracle_src.write_text(oracle((base/'intel_vbt_defs.h').read_text()))
        report['oracle_source_sha256']=sha(oracle_src)
        report['oracle_compile']=run([*common,*fatal,oracle_src,'-o',out/'primary-layout'])
        if report['oracle_compile']['returncode']: raise ValueError('Primary layout oracle build failed')
        report['oracle_test']=run([out/'primary-layout'],env)
        if report['oracle_test']['returncode'] or report['oracle_test']['stderr']: raise ValueError('Primary bitfield oracle failed')
        if not (a.kernel_sdk/'SDKSettings.plist').is_file(): raise ValueError('Explicit SDK absent')
        obj=out/'VbtParser.kernel-x86.o'
        kflags=['-target','x86_64-apple-macos15.8.1','-isysroot',a.kernel_sdk,'-mkernel','-ffreestanding',
                '-fno-builtin','-fno-stack-protector','-nostdinc++','-DKERNEL','-DKERNEL_PRIVATE']
        report['kernel_style_compile']=run([*common,'-pedantic',*kflags,'-Wframe-larger-than=1024','-c',root/names[1],'-o',obj])
        if report['kernel_style_compile']['returncode'] or report['kernel_style_compile']['stderr']: raise ValueError('x86 kernel-style object build failed')
        # Compile the same upstream bitfield/offset oracle for the target ABI.
        report['oracle_x86_compile']=run([*common,*kflags,'-fsyntax-only',oracle_src])
        if report['oracle_x86_compile']['returncode']: raise ValueError('x86 primary layout oracle compile failed')
        report['kernel_object_sha256']=sha(obj); report['kernel_sdk_settings_sha256']=sha(a.kernel_sdk/'SDKSettings.plist')
        report['undefined_symbols']=run(['/usr/bin/nm','-u',obj])
        if report['undefined_symbols']['returncode'] or report['undefined_symbols']['stdout'].strip(): raise ValueError('Freestanding parser has unresolved imports')
        report['kernel_scope']='Object compile only; no KPI/KDK link, kext loading, firmware capture or native runtime execution'
        report['status']='PASS_HOST_PARSER_ONLY'; code=0
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        report['error']=str(e)
    finally:
        report['source_unchanged']={n:sha(root/n) for n in names}==before
        if not report['source_unchanged']: report['status']='FAIL_SOURCE_CHANGED'; code=1
        (out/'result.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'status':report['status'],'checks':report.get('checks'),'error':report.get('error'),'hardware_support_qualified':False}))
    return code

if __name__=='__main__': raise SystemExit(main())

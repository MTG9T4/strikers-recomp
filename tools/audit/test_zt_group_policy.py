"""Authenticated SDK-boundary contracts, with spoofed IPv6 sources. No network/game."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid
from build_runtime import ROOT, msvc_environment

PROBE = r'''
#include "zt_group_filter.h"
#include <stdio.h>
static int check(bool v,int line){if(!v)printf("FAIL line %d\n",line);return v?0:line;}
#define CHECK(x) do {int rc=check((x),__LINE__);if(rc)return rc;} while(0)
int main(){
 const uint64_t nw=0xff61286137000000ULL,a=0x123456789aULL,b=0x23456789abULL,c=0x3456789abcULL;
 uint64_t peers[]={a,b};uint8_t frame[48]={0x60};frame[6]=17;
 uint8_t address[]={0xfc,0xc8,0x61,0x28,0x61,0x12,0x34,0x56,0x78,0x9a,0,0,0,0,0,1};
 memcpy(frame+8,address,16);
 CHECK(zts_strikers_group_peers(1,nw,peers,2)==0);
 CHECK(strikers_group_frame(nw,a,0x86dd,frame,sizeof frame));
 CHECK(!strikers_group_frame(nw,c,0x86dd,frame,sizeof frame)); // outsider spoofs enrolled IP
 CHECK(!strikers_group_frame(nw,b,0x86dd,frame,sizeof frame)); // enrolled sender spoofs another
 CHECK(!strikers_group_frame(nw+1,a,0x86dd,frame,sizeof frame));
 CHECK(!strikers_group_frame(nw,a,0x800,frame,sizeof frame));
 CHECK(!strikers_group_frame(nw,a,0x86dd,frame,39));
 /* Audit 08: neighbour discovery may only speak for its authenticated sender. */
 uint8_t nd[72]={0x60};nd[6]=58;memcpy(nd+8,address,16);memcpy(nd+48,address,16);
 uint8_t other[16];memcpy(other,address,16);other[5]=0x23;other[6]=0x45;other[7]=0x67;other[8]=0x89;other[9]=0xab;
 uint8_t local[16]={0xfe,0x80,0,0,0,0,0,0,0x22,0x33,0x44,0xff,0xfe,0x55,0x66,0x77};
 nd[40]=136;CHECK(strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));            // NA for itself, from its 6plane address
 CHECK(!strikers_group_frame(nw,c,0x86dd,nd,sizeof nd));                      // outsider
 CHECK(!strikers_group_frame(nw,b,0x86dd,nd,sizeof nd));                      // member claims another member's source
 memcpy(nd+8,other,16);CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));
 memcpy(nd+8,address,16);memcpy(nd+48,other,16);
 CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));                      // NA naming a victim: lwIP marks it duplicated
 memcpy(nd+8,local,16);CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));
 nd[40]=135;CHECK(strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));            // NUD probe from link-local
 memcpy(nd+8,address,16);CHECK(strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));
 memcpy(nd+8,other,16);CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));// NS rewriting another member's neighbour entry
 memset(nd+8,0,16);CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));    // unspecified source: DAD never crosses
 memcpy(nd+8,address,16);
 for(int type:{1,2,128,129,133,134,137,143}){nd[40]=(uint8_t)type;CHECK(!strikers_group_frame(nw,a,0x86dd,nd,sizeof nd));}
 nd[40]=135;CHECK(!strikers_group_frame(nw,a,0x86dd,nd,63));                  // shorter than an NS
 frame[6]=6;CHECK(!strikers_group_frame(nw,a,0x86dd,frame,sizeof frame));     // no TCP in ad-hoc
 frame[6]=44;CHECK(!strikers_group_frame(nw,a,0x86dd,frame,sizeof frame));frame[6]=17;
 CHECK(zts_strikers_group_peers(1,nw,peers+1,1)==0);
 CHECK(!strikers_group_frame(nw,a,0x86dd,frame,sizeof frame));
 CHECK(zts_strikers_group_peers(1,nw,NULL,129)==-1);
 CHECK(!strikers_group_frame(nw,a,0x86dd,frame,sizeof frame));
 CHECK(zts_strikers_group_peers(1,nw,NULL,0)==0);
 CHECK(!strikers_group_frame(nw,b,0x86dd,frame,sizeof frame));
 CHECK(zts_strikers_group_peers(1,0xaabbccddeeff0011ULL,peers,2)==0);
 CHECK(strikers_group_frame(0xaabbccddeeff0011ULL,b,0x800,frame,sizeof frame));
 CHECK(!strikers_group_frame(0xaabbccddeeff0011ULL,c,0x800,frame,sizeof frame));
 CHECK(zts_strikers_group_peers(0,0,NULL,0)==0);
 CHECK(strikers_group_frame(nw,c,0x86dd,frame,sizeof frame)); // manual mode unchanged
 puts("PASS authenticated source, spoofed IP, neighbour discovery, wrong network, removal, empty list, Central and manual mode");return 0;
}
'''


def main():
    folder=Path(tempfile.gettempdir())/('strikers-group-policy-'+uuid.uuid4().hex);folder.mkdir()
    try:
        env=msvc_environment();source=folder/'policy.cpp';source.write_text(PROBE)
        exe=folder/'policy.exe'
        r=subprocess.run([str(Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'),'/nologo','/MD','/O2','/EHsc','/std:c++17','/I'+str(ROOT/'runtime/services'),str(source),'/Fe:'+str(exe)],cwd=folder,env=env,capture_output=True,text=True)
        assert r.returncode==0,r.stdout+r.stderr
        subprocess.run([str(exe)],check=True)
    finally:
        assert folder.resolve().parent==Path(tempfile.gettempdir()).resolve();shutil.rmtree(folder)


if __name__=='__main__':main()

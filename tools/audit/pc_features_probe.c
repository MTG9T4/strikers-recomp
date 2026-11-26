/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the production parser, bindings, input quarantine and fit geometry. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static short keys[256];
static short test_key_state(int k) { return keys[k]; }
#define PC_KEY_STATE(k) test_key_state(k)
int nnpad_kb_vk(int idx) { (void)idx; return 0; }
#include "gfx_options.c"
#include "pc_geometry.h"
static int bad;
#define CHECK(x) do {if(!(x)) {printf("FAIL %d: %s\n",__LINE__,#x);bad++;}} while(0)
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    _putenv_s("STRIKERS_PC_CONFIG",argv[1]);
    FILE *f=fopen(argv[1],"wb");if(!f)return 2;
    fputs("window=windowed\nlanguage = PT-br\nresolution_scale=2\nsharpness=nan\nframe_cap=144\nkey.A = Z, SPACE, 0x4d\nkey.STICK_L=SHIFT\nkey.RSTICK_RIGHT=NUM6\n[Network]\nmode = zerotier\nzerotier_network = ff61286137000000\ngroup_service = https://group.example\ngroup_code = ABCD-2345\n",f);fclose(f);
    GfxOptions o;pc_settings_get(&o);CHECK(o.scale==2);CHECK(o.frame_cap==60);CHECK(o.sharpness==0.2f);
    CHECK(!strcmp(o.language,"pt-BR"));CHECK(pc_language("zh-hant")==&pc_languages[13]);CHECK(!pc_language("auto"));CHECK(!pc_language(""));
    CHECK(!strcmp(o.network_mode,"zerotier"));CHECK(!strcmp(o.zerotier_network,"ff61286137000000"));
    CHECK(!strcmp(o.group_service,"https://group.example"));CHECK(!strcmp(o.group_code,"ABCD-2345"));
    CHECK(o.keys[0][0]=='Z' && o.keys[0][1]==VK_SPACE && o.keys[0][2]=='M');
    CHECK(pc_parse_key("NUM8")==VK_NUMPAD8);CHECK(pc_parse_key("bogus")==0);
    CHECK(pc_parse_key("0x101")==0);CHECK(pc_parse_key("CTRL")==VK_CONTROL);
    keys[VK_SPACE]=(short)0x8000;CHECK(pc_key_down(0,'Z'));CHECK(!pc_key_down(1,'X'));
    pc_settings_toggle();CHECK(pc_settings_open());CHECK(!pc_key_down(0,'Z'));
    CHECK(pc_input_gate(0,1,32000,0,0,0));pc_settings_toggle();CHECK(!pc_settings_open());CHECK(pc_input_gate(0,1,0,0,0,0));
    CHECK(!pc_key_down(0,'Z'));keys[VK_SPACE]=0;CHECK(!pc_key_down(0,'Z'));keys[VK_SPACE]=(short)0x8000;CHECK(pc_key_down(0,'Z'));keys[VK_SPACE]=0;
    CHECK(!pc_input_gate(0,0,0,0,0,0));CHECK(!pc_input_gate(1,0,0,0,0,0));
    CHECK(pc_input_gate(2,(1ull<<10)|(1ull<<11),0,0,0,0));CHECK(pc_settings_open());
    CHECK(pc_input_gate(2,(1ull<<10)|(1ull<<11),0,0,0,0));CHECK(pc_settings_open());
    pc_input_gate(2,0,0,0,0,0);pc_input_gate(2,(1ull<<10)|(1ull<<11),0,0,0,0);CHECK(!pc_settings_open());CHECK(pc_input_gate(2,1,0,0,0,0));CHECK(!pc_input_gate(2,0,0,0,0,0));
    CHECK(pc_screen_class(1920,1080));CHECK(pc_screen_class(1723,969));CHECK(pc_screen_class(1723,970));
    CHECK(pc_screen_class(1600,900));CHECK(pc_screen_class(120,67));CHECK(pc_screen_class(60,33));
    CHECK(!pc_screen_class(2048,2048));CHECK(!pc_screen_class(1064,122));CHECK(!pc_screen_class(1000,58));
    for(int h=180;h<=1080;h++)CHECK(pc_screen_class((h*16+4)/9,h));
    int x=0,w=INT_MAX;pc_scale_axis(&x,&w,2,3840);CHECK(x==0 && w==3840);
    x=0;w=INT_MAX;pc_scale_axis(&x,&w,1.5f,2880);CHECK(x==0 && w==2880);
    x=128;w=INT_MAX;pc_scale_axis(&x,&w,2,3840);CHECK(x==256 && w==3584);
    x=-10;w=20;pc_scale_axis(&x,&w,2,3840);CHECK(x==0 && w==20);
    x=INT_MAX;w=INT_MAX;pc_scale_axis(&x,&w,2,3840);CHECK(x==3840 && w==0);
    int d[4];pc_fit_rect(1920,1080,1280,720,0,d);CHECK(d[0]==0 && d[1]==0 && d[2]==1280 && d[3]==720);
    pc_fit_rect(1920,1080,3440,1440,0,d);CHECK(d[0]==440 && d[1]==0 && d[2]==2560 && d[3]==1440);
    pc_fit_rect(1920,1080,1280,1024,0,d);CHECK(d[0]==0 && d[1]==152 && d[2]==1280 && d[3]==720);
    pc_fit_rect(960,540,3840,2160,1,d);CHECK(d[2]==3840 && d[3]==2160);
    pc_fit_rect(960,540,2560,1440,1,d);CHECK(d[0]==320 && d[1]==180 && d[2]==1920 && d[3]==1080);
    pc_fit_rect(1920,1080,720,1280,1,d);CHECK(d[2]==720 && d[3]==405 && d[1]==437);
    for(int ww=320;ww<4000;ww+=77)for(int wh=180;wh<2500;wh+=81) {pc_fit_rect(1920,1080,ww,wh,0,d);CHECK(d[0]>=0 && d[1]>=0 && d[0]+d[2]<=ww && d[1]+d[3]<=wh);}
    o.keys_set=0xffffff;CHECK(gfx_options_save(&o));f=fopen(argv[1],"rb");CHECK(f!=NULL);char data[8192]={0};if(f){fread(data,1,sizeof data-1,f);fclose(f);}CHECK(strstr(data,"resolution_scale = 2"));CHECK(strstr(data,"key.A = 0x5A, 0x20, 0x4D"));
    CHECK(strstr(data,"\nlanguage = pt-BR\n") && strstr(data,"\nlanguage = pt-BR\n")<strstr(data,"[Network]"));
    CHECK(strstr(data,"[Network]\nmode = zerotier\nroom_key = \nrelay = \nzerotier_network = ff61286137000000\n"));
    CHECK(strstr(data,"group_service = https://group.example\ngroup_code = ABCD-2345\n"));
    o.language[0]=0;CHECK(gfx_options_save(&o));f=fopen(argv[1],"rb");memset(data,0,sizeof data);if(f){fread(data,1,sizeof data-1,f);fclose(f);}CHECK(strstr(data,"\nlanguage = auto\n"));
    printf("PC feature contracts: %d failed\n",bad);return bad!=0;
}

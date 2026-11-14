/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_GEOMETRY_H
#define PC_GEOMETRY_H
#include <math.h>
/* Dynamic scene sizes and the bloom pyramid allow one pixel of 16:9
 * rounding. Square shadows and irregular UI attachments keep their grid. */
static int pc_screen_class(int w,int h) {
    long long delta=(long long)w*9-(long long)h*16;
    return w>=40 && h>=22 && delta>=-16 && delta<=16;
}
/* NVN commonly sets an unrestricted scissor with INT_MAX extents. Sum and
 * scale in double, then clamp to the attachment BEFORE converting to int. */
static void pc_scale_axis(int *origin,int *count,float scale,int limit) {
    double lo=floor((double)*origin*scale);
    double hi=ceil(((double)*origin+(double)*count)*scale-0.001);
    if(lo<0)lo=0;if(lo>limit)lo=limit;
    if(hi<lo)hi=lo;if(hi>limit)hi=limit;
    *origin=(int)lo;*count=(int)(hi-lo);
}
/* Integer fit never crops. Below 1:1 use nearest fractional fit. Edge-aligned
 * rectangles preserve the texel-centre mapping of Vulkan's unmodified 1x blit. */
static void pc_fit_rect(int sw,int sh,int ww,int wh,int integer,int out[4]) {
    if(sw<=0||sh<=0||ww<=0||wh<=0) {out[0]=out[1]=out[2]=out[3]=0;return;}
    double s=(double)ww/sw,sy=(double)wh/sh;if(sy<s)s=sy;
    if(integer && s>=1)s=(int)s;
    int w=(int)(sw*s+0.5),h=(int)(sh*s+0.5);
    if(w>ww)w=ww;if(h>wh)h=wh;if(w<1)w=1;if(h<1)h=1;
    out[0]=(ww-w)/2;out[1]=(wh-h)/2;out[2]=w;out[3]=h;
}
#endif

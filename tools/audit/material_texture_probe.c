/* GPL-3.0-or-later. Inspect the precise static resources saved by material trace. */
#include "nvn_front.h"
#include "tex_decode.h"
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    if(argc!=2)return 2;char name[1024];int meta[12];TexBuilder b;EView e;SamplerParams sp;
    snprintf(name,sizeof name,"%s.meta",argv[1]);FILE *f=fopen(name,"rb");if(!f)return 2;
    if(fread(meta,1,sizeof meta,f)!=sizeof meta || fread(&b,1,sizeof b,f)!=sizeof b || fread(&e,1,sizeof e,f)!=sizeof e || fread(&sp,1,sizeof sp,f)!=sizeof sp)return 2;fclose(f);
    printf("%s format %x Vk %d %dx%dx%d %d layers %d levels | view %d levels %d+%d layers %d+%d swz %d %d %d %d | sampler min/mag %d/%d wrap %d %d %d compare %d/%d lod %g..%g bias %g border %g %g %g %g\n",argv[1],b.format,meta[1],b.width,b.height,b.depth,meta[5],meta[6],e.target,e.lvl0,e.nlvl,e.lay0,e.nlay,e.swz[0],e.swz[1],e.swz[2],e.swz[3],sp.min,sp.mag,sp.ws,sp.wt,sp.wr,sp.cmode,sp.cfunc,sp.lodmin,sp.lodmax,sp.bias,sp.border[0],sp.border[1],sp.border[2],sp.border[3]);
    TexLayout l;tex_layout(&l,b.target,b.format,b.width,b.height,b.depth,b.levels,b.flags&~0x50,0,b.has_layout&&b.layout[0]<=4?b.layout[0]:-1);
    snprintf(name,sizeof name,"%s.swizzled",argv[1]);f=fopen(name,"rb");if(!f)return 0;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);uint8_t *raw=malloc(n);fread(raw,1,n,f);fclose(f);
    const TexFormat *fmt=tex_format(b.format);
    for(int layer=0;layer<l.layers;layer++)for(int level=0;level<l.levels;level++) {
        int w=l.lvl_w[level],h=l.lvl_h[level],d=l.lvl_d[level];size_t bytes=l.lvl_linear_size[level];
        if((size_t)layer*l.layer_stride+l.lvl_offset[level]+l.lvl_size[level]>(size_t)n){printf("snapshot shorter than computed level\n");return 2;}
        uint8_t *linear=malloc(bytes);tex_deswizzle_level(&l,level,tex_level_ptr(&l,raw,level,layer),linear);
        snprintf(name,sizeof name,"%s_l%d_m%d.linear",argv[1],layer,level);f=fopen(name,"wb");if(f){fwrite(linear,1,bytes,f);fclose(f);}
        if(!level) {
            uint8_t *rgba=malloc((size_t)w*h*d*4);
            int ok=fmt->compressed?tex_decode_rgba8(b.format,linear,w,h*d,rgba):tex_convert_rgba8(b.format,linear,w,h*d,rgba);
            if(!ok){snprintf(name,sizeof name,"%s_l%d.png",argv[1],layer);tex_write_png(name,rgba,w,h*d);double mean[4]={0};for(int k=0;k<w*h*d;k++)for(int c=0;c<4;c++)mean[c]+=rgba[4*k+c];printf(" layer %d mean RGBA %.3f %.3f %.3f %.3f\n",layer,mean[0]/(w*h*d),mean[1]/(w*h*d),mean[2]/(w*h*d),mean[3]/(w*h*d));}free(rgba);
        }
        free(linear);
    }
    free(raw);return 0;
}

/* GPL-3.0-or-later. Translate captured packages with production parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_translate.h"
static unsigned u32(const unsigned char *p) { unsigned v; memcpy(&v,p,4); return v; }
int main(int argc, char **argv) {
    int bad = 0;
    for (int k = 1; k < argc; ++k) {
        FILE *f = fopen(argv[k], "rb"); unsigned h[5];
        if (!f || fread(h,1,20,f)!=20 || h[0]!=0x3144584d) return 2;
        unsigned char *ctl=malloc(h[3]), *data=malloc(h[4]);
        if (fread(ctl,1,h[3],f)!=h[3] || fread(data,1,h[4],f)!=h[4]) return 2;
        fclose(f);
        MxIn in={0}; MxOut out;
        int stages[6]={-1,0,3,4,2,1};
        in.stage = u32(ctl+0x714)==5 ? 5 : stages[(u32(data+0x30)>>10)&15];
        in.target=1;
        in.code=data+(in.stage==5 ? u32(ctl+0x708) : 0x30);
        in.code_len=u32(ctl+0x6f8);
        if (in.stage==5) {
            memcpy(in.local_size,ctl+0x748,12); in.smem_bytes=u32(ctl+0x754); in.lmem_bytes=u32(ctl+0x758);
        }
        if (u32(ctl+0x6fc)) { in.cdata=data+u32(ctl+0x700); in.cdata_size=u32(ctl+0x6fc); }
        int ok=mx_translate(&in,&out);
        printf("%s stage %d: %s\n",argv[k],in.stage,ok ? "PASS" : out.why);
        char name[1024]; snprintf(name,sizeof name,"%s.glsl",argv[k]);
        if (ok) { f=fopen(name,"w"); fputs(out.glsl,f); fclose(f); } else bad++;
        free(out.glsl); free(ctl); free(data);
    }
    return bad ? 1 : 0;
}

/* GPL-3.0-or-later. Inspect captured Maxwell code without modifying the package. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_translate.h"
static unsigned u32(const unsigned char *p) { unsigned v; memcpy(&v,p,4); return v; }
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    unsigned char *raw=malloc(len); if (fread(raw,1,len,f)!=(size_t)len) return 2; fclose(f);
    MxIn in={0}; MxOut out;
    if (u32(raw)==0x3144584d) {
        unsigned char *ctl=raw+20, *data=ctl+u32(raw+12);
        int stages[6]={-1,0,3,4,2,1};
        in.stage=u32(ctl+0x714)==5?5:stages[(u32(data+0x30)>>10)&15];
        in.code=data+(in.stage==5?u32(ctl+0x708):0x30); in.code_len=u32(ctl+0x6f8);
        if (in.stage==5) { memcpy(in.local_size,ctl+0x748,12); in.smem_bytes=u32(ctl+0x754); in.lmem_bytes=u32(ctl+0x758); }
        if (u32(ctl+0x6fc)) { in.cdata=data+u32(ctl+0x700); in.cdata_size=u32(ctl+0x6fc); }
    } else {
        in.stage=atoi(argv[2]); in.code=raw; in.code_len=(int)len;
    }
    in.target=1; if (getenv("MX_INT_TEX")) in.int_tex=(unsigned)strtoul(getenv("MX_INT_TEX"),NULL,0);
    if (getenv("MX_INT_ATTRS")) in.int_attrs=(unsigned)strtoul(getenv("MX_INT_ATTRS"),NULL,0);
    if (getenv("MX_SINT_ATTRS")) in.sint_attrs=(unsigned)strtoul(getenv("MX_SINT_ATTRS"),NULL,0);
    if (getenv("MX_SINT_TEX")) in.sint_tex=(unsigned)strtoul(getenv("MX_SINT_TEX"),NULL,0);
    char name[1024]; snprintf(name,sizeof name,"%s.asm",argv[1]); f=fopen(name,"w");
    unsigned hdr=in.stage==5?0:0x50;
    for(unsigned pc=1;pc<(in.code_len-hdr)/8;pc++) {
        if(!(pc&3)) continue;
        unsigned long long i;memcpy(&i,in.code+hdr+pc*8,8);
        fprintf(f,"%04u %016llx %-16s dst %u a %u b %u c %u pred %u%s\n",pc,i,mx_opcode_name(i),
            (unsigned)(i&255),(unsigned)(i>>8&255),(unsigned)(i>>20&255),(unsigned)(i>>39&255),(unsigned)(i>>16&7),i>>19&1?"!":"");
    }
    fclose(f);
    int ok=mx_translate(&in,&out);
    printf("%s stage %d: %s (%d instructions, cbufs %x, textures %x, images %x)\n",argv[1],in.stage,ok?"PASS":out.why,out.insns,out.cbufs,out.tex_units,out.images);
    snprintf(name,sizeof name,"%s.glsl",argv[1]);
    if(ok) { f=fopen(name,"w");fputs(out.glsl,f);fclose(f); }
    free(out.glsl);free(raw);return ok?0:1;
}

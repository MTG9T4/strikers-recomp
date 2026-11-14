/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original lifted SDK zlib; Python zlib independently inflates each result. */
__declspec(dllexport) unsigned char job15_zip_input[4096];
__declspec(dllexport) unsigned char job15_zip_output[5][8192];
__declspec(dllexport) uint64_t job15_zip_sizes[5];
/* The real game supplies these application imports; this fixture has no game
 * image/allocator, so supply ordinary test allocations at that boundary. */
static void zip_malloc(Ctx *c) {c->x[0]=(uint64_t)(uintptr_t)malloc((size_t)c->x[0]);}
static void zip_free(Ctx *c) {free((void *)(uintptr_t)c->x[0]);}
static void zip_refer_symbol(Ctx *c) {(void)c;}
static int job15_zip_tests(Ctx *c) {
    const int levels[4]={0,1,6,9};
    sdk_host_override("malloc",zip_malloc);sdk_host_override("free",zip_free);
    sdk_host_override("_ZN2nn4util11ReferSymbolEPKv",zip_refer_symbol);
    for(unsigned i=0;i<sizeof job15_zip_input;i++)job15_zip_input[i]=(unsigned char)(i%127<96?'A'+i%19:i);
    for(int i=0;i<4;i++) {
        uint8_t stream[0x70]={0};uint64_t z=(uint64_t)(uintptr_t)stream;
        wr64(z,(uint64_t)(uintptr_t)job15_zip_input);wr32(z+8,sizeof job15_zip_input);
        wr64(z+24,(uint64_t)(uintptr_t)job15_zip_output[i]);wr32(z+32,8192);
        c->x[0]=z;c->x[1]=levels[i];c->x[2]=8;c->x[3]=15;c->x[4]=8;c->x[5]=0;
        c->x[6]=(uint64_t)(uintptr_t)"1.2.11";c->x[7]=0x70;
        sdk_dispatch(c,sdk_symbol("nnutilZlib_deflateInit2_"));REQUIRE((int)c->x[0]==0);
        c->x[0]=z;c->x[1]=4;sdk_dispatch(c,sdk_symbol("nnutilZlib_deflate"));
        REQUIRE((int)c->x[0]==1 && rd64(z+16)==sizeof job15_zip_input && rd32(z+8)==0);
        job15_zip_sizes[i]=rd64(z+40);REQUIRE(job15_zip_sizes[i]>0&&job15_zip_sizes[i]<8192);
        c->x[0]=z;sdk_dispatch(c,sdk_symbol("nnutilZlib_deflateEnd"));REQUIRE((int)c->x[0]==0);
    }
    /* Pia uses this wrapper and its workspace-backed allocator callbacks. */
    unsigned char *work=malloc(524288);REQUIRE(work);
    c->x[0]=(uint64_t)(uintptr_t)&job15_zip_sizes[4];c->x[1]=(uint64_t)(uintptr_t)job15_zip_output[4];c->x[2]=8192;
    c->x[3]=(uint64_t)(uintptr_t)job15_zip_input;c->x[4]=sizeof job15_zip_input;c->x[5]=(uint64_t)(uintptr_t)work;c->x[6]=524288;
    sdk_dispatch(c,sdk_symbol("_ZN2nn4util15CompressDeflateEPmPvmPKvmS2_m"));
    free(work);REQUIRE(c->x[0]==1 && job15_zip_sizes[4]>0 && job15_zip_sizes[4]<8192);
    return 0;
}

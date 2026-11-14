/* SPDX-License-Identifier: GPL-3.0-or-later
 * Audit build ONLY (-DSTRIKERS_PC_SCALE_TEST), absent from the default host.
 * Use the game's ordinary Pause, then rerender its frozen scene at each size.
 * No guest-state edits: only replace host images at a finished frame boundary.
 * Start with STRIKERS_VK_SCALE=1.000001: dimensions round to native, while all
 * fragment stages already use the production resolution-aware translation.
 * Keeping RS_ON true avoids changing shader/library classes during this test.
 */
static volatile LONG pc_scale_requested;
void pc_scale_test_request(int percent) {
    if(percent==50 || percent==100 || percent==150 || percent==200)
        InterlockedExchange(&pc_scale_requested,percent);
}
static void pc_scale_test_update(void) {
    int percent=(int)InterlockedExchange(&pc_scale_requested,0);
    if(!percent)return;
    if(!RS_ON)vkd_fail("scale comparison must start at STRIKERS_VK_SCALE=1.000001");
    pass_end();cmd_wait_all();
    VTex **images=NULL;int n=0;
    /* Only the frozen view's working set. Boot retains hundreds of obsolete
     * dynamic-size depth textures; scaling those serves no draw and exhausts
     * VRAM. This audit build is deliberately not a general live-scale mode. */
    for(VTex *v=g_vtex_all;v;v=v->next)if(v->rs && v->t && !v->t->dead && v->t->vk==v &&
        v->t->last_use>=g_cur_frame-4) {
        images=realloc(images,(size_t)(n+1)*sizeof *images);images[n++]=v;
    }
    g_rs=percent==100?1.000001f:percent/100.0f;
    for(int i=0;i<n;i++) {
        VTex *old=images[i];Tex *t=old->t;
        if(old->samples!=1 || old->layers!=1 || old->levels!=1 ||
           !fmt_supported(old->fmt,VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT))
            vkd_fail("scale comparison unsupported host image shape/format");
        t->vk=NULL;
        VTex *fresh=vt_get2(t,0);
        if(old->layout!=VK_IMAGE_LAYOUT_UNDEFINED) {
            vt_layout(old,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vt_layout(fresh,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit b;memset(&b,0,sizeof b);
            b.srcSubresource.aspectMask=old->aspect;b.srcSubresource.layerCount=1;
            b.dstSubresource=b.srcSubresource;
            b.srcOffsets[1].x=old->w;b.srcOffsets[1].y=old->h;b.srcOffsets[1].z=1;
            b.dstOffsets[1].x=fresh->w;b.dstOffsets[1].y=fresh->h;b.dstOffsets[1].z=1;
            vkCmdBlitImage(R.cmd,old->img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           fresh->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,rs_filter(old));
        }
        cmd_wait_all();vt_destroy(old);
    }
    free(images);
    R.dyn_dirty=1;R.pipe_bound=NULL;g_dyn_inval=1;
    printf("pc-scale-test: host images replaced at %.2fx (%d targets); next frames rerender normally\n",percent/100.0,n);
}

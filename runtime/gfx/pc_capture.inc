/* SPDX-License-Identifier: GPL-3.0-or-later
 * Opt-in validation evidence: final swapchain bytes after FSR/settings. Does
 * not run or request transfer-source swapchain usage without STRIKERS_PC_SHOT. */
static struct { VkBuffer buffer; VMem mem; int w,h,bgr,raw; long long frame; } pc_capture;
static volatile LONG pc_capture_requested;
void pc_capture_request(void) {InterlockedExchange(&pc_capture_requested,1);}
static int pc_capture_frame(long long frame) {
    if(InterlockedExchange(&pc_capture_requested,0))return 1;
    const char *p=getenv("STRIKERS_PC_SHOT");
    while(p && *p) {char *end;long long n=_strtoi64(p,&end,10);if(end==p)break;if(n==frame)return 1;p=*end==','?end+1:NULL;}
    return 0;
}
static void pc_capture_record(VkImage image,long long frame,VkImageLayout layout) {
    if(!V.capture || !pc_capture_frame(frame))return;
    pc_capture.w=V.extent.width;pc_capture.h=V.extent.height;pc_capture.frame=frame;
    pc_capture.bgr=V.format.format==VK_FORMAT_B8G8R8A8_UNORM||V.format.format==VK_FORMAT_B8G8R8A8_SRGB;
    pc_capture.raw=V.hdr?(int)V.hdr_fmt.format:0;             /* HDR: the 10-bit words as they are (pc_<frame>.raw) */
    pc_capture.buffer=vbuf_new((uint64_t)pc_capture.w*pc_capture.h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&pc_capture.mem);
    image_barrier(image,VK_IMAGE_ASPECT_COLOR_BIT,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy;memset(&copy,0,sizeof copy);copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;copy.imageExtent.width=pc_capture.w;copy.imageExtent.height=pc_capture.h;copy.imageExtent.depth=1;
    vkCmdCopyImageToBuffer(R.cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pc_capture.buffer,1,&copy);
    image_barrier(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout);
}
static void pc_capture_write(void) {
    if(!pc_capture.buffer)return;
    cmd_wait_all();
    char path[512];const char *dir=getenv("STRIKERS_SHOT_DIR");
    if(pc_capture.raw) {
        snprintf(path,sizeof path,"%s/pc_%lld.raw",dir?dir:".",pc_capture.frame);
        FILE *rf=fopen(path,"wb");if(rf){fwrite(pc_capture.mem.map,4,(size_t)pc_capture.w*pc_capture.h,rf);fclose(rf);}
        printf("pc-capture: %s (%dx%d, 32-bit words, Vulkan format %d)\n",path,pc_capture.w,pc_capture.h,pc_capture.raw);
        vkDestroyBuffer(V.device,pc_capture.buffer,NULL);vmem_free(pc_capture.mem);memset(&pc_capture,0,sizeof pc_capture);
        return;
    }
    snprintf(path,sizeof path,"%s/pc_%lld.bmp",dir?dir:".",pc_capture.frame);
    FILE *f=fopen(path,"wb");
    if(f) {
        int w=pc_capture.w,h=pc_capture.h;unsigned row=(w*3+3)&~3u,size=54+row*h;unsigned char head[54]={'B','M'};
        memcpy(head+2,&size,4);unsigned offset=54,header=40;memcpy(head+10,&offset,4);memcpy(head+14,&header,4);memcpy(head+18,&w,4);memcpy(head+22,&h,4);head[26]=1;head[28]=24;fwrite(head,1,54,f);
        unsigned char *line=calloc(1,row),*pixels=pc_capture.mem.map;
        for(int y=h-1;y>=0;y--) {for(int x=0;x<w;x++) {size_t s=((size_t)y*w+x)*4;line[3*x]=pixels[s+(pc_capture.bgr?0:2)];line[3*x+1]=pixels[s+1];line[3*x+2]=pixels[s+(pc_capture.bgr?2:0)];}fwrite(line,1,row,f);}free(line);fclose(f);
        printf("pc-capture: %s (%dx%d)\n",path,w,h);
    }
    vkDestroyBuffer(V.device,pc_capture.buffer,NULL);vmem_free(pc_capture.mem);memset(&pc_capture,0,sizeof pc_capture);
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Two-pass FSR 1.0.2. AMD shader text is embedded by build_exe.py from the
 * pinned, MIT-licensed files in fsr/. No shader/network dependency at runtime.
 * Input/output values are display-encoded: use an UNORM view of sRGB input,
 * then byte-copy the final image to the swapchain (no second sRGB encode). */
#include "fsr_sources.h"
typedef struct { VkImage img; VkImageView view; VMem mem; VkImageLayout layout; } FsrImage;
static struct { VkPipeline pipe[2]; VkPipelineLayout layout; VkDescriptorSetLayout dl;
    VkDescriptorPool pool; VkDescriptorSet sets[NSLOT][2]; VkSampler sampler;
    FsrImage image[2]; int w,h; LONG64 frames; } FSR;

static void fsr_drop_images(void) {
    for(int i=0;i<2;i++) {FsrImage *t=&FSR.image[i];if(t->view)vkDestroyImageView(V.device,t->view,NULL);if(t->img)vkDestroyImage(V.device,t->img,NULL);if(t->mem.mem)vmem_free(t->mem);memset(t,0,sizeof *t);}
    FSR.w=FSR.h=0;
}
static void fsr_shutdown(void) {
    fsr_drop_images();
    for(int i=0;i<2;i++)if(FSR.pipe[i])vkDestroyPipeline(V.device,FSR.pipe[i],NULL);
    if(FSR.layout)vkDestroyPipelineLayout(V.device,FSR.layout,NULL);
    if(FSR.pool)vkDestroyDescriptorPool(V.device,FSR.pool,NULL);
    if(FSR.dl)vkDestroyDescriptorSetLayout(V.device,FSR.dl,NULL);
    if(FSR.sampler)vkDestroySampler(V.device,FSR.sampler,NULL);
    printf("pc-fsr: %lld EASU/RCAS frames\n",(long long)FSR.frames);
    memset(&FSR,0,sizeof FSR);
}
static void fsr_init(void) {
    if(FSR.layout)return;
    VkDescriptorSetLayoutBinding b[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=2;di.pBindings=b;
    VKC(vkCreateDescriptorSetLayout(V.device,&di,NULL,&FSR.dl));
    VkPushConstantRange pc={VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&FSR.dl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&pc;
    VKC(vkCreatePipelineLayout(V.device,&li,NULL,&FSR.layout));
    VkDescriptorPoolSize sz[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,NSLOT*2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,NSLOT*2}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.poolSizeCount=2;pi.pPoolSizes=sz;pi.maxSets=NSLOT*2;
    VKC(vkCreateDescriptorPool(V.device,&pi,NULL,&FSR.pool));
    for(int i=0;i<NSLOT;i++)for(int j=0;j<2;j++) {VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=FSR.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&FSR.dl;VKC(vkAllocateDescriptorSets(V.device,&ai,&FSR.sets[i][j]));}
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
    VKC(vkCreateSampler(V.device,&si,NULL,&FSR.sampler));
    const char *common="\nlayout(set=0,binding=0) uniform sampler2D sourceImage;\nlayout(rgba8,set=0,binding=1) writeonly uniform image2D outputImage;\nlayout(push_constant) uniform Pc { vec4 src; vec4 dst; };\nlayout(local_size_x=8,local_size_y=8) in;\n";
    const char *callbacks[2]={
        "\nAF4 FsrEasuRF(AF2 p){return textureGather(sourceImage,p,0);}\nAF4 FsrEasuGF(AF2 p){return textureGather(sourceImage,p,1);}\nAF4 FsrEasuBF(AF2 p){return textureGather(sourceImage,p,2);}\n",
        "\nAF4 FsrRcasLoadF(ASU2 p){return texelFetch(sourceImage,clamp(p,ivec2(0),textureSize(sourceImage,0)-1),0);}\nvoid FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}\n"};
    const char *mains[2]={
        "\nvoid main(){uvec2 p=gl_GlobalInvocationID.xy;if(any(greaterThanEqual(p,uvec2(dst.xy))))return;vec3 c;uvec4 c0,c1,c2,c3;FsrEasuConOffset(c0,c1,c2,c3,src.x,src.y,float(textureSize(sourceImage,0).x),float(textureSize(sourceImage,0).y),dst.x,dst.y,src.z,src.w);if(src.x<=dst.x && src.y<=dst.y)FsrEasuF(c,p,c0,c1,c2,c3);else c=textureLod(sourceImage,((vec2(p)+0.5)*src.xy/dst.xy+src.zw)/vec2(textureSize(sourceImage,0)),0).rgb;imageStore(outputImage,ivec2(p),vec4(c,1));}\n",
        "\nvoid main(){uvec2 p=gl_GlobalInvocationID.xy;if(any(greaterThanEqual(p,uvec2(dst.xy))))return;uvec4 con;FsrRcasCon(con,2.0*(1.0-dst.z));vec3 c;FsrRcasF(c.r,c.g,c.b,p,con);if(dst.w>0.5)c=c.bgr;imageStore(outputImage,ivec2(p),vec4(c,1));}\n"};
    for(int i=0;i<2;i++) {
        size_t cap=sizeof fsr_a+sizeof fsr_1+strlen(common)+strlen(callbacks[i])+strlen(mains[i])+512;
        char *source=malloc(cap);if(!source)vkd_fail("FSR source allocation");
        snprintf(source,cap,"#version 450\n#define A_GPU 1\n#define A_GLSL 1\n#define %s 1\n%s\n%s\n%s\n%s\n%s",i?"FSR_RCAS_F":"FSR_EASU_F",fsr_a,common,fsr_1,callbacks[i],mains[i]);
        uint64_t hash;VkShaderModule mod=module_from_glsl(5,source,i?"FSR1 RCAS":"FSR1 EASU",&hash,NULL);free(source);
        if(!mod)vkd_fail("FSR shader compilation failed");
        VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=FSR.layout;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=mod;ci.stage.pName="main";
        VKC(vkCreateComputePipelines(V.device,VK_NULL_HANDLE,1,&ci,NULL,&FSR.pipe[i]));vkDestroyShaderModule(V.device,mod,NULL);
    }
    printf("pc-fsr: AMD FSR 1.0.2 EASU + RCAS ready (MIT, a21ffb8f)\n");
}
static void fsr_resize(int w,int h) {
    if(FSR.w==w && FSR.h==h)return;
    cmd_wait_all();fsr_drop_images();FSR.w=w;FSR.h=h;
    for(int i=0;i<2;i++) {
        FsrImage *t=&FSR.image[i];VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent.width=w;ci.extent.height=h;ci.extent.depth=1;ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateImage(V.device,&ci,NULL,&t->img));VkMemoryRequirements rq;vkGetImageMemoryRequirements(V.device,t->img,&rq);t->mem=vmem_alloc(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);VKC(vkBindImageMemory(V.device,t->img,t->mem.mem,t->mem.off));
        VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t->img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;VKC(vkCreateImageView(V.device,&vi,NULL,&t->view));
    }
}
/* A sampled 2D view of the window texture in format fmt (its UNORM or sRGB twin), made once and kept with the texture's
 * views (tag: vtype). FSR reads the stored values through the UNORM one; SMAA's last pass reads linear values through the
 * sRGB one. NVN 0x25 is RGBA8 UNORM; 0x38 is BGRA8 UNORM: views are made directly, so NVN numbering is not involved. */
static VkImageView vt_view_as(VTex *vt,VkFormat fmt,int vtype) {
    for(VView *v=vt->views;v;v=v->next)if(v->vtype==vtype)return v->view;
    VView *v=calloc(1,sizeof *v);if(!v)vkd_fail("view allocation");v->vtype=vtype;
    VkImageViewUsageCreateInfo use={VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};use.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.pNext=&use;vi.image=vt->img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;
    VKC(vkCreateImageView(V.device,&vi,NULL,&v->view));v->next=vt->views;vt->views=v;
    return v->view;
}
static int fsr_present_view(VkImageView input,const int s[4],const int d[4],VkImage output,float sharpness);
static int fsr_present(VTex *vt,const int s[4],const int d[4],VkImage output,float sharpness) {
    if(vt->fmt!=VK_FORMAT_R8G8B8A8_UNORM && vt->fmt!=VK_FORMAT_R8G8B8A8_SRGB && vt->fmt!=VK_FORMAT_B8G8R8A8_UNORM && vt->fmt!=VK_FORMAT_B8G8R8A8_SRGB)return 0;
    fsr_init();fsr_resize(d[2],d[3]);cmd_begin();
    VkFormat fmt=vt->fmt==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_UNORM:vt->fmt==VK_FORMAT_B8G8R8A8_SRGB?VK_FORMAT_B8G8R8A8_UNORM:vt->fmt;
    VkImageView input=vt_view_as(vt,fmt,777);
    vt_layout(vt,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return fsr_present_view(input,s,d,output,sharpness);
}
/* input: display-encoded values (a UNORM view) in SHADER_READ_ONLY_OPTIMAL */
static int fsr_present_view(VkImageView input,const int s[4],const int d[4],VkImage output,float sharpness) {
    fsr_init();fsr_resize(d[2],d[3]);cmd_begin();
    float pc[8]={(float)s[2],(float)s[3],(float)s[0],(float)s[1],(float)d[2],(float)d[3],sharpness,(V.format.format==VK_FORMAT_B8G8R8A8_UNORM||V.format.format==VK_FORMAT_B8G8R8A8_SRGB)?1.0f:0.0f};
    for(int i=0;i<2;i++) {
        FsrImage *t=&FSR.image[i];image_barrier(t->img,VK_IMAGE_ASPECT_COLOR_BIT,t->layout,VK_IMAGE_LAYOUT_GENERAL);t->layout=VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo info[2]={{FSR.sampler,i?FSR.image[0].view:input,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,t->view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2];memset(writes,0,sizeof writes);for(int k=0;k<2;k++){writes[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[k].dstSet=FSR.sets[R.cur][i];writes[k].dstBinding=k;writes[k].descriptorCount=1;writes[k].descriptorType=k?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[k].pImageInfo=&info[k];}
        vkUpdateDescriptorSets(V.device,2,writes,0,NULL);vkCmdBindPipeline(R.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,FSR.pipe[i]);vkCmdBindDescriptorSets(R.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,FSR.layout,0,1,&FSR.sets[R.cur][i],0,NULL);vkCmdPushConstants(R.cmd,FSR.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,32,pc);vkCmdDispatch(R.cmd,(d[2]+7)/8,(d[3]+7)/8,1);
        VkImageLayout next=i?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image_barrier(t->img,VK_IMAGE_ASPECT_COLOR_BIT,t->layout,next);t->layout=next;
    }
    VkImageCopy copy;memset(&copy,0,sizeof copy);copy.srcSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.srcSubresource.layerCount=1;copy.dstSubresource=copy.srcSubresource;copy.dstOffset.x=d[0];copy.dstOffset.y=d[1];copy.extent.width=d[2];copy.extent.height=d[3];copy.extent.depth=1;
    vkCmdCopyImage(R.cmd,FSR.image[1].img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);FSR.frames++;
    return 1;
}

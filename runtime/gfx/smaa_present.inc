/* SPDX-License-Identifier: GPL-3.0-or-later
 * SMAA 1x (Jimenez et al., MIT; runtime/gfx/smaa, iryoku/smaa 71c806a, unmodified) on the game's final picture, before it is
 * scaled to the window (Settings > Graphics, strikers.ini antialiasing = smaa). Three short passes at the picture's own size:
 * edges from the display-encoded values (luma), blending weights from SMAA's area and search tables, then the blend. The
 * blend reads linear values when the picture is sRGB (an sRGB view in, an sRGB image out), so the result keeps the
 * picture's colour class and its copy to the window stays a byte copy, as without SMAA. The PC menus are drawn after it.
 * The shader text (smaa_src) is smaa/smaa_src.h, made from SMAA.hlsl by tools/release/embed_smaa.py. */
#include "smaa/smaa_src.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
typedef struct { VkImage img; VkImageView view, view_unorm; VMem mem; VkImageLayout layout; } SmaaImage;
static struct { VkPipeline pipe[4]; VkPipelineLayout layout; VkDescriptorSetLayout dl; VkDescriptorPool pool; VkDescriptorSet sets[NSLOT][3];
    VkSampler lin, point; SmaaImage edges, blend, out, area, search; int w, h, srgb, broken, tables; LONG64 frames; } SM;

static void smaa_drop(SmaaImage *t) {
    if(t->view_unorm && t->view_unorm!=t->view)vkDestroyImageView(V.device,t->view_unorm,NULL);
    if(t->view)vkDestroyImageView(V.device,t->view,NULL);
    if(t->img)vkDestroyImage(V.device,t->img,NULL);
    if(t->mem.mem)vmem_free(t->mem);
    memset(t,0,sizeof *t);
}
static void smaa_image(SmaaImage *t,int w,int h,VkFormat fmt,VkFormat unorm,VkImageUsageFlags usage) {
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=fmt;ci.extent.width=(uint32_t)w;ci.extent.height=(uint32_t)h;ci.extent.depth=1;
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;if(unorm!=fmt)ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VKC(vkCreateImage(V.device,&ci,NULL,&t->img));VkMemoryRequirements rq;vkGetImageMemoryRequirements(V.device,t->img,&rq);
    t->mem=vmem_alloc(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);VKC(vkBindImageMemory(V.device,t->img,t->mem.mem,t->mem.off));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t->img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;
    VKC(vkCreateImageView(V.device,&vi,NULL,&t->view));t->view_unorm=t->view;
    if(unorm!=fmt) {vi.format=unorm;VKC(vkCreateImageView(V.device,&vi,NULL,&t->view_unorm));}
    t->layout=VK_IMAGE_LAYOUT_UNDEFINED;
}
static void smaa_shutdown(void) {
    smaa_drop(&SM.edges);smaa_drop(&SM.blend);smaa_drop(&SM.out);smaa_drop(&SM.area);smaa_drop(&SM.search);
    for(int i=0;i<4;i++)if(SM.pipe[i])vkDestroyPipeline(V.device,SM.pipe[i],NULL);
    if(SM.layout)vkDestroyPipelineLayout(V.device,SM.layout,NULL);
    if(SM.pool)vkDestroyDescriptorPool(V.device,SM.pool,NULL);
    if(SM.dl)vkDestroyDescriptorSetLayout(V.device,SM.dl,NULL);
    if(SM.lin)vkDestroySampler(V.device,SM.lin,NULL);
    if(SM.point)vkDestroySampler(V.device,SM.point,NULL);
    if(SM.frames)printf("pc-smaa: %lld frames\n",(long long)SM.frames);
    memset(&SM,0,sizeof SM);
}

/* The vertex stages draw one triangle over the picture (texcoord 0..1, top-left first, as Vulkan's clip space runs). */
static const char *const k_smaa_vs[3]={
    "layout(location=0) out vec2 tc;layout(location=1) out vec4 off[3];\n"
    "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));tc=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);SMAAEdgeDetectionVS(tc,off);}\n",
    "layout(location=0) out vec2 tc;layout(location=1) out vec2 pix;layout(location=2) out vec4 off[3];\n"
    "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));tc=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);SMAABlendingWeightCalculationVS(tc,pix,off);}\n",
    "layout(location=0) out vec2 tc;layout(location=1) out vec4 off;\n"
    "void main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));tc=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);SMAANeighborhoodBlendingVS(tc,off);}\n"};
static const char *const k_smaa_fs[3]={
    "layout(location=0) in vec2 tc;layout(location=1) in vec4 off[3];layout(set=0,binding=0) uniform sampler2D colorTex;layout(location=0) out vec2 o;\n"
    "void main(){o=SMAALumaEdgeDetectionPS(tc,off,colorTex);}\n",
    "layout(location=0) in vec2 tc;layout(location=1) in vec2 pix;layout(location=2) in vec4 off[3];\n"
    "layout(set=0,binding=0) uniform sampler2D edgesTex;layout(set=0,binding=1) uniform sampler2D areaTex;layout(set=0,binding=2) uniform sampler2D searchTex;layout(location=0) out vec4 o;\n"
    "void main(){o=SMAABlendingWeightCalculationPS(tc,pix,off,edgesTex,areaTex,searchTex,vec4(0.0));}\n",
    "layout(location=0) in vec2 tc;layout(location=1) in vec4 off;layout(set=0,binding=0) uniform sampler2D colorTex;layout(set=0,binding=1) uniform sampler2D blendTex;layout(location=0) out vec4 o;\n"
    "void main(){o=SMAANeighborhoodBlendingPS(tc,off,colorTex,blendTex);}\n"};
static VkShaderModule smaa_module(int stage,int pass) {
    const char *body=stage?k_smaa_fs[pass]:k_smaa_vs[pass];
    size_t cap=sizeof smaa_src+strlen(body)+512;char *src=malloc(cap);if(!src)vkd_fail("SMAA source allocation");
    snprintf(src,cap,"#version 450\n#define SMAA_GLSL_4 1\n#define SMAA_PRESET_HIGH 1\nlayout(push_constant) uniform Pc { vec4 rt; };\n#define SMAA_RT_METRICS rt\n"
                     "#define SMAA_INCLUDE_VS %d\n#define SMAA_INCLUDE_PS %d\n%s\n%s",stage?0:1,stage?1:0,smaa_src,body);
    uint64_t hash;char what[32];snprintf(what,sizeof what,"SMAA %s %d",stage?"FS":"VS",pass);
    VkShaderModule m=module_from_glsl(stage,src,what,&hash,NULL);free(src);
    return m;
}
static VkPipeline smaa_pipeline(int pass,VkFormat fmt) {
    VkShaderModule vs=smaa_module(0,pass),fs=smaa_module(1,pass);
    VkPipeline p=VK_NULL_HANDLE;
    if(vs && fs) {
        VkPipelineShaderStageCreateInfo st[2];memset(st,0,sizeof st);
        for(int i=0;i<2;i++){st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[i].pName="main";}
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vs;st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba;memset(&ba,0,sizeof ba);ba.colorWriteMask=0xF;
        VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ba;
        VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
        VkPipelineRenderingCreateInfo ri={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};ri.colorAttachmentCount=1;ri.pColorAttachmentFormats=&fmt;
        VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&ri;ci.stageCount=2;ci.pStages=st;
        ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pColorBlendState=&cb;ci.pDynamicState=&ds;ci.layout=SM.layout;
        if(vkCreateGraphicsPipelines(V.device,VK_NULL_HANDLE,1,&ci,NULL,&p)!=VK_SUCCESS)p=VK_NULL_HANDLE;
    }
    if(vs)vkDestroyShaderModule(V.device,vs,NULL);
    if(fs)vkDestroyShaderModule(V.device,fs,NULL);
    return p;
}
static int smaa_init(void) {
    if(SM.broken)return 0;
    if(SM.layout)return 1;
    VkDescriptorSetLayoutBinding b[3];memset(b,0,sizeof b);
    for(int i=0;i<3;i++){b[i].binding=(uint32_t)i;b[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[i].descriptorCount=1;b[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
    VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=3;di.pBindings=b;
    VKC(vkCreateDescriptorSetLayout(V.device,&di,NULL,&SM.dl));
    VkPushConstantRange pc={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&SM.dl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&pc;
    VKC(vkCreatePipelineLayout(V.device,&li,NULL,&SM.layout));
    VkDescriptorPoolSize sz={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,NSLOT*3*3};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.poolSizeCount=1;pi.pPoolSizes=&sz;pi.maxSets=NSLOT*3;
    VKC(vkCreateDescriptorPool(V.device,&pi,NULL,&SM.pool));
    for(int i=0;i<NSLOT;i++)for(int j=0;j<3;j++){VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=SM.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&SM.dl;VKC(vkAllocateDescriptorSets(V.device,&ai,&SM.sets[i][j]));}
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
    VKC(vkCreateSampler(V.device,&si,NULL,&SM.lin));
    si.magFilter=si.minFilter=VK_FILTER_NEAREST;VKC(vkCreateSampler(V.device,&si,NULL,&SM.point));
    SM.pipe[0]=smaa_pipeline(0,VK_FORMAT_R8G8_UNORM);SM.pipe[1]=smaa_pipeline(1,VK_FORMAT_R8G8B8A8_UNORM);
    SM.pipe[2]=smaa_pipeline(2,VK_FORMAT_R8G8B8A8_UNORM);SM.pipe[3]=smaa_pipeline(2,VK_FORMAT_R8G8B8A8_SRGB);
    for(int i=0;i<4;i++)if(!SM.pipe[i]) {printf("pc-smaa: pipeline %d could not be made: SMAA is off for this run\n",i);SM.broken=1;return 0;}
    printf("pc-smaa: SMAA 1x (high preset) ready (MIT, iryoku/smaa 71c806a)\n");
    return 1;
}
static void smaa_resize(int w,int h,int srgb) {
    if(SM.w==w && SM.h==h && SM.srgb==srgb)return;
    cmd_wait_all();smaa_drop(&SM.edges);smaa_drop(&SM.blend);smaa_drop(&SM.out);
    VkImageUsageFlags att=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    smaa_image(&SM.edges,w,h,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_UNORM,att);
    smaa_image(&SM.blend,w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,att);
    smaa_image(&SM.out,w,h,srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,att|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    SM.w=w;SM.h=h;SM.srgb=srgb;
}
static void smaa_upload(SmaaImage *t,const void *data,uint64_t size,int w,int h) {
    uint64_t off=ring_raw(data,size);
    pass_end();cmd_begin();
    image_barrier(t->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;memset(&c,0,sizeof c);c.bufferOffset=off;c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;c.imageSubresource.layerCount=1;
    c.imageExtent.width=(uint32_t)w;c.imageExtent.height=(uint32_t)h;c.imageExtent.depth=1;
    vkCmdCopyBufferToImage(R.cmd,R.ring,t->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    image_barrier(t->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
static void smaa_tables(void) {
    if(SM.tables)return;
    smaa_image(&SM.area,AREATEX_WIDTH,AREATEX_HEIGHT,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    smaa_image(&SM.search,SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    smaa_upload(&SM.area,areaTexBytes,sizeof areaTexBytes,AREATEX_WIDTH,AREATEX_HEIGHT);
    smaa_upload(&SM.search,searchTexBytes,sizeof searchTexBytes,SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT);
    SM.tables=1;
}
static void smaa_pass(int pass,int pipe,SmaaImage *target,int n,const VkImageView *in,const VkSampler *smp,const float rt[4]) {
    image_barrier(target->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkDescriptorImageInfo info[3];VkWriteDescriptorSet wr[3];memset(wr,0,sizeof wr);
    for(int i=0;i<n;i++) {
        info[i].sampler=smp[i];info[i].imageView=in[i];info[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=SM.sets[R.cur][pass];wr[i].dstBinding=(uint32_t)i;wr[i].descriptorCount=1;
        wr[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[i].pImageInfo=&info[i];
    }
    vkUpdateDescriptorSets(V.device,(uint32_t)n,wr,0,NULL);
    VkRenderingAttachmentInfo ca={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};ca.imageView=target->view;ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;memset(&ca.clearValue,0,sizeof ca.clearValue);
    VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent.width=(uint32_t)SM.w;ri.renderArea.extent.height=(uint32_t)SM.h;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&ca;
    vkCmdBeginRendering(R.cmd,&ri);
    vkCmdBindPipeline(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,SM.pipe[pipe]);
    VkViewport vp={0,0,(float)SM.w,(float)SM.h,0,1};VkRect2D sc={{0,0},{(uint32_t)SM.w,(uint32_t)SM.h}};
    vkCmdSetViewport(R.cmd,0,1,&vp);vkCmdSetScissor(R.cmd,0,1,&sc);
    vkCmdBindDescriptorSets(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,SM.layout,0,1,&SM.sets[R.cur][pass],0,NULL);
    vkCmdPushConstants(R.cmd,SM.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,16,rt);
    vkCmdDraw(R.cmd,3,1,0,0);
    vkCmdEndRendering(R.cmd);
    image_barrier(target->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    target->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
/* The game's picture through SMAA into SM.out (same size, R8G8B8A8 in the picture's colour class), left in
 * SHADER_READ_ONLY_OPTIMAL. 0: not done (another format, multisampled, or SMAA could not start): use vt as it is. */
static int smaa_run(VTex *vt) {
    if(!vt || vt->samples!=1 || vt->w<=0 || vt->h<=0)return 0;
    int srgb=vt->fmt==VK_FORMAT_R8G8B8A8_SRGB||vt->fmt==VK_FORMAT_B8G8R8A8_SRGB;
    VkFormat unorm=vt->fmt==VK_FORMAT_R8G8B8A8_SRGB||vt->fmt==VK_FORMAT_R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_UNORM:
                   vt->fmt==VK_FORMAT_B8G8R8A8_SRGB||vt->fmt==VK_FORMAT_B8G8R8A8_UNORM?VK_FORMAT_B8G8R8A8_UNORM:VK_FORMAT_UNDEFINED;
    if(unorm==VK_FORMAT_UNDEFINED || !smaa_init())return 0;
    smaa_resize(vt->w,vt->h,srgb);
    pass_end();cmd_begin();
    smaa_tables();
    VkImageView gamma=vt_view_as(vt,unorm,777),linear=srgb?vt_view_as(vt,unorm==VK_FORMAT_R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_B8G8R8A8_SRGB,778):gamma;
    vt_layout(vt,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float rt[4]={1.0f/(float)SM.w,1.0f/(float)SM.h,(float)SM.w,(float)SM.h};
    VkImageView in0[1]={gamma};VkSampler s0[1]={SM.lin};
    smaa_pass(0,0,&SM.edges,1,in0,s0,rt);
    VkImageView in1[3]={SM.edges.view,SM.area.view,SM.search.view};VkSampler s1[3]={SM.lin,SM.lin,SM.point};
    smaa_pass(1,1,&SM.blend,3,in1,s1,rt);
    VkImageView in2[2]={linear,SM.blend.view};VkSampler s2[2]={SM.lin,SM.lin};
    smaa_pass(2,srgb?3:2,&SM.out,2,in2,s2,rt);
    SM.frames++;
    return 1;
}

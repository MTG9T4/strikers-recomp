/* SPDX-License-Identifier: GPL-3.0-or-later
 * HDR output (Settings > Graphics, strikers.ini hdr = on; Windows' HDR switched on for the monitor). The game draws an SDR
 * picture; everything shown at a present (the picture, the loading bar, the PC menus) is drawn as without HDR into an 8-bit
 * image of the window's size (HDR.src), and one last pass writes it to the HDR10 swapchain: linear values (an sRGB view)
 * from BT.709 to BT.2020, white at `paper` nits, the brightest parts raised towards `peak` nits by their luminance (1 at
 * black and mid-tones, peak/paper at white: the sun, sparks and lights gain range, the picture's mid-tones stay), then the
 * PQ curve (SMPTE ST 2084). STRIKERS_HDR_TEST: the same values into a 10-bit SDR surface, read back by tools/audit. */
static struct { VkImage img; VkImageView view_srgb; VMem mem; uint32_t w, h; VkFormat fmt, out_fmt; VkPipeline pipe; VkPipelineLayout layout;
    VkDescriptorSetLayout dl; VkDescriptorPool pool; VkDescriptorSet sets[NSLOT]; VkSampler smp; LONG64 frames; int broken; } HDR;
int vk_hdr_available(void) { return V.hdr_avail; }      /* pc_overlay.cpp: 1 Windows HDR on, 2 test, 0 none */
int vk_hdr_active(void) { return V.hdr; }

static void hdr_drop_image(void) {
    if(HDR.view_srgb)vkDestroyImageView(V.device,HDR.view_srgb,NULL);
    if(HDR.img)vkDestroyImage(V.device,HDR.img,NULL);
    if(HDR.mem.mem)vmem_free(HDR.mem);
    HDR.view_srgb=VK_NULL_HANDLE;HDR.img=VK_NULL_HANDLE;memset(&HDR.mem,0,sizeof HDR.mem);HDR.w=HDR.h=0;HDR.fmt=0;
}
static void hdr_shutdown(void) {
    hdr_drop_image();
    if(HDR.pipe)vkDestroyPipeline(V.device,HDR.pipe,NULL);
    if(HDR.layout)vkDestroyPipelineLayout(V.device,HDR.layout,NULL);
    if(HDR.pool)vkDestroyDescriptorPool(V.device,HDR.pool,NULL);
    if(HDR.dl)vkDestroyDescriptorSetLayout(V.device,HDR.dl,NULL);
    if(HDR.smp)vkDestroySampler(V.device,HDR.smp,NULL);
    if(HDR.frames)printf("pc-hdr: %lld frames in HDR10\n",(long long)HDR.frames);
    memset(&HDR,0,sizeof HDR);
}
/* The image everything is drawn into this present: the 8-bit picture of the window's size, in fmt (UNORM or sRGB, the
 * window texture's colour class), left in TRANSFER_DST_OPTIMAL like a swapchain image after its clear. */
static VkImage hdr_target(VkFormat fmt) {
    if(HDR.img && (HDR.w!=V.extent.width || HDR.h!=V.extent.height || HDR.fmt!=fmt)) {cmd_wait_all();hdr_drop_image();}
    if(!HDR.img) {
        VkFormat srgb=fmt==VK_FORMAT_R8G8B8A8_UNORM||fmt==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_B8G8R8A8_SRGB;
        VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=fmt;ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.extent.width=V.extent.width;ci.extent.height=V.extent.height;ci.extent.depth=1;ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateImage(V.device,&ci,NULL,&HDR.img));VkMemoryRequirements rq;vkGetImageMemoryRequirements(V.device,HDR.img,&rq);
        HDR.mem=vmem_alloc(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);VKC(vkBindImageMemory(V.device,HDR.img,HDR.mem.mem,HDR.mem.off));
        VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=HDR.img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=srgb;
        VkImageViewUsageCreateInfo use={VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};use.usage=VK_IMAGE_USAGE_SAMPLED_BIT;vi.pNext=&use;
        vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;
        VKC(vkCreateImageView(V.device,&vi,NULL,&HDR.view_srgb));
        HDR.w=V.extent.width;HDR.h=V.extent.height;HDR.fmt=fmt;
    }
    return HDR.img;
}
static const char k_hdr_vs[]="#version 450\nvoid main(){vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
static const char k_hdr_fs[]=
    "#version 450\nlayout(set=0,binding=0) uniform sampler2D src;\nlayout(push_constant) uniform Pc { float paper; float peak; float pad0; float pad1; };\n"
    "layout(location=0) out vec4 o;\n"
    "vec3 pq(vec3 nits){vec3 y=clamp(nits/10000.0,0.0,1.0);vec3 p=pow(y,vec3(0.1593017578125));return pow((0.8359375+18.8515625*p)/(1.0+18.6875*p),vec3(78.84375));}\n"
    "void main(){vec3 c=texelFetch(src,ivec2(gl_FragCoord.xy),0).rgb;\n"
    " float y=clamp(dot(c,vec3(0.2126,0.7152,0.0722)),0.0,1.0);\n"
    " float gain=1.0+(max(peak,paper)/paper-1.0)*y*y*y*y;\n"
    " const mat3 bt2020=mat3(0.6274040,0.0690970,0.0163916, 0.3292820,0.9195400,0.0880132, 0.0433136,0.0113612,0.8955950);\n"
    " o=vec4(pq(bt2020*(c*paper*gain)),1.0);}\n";
static int hdr_init(VkFormat out_fmt) {
    if(HDR.broken)return 0;
    if(HDR.pipe && HDR.out_fmt==out_fmt)return 1;
    if(HDR.pipe){cmd_wait_all();vkDestroyPipeline(V.device,HDR.pipe,NULL);HDR.pipe=VK_NULL_HANDLE;}
    if(!HDR.layout) {
        VkDescriptorSetLayoutBinding b={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=1;di.pBindings=&b;VKC(vkCreateDescriptorSetLayout(V.device,&di,NULL,&HDR.dl));
        VkPushConstantRange pc={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
        VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&HDR.dl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&pc;
        VKC(vkCreatePipelineLayout(V.device,&li,NULL,&HDR.layout));
        VkDescriptorPoolSize sz={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,NSLOT};
        VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.poolSizeCount=1;pi.pPoolSizes=&sz;pi.maxSets=NSLOT;VKC(vkCreateDescriptorPool(V.device,&pi,NULL,&HDR.pool));
        for(int i=0;i<NSLOT;i++){VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=HDR.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&HDR.dl;VKC(vkAllocateDescriptorSets(V.device,&ai,&HDR.sets[i]));}
        VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(V.device,&si,NULL,&HDR.smp));
    }
    uint64_t hash;
    VkShaderModule vs=module_from_glsl(0,k_hdr_vs,"HDR VS",&hash,NULL),fs=module_from_glsl(1,k_hdr_fs,"HDR FS",&hash,NULL);
    if(vs && fs) {
        VkPipelineShaderStageCreateInfo st[2];memset(st,0,sizeof st);
        for(int i=0;i<2;i++){st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[i].pName="main";}
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vs;st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba;memset(&ba,0,sizeof ba);ba.colorWriteMask=0xF;
        VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ba;
        VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
        VkPipelineRenderingCreateInfo ri={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};ri.colorAttachmentCount=1;ri.pColorAttachmentFormats=&out_fmt;
        VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&ri;ci.stageCount=2;ci.pStages=st;
        ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pColorBlendState=&cb;ci.pDynamicState=&ds;ci.layout=HDR.layout;
        if(vkCreateGraphicsPipelines(V.device,VK_NULL_HANDLE,1,&ci,NULL,&HDR.pipe)!=VK_SUCCESS)HDR.pipe=VK_NULL_HANDLE;
    }
    if(vs)vkDestroyShaderModule(V.device,vs,NULL);
    if(fs)vkDestroyShaderModule(V.device,fs,NULL);
    if(!HDR.pipe){printf("pc-hdr: the HDR pass could not be made: SDR for this run\n");HDR.broken=1;return 0;}
    HDR.out_fmt=out_fmt;
    printf("pc-hdr: HDR10 pass ready (swapchain format %d, colour space %d)\n",(int)out_fmt,(int)V.hdr_fmt.colorSpace);
    return 1;
}
/* HDR.src (TRANSFER_DST_OPTIMAL or COLOR_ATTACHMENT_OPTIMAL as `from`) -> the swapchain image, left in `final_layout` */
static void hdr_convert(VkImage out,VkImageLayout from,float paper,float peak,VkImageLayout final_layout) {
    image_barrier(HDR.img,VK_IMAGE_ASPECT_COLOR_BIT,from,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(out,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageView view;VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=V.hdr_fmt.format;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;
    VKC(vkCreateImageView(V.device,&vi,NULL,&view));
    static VkImageView retire[NSLOT];                   /* a view is destroyed when its slot comes round again (its command buffer has finished) */
    if(retire[R.cur])vkDestroyImageView(V.device,retire[R.cur],NULL);
    retire[R.cur]=view;
    VkDescriptorImageInfo info={HDR.smp,HDR.view_srgb,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=HDR.sets[R.cur];w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info;
    vkUpdateDescriptorSets(V.device,1,&w,0,NULL);
    VkRenderingAttachmentInfo ca={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};ca.imageView=view;ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea.extent=V.extent;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&ca;
    vkCmdBeginRendering(R.cmd,&ri);
    vkCmdBindPipeline(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,HDR.pipe);
    VkViewport vp={0,0,(float)V.extent.width,(float)V.extent.height,0,1};VkRect2D sc={{0,0},V.extent};
    vkCmdSetViewport(R.cmd,0,1,&vp);vkCmdSetScissor(R.cmd,0,1,&sc);
    vkCmdBindDescriptorSets(R.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,HDR.layout,0,1,&HDR.sets[R.cur],0,NULL);
    float pc[4]={paper,peak,0,0};vkCmdPushConstants(R.cmd,HDR.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,pc);
    vkCmdDraw(R.cmd,3,1,0,0);
    vkCmdEndRendering(R.cmd);
    image_barrier(out,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,final_layout);
    HDR.frames++;
}

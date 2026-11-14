/* GPL-3.0-or-later. Resources for an isolated fragment diagnostic.
 * MATERIAL_REAL_SHADOW requires the depth snapshot; otherwise an all-lit map
 * is substituted explicitly. No diagnostic resource replacement is a fix. */
#include "nvn_front.h"
#include "tex_decode.h"
static Buf filebuf(const char *path) {
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"missing %s\n",path);exit(2);}
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);Buf b=buffer(n);fread(b.p,1,n,f);fclose(f);return b;
}
static VkImageView texview(VkImage im,VkFormat fmt,int cube,int volume,int levels,int layers,VkImageAspectFlags aspect,const uint8_t *swz) {
    VkImageViewCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=im;ci.format=fmt;ci.viewType=cube?VK_IMAGE_VIEW_TYPE_CUBE:volume?VK_IMAGE_VIEW_TYPE_3D:VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange=(VkImageSubresourceRange){aspect,0,levels,0,layers};
    if(swz){static const VkComponentSwizzle sw[6]={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A};ci.components=(VkComponentMapping){sw[swz[0]],sw[swz[1]],sw[swz[2]],sw[swz[3]]};}
    VkImageView v;CK(vkCreateImageView(dev,&ci,0,&v));return v;
}
typedef struct {Img t; VkSampler sampler; VkBufferImageCopy reg[96];int nr,levels,layers;VkImageAspectFlags aspect;VkFormat fmt;} ReplayTex;
static const char *resource_frame(void) { const char *p=getenv("MATERIAL_FRAME");return p?p:"7500"; }
static ReplayTex resource(const char *folder,int unit) {
    char path[1024],prefix[1024],envkey[64];snprintf(envkey,sizeof envkey,"MATERIAL_OVERRIDE_T%d",unit);
    const char *override=getenv(envkey);
    if(override)snprintf(prefix,sizeof prefix,"%s",override);else snprintf(prefix,sizeof prefix,"%s/f%s_s1_t%d",folder,resource_frame(),unit);
    snprintf(path,sizeof path,"%s.meta",prefix);FILE *f=fopen(path,"rb");if(!f)exit(2);
    int m[12];TexBuilder b;EView e;SamplerParams sp;fread(m,1,sizeof m,f);fread(&b,1,sizeof b,f);fread(&e,1,sizeof e,f);fread(&sp,1,sizeof sp,f);fclose(f);
    ReplayTex t={0};t.levels=m[6];t.layers=m[5];int is_depth=m[1]==VK_FORMAT_D24_UNORM_S8_UINT || m[1]==VK_FORMAT_D32_SFLOAT || m[1]==VK_FORMAT_D16_UNORM; t.aspect=is_depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    int real_shadow=is_depth && getenv("MATERIAL_REAL_SHADOW");
    int volume=b.target==2,cube=t.layers==6;VkFormat fmt=is_depth?(real_shadow?(VkFormat)m[1]:VK_FORMAT_D32_SFLOAT):(VkFormat)m[1];
    if(is_depth && getenv("MATERIAL_SHADOW_D32"))fmt=VK_FORMAT_D32_SFLOAT;
    t.fmt=fmt;
    const TexFormat *tf=tex_format(b.format);int decode=tf->sw_decode;TexLayout l;tex_layout(&l,b.target,b.format,b.width,b.height,b.depth,b.levels,b.flags&~0x50,0,b.has_layout&&b.layout[0]<=4?b.layout[0]:-1);
    size_t bytes=0;for(int a=0;a<t.layers;a++)for(int z=0;z<t.levels;z++) {int w=b.width>>z,h=b.height>>z,d=volume?b.depth>>z:1;if(!w)w=1;if(!h)h=1;if(!d)d=1;bytes+=decode?(size_t)w*h*d*4:is_depth?(size_t)w*h*4:l.lvl_linear_size[z];}
    t.t.transfer=buffer(bytes);size_t off=0;
    for(int a=0;a<t.layers;a++)for(int z=0;z<t.levels;z++) {
        int w=b.width>>z,h=b.height>>z,d=volume?b.depth>>z:1;if(!w)w=1;if(!h)h=1;if(!d)d=1;
        VkBufferImageCopy *r=&t.reg[t.nr++];r->bufferOffset=off;r->imageSubresource=(VkImageSubresourceLayers){t.aspect,z,a,1};r->imageExtent=(VkExtent3D){w,h,d};
        size_t n=decode?(size_t)w*h*d*4:is_depth?(size_t)w*h*4:l.lvl_linear_size[z];
        if(real_shadow) {
            snprintf(path,sizeof path,"%s.depth",prefix);f=fopen(path,"rb");if(!f){fprintf(stderr,"missing real shadow %s\n",path);exit(2);}
            if(fread((char*)t.t.transfer.p+off,1,n,f)!=n)exit(2);fclose(f);
            if(fmt==VK_FORMAT_D32_SFLOAT && m[1]==VK_FORMAT_D24_UNORM_S8_UINT)for(size_t k=0;k<n/4;k++){uint32_t raw=((uint32_t*)((char*)t.t.transfer.p+off))[k];((float*)((char*)t.t.transfer.p+off))[k]=(raw&0xffffff)/16777215.0f;}
        }
        else if(is_depth) {for(size_t k=0;k<n/4;k++)((float*)((char*)t.t.transfer.p+off))[k]=getenv("MATERIAL_DARK_SHADOW")?0:1;}
        else {
            snprintf(path,sizeof path,"%s_l%d_m%d.linear",prefix,a,z);f=fopen(path,"rb");
            if(!f){snprintf(path,sizeof path,"%s.linear",prefix);f=fopen(path,"rb");}if(!f){fprintf(stderr,"missing %s\n",path);exit(2);}
            if(decode){void *raw=malloc(l.lvl_linear_size[z]);fread(raw,1,l.lvl_linear_size[z],f);if(tex_decode_rgba8(b.format,raw,w,h*d,(uint8_t*)t.t.transfer.p+off))exit(2);free(raw);}
            else fread((char*)t.t.transfer.p+off,1,n,f);fclose(f);
        }off+=n;
    }
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;ci.imageType=volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;ci.format=fmt;ci.extent=(VkExtent3D){b.width,b.height,volume?b.depth:1};ci.mipLevels=t.levels;ci.arrayLayers=t.layers;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateImage(dev,&ci,0,&t.t.im));VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,t.t.im,&mr);VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt(mr.memoryTypeBits,0);CK(vkAllocateMemory(dev,&ai,0,&t.t.mem));CK(vkBindImageMemory(dev,t.t.im,t.t.mem,0));t.t.view=texview(t.t.im,fmt,cube,volume,t.levels,t.layers,t.aspect,e.swz);
    if(getenv("MATERIAL_COLOUR_SHADOW_MASK") && ((strtoul(getenv("MATERIAL_COLOUR_SHADOW_MASK"),0,0)>>unit)&1))sp.cmode=0;
    static const VkSamplerAddressMode wrap[8]={2,0,4,4,4,3,1,2};VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.minFilter=sp.min&1;si.magFilter=sp.mag?VK_FILTER_LINEAR:VK_FILTER_NEAREST;si.mipmapMode=sp.min>=4;si.addressModeU=wrap[sp.ws&7];si.addressModeV=wrap[sp.wt&7];si.addressModeW=wrap[sp.wr&7];si.minLod=sp.lodmin;si.maxLod=sp.min<2?.25f:sp.lodmax;si.mipLodBias=sp.bias;si.compareEnable=sp.cmode==1;si.compareOp=sp.cfunc?(sp.cfunc-1)&7:7;CK(vkCreateSampler(dev,&si,0,&t.sampler));return t;
}
static void upload_resource(VkCommandBuffer cmd,ReplayTex *t) {
    VkImageAspectFlags aspects=t->aspect;if(t->fmt==VK_FORMAT_D24_UNORM_S8_UINT || t->fmt==VK_FORMAT_D32_SFLOAT_S8_UINT)aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.image=t->t.im;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=(VkImageSubresourceRange){aspects,0,t->levels,0,t->layers};b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b);
    vkCmdCopyBufferToImage(cmd,t->t.transfer.b,t->t.im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,t->nr,t->reg);b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,0,0,0,1,&b);
}
static ReplayTex attachment_depth(const char *path) {
    ReplayTex t={0};t.t.transfer=filebuf(path);t.levels=t.layers=1;
    t.aspect=VK_IMAGE_ASPECT_DEPTH_BIT;t.fmt=VK_FORMAT_D24_UNORM_S8_UINT;
    if(t.t.transfer.size!=1920*1080*4)exit(2);
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=t.fmt;ci.extent=(VkExtent3D){1920,1080,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    CK(vkCreateImage(dev,&ci,0,&t.t.im));VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,t.t.im,&mr);VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt(mr.memoryTypeBits,0);CK(vkAllocateMemory(dev,&ai,0,&t.t.mem));CK(vkBindImageMemory(dev,t.t.im,t.t.mem,0));
    t.t.view=texview(t.t.im,t.fmt,0,0,1,1,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,NULL);
    t.nr=1;t.reg[0].imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};t.reg[0].imageExtent=ci.extent;return t;
}

/* GPL-3.0-or-later. Execute captured #3117 on real Vulkan images.
 * One source texel survives the shader's alpha test. The supplied projection
 * puts it at (10,10). Check .D raw half-pair loads, .P formatted stores, TXQ,
 * and the two branch joins against that independently specified result. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nvn_front.h"
#define CK(x) do { VkResult r=(x); if(r) {fprintf(stderr,"%s: %d\n",#x,r);exit(2);} } while(0)
static VkDevice dev; static VkPhysicalDeviceMemoryProperties mp;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize size; } Buf;
typedef struct { VkImage im; VkDeviceMemory mem; VkImageView view,raw; Buf transfer; int bpp; } Img;
static uint32_t mt(uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((bits>>i&1) && (mp.memoryTypes[i].propertyFlags&flags)==flags) return i;
    exit(2);
}
static Buf buffer(VkDeviceSize size) {
    Buf b={0};b.size=size;
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateBuffer(dev,&ci,0,&b.b));VkMemoryRequirements mr;vkGetBufferMemoryRequirements(dev,b.b,&mr);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(dev,&ai,0,&b.m));CK(vkBindBufferMemory(dev,b.b,b.m,0));CK(vkMapMemory(dev,b.m,0,size,0,&b.p));memset(b.p,0,(size_t)size);return b;
}
static void freebuf(Buf b) { vkDestroyBuffer(dev,b.b,0);vkFreeMemory(dev,b.m,0); }
static VkImageView view(VkImage im,VkFormat fmt) {
    VkImageViewCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=im;ci.format=fmt;ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView v;CK(vkCreateImageView(dev,&ci,0,&v));return v;
}
static Img image(int half) {
    Img t={0};t.bpp=half?8:4;t.transfer=buffer(32*32*t.bpp);
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=half?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R32_SFLOAT;
    ci.extent=(VkExtent3D){32,32,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateImage(dev,&ci,0,&t.im));VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,t.im,&mr);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=mt(mr.memoryTypeBits,0);
    CK(vkAllocateMemory(dev,&ai,0,&t.mem));CK(vkBindImageMemory(dev,t.im,t.mem,0));t.view=view(t.im,ci.format);t.raw=view(t.im,half?VK_FORMAT_R32G32_UINT:VK_FORMAT_R32_UINT);return t;
}
static VkBufferImageCopy region(void) { VkBufferImageCopy c={0};c.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent=(VkExtent3D){32,32,1};return c; }
static void barrier(VkCommandBuffer cmd,Img *t,VkImageLayout from,VkImageLayout to) {
    VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;b.image=t->im;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,0,0,0,1,&b);
}
static uint32_t bits(float f) { uint32_t u;memcpy(&u,&f,4);return u; }
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Strikers material regression";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance instance;CK(vkCreateInstance(&ic,0,&instance));
    uint32_t n=0;CK(vkEnumeratePhysicalDevices(instance,&n,0));VkPhysicalDevice *gpus=calloc(n,sizeof *gpus);CK(vkEnumeratePhysicalDevices(instance,&n,gpus));VkPhysicalDevice gpu=gpus[0];free(gpus);
    vkGetPhysicalDeviceMemoryProperties(gpu,&mp);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,0);VkQueueFamilyProperties *qp=calloc(n,sizeof *qp);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,qp);
    uint32_t qf=0;while(qf<n && !(qp[qf].queueFlags&VK_QUEUE_COMPUTE_BIT))qf++;free(qp);if(qf==n)return 2;
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=qf;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(gpu,&features);VkDeviceCreateInfo dc={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;dc.pEnabledFeatures=&features;CK(vkCreateDevice(gpu,&dc,0,&dev));VkQueue queue;vkGetDeviceQueue(dev,qf,0,&queue);
    Buf cb[3]={buffer(256),buffer(1024),buffer(256)};
    FILE *f=fopen(argv[2],"rb");if(!f)return 2;fread(cb[0].p,1,256,f);fclose(f);
    /* Same package-relative helper used by the runtime's actual C1 binding.
     * #3117 starts at data+256; the former code+constant_offset was wrong. */
    f=fopen(argv[3],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long package_size=ftell(f);rewind(f);uint8_t *package=malloc(package_size);fread(package,1,package_size,f);fclose(f);
    uint32_t ctl_size;memcpy(&ctl_size,package+12,4);uint8_t *ctl=package+20,*data=ctl+ctl_size;Stage captured={0};
    memcpy(&captured.ctl[3],ctl+0x6fc,4);memcpy(&captured.ctl[4],ctl+0x700,4);memcpy(&captured.ctl[6],ctl+0x708,4);captured.code=data+captured.ctl[6];
    if(captured.ctl[6]!=256 || captured.ctl[3]!=8 || memcmp(nvn_stage_data(&captured)+captured.ctl[4],cb[0].p,8))return 2;
    if(!memcmp(captured.code+captured.ctl[4],cb[0].p,8))return 2;
    memcpy(cb[0].p,nvn_stage_data(&captured)+captured.ctl[4],captured.ctl[3]);free(package);
    printf("captured #3117 package-relative C1 binding: PASS (entry offset 256, 8 constant bytes)\n");
    uint32_t *c2=cb[1].p,*c3=cb[2].p;c2[228]=32;c2[229]=32;c2[230]=1;c2[231]=1;
    c3[12]=bits(-.375f);c3[13]=bits(.375f);c3[15]=bits(1);c3[16]=c3[17]=bits(1.0f/32);c3[18]=bits(.25f);
    Img imgs[4]={image(1),image(1),image(0),image(0)};
    uint16_t *src=imgs[0].transfer.p;src[0]=0x3000;src[1]=0x3400;src[2]=0x3800;src[3]=0x3c00; /* .125 .25 .5 1 */
    for(int k=0;k<32*32;k++) { uint16_t *d=imgs[1].transfer.p;d[k*4]=d[k*4+1]=d[k*4+2]=0x4880;d[k*4+3]=0x3c00;((float*)imgs[2].transfer.p)[k]=2;((float*)imgs[3].transfer.p)[k]=9; }
    VkDescriptorSetLayoutBinding b0[3]={{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}};
    VkDescriptorSetLayoutBinding b1[5];uint32_t bindings[5]={512,1,513,514,3};for(int k=0;k<5;k++)b1[k]=(VkDescriptorSetLayoutBinding){bindings[k],VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,0};
    VkDescriptorSetLayout dl[2];for(int s=0;s<2;s++){VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=s?5:3;ci.pBindings=s?b1:b0;CK(vkCreateDescriptorSetLayout(dev,&ci,0,&dl[s]));}
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=2;li.pSetLayouts=dl;VkPipelineLayout layout;CK(vkCreatePipelineLayout(dev,&li,0,&layout));
    f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);uint32_t *spv=malloc(len);fread(spv,1,len,f);fclose(f);
    VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=len;mi.pCode=spv;VkShaderModule module;CK(vkCreateShaderModule(dev,&mi,0,&module));free(spv);
    VkComputePipelineCreateInfo pi={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;pi.stage=(VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",0};VkPipeline pipeline;CK(vkCreateComputePipelines(dev,0,1,&pi,0,&pipeline));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,5}};VkDescriptorPoolCreateInfo pci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pci.maxSets=2;pci.poolSizeCount=2;pci.pPoolSizes=sizes;VkDescriptorPool pool;CK(vkCreateDescriptorPool(dev,&pci,0,&pool));
    VkDescriptorSet sets[2];VkDescriptorSetAllocateInfo sa={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=pool;sa.descriptorSetCount=2;sa.pSetLayouts=dl;CK(vkAllocateDescriptorSets(dev,&sa,sets));
    VkWriteDescriptorSet writes[8]={0};VkDescriptorBufferInfo bi[3];VkDescriptorImageInfo ii[5];
    for(int k=0;k<3;k++){bi[k]=(VkDescriptorBufferInfo){cb[k].b,0,cb[k].size};writes[k]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,sets[0],k+1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,0,&bi[k],0};}
    VkImageView views[5]={imgs[0].raw,imgs[1].view,imgs[1].raw,imgs[2].raw,imgs[3].view};
    for(int k=0;k<5;k++){ii[k]=(VkDescriptorImageInfo){0,views[k],VK_IMAGE_LAYOUT_GENERAL};writes[3+k]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,sets[1],bindings[k],0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,&ii[k],0,0};}vkUpdateDescriptorSets(dev,8,writes,0,0);
    VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpci.queueFamilyIndex=qf;VkCommandPool cp;CK(vkCreateCommandPool(dev,&cpci,0,&cp));VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=cp;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;CK(vkAllocateCommandBuffers(dev,&ca,&cmd));VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CK(vkBeginCommandBuffer(cmd,&begin));
    VkBufferImageCopy reg=region();for(int k=0;k<4;k++){barrier(cmd,&imgs[k],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(cmd,imgs[k].transfer.b,imgs[k].im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&reg);barrier(cmd,&imgs[k],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);}
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,2,sets,0,0);vkCmdDispatch(cmd,1,1,1);
    for(int k=1;k<4;k+=2){barrier(cmd,&imgs[k],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cmd,imgs[k].im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,imgs[k].transfer.b,1,&reg);}CK(vkEndCommandBuffer(cmd));VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;CK(vkQueueSubmit(queue,1,&si,0));CK(vkQueueWaitIdle(queue));
    int bad=0;for(int k=0;k<32*32;k++){uint16_t *d=imgs[1].transfer.p;uint16_t expected[4]={0x4880,0x4880,0x4880,0x3c00};if(k==10*32+10){expected[0]=0x3000;expected[1]=0x3400;expected[2]=0x3800;expected[3]=0x3a00;}if(memcmp(d+k*4,expected,8)){if(bad<3)printf("pixel %d got %04x %04x %04x %04x\n",k,d[k*4],d[k*4+1],d[k*4+2],d[k*4+3]);bad++;}if(((float*)imgs[3].transfer.p)[k]!=(k==330?2:9))bad++;}
    printf("captured #3117 raw/formatted image, TXQ and branch execution: %s (%d mismatches)\n",bad?"FAIL":"PASS",bad);
    vkDestroyCommandPool(dev,cp,0);vkDestroyDescriptorPool(dev,pool,0);vkDestroyPipeline(dev,pipeline,0);vkDestroyShaderModule(dev,module,0);vkDestroyPipelineLayout(dev,layout,0);for(int k=0;k<2;k++)vkDestroyDescriptorSetLayout(dev,dl[k],0);
    for(int k=0;k<3;k++)freebuf(cb[k]);for(int k=0;k<4;k++){freebuf(imgs[k].transfer);vkDestroyImageView(dev,imgs[k].view,0);vkDestroyImageView(dev,imgs[k].raw,0);vkDestroyImage(dev,imgs[k].im,0);vkFreeMemory(dev,imgs[k].mem,0);}vkDestroyDevice(dev,0);vkDestroyInstance(instance,0);return bad?1:0;
}

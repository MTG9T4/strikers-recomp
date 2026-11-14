/* GPL-3.0-or-later. Compare the captured depth and colour vertex programs on
 * identical vertex/uniform/bone data, using separate GPU pipelines. */
#define main unused_image_test_main
#include "material_compute_test.c"
#undef main
#include <math.h>
static Buf filebuf(const char *name) {
    FILE *f=fopen(name,"rb");if(!f){perror(name);exit(2);}fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);Buf b=buffer(len);fread(b.p,1,len,f);fclose(f);return b;
}
int main(int argc,char **argv) {
    if(argc!=7)return 2;
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;VkInstanceCreateInfo ic={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance instance;CK(vkCreateInstance(&ic,0,&instance));
    uint32_t n=0;CK(vkEnumeratePhysicalDevices(instance,&n,0));VkPhysicalDevice *gpus=calloc(n,sizeof *gpus);CK(vkEnumeratePhysicalDevices(instance,&n,gpus));VkPhysicalDevice gpu=gpus[0];free(gpus);vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,0);VkQueueFamilyProperties *qp=calloc(n,sizeof *qp);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,qp);uint32_t qf=0;while(qf<n && !(qp[qf].queueFlags&VK_QUEUE_COMPUTE_BIT))qf++;free(qp);if(qf==n)return 2;
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=qf;qi.queueCount=1;qi.pQueuePriorities=&priority;VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(gpu,&features);
    VkDeviceCreateInfo dc={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;dc.pEnabledFeatures=&features;CK(vkCreateDevice(gpu,&dc,0,&dev));VkQueue queue;vkGetDeviceQueue(dev,qf,0,&queue);
    Buf inputs=filebuf(argv[3]),c3=filebuf(argv[4]),s2=filebuf(argv[5]);uint32_t vertices=(uint32_t)(inputs.size/(128*4));Buf output=buffer((VkDeviceSize)vertices*132*4);Buf buffers[4]={inputs,output,s2,c3};
    VkDescriptorSetLayoutBinding b[4];for(int k=0;k<4;k++)b[k]=(VkDescriptorSetLayoutBinding){k,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};VkDescriptorSetLayoutCreateInfo dlci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dlci.bindingCount=4;dlci.pBindings=b;VkDescriptorSetLayout dl;CK(vkCreateDescriptorSetLayout(dev,&dlci,0,&dl));
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&dl;VkPipelineLayout layout;CK(vkCreatePipelineLayout(dev,&li,0,&layout));VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4};VkDescriptorPoolCreateInfo pci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pci.maxSets=1;pci.poolSizeCount=1;pci.pPoolSizes=&ps;VkDescriptorPool pool;CK(vkCreateDescriptorPool(dev,&pci,0,&pool));VkDescriptorSetAllocateInfo da={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&dl;VkDescriptorSet set;CK(vkAllocateDescriptorSets(dev,&da,&set));
    VkDescriptorBufferInfo bi[4];VkWriteDescriptorSet wr[4];for(int k=0;k<4;k++){bi[k]=(VkDescriptorBufferInfo){buffers[k].b,0,buffers[k].size};wr[k]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,set,k,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,0,&bi[k],0};}vkUpdateDescriptorSets(dev,4,wr,0,0);
    VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpci.queueFamilyIndex=qf;cpci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool cp;CK(vkCreateCommandPool(dev,&cpci,0,&cp));VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=cp;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;CK(vkAllocateCommandBuffers(dev,&ca,&cmd));
    uint32_t *first=malloc((size_t)output.size);int bad=0;
    for(int pass=0;pass<2;pass++) {
        Buf code=filebuf(argv[1+pass]);VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=(size_t)code.size;mi.pCode=code.p;VkShaderModule module;CK(vkCreateShaderModule(dev,&mi,0,&module));
        VkComputePipelineCreateInfo pi={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;pi.stage=(VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",0};VkPipeline pipeline;CK(vkCreateComputePipelines(dev,0,1,&pi,0,&pipeline));
        CK(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CK(vkBeginCommandBuffer(cmd,&begin));vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,0);vkCmdDispatch(cmd,(vertices+63)/64,1,1);CK(vkEndCommandBuffer(cmd));VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;CK(vkQueueSubmit(queue,1,&si,0));CK(vkQueueWaitIdle(queue));
        char path[1024];snprintf(path,sizeof path,"%s-%d.bin",argv[6],pass);FILE *f=fopen(path,"wb");if(f){fwrite(output.p,1,(size_t)output.size,f);fclose(f);}
        if(!pass)memcpy(first,output.p,(size_t)output.size);
        else for(uint32_t v=0;v<vertices;v++) {
            uint32_t *p=(uint32_t*)output.p+132*v;
            if(memcmp(first+132*v,p,16)) {if(bad<5){float a[4],b[4];memcpy(a,first+132*v,16);memcpy(b,p,16);printf("vertex %u depth (%g %g %g %g) colour (%g %g %g %g) bits %08x/%08x\n",v,a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3],first[132*v+2],p[2]);}bad++;}
        }
        vkDestroyPipeline(dev,pipeline,0);vkDestroyShaderModule(dev,module,0);freebuf(code);
    }
    printf("captured depth/colour positions: %u vertices, %d bitwise mismatches\n",vertices,bad);
    free(first);vkDestroyCommandPool(dev,cp,0);vkDestroyDescriptorPool(dev,pool,0);vkDestroyPipelineLayout(dev,layout,0);vkDestroyDescriptorSetLayout(dev,dl,0);for(int k=0;k<4;k++)freebuf(buffers[k]);vkDestroyDevice(dev,0);vkDestroyInstance(instance,0);return bad?1:0;
}

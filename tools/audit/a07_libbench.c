/* Audit 07: what one stage costs in the driver, outside the game. Reads a list of the SPIR-V files of the stage cache and makes
 * the pipelines the runtime makes from them, on T threads, into a driver cache folder the caller chose:
 *   a07_libbench <list.txt> <threads> <mode> [count]
 * mode: lib      a stage library as nvn_vk_pipelines.c stagelib_ensure makes it
 *       noopt    the same with VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT
 *       whole    a whole pipeline of each fragment stage with the first vertex stage of the list
 *       module   vkCreateShaderModule only
 * Prints per-kind wall, thread CPU, and the slowest call. Environment: BENCH_PCACHE=1 passes a pipeline cache object.
 * Set __GL_SHADER_DISK_CACHE_PATH in the environment to an empty folder for a cold driver. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define F(n) static PFN_##n n
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceFeatures2); F(vkGetPhysicalDeviceProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr);
F(vkCreateShaderModule); F(vkDestroyShaderModule); F(vkCreateGraphicsPipelines); F(vkDestroyPipeline); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreatePipelineCache);
F(vkGetPhysicalDeviceQueueFamilyProperties);
static VkDevice dev; static VkPipelineLayout ulayout; static VkPipelineCache pcache; static VkShaderModule g_vs;
typedef struct { char path[300]; uint32_t *w; size_t n; int frag; double ms, cpu; } Item;
static Item *items; static int nitems; static volatile LONG next_i; static int mode;
static double qf;
static double now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / qf; }
static double thread_cpu(void) { FILETIME a, b, k, u; GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u); return ((double)(((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime))) / 1e7; }
static uint32_t dyn_list(VkDynamicState *dyn) {
    static const VkDynamicState base[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
        VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT};
    memcpy(dyn, base, sizeof base);
    return (uint32_t)(sizeof base / sizeof base[0]);
}
static void make(Item *it) {
    double t0 = now(), c0 = thread_cpu();
    VkShaderModule mod = NULL;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = it->n * 4; mi.pCode = it->w;
    if (vkCreateShaderModule(dev, &mi, NULL, &mod) != VK_SUCCESS) { printf("module failed: %s\n", it->path); return; }
    if (mode) {
        int isfs = it->frag;
        VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage = isfs ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; st.module = mod; st.pName = "main";
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
        pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.pNext = &pv;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.maxDepthBounds = 1.0f;
        VkDynamicState dyn[32];
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = dyn_list(dyn); dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        gl.pNext = &ri; gl.flags = isfs ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (mode == 2 ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0);
        gi.stageCount = 1; gi.pStages = &st; gi.pDynamicState = &dy; gi.layout = ulayout;
        if (!isfs) { gi.pViewportState = &vp; gi.pRasterizationState = &rs; }
        if (isfs) gi.pDepthStencilState = &ds;
        VkPipeline lib = NULL;
        VkPipelineShaderStageCreateInfo st2[2];
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba = {0}; ba.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        if (mode == 3) {                           /* a whole pipeline: the first vertex stage of the list with this fragment stage */
            if (!isfs) goto done;
            memset(st2, 0, sizeof st2);
            st2[0] = st; st2[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st2[0].module = g_vs; st2[1] = st;
            ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
            gi.pNext = &ri; gi.flags = 0; gi.stageCount = 2; gi.pStages = st2; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb;
        }
        VkResult r = vkCreateGraphicsPipelines(dev, pcache, 1, &gi, NULL, &lib);
        if (r != VK_SUCCESS) printf("pipeline failed %d: %s\n", r, it->path);
        else if (!getenv("BENCH_KEEP")) vkDestroyPipeline(dev, lib, NULL);
    }
done:
    if (!getenv("BENCH_KEEPMOD")) vkDestroyShaderModule(dev, mod, NULL);
    it->ms = (now() - t0) * 1000.0; it->cpu = (thread_cpu() - c0) * 1000.0;
}
static DWORD WINAPI worker(void *a) { (void)a; for (;;) { LONG i = InterlockedIncrement(&next_i) - 1; if (i >= nitems) return 0; make(&items[i]); } }
static int cmpd(const void *a, const void *b) { double x = ((const Item *)a)->ms, y = ((const Item *)b)->ms; return x < y ? 1 : x > y ? -1 : 0; }
int main(int argc, char **argv) {
    if (argc < 4) { puts("a07_libbench <list.txt> <threads> <lib|noopt|module> [count]"); return 2; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf = (double)f.QuadPart;
    setvbuf(stdout, NULL, _IONBF, 0);
    int threads = atoi(argv[2]), count = argc > 4 ? atoi(argv[4]) : 1 << 30;
    mode = !strcmp(argv[3], "lib") ? 1 : !strcmp(argv[3], "noopt") ? 2 : !strcmp(argv[3], "whole") ? 3 : 0;
    FILE *lf = fopen(argv[1], "r");
    if (!lf) return 3;
    items = calloc(65536, sizeof *items);
    char line[320];
    while (fgets(line, sizeof line, lf) && nitems < count) {
        line[strcspn(line, "\r\n")] = 0;
        FILE *fp = fopen(line, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint32_t *w = malloc((size_t)n);
        fread(w, 1, (size_t)n, fp); fclose(fp);
        Item *it = &items[nitems];
        snprintf(it->path, sizeof it->path, "%s", line); it->w = w; it->n = (size_t)n / 4; it->frag = -1;
        for (size_t i = 5; i < it->n;) { uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (!l) break; if (op == 15) { it->frag = w[i + 1] == 4; break; } i += l; }      /* OpEntryPoint: Fragment is 4, Vertex 0 */
        if (it->frag < 0 || w[0] != 0x07230203) { free(w); continue; }
        if (!it->frag) { size_t i = 5; for (; i < it->n;) { uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (op == 15 || !l) break; i += l; } if (i + 1 >= it->n || w[i + 1] != 0) { free(w); continue; } }      /* no compute / geometry */
        nitems++;
    }
    fclose(lf);
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    vkGetInstanceProcAddr = (void *)GetProcAddress(m, "vkGetInstanceProcAddr");
#define I(n) n = (void *)vkGetInstanceProcAddr(inst, #n)
    VkInstance inst = NULL;
    I(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_3; ai.pApplicationName = "a07_libbench";
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &ai;
    if (vkCreateInstance(&ic, NULL, &inst)) return 4;
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceFeatures2); I(vkGetPhysicalDeviceProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetPhysicalDeviceQueueFamilyProperties);
    VkPhysicalDevice pd[4]; uint32_t npd = 4; vkEnumeratePhysicalDevices(inst, &npd, pd);
    VkPhysicalDeviceProperties props; int use = -1;
    for (uint32_t i = 0; i < npd; i++) { vkGetPhysicalDeviceProperties(pd[i], &props); if (props.vendorID == 0x10de) { use = (int)i; break; } }
    if (use < 0) return 5;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fg = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT}; f3.pNext = &fg;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT}; fv.pNext = &f3;
    VkPhysicalDeviceDepthClipControlFeaturesEXT fd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT}; fd.pNext = &fv;
    VkPhysicalDeviceProvokingVertexFeaturesEXT fp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT}; fp2.pNext = &fd;
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; v13.pNext = &fp2;
    VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; v12.pNext = &v13;
    VkPhysicalDeviceVulkan11Features v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; v11.pNext = &v12;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &v11;
    vkGetPhysicalDeviceFeatures2(pd[use], &f2);
    float pr = 1; VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueCount = 1; qc.pQueuePriorities = &pr;
    const char *ext[] = {"VK_KHR_pipeline_library", "VK_EXT_graphics_pipeline_library", "VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state", "VK_EXT_depth_clip_control", "VK_EXT_provoking_vertex"};
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext = &f2; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc; dc.enabledExtensionCount = 6; dc.ppEnabledExtensionNames = ext;
    VkResult r = vkCreateDevice(pd[use], &dc, NULL, &dev);
    if (r) { printf("vkCreateDevice %d\n", r); return 6; }
#define D2(n) n = (void *)vkGetDeviceProcAddr(dev, #n)
    D2(vkCreateShaderModule); D2(vkDestroyShaderModule); D2(vkCreateGraphicsPipelines); D2(vkDestroyPipeline); D2(vkCreateDescriptorSetLayout); D2(vkCreatePipelineLayout); D2(vkCreatePipelineCache);
    static VkDescriptorSetLayoutBinding b0[66], b1[128];
    int n0 = 0, n1 = 0;
    for (unsigned s = 0; s < 2; s++) {
        VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        for (unsigned n = 1; n < 18; n++) { VkDescriptorSetLayoutBinding b = {40u * s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
        for (unsigned k = 0; k < 16; k++) { VkDescriptorSetLayoutBinding b = {40u * s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
        for (unsigned u = 0; u < 32; u++) { VkDescriptorSetLayoutBinding b = {40u * s + u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[n1++] = b; b.binding += 240; b1[n1++] = b; }
    }
    VkDescriptorSetLayout dl[2];
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
        vkCreateDescriptorSetLayout(dev, &li, NULL, &dl[i]);
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(dev, &pl, NULL, &ulayout);
    if (getenv("BENCH_PCACHE")) { VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; vkCreatePipelineCache(dev, &ci, NULL, &pcache); }
    for (int i = 0; i < nitems && !g_vs; i++) if (!items[i].frag) { VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = items[i].n * 4; mi.pCode = items[i].w; vkCreateShaderModule(dev, &mi, NULL, &g_vs); }
    double t0 = now();
    HANDLE th[64];
    for (int i = 0; i < threads; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)threads, th, TRUE, INFINITE);
    double wall = now() - t0, ms[2] = {0, 0}, cpu[2] = {0, 0}; int cnt[2] = {0, 0}; size_t bytes[2] = {0, 0};
    for (int i = 0; i < nitems; i++) { int k = items[i].frag; ms[k] += items[i].ms; cpu[k] += items[i].cpu; cnt[k]++; bytes[k] += items[i].n * 4; }
    printf("%s, %d threads, %d stages: wall %.2f s (%.1f stages/s)\n", argv[3], threads, nitems, wall, nitems / wall);
    for (int k = 0; k < 2; k++) printf("  %s: %d, %.0f KB average SPIR-V, call %.1f ms average, thread CPU %.1f ms average\n", k ? "fragment" : "vertex", cnt[k], cnt[k] ? bytes[k] / 1024.0 / cnt[k] : 0, cnt[k] ? ms[k] / cnt[k] : 0, cnt[k] ? cpu[k] / cnt[k] : 0);
    qsort(items, (size_t)nitems, sizeof *items, cmpd);
    for (int i = 0; i < 5 && i < nitems; i++) printf("  slowest %.0f ms (cpu %.0f): %s, %zu KB\n", items[i].ms, items[i].cpu, items[i].path, items[i].n / 256);
    fflush(stdout);
    ExitProcess(0);
}

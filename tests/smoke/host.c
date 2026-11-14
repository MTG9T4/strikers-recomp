/* Toolchain smoke test: MSVC + SDL3 + Vulkan headers. Opens a hidden Vulkan window, creates an instance, names the GPU,
   then calls into a DLL built by clang 9 the way the lifted game code will be. */
#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

__declspec(dllimport) int lifted_add(int a, int b);

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *w = SDL_CreateWindow("smoke", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!w) { printf("SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    Uint32 n = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&n);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "smoke", 1, "smoke", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, n, exts };
    VkInstance inst;
    if (create(&ci, NULL, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 1; }

    PFN_vkEnumeratePhysicalDevices enumdev = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDevice devs[8];
    uint32_t count = 8;
    enumdev(inst, &count, devs);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        props(devs[i], &p);
        printf("gpu %u: %s (Vulkan %u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion));
    }
    printf("lifted_add(40, 2) = %d\n", lifted_add(40, 2));
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}

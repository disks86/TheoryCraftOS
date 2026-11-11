/* A Windows GUI program for checking Vulkan presents into a window of the desktop by eye (or by screendump): a 320 x 240 window whose client area a swapchain clears
 * and presents every frame, its colour sliding from red to blue and back. It keeps running until its window is closed.
 *   x86_64-w64-mingw32-gcc -O1 -mwindows -o vkwindowtest.exe tests/windows/vkwindowtest.c -luser32
 * mingw gcc, no import library for vulkan-1.dll: it is loaded by name.
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include "../../src/Vulkan/khronos/vulkan/vulkan_core.h"
#include "../../src/Vulkan/khronos/vulkan/vulkan_win32.h"

static int g_quit;

static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_DESTROY) {
    g_quit = 1;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

#define FN(name) static PFN_##name name;
FN(vkCreateInstance) FN(vkEnumeratePhysicalDevices) FN(vkCreateDevice) FN(vkGetDeviceQueue) FN(vkCreateCommandPool) FN(vkAllocateCommandBuffers) FN(vkCreateFence)
FN(vkResetFences) FN(vkWaitForFences) FN(vkQueueSubmit) FN(vkBeginCommandBuffer) FN(vkEndCommandBuffer) FN(vkResetCommandBuffer) FN(vkCmdClearColorImage)
FN(vkCreateWin32SurfaceKHR) FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) FN(vkGetPhysicalDeviceSurfaceFormatsKHR) FN(vkCreateSwapchainKHR) FN(vkGetSwapchainImagesKHR)
FN(vkAcquireNextImageKHR) FN(vkQueuePresentKHR)

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
  (void)previous; (void)command; (void)show;
  WNDCLASSW cls = {0};
  cls.lpfnWndProc = proc;
  cls.hInstance = instance;
  cls.lpszClassName = L"vkwindowtest";
  RegisterClassW(&cls);
  RECT r = {0, 0, 320, 240};
  AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
  HWND hwnd = CreateWindowExW(0, L"vkwindowtest", L"Vulkan window", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 120, r.right - r.left, r.bottom - r.top, NULL, NULL, instance, NULL);
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!hwnd || !lib) return 1;
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
  vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  const char *ix[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.enabledExtensionCount = 2, ici.ppEnabledExtensionNames = ix;
  VkInstance vi = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, NULL, &vi) != VK_SUCCESS) return 2;
#define L(name) name = (PFN_##name)gipa(vi, #name);
  L(vkEnumeratePhysicalDevices) L(vkCreateDevice) L(vkGetDeviceQueue) L(vkCreateCommandPool) L(vkAllocateCommandBuffers) L(vkCreateFence) L(vkResetFences) L(vkWaitForFences)
  L(vkQueueSubmit) L(vkBeginCommandBuffer) L(vkEndCommandBuffer) L(vkResetCommandBuffer) L(vkCmdClearColorImage) L(vkCreateWin32SurfaceKHR) L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
  L(vkGetPhysicalDeviceSurfaceFormatsKHR) L(vkCreateSwapchainKHR) L(vkGetSwapchainImagesKHR) L(vkAcquireNextImageKHR) L(vkQueuePresentKHR)
  unsigned npd = 1;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  vkEnumeratePhysicalDevices(vi, &npd, &pd);
  if (!pd) return 3;
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueCount = 1, qci.pQueuePriorities = &priority;
  const char *dx[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = dx;
  VkDevice dev = VK_NULL_HANDLE;
  if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) return 4;
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, 0, 0, &queue);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  vkCreateCommandPool(dev, &pci, NULL, &pool);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(dev, &cai, &cb);
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vkCreateFence(dev, &fci, NULL, &fence);

  VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = instance, sci.hwnd = hwnd;
  VkSurfaceKHR surface;
  if (vkCreateWin32SurfaceKHR(vi, &sci, NULL, &surface) != VK_SUCCESS) return 5;
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
  unsigned nf = 1;
  VkSurfaceFormatKHR format;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, &format);
  VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface, ci.minImageCount = 2, ci.imageFormat = format.format, ci.imageColorSpace = format.colorSpace, ci.imageExtent = caps.currentExtent, ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, ci.presentMode = VK_PRESENT_MODE_FIFO_KHR, ci.clipped = VK_TRUE;
  VkSwapchainKHR chain;
  if (vkCreateSwapchainKHR(dev, &ci, NULL, &chain) != VK_SUCCESS) return 6;
  unsigned n = 8;
  VkImage images[8];
  vkGetSwapchainImagesKHR(dev, chain, &n, images);

  for (unsigned frame = 0; !g_quit; ++frame) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (g_quit) break;
    unsigned index = 0;
    vkAcquireNextImageKHR(dev, chain, ~0ull, VK_NULL_HANDLE, VK_NULL_HANDLE, &index);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    const float t = (float)(frame % 120) / 60.0f, k = t <= 1.0f ? t : 2.0f - t;
    VkClearColorValue cc = {{1.0f - k, 0.2f, k, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1, pi.pSwapchains = &chain, pi.pImageIndices = &index;
    vkQueuePresentKHR(queue, &pi);
    Sleep(16);
  }
  return 0;
}

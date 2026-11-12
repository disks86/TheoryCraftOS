/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* A Vulkan program as toolkits and games write one: glibc, libwayland-client, an xdg-shell toplevel, and Vulkan through libvulkan.so.1 with
 * VK_KHR_wayland_surface and VK_KHR_swapchain (no dmabuf or protocol of its own: the driver hands the swapchain's images to the compositor). It checks
 * what the surface reports (an extent the swapchain picks, one format, the present modes), renders frames with a render pass that clears each image
 * (FIFO, then a second swapchain of another size made from the first, in MAILBOX), that an acquire with
 * no timeout answers VK_NOT_READY when the program holds every free image, and that the program's own event queue keeps working beside the swapchain's.
 * The last frame is purple (0xFF8040C0). Prints "vk-swapchain: N checks, M failed". tests/linux_vulkan_wayland_emulated.sh runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "../../src/Vulkan/khronos/vulkan/vulkan_core.h"
#include "../../src/Vulkan/khronos/vulkan/vulkan_wayland.h"

static int checks, failed;
#define CHECK(c)                                                    \
  do {                                                              \
    ++checks;                                                       \
    if (!(c)) {                                                     \
      ++failed;                                                     \
      printf("FAILED line %d: %s\n", __LINE__, #c);                 \
    }                                                               \
  } while (0)

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm;
static struct zxdg_decoration_manager_v1 *decorations;
static int configured, frames_done;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void)data;
  if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
  else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, "zxdg_decoration_manager_v1")) decorations = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data, (void)r, (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void ping(void *data, struct xdg_wm_base *b, uint32_t serial) { (void)data, xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};
static void surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(s, serial);
  configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {surface_configure};
static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
  (void)data, (void)t, (void)w, (void)h, (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) { (void)data, (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};
static void own_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  (void)data, (void)time;
  ++frames_done;
  wl_callback_destroy(cb);
}
static const struct wl_callback_listener own_frame_listener = {own_frame_done};

typedef struct Chain {
  VkSwapchainKHR swapchain;
  uint32_t n, w, h;
  VkImage images[8];
  VkImageView views[8];
  VkFramebuffer framebuffers[8];
} Chain;

static VkDevice dev;
static VkQueue queue;
static VkRenderPass pass;
static VkCommandBuffer cb;
static VkFence fence;

static int make_chain(VkSurfaceKHR surface, uint32_t w, uint32_t h, VkPresentModeKHR mode, VkSwapchainKHR old, Chain *c) {
  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface, sci.minImageCount = 3, sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM, sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sci.imageExtent.width = w, sci.imageExtent.height = h, sci.imageArrayLayers = 1, sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, sci.presentMode = mode, sci.clipped = VK_TRUE, sci.oldSwapchain = old;
  memset(c, 0, sizeof *c);
  c->w = w, c->h = h;
  if (vkCreateSwapchainKHR(dev, &sci, NULL, &c->swapchain) != VK_SUCCESS) return 0;
  c->n = 8;
  if (vkGetSwapchainImagesKHR(dev, c->swapchain, &c->n, c->images) != VK_SUCCESS) return 0;
  for (uint32_t i = 0; i < c->n; ++i) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = c->images[i], vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, vi.subresourceRange.levelCount = 1, vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vi, NULL, &c->views[i]) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass, fi.attachmentCount = 1, fi.pAttachments = &c->views[i], fi.width = w, fi.height = h, fi.layers = 1;
    if (vkCreateFramebuffer(dev, &fi, NULL, &c->framebuffers[i]) != VK_SUCCESS) return 0;
  }
  return 1;
}

static void free_chain(Chain *c) {
  for (uint32_t i = 0; i < c->n; ++i) {
    vkDestroyFramebuffer(dev, c->framebuffers[i], NULL);
    vkDestroyImageView(dev, c->views[i], NULL);
  }
  vkDestroySwapchainKHR(dev, c->swapchain, NULL);
}

/* one frame: acquire, clear the image to (r, g, b), present */
static int frame(Chain *c, float r, float g, float b) {
  uint32_t index = 99;
  VkResult ar = vkAcquireNextImageKHR(dev, c->swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &index);
  if (ar != VK_SUCCESS || index >= c->n) return 0;
  vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev, 1, &fence);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cb, &bi);
  VkClearValue clear = {{{r, g, b, 1.0f}}};
  VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = pass, rp.framebuffer = c->framebuffers[index], rp.renderArea.extent.width = c->w, rp.renderArea.extent.height = c->h;
  rp.clearValueCount = 1, rp.pClearValues = &clear;
  vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdEndRenderPass(cb);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &cb;
  if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return 0;
  vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev, 1, &fence);
  VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  VkResult result = VK_ERROR_UNKNOWN;
  pi.swapchainCount = 1, pi.pSwapchains = &c->swapchain, pi.pImageIndices = &index, pi.pResults = &result;
  return vkQueuePresentKHR(queue, &pi) == VK_SUCCESS && result == VK_SUCCESS;
}

int main(void) {
  struct wl_display *display = wl_display_connect(NULL);
  CHECK(display != NULL);
  if (!display) return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  CHECK(compositor && wm);
  if (!compositor || !wm) return 1;
  xdg_wm_base_add_listener(wm, &wm_listener, NULL);
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surface);
  xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
  xdg_toplevel_set_title(top, "Linux Vulkan swapchain");
  struct zxdg_toplevel_decoration_v1 *decoration = NULL;
  if (decorations) {      /* (framed by the desktop, as a window of its own) */
    decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decorations, top);
    zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  wl_surface_commit(surface);
  while (!configured && wl_display_dispatch(display) >= 0) {
  }
  CHECK(configured);

  /* the instance: the surface extensions are there */
  uint32_t count = 0;
  CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count >= 2);
  VkExtensionProperties props[16];
  if (count > 16) count = 16;
  vkEnumerateInstanceExtensionProperties(NULL, &count, props);
  int have_surface = 0, have_wayland = 0;
  for (uint32_t i = 0; i < count; ++i) {
    have_surface |= !strcmp(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
    have_wayland |= !strcmp(props[i].extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
  }
  CHECK(have_surface && have_wayland);
  const char *instance_ext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app, ici.enabledExtensionCount = 2, ici.ppEnabledExtensionNames = instance_ext;
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
  PFN_vkCreateWaylandSurfaceKHR create_surface = (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR");
  CHECK(create_surface == vkCreateWaylandSurfaceKHR);
  uint32_t n = 1;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  CHECK(vkEnumeratePhysicalDevices(instance, &n, &pd) >= 0 && pd);
  if (!pd) return 1;
  CHECK(vkGetPhysicalDeviceWaylandPresentationSupportKHR(pd, 0, display) == VK_TRUE);
  VkWaylandSurfaceCreateInfoKHR wci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
  wci.display = display, wci.surface = surface;
  VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
  CHECK(vkCreateWaylandSurfaceKHR(instance, &wci, NULL, &vk_surface) == VK_SUCCESS);
  VkWaylandSurfaceCreateInfoKHR bad = wci;
  bad.surface = NULL;
  VkSurfaceKHR none = VK_NULL_HANDLE;
  CHECK(vkCreateWaylandSurfaceKHR(instance, &bad, NULL, &none) == VK_ERROR_INITIALIZATION_FAILED);
  VkBool32 supported = VK_FALSE;
  CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, vk_surface, &supported) == VK_SUCCESS && supported);

  /* what the surface takes */
  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vk_surface, &caps) == VK_SUCCESS);
  CHECK(caps.currentExtent.width == 0xFFFFFFFFu && caps.currentExtent.height == 0xFFFFFFFFu);      /* (the swapchain picks the window's size) */
  CHECK(caps.minImageCount >= 2 && caps.maxImageCount >= 3);
  CHECK(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR);
  VkSurfaceFormatKHR formats[4];
  count = 4;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vk_surface, &count, formats) == VK_SUCCESS && count == 1 && formats[0].format == VK_FORMAT_B8G8R8A8_UNORM);
  VkPresentModeKHR modes[4];
  count = 4;
  CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vk_surface, &count, modes) == VK_SUCCESS && count >= 2);

  /* the device */
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueCount = 1, q.pQueuePriorities = &priority;
  const char *device_ext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &q, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = device_ext;
  CHECK(vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS);
  if (!dev) return 1;
  vkGetDeviceQueue(dev, 0, 0, &queue);
  VkAttachmentDescription color = {0};
  color.format = VK_FORMAT_B8G8R8A8_UNORM, color.samples = VK_SAMPLE_COUNT_1_BIT, color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1, rpi.pAttachments = &color, rpi.subpassCount = 1, rpi.pSubpasses = &sub;
  CHECK(vkCreateRenderPass(dev, &rpi, NULL, &pass) == VK_SUCCESS);
  VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool = VK_NULL_HANDLE;
  CHECK(vkCreateCommandPool(dev, &pc, NULL, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = pool, ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ca.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(dev, &ca, &cb) == VK_SUCCESS);
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(vkCreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS);

  /* the first swapchain: FIFO, 320 x 200; the window takes its size */
  Chain a;
  CHECK(make_chain(vk_surface, 320, 200, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE, &a) && a.n == 3);
  int good = 0;
  for (int i = 0; i < 12; ++i) good += frame(&a, 0.0f, (float)(i % 4) / 3.0f, 1.0f);
  CHECK(good == 12);
  /* the program's own queue still works: a frame callback of its own, and a roundtrip */
  struct wl_callback *own = wl_surface_frame(surface);
  wl_callback_add_listener(own, &own_frame_listener, NULL);
  CHECK(frame(&a, 1.0f, 0.0f, 0.0f));
  while (!frames_done && wl_display_dispatch(display) >= 0) {
  }
  CHECK(frames_done == 1);
  CHECK(wl_display_roundtrip(display) >= 0);
  /* holding every free image: an acquire that may not wait says so */
  uint32_t held[8], nheld = 0;
  for (; nheld < a.n; ++nheld) {
    VkResult r = vkAcquireNextImageKHR(dev, a.swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &held[nheld]);
    if (r != VK_SUCCESS) {
      CHECK(r == VK_NOT_READY);
      break;
    }
  }
  CHECK(nheld >= 1 && nheld < a.n);      /* (the compositor still has the last frame) */
  {
    uint32_t extra = 0;
    CHECK(vkAcquireNextImageKHR(dev, a.swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &extra) == VK_NOT_READY);
  }
  /* (give them back: each presented once, with what it held) */
  for (uint32_t i = 0; i < nheld; ++i) {
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1, pi.pSwapchains = &a.swapchain, pi.pImageIndices = &held[i];
    CHECK(vkQueuePresentKHR(queue, &pi) == VK_SUCCESS);
  }

  /* a second swapchain made from the first (a resize), MAILBOX, 256 x 160 */
  Chain b;
  CHECK(make_chain(vk_surface, 256, 160, VK_PRESENT_MODE_MAILBOX_KHR, a.swapchain, &b));
  free_chain(&a);
  good = 0;
  for (int i = 0; i < 8; ++i) good += frame(&b, 0.5f, 0.25f, 0.75f);      /* purple: 0xFF8040C0 */
  CHECK(good == 8);
  CHECK(wl_display_roundtrip(display) >= 0);
  /* the desktop draws the last frame (the stand-in desktop of the emulator notes a pixel of the window) */
  for (int i = 0; i < 3; ++i) {
    struct wl_callback *again = wl_surface_frame(surface);
    wl_callback_add_listener(again, &own_frame_listener, NULL);
    CHECK(frame(&b, 0.5f, 0.25f, 0.75f));
    const int before = frames_done;
    while (frames_done == before && wl_display_dispatch(display) >= 0) {
    }
  }

  free_chain(&b);
  vkDestroySurfaceKHR(instance, vk_surface, NULL);
  vkDestroyFence(dev, fence, NULL);
  vkDestroyCommandPool(dev, pool, NULL);
  vkDestroyRenderPass(dev, pass, NULL);
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(instance, NULL);
  CHECK(wl_display_roundtrip(display) >= 0 && wl_display_get_error(display) == 0);
  if (decoration) zxdg_toplevel_decoration_v1_destroy(decoration);
  xdg_toplevel_destroy(top);
  xdg_surface_destroy(xs);
  wl_surface_destroy(surface);
  wl_display_disconnect(display);
  printf("vk-swapchain: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

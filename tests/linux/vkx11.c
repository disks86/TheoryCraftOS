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

/* A Vulkan program of an X11 toolkit: glibc, libxcb, libX11 and libX11-xcb, and Vulkan through libvulkan.so.1 with VK_KHR_xcb_surface and VK_KHR_xlib_surface.
 * It checks the instance's surface extensions, presentation support, what an xcb surface reports (the window's size as the current extent), frames rendered
 * with a render pass that clears each image (FIFO, then MAILBOX on a second swapchain made from the first), an acquire with no timeout when the program holds
 * every image, and then the same through Xlib (an XCreateSimpleWindow window and vkCreateXlibSurfaceKHR). The xcb window's last frame is purple (0xFF8040C0),
 * the Xlib window's teal (0xFF2080A0). Prints "vk-x11: N checks, M failed". tests/linux_vulkan_x11_emulated.sh runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#define VK_USE_PLATFORM_XCB_KHR 1
#define VK_USE_PLATFORM_XLIB_KHR 1
#include <vulkan/vulkan.h>

static int checks, failed;
#define CHECK(c)                                          \
  do {                                                    \
    ++checks;                                             \
    if (!(c)) {                                           \
      ++failed;                                           \
      printf("FAILED line %d: %s\n", __LINE__, #c);       \
    }                                                     \
  } while (0)

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

static void set_title(xcb_connection_t *c, xcb_window_t w, const char *title) {
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
}

int main(void) {
  /* ---- an xcb window */
  int screen_number = 0;
  xcb_connection_t *conn = xcb_connect(NULL, &screen_number);
  CHECK(conn && !xcb_connection_has_error(conn));
  if (!conn || xcb_connection_has_error(conn)) return 1;
  const xcb_setup_t *setup = xcb_get_setup(conn);
  xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
  CHECK(screen != NULL);
  if (!screen) return 1;
  xcb_window_t win = xcb_generate_id(conn);
  const uint32_t values[2] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 40, 40, 200, 150, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  set_title(conn, win, "Linux Vulkan xcb");
  xcb_map_window(conn, win);
  xcb_flush(conn);
  free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));      /* (mapped before the surface asks its size) */

  /* ---- the instance */
  uint32_t count = 0;
  CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count >= 3);
  VkExtensionProperties props[16];
  if (count > 16) count = 16;
  vkEnumerateInstanceExtensionProperties(NULL, &count, props);
  int have_xcb = 0, have_xlib = 0;
  for (uint32_t i = 0; i < count; ++i) {
    have_xcb |= !strcmp(props[i].extensionName, VK_KHR_XCB_SURFACE_EXTENSION_NAME);
    have_xlib |= !strcmp(props[i].extensionName, VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
  }
  CHECK(have_xcb && have_xlib);
  const char *instance_ext[3] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app, ici.enabledExtensionCount = 3, ici.ppEnabledExtensionNames = instance_ext;
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
  CHECK(vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR") != NULL && vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR") != NULL);
  uint32_t n = 1;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  CHECK(vkEnumeratePhysicalDevices(instance, &n, &pd) >= 0 && pd);
  if (!pd) return 1;
  CHECK(vkGetPhysicalDeviceXcbPresentationSupportKHR(pd, 0, conn, screen->root_visual) == VK_TRUE);

  /* ---- the xcb surface: its extent is the window's */
  VkXcbSurfaceCreateInfoKHR xci = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
  xci.connection = conn, xci.window = win;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  CHECK(vkCreateXcbSurfaceKHR(instance, &xci, NULL, &surface) == VK_SUCCESS);
  VkXcbSurfaceCreateInfoKHR bad = xci;
  bad.window = 0;
  VkSurfaceKHR none = VK_NULL_HANDLE;
  if (!getenv("TCOS_VK_LOADER")) CHECK(vkCreateXcbSurfaceKHR(instance, &bad, NULL, &none) == VK_ERROR_INITIALIZATION_FAILED);
  VkBool32 supported = VK_FALSE;
  CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &supported) == VK_SUCCESS && supported);
  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps) == VK_SUCCESS);
  CHECK(caps.currentExtent.width == 200 && caps.currentExtent.height == 150);
  if (caps.currentExtent.width != 200) printf("extent %u x %u\n", caps.currentExtent.width, caps.currentExtent.height);
  uint32_t nformats = 0;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nformats, NULL) == VK_SUCCESS && nformats >= 1);

  /* ---- the device */
  const char *device_ext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = 0, qi.queueCount = 1, qi.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = device_ext;
  CHECK(vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS);
  if (!dev) return 1;
  vkGetDeviceQueue(dev, 0, 0, &queue);
  VkAttachmentDescription att = {0};
  att.format = VK_FORMAT_B8G8R8A8_UNORM, att.samples = VK_SAMPLE_COUNT_1_BIT, att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1, rpi.pAttachments = &att, rpi.subpassCount = 1, rpi.pSubpasses = &sub;
  CHECK(vkCreateRenderPass(dev, &rpi, NULL, &pass) == VK_SUCCESS);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool = VK_NULL_HANDLE;
  CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS);
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(vkCreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS);

  /* ---- frames: FIFO, then MAILBOX on a swapchain made from the first */
  Chain a, b;
  CHECK(make_chain(surface, 200, 150, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE, &a));
  int good = 1;
  for (int i = 0; i < 4; ++i) good &= frame(&a, 0.1f * (float)i, 0.5f, 0.2f);
  CHECK(good);
  /* every image held: an acquire with no timeout answers VK_NOT_READY */
  uint32_t held[8], nheld = 0;
  for (;;) {
    VkResult r = vkAcquireNextImageKHR(dev, a.swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &held[nheld]);
    if (r != VK_SUCCESS || nheld == 7) break;
    ++nheld;
  }
  CHECK(nheld == a.n);
  uint32_t extra = 0;
  CHECK(vkAcquireNextImageKHR(dev, a.swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &extra) == VK_NOT_READY);
  for (uint32_t i = 0; i < nheld; ++i) {      /* (presented back, cleared to the last colour) */
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1, pi.pSwapchains = &a.swapchain, pi.pImageIndices = &held[i];
    CHECK(vkQueuePresentKHR(queue, &pi) == VK_SUCCESS);
  }
  CHECK(make_chain(surface, 200, 150, VK_PRESENT_MODE_MAILBOX_KHR, a.swapchain, &b));
  free_chain(&a);
  good = 1;
  for (int i = 0; i < 3; ++i) good &= frame(&b, 0.5f, 0.25f, 0.75f);      /* (purple: 0xFF8040C0) */
  CHECK(good);
  free_chain(&b);
  vkDestroySurfaceKHR(instance, surface, NULL);

  /* ---- Xlib: its own connection and window, made a surface through its xcb connection */
  Display *dpy = XOpenDisplay(NULL);
  CHECK(dpy != NULL);
  if (dpy) {
    Window xw = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 300, 60, 160, 120, 0, 0, 0xFFFFFF);
    XStoreName(dpy, xw, "Linux Vulkan xlib");
    XMapWindow(dpy, xw);
    XSync(dpy, False);
    CHECK(vkGetPhysicalDeviceXlibPresentationSupportKHR(pd, 0, dpy, XVisualIDFromVisual(DefaultVisual(dpy, DefaultScreen(dpy)))) == VK_TRUE);
    VkXlibSurfaceCreateInfoKHR lci = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    lci.dpy = dpy, lci.window = xw;
    VkSurfaceKHR ls = VK_NULL_HANDLE;
    CHECK(vkCreateXlibSurfaceKHR(instance, &lci, NULL, &ls) == VK_SUCCESS);
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, ls, &caps) == VK_SUCCESS && caps.currentExtent.width == 160 && caps.currentExtent.height == 120);
    Chain c;
    CHECK(make_chain(ls, 160, 120, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE, &c));
    good = 1;
    for (int i = 0; i < 3; ++i) good &= frame(&c, 0.125f, 0.5f, 0.625f);      /* (teal: 0xFF2080A0) */
    CHECK(good);
    free_chain(&c);
    vkDestroySurfaceKHR(instance, ls, NULL);
    XSync(dpy, False);
  }

  vkDestroyFence(dev, fence, NULL);
  vkDestroyCommandPool(dev, pool, NULL);
  vkDestroyRenderPass(dev, pass, NULL);
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(instance, NULL);
  printf("vk-x11: %d checks, %d failed\n", checks, failed);
  fflush(stdout);
  if (getenv("TCOS_VK_X11_HOLD")) {      /* (the windows stay a moment, for the desktop to draw them) */
    struct timespec t = {2, 0};
    nanosleep(&t, NULL);
  }
  if (dpy) XCloseDisplay(dpy);
  xcb_disconnect(conn);
  return failed ? 1 : 0;
}

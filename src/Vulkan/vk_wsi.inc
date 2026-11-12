/* The Vulkan driver, part 6: VK_KHR_surface and VK_KHR_swapchain. Two kinds of surface:
 *
 * - shown by the kernel: a swapchain image is an allocation the kernel can show (flip), into its window's client area when the surface names a window of the desktop (the
 *   kernel copies it into the window's surface), else on the display. VK_KHR_win32_surface on Windows, VK_EXT_headless_surface on Linux.
 * - shown by a compositor (VK_KHR_wayland_surface on Linux): the platform's surface has `os` set, its swapchain images are shareable allocations (made like an image with
 *   VkExternalMemoryImageCreateInfo), and the platform hands them to the compositor (vk_os.h: vk_os_swapchain_made / gone, vk_os_acquire, vk_os_present).
 *
 * Making a surface is the platform's: it fills in a Surface (make_surface), and says how big its window is (vk_os_surface_extent). */

struct Surface {
  uint64_t window;    /* what D3DKMTPresent takes as hWindow: a desktop window, or 0 for the display */
  uint32_t kind;      /* the platform's own (which extension made it) */
  void *os;           /* a compositor's surface: the platform's state for it (NULL: the kernel shows the images) */
};

typedef struct Swapchain {
  Device *dev;
  Img **img;
  uint32_t n;
  uint32_t next;
  uint32_t w, h;
  VkFormat format;
  uint64_t window;
  Surface *surface;
  bool premultiplied;   /* VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR (else the compositor ignores alpha) */
  VkPresentModeKHR mode;
  void *os;             /* the platform's, for a compositor's surface */
} Swapchain;

#if VK_OS_COMPOSITOR_SURFACES
static VkResult vk_os_swapchain_made(Swapchain *sc);
static void vk_os_swapchain_gone(Swapchain *sc);
static VkResult vk_os_acquire(Swapchain *sc, uint64_t timeout, uint32_t *index);
static VkResult vk_os_present(Swapchain *sc, uint32_t index);
#endif
static bool compositor_surface(const Surface *s) { return s && s->os; }

/* A surface of the platform's: `window` is the present target (0: the display). */
static VkResult make_surface(uint64_t window, uint32_t kind, VkSurfaceKHR *out) {
  Surface *s = vk_alloc(sizeof(Surface));
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->window = window, s->kind = kind;
  *out = (VkSurfaceKHR)s;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc) {
  (void)instance, (void)alloc;
  if (surface) vk_free(((Surface *)surface)->os);
  vk_free(surface);
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR surface, VkBool32 *supported) {
  (void)pd, (void)surface;
  *supported = family == 0;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *c) {
  const PhysDev *pd = (const PhysDev *)physical;
  Surface *s = (Surface *)surface;
  memset(c, 0, sizeof *c);
  uint32_t w = 0, h = 0;
  if (compositor_surface(s)) w = h = 0xFFFFFFFFu;      /* (the swapchain says how big the surface is: a Wayland window takes the size of its buffers) */
  else if (!vk_os_surface_extent(s, &w, &h) || w == 0 || h == 0) w = 640, h = 480;
  uint32_t dim = pd->caps.max_texture_dimension ? pd->caps.max_texture_dimension : 4096;
  c->minImageCount = 2, c->maxImageCount = 8;
  c->currentExtent.width = w, c->currentExtent.height = h;
  c->minImageExtent.width = c->minImageExtent.height = 1;
  c->maxImageExtent.width = c->maxImageExtent.height = dim;
  c->maxImageArrayLayers = 1;
  c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | (compositor_surface(s) ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : 0);
  c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *out) {
  (void)pd;
  static const VkSurfaceFormatKHR formats[2] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  /* (a compositor takes B8G8R8A8 only: a dmabuf in XRGB8888 / ARGB8888, the format every compositor reads) */
  const uint32_t total = compositor_surface((const Surface *)surface) ? 1u : 2u;
  if (!out) {
    *count = total;
    return VK_SUCCESS;
  }
  uint32_t n = MIN(*count, total);
  for (uint32_t i = 0; i < n; ++i) out[i] = formats[i];
  *count = n;
  return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *out) {
  (void)pd, (void)surface;
  static const VkPresentModeKHR modes[3] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  if (!out) {
    *count = 3;
    return VK_SUCCESS;
  }
  uint32_t n = MIN(*count, 3u);
  for (uint32_t i = 0; i < n; ++i) out[i] = modes[i];
  *count = n;
  return n < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
  Device *d = (Device *)device;
  (void)alloc;
  Surface *surface = (Surface *)ci->surface;
  const bool composited = compositor_surface(surface);
  if (ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && (composited || ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (ci->imageExtent.width == 0 || ci->imageExtent.height == 0 || ci->minImageCount > 8) return VK_ERROR_INITIALIZATION_FAILED;
#if !VK_OS_COMPOSITOR_SURFACES
  if (composited) return VK_ERROR_SURFACE_LOST_KHR;
#endif
  Swapchain *sc = vk_alloc(sizeof(Swapchain));
  if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
  sc->dev = d, sc->n = MAX(ci->minImageCount, 2u), sc->w = ci->imageExtent.width, sc->h = ci->imageExtent.height, sc->format = ci->imageFormat;
  sc->next = 0;
  sc->window = surface ? surface->window : 0;
  sc->surface = surface, sc->mode = ci->presentMode, sc->premultiplied = ci->compositeAlpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  sc->img = vk_alloc(sizeof(Img *) * sc->n);
  if (!sc->img) {
    vk_free(sc);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  const FormatInfo *fi = format_info(d->pd, ci->imageFormat);
  for (uint32_t i = 0; i < sc->n; ++i) {
    Img *img = vk_alloc(sizeof(Img));
    if (!img) break;
    img->dev = d, img->type = VK_IMAGE_TYPE_2D, img->format = ci->imageFormat, img->dfmt = fi->dfmt, img->bpp = fi->bpp, img->swap_rb = fi->swap_rb != 0;
    img->w = sc->w, img->h = sc->h, img->depth = 1, img->mips = 1, img->layers = 1, img->samples = 1;
    img->tiling = VK_IMAGE_TILING_OPTIMAL, img->usage = ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (composited) img->external = true;      /* (shared with the compositor, which samples it in place; never flipped) */
    else img->swapchain = true;
    if (image_create_allocation(img) != VK_SUCCESS) {
      vk_free(img);
      break;
    }
    sc->img[i] = img;
  }
  for (uint32_t i = 0; i < sc->n; ++i)
    if (!sc->img[i]) {
      for (uint32_t k = 0; k < sc->n; ++k) {
        if (sc->img[k]) kmt_free(d, sc->img[k]->alloc);
        vk_free(sc->img[k]);
      }
      vk_free(sc->img);
      vk_free(sc);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
#if VK_OS_COMPOSITOR_SURFACES
  if (composited) {
    const VkResult r = vk_os_swapchain_made(sc);
    if (r != VK_SUCCESS) {
      for (uint32_t k = 0; k < sc->n; ++k) {
        kmt_free(d, sc->img[k]->alloc);
        vk_free(sc->img[k]);
      }
      vk_free(sc->img);
      vk_free(sc);
      return r;
    }
  }
#endif
  *out = (VkSwapchainKHR)sc;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc) {
  Swapchain *sc = (Swapchain *)swapchain;
  (void)alloc;
  if (!sc) return;
#if VK_OS_COMPOSITOR_SURFACES
  if (compositor_surface(sc->surface)) vk_os_swapchain_gone(sc);      /* (the compositor lets go of the images first) */
#endif
  for (uint32_t i = 0; i < sc->n; ++i) {
    kmt_free((Device *)device, sc->img[i]->alloc);
    vk_free(sc->img[i]);
  }
  vk_free(sc->img);
  vk_free(sc);
}
VKAPI VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *out) {
  Swapchain *sc = (Swapchain *)swapchain;
  (void)device;
  if (!out) {
    *count = sc->n;
    return VK_SUCCESS;
  }
  uint32_t n = MIN(*count, sc->n);
  for (uint32_t i = 0; i < n; ++i) out[i] = (VkImage)sc->img[i];
  *count = n;
  return n < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index) {
  Swapchain *sc = (Swapchain *)swapchain;
  (void)device, (void)semaphore;
#if VK_OS_COMPOSITOR_SURFACES
  if (compositor_surface(sc->surface)) {
    const VkResult r = vk_os_acquire(sc, timeout, index);
    if (r == VK_SUCCESS && fence) ((Fence *)fence)->signaled = true;      /* (the image is free when it is handed out: nothing more to wait for) */
    return r;
  }
#endif
  (void)timeout;
  *index = sc->next;
  sc->next = (sc->next + 1) % sc->n;
  if (fence) ((Fence *)fence)->signaled = true;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
  Queue *q = (Queue *)queue;
  VkResult result = VK_SUCCESS;
  for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
    Swapchain *sc = (Swapchain *)pi->pSwapchains[i];
    if (pi->pImageIndices[i] >= sc->n) return VK_ERROR_OUT_OF_DATE_KHR;
#if VK_OS_COMPOSITOR_SURFACES
    if (compositor_surface(sc->surface)) {      /* (every submission has run when vkQueueSubmit returned: the image holds the frame) */
      const VkResult r = vk_os_present(sc, pi->pImageIndices[i]);
      if (pi->pResults) pi->pResults[i] = r;
      if (r != VK_SUCCESS) result = r;
      continue;
    }
#endif
    Img *img = sc->img[pi->pImageIndices[i]];
    uint8_t args[128];
    memset(args, 0, sizeof args);
    put32(args, 0, q->dev->ctx);
    put64(args, 8, sc->window);
    put32(args, 20, img->alloc);
    put32(args, 88, 4);   /* flip */
    KmtStatus st = kmt(TCOS_DXG_PRESENT, args);
    VkResult r = STATUS_OK(st) ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
    if (pi->pResults) pi->pResults[i] = r;
    if (r != VK_SUCCESS) result = r;
  }
  return result;
}

/* vulkan-1.c, part 6: VK_KHR_surface, VK_KHR_win32_surface and VK_KHR_swapchain. A swapchain image is an allocation the kernel can show (flip): into its window's client
 * area when the window is one of the desktop's (the kernel copies it into the window's surface), else on the display. */

typedef struct Surface {
  HINSTANCE instance;
  HWND window;
} Surface;

typedef struct Swapchain {
  Device *dev;
  Img **img;
  UINT n;
  UINT next;
  UINT w, h;
  VkFormat format;
  HWND window;
} Swapchain;

VKAPI VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
  (void)instance, (void)alloc;
  Surface *s = vk_alloc(sizeof(Surface));
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->instance = ci->hinstance, s->window = ci->hwnd;
  *out = (VkSurfaceKHR)s;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc) {
  (void)instance, (void)alloc;
  vk_free(surface);
}
VKAPI VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice pd, uint32_t family) {
  (void)pd;
  return family == 0;
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
  RECT r;
  UINT w = 0, h = 0;
  if (s->window && GetClientRect(s->window, &r)) w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
  if (w == 0 || h == 0) w = 640, h = 480;
  UINT dim = pd->caps.max_texture_dimension ? pd->caps.max_texture_dimension : 4096;
  c->minImageCount = 2, c->maxImageCount = 8;
  c->currentExtent.width = w, c->currentExtent.height = h;
  c->minImageExtent.width = c->minImageExtent.height = 1;
  c->maxImageExtent.width = c->maxImageExtent.height = dim;
  c->maxImageArrayLayers = 1;
  c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *out) {
  (void)pd, (void)surface;
  static const VkSurfaceFormatKHR formats[2] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  if (!out) {
    *count = 2;
    return VK_SUCCESS;
  }
  UINT n = MIN(*count, 2u);
  for (UINT i = 0; i < n; ++i) out[i] = formats[i];
  *count = n;
  return n < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *out) {
  (void)pd, (void)surface;
  static const VkPresentModeKHR modes[3] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  if (!out) {
    *count = 3;
    return VK_SUCCESS;
  }
  UINT n = MIN(*count, 3u);
  for (UINT i = 0; i < n; ++i) out[i] = modes[i];
  *count = n;
  return n < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
  Device *d = (Device *)device;
  (void)alloc;
  if (ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  Swapchain *sc = vk_alloc(sizeof(Swapchain));
  if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
  sc->dev = d, sc->n = MAX(ci->minImageCount, 2u), sc->w = ci->imageExtent.width, sc->h = ci->imageExtent.height, sc->format = ci->imageFormat;
  sc->next = 0;
  sc->window = ci->surface ? ((Surface *)ci->surface)->window : NULL;
  sc->img = vk_alloc(sizeof(Img *) * sc->n);
  if (!sc->img) {
    vk_free(sc);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  const FormatInfo *fi = format_info(d->pd, ci->imageFormat);
  for (UINT i = 0; i < sc->n; ++i) {
    Img *img = vk_alloc(sizeof(Img));
    if (!img) break;
    img->dev = d, img->type = VK_IMAGE_TYPE_2D, img->format = ci->imageFormat, img->dfmt = fi->dfmt, img->bpp = fi->bpp, img->swap_rb = fi->swap_rb != 0;
    img->w = sc->w, img->h = sc->h, img->depth = 1, img->mips = 1, img->layers = 1, img->samples = 1;
    img->tiling = VK_IMAGE_TILING_OPTIMAL, img->usage = ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, img->swapchain = TRUE;
    if (image_create_allocation(img) != VK_SUCCESS) {
      vk_free(img);
      break;
    }
    sc->img[i] = img;
  }
  for (UINT i = 0; i < sc->n; ++i)
    if (!sc->img[i]) {
      for (UINT k = 0; k < sc->n; ++k) {
        if (sc->img[k]) kmt_free(d, sc->img[k]->alloc);
        vk_free(sc->img[k]);
      }
      vk_free(sc->img);
      vk_free(sc);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
  *out = (VkSwapchainKHR)sc;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc) {
  Swapchain *sc = (Swapchain *)swapchain;
  (void)alloc;
  if (!sc) return;
  for (UINT i = 0; i < sc->n; ++i) {
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
  UINT n = MIN(*count, sc->n);
  for (UINT i = 0; i < n; ++i) out[i] = (VkImage)sc->img[i];
  *count = n;
  return n < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index) {
  Swapchain *sc = (Swapchain *)swapchain;
  (void)device, (void)timeout, (void)semaphore;
  *index = sc->next;
  sc->next = (sc->next + 1) % sc->n;
  if (fence) ((Fence *)fence)->signaled = TRUE;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
  Queue *q = (Queue *)queue;
  VkResult result = VK_SUCCESS;
  for (UINT i = 0; i < pi->swapchainCount; ++i) {
    Swapchain *sc = (Swapchain *)pi->pSwapchains[i];
    Img *img = sc->img[pi->pImageIndices[i]];
    BYTE args[128];
    memset(args, 0, sizeof args);
    put32(args, 0, q->dev->ctx);
    put64(args, 8, (ULONGLONG)(ULONG_PTR)sc->window);
    put32(args, 20, img->alloc);
    put32(args, 88, 4);   /* flip */
    NTSTATUS st = D3DKMTPresent(args);
    VkResult r = STATUS_OK(st) ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
    if (pi->pResults) pi->pResults[i] = r;
    if (r != VK_SUCCESS) result = r;
  }
  return result;
}

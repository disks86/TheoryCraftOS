/* The Vulkan driver, part 8: images as descriptors of the platform - VK_KHR_external_memory, VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf, where the platform
 * has them (vk_os.h: VK_OS_EXTERNAL_MEMORY_FD; on Linux a descriptor is a dmabuf of /dev/dxg, the kind a Wayland client hands the compositor with zwp_linux_dmabuf_v1).
 *
 * An image's pixels are its allocation, not its memory (the memory is the program's view, copied for linear images), so what is exported or imported is the allocation of the
 * image the memory belongs to: the one named by VkMemoryDedicatedAllocateInfo, else the first image bound to it. An image made with VkExternalMemoryImageCreateInfo has a
 * shareable allocation that is also a texture (the compositor samples it in place). Importing gives the dedicated image the descriptor's allocation in place of its own: the
 * sizes must agree and the formats be the same, else VK_ERROR_INVALID_EXTERNAL_HANDLE. Only 2D colour images of one level and one sample, 8 bits a channel. */
#if VK_OS_EXTERNAL_MEMORY_FD

static bool external_handle_type(VkExternalMemoryHandleTypeFlagBits t) {
  return t == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT || t == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
}

static VkResult memory_external_info(Device *d, const VkMemoryAllocateInfo *ai, Mem *m) {
  const VkImportMemoryFdInfoKHR *import = NULL;
  for (const VkBaseInStructure *n = (const VkBaseInStructure *)ai->pNext; n; n = n->pNext) {
    if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) m->image = (Img *)((const VkMemoryDedicatedAllocateInfo *)n)->image;
    else if (n->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) m->exportable = ((const VkExportMemoryAllocateInfo *)n)->handleTypes != 0;
    else if (n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) import = (const VkImportMemoryFdInfoKHR *)n;
  }
  if (!import || import->handleType == 0) return VK_SUCCESS;
  Img *img = m->image;
  if (!external_handle_type(import->handleType) || !img || import->fd < 0) {
    vk_os_trace("vkAllocateMemory: an import needs a dma_buf descriptor and a dedicated image\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  uint32_t handle = 0, w = 0, h = 0, format = 0, pitch = 0;
  if (!vk_os_import_fd(import->fd, d->dev, &handle, &w, &h, &format, &pitch)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (w != img->w || h != img->h || format != img->dfmt) {
    vk_os_trace("vkAllocateMemory: the imported image is not the dedicated image's size and format\n");
    kmt_free(d, handle);
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  kmt_free(d, img->alloc);
  img->alloc = handle;
  img->pitch = pitch ? pitch : img->w * img->bpp;
  vk_os_close_fd(import->fd);      /* (a successful import owns the descriptor; the handle is what stays) */
  return VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkGetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd) {
  (void)device;
  Mem *m = (Mem *)info->memory;
  if (!external_handle_type(info->handleType) || !m || !m->image || !m->image->alloc) {
    vk_os_trace("vkGetMemoryFdKHR: only the memory of an image can be exported, as a dma_buf\n");
    return VK_ERROR_TOO_MANY_OBJECTS;
  }
  if (!m->image->external) {
    vk_os_trace("vkGetMemoryFdKHR: the image was not made with VkExternalMemoryImageCreateInfo\n");
    return VK_ERROR_TOO_MANY_OBJECTS;
  }
  return vk_os_export_fd(m->image->alloc, fd) ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI VkResult VKAPI_CALL vkGetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits type, int fd, VkMemoryFdPropertiesKHR *props) {
  (void)device;
  if (type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT || fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  props->memoryTypeBits = 7;
  return VK_SUCCESS;
}

#endif

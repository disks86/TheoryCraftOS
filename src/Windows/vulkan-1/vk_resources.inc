/* vulkan-1.c, part 3: memory, buffers, images, samplers, shaders, descriptors, render passes. */

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
  Device *d = (Device *)device;
  (void)alloc;
  if (ai->memoryTypeIndex > 2 || ai->allocationSize == 0 || ai->allocationSize > (1ull << 31)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  Mem *m = vk_alloc(sizeof(Mem));
  if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
  m->data = vk_alloc((size_t)ai->allocationSize);
  if (!m->data) {
    vk_free(m);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  m->dev = d, m->size = ai->allocationSize, m->type = ai->memoryTypeIndex, m->version = 1;
  *out = (VkDeviceMemory)m;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc) {
  Mem *m = (Mem *)memory;
  (void)device, (void)alloc;
  if (!m) return;
  vk_free(m->data);
  vk_free(m);
}
VKAPI VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
  Mem *m = (Mem *)memory;
  (void)device, (void)flags;
  if (m->type == 0 || m->mapped || offset >= m->size) return VK_ERROR_MEMORY_MAP_FAILED;
  if (size != VK_WHOLE_SIZE && size > m->size - offset) return VK_ERROR_MEMORY_MAP_FAILED;
  m->mapped = TRUE;
  ++m->version;
  *data = m->data + offset;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
  Mem *m = (Mem *)memory;
  (void)device;
  m->mapped = FALSE;
  ++m->version;
}
VKAPI VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges) {
  (void)device;
  for (UINT i = 0; i < count; ++i) ++((Mem *)ranges[i].memory)->version;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges) {
  (void)device, (void)count, (void)ranges;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize *bytes) {
  (void)device;
  *bytes = ((Mem *)memory)->size;
}

/* The version of a memory object, counting a mapped one as written since it was last looked at (a mapping is how the program writes without telling us). */
static UINT mem_version(Mem *m) {
  if (m->mapped) ++m->version;
  return m->version;
}

/* ---- buffers --------------------------------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBuffer *out) {
  (void)alloc;
  if (ci->flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)) return VK_ERROR_FEATURE_NOT_PRESENT;
  Buf *b = vk_alloc(sizeof(Buf));
  if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
  b->dev = (Device *)device, b->size = ci->size, b->usage = ci->usage;
  *out = (VkBuffer)b;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *alloc) {
  Buf *b = (Buf *)buffer;
  (void)alloc;
  if (!b) return;
  kmt_free((Device *)device, b->alloc);
  vk_free(b);
}
VKAPI void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *r) {
  (void)device;
  r->size = (((Buf *)buffer)->size + 15) & ~15ull;
  r->alignment = 16;
  r->memoryTypeBits = 7;
}
VKAPI VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
  Buf *b = (Buf *)buffer;
  Mem *m = (Mem *)memory;
  (void)device;
  if (offset > m->size || b->size > m->size - offset) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  b->mem = m, b->off = offset;
  return VK_SUCCESS;
}

/* The buffer's contents on the GPU side (made when the buffer is first used by a draw; brought up to date when the program has written the memory since). */
static BOOL buf_sync(Buf *b, UINT *alloc_out) {
  if (!b->mem) return FALSE;
  if (!b->alloc) {
    UINT flags = 0;
    if (b->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) flags |= A_VERTEX;
    if (b->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) flags |= A_INDEX;
    if (b->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) flags |= A_UAV;
    if (!flags) flags = A_VERTEX;
    ULONGLONG size = (b->size + 15) & ~15ull;
    if (size < 16) size = 16;
    if (!kmt_make_buffer(b->dev, size, flags, &b->alloc)) return FALSE;
    b->synced = 0;
  }
  UINT version = mem_version(b->mem);
  if (b->synced != version) {
    BYTE *p = kmt_lock(b->dev, b->alloc, 0, LOCK_DISCARD);
    if (!p) return FALSE;
    memcpy(p, b->mem->data + b->off, (size_t)b->size);
    kmt_unlock(b->dev, b->alloc);
    b->synced = version;
  }
  *alloc_out = b->alloc;
  return TRUE;
}

/* What the GPU wrote into bytes off .. off + size of a buffer (a storage buffer of a dispatch or a draw) back into its memory, so that the program, the copies the CPU does and the
 * uniform data read from the memory see it. The buffer stays in step: nothing is copied up again for it. */
static void buf_readback(Buf *b, ULONGLONG off, ULONGLONG size) {
  if (!b->mem || !b->alloc || off >= b->size) return;
  if (size > b->size - off) size = b->size - off;
  BYTE *p = kmt_lock(b->dev, b->alloc, 0, LOCK_READ_ONLY);
  if (!p) return;
  memcpy(b->mem->data + b->off + off, p + off, (size_t)size);
  kmt_unlock(b->dev, b->alloc);
  b->synced = b->mem->version;
}

/* ---- images ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static UINT level_extent(UINT v, UINT level) {
  v >>= level;
  return v ? v : 1;
}

static ULONGLONG image_total_bytes(const Img *img) {
  ULONGLONG total = 0;
  for (UINT l = 0; l < img->mips; ++l) total += (ULONGLONG)level_extent(img->w, l) * level_extent(img->h, l) * img->bpp * (img->layers > 1 ? img->layers : 1) * (img->kind == 1 ? 1 : 1);
  return total * (img->kind == 1 ? img->depth : 1);
}

static VkResult image_create_allocation(Img *img) {
  Device *d = img->dev;
  UINT flags = 0;
  if (img->is_depth) {
    flags = A_DEPTH;
  } else {
    if (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) flags |= A_TARGET;
    if (img->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) flags |= A_TEXTURE;
    if (!flags) flags = A_TEXTURE;
    if (img->mips > 1) {
      if (flags & A_TARGET) return VK_ERROR_FORMAT_NOT_SUPPORTED;
      flags = A_TEXTURE;
    }
    if (img->samples > 1) flags = A_TARGET;
    if (img->swapchain) flags = A_TARGET | A_TEXTURE | A_PRESENT;
  }
  UINT w[AW_WORDS];
  memset(w, 0, sizeof w);
  w[AW_VERSION] = 1;
  w[AW_FLAGS] = flags;
  w[AW_FORMAT] = img->dfmt;
  w[AW_WIDTH] = img->w;
  w[AW_HEIGHT] = img->h;
  w[AW_MIPS] = img->mips;
  w[AW_KIND] = img->kind;
  w[AW_SAMPLES] = img->samples;
  w[AW_LAYERS] = img->kind == 1 ? img->depth : img->kind == 3 ? 6 : img->kind == 2 ? img->layers : 1;
  UINT pitch = 0;
  if (!kmt_create_allocation(d, w, &pitch, &img->alloc)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  img->pitch = pitch ? pitch : img->w * img->bpp;
  return VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImage *out) {
  Device *d = (Device *)device;
  const PhysDev *pd = d->pd;
  (void)alloc;
  const FormatInfo *fi = format_info(pd, ci->format);
  if (!fi) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (ci->flags & ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) return VK_ERROR_FEATURE_NOT_PRESENT;
  if (ci->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  UINT kind = 0;
  UINT layers = ci->arrayLayers;
  if (ci->imageType == VK_IMAGE_TYPE_3D) {
    if (!(pd->caps.features & FEAT_TEXTURE_KINDS) || ci->mipLevels != 1) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    kind = 1;
  } else if (ci->imageType != VK_IMAGE_TYPE_2D) {
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  } else if ((ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && layers == 6) {
    if (!(pd->caps.features & FEAT_TEXTURE_KINDS) || ci->mipLevels != 1 || ci->extent.width != ci->extent.height) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    kind = 3;
  } else if (layers > 1) {
    if (!(pd->caps.features & FEAT_TEXTURE_KINDS) || ci->mipLevels != 1) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    kind = 2;
  }
  if (ci->extent.width > pd->caps.max_texture_dimension || ci->extent.height > pd->caps.max_texture_dimension) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  UINT samples = ci->samples;
  if (samples != 1 && (!(pd->caps.features & FEAT_MULTISAMPLE) || !((pd->caps.sample_counts >> samples) & 1))) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  Img *img = vk_alloc(sizeof(Img));
  if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
  img->dev = d, img->type = ci->imageType, img->format = ci->format, img->dfmt = fi->dfmt, img->bpp = fi->bpp, img->swap_rb = fi->swap_rb != 0;
  img->w = ci->extent.width, img->h = ci->extent.height, img->depth = ci->extent.depth ? ci->extent.depth : 1;
  img->mips = ci->mipLevels, img->layers = layers ? layers : 1, img->samples = samples;
  img->tiling = ci->tiling, img->usage = ci->usage, img->flags = ci->flags, img->kind = kind;
  img->is_depth = format_is_depth(ci->format);
  VkResult r = image_create_allocation(img);
  if (r != VK_SUCCESS) {
    vk_free(img);
    return r;
  }
  *out = (VkImage)img;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc) {
  Img *img = (Img *)image;
  (void)alloc;
  if (!img) return;
  kmt_free((Device *)device, img->alloc);
  vk_free(img);
}
VKAPI void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *r) {
  (void)device;
  Img *img = (Img *)image;
  r->size = (image_total_bytes(img) + 15) & ~15ull;
  r->alignment = 16;
  r->memoryTypeBits = 7;
}
VKAPI VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
  Img *img = (Img *)image;
  Mem *m = (Mem *)memory;
  (void)device;
  if (offset > m->size || image_total_bytes(img) > m->size - offset) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  img->mem = m, img->off = offset;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t *count, VkSparseImageMemoryRequirements *r) {
  (void)device, (void)image, (void)r;
  *count = 0;
}
VKAPI void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *sub, VkSubresourceLayout *layout) {
  (void)device;
  Img *img = (Img *)image;
  ULONGLONG off = 0;
  for (UINT l = 0; l < sub->mipLevel && l < img->mips; ++l) off += (ULONGLONG)level_extent(img->w, l) * level_extent(img->h, l) * img->bpp;
  layout->offset = off;
  layout->rowPitch = (ULONGLONG)level_extent(img->w, sub->mipLevel) * img->bpp;
  layout->size = layout->rowPitch * level_extent(img->h, sub->mipLevel);
  layout->arrayPitch = layout->size;
  layout->depthPitch = layout->size;
}

/* The memory of a linear image to the allocation, and back: a program that maps an image sees the rows of each level one after the other. */
static void image_levels_copy(Img *img, BOOL to_gpu) {
  if (!img->mem) return;
  ULONGLONG off = img->off;
  UINT layers = img->kind == 1 ? img->depth : img->kind == 3 ? 6 : img->kind == 2 ? img->layers : 1;
  for (UINT l = 0; l < img->mips; ++l) {
    UINT w = level_extent(img->w, l), h = level_extent(img->h, l);
    ULONGLONG bytes = (ULONGLONG)w * h * img->bpp * layers;
    if (off + bytes > img->mem->size) return;
    BYTE *p = kmt_lock(img->dev, img->alloc, l, to_gpu ? LOCK_DISCARD : LOCK_READ_ONLY);
    if (!p) return;
    if (to_gpu) {
      memcpy(p, img->mem->data + off, (size_t)bytes);
      if (img->swap_rb) swap_rb_pixels(p, bytes / 4);
    } else {
      memcpy(img->mem->data + off, p, (size_t)bytes);
      if (img->swap_rb) swap_rb_pixels(img->mem->data + off, bytes / 4);
    }
    kmt_unlock(img->dev, img->alloc);
    off += bytes;
  }
}
static void image_sync_up(Img *img) {
  if (img->tiling != VK_IMAGE_TILING_LINEAR || !img->mem) return;
  UINT v = mem_version(img->mem);
  if (img->synced == v) return;
  image_levels_copy(img, TRUE);
  img->synced = v;
}
static void image_sync_down(Img *img) {
  if (img->tiling != VK_IMAGE_TILING_LINEAR || !img->mem) return;
  image_levels_copy(img, FALSE);
  img->synced = mem_version(img->mem);
}

VKAPI VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImageView *out) {
  (void)device, (void)alloc;
  View *v = vk_alloc(sizeof(View));
  if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
  v->img = (Img *)ci->image, v->type = ci->viewType, v->format = ci->format;
  v->base_mip = ci->subresourceRange.baseMipLevel;
  v->nmips = ci->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS ? v->img->mips - v->base_mip : ci->subresourceRange.levelCount;
  v->base_layer = ci->subresourceRange.baseArrayLayer;
  v->nlayers = ci->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS ? v->img->layers - v->base_layer : ci->subresourceRange.layerCount;
  *out = (VkImageView)v;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(view);
}

/* ---- buffer views, samplers, shader modules, pipeline caches ------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBufferView *out) {
  (void)device, (void)ci, (void)alloc, (void)out;
  return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI void VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView view, const VkAllocationCallbacks *alloc) {
  (void)device, (void)view, (void)alloc;
}

static UINT address_mode(VkSamplerAddressMode m) { return m == VK_SAMPLER_ADDRESS_MODE_REPEAT || m == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT ? 0 : 1; }

VKAPI VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *alloc, VkSampler *out) {
  (void)device, (void)alloc;
  Sampler *s = vk_alloc(sizeof(Sampler));
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->filter = ci->magFilter == VK_FILTER_LINEAR ? 1 : 0;
  s->mip_filter = ci->maxLod <= 0.0f ? 0 : ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? 2 : 1;
  s->addr_u = address_mode(ci->addressModeU), s->addr_v = address_mode(ci->addressModeV);
  s->lod_bias = ci->mipLodBias, s->min_lod = ci->minLod, s->max_lod = ci->maxLod;
  *out = (VkSampler)s;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(sampler);
}

VKAPI VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc, VkShaderModule *out) {
  (void)device, (void)alloc;
  if (ci->codeSize < 20 || ci->codeSize % 4 || ci->pCode[0] != 0x07230203u) return VK_ERROR_INVALID_SHADER_NV;
  Shader *s = vk_alloc(sizeof(Shader));
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->words = vk_alloc(ci->codeSize);
  if (!s->words) {
    vk_free(s);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  memcpy(s->words, ci->pCode, ci->codeSize);
  s->nwords = (UINT)(ci->codeSize / 4);
  *out = (VkShaderModule)s;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *alloc) {
  Shader *s = (Shader *)module;
  (void)device, (void)alloc;
  if (!s) return;
  vk_free(s->words);
  vk_free(s);
}

VKAPI VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipelineCache *out) {
  (void)device, (void)ci, (void)alloc;
  *out = (VkPipelineCache)vk_alloc(8);
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(cache);
}
VKAPI VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *size, void *data) {
  (void)device, (void)cache, (void)data;
  *size = 0;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dst, uint32_t count, const VkPipelineCache *src) {
  (void)device, (void)dst, (void)count, (void)src;
  return VK_SUCCESS;
}

/* ---- synchronisation objects: every submission has run when vkQueueSubmit returns, so these only keep a state ------------------------------------------------------ */
typedef struct Fence {
  BOOL signaled;
} Fence;
VKAPI VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkFence *out) {
  (void)device, (void)alloc;
  Fence *f = vk_alloc(sizeof(Fence));
  if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
  f->signaled = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
  *out = (VkFence)f;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(fence);
}
VKAPI VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence *fences) {
  (void)device;
  for (UINT i = 0; i < count; ++i) ((Fence *)fences[i])->signaled = FALSE;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence) {
  (void)device;
  return ((Fence *)fence)->signaled ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 wait_all, uint64_t timeout) {
  (void)device, (void)timeout;
  UINT signaled = 0;
  for (UINT i = 0; i < count; ++i) signaled += ((Fence *)fences[i])->signaled;
  return (wait_all ? signaled == count : signaled > 0) ? VK_SUCCESS : VK_TIMEOUT;
}
VKAPI VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *alloc, VkSemaphore *out) {
  (void)device, (void)ci, (void)alloc;
  *out = (VkSemaphore)vk_alloc(8);
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore s, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(s);
}
VKAPI VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const VkEventCreateInfo *ci, const VkAllocationCallbacks *alloc, VkEvent *out) {
  (void)device, (void)ci, (void)alloc;
  *out = (VkEvent)vk_alloc(8);
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent e, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(e);
}
VKAPI VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent e) {
  (void)device;
  return *(UINT *)e ? VK_EVENT_SET : VK_EVENT_RESET;
}
VKAPI VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent e) {
  (void)device;
  *(UINT *)e = 1;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent e) {
  (void)device;
  *(UINT *)e = 0;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkQueryPool *out) {
  (void)device, (void)ci, (void)alloc, (void)out;
  return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks *alloc) {
  (void)device, (void)pool, (void)alloc;
}
VKAPI VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
  (void)device, (void)pool, (void)first, (void)count, (void)size, (void)data, (void)stride, (void)flags;
  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* ---- render passes and framebuffers ---------------------------------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *alloc, VkRenderPass *out) {
  (void)alloc;
  if (ci->attachmentCount > 9 || ci->subpassCount > 4) return VK_ERROR_FEATURE_NOT_PRESENT;
  RenderPass *rp = vk_alloc(sizeof(RenderPass));
  if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
  rp->dev = (Device *)device;
  rp->natt = ci->attachmentCount;
  for (UINT i = 0; i < rp->natt; ++i) {
    const VkAttachmentDescription *a = &ci->pAttachments[i];
    rp->att[i].format = a->format, rp->att[i].samples = a->samples, rp->att[i].load = a->loadOp, rp->att[i].stencil_load = a->stencilLoadOp, rp->att[i].store = a->storeOp;
  }
  rp->nsub = ci->subpassCount;
  for (UINT s = 0; s < rp->nsub; ++s) {
    const VkSubpassDescription *sd = &ci->pSubpasses[s];
    if (sd->colorAttachmentCount > 4 || sd->inputAttachmentCount) {
      vk_free(rp);
      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    rp->sub[s].ncolor = sd->colorAttachmentCount;
    for (UINT c = 0; c < sd->colorAttachmentCount; ++c) rp->sub[s].color[c] = sd->pColorAttachments[c].attachment;
    rp->sub[s].depth = sd->pDepthStencilAttachment ? sd->pDepthStencilAttachment->attachment : 0xFFFFFFFFu;
    if (sd->pResolveAttachments) {
      rp->sub[s].nresolve = sd->colorAttachmentCount;
      for (UINT c = 0; c < sd->colorAttachmentCount; ++c) rp->sub[s].resolve[c] = sd->pResolveAttachments[c].attachment;
    }
  }
  *out = (VkRenderPass)rp;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(pass);
}
VKAPI void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice device, VkRenderPass pass, VkExtent2D *g) {
  (void)device, (void)pass;
  g->width = g->height = 1;
}
VKAPI VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkFramebuffer *out) {
  (void)alloc;
  if (ci->attachmentCount > 9) return VK_ERROR_FEATURE_NOT_PRESENT;
  Framebuffer *fb = vk_alloc(sizeof(Framebuffer));
  if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
  fb->dev = (Device *)device, fb->pass = (RenderPass *)ci->renderPass, fb->nview = ci->attachmentCount;
  fb->w = ci->width, fb->h = ci->height, fb->layers = ci->layers;
  for (UINT i = 0; i < fb->nview; ++i) fb->view[i] = (View *)ci->pAttachments[i];
  *out = (VkFramebuffer)fb;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(fb);
}

/* ---- descriptors ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
VKAPI VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *out) {
  (void)alloc;
  DSetLayout *l = vk_alloc(sizeof(DSetLayout));
  if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
  l->dev = (Device *)device;
  l->nb = ci->bindingCount;
  l->b = vk_alloc((l->nb ? l->nb : 1) * sizeof(DescBinding));
  if (!l->b) {
    vk_free(l);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  for (UINT i = 0; i < l->nb; ++i) {
    const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
    if (b->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
      vk_free(l->b);
      vk_free(l);
      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    l->b[i].binding = b->binding, l->b[i].type = b->descriptorType, l->b[i].count = b->descriptorCount, l->b[i].stages = b->stageFlags;
  }
  *out = (VkDescriptorSetLayout)l;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks *alloc) {
  DSetLayout *l = (DSetLayout *)layout;
  (void)device, (void)alloc;
  if (!l) return;
  vk_free(l->b);
  vk_free(l);
}

static int layout_binding_index(const DSetLayout *l, UINT binding) {
  for (UINT i = 0; i < l->nb; ++i)
    if (l->b[i].binding == binding) return (int)i;
  return -1;
}

/* The binding with the next higher number after `binding` (-1: none). */
static int next_binding_index(const DSetLayout *l, UINT binding) {
  int best = -1;
  for (UINT i = 0; i < l->nb; ++i)
    if (l->b[i].binding > binding && (best < 0 || l->b[i].binding < l->b[best].binding)) best = (int)i;
  return best;
}

static BOOL is_dynamic(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
/* The number of a dynamic descriptor among those of its set: the dynamic offsets come in the order of the bindings, then of the array elements. */
static UINT dynamic_index(const DSetLayout *l, UINT binding, UINT element) {
  UINT n = element;
  for (UINT k = 0; k < l->nb; ++k)
    if (is_dynamic(l->b[k].type) && l->b[k].binding < binding) n += l->b[k].count;
  return n;
}

VKAPI VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc, VkPipelineLayout *out) {
  (void)alloc;
  if (ci->setLayoutCount > 8) return VK_ERROR_FEATURE_NOT_PRESENT;
  PipeLayout *l = vk_alloc(sizeof(PipeLayout));
  if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
  l->dev = (Device *)device, l->nsets = ci->setLayoutCount;
  for (UINT i = 0; i < l->nsets; ++i) l->sets[i] = (DSetLayout *)ci->pSetLayouts[i];
  for (UINT i = 0; i < ci->pushConstantRangeCount; ++i) l->push_bytes = MAX(l->push_bytes, ci->pPushConstantRanges[i].offset + ci->pPushConstantRanges[i].size);
  *out = (VkPipelineLayout)l;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  vk_free(layout);
}

typedef struct DescPool {
  DescSet **sets;
  UINT n, cap;
} DescPool;

VKAPI VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDescriptorPool *out) {
  (void)device, (void)ci, (void)alloc;
  DescPool *p = vk_alloc(sizeof(DescPool));
  if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
  *out = (VkDescriptorPool)p;
  return VK_SUCCESS;
}
static void free_set(DescSet *s) {
  if (!s) return;
  vk_free(s->first);
  vk_free(s->d);
  vk_free(s);
}
VKAPI VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
  DescPool *p = (DescPool *)pool;
  (void)device, (void)flags;
  for (UINT i = 0; i < p->n; ++i) free_set(p->sets[i]);
  p->n = 0;
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *alloc) {
  DescPool *p = (DescPool *)pool;
  (void)alloc;
  if (!p) return;
  vkResetDescriptorPool(device, pool, 0);
  vk_free(p->sets);
  vk_free(p);
}
VKAPI VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
  DescPool *p = (DescPool *)ai->descriptorPool;
  for (UINT i = 0; i < ai->descriptorSetCount; ++i) {
    DSetLayout *l = (DSetLayout *)ai->pSetLayouts[i];
    DescSet *s = vk_alloc(sizeof(DescSet));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->dev = (Device *)device, s->layout = l;
    s->first = vk_alloc((l->nb ? l->nb : 1) * sizeof(UINT));
    UINT total = 0;
    for (UINT b = 0; b < l->nb; ++b) {
      if (s->first) s->first[b] = total;
      total += l->b[b].count;
    }
    s->nd = total;
    s->d = vk_alloc((total ? total : 1) * sizeof(Descr));
    if (!s->first || !s->d || !LIST_PUSH(p->sets, p->n, p->cap, s)) {
      free_set(s);
      for (UINT k = 0; k < i; ++k) out[k] = VK_NULL_HANDLE;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    out[i] = (VkDescriptorSet)s;
  }
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *sets) {
  DescPool *p = (DescPool *)pool;
  (void)device;
  for (UINT i = 0; i < count; ++i)
    for (UINT k = 0; k < p->n; ++k)
      if (p->sets[k] == (DescSet *)sets[i]) {
        free_set(p->sets[k]);
        p->sets[k] = p->sets[--p->n];
        break;
      }
  return VK_SUCCESS;
}

VKAPI void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t nwrites, const VkWriteDescriptorSet *writes, uint32_t ncopies, const VkCopyDescriptorSet *copies) {
  (void)device;
  for (UINT i = 0; i < nwrites; ++i) {
    const VkWriteDescriptorSet *w = &writes[i];
    DescSet *s = (DescSet *)w->dstSet;
    int bi = layout_binding_index(s->layout, w->dstBinding);
    if (bi < 0) continue;
    UINT element = w->dstArrayElement;
    for (UINT k = 0; k < w->descriptorCount; ++k, ++element) {
      /* (descriptors past the end of a binding go on into the next binding: Vulkan's consecutive binding updates) */
      while (bi >= 0 && element >= s->layout->b[bi].count) {
        element -= s->layout->b[bi].count;
        bi = next_binding_index(s->layout, s->layout->b[bi].binding);
      }
      if (bi < 0) break;
      UINT index = s->first[bi] + element;
      if (index >= s->nd) break;
      Descr *d = &s->d[index];
      if (w->pBufferInfo && (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                             w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
        const VkDescriptorBufferInfo *bi2 = &w->pBufferInfo[k];
        d->buf = (Buf *)bi2->buffer, d->off = bi2->offset;
        d->range = bi2->range == VK_WHOLE_SIZE ? (d->buf ? d->buf->size - bi2->offset : 0) : bi2->range;
      } else if (w->pImageInfo) {
        const VkDescriptorImageInfo *ii = &w->pImageInfo[k];
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) d->sampler = (Sampler *)ii->sampler;
        if (w->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) d->view = (View *)ii->imageView;
      }
    }
  }
  for (UINT i = 0; i < ncopies; ++i) {
    const VkCopyDescriptorSet *c = &copies[i];
    DescSet *src = (DescSet *)c->srcSet, *dst = (DescSet *)c->dstSet;
    int sb = layout_binding_index(src->layout, c->srcBinding), db = layout_binding_index(dst->layout, c->dstBinding);
    if (sb < 0 || db < 0) continue;
    for (UINT k = 0; k < c->descriptorCount; ++k) {
      UINT si = src->first[sb] + c->srcArrayElement + k, di = dst->first[db] + c->dstArrayElement + k;
      if (si < src->nd && di < dst->nd) dst->d[di] = src->d[si];
    }
  }
}

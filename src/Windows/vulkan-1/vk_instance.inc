/* vulkan-1.c, part 2: the kernel's allocations, the instance, the physical devices, the device and its queue. */

/* ---- allocations of the Direct3D kernel layer -------------------------------------------------------------------------------------------------------------------- */
enum { AW_VERSION = 0, AW_FLAGS, AW_FORMAT, AW_WIDTH, AW_HEIGHT, AW_MIPS, AW_SIZE_LO, AW_SIZE_HI, AW_KIND, AW_SAMPLES, AW_LAYERS, AW_STATE, AW_HEAP, AW_HEAP_LO, AW_HEAP_HI, AW_PITCH, AW_BYTES_LO,
       AW_BYTES_HI, AW_WORDS };

static BOOL kmt_create_allocation(Device *d, const UINT *words, UINT *pitch_out, KMT_HANDLE *out) {
  UINT data[AW_WORDS];
  memcpy(data, words, AW_WORDS * 4);
  BYTE info[40], args[72];
  memset(info, 0, sizeof info);
  memset(args, 0, sizeof args);
  put64(info, 16, (ULONGLONG)(ULONG_PTR)data);
  put32(info, 24, sizeof data);
  put32(args, 0, d->dev);
  put32(args, 44, 1);
  put64(args, 48, (ULONGLONG)(ULONG_PTR)info);
  put32(args, 56, 0);
  if (!STATUS_OK(D3DKMTCreateAllocation(args))) return FALSE;
  *out = get32(info, 0);
  if (pitch_out) *pitch_out = data[AW_PITCH];
  return *out != 0;
}

static BOOL kmt_make_buffer(Device *d, ULONGLONG size, UINT flags, KMT_HANDLE *out) {
  UINT w[AW_WORDS];
  memset(w, 0, sizeof w);
  w[AW_VERSION] = 1;
  w[AW_FLAGS] = flags;
  w[AW_SIZE_LO] = (UINT)size;
  w[AW_SIZE_HI] = (UINT)(size >> 32);
  w[AW_MIPS] = 1;
  w[AW_LAYERS] = 1;
  w[AW_SAMPLES] = 1;
  return kmt_create_allocation(d, w, NULL, out);
}

static void kmt_free(Device *d, KMT_HANDLE a) {
  if (!a) return;
  BYTE args[32];
  KMT_HANDLE list[1] = {a};
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put64(args, 8, (ULONGLONG)(ULONG_PTR)list);
  put32(args, 16, 1);
  D3DKMTDestroyAllocation(args);
}

/* The CPU's view of one level of an allocation (until kmt_unlock). */
static BYTE *kmt_lock(Device *d, KMT_HANDLE a, UINT level, UINT flags) {
  BYTE args[48];
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put32(args, 4, a);
  put32(args, 8, level);
  put32(args, 32, flags);
  if (!STATUS_OK(D3DKMTLock(args))) return NULL;
  return (BYTE *)(ULONG_PTR)get64(args, 24);
}
static void kmt_unlock(Device *d, KMT_HANDLE a) {
  BYTE args[16];
  KMT_HANDLE list[1] = {a};
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put32(args, 4, 1);
  put64(args, 8, (ULONGLONG)(ULONG_PTR)list);
  D3DKMTUnlock(args);
}

enum { LOCK_READ_ONLY = 1, LOCK_DISCARD = 0x80 };

static NTSTATUS kmt_escape(Device *d, void *data, UINT bytes) {
  BYTE args[32];
  memset(args, 0, sizeof args);
  put32(args, 0, d->pd->adapter);
  put32(args, 4, d->dev);
  put32(args, 8, 0);
  put64(args, 16, (ULONGLONG)(ULONG_PTR)data);
  put32(args, 24, bytes);
  put32(args, 28, d->ctx);
  return D3DKMTEscape(args);
}

/* ---- the instance ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static const VkExtensionProperties kInstanceExtensions[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
};
static const VkExtensionProperties kDeviceExtensions[] = {
    {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION},
};

static VkResult fill_extensions(const VkExtensionProperties *list, UINT n, uint32_t *count, VkExtensionProperties *out) {
  if (!out) {
    *count = n;
    return VK_SUCCESS;
  }
  UINT m = MIN(*count, n);
  for (UINT i = 0; i < m; ++i) out[i] = list[i];
  *count = m;
  return m < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props) {
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  return fill_extensions(kInstanceExtensions, sizeof kInstanceExtensions / sizeof kInstanceExtensions[0], count, props);
}
VKAPI VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
  (void)props;
  *count = 0;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {
  *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *props) {
  (void)pd;
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  return fill_extensions(kDeviceExtensions, sizeof kDeviceExtensions / sizeof kDeviceExtensions[0], count, props);
}
VKAPI VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice pd, uint32_t *count, VkLayerProperties *props) {
  (void)pd, (void)props;
  *count = 0;
  return VK_SUCCESS;
}

static BOOL has_extension(const char *const *names, UINT n, const VkExtensionProperties *list, UINT nlist) {
  for (UINT i = 0; i < n; ++i) {
    BOOL found = FALSE;
    for (UINT j = 0; j < nlist; ++j) found |= streq(names[i], list[j].extensionName);
    if (!found) return FALSE;
  }
  return TRUE;
}

static void load_adapters(Instance *in) {
  BYTE args[16];
  memset(args, 0, sizeof args);
  if (!STATUS_OK(D3DKMTEnumAdapters2(args))) return;
  UINT n = get32(args, 0);
  if (n == 0 || n > 16) return;
  BYTE *infos = vk_alloc(n * 20);
  if (!infos) return;
  put64(args, 8, (ULONGLONG)(ULONG_PTR)infos);
  if (!STATUS_OK(D3DKMTEnumAdapters2(args))) {
    vk_free(infos);
    return;
  }
  n = MIN(get32(args, 0), n);
  in->pd = vk_alloc(sizeof(PhysDev) * n);
  for (UINT i = 0; in->pd && i < n; ++i) {
    PhysDev *pd = &in->pd[in->npd];
    pd->h.magic = MAGIC_PHYS;
    pd->inst = in;
    pd->adapter = get32(infos + i * 20, 0);
    memcpy(&pd->luid, infos + i * 20 + 4, 8);
    BYTE query[24];
    BYTE blob[128];
    memset(query, 0, sizeof query);
    memset(blob, 0, sizeof blob);
    put32(query, 0, pd->adapter);
    put32(query, 4, 0);
    put64(query, 8, (ULONGLONG)(ULONG_PTR)blob);
    put32(query, 16, sizeof blob);
    if (!STATUS_OK(D3DKMTQueryAdapterInfo(query))) {
      put32(query, 16, 96);   /* (a kernel that has the capabilities blob without its tail) */
      if (!STATUS_OK(D3DKMTQueryAdapterInfo(query))) continue;
    }
    UINT *w = (UINT *)blob;
    pd->caps.max_texture_dimension = w[0], pd->caps.max_vertex_attributes = w[1], pd->caps.max_varyings = w[2], pd->caps.max_texture_units = w[3];
    pd->caps.max_push_constant_bytes = w[4], pd->caps.max_vertex_stride = w[5], pd->caps.flags = w[6], pd->caps.features = w[7];
    memcpy(pd->caps.name, blob + 32, 64);
    pd->caps.name[63] = 0;
    pd->caps.sample_counts = w[26];
    pd->caps.device_kind = w[27];
    pd->caps.vendor_id = w[28] ? w[28] : 0x10005;
    pd->caps.device_id = w[29];
    BYTE seg[24];
    memset(query, 0, sizeof query);
    memset(seg, 0, sizeof seg);
    put32(query, 0, pd->adapter);
    put32(query, 4, 3);
    put64(query, 8, (ULONGLONG)(ULONG_PTR)seg);
    put32(query, 16, sizeof seg);
    pd->local_memory = STATUS_OK(D3DKMTQueryAdapterInfo(query)) ? get64(seg, 0) : 0;
    if (pd->local_memory == 0) pd->local_memory = 256ull << 20;
    ++in->npd;
  }
  vk_free(infos);
}

VKAPI VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
  (void)alloc;
  if (ci->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
  if (!has_extension(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, kInstanceExtensions, sizeof kInstanceExtensions / sizeof kInstanceExtensions[0])) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (ci->pApplicationInfo && VK_VERSION_MAJOR(ci->pApplicationInfo->apiVersion) > 1) return VK_ERROR_INCOMPATIBLE_DRIVER;
  Instance *in = vk_alloc(sizeof(Instance));
  if (!in) return VK_ERROR_OUT_OF_HOST_MEMORY;
  in->h.magic = MAGIC_INSTANCE;
  in->api_version = VK_API_VERSION_1_0;
  load_adapters(in);
  *out = (VkInstance)in;
  return VK_SUCCESS;
}

VKAPI void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
  Instance *in = (Instance *)instance;
  (void)alloc;
  if (!in) return;
  for (UINT i = 0; i < in->npd; ++i) {
    BYTE a[4];
    put32(a, 0, in->pd[i].adapter);
    D3DKMTCloseAdapter(a);
  }
  vk_free(in->pd);
  vk_free(in);
}

VKAPI VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *out) {
  Instance *in = (Instance *)instance;
  if (!out) {
    *count = in->npd;
    return VK_SUCCESS;
  }
  UINT n = MIN(*count, in->npd);
  for (UINT i = 0; i < n; ++i) out[i] = (VkPhysicalDevice)&in->pd[i];
  *count = n;
  return n < in->npd ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- what a physical device is ---------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *p) {
  const PhysDev *pd = (const PhysDev *)physical;
  const Caps *c = &pd->caps;
  memset(p, 0, sizeof *p);
  p->apiVersion = VK_API_VERSION_1_0;
  p->driverVersion = 1;
  p->vendorID = c->vendor_id;
  p->deviceID = c->device_id;
  p->deviceType = c->device_kind == 0 ? VK_PHYSICAL_DEVICE_TYPE_CPU : c->device_kind == 1 ? VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  copy_string(p->deviceName, sizeof p->deviceName, c->name);
  for (int i = 0; i < 16; ++i) p->pipelineCacheUUID[i] = (BYTE)(0x54 + i + (pd->luid.LowPart & 0xF));
  VkPhysicalDeviceLimits *l = &p->limits;
  UINT dim = c->max_texture_dimension ? c->max_texture_dimension : 4096;
  l->maxImageDimension1D = dim, l->maxImageDimension2D = dim, l->maxImageDimension3D = 256, l->maxImageDimensionCube = dim;
  l->maxImageArrayLayers = 256;
  l->maxTexelBufferElements = 65536;
  l->maxUniformBufferRange = c->max_push_constant_bytes;
  l->maxStorageBufferRange = 128u << 20;
  l->maxPushConstantsSize = c->max_push_constant_bytes;
  l->maxMemoryAllocationCount = 4096;
  l->maxSamplerAllocationCount = 4000;
  l->bufferImageGranularity = 1;
  l->maxBoundDescriptorSets = 8;
  l->maxPerStageDescriptorSamplers = c->max_texture_units, l->maxPerStageDescriptorUniformBuffers = 12, l->maxPerStageDescriptorSampledImages = c->max_texture_units;
  l->maxPerStageResources = 128;
  l->maxDescriptorSetSamplers = c->max_texture_units, l->maxDescriptorSetUniformBuffers = 12, l->maxDescriptorSetUniformBuffersDynamic = 8, l->maxDescriptorSetSampledImages = c->max_texture_units;
  l->maxVertexInputAttributes = MIN(c->max_vertex_attributes, 8);
  l->maxVertexInputBindings = 8;
  l->maxVertexInputAttributeOffset = 2047;
  l->maxVertexInputBindingStride = 2048;
  l->maxVertexOutputComponents = 4 * c->max_varyings + 4;
  l->maxFragmentInputComponents = 4 * c->max_varyings + 4;
  l->maxFragmentOutputAttachments = (c->features & FEAT_MULTIPLE_TARGETS) ? 4 : 1;
  l->maxFragmentDualSrcAttachments = 0;
  l->maxFragmentCombinedOutputResources = 4;
  l->maxComputeSharedMemorySize = 0;
  l->maxDrawIndexedIndexValue = 0xFFFFFFFFu;
  l->maxDrawIndirectCount = 1;
  l->maxSamplerLodBias = 2.0f, l->maxSamplerAnisotropy = 1.0f;
  l->maxViewports = 1;
  l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = dim;
  l->viewportBoundsRange[0] = -2.0f * dim, l->viewportBoundsRange[1] = 2.0f * dim - 1;
  l->minMemoryMapAlignment = 64;
  l->minTexelBufferOffsetAlignment = 16, l->minUniformBufferOffsetAlignment = 16, l->minStorageBufferOffsetAlignment = 16;
  l->minTexelOffset = -8, l->maxTexelOffset = 7;
  l->maxFramebufferWidth = dim, l->maxFramebufferHeight = dim, l->maxFramebufferLayers = 1;
  UINT counts = VK_SAMPLE_COUNT_1_BIT;
  for (UINT s = 2; s <= 8; s <<= 1)
    if ((c->features & FEAT_MULTISAMPLE) && ((c->sample_counts >> s) & 1)) counts |= s;
  l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = l->framebufferNoAttachmentsSampleCounts = counts;
  l->sampledImageColorSampleCounts = l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->sampledImageIntegerSampleCounts = l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->maxColorAttachments = l->maxFragmentOutputAttachments;
  l->maxSampleMaskWords = 1;
  l->timestampComputeAndGraphics = VK_FALSE;
  l->timestampPeriod = 1.0f;
  l->maxClipDistances = 0, l->maxCullDistances = 0, l->maxCombinedClipAndCullDistances = 0;
  l->discreteQueuePriorities = 1;
  l->pointSizeRange[0] = 1.0f, l->pointSizeRange[1] = 1.0f;
  l->lineWidthRange[0] = 1.0f, l->lineWidthRange[1] = 1.0f;
  l->optimalBufferCopyOffsetAlignment = 4, l->optimalBufferCopyRowPitchAlignment = 4;
  l->nonCoherentAtomSize = 64;
  l->maxBoundDescriptorSets = 8;
  l->standardSampleLocations = VK_TRUE;
  l->strictLines = VK_FALSE;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *f) {
  const PhysDev *pd = (const PhysDev *)physical;
  memset(f, 0, sizeof *f);
  f->independentBlend = (pd->caps.features & FEAT_INDEPENDENT_BLEND) != 0;
  f->fillModeNonSolid = VK_FALSE;
  f->depthClamp = VK_FALSE;
  f->samplerAnisotropy = VK_FALSE;
  f->fragmentStoresAndAtomics = VK_FALSE;
  f->largePoints = VK_FALSE;
  f->robustBufferAccess = VK_FALSE;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties *p) {
  (void)physical;
  if (!p) {
    *count = 1;
    return;
  }
  if (*count < 1) return;
  memset(&p[0], 0, sizeof p[0]);
  p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
  p[0].queueCount = 1;
  p[0].timestampValidBits = 0;
  p[0].minImageTransferGranularity.width = p[0].minImageTransferGranularity.height = p[0].minImageTransferGranularity.depth = 1;
  *count = 1;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *p) {
  const PhysDev *pd = (const PhysDev *)physical;
  memset(p, 0, sizeof *p);
  p->memoryHeapCount = 1;
  p->memoryHeaps[0].size = pd->local_memory;
  p->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
  p->memoryTypeCount = 3;
  p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  p->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}

static VkFormatFeatureFlags image_features(const PhysDev *pd, VkFormat f, BOOL linear) {
  const FormatInfo *fi = format_info(pd, f);
  if (!fi) return 0;
  VkFormatFeatureFlags r = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if (format_is_depth(f)) return linear ? 0 : (r | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  if (!(pd->caps.flags & CAP_TEXTURES)) return linear ? 0 : r | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  r |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  BOOL integer = fi->dfmt == DF_RGBA8UI || fi->dfmt == DF_RGBA16UI || fi->dfmt == DF_RGBA16SI || fi->dfmt == DF_RGBA32UI || fi->dfmt == DF_RGBA32SI;
  if (!integer && (fi->bpp <= 8 || (pd->caps.features & FEAT_FLOAT_FORMATS))) r |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if (!linear) {
    r |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (!integer) r |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
  }
  return r;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *p) {
  const PhysDev *pd = (const PhysDev *)physical;
  memset(p, 0, sizeof *p);
  p->optimalTilingFeatures = image_features(pd, format, FALSE);
  p->linearTilingFeatures = image_features(pd, format, TRUE) & (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  if (vertex_format(format)) p->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VKAPI VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physical, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
                                                                   VkImageFormatProperties *p) {
  const PhysDev *pd = (const PhysDev *)physical;
  memset(p, 0, sizeof *p);
  VkFormatFeatureFlags have = image_features(pd, format, tiling == VK_IMAGE_TILING_LINEAR);
  VkFormatFeatureFlags need = 0;
  if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if ((have & need) != need || !have || type != VK_IMAGE_TYPE_2D || (flags & ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  UINT dim = pd->caps.max_texture_dimension ? pd->caps.max_texture_dimension : 4096;
  p->maxExtent.width = p->maxExtent.height = dim;
  p->maxExtent.depth = 1;
  p->maxMipLevels = 12;
  p->maxArrayLayers = 1;
  p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  p->maxResourceSize = 1ull << 30;
  return VK_SUCCESS;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physical, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                                                     VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *p) {
  (void)physical, (void)format, (void)type, (void)samples, (void)usage, (void)tiling, (void)p;
  *count = 0;
}

/* ---- the device and its queue ----------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
  PhysDev *pd = (PhysDev *)physical;
  (void)alloc;
  if (!has_extension(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, kDeviceExtensions, sizeof kDeviceExtensions / sizeof kDeviceExtensions[0])) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (ci->pEnabledFeatures) {
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(physical, &have);
    const VkBool32 *want = (const VkBool32 *)ci->pEnabledFeatures, *got = (const VkBool32 *)&have;
    for (UINT i = 0; i < sizeof have / sizeof(VkBool32); ++i)
      if (want[i] && !got[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  for (UINT i = 0; i < ci->queueCreateInfoCount; ++i)
    if (ci->pQueueCreateInfos[i].queueFamilyIndex != 0 || ci->pQueueCreateInfos[i].queueCount > 1) return VK_ERROR_INITIALIZATION_FAILED;
  Device *d = vk_alloc(sizeof(Device));
  if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
  d->h.magic = MAGIC_DEVICE;
  d->pd = pd;
  BYTE open[16];
  memset(open, 0, sizeof open);
  BYTE args[64];
  memset(args, 0, sizeof args);
  put32(args, 0, pd->adapter);
  NTSTATUS st = D3DKMTCreateDevice(args);
  if (!STATUS_OK(st)) {
    trace_status("D3DKMTCreateDevice", (UINT)st);
    vk_free(d);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  d->dev = get32(args, 12);
  BYTE cargs[96];
  memset(cargs, 0, sizeof cargs);
  put32(cargs, 0, d->dev);
  st = D3DKMTCreateContext(cargs);
  if (!STATUS_OK(st)) {
    trace_status("D3DKMTCreateContext", (UINT)st);
    BYTE a[4];
    put32(a, 0, d->dev);
    D3DKMTDestroyDevice(a);
    vk_free(d);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  d->ctx = get32(cargs, 32);
  d->dma = (BYTE *)(ULONG_PTR)get64(cargs, 40);
  d->dma_bytes = get32(cargs, 48);
  d->alloc_list = (BYTE *)(ULONG_PTR)get64(cargs, 56);
  d->alloc_entries = get32(cargs, 64);
  d->queue = vk_alloc(sizeof(Queue));
  if (!d->queue || !d->dma || d->dma_bytes < 4096) {
    trace_status("the context has no command buffer", d->dma_bytes);
    BYTE a[4];
    put32(a, 0, d->ctx);
    D3DKMTDestroyContext(a);
    put32(a, 0, d->dev);
    D3DKMTDestroyDevice(a);
    vk_free(d->queue);
    vk_free(d);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  d->queue->h.magic = MAGIC_QUEUE;
  d->queue->dev = d;
  *out = (VkDevice)d;
  return VK_SUCCESS;
}

VKAPI void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
  Device *d = (Device *)device;
  (void)alloc;
  if (!d) return;
  BYTE a[4];
  put32(a, 0, d->ctx);
  D3DKMTDestroyContext(a);
  put32(a, 0, d->dev);
  D3DKMTDestroyDevice(a);
  vk_free(d->queue);
  vk_free(d);
}

VKAPI void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
  Device *d = (Device *)device;
  *queue = (family == 0 && index == 0) ? (VkQueue)d->queue : NULL;
}
VKAPI VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  (void)device;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  (void)queue;
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *binds, VkFence fence) {
  (void)queue, (void)count, (void)binds, (void)fence;
  return VK_ERROR_FEATURE_NOT_PRESENT;
}

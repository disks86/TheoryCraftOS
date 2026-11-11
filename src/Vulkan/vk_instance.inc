/* The Vulkan driver, part 2: the kernel's allocations, the instance, the physical devices, the device and its queue. */

/* ---- allocations of the Direct3D kernel layer -------------------------------------------------------------------------------------------------------------------- */
enum { AW_VERSION = 0, AW_FLAGS, AW_FORMAT, AW_WIDTH, AW_HEIGHT, AW_MIPS, AW_SIZE_LO, AW_SIZE_HI, AW_KIND, AW_SAMPLES, AW_LAYERS, AW_STATE, AW_HEAP, AW_HEAP_LO, AW_HEAP_HI, AW_PITCH, AW_BYTES_LO,
       AW_BYTES_HI, AW_WORDS };

static bool kmt_create_allocation(Device *d, const uint32_t *words, uint32_t *pitch_out, KMT_HANDLE *out) {
  uint32_t data[AW_WORDS];
  memcpy(data, words, AW_WORDS * 4);
  uint8_t info[40], args[72];
  memset(info, 0, sizeof info);
  memset(args, 0, sizeof args);
  put64(info, 16, (uint64_t)(uintptr_t)data);
  put32(info, 24, sizeof data);
  put32(args, 0, d->dev);
  put32(args, 44, 1);
  put64(args, 48, (uint64_t)(uintptr_t)info);
  put32(args, 56, 0);
  if (!STATUS_OK(kmt(TCOS_DXG_CREATE_ALLOCATION, args))) return false;
  *out = get32(info, 0);
  if (pitch_out) *pitch_out = data[AW_PITCH];
  return *out != 0;
}

static bool kmt_make_buffer(Device *d, uint64_t size, uint32_t flags, KMT_HANDLE *out) {
  uint32_t w[AW_WORDS];
  memset(w, 0, sizeof w);
  w[AW_VERSION] = 1;
  w[AW_FLAGS] = flags;
  w[AW_SIZE_LO] = (uint32_t)size;
  w[AW_SIZE_HI] = (uint32_t)(size >> 32);
  w[AW_MIPS] = 1;
  w[AW_LAYERS] = 1;
  w[AW_SAMPLES] = 1;
  return kmt_create_allocation(d, w, NULL, out);
}

static void kmt_free(Device *d, KMT_HANDLE a) {
  if (!a) return;
  uint8_t args[32];
  KMT_HANDLE list[1] = {a};
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put64(args, 8, (uint64_t)(uintptr_t)list);
  put32(args, 16, 1);
  kmt(TCOS_DXG_DESTROY_ALLOCATION, args);
}

/* The CPU's view of one level of an allocation (until kmt_unlock). */
static uint8_t *kmt_lock(Device *d, KMT_HANDLE a, uint32_t level, uint32_t flags) {
  uint8_t args[48];
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put32(args, 4, a);
  put32(args, 8, level);
  put32(args, 32, flags);
  if (!STATUS_OK(kmt(TCOS_DXG_LOCK, args))) return NULL;
  return (uint8_t *)(uintptr_t)get64(args, 24);
}
static void kmt_unlock(Device *d, KMT_HANDLE a) {
  uint8_t args[16];
  KMT_HANDLE list[1] = {a};
  memset(args, 0, sizeof args);
  put32(args, 0, d->dev);
  put32(args, 4, 1);
  put64(args, 8, (uint64_t)(uintptr_t)list);
  kmt(TCOS_DXG_UNLOCK, args);
}

enum { LOCK_READ_ONLY = 1, LOCK_DISCARD = 0x80 };

static KmtStatus kmt_escape(Device *d, void *data, uint32_t bytes) {
  uint8_t args[32];
  memset(args, 0, sizeof args);
  put32(args, 0, d->pd->adapter);
  put32(args, 4, d->dev);
  put32(args, 8, 0);
  put64(args, 16, (uint64_t)(uintptr_t)data);
  put32(args, 24, bytes);
  put32(args, 28, d->ctx);
  return kmt(TCOS_DXG_ESCAPE, args);
}

/* ---- the instance ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static const VkExtensionProperties kInstanceExtensions[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    VK_OS_INSTANCE_EXTENSIONS   /* the platform's surface kinds (vk_os.h) */
};
static const VkExtensionProperties kDeviceExtensions[] = {
    {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION},
};

static VkResult fill_extensions(const VkExtensionProperties *list, uint32_t n, uint32_t *count, VkExtensionProperties *out) {
  if (!out) {
    *count = n;
    return VK_SUCCESS;
  }
  uint32_t m = MIN(*count, n);
  for (uint32_t i = 0; i < m; ++i) out[i] = list[i];
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

static bool has_extension(const char *const *names, uint32_t n, const VkExtensionProperties *list, uint32_t nlist) {
  for (uint32_t i = 0; i < n; ++i) {
    bool found = false;
    for (uint32_t j = 0; j < nlist; ++j) found |= streq(names[i], list[j].extensionName);
    if (!found) return false;
  }
  return true;
}

static void load_adapters(Instance *in) {
  uint8_t args[16];
  memset(args, 0, sizeof args);
  if (!STATUS_OK(kmt(TCOS_DXG_ENUM_ADAPTERS2, args))) return;
  uint32_t n = get32(args, 0);
  if (n == 0 || n > 16) return;
  uint8_t *infos = vk_alloc(n * 20);
  if (!infos) return;
  put64(args, 8, (uint64_t)(uintptr_t)infos);
  if (!STATUS_OK(kmt(TCOS_DXG_ENUM_ADAPTERS2, args))) {
    vk_free(infos);
    return;
  }
  n = MIN(get32(args, 0), n);
  in->pd = vk_alloc(sizeof(PhysDev) * n);
  for (uint32_t i = 0; in->pd && i < n; ++i) {
    PhysDev *pd = &in->pd[in->npd];
    pd->h.magic = MAGIC_PHYS;
    pd->inst = in;
    pd->adapter = get32(infos + i * 20, 0);
    memcpy(&pd->luid, infos + i * 20 + 4, 8);
    uint8_t query[24];
    uint8_t blob[128];
    memset(query, 0, sizeof query);
    memset(blob, 0, sizeof blob);
    put32(query, 0, pd->adapter);
    put32(query, 4, 0);
    put64(query, 8, (uint64_t)(uintptr_t)blob);
    put32(query, 16, sizeof blob);
    if (!STATUS_OK(kmt(TCOS_DXG_QUERY_ADAPTER_INFO, query))) {
      put32(query, 16, 96);   /* (a kernel that has the capabilities blob without its tail) */
      if (!STATUS_OK(kmt(TCOS_DXG_QUERY_ADAPTER_INFO, query))) continue;
    }
    uint32_t *w = (uint32_t *)blob;
    pd->caps.max_texture_dimension = w[0], pd->caps.max_vertex_attributes = w[1], pd->caps.max_varyings = w[2], pd->caps.max_texture_units = w[3];
    pd->caps.max_push_constant_bytes = w[4], pd->caps.max_vertex_stride = w[5], pd->caps.flags = w[6], pd->caps.features = w[7];
    memcpy(pd->caps.name, blob + 32, 64);
    pd->caps.name[63] = 0;
    pd->caps.sample_counts = w[26];
    pd->caps.device_kind = w[27];
    pd->caps.vendor_id = w[28] ? w[28] : 0x10005;
    pd->caps.device_id = w[29];
    uint8_t seg[24];
    memset(query, 0, sizeof query);
    memset(seg, 0, sizeof seg);
    put32(query, 0, pd->adapter);
    put32(query, 4, 3);
    put64(query, 8, (uint64_t)(uintptr_t)seg);
    put32(query, 16, sizeof seg);
    pd->local_memory = STATUS_OK(kmt(TCOS_DXG_QUERY_ADAPTER_INFO, query)) ? get64(seg, 0) : 0;
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
  for (uint32_t i = 0; i < in->npd; ++i) {
    uint8_t a[4];
    put32(a, 0, in->pd[i].adapter);
    kmt(TCOS_DXG_CLOSE_ADAPTER, a);
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
  uint32_t n = MIN(*count, in->npd);
  for (uint32_t i = 0; i < n; ++i) out[i] = (VkPhysicalDevice)&in->pd[i];
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
  for (int i = 0; i < 16; ++i) p->pipelineCacheUUID[i] = (uint8_t)(0x54 + i + (pd->luid.LowPart & 0xF));
  VkPhysicalDeviceLimits *l = &p->limits;
  uint32_t dim = c->max_texture_dimension ? c->max_texture_dimension : 4096;
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
  l->maxVertexInputAttributes = MIN(c->max_vertex_attributes, MAX_ATTRIBUTES);
  l->maxVertexInputBindings = 8;
  l->maxVertexInputAttributeOffset = 2047;
  l->maxVertexInputBindingStride = 2048;
  l->maxVertexOutputComponents = 4 * c->max_varyings + 4;
  l->maxFragmentInputComponents = 4 * c->max_varyings + 4;
  l->maxFragmentOutputAttachments = (c->features & FEAT_MULTIPLE_TARGETS) ? 4 : 1;
  l->maxFragmentDualSrcAttachments = 0;
  l->maxFragmentCombinedOutputResources = 4;
  /* compute (the kernel's: 32 KiB of group-shared memory, 65535 groups a dimension; 256 threads a group, which is what a group with barriers may have) */
  bool compute = (c->features & FEAT_COMPUTE) != 0;
  l->maxComputeSharedMemorySize = compute ? 32768 : 0;
  for (int i = 0; i < 3; ++i) l->maxComputeWorkGroupCount[i] = compute ? 65535 : 0;
  l->maxComputeWorkGroupInvocations = compute ? 256 : 0;
  l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = compute ? 256 : 0, l->maxComputeWorkGroupSize[2] = compute ? 64 : 0;
  /* storage buffers: four written and four read only at a time, in any stage that has them */
  bool storage = compute || (c->features & FEAT_STORAGE_IN_DRAWS);
  l->maxPerStageDescriptorStorageBuffers = storage ? UAV_SLOTS + SRV_SLOTS : 0;
  l->maxDescriptorSetStorageBuffers = storage ? UAV_SLOTS + SRV_SLOTS : 0;
  l->maxDescriptorSetStorageBuffersDynamic = storage ? 4 : 0;
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
  uint32_t counts = VK_SAMPLE_COUNT_1_BIT;
  for (uint32_t s = 2; s <= 8; s <<= 1)
    if ((c->features & FEAT_MULTISAMPLE) && ((c->sample_counts >> s) & 1)) counts |= s;
  l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = l->framebufferNoAttachmentsSampleCounts = counts;
  l->sampledImageColorSampleCounts = l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->sampledImageIntegerSampleCounts = l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->maxColorAttachments = l->maxFragmentOutputAttachments;
  l->maxSampleMaskWords = 1;
  l->timestampComputeAndGraphics = VK_TRUE;   /* (timestamp queries: the CPU's clock in nanoseconds, vk_resources.inc) */
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
  f->imageCubeArray = (pd->caps.features & FEAT_TEXTURE_KINDS) != 0;   /* (cubes are 2D arrays: vkCreateImage) */
  f->fillModeNonSolid = VK_FALSE;
  f->depthClamp = VK_FALSE;
  f->samplerAnisotropy = VK_FALSE;
  f->fragmentStoresAndAtomics = (pd->caps.features & FEAT_FRAGMENT_STORES) != 0;
  f->vertexPipelineStoresAndAtomics = (pd->caps.features & FEAT_VERTEX_STORES) != 0;
  f->largePoints = VK_FALSE;
  f->robustBufferAccess = VK_FALSE;
}

VKAPI void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties *p) {
  const PhysDev *pd = (const PhysDev *)physical;
  if (!p) {
    *count = 1;
    return;
  }
  if (*count < 1) return;
  memset(&p[0], 0, sizeof p[0]);
  p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT | ((pd->caps.features & FEAT_COMPUTE) ? VK_QUEUE_COMPUTE_BIT : 0);
  p[0].queueCount = 1;
  p[0].timestampValidBits = 64;
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

static VkFormatFeatureFlags image_features(const PhysDev *pd, VkFormat f, bool linear) {
  const FormatInfo *fi = format_info(pd, f);
  if (!fi) return 0;
  VkFormatFeatureFlags r = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if (format_is_depth(f)) return linear ? 0 : (r | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  if (!(pd->caps.flags & CAP_TEXTURES)) return linear ? 0 : r | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  r |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  bool integer = fi->dfmt == DF_RGBA8UI || fi->dfmt == DF_RGBA16UI || fi->dfmt == DF_RGBA16SI || fi->dfmt == DF_RGBA32UI || fi->dfmt == DF_RGBA32SI;
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
  p->optimalTilingFeatures = image_features(pd, format, false);
  p->linearTilingFeatures = image_features(pd, format, true) & (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
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
  uint32_t dim = pd->caps.max_texture_dimension ? pd->caps.max_texture_dimension : 4096;
  p->maxExtent.width = p->maxExtent.height = dim;
  p->maxExtent.depth = 1;
  p->maxMipLevels = 12;
  /* (arrays and cubes: sampled images of an 8-bit colour format, one level each - the kernel's Texture2DArray) */
  const FormatInfo *afi = format_info(pd, format);
  p->maxArrayLayers = (pd->caps.features & FEAT_TEXTURE_KINDS) && afi && (afi->dfmt == DF_A8R8G8B8 || afi->dfmt == DF_A8B8G8R8 || afi->dfmt == DF_A8R8G8B8_SRGB || afi->dfmt == DF_A8B8G8R8_SRGB) && !(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? 256 : 1;
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
    for (uint32_t i = 0; i < sizeof have / sizeof(VkBool32); ++i)
      if (want[i] && !got[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i)
    if (ci->pQueueCreateInfos[i].queueFamilyIndex != 0 || ci->pQueueCreateInfos[i].queueCount > 1) return VK_ERROR_INITIALIZATION_FAILED;
  Device *d = vk_alloc(sizeof(Device));
  if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
  d->h.magic = MAGIC_DEVICE;
  d->pd = pd;
  uint8_t open[16];
  memset(open, 0, sizeof open);
  uint8_t args[64];
  memset(args, 0, sizeof args);
  put32(args, 0, pd->adapter);
  KmtStatus st = kmt(TCOS_DXG_CREATE_DEVICE, args);
  if (!STATUS_OK(st)) {
    trace_status("D3DKMTCreateDevice", (uint32_t)st);
    vk_free(d);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  d->dev = get32(args, 12);
  uint8_t cargs[96];
  memset(cargs, 0, sizeof cargs);
  put32(cargs, 0, d->dev);
  st = kmt(TCOS_DXG_CREATE_CONTEXT, cargs);
  if (!STATUS_OK(st)) {
    trace_status("D3DKMTCreateContext", (uint32_t)st);
    uint8_t a[4];
    put32(a, 0, d->dev);
    kmt(TCOS_DXG_DESTROY_DEVICE, a);
    vk_free(d);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  d->ctx = get32(cargs, 32);
  d->dma = (uint8_t *)(uintptr_t)get64(cargs, 40);
  d->dma_bytes = get32(cargs, 48);
  d->alloc_list = (uint8_t *)(uintptr_t)get64(cargs, 56);
  d->alloc_entries = get32(cargs, 64);
  d->queue = vk_alloc(sizeof(Queue));
  if (!d->queue || !d->dma || d->dma_bytes < 4096) {
    trace_status("the context has no command buffer", d->dma_bytes);
    uint8_t a[4];
    put32(a, 0, d->ctx);
    kmt(TCOS_DXG_DESTROY_CONTEXT, a);
    put32(a, 0, d->dev);
    kmt(TCOS_DXG_DESTROY_DEVICE, a);
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
  uint8_t a[4];
  put32(a, 0, d->ctx);
  kmt(TCOS_DXG_DESTROY_CONTEXT, a);
  put32(a, 0, d->dev);
  kmt(TCOS_DXG_DESTROY_DEVICE, a);
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

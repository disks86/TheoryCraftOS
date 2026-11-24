/* The Venus backend's vkGetInstanceProcAddr and vkGetDeviceProcAddr (included by vk_dispatch.inc after the tables). A core function is found when the instance's
 * (or device's) version has it, an extension's when the extension is offered (instance) or enabled (device); the global functions without an instance. */
static const VnProc *vn_lookup(const VnProc *table, uint32_t n, const char *name) {
  for (uint32_t i = 0; i < n; ++i)
    if (streq(table[i].name, name)) return &table[i];
  return NULL;
}

static uint32_t vn_version_key(uint32_t api) { return VK_API_VERSION_MAJOR(api) << 8 | VK_API_VERSION_MINOR(api); }

static PFN_vkVoidFunction VKAPI_CALL vn_vkGetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction VKAPI_CALL vn_vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (!name) return NULL;
  if (streq(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vn_vkGetInstanceProcAddr;
  if (streq(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vn_vkGetDeviceProcAddr;
  static const char *const global[] = {"vkEnumerateInstanceVersion", "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties", "vkCreateInstance"};
  for (uint32_t i = 0; i < sizeof global / sizeof global[0]; ++i)
    if (streq(name, global[i])) {
      const VnProc *p = vn_lookup(kVnProcs, sizeof kVnProcs / sizeof kVnProcs[0], name);
      return p ? p->fn : NULL;
    }
  if (!instance) return NULL;
  const VnProc *p = vn_lookup(kVnProcs, sizeof kVnProcs / sizeof kVnProcs[0], name);
  if (!p) p = vn_lookup(kVnShared, sizeof kVnShared / sizeof kVnShared[0], name);
  if (!p) return NULL;
  if (!p->extension) return p->version <= vn_version_key(VN_API_VERSION) ? p->fn : NULL;
  /* (an extension's function: an instance extension this driver offers, or a device extension some physical device may offer) */
  if (vn_offered(kVnInstanceExtensions, VN_INSTANCE_EXTENSION_COUNT, p->extension) || streq(p->extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || vn_extension_allowed(p->extension))
    return p->fn;
  return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL vn_vkGetDeviceProcAddr(VkDevice device, const char *name) {
  if (!name || !device) return NULL;
  if (streq(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vn_vkGetDeviceProcAddr;
  const VnDevice *dev = vn_device(device);
  const VnProc *p = vn_lookup(kVnProcs, sizeof kVnProcs / sizeof kVnProcs[0], name);
  if (!p || !p->device_level) return NULL;
  if (!p->extension) return p->version <= vn_version_key(dev->api_version) ? p->fn : NULL;
  return vn_device_has(dev, p->extension) ? p->fn : NULL;
}

/* The exported lookups: the selected backend's answer, given as the exported entry point when the name is one (so a pointer from the lookup and the export of the
 * same name are the same function, as with a loader's trampolines). */
static PFN_vkVoidFunction vk_exported(const char *name, PFN_vkVoidFunction found) {
  if (!found) return NULL;
  for (uint32_t i = 0; i < sizeof kExportedProcs / sizeof kExportedProcs[0]; ++i)
    if (streq(kExportedProcs[i].name, name)) return kExportedProcs[i].fn;
  return found;
}

VKAPI PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name);

VKAPI PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (name && streq(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (name && streq(name, "vkGetDeviceProcAddr") && (instance || vn_selected())) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  return vk_exported(name, vn_selected() ? vn_vkGetInstanceProcAddr(instance, name) : dx_vkGetInstanceProcAddr(instance, name));
}

VKAPI PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
  if (name && streq(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  return vk_exported(name, vn_selected() ? vn_vkGetDeviceProcAddr(device, name) : dx_vkGetDeviceProcAddr(device, name));
}

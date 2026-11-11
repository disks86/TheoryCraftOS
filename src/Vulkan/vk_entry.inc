/* The Vulkan driver, part 7: finding the functions by name (the shared ones, then the platform's own: VK_OS_PROCS of vk_os.h). */

typedef struct ProcEntry {
  const char *name;
  PFN_vkVoidFunction fn;
} ProcEntry;

static const ProcEntry kProcs[] = {
#define X(n) {#n, (PFN_vkVoidFunction)n},
#include "vk_exports.inc"
    VK_OS_PROCS(X)
#undef X
};

static PFN_vkVoidFunction find_proc(const char *name) {
  if (!name) return NULL;
  for (uint32_t i = 0; i < sizeof kProcs / sizeof kProcs[0]; ++i)
    if (streq(kProcs[i].name, name)) return kProcs[i].fn;
  return NULL;
}

VKAPI PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  (void)instance;
  return find_proc(name);
}

VKAPI PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
  (void)device;
  return find_proc(name);
}

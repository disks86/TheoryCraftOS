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

/* vulkan-1.dll's Venus backend (docs/Venus.md) on the host's own Vulkan driver: the devices are the host's (driver id VK_DRIVER_ID_MESA_VENUS, the host driver
 * named in driverInfo), and on each one the parts of the backend that are more than forwarding: host-visible memory mapped into the program (written by the CPU,
 * copied by the GPU, read back), command buffers small and large (a large stream goes in shared memory of its own), fences and timeline semaphores waited for
 * by polling, vkQueueWaitIdle, a compute shader with its descriptors written through a descriptor update template, timestamps read with WAIT, an image cleared
 * and copied out, private data, vkGetDeviceProcAddr's answers, and the 1.1 - 1.3 entry points the dispatch layer exports.
 * Run by tests/venus_qemu.sh. Prints "venustest: N checks, M failed"; the exit code is the number of failures. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/Vulkan/khronos/vulkan/vulkan_core.h"
#include "../../src/Vulkan/khronos/vulkan/vulkan_win32.h"
#include "vulkantest_shaders.h"

static int g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL line %d: %s\n", line, what);
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance g_instance;
#define FN(name) static PFN_##name name;
#define FUNCTIONS(X)                                                                                                                                             \
  X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2)                      \
  X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties)   \
  X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkGetDeviceProcAddr) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)         \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool)                      \
  X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdCopyBuffer)                   \
  X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkCreateFence) X(vkDestroyFence)     \
  X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkSignalSemaphore)                  \
  X(vkGetSemaphoreCounterValue) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)                \
  X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)     \
  X(vkAllocateDescriptorSets) X(vkCreateDescriptorUpdateTemplate) X(vkDestroyDescriptorUpdateTemplate) X(vkUpdateDescriptorSetWithTemplate)                   \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool)      \
  X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                      \
  X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer) X(vkCreatePrivateDataSlot) X(vkDestroyPrivateDataSlot) X(vkSetPrivateData) X(vkGetPrivateData)              \
  X(vkQueueSubmit2) X(vkCmdPipelineBarrier2)
FUNCTIONS(FN)

static void load_all(void) {
#define LOAD(name) name = (PFN_##name)gipa(g_instance, #name);
  FUNCTIONS(LOAD)
#undef LOAD
}

typedef struct Rig {
  VkPhysicalDevice pd;
  VkDevice dev;
  VkQueue queue;
  uint32_t family;
  VkCommandPool pool;
  VkPhysicalDeviceMemoryProperties memory;
  float timestamp_period;
  uint32_t timestamp_bits;
} Rig;

static uint32_t memory_type(const Rig *r, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < r->memory.memoryTypeCount; ++i)
    if (((bits >> i) & 1) && (r->memory.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

typedef struct Buf {
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *map;
} Buf;

static int make_buffer(Rig *r, VkDeviceSize size, VkBufferUsageFlags usage, Buf *b) {
  memset(b, 0, sizeof *b);
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size, bci.usage = usage;
  if (vkCreateBuffer(r->dev, &bci, NULL, &b->buffer) != VK_SUCCESS) return 0;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(r->dev, b->buffer, &req);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(r->dev, &mai, NULL, &b->memory) != VK_SUCCESS) return 0;
  if (vkBindBufferMemory(r->dev, b->buffer, b->memory, 0) != VK_SUCCESS) return 0;
  return vkMapMemory(r->dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map) == VK_SUCCESS && b->map;
}

static void free_buffer(Rig *r, Buf *b) {
  if (b->buffer) vkDestroyBuffer(r->dev, b->buffer, NULL);
  if (b->memory) vkFreeMemory(r->dev, b->memory, NULL);
}

static VkCommandBuffer begin(Rig *r) {
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = r->pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(r->dev, &cai, &cb);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  return cb;
}

/* Ends, submits with a fence, waits for it; true when all of that worked. */
static int run(Rig *r, VkCommandBuffer cb) {
  if (vkEndCommandBuffer(cb) != VK_SUCCESS) return 0;
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(r->dev, &fci, NULL, &fence);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &cb;
  int ok = vkQueueSubmit(r->queue, 1, &si, fence) == VK_SUCCESS && vkWaitForFences(r->dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS &&
           vkGetFenceStatus(r->dev, fence) == VK_SUCCESS;
  vkDestroyFence(r->dev, fence, NULL);
  vkFreeCommandBuffers(r->dev, r->pool, 1, &cb);
  return ok;
}

static void barrier_transfer_to_host(VkCommandBuffer cb) {
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                       NULL, 0, NULL);
}

static void test_memory(Rig *r) {
  Buf a, b;
  check(make_buffer(r, 65536, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &a), "a host-visible buffer is made and mapped");
  check(make_buffer(r, 65536, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b), "a second one");
  if (!a.map || !b.map) return;
  uint32_t *pa = a.map, *pb = b.map;
  for (uint32_t i = 0; i < 16384; ++i) pa[i] = 0x9E3779B9u * (i + 1), pb[i] = 0;
  /* the GPU copies what the CPU wrote, fills and updates parts of it */
  VkCommandBuffer cb = begin(r);
  VkBufferCopy copy = {0, 0, 65536};
  vkCmdCopyBuffer(cb, a.buffer, b.buffer, 1, &copy);
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
  vkCmdFillBuffer(cb, b.buffer, 1024, 256, 0xDEADBEEFu);
  const uint32_t words[4] = {1, 2, 3, 4};
  vkCmdUpdateBuffer(cb, b.buffer, 4096, sizeof words, words);
  barrier_transfer_to_host(cb);
  check(run(r, cb), "the copy runs");
  int same = 1;
  for (uint32_t i = 0; i < 16384; ++i) {
    uint32_t want = 0x9E3779B9u * (i + 1);
    if (i >= 256 && i < 320) want = 0xDEADBEEFu;
    if (i >= 1024 && i < 1028) want = i - 1023;
    if (pb[i] != want) same = 0;
  }
  check(same, "the GPU's copy, fill and update are seen through the mapping");
  /* a big command buffer: its stream (over 8 KiB) goes to the host in shared memory of its own */
  cb = begin(r);
  for (uint32_t i = 0; i < 2048; ++i) vkCmdFillBuffer(cb, b.buffer, (VkDeviceSize)(i % 1024) * 64, 64, i);
  barrier_transfer_to_host(cb);
  check(run(r, cb), "a command buffer of 2048 commands runs");
  int filled = 1;
  for (uint32_t k = 0; k < 1024; ++k)
    for (uint32_t w = 0; w < 16; ++w)
      if (pb[k * 16 + w] != k + 1024) filled = 0;   /* (the second fill of each range is the one that stays) */
  check(filled, "every fill of it landed");
  /* map, unmap and map again gives the same memory */
  vkUnmapMemory(r->dev, a.memory);
  void *again = NULL;
  check(vkMapMemory(r->dev, a.memory, 256, 1024, 0, &again) == VK_SUCCESS && again && ((uint32_t *)again)[0] == 0x9E3779B9u * 65, "a map at an offset");
  free_buffer(r, &a);
  free_buffer(r, &b);
}

static void test_sync(Rig *r) {
  /* vkQueueWaitIdle and vkDeviceWaitIdle with work in flight */
  Buf b;
  check(make_buffer(r, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b), "a buffer for the waits");
  VkCommandBuffer cb = begin(r);
  vkCmdFillBuffer(cb, b.buffer, 0, 4096, 0x12345678u);
  barrier_transfer_to_host(cb);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &cb;
  check(vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "a submission without a fence");
  check(vkQueueWaitIdle(r->queue) == VK_SUCCESS, "vkQueueWaitIdle");
  check(b.map && ((uint32_t *)b.map)[1023] == 0x12345678u, "the work is done when the queue is idle");
  check(vkDeviceWaitIdle(r->dev) == VK_SUCCESS, "vkDeviceWaitIdle");
  vkFreeCommandBuffers(r->dev, r->pool, 1, &cb);
  /* a fence that never signals times out; a signaled one is seen at once */
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence unsignaled, signaled;
  vkCreateFence(r->dev, &fci, NULL, &unsignaled);
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(r->dev, &fci, NULL, &signaled);
  check(vkWaitForFences(r->dev, 1, &unsignaled, VK_TRUE, 0) == VK_TIMEOUT, "an unsignaled fence: VK_TIMEOUT for a zero wait");
  check(vkWaitForFences(r->dev, 1, &unsignaled, VK_TRUE, 2000000) == VK_TIMEOUT, "and for a 2 ms wait");
  VkFence both[2] = {unsignaled, signaled};
  check(vkWaitForFences(r->dev, 2, both, VK_FALSE, 1000000) == VK_SUCCESS, "any of two: the signaled one is enough");
  check(vkWaitForFences(r->dev, 2, both, VK_TRUE, 0) == VK_TIMEOUT, "all of two: not yet");
  check(vkResetFences(r->dev, 1, &signaled) == VK_SUCCESS && vkGetFenceStatus(r->dev, signaled) == VK_NOT_READY, "a reset fence is unsignaled");
  vkDestroyFence(r->dev, unsignaled, NULL);
  vkDestroyFence(r->dev, signaled, NULL);
  /* a timeline semaphore: signaled from the host CPU and from a submission, waited for by value */
  VkSemaphoreTypeCreateInfo type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, type.initialValue = 1;
  VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sci.pNext = &type;
  VkSemaphore timeline = VK_NULL_HANDLE;
  check(vkCreateSemaphore(r->dev, &sci, NULL, &timeline) == VK_SUCCESS, "a timeline semaphore");
  uint64_t value = 0;
  check(vkGetSemaphoreCounterValue(r->dev, timeline, &value) == VK_SUCCESS && value == 1, "its initial value");
  VkSemaphoreSignalInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
  signal.semaphore = timeline, signal.value = 5;
  check(vkSignalSemaphore(r->dev, &signal) == VK_SUCCESS, "vkSignalSemaphore");
  uint64_t want = 5;
  VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = 1, wi.pSemaphores = &timeline, wi.pValues = &want;
  check(vkWaitSemaphores(r->dev, &wi, 1000000) == VK_SUCCESS, "waiting for a value it has");
  want = 9;
  check(vkWaitSemaphores(r->dev, &wi, 1000000) == VK_TIMEOUT, "waiting for one it does not: VK_TIMEOUT");
  VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  tsi.signalSemaphoreValueCount = 1, tsi.pSignalSemaphoreValues = &want;
  VkSubmitInfo s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  s2.pNext = &tsi, s2.signalSemaphoreCount = 1, s2.pSignalSemaphores = &timeline;
  check(vkQueueSubmit(r->queue, 1, &s2, VK_NULL_HANDLE) == VK_SUCCESS, "a submission that signals 9");
  check(vkWaitSemaphores(r->dev, &wi, 5000000000ull) == VK_SUCCESS, "and the wait for 9 ends");
  /* vkQueueSubmit2 (Vulkan 1.3) with a timeline value */
  VkSemaphoreSubmitInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  ss.semaphore = timeline, ss.value = 12, ss.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si2.signalSemaphoreInfoCount = 1, si2.pSignalSemaphoreInfos = &ss;
  if (vkQueueSubmit2) {
    check(vkQueueSubmit2(r->queue, 1, &si2, VK_NULL_HANDLE) == VK_SUCCESS, "vkQueueSubmit2");
    want = 12;
    check(vkWaitSemaphores(r->dev, &wi, 5000000000ull) == VK_SUCCESS, "its signal arrives");
  }
  vkDestroySemaphore(r->dev, timeline, NULL);
  free_buffer(r, &b);
}

static void test_compute(Rig *r) {
  Buf src, dst;
  const uint32_t count = 16;
  check(make_buffer(r, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &src) && make_buffer(r, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &dst), "storage buffers");
  if (!src.map || !dst.map) return;
  float *in = src.map;
  in[0] = 0.0f, in[1] = 0.5f;   /* offset (vec2) */
  for (uint32_t i = 0; i < count; ++i) in[2 + i] = (float)i;
  memset(dst.map, 0, 4096);
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof add_comp, smi.pCode = add_comp;
  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(r->dev, &smi, NULL, &module) == VK_SUCCESS, "the compute shader module");
  VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
  VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = 2, dli.pBindings = bindings;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(r->dev, &dli, NULL, &layout);
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1, pli.pSetLayouts = &layout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &range;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  vkCreatePipelineLayout(r->dev, &pli, NULL, &pl);
  VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cpi.stage.module = module, cpi.stage.pName = "main";
  cpi.layout = pl;
  VkPipeline pipeline = VK_NULL_HANDLE;
  check(vkCreateComputePipelines(r->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline) == VK_SUCCESS && pipeline, "the compute pipeline");
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1, dpi.poolSizeCount = 1, dpi.pPoolSizes = &ps;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  vkCreateDescriptorPool(r->dev, &dpi, NULL, &dpool);
  VkDescriptorSetAllocateInfo dsa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsa.descriptorPool = dpool, dsa.descriptorSetCount = 1, dsa.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  check(vkAllocateDescriptorSets(r->dev, &dsa, &set) == VK_SUCCESS && set, "a descriptor set");
  /* the descriptors through a template: two buffer infos in a struct of the program's */
  struct {
    VkDescriptorBufferInfo in, out;
  } data = {{src.buffer, 0, VK_WHOLE_SIZE}, {dst.buffer, 0, VK_WHOLE_SIZE}};
  VkDescriptorUpdateTemplateEntry entries[2] = {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, sizeof(VkDescriptorBufferInfo)},
                                                {1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(VkDescriptorBufferInfo), sizeof(VkDescriptorBufferInfo)}};
  VkDescriptorUpdateTemplateCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
  tci.descriptorUpdateEntryCount = 2, tci.pDescriptorUpdateEntries = entries, tci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
  tci.descriptorSetLayout = layout;
  VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
  check(vkCreateDescriptorUpdateTemplate(r->dev, &tci, NULL, &tmpl) == VK_SUCCESS && tmpl, "a descriptor update template");
  vkUpdateDescriptorSetWithTemplate(r->dev, set, tmpl, &data);
  VkCommandBuffer cb = begin(r);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
  const struct {
    float scale;
    uint32_t count;
  } pc = {2.0f, count};
  vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  vkCmdDispatch(cb, 2, 1, 1);
  barrier_transfer_to_host(cb);
  check(run(r, cb), "the dispatch runs");
  const uint32_t *out = dst.map;
  check(out[0] == 136, "the atomic total (1 + 2 + ... + 16)");
  int qs = 1;
  for (int i = 0; i < 8; ++i) {
    const int v = i - 5;
    const int floored = ((v % 3) + 3) % 3;   /* (glslc makes GLSL's % on ints OpSMod: the sign of the divisor) */
    if ((int)out[1 + i] != (v / 3) * 100 + floored) qs = 0;
  }
  check(qs, "signed division and remainder");
  const float *b = (const float *)(out + 9);
  int values = 1;
  for (uint32_t i = 0; i < count; ++i)
    if (b[i] != (float)i * 2.0f + 0.5f) values = 0;
  check(values, "every invocation's result");
  vkDestroyDescriptorUpdateTemplate(r->dev, tmpl, NULL);
  vkDestroyDescriptorPool(r->dev, dpool, NULL);
  vkDestroyPipeline(r->dev, pipeline, NULL);
  vkDestroyPipelineLayout(r->dev, pl, NULL);
  vkDestroyDescriptorSetLayout(r->dev, layout, NULL);
  vkDestroyShaderModule(r->dev, module, NULL);
  free_buffer(r, &src);
  free_buffer(r, &dst);
}

static void test_image_and_queries(Rig *r) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_R8G8B8A8_UNORM, ici.extent.width = 64, ici.extent.height = 32, ici.extent.depth = 1;
  ici.mipLevels = 1, ici.arrayLayers = 1, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage image = VK_NULL_HANDLE;
  check(vkCreateImage(r->dev, &ici, NULL, &image) == VK_SUCCESS, "an image");
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(r->dev, image, &req);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size, mai.memoryTypeIndex = memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = memory_type(r, req.memoryTypeBits, 0);
  VkDeviceMemory memory = VK_NULL_HANDLE;
  check(vkAllocateMemory(r->dev, &mai, NULL, &memory) == VK_SUCCESS && vkBindImageMemory(r->dev, image, memory, 0) == VK_SUCCESS, "its memory (device local)");
  void *no = NULL;
  if (!(r->memory.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    check(vkMapMemory(r->dev, memory, 0, VK_WHOLE_SIZE, 0, &no) == VK_ERROR_MEMORY_MAP_FAILED, "memory the host cannot see does not map");
  Buf out;
  check(make_buffer(r, 64 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out), "a buffer for the picture");
  VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_TIMESTAMP, qpi.queryCount = 2;
  VkQueryPool queries = VK_NULL_HANDLE;
  const int timestamps = r->timestamp_bits != 0;
  if (timestamps) check(vkCreateQueryPool(r->dev, &qpi, NULL, &queries) == VK_SUCCESS, "a timestamp query pool");
  VkCommandBuffer cb = begin(r);
  if (queries) vkCmdResetQueryPool(cb, queries, 0, 2), vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
  VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, ib.image = image;
  ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, ib.subresourceRange.levelCount = 1, ib.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
  VkClearColorValue colour = {{0.0f, 1.0f, 0.5f, 1.0f}};
  vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &ib.subresourceRange);
  ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
  VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 32, 1}};
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
  if (queries) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
  barrier_transfer_to_host(cb);
  check(run(r, cb), "clear and copy out");
  const uint8_t *px = out.map;
  check(px && px[0] == 0 && px[1] == 255 && (px[2] == 127 || px[2] == 128) && px[3] == 255, "the first pixel is the clear colour");
  check(px && px[(64 * 32 - 1) * 4 + 1] == 255, "and the last");
  if (queries) {
    uint64_t t[2] = {0, 0};
    check(vkGetQueryPoolResults(r->dev, queries, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS, "timestamps with WAIT");
    check(t[1] >= t[0] && t[1] != 0, "the second is not before the first");
    vkDestroyQueryPool(r->dev, queries, NULL);
  }
  free_buffer(r, &out);
  vkDestroyImage(r->dev, image, NULL);
  vkFreeMemory(r->dev, memory, NULL);
}

static void test_private_data(Rig *r) {
  if (!vkCreatePrivateDataSlot) return;
  VkPrivateDataSlotCreateInfo pci = {VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
  VkPrivateDataSlot slot = VK_NULL_HANDLE;
  check(vkCreatePrivateDataSlot(r->dev, &pci, NULL, &slot) == VK_SUCCESS, "a private data slot");
  check(vkSetPrivateData(r->dev, VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)(uintptr_t)r->pool, slot, 0x1234) == VK_SUCCESS, "data on a command pool");
  check(vkSetPrivateData(r->dev, VK_OBJECT_TYPE_QUEUE, (uint64_t)(uintptr_t)r->queue, slot, 0x5678) == VK_SUCCESS, "data on the queue");
  uint64_t v = 0;
  vkGetPrivateData(r->dev, VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)(uintptr_t)r->pool, slot, &v);
  check(v == 0x1234, "read back from the pool");
  vkGetPrivateData(r->dev, VK_OBJECT_TYPE_QUEUE, (uint64_t)(uintptr_t)r->queue, slot, &v);
  check(v == 0x5678, "read back from the queue");
  vkDestroyPrivateDataSlot(r->dev, slot, NULL);
}

static void test_device(VkPhysicalDevice pd, unsigned index) {
  Rig r;
  memset(&r, 0, sizeof r);
  r.pd = pd;
  VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &driver;
  vkGetPhysicalDeviceProperties2(pd, &p2);
  printf("venus: device %u: %s, Vulkan %u.%u, %s (%s)\n", index, p2.properties.deviceName, VK_API_VERSION_MAJOR(p2.properties.apiVersion),
         VK_API_VERSION_MINOR(p2.properties.apiVersion), driver.driverName, driver.driverInfo);
  check(driver.driverID == VK_DRIVER_ID_MESA_VENUS, "the device is the host's, behind Venus");
  check(VK_API_VERSION_MINOR(p2.properties.apiVersion) >= 1 && VK_API_VERSION_MINOR(p2.properties.apiVersion) <= 3, "Vulkan 1.1 .. 1.3");
  r.timestamp_period = p2.properties.limits.timestampPeriod;
  vkGetPhysicalDeviceMemoryProperties(pd, &r.memory);
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
  VkExtensionProperties ext[512];
  if (n > 512) n = 512;
  vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ext);
  int swapchain = 0, fd = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (!strcmp(ext[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain = 1;
    if (!strcmp(ext[i].extensionName, "VK_KHR_external_memory_fd")) fd = 1;
  }
  check(swapchain, "VK_KHR_swapchain is offered");
  check(!fd, "the external memory descriptors of the host are not");
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, NULL);
  VkQueueFamilyProperties fp[16];
  if (families > 16) families = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, fp);
  r.family = UINT32_MAX;
  for (uint32_t i = 0; i < families && r.family == UINT32_MAX; ++i)
    if (fp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) r.family = i, r.timestamp_bits = fp[i].timestampValidBits;
  check(r.family != UINT32_MAX, "a queue family with compute");
  if (r.family == UINT32_MAX) return;
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f12.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  check(f12.timelineSemaphore, "timeline semaphores are a feature of the host's device");
  VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  e12.timelineSemaphore = VK_TRUE, e12.pNext = &e13;
  e13.synchronization2 = f13.synchronization2, e13.privateData = f13.privateData;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = r.family, qci.queueCount = 1, qci.pQueuePriorities = &priority;
  const char *names[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &e12, dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = names;
  const char *bad[1] = {"VK_NOT_AN_EXTENSION"};
  dci.ppEnabledExtensionNames = bad;
  VkDevice none = VK_NULL_HANDLE;
  check(vkCreateDevice(pd, &dci, NULL, &none) == VK_ERROR_EXTENSION_NOT_PRESENT, "a device extension that is not offered is refused");
  dci.ppEnabledExtensionNames = names;
  check(vkCreateDevice(pd, &dci, NULL, &r.dev) == VK_SUCCESS && r.dev, "vkCreateDevice (with VK_KHR_swapchain)");
  if (!r.dev) return;
  vkGetDeviceQueue(r.dev, r.family, 0, &r.queue);
  check(r.queue != NULL, "vkGetDeviceQueue");
  check(vkGetDeviceProcAddr(r.dev, "vkCmdDraw") != NULL && vkGetDeviceProcAddr(r.dev, "vkCreateSwapchainKHR") != NULL, "device functions by name");
  check(vkGetDeviceProcAddr(r.dev, "vkCmdDrawMeshTasksEXT") == NULL, "a function of an extension not enabled is NULL");
  check(vkGetDeviceProcAddr(r.dev, "vkCreateInstance") == NULL, "an instance function is not a device function");
  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = r.family, cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  check(vkCreateCommandPool(r.dev, &cpi, NULL, &r.pool) == VK_SUCCESS, "a command pool");
  test_memory(&r);
  test_sync(&r);
  test_compute(&r);
  test_image_and_queries(&r);
  if (f13.privateData) test_private_data(&r);
  check(vkDeviceWaitIdle(r.dev) == VK_SUCCESS, "idle at the end");
  vkDestroyCommandPool(r.dev, r.pool, NULL);
  vkDestroyDevice(r.dev, NULL);
}

int main(void) {
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  check(lib != NULL, "vulkan-1.dll loads");
  if (!lib) {
    printf("venustest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
  }
  gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
  check(GetProcAddress(lib, "vkQueueSubmit2") != NULL && GetProcAddress(lib, "vkGetPhysicalDeviceProperties2") != NULL, "Vulkan 1.1 and 1.3 functions are exported");
  PFN_vkEnumerateInstanceVersion version = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
  uint32_t v = 0;
  check(version && version(&v) == VK_SUCCESS && VK_API_VERSION_MINOR(v) >= 1, "vkEnumerateInstanceVersion: 1.1 or more (the Venus backend)");
  vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "venustest", app.apiVersion = VK_API_VERSION_1_3;
  const char *extensions[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app, ici.enabledExtensionCount = 2, ici.ppEnabledExtensionNames = extensions;
  check(vkCreateInstance && vkCreateInstance(&ici, NULL, &g_instance) == VK_SUCCESS && g_instance, "vkCreateInstance");
  if (!g_instance) {
    printf("venustest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
  }
  load_all();
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(g_instance, &count, NULL) == VK_SUCCESS && count >= 1, "the host has a device");
  VkPhysicalDevice devices[8];
  if (count > 8) count = 8;
  vkEnumeratePhysicalDevices(g_instance, &count, devices);
  printf("venus: %u device(s) of the host\n", count);
  for (uint32_t i = 0; i < count; ++i) test_device(devices[i], i);
  vkDestroyInstance(g_instance, NULL);
  printf("venustest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

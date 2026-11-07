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

/* vulkan-1.dll: the loader entry points, the physical devices (every adapter of the machine: the CPU renderer and each GPU), and on each of them a real render through the whole
 * stack - SPIR-V shaders, a vertex buffer, a uniform buffer, a texture uploaded through a staging buffer, a render pass, a copy of the picture back to the CPU - checked pixel by pixel.
 * A pipeline that needs more than a small GPU has (the adapter reports its limits) is skipped on that adapter, not failed.
 * mingw gcc, no import library: the DLL is loaded by name. Prints "vulkantest: N checks, M failed"; the exit code is the number of failures. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/Windows/vulkan-1/khronos/vulkan/vulkan_core.h"
#include "../../src/Windows/vulkan-1/khronos/vulkan/vulkan_win32.h"
#include "vulkantest_shaders.h"

static int g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL line %d: %s\n", line, what);
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

static PFN_vkGetInstanceProcAddr pGetInstanceProcAddr;
#define FN(name) static PFN_##name p_##name;
FN(vkCreateInstance) FN(vkDestroyInstance) FN(vkEnumeratePhysicalDevices) FN(vkGetPhysicalDeviceProperties) FN(vkGetPhysicalDeviceFeatures) FN(vkGetPhysicalDeviceMemoryProperties)
FN(vkGetPhysicalDeviceQueueFamilyProperties) FN(vkGetPhysicalDeviceFormatProperties) FN(vkGetPhysicalDeviceImageFormatProperties) FN(vkEnumerateInstanceExtensionProperties)
FN(vkEnumerateDeviceExtensionProperties) FN(vkCreateDevice) FN(vkDestroyDevice) FN(vkGetDeviceQueue) FN(vkGetDeviceProcAddr) FN(vkCreateBuffer) FN(vkDestroyBuffer)
FN(vkGetBufferMemoryRequirements) FN(vkAllocateMemory) FN(vkFreeMemory) FN(vkBindBufferMemory) FN(vkMapMemory) FN(vkUnmapMemory) FN(vkCreateImage) FN(vkDestroyImage)
FN(vkGetImageMemoryRequirements) FN(vkBindImageMemory) FN(vkCreateImageView) FN(vkDestroyImageView) FN(vkCreateSampler) FN(vkDestroySampler) FN(vkCreateShaderModule)
FN(vkDestroyShaderModule) FN(vkCreateRenderPass) FN(vkDestroyRenderPass) FN(vkCreateFramebuffer) FN(vkDestroyFramebuffer) FN(vkCreateDescriptorSetLayout)
FN(vkDestroyDescriptorSetLayout) FN(vkCreatePipelineLayout) FN(vkDestroyPipelineLayout) FN(vkCreateDescriptorPool) FN(vkDestroyDescriptorPool) FN(vkAllocateDescriptorSets)
FN(vkUpdateDescriptorSets) FN(vkCreateGraphicsPipelines) FN(vkDestroyPipeline) FN(vkCreateCommandPool) FN(vkDestroyCommandPool) FN(vkAllocateCommandBuffers)
FN(vkBeginCommandBuffer) FN(vkEndCommandBuffer) FN(vkCmdBeginRenderPass) FN(vkCmdEndRenderPass) FN(vkCmdBindPipeline) FN(vkCmdBindVertexBuffers) FN(vkCmdBindDescriptorSets)
FN(vkCmdDraw) FN(vkCmdSetViewport) FN(vkCmdSetScissor) FN(vkCmdCopyImageToBuffer) FN(vkCmdCopyBufferToImage) FN(vkCmdPipelineBarrier) FN(vkCmdPushConstants) FN(vkQueueSubmit)
FN(vkQueueWaitIdle) FN(vkCreateFence) FN(vkDestroyFence) FN(vkWaitForFences) FN(vkResetFences) FN(vkFlushMappedMemoryRanges) FN(vkCmdClearColorImage) FN(vkCmdFillBuffer)
FN(vkCmdDrawIndexed) FN(vkCmdBindIndexBuffer) FN(vkCmdCopyBuffer) FN(vkDeviceWaitIdle) FN(vkCmdUpdateBuffer) FN(vkResetCommandBuffer) FN(vkCreateWin32SurfaceKHR) FN(vkDestroySurfaceKHR) FN(vkGetPhysicalDeviceSurfaceSupportKHR) FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
FN(vkGetPhysicalDeviceSurfaceFormatsKHR) FN(vkGetPhysicalDeviceSurfacePresentModesKHR) FN(vkCreateSwapchainKHR) FN(vkDestroySwapchainKHR) FN(vkGetSwapchainImagesKHR) FN(vkAcquireNextImageKHR) FN(vkQueuePresentKHR)
FN(vkCreateComputePipelines) FN(vkCmdDispatch) FN(vkCmdDispatchIndirect) FN(vkCreateQueryPool) FN(vkDestroyQueryPool) FN(vkGetQueryPoolResults)
FN(vkCmdResetQueryPool) FN(vkCmdWriteTimestamp)

static void *load(VkInstance instance, const char *name) {
  void *p = (void *)pGetInstanceProcAddr(instance, name);
  if (!p) {
    printf("FAIL: %s is not exported\n", name);
    ++g_failed;
  }
  return p;
}

static VkInstance g_instance;
static void load_all(void) {
#define L(name) p_##name = (PFN_##name)load(g_instance, #name);
  L(vkDestroyInstance) L(vkEnumeratePhysicalDevices) L(vkGetPhysicalDeviceProperties) L(vkGetPhysicalDeviceFeatures) L(vkGetPhysicalDeviceMemoryProperties)
  L(vkGetPhysicalDeviceQueueFamilyProperties) L(vkGetPhysicalDeviceFormatProperties) L(vkGetPhysicalDeviceImageFormatProperties) L(vkEnumerateDeviceExtensionProperties) L(vkCreateDevice)
  L(vkDestroyDevice) L(vkGetDeviceQueue) L(vkGetDeviceProcAddr) L(vkCreateBuffer) L(vkDestroyBuffer) L(vkGetBufferMemoryRequirements) L(vkAllocateMemory) L(vkFreeMemory)
  L(vkBindBufferMemory) L(vkMapMemory) L(vkUnmapMemory) L(vkCreateImage) L(vkDestroyImage) L(vkGetImageMemoryRequirements) L(vkBindImageMemory) L(vkCreateImageView) L(vkDestroyImageView)
  L(vkCreateSampler) L(vkDestroySampler) L(vkCreateShaderModule) L(vkDestroyShaderModule) L(vkCreateRenderPass) L(vkDestroyRenderPass) L(vkCreateFramebuffer) L(vkDestroyFramebuffer)
  L(vkCreateDescriptorSetLayout) L(vkDestroyDescriptorSetLayout) L(vkCreatePipelineLayout) L(vkDestroyPipelineLayout) L(vkCreateDescriptorPool) L(vkDestroyDescriptorPool)
  L(vkAllocateDescriptorSets) L(vkUpdateDescriptorSets) L(vkCreateGraphicsPipelines) L(vkDestroyPipeline) L(vkCreateCommandPool) L(vkDestroyCommandPool) L(vkAllocateCommandBuffers)
  L(vkBeginCommandBuffer) L(vkEndCommandBuffer) L(vkCmdBeginRenderPass) L(vkCmdEndRenderPass) L(vkCmdBindPipeline) L(vkCmdBindVertexBuffers) L(vkCmdBindDescriptorSets) L(vkCmdDraw)
  L(vkCmdSetViewport) L(vkCmdSetScissor) L(vkCmdCopyImageToBuffer) L(vkCmdCopyBufferToImage) L(vkCmdPipelineBarrier) L(vkCmdPushConstants) L(vkQueueSubmit) L(vkQueueWaitIdle)
  L(vkCreateFence) L(vkDestroyFence) L(vkWaitForFences) L(vkResetFences) L(vkFlushMappedMemoryRanges) L(vkCmdClearColorImage) L(vkCmdFillBuffer) L(vkCmdDrawIndexed) L(vkCmdBindIndexBuffer)
  L(vkCmdCopyBuffer) L(vkDeviceWaitIdle) L(vkCmdUpdateBuffer) L(vkResetCommandBuffer) L(vkCreateWin32SurfaceKHR) L(vkDestroySurfaceKHR) L(vkGetPhysicalDeviceSurfaceSupportKHR)
  L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) L(vkGetPhysicalDeviceSurfaceFormatsKHR) L(vkGetPhysicalDeviceSurfacePresentModesKHR) L(vkCreateSwapchainKHR) L(vkDestroySwapchainKHR)
  L(vkGetSwapchainImagesKHR) L(vkAcquireNextImageKHR) L(vkQueuePresentKHR) L(vkCreateComputePipelines) L(vkCmdDispatch) L(vkCmdDispatchIndirect) L(vkCreateQueryPool) L(vkDestroyQueryPool)
  L(vkGetQueryPoolResults) L(vkCmdResetQueryPool) L(vkCmdWriteTimestamp)
}

static int memory_type(const VkPhysicalDeviceMemoryProperties *mp, unsigned bits, VkMemoryPropertyFlags want) {
  for (unsigned i = 0; i < mp->memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

/* One device under test. */
typedef struct Rig {
  VkPhysicalDevice pd;
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceMemoryProperties mem;
  VkDevice dev;
  VkQueue queue;
  VkCommandPool pool;
  VkCommandBuffer cb;
  VkFence fence;
} Rig;

static VkBuffer make_buffer(Rig *r, VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapped) {
  VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size, ci.usage = usage, ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer b = VK_NULL_HANDLE;
  check(p_vkCreateBuffer(r->dev, &ci, NULL, &b) == VK_SUCCESS, "vkCreateBuffer");
  VkMemoryRequirements req;
  p_vkGetBufferMemoryRequirements(r->dev, b, &req);
  check(req.size >= size && req.alignment >= 1 && req.memoryTypeBits != 0, "buffer memory requirements");
  int type = memory_type(&r->mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  check(type >= 0, "a host visible memory type");
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size, ai.memoryTypeIndex = type < 0 ? 0 : (unsigned)type;
  check(p_vkAllocateMemory(r->dev, &ai, NULL, memory) == VK_SUCCESS, "vkAllocateMemory");
  check(p_vkBindBufferMemory(r->dev, b, *memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
  check(p_vkMapMemory(r->dev, *memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS && *mapped, "vkMapMemory");
  return b;
}

static void submit_and_wait(Rig *r) {
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &r->cb;
  p_vkResetFences(r->dev, 1, &r->fence);
  check(p_vkQueueSubmit(r->queue, 1, &si, r->fence) == VK_SUCCESS, "vkQueueSubmit");
  check(p_vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, ~0ull) == VK_SUCCESS, "vkWaitForFences");
}

static unsigned pixel(const unsigned char *picture, unsigned width, unsigned x, unsigned y) {
  const unsigned char *p = picture + ((size_t)y * width + x) * 4;
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
/* B, G, R, A of a pixel of the picture against what is expected (a few levels of rounding allowed) */
static int near_pixel(unsigned got, int b, int g, int rr, int a) {
  int gb = got & 255, gg = (got >> 8) & 255, gr = (got >> 16) & 255, ga = (got >> 24) & 255;
#define D(x, y) ((x) - (y) < 0 ? (y) - (x) : (x) - (y))
  return D(gb, b) <= 2 && D(gg, g) <= 2 && D(gr, rr) <= 2 && D(ga, a) <= 2;
}

enum { W = 64, H = 64 };

typedef struct Scene {
  VkImage target;
  VkDeviceMemory target_memory;
  VkImageView target_view;
  VkRenderPass pass;
  VkFramebuffer framebuffer;
  VkBuffer vertex, uniform, readback, staging;
  VkDeviceMemory vertex_memory, uniform_memory, readback_memory, staging_memory;
  void *vertex_map, *uniform_map, *readback_map, *staging_map;
  VkDescriptorSetLayout set_layout;
  VkPipelineLayout layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet set;
  VkImage texture;
  VkDeviceMemory texture_memory;
  VkImageView texture_view;
  VkSampler sampler;
  VkShaderModule vs, fs_color, fs_tex;
} Scene;

static VkPipeline make_pipeline(Rig *r, Scene *s, VkShaderModule fs, VkCullModeFlags cull, VkFrontFace front, VkResult *result) {
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = s->vs, stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
  VkVertexInputBindingDescription binding = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding, vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport = {0, 0, W, H, 0, 1};
  VkRect2D scissor = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1, vp.pViewports = &viewport, vp.scissorCount = 1, vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = cull, rs.frontFace = front, rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba = {0};
  ba.colorWriteMask = 15;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1, cb.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2, gp.pStages = stages, gp.pVertexInputState = &vi, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp, gp.pRasterizationState = &rs, gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb, gp.layout = s->layout, gp.renderPass = s->pass, gp.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  *result = p_vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline);
  return pipeline;
}

/* Begins the pass, draws once with the pipeline, ends it and copies the picture to the readback buffer. */
static void draw_frame_with(Rig *r, Scene *s, VkPipeline pipeline, VkDescriptorSet set, float clear_r, float clear_g, float clear_b);
static void draw_frame(Rig *r, Scene *s, VkPipeline pipeline, float clear_r, float clear_g, float clear_b) { draw_frame_with(r, s, pipeline, s->set, clear_r, clear_g, clear_b); }
static void draw_frame_with(Rig *r, Scene *s, VkPipeline pipeline, VkDescriptorSet set, float clear_r, float clear_g, float clear_b) {
  p_vkResetCommandBuffer(r->cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(p_vkBeginCommandBuffer(r->cb, &bi) == VK_SUCCESS, "vkBeginCommandBuffer");
  VkClearValue clear;
  memset(&clear, 0, sizeof clear);
  clear.color.float32[0] = clear_r, clear.color.float32[1] = clear_g, clear.color.float32[2] = clear_b, clear.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = s->pass, rb.framebuffer = s->framebuffer, rb.renderArea.extent.width = W, rb.renderArea.extent.height = H, rb.clearValueCount = 1, rb.pClearValues = &clear;
  p_vkCmdBeginRenderPass(r->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkDeviceSize offset = 0;
  p_vkCmdBindVertexBuffers(r->cb, 0, 1, &s->vertex, &offset);
  p_vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &set, 0, NULL);
  p_vkCmdDraw(r->cb, 3, 1, 0, 0);
  p_vkCmdEndRenderPass(r->cb);
  VkBufferImageCopy copy;
  memset(&copy, 0, sizeof copy);
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, copy.imageSubresource.layerCount = 1, copy.imageExtent.width = W, copy.imageExtent.height = H, copy.imageExtent.depth = 1;
  p_vkCmdCopyImageToBuffer(r->cb, s->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->readback, 1, &copy);
  check(p_vkEndCommandBuffer(r->cb) == VK_SUCCESS, "vkEndCommandBuffer");
  memset(s->readback_map, 0xCD, W * H * 4);
  submit_and_wait(r);
}


/* Push constants, a vertex attribute of normalised bytes (the driver gathers it into floats), an indexed draw with a vertex buffer offset, and a depth buffer: two quads, the nearer first. */
static void test_depth_and_push(Rig *r, Scene *s) {
  VkFormatProperties fp;
  p_vkGetPhysicalDeviceFormatProperties(r->pd, VK_FORMAT_D32_SFLOAT, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    printf("  (no depth buffer on this adapter)\n");
    return;
  }
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_D32_SFLOAT, ici.extent.width = W, ici.extent.height = H, ici.extent.depth = 1, ici.mipLevels = 1, ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL, ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VkImage depth = VK_NULL_HANDLE;
  check(p_vkCreateImage(r->dev, &ici, NULL, &depth) == VK_SUCCESS, "vkCreateImage (depth)");
  VkMemoryRequirements req;
  p_vkGetImageMemoryRequirements(r->dev, depth, &req);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size, mai.memoryTypeIndex = (unsigned)memory_type(&r->mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  check(p_vkAllocateMemory(r->dev, &mai, NULL, &depth_memory) == VK_SUCCESS && p_vkBindImageMemory(r->dev, depth, depth_memory, 0) == VK_SUCCESS, "the depth image's memory");
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = depth, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = ici.format;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, vci.subresourceRange.levelCount = 1, vci.subresourceRange.layerCount = 1;
  VkImageView depth_view = VK_NULL_HANDLE;
  check(p_vkCreateImageView(r->dev, &vci, NULL, &depth_view) == VK_SUCCESS, "vkCreateImageView (depth)");

  VkAttachmentDescription ad[2];
  memset(ad, 0, sizeof ad);
  ad[0].format = VK_FORMAT_B8G8R8A8_UNORM, ad[0].samples = VK_SAMPLE_COUNT_1_BIT, ad[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ad[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ad[0].stencilLoadOp = ad[0].stencilStoreOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, ad[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  ad[1].format = VK_FORMAT_D32_SFLOAT, ad[1].samples = VK_SAMPLE_COUNT_1_BIT, ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ad[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ad[1].stencilLoadOp = ad[1].stencilStoreOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, ad[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub;
  memset(&sub, 0, sizeof sub);
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &color_ref, sub.pDepthStencilAttachment = &depth_ref;
  VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 2, rpci.pAttachments = ad, rpci.subpassCount = 1, rpci.pSubpasses = &sub;
  VkRenderPass pass = VK_NULL_HANDLE;
  check(p_vkCreateRenderPass(r->dev, &rpci, NULL, &pass) == VK_SUCCESS, "vkCreateRenderPass (depth)");
  VkImageView views[2] = {s->target_view, depth_view};
  VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = pass, fbci.attachmentCount = 2, fbci.pAttachments = views, fbci.width = W, fbci.height = H, fbci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  check(p_vkCreateFramebuffer(r->dev, &fbci, NULL, &fb) == VK_SUCCESS, "vkCreateFramebuffer (depth)");

  VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  check(p_vkCreatePipelineLayout(r->dev, &plci, NULL, &layout) == VK_SUCCESS, "vkCreatePipelineLayout (push constants)");
  VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  VkShaderModule vs, fs;
  smci.codeSize = sizeof push_vert, smci.pCode = push_vert;
  check(p_vkCreateShaderModule(r->dev, &smci, NULL, &vs) == VK_SUCCESS, "vkCreateShaderModule (push vertex)");
  smci.codeSize = sizeof flat_frag, smci.pCode = flat_frag;
  check(p_vkCreateShaderModule(r->dev, &smci, NULL, &fs) == VK_SUCCESS, "vkCreateShaderModule (flat fragment)");
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
  VkVertexInputBindingDescription binding = {0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding, vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};   /* (this pipeline takes both from the command buffer) */
  vp.viewportCount = 1, vp.scissorCount = 1;
  VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2, dyn.pDynamicStates = dynamic;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE, ds.depthWriteEnable = VK_TRUE, ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ba = {0};
  ba.colorWriteMask = 15;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1, cb.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2, gp.pStages = stages, gp.pVertexInputState = &vi, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp, gp.pRasterizationState = &rs, gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds, gp.pColorBlendState = &cb, gp.pDynamicState = &dyn, gp.layout = layout, gp.renderPass = pass, gp.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult pr = p_vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline);
  check(pr == VK_SUCCESS && pipeline, "the depth and push constant pipeline");
  if (pipeline) {
    VkDeviceMemory vm, im;
    void *vmap, *imap;
    VkBuffer vb = make_buffer(r, 8 * 12, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vm, &vmap);
    VkBuffer ib = make_buffer(r, 12, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &im, &imap);
    struct { float x, y; unsigned char c[4]; } quad[8];
    static const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int q = 0; q < 2; ++q)
      for (int i = 0; i < 4; ++i) {
        quad[q * 4 + i].x = corners[i][0], quad[q * 4 + i].y = corners[i][1];
        quad[q * 4 + i].c[0] = q ? 0 : 255, quad[q * 4 + i].c[1] = q ? 255 : 0, quad[q * 4 + i].c[2] = 0, quad[q * 4 + i].c[3] = 255;
      }
    memcpy(vmap, quad, sizeof quad);
    static const unsigned short indices[6] = {0, 1, 2, 0, 2, 3};
    memcpy(imap, indices, sizeof indices);
    p_vkResetCommandBuffer(r->cb, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkBeginCommandBuffer(r->cb, &cbi);
    VkClearValue clears[2];
    memset(clears, 0, sizeof clears);
    clears[0].color.float32[0] = 0.0f, clears[0].color.float32[1] = 0.0f, clears[0].color.float32[2] = 1.0f, clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass, rb.framebuffer = fb, rb.renderArea.extent.width = W, rb.renderArea.extent.height = H, rb.clearValueCount = 2, rb.pClearValues = clears;
    p_vkCmdBeginRenderPass(r->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport = {0, 0, W, H, 0, 1};
    VkRect2D scissor = {{0, 0}, {W, H}};
    p_vkCmdSetViewport(r->cb, 0, 1, &viewport);
    p_vkCmdSetScissor(r->cb, 0, 1, &scissor);
    p_vkCmdBindIndexBuffer(r->cb, ib, 0, VK_INDEX_TYPE_UINT16);
    float push[8] = {-0.25f, 0, 0, 0, 0.5f, 0, 0, 0};     /* the first quad, near, shifted left */
    VkDeviceSize offset = 0;
    p_vkCmdBindVertexBuffers(r->cb, 0, 1, &vb, &offset);
    p_vkCmdPushConstants(r->cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, push);
    p_vkCmdDrawIndexed(r->cb, 6, 1, 0, 0, 0);
    push[0] = 0.25f, push[4] = 0.8f;                         /* the second, farther, shifted right, with the vertices of its own */
    offset = 4 * 12;
    p_vkCmdBindVertexBuffers(r->cb, 0, 1, &vb, &offset);
    p_vkCmdPushConstants(r->cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, push);
    p_vkCmdDrawIndexed(r->cb, 6, 1, 0, 0, 0);
    p_vkCmdEndRenderPass(r->cb);
    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, copy.imageSubresource.layerCount = 1, copy.imageExtent.width = W, copy.imageExtent.height = H, copy.imageExtent.depth = 1;
    p_vkCmdCopyImageToBuffer(r->cb, s->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->readback, 1, &copy);
    p_vkEndCommandBuffer(r->cb);
    memset(s->readback_map, 0xCD, W * H * 4);
    submit_and_wait(r);
    const unsigned char *picture = (const unsigned char *)s->readback_map;
    printf("  depth pixels: %08x %08x %08x %08x\n", pixel(picture, W, 4, 32), pixel(picture, W, 12, 32), pixel(picture, W, 32, 32), pixel(picture, W, 50, 32));
    check(near_pixel(pixel(picture, W, 4, 32), 255, 0, 0, 255), "depth: outside both quads is the clear colour");
    check(near_pixel(pixel(picture, W, 12, 32), 0, 0, 255, 255), "depth: the first quad (red) where only it is");
    check(near_pixel(pixel(picture, W, 32, 32), 0, 0, 255, 255), "depth: ... and where the second is behind it");
    check(near_pixel(pixel(picture, W, 50, 32), 0, 255, 0, 255), "depth: the second quad (green) where only it is");
    p_vkDestroyBuffer(r->dev, vb, NULL);
    p_vkDestroyBuffer(r->dev, ib, NULL);
    p_vkFreeMemory(r->dev, vm, NULL);
    p_vkFreeMemory(r->dev, im, NULL);
    p_vkDestroyPipeline(r->dev, pipeline, NULL);
  }
  p_vkDestroyShaderModule(r->dev, vs, NULL);
  p_vkDestroyShaderModule(r->dev, fs, NULL);
  p_vkDestroyPipelineLayout(r->dev, layout, NULL);
  p_vkDestroyFramebuffer(r->dev, fb, NULL);
  p_vkDestroyRenderPass(r->dev, pass, NULL);
  p_vkDestroyImageView(r->dev, depth_view, NULL);
  p_vkDestroyImage(r->dev, depth, NULL);
  p_vkFreeMemory(r->dev, depth_memory, NULL);
}

/* A surface (of no window: the size comes from the capabilities), a swapchain, a frame cleared and presented. */
static void test_swapchain(Rig *r) {
  VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = GetModuleHandleA(NULL);
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  check(p_vkCreateWin32SurfaceKHR(g_instance, &sci, NULL, &surface) == VK_SUCCESS && surface, "vkCreateWin32SurfaceKHR");
  VkBool32 supported = VK_FALSE;
  check(p_vkGetPhysicalDeviceSurfaceSupportKHR(r->pd, 0, surface, &supported) == VK_SUCCESS && supported, "the queue family presents");
  VkSurfaceCapabilitiesKHR caps;
  check(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->pd, surface, &caps) == VK_SUCCESS && caps.minImageCount >= 1 && caps.currentExtent.width > 0, "surface capabilities");
  unsigned nf = 0;
  VkSurfaceFormatKHR formats[4];
  p_vkGetPhysicalDeviceSurfaceFormatsKHR(r->pd, surface, &nf, NULL);
  if (nf > 4) nf = 4;
  check(p_vkGetPhysicalDeviceSurfaceFormatsKHR(r->pd, surface, &nf, formats) >= VK_SUCCESS && nf >= 1, "surface formats");
  unsigned nm = 0;
  p_vkGetPhysicalDeviceSurfacePresentModesKHR(r->pd, surface, &nm, NULL);
  check(nm >= 1, "present modes");
  VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface, ci.minImageCount = 2, ci.imageFormat = formats[0].format, ci.imageColorSpace = formats[0].colorSpace, ci.imageExtent = caps.currentExtent, ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, ci.presentMode = VK_PRESENT_MODE_FIFO_KHR, ci.clipped = VK_TRUE;
  VkSwapchainKHR chain = VK_NULL_HANDLE;
  check(p_vkCreateSwapchainKHR(r->dev, &ci, NULL, &chain) == VK_SUCCESS && chain, "vkCreateSwapchainKHR");
  if (chain) {
    unsigned n = 0;
    VkImage images[8];
    p_vkGetSwapchainImagesKHR(r->dev, chain, &n, NULL);
    if (n > 8) n = 8;
    check(p_vkGetSwapchainImagesKHR(r->dev, chain, &n, images) == VK_SUCCESS && n >= 2, "swapchain images");
    unsigned index = 99;
    check(p_vkAcquireNextImageKHR(r->dev, chain, ~0ull, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) == VK_SUCCESS && index < n, "vkAcquireNextImageKHR");
    p_vkResetCommandBuffer(r->cb, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkBeginCommandBuffer(r->cb, &cbi);
    VkClearColorValue cc = {{0.0f, 0.5f, 1.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(r->cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    p_vkEndCommandBuffer(r->cb);
    submit_and_wait(r);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1, pi.pSwapchains = &chain, pi.pImageIndices = &index;
    check(p_vkQueuePresentKHR(r->queue, &pi) == VK_SUCCESS, "vkQueuePresentKHR");
    p_vkDestroySwapchainKHR(r->dev, chain, NULL);
  }
  p_vkDestroySurfaceKHR(g_instance, surface, NULL);
}

/* Compute: storage buffers (one read only), push constants, atomics and signed division (cs_add.comp), then group-shared memory and a barrier with a std430 struct (cs_shared.comp).
 * In one submission: a dispatch, a copy of what it wrote (done by the CPU: it must see the GPU's results), an indirect dispatch whose group counts come from a buffer. Skipped on an
 * adapter without compute (no VK_QUEUE_COMPUTE_BIT). */
static void test_compute(Rig *r) {
  VkQueueFamilyProperties fam;
  unsigned nfam = 1;
  p_vkGetPhysicalDeviceQueueFamilyProperties(r->pd, &nfam, &fam);
  if (!(fam.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
    printf("    no compute shaders on this adapter: skipped\n");
    return;
  }
  const VkPhysicalDeviceLimits *l = &r->props.limits;
  check(l->maxComputeWorkGroupInvocations >= 128 && l->maxComputeSharedMemorySize >= 16384 && l->maxPerStageDescriptorStorageBuffers >= 4 && l->maxComputeWorkGroupCount[0] >= 65535,
        "compute limits (the minimums of Vulkan 1.0)");
  VkDescriptorSetLayoutBinding bindings[2];
  memset(bindings, 0, sizeof bindings);
  for (unsigned i = 0; i < 2; ++i) bindings[i].binding = i, bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindings[i].descriptorCount = 1, bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 2, dl.pBindings = bindings;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  check(p_vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &set_layout) == VK_SUCCESS, "a set layout of storage buffers");
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1, pl.pSetLayouts = &set_layout, pl.pushConstantRangeCount = 1, pl.pPushConstantRanges = &push;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  check(p_vkCreatePipelineLayout(r->dev, &pl, NULL, &layout) == VK_SUCCESS, "a compute pipeline layout");
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 2, dpi.poolSizeCount = 1, dpi.pPoolSizes = &ps;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  check(p_vkCreateDescriptorPool(r->dev, &dpi, NULL, &pool) == VK_SUCCESS, "a descriptor pool");
  VkDescriptorSetLayout layouts[2] = {set_layout, set_layout};
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool, dai.descriptorSetCount = 2, dai.pSetLayouts = layouts;
  VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  check(p_vkAllocateDescriptorSets(r->dev, &dai, sets) == VK_SUCCESS, "descriptor sets");

  /* the buffers: cs_add's input and output, a copy of the output, the indirect group counts; cs_shared's items and sums */
  VkDeviceMemory mem[6];
  void *map[6];
  const VkDeviceSize sizes[6] = {8 + 16 * 4, 36 + 16 * 4, 36 + 16 * 4, 16, 32 * 32, 2 * 4};
  const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  VkBuffer buf[6];
  for (unsigned i = 0; i < 6; ++i) buf[i] = make_buffer(r, sizes[i], usage, &mem[i], &map[i]);
  float *src = map[0], *items = map[4];
  unsigned *dst = map[1], *copy = map[2], *groups = map[3];
  src[0] = 0.25f, src[1] = 0.5f;
  for (unsigned i = 0; i < 16; ++i) {
    src[2 + i] = i * 1.5f - 3.0f;
    ((float *)dst)[9 + i] = -7.0f;
  }
  dst[0] = 0;
  groups[0] = 1, groups[1] = 1, groups[2] = 1;
  float expected_sum[2] = {0, 0};
  for (unsigned i = 0; i < 32; ++i) {
    const float f[8] = {0.5f * i, 1.0f + 0.25f * (i % 5), 0.1f * (i % 3), 0.01f * i, 1.0f, 2.0f + i, 3.0f, 0.5f * (i % 4)};
    memcpy(items + 8 * i, f, sizeof f);
    expected_sum[i / 16] += f[0] + f[1] * f[2] + f[3] + f[5] + f[7] * 2.0f;
  }
  VkDescriptorBufferInfo infos[4] = {{buf[0], 0, VK_WHOLE_SIZE}, {buf[1], 0, VK_WHOLE_SIZE}, {buf[4], 0, VK_WHOLE_SIZE}, {buf[5], 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet writes[2];
  memset(writes, 0, sizeof writes);
  for (unsigned s = 0; s < 2; ++s) {
    writes[s].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, writes[s].dstSet = sets[s], writes[s].dstBinding = 0, writes[s].descriptorCount = 2;
    writes[s].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[s].pBufferInfo = &infos[2 * s];
  }
  p_vkUpdateDescriptorSets(r->dev, 2, writes, 0, NULL);

  VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkPipeline pipes[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  const unsigned *code[2] = {add_comp, shared_comp};
  const size_t code_size[2] = {sizeof add_comp, sizeof shared_comp};
  for (unsigned k = 0; k < 2; ++k) {
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = code_size[k], smi.pCode = code[k];
    check(p_vkCreateShaderModule(r->dev, &smi, NULL, &modules[k]) == VK_SUCCESS, "a compute shader module");
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cpi.stage.module = modules[k], cpi.stage.pName = "main";
    cpi.layout = layout;
    VkResult res = p_vkCreateComputePipelines(r->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipes[k]);
    check(res == VK_SUCCESS && pipes[k], k == 0 ? "vkCreateComputePipelines (storage buffers, atomics)" : "vkCreateComputePipelines (group-shared memory, a barrier)");
  }
  if (pipes[0] && pipes[1]) {
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkResetCommandBuffer(r->cb, 0);
    p_vkBeginCommandBuffer(r->cb, &cbi);
    p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[0]);
    p_vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[0], 0, NULL);
    const struct { float scale; unsigned count; } pc = {2.0f, 13};
    p_vkCmdPushConstants(r->cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    p_vkCmdDispatch(r->cb, 2, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    p_vkCmdPipelineBarrier(r->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferCopy region = {0, 0, sizes[1]};
    p_vkCmdCopyBuffer(r->cb, buf[1], buf[2], 1, &region);
    p_vkCmdDispatchIndirect(r->cb, buf[3], 0);   /* (one more group: threads 0 .. 7 add 1 .. 8 again) */
    p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[1]);
    p_vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[1], 0, NULL);
    p_vkCmdDispatch(r->cb, 2, 1, 1);
    p_vkEndCommandBuffer(r->cb);
    submit_and_wait(r);
    unsigned wrong = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const float want = i < 13 ? (i * 1.5f - 3.0f) * 2.0f + 0.5f : -7.0f;
      const float got = ((const float *)dst)[9 + i];
      wrong += got < want - 1e-4f || got > want + 1e-4f;
    }
    for (unsigned i = 0; i < 8; ++i) {
      const int v = (int)i - 5;
      wrong += (int)dst[1 + i] != (v / 3) * 100 + ((v % 3) + 3) % 3;
    }
    check(wrong == 0, "a dispatch writes its storage buffer (cs_add: arithmetic, signed division and modulo)");
    check(copy[0] == 136 && dst[0] == 136 + 36, "atomicAdd, a copy made after the dispatch sees what it wrote, an indirect dispatch");
    const float *sums = map[5];
    check(sums[0] > expected_sum[0] - 1e-2f && sums[0] < expected_sum[0] + 1e-2f && sums[1] > expected_sum[1] - 1e-2f && sums[1] < expected_sum[1] + 1e-2f,
          "group-shared memory and a barrier (cs_shared: the sums of two groups)");
    unsigned wrong_w = 0;
    for (unsigned i = 0; i < 32; ++i) wrong_w += items[8 * i + 3] != (float)(i % 16);
    check(wrong_w == 0, "a store into a member of a struct in a runtime array");
    if (wrong || copy[0] != 136) printf("    cs_add: total %u (copy %u), b[0] %g, q[0] %d; cs_shared: sums %g %g (want %g %g)\n", dst[0], copy[0], ((const float *)dst)[9], (int)dst[1], sums[0], sums[1], expected_sum[0], expected_sum[1]);
  }
  for (unsigned k = 0; k < 2; ++k) {
    if (pipes[k]) p_vkDestroyPipeline(r->dev, pipes[k], NULL);
    if (modules[k]) p_vkDestroyShaderModule(r->dev, modules[k], NULL);
  }
  for (unsigned i = 0; i < 6; ++i) {
    p_vkDestroyBuffer(r->dev, buf[i], NULL);
    p_vkFreeMemory(r->dev, mem[i], NULL);
  }
  p_vkDestroyDescriptorPool(r->dev, pool, NULL);
  p_vkDestroyPipelineLayout(r->dev, layout, NULL);
  p_vkDestroyDescriptorSetLayout(r->dev, set_layout, NULL);
}

/* A shader far past the old limits (cs_big.comp: 80 values live through 30 rounds of updates, a few thousand instructions, more than 32 registers at once): the translator, the
 * kernel and, on virgl, the shader text sent in pieces and the host's compiler all take it, and it computes what the same arithmetic in C does. */
static void test_compute_big(Rig *r) {
  VkQueueFamilyProperties fam;
  unsigned nfam = 1;
  p_vkGetPhysicalDeviceQueueFamilyProperties(r->pd, &nfam, &fam);
  if (!(fam.queueFlags & VK_QUEUE_COMPUTE_BIT)) return;
  VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
  VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 1, dl.pBindings = &binding;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  p_vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &set_layout);
  VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1, pl.pSetLayouts = &set_layout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  p_vkCreatePipelineLayout(r->dev, &pl, NULL, &layout);
  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1, dpi.poolSizeCount = 1, dpi.pPoolSizes = &ps;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  p_vkCreateDescriptorPool(r->dev, &dpi, NULL, &pool);
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  check(p_vkAllocateDescriptorSets(r->dev, &dai, &set) == VK_SUCCESS, "a descriptor set (large shader)");
  VkDeviceMemory mem;
  void *map;
  VkBuffer buf = make_buffer(r, 8 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mem, &map);
  float *v = map;
  for (unsigned i = 0; i < 8; ++i) v[i] = 0.25f * i - 1.0f;
  VkDescriptorBufferInfo info = {buf, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w.pBufferInfo = &info;
  p_vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof big_comp, smi.pCode = big_comp;
  VkShaderModule module = VK_NULL_HANDLE;
  check(p_vkCreateShaderModule(r->dev, &smi, NULL, &module) == VK_SUCCESS, "a large compute shader module");
  VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cpi.stage.module = module, cpi.stage.pName = "main";
  cpi.layout = layout;
  VkPipeline pipe = VK_NULL_HANDLE;
  check(p_vkCreateComputePipelines(r->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe) == VK_SUCCESS && pipe, "vkCreateComputePipelines (a shader of thousands of instructions, 80 live values)");
  if (pipe) {
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkResetCommandBuffer(r->cb, 0);
    p_vkBeginCommandBuffer(r->cb, &cbi);
    p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p_vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    p_vkCmdDispatch(r->cb, 2, 1, 1);
    p_vkEndCommandBuffer(r->cb);
    submit_and_wait(r);
    unsigned wrong = 0;
    for (unsigned i = 0; i < 8; ++i) {
      const float x = 0.25f * i - 1.0f;
      float a[80], sum = 0;
      for (int k = 0; k < 80; ++k) a[k] = x * (float)(k % 7 + 1) + (float)k;
      for (int round = 0; round < 30; ++round)
        for (int k = 0; k < 80; ++k) a[k] = a[k] * 0.5f + a[(k + 7) % 80] * 0.25f + (float)(round % 5);
      for (int k = 0; k < 80; ++k) sum = sum + a[k] * (float)(k % 3 + 1);
      const float d = v[i] - sum, tol = 1e-4f * (1 + (sum < 0 ? -sum : sum));
      if (d > tol || d < -tol) {
        if (!wrong) printf("    large shader: v[%u] = %g, expected %g\n", i, v[i], sum);
        ++wrong;
      }
    }
    check(wrong == 0, "the large shader computes what C does");
    p_vkDestroyPipeline(r->dev, pipe, NULL);
  }
  if (module) p_vkDestroyShaderModule(r->dev, module, NULL);
  p_vkDestroyBuffer(r->dev, buf, NULL);
  p_vkFreeMemory(r->dev, mem, NULL);
  p_vkDestroyDescriptorPool(r->dev, pool, NULL);
  p_vkDestroyPipelineLayout(r->dev, layout, NULL);
  p_vkDestroyDescriptorSetLayout(r->dev, set_layout, NULL);
}

/* A cube map array (two cubes: twelve layers of 4 x 4, each a colour of its own) sampled in a pixel shader along directions from the pixel's place (vk_cubetex.frag): each pixel
 * shows the layer Vulkan's table picks - face + 6 * cube. Only on an adapter with imageCubeArray. */
static void test_cube_array(Rig *r, Scene *s) {
  VkPhysicalDeviceFeatures features;
  p_vkGetPhysicalDeviceFeatures(r->pd, &features);
  if (!features.imageCubeArray) {
    printf("    no cube map arrays on this adapter: skipped\n");
    return;
  }
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_B8G8R8A8_UNORM, ici.extent.width = 4, ici.extent.height = 4, ici.extent.depth = 1;
  ici.mipLevels = 1, ici.arrayLayers = 12, ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  check(p_vkCreateImage(r->dev, &ici, NULL, &image) == VK_SUCCESS && image, "a cube compatible image of twelve layers");
  if (!image) return;
  VkMemoryRequirements req;
  p_vkGetImageMemoryRequirements(r->dev, image, &req);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size, ai.memoryTypeIndex = (unsigned)memory_type(&r->mem, req.memoryTypeBits, 0);
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  p_vkAllocateMemory(r->dev, &ai, NULL, &image_memory);
  p_vkBindImageMemory(r->dev, image, image_memory, 0);
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image, vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, vci.format = ici.format;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, vci.subresourceRange.levelCount = 1, vci.subresourceRange.layerCount = 12;
  VkImageView view = VK_NULL_HANDLE;
  check(p_vkCreateImageView(r->dev, &vci, NULL, &view) == VK_SUCCESS, "a cube array view");
  /* the texels, one colour a layer (B, G, R, A in memory) */
  VkDeviceMemory staging_memory;
  void *staging_map;
  VkBuffer staging = make_buffer(r, 12 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_memory, &staging_map);
  unsigned char *texels = staging_map;
  for (unsigned layer = 0; layer < 12; ++layer)
    for (unsigned t = 0; t < 16; ++t) {
      unsigned char *px = texels + (layer * 16 + t) * 4;
      px[0] = (unsigned char)(layer * 7), px[1] = (unsigned char)(200 - layer * 15), px[2] = (unsigned char)(layer * 20 + 10), px[3] = 255;
    }
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1, dpi.poolSizeCount = 2, dpi.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  p_vkCreateDescriptorPool(r->dev, &dpi, NULL, &pool);
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &s->set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  p_vkAllocateDescriptorSets(r->dev, &dai, &set);
  VkDescriptorBufferInfo dbi = {s->uniform, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo dii = {s->sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2];
  memset(writes, 0, sizeof writes);
  writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = writes[1].dstSet = set;
  writes[0].dstBinding = 0, writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[0].pBufferInfo = &dbi;
  writes[1].dstBinding = 1, writes[1].descriptorCount = 1, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &dii;
  p_vkUpdateDescriptorSets(r->dev, 2, writes, 0, NULL);
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof cubetex_frag, smi.pCode = cubetex_frag;
  VkShaderModule fs = VK_NULL_HANDLE;
  p_vkCreateShaderModule(r->dev, &smi, NULL, &fs);
  VkResult result;
  VkPipeline pipeline = make_pipeline(r, s, fs, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, &result);
  check(result == VK_SUCCESS && pipeline, "a pipeline that samples a cube map array");
  if (pipeline) {
    /* the texels in (one copy of all twelve layers), then the picture */
    p_vkResetCommandBuffer(r->cb, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkBeginCommandBuffer(r->cb, &cbi);
    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, copy.imageSubresource.layerCount = 12, copy.imageExtent.width = 4, copy.imageExtent.height = 4, copy.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(r->cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    p_vkEndCommandBuffer(r->cb);
    submit_and_wait(r);
    static const float white[15] = {-1, -1, 1, 1, 1, 3, -1, 1, 1, 1, -1, 3, 1, 1, 1};
    memcpy(s->vertex_map, white, sizeof white);
    const float tint[4] = {1, 1, 1, 1};
    memcpy(s->uniform_map, tint, sizeof tint);
    draw_frame_with(r, s, pipeline, set, 0, 0, 0);
    const unsigned char *picture = (const unsigned char *)s->readback_map;
    unsigned wrong = 0, compared = 0;
    for (unsigned py = 2; py < H; py += 4)
      for (unsigned px = 2; px < W; px += 4) {
        const float u = (px + 0.5f) / W, v = (py + 0.5f) / H;
        const float cube = u < 0.5f ? 0.0f : 1.0f;
        float qx = u * 2.0f;
        qx = (qx - (float)(int)qx) * 2.0f - 1.0f;
        const float x = qx, y = v * 2.0f - 1.0f;   /* (z: -0.55) */
        const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = 0.55f;
        /* (not where two axes are nearly as long: the face there depends on rounding) */
        const float m1 = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
        const float m2 = ax + ay + az - m1 - (ax < ay ? (ax < az ? ax : az) : (ay < az ? ay : az));
        if (m1 - m2 < 0.06f) continue;
        unsigned face = ax >= ay && ax >= az ? (x >= 0 ? 0 : 1) : ay >= az ? (y >= 0 ? 2 : 3) : 5;
        const unsigned layer = face + 6 * (unsigned)cube;
        ++compared;
        if (!near_pixel(pixel(picture, W, px, py), layer * 7, 200 - layer * 15, layer * 20 + 10, 255)) {
          if (!wrong) printf("    cube array: pixel %u,%u is %08x, want layer %u\n", px, py, pixel(picture, W, px, py), layer);
          ++wrong;
        }
      }
    check(compared > 100 && wrong == 0, "a cube map array: every pixel shows the face and the cube its direction picks");
    p_vkDestroyPipeline(r->dev, pipeline, NULL);
  }
  if (fs) p_vkDestroyShaderModule(r->dev, fs, NULL);
  p_vkDestroyDescriptorPool(r->dev, pool, NULL);
  p_vkDestroyBuffer(r->dev, staging, NULL);
  p_vkFreeMemory(r->dev, staging_memory, NULL);
  p_vkDestroyImageView(r->dev, view, NULL);
  p_vkDestroyImage(r->dev, image, NULL);
  p_vkFreeMemory(r->dev, image_memory, NULL);
}

/* Textures outside pixel shaders: a vertex shader whose colour is a texel it fetches (green), and a compute shader that fetches, looks up at level 0 and asks the size of the
 * scene's 2 x 2 texture (red, green / blue, white). */
static void test_textures_in_vertex_and_compute(Rig *r, Scene *s) {
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof vtexdraw_vert, smi.pCode = vtexdraw_vert;
  VkShaderModule vs = VK_NULL_HANDLE;
  p_vkCreateShaderModule(r->dev, &smi, NULL, &vs);
  VkShaderModule scene_vs = s->vs;
  s->vs = vs;
  VkResult result;
  VkPipeline pipeline = make_pipeline(r, s, s->fs_color, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, &result);
  s->vs = scene_vs;
  check(result == VK_SUCCESS && pipeline, "a pipeline whose vertex shader reads a texture");
  if (pipeline) {
    static const float white[15] = {-1, -1, 1, 1, 1, 3, -1, 1, 1, 1, -1, 3, 1, 1, 1};
    memcpy(s->vertex_map, white, sizeof white);
    draw_frame(r, s, pipeline, 0, 0, 0);
    const unsigned char *picture = (const unsigned char *)s->readback_map;
    check(near_pixel(pixel(picture, W, 10, 10), 0, 255, 0, 255) && near_pixel(pixel(picture, W, 50, 40), 0, 255, 0, 255), "a vertex shader's texel fetch colours the draw");
    p_vkDestroyPipeline(r->dev, pipeline, NULL);
  }
  if (vs) p_vkDestroyShaderModule(r->dev, vs, NULL);

  VkQueueFamilyProperties fam;
  unsigned nfam = 1;
  p_vkGetPhysicalDeviceQueueFamilyProperties(r->pd, &nfam, &fam);
  if (!(fam.queueFlags & VK_QUEUE_COMPUTE_BIT)) return;
  VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
  VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 2, dl.pBindings = bindings;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  p_vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &set_layout);
  VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1, pl.pSetLayouts = &set_layout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  p_vkCreatePipelineLayout(r->dev, &pl, NULL, &layout);
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1, dpi.poolSizeCount = 2, dpi.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  p_vkCreateDescriptorPool(r->dev, &dpi, NULL, &pool);
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  p_vkAllocateDescriptorSets(r->dev, &dai, &set);
  VkDeviceMemory mem;
  void *map;
  VkBuffer buf = make_buffer(r, 3 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mem, &map);
  memset(map, 0, 3 * 16);
  VkDescriptorImageInfo dii = {s->sampler, s->texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[2];
  memset(writes, 0, sizeof writes);
  writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = writes[1].dstSet = set;
  writes[0].dstBinding = 0, writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[0].pImageInfo = &dii;
  writes[1].dstBinding = 1, writes[1].descriptorCount = 1, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[1].pBufferInfo = &dbi;
  p_vkUpdateDescriptorSets(r->dev, 2, writes, 0, NULL);
  smi.codeSize = sizeof ctex_comp, smi.pCode = ctex_comp;
  VkShaderModule cs = VK_NULL_HANDLE;
  p_vkCreateShaderModule(r->dev, &smi, NULL, &cs);
  VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cpi.stage.module = cs, cpi.stage.pName = "main";
  cpi.layout = layout;
  VkPipeline cpipe = VK_NULL_HANDLE;
  check(p_vkCreateComputePipelines(r->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe) == VK_SUCCESS && cpipe, "a compute pipeline that reads a texture");
  if (cpipe) {
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkResetCommandBuffer(r->cb, 0);
    p_vkBeginCommandBuffer(r->cb, &cbi);
    p_vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    p_vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    p_vkCmdDispatch(r->cb, 1, 1, 1);
    p_vkEndCommandBuffer(r->cb);
    submit_and_wait(r);
    const float *o = map;
    const int fetched = o[0] > 0.99f && o[1] > 0.99f && o[2] > 0.99f && o[3] > 0.99f;                 /* (1, 1): white */
    const int looked = o[4] < 0.01f && o[5] < 0.01f && o[6] > 0.99f && o[7] > 0.99f;                  /* (0.25, 0.75): blue */
    check(fetched && looked && o[8] == 2.0f && o[9] == 2.0f, "a compute shader reads a texture: a fetch, a lookup at level 0, its size");
    if (!(fetched && looked && o[8] == 2.0f)) printf("    compute texture: %g %g %g %g | %g %g %g %g | %g %g\n", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9]);
    p_vkDestroyPipeline(r->dev, cpipe, NULL);
  }
  if (cs) p_vkDestroyShaderModule(r->dev, cs, NULL);
  p_vkDestroyBuffer(r->dev, buf, NULL);
  p_vkFreeMemory(r->dev, mem, NULL);
  p_vkDestroyDescriptorPool(r->dev, pool, NULL);
  p_vkDestroyPipelineLayout(r->dev, layout, NULL);
  p_vkDestroyDescriptorSetLayout(r->dev, set_layout, NULL);
}

/* Twelve texture units (vk_units.frag: an array of twelve samplers) bound in turn to three 1 x 1 images, red, green and blue: the picture mixes units 1, 6 and 11 (green, red,
 * blue). Skipped on an adapter with fewer units. */
static void test_many_units(Rig *r, Scene *s) {
  if (r->props.limits.maxPerStageDescriptorSamplers < 12) {
    printf("    %u texture units on this adapter: skipped\n", r->props.limits.maxPerStageDescriptorSamplers);
    return;
  }
  VkImage images[3];
  VkDeviceMemory memories[3];
  VkImageView views[3];
  VkDeviceMemory staging_memory;
  void *staging_map;
  VkBuffer staging = make_buffer(r, 3 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_memory, &staging_map);
  static const unsigned char colours[12] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255};   /* B, G, R, A: red, green, blue */
  memcpy(staging_map, colours, sizeof colours);
  p_vkResetCommandBuffer(r->cb, 0);
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  p_vkBeginCommandBuffer(r->cb, &cbi);
  for (unsigned k = 0; k < 3; ++k) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_B8G8R8A8_UNORM, ici.extent.width = ici.extent.height = ici.extent.depth = 1, ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL, ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p_vkCreateImage(r->dev, &ici, NULL, &images[k]);
    VkMemoryRequirements req;
    p_vkGetImageMemoryRequirements(r->dev, images[k], &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = (unsigned)memory_type(&r->mem, req.memoryTypeBits, 0);
    p_vkAllocateMemory(r->dev, &ai, NULL, &memories[k]);
    p_vkBindImageMemory(r->dev, images[k], memories[k], 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = images[k], vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = ici.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, vci.subresourceRange.levelCount = 1, vci.subresourceRange.layerCount = 1;
    p_vkCreateImageView(r->dev, &vci, NULL, &views[k]);
    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof copy);
    copy.bufferOffset = 4 * k, copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, copy.imageSubresource.layerCount = 1, copy.imageExtent.width = copy.imageExtent.height = copy.imageExtent.depth = 1;
    p_vkCmdCopyBufferToImage(r->cb, staging, images[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  }
  p_vkEndCommandBuffer(r->cb);
  submit_and_wait(r);
  VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
  VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 2, dl.pBindings = bindings;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  p_vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &set_layout);
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1, plci.pSetLayouts = &set_layout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  p_vkCreatePipelineLayout(r->dev, &plci, NULL, &layout);
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1, dpi.poolSizeCount = 2, dpi.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  p_vkCreateDescriptorPool(r->dev, &dpi, NULL, &pool);
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool, dai.descriptorSetCount = 1, dai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  p_vkAllocateDescriptorSets(r->dev, &dai, &set);
  VkDescriptorBufferInfo dbi = {s->uniform, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo dii[12];
  for (unsigned k = 0; k < 12; ++k) dii[k].sampler = s->sampler, dii[k].imageView = views[k % 3], dii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet writes[2];
  memset(writes, 0, sizeof writes);
  writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = writes[1].dstSet = set;
  writes[0].dstBinding = 0, writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[0].pBufferInfo = &dbi;
  writes[1].dstBinding = 1, writes[1].descriptorCount = 12, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = dii;
  p_vkUpdateDescriptorSets(r->dev, 2, writes, 0, NULL);
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof units_frag, smi.pCode = units_frag;
  VkShaderModule fs = VK_NULL_HANDLE;
  p_vkCreateShaderModule(r->dev, &smi, NULL, &fs);
  VkPipelineLayout scene_layout = s->layout;
  s->layout = layout;
  VkResult result;
  VkPipeline pipeline = make_pipeline(r, s, fs, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, &result);
  s->layout = scene_layout;
  check(result == VK_SUCCESS && pipeline, "a pipeline of twelve texture units");
  if (pipeline) {
    static const float white[15] = {-1, -1, 1, 1, 1, 3, -1, 1, 1, 1, -1, 3, 1, 1, 1};
    memcpy(s->vertex_map, white, sizeof white);
    s->layout = layout;
    draw_frame_with(r, s, pipeline, set, 0, 0, 0);
    s->layout = scene_layout;
    const unsigned char *picture = (const unsigned char *)s->readback_map;
    /* unit 1 green, unit 6 red, unit 11 blue: 0.25 red, 0.25 green, 0.5 blue */
    check(near_pixel(pixel(picture, W, 20, 20), 128, 64, 64, 255) && near_pixel(pixel(picture, W, 45, 50), 128, 64, 64, 255), "twelve texture units: each reads the image bound to it");
    if (!near_pixel(pixel(picture, W, 20, 20), 128, 64, 64, 255)) printf("    twelve units: pixel %08x\n", pixel(picture, W, 20, 20));
    p_vkDestroyPipeline(r->dev, pipeline, NULL);
  }
  if (fs) p_vkDestroyShaderModule(r->dev, fs, NULL);
  p_vkDestroyDescriptorPool(r->dev, pool, NULL);
  p_vkDestroyPipelineLayout(r->dev, layout, NULL);
  p_vkDestroyDescriptorSetLayout(r->dev, set_layout, NULL);
  for (unsigned k = 0; k < 3; ++k) {
    p_vkDestroyImageView(r->dev, views[k], NULL);
    p_vkDestroyImage(r->dev, images[k], NULL);
    p_vkFreeMemory(r->dev, memories[k], NULL);
  }
  p_vkDestroyBuffer(r->dev, staging, NULL);
  p_vkFreeMemory(r->dev, staging_memory, NULL);
}

/* Timestamps: two around a dispatch-free submission's work, read back with and without availability; they go up. */
static void test_timestamps(Rig *r) {
  VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qci.queryType = VK_QUERY_TYPE_TIMESTAMP, qci.queryCount = 4;
  VkQueryPool pool = VK_NULL_HANDLE;
  check(p_vkCreateQueryPool(r->dev, &qci, NULL, &pool) == VK_SUCCESS && pool, "a timestamp query pool");
  if (!pool) return;
  unsigned long long before[2] = {0, 0};
  check(p_vkGetQueryPoolResults(r->dev, pool, 0, 1, sizeof before, before, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY && before[1] == 0,
        "a timestamp that was not written is not available");
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  p_vkResetCommandBuffer(r->cb, 0);
  p_vkBeginCommandBuffer(r->cb, &cbi);
  p_vkCmdResetQueryPool(r->cb, pool, 0, 4);
  p_vkCmdWriteTimestamp(r->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
  p_vkCmdWriteTimestamp(r->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
  p_vkEndCommandBuffer(r->cb);
  submit_and_wait(r);
  unsigned long long t[2] = {0, 0};
  check(p_vkGetQueryPoolResults(r->dev, pool, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && t[0] != 0 && t[1] >= t[0],
        "timestamps are written and go up");
  p_vkDestroyQueryPool(r->dev, pool, NULL);
}

static void test_device(VkPhysicalDevice pd, unsigned index) {
  Rig r;
  memset(&r, 0, sizeof r);
  r.pd = pd;
  p_vkGetPhysicalDeviceProperties(pd, &r.props);
  p_vkGetPhysicalDeviceMemoryProperties(pd, &r.mem);
  printf("  adapter %u: %s (vendor %04x, device %04x, type %d, push constants %u bytes, %u texture units)\n", index, r.props.deviceName, r.props.vendorID, r.props.deviceID, r.props.deviceType,
         r.props.limits.maxPushConstantsSize, r.props.limits.maxPerStageDescriptorSamplers);
  check(r.props.deviceName[0] != 0 && r.props.vendorID != 0, "the adapter has a name and a vendor");
  check(r.props.apiVersion >= VK_API_VERSION_1_0 && VK_VERSION_MAJOR(r.props.apiVersion) == 1, "api version");
  check(r.props.limits.maxImageDimension2D >= 256 && r.props.limits.maxPushConstantsSize >= 16 && r.props.limits.maxBoundDescriptorSets >= 1, "limits");
  check(r.props.limits.maxViewports >= 1 && r.props.limits.minUniformBufferOffsetAlignment >= 1 && r.props.limits.nonCoherentAtomSize >= 1, "limits that programs divide by");
  check(r.mem.memoryTypeCount >= 1 && r.mem.memoryHeapCount >= 1, "memory types");
  unsigned nfam = 0;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfam, NULL);
  check(nfam >= 1, "a queue family");
  VkQueueFamilyProperties fam[4];
  if (nfam > 4) nfam = 4;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfam, fam);
  check(nfam >= 1 && (fam[0].queueFlags & VK_QUEUE_GRAPHICS_BIT) && fam[0].queueCount >= 1, "queue family 0 does graphics");
  VkFormatProperties fp;
  p_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
  check((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT), "B8G8R8A8_UNORM renders");
  p_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32G32_SFLOAT, &fp);
  check(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, "a float vertex format");
  VkImageFormatProperties ifp;
  check(p_vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp) == VK_SUCCESS &&
        ifp.maxExtent.width >= 256, "image format properties");
  check(p_vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_UNDEFINED, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp) == VK_ERROR_FORMAT_NOT_SUPPORTED, "an unsupported format");
  unsigned next = 0;
  p_vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
  check(next >= 1, "the swapchain extension");

  /* the device */
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = 0, qci.queueCount = 1, qci.pQueuePriorities = &priority;
  const char *extensions[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = extensions;
  check(p_vkCreateDevice(pd, &dci, NULL, &r.dev) == VK_SUCCESS && r.dev, "vkCreateDevice");
  if (!r.dev) return;
  p_vkGetDeviceQueue(r.dev, 0, 0, &r.queue);
  check(r.queue != NULL, "vkGetDeviceQueue");
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  check(p_vkCreateCommandPool(r.dev, &pci, NULL, &r.pool) == VK_SUCCESS, "vkCreateCommandPool");
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = r.pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
  check(p_vkAllocateCommandBuffers(r.dev, &cai, &r.cb) == VK_SUCCESS && r.cb, "vkAllocateCommandBuffers");
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  check(p_vkCreateFence(r.dev, &fci, NULL, &r.fence) == VK_SUCCESS, "vkCreateFence");

  /* the scene */
  Scene s;
  memset(&s, 0, sizeof s);
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D, ici.format = VK_FORMAT_B8G8R8A8_UNORM, ici.extent.width = W, ici.extent.height = H, ici.extent.depth = 1, ici.mipLevels = 1, ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT, ici.tiling = VK_IMAGE_TILING_OPTIMAL, ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE, ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  check(p_vkCreateImage(r.dev, &ici, NULL, &s.target) == VK_SUCCESS, "vkCreateImage (the target)");
  VkMemoryRequirements req;
  p_vkGetImageMemoryRequirements(r.dev, s.target, &req);
  check(req.size >= W * H * 4, "image memory requirements");
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size, mai.memoryTypeIndex = (unsigned)memory_type(&r.mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(p_vkAllocateMemory(r.dev, &mai, NULL, &s.target_memory) == VK_SUCCESS && p_vkBindImageMemory(r.dev, s.target, s.target_memory, 0) == VK_SUCCESS, "the target's memory");
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = s.target, vci.viewType = VK_IMAGE_VIEW_TYPE_2D, vci.format = ici.format;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, vci.subresourceRange.levelCount = 1, vci.subresourceRange.layerCount = 1;
  check(p_vkCreateImageView(r.dev, &vci, NULL, &s.target_view) == VK_SUCCESS, "vkCreateImageView");

  VkAttachmentDescription ad;
  memset(&ad, 0, sizeof ad);
  ad.format = ici.format, ad.samples = VK_SAMPLE_COUNT_1_BIT, ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub;
  memset(&sub, 0, sizeof sub);
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1, rpci.pAttachments = &ad, rpci.subpassCount = 1, rpci.pSubpasses = &sub;
  check(p_vkCreateRenderPass(r.dev, &rpci, NULL, &s.pass) == VK_SUCCESS, "vkCreateRenderPass");
  VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = s.pass, fbci.attachmentCount = 1, fbci.pAttachments = &s.target_view, fbci.width = W, fbci.height = H, fbci.layers = 1;
  check(p_vkCreateFramebuffer(r.dev, &fbci, NULL, &s.framebuffer) == VK_SUCCESS, "vkCreateFramebuffer");

  s.vertex = make_buffer(&r, 3 * 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s.vertex_memory, &s.vertex_map);
  s.uniform = make_buffer(&r, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s.uniform_memory, &s.uniform_map);
  s.readback = make_buffer(&r, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s.readback_memory, &s.readback_map);
  s.staging = make_buffer(&r, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s.staging_memory, &s.staging_map);
  /* the big triangle that covers the picture: clockwise as seen on the screen (Y is down), colour (1, 0.5, 0.25) */
  static const float triangle[15] = {-1, -1, 1, 0.5f, 0.25f, 3, -1, 1, 0.5f, 0.25f, -1, 3, 1, 0.5f, 0.25f};
  memcpy(s.vertex_map, triangle, sizeof triangle);
  static const float tint[4] = {1, 1, 1, 1};
  memcpy(s.uniform_map, tint, sizeof tint);

  VkDescriptorSetLayoutBinding bindings[2];
  memset(bindings, 0, sizeof bindings);
  bindings[0].binding = 0, bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bindings[0].descriptorCount = 1, bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  bindings[1].binding = 1, bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindings[1].descriptorCount = 1, bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 2, dslci.pBindings = bindings;
  check(p_vkCreateDescriptorSetLayout(r.dev, &dslci, NULL, &s.set_layout) == VK_SUCCESS, "vkCreateDescriptorSetLayout");
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1, plci.pSetLayouts = &s.set_layout;
  check(p_vkCreatePipelineLayout(r.dev, &plci, NULL, &s.layout) == VK_SUCCESS, "vkCreatePipelineLayout");
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1, dpci.poolSizeCount = 2, dpci.pPoolSizes = sizes;
  check(p_vkCreateDescriptorPool(r.dev, &dpci, NULL, &s.descriptor_pool) == VK_SUCCESS, "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = s.descriptor_pool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &s.set_layout;
  check(p_vkAllocateDescriptorSets(r.dev, &dsai, &s.set) == VK_SUCCESS, "vkAllocateDescriptorSets");

  /* the texture: 2 x 2, uploaded through the staging buffer */
  VkImageCreateInfo tci = ici;
  tci.format = VK_FORMAT_R8G8B8A8_UNORM, tci.extent.width = 2, tci.extent.height = 2, tci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  check(p_vkCreateImage(r.dev, &tci, NULL, &s.texture) == VK_SUCCESS, "vkCreateImage (the texture)");
  p_vkGetImageMemoryRequirements(r.dev, s.texture, &req);
  mai.allocationSize = req.size, mai.memoryTypeIndex = (unsigned)memory_type(&r.mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(p_vkAllocateMemory(r.dev, &mai, NULL, &s.texture_memory) == VK_SUCCESS && p_vkBindImageMemory(r.dev, s.texture, s.texture_memory, 0) == VK_SUCCESS, "the texture's memory");
  VkImageViewCreateInfo tvci = vci;
  tvci.image = s.texture, tvci.format = tci.format;
  check(p_vkCreateImageView(r.dev, &tvci, NULL, &s.texture_view) == VK_SUCCESS, "vkCreateImageView (the texture)");
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST, sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, sci.maxLod = 0;
  check(p_vkCreateSampler(r.dev, &sci, NULL, &s.sampler) == VK_SUCCESS, "vkCreateSampler");
  static const unsigned char texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};   /* red, green / blue, white (R, G, B, A) */
  memcpy(s.staging_map, texels, sizeof texels);
  p_vkResetCommandBuffer(r.cb, 0);
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  p_vkBeginCommandBuffer(r.cb, &cbi);
  VkBufferImageCopy up;
  memset(&up, 0, sizeof up);
  up.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, up.imageSubresource.layerCount = 1, up.imageExtent.width = 2, up.imageExtent.height = 2, up.imageExtent.depth = 1;
  p_vkCmdCopyBufferToImage(r.cb, s.staging, s.texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
  p_vkEndCommandBuffer(r.cb);
  submit_and_wait(&r);

  VkDescriptorBufferInfo dbi = {s.uniform, 0, 16};
  VkDescriptorImageInfo dii = {s.sampler, s.texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[2];
  memset(writes, 0, sizeof writes);
  writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = writes[1].dstSet = s.set;
  writes[0].dstBinding = 0, writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[0].pBufferInfo = &dbi;
  writes[1].dstBinding = 1, writes[1].descriptorCount = 1, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &dii;
  p_vkUpdateDescriptorSets(r.dev, 2, writes, 0, NULL);

  VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = sizeof tri_vert, smci.pCode = tri_vert;
  check(p_vkCreateShaderModule(r.dev, &smci, NULL, &s.vs) == VK_SUCCESS, "vkCreateShaderModule (vertex)");
  smci.codeSize = sizeof tri_frag, smci.pCode = tri_frag;
  check(p_vkCreateShaderModule(r.dev, &smci, NULL, &s.fs_color) == VK_SUCCESS, "vkCreateShaderModule (fragment)");
  smci.codeSize = sizeof tex_frag, smci.pCode = tex_frag;
  check(p_vkCreateShaderModule(r.dev, &smci, NULL, &s.fs_tex) == VK_SUCCESS, "vkCreateShaderModule (textured fragment)");
  const unsigned char *picture = (const unsigned char *)s.readback_map;

  /* flat colour */
  VkResult pr = VK_SUCCESS;
  VkPipeline flat = make_pipeline(&r, &s, s.fs_color, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, &pr);
  if (pr == VK_ERROR_FEATURE_NOT_PRESENT && r.props.limits.maxPushConstantsSize < 64) {
    printf("  (this adapter is too small for the pipeline: %d)\n", (int)pr);
  } else {
    check(pr == VK_SUCCESS && flat, "the flat colour pipeline");
    if (flat) {
      draw_frame(&r, &s, flat, 0.1f, 0.2f, 0.3f);
      check(near_pixel(pixel(picture, W, 8, 8), 64, 128, 255, 255), "flat: the triangle covers the picture (8, 8)");
      check(near_pixel(pixel(picture, W, 60, 3), 64, 128, 255, 255), "flat: ... (60, 3)");
      check(near_pixel(pixel(picture, W, 3, 60), 64, 128, 255, 255), "flat: ... (3, 60)");
      check(near_pixel(pixel(picture, W, 60, 60), 64, 128, 255, 255), "flat: ... (60, 60)");
      /* a change in the uniform buffer is seen by the next frame (the memory is mapped: no flush is needed) */
      const float dim[4] = {0.5f, 1, 0, 1};
      memcpy(s.uniform_map, dim, sizeof dim);
      draw_frame(&r, &s, flat, 0.1f, 0.2f, 0.3f);
      check(near_pixel(pixel(picture, W, 32, 32), 0, 128, 128, 255), "flat: the uniform tint (0.5, 1, 0) multiplies the colour");
      memcpy(s.uniform_map, tint, sizeof tint);
      p_vkDestroyPipeline(r.dev, flat, NULL);
    }
    /* back face culling, with the winding the triangle has (clockwise on the screen) */
    VkPipeline front_clockwise = make_pipeline(&r, &s, s.fs_color, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE, &pr);
    check(pr == VK_SUCCESS && front_clockwise, "the clockwise pipeline");
    if (front_clockwise) {
      draw_frame(&r, &s, front_clockwise, 0.1f, 0.2f, 0.3f);
      check(near_pixel(pixel(picture, W, 8, 8), 64, 128, 255, 255), "culling: a clockwise triangle is a front face of a clockwise pipeline");
      p_vkDestroyPipeline(r.dev, front_clockwise, NULL);
    }
    VkPipeline front_ccw = make_pipeline(&r, &s, s.fs_color, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, &pr);
    check(pr == VK_SUCCESS && front_ccw, "the counter-clockwise pipeline");
    if (front_ccw) {
      draw_frame(&r, &s, front_ccw, 0.1f, 0.2f, 0.3f);
      check(near_pixel(pixel(picture, W, 8, 8), 77, 51, 26, 255), "culling: ... and a back face of a counter-clockwise one (the clear colour is left)");
      p_vkDestroyPipeline(r.dev, front_ccw, NULL);
    }
  }

  /* the textured pipeline: two texture units are all a small adapter has, this uses one */
  VkPipeline textured = make_pipeline(&r, &s, s.fs_tex, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, &pr);
  if (pr == VK_ERROR_FEATURE_NOT_PRESENT && r.props.limits.maxPerStageDescriptorSamplers < 1) {
    printf("  (no texture units on this adapter)\n");
  } else {
    check(pr == VK_SUCCESS && textured, "the textured pipeline");
    if (textured) {
      static const float white[15] = {-1, -1, 1, 1, 1, 3, -1, 1, 1, 1, -1, 3, 1, 1, 1};
      memcpy(s.vertex_map, white, sizeof white);
      draw_frame(&r, &s, textured, 0, 0, 0);
      printf("  texture pixels: %08x %08x %08x %08x\n", pixel(picture, W, 8, 8), pixel(picture, W, 56, 8), pixel(picture, W, 8, 56), pixel(picture, W, 56, 56));
      /* the top left of the picture samples the first texel: Y is down in the picture and in the texture */
      check(near_pixel(pixel(picture, W, 8, 8), 0, 0, 255, 255), "texture: top left is red");
      check(near_pixel(pixel(picture, W, 56, 8), 0, 255, 0, 255), "texture: top right is green");
      check(near_pixel(pixel(picture, W, 8, 56), 255, 0, 0, 255), "texture: bottom left is blue");
      check(near_pixel(pixel(picture, W, 56, 56), 255, 255, 255, 255), "texture: bottom right is white");
      p_vkDestroyPipeline(r.dev, textured, NULL);
    }
  }

  test_depth_and_push(&r, &s);
  test_swapchain(&r);
  /* (after a present: the GPU work after it must still run - a present the host refused used to lose the context without a word) */
  test_compute(&r);
  test_compute_big(&r);
  test_cube_array(&r, &s);
  test_textures_in_vertex_and_compute(&r, &s);
  test_many_units(&r, &s);
  test_timestamps(&r);

  /* copies and clears that need no GPU */
  p_vkResetCommandBuffer(r.cb, 0);
  p_vkBeginCommandBuffer(r.cb, &cbi);
  VkClearColorValue cc = {{0.0f, 1.0f, 0.0f, 1.0f}};
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  p_vkCmdClearColorImage(r.cb, s.target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
  VkBufferImageCopy full;
  memset(&full, 0, sizeof full);
  full.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, full.imageSubresource.layerCount = 1, full.imageExtent.width = W, full.imageExtent.height = H, full.imageExtent.depth = 1;
  p_vkCmdCopyImageToBuffer(r.cb, s.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback, 1, &full);
  p_vkCmdFillBuffer(r.cb, s.staging, 0, 16, 0x01020304u);
  p_vkEndCommandBuffer(r.cb);
  submit_and_wait(&r);
  check(near_pixel(pixel(picture, W, 5, 5), 0, 255, 0, 255) && near_pixel(pixel(picture, W, 63, 63), 0, 255, 0, 255), "vkCmdClearColorImage and vkCmdCopyImageToBuffer");
  check(((const unsigned *)s.staging_map)[0] == 0x01020304u && ((const unsigned *)s.staging_map)[3] == 0x01020304u, "vkCmdFillBuffer");

  p_vkDeviceWaitIdle(r.dev);
  p_vkDestroySampler(r.dev, s.sampler, NULL);
  p_vkDestroyShaderModule(r.dev, s.vs, NULL);
  p_vkDestroyShaderModule(r.dev, s.fs_color, NULL);
  p_vkDestroyShaderModule(r.dev, s.fs_tex, NULL);
  p_vkDestroyDescriptorPool(r.dev, s.descriptor_pool, NULL);
  p_vkDestroyPipelineLayout(r.dev, s.layout, NULL);
  p_vkDestroyDescriptorSetLayout(r.dev, s.set_layout, NULL);
  p_vkDestroyFramebuffer(r.dev, s.framebuffer, NULL);
  p_vkDestroyRenderPass(r.dev, s.pass, NULL);
  p_vkDestroyImageView(r.dev, s.texture_view, NULL);
  p_vkDestroyImage(r.dev, s.texture, NULL);
  p_vkFreeMemory(r.dev, s.texture_memory, NULL);
  p_vkDestroyImageView(r.dev, s.target_view, NULL);
  p_vkDestroyImage(r.dev, s.target, NULL);
  p_vkFreeMemory(r.dev, s.target_memory, NULL);
  p_vkDestroyBuffer(r.dev, s.vertex, NULL);
  p_vkDestroyBuffer(r.dev, s.uniform, NULL);
  p_vkDestroyBuffer(r.dev, s.readback, NULL);
  p_vkDestroyBuffer(r.dev, s.staging, NULL);
  p_vkFreeMemory(r.dev, s.vertex_memory, NULL);
  p_vkFreeMemory(r.dev, s.uniform_memory, NULL);
  p_vkFreeMemory(r.dev, s.readback_memory, NULL);
  p_vkFreeMemory(r.dev, s.staging_memory, NULL);
  p_vkDestroyFence(r.dev, r.fence, NULL);
  p_vkDestroyCommandPool(r.dev, r.pool, NULL);
  p_vkDestroyDevice(r.dev, NULL);
}

int main(void) {
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  check(lib != NULL, "vulkan-1.dll loads");
  if (!lib) {
    printf("vulkantest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
  }
  pGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
  check(pGetInstanceProcAddr != NULL, "vkGetInstanceProcAddr is exported");
  check(GetProcAddress(lib, "vkCreateInstance") != NULL && GetProcAddress(lib, "vkEnumeratePhysicalDevices") != NULL && GetProcAddress(lib, "vkCmdDraw") != NULL, "the core functions are exported by name");
  if (!pGetInstanceProcAddr) return g_failed + 1;
  p_vkCreateInstance = (PFN_vkCreateInstance)pGetInstanceProcAddr(NULL, "vkCreateInstance");
  p_vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)pGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
  check(p_vkCreateInstance && p_vkEnumerateInstanceExtensionProperties, "global functions");
  check(pGetInstanceProcAddr(NULL, "vkNoSuchFunction") == NULL, "an unknown name gives NULL");

  unsigned next = 0;
  check(p_vkEnumerateInstanceExtensionProperties(NULL, &next, NULL) == VK_SUCCESS && next >= 2, "instance extensions");
  const char *bad_extension[1] = {"VK_NOT_AN_EXTENSION"};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.enabledExtensionCount = 1, ici.ppEnabledExtensionNames = bad_extension;
  VkInstance none = VK_NULL_HANDLE;
  check(p_vkCreateInstance(&ici, NULL, &none) == VK_ERROR_EXTENSION_NOT_PRESENT, "an extension that is not there is refused");

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vulkantest", app.apiVersion = VK_API_VERSION_1_0;
  const char *extensions[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  ici.pApplicationInfo = &app, ici.enabledExtensionCount = 2, ici.ppEnabledExtensionNames = extensions;
  check(p_vkCreateInstance(&ici, NULL, &g_instance) == VK_SUCCESS && g_instance, "vkCreateInstance");
  if (!g_instance) return g_failed + 1;
  load_all();
  unsigned count = 0;
  check(p_vkEnumeratePhysicalDevices(g_instance, &count, NULL) == VK_SUCCESS && count >= 1, "there is a physical device");
  VkPhysicalDevice devices[8];
  unsigned n = count > 8 ? 8 : count;
  check(p_vkEnumeratePhysicalDevices(g_instance, &n, devices) == VK_SUCCESS && n >= 1, "vkEnumeratePhysicalDevices");
  printf("vulkantest: %u adapter(s)\n", n);
  for (unsigned i = 0; i < n; ++i) test_device(devices[i], i);
  p_vkDestroyInstance(g_instance, NULL);
  printf("vulkantest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

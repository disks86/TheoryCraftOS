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

/* A Linux program that uses the shared Vulkan driver through libvulkan.a (src/Linux/libvulkan) and /dev/dxg: the extensions a Linux program gets, every physical device, a
 * triangle drawn with push constants and read back, a compute dispatch over storage buffers, and a swapchain of a headless surface presented. Freestanding (no C library), so
 * the same source builds for x86-64, AArch64 and RISC-V; tests/linux_vulkan.sh runs it on the kernel's CPU emulator with the software renderer as the adapter. Prints
 * "vulkan-linux: N checks, M failed" and exits with 1 when something failed. */
#include "../../src/Vulkan/khronos/vulkan/vulkan_core.h"
#include "../windows/vulkantest_shaders.h"

/* ---- a little runtime ---------------------------------------------------------------------------------------------------------------------------------------------------- */
#if defined(__x86_64__)
static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}
enum { NR_WRITE = 1, NR_EXIT_GROUP = 231 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
enum { NR_WRITE = 64, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys3(long n, long a, long b, long c) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
  return a0;
}
enum { NR_WRITE = 64, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif

static void out(const char *s) {
  long n = 0;
  while (s[n]) ++n;
  sys3(NR_WRITE, 1, (long)s, n);
}
static void out_u(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = (char)('0' + v % 10), v /= 10;
  while (v);
  out(b + i);
}
static void out_x(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = "0123456789abcdef"[v & 15], v >>= 4;
  while (v);
  out("0x");
  out(b + i);
}
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}
static void zero(void *p, unsigned long n) {
  unsigned char *q = p;
  while (n--) *q++ = 0;
}

static unsigned g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  out("FAIL line ");
  out_u((unsigned long)line);
  out(": ");
  out(what);
  out("\n");
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

/* ---- the device under test ----------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct Rig {
  VkPhysicalDevice pd;
  VkPhysicalDeviceMemoryProperties mem;
  VkDevice dev;
  VkQueue queue;
  VkCommandPool pool;
  VkCommandBuffer cb;
  VkFence fence;
} Rig;

static int memory_type(const Rig *r, unsigned bits, VkMemoryPropertyFlags want) {
  for (unsigned i = 0; i < r->mem.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (r->mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

static VkDeviceMemory allocate(Rig *r, VkMemoryRequirements req, VkMemoryPropertyFlags want) {
  int type = memory_type(r, req.memoryTypeBits, want);
  check(type >= 0, "a memory type");
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size, ai.memoryTypeIndex = type < 0 ? 0 : (unsigned)type;
  VkDeviceMemory m = VK_NULL_HANDLE;
  check(vkAllocateMemory(r->dev, &ai, NULL, &m) == VK_SUCCESS, "vkAllocateMemory");
  return m;
}

static VkBuffer make_buffer(Rig *r, VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapped) {
  VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size, ci.usage = usage, ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer b = VK_NULL_HANDLE;
  check(vkCreateBuffer(r->dev, &ci, NULL, &b) == VK_SUCCESS, "vkCreateBuffer");
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(r->dev, b, &req);
  *memory = allocate(r, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  check(vkBindBufferMemory(r->dev, b, *memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
  check(vkMapMemory(r->dev, *memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS && *mapped, "vkMapMemory");
  return b;
}

static VkShaderModule make_shader(Rig *r, const unsigned *code, unsigned long bytes) {
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes, ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  check(vkCreateShaderModule(r->dev, &ci, NULL, &m) == VK_SUCCESS, "vkCreateShaderModule");
  return m;
}

static void begin(Rig *r) {
  vkResetCommandBuffer(r->cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(r->cb, &bi) == VK_SUCCESS, "vkBeginCommandBuffer");
}
static void submit_and_wait(Rig *r) {
  check(vkEndCommandBuffer(r->cb) == VK_SUCCESS, "vkEndCommandBuffer");
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &r->cb;
  vkResetFences(r->dev, 1, &r->fence);
  check(vkQueueSubmit(r->queue, 1, &si, r->fence) == VK_SUCCESS, "vkQueueSubmit");
  check(vkWaitForFences(r->dev, 1, &r->fence, VK_TRUE, ~0ull) == VK_SUCCESS, "vkWaitForFences");
}

enum { W = 64, H = 64 };

static int near(unsigned got, unsigned want) {
  for (int i = 0; i < 32; i += 8) {
    int a = (int)((got >> i) & 255), b = (int)((want >> i) & 255);
    if (a - b > 2 || b - a > 2) return 0;
  }
  return 1;
}

/* A green triangle over the middle of a blue picture (push constants move it), read back. Pixels are R, G, B, A in memory (R8G8B8A8). */
static void test_triangle(Rig *r) {
  VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ic.imageType = VK_IMAGE_TYPE_2D, ic.format = VK_FORMAT_R8G8B8A8_UNORM, ic.extent.width = W, ic.extent.height = H, ic.extent.depth = 1, ic.mipLevels = 1, ic.arrayLayers = 1;
  ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL, ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage target = VK_NULL_HANDLE;
  check(vkCreateImage(r->dev, &ic, NULL, &target) == VK_SUCCESS, "vkCreateImage");
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(r->dev, target, &req);
  VkDeviceMemory target_memory = allocate(r, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkBindImageMemory(r->dev, target, target_memory, 0) == VK_SUCCESS, "vkBindImageMemory");
  VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = target, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = ic.format, vc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, vc.subresourceRange.levelCount = 1,
  vc.subresourceRange.layerCount = 1;
  VkImageView view = VK_NULL_HANDLE;
  check(vkCreateImageView(r->dev, &vc, NULL, &view) == VK_SUCCESS, "vkCreateImageView");

  VkAttachmentDescription att = {0};
  att.format = ic.format, att.samples = VK_SAMPLE_COUNT_1_BIT, att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1, rp.pAttachments = &att, rp.subpassCount = 1, rp.pSubpasses = &sub;
  VkRenderPass pass = VK_NULL_HANDLE;
  check(vkCreateRenderPass(r->dev, &rp, NULL, &pass) == VK_SUCCESS, "vkCreateRenderPass");
  VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fc.renderPass = pass, fc.attachmentCount = 1, fc.pAttachments = &view, fc.width = W, fc.height = H, fc.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  check(vkCreateFramebuffer(r->dev, &fc, NULL, &framebuffer) == VK_SUCCESS, "vkCreateFramebuffer");

  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 20};
  VkPipelineLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.pushConstantRangeCount = 1, lc.pPushConstantRanges = &range;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  check(vkCreatePipelineLayout(r->dev, &lc, NULL, &layout) == VK_SUCCESS, "vkCreatePipelineLayout");
  VkShaderModule vs = make_shader(r, push_vert, sizeof push_vert), fs = make_shader(r, flat_frag, sizeof flat_frag);
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
  VkVertexInputBindingDescription binding = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding, vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport = {0, 0, W, H, 0, 1};
  VkRect2D scissor = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1, vp.pViewports = &viewport, vp.scissorCount = 1, vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba = {0};
  ba.colorWriteMask = 15;
  VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1, cbs.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2, gp.pStages = stages, gp.pVertexInputState = &vi, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp, gp.pRasterizationState = &rs, gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cbs, gp.layout = layout, gp.renderPass = pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  check(vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline) == VK_SUCCESS, "vkCreateGraphicsPipelines");

  /* a triangle around the origin (x, y, then r, g, b, a), moved right by the push constant */
  static const float vertices[3][6] = {{-0.5f, -0.5f, 0, 1, 0, 1}, {0.5f, -0.5f, 0, 1, 0, 1}, {0.0f, 0.5f, 0, 1, 0, 1}};
  VkDeviceMemory vertex_memory, readback_memory;
  void *vertex_map, *readback_map;
  VkBuffer vertex = make_buffer(r, sizeof vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertex_memory, &vertex_map);
  VkBuffer readback = make_buffer(r, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_memory, &readback_map);
  {
    const unsigned char *from = (const unsigned char *)vertices;
    unsigned char *to = vertex_map;
    for (unsigned i = 0; i < sizeof vertices; ++i) to[i] = from[i];
  }
  const float push[5] = {0.25f, 0.0f, 0.0f, 0.0f, 0.5f};

  begin(r);
  VkClearValue clear;
  zero(&clear, sizeof clear);
  clear.color.float32[2] = 1.0f, clear.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = pass, rb.framebuffer = framebuffer, rb.renderArea.extent.width = W, rb.renderArea.extent.height = H, rb.clearValueCount = 1, rb.pClearValues = &clear;
  vkCmdBeginRenderPass(r->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(r->cb, 0, 1, &vertex, &offset);
  vkCmdPushConstants(r->cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
  vkCmdDraw(r->cb, 3, 1, 0, 0);
  vkCmdEndRenderPass(r->cb);
  VkBufferImageCopy copy;
  zero(&copy, sizeof copy);
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, copy.imageSubresource.layerCount = 1, copy.imageExtent.width = W, copy.imageExtent.height = H, copy.imageExtent.depth = 1;
  vkCmdCopyImageToBuffer(r->cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
  submit_and_wait(r);

  const unsigned *px = readback_map;
  const unsigned green = 0xFF00FF00u;      /* (R8G8B8A8 read as a little-endian word: red is the low byte, so blue is 0xFFFF0000) */
  check(near(px[2 * W + 2], 0xFFFF0000u), "the corner is the clear colour (blue)");
  check(near(px[(H / 2) * W + W / 2 + W / 8], green), "the middle (moved right by the push constant) is the triangle (green)");
  check(near(px[(H / 2) * W + 4], 0xFFFF0000u), "left of the moved triangle is clear");
  if (g_failed) {
    out("  corner ");
    out_x(px[2 * W + 2]);
    out(" middle ");
    out_x(px[(H / 2) * W + W / 2 + W / 8]);
    out("\n");
  }

  vkDestroyPipeline(r->dev, pipeline, NULL);
  vkDestroyShaderModule(r->dev, vs, NULL);
  vkDestroyShaderModule(r->dev, fs, NULL);
  vkDestroyPipelineLayout(r->dev, layout, NULL);
  vkDestroyFramebuffer(r->dev, framebuffer, NULL);
  vkDestroyRenderPass(r->dev, pass, NULL);
  vkDestroyImageView(r->dev, view, NULL);
  vkDestroyImage(r->dev, target, NULL);
  vkFreeMemory(r->dev, target_memory, NULL);
  vkDestroyBuffer(r->dev, vertex, NULL);
  vkFreeMemory(r->dev, vertex_memory, NULL);
  vkDestroyBuffer(r->dev, readback, NULL);
  vkFreeMemory(r->dev, readback_memory, NULL);
}

/* cs_add.comp: b[i] = a[i] * scale + offset.y for i < count, total = sum of (i + 1) over the 16 invocations, q[i] = ((i - 5) / 3) * 100 + (i - 5) mod 3. */
static void test_compute(Rig *r, const VkPhysicalDeviceFeatures *features) {
  enum { N = 16 };
  VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
  VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dl.bindingCount = 2, dl.pBindings = b;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  check(vkCreateDescriptorSetLayout(r->dev, &dl, NULL, &set_layout) == VK_SUCCESS, "vkCreateDescriptorSetLayout");
  VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  VkPipelineLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.setLayoutCount = 1, lc.pSetLayouts = &set_layout, lc.pushConstantRangeCount = 1, lc.pPushConstantRanges = &range;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  check(vkCreatePipelineLayout(r->dev, &lc, NULL, &layout) == VK_SUCCESS, "vkCreatePipelineLayout (compute)");
  VkShaderModule cs = make_shader(r, add_comp, sizeof add_comp);
  VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cp.stage.module = cs, cp.stage.pName = "main", cp.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  check(vkCreateComputePipelines(r->dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipeline) == VK_SUCCESS, "vkCreateComputePipelines");

  VkDeviceMemory in_memory, out_memory;
  void *in_map, *out_map;
  VkBuffer in = make_buffer(r, 8 + N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &in_memory, &in_map);
  VkBuffer outb = make_buffer(r, 36 + N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out_memory, &out_map);
  float *src = in_map;
  src[0] = 0, src[1] = 0.5f;
  for (int i = 0; i < N; ++i) src[2 + i] = (float)i;
  zero(out_map, 36 + N * 4);

  VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pc.maxSets = 1, pc.poolSizeCount = 1, pc.pPoolSizes = &ps;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  check(vkCreateDescriptorPool(r->dev, &pc, NULL, &dpool) == VK_SUCCESS, "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  sa.descriptorPool = dpool, sa.descriptorSetCount = 1, sa.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  check(vkAllocateDescriptorSets(r->dev, &sa, &set) == VK_SUCCESS, "vkAllocateDescriptorSets");
  VkDescriptorBufferInfo bi[2] = {{in, 0, VK_WHOLE_SIZE}, {outb, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 2, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w.pBufferInfo = bi;
  vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);

  struct {
    float scale;
    unsigned count;
  } push = {2.0f, N};
  begin(r);
  vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
  vkCmdPushConstants(r->cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
  vkCmdDispatch(r->cb, 2, 1, 1);      /* 2 groups of 4 x 2 = 16 invocations */
  submit_and_wait(r);

  const unsigned *total = out_map;
  const int *q = (const int *)((const unsigned char *)out_map + 4);
  (void)features;
  check(*total == N * (N + 1) / 2, "atomicAdd over every invocation");
  int ok = 1;
  for (int i = 0; i < 8; ++i) {
    int v = i - 5;
    ok &= q[i] == (v / 3) * 100 + ((v % 3) + 3) % 3;      /* (/ truncates: OpSDiv; % takes the sign of the divisor: glslc makes it OpSMod) */
  }
  check(ok, "signed division and remainder");
  const float *b0 = (const float *)((const unsigned char *)out_map + 36);      /* (std430: total at 0, q[8] at 4, the runtime array b[] at 36) */
  ok = 1;
  for (int i = 0; i < N; ++i) ok &= b0[i] == (float)i * 2.0f + 0.5f;
  check(ok, "b[i] = a[i] * scale + offset.y");
  if (!ok) {
    out("  b[1] bits ");
    out_x(((const unsigned *)b0)[1]);
    out("\n");
  }

  vkDestroyDescriptorPool(r->dev, dpool, NULL);
  vkDestroyPipeline(r->dev, pipeline, NULL);
  vkDestroyShaderModule(r->dev, cs, NULL);
  vkDestroyPipelineLayout(r->dev, layout, NULL);
  vkDestroyDescriptorSetLayout(r->dev, set_layout, NULL);
  vkDestroyBuffer(r->dev, in, NULL);
  vkFreeMemory(r->dev, in_memory, NULL);
  vkDestroyBuffer(r->dev, outb, NULL);
  vkFreeMemory(r->dev, out_memory, NULL);
}

/* A headless surface: its swapchain's images can be cleared and presented (onto the display). */
static void test_headless_swapchain(Rig *r, VkInstance instance) {
  VkHeadlessSurfaceCreateInfoEXT hc = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  check(vkCreateHeadlessSurfaceEXT(instance, &hc, NULL, &surface) == VK_SUCCESS && surface, "vkCreateHeadlessSurfaceEXT");
  VkBool32 supported = VK_FALSE;
  check(vkGetPhysicalDeviceSurfaceSupportKHR(r->pd, 0, surface, &supported) == VK_SUCCESS && supported, "queue family 0 presents to the surface");
  VkSurfaceCapabilitiesKHR caps;
  check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->pd, surface, &caps) == VK_SUCCESS && caps.minImageCount >= 1, "surface capabilities");
  VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sc.surface = surface, sc.minImageCount = 2, sc.imageFormat = VK_FORMAT_B8G8R8A8_UNORM, sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sc.imageExtent.width = 320, sc.imageExtent.height = 240, sc.imageArrayLayers = 1, sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, sc.presentMode = VK_PRESENT_MODE_FIFO_KHR, sc.clipped = VK_TRUE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  check(vkCreateSwapchainKHR(r->dev, &sc, NULL, &swapchain) == VK_SUCCESS, "vkCreateSwapchainKHR");
  uint32_t index = 99;
  check(vkAcquireNextImageKHR(r->dev, swapchain, ~0ull, VK_NULL_HANDLE, VK_NULL_HANDLE, &index) == VK_SUCCESS && index < 8, "vkAcquireNextImageKHR");
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(r->dev, swapchain, &count, NULL);
  VkImage images[8];
  if (count > 8) count = 8;
  vkGetSwapchainImagesKHR(r->dev, swapchain, &count, images);
  check(count >= 2, "two swapchain images");
  if (index < count) {
    begin(r);
    VkClearColorValue c;
    zero(&c, sizeof c);
    c.float32[1] = 1.0f, c.float32[3] = 1.0f;
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(r->cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
    submit_and_wait(r);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1, pi.pSwapchains = &swapchain, pi.pImageIndices = &index;
    check(vkQueuePresentKHR(r->queue, &pi) == VK_SUCCESS, "vkQueuePresentKHR (onto the display)");
  }
  vkDestroySwapchainKHR(r->dev, swapchain, NULL);
  vkDestroySurfaceKHR(instance, surface, NULL);
}

static void test_device(VkInstance instance, VkPhysicalDevice pd, unsigned number) {
  Rig rig, *r = &rig;
  zero(r, sizeof *r);
  r->pd = pd;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  out("device ");
  out_u(number);
  out(": ");
  out(props.deviceName);
  out("\n");
  check(props.deviceName[0] != 0, "the device has a name");
  check(VK_API_VERSION_MAJOR(props.apiVersion) == 1, "Vulkan 1.x");
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(pd, &features);
  vkGetPhysicalDeviceMemoryProperties(pd, &r->mem);
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, NULL);
  VkQueueFamilyProperties family;
  uint32_t one = 1;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &one, &family);
  check(families >= 1 && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT), "a graphics queue family");

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex = 0, qc.queueCount = 1, qc.pQueuePriorities = &priority;
  const char *ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.queueCreateInfoCount = 1, dc.pQueueCreateInfos = &qc, dc.enabledExtensionCount = 1, dc.ppEnabledExtensionNames = ext;
  if (vkCreateDevice(pd, &dc, NULL, &r->dev) != VK_SUCCESS) {
    check(0, "vkCreateDevice");
    return;
  }
  vkGetDeviceQueue(r->dev, 0, 0, &r->queue);
  VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  check(vkCreateCommandPool(r->dev, &pc, NULL, &r->pool) == VK_SUCCESS, "vkCreateCommandPool");
  VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = r->pool, ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ca.commandBufferCount = 1;
  check(vkAllocateCommandBuffers(r->dev, &ca, &r->cb) == VK_SUCCESS, "vkAllocateCommandBuffers");
  VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  check(vkCreateFence(r->dev, &fc, NULL, &r->fence) == VK_SUCCESS, "vkCreateFence");

  test_triangle(r);
  if (family.queueFlags & VK_QUEUE_COMPUTE_BIT) test_compute(r, &features);
  test_headless_swapchain(r, instance);

  vkDestroyFence(r->dev, r->fence, NULL);
  vkDestroyCommandPool(r->dev, r->pool, NULL);
  vkDestroyDevice(r->dev, NULL);
}

static int run(void) {
  uint32_t n = 0;
  check(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) == VK_SUCCESS && n >= 2, "instance extensions");
  VkExtensionProperties props[16];
  if (n > 16) n = 16;
  vkEnumerateInstanceExtensionProperties(NULL, &n, props);
  int surface = 0, headless = 0, win32 = 0;
  for (uint32_t i = 0; i < n; ++i) {
    surface |= streq(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
    headless |= streq(props[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    win32 |= streq(props[i].extensionName, "VK_KHR_win32_surface");
  }
  check(surface && headless, "VK_KHR_surface and VK_EXT_headless_surface");
  check(!win32, "no VK_KHR_win32_surface on Linux");

  const char *ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vulkantest-linux", app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ic.pApplicationInfo = &app, ic.enabledExtensionCount = 2, ic.ppEnabledExtensionNames = ext;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&ic, NULL, &instance) != VK_SUCCESS) {
    check(0, "vkCreateInstance");
    return 1;
  }
  check(vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT") == (PFN_vkVoidFunction)vkCreateHeadlessSurfaceEXT, "vkGetInstanceProcAddr finds the platform's surface function");
  check(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR") == NULL, "and not Windows'");
  check(vkGetInstanceProcAddr(instance, "vkQueueSubmit") == (PFN_vkVoidFunction)vkQueueSubmit, "vkGetInstanceProcAddr finds a shared function");

  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(instance, &count, NULL) == VK_SUCCESS && count >= 1, "at least one physical device (the kernel's adapters through /dev/dxg)");
  VkPhysicalDevice devices[8];
  if (count > 8) count = 8;
  vkEnumeratePhysicalDevices(instance, &count, devices);
  for (uint32_t i = 0; i < count; ++i) test_device(instance, devices[i], i);
  vkDestroyInstance(instance, NULL);
  return 0;
}

__attribute__((used)) void start_c(void) {
  run();
  out("vulkan-linux: ");
  out_u(g_checks);
  out(" checks, ");
  out_u(g_failed);
  out(" failed\n");
  sys3(NR_EXIT_GROUP, g_failed ? 1 : 0, 0, 0);
  for (;;) {
  }
}

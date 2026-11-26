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

/* The kernel's own Vulkan: the shared driver's Venus backend (src/Vulkan/vk_venus.inc, docs/Venus.md) compiled into the kernel (KernelVulkan.c), so that kernel code
 * (the Venus graphics device, VenusDevice.hpp) calls the host's Vulkan driver the way a program does. The backend reaches the host through a render-node session
 * of the kernel's own (graphics::virtgpu::Session over the system's virtio-gpu, KernelVulkanOs.cpp) instead of /dev/dri/renderD128 or NtTcosVirtGpu, and is built
 * without presentation (the kernel shows images itself). Everything else - the ring, replies, big command streams, blobs for host-visible memory, polled waits,
 * what is offered - is the same code that programs run.
 *
 * The functions are reached through one table (tcos_kernel_vulkan()); the list is TCOS_KVK_PROCS. Shared by C (KernelVulkan.c) and C++ (the device). */
#ifndef TCOS_KERNEL_VULKAN_H
#define TCOS_KERNEL_VULKAN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "Vulkan/khronos/vulkan/vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* X(name): vk<name> of the Venus backend. */
#define TCOS_KVK_PROCS(X)                                                                                                                                   \
  X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures)      \
  X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties)               \
  X(GetPhysicalDeviceFormatProperties2) X(GetPhysicalDeviceImageFormatProperties) X(GetPhysicalDeviceImageFormatProperties2) X(EnumerateDeviceExtensionProperties) \
  X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(DeviceWaitIdle) X(QueueWaitIdle) X(QueueSubmit) X(QueueSubmit2)                          \
  X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges)                                           \
  X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateBufferView) X(DestroyBufferView)                                   \
  X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(GetImageSubresourceLayout) X(CreateImageView) X(DestroyImageView)           \
  X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                    \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline)                                       \
  X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets)               \
  X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)       \
  X(ResetCommandBuffer) X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(GetFenceStatus) X(CreateSemaphore) X(DestroySemaphore)                 \
  X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(ResetQueryPool)                                                                                \
  X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindVertexBuffers2) X(CmdBindIndexBuffer) X(CmdBindDescriptorSets)  \
  X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdDispatch) X(CmdDispatchIndirect) X(CmdSetViewport)          \
  X(CmdSetScissor) X(CmdSetDepthBounds) X(CmdSetDepthBoundsTestEnable) X(CmdSetStencilReference) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetBlendConstants) X(CmdSetDepthBias) X(CmdSetLineWidth) \
  X(CmdSetCullMode) X(CmdSetFrontFace) X(CmdSetPrimitiveTopology) X(CmdSetPrimitiveRestartEnable) X(CmdPipelineBarrier) X(CmdPipelineBarrier2)                 \
  X(CmdCopyBuffer) X(CmdCopyImage) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdBlitImage) X(CmdResolveImage) X(CmdClearColorImage)                    \
  X(CmdClearDepthStencilImage) X(CmdClearAttachments) X(CmdFillBuffer) X(CmdUpdateBuffer) X(CmdBeginQuery) X(CmdEndQuery) X(CmdResetQueryPool)                  \
  X(CmdWriteTimestamp) X(CmdCopyQueryPoolResults) X(CmdBeginConditionalRenderingEXT) X(CmdEndConditionalRenderingEXT)                                           \
  X(CmdBindTransformFeedbackBuffersEXT) X(CmdBeginTransformFeedbackEXT) X(CmdEndTransformFeedbackEXT) X(CmdBeginQueryIndexedEXT) X(CmdEndQueryIndexedEXT)       \
  X(CmdDrawIndirectByteCountEXT) X(CmdSetSampleLocationsEXT) X(GetPhysicalDeviceMultisamplePropertiesEXT) X(CmdSetFragmentShadingRateKHR)              \
  X(GetPhysicalDeviceFragmentShadingRatesKHR) X(QueueBindSparse) X(GetImageSparseMemoryRequirements) X(GetPhysicalDeviceSparseImageFormatProperties)

typedef struct TcosKernelVulkan {
#define TCOS_KVK_MEMBER(name) PFN_vk##name name;
  TCOS_KVK_PROCS(TCOS_KVK_MEMBER)
#undef TCOS_KVK_MEMBER
} TcosKernelVulkan;

/* The table (the same object every call). The backend opens the system's render node when an instance is made: vkCreateInstance fails
 * (VK_ERROR_INCOMPATIBLE_DRIVER) when the machine has no virtio-gpu with the Venus context type, blobs and a host-visible window. */
const TcosKernelVulkan *tcos_kernel_vulkan(void);
/* Whether the system's render node offers Venus with the wire format this protocol speaks (opens and closes a probe session). */
bool tcos_kernel_vulkan_available(void);

/* ---- what KernelVulkan.c needs from the kernel (KernelVulkanOs.cpp) ---- */
void *tcos_kvk_alloc(size_t bytes);   /* zeroed; NULL when out of memory */
void tcos_kvk_free(void *p);
void tcos_kvk_trace(const char *text);
uint64_t tcos_kvk_now_ns(void);
void tcos_kvk_sleep_ns(uint64_t ns);
int64_t tcos_kvk_virtgpu_open(void);
int tcos_kvk_virtgpu_ioctl(int64_t node, uint32_t request, void *argument);
void *tcos_kvk_virtgpu_map(int64_t node, uint64_t offset, uint64_t size);
void tcos_kvk_virtgpu_unmap(int64_t node, void *address, uint64_t size);
void tcos_kvk_virtgpu_close(int64_t node);

#ifdef __cplusplus
}
#endif
#endif

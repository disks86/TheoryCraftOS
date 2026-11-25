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

// The kernel's 3D device on the host's Vulkan driver (docs/Venus.md, "The kernel's device"): IGraphicsDevice over the kernel's own Vulkan (KernelVulkan.h,
// VenusContext), the device QEMU's virtio-gpu offers with venus=on. Everything that draws through IGraphicsDevice - the Direct3D layer, OpenGL and WebGL, the
// desktop, the Vulkan driver's Direct3D backend - runs on the host's GPU through it, as it does through VirglDevice on virglrenderer's OpenGL.
//
// How it maps the interface:
//  - Buffers are host-visible memory the kernel maps (IBuffer::map is that memory); images are device-local optimal images, always in the GENERAL layout, filled
//    and read through a staging buffer by copies on the queue. CPU transfers are 0xAARRGGBB words (WidePixels.hpp's Vec4s for the wide formats), rows top first,
//    which is how Vulkan keeps an image: there are no flips anywhere.
//  - Shader modules keep their TGSI text; a pipeline compiles it to SPIR-V (TgsiSpirv.hpp) with what the pipeline and the bound views say, and keeps a variant for
//    each binding shape it meets (the value types of the bound textures and storage images, the topology).
//  - Constants (IGraphicsDevice's push block, with the samplers' comparison rows at 256) are a uniform buffer: each draw's block goes into a per-submit ring, bound
//    with a dynamic offset. Textures, storage buffers and images are descriptor sets of the pipeline's layout, made once per binding shape and reused.
//  - submit() records the CommandRecorder's list into a Vulkan command buffer, submits it and waits for its fence: every call that comes after it sees its results
//    (as the interface says), and nothing it used can be in flight when it returns.
//  - present() copies the image into the virtio-gpu's own 2D screen (VirtioGpu::blit + present), which every monitor layout and the console already use.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Graphics/Graphics3D/Venus/VenusContext.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
class IGraphicsProcessingUnit;
}

namespace graphics {

using venus::VenusCaps;
using venus::VenusContext;
using venus::vk_struct;

class VenusBuffer;
class VenusImage;
class VenusShaderModule;
class VenusPipeline;
class VenusCommands;

// A host-visible allocation the device maps (buffers, the staging buffer, the constant ring).
struct VenusMemory {
  VkDeviceMemory memory = nullptr;
  VkBuffer buffer = nullptr;
  uint64_t size = 0;
  uint8_t* map = nullptr;
};

class VenusDevice final : public IGraphicsDevice {
 public:
  // `screen` shows presented images (the virtio-gpu's own 2D screen); `account`, when given, is told of the memory the device's objects take and give back (the
  // virtio-gpu's video memory figure), with `account_context`.
  using MemoryAccount = void (*)(void* context, int64_t bytes);
  VenusDevice(allocators::IAllocator& allocator, hardware::IGraphicsProcessingUnit& screen, MemoryAccount account = nullptr, void* account_context = nullptr);
  ~VenusDevice();
  VenusDevice(const VenusDevice&) = delete;
  VenusDevice& operator=(const VenusDevice&) = delete;

  // Opens the host's device and makes what every submit uses. False: there is no Venus (or the host's device lacks what the device needs); nothing stays made.
  [[nodiscard]] bool initialize();

  DeviceInfo info() const override;
  uint32_t format_caps(Format format) const override;
  uint32_t max_image_dimension() const override;
  IBuffer* create_buffer(uint64_t size, uint32_t usage) override;
  void destroy_buffer(IBuffer* buffer) override;
  IImage* create_image(const ImageDesc& desc) override;
  void destroy_image(IImage* image) override;
  IShaderModule* create_shader_module(const ShaderModuleDesc& desc) override;
  void destroy_shader_module(IShaderModule* module) override;
  IPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
  IPipeline* create_compute_pipeline(const IShaderModule* shader) override;
  void destroy_pipeline(IPipeline* pipeline) override;
  ICommandBuffer* create_command_buffer() override;
  void destroy_command_buffer(ICommandBuffer* commands) override;
  void interrupt_clock() override { __atomic_store_n(&clock_interrupted_, true, __ATOMIC_RELEASE); }
  bool submit(ICommandBuffer* commands) override;
  bool present(IImage* image) override;

  // What this host's Vulkan device lacks for the device to behave as virgl does, beyond what info() shows (features it reports but would refuse, or approximate,
  // in some of their uses): bits of ParityGap, 0 when nothing. RenderNode's AUTO choice keeps virgl while any is set.
  enum ParityGap : uint32_t {
    kGapNullDescriptor = 1u << 0,     // robustness2 nullDescriptor: unbound units, buffers and images read zero
    kGapProvokingLast = 1u << 1,      // VK_EXT_provoking_vertex last-vertex mode: GL's flat varyings
    kGapDepthBiasFloat = 1u << 2,     // VK_EXT_depth_bias_control's float representation: Direct3D's bias of normalised depth formats
    kGapSampleLocations = 1u << 3,    // VK_EXT_sample_locations for every offered sample count: rasterization without multisampling on multisampled targets
    kGapCustomBorder = 1u << 4,       // VK_EXT_custom_border_color: border colours other than the three built-in ones
    kGapDivisor = 1u << 5,            // VK_EXT_vertex_attribute_divisor with zero divisors: instance step rates
    kGapDepthClamp = 1u << 6,         // depthClamp: pipelines without depth clipping
  };
  uint32_t parity_gaps() const;

  // ---- for the device's objects ----
  VenusContext& context() { return ctx_; }
  const TcosKernelVulkan& vk() const { return ctx_.vk(); }
  VkDevice device() const { return ctx_.device(); }
  allocators::IAllocator& allocator() { return allocator_; }
  // Host-visible memory of `size` bytes as a buffer of `usage` (mapped; zeroed). False when the host is out of memory or the window is full.
  [[nodiscard]] bool make_memory(uint64_t size, VkBufferUsageFlags usage, VenusMemory* out);
  void free_memory(VenusMemory* m);
  // One-off work on the queue (copies to and from images): records with `record`, submits, waits. False if anything failed.
  template <class F>
  bool run_once(F&& record) {
    if (!begin_once()) return false;
    record(once_);
    return end_once();
  }
  // The staging buffer, at least `size` bytes (it grows; the old one goes). nullptr when memory ran out. Only with staging_lock() held, from filling it to reading
  // it back: images are written and read, and frames presented, from several threads, and each of them stages through this one buffer.
  VenusMemory* staging(uint64_t size);
  kernel::Mutex& staging_lock() { return staging_lock_; }   // (taken before the queue's lock, never while holding it)
  // The format the host keeps an image of `format` in (VK_FORMAT_UNDEFINED: the device does not take it), and whether the CPU swaps R and B for it.
  VkFormat vk_format(Format format, bool storage, bool* swap_rb) const;
  bool format_supports(VkFormat f, VkFormatFeatureFlags features) const;
  // Whether images of a 96-bit format are kept as 128-bit ones (the host has no optimal images of it).
  bool widened_96(Format f) const;
  // Pipelines, descriptor sets (a new pool; the newest one, nullptr before the first)
  VkDescriptorPool descriptor_pool();
  VkDescriptorPool descriptor_pool_current();
  VenusMemory& constants_ring() { return constants_; }
  // The query pools (made on first use; every submit resets the ones it uses): occlusion, pipeline statistics (all eleven), transform feedback streams,
  // timestamps. kQuerySegments segments of each a submit.
  static constexpr uint32_t kQuerySegments = 4096, kTimestamps = 512, kStreamOutputCounters = 4096;   // (counter slots: one per binding and one per draw that writes it)
  enum QueryPoolKind : uint32_t { kPoolOcclusion, kPoolStatistics, kPoolStreams, kPoolTimestamps, kPoolKinds };
  VkQueryPool query_pool(QueryPoolKind kind);
  // The counters of stream output bindings (host-visible: the replay writes each binding's start, the GPU where it got to).
  VenusMemory* stream_output_counters();
  // The pipeline of a colour resolve to the least or greatest sample (a full-screen triangle whose fragment shader fetches every sample), for a target format and a
  // sample count; made once each. nullptr when it cannot be made.
  VenusPipeline* extreme_resolve_pipeline(VkFormat target, uint32_t samples, bool greatest);
  // A single-sampled colour image of `format` and `extent` a resolve goes through when the destination's format differs (kept for the next).
  VenusImage* resolve_scratch(Format format, Extent2D extent);
  bool clock_interrupted() const { return __atomic_load_n(&clock_interrupted_, __ATOMIC_ACQUIRE); }
  void clear_clock_interrupted() { __atomic_store_n(&clock_interrupted_, false, __ATOMIC_RELEASE); }
  // submit's command buffer: begun (the queue's lock taken), and submitted, waited for and reset (the lock let go). Also used to split a long submit.
  VkCommandBuffer begin_main();
  bool finish_main(VkCommandBuffer cb);
  bool lock_queue();
  void unlock_queue();
  uint32_t uniform_alignment() const { return uniform_alignment_; }
  VkSampler sampler_for(const SamplerDesc& s, bool integer_texture);
  void add_video_memory(int64_t bytes);
  // A line of diagnostics (why a shader or a pipeline was refused): the kernel's log (tcos_kvk_trace).
  static void trace(StringView what, uint32_t line, StringView message);

 private:
  friend class VenusCommands;
  bool begin_once();
  bool end_once();
  bool wait_fence(VkFence fence);

  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit& screen_;
  MemoryAccount account_;
  void* account_context_;
  VenusContext ctx_;
  VkCommandPool pool_ = nullptr;
  VkCommandBuffer once_ = nullptr;     // run_once's command buffer
  VkCommandBuffer main_ = nullptr;     // submit's
  VkFence fence_ = nullptr;
  VenusMemory staging_;
  VenusMemory constants_;              // the per-submit ring of constant blocks
  VenusMemory so_counters_;
  VkQueryPool pools_by_kind_[kPoolKinds] = {};
  List<VkDescriptorPool> pools_;
  struct CachedSampler {
    SamplerDesc desc;
    bool integer = false;
    VkSampler sampler = nullptr;
  };
  List<CachedSampler> samplers_;
  struct ResolvePipeline {
    VkFormat target = VK_FORMAT_UNDEFINED;
    uint32_t samples = 0;
    bool greatest = false;
    VenusShaderModule* vs = nullptr;
    VenusShaderModule* fs = nullptr;
    VenusPipeline* pipeline = nullptr;
  };
  List<ResolvePipeline> resolve_pipelines_;
  VenusImage* resolve_scratch_ = nullptr;
  uint32_t uniform_alignment_ = 256;
  uint32_t max_image_dimension_ = 4096;
  uint32_t sample_counts_ = 0;
  kernel::Mutex staging_lock_;        // staging_ and what is in it
  kernel::Mutex queue_lock_;          // the queue and the two command buffers (images are written from several threads)
  bool clock_interrupted_ = false;
  bool initialized_ = false;
};

}  // namespace graphics

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

// The Venus device's resources: the device itself, formats, buffers, images and their CPU transfers, samplers, presentation (VenusDevice.hpp). Pipelines are in
// VenusPipeline.cpp, the command replay in VenusSubmit.cpp.
#include "Graphics/Graphics3D/Venus/VenusDevice.hpp"
#include <new>
#include "Graphics/BlockCompression.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Graphics/Graphics3D/Venus/VenusObjects.hpp"
#include "Graphics/WidePixels.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Utility/ByteStream.hpp"

namespace graphics {

namespace {
constexpr uint64_t kConstantRingBytes = 8u << 20;   // (a submit's constant blocks: 4368 bytes each at most, so ~1900 draws; more and the submit is split)
constexpr uint64_t kStagingStart = 4u << 20;
}  // namespace

VenusDevice::VenusDevice(allocators::IAllocator& allocator, hardware::IGraphicsProcessingUnit& screen, MemoryAccount account, void* account_context)
    : allocator_(allocator), screen_(screen), account_(account), account_context_(account_context), pools_(allocator), samplers_(allocator), resolve_pipelines_(allocator) {}

void VenusDevice::trace(StringView what, uint32_t line, StringView message) {
  char text[200];
  uint32_t n = 0;
  auto put = [&](StringView s) {
    for (char ch : s)
      if (n + 1 < sizeof(text)) text[n++] = ch;
  };
  put("venus device: "), put(what);
  if (line) {
    char digits[12];
    uint32_t k = 0;
    do digits[k++] = static_cast<char>('0' + line % 10), line /= 10;
    while (line);
    put(" (line ");
    while (k) put(StringView(&digits[--k], 1));
    put(")");
  }
  put(": "), put(message);
  text[n] = 0;
  tcos_kvk_trace(text);
}

VenusDevice::~VenusDevice() {
  if (!ctx_.opened()) return;
  vk().DeviceWaitIdle(device());
  for (ResolvePipeline& r : resolve_pipelines_) {
    destroy_pipeline(r.pipeline);
    destroy_shader_module(r.fs);
    destroy_shader_module(r.vs);
  }
  destroy_image(resolve_scratch_);
  free_rate_attachment();
  for (CachedSampler& s : samplers_) vk().DestroySampler(device(), s.sampler, nullptr);
  for (VkDescriptorPool p : pools_) vk().DestroyDescriptorPool(device(), p, nullptr);
  for (VkQueryPool p : pools_by_kind_)
    if (p) vk().DestroyQueryPool(device(), p, nullptr);
  free_memory(&so_counters_);
  free_memory(&staging_);
  free_memory(&constants_);
  if (fence_) vk().DestroyFence(device(), fence_, nullptr);
  if (pool_) vk().DestroyCommandPool(device(), pool_, nullptr);
  ctx_.close();
}

bool VenusDevice::initialize() {
  if (!ctx_.open()) return false;
  const VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, ctx_.queue_family()};
  VkCommandBuffer buffers[2] = {};
  const VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, nullptr, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
  const VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
  if (vk().CreateCommandPool(device(), &pci, nullptr, &pool_) != VK_SUCCESS) {
    pool_ = nullptr;
    ctx_.close();
    return false;
  }
  VkCommandBufferAllocateInfo ai = cai;
  ai.commandPool = pool_;
  if (vk().AllocateCommandBuffers(device(), &ai, buffers) != VK_SUCCESS || vk().CreateFence(device(), &fci, nullptr, &fence_) != VK_SUCCESS ||
      !make_memory(kConstantRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &constants_) || !staging(kStagingStart)) {
    fence_ = nullptr;
    if (pool_) vk().DestroyCommandPool(device(), pool_, nullptr);
    pool_ = nullptr;
    free_memory(&constants_);
    ctx_.close();
    return false;
  }
  once_ = buffers[0], main_ = buffers[1];
  const VkPhysicalDeviceLimits& l = ctx_.properties().limits;
  uniform_alignment_ = static_cast<uint32_t>(l.minUniformBufferOffsetAlignment > 16 ? l.minUniformBufferOffsetAlignment : 16);
  max_image_dimension_ = l.maxImageDimension2D < 16384 ? l.maxImageDimension2D : 16384;
  const VkSampleCountFlags counts = l.framebufferColorSampleCounts & l.framebufferDepthSampleCounts & l.sampledImageColorSampleCounts;
  for (uint32_t n = 2; n <= 16; n *= 2)
    if (counts & n) sample_counts_ |= 1u << n;
  // (coarse shading's attachment is R8_UINT, filled by blits from the Direct3D layer's shading-rate image)
  shading_rate_usable_ = ctx_.caps().shading_rate &&
                         format_supports(VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
  initialized_ = true;
  return true;
}

// ---- what the device does ----------------------------------------------------------------------------------------------------------------------------
// What this device implements, where the host's device has it.
uint32_t VenusDevice::parity_gaps() const {
  const VenusCaps& c = ctx_.caps();
  uint32_t gaps = 0;
  for (uint32_t n = 2; n <= 8; n *= 2)   // (info()'s sample counts are bit n for n samples; VkSampleCountFlagBits is the value n)
    if ((sample_counts_ >> n & 1) && !(c.sample_location_counts & n)) gaps |= kGapSampleLocations;
  if (!c.null_descriptor) gaps |= kGapNullDescriptor;
  if (!c.provoking_vertex_last) gaps |= kGapProvokingLast;
  if (!c.depth_bias_float) gaps |= kGapDepthBiasFloat;
  if (!c.custom_border_color) gaps |= kGapCustomBorder;
  if (!c.vertex_attribute_divisor || !c.vertex_attribute_zero_divisor) gaps |= kGapDivisor;
  if (!c.depth_clamp) gaps |= kGapDepthClamp;
  return gaps;
}

DeviceInfo VenusDevice::info() const {
  DeviceInfo i{};
  i.name = "Venus (virtio-gpu)", i.kind = DeviceKind::VirtualGpu, i.accelerated = true;
  const VenusCaps& c = ctx_.caps();
  i.max_push_constant_bytes = kMaxPushConstantBytes;
  i.blend_constant = true;
  i.features = kFeatureBlendState | kFeatureTexelAccess | kFeatureSystemInputs | kFeatureInstancing | kFeatureLines | kFeatureMultipleTargets | kFeatureFloatFormats |
               kFeatureTextureKinds | kFeatureIndependentBlend | kFeatureCompute | kFeatureSrgbFormats | kFeatureUnorm16Formats | kFeatureIntegerFormats |
               kFeatureSnormFormats | kFeatureStorageInDraws | kFeatureVertexFormats | kFeatureAddressModes;
  if (c.fragment_stores_and_atomics) i.features |= kFeatureFragmentStores;
  if (c.vertex_pipeline_stores_and_atomics) i.features |= kFeatureVertexStores;
  if (c.geometry_shader) i.features |= kFeatureGeometryShaders;
  if (c.geometry_shader && c.vertex_pipeline_stores_and_atomics) i.features |= kFeatureGeometryStores;
  if (c.tessellation_shader) i.features |= kFeatureTessellation;
  if (c.tessellation_shader && c.vertex_pipeline_stores_and_atomics) i.features |= kFeatureTessellationStores;
  if (c.shader_clip_distance) i.features |= kFeatureClipDistances;
  if (c.texture_compression_bc) i.features |= kFeatureCompressedFormats;
  i.features |= kFeatureOcclusionQueries;
  if (c.timestamp_valid_bits) i.features |= kFeatureTimestampQueries;
  if (c.pipeline_statistics_query) i.features |= kFeatureStatisticsQueries;
  if (c.transform_feedback) i.features |= kFeatureStreamOutput;
  i.features2 = kFeature2SingleChannelFormats | kFeature2DepthTextures | kFeature2RasterizerState | kFeature2SamplerState | kFeature2StorageImages |
                kFeature2StorageImagesInDraws | kFeature2TexelBuffers | kFeature2StorageImagesAllStages | kFeature2MipmappedLayers | kFeature2LayeredTargets |
                kFeature2DepthAndStencilPlanes;
  if (c.logic_op) i.features2 |= kFeature2LogicOps;
  if (c.dual_src_blend) i.features2 |= kFeature2DualSourceBlend;
  if (c.multi_viewport) i.features2 |= kFeature2Viewports;
  if (c.shader_output_layer && c.shader_output_viewport_index) i.features2 |= kFeature2LayerFromAnyStage;
  if (c.conservative_rasterization) i.features2 |= kFeature2ConservativeRaster;
  // (gap pass 4: the planes of depth images and whole subresources by copies, a magnification filter of its own, min / max reductions, Direct3D's line modes -
  // smooth lines for the alpha-antialiased ones, rectangular lines of 1.4 and 1.0 pixels for the quadrilateral ones -, the depth bounds test, copies of samples)
  i.features2 |= kFeature2DepthCopies | kFeature2SeparateFilters | kFeature2SampleCopies;
  if (c.sampler_minmax) i.features2 |= kFeature2SamplerReductions;
  if (c.smooth_lines && c.rectangular_lines && c.max_line_width >= 1.4f) i.features2 |= kFeature2LineModes;
  if (c.depth_bounds) i.features2 |= kFeature2DepthBounds;
  // (rasterizer ordered views: the fragment shader in a pixel interlock - its body between OpBeginInvocationInterlockEXT and OpEndInvocationInterlockEXT, PixelInterlockOrderedEXT)
  if (c.pixel_interlock && c.fragment_stores_and_atomics) i.features3 |= kFeature3RasterizerOrderedViews;
  // (a slot with no buffer is VK_NULL_HANDLE, robustness2's null descriptor: its attributes read 0, 0, 0 and 0 or 1 as the format has; Vulkan takes bindings of stride 0)
  if (c.null_descriptor) i.features3 |= kFeature3NullVertexBuffers;
  i.features3 |= kFeature3ZeroStride;
  i.sample_counts = sample_counts_ & ((1u << 2) | (1u << 4) | (1u << 8));
  if (i.sample_counts) i.features |= kFeatureMultisample | kFeatureDepthResolve, i.features2 |= kFeature2ColorResolveModes;
  // (gap pass 5, each where the host has what it takes: programmable sample positions - dynamic sample locations of one sample and of every offered count, a quad's
  // patterns in a 2 x 2 grid, pipelines of other locations in the same rendering -; view instancing - the draw loop of the CPU renderer, with SV_ViewID and the offsets
  // of the viewport and the layer in the constants' system row, which the last stage before the rasterizer adds to what it writes -; coarse shading - the pipeline's,
  // the primitive's and an attachment's rate with every combiner, up to 4x4 coarse pixels, Direct3D's AdditionalShadingRatesSupported -; barycentrics)
  if (c.sample_locations && c.variable_sample_locations && (c.sample_location_quad_counts & (1u << 1)) && (i.sample_counts & ~c.sample_location_quad_counts) == 0)
    i.features2 |= kFeature2SamplePositions;
  if (c.multi_viewport && c.shader_output_layer && c.shader_output_viewport_index) i.features2 |= kFeature2ViewInstancing;
  if (shading_rate_usable_) {
    // (coarse pixels up to the host's largest: below 4x4 - RADV's stop at 2x2 - the device says so, kFeature3CoarsePixels2x2Only, and the host clamps the larger rates to
    // 2x2 as Direct3D does without AdditionalShadingRatesSupported)
    i.features2 |= kFeature2ShadingRate;
    if (c.max_fragment_width < 4 || c.max_fragment_height < 4) i.features3 |= kFeature3CoarsePixels2x2Only;
  }
  if (c.fragment_barycentric) i.features2 |= kFeature2Barycentrics;
  i.features2 |= kFeature2UnnormalizedCoordinates;   // (normalised samplers of one level, the shaders dividing by its size: VenusDevice::sampler_for)
  // (residency feedback - Direct3D's tiled resources tier 2, and with volumes and arrays of levels tiers 3 and 4 -: sparse 2D and 3D images of the standard block
  // shapes whose unbound texels read zero, bound by a queue of the device, the shaders' sparse lookups with a minimum level of detail: VenusImage::set_residency)
  if (c.sparse_queue && c.sparse_residency_2d && c.sparse_residency_3d && c.shader_resource_residency && c.shader_resource_min_lod && c.residency_standard_2d &&
      c.residency_standard_3d && c.residency_strict)
    i.features2 |= kFeature2TileResidency;
  i.d3d_sample_order = true;   // (Vulkan's standard sample locations are Direct3D's)
  // (multisampled images read by shaders - OpImageFetch with a sample, depth ones too, arrays of them -, Flat / NoPerspective / Centroid / Sample inputs, SampleId, the
  // InterpolateAt* of GLSL.std.450 and SampleMask in and out: every Vulkan device with sampleRateShading, as RADV has it; inner coverage would need
  // VK_EXT_conservative_rasterization's fullyCoveredFragmentShaderInputVariable, which the device does not ask for)
  i.features3 |= kFeature3InterpolationModes;
  if (i.sample_counts && c.sample_rate_shading)
    i.features3 |= kFeature3MultisampleLoads | kFeature3MultisampleArrays | kFeature3MultisampleDepthLoads | kFeature3SampleShading | kFeature3EvaluateAttributes | kFeature3Coverage;
  i.limits.texture_units = kMaxTextureUnits;
  i.limits.vertex_attributes = kMaxVertexAttributes;
  i.limits.varyings = 15;            // (as virgl's: the Direct3D layer's text keeps one more output, the position as written, after them)
  i.limits.color_targets = kMaxColorTargets;
  i.limits.vertex_buffers = kMaxVertexBuffers;
  i.limits.control_varyings = c.tessellation_shader ? 30 : 0;
  return i;
}

// ---- memory ------------------------------------------------------------------------------------------------------------------------------------------
bool VenusDevice::make_memory(uint64_t size, VkBufferUsageFlags usage, VenusMemory* out) {
  *out = VenusMemory{};
  const uint64_t bytes = (size + 4095) & ~uint64_t{4095};
  VkBufferCreateInfo bi = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  bi.size = bytes;
  bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vk().CreateBuffer(device(), &bi, nullptr, &out->buffer) != VK_SUCCESS) {
    out->buffer = nullptr;
    return false;
  }
  VkMemoryRequirements req{};
  vk().GetBufferMemoryRequirements(device(), out->buffer, &req);
  const int32_t type = ctx_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  VkMemoryAllocateInfo mi = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  mi.allocationSize = req.size;
  mi.memoryTypeIndex = static_cast<uint32_t>(type);
  void* map = nullptr;
  if (type < 0 || vk().AllocateMemory(device(), &mi, nullptr, &out->memory) != VK_SUCCESS) {
    out->memory = nullptr;
    free_memory(out);
    return false;
  }
  if (vk().BindBufferMemory(device(), out->buffer, out->memory, 0) != VK_SUCCESS || vk().MapMemory(device(), out->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS || !map) {
    free_memory(out);
    return false;
  }
  out->map = static_cast<uint8_t*>(map);
  out->size = bytes;
  ByteView(out->map, bytes).fill(0);
  return true;
}

void VenusDevice::free_memory(VenusMemory* m) {
  if (!ctx_.opened()) return;
  if (m->buffer) vk().DestroyBuffer(device(), m->buffer, nullptr);
  if (m->memory) vk().FreeMemory(device(), m->memory, nullptr);   // (the mapping goes with it)
  *m = VenusMemory{};
}

VenusMemory* VenusDevice::staging(uint64_t size) {
  if (staging_.buffer && staging_.size >= size) return &staging_;
  uint64_t want = staging_.size ? staging_.size : kStagingStart;
  while (want < size) {
    if (want > (uint64_t{1} << 40)) return nullptr;
    want *= 2;
  }
  VenusMemory fresh;
  if (!make_memory(want, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, &fresh)) return nullptr;
  free_memory(&staging_);
  staging_ = fresh;
  return &staging_;
}

void VenusDevice::add_video_memory(int64_t bytes) {
  if (account_) account_(account_context_, bytes);
}

bool VenusDevice::lock_queue() {
  queue_lock_.lock();
  return true;
}
void VenusDevice::unlock_queue() { queue_lock_.unlock(); }

bool VenusDevice::bind_sparse(const VkBindSparseInfo& info) {
  if (!ctx_.caps().sparse_queue) return false;
  lock_queue();
  const bool ok = vk().QueueBindSparse(ctx_.queue(), 1, &info, fence_) == VK_SUCCESS && wait_fence(fence_);
  unlock_queue();
  return ok;
}

bool VenusDevice::wait_fence(VkFence fence) {
  // (the backend polls the host; 20 s, as virgl's 3D commands wait for a cold shader compile)
  const VkResult r = vk().WaitForFences(device(), 1, &fence, VK_TRUE, 20ull * 1000 * 1000 * 1000);
  vk().ResetFences(device(), 1, &fence);
  return r == VK_SUCCESS;
}

bool VenusDevice::begin_once() {
  lock_queue();
  const VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  if (vk().BeginCommandBuffer(once_, &bi) != VK_SUCCESS) {
    unlock_queue();
    return false;
  }
  return true;
}

bool VenusDevice::end_once() {
  // (everything the copies wrote reaches the host's view before the fence: the CPU reads it after)
  VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
  vk().CmdPipelineBarrier2(once_, &di);
  bool ok = vk().EndCommandBuffer(once_) == VK_SUCCESS;
  if (ok) {
    VkSubmitInfo si = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1, si.pCommandBuffers = &once_;
    ok = vk().QueueSubmit(ctx_.queue(), 1, &si, fence_) == VK_SUCCESS && wait_fence(fence_);
  }
  vk().ResetCommandBuffer(once_, 0);
  unlock_queue();
  return ok;
}

VkDescriptorPool VenusDevice::descriptor_pool() {
  // A pool of many sets of every kind; pipelines take their sets from the newest and a new pool is made when it is full (VenusPipeline.cpp).
  static constexpr uint32_t kSets = 1024;
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets},         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kSets * 4},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets * 8},             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets * 4},           {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, kSets * 4},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * 4},              {VK_DESCRIPTOR_TYPE_SAMPLER, kSets * 4},
  };
  VkDescriptorPoolCreateInfo ci = vk_struct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  ci.maxSets = kSets;
  ci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
  ci.pPoolSizes = sizes;
  VkDescriptorPool p = nullptr;
  if (vk().CreateDescriptorPool(device(), &ci, nullptr, &p) != VK_SUCCESS) return nullptr;
  if (!pools_.push_back(p)) {
    vk().DestroyDescriptorPool(device(), p, nullptr);
    return nullptr;
  }
  return p;
}

VkQueryPool VenusDevice::query_pool(QueryPoolKind kind) {
  if (pools_by_kind_[kind]) return pools_by_kind_[kind];
  VkQueryPoolCreateInfo ci = vk_struct<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
  ci.queryCount = kind == kPoolTimestamps ? kTimestamps : kQuerySegments;
  switch (kind) {
    case kPoolOcclusion: ci.queryType = VK_QUERY_TYPE_OCCLUSION; break;
    case kPoolStatistics:
      if (!ctx_.caps().pipeline_statistics_query) return nullptr;
      ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      ci.pipelineStatistics = 0x7FF;   // (all eleven, in Direct3D's order: VkQueryPipelineStatisticFlagBits come in the same)
      break;
    case kPoolStreams:
      if (!ctx_.caps().transform_feedback) return nullptr;
      ci.queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT;
      break;
    default: ci.queryType = VK_QUERY_TYPE_TIMESTAMP; break;
  }
  if (vk().CreateQueryPool(device(), &ci, nullptr, &pools_by_kind_[kind]) != VK_SUCCESS) pools_by_kind_[kind] = nullptr;
  return pools_by_kind_[kind];
}

VenusMemory* VenusDevice::stream_output_counters() {
  if (!so_counters_.buffer && !make_memory(kStreamOutputCounters * 4, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT, &so_counters_)) return nullptr;
  return &so_counters_;
}

// ---- resolves --------------------------------------------------------------------------------------------------------------------------------------
VenusImage* VenusDevice::resolve_scratch(Format format, Extent2D extent) {
  if (resolve_scratch_ && resolve_scratch_->format() == format && resolve_scratch_->extent().width == extent.width && resolve_scratch_->extent().height == extent.height)
    return resolve_scratch_;
  destroy_image(resolve_scratch_);
  resolve_scratch_ = static_cast<VenusImage*>(create_image({extent, format, kImageColorAttachment}));
  return resolve_scratch_;
}

void VenusDevice::free_rate_attachment() {
  if (accounted_rate_) add_video_memory(-static_cast<int64_t>(accounted_rate_));
  accounted_rate_ = 0;
  if (rate_attachment_.view) vk().DestroyImageView(device(), rate_attachment_.view, nullptr);
  if (rate_attachment_.image) vk().DestroyImage(device(), rate_attachment_.image, nullptr);
  if (rate_attachment_.memory) vk().FreeMemory(device(), rate_attachment_.memory, nullptr);
  rate_attachment_ = RateAttachment();
}

const VenusDevice::RateAttachment* VenusDevice::rate_attachment(VkCommandBuffer cb) {
  if (!ctx_.caps().shading_rate || !ctx_.caps().shading_rate_texel) return nullptr;
  if (rate_attachment_.view) return &rate_attachment_;
  // (one image for every target there may be: it never has to grow while a command buffer refers to it)
  const uint32_t texel = ctx_.caps().shading_rate_texel;
  const uint32_t side = (max_image_dimension() + texel - 1) / texel;
  VkImageCreateInfo ci = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8_UINT, ci.extent = {side, side, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vk().CreateImage(device(), &ci, nullptr, &rate_attachment_.image) != VK_SUCCESS) {
    rate_attachment_.image = nullptr;
    return nullptr;
  }
  VkMemoryRequirements req{};
  vk().GetImageMemoryRequirements(device(), rate_attachment_.image, &req);
  int32_t type = ctx_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type < 0) type = ctx_.memory_type(req.memoryTypeBits, 0);
  VkMemoryAllocateInfo mi = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  mi.allocationSize = req.size, mi.memoryTypeIndex = static_cast<uint32_t>(type);
  if (type < 0 || vk().AllocateMemory(device(), &mi, nullptr, &rate_attachment_.memory) != VK_SUCCESS) {
    rate_attachment_.memory = nullptr;
    free_rate_attachment();
    return nullptr;
  }
  VkImageViewCreateInfo vi = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  vi.image = rate_attachment_.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = VK_FORMAT_R8_UINT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vk().BindImageMemory(device(), rate_attachment_.image, rate_attachment_.memory, 0) != VK_SUCCESS ||
      vk().CreateImageView(device(), &vi, nullptr, &rate_attachment_.view) != VK_SUCCESS) {
    rate_attachment_.view = nullptr;
    free_rate_attachment();
    return nullptr;
  }
  rate_attachment_.width = rate_attachment_.height = side;
  accounted_rate_ = req.size;
  add_video_memory(static_cast<int64_t>(accounted_rate_));
  // (in GENERAL from here on, as every image of the device)
  VkImageMemoryBarrier2 b = vk_struct<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
  b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = rate_attachment_.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
  vk().CmdPipelineBarrier2(cb, &di);
  return &rate_attachment_;
}

VenusPipeline* VenusDevice::extreme_resolve_pipeline(VkFormat target, uint32_t samples, bool greatest) {
  for (const ResolvePipeline& r : resolve_pipelines_)
    if (r.target == target && r.samples == samples && r.greatest == greatest) return r.pipeline;
  // (a triangle over the target from the vertex number: (-1, -1), (3, -1), (-1, 3); the fragment shader takes every sample at its pixel and keeps the least or greatest)
  static const char kVs[] =
      "VERT\nDCL SV[0], VERTEXID\nDCL OUT[0], POSITION\nDCL TEMP[0]\nIMM[0] UINT32 {1, 1, 0, 0}\nIMM[1] FLT32 {4.0, -1.0, 0.0, 1.0}\n"
      "  0: AND TEMP[0].x, SV[0].xxxx, IMM[0].xxxx\n  1: USHR TEMP[0].y, SV[0].xxxx, IMM[0].yyyy\n  2: AND TEMP[0].y, TEMP[0].yyyy, IMM[0].xxxx\n"
      "  3: U2F TEMP[0].xy, TEMP[0].xyyy\n  4: MAD OUT[0].xy, TEMP[0].xyyy, IMM[1].xxxx, IMM[1].yyyy\n  5: MOV OUT[0].zw, IMM[1].zzzw\n  6: END\n";
  char fs[2048];
  uint32_t n = 0;
  auto put = [&](const char* t) {
    for (; *t && n + 1 < sizeof(fs); ++t) fs[n++] = *t;
  };
  put("FRAG\nDCL IN[0], POSITION, LINEAR\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D_MSAA, FLOAT\nDCL TEMP[0..2]\n");
  put("IMM[0] UINT32 {0, 1, 2, 3}\nIMM[1] UINT32 {4, 5, 6, 7}\n");
  put("  F2I TEMP[0].xy, IN[0].xyyy\n  MOV TEMP[0].zw, IMM[0].xxxx\n  TXF TEMP[1], TEMP[0], SAMP[0], 2D_MSAA\n");
  static const char* const kSample[8] = {"IMM[0].xxxx", "IMM[0].yyyy", "IMM[0].zzzz", "IMM[0].wwww", "IMM[1].xxxx", "IMM[1].yyyy", "IMM[1].zzzz", "IMM[1].wwww"};
  for (uint32_t s = 1; s < samples && s < 8; ++s) {
    put("  MOV TEMP[0].w, "), put(kSample[s]), put("\n  TXF TEMP[2], TEMP[0], SAMP[0], 2D_MSAA\n");
    put(greatest ? "  MAX TEMP[1], TEMP[1], TEMP[2]\n" : "  MIN TEMP[1], TEMP[1], TEMP[2]\n");
  }
  put("  MOV OUT[0], TEMP[1]\n  END\n");
  fs[n] = 0;
  ShaderModuleDesc vd;
  vd.stage = ShaderStage::Vertex, vd.tgsi = kVs;
  ShaderModuleDesc fd;
  fd.stage = ShaderStage::Fragment, fd.tgsi = fs;
  ResolvePipeline r;
  r.target = target, r.samples = samples, r.greatest = greatest;
  r.vs = static_cast<VenusShaderModule*>(create_shader_module(vd));
  r.fs = static_cast<VenusShaderModule*>(create_shader_module(fd));
  if (r.vs && r.fs) {
    GraphicsPipelineDesc pd;
    pd.vertex_shader = r.vs, pd.fragment_shader = r.fs;
    pd.texture_count = 1, pd.cull_mode = CullMode::None, pd.color_target_count = 1;
    r.pipeline = static_cast<VenusPipeline*>(create_graphics_pipeline(pd));
  }
  if (!r.pipeline || !resolve_pipelines_.push_back(r)) {
    destroy_pipeline(r.pipeline);
    destroy_shader_module(r.fs);
    destroy_shader_module(r.vs);
    return nullptr;
  }
  return r.pipeline;
}

// ---- formats ---------------------------------------------------------------------------------------------------------------------------------------
VkFormat VenusDevice::vk_format(Format f, bool storage, bool* swap_rb) const {
  *swap_rb = false;
  switch (f) {
    case Format::B8G8R8A8Unorm:
      if (storage) {   // (a storage image of 8-bit colour is RGBA: BGRA is not a storage format Vulkan promises; the CPU turns red and blue around)
        *swap_rb = true;
        return VK_FORMAT_R8G8B8A8_UNORM;
      }
      return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R8G8B8A8Unorm: *swap_rb = true; return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R8G8B8A8Srgb: *swap_rb = true; return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::R16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R16G16B16A16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::R32G32B32A32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::R32G32B32A32Sint: return VK_FORMAT_R32G32B32A32_SINT;
    case Format::R16G16B16A16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case Format::R16G16B16A16Sint: return VK_FORMAT_R16G16B16A16_SINT;
    case Format::R8G8B8A8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case Format::R16G16B16A16Snorm: return VK_FORMAT_R16G16B16A16_SNORM;
    case Format::R10G10B10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::Bc1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::Bc2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::Bc3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::Bc1Srgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::Bc2Srgb: return VK_FORMAT_BC2_SRGB_BLOCK;
    case Format::Bc3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::R32Uint: return VK_FORMAT_R32_UINT;
    case Format::R32Sint: return VK_FORMAT_R32_SINT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::A8Unorm: return VK_FORMAT_R8_UNORM;   // (sampled through a view that reads (0, 0, 0, r))
    case Format::B5G6R5Unorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::B5G5R5A1Unorm: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case Format::B4G4R4A4Unorm: return VK_FORMAT_A4R4G4B4_UNORM_PACK16;
    case Format::R8Snorm: return VK_FORMAT_R8_SNORM;
    case Format::R8Uint: return VK_FORMAT_R8_UINT;
    case Format::R8Sint: return VK_FORMAT_R8_SINT;
    case Format::R8G8Snorm: return VK_FORMAT_R8G8_SNORM;
    case Format::R8G8Uint: return VK_FORMAT_R8G8_UINT;
    case Format::R8G8Sint: return VK_FORMAT_R8G8_SINT;
    case Format::R8G8B8A8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::R8G8B8A8Sint: return VK_FORMAT_R8G8B8A8_SINT;
    case Format::R16Unorm: return VK_FORMAT_R16_UNORM;
    case Format::R16Snorm: return VK_FORMAT_R16_SNORM;
    case Format::R16Uint: return VK_FORMAT_R16_UINT;
    case Format::R16Sint: return VK_FORMAT_R16_SINT;
    case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
    case Format::R16G16Unorm: return VK_FORMAT_R16G16_UNORM;
    case Format::R16G16Snorm: return VK_FORMAT_R16G16_SNORM;
    case Format::R16G16Uint: return VK_FORMAT_R16G16_UINT;
    case Format::R16G16Sint: return VK_FORMAT_R16G16_SINT;
    case Format::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case Format::R32G32Uint: return VK_FORMAT_R32G32_UINT;
    case Format::R32G32Sint: return VK_FORMAT_R32G32_SINT;
    case Format::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    // (96-bit images are often not offered as optimal images: they are kept as 128-bit ones then, the fourth component 1; widened_96())
    case Format::R32G32B32Uint: return widened_96(f) ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32_UINT;
    case Format::R32G32B32Sint: return widened_96(f) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32_SINT;
    case Format::R32G32B32Float: return widened_96(f) ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT;
    case Format::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::R9G9B9E5Float: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case Format::R10G10B10A2Uint: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case Format::Bc4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::Bc4Snorm: return VK_FORMAT_BC4_SNORM_BLOCK;
    case Format::Bc5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::Bc5Snorm: return VK_FORMAT_BC5_SNORM_BLOCK;
    case Format::Bc6hUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case Format::Bc6hSfloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case Format::Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::Bc7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}

bool VenusDevice::widened_96(Format f) const {
  const VkFormat own = f == Format::R32G32B32Uint ? VK_FORMAT_R32G32B32_UINT : f == Format::R32G32B32Sint ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32_SFLOAT;
  return !format_supports(own, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
}

bool VenusDevice::format_supports(VkFormat f, VkFormatFeatureFlags features) const {
  if (f == VK_FORMAT_UNDEFINED) return false;
  VkFormatProperties p{};
  vk().GetPhysicalDeviceFormatProperties(ctx_.physical(), f, &p);
  return (p.optimalTilingFeatures & features) == features;
}

uint32_t VenusDevice::format_caps(Format format) const {
  // (as VirglDevice: only the formats numbered from 64 answer; the others are described by the features)
  if (static_cast<uint32_t>(format) < 64) return 0;
  bool swap = false;
  const VkFormat f = vk_format(format, false, &swap);
  if (!format_supports(f, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
  if (is_compressed_format(format)) return kFormatSampled | kFormatFiltered | kFormatMipmaps | kFormatNative;
  uint32_t caps = kFormatSampled | kFormatMipmaps;
  if (!is_integer_format(format) && format_supports(f, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) caps |= kFormatFiltered;
  const bool rgb96 = format == Format::R32G32B32Uint || format == Format::R32G32B32Sint || format == Format::R32G32B32Float;
  if (format != Format::A8Unorm && format != Format::R9G9B9E5Float && !(rgb96 && widened_96(format)) && format_supports(f, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
    caps |= kFormatRenderTarget;
    if (!is_integer_format(format) && format_supports(f, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) caps |= kFormatBlendable;
  }
  const bool storable = format != Format::A8Unorm && format != Format::B5G6R5Unorm && format != Format::B5G5R5A1Unorm && format != Format::B4G4R4A4Unorm &&
                        format != Format::R9G9B9E5Float && format != Format::R32G32B32Uint && format != Format::R32G32B32Sint && format != Format::R32G32B32Float;
  if (storable && format_supports(f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) caps |= kFormatStorage;
  return caps;
}

uint32_t VenusDevice::max_image_dimension() const { return max_image_dimension_ < 4096 ? 4096 : max_image_dimension_; }

// ---- samplers ----------------------------------------------------------------------------------------------------------------------------------------
namespace {
VkSamplerAddressMode address_mode(AddressMode m) {
  switch (m) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case AddressMode::MirrorOnce: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
  }
  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
bool same_sampler(const SamplerDesc& a, const SamplerDesc& b) {
  return a.filter == b.filter && a.address_u == b.address_u && a.address_v == b.address_v && a.border == b.border && a.mip_filter == b.mip_filter &&
         __builtin_memcmp(&a.lod_bias, &b.lod_bias, 4) == 0 && __builtin_memcmp(&a.min_lod, &b.min_lod, 4) == 0 && __builtin_memcmp(&a.max_lod, &b.max_lod, 4) == 0 &&
         a.max_anisotropy == b.max_anisotropy && a.custom_border == b.custom_border && __builtin_memcmp(a.border_rgba, b.border_rgba, 16) == 0 &&
         magnification_filter(a) == magnification_filter(b) && a.reduction == b.reduction && a.unnormalized == b.unnormalized;
}
float clamp_lod(float v, float lo, float hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }
}  // namespace

// The sampler of a SamplerDesc (VirglDevice's rules: integer textures point-sampled, r always clamped to the edge, no comparison - the shaders compare -, a border
// only when an axis clamps to it, anisotropy only with linear filtering). Made once each.
VkSampler VenusDevice::sampler_for(const SamplerDesc& d, bool integer) {
  for (const CachedSampler& c : samplers_)
    if (c.integer == integer && same_sampler(c.desc, d)) return c.sampler;
  // (reductions need the host's samplerFilterMinmax)
  if (d.reduction != SamplerReduction::Weighted && !ctx_.caps().sampler_minmax) return nullptr;
  VkSamplerCreateInfo si = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  const bool linear = d.filter == Filter::Linear && !integer;
  si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.magFilter = magnification_filter(d) == Filter::Linear && !integer ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;   // (kFeature2SeparateFilters)
  si.mipmapMode = d.mip_filter == MipFilter::Linear && !integer ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = address_mode(d.address_u);
  si.addressModeV = address_mode(d.address_v);
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (d.mip_filter == MipFilter::None || d.unnormalized) {
    // (coordinates in texels, kFeature2UnnormalizedCoordinates: a normalised sampler of the view's first level alone - the shader divides the coordinates by that
    // level's size, Venus' TgsiSpirv unnormalized_units -, not Vulkan's unnormalized samplers, which take no offsets, borders of every kind or array views)
    si.minLod = si.maxLod = 0, si.mipLodBias = 0;
    if (d.unnormalized) si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  } else {
    si.mipLodBias = clamp_lod(d.lod_bias, -16, 16);
    si.minLod = clamp_lod(d.min_lod, 0, 16);
    si.maxLod = clamp_lod(d.max_lod, 0, 16);
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
  }
  if (linear && d.max_anisotropy > 1 && ctx_.caps().sampler_anisotropy && !d.unnormalized) {
    si.anisotropyEnable = VK_TRUE;
    const float most = ctx_.caps().max_anisotropy < 16 ? ctx_.caps().max_anisotropy : 16;
    si.maxAnisotropy = d.max_anisotropy > most ? most : d.max_anisotropy;
  }
  VkSamplerReductionModeCreateInfo reduction = vk_struct<VkSamplerReductionModeCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO);
  if (d.reduction != SamplerReduction::Weighted) {   // (kFeature2SamplerReductions: the least or greatest of the texels the filter reads)
    reduction.reductionMode = d.reduction == SamplerReduction::Minimum ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
    si.pNext = &reduction;
  }
  VkSamplerCustomBorderColorCreateInfoEXT custom = vk_struct<VkSamplerCustomBorderColorCreateInfoEXT>(VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);
  const bool border = d.address_u == AddressMode::ClampToBorder || d.address_v == AddressMode::ClampToBorder;
  if (border) {
    if (d.custom_border && ctx_.caps().custom_border_color) {
      si.borderColor = integer ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
      __builtin_memcpy(custom.customBorderColor.uint32, d.border_rgba, 16);   // (the 32-bit patterns as given: integer textures see the integer bits)
      custom.format = VK_FORMAT_UNDEFINED;
      custom.pNext = si.pNext;
      si.pNext = &custom;
    } else {
      const BorderColor b = d.border;
      si.borderColor = b == BorderColor::OpaqueWhite ? (integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
                       : b == BorderColor::OpaqueBlack ? (integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                                       : (integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    }
  }
  VkSampler s = nullptr;
  if (vk().CreateSampler(device(), &si, nullptr, &s) != VK_SUCCESS) return nullptr;
  CachedSampler c;
  c.desc = d, c.integer = integer, c.sampler = s;
  if (!samplers_.push_back(c)) {
    vk().DestroySampler(device(), s, nullptr);
    return nullptr;
  }
  return s;
}

// ---- buffers -----------------------------------------------------------------------------------------------------------------------------------------
IBuffer* VenusDevice::create_buffer(uint64_t size, uint32_t usage) {
  if (size == 0 || size > 0x7FFFFFFFu) return nullptr;
  VkBufferUsageFlags u = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (ctx_.caps().transform_feedback) u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
  if (ctx_.caps().conditional_rendering) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
  void* memory = allocator_.allocate_aligned(sizeof(VenusBuffer), alignof(VenusBuffer));
  if (!memory) return nullptr;
  VenusBuffer* b = new (memory) VenusBuffer(*this, size);
  // (a stream output buffer has a word more than it shows: a binding to its very end is bound one byte longer, Replay::begin_stream_output)
  const uint64_t room = (usage & kBufferStreamOutput) ? size + 4 : size;
  if (!make_memory(room, u, &b->memory_)) {
    b->~VenusBuffer();
    allocator_.deallocate(memory);
    return nullptr;
  }
  add_video_memory(static_cast<int64_t>(size));
  return b;
}

void VenusDevice::destroy_buffer(IBuffer* buffer) {
  if (!buffer) return;
  VenusBuffer* b = static_cast<VenusBuffer*>(buffer);
  add_video_memory(-static_cast<int64_t>(b->size()));
  free_memory(&b->memory_);
  b->~VenusBuffer();
  allocator_.deallocate(b);
}

ByteView VenusBuffer::map() { return ByteView(memory_.map, size_); }

// ---- images ------------------------------------------------------------------------------------------------------------------------------------------
IImage* VenusDevice::create_image(const ImageDesc& desc) {
  void* memory = allocator_.allocate_aligned(sizeof(VenusImage), alignof(VenusImage));
  if (!memory) return nullptr;
  VenusImage* image = new (memory) VenusImage(*this, desc);
  if (!image->create()) {
    image->~VenusImage();
    allocator_.deallocate(memory);
    return nullptr;
  }
  return image;
}

void VenusDevice::destroy_image(IImage* image) {
  if (!image) return;
  VenusImage* v = static_cast<VenusImage*>(image);
  v->~VenusImage();
  allocator_.deallocate(v);
}

VenusImage::VenusImage(VenusDevice& device, const ImageDesc& desc) : device_(device), desc_(desc), views_(device.allocator()), targets_(device.allocator()) {}

VenusImage::~VenusImage() {
  const TcosKernelVulkan& vk = device_.vk();
  for (const View& v : views_) vk.DestroyImageView(device_.device(), v.view, nullptr);
  for (const Target& t : targets_) vk.DestroyImageView(device_.device(), t.view, nullptr);
  if (image_) vk.DestroyImage(device_.device(), image_, nullptr);
  if (memory_) vk.FreeMemory(device_.device(), memory_, nullptr);
  if (accounted_) device_.add_video_memory(-static_cast<int64_t>(accounted_));
}

// The same refusals as VirglDevice::create_image (the kernel probes them), then the image.
bool VenusImage::create() {
  const ImageDesc& d = desc_;
  const bool compressed = is_compressed_format(d.format);
  const bool depth = d.format == Format::D32Float;
  const bool storage = (d.usage & kImageStorage) != 0;
  if (d.extent.width == 0 || d.extent.height == 0 || d.format == Format::Undefined) return false;
  if (d.extent.width > device_.max_image_dimension() || d.extent.height > device_.max_image_dimension()) return false;
  if (d.mip_levels == 0 || d.mip_levels > mip_level_limit(d.extent)) return false;
  if (is_multisampled_kind(d.kind)) return false;
  if (d.kind == TextureKind::TextureCubeArray && (d.layers == 0 || d.layers % 6 != 0)) return false;
  if (d.kind != TextureKind::Texture2D && (d.layers == 0 || d.layers > kMaxTextureLayers)) return false;
  if (d.kind == TextureKind::Texture2D && d.layers != 1) return false;
  if ((d.kind == TextureKind::TextureCube || d.kind == TextureKind::TextureCubeArray) && d.extent.width != d.extent.height) return false;
  if (d.stencil && !depth) return false;
  if ((d.usage & kImageSampled) && (d.usage & kImagePresent)) return false;   // (as VirglDevice: callers render into a present image and copy)
  if (static_cast<uint32_t>(d.format) >= 64) {
    const uint32_t caps = device_.format_caps(d.format);
    if (!(caps & kFormatSampled) || ((d.usage & kImageColorAttachment) && !(caps & kFormatRenderTarget)) || (storage && !(caps & kFormatStorage)) ||
        (d.mip_levels > 1 && !(caps & kFormatMipmaps)) || d.samples != 1)
      return false;
  }
  if (compressed && (d.usage != kImageSampled || d.samples != 1)) return false;
  if (storage && (!(d.usage & kImageSampled) || d.samples != 1 || depth || is_srgb_format(d.format))) return false;
  if (d.samples != 1) {
    // (an image of several samples: 2D, or an array of them - kFeature3MultisampleArrays -, a colour or depth attachment, which shaders may fetch samples of - kFeature3MultisampleLoads /
    // MultisampleDepthLoads)
    if (!(device_.info().sample_counts & (1u << d.samples)) || (d.kind != TextureKind::Texture2D && d.kind != TextureKind::Texture2DArray) || d.mip_levels != 1 || (d.usage & (kImagePresent | kImageStorage))) return false;
    if (d.kind == TextureKind::Texture2DArray && !(device_.info().features3 & kFeature3MultisampleArrays)) return false;
    if (depth ? (d.usage & ~(kImageDepthAttachment | kImageSampled)) != 0
              : (!(d.usage & kImageColorAttachment) || (d.usage & ~(kImageColorAttachment | kImageSampled)) || is_integer_format(d.format) || is_compressed_format(d.format)))
      return false;
  }
  if ((d.usage & kImageSampled) && depth && (d.usage & ~(kImageSampled | kImageDepthAttachment))) return false;
  if (d.format == Format::A8Unorm && (d.usage & kImageColorAttachment)) return false;

  format_ = device_.vk_format(d.format, storage, &swap_rb_);
  widened_ = (d.format == Format::R32G32B32Uint || d.format == Format::R32G32B32Sint || d.format == Format::R32G32B32Float) && device_.widened_96(d.format);
  if (depth && d.stencil) format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
  if (format_ == VK_FORMAT_UNDEFINED) return false;
  aspect_ = depth ? (d.stencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
  layers_ = image_layer_count(d.kind, d.layers);
  return make_image(false);
}

bool VenusImage::make_image(bool sparse) {
  const ImageDesc& d = desc_;
  const bool compressed = is_compressed_format(d.format);
  const bool depth = d.format == Format::D32Float;
  const bool storage = (d.usage & kImageStorage) != 0;
  VkImageCreateInfo ci = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  ci.imageType = d.kind == TextureKind::Texture3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ci.format = format_;
  ci.extent = {d.extent.width, d.extent.height, d.kind == TextureKind::Texture3D ? d.layers : 1u};
  ci.mipLevels = d.mip_levels;
  ci.arrayLayers = d.kind == TextureKind::Texture3D ? 1 : layers_;
  ci.samples = static_cast<VkSampleCountFlagBits>(d.samples);
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (d.usage & kImageSampled) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (d.usage & kImageColorAttachment) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (d.usage & kImageDepthAttachment) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (d.samples != 1 && !depth) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;   // (Texture2DMS units fetch from it)
  // (a 2D array of square faces in sixes may be seen as a cube; a storage image may be seen in another format of its size)
  if (ci.imageType == VK_IMAGE_TYPE_2D && layers_ >= 6 && d.extent.width == d.extent.height && d.samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  if (storage || (d.usage & kImageSampled)) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  if (ci.imageType == VK_IMAGE_TYPE_3D && (d.usage & (kImageColorAttachment | kImageDepthAttachment)) && !sparse) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
  if (sparse) ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;   // (kFeature2TileResidency: make_sparse)
  // (a depth buffer a pipeline of other sample locations draws into: the centre's of a pipeline without multisampling, or the positions set, kFeature2SamplePositions)
  if (depth && (d.usage & kImageDepthAttachment) && device_.context().caps().sample_locations) ci.flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const TcosKernelVulkan& vk = device_.vk();
  if (vk.CreateImage(device_.device(), &ci, nullptr, &image_) != VK_SUCCESS) {
    image_ = nullptr;
    return false;
  }
  VkMemoryRequirements req{};
  vk.GetImageMemoryRequirements(device_.device(), image_, &req);
  const int32_t type = device_.context().memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  const int32_t any = type >= 0 ? type : device_.context().memory_type(req.memoryTypeBits, 0);
  VkMemoryAllocateInfo mi = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  mi.allocationSize = req.size;
  mi.memoryTypeIndex = static_cast<uint32_t>(any);
  if (any < 0 || vk.AllocateMemory(device_.device(), &mi, nullptr, &memory_) != VK_SUCCESS) {
    memory_ = nullptr;
    return false;
  }
  // (a sparse image's memory is bound block by block: bind_residency; the blocks are at multiples of the alignment, in the order of ImageResidency's bytes)
  if (!sparse && vk.BindImageMemory(device_.device(), image_, memory_, 0) != VK_SUCCESS) return false;
  block_bytes_ = req.alignment;
  accounted_ = req.size;
  device_.add_video_memory(static_cast<int64_t>(accounted_));
  // In GENERAL from here on, cleared to zero (what a fresh image holds on every device of this system).
  return device_.run_once([&](VkCommandBuffer cb) {
    VkImageMemoryBarrier2 b = vk_struct<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {aspect_, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vk.CmdPipelineBarrier2(cb, &di);
    const VkImageSubresourceRange range = {aspect_, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (compressed || sparse) return;   // (only copies may write a compressed image; a sparse one has nothing bound yet, and is written after its binding)
    if (depth) {
      const VkClearDepthStencilValue z = {0.0f, 0};
      vk.CmdClearDepthStencilImage(cb, image_, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    } else {
      const VkClearColorValue c = {};
      vk.CmdClearColorImage(cb, image_, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    }
  });
}

// ---- residency (kFeature2TileResidency) ------------------------------------------------------------------------------------------------------------------------
bool VenusImage::set_residency(const ImageResidency& r) {
  if (!(device_.info().features2 & kFeature2TileResidency)) return false;
  const ImageDesc& d = desc_;
  const bool volume = d.kind == TextureKind::Texture3D;
  if (d.samples != 1 || d.format == Format::D32Float || aspect_ != VK_IMAGE_ASPECT_COLOR_BIT || widened_) return false;
  if (d.kind != TextureKind::Texture2D && d.kind != TextureKind::Texture2DArray && !volume) return false;
  if (r.tile_width == 0 || r.tile_height == 0 || r.tile_depth == 0 || r.standard_levels > d.mip_levels || r.slices != (volume ? 1u : layers_)) return false;
  // (the bytes: each slice's standard levels' tiles, then a byte for its packed levels when it has any)
  uint64_t per_slice = 0;
  for (uint32_t level = 0; level < r.standard_levels; ++level) {
    const uint64_t w = mip_extent(d.extent.width, level), h = mip_extent(d.extent.height, level), z = volume ? mip_extent(d.layers, level) : 1;
    per_slice += ((w + r.tile_width - 1) / r.tile_width) * ((h + r.tile_height - 1) / r.tile_height) * ((z + r.tile_depth - 1) / r.tile_depth);
  }
  const bool tail = r.standard_levels < d.mip_levels;
  if (tail) ++per_slice;
  if (per_slice == 0 || r.mapped.size() != per_slice * r.slices) return false;
  if (!sparse_ && !make_sparse(r)) return false;
  return bind_residency(r);
}

// The image again as a sparse one (its views go: they are made again when asked for). Refused - the image stays as it is - when the host's blocks are not the tiles.
bool VenusImage::make_sparse(const ImageResidency& r) {
  const TcosKernelVulkan& vk = device_.vk();
  VenusContext& ctx = device_.context();
  const ImageDesc& d = desc_;
  const bool volume = d.kind == TextureKind::Texture3D;
  if (volume && (d.usage & (kImageColorAttachment | kImageDepthAttachment))) return false;   // (a sparse volume cannot be drawn into as layers)
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (d.usage & kImageSampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (d.usage & kImageColorAttachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (d.usage & kImageStorage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  VkSparseImageFormatProperties props[4] = {};
  uint32_t count = 4;
  vk.GetPhysicalDeviceSparseImageFormatProperties(ctx.physical(), format_, volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &count,
                                                  props);
  bool shaped = false;
  for (uint32_t i = 0; i < count && i < 4; ++i)
    if ((props[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && !(props[i].flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT) &&
        props[i].imageGranularity.width == r.tile_width && props[i].imageGranularity.height == r.tile_height && props[i].imageGranularity.depth == r.tile_depth)
      shaped = true;
  if (!shaped) return false;   // (Direct3D's tile is not the host's block for this format: no residency)
  // the old image goes (the memory manager writes the contents after the binding)
  for (const View& v : views_) vk.DestroyImageView(device_.device(), v.view, nullptr);
  for (const Target& t : targets_) vk.DestroyImageView(device_.device(), t.view, nullptr);
  views_.clear(), targets_.clear();
  VkImage old_image = image_;
  VkDeviceMemory old_memory = memory_;
  const uint64_t old_accounted = accounted_;
  image_ = nullptr, memory_ = nullptr, accounted_ = 0;
  sparse_ = true;
  bool ok = make_image(true);
  if (ok) {
    VkSparseImageMemoryRequirements req[4] = {};
    uint32_t n = 4;
    vk.GetImageSparseMemoryRequirements(device_.device(), image_, &n, req);
    ok = false;
    for (uint32_t i = 0; i < n && i < 4; ++i) {
      if (!(req[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) continue;
      block_ = req[i].formatProperties.imageGranularity;
      tail_first_ = req[i].imageMipTailFirstLod;
      tail_size_ = req[i].imageMipTailSize, tail_offset_ = req[i].imageMipTailOffset, tail_stride_ = req[i].imageMipTailStride;
      single_tail_ = (req[i].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
      // (the host's mip tail must begin where Direct3D's packed levels do: then a tail is a slice's packed levels)
      const bool tail = r.standard_levels < d.mip_levels;
      ok = tail ? tail_first_ == r.standard_levels : tail_first_ >= d.mip_levels;
      if (tail && single_tail_ && r.slices > 1) ok = false;   // (one tail for every layer: Direct3D's are per slice)
    }
    // (the memory is the blocks of every slice's standard levels in ImageResidency's order, then every slice's tail)
    uint64_t blocks = 0;
    for (uint32_t level = 0; ok && level < r.standard_levels; ++level) {
      const uint64_t w = mip_extent(d.extent.width, level), h = mip_extent(d.extent.height, level), z = volume ? mip_extent(d.layers, level) : 1;
      blocks += ((w + block_.width - 1) / block_.width) * ((h + block_.height - 1) / block_.height) * ((z + block_.depth - 1) / block_.depth);
    }
    blocks *= r.slices;
    const uint64_t tails = r.standard_levels < d.mip_levels ? (tail_size_ + block_bytes_ - 1) / block_bytes_ * r.slices : 0;
    if (ok && (blocks + tails) * block_bytes_ > accounted_) {
      // (more than the image asked for: a memory of that size)
      const uint64_t bytes = (blocks + tails) * block_bytes_;
      VkMemoryRequirements mr{};
      vk.GetImageMemoryRequirements(device_.device(), image_, &mr);
      vk.FreeMemory(device_.device(), memory_, nullptr);
      device_.add_video_memory(-static_cast<int64_t>(accounted_));
      memory_ = nullptr, accounted_ = 0;
      const int32_t type = ctx.memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkMemoryAllocateInfo mi = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
      mi.allocationSize = bytes, mi.memoryTypeIndex = static_cast<uint32_t>(type);
      ok = type >= 0 && vk.AllocateMemory(device_.device(), &mi, nullptr, &memory_) == VK_SUCCESS;
      if (!ok) memory_ = nullptr;
      else accounted_ = bytes, device_.add_video_memory(static_cast<int64_t>(bytes));
    }
  }
  if (!ok) {
    // (back to the old image, whole)
    if (image_) vk.DestroyImage(device_.device(), image_, nullptr);
    if (memory_) vk.FreeMemory(device_.device(), memory_, nullptr);
    if (accounted_) device_.add_video_memory(-static_cast<int64_t>(accounted_));
    image_ = old_image, memory_ = old_memory, accounted_ = old_accounted;
    sparse_ = false;
    return false;
  }
  vk.DestroyImage(device_.device(), old_image, nullptr);
  vk.FreeMemory(device_.device(), old_memory, nullptr);
  device_.add_video_memory(-static_cast<int64_t>(old_accounted));
  return true;
}

// Every block and tail bound or unbound as `r` says, in one vkQueueBindSparse that is waited for.
bool VenusImage::bind_residency(const ImageResidency& r) {
  const TcosKernelVulkan& vk = device_.vk();
  const ImageDesc& d = desc_;
  const bool volume = d.kind == TextureKind::Texture3D;
  List<VkSparseImageMemoryBind> binds(device_.allocator());
  List<VkSparseMemoryBind> tails(device_.allocator());
  uint64_t block = 0;   // (the block's place in the memory)
  uint64_t byte = 0;    // (its byte in r.mapped)
  const bool has_tail = r.standard_levels < d.mip_levels;
  uint64_t standard_blocks = 0;
  for (uint32_t level = 0; level < r.standard_levels; ++level) {
    const uint64_t w = mip_extent(d.extent.width, level), h = mip_extent(d.extent.height, level), z = volume ? mip_extent(d.layers, level) : 1;
    standard_blocks += ((w + block_.width - 1) / block_.width) * ((h + block_.height - 1) / block_.height) * ((z + block_.depth - 1) / block_.depth);
  }
  for (uint32_t slice = 0; slice < r.slices; ++slice) {
    for (uint32_t level = 0; level < r.standard_levels; ++level) {
      const uint32_t w = mip_extent(d.extent.width, level), h = mip_extent(d.extent.height, level), z = volume ? mip_extent(d.layers, level) : 1;
      for (uint32_t bz = 0; bz < z; bz += block_.depth)
        for (uint32_t by = 0; by < h; by += block_.height)
          for (uint32_t bx = 0; bx < w; bx += block_.width) {
            VkSparseImageMemoryBind b{};
            b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, volume ? 0u : slice};
            b.offset = {static_cast<int32_t>(bx), static_cast<int32_t>(by), static_cast<int32_t>(bz)};
            // (a block at the level's edge is bound to the edge: Vulkan wants the extent a multiple of the block or reaching the end)
            b.extent = {w - bx < block_.width ? w - bx : block_.width, h - by < block_.height ? h - by : block_.height, z - bz < block_.depth ? z - bz : block_.depth};
            const bool mapped = byte < r.mapped.size() && r.mapped[byte] != 0;
            b.memory = mapped ? memory_ : nullptr;
            b.memoryOffset = mapped ? block * block_bytes_ : 0;
            if (!binds.push_back(b)) return false;
            ++block, ++byte;
          }
    }
    if (has_tail) {
      // (the slice's packed levels: its mip tail, after every slice's blocks)
      const uint64_t tail_blocks = (tail_size_ + block_bytes_ - 1) / block_bytes_;
      VkSparseMemoryBind t{};
      t.resourceOffset = tail_offset_ + (single_tail_ ? 0 : uint64_t{slice} * tail_stride_);
      t.size = tail_size_;
      const bool mapped = byte < r.mapped.size() && r.mapped[byte] != 0;
      t.memory = mapped ? memory_ : nullptr;
      t.memoryOffset = mapped ? (standard_blocks * r.slices + uint64_t{slice} * tail_blocks) * block_bytes_ : 0;
      if (!tails.push_back(t)) return false;
      ++byte;
    }
  }
  VkSparseImageMemoryBindInfo image_info{image_, static_cast<uint32_t>(binds.count()), binds.view().data()};
  VkSparseImageOpaqueMemoryBindInfo opaque_info{image_, static_cast<uint32_t>(tails.count()), tails.view().data()};
  VkBindSparseInfo bi = vk_struct<VkBindSparseInfo>(VK_STRUCTURE_TYPE_BIND_SPARSE_INFO);
  bi.imageBindCount = binds.count() ? 1 : 0, bi.pImageBinds = &image_info;
  bi.imageOpaqueBindCount = tails.count() ? 1 : 0, bi.pImageOpaqueBinds = &opaque_info;
  return device_.bind_sparse(bi);
}

uint32_t VenusImage::level_layers(uint32_t level) const { return level_layer_count(desc_.kind, desc_.layers, level); }

// Copies between the staging buffer and one level of one layer (a volume's slice); the staging buffer holds `width * height` texels of `texel` bytes (blocks for
// a compressed image) from offset 0.
bool VenusImage::copy(bool to_image, uint32_t level, uint32_t layer, uint32_t row_texels) {
  const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
  VenusMemory* staging = device_.staging(0);
  if (!staging) return false;
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = row_texels;
  region.bufferImageHeight = 0;
  const bool volume = desc_.kind == TextureKind::Texture3D;
  const VkImageAspectFlags aspect = (aspect_ & VK_IMAGE_ASPECT_DEPTH_BIT) ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : aspect_;
  region.imageSubresource = {aspect, level, volume ? 0u : layer, 1};
  region.imageOffset = {0, 0, volume ? static_cast<int32_t>(layer) : 0};
  region.imageExtent = {width, height, 1};
  const TcosKernelVulkan& vk = device_.vk();
  VkImage image = image_;
  VkBuffer buffer = staging->buffer;
  return device_.run_once([&](VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vk.CmdPipelineBarrier2(cb, &di);
    if (to_image) vk.CmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    else vk.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
  });
}

// 0xAARRGGBB words into the staging buffer as the image keeps them; its texel bytes (0: they do not fit or the format is wide).
uint32_t VenusImage::stage(ArrayView<const uint32_t> pixels) {
  if (is_wide_format(desc_.format) || is_compressed_format(desc_.format) || desc_.format == Format::D32Float) return 0;
  const uint32_t texel = is_packed32_format(desc_.format) ? bytes_per_pixel(desc_.format) : 4u;
  VenusMemory* s = device_.staging(pixels.size() * texel);
  if (!s) return 0;
  ByteWriter out{ByteView(s->map, s->size)};
  for (const uint32_t p : pixels) {
    if (texel == 4) out.le32(swap_rb_ ? (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF) : p);
    else if (desc_.format == Format::A8Unorm) out.u8(static_cast<uint8_t>(p >> 24));
    else if (texel == 1) out.u8(static_cast<uint8_t>(packed32_from_argb(desc_.format, p)));
    else out.le16(static_cast<uint16_t>(packed32_from_argb(desc_.format, p)));
  }
  return out.ok() ? texel : 0;
}

bool VenusImage::unstage(ArrayView<uint32_t> out) {
  const uint32_t texel = is_packed32_format(desc_.format) ? bytes_per_pixel(desc_.format) : 4u;
  VenusMemory* s = device_.staging(out.size() * texel);
  if (!s) return false;
  ByteReader in{ConstByteView(s->map, s->size)};
  for (uint32_t& p : out) {
    if (texel == 4) {
      const uint32_t w = in.le32();
      p = swap_rb_ ? (w & 0xFF00FF00u) | ((w & 0xFF) << 16) | ((w >> 16) & 0xFF) : w;
    } else if (desc_.format == Format::A8Unorm) {
      p = static_cast<uint32_t>(in.u8()) << 24;
    } else {
      p = packed32_to_argb(desc_.format, texel == 1 ? in.u8() : in.le16());
    }
  }
  return in.ok();
}

// The format the image's texels are in memory as (a widened 96-bit image's is the 128-bit one of its kind).
Format VenusImage::kept_format() const {
  if (!widened_) return desc_.format;
  return desc_.format == Format::R32G32B32Uint ? Format::R32G32B32A32Uint : desc_.format == Format::R32G32B32Sint ? Format::R32G32B32A32Sint : Format::R32G32B32A32Float;
}

bool VenusImage::stage_wide(ArrayView<const Vec4> pixels) {
  const Format kept = kept_format();
  VenusMemory* s = device_.staging(pixels.size() * bytes_per_pixel(kept));
  if (!s) return false;
  ByteWriter out{ByteView(s->map, s->size)};
  // (a widened image's fourth component is 1: the float one, or the integer one as bits)
  const float one = kept == Format::R32G32B32A32Float ? 1.0f : __builtin_bit_cast(float, 1u);
  for (const Vec4& c : pixels) encode_wide_pixel(kept, widened_ ? Vec4{c.x, c.y, c.z, one} : c, out);
  return out.ok();
}

bool VenusImage::unstage_wide(ArrayView<Vec4> out) {
  const Format kept = kept_format();
  VenusMemory* s = device_.staging(out.size() * bytes_per_pixel(kept));
  if (!s) return false;
  ByteReader in{ConstByteView(s->map, s->size)};
  for (Vec4& c : out) {
    c = decode_wide_pixel(kept, in);
    if (widened_) c = quantize_wide_pixel(desc_.format, c);   // (what a 96-bit image reads back: its own channels, the fourth as the format says)
  }
  return in.ok();
}

bool VenusImage::transferable(uint32_t level, uint32_t layer, uint64_t count) const {
  if (desc_.samples != 1 || level >= desc_.mip_levels || layer >= level_layers(level)) return false;
  return count == uint64_t{mip_extent(desc_.extent.width, level)} * mip_extent(desc_.extent.height, level);
}

bool VenusImage::write_subresource(uint32_t level, uint32_t layer, ArrayView<const uint32_t> pixels) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (!transferable(level, layer, pixels.size()) || is_wide_format(desc_.format)) return false;
  const uint32_t width = mip_extent(desc_.extent.width, level);
  return stage(pixels) != 0 && copy(true, level, layer, width);
}

bool VenusImage::read_subresource(uint32_t level, uint32_t layer, ArrayView<uint32_t> out) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (!transferable(level, layer, out.size()) || is_wide_format(desc_.format) || is_compressed_format(desc_.format) || desc_.format == Format::D32Float) return false;
  const uint32_t width = mip_extent(desc_.extent.width, level);
  const uint32_t texel = is_packed32_format(desc_.format) ? bytes_per_pixel(desc_.format) : 4u;
  if (!device_.staging(out.size() * texel)) return false;
  return copy(false, level, layer, width) && unstage(out);
}

bool VenusImage::write_subresource_float(uint32_t level, uint32_t layer, ArrayView<const Vec4> pixels) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (!transferable(level, layer, pixels.size()) || !is_wide_format(desc_.format)) return false;
  return stage_wide(pixels) && copy(true, level, layer, mip_extent(desc_.extent.width, level));
}

bool VenusImage::read_subresource_float(uint32_t level, uint32_t layer, ArrayView<Vec4> out) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (!transferable(level, layer, out.size()) || !is_wide_format(desc_.format)) return false;
  if (!device_.staging(out.size() * bytes_per_pixel(kept_format()))) return false;
  return copy(false, level, layer, mip_extent(desc_.extent.width, level)) && unstage_wide(out);
}

bool VenusImage::read_pixels(ArrayView<uint32_t> out) { return read_subresource(0, 0, out); }
bool VenusImage::write_pixels(ArrayView<const uint32_t> pixels) { return write_subresource(0, 0, pixels); }
bool VenusImage::read_pixels_float(ArrayView<Vec4> out) { return read_subresource_float(0, 0, out); }
bool VenusImage::write_pixels_float(ArrayView<const Vec4> pixels) { return write_subresource_float(0, 0, pixels); }
bool VenusImage::write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) { return desc_.kind != TextureKind::Texture2D && write_subresource(0, layer, pixels); }
bool VenusImage::read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) { return desc_.kind != TextureKind::Texture2D && read_subresource(0, layer, out); }
bool VenusImage::write_layer_pixels_float(uint32_t layer, ArrayView<const Vec4> pixels) {
  return desc_.kind != TextureKind::Texture2D && write_subresource_float(0, layer, pixels);
}
bool VenusImage::read_layer_pixels_float(uint32_t layer, ArrayView<Vec4> out) { return desc_.kind != TextureKind::Texture2D && read_subresource_float(0, layer, out); }
bool VenusImage::write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) { return desc_.kind == TextureKind::Texture2D && write_subresource(level, 0, pixels); }
bool VenusImage::read_mip_pixels(uint32_t level, ArrayView<uint32_t> out) { return desc_.kind == TextureKind::Texture2D && read_subresource(level, 0, out); }

bool VenusImage::write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (area.width == 0 || area.height == 0 || area.x < 0 || area.y < 0 || pitch < area.width || desc_.samples != 1) return false;
  if (static_cast<uint64_t>(area.x) + area.width > desc_.extent.width || static_cast<uint64_t>(area.y) + area.height > desc_.extent.height) return false;
  if (is_wide_format(desc_.format) || is_compressed_format(desc_.format) || desc_.format == Format::D32Float) return false;
  const uint64_t last = uint64_t{area.height - 1} * pitch + area.width;
  if (pixels.size() < last) return false;
  const uint32_t texel = is_packed32_format(desc_.format) ? bytes_per_pixel(desc_.format) : 4u;
  VenusMemory* s = device_.staging(uint64_t{area.width} * area.height * texel);
  if (!s) return false;
  // (the rectangle's rows one after another, then one copy)
  for (uint32_t y = 0; y < area.height; ++y) {
    ArrayView<const uint32_t> row = pixels.subview(uint64_t{y} * pitch, area.width);
    ByteWriter out{ByteView(s->map + uint64_t{y} * area.width * texel, uint64_t{area.width} * texel)};
    for (const uint32_t p : row) {
      if (texel == 4) out.le32(swap_rb_ ? (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF) : p);
      else if (desc_.format == Format::A8Unorm) out.u8(static_cast<uint8_t>(p >> 24));
      else if (texel == 1) out.u8(static_cast<uint8_t>(packed32_from_argb(desc_.format, p)));
      else out.le16(static_cast<uint16_t>(packed32_from_argb(desc_.format, p)));
    }
    if (!out.ok()) return false;
  }
  VkBufferImageCopy region{};
  region.bufferRowLength = area.width;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {area.x, area.y, 0};
  region.imageExtent = {area.width, area.height, 1};
  const TcosKernelVulkan& vk = device_.vk();
  VkImage image = image_;
  VkBuffer buffer = s->buffer;
  return device_.run_once([&](VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vk.CmdPipelineBarrier2(cb, &di);
    vk.CmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  });
}

bool VenusImage::write_compressed(uint32_t level, uint32_t layer, ConstByteView blocks) {
  kernel::ScopedMutex staged(device_.staging_lock());   // (the staging buffer is the device's: one transfer at a time)
  if (!is_compressed_format(desc_.format) || level >= desc_.mip_levels || layer >= level_layers(level)) return false;
  const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
  if (blocks.size() != compressed_image_bytes(desc_.format, width, height)) return false;
  VenusMemory* s = device_.staging(blocks.size());
  if (!s || !ByteView(s->map, blocks.size()).copy_from(blocks)) return false;
  return copy(true, level, layer, (width + 3) & ~3u);
}

// ---- the planes of depth images and whole subresources (kFeature2DepthCopies) ----------------------------------------------------------------------
namespace {
// What came before (the host's writes included) is done and visible to the copies that follow.
void barrier_before_copies(const TcosKernelVulkan& vk, VkCommandBuffer cb) {
  VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
  vk.CmdPipelineBarrier2(cb, &di);
}
}  // namespace

bool VenusImage::depth_planes_ok(uint32_t level, uint32_t layer, uint64_t depth, uint64_t stencil) const {
  if (desc_.format != Format::D32Float || desc_.samples != 1 || level >= desc_.mip_levels || layer >= level_layers(level)) return false;
  const uint64_t count = uint64_t{mip_extent(desc_.extent.width, level)} * mip_extent(desc_.extent.height, level);
  return (depth == 0 || depth == count) && (stencil == 0 || (stencil == count && desc_.stencil)) && (depth != 0 || stencil != 0);
}

bool VenusImage::copy_planes(bool to_image, uint32_t level, uint32_t layer, bool depth, bool stencil, uint64_t stencil_offset) {
  const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
  VenusMemory* staging = device_.staging(0);
  if (!staging) return false;
  VkBufferImageCopy regions[2] = {};
  uint32_t n = 0;
  for (uint32_t plane = 0; plane < 2; ++plane) {
    if (plane == 0 ? !depth : !stencil) continue;
    VkBufferImageCopy& r = regions[n++];
    r.bufferOffset = plane == 0 ? 0 : stencil_offset;   // (rows tightly packed: 4 bytes a texel of depth, 1 of stencil)
    r.imageSubresource = {static_cast<VkImageAspectFlags>(plane == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT), level, layer, 1};
    r.imageExtent = {width, height, 1};
  }
  const TcosKernelVulkan& vk = device_.vk();
  VkImage image = image_;
  VkBuffer buffer = staging->buffer;
  return device_.run_once([&](VkCommandBuffer cb) {
    barrier_before_copies(vk, cb);
    if (to_image) vk.CmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_GENERAL, n, regions);
    else vk.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buffer, n, regions);
  });
}

bool VenusImage::read_depth_subresource(uint32_t level, uint32_t layer, ArrayView<float> depth, ArrayView<uint8_t> stencil) {
  if (!depth_planes_ok(level, layer, depth.size(), stencil.size())) return false;
  kernel::ScopedMutex staged(device_.staging_lock());
  const uint64_t count = depth.empty() ? stencil.size() : depth.size();
  const uint64_t stencil_offset = (count * 4 + 3) & ~uint64_t{3};   // (a stencil region's offset is a multiple of 4)
  VenusMemory* s = device_.staging(stencil_offset + count);
  if (!s || !copy_planes(false, level, layer, !depth.empty(), !stencil.empty(), stencil_offset)) return false;
  ByteReader in{ConstByteView(s->map, s->size)};
  for (float& d : depth) d = __builtin_bit_cast(float, in.le32());
  if (!stencil.empty()) {
    ConstByteView plane;
    if (!ConstByteView(s->map, s->size).try_subview(stencil_offset, count, &plane)) return false;
    ByteView(stencil.data(), stencil.size()).copy_from(plane);
  }
  return in.ok();
}

bool VenusImage::write_depth_subresource(uint32_t level, uint32_t layer, ArrayView<const float> depth, ArrayView<const uint8_t> stencil) {
  if (!depth_planes_ok(level, layer, depth.size(), stencil.size())) return false;
  kernel::ScopedMutex staged(device_.staging_lock());
  const uint64_t count = depth.empty() ? stencil.size() : depth.size();
  const uint64_t stencil_offset = (count * 4 + 3) & ~uint64_t{3};
  VenusMemory* s = device_.staging(stencil_offset + count);
  if (!s) return false;
  ByteWriter out{ByteView(s->map, s->size)};
  for (const float v : depth) out.le32(__builtin_bit_cast(uint32_t, !(v == v) ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v));   // (a depth image keeps 0 .. 1)
  if (!out.ok()) return false;
  if (!stencil.empty()) {
    ByteView plane;
    if (!ByteView(s->map, s->size).try_subview(stencil_offset, count, &plane) || !plane.copy_from(ConstByteView(stencil.data(), stencil.size()))) return false;
  }
  // (one plane of two: Vulkan copies each aspect by itself, so the other keeps what it had)
  return copy_planes(true, level, layer, !depth.empty(), !stencil.empty(), stencil_offset);
}

// A whole subresource of another image of this device: the same format, samples and size (the planes asked for of a depth image; a volume's layer is a slice).
bool VenusImage::copy_subresource(IImage& source_image, uint32_t level, uint32_t layer, uint32_t source_level, uint32_t source_layer, uint32_t planes) {
  VenusImage& from = static_cast<VenusImage&>(source_image);   // (the kernel copies between images of one device)
  const bool depth = desc_.format == Format::D32Float;
  const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
  if (from.desc_.format != desc_.format || from.desc_.samples != desc_.samples || from.desc_.stencil != desc_.stencil || from.desc_.kind != desc_.kind ||
      level >= desc_.mip_levels || layer >= level_layers(level) || source_level >= from.desc_.mip_levels || source_layer >= from.level_layers(source_level) ||
      mip_extent(from.desc_.extent.width, source_level) != width || mip_extent(from.desc_.extent.height, source_level) != height || planes == 0 || planes > 3 ||
      (!depth && planes != 1) || ((planes & 2) && !desc_.stencil) || (&from == this && level == source_level && layer == source_layer))
    return false;
  const VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT, stencil_aspect = VK_IMAGE_ASPECT_STENCIL_BIT, color_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageAspectFlags aspect = depth ? ((planes & 1) ? depth_aspect : 0u) | ((planes & 2) ? stencil_aspect : 0u) : color_aspect;
  const bool volume = desc_.kind == TextureKind::Texture3D;
  VkImageCopy region{};
  region.srcSubresource = {aspect, source_level, volume ? 0u : source_layer, 1};
  region.srcOffset = {0, 0, volume ? static_cast<int32_t>(source_layer) : 0};
  region.dstSubresource = {aspect, level, volume ? 0u : layer, 1};
  region.dstOffset = {0, 0, volume ? static_cast<int32_t>(layer) : 0};
  region.extent = {width, height, 1};
  const TcosKernelVulkan& vk = device_.vk();
  VkImage src = from.image_, dst = image_;
  return device_.run_once([&](VkCommandBuffer cb) {
    barrier_before_copies(vk, cb);
    vk.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  });
}

// Every level above 0 from the one before it, by linear blits (Vulkan's box filter for a halving): every layer its own chain, a volume's levels from its slices.
bool VenusImage::generate_mipmaps() {
  if (desc_.mip_levels < 2 || is_compressed_format(desc_.format) || desc_.format == Format::D32Float || is_integer_format(desc_.format)) return false;
  if (!device_.format_supports(format_, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return false;
  const TcosKernelVulkan& vk = device_.vk();
  const bool volume = desc_.kind == TextureKind::Texture3D;
  return device_.run_once([&](VkCommandBuffer cb) {
    for (uint32_t level = 1; level < desc_.mip_levels; ++level) {
      VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
      mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
      mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
      VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
      di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
      vk.CmdPipelineBarrier2(cb, &di);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, volume ? 1u : layers_};
      blit.srcOffsets[1] = {static_cast<int32_t>(mip_extent(desc_.extent.width, level - 1)), static_cast<int32_t>(mip_extent(desc_.extent.height, level - 1)),
                            volume ? static_cast<int32_t>(level_layers(level - 1)) : 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, volume ? 1u : layers_};
      blit.dstOffsets[1] = {static_cast<int32_t>(mip_extent(desc_.extent.width, level)), static_cast<int32_t>(mip_extent(desc_.extent.height, level)),
                            volume ? static_cast<int32_t>(level_layers(level)) : 1};
      vk.CmdBlitImage(cb, image_, VK_IMAGE_LAYOUT_GENERAL, image_, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    }
  });
}

// A view of the image for sampling: its kind, levels and layers (made once each). The stencil plane of a depth image reads (0, s, 0, 1); its depth (d, 0, 0, 1);
// A8Unorm (0, 0, 0, a).
VkImageView VenusImage::sampled_view(const TextureView& v, bool stencil) {
  for (const View& c : views_)
    if (!c.storage && c.stencil == stencil && c.v.kind == v.kind && c.v.base_level == v.base_level && c.v.level_count == v.level_count &&
        c.v.first_layer == v.first_layer && c.v.layer_count == v.layer_count)
      return c.view;
  VkImageViewCreateInfo ci = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  ci.image = image_;
  switch (v.kind) {
    case TextureKind::Texture3D: ci.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
    case TextureKind::Texture2DArray: case TextureKind::Texture2DMSArray: ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case TextureKind::TextureCube: ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case TextureKind::TextureCubeArray: ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
    default: ci.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
  }
  ci.format = format_;
  ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  if (desc_.format == Format::D32Float) {
    aspect = stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil) ci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    else ci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
  } else if (desc_.format == Format::A8Unorm) {
    ci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
  } else if (desc_.format == Format::R8Unorm || desc_.format == Format::R8G8Unorm) {
    // (the packed 32-bit formats read their missing channels as 0 and alpha as 1, as their 0xAARRGGBB words say)
    ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE};
  }
  ci.subresourceRange = {aspect, v.base_level, v.level_count, v.first_layer, v.layer_count};
  if (desc_.kind == TextureKind::Texture3D) ci.subresourceRange.baseArrayLayer = 0, ci.subresourceRange.layerCount = 1;
  VkImageView view = nullptr;
  if (device_.vk().CreateImageView(device_.device(), &ci, nullptr, &view) != VK_SUCCESS) return nullptr;
  View c;
  c.v = v, c.stencil = stencil, c.view = view;
  if (!views_.push_back(c)) {
    device_.vk().DestroyImageView(device_.device(), view, nullptr);
    return nullptr;
  }
  return view;
}

// The whole image as its own kind (bind_texture without a view).
VkImageView VenusImage::whole_view(bool stencil) {
  TextureView v;
  v.kind = desc_.kind;
  v.base_level = 0, v.level_count = desc_.mip_levels;
  v.first_layer = 0, v.layer_count = desc_.kind == TextureKind::Texture3D ? 1 : layers_;
  return sampled_view(v, stencil);
}

// A storage view: one level, a run of layers (a volume's slices: the whole level as a 3D view), in `format` (the image's or a same-size reinterpretation).
VkImageView VenusImage::storage_view(uint32_t level, uint32_t first_layer, uint32_t layer_count, VkFormat format) {
  for (const View& c : views_)
    if (c.storage && c.v.base_level == level && c.v.first_layer == first_layer && c.v.layer_count == layer_count && c.format == format) return c.view;
  VkImageViewCreateInfo ci = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  ci.image = image_;
  const bool volume = desc_.kind == TextureKind::Texture3D;
  ci.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : (desc_.kind == TextureKind::Texture2D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY);
  ci.format = format;
  ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, volume ? 0u : first_layer, volume ? 1u : layer_count};
  VkImageView view = nullptr;
  if (device_.vk().CreateImageView(device_.device(), &ci, nullptr, &view) != VK_SUCCESS) return nullptr;
  View c;
  c.storage = true, c.format = format, c.view = view;
  c.v.base_level = level, c.v.first_layer = first_layer, c.v.layer_count = layer_count;
  if (!views_.push_back(c)) {
    device_.vk().DestroyImageView(device_.device(), view, nullptr);
    return nullptr;
  }
  return view;
}

// An attachment view: one level, a run of layers (a volume's slices through its 2D-array compatibility).
VkImageView VenusImage::target_view(uint32_t level, uint32_t first_layer, uint32_t layer_count) {
  for (const Target& t : targets_)
    if (t.level == level && t.first == first_layer && t.count == layer_count) return t.view;
  VkImageViewCreateInfo ci = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  ci.image = image_;
  ci.viewType = layer_count > 1 || desc_.kind != TextureKind::Texture2D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  ci.format = format_;
  ci.subresourceRange = {aspect_, level, 1, first_layer, layer_count};
  VkImageView view = nullptr;
  if (device_.vk().CreateImageView(device_.device(), &ci, nullptr, &view) != VK_SUCCESS) return nullptr;
  Target t;
  t.level = level, t.first = first_layer, t.count = layer_count, t.view = view;
  if (!targets_.push_back(t)) {
    device_.vk().DestroyImageView(device_.device(), view, nullptr);
    return nullptr;
  }
  return view;
}

// ---- presentation ----------------------------------------------------------------------------------------------------------------------------------
// The image's pixels go to the virtio-gpu's 2D screen (the display driver's own: monitors, layouts, the console's comeback all work as they do for every 2D
// present). One copy through guest memory per frame.
bool VenusDevice::present(IImage* image) {
  if (!image) return false;
  VenusImage* v = static_cast<VenusImage*>(image);
  if (!(v->desc().usage & kImagePresent)) return false;
  const Extent2D e = v->extent();
  const uint64_t count = uint64_t{e.width} * e.height;
  kernel::ScopedMutex staged(staging_lock_);   // (the frame goes through the staging buffer, which image transfers on other threads use too)
  VenusMemory* s = staging(count * 4);
  if (!s) return false;
  if (!v->copy(false, 0, 0, e.width)) return false;
  ArrayView<uint32_t> words;   // (B8G8R8A8 rows are 0xAARRGGBB words)
  if (!ByteView(s->map, count * 4).try_cast<uint32_t>(&words)) return false;
  if (v->swaps_rb())
    for (uint32_t& w : words) w = (w & 0xFF00FF00u) | ((w & 0xFF) << 16) | ((w >> 16) & 0xFF);
  screen_.blit(0, 0, e.width < screen_.width() ? e.width : screen_.width(), e.height < screen_.height() ? e.height : screen_.height(), words.data(), e.width);
  screen_.present();
  return true;
}

}  // namespace graphics

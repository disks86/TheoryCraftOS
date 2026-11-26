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

// VenusDevice::submit: a CommandRecorder's list replayed into a Vulkan command buffer, with VirglDevice's refusals and semantics (nothing survives a submit; a
// pass resets the pipeline; textures, buffers, constants and topology carry over between passes), then submitted and waited for. Every pass, dispatch and
// storing draw is fenced off by a full memory barrier, which is what GL gives virgl implicitly.
#include <new>
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Graphics/Graphics3D/Venus/VenusObjects.hpp"
#include "Utility/ByteStream.hpp"

namespace graphics {

ICommandBuffer* VenusDevice::create_command_buffer() {
  void* memory = allocator_.allocate_aligned(sizeof(VenusCommands), alignof(VenusCommands));
  if (!memory) return nullptr;
  return new (memory) VenusCommands(allocator_);
}

void VenusDevice::destroy_command_buffer(ICommandBuffer* commands) {
  if (!commands) return;
  VenusCommands* c = static_cast<VenusCommands*>(commands);
  c->~VenusCommands();
  allocator_.deallocate(c);
}

VkDescriptorPool VenusDevice::descriptor_pool_current() { return pools_.count() ? pools_[pools_.count() - 1] : nullptr; }

namespace {

VkFormat texel_vk_format(TexelFormat f) {
  switch (f) {
    case TexelFormat::R32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TexelFormat::R32G32B32A32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case TexelFormat::R32G32B32A32Sint: return VK_FORMAT_R32G32B32A32_SINT;
    case TexelFormat::R32G32B32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case TexelFormat::R32G32B32Uint: return VK_FORMAT_R32G32B32_UINT;
    case TexelFormat::R32G32B32Sint: return VK_FORMAT_R32G32B32_SINT;
    case TexelFormat::R16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TexelFormat::R16G16B16A16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case TexelFormat::R16G16B16A16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case TexelFormat::R16G16B16A16Snorm: return VK_FORMAT_R16G16B16A16_SNORM;
    case TexelFormat::R16G16B16A16Sint: return VK_FORMAT_R16G16B16A16_SINT;
    case TexelFormat::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    case TexelFormat::R32G32Uint: return VK_FORMAT_R32G32_UINT;
    case TexelFormat::R32G32Sint: return VK_FORMAT_R32G32_SINT;
    case TexelFormat::R10G10B10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case TexelFormat::R10G10B10A2Uint: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case TexelFormat::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case TexelFormat::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TexelFormat::R8G8B8A8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case TexelFormat::R8G8B8A8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
    case TexelFormat::R8G8B8A8Sint: return VK_FORMAT_R8G8B8A8_SINT;
    case TexelFormat::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case TexelFormat::R16G16Unorm: return VK_FORMAT_R16G16_UNORM;
    case TexelFormat::R16G16Uint: return VK_FORMAT_R16G16_UINT;
    case TexelFormat::R16G16Snorm: return VK_FORMAT_R16G16_SNORM;
    case TexelFormat::R16G16Sint: return VK_FORMAT_R16G16_SINT;
    case TexelFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
    case TexelFormat::R32Uint: return VK_FORMAT_R32_UINT;
    case TexelFormat::R32Sint: return VK_FORMAT_R32_SINT;
    case TexelFormat::R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
    case TexelFormat::R8G8Uint: return VK_FORMAT_R8G8_UINT;
    case TexelFormat::R8G8Snorm: return VK_FORMAT_R8G8_SNORM;
    case TexelFormat::R8G8Sint: return VK_FORMAT_R8G8_SINT;
    case TexelFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
    case TexelFormat::R16Unorm: return VK_FORMAT_R16_UNORM;
    case TexelFormat::R16Uint: return VK_FORMAT_R16_UINT;
    case TexelFormat::R16Snorm: return VK_FORMAT_R16_SNORM;
    case TexelFormat::R16Sint: return VK_FORMAT_R16_SINT;
    case TexelFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
    case TexelFormat::R8Uint: return VK_FORMAT_R8_UINT;
    case TexelFormat::R8Snorm: return VK_FORMAT_R8_SNORM;
    case TexelFormat::R8Sint: return VK_FORMAT_R8_SINT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

tgsi::ValueType value_of(Format f) {
  if (!is_integer_format(f)) return tgsi::ValueType::Float;
  return is_signed_integer_format(f) ? tgsi::ValueType::Sint : tgsi::ValueType::Uint;
}

tgsi::ValueType value_of(TexelFormat f) {
  const TexelKind k = texel_layout(f).kind;
  return k == TexelKind::Uint ? tgsi::ValueType::Uint : k == TexelKind::Sint ? tgsi::ValueType::Sint : tgsi::ValueType::Float;
}

// The TGSI image format a storage image is accessed as (the format the shader's image type names).
tgsi::ImageFormat tgsi_format(Format f) {
  using I = tgsi::ImageFormat;
  switch (f) {
    case Format::B8G8R8A8Unorm: case Format::R8G8B8A8Unorm: return I::R8G8B8A8Unorm;   // (8-bit storage images are kept RGBA)
    case Format::R16G16B16A16Float: return I::R16G16B16A16Float;
    case Format::R32G32B32A32Float: return I::R32G32B32A32Float;
    case Format::R16G16B16A16Unorm: return I::R16G16B16A16Unorm;
    case Format::R32G32B32A32Uint: return I::R32G32B32A32Uint;
    case Format::R32G32B32A32Sint: return I::R32G32B32A32Sint;
    case Format::R16G16B16A16Uint: return I::R16G16B16A16Uint;
    case Format::R16G16B16A16Sint: return I::R16G16B16A16Sint;
    case Format::R8G8B8A8Uint: return I::R8G8B8A8Uint;
    case Format::R16G16B16A16Snorm: return I::R16G16B16A16Snorm;
    case Format::R10G10B10A2Unorm: return I::R10G10B10A2Unorm;
    case Format::R32Uint: return I::R32Uint;
    case Format::R32Sint: return I::R32Sint;
    case Format::R32Float: return I::R32Float;
    case Format::R8Unorm: return I::R8Unorm;
    case Format::R8G8Unorm: return I::R8G8Unorm;
    case Format::R8Snorm: return I::R8Snorm;
    case Format::R8Uint: return I::R8Uint;
    case Format::R8Sint: return I::R8Sint;
    case Format::R8G8Snorm: return I::R8G8Snorm;
    case Format::R8G8Uint: return I::R8G8Uint;
    case Format::R8G8Sint: return I::R8G8Sint;
    case Format::R8G8B8A8Snorm: return I::R8G8B8A8Snorm;
    case Format::R8G8B8A8Sint: return I::R8G8B8A8Sint;
    case Format::R16Unorm: return I::R16Unorm;
    case Format::R16Snorm: return I::R16Snorm;
    case Format::R16Uint: return I::R16Uint;
    case Format::R16Sint: return I::R16Sint;
    case Format::R16Float: return I::R16Float;
    case Format::R16G16Unorm: return I::R16G16Unorm;
    case Format::R16G16Snorm: return I::R16G16Snorm;
    case Format::R16G16Uint: return I::R16G16Uint;
    case Format::R16G16Sint: return I::R16G16Sint;
    case Format::R16G16Float: return I::R16G16Float;
    case Format::R32G32Uint: return I::R32G32Uint;
    case Format::R32G32Sint: return I::R32G32Sint;
    case Format::R32G32Float: return I::R32G32Float;
    case Format::R11G11B10Float: return I::R11G11B10Float;
    case Format::R10G10B10A2Uint: return I::R10G10B10A2Uint;
    default: return I::None;
  }
}

tgsi::ImageFormat tgsi_format(TexelFormat f) {
  using I = tgsi::ImageFormat;
  switch (f) {
    case TexelFormat::R32G32B32A32Float: return I::R32G32B32A32Float;
    case TexelFormat::R32G32B32A32Uint: return I::R32G32B32A32Uint;
    case TexelFormat::R32G32B32A32Sint: return I::R32G32B32A32Sint;
    case TexelFormat::R16G16B16A16Float: return I::R16G16B16A16Float;
    case TexelFormat::R16G16B16A16Unorm: return I::R16G16B16A16Unorm;
    case TexelFormat::R16G16B16A16Uint: return I::R16G16B16A16Uint;
    case TexelFormat::R16G16B16A16Snorm: return I::R16G16B16A16Snorm;
    case TexelFormat::R16G16B16A16Sint: return I::R16G16B16A16Sint;
    case TexelFormat::R32G32Float: return I::R32G32Float;
    case TexelFormat::R32G32Uint: return I::R32G32Uint;
    case TexelFormat::R32G32Sint: return I::R32G32Sint;
    case TexelFormat::R10G10B10A2Unorm: return I::R10G10B10A2Unorm;
    case TexelFormat::R10G10B10A2Uint: return I::R10G10B10A2Uint;
    case TexelFormat::R11G11B10Float: return I::R11G11B10Float;
    case TexelFormat::R8G8B8A8Unorm: return I::R8G8B8A8Unorm;
    case TexelFormat::R8G8B8A8Uint: return I::R8G8B8A8Uint;
    case TexelFormat::R8G8B8A8Snorm: return I::R8G8B8A8Snorm;
    case TexelFormat::R8G8B8A8Sint: return I::R8G8B8A8Sint;
    case TexelFormat::R16G16Float: return I::R16G16Float;
    case TexelFormat::R16G16Unorm: return I::R16G16Unorm;
    case TexelFormat::R16G16Uint: return I::R16G16Uint;
    case TexelFormat::R16G16Snorm: return I::R16G16Snorm;
    case TexelFormat::R16G16Sint: return I::R16G16Sint;
    case TexelFormat::R32Float: return I::R32Float;
    case TexelFormat::R32Uint: return I::R32Uint;
    case TexelFormat::R32Sint: return I::R32Sint;
    case TexelFormat::R8G8Unorm: return I::R8G8Unorm;
    case TexelFormat::R8G8Uint: return I::R8G8Uint;
    case TexelFormat::R8G8Snorm: return I::R8G8Snorm;
    case TexelFormat::R8G8Sint: return I::R8G8Sint;
    case TexelFormat::R16Float: return I::R16Float;
    case TexelFormat::R16Unorm: return I::R16Unorm;
    case TexelFormat::R16Uint: return I::R16Uint;
    case TexelFormat::R16Snorm: return I::R16Snorm;
    case TexelFormat::R16Sint: return I::R16Sint;
    case TexelFormat::R8Unorm: return I::R8Unorm;
    case TexelFormat::R8Uint: return I::R8Uint;
    case TexelFormat::R8Snorm: return I::R8Snorm;
    case TexelFormat::R8Sint: return I::R8Sint;
    default: return I::None;
  }
}

struct BoundTexture {
  VenusImage* image = nullptr;
  SamplerDesc sampler;
  bool has_view = false;
  TextureView view;
};
struct BoundStorage {
  VenusBuffer* buffer = nullptr;
  uint64_t offset = 0, size = 0;
};
struct BoundImage {
  VenusImage* image = nullptr;
  StorageImageView view;
};
struct BoundTexel {
  VenusBuffer* buffer = nullptr;
  TexelBufferView view;
};

// One submit's replay.
class Replay {
 public:
  Replay(VenusDevice& device, VenusCommands& commands)
      : d_(device), vk_(device.vk()), rec_(commands), views_(device.allocator()), pipelines_(device.allocator()), segments_(device.allocator()), so_draws_(device.allocator()) {
    for (int32_t& c : current_) c = -1;
    features2_ = device.info().features2;
  }
  bool run();
  void release();   // (after the GPU is done: what the replay made for itself)

 private:
  bool begin_pass(const RenderPassBegin& p);
  void end_pass();
  bool resume_rendering();
  void barrier();
  bool draw(const RecordedCommand& c, bool indexed);
  bool dispatch(const RecordedCommand& c);
  bool bind_resources(VenusPipeline& p, VenusPipeline::Variant& v, VkPipelineBindPoint point);
  bool key_for(VenusPipeline& p, VenusVariantKey* key);
  uint32_t constants_offset(bool* ok);
  VkBufferView buffer_view(VenusBuffer* b, uint64_t offset, uint64_t range, VkFormat format);
  bool flush_midway();
  void set_viewports();
  void note_pipeline(VenusPipeline* p);
  // queries
  bool begin_query(const RecordedCommand& c);
  bool end_query(const RecordedCommand& c);
  void segments_close();
  void segments_open();
  void segment_close(uint32_t category);
  void segment_open(uint32_t category, bool no_fragment);
  void set_depth_bounds_state();
  bool copy_samples(const RecordedCommand& c);
  // gap pass 5: sample positions, view instancing, coarse shading
  bool set_sample_positions(const RecordedCommand& c, ConstByteView all_constants);
  bool custom_locations() const;
  void set_sample_locations();
  bool set_shading_rate_image(VenusImage* image);
  bool prepare_rate_attachment();
  VkRenderingFragmentShadingRateAttachmentInfoKHR rate_info() {
    VkRenderingFragmentShadingRateAttachmentInfoKHR r = vk_struct<VkRenderingFragmentShadingRateAttachmentInfoKHR>(VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
    const VenusDevice::RateAttachment* a = rate_attached_ ? d_.rate_attachment(cb_) : nullptr;
    r.imageView = a ? a->view : nullptr;
    r.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    r.shadingRateAttachmentTexelSize = {d_.context().caps().shading_rate_texel, d_.context().caps().shading_rate_texel};
    return r;
  }
  void set_fragment_shading_rate();
  bool read_queries();
  bool resolve(const RecordedCommand& c);
  // stream output
  bool bind_stream_output(const RecordedCommand& c);
  bool begin_stream_output(VenusPipeline& p);
  void end_stream_output();
  void stream_output_results();

  VenusDevice& d_;
  const TcosKernelVulkan& vk_;
  VenusCommands& rec_;
  VkCommandBuffer cb_ = nullptr;
  // the pass
  bool in_pass_ = false, rendering_ = false;
  RenderPassBegin pass_;
  VenusImage* targets_[kMaxColorTargets] = {};
  uint32_t target_count_ = 0;
  VenusImage* depth_ = nullptr;
  Extent2D extent_;
  uint32_t layers_ = 1;
  // bindings
  VenusPipeline* pipeline_ = nullptr;          // graphics
  VenusPipeline* compute_ = nullptr;
  VkPipeline bound_pipeline_ = nullptr;
  struct VertexBinding {
    VenusBuffer* buffer = nullptr;
    uint64_t offset = 0;
  } vertex_[kMaxVertexBuffers];
  VenusBuffer* index_ = nullptr;
  IndexType index_type_ = IndexType::Uint16;
  uint64_t index_offset_ = 0;
  BoundTexture textures_[kMaxTextureUnits];
  BoundStorage storage_[kMaxStorageBuffers];
  BoundImage images_[kMaxStorageImages];
  BoundTexel texels_[kMaxImageSlots];
  ConstByteView constants_;
  bool constants_dirty_ = true;
  uint32_t constants_at_ = 0;      // where the current block is in the ring
  uint32_t ring_used_ = 0;
  uint32_t flushes_ = 0;
  PrimitiveTopology topology_ = PrimitiveTopology::TriangleList;
  uint32_t patch_points_ = 0;
  Viewport viewports_[kMaxViewports];
  Rect2D scissors_[kMaxViewports];
  uint32_t viewport_count_ = 1, scissor_count_ = 1;
  // the depth bounds test (kFeature2DepthBounds; dynamic state, set before a draw when it changed or the command buffer is new)
  bool depth_bounds_ = false, depth_bounds_dirty_ = true;
  float depth_min_ = 0, depth_max_ = 1;
  uint32_t features2_ = 0;   // (the device's DeviceInfo::features2: the commands of features it does not offer fail the submit)
  // the sample positions set (kFeature2SamplePositions; 0 pixels: the standard ones)
  uint32_t positions_samples_ = 0, positions_pixels_ = 0;
  SamplePosition positions_[kMaxSamplePositions];
  // view instancing (kFeature2ViewInstancing): the view of the draws that follow and what is added to their viewport and layer (the constants' system row)
  uint32_t view_id_ = 0, view_viewport_ = 0, view_layer_ = 0;
  // coarse shading (kFeature2ShadingRate): the rate, the combiners, the shading-rate image and whether the current rendering reads it
  uint32_t rate_ = 0;
  ShadingRateCombiner rate_primitive_ = ShadingRateCombiner::Passthrough, rate_image_combiner_ = ShadingRateCombiner::Passthrough;
  VenusImage* rate_image_ = nullptr;
  bool rate_attached_ = false;
  // what the replay made
  struct MadeView {
    VkBufferView view;
  };
  List<MadeView> views_;
  List<VenusPipeline*> pipelines_;   // (their descriptor sets are given back after the submit)
  // Queries. Vulkan, like GL, counts one query of a kind at a time: each query of ours is the sum of the segments of its kind's pool that ran while it was open.
  // A segment ends and the next begins whenever a query of its kind begins or ends, a rendering begins or ends, or the command buffer is split; a timestamp is
  // written where its query ends.
  enum Category : uint32_t { kCatOcclusion, kCatStatistics, kCatStream0, kCategories = kCatStream0 + kMaxGeometryStreams };
  struct UserQuery {
    bool begun = false, ended = false;
    QueryKind kind = QueryKind::Occlusion;
    uint32_t stream = 0;
    uint32_t timestamp = 0;
    uint64_t synthetic = 0;
  };
  struct Segment {
    uint32_t category = 0, index = 0;
    bool no_fragment = false;   // (a statistics segment around a draw whose pixel shader stands in for none: its fragment shader runs are not counted)
    uint64_t open[kMaxOcclusionQueries / 64] = {};
  };
  UserQuery queries_[kMaxOcclusionQueries];
  uint64_t open_[kMaxOcclusionQueries / 64] = {};
  int32_t order_[kMaxOcclusionQueries];   // (the open queries in the order they began: end_occlusion_query ends the newest)
  uint32_t order_count_ = 0;
  List<Segment> segments_;
  int32_t current_[kCategories];
  uint32_t used_[VenusDevice::kPoolKinds] = {};
  bool any_queries_ = false;
  // Stream output. Each binding (a Fill) starts at its offset; each draw that writes it reads a counter slot and leaves where it got to in a new one, so that the start
  // of every draw is known afterwards. (A host that finds a primitive too big for the room left sets the counter to the binding's end - RADV does -, where Direct3D
  // keeps it after the last primitive written: the read-back works Direct3D's out from the draw's start and its primitives' size.)
  struct Target {
    VenusBuffer* buffer = nullptr;
    uint32_t counter = 0;                // (the slot with where the next draw starts)
    uint32_t fill = 0;
    uint64_t end = kStreamOutputNoEnd;   // (where the writes stop: the binding's end, or the buffer's)
  } so_[kMaxStreamOutputBuffers];
  struct Fill {
    VenusBuffer* buffer = nullptr;
    uint32_t start_slot = 0;
    uint64_t limit = 0;                  // (the binding's end within the buffer)
  };
  struct SoDraw {
    uint32_t fill = 0, read = 0, write = 0;
    uint32_t primitive_bytes = 0;        // (what one primitive of the draw takes in this buffer: 0 when not known)
  };
  static constexpr uint32_t kMaxFills = 256;   // (bindings of stream output buffers a submit makes)
  Fill fills_[kMaxFills];
  uint32_t fill_count_ = 0;
  List<SoDraw> so_draws_;
  uint32_t slots_used_ = 0;
  bool new_slot(uint32_t* slot) {
    if (slots_used_ >= VenusDevice::kStreamOutputCounters) return false;
    *slot = slots_used_++;
    return true;
  }
  bool so_active_ = false;
  uint32_t so_used_ = 0;
  VkBuffer so_counter_buffers_[kMaxStreamOutputBuffers] = {};
  VkDeviceSize so_counter_offsets_[kMaxStreamOutputBuffers] = {};
};

uint32_t category_of(QueryKind k, uint32_t stream) {
  switch (k) {
    case QueryKind::Occlusion: case QueryKind::OcclusionPredicate: return 0;
    case QueryKind::SoPrimitivesWritten: case QueryKind::SoPrimitivesNeeded: return 2 + stream;
    case QueryKind::Timestamp: case QueryKind::TimestampFrequency: case QueryKind::TimestampDisjoint: return ~0u;
    default: return 1;
  }
}

void Replay::barrier() {
  VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
  vk_.CmdPipelineBarrier2(cb_, &di);
}

void Replay::note_pipeline(VenusPipeline* p) {
  for (VenusPipeline* q : pipelines_)
    if (q == p) return;
  if (pipelines_.push_back(p)) return;
}

VkBufferView Replay::buffer_view(VenusBuffer* b, uint64_t offset, uint64_t range, VkFormat format) {
  VkBufferViewCreateInfo ci = vk_struct<VkBufferViewCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
  ci.buffer = b->buffer(), ci.format = format, ci.offset = offset, ci.range = range;
  VkBufferView v = nullptr;
  if (vk_.CreateBufferView(d_.device(), &ci, nullptr, &v) != VK_SUCCESS) return nullptr;
  if (!views_.push_back({v})) {
    vk_.DestroyBufferView(d_.device(), v, nullptr);
    return nullptr;
  }
  return v;
}

void Replay::release() {
  for (const MadeView& v : views_) vk_.DestroyBufferView(d_.device(), v.view, nullptr);
  views_.clear();
  for (VenusPipeline* p : pipelines_) p->reset_sets();
}

// The pass's attachments, cleared or kept (VirglDevice's refusals: a colour target is needed, depth is D32F, the extents agree, no gaps in the extra targets).
bool Replay::begin_pass(const RenderPassBegin& p) {
  if (in_pass_ || !p.color) return false;
  VenusImage* color = static_cast<VenusImage*>(p.color);
  if (!(color->desc().usage & kImageColorAttachment) || color->format() == Format::D32Float) return false;
  VenusImage* depth = static_cast<VenusImage*>(p.depth);
  if (depth && (depth->format() != Format::D32Float || !(depth->desc().usage & kImageDepthAttachment))) return false;
  if (p.level[0] >= color->mip_levels() || (depth && p.level[kMaxColorTargets] >= depth->mip_levels())) return false;
  const Extent2D e = {mip_extent(color->extent().width, p.level[0]), mip_extent(color->extent().height, p.level[0])};
  if (depth && (mip_extent(depth->extent().width, p.level[kMaxColorTargets]) != e.width || mip_extent(depth->extent().height, p.level[kMaxColorTargets]) != e.height)) return false;
  if (depth && depth->desc().samples != color->desc().samples) return false;
  targets_[0] = color;
  target_count_ = 1;
  bool gap = false;
  for (uint32_t i = 0; i < kMaxColorTargets - 1; ++i) {
    VenusImage* x = static_cast<VenusImage*>(p.extra_colors[i]);
    if (!x) {
      gap = true;
      continue;
    }
    if (gap || !(x->desc().usage & kImageColorAttachment) || x->format() == Format::D32Float || x == color || x == depth) return false;
    for (uint32_t j = 0; j < target_count_; ++j)
      if (targets_[j] == x) return false;
    if (p.level[i + 1] >= x->mip_levels() || mip_extent(x->extent().width, p.level[i + 1]) != e.width || mip_extent(x->extent().height, p.level[i + 1]) != e.height ||
        x->desc().samples != color->desc().samples)
      return false;
    targets_[target_count_++] = x;
  }
  for (uint32_t t = 0; t <= kMaxColorTargets; ++t) {
    VenusImage* img = t == kMaxColorTargets ? depth : t < target_count_ ? targets_[t] : nullptr;
    if (!img) continue;
    const uint32_t have = img->level_layers(p.level[t]);
    if (p.layer_count[t] == 0 || p.first_layer[t] >= have || p.layer_count[t] > have - p.first_layer[t]) return false;
    if (!img->target_view(p.level[t], p.first_layer[t], p.layer_count[t])) return false;
  }
  pass_ = p;
  depth_ = depth;
  extent_ = e;
  layers_ = p.layer_count[0];
  for (uint32_t t = 1; t < target_count_; ++t)
    if (p.layer_count[t] > layers_) layers_ = p.layer_count[t];
  if (depth && p.layer_count[kMaxColorTargets] > layers_) layers_ = p.layer_count[kMaxColorTargets];
  in_pass_ = true;
  constants_dirty_ = true;   // (the pass's samples are in the constant block)
  pipeline_ = nullptr;   // (every pass binds its pipeline, as on virgl)
  bound_pipeline_ = nullptr;
  viewports_[0] = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
  scissors_[0] = {0, 0, e.width, e.height};
  viewport_count_ = scissor_count_ = 1;
  barrier();
  // the first rendering clears what the pass clears; resume_rendering keeps everything
  VkRenderingAttachmentInfo colors[kMaxColorTargets];
  for (uint32_t t = 0; t < target_count_; ++t) {
    colors[t] = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    colors[t].imageView = targets_[t]->target_view(p.level[t], p.first_layer[t], p.layer_count[t]);
    colors[t].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[t].loadOp = p.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[t].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const Vec4 c = p.clear_color_value;
    const Format f = targets_[t]->format();
    if (is_integer_format(f)) {   // (an integer target's clear colour is the integers' bits in the floats, as integer values travel everywhere in this interface)
      const float in[4] = {c.x, c.y, c.z, c.w};
      __builtin_memcpy(colors[t].clearValue.color.uint32, in, sizeof in);
    } else {
      colors[t].clearValue.color.float32[0] = c.x, colors[t].clearValue.color.float32[1] = c.y;
      colors[t].clearValue.color.float32[2] = c.z, colors[t].clearValue.color.float32[3] = c.w;
    }
  }
  VkRenderingAttachmentInfo dz = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
  VkRenderingAttachmentInfo st = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
  if (depth) {
    dz.imageView = depth->target_view(p.level[kMaxColorTargets], p.first_layer[kMaxColorTargets], p.layer_count[kMaxColorTargets]);
    dz.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dz.loadOp = p.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    dz.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    dz.clearValue.depthStencil = {p.clear_depth_value, p.clear_stencil_value};
    st = dz;
    st.loadOp = p.clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  }
  VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
  ri.renderArea = {{0, 0}, {e.width, e.height}};
  ri.layerCount = layers_;
  ri.colorAttachmentCount = target_count_, ri.pColorAttachments = colors;
  ri.pDepthAttachment = depth ? &dz : nullptr;
  ri.pStencilAttachment = depth && depth->desc().stencil ? &st : nullptr;
  segments_close();
  if (!prepare_rate_attachment()) return false;
  VkRenderingFragmentShadingRateAttachmentInfoKHR rate = rate_info();
  if (rate_attached_) ri.pNext = &rate;
  vk_.CmdBeginRendering(cb_, &ri);
  rendering_ = true;
  segments_open();
  return true;
}

// The pass's rendering again, keeping what the attachments hold (after a barrier inside the pass, or a flush).
bool Replay::resume_rendering() {
  VkRenderingAttachmentInfo colors[kMaxColorTargets];
  for (uint32_t t = 0; t < target_count_; ++t) {
    colors[t] = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    colors[t].imageView = targets_[t]->target_view(pass_.level[t], pass_.first_layer[t], pass_.layer_count[t]);
    colors[t].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[t].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, colors[t].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingAttachmentInfo dz = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
  if (depth_) {
    dz.imageView = depth_->target_view(pass_.level[kMaxColorTargets], pass_.first_layer[kMaxColorTargets], pass_.layer_count[kMaxColorTargets]);
    dz.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dz.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, dz.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
  ri.renderArea = {{0, 0}, {extent_.width, extent_.height}};
  ri.layerCount = layers_;
  ri.colorAttachmentCount = target_count_, ri.pColorAttachments = colors;
  ri.pDepthAttachment = depth_ ? &dz : nullptr;
  ri.pStencilAttachment = depth_ && depth_->desc().stencil ? &dz : nullptr;
  segments_close();
  if (!prepare_rate_attachment()) return false;
  VkRenderingFragmentShadingRateAttachmentInfoKHR rate = rate_info();
  if (rate_attached_) ri.pNext = &rate;
  vk_.CmdBeginRendering(cb_, &ri);
  rendering_ = true;
  segments_open();
  bound_pipeline_ = nullptr;   // (bindings are re-made at the next draw)
  return true;
}

void Replay::end_pass() {
  end_stream_output();
  segments_close();
  if (rendering_) vk_.CmdEndRendering(cb_);
  segments_open();
  rendering_ = false;
  in_pass_ = false;
  pipeline_ = nullptr;
  bound_pipeline_ = nullptr;
  barrier();
}

// The constant block of the next draw or dispatch, in this submit's ring (the push constants, zero up to row 256, then the bound samplers' comparisons, the pass's samples per pixel -
// kRasterizerRegister - and the system row, kVenusSystemRow).
uint32_t Replay::constants_offset(bool* ok) {
  *ok = true;
  if (!constants_dirty_) return constants_at_;
  constexpr uint32_t kBlock = kVenusConstantRows * 16;
  const uint32_t align = d_.uniform_alignment();
  const uint32_t at = (ring_used_ + align - 1) / align * align;
  VenusMemory& ring = d_.constants_ring();
  if (uint64_t{at} + kBlock > ring.size) {
    *ok = false;   // (the caller flushes and asks again)
    return 0;
  }
  ByteView block(ring.map + at, kBlock);
  block.fill(0);
  if (!constants_.empty()) (void)block.first(constants_.size()).copy_from(constants_);
  for (uint32_t u = 0; u < kMaxTextureUnits; ++u) {
    float row[4];
    sampler_compare_constant(textures_[u].image ? textures_[u].sampler.compare : CompareOp::Never, textures_[u].image && textures_[u].sampler.filter == Filter::Linear, row);
    (void)block.subview(uint64_t{kSamplerCompareRegister + u} * 16, 16).copy_from(ConstByteView(reinterpret_cast<const uint8_t*>(row), 16));   // (four floats' bytes)
  }
  const uint32_t samples = in_pass_ && targets_[0] ? targets_[0]->desc().samples : 1u;
  const uint32_t raster_row[4] = {samples, 0, 0, 0};
  (void)block.subview(uint64_t{kRasterizerRegister} * 16, 16).copy_from(ConstByteView(reinterpret_cast<const uint8_t*>(raster_row), 16));   // (an unsigned integer's bits)
  // (the system row: the view of view instancing - SV_ViewID - and what is added to the primitives' viewport and layer, as 32-bit unsigned numbers)
  ByteWriter system(block.subview(uint64_t{kVenusSystemRow} * 16, 16));
  system.le32(view_id_), system.le32(view_viewport_), system.le32(view_layer_), system.le32(0);
  constants_at_ = at;
  ring_used_ = at + kBlock;
  constants_dirty_ = false;
  return at;
}

// What the bound textures and storage make of the pipeline's variant.
bool Replay::key_for(VenusPipeline& p, VenusVariantKey* key) {
  *key = VenusVariantKey();
  for (uint32_t u = 0; u < kMaxTextureUnits; ++u)
    if (textures_[u].image) {
      const bool stencil = textures_[u].sampler.stencil && textures_[u].image->desc().stencil;
      key->unit_type[u] = static_cast<uint8_t>(stencil ? tgsi::ValueType::Uint : value_of(textures_[u].image->format()));
      if (textures_[u].sampler.unnormalized) key->unnormalized |= 1u << u;   // (the shaders divide the coordinates by the size)
    }
  const VenusShaderModule* modules[tgsi::kMaxStages] = {};
  const uint32_t n = p.stage_modules(modules);
  for (uint32_t s = 0; s < n; ++s) {
    const TgsiBindings& tb = modules[s]->desc().tgsi_bindings;
    for (uint32_t k = 0; k < tb.texel_count && k < tgsi::kMaxTexelViews; ++k) {
      const uint32_t slot = tb.texels[k];
      if (slot < kMaxImageSlots && texels_[slot].buffer) key->texel_type[s][k] = static_cast<uint8_t>(value_of(texels_[slot].view.format));
    }
    for (uint32_t k = 0; k < tb.image_count && k < tgsi::kMaxImages; ++k) {
      const uint32_t slot = tb.images[k];
      tgsi::ImageFormat f = tgsi::ImageFormat::None;
      if (slot < kMaxStorageImages && images_[slot].image) {
        const Format view = images_[slot].view.format != Format::Undefined ? images_[slot].view.format : images_[slot].image->format();
        f = tgsi_format(view);
      } else if (slot < kMaxImageSlots && texels_[slot].buffer) {
        f = tgsi_format(texels_[slot].view.format);
      }
      key->image_format[s][k] = static_cast<uint8_t>(f);
    }
  }
  if (!p.compute()) {
    for (uint32_t t = 0; t < target_count_; ++t) key->color_format[t] = targets_[t]->vk_format();
    key->depth_format = depth_ ? depth_->vk_format() : 0;
    key->samples = targets_[0]->desc().samples;
    key->topology = static_cast<uint32_t>(topology_);
    key->patch_points = topology_ == PrimitiveTopology::PatchList ? patch_points_ : 0;
    if (custom_locations()) key->flags |= kVariantSampleLocations;
    if (view_viewport_ || view_layer_) key->flags |= kVariantViewOffsets;
  }
  return true;
}

// The variant's descriptor set, written with what is bound, and bound at the pipeline's point.
bool Replay::bind_resources(VenusPipeline& p, VenusPipeline::Variant& v, VkPipelineBindPoint point) {
  bool room = false;
  uint32_t offset = constants_offset(&room);
  if (!room) {
    if (!flush_midway()) return false;
    offset = constants_offset(&room);
    if (!room) return false;
  }
  VkDescriptorSet set = p.next_set(v);
  if (!set) {
    if (!flush_midway()) return false;   // (a set this submit has not used is free again after a flush)
    set = p.next_set(v);
    if (!set) return false;
    offset = constants_offset(&room);
    if (!room) return false;
  }
  note_pipeline(&p);
  static constexpr uint32_t kMaxWrites = 1 + 2 * tgsi::kMaxUnits + tgsi::kMaxStages * (tgsi::kMaxTexelViews + tgsi::kMaxStorage + tgsi::kMaxImages);
  VkWriteDescriptorSet writes[kMaxWrites];
  VkDescriptorImageInfo image_infos[kMaxWrites];
  VkDescriptorBufferInfo buffer_infos[kMaxWrites];
  VkBufferView texel_views[kMaxWrites];
  uint32_t n = 0;
  auto write = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet& {
    VkWriteDescriptorSet& w = writes[n];
    w = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = set, w.dstBinding = binding, w.descriptorCount = 1, w.descriptorType = type;
    w.pImageInfo = &image_infos[n], w.pBufferInfo = &buffer_infos[n], w.pTexelBufferView = &texel_views[n];
    image_infos[n] = {}, buffer_infos[n] = {}, texel_views[n] = nullptr;
    ++n;
    return w;
  };
  const bool null_ok = d_.context().caps().null_descriptor;
  {
    write(tgsi::kBindingConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    buffer_infos[n - 1] = {d_.constants_ring().buffer, 0, kVenusConstantRows * 16};
  }
  // texture units: draws bind units 0 .. texture_count - 1 and every one must be bound; a dispatch the units up to the highest bound (the others null)
  for (uint32_t u = 0; u < tgsi::kMaxUnits; ++u) {
    const bool wants = (v.unit_mask >> u & 1) || (v.sampler_mask >> u & 1);
    if (!wants) continue;
    BoundTexture& t = textures_[u];
    if (!t.image) {
      if (!p.compute() || !null_ok) return false;
      if (v.unit_mask >> u & 1) write(tgsi::kBindingTexture + u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      if (v.sampler_mask >> u & 1) {
        write(tgsi::kBindingSampler + u, VK_DESCRIPTOR_TYPE_SAMPLER);
        image_infos[n - 1].sampler = d_.sampler_for(SamplerDesc(), false);
      }
      continue;
    }
    // (a view of levels and layers the pass draws into is refused; other levels or layers of a target may be read, as on virgl)
    if (in_pass_ && !p.compute()) {
      const ImageDesc& id = t.image->desc();
      const uint32_t first_level = t.has_view ? t.view.base_level : 0, levels = t.has_view ? t.view.level_count : id.mip_levels;
      const uint32_t first_layer = t.has_view ? t.view.first_layer : 0, layers = t.has_view ? t.view.layer_count : t.image->layer_count();
      auto drawn_into = [&](uint32_t level, uint32_t first, uint32_t count) {
        if (level < first_level || level >= first_level + levels) return false;
        if (id.kind == TextureKind::Texture3D) return true;   // (a volume's slices are one subresource of its level)
        return first < first_layer + layers && first_layer < first + count;
      };
      for (uint32_t k = 0; k < target_count_; ++k)
        if (targets_[k] == t.image && drawn_into(pass_.level[k], pass_.first_layer[k], pass_.layer_count[k])) return false;
      if (depth_ == t.image && drawn_into(pass_.level[kMaxColorTargets], pass_.first_layer[kMaxColorTargets], pass_.layer_count[kMaxColorTargets])) return false;
    }
    const bool stencil = t.sampler.stencil && t.image->desc().stencil;
    VkImageView view = t.has_view ? t.image->sampled_view(t.view, stencil) : t.image->whole_view(stencil);
    if (!view) return false;
    if (v.unit_mask >> u & 1) {
      write(tgsi::kBindingTexture + u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      image_infos[n - 1].imageView = view, image_infos[n - 1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (v.sampler_mask >> u & 1) {
      const bool integer = stencil || is_integer_format(t.image->format());
      VkSampler s = d_.sampler_for(t.sampler, integer);
      if (!s) return false;
      write(tgsi::kBindingSampler + u, VK_DESCRIPTOR_TYPE_SAMPLER);
      image_infos[n - 1].sampler = s;
    }
  }
  // each stage's storage, through its module's numbering
  for (uint32_t s = 0; s < v.stage_count; ++s) {
    const TgsiBindings& tb = v.stage_module[s]->desc().tgsi_bindings;
    for (uint32_t k = 0; k < tgsi::kMaxTexelViews; ++k) {
      if (!(v.texel_mask[s] >> k & 1)) continue;
      write(tgsi::binding_texel(s, k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
      const uint32_t slot = k < tb.texel_count ? tb.texels[k] : kMaxImageSlots;
      if (slot < kMaxImageSlots && texels_[slot].buffer) {
        const BoundTexel& t = texels_[slot];
        texel_views[n - 1] = buffer_view(t.buffer, t.view.offset, uint64_t{t.view.elements} * texel_bytes(t.view.format), texel_vk_format(t.view.format));
        if (!texel_views[n - 1]) return false;
      } else if (!null_ok) {
        return false;
      }
    }
    for (uint32_t k = 0; k < tgsi::kMaxStorage; ++k) {
      if (!(v.storage_mask[s] >> k & 1)) continue;
      write(tgsi::binding_storage(s, k), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
      const uint32_t slot = k < tb.storage_count ? tb.storage[k] : kMaxStorageBuffers;
      if (slot < kMaxStorageBuffers && storage_[slot].buffer && storage_[slot].size) {
        buffer_infos[n - 1] = {storage_[slot].buffer->buffer(), storage_[slot].offset, storage_[slot].size};
      } else if (!null_ok) {
        return false;
      } else {
        buffer_infos[n - 1] = {nullptr, 0, VK_WHOLE_SIZE};   // (an unbound slot reads 0 and drops writes)
      }
    }
    for (uint32_t k = 0; k < tgsi::kMaxImages; ++k) {
      if (!(v.image_mask[s] >> k & 1)) continue;
      const bool buffer = (v.image_buffer_mask[s] >> k & 1) != 0;
      write(tgsi::binding_image(s, k), buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
      const uint32_t slot = k < tb.image_count ? tb.images[k] : kMaxImageSlots;
      if (buffer) {
        if (slot < kMaxImageSlots && texels_[slot].buffer) {
          const BoundTexel& t = texels_[slot];
          texel_views[n - 1] = buffer_view(t.buffer, t.view.offset, uint64_t{t.view.elements} * texel_bytes(t.view.format), texel_vk_format(t.view.format));
          if (!texel_views[n - 1]) return false;
        } else if (!null_ok) {
          return false;
        }
      } else if (slot < kMaxStorageImages && images_[slot].image) {
        BoundImage& b = images_[slot];
        const Format vf = b.view.format != Format::Undefined ? b.view.format : b.image->format();
        bool swap = false;
        VkImageView view = b.image->storage_view(b.view.level, b.view.first_layer, b.view.layer_count, d_.vk_format(vf, true, &swap));
        if (!view) return false;
        image_infos[n - 1].imageView = view, image_infos[n - 1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      } else if (!null_ok) {
        return false;
      }
    }
  }
  vk_.UpdateDescriptorSets(d_.device(), n, writes, 0, nullptr);
  vk_.CmdBindDescriptorSets(cb_, point, v.layout, 0, 1, &set, 1, &offset);
  return true;
}

void Replay::set_viewports() {
  const uint32_t count = d_.context().caps().multi_viewport ? kMaxViewports : 1;
  VkViewport vps[kMaxViewports];
  VkRect2D scs[kMaxViewports];
  for (uint32_t i = 0; i < count; ++i) {
    // (the viewports and scissors past those set repeat the first, as on virgl; the scissors are clamped to the pass)
    const Viewport& v = viewports_[i < viewport_count_ ? i : 0];
    vps[i] = {v.x, v.y, v.width > 0 ? v.width : 1e-3f, v.height > 0 ? v.height : 1e-3f, v.min_depth, v.max_depth};
    if (!(v.width > 0) || !(v.height > 0)) vps[i].width = vps[i].height = 1e-3f;
    const Rect2D& r = scissors_[i < scissor_count_ && i < viewport_count_ ? i : 0];
    int64_t x0 = r.x, y0 = r.y, x1 = int64_t{r.x} + r.width, y1 = int64_t{r.y} + r.height;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > extent_.width ? extent_.width : x1, y1 = y1 > extent_.height ? extent_.height : y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    scs[i] = {{static_cast<int32_t>(x0), static_cast<int32_t>(y0)}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
  }
  vk_.CmdSetViewport(cb_, 0, count, vps);
  vk_.CmdSetScissor(cb_, 0, count, scs);
}

bool Replay::draw(const RecordedCommand& c, bool indexed) {
  if (!in_pass_ || !pipeline_) return false;
  VenusPipeline& p = *pipeline_;
  const GraphicsPipelineDesc& desc = p.desc();
  if (desc.color_target_count > target_count_) return false;
  if (indexed && !index_) return false;
  for (uint32_t u = 0; u < desc.texture_count; ++u)
    if (!textures_[u].image) return false;
  VenusVariantKey key;
  if (!key_for(p, &key)) return false;
  VenusPipeline::Variant* v = p.variant(key);
  if (!v) return false;
  if (bound_pipeline_ != v->pipeline) {
    vk_.CmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
    bound_pipeline_ = v->pipeline;
  }
  set_viewports();
  if (desc.stencil.enable) {
    vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_BIT, desc.stencil.reference);
    vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_BACK_BIT, desc.separate_back_stencil ? desc.back_stencil.reference : desc.stencil.reference);
  } else {
    vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  }
  const uint32_t flushes = flushes_;
  if (!bind_resources(p, *v, VK_PIPELINE_BIND_POINT_GRAPHICS)) return false;
  if (flushes != flushes_ && !bind_resources(p, *v, VK_PIPELINE_BIND_POINT_GRAPHICS)) return false;   // (a flush started a new command buffer: everything bound again)
  if (bound_pipeline_ != v->pipeline) {
    vk_.CmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
    bound_pipeline_ = v->pipeline;
    set_viewports();
    vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_BIT, desc.stencil.reference);
    vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_BACK_BIT, desc.separate_back_stencil ? desc.back_stencil.reference : desc.stencil.reference);
  }
  for (uint32_t b = 0; b < p.binding_count(); ++b) {
    const VertexBinding& vb = vertex_[p.binding_slot(b)];
    if (!vb.buffer && !d_.context().caps().null_descriptor) return false;   // (no buffer: the null descriptor, kFeature3NullVertexBuffers)
    const VkBuffer buffer = vb.buffer ? vb.buffer->buffer() : VK_NULL_HANDLE;
    const VkDeviceSize offset = vb.buffer ? vb.offset & 0xFFFFFFFFu : 0;
    vk_.CmdBindVertexBuffers(cb_, b, 1, &buffer, &offset);
  }
  if (depth_bounds_dirty_) set_depth_bounds_state();
  if (key.flags & kVariantSampleLocations) set_sample_locations();
  set_fragment_shading_rate();
  if (desc.stream_output_count && !begin_stream_output(p)) return false;
  // (a fragment shader that stands in for no pixel shader: Vulkan counts its runs, Direct3D none - the draw gets a statistics segment of its own whose PsInvocations
  // are left out)
  const bool stand_in = desc.stand_in_fragment && current_[kCatStatistics] >= 0;
  if (stand_in) segment_close(kCatStatistics), segment_open(kCatStatistics, true);
  if (indexed) {
    vk_.CmdBindIndexBuffer(cb_, index_->buffer(), index_offset_, index_type_ == IndexType::Uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vk_.CmdDrawIndexed(cb_, c.count, c.instance_count, c.first, c.vertex_offset, c.first_instance);
  } else {
    vk_.CmdDraw(cb_, c.count, c.instance_count, c.first, c.first_instance);
  }
  if (stand_in) segment_close(kCatStatistics), segment_open(kCatStatistics, false);
  end_stream_output();
  // (what a storing draw wrote is there for the next draw: the rendering is ended for a barrier and resumed)
  if (desc.fragment_stores || desc.vertex_stores || desc.geometry_stores || desc.tessellation_stores) {
    segments_close();
    vk_.CmdEndRendering(cb_);
    rendering_ = false;
    barrier();
    resume_rendering();
  }
  return true;
}

bool Replay::dispatch(const RecordedCommand& c) {
  if (in_pass_ || !compute_) return false;
  VenusVariantKey key;
  if (!key_for(*compute_, &key)) return false;
  VenusPipeline::Variant* v = compute_->variant(key);
  if (!v) return false;
  const uint32_t flushes = flushes_;
  if (!bind_resources(*compute_, *v, VK_PIPELINE_BIND_POINT_COMPUTE)) return false;
  if (flushes != flushes_ && !bind_resources(*compute_, *v, VK_PIPELINE_BIND_POINT_COMPUTE)) return false;   // (a flush started a new command buffer: bound again)
  vk_.CmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipeline);
  vk_.CmdDispatch(cb_, c.count, c.first, c.instance_count);
  barrier();   // (what the dispatch wrote is there for everything after it)
  return true;
}

// The work so far submitted and waited for, and recording goes on in a fresh command buffer (the constant ring and descriptor sets are free again).
bool Replay::flush_midway() {
  const bool resume = rendering_;
  end_stream_output();
  segments_close();
  if (rendering_) vk_.CmdEndRendering(cb_);
  rendering_ = false;
  if (!d_.finish_main(cb_)) return false;
  release();
  ++flushes_;
  ring_used_ = 0;
  constants_dirty_ = true;
  cb_ = d_.begin_main();
  if (!cb_) return false;
  bound_pipeline_ = nullptr;
  depth_bounds_dirty_ = true;
  if (resume) resume_rendering();
  else segments_open();
  return true;
}

// ---- queries --------------------------------------------------------------------------------------------------------------------------------------
void Replay::segments_close() {
  for (uint32_t cat = 0; cat < kCategories; ++cat) segment_close(cat);
}

void Replay::segment_close(uint32_t cat) {
  if (current_[cat] < 0) return;
  const Segment& seg = segments_[static_cast<uint64_t>(current_[cat])];
  if (cat == kCatOcclusion) vk_.CmdEndQuery(cb_, d_.query_pool(VenusDevice::kPoolOcclusion), seg.index);
  else if (cat == kCatStatistics) vk_.CmdEndQuery(cb_, d_.query_pool(VenusDevice::kPoolStatistics), seg.index);
  else vk_.CmdEndQueryIndexedEXT(cb_, d_.query_pool(VenusDevice::kPoolStreams), seg.index, cat - kCatStream0);
  current_[cat] = -1;
}

void Replay::segments_open() {
  for (uint32_t cat = 0; cat < kCategories; ++cat) segment_open(cat, false);
}

void Replay::segment_open(uint32_t cat, bool no_fragment) {
  Segment seg;
  seg.category = cat;
  seg.no_fragment = no_fragment;
  bool any = false;
  for (uint32_t q = 0; q < kMaxOcclusionQueries; ++q)
    if ((open_[q / 64] >> (q % 64) & 1) && category_of(queries_[q].kind, queries_[q].stream) == cat) seg.open[q / 64] |= uint64_t{1} << (q % 64), any = true;
  if (!any) return;
  const VenusDevice::QueryPoolKind kind = cat == kCatOcclusion ? VenusDevice::kPoolOcclusion : cat == kCatStatistics ? VenusDevice::kPoolStatistics : VenusDevice::kPoolStreams;
  VkQueryPool pool = d_.query_pool(kind);
  if (!pool || used_[kind] >= VenusDevice::kQuerySegments) return;   // (refused at the query's begin; or out of segments: the rest is not counted)
  seg.index = used_[kind]++;
  if (!segments_.push_back(seg)) return;
  current_[cat] = static_cast<int32_t>(segments_.count() - 1);
  if (cat == kCatStream0 + 0 || cat > kCatStream0) vk_.CmdBeginQueryIndexedEXT(cb_, pool, seg.index, 0, cat - kCatStream0);
  else vk_.CmdBeginQuery(cb_, pool, seg.index, cat == kCatOcclusion && d_.context().caps().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}

bool Replay::begin_query(const RecordedCommand& c) {
  const uint32_t slot = c.unit;
  const QueryKind kind = static_cast<QueryKind>(c.count & 0xFF);
  const uint32_t stream = (c.count >> 8) & 3;
  if (slot >= kMaxOcclusionQueries || static_cast<uint32_t>(kind) > static_cast<uint32_t>(QueryKind::PipelineStatistics) || queries_[slot].begun) return false;
  UserQuery& q = queries_[slot];
  q.begun = true, q.kind = kind, q.stream = stream;
  any_queries_ = true;
  switch (kind) {
    case QueryKind::TimestampFrequency: q.synthetic = 1000000000ull; return true;
    case QueryKind::TimestampDisjoint: q.synthetic = d_.clock_interrupted() ? 1 : 0; return true;
    case QueryKind::Timestamp: return true;
    case QueryKind::Occlusion: case QueryKind::OcclusionPredicate: break;
    case QueryKind::SoPrimitivesWritten: case QueryKind::SoPrimitivesNeeded:
      if (!d_.query_pool(VenusDevice::kPoolStreams)) return false;
      break;
    default:
      if (!d_.query_pool(VenusDevice::kPoolStatistics)) return false;
      break;
  }
  segments_close();
  open_[slot / 64] |= uint64_t{1} << (slot % 64);
  order_[order_count_++] = static_cast<int32_t>(slot);
  segments_open();
  return true;
}

bool Replay::end_query(const RecordedCommand& c) {
  int32_t slot = -1;
  if (c.count == 1) {
    slot = c.unit < kMaxOcclusionQueries ? static_cast<int32_t>(c.unit) : -1;
  } else {   // (the newest query still open)
    for (uint32_t i = order_count_; i > 0 && slot < 0; --i)
      if (!queries_[order_[i - 1]].ended) slot = order_[i - 1];
  }
  if (slot < 0 || !queries_[slot].begun || queries_[slot].ended) return false;
  UserQuery& q = queries_[slot];
  q.ended = true;
  if (q.kind == QueryKind::Timestamp) {
    VkQueryPool pool = d_.query_pool(VenusDevice::kPoolTimestamps);
    if (!pool || used_[VenusDevice::kPoolTimestamps] >= VenusDevice::kTimestamps) return false;
    q.timestamp = used_[VenusDevice::kPoolTimestamps]++;
    vk_.CmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, q.timestamp);
    return true;
  }
  if (category_of(q.kind, q.stream) == ~0u) return true;
  segments_close();
  open_[slot / 64] &= ~(uint64_t{1} << (slot % 64));
  segments_open();
  return true;
}

// After the fence: every segment's counts, added up into the queries that were open while it ran.
bool Replay::read_queries() {
  if (!any_queries_) return true;
  const uint32_t strides[VenusDevice::kPoolKinds] = {1, kPipelineStatisticCount, 2, 1};
  List<uint64_t> results[VenusDevice::kPoolKinds] = {List<uint64_t>(d_.allocator()), List<uint64_t>(d_.allocator()), List<uint64_t>(d_.allocator()), List<uint64_t>(d_.allocator())};
  for (uint32_t k = 0; k < VenusDevice::kPoolKinds; ++k) {
    if (!used_[k]) continue;
    const uint64_t words = uint64_t{used_[k]} * strides[k];
    for (uint64_t i = 0; i < words; ++i)
      if (!results[k].push_back(0)) return false;
    if (vk_.GetQueryPoolResults(d_.device(), d_.query_pool(static_cast<VenusDevice::QueryPoolKind>(k)), 0, used_[k], words * 8, results[k].view().data(), strides[k] * 8,
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
      return false;
  }
  const double period = d_.context().caps().timestamp_period;
  const uint32_t bits = d_.context().caps().timestamp_valid_bits;
  for (uint32_t slot = 0; slot < kMaxOcclusionQueries; ++slot) {
    const UserQuery& q = queries_[slot];
    if (!q.begun) continue;
    uint64_t values[kMaxQueryValues] = {};
    if (q.kind == QueryKind::TimestampFrequency || q.kind == QueryKind::TimestampDisjoint) {
      values[0] = q.synthetic;
    } else if (q.kind == QueryKind::Timestamp) {
      uint64_t ticks = results[VenusDevice::kPoolTimestamps][q.timestamp];
      if (bits && bits < 64) ticks &= (uint64_t{1} << bits) - 1;
      values[0] = static_cast<uint64_t>(static_cast<double>(ticks) * period);   // (nanoseconds)
    } else {
      const uint32_t cat = category_of(q.kind, q.stream);
      for (const Segment& seg : segments_) {
        if (seg.category != cat || !(seg.open[slot / 64] >> (slot % 64) & 1)) continue;
        if (cat == kCatOcclusion) {
          values[0] += results[VenusDevice::kPoolOcclusion][seg.index];
        } else if (cat == kCatStatistics) {
          for (uint32_t k = 0; k < kPipelineStatisticCount; ++k)
            if (!(seg.no_fragment && k == static_cast<uint32_t>(PipelineStatistic::PsInvocations)))
              values[k] += results[VenusDevice::kPoolStatistics][uint64_t{seg.index} * kPipelineStatisticCount + k];
        } else {
          values[0] += results[VenusDevice::kPoolStreams][uint64_t{seg.index} * 2 + (q.kind == QueryKind::SoPrimitivesWritten ? 0 : 1)];
        }
      }
      switch (q.kind) {
        case QueryKind::OcclusionPredicate: values[0] = values[0] ? 1 : 0; break;
        case QueryKind::IaVertices: values[0] = values[static_cast<uint32_t>(PipelineStatistic::IaVertices)]; break;
        case QueryKind::IaPrimitives: values[0] = values[static_cast<uint32_t>(PipelineStatistic::IaPrimitives)]; break;
        case QueryKind::VsInvocations: values[0] = values[static_cast<uint32_t>(PipelineStatistic::VsInvocations)]; break;
        case QueryKind::PsInvocations: values[0] = values[static_cast<uint32_t>(PipelineStatistic::PsInvocations)]; break;
        default: break;
      }
      if (q.kind != QueryKind::PipelineStatistics)
        for (uint32_t k = 1; k < kMaxQueryValues; ++k) values[k] = 0;
    }
    rec_.set_query_values(slot, ArrayView<const uint64_t>(values));
  }
  return true;
}

// ---- resolves ------------------------------------------------------------------------------------------------------------------------------------------
// VirglDevice's rules: outside a pass, a multisampled source and a single-sampled destination of one extent; colour to the average (or, with the resolve modes, the
// least or greatest sample), depth (and stencil) from sample 0 only. Queries do not count what a resolve does.
bool Replay::resolve(const RecordedCommand& c) {
  VenusImage* dst = static_cast<VenusImage*>(c.image);
  VenusImage* src = static_cast<VenusImage*>(c.image2);
  const ResolveMode mode = static_cast<ResolveMode>(c.count);
  const uint32_t layer = c.first;   // (the source's layer: resolve_layer)
  if (in_pass_ || !dst || !src || dst->desc().samples != 1 || src->desc().samples < 2 || dst->extent().width != src->extent().width ||
      dst->extent().height != src->extent().height || layer >= src->layer_count())
    return false;
  const bool depth = src->format() == Format::D32Float;
  if (depth != (dst->format() == Format::D32Float)) return false;
  if (depth ? mode != ResolveMode::Sample0 : mode == ResolveMode::Sample0) return false;
  const Extent2D e = dst->extent();
  segments_close();
  bool ok = true;
  if (depth) {
    // (a rendering with no draws whose end resolves the depth - and the stencil, when both have it - from sample 0)
    if (!(dst->desc().usage & kImageDepthAttachment)) ok = false;
    VkRenderingAttachmentInfo dz = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    if (ok) {
      dz.imageView = src->target_view(0, layer, 1);
      dz.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      dz.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      dz.resolveImageView = dst->target_view(0, 0, 1);
      dz.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
      dz.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, dz.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      ok = dz.imageView && dz.resolveImageView;
    }
    if (ok) {
      VkRenderingAttachmentInfo st = dz;
      const bool stencil = src->desc().stencil && dst->desc().stencil;
      if (!stencil) st.resolveMode = VK_RESOLVE_MODE_NONE, st.resolveImageView = nullptr;
      VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
      ri.renderArea = {{0, 0}, {e.width, e.height}};
      ri.layerCount = 1;
      ri.pDepthAttachment = &dz;
      ri.pStencilAttachment = src->desc().stencil ? &st : nullptr;
      if (src->desc().stencil && !dst->desc().stencil) ok = false;   // (a stencil source into a depth-only destination: the formats of a rendering's resolve must match)
      if (ok) {
        vk_.CmdBeginRendering(cb_, &ri);
        vk_.CmdEndRendering(cb_);
      }
    }
  } else if (mode == ResolveMode::Average) {
    VkImageResolve r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {e.width, e.height, 1};
    if (src->vk_format() == dst->vk_format()) {
      vk_.CmdResolveImage(cb_, src->image(), VK_IMAGE_LAYOUT_GENERAL, dst->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
      // (resolved into an image of the source's format, then blitted into the destination's: the blit converts, as GL's resolve blit does)
      VenusImage* scratch = d_.resolve_scratch(src->format(), e);
      if (!scratch) ok = false;
      if (ok) {
        vk_.CmdResolveImage(cb_, src->image(), VK_IMAGE_LAYOUT_GENERAL, scratch->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        barrier();
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = b.dstOffsets[1] = {static_cast<int32_t>(e.width), static_cast<int32_t>(e.height), 1};
        vk_.CmdBlitImage(cb_, scratch->image(), VK_IMAGE_LAYOUT_GENERAL, dst->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
      }
    }
  } else {
    // the least or greatest sample: a full-screen draw into the destination (of layer 0: the draw's texture is the whole image)
    VenusPipeline* rp = (dst->desc().usage & kImageColorAttachment) && layer == 0 ? d_.extreme_resolve_pipeline(dst->vk_format(), src->desc().samples, mode == ResolveMode::Max) : nullptr;
    VenusVariantKey key;
    key.unit_type[0] = static_cast<uint8_t>(value_of(src->format()));
    key.color_format[0] = dst->vk_format();
    key.topology = static_cast<uint32_t>(PrimitiveTopology::TriangleList);
    VenusPipeline::Variant* v = rp ? rp->variant(key) : nullptr;
    VkImageView target = v ? dst->target_view(0, 0, 1) : nullptr;
    ok = target != nullptr;
    if (ok) {
      VkRenderingAttachmentInfo color = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
      color.imageView = target, color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
      ri.renderArea = {{0, 0}, {e.width, e.height}};
      ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &color;
      const BoundTexture saved = textures_[0];
      textures_[0] = BoundTexture();
      textures_[0].image = src;
      const ConstByteView saved_constants = constants_;
      constants_ = ConstByteView();
      constants_dirty_ = true;
      const uint32_t flushes = flushes_;
      ok = bind_resources(*rp, *v, VK_PIPELINE_BIND_POINT_GRAPHICS);
      if (ok && flushes != flushes_) ok = bind_resources(*rp, *v, VK_PIPELINE_BIND_POINT_GRAPHICS);
      if (ok) {
        vk_.CmdBeginRendering(cb_, &ri);
        vk_.CmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
        const VkViewport vp = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        const VkRect2D sc = {{0, 0}, {e.width, e.height}};
        const uint32_t count = d_.context().caps().multi_viewport ? kMaxViewports : 1;
        VkViewport vps[kMaxViewports];
        VkRect2D scs[kMaxViewports];
        for (uint32_t i = 0; i < count; ++i) vps[i] = vp, scs[i] = sc;
        vk_.CmdSetViewport(cb_, 0, count, vps);
        vk_.CmdSetScissor(cb_, 0, count, scs);
        vk_.CmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        if (features2_ & kFeature2ShadingRate) {   // (every pixel shaded: the draws set their own rate again)
          const VkExtent2D one = {1, 1};
          const VkFragmentShadingRateCombinerOpKHR keep[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
          vk_.CmdSetFragmentShadingRateKHR(cb_, &one, keep);
        }
        if (d_.context().caps().depth_bounds) {   // (the pipeline's dynamic state: the test off for the resolve, the user's set again before the next draw)
          vk_.CmdSetDepthBoundsTestEnable(cb_, VK_FALSE);
          vk_.CmdSetDepthBounds(cb_, 0.0f, 1.0f);
          depth_bounds_dirty_ = true;
        }
        vk_.CmdDraw(cb_, 3, 1, 0, 0);
        vk_.CmdEndRendering(cb_);
      }
      textures_[0] = saved;
      constants_ = saved_constants;
      constants_dirty_ = true;
      bound_pipeline_ = nullptr;
    }
  }
  barrier();
  segments_open();
  return ok;
}

// ---- the depth bounds test and copies of samples ---------------------------------------------------------------------------------------------------------
void Replay::set_depth_bounds_state() {
  depth_bounds_dirty_ = false;
  if (!d_.context().caps().depth_bounds) return;
  // (Vulkan tests the bounds only where there is a depth buffer, as Direct3D does)
  vk_.CmdSetDepthBoundsTestEnable(cb_, depth_bounds_ ? VK_TRUE : VK_FALSE);
  vk_.CmdSetDepthBounds(cb_, depth_min_, depth_max_);
}

// A region of a multisampled image, every sample of each pixel (a depth image's depth and stencil), to (x, y) of another of the same format and samples, outside a
// pass (kFeature2SampleCopies: Direct3D 12's D3D12_RESOLVE_MODE_DECOMPRESS).
bool Replay::copy_samples(const RecordedCommand& c) {
  VenusImage* dst = static_cast<VenusImage*>(c.image);
  VenusImage* src = static_cast<VenusImage*>(c.image2);
  if (in_pass_ || !dst || !src || dst == src || dst->format() != src->format() || dst->desc().samples != src->desc().samples || src->desc().samples < 2 ||
      dst->desc().stencil != src->desc().stencil)
    return false;
  const Rect2D& r = c.scissor;
  const Extent2D se = src->extent(), de = dst->extent();
  if (r.x < 0 || r.y < 0 || r.width == 0 || r.height == 0 || uint64_t{static_cast<uint32_t>(r.x)} + r.width > se.width ||
      uint64_t{static_cast<uint32_t>(r.y)} + r.height > se.height || uint64_t{c.count} + r.width > de.width || uint64_t{c.first} + r.height > de.height)
    return false;
  barrier();
  VkImageCopy region{};
  region.srcSubresource = {src->aspect(), 0, 0, 1};
  region.srcOffset = {r.x, r.y, 0};
  region.dstSubresource = {dst->aspect(), 0, 0, 1};
  region.dstOffset = {static_cast<int32_t>(c.count), static_cast<int32_t>(c.first), 0};
  region.extent = {r.width, r.height, 1};
  vk_.CmdCopyImage(cb_, src->image(), VK_IMAGE_LAYOUT_GENERAL, dst->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  barrier();
  return true;
}

// ---- stream output -------------------------------------------------------------------------------------------------------------------------------
// VirglDevice's rules: an append starts where the buffer's last stream output ended, which is not known for a buffer that is a target earlier in the same submit
// (refused) or whose end is not known (refused); a slot bound again with the same buffer and an append goes on.
bool Replay::bind_stream_output(const RecordedCommand& c) {
  if (c.unit >= kMaxStreamOutputBuffers) return false;
  end_stream_output();
  Target& t = so_[c.unit];
  VenusBuffer* b = static_cast<VenusBuffer*>(c.buffer);
  if (!b) {
    t = Target();
    return true;
  }
  if (!d_.context().caps().transform_feedback) return false;
  if (c.offset == kStreamOutputAppend && t.buffer == b && t.end == c.end) return true;   // (it goes on)
  uint64_t start = c.offset;
  if (c.offset == kStreamOutputAppend) {
    for (uint32_t i = 0; i < fill_count_; ++i)
      if (fills_[i].buffer == b) return false;
    start = b->stream_output_filled();
    if (start == kStreamOutputAppend) return false;
  }
  const uint64_t limit = c.end < b->size() ? c.end : b->size();
  uint32_t slot = 0;
  if (start > b->size() || start > 0xFFFFFFFFu || fill_count_ >= kMaxFills || !new_slot(&slot)) return false;
  VenusMemory* counters = d_.stream_output_counters();
  if (!counters) return false;
  ArrayView<uint32_t> words;
  if (!ByteView(counters->map, counters->size).try_cast<uint32_t>(&words)) return false;
  words[slot] = static_cast<uint32_t>(start);   // (Vulkan's counter is a byte offset into the binding, which starts at the buffer's start)
  t.buffer = b;
  t.counter = slot;
  t.fill = fill_count_;
  t.end = c.end;
  fills_[fill_count_++] = {b, slot, limit};
  return true;
}

namespace {
// The vertices of one primitive the pipeline's stream output writes: its geometry shader's output primitive (points, line strips, triangle strips).
uint32_t output_vertices(const VenusPipeline& p) {
  const VenusShaderModule* gs = static_cast<const VenusShaderModule*>(p.desc().geometry_shader);
  if (!gs) return 0;
  const uint32_t out = gs->program().get(tgsi::Property::GsOutputPrimitive);
  return out == 0 ? 1u : out == 1 || out == 3 ? 2u : 3u;
}
}  // namespace

bool Replay::begin_stream_output(VenusPipeline& p) {
  if (!rendering_) return false;
  VenusMemory* counters = d_.stream_output_counters();
  if (!counters) return false;
  VkBuffer buffers[kMaxStreamOutputBuffers], counter_buffers[kMaxStreamOutputBuffers];
  VkDeviceSize offsets[kMaxStreamOutputBuffers], sizes[kMaxStreamOutputBuffers], counter_offsets[kMaxStreamOutputBuffers];
  uint32_t used = 0;   // (the slots up to the last bound one; an unbound slot below it is bound to the counters' buffer with no room)
  for (uint32_t s = 0; s < kMaxStreamOutputBuffers; ++s)
    if (so_[s].buffer) used = s + 1;
  if (!used) return true;   // (nothing bound: nothing is written)
  const uint32_t vertices = output_vertices(p);
  for (uint32_t s = 0; s < used; ++s) {
    if (so_[s].buffer) {
      // (the binding is the buffer up to the bound end and one byte more: records are whole words, so none ends in that byte, and a host that takes a record
      // ending exactly at the binding's end for one that does not fit - RADV seems to - writes it; the buffer has the room, VenusDevice::create_buffer)
      const Fill& f = fills_[so_[s].fill];
      uint32_t write = 0;
      if (!new_slot(&write)) return false;
      if (!so_draws_.push_back({so_[s].fill, so_[s].counter, write, vertices * p.desc().stream_output_stride[s]})) return false;
      buffers[s] = so_[s].buffer->buffer(), offsets[s] = 0, sizes[s] = f.limit < so_[s].buffer->room() ? f.limit + 1 : f.limit;
      if (sizes[s] == 0) return false;   // (Vulkan takes no empty binding)
      counter_buffers[s] = counters->buffer, counter_offsets[s] = uint64_t{so_[s].counter} * 4;
      so_counter_offsets_[s] = uint64_t{write} * 4;
      so_[s].counter = write;
    } else {
      buffers[s] = counters->buffer, offsets[s] = counters->size - 4, sizes[s] = 4;
      counter_buffers[s] = nullptr, counter_offsets[s] = 0;
      so_counter_offsets_[s] = 0;
    }
    so_counter_buffers_[s] = counter_buffers[s];
  }
  vk_.CmdBindTransformFeedbackBuffersEXT(cb_, 0, used, buffers, offsets, sizes);
  vk_.CmdBeginTransformFeedbackEXT(cb_, 0, used, counter_buffers, counter_offsets);   // (from the slots with each binding's start ...)
  so_active_ = true;
  so_used_ = used;
  return true;
}

void Replay::end_stream_output() {
  if (!so_active_) return;
  vk_.CmdEndTransformFeedbackEXT(cb_, 0, so_used_, so_counter_buffers_, so_counter_offsets_);   // (... into new ones)
  so_active_ = false;
}

// After the fence: where each buffer's last binding got to, Direct3D's way - after the last whole primitive written, which on a host that moved the counter to the
// binding's end when a primitive did not fit is the draw's start and the primitives that fitted there (the host writes them in order until one does not).
void Replay::stream_output_results() {
  VenusMemory* counters = fill_count_ ? d_.stream_output_counters() : nullptr;
  if (!counters) return;
  ArrayView<uint32_t> words;
  if (!ByteView(counters->map, counters->size).try_cast<uint32_t>(&words)) return;
  for (uint32_t i = 0; i < fill_count_; ++i) {
    bool later = false;
    for (uint32_t j = i + 1; j < fill_count_; ++j) later = later || fills_[j].buffer == fills_[i].buffer;
    if (later) continue;
    const uint64_t limit = fills_[i].limit;
    uint64_t at = words[fills_[i].start_slot];
    bool full = false;
    for (const SoDraw& d : so_draws_) {
      if (d.fill != i || full) continue;
      const uint64_t got = words[d.write];
      if (got <= limit && got % 4 == 0 && got >= at) {
        at = got;
      } else {   // (a primitive did not fit: nothing more is written to this binding, by the host or by Direct3D)
        full = true;
        if (d.primitive_bytes && at <= limit) at += (limit - at) / d.primitive_bytes * d.primitive_bytes;
      }
    }
    fills_[i].buffer->set_stream_output_filled(at <= fills_[i].buffer->size() ? at : kStreamOutputAppend);
  }
}

// ---- gap pass 5: sample positions, coarse shading ------------------------------------------------------------------------------------------------------
// The positions (kFeature2SamplePositions): `count` samples of `first` pixels (1, or 4 for a 2 x 2 quad), as x, y bytes in the recorder's constants.
bool Replay::set_sample_positions(const RecordedCommand& c, ConstByteView all_constants) {
  if (!(features2_ & kFeature2SamplePositions)) return false;
  const uint32_t samples = c.count, pixels = c.first;
  if (pixels == 0) {
    positions_samples_ = positions_pixels_ = 0;
    return true;
  }
  if ((pixels != 1 && pixels != 4) || (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) || uint64_t{samples} * pixels > kMaxSamplePositions) return false;
  ConstByteView bytes;
  if (c.constants_size != samples * pixels * 2 || !all_constants.try_subview(c.constants_offset, c.constants_size, &bytes)) return false;
  for (uint32_t i = 0; i < samples * pixels; ++i) {
    positions_[i].x = static_cast<int8_t>(bytes[uint64_t{i} * 2]);
    positions_[i].y = static_cast<int8_t>(bytes[uint64_t{i} * 2 + 1]);
    if (positions_[i].x < -8 || positions_[i].x > 7 || positions_[i].y < -8 || positions_[i].y > 7) return false;
  }
  positions_samples_ = samples, positions_pixels_ = pixels;
  return true;
}

// Whether the draws of the pass take the positions set: those of the pass's sample count, for a pipeline that multisamples (one that does not puts every sample at
// the pixel's centre) or a pass of one sample.
bool Replay::custom_locations() const {
  if (!positions_pixels_ || !in_pass_ || !pipeline_ || target_count_ == 0) return false;
  const uint32_t samples = targets_[0]->desc().samples;
  return positions_samples_ == samples && (samples == 1 || pipeline_->desc().multisample);
}

void Replay::set_sample_locations() {
  // (Direct3D's sixteenths of a pixel from its centre, -8 .. 7, +y down, are Vulkan's 0 .. 15/16 from its top left corner; a quad's patterns are a 2 x 2 grid in
  // Direct3D's order - upper left, upper right, lower left, lower right -, which is Vulkan's: pixel (x, y) of the grid at (x + y * 2) * samples)
  VkSampleLocationEXT at[kMaxSamplePositions];
  const uint32_t n = positions_samples_ * positions_pixels_;
  for (uint32_t i = 0; i < n && i < kMaxSamplePositions; ++i) at[i] = {(positions_[i].x + 8) / 16.0f, (positions_[i].y + 8) / 16.0f};
  VkSampleLocationsInfoEXT info = vk_struct<VkSampleLocationsInfoEXT>(VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
  info.sampleLocationsPerPixel = static_cast<VkSampleCountFlagBits>(positions_samples_);
  info.sampleLocationGridSize = positions_pixels_ == 4 ? VkExtent2D{2, 2} : VkExtent2D{1, 1};
  info.sampleLocationsCount = n;
  info.pSampleLocations = at;
  vk_.CmdSetSampleLocationsEXT(cb_, &info);
}

// Coarse shading (kFeature2ShadingRate): Direct3D's rate is Vulkan's fragment size in the same log2 axes ((x << 2) | y), and its combiners are Vulkan's in the
// same places (PASSTHROUGH keeps, OVERRIDE replaces, MIN, MAX; SUM adds the axes' logarithms, which is MUL of the sizes). Set before every draw: the state is
// dynamic in every pipeline of the device.
void Replay::set_fragment_shading_rate() {
  if (!(features2_ & kFeature2ShadingRate)) return;
  auto op = [](ShadingRateCombiner c) {
    switch (c) {
      case ShadingRateCombiner::Passthrough: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
      case ShadingRateCombiner::Override: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
      case ShadingRateCombiner::Min: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
      case ShadingRateCombiner::Max: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
      case ShadingRateCombiner::Sum: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
    }
    return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
  };
  const uint32_t rate = valid_shading_rate(rate_) ? rate_ : 0;
  const VkExtent2D size = {shading_rate_x(rate), shading_rate_y(rate)};
  // (without a shading-rate image the image's rate is 1x1, which Vulkan's attachment rate is when the rendering has none)
  const VkFragmentShadingRateCombinerOpKHR ops[2] = {op(rate_primitive_), op(rate_image_combiner_)};
  vk_.CmdSetFragmentShadingRateKHR(cb_, &size, ops);
}

// The shading-rate image of the draws that follow (an R8_UINT image of one level, one sample; nullptr: none). Inside a pass the rendering ends and resumes with it.
bool Replay::set_shading_rate_image(VenusImage* image) {
  if (!(features2_ & kFeature2ShadingRate)) return false;
  if (image && (image->format() != Format::R8Uint || image->mip_levels() != 1 || image->desc().samples != 1 || image->desc().kind != TextureKind::Texture2D)) return false;
  if (image == rate_image_) return true;
  rate_image_ = image;
  if (!rendering_) return true;
  end_stream_output();
  segments_close();
  vk_.CmdEndRendering(cb_);
  rendering_ = false;
  barrier();
  return resume_rendering();
}

// Before a rendering: the shading-rate image's tiles (kShadingRateTileSize pixels) into the attachment's texels (the host's texel size, a divisor of the tile):
// each tile is (16 / texel)^2 texels of its rate, and everything past the image is 1x1 (0). False when it cannot be made.
bool Replay::prepare_rate_attachment() {
  rate_attached_ = false;
  if (!rate_image_) return true;
  const VenusDevice::RateAttachment* a = d_.rate_attachment(cb_);
  if (!a) return false;
  const uint32_t texel = d_.context().caps().shading_rate_texel, scale = kShadingRateTileSize / texel;
  barrier();
  const VkClearColorValue zero = {};
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk_.CmdClearColorImage(cb_, a->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  barrier();
  uint32_t w = rate_image_->extent().width, h = rate_image_->extent().height;
  if (w > a->width / scale) w = a->width / scale;
  if (h > a->height / scale) h = a->height / scale;
  VkImageBlit b{};
  b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  b.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
  b.dstOffsets[1] = {static_cast<int32_t>(w * scale), static_cast<int32_t>(h * scale), 1};
  vk_.CmdBlitImage(cb_, rate_image_->image(), VK_IMAGE_LAYOUT_GENERAL, a->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
  barrier();
  rate_attached_ = true;
  return true;
}

bool Replay::run() {
  cb_ = d_.begin_main();
  if (!cb_) return false;
  // (the pools this submit may use, reset before anything is counted)
  for (const RecordedCommand& c : rec_.commands())
    if (c.type == RecordedCommand::Type::BeginQuery || c.type == RecordedCommand::Type::EndQuery) {
      for (uint32_t k = 0; k < VenusDevice::kPoolKinds; ++k)
        if (VkQueryPool pool = d_.query_pool(static_cast<VenusDevice::QueryPoolKind>(k)))
          vk_.CmdResetQueryPool(cb_, pool, 0, k == VenusDevice::kPoolTimestamps ? VenusDevice::kTimestamps : VenusDevice::kQuerySegments);
      break;
    }
  const ConstByteView all_constants = rec_.constants();
  bool ok = true;
  for (const RecordedCommand& c : rec_.commands()) {
    switch (c.type) {
      case RecordedCommand::Type::BeginRenderPass: ok = begin_pass(c.pass); break;
      case RecordedCommand::Type::EndRenderPass:
        if (!in_pass_) ok = false;
        else end_pass();
        break;
      case RecordedCommand::Type::BindPipeline: {
        VenusPipeline* p = const_cast<VenusPipeline*>(static_cast<const VenusPipeline*>(c.pipeline));   // (the recorder keeps pipelines const; their variant caches are the device's)
        if (!p || p->compute()) ok = false;
        else pipeline_ = p, bound_pipeline_ = nullptr, constants_dirty_ = true;
        break;
      }
      case RecordedCommand::Type::BindComputePipeline: {
        VenusPipeline* p = const_cast<VenusPipeline*>(static_cast<const VenusPipeline*>(c.pipeline));
        if (!p || !p->compute() || in_pass_) ok = false;
        else compute_ = p, constants_dirty_ = true;
        break;
      }
      case RecordedCommand::Type::BindVertexBuffer:
        if (c.unit >= kMaxVertexBuffers) ok = false;
        else vertex_[c.unit] = {static_cast<VenusBuffer*>(c.buffer), c.offset};
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        index_ = static_cast<VenusBuffer*>(c.buffer), index_type_ = c.index_type, index_offset_ = c.offset;
        break;
      case RecordedCommand::Type::PushConstants:
        if (!all_constants.try_subview(c.constants_offset, c.constants_size, &constants_)) ok = false;
        constants_dirty_ = true;
        break;
      case RecordedCommand::Type::BindTexture: {
        VenusImage* image = static_cast<VenusImage*>(c.image);
        if (c.unit >= kMaxTextureUnits || !image || !(image->desc().usage & kImageSampled)) {
          ok = false;
          break;
        }
        BoundTexture& t = textures_[c.unit];
        t.image = image, t.sampler = c.sampler, t.has_view = c.has_view, t.view = c.texture_view;
        if (c.has_view) {
          // (the counts made whole, as texture_view_fits wants them)
          TextureView v = c.texture_view;
          if (v.level_count == 0) v.level_count = image->mip_levels() > v.base_level ? image->mip_levels() - v.base_level : 0;
          if (v.layer_count == 0) v.layer_count = image->layer_count() > v.first_layer ? image->layer_count() - v.first_layer : 0;
          if (!texture_view_fits(image->desc().kind, image->desc().layers, image->mip_levels(), v)) ok = false;
          t.view = v;
        }
        constants_dirty_ = true;   // (the comparison rows follow the samplers)
        break;
      }
      case RecordedCommand::Type::SetViewport:
        viewports_[0] = c.viewport, viewport_count_ = 1;
        break;
      case RecordedCommand::Type::SetViewportAt:
        if (c.count > 1 && !d_.context().caps().multi_viewport) {
          ok = false;
          break;
        }
        viewports_[c.unit] = c.viewport, viewport_count_ = c.count;
        break;
      case RecordedCommand::Type::SetScissor:
        if (!in_pass_) ok = false;
        else scissors_[0] = c.scissor, scissor_count_ = 1;
        break;
      case RecordedCommand::Type::SetScissorAt:
        if (!in_pass_ || (c.count > 1 && !d_.context().caps().multi_viewport)) ok = false;
        else scissors_[c.unit] = c.scissor, scissor_count_ = c.count;
        break;
      case RecordedCommand::Type::Draw: ok = draw(c, false); break;
      case RecordedCommand::Type::DrawIndexed: ok = draw(c, true); break;
      case RecordedCommand::Type::SetTopology: topology_ = c.topology; break;
      case RecordedCommand::Type::SetPatchPoints:
        if (c.count == 0 || c.count > kMaxPatchControlPoints) ok = false;
        else patch_points_ = c.count;
        break;
      case RecordedCommand::Type::BindStorageBuffer:
        if (c.unit >= kMaxStorageBuffers || !c.buffer) ok = false;
        else storage_[c.unit] = {static_cast<VenusBuffer*>(c.buffer), c.offset, c.count};
        break;
      case RecordedCommand::Type::BindStorageImage: {
        if (c.unit >= kMaxStorageImages) {
          ok = false;
          break;
        }
        VenusImage* image = static_cast<VenusImage*>(c.image);
        if (image) {
          const StorageImageView& v = c.storage_view;
          if (!(image->desc().usage & kImageStorage) || v.level >= image->mip_levels() || v.layer_count == 0 || v.first_layer >= image->level_layers(v.level) ||
              v.layer_count > image->level_layers(v.level) - v.first_layer || !storage_view_compatible(image->format(), v.format)) {
            ok = false;
            break;
          }
        }
        images_[c.unit] = {image, c.storage_view};
        texels_[c.unit] = BoundTexel();   // (the u# slot holds an image or a typed buffer view, not both)
        break;
      }
      case RecordedCommand::Type::BindStorageTexelBuffer: {
        if (c.unit >= kMaxImageSlots) {
          ok = false;
          break;
        }
        VenusBuffer* b = static_cast<VenusBuffer*>(c.buffer);
        if (b) {
          const TexelBufferView& v = c.texel_view;
          const uint32_t bytes = texel_bytes(v.format);
          if (!texel_format_valid(v.format) || texel_vk_format(v.format) == VK_FORMAT_UNDEFINED || v.elements == 0 || bytes == 0 || v.offset % bytes != 0 ||
              v.offset > b->size() || uint64_t{v.elements} * bytes > b->size() - v.offset) {
            ok = false;
            break;
          }
        }
        texels_[c.unit] = {b, c.texel_view};
        if (c.unit < kMaxStorageImages) images_[c.unit] = BoundImage();
        break;
      }
      case RecordedCommand::Type::Dispatch: ok = dispatch(c); break;
      case RecordedCommand::Type::BeginQuery: ok = begin_query(c); break;
      case RecordedCommand::Type::EndQuery: ok = end_query(c); break;
      case RecordedCommand::Type::BindStreamOutput: ok = bind_stream_output(c); break;
      case RecordedCommand::Type::Resolve: ok = resolve(c); break;
      case RecordedCommand::Type::SetDepthBounds:
        if (!d_.context().caps().depth_bounds) {
          ok = false;
          break;
        }
        depth_bounds_ = c.count != 0, depth_min_ = c.viewport.min_depth, depth_max_ = c.viewport.max_depth;
        depth_bounds_dirty_ = true;
        break;
      case RecordedCommand::Type::CopySamples: ok = copy_samples(c); break;
      // (each only where the device offers its feature: the kernel sends none otherwise, and a stray one fails the submit)
      case RecordedCommand::Type::SetSamplePositions: ok = set_sample_positions(c, all_constants); break;
      case RecordedCommand::Type::SetViewInstance:
        if (!(features2_ & kFeature2ViewInstancing)) {
          ok = false;
          break;
        }
        view_id_ = c.count, view_viewport_ = c.first, view_layer_ = c.unit;
        constants_dirty_ = true;
        break;
      case RecordedCommand::Type::SetShadingRate:
        if (!(features2_ & kFeature2ShadingRate) || (c.first & 0xFF) > static_cast<uint32_t>(ShadingRateCombiner::Sum) ||
            (c.first >> 8 & 0xFF) > static_cast<uint32_t>(ShadingRateCombiner::Sum)) {
          ok = false;
          break;
        }
        rate_ = c.count;
        rate_primitive_ = static_cast<ShadingRateCombiner>(c.first & 0xFF), rate_image_combiner_ = static_cast<ShadingRateCombiner>(c.first >> 8 & 0xFF);
        break;
      case RecordedCommand::Type::SetShadingRateImage: ok = set_shading_rate_image(static_cast<VenusImage*>(c.image)); break;
    }
    if (!ok) break;
  }
  if (ok && in_pass_) ok = false;   // (a pass left open)
  for (const UserQuery& q : queries_)
    if (q.begun && !q.ended) ok = false;   // (a query left open fails the submit, as on virgl)
  end_stream_output();
  segments_close();
  if (rendering_) vk_.CmdEndRendering(cb_);
  rendering_ = false;
  const bool done = d_.finish_main(cb_);
  release();
  if (!(ok && done)) return false;
  stream_output_results();
  for (const UserQuery& q : queries_)
    if (q.begun && q.kind == QueryKind::TimestampDisjoint) d_.clear_clock_interrupted();
  return read_queries();
}

}  // namespace

// The submit's command buffer, begun (the queue's lock held until finish_main).
VkCommandBuffer VenusDevice::begin_main() {
  lock_queue();
  const VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  if (vk().BeginCommandBuffer(main_, &bi) != VK_SUCCESS) {
    unlock_queue();
    return nullptr;
  }
  return main_;
}

bool VenusDevice::finish_main(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
  vk().CmdPipelineBarrier2(cb, &di);
  bool ok = vk().EndCommandBuffer(cb) == VK_SUCCESS;
  if (ok) {
    VkSubmitInfo si = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    ok = vk().QueueSubmit(ctx_.queue(), 1, &si, fence_) == VK_SUCCESS && wait_fence(fence_);
  }
  vk().ResetCommandBuffer(cb, 0);
  unlock_queue();
  return ok;
}

bool VenusDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  Replay replay(*this, *static_cast<VenusCommands*>(commands));
  return replay.run();
}

}  // namespace graphics

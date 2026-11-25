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

// The kernel objects a process holds handles to, besides allocations (VidMm.hpp): synchronization objects, shaders and pipelines, devices and contexts, and the
// submissions that wait in a context's queue.
#pragma once
#include "Kernel/Dxgkrnl/D3d12.hpp"
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaDecoder.hpp"
#include "Utility/Move.hpp"
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "Kernel/Dxgkrnl/VidMm.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace dxgk {

using StorageMask = graphics::SlotMask<graphics::kMaxStorageBuffers>;   // (the storage buffer slots a shader or pipeline uses)

class Adapter;

class SyncObject {
 public:
  SyncObject(SyncType type, uint64_t initial, uint64_t limit) : type_(type), value_(initial), limit_(limit) {}
  SyncType type() const { return type_; }
  // A fence: the value reached so far. A semaphore: the signals not yet consumed.
  uint64_t value() const { return value_; }
  // Whether a wait for `target` is satisfied now (a fence reached it; a semaphore has a signal to take).
  bool satisfied(uint64_t target) const { return type_ == SyncType::Fence ? value_ >= target : value_ > 0; }
  // A wait that is satisfied takes what it waits for (nothing for a fence, one signal of a semaphore).
  void consume() {
    if (type_ == SyncType::Semaphore && value_) --value_;
  }
  // Raises a fence to `value` (not below its current value) or adds one signal to a semaphore (not past its limit). False if that is not allowed.
  bool signal(uint64_t value) {
    if (type_ == SyncType::Fence) {
      if (value < value_) return false;
      value_ = value;
      return true;
    }
    if (value_ >= limit_) return false;
    ++value_;
    return true;
  }

 private:
  SyncType type_;
  uint64_t value_, limit_;
};

class ShaderObject {
 public:
  ShaderObject(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device) : allocator_(allocator), device_(device) {}
  ~ShaderObject();
  ShaderObject(const ShaderObject&) = delete;
  ShaderObject& operator=(const ShaderObject&) = delete;
  graphics::ShaderStage stage() const { return stage_; }
  graphics::IShaderModule* module() const { return module_; }
  graphics::GeometryPrimitive geometry_input() const { return geometry_input_; }
  uint32_t geometry_streams() const { return geometry_streams_; }
  uint32_t tess_input_points() const { return tess_input_points_; }
  uint32_t tess_output_points() const { return tess_output_points_; }
  graphics::TessDomain tess_domain() const { return tess_domain_; }
  graphics::TextureKind texture_kind(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? texture_kinds_[unit] : graphics::TextureKind::Texture2D; }
  // The texture register (t#) and the sampler register (s#) a DXBC shader reads through `unit` (D3d9Program::texture_register / sampler_register; the others: the unit's own).
  uint32_t unit_texture(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? unit_texture_[unit] : unit; }
  uint32_t unit_sampler(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? unit_sampler_[unit] : unit; }

 private:
  friend class Adapter;
  static_assert(graphics::kMaxTextureUnits == 16, "the pairs list every unit");
  uint8_t unit_texture_[graphics::kMaxTextureUnits] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  uint8_t unit_sampler_[graphics::kMaxTextureUnits] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  uint32_t sampled_mask_ = 0;   // (D3d9Program::sampled_units: the units whose sampler register is the shader's)
  allocators::IAllocator& allocator_;
  graphics::IGraphicsDevice& device_;
  graphics::IShaderModule* module_ = nullptr;
  graphics::ShaderStage stage_ = graphics::ShaderStage::Vertex;
  ByteView text_;                 // the TGSI text the device may keep pointing at (NUL terminated)
  ArrayView<uint32_t> tokens_;    // the Direct3D 9 tokens
  graphics::TextureKind texture_kinds_[graphics::kMaxTextureUnits] = {};   // what a DXBC shader declared for its units
  uint32_t texture_mask_ = 0;               // ... the units it reads (bit n: unit n)
  bool knows_textures_ = false;             // made from DXBC: texture_mask_ says all it reads
  uint32_t thread_group_[3] = {1, 1, 1};     // a compute shader's thread group
  StorageMask storage_mask_;                 // ... and the storage buffer slots it uses (u<n>, the views of buffers after them, the counters)
  bool fragment_stores_ = false;             // a pixel shader that writes them
  bool vertex_stores_ = false;               // ... or a vertex shader
  bool geometry_stores_ = false;             // ... or a geometry shader
  bool tessellation_stores_ = false;         // ... or a hull or domain shader
  graphics::GeometryPrimitive geometry_input_ = graphics::GeometryPrimitive::Triangle;   // a geometry shader's input
  uint32_t geometry_streams_ = 1;                          // ... and how many output streams it has
  uint32_t tess_input_points_ = 0, tess_output_points_ = 0;   // a hull or domain shader's control points
  graphics::TessDomain tess_domain_ = graphics::TessDomain::Triangle;   // ... and the domain it tessellates
  // The storage images a shader declares (the u# ones, and the typed views of buffers of t#: graphics::kMaxImageSlots), their dimensions and the values they read and write; a geometry
  // shader that writes SV_RenderTargetArrayIndex.
  D3d9Program::ImageMask image_mask_;
  D3d9Program::ImageDim image_dims_[graphics::kMaxImageSlots] = {};
  D3d9Program::ImageReturn image_returns_[graphics::kMaxImageSlots] = {};
  bool writes_layer_ = false;
  D3d9Binding d3d9_;              // Direct3D 9 bytecode being interpreted (the module's function finds the program through it)
};

class PipelineObject {
 public:
  PipelineObject(graphics::IGraphicsDevice& device) : device_(device) {}
  ~PipelineObject();
  PipelineObject(const PipelineObject&) = delete;
  PipelineObject& operator=(const PipelineObject&) = delete;
  const graphics::IPipeline* pipeline() const { return pipeline_; }
  uint32_t vertex_stride() const { return stride_; }
  // The vertex buffers (slots) the attributes read, which of them per vertex and which per instance, and the stride of each.
  uint32_t vertex_slots() const { return vertex_slot_mask_ | instance_slot_mask_; }
  uint32_t per_vertex_slots() const { return vertex_slot_mask_; }
  uint32_t per_instance_slots() const { return instance_slot_mask_; }
  // The smallest step rate of the per-instance attributes a slot feeds, of those that step (graphics::VertexAttribute::step_rate); 0 when none of them steps.
  uint32_t slot_step_rate(uint32_t slot) const { return slot < graphics::kMaxVertexBuffers ? slot_step_rates_[slot] : 0; }
  uint32_t slot_stride(uint32_t slot) const { return slot == 0 ? stride_ : slot_strides_[slot]; }
  uint32_t attribute_count() const { return attribute_count_; }
  uint32_t texture_count() const { return texture_count_; }
  uint32_t color_target_count() const { return color_target_count_; }
  // A compute pipeline (Adapter::create_compute_pipeline): dispatched, not drawn with. It says its thread group and which storage buffer slots (bit n: u<n>) it reads or writes.
  bool compute() const { return compute_; }
  // The geometry shader's input (a draw must make triangles, lines or points to match), if the pipeline has one.
  // Does the pipeline blend (any target)? Draws into integer targets must not.
  bool blends() const { return blends_; }
  bool has_geometry() const { return static_cast<bool>(geometry_); }
  // Tessellation: the pipeline has a hull and a domain shader, which take patches of this many control points.
  bool has_tessellation() const { return static_cast<bool>(hull_); }
  uint32_t tess_input_points() const { return tess_input_points_; }
  graphics::GeometryPrimitive geometry_input() const { return geometry_input_; }
  uint32_t thread_group(uint32_t axis) const { return axis < 3 ? thread_group_[axis] : 1; }
  const StorageMask& storage_mask() const { return storage_mask_; }
  uint32_t texture_mask() const { return texture_mask_; }   // a compute pipeline: the texture units its shader reads
  uint32_t declared_units() const { return declared_units_; }   // the texture units the shaders of the pipeline declare (bit n: unit n), any pipeline
  bool writes_storage() const { return fragment_stores_ || vertex_stores_ || geometry_stores_ || tessellation_stores_; }   // a shader of the pipeline writes buffers
  bool has_per_instance() const { return per_instance_; }
  // Stream output: which buffers (bit n: buffer n) the pipeline's declaration writes to, and whether it has one.
  uint32_t stream_output_buffers() const { return stream_output_buffers_; }
  bool has_stream_output() const { return stream_output_buffers_ != 0; }
  // The kind of texture the shaders declared for a unit (2D when they do not say: only a DXBC program does).
  graphics::TextureKind texture_kind(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? texture_kinds_[unit] : graphics::TextureKind::Texture2D; }
  // The texture and sampler registers the shaders read through a unit (ShaderObject::unit_texture / unit_sampler, of the stage that reads it): what Direct3D 12's binding puts there.
  uint32_t unit_texture(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? unit_texture_[unit] : unit; }
  uint32_t unit_sampler(uint32_t unit) const { return unit < graphics::kMaxTextureUnits ? unit_sampler_[unit] : unit; }
  // The storage images (and typed views of buffers) the pipeline's shaders declare, with each one's dimension and the values it reads and writes: a dispatch or draw needs them bound.
  const D3d9Program::ImageMask& image_mask() const { return image_mask_; }
  D3d9Program::ImageDim image_dim(uint32_t slot) const { return slot < graphics::kMaxImageSlots ? image_dims_[slot] : D3d9Program::ImageDim::None; }
  D3d9Program::ImageReturn image_return(uint32_t slot) const { return slot < graphics::kMaxImageSlots ? image_returns_[slot] : D3d9Program::ImageReturn::Float; }

 private:
  friend class Adapter;
  graphics::IGraphicsDevice& device_;
  D3d9Program::ImageMask image_mask_;
  D3d9Program::ImageDim image_dims_[graphics::kMaxImageSlots] = {};
  D3d9Program::ImageReturn image_returns_[graphics::kMaxImageSlots] = {};
  graphics::IPipeline* pipeline_ = nullptr;
  bool per_instance_ = false;
  uint32_t stream_output_buffers_ = 0;
  bool compute_ = false;
  bool blends_ = false;
  SharedPointer<ShaderObject> geometry_, hull_, domain_;
  uint32_t tess_input_points_ = 0;
  graphics::GeometryPrimitive geometry_input_ = graphics::GeometryPrimitive::Triangle;
  uint32_t thread_group_[3] = {1, 1, 1};
  StorageMask storage_mask_;
  bool fragment_stores_ = false, vertex_stores_ = false, geometry_stores_ = false, tessellation_stores_ = false;
  SharedPointer<ShaderObject> vertex_, fragment_;   // (kept alive for as long as the pipeline is)
  graphics::TextureKind texture_kinds_[graphics::kMaxTextureUnits] = {};
  uint8_t unit_texture_[graphics::kMaxTextureUnits] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  uint8_t unit_sampler_[graphics::kMaxTextureUnits] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  uint32_t texture_mask_ = 0;   // a compute pipeline: the units its shader reads, which a dispatch needs bound
  uint32_t declared_units_ = 0;
  uint32_t vertex_slot_mask_ = 0, instance_slot_mask_ = 0, slot_strides_[graphics::kMaxVertexBuffers] = {};
  uint32_t slot_step_rates_[graphics::kMaxVertexBuffers] = {};
  uint32_t stride_ = 0, attribute_count_ = 0, texture_count_ = 0, color_target_count_ = 1;
};

class DeviceObject {
 public:
  explicit DeviceObject(Adapter& adapter) : adapter_(adapter) {}
  Adapter& adapter() const { return adapter_; }
  DeviceState state;

 private:
  Adapter& adapter_;
};

// A video decoder (Kernel::create_video_decoder): the DXVA decoder and what it was made for. The streams that decode with it hold it (Submission::decoders).
class VideoDecoderObject {
 public:
  VideoDecoderObject(UniquePointer<hardware::IDxvaDecoder> decoder, const VideoDecoderDesc& desc) : decoder_(util::move(decoder)), desc_(desc) {}
  hardware::IDxvaDecoder& decoder() { return *decoder_; }
  const VideoDecoderDesc& desc() const { return desc_; }
  uint64_t pictures = 0, failures = 0;   // decoded, and refused (diagnostics)

 private:
  UniquePointer<hardware::IDxvaDecoder> decoder_;
  VideoDecoderDesc desc_;
};

// What waits in a context's queue (WDDM's per-context DMA buffer queue).
enum class SubmissionKind : uint8_t { Render, Signal, Wait, Present };

struct Submission {
  explicit Submission(allocators::IAllocator& allocator) : commands(allocator), allocations(allocator), pipelines(allocator), signatures(allocator), heaps(allocator), decoders(allocator) {}
  SubmissionKind kind = SubmissionKind::Render;
  // Render: a copy of the command stream (what the process wrote afterwards does not matter), the allocation list and the pipelines it binds, in the order it binds them.
  ByteBuffer commands;
  List<SharedPointer<Allocation>> allocations;
  List<SharedPointer<PipelineObject>> pipelines;
  List<SharedPointer<RootSignatureObject>> signatures;   // (Direct3D 12: the root signatures and descriptor heaps it sets, in order)
  List<SharedPointer<DescriptorHeapObject>> heaps;
  List<SharedPointer<VideoDecoderObject>> decoders;   // (the video decoders its DecodeFrames name, in order)
  // Signal / Wait
  SharedPointer<SyncObject> sync;
  uint64_t value = 0;
  // Present: allocations[0] is the source, allocations[1] the destination (if any).
  PresentArgs present;
};

class ContextObject {
 public:
  ContextObject(allocators::IAllocator& allocator, SharedPointer<DeviceObject> device, ContextPriority priority)
      : device(static_cast<SharedPointer<DeviceObject>&&>(device)), priority(priority), queue(allocator) {}
  SharedPointer<DeviceObject> device;
  ContextPriority priority;
  bool lost = false;       // a submission failed: nothing more runs on this context
  uint64_t queued_bytes = 0;
  List<UniquePointer<Submission>> queue;
};

// How a command stream finds the pipelines it names (the calling process's handles).
class IPipelineLookup {
 public:
  virtual SharedPointer<PipelineObject> find_pipeline(Handle handle) = 0;
  virtual SharedPointer<RootSignatureObject> find_root_signature(Handle handle) = 0;
  virtual SharedPointer<DescriptorHeapObject> find_descriptor_heap(Handle handle) = 0;
  virtual SharedPointer<VideoDecoderObject> find_video_decoder(Handle handle) = 0;

 protected:
  IPipelineLookup() = default;
  ~IPipelineLookup() = default;
};

}  // namespace dxgk

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

// The command stream a user-mode driver hands to Kernel::render(): the "DMA buffer" of this kernel. It is not hardware-specific - the kernel validates it against the
// allocation list and the pipelines it names and replays it on the adapter's graphics::IGraphicsDevice - so one user-mode driver serves every adapter.
//
// Layout: a sequence of packets, little-endian 32-bit words. Word 0 of a packet is op | (size_in_words << 16) (the size counts that word); the payload follows.
// Allocations are named by an index into the allocation list passed with the stream (never by address: the kernel decides where memory is), pipelines by their handle.
// State (pass, pipeline, buffers, textures) does not carry over from one stream to the next.
//
//   BeginPass        color_index, depth_index (kNoAllocation: none), flags, clear r, g, b, a (floats), clear depth (float), clear stencil, [up to three more colour indices (kFeatureMultipleTargets)]
//   EndPass
//   BindPipeline     pipeline handle
//   BindVertexBuffer allocation_index, offset
//   BindIndexBuffer  allocation_index, index_type (0: 16 bit, 1: 32 bit), offset
//   PushConstants    byte_count (a multiple of 16, at most the adapter's max_push_constant_bytes: 128 for most, 4096 for the CPU renderer and virgl), then the bytes padded to a whole word
//   BindTexture      unit, allocation_index, filter, address_u, address_v, mip_filter, lod_bias, min_lod, max_lod (floats)
//   SetViewport      x, y, width, height, min_depth, max_depth (floats)
//   SetScissor       x, y, width, height (x and y signed)
//   Draw             vertex_count, first_vertex
//   DrawIndexed      index_count, first_index, vertex_offset (signed)
//   CopyRegion       dst_index, dst_x, dst_y, src_index, src_x, src_y, width, height   (colour images of one format; also staging memory)
//   GenerateMipmaps  allocation_index
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include <initializer_list>
#include "Utility/ByteBuffer.hpp"

namespace dxgk {

enum class Op : uint16_t {
  BeginPass = 1,
  EndPass,
  BindPipeline,
  BindVertexBuffer,
  BindIndexBuffer,
  PushConstants,
  BindTexture,
  SetViewport,
  SetScissor,
  Draw,
  DrawIndexed,
  CopyRegion,
  GenerateMipmaps,
  SetTopology,
  DrawInstanced,
  DrawIndexedInstanced,
};

// How the vertices (or indices) of a draw make triangles (D3D10 IASetPrimitiveTopology / D3D9 primitive types); the default is a list. A strip or fan is expanded to a list of
// triangles by the kernel, so any device draws it; with an index buffer the index 0xFFFF (0xFFFFFFFF for 32-bit indices) ends a strip and starts the next.
enum class Topology : uint32_t { TriangleList = 0, TriangleStrip = 1, TriangleFan = 2, LineList = 3, LineStrip = 4, PointList = 5 };   // (lines and points: kFeatureLines of the adapter)

constexpr uint32_t kNoAllocation = 0xFFFFFFFFu;
constexpr uint32_t kMaxPacketWords = 2 + graphics::kMaxPushConstantBytes / 4;   // (PushConstants is the longest: the byte count, then up to kMaxPushConstantBytes of constants)
constexpr uint64_t kMaxCommandBytes = 16 * 1024 * 1024;

enum BeginPassFlags : uint32_t { kPassClearColor = 1u << 0, kPassClearDepth = 1u << 1, kPassClearStencil = 1u << 2 };

inline uint32_t float_bits(float f) { return __builtin_bit_cast(uint32_t, f); }
inline float bits_float(uint32_t w) { return __builtin_bit_cast(float, w); }

// What a user-mode driver (and the tests) build a stream with. Every call returns false once memory ran out; `ok()` says whether the stream is whole.
class CommandWriter {
 public:
  explicit CommandWriter(allocators::IAllocator& allocator) : bytes_(allocator) {}

  bool ok() const { return ok_; }
  ConstByteView view() const { return bytes_.view(); }
  void clear() { bytes_.clear(); ok_ = true; }

  void begin_pass(uint32_t color, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil) {
    packet(Op::BeginPass, {color, depth, flags, float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w), float_bits(clear_depth), clear_stencil});
  }
  // A pass with several colour targets (the adapter's kFeatureMultipleTargets): `extra` are the allocation indices of targets 1 .. n (at most three).
  void begin_pass_targets(uint32_t color, std::initializer_list<uint32_t> extra, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::BeginPass) | ((10 + static_cast<uint32_t>(extra.size())) << 16));
    for (uint32_t w : {color, depth, flags, float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w), float_bits(clear_depth), clear_stencil})
      ok_ &= bytes_.put32(w);
    for (uint32_t w : extra) ok_ &= bytes_.put32(w);
  }
  void end_pass() { packet(Op::EndPass, {}); }
  void bind_pipeline(Handle pipeline) { packet(Op::BindPipeline, {pipeline}); }
  void bind_vertex_buffer(uint32_t allocation, uint32_t offset = 0) { packet(Op::BindVertexBuffer, {allocation, offset}); }
  void bind_index_buffer(uint32_t allocation, graphics::IndexType type, uint32_t offset = 0) { packet(Op::BindIndexBuffer, {allocation, type == graphics::IndexType::Uint32 ? 1u : 0u, offset}); }
  void push_constants(ConstByteView data) {
    const uint32_t words = static_cast<uint32_t>((data.size() + 3) / 4);
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::PushConstants) | ((2 + words) << 16));
    ok_ &= bytes_.put32(static_cast<uint32_t>(data.size()));
    ok_ &= bytes_.append(data);
    ok_ &= bytes_.put_zeros(uint64_t{words} * 4 - data.size());
  }
  void bind_texture(uint32_t unit, uint32_t allocation, const graphics::SamplerDesc& s) {
    packet(Op::BindTexture, {unit, allocation, static_cast<uint32_t>(s.filter), static_cast<uint32_t>(s.address_u), static_cast<uint32_t>(s.address_v), static_cast<uint32_t>(s.mip_filter),
                             float_bits(s.lod_bias), float_bits(s.min_lod), float_bits(s.max_lod)});
  }
  void set_viewport(const graphics::Viewport& v) { packet(Op::SetViewport, {float_bits(v.x), float_bits(v.y), float_bits(v.width), float_bits(v.height), float_bits(v.min_depth), float_bits(v.max_depth)}); }
  void set_scissor(const graphics::Rect2D& r) { packet(Op::SetScissor, {static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), r.width, r.height}); }
  void set_topology(Topology topology) { packet(Op::SetTopology, {static_cast<uint32_t>(topology)}); }
  void draw(uint32_t vertex_count, uint32_t first_vertex = 0) { packet(Op::Draw, {vertex_count, first_vertex}); }
  // Several instances (the adapter's kFeatureInstancing): the draw is made instance_count times, numbered from first_instance; per-instance attributes step with them.
  void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex = 0, uint32_t first_instance = 0) { packet(Op::DrawInstanced, {vertex_count, first_vertex, instance_count, first_instance}); }
  void draw_indexed_instanced(uint32_t index_count, uint32_t instance_count, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) {
    packet(Op::DrawIndexedInstanced, {index_count, first_index, static_cast<uint32_t>(vertex_offset), instance_count, first_instance});
  }
  void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0) { packet(Op::DrawIndexed, {index_count, first_index, static_cast<uint32_t>(vertex_offset)}); }
  void copy_region(uint32_t dst, uint32_t dst_x, uint32_t dst_y, uint32_t src, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height) {
    packet(Op::CopyRegion, {dst, dst_x, dst_y, src, src_x, src_y, width, height});
  }
  void generate_mipmaps(uint32_t allocation) { packet(Op::GenerateMipmaps, {allocation}); }

 private:
  void packet(Op op, std::initializer_list<uint32_t> payload) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(op) | ((1 + static_cast<uint32_t>(payload.size())) << 16));
    for (uint32_t word : payload) ok_ &= bytes_.put32(word);
  }
  ByteBuffer bytes_;
  bool ok_ = true;
};

}  // namespace dxgk

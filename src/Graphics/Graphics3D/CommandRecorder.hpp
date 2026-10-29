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

// ICommandBuffer implementation that just records: backends execute the list
// at submit time (the software device by rasterizing, the GPU device by
// translating to its command stream).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "ObjectContainer/List.hpp"

namespace graphics {

struct RecordedCommand {
  enum class Type : uint8_t {
    BeginRenderPass, BindPipeline, BindVertexBuffer, BindIndexBuffer, PushConstants, BindTexture, SetViewport, SetScissor,
    Draw, DrawIndexed, EndRenderPass, SetTopology,
  };
  Type type = Type::Draw;
  RenderPassBegin pass;
  const IPipeline* pipeline = nullptr;
  IBuffer* buffer = nullptr;
  uint64_t offset = 0;
  IndexType index_type = IndexType::Uint16;
  uint32_t constants_offset = 0, constants_size = 0;  // into CommandRecorder::constants()
  Viewport viewport;
  Rect2D scissor;
  uint32_t count = 0, first = 0;
  int32_t vertex_offset = 0;
  uint32_t instance_count = 1, first_instance = 0;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;   // SetTopology
  uint32_t unit = 0;       // BindTexture
  IImage* image = nullptr;
  SamplerDesc sampler;
};

class CommandRecorder : public ICommandBuffer {
 public:
  explicit CommandRecorder(allocators::IAllocator& allocator) : commands_(allocator), constants_(allocator) {}

  void begin() override {
    commands_.clear();
    constants_.clear();
    ok_ = true;
  }
  void begin_render_pass(const RenderPassBegin& pass) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::BeginRenderPass;
    c.pass = pass;
    add(c);
  }
  void bind_pipeline(const IPipeline* pipeline) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::BindPipeline;
    c.pipeline = pipeline;
    add(c);
  }
  void bind_vertex_buffer(IBuffer* buffer, uint64_t offset) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::BindVertexBuffer;
    c.buffer = buffer;
    c.offset = offset;
    add(c);
  }
  void bind_index_buffer(IBuffer* buffer, IndexType type, uint64_t offset) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::BindIndexBuffer;
    c.buffer = buffer;
    c.index_type = type;
    c.offset = offset;
    add(c);
  }
  void push_constants(const void* data, uint32_t size) override {
    if (!data || size > kMaxPushConstantBytes || size % 16 != 0) {
      ok_ = false;
      return;
    }
    RecordedCommand c;
    c.type = RecordedCommand::Type::PushConstants;
    c.constants_offset = static_cast<uint32_t>(constants_.count());
    c.constants_size = size;
    for (uint32_t i = 0; i < size; ++i)
      if (!constants_.push_back(static_cast<const uint8_t*>(data)[i])) ok_ = false;
    add(c);
  }
  void bind_texture(uint32_t unit, IImage* image, const SamplerDesc& sampler) override {
    if (unit >= kMaxTextureUnits || !image) {
      ok_ = false;
      return;
    }
    RecordedCommand c;
    c.type = RecordedCommand::Type::BindTexture;
    c.unit = unit;
    c.image = image;
    c.sampler = sampler;
    add(c);
  }
  void set_viewport(const Viewport& viewport) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::SetViewport;
    c.viewport = viewport;
    add(c);
  }
  void set_scissor(const Rect2D& scissor) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::SetScissor;
    c.scissor = scissor;
    add(c);
  }
  void draw(uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count, uint32_t first_instance) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::Draw;
    c.count = vertex_count;
    c.first = first_vertex;
    c.instance_count = instance_count;
    c.first_instance = first_instance;
    add(c);
  }
  void draw_indexed(uint32_t index_count, uint32_t first_index, int32_t vertex_offset, uint32_t instance_count, uint32_t first_instance) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::DrawIndexed;
    c.count = index_count;
    c.first = first_index;
    c.vertex_offset = vertex_offset;
    c.instance_count = instance_count;
    c.first_instance = first_instance;
    add(c);
  }
  void set_primitive_topology(PrimitiveTopology topology) override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::SetTopology;
    c.topology = topology;
    add(c);
  }
  void end_render_pass() override {
    RecordedCommand c;
    c.type = RecordedCommand::Type::EndRenderPass;
    add(c);
  }
  bool end() override { return ok_; }

  const List<RecordedCommand>& commands() const { return commands_; }
  ConstByteView constants() const { return ConstByteView(constants_.view()); }

 private:
  void add(const RecordedCommand& c) {
    if (!commands_.push_back(c)) ok_ = false;
  }

  List<RecordedCommand> commands_;
  List<uint8_t> constants_;
  bool ok_ = true;
};

}  // namespace graphics

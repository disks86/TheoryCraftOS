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

// The geometry and tessellation stages of a draw (desktop GL), between the vertex stage and the rasterizer. run_draw shades the vertices of each input primitive (or patch) into
// records, the outputs the next stage reads (ProgramObj::vs_record), and hands them to StagePipeline::primitive:
//  - tessellation: the control stage runs once per output vertex of the patch, its invocations side by side up to each barrier(), writing the patch's outputs in its shared region;
//    the primitive generator (graphics::Tessellator, the Direct3D 11 reference one, which is also OpenGL's) makes the domain points and their triangles, lines or points; the
//    evaluation stage runs once per point;
//  - geometry: runs once per primitive (times its invocations); each EmitVertex() takes the outputs as a vertex of the output strip.
// The vertices of the last stage go to the rasterizer through IPrimitiveOut.
#pragma once
#include "Graphics/Graphics3D/Software/Tessellator.hpp"
#include "WebGl/GlContext.hpp"

namespace webgl {

class IPrimitiveOut {
 public:
  virtual void point(const gpu::RVertex& a) = 0;
  virtual void line(const gpu::RVertex& a, const gpu::RVertex& b, bool first) = 0;
  virtual void triangle(const gpu::RVertex& a, const gpu::RVertex& b, const gpu::RVertex& c) = 0;

 protected:
  ~IPrimitiveOut() = default;
};

class StagePipeline final : public shader::IStreamHost {
 public:
  StagePipeline(allocators::IAllocator& allocator, ProgramObj& program, const DesktopState& desktop, shader::ITextureHost* textures, IPrimitiveOut& out);
  StagePipeline(const StagePipeline&) = delete;
  StagePipeline& operator=(const StagePipeline&) = delete;

  // Makes the machines and the starting private regions; false if out of memory.
  bool begin();
  // One input primitive or patch: the vertex stage's record of each of its vertices (ProgramObj::vs_record_slots slots each). False if a shader failed.
  bool primitive(ArrayView<const uint32_t* const> records, uint32_t primitive_id);

  // shader::IStreamHost (the geometry stage)
  bool emit_vertex(uint32_t stream) override;
  bool end_primitive(uint32_t stream) override;

 private:
  struct StageRun {
    ExtraStage* stage = nullptr;
    List<uint32_t> snapshot;      // the private region with this primitive's inputs, restored for each invocation
    int32_t invocation_slot = -1, primitive_slot = -1, vertices_slot = -1, coord_slot = -1;
    explicit StageRun(allocators::IAllocator& a) : snapshot(a) {}
  };
  bool prepare(StageRun& run);
  bool tessellate(ArrayView<const uint32_t* const> records, uint32_t primitive_id);
  bool geometry(ArrayView<const uint32_t* const> records, uint32_t primitive_id);
  // The inputs from per-vertex records into a stage's snapshot.
  bool load_inputs(StageRun& run, ArrayView<const uint32_t* const> records);
  // A vertex for the rasterizer from the last stage's private region.
  void to_vertex(const uint32_t* privates, gpu::RVertex* out) const;
  // The outputs a per-vertex stage keeps for the next one, from its private region.
  bool gather(const ExtraStage& stage, const uint32_t* privates, uint32_t* record) const;
  bool run_one(shader::Machine& m, const List<uint32_t>& snapshot, int32_t invocation_slot, uint32_t invocation);

  allocators::IAllocator& a_;
  ProgramObj& p_;
  const DesktopState& desktop_;
  shader::ITextureHost* textures_;
  IPrimitiveOut& out_;
  StageRun gs_, tcs_, tes_;
  const shader::Module& last_;
  uint32_t pos_slot_ = 0, size_slot_ = 0;
  bool has_pos_ = false, has_size_ = false;
  // the geometry stage's output strip
  gpu::RVertex strip_[2];
  uint32_t strip_count_ = 0, emitted_ = 0, max_vertices_ = 0, output_ = 0, gs_primitives_ = 0;
  // the evaluation stage's vertices of a patch (records, when a geometry stage follows; else vertices)
  List<uint32_t> tes_records_;
  List<gpu::RVertex> tes_vertices_;
  UniquePointer<graphics::Tessellator> tessellator_;
};

}  // namespace webgl

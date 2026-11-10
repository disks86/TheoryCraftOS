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

#include "WebGl/GlStages.hpp"

namespace webgl {

using shader::Machine;

namespace {

inline float u2f(uint32_t u) { return __builtin_bit_cast(float, u); }
inline uint32_t f2u(float f) { return __builtin_bit_cast(uint32_t, f); }

int32_t slot_of(const shader::Module& m, const char* name) {
  const int32_t g = m.find_global(name);
  return g >= 0 ? static_cast<int32_t>(m.globals[static_cast<uint64_t>(g)]->slot) : -1;
}

// Copies a list of slots, both ends checked against their regions.
bool copy_slots(ArrayView<uint32_t> dst, uint64_t to, ArrayView<const uint32_t> src, uint64_t from, uint32_t count) {
  if (to > dst.size() || count > dst.size() - to || from > src.size() || count > src.size() - from) return false;
  for (uint32_t k = 0; k < count; ++k) dst[to + k] = src[from + k];
  return true;
}

}  // namespace

StagePipeline::StagePipeline(allocators::IAllocator& allocator, ProgramObj& program, const DesktopState& desktop, shader::ITextureHost* textures, IPrimitiveOut& out,
                             GlContext::TfRecorder& tf)
    : a_(allocator), p_(program), desktop_(desktop), textures_(textures), out_(out), tf_(tf), gs_(allocator), tcs_(allocator), tes_(allocator), last_(program.last_module()),
      strip_tf_(allocator), tes_records_(allocator), tes_vertices_(allocator), tes_tf_(allocator) {
  gs_.stage = p_.extra[kStageGeometry].get();
  tcs_.stage = p_.extra[kStageTessControl].get();
  tes_.stage = p_.extra[kStageTessEval].get();
  const int32_t pos = last_.find_builtin_global(shader::kBvPosition), size = last_.find_builtin_global(shader::kBvPointSize);
  if (pos >= 0) { has_pos_ = true; pos_slot_ = last_.globals[static_cast<uint64_t>(pos)]->slot; }
  if (size >= 0) { has_size_ = true; size_slot_ = last_.globals[static_cast<uint64_t>(size)]->slot; }
}

bool StagePipeline::prepare(StageRun& run) {
  if (!run.stage) return true;
  ExtraStage& st = *run.stage;
  const shader::Module& m = *st.module;
  const uint32_t machines = &run == &tcs_ ? static_cast<uint32_t>(m.tcs_vertices) : 1;
  while (st.machines.count() < machines) {
    UniquePointer<Machine> mach = UniquePointer<Machine>::make(a_, m, a_);
    if (!mach || !mach->valid() || !st.machines.push_back(static_cast<UniquePointer<Machine>&&>(mach))) return false;
  }
  for (UniquePointer<Machine>& mach : st.machines) {
    mach->set_shared(st.shared.view().data());
    mach->set_texture_host(textures_);
    mach->set_stream_host(&run == &gs_ ? this : nullptr);
  }
  if (st.initial.count() != m.private_slots) {
    Machine& first = *st.machines[0];
    if (!first.reset_privates()) return false;
    st.initial.clear();
    if (!st.initial.reserve(m.private_slots)) return false;
    for (uint32_t i = 0; i < m.private_slots; ++i) st.initial.push_back(first.private_region()[i]);
  }
  run.snapshot.clear();
  if (!run.snapshot.reserve(m.private_slots)) return false;
  for (uint32_t i = 0; i < m.private_slots; ++i) run.snapshot.push_back(0);
  run.invocation_slot = slot_of(m, "gl_InvocationID");
  run.primitive_slot = slot_of(m, &run == &gs_ ? "gl_PrimitiveIDIn" : "gl_PrimitiveID");
  run.vertices_slot = slot_of(m, "gl_PatchVerticesIn");
  run.coord_slot = slot_of(m, "gl_TessCoord");
  return true;
}

bool StagePipeline::begin() {
  if (!prepare(gs_) || !prepare(tcs_) || !prepare(tes_)) return false;
  if (gs_.stage && tf_.capturing()) {
    const uint64_t n = uint64_t{3} * tf_.record_slots();
    if (!strip_tf_.reserve(n)) return false;
    for (uint64_t i = 0; i < n; ++i) strip_tf_.push_back(0);
  }
  if (tes_.stage) {
    tessellator_ = UniquePointer<graphics::Tessellator>::make(a_, a_);
    if (!tessellator_ || !tessellator_->valid()) return false;
  }
  return true;
}

// The snapshot from the stage's starting privates and the per-vertex inputs from the records (each `record_slots` slots).
bool StagePipeline::load_inputs(StageRun& run, ArrayView<const uint32_t* const> records) {
  ExtraStage& st = *run.stage;
  const uint32_t record_slots = &run == &gs_ && tes_.stage ? tes_.stage->record_slots : p_.vs_record_slots;
  ArrayView<uint32_t> snap = run.snapshot.view();
  for (uint64_t i = 0; i < snap.size() && i < st.initial.count(); ++i) snap[i] = st.initial[i];
  for (const StageCopy& c : st.inputs) {
    if (!c.per_vertex) continue;
    for (uint64_t v = 0; v < records.size(); ++v) {
      const ArrayView<const uint32_t> rec(records[v], record_slots);
      if (!copy_slots(snap, c.dst + v * c.dst_stride, rec, c.src, c.count)) break;      // (past the end of the array: the primitive has more vertices than it holds)
    }
  }
  return true;
}

void StagePipeline::to_vertex(const uint32_t* privates, gpu::RVertex* out) const {
  const uint32_t n = last_.private_slots;
  for (int i = 0; i < 4; ++i) out->pos[i] = has_pos_ && pos_slot_ + i < n ? u2f(privates[pos_slot_ + i]) : (i == 3 ? 1.0f : 0.0f);
  out->point_size = has_size_ && size_slot_ < n ? u2f(privates[size_slot_]) : 1.0f;
  out->edge = true;
  for (const VaryingLink& l : p_.links)
    for (uint32_t k = 0; k < l.count; ++k)
      if (l.vs_slot + k < n && l.var_offset + k < gpu::kMaxVarying) out->var[l.var_offset + k] = clamp_varying(l, desktop_.clamp_vertex_color, u2f(privates[l.vs_slot + k]));
  p_.fill_clip_distances(privates, n, desktop_.clip_planes, out->var);
  p_.fill_back_colors(privates, n, desktop_.clamp_vertex_color, out->var);
}

bool StagePipeline::gather(const ExtraStage& stage, const uint32_t* privates, uint32_t* record) const {
  const ArrayView<const uint32_t> src(privates, stage.module->private_slots);
  ArrayView<uint32_t> dst(record, stage.record_slots);
  for (const StageCopy& r : stage.record)
    if (!copy_slots(dst, r.dst, src, r.src, r.count)) return false;
  return true;
}

bool StagePipeline::run_one(Machine& m, const List<uint32_t>& snapshot, int32_t invocation_slot, uint32_t invocation) {
  uint32_t* pr = m.private_region();
  for (uint64_t i = 0; i < snapshot.count(); ++i) pr[i] = snapshot[i];
  if (invocation_slot >= 0 && static_cast<uint64_t>(invocation_slot) < snapshot.count()) pr[invocation_slot] = invocation;
  return m.start(m.module().main_function, ArrayView<const uint32_t* const>()) == Machine::Status::Done;
}

bool StagePipeline::primitive(ArrayView<const uint32_t* const> records, uint32_t primitive_id) {
  if (tes_.stage) return tessellate(records, primitive_id);
  if (gs_.stage) return geometry(records, primitive_id);
  return true;
}

// --- Geometry -------------------------------------------------------------------------------------------------------------------------------------------------

bool StagePipeline::geometry(ArrayView<const uint32_t* const> records, uint32_t primitive_id) {
  ExtraStage& st = *gs_.stage;
  const shader::Module& m = *st.module;
  if (!load_inputs(gs_, records)) return false;
  if (gs_.primitive_slot >= 0 && static_cast<uint64_t>(gs_.primitive_slot) < gs_.snapshot.count()) gs_.snapshot[static_cast<uint64_t>(gs_.primitive_slot)] = primitive_id;
  max_vertices_ = m.gs_max_vertices > 0 ? static_cast<uint32_t>(m.gs_max_vertices) : 0;
  output_ = m.gs_output;
  for (uint32_t inv = 0; inv < m.gs_invocations; ++inv) {
    strip_count_ = 0;
    emitted_ = 0;
    for (uint32_t& e : stream_emitted_) e = 0;
    if (!run_one(*st.machines[0], gs_.snapshot, gs_.invocation_slot, inv)) return false;
  }
  return true;
}

bool StagePipeline::emit_vertex(uint32_t stream) {
  const uint32_t* privates = gs_.stage->machines[0]->private_region();
  if (stream != 0) {      // (another stream: not rasterized; a point for transform feedback and the stream's queries)
    if (stream >= 4 || stream_emitted_[stream] >= max_vertices_) return true;
    ++stream_emitted_[stream];
    const uint32_t rs = tf_.record_slots();
    if (tf_.capturing() && !strip_tf_.empty()) {
      const ArrayView<uint32_t> scratch = strip_tf_.view().subview(uint64_t{2} * rs, rs);
      tf_.gather(ArrayView<const uint32_t>(privates, last_.private_slots), scratch);
      tf_.record(scratch);
    }
    tf_.primitive(stream);
    return true;
  }
  if (emitted_ >= max_vertices_) return true;      // (vertices past max_vertices are dropped)
  ++emitted_;
  gpu::RVertex v;
  to_vertex(privates, &v);
  // (transform feedback: the vertices of each primitive, in the order they are rasterized)
  // (transform feedback: the vertices of each primitive, in the order they are rasterized; records 0 and 1 the strip's, 2 the new vertex's)
  const bool tf = tf_.capturing() && !strip_tf_.empty();
  const uint32_t rs = tf_.record_slots();
  const ArrayView<uint32_t> recs = strip_tf_.view();
  if (tf) tf_.gather(ArrayView<const uint32_t>(privates, last_.private_slots), recs.subview(uint64_t{2} * rs, rs));
  const auto rec = [&](uint32_t k) { if (tf) tf_.record(recs.subview(uint64_t{k} * rs, rs)); };
  const auto keep = [&](uint32_t to, uint32_t from) { if (tf) recs.subview(uint64_t{to} * rs, rs).copy_from(recs.subview(uint64_t{from} * rs, rs)); };
  switch (output_) {
    case 0:
      rec(2);
      out_.point(v);
      break;
    case 3:
      if (strip_count_ >= 1) {
        rec(1);
        rec(2);
        out_.line(strip_[1], v, strip_count_ == 1);
      }
      strip_[1] = v;
      keep(1, 2);
      break;
    default:      // a triangle strip: every vertex after the second makes a triangle, every other one wound the other way, the new vertex last (it is the provoking one)
      if (strip_count_ >= 2) {
        const bool even = strip_count_ % 2 == 0;
        rec(even ? 0 : 1);
        rec(even ? 1 : 0);
        rec(2);
        if (even) out_.triangle(strip_[0], strip_[1], v);
        else out_.triangle(strip_[1], strip_[0], v);
      }
      strip_[0] = strip_[1];
      strip_[1] = v;
      keep(0, 1);
      keep(1, 2);
      break;
  }
  ++strip_count_;
  return true;
}

bool StagePipeline::end_primitive(uint32_t stream) {
  if (stream == 0) strip_count_ = 0;
  return true;
}

// --- Tessellation ---------------------------------------------------------------------------------------------------------------------------------------------

bool StagePipeline::tessellate(ArrayView<const uint32_t* const> records, uint32_t primitive_id) {
  float outer[4], inner[2];
  for (int i = 0; i < 4; ++i) outer[i] = desktop_.patch_outer[i];
  for (int i = 0; i < 2; ++i) inner[i] = desktop_.patch_inner[i];
  const uint32_t patch_vertices = static_cast<uint32_t>(records.size());
  ExtraStage& te = *tes_.stage;
  const shader::Module& tem = *te.module;

  // The control stage: one invocation per output vertex, run side by side so that each stops at a barrier() until all have reached it.
  if (tcs_.stage) {
    ExtraStage& tc = *tcs_.stage;
    const shader::Module& tcm = *tc.module;
    ArrayView<uint32_t> shared = tc.shared.view();
    for (const ExtraStage::Range& r : tc.output_slots)
      for (uint32_t k = 0; k < r.count; ++k)
        if (r.slot + k < shared.size()) shared[r.slot + k] = 0;
    if (!load_inputs(tcs_, records)) return false;
    ArrayView<uint32_t> snap = tcs_.snapshot.view();
    if (tcs_.vertices_slot >= 0 && static_cast<uint64_t>(tcs_.vertices_slot) < snap.size()) snap[static_cast<uint64_t>(tcs_.vertices_slot)] = patch_vertices;
    if (tcs_.primitive_slot >= 0 && static_cast<uint64_t>(tcs_.primitive_slot) < snap.size()) snap[static_cast<uint64_t>(tcs_.primitive_slot)] = primitive_id;
    const uint32_t invocations = static_cast<uint32_t>(tcm.tcs_vertices);
    Machine::Status status[32];
    for (uint32_t j = 0; j < invocations && j < 32; ++j) {
      Machine& mj = *tc.machines[j];
      uint32_t* pr = mj.private_region();
      for (uint64_t i = 0; i < snap.size(); ++i) pr[i] = snap[i];
      if (tcs_.invocation_slot >= 0 && static_cast<uint64_t>(tcs_.invocation_slot) < snap.size()) pr[tcs_.invocation_slot] = j;
      status[j] = mj.start(tcm.main_function, ArrayView<const uint32_t* const>());
    }
    for (uint32_t round = 0; round < 4096; ++round) {
      bool waiting = false;
      for (uint32_t j = 0; j < invocations && j < 32; ++j) {
        if (status[j] == Machine::Status::Failed || status[j] == Machine::Status::Discarded) return false;
        waiting = waiting || status[j] == Machine::Status::Yield;
      }
      if (!waiting) break;
      for (uint32_t j = 0; j < invocations && j < 32; ++j)
        if (status[j] == Machine::Status::Yield) status[j] = tc.machines[j]->resume();
    }
    for (uint32_t j = 0; j < invocations && j < 32; ++j)
      if (status[j] != Machine::Status::Done) return false;
    const int32_t o = slot_of(tcm, "gl_TessLevelOuter"), in = slot_of(tcm, "gl_TessLevelInner");
    for (int i = 0; i < 4; ++i) outer[i] = o >= 0 && static_cast<uint64_t>(o) + i < shared.size() ? u2f(shared[static_cast<uint64_t>(o) + i]) : 0.0f;
    for (int i = 0; i < 2; ++i) inner[i] = in >= 0 && static_cast<uint64_t>(in) + i < shared.size() ? u2f(shared[static_cast<uint64_t>(in) + i]) : 0.0f;
    // the evaluation stage's inputs: the patch's outputs, element by element
    ArrayView<uint32_t> tsnap = tes_.snapshot.view();
    for (uint64_t i = 0; i < tsnap.size() && i < te.initial.count(); ++i) tsnap[i] = te.initial[i];
    const ArrayView<const uint32_t> from = tc.shared.view();
    for (const StageCopy& c : te.inputs)
      for (uint32_t e = 0; e < c.elements; ++e)
        if (!copy_slots(tsnap, c.dst + uint64_t{e} * c.dst_stride, from, c.src + uint64_t{e} * c.src_stride, c.count)) break;
  } else {
    if (!load_inputs(tes_, records)) return false;
    // (no control stage: the levels of glPatchParameterfv)
    const int32_t o = slot_of(tem, "gl_TessLevelOuter"), in = slot_of(tem, "gl_TessLevelInner");
    ArrayView<uint32_t> tsnap = tes_.snapshot.view();
    for (int i = 0; i < 4; ++i) if (o >= 0 && static_cast<uint64_t>(o) + i < tsnap.size()) tsnap[static_cast<uint64_t>(o) + i] = f2u(outer[i]);
    for (int i = 0; i < 2; ++i) if (in >= 0 && static_cast<uint64_t>(in) + i < tsnap.size()) tsnap[static_cast<uint64_t>(in) + i] = f2u(inner[i]);
  }
  ArrayView<uint32_t> tsnap = tes_.snapshot.view();
  const uint32_t patch_in = tcs_.stage ? static_cast<uint32_t>(tcs_.stage->module->tcs_vertices) : patch_vertices;
  if (tes_.vertices_slot >= 0 && static_cast<uint64_t>(tes_.vertices_slot) < tsnap.size()) tsnap[static_cast<uint64_t>(tes_.vertices_slot)] = patch_in;
  if (tes_.primitive_slot >= 0 && static_cast<uint64_t>(tes_.primitive_slot) < tsnap.size()) tsnap[static_cast<uint64_t>(tes_.primitive_slot)] = primitive_id;

  // The primitive generator.
  const graphics::TessDomain domain = tem.tes_mode == 7 ? graphics::TessDomain::Quad : (tem.tes_mode == 0x8E7A ? graphics::TessDomain::Isoline : graphics::TessDomain::Triangle);
  const graphics::TessPartitioning partitioning = tem.tes_spacing == 0x8E7C   ? graphics::TessPartitioning::FractionalEven
                                                  : tem.tes_spacing == 0x8E7B ? graphics::TessPartitioning::FractionalOdd
                                                                              : graphics::TessPartitioning::Integer;
  const graphics::TessOutput output = tem.tes_point_mode                         ? graphics::TessOutput::Point
                                      : domain == graphics::TessDomain::Isoline ? graphics::TessOutput::Line
                                      : tem.tes_order == 0x0900                 ? graphics::TessOutput::TriangleCw
                                                                                : graphics::TessOutput::TriangleCcw;
  if (!tessellator_->tessellate(domain, partitioning, output, outer, inner)) return true;      // (culled: an outer level not above zero)
  const ArrayView<const graphics::Tessellator::Point> points = tessellator_->points();
  const ArrayView<const uint16_t> indices = tessellator_->indices();

  // The evaluation stage, once per point.
  const bool to_geometry = gs_.stage != nullptr;
  const uint32_t rs = te.record_slots;
  tes_records_.clear();
  tes_vertices_.clear();
  tes_tf_.clear();
  if (to_geometry ? !tes_records_.reserve(uint64_t{rs} * points.size()) : !tes_vertices_.reserve(points.size())) return false;
  const bool tf = !to_geometry && tf_.capturing();
  const uint32_t tfs = tf_.record_slots();
  if (tf && !tes_tf_.reserve(uint64_t{tfs} * points.size())) return false;
  Machine& tm = *te.machines[0];
  for (const graphics::Tessellator::Point& pt : points) {
    if (tes_.coord_slot >= 0 && static_cast<uint64_t>(tes_.coord_slot) + 2 < tsnap.size()) {
      const float w = domain == graphics::TessDomain::Triangle ? 1.0f - pt.u - pt.v : 0.0f;
      tsnap[static_cast<uint64_t>(tes_.coord_slot)] = f2u(pt.u);
      tsnap[static_cast<uint64_t>(tes_.coord_slot) + 1] = f2u(pt.v);
      tsnap[static_cast<uint64_t>(tes_.coord_slot) + 2] = f2u(w < 0 ? 0.0f : w);
    }
    if (!run_one(tm, tes_.snapshot, -1, 0)) return false;
    if (to_geometry) {
      const uint64_t at = tes_records_.count();
      for (uint32_t k = 0; k < rs; ++k) tes_records_.push_back(0);
      if (!gather(te, tm.private_region(), tes_records_.view().data() + at)) return false;
    } else {
      gpu::RVertex v;
      to_vertex(tm.private_region(), &v);
      tes_vertices_.push_back(v);
      if (tf) {
        const uint64_t at = tes_tf_.count();
        for (uint32_t k = 0; k < tfs; ++k) tes_tf_.push_back(0);
        tf_.gather(ArrayView<const uint32_t>(tm.private_region(), tem.private_slots), tes_tf_.view().subview(at, tfs));
      }
    }
  }
  const uint32_t per = output == graphics::TessOutput::Point ? 1 : (output == graphics::TessOutput::Line ? 2 : 3);
  for (uint64_t i = 0; i + per <= indices.size(); i += per) {
    bool ok = true;
    for (uint32_t k = 0; k < per; ++k) ok = ok && indices[i + k] < points.size();
    if (!ok) continue;
    if (to_geometry) {
      const uint32_t* recs[3];
      for (uint32_t k = 0; k < per; ++k) recs[k] = tes_records_.view().data() + uint64_t{indices[i + k]} * rs;
      if (!geometry(ArrayView<const uint32_t* const>(recs, per), gs_primitives_++)) return false;      // (the geometry stage counts the draw's tessellated primitives)
    } else {
      if (tf)
        for (uint32_t k = 0; k < per; ++k) tf_.record(ArrayView<const uint32_t>(tes_tf_.view()).subview(uint64_t{indices[i + k]} * tfs, tfs));
      if (per == 1) out_.point(tes_vertices_[indices[i]]);
      else if (per == 2) out_.line(tes_vertices_[indices[i]], tes_vertices_[indices[i + 1]], true);
      else out_.triangle(tes_vertices_[indices[i]], tes_vertices_[indices[i + 1]], tes_vertices_[indices[i + 2]]);
    }
  }
  return true;
}

}  // namespace webgl

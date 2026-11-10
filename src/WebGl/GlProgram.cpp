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

// Shaders and programs of the GL context: compiling, linking (attributes, varyings, uniforms, blocks), setting and querying uniforms.
#include "WebGl/GlContext.hpp"

namespace webgl {

namespace {
constexpr uint32_t INVALID_ENUM = 0x500, INVALID_VALUE = 0x501, INVALID_OPERATION = 0x502;
using shader::Base;
using shader::GlobalKind;
using shader::Type;

uint32_t gl_type_of(const Type& t) {
  if (t.base == Base::Sampler) {
    const bool shadow = t.shadow;
    if (t.texel == Base::Float) {
      switch (t.dim) {
        case shader::kDim2D: return shadow ? 0x8B62 : 0x8B5E;
        case shader::kDimCube: return shadow ? 0x8DC5 : 0x8B60;
        case shader::kDim3D: return 0x8B5F;
        default: return shadow ? 0x8DC4 : 0x8DC1;
      }
    }
    if (t.texel == Base::Int) {
      switch (t.dim) { case shader::kDim2D: return 0x8DCA; case shader::kDimCube: return 0x8DCC; case shader::kDim3D: return 0x8DCB; default: return 0x8DCF; }
    }
    switch (t.dim) { case shader::kDim2D: return 0x8DD2; case shader::kDimCube: return 0x8DD4; case shader::kDim3D: return 0x8DD3; default: return 0x8DD7; }
  }
  if (t.cols > 1) {
    if (t.cols == 2 && t.rows == 2) return 0x8B5A;
    if (t.cols == 3 && t.rows == 3) return 0x8B5B;
    if (t.cols == 4 && t.rows == 4) return 0x8B5C;
    if (t.cols == 2 && t.rows == 3) return 0x8B65;
    if (t.cols == 2 && t.rows == 4) return 0x8B66;
    if (t.cols == 3 && t.rows == 2) return 0x8B67;
    if (t.cols == 3 && t.rows == 4) return 0x8B68;
    if (t.cols == 4 && t.rows == 2) return 0x8B69;
    return 0x8B6A;
  }
  switch (t.base) {
    case Base::Float: return t.rows == 1 ? 0x1406 : 0x8B50 + t.rows - 2;
    case Base::Int: return t.rows == 1 ? 0x1404 : 0x8B53 + t.rows - 2;
    case Base::Uint: return t.rows == 1 ? 0x1405 : 0x8DC6 + t.rows - 2;
    case Base::Bool: return t.rows == 1 ? 0x8B56 : 0x8B57 + t.rows - 2;
    default: return 0;
  }
}
}  // namespace

uint32_t GlContext::uniform_gl_type(const Type& t) const { return gl_type_of(t); }

void GlContext::shader_source(uint32_t id, StringView src) {
  ShaderObj* s = shader_at(id);
  if (!s) { set_error(INVALID_VALUE); return; }
  s->source.assign(src);
}

static shader::Stage stage_of_shader(uint32_t type) {
  switch (type) {
    case 0x8B31: return shader::Stage::Vertex;
    case 0x8DD9: return shader::Stage::Geometry;
    case 0x8E88: return shader::Stage::TessControl;
    case 0x8E87: return shader::Stage::TessEval;
    default: return shader::Stage::Fragment;
  }
}

void GlContext::compile_shader(uint32_t id) {
  ShaderObj* s = shader_at(id);
  if (!s) { set_error(INVALID_VALUE); return; }
  UniquePointer<shader::Module> m = UniquePointer<shader::Module>::make(a_, a_);
  s->log.clear();
  s->compiled = false;
  if (!m) { s->log.assign("out of memory"); return; }
  shader::GlslOptions glsl;
  glsl.desktop = desktop.active;
  glsl.compile_only = desktop.active;      // (desktop GL: a shader may leave main or its functions' bodies to another shader of the program)
  const bool ok = shader::parse_glsl(*m, s->source.view(), stage_of_shader(s->type), glsl);
  if (ok && !es3_ && m->glsl_version >= 300) {
    s->log.assign("ERROR: 0:1: '#version 300 es' needs a WebGL 2 context");
  } else if (!ok) {
    s->log.assign("ERROR: ");
    s->log.append(m->error.view());
  } else {
    s->compiled = true;
    s->module = static_cast<UniquePointer<shader::Module>&&>(m);
  }
}

bool GlContext::shader_status(uint32_t id, uint32_t pname, double* out) {
  ShaderObj* s = shader_at(id);
  if (!s) { set_error(INVALID_VALUE); return false; }
  switch (pname) {
    case 0x8B4F: *out = s->type; return true;
    case 0x8B80: *out = s->deleted; return true;
    case 0x8B81: *out = s->compiled; return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

StringView GlContext::shader_log(uint32_t id) { ShaderObj* s = shader_at(id); return s ? s->log.view() : StringView(); }
StringView GlContext::shader_src(uint32_t id) { ShaderObj* s = shader_at(id); return s ? s->source.view() : StringView(); }

void GlContext::attach_shader(uint32_t program, uint32_t shader) {
  ProgramObj* p = program_at(program);
  ShaderObj* s = shader_at(shader);
  if (!p || !s) { set_error(INVALID_VALUE); return; }
  // The same shader twice is an error; so is a second shader of a stage, except in desktop GL.
  if (p->has_shader(shader) || (!desktop.active && p->has_stage(s->type))) { set_error(INVALID_OPERATION); return; }
  if (!p->shaders.push_back(ProgramObj::Attached{shader, s->type})) set_error(0x505);
}

void GlContext::detach_shader(uint32_t program, uint32_t shader) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return; }
  for (uint64_t i = 0; i < p->shaders.count(); ++i) {
    if (p->shaders[i].id != shader) continue;
    p->shaders.remove_at(i);
    release_if_unattached(shader);
    return;
  }
  set_error(INVALID_OPERATION);
}

// A shader deleted while attached goes once the last program lets it go.
void GlContext::release_if_unattached(uint32_t shader) {
  ShaderObj* s = shader_at(shader);
  if (!s || !s->deleted) return;
  for (auto& other : objects_->programs) if (other && other->has_shader(shader)) return;
  objects_->shaders[shader - 1].reset();
}

void GlContext::free_program(uint32_t id) {
  if (!id || id > objects_->programs.count() || !objects_->programs[id - 1]) return;
  List<uint32_t> attached(a_);
  for (const ProgramObj::Attached& s : objects_->programs[id - 1]->shaders) attached.push_back(s.id);
  objects_->programs[id - 1].reset();
  for (const uint32_t s : attached) release_if_unattached(s);
}

uint32_t GlContext::attached_shaders(uint32_t program, uint32_t* out, uint32_t max) {
  ProgramObj* p = program_at(program);
  uint32_t n = 0;
  if (!p) return 0;
  for (const ProgramObj::Attached& s : p->shaders) if (n < max) out[n++] = s.id;
  return n;
}

void GlContext::link_failed(ProgramObj* p, StringView why) {
  p->linked = false;
  p->log.assign("ERROR: ");
  p->log.append(why);
}

// `stage`: 0 vertex, 1 fragment, 2 + kStage* the others.
void GlContext::flatten_uniform(ProgramObj* p, const shader::Module& m, int stage, const shader::Global& g, const Type& t, StringView name, uint32_t slot) {
  if (t.array && t.base == Base::Struct) {
    const uint32_t es = m.slots_of(t.element());
    for (uint32_t i = 0; i < t.array; ++i) {
      String n(a_);
      n.assign(name);
      n.push_back('[');
      n.append_uint(i);
      n.push_back(']');
      flatten_uniform(p, m, stage, g, t.element(), n.view(), slot + i * es);
    }
    return;
  }
  if (t.base == Base::Struct) {
    const shader::StructDef* sd = m.struct_of(t);
    if (!sd) return;
    for (uint64_t i = 0; i < sd->fields.count(); ++i) {
      String n(a_);
      n.assign(name);
      n.push_back('.');
      n.append(sd->fields[i].name.view());
      flatten_uniform(p, m, stage, g, sd->fields[i].type, n.view(), slot + sd->fields[i].offset);
    }
    return;
  }
  String full(a_);
  full.assign(name);
  if (t.array) full.append("[0]");
  UniformLeaf* leaf = nullptr;
  for (uint64_t i = 0; i < p->uniforms.count(); ++i)
    if (p->uniforms[i]->name.view().equals(full.view())) { leaf = p->uniforms[i].get(); break; }
  if (!leaf) {
    UniquePointer<UniformLeaf> l = UniquePointer<UniformLeaf>::make(a_, a_);
    if (!l) return;
    l->name.assign(full.view());
    l->type = t.element();
    l->size = t.array ? t.array : 1;
    l->gl_type = gl_type_of(t.element());
    l->elem_slots = m.slots_of(t.element());
    leaf = l.get();
    p->uniforms.push_back(static_cast<UniquePointer<UniformLeaf>&&>(l));
  }
  if (stage == 0) leaf->vs_slot = static_cast<int32_t>(slot);
  else if (stage == 1) leaf->fs_slot = static_cast<int32_t>(slot);
  else leaf->extra_slot[stage - 2] = static_cast<int32_t>(slot);
}

bool GlContext::link_collect_uniforms(ProgramObj* p) {
  const shader::Module* mods[5] = {p->vsm.get(), p->fsm.get(), nullptr, nullptr, nullptr};
  for (uint32_t k = 0; k < 3; ++k) mods[2 + k] = p->extra[k] ? p->extra[k]->module.get() : nullptr;
  for (int s = 0; s < 5; ++s) {
    if (!mods[s]) continue;
    const shader::Module& m = *mods[s];
    for (uint64_t i = 0; i < m.globals.count(); ++i) {
      const shader::Global& g = *m.globals[i];
      if (g.kind != GlobalKind::Uniform || g.block >= 0) continue;
      flatten_uniform(p, m, s, g, g.type, g.name.view(), g.slot);
    }
  }
  // a uniform declared in both stages must have the same type
  for (uint64_t i = 0; i < p->uniforms.count(); ++i) {
    UniformLeaf& l = *p->uniforms[i];
    if (l.vs_slot >= 0 && l.fs_slot >= 0) {
      // (the first stage's type was recorded; compare through the global of the second)
    }
    l.location = static_cast<uint32_t>(p->locs.count());
    for (uint32_t e = 0; e < l.size; ++e) p->locs.push_back(ProgramObj::Loc{static_cast<uint32_t>(i), e});
  }
  return true;
}

// The geometry and tessellation stages of a program (desktop GL): parsed from the attached shaders of each, checked for the layouts they must declare, and joined to the stages
// around them (StageCopy). Fails the link (and says why) or returns true.
bool GlContext::link_extra_stages(ProgramObj* p) {
  static constexpr uint32_t kTypes[3] = {0x8DD9, 0x8E88, 0x8E87};
  static constexpr shader::Stage kStages[3] = {shader::Stage::Geometry, shader::Stage::TessControl, shader::Stage::TessEval};
  static const char* const kNames[3] = {"geometry shader: ", "tessellation control shader: ", "tessellation evaluation shader: "};
  for (UniquePointer<ExtraStage>& st : p->extra) st.reset();
  p->vs_record.clear();
  p->vs_record_slots = 0;
  shader::GlslOptions glsl;
  glsl.desktop = desktop.active;
  for (uint32_t k = 0; k < 3; ++k) {
    List<StringView> sources(a_);
    for (const ProgramObj::Attached& at : p->shaders) {
      if (at.type != kTypes[k]) continue;
      ShaderObj* s = shader_at(at.id);
      if (!s || !sources.push_back(s->source.view())) { link_failed(p, "out of memory"); return false; }
    }
    if (sources.empty()) continue;
    UniquePointer<ExtraStage> st = UniquePointer<ExtraStage>::make(a_, a_);
    if (!st) { link_failed(p, "out of memory"); return false; }
    st->module = UniquePointer<shader::Module>::make(a_, a_);
    if (!st->module) { link_failed(p, "out of memory"); return false; }
    if (!shader::parse_glsl_units(*st->module, static_cast<const List<StringView>&>(sources).view(), kStages[k], glsl)) {
      String why(a_);
      why.assign(kNames[k]);
      why.append(st->module->error.view());
      link_failed(p, why.view());
      return false;
    }
    p->extra[k] = static_cast<UniquePointer<ExtraStage>&&>(st);
  }
  ExtraStage* gs = p->extra[kStageGeometry].get();
  ExtraStage* tcs = p->extra[kStageTessControl].get();
  ExtraStage* tes = p->extra[kStageTessEval].get();
  if (!gs && !tcs && !tes) return true;
  if (tcs && !tes) { link_failed(p, "a tessellation control shader needs a tessellation evaluation shader"); return false; }
  if (gs) {
    const shader::Module& m = *gs->module;
    if (m.gs_input == 0xFFFFFFFFu) { link_failed(p, "the geometry shader declares no input primitive"); return false; }
    if (m.gs_output == 0xFFFFFFFFu) { link_failed(p, "the geometry shader declares no output primitive"); return false; }
    if (m.gs_max_vertices < 0) { link_failed(p, "the geometry shader declares no max_vertices"); return false; }
  }
  if (tcs && tcs->module->tcs_vertices <= 0) { link_failed(p, "the tessellation control shader declares no output patch size"); return false; }
  if (tes && !tes->module->tes_mode) { link_failed(p, "the tessellation evaluation shader declares no primitive mode"); return false; }
  // vertex -> control -> evaluation -> geometry: what each reads of the stage before
  const shader::Module* before = p->vsm.get();
  bool before_is_control = false;
  List<StageCopy>* before_record = &p->vs_record;
  uint32_t* before_slots = &p->vs_record_slots;
  ExtraStage* chain[3] = {tcs, tes, gs};
  for (uint32_t k = 0; k < 3; ++k) {
    ExtraStage* st = chain[k];
    if (!st) continue;
    if (!link_stage_inputs(p, *before, before_is_control, *st, before_record, before_slots, k == 0 ? kNames[1] : (k == 1 ? kNames[2] : kNames[0]))) return false;
    before = st->module.get();
    before_is_control = st == tcs;
    before_record = &st->record;
    before_slots = &st->record_slots;
  }
  if (tcs) {
    // (the control stage's outputs, cleared for each patch)
    const shader::Module& m = *tcs->module;
    for (uint64_t i = 0; i < m.globals.count(); ++i) {
      const shader::Global& g = *m.globals[i];
      if (g.kind == GlobalKind::Out && g.shared && !tcs->output_slots.push_back(ExtraStage::Range{g.slot, m.slots_of(g.type)})) { link_failed(p, "out of memory"); return false; }
    }
  }
  return true;
}

// Which of `producer`'s outputs feed each input of `consumer`, by name (gl_in[].gl_Position from gl_Position, or from the control stage's gl_out[]), and the record of the producer's
// outputs that a per-vertex producer keeps for each vertex.
bool GlContext::link_stage_inputs(ProgramObj* p, const shader::Module& producer, bool producer_is_control, ExtraStage& consumer, List<StageCopy>* record, uint32_t* record_slots,
                                  const char* name) {
  const shader::Module& cm = *consumer.module;
  String want(a_);
  for (uint64_t i = 0; i < cm.globals.count(); ++i) {
    const shader::Global& cg = *cm.globals[i];
    if (cg.kind != GlobalKind::In) continue;
    const StringView cn = cg.name.view();
    if (cn.starts_with("gl_in.")) {
      want.assign(producer_is_control ? "gl_out." : "");
      want.append(cn.substr(6, cn.size() - 6));
    } else {
      want.assign(cn);
    }
    const shader::Global* pg = nullptr;
    for (uint64_t k = producer.globals.count(); k > 0; --k) {      // (the last declaration: a redeclared gl_ClipDistance[n] over the built-in)
      const shader::Global& g = *producer.globals[k - 1];
      const bool output = g.kind == GlobalKind::Out || (g.kind == GlobalKind::Builtin && (g.builtin == shader::kBvPosition || g.builtin == shader::kBvPointSize || g.builtin == shader::kBvClipDistance)) ||
                          (g.kind == GlobalKind::Private && g.name.view().equals("gl_ClipVertex"));      // (a private of the compatibility profile)
      if (output && g.name.view().equals(want.view())) { pg = &g; break; }
    }
    if (!pg) {
      if (cg.referenced && !cn.starts_with("gl_")) {
        String why(a_);
        why.assign(name);
        why.append("the input ");
        why.append(cn);
        why.append(" is not written by the stage before");
        link_failed(p, why.view());
        return false;
      }
      continue;      // (zero)
    }
    // (per vertex: the element, or of an array of arrays the vertex's array)
    Type ct = cg.per_vertex ? cg.type.element() : cg.type;
    if (cg.per_vertex && cg.inner) ct.array = cg.inner;
    Type pt = producer_is_control && pg->per_vertex ? pg->type.element() : pg->type;
    if (producer_is_control && pg->per_vertex && pg->inner) pt.array = pg->inner;
    const bool clip_distances = want.view().ends_with("gl_ClipDistance");      // (sized differently on the two sides: the shorter is copied)
    if (!(ct == pt) && !(ct.base == Base::Struct && pt.base == Base::Struct) && !clip_distances) {
      String why(a_);
      why.assign(name);
      why.append("the type of ");
      why.append(cn);
      why.append(" does not match the stage before");
      link_failed(p, why.view());
      return false;
    }
    StageCopy c;
    c.dst = cg.slot;
    c.count = cm.slots_of(ct);
    if (clip_distances && producer.slots_of(pt) < c.count) c.count = producer.slots_of(pt);
    if (producer_is_control) {      // from the patch's outputs, element by element (or once, per patch)
      c.src = pg->slot;
      c.per_vertex = false;
      if (cg.per_vertex && pg->per_vertex) {
        c.src_stride = producer.slots_of(pt);
        c.dst_stride = cm.slots_of(ct);
        const uint32_t pv = pg->type.array / (pg->inner ? pg->inner : 1), cv = cg.type.array / (cg.inner ? cg.inner : 1);
        c.elements = pv < cv ? pv : cv;
      }
    } else {      // from each vertex's record
      if (!cg.per_vertex) continue;      // (a per-patch input needs a control stage: zero)
      uint32_t at = 0xFFFFFFFFu;
      for (const StageCopy& r : *record) if (r.src == pg->slot && r.count == c.count) at = r.dst;
      if (at == 0xFFFFFFFFu) {
        StageCopy r;
        r.src = pg->slot;
        r.dst = *record_slots;
        r.count = c.count;
        if (!record->push_back(r)) { link_failed(p, "out of memory"); return false; }
        at = *record_slots;
        *record_slots += c.count;
      }
      c.src = at;
      c.per_vertex = true;
      c.dst_stride = cm.slots_of(ct);
    }
    if (!consumer.inputs.push_back(c)) { link_failed(p, "out of memory"); return false; }
  }
  return true;
}

void GlContext::link_program(uint32_t id) {
  ProgramObj* p = program_at(id);
  if (!p) { set_error(INVALID_VALUE); return; }
  p->log.clear();
  p->linked = false;
  p->uniforms.clear();
  p->attribs.clear();
  p->links.clear();
  p->locs.clear();
  p->outputs.clear();
  p->nvar = 0;
  p->vmach.reset();
  p->release_accel();
  p->fmach.reset();
  // Every attached shader of a stage, in the order attached (each must have compiled).
  List<StringView> vs_sources(a_), fs_sources(a_);
  for (const ProgramObj::Attached& at : p->shaders) {
    ShaderObj* s = shader_at(at.id);
    if (!s || !s->compiled || !s->module) { link_failed(p, "attached shaders did not compile"); return; }
    if (at.type != 0x8B31 && at.type != 0x8B30) continue;      // (the geometry and tessellation stages: link_extra_stages)
    if (!(at.type == 0x8B31 ? vs_sources : fs_sources).push_back(s->source.view())) { link_failed(p, "out of memory"); return; }
  }
  // (desktop GL: a program with geometry or tessellation stages may leave out the fragment stage; its fragments' colours are undefined, here unwritten)
  const bool extra_stages = p->has_stage(0x8DD9) || p->has_stage(0x8E88) || p->has_stage(0x8E87);
  if (fs_sources.empty() && desktop.active && extra_stages && !fs_sources.push_back(StringView("#version 150\nvoid main() {}\n"))) { link_failed(p, "out of memory"); return; }
  if (vs_sources.empty() || fs_sources.empty()) { link_failed(p, "a program needs a vertex and a fragment shader"); return; }
  shader::GlslOptions link_glsl;
  link_glsl.desktop = desktop.active;
  // Each program owns its own parse of the shaders, so recompiling a shader later does not change it.
  p->vsm = UniquePointer<shader::Module>::make(a_, a_);
  p->fsm = UniquePointer<shader::Module>::make(a_, a_);
  if (!p->vsm || !p->fsm) { link_failed(p, "out of memory"); return; }
  if (!shader::parse_glsl_units(*p->vsm, static_cast<const List<StringView>&>(vs_sources).view(), shader::Stage::Vertex, link_glsl)) {
    String why(a_);
    why.assign("vertex shader: ");
    why.append(p->vsm->error.view());
    link_failed(p, why.view());
    return;
  }
  if (!shader::parse_glsl_units(*p->fsm, static_cast<const List<StringView>&>(fs_sources).view(), shader::Stage::Fragment, link_glsl)) {
    String why(a_);
    why.assign("fragment shader: ");
    why.append(p->fsm->error.view());
    link_failed(p, why.view());
    return;
  }
  if (!link_extra_stages(p)) return;
  const shader::Module& vm = *p->vsm;
  const shader::Module& fm = *p->fsm;
  const shader::Module& lm = p->last_module();      // (whose outputs the fragment stage reads)
  if (desktop.active && vm.desktop) {
    // A vertex shader must write gl_Position (GLSL 1.10 to 1.30: the linker says so).
    const int32_t pos = vm.find_builtin_global(shader::kBvPosition);
    if (!p->has_extra() && vm.desktop_version < 140 && (pos < 0 || !vm.globals[static_cast<uint64_t>(pos)]->referenced)) { link_failed(p, "the vertex shader does not write gl_Position"); return; }
    // A uniform both stages declare must have one type.
    for (uint64_t i = 0; i < vm.globals.count(); ++i) {
      const shader::Global& vg = *vm.globals[i];
      if (vg.kind != GlobalKind::Uniform || vg.block >= 0) continue;      // (members of blocks belong to their blocks: different blocks may use one name)
      for (uint64_t k = 0; k < fm.globals.count(); ++k) {
        const shader::Global& fg = *fm.globals[k];
        // (structure types are numbered per shader: those are not compared here)
        if (fg.kind == GlobalKind::Uniform && fg.block < 0 && fg.name.view().equals(vg.name.view()) && fg.type.base != Base::Struct && !(fg.type == vg.type)) { link_failed(p, "a uniform has different types in the two stages"); return; }
      }
    }
  }
  // (desktop GL links stages of different versions)
  if (!desktop.active && vm.glsl_version != fm.glsl_version) { link_failed(p, "vertex and fragment shaders must use the same GLSL version"); return; }

  // attributes
  uint32_t used = 0;
  for (uint64_t i = 0; i < vm.globals.count(); ++i) {
    const shader::Global& g = *vm.globals[i];
    if (g.kind != GlobalKind::In || g.builtin != shader::kBvNone) continue;
    UniquePointer<AttribInfo> ai = UniquePointer<AttribInfo>::make(a_, a_);
    if (!ai) { link_failed(p, "out of memory"); return; }
    ai->name.assign(g.name.view());
    ai->type = g.type.element();
    ai->size = g.type.array ? g.type.array : 1;
    ai->gl_type = gl_type_of(g.type.element());
    ai->slot = g.slot;
    ai->location = g.location;
    for (uint64_t b = 0; b < p->bound_attribs.count(); ++b)
      if (p->bound_attribs[b]->name.view().equals(g.name.view())) ai->location = p->bound_attribs[b]->loc;
    const uint32_t width = g.type.cols > 1 ? g.type.cols : 1;
    if (ai->location >= 0) {
      for (uint32_t c = 0; c < width; ++c) {
        if (ai->location + c >= (desktop.active ? kMaxAttribSlots : kMaxAttribs) || (used & (1u << (ai->location + c)))) { link_failed(p, "attribute locations collide"); return; }
        used |= 1u << (ai->location + c);
      }
    }
    p->attribs.push_back(static_cast<UniquePointer<AttribInfo>&&>(ai));
  }
  for (uint64_t i = 0; i < p->attribs.count(); ++i) {
    AttribInfo& ai = *p->attribs[i];
    if (ai.location >= 0) continue;
    const uint32_t width = ai.type.cols > 1 ? ai.type.cols : 1;
    for (uint32_t loc = 0; loc + width <= kMaxAttribs; ++loc) {
      uint32_t mask = 0;
      for (uint32_t c = 0; c < width; ++c) mask |= 1u << (loc + c);
      if (!(used & mask)) {
        used |= mask;
        ai.location = static_cast<int32_t>(loc);
        break;
      }
    }
    if (ai.location < 0) { link_failed(p, "too many attributes"); return; }
  }

  // varyings
  uint32_t offset = 0;
  for (uint64_t i = 0; i < fm.globals.count(); ++i) {
    const shader::Global& fg = *fm.globals[i];
    if (fg.kind != GlobalKind::In || fg.builtin != shader::kBvNone) continue;
    if (fg.name.view().equals("gl_ClipDistance")) continue;      // (a redeclared one: with the clip distances below)
    const shader::Global* match = nullptr;
    for (uint64_t k = 0; k < lm.globals.count(); ++k) {
      const shader::Global& vg = *lm.globals[k];
      if (vg.kind == GlobalKind::Out && vg.builtin == shader::kBvNone && vg.name.view().equals(fg.name.view())) { match = &vg; break; }
    }
    if (!match || (desktop.active && !match->referenced)) {
      // Desktop GL: a varying the fragment stage reads must be written by the vertex stage (the compatibility ones, tcos_*, may be left unwritten).
      if (desktop.active && fm.desktop && !fg.name.view().starts_with("tcos_") && fg.referenced) { link_failed(p, "a fragment shader input is not written by the vertex shader"); return; }
      if (match) {      // (declared, never written: its value is undefined; zero here)
        VaryingLink l;
        l.vs_slot = match->slot;
        l.fs_slot = fg.slot;
        l.count = fm.slots_of(fg.type);
        l.flat = true;
        l.var_offset = offset;
        offset += l.count;
        if (offset > gpu::kMaxVarying) { link_failed(p, "too many varyings"); return; }
        p->links.push_back(l);
      }
      continue;   // read but never written: stays zero
    }
    if (match->type != fg.type) { link_failed(p, "varying types do not match between the shaders"); return; }
    VaryingLink l;
    l.vs_slot = match->slot;
    l.fs_slot = fg.slot;
    l.count = fm.slots_of(fg.type);
    l.flat = fg.flat || match->flat || fg.type.base == Base::Int || fg.type.base == Base::Uint;
    l.var_offset = offset;
    offset += l.count;
    if (offset > gpu::kMaxVarying) { link_failed(p, "too many varyings"); return; }
    p->links.push_back(l);
  }
  // Clip distances: eight varyings from what the last pre-raster stage writes (a redeclared gl_ClipDistance[n] or the built-in one; a compatibility shader's gl_ClipVertex),
  // read by the fragment stage's gl_ClipDistance too.
  p->clip_varying = -1;
  p->clip_distance_slot = p->clip_vertex_slot = -1;
  p->clip_count = 0;
  if (desktop.active) {
    for (uint64_t k = lm.globals.count(); k > 0; --k) {
      const shader::Global& g = *lm.globals[k - 1];
      const bool distance = g.name.view().equals("gl_ClipDistance") && (g.kind == GlobalKind::Out || (g.kind == GlobalKind::Builtin && g.builtin == shader::kBvClipDistance));
      if (distance && g.referenced && g.type.array) {
        p->clip_distance_slot = static_cast<int32_t>(g.slot);
        p->clip_count = g.type.array < 8 ? g.type.array : 8;
        break;
      }
    }
    if (p->clip_distance_slot < 0) {
      const int32_t cv = lm.find_global("gl_ClipVertex");
      if (cv >= 0 && lm.globals[static_cast<uint64_t>(cv)]->referenced) p->clip_vertex_slot = static_cast<int32_t>(lm.globals[static_cast<uint64_t>(cv)]->slot);
      // (neither written, in the compatibility profile: gl_Position is compared with the planes, GLSL 1.30 section 7.1)
      const int32_t pos = lm.find_builtin_global(shader::kBvPosition);
      if (p->clip_vertex_slot < 0 && lm.compatibility && pos >= 0) p->clip_vertex_slot = static_cast<int32_t>(lm.globals[static_cast<uint64_t>(pos)]->slot);
    }
    if ((p->clip_distance_slot >= 0 || p->clip_vertex_slot >= 0) && offset + 8 <= gpu::kMaxVarying) {
      p->clip_varying = static_cast<int32_t>(offset);
      for (uint64_t k = fm.globals.count(); k > 0; --k) {
        const shader::Global& fg = *fm.globals[k - 1];
        if (!fg.name.view().equals("gl_ClipDistance") || !fg.referenced || !fg.type.array || (fg.kind != GlobalKind::In && fg.kind != GlobalKind::Builtin)) continue;
        VaryingLink l;
        l.vs_slot = p->clip_distance_slot >= 0 ? static_cast<uint32_t>(p->clip_distance_slot) : 0;
        l.fs_slot = fg.slot;
        l.count = fg.type.array < 8 ? fg.type.array : 8;
        l.flat = false;
        l.var_offset = offset;
        if (p->clip_distance_slot >= 0) p->links.push_back(l);
        break;
      }
      offset += 8;
    }
  }
  // gl_PrimitiveID: what the geometry stage writes, to the fragment stage (flat).
  if (p->extra[kStageGeometry]) {
    const int32_t fgi = fm.find_global("gl_PrimitiveID"), lgi = lm.find_global("gl_PrimitiveID");
    if (fgi >= 0 && lgi >= 0 && fm.globals[static_cast<uint64_t>(fgi)]->referenced && offset < gpu::kMaxVarying) {
      VaryingLink l;
      l.vs_slot = lm.globals[static_cast<uint64_t>(lgi)]->slot;
      l.fs_slot = fm.globals[static_cast<uint64_t>(fgi)]->slot;
      l.count = 1;
      l.flat = true;
      l.var_offset = offset++;
      p->links.push_back(l);
    }
  }
  p->nvar = offset;

  // fragment outputs
  p->frag_writes_all = false;
  for (uint64_t i = 0; i < fm.globals.count(); ++i) {
    const shader::Global& g = *fm.globals[i];
    if (g.kind == GlobalKind::Builtin && g.builtin == shader::kBvFragColor && g.referenced) {
      p->outputs.push_back(ProgramObj::Output{0, g.slot, g.type});
      p->frag_writes_all = true;
    } else if (g.kind == GlobalKind::Builtin && g.builtin == shader::kBvFragData && g.referenced) {
      for (uint32_t k = 0; k < g.type.array; ++k) p->outputs.push_back(ProgramObj::Output{static_cast<int32_t>(k), g.slot + 4 * k, shader::make_type(Base::Float, 4)});
    } else if (g.kind == GlobalKind::Out && g.builtin == shader::kBvNone) {
      const uint32_t cnt = g.type.array ? g.type.array : 1;
      for (uint32_t k = 0; k < cnt; ++k) p->outputs.push_back(ProgramObj::Output{(g.location < 0 ? 0 : g.location) + static_cast<int32_t>(k), g.slot + k * fm.slots_of(g.type.element()), g.type.element()});
    }
  }

  link_collect_uniforms(p);
  // uniform blocks: one binding per distinct name
  p->block_bindings.clear();
  p->for_each_block([&](uint32_t, const shader::Module& m, uint64_t b) {
    p->block_bindings.push_back(m.blocks[b]->binding >= 0 ? m.blocks[b]->binding : 0);
    return true;
  });
  p->vs_shared.clear();
  p->fs_shared.clear();
  for (uint32_t i = 0; i < (vm.shared_slots ? vm.shared_slots : 1); ++i) p->vs_shared.push_back(0);
  for (uint32_t i = 0; i < (fm.shared_slots ? fm.shared_slots : 1); ++i) p->fs_shared.push_back(0);
  for (const UniquePointer<ExtraStage>& st : p->extra) {
    if (!st) continue;
    st->shared.clear();
    const uint32_t n = st->module->shared_slots ? st->module->shared_slots : 1;
    if (!st->shared.reserve(n)) { link_failed(p, "out of memory"); return; }
    for (uint32_t i = 0; i < n; ++i) st->shared.push_back(0);
  }
  p->linked = true;
  p->log.assign("");
}

void GlContext::validate_program(uint32_t id) {
  ProgramObj* p = program_at(id);
  if (!p) { set_error(INVALID_VALUE); return; }
  p->validate_ok = p->linked;
}

bool GlContext::program_status(uint32_t id, uint32_t pname, double* out) {
  ProgramObj* p = program_at(id);
  if (!p) { set_error(INVALID_VALUE); return false; }
  switch (pname) {
    case 0x8B80: *out = p->deleted; return true;
    case 0x8B82: *out = p->linked; return true;
    case 0x8B83: *out = p->validate_ok; return true;
    case 0x8B85: *out = static_cast<double>(p->shaders.count()); return true;
    // geometry and tessellation (the linked program's)
    case 0x8916: case 0x8917: case 0x8918: case 0x887F: {      // GEOMETRY_VERTICES_OUT, _INPUT_TYPE, _OUTPUT_TYPE, _SHADER_INVOCATIONS
      if (!p->linked || !p->extra[kStageGeometry]) { set_error(INVALID_OPERATION); return false; }
      const shader::Module& m = *p->extra[kStageGeometry]->module;
      *out = pname == 0x8916 ? m.gs_max_vertices : (pname == 0x8917 ? m.gs_input : (pname == 0x8918 ? m.gs_output : m.gs_invocations));
      return true;
    }
    case 0x8E75: {      // TESS_CONTROL_OUTPUT_VERTICES
      if (!p->linked || !p->extra[kStageTessControl]) { set_error(INVALID_OPERATION); return false; }
      *out = p->extra[kStageTessControl]->module->tcs_vertices;
      return true;
    }
    case 0x8E76: case 0x8E77: case 0x8E78: case 0x8E79: {      // TESS_GEN_MODE, _SPACING, _VERTEX_ORDER, _POINT_MODE
      if (!p->linked || !p->extra[kStageTessEval]) { set_error(INVALID_OPERATION); return false; }
      const shader::Module& m = *p->extra[kStageTessEval]->module;
      *out = pname == 0x8E76 ? m.tes_mode : (pname == 0x8E77 ? (m.tes_spacing ? m.tes_spacing : 0x0202) : (pname == 0x8E78 ? (m.tes_order ? m.tes_order : 0x0901) : m.tes_point_mode));
      return true;
    }
    case 0x8B86: *out = p->uniforms.count(); return true;
    case 0x8B89: *out = p->attribs.count(); return true;
    case 0x8B87: { uint64_t m = 0; for (auto& u : p->uniforms) if (u->name.size() + 1 > m) m = u->name.size() + 1; *out = static_cast<double>(m); return true; }
    case 0x8B8A: { uint64_t m = 0; for (auto& u : p->attribs) if (u->name.size() + 1 > m) m = u->name.size() + 1; *out = static_cast<double>(m); return true; }
    case 0x8A36: *out = uniform_block_count(id); return true;
    case 0x8C83: *out = p->tf_varyings.count(); return true;
    case 0x8C7F: *out = p->tf_mode; return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

StringView GlContext::program_log(uint32_t id) { ProgramObj* p = program_at(id); return p ? p->log.view() : StringView(); }
const ProgramObj* GlContext::program(uint32_t id) const { return id && id <= objects_->programs.count() ? objects_->programs[id - 1].get() : nullptr; }

void GlContext::bind_attrib_location(uint32_t program, uint32_t index, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return; }
  if (index >= (desktop.active ? kMaxAttribSlots : kMaxAttribs)) { set_error(INVALID_VALUE); return; }
  if (name.starts_with("gl_")) { set_error(INVALID_OPERATION); return; }
  for (uint64_t i = 0; i < p->bound_attribs.count(); ++i)
    if (p->bound_attribs[i]->name.view().equals(name)) { p->bound_attribs[i]->loc = static_cast<int32_t>(index); return; }
  UniquePointer<ProgramObj::Bound> b = UniquePointer<ProgramObj::Bound>::make(a_, a_);
  if (!b) return;
  b->name.assign(name);
  b->loc = static_cast<int32_t>(index);
  p->bound_attribs.push_back(static_cast<UniquePointer<ProgramObj::Bound>&&>(b));
}

int32_t GlContext::get_attrib_location(uint32_t program, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return -1; }
  if (!p->linked) { set_error(INVALID_OPERATION); return -1; }
  for (uint64_t i = 0; i < p->attribs.count(); ++i)
    if (p->attribs[i]->name.view().equals(name)) return p->attribs[i]->location;
  return -1;
}

const UniformLeaf* GlContext::active_uniform(uint32_t program, uint32_t index) {
  ProgramObj* p = program_at(program);
  if (!p || index >= p->uniforms.count()) { set_error(INVALID_VALUE); return nullptr; }
  return p->uniforms[index].get();
}

const AttribInfo* GlContext::active_attrib(uint32_t program, uint32_t index) {
  ProgramObj* p = program_at(program);
  if (!p || index >= p->attribs.count()) { set_error(INVALID_VALUE); return nullptr; }
  return p->attribs[index].get();
}

int32_t GlContext::get_frag_data_location(uint32_t program, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked) { set_error(INVALID_OPERATION); return -1; }
  const shader::Module& fm = *p->fsm;
  for (uint64_t i = 0; i < fm.globals.count(); ++i) {
    const shader::Global& g = *fm.globals[i];
    if (g.kind == GlobalKind::Out && g.name.view().equals(name)) return g.location < 0 ? 0 : g.location;
  }
  return -1;
}

int32_t GlContext::get_uniform_location(uint32_t program, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return -1; }
  if (!p->linked) { set_error(INVALID_OPERATION); return -1; }
  // "name", "name[0]" and "name[3]": strip an array index and look for the "[0]" form
  uint32_t element = 0;
  String base(a_);
  base.assign(name);
  bool indexed = false;
  if (name.size() > 0 && name[name.size() - 1] == ']') {
    const uint64_t open = name.rfind('[');
    if (open != StringView::npos && open + 1 < name.size() - 1) {
      uint32_t v = 0;
      for (uint64_t i = open + 1; i + 1 < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        v = v * 10 + static_cast<uint32_t>(name[i] - '0');
      }
      element = v;
      base.assign(name.substr(0, open));
      indexed = true;
    }
  }
  for (uint64_t i = 0; i < p->uniforms.count(); ++i) {
    const UniformLeaf& l = *p->uniforms[i];
    StringView ln = l.name.view();
    const bool is_array = ln.size() > 3 && ln.substr(ln.size() - 3).equals("[0]");
    if (is_array) {
      const StringView lb = ln.substr(0, ln.size() - 3);
      if (lb.equals(base.view())) {
        if (element >= l.size) return -1;
        return static_cast<int32_t>(l.location + element);
      }
    } else if (!indexed && ln.equals(name)) {
      return static_cast<int32_t>(l.location);
    }
  }
  // struct members inside struct arrays carry their own "[i]" in the middle: exact match
  for (uint64_t i = 0; i < p->uniforms.count(); ++i)
    if (p->uniforms[i]->name.view().equals(name)) return static_cast<int32_t>(p->uniforms[i]->location);
  return -1;
}

void GlContext::uniform(int32_t location, uint32_t base_kind, uint32_t cols, uint32_t rows, uint32_t count, const uint32_t* lanes) {
  ProgramObj* p = program_at(program_);
  if (!p || !p->linked) { set_error(INVALID_OPERATION); return; }
  if (location < 0) return;
  if (static_cast<uint64_t>(location) >= p->locs.count()) { set_error(INVALID_OPERATION); return; }
  const ProgramObj::Loc loc = p->locs[static_cast<uint64_t>(location)];
  const UniformLeaf& l = *p->uniforms[loc.leaf];
  const Type& t = l.type;
  if (t.base == Base::Sampler) {
    if (base_kind != 'i' || cols != 1 || rows != 1) { set_error(INVALID_OPERATION); return; }
  } else {
    if (t.cols != cols || t.rows != rows) { set_error(INVALID_OPERATION); return; }
    bool ok;
    switch (t.base) {
      case Base::Float: ok = base_kind == 'f'; break;
      case Base::Int: ok = base_kind == 'i'; break;
      case Base::Uint: ok = base_kind == 'u'; break;
      case Base::Bool: ok = base_kind == 'i' || base_kind == 'f' || base_kind == 'u'; break;
      default: ok = false; break;
    }
    if (!ok) { set_error(INVALID_OPERATION); return; }
  }
  if (count > 1 && l.size == 1) { set_error(INVALID_OPERATION); return; }
  const uint32_t per = l.elem_slots;
  const uint32_t n = count < l.size - loc.element ? count : l.size - loc.element;
  for (uint32_t e = 0; e < n; ++e) {
    for (int stage = 0; stage < 5; ++stage) {
      const int32_t slot = stage == 0 ? l.vs_slot : (stage == 1 ? l.fs_slot : l.extra_slot[stage - 2]);
      if (slot < 0) continue;
      if (stage >= 2 && !p->extra[stage - 2]) continue;
      List<uint32_t>& store = stage == 0 ? p->vs_shared : (stage == 1 ? p->fs_shared : p->extra[stage - 2]->shared);
      for (uint32_t k = 0; k < per; ++k) {
        uint32_t v = lanes[e * per + k];
        if (t.base == Base::Bool) v = base_kind == 'f' ? (__builtin_bit_cast(float, v) != 0.0f) : (v != 0);
        store[static_cast<uint64_t>(slot) + (loc.element + e) * per + k] = v;
      }
    }
  }
}

bool GlContext::get_uniform(uint32_t program, int32_t location, double* out, uint32_t* count) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked) { set_error(INVALID_OPERATION); return false; }
  if (location < 0 || static_cast<uint64_t>(location) >= p->locs.count()) { set_error(INVALID_OPERATION); return false; }
  const ProgramObj::Loc loc = p->locs[static_cast<uint64_t>(location)];
  const UniformLeaf& l = *p->uniforms[loc.leaf];
  int32_t slot = l.vs_slot >= 0 ? l.vs_slot : l.fs_slot;
  const List<uint32_t>* store_ptr = l.vs_slot >= 0 ? &p->vs_shared : &p->fs_shared;
  for (uint32_t k = 0; slot < 0 && k < 3; ++k)
    if (p->extra[k] && l.extra_slot[k] >= 0) { slot = l.extra_slot[k]; store_ptr = &p->extra[k]->shared; }
  if (slot < 0) { set_error(INVALID_OPERATION); return false; }
  const List<uint32_t>& store = *store_ptr;
  *count = l.elem_slots;
  for (uint32_t k = 0; k < l.elem_slots && k < 16; ++k) {
    const uint32_t v = store[static_cast<uint64_t>(slot) + loc.element * l.elem_slots + k];
    switch (l.type.base) {
      case Base::Float: out[k] = __builtin_bit_cast(float, v); break;
      case Base::Int: out[k] = static_cast<int32_t>(v); break;
      default: out[k] = v; break;
    }
  }
  return true;
}

uint32_t GlContext::uniform_block_count(uint32_t program) const {
  const ProgramObj* p = this->program(program);
  return p && p->linked ? static_cast<uint32_t>(p->block_bindings.count()) : 0;
}

uint32_t GlContext::get_uniform_block_index(uint32_t program, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked) { set_error(INVALID_OPERATION); return 0xFFFFFFFFu; }
  uint32_t found = 0xFFFFFFFFu;
  p->for_each_block([&](uint32_t idx, const shader::Module& m, uint64_t b) {
    if (m.blocks[b]->name.view().equals(name)) found = idx;
    return found == 0xFFFFFFFFu;
  });
  return found;
}

StringView GlContext::uniform_block_name(uint32_t program, uint32_t block) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked) return StringView();
  StringView found;
  p->for_each_block([&](uint32_t idx, const shader::Module& m, uint64_t b) {
    if (idx == block) found = m.blocks[b]->name.view();
    return idx != block;
  });
  return found;
}

void GlContext::uniform_block_binding(uint32_t program, uint32_t block, uint32_t binding) {
  ProgramObj* p = program_at(program);
  if (!p || block >= p->block_bindings.count() || binding >= kMaxUniformBindings) { set_error(INVALID_VALUE); return; }
  p->block_bindings[block] = static_cast<int32_t>(binding);
}

bool GlContext::uniform_block_info(uint32_t program, uint32_t block, uint32_t pname, double* out) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked || block >= p->block_bindings.count()) { set_error(INVALID_VALUE); return false; }
  const StringView name = uniform_block_name(program, block);
  uint32_t size = 0, members = 0;
  bool in_stage[5] = {};
  for (int s = 0; s < 5; ++s) {
    if (!p->stage_module(s)) continue;
    const shader::Module& m = *p->stage_module(s);
    for (uint64_t b = 0; b < m.blocks.count(); ++b) {
      if (!m.blocks[b]->name.view().equals(name)) continue;
      in_stage[s] = true;
      uint32_t off = 0, cnt = 0;
      for (uint64_t g = 0; g < m.globals.count(); ++g) {
        if (m.globals[g]->block != static_cast<int32_t>(b)) continue;
        uint32_t al, sz;
        shader::layout_of(m, m.globals[g]->type, shader::Layout::Std140, &al, &sz);
        off = (off + al - 1) / al * al + sz;
        ++cnt;
      }
      size = (off + 15) / 16 * 16;
      members = cnt;
    }
  }
  switch (pname) {
    case 0x8A3F: *out = p->block_bindings[block]; return true;      // UNIFORM_BLOCK_BINDING
    case 0x8A40: *out = size; return true;                          // _DATA_SIZE
    case 0x8A41: *out = static_cast<double>(name.size() + 1); return true;      // _NAME_LENGTH
    case 0x8A42: *out = members; return true;                       // _ACTIVE_UNIFORMS
    case 0x8A44: *out = in_stage[0]; return true;                   // _REFERENCED_BY_VERTEX_SHADER
    case 0x8A45: *out = in_stage[2 + kStageGeometry]; return true;  // _REFERENCED_BY_GEOMETRY_SHADER
    case 0x8A46: *out = in_stage[1]; return true;                   // _REFERENCED_BY_FRAGMENT_SHADER
    case 0x84F0: *out = in_stage[2 + kStageTessControl]; return true;
    case 0x84F1: *out = in_stage[2 + kStageTessEval]; return true;
    default: set_error(INVALID_ENUM); return false;
  }
}

void GlContext::transform_feedback_varyings(uint32_t program, ArrayView<const StringView> names, uint32_t mode) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return; }
  p->tf_varyings.clear();
  for (const StringView& n : names) {
    UniquePointer<String> s = UniquePointer<String>::make(a_, a_);
    if (!s) return;
    s->assign(n);
    p->tf_varyings.push_back(static_cast<UniquePointer<String>&&>(s));
  }
  p->tf_mode = mode;
}

void GlContext::begin_transform_feedback(uint32_t) {
  TransformFeedbackObj* tf = tf_bound_ ? tfs_[tf_bound_ - 1].get() : &default_tf_;
  tf->active = true;
  tf_active_ = true;
  for (auto& v : tf_pos_) v = 0;
}
void GlContext::end_transform_feedback() {
  TransformFeedbackObj* tf = tf_bound_ ? tfs_[tf_bound_ - 1].get() : &default_tf_;
  tf->active = false;
  tf_active_ = false;
}

// Appends the captured varyings of one vertex (the vertex shader's private slots) to the bound feedback buffers.
void GlContext::tf_capture(const ProgramObj& p, const uint32_t* priv) {
  const bool separate = p.tf_mode == 0x8C8D;
  for (uint64_t i = 0; i < p.tf_varyings.count(); ++i) {
    const StringView name = p.tf_varyings[i]->view();
    const shader::Global* g = nullptr;
    for (uint64_t k = 0; k < p.vsm->globals.count() && !g; ++k) {
      const shader::Global& c = *p.vsm->globals[k];
      if (c.kind != shader::GlobalKind::Out && !(c.kind == shader::GlobalKind::Builtin && c.builtin == shader::kBvPosition)) continue;
      if (c.name.view().equals(name)) g = &c;
    }
    if (!g) continue;
    const uint32_t binding = separate ? static_cast<uint32_t>(i) : 0;
    if (binding >= 4) continue;
    const BufferBinding& bb = tf_bindings_[binding];
    Buffer* b = buffer_at(bb.buffer);
    if (!b) continue;
    const uint32_t n = p.vsm->slots_of(g->type);
    const uint64_t limit = bb.size ? bb.offset + bb.size : b->data.size();
    ByteView bytes = b->data.writable_view();
    for (uint32_t k = 0; k < n; ++k) {
      const uint64_t at = bb.offset + tf_pos_[binding];
      if (at + 4 > limit || at + 4 > bytes.size()) return;
      const uint32_t v = priv[g->slot + k];
      __builtin_memcpy(bytes.data() + at, &v, 4);
      tf_pos_[binding] += 4;
    }
  }
}

}  // namespace webgl

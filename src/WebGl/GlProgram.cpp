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

void GlContext::compile_shader(uint32_t id) {
  ShaderObj* s = shader_at(id);
  if (!s) { set_error(INVALID_VALUE); return; }
  UniquePointer<shader::Module> m = UniquePointer<shader::Module>::make(a_, a_);
  s->log.clear();
  s->compiled = false;
  if (!m) { s->log.assign("out of memory"); return; }
  const bool ok = shader::parse_glsl(*m, s->source.view(), s->type == 0x8B31 ? shader::Stage::Vertex : shader::Stage::Fragment);
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
  uint32_t& slot = s->type == 0x8B31 ? p->vs : p->fs;
  if (slot) { set_error(INVALID_OPERATION); return; }
  slot = shader;
}

void GlContext::detach_shader(uint32_t program, uint32_t shader) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return; }
  if (p->vs == shader) p->vs = 0;
  else if (p->fs == shader) p->fs = 0;
  else set_error(INVALID_OPERATION);
}

uint32_t GlContext::attached_shaders(uint32_t program, uint32_t* out, uint32_t max) {
  ProgramObj* p = program_at(program);
  uint32_t n = 0;
  if (!p) return 0;
  if (p->vs && n < max) out[n++] = p->vs;
  if (p->fs && n < max) out[n++] = p->fs;
  return n;
}

void GlContext::link_failed(ProgramObj* p, StringView why) {
  p->linked = false;
  p->log.assign("ERROR: ");
  p->log.append(why);
}

void GlContext::flatten_uniform(ProgramObj* p, const shader::Module& m, bool is_vs, const shader::Global& g, const Type& t, StringView name, uint32_t slot) {
  if (t.array && t.base == Base::Struct) {
    const uint32_t es = m.slots_of(t.element());
    for (uint32_t i = 0; i < t.array; ++i) {
      String n(a_);
      n.assign(name);
      n.push_back('[');
      n.append_uint(i);
      n.push_back(']');
      flatten_uniform(p, m, is_vs, g, t.element(), n.view(), slot + i * es);
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
      flatten_uniform(p, m, is_vs, g, sd->fields[i].type, n.view(), slot + sd->fields[i].offset);
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
  (is_vs ? leaf->vs_slot : leaf->fs_slot) = static_cast<int32_t>(slot);
}

bool GlContext::link_collect_uniforms(ProgramObj* p) {
  const shader::Module* mods[2] = {p->vsm.get(), p->fsm.get()};
  for (int s = 0; s < 2; ++s) {
    const shader::Module& m = *mods[s];
    for (uint64_t i = 0; i < m.globals.count(); ++i) {
      const shader::Global& g = *m.globals[i];
      if (g.kind != GlobalKind::Uniform || g.block >= 0) continue;
      flatten_uniform(p, m, s == 0, g, g.type, g.name.view(), g.slot);
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
  p->fmach.reset();
  ShaderObj* vs = shader_at(p->vs);
  ShaderObj* fs = shader_at(p->fs);
  if (!vs || !fs) { link_failed(p, "a program needs a vertex and a fragment shader"); return; }
  if (!vs->compiled || !fs->compiled || !vs->module || !fs->module) { link_failed(p, "attached shaders did not compile"); return; }
  // Each program owns its own parse of the shaders, so recompiling a shader later does not change it.
  p->vsm = UniquePointer<shader::Module>::make(a_, a_);
  p->fsm = UniquePointer<shader::Module>::make(a_, a_);
  if (!p->vsm || !p->fsm || !shader::parse_glsl(*p->vsm, vs->source.view(), shader::Stage::Vertex) || !shader::parse_glsl(*p->fsm, fs->source.view(), shader::Stage::Fragment)) {
    link_failed(p, "could not prepare the shaders");
    return;
  }
  const shader::Module& vm = *p->vsm;
  const shader::Module& fm = *p->fsm;
  if (vm.glsl_version != fm.glsl_version) { link_failed(p, "vertex and fragment shaders must use the same GLSL version"); return; }

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
        if (ai->location + c >= kMaxAttribs || (used & (1u << (ai->location + c)))) { link_failed(p, "attribute locations collide"); return; }
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
    const shader::Global* match = nullptr;
    for (uint64_t k = 0; k < vm.globals.count(); ++k) {
      const shader::Global& vg = *vm.globals[k];
      if (vg.kind == GlobalKind::Out && vg.builtin == shader::kBvNone && vg.name.view().equals(fg.name.view())) { match = &vg; break; }
    }
    if (!match) continue;   // read but never written: stays zero
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
  for (int s = 0; s < 2; ++s) {
    const shader::Module& m = s == 0 ? vm : fm;
    for (uint64_t b = 0; b < m.blocks.count(); ++b) {
      bool seen = false;
      if (s == 1)
        for (uint64_t k = 0; k < vm.blocks.count(); ++k) seen = seen || vm.blocks[k]->name.view().equals(m.blocks[b]->name.view());
      if (!seen) p->block_bindings.push_back(m.blocks[b]->binding >= 0 ? m.blocks[b]->binding : 0);
    }
  }
  p->vs_shared.clear();
  p->fs_shared.clear();
  for (uint32_t i = 0; i < (vm.shared_slots ? vm.shared_slots : 1); ++i) p->vs_shared.push_back(0);
  for (uint32_t i = 0; i < (fm.shared_slots ? fm.shared_slots : 1); ++i) p->fs_shared.push_back(0);
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
    case 0x8B85: *out = (p->vs ? 1 : 0) + (p->fs ? 1 : 0); return true;
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
const ProgramObj* GlContext::program(uint32_t id) const { return id && id <= programs_.count() ? programs_[id - 1].get() : nullptr; }

void GlContext::bind_attrib_location(uint32_t program, uint32_t index, StringView name) {
  ProgramObj* p = program_at(program);
  if (!p) { set_error(INVALID_VALUE); return; }
  if (index >= kMaxAttribs) { set_error(INVALID_VALUE); return; }
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
    for (int stage = 0; stage < 2; ++stage) {
      const int32_t slot = stage == 0 ? l.vs_slot : l.fs_slot;
      if (slot < 0) continue;
      List<uint32_t>& store = stage == 0 ? p->vs_shared : p->fs_shared;
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
  const int32_t slot = l.vs_slot >= 0 ? l.vs_slot : l.fs_slot;
  const List<uint32_t>& store = l.vs_slot >= 0 ? p->vs_shared : p->fs_shared;
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
  uint32_t idx = 0;
  for (uint64_t b = 0; b < p->vsm->blocks.count(); ++b, ++idx)
    if (p->vsm->blocks[b]->name.view().equals(name)) return idx;
  for (uint64_t b = 0; b < p->fsm->blocks.count(); ++b) {
    bool seen = false;
    for (uint64_t k = 0; k < p->vsm->blocks.count(); ++k) seen = seen || p->vsm->blocks[k]->name.view().equals(p->fsm->blocks[b]->name.view());
    if (seen) continue;
    if (p->fsm->blocks[b]->name.view().equals(name)) return idx;
    ++idx;
  }
  return 0xFFFFFFFFu;
}

StringView GlContext::uniform_block_name(uint32_t program, uint32_t block) {
  ProgramObj* p = program_at(program);
  if (!p || !p->linked) return StringView();
  uint32_t idx = 0;
  for (uint64_t b = 0; b < p->vsm->blocks.count(); ++b, ++idx)
    if (idx == block) return p->vsm->blocks[b]->name.view();
  for (uint64_t b = 0; b < p->fsm->blocks.count(); ++b) {
    bool seen = false;
    for (uint64_t k = 0; k < p->vsm->blocks.count(); ++k) seen = seen || p->vsm->blocks[k]->name.view().equals(p->fsm->blocks[b]->name.view());
    if (seen) continue;
    if (idx == block) return p->fsm->blocks[b]->name.view();
    ++idx;
  }
  return StringView();
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
  const shader::Module* mods[2] = {p->vsm.get(), p->fsm.get()};
  uint32_t size = 0, members = 0;
  bool in_vs = false, in_fs = false;
  for (int s = 0; s < 2; ++s) {
    const shader::Module& m = *mods[s];
    for (uint64_t b = 0; b < m.blocks.count(); ++b) {
      if (!m.blocks[b]->name.view().equals(name)) continue;
      (s == 0 ? in_vs : in_fs) = true;
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
    case 0x8A44: *out = p->block_bindings[block]; return true;
    case 0x8A40: *out = size; return true;
    case 0x8A42: *out = members; return true;
    case 0x8A46: *out = in_vs; return true;
    case 0x8A47: *out = in_fs; return true;
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

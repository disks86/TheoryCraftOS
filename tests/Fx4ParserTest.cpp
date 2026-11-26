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

// d3d10.dll's reader of Direct3D 10 effects (src/Windows/d3d10/fx4.h): what it reads from real fx_4_0 / fx_4_1 binaries (tests/data/fx, compiled with Microsoft's
// d3dcompiler_47: names, types, default values, annotations, state assignments, expressions and their values), its math against the C library's, and every prefix and
// every one-bit change of each binary - a binary that is read is then walked through all of it (defaults unpacked, expressions run) under ASan / UBSan, in buffers of
// exactly their size: nothing may be read or written outside.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma GCC diagnostic ignored "-Wunused-function"      /* (the headers are shared with d3d10.dll: this uses part of them) */
#define DXD_REALLOC(p, n) std::realloc((p), (n))
#define DXD_FREE(p) std::free(p)
#include "Windows/d3d10/fx4.h"
#include "Windows/d3d10/fx4_disasm.h"

static int g_failures, g_checks;
#define CHECK(c)                                                                         \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

static void *test_alloc(void *, size_t n) { return std::calloc(1, n); }
static void test_release(void *, void *p) { std::free(p); }
static const Fx4Alloc g_alloc = {test_alloc, test_release, nullptr};

static std::vector<uint8_t> load(const char *name) {
  std::ifstream f(std::string("tests/data/fx/") + name, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool same(const char *a, const char *b) { return a && b && !std::strcmp(a, b); }
static float f32(const uint8_t *p) { float f; std::memcpy(&f, p, 4); return f; }
static uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

/* An expression run with its tables (the constant buffer from `cb`: floats at the registers' places), the result's first value. */
static std::vector<float> run(const Fx4Preshader &p, const std::vector<float> &cb) {
  std::vector<float> constants(p.literal_count + 1), cbt(p.table_size[FX4_TABLE_CB] + 1), result(p.table_size[FX4_TABLE_RESULT] + 1),
      temp(p.table_size[FX4_TABLE_TEMP] + 1);
  for (uint32_t i = 0; i < p.literal_count; ++i) constants[i] = f32(p.literals + 4 * i);
  for (size_t i = 0; i < cb.size() && i < cbt.size(); ++i) cbt[i] = cb[i];
  float *tables[FX4_TABLES] = {};
  tables[FX4_TABLE_CONSTANTS] = constants.data();
  tables[FX4_TABLE_CB] = cbt.data();
  tables[FX4_TABLE_RESULT] = result.data();
  tables[FX4_TABLE_TEMP] = temp.data();
  fx4_preshader_eval(&p, tables);
  result.resize(p.table_size[FX4_TABLE_RESULT]);
  return result;
}

/* Everything of a description used as d3d10.dll uses it: defaults unpacked into memory of exactly the type's footprint, every expression run in tables of exactly the
 * sizes it asks for. (ASan finds what reaches past them.) */
static void walk_expression(const Fx4Preshader &p) {
  float *tables[FX4_TABLES] = {};
  std::vector<float *> owned;
  for (uint32_t t : {FX4_TABLE_CONSTANTS, FX4_TABLE_CB, FX4_TABLE_RESULT, FX4_TABLE_TEMP}) {
    const uint32_t n = t == FX4_TABLE_CONSTANTS ? p.literal_count : p.table_size[t];
    tables[t] = static_cast<float *>(std::calloc(n ? n : 1, sizeof(float)));
    owned.push_back(tables[t]);
  }
  fx4_preshader_eval(&p, tables);
  for (float *f : owned) std::free(f);
}
static void walk_assignments(const Fx4Assignments &list) {
  for (uint32_t i = 0; i < list.count; ++i) {
    const Fx4Assignment &a = list.items[i];
    if (a.op == FX4_OP_INDEX_EXPRESSION || a.op == FX4_OP_VALUE_EXPRESSION) walk_expression(a.expression);
  }
}
static void walk_annotations(const Fx4Annotations &a) {
  for (uint32_t i = 0; i < a.count; ++i)
    if (a.items[i].values) {
      const uint32_t reach = fx4_footprint(a.items[i].type, 0);
      if (reach == 0xFFFFFFFFu) continue;
      uint8_t *mem = static_cast<uint8_t *>(std::calloc(reach ? reach : 1, 1));
      fx4_unpack(a.items[i].type, a.items[i].values, a.items[i].type->packed, mem, 0);
      std::free(mem);
    }
}
static void walk(const Fx4Effect &e) {
  for (uint32_t b = 0; b < e.buffer_count; ++b) {
    const Fx4Buffer &buf = e.buffers[b];
    uint8_t *mem = static_cast<uint8_t *>(std::calloc(buf.size ? buf.size : 1, 1));
    walk_annotations(buf.annotations);
    for (uint32_t v = 0; v < buf.count; ++v) {
      const Fx4Numeric &n = buf.vars[v];
      walk_annotations(n.annotations);
      if (n.defaults) fx4_unpack(n.type, n.defaults, static_cast<uint32_t>(fx4_packed_bytes(n.type, 0)), mem + n.offset, 0);
    }
    std::free(mem);
  }
  for (uint32_t o = 0; o < e.object_count; ++o) {
    walk_annotations(e.objects[o].annotations);
    for (uint32_t k = 0; k < e.objects[o].element_count; ++k) walk_assignments(e.objects[o].elements[k].state);
  }
  for (uint32_t t = 0; t < e.technique_count; ++t) {
    walk_annotations(e.techniques[t].annotations);
    for (uint32_t p = 0; p < e.techniques[t].pass_count; ++p) {
      walk_annotations(e.techniques[t].passes[p].annotations);
      walk_assignments(e.techniques[t].passes[p].state);
    }
  }
  /* (and its listing, plain and as HTML, every shader in it listed too) */
  for (int html = 0; html < 2; ++html) {
    DxdText text = {nullptr, 0, 0, 0, html};
    if (!fx4_disassemble(&text, &e, 0)) { ++g_failures; std::printf("FAIL: listing\n"); }
    std::free(text.data);
  }
}
/* A parse of exactly these bytes (a heap copy of their size); walked when it is read. */
static int parse_exact(const uint8_t *data, size_t size, int allow_10_1) {
  uint8_t *copy = static_cast<uint8_t *>(std::malloc(size ? size : 1));
  std::memcpy(copy, data, size);
  Fx4Effect e;
  const int status = fx4_parse(copy, size, allow_10_1, &g_alloc, &e);
  if (status == FX4_OK) {
    walk(e);
    fx4_free(&e);
  }
  std::free(copy);
  return status;
}

static void test_annotations() {
  const std::vector<uint8_t> d = load("annotations.fxo");
  CHECK(!d.empty());
  Fx4Effect e;
  CHECK(fx4_parse(d.data(), d.size(), 0, &g_alloc, &e) == FX4_OK);
  CHECK(e.version == FX4_VERSION_4_0);
  CHECK(e.buffer_count == 1 && e.object_count == 5 && e.technique_count == 1 && e.numeric_count == 3 && e.string_count == 4);
  const Fx4Buffer &b = e.buffers[0];
  CHECK(same(b.name, "$Globals") && b.count == 3 && b.size == 0x70 && !b.tbuffer && b.bind == 0xFFFFFFFFu);
  const Fx4Numeric &color = b.vars[0];
  CHECK(same(color.name, "color") && same(color.semantic, "COLOR") && color.offset == 0 && color.defaults);
  CHECK(color.type->kind == FX4_KIND_NUMERIC && color.type->numeric_class == FX4_CLASS_VECTOR && color.type->columns == 4 && color.type->rows == 1);
  uint8_t mem[0x70] = {};
  fx4_unpack(color.type, color.defaults, 16, mem, 0);
  CHECK(f32(mem) == 1.0f && f32(mem + 4) == 0.5f && f32(mem + 8) == 0.25f && f32(mem + 12) == 1.0f);
  const Fx4Numeric &arr = b.vars[1];
  CHECK(same(arr.name, "arr") && arr.offset == 0x10 && arr.type->elements == 3 && arr.type->stride == 16 && arr.type->unpacked == 36);
  fx4_unpack(arr.type, arr.defaults, 12, mem + arr.offset, 0);
  CHECK(f32(mem + 0x10) == 1.0f && f32(mem + 0x20) == 2.0f && f32(mem + 0x30) == 3.0f);
  CHECK(arr.annotations.count == 4);
  const Fx4Annotation *an = arr.annotations.items;
  CHECK(same(an[0].name, "desc") && an[0].strings && same(an[0].strings[0], "array"));
  CHECK(same(an[1].name, "n") && an[1].values && u32(an[1].values) == 7);
  CHECK(same(an[2].name, "v") && an[2].values && f32(an[2].values) == 1.0f && f32(an[2].values + 4) == 2.0f);
  CHECK(same(an[3].name, "names") && an[3].type->elements == 2 && same(an[3].strings[0], "a") && same(an[3].strings[1], "bb"));
  const Fx4Numeric &m = b.vars[2];
  CHECK(same(m.name, "m") && m.type->numeric_class == FX4_CLASS_MATRIX && m.type->rows == 4 && m.type->columns == 3 && m.type->column_major && !m.defaults);
  CHECK(fx4_footprint(m.type, 0) == 2 * 16 + 4 * 4);      /* (three columns of four rows) */
  CHECK(m.annotations.count == 1 && f32(m.annotations.items[0].values + 4) == 4.0f);
  CHECK(same(e.objects[0].name, "s_ann") && same(e.objects[0].elements[0].string, "x"));
  CHECK(same(e.objects[2].name, "strs") && e.objects[2].element_count == 2 && same(e.objects[2].elements[1].string, "two"));
  CHECK(same(e.objects[3].name, "tex") && e.objects[3].type->object == FX4_OBJ_TEXTURE2D);
  const Fx4Object &samp = e.objects[4];
  CHECK(same(samp.name, "samp") && samp.type->object == FX4_OBJ_SAMPLER && samp.elements[0].state.count == 2);
  const Fx4Assignment &filter = samp.elements[0].state.items[0];
  CHECK(filter.id == 45 && filter.op == FX4_OP_CONST && filter.value_count == 1 && filter.values[0].bits == 0x15);
  const Fx4Technique &t = e.techniques[0];
  CHECK(same(t.name, "t0") && t.pass_count == 1 && t.annotations.count == 1 && u32(t.annotations.items[0].values) == 2);
  const Fx4Pass &p = t.passes[0];
  CHECK(same(p.name, "p0") && same(p.annotations.items[0].strings[0], "pass") && p.state.count == 3);
  CHECK(p.state.items[0].id == 10 && p.state.items[0].value_count == 4);      /* (BlendFactor) */
  CHECK(p.state.items[2].id == 2 && p.state.items[2].op == FX4_OP_CONST && p.state.items[2].values[0].bits == 0);      /* (BlendState NULL) */
  fx4_free(&e);
}

static void test_pass() {
  const std::vector<uint8_t> d = load("pass.fxo");
  Fx4Effect e;
  CHECK(fx4_parse(d.data(), d.size(), 0, &g_alloc, &e) == FX4_OK);
  CHECK(e.shader_count == 3 && e.anonymous_count == 1 && e.blend_count == 2);
  const Fx4Buffer &cb = e.buffers[0];
  CHECK(same(cb.name, "cb0") && cb.count == 3 && same(cb.vars[1].name, "idx") && same(cb.vars[2].name, "bias"));
  /* the objects: the shaders' code is DXBC; the stream output one has its declaration */
  const Fx4Object *gso = nullptr, *vs = nullptr, *bs = nullptr;
  for (uint32_t i = 0; i < e.object_count; ++i) {
    if (same(e.objects[i].name, "gso")) gso = &e.objects[i];
    if (same(e.objects[i].name, "vs")) vs = &e.objects[i];
    if (same(e.objects[i].name, "bs")) bs = &e.objects[i];
  }
  CHECK(vs && vs->elements[0].shader.code && !std::memcmp(vs->elements[0].shader.code, "DXBC", 4));
  CHECK(gso && gso->type->object == FX4_OBJ_GEOMETRYSHADER_SO && same(gso->elements[0].shader.so_decl, "SV_POSITION.xyz; COLOR.x"));
  CHECK(bs && bs->element_count == 2 && bs->elements[0].state.count == 2);
  const Fx4Pass &p = e.techniques[0].passes[0];
  const Fx4Assignment *value = nullptr, *index = nullptr, *anon = nullptr;
  for (uint32_t i = 0; i < p.state.count; ++i) {
    if (p.state.items[i].op == FX4_OP_VALUE_EXPRESSION) value = &p.state.items[i];
    if (p.state.items[i].op == FX4_OP_INDEX_EXPRESSION) index = &p.state.items[i];
    if (p.state.items[i].op == FX4_OP_ANONYMOUS_SHADER) anon = &p.state.items[i];
  }
  CHECK(anon && anon->id == 7 && anon->shader.code);
  /* BlendFactor = float4(bias, 0, 0, 1): bias from its register */
  CHECK(value && value->id == 10 && value->expression.var_count == 1 && same(value->expression.vars[0].name, "bias"));
  if (value) {
    std::vector<float> cbv(value->expression.table_size[FX4_TABLE_CB], 0.0f);
    cbv[4 * value->expression.vars[0].reg_index] = 0.375f;
    const std::vector<float> r = run(value->expression, cbv);
    CHECK(r.size() == 4 && r[0] == 0.375f && r[1] == 0.0f && r[2] == 0.0f && r[3] == 1.0f);
  }
  /* SetBlendState(bs[idx], ...): the index from idx */
  CHECK(index && index->id == 2 && same(index->name, "bs") && same(index->expression.vars[0].name, "idx"));
  if (index) {
    std::vector<float> cbv(index->expression.table_size[FX4_TABLE_CB], 0.0f);
    uint32_t one = 1;
    std::memcpy(&cbv[4 * index->expression.vars[0].reg_index], &one, 4);
    const std::vector<float> r = run(index->expression, cbv);
    uint32_t got;
    std::memcpy(&got, &r[0], 4);
    CHECK(got == 1);
  }
  fx4_free(&e);
}

static void test_blend41_pool_child() {
  const std::vector<uint8_t> d = load("blend41.fxo");
  Fx4Effect e;
  CHECK(fx4_parse(d.data(), d.size(), 0, &g_alloc, &e) == FX4_BAD);      /* (d3d10.dll: no blend of a target but the first) */
  CHECK(fx4_parse(d.data(), d.size(), 1, &g_alloc, &e) == FX4_OK);
  CHECK(e.version == FX4_VERSION_4_1);
  bool second_target = false, cube_array = false;
  for (uint32_t i = 0; i < e.object_count; ++i) {
    if (e.objects[i].type->object == FX4_OBJ_TEXTURECUBEARRAY) cube_array = true;
    if (!same(e.objects[i].name, "per_target")) continue;
    for (uint32_t k = 0; k < e.objects[i].elements[0].state.count; ++k)
      if (e.objects[i].elements[0].state.items[k].id == 38 && e.objects[i].elements[0].state.items[k].index == 1) second_target = true;
  }
  CHECK(second_target && cube_array);
  fx4_free(&e);
  const std::vector<uint8_t> pool = load("pool.fxo"), child = load("child.fxo");
  CHECK(fx4_parse(pool.data(), pool.size(), 0, &g_alloc, &e) == FX4_OK);
  CHECK(e.buffer_count == 1 && same(e.buffers[0].name, "shared_cb") && e.buffers[0].count == 2 && !e.shared_buffer_count);
  fx4_free(&e);
  CHECK(fx4_parse(child.data(), child.size(), 0, &g_alloc, &e) == FX4_OK);
  CHECK(e.shared_buffer_count == 1 && same(e.shared_buffers[0].name, "shared_cb") && e.shared_buffers[0].count == 2);
  CHECK(e.shared_object_count == 2 && same(e.shared_objects[0].name, "shared_tex") && same(e.shared_objects[1].name, "shared_blend"));
  CHECK(e.buffer_count == 1 && same(e.buffers[0].name, "local_cb"));
  fx4_free(&e);
}

/* A container of one FXLC part of the instructions given, and literals. */
static std::vector<uint8_t> container(const std::vector<uint32_t> &code, const std::vector<float> &literals) {
  std::vector<uint8_t> cli4(4 + 4 * literals.size()), fxlc(4 * code.size());
  const uint32_t n = static_cast<uint32_t>(literals.size());
  std::memcpy(cli4.data(), &n, 4);
  std::memcpy(cli4.data() + 4, literals.data(), 4 * literals.size());
  std::memcpy(fxlc.data(), code.data(), 4 * code.size());
  std::vector<uint8_t> out(32 + 8);
  std::memcpy(out.data(), "DXBC", 4);
  const uint32_t parts = 2;
  std::memcpy(out.data() + 28, &parts, 4);
  uint32_t at = 40;
  std::memcpy(out.data() + 32, &at, 4);
  at += 8 + static_cast<uint32_t>(cli4.size());
  std::memcpy(out.data() + 36, &at, 4);
  for (const auto *part : {&cli4, &fxlc}) {
    const char *tag = part == &cli4 ? "CLI4" : "FXLC";
    const uint32_t size = static_cast<uint32_t>(part->size());
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
    out.insert(out.end(), part->begin(), part->end());
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  return out;
}
static uint32_t token(uint32_t op, uint32_t comps, bool scalar = false) { return comps | (op << 20) | (scalar ? 0x80000000u : 0); }

static void test_expressions() {
  Fx4Effect e;
  std::memset(&e, 0, sizeof e);
  e.alloc = g_alloc;
  int status = FX4_OK;
  /* result.xy = literal.xy * literal.z (scalar), then result.z = atan2(lit.x, lit.y) */
  const std::vector<uint32_t> code = {2,
                                      token(0x205, 2, true), 2, 0, FX4_TABLE_CONSTANTS, 2, 0, FX4_TABLE_CONSTANTS, 0, 0, FX4_TABLE_RESULT, 0,
                                      token(0x206, 1), 2, 0, FX4_TABLE_CONSTANTS, 0, 0, FX4_TABLE_CONSTANTS, 1, 0, FX4_TABLE_RESULT, 2};
  std::vector<uint8_t> c = container(code, {3.0f, 4.0f, 0.5f});
  Fx4Preshader p;
  std::memset(&p, 0, sizeof p);
  CHECK(fx4_preshader(&e, c.data(), static_cast<uint32_t>(c.size()), &p, &status));
  CHECK(p.table_size[FX4_TABLE_RESULT] == 3);
  const std::vector<float> r = run(p, {});
  CHECK(r[0] == 1.5f && r[1] == 2.0f && std::fabs(r[2] - std::atan2(3.0f, 4.0f)) < 1e-6f);
  /* refused: an unknown opcode, a parameter past the literals, an output into the literals, a truncated instruction */
  std::vector<uint32_t> bad = code;
  bad[1] = token(0x7FF, 2);
  c = container(bad, {3.0f, 4.0f, 0.5f});
  std::memset(&p, 0, sizeof p);
  CHECK(!fx4_preshader(&e, c.data(), static_cast<uint32_t>(c.size()), &p, &status));
  bad = code;
  bad[5] = 3;      /* (literal 3 of three) */
  c = container(bad, {3.0f, 4.0f, 0.5f});
  std::memset(&p, 0, sizeof p);
  CHECK(!fx4_preshader(&e, c.data(), static_cast<uint32_t>(c.size()), &p, &status));
  bad = code;
  bad[10] = FX4_TABLE_CONSTANTS;
  c = container(bad, {3.0f, 4.0f, 0.5f});
  std::memset(&p, 0, sizeof p);
  CHECK(!fx4_preshader(&e, c.data(), static_cast<uint32_t>(c.size()), &p, &status));
  bad.assign(code.begin(), code.end() - 5);
  c = container(bad, {3.0f, 4.0f, 0.5f});
  std::memset(&p, 0, sizeof p);
  CHECK(!fx4_preshader(&e, c.data(), static_cast<uint32_t>(c.size()), &p, &status));
  fx4_free(&e);
}

/* The math of expressions against the C library's (to a few units in the last place). */
static bool near_ulps(float a, double b, double ulps) {
  if (std::isnan(b)) return std::isnan(a);
  const double scale = std::fabs(b) > 1e-30 ? std::fabs(b) : 1e-30;
  return std::fabs(a - b) <= ulps * scale * 1.2e-7 + 1e-37;
}
static std::string listing(const char *name, int allow_10_1, int child, int html) {
  std::vector<uint8_t> data = load(name);
  Fx4Effect e;
  std::string out;
  if (fx4_parse(data.data(), data.size(), allow_10_1, &g_alloc, &e) != FX4_OK) return out;
  DxdText text = {nullptr, 0, 0, 0, html};
  if (fx4_disassemble(&text, &e, child) && text.data) out.assign(text.data, text.size);
  std::free(text.data);
  fx4_free(&e);
  return out;
}
static bool has(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }
static void test_disassemble() {
  const std::string pass = listing("pass.fxo", 0, 0, 0);
  CHECK(has(pass, "// FX Version: fx_4_0\n// Child effect (requires effect pool): false\n"));
  CHECK(has(pass, "// 1 local buffer(s)") && has(pass, "cbuffer cb0"));
  CHECK(has(pass, "float4 tint") && has(pass, "// Offset:    0, size:   16"));
  CHECK(has(pass, "BlendState bs[2]") && has(pass, "BlendEnable[0] = bool(TRUE);") && has(pass, "SrcBlend = int(ONE /* 2 */);"));
  CHECK(has(pass, "RasterizerState rs\n{\n    CullMode = int(NONE /* 1 */);\n    DepthBias = int(3);\n};"));
  CHECK(has(pass, "GeometryShader gso = ConstructGSWithSO( asm {") && has(pass, "\"SV_POSITION.xyz; COLOR.x\" );"));
  CHECK(has(pass, "technique10 ") && has(pass, "    pass ") && has(pass, "        VertexShader = vs;"));
  CHECK(has(pass, "        BlendState = bs[\n            asm {\n") && has(pass, "// c0: idx"));
  CHECK(has(pass, "        StencilRef = uint(2);") && has(pass, "SampleMask = uint(4294967295);"));
  CHECK(has(pass, "vs_4_0") && has(pass, "ps_4_0") && has(pass, "gs_4_0"));
  CHECK(has(pass, "expr0."));
  const std::string html = listing("pass.fxo", 0, 0, 1);
  CHECK(html.rfind("<html><body><pre>\n", 0) == 0 && has(html, "</pre></body></html>\n"));
  CHECK(html.find("<html>", 1) == std::string::npos);      /* (the shaders' listings without pages of their own) */
  const std::string ten_one = listing("blend41.fxo", 1, 0, 0);
  CHECK(has(ten_one, "FX Version: fx_4_1") && has(ten_one, "BlendState per_target") && has(ten_one, "DestBlend[1] = int(INV_SRC_ALPHA /* 6 */);"));
  CHECK(has(ten_one, "TextureCubeArray cubes;"));
  const std::string child = listing("child.fxo", 0, 1, 0);
  CHECK(has(child, "Child effect (requires effect pool): true") && has(child, "// 1 shared buffer(s)") && has(child, "shared cbuffer shared_cb"));
  CHECK(has(child, "shared Texture2D shared_tex;") && has(child, "shared BlendState shared_blend;") && has(child, "BlendState = shared_blend;"));
  const std::string pool = listing("pool.fxo", 0, 0, 0);
  CHECK(has(pool, "float4 shared_color : COLOR0 = { 0.250000, 0.500000, 0.750000, 1.000000 };"));
  CHECK(has(pool, "SrcBlend = int(ONE /* 2 */);"));
  if (g_failures) std::fwrite(pass.data(), 1, pass.size(), stdout);
}

static void test_math() {
  for (int i = -2000; i <= 2000; ++i) {
    const float x = i * 0.0137f;
    CHECK(near_ulps(fx4_sin(x), std::sin(static_cast<double>(x)), 2) || std::fabs(fx4_sin(x) - std::sin(x)) < 1e-7f);
    CHECK(near_ulps(fx4_cos(x), std::cos(static_cast<double>(x)), 2) || std::fabs(fx4_cos(x) - std::cos(x)) < 1e-7f);
    CHECK(near_ulps(fx4_atan(x), std::atan(static_cast<double>(x)), 2));
    CHECK(near_ulps(fx4_exp2(x * 0.1f), std::exp2(static_cast<double>(x * 0.1f)), 1));
    if (x > 0) {
      CHECK(near_ulps(fx4_log2(x), std::log2(static_cast<double>(x)), 2) || std::fabs(fx4_log2(x) - std::log2(x)) < 1e-6f);
      CHECK(near_ulps(fx4_sqrt(x), std::sqrt(static_cast<double>(x)), 1));
    }
    const float y = i / 2000.0f;
    CHECK(near_ulps(fx4_asin(y), std::asin(static_cast<double>(y)), 2) || std::fabs(fx4_asin(y) - std::asin(y)) < 1e-7f);
    CHECK(near_ulps(fx4_acos(y), std::acos(static_cast<double>(y)), 2));
    CHECK(near_ulps(fx4_atan2(y, x), std::atan2(static_cast<double>(y), static_cast<double>(x)), 2) || std::fabs(fx4_atan2(y, x) - std::atan2(y, x)) < 1e-7f);
  }
  CHECK(fx4_floor(-1.5f) == -2.0f && fx4_ceil(-1.5f) == -1.0f && fx4_floor(3.0f) == 3.0f && fx4_ceil(1e30f) == 1e30f);
  CHECK(fx4_exp2(3.0f) == 8.0f && fx4_log2(8.0f) == 3.0f && fx4_sqrt(16.0f) == 4.0f && std::isnan(fx4_sqrt(-1.0f)));
  CHECK(fx4_f2i(1e20f) == 0x7FFFFFFF && fx4_f2u(-3.0f) == 0 && fx4_f2u(3.7f) == 3 && fx4_f2i(std::nanf("")) == 0);
}

/* Every prefix of a binary is refused; every one-bit change is refused or read whole and walked. */
static void fuzz(const char *name, int allow_10_1) {
  const std::vector<uint8_t> d = load(name);
  CHECK(!d.empty());
  CHECK(parse_exact(d.data(), d.size(), allow_10_1) == FX4_OK);
  int refused = 0;
  for (size_t n = 0; n < d.size(); ++n) refused += parse_exact(d.data(), n, allow_10_1) != FX4_OK;
  CHECK(refused == static_cast<int>(d.size()));
  std::vector<uint8_t> flip = d;
  size_t read_ok = 0, total = 0;
  for (size_t i = 0; i < d.size(); ++i)
    for (int b = 0; b < 8; ++b) {
      flip[i] ^= static_cast<uint8_t>(1u << b);
      read_ok += parse_exact(flip.data(), flip.size(), allow_10_1) == FX4_OK;
      ++total;
      flip[i] = d[i];
    }
  std::printf("  %s: %zu prefixes refused, %zu of %zu one-bit changes still read\n", name, d.size(), read_ok, total);
}

int main() {
  test_annotations();
  test_pass();
  test_blend41_pool_child();
  test_expressions();
  test_math();
  test_disassemble();
  fuzz("annotations.fxo", 0);
  fuzz("pass.fxo", 0);
  fuzz("blend41.fxo", 1);
  fuzz("pool.fxo", 0);
  fuzz("child.fxo", 0);
  std::printf("Fx4ParserTest: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

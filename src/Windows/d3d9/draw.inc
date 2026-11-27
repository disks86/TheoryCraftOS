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

/* Part of d3d9.c: drawing. A draw decodes the vertices it reads into the vertex shader's layout (the fixed-function one's, or one register for each
 * input of the program's), copies its indices, works out the Direct3D 12
 * pipeline and the constants of the state that is set, and records the draw. */

/* ---- vertices --------------------------------------------------------------------------------------------------------------------------------------- */

/* A stream as a draw reads it: per vertex, or (instanced) one element for every vertex of the instance being drawn. */
typedef struct Source { const BYTE *data; UINT bytes, stride; BOOL instanced; UINT element; } Source;
/* Where an attribute of vertex `vertex` is. */
static const BYTE *source_at(const Source *s, UINT vertex, UINT offset) { return s->data + (SIZE_T)(s->instanced ? s->element : vertex) * s->stride + offset; }

static float half_to_float(WORD h) {
  const UINT exponent = (h >> 10) & 31, mantissa = h & 1023;
  float value;
  if (exponent == 0) value = (float)mantissa / 1024.0f / 16384.0f;
  else {
    value = 1.0f + (float)mantissa / 1024.0f;
    for (UINT e = exponent; e > 15; --e) value *= 2.0f;
    for (UINT e = exponent; e < 15; ++e) value *= 0.5f;
  }
  return (h & 0x8000) ? -value : value;
}
/* The bytes an element of a declaration type takes, 0 for a type that is not known. */
static UINT type_bytes(BYTE type) {
  static const BYTE bytes[17] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
  return type < 17 ? bytes[type] : 0;
}
/* The numbers an element of a declaration type has. */
static int type_components(BYTE type) {
  static const BYTE components[17] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4};
  return type < 17 ? components[type] : 0;
}
/* An element as four floats; what the element has not is 0, 0, 0, 1. */
static void decode_element(BYTE type, const BYTE *p, float out[4]) {
  short s[4];
  WORD w[4];
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  switch (type) {
    case D3DDECLTYPE_FLOAT1: case D3DDECLTYPE_FLOAT2: case D3DDECLTYPE_FLOAT3: case D3DDECLTYPE_FLOAT4:
      memcpy(out, p, 4u * (type - D3DDECLTYPE_FLOAT1 + 1));
      break;
    case D3DDECLTYPE_D3DCOLOR:      /* (blue first in memory) */
      out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
      break;
    case D3DDECLTYPE_UBYTE4: for (int i = 0; i < 4; ++i) out[i] = (float)p[i]; break;
    case D3DDECLTYPE_UBYTE4N: for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f; break;
    case D3DDECLTYPE_SHORT2: memcpy(s, p, 4); out[0] = (float)s[0]; out[1] = (float)s[1]; break;
    case D3DDECLTYPE_SHORT4: memcpy(s, p, 8); for (int i = 0; i < 4; ++i) out[i] = (float)s[i]; break;
    /* (signed normalised: -32768 is -1 as -32767 is) */
    case D3DDECLTYPE_SHORT2N: memcpy(s, p, 4); for (int i = 0; i < 2; ++i) out[i] = s[i] < -32767 ? -1.0f : s[i] / 32767.0f; break;
    case D3DDECLTYPE_SHORT4N: memcpy(s, p, 8); for (int i = 0; i < 4; ++i) out[i] = s[i] < -32767 ? -1.0f : s[i] / 32767.0f; break;
    case D3DDECLTYPE_USHORT2N: memcpy(w, p, 4); out[0] = w[0] / 65535.0f; out[1] = w[1] / 65535.0f; break;
    case D3DDECLTYPE_USHORT4N: memcpy(w, p, 8); for (int i = 0; i < 4; ++i) out[i] = w[i] / 65535.0f; break;
    case D3DDECLTYPE_FLOAT16_2: memcpy(w, p, 4); out[0] = half_to_float(w[0]); out[1] = half_to_float(w[1]); break;
    case D3DDECLTYPE_FLOAT16_4: memcpy(w, p, 8); for (int i = 0; i < 4; ++i) out[i] = half_to_float(w[i]); break;
    case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: {      /* (three 10-bit fields from bit 0; the top two bits are not read; UDEC3 unsigned as they are, DEC3N signed and normalised) */
      UINT v;
      memcpy(&v, p, 4);
      for (int i = 0; i < 3; ++i) {
        const UINT field = (v >> (10 * i)) & 1023u;
        if (type == D3DDECLTYPE_UDEC3) out[i] = (float)field;
        else {
          const int value = field & 512u ? (int)field - 1024 : (int)field;
          out[i] = value < -511 ? -1.0f : value / 511.0f;
        }
      }
      break;
    }
    default: break;
  }
}
/* Whether the vertices `first .. first + count - 1` of an attribute are inside its stream. */
static BOOL attribute_inside(const Attribute *a, const Source *sources, UINT first, UINT count) {
  if (a->type == 255) return TRUE;
  const Source *s = &sources[a->stream];
  const UINT size = type_bytes(a->type);
  if (!s->data || !size || count == 0) return FALSE;
  if (s->instanced) return (UINT64)s->element * s->stride + a->offset + size <= s->bytes;      /* (the instance's one element) */
  const UINT64 last = (UINT64)(first + count - 1) * s->stride + a->offset + size;
  return first <= 0xFFFFFFFFu - count && last <= s->bytes;
}
/* Decodes `count` vertices from `first` into the shaders' layout. */
/* How the coordinates of the texture stages are made for a draw (ffl_texture_coordinates). */
typedef struct Coordinates {
  FflTextureStage stage[STAGES];
  UINT set[STAGES];              /* the set of the vertex's a stage reads */
  UINT sets;                     /* the stages a vertex gets coordinates for */
  float world_view[4][4], normal[3][3];
  int normalize, for_program, needs_normal;
} Coordinates;
/* Vertex blending and tweening of the fixed-function pipeline (D3DRS_VERTEXBLEND), done here where the vertices are decoded: a vertex's position and normal go
 * through each of the world matrices its weights name (D3DTS_WORLDMATRIX(n): the first ones, or with D3DRS_INDEXEDVERTEXBLENDENABLE those its blend indices
 * say), and are added up by the weights - the last weight is 1 less the others -, into world space (the draw's world matrix is the identity then); tweening
 * blends the vertex's two positions and normals by D3DRS_TWEENFACTOR. */
typedef struct Blend {
  UINT matrices;                 /* 0: no blending; 1 .. 4: so many matrices a vertex */
  BOOL indexed, tween;
  float tween_factor;
  const D3DMATRIX *world;        /* the 256 world matrices */
  D3DMATRIX copy[256];           /* (the matrices as the program set them: the device's first is the identity during the draw) */
  float normal[256][3][3];       /* (the matrices for normals, the inverse of each one's 3 x 3 transposed, made as first needed) */
  BYTE made[256];
} Blend;
static void blend_point(const D3DMATRIX *m, const float in[4], float out[4]) {
  for (int k = 0; k < 4; ++k) out[k] = in[0] * m->m[0][k] + in[1] * m->m[1][k] + in[2] * m->m[2][k] + in[3] * m->m[3][k];
}
/* The position (and the normal) of a vertex blended into world space. */
static void blend_vertex(Blend *b, const Layout *l, const Source *sources, UINT vertex, float position[4], float normal[4]) {
  float weights[4] = {0, 0, 0, 0}, indices[4] = {0, 1, 2, 3};
  if (l->weights.type != 255) decode_element(l->weights.type, source_at(&sources[l->weights.stream], vertex, l->weights.offset), weights);
  if (b->indexed && l->indices.type != 255) {
    decode_element(l->indices.type, source_at(&sources[l->indices.stream], vertex, l->indices.offset), indices);
    if (l->indices.type == D3DDECLTYPE_D3DCOLOR || l->indices.type == D3DDECLTYPE_UBYTE4N)
      for (int k = 0; k < 4; ++k) indices[k] *= 255.0f;      /* (a D3DCOLOR's bytes, as the numbers they are) */
  }
  float last = 1.0f, p[4] = {0, 0, 0, 0}, n[3] = {0, 0, 0};
  for (UINT i = 0; i < b->matrices; ++i) {
    const float w = i + 1 < b->matrices ? weights[i] : last;
    last -= w;
    const float at = indices[i] < 0.0f ? 0.0f : indices[i] > 255.0f ? 255.0f : indices[i];
    const UINT m = b->indexed ? (UINT)(at + 0.5f) : i;
    float q[4];
    blend_point(&b->world[m], position, q);
    for (int k = 0; k < 4; ++k) p[k] += w * q[k];
    if (!b->made[m]) {
      ffl_normal_matrix(b->world[m].m, b->normal[m]);
      b->made[m] = 1;
    }
    for (int k = 0; k < 3; ++k) n[k] += w * (normal[0] * b->normal[m][0][k] + normal[1] * b->normal[m][1][k] + normal[2] * b->normal[m][2][k]);
  }
  memcpy(position, p, sizeof p);
  memcpy(normal, n, sizeof n);
}
/* Decodes `count` vertices from `first` for the fixed-function shaders; with `lit` their colours are what the lights make of them. `blend`: blending or
 * tweening (NULL: neither). */
static void decode_vertices(const Layout *l, const Source *sources, UINT first, UINT count, BYTE *out, const FflSetup *lit, const Coordinates *c, Blend *blend) {
  for (UINT v = 0; v < count; ++v, out += VERTEX_BYTES(c->sets)) {
    float *f = (float *)out, value[4];
    f[3] = 1.0f;      /* (the fourth of a position that are pixels is 1 / w; one of three numbers has none) */
    decode_element(l->position.type, source_at(&sources[l->position.stream], first + v, l->position.offset), f);
    f[4] = f[5] = f[6] = f[7] = 1.0f;
    if (l->colour.type != 255) decode_element(l->colour.type, source_at(&sources[l->colour.stream], first + v, l->colour.offset), f + 4);
    float normal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if ((lit || c->needs_normal || blend) && l->normal.type != 255)
      decode_element(l->normal.type, source_at(&sources[l->normal.stream], first + v, l->normal.offset), normal);
    if (blend && blend->tween) {
      float second[4] = {0, 0, 0, 1}, second_normal[4] = {0, 0, 0, 0};
      if (l->position1.type != 255) decode_element(l->position1.type, source_at(&sources[l->position1.stream], first + v, l->position1.offset), second);
      if (l->normal1.type != 255) decode_element(l->normal1.type, source_at(&sources[l->normal1.stream], first + v, l->normal1.offset), second_normal);
      const float t = blend->tween_factor;
      for (int k = 0; k < 3; ++k) {
        f[k] = f[k] * (1.0f - t) + second[k] * t;
        normal[k] = normal[k] * (1.0f - t) + second_normal[k] * t;
      }
    } else if (blend && blend->matrices) blend_vertex(blend, l, sources, first + v, f, normal);
    for (UINT t = 0; t < c->sets; ++t) {
      const Attribute *a = &l->texture[c->set[t]];
      value[0] = value[1] = value[2] = 0.0f;
      value[3] = 1.0f;
      if (a->type != 255) decode_element(a->type, source_at(&sources[a->stream], first + v, a->offset), value);
      ffl_texture_coordinates(&c->stage[t], c->world_view, c->normal, c->normalize, f, normal, value, a->type != 255 ? type_components(a->type) : 0, c->for_program,
                              f + 12 + 4 * t);
    }
    /* the second colour, and the fog factor a vertex brings, which is its alpha; a vertex without one brings none (1) */
    float second[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (l->specular.type != 255)
      decode_element(l->specular.type, source_at(&sources[l->specular.stream], first + v, l->specular.offset), second);
    f[8] = second[3];
    f[9] = second[0]; f[10] = second[1]; f[11] = second[2];
    if (lit) {
      const float first_colour[4] = {f[4], f[5], f[6], f[7]};
      ffl_light_vertex(lit, f, normal, l->colour.type != 255 ? first_colour : NULL, l->specular.type != 255 ? second : NULL, f + 4, f + 9);
    }
  }
}

/* Where the inputs of a program's vertex shader are in the vertices: for each, the element of the declaration that is the same thing (usage and
 * index); type 255 for an input the vertices have not, which the shader reads as 0, 0, 0, 1. */
static void shader_attributes(const Shader *vs, const D3DVERTEXELEMENT9 *e, Attribute *out) {
  for (UINT i = 0; i < vs->input_count; ++i) {
    out[i] = g_no_attribute;
    for (UINT k = 0; k < MAX_ELEMENTS && e[k].Stream != 0xFF; ++k) {
      if (e[k].Stream >= STREAMS || e[k].Usage != vs->inputs[i].usage || e[k].UsageIndex != vs->inputs[i].index || !type_bytes(e[k].Type)) continue;
      const Attribute a = {(BYTE)e[k].Stream, e[k].Type, e[k].Offset};
      out[i] = a;
      break;
    }
  }
}
/* Decodes `count` vertices from `first` for a program's vertex shader: four floats for each of its `inputs` inputs. */
static void decode_shader_vertices(const Attribute *a, UINT inputs, const Source *sources, UINT first, UINT count, BYTE *out) {
  for (UINT v = 0; v < count; ++v) {
    for (UINT i = 0; i < inputs; ++i, out += 16) {
      float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      if (a[i].type != 255) decode_element(a[i].type, source_at(&sources[a[i].stream], first + v, a[i].offset), value);
      memcpy(out, value, 16);
    }
  }
}

/* ---- pipelines -------------------------------------------------------------------------------------------------------------------------------------- */

/* (the same numbers, D3DBLEND_BLENDFACTOR and D3DBLEND_INVBLENDFACTOR too; D3DBLEND_BOTHSRCALPHA and BOTHINVSRCALPHA set both factors: blend_pair) */
static D3D12_BLEND blend_of(DWORD value) {
  return (value >= D3DBLEND_ZERO && value <= D3DBLEND_SRCALPHASAT) || value == D3DBLEND_BLENDFACTOR || value == D3DBLEND_INVBLENDFACTOR ? (D3D12_BLEND)value : D3D12_BLEND_ONE;
}
/* The source and destination factors of a source blend and a destination blend: a source of D3DBLEND_BOTHSRCALPHA is the source's alpha and the destination one minus it
 * (BOTHINVSRCALPHA the other way round), whatever the destination blend says. */
static void blend_pair(DWORD src, DWORD dst, UINT *src_out, UINT *dst_out) {
  if (src == D3DBLEND_BOTHSRCALPHA) { *src_out = D3D12_BLEND_SRC_ALPHA; *dst_out = D3D12_BLEND_INV_SRC_ALPHA; return; }
  if (src == D3DBLEND_BOTHINVSRCALPHA) { *src_out = D3D12_BLEND_INV_SRC_ALPHA; *dst_out = D3D12_BLEND_SRC_ALPHA; return; }
  *src_out = blend_of(src);
  *dst_out = blend_of(dst);
}
static UINT op_of(DWORD value, UINT lowest, UINT highest, UINT otherwise) { return value >= lowest && value <= highest ? value : otherwise; }

/* The textures a draw's tables hold: the samplers its pixel shader reads, as 2, 4, 8 or 16. */
static UINT texture_slots(const Shader *ps) {
  const UINT count = ps->sampler_count;
  return count <= 2 ? 2 : count <= 4 ? 4 : count <= 8 ? 8 : 16;
}
/* The sets of texture coordinates the vertices of the fixed-function vertex shader have for a pixel shader: what it reads, as 2, 4 or 8. */
static UINT coordinate_sets(const Shader *ps) { return ps->coordinates <= 2 ? 2 : ps->coordinates <= 4 ? 4 : 8; }
/* The root signature for so many textures: the block of constants (b0), the textures (t0 ..) and their samplers (s0 ..). NULL: it cannot be made. */
static ID3D12RootSignature *root_get(Dev *d, UINT slots) {
  ID3D12RootSignature **root = &d->roots[slots <= 2 ? 0 : slots <= 4 ? 1 : slots <= 8 ? 2 : 3];
  if (*root) return *root;
  D3D12_DESCRIPTOR_RANGE views = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slots, 0, 0, 0}, samplers = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, slots, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &views;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &samplers;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {3, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
  if (error) ID3D10Blob_Release(error);
  if (SUCCEEDED(hr) && blob)
    hr = ID3D12Device_CreateRootSignature(d->d12, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)root);
  if (blob) ID3D10Blob_Release(blob);
  if (FAILED(hr)) *root = NULL;
  return *root;
}

/* The Direct3D 12 pipeline state of the shaders (no vertex shader: the fixed-function one) and the render states that are set; NULL when it
 * cannot be made. */
/* The targets a draw writes: target 0, and those of 1 .. 3 that are set one after the other from it (Direct3D 12 here takes no gap) and are its size. */
static UINT draw_targets(const Dev *d, Surface **out) {
  out[0] = d->target[0];
  UINT n = 1;
  const UINT w = sub_width(d->target[0]->tex, d->target[0]->sub), h = sub_height(d->target[0]->tex, d->target[0]->sub);
  for (; n < TARGETS && d->target[n]; ++n) {
    const Surface *s = d->target[n];
    if (sub_width(s->tex, s->sub) != w || sub_height(s->tex, s->sub) != h) break;
    out[n] = d->target[n];
  }
  return n;
}
static ID3D12PipelineState *pipeline_get(Dev *d, const Shader *vs, const Shader *ps, D3DPRIMITIVETYPE type) {
  const DWORD *rs = d->state.rs;
  Pipeline key;
  memset(&key, 0, sizeof key);
  key.topology = type == D3DPT_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : type == D3DPT_LINELIST || type == D3DPT_LINESTRIP ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                                                                                                 : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  key.depth_format = d->depth ? (DWORD)depth_dxgi(d->depth->tex->format) : 0;
  key.target_format = (DWORD)gpu_dxgi(d->target[0]->tex->format);
  {
    Surface *targets[TARGETS];
    key.targets = draw_targets(d, targets);
    static const D3DRENDERSTATETYPE masks[3] = {D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3};
    for (UINT i = 1; i < key.targets; ++i) {
      key.extra_format[i - 1] = (DWORD)gpu_dxgi(targets[i]->tex->format);
      key.extra_write |= (rs[masks[i - 1]] & 0xFu) << (4 * (i - 1));
    }
  }
  key.vs = vs ? vs->c.id : 0;
  key.ps = ps->c.id;
  key.sets = vs ? 0 : coordinate_sets(ps);
  key.cull = rs[D3DRS_CULLMODE];
  key.z = rs[D3DRS_ZENABLE] != D3DZB_FALSE && d->depth != NULL;
  key.zwrite = rs[D3DRS_ZWRITEENABLE] != 0;
  key.zfunc = op_of(rs[D3DRS_ZFUNC], 1, 8, D3DCMP_LESSEQUAL);
  key.blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
  {
    UINT s_, d_;
    blend_pair(rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], &s_, &d_);
    key.src = s_;
    key.dst = d_;
  }
  key.op = op_of(rs[D3DRS_BLENDOP], 1, 5, D3DBLENDOP_ADD);
  key.separate = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
  key.src_alpha = key.src;
  key.dst_alpha = key.dst;
  if (key.separate) {
    UINT s_, d_;
    blend_pair(rs[D3DRS_SRCBLENDALPHA], rs[D3DRS_DESTBLENDALPHA], &s_, &d_);
    key.src_alpha = s_;
    key.dst_alpha = d_;
  }
  key.op_alpha = key.separate ? op_of(rs[D3DRS_BLENDOPALPHA], 1, 5, D3DBLENDOP_ADD) : key.op;
  key.write = rs[D3DRS_COLORWRITEENABLE] & 0xF;
  key.stencil = rs[D3DRS_STENCILENABLE] != 0 && d->depth != NULL;
  key.sfail = op_of(rs[D3DRS_STENCILFAIL], 1, 8, D3DSTENCILOP_KEEP);
  key.szfail = op_of(rs[D3DRS_STENCILZFAIL], 1, 8, D3DSTENCILOP_KEEP);
  key.spass = op_of(rs[D3DRS_STENCILPASS], 1, 8, D3DSTENCILOP_KEEP);
  key.sfunc = op_of(rs[D3DRS_STENCILFUNC], 1, 8, D3DCMP_ALWAYS);
  key.smask = rs[D3DRS_STENCILMASK] & 0xFF;
  key.swrite = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
  /* (two-sided stencil: the counter-clockwise faces - the back ones here - have operations of their own) */
  key.twosided = key.stencil && rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;
  key.ccw_fail = key.twosided ? op_of(rs[D3DRS_CCW_STENCILFAIL], 1, 8, D3DSTENCILOP_KEEP) : key.sfail;
  key.ccw_zfail = key.twosided ? op_of(rs[D3DRS_CCW_STENCILZFAIL], 1, 8, D3DSTENCILOP_KEEP) : key.szfail;
  key.ccw_pass = key.twosided ? op_of(rs[D3DRS_CCW_STENCILPASS], 1, 8, D3DSTENCILOP_KEEP) : key.spass;
  key.ccw_func = key.twosided ? op_of(rs[D3DRS_CCW_STENCILFUNC], 1, 8, D3DCMP_ALWAYS) : key.sfunc;
  /* (wireframe; D3DFILL_POINT is drawn solid: Direct3D 12 below has no point fill) */
  key.wireframe = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME;
  /* The depth bias: Direct3D 9's is a depth (offset = bias + slope scale * slope); Direct3D 12's counts the depth buffer's units, which for the D24 buffer here is 1 / (2^24 - 1). */
  if (d->depth) {
    float bias, slope;
    memcpy(&bias, &rs[D3DRS_DEPTHBIAS], 4);
    memcpy(&slope, &rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
    const double units = bias == bias ? (double)bias * 16777215.0 : 0.0;
    key.bias = (DWORD)(INT)(units > 2147483647.0 ? 2147483647.0 : units < -2147483648.0 ? -2147483648.0 : (units < 0 ? units - 0.5 : units + 0.5));
    key.slope = slope == slope ? rs[D3DRS_SLOPESCALEDEPTHBIAS] : 0;
  }
  Pipeline *oldest = &d->pipelines[0];
  for (UINT i = 0; i < d->pipeline_count; ++i) {
    Pipeline *p = &d->pipelines[i];
    if (memcmp(p, &key, offsetof(Pipeline, state)) == 0) {
      p->last_used = ++d->pipeline_clock;
      return p->state;
    }
    if (p->last_used < oldest->last_used) oldest = p;
  }

  /* The fixed-function vertex shader's inputs: the position, the colour, the fog factor and specular colour (TEXCOORD8), and the eight sets of
   * texture coordinates - of which the vertices of this draw have `key.sets`; the others read the last that is there, and nothing reads them. */
  D3D12_INPUT_ELEMENT_DESC layout[3 + TEXTURE_SETS] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                       {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                       {"TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  for (UINT i = 0; !vs && i < TEXTURE_SETS; ++i) {
    const D3D12_INPUT_ELEMENT_DESC set = {"TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48 + 16 * (i < key.sets ? i : key.sets - 1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    layout[3 + i] = set;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  /* (a program's vertex shader: its inputs are "ATTR" and the number of the register, each four floats, in the order the vertices are decoded in) */
  D3D12_INPUT_ELEMENT_DESC inputs[SHADER_INPUTS];
  for (UINT i = 0; vs && i < vs->input_count; ++i) {
    const D3D12_INPUT_ELEMENT_DESC input = {"ATTR", vs->inputs[i].reg, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16 * i, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    inputs[i] = input;
  }
  pd.pRootSignature = root_get(d, texture_slots(ps));
  if (!pd.pRootSignature) return NULL;
  pd.VS.pShaderBytecode = vs ? vs->dxbc : g_ff_vs;
  pd.VS.BytecodeLength = vs ? vs->dxbc_bytes : sizeof g_ff_vs;
  pd.PS.pShaderBytecode = ps->dxbc;
  pd.PS.BytecodeLength = ps->dxbc_bytes;
  pd.SampleMask = 0xFFFFFFFFu;
  D3D12_RENDER_TARGET_BLEND_DESC *blend = &pd.BlendState.RenderTarget[0];
  blend->BlendEnable = key.blend;
  blend->SrcBlend = (D3D12_BLEND)key.src;
  blend->DestBlend = (D3D12_BLEND)key.dst;
  blend->BlendOp = (D3D12_BLEND_OP)key.op;
  blend->SrcBlendAlpha = (D3D12_BLEND)key.src_alpha;
  blend->DestBlendAlpha = (D3D12_BLEND)key.dst_alpha;
  blend->BlendOpAlpha = (D3D12_BLEND_OP)key.op_alpha;
  blend->LogicOp = D3D12_LOGIC_OP_NOOP;
  blend->RenderTargetWriteMask = (UINT8)key.write;
  /* (the other targets: the same blending, each its own write mask) */
  pd.BlendState.IndependentBlendEnable = key.targets > 1;
  for (UINT i = 1; i < key.targets; ++i) {
    pd.BlendState.RenderTarget[i] = *blend;
    pd.BlendState.RenderTarget[i].RenderTargetWriteMask = (UINT8)((key.extra_write >> (4 * (i - 1))) & 0xF);
  }
  pd.RasterizerState.FillMode = key.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.DepthBias = (INT)key.bias;
  memcpy(&pd.RasterizerState.SlopeScaledDepthBias, &key.slope, 4);
  /* Direct3D 9 names the winding that is culled; the front of a triangle is the clockwise side here. */
  pd.RasterizerState.CullMode = key.cull == D3DCULL_CW ? D3D12_CULL_MODE_FRONT : key.cull == D3DCULL_CCW ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.DepthStencilState.DepthEnable = key.z;
  pd.DepthStencilState.DepthWriteMask = key.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)key.zfunc;
  pd.DepthStencilState.StencilEnable = key.stencil;
  pd.DepthStencilState.StencilReadMask = (UINT8)key.smask;
  pd.DepthStencilState.StencilWriteMask = (UINT8)key.swrite;
  pd.DepthStencilState.FrontFace.StencilFailOp = (D3D12_STENCIL_OP)key.sfail;
  pd.DepthStencilState.FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)key.szfail;
  pd.DepthStencilState.FrontFace.StencilPassOp = (D3D12_STENCIL_OP)key.spass;
  pd.DepthStencilState.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)key.sfunc;
  pd.DepthStencilState.BackFace.StencilFailOp = (D3D12_STENCIL_OP)key.ccw_fail;
  pd.DepthStencilState.BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)key.ccw_zfail;
  pd.DepthStencilState.BackFace.StencilPassOp = (D3D12_STENCIL_OP)key.ccw_pass;
  pd.DepthStencilState.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)key.ccw_func;
  pd.InputLayout.pInputElementDescs = vs ? inputs : layout;
  pd.InputLayout.NumElements = vs ? vs->input_count : 3 + TEXTURE_SETS;
  pd.NumRenderTargets = key.targets;
  pd.RTVFormats[0] = (DXGI_FORMAT)key.target_format;
  for (UINT i = 1; i < key.targets; ++i) pd.RTVFormats[i] = (DXGI_FORMAT)key.extra_format[i - 1];
  if (d->depth) pd.DSVFormat = (DXGI_FORMAT)key.depth_format;
  pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)key.topology;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *state = NULL;
  TcosD3D12PipelineOptions(d->d12, vs ? vs->registers : VERTEX_REGISTERS, 0);      /* (the pixel shader's constants come after the vertex shader's) */
  if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d12, &pd, &IID_ID3D12PipelineState, (void **)&state)) || !state) {
    /* (the system has only so many shaders at a time: without the pipelines that are kept there may be room) */
    if (!d->pipeline_count) return NULL;
    flush(d);      /* (a recorded draw may use one of them) */
    for (UINT i = 0; i < d->pipeline_count; ++i) ID3D12PipelineState_Release(d->pipelines[i].state);
    d->pipeline_count = 0;
    oldest = &d->pipelines[0];
    state = NULL;
    TcosD3D12PipelineOptions(d->d12, vs ? vs->registers : VERTEX_REGISTERS, 0);
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d12, &pd, &IID_ID3D12PipelineState, (void **)&state)) || !state) return NULL;
  }
  const BOOL full = d->pipeline_count >= PIPELINES;
  Pipeline *slot = full ? oldest : &d->pipelines[d->pipeline_count++];
  if (full) {
    flush(d);      /* (a recorded draw may use the one that goes) */
    ID3D12PipelineState_Release(slot->state);
  }
  *slot = key;
  slot->state = state;
  slot->last_used = ++d->pipeline_clock;
  return state;
}

/* ---- the constants of a draw ------------------------------------------------------------------------------------------------------------------------ */

/* The pixel shader of the texture stages as they are set (ff_stages.h): one for each set-up a draw came with, kept by its key. NULL: it cannot be
 * made. */
static HRESULT shader_make_ex(Dev *d, const DWORD *code, BOOL vertex, int implicit, Shader **out);
static const Shader *fixed_shader_get(Dev *d) {
  const State *s = &d->state;
  FfsKey key;
  unsigned textured = 0;
  memset(&key, 0, sizeof key);
  key.specular = s->rs[D3DRS_SPECULARENABLE] != 0;
  for (UINT n = 0; n < STAGES; ++n) {
    const DWORD *t = s->stage[n];
    FfsStage *stage = &key.stage[n];
    /* (what is not a number of an operation switches the stage off, like D3DTOP_DISABLE) */
    stage->colour_op = (uint8_t)(t[D3DTSS_COLOROP] <= FFS_LERP ? t[D3DTSS_COLOROP] : FFS_DISABLE);
    stage->alpha_op = (uint8_t)(t[D3DTSS_ALPHAOP] <= FFS_LERP ? t[D3DTSS_ALPHAOP] : FFS_DISABLE);
    stage->colour_arg[0] = (uint8_t)t[D3DTSS_COLORARG0]; stage->colour_arg[1] = (uint8_t)t[D3DTSS_COLORARG1]; stage->colour_arg[2] = (uint8_t)t[D3DTSS_COLORARG2];
    stage->alpha_arg[0] = (uint8_t)t[D3DTSS_ALPHAARG0]; stage->alpha_arg[1] = (uint8_t)t[D3DTSS_ALPHAARG1]; stage->alpha_arg[2] = (uint8_t)t[D3DTSS_ALPHAARG2];
    stage->to_temp = (t[D3DTSS_RESULTARG] & D3DTA_SELECTMASK) == D3DTA_TEMP;
    stage->kill = t[TCOS_TSS_KEY_KILL] != 0;
    const Tex *bound = s->tex[n];
    if (bound && bound->res && !bound->depth) textured |= 1u << n;
  }
  ffs_normalize(&key, textured);
  FixedShader *oldest = &d->fixed[0];
  for (UINT i = 0; i < d->fixed_count; ++i) {
    FixedShader *f = &d->fixed[i];
    if (memcmp(&f->key, &key, sizeof key) == 0) {
      f->last_used = ++d->fixed_clock;
      return f->shader;
    }
    if (f->last_used < oldest->last_used) oldest = f;
  }
  DWORD *tokens = mem_alloc(FFS_MAX_TOKENS * sizeof(DWORD));      /* (too many for the stack) */
  Shader *shader = NULL;
  /* (the device's own: no reference of the device, freed by the device) */
  if (tokens && (!ffs_tokens(&key, (uint32_t *)tokens, FFS_MAX_TOKENS) || FAILED(shader_make_ex(d, tokens, FALSE, 1, &shader)))) shader = NULL;
  mem_free(tokens);
  if (!shader) return NULL;
  FixedShader *slot = d->fixed_count < FIXED_SHADERS ? &d->fixed[d->fixed_count++] : oldest;
  if (slot == oldest && slot->shader) obj_free(OBJ(slot->shader));      /* (the pipelines made of the one that goes are not found again, and go in their turn) */
  slot->key = key;
  slot->shader = shader;
  slot->last_used = ++d->fixed_clock;
  return shader;
}
/* The constants of a program's shader, as its translation reads them: the floats c0 .. and, if it reads bools, 16 registers whose first word is
 * all ones for a bool that is set. */
static void shader_constants(float *out, const Shader *shader, const float (*floats)[4], UINT have, DWORD bools) {
  memcpy(out, floats, (SIZE_T)(shader->floats < have ? shader->floats : have) * 16);
  for (UINT i = 0; shader->bools && i < SM_BOOL_REGISTERS; ++i) {
    const DWORD word = (bools >> i & 1) ? 0xFFFFFFFFu : 0u;
    memcpy(out + (shader->floats + i) * 4, &word, 4);
  }
}
/* What a fog mode makes of a depth, as the shaders work it out: `fog` x, y for the linear factor depth * x + y, z, w for the exponential one
 * 2 ^ -(depth * (z + w * depth)); `source` y or z becomes 1 for the one that is used, and w 0. FALSE for no mode. */
static BOOL fog_terms(const State *s, DWORD mode, float fog[4], float source[4]) {
  float start, end, density;
  memcpy(&start, &s->rs[D3DRS_FOGSTART], 4);
  memcpy(&end, &s->rs[D3DRS_FOGEND], 4);
  memcpy(&density, &s->rs[D3DRS_FOGDENSITY], 4);
  if (mode == D3DFOG_LINEAR) {      /* (end - depth) / (end - start) */
    const float range = end - start;
    fog[0] = range != 0.0f ? -1.0f / range : 0.0f;
    fog[1] = range != 0.0f ? end / range : 1.0f;
    source[1] = 1.0f;
  } else if (mode == D3DFOG_EXP || mode == D3DFOG_EXP2) {      /* e ^ -(density * depth), e ^ -(density * depth) ^ 2: as powers of two */
    const float log2_e = 1.44269504f;
    fog[mode == D3DFOG_EXP ? 2 : 3] = (mode == D3DFOG_EXP ? density : density * density) * log2_e;
    source[2] = 1.0f;
  } else return FALSE;
  source[3] = 0.0f;
  return TRUE;
}
/* Whether the fixed-function vertex stage works out the fog factor of this draw: fog is on, by the vertex (no table mode) in a mode, and the
 * positions are not pixels already. (Where fog is by the vertex and this is not so, the factor is the one the vertex brings.) */
static BOOL vertex_fog(const State *s, const Layout *l) {
  return s->rs[D3DRS_FOGENABLE] && s->rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE && s->rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE && !l->transformed;
}
/* The state registers of a pixel shader (sm_to_dxbc.h; the fixed-function one has the same), out of the render states: the alpha test, and fog
 * if the shader does fog (`fog`: not Shader Model 3) - by the depth of the pixel when a table mode is set (its w if `by_w`, else its z), else by
 * the factor the vertex stage gives (`vertex_factor`: it gives one - the fixed-function one always does, 1 where there is no fog by the vertex). */
static void shader_state(float *out, const State *s, BOOL fog, BOOL vertex_factor, BOOL by_w) {
  float state[SM_STATE_REGISTERS][4];
  sm_state_none(state);
  if (s->rs[D3DRS_ALPHATESTENABLE]) {
    /* (D3DCMP_NEVER .. D3DCMP_ALWAYS are 1 .. 8: bit 0 of the number less one keeps what is below, bit 1 what is equal, bit 2 what is above) */
    const DWORD function = s->rs[D3DRS_ALPHAFUNC] >= D3DCMP_NEVER && s->rs[D3DRS_ALPHAFUNC] <= D3DCMP_ALWAYS ? s->rs[D3DRS_ALPHAFUNC] - 1 : 7;
    state[SM_STATE_ALPHA][0] = (float)(s->rs[D3DRS_ALPHAREF] & 0xFF);
    for (UINT i = 0; i < 3; ++i) state[SM_STATE_ALPHA_KEEP][i] = (function >> i & 1) ? 1.0f : 0.0f;
  }
  if (s->rs[D3DRS_FOGENABLE] && fog) {
    const DWORD colour = s->rs[D3DRS_FOGCOLOR];
    state[SM_STATE_FOG_COLOUR][0] = ((colour >> 16) & 255) / 255.0f;
    state[SM_STATE_FOG_COLOUR][1] = ((colour >> 8) & 255) / 255.0f;
    state[SM_STATE_FOG_COLOUR][2] = (colour & 255) / 255.0f;
    if (fog_terms(s, s->rs[D3DRS_FOGTABLEMODE], state[SM_STATE_FOG], state[SM_STATE_FOG_SOURCE])) {
      state[SM_STATE_ALPHA][1] = by_w ? 0.0f : 1.0f;
      state[SM_STATE_ALPHA][2] = by_w ? 1.0f : 0.0f;
    } else if (vertex_factor) {
      state[SM_STATE_FOG_SOURCE][0] = 1.0f;
      state[SM_STATE_FOG_SOURCE][3] = 0.0f;
    }
  }
  memcpy(out, state, sizeof state);
}
/* Whether fog by the pixel's depth goes by w: Direct3D 9 takes the depth before the projection where the projection matrix is a perspective one
 * (its fourth column is not 0 0 0 1), and z where it is not. Vertices that are pixels already bring their w (as 1 / w), and the same holds. */
static BOOL fog_by_w(const State *s) {
  const D3DMATRIX *p = &s->projection;
  return (p->m[0][3] != 0.0f || p->m[1][3] != 0.0f || p->m[2][3] != 0.0f || p->m[3][3] != 1.0f);
}
/* A colour of Direct3D 9 as three numbers. */
static void colour_of(const D3DCOLORVALUE *c, float out[3]) { out[0] = c->r; out[1] = c->g; out[2] = c->b; }
/* What lights the vertices of a draw (ff_lighting.h), out of the state: the matrices, the material and where its colours come from, and the
 * first FFL_LIGHTS lights that shine, brought into view space. */
static void lighting_of(const State *s, FflSetup *out) {
  memset(out, 0, sizeof *out);
  const D3DMATRIX world_view = matrix_multiply(&s->world, &s->view);
  memcpy(out->world_view, world_view.m, sizeof out->world_view);
  ffl_normal_matrix(out->world_view, out->normal);
  const DWORD ambient = s->rs[D3DRS_AMBIENT];
  out->ambient[0] = ((ambient >> 16) & 255) / 255.0f;
  out->ambient[1] = ((ambient >> 8) & 255) / 255.0f;
  out->ambient[2] = (ambient & 255) / 255.0f;
  colour_of(&s->material.Diffuse, out->material_diffuse);
  out->material_diffuse[3] = s->material.Diffuse.a;
  colour_of(&s->material.Ambient, out->material_ambient);
  colour_of(&s->material.Specular, out->material_specular);
  colour_of(&s->material.Emissive, out->material_emissive);
  out->power = s->material.Power;
  if (s->rs[D3DRS_COLORVERTEX]) {      /* (D3DMCS_MATERIAL, _COLOR1, _COLOR2 are 0, 1, 2; without this state all is the material's) */
    out->diffuse_source = (int)s->rs[D3DRS_DIFFUSEMATERIALSOURCE];
    out->ambient_source = (int)s->rs[D3DRS_AMBIENTMATERIALSOURCE];
    out->specular_source = (int)s->rs[D3DRS_SPECULARMATERIALSOURCE];
    out->emissive_source = (int)s->rs[D3DRS_EMISSIVEMATERIALSOURCE];
  }
  out->specular = s->rs[D3DRS_SPECULARENABLE] != 0;
  out->normalize = s->rs[D3DRS_NORMALIZENORMALS] != 0;
  out->local_viewer = s->rs[D3DRS_LOCALVIEWER] != 0;
  for (UINT i = 0; i < s->light_count && out->lights < FFL_LIGHTS; ++i) {
    if (!s->lights[i].enabled) continue;
    const D3DLIGHT9 *from = &s->lights[i].light;
    FflLight *light = &out->light[out->lights++];
    light->type = from->Type == D3DLIGHT_DIRECTIONAL ? FFL_DIRECTIONAL : from->Type == D3DLIGHT_SPOT ? FFL_SPOT : FFL_POINT;
    colour_of(&from->Diffuse, light->diffuse);
    colour_of(&from->Specular, light->specular);
    colour_of(&from->Ambient, light->ambient);
    const float place[3] = {from->Position.x, from->Position.y, from->Position.z}, way[3] = {from->Direction.x, from->Direction.y, from->Direction.z};
    for (int k = 0; k < 3; ++k) {
      light->position[k] = place[0] * s->view.m[0][k] + place[1] * s->view.m[1][k] + place[2] * s->view.m[2][k] + s->view.m[3][k];
      light->to_light[k] = -(way[0] * s->view.m[0][k] + way[1] * s->view.m[1][k] + way[2] * s->view.m[2][k]);
    }
    ffl_normalize(light->to_light);
    light->range = from->Range;
    light->attenuation[0] = from->Attenuation0;
    light->attenuation[1] = from->Attenuation1;
    light->attenuation[2] = from->Attenuation2;
    light->cos_inner = ffl_cos(from->Theta * 0.5f);
    light->cos_outer = ffl_cos(from->Phi * 0.5f);
    light->falloff = from->Falloff;
  }
}
/* How the coordinates of the texture stages are made for a draw, out of the stage states: which set of the vertex's a stage reads or what is
 * generated in its place (D3DTSS_TEXCOORDINDEX), and the stage's texture matrix where D3DTSS_TEXTURETRANSFORMFLAGS asks for it. Positions that
 * are pixels already have their own sets, as they are. */
static void coordinates_of(const State *s, const Layout *l, BOOL for_program, UINT sets, Coordinates *out) {
  memset(out, 0, sizeof *out);
  out->for_program = for_program;
  out->sets = sets;
  out->normalize = s->rs[D3DRS_NORMALIZENORMALS] != 0;
  for (UINT t = 0; t < sets; ++t) {
    const DWORD index = s->stage[t][D3DTSS_TEXCOORDINDEX], flags = s->stage[t][D3DTSS_TEXTURETRANSFORMFLAGS];
    FflTextureStage *stage = &out->stage[t];
    out->set[t] = index & (TEXTURE_SETS - 1);
    if (l->transformed) continue;
    switch (index & 0xFFFF0000u) {
      case D3DTSS_TCI_CAMERASPACENORMAL: stage->from = FFL_FROM_NORMAL; break;
      case D3DTSS_TCI_CAMERASPACEPOSITION: stage->from = FFL_FROM_POSITION; break;
      case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: stage->from = FFL_FROM_REFLECTION; break;
      case D3DTSS_TCI_SPHEREMAP: stage->from = FFL_FROM_SPHERE; break;
      default: break;
    }
    if (stage->from != FFL_FROM_VERTEX) out->needs_normal = 1;
    stage->count = (int)(flags & 0xFF) > 4 ? 4 : (int)(flags & 0xFF);
    stage->projected = (flags & D3DTTFF_PROJECTED) != 0;
    memcpy(stage->matrix, s->texture[t].m, sizeof stage->matrix);
  }
  if (out->needs_normal) {
    const D3DMATRIX world_view = matrix_multiply(&s->world, &s->view);
    memcpy(out->world_view, world_view.m, sizeof out->world_view);
    ffl_normal_matrix(out->world_view, out->normal);
  }
}
/* The registers a draw's block of constants has: the vertex shader's, then the pixel shader's. */
static UINT constant_registers(const Shader *vs, const Shader *ps) { return (vs ? vs->registers : VERTEX_REGISTERS) + ps->registers; }
/* A colour of a render or stage state as four numbers. */
static void colour_of_state(DWORD colour, float out[4]) {
  out[0] = ((colour >> 16) & 255) / 255.0f;
  out[1] = ((colour >> 8) & 255) / 255.0f;
  out[2] = (colour & 255) / 255.0f;
  out[3] = (colour >> 24) / 255.0f;
}
static Tex *stage_texture(const Dev *d, const Shader *ps, UINT i);
/* Writes the block of constants of the draw and gives its address. (The ring has room for a whole block: draw() saw to it.) `ps` is the
 * program's pixel shader, or - `fixed` - the one of the texture stages. */
static UINT64 constants_of(Dev *d, const Layout *l, const Shader *vs, const Shader *ps, BOOL fixed) {
  const State *s = &d->state;
  const UINT vertex_registers = vs ? vs->registers : VERTEX_REGISTERS;
  const UINT bytes = (constant_registers(vs, ps) * 16 + 255) & ~255u;
  float *block = (float *)(d->constant_data + d->constant_used);      /* (written where it will stand, and left there unless it is the last one again) */
  memset(block, 0, bytes);
  if (vs) {
    shader_constants(block, vs, s->vs_float, VS_FLOATS, s->vs_bool);
    /* (its state register: half a pixel to the right and down, in clip space at w = 1 - Direct3D 9's pixel centres, sm_to_dxbc.h) */
    block[vs->state * 4 + 0] = 1.0f / (float)(s->viewport.Width ? s->viewport.Width : 1);
    block[vs->state * 4 + 1] = -1.0f / (float)(s->viewport.Height ? s->viewport.Height : 1);
  } else {
    const D3DMATRIX world_view = matrix_multiply(&s->world, &s->view), all = matrix_multiply(&world_view, &s->projection);
    for (int row = 0; row < 4; ++row)
      for (int column = 0; column < 4; ++column) block[column * 4 + row] = all.m[row][column];      /* (kept column by column) */
    block[16] = 2.0f / (float)s->viewport.Width;
    block[17] = 2.0f / (float)s->viewport.Height;
    block[18] = l->transformed ? 1.0f : 0.0f;
    block[20] = (float)s->viewport.X;
    block[21] = (float)s->viewport.Y;
    /* vertex fog: by the depth in view space (the third column of world * view), or with range fog by the distance from the eye (the first two
     * columns as well), in the mode the render state names; else the factor is 1 */
    block[35] = 1.0f;
    if (vertex_fog(s, l)) {
      for (int row = 0; row < 4; ++row) block[24 + row] = world_view.m[row][2];
      if (s->rs[D3DRS_RANGEFOGENABLE])
        for (int row = 0; row < 4; ++row) {
          block[36 + row] = world_view.m[row][0];
          block[40 + row] = world_view.m[row][1];
        }
      fog_terms(s, s->rs[D3DRS_FOGVERTEXMODE], block + 28, block + 32);
    } else if (s->rs[D3DRS_FOGENABLE] && s->rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE) {      /* the factor the vertex brings */
      block[32] = 1.0f;
      block[35] = 0.0f;
    }
  }
  float *pixel = block + vertex_registers * 4;
  if (fixed) {      /* (ff_stages.h: the texture factor, a register the shader has its own numbers in, and the constants of the stages) */
    float stages[FFS_CONSTANTS][4];
    memset(stages, 0, sizeof stages);
    colour_of_state(s->rs[D3DRS_TEXTUREFACTOR], stages[0]);
    for (UINT stage = 0; stage < STAGES; ++stage) {
      colour_of_state(s->stage[stage][D3DTSS_CONSTANT], stages[2 + stage]);
      /* (the bump environment matrix and luminance scale and offset: floats kept as the bits of the states) */
      memcpy(&stages[FFS_BUMP_MATRIX + stage][0], &s->stage[stage][D3DTSS_BUMPENVMAT00], 4);
      memcpy(&stages[FFS_BUMP_MATRIX + stage][1], &s->stage[stage][D3DTSS_BUMPENVMAT01], 4);
      memcpy(&stages[FFS_BUMP_MATRIX + stage][2], &s->stage[stage][D3DTSS_BUMPENVMAT10], 4);
      memcpy(&stages[FFS_BUMP_MATRIX + stage][3], &s->stage[stage][D3DTSS_BUMPENVMAT11], 4);
      memcpy(&stages[FFS_BUMP_LUMINANCE + stage][0], &s->stage[stage][D3DTSS_BUMPENVLSCALE], 4);
      memcpy(&stages[FFS_BUMP_LUMINANCE + stage][1], &s->stage[stage][D3DTSS_BUMPENVLOFFSET], 4);
    }
    shader_constants(pixel, ps, stages, FFS_CONSTANTS, 0);
  } else {
    shader_constants(pixel, ps, s->ps_float, PS_FLOATS, s->ps_bool);
    /* (a pixel shader 1.x's bump registers: each stage's bump environment matrix, then its luminance scale and offset) */
    for (UINT stage = 0; stage < ps->bump_stages && stage < STAGES; ++stage) {
      float *m = pixel + (ps->bump + 2 * stage) * 4;
      memcpy(&m[0], &s->stage[stage][D3DTSS_BUMPENVMAT00], 4);
      memcpy(&m[1], &s->stage[stage][D3DTSS_BUMPENVMAT01], 4);
      memcpy(&m[2], &s->stage[stage][D3DTSS_BUMPENVMAT10], 4);
      memcpy(&m[3], &s->stage[stage][D3DTSS_BUMPENVMAT11], 4);
      memcpy(&m[4], &s->stage[stage][D3DTSS_BUMPENVLSCALE], 4);
      memcpy(&m[5], &s->stage[stage][D3DTSS_BUMPENVLOFFSET], 4);
    }
  }
  shader_state(pixel + ps->state * 4, s, ps->major < 3, TRUE, fog_by_w(s));
  /* (what each sampler's texture has not of the channels Direct3D 9 reads as 1: sm_to_dxbc.h's fill registers) */
  for (UINT i = 0; i < ps->sampler_count; ++i) format_fill(stage_texture(d, ps, i)->format, pixel + (ps->fill + i) * 4);
  if (!d->constant_valid || d->constant_last_bytes != bytes || memcmp(d->constant_data + d->constant_last, block, bytes) != 0) {
    d->constant_last = d->constant_used;
    d->constant_last_bytes = bytes;
    d->constant_used += bytes;
    d->constant_valid = 1;
  }
  return ID3D12Resource_GetGPUVirtualAddress(d->constant_ring) + d->constant_last;
}

/* Before a draw samples them: the levels of D3DUSAGE_AUTOGENMIPMAP textures whose top level changed are made again, and P8 and A8P8 textures are sent again
 * through the palette that is current now if it is not the one their pixels were sent through. */
static void textures_refresh(Dev *d, const Shader *ps) {
  const UINT count = ps ? texture_slots(ps) : TEXTURE_SLOTS;
  for (UINT i = 0; i < count && i < TEXTURE_SLOTS; ++i) {
    Tex *t = d->state.tex[i];
    if (!t || !t->res) continue;
    if (t->autogen_dirty) tex_generate_levels(d, t);
    if (format_is_palette(t->format) && (t->palette != d->current_palette || t->palette_version != d->palette_version))
      for (UINT sub = 0; sub < tex_subresources(t); ++sub) tex_upload(d, t, sub);
  }
}

/* What stage `i` of a draw with pixel shader `ps` samples: the texture set there when it is of the kind the shader declares (the kernel refuses a draw with a
 * texture of another kind), else a white one of that kind. */
static Tex *stage_texture(const Dev *d, const Shader *ps, UINT i) {
  Tex *t = d->state.tex[i];
  const UINT kind = ps && i < 16 ? ps->sampler_kind[i] : 0;
  if (t && t->res && !t->depth && t->kind == kind) return t;
  return kind && kind < 4 && d->blank_kind[kind] ? d->blank_kind[kind] : d->blank;
}

/* The textures and samplers of the first `slots` stages: a table of each, made again when what is set changed. */
static void tables_of(Dev *d, UINT slots, const Shader *ps) {
  const State *s = &d->state;
  UINT ids[TEXTURE_SLOTS];
  Tex *textures[TEXTURE_SLOTS];
  DWORD samplers[TEXTURE_SLOTS][8];      /* (address modes, magnification, minification and mip filters, anisotropy, border colour, LOD bias, largest level) */
  if (d->table_count != slots) d->tables_valid = 0;
  d->table_count = slots;
  for (UINT i = 0; i < slots; ++i) {
    textures[i] = stage_texture(d, ps, i);
    ids[i] = textures[i]->c.id;
    samplers[i][0] = s->sampler[i][D3DSAMP_ADDRESSU] | s->sampler[i][D3DSAMP_ADDRESSV] << 8;
    samplers[i][1] = s->sampler[i][D3DSAMP_MAGFILTER];
    samplers[i][2] = s->sampler[i][D3DSAMP_MINFILTER];
    samplers[i][3] = s->sampler[i][D3DSAMP_MIPFILTER];
    samplers[i][4] = s->sampler[i][D3DSAMP_MAXANISOTROPY];
    samplers[i][5] = s->sampler[i][D3DSAMP_BORDERCOLOR];
    samplers[i][6] = s->sampler[i][D3DSAMP_MIPMAPLODBIAS];
    samplers[i][7] = s->sampler[i][D3DSAMP_MAXMIPLEVEL];
  }
  const UINT step_views = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT step_samplers = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  if (!d->tables_valid || memcmp(ids, d->table_ids, slots * sizeof ids[0]) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->view_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->view_heap);
    at.ptr += (SIZE_T)d->view_used * step_views;
    d->table_views = gpu.ptr + (UINT64)d->view_used * step_views;
    for (UINT i = 0; i < slots; ++i) {
      ID3D12Device_CreateShaderResourceView(d->d12, textures[i]->res, NULL, at);
      at.ptr += step_views;
    }
    d->view_used += slots;
    memcpy(d->table_ids, ids, slots * sizeof ids[0]);
  }
  if (!d->tables_valid || memcmp(samplers, d->table_sampler_states, slots * sizeof samplers[0]) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->sampler_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->sampler_heap);
    at.ptr += (SIZE_T)d->sampler_used * step_samplers;
    d->table_samplers = gpu.ptr + (UINT64)d->sampler_used * step_samplers;
    for (UINT i = 0; i < slots; ++i) {
      D3D12_SAMPLER_DESC desc;
      memset(&desc, 0, sizeof desc);
      /* (D3D12_FILTER: bit 4 a linear minification, bit 2 a linear magnification, bit 0 a linear step between levels) */
      desc.Filter = (D3D12_FILTER)((samplers[i][2] >= D3DTEXF_LINEAR ? 0x10 : 0) | (samplers[i][1] >= D3DTEXF_LINEAR ? 0x4 : 0) | (samplers[i][3] >= D3DTEXF_LINEAR ? 0x1 : 0));
      /* (an anisotropic minification filter with D3DSAMP_MAXANISOTROPY above 1: Direct3D 12's anisotropic filter, linear between the levels when there are levels) */
      const DWORD anisotropy = samplers[i][4] < 1 ? 1 : samplers[i][4] > 16 ? 16 : samplers[i][4];
      if (samplers[i][2] == D3DTEXF_ANISOTROPIC && anisotropy > 1) desc.Filter = D3D12_FILTER_ANISOTROPIC;
      desc.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)op_of(samplers[i][0] & 0xFF, 1, 5, D3DTADDRESS_WRAP);      /* (the same numbers) */
      desc.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)op_of(samplers[i][0] >> 8, 1, 5, D3DTADDRESS_WRAP);
      desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      desc.MaxAnisotropy = desc.Filter == D3D12_FILTER_ANISOTROPIC ? anisotropy : 1;
      desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      /* (D3DSAMP_BORDERCOLOR is a D3DCOLOR: alpha, red, green, blue from the top byte down) */
      const DWORD border = samplers[i][5];
      desc.BorderColor[0] = (float)((border >> 16) & 0xFF) / 255.0f, desc.BorderColor[1] = (float)((border >> 8) & 0xFF) / 255.0f;
      desc.BorderColor[2] = (float)(border & 0xFF) / 255.0f, desc.BorderColor[3] = (float)(border >> 24) / 255.0f;
      /* (D3DSAMP_MIPMAPLODBIAS is a float; D3DSAMP_MAXMIPLEVEL the most detailed level the sampler reads, which is a least level of detail) */
      float bias;
      memcpy(&bias, &samplers[i][6], 4);
      desc.MipLODBias = bias == bias ? bias : 0.0f;
      desc.MinLOD = samplers[i][3] == D3DTEXF_NONE ? 0.0f : (float)(samplers[i][7] > 15 ? 15 : samplers[i][7]);
      desc.MaxLOD = samplers[i][3] == D3DTEXF_NONE ? 0.0f : 1000.0f;
      ID3D12Device_CreateSampler(d->d12, &desc, at);
      at.ptr += step_samplers;
    }
    d->sampler_used += slots;
    memcpy(d->table_sampler_states, samplers, slots * sizeof samplers[0]);
  }
  d->tables_valid = 1;
}

/* ---- a draw ----------------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d) {
  for (UINT i = 0; i < d->clear_count; ++i) {
    const PendingClear *p = &d->clears[i];
    ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, p->depth->view, (D3D12_CLEAR_FLAGS)p->flags, p->z, p->stencil, 0, NULL);
    p->depth->used = d->epoch;      /* (the list uses it now: a release, or a CPU write, waits for the list) */
    d->recorded = 1;
  }
}

/* The vertices or indices `primitives` primitives of a type read; 0 for a type that is not one. */
static UINT primitive_elements(D3DPRIMITIVETYPE type, UINT primitives) {
  if (primitives == 0 || primitives > 0x00FFFFFFu) return 0;
  switch (type) {
    case D3DPT_POINTLIST: return primitives;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    case D3DPT_TRIANGLELIST: return primitives * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primitives + 2;
    default: return 0;
  }
}

/* One draw. `sources`: the streams. The vertices it reads are `first_vertex .. first_vertex + vertices - 1` of the streams. `indices` (or NULL):
 * `elements` indices of `index_bytes` bytes each, which count from the vertex `index_base` of the streams - an index i reads vertex index_base + i,
 * and first_vertex is the lowest such vertex. Without indices the draw reads `elements` vertices from first_vertex on. */
static HRESULT draw(Dev *d, D3DPRIMITIVETYPE type, UINT primitives, const Source *sources, UINT first_vertex, UINT vertices, const BYTE *indices, UINT index_bytes, INT index_base) {
  const UINT elements = primitive_elements(type, primitives);
  if (!elements || !d->target[0]) return D3DERR_INVALIDCALL;
  Layout layout;
  D3DVERTEXELEMENT9 fvf[16];
  const D3DVERTEXELEMENT9 *declared = d->state.decl ? d->state.decl->elements : fvf;
  if (d->state.decl) layout = d->state.decl->layout;
  else layout_of_elements(fvf, fvf_elements(d->state.fvf, fvf), &layout);
  /* The shaders of the draw. Positions that are pixels already pass no vertex shader, as Direct3D 9 has it. */
  const Shader *vs = layout.transformed ? NULL : d->state.vs, *ps = d->state.ps ? d->state.ps : fixed_shader_get(d);
  if (!ps) return D3DERR_INVALIDCALL;
  const UINT sets = coordinate_sets(ps);
  Attribute inputs[SHADER_INPUTS];
  if (!indices) vertices = elements;
  /* (no declaration and no FVF code: Direct3D 9 refuses the draw. Vertices a stream does not have - no buffer set, or one too short - are not read: Direct3D 9's
   * runtime takes the draw as it is, and here it draws nothing) */
  if (!d->state.decl && !d->state.fvf) return D3DERR_INVALIDCALL;
  if (vs) {
    shader_attributes(vs, declared, inputs);
    for (UINT i = 0; i < vs->input_count; ++i)
      if (!attribute_inside(&inputs[i], sources, first_vertex, vertices)) return D3D_OK;
  } else {
    if (layout.position.type == 255) return D3D_OK;
    if (!attribute_inside(&layout.position, sources, first_vertex, vertices) || !attribute_inside(&layout.colour, sources, first_vertex, vertices) ||
        !attribute_inside(&layout.specular, sources, first_vertex, vertices) || !attribute_inside(&layout.normal, sources, first_vertex, vertices) ||
        !attribute_inside(&layout.weights, sources, first_vertex, vertices) || !attribute_inside(&layout.indices, sources, first_vertex, vertices) ||
        !attribute_inside(&layout.position1, sources, first_vertex, vertices) || !attribute_inside(&layout.normal1, sources, first_vertex, vertices))
      return D3D_OK;
    for (UINT t = 0; t < sets; ++t)
      if (!attribute_inside(&layout.texture[d->state.stage[t][D3DTSS_TEXCOORDINDEX] & (TEXTURE_SETS - 1)], sources, first_vertex, vertices)) return D3D_OK;
  }
  const UINT stride = vs ? 16 * (vs->input_count ? vs->input_count : 1) : VERTEX_BYTES(sets);
  const UINT slots = texture_slots(ps);
  const BOOL fan = type == D3DPT_TRIANGLEFAN;
  const UINT out_indices = fan ? primitives * 3 : indices ? elements : 0;
  /* D3DSHADE_FLAT (the fixed-function vertex stage): each primitive's vertices are copied with the colours of its first vertex - the second of a fan's
   * triangle, as Direct3D 9 has it - and drawn as a list (the colours are interpolated otherwise; there is no provoking-vertex state below). */
  const BOOL flat = !vs && d->state.rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT && type != D3DPT_POINTLIST;
  const UINT flat_vertices = flat ? (type == D3DPT_LINELIST || type == D3DPT_LINESTRIP ? primitives * 2 : primitives * 3) : 0;
  if (vertices > VERTEX_RING / stride || flat_vertices > VERTEX_RING / stride - vertices || out_indices > INDEX_RING / 4 || constant_registers(vs, ps) > CONSTANT_REGISTERS)
    return D3DERR_INVALIDCALL;

  /* what may have to send the list first */
  settle(d, d->target[0]->tex->sampled);      /* (a texture the list's draws sampled cannot become its target) */
  for (UINT i = 1; i < TARGETS && d->target[i]; ++i) settle(d, d->target[i]->tex->sampled);
  textures_refresh(d, ps);
  if (d->vertex_used + (vertices + flat_vertices) * stride > VERTEX_RING || d->index_used + out_indices * 4 > INDEX_RING || d->constant_used + ((constant_registers(vs, ps) * 16 + 255) & ~255u) > CONSTANT_RING ||
      d->view_used + slots > VIEW_HEAP || d->sampler_used + slots > VIEW_HEAP || (d->open_queries && queries_need_slots(d)))
    flush(d);
  const D3DPRIMITIVETYPE draw_type = !flat ? type : type == D3DPT_LINELIST || type == D3DPT_LINESTRIP ? D3DPT_LINELIST : D3DPT_TRIANGLELIST;
  ID3D12PipelineState *pipeline = pipeline_get(d, vs, ps, draw_type);
  if (!pipeline) return D3DERR_INVALIDCALL;

  /* the vertices, and the indices counted from the first of them */
  const UINT vertex_at = d->vertex_used, index_at = d->index_used;
  /* (vertex blending: the vertices come out in world space, and the draw's world matrix is the identity; it is put back at the end) */
  Blend *blend = NULL;
  D3DMATRIX world = d->state.world[0];
  if (!vs && !layout.transformed) {
    const DWORD mode = d->state.rs[D3DRS_VERTEXBLEND];
    const UINT matrices = mode == D3DVBF_0WEIGHTS ? 1u : mode >= D3DVBF_1WEIGHTS && mode <= D3DVBF_3WEIGHTS ? (UINT)mode + 1u : 0u;
    if (matrices || mode == D3DVBF_TWEENING) {
      blend = mem_alloc(sizeof *blend);
      if (!blend) return E_OUTOFMEMORY;
      blend->matrices = mode == D3DVBF_TWEENING ? 0 : matrices;
      blend->indexed = d->state.rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0;
      blend->tween = mode == D3DVBF_TWEENING;
      memcpy(&blend->tween_factor, &d->state.rs[D3DRS_TWEENFACTOR], 4);
      blend->world = d->state.world;
      if (!blend->tween) d->state.world[0] = g_identity;
    }
  }
  if (vs) decode_shader_vertices(inputs, vs->input_count, sources, first_vertex, vertices, d->vertex_data + vertex_at);
  else {
    /* (lighting is the fixed-function vertex stage's, for positions that are not pixels already) */
    FflSetup lighting;
    const BOOL lit = d->state.rs[D3DRS_LIGHTING] && !layout.transformed;
    if (lit) lighting_of(&d->state, &lighting);
    Coordinates coordinates;
    coordinates_of(&d->state, &layout, d->state.ps != NULL, sets, &coordinates);
    if (blend && !blend->tween) {
      /* (the matrices are read from a copy: the device's world matrix 0 is the identity for this draw) */
      memcpy(blend->copy, d->state.world, sizeof blend->copy);
      blend->copy[0] = world;
      blend->world = blend->copy;
      decode_vertices(&layout, sources, first_vertex, vertices, d->vertex_data + vertex_at, lit ? &lighting : NULL, &coordinates, blend);
    } else decode_vertices(&layout, sources, first_vertex, vertices, d->vertex_data + vertex_at, lit ? &lighting : NULL, &coordinates, blend);
  }
  d->vertex_used += vertices * stride;
  if (out_indices) {
    UINT *out = (UINT *)(d->index_data + index_at);
    const INT lowest = (INT)first_vertex - index_base;      /* (the index that reads the first decoded vertex) */
    for (UINT i = 0; i < out_indices; ++i) {
      /* (a fan: the triangles 0 k k+1) */
      const UINT element = !fan ? i : i % 3 == 0 ? 0 : i / 3 + i % 3;
      UINT value = element;
      if (indices) value = index_bytes == 4 ? ((const UINT *)indices)[element] : ((const WORD *)indices)[element];
      const INT64 vertex = indices ? (INT64)value - lowest : (INT64)value;
      out[i] = vertex >= 0 && vertex < (INT64)vertices ? (UINT)vertex : 0;      /* (an index outside what the program said the draw reads) */
    }
    d->index_used += out_indices * 4;
  }
  UINT flat_at = 0;
  if (flat) {
    /* (the decoded vertices of each primitive in turn, with the provoking vertex's colour and specular colour; the fog factor stays each vertex's) */
    flat_at = d->vertex_used;
    BYTE *base = d->vertex_data + vertex_at, *out = d->vertex_data + flat_at;
    const UINT per = flat_vertices / primitives;
    const INT lowest = (INT)first_vertex - index_base;
    for (UINT p = 0; p < primitives; ++p) {
      UINT element[3];
      if (type == D3DPT_LINELIST) element[0] = 2 * p, element[1] = 2 * p + 1;
      else if (type == D3DPT_LINESTRIP) element[0] = p, element[1] = p + 1;
      else if (type == D3DPT_TRIANGLELIST) element[0] = 3 * p, element[1] = 3 * p + 1, element[2] = 3 * p + 2;
      else if (type == D3DPT_TRIANGLESTRIP) {      /* (every other triangle of a strip turns the other way: as a list it keeps its winding) */
        element[0] = p;
        element[1] = (p & 1) ? p + 2 : p + 1;
        element[2] = (p & 1) ? p + 1 : p + 2;
      } else element[0] = 0, element[1] = p + 1, element[2] = p + 2;
      UINT vertex[3];
      for (UINT k = 0; k < per; ++k) {
        UINT value = element[k];
        if (indices) value = index_bytes == 4 ? ((const UINT *)indices)[element[k]] : ((const WORD *)indices)[element[k]];
        const INT64 v = indices ? (INT64)value - lowest : (INT64)value;
        vertex[k] = v >= 0 && v < (INT64)vertices ? (UINT)v : 0;
      }
      const UINT provoking = type == D3DPT_TRIANGLEFAN ? vertex[1] : vertex[0];
      for (UINT k = 0; k < per; ++k) {
        BYTE *o = out + (SIZE_T)(p * per + k) * stride;
        memcpy(o, base + (SIZE_T)vertex[k] * stride, stride);
        memcpy(o + 16, base + (SIZE_T)provoking * stride + 16, 16);      /* (the colour) */
        memcpy(o + 36, base + (SIZE_T)provoking * stride + 36, 12);      /* (the specular colour, after the fog factor) */
      }
    }
    d->vertex_used += flat_vertices * stride;
  }

  if (d->open_queries) queries_before_draw(d);      /* (occlusion queries count from the first draw after their begin) */
  ID3D12GraphicsCommandList *list = d->list;
  Tex *target = d->target[0]->tex;
  const BOOL depth = d->depth != NULL;
  if (d->targets_dirty) {
    const State *s = &d->state;
    ID3D12DescriptorHeap *heaps[2] = {d->view_heap, d->sampler_heap};
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    Surface *targets[TARGETS];
    D3D12_CPU_DESCRIPTOR_HANDLE target_views[TARGETS];
    const UINT target_count = draw_targets(d, targets);
    for (UINT i = 0; i < target_count; ++i) {
      target_views[i] = surface_target_view(d, targets[i]);
      if (!target_views[i].ptr) return E_OUTOFMEMORY;
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, target_count, target_views, FALSE, depth ? &d->depth->tex->view : NULL);
    D3D12_VIEWPORT vp = {(FLOAT)s->viewport.X, (FLOAT)s->viewport.Y, (FLOAT)s->viewport.Width, (FLOAT)s->viewport.Height, s->viewport.MinZ, s->viewport.MaxZ};
    D3D12_RECT sc = {(LONG)s->viewport.X, (LONG)s->viewport.Y, (LONG)(s->viewport.X + s->viewport.Width), (LONG)(s->viewport.Y + s->viewport.Height)};
    if (s->rs[D3DRS_SCISSORTESTENABLE]) {      /* (the rectangle, inside the viewport) */
      if (s->scissor.left > sc.left) sc.left = s->scissor.left;
      if (s->scissor.top > sc.top) sc.top = s->scissor.top;
      if (s->scissor.right < sc.right) sc.right = s->scissor.right;
      if (s->scissor.bottom < sc.bottom) sc.bottom = s->scissor.bottom;
      if (sc.right < sc.left) sc.right = sc.left;
      if (sc.bottom < sc.top) sc.bottom = sc.top;
    }
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    d->targets_dirty = 0;
  }
  ID3D12RootSignature *root = root_get(d, slots);      /* (there: the pipeline was made with it) */
  if (d->root_set != root) {
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    d->root_set = root;
  }
  ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, draw_type == D3DPT_POINTLIST ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : draw_type == D3DPT_LINELIST ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                         : draw_type == D3DPT_LINESTRIP ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
                                                         : draw_type == D3DPT_TRIANGLESTRIP ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_OMSetStencilRef(list, d->state.rs[D3DRS_STENCILREF] & 0xFF);
  {
    /* (D3DRS_BLENDFACTOR: a D3DCOLOR, alpha in the top byte) */
    const DWORD c = d->state.rs[D3DRS_BLENDFACTOR];
    const float factor[4] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f};
    ID3D12GraphicsCommandList_OMSetBlendFactor(list, factor);
  }
  D3D12_VERTEX_BUFFER_VIEW vertex_view = {ID3D12Resource_GetGPUVirtualAddress(d->vertex_ring) + (flat ? flat_at : vertex_at), (flat ? flat_vertices : vertices) * stride, stride};
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vertex_view);
  ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, constants_of(d, &layout, vs, ps, d->state.ps == NULL));
  if (blend) {
    d->state.world[0] = world;
    mem_free(blend);
  }
  tables_of(d, slots, ps);
  D3D12_GPU_DESCRIPTOR_HANDLE views = {d->table_views}, samplers = {d->table_samplers};
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, views);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, samplers);
  if (flat) {
    ID3D12GraphicsCommandList_DrawInstanced(list, flat_vertices, 1, 0, 0);
  } else if (out_indices) {
    D3D12_INDEX_BUFFER_VIEW index_view = {ID3D12Resource_GetGPUVirtualAddress(d->index_ring) + index_at, out_indices * 4, DXGI_FORMAT_R32_UINT};
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &index_view);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, out_indices, 1, 0, 0, 0);
  } else {
    ID3D12GraphicsCommandList_DrawInstanced(list, vertices, 1, 0, 0);
  }

  for (UINT i = 0; i < slots; ++i)
    if (d->state.tex[i]) ((Tex *)d->state.tex[i])->used = ((Tex *)d->state.tex[i])->sampled = d->epoch;
  target->used = d->epoch;
  for (UINT i = 1; i < TARGETS && d->target[i]; ++i) d->target[i]->tex->used = d->epoch;
  if (depth) {
    d->depth->tex->used = d->epoch;
    for (UINT i = 0; i < d->clear_count;) {      /* (its clear is done by the pass this draw begins) */
      if (d->clears[i].depth == d->depth->tex) d->clears[i] = d->clears[--d->clear_count]; else ++i;
    }
  }
  d->recorded = 1;
  return D3D_OK;
}

/* The streams of a draw; a stream of instance data (D3DSTREAMSOURCE_INSTANCEDATA) gives every vertex its element of instance 0 (instance_sources). */
static void stream_sources(Dev *d, Source *sources) {
  for (UINT i = 0; i < STREAMS; ++i) {
    const Stream *s = &d->state.stream[i];
    sources[i].data = s->buffer && s->offset < s->buffer->length ? s->buffer->data + s->offset : NULL;
    sources[i].bytes = sources[i].data ? s->buffer->length - s->offset : 0;
    sources[i].stride = s->stride;
    sources[i].instanced = (s->freq & D3DSTREAMSOURCE_INSTANCEDATA) != 0;
    sources[i].element = 0;
  }
}
/* The streams of instance data at instance `instance`: element instance / n of a stream of D3DSTREAMSOURCE_INSTANCEDATA | n (n = 0: element 0, it does not step). */
static void instance_sources(Dev *d, Source *sources, UINT instance) {
  for (UINT i = 0; i < STREAMS; ++i) {
    const UINT n = d->state.stream[i].freq & ~(D3DSTREAMSOURCE_INDEXEDDATA | D3DSTREAMSOURCE_INSTANCEDATA);
    if (sources[i].instanced) sources[i].element = n ? instance / n : 0;
  }
}

static HRESULT STDMETHODCALLTYPE dev_DrawPrimitive(Dev *d, D3DPRIMITIVETYPE type, UINT start, UINT primitives) {
  Source sources[STREAMS];
  EnterCriticalSection(&d->lock);
  stream_sources(d, sources);
  const HRESULT hr = draw(d, type, primitives, sources, start, 0, NULL, 0, 0);
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_DrawIndexedPrimitive(Dev *d, D3DPRIMITIVETYPE type, INT base, UINT lowest, UINT vertices, UINT start, UINT primitives) {
  Source sources[STREAMS];
  const Buffer *ib = d->state.indices;
  const UINT elements = primitive_elements(type, primitives);
  if (!elements || vertices == 0 || (!d->state.decl && !d->state.fvf)) return D3DERR_INVALIDCALL;
  if (!ib) return D3D_OK;      /* (no index buffer, with a declaration: Windows draws nothing and succeeds) */
  const UINT index_bytes = ib->format == D3DFMT_INDEX32 ? 4 : 2;
  if (start > ib->length / index_bytes || elements > ib->length / index_bytes - start) return D3DERR_INVALIDCALL;
  if ((INT64)base + lowest < 0) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  stream_sources(d, sources);
  /* Instancing: stream 0's D3DSTREAMSOURCE_INDEXEDDATA | n draws the indices n times (n = 0: no instance), the instance data streams stepping with the instances. */
  const DWORD freq0 = d->state.stream[0].freq;
  const UINT instances = freq0 & D3DSTREAMSOURCE_INDEXEDDATA ? freq0 & ~D3DSTREAMSOURCE_INDEXEDDATA : 1;
  HRESULT hr = D3D_OK;
  for (UINT instance = 0; instance < instances && SUCCEEDED(hr); ++instance) {
    instance_sources(d, sources, instance);
    hr = draw(d, type, primitives, sources, (UINT)(base + (INT)lowest), vertices, ib->data + (SIZE_T)start * index_bytes, index_bytes, base);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* (the "UP" draws take the vertices from the program's memory, as stream 0, and leave stream 0 and the indices unset) */
static HRESULT STDMETHODCALLTYPE dev_DrawPrimitiveUP(Dev *d, D3DPRIMITIVETYPE type, UINT primitives, const void *data, UINT stride) {
  const UINT elements = primitive_elements(type, primitives);
  if (!data || !stride || !elements) return D3DERR_INVALIDCALL;
  Source sources[STREAMS];
  memset(sources, 0, sizeof sources);
  sources[0].data = data;
  sources[0].bytes = elements * stride;
  sources[0].stride = stride;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = draw(d, type, primitives, sources, 0, 0, NULL, 0, 0);
  bind(&d->state.stream[0].buffer, NULL);
  if (SUCCEEDED(hr)) d->state.stream[0].offset = d->state.stream[0].stride = 0;      /* (GetStreamSource then gives NULL, 0, 0; a draw refused keeps them, as on Windows) */
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_DrawIndexedPrimitiveUP(Dev *d, D3DPRIMITIVETYPE type, UINT lowest, UINT vertices, UINT primitives, const void *indices, D3DFORMAT index_format,
                                                           const void *data, UINT stride) {
  if (!data || !stride || !indices || vertices == 0 || vertices > 0x00FFFFFFu || lowest > 0x00FFFFFFu || (index_format != D3DFMT_INDEX16 && index_format != D3DFMT_INDEX32))
    return D3DERR_INVALIDCALL;
  Source sources[STREAMS];
  memset(sources, 0, sizeof sources);
  sources[0].data = data;
  sources[0].bytes = (lowest + vertices) * stride;
  sources[0].stride = stride;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = draw(d, type, primitives, sources, lowest, vertices, indices, index_format == D3DFMT_INDEX32 ? 4 : 2, 0);
  bind(&d->state.stream[0].buffer, NULL);
  if (SUCCEEDED(hr)) d->state.stream[0].offset = d->state.stream[0].stride = 0;
  bind(&d->state.indices, NULL);
  LeaveCriticalSection(&d->lock);
  return hr;
}

/* ProcessVertices: the fixed-function vertex stage on the CPU, its results written into a vertex buffer - what the draws would give the rasterizer: positions
 * in pixels (x, y, z and 1 / w) out of the world, view and projection matrices and the viewport, the colours lit as the lights say (or the vertex's own), vertex
 * blending and tweening, the vertex fog factor in the specular colour's alpha, the texture coordinates as the vertex has them, and the point size.
 * The output is the declaration's (`declaration`) or else the destination buffer's FVF code; a part the input has not comes out as Direct3D 9 makes it (white,
 * black specular, zero). With D3DRS_CLIPPING a vertex outside the clip volume is not made pixels, as Direct3D 9 (and wined3d) has it: its x and y are
 * (x + w) / 2 and (y + w) / 2 of clip space and its z and w are kept; the clip status gets every vertex's clip codes. D3DPV_DONOTCOPYDATA: only the positions
 * are written. A program's vertex shader is not run here: that needs a CPU interpreter of the shader models, which there is not (D3DERR_INVALIDCALL). */
typedef struct PvOutput {
  int position;                  /* the offset of each part, -1 where the output has none */
  BOOL rhw;                      /* a position of four (x, y, z, 1 / w), else three */
  int diffuse, specular, point_size, normal;
  int texture[TEXTURE_SETS];
  BYTE texture_type[TEXTURE_SETS];
  BYTE colour_type[2];           /* D3DDECLTYPE_D3DCOLOR, or a float type of a declaration */
  UINT stride;
} PvOutput;
/* What the output of ProcessVertices is, from the elements of stream 0 of a declaration (or of an FVF code); FALSE when it has no position. */
static BOOL pv_output_of(const D3DVERTEXELEMENT9 *e, UINT count, PvOutput *o) {
  memset(o, 0xFF, sizeof *o);
  o->rhw = FALSE;
  o->stride = 0;
  for (UINT i = 0; i < count && e[i].Stream != 0xFF; ++i) {
    if (e[i].Stream != 0 || e[i].Type > D3DDECLTYPE_FLOAT16_4) continue;
    const UINT end = e[i].Offset + type_bytes(e[i].Type);
    if (end > o->stride) o->stride = end;
    const int at = e[i].Offset;
    switch (e[i].Usage) {
      case D3DDECLUSAGE_POSITIONT:
      case D3DDECLUSAGE_POSITION:
        if (e[i].UsageIndex != 0 || (e[i].Type != D3DDECLTYPE_FLOAT3 && e[i].Type != D3DDECLTYPE_FLOAT4)) break;
        o->position = at;
        o->rhw = e[i].Type == D3DDECLTYPE_FLOAT4;
        break;
      case D3DDECLUSAGE_COLOR:
        if (e[i].UsageIndex > 1 || (e[i].Type != D3DDECLTYPE_D3DCOLOR && e[i].Type != D3DDECLTYPE_FLOAT4 && e[i].Type != D3DDECLTYPE_FLOAT3)) break;
        if (e[i].UsageIndex == 0) o->diffuse = at; else o->specular = at;
        o->colour_type[e[i].UsageIndex] = e[i].Type;
        break;
      case D3DDECLUSAGE_PSIZE:
        if (e[i].Type == D3DDECLTYPE_FLOAT1) o->point_size = at;
        break;
      case D3DDECLUSAGE_NORMAL:
        if (e[i].UsageIndex == 0 && e[i].Type == D3DDECLTYPE_FLOAT3) o->normal = at;
        break;
      case D3DDECLUSAGE_TEXCOORD:
        if (e[i].UsageIndex < TEXTURE_SETS && e[i].Type <= D3DDECLTYPE_FLOAT4) {
          o->texture[e[i].UsageIndex] = at;
          o->texture_type[e[i].UsageIndex] = e[i].Type;
        }
        break;
      default: break;
    }
  }
  return o->position >= 0;
}
static void pv_put_floats(BYTE *at, const float *v, UINT n) { memcpy(at, v, n * sizeof(float)); }
static void pv_put_colour(BYTE *at, BYTE type, const float c[4]) {
  if (type != D3DDECLTYPE_D3DCOLOR) {
    pv_put_floats(at, c, type == D3DDECLTYPE_FLOAT3 ? 3 : 4);
    return;
  }
  DWORD v = 0;
  const int order[4] = {2, 1, 0, 3};      /* (a D3DCOLOR: blue in the low byte, alpha in the top one) */
  for (int k = 0; k < 4; ++k) {
    const float x = ffl_clamp(c[order[k]]);
    v |= (DWORD)(x * 255.0f + 0.5f) << (8 * k);
  }
  memcpy(at, &v, 4);
}
/* The clip codes of a position in clip space (D3DCS_*): the six planes of the volume, and the program's clip planes that are on. */
static DWORD pv_clip_codes(const State *s, const float c[4], const float world[4]) {
  DWORD codes = 0;
  if (c[0] < -c[3]) codes |= D3DCS_LEFT;
  if (c[0] > c[3]) codes |= D3DCS_RIGHT;
  if (c[1] < -c[3]) codes |= D3DCS_BOTTOM;
  if (c[1] > c[3]) codes |= D3DCS_TOP;
  if (c[2] < 0.0f) codes |= D3DCS_FRONT;
  if (c[2] > c[3]) codes |= D3DCS_BACK;
  for (UINT i = 0; i < 6; ++i) {
    if (!(s->rs[D3DRS_CLIPPLANEENABLE] >> i & 1)) continue;
    const float *p = s->clip_plane[i];
    /* (the planes are in world space for the fixed-function stage) */
    if (p[0] * world[0] + p[1] * world[1] + p[2] * world[2] + p[3] * world[3] < 0.0f) codes |= D3DCS_PLANE0 << i;
  }
  return codes;
}
static HRESULT STDMETHODCALLTYPE dev_ProcessVertices(Dev *d, UINT src_start, UINT dst_index, UINT count, IDirect3DVertexBuffer9 *dst_buffer,
                                                     IDirect3DVertexDeclaration9 *declaration, DWORD flags) {
  Buffer *out = vb_of(dst_buffer);
  Decl *out_decl = decl_of(declaration);
  if (!out || (declaration && !out_decl) || (flags & ~(DWORD)D3DPV_DONOTCOPYDATA)) return D3DERR_INVALIDCALL;
  D3DVERTEXELEMENT9 out_elements[MAX_ELEMENTS + 1];
  PvOutput o;
  const BOOL have = out_decl ? pv_output_of(out_decl->elements, out_decl->count, &o) : pv_output_of(out_elements, fvf_elements(out->fvf, out_elements), &o);
  if (!have || o.stride == 0) return D3DERR_INVALIDCALL;
  if (count == 0) return D3D_OK;
  if (dst_index > out->length / o.stride || count > out->length / o.stride - dst_index) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  State *s = &d->state;
  HRESULT hr = D3D_OK;
  Layout layout;
  D3DVERTEXELEMENT9 fvf[16];
  if (s->decl) layout = s->decl->layout;
  else layout_of_elements(fvf, fvf_elements(s->fvf, fvf), &layout);
  Source sources[STREAMS];
  stream_sources(d, sources);
  /* (what the vertices have not - no declaration, no position, a stream too short - is refused, as is a program's vertex shader: see above) */
  BOOL inside = (s->decl || s->fvf) && !s->vs && layout.position.type != 255;
  const Attribute *parts[] = {&layout.position, &layout.colour, &layout.specular, &layout.normal, &layout.weights, &layout.indices, &layout.position1, &layout.normal1,
                              &layout.point_size};
  for (UINT i = 0; inside && i < sizeof parts / sizeof parts[0]; ++i) inside = attribute_inside(parts[i], sources, src_start, count);
  for (UINT t = 0; inside && t < TEXTURE_SETS; ++t) inside = o.texture[t] < 0 || attribute_inside(&layout.texture[t], sources, src_start, count);
  BYTE *decoded = inside ? mem_alloc((SIZE_T)count * VERTEX_BYTES(0)) : NULL;
  Blend *blend = NULL;
  const D3DMATRIX world = s->world[0];
  if (!inside) hr = D3DERR_INVALIDCALL;
  else if (!decoded) hr = E_OUTOFMEMORY;
  else if (!layout.transformed) {
    const DWORD mode = s->rs[D3DRS_VERTEXBLEND];
    const UINT matrices = mode == D3DVBF_0WEIGHTS ? 1u : mode >= D3DVBF_1WEIGHTS && mode <= D3DVBF_3WEIGHTS ? (UINT)mode + 1u : 0u;
    if (matrices || mode == D3DVBF_TWEENING) {
      blend = mem_alloc(sizeof *blend);
      if (!blend) hr = E_OUTOFMEMORY;
      else {
        blend->matrices = mode == D3DVBF_TWEENING ? 0 : matrices;
        blend->indexed = s->rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0;
        blend->tween = mode == D3DVBF_TWEENING;
        memcpy(&blend->tween_factor, &s->rs[D3DRS_TWEENFACTOR], 4);
        memcpy(blend->copy, s->world, sizeof blend->copy);
        blend->world = blend->copy;
        if (!blend->tween) s->world[0] = g_identity;      /* (blended into world space: the world matrix is the identity for what follows) */
      }
    }
  }
  if (SUCCEEDED(hr)) {
    FflSetup lighting;
    const BOOL lit = s->rs[D3DRS_LIGHTING] && !layout.transformed;
    if (lit) lighting_of(s, &lighting);
    Coordinates coordinates;
    coordinates_of(s, &layout, FALSE, 0, &coordinates);
    decode_vertices(&layout, sources, src_start, count, decoded, lit ? &lighting : NULL, &coordinates, blend);
    const D3DMATRIX world_view = matrix_multiply(&s->world[0], &s->view), all = matrix_multiply(&world_view, &s->projection);
    const D3DVIEWPORT9 *vp = &s->viewport;
    const BOOL clipping = s->rs[D3DRS_CLIPPING] != 0, fog = vertex_fog(s, &layout);
    float start, end, density, point_size;
    memcpy(&start, &s->rs[D3DRS_FOGSTART], 4);
    memcpy(&end, &s->rs[D3DRS_FOGEND], 4);
    memcpy(&density, &s->rs[D3DRS_FOGDENSITY], 4);
    memcpy(&point_size, &s->rs[D3DRS_POINTSIZE], 4);
    DWORD clip_union = 0, clip_intersection = 0xFFFFFFFFu;
    BYTE *to = out->data + (SIZE_T)dst_index * o.stride;
    for (UINT v = 0; v < count; ++v, to += o.stride) {
      const float *f = (const float *)(decoded + (SIZE_T)v * VERTEX_BYTES(0));
      float position[4] = {f[0], f[1], f[2], f[3]};
      if (!layout.transformed) {
        const float p[4] = {f[0], f[1], f[2], 1.0f};
        float c[4];
        blend_point(&all, p, c);
        if (clipping) {
          const DWORD codes = pv_clip_codes(s, c, p);
          clip_union |= codes;
          clip_intersection &= codes;
        }
        const float eps = 1e-8f;
        if (!clipping || (-c[3] - eps < c[0] && -c[3] - eps < c[1] && c[0] - eps < c[3] && c[1] - eps < c[3] && c[2] < c[3] + eps && c[2] > -eps && c[3] > eps)) {
          const float inverse = c[3] != 0.0f ? 1.0f / c[3] : 1.0f;
          position[0] = c[0] * inverse * (vp->Width / 2.0f) + vp->Width / 2.0f + vp->X;
          position[1] = -c[1] * inverse * (vp->Height / 2.0f) + vp->Height / 2.0f + vp->Y;
          position[2] = c[2] * inverse * (vp->MaxZ - vp->MinZ) + vp->MinZ;
          position[3] = inverse;
        } else {
          position[0] = (c[0] + c[3]) / 2.0f;
          position[1] = (c[1] + c[3]) / 2.0f;
          position[2] = c[2];
          position[3] = c[3];
        }
      }
      pv_put_floats(to + o.position, position, o.rhw ? 4 : 3);
      if (flags & D3DPV_DONOTCOPYDATA) continue;
      if (o.diffuse >= 0) pv_put_colour(to + o.diffuse, o.colour_type[0], f + 4);
      if (o.specular >= 0) {
        float specular[4] = {f[9], f[10], f[11], f[8]};
        if (fog) {      /* (the vertex fog factor, by the depth in view space, in its alpha) */
          const float p[4] = {f[0], f[1], f[2], 1.0f};
          float view[4];
          blend_point(&world_view, p, view);
          float depth = view[2] < 0.0f ? -view[2] : view[2];
          if (s->rs[D3DRS_RANGEFOGENABLE]) depth = ffl_sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
          const DWORD mode = s->rs[D3DRS_FOGVERTEXMODE];
          const float log2_e = 1.44269504f;
          float factor = 1.0f;
          if (mode == D3DFOG_LINEAR) factor = end != start ? (end - depth) / (end - start) : 1.0f;
          else if (mode == D3DFOG_EXP) factor = ffl_pow(2.0f, -density * depth * log2_e);
          else if (mode == D3DFOG_EXP2) factor = ffl_pow(2.0f, -density * density * depth * depth * log2_e);
          specular[3] = ffl_clamp(factor);
        }
        pv_put_colour(to + o.specular, o.colour_type[1], specular);
      }
      if (o.point_size >= 0) {
        float size = point_size;
        if (layout.point_size.type != 255) {
          float value[4] = {0, 0, 0, 1};
          decode_element(layout.point_size.type, source_at(&sources[layout.point_size.stream], src_start + v, layout.point_size.offset), value);
          size = value[0];
        }
        pv_put_floats(to + o.point_size, &size, 1);
      }
      if (o.normal >= 0) {
        float normal[4] = {0, 0, 0, 0};
        if (layout.normal.type != 255) decode_element(layout.normal.type, source_at(&sources[layout.normal.stream], src_start + v, layout.normal.offset), normal);
        pv_put_floats(to + o.normal, normal, 3);
      }
      for (UINT t = 0; t < TEXTURE_SETS; ++t) {
        if (o.texture[t] < 0) continue;
        float value[4] = {0, 0, 0, 1};
        const Attribute *a = &layout.texture[t];
        if (a->type != 255) decode_element(a->type, source_at(&sources[a->stream], src_start + v, a->offset), value);
        pv_put_floats(to + o.texture[t], value, (UINT)o.texture_type[t] + 1u);      /* (FLOAT1 .. FLOAT4 are 0 .. 3) */
      }
    }
    if (clipping) {
      d->clip_status.ClipUnion = clip_union;
      d->clip_status.ClipIntersection = clip_intersection;
    }
  }
  if (blend) {
    s->world[0] = world;
    mem_free(blend);
  }
  if (decoded) mem_free(decoded);
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_BeginScene(Dev *d) {
  if (d->in_scene) return D3DERR_INVALIDCALL;
  d->in_scene = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_EndScene(Dev *d) {
  if (!d->in_scene) return D3DERR_INVALIDCALL;
  d->in_scene = 0;
  return D3D_OK;
}

/* Higher-order patches: the caps have none (no D3DDEVCAPS_RTPATCHES, D3DDEVCAPS_NPATCHES, D3DDEVCAPS_QUINTICRTPATCHES), and Direct3D 9 refuses their draws on a
 * device without them. */
static HRESULT STDMETHODCALLTYPE dev_DrawRectPatch(Dev *d, UINT handle, const float *segments, const D3DRECTPATCH_INFO *info) {
  (void)d; (void)handle; (void)segments; (void)info;
  return D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE dev_DrawTriPatch(Dev *d, UINT handle, const float *segments, const D3DTRIPATCH_INFO *info) {
  (void)d; (void)handle; (void)segments; (void)info;
  return D3DERR_INVALIDCALL;
}
static HRESULT STDMETHODCALLTYPE dev_DeletePatch(Dev *d, UINT handle) {
  (void)d; (void)handle;
  return D3DERR_INVALIDCALL;
}

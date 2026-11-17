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

typedef struct Source { const BYTE *data; UINT bytes, stride; } Source;

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
    case D3DDECLTYPE_SHORT2N: memcpy(s, p, 4); out[0] = s[0] / 32767.0f; out[1] = s[1] / 32767.0f; break;
    case D3DDECLTYPE_SHORT4N: memcpy(s, p, 8); for (int i = 0; i < 4; ++i) out[i] = s[i] / 32767.0f; break;
    case D3DDECLTYPE_USHORT2N: memcpy(w, p, 4); out[0] = w[0] / 65535.0f; out[1] = w[1] / 65535.0f; break;
    case D3DDECLTYPE_USHORT4N: memcpy(w, p, 8); for (int i = 0; i < 4; ++i) out[i] = w[i] / 65535.0f; break;
    case D3DDECLTYPE_FLOAT16_2: memcpy(w, p, 4); out[0] = half_to_float(w[0]); out[1] = half_to_float(w[1]); break;
    case D3DDECLTYPE_FLOAT16_4: memcpy(w, p, 8); for (int i = 0; i < 4; ++i) out[i] = half_to_float(w[i]); break;
    default: break;
  }
}
/* Whether the vertices `first .. first + count - 1` of an attribute are inside its stream. */
static BOOL attribute_inside(const Attribute *a, const Source *sources, UINT first, UINT count) {
  if (a->type == 255) return TRUE;
  const Source *s = &sources[a->stream];
  const UINT size = type_bytes(a->type);
  if (!s->data || !size || count == 0) return FALSE;
  const UINT64 last = (UINT64)(first + count - 1) * s->stride + a->offset + size;
  return first <= 0xFFFFFFFFu - count && last <= s->bytes;
}
/* Decodes `count` vertices from `first` into the shaders' layout. */
static void decode_vertices(const Layout *l, const Source *sources, UINT first, UINT count, BYTE *out) {
  for (UINT v = 0; v < count; ++v, out += VERTEX_BYTES) {
    float *f = (float *)out, value[4];
    decode_element(l->position.type, sources[l->position.stream].data + (SIZE_T)(first + v) * sources[l->position.stream].stride + l->position.offset, f);
    f[4] = f[5] = f[6] = f[7] = 1.0f;
    f[8] = f[9] = f[10] = f[11] = 0.0f;
    if (l->colour.type != 255) decode_element(l->colour.type, sources[l->colour.stream].data + (SIZE_T)(first + v) * sources[l->colour.stream].stride + l->colour.offset, f + 4);
    for (UINT t = 0; t < 2; ++t) {
      if (l->texture[t].type == 255) continue;
      decode_element(l->texture[t].type, sources[l->texture[t].stream].data + (SIZE_T)(first + v) * sources[l->texture[t].stream].stride + l->texture[t].offset, value);
      f[8 + 2 * t] = value[0];
      f[9 + 2 * t] = value[1];
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
      if (a[i].type != 255) decode_element(a[i].type, sources[a[i].stream].data + (SIZE_T)(first + v) * sources[a[i].stream].stride + a[i].offset, value);
      memcpy(out, value, 16);
    }
  }
}

/* ---- pipelines -------------------------------------------------------------------------------------------------------------------------------------- */

static D3D12_BLEND blend_of(DWORD value) { return value >= D3DBLEND_ZERO && value <= D3DBLEND_SRCALPHASAT ? (D3D12_BLEND)value : D3D12_BLEND_ONE; }      /* (the same numbers) */
static UINT op_of(DWORD value, UINT lowest, UINT highest, UINT otherwise) { return value >= lowest && value <= highest ? value : otherwise; }

/* The textures a draw's tables hold: the two stages of the fixed-function pixel shader, or the samplers a program's reads, as 2, 4, 8 or 16. */
static UINT texture_slots(const Shader *ps) {
  const UINT count = ps ? ps->sampler_count : STAGES;
  return count <= 2 ? 2 : count <= 4 ? 4 : count <= 8 ? 8 : 16;
}
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

/* The Direct3D 12 pipeline state of the shaders (NULL: the fixed-function one) and the render states that are set; NULL when it cannot be made. */
static ID3D12PipelineState *pipeline_get(Dev *d, const Shader *vs, const Shader *ps) {
  const DWORD *rs = d->state.rs;
  Pipeline key;
  memset(&key, 0, sizeof key);
  key.vs = vs ? vs->c.id : 0;
  key.ps = ps ? ps->c.id : 0;
  key.cull = rs[D3DRS_CULLMODE];
  key.z = rs[D3DRS_ZENABLE] != D3DZB_FALSE && d->depth != NULL;
  key.zwrite = rs[D3DRS_ZWRITEENABLE] != 0;
  key.zfunc = op_of(rs[D3DRS_ZFUNC], 1, 8, D3DCMP_LESSEQUAL);
  key.blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
  key.src = blend_of(rs[D3DRS_SRCBLEND]);
  key.dst = blend_of(rs[D3DRS_DESTBLEND]);
  key.op = op_of(rs[D3DRS_BLENDOP], 1, 5, D3DBLENDOP_ADD);
  key.separate = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
  key.src_alpha = key.separate ? blend_of(rs[D3DRS_SRCBLENDALPHA]) : key.src;
  key.dst_alpha = key.separate ? blend_of(rs[D3DRS_DESTBLENDALPHA]) : key.dst;
  key.op_alpha = key.separate ? op_of(rs[D3DRS_BLENDOPALPHA], 1, 5, D3DBLENDOP_ADD) : key.op;
  key.write = rs[D3DRS_COLORWRITEENABLE] & 0xF;
  key.stencil = rs[D3DRS_STENCILENABLE] != 0 && d->depth != NULL;
  key.sfail = op_of(rs[D3DRS_STENCILFAIL], 1, 8, D3DSTENCILOP_KEEP);
  key.szfail = op_of(rs[D3DRS_STENCILZFAIL], 1, 8, D3DSTENCILOP_KEEP);
  key.spass = op_of(rs[D3DRS_STENCILPASS], 1, 8, D3DSTENCILOP_KEEP);
  key.sfunc = op_of(rs[D3DRS_STENCILFUNC], 1, 8, D3DCMP_ALWAYS);
  key.smask = rs[D3DRS_STENCILMASK] & 0xFF;
  key.swrite = rs[D3DRS_STENCILWRITEMASK] & 0xFF;
  Pipeline *oldest = &d->pipelines[0];
  for (UINT i = 0; i < d->pipeline_count; ++i) {
    Pipeline *p = &d->pipelines[i];
    if (memcmp(p, &key, offsetof(Pipeline, state)) == 0) {
      p->last_used = ++d->pipeline_clock;
      return p->state;
    }
    if (p->last_used < oldest->last_used) oldest = p;
  }

  static const D3D12_INPUT_ELEMENT_DESC layout[4] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                     {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                     {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                                     {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
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
  pd.PS.pShaderBytecode = ps ? ps->dxbc : g_ff_ps;
  pd.PS.BytecodeLength = ps ? ps->dxbc_bytes : sizeof g_ff_ps;
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
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
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
  pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
  pd.InputLayout.pInputElementDescs = vs ? inputs : layout;
  pd.InputLayout.NumElements = vs ? vs->input_count : 4;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (d->depth) pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  pd.SampleDesc.Count = 1;
  ID3D12PipelineState *state = NULL;
  TcosD3D12PipelineOptions(d->d12, vs ? vs->registers : VERTEX_REGISTERS, 0);      /* (the pixel shader's constants come after the vertex shader's) */
  if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d12, &pd, &IID_ID3D12PipelineState, (void **)&state)) || !state) return NULL;
  Pipeline *slot = d->pipeline_count < PIPELINES ? &d->pipelines[d->pipeline_count++] : oldest;
  if (slot == oldest && slot->state) {
    flush(d);      /* (a recorded draw may use the one that goes) */
    ID3D12PipelineState_Release(slot->state);
  }
  *slot = key;
  slot->state = state;
  slot->last_used = ++d->pipeline_clock;
  return state;
}

/* ---- the constants of a draw ------------------------------------------------------------------------------------------------------------------------ */

/* What a texture stage does, as the pixel shader's three weights (ff_ps.hlsl): x texture * current, y texture, z current. */
static void stage_weights(DWORD op, DWORD arg1, DWORD arg2, float out[4]) {
  const BOOL texture1 = (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE, texture2 = (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
  out[0] = out[1] = out[3] = 0.0f;
  out[2] = 1.0f;      /* (the stage changes nothing: switched off, or an operation this runtime has not) */
  switch (op) {
    case D3DTOP_SELECTARG1: if (texture1) { out[1] = 1.0f; out[2] = 0.0f; } break;
    case D3DTOP_SELECTARG2: if (texture2) { out[1] = 1.0f; out[2] = 0.0f; } break;
    case D3DTOP_MODULATE: case D3DTOP_MODULATE2X: case D3DTOP_MODULATE4X:
      if (texture1 != texture2) { out[0] = op == D3DTOP_MODULATE ? 1.0f : op == D3DTOP_MODULATE2X ? 2.0f : 4.0f; out[2] = 0.0f; }
      else if (texture1) { out[1] = 1.0f; out[2] = 0.0f; }
      break;
    case D3DTOP_ADD: if (texture1 != texture2) out[1] = 1.0f; break;
    default: break;
  }
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
/* The state registers of a program's pixel shader (sm_to_dxbc.h), out of the render states: the alpha test, and fog for the shaders below Shader
 * Model 3 - by the depth of the pixel when a table mode is set, else by the factor a program's vertex shader gives. (The fixed-function vertex
 * stage works out no fog factor.) */
static void shader_state(float *out, const State *s, const Shader *ps, BOOL vertex_shader) {
  float state[SM_STATE_REGISTERS][4];
  sm_state_none(state);
  if (s->rs[D3DRS_ALPHATESTENABLE]) {
    /* (D3DCMP_NEVER .. D3DCMP_ALWAYS are 1 .. 8: bit 0 of the number less one keeps what is below, bit 1 what is equal, bit 2 what is above) */
    const DWORD function = s->rs[D3DRS_ALPHAFUNC] >= D3DCMP_NEVER && s->rs[D3DRS_ALPHAFUNC] <= D3DCMP_ALWAYS ? s->rs[D3DRS_ALPHAFUNC] - 1 : 7;
    state[SM_STATE_ALPHA][0] = (float)(s->rs[D3DRS_ALPHAREF] & 0xFF);
    for (UINT i = 0; i < 3; ++i) state[SM_STATE_ALPHA_KEEP][i] = (function >> i & 1) ? 1.0f : 0.0f;
  }
  if (s->rs[D3DRS_FOGENABLE] && ps->major < 3) {
    const DWORD colour = s->rs[D3DRS_FOGCOLOR], table = s->rs[D3DRS_FOGTABLEMODE];
    float start, end, density;
    memcpy(&start, &s->rs[D3DRS_FOGSTART], 4);
    memcpy(&end, &s->rs[D3DRS_FOGEND], 4);
    memcpy(&density, &s->rs[D3DRS_FOGDENSITY], 4);
    state[SM_STATE_FOG_COLOUR][0] = ((colour >> 16) & 255) / 255.0f;
    state[SM_STATE_FOG_COLOUR][1] = ((colour >> 8) & 255) / 255.0f;
    state[SM_STATE_FOG_COLOUR][2] = (colour & 255) / 255.0f;
    if (table == D3DFOG_LINEAR) {      /* (end - depth) / (end - start) */
      const float range = end - start;
      state[SM_STATE_FOG][0] = range != 0.0f ? -1.0f / range : 0.0f;
      state[SM_STATE_FOG][1] = range != 0.0f ? end / range : 1.0f;
      state[SM_STATE_FOG_SOURCE][1] = 1.0f;
      state[SM_STATE_FOG_SOURCE][3] = 0.0f;
    } else if (table == D3DFOG_EXP || table == D3DFOG_EXP2) {      /* e ^ -(density * depth), e ^ -(density * depth) ^ 2: as powers of two */
      const float log2_e = 1.44269504f;
      state[SM_STATE_FOG][table == D3DFOG_EXP ? 2 : 3] = (table == D3DFOG_EXP ? density : density * density) * log2_e;
      state[SM_STATE_FOG_SOURCE][2] = 1.0f;
      state[SM_STATE_FOG_SOURCE][3] = 0.0f;
    } else if (vertex_shader) {
      state[SM_STATE_FOG_SOURCE][0] = 1.0f;
      state[SM_STATE_FOG_SOURCE][3] = 0.0f;
    }
  }
  memcpy(out, state, sizeof state);
}
/* The registers a draw's block of constants has: the vertex shader's, then the pixel shader's. */
static UINT constant_registers(const Shader *vs, const Shader *ps) { return (vs ? vs->registers : VERTEX_REGISTERS) + (ps ? ps->registers : PIXEL_REGISTERS); }
/* Writes the block of constants of the draw and gives its address. (The ring has room for a whole block: draw() saw to it.) */
static UINT64 constants_of(Dev *d, const Layout *l, const Shader *vs, const Shader *ps) {
  const State *s = &d->state;
  const UINT vertex_registers = vs ? vs->registers : VERTEX_REGISTERS;
  const UINT bytes = (constant_registers(vs, ps) * 16 + 255) & ~255u;
  float *block = (float *)(d->constant_data + d->constant_used);      /* (written where it will stand, and left there unless it is the last one again) */
  memset(block, 0, bytes);
  if (vs) shader_constants(block, vs, s->vs_float, VS_FLOATS, s->vs_bool);
  else {
    const D3DMATRIX world_view = matrix_multiply(&s->world, &s->view), all = matrix_multiply(&world_view, &s->projection);
    for (int row = 0; row < 4; ++row)
      for (int column = 0; column < 4; ++column) block[column * 4 + row] = all.m[row][column];      /* (kept column by column) */
    block[16] = 2.0f / (float)s->viewport.Width;
    block[17] = 2.0f / (float)s->viewport.Height;
    block[18] = l->transformed ? 1.0f : 0.0f;
    block[20] = (float)s->viewport.X;
    block[21] = (float)s->viewport.Y;
  }
  float *pixel = block + vertex_registers * 4;
  if (ps) {
    shader_constants(pixel, ps, s->ps_float, PS_FLOATS, s->ps_bool);
    shader_state(pixel + ps->state * 4, s, ps, vs != NULL);
  } else {
    BOOL on = TRUE;      /* (a stage that is switched off switches off the ones after it) */
    for (UINT stage = 0; stage < STAGES; ++stage) {
      const DWORD *t = s->stage[stage];
      on = on && t[D3DTSS_COLOROP] != D3DTOP_DISABLE;
      /* (a stage with no texture set passes on what it is given: untextured triangles keep their colour and their alpha, whatever the stage says) */
      const BOOL textured = on && s->tex[stage] && s->tex[stage]->res && !s->tex[stage]->depth;
      stage_weights(textured ? t[D3DTSS_COLOROP] : D3DTOP_DISABLE, t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2], pixel + 8 * stage);
      stage_weights(textured ? t[D3DTSS_ALPHAOP] : D3DTOP_DISABLE, t[D3DTSS_ALPHAARG1], t[D3DTSS_ALPHAARG2], pixel + 8 * stage + 4);
    }
    /* The alpha test, as the least alpha that is kept (the tests that keep the higher alphas; the others keep everything). */
    float least = -1.0f;
    if (s->rs[D3DRS_ALPHATESTENABLE]) {
      const float reference = (float)(s->rs[D3DRS_ALPHAREF] & 0xFF);
      if (s->rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATEREQUAL) least = reference / 255.0f;
      else if (s->rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATER || s->rs[D3DRS_ALPHAFUNC] == D3DCMP_NOTEQUAL) least = (reference + 0.5f) / 255.0f;
    }
    pixel[16] = least;
  }
  if (!d->constant_valid || d->constant_last_bytes != bytes || memcmp(d->constant_data + d->constant_last, block, bytes) != 0) {
    d->constant_last = d->constant_used;
    d->constant_last_bytes = bytes;
    d->constant_used += bytes;
    d->constant_valid = 1;
  }
  return ID3D12Resource_GetGPUVirtualAddress(d->constant_ring) + d->constant_last;
}

/* The textures and samplers of the first `slots` stages: a table of each, made again when what is set changed. */
static void tables_of(Dev *d, UINT slots) {
  const State *s = &d->state;
  UINT ids[TEXTURE_SLOTS];
  DWORD samplers[TEXTURE_SLOTS][4];
  if (d->table_count != slots) d->tables_valid = 0;
  d->table_count = slots;
  for (UINT i = 0; i < slots; ++i) {
    ids[i] = s->tex[i] && s->tex[i]->res && !s->tex[i]->depth ? s->tex[i]->c.id : 0;
    samplers[i][0] = s->sampler[i][D3DSAMP_ADDRESSU] | s->sampler[i][D3DSAMP_ADDRESSV] << 8;
    samplers[i][1] = s->sampler[i][D3DSAMP_MAGFILTER];
    samplers[i][2] = s->sampler[i][D3DSAMP_MINFILTER];
    samplers[i][3] = s->sampler[i][D3DSAMP_MIPFILTER];
  }
  const UINT step_views = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT step_samplers = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  if (!d->tables_valid || memcmp(ids, d->table_ids, slots * sizeof ids[0]) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->view_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->view_heap);
    at.ptr += (SIZE_T)d->view_used * step_views;
    d->table_views = gpu.ptr + (UINT64)d->view_used * step_views;
    for (UINT i = 0; i < slots; ++i) {
      ID3D12Device_CreateShaderResourceView(d->d12, ids[i] ? s->tex[i]->res : d->blank->res, NULL, at);
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
      desc.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)op_of(samplers[i][0] & 0xFF, 1, 5, D3DTADDRESS_WRAP);      /* (the same numbers) */
      desc.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)op_of(samplers[i][0] >> 8, 1, 5, D3DTADDRESS_WRAP);
      desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      desc.MaxAnisotropy = 1;
      desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
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
  if (!elements || !d->target) return D3DERR_INVALIDCALL;
  Layout layout;
  D3DVERTEXELEMENT9 fvf[16];
  const D3DVERTEXELEMENT9 *declared = d->state.decl ? d->state.decl->elements : fvf;
  if (d->state.decl) layout = d->state.decl->layout;
  else layout_of_elements(fvf, fvf_elements(d->state.fvf, fvf), &layout);
  /* The shaders of the draw. Positions that are pixels already pass no vertex shader, as Direct3D 9 has it. */
  const Shader *vs = layout.transformed ? NULL : d->state.vs, *ps = d->state.ps;
  Attribute inputs[SHADER_INPUTS];
  if (!indices) vertices = elements;
  if (vs) {
    shader_attributes(vs, declared, inputs);
    for (UINT i = 0; i < vs->input_count; ++i)
      if (!attribute_inside(&inputs[i], sources, first_vertex, vertices)) return D3DERR_INVALIDCALL;
  } else {
    if (layout.position.type == 255) return D3DERR_INVALIDCALL;
    if (!attribute_inside(&layout.position, sources, first_vertex, vertices) || !attribute_inside(&layout.colour, sources, first_vertex, vertices) ||
        !attribute_inside(&layout.texture[0], sources, first_vertex, vertices) || !attribute_inside(&layout.texture[1], sources, first_vertex, vertices))
      return D3DERR_INVALIDCALL;
  }
  const UINT stride = vs ? 16 * (vs->input_count ? vs->input_count : 1) : VERTEX_BYTES;
  const UINT slots = texture_slots(ps);
  const BOOL fan = type == D3DPT_TRIANGLEFAN;
  const UINT out_indices = fan ? primitives * 3 : indices ? elements : 0;
  if (vertices > VERTEX_RING / stride || out_indices > INDEX_RING / 4 || constant_registers(vs, ps) > CONSTANT_REGISTERS) return D3DERR_INVALIDCALL;

  /* what may have to send the list first */
  settle(d, d->target->tex->sampled);      /* (a texture the list's draws sampled cannot become its target) */
  if (d->vertex_used + vertices * stride > VERTEX_RING || d->index_used + out_indices * 4 > INDEX_RING || d->constant_used + CONSTANT_REGISTERS * 16 > CONSTANT_RING ||
      d->view_used + slots > VIEW_HEAP || d->sampler_used + slots > VIEW_HEAP)
    flush(d);
  ID3D12PipelineState *pipeline = pipeline_get(d, vs, ps);
  if (!pipeline) return D3DERR_INVALIDCALL;

  /* the vertices, and the indices counted from the first of them */
  const UINT vertex_at = d->vertex_used, index_at = d->index_used;
  if (vs) decode_shader_vertices(inputs, vs->input_count, sources, first_vertex, vertices, d->vertex_data + vertex_at);
  else decode_vertices(&layout, sources, first_vertex, vertices, d->vertex_data + vertex_at);
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

  ID3D12GraphicsCommandList *list = d->list;
  Tex *target = d->target->tex;
  const BOOL depth = d->depth != NULL;
  if (d->targets_dirty) {
    const State *s = &d->state;
    ID3D12DescriptorHeap *heaps[2] = {d->view_heap, d->sampler_heap};
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &target->view, FALSE, depth ? &d->depth->tex->view : NULL);
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
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, type == D3DPT_POINTLIST ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : type == D3DPT_LINELIST ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                         : type == D3DPT_LINESTRIP ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
                                                         : type == D3DPT_TRIANGLESTRIP ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12GraphicsCommandList_OMSetStencilRef(list, d->state.rs[D3DRS_STENCILREF] & 0xFF);
  D3D12_VERTEX_BUFFER_VIEW vertex_view = {ID3D12Resource_GetGPUVirtualAddress(d->vertex_ring) + vertex_at, vertices * stride, stride};
  ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vertex_view);
  ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, constants_of(d, &layout, vs, ps));
  tables_of(d, slots);
  D3D12_GPU_DESCRIPTOR_HANDLE views = {d->table_views}, samplers = {d->table_samplers};
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, views);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, samplers);
  if (out_indices) {
    D3D12_INDEX_BUFFER_VIEW index_view = {ID3D12Resource_GetGPUVirtualAddress(d->index_ring) + index_at, out_indices * 4, DXGI_FORMAT_R32_UINT};
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &index_view);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, out_indices, 1, 0, 0, 0);
  } else {
    ID3D12GraphicsCommandList_DrawInstanced(list, vertices, 1, 0, 0);
  }

  for (UINT i = 0; i < slots; ++i)
    if (d->state.tex[i]) d->state.tex[i]->used = d->state.tex[i]->sampled = d->epoch;
  target->used = d->epoch;
  if (depth) {
    d->depth->tex->used = d->epoch;
    for (UINT i = 0; i < d->clear_count;) {      /* (its clear is done by the pass this draw begins) */
      if (d->clears[i].depth == d->depth->tex) d->clears[i] = d->clears[--d->clear_count]; else ++i;
    }
  }
  d->recorded = 1;
  return D3D_OK;
}

static void stream_sources(Dev *d, Source *sources) {
  for (UINT i = 0; i < STREAMS; ++i) {
    const Stream *s = &d->state.stream[i];
    sources[i].data = s->buffer && s->offset < s->buffer->length ? s->buffer->data + s->offset : NULL;
    sources[i].bytes = sources[i].data ? s->buffer->length - s->offset : 0;
    sources[i].stride = s->stride;
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
  if (!ib || !elements || vertices == 0) return D3DERR_INVALIDCALL;
  const UINT index_bytes = ib->format == D3DFMT_INDEX32 ? 4 : 2;
  if (start > ib->length / index_bytes || elements > ib->length / index_bytes - start) return D3DERR_INVALIDCALL;
  if ((INT64)base + lowest < 0) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  stream_sources(d, sources);
  const HRESULT hr = draw(d, type, primitives, sources, (UINT)(base + (INT)lowest), vertices, ib->data + (SIZE_T)start * index_bytes, index_bytes, base);
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
  bind(&d->state.indices, NULL);
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_Clear(Dev *d, DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
  (void)rect_count; (void)rects;      /* (the whole of the target is cleared: Direct3D 12 here has no clear of a part) */
  EnterCriticalSection(&d->lock);
  if ((flags & D3DCLEAR_TARGET) && d->target) {
    const float c[4] = {((colour >> 16) & 255) / 255.0f, ((colour >> 8) & 255) / 255.0f, (colour & 255) / 255.0f, (colour >> 24) / 255.0f};
    ID3D12GraphicsCommandList_ClearRenderTargetView(d->list, d->target->tex->view, c, 0, NULL);
    d->target->tex->used = d->epoch;
    d->recorded = 1;
  }
  if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && d->depth) {
    /* Direct3D 12 here clears a depth buffer when the next pass with it begins, and forgets the clear with the list: it is kept until a draw has
     * followed it, and recorded again in every list until then. */
    Tex *t = d->depth->tex;
    const UINT what = ((flags & D3DCLEAR_ZBUFFER) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & D3DCLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0);
    UINT at = d->clear_count;
    for (UINT i = 0; i < d->clear_count; ++i) if (d->clears[i].depth == t) at = i;
    if (at == d->clear_count && d->clear_count < 4) ++d->clear_count;
    if (at < 4) {
      PendingClear *p = &d->clears[at];
      p->flags = (p->depth == t ? p->flags : 0) | what;
      p->depth = t;
      if (flags & D3DCLEAR_ZBUFFER) p->z = z;
      if (flags & D3DCLEAR_STENCIL) p->stencil = (UINT8)stencil;
    }
    ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, t->view, (D3D12_CLEAR_FLAGS)what, z, (UINT8)stencil, 0, NULL);
    t->used = d->epoch;
    d->recorded = 1;
  }
  LeaveCriticalSection(&d->lock);
  return D3D_OK;
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

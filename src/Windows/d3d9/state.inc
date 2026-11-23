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

/* Part of d3d9.c: the state a program sets on the device - render, sampler and texture stage states, the transforms, the viewport, what is bound -
 * and state blocks. Setting only remembers; a draw reads it (draw.inc). */

static const D3DMATRIX g_identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};

/* The state of a new device (and of one that was reset). `depth`: it has a depth buffer of its own. */
static void state_defaults(State *s, BOOL depth, UINT width, UINT height) {
  memset(s->rs, 0, sizeof s->rs);
  s->rs[D3DRS_ZENABLE] = depth ? D3DZB_TRUE : D3DZB_FALSE;
  s->rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  s->rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  s->rs[D3DRS_ZWRITEENABLE] = TRUE;
  s->rs[D3DRS_LASTPIXEL] = TRUE;
  s->rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  s->rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  s->rs[D3DRS_CULLMODE] = D3DCULL_CCW;
  s->rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  s->rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  {
    const float one = 1.0f;
    memcpy(&s->rs[D3DRS_FOGEND], &one, 4);
    memcpy(&s->rs[D3DRS_FOGDENSITY], &one, 4);
  }
  s->rs[D3DRS_STENCILFAIL] = s->rs[D3DRS_STENCILZFAIL] = s->rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  s->rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  s->rs[D3DRS_STENCILMASK] = s->rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
  s->rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
  s->rs[D3DRS_LIGHTING] = TRUE;
  s->rs[D3DRS_COLORVERTEX] = TRUE;
  s->rs[D3DRS_LOCALVIEWER] = TRUE;
  s->rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  s->rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  s->rs[D3DRS_AMBIENTMATERIALSOURCE] = s->rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  s->rs[D3DRS_CLIPPING] = TRUE;
  s->rs[D3DRS_COLORWRITEENABLE] = 0xF;
  s->rs[D3DRS_BLENDOP] = s->rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  s->rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  s->rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  memset(s->sampler, 0, sizeof s->sampler);
  memset(s->stage, 0, sizeof s->stage);
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) {
    s->sampler[i][D3DSAMP_ADDRESSU] = s->sampler[i][D3DSAMP_ADDRESSV] = s->sampler[i][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    s->sampler[i][D3DSAMP_MAGFILTER] = s->sampler[i][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    s->sampler[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    s->sampler[i][D3DSAMP_MAXANISOTROPY] = 1;
    s->stage[i][D3DTSS_COLOROP] = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    s->stage[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    s->stage[i][D3DTSS_COLORARG1] = s->stage[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    s->stage[i][D3DTSS_COLORARG2] = s->stage[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    s->stage[i][D3DTSS_COLORARG0] = s->stage[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    s->stage[i][D3DTSS_TEXCOORDINDEX] = i;
    s->stage[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    s->texture[i] = g_identity;
  }
  s->world = s->view = s->projection = g_identity;
  const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
  s->viewport = viewport;
  SetRect(&s->scissor, 0, 0, (int)width, (int)height);
  memset(&s->material, 0, sizeof s->material);
  memset(s->light, 0, sizeof s->light);
  s->light_set = s->light_on = 0;
  s->fvf = 0;
  memset(s->vs_float, 0, sizeof s->vs_float);
  memset(s->ps_float, 0, sizeof s->ps_float);
  memset(s->vs_int, 0, sizeof s->vs_int);
  memset(s->ps_int, 0, sizeof s->ps_int);
  s->vs_bool = s->ps_bool = 0;
}
/* Gives back the references a state holds. */
static void state_release(State *s) {
  bind(&s->decl, NULL);
  bind(&s->vs, NULL);
  bind(&s->ps, NULL);
  bind(&s->indices, NULL);
  for (UINT i = 0; i < STREAMS; ++i) bind(&s->stream[i].buffer, NULL);
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) bind(&s->tex[i], NULL);
}
/* `*to` becomes `*from`, with the references of what both hold moved. */
static void state_copy(State *to, const State *from) {
  bind(&to->decl, from->decl);
  bind(&to->vs, from->vs);
  bind(&to->ps, from->ps);
  bind(&to->indices, from->indices);
  for (UINT i = 0; i < STREAMS; ++i) bind(&to->stream[i].buffer, from->stream[i].buffer);
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) bind(&to->tex[i], from->tex[i]);
  *to = *from;      /* (the pointers are the same now; the rest is plain) */
}

/* ---- render, sampler and stage states -------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_SetRenderState(Dev *d, D3DRENDERSTATETYPE state, DWORD value) {
  if ((UINT)state >= RENDER_STATES) return D3D_OK;      /* (a state this runtime has no use for) */
  d->state.rs[state] = value;
  if (state == D3DRS_SCISSORTESTENABLE) d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderState(Dev *d, D3DRENDERSTATETYPE state, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  *value = (UINT)state < RENDER_STATES ? d->state.rs[state] : 0;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetSamplerState(Dev *d, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler >= TEXTURE_SLOTS || (UINT)type >= SAMPLER_STATES) return D3D_OK;
  d->state.sampler[sampler][type] = value;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetSamplerState(Dev *d, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  *value = sampler < TEXTURE_SLOTS && (UINT)type < SAMPLER_STATES ? d->state.sampler[sampler][type] : 0;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetTextureStageState(Dev *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage >= TEXTURE_SLOTS || (UINT)type >= STAGE_STATES) return D3D_OK;
  d->state.stage[stage][type] = value;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTextureStageState(Dev *d, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (!value) return D3DERR_INVALIDCALL;
  *value = stage < TEXTURE_SLOTS && (UINT)type < STAGE_STATES ? d->state.stage[stage][type] : 0;
  return D3D_OK;
}

/* ---- transforms, the viewport, the material -------------------------------------------------------------------------------------------------------- */

static D3DMATRIX *transform_of(Dev *d, D3DTRANSFORMSTATETYPE which) {
  if (which == D3DTS_VIEW) return &d->state.view;
  if (which == D3DTS_PROJECTION) return &d->state.projection;
  if (which == D3DTS_WORLD) return &d->state.world;
  if (which >= D3DTS_TEXTURE0 && which <= D3DTS_TEXTURE7) return &d->state.texture[which - D3DTS_TEXTURE0];
  return NULL;
}
/* a * b, as Direct3D multiplies (a row vector goes through a first). */
static D3DMATRIX matrix_multiply(const D3DMATRIX *a, const D3DMATRIX *b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a->m[i][k] * b->m[k][j];
      r.m[i][j] = sum;
    }
  return r;
}
static HRESULT STDMETHODCALLTYPE dev_SetTransform(Dev *d, D3DTRANSFORMSTATETYPE which, const D3DMATRIX *matrix) {
  D3DMATRIX *m = transform_of(d, which);
  if (!matrix) return D3DERR_INVALIDCALL;
  if (m) *m = *matrix;      /* (the other world matrices, for blending: not used) */
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTransform(Dev *d, D3DTRANSFORMSTATETYPE which, D3DMATRIX *matrix) {
  const D3DMATRIX *m = transform_of(d, which);
  if (!matrix) return D3DERR_INVALIDCALL;
  *matrix = m ? *m : g_identity;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_MultiplyTransform(Dev *d, D3DTRANSFORMSTATETYPE which, const D3DMATRIX *matrix) {
  D3DMATRIX *m = transform_of(d, which);
  if (!matrix) return D3DERR_INVALIDCALL;
  if (m) *m = matrix_multiply(matrix, m);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetViewport(Dev *d, const D3DVIEWPORT9 *viewport) {
  if (!viewport || viewport->Width == 0 || viewport->Height == 0) return D3DERR_INVALIDCALL;
  d->state.viewport = *viewport;
  d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetViewport(Dev *d, D3DVIEWPORT9 *viewport) {
  if (!viewport) return D3DERR_INVALIDCALL;
  *viewport = d->state.viewport;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetScissorRect(Dev *d, const RECT *rect) {
  if (!rect) return D3DERR_INVALIDCALL;
  d->state.scissor = *rect;
  d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetScissorRect(Dev *d, RECT *rect) {
  if (!rect) return D3DERR_INVALIDCALL;
  *rect = d->state.scissor;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetMaterial(Dev *d, const D3DMATERIAL9 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  d->state.material = *material;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetMaterial(Dev *d, D3DMATERIAL9 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  *material = d->state.material;
  return D3D_OK;
}
/* The lights, by their numbers (LIGHTS of them); draw.inc lights the vertices of the fixed-function pipeline with the ones that shine. */
static HRESULT STDMETHODCALLTYPE dev_SetLight(Dev *d, DWORD index, const D3DLIGHT9 *light) {
  if (!light || index >= LIGHTS || light->Type < D3DLIGHT_POINT || light->Type > D3DLIGHT_DIRECTIONAL) return D3DERR_INVALIDCALL;
  d->state.light[index] = *light;
  d->state.light_set |= 1u << index;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetLight(Dev *d, DWORD index, D3DLIGHT9 *light) {
  if (!light || index >= LIGHTS || !(d->state.light_set >> index & 1)) return D3DERR_INVALIDCALL;
  *light = d->state.light[index];
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_LightEnable(Dev *d, DWORD index, BOOL enable) {
  if (index >= LIGHTS) return D3DERR_INVALIDCALL;
  if (!(d->state.light_set >> index & 1)) {      /* (a light nobody set is a white one that shines along z) */
    D3DLIGHT9 *light = &d->state.light[index];
    memset(light, 0, sizeof *light);
    light->Type = D3DLIGHT_DIRECTIONAL;
    light->Diffuse.r = light->Diffuse.g = light->Diffuse.b = 1.0f;
    light->Direction.z = 1.0f;
    d->state.light_set |= 1u << index;
  }
  if (enable) d->state.light_on |= 1u << index;
  else d->state.light_on &= ~(1u << index);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetLightEnable(Dev *d, DWORD index, BOOL *enable) {
  if (!enable || index >= LIGHTS || !(d->state.light_set >> index & 1)) return D3DERR_INVALIDCALL;
  *enable = (d->state.light_on >> index & 1) ? TRUE : FALSE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetClipPlane(Dev *d, DWORD index, const float *plane) { (void)d; (void)index; (void)plane; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE dev_SetNPatchMode(Dev *d, float segments) { (void)d; (void)segments; return D3D_OK; }
static float STDMETHODCALLTYPE dev_GetNPatchMode(Dev *d) { (void)d; return 0.0f; }
static HRESULT STDMETHODCALLTYPE dev_SetSoftwareVertexProcessing(Dev *d, BOOL software) { (void)d; (void)software; return D3D_OK; }
static BOOL STDMETHODCALLTYPE dev_GetSoftwareVertexProcessing(Dev *d) { (void)d; return FALSE; }

/* ---- what is bound ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_SetFVF(Dev *d, DWORD fvf) {
  d->state.fvf = fvf;
  if (fvf) bind(&d->state.decl, NULL);      /* (a code takes the place of a declaration) */
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetFVF(Dev *d, DWORD *fvf) {
  if (!fvf) return D3DERR_INVALIDCALL;
  *fvf = d->state.fvf;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetVertexDeclaration(Dev *d, IDirect3DVertexDeclaration9 *decl) {
  bind(&d->state.decl, decl);
  if (decl) d->state.fvf = 0;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexDeclaration(Dev *d, IDirect3DVertexDeclaration9 **decl) {
  if (!decl) return D3DERR_INVALIDCALL;
  *decl = (IDirect3DVertexDeclaration9 *)d->state.decl;
  if (*decl) dx_AddRef(OBJ(*decl));
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetStreamSource(Dev *d, UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) {
  if (stream >= STREAMS) return D3DERR_INVALIDCALL;
  bind(&d->state.stream[stream].buffer, buffer);
  d->state.stream[stream].offset = offset;
  d->state.stream[stream].stride = stride;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetStreamSource(Dev *d, UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) {
  if (stream >= STREAMS || !buffer) return D3DERR_INVALIDCALL;
  *buffer = (IDirect3DVertexBuffer9 *)d->state.stream[stream].buffer;
  if (*buffer) dx_AddRef(OBJ(*buffer));
  if (offset) *offset = d->state.stream[stream].offset;
  if (stride) *stride = d->state.stream[stream].stride;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetStreamSourceFreq(Dev *d, UINT stream, UINT divider) { (void)d; (void)divider; return stream < STREAMS ? D3D_OK : D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE dev_GetStreamSourceFreq(Dev *d, UINT stream, UINT *divider) {
  (void)d;
  if (stream >= STREAMS || !divider) return D3DERR_INVALIDCALL;
  *divider = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetIndices(Dev *d, IDirect3DIndexBuffer9 *buffer) {
  bind(&d->state.indices, buffer);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetIndices(Dev *d, IDirect3DIndexBuffer9 **buffer) {
  if (!buffer) return D3DERR_INVALIDCALL;
  *buffer = (IDirect3DIndexBuffer9 *)d->state.indices;
  if (*buffer) dx_AddRef(OBJ(*buffer));
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetTexture(Dev *d, DWORD stage, IDirect3DBaseTexture9 *texture) {
  if (stage >= TEXTURE_SLOTS) return D3D_OK;      /* (the vertex shader's samplers and the like) */
  if (texture && ((DxObj *)texture)->lpVtbl != (const void *)&tex_vtbl && ((DxObj *)texture)->lpVtbl != (const void *)&cube_vtbl &&
      ((DxObj *)texture)->lpVtbl != (const void *)&vol_vtbl)
    return D3DERR_INVALIDCALL;
  bind(&d->state.tex[stage], texture);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetTexture(Dev *d, DWORD stage, IDirect3DBaseTexture9 **texture) {
  if (!texture) return D3DERR_INVALIDCALL;
  *texture = stage < TEXTURE_SLOTS ? (IDirect3DBaseTexture9 *)d->state.tex[stage] : NULL;
  if (*texture) dx_AddRef(OBJ(*texture));
  return D3D_OK;
}

/* ---- the program's shaders -------------------------------------------------------------------------------------------------------------------------- */

static void shader_destroy(DxObj *o) {
  Shader *s = (Shader *)o;
  mem_free(s->code);
  mem_free(s->dxbc);
  child_destroy(o);
}
static HRESULT STDMETHODCALLTYPE shader_GetFunction(Shader *s, void *data, UINT *size) {
  if (!size) return D3DERR_INVALIDCALL;
  if (data) {
    if (*size < s->code_bytes) return D3DERR_INVALIDCALL;
    memcpy(data, s->code, s->code_bytes);
  }
  *size = s->code_bytes;
  return D3D_OK;
}
/* Makes a shader of the tokens at `code`, which end where the shader says they do. The translation is what a draw's pipeline is made of; a shader
 * the translator does not take (sm_to_dxbc.h says which) cannot be made. */
static HRESULT shader_make(Dev *d, const DWORD *code, BOOL vertex, Shader **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!code) return D3DERR_INVALIDCALL;
  /* (the two are far too big for the stack) */
  SmShader *translated = mem_alloc(sizeof *translated);
  SmContext *context = mem_alloc(sizeof *context);
  Shader *s = NULL;
  HRESULT hr = E_OUTOFMEMORY;
  if (translated && context) {
    const int status = sm_translate((const uint32_t *)code, SM_MAX_TOKENS, translated, context);
    hr = status == SM_OK && (translated->vertex != 0) == (vertex != 0) ? D3D_OK : D3DERR_INVALIDCALL;
    if (status == SM_UNSUPPORTED) OutputDebugStringA(vertex ? "d3d9: a vertex shader uses what this Direct3D 9 has not\n" : "d3d9: a pixel shader uses what this Direct3D 9 has not\n");
  }
  if (SUCCEEDED(hr)) {
    s = child_new(sizeof *s, d, vertex ? (const void *)&vs_vtbl : (const void *)&ps_vtbl, vertex ? g_vs_iids : g_ps_iids, shader_destroy);
    hr = s ? D3D_OK : E_OUTOFMEMORY;
  }
  if (s) {
    s->vertex = vertex;
    s->code_bytes = (UINT)translated->tokens * 4;
    s->dxbc_bytes = (UINT)translated->dxbc_size;
    s->code = mem_alloc(s->code_bytes);
    s->dxbc = mem_alloc(s->dxbc_bytes);
    if (s->code && s->dxbc) {
      memcpy(s->code, code, s->code_bytes);
      memcpy(s->dxbc, translated->dxbc, s->dxbc_bytes);
      s->floats = translated->float_registers;
      s->bools = translated->bools_used;
      s->registers = translated->constant_registers;
      s->major = translated->major;
      s->state = translated->state_register;
      for (UINT i = 0; i < 16; ++i) if (translated->sampler_mask >> i & 1) s->sampler_count = i + 1;
      memcpy(s->sampler_kind, translated->sampler_kind, sizeof s->sampler_kind);
      for (UINT i = 0; i < TEXTURE_SETS; ++i) if (translated->varying_mask >> (SM_SLOT_TEXCOORD + i) & 1) s->coordinates = i + 1;
      s->input_count = translated->input_count;
      memcpy(s->inputs, translated->inputs, sizeof s->inputs);
      *out = s;
    } else {
      dx_Release(OBJ(s));
      hr = E_OUTOFMEMORY;
    }
  }
  mem_free(translated);
  mem_free(context);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexShader(Dev *d, const DWORD *code, IDirect3DVertexShader9 **shader) { return shader_make(d, code, TRUE, (Shader **)shader); }
static HRESULT STDMETHODCALLTYPE dev_CreatePixelShader(Dev *d, const DWORD *code, IDirect3DPixelShader9 **shader) { return shader_make(d, code, FALSE, (Shader **)shader); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexShader(Dev *d, IDirect3DVertexShader9 *shader) {
  if (shader && ((DxObj *)shader)->lpVtbl != (const void *)&vs_vtbl) return D3DERR_INVALIDCALL;
  bind(&d->state.vs, shader);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShader(Dev *d, IDirect3DVertexShader9 **shader) {
  if (!shader) return D3DERR_INVALIDCALL;
  *shader = (IDirect3DVertexShader9 *)d->state.vs;
  if (*shader) dx_AddRef(OBJ(*shader));
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetPixelShader(Dev *d, IDirect3DPixelShader9 *shader) {
  if (shader && ((DxObj *)shader)->lpVtbl != (const void *)&ps_vtbl) return D3DERR_INVALIDCALL;
  bind(&d->state.ps, shader);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShader(Dev *d, IDirect3DPixelShader9 **shader) {
  if (!shader) return D3DERR_INVALIDCALL;
  *shader = (IDirect3DPixelShader9 *)d->state.ps;
  if (*shader) dx_AddRef(OBJ(*shader));
  return D3D_OK;
}

/* The constants: registers of four floats or four integers, and bools, kept for each stage whatever shader is set. (No shader the translator takes
 * reads the integers: they count loops.) */
/* `count` registers of 16 bytes from `first`, between a program's memory and the `limit` registers at `registers`. */
static HRESULT registers_copy(void *registers, UINT limit, UINT first, void *to, const void *from, UINT count) {
  if ((!to && !from) || first > limit || count > limit - first) return D3DERR_INVALIDCALL;
  if (count && from) memcpy((BYTE *)registers + (SIZE_T)first * 16, from, (SIZE_T)count * 16);
  if (count && to) memcpy(to, (const BYTE *)registers + (SIZE_T)first * 16, (SIZE_T)count * 16);
  return D3D_OK;
}
static HRESULT bools_set(DWORD *bools, UINT first, const BOOL *data, UINT count) {
  if (!data || first > 16 || count > 16 - first) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < count; ++i) *bools = (*bools & ~(1u << (first + i))) | (data[i] ? 1u << (first + i) : 0u);
  return D3D_OK;
}
static HRESULT bools_get(DWORD bools, UINT first, BOOL *data, UINT count) {
  if (!data || first > 16 || count > 16 - first) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < count; ++i) data[i] = (bools >> (first + i)) & 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantF(Dev *d, UINT first, const float *data, UINT count) { return registers_copy(d->state.vs_float, VS_FLOATS, first, NULL, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantF(Dev *d, UINT first, float *data, UINT count) { return registers_copy(d->state.vs_float, VS_FLOATS, first, data, NULL, count); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantI(Dev *d, UINT first, const int *data, UINT count) { return registers_copy(d->state.vs_int, 16, first, NULL, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantI(Dev *d, UINT first, int *data, UINT count) { return registers_copy(d->state.vs_int, 16, first, data, NULL, count); }
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstantB(Dev *d, UINT first, const BOOL *data, UINT count) { return bools_set(&d->state.vs_bool, first, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstantB(Dev *d, UINT first, BOOL *data, UINT count) { return bools_get(d->state.vs_bool, first, data, count); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantF(Dev *d, UINT first, const float *data, UINT count) { return registers_copy(d->state.ps_float, PS_FLOATS, first, NULL, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantF(Dev *d, UINT first, float *data, UINT count) { return registers_copy(d->state.ps_float, PS_FLOATS, first, data, NULL, count); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantI(Dev *d, UINT first, const int *data, UINT count) { return registers_copy(d->state.ps_int, 16, first, NULL, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantI(Dev *d, UINT first, int *data, UINT count) { return registers_copy(d->state.ps_int, 16, first, data, NULL, count); }
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstantB(Dev *d, UINT first, const BOOL *data, UINT count) { return bools_set(&d->state.ps_bool, first, data, count); }
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstantB(Dev *d, UINT first, BOOL *data, UINT count) { return bools_get(d->state.ps_bool, first, data, count); }

/* ---- state blocks ----------------------------------------------------------------------------------------------------------------------------------- */

static void sb_destroy(DxObj *o) {
  state_release(&((StateBlock *)o)->state);
  child_destroy(o);
}
/* A block holds all of the device's state, whatever kind was asked for: applying it sets everything back to what it was when it was captured. */
static HRESULT STDMETHODCALLTYPE sb_Capture(StateBlock *b) {
  state_copy(&b->state, &b->c.dev->state);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE sb_Apply(StateBlock *b) {
  Dev *d = b->c.dev;
  state_copy(&d->state, &b->state);
  d->targets_dirty = 1;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateStateBlock(Dev *d, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out) {
  (void)type;
  if (!out) return D3DERR_INVALIDCALL;
  StateBlock *b = child_new(sizeof *b, d, &sb_vtbl, g_sb_iids, sb_destroy);
  *out = (IDirect3DStateBlock9 *)b;
  if (!b) return E_OUTOFMEMORY;
  state_copy(&b->state, &d->state);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_BeginStateBlock(Dev *d) { (void)d; return D3D_OK; }
static HRESULT STDMETHODCALLTYPE dev_EndStateBlock(Dev *d, IDirect3DStateBlock9 **out) { return dev_CreateStateBlock(d, D3DSBT_ALL, out); }

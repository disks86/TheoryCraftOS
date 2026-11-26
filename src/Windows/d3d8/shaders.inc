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

/* d3d8.dll: vertex and pixel shaders by handle. A vertex shader is a Direct3D 9 vertex declaration made of its D3DVSD tokens and (unless it is the
 * fixed-function pipeline with that declaration) a Direct3D 9 vs_1_1 with dcl instructions (d3d8_shader.h); a vertex shader handle at or below
 * VS_HANDLE_BASE is an FVF code. Included by d3d8.c. */

#define TOKEN_LIMIT (SIZE_MAX / 8)      /* (a shader or declaration is read until its end token, as Direct3D 8 reads it) */
#define PS_CONSTANTS 8u

static void vertex_shader_free(VertexShader8 *s) {
  if (!s) return;
  if (s->decl9) CALL9(s->decl9, Release);
  if (s->vs9) CALL9(s->vs9, Release);
  mem_free(s->declaration);
  mem_free(s->function);
  mem_free(s->load_data);
  mem_free(s);
}
static void pixel_shader_free(PixelShader8 *s) {
  if (!s) return;
  if (s->ps9) CALL9(s->ps9, Release);
  mem_free(s->function);
  mem_free(s);
}

/* The sizing protocol of GetVertexShaderDeclaration, GetVertexShaderFunction and GetPixelShaderFunction: no buffer, the size; a buffer too small,
 * D3DERR_INVALIDCALL with the size as it was. */
static HRESULT copy_out(const void *source, UINT bytes, void *data, DWORD *size) {
  if (!size) return D3DERR_INVALIDCALL;
  if (!data) { *size = bytes; return D3D_OK; }
  if (*size < bytes) return D3DERR_INVALIDCALL;
  if (bytes) memcpy(data, source, bytes);
  *size = bytes;
  return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateVertexShader(Device8 *d, const DWORD *declaration, const DWORD *function, DWORD *handle, DWORD usage) {
  if (!declaration || !handle) return D3DERR_INVALIDCALL;
  if (usage & ~(DWORD)D3DUSAGE_SOFTWAREPROCESSING) return D3DERR_INVALIDCALL;
  D8Declaration *decl = mem_alloc(sizeof *decl);
  VertexShader8 *s = mem_alloc(sizeof *s);
  uint32_t *tokens = NULL;
  HRESULT hr = decl && s ? D3D_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr) && d8_declaration_parse((const uint32_t *)declaration, TOKEN_LIMIT, decl)) hr = D3DERR_INVALIDCALL;
  /* The fixed-function pipeline takes a normal of three floats only. */
  if (SUCCEEDED(hr) && !function)
    for (UINT e = 0; e < decl->element_count; ++e)
      if (decl->elements[e].usage == D8_USAGE_NORMAL && decl->elements[e].method == D8_METHOD_DEFAULT && decl->elements[e].type != D3DVSDT_FLOAT3) hr = D3DERR_INVALIDCALL;
  D8ShaderInfo info = {0};
  if (SUCCEEDED(hr) && function && d8_shader_scan((const uint32_t *)function, TOKEN_LIMIT, 1, &info)) hr = D3DERR_INVALIDCALL;
  /* Direct3D 8 refuses a shader that reads an input its declaration has not, or a constant beyond the caps' (by number or def). */
  if (SUCCEEDED(hr) && function && ((info.inputs_read & ~decl->declared) || info.constants_used > d->vs_constants)) hr = D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr)) {
    /* The program's tokens, kept for the Get methods, and the constants the declaration loads. */
    s->declaration_bytes = decl->tokens * 4u;
    s->declaration = mem_alloc(s->declaration_bytes);
    s->load_count = decl->load_count;
    UINT floats = 0;
    for (UINT i = 0; i < decl->load_count; ++i) { s->loads[i] = decl->loads[i]; floats += decl->loads[i].count * 4u; }
    s->load_data = mem_alloc((SIZE_T)floats * sizeof(float));
    if (function) {
      s->function_bytes = info.tokens * 4u;
      s->function = mem_alloc(s->function_bytes);
    }
    if (!s->declaration || !s->load_data || (function && !s->function)) hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) {
    memcpy(s->declaration, declaration, s->declaration_bytes);
    if (function) memcpy(s->function, function, s->function_bytes);
    UINT at = 0;
    for (UINT i = 0; i < decl->load_count; ++i) {
      memcpy(s->load_data + at, declaration + decl->loads[i].token, (SIZE_T)decl->loads[i].count * 16u);
      s->loads[i].token = at;      /* (from here on: where its floats are in load_data) */
      at += decl->loads[i].count * 4u;
    }
    /* The Direct3D 9 declaration: the elements and the end. */
    D3DVERTEXELEMENT9 elements[D8_MAX_ELEMENTS + 1];
    for (UINT e = 0; e < decl->element_count; ++e) memcpy(&elements[e], &decl->elements[e], sizeof elements[e]);
    const D3DVERTEXELEMENT9 end = {D3D9_DECL_END_STREAM, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};
    elements[decl->element_count] = end;
    hr = CALL9(D9(d), CreateVertexDeclaration, elements, &s->decl9);
  }
  if (SUCCEEDED(hr) && function) {
    const size_t count = d8_vertex_shader_tokens((const uint32_t *)function, &info, decl, NULL, 0);
    tokens = mem_alloc(count * sizeof *tokens);
    if (!tokens) hr = E_OUTOFMEMORY;
    else if (d8_vertex_shader_tokens((const uint32_t *)function, &info, decl, tokens, count) != count) hr = D3DERR_INVALIDCALL;
    else hr = CALL9(D9(d), CreateVertexShader, (const DWORD *)tokens, &s->vs9);
  }
  DWORD h = 0;
  if (SUCCEEDED(hr)) {
    EnterCriticalSection(&d->lock);
    h = handle_add(&d->vertex_shaders, s);
    LeaveCriticalSection(&d->lock);
    if (!h) hr = E_OUTOFMEMORY;
  }
  mem_free(tokens);
  mem_free(decl);
  if (FAILED(hr)) { vertex_shader_free(s); return hr == E_OUTOFMEMORY ? hr : D3DERR_INVALIDCALL; }
  *handle = h + VS_HANDLE_BASE;
  return D3D_OK;
}
static VertexShader8 *vertex_shader_of(Device8 *d, DWORD handle) {
  if (handle <= VS_HANDLE_BASE) return NULL;
  EnterCriticalSection(&d->lock);
  VertexShader8 *s = handle_get(&d->vertex_shaders, handle - VS_HANDLE_BASE);
  LeaveCriticalSection(&d->lock);
  return s;
}
static PixelShader8 *pixel_shader_of(Device8 *d, DWORD handle) {
  if (handle <= VS_HANDLE_BASE) return NULL;
  EnterCriticalSection(&d->lock);
  PixelShader8 *s = handle_get(&d->pixel_shaders, handle - VS_HANDLE_BASE);
  LeaveCriticalSection(&d->lock);
  return s;
}

/* Sets a vertex shader handle (an FVF code, or a shader) on the Direct3D 9 device: its declaration and shader, and the constants its declaration
 * loads. While a state block is recorded, Direct3D 9 records these calls. */
static HRESULT vertex_shader_apply(Device8 *d, DWORD handle) {
  if (handle <= VS_HANDLE_BASE) {
    HRESULT hr = CALL9(D9(d), SetVertexShader, NULL);
    if (SUCCEEDED(hr)) hr = CALL9(D9(d), SetFVF, handle);
    return hr;
  }
  VertexShader8 *s = vertex_shader_of(d, handle);
  if (!s) return D3DERR_INVALIDCALL;
  HRESULT hr = CALL9(D9(d), SetVertexDeclaration, s->decl9);
  if (SUCCEEDED(hr)) hr = CALL9(D9(d), SetVertexShader, s->vs9);
  for (UINT i = 0; SUCCEEDED(hr) && i < s->load_count; ++i)
    hr = CALL9(D9(d), SetVertexShaderConstantF, s->loads[i].first, s->load_data + s->loads[i].token, s->loads[i].count);
  return hr;
}
static HRESULT pixel_shader_apply(Device8 *d, DWORD handle) {
  if (!handle) return CALL9(D9(d), SetPixelShader, NULL);
  PixelShader8 *s = pixel_shader_of(d, handle);
  return s ? CALL9(D9(d), SetPixelShader, s->ps9) : D3DERR_INVALIDCALL;
}

/* What a state block being recorded takes instead of the device (Direct3D 8's own state; Direct3D 9 records its own calls). */
static State8 *state_target(Device8 *d, UINT has) {
  if (d->recording) { d->recording->has |= has; return &d->recording->state; }
  return &d->state;
}

static HRESULT STDMETHODCALLTYPE dev_SetVertexShader(Device8 *d, DWORD handle) {
  EnterCriticalSection(&d->lock);
  const HRESULT hr = vertex_shader_apply(d, handle);
  if (SUCCEEDED(hr)) state_target(d, HAS_VS)->vs = handle;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShader(Device8 *d, DWORD *handle) {
  if (!handle) return D3DERR_INVALIDCALL;
  *handle = d->state.vs;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_DeleteVertexShader(Device8 *d, DWORD handle) {
  if (handle <= VS_HANDLE_BASE) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  VertexShader8 *s = handle_remove(&d->vertex_shaders, handle - VS_HANDLE_BASE);
  if (s && d->state.vs == handle) {      /* (the shader that is set: the device has none after it) */
    CALL9(D9(d), SetVertexShader, NULL);
    d->state.vs = 0;
  }
  LeaveCriticalSection(&d->lock);
  if (!s) return D3DERR_INVALIDCALL;
  vertex_shader_free(s);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetVertexShaderConstant(Device8 *d, DWORD first, const void *data, DWORD count) {
  if (first > d->vs_constants || count > d->vs_constants - first) return D3DERR_INVALIDCALL;
  if (!count) return D3D_OK;
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetVertexShaderConstantF, first, data, count);
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderConstant(Device8 *d, DWORD first, void *data, DWORD count) {
  if (first > d->vs_constants || count > d->vs_constants - first) return D3DERR_INVALIDCALL;
  if (!count) return D3D_OK;
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetVertexShaderConstantF, first, data, count);
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderDeclaration(Device8 *d, DWORD handle, void *data, DWORD *size) {
  EnterCriticalSection(&d->lock);
  VertexShader8 *s = vertex_shader_of(d, handle);
  const HRESULT hr = s ? copy_out(s->declaration, s->declaration_bytes, data, size) : D3DERR_INVALIDCALL;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetVertexShaderFunction(Device8 *d, DWORD handle, void *data, DWORD *size) {
  EnterCriticalSection(&d->lock);
  VertexShader8 *s = vertex_shader_of(d, handle);
  const HRESULT hr = s ? copy_out(s->function, s->function_bytes, data, size) : D3DERR_INVALIDCALL;      /* (the fixed-function pipeline: 0 bytes) */
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CreatePixelShader(Device8 *d, const DWORD *function, DWORD *handle) {
  if (!function || !handle) return D3DERR_INVALIDCALL;
  D8ShaderInfo info;
  if (d8_shader_scan((const uint32_t *)function, TOKEN_LIMIT, 0, &info)) return D3DERR_INVALIDCALL;
  PixelShader8 *s = mem_alloc(sizeof *s);
  uint32_t *tokens = mem_alloc((SIZE_T)info.tokens * sizeof *tokens);
  HRESULT hr = s && tokens ? D3D_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    s->function_bytes = info.tokens * 4u;
    s->function = mem_alloc(s->function_bytes);
    if (!s->function) hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) {
    memcpy(s->function, function, s->function_bytes);
    d8_pixel_shader_tokens((const uint32_t *)function, &info, tokens, info.tokens);
    hr = CALL9(D9(d), CreatePixelShader, (const DWORD *)tokens, &s->ps9);
  }
  DWORD h = 0;
  if (SUCCEEDED(hr)) {
    EnterCriticalSection(&d->lock);
    h = handle_add(&d->pixel_shaders, s);
    LeaveCriticalSection(&d->lock);
    if (!h) hr = E_OUTOFMEMORY;
  }
  mem_free(tokens);
  if (FAILED(hr)) { pixel_shader_free(s); return hr == E_OUTOFMEMORY ? hr : D3DERR_INVALIDCALL; }
  *handle = h + VS_HANDLE_BASE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetPixelShader(Device8 *d, DWORD handle) {
  EnterCriticalSection(&d->lock);
  const HRESULT hr = pixel_shader_apply(d, handle);
  if (SUCCEEDED(hr)) state_target(d, HAS_PS)->ps = handle;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShader(Device8 *d, DWORD *handle) {
  if (!handle) return D3DERR_INVALIDCALL;
  *handle = d->state.ps;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_DeletePixelShader(Device8 *d, DWORD handle) {
  if (handle <= VS_HANDLE_BASE) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  PixelShader8 *s = handle_remove(&d->pixel_shaders, handle - VS_HANDLE_BASE);
  if (s && d->state.ps == handle) {
    CALL9(D9(d), SetPixelShader, NULL);
    d->state.ps = 0;
  }
  LeaveCriticalSection(&d->lock);
  if (!s) return D3DERR_INVALIDCALL;
  pixel_shader_free(s);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetPixelShaderConstant(Device8 *d, DWORD first, const void *data, DWORD count) {
  if (first > PS_CONSTANTS || count > PS_CONSTANTS - first) return D3DERR_INVALIDCALL;
  if (!count) return D3D_OK;
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetPixelShaderConstantF, first, data, count);
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderConstant(Device8 *d, DWORD first, void *data, DWORD count) {
  if (first > PS_CONSTANTS || count > PS_CONSTANTS - first) return D3DERR_INVALIDCALL;
  if (!count) return D3D_OK;
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetPixelShaderConstantF, first, data, count);
}
static HRESULT STDMETHODCALLTYPE dev_GetPixelShaderFunction(Device8 *d, DWORD handle, void *data, DWORD *size) {
  EnterCriticalSection(&d->lock);
  PixelShader8 *s = pixel_shader_of(d, handle);
  const HRESULT hr = s ? copy_out(s->function, s->function_bytes, data, size) : D3DERR_INVALIDCALL;
  LeaveCriticalSection(&d->lock);
  return hr;
}

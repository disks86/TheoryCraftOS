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

/* Part of d3d10.c: state blocks. A D3D10_STATE_BLOCK_MASK has a bit for each piece of the device's state (a shader, each sampler, shader resource and constant buffer
 * slot of the three stages, each vertex buffer slot, the index buffer, ...); the D3D10StateBlockMask* functions set and combine them, and a state block made with a mask
 * (D3D10CreateStateBlock) captures those pieces of a device and puts them back - through the device's own Get and Set methods, so it holds references to what it
 * captured until it is released, captures again or ReleaseAllDeviceObjects drops them. */

/* ---- masks --------------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The bits of a state type: where they are in the mask and how many there are (0: not a state type). */
static BYTE *mask_field(D3D10_STATE_BLOCK_MASK *m, D3D10_DEVICE_STATE_TYPES type, UINT *bits) {
  *bits = 1;
  switch (type) {
    case D3D10_DST_SO_BUFFERS: return &m->SOBuffers;
    case D3D10_DST_OM_RENDER_TARGETS: return &m->OMRenderTargets;
    case D3D10_DST_OM_DEPTH_STENCIL_STATE: return &m->OMDepthStencilState;
    case D3D10_DST_OM_BLEND_STATE: return &m->OMBlendState;
    case D3D10_DST_VS: return &m->VS;
    case D3D10_DST_VS_SAMPLERS: *bits = D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; return m->VSSamplers;
    case D3D10_DST_VS_SHADER_RESOURCES: *bits = D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; return m->VSShaderResources;
    case D3D10_DST_VS_CONSTANT_BUFFERS: *bits = D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; return m->VSConstantBuffers;
    case D3D10_DST_GS: return &m->GS;
    case D3D10_DST_GS_SAMPLERS: *bits = D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; return m->GSSamplers;
    case D3D10_DST_GS_SHADER_RESOURCES: *bits = D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; return m->GSShaderResources;
    case D3D10_DST_GS_CONSTANT_BUFFERS: *bits = D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; return m->GSConstantBuffers;
    case D3D10_DST_PS: return &m->PS;
    case D3D10_DST_PS_SAMPLERS: *bits = D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; return m->PSSamplers;
    case D3D10_DST_PS_SHADER_RESOURCES: *bits = D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; return m->PSShaderResources;
    case D3D10_DST_PS_CONSTANT_BUFFERS: *bits = D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; return m->PSConstantBuffers;
    case D3D10_DST_IA_VERTEX_BUFFERS: *bits = D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; return m->IAVertexBuffers;
    case D3D10_DST_IA_INDEX_BUFFER: return &m->IAIndexBuffer;
    case D3D10_DST_IA_INPUT_LAYOUT: return &m->IAInputLayout;
    case D3D10_DST_IA_PRIMITIVE_TOPOLOGY: return &m->IAPrimitiveTopology;
    case D3D10_DST_RS_VIEWPORTS: return &m->RSViewports;
    case D3D10_DST_RS_SCISSOR_RECTS: return &m->RSScissorRects;
    case D3D10_DST_RS_RASTERIZER_STATE: return &m->RSRasterizerState;
    case D3D10_DST_PREDICATION: return &m->Predication;
    default: *bits = 0; return NULL;
  }
}
/* Sets (or clears) bits start .. start + count - 1 of a state type: E_INVALIDARG for bits it has not. */
static HRESULT mask_change(D3D10_STATE_BLOCK_MASK *m, D3D10_DEVICE_STATE_TYPES type, UINT start, UINT count, BOOL on) {
  UINT bits;
  if (!m) return E_INVALIDARG;
  BYTE *field = mask_field(m, type, &bits);
  if (!field || start >= bits || count > bits - start) return E_INVALIDARG;
  for (UINT i = start; i < start + count; ++i) {
    if (on) field[i / 8] |= (BYTE)(1u << (i % 8));
    else field[i / 8] &= (BYTE)~(1u << (i % 8));
  }
  return S_OK;
}
EXPORT HRESULT WINAPI D3D10StateBlockMaskEnableCapture(D3D10_STATE_BLOCK_MASK *mask, D3D10_DEVICE_STATE_TYPES type, UINT start, UINT count) {
  return mask_change(mask, type, start, count, TRUE);
}
EXPORT HRESULT WINAPI D3D10StateBlockMaskDisableCapture(D3D10_STATE_BLOCK_MASK *mask, D3D10_DEVICE_STATE_TYPES type, UINT start, UINT count) {
  return mask_change(mask, type, start, count, FALSE);
}
EXPORT HRESULT WINAPI D3D10StateBlockMaskEnableAll(D3D10_STATE_BLOCK_MASK *mask) {
  if (!mask) return E_INVALIDARG;
  memset(mask, 0xFF, sizeof *mask);      /* (every byte, the bits past the slots too, as Windows) */
  return S_OK;
}
EXPORT HRESULT WINAPI D3D10StateBlockMaskDisableAll(D3D10_STATE_BLOCK_MASK *mask) {
  if (!mask) return E_INVALIDARG;
  memset(mask, 0, sizeof *mask);
  return S_OK;
}
/* The bit of slot `index` as it is in the mask (not 0 or 1: the byte's bit itself, as Windows answers), FALSE for a slot the type has not. */
EXPORT BOOL WINAPI D3D10StateBlockMaskGetSetting(D3D10_STATE_BLOCK_MASK *mask, D3D10_DEVICE_STATE_TYPES type, UINT index) {
  UINT bits;
  if (!mask) return FALSE;
  const BYTE *field = mask_field(mask, type, &bits);
  if (!field || index >= bits) return FALSE;
  return field[index / 8] & (1u << (index % 8));
}
static HRESULT mask_combine(const D3D10_STATE_BLOCK_MASK *x, const D3D10_STATE_BLOCK_MASK *y, D3D10_STATE_BLOCK_MASK *result, int how) {
  if (!x || !y || !result) return E_INVALIDARG;
  const BYTE *a = (const BYTE *)x, *b = (const BYTE *)y;
  BYTE *r = (BYTE *)result;
  for (SIZE_T i = 0; i < sizeof *result; ++i) r[i] = how == 0 ? (BYTE)(a[i] ^ b[i]) : how == 1 ? (BYTE)(a[i] & b[i]) : (BYTE)(a[i] | b[i]);
  return S_OK;
}
EXPORT HRESULT WINAPI D3D10StateBlockMaskDifference(D3D10_STATE_BLOCK_MASK *x, D3D10_STATE_BLOCK_MASK *y, D3D10_STATE_BLOCK_MASK *result) { return mask_combine(x, y, result, 0); }
EXPORT HRESULT WINAPI D3D10StateBlockMaskIntersect(D3D10_STATE_BLOCK_MASK *x, D3D10_STATE_BLOCK_MASK *y, D3D10_STATE_BLOCK_MASK *result) { return mask_combine(x, y, result, 1); }
EXPORT HRESULT WINAPI D3D10StateBlockMaskUnion(D3D10_STATE_BLOCK_MASK *x, D3D10_STATE_BLOCK_MASK *y, D3D10_STATE_BLOCK_MASK *result) { return mask_combine(x, y, result, 2); }

/* ---- state blocks --------------------------------------------------------------------------------------------------------------------------------------------------- */

#define SB_SAMPLERS D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
#define SB_VIEWS D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define SB_BUFFERS D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
#define SB_VERTEX D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
#define SB_RECTS D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
/* What a state block holds of one shader stage. */
typedef struct SbStage {
  IUnknown *shader;
  ID3D10SamplerState *samplers[SB_SAMPLERS];
  ID3D10ShaderResourceView *views[SB_VIEWS];
  ID3D10Buffer *buffers[SB_BUFFERS];
} SbStage;
typedef struct StateBlock {
  DxObj o;
  ID3D10Device *device;
  D3D10_STATE_BLOCK_MASK mask;
  SbStage stage[3];                       /* vertex, geometry, pixel */
  ID3D10Buffer *vb[SB_VERTEX];
  UINT vb_stride[SB_VERTEX], vb_offset[SB_VERTEX];
  ID3D10Buffer *ib;
  DXGI_FORMAT ib_format;
  UINT ib_offset;
  ID3D10InputLayout *layout;
  D3D10_PRIMITIVE_TOPOLOGY topology;
  ID3D10RenderTargetView *rtv[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
  ID3D10DepthStencilView *dsv;
  ID3D10DepthStencilState *depth;
  UINT stencil_ref;
  ID3D10BlendState *blend;
  FLOAT blend_factor[4];
  UINT sample_mask;
  D3D10_VIEWPORT viewports[SB_RECTS];
  UINT viewport_count;
  D3D10_RECT scissors[SB_RECTS];
  UINT scissor_count;
  ID3D10RasterizerState *raster;
  ID3D10Buffer *so[D3D10_SO_BUFFER_SLOT_COUNT];
  UINT so_offset[D3D10_SO_BUFFER_SLOT_COUNT];
  ID3D10Predicate *predicate;
  BOOL predicate_value;
} StateBlock;

static void sb_drop(void *slot_address) {
  IUnknown **slot = slot_address;
  if (*slot) { (*slot)->lpVtbl->Release(*slot); *slot = NULL; }
}
static void sb_drop_all(StateBlock *s) {
  for (UINT k = 0; k < 3; ++k) {
    sb_drop(&s->stage[k].shader);
    for (UINT i = 0; i < SB_SAMPLERS; ++i) sb_drop(&s->stage[k].samplers[i]);
    for (UINT i = 0; i < SB_VIEWS; ++i) sb_drop(&s->stage[k].views[i]);
    for (UINT i = 0; i < SB_BUFFERS; ++i) sb_drop(&s->stage[k].buffers[i]);
  }
  for (UINT i = 0; i < SB_VERTEX; ++i) sb_drop(&s->vb[i]);
  sb_drop(&s->ib);
  sb_drop(&s->layout);
  for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) sb_drop(&s->rtv[i]);
  sb_drop(&s->dsv);
  sb_drop(&s->depth);
  sb_drop(&s->blend);
  sb_drop(&s->raster);
  for (UINT i = 0; i < D3D10_SO_BUFFER_SLOT_COUNT; ++i) sb_drop(&s->so[i]);
  sb_drop(&s->predicate);
}
static void sb_destroy(DxObj *o) {
  StateBlock *s = (StateBlock *)o;
  sb_drop_all(s);
  if (s->device) ID3D10Device_Release(s->device);
}
static BOOL sb_bit(const BYTE *field, UINT i) { return (field[i / 8] >> (i % 8)) & 1; }

/* The slots of a stage that are in the mask, one by one, with the device's getters (in place: each slot's old reference given back first). */
#define SB_GET_SLOTS(field, count, array, getter)                                                                                                                \
  for (UINT i = 0; i < (count); ++i) if (sb_bit((field), i)) { sb_drop(&(array)[i]); getter(dev, i, 1, &(array)[i]); }
#define SB_SET_SLOTS(field, count, array, setter)                                                                                                                \
  for (UINT i = 0; i < (count); ++i) if (sb_bit((field), i)) setter(dev, i, 1, &(array)[i]);

static HRESULT STDMETHODCALLTYPE sb_Capture(StateBlock *s) {
  ID3D10Device *dev = s->device;
  const D3D10_STATE_BLOCK_MASK *m = &s->mask;
  if (m->VS & 1) { sb_drop(&s->stage[0].shader); ID3D10Device_VSGetShader(dev, (ID3D10VertexShader **)&s->stage[0].shader); }
  SB_GET_SLOTS(m->VSSamplers, SB_SAMPLERS, s->stage[0].samplers, ID3D10Device_VSGetSamplers)
  SB_GET_SLOTS(m->VSShaderResources, SB_VIEWS, s->stage[0].views, ID3D10Device_VSGetShaderResources)
  SB_GET_SLOTS(m->VSConstantBuffers, SB_BUFFERS, s->stage[0].buffers, ID3D10Device_VSGetConstantBuffers)
  if (m->GS & 1) { sb_drop(&s->stage[1].shader); ID3D10Device_GSGetShader(dev, (ID3D10GeometryShader **)&s->stage[1].shader); }
  SB_GET_SLOTS(m->GSSamplers, SB_SAMPLERS, s->stage[1].samplers, ID3D10Device_GSGetSamplers)
  SB_GET_SLOTS(m->GSShaderResources, SB_VIEWS, s->stage[1].views, ID3D10Device_GSGetShaderResources)
  SB_GET_SLOTS(m->GSConstantBuffers, SB_BUFFERS, s->stage[1].buffers, ID3D10Device_GSGetConstantBuffers)
  if (m->PS & 1) { sb_drop(&s->stage[2].shader); ID3D10Device_PSGetShader(dev, (ID3D10PixelShader **)&s->stage[2].shader); }
  SB_GET_SLOTS(m->PSSamplers, SB_SAMPLERS, s->stage[2].samplers, ID3D10Device_PSGetSamplers)
  SB_GET_SLOTS(m->PSShaderResources, SB_VIEWS, s->stage[2].views, ID3D10Device_PSGetShaderResources)
  SB_GET_SLOTS(m->PSConstantBuffers, SB_BUFFERS, s->stage[2].buffers, ID3D10Device_PSGetConstantBuffers)
  for (UINT i = 0; i < SB_VERTEX; ++i)
    if (sb_bit(m->IAVertexBuffers, i)) { sb_drop(&s->vb[i]); ID3D10Device_IAGetVertexBuffers(dev, i, 1, &s->vb[i], &s->vb_stride[i], &s->vb_offset[i]); }
  if (m->IAIndexBuffer & 1) { sb_drop(&s->ib); ID3D10Device_IAGetIndexBuffer(dev, &s->ib, &s->ib_format, &s->ib_offset); }
  if (m->IAInputLayout & 1) { sb_drop(&s->layout); ID3D10Device_IAGetInputLayout(dev, &s->layout); }
  if (m->IAPrimitiveTopology & 1) ID3D10Device_IAGetPrimitiveTopology(dev, &s->topology);
  if (m->OMRenderTargets & 1) {
    for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) sb_drop(&s->rtv[i]);
    sb_drop(&s->dsv);
    ID3D10Device_OMGetRenderTargets(dev, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtv, &s->dsv);
  }
  if (m->OMDepthStencilState & 1) { sb_drop(&s->depth); ID3D10Device_OMGetDepthStencilState(dev, &s->depth, &s->stencil_ref); }
  if (m->OMBlendState & 1) { sb_drop(&s->blend); ID3D10Device_OMGetBlendState(dev, &s->blend, s->blend_factor, &s->sample_mask); }
  if (m->RSViewports & 1) { s->viewport_count = SB_RECTS; ID3D10Device_RSGetViewports(dev, &s->viewport_count, s->viewports); }
  if (m->RSScissorRects & 1) { s->scissor_count = SB_RECTS; ID3D10Device_RSGetScissorRects(dev, &s->scissor_count, s->scissors); }
  if (m->RSRasterizerState & 1) { sb_drop(&s->raster); ID3D10Device_RSGetState(dev, &s->raster); }
  if (m->SOBuffers & 1) {
    for (UINT i = 0; i < D3D10_SO_BUFFER_SLOT_COUNT; ++i) sb_drop(&s->so[i]);
    ID3D10Device_SOGetTargets(dev, D3D10_SO_BUFFER_SLOT_COUNT, s->so, s->so_offset);
  }
  if (m->Predication & 1) { sb_drop(&s->predicate); ID3D10Device_GetPredication(dev, &s->predicate, &s->predicate_value); }
  return S_OK;
}

/* (the viewports and scissor rectangles are set as they were captured: the count they had) */
static HRESULT STDMETHODCALLTYPE sb_Apply(StateBlock *s) {
  ID3D10Device *dev = s->device;
  const D3D10_STATE_BLOCK_MASK *m = &s->mask;
  if (m->VS & 1) ID3D10Device_VSSetShader(dev, (ID3D10VertexShader *)s->stage[0].shader);
  SB_SET_SLOTS(m->VSSamplers, SB_SAMPLERS, s->stage[0].samplers, ID3D10Device_VSSetSamplers)
  SB_SET_SLOTS(m->VSShaderResources, SB_VIEWS, s->stage[0].views, ID3D10Device_VSSetShaderResources)
  SB_SET_SLOTS(m->VSConstantBuffers, SB_BUFFERS, s->stage[0].buffers, ID3D10Device_VSSetConstantBuffers)
  if (m->GS & 1) ID3D10Device_GSSetShader(dev, (ID3D10GeometryShader *)s->stage[1].shader);
  SB_SET_SLOTS(m->GSSamplers, SB_SAMPLERS, s->stage[1].samplers, ID3D10Device_GSSetSamplers)
  SB_SET_SLOTS(m->GSShaderResources, SB_VIEWS, s->stage[1].views, ID3D10Device_GSSetShaderResources)
  SB_SET_SLOTS(m->GSConstantBuffers, SB_BUFFERS, s->stage[1].buffers, ID3D10Device_GSSetConstantBuffers)
  if (m->PS & 1) ID3D10Device_PSSetShader(dev, (ID3D10PixelShader *)s->stage[2].shader);
  SB_SET_SLOTS(m->PSSamplers, SB_SAMPLERS, s->stage[2].samplers, ID3D10Device_PSSetSamplers)
  SB_SET_SLOTS(m->PSShaderResources, SB_VIEWS, s->stage[2].views, ID3D10Device_PSSetShaderResources)
  SB_SET_SLOTS(m->PSConstantBuffers, SB_BUFFERS, s->stage[2].buffers, ID3D10Device_PSSetConstantBuffers)
  for (UINT i = 0; i < SB_VERTEX; ++i)
    if (sb_bit(m->IAVertexBuffers, i)) ID3D10Device_IASetVertexBuffers(dev, i, 1, &s->vb[i], &s->vb_stride[i], &s->vb_offset[i]);
  if (m->IAIndexBuffer & 1) ID3D10Device_IASetIndexBuffer(dev, s->ib, s->ib_format, s->ib_offset);
  if (m->IAInputLayout & 1) ID3D10Device_IASetInputLayout(dev, s->layout);
  if (m->IAPrimitiveTopology & 1) ID3D10Device_IASetPrimitiveTopology(dev, s->topology);
  if (m->OMRenderTargets & 1) ID3D10Device_OMSetRenderTargets(dev, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtv, s->dsv);
  if (m->OMDepthStencilState & 1) ID3D10Device_OMSetDepthStencilState(dev, s->depth, s->stencil_ref);
  if (m->OMBlendState & 1) ID3D10Device_OMSetBlendState(dev, s->blend, s->blend_factor, s->sample_mask);
  if (m->RSViewports & 1) ID3D10Device_RSSetViewports(dev, s->viewport_count, s->viewports);
  if (m->RSScissorRects & 1) ID3D10Device_RSSetScissorRects(dev, s->scissor_count, s->scissors);
  if (m->RSRasterizerState & 1) ID3D10Device_RSSetState(dev, s->raster);
  if (m->SOBuffers & 1) ID3D10Device_SOSetTargets(dev, D3D10_SO_BUFFER_SLOT_COUNT, s->so, s->so_offset);
  if (m->Predication & 1) ID3D10Device_SetPredication(dev, s->predicate, s->predicate_value);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sb_ReleaseAllDeviceObjects(StateBlock *s) { sb_drop_all(s); return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_GetDevice(StateBlock *s, ID3D10Device **out) {
  if (!out) return E_INVALIDARG;
  ID3D10Device_AddRef(s->device);
  *out = s->device;
  return S_OK;
}

extern const ID3D10StateBlockVtbl sb_vtbl;
EXPORT HRESULT WINAPI D3D10CreateStateBlock(ID3D10Device *device, D3D10_STATE_BLOCK_MASK *mask, ID3D10StateBlock **out) {
  static const GUID *const iids[] = {&IID_ID3D10StateBlock, NULL};
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!device || !mask) return E_INVALIDARG;
  StateBlock *s = mem_alloc(sizeof *s);
  if (!s) return E_OUTOFMEMORY;
  dx_init(&s->o, &sb_vtbl, iids, sb_destroy);
  ID3D10Device_AddRef(device);
  s->device = device;
  s->mask = *mask;
  *out = (ID3D10StateBlock *)s;
  return S_OK;
}

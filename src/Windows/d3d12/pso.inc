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

/* d3d12.dll: root signatures (ID3D12RootSignature and the serializer), pipeline states (ID3D12PipelineState), blobs. Included by d3d12.c.
 *
 * A pipeline state is made of kernel shaders and pipelines. The kernel's pipeline has the vertex stride in it, which Direct3D 12 only gives with IASetVertexBuffers, and the
 * stencil reference, which OMSetStencilRef gives: so the kernel pipelines are made when a draw needs one, one for each (stride, reference) pair. */

/* ---- blobs (ID3DBlob) ---------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Blob {
  DxObj o;
  void *data;
  SIZE_T size;
} Blob;
static const GUID *const g_blob_iids[] = {&IID_ID3D10Blob, NULL};
static void blob_destroy(DxObj *o) { mem_free(((Blob *)o)->data); }
static LPVOID STDMETHODCALLTYPE blob_GetBufferPointer(Blob *This) { return This->data; }
static SIZE_T STDMETHODCALLTYPE blob_GetBufferSize(Blob *This) { return This->size; }

static Blob *blob_new(const void *data, SIZE_T size) {
  extern const ID3D10BlobVtbl blob_vtbl;
  Blob *b = mem_alloc(sizeof *b);
  if (!b) return NULL;
  dx_init(OBJ(b), &blob_vtbl, g_blob_iids, blob_destroy);
  b->data = mem_alloc(size);
  b->size = size;
  if (!b->data) { dx_Release(OBJ(b)); return NULL; }
  if (data) memcpy(b->data, data, size);
  return b;
}

/* ---- DXBC containers --------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define FOURCC(a, b, c, d) ((UINT)(a) | ((UINT)(b) << 8) | ((UINT)(c) << 16) | ((UINT)(d) << 24))

static UINT rd32(const BYTE *p) { return (UINT)p[0] | ((UINT)p[1] << 8) | ((UINT)p[2] << 16) | ((UINT)p[3] << 24); }

/* The chunk with the four-character code in a DXBC container. */
static const BYTE *dxbc_chunk(const BYTE *blob, SIZE_T size, UINT fourcc, UINT *chunk_size) {
  if (size < 32 || rd32(blob) != FOURCC('D', 'X', 'B', 'C')) return NULL;
  const UINT count = rd32(blob + 28);
  if (count > 64 || size < 32 + (SIZE_T)count * 4) return NULL;
  for (UINT i = 0; i < count; ++i) {
    const UINT offset = rd32(blob + 32 + i * 4);
    if (offset > size || size - offset < 8) return NULL;
    const UINT n = rd32(blob + offset + 4);
    if (size - offset - 8 < n) return NULL;
    if (rd32(blob + offset) == fourcc) {
      *chunk_size = n;
      return blob + offset + 8;
    }
  }
  return NULL;
}

typedef struct SigElement { char name[32]; UINT index, sysval, reg; } SigElement;

/* The elements of the input or output signature (ISGN / ISG1 or OSGN / OSG1). */
static UINT dxbc_signature(const BYTE *blob, SIZE_T size, int output, SigElement *out, UINT capacity, int *found) {
  UINT n = 0, stride = 24;
  UINT chunk_size = 0;
  const BYTE *c = dxbc_chunk(blob, size, output ? FOURCC('O', 'S', 'G', 'N') : FOURCC('I', 'S', 'G', 'N'), &chunk_size);
  if (!c) {
    c = dxbc_chunk(blob, size, output ? FOURCC('O', 'S', 'G', '1') : FOURCC('I', 'S', 'G', '1'), &chunk_size);
    stride = 32;
  }
  *found = c != NULL;
  if (!c || chunk_size < 8) return 0;
  const UINT count = rd32(c);
  for (UINT i = 0; i < count && n < capacity; ++i) {
    const UINT at = 8 + i * stride;
    if (at + stride > chunk_size) break;
    const BYTE *e = c + at + (stride == 32 ? 4 : 0);
    const UINT name_offset = rd32(e);
    SigElement *el = &out[n++];
    memset(el, 0, sizeof *el);
    for (UINT k = 0; name_offset < chunk_size && k < sizeof el->name - 1 && c[name_offset + k]; ++k) el->name[k] = (char)c[name_offset + k];
    el->index = rd32(e + 4);
    el->sysval = rd32(e + 8);
    el->reg = rd32(e + 16);
  }
  return n;
}

static int ieq(const char *a, const char *b) {
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* ---- root signatures ------------------------------------------------------------------------------------------------------------------------------------------------------- */

struct RootSig {
  DObj d;
  UINT khandle;
  BYTE *blob;
  SIZE_T blob_size;
  UINT textures;           /* the texture units the signature's SRV ranges reach (t0 .. t<n-1>); an unbounded range reaches as many as the device has */
  int textures_unbounded;
  UINT parameter_count;
  UINT ptype[16];          /* of each parameter: D3D12_ROOT_PARAMETER_TYPE */
  UINT pconstants[16];     /* the 32-bit values of a constants parameter */
};

static const GUID *const g_rs_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12RootSignature, NULL};

static void rs_destroy(DxObj *o) {
  RootSig *r = (RootSig *)o;
  if (r->khandle) {
    UINT request[2] = {ESC_DESTROY_ROOT_SIGNATURE, r->khandle};
    dev_escape(r->d.dev, request, 2, NULL, 0);
  }
  mem_free(r->blob);
  dobj_child_destroy(o);
}

/* Reads what the pipeline needs from a serialized root signature: the highest texture register of its SRV ranges. */
static void rs_scan(RootSig *r, const BYTE *b, SIZE_T n) {
  UINT chunk = 0;
  const BYTE *rts0 = dxbc_chunk(b, n, FOURCC('R', 'T', 'S', '0'), &chunk);
  if (rts0) { b = rts0; n = chunk; }
  if (n < 24) return;
  const UINT version = rd32(b), count = rd32(b + 4), params = rd32(b + 8);
  if (version != 1 && version != 2) return;
  r->parameter_count = count;
  for (UINT i = 0; i < count && i < 16; ++i) {
    if (params + (SIZE_T)i * 12 + 12 > n) return;
    const UINT type = rd32(b + params + i * 12), payload = rd32(b + params + i * 12 + 8);
    r->ptype[i] = type;
    if (type == 1 && payload + 12 <= n) r->pconstants[i] = rd32(b + payload + 8);
    if (type != 0 || payload + 8 > n) continue;
    const UINT range_count = rd32(b + payload), ranges = rd32(b + payload + 4), stride = version == 2 ? 24 : 20;
    for (UINT k = 0; k < range_count && k < 8; ++k) {
      if (ranges + (SIZE_T)k * stride + stride > n) return;
      const BYTE *range = b + ranges + k * stride;
      if (rd32(range) != 0) continue;      /* SRV */
      const UINT num = rd32(range + 4), base = rd32(range + 8);
      if (num == 0xFFFFFFFFu) r->textures_unbounded = 1;
      const UINT top = num == 0xFFFFFFFFu ? 0 : base + num;
      if (top > r->textures) r->textures = top;
    }
  }
}

static HRESULT STDMETHODCALLTYPE dev_CreateRootSignature(Dev *This, UINT mask, const void *bytecode, SIZE_T length, REFIID riid, void **out) {
  extern const ID3D12RootSignatureVtbl rs_vtbl;
  if (!bytecode || length < 24 || length > 4096) return E_INVALIDARG;
  RootSig *r = dobj_new(sizeof *r, This, &rs_vtbl, g_rs_iids, rs_destroy);
  if (!r) return E_OUTOFMEMORY;
  r->blob = mem_alloc(length);
  if (!r->blob) { dx_Release(OBJ(r)); return E_OUTOFMEMORY; }
  memcpy(r->blob, bytecode, length);
  r->blob_size = length;
  rs_scan(r, r->blob, length);
  const UINT words = 2 + (UINT)((length + 3) / 4);
  UINT *request = mem_alloc((SIZE_T)(words + 2) * 4);
  UINT reply[1] = {0};
  if (!request) { dx_Release(OBJ(r)); return E_OUTOFMEMORY; }
  request[0] = ESC_CREATE_ROOT_SIGNATURE;
  request[1] = (UINT)length;
  memcpy(request + 2, bytecode, length);
  const NTSTATUS st = dev_escape(This, request, words, reply, 1);
  mem_free(request);
  if (!KMT_OK(st)) { dx_Release(OBJ(r)); return hr_from(st) == E_FAIL ? E_INVALIDARG : hr_from(st); }
  r->khandle = reply[0];
  return query_out((IUnknown *)r, riid, out);
}

/* ---- the serializer ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Bytes { BYTE *p; SIZE_T size, cap; int bad; } Bytes;
static void bytes_put(Bytes *b, UINT v) {
  if (b->size + 4 > b->cap) {
    const SIZE_T cap = b->cap ? b->cap * 2 : 256;
    BYTE *n = mem_grow(b->p, cap);
    if (!n) { b->bad = 1; return; }
    b->p = n; b->cap = cap;
  }
  memcpy(b->p + b->size, &v, 4);
  b->size += 4;
}

/* Writes a root signature of version 1.0 (`v11` 0) or 1.1 from the 1.0 description, or a 1.1 description (`d11` set). Parameter payloads follow the parameter array, then ranges,
 * then the static samplers. */
static Blob *serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC *d10, const D3D12_ROOT_SIGNATURE_DESC1 *d11, int out_v11) {
  Bytes b = {0};
  const UINT np = d11 ? d11->NumParameters : d10->NumParameters;
  const UINT ns = d11 ? d11->NumStaticSamplers : d10->NumStaticSamplers;
  const D3D12_STATIC_SAMPLER_DESC *samplers = d11 ? d11->pStaticSamplers : d10->pStaticSamplers;
  const UINT flags = d11 ? d11->Flags : d10->Flags;
  const UINT range_stride = out_v11 ? 24 : 20;
  /* pass 1: the sizes */
  UINT payload_bytes = 0, range_bytes = 0;
  for (UINT i = 0; i < np; ++i) {
    const UINT type = d11 ? d11->pParameters[i].ParameterType : d10->pParameters[i].ParameterType;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      payload_bytes += 8;
      range_bytes += (d11 ? d11->pParameters[i].DescriptorTable.NumDescriptorRanges : d10->pParameters[i].DescriptorTable.NumDescriptorRanges) * range_stride;
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      payload_bytes += 12;
    } else {
      payload_bytes += out_v11 ? 12 : 8;
    }
  }
  const UINT params_at = 24, payloads_at = params_at + np * 12, ranges_at = payloads_at + payload_bytes, samplers_at = ranges_at + range_bytes;
  bytes_put(&b, out_v11 ? 2 : 1);
  bytes_put(&b, np);
  bytes_put(&b, params_at);
  bytes_put(&b, ns);
  bytes_put(&b, samplers_at);
  bytes_put(&b, flags);
  UINT payload = payloads_at;
  for (UINT i = 0; i < np; ++i) {
    const UINT type = d11 ? d11->pParameters[i].ParameterType : d10->pParameters[i].ParameterType;
    const UINT vis = d11 ? d11->pParameters[i].ShaderVisibility : d10->pParameters[i].ShaderVisibility;
    bytes_put(&b, type);
    bytes_put(&b, vis);
    bytes_put(&b, payload);
    payload += type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 8 : type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? 12 : (out_v11 ? 12 : 8);
  }
  UINT range_at = ranges_at;
  for (UINT i = 0; i < np; ++i) {
    const UINT type = d11 ? d11->pParameters[i].ParameterType : d10->pParameters[i].ParameterType;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      const UINT n = d11 ? d11->pParameters[i].DescriptorTable.NumDescriptorRanges : d10->pParameters[i].DescriptorTable.NumDescriptorRanges;
      bytes_put(&b, n);
      bytes_put(&b, range_at);
      range_at += n * range_stride;
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      const D3D12_ROOT_CONSTANTS *c = d11 ? &d11->pParameters[i].Constants : &d10->pParameters[i].Constants;
      bytes_put(&b, c->ShaderRegister); bytes_put(&b, c->RegisterSpace); bytes_put(&b, c->Num32BitValues);
    } else {
      const D3D12_ROOT_DESCRIPTOR *rd = d11 ? (const D3D12_ROOT_DESCRIPTOR *)&d11->pParameters[i].Descriptor : &d10->pParameters[i].Descriptor;
      bytes_put(&b, rd->ShaderRegister); bytes_put(&b, rd->RegisterSpace);
      if (out_v11) bytes_put(&b, d11 ? d11->pParameters[i].Descriptor.Flags : 0);
    }
  }
  for (UINT i = 0; i < np; ++i) {
    const UINT type = d11 ? d11->pParameters[i].ParameterType : d10->pParameters[i].ParameterType;
    if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    const UINT n = d11 ? d11->pParameters[i].DescriptorTable.NumDescriptorRanges : d10->pParameters[i].DescriptorTable.NumDescriptorRanges;
    for (UINT k = 0; k < n; ++k) {
      UINT rt, num, base, space, offset, rflags = 0;
      if (d11) {
        const D3D12_DESCRIPTOR_RANGE1 *r = &d11->pParameters[i].DescriptorTable.pDescriptorRanges[k];
        rt = r->RangeType; num = r->NumDescriptors; base = r->BaseShaderRegister; space = r->RegisterSpace; offset = r->OffsetInDescriptorsFromTableStart; rflags = r->Flags;
      } else {
        const D3D12_DESCRIPTOR_RANGE *r = &d10->pParameters[i].DescriptorTable.pDescriptorRanges[k];
        rt = r->RangeType; num = r->NumDescriptors; base = r->BaseShaderRegister; space = r->RegisterSpace; offset = r->OffsetInDescriptorsFromTableStart;
      }
      bytes_put(&b, rt); bytes_put(&b, num); bytes_put(&b, base); bytes_put(&b, space);
      if (out_v11) bytes_put(&b, rflags);
      bytes_put(&b, offset);
    }
  }
  for (UINT i = 0; i < ns; ++i) {
    const D3D12_STATIC_SAMPLER_DESC *s = &samplers[i];
    bytes_put(&b, s->Filter); bytes_put(&b, s->AddressU); bytes_put(&b, s->AddressV); bytes_put(&b, s->AddressW);
    bytes_put(&b, bits_of(s->MipLODBias)); bytes_put(&b, s->MaxAnisotropy); bytes_put(&b, s->ComparisonFunc); bytes_put(&b, s->BorderColor);
    bytes_put(&b, bits_of(s->MinLOD)); bytes_put(&b, bits_of(s->MaxLOD)); bytes_put(&b, s->ShaderRegister); bytes_put(&b, s->RegisterSpace); bytes_put(&b, s->ShaderVisibility);
  }
  if (b.bad) { mem_free(b.p); return NULL; }
  Blob *blob = blob_new(b.p, b.size);
  mem_free(b.p);
  return blob;
}

EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error) {
  if (blob) *blob = NULL;
  if (error) *error = NULL;
  if (!desc || !blob || (version != D3D_ROOT_SIGNATURE_VERSION_1_0 && version != D3D_ROOT_SIGNATURE_VERSION_1_1)) return E_INVALIDARG;
  if ((desc->NumParameters && !desc->pParameters) || (desc->NumStaticSamplers && !desc->pStaticSamplers)) return E_INVALIDARG;
  Blob *b = serialize_root_signature(desc, NULL, version == D3D_ROOT_SIGNATURE_VERSION_1_1);
  if (!b) return E_OUTOFMEMORY;
  *blob = (ID3DBlob *)b;
  return S_OK;
}

EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **error) {
  if (blob) *blob = NULL;
  if (error) *error = NULL;
  if (!desc || !blob) return E_INVALIDARG;
  Blob *b = NULL;
  if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) b = serialize_root_signature(&desc->Desc_1_0, NULL, 0);
  else if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) b = serialize_root_signature(NULL, &desc->Desc_1_1, 1);
  else return E_INVALIDARG;
  if (!b) return E_OUTOFMEMORY;
  *blob = (ID3DBlob *)b;
  return S_OK;
}

/* ---- pipeline states ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define MAX_PSO_PIPELINES 8
typedef struct PsoPipe { UINT strides[8], stencil_ref, handle; } PsoPipe;

typedef struct Attr { UINT location, floats, offset, per_instance, slot; } Attr;

struct Pso {
  DObj d;
  int compute;
  RootSig *rs;
  UINT shaders[6];             /* vertex, pixel, geometry, hull, domain, compute (the kernel's handles; 0: none) */
  UINT attr_count;
  Attr attrs[16];
  UINT varyings, textures;
  D3D12_BLEND_DESC blend;
  D3D12_RASTERIZER_DESC raster;
  D3D12_DEPTH_STENCIL_DESC depth;
  UINT render_targets;
  DXGI_FORMAT rtv_formats[8];
  UINT slots_used;             /* the vertex buffers (input slots) the layout reads, as a mask */
  DXGI_FORMAT dsv_format;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  UINT so_entries, so_rasterized, so_strides[4];
  UINT so_entry[16][2];        /* the words of the kernel's description of each captured output */
  PsoPipe pipes[MAX_PSO_PIPELINES];
  UINT pipe_count;
  UINT compute_pipe;
};

static const GUID *const g_pso_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12PipelineState, NULL};

static void pso_destroy(DxObj *o) {
  Pso *p = (Pso *)o;
  for (UINT i = 0; i < p->pipe_count; ++i) {
    UINT request[2] = {ESC_DESTROY_PIPELINE, p->pipes[i].handle};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
  if (p->compute_pipe) {
    UINT request[2] = {ESC_DESTROY_PIPELINE, p->compute_pipe};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
  for (UINT i = 0; i < 6; ++i)
    if (p->shaders[i]) {
      UINT request[2] = {ESC_DESTROY_SHADER, p->shaders[i]};
      dev_escape(p->d.dev, request, 2, NULL, 0);
    }
  if (p->rs) dx_Release(OBJ(p->rs));
  dobj_child_destroy(o);
}

static HRESULT make_shader(Dev *dev, const D3D12_SHADER_BYTECODE *code, UINT stage, UINT *handle) {
  *handle = 0;
  if (!code->pShaderBytecode || !code->BytecodeLength) return S_OK;
  if (code->BytecodeLength > 60000) return DXGI_ERROR_UNSUPPORTED;
  const UINT words = 4 + (UINT)((code->BytecodeLength + 3) / 4);
  UINT *request = mem_alloc((SIZE_T)(words + 2) * 4);
  UINT reply[1] = {0};
  if (!request) return E_OUTOFMEMORY;
  request[0] = ESC_CREATE_SHADER;
  request[1] = 7;      /* ShaderKind::Dxbc (DXBC and the DXIL of Shader Model 6) */
  request[2] = stage;
  request[3] = (UINT)code->BytecodeLength;
  memcpy(request + 4, code->pShaderBytecode, code->BytecodeLength);
  const NTSTATUS st = dev_escape(dev, request, words, reply, 1);
  mem_free(request);
  if (!KMT_OK(st)) return hr_from(st) == E_FAIL ? E_INVALIDARG : hr_from(st);
  *handle = reply[0];
  return S_OK;
}

static int vertex_floats(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32_FLOAT: return 1;
    case DXGI_FORMAT_R32G32_FLOAT: return 2;
    case DXGI_FORMAT_R32G32B32_FLOAT: return 3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 4;
    default: return 0;
  }
}

static HRESULT STDMETHODCALLTYPE dev_CreateGraphicsPipelineState(Dev *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12PipelineStateVtbl pso_vtbl;
  if (!desc || !desc->pRootSignature || !desc->VS.pShaderBytecode) return E_INVALIDARG;
  if (desc->SampleDesc.Count > 1) return DXGI_ERROR_UNSUPPORTED;
  if (desc->StreamOutput.NumEntries > 16 || desc->StreamOutput.NumStrides > 4) return DXGI_ERROR_UNSUPPORTED;
  Pso *p = dobj_new(sizeof *p, This, &pso_vtbl, g_pso_iids, pso_destroy);
  if (!p) return E_OUTOFMEMORY;
  p->rs = (RootSig *)desc->pRootSignature;
  dx_AddRef(OBJ(p->rs));
  p->blend = desc->BlendState;
  p->raster = desc->RasterizerState;
  p->depth = desc->DepthStencilState;
  p->render_targets = desc->NumRenderTargets;
  memcpy(p->rtv_formats, desc->RTVFormats, sizeof p->rtv_formats);
  p->dsv_format = desc->DSVFormat;
  p->topology_type = desc->PrimitiveTopologyType;
  HRESULT hr = make_shader(This, &desc->VS, 0, &p->shaders[0]);
  if (SUCCEEDED(hr)) {
    /* (a pipeline with no pixel shader - stream output without rasterizing - gets one that the kernel's pipeline needs and nothing uses) */
    D3D12_SHADER_BYTECODE null_ps = {g_null_pixel_shader, sizeof g_null_pixel_shader};
    hr = make_shader(This, desc->PS.pShaderBytecode ? &desc->PS : &null_ps, 1, &p->shaders[1]);
  }
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->GS, 3, &p->shaders[2]);
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->HS, 4, &p->shaders[3]);
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->DS, 5, &p->shaders[4]);
  if (FAILED(hr)) { dx_Release(OBJ(p)); return hr; }
  /* the vertex inputs: register numbers from the vertex shader's signature */
  SigElement inputs[16], outputs[16];
  int found = 0, found_out = 0;
  const UINT n_in = dxbc_signature(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, 0, inputs, 16, &found);
  const Dev *dev = This;
  if (desc->InputLayout.NumElements > dev->caps.max_vertex_attributes || desc->InputLayout.NumElements > 16) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  if (desc->NumRenderTargets > dev->caps.max_color_targets || desc->NumRenderTargets > 8) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  UINT appended[8] = {0};
  for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
    const D3D12_INPUT_ELEMENT_DESC *e = &desc->InputLayout.pInputElementDescs[i];
    const int floats = vertex_floats(e->Format);
    if (e->InputSlot >= dev->caps.max_vertex_buffers || e->InputSlot >= 8 || !floats) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
    Attr *a = &p->attrs[p->attr_count++];
    a->floats = (UINT)floats;
    a->slot = e->InputSlot;
    a->per_instance = e->InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    a->offset = e->AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? appended[e->InputSlot] : e->AlignedByteOffset;
    appended[e->InputSlot] = a->offset + (UINT)floats * 4;
    p->slots_used |= 1u << e->InputSlot;
    a->location = i;
    for (UINT k = 0; k < n_in; ++k)
      if (ieq(inputs[k].name, e->SemanticName) && inputs[k].index == e->SemanticIndex) { a->location = inputs[k].reg; break; }
    if (a->location >= dev->caps.max_vertex_attributes) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  }
  /* the interpolants between the stages: the kernel counts the registers the vertex shader writes, its position included */
  const UINT n_out = dxbc_signature(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, 1, outputs, 16, &found_out);
  p->varyings = found_out ? n_out : 3;
  if (p->varyings > dev->caps.max_varyings) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  p->textures = p->rs->textures_unbounded ? dev->caps.max_texture_units : p->rs->textures;
  if (p->textures > dev->caps.max_texture_units) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  /* stream output: the captured outputs by their register in the output signature of the last stage before the rasterizer (the geometry shader, else the vertex shader) */
  if (desc->StreamOutput.NumEntries) {
    const D3D12_SHADER_BYTECODE *last = desc->GS.pShaderBytecode ? &desc->GS : &desc->VS;
    SigElement so_outputs[16];
    int so_found = 0;
    const UINT so_n = dxbc_signature(last->pShaderBytecode, last->BytecodeLength, 1, so_outputs, 16, &so_found);
    p->so_entries = desc->StreamOutput.NumEntries;
    p->so_rasterized = desc->StreamOutput.RasterizedStream;
    for (UINT i = 0; i < desc->StreamOutput.NumStrides; ++i) p->so_strides[i] = desc->StreamOutput.pBufferStrides[i];
    for (UINT i = 0; i < p->so_entries; ++i) {
      const D3D12_SO_DECLARATION_ENTRY *e = &desc->StreamOutput.pSODeclaration[i];
      UINT reg = 0xFFFFFFFFu;
      for (UINT k = 0; e->SemanticName && k < so_n; ++k)
        if (ieq(so_outputs[k].name, e->SemanticName) && so_outputs[k].index == e->SemanticIndex) reg = so_outputs[k].reg;
      if (reg == 0xFFFFFFFFu || e->OutputSlot > 3 || e->Stream > 3 || e->StartComponent > 3 || e->ComponentCount == 0 || e->ComponentCount > 4) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
      p->so_entry[i][0] = e->Stream | (reg << 8) | ((UINT)e->StartComponent << 16) | ((UINT)e->ComponentCount << 24);
      p->so_entry[i][1] = e->OutputSlot;
    }
  }
  return query_out((IUnknown *)p, riid, out);
}

static HRESULT STDMETHODCALLTYPE dev_CreateComputePipelineState(Dev *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12PipelineStateVtbl pso_vtbl;
  if (!desc || !desc->pRootSignature || !desc->CS.pShaderBytecode) return E_INVALIDARG;
  Pso *p = dobj_new(sizeof *p, This, &pso_vtbl, g_pso_iids, pso_destroy);
  if (!p) return E_OUTOFMEMORY;
  p->compute = 1;
  p->rs = (RootSig *)desc->pRootSignature;
  dx_AddRef(OBJ(p->rs));
  HRESULT hr = make_shader(This, &desc->CS, 2, &p->shaders[5]);
  if (SUCCEEDED(hr)) {
    UINT request[2] = {ESC_CREATE_COMPUTE_PIPELINE, p->shaders[5]};
    UINT reply[1] = {0};
    const NTSTATUS st = dev_escape(This, request, 2, reply, 1);
    if (KMT_OK(st)) p->compute_pipe = reply[0];
    else hr = hr_from(st);
  }
  if (FAILED(hr)) { dx_Release(OBJ(p)); return hr; }
  return query_out((IUnknown *)p, riid, out);
}

static HRESULT STDMETHODCALLTYPE pso_GetCachedBlob(Pso *This, ID3DBlob **blob) { if (blob) *blob = NULL; return E_NOTIMPL; }

/* ---- the translation of the fixed state to the kernel's -------------------------------------------------------------------------------------------------------------------------- */

static UINT blend_factor(UINT f) {
  switch (f) {
    case D3D12_BLEND_ZERO: return 0;
    case D3D12_BLEND_ONE: return 1;
    case D3D12_BLEND_SRC_COLOR: return 2;
    case D3D12_BLEND_INV_SRC_COLOR: return 3;
    case D3D12_BLEND_SRC_ALPHA: return 4;
    case D3D12_BLEND_INV_SRC_ALPHA: return 5;
    case D3D12_BLEND_DEST_ALPHA: return 8;
    case D3D12_BLEND_INV_DEST_ALPHA: return 9;
    case D3D12_BLEND_DEST_COLOR: return 6;
    case D3D12_BLEND_INV_DEST_COLOR: return 7;
    case D3D12_BLEND_SRC_ALPHA_SAT: return 10;
    case D3D12_BLEND_INV_BLEND_FACTOR: return 0;
    default: return 1;
  }
}
static UINT blend_op(UINT o) { return o >= 1 && o <= 5 ? o - 1 : 0; }
static UINT cmp_func(UINT f) { return f >= 1 && f <= 8 ? f - 1 : 7; }
static UINT stencil_op(UINT o) { return o >= 1 && o <= 8 ? o - 1 : 0; }

/* The kernel pipeline of the state for these draws, made when first needed. Returns 0 when the kernel refuses. */
static UINT pso_pipeline(Pso *p, const UINT strides[8], UINT stencil_ref) {
  if (p->compute) return p->compute_pipe;
  for (UINT i = 0; i < p->pipe_count; ++i)
    if (p->pipes[i].stencil_ref == stencil_ref && memcmp(p->pipes[i].strides, strides, 8 * sizeof(UINT)) == 0) return p->pipes[i].handle;
  const UINT stride = strides[0];
  UINT r[320];
  UINT w = 0;
  const D3D12_RENDER_TARGET_BLEND_DESC *rt = &p->blend.RenderTarget[0];
  r[w++] = ESC_CREATE_PIPELINE;
  r[w++] = p->shaders[0];
  r[w++] = p->shaders[1];
  r[w++] = stride;
  r[w++] = p->attr_count;
  r[w++] = p->varyings;
  r[w++] = p->textures;
  r[w++] = p->raster.CullMode == D3D12_CULL_MODE_NONE ? 0 : p->raster.CullMode == D3D12_CULL_MODE_BACK ? 1 : 2;
  r[w++] = p->raster.FrontCounterClockwise ? 0 : 1;
  r[w++] = (p->depth.DepthEnable ? 1u : 0u) | (p->depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 2u : 0u) | (rt->BlendEnable ? 4u : 0u);
  for (UINT i = 0; i < 4; ++i) {
    r[w++] = i < p->attr_count ? p->attrs[i].location : 0;
    r[w++] = i < p->attr_count ? p->attrs[i].floats - 1 : 0;
    r[w++] = i < p->attr_count ? p->attrs[i].offset : 0;
  }
  r[w++] = p->depth.StencilEnable ? 1 : 0;
  r[w++] = cmp_func(p->depth.FrontFace.StencilFunc);
  r[w++] = stencil_op(p->depth.FrontFace.StencilFailOp);
  r[w++] = stencil_op(p->depth.FrontFace.StencilDepthFailOp);
  r[w++] = stencil_op(p->depth.FrontFace.StencilPassOp);
  r[w++] = p->depth.StencilReadMask;
  r[w++] = p->depth.StencilWriteMask;
  r[w++] = stencil_ref & 0xFF;
  r[w++] = p->depth.DepthEnable ? cmp_func(p->depth.DepthFunc) : 7;
  r[w++] = rt->BlendEnable ? 1 : 0;      /* custom */
  r[w++] = rt->BlendEnable ? 1 : 0;
  r[w++] = blend_factor(rt->SrcBlend);
  r[w++] = blend_factor(rt->DestBlend);
  r[w++] = blend_factor(rt->SrcBlendAlpha);
  r[w++] = blend_factor(rt->DestBlendAlpha);
  r[w++] = blend_op(rt->BlendOp);
  r[w++] = blend_op(rt->BlendOpAlpha);
  r[w++] = rt->RenderTargetWriteMask & 0xF;
  UINT instance_bits = 0;
  for (UINT i = 0; i < p->attr_count && i < 4; ++i)
    if (p->attrs[i].per_instance) instance_bits |= 1u << i;
  r[w++] = instance_bits;
  r[w++] = p->render_targets ? p->render_targets : 1;
  r[w++] = p->blend.IndependentBlendEnable ? 1 : 0;
  for (UINT t = 1; t < 4; ++t) {
    const D3D12_RENDER_TARGET_BLEND_DESC *b = &p->blend.RenderTarget[p->blend.IndependentBlendEnable ? t : 0];
    r[w++] = b->BlendEnable ? 1 : 0;
    r[w++] = b->BlendEnable ? 1 : 0;
    r[w++] = blend_factor(b->SrcBlend);
    r[w++] = blend_factor(b->DestBlend);
    r[w++] = blend_factor(b->SrcBlendAlpha);
    r[w++] = blend_factor(b->DestBlendAlpha);
    r[w++] = blend_op(b->BlendOp);
    r[w++] = blend_op(b->BlendOpAlpha);
    r[w++] = b->RenderTargetWriteMask & 0xF;
  }
  r[w++] = 0;      /* multisample */
  r[w++] = p->shaders[2];
  r[w++] = p->shaders[3];
  r[w++] = p->shaders[4];
  if (p->so_entries) {
    r[w++] = p->so_entries;
    r[w++] = p->so_rasterized;
    for (UINT i = 0; i < 4; ++i) r[w++] = p->so_strides[i];
    for (UINT i = 0; i < p->so_entries; ++i) { r[w++] = p->so_entry[i][0]; r[w++] = p->so_entry[i][1]; }
  }
  /* beyond four attributes, a vertex buffer other than the first, or colour targets 4 .. 7 with their own blending: the last part of the description */
  UINT max_slot = 0;
  for (UINT i = 0; i < p->attr_count; ++i)
    if (p->attrs[i].slot > max_slot) max_slot = p->attrs[i].slot;
  const UINT more_blends = p->blend.IndependentBlendEnable && p->render_targets > 4 ? p->render_targets - 4 : 0;
  if (p->attr_count > 4 || max_slot > 0 || more_blends) {
    if (!p->so_entries) r[w++] = 0;
    const UINT extra = p->attr_count > 4 ? p->attr_count - 4 : 0;
    r[w++] = extra;
    UINT instance_high = 0;
    for (UINT k = 0; k < extra; ++k) {
      const Attr *a = &p->attrs[4 + k];
      r[w++] = a->location;
      r[w++] = (a->floats - 1) | (a->slot << 8);
      r[w++] = a->offset;
      if (a->per_instance) instance_high |= 1u << k;
    }
    r[w++] = instance_high;
    UINT slots_low = 0;
    for (UINT i = 0; i < p->attr_count && i < 4; ++i) slots_low |= p->attrs[i].slot << (4 * i);
    r[w++] = slots_low;
    r[w++] = max_slot;      /* the strides of slots 1 .. max_slot */
    for (UINT sl = 1; sl <= max_slot; ++sl) r[w++] = strides[sl];
    r[w++] = more_blends;
    for (UINT k = 0; k < more_blends; ++k) {
      const D3D12_RENDER_TARGET_BLEND_DESC *b = &p->blend.RenderTarget[4 + k];
      r[w++] = b->BlendEnable ? 1 : 0;
      r[w++] = b->BlendEnable ? 1 : 0;
      r[w++] = blend_factor(b->SrcBlend);
      r[w++] = blend_factor(b->DestBlend);
      r[w++] = blend_factor(b->SrcBlendAlpha);
      r[w++] = blend_factor(b->DestBlendAlpha);
      r[w++] = blend_op(b->BlendOp);
      r[w++] = blend_op(b->BlendOpAlpha);
      r[w++] = b->RenderTargetWriteMask & 0xF;
    }
  }
  UINT reply[1] = {0};
  if (!KMT_OK(dev_escape(p->d.dev, r, w, reply, 1))) return 0;
  if (p->pipe_count == MAX_PSO_PIPELINES) {
    UINT drop[2] = {ESC_DESTROY_PIPELINE, p->pipes[0].handle};
    dev_escape(p->d.dev, drop, 2, NULL, 0);
    memmove(&p->pipes[0], &p->pipes[1], (MAX_PSO_PIPELINES - 1) * sizeof p->pipes[0]);
    p->pipe_count--;
  }
  memcpy(p->pipes[p->pipe_count].strides, strides, 8 * sizeof(UINT));
  p->pipes[p->pipe_count].stencil_ref = stencil_ref;
  p->pipes[p->pipe_count].handle = reply[0];
  p->pipe_count++;
  return reply[0];
}

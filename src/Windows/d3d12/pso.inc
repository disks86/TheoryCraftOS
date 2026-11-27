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

typedef struct SigElement { char name[32]; UINT index, sysval, reg, read_mask; } SigElement;      /* (read_mask: the components the shader reads, of an input) */

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
    el->read_mask = e[21];
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
  UINT pdescriptors[16];   /* the descriptors a table parameter reaches from its start (0xFFFFFFFF: to the end of the heap) */
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
  if (version < 1 || version > 3) return;      /* (1.0, 1.1, 1.2: 1.2 differs from 1.1 in its static samplers alone) */
  r->parameter_count = count;
  for (UINT i = 0; i < count && i < 16; ++i) {
    if (params + (SIZE_T)i * 12 + 12 > n) return;
    const UINT type = rd32(b + params + i * 12), payload = rd32(b + params + i * 12 + 8);
    r->ptype[i] = type;
    if (type == 1 && payload + 12 <= n) r->pconstants[i] = rd32(b + payload + 8);
    if (type != 0 || payload + 8 > n) continue;
    const UINT range_count = rd32(b + payload), ranges = rd32(b + payload + 4), stride = version >= 2 ? 24 : 20;
    UINT next = 0;      /* where a range that is appended begins */
    for (UINT k = 0; k < range_count && k < 8; ++k) {
      if (ranges + (SIZE_T)k * stride + stride > n) return;
      const BYTE *range = b + ranges + k * stride;
      const UINT num = rd32(range + 4), base = rd32(range + 8), offset = rd32(range + stride - 4);
      const UINT start = offset == 0xFFFFFFFFu ? next : offset;
      if (num == 0xFFFFFFFFu || start > 0xFFFFFFFFu - num) r->pdescriptors[i] = 0xFFFFFFFFu;
      else {
        next = start + num;
        if (r->pdescriptors[i] != 0xFFFFFFFFu && next > r->pdescriptors[i]) r->pdescriptors[i] = next;
      }
      if (rd32(range) != 0) continue;      /* SRV */
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

/* (the serializer, D3D12SerializeRootSignature and D3D12SerializeVersionedRootSignature, is in rootsig.inc) */

/* ---- pipeline states ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define MAX_PSO_PIPELINES 8
/* One kernel pipeline of a PSO: what it was made for, and how many unreset command lists bound it (it is destroyed only when none does). */
typedef struct PsoPipe { UINT strides[8], stencil_ref, stencil_back, handle; float blend_factor[4], depth_bias[3]; UINT users; } PsoPipe;

typedef struct Attr { UINT location, format, offset, per_instance, slot, step_rate; } Attr;      /* (format: the kernel's graphics::VertexFormat; step_rate: InstanceDataStepRate) */

struct Pso {
  DObj d;
  int compute;
  RootSig *rs;
  UINT shaders[6];             /* vertex, pixel, geometry, hull, domain, compute (the kernel's handles; 0: none) */
  UINT attr_count;
  Attr attrs[16];
  UINT varyings, textures;
  D3D12_BLEND_DESC blend;
  int blend_constant;          /* a target blends with the blend factor (OMSetBlendFactor): the kernel pipelines are made for each factor too */
  D3D12_RASTERIZER_DESC raster;
  D3D12_DEPTH_STENCIL_DESC depth;
  UINT8 back_read_mask, back_write_mask;      /* the back faces' stencil masks (D3D12_DEPTH_STENCIL_DESC2's own; else the front's) */
  FLOAT depth_bias;            /* the constant depth bias: RASTERIZER1/2's float, else D3D12_RASTERIZER_DESC::DepthBias */
  int dynamic_bias;            /* D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS: the bias is the command list's (RSSetDepthBias), the kernel pipelines made for each value */
  UINT line_mode;              /* how lines are drawn: D3D12_LINE_RASTERIZATION_MODE (aliased, alpha-antialiased, quadrilateral 1.4 and 1.0 wide), the kernel's graphics::LineMode */
  UINT strip_cut;              /* IBStripCutValue (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE: the kernel's StripCut), what a list setting the pipeline draws strips with */
  UINT render_targets;
  DXGI_FORMAT rtv_formats[8];
  UINT slots_used;             /* the vertex buffers (input slots) the layout reads, as a mask */
  DXGI_FORMAT dsv_format;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  int no_pixel_shader;          /* the description has none: the kernel's pipeline gets the null one, flagged so that it is not counted as PSInvocations */
  UINT so_entries, so_rasterized, so_strides[4];
  UINT samples;                /* of the targets it draws into (1: not multisampled) */
  UINT forced_samples;         /* the kernel's forced sample count (the part 'FSC1', DEV_FEATURE3_FORCED_SAMPLE_COUNT): ForcedSampleCount; 0 not forced */
  UINT so_entry[16][2];        /* the words of the kernel's description of each captured output */
  PsoPipe pipes[MAX_PSO_PIPELINES];      /* the pipelines a draw may find (the latest ones) */
  UINT pipe_count;
  PsoPipe *retired;            /* pushed out of `pipes` while lists still bind them: destroyed when the last of those lists is reset */
  UINT nretired, cap_retired;
  SRWLOCK pipe_lock;           /* pipes and retired: lists recorded on several threads share the PSO */
  UINT compute_pipe;
  UINT64 hash;                 /* what its description was (desc_hash_*): what a pipeline library stores it by, and compares a description loaded under its name with */
  int depth_bounds;            /* DepthBoundsTestEnable (DEPTH_STENCIL1/2): the draws test the depth buffer against OMSetDepthBounds' range */
  int views_on;                /* view instancing (VIEW_INSTANCING of more than one view, or of one moved or masked): the views each draw makes and D3D12_VIEW_INSTANCING_FLAGS;
                                * the viewport and layer of each */
  UINT view_count, view_flags;
  D3D12_VIEW_INSTANCE_LOCATION view_loc[4];
};

/* ---- the fingerprint of a pipeline description ----------------------------------------------------------------------------------------------------------------------- */
/* FNV-1a over everything a description says - the shaders' bytes, the input layout's elements and names, the stream output's entries, the fixed state - except what
 * names objects of a run (the root signature is compared by the bytes it was made from) and the cached blob. */
static UINT64 pfnv(UINT64 h, const void *data, SIZE_T n) {
  const BYTE *p = data;
  for (SIZE_T i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
static UINT64 fnv_str(UINT64 h, const char *s) { if (!s) return pfnv(h, "", 1); SIZE_T n = 0; while (s[n]) ++n; return pfnv(h, s, n + 1); }
static UINT64 fnv_shader(UINT64 h, const D3D12_SHADER_BYTECODE *b) {
  const UINT64 n = b->pShaderBytecode ? b->BytecodeLength : 0;
  h = pfnv(h, &n, 8);
  return n ? pfnv(h, b->pShaderBytecode, (SIZE_T)n) : h;
}
static UINT64 fnv_root(UINT64 h, ID3D12RootSignature *rs) {
  const RootSig *r = (const RootSig *)rs;
  return r && r->blob ? pfnv(h, r->blob, r->blob_size) : pfnv(h, "-", 1);
}
static UINT64 desc_hash_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d) {
  UINT64 h = 14695981039346656037ull;
  h = pfnv(h, "G", 1);
  h = fnv_root(h, d->pRootSignature);
  const D3D12_SHADER_BYTECODE *stages[5] = {&d->VS, &d->PS, &d->DS, &d->HS, &d->GS};
  for (UINT i = 0; i < 5; ++i) h = fnv_shader(h, stages[i]);
  h = pfnv(h, &d->StreamOutput.NumEntries, 4);
  for (UINT i = 0; d->StreamOutput.pSODeclaration && i < d->StreamOutput.NumEntries; ++i) {
    const D3D12_SO_DECLARATION_ENTRY *e = &d->StreamOutput.pSODeclaration[i];
    h = fnv_str(h, e->SemanticName);
    h = pfnv(h, &e->Stream, 4); h = pfnv(h, &e->SemanticIndex, 4); h = pfnv(h, &e->StartComponent, 1); h = pfnv(h, &e->ComponentCount, 1); h = pfnv(h, &e->OutputSlot, 1);
  }
  for (UINT i = 0; d->StreamOutput.pBufferStrides && i < d->StreamOutput.NumStrides; ++i) h = pfnv(h, &d->StreamOutput.pBufferStrides[i], 4);
  h = pfnv(h, &d->StreamOutput.RasterizedStream, 4);
  /* (field by field: the structures' padding is whatever the program's memory held) */
  h = pfnv(h, &d->BlendState.AlphaToCoverageEnable, 4);
  h = pfnv(h, &d->BlendState.IndependentBlendEnable, 4);
  for (UINT t = 0; t < 8; ++t) {
    const D3D12_RENDER_TARGET_BLEND_DESC *b = &d->BlendState.RenderTarget[t];
    const UINT words[9] = {(UINT)b->BlendEnable, (UINT)b->LogicOpEnable, b->SrcBlend, b->DestBlend, b->BlendOp, b->SrcBlendAlpha, b->DestBlendAlpha, b->BlendOpAlpha, b->LogicOp};
    h = pfnv(h, words, sizeof words);
    h = pfnv(h, &b->RenderTargetWriteMask, 1);
  }
  h = pfnv(h, &d->SampleMask, 4);
  h = pfnv(h, &d->RasterizerState, sizeof d->RasterizerState);      /* (no padding: eleven 4-byte fields) */
  {
    const D3D12_DEPTH_STENCIL_DESC *z = &d->DepthStencilState;
    const UINT words[4] = {(UINT)z->DepthEnable, z->DepthWriteMask, z->DepthFunc, (UINT)z->StencilEnable};
    h = pfnv(h, words, sizeof words);
    h = pfnv(h, &z->StencilReadMask, 1);
    h = pfnv(h, &z->StencilWriteMask, 1);
    const D3D12_DEPTH_STENCILOP_DESC *faces[2] = {&z->FrontFace, &z->BackFace};
    for (UINT f = 0; f < 2; ++f) h = pfnv(h, faces[f], sizeof *faces[f]);      /* (four 4-byte enums) */
  }
  h = pfnv(h, &d->InputLayout.NumElements, 4);
  for (UINT i = 0; d->InputLayout.pInputElementDescs && i < d->InputLayout.NumElements; ++i) {
    const D3D12_INPUT_ELEMENT_DESC *e = &d->InputLayout.pInputElementDescs[i];
    h = fnv_str(h, e->SemanticName);
    h = pfnv(h, &e->SemanticIndex, 4); h = pfnv(h, &e->Format, 4); h = pfnv(h, &e->InputSlot, 4); h = pfnv(h, &e->AlignedByteOffset, 4);
    h = pfnv(h, &e->InputSlotClass, 4); h = pfnv(h, &e->InstanceDataStepRate, 4);
  }
  h = pfnv(h, &d->IBStripCutValue, 4);
  h = pfnv(h, &d->PrimitiveTopologyType, 4);
  h = pfnv(h, &d->NumRenderTargets, 4);
  h = pfnv(h, d->RTVFormats, sizeof d->RTVFormats);
  h = pfnv(h, &d->DSVFormat, 4);
  h = pfnv(h, &d->SampleDesc, sizeof d->SampleDesc);
  h = pfnv(h, &d->NodeMask, 4);
  h = pfnv(h, &d->Flags, 4);
  return h;
}
static UINT64 desc_hash_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *d) {
  UINT64 h = 14695981039346656037ull;
  h = pfnv(h, "C", 1);
  h = fnv_root(h, d->pRootSignature);
  h = fnv_shader(h, &d->CS);
  h = pfnv(h, &d->NodeMask, 4);
  h = pfnv(h, &d->Flags, 4);
  return h;
}

static const GUID *const g_pso_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12PipelineState, NULL};

static void release_shader(Dev *dev, UINT handle);
static void pso_destroy(DxObj *o) {
  Pso *p = (Pso *)o;
  for (UINT i = 0; i < p->pipe_count; ++i) {
    UINT request[2] = {ESC_DESTROY_PIPELINE, p->pipes[i].handle};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
  for (UINT i = 0; i < p->nretired; ++i) {      /* (none has users left: a list that bound one holds the PSO) */
    UINT request[2] = {ESC_DESTROY_PIPELINE, p->retired[i].handle};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
  mem_free(p->retired);
  if (p->compute_pipe) {
    UINT request[2] = {ESC_DESTROY_PIPELINE, p->compute_pipe};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
  for (UINT i = 0; i < 6; ++i)
    if (p->shaders[i]) release_shader(p->d.dev, p->shaders[i]);
  if (p->rs) dx_Release(OBJ(p->rs));
  dobj_child_destroy(o);
}

/* The kernel's shaders, each once: pipelines made of the same bytecode - a runtime above makes one for every combination of render states - share
 * the shader, of which the kernel has only so many at a time where it runs them itself. */
typedef struct SharedShader {
  UINT handle, references;
  UINT kind, stage, bytes;       /* what the kernel was asked for: the kind word, the stage, and the bytecode's length ... */
  UINT64 hash;                   /* ... and a hash of it */
  BYTE *code;
} SharedShader;
static SRWLOCK g_shared_lock = SRWLOCK_INIT;
static UINT64 shader_hash(const BYTE *p, SIZE_T n) {
  UINT64 h = 0xCBF29CE484222325ull;
  for (SIZE_T i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
/* Gives back a shader make_shader gave: the kernel's goes when no pipeline has it any more. */
static void release_shader(Dev *dev, UINT handle) {
  BOOL destroy = TRUE;
  AcquireSRWLockExclusive(&g_shared_lock);
  for (UINT i = 0; i < dev->shared_count; ++i) {
    SharedShader *s = &dev->shared[i];
    if (s->handle != handle) continue;
    destroy = --s->references == 0;
    if (destroy) {
      mem_free(s->code);
      *s = dev->shared[--dev->shared_count];
    }
    break;
  }
  ReleaseSRWLockExclusive(&g_shared_lock);
  if (destroy) {
    UINT request[2] = {ESC_DESTROY_SHADER, handle};
    dev_escape(dev, request, 2, NULL, 0);
  }
}
static HRESULT make_shader(Dev *dev, const D3D12_SHADER_BYTECODE *code, UINT stage, UINT *handle) {
  *handle = 0;
  if (!code->pShaderBytecode || !code->BytecodeLength) return S_OK;
  if (code->BytecodeLength > TCOS_MAX_SHADER_BYTES) return DXGI_ERROR_UNSUPPORTED;
  /* ShaderKind::Dxbc (DXBC and the DXIL of Shader Model 6), or what the runtime above asked for (TcosD3D12PipelineOptions); above the kind, where a
   * pixel shader's constant buffers begin */
  UINT base = stage == 1 ? dev->next_pixel_base : stage >= 3 && stage <= 5 ? dev->next_stage_base[stage - 3] : 0;
  const UINT kind = (dev->next_shader_kind ? dev->next_shader_kind : 7) | (!dev->next_shader_kind ? base << 8 : 0);
  const UINT64 hash = shader_hash(code->pShaderBytecode, code->BytecodeLength);
  AcquireSRWLockExclusive(&g_shared_lock);
  for (UINT i = 0; i < dev->shared_count; ++i) {
    SharedShader *s = &dev->shared[i];
    if (s->hash != hash || s->kind != kind || s->stage != stage || s->bytes != code->BytecodeLength || memcmp(s->code, code->pShaderBytecode, s->bytes) != 0) continue;
    ++s->references;
    *handle = s->handle;
    break;
  }
  ReleaseSRWLockExclusive(&g_shared_lock);
  if (*handle) return S_OK;
  const UINT words = 4 + (UINT)((code->BytecodeLength + 3) / 4);
  UINT *request = mem_alloc((SIZE_T)(words + 2) * 4);
  UINT reply[1] = {0};
  if (!request) return E_OUTOFMEMORY;
  request[0] = ESC_CREATE_SHADER;
  request[1] = kind;
  request[2] = stage;
  request[3] = (UINT)code->BytecodeLength;
  memcpy(request + 4, code->pShaderBytecode, code->BytecodeLength);
  const NTSTATUS st = dev_escape(dev, request, words, reply, 1);
  mem_free(request);
  if (!KMT_OK(st)) return hr_from(st) == E_FAIL ? E_INVALIDARG : hr_from(st);
  *handle = reply[0];
  /* (kept for the pipelines after this one; a shader that cannot be kept is this pipeline's alone, which release_shader gives back all the same) */
  BYTE *copy = mem_alloc(code->BytecodeLength);
  AcquireSRWLockExclusive(&g_shared_lock);
  if (copy && dev->shared_count == dev->shared_capacity) {
    const UINT capacity = dev->shared_capacity ? dev->shared_capacity * 2 : 16;
    SharedShader *grown = mem_grow(dev->shared, (SIZE_T)capacity * sizeof *grown);
    if (grown) {
      dev->shared = grown;
      dev->shared_capacity = capacity;
    }
  }
  if (copy && dev->shared_count < dev->shared_capacity) {
    SharedShader *s = &dev->shared[dev->shared_count++];
    memcpy(copy, code->pShaderBytecode, code->BytecodeLength);
    s->handle = reply[0];
    s->references = 1;
    s->kind = kind;
    s->stage = stage;
    s->bytes = (UINT)code->BytecodeLength;
    s->hash = hash;
    s->code = copy;
    copy = NULL;
  }
  ReleaseSRWLockExclusive(&g_shared_lock);
  mem_free(copy);
  return S_OK;
}

/* A vertex element's format as the kernel's graphics::VertexFormat (type * 4 + components - 1 with the types float, unorm8, snorm8, uint8, sint8, unorm16, snorm16, uint16,
 * sint16, half, uint32, sint32; 48 is B8G8R8A8_UNORM, 49 R10G10B10A2_UNORM, 50 R10G10B10A2_UINT, 51 R11G11B10_FLOAT and 52 B8G8R8X8_UNORM) and the bytes it takes in
 * the buffer; -1 for a format a vertex cannot have (Direct3D's IA_VERTEX_BUFFER formats are all here; B5G6R5, B5G5R5A1 and B4G4R4A4 are not among them). Formats other
 * than 32-bit floats need a device that reads them (DEV_FEATURE_VERTEX_FORMATS; the kernel refuses the pipeline on one that does not). */
static int vertex_format(DXGI_FORMAT f, UINT *bytes);
/* (the bytes of a vertex of the format for the input layout's checks: 0 for a format no vertex has) */
static uint32_t layout_element_bytes(uint32_t format) {
  UINT bytes = 0;
  return vertex_format((DXGI_FORMAT)format, &bytes) >= 0 ? bytes : 0;
}
static int vertex_format(DXGI_FORMAT f, UINT *bytes) {
#define VF(type, components, size) (*bytes = (size) * (components), (type) * 4 + (components) - 1)
  switch (f) {
    case DXGI_FORMAT_R32_FLOAT: return VF(0, 1, 4);
    case DXGI_FORMAT_R32G32_FLOAT: return VF(0, 2, 4);
    case DXGI_FORMAT_R32G32B32_FLOAT: return VF(0, 3, 4);
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VF(0, 4, 4);
    case DXGI_FORMAT_R8_UNORM: return VF(1, 1, 1);
    case DXGI_FORMAT_R8G8_UNORM: return VF(1, 2, 1);
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VF(1, 4, 1);
    case DXGI_FORMAT_R8_SNORM: return VF(2, 1, 1);
    case DXGI_FORMAT_R8G8_SNORM: return VF(2, 2, 1);
    case DXGI_FORMAT_R8G8B8A8_SNORM: return VF(2, 4, 1);
    case DXGI_FORMAT_R8_UINT: return VF(3, 1, 1);
    case DXGI_FORMAT_R8G8_UINT: return VF(3, 2, 1);
    case DXGI_FORMAT_R8G8B8A8_UINT: return VF(3, 4, 1);
    case DXGI_FORMAT_R8_SINT: return VF(4, 1, 1);
    case DXGI_FORMAT_R8G8_SINT: return VF(4, 2, 1);
    case DXGI_FORMAT_R8G8B8A8_SINT: return VF(4, 4, 1);
    case DXGI_FORMAT_R16_UNORM: return VF(5, 1, 2);
    case DXGI_FORMAT_R16G16_UNORM: return VF(5, 2, 2);
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VF(5, 4, 2);
    case DXGI_FORMAT_R16_SNORM: return VF(6, 1, 2);
    case DXGI_FORMAT_R16G16_SNORM: return VF(6, 2, 2);
    case DXGI_FORMAT_R16G16B16A16_SNORM: return VF(6, 4, 2);
    case DXGI_FORMAT_R16_UINT: return VF(7, 1, 2);
    case DXGI_FORMAT_R16G16_UINT: return VF(7, 2, 2);
    case DXGI_FORMAT_R16G16B16A16_UINT: return VF(7, 4, 2);
    case DXGI_FORMAT_R16_SINT: return VF(8, 1, 2);
    case DXGI_FORMAT_R16G16_SINT: return VF(8, 2, 2);
    case DXGI_FORMAT_R16G16B16A16_SINT: return VF(8, 4, 2);
    case DXGI_FORMAT_R16_FLOAT: return VF(9, 1, 2);
    case DXGI_FORMAT_R16G16_FLOAT: return VF(9, 2, 2);
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VF(9, 4, 2);
    case DXGI_FORMAT_R32_UINT: return VF(10, 1, 4);
    case DXGI_FORMAT_R32G32_UINT: return VF(10, 2, 4);
    case DXGI_FORMAT_R32G32B32_UINT: return VF(10, 3, 4);
    case DXGI_FORMAT_R32G32B32A32_UINT: return VF(10, 4, 4);
    case DXGI_FORMAT_R32_SINT: return VF(11, 1, 4);
    case DXGI_FORMAT_R32G32_SINT: return VF(11, 2, 4);
    case DXGI_FORMAT_R32G32B32_SINT: return VF(11, 3, 4);
    case DXGI_FORMAT_R32G32B32A32_SINT: return VF(11, 4, 4);
    case DXGI_FORMAT_B8G8R8A8_UNORM: *bytes = 4; return 48;
    case DXGI_FORMAT_R10G10B10A2_UNORM: *bytes = 4; return 49;
    case DXGI_FORMAT_R10G10B10A2_UINT: *bytes = 4; return 50;
    case DXGI_FORMAT_R11G11B10_FLOAT: *bytes = 4; return 51;
    case DXGI_FORMAT_B8G8R8X8_UNORM: *bytes = 4; return 52;
    default: *bytes = 0; return -1;
  }
#undef VF
}

static HRESULT blend_state_check(const Dev *dev, const D3D12_BLEND_DESC *b, UINT targets, int *constant);
/* What a pipeline stream (device1.inc stream_parse) says beyond D3D12_GRAPHICS_PIPELINE_STATE_DESC: RASTERIZER1/2's float depth bias and DEPTH_STENCIL2's back faces' masks. */
typedef struct PsoExtras {
  int has_bias;
  FLOAT depth_bias;
  int has_line_mode;           /* RASTERIZER2's LineRasterizationMode (D3D12_LINE_RASTERIZATION_MODE: also narrow quadrilateral lines, which the desc's two flags cannot say) */
  UINT line_mode;
  int has_back_masks;
  UINT8 back_read_mask, back_write_mask;
  int depth_bounds;            /* DEPTH_STENCIL1/2's DepthBoundsTestEnable */
  UINT view_count, view_flags; /* VIEW_INSTANCING: the views (0: none) and D3D12_VIEW_INSTANCING_FLAGS (what of them is view instancing: Pso::views_on) */
  D3D12_VIEW_INSTANCE_LOCATION views[4];
} PsoExtras;
/* (the fingerprint of a description with them) */
static UINT64 desc_hash_graphics_x(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, const PsoExtras *x) {
  UINT64 h = desc_hash_graphics(d);
  if (x && x->has_bias) h = pfnv(pfnv(h, "B", 1), &x->depth_bias, 4);
  if (x && x->depth_bounds) h = pfnv(h, "Z", 1);
  if (x && x->view_count >= 1) {
    h = pfnv(pfnv(pfnv(h, "V", 1), &x->view_count, 4), &x->view_flags, 4);
    h = pfnv(h, x->views, x->view_count * sizeof x->views[0]);
  }
  if (x && x->has_line_mode) h = pfnv(pfnv(h, "L", 1), &x->line_mode, 4);
  if (x && x->has_back_masks) {
    const UINT8 masks[2] = {x->back_read_mask, x->back_write_mask};
    h = pfnv(pfnv(h, "M", 1), masks, 2);
  }
  return h;
}
static HRESULT raster_state_check(const Dev *dev, const D3D12_RASTERIZER_DESC *r, FLOAT depth_bias, const D3D12_DEPTH_STENCIL_DESC *z, UINT8 back_read_mask, UINT8 back_write_mask,
                                  int dynamic_bias);
static HRESULT pso_create_graphics(Dev *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, const PsoExtras *x, REFIID riid, void **out);
static HRESULT STDMETHODCALLTYPE dev_CreateGraphicsPipelineState(Dev *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
  return pso_create_graphics(This, desc, NULL, riid, out);
}
static HRESULT pso_create_graphics(Dev *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, const PsoExtras *x, REFIID riid, void **out) {
  extern const ID3D12PipelineStateVtbl pso_vtbl;
  if (!desc || !desc->pRootSignature || !desc->VS.pShaderBytecode) return E_INVALIDARG;
  /* (a strip cut set by the command list - DynamicIndexBufferStripCutSupported, OPTIONS15: every device, the kernel cutting the strips it expands -; a depth bias set by it -
   * DynamicDepthBiasSupported, OPTIONS16 - where the device has the rasterizer state; IBStripCutValue one of the enumeration's) */
  if ((UINT)desc->IBStripCutValue > (UINT)D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) return E_INVALIDARG;
  if ((desc->Flags & D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS) && !(This->caps.features2 & DEV_FEATURE2_RASTERIZER_STATE)) return E_INVALIDARG;
  if (desc->SampleDesc.Count > 1 && (desc->SampleDesc.Count >= 32 || !((This->caps.sample_counts >> desc->SampleDesc.Count) & 1))) return DXGI_ERROR_UNSUPPORTED;
  if (desc->StreamOutput.NumEntries > 16 || desc->StreamOutput.NumStrides > 4) return DXGI_ERROR_UNSUPPORTED;
  Pso *p = dobj_new(sizeof *p, This, &pso_vtbl, g_pso_iids, pso_destroy);
  if (!p) return E_OUTOFMEMORY;
  p->hash = desc_hash_graphics_x(desc, x);
  p->rs = (RootSig *)desc->pRootSignature;
  dx_AddRef(OBJ(p->rs));
  p->blend = desc->BlendState;
  {
    const HRESULT bh = blend_state_check(This, &desc->BlendState, desc->NumRenderTargets, &p->blend_constant);
    if (FAILED(bh)) { dx_Release(OBJ(p)); return bh; }
  }
  /* (conservative rasterization: on a device with the kernel's kFeature2ConservativeRaster - the CPU renderer -, tier 1; elsewhere the tier is NOT_SUPPORTED and asking for it is
   * an invalid description, as on Windows) */
  if ((UINT)desc->RasterizerState.ConservativeRaster > (UINT)D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON ||
      (desc->RasterizerState.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON && !(This->caps.features2 & DEV_FEATURE2_CONSERVATIVE_RASTER))) {
    dx_Release(OBJ(p));
    return E_INVALIDARG;
  }
  p->raster = desc->RasterizerState;
  p->depth = desc->DepthStencilState;
  p->back_read_mask = x && x->has_back_masks ? x->back_read_mask : desc->DepthStencilState.StencilReadMask;
  p->back_write_mask = x && x->has_back_masks ? x->back_write_mask : desc->DepthStencilState.StencilWriteMask;
  p->depth_bias = x && x->has_bias ? x->depth_bias : (FLOAT)desc->RasterizerState.DepthBias;
  /* (the depth bounds test and the views: what the command list sends with each draw - the kernel's SetDepthBounds and SetViewInstance -, not the kernel pipeline's) */
  p->depth_bounds = x && x->depth_bounds;
  /* (one view is view instancing too where its location moves it or the mask may turn it off - on a device that has view instancing; elsewhere it draws as it is) */
  const int one_moved = x && x->view_count == 1 && (x->views[0].ViewportArrayIndex || x->views[0].RenderTargetArrayIndex || x->view_flags);
  p->views_on = x && (x->view_count > 1 || one_moved) && (This->caps.features2 & DEV_FEATURE2_VIEW_INSTANCING);
  p->view_count = p->views_on ? x->view_count : 1;
  p->view_flags = p->views_on ? x->view_flags : 0;
  if (p->views_on) memcpy(p->view_loc, x->views, x->view_count * sizeof x->views[0]);
  p->dynamic_bias = (desc->Flags & D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS) != 0;
  p->strip_cut = (UINT)desc->IBStripCutValue;
  /* (the line mode: RASTERIZER2's, or MultisampleEnable - quadrilateral lines, 1.4 pixels wide - before AntialiasedLineEnable - alpha-antialiased lines -, as Direct3D 10 to 12 choose.
   * On a device without the kernel's line modes lines are aliased whatever the state says - its rasterizer has no other kind -; narrow quadrilateral lines need the line modes
   * (NarrowQuadrilateralLinesSupported, OPTIONS19)) */
  p->line_mode = x && x->has_line_mode ? x->line_mode
                 : desc->RasterizerState.MultisampleEnable ? (UINT)D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE
                 : desc->RasterizerState.AntialiasedLineEnable ? (UINT)D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED : (UINT)D3D12_LINE_RASTERIZATION_MODE_ALIASED;
  if (p->line_mode > (UINT)D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW ||
      (p->line_mode == (UINT)D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW && !(This->caps.features2 & DEV_FEATURE2_LINE_MODES))) {
    dx_Release(OBJ(p));
    return E_INVALIDARG;
  }
  if (!(This->caps.features2 & DEV_FEATURE2_LINE_MODES)) p->line_mode = 0;
  {
    const HRESULT rh = raster_state_check(This, &desc->RasterizerState, p->depth_bias, &desc->DepthStencilState, p->back_read_mask, p->back_write_mask, p->dynamic_bias);
    if (FAILED(rh)) { dx_Release(OBJ(p)); return rh; }
  }
  p->render_targets = desc->NumRenderTargets;
  p->samples = desc->SampleDesc.Count > 1 ? desc->SampleDesc.Count : 1;
  {
    /* ForcedSampleCount: 0 is not forced. On a device with the kernel's forced sample count (DEV_FEATURE3_FORCED_SAMPLE_COUNT: target-independent rasterization) 1, 2, 4, 8 or 16 is
     * the samples triangles are rasterized with: above 1 a pixel of single-sample targets (or of none - UAV-only rendering) is shaded once when the triangle covers any of them, which
     * goes with a sample count of 1 and no depth or stencil test; 1 on multisampled targets is the pixel centre's coverage for all its samples (MultisampleRTVWithForcedSampleCountOne).
     * Elsewhere 1 is not forced, and 4, 8 or 16 goes only with no render target and no depth buffer (UAV-only rendering through a target of that many samples the device must have) */
    const UINT forced = desc->RasterizerState.ForcedSampleCount;
    if (This->caps.features3 & DEV_FEATURE3_FORCED_SAMPLE_COUNT) {
      if (forced != 0 && forced != 1 && forced != 2 && forced != 4 && forced != 8 && forced != 16) {
        dx_Release(OBJ(p));
        return E_INVALIDARG;
      }
      if (forced > 1 && (desc->SampleDesc.Count > 1 || desc->DepthStencilState.DepthEnable || desc->DepthStencilState.StencilEnable)) {
        dx_Release(OBJ(p));
        return E_INVALIDARG;
      }
      p->forced_samples = forced;
    } else if (forced > 1) {
      if ((forced != 4 && forced != 8 && forced != 16) || desc->NumRenderTargets || desc->DSVFormat != DXGI_FORMAT_UNKNOWN || desc->SampleDesc.Count > 1 ||
          !((This->caps.sample_counts >> forced) & 1)) {
        dx_Release(OBJ(p));
        return E_INVALIDARG;
      }
      p->samples = forced;
    }
  }
  memcpy(p->rtv_formats, desc->RTVFormats, sizeof p->rtv_formats);
  p->dsv_format = desc->DSVFormat;
  p->topology_type = desc->PrimitiveTopologyType;
  HRESULT hr = make_shader(This, &desc->VS, 0, &p->shaders[0]);
  if (SUCCEEDED(hr)) {
    /* (a pipeline with no pixel shader - stream output without rasterizing - gets one that the kernel's pipeline needs and nothing uses) */
    D3D12_SHADER_BYTECODE null_ps = {g_null_pixel_shader, sizeof g_null_pixel_shader};
    p->no_pixel_shader = !desc->PS.pShaderBytecode || !desc->PS.BytecodeLength;
    hr = make_shader(This, !p->no_pixel_shader ? &desc->PS : &null_ps, 1, &p->shaders[1]);
  }
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->GS, 3, &p->shaders[2]);
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->HS, 4, &p->shaders[3]);
  if (SUCCEEDED(hr)) hr = make_shader(This, &desc->DS, 5, &p->shaders[4]);
  This->next_pixel_base = This->next_shader_kind = 0;      /* (the options were for this pipeline) */
  This->next_stage_base[0] = This->next_stage_base[1] = This->next_stage_base[2] = 0;
  if (FAILED(hr)) { dx_Release(OBJ(p)); return hr; }
  /* the vertex inputs: register numbers from the vertex shader's signature */
  SigElement inputs[DXL_MAX_SIGNATURE], outputs[16];
  int found = 0, found_out = 0;
  const UINT n_in = dxbc_signature(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, 0, inputs, DXL_MAX_SIGNATURE, &found);
  const Dev *dev = This;
  /* The input layout, checked as the runtime checks it (src/Windows/dx_layout.h: its elements, and an element for every input the vertex shader reads): E_INVALIDARG.
   * Elements the vertex shader has no input for are not read (and need nothing of the device). */
  {
    _Static_assert(sizeof(D3D12_INPUT_ELEMENT_DESC) == sizeof(DxlElement) && offsetof(D3D12_INPUT_ELEMENT_DESC, InstanceDataStepRate) == offsetof(DxlElement, step_rate), "the same layout");
    const DxlElement *checked = (const DxlElement *)(const void *)desc->InputLayout.pInputElementDescs;      /* (the same seven fields: dx_layout.h) */
    if (dxl_check_elements(checked, desc->InputLayout.NumElements, layout_element_bytes, DXL_RULES_D3D12) != DXL_OK ||
        dxl_check_signature(checked, desc->InputLayout.NumElements, desc->VS.pShaderBytecode, desc->VS.BytecodeLength) != DXL_OK) {
      dx_Release(OBJ(p));
      return E_INVALIDARG;
    }
  }
  if (desc->NumRenderTargets > dev->caps.max_color_targets || desc->NumRenderTargets > 8) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  UINT appended[DXL_MAX_SLOTS] = {0};
  for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
    const D3D12_INPUT_ELEMENT_DESC *e = &desc->InputLayout.pInputElementDescs[i];
    UINT bytes = 0;
    const int format = vertex_format(e->Format, &bytes);
    /* (an appended element starts at the next multiple of its size, of four bytes at most - elements the shader does not read count too) */
    const UINT unit = bytes >= 4 ? 4 : bytes ? bytes : 1;
    const UINT offset = e->AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? (appended[e->InputSlot] + unit - 1) / unit * unit : e->AlignedByteOffset;
    appended[e->InputSlot] = offset + bytes;
    UINT location = 0xFFFFFFFFu;
    for (UINT k = 0; k < n_in; ++k)
      if (inputs[k].sysval == 0 && ieq(inputs[k].name, e->SemanticName) && inputs[k].index == e->SemanticIndex) {
        location = (inputs[k].read_mask & 0xF) ? inputs[k].reg : 0xFFFFFFFEu;      /* (an input the shader declares and never reads: nothing to fetch) */
        break;
      }
    if (location == 0xFFFFFFFEu || (location == 0xFFFFFFFFu && found)) continue;      /* (an element the vertex shader has no input for, or one it does not read) */
    if (location == 0xFFFFFFFFu) location = i;      /* (a shader without a signature: the elements in order) */
    if (p->attr_count >= dev->caps.max_vertex_attributes || p->attr_count >= 16) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
    if (e->InputSlot >= dev->caps.max_vertex_buffers || e->InputSlot >= 8 || (format > 3 && !(dev->caps.features & DEV_FEATURE_VERTEX_FORMATS))) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
    Attr *a = &p->attrs[p->attr_count++];
    a->format = (UINT)format;
    a->slot = e->InputSlot;
    a->per_instance = e->InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    a->step_rate = a->per_instance ? e->InstanceDataStepRate : 0;      /* (0: every instance reads the draw's first; a per-vertex element's is not looked at) */
    a->offset = offset;
    p->slots_used |= 1u << e->InputSlot;
    a->location = location;
    if (a->location >= dev->caps.max_vertex_attributes) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  }
  /* the interpolants between the stages: the kernel counts the registers the vertex shader writes, its position included */
  const UINT n_out = dxbc_signature(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, 1, outputs, 16, &found_out);
  p->varyings = found_out ? n_out : 3;
  /* (and the kernel gives each output its place by what it is - COLORn the place n, TEXCOORDn the place 2 + n - so a shader that writes TEXCOORD5 and
   * not the ones below it needs the places up to that one) */
  for (UINT k = 0; k < n_out; ++k) {
    UINT place = 0;
    if (ieq(outputs[k].name, "COLOR") && outputs[k].index < 2) place = outputs[k].index + 1;
    else if (ieq(outputs[k].name, "TEXCOORD") && outputs[k].index < 14) place = outputs[k].index + 3;
    if (place > p->varyings) p->varyings = place;
  }
  if (p->varyings > dev->caps.max_varyings) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  p->textures = p->rs->textures_unbounded ? dev->caps.max_texture_units : p->rs->textures;
  if (p->textures > dev->caps.max_texture_units) { dx_Release(OBJ(p)); return DXGI_ERROR_UNSUPPORTED; }
  /* stream output: the captured outputs by their register in the output signature of the last stage before the rasterizer (the geometry shader, else the domain
   * shader, else the vertex shader) */
  if (desc->StreamOutput.NumEntries) {
    const D3D12_SHADER_BYTECODE *last = desc->GS.pShaderBytecode ? &desc->GS : desc->DS.pShaderBytecode ? &desc->DS : &desc->VS;
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
  if (desc->Flags & (D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS | D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT)) return E_INVALIDARG;
  Pso *p = dobj_new(sizeof *p, This, &pso_vtbl, g_pso_iids, pso_destroy);
  if (!p) return E_OUTOFMEMORY;
  p->compute = 1;
  p->hash = desc_hash_compute(desc);
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

/* The cached form of a pipeline. Pipelines are made from their descriptions each time (the kernel keeps each shader once: SharedShader), so there is nothing to cache: the
 * blob is a mark that a program can store and hand back in CachedPSO, which is not looked at. */
static HRESULT STDMETHODCALLTYPE pso_GetCachedBlob(Pso *This, ID3DBlob **blob) {
  static const char mark[16] = "TCOS-PSO-CACHE1";
  if (!blob) return E_INVALIDARG;
  *blob = (ID3DBlob *)blob_new(mark, sizeof mark);
  return *blob ? S_OK : E_OUTOFMEMORY;
}

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
    case D3D12_BLEND_BLEND_FACTOR: return 11;            /* (graphics::BlendFactor::ConstantColor: each channel its own; an alpha factor takes the alpha) */
    case D3D12_BLEND_INV_BLEND_FACTOR: return 12;
    case D3D12_BLEND_ALPHA_FACTOR: return 13;            /* (ConstantAlpha: the factor's alpha in every channel) */
    case D3D12_BLEND_INV_ALPHA_FACTOR: return 14;
    case D3D12_BLEND_SRC1_COLOR: return 15;              /* (dual-source blending: the pixel shader's SV_Target1) */
    case D3D12_BLEND_INV_SRC1_COLOR: return 16;
    case D3D12_BLEND_SRC1_ALPHA: return 17;
    case D3D12_BLEND_INV_SRC1_ALPHA: return 18;
    default: return 1;
  }
}
/* What a blend factor needs: 0 nothing more, 1 the blend factor (OMSetBlendFactor), 2 the pixel shader's second colour (dual-source blending), -1 a value that is not a factor. */
static int blend_factor_need(UINT f) {
  if (f == D3D12_BLEND_BLEND_FACTOR || f == D3D12_BLEND_INV_BLEND_FACTOR || f == D3D12_BLEND_ALPHA_FACTOR || f == D3D12_BLEND_INV_ALPHA_FACTOR) return 1;
  if (f >= D3D12_BLEND_SRC1_COLOR && f <= D3D12_BLEND_INV_SRC1_ALPHA) return 2;
  return (f >= D3D12_BLEND_ZERO && f <= D3D12_BLEND_SRC_ALPHA_SAT) ? 0 : -1;
}
/* The blend state's needs, over the targets that blend: E_INVALIDARG for a value that is no factor, an operation that is none, a target that blends and has a logic
 * operation, targets of independent blending with different logic operations (the kernel's pipeline has one) and dual-source blending into more than one target;
 * DXGI_ERROR_UNSUPPORTED for dual-source blending, the blend factor and logic operations on a device without them. *constant: some target uses the blend factor. */
static HRESULT blend_state_check(const Dev *dev, const D3D12_BLEND_DESC *b, UINT targets, int *constant) {
  *constant = 0;
  int dual = 0, logic = 0;
  const UINT n = b->IndependentBlendEnable ? (targets ? targets : 1) : 1;
  for (UINT t = 0; t < n && t < 8; ++t) {
    const D3D12_RENDER_TARGET_BLEND_DESC *rt = &b->RenderTarget[t];
    if (rt->LogicOpEnable) {
      /* (one logic operation for every target: with independent blending each target must have the same) */
      if (rt->BlendEnable || (UINT)rt->LogicOp > D3D12_LOGIC_OP_OR_INVERTED) return E_INVALIDARG;
      logic = 1;
    }
    if (b->IndependentBlendEnable && t > 0 && (!rt->LogicOpEnable != !b->RenderTarget[0].LogicOpEnable || (rt->LogicOpEnable && rt->LogicOp != b->RenderTarget[0].LogicOp))) return E_INVALIDARG;
    if (!rt->BlendEnable) continue;
    if (rt->BlendOp < D3D12_BLEND_OP_ADD || rt->BlendOp > D3D12_BLEND_OP_MAX || rt->BlendOpAlpha < D3D12_BLEND_OP_ADD || rt->BlendOpAlpha > D3D12_BLEND_OP_MAX) return E_INVALIDARG;
    const UINT factors[4] = {rt->SrcBlend, rt->DestBlend, rt->SrcBlendAlpha, rt->DestBlendAlpha};
    for (UINT k = 0; k < 4; ++k) {
      const int need = blend_factor_need(factors[k]);
      if (need < 0) return E_INVALIDARG;
      if (need == 1) *constant = 1;
      if (need == 2) dual = 1;
    }
  }
  if (dual && targets > 1) return E_INVALIDARG;      /* (dual-source blending writes one target: the second colour is a source of its blend) */
  if (dual && !(dev->caps.features2 & DEV_FEATURE2_DUAL_SOURCE)) return DXGI_ERROR_UNSUPPORTED;
  if (logic && !(dev->caps.features2 & DEV_FEATURE2_LOGIC_OPS)) return DXGI_ERROR_UNSUPPORTED;
  if (*constant && !(dev->caps.flags & KMT_CAPS_BLEND_CONSTANT)) return DXGI_ERROR_UNSUPPORTED;
  return S_OK;
}
/* The rasterizer and depth-stencil state beyond what every kernel pipeline has: E_INVALIDARG for values that are none, DXGI_ERROR_UNSUPPORTED on a device without it. */
static HRESULT raster_state_check(const Dev *dev, const D3D12_RASTERIZER_DESC *r, FLOAT depth_bias, const D3D12_DEPTH_STENCIL_DESC *z, UINT8 back_read_mask, UINT8 back_write_mask,
                                  int dynamic_bias) {
  if (r->FillMode != D3D12_FILL_MODE_WIREFRAME && r->FillMode != D3D12_FILL_MODE_SOLID) return E_INVALIDARG;
  if (r->CullMode < D3D12_CULL_MODE_NONE || r->CullMode > D3D12_CULL_MODE_BACK) return E_INVALIDARG;
  if (depth_bias != depth_bias || r->SlopeScaledDepthBias != r->SlopeScaledDepthBias || r->DepthBiasClamp != r->DepthBiasClamp) return E_INVALIDARG;
  const D3D12_DEPTH_STENCILOP_DESC *faces[2] = {&z->FrontFace, &z->BackFace};
  for (UINT f = 0; f < 2 && z->StencilEnable; ++f)
    if (faces[f]->StencilFunc < D3D12_COMPARISON_FUNC_NEVER || faces[f]->StencilFunc > D3D12_COMPARISON_FUNC_ALWAYS || faces[f]->StencilFailOp < D3D12_STENCIL_OP_KEEP ||
        faces[f]->StencilFailOp > D3D12_STENCIL_OP_DECR || faces[f]->StencilDepthFailOp < D3D12_STENCIL_OP_KEEP || faces[f]->StencilDepthFailOp > D3D12_STENCIL_OP_DECR ||
        faces[f]->StencilPassOp < D3D12_STENCIL_OP_KEEP || faces[f]->StencilPassOp > D3D12_STENCIL_OP_DECR)
      return E_INVALIDARG;
  if ((r->FillMode == D3D12_FILL_MODE_WIREFRAME || depth_bias != 0.0f || r->SlopeScaledDepthBias != 0.0f || !r->DepthClipEnable || dynamic_bias ||
       (z->StencilEnable && (memcmp(&z->FrontFace, &z->BackFace, sizeof z->FrontFace) != 0 || back_read_mask != z->StencilReadMask || back_write_mask != z->StencilWriteMask))) &&
      !(dev->caps.features2 & DEV_FEATURE2_RASTERIZER_STATE))
    return DXGI_ERROR_UNSUPPORTED;
  return S_OK;
}
/* The bits of a depth format's unit of depth bias (D3D12_RASTERIZER_DESC::DepthBias): 16 and 24 for the normalised formats, 0 for the float ones (and none). */
static UINT depth_bias_bits(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return 16;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return 24;
    default: return 0;
  }
}
static UINT blend_op(UINT o) { return o >= 1 && o <= 5 ? o - 1 : 0; }
static UINT cmp_func(UINT f) { return f >= 1 && f <= 8 ? f - 1 : 7; }
static UINT stencil_op(UINT o) { return o >= 1 && o <= 8 ? o - 1 : 0; }

/* The kernel pipeline of the state for these draws, made when first needed. Returns 0 when the kernel refuses. */
/* A command list no longer binds the kernel pipeline `handle` of p (it is reset or destroyed, or it had it already): one user less. (The use is taken by
 * pso_pipeline, under the lock, so no other thread can retire it in between.) */
static void pso_unuse(Pso *p, UINT handle) {
  UINT destroy = 0;
  AcquireSRWLockExclusive(&p->pipe_lock);
  for (UINT i = 0; i < p->pipe_count; ++i)
    if (p->pipes[i].handle == handle && p->pipes[i].users) --p->pipes[i].users;
  for (UINT i = 0; i < p->nretired; ++i)
    if (p->retired[i].handle == handle) {
      if (p->retired[i].users && --p->retired[i].users == 0) {
        destroy = handle;
        p->retired[i] = p->retired[--p->nretired];
      }
      break;
    }
  ReleaseSRWLockExclusive(&p->pipe_lock);
  if (destroy) {
    UINT request[2] = {ESC_DESTROY_PIPELINE, destroy};
    dev_escape(p->d.dev, request, 2, NULL, 0);
  }
}
static UINT pso_pipeline_locked(Pso *p, const UINT strides[8], UINT stencil_ref, UINT stencil_back, const float depth_bias_in[3], const float blend_factor_in[4]);
/* The kernel pipeline for a draw: of the vertex buffers' strides, the stencil references of the two faces, the command list's depth bias (a pipeline with a dynamic one) and
 * blend factor (one that blends with it). */
static UINT pso_pipeline(Pso *p, const UINT strides[8], UINT stencil_ref, UINT stencil_back, const float depth_bias_in[3], const float blend_factor_in[4]) {
  if (p->compute) return p->compute_pipe;
  AcquireSRWLockExclusive(&p->pipe_lock);
  const UINT handle = pso_pipeline_locked(p, strides, stencil_ref, stencil_back, depth_bias_in, blend_factor_in);
  /* (the caller's use of it: a command list, which lets it go with pso_unuse) */
  for (UINT i = 0; handle && i < p->pipe_count; ++i)
    if (p->pipes[i].handle == handle) ++p->pipes[i].users;
  ReleaseSRWLockExclusive(&p->pipe_lock);
  return handle;
}
static UINT pso_pipeline_locked(Pso *p, const UINT strides[8], UINT stencil_ref, UINT stencil_back, const float depth_bias_in[3], const float blend_factor_in[4]) {
  /* (the blend factor counts only where a target blends with it; out of range it is clamped, as the targets' fixed-point formats clamp it) */
  float factor[4] = {0, 0, 0, 0};
  if (p->blend_constant)
    for (UINT c = 0; c < 4; ++c) factor[c] = blend_factor_in[c] != blend_factor_in[c] ? 0.0f : blend_factor_in[c];
  /* (the depth bias, its clamp and slope: the pipeline's, or with a dynamic one the command list's) */
  const float depth_bias[3] = {p->dynamic_bias ? depth_bias_in[0] : p->depth_bias, p->dynamic_bias ? depth_bias_in[1] : p->raster.DepthBiasClamp,
                               p->dynamic_bias ? depth_bias_in[2] : p->raster.SlopeScaledDepthBias};
  /* (the back faces' reference counts only with the stencil test) */
  if (!p->depth.StencilEnable) stencil_back = stencil_ref;
  for (UINT i = 0; i < p->pipe_count; ++i)
    if (p->pipes[i].stencil_ref == stencil_ref && p->pipes[i].stencil_back == stencil_back && memcmp(p->pipes[i].strides, strides, 8 * sizeof(UINT)) == 0 &&
        memcmp(p->pipes[i].blend_factor, factor, sizeof factor) == 0 && memcmp(p->pipes[i].depth_bias, depth_bias, sizeof depth_bias) == 0)
      return p->pipes[i].handle;
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
  r[w++] = (p->raster.CullMode == D3D12_CULL_MODE_NONE ? 0 : p->raster.CullMode == D3D12_CULL_MODE_BACK ? 1 : 2) |
           (p->raster.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON ? KCULL_CONSERVATIVE : 0u);      /* (the kernel's kCullConservative) */
  r[w++] = p->raster.FrontCounterClockwise ? 0 : 1;
  /* (depth writes go with the depth test: DepthEnable FALSE neither tests nor writes, whatever DepthWriteMask says) */
  r[w++] = (p->depth.DepthEnable ? 1u : 0u) | (p->depth.DepthEnable && p->depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 2u : 0u) | (rt->BlendEnable ? 4u : 0u) |
           (p->no_pixel_shader ? 8u : 0u);      /* (8: dxgk::kPipelineFlagStandInFragment) */
  for (UINT i = 0; i < 4; ++i) {
    r[w++] = i < p->attr_count ? p->attrs[i].location : 0;
    r[w++] = i < p->attr_count ? p->attrs[i].format : 0;
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
  /* custom: blending, or a write mask that leaves channels alone (the kernel writes every channel of a state that is not custom) */
  r[w++] = rt->BlendEnable || (rt->RenderTargetWriteMask & format_write_mask(p->rtv_formats[0])) != format_write_mask(p->rtv_formats[0]) ? 1 : 0;
  r[w++] = rt->BlendEnable ? 1 : 0;
  r[w++] = blend_factor(rt->SrcBlend);
  r[w++] = blend_factor(rt->DestBlend);
  r[w++] = blend_factor(rt->SrcBlendAlpha);
  r[w++] = blend_factor(rt->DestBlendAlpha);
  r[w++] = blend_op(rt->BlendOp);
  r[w++] = blend_op(rt->BlendOpAlpha);
  r[w++] = rt->RenderTargetWriteMask & format_write_mask(p->rtv_formats[0]);      /* (a narrow target: its channels only) */
  UINT instance_bits = 0;
  for (UINT i = 0; i < p->attr_count && i < 4; ++i)
    if (p->attrs[i].per_instance) instance_bits |= 1u << i;
  r[w++] = instance_bits;
  r[w++] = p->render_targets ? p->render_targets : 1;
  r[w++] = p->blend.IndependentBlendEnable ? 1 : 0;
  for (UINT t = 1; t < 4; ++t) {
    const D3D12_RENDER_TARGET_BLEND_DESC *b = &p->blend.RenderTarget[p->blend.IndependentBlendEnable ? t : 0];
    r[w++] = b->BlendEnable || (b->RenderTargetWriteMask & format_write_mask(p->rtv_formats[t])) != format_write_mask(p->rtv_formats[t]) ? 1 : 0;      /* (custom, as above) */
    r[w++] = b->BlendEnable ? 1 : 0;
    r[w++] = blend_factor(b->SrcBlend);
    r[w++] = blend_factor(b->DestBlend);
    r[w++] = blend_factor(b->SrcBlendAlpha);
    r[w++] = blend_factor(b->DestBlendAlpha);
    r[w++] = blend_op(b->BlendOp);
    r[w++] = blend_op(b->BlendOpAlpha);
    r[w++] = b->RenderTargetWriteMask & format_write_mask(p->rtv_formats[t]);
  }
  r[w++] = p->samples > 1 ? (1u | (p->blend.AlphaToCoverageEnable ? 2u : 0u)) : 0u;      /* multisample: coverage for each sample, alpha to coverage */
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
  /* (the rasterizer state of the tagged part: a fill mode, depth bias, depth clipping, the back faces' stencil and a logic operation other than the plain ones) */
  const D3D12_DEPTH_STENCILOP_DESC *back = &p->depth.BackFace, *front = &p->depth.FrontFace;
  const int separate_back = p->depth.StencilEnable && (memcmp(back, front, sizeof *back) != 0 || p->back_read_mask != p->depth.StencilReadMask || p->back_write_mask != p->depth.StencilWriteMask ||
                                                       (stencil_back & 0xFF) != (stencil_ref & 0xFF));
  const int bias = p->dsv_format != DXGI_FORMAT_UNKNOWN && (depth_bias[0] != 0.0f || depth_bias[2] != 0.0f);
  const int logic = rt->LogicOpEnable;      /* (every target's: blend_state_check) */
  const int raster_part = p->raster.FillMode == D3D12_FILL_MODE_WIREFRAME || bias || !p->raster.DepthClipEnable || separate_back || logic;
  UINT rates = 0;      /* (the attributes up to the last per-instance one that does not step by 1: the kernel's step rates) */
  for (UINT i = 0; i < p->attr_count; ++i)
    if (p->attrs[i].per_instance && p->attrs[i].step_rate != 1) rates = i + 1;
  const UINT line_mode = p->line_mode;      /* (not aliased: the tagged part 'LIN1') */
  const UINT forced = p->forced_samples;      /* (forced: the tagged part 'FSC1') */
  if (p->attr_count > 4 || max_slot > 0 || more_blends || p->blend_constant || raster_part || rates || line_mode || forced) {
    if (!p->so_entries) r[w++] = 0;
    const UINT extra = p->attr_count > 4 ? p->attr_count - 4 : 0;
    r[w++] = extra;
    UINT instance_high = 0;
    for (UINT k = 0; k < extra; ++k) {
      const Attr *a = &p->attrs[4 + k];
      r[w++] = a->location;
      r[w++] = a->format | (a->slot << 8);
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
      r[w++] = b->BlendEnable || (b->RenderTargetWriteMask & format_write_mask(p->rtv_formats[4 + k])) != format_write_mask(p->rtv_formats[4 + k]) ? 1 : 0;      /* (custom, as above) */
      r[w++] = b->BlendEnable ? 1 : 0;
      r[w++] = blend_factor(b->SrcBlend);
      r[w++] = blend_factor(b->DestBlend);
      r[w++] = blend_factor(b->SrcBlendAlpha);
      r[w++] = blend_factor(b->DestBlendAlpha);
      r[w++] = blend_op(b->BlendOp);
      r[w++] = blend_op(b->BlendOpAlpha);
      r[w++] = b->RenderTargetWriteMask & format_write_mask(p->rtv_formats[4 + k]);
    }
    if (p->blend_constant || raster_part || rates || line_mode || forced)
      for (UINT c = 0; c < 4; ++c) r[w++] = bits_of(factor[c]);      /* (the colour of the constant blend factors) */
    if (raster_part) {
      r[w++] = 0x31545352u;      /* 'RST1' (dxgk::kPipelineRasterTag), and its 16 words */
      r[w++] = 16;
      r[w++] = p->raster.FillMode == D3D12_FILL_MODE_WIREFRAME ? 1 : 0;
      r[w++] = p->raster.DepthClipEnable ? 1 : 0;
      r[w++] = bias ? bits_of(depth_bias[0]) : 0;      /* (a float: RASTERIZER1/2's, or DepthBias as one) */
      r[w++] = bias ? bits_of(depth_bias[2]) : 0;
      r[w++] = bias ? bits_of(depth_bias[1]) : 0;
      r[w++] = depth_bias_bits(p->dsv_format);
      r[w++] = separate_back ? 1 : 0;
      r[w++] = cmp_func(back->StencilFunc);
      r[w++] = stencil_op(back->StencilFailOp);
      r[w++] = stencil_op(back->StencilDepthFailOp);
      r[w++] = stencil_op(back->StencilPassOp);
      r[w++] = p->back_read_mask;
      r[w++] = p->back_write_mask;
      r[w++] = logic ? 1 : 0;
      r[w++] = logic ? (UINT)rt->LogicOp : 4;      /* (D3D12_LOGIC_OP and graphics::LogicOp are numbered alike) */
      r[w++] = separate_back ? stencil_back & 0xFF : stencil_ref & 0xFF;      /* (the back faces' reference: OMSetFrontAndBackStencilRef) */
    }
    if (rates) {      /* 'STP1' (dxgk::kPipelineStepRateTag): the step rates, a per-vertex attribute's word 0, a per-instance one's its InstanceDataStepRate */
      r[w++] = 0x31505453u;
      r[w++] = rates;
      for (UINT i = 0; i < rates; ++i) r[w++] = p->attrs[i].per_instance ? p->attrs[i].step_rate : 0;
    }
    if (line_mode) {      /* 'LIN1' (dxgk::kPipelineLineTag): the line mode - D3D12_LINE_RASTERIZATION_MODE and graphics::LineMode are numbered alike */
      r[w++] = 0x314E494Cu;
      r[w++] = 1;
      r[w++] = line_mode;
    }
    if (forced) {      /* 'FSC1' (dxgk::kPipelineForcedSamplesTag): the forced sample count */
      r[w++] = 0x31435346u;
      r[w++] = 1;
      r[w++] = forced;
    }
  }
  UINT reply[1] = {0};
  if (!KMT_OK(dev_escape(p->d.dev, r, w, reply, 1))) return 0;
  if (p->pipe_count == MAX_PSO_PIPELINES) {
    /* (the oldest goes: destroyed now if no list binds it, else retired until the last list that does is reset) */
    if (p->pipes[0].users) {
      if (p->nretired == p->cap_retired) {
        const UINT cap = p->cap_retired ? p->cap_retired * 2 : 8;
        PsoPipe *n = cap < (1u << 20) ? mem_grow(p->retired, (SIZE_T)cap * sizeof *n) : NULL;
        if (!n) {      /* (no memory to remember the old one: the new pipeline is let go, and the draw is not made) */
          UINT drop[2] = {ESC_DESTROY_PIPELINE, reply[0]};
          dev_escape(p->d.dev, drop, 2, NULL, 0);
          return 0;
        }
        p->retired = n;
        p->cap_retired = cap;
      }
      p->retired[p->nretired++] = p->pipes[0];
    } else {
      UINT drop[2] = {ESC_DESTROY_PIPELINE, p->pipes[0].handle};
      dev_escape(p->d.dev, drop, 2, NULL, 0);
    }
    memmove(&p->pipes[0], &p->pipes[1], (MAX_PSO_PIPELINES - 1) * sizeof p->pipes[0]);
    p->pipe_count--;
  }
  memcpy(p->pipes[p->pipe_count].strides, strides, 8 * sizeof(UINT));
  p->pipes[p->pipe_count].stencil_ref = stencil_ref;
  p->pipes[p->pipe_count].stencil_back = stencil_back;
  memcpy(p->pipes[p->pipe_count].blend_factor, factor, sizeof factor);
  memcpy(p->pipes[p->pipe_count].depth_bias, depth_bias, sizeof depth_bias);
  p->pipes[p->pipe_count].handle = reply[0];
  p->pipes[p->pipe_count].users = 0;
  p->pipe_count++;
  return reply[0];
}

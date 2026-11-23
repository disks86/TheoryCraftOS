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

/* Part of d3d12.c: the root signature deserializers (D3D12CreateRootSignatureDeserializer, D3D12CreateVersionedRootSignatureDeserializer). They read what
 * D3D12SerializeRootSignature writes - the root signature by itself (pso.inc serialize_root_signature) or in the RTS0 part of a DXBC container, as Windows and the
 * shader compilers write it - every offset and count checked against the blob. A description of the other version is converted as Direct3D 12 converts it: from
 * 1.1 to 1.0 the flags are dropped; from 1.0 to 1.1 descriptor ranges are DESCRIPTORS_VOLATILE (and DATA_VOLATILE, but for samplers) and root descriptors
 * DATA_VOLATILE, which is what a 1.0 root signature promises. */

/* A parsed description of either version, with the arrays it points into (one allocation each). */
typedef struct RsDesc {
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC v;
  void *memory;
} RsDesc;

static BOOL rs_u32(const BYTE *b, SIZE_T size, SIZE_T at, UINT *out) {
  if (at > size || size - at < 4) return FALSE;
  memcpy(out, b + at, 4);
  return TRUE;
}
static FLOAT rs_float(UINT v) { FLOAT f; memcpy(&f, &v, 4); return f; }

/* The root signature bytes of a blob: itself, or the RTS0 part of a DXBC container. */
static BOOL rs_bytes(const void *data, SIZE_T size, const BYTE **out, SIZE_T *out_size) {
  const BYTE *b = data;
  if (!b || size < 24) return FALSE;
  if (memcmp(b, "DXBC", 4) != 0) { *out = b; *out_size = size; return TRUE; }
  UINT total, parts;
  if (!rs_u32(b, size, 24, &total) || !rs_u32(b, size, 28, &parts) || total > size || parts > 64) return FALSE;
  for (UINT i = 0; i < parts; ++i) {
    UINT at, length;
    if (!rs_u32(b, size, 32 + 4 * (SIZE_T)i, &at) || !rs_u32(b, size, (SIZE_T)at + 4, &length)) return FALSE;
    if (at > size - 8 || length > size - at - 8) return FALSE;
    if (memcmp(b + at, "RTS0", 4) == 0) { *out = b + at + 8; *out_size = length; return TRUE; }
  }
  return FALSE;
}

/* Reads the blob into a description of the version it was written in. */
static HRESULT rs_parse(const void *data, SIZE_T size, RsDesc *out) {
  const BYTE *b;
  SIZE_T n;
  memset(out, 0, sizeof *out);
  if (!rs_bytes(data, size, &b, &n)) return E_INVALIDARG;
  UINT version, np, params_at, ns, samplers_at, flags;
  if (!rs_u32(b, n, 0, &version) || !rs_u32(b, n, 4, &np) || !rs_u32(b, n, 8, &params_at) || !rs_u32(b, n, 12, &ns) || !rs_u32(b, n, 16, &samplers_at) || !rs_u32(b, n, 20, &flags))
    return E_INVALIDARG;
  if ((version != 1 && version != 2) || np > 64 || ns > 2032) return E_INVALIDARG;
  const BOOL v11 = version == 2;
  /* (the ranges first counted, to make one allocation) */
  UINT ranges = 0;
  for (UINT i = 0; i < np; ++i) {
    UINT type, payload, count;
    if (!rs_u32(b, n, params_at + 12 * (SIZE_T)i, &type) || !rs_u32(b, n, params_at + 12 * (SIZE_T)i + 8, &payload)) return E_INVALIDARG;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      if (!rs_u32(b, n, payload, &count) || count > 4096 || ranges + count < ranges) return E_INVALIDARG;
      ranges += count;
    }
  }
  if (ranges > 65536) return E_INVALIDARG;
  const SIZE_T param_size = v11 ? sizeof(D3D12_ROOT_PARAMETER1) : sizeof(D3D12_ROOT_PARAMETER), range_size = v11 ? sizeof(D3D12_DESCRIPTOR_RANGE1) : sizeof(D3D12_DESCRIPTOR_RANGE);
  BYTE *memory = mem_alloc(param_size * np + range_size * ranges + sizeof(D3D12_STATIC_SAMPLER_DESC) * ns + 1);
  if (!memory) return E_OUTOFMEMORY;
  memset(memory, 0, param_size * np + range_size * ranges + sizeof(D3D12_STATIC_SAMPLER_DESC) * ns + 1);
  BYTE *params = memory, *range_mem = memory + param_size * np, *sampler_mem = range_mem + range_size * ranges;
  UINT next_range = 0;
  for (UINT i = 0; i < np; ++i) {
    UINT type, vis, payload;
    rs_u32(b, n, params_at + 12 * (SIZE_T)i, &type);
    rs_u32(b, n, params_at + 12 * (SIZE_T)i + 4, &vis);
    rs_u32(b, n, params_at + 12 * (SIZE_T)i + 8, &payload);
    if (type > D3D12_ROOT_PARAMETER_TYPE_UAV || vis > D3D12_SHADER_VISIBILITY_MESH) { mem_free(memory); return E_INVALIDARG; }
    D3D12_ROOT_PARAMETER *p10 = (D3D12_ROOT_PARAMETER *)(params + param_size * i);
    D3D12_ROOT_PARAMETER1 *p11 = (D3D12_ROOT_PARAMETER1 *)(params + param_size * i);
    if (v11) { p11->ParameterType = type; p11->ShaderVisibility = vis; } else { p10->ParameterType = type; p10->ShaderVisibility = vis; }
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      UINT count, at;
      if (!rs_u32(b, n, payload, &count) || !rs_u32(b, n, payload + 4, &at)) { mem_free(memory); return E_INVALIDARG; }
      const UINT stride = v11 ? 24 : 20;
      for (UINT k = 0; k < count; ++k) {
        UINT w[6] = {0};
        for (UINT j = 0; j < stride / 4; ++j)
          if (!rs_u32(b, n, at + (SIZE_T)k * stride + 4 * j, &w[j])) { mem_free(memory); return E_INVALIDARG; }
        if (w[0] > D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) { mem_free(memory); return E_INVALIDARG; }
        if (v11) {
          D3D12_DESCRIPTOR_RANGE1 *r = (D3D12_DESCRIPTOR_RANGE1 *)(range_mem + range_size * (next_range + k));
          r->RangeType = w[0]; r->NumDescriptors = w[1]; r->BaseShaderRegister = w[2]; r->RegisterSpace = w[3]; r->Flags = w[4]; r->OffsetInDescriptorsFromTableStart = w[5];
        } else {
          D3D12_DESCRIPTOR_RANGE *r = (D3D12_DESCRIPTOR_RANGE *)(range_mem + range_size * (next_range + k));
          r->RangeType = w[0]; r->NumDescriptors = w[1]; r->BaseShaderRegister = w[2]; r->RegisterSpace = w[3]; r->OffsetInDescriptorsFromTableStart = w[4];
        }
      }
      if (v11) { p11->DescriptorTable.NumDescriptorRanges = count; p11->DescriptorTable.pDescriptorRanges = (const D3D12_DESCRIPTOR_RANGE1 *)(range_mem + range_size * next_range); }
      else { p10->DescriptorTable.NumDescriptorRanges = count; p10->DescriptorTable.pDescriptorRanges = (const D3D12_DESCRIPTOR_RANGE *)(range_mem + range_size * next_range); }
      next_range += count;
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      UINT reg, space, values;
      if (!rs_u32(b, n, payload, &reg) || !rs_u32(b, n, payload + 4, &space) || !rs_u32(b, n, payload + 8, &values)) { mem_free(memory); return E_INVALIDARG; }
      D3D12_ROOT_CONSTANTS *c = v11 ? &p11->Constants : &p10->Constants;
      c->ShaderRegister = reg; c->RegisterSpace = space; c->Num32BitValues = values;
    } else {
      UINT reg, space, dflags = 0;
      if (!rs_u32(b, n, payload, &reg) || !rs_u32(b, n, payload + 4, &space) || (v11 && !rs_u32(b, n, payload + 8, &dflags))) { mem_free(memory); return E_INVALIDARG; }
      if (v11) { p11->Descriptor.ShaderRegister = reg; p11->Descriptor.RegisterSpace = space; p11->Descriptor.Flags = dflags; }
      else { p10->Descriptor.ShaderRegister = reg; p10->Descriptor.RegisterSpace = space; }
    }
  }
  for (UINT i = 0; i < ns; ++i) {
    UINT w[13];
    for (UINT j = 0; j < 13; ++j)
      if (!rs_u32(b, n, samplers_at + (SIZE_T)i * 52 + 4 * j, &w[j])) { mem_free(memory); return E_INVALIDARG; }
    D3D12_STATIC_SAMPLER_DESC *s = (D3D12_STATIC_SAMPLER_DESC *)(sampler_mem + sizeof *s * i);
    s->Filter = w[0]; s->AddressU = w[1]; s->AddressV = w[2]; s->AddressW = w[3]; s->MipLODBias = rs_float(w[4]); s->MaxAnisotropy = w[5]; s->ComparisonFunc = w[6];
    s->BorderColor = w[7]; s->MinLOD = rs_float(w[8]); s->MaxLOD = rs_float(w[9]); s->ShaderRegister = w[10]; s->RegisterSpace = w[11]; s->ShaderVisibility = w[12];
  }
  out->memory = memory;
  if (v11) {
    out->v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    out->v.Desc_1_1.NumParameters = np;
    out->v.Desc_1_1.pParameters = np ? (const D3D12_ROOT_PARAMETER1 *)params : NULL;
    out->v.Desc_1_1.NumStaticSamplers = ns;
    out->v.Desc_1_1.pStaticSamplers = ns ? (const D3D12_STATIC_SAMPLER_DESC *)sampler_mem : NULL;
    out->v.Desc_1_1.Flags = flags;
  } else {
    out->v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    out->v.Desc_1_0.NumParameters = np;
    out->v.Desc_1_0.pParameters = np ? (const D3D12_ROOT_PARAMETER *)params : NULL;
    out->v.Desc_1_0.NumStaticSamplers = ns;
    out->v.Desc_1_0.pStaticSamplers = ns ? (const D3D12_STATIC_SAMPLER_DESC *)sampler_mem : NULL;
    out->v.Desc_1_0.Flags = flags;
  }
  return S_OK;
}

/* The description converted to the other version (a new one; the samplers' array is shared with `from`, which outlives it). */
static HRESULT rs_convert(const RsDesc *from, D3D_ROOT_SIGNATURE_VERSION to, RsDesc *out) {
  memset(out, 0, sizeof *out);
  const BOOL up = to == D3D_ROOT_SIGNATURE_VERSION_1_1;
  const UINT np = up ? from->v.Desc_1_0.NumParameters : from->v.Desc_1_1.NumParameters;
  UINT ranges = 0;
  for (UINT i = 0; i < np; ++i) {
    const UINT type = up ? from->v.Desc_1_0.pParameters[i].ParameterType : from->v.Desc_1_1.pParameters[i].ParameterType;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      ranges += up ? from->v.Desc_1_0.pParameters[i].DescriptorTable.NumDescriptorRanges : from->v.Desc_1_1.pParameters[i].DescriptorTable.NumDescriptorRanges;
  }
  const SIZE_T param_size = up ? sizeof(D3D12_ROOT_PARAMETER1) : sizeof(D3D12_ROOT_PARAMETER), range_size = up ? sizeof(D3D12_DESCRIPTOR_RANGE1) : sizeof(D3D12_DESCRIPTOR_RANGE);
  BYTE *memory = mem_alloc(param_size * np + range_size * ranges + 1);
  if (!memory) return E_OUTOFMEMORY;
  memset(memory, 0, param_size * np + range_size * ranges + 1);
  BYTE *range_mem = memory + param_size * np;
  UINT next = 0;
  for (UINT i = 0; i < np; ++i) {
    if (up) {
      const D3D12_ROOT_PARAMETER *s = &from->v.Desc_1_0.pParameters[i];
      D3D12_ROOT_PARAMETER1 *d = (D3D12_ROOT_PARAMETER1 *)(memory + param_size * i);
      d->ParameterType = s->ParameterType;
      d->ShaderVisibility = s->ShaderVisibility;
      if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        D3D12_DESCRIPTOR_RANGE1 *r = (D3D12_DESCRIPTOR_RANGE1 *)(range_mem + range_size * next);
        for (UINT k = 0; k < s->DescriptorTable.NumDescriptorRanges; ++k) {
          const D3D12_DESCRIPTOR_RANGE *sr = &s->DescriptorTable.pDescriptorRanges[k];
          r[k].RangeType = sr->RangeType; r[k].NumDescriptors = sr->NumDescriptors; r[k].BaseShaderRegister = sr->BaseShaderRegister; r[k].RegisterSpace = sr->RegisterSpace;
          r[k].OffsetInDescriptorsFromTableStart = sr->OffsetInDescriptorsFromTableStart;
          r[k].Flags = sr->RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
                                                                             : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        }
        d->DescriptorTable.NumDescriptorRanges = s->DescriptorTable.NumDescriptorRanges;
        d->DescriptorTable.pDescriptorRanges = r;
        next += s->DescriptorTable.NumDescriptorRanges;
      } else if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        d->Constants = s->Constants;
      } else {
        d->Descriptor.ShaderRegister = s->Descriptor.ShaderRegister;
        d->Descriptor.RegisterSpace = s->Descriptor.RegisterSpace;
        d->Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
      }
    } else {
      const D3D12_ROOT_PARAMETER1 *s = &from->v.Desc_1_1.pParameters[i];
      D3D12_ROOT_PARAMETER *d = (D3D12_ROOT_PARAMETER *)(memory + param_size * i);
      d->ParameterType = s->ParameterType;
      d->ShaderVisibility = s->ShaderVisibility;
      if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        D3D12_DESCRIPTOR_RANGE *r = (D3D12_DESCRIPTOR_RANGE *)(range_mem + range_size * next);
        for (UINT k = 0; k < s->DescriptorTable.NumDescriptorRanges; ++k) {
          const D3D12_DESCRIPTOR_RANGE1 *sr = &s->DescriptorTable.pDescriptorRanges[k];
          r[k].RangeType = sr->RangeType; r[k].NumDescriptors = sr->NumDescriptors; r[k].BaseShaderRegister = sr->BaseShaderRegister; r[k].RegisterSpace = sr->RegisterSpace;
          r[k].OffsetInDescriptorsFromTableStart = sr->OffsetInDescriptorsFromTableStart;
        }
        d->DescriptorTable.NumDescriptorRanges = s->DescriptorTable.NumDescriptorRanges;
        d->DescriptorTable.pDescriptorRanges = r;
        next += s->DescriptorTable.NumDescriptorRanges;
      } else if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        d->Constants = s->Constants;
      } else {
        d->Descriptor.ShaderRegister = s->Descriptor.ShaderRegister;
        d->Descriptor.RegisterSpace = s->Descriptor.RegisterSpace;
      }
    }
  }
  out->memory = memory;
  if (up) {
    out->v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    out->v.Desc_1_1.NumParameters = np;
    out->v.Desc_1_1.pParameters = np ? (const D3D12_ROOT_PARAMETER1 *)memory : NULL;
    out->v.Desc_1_1.NumStaticSamplers = from->v.Desc_1_0.NumStaticSamplers;
    out->v.Desc_1_1.pStaticSamplers = from->v.Desc_1_0.pStaticSamplers;
    out->v.Desc_1_1.Flags = from->v.Desc_1_0.Flags;
  } else {
    out->v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    out->v.Desc_1_0.NumParameters = np;
    out->v.Desc_1_0.pParameters = np ? (const D3D12_ROOT_PARAMETER *)memory : NULL;
    out->v.Desc_1_0.NumStaticSamplers = from->v.Desc_1_1.NumStaticSamplers;
    out->v.Desc_1_0.pStaticSamplers = from->v.Desc_1_1.pStaticSamplers;
    out->v.Desc_1_0.Flags = from->v.Desc_1_1.Flags;
  }
  return S_OK;
}

/* The deserializer objects: both kinds are the one structure (the plain one gives the 1.0 description). */
typedef struct RsDeserializer {
  DxObj o;
  RsDesc parsed;          /* as the blob has it */
  RsDesc other;           /* the other version, made when asked for */
  CRITICAL_SECTION lock;
} RsDeserializer;
static const GUID *const g_rsd_iids[] = {&IID_ID3D12RootSignatureDeserializer, NULL};
static const GUID *const g_vrsd_iids[] = {&IID_ID3D12VersionedRootSignatureDeserializer, NULL};
static void rsd_destroy(DxObj *o) {
  RsDeserializer *d = (RsDeserializer *)o;
  mem_free(d->other.memory);
  mem_free(d->parsed.memory);
  DeleteCriticalSection(&d->lock);
}
static const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *rsd_at_version(RsDeserializer *d, D3D_ROOT_SIGNATURE_VERSION version) {
  if (d->parsed.v.Version == version) return &d->parsed.v;
  EnterCriticalSection(&d->lock);
  if (!d->other.memory && FAILED(rs_convert(&d->parsed, version, &d->other))) { LeaveCriticalSection(&d->lock); return NULL; }
  LeaveCriticalSection(&d->lock);
  return &d->other.v;
}
static const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE rsd_GetRootSignatureDesc(RsDeserializer *d) {
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *v = rsd_at_version(d, D3D_ROOT_SIGNATURE_VERSION_1_0);
  return v ? &v->Desc_1_0 : NULL;
}
static HRESULT STDMETHODCALLTYPE vrsd_GetRootSignatureDescAtVersion(RsDeserializer *d, D3D_ROOT_SIGNATURE_VERSION version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (version != D3D_ROOT_SIGNATURE_VERSION_1_0 && version != D3D_ROOT_SIGNATURE_VERSION_1_1) return E_INVALIDARG;
  *out = rsd_at_version(d, version);
  return *out ? S_OK : E_OUTOFMEMORY;
}
static const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE vrsd_GetUnconvertedRootSignatureDesc(RsDeserializer *d) { return &d->parsed.v; }

static HRESULT rsd_make(const void *data, SIZE_T size, BOOL versioned, REFIID riid, void **out) {
  extern const ID3D12RootSignatureDeserializerVtbl rsd_vtbl;
  extern const ID3D12VersionedRootSignatureDeserializerVtbl vrsd_vtbl;
  if (!out) return E_POINTER;
  *out = NULL;
  RsDeserializer *d = mem_alloc(sizeof *d);
  if (!d) return E_OUTOFMEMORY;
  memset(d, 0, sizeof *d);
  dx_init(OBJ(d), versioned ? (const void *)&vrsd_vtbl : (const void *)&rsd_vtbl, versioned ? g_vrsd_iids : g_rsd_iids, rsd_destroy);
  InitializeCriticalSection(&d->lock);
  const HRESULT hr = rs_parse(data, size, &d->parsed);
  if (FAILED(hr)) { dx_Release(OBJ(d)); return hr; }
  const HRESULT q = dx_QueryInterface(OBJ(d), riid, out);
  dx_Release(OBJ(d));
  return q;
}
EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return rsd_make(data, size, FALSE, riid, out); }
EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return rsd_make(data, size, TRUE, riid, out); }

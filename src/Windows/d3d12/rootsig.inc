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

/* Part of d3d12.c: serialized root signatures, versions 1.0, 1.1 and 1.2 - the serializer (D3D12SerializeRootSignature, D3D12SerializeVersionedRootSignature) and the
 * deserializers (D3D12CreateRootSignatureDeserializer, D3D12CreateVersionedRootSignatureDeserializer, ...FromSubobjectInLibrary).
 *
 * The bytes are Direct3D 12's: a header (version 1 / 2 / 3, parameter count and offset, static sampler count and offset, flags), 12 bytes per parameter (type, visibility,
 * offset of its payload), the payloads (a table: range count and offset; constants: register, space, count; a root descriptor: register, space and, from 1.1, flags), the
 * ranges (20 bytes; 24 from 1.1, with flags before the offset) and the static samplers (52 bytes; 56 in 1.2, with D3D12_SAMPLER_FLAGS last). A blob is that alone or the
 * RTS0 part of a DXBC container; a DXIL library carries its root signatures as subobjects of its RDAT part. Everything read is checked against the blob.
 *
 * Every version is read into one form, the 1.2 one (RsCanon), and the description of any version is made from it. A description of another version is converted as
 * Direct3D 12 converts it: to 1.0 the flags are dropped; from 1.0 to 1.1 or 1.2 descriptor ranges are DESCRIPTORS_VOLATILE (and DATA_VOLATILE, but for samplers) and root
 * descriptors DATA_VOLATILE, which is what a 1.0 root signature promises; from 1.2 down, a static sampler with flags has no description (E_INVALIDARG), one without loses
 * nothing. */

/* ---- the one form ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct RsCanon {
  D3D_ROOT_SIGNATURE_VERSION version;      /* what it was written as */
  UINT np, ns, flags;
  D3D12_ROOT_PARAMETER1 *params;           /* (tables point into ranges) */
  D3D12_DESCRIPTOR_RANGE1 *ranges;
  UINT range_count;
  D3D12_STATIC_SAMPLER_DESC1 *samplers;
  void *memory;                            /* (all three arrays) */
} RsCanon;

static void rs_canon_free(RsCanon *c) { mem_free(c->memory); memset(c, 0, sizeof *c); }
static BOOL rs_canon_alloc(RsCanon *c, UINT np, UINT ranges, UINT ns) {
  const SIZE_T bytes = sizeof(D3D12_ROOT_PARAMETER1) * (SIZE_T)np + sizeof(D3D12_DESCRIPTOR_RANGE1) * (SIZE_T)ranges + sizeof(D3D12_STATIC_SAMPLER_DESC1) * (SIZE_T)ns;
  BYTE *m = mem_alloc(bytes + 1);      /* (zeroed) */
  if (!m) return FALSE;
  c->memory = m;
  c->params = (D3D12_ROOT_PARAMETER1 *)m;
  c->ranges = (D3D12_DESCRIPTOR_RANGE1 *)(m + sizeof(D3D12_ROOT_PARAMETER1) * (SIZE_T)np);
  c->samplers = (D3D12_STATIC_SAMPLER_DESC1 *)(m + sizeof(D3D12_ROOT_PARAMETER1) * (SIZE_T)np + sizeof(D3D12_DESCRIPTOR_RANGE1) * (SIZE_T)ranges);
  c->np = np;
  c->range_count = ranges;
  c->ns = ns;
  return TRUE;
}

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

/* The flags a 1.0 root signature's ranges and root descriptors have in the later versions. */
static UINT rs_volatile_range_flags(UINT range_type) {
  return range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
                                                           : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
}

/* Reads a serialized root signature (any version) into the one form. */
static HRESULT rs_parse(const void *data, SIZE_T size, RsCanon *out) {
  const BYTE *b;
  SIZE_T n;
  memset(out, 0, sizeof *out);
  if (!rs_bytes(data, size, &b, &n)) return E_INVALIDARG;
  UINT version, np, params_at, ns, samplers_at, flags;
  if (!rs_u32(b, n, 0, &version) || !rs_u32(b, n, 4, &np) || !rs_u32(b, n, 8, &params_at) || !rs_u32(b, n, 12, &ns) || !rs_u32(b, n, 16, &samplers_at) || !rs_u32(b, n, 20, &flags))
    return E_INVALIDARG;
  if (version < 1 || version > 3 || np > 64 || ns > 2032) return E_INVALIDARG;
  const BOOL later = version >= 2;
  const UINT range_stride = later ? 24 : 20, sampler_words = version == 3 ? 14 : 13;
  /* (the ranges first counted, to make one allocation) */
  UINT ranges = 0;
  for (UINT i = 0; i < np; ++i) {
    UINT type, payload, count;
    if (!rs_u32(b, n, params_at + 12 * (SIZE_T)i, &type) || !rs_u32(b, n, params_at + 12 * (SIZE_T)i + 8, &payload)) return E_INVALIDARG;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      if (!rs_u32(b, n, payload, &count) || count > 4096 || ranges > 65536 - count) return E_INVALIDARG;
      ranges += count;
    }
  }
  if (!rs_canon_alloc(out, np, ranges, ns)) return E_OUTOFMEMORY;
  out->version = (D3D_ROOT_SIGNATURE_VERSION)version;
  out->flags = flags;
  UINT next_range = 0;
  for (UINT i = 0; i < np; ++i) {
    UINT type = 0, vis = 0, payload = 0;      /* (type and payload were read when the ranges were counted) */
    rs_u32(b, n, params_at + 12 * (SIZE_T)i, &type);
    if (!rs_u32(b, n, params_at + 12 * (SIZE_T)i + 4, &vis)) { rs_canon_free(out); return E_INVALIDARG; }
    rs_u32(b, n, params_at + 12 * (SIZE_T)i + 8, &payload);
    if (type > D3D12_ROOT_PARAMETER_TYPE_UAV || vis > D3D12_SHADER_VISIBILITY_MESH) { rs_canon_free(out); return E_INVALIDARG; }
    D3D12_ROOT_PARAMETER1 *p = &out->params[i];
    p->ParameterType = type;
    p->ShaderVisibility = vis;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      UINT count, at;
      if (!rs_u32(b, n, payload, &count) || !rs_u32(b, n, (SIZE_T)payload + 4, &at)) { rs_canon_free(out); return E_INVALIDARG; }
      for (UINT k = 0; k < count; ++k) {
        UINT w[6] = {0};
        for (UINT j = 0; j < range_stride / 4; ++j)
          if (!rs_u32(b, n, at + (SIZE_T)k * range_stride + 4 * (SIZE_T)j, &w[j])) { rs_canon_free(out); return E_INVALIDARG; }
        if (w[0] > D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) { rs_canon_free(out); return E_INVALIDARG; }
        D3D12_DESCRIPTOR_RANGE1 *r = &out->ranges[next_range + k];
        r->RangeType = w[0]; r->NumDescriptors = w[1]; r->BaseShaderRegister = w[2]; r->RegisterSpace = w[3];
        if (later) { r->Flags = w[4]; r->OffsetInDescriptorsFromTableStart = w[5]; }
        else { r->Flags = rs_volatile_range_flags(w[0]); r->OffsetInDescriptorsFromTableStart = w[4]; }
      }
      p->DescriptorTable.NumDescriptorRanges = count;
      p->DescriptorTable.pDescriptorRanges = count ? &out->ranges[next_range] : NULL;
      next_range += count;
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      UINT reg, space, values;
      if (!rs_u32(b, n, payload, &reg) || !rs_u32(b, n, (SIZE_T)payload + 4, &space) || !rs_u32(b, n, (SIZE_T)payload + 8, &values)) { rs_canon_free(out); return E_INVALIDARG; }
      p->Constants.ShaderRegister = reg; p->Constants.RegisterSpace = space; p->Constants.Num32BitValues = values;
    } else {
      UINT reg, space, dflags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
      if (!rs_u32(b, n, payload, &reg) || !rs_u32(b, n, (SIZE_T)payload + 4, &space) || (later && !rs_u32(b, n, (SIZE_T)payload + 8, &dflags))) { rs_canon_free(out); return E_INVALIDARG; }
      p->Descriptor.ShaderRegister = reg; p->Descriptor.RegisterSpace = space; p->Descriptor.Flags = dflags;
    }
  }
  for (UINT i = 0; i < ns; ++i) {
    UINT w[14] = {0};
    for (UINT j = 0; j < sampler_words; ++j)
      if (!rs_u32(b, n, samplers_at + (SIZE_T)i * sampler_words * 4 + 4 * (SIZE_T)j, &w[j])) { rs_canon_free(out); return E_INVALIDARG; }
    D3D12_STATIC_SAMPLER_DESC1 *s = &out->samplers[i];
    s->Filter = w[0]; s->AddressU = w[1]; s->AddressV = w[2]; s->AddressW = w[3]; s->MipLODBias = rs_float(w[4]); s->MaxAnisotropy = w[5]; s->ComparisonFunc = w[6];
    s->BorderColor = w[7]; s->MinLOD = rs_float(w[8]); s->MaxLOD = rs_float(w[9]); s->ShaderRegister = w[10]; s->RegisterSpace = w[11]; s->ShaderVisibility = w[12];
    s->Flags = w[13];
  }
  return S_OK;
}

/* ---- descriptions of each version, made from the one form ---------------------------------------------------------------------------------------------------------------- */

typedef struct RsVersioned {
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 v;      /* (the Agility SDK's layout; mingw's D3D12_VERSIONED_ROOT_SIGNATURE_DESC is its first part) */
  void *memory;
  BOOL made;
} RsVersioned;

/* The description at `version` (1.2's points into the one form itself; the others have arrays of their own). */
static HRESULT rs_describe(const RsCanon *c, D3D_ROOT_SIGNATURE_VERSION version, RsVersioned *out) {
  memset(out, 0, sizeof *out);
  out->v.Version = version;
  if (version == D3D_ROOT_SIGNATURE_VERSION_1_2) {
    out->v.Desc_1_2.NumParameters = c->np;
    out->v.Desc_1_2.pParameters = c->np ? c->params : NULL;
    out->v.Desc_1_2.NumStaticSamplers = c->ns;
    out->v.Desc_1_2.pStaticSamplers = c->ns ? c->samplers : NULL;
    out->v.Desc_1_2.Flags = c->flags;
    out->made = TRUE;
    return S_OK;
  }
  for (UINT i = 0; i < c->ns; ++i)
    if (c->samplers[i].Flags) return E_INVALIDARG;      /* (a sampler flag has no place in a 1.0 or 1.1 description) */
  const BOOL v10 = version == D3D_ROOT_SIGNATURE_VERSION_1_0;
  const SIZE_T param_bytes = v10 ? sizeof(D3D12_ROOT_PARAMETER) * (SIZE_T)c->np : 0, range_bytes = v10 ? sizeof(D3D12_DESCRIPTOR_RANGE) * (SIZE_T)c->range_count : 0;
  BYTE *m = mem_alloc(param_bytes + range_bytes + sizeof(D3D12_STATIC_SAMPLER_DESC) * (SIZE_T)c->ns + 1);
  if (!m) return E_OUTOFMEMORY;
  out->memory = m;
  D3D12_STATIC_SAMPLER_DESC *samplers = (D3D12_STATIC_SAMPLER_DESC *)(m + param_bytes + range_bytes);
  for (UINT i = 0; i < c->ns; ++i) memcpy(&samplers[i], &c->samplers[i], sizeof samplers[i]);      /* (the 1.2 sampler is the older one and its flags) */
  if (v10) {
    D3D12_ROOT_PARAMETER *params = (D3D12_ROOT_PARAMETER *)m;
    D3D12_DESCRIPTOR_RANGE *ranges = (D3D12_DESCRIPTOR_RANGE *)(m + param_bytes);
    UINT next = 0;
    for (UINT i = 0; i < c->np; ++i) {
      const D3D12_ROOT_PARAMETER1 *s = &c->params[i];
      D3D12_ROOT_PARAMETER *d = &params[i];
      d->ParameterType = s->ParameterType;
      d->ShaderVisibility = s->ShaderVisibility;
      if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const UINT count = s->DescriptorTable.NumDescriptorRanges;
        for (UINT k = 0; k < count; ++k) {
          const D3D12_DESCRIPTOR_RANGE1 *sr = &s->DescriptorTable.pDescriptorRanges[k];
          D3D12_DESCRIPTOR_RANGE *r = &ranges[next + k];
          r->RangeType = sr->RangeType; r->NumDescriptors = sr->NumDescriptors; r->BaseShaderRegister = sr->BaseShaderRegister; r->RegisterSpace = sr->RegisterSpace;
          r->OffsetInDescriptorsFromTableStart = sr->OffsetInDescriptorsFromTableStart;
        }
        d->DescriptorTable.NumDescriptorRanges = count;
        d->DescriptorTable.pDescriptorRanges = count ? &ranges[next] : NULL;
        next += count;
      } else if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        d->Constants = s->Constants;
      } else {
        d->Descriptor.ShaderRegister = s->Descriptor.ShaderRegister;
        d->Descriptor.RegisterSpace = s->Descriptor.RegisterSpace;
      }
    }
    out->v.Desc_1_0.NumParameters = c->np;
    out->v.Desc_1_0.pParameters = c->np ? params : NULL;
    out->v.Desc_1_0.NumStaticSamplers = c->ns;
    out->v.Desc_1_0.pStaticSamplers = c->ns ? samplers : NULL;
    out->v.Desc_1_0.Flags = c->flags;
  } else {
    out->v.Desc_1_1.NumParameters = c->np;
    out->v.Desc_1_1.pParameters = c->np ? c->params : NULL;
    out->v.Desc_1_1.NumStaticSamplers = c->ns;
    out->v.Desc_1_1.pStaticSamplers = c->ns ? samplers : NULL;
    out->v.Desc_1_1.Flags = c->flags;
  }
  out->made = TRUE;
  return S_OK;
}

/* ---- the serializer ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The one form of a description (`version` the description's own; `volatile_up`: a 1.0 description's ranges and root descriptors get the flags a 1.0 root signature
 * promises - for a description read back at a later version; written by D3D12SerializeRootSignature at 1.1 they are 0, as they were given). The arrays are copied. */
static HRESULT rs_canon_of_desc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *desc, BOOL volatile_up, RsCanon *out) {
  memset(out, 0, sizeof *out);
  const D3D_ROOT_SIGNATURE_VERSION version = desc->Version;
  if (version != D3D_ROOT_SIGNATURE_VERSION_1_0 && version != D3D_ROOT_SIGNATURE_VERSION_1_1 && version != D3D_ROOT_SIGNATURE_VERSION_1_2) return E_INVALIDARG;
  const BOOL v10 = version == D3D_ROOT_SIGNATURE_VERSION_1_0;
  /* (1.1 and 1.2 have the same parameters) */
  const UINT np = v10 ? desc->Desc_1_0.NumParameters : desc->Desc_1_1.NumParameters, ns = v10 ? desc->Desc_1_0.NumStaticSamplers : desc->Desc_1_1.NumStaticSamplers;
  const void *pp = v10 ? (const void *)desc->Desc_1_0.pParameters : (const void *)desc->Desc_1_1.pParameters;
  const void *ps = v10 ? (const void *)desc->Desc_1_0.pStaticSamplers : (const void *)desc->Desc_1_1.pStaticSamplers;
  if ((np && !pp) || (ns && !ps) || np > 64 || ns > 2032) return E_INVALIDARG;
  UINT ranges = 0;
  for (UINT i = 0; i < np; ++i) {
    const UINT type = v10 ? desc->Desc_1_0.pParameters[i].ParameterType : desc->Desc_1_1.pParameters[i].ParameterType;
    if (type > D3D12_ROOT_PARAMETER_TYPE_UAV) return E_INVALIDARG;
    if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    const UINT count = v10 ? desc->Desc_1_0.pParameters[i].DescriptorTable.NumDescriptorRanges : desc->Desc_1_1.pParameters[i].DescriptorTable.NumDescriptorRanges;
    const void *r = v10 ? (const void *)desc->Desc_1_0.pParameters[i].DescriptorTable.pDescriptorRanges : (const void *)desc->Desc_1_1.pParameters[i].DescriptorTable.pDescriptorRanges;
    if ((count && !r) || count > 4096 || ranges > 65536 - count) return E_INVALIDARG;
    ranges += count;
  }
  if (!rs_canon_alloc(out, np, ranges, ns)) return E_OUTOFMEMORY;
  out->version = version;
  out->flags = v10 ? desc->Desc_1_0.Flags : desc->Desc_1_1.Flags;
  UINT next = 0;
  for (UINT i = 0; i < np; ++i) {
    D3D12_ROOT_PARAMETER1 *d = &out->params[i];
    if (v10) {
      const D3D12_ROOT_PARAMETER *s = &desc->Desc_1_0.pParameters[i];
      d->ParameterType = s->ParameterType;
      d->ShaderVisibility = s->ShaderVisibility;
      if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const UINT count = s->DescriptorTable.NumDescriptorRanges;
        for (UINT k = 0; k < count; ++k) {
          const D3D12_DESCRIPTOR_RANGE *sr = &s->DescriptorTable.pDescriptorRanges[k];
          D3D12_DESCRIPTOR_RANGE1 *r = &out->ranges[next + k];
          r->RangeType = sr->RangeType; r->NumDescriptors = sr->NumDescriptors; r->BaseShaderRegister = sr->BaseShaderRegister; r->RegisterSpace = sr->RegisterSpace;
          r->OffsetInDescriptorsFromTableStart = sr->OffsetInDescriptorsFromTableStart;
          r->Flags = volatile_up ? rs_volatile_range_flags(sr->RangeType) : 0;
        }
        d->DescriptorTable.NumDescriptorRanges = count;
        d->DescriptorTable.pDescriptorRanges = count ? &out->ranges[next] : NULL;
        next += count;
      } else if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        d->Constants = s->Constants;
      } else {
        d->Descriptor.ShaderRegister = s->Descriptor.ShaderRegister;
        d->Descriptor.RegisterSpace = s->Descriptor.RegisterSpace;
        d->Descriptor.Flags = volatile_up ? D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE : 0;
      }
    } else {
      const D3D12_ROOT_PARAMETER1 *s = &desc->Desc_1_1.pParameters[i];
      *d = *s;
      if (s->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const UINT count = s->DescriptorTable.NumDescriptorRanges;
        if (count) memcpy(&out->ranges[next], s->DescriptorTable.pDescriptorRanges, sizeof(D3D12_DESCRIPTOR_RANGE1) * (SIZE_T)count);
        d->DescriptorTable.pDescriptorRanges = count ? &out->ranges[next] : NULL;
        next += count;
      }
    }
  }
  for (UINT i = 0; i < ns; ++i) {
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_2) out->samplers[i] = desc->Desc_1_2.pStaticSamplers[i];
    else memcpy(&out->samplers[i], v10 ? &desc->Desc_1_0.pStaticSamplers[i] : &desc->Desc_1_1.pStaticSamplers[i], sizeof(D3D12_STATIC_SAMPLER_DESC));
  }
  return S_OK;
}

/* What the serializer refuses in a static sampler: a border colour outside the five, the integer border colours before 1.2 or without D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR
 * (and that flag with a float colour), flags that do not exist. */
static BOOL rs_sampler_ok(const D3D12_STATIC_SAMPLER_DESC1 *s, D3D_ROOT_SIGNATURE_VERSION version) {
  const UINT known = D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR | D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
  if ((UINT)s->BorderColor > (UINT)D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT || (s->Flags & ~known)) return FALSE;
  if (s->ShaderVisibility > D3D12_SHADER_VISIBILITY_MESH) return FALSE;
  const BOOL uint_colour = s->BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT || s->BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT;
  if (version != D3D_ROOT_SIGNATURE_VERSION_1_2) return !uint_colour;
  return uint_colour == ((s->Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) != 0);
}

typedef struct Bytes { BYTE *p; SIZE_T size, cap; int bad; } Bytes;
static void bytes_put(Bytes *b, UINT v) {
  if (b->bad) return;
  if (b->size + 4 > b->cap) {
    const SIZE_T cap = b->cap ? b->cap * 2 : 256;
    BYTE *n = mem_grow(b->p, cap);
    if (!n) { b->bad = 1; return; }
    b->p = n; b->cap = cap;
  }
  memcpy(b->p + b->size, &v, 4);
  b->size += 4;
}

/* The one form written as a root signature of `version`: parameter payloads follow the parameter array, then the ranges, then the static samplers. */
static Blob *rs_write(const RsCanon *c, D3D_ROOT_SIGNATURE_VERSION version) {
  Bytes b = {0};
  const BOOL later = version != D3D_ROOT_SIGNATURE_VERSION_1_0, v12 = version == D3D_ROOT_SIGNATURE_VERSION_1_2;
  const UINT range_stride = later ? 24 : 20;
  UINT payload_bytes = 0, range_bytes = 0;
  for (UINT i = 0; i < c->np; ++i) {
    const UINT type = c->params[i].ParameterType;
    if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      payload_bytes += 8;
      range_bytes += c->params[i].DescriptorTable.NumDescriptorRanges * range_stride;
    } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      payload_bytes += 12;
    } else {
      payload_bytes += later ? 12 : 8;
    }
  }
  const UINT params_at = 24, payloads_at = params_at + c->np * 12, ranges_at = payloads_at + payload_bytes, samplers_at = ranges_at + range_bytes;
  bytes_put(&b, (UINT)version);
  bytes_put(&b, c->np);
  bytes_put(&b, params_at);
  bytes_put(&b, c->ns);
  bytes_put(&b, samplers_at);
  bytes_put(&b, c->flags);
  UINT payload = payloads_at;
  for (UINT i = 0; i < c->np; ++i) {
    const UINT type = c->params[i].ParameterType;
    bytes_put(&b, type);
    bytes_put(&b, c->params[i].ShaderVisibility);
    bytes_put(&b, payload);
    payload += type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 8 : type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? 12 : (later ? 12 : 8);
  }
  UINT range_at = ranges_at;
  for (UINT i = 0; i < c->np; ++i) {
    const D3D12_ROOT_PARAMETER1 *p = &c->params[i];
    if (p->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      bytes_put(&b, p->DescriptorTable.NumDescriptorRanges);
      bytes_put(&b, range_at);
      range_at += p->DescriptorTable.NumDescriptorRanges * range_stride;
    } else if (p->ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      bytes_put(&b, p->Constants.ShaderRegister); bytes_put(&b, p->Constants.RegisterSpace); bytes_put(&b, p->Constants.Num32BitValues);
    } else {
      bytes_put(&b, p->Descriptor.ShaderRegister); bytes_put(&b, p->Descriptor.RegisterSpace);
      if (later) bytes_put(&b, p->Descriptor.Flags);
    }
  }
  for (UINT i = 0; i < c->np; ++i) {
    const D3D12_ROOT_PARAMETER1 *p = &c->params[i];
    if (p->ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
    for (UINT k = 0; k < p->DescriptorTable.NumDescriptorRanges; ++k) {
      const D3D12_DESCRIPTOR_RANGE1 *r = &p->DescriptorTable.pDescriptorRanges[k];
      bytes_put(&b, r->RangeType); bytes_put(&b, r->NumDescriptors); bytes_put(&b, r->BaseShaderRegister); bytes_put(&b, r->RegisterSpace);
      if (later) bytes_put(&b, r->Flags);
      bytes_put(&b, r->OffsetInDescriptorsFromTableStart);
    }
  }
  for (UINT i = 0; i < c->ns; ++i) {
    const D3D12_STATIC_SAMPLER_DESC1 *s = &c->samplers[i];
    bytes_put(&b, s->Filter); bytes_put(&b, s->AddressU); bytes_put(&b, s->AddressV); bytes_put(&b, s->AddressW);
    bytes_put(&b, bits_of(s->MipLODBias)); bytes_put(&b, s->MaxAnisotropy); bytes_put(&b, s->ComparisonFunc); bytes_put(&b, s->BorderColor);
    bytes_put(&b, bits_of(s->MinLOD)); bytes_put(&b, bits_of(s->MaxLOD)); bytes_put(&b, s->ShaderRegister); bytes_put(&b, s->RegisterSpace); bytes_put(&b, s->ShaderVisibility);
    if (v12) bytes_put(&b, s->Flags);
  }
  if (b.bad) { mem_free(b.p); return NULL; }
  Blob *blob = blob_new(b.p, b.size);
  mem_free(b.p);
  return blob;
}

/* A description serialized at `version` (the description's own, or 1.1 for a 1.0 description through D3D12SerializeRootSignature). */
static HRESULT rs_serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob) {
  RsCanon c;
  HRESULT hr = rs_canon_of_desc(desc, FALSE, &c);
  if (FAILED(hr)) return hr;
  for (UINT i = 0; i < c.ns; ++i)
    if (!rs_sampler_ok(&c.samplers[i], desc->Version)) { rs_canon_free(&c); return E_INVALIDARG; }
  Blob *b = rs_write(&c, version);
  rs_canon_free(&c);
  if (!b) return E_OUTOFMEMORY;
  *blob = (ID3DBlob *)b;
  return S_OK;
}

EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error) {
  if (blob) *blob = NULL;
  if (error) *error = NULL;
  if (!desc || !blob || (version != D3D_ROOT_SIGNATURE_VERSION_1_0 && version != D3D_ROOT_SIGNATURE_VERSION_1_1)) return E_INVALIDARG;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 v;
  memset(&v, 0, sizeof v);
  v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  v.Desc_1_0 = *desc;
  return rs_serialize(&v, version, blob);
}

EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **error) {
  if (blob) *blob = NULL;
  if (error) *error = NULL;
  if (!desc || !blob) return E_INVALIDARG;
  /* (the program's description is the Agility SDK's, with Desc_1_2 in the union: read as such for 1.2, and the 1.0 / 1.1 parts are where mingw has them) */
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 v;
  memset(&v, 0, sizeof v);
  v.Version = desc->Version;
  if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) v.Desc_1_0 = desc->Desc_1_0;
  else if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) v.Desc_1_1 = desc->Desc_1_1;
  else if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_2) v.Desc_1_2 = ((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *)desc)->Desc_1_2;
  else return E_INVALIDARG;
  return rs_serialize(&v, desc->Version, blob);
}

/* ---- the deserializers ------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The deserializer objects: both kinds are the one structure (the plain one gives the 1.0 description). */
typedef struct RsDeserializer {
  DxObj o;
  RsCanon canon;
  RsVersioned at[3];      /* the descriptions of 1.0, 1.1 and 1.2, made when asked for */
  CRITICAL_SECTION lock;
} RsDeserializer;
static const GUID *const g_rsd_iids[] = {&IID_ID3D12RootSignatureDeserializer, NULL};
static const GUID *const g_vrsd_iids[] = {&IID_ID3D12VersionedRootSignatureDeserializer, NULL};
static void rsd_destroy(DxObj *o) {
  RsDeserializer *d = (RsDeserializer *)o;
  for (UINT i = 0; i < 3; ++i) mem_free(d->at[i].memory);
  rs_canon_free(&d->canon);
  DeleteCriticalSection(&d->lock);
}
static HRESULT rsd_at_version(RsDeserializer *d, D3D_ROOT_SIGNATURE_VERSION version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 **out) {
  if (version < D3D_ROOT_SIGNATURE_VERSION_1_0 || version > D3D_ROOT_SIGNATURE_VERSION_1_2) return E_INVALIDARG;
  RsVersioned *v = &d->at[version - 1];
  EnterCriticalSection(&d->lock);
  const HRESULT hr = v->made ? S_OK : rs_describe(&d->canon, version, v);
  LeaveCriticalSection(&d->lock);
  *out = SUCCEEDED(hr) ? &v->v : NULL;
  return hr;
}
static const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE rsd_GetRootSignatureDesc(RsDeserializer *d) {
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *v;
  return SUCCEEDED(rsd_at_version(d, D3D_ROOT_SIGNATURE_VERSION_1_0, &v)) ? &v->Desc_1_0 : NULL;
}
static HRESULT STDMETHODCALLTYPE vrsd_GetRootSignatureDescAtVersion(RsDeserializer *d, D3D_ROOT_SIGNATURE_VERSION version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *v;
  const HRESULT hr = rsd_at_version(d, version, &v);
  if (SUCCEEDED(hr)) *out = (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)v;
  return hr;
}
static const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE vrsd_GetUnconvertedRootSignatureDesc(RsDeserializer *d) {
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *v;
  return SUCCEEDED(rsd_at_version(d, d->canon.version, &v)) ? (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)v : NULL;
}

/* A DXIL library's root signature subobject (GlobalRootSignature / LocalRootSignature) of that name: its bytes. The RDAT part is a header (version 0x10, part count, the
 * parts' offsets) and parts of a type and size each: the string buffer (1), the raw bytes (5) and the subobject table (6: a record count and stride, then records of the
 * subobject kind, the name's offset in the strings and, for a root signature, the offset and size of its bytes in the raw bytes). Everything read is checked. */
static BOOL rs_library_subobject(const void *data, SIZE_T size, LPCWSTR name, const BYTE **out, SIZE_T *out_size) {
  const BYTE *b = data, *rdat = NULL;
  SIZE_T rdat_size = 0;
  UINT total, parts;
  if (!b || size < 32 || memcmp(b, "DXBC", 4) != 0 || !name) return FALSE;
  if (!rs_u32(b, size, 24, &total) || !rs_u32(b, size, 28, &parts) || total > size || parts > 64) return FALSE;
  for (UINT i = 0; i < parts && !rdat; ++i) {
    UINT at, length;
    if (!rs_u32(b, size, 32 + 4 * (SIZE_T)i, &at) || !rs_u32(b, size, (SIZE_T)at + 4, &length)) return FALSE;
    if (at > size - 8 || length > size - at - 8) return FALSE;
    if (memcmp(b + at, "RDAT", 4) == 0) { rdat = b + at + 8; rdat_size = length; }
  }
  UINT version, count;
  if (!rdat || !rs_u32(rdat, rdat_size, 0, &version) || !rs_u32(rdat, rdat_size, 4, &count) || version != 0x10 || count > 64) return FALSE;
  const BYTE *strings = NULL, *raw = NULL, *table = NULL;
  SIZE_T strings_size = 0, raw_size = 0, table_size = 0;
  for (UINT i = 0; i < count; ++i) {
    UINT at, type, length;
    if (!rs_u32(rdat, rdat_size, 8 + 4 * (SIZE_T)i, &at) || !rs_u32(rdat, rdat_size, at, &type) || !rs_u32(rdat, rdat_size, (SIZE_T)at + 4, &length)) return FALSE;
    if (length > rdat_size - at - 8) return FALSE;
    const BYTE *body = rdat + at + 8;
    if (type == 1) { strings = body; strings_size = length; }
    else if (type == 5) { raw = body; raw_size = length; }
    else if (type == 6) { table = body; table_size = length; }
  }
  UINT records, stride;
  if (!strings || !table || !rs_u32(table, table_size, 0, &records) || !rs_u32(table, table_size, 4, &stride) || stride < 16 || stride % 4) return FALSE;
  if (records > (table_size - 8) / stride) return FALSE;
  for (UINT r = 0; r < records; ++r) {
    const SIZE_T at = 8 + (SIZE_T)r * stride;
    UINT kind, name_at, bytes_at, bytes_size;
    if (!rs_u32(table, table_size, at, &kind) || !rs_u32(table, table_size, at + 4, &name_at) || !rs_u32(table, table_size, at + 8, &bytes_at) ||
        !rs_u32(table, table_size, at + 12, &bytes_size))
      return FALSE;
    if ((kind != 1 && kind != 2) || name_at >= strings_size) continue;      /* (1 global, 2 local root signature) */
    /* (the name: UTF-8 in the strings, compared with the wide name - root signature names are ASCII identifiers) */
    SIZE_T k = 0;
    BOOL same = TRUE;
    for (;; ++k) {
      if (name_at + k >= strings_size) { same = FALSE; break; }
      const BYTE ch = strings[name_at + k];
      if (ch != name[k]) { same = FALSE; break; }
      if (!ch) break;
    }
    if (!same) continue;
    if (!raw || bytes_at > raw_size || bytes_size > raw_size - bytes_at) return FALSE;
    *out = raw + bytes_at;
    *out_size = bytes_size;
    return TRUE;
  }
  return FALSE;
}

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
  HRESULT hr = rs_parse(data, size, &d->canon);
  /* (the plain deserializer answers with the 1.0 description: one it cannot make is a root signature it does not take) */
  if (SUCCEEDED(hr) && !versioned) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC2 *v;
    hr = rsd_at_version(d, D3D_ROOT_SIGNATURE_VERSION_1_0, &v);
  }
  if (FAILED(hr)) { dx_Release(OBJ(d)); return hr; }
  const HRESULT q = dx_QueryInterface(OBJ(d), riid, out);
  dx_Release(OBJ(d));
  return q;
}
EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return rsd_make(data, size, FALSE, riid, out); }
EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **out) { return rsd_make(data, size, TRUE, riid, out); }
EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(const void *data, SIZE_T size, LPCWSTR name, REFIID riid, void **out) {
  if (out) *out = NULL;
  const BYTE *bytes;
  SIZE_T n;
  if (!out) return E_POINTER;
  if (!rs_library_subobject(data, size, name, &bytes, &n)) return E_INVALIDARG;
  return rsd_make(bytes, n, TRUE, riid, out);
}

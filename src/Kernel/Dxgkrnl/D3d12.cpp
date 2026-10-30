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

#include "Kernel/Dxgkrnl/D3d12.hpp"

namespace dxgk {

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) { return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 | static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24; }

float word_float(uint32_t bits) { return __builtin_bit_cast(float, bits); }

// A reader at `offset` of the blob.
bool reader_at(ConstByteView blob, uint64_t offset, ByteReader* out) {
  ConstByteView rest;
  if (offset > blob.size() || !blob.try_subview(offset, blob.size() - offset, &rest)) return false;
  *out = ByteReader(rest);
  return true;
}

graphics::AddressMode address_of(uint32_t mode) { return mode == 1 || mode == 2 ? graphics::AddressMode::Repeat : graphics::AddressMode::ClampToEdge; }   // (wrap and mirror; clamp, border and mirror once)

NtStatus parse_blob(ConstByteView blob, RootSignatureDesc* out) {
  ByteReader head(blob);
  const uint32_t version = head.le32(), parameter_count = head.le32(), parameters_offset = head.le32(), sampler_count = head.le32(), samplers_offset = head.le32();
  const uint32_t flags = version == 1 || version == 2 ? head.le32() : 0;
  if (!head.ok() || (version != 1 && version != 2)) return status::kInvalidParameter;
  if (parameter_count > kMaxRootParameters || sampler_count > kMaxStaticSamplers) return status::kInvalidParameter;
  const bool v11 = version == 2;
  out->flags = flags;
  out->parameter_count = parameter_count;
  out->sampler_count = sampler_count;
  uint32_t dwords = 0;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    ByteReader at;
    if (!reader_at(blob, uint64_t{parameters_offset} + uint64_t{i} * 12, &at)) return status::kInvalidParameter;
    const uint32_t type = at.le32(), visibility = at.le32(), payload = at.le32();
    if (!at.ok() || type > 4 || visibility > 5) return status::kInvalidParameter;
    RootParameter& param = out->parameters[i];
    param.type = static_cast<RootParameterType>(type);
    param.visibility = visibility;
    ByteReader body;
    if (!reader_at(blob, payload, &body)) return status::kInvalidParameter;
    if (param.type == RootParameterType::Table) {
      const uint32_t range_count = body.le32(), ranges_offset = body.le32();
      if (!body.ok() || range_count == 0 || range_count > kMaxTableRanges) return status::kInvalidParameter;
      param.range_count = range_count;
      const uint32_t stride = v11 ? 24 : 20;
      uint64_t end = 0;
      bool sampler_seen = false, view_seen = false;
      for (uint32_t r = 0; r < range_count; ++r) {
        ByteReader range;
        if (!reader_at(blob, uint64_t{ranges_offset} + uint64_t{r} * stride, &range)) return status::kInvalidParameter;
        const uint32_t range_type = range.le32(), count = range.le32(), base = range.le32(), space = range.le32();
        if (v11) range.le32();   // (flags)
        const uint32_t offset = range.le32();
        if (!range.ok() || range_type > 3 || count == 0) return status::kInvalidParameter;
        if (end == kUnboundedDescriptors) return status::kInvalidParameter;   // (an unbounded range is the last)
        DescriptorRange& dr = param.ranges[r];
        dr.type = static_cast<RangeType>(range_type);
        dr.count = count;
        dr.base_register = base;
        dr.space = space;
        const uint64_t start = offset == 0xFFFFFFFFu ? end : offset;
        if (start > kMaxHeapDescriptors * 256ull) return status::kInvalidParameter;
        dr.offset = static_cast<uint32_t>(start);
        end = count == kUnboundedDescriptors ? kUnboundedDescriptors : start + count;
        if (count != kUnboundedDescriptors && end > (1ull << 24)) return status::kInvalidParameter;
        (dr.type == RangeType::Sampler ? sampler_seen : view_seen) = true;
      }
      if (sampler_seen && view_seen) return status::kInvalidParameter;   // (a table points into one heap)
      param.sampler_table = sampler_seen;
      param.table_size = end == kUnboundedDescriptors ? kUnboundedDescriptors : static_cast<uint32_t>(end);
      dwords += 1;
    } else if (param.type == RootParameterType::Constants) {
      param.shader_register = body.le32();
      param.space = body.le32();
      param.constant_count = body.le32();
      if (!body.ok() || param.constant_count == 0 || param.constant_count > kMaxRootDwords) return status::kInvalidParameter;
      dwords += param.constant_count;
    } else {
      param.shader_register = body.le32();
      param.space = body.le32();
      if (v11) body.le32();   // (flags)
      if (!body.ok()) return status::kInvalidParameter;
      dwords += 2;
    }
    if (dwords > kMaxRootDwords) return status::kInvalidParameter;
  }
  out->dwords = dwords;
  for (uint32_t i = 0; i < sampler_count; ++i) {
    ByteReader at;
    if (!reader_at(blob, uint64_t{samplers_offset} + uint64_t{i} * 52, &at)) return status::kInvalidParameter;
    const uint32_t filter = at.le32(), address_u = at.le32(), address_v = at.le32();
    at.le32();   // (address w)
    const float bias = word_float(at.le32());
    at.le32();   // (anisotropy)
    at.le32();   // (comparison)
    at.le32();   // (border colour)
    const float min_lod = word_float(at.le32()), max_lod = word_float(at.le32());
    const uint32_t reg = at.le32(), space = at.le32(), visibility = at.le32();
    if (!at.ok() || address_u < 1 || address_u > 5 || address_v < 1 || address_v > 5 || visibility > 5) return status::kInvalidParameter;
    StaticSampler& s = out->samplers[i];
    // (the filter: bit 0 mip, bit 2 mag, bit 4 min; anisotropic filtering (0x55) and comparison filters are the linear ones)
    s.sampler.filter = (filter & 0x4) || filter == 0x55 || filter == 0xD5 ? graphics::Filter::Linear : graphics::Filter::Nearest;
    s.sampler.mip_filter = filter & 0x1 ? graphics::MipFilter::Linear : graphics::MipFilter::Nearest;
    s.sampler.address_u = address_of(address_u);
    s.sampler.address_v = address_of(address_v);
    s.sampler.lod_bias = bias == bias && bias > -100.0f && bias < 100.0f ? bias : 0.0f;
    s.sampler.min_lod = min_lod == min_lod && min_lod > -100.0f && min_lod < 1000.0f ? min_lod : 0.0f;
    s.sampler.max_lod = max_lod == max_lod && max_lod > -100.0f && max_lod < 1000.0f ? max_lod : 1000.0f;
    s.shader_register = reg;
    s.space = space;
    s.visibility = visibility;
  }
  return status::kSuccess;
}

}  // namespace

NtStatus parse_root_signature(ConstByteView blob, RootSignatureDesc* out) {
  if (blob.size() < 24 || blob.size() > kMaxRootSignatureBytes) return status::kInvalidParameter;
  *out = RootSignatureDesc();
  ByteReader in(blob);
  if (in.le32() == fourcc('D', 'X', 'B', 'C')) {
    // A DXBC container: 16 bytes of hash, a version, the size, the chunk count and offsets; the RTS0 chunk is the root signature.
    in.skip(16);
    in.le32();
    in.le32();
    const uint32_t chunks = in.le32();
    if (!in.ok() || chunks > 32) return status::kInvalidParameter;
    for (uint32_t c = 0; c < chunks; ++c) {
      const uint32_t offset = in.le32();
      ByteReader chunk;
      if (!in.ok() || !reader_at(blob, offset, &chunk)) return status::kInvalidParameter;
      const uint32_t tag = chunk.le32(), size = chunk.le32();
      if (!chunk.ok()) return status::kInvalidParameter;
      if (tag != fourcc('R', 'T', 'S', '0')) continue;
      ConstByteView body;
      if (uint64_t{offset} + 8 > blob.size() || !blob.try_subview(uint64_t{offset} + 8, size, &body)) return status::kInvalidParameter;
      return parse_blob(body, out);
    }
    return status::kInvalidParameter;
  }
  return parse_blob(blob, out);
}

}  // namespace dxgk

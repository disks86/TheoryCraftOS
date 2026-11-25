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

#include "Graphics/Graphics3D/Venus/Spirv.hpp"

namespace graphics::spirv {
namespace {
// (the keys of the type and constant cache: what kind of thing, then its words)
enum KeyKind : uint32_t { kKeyVoid = 1, kKeyBool, kKeyInt, kKeyFloat, kKeyVector, kKeyPointer, kKeyArray, kKeyFunction, kKeySampler, kKeyImage, kKeySampledImage,
                          kKeyConstant, kKeyComposite, kKeyNull, kKeyBoolConstant };
}  // namespace

Builder::Builder(allocators::IAllocator& allocator)
    : allocator_(allocator),
      sections_{List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator),
                List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator), List<uint32_t>(allocator),
                List<uint32_t>(allocator)},
      cache_(allocator),
      interface_(allocator),
      capabilities_(allocator) {}

void Builder::put(Section s, uint32_t word) {
  if (!sections_[s].push_back(word)) ok_ = false;
}

void Builder::op(Section s, uint16_t opcode, std::initializer_list<uint32_t> words) {
  put(s, static_cast<uint32_t>(words.size() + 1) << 16 | opcode);
  for (uint32_t w : words) put(s, w);
}

void Builder::op_list(Section s, uint16_t opcode, const uint32_t* words, uint32_t count) {
  put(s, (count + 1) << 16 | opcode);
  for (uint32_t i = 0; i < count; ++i) put(s, words[i]);
}

void Builder::op_string(Section s, uint16_t opcode, std::initializer_list<uint32_t> before, StringView text, const uint32_t* after, uint32_t after_count) {
  const uint32_t string_words = static_cast<uint32_t>(text.size() / 4 + 1);   // (always at least one zero byte at the end)
  put(s, static_cast<uint32_t>(1 + before.size() + string_words + after_count) << 16 | opcode);
  for (uint32_t w : before) put(s, w);
  for (uint32_t i = 0; i < string_words; ++i) {
    uint32_t w = 0;
    for (uint32_t b = 0; b < 4; ++b) {
      const uint64_t at = uint64_t{i} * 4 + b;
      if (at < text.size()) w |= static_cast<uint32_t>(static_cast<uint8_t>(text[at])) << (8 * b);
    }
    put(s, w);
  }
  for (uint32_t i = 0; i < after_count; ++i) put(s, after[i]);
}

void Builder::capability(uint32_t cap) {
  for (uint32_t c : capabilities_)
    if (c == cap) return;
  if (!capabilities_.push_back(cap)) ok_ = false;
  op(kCapabilities, OpCapability, {cap});
}

void Builder::extension(StringView name) { op_string(kExtensions, OpExtension, {}, name, nullptr, 0); }

void Builder::decorate(uint32_t target, uint32_t decoration, std::initializer_list<uint32_t> values) {
  put(kAnnotations, static_cast<uint32_t>(3 + values.size()) << 16 | OpDecorate);
  put(kAnnotations, target);
  put(kAnnotations, decoration);
  for (uint32_t v : values) put(kAnnotations, v);
}

void Builder::member_decorate(uint32_t type, uint32_t member, uint32_t decoration, std::initializer_list<uint32_t> values) {
  put(kAnnotations, static_cast<uint32_t>(4 + values.size()) << 16 | OpMemberDecorate);
  put(kAnnotations, type);
  put(kAnnotations, member);
  put(kAnnotations, decoration);
  for (uint32_t v : values) put(kAnnotations, v);
}

uint32_t Builder::find(const Key& k) const {
  for (const Cached& c : cache_)
    if (c.key == k) return c.id;
  return 0;
}

void Builder::remember(const Key& k, uint32_t id) {
  if (!cache_.push_back({k, id})) ok_ = false;
}

uint32_t Builder::t_void() {
  const Key k{kKeyVoid};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeVoid, {r});
  remember(k, r);
  return r;
}

uint32_t Builder::t_bool() {
  const Key k{kKeyBool};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeBool, {r});
  remember(k, r);
  return r;
}

uint32_t Builder::t_uint() {
  const Key k{kKeyInt, 0};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeInt, {r, 32, 0});
  remember(k, r);
  return r;
}

uint32_t Builder::t_int() {
  const Key k{kKeyInt, 1};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeInt, {r, 32, 1});
  remember(k, r);
  return r;
}

uint32_t Builder::t_float() {
  const Key k{kKeyFloat, 32};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeFloat, {r, 32});
  remember(k, r);
  return r;
}

uint32_t Builder::t_double() {
  const Key k{kKeyFloat, 64};
  if (uint32_t f = find(k)) return f;
  capability(CapFloat64);
  const uint32_t r = id();
  op(kGlobals, OpTypeFloat, {r, 64});
  remember(k, r);
  return r;
}

uint32_t Builder::t_vector(uint32_t component, uint32_t count) {
  if (count == 1) return component;
  const Key k{kKeyVector, component, count};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeVector, {r, component, count});
  remember(k, r);
  return r;
}

uint32_t Builder::t_pointer(uint32_t storage, uint32_t type) {
  const Key k{kKeyPointer, storage, type};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypePointer, {r, storage, type});
  remember(k, r);
  return r;
}

uint32_t Builder::t_array(uint32_t element, uint32_t length) {
  const Key k{kKeyArray, element, length};
  if (uint32_t f = find(k)) return f;
  const uint32_t n = c_uint(length);
  const uint32_t r = id();
  op(kGlobals, OpTypeArray, {r, element, n});
  remember(k, r);
  return r;
}

uint32_t Builder::t_function_void() {
  const Key k{kKeyFunction};
  if (uint32_t f = find(k)) return f;
  const uint32_t v = t_void();
  const uint32_t r = id();
  op(kGlobals, OpTypeFunction, {r, v});
  remember(k, r);
  return r;
}

uint32_t Builder::t_sampler() {
  const Key k{kKeySampler};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeSampler, {r});
  remember(k, r);
  return r;
}

uint32_t Builder::t_image(uint32_t sampled_type, uint32_t dim, bool arrayed, bool multisampled, uint32_t sampled, uint32_t format) {
  const Key k{kKeyImage, sampled_type, dim, arrayed ? 1u : 0u, multisampled ? 1u : 0u, sampled, format};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeImage, {r, sampled_type, dim, 0, arrayed ? 1u : 0u, multisampled ? 1u : 0u, sampled, format});
  remember(k, r);
  return r;
}

uint32_t Builder::t_sampled_image(uint32_t image) {
  const Key k{kKeySampledImage, image};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpTypeSampledImage, {r, image});
  remember(k, r);
  return r;
}

uint32_t Builder::constant(uint32_t type, uint32_t value) {
  const Key k{kKeyConstant, type, value};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpConstant, {type, r, value});
  remember(k, r);
  return r;
}

uint32_t Builder::c_uint(uint32_t v) { return constant(t_uint(), v); }

uint32_t Builder::c_float(float f) {
  uint32_t b;
  __builtin_memcpy(&b, &f, 4);
  return c_float_bits(b);
}

uint32_t Builder::c_double(double v) {
  uint64_t bits;
  __builtin_memcpy(&bits, &v, 8);
  const uint32_t t = t_double();
  const Key k{kKeyConstant, t, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32)};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpConstant, {t, r, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32)});
  remember(k, r);
  return r;
}

uint32_t Builder::c_bool(bool v) {
  const uint32_t t = t_bool();
  const Key k{kKeyBoolConstant, v ? 1u : 0u};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, v ? OpConstantTrue : OpConstantFalse, {t, r});
  remember(k, r);
  return r;
}

uint32_t Builder::c_composite(uint32_t type, std::initializer_list<uint32_t> parts) {
  Key k{kKeyComposite, type};
  uint32_t* slots[] = {&k.c, &k.d, &k.e, &k.f};
  uint32_t i = 0;
  for (uint32_t p : parts)
    if (i < 4) *slots[i++] = p;
  if (parts.size() > 4) {   // (not cached: longer composites are not made twice)
    const uint32_t r = id();
    put(kGlobals, static_cast<uint32_t>(3 + parts.size()) << 16 | OpConstantComposite);
    put(kGlobals, type);
    put(kGlobals, r);
    for (uint32_t p : parts) put(kGlobals, p);
    return r;
  }
  k.g = static_cast<uint32_t>(parts.size());
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  put(kGlobals, static_cast<uint32_t>(3 + parts.size()) << 16 | OpConstantComposite);
  put(kGlobals, type);
  put(kGlobals, r);
  for (uint32_t p : parts) put(kGlobals, p);
  remember(k, r);
  return r;
}

uint32_t Builder::c_uvec4(const uint32_t bits[4]) {
  const uint32_t t = t_vector(t_uint(), 4);
  return c_composite(t, {c_uint(bits[0]), c_uint(bits[1]), c_uint(bits[2]), c_uint(bits[3])});
}

uint32_t Builder::c_null(uint32_t type) {
  const Key k{kKeyNull, type};
  if (uint32_t f = find(k)) return f;
  const uint32_t r = id();
  op(kGlobals, OpConstantNull, {type, r});
  remember(k, r);
  return r;
}

uint32_t Builder::global(uint32_t storage, uint32_t type, uint32_t initializer) {
  const uint32_t p = t_pointer(storage, type);
  const uint32_t r = id();
  if (initializer) op(kGlobals, OpVariable, {p, r, storage, initializer});
  else op(kGlobals, OpVariable, {p, r, storage});
  if (!interface_.push_back(r)) ok_ = false;
  return r;
}

bool Builder::finish(List<uint32_t>* out) {
  if (!ok_) return false;
  out->clear();
  bool fits = out->push_back(0x07230203u) && out->push_back(0x00010500u) && out->push_back(0x54430000u) && out->push_back(next_) && out->push_back(0);
  for (const List<uint32_t>& s : sections_)
    for (uint32_t w : s) fits = fits && out->push_back(w);
  return fits;
}

}  // namespace graphics::spirv

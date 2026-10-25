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

// Byte layouts of shader values in buffers (std140 for GL uniform blocks, WGSL's rules for WebGPU), and moving values between those bytes and the flat slots.
#include "WebGl/Shader.hpp"

namespace shader {

namespace {
uint32_t round_up(uint32_t v, uint32_t a) { return a ? (v + a - 1) / a * a : v; }

void vec_layout(uint32_t n, uint32_t* align, uint32_t* size) {
  *size = 4 * n;
  *align = n == 1 ? 4 : (n == 2 ? 8 : 16);
}
}  // namespace

void layout_of(const Module& m, const Type& t, Layout kind, uint32_t* align, uint32_t* size) {
  const bool std140 = kind == Layout::Std140;
  const bool uniform_rules = kind != Layout::Natural;   // array strides round to 16
  uint32_t ea, es;
  if (t.array) {
    layout_of(m, t.element(), kind, &ea, &es);
    uint32_t stride = round_up(es, ea);
    if (uniform_rules) stride = round_up(stride, 16);
    *align = uniform_rules ? round_up(ea, 16) : ea;
    *size = stride * t.array;
    return;
  }
  if (t.base == Base::Struct) {
    const StructDef* sd = m.struct_of(t);
    uint32_t off = 0, maxa = 1;
    if (sd)
      for (uint64_t i = 0; i < sd->fields.count(); ++i) {
        layout_of(m, sd->fields[i].type, kind, &ea, &es);
        off = round_up(off, ea) + es;
        if (ea > maxa) maxa = ea;
      }
    if (uniform_rules) maxa = round_up(maxa, 16);
    *align = maxa;
    *size = round_up(off, maxa);
    return;
  }
  if (t.cols > 1) {   // matrix: an array of column vectors
    uint32_t ca, cs;
    vec_layout(t.rows, &ca, &cs);
    const uint32_t stride = std140 ? 16 : round_up(cs, ca);
    *align = std140 ? 16 : ca;
    *size = stride * t.cols;
    return;
  }
  vec_layout(t.rows, align, size);
}

uint32_t field_offset(const Module& m, const StructDef& sd, uint32_t field, Layout kind) {
  uint32_t off = 0, ea, es;
  for (uint32_t i = 0; i <= field && i < sd.fields.count(); ++i) {
    layout_of(m, sd.fields[i].type, kind, &ea, &es);
    off = round_up(off, ea);
    if (i == field) return off;
    off += es;
  }
  return off;
}

void load_layout(const Module& m, const Type& t, Layout kind, ConstByteView bytes, uint64_t offset, uint32_t* slots) {
  auto word = [&](uint64_t at) -> uint32_t {
    if (at + 4 > bytes.size()) return 0;
    uint32_t v;
    __builtin_memcpy(&v, bytes.data() + at, 4);
    return v;
  };
  uint32_t ea, es;
  const bool uniform_rules = kind != Layout::Natural;
  if (t.array) {
    layout_of(m, t.element(), kind, &ea, &es);
    uint32_t stride = round_up(es, ea);
    if (uniform_rules) stride = round_up(stride, 16);
    const uint32_t eslots = m.slots_of(t.element());
    offset = (offset + ea - 1) / ea * ea;
    for (uint32_t i = 0; i < t.array; ++i) load_layout(m, t.element(), kind, bytes, offset + uint64_t{i} * stride, slots + i * eslots);
    return;
  }
  if (t.base == Base::Struct) {
    const StructDef* sd = m.struct_of(t);
    if (!sd) return;
    layout_of(m, t, kind, &ea, &es);
    offset = (offset + ea - 1) / ea * ea;
    for (uint32_t i = 0; i < sd->fields.count(); ++i)
      load_layout(m, sd->fields[i].type, kind, bytes, offset + field_offset(m, *sd, i, kind), slots + sd->fields[i].offset);
    return;
  }
  if (t.cols > 1) {
    uint32_t ca, cs;
    vec_layout(t.rows, &ca, &cs);
    const uint32_t stride = kind == Layout::Std140 ? 16 : round_up(cs, ca);
    offset = (offset + (kind == Layout::Std140 ? 16 : ca) - 1) / (kind == Layout::Std140 ? 16 : ca) * (kind == Layout::Std140 ? 16 : ca);
    for (uint32_t c = 0; c < t.cols; ++c)
      for (uint32_t r = 0; r < t.rows; ++r) slots[c * t.rows + r] = word(offset + uint64_t{c} * stride + 4 * r);
    return;
  }
  vec_layout(t.rows, &ea, &es);
  offset = (offset + ea - 1) / ea * ea;
  for (uint32_t r = 0; r < t.rows; ++r) slots[r] = word(offset + 4 * r);
}

void store_layout(const Module& m, const Type& t, Layout kind, ByteView bytes, uint64_t offset, const uint32_t* slots) {
  auto put = [&](uint64_t at, uint32_t v) {
    if (at + 4 <= bytes.size()) __builtin_memcpy(bytes.data() + at, &v, 4);
  };
  uint32_t ea, es;
  const bool uniform_rules = kind != Layout::Natural;
  if (t.array) {
    layout_of(m, t.element(), kind, &ea, &es);
    uint32_t stride = round_up(es, ea);
    if (uniform_rules) stride = round_up(stride, 16);
    const uint32_t eslots = m.slots_of(t.element());
    offset = (offset + ea - 1) / ea * ea;
    for (uint32_t i = 0; i < t.array; ++i) store_layout(m, t.element(), kind, bytes, offset + uint64_t{i} * stride, slots + i * eslots);
    return;
  }
  if (t.base == Base::Struct) {
    const StructDef* sd = m.struct_of(t);
    if (!sd) return;
    layout_of(m, t, kind, &ea, &es);
    offset = (offset + ea - 1) / ea * ea;
    for (uint32_t i = 0; i < sd->fields.count(); ++i)
      store_layout(m, sd->fields[i].type, kind, bytes, offset + field_offset(m, *sd, i, kind), slots + sd->fields[i].offset);
    return;
  }
  if (t.cols > 1) {
    uint32_t ca, cs;
    vec_layout(t.rows, &ca, &cs);
    const uint32_t stride = kind == Layout::Std140 ? 16 : round_up(cs, ca);
    const uint32_t al = kind == Layout::Std140 ? 16 : ca;
    offset = (offset + al - 1) / al * al;
    for (uint32_t c = 0; c < t.cols; ++c)
      for (uint32_t r = 0; r < t.rows; ++r) put(offset + uint64_t{c} * stride + 4 * r, slots[c * t.rows + r]);
    return;
  }
  vec_layout(t.rows, &ea, &es);
  offset = (offset + ea - 1) / ea * ea;
  for (uint32_t r = 0; r < t.rows; ++r) put(offset + 4 * r, slots[r]);
}

}  // namespace shader

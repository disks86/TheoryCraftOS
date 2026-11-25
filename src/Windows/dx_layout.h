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

/* Input layouts checked as the runtimes check them: ID3D11Device::CreateInputLayout (d3d11.dll) and the input layout of
 * ID3D12Device::CreateGraphicsPipelineState (d3d12.dll), against the input signature of the vertex shader. Portable C (both DLLs, and the host test
 * tests/DxLayoutTest.cpp).
 *
 * The elements themselves (dxl_check_elements; the D3D11_MESSAGE_ID_CREATEINPUTLAYOUT_* errors, each of them E_INVALIDARG):
 *   - more than 32 elements (D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT), elements given as NULL with a count;
 *   - an element with no semantic name, a format no vertex buffer can have (the caller's table: 0 bytes), an input slot past the 32nd;
 *   - a classification that is neither per vertex nor per instance, a per-vertex element with a step rate (Direct3D 11 only);
 *   - elements of one slot with different classifications or step rates (of per-vertex ones Direct3D 12 does not look at the rate);
 *   - two elements with the same semantic name (letter case aside) and index;
 *   - an AlignedByteOffset that is not a multiple of the element's size, or of 4 when that is smaller (D3D11_APPEND_ALIGNED_ELEMENT aside; Direct3D 11 only here).
 * Against the signature (dxl_check_signature): the shader is a DXBC container with an input signature (ISGN, or ISG1 of Shader Model 5.1 and 6), else
 * E_INVALIDARG; every element of it that the shader reads (its read mask not 0) and that is not a value the input assembler makes itself (a system value:
 * SV_VertexID, SV_InstanceID, ...) needs an element of the layout with its semantic name and index, else E_INVALIDARG (MISSINGELEMENT). A layout may have
 * elements the shader has no input for (they are not read), and an element's format may have fewer components than the shader reads (the others are 0, 0,
 * 1 as the input assembler fills them) or another type (the bits are reinterpreted): the runtime only warns about those. An input the shader declares but
 * never reads is not looked for (its read mask is 0; NOT checked against Windows: the runtime may want those too - kept lenient, so that no program that
 * runs on Windows fails here).
 * Everything read is checked: a container cut short or damaged gives DXL_INVALID, never a read outside the bytes given. */
#ifndef TCOS_DX_LAYOUT_H
#define TCOS_DX_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define DXL_OK 0
#define DXL_INVALID 1

#define DXL_MAX_ELEMENTS 32u            /* D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT, and Direct3D 12's */
#define DXL_MAX_SLOTS 32u               /* D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT */
#define DXL_APPEND 0xFFFFFFFFu          /* D3D11_APPEND_ALIGNED_ELEMENT / D3D12_APPEND_ALIGNED_ELEMENT */
#define DXL_MAX_SIGNATURE 64u           /* the input registers looked at (Shader Model 5 has 32, 6 a few more) */

/* D3D11_INPUT_ELEMENT_DESC and D3D12_INPUT_ELEMENT_DESC, which are the same. */
typedef struct DxlElement {
  const char *name;
  uint32_t index, format, slot, offset, classification, step_rate;
} DxlElement;

typedef struct DxlInput {
  char name[64];
  uint32_t index, system_value, component_type, reg, mask, read_mask;
} DxlInput;

static uint32_t dxl_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int dxl_same_name(const char *a, const char *b) {
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* The elements; `element_bytes`: the bytes of a vertex of the format (0: not a vertex format). `rules`: DXL_RULES_D3D11, or DXL_RULES_D3D12 - Direct3D 12 does not look at
 * the step rate of a per-vertex element (and a per-vertex slot's elements may then differ in it) nor, here, at the alignment of an offset (not checked against Windows: kept
 * lenient, as d3d12.dll was). */
#define DXL_RULES_D3D11 0
#define DXL_RULES_D3D12 1
static int dxl_check_elements(const DxlElement *e, uint32_t count, uint32_t (*element_bytes)(uint32_t format), int rules) {
  if (count > DXL_MAX_ELEMENTS || (count && !e)) return DXL_INVALID;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t size = element_bytes(e[i].format), unit = size >= 4 ? 4 : size;
    if (!e[i].name || !size || e[i].slot >= DXL_MAX_SLOTS) return DXL_INVALID;
    if (e[i].classification > 1) return DXL_INVALID;
    if (rules == DXL_RULES_D3D11 && e[i].classification == 0 && e[i].step_rate != 0) return DXL_INVALID;
    if (rules == DXL_RULES_D3D11 && e[i].offset != DXL_APPEND && e[i].offset % unit) return DXL_INVALID;
    for (uint32_t k = 0; k < i; ++k) {
      const int rates_differ = e[k].step_rate != e[i].step_rate && (rules == DXL_RULES_D3D11 || e[i].classification == 1);
      if (e[k].slot == e[i].slot && (e[k].classification != e[i].classification || rates_differ)) return DXL_INVALID;
      if (e[k].index == e[i].index && dxl_same_name(e[k].name, e[i].name)) return DXL_INVALID;
    }
  }
  return DXL_OK;
}

/* The input signature of a DXBC container: its elements into `out` (at most DXL_MAX_SIGNATURE), *count. DXL_INVALID for a container that is not one, is cut short,
 * or has no input signature. */
static int dxl_read_inputs(const void *code, size_t bytes, DxlInput *out, uint32_t *count) {
  const uint8_t *b = (const uint8_t *)code;
  *count = 0;
  if (!b || bytes < 32 || b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return DXL_INVALID;
  const uint32_t total = dxl_le32(b + 24), parts = dxl_le32(b + 28);
  if (total > bytes || total < 32 || parts > 64 || parts > (total - 32) / 4) return DXL_INVALID;
  const uint8_t *chunk = NULL;
  uint32_t chunk_size = 0, stride = 0;
  for (uint32_t i = 0; i < parts; ++i) {
    const uint32_t at = dxl_le32(b + 32 + 4 * i);
    if (at > total || total - at < 8) return DXL_INVALID;
    const uint32_t n = dxl_le32(b + at + 4);
    if (n > total - at - 8) return DXL_INVALID;
    const uint8_t *tag = b + at;
    if (tag[0] != 'I' || tag[1] != 'S' || tag[2] != 'G') continue;
    if (tag[3] == 'N' || tag[3] == '1') {
      if (chunk) return DXL_INVALID;      /* (two input signatures) */
      chunk = b + at + 8, chunk_size = n, stride = tag[3] == '1' ? 32 : 24;
    }
  }
  if (!chunk || chunk_size < 8) return DXL_INVALID;
  const uint32_t n = dxl_le32(chunk);
  if (n > DXL_MAX_SIGNATURE || n > (chunk_size - 8) / stride) return DXL_INVALID;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t *e = chunk + 8 + (size_t)i * stride + (stride == 32 ? 4 : 0);      /* (ISG1: the stream first) */
    DxlInput *in = &out[i];
    const uint32_t name_at = dxl_le32(e);
    uint32_t k = 0;
    for (;; ++k) {
      if (name_at >= chunk_size || k >= chunk_size - name_at || k + 1 >= sizeof in->name) return DXL_INVALID;      /* (a name past the part, or too long) */
      in->name[k] = (char)chunk[name_at + k];
      if (!in->name[k]) break;
    }
    if (k == 0) return DXL_INVALID;
    in->index = dxl_le32(e + 4);
    in->system_value = dxl_le32(e + 8);
    in->component_type = dxl_le32(e + 12);
    in->reg = dxl_le32(e + 16);
    in->mask = e[20];
    in->read_mask = e[21];
  }
  *count = n;
  return DXL_OK;
}

/* The layout against the shader's input signature (see the top). */
static int dxl_check_signature(const DxlElement *e, uint32_t count, const void *code, size_t bytes) {
  DxlInput inputs[DXL_MAX_SIGNATURE];
  uint32_t n = 0;
  if (dxl_read_inputs(code, bytes, inputs, &n) != DXL_OK) return DXL_INVALID;
  for (uint32_t i = 0; i < n; ++i) {
    if (inputs[i].system_value != 0 || !(inputs[i].read_mask & 0xF)) continue;      /* (made by the input assembler; never read) */
    int found = 0;
    for (uint32_t k = 0; k < count && !found; ++k) found = e[k].name && e[k].index == inputs[i].index && dxl_same_name(e[k].name, inputs[i].name);
    if (!found) return DXL_INVALID;
  }
  return DXL_OK;
}

#endif

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

// Host-side tests of src/Windows/dx_layout.h, the input layout checks of d3d11.dll (CreateInputLayout) and d3d12.dll (CreateGraphicsPipelineState):
//   - the input signature of a vertex shader Microsoft's compiler made (tests/data/dxbc/vs_layout_sig.hlsl) is read as its disassembly lists it;
//   - the elements' own checks (count, names, formats, slots, classifications, step rates, duplicates, alignment);
//   - the layout against the signature: every input the shader reads needs an element of its semantic name (any letter case) and index, the system values
//     (SV_VertexID, SV_InstanceID) none, an input the shader never reads none; elements the shader has no input for are allowed;
//   - every prefix of the container and every bit turned is read without a read outside it (ASan, UBSan).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../src/Windows/dx_layout.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  uint8_t buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
  fclose(f);
  return out;
}

// DXGI formats of the test: R32G32B32A32_FLOAT 2, R32G32B32_FLOAT 6, R32G32_FLOAT 16, R8G8B8A8_UINT 30, R32_UINT 42, R16_FLOAT 54 (2 bytes), R8_UINT 62 (1 byte);
// 0 (UNKNOWN) and 3 (R32G32B32A32_UINT... treated as no vertex format here: the callback says).
uint32_t bytes_of(uint32_t format) {
  switch (format) {
    case 2: return 16;
    case 6: return 12;
    case 16: return 8;
    case 30: return 4;
    case 42: return 4;
    case 54: return 2;
    case 62: return 1;
    default: return 0;
  }
}

DxlElement element(const char* name, uint32_t index, uint32_t format, uint32_t slot = 0, uint32_t offset = DXL_APPEND, uint32_t classification = 0, uint32_t step = 0) {
  DxlElement e;
  e.name = name, e.index = index, e.format = format, e.slot = slot, e.offset = offset, e.classification = classification, e.step_rate = step;
  return e;
}

void test_elements() {
  std::vector<DxlElement> ok = {element("POSITION", 0, 2), element("TEXCOORD", 3, 16), element("BLENDINDICES", 0, 30), element("COLOR", 0, 2, 1, 0, 1, 1)};
  CHECK(dxl_check_elements(ok.data(), static_cast<uint32_t>(ok.size()), bytes_of, DXL_RULES_D3D11) == DXL_OK);
  CHECK(dxl_check_elements(nullptr, 0, bytes_of, DXL_RULES_D3D11) == DXL_OK);   // (no elements: a shader with no inputs)
  CHECK(dxl_check_elements(nullptr, 1, bytes_of, DXL_RULES_D3D11) == DXL_INVALID);
  std::vector<DxlElement> many(33, element("TEXCOORD", 0, 2));
  for (uint32_t i = 0; i < many.size(); ++i) many[i].index = i;
  CHECK(dxl_check_elements(many.data(), 32, bytes_of, DXL_RULES_D3D11) == DXL_OK && dxl_check_elements(many.data(), 33, bytes_of, DXL_RULES_D3D11) == DXL_INVALID);   // (32 at most)
  auto one_bad = [&](DxlElement bad) {
    std::vector<DxlElement> v = ok;
    v.push_back(bad);
    return dxl_check_elements(v.data(), static_cast<uint32_t>(v.size()), bytes_of, DXL_RULES_D3D11);
  };
  CHECK(one_bad(element(nullptr, 0, 2)) == DXL_INVALID);                       // (no name)
  CHECK(one_bad(element("NORMAL", 0, 0)) == DXL_INVALID);                      // (no vertex format)
  CHECK(one_bad(element("NORMAL", 0, 2, 31)) == DXL_OK && one_bad(element("NORMAL", 0, 2, 32)) == DXL_INVALID);   // (slots 0 .. 31)
  CHECK(one_bad(element("NORMAL", 0, 2, 2, DXL_APPEND, 2)) == DXL_INVALID);   // (a classification that is neither)
  CHECK(one_bad(element("NORMAL", 0, 2, 2, DXL_APPEND, 0, 1)) == DXL_INVALID);   // (a step rate per vertex)
  CHECK(one_bad(element("NORMAL", 0, 2, 2, DXL_APPEND, 1, 0)) == DXL_OK);        // (per instance, step rate 0: every instance the first)
  CHECK(one_bad(element("NORMAL", 0, 2, 1, DXL_APPEND, 1, 2)) == DXL_INVALID);   // (slot 1's elements step at 1)
  CHECK(one_bad(element("NORMAL", 0, 2, 1, DXL_APPEND, 0, 0)) == DXL_INVALID);   // (slot 1's are per instance)
  CHECK(one_bad(element("position", 0, 2, 3)) == DXL_INVALID);                 // (POSITION 0 again, in another case)
  CHECK(one_bad(element("POSITION", 1, 2, 3)) == DXL_OK);                       // (another index)
  CHECK(one_bad(element("NORMAL", 0, 2, 2, 6)) == DXL_INVALID && one_bad(element("NORMAL", 0, 2, 2, 8)) == DXL_OK);   // (a multiple of 4)
  CHECK(one_bad(element("NORMAL", 0, 54, 2, 2)) == DXL_OK && one_bad(element("NORMAL", 0, 54, 2, 3)) == DXL_INVALID);   // (of 2 for a 2-byte format)
  CHECK(one_bad(element("NORMAL", 0, 62, 2, 3)) == DXL_OK);                     // (any byte for a 1-byte one)
  // Direct3D 12: a per-vertex element's step rate and an offset's alignment are not looked at; the rest is the same
  auto one_bad12 = [&](DxlElement bad) {
    std::vector<DxlElement> v = ok;
    v.push_back(bad);
    return dxl_check_elements(v.data(), static_cast<uint32_t>(v.size()), bytes_of, DXL_RULES_D3D12);
  };
  CHECK(one_bad12(element("NORMAL", 0, 2, 2, DXL_APPEND, 0, 5)) == DXL_OK && one_bad12(element("NORMAL", 0, 2, 2, 6)) == DXL_OK);
  CHECK(one_bad12(element("NORMAL", 0, 2, 0, DXL_APPEND, 0, 3)) == DXL_OK);       // (per-vertex elements of a slot with different rates)
  CHECK(one_bad12(element("NORMAL", 0, 2, 1, DXL_APPEND, 1, 2)) == DXL_INVALID);  // (per-instance ones may not differ)
  CHECK(one_bad12(element("position", 0, 2, 3)) == DXL_INVALID && one_bad12(element("NORMAL", 0, 0)) == DXL_INVALID && one_bad12(element("NORMAL", 0, 2, 32)) == DXL_INVALID);
}

void test_signature(const std::vector<uint8_t>& vs) {
  DxlInput in[DXL_MAX_SIGNATURE];
  uint32_t n = 0;
  CHECK(dxl_read_inputs(vs.data(), vs.size(), in, &n) == DXL_OK && n == 6);
  if (n != 6) return;
  // (as Microsoft's disassembler lists it: name, index, register, system value, read mask)
  CHECK(strcmp(in[0].name, "POSITION") == 0 && in[0].index == 0 && in[0].reg == 0 && in[0].system_value == 0 && in[0].read_mask == 0xF);
  CHECK(strcmp(in[1].name, "TEXCOORD") == 0 && in[1].index == 3 && in[1].reg == 1 && in[1].read_mask == 0x3);
  CHECK(strcmp(in[2].name, "BLENDINDICES") == 0 && in[2].component_type == 1 && in[2].read_mask == 0x1);
  CHECK(strcmp(in[3].name, "COLOR") == 0 && in[3].read_mask == 0);
  CHECK(strcmp(in[4].name, "SV_VertexID") == 0 && in[4].system_value == 6 && strcmp(in[5].name, "SV_InstanceID") == 0 && in[5].system_value == 8);
  const std::vector<DxlElement> full = {element("POSITION", 0, 2), element("TEXCOORD", 3, 16), element("BLENDINDICES", 0, 30)};
  CHECK(dxl_check_signature(full.data(), 3, vs.data(), vs.size()) == DXL_OK);
  // (the semantics in another letter case; formats of fewer components and another type: allowed)
  const std::vector<DxlElement> other = {element("position", 0, 6), element("TexCoord", 3, 42), element("blendindices", 0, 2)};
  CHECK(dxl_check_signature(other.data(), 3, vs.data(), vs.size()) == DXL_OK);
  // (more elements than the shader reads, the unread COLOR too, and one for a system value: allowed)
  const std::vector<DxlElement> more = {element("NORMAL", 0, 2), element("POSITION", 0, 2), element("TEXCOORD", 3, 16), element("BLENDINDICES", 0, 30), element("COLOR", 0, 2),
                                        element("SV_VertexID", 0, 42)};
  CHECK(dxl_check_signature(more.data(), 6, vs.data(), vs.size()) == DXL_OK);
  // (one the shader reads left out, another index, a wrong name: E_INVALIDARG)
  for (uint32_t drop = 0; drop < 3; ++drop) {
    std::vector<DxlElement> less;
    for (uint32_t k = 0; k < 3; ++k)
      if (k != drop) less.push_back(full[k]);
    CHECK(dxl_check_signature(less.data(), 2, vs.data(), vs.size()) == DXL_INVALID);
  }
  const std::vector<DxlElement> index = {element("POSITION", 0, 2), element("TEXCOORD", 2, 16), element("BLENDINDICES", 0, 30)};
  CHECK(dxl_check_signature(index.data(), 3, vs.data(), vs.size()) == DXL_INVALID);
  const std::vector<DxlElement> name = {element("POSITIONS", 0, 2), element("TEXCOORD", 3, 16), element("BLENDINDICES", 0, 30)};
  CHECK(dxl_check_signature(name.data(), 3, vs.data(), vs.size()) == DXL_INVALID);
  CHECK(dxl_check_signature(nullptr, 0, vs.data(), vs.size()) == DXL_INVALID);
  // (no shader, or something that is not a container, or a container with no input signature)
  CHECK(dxl_check_signature(full.data(), 3, nullptr, 0) == DXL_INVALID);
  const uint8_t junk[64] = {'D', 'X', 'B', 'X'};
  CHECK(dxl_check_signature(full.data(), 3, junk, sizeof junk) == DXL_INVALID);
  std::vector<uint8_t> no_isgn = vs;
  for (size_t i = 32; i + 4 <= no_isgn.size(); ++i)
    if (memcmp(no_isgn.data() + i, "ISGN", 4) == 0) no_isgn[i + 3] = 'X';
  CHECK(dxl_check_signature(full.data(), 3, no_isgn.data(), no_isgn.size()) == DXL_INVALID);
}

void test_damaged(const std::vector<uint8_t>& vs) {
  const std::vector<DxlElement> full = {element("POSITION", 0, 2), element("TEXCOORD", 3, 16), element("BLENDINDICES", 0, 30)};
  int read = 0;
  for (size_t n = 0; n < vs.size(); ++n) {
    const std::vector<uint8_t> cut(vs.begin(), vs.begin() + static_cast<long>(n));
    if (dxl_check_signature(full.data(), 3, cut.empty() ? nullptr : cut.data(), cut.size()) == DXL_OK) ++read;
  }
  CHECK(read == 0);   // (a container cut short is not one)
  int taken = 0;
  for (size_t bit = 0; bit < vs.size() * 8; ++bit) {
    std::vector<uint8_t> turned = vs;
    turned[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    DxlInput in[DXL_MAX_SIGNATURE];
    uint32_t n = 0;
    if (dxl_read_inputs(turned.data(), turned.size(), in, &n) == DXL_OK) ++taken;
    (void)dxl_check_signature(full.data(), 3, turned.data(), turned.size());
  }
  printf("  damaged: %d of %zu turned bits read\n", taken, vs.size() * 8);
  CHECK(taken > 0);
}

}  // namespace

int main() {
  const std::vector<uint8_t> vs = read_file("tests/data/dxbc/vs_layout_sig.dxbc");
  CHECK(vs.size() > 100);
  if (vs.size() <= 100) {
    printf("tests/data/dxbc/vs_layout_sig.dxbc missing (git lfs checkout tests/data)\n");
    return 1;
  }
  test_elements();
  test_signature(vs);
  test_damaged(vs);
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}

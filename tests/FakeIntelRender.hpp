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

// A model of the Gen9 3D pipeline for the host tests of the batch builder and the graphics device: it decodes a batch the way the hardware reads it (every pointer is
// resolved through the simulated GPU's page tables) and runs the job on the render target it names. The including test defines CHECK, g_failed and as_float.
#pragma once
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderBatch.hpp"

// --- A model of the pipeline for the jobs of the batch builder -------------------------------------------------------------------------------

struct Decoded {
  const std::vector<uint32_t>& dump;
  std::map<uint32_t, std::vector<uint32_t>> at;   // header (bits 31:16) -> dword indices
  explicit Decoded(const std::vector<uint32_t>& d) : dump(d) {
    uint32_t i = 0;
    while (i < dump.size()) {
      const uint32_t length = render_command_length(ArrayView<const uint32_t>(dump.data() + i, dump.size() - i));
      if (length == 0) break;
      if (dump[i] == kMiBatchBufferEnd) break;
      at[dump[i] & 0xFFFF0000u].push_back(i);
      i += length;
    }
  }
  bool has(uint32_t header) const { return at.count(header) != 0; }
  uint32_t one(uint32_t header) const { auto it = at.find(header); return it == at.end() || it->second.size() != 1 ? 0xFFFFFFFFu : it->second[0]; }
};

// Reads a dword of the context's address space; false (and a violation) if it is not mapped.
static bool read_dword(FakeGpu& gpu, uint64_t address, uint32_t* out, const char* what) {
  bool ok;
  uint8_t* p = gpu.ppgtt_bytes(address, 4, what, &ok);
  if (!ok) return false;
  memcpy(out, p, 4);
  return true;
}

// Runs the job the batch describes on the render target it names, as far as the builder's jobs go: solid colour kernels over rectangle lists and triangle lists.
// The hardware's rounding at primitive edges is not modelled, so tests look at pixels well inside and well outside.
static int g_executed = 0;
static void execute(FakeGpu& gpu, const std::vector<uint32_t>& dump) {
  Decoded d(dump);
  const uint32_t sba = d.one(kStateBaseAddress), ps = d.one(kPs), bt = d.one(kBindingTablePointersPs), rect = d.one(kDrawingRectangle), vb = d.one(kVertexBuffers),
                 topo = d.one(kVfTopology), prim = d.one(k3dPrimitive);
  CHECK(sba != 0xFFFFFFFFu && ps != 0xFFFFFFFFu && bt != 0xFFFFFFFFu && rect != 0xFFFFFFFFu && vb != 0xFFFFFFFFu && topo != 0xFFFFFFFFu && prim != 0xFFFFFFFFu);
  if (sba == 0xFFFFFFFFu || ps == 0xFFFFFFFFu || bt == 0xFFFFFFFFu || rect == 0xFFFFFFFFu || vb == 0xFFFFFFFFu || topo == 0xFFFFFFFFu || prim == 0xFFFFFFFFu) return;
  const uint32_t surface_base = dump[sba + 4] & ~1u, instruction_base = dump[sba + 10] & ~1u, dynamic_base = dump[sba + 6] & ~1u;
  CHECK(surface_base == dynamic_base && surface_base == instruction_base);
  // The kernel is the constant-colour one (the colour is in its immediates) or the interpolating one.
  uint32_t kernel[kFlatKernelDwords];
  for (uint32_t i = 0; i < kFlatKernelDwords; ++i)
    if (!read_dword(gpu, uint64_t{instruction_base} + dump[ps + 1] + i * 4, &kernel[i], "kernel")) return;
  const bool interpolated = kernel[0] == 0x0080005a;
  CHECK(interpolated ? kernel[16] == 0x05800031 : (kernel[0] == 0x00600001 && kernel[32] == 0x05800031));
  const float flat_color[4] = {as_float(kernel[3]), as_float(kernel[11]), as_float(kernel[19]), as_float(kernel[27])};
  const uint32_t ps_blend = d.one(kPsBlend);
  const bool blend = ps_blend != 0xFFFFFFFFu && (dump[ps_blend + 1] & (1u << 29));
  // The colour write masks of render target 0 (the low bits of its blend state entry).
  const uint32_t blend_ptr = d.one(kBlendStatePointers);
  uint32_t color_mask = 0;
  if (blend_ptr != 0xFFFFFFFFu && !read_dword(gpu, uint64_t{dynamic_base} + (dump[blend_ptr + 1] & ~1u) + 4, &color_mask, "blend state")) return;
  const bool writes_color = (color_mask & 0xF) != 0xF;
  // The depth buffer: its packet and the test.
  const uint32_t db = d.one(kDepthBuffer), ds = d.one(kWmDepthStencil);
  const bool has_depth = db != 0xFFFFFFFFu && (dump[db + 1] >> 29) == 1;
  const uint32_t depth_pitch = has_depth ? (dump[db + 1] & 0x3FFFF) + 1 : 0;
  const uint64_t depth_base = has_depth ? (uint64_t{dump[db + 3]} << 32) | dump[db + 2] : 0;
  const bool depth_write = has_depth && ds != 0xFFFFFFFFu && (dump[ds + 1] & 1) && (dump[db + 1] & (1u << 28));
  const bool depth_test = has_depth && ds != 0xFFFFFFFFu && (dump[ds + 1] & (1u << 1));
  const uint32_t depth_func = ds != 0xFFFFFFFFu ? (dump[ds + 1] >> 5) & 7 : 7;
  if (has_depth) CHECK(dump[db + 1] & (1u << 18));   // D32_FLOAT
  // Binding table entry 0, its surface state, and the target.
  uint32_t entry = 0, ss[10] = {};
  if (!read_dword(gpu, uint64_t{surface_base} + dump[bt + 1], &entry, "binding table")) return;
  for (uint32_t i = 0; i < 10; ++i)
    if (!read_dword(gpu, uint64_t{surface_base} + entry + i * 4, &ss[i], "surface state")) return;
  const uint32_t format = (ss[0] >> 18) & 0x1FF, width = (ss[2] & 0x3FFF) + 1, height = ((ss[2] >> 16) & 0x3FFF) + 1, pitch = (ss[3] & 0x3FFFF) + 1;
  const uint64_t target = (uint64_t{ss[9]} << 32) | ss[8];
  CHECK(ss[1] == (2u << 24));   // memory object control: table entry 1 (the page table's), the field is the index shifted left by one
  CHECK((ss[0] >> 29) == 1 && (format == 0x0C0 || format == 0x0C7) && ss[7] == ((7u << 16) | (6u << 19) | (5u << 22) | (4u << 25)));
  // The vertices.
  const uint64_t vertices_at = (uint64_t{dump[vb + 3]} << 32) | dump[vb + 2];
  const uint32_t count = dump[prim + 2], stride = dump[vb + 1] & 0xFFF;
  CHECK(stride == 32 && dump[vb + 4] == count * stride);
  std::vector<float> v(count * 8);
  for (uint32_t i = 0; i < count * 8; ++i) {
    uint32_t bits;
    if (!read_dword(gpu, vertices_at + i * 4, &bits, "vertices")) return;
    v[i] = as_float(bits);
  }
  const int32_t x_min = static_cast<int32_t>(dump[rect + 1] & 0xFFFF), y_min = static_cast<int32_t>(dump[rect + 1] >> 16);
  const int32_t x_max = static_cast<int32_t>(dump[rect + 2] & 0xFFFF), y_max = static_cast<int32_t>(dump[rect + 2] >> 16);
  auto to8 = [](float c) { return static_cast<uint8_t>((c < 0 ? 0 : c > 1 ? 1 : c) * 255.0f + 0.5f); };
  auto cross = [](float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
  for (int32_t y = y_min; y <= y_max && y < static_cast<int32_t>(height); ++y) {
    for (int32_t x = x_min; x <= x_max && x < static_cast<int32_t>(width); ++x) {
      const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
      float color[4] = {flat_color[0], flat_color[1], flat_color[2], flat_color[3]};
      bool covered = false;
      float z = v[2];
      if (dump[topo + 1] == kTopologyRectList) {
        float lx = v[0], hx = v[0], ly = v[1], hy = v[1];
        for (int k = 1; k < 3; ++k) {
          if (v[k * 8] < lx) lx = v[k * 8];
          if (v[k * 8] > hx) hx = v[k * 8];
          if (v[k * 8 + 1] < ly) ly = v[k * 8 + 1];
          if (v[k * 8 + 1] > hy) hy = v[k * 8 + 1];
        }
        covered = px > lx && px < hx && py > ly && py < hy;
      } else {
        for (uint32_t t = 0; t + 2 < count && !covered; t += 3) {
          const float* a = &v[t * 8];
          const float* b = &v[(t + 1) * 8];
          const float* c = &v[(t + 2) * 8];
          const float total = cross(a[0], a[1], b[0], b[1], c[0], c[1]);
          if (total == 0) continue;
          // Barycentric weights; the pixel is inside when none is negative.
          const float w0 = cross(b[0], b[1], c[0], c[1], px, py) / total, w1 = cross(c[0], c[1], a[0], a[1], px, py) / total, w2 = cross(a[0], a[1], b[0], b[1], px, py) / total;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          covered = true;
          // Depth is linear in window space; colours are interpolated perspective-correct from the 1/w the vertices carry.
          z = w0 * a[2] + w1 * b[2] + w2 * c[2];
          const float p0 = w0 * a[3], p1 = w1 * b[3], p2 = w2 * c[3], sum = p0 + p1 + p2;
          if (interpolated)
            for (int k = 0; k < 4; ++k) color[k] = (p0 * a[4 + k] + p1 * b[4 + k] + p2 * c[4 + k]) / sum;
        }
      }
      if (!covered) continue;
      uint8_t* pixel[4];
      bool ok = true;
      for (int k = 0; k < 4; ++k) {
        bool mapped;
        pixel[k] = gpu.ppgtt_bytes(target + uint64_t{static_cast<uint32_t>(y)} * pitch + static_cast<uint32_t>(x) * 4 + k, 1, "render target", &mapped);
        ok = ok && mapped;
      }
      if (!ok) continue;
      if (has_depth) {
        bool mapped;
        uint8_t* stored = gpu.ppgtt_bytes(depth_base + uint64_t{static_cast<uint32_t>(y)} * depth_pitch + static_cast<uint32_t>(x) * 4, 4, "depth buffer", &mapped);
        if (!mapped) continue;
        float old_z;
        memcpy(&old_z, stored, 4);
        const bool passes = !depth_test || depth_func == 7 || (depth_func == 3 && z <= old_z);
        if (!passes) continue;
        if (depth_write) memcpy(stored, &z, 4);
      }
      if (!writes_color) continue;
      // The target's byte order: B8G8R8A8 is blue first.
      const int red = format == 0x0C0 ? 2 : 0, blue = format == 0x0C0 ? 0 : 2;
      if (blend) {
        // Source over: colour by source alpha and its inverse, alpha by one and the inverse.
        const float sa = color[3] < 0 ? 0 : color[3] > 1 ? 1 : color[3];
        const float dst[4] = {*pixel[red] / 255.0f, *pixel[1] / 255.0f, *pixel[blue] / 255.0f, *pixel[3] / 255.0f};
        for (int k = 0; k < 3; ++k) color[k] = color[k] * sa + dst[k] * (1.0f - sa);
        color[3] = sa + dst[3] * (1.0f - sa);
      }
      *pixel[red] = to8(color[0]);
      *pixel[1] = to8(color[1]);
      *pixel[blue] = to8(color[2]);
      *pixel[3] = to8(color[3]);
    }
  }
  ++g_executed;
}


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

// A texture the pixel kernel samples (the model of the sampler for the jobs of RenderBatch::textured_triangles): the surface state's size, pitch, base and byte order, and
// the sampler state's filter and address modes. Normalised coordinates, (0, 0) at the top left corner of the top left texel; no mipmaps.
struct FakeTexture {
  FakeGpu* gpu = nullptr;
  uint64_t base = 0;
  uint32_t width = 0, height = 0, pitch = 0, format = 0;
  bool linear = false;
  uint32_t mode_u = 2, mode_v = 2;   // 0 wrap, 2 clamp
  static int64_t floor_to(float f) {
    const int64_t i = static_cast<int64_t>(f);
    return static_cast<float>(i) > f ? i - 1 : i;
  }
  static int64_t address(int64_t i, int64_t size, uint32_t mode) {
    if (mode == 0) {
      i %= size;
      return i < 0 ? i + size : i;
    }
    return i < 0 ? 0 : (i >= size ? size - 1 : i);
  }
  bool texel(int64_t x, int64_t y, float out[4]) const {
    bool mapped;
    uint8_t* p = gpu->ppgtt_bytes(base + uint64_t(y) * pitch + uint64_t(x) * 4, 4, "texture", &mapped);
    if (!mapped) return false;
    const int red = format == 0x0C0 ? 2 : 0, blue = format == 0x0C0 ? 0 : 2;
    out[0] = p[red] / 255.0f;
    out[1] = p[1] / 255.0f;
    out[2] = p[blue] / 255.0f;
    out[3] = p[3] / 255.0f;
    return true;
  }
  bool sample(float u, float v, float out[4]) const {
    if (!(u == u) || !(v == v) || u > 1e6f || u < -1e6f || v > 1e6f || v < -1e6f) return false;
    if (!linear) return texel(address(floor_to(u * width), width, mode_u), address(floor_to(v * height), height, mode_v), out);
    const float fx = u * width - 0.5f, fy = v * height - 0.5f;
    const int64_t x0 = floor_to(fx), y0 = floor_to(fy);
    const float tx = fx - x0, ty = fy - y0;
    const int64_t xs[2] = {address(x0, width, mode_u), address(x0 + 1, width, mode_u)}, ys[2] = {address(y0, height, mode_v), address(y0 + 1, height, mode_v)};
    float a[4], b[4], c[4], d[4];
    if (!texel(xs[0], ys[0], a) || !texel(xs[1], ys[0], b) || !texel(xs[0], ys[1], c) || !texel(xs[1], ys[1], d)) return false;
    for (int k = 0; k < 4; ++k) out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
    return true;
  }
};

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
  // The sampling kernel (the copy kernel: two plane equations, a sampler message, the write) also begins with a plane equation; the interpolating one has four.
  const bool sampled = kernel[0] == 0x0080005a && kernel[8] == 0x02800031;
  const bool interpolated = kernel[0] == 0x0080005a && !sampled;
  CHECK(sampled ? kernel[12] == 0x05800031 : interpolated ? kernel[16] == 0x05800031 : (kernel[0] == 0x00600001 && kernel[32] == 0x05800031));
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
  // The stencil test (3DSTATE_WM_DEPTH_STENCIL) and the stencil buffer packet (3DSTATE_STENCIL_BUFFER): on only when both say so.
  const uint32_t sb = d.one(kStencilBuffer);
  const bool stencil_buffer_on = sb != 0xFFFFFFFFu && (dump[sb + 1] & (1u << 31));
  const bool stencil_test = has_depth && ds != 0xFFFFFFFFu && (dump[ds + 1] & (1u << 3));
  const bool stencil_write = stencil_test && (dump[ds + 1] & (1u << 2)) && (dump[db + 1] & (1u << 27));
  if (stencil_test) CHECK(stencil_buffer_on);   // a stencil test needs the buffer
  const uint64_t stencil_base = stencil_buffer_on ? (uint64_t{dump[sb + 3]} << 32) | dump[sb + 2] : 0;
  const uint32_t stencil_func = stencil_test ? (dump[ds + 1] >> 8) & 7 : 7, stencil_pass_op = stencil_test ? (dump[ds + 1] >> 23) & 7 : 0, stencil_depth_fail_op = stencil_test ? (dump[ds + 1] >> 26) & 7 : 0,
                 stencil_fail_op = stencil_test ? (dump[ds + 1] >> 29) & 7 : 0, stencil_write_mask = stencil_test ? (dump[ds + 2] >> 16) & 0xFF : 0, stencil_compare_mask = stencil_test ? (dump[ds + 2] >> 24) & 0xFF : 0,
                 stencil_ref = stencil_test ? (dump[ds + 3] >> 8) & 0xFF : 0;
  // Binding table entry 0, its surface state, and the target.
  uint32_t entry = 0, ss[10] = {};
  if (!read_dword(gpu, uint64_t{surface_base} + dump[bt + 1], &entry, "binding table")) return;
  for (uint32_t i = 0; i < 10; ++i)
    if (!read_dword(gpu, uint64_t{surface_base} + entry + i * 4, &ss[i], "surface state")) return;
  const uint32_t format = (ss[0] >> 18) & 0x1FF, width = (ss[2] & 0x3FFF) + 1, height = ((ss[2] >> 16) & 0x3FFF) + 1, pitch = (ss[3] & 0x3FFFF) + 1;
  const uint64_t target = (uint64_t{ss[9]} << 32) | ss[8];
  CHECK(ss[1] == (2u << 24));   // memory object control: table entry 1 (the page table's), the field is the index shifted left by one
  CHECK((ss[0] >> 29) == 1 && (format == 0x0C0 || format == 0x0C7) && ss[7] == ((7u << 16) | (6u << 19) | (5u << 22) | (4u << 25)));
  // The texture, if the kernel samples one: binding table entry 1, and the sampler state the job points to.
  FakeTexture texture;
  if (sampled) {
    uint32_t texture_entry = 0, ts[10] = {};
    if (!read_dword(gpu, uint64_t{surface_base} + dump[bt + 1] + 4, &texture_entry, "binding table")) return;
    for (uint32_t i = 0; i < 10; ++i)
      if (!read_dword(gpu, uint64_t{surface_base} + texture_entry + i * 4, &ts[i], "texture surface state")) return;
    CHECK((ts[0] >> 29) == 1 && (((ts[0] >> 18) & 0x1FF) == 0x0C0 || ((ts[0] >> 18) & 0x1FF) == 0x0C7) && ts[1] == (2u << 24) && ts[7] == ((7u << 16) | (6u << 19) | (5u << 22) | (4u << 25)));
    const uint32_t sp = d.one(kSamplerStatePointersPs);
    CHECK(sp != 0xFFFFFFFFu && (dump[ps + 3] & (1u << 27)));   // a sampler pointer, and the pixel shader is told it has one
    uint32_t sampler[4] = {};
    if (sp == 0xFFFFFFFFu) return;
    for (uint32_t i = 0; i < 4; ++i)
      if (!read_dword(gpu, uint64_t{dynamic_base} + dump[sp + 1] + i * 4, &sampler[i], "sampler state")) return;
    texture.gpu = &gpu;
    texture.base = (uint64_t{ts[9]} << 32) | ts[8];
    texture.format = (ts[0] >> 18) & 0x1FF;
    texture.width = (ts[2] & 0x3FFF) + 1;
    texture.height = ((ts[2] >> 16) & 0x3FFF) + 1;
    texture.pitch = (ts[3] & 0x3FFFF) + 1;
    const uint32_t min_filter = (sampler[0] >> 14) & 7, mag_filter = (sampler[0] >> 17) & 7;
    CHECK(min_filter == mag_filter && min_filter <= 1);
    texture.linear = mag_filter == 1;
    texture.mode_u = (sampler[3] >> 6) & 7;   // TCX (the s coordinate)
    texture.mode_v = (sampler[3] >> 3) & 7;   // TCY (t)
    CHECK(((sampler[3] >> 0) & 7) == 2);
  }
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
          if (interpolated || sampled) {
            float attribute[4];
            for (int k = 0; k < 4; ++k) attribute[k] = (p0 * a[4 + k] + p1 * b[4 + k] + p2 * c[4 + k]) / sum;
            if (sampled) {
              if (!texture.sample(attribute[0], attribute[1], color)) { covered = false; continue; }   // (an unmapped texture: the pixel is not drawn)
            } else {
              for (int k = 0; k < 4; ++k) color[k] = attribute[k];
            }
          }
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
      bool depth_passes = true;
      uint8_t* depth_word = nullptr;
      if (has_depth) {
        bool mapped;
        depth_word = gpu.ppgtt_bytes(depth_base + uint64_t{static_cast<uint32_t>(y)} * depth_pitch + static_cast<uint32_t>(x) * 4, 4, "depth buffer", &mapped);
        if (!mapped) continue;
        float old_z;
        memcpy(&old_z, depth_word, 4);
        depth_passes = !depth_test || depth_func == 7 || (depth_func == 3 && z <= old_z);
      }
      if (stencil_test) {
        // The stencil test first, then the depth test: each outcome has its own operation, applied through the write mask. (The model keeps the stencil values in rows of the
        // target's width; the hardware's W tiling, which the device never looks at, is not modelled.)
        bool mapped;
        uint8_t* value = gpu.ppgtt_bytes(stencil_base + uint64_t{static_cast<uint32_t>(y)} * width + static_cast<uint32_t>(x), 1, "stencil buffer", &mapped);
        if (!mapped) continue;
        const uint32_t stored = *value, ref = stencil_ref & stencil_compare_mask, masked = stored & stencil_compare_mask;
        bool test_passes = false;
        switch (stencil_func) {
          case 0: test_passes = false; break;
          case 1: test_passes = ref < masked; break;
          case 2: test_passes = ref == masked; break;
          case 3: test_passes = ref <= masked; break;
          case 4: test_passes = ref > masked; break;
          case 5: test_passes = ref != masked; break;
          case 6: test_passes = ref >= masked; break;
          default: test_passes = true; break;
        }
        const uint32_t op = !test_passes ? stencil_fail_op : !depth_passes ? stencil_depth_fail_op : stencil_pass_op;
        uint32_t result = stored;
        switch (op) {   // 3D_Stencil_Operation
          case 1: result = 0; break;
          case 2: result = stencil_ref; break;
          case 3: result = stored == 255 ? 255 : stored + 1; break;
          case 4: result = stored == 0 ? 0 : stored - 1; break;
          case 5: result = (stored + 1) & 0xFF; break;
          case 6: result = (stored - 1) & 0xFF; break;
          case 7: result = ~stored & 0xFF; break;
          default: break;
        }
        if (stencil_write) *value = static_cast<uint8_t>((stored & ~stencil_write_mask) | (result & stencil_write_mask));
        if (!test_passes) continue;
      }
      if (!depth_passes) continue;
      if (depth_write && depth_word) memcpy(depth_word, &z, 4);
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


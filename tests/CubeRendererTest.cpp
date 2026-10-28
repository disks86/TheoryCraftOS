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

// Host-side tests for the 3D test's cube, which is drawn through the kernel-mode Direct3D driver (Desktop/CubeRenderer): on a kernel with the software rasterizer behind
// an adapter the flat cube has its face colors, the logo comes out upright and not mirrored on the front and the back face, the picture changes with the angle, the renderer
// can be reopened at other sizes, refuses what it cannot do, and gives its process back. It also prints a small benchmark (frame times, split into submit and readback) -
// the same numbers the 3D test window shows.
// Usage: CubeRendererTest [path to logo.png]   (default assets/logo.png)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include "Desktop/CubeRenderer.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/CubeMesh.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Logo/Logo.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

using namespace desktop;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

namespace hardware {
uint64_t uptime_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}
}

// The logo the boot image carries, from the file on the development machine.
static std::vector<uint8_t> g_logo;
namespace graphics {
ConstByteView embedded_logo_png() { return ConstByteView(g_logo.data(), g_logo.size()); }
}

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> data;
  if (FILE* f = fopen(path, "rb")) {
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
    fclose(f);
  }
  return data;
}

static int channel(uint32_t p, int shift) { return static_cast<int>((p >> shift) & 0xFF); }

static uint32_t pixel(const CubeRenderer& r, uint32_t x, uint32_t y) { return r.picture().pixels[uint64_t{y} * r.picture().pitch + x]; }

// The mean difference between the middle of the cube's front face and the logo, with the logo optionally mirrored. The camera looks at the face straight on
// (angle 0) from 4.5 units, with a 0.9 radian field of view, so the face is a square of 2 / (2 tan(0.45) 3.5) of the picture's height.
static double face_error(const CubeRenderer& r, const ArrayView<uint32_t>& logo, bool mirror_x, bool mirror_y) {
  const double height = r.height(), half = height * 0.5 / (0.4831 * 3.5);   // (half the face, in pixels)
  const double cx = r.width() * 0.5, cy = height * 0.5;
  double sum = 0;
  uint32_t n = 0;
  for (int dy = -static_cast<int>(half * 0.7); dy <= static_cast<int>(half * 0.7); dy += 2) {
    for (int dx = -static_cast<int>(half * 0.7); dx <= static_cast<int>(half * 0.7); dx += 2) {
      const double fx = 0.5 + dx / (2 * half), fy = 0.5 + dy / (2 * half);   // 0..1 across the face
      const uint32_t lx = static_cast<uint32_t>((mirror_x ? 1 - fx : fx) * 256), ly = static_cast<uint32_t>((mirror_y ? 1 - fy : fy) * 256);
      const uint32_t want = logo[ly * 256 + lx], got = pixel(r, static_cast<uint32_t>(cx + dx), static_cast<uint32_t>(cy + dy));
      for (int shift : {0, 8, 16}) sum += abs(channel(want, shift) - channel(got, shift));
      n += 3;
    }
  }
  return sum / n;
}

static void test_flat(TestAllocator& a) {
  graphics::SoftwareDevice device{a, nullptr};
  dxgk::Kernel kernel(a);
  CHECK(kernel.add_adapter(device, dxgk::AdapterConfig()) == 0);
  CHECK(CubeRenderer::find_adapter(kernel, true) == dxgk::kInvalidIndex);   // (no GPU here)
  CHECK(CubeRenderer::find_adapter(kernel, false) == 0);

  CubeRenderer r(a, kernel, 0x3D);
  CHECK(!r.is_open());
  CubeFrameTimes times;
  CHECK(!r.render(0, false, &times));                                      // nothing to draw on yet
  CHECK(r.open(0, 160, 120) && r.is_open());
  CHECK(r.width() == 160 && r.height() == 120 && r.adapter_index() == 0 && !r.caps().accelerated);
  CHECK(r.render(0, false, &times));
  CHECK(!r.last_textured());
  CHECK(pixel(r, 0, 0) == 0xFF10101A && pixel(r, 159, 119) == 0xFF10101A);   // the clear color in the corners
  const uint32_t middle = pixel(r, 80, 60);
  CHECK(channel(middle, 16) > 200 && channel(middle, 8) < 110 && channel(middle, 0) < 110);   // the front face is red
  CHECK(times.submit_us > 0);

  // Turned half way round the cube shows another face (the back one: green), and a quarter turn something else again.
  CHECK(r.render(3.14159265f, false, &times));
  const uint32_t back = pixel(r, 80, 60);
  CHECK(channel(back, 8) > 150 && channel(back, 16) < 140);
  std::vector<uint32_t> before(r.picture().pixels.data(), r.picture().pixels.data() + r.picture().pixels.size());
  CHECK(r.render(0.7f, false, &times));
  uint64_t differing = 0;
  for (uint64_t i = 0; i < before.size(); ++i) differing += before[i] != r.picture().pixels[i];
  CHECK(differing > 500);

  // Reopened at another size; the old objects go with the device.
  CHECK(r.open(0, 96, 96) && r.width() == 96 && r.picture().width == 96);
  CHECK(r.render(0, false, &times) && pixel(r, 0, 0) == 0xFF10101A);
  // What cannot be done is refused and leaves nothing behind.
  CHECK(!r.open(5, 96, 96) && !r.is_open());
  CHECK(!r.open(0, 0, 96) && !r.is_open());
  CHECK(!r.render(0, false, &times));
  CHECK(r.open(0, 64, 64) && r.render(0, true, &times));
  r.close();
  CHECK(!r.is_open() && !r.render(0, false, &times));

  // The process is the renderer's: a second renderer of the same process id is refused by the kernel only if the first still holds it.
  {
    CubeRenderer again(a, kernel, 0x3D);
    CHECK(again.open(0, 64, 64) && again.render(0, false, &times));
  }
  CubeRenderer other(a, kernel, 0x3E);
  CHECK(other.open(0, 64, 64) && other.render(1.0f, true, &times));
}


// The same frame drawn straight on the device, with the commands the renderer sends the kernel.
static bool draw_on_device(TestAllocator& a, graphics::SoftwareDevice& device, uint32_t w, uint32_t h, float angle, const ArrayView<uint32_t>& texels, ArrayView<uint32_t> out) {
  using namespace graphics;
  IShaderModule* vs = device.create_shader_module(textured_vertex_shader());
  IShaderModule* fs = device.create_shader_module(textured_fragment_shader());
  GraphicsPipelineDesc desc;
  describe_textured_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  desc.cull_mode = CullMode::None;
  desc.depth_test = desc.depth_write = true;
  IPipeline* pipeline = device.create_graphics_pipeline(desc);
  IBuffer* vb = device.create_buffer(sizeof(TexturedVertex) * kCubeVertices, kBufferVertex);
  IBuffer* ib = device.create_buffer(sizeof(uint16_t) * kCubeIndices, kBufferIndex);
  IImage* color = device.create_image({{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* depth = device.create_image({{w, h}, Format::D32Float, kImageDepthAttachment});
  IImage* texture = device.create_image({{256, 256}, Format::B8G8R8A8Unorm, kImageSampled});
  ICommandBuffer* cb = device.create_command_buffer();
  bool ok = vs && fs && pipeline && vb && ib && color && depth && texture && cb && texture->write_pixels(ArrayView<const uint32_t>(texels.data(), texels.size()));
  if (ok) {
    ByteView vbytes = vb->map(), ibytes = ib->map();
    build_textured_cube(ArrayView<TexturedVertex>(reinterpret_cast<TexturedVertex*>(vbytes.data()), kCubeVertices), ArrayView<uint16_t>(reinterpret_cast<uint16_t*>(ibytes.data()), kCubeIndices));
    vb->unmap();
    ib->unmap();
    const ColoredConstants constants{CubeRenderer::transform(angle, float(w) / float(h))};
    SamplerDesc sampler;
    sampler.filter = Filter::Linear;
    sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
    RenderPassBegin pass;
    pass.color = color;
    pass.depth = depth;
    pass.clear_color_value = {0.063f, 0.063f, 0.102f, 1.0f};
    cb->begin();
    cb->begin_render_pass(pass);
    cb->bind_pipeline(pipeline);
    cb->bind_vertex_buffer(vb);
    cb->bind_index_buffer(ib, IndexType::Uint16);
    cb->bind_texture(0, texture, sampler);
    cb->push_constants(&constants, sizeof constants);
    cb->draw_indexed(kCubeIndices);
    cb->end_render_pass();
    ok = cb->end() && device.submit(cb) && color->read_pixels(out);
  }
  if (cb) device.destroy_command_buffer(cb);
  if (texture) device.destroy_image(texture);
  if (depth) device.destroy_image(depth);
  if (color) device.destroy_image(color);
  if (ib) device.destroy_buffer(ib);
  if (vb) device.destroy_buffer(vb);
  if (pipeline) device.destroy_pipeline(pipeline);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

static void test_logo(TestAllocator& a) {
  graphics::Logo logo(a);
  CHECK(logo.load(ConstByteView(g_logo.data(), g_logo.size())));
  ArrayView<uint32_t> reference = allocate_view<uint32_t>(a, 256 * 256);
  CHECK(logo.render(256, 0x10101A, reference));

  graphics::SoftwareDevice device{a, nullptr};
  dxgk::Kernel kernel(a);
  kernel.add_adapter(device, dxgk::AdapterConfig());
  CubeRenderer r(a, kernel, 1);
  CHECK(r.open(0, 200, 200) && r.texture_available());
  CubeFrameTimes times;
  CHECK(r.render(0, true, &times) && r.last_textured());
  const double right = face_error(r, reference, false, false), mirrored_x = face_error(r, reference, true, false), mirrored_y = face_error(r, reference, false, true);
  printf("   front face: error %.1f, mirrored across %.1f, upside down %.1f\n", right, mirrored_x, mirrored_y);
  CHECK(right < 40 && right * 1.5 < mirrored_x && right * 1.5 < mirrored_y);
  // The kernel path draws what the device draws when given the same commands (the picture is the same, pixel for pixel).
  for (float angle : {0.0f, 1.3f}) {
    ArrayView<uint32_t> direct = allocate_view<uint32_t>(a, 200 * 200);
    CHECK(r.render(angle, true, &times) && draw_on_device(a, device, 200, 200, angle, reference, direct));
    uint64_t differing = 0;
    for (uint64_t i = 0; i < direct.size(); ++i) differing += direct[i] != r.picture().pixels[i];
    printf("   angle %.1f: %llu of %llu pixels differ from the device's own picture\n", angle, static_cast<unsigned long long>(differing), static_cast<unsigned long long>(direct.size()));
    CHECK(differing == 0);
    release_view(a, direct);
  }
  // Asking for flat colors on a textured-capable renderer gives them.
  CHECK(r.render(0, false, &times) && !r.last_textured());
  release_view(a, reference);
}

// A small benchmark: frames at the size of the 3D test's window on the software rasterizer, flat and with the logo.
static void test_benchmark(TestAllocator& a) {
  graphics::SoftwareDevice device{a, nullptr};
  dxgk::Kernel kernel(a);
  kernel.add_adapter(device, dxgk::AdapterConfig());
  CubeRenderer r(a, kernel, 1);
  CHECK(r.open(0, 400, 300));
  for (bool textured : {false, true}) {
    uint64_t submit = 0, readback = 0;
    constexpr uint32_t kFrames = 12;
    for (uint32_t i = 0; i < kFrames; ++i) {
      CubeFrameTimes t;
      CHECK(r.render(i * 0.5f, textured, &t));
      submit += t.submit_us;
      readback += t.readback_us;
    }
    printf("   %s 400x300: %.2f ms a frame (submit %.2f, readback %.2f), %.1f FPS\n", textured ? "logo" : "flat", (submit + readback) / 1000.0 / kFrames, submit / 1000.0 / kFrames,
           readback / 1000.0 / kFrames, kFrames * 1e6 / double(submit + readback));
  }
}

int main(int argc, char** argv) {
  g_logo = read_file(argc > 1 ? argv[1] : "assets/logo.png");
  if (g_logo.empty()) {
    printf("cannot read the logo\n");
    return 2;
  }
  TestAllocator a;
  test_flat(a);
  test_logo(a);
  test_benchmark(a);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("CubeRendererTest passed\n");
  return 0;
}

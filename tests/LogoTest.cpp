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

// Host-side tests for the OS logo: the PNG (assets/logo.png) decodes and scales, damaged data is refused, and drawn on the faces of the 3D test's
// cube by the software device (which samples the texture) it comes out upright and not mirrored on the front and the back face.
// Usage: LogoTest [path to logo.png]   (default assets/logo.png)
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/CubeMesh.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Logo/Logo.hpp"
#include "Utility/AllocatedView.hpp"

using namespace graphics;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

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

static void test_logo(TestAllocator& a, const std::vector<uint8_t>& png) {
  Logo logo(a);
  CHECK(!logo.loaded());
  CHECK(logo.load(ConstByteView(png.data(), png.size())));
  CHECK(logo.width() == 256 && logo.height() == 256);

  ArrayView<uint32_t> full = allocate_view<uint32_t>(a, 256 * 256);
  CHECK(logo.render(256, 0x101018, full));
  CHECK(full[0] == 0xFF101018u);                        // outside the rounded tile: the background shows through
  CHECK(full[21 * 256 + 232] == 0xFFFFFFFFu);           // the bar in the top-right corner
  CHECK(full[238 * 256 + 18] == 0xFFFFFFFFu);           // the dot in the bottom-left corner
  CHECK(full[10 * 256 + 232] != 0xFFFFFFFFu && full[250 * 256 + 18] != 0xFFFFFFFFu);

  // Shrunk: the boot screen's size. The corner marks stay where they were, in proportion, and the picture is not empty.
  ArrayView<uint32_t> small = allocate_view<uint32_t>(a, 128 * 128);
  CHECK(logo.render(128, 0x101018, small));
  CHECK(channel(small[10 * 128 + 116], 0) > 200 && channel(small[119 * 128 + 9], 0) > 200);
  uint64_t lit = 0;
  for (uint32_t p : small) lit += channel(p, 16) + channel(p, 8) + channel(p, 0) > 150;
  CHECK(lit > 128 * 128 / 4);
  // Enlarged works as well (every output pixel covers at least one source pixel).
  ArrayView<uint32_t> large = allocate_view<uint32_t>(a, 512 * 512);
  CHECK(logo.render(512, 0, large) && channel(large[42 * 512 + 464], 0) == 255);
  release_view(a, large);
  CHECK(!logo.render(128, 0, small.first(100)));        // an output that is too small is refused
  CHECK(!logo.render(0, 0, small));
  release_view(a, small);
  release_view(a, full);

  Logo empty(a);
  ArrayView<uint32_t> none = allocate_view<uint32_t>(a, 16);
  CHECK(!empty.render(4, 0, none));
  release_view(a, none);
}

// Truncated and bit-flipped files must be refused or decoded to something, never read outside the data (the sanitizers watch).
static void test_damaged(TestAllocator& a, const std::vector<uint8_t>& png) {
  for (size_t length : {size_t{0}, size_t{7}, size_t{8}, size_t{33}, png.size() / 4, png.size() / 2, png.size() - 40}) {
    Logo logo(a);
    CHECK(!logo.load(ConstByteView(png.data(), length)));
  }
  for (size_t at = 0; at < png.size(); at += png.size() / 97 + 1) {
    for (int bit = 0; bit < 8; bit += 3) {
      std::vector<uint8_t> bad = png;
      bad[at] ^= static_cast<uint8_t>(1u << bit);
      Logo logo(a);
      if (logo.load(ConstByteView(bad.data(), bad.size()))) {   // (a flip in the pixel data may still decode)
        ArrayView<uint32_t> out = allocate_view<uint32_t>(a, 32 * 32);
        CHECK(logo.render(32, 0, out));
        release_view(a, out);
      }
    }
  }
}

// Draws the cube straight on (no perspective): the front face (+Z) at `angle` 0, the back face (-Z) at pi, filling most of a 256x256 picture.
static ArrayView<uint32_t> draw_cube(TestAllocator& a, SoftwareDevice& device, const Logo& logo, float angle) {
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
  IImage* color = device.create_image({{256, 256}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IImage* depth = device.create_image({{256, 256}, Format::D32Float, kImageDepthAttachment});
  IImage* texture = device.create_image({{256, 256}, Format::B8G8R8A8Unorm, kImageSampled});
  ICommandBuffer* cb = device.create_command_buffer();
  CHECK(vs && fs && pipeline && vb && ib && color && depth && texture && cb);

  ArrayView<uint32_t> texels = allocate_view<uint32_t>(a, 256 * 256);
  CHECK(logo.render(256, 0x10101A, texels) && texture->write_pixels(ArrayView<const uint32_t>(texels.data(), texels.size())));
  release_view(a, texels);
  ByteView vbytes = vb->map(), ibytes = ib->map();
  build_textured_cube(ArrayView<TexturedVertex>(reinterpret_cast<TexturedVertex*>(vbytes.data()), kCubeVertices),
                      ArrayView<uint16_t>(reinterpret_cast<uint16_t*>(ibytes.data()), kCubeIndices));
  vb->unmap();
  ib->unmap();

  // Depth in this device runs 0 (near) to 1: the z scale is negative so the face at +z (after the turn) is the nearer one.
  const Mat4 mvp = Mat4::translation(0, 0, 0.5f) * Mat4::scaling(0.8f, 0.8f, -0.25f) * Mat4::rotation_y(angle);
  SamplerDesc sampler;
  sampler.filter = Filter::Linear;
  sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
  RenderPassBegin pass;
  pass.color = color;
  pass.depth = depth;
  pass.clear_color_value = {0, 0, 0, 1};
  cb->begin();
  cb->begin_render_pass(pass);
  cb->bind_pipeline(pipeline);
  cb->bind_vertex_buffer(vb);
  cb->bind_index_buffer(ib, IndexType::Uint16);
  cb->bind_texture(0, texture, sampler);
  cb->push_constants(&mvp, sizeof mvp);
  cb->draw_indexed(kCubeIndices);
  cb->end_render_pass();
  CHECK(cb->end() && device.submit(cb));
  ArrayView<uint32_t> out = allocate_view<uint32_t>(a, 256 * 256);
  CHECK(color->read_pixels(out));

  device.destroy_command_buffer(cb);
  device.destroy_image(texture);
  device.destroy_image(depth);
  device.destroy_image(color);
  device.destroy_buffer(ib);
  device.destroy_buffer(vb);
  device.destroy_pipeline(pipeline);
  device.destroy_shader_module(fs);
  device.destroy_shader_module(vs);
  return out;
}

// The mean difference between the drawn face and the logo it should show, over the middle of the face, with the logo optionally mirrored.
static double face_error(const ArrayView<uint32_t>& drawn, const ArrayView<uint32_t>& logo256, bool mirror_x, bool mirror_y) {
  double sum = 0;
  uint32_t n = 0;
  for (uint32_t y = 60; y < 200; y += 3) {
    for (uint32_t x = 60; x < 200; x += 3) {
      const double fx = (x + 0.5 - 25.6) / 204.8, fy = (y + 0.5 - 25.6) / 204.8;   // 0..1 across the face
      const uint32_t lx = static_cast<uint32_t>((mirror_x ? 1 - fx : fx) * 256), ly = static_cast<uint32_t>((mirror_y ? 1 - fy : fy) * 256);
      const uint32_t want = logo256[ly * 256 + lx], got = drawn[y * 256 + x];
      for (int shift : {0, 8, 16}) sum += abs(channel(want, shift) - channel(got, shift));
      n += 3;
    }
  }
  return sum / n;
}

static void test_cube(TestAllocator& a, const std::vector<uint8_t>& png) {
  Logo logo(a);
  CHECK(logo.load(ConstByteView(png.data(), png.size())));
  ArrayView<uint32_t> reference = allocate_view<uint32_t>(a, 256 * 256);
  CHECK(logo.render(256, 0x10101A, reference));
  SoftwareDevice device{a, nullptr};

  for (float angle : {0.0f, 3.14159265f}) {
    ArrayView<uint32_t> drawn = draw_cube(a, device, logo, angle);
    const double right = face_error(drawn, reference, false, false);
    const double mirrored_x = face_error(drawn, reference, true, false);
    const double mirrored_y = face_error(drawn, reference, false, true);
    printf("   angle %.2f: error %.1f, mirrored across %.1f, upside down %.1f\n", angle, right, mirrored_x, mirrored_y);
    CHECK(right < 12.0);
    CHECK(mirrored_x > right * 2 && mirrored_y > right * 2);   // the picture has no symmetry, so a flipped one is far off
    // The corner marks of the tile are where they belong on the face.
    const uint32_t bar = drawn[44 * 256 + 211], dot = drawn[216 * 256 + 40];
    CHECK(channel(bar, 0) > 200 && channel(bar, 8) > 200 && channel(bar, 16) > 200);
    CHECK(channel(dot, 0) > 200 && channel(dot, 8) > 200 && channel(dot, 16) > 200);
    release_view(a, drawn);
  }
  release_view(a, reference);
}

int main(int argc, char** argv) {
  const std::vector<uint8_t> png = read_file(argc > 1 ? argv[1] : "assets/logo.png");
  if (png.empty()) {
    printf("cannot read the logo\n");
    return 1;
  }
  TestAllocator a;
  test_logo(a, png);
  test_damaged(a, png);
  test_cube(a, png);
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("logo: all checks passed\n");
  return 0;
}

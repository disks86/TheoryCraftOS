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

// The Venus device (src/Graphics/Graphics3D/Venus/VenusDevice.hpp) on the build host's own Vulkan driver: its kernel Vulkan table (KernelVulkan.h) is filled
// with the host's loader instead of the Venus backend, so the same device code runs the Direct3D layer's conformance scenes (src/Kernel/Dxgkrnl/Conformance.cpp)
// and the GL ones (src/WebGl/AccelConformance.cpp) against the CPU renderer in seconds, without QEMU. What runs in the guest differs only below the table (the
// Venus protocol); tests/venus_qemu.sh checks that part. Skips (passes) when the host has no Vulkan 1.3 device.
// VENUS_SCENE=<text>: only the scenes whose name contains it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include "Graphics/BlockCompression.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"
#include "Graphics/Graphics3D/LockedDevice.hpp"
#include "Graphics/Graphics3D/Venus/VenusDevice.hpp"
#include "Graphics/Graphics3D/WebGlAccelerator.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "WebGl/AccelConformance.hpp"

#include "VenusHostVulkan.hpp"

// ---- the test --------------------------------------------------------------------------------------------------------------------------------------
struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { return size ? aligned_alloc(alignment < 16 ? 16 : alignment, (size + 63) & ~uint64_t{63}) : nullptr; }
  void deallocate(void* p) override { free(p); }
};

class FakeScreen final : public hardware::IGraphicsProcessingUnit {
 public:
  const char* name() const override { return "fake screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t i, hardware::DisplayMode* out) const override {
    if (i) return false;
    *out = {64, 48};
    return true;
  }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t i) override { return i == 0; }
  uint32_t width() const override { return 64; }
  uint32_t height() const override { return 48; }
  void put_pixel(uint32_t x, uint32_t y, uint32_t c) override {
    if (x < 64 && y < 48) pixels[y * 64 + x] = c;
  }
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  void present() override { ++presents; }
  uint32_t pixels[64 * 48] = {};
  uint32_t presents = 0;
};

static int g_failed = 0;
static const char* g_filter = nullptr;

int main() {
  g_filter = getenv("VENUS_SCENE");
  if (!load_host_vulkan()) {
    printf("no host Vulkan loader: skipped\n");
    return 0;
  }
  HostAllocator allocator;
  FakeScreen screen;
  graphics::VenusDevice device(allocator, screen);
  if (!device.initialize()) {
    printf("no Vulkan 1.3 device on the host: skipped\n");
    return 0;
  }
  printf("host device: %s (%s)\n", device.context().properties().deviceName, device.context().driver_name());
  // present: a 64 x 48 image shown on the screen
  {
    graphics::IImage* image = device.create_image({{64, 48}, graphics::Format::B8G8R8A8Unorm, graphics::kImageColorAttachment | graphics::kImagePresent});
    uint32_t px[64 * 48];
    for (uint32_t i = 0; i < 64 * 48; ++i) px[i] = 0xFF000000u | i;
    const bool ok = image && image->write_pixels(ArrayView<const uint32_t>(px, 64 * 48)) && device.present(image) && screen.presents == 1 &&
                    memcmp(screen.pixels, px, sizeof px) == 0;
    if (!ok) printf("FAILED present\n"), ++g_failed;
    if (image) device.destroy_image(image);
  }
  // a constant colour drawn into a target of each format and read back (the device's own transfers and formats, without the Direct3D layer)
  {
    static const char kVs[] = "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n  0: MOV OUT[0], IN[0]\n  1: END\n";
    static const char kFs[] = "FRAG\nDCL OUT[0], COLOR\nIMM[0] FLT32 {0.300000000000, -0.600000000000, 0.900000000000, 1.000000000000}\n  0: MOV OUT[0], IMM[0]\n  1: END\n";
    graphics::ShaderModuleDesc vd, fd;
    vd.stage = graphics::ShaderStage::Vertex, vd.tgsi = kVs;
    fd.stage = graphics::ShaderStage::Fragment, fd.tgsi = kFs;
    graphics::IShaderModule* vs = device.create_shader_module(vd);
    graphics::IShaderModule* fs = device.create_shader_module(fd);
    graphics::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = 16, pd.attribute_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float4, 0};
    pd.cull_mode = graphics::CullMode::None;
    graphics::IPipeline* pipe = vs && fs ? device.create_graphics_pipeline(pd) : nullptr;
    const float tri[12] = {-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1};
    graphics::IBuffer* vb = device.create_buffer(sizeof tri, graphics::kBufferVertex);
    if (vb) {
      memcpy(vb->map().data(), tri, sizeof tri);
      vb->unmap();
    }
    const graphics::Format formats[] = {graphics::Format::R16G16B16A16Snorm, graphics::Format::R16G16B16A16Float, graphics::Format::R10G10B10A2Unorm};
    for (graphics::Format f : formats) {
      graphics::IImage* image = device.create_image({{4, 4}, f, graphics::kImageColorAttachment | graphics::kImageSampled});
      graphics::ICommandBuffer* cb = device.create_command_buffer();
      bool ok = image && cb && pipe && vb;
      graphics::Vec4 px[16] = {};
      if (ok) {
        graphics::RenderPassBegin pass;
        pass.color = image;
        cb->begin();
        cb->begin_render_pass(pass);
        cb->bind_pipeline(pipe);
        cb->bind_vertex_buffer(vb);
        cb->draw(3);
        cb->end_render_pass();
        ok = cb->end() && device.submit(cb) && image->read_pixels_float(ArrayView<graphics::Vec4>(px, 16));
      }
      printf("  format %u: %s (%.3f, %.3f, %.3f, %.3f)\n", static_cast<unsigned>(f), ok ? "drawn" : "FAILED", px[5].x, px[5].y, px[5].z, px[5].w);
      if (!ok) ++g_failed;
      if (cb) device.destroy_command_buffer(cb);
      if (image) device.destroy_image(image);
    }
    if (vb) device.destroy_buffer(vb);
    if (pipe) device.destroy_pipeline(pipe);
    if (fs) device.destroy_shader_module(fs);
    if (vs) device.destroy_shader_module(vs);
  }
  // Coarse shading (kFeature2ShadingRate) on the device itself: the pipeline's rate, a vertex shader's per-primitive rate and a shading-rate image, each with its
  // combiner. The fragment shader shows where it ran (red: frac(x / 8) of SV_Position, the coarse pixel's centre) and the rate it reads (blue: rate / 16). Rates of
  // 2x2 and less, which every host with the extension has (RADV's coarse pixels stop there: the device says so with kFeature3CoarsePixels2x2Only).
  if (device.info().features2 & graphics::kFeature2ShadingRate) {
    static const char kVs[] = "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\nDCL OUT[1], SHADINGRATE\nIMM[0] UINT32 {5, 5, 5, 5}\n"
                              "  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1].x, IMM[0].xxxx\n  2: END\n";
    static const char kVsPlain[] = "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n  0: MOV OUT[0], IN[0]\n  1: END\n";
    static const char kFs[] = "FRAG\nDCL IN[0], POSITION, LINEAR\nDCL IN[1], SHADINGRATE, CONSTANT\nDCL OUT[0], COLOR\nDCL TEMP[0]\n"
                              "IMM[0] FLT32 {0.125000000000, 0.062500000000, 1.000000000000, 0.000000000000}\n"
                              "  0: MUL TEMP[0].xy, IN[0].xyyy, IMM[0].xxxx\n  1: FRC TEMP[0].xy, TEMP[0].xyyy\n  2: U2F TEMP[0].z, IN[1].xxxx\n"
                              "  3: MUL TEMP[0].z, TEMP[0].zzzz, IMM[0].yyyy\n  4: MOV TEMP[0].w, IMM[0].zzzz\n  5: MOV OUT[0], TEMP[0]\n  6: END\n";
    graphics::ShaderModuleDesc vd, pd_, fd;
    vd.stage = pd_.stage = graphics::ShaderStage::Vertex, vd.tgsi = kVsPlain, pd_.tgsi = kVs;
    fd.stage = graphics::ShaderStage::Fragment, fd.tgsi = kFs;
    graphics::IShaderModule* vs = device.create_shader_module(vd);
    graphics::IShaderModule* vs_rate = device.create_shader_module(pd_);
    graphics::IShaderModule* fs = device.create_shader_module(fd);
    graphics::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = 16, pd.attribute_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float4, 0};
    pd.cull_mode = graphics::CullMode::None;
    graphics::IPipeline* plain = vs && fs ? device.create_graphics_pipeline(pd) : nullptr;
    pd.vertex_shader = vs_rate;
    graphics::IPipeline* primitive = vs_rate && fs ? device.create_graphics_pipeline(pd) : nullptr;
    const float tri[12] = {-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1};
    graphics::IBuffer* vb = device.create_buffer(sizeof tri, graphics::kBufferVertex);
    if (vb) {
      memcpy(vb->map().data(), tri, sizeof tri);
      vb->unmap();
    }
    // the image: 2 x 2 tiles of 16 pixels, the left column 1x1, the right 2x2 (R8_UINT, a wide format: its integers travel as the bits of Vec4 floats)
    graphics::IImage* rates = device.create_image({{2, 2}, graphics::Format::R8Uint, graphics::kImageSampled});
    auto bits = [](uint32_t v) { return __builtin_bit_cast(float, v); };
    const graphics::Vec4 rate_texels[4] = {{bits(0), 0, 0, 0}, {bits(5), 0, 0, 0}, {bits(0), 0, 0, 0}, {bits(5), 0, 0, 0}};
    const bool rates_ok = rates && rates->write_pixels_float(ArrayView<const graphics::Vec4>(rate_texels, 4));
    using C = graphics::ShadingRateCombiner;
    struct Case {
      const char* what;
      graphics::IPipeline* pipe;
      uint32_t rate;
      C primitive, image;
      bool with_image;
      uint32_t red0, red1, red16, red17, blue0, blue16;   // (at pixels x 0, 1, 16, 17 of row 4)
    };
    const Case cases[] = {{"1x1", plain, 0x0, C::Passthrough, C::Passthrough, false, 16, 48, 16, 48, 0, 0},
                          {"2x2 of the pipeline", plain, 0x5, C::Passthrough, C::Passthrough, false, 32, 32, 32, 32, 80, 80},
                          {"2x2 of the primitive", primitive, 0x0, C::Override, C::Passthrough, false, 32, 32, 32, 32, 80, 80},
                          {"1x1 and 2x2 of the image", plain, 0x0, C::Passthrough, C::Override, true, 16, 48, 32, 32, 0, 80},
                          {"2x1 sum the image (4x2 there: 2x2 on the host)", plain, 0x4, C::Passthrough, C::Sum, true, 32, 32, 32, 32, 64, 80}};
    for (const Case& k : cases) {
      graphics::IImage* image = device.create_image({{32, 32}, graphics::Format::B8G8R8A8Unorm, graphics::kImageColorAttachment | graphics::kImageSampled});
      graphics::ICommandBuffer* cb = device.create_command_buffer();
      bool ok = image && cb && k.pipe && vb && (rates_ok || !k.with_image);
      uint32_t px[32 * 32] = {};
      if (ok) {
        graphics::RenderPassBegin pass;
        pass.color = image;
        cb->begin();
        cb->set_shading_rate(k.rate, k.primitive, k.image);
        cb->set_shading_rate_image(k.with_image ? rates : nullptr);
        cb->begin_render_pass(pass);
        cb->bind_pipeline(k.pipe);
        cb->bind_vertex_buffer(vb);
        cb->draw(3);
        cb->end_render_pass();
        ok = cb->end() && device.submit(cb) && image->read_pixels(ArrayView<uint32_t>(px, 32 * 32));
      }
      auto red = [&](uint32_t x) { return px[4 * 32 + x] >> 16 & 0xFF; };
      auto blue = [&](uint32_t x) { return px[4 * 32 + x] & 0xFF; };
      auto near = [](uint32_t a, uint32_t b) { return a + 1 >= b && a <= b + 1; };
      const bool right = ok && near(red(0), k.red0) && near(red(1), k.red1) && near(red(16), k.red16) && near(red(17), k.red17) && near(blue(0), k.blue0) && near(blue(16), k.blue16);
      printf("  shading rate %s: %s (red %u %u %u %u, blue %u %u%s)\n", k.what, right ? "right" : "FAILED", red(0), red(1), red(16), red(17), blue(0), blue(16), ok ? "" : rates_ok ? ", not drawn" : ", no image");
      if (!right) ++g_failed;
      if (cb) device.destroy_command_buffer(cb);
      if (image) device.destroy_image(image);
    }
    if (rates) device.destroy_image(rates);
    if (vb) device.destroy_buffer(vb);
    if (primitive) device.destroy_pipeline(primitive);
    if (plain) device.destroy_pipeline(plain);
    if (fs) device.destroy_shader_module(fs);
    if (vs_rate) device.destroy_shader_module(vs_rate);
    if (vs) device.destroy_shader_module(vs);
  }
  // A submit the device leaves running (ICommandBuffer::defer_completion): whatever reads it afterwards waits for it - a query's count, an image's pixels, map() -,
  // the next submit too, and from several threads at once (through LockedDevice, as the kernel shares the device).
  {
    static const char kVs[] = "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n  0: MOV OUT[0], IN[0]\n  1: END\n";
    static const char kFs[] = "FRAG\nDCL OUT[0], COLOR\nIMM[0] FLT32 {0.300000000000, 0.000000000000, 0.900000000000, 1.000000000000}\n  0: MOV OUT[0], IMM[0]\n  1: END\n";
    graphics::LockedDevice locked(device);
    graphics::ShaderModuleDesc vd, fd;
    vd.stage = graphics::ShaderStage::Vertex, vd.tgsi = kVs;
    fd.stage = graphics::ShaderStage::Fragment, fd.tgsi = kFs;
    graphics::IShaderModule* vs = locked.create_shader_module(vd);
    graphics::IShaderModule* fs = locked.create_shader_module(fd);
    graphics::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = 16, pd.attribute_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float4, 0};
    pd.cull_mode = graphics::CullMode::None;
    graphics::IPipeline* pipe = vs && fs ? locked.create_graphics_pipeline(pd) : nullptr;
    const float tri[12] = {-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1};
    graphics::IBuffer* vb = locked.create_buffer(sizeof tri, graphics::kBufferVertex);
    if (vb) {
      memcpy(vb->map().data(), tri, sizeof tri);
      vb->unmap();
    }
    // (B8G8R8A8: 0.3 and 0.9 are 76.5 and 229.5, either way)
    auto right_colour = [](uint32_t px) {
      const uint32_t r = px >> 16 & 0xFF, g = px >> 8 & 0xFF, b = px & 0xFF;
      return (px >> 24) == 0xFF && r >= 76 && r <= 77 && g == 0 && b >= 229 && b <= 230;
    };
    // draws the triangle (the whole of a w x w target) into `image`, counted by occlusion query `slot` unless it is ~0, and submits it left running
    auto draw = [&](graphics::ICommandBuffer* cb, graphics::IImage* image, uint32_t slot) {
      graphics::RenderPassBegin pass;
      pass.color = image;
      pass.clear_color = true;
      cb->defer_completion(true);
      cb->begin();
      if (slot != ~0u) cb->begin_query(slot, graphics::QueryKind::Occlusion);
      cb->begin_render_pass(pass);
      cb->bind_pipeline(pipe);
      cb->bind_vertex_buffer(vb);
      cb->draw(3);
      cb->end_render_pass();
      if (slot != ~0u) cb->end_query(slot);
      return cb->end() && locked.submit(cb);
    };
    {
      graphics::IImage* image = locked.create_image({{4, 4}, graphics::Format::B8G8R8A8Unorm, graphics::kImageColorAttachment | graphics::kImageSampled});
      graphics::ICommandBuffer* cb = locked.create_command_buffer();
      graphics::ICommandBuffer* second = locked.create_command_buffer();
      uint64_t samples = 0, again = 0;
      uint32_t px[16] = {};
      const bool drawn = image && cb && second && pipe && vb && draw(cb, image, 3);
      const bool pending_before = drawn && !cb->query_results_ready();   // (the count comes with the wait)
      const bool counted = drawn && cb->query_value(3, 0, &samples) && cb->query_results_ready();
      // (a second submit left running while the first's counts are kept: the next one waits for it, and reading the image waits for the last)
      const bool drawn2 = drawn && draw(second, image, 5) && draw(second, image, ~0u);
      const bool read = drawn2 && image->read_pixels(ArrayView<uint32_t>(px, 16));
      const bool kept = counted && cb->query_value(3, 0, &again) && again == samples;
      bool all = read;
      for (uint32_t p : px) all = all && right_colour(p);
      const bool ok = pending_before && counted && samples == 16 && kept && all;
      printf("  left running: %s (count %llu, pending first %d, pixel 0x%08x)\n", ok ? "right" : "FAILED", static_cast<unsigned long long>(samples), pending_before ? 1 : 0, px[5]);
      if (!ok) ++g_failed;
      // (a command buffer let go of while its counts are still to come)
      graphics::ICommandBuffer* dropped = locked.create_command_buffer();
      if (dropped && image && draw(dropped, image, 1)) locked.destroy_command_buffer(dropped);
      else if (dropped) locked.destroy_command_buffer(dropped), ++g_failed, printf("FAILED left running: drop\n");
      if (second) locked.destroy_command_buffer(second);
      if (cb) locked.destroy_command_buffer(cb);
      if (image) locked.destroy_image(image);
    }
    // threads: two draw and count into images of their own, left running; one writes and reads back buffers of its own through map() meanwhile
    {
      std::atomic<int> failures{0};
      auto drawer = [&](uint32_t rounds) {
        graphics::IImage* image = locked.create_image({{8, 8}, graphics::Format::B8G8R8A8Unorm, graphics::kImageColorAttachment | graphics::kImageSampled});
        graphics::ICommandBuffer* cb = locked.create_command_buffer();
        if (!image || !cb) ++failures;
        for (uint32_t i = 0; image && cb && i < rounds; ++i) {
          uint64_t samples = 0;
          uint32_t px[64] = {};
          if (!draw(cb, image, i % 7) || !cb->query_value(i % 7, 0, &samples) || samples != 64) ++failures;
          if (i % 8 == 7 && (!image->read_pixels(ArrayView<uint32_t>(px, 64)) || !right_colour(px[27]))) ++failures;
        }
        if (cb) locked.destroy_command_buffer(cb);
        if (image) locked.destroy_image(image);
      };
      auto mapper = [&](uint32_t rounds) {
        for (uint32_t i = 0; i < rounds; ++i) {
          graphics::IBuffer* b = locked.create_buffer(4096, graphics::kBufferVertex);
          if (!b) {
            ++failures;
            continue;
          }
          ByteView m = b->map();
          for (uint64_t k = 0; k < m.size(); ++k) m[k] = static_cast<uint8_t>(i + k);
          b->unmap();
          ConstByteView back = b->map();
          for (uint64_t k = 0; k < back.size(); ++k)
            if (back[k] != static_cast<uint8_t>(i + k)) {
              ++failures;
              break;
            }
          locked.destroy_buffer(b);
        }
      };
      std::thread a(drawer, 120), b(drawer, 120), c(mapper, 200);
      a.join(), b.join(), c.join();
      const bool ok = failures.load() == 0 && pipe && vb;
      printf("  left running, three threads: %s (%d failures)\n", ok ? "right" : "FAILED", failures.load());
      if (!ok) ++g_failed;
    }
    if (vb) locked.destroy_buffer(vb);
    if (pipe) locked.destroy_pipeline(pipe);
    if (fs) locked.destroy_shader_module(fs);
    if (vs) locked.destroy_shader_module(vs);
  }
  // what format_caps says of each format is what create_image does (the kernel hands the caps to user mode as format support)
  for (uint32_t fi = 64; fi < 128; ++fi) {
    const graphics::Format f = static_cast<graphics::Format>(fi);
    const uint32_t caps = device.format_caps(f);
    if (!caps) continue;
    struct Probe {
      uint32_t cap, usage;
      const char* what;
    };
    const Probe probes[] = {{graphics::kFormatSampled, graphics::kImageSampled, "texture"},
                            {graphics::kFormatRenderTarget, graphics::kImageSampled | graphics::kImageColorAttachment, "render target"},
                            {graphics::kFormatStorage, graphics::kImageSampled | graphics::kImageStorage, "storage image"}};
    for (const Probe& pr : probes) {
      if (!(caps & pr.cap)) continue;
      graphics::ImageDesc d{{4, 4}, f, pr.usage};
      if (graphics::is_compressed_format(f)) d.extent = {8, 8};
      graphics::IImage* image = device.create_image(d);
      if (!image) printf("FAILED format %u: the caps say %s, create_image refuses it\n", fi, pr.what), ++g_failed;
      else device.destroy_image(image);
    }
  }
  // the Direct3D layer's scenes
  {
    graphics::SoftwareDevice reference(allocator, &screen);
    dxgk::Kernel kernel(allocator);
    dxgk::AdapterConfig c;
    c.display = &screen;
    if (kernel.add_adapter(device, c) != 0 || kernel.add_adapter(reference, c) != 1) {
      printf("FAILED adapters\n");
      return 1;
    }
    struct Count {
      int same = 0, differs = 0, refused = 0, broken = 0;
    } n;
    dxgk::run_conformance(allocator, kernel, 0, 1, [](void* context, const dxgk::SceneResult& r) {
      Count& k = *static_cast<Count*>(context);
      if (g_filter && !strstr(r.name, g_filter)) return;
      switch (r.verdict) {
        case dxgk::SceneVerdict::Same: ++k.same; break;
        case dxgk::SceneVerdict::Differs:
          ++k.differs;
          printf("FAILED Direct3D %s: differs in %u pixels, first %u,%u: 0x%x instead of 0x%x\n", r.name, r.bad_pixels, r.first_bad % 32, r.first_bad / 32, r.device_value, r.reference_value);
          if (getenv("VENUS_DUMP") && r.device_picture && r.reference_picture)   // (rows of the two pictures, every second pixel)
            for (uint32_t y = 0; y < 32; y += 2) {
              for (uint32_t x = 0; x < 32; x += 2) printf("%06x%c", r.device_picture[y * 32 + x] & 0xFFFFFF, r.device_picture[y * 32 + x] == r.reference_picture[y * 32 + x] ? ' ' : '*');
              printf("   ");
              for (uint32_t x = 0; x < 32; x += 2) printf("%06x ", r.reference_picture[y * 32 + x] & 0xFFFFFF);
              printf("\n");
            }
          break;
        case dxgk::SceneVerdict::Refused: ++k.refused; printf("  Direct3D %s: not supported (%s)\n", r.name, r.step); break;
        case dxgk::SceneVerdict::Broken: ++k.broken; printf("FAILED Direct3D %s: broken at %s, status 0x%x\n", r.name, r.step, static_cast<unsigned>(r.status)); break;
      }
    }, &n);
    printf("Direct3D scenes: %d same, %d differ, %d refused, %d broken\n", n.same, n.differs, n.refused, n.broken);
    g_failed += n.differs + n.broken;
  }
  // the GL translator's scenes
  {
    graphics::WebGlAccelerator accelerator(device, allocator);
    struct Count {
      int same = 0, differs = 0, not_taken = 0, broken = 0;
    } n;
    webgl::run_accel_conformance(allocator, accelerator, [](void* context, const webgl::AccelSceneResult& r) {
      Count& k = *static_cast<Count*>(context);
      if (g_filter && !strstr(r.name, g_filter)) return;
      switch (r.verdict) {
        case webgl::AccelVerdict::Same: ++k.same; break;
        case webgl::AccelVerdict::Differs:
          ++k.differs;
          printf("FAILED GL %s: %u channels differ, first %u,%u channel %u: %d instead of %d\n", r.name, r.bad_channels, r.first_x, r.first_y, r.first_channel, r.device_value, r.cpu_value);
          break;
        case webgl::AccelVerdict::NotTaken: ++k.not_taken; printf("  GL %s: not taken (%s)\n", r.name, r.why ? r.why : "-"); break;
        default: ++k.broken; printf("FAILED GL %s: broken\n", r.name); break;
      }
    }, &n);
    printf("GL scenes: %d same, %d differ, %d not taken, %d broken\n", n.same, n.differs, n.not_taken, n.broken);
    g_failed += n.differs + n.broken;
  }
  printf("%d failed\n", g_failed);
  return g_failed ? 1 : 0;
}

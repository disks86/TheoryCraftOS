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
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"
#include "Graphics/Graphics3D/Venus/VenusDevice.hpp"
#include "Graphics/Graphics3D/WebGlAccelerator.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "WebGl/AccelConformance.hpp"

// ---- the host's Vulkan as the kernel's table -----------------------------------------------------------------------------------------------------------
namespace {
void* g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_instance = nullptr;
TcosKernelVulkan g_table;

PFN_vkVoidFunction proc(const char* name) {
  PFN_vkVoidFunction f = g_gipa(g_instance, name);
  if (!f) {
    fprintf(stderr, "the host's Vulkan has no %s\n", name);
    abort();
  }
  return f;
}

// (functions looked up once the instance exists: every one of the table goes through these)
#define TRAMPOLINE(name, ret, params, args)                                                     \
  ret VKAPI_CALL t_##name params {                                                             \
    static PFN_vk##name f = reinterpret_cast<PFN_vk##name>(proc("vk" #name));                  \
    return f args;                                                                             \
  }
VkResult VKAPI_CALL t_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
  auto f = reinterpret_cast<PFN_vkCreateInstance>(g_gipa(nullptr, "vkCreateInstance"));
  const VkResult r = f(ci, a, out);
  if (r == VK_SUCCESS) g_instance = *out;
  return r;
}
}  // namespace

#include "VenusHostVulkan.inc"

extern "C" {
const TcosKernelVulkan* tcos_kernel_vulkan(void) { return &g_table; }
bool tcos_kernel_vulkan_available(void) { return g_gipa != nullptr; }
void* tcos_kvk_alloc(size_t bytes) { return calloc(1, bytes ? bytes : 1); }
void tcos_kvk_free(void* p) { free(p); }
void tcos_kvk_trace(const char* text) { printf("  %s\n", text); }
}

static bool load_host_vulkan() {
  g_loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!g_loader) return false;
  g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
  if (!g_gipa) return false;
  fill_table(&g_table);
  return true;
}

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

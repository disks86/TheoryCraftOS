// A check of a 3D device through the kernel-mode Direct3D driver (src/Kernel/Dxgkrnl): the same Direct3D 9 shader pair, pipeline and draw is made on the device and on the software
// device and the pictures compared. Any new IGraphicsDevice (the AMD one, for instance) should pass it: it is what a Direct3D application's first triangle needs from the device.
//
// The scene is dxgk::self_test_render (SelfTest.hpp), which the system also runs at boot on each accelerated GPU. The shaders are the fixed-function kind every device can take in some form: vs_2_0 `m4x4 oPos, v0, c0 ; mov oD0, v1` (position float3, colour float4, the matrix as the rows dp4
// takes) and ps_2_0 `mov oC0, v0`. What a device does with them is its own: run the interpreter's function, take TGSI text, take the Direct3D 9 tokens, or recognise the
// fixed kernels (Adapter::create_shader hands each device every form).
//
//   dxgk_suite::render(allocator, device, display, &pixels, &report)   -> 32 x 32 words (B8G8R8A8), false (and a report) where a step failed
//   dxgk_suite::compare(...)                                           -> renders on `device` and on a software device and compares
//   dxgk_suite::conformance(allocator, device, display, name, &counts) -> every scene of dxgk::run_conformance on `device` against a software device; the
//                                                                        number of scenes it drew differently or broke (each printed), 0 when it draws or refuses each
//                                                                        (CONFORMANCE_PICTURES=1 prints both pictures of a scene that differs, CONFORMANCE_REFUSALS=1
//                                                                        the step each refused scene stopped at)
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Kernel/Dxgkrnl/SelfTest.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk_suite {

using namespace dxgk;

struct Report {
  std::string failure;       // the first step that failed
  bool stencil = false, mipmaps = false;   // what the adapter says the device can do
  AdapterCaps caps;
};

inline ConstByteView bytes_of(const void* p, uint64_t n) { return ConstByteView(static_cast<const uint8_t*>(p), n); }

// Renders the triangle on `device` (the self-test of src/Kernel/Dxgkrnl/SelfTest.hpp). `display` may be null.
inline bool render(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, hardware::IGraphicsProcessingUnit* display, std::vector<uint32_t>* pixels, Report* report) {
  Kernel kernel(allocator);
  AdapterConfig config;
  config.display = display;
  if (kernel.add_adapter(device, config) == kInvalidIndex) {
    report->failure = "add_adapter";
    return false;
  }
  report->stencil = kernel.adapter(0)->caps().stencil, report->mipmaps = kernel.adapter(0)->caps().mipmaps;
  report->caps = kernel.adapter(0)->caps();
  pixels->assign(uint64_t{dxgk::kSelfTestSize} * dxgk::kSelfTestSize, 0);
  const char* step = "";
  const NtStatus s = dxgk::self_test_render(allocator, kernel, 0, ArrayView<uint32_t>(pixels->data(), pixels->size()), &step);
  if (s != status::kSuccess) {
    char text[96];
    snprintf(text, sizeof text, "%s: %08x", step, static_cast<unsigned>(s));
    report->failure = text;
    return false;
  }
  return true;
}

// Renders on `device` and on a software device; `allowed_bad` is the number of pixels that may differ by more than `tolerance` in a channel (rasterization rules differ at edges).
// Returns the number of failures (0: good), printing what failed.
inline int compare(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, hardware::IGraphicsProcessingUnit* display, const char* name, uint32_t tolerance, uint32_t allowed_bad,
                   bool compare_pixels = true) {
  int failures = 0;
  std::vector<uint32_t> got, want;
  Report report, reference_report;
  graphics::SoftwareDevice software(allocator, nullptr);
  if (!render(allocator, software, nullptr, &want, &reference_report)) {
    printf("  device suite: the software device failed (%s)\n", reference_report.failure.c_str());
    ++failures;
  }
  if (!render(allocator, device, display, &got, &report)) {
    printf("  device suite: %s failed at %s\n", name, report.failure.c_str());
    return failures + 1;
  }
  if (!compare_pixels || want.size() != got.size()) return failures;
  uint32_t bad = 0, lit = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    uint32_t worst = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const int a = static_cast<int>((got[i] >> shift) & 0xFF), b = static_cast<int>((want[i] >> shift) & 0xFF);
      const uint32_t dist = static_cast<uint32_t>(a > b ? a - b : b - a);
      if (dist > worst) worst = dist;
    }
    if (worst > tolerance) ++bad;
    if (got[i] != 0xFF000000u) ++lit;
  }
  if (bad > allowed_bad || lit < 100) {
    printf("  device suite: %s differs from the software device in %u of %zu pixels (%u lit)\n", name, bad, got.size(), lit);
    ++failures;
  }
  return failures;
}

struct ConformanceCounts {
  uint32_t same = 0, refused = 0, differs = 0, broken = 0;
};

// The scenes the system runs at boot on each accelerated GPU (Conformance.hpp), on `device` against a software device. A device may refuse a scene (it says so, a program
// falls back) but never draw something else or break: the number of scenes that did is returned, each printed with `name`.
inline uint32_t conformance(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, hardware::IGraphicsProcessingUnit* display, const char* name,
                            ConformanceCounts* counts) {
  struct Context {
    const char* name;
    ConformanceCounts* counts;
  } context{name, counts};
  graphics::SoftwareDevice reference(allocator, nullptr);
  Kernel kernel(allocator);
  AdapterConfig config;
  config.display = display;
  if (kernel.add_adapter(device, config) != 0 || kernel.add_adapter(reference, AdapterConfig()) != 1) {
    printf("  conformance: %s: add_adapter failed\n", name);
    return 1;
  }
  return run_conformance(allocator, kernel, 0, 1, [](void* raw, const SceneResult& r) {
    Context& c = *static_cast<Context*>(raw);
    switch (r.verdict) {
      case SceneVerdict::Same: ++c.counts->same; break;
      case SceneVerdict::Refused:
        ++c.counts->refused;
        if (getenv("CONFORMANCE_REFUSALS")) printf("  conformance: %s: %s refused at %s: %08x\n", c.name, r.name, r.step, static_cast<unsigned>(r.status));
        break;
      case SceneVerdict::Differs:
        ++c.counts->differs;
        printf("  conformance: %s: %s differs in %u pixels (first %u,%u: %08x, reference %08x)\n", c.name, r.name, r.bad_pixels, r.first_bad % 32, r.first_bad / 32,
               r.device_value, r.reference_value);
        if (getenv("CONFORMANCE_PICTURES") && r.device_picture && r.reference_picture) {   // (each pixel as its strongest channel: r g b, w white, . black)
          for (uint32_t y = 0; y < 32; ++y) {
            char line[2][33];
            for (int side = 0; side < 2; ++side) {
              for (uint32_t x = 0; x < 32; ++x) {
                const uint32_t px = (side ? r.reference_picture : r.device_picture)[y * 32 + x];
                const uint32_t red = (px >> 16) & 0xFF, green = (px >> 8) & 0xFF, blue = px & 0xFF;
                line[side][x] = red > 160 && green > 160 && blue > 160 ? 'w' : red > green && red > blue && red > 40 ? 'r' : green > blue && green > 40 ? 'g' : blue > 40 ? 'b' : '.';
              }
              line[side][32] = 0;
            }
            printf("    %s   %s\n", line[0], line[1]);
          }
        }
        break;
      case SceneVerdict::Broken:
        ++c.counts->broken;
        printf("  conformance: %s: %s broken at %s: %08x\n", c.name, r.name, r.step, static_cast<unsigned>(r.status));
        break;
    }
  }, &context);
}

}  // namespace dxgk_suite

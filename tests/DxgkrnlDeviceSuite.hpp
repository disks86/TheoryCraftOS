// A check of a 3D device through the kernel-mode Direct3D driver (src/Kernel/Dxgkrnl): the same Direct3D 9 shader pair, pipeline and draw is made on the device and on the software
// device and the pictures compared. Any new IGraphicsDevice (the AMD one, for instance) should pass it: it is what a Direct3D application's first triangle needs from the device.
//
// The scene is dxgk::self_test_render (SelfTest.hpp), which the system also runs at boot on each accelerated GPU. The shaders are the fixed-function kind every device can take in some form: vs_2_0 `m4x4 oPos, v0, c0 ; mov oD0, v1` (position float3, colour float4, the matrix as the rows dp4
// takes) and ps_2_0 `mov oC0, v0`. What a device does with them is its own: run the interpreter's function, take TGSI text, take the Direct3D 9 tokens, or recognise the
// fixed kernels (Adapter::create_shader hands each device every form).
//
//   dxgk_suite::render(allocator, device, display, &pixels, &report)   -> 32 x 32 words (B8G8R8A8), false (and a report) where a step failed
//   dxgk_suite::compare(...)                                           -> renders on `device` and on a software device and compares
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
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

}  // namespace dxgk_suite

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

// Host-side tests for the golden-image helper (tests/PixelGolden.hpp) the layout and paint tests are built on.
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "PixelGolden.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::vector<uint32_t> picture(uint32_t w, uint32_t h) {
  std::vector<uint32_t> p(w * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) p[y * w + x] = 0xFF000000u | (x * 16) << 16 | (y * 16) << 8 | 0x40;
  return p;
}

static ArrayView<const uint32_t> view(const std::vector<uint32_t>& v) { return ArrayView<const uint32_t>(v.data(), v.size()); }

int main() {
  TestAllocator a;
  {
    char dir[] = "/tmp/tcos-golden-test-XXXXXX";
    if (!mkdtemp(dir)) { printf("no temp dir\n"); return 1; }
    const std::string root = dir;
    setenv("TCOS_GOLDEN_DIR", (root + "/golden").c_str(), 1);
    setenv("TCOS_GOLDEN_OUT", (root + "/out").c_str(), 1);
    unsetenv("TCOS_UPDATE_GOLDEN");

    const std::vector<uint32_t> base = picture(16, 8);

    // no golden yet: a failure that says how to make one
    golden::Report r = golden::check(a, "box", view(base), 16, 8);
    CHECK(!r.ok && r.message.find("TCOS_UPDATE_GOLDEN") != std::string::npos);

    // update mode writes it, then it matches exactly
    setenv("TCOS_UPDATE_GOLDEN", "1", 1);
    r = golden::check(a, "box", view(base), 16, 8);
    CHECK(r.ok && r.wrote);
    unsetenv("TCOS_UPDATE_GOLDEN");
    r = golden::check(a, "box", view(base), 16, 8);
    CHECK(r.ok && !r.wrote && r.differing == 0 && r.max_delta == 0);

    // a small colour change passes with a tolerance, fails without one
    std::vector<uint32_t> near = base;
    near[5] += 0x00000003;   // blue channel + 3
    r = golden::check(a, "box", view(near), 16, 8, 0);
    CHECK(!r.ok && r.differing == 1 && r.max_delta == 3);
    r = golden::check(a, "box", view(near), 16, 8, 3);
    CHECK(r.ok && r.max_delta == 3);

    // a moved or changed pixel is found and the actual and diff pictures are written
    std::vector<uint32_t> moved = base;
    moved[20] = 0xFFFFFFFFu;
    moved[21] = 0xFF00FF00u;
    r = golden::check(a, "box", view(moved), 16, 8, 8);
    CHECK(!r.ok && r.differing == 2 && r.message.find("box.actual.png") != std::string::npos);
    ByteBuffer actual(a), diff(a);
    CHECK(golden::detail::read_file(root + "/out/box.actual.png", &actual) && actual.size() > 0);
    CHECK(golden::detail::read_file(root + "/out/box.diff.png", &diff) && diff.size() > 0);
    media::Image decoded(a);
    CHECK(media::decode_image(diff.view(), &decoded) && decoded.width() == 16 && decoded.frame(0)[20] == 0xFFFF0000u);

    // a different size, a wrong pixel count and an alpha difference are all refused
    r = golden::check(a, "box", view(picture(8, 8)), 8, 8);
    CHECK(!r.ok && r.message.find("16x8") != std::string::npos);
    r = golden::check(a, "box", view(base), 4, 4);
    CHECK(!r.ok);
    std::vector<uint32_t> faint = base;
    faint[0] = (faint[0] & 0x00FFFFFFu) | 0x80000000u;
    r = golden::check(a, "box", view(faint), 16, 8, 16);
    CHECK(!r.ok && r.differing == 1);
  }
  CHECK(a.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("pixel golden tests passed\n");
  return 0;
}

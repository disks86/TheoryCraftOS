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

// Host-side tests for the system clipboard.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Kernel/Clipboard.hpp"

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

int main() {
  using kernel::Clipboard;
  using kernel::ClipboardFormat;
  TestAllocator a;
  {
    Clipboard cb(a);
    String text(a);
    uint32_t w = 0, h = 0;
    CHECK(cb.format() == ClipboardFormat::Empty && !cb.has_text() && !cb.has_image() && cb.sequence() == 0);
    CHECK(!cb.get_text(&text) && !cb.image_size(&w, &h));

    // Text is copied in and copied out again.
    char original[] = "hello clipboard";
    CHECK(cb.set_text(original) && cb.has_text() && cb.sequence() == 1);
    original[0] = 'J';                                           // changing the owner's copy changes nothing
    CHECK(cb.get_text(&text) && text.view().equals("hello clipboard"));
    CHECK(!cb.set_text("") && cb.sequence() == 1 && cb.has_text());   // empty text is not a copy
    CHECK(!cb.image_size(&w, &h));

    // A picture replaces the text.
    const uint32_t pixels[6] = {1, 2, 3, 4, 5, 6};
    CHECK(cb.set_image(pixels, 3, 2) && cb.has_image() && !cb.has_text() && cb.sequence() == 2);
    CHECK(cb.image_size(&w, &h) && w == 3 && h == 2 && !cb.get_text(&text));
    uint32_t out[6] = {};
    CHECK(cb.get_image(out, 6) && !memcmp(out, pixels, sizeof pixels));
    CHECK(!cb.get_image(out, 5));                                  // too small
    CHECK(!cb.set_image(nullptr, 3, 2) && !cb.set_image(pixels, 0, 2) && !cb.set_image(pixels, 3, 0) && cb.sequence() == 2);
    CHECK(!cb.set_image(pixels, 70000, 70000));                    // over the limit (and never read)
    CHECK(cb.has_image());

    // A failed copy leaves the old contents alone.
    a.fail = true;
    CHECK(!cb.set_text("no room") && cb.has_image() && cb.sequence() == 2);
    a.fail = false;

    // Clearing, and the count of changes.
    cb.clear();
    CHECK(cb.format() == ClipboardFormat::Empty && cb.sequence() == 3);
    cb.clear();
    CHECK(cb.sequence() == 3);
    String big(a);
    for (int i = 0; i < 1000; ++i) big.append("0123456789");
    CHECK(cb.set_text(big.view()) && cb.get_text(&text) && text.size() == 10000 && cb.sequence() == 4);
    CHECK(cb.set_text("x") && cb.get_text(&text) && text.view().equals("x"));
  }
  CHECK(a.live == 0);
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("clipboard: all checks passed\n");
  return 0;
}

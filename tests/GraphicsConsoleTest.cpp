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

// Host-side tests for when the graphics console presents. Presenting on a GPU whose 3D images are shown directly (virtio-gpu) takes the screen back from
// whatever image has it, so a flush with nothing new to show must not reach the GPU: that was a frame of old console text in the middle of a spinning cube.
#include <stdio.h>
#include "GraphicsConsole.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
class FakeGpu final : public hardware::IGraphicsProcessingUnit {
 public:
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t, hardware::DisplayMode*) const override { return false; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return false; }
  uint32_t width() const override { return 800; }
  uint32_t height() const override { return 600; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override { ++drawn; }
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { ++drawn; }
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { ++drawn; }
  void clear(uint32_t) override { ++drawn; }
  void blit(uint32_t, uint32_t, uint32_t, uint32_t, const uint32_t*, uint32_t) override { ++drawn; }
  void present() override { ++presents; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override { ++drawn; }
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override { ++drawn; }

  int drawn = 0;
  int presents = 0;
};

// A flush with nothing drawn since the last one leaves the GPU alone, however often it comes.
void test_idle_flush_does_not_present() {
  FakeGpu gpu;
  GraphicsConsole console;
  console.attach(&gpu);
  console.flush();   // (the cleared screen goes out)
  CHECK(gpu.presents == 1);
  for (int i = 0; i < 100; ++i) console.flush();
  CHECK(gpu.presents == 1);
}

// Whatever the console draws is shown, once, by the next flush.
void test_each_kind_of_drawing_is_shown() {
  FakeGpu gpu;
  GraphicsConsole console;
  console.attach(&gpu);
  console.flush();

  console.put('x');
  console.flush();
  CHECK(gpu.presents == 2);
  console.flush();
  CHECK(gpu.presents == 2);

  console.put('a');
  console.put('b');
  console.put('c');
  console.flush();
  CHECK(gpu.presents == 3);

  // Scrolling at the bottom of the screen moves what is shown even though no character is drawn.
  for (int i = 0; i < 100; ++i) console.put('\n');
  console.flush();
  CHECK(gpu.presents == 4);

  const uint32_t pixels[4] = {1, 2, 3, 4};
  console.show_banner(ArrayView<const uint32_t>(pixels, 4), 2, 2);
  console.flush();
  CHECK(gpu.presents == 5);
  console.flush();
  CHECK(gpu.presents == 5);
}

// A character that changes nothing on the screen gives the next flush nothing to do.
void test_carriage_return_is_not_drawing() {
  FakeGpu gpu;
  GraphicsConsole console;
  console.attach(&gpu);
  console.flush();
  console.put('\r');
  console.flush();
  CHECK(gpu.presents == 1);
}

// A newline that only moves the cursor down changes nothing on the screen either.
void test_newline_without_scroll_is_not_drawing() {
  FakeGpu gpu;
  GraphicsConsole console;
  console.attach(&gpu);
  console.flush();
  console.put('\n');
  console.flush();
  CHECK(gpu.presents == 1);
}

// Attaching clears the screen, and that has to be shown even if no text follows; a console with no GPU has nothing to present.
void test_attach() {
  GraphicsConsole console;
  console.flush();
  console.put('x');
  CHECK(!console.attached());

  FakeGpu first;
  console.attach(&first);
  console.flush();
  CHECK(first.presents == 1);

  FakeGpu second;
  console.put('y');   // (a change that first never gets to show: it is no longer attached)
  console.attach(&second);
  console.flush();
  CHECK(first.presents == 1);
  CHECK(second.presents == 1);

  console.attach(nullptr);
  console.flush();
  CHECK(second.presents == 1);
}
}  // namespace

int main() {
  test_idle_flush_does_not_present();
  test_each_kind_of_drawing_is_shown();
  test_carriage_return_is_not_drawing();
  test_newline_without_scroll_is_not_drawing();
  test_attach();
  printf(g_failed ? "GraphicsConsoleTest: %d FAILED\n" : "GraphicsConsoleTest: all ok\n", g_failed);
  return g_failed ? 1 : 0;
}

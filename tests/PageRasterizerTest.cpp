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

// PageRasterizer on real threads (tests/HostThreads.cpp): a page is drawn on its own thread; the side that shows it (the desktop's render thread) only
// takes what is finished, and is never held up by a drawing in progress, however long that takes. Also built with ThreadSanitizer.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "Gui/Controls/PageRasterizer.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Thread.hpp"

using namespace gui;

namespace {
int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct HeapAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t) override { return size ? malloc(size) : nullptr; }
  void deallocate(void* p) override { free(p); }
};

uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t{static_cast<uint64_t>(ts.tv_sec)} * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

// Draws node n in its colour; counts what it was asked to draw; can take its time (a page that is slow to draw).
struct Objects final : browser::IObjectPainter {
  uint32_t colors[3] = {0, 0xFF00FF00u, 0xFF0000FFu};
  int calls[3] = {0, 0, 0};
  uint64_t delay_us = 0;
  void paint_object(graphics::IRenderer2D& r, uint32_t node, browser::ObjectKind, int32_t x, int32_t y, uint32_t w, uint32_t h, const graphics::Rect2D&) override {
    if (node > 2) return;
    __atomic_add_fetch(&calls[node], 1, __ATOMIC_ACQ_REL);
    const uint64_t d = __atomic_load_n(&delay_us, __ATOMIC_ACQUIRE);
    if (d) kernel::sleep_us(d);
    r.fill_rect({x, y, w, h}, __atomic_load_n(&colors[node], __ATOMIC_ACQUIRE));
  }
  int count(uint32_t node) { return __atomic_load_n(&calls[node], __ATOMIC_ACQUIRE); }
};

SharedPointer<browser::PagePicture> page(HeapAllocator& a) {
  auto picture = SharedPointer<browser::PagePicture>::make(a, a);
  paint::Op background;
  background.kind = paint::OpKind::FillRect;
  background.rect = {0, 0, 120, 300};
  background.color = 0xFF202020u;
  picture->list.add(background);
  for (uint32_t node = 1; node <= 2; ++node) {
    paint::Op video;
    video.kind = paint::OpKind::Object;
    video.node = node;
    video.rect = {10, node == 1 ? 10 : 60, 40, 30};
    picture->list.add(video);
  }
  picture->list.set_extent(120, 300);
  return picture;
}

int g_ready = 0;
void ready(void*) { __atomic_add_fetch(&g_ready, 1, __ATOMIC_ACQ_REL); }

// What the view would do: the finished picture's damage, and a pixel of it.
struct Taken {
  bool got = false;
  graphics::Rect2D damage;
  uint32_t px[2] = {0, 0};
};
Taken take(PageRasterizer& r, bool all = false) {
  Taken t;
  t.got = r.take(all, [&](const graphics::Surface& front, const graphics::Rect2D& damage) {
    t.damage = damage;
    t.px[0] = front.row(20)[20];   // in video 1
    t.px[1] = front.row(70)[20];   // in video 2
  });
  return t;
}
}  // namespace

static void test_drawing_and_damage() {
  HeapAllocator a;
  Objects objects;
  PageRasterizer r(a);
  r.set_object_painter(&objects);
  CHECK(r.start(&ready, nullptr));
  CHECK(!take(r).got);   // (nothing drawn yet)
  r.request(page(a), 1, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>());
  CHECK(r.wait_idle(5000000));
  Taken t = take(r);
  CHECK(t.got && t.damage.x == 0 && t.damage.y == 0 && t.damage.width == 120 && t.damage.height == 100);   // the first drawing: all of it
  CHECK(t.px[0] == 0xFF00FF00u && t.px[1] == 0xFF0000FFu && objects.count(1) == 1 && objects.count(2) == 1);
  CHECK(!take(r).got);   // (taken)
  // a new frame of video 1: only its rectangle is drawn again, and is the damage
  __atomic_store_n(&objects.colors[1], 0xFFFF0000u, __ATOMIC_RELEASE);
  const uint32_t one[1] = {1};
  r.request(page(a), 1, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>(one, 1));
  CHECK(r.wait_idle(5000000));
  t = take(r);
  CHECK(t.got && t.damage.x == 10 && t.damage.y == 10 && t.damage.width == 40 && t.damage.height == 30);
  CHECK(t.px[0] == 0xFFFF0000u && t.px[1] == 0xFF0000FFu && objects.count(1) == 2 && objects.count(2) == 1);
  // the whole picture when the view lost its copy, damaged or not
  t = take(r, true);
  CHECK(t.got && t.damage.width == 120 && t.damage.height == 100 && t.px[0] == 0xFFFF0000u);
  // scrolled: all of it again, moved
  r.request(page(a), 1, 50, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>());
  CHECK(r.wait_idle(5000000));
  t = take(r);
  CHECK(t.got && t.damage.height == 100 && t.px[0] == 0xFF0000FFu);   // (video 2, at 60..90, is at 10..40 now)
  // another size: all of it, in buffers of that size
  r.request(page(a), 1, 0, 100, 80, 0xFFFFFFFFu, ArrayView<const uint32_t>());
  CHECK(r.wait_idle(5000000));
  bool sized = false;
  r.take(false, [&](const graphics::Surface& front, const graphics::Rect2D& damage) { sized = front.width == 100 && front.height == 80 && damage.width == 100; });
  CHECK(sized && __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) >= 4);
  r.set_object_painter(nullptr);
  r.stop();
  CHECK(!r.running());
}

// A drawing that takes long holds up nothing on the side that shows the page: take() answers at once (with the drawing before), requests made
// meanwhile are coalesced into one drawing, and changing the object painter waits for the drawing to be done.
static void test_slow_page_blocks_nobody() {
  HeapAllocator a;
  Objects objects;
  PageRasterizer r(a);
  r.set_object_painter(&objects);
  CHECK(r.start(&ready, nullptr));
  r.request(page(a), 1, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>());
  CHECK(r.wait_idle(5000000));
  CHECK(take(r).got);
  __atomic_store_n(&objects.delay_us, uint64_t{300000}, __ATOMIC_RELEASE);   // 0.3 s per object: a page that takes 0.6 s to draw
  const uint64_t drawings = r.drawings();
  r.request(page(a), 2, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>());   // (a new picture: drawn whole, slowly)
  kernel::sleep_us(50000);   // (it has begun)
  uint64_t slowest = 0;
  for (int i = 0; i < 20; ++i) {   // the render thread's frames while it draws
    const uint64_t t0 = now_us();
    (void)take(r);
    const uint64_t spent = now_us() - t0;
    if (spent > slowest) slowest = spent;
    kernel::sleep_us(10000);
  }
  CHECK(slowest < 50000);   // (each frame took the lock for a moment, never for the drawing)
  // requests while it draws: coalesced into one more drawing
  const uint32_t one[1] = {1}, two[1] = {2};
  __atomic_store_n(&objects.delay_us, uint64_t{0}, __ATOMIC_RELEASE);
  for (int i = 0; i < 10; ++i) r.request(page(a), 2, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>(i % 2 ? one : two, 1));
  // changing the painter waits for the drawing in progress: once it returns, the old painter is never called again
  Objects other;
  r.set_object_painter(&other);
  const int before = objects.count(1) + objects.count(2);
  CHECK(r.wait_idle(10000000));
  CHECK(objects.count(1) + objects.count(2) == before);
  CHECK(r.drawings() - drawings <= 3);   // (the slow one, and the coalesced requests: one or two drawings, not ten)
  Taken t = take(r);
  CHECK(t.got);
  r.set_object_painter(nullptr);
  r.stop();
}

// The picture may be dropped by its owner while it is drawn (the browser shows the next one): the drawing thread keeps its own reference.
static void test_picture_outlives_its_owner() {
  HeapAllocator a;
  Objects objects;
  objects.delay_us = 100000;
  PageRasterizer r(a);
  r.set_object_painter(&objects);
  CHECK(r.start(&ready, nullptr));
  for (uint64_t v = 1; v <= 5; ++v) {
    SharedPointer<browser::PagePicture> p = page(a);
    r.request(p, v, 0, 120, 100, 0xFFFFFFFFu, ArrayView<const uint32_t>());
  }   // (every reference here is gone)
  CHECK(r.wait_idle(10000000));
  CHECK(take(r).got);
  r.set_object_painter(nullptr);
  r.stop();
  r.stop();   // (twice is fine)
}

int main() {
  test_drawing_and_damage();
  test_slow_page_blocks_nobody();
  test_picture_outlives_its_owner();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("page rasterizer tests passed\n");
  return 0;
}

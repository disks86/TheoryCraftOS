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

// Draws a web page's picture on a thread of its own, so that a page that takes long to draw (a big page changing, a video playing in it) never holds up
// the desktop: the thread that draws every window (gui-render, under the UI lock) only copies what this thread has finished, and only the part that
// changed. One per page view.
//
// The page is drawn with the software renderer into a back buffer; when a drawing is done the buffers are swapped (under a short lock) and the part
// drawn is added to the damage the view has not taken yet; that part is then copied into the new back buffer, so the two always agree but for the
// drawing in progress. take() hands the view the finished front buffer and its damage under the same lock, which it holds only for as long as it copies
// those pixels (to the GPU). A request coalesces with the ones not yet started: the newest picture, scroll and size win, the changed objects add up.
#pragma once
#include "Browser/ObjectTypes.hpp"
#include "Browser/PagePicture.hpp"
#include "Font/FontSet.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace gui {

// Where the Object entries of the elements `nodes` are drawn when page position (0, 0) is at (dx, dy): the union of their rectangles (an object under a
// transform or a filter counts with the bounds of the outermost one, which is where it can show), inside `bounds`. False if none of them shows there.
bool changed_objects_area(const paint::DisplayList& list, ArrayView<const uint32_t> nodes, int32_t dx, int32_t dy, const graphics::Rect2D& bounds,
                          graphics::Rect2D* area);

class PageRasterizer {
 public:
  using Rect2D = graphics::Rect2D;
  static constexpr uint32_t kMaxChangedObjects = 16;

  explicit PageRasterizer(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~PageRasterizer() { stop(); }
  PageRasterizer(const PageRasterizer&) = delete;
  PageRasterizer& operator=(const PageRasterizer&) = delete;

  // Starts the thread; false where there are no threads (then the caller draws the page itself). `ready` is called (on the drawing thread, no lock
  // held) each time a drawing is finished: the view asks for a frame.
  bool start(void (*ready)(void* context), void* context);
  void stop();
  bool running() const { return __atomic_load_n(&thread_, __ATOMIC_ACQUIRE) != nullptr; }

  // Asks for `picture` scrolled to `scroll_y`, `width` x `height` pixels on `paper`, to be drawn. `version` names the picture's content: a new one
  // (or another scroll or size) is drawn whole; with the same, only where the elements `nodes` are drawn (their pictures changed: a video's frame).
  void request(SharedPointer<browser::PagePicture> picture, uint64_t version, int32_t scroll_y, uint32_t width, uint32_t height, uint32_t paper,
               ArrayView<const uint32_t> nodes);

  // Who draws the pictures, videos and canvases: waits until a drawing in progress is done, so the old one is not used after this returns.
  void set_object_painter(browser::IObjectPainter* painter);

  // The finished picture: calls `fn(front, damage)` with the lock held when there is damage the caller has not taken (`all`: the whole picture is
  // wanted, damaged or not - the caller lost its copy), and clears it. `front` is width x height, opaque, 0xAARRGGBB. False if nothing was handed over.
  template <class Fn>
  bool take(bool all, Fn&& fn) {
    kernel::ScopedMutex guard(mutex_);
    if (front_.pixels.empty() || !front_complete_) return false;
    if (all) damage_ = {0, 0, front_.width, front_.height};
    if (!damage_.width || !damage_.height) return false;
    fn(static_cast<const graphics::Surface&>(front_), damage_);
    damage_ = {};
    return true;
  }

  // How many drawings were finished (for tests).
  uint64_t drawings() const { return __atomic_load_n(&drawings_, __ATOMIC_ACQUIRE); }
  // Waits up to `timeout_us` until no request is waiting or being drawn (for tests).
  bool wait_idle(uint64_t timeout_us);

 private:
  struct Request {
    SharedPointer<browser::PagePicture> picture;
    uint64_t version = 0;
    int32_t scroll_y = 0;
    uint32_t width = 0, height = 0, paper = 0;
    uint32_t nodes[kMaxChangedObjects] = {};
    uint32_t node_count = 0;
    bool all_nodes = false;   // more changed than fit: everything
  };

  static void thread_main(void* self);
  void run();
  bool draw(const Request& r);   // into back_; false if out of memory
  bool ensure_buffers(uint32_t width, uint32_t height);
  void release_buffers();
  bool changed_area(const Request& r, Rect2D* area) const;

  allocators::IAllocator& allocator_;
  kernel::Thread* thread_ = nullptr;
  bool stop_ = false;
  void (*ready_)(void*) = nullptr;
  void* ready_context_ = nullptr;
  kernel::Event wake_{true};

  // mutex_: the request waiting, the front buffer and its damage, and the buffers' swap. (A blocking mutex, not a spin lock: the view holds it while it
  // copies the damaged pixels to the GPU.)
  kernel::Mutex mutex_;
  Request pending_;
  bool has_pending_ = false, busy_ = false;
  graphics::Surface front_;
  bool front_complete_ = false;   // the front buffer holds a whole drawing of the current size
  Rect2D damage_;
  uint64_t drawings_ = 0;

  // paint_mutex_: held while drawing (the object painter is used then).
  kernel::Mutex paint_mutex_;
  browser::IObjectPainter* objects_ = nullptr;

  // The drawing thread's own: the back buffer, its fonts, and what the buffers hold.
  graphics::Surface back_;
  UniquePointer<font::FontSet> fonts_;
  uint64_t drawn_version_ = 0;
  int32_t drawn_scroll_ = 0;
  bool drawn_any_ = false;
};

}  // namespace gui

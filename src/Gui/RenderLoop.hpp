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

// The render thread of a Window: sleeps until something marks the window dirty, draws it (the tree is
// locked only while it is drawn), then pauses a few milliseconds so a burst of changes becomes one frame.
// The UI therefore keeps repainting however long the input thread or a handler is busy.
#pragma once
#include "Gui/Window.hpp"

namespace gui {

class RenderLoop {
  public:
    explicit RenderLoop(Window& window) : window_(window) {}
    ~RenderLoop() { stop(); }
    RenderLoop(const RenderLoop&) = delete;
    RenderLoop& operator=(const RenderLoop&) = delete;

    // Starts the thread; false if threads are unavailable (kernel not started) or it already runs.
    bool start();
    // Ends the thread and waits for it. After this the caller may use the window's renderer again.
    void stop();
    bool running() const { return __atomic_load_n(&thread_, __ATOMIC_ACQUIRE) != nullptr; }
    // Frames drawn so far.
    uint32_t frames() const { return __atomic_load_n(&frames_, __ATOMIC_ACQUIRE); }
    // Last render time in microseconds (0 if not yet rendered).
    uint64_t last_render_time_us() const { return __atomic_load_n(&last_render_time_us_, __ATOMIC_ACQUIRE); }

    static constexpr uint64_t kFramePauseUs = 8000;

  private:
    static void thread_main(void* loop);
    void run();

    Window& window_;
    kernel::Thread* thread_ = nullptr;
    bool stop_ = false;
    uint32_t frames_ = 0;
    // Tracks last render time for watchdog purposes; accessed atomically.
    uint64_t last_render_time_us_ = 0;
};

}  // namespace gui

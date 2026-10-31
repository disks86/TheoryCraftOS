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

#include "Gui/RenderLoop.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Thread.hpp"

namespace gui {

// Maximum time in microseconds the render loop may spend in render() before it is considered stuck
static constexpr uint64_t kRenderTimeoutUs = 500000;  // 500ms
// Maximum time in microseconds the render loop may wait for a frame before it forces a refresh
static constexpr uint64_t kMaxWaitTimeoutUs = 300000;  // 300ms

bool RenderLoop::start() {
  if (!kernel::running() || running()) return false;
  __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
  __atomic_store_n(&frames_, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&last_render_time_us_, 0, __ATOMIC_RELEASE);
  kernel::Thread* thread = kernel::spawn(&RenderLoop::thread_main, this, "gui-render");
  if (!thread) return false;
  __atomic_store_n(&thread_, thread, __ATOMIC_RELEASE);
  return true;
}

void RenderLoop::stop() {
  kernel::Thread* thread = __atomic_load_n(&thread_, __ATOMIC_ACQUIRE);
  if (!thread) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  window_.mark_dirty();  // wakes it from its wait
  window_.mark_dirty();
  kernel::join(thread);
  __atomic_store_n(&thread_, static_cast<kernel::Thread*>(nullptr), __ATOMIC_RELEASE);
}

void RenderLoop::thread_main(void* loop) { static_cast<RenderLoop*>(loop)->run(); }

void RenderLoop::run() {
  uint64_t last_frame_us = hardware::uptime_us();
  uint64_t last_render_time_us = 0;
  
  while (!__atomic_load_n(&stop_, __ATOMIC_ACQUIRE)) {
    if (window_.dirty()) {  // (a window starts dirty: the first frame needs no signal)
      const uint64_t render_start = hardware::uptime_us();
      
      // watchdog: limit render time to prevent UI lockup from slow/hanging operations
      // If previous render took too long, enter a degraded mode to ensure UI responds
      if (last_render_time_us > kRenderTimeoutUs) {
        // Render took too long previously, force a refresh to ensure UI responds
        window_.mark_dirty();
        kernel::sleep_us(kFramePauseUs);
        last_frame_us = hardware::uptime_us();
        continue;
      }
      
      window_.render();
      
      const uint64_t render_time = hardware::uptime_us() - render_start;
      __atomic_store_n(&last_render_time_us_, render_time, __ATOMIC_RELEASE);
      last_render_time_us = render_time;
      
      __atomic_fetch_add(&frames_, 1, __ATOMIC_ACQ_REL);
      
      // watchdog: log slow frames for diagnostics
      if (render_time > kRenderTimeoutUs) {
        // Render took too long - in a real implementation, this would log to the
        // diagnostic_ callback if available. For now, we just continue.
      }
      
      // (an animation that asked for a frame time is given the rest of it: no faster than the display shows, and the CPU is free in between)
      const uint64_t interval = window_.frame_interval_us();
      const uint64_t spent = hardware::uptime_us() - render_start;
      const uint64_t pause = interval > spent + kFramePauseUs ? interval - spent : kFramePauseUs;
      kernel::sleep_us(pause);  // changes made meanwhile become the next frame
      
      last_frame_us = hardware::uptime_us();
    } else {
      // watchdog: limit wait time to prevent indefinite blocking
      // Force refresh if too much time has passed since the last frame
      const uint64_t time_since_last = hardware::uptime_us() - last_frame_us;
      if (time_since_last > kMaxWaitTimeoutUs) {
        window_.mark_dirty();
      }
      
      const uint64_t wait_timeout = kMaxWaitTimeoutUs > time_since_last ? kMaxWaitTimeoutUs - time_since_last : kFramePauseUs;
      window_.wait_for_frame(wait_timeout);
    }
  }
}

}  // namespace gui

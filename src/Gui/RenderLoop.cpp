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

namespace gui {

bool RenderLoop::start() {
  if (!kernel::running() || running()) return false;
  __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
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
  while (!__atomic_load_n(&stop_, __ATOMIC_ACQUIRE)) {
    if (window_.dirty()) {  // (a window starts dirty: the first frame needs no signal)
      window_.render();
      __atomic_fetch_add(&frames_, 1, __ATOMIC_ACQ_REL);
      kernel::sleep_us(kFramePauseUs);  // changes made meanwhile become the next frame
    } else {
      window_.wait_for_frame(100000);  // also wakes on a timer, so a missed signal cannot stall us for long
    }
  }
}

}  // namespace gui

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

#include "Kernel/Firmware.hpp"
#include "Kernel/Sync.hpp"

namespace kernel {

namespace {
struct Service {
  Mutex firmware_lock;    // one firmware call at a time
  Mutex request_lock;     // one hand-off at a time
  Semaphore work{0};
  Event done{true};
  void (*function)(void*) = nullptr;
  void* context = nullptr;
  uint32_t boot_core = 0;
  bool started = false;
};
Service g;

void service_main(void*) {
  for (;;) {
    g.work.acquire();
    g.firmware_lock.lock();
    g.function(g.context);
    g.firmware_lock.unlock();
    g.done.set();
  }
}
}  // namespace

bool may_call_firmware_directly() {
  if (!running() || !g.started) return true;
  const Thread* t = current_thread();
  return t && t->affinity == static_cast<int32_t>(g.boot_core);
}

bool firmware_service_start() {
  g.boot_core = current_core();
  Thread* t = spawn(service_main, nullptr, "firmware", static_cast<int32_t>(g.boot_core), -1, 32 * 1024);
  if (!t) return false;
  detach(t);
  g.started = true;
  return true;
}

void firmware_call(void (*function)(void*), void* context) {
  if (!running() || !g.started) {
    function(context);
    return;
  }
  if (may_call_firmware_directly()) {
    g.firmware_lock.lock();
    function(context);
    g.firmware_lock.unlock();
    return;
  }
  g.request_lock.lock();
  g.function = function;
  g.context = context;
  g.work.release();
  g.done.wait();
  g.request_lock.unlock();
}

}  // namespace kernel

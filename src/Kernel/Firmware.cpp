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
// One hand-off to the service thread. It lives on the caller's stack, and the caller does not leave until the service says the call has run: the lambda
// the call executes (and everything it captured by reference) is on that stack too.
struct Request {
  void (*function)(void*);
  void* context;
  uint32_t finished;   // set last by the service, read by the caller (atomically)
};

struct Service {
  Mutex firmware_lock;    // one firmware call at a time
  Mutex request_lock;     // one hand-off at a time
  Semaphore work{0};
  Event done{true};       // a hint that some request finished; never proof of it (see firmware_call)
  Request* request = nullptr;
  uint32_t boot_core = 0;
  bool started = false;
};
Service g;

void service_main(void*) {
  for (;;) {
    g.work.acquire();
    Request* r = g.request;
    if (!r) continue;   // (woken with nothing to do: never run an old request again)
    g.request = nullptr;
    g.firmware_lock.lock();
    r->function(r->context);
    g.firmware_lock.unlock();
    __atomic_store_n(&r->finished, 1u, __ATOMIC_RELEASE);   // the caller may leave now: r is not touched again
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
  Request request{function, context, 0};
  g.request = &request;
  g.work.release();
  // The event can be set when no request finished (an earlier completion whose waiter had already gone on leaves it set), so waking up proves nothing:
  // wait until THIS request says it ran. Leaving earlier would let the service run the call later against a stack frame that is gone, and the firmware
  // would write its answers (an EFI status such as NOT_READY) over whatever lives there now - a return address, say.
  while (!__atomic_load_n(&request.finished, __ATOMIC_ACQUIRE)) g.done.wait(1000);
  g.request_lock.unlock();
}

}  // namespace kernel

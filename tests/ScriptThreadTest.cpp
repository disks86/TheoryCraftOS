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

// The script engine on real threads (HostThreads): a script runs on a thread with a big stack, and a script that another script starts while it runs (a page
// registering a service worker evaluates the worker's script) runs on that same thread. The outer one may hold recursive locks that the inner one takes
// again - the service worker hub's: on another thread the inner script would wait for the outer one, which waits for it (YouTube hung the browser and
// then the shutdown this way).
#include <stdio.h>
#include <stdlib.h>
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Script/Engine.hpp"

using namespace script;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

struct TestHost final : IHost {
  void console(int, StringView) override {}
  void alert(StringView) override {}
  void document_write(StringView, bool) override {}
  uint64_t now_ms() override { return 1700000000000ull; }
  StringView location() override { return "http://example.com/page"; }
  StringView title() override { return "T"; }
  void set_title(StringView) override {}
  StringView user_agent() override { return "Test/1"; }
};

namespace {
kernel::RecursiveMutex g_hub;   // stands for the service worker hub's lock
Engine* g_inner = nullptr;
kernel::Thread* g_outer_thread = nullptr;
kernel::Thread* g_inner_thread = nullptr;
bool g_inner_locked = false, g_inner_ran = false;

// The inner script's native: takes the lock again (try_lock: on another thread it would wait forever).
bool inner_native(Vm&, const Value&, const Value*, uint32_t, Value* result) {
  g_inner_thread = kernel::current_thread();
  g_inner_locked = g_hub.try_lock();
  if (g_inner_locked) g_hub.unlock();
  *result = Value();
  return true;
}

// The outer script's native: holds the lock and runs the inner engine's script.
bool outer_native(Vm&, const Value&, const Value*, uint32_t, Value* result) {
  g_outer_thread = kernel::current_thread();
  g_hub.lock();
  String error(g_inner->vm().allocator());
  Value value;
  g_inner_ran = g_inner->evaluate("inner(); 1", &error, &value);
  g_hub.unlock();
  *result = Value();
  return true;
}
}  // namespace

int main() {
  TestAllocator a;
  TestHost host;
  Engine outer(a, &host), inner(a, &host);
  g_inner = &inner;
  outer.vm().define_native(outer.vm().global(), "outer", outer_native, 0);
  inner.vm().define_native(inner.vm().global(), "inner", inner_native, 0);
  String error(a);
  CHECK(outer.run("outer(); 'done'", &error));
  CHECK(g_inner_ran);
  CHECK(g_outer_thread != nullptr && g_outer_thread == g_inner_thread);   // the nested script stayed on the thread of the one that started it
  CHECK(g_inner_locked);                                                  // and so got the lock its caller holds
  CHECK(g_outer_thread != kernel::current_thread());                      // (the outer script itself moved to a thread with a big stack)
  if (g_failed == 0) printf("script thread tests passed\n");
  return g_failed == 0 ? 0 : 1;
}

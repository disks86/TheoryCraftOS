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
#include "Win32kTestSupport.hpp"

using namespace w32test;
using namespace process;
using namespace process::win32k;

static void test_call_helpers() {
  System sys;
  Program prog(11);
  SyscallRequest r;
  r.number = kServiceBase;
  r.args[0] = 1; r.args[1] = 2; r.args[2] = 3; r.args[3] = 4;
  r.user_stack = kStack;
  prog.memory.put64(kStack + 0x28, 5);
  prog.memory.put64(kStack + 0x30, 6);
  Call c(prog.process, r);
  CHECK(c.arg(0) == 1 && c.arg(3) == 4 && c.arg(4) == 5 && c.arg(5) == 6 && !c.bad_stack());
  r.user_stack = 0x1000;      // not mapped
  Call bad(prog.process, r);
  CHECK(bad.arg(4) == 0 && bad.bad_stack());

  // memory helpers
  CHECK(c.write_u32(kScratch, 0xAABBCCDDu) && prog.memory.get32(kScratch) == 0xAABBCCDDu);
  uint32_t v = 0;
  CHECK(c.read_u32(kScratch, &v) && v == 0xAABBCCDDu);
  CHECK(!c.read_u32(0, &v) && !c.read_u32(0x10, &v) && !c.write_u32(kBase + kSize - 2, 1));
  struct Rect { int32_t l, t, r, b; } rect = {1, 2, 3, 4}, back = {};
  CHECK(c.write_struct(kScratch + 0x10, rect) && c.read_struct(kScratch + 0x10, &back) && back.l == 1 && back.b == 4);

  // text
  prog.memory.put_wide(kScratch + 0x100, "Hello");
  WideString s(*c.allocator());
  CHECK(c.read_wide_z(kScratch + 0x100, 64, &s) && s.size() == 5 && s[1] == u'e');
  WideString counted(*c.allocator());
  CHECK(c.read_wide(kScratch + 0x100, 3, &counted) && counted.size() == 3);
  prog.memory.put_unicode_string(kScratch + 0x200, kScratch + 0x300, "Window");
  WideString us(*c.allocator());
  CHECK(c.read_unicode_string(kScratch + 0x200, &us) && us.equals(WideString(*c.allocator()).view()) == false && us.size() == 6);
  WideString too_long(*c.allocator());
  CHECK(!c.read_wide_z(kScratch + 0x100, 3, &too_long));      // longer than the limit is an error
  const char16_t out[] = u"abc";
  CHECK(c.write_wide(kScratch + 0x400, ArrayView<const char16_t>(out, 3), 8) == 3 && prog.memory.get_wide_z(kScratch + 0x400) == "abc");
  CHECK(c.write_wide(kScratch + 0x400, ArrayView<const char16_t>(out, 3), 2) == 2);      // cut to the capacity, no room for the NUL
  CHECK(c.write_wide(0x10, ArrayView<const char16_t>(out, 3), 8) == -1);

  // last error goes to the TEB (gs base) when the process has no thread table entry
  prog.process.set_gs_base(kScratch + 0x1000);
  c.set_last_error(1400);
  CHECK(prog.memory.get32(kScratch + 0x1000 + 0x68) == 1400);
  CHECK(c.fail(87, 7) == 7 && prog.memory.get32(kScratch + 0x1000 + 0x68) == 87);
}

static void test_table() {
  System sys;
  CHECK(duplicate_registrations() == 0);
  // a number nobody implements answers STATUS_NOT_IMPLEMENTED; one outside the win32k range is none of win32k's business
  Program prog(12);
  uint32_t free_number = 0;
  for (uint32_t n = kServiceBase; n < kServiceEnd && !free_number; ++n) if (!handler_of(n)) free_number = n;
  if (free_number) CHECK(prog.call32(static_cast<WindowsSyscallNumber>(free_number)) == 0xC0000002u);
  uint64_t result = 0;
  CHECK(!dispatch(prog.process, SyscallRequest(), &result));
  CHECK(prog.call32(static_cast<WindowsSyscallNumber>(kServiceBase - 1)) == 0xC000001Cu && prog.call32(static_cast<WindowsSyscallNumber>(kServiceEnd)) == 0xC000001Cu);
  CHECK(!register_handler(static_cast<WindowsSyscallNumber>(kServiceBase - 1), [](Call&) -> uint64_t { return 0; }));
  CHECK(!register_handler(static_cast<WindowsSyscallNumber>(kServiceBase), nullptr));
}

static uint64_t g_probe_calls = 0;
static void test_registration_and_wait() {
  System sys;
  // Registering a number that has no handler (the first free one) works once and a second time is a counted duplicate.
  uint32_t free_number = 0;
  for (uint32_t n = kServiceEnd - 1; n >= kServiceBase && !free_number; --n) if (!handler_of(n)) free_number = n;
  if (!free_number) return;
  const uint32_t before = duplicate_registrations();
  auto handler = [](Call& c) -> uint64_t { ++g_probe_calls; return c.arg(0) + c.arg(5); };
  CHECK(register_handler(static_cast<WindowsSyscallNumber>(free_number), handler));
  CHECK(!register_handler(static_cast<WindowsSyscallNumber>(free_number), handler) && duplicate_registrations() == before + 1);
  Program prog(13);
  CHECK(prog.call(static_cast<WindowsSyscallNumber>(free_number), 40, 0, 0, 0, 0, 2) == 42 && g_probe_calls == 1);

  // wait_until: gives up at the timeout, and returns at once when the condition holds
  struct Probe { Program* p; bool ready; } probe{&prog, false};
  SyscallRequest r;
  Call call(prog.process, r);
  ObjectManager* objects = system_objects();
  objects->sync().lock();
  CHECK(!wait_until(call, 2000000, [&] { return probe.ready; }));
  probe.ready = true;
  CHECK(wait_until(call, 2000000, [&] { return probe.ready; }));
  objects->sync().unlock();
}

int main() {
  test_call_helpers();
  test_table();
  test_registration_and_wait();
  return finish("Win32kCoreTest");
}

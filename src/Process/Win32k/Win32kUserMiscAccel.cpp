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
#include "Process/Win32k/Win32kUserMisc.hpp"

// Accelerator tables (CreateAcceleratorTable, CopyAcceleratorTable, DestroyAcceleratorTable). TranslateAccelerator (a message service of the window manager) reads the
// entries through accelerator_entries().
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kMaxEntries = 0xFFFF, kAccelSize = 6;      // ACCEL { BYTE fVirt; WORD key; WORD cmd; } with the padding after fVirt

struct AccelTable {
  uint64_t pid;
  List<AcceleratorEntry> entries;
  AccelTable(allocators::IAllocator& a, uint64_t owner) : pid(owner), entries(a) {}
};
struct AccelState {
  Registry<AccelTable> tables;
  explicit AccelState(allocators::IAllocator& a) : tables(a, 0x0003) {}
};
UniquePointer<AccelState> g_accel;
AccelState* st() { return lazy_state(g_accel); }

uint64_t svc_create(Call& c) {
  AccelState* s = st();
  const int32_t count = c.argi(1);
  if (!s) return 0;
  if (count < 1 || uint32_t(count) > kMaxEntries) return c.fail(kErrorInvalidParameter);
  uint32_t handle = 0;
  AccelTable* table = s->tables.create(&handle, c.pid());
  if (!table) return c.fail(kErrorNoMoreUserHandles);
  for (int32_t i = 0; i < count; ++i) {
    uint8_t raw[kAccelSize];
    if (!c.read(c.arg(0) + uint64_t(i) * kAccelSize, ByteView(raw, kAccelSize))) {
      s->tables.destroy(handle);
      return c.fail(kErrorNoAccess);
    }
    AcceleratorEntry e;
    e.virt = raw[0];
    e.key = uint16_t(raw[2] | raw[3] << 8);
    e.command = uint16_t(raw[4] | raw[5] << 8);
    if (!table->entries.push_back(e)) { s->tables.destroy(handle); return c.fail(kErrorNotEnoughMemory); }
  }
  return handle;
}

uint64_t svc_copy(Call& c) {
  AccelState* s = st();
  AccelTable* table = s && (c.arg(0) >> 32) == 0 ? s->tables.find(c.arg32(0)) : nullptr;
  if (!table) return c.fail(kErrorInvalidAccelHandle);
  if (c.arg(1) == 0) return table->entries.count();
  const int32_t count = c.argi(2);
  if (count <= 0) return 0;
  const uint64_t n = table->entries.count() < uint32_t(count) ? table->entries.count() : uint32_t(count);
  for (uint64_t i = 0; i < n; ++i) {
    const AcceleratorEntry& e = table->entries[i];
    const uint8_t raw[kAccelSize] = {e.virt, 0, uint8_t(e.key), uint8_t(e.key >> 8), uint8_t(e.command), uint8_t(e.command >> 8)};
    if (!c.write(c.arg(1) + i * kAccelSize, ConstByteView(raw, kAccelSize))) return c.fail(kErrorNoAccess);
  }
  return n;
}

uint64_t svc_destroy(Call& c) {
  AccelState* s = st();
  if (!s || (c.arg(0) >> 32) != 0 || !s->tables.destroy(c.arg32(0))) return c.fail(kErrorInvalidAccelHandle);
  return 1;
}

}  // namespace

bool accelerator_entries(uint32_t table, List<AcceleratorEntry>* out) {
  AccelState* s = g_accel.get();
  AccelTable* t = s ? s->tables.find(table) : nullptr;
  if (!t) return false;
  for (const AcceleratorEntry& e : t->entries.view()) out->push_back(e);
  return true;
}

void accelerators_process_exited(uint64_t pid) {
  if (!g_accel) return;
  List<uint32_t> all(*system_allocator());
  g_accel->tables.handles(&all);
  for (uint32_t h : all.view()) if (AccelTable* t = g_accel->tables.find(h)) if (t->pid == pid) g_accel->tables.destroy(h);
}

void install_accelerators() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserCreateAcceleratorTable, svc_create);
  register_handler(N::NtUserCopyAcceleratorTable, svc_copy);
  register_handler(N::NtUserDestroyAcceleratorTable, svc_destroy);
}

}  // namespace process::win32k::user_misc

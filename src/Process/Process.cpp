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
#include "Process/Process.hpp"

namespace process {

namespace {
constexpr uint64_t kChunk = 1024;
constexpr uint64_t kUnknownLinux = ~uint64_t{37};      // -ENOSYS
constexpr uint64_t kUnknownWindows = 0xC000001Cu;      // STATUS_INVALID_SYSTEM_SERVICE
}  // namespace

Process::Process(uint64_t id, const ProcessBinding& binding, IUserMemory& memory, IProcessHost& host)
    : id_(id), binding_(binding), memory_(memory), host_(host) {}

uint64_t Process::syscall(const SyscallRequest& request) {
  if (!binding_.interface || exited_) return binding_.abi == SyscallAbi::Windows ? kUnknownWindows : kUnknownLinux;
  return binding_.interface->dispatch(*this, request);
}

bool Process::read_user_u64(uint64_t address, uint64_t* out) {
  uint8_t raw[8];
  if (!read_user(address, ByteView(raw, 8))) return false;
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) value |= uint64_t{raw[i]} << (8 * i);
  *out = value;
  return true;
}

bool Process::write_user_u64(uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(value >> (8 * i));
  return write_user(address, ConstByteView(raw, 8));
}

bool Process::close_descriptor(uint64_t slot) {
  if (slot >= kMaxDescriptors || slots_[slot] == DescriptorKind::Closed) return false;
  slots_[slot] = DescriptorKind::Closed;
  return true;
}

uint64_t copy_user_to_console(Process& process, uint32_t stream, uint64_t address, uint64_t count, bool* faulted) {
  *faulted = false;
  uint8_t buffer[kChunk];
  uint64_t done = 0;
  while (done < count) {
    const uint64_t n = count - done < kChunk ? count - done : kChunk;
    if (address > ~uint64_t{0} - done - n + 1) { *faulted = true; break; }      // the range would wrap past the end of the address space
    uint64_t ok = n;
    if (!process.read_user(address + done, ByteView(buffer, n))) {
      // The piece runs into unmapped memory: send the bytes before the hole, as a real kernel does, and report the fault.
      *faulted = true;
      ok = 0;
      while (ok < n && process.read_user(address + done + ok, ByteView(buffer + ok, 1))) ++ok;
    }
    if (ok && !process.host().write_console(stream, ConstByteView(buffer, ok))) { *faulted = false; break; }
    done += ok;
    if (*faulted) break;
  }
  return done;
}

uint64_t copy_console_to_user(Process& process, uint64_t address, uint64_t count, bool* faulted) {
  *faulted = false;
  uint8_t buffer[kChunk];
  const uint64_t n = count < kChunk ? count : kChunk;      // one read: a terminal read returns what is there, not what was asked for
  if (n == 0) return 0;
  uint64_t got = 0;
  if (!process.host().read_console(ByteView(buffer, n), &got) || got > n) return 0;
  if (!process.write_user(address, ConstByteView(buffer, got))) { *faulted = true; return 0; }
  return got;
}

}  // namespace process

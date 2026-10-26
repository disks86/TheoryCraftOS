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
#include "Program/ProgramHost.hpp"
#include "FileSystem/FileSystemTypes.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Thread.hpp"

namespace desktop {

ProgramHost::ProgramHost(allocators::IAllocator& allocator)
    : allocator_(allocator), buffer_(allocator), random_(entropy_), unhandled_(allocator), input_(allocator) {
  start_real_ns_ = filesystem::unix_time_now() * 1000000000ull;
  start_tsc_ = hardware::read_tsc();
  random_.initialize();      // (stays not ready on a processor without RDSEED and RDRAND)
}

bool ProgramHost::take_output(String* out) {
  kernel::ScopedMutex lock(mutex_);
  if (buffer_.empty()) return false;
  out->append(buffer_.view());
  buffer_.clear();
  return true;
}

uint64_t ProgramHost::dropped() const {
  kernel::ScopedMutex lock(mutex_);
  return dropped_;
}

bool ProgramHost::write_console(uint32_t, ConstByteView data) {
  kernel::ScopedMutex lock(mutex_);
  const uint64_t room = buffer_.size() < kMaxBuffered ? kMaxBuffered - buffer_.size() : 0;
  const uint64_t keep = data.size() < room ? data.size() : room;
  dropped_ += data.size() - keep;
  return keep == 0 || buffer_.append(StringView(data.first(keep)));
}

bool ProgramHost::read_console(ByteView out, uint64_t* got) {
  *got = 0;
  if (out.empty()) return true;
  kernel::ScopedMutex lock(mutex_);
  // Wait for a line (or the end): a terminal hands out one line per read.
  while (input_.empty() && !input_closed_ && !interrupted()) input_ready_.wait(mutex_, 20000);
  if (interrupted()) return false;
  if (input_.empty()) return true;      // closed: end of file
  uint64_t n = 0;
  const StringView text = input_.view();
  while (n < text.size() && n < out.size()) {
    out[n] = static_cast<uint8_t>(text[n]);
    ++n;
    if (text[n - 1] == '\n') break;
  }
  input_.erase(0, n);
  *got = n;
  return true;
}

uint64_t ProgramHost::console_input_available(bool* at_end) {
  kernel::ScopedMutex lock(mutex_);
  *at_end = input_.empty() && (input_closed_ || interrupted());
  return input_.size();
}

void ProgramHost::push_input(StringView text) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (input_closed_ || input_.size() + text.size() > kMaxInput) return;
    input_.append(text);
  }
  input_ready_.notify_all();
}

void ProgramHost::close_input() {
  {
    kernel::ScopedMutex lock(mutex_);
    input_closed_ = true;
  }
  input_ready_.notify_all();
}

void ProgramHost::set_interrupt_callback(void (*callback)(void*), void* context) {
  kernel::ScopedMutex lock(mutex_);
  interrupt_callback_ = callback;
  interrupt_context_ = context;
}

void ProgramHost::interrupt() {
  {
    kernel::ScopedMutex lock(mutex_);
    if (interrupt_callback_) interrupt_callback_(interrupt_context_);
  }
  __atomic_store_n(&interrupted_, true, __ATOMIC_RELEASE);
  input_ready_.notify_all();
}

uint64_t ProgramHost::monotonic_ns() {
  const uint64_t per_us = hardware::tsc_ticks_per_us();
  return per_us ? (hardware::read_tsc() - start_tsc_) / per_us * 1000 : 0;
}

uint64_t ProgramHost::realtime_ns() { return start_real_ns_ + monotonic_ns(); }

void ProgramHost::sleep_ns(uint64_t ns) {
  // In slices, so that Ctrl+C is noticed.
  for (uint64_t left = ns / 1000; left > 0 && !interrupted();) {
    const uint64_t slice = left < 10000 ? left : 10000;
    kernel::sleep_us(slice);
    left -= slice;
  }
}
void ProgramHost::yield() { kernel::yield(); }

bool ProgramHost::random_bytes(ByteView out) {
  kernel::ScopedMutex lock(mutex_);
  return random_.ready() && random_.fill(out);
}

void ProgramHost::unhandled_syscall(process::SyscallAbi abi, uint64_t number) {
  kernel::ScopedMutex lock(mutex_);
  if (unhandled_count_ >= 8) return;
  ++unhandled_count_;
  if (!unhandled_.empty()) unhandled_.append(", ");
  unhandled_.append(abi == process::SyscallAbi::Windows ? "NT service " : "Linux system call ");
  unhandled_.append_uint(number);
}

bool ProgramHost::take_unhandled(String* out) {
  kernel::ScopedMutex lock(mutex_);
  if (unhandled_.empty()) return false;
  out->append(unhandled_.view());
  unhandled_.clear();
  return true;
}

}  // namespace desktop

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
#pragma once
#include <stdint.h>
#include "Crypto/Random.hpp"
#include "Kernel/Sync.hpp"
#include "Process/Process.hpp"
#include "Utility/String.hpp"

namespace desktop {

// What a program started from the shell gets from the rest of the system. A program runs on a thread of its own, so its console output cannot go straight
// to a window: write_console() keeps it in a buffer, and the thread that waits for the program takes it out with take_output() and shows it. A program has
// no keyboard input yet (reading the console gives end of file). Time comes from the wall clock and the processor's time stamp counter; random bytes
// from a seeded generator (RDSEED/RDRAND), so getrandom() works only on processors that have them.
class ProgramHost final : public process::IProcessHost {
 public:
  explicit ProgramHost(allocators::IAllocator& allocator);

  // Moves what the program printed so far to the end of `out` (it may be partial UTF-8). True if there was anything.
  bool take_output(String* out);
  // Output stops being kept once this much has piled up unread (a program printing in a tight loop cannot use up memory); the rest is dropped.
  static constexpr uint64_t kMaxBuffered = 1 << 20;
  uint64_t dropped() const;

  bool write_console(uint32_t stream, ConstByteView data) override;
  bool read_console(ByteView out, uint64_t* got) override;
  uint64_t realtime_ns() override;
  uint64_t monotonic_ns() override;
  void sleep_ns(uint64_t ns) override;
  void yield() override;
  bool random_bytes(ByteView out) override;
  void unhandled_syscall(process::SyscallAbi abi, uint64_t number) override;

  // Calls the program made that this OS does not have, as text for the user ("Linux system call 202"). At most a few are kept.
  bool take_unhandled(String* out);

 private:
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  String buffer_;
  uint64_t dropped_ = 0;
  uint64_t start_real_ns_ = 0, start_tsc_ = 0;
  crypto::CpuEntropySource entropy_;
  crypto::DrbgRandom random_;
  String unhandled_;
  uint32_t unhandled_count_ = 0;
};

}  // namespace desktop

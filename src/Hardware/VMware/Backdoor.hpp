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

namespace hardware {

// VMware's "backdoor": an I/O port (0x5658) that only a VMware-compatible hypervisor answers (VMware itself, QEMU with `-machine vmport=on`).
// A request puts a magic number in EAX, the command in ECX and the arguments in the other registers, and reads the port with `in`; the
// hypervisor answers in the same registers. On real hardware the port reads as all ones, so a probe is safe - but it is only made when CPUID
// says a hypervisor is present.
struct BackdoorRegisters {
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, esi = 0, edi = 0;
};

class Backdoor {
 public:
  static constexpr uint32_t kMagic = 0x564D5868;   // "VMXh"
  static constexpr uint16_t kPort = 0x5658;
  static constexpr uint32_t kCommandGetVersion = 10, kCommandMessage = 30, kCommandAbsPointerData = 39, kCommandAbsPointerStatus = 40,
                            kCommandAbsPointerCommand = 41, kCommandGetHz = 45;

  static void call(BackdoorRegisters& r);
  // True if a VMware-compatible hypervisor answers; then version() and product() are set.
  static bool probe();
  static bool present() { return present_; }
  static uint32_t version() { return version_; }
  // 1 Express, 2 ESX, 3 GSX, 4 Workstation (as the host reports it); other hosts say something else.
  static uint32_t product() { return product_; }
  // The host's idea of the processor's frequency in Hz (0 if it does not say).
  static uint64_t cpu_hz();

 private:
  static inline bool present_ = false;
  static inline uint32_t version_ = 0, product_ = 0;
};

// A message channel to the host (the "RPCI" request/reply channel and the "TCLO" channel on which the host sends commands such as halt).
// Messages are text. Used from one thread at a time.
class BackdoorChannel {
 public:
  static constexpr uint32_t kProtocolRpci = 0x49435052;   // "RPCI"
  static constexpr uint32_t kProtocolTclo = 0x4F4C4354;   // "TCLO"

  ~BackdoorChannel() { close(); }
  bool open(uint32_t protocol);
  void close();
  bool is_open() const { return open_; }

  bool send(const char* data, uint32_t length);
  // Takes the reply or pending command: its length, or -1 if the exchange failed (0: nothing waiting).
  int32_t receive(char* buffer, uint32_t capacity);

 private:
  bool step(uint32_t type, uint32_t argument, BackdoorRegisters* out) const;
  uint32_t channel_ = 0;
  bool open_ = false;
};

}  // namespace hardware

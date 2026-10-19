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
#include "Hardware/VMware/Backdoor.hpp"

namespace kernel {
struct Thread;
}

namespace hardware {

// The part of VMware Tools a guest needs to be managed from the host: the host's commands arrive on the "TCLO" channel (power off, reboot,
// ping, "the window is now this size") and the guest reports what it can do over "RPCI" (version, which capabilities it has). A thread
// polls the channel. Written from the open-vm-tools protocol; QEMU's vmport has no message channel, so this has only been exercised up to
// the point where the host says no.
class VmwareTools {
 public:
  VmwareTools() = default;
  ~VmwareTools() { stop(); }
  VmwareTools(const VmwareTools&) = delete;
  VmwareTools& operator=(const VmwareTools&) = delete;

  // Opens the channels and announces the guest. False if the host has no message channel.
  bool start();
  void stop();

  // The host asked for the guest to be powered off or restarted (true once per request).
  bool take_shutdown_request(bool* restart);

  // The host window's size, once per change.
  bool take_resolution(uint32_t* width, uint32_t* height);

 private:
  static void service_main(void* self);
  void run();
  bool request(const char* command, char* reply, uint32_t capacity);   // one RPCI exchange
  void announce();
  uint32_t answer(const char* command, uint32_t length, char* reply, uint32_t capacity);   // the TCLO reply to a host command

  BackdoorChannel tclo_;
  kernel::Thread* thread_ = nullptr;
  bool stop_ = false;
  bool shutdown_pending_ = false, restart_ = false;
  bool resolution_pending_ = false;
  uint32_t width_ = 0, height_ = 0;
};

}  // namespace hardware

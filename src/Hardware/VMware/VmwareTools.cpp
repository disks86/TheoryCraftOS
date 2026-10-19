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

#include "Hardware/VMware/VmwareTools.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

namespace {
bool starts_with(const char* text, uint32_t length, const char* prefix) {
  uint32_t i = 0;
  for (; prefix[i]; ++i)
    if (i >= length || text[i] != prefix[i]) return false;
  return true;
}

uint32_t copy(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  while (text[n] && n + 1 < capacity) { out[n] = text[n]; ++n; }
  return n;
}

bool parse_number(const char*& p, const char* end, uint32_t* out) {
  while (p < end && *p == ' ') ++p;
  if (p >= end || *p < '0' || *p > '9') return false;
  uint32_t v = 0;
  while (p < end && *p >= '0' && *p <= '9') v = v * 10 + static_cast<uint32_t>(*p++ - '0');
  *out = v;
  return true;
}
}  // namespace

// One request/reply exchange on a channel of its own. The reply starts with "1 " for success, "0 " for failure.
bool VmwareTools::request(const char* command, char* reply, uint32_t capacity) {
  BackdoorChannel rpci;
  if (!rpci.open(BackdoorChannel::kProtocolRpci)) return false;
  uint32_t length = 0;
  while (command[length]) ++length;
  bool ok = rpci.send(command, length);
  int32_t n = ok ? rpci.receive(reply, capacity) : -1;
  rpci.close();
  return n >= 2 && reply[0] == '1';
}

void VmwareTools::announce() {
  char reply[64];
  request("tools.set.version 2147483647", reply, sizeof reply);   // "version unmanaged": no tools installer should be offered
  request("tools.capability.resolution_set 1", reply, sizeof reply);
  request("tools.capability.statechange", reply, sizeof reply);
}

// The host's commands and what to say back.
uint32_t VmwareTools::answer(const char* command, uint32_t length, char* reply, uint32_t capacity) {
  if (starts_with(command, length, "reset")) return copy(reply, capacity, "OK ATR toolbox");
  if (starts_with(command, length, "ping") || starts_with(command, length, "Set_Option") || starts_with(command, length, "OS_PowerOn") ||
      starts_with(command, length, "OS_Resume") || starts_with(command, length, "OS_Suspend") || starts_with(command, length, "Capabilities_Register"))
    return copy(reply, capacity, "OK ");
  if (starts_with(command, length, "OS_Halt") || starts_with(command, length, "OS_Reboot")) {
    restart_ = starts_with(command, length, "OS_Reboot");
    __atomic_store_n(&shutdown_pending_, true, __ATOMIC_RELEASE);
    return copy(reply, capacity, "OK ");
  }
  if (starts_with(command, length, "Resolution_Set")) {
    const char* p = command + 14;
    const char* end = command + length;
    uint32_t w, h;
    if (parse_number(p, end, &w) && parse_number(p, end, &h) && w >= 320 && h >= 200 && w <= 16384 && h <= 16384) {
      width_ = w;
      height_ = h;
      __atomic_store_n(&resolution_pending_, true, __ATOMIC_RELEASE);
      return copy(reply, capacity, "OK ");
    }
    return copy(reply, capacity, "ERROR bad size");
  }
  return copy(reply, capacity, "ERROR Unknown command");
}

void VmwareTools::run() {
  char command[256], reply[64];
  uint32_t reply_length = 0;
  int failures = 0;
  while (!__atomic_load_n(&stop_, __ATOMIC_ACQUIRE)) {
    // Each turn sends the answer to the previous command (nothing at first) and takes the next command, if the host has one.
    if (!tclo_.send(reply, reply_length)) {
      // The channel was reset (a snapshot restore, say): open it again. A host that keeps refusing is given up on.
      if (++failures > 10 || !tclo_.open(BackdoorChannel::kProtocolTclo)) return;
      reply_length = 0;
      continue;
    }
    failures = 0;
    const int32_t n = tclo_.receive(command, sizeof command - 1);
    if (n > 0) {
      command[n] = 0;
      reply_length = answer(command, static_cast<uint32_t>(n), reply, sizeof reply);
      continue;   // (send the answer at once)
    }
    reply_length = 0;
    kernel::sleep_us(100000);
  }
}

void VmwareTools::service_main(void* self) { static_cast<VmwareTools*>(self)->run(); }

bool VmwareTools::start() {
  if (thread_) return true;
  if (!Backdoor::present() || !tclo_.open(BackdoorChannel::kProtocolTclo)) return false;
  announce();
  stop_ = false;
  thread_ = kernel::spawn(service_main, this, "vmware-tools");
  if (!thread_) tclo_.close();
  return thread_ != nullptr;
}

void VmwareTools::stop() {
  if (!thread_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(thread_);
  thread_ = nullptr;
  tclo_.close();
}

bool VmwareTools::take_shutdown_request(bool* restart) {
  if (!__atomic_exchange_n(&shutdown_pending_, false, __ATOMIC_ACQ_REL)) return false;
  *restart = restart_;
  return true;
}

bool VmwareTools::take_resolution(uint32_t* width, uint32_t* height) {
  if (!__atomic_exchange_n(&resolution_pending_, false, __ATOMIC_ACQ_REL)) return false;
  *width = width_;
  *height = height_;
  return true;
}

}  // namespace hardware

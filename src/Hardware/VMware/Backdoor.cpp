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

#include "Hardware/VMware/Backdoor.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"

namespace hardware {

void Backdoor::call(BackdoorRegisters& r) {
  r.eax = kMagic;
  r.edx = (r.edx & 0xFFFF0000u) | kPort;
  __asm__ volatile("inl %%dx, %%eax" : "+a"(r.eax), "+b"(r.ebx), "+c"(r.ecx), "+d"(r.edx), "+S"(r.esi), "+D"(r.edi)::"memory");
}

bool Backdoor::probe() {
  present_ = false;
  if (!(cpuid(1).ecx & (1u << 31))) return false;   // no hypervisor: the port is just empty
  BackdoorRegisters r;
  r.ebx = 0xFFFFFFFFu;
  r.ecx = kCommandGetVersion;
  call(r);
  if (r.ebx != kMagic || r.eax == 0xFFFFFFFFu) return false;
  version_ = r.eax;
  product_ = r.ecx;
  present_ = true;
  return true;
}

uint64_t Backdoor::cpu_hz() {
  if (!present_) return 0;
  BackdoorRegisters r;
  r.ecx = kCommandGetHz;
  call(r);
  if (r.eax == 0xFFFFFFFFu) return 0;
  return (uint64_t{r.ebx} << 32) | r.eax;
}

// --- Messages -------------------------------------------------------------------------------------------------------
// Message sub-commands (the high half of ECX) and the status flags the host returns there.
namespace {
constexpr uint32_t kOpen = 0, kSendSize = 1, kSendPayload = 2, kReceiveSize = 3, kReceivePayload = 4, kReceiveStatus = 5, kClose = 6;
constexpr uint32_t kStatusSuccess = 0x1, kStatusDoReceive = 0x2;
}  // namespace

bool BackdoorChannel::step(uint32_t type, uint32_t argument, BackdoorRegisters* out) const {
  BackdoorRegisters r;
  r.ebx = argument;
  r.ecx = Backdoor::kCommandMessage | (type << 16);
  r.edx = channel_ << 16;
  Backdoor::call(r);
  *out = r;
  return ((r.ecx >> 16) & kStatusSuccess) != 0;
}

bool BackdoorChannel::open(uint32_t protocol) {
  close();
  BackdoorRegisters r;
  if (!step(kOpen, protocol, &r)) return false;
  channel_ = r.edx >> 16;
  open_ = true;
  return true;
}

void BackdoorChannel::close() {
  if (!open_) return;
  BackdoorRegisters r;
  step(kClose, 0, &r);
  open_ = false;
}

bool BackdoorChannel::send(const char* data, uint32_t length) {
  if (!open_) return false;
  BackdoorRegisters r;
  if (!step(kSendSize, length, &r)) return false;
  for (uint32_t i = 0; i < length; i += 4) {
    uint32_t word = 0;
    for (uint32_t b = 0; b < 4 && i + b < length; ++b) word |= uint32_t{static_cast<uint8_t>(data[i + b])} << (8 * b);
    if (!step(kSendPayload, word, &r)) return false;
  }
  return true;
}

int32_t BackdoorChannel::receive(char* buffer, uint32_t capacity) {
  if (!open_) return -1;
  BackdoorRegisters r;
  if (!step(kReceiveSize, 0, &r)) return -1;
  if (!(((r.ecx >> 16) & kStatusDoReceive))) return 0;   // nothing waiting
  const uint32_t size = r.ebx;
  for (uint32_t i = 0; i < size; i += 4) {
    if (!step(kReceivePayload, 1, &r)) return -1;
    for (uint32_t b = 0; b < 4 && i + b < size; ++b)
      if (i + b < capacity) buffer[i + b] = static_cast<char>(r.ebx >> (8 * b));
  }
  step(kReceiveStatus, 1, &r);   // acknowledge: the host may drop the message
  return static_cast<int32_t>(size < capacity ? size : capacity);
}

}  // namespace hardware

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

#include "Hardware/Mouse/VMware/VmwareMouse.hpp"
#include "Hardware/VMware/Backdoor.hpp"

namespace hardware {

namespace {
constexpr uint32_t kCommandEnable = 0x45414552, kCommandDisable = 0x000000F5, kCommandRequestAbsolute = 0x53424152;
constexpr uint32_t kVersionId = 0x3442554A;
constexpr uint32_t kStatusError = 0xFFFF0000u, kStatusRelative = 0x00010000u;
constexpr uint32_t kButtonLeft = 0x20, kButtonRight = 0x10, kButtonMiddle = 0x08;

void command(uint32_t what) {
  BackdoorRegisters r;
  r.ebx = what;
  r.ecx = Backdoor::kCommandAbsPointerCommand;
  Backdoor::call(r);
}
uint32_t status() {
  BackdoorRegisters r;
  r.ecx = Backdoor::kCommandAbsPointerStatus;
  Backdoor::call(r);
  return r.eax;
}
}  // namespace

bool VmwareMouse::initialize() {
  if (!Backdoor::present()) return false;
  command(kCommandEnable);
  if (status() == kStatusError) return false;
  // (the enable command also queues the interface's version, read next)
  BackdoorRegisters r;
  r.ebx = 1;
  r.ecx = Backdoor::kCommandAbsPointerData;
  Backdoor::call(r);
  if (r.eax != kVersionId) {
    command(kCommandDisable);
    return false;
  }
  command(kCommandRequestAbsolute);
  enabled_ = true;
  return true;
}

void VmwareMouse::shutdown() {
  if (!enabled_) return;
  enabled_ = false;
  command(kCommandDisable);
}

void VmwareMouse::fetch() {
  if (!enabled_) return;
  MouseEvent discard;
  while (ps2_.poll(&discard)) {}   // (their content arrives through the backdoor)
  for (int n = 0; n < 16; ++n) {
    const uint32_t s = status();
    if (s == kStatusError || (s & 0xFFFF) < 4) return;
    BackdoorRegisters r;
    r.ebx = 4;
    r.ecx = Backdoor::kCommandAbsPointerData;
    Backdoor::call(r);   // eax: buttons and flags, ebx/ecx: x and y, edx: wheel
    MouseEvent m{};
    m.buttons = ((r.eax & kButtonLeft) ? mouse_button::kLeft : 0) | ((r.eax & kButtonRight) ? mouse_button::kRight : 0) |
                ((r.eax & kButtonMiddle) ? mouse_button::kMiddle : 0);
    m.wheel = -static_cast<int8_t>(r.edx);   // (VMware counts up as the wheel turns toward the user)
    if (s & kStatusRelative) {
      m.dx = static_cast<int32_t>(r.ebx);
      m.dy = static_cast<int32_t>(r.ecx);
    } else {
      m.absolute = true;
      m.x = r.ebx & 0xFFFF;
      m.y = r.ecx & 0xFFFF;
    }
    push(m);
  }
}

}  // namespace hardware

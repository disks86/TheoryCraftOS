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

// NtTcosGamepad(PULONG64 arguments): this OS's own service (512), behind xinput1_4.dll (and the older XInput names the loader folds into it). The
// work is the game controller service's (src/Hardware/GameController/WindowsGamepadService.hpp), installed at start-up with set_gamepad_handler so
// that the process layer does not depend on it. The arguments are 8 ULONG64 in the program's memory - one layout for 32- and 64-bit programs:
//
//   [0] operation  [1] the user (player slot 0..7; XInput uses 0..3)  [2..3] parameters  [4..7] results (written back)
//
//   1 GET_STATE      [4] = the packet number (grows when the state changes), [5] = the buttons (XInput's bits, guide 0x0400 included, and this OS's
//                    extra ones above bit 15), [6] = left trigger | right trigger << 8, [7] = left x | left y << 16 | right x << 32 | right y << 48
//                    (signed 16-bit each, up and right positive).
//   2 SET_VIBRATION  [2] the low-frequency (left) motor, [3] the high-frequency (right) one, 0..65535 each.
//   3 GET_INFO       [4] = the protocol (1 Xbox 360, 2 Xbox One, 3 DualShock 4, 4 DualSense, 5 Switch Pro, 6 HID), [5] = vendor id | product id
//                    << 16, [6] = the capabilities (1 rumble, 2 light bar, 4 player LEDs, 8 battery, 16 analog triggers, 32 right stick),
//                    [7] = battery kind (0 unknown, 1 wired, 2 alkaline, 3 rechargeable) | percent << 8 | charging << 16.
//   4 SET_LIGHT      [2] red | green << 8 | blue << 16: the light bar's colour.
// STATUS_DEVICE_NOT_CONNECTED for an empty slot, STATUS_NOT_SUPPORTED for what the pad does not have (or without the handler),
// STATUS_INVALID_PARAMETER for a slot past 7 or an unknown operation, STATUS_ACCESS_VIOLATION for memory the program does not have.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

using GamepadHandler = uint32_t (*)(Process& p, const SyscallRequest& r);
void set_gamepad_handler(GamepadHandler handler);
uint32_t nt_tcos_gamepad(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi

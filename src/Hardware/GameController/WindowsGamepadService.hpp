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

// The kernel side of NtTcosGamepad (src/Process/WindowsGamepad.hpp: the arguments and the answers), over the game controller service.
#pragma once
#include <stdint.h>
#include "Hardware/GameController/GameControllerService.hpp"
#include "Utility/ArrayView.hpp"

namespace process {
class Process;
struct SyscallRequest;
}  // namespace process

namespace hardware {

constexpr uint32_t kGamepadServiceWords = 8;

// One call on the 8 argument words (results written into them); returns the NTSTATUS. Separate from the system call (GamepadServiceCall.cpp),
// so that it is tested without a process.
uint32_t gamepad_service_call(GameControllerService& service, ArrayView<uint64_t> words);
// GET_NAME (5): the name of a slot's pad into `out` (as much as fits); *length = its full length. STATUS_DEVICE_NOT_CONNECTED for an empty slot.
uint32_t gamepad_service_name(GameControllerService& service, uint64_t player, ByteView out, uint32_t* length);
// The system call: reads the words from the program, calls, writes them back.
uint32_t windows_gamepad_service(process::Process& p, const process::SyscallRequest& r);
// Makes NtTcosGamepad reach `service` (null: STATUS_NOT_SUPPORTED again). The service must outlive the programs.
void install_windows_gamepad(GameControllerService* service);

}  // namespace hardware

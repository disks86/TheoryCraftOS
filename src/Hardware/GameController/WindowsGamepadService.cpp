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

#include "Hardware/GameController/WindowsGamepadService.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsGamepad.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kAccessViolation = 0xC0000005u, kNotSupported = 0xC00000BBu;
GameControllerService* g_service = nullptr;
}  // namespace

uint32_t windows_gamepad_service(process::Process& p, const process::SyscallRequest& r) {
  GameControllerService* service = __atomic_load_n(&g_service, __ATOMIC_ACQUIRE);
  if (!service) return kNotSupported;
  const uint64_t arguments = r.args[0];
  uint8_t raw[kGamepadServiceWords * 8];
  if (!p.read_user(arguments, ByteView(raw, sizeof raw))) return kAccessViolation;
  uint64_t w[kGamepadServiceWords];
  ByteReader in{ConstByteView(raw, sizeof raw)};
  for (uint64_t& word : w) word = in.le64();
  const uint32_t status = gamepad_service_call(*service, ArrayView<uint64_t>(w));
  ByteWriter out{ByteView(raw, sizeof raw)};
  for (uint64_t word : w) out.le64(word);
  // (Only the result words change; all eight go back, which the program's memory held a moment ago.)
  if (!p.write_user(arguments, ConstByteView(raw, sizeof raw))) return kAccessViolation;
  return status;
}

void install_windows_gamepad(GameControllerService* service) {
  __atomic_store_n(&g_service, service, __ATOMIC_RELEASE);
  process::windowsabi::set_gamepad_handler(service ? &windows_gamepad_service : nullptr);
}

}  // namespace hardware

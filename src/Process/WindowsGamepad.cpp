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

#include "Process/WindowsGamepad.hpp"

namespace process::windowsabi {

namespace {
GamepadHandler g_handler = nullptr;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;
}  // namespace

void set_gamepad_handler(GamepadHandler handler) { __atomic_store_n(&g_handler, handler, __ATOMIC_RELEASE); }

uint32_t nt_tcos_gamepad(Process& p, const SyscallRequest& r) {
  const GamepadHandler handler = __atomic_load_n(&g_handler, __ATOMIC_ACQUIRE);
  return handler ? handler(p, r) : kStatusNotSupported;
}

}  // namespace process::windowsabi

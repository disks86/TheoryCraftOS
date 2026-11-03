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
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Win32k.hpp"

// The Direct3D services of win32u.dll. The kernel calls install_dxg() after dxgk::set_system_kernel(); the handlers look the kernel up on every call, so a call made before
// there is one answers STATUS_NO_SUCH_DEVICE. What is implemented and how is in docs/win32k/Dxg.md.
namespace process::win32k {

void install_dxg() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  dxg::register_adapters();
  dxg::register_allocations();
  dxg::register_sync();
  dxg::register_work();
  dxg::register_unsupported();
  register_process_exit_hook(dxg::process_exited);
}

}  // namespace process::win32k

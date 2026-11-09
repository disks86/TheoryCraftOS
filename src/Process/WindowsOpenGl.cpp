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

#include "Process/WindowsOpenGl.hpp"

namespace process::windowsabi {

namespace {
OpenGlHandler g_handler = nullptr;
GlWindowPresent g_present = nullptr;
constexpr uint32_t kStatusNotSupported = 0xC00000BBu;
}  // namespace

void set_opengl_handler(OpenGlHandler handler) { g_handler = handler; }
void set_gl_window_present(GlWindowPresent present) { g_present = present; }
GlWindowPresent gl_window_present() { return g_present; }

uint32_t nt_tcos_opengl(Process& p, const SyscallRequest& r) { return g_handler ? g_handler(p, r) : kStatusNotSupported; }

}  // namespace process::windowsabi

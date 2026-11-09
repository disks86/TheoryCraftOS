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

// NtTcosOpenGl(PTCOS_GL_REQUEST request): this OS's own service (509), behind opengl32.dll. The work is the kernel's OpenGL (src/OpenGl/GlService.hpp), installed at start-up with
// set_opengl_handler so that the process layer does not depend on it. One structure in the program's memory (64 bytes, little endian):
//
//    0 ULONG size (64)  4 ULONG operation  8 ULONG context  12 ULONG share  16 ULONG64 window (HWND)  24 PVOID data  32 ULONG length  36 ULONG width  40 ULONG height
//   44 ULONG flags  48 ULONG64 result  56 ULONG major  60 ULONG minor
//
//   1 CREATE_CONTEXT  major, minor, share (0 or a context to share objects with), flags (1 core profile, 2 forward compatible, 4 debug); result = the context.
//   2 DELETE_CONTEXT  context.
//   3 MAKE_CURRENT    context (0: release the thread's), window (0: no drawable), width, height of its client area, flags (1 double buffered, 2 depth, 4 stencil, 8 accumulation,
//                     16 alpha). A context current on another thread: STATUS_RESOURCE_IN_USE.
//   4 EXECUTE         data = a command stream (src/OpenGl/GlStream.hpp), length bytes (at most 64 KiB); result = the result of its last command that has one.
//   5 SWAP            window, width, height (the client size now); result = 1 when the kernel put the picture in the window, 0 when the program must (READ_FRONT + SetDIBitsToDevice).
//   6 READ_FRONT      window, data = width * height * 4 bytes (B, G, R, A, top row first), length = their size; width and height are written back (with data null: only those).
//   7 SHARE_LISTS     share = the source, context = the one that takes its objects.
//   8 FLUSHED         window: shows a single-buffered window after glFlush/glFinish (result as SWAP), result 2 when nothing was drawn since.
// STATUS_INVALID_HANDLE for a context or window that is not there, STATUS_INVALID_DEVICE_STATE for EXECUTE without a current context, STATUS_NOT_SUPPORTED without OpenGL.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ArrayView.hpp"

namespace process::windowsabi {

using OpenGlHandler = uint32_t (*)(Process& p, const SyscallRequest& r);
void set_opengl_handler(OpenGlHandler handler);
uint32_t nt_tcos_opengl(Process& p, const SyscallRequest& r);

// How a picture gets into a window (win32k installs it): rows top first, R, G, B, A bytes. False if the window does not take it (the program then shows it itself).
using GlWindowPresent = bool (*)(uint64_t window, ConstByteView rgba, uint32_t width, uint32_t height);
void set_gl_window_present(GlWindowPresent present);
GlWindowPresent gl_window_present();

}  // namespace process::windowsabi

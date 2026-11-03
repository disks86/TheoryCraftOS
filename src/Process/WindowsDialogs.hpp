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

#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

// NtTcosCommonDialog(PTCOS_COMMON_DIALOG request): this OS's own service, behind comdlg32.dll's GetOpenFileName, GetSaveFileName and ChooseColor. Windows draws those
// dialogs in the program's own process (user32 windows); here the desktop shows them (the file dialog and the colour dialog every program shares) and the program's thread
// waits for the answer. The request is one structure in the program's memory (src/Windows/tcos_syscalls.h, TCOS_COMMON_DIALOG, 72 bytes):
//
//    0 ULONG size (72)           4 ULONG kind (1 open file, 2 save file, 3 colour)     8 ULONG flags (0)       12 ULONG filter index (0-based; in and out)
//   16 ULONG colour (in, a COLORREF)   20 ULONG colour (out, a COLORREF)       24 ULONG accepted (out: 1 = the user chose, 0 = cancelled)
//   32 PCWSTR title (or 0)   40 PCWSTR start path (a DOS path to a folder or a file, or 0)   48 PCWSTR filters (pairs "name\0pattern;pattern\0", ended by an empty name; or 0)
//   56 PWSTR result buffer   64 ULONG capacity of it in WCHARs   68 ULONG length of the path in WCHARs without the NUL (out)
//
// The patterns of the filters are the extensions without "*." ("txt;md"), or "*". The path comes back as a DOS path (C:\...), so a file outside the program's C: drive cannot be
// chosen (STATUS_OBJECT_PATH_NOT_FOUND, nothing chosen). Answers STATUS_NOT_SUPPORTED when the machine has no screen to show the dialog on or another dialog is open,
// STATUS_BUFFER_TOO_SMALL (with the length it needs and `accepted` set) when the path does not fit.
uint32_t nt_tcos_common_dialog(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi

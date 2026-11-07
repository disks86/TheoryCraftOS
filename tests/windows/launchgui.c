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

/* The window program launchtest starts: tells, through the pipe whose handle is on its command line, what its standard handles are and whether each is either none or
 * a handle of a known type ("usable"), as a C runtime needs them to be. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR line, int show) {
  (void)instance; (void)previous; (void)show;
  const wchar_t* at = wcsstr(GetCommandLineW(), L"--pipe=");
  if (!at) return 2;
  HANDLE pipe = (HANDLE)(ULONG_PTR)wcstoull(at + 7, NULL, 10);
  char text[400];
  int n = 0, usable = 1;
  for (DWORD which = STD_INPUT_HANDLE; which >= STD_ERROR_HANDLE; --which) {
    HANDLE h = GetStdHandle(which);
    const DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : 0;
    if (h && h != INVALID_HANDLE_VALUE && type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) usable = 0;
    n += snprintf(text + n, sizeof text - n, "%p/%lu ", h, type);
  }
  n += snprintf(text + n, sizeof text - n, "%s", usable ? "usable" : "not usable");
  /* What Chromium's children do to log to the console of the program that started them (base::RouteStdioToConsole): attach to it, then open CONOUT$ the way the C
     runtime's freopen("CONOUT$", "w") does (CREATE_ALWAYS), and write. */
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const DWORD error = out == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    DWORD written = 0;
    const BOOL wrote = out != INVALID_HANDLE_VALUE && WriteFile(out, "launchgui: through CONOUT$\n", 28, &written, NULL);
    n += snprintf(text + n, sizeof text - n, "; attached, CONOUT$ %p error %lu type %lu wrote %d", out, error, out != INVALID_HANDLE_VALUE ? GetFileType(out) : 0, wrote);
  } else {
    n += snprintf(text + n, sizeof text - n, "; not attached (error %lu)", GetLastError());
  }
  DWORD wrote = 0;
  return WriteFile(pipe, text, (DWORD)n, &wrote, NULL) ? 0 : 3;
}

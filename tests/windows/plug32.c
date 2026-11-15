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

/* A small 32-bit library that tests/windows/crt32.c loads while it runs, the way a setup program loads its plug-ins: from a folder of temporary
 * files, by its full path (tests/wow64_qemu.sh puts it on the disk as C:\plug32.dll).
 *   i686-w64-mingw32-gcc -O1 -shared -o plug32.dll tests/windows/plug32.c */
#include <windows.h>

static int g_attached;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) g_attached = 1;
  return TRUE;
}
/* (cdecl, five arguments, and named "_": the shape of an NSIS plug-in's function) */
__declspec(dllexport) void __cdecl _(HWND parent, int string_size, char *variables, void **stack, void *extra) {
  (void)parent; (void)variables; (void)stack;
  *(int *)extra = 1000 + string_size + g_attached;
}
__declspec(dllexport) int __stdcall Twice(int value) { return 2 * value; }

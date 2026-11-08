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

/* comctl32.dll for TheoryCraftOS: the registration of the common controls. A program asks for them before it makes its windows; the controls' window classes themselves
 * (list views, trees, progress bars ...) are not here yet, so a window of one of those classes fails to be created as for any unknown class. Everything else is still a
 * stub (src/Windows/stubs/comctl32.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

typedef struct { DWORD dwSize, dwICC; } InitCommonControlsExArgs;      /* INITCOMMONCONTROLSEX */

/* Success for a well-formed request, as Windows answers (the classes are "registered": there is nothing to register). */
EXPORT BOOL WINAPI InitCommonControlsEx(const InitCommonControlsExArgs *init) {
  if (!init || init->dwSize != sizeof *init) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT void WINAPI InitCommonControls(void) {}

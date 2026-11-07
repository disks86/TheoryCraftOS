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

/* powrprof.dll for TheoryCraftOS (also api-ms-win-power-base / -setting, which the loader folds into it): CallNtPowerInformation is NtPowerInformation (the kernel answers
 * the levels programs ask for: battery state, capabilities, the processors' clock), the platform role is a desktop, and the active power scheme is Balanced. Power
 * settings are not kept, so reading one finds nothing (ERROR_FILE_NOT_FOUND, as for a setting that is not in the scheme). The rest is still stubs
 * (src/Windows/stubs/powrprof.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) LONG NTAPI NtPowerInformation(int level, PVOID in, ULONG in_length, PVOID out, ULONG out_length);

#define PlatformRoleDesktop_ 1
#define POWER_PLATFORM_ROLE_V2_ 2

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT LONG WINAPI CallNtPowerInformation(int level, PVOID in, ULONG in_length, PVOID out, ULONG out_length) {
  return NtPowerInformation(level, in, in_length, out, out_length);
}

EXPORT int WINAPI PowerDeterminePlatformRole(void) { return PlatformRoleDesktop_; }
EXPORT int WINAPI PowerDeterminePlatformRoleEx(ULONG version) {
  if (version != 1 && version != POWER_PLATFORM_ROLE_V2_) return 0;      /* PlatformRoleUnspecified */
  return PlatformRoleDesktop_;
}

/* The Balanced scheme, {381b4222-f694-41f0-9685-ff5bb260df2e}, in a LocalAlloc block the caller frees with LocalFree. */
EXPORT DWORD WINAPI PowerGetActiveScheme(HKEY root, GUID **scheme) {
  (void)root;
  if (!scheme) return ERROR_INVALID_PARAMETER;
  static const GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
  GUID *copy = LocalAlloc(LMEM_FIXED, sizeof *copy);
  if (!copy) { *scheme = NULL; return ERROR_NOT_ENOUGH_MEMORY; }
  *copy = kBalanced;
  *scheme = copy;
  return ERROR_SUCCESS;
}

/* PowerReadACValue / PowerReadDCValue(root, scheme, subgroup, setting, type, buffer, buffer size) */
static DWORD no_setting(PULONG type, LPDWORD size) {
  if (type) *type = 0;
  if (size) *size = 0;
  return ERROR_FILE_NOT_FOUND;
}
EXPORT DWORD WINAPI PowerReadACValue(HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, PULONG type, LPBYTE buffer, LPDWORD size) {
  (void)root; (void)scheme; (void)subgroup; (void)setting; (void)buffer;
  return no_setting(type, size);
}
EXPORT DWORD WINAPI PowerReadDCValue(HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, PULONG type, PUCHAR buffer, LPDWORD size) {
  (void)root; (void)scheme; (void)subgroup; (void)setting; (void)buffer;
  return no_setting(type, size);
}

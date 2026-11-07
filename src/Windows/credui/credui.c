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

/* credui.dll for TheoryCraftOS: the credential prompts. There is no credential dialog yet, so a prompt ends as if the user cancelled it (ERROR_CANCELLED) with every
 * out-parameter cleared; a stub answered NO_ERROR, which told the program that the user had typed credentials it never got. The rest is still stubs
 * (src/Windows/stubs/credui.inc). Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT DWORD WINAPI CredUIPromptForWindowsCredentialsW(void *info, DWORD error, ULONG *package, LPCVOID in, ULONG in_size, LPVOID *out, ULONG *out_size, BOOL *save,
                                                        DWORD flags) {
  (void)info; (void)error; (void)in; (void)in_size; (void)flags;
  if (out) *out = NULL;
  if (out_size) *out_size = 0;
  if (save) *save = FALSE;
  (void)package;      /* (in and out: left as the caller set it) */
  return ERROR_CANCELLED;
}
EXPORT DWORD WINAPI CredUIPromptForWindowsCredentialsA(void *info, DWORD error, ULONG *package, LPCVOID in, ULONG in_size, LPVOID *out, ULONG *out_size, BOOL *save,
                                                        DWORD flags) {
  return CredUIPromptForWindowsCredentialsW(info, error, package, in, in_size, out, out_size, save, flags);
}

/* CredUIPromptForCredentials(info, target, reserved, error, user, user size, password, password size, save, flags) */
EXPORT DWORD WINAPI CredUIPromptForCredentialsW(void *info, LPCWSTR target, void *reserved, DWORD error, LPWSTR user, ULONG user_size, LPWSTR password,
                                                ULONG password_size, BOOL *save, DWORD flags) {
  (void)info; (void)target; (void)reserved; (void)error; (void)user; (void)user_size; (void)flags;
  if (password && password_size) password[0] = 0;
  if (save) *save = FALSE;
  return ERROR_CANCELLED;
}
EXPORT DWORD WINAPI CredUIPromptForCredentialsA(void *info, LPCSTR target, void *reserved, DWORD error, LPSTR user, ULONG user_size, LPSTR password,
                                                ULONG password_size, BOOL *save, DWORD flags) {
  (void)info; (void)target; (void)reserved; (void)error; (void)user; (void)user_size; (void)flags;
  if (password && password_size) password[0] = 0;
  if (save) *save = FALSE;
  return ERROR_CANCELLED;
}

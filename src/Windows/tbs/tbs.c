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

/* tbs.dll for TheoryCraftOS: the TPM Base Services. There is no TPM driver here: every call says TBS_E_TPM_NOT_FOUND and gives no context, no device information and
 * no result bytes. Built freestanding over kernel32.dll and ntdll.dll. */
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

typedef UINT32 TBS_RESULT;
#define TBS_E_INVALID_CONTEXT ((TBS_RESULT)0x80284004u)
#define TBS_E_TPM_NOT_FOUND ((TBS_RESULT)0x8028400Fu)

EXPORT TBS_RESULT WINAPI Tbsi_GetDeviceInfo(UINT32 size, void *info) { (void)size; (void)info; return TBS_E_TPM_NOT_FOUND; }
EXPORT TBS_RESULT WINAPI Tbsi_Context_Create(const void *parameters, void **context) { (void)parameters; if (context) *context = NULL; return TBS_E_TPM_NOT_FOUND; }
EXPORT TBS_RESULT WINAPI Tbsip_Context_Close(void *context) { (void)context; return TBS_E_INVALID_CONTEXT; }
EXPORT TBS_RESULT WINAPI Tbsip_Submit_Command(void *context, UINT32 locality, UINT32 priority, const BYTE *command, UINT32 command_size, BYTE *result, UINT32 *result_size) {
  (void)context; (void)locality; (void)priority; (void)command; (void)command_size; (void)result;
  if (result_size) *result_size = 0;
  return TBS_E_TPM_NOT_FOUND;
}

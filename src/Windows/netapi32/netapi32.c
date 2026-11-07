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

/* netapi32.dll for TheoryCraftOS: what a program asks about the machine's place in a network. The machine is a stand-alone workstation in the workgroup "WORKGROUP",
 * joined to no domain and no Azure AD tenant (NetGetAadJoinInformation succeeds with no information, which is how Windows says "not joined"). The buffers the
 * functions hand out come from NetApiBufferAllocate (the process heap) and go back with NetApiBufferFree. There is no user database to query remotely:
 * NetUserGetInfo says the user is not found. Built freestanding over kernel32.dll and ntdll.dll. */
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

#define NERR_Success 0
#define NERR_UserNotFound 2221
#define NERR_InvalidComputer 2351
#define NERR_BufTooSmall 2123

static const WCHAR kWorkgroup[] = L"WORKGROUP";

static void copy_bytes(void *to, const void *from, SIZE_T n) {
  BYTE *d = to;
  const BYTE *s = from;
  while (n--) *d++ = *s++;
}

/* ------------------------------------------------------------------------------------------------------------------- buffers */

EXPORT DWORD WINAPI NetApiBufferAllocate(DWORD size, LPVOID *buffer) {
  if (!buffer) return ERROR_INVALID_PARAMETER;
  *buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
  return *buffer ? NERR_Success : ERROR_NOT_ENOUGH_MEMORY;
}

EXPORT DWORD WINAPI NetApiBufferFree(LPVOID buffer) {
  if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
  return NERR_Success;
}

EXPORT DWORD WINAPI NetApiBufferSize(LPVOID buffer, LPDWORD size) {
  if (!buffer || !size) return ERROR_INVALID_PARAMETER;
  const SIZE_T n = HeapSize(GetProcessHeap(), 0, buffer);
  if (n == (SIZE_T)-1) return ERROR_INVALID_PARAMETER;
  *size = (DWORD)n;
  return NERR_Success;
}

EXPORT DWORD WINAPI NetApiBufferReallocate(LPVOID old, DWORD size, LPVOID *buffer) {
  if (!buffer) return ERROR_INVALID_PARAMETER;
  if (!old) return NetApiBufferAllocate(size, buffer);
  if (!size) { NetApiBufferFree(old); *buffer = NULL; return NERR_Success; }
  void *p = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, old, size);
  if (!p) return ERROR_NOT_ENOUGH_MEMORY;
  *buffer = p;
  return NERR_Success;
}

/* A copy of a string in a buffer of NetApiBufferAllocate. */
static WCHAR *buffer_string(const WCHAR *text) {
  SIZE_T n = 0;
  while (text[n]) ++n;
  WCHAR *out;
  if (NetApiBufferAllocate((DWORD)((n + 1) * sizeof(WCHAR)), (LPVOID *)&out) != NERR_Success) return NULL;
  copy_bytes(out, text, (n + 1) * sizeof(WCHAR));
  return out;
}

/* Only this machine is known: no server name, an empty one, or its own name (with or without the leading backslashes). */
static BOOL is_local(LPCWSTR server) {
  if (!server || !*server) return TRUE;
  while (*server == '\\') ++server;
  WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (!GetComputerNameW(name, &n)) return FALSE;
  DWORD i = 0;
  for (; name[i] && server[i]; ++i) {
    WCHAR a = name[i], b = server[i];
    if (a >= 'a' && a <= 'z') a = (WCHAR)(a - 32);
    if (b >= 'a' && b <= 'z') b = (WCHAR)(b - 32);
    if (a != b) return FALSE;
  }
  return !name[i] && !server[i];
}

/* ------------------------------------------------------------------------------------------------------------------- join state */

/* NETSETUP_JOIN_STATUS: NetSetupWorkgroupName (2). */
EXPORT DWORD WINAPI NetGetJoinInformation(LPCWSTR server, LPWSTR *name, DWORD *type) {
  if (name) *name = NULL;
  if (type) *type = 0;
  if (!name || !type) return ERROR_INVALID_PARAMETER;
  if (!is_local(server)) return NERR_InvalidComputer;
  *name = buffer_string(kWorkgroup);
  if (!*name) return ERROR_NOT_ENOUGH_MEMORY;
  *type = 2;
  return NERR_Success;
}

/* Azure AD: S_OK with no information is "not joined". */
EXPORT HRESULT WINAPI NetGetAadJoinInformation(LPCWSTR tenant, void **info) {
  (void)tenant;
  if (!info) return E_INVALIDARG;
  *info = NULL;
  return S_OK;
}

EXPORT void WINAPI NetFreeAadJoinInformation(void *info) {
  if (info) HeapFree(GetProcessHeap(), 0, info);      /* (never handed out) */
}

/* DSROLE_PRIMARY_DOMAIN_INFO_BASIC (level DsRolePrimaryDomainInfoBasic, 1): a stand-alone workstation (DsRole_RoleStandaloneWorkstation, 0) in the workgroup. The
 * strings are in the same block, after the structure, so that DsRoleFreeMemory frees everything. */
typedef struct {
  DWORD MachineRole;
  ULONG Flags;
  LPWSTR DomainNameFlat;
  LPWSTR DomainNameDns;
  LPWSTR DomainForestName;
  GUID DomainGuid;
} DsRoleBasic;

EXPORT DWORD WINAPI DsRoleGetPrimaryDomainInformation(LPCWSTR server, DWORD level, PBYTE *buffer) {
  if (!buffer) return ERROR_INVALID_PARAMETER;
  *buffer = NULL;
  if (!is_local(server)) return ERROR_BAD_NETPATH;
  if (level != 1) return ERROR_INVALID_PARAMETER;      /* (the upgrade status and operation state levels are not kept) */
  const SIZE_T size = sizeof(DsRoleBasic) + sizeof kWorkgroup;
  BYTE *block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
  if (!block) return ERROR_NOT_ENOUGH_MEMORY;
  DsRoleBasic *info = (DsRoleBasic *)block;
  info->MachineRole = 0;
  info->DomainNameFlat = (LPWSTR)(block + sizeof(DsRoleBasic));
  copy_bytes(info->DomainNameFlat, kWorkgroup, sizeof kWorkgroup);
  *buffer = block;
  return ERROR_SUCCESS;
}

EXPORT void WINAPI DsRoleFreeMemory(PVOID buffer) {
  if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
}

/* ------------------------------------------------------------------------------------------------------------------- users */

EXPORT DWORD WINAPI NetUserGetInfo(LPCWSTR server, LPCWSTR user, DWORD level, LPBYTE *buffer) {
  (void)user;
  (void)level;
  if (buffer) *buffer = NULL;
  if (!buffer) return ERROR_INVALID_PARAMETER;
  if (!is_local(server)) return NERR_InvalidComputer;
  return NERR_UserNotFound;
}

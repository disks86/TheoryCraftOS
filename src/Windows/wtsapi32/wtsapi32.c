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

/* wtsapi32.dll for TheoryCraftOS: the terminal-services session of the process. There is one session, the local console: always active, never remote. Its user is the
 * user of the process (advapi32's GetUserNameW, looked up when needed: this library is linked over kernel32 only), its domain the computer name. The buffers given out
 * are LocalAlloc'd and freed by WTSFreeMemory. Session notifications are accepted and never sent (sessions do not change). The rest is still stubs
 * (src/Windows/stubs/wtsapi32.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define WTS_CURRENT_SESSION_ ((DWORD)-1)
enum {      /* WTS_INFO_CLASS */
  WTSInitialProgram_ = 0, WTSApplicationName_, WTSWorkingDirectory_, WTSOEMId_, WTSSessionId_, WTSUserName_, WTSWinStationName_, WTSDomainName_, WTSConnectState_,
  WTSClientBuildNumber_, WTSClientName_, WTSClientDirectory_, WTSClientProductId_, WTSClientHardwareId_, WTSClientAddress_, WTSClientDisplay_, WTSClientProtocolType_,
  WTSIsRemoteSession_ = 29
};
#define WTSActive_ 0

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

static DWORD current_session(void) {
  DWORD id = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &id)) id = 1;
  return id;
}

/* A copy of `size` bytes in a LocalAlloc block, the result of a query. */
static BOOL give(const void *data, DWORD size, LPWSTR *buffer, DWORD *bytes) {
  BYTE *block = LocalAlloc(LMEM_FIXED, size ? size : 1);
  if (!block) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  for (DWORD i = 0; i < size; ++i) block[i] = ((const BYTE *)data)[i];
  *buffer = (LPWSTR)block;
  if (bytes) *bytes = size;
  return TRUE;
}
static BOOL give_text(const WCHAR *text, LPWSTR *buffer, DWORD *bytes) {
  DWORD n = 0;
  while (text[n]) ++n;
  return give(text, (n + 1) * (DWORD)sizeof(WCHAR), buffer, bytes);
}

static BOOL user_name(WCHAR *out, DWORD room) {
  typedef BOOL (WINAPI *GetUserNameW_)(LPWSTR, LPDWORD);
  HMODULE advapi32 = LoadLibraryW(L"advapi32.dll");
  GetUserNameW_ get = advapi32 ? (GetUserNameW_)(void *)GetProcAddress(advapi32, "GetUserNameW") : NULL;
  DWORD n = room;
  return get && get(out, &n);
}

EXPORT BOOL WINAPI WTSQuerySessionInformationW(HANDLE server, DWORD session, int info_class, LPWSTR *buffer, DWORD *bytes) {
  (void)server;      /* (WTS_CURRENT_SERVER_HANDLE: there is no other server) */
  if (buffer) *buffer = NULL;
  if (bytes) *bytes = 0;
  if (!buffer || !bytes) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD id = current_session();
  if (session != WTS_CURRENT_SESSION_ && session != id) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
  switch (info_class) {
    case WTSSessionId_: return give(&id, sizeof id, buffer, bytes);
    case WTSConnectState_: { const DWORD state = WTSActive_; return give(&state, sizeof state, buffer, bytes); }
    case WTSClientProtocolType_: { const USHORT console = 0; return give(&console, sizeof console, buffer, bytes); }
    case WTSIsRemoteSession_: { const BOOL remote = FALSE; return give(&remote, sizeof remote, buffer, bytes); }
    case WTSClientBuildNumber_: { const ULONG build = 0; return give(&build, sizeof build, buffer, bytes); }
    case WTSUserName_: {
      WCHAR name[257];
      if (!user_name(name, 257)) name[0] = 0;
      return give_text(name, buffer, bytes);
    }
    case WTSDomainName_: {
      WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
      DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
      if (!GetComputerNameW(name, &n)) name[0] = 0;
      return give_text(name, buffer, bytes);
    }
    case WTSWinStationName_: return give_text(L"Console", buffer, bytes);
    case WTSInitialProgram_: case WTSApplicationName_: case WTSWorkingDirectory_: case WTSClientName_: case WTSClientDirectory_: case WTSOEMId_:
      return give_text(L"", buffer, bytes);
    default: SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
  }
}

EXPORT BOOL WINAPI WTSQuerySessionInformationA(HANDLE server, DWORD session, int info_class, LPSTR *buffer, DWORD *bytes) {
  LPWSTR wide = NULL;
  DWORD wide_bytes = 0;
  if (buffer) *buffer = NULL;
  if (bytes) *bytes = 0;
  if (!WTSQuerySessionInformationW(server, session, info_class, &wide, &wide_bytes)) return FALSE;
  switch (info_class) {
    case WTSUserName_: case WTSDomainName_: case WTSWinStationName_: case WTSInitialProgram_: case WTSApplicationName_: case WTSWorkingDirectory_: case WTSClientName_:
    case WTSClientDirectory_: case WTSOEMId_: {
      const int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
      char *narrow = n > 0 ? LocalAlloc(LMEM_FIXED, (SIZE_T)n) : NULL;
      if (!narrow || WideCharToMultiByte(CP_ACP, 0, wide, -1, narrow, n, NULL, NULL) <= 0) {
        if (narrow) LocalFree(narrow);
        LocalFree(wide);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
      }
      LocalFree(wide);
      *buffer = narrow;
      *bytes = (DWORD)n;
      return TRUE;
    }
    default:
      *buffer = (LPSTR)wide;
      *bytes = wide_bytes;
      return TRUE;
  }
}

EXPORT void WINAPI WTSFreeMemory(PVOID memory) {
  if (memory) LocalFree(memory);
}

EXPORT BOOL WINAPI WTSRegisterSessionNotification(HWND window, DWORD flags) {
  (void)flags;
  if (!window) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI WTSUnRegisterSessionNotification(HWND window) {
  if (!window) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI WTSRegisterSessionNotificationEx(HANDLE server, HWND window, DWORD flags) { (void)server; return WTSRegisterSessionNotification(window, flags); }
EXPORT BOOL WINAPI WTSUnRegisterSessionNotificationEx(HANDLE server, HWND window) { (void)server; return WTSUnRegisterSessionNotification(window); }

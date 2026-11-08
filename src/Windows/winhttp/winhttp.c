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

/* winhttp.dll for TheoryCraftOS: the parts of WinHTTP that a program asks before it talks to the network. The proxy configuration is "no proxy, no auto-detection"
 * (WinHttpGetIEProxyConfigForCurrentUser, WinHttpGetDefaultProxyConfiguration), automatic proxy discovery always fails the way it fails on a network without WPAD,
 * WinHttpCrackUrl is real (both the pointer-into-the-URL and the copy-into-your-buffer forms). Sessions, connections and requests are real handles of a table here, so
 * options, timeouts and callbacks can be set and the handles closed, but no request is ever sent: WinHttpSendRequest fails with ERROR_WINHTTP_CANNOT_CONNECT and the
 * calls after it with ERROR_WINHTTP_INCORRECT_HANDLE_STATE (programs fall back to their own HTTP stack, as Chrome does). The rest of the exports stay stubs
 * (src/Windows/stubs/winhttp.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define _WINHTTP_INTERNAL_
#include <windows.h>
#include <winhttp.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------------- handles */

/* A handle is HANDLE_BASE + its slot, checked against the table (a made-up value is refused). */
enum { KIND_FREE, KIND_SESSION, KIND_CONNECT, KIND_REQUEST, KIND_RESOLVER };
typedef struct {
  int kind;
  UINT parent;      /* slot of the session (connect) or the connection (request) */
  WINHTTP_STATUS_CALLBACK callback;
  DWORD notifications;
  DWORD_PTR context;
} Handle;
#define MAX_HANDLES 256
#define HANDLE_BASE 0x57480000u
static Handle g_handles[MAX_HANDLES];
static SRWLOCK g_lock = SRWLOCK_INIT;

static HINTERNET handle_new(int kind, UINT parent) {
  AcquireSRWLockExclusive(&g_lock);
  UINT slot = 1;      /* (slot 0 is never used: no handle equals HANDLE_BASE) */
  while (slot < MAX_HANDLES && g_handles[slot].kind != KIND_FREE) ++slot;
  if (slot == MAX_HANDLES) { ReleaseSRWLockExclusive(&g_lock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  g_handles[slot].kind = kind;
  g_handles[slot].parent = parent;
  g_handles[slot].callback = NULL;
  g_handles[slot].notifications = 0;
  g_handles[slot].context = 0;
  /* A connection and a request start with the callback of their session, as on Windows. */
  if (parent && parent < MAX_HANDLES) {
    g_handles[slot].callback = g_handles[parent].callback;
    g_handles[slot].notifications = g_handles[parent].notifications;
  }
  ReleaseSRWLockExclusive(&g_lock);
  return (HINTERNET)(ULONG_PTR)(HANDLE_BASE + slot);
}

/* The slot of a handle of one of the kinds in the mask (1 << kind), or 0 with the error set. */
static UINT slot_of(HINTERNET handle, unsigned kinds) {
  const ULONG_PTR h = (ULONG_PTR)handle;
  if (!handle) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (h <= HANDLE_BASE || h >= HANDLE_BASE + MAX_HANDLES) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  const UINT slot = (UINT)(h - HANDLE_BASE);
  AcquireSRWLockShared(&g_lock);
  const int kind = g_handles[slot].kind;
  ReleaseSRWLockShared(&g_lock);
  if (kind == KIND_FREE) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (!(kinds & (1u << kind))) { SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE); return 0; }
  return slot;
}
#define ANY_KIND ((1u << KIND_SESSION) | (1u << KIND_CONNECT) | (1u << KIND_REQUEST) | (1u << KIND_RESOLVER))

EXPORT BOOL WINAPI WinHttpCloseHandle(HINTERNET handle) {
  const UINT slot = slot_of(handle, ANY_KIND);
  if (!slot) return FALSE;
  AcquireSRWLockExclusive(&g_lock);
  g_handles[slot].kind = KIND_FREE;
  ReleaseSRWLockExclusive(&g_lock);
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

EXPORT BOOL WINAPI WinHttpCheckPlatform(void) { return TRUE; }

/* ------------------------------------------------------------------------------------------------------------------------- sessions */

EXPORT HINTERNET WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags) {
  (void)agent;
  (void)bypass;
  if (access == WINHTTP_ACCESS_TYPE_NAMED_PROXY && (!proxy || !*proxy)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (flags & ~(DWORD)(WINHTTP_FLAG_ASYNC | 0x20000000 /* WINHTTP_FLAG_SECURE_DEFAULTS */)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  HINTERNET h = handle_new(KIND_SESSION, 0);
  if (h) SetLastError(ERROR_SUCCESS);
  return h;
}

EXPORT HINTERNET WINAPI WinHttpConnect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved) {
  (void)port;
  const UINT s = slot_of(session, 1u << KIND_SESSION);
  if (!s) return NULL;
  if (!server || !*server || reserved) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return handle_new(KIND_CONNECT, s);
}

EXPORT HINTERNET WINAPI WinHttpOpenRequest(HINTERNET connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version, LPCWSTR referrer, LPCWSTR *accept_types, DWORD flags) {
  (void)verb;
  (void)object;
  (void)version;
  (void)referrer;
  (void)accept_types;
  (void)flags;
  const UINT c = slot_of(connect, 1u << KIND_CONNECT);
  if (!c) return NULL;
  return handle_new(KIND_REQUEST, c);
}

EXPORT BOOL WINAPI WinHttpAddRequestHeaders(HINTERNET request, LPCWSTR headers, DWORD length, DWORD modifiers) {
  (void)length;
  (void)modifiers;
  if (!slot_of(request, 1u << KIND_REQUEST)) return FALSE;
  if (!headers) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_SUCCESS);
  return TRUE;      /* (kept nowhere: the request is never sent) */
}

EXPORT BOOL WINAPI WinHttpSendRequest(HINTERNET request, LPCWSTR headers, DWORD headers_length, LPVOID optional, DWORD optional_length, DWORD total_length,
                                      DWORD_PTR context) {
  (void)headers;
  (void)headers_length;
  (void)optional;
  (void)optional_length;
  (void)total_length;
  (void)context;
  if (!slot_of(request, 1u << KIND_REQUEST)) return FALSE;
  SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
  return FALSE;
}

/* The calls that need a sent request. */
static BOOL not_sent(HINTERNET request) {
  if (!slot_of(request, 1u << KIND_REQUEST)) return FALSE;
  SetLastError(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}

EXPORT BOOL WINAPI WinHttpReceiveResponse(HINTERNET request, LPVOID reserved) {
  (void)reserved;
  return not_sent(request);
}

EXPORT BOOL WINAPI WinHttpQueryHeaders(HINTERNET request, DWORD level, LPCWSTR name, LPVOID buffer, LPDWORD length, LPDWORD index) {
  (void)level;
  (void)name;
  (void)buffer;
  (void)index;
  if (length && !buffer) *length = 0;
  return not_sent(request);
}

EXPORT BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET request, LPDWORD available) {
  if (available) *available = 0;
  return not_sent(request);
}

EXPORT BOOL WINAPI WinHttpReadData(HINTERNET request, LPVOID buffer, DWORD size, LPDWORD read) {
  (void)buffer;
  (void)size;
  if (read) *read = 0;
  return not_sent(request);
}

EXPORT BOOL WINAPI WinHttpWriteData(HINTERNET request, LPCVOID buffer, DWORD size, LPDWORD written) {
  (void)buffer;
  (void)size;
  if (written) *written = 0;
  return not_sent(request);
}

EXPORT WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(HINTERNET handle, WINHTTP_STATUS_CALLBACK callback, DWORD notifications, DWORD_PTR reserved) {
  (void)reserved;
  const UINT slot = slot_of(handle, ANY_KIND);
  if (!slot) return WINHTTP_INVALID_STATUS_CALLBACK;
  AcquireSRWLockExclusive(&g_lock);
  WINHTTP_STATUS_CALLBACK previous = g_handles[slot].callback;
  g_handles[slot].callback = callback;
  g_handles[slot].notifications = notifications;
  ReleaseSRWLockExclusive(&g_lock);
  SetLastError(ERROR_SUCCESS);
  return previous;
}

EXPORT BOOL WINAPI WinHttpSetTimeouts(HINTERNET handle, int resolve, int connect, int send, int receive) {
  if (!slot_of(handle, (1u << KIND_SESSION) | (1u << KIND_CONNECT) | (1u << KIND_REQUEST))) return FALSE;
  if (resolve < -1 || connect < -1 || send < -1 || receive < -1) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------------- proxies */

EXPORT BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *config) {
  if (!config) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  config->fAutoDetect = FALSE;
  config->lpszAutoConfigUrl = NULL;
  config->lpszProxy = NULL;
  config->lpszProxyBypass = NULL;
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

EXPORT BOOL WINAPI WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *info) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  info->dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
  info->lpszProxy = NULL;
  info->lpszProxyBypass = NULL;
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

EXPORT BOOL WINAPI WinHttpDetectAutoProxyConfigUrl(DWORD flags, LPWSTR *url) {
  if (url) *url = NULL;
  if (!url || !flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED);
  return FALSE;
}

/* No proxy auto-configuration: there is no WPAD server to find and no script engine to run a PAC file. */
EXPORT BOOL WINAPI WinHttpGetProxyForUrl(HINTERNET session, LPCWSTR url, WINHTTP_AUTOPROXY_OPTIONS *options, WINHTTP_PROXY_INFO *info) {
  if (info) { info->dwAccessType = 0; info->lpszProxy = NULL; info->lpszProxyBypass = NULL; }
  if (!slot_of(session, 1u << KIND_SESSION)) return FALSE;
  if (!url || !*url || !options || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_WINHTTP_AUTODETECTION_FAILED);
  return FALSE;
}

/* The resolver API of Windows 8: not offered, so programs use WinHttpGetProxyForUrl (Chrome does). */
EXPORT DWORD WINAPI WinHttpCreateProxyResolver(HINTERNET session, HINTERNET *resolver) {
  if (resolver) *resolver = NULL;
  if (!slot_of(session, 1u << KIND_SESSION)) return GetLastError();
  if (!resolver) return ERROR_INVALID_PARAMETER;
  return ERROR_NOT_SUPPORTED;
}

EXPORT DWORD WINAPI WinHttpGetProxyForUrlEx(HINTERNET resolver, PCWSTR url, WINHTTP_AUTOPROXY_OPTIONS *options, DWORD_PTR context) {
  (void)url;
  (void)options;
  (void)context;
  if (!slot_of(resolver, 1u << KIND_RESOLVER)) return GetLastError();
  return ERROR_WINHTTP_AUTODETECTION_FAILED;
}

EXPORT DWORD WINAPI WinHttpGetProxyResult(HINTERNET resolver, WINHTTP_PROXY_RESULT *result) {
  if (result) { result->cEntries = 0; result->pEntries = NULL; }
  if (!slot_of(resolver, 1u << KIND_RESOLVER)) return GetLastError();
  if (!result) return ERROR_INVALID_PARAMETER;
  return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
}

/* (A result of this DLL never holds entries; one filled in by the program is left to the program.) */
EXPORT VOID WINAPI WinHttpFreeProxyResult(WINHTTP_PROXY_RESULT *result) {
  if (!result) return;
  result->cEntries = 0;
  result->pEntries = NULL;
}

/* ------------------------------------------------------------------------------------------------------------------------- WinHttpCrackUrl */

static DWORD wide_length(LPCWSTR s) {
  DWORD n = 0;
  while (s[n]) ++n;
  return n;
}
static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static BOOL equal_ascii_i(LPCWSTR s, DWORD n, const char *word) {
  DWORD i = 0;
  for (; i < n && word[i]; ++i)
    if (lower(s[i]) != (WCHAR)word[i]) return FALSE;
  return i == n && !word[i];
}
static int hex_value(WCHAR c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* One part of the URL [start, start + n) (start may be NULL for a part that is not there), given to the caller the way its URL_COMPONENTS field asks:
 * NULL with a length - point into the URL; a buffer with a length - copy (decoding %XX for ICU_DECODE); NULL and 0 - not wanted. Gives FALSE for a buffer too small,
 * with the length needed (terminator included) in *length. */
static BOOL give_part(LPCWSTR start, DWORD n, BOOL decode, LPWSTR *pointer, DWORD *length) {
  if (!*pointer) {
    if (*length) {
      *pointer = (LPWSTR)start;
      *length = start ? n : 0;
    }
    return TRUE;
  }
  if (!*length) return TRUE;      /* (a buffer of no size: not wanted) */
  DWORD needed = 0;
  for (DWORD i = 0; i < n; ++i) {
    if (decode && start[i] == '%' && i + 2 < n && hex_value(start[i + 1]) >= 0 && hex_value(start[i + 2]) >= 0) i += 2;
    ++needed;
  }
  if (needed >= *length) {
    *length = needed + 1;
    return FALSE;
  }
  DWORD o = 0;
  for (DWORD i = 0; i < n; ++i) {
    if (decode && start[i] == '%' && i + 2 < n && hex_value(start[i + 1]) >= 0 && hex_value(start[i + 2]) >= 0) {
      (*pointer)[o++] = (WCHAR)(hex_value(start[i + 1]) * 16 + hex_value(start[i + 2]));
      i += 2;
    } else {
      (*pointer)[o++] = start[i];
    }
  }
  (*pointer)[o] = 0;
  *length = o;
  return TRUE;
}

EXPORT BOOL WINAPI WinHttpCrackUrl(LPCWSTR url, DWORD url_length, DWORD flags, LPURL_COMPONENTS parts) {
  if (!url || !parts || parts->dwStructSize != sizeof(URL_COMPONENTS)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags & ~(DWORD)(ICU_DECODE | ICU_ESCAPE | ICU_REJECT_USERPWD)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD n = url_length ? url_length : wide_length(url);
  const BOOL decode = (flags & ICU_DECODE) != 0;
  /* scheme ":" "//" */
  DWORD colon = 0;
  while (colon < n && url[colon] != ':') ++colon;
  if (colon == n || colon == 0) { SetLastError(ERROR_WINHTTP_UNRECOGNIZED_SCHEME); return FALSE; }
  INTERNET_SCHEME scheme;
  INTERNET_PORT default_port;
  if (equal_ascii_i(url, colon, "http")) { scheme = INTERNET_SCHEME_HTTP; default_port = INTERNET_DEFAULT_HTTP_PORT; }
  else if (equal_ascii_i(url, colon, "https")) { scheme = INTERNET_SCHEME_HTTPS; default_port = INTERNET_DEFAULT_HTTPS_PORT; }
  else { SetLastError(ERROR_WINHTTP_UNRECOGNIZED_SCHEME); return FALSE; }
  if (n - colon < 3 || url[colon + 1] != '/' || url[colon + 2] != '/') { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
  /* authority: [user[:password]@]host[:port] up to "/", "?", "#" */
  const DWORD authority = colon + 3;
  DWORD end = authority;
  while (end < n && url[end] != '/' && url[end] != '?' && url[end] != '#') ++end;
  DWORD at = end;
  for (DWORD i = authority; i < end; ++i)
    if (url[i] == '@') at = i;
  LPCWSTR user = NULL, password = NULL;
  DWORD user_n = 0, password_n = 0, host = authority;
  if (at != end) {
    if (flags & ICU_REJECT_USERPWD) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
    DWORD split = authority;
    while (split < at && url[split] != ':') ++split;
    user = url + authority;
    user_n = split - authority;
    if (split < at) { password = url + split + 1; password_n = at - split - 1; }
    host = at + 1;
  }
  /* host (an IPv6 literal keeps its brackets) and port */
  DWORD host_end = host;
  if (host_end < end && url[host_end] == '[') {
    while (host_end < end && url[host_end] != ']') ++host_end;
    if (host_end == end) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
    ++host_end;
  } else {
    while (host_end < end && url[host_end] != ':') ++host_end;
  }
  if (host_end == host) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
  DWORD port = default_port;
  if (host_end < end) {
    if (url[host_end] != ':') { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
    if (host_end + 1 < end) {
      port = 0;
      for (DWORD i = host_end + 1; i < end; ++i) {
        if (url[i] < '0' || url[i] > '9') { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
        port = port * 10 + (DWORD)(url[i] - '0');
        if (port > 65535) { SetLastError(ERROR_WINHTTP_INVALID_URL); return FALSE; }
      }
    }
  }
  /* path up to "?" or "#", then the extra information (query and fragment, with their "?" / "#") */
  DWORD path_end = end;
  while (path_end < n && url[path_end] != '?' && url[path_end] != '#') ++path_end;
  BOOL ok = TRUE;
  ok &= give_part(url, colon, FALSE, &parts->lpszScheme, &parts->dwSchemeLength);
  ok &= give_part(url + host, host_end - host, FALSE, &parts->lpszHostName, &parts->dwHostNameLength);
  ok &= give_part(user, user_n, decode, &parts->lpszUserName, &parts->dwUserNameLength);
  ok &= give_part(password, password_n, decode, &parts->lpszPassword, &parts->dwPasswordLength);
  ok &= give_part(url + end, path_end - end, decode, &parts->lpszUrlPath, &parts->dwUrlPathLength);
  ok &= give_part(url + path_end, n - path_end, decode, &parts->lpszExtraInfo, &parts->dwExtraInfoLength);
  parts->nScheme = scheme;
  parts->nPort = (INTERNET_PORT)port;
  if (!ok) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

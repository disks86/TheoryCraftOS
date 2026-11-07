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

/* winhttp.dll on TheoryCraftOS: WinHttpCrackUrl (pointer and copy forms, ports, user info, IPv6 hosts, query and fragment, errors, short buffers), the proxy
 * configuration (no proxy, no auto-detection), the handle table and the requests that cannot be sent (mingw gcc, -lwinhttp). Prints "winhttptest: N checks, M failed". */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

/* A part given as a pointer into the URL and a length is the text expected. */
static int part_is(const WCHAR *p, DWORD n, const WCHAR *expected) {
  if (!expected) return p == NULL && n == 0;
  return p && n == wcslen(expected) && wcsncmp(p, expected, n) == 0;
}

static void pointer_form(void) {
  static const WCHAR url[] = L"https://user:pw@www.example.com:8443/a/b.html?x=1&y=2#frag";
  URL_COMPONENTS c;
  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.dwSchemeLength = c.dwHostNameLength = c.dwUserNameLength = c.dwPasswordLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
  check(WinHttpCrackUrl(url, 0, 0, &c), "crack: full URL");
  check(c.nScheme == INTERNET_SCHEME_HTTPS && c.nPort == 8443, "crack: scheme and port");
  check(part_is(c.lpszScheme, c.dwSchemeLength, L"https"), "crack: scheme text");
  check(part_is(c.lpszHostName, c.dwHostNameLength, L"www.example.com"), "crack: host");
  check(part_is(c.lpszUserName, c.dwUserNameLength, L"user"), "crack: user");
  check(part_is(c.lpszPassword, c.dwPasswordLength, L"pw"), "crack: password");
  check(part_is(c.lpszUrlPath, c.dwUrlPathLength, L"/a/b.html"), "crack: path");
  check(part_is(c.lpszExtraInfo, c.dwExtraInfoLength, L"?x=1&y=2#frag"), "crack: extra info");
  check(c.lpszHostName >= url && c.lpszHostName < url + wcslen(url), "crack: points into the URL");

  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.dwHostNameLength = c.dwUserNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
  check(WinHttpCrackUrl(L"http://example.org", 0, 0, &c), "crack: host only");
  check(c.nScheme == INTERNET_SCHEME_HTTP && c.nPort == 80, "crack: default port 80");
  check(part_is(c.lpszHostName, c.dwHostNameLength, L"example.org"), "crack: host only text");
  check(part_is(c.lpszUserName, c.dwUserNameLength, NULL), "crack: no user");
  check(c.dwUrlPathLength == 0 && c.dwExtraInfoLength == 0, "crack: empty path and extra");

  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.dwHostNameLength = c.dwUrlPathLength = (DWORD)-1;
  check(WinHttpCrackUrl(L"https://[::1]/x", 0, 0, &c), "crack: IPv6 literal");
  check(part_is(c.lpszHostName, c.dwHostNameLength, L"[::1]") && c.nPort == 443, "crack: IPv6 host and default port 443");
  check(part_is(c.lpszUrlPath, c.dwUrlPathLength, L"/x"), "crack: IPv6 path");

  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.dwHostNameLength = (DWORD)-1;
  check(WinHttpCrackUrl(L"http://a.b:81/zzzz", 13, 0, &c), "crack: explicit length");
  check(part_is(c.lpszHostName, c.dwHostNameLength, L"a.b") && c.nPort == 81, "crack: explicit length cuts the URL");
}

static void copy_form(void) {
  WCHAR host[64], path[64], extra[64], scheme[8];
  URL_COMPONENTS c;
  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.lpszHostName = host; c.dwHostNameLength = 64;
  c.lpszUrlPath = path; c.dwUrlPathLength = 64;
  c.lpszExtraInfo = extra; c.dwExtraInfoLength = 64;
  c.lpszScheme = scheme; c.dwSchemeLength = 8;
  check(WinHttpCrackUrl(L"http://host.test/p%20q?r", 0, ICU_DECODE, &c), "copy: crack");
  check(wcscmp(host, L"host.test") == 0 && c.dwHostNameLength == 9, "copy: host and its length");
  check(wcscmp(path, L"/p q") == 0 && c.dwUrlPathLength == 4, "copy: decoded path");
  check(wcscmp(extra, L"?r") == 0 && wcscmp(scheme, L"http") == 0, "copy: extra and scheme");

  WCHAR tiny[4];
  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.lpszHostName = tiny; c.dwHostNameLength = 4;
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http://longhost/", 0, 0, &c) && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "copy: short buffer fails");
  check(c.dwHostNameLength == 9, "copy: short buffer gives the size needed");
}

static void errors(void) {
  URL_COMPONENTS c;
  memset(&c, 0, sizeof c);
  c.dwStructSize = sizeof c;
  c.dwHostNameLength = (DWORD)-1;
  SetLastError(0);
  check(!WinHttpCrackUrl(L"ftp://x/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_UNRECOGNIZED_SCHEME, "error: ftp scheme");
  SetLastError(0);
  check(!WinHttpCrackUrl(L"no scheme here", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_UNRECOGNIZED_SCHEME, "error: no scheme");
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http:/x", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL, "error: one slash");
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http://x:99999/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL, "error: port too large");
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http://x:8a/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL, "error: port not a number");
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http:///path", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL, "error: no host");
  c.dwStructSize = 4;
  SetLastError(0);
  check(!WinHttpCrackUrl(L"http://x/", 0, 0, &c) && GetLastError() == ERROR_INVALID_PARAMETER, "error: bad structure size");
  /* Every prefix of a URL is cracked or refused, never read past. */
  static const WCHAR url[] = L"https://u:p@[fe80::1]:443/a?b#c";
  for (DWORD n = 1; n < wcslen(url); ++n) {
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.dwHostNameLength = c.dwUserNameLength = c.dwPasswordLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
    if (WinHttpCrackUrl(url, n, 0, &c)) check(c.lpszHostName + c.dwHostNameLength <= url + n, "prefix: inside the given length");
  }
}

static void proxies_and_handles(void) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie;
  memset(&ie, 0xCC, sizeof ie);
  check(WinHttpGetIEProxyConfigForCurrentUser(&ie), "proxy: IE config");
  check(!ie.fAutoDetect && !ie.lpszAutoConfigUrl && !ie.lpszProxy && !ie.lpszProxyBypass, "proxy: no proxy, no auto-detection");

  HINTERNET session = WinHttpOpen(L"test", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  check(session != NULL, "handles: session");
  check(WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000), "handles: timeouts");
  check(WinHttpSetStatusCallback(session, NULL, 0, 0) == NULL, "handles: no previous callback");

  WINHTTP_AUTOPROXY_OPTIONS options;
  memset(&options, 0, sizeof options);
  options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
  options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
  WINHTTP_PROXY_INFO info;
  SetLastError(0);
  check(!WinHttpGetProxyForUrl(session, L"http://example.com/", &options, &info) && GetLastError() == ERROR_WINHTTP_AUTODETECTION_FAILED,
        "proxy: auto-detection fails");

  HINTERNET connect = WinHttpConnect(session, L"example.com", 80, 0);
  check(connect != NULL, "handles: connection");
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
  check(request != NULL, "handles: request");
  SetLastError(0);
  check(!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT,
        "handles: send fails to connect");
  SetLastError(0);
  check(!WinHttpConnect(request, L"x", 80, 0) && GetLastError() == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE, "handles: wrong handle kind");
  check(WinHttpCloseHandle(request) && WinHttpCloseHandle(connect) && WinHttpCloseHandle(session), "handles: close");
  SetLastError(0);
  check(!WinHttpCloseHandle(session) && GetLastError() == ERROR_INVALID_HANDLE, "handles: closing twice fails");
  check(!WinHttpCloseHandle((HINTERNET)(ULONG_PTR)0x1234), "handles: made-up handle refused");
}

int main(void) {
  pointer_form();
  copy_form();
  errors();
  proxies_and_handles();
  printf("winhttptest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}

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

/* Build: x86_64-w64-mingw32-gcc -O1 -o wsadownloadtest.exe wsadownloadtest.c -lws2_32
 * Run:   wsadownloadtest.exe cachefly.cachefly.net /10mb.test 4   (writes C:\nettest.txt; needs the network) */
/* Reads an HTTP download the way Chrome's TCPSocketWin does (WSAEventSelect FD_READ|FD_CLOSE, non-blocking recv once per readiness, WSAEnumNetworkEvents),
 * with a reader per thread for N connections at once, and says when a read waits more than 5 s although the kernel says bytes are there. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
static const char *g_host, *g_path; static FILE *g_out; static CRITICAL_SECTION g_lock;
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); EnterCriticalSection(&g_lock); vfprintf(g_out, fmt, a); fflush(g_out); LeaveCriticalSection(&g_lock); va_end(a); }
static DWORD WINAPI reader(LPVOID arg) {
  int id = (int)(INT_PTR)arg;
  struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(g_host, "80", &hints, &res) || !res) { say("%d: resolve failed %d\n", id, WSAGetLastError()); return 1; }
  SOCKET s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
  if (connect(s, res->ai_addr, (int)res->ai_addrlen)) { say("%d: connect failed %d\n", id, WSAGetLastError()); return 1; }
  char req[512]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: nettest/1\r\nConnection: close\r\n\r\n", g_path, g_host);
  send(s, req, n, 0);
  WSAEVENT ev = WSACreateEvent();
  WSAEventSelect(s, ev, FD_READ | FD_CLOSE);
  static char buf[8][32768]; long long total = 0; DWORD start = GetTickCount(); int stalls = 0;
  for (;;) {
    int r = recv(s, buf[id], sizeof buf[id], 0);
    if (r > 0) { total += r; continue; }
    if (r == 0) break;
    int e = WSAGetLastError();
    if (e != WSAEWOULDBLOCK) { say("%d: recv error %d\n", id, e); break; }
    DWORD w = WaitForSingleObject(ev, 5000);
    if (w == WAIT_TIMEOUT) {
      u_long avail = 0; ioctlsocket(s, FIONREAD, &avail);
      say("%d: STALL after %lld bytes: no event in 5 s, %lu bytes readable\n", id, total, avail);
      if (++stalls > 3) break;
      continue;
    }
    WSANETWORKEVENTS ne; WSAEnumNetworkEvents(s, ev, &ne);
    if (!(ne.lNetworkEvents & (FD_READ | FD_CLOSE))) say("%d: event without FD_READ/FD_CLOSE (%ld)\n", id, ne.lNetworkEvents);
  }
  say("%d: done %lld bytes in %lu ms, %d stalls\n", id, total, GetTickCount() - start, stalls);
  closesocket(s);
  return 0;
}
int main(int argc, char **argv) {
  g_out = fopen("C:\\nettest.txt", "w"); InitializeCriticalSection(&g_lock);
  g_host = argc > 1 ? argv[1] : "speedtest.tele2.net"; g_path = argc > 2 ? argv[2] : "/10MB.zip";
  int count = argc > 3 ? atoi(argv[3]) : 4; if (count > 8) count = 8;
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  say("nettest %s%s x%d\n", g_host, g_path, count);
  HANDLE t[8]; for (int i = 0; i < count; ++i) t[i] = CreateThread(NULL, 0, reader, (LPVOID)(INT_PTR)i, 0, NULL);
  WaitForMultipleObjects(count, t, TRUE, 240000);
  say("all done\n");
  fclose(g_out);
  return 0;
}

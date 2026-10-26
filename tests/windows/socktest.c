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

/* ws2_32 on TheoryCraftOS (mingw gcc; see tests/windows/build.sh): a lighter smoke test than the host test (tests/WindowsSocketTest.cpp), which already covers the TCP/UDP
 * logic itself against a simulated peer. This one checks only what does not depend on a real network actually being reachable in the sandbox this runs in (there is no
 * loopback interface: a program's own address is one specific NIC's, reached only by whatever QEMU's network backend does with traffic sent to it, which this test does
 * not rely on) - that the DLL loads and links, WSAStartup/WSACleanup, socket()/bind()/closesocket(), getsockname() after bind, ioctlsocket(FIONBIO), the local-only error a
 * getpeername() of an unconnected socket gives, select() on a socket nothing is waiting on (write-ready: a fresh or UDP socket always is), and the byte-order helpers.
 * Prints "socktest: N checks, M failed"; the exit code is the number of failures. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (WSA error %d)\n", what, WSAGetLastError());
}

int main(void) {
  WSADATA data;
  check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
  check(data.wVersion == MAKEWORD(2, 2), "WSAStartup negotiates 2.2");

  check(htons(ntohs((u_short)0x1234)) == 0x1234, "htons/ntohs round trip");
  check(htonl(ntohl(0x01020304u)) == 0x01020304u, "htonl/ntohl round trip");
  check(htons(1) == 0x0100, "htons swaps the bytes (host is little-endian)");
  check(inet_addr("10.0.2.5") == htonl(0x0A000205u), "inet_addr matches a hand-built network-order address");
  check(inet_addr("not an address") == INADDR_NONE, "inet_addr rejects garbage");

  SOCKET tcp = socket(AF_INET, SOCK_STREAM, 0);
  check(tcp != INVALID_SOCKET, "socket() creates a TCP socket");
  SOCKET udp = socket(AF_INET, SOCK_DGRAM, 0);
  check(udp != INVALID_SOCKET, "socket() creates a UDP socket");
  SOCKET bad = socket(AF_INET, 99 /* not SOCK_STREAM or SOCK_DGRAM */, 0);
  check(bad == INVALID_SOCKET && WSAGetLastError() == WSAEINVAL, "socket() rejects an unknown type");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = 0;
  check(bind(udp, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind() an ephemeral UDP port");
  struct sockaddr_in bound;
  int bound_len = sizeof(bound);
  check(getsockname(udp, (struct sockaddr *)&bound, &bound_len) == 0 && bound.sin_port != 0, "getsockname() reports the assigned port");

  struct sockaddr_in unused;
  int unused_len = sizeof(unused);
  check(getpeername(tcp, (struct sockaddr *)&unused, &unused_len) == SOCKET_ERROR && WSAGetLastError() == WSAENOTCONN,
        "getpeername() of an unconnected socket fails with WSAENOTCONN");

  u_long one = 1;
  check(ioctlsocket(tcp, FIONBIO, &one) == 0, "ioctlsocket(FIONBIO) makes a socket non-blocking");

  /* select(): no real traffic needed - a fresh TCP socket (nothing to wait on yet) and any UDP socket are always write-ready. */
  fd_set writefds;
  FD_ZERO(&writefds);
  FD_SET(tcp, &writefds);
  FD_SET(udp, &writefds);
  struct timeval no_wait;
  no_wait.tv_sec = 0; no_wait.tv_usec = 0;
  const int ready = select(0, NULL, &writefds, NULL, &no_wait);
  check(ready == 2 && FD_ISSET(tcp, &writefds) && FD_ISSET(udp, &writefds), "select() finds both sockets write-ready at once, no timeout needed");

  check(closesocket(tcp) == 0, "closesocket() the TCP socket");
  check(closesocket(udp) == 0, "closesocket() the UDP socket");
  check(send(tcp, "x", 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK, "using a closed handle fails with WSAENOTSOCK, not a crash");

  check(WSACleanup() == 0, "WSACleanup");
  printf("socktest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

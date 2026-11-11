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

/* A Windows program that is an X client of the kernel's X server: Winsock's AF_UNIX (<afunix.h>) to the abstract name "/tmp/.X11-unix/X0" (the one
 * the Linux programs' libxcb uses too, shared by both kinds of program), then a window opened, drawn and read back (tests/x11/x11session.h). Prints
 * "x11-windows: N checks, M failed". Built with mingw; tests/x11_emulated.sh runs it on the kernel's CPU emulator with the OS's own DLLs. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include "../x11/x11session.h"

typedef struct {
  unsigned short sun_family;
  char sun_path[108];
} SockaddrUn;      /* <afunix.h> */

static int io_send(void* context, const unsigned char* data, unsigned long size) {
  SOCKET s = *(SOCKET*)context;
  while (size) {
    const int n = send(s, (const char*)data, (int)size, 0);
    if (n <= 0) return 0;
    data += n;
    size -= (unsigned long)n;
  }
  return 1;
}
static int io_recv(void* context, unsigned char* data, unsigned long size) {
  SOCKET s = *(SOCKET*)context;
  while (size) {
    const int n = recv(s, (char*)data, (int)size, 0);
    if (n <= 0) return 0;
    data += n;
    size -= (unsigned long)n;
  }
  return 1;
}
static void io_fail(void* context, const char* what, int line) {
  (void)context;
  printf("FAIL line %d: %s\n", line, what);
}

int main(int argc, char** argv) {
  WSADATA wsa;
  unsigned checks = 1, failed = 0;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    printf("x11-windows: WSAStartup failed\n");
    return 1;
  }
  SOCKET s = socket(1 /* AF_UNIX */, SOCK_STREAM, 0);
  SockaddrUn address;
  memset(&address, 0, sizeof address);
  address.sun_family = 1;
  const char* name = "/tmp/.X11-unix/X0";
  memcpy(address.sun_path + 1, name, strlen(name));      /* (a leading NUL: the abstract name) */
  if (s == INVALID_SOCKET || connect(s, (const struct sockaddr*)&address, (int)(2 + 1 + strlen(name))) != 0) {
    printf("FAIL: no connection to the X server (%d)\n", WSAGetLastError());
    ++failed;
  } else {
    X11Session session;
    session.io.context = &s;
    session.io.send = io_send;
    session.io.recv = io_recv;
    session.io.fail = io_fail;
    session.checks = session.failed = 0;
    x11_run(&session, "Windows X11 client", 0x22AA44);
    checks += session.checks;
    failed += session.failed;
  }
  printf("x11-windows: %u checks, %u failed\n", checks, failed);
  fflush(stdout);
  if (argc > 1) Sleep(8000);
  if (s != INVALID_SOCKET) closesocket(s);
  WSACleanup();
  (void)argv;
  return failed ? 1 : 0;
}

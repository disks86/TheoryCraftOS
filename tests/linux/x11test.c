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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that talks to the kernel's X server through socket(2) and
 * connect(2) the way libxcb does for DISPLAY=:0: once at the path /tmp/.X11-unix/X0 and once at the abstract name. Each connection opens a window,
 * draws, reads the pixels back (tests/x11/x11session.h). Prints "x11-linux: N checks, M failed"; then stays a few seconds when given an argument
 * (so the desktop can be looked at) and exits 1 when something failed. tests/x11_emulated.sh runs it on the kernel's CPU emulator. */
#include "../x11/x11session.h"

#if defined(__x86_64__)
static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_SOCKET = 41, NR_CONNECT = 42, NR_NANOSLEEP = 35, NR_EXIT_GROUP = 231 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_SOCKET = 198, NR_CONNECT = 203, NR_NANOSLEEP = 101, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys3(long n, long a, long b, long c) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_SOCKET = 198, NR_CONNECT = 203, NR_NANOSLEEP = 101, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  mv a0, sp\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif

static void out(const char* s) { sys3(NR_WRITE, 1, (long)s, (long)x11_len(s)); }
static void out_u(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = (char)('0' + v % 10), v /= 10;
  while (v);
  out(b + i);
}

static int io_send(void* context, const unsigned char* data, unsigned long size) {
  const long fd = *(const long*)context;
  while (size) {
    const long n = sys3(NR_WRITE, fd, (long)data, (long)size);
    if (n <= 0) return 0;
    data += n;
    size -= (unsigned long)n;
  }
  return 1;
}
static int io_recv(void* context, unsigned char* data, unsigned long size) {
  const long fd = *(const long*)context;
  while (size) {
    const long n = sys3(NR_READ, fd, (long)data, (long)size);
    if (n <= 0) return 0;
    data += n;
    size -= (unsigned long)n;
  }
  return 1;
}
static void io_fail(void* context, const char* what, int line) {
  (void)context;
  out("FAIL line ");
  out_u((unsigned long)line);
  out(": ");
  out(what);
  out("\n");
}

/* A connection to the X server: the path name, or the abstract one (sun_path starting with a NUL). -1 on failure. */
static long connect_x(int abstract_name) {
  const char* path = "/tmp/.X11-unix/X0";
  unsigned char address[110];
  for (unsigned i = 0; i < sizeof address; ++i) address[i] = 0;
  address[0] = 1;      /* AF_UNIX */
  unsigned at = 2 + (abstract_name ? 1u : 0u);
  for (const char* p = path; *p; ++p) address[at++] = (unsigned char)*p;
  const long fd = sys3(NR_SOCKET, 1, 1, 0);      /* AF_UNIX, SOCK_STREAM */
  if (fd < 0) return -1;
  if (sys3(NR_CONNECT, fd, (long)address, abstract_name ? (long)at : (long)at + 1) != 0) {
    sys3(NR_CLOSE, fd, 0, 0);
    return -1;
  }
  return fd;
}

__attribute__((used)) void start_c(long* stack) {
  const long argc = stack[0];
  unsigned checks = 0, failed = 0;
  static const char* const titles[2] = {"Linux X11 (path)", "Linux X11 (abstract)"};
  static const unsigned colours[2] = {0x2266CC, 0xCC6622};
  long fds[2] = {-1, -1};
  for (int k = 0; k < 2; ++k) {
    fds[k] = connect_x(k);
    ++checks;
    if (fds[k] < 0) {
      ++failed;
      out(k ? "FAIL: no connection at the abstract name\n" : "FAIL: no connection at /tmp/.X11-unix/X0\n");
      continue;
    }
    X11Session s;
    s.io.context = &fds[k];
    s.io.send = io_send;
    s.io.recv = io_recv;
    s.io.fail = io_fail;
    s.checks = s.failed = 0;
    x11_run(&s, titles[k], colours[k]);
    checks += s.checks;
    failed += s.failed;
  }
  out("x11-linux: ");
  out_u(checks);
  out(" checks, ");
  out_u(failed);
  out(" failed\n");
  if (argc > 1) {      /* stay on screen a while */
    const long ts[2] = {8, 0};
    sys3(NR_NANOSLEEP, (long)ts, 0, 0);
  }
  for (int k = 0; k < 2; ++k) if (fds[k] >= 0) sys3(NR_CLOSE, fds[k], 0, 0);
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
  for (;;) {}
}

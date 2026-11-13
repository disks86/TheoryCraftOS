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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that uses named pipes the way programs do: mknodat(S_IFIFO) - mkfifo() -
 * in /tmp, stat()/fstat() report S_IFIFO, open() follows POSIX (writing only without waiting and no reader: ENXIO), read/write/ppoll, the end of the file
 * and POLLHUP when the writer goes, unlink. Then the kernel's module-pipe-sink and -source (loaded by the emulator with TCOS_EMU_PIPE): it reads half a
 * second of raw PCM from /tmp/music.output (what the emulator plays into the pipe sink: 440 Hz, s16le 48 kHz stereo) and counts the tone's zero
 * crossings, and writes a second of a 600 Hz triangle into /tmp/music.input for the emulator to hear from the pipe source.
 * Prints "fifotest: N checks, M failed". Run by tests/pipewire_emulated.sh [arch] pipe. */

#if defined(__x86_64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_FSTAT = 5, NR_OPENAT = 257, NR_MKNODAT = 259, NR_NEWFSTATAT = 262, NR_UNLINKAT = 263, NR_PPOLL = 271,
       NR_EXIT_GROUP = 231 };
enum { ST_MODE = 24 };      /* (struct stat's st_mode: x86-64's layout) */
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e,
                x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_FSTAT = 80, NR_OPENAT = 56, NR_MKNODAT = 33, NR_NEWFSTATAT = 79, NR_UNLINKAT = 35, NR_PPOLL = 73,
       NR_EXIT_GROUP = 94 };
enum { ST_MODE = 16 };      /* (the generic layout) */
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e,
                a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_FSTAT = 80, NR_OPENAT = 56, NR_MKNODAT = 33, NR_NEWFSTATAT = 79, NR_UNLINKAT = 35, NR_PPOLL = 73,
       NR_EXIT_GROUP = 94 };
enum { ST_MODE = 16 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  mv a0, sp\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif
static long sys3(long n, long a, long b, long c) { return sys6(n, a, b, c, 0, 0, 0); }

/* (the compiler may call these for initialisers and copies: there is no C library) */
void* memset(void* d, int c, unsigned long n) { unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)c; return d; }
void* memcpy(void* d, const void* s, unsigned long n) { unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s; while (n--) *p++ = *q++; return d; }

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long ulong;

enum { AT_FDCWD = -100, O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_NONBLOCK = 0x800, S_IFIFO = 010000, S_IFMT = 0170000, POLLIN = 1, POLLHUP = 0x10,
       ENOENT = 2, ENXIO = 6, EAGAIN = 11, EEXIST = 17 };

static ulong slen(const char* s) { ulong n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sys3(NR_WRITE, 1, (long)s, (long)slen(s)); }
static void out_i(long v) {
  char b[24];
  int i = 23;
  const int negative = v < 0;
  ulong u = negative ? (ulong)-v : (ulong)v;
  b[i] = 0;
  do b[--i] = (char)('0' + u % 10), u /= 10;
  while (u);
  if (negative) b[--i] = '-';
  out(b + i);
}
static int checks, failed;
static void check(int ok, const char* what) {
  ++checks;
  if (!ok) { ++failed; out("FAILED: "); out(what); out("\n"); }
}
static int same(const u8* a, const char* b, ulong n) { for (ulong i = 0; i < n; ++i) if (a[i] != (u8)b[i]) return 0; return 1; }

static u8 st[256];
static long stat_mode(const char* path, unsigned* mode) {
  const long r = sys6(NR_NEWFSTATAT, AT_FDCWD, (long)path, (long)st, 0, 0, 0);
  *mode = (unsigned)st[ST_MODE] | (unsigned)st[ST_MODE + 1] << 8 | (unsigned)st[ST_MODE + 2] << 16;
  return r;
}
static unsigned fstat_mode(long fd) {
  if (sys3(NR_FSTAT, fd, (long)st, 0) != 0) return 0;
  return (unsigned)st[ST_MODE] | (unsigned)st[ST_MODE + 1] << 8 | (unsigned)st[ST_MODE + 2] << 16;
}
static unsigned poll_one(long fd, unsigned events) {
  u8 pfd[8] = {0};
  long zero[2] = {0, 0};      /* (a timespec of 0: do not wait) */
  pfd[0] = (u8)fd; pfd[1] = (u8)(fd >> 8); pfd[4] = (u8)events;
  if (sys6(NR_PPOLL, (long)pfd, 1, (long)zero, 0, 8, 0) != 1) return 0;
  return (unsigned)pfd[6] | (unsigned)pfd[7] << 8;
}

static u8 buf[96000];

static void basics(void) {
  const char* path = "/tmp/fifotest";
  unsigned mode = 0;
  check(sys6(NR_MKNODAT, AT_FDCWD, (long)path, S_IFIFO | 0644, 0, 0, 0) == 0, "mkfifo");
  check(sys6(NR_MKNODAT, AT_FDCWD, (long)path, S_IFIFO | 0644, 0, 0, 0) == -EEXIST, "mkfifo again: EEXIST");
  check(stat_mode(path, &mode) == 0 && (mode & S_IFMT) == S_IFIFO, "stat: S_IFIFO");
  check(sys6(NR_OPENAT, AT_FDCWD, (long)path, O_WRONLY | O_NONBLOCK, 0, 0, 0) == -ENXIO, "open for writing, not waiting, no reader: ENXIO");
  const long r = sys6(NR_OPENAT, AT_FDCWD, (long)path, O_RDONLY | O_NONBLOCK, 0, 0, 0);
  const long w = sys6(NR_OPENAT, AT_FDCWD, (long)path, O_WRONLY | O_NONBLOCK, 0, 0, 0);
  check(r >= 0 && w >= 0, "open both ends");
  check((fstat_mode(r) & S_IFMT) == S_IFIFO, "fstat: S_IFIFO");
  check(poll_one(r, POLLIN) == 0, "nothing to read yet");
  check(sys3(NR_WRITE, w, (long)"hello fifo", 10) == 10, "write");
  check(poll_one(r, POLLIN) == POLLIN, "ppoll: POLLIN");
  check(sys3(NR_READ, r, (long)buf, 64) == 10 && same(buf, "hello fifo", 10), "read what was written");
  check(sys3(NR_READ, r, (long)buf, 64) == -EAGAIN, "empty: EAGAIN");
  check(sys3(NR_CLOSE, w, 0, 0) == 0, "close the writer");
  check(sys3(NR_READ, r, (long)buf, 64) == 0, "no writer: the end of the file");
  check((poll_one(r, POLLIN) & POLLHUP) != 0, "ppoll: POLLHUP");
  check(sys3(NR_CLOSE, r, 0, 0) == 0, "close the reader");
  check(sys3(NR_UNLINKAT, AT_FDCWD, (long)path, 0) == 0, "unlink");
  check(stat_mode(path, &mode) == -ENOENT, "gone");
}

/* half a second from module-pipe-sink's FIFO (blocking reads: the kernel's end writes as the sink plays) */
static void from_pipe_sink(void) {
  unsigned mode = 0;
  if (stat_mode("/tmp/music.output", &mode) != 0) { out("fifotest: no /tmp/music.output (no pipe sink)\n"); return; }
  check((mode & S_IFMT) == S_IFIFO, "/tmp/music.output is a FIFO");
  const long fd = sys6(NR_OPENAT, AT_FDCWD, (long)"/tmp/music.output", O_RDONLY, 0, 0, 0);      /* (does not wait: the module has it open) */
  check(fd >= 0, "open /tmp/music.output");
  if (fd < 0) return;
  ulong have = 0;
  while (have < sizeof buf) {
    const long n = sys3(NR_READ, fd, (long)(buf + have), (long)(sizeof buf - have));
    if (n <= 0) break;
    have += (ulong)n;
  }
  sys3(NR_CLOSE, fd, 0, 0);
  check(have == sizeof buf, "read half a second of PCM");
  /* the left channel's zero crossings (with a little hysteresis) and its peak: 440 Hz is 440 crossings in half a second */
  long crossings = 0, peak = 0;
  int side = 0;
  for (ulong i = 0; i + 3 < have; i += 4) {
    const long v = (short)(buf[i] | buf[i + 1] << 8);
    if (v > peak) peak = v;
    if (v > 500 && side <= 0) { if (side < 0) ++crossings; side = 1; }
    else if (v < -500 && side >= 0) { if (side > 0) ++crossings; side = -1; }
  }
  out("fifotest: pipe sink: "); out_i(crossings); out(" zero crossings, peak "); out_i(peak); out("\n");
  check(crossings > 420 && crossings < 460 && peak > 4000, "the pipe sink's 440 Hz tone");
}

/* a second of a 600 Hz triangle (80 samples a period, both sides at 8000) into module-pipe-source's FIFO */
static void into_pipe_source(void) {
  unsigned mode = 0;
  if (stat_mode("/tmp/music.input", &mode) != 0) { out("fifotest: no /tmp/music.input (no pipe source)\n"); return; }
  check((mode & S_IFMT) == S_IFIFO, "/tmp/music.input is a FIFO");
  const long fd = sys6(NR_OPENAT, AT_FDCWD, (long)"/tmp/music.input", O_WRONLY, 0, 0, 0);      /* (does not wait: the module reads it) */
  check(fd >= 0, "open /tmp/music.input");
  if (fd < 0) return;
  ulong written = 0;
  for (ulong frame = 0; frame < 48000;) {
    u8 piece[4096];
    ulong n = 0;
    for (; n + 4 <= sizeof piece && frame < 48000; n += 4, ++frame) {
      const long phase = (long)(frame % 80);
      const long v = phase < 40 ? -8000 + phase * 400 : 8000 - (phase - 40) * 400;
      piece[n] = piece[n + 2] = (u8)v;
      piece[n + 1] = piece[n + 3] = (u8)((unsigned long)v >> 8);
    }
    const long put = sys3(NR_WRITE, fd, (long)piece, (long)n);
    if (put != (long)n) break;
    written += n;
  }
  sys3(NR_CLOSE, fd, 0, 0);
  check(written == 48000 * 4, "wrote a second of PCM into the pipe source");
}

void start_c(long* stack) {
  (void)stack;
  basics();
  from_pipe_sink();
  into_pipe_source();
  out("fifotest: "); out_i(checks); out(" checks, "); out_i(failed); out(" failed\n");
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
}

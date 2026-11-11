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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that is a Wayland client the hard way, through the OS's Linux system
 * calls only: it finds the compositor through XDG_RUNTIME_DIR and WAYLAND_DISPLAY, connects with socket(2)/connect(2), speaks the wire protocol itself,
 * keeps its picture in a file in memory (memfd_create, ftruncate, fallocate, F_ADD_SEALS, mmap MAP_SHARED) that it passes with sendmsg(SCM_RIGHTS), takes
 * the keyboard's keymap from recvmsg(SCM_RIGHTS) and maps it (MAP_PRIVATE), shows a decorated toplevel, redraws it in place, grows its pool. Prints
 * "wayland-linux: N checks, M failed"; stays a few seconds when given an argument (so the desktop can be looked at); exits 1 when something failed.
 * tests/wayland_emulated.sh runs it on the kernel's CPU emulator with the compositor and its transport. */

#if defined(__x86_64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_FSTAT = 5, NR_LSEEK = 8, NR_MMAP = 9, NR_MUNMAP = 11, NR_PREAD = 17, NR_SOCKET = 41, NR_CONNECT = 42,
       NR_SENDMSG = 46, NR_RECVMSG = 47, NR_FCNTL = 72, NR_FTRUNCATE = 77, NR_NANOSLEEP = 35, NR_EXIT_GROUP = 231, NR_MEMFD_CREATE = 319, NR_FALLOCATE = 285, NR_PIPE2 = 293 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e,
                x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_FSTAT = 80, NR_LSEEK = 62, NR_MMAP = 222, NR_MUNMAP = 215, NR_PREAD = 67, NR_SOCKET = 198,
       NR_CONNECT = 203, NR_SENDMSG = 211, NR_RECVMSG = 212, NR_FCNTL = 25, NR_FTRUNCATE = 46, NR_NANOSLEEP = 101, NR_EXIT_GROUP = 94, NR_MEMFD_CREATE = 279,
       NR_FALLOCATE = 47, NR_PIPE2 = 59 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e,
                a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_FSTAT = 80, NR_LSEEK = 62, NR_MMAP = 222, NR_MUNMAP = 215, NR_PREAD = 67, NR_SOCKET = 198,
       NR_CONNECT = 203, NR_SENDMSG = 211, NR_RECVMSG = 212, NR_FCNTL = 25, NR_FTRUNCATE = 46, NR_NANOSLEEP = 101, NR_EXIT_GROUP = 94, NR_MEMFD_CREATE = 279,
       NR_FALLOCATE = 47, NR_PIPE2 = 59 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  mv a0, sp\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

/* (the compiler may call it for zeroed arrays; the volatile stores keep it from being made into a call to itself) */
void* memset(void* d, int c, unsigned long n) {
  volatile u8* p = d;
  for (unsigned long i = 0; i < n; ++i) p[i] = (u8)c;
  return d;
}

static unsigned checks, failed;
static unsigned long len(const char* s) { unsigned long n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sys3(NR_WRITE, 1, s, len(s)); }
static void out_u(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = (char)('0' + v % 10), v /= 10;
  while (v);
  out(b + i);
}
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; out("FAIL line "); out_u(__LINE__); out(": " #c "\n"); } } while (0)
static int same(const char* a, const char* b) { while (*a && *a == *b) ++a, ++b; return *a == *b; }
static int starts(const char* s, const char* p) { while (*p && *s == *p) ++s, ++p; return *p == 0; }
static void copy(u8* d, const void* s, unsigned long n) { const u8* p = s; for (unsigned long i = 0; i < n; ++i) d[i] = p[i]; }
static void sleep_ms(long ms) { const long ts[2] = {ms / 1000, (ms % 1000) * 1000000}; sys3(NR_NANOSLEEP, ts, 0, 0); }

/* ---- the wire */
static long sock = -1;
static u8 msg[1024];
static u32 msg_len;
static int pending_fd = -1;      /* goes with the next message */
static void begin(u32 object, u32 opcode) { msg_len = 8; copy(msg, &object, 4); copy(msg + 4, &opcode, 4); }
static void put(u32 v) { copy(msg + msg_len, &v, 4); msg_len += 4; }
static void put_s(const char* s) {
  const u32 n = (u32)len(s) + 1;
  put(n);
  copy(msg + msg_len, s, n);
  msg_len += n;
  while (msg_len % 4) msg[msg_len++] = 0;
}
static void end(void) {
  const u32 word = msg_len << 16 | (u32)(msg[4] | msg[5] << 8);
  copy(msg + 4, &word, 4);
  long iov[2] = {(long)msg, (long)msg_len};
  long control[3] = {0, 0, 0};      /* cmsghdr: len, level | type, one int */
  long hdr[7] = {0, 0, (long)iov, 1, 0, 0, 0};      /* msghdr: name, namelen, iov, iovlen, control, controllen, flags */
  if (pending_fd >= 0) {
    control[0] = 16 + 4;
    control[1] = 1L | (1L << 32);      /* SOL_SOCKET, SCM_RIGHTS */
    copy((u8*)&control[2], &pending_fd, 4);
    hdr[4] = (long)control;
    hdr[5] = 24;
  }
  const long sent = sys3(NR_SENDMSG, sock, hdr, 0);
  if (sent != (long)msg_len) { ++failed; out("FAIL: sendmsg\n"); }
  pending_fd = -1;
}

/* events in; descriptors that came with them, in order */
static u8 in[65536];
static u32 in_len;
static int fds_in[16];
static int fds_count;
static int fill(void) {
  long iov[2] = {(long)(in + in_len), (long)(sizeof in - in_len)};
  long control[8] = {0};
  long hdr[7] = {0, 0, (long)iov, 1, (long)control, sizeof control, 0};
  const long got = sys3(NR_RECVMSG, sock, hdr, 0);
  if (got <= 0) return 0;
  in_len += (u32)got;
  if ((u64)control[0] > 16 && (control[1] & 0xFFFFFFFF) == 1 && (control[1] >> 32) == 1) {
    const int n = (int)(((u64)control[0] - 16) / 4);
    for (int i = 0; i < n && fds_count < 16; ++i) copy((u8*)&fds_in[fds_count++], (u8*)&control[2] + 4 * i, 4);
  }
  return 1;
}
typedef struct { u32 object, opcode, size; const u8* args; } Event;
static Event ev;
static u8 ev_copy[4096];
static int next_event(void) {
  while (in_len < 8 || in_len < (u32)(in[6] | in[7] << 8)) if (!fill()) return 0;
  ev.object = (u32)(in[0] | in[1] << 8 | in[2] << 16 | (u32)in[3] << 24);
  ev.opcode = (u32)(in[4] | in[5] << 8);
  ev.size = (u32)(in[6] | in[7] << 8);
  copy(ev_copy, in + 8, ev.size - 8);
  ev.args = ev_copy;
  for (u32 i = ev.size; i < in_len; ++i) in[i - ev.size] = in[i];
  in_len -= ev.size;
  return 1;
}
static u32 arg(u32 word) { u32 v; copy((u8*)&v, ev.args + 4 * word, 4); return v; }
static int released_21;      /* wl_buffer 21 was released (seen whenever it came) */
/* Reads events until one of that object and opcode came (1), or the connection ended or an error came (0). */
static int wait_event(u32 object, u32 opcode) {
  for (;;) {
    if (!next_event()) return 0;
    if (ev.object == 1 && ev.opcode == 0) { out("FAIL: protocol error "); out_u(arg(1)); out(" on "); out_u(arg(0)); out("\n"); ++failed; return 0; }
    if (ev.object == 21 && ev.opcode == 0) released_21 = 1;
    if (ev.object == object && ev.opcode == opcode) return 1;
  }
}

/* globals */
static u32 g_compositor, g_shm, g_wm, g_seat, g_deco, g_ddm;
/* the descriptor that came last (with the event just read), or -1 */
static int take_fd(void) { return fds_count > 0 ? fds_in[--fds_count] : -1; }
/* reads a pipe until its writer closes it */
static unsigned long read_all(long fd, char* out, unsigned long room) {
  unsigned long n = 0;
  for (;;) {
    const long got = sys3(NR_READ, fd, out + n, room - n);
    if (got <= 0) break;
    n += (unsigned long)got;
    if (n == room) break;
  }
  return n;
}
static int text_is(const char* got, unsigned long n, const char* want) { return n == len(want) && starts(got, want); }
static const char* env_value(char** envp, const char* name) {
  for (; *envp; ++envp) {
    const char* e = *envp;
    const char* n = name;
    while (*n && *e == *n) ++e, ++n;
    if (!*n && *e == '=') return e + 1;
  }
  return 0;
}

static u32* map_pool(long fd, u64 size) {
  const long m = sys6(NR_MMAP, 0, (long)size, 3, 1, fd, 0);      /* PROT_READ | PROT_WRITE, MAP_SHARED */
  return m < 0 && m > -4096 ? 0 : (u32*)m;
}

__attribute__((used)) void start_c(long* stack) {
  const long argc = stack[0];
  char** envp = (char**)(stack + 1 + argc + 1);
  const char* runtime = env_value(envp, "XDG_RUNTIME_DIR");
  const char* display = env_value(envp, "WAYLAND_DISPLAY");
  CHECK(runtime && display && same(runtime, "/run/user/0") && same(display, "wayland-0"));
  u8 address[110] = {0};
  address[0] = 1;      /* AF_UNIX */
  unsigned at = 2;
  for (const char* p = runtime ? runtime : "/run/user/0"; *p; ++p) address[at++] = (u8)*p;
  address[at++] = '/';
  for (const char* p = display ? display : "wayland-0"; *p; ++p) address[at++] = (u8)*p;
  sock = sys3(NR_SOCKET, 1, 1 | 0x80000, 0);      /* SOCK_STREAM | SOCK_CLOEXEC */
  CHECK(sock >= 0 && sys3(NR_CONNECT, sock, address, at + 1) == 0);
  if (failed) goto done;

  /* the registry */
  begin(1, 1); put(2); end();
  begin(1, 0); put(3); end();
  while (next_event() && !(ev.object == 3 && ev.opcode == 0)) {
    if (ev.object != 2 || ev.opcode != 0) continue;
    const char* name = (const char*)ev.args + 8;
    const u32 global = arg(0);
    if (same(name, "wl_compositor")) g_compositor = global;
    else if (same(name, "wl_shm")) g_shm = global;
    else if (same(name, "xdg_wm_base")) g_wm = global;
    else if (same(name, "wl_seat")) g_seat = global;
    else if (same(name, "zxdg_decoration_manager_v1")) g_deco = global;
    else if (same(name, "wl_data_device_manager")) g_ddm = global;
  }
  CHECK(g_compositor && g_shm && g_wm && g_seat && g_deco);
  begin(2, 0); put(g_compositor); put_s("wl_compositor"); put(5); put(10); end();
  begin(2, 0); put(g_shm); put_s("wl_shm"); put(1); put(11); end();
  begin(2, 0); put(g_wm); put_s("xdg_wm_base"); put(5); put(12); end();
  begin(2, 0); put(g_seat); put_s("wl_seat"); put(7); put(13); end();
  begin(2, 0); put(g_deco); put_s("zxdg_decoration_manager_v1"); put(1); put(14); end();
  begin(13, 1); put(15); end();      /* get_keyboard: the keymap comes as a descriptor */
  CHECK(wait_event(15, 0));
  CHECK(arg(0) == 1 && fds_count == 1);
  if (fds_count >= 1) {      /* the keymap: mapped privately, as libxkbcommon does */
    const u32 size = arg(1);
    const long keymap_fd = fds_in[--fds_count];
    const long m = sys6(NR_MMAP, 0, size, 1, 2, keymap_fd, 0);      /* PROT_READ, MAP_PRIVATE */
    CHECK(m > 0 || m < -4096);
    if (m > 0 || m < -4096) {
      CHECK(starts((const char*)m, "xkb_keymap {"));
      CHECK(((const char*)m)[size - 1] == 0);
      sys3(NR_MUNMAP, m, size, 0);
    }
    u8 st[144];
    CHECK(sys3(NR_FSTAT, keymap_fd, st, 0) == 0);
    CHECK(sys6(NR_MMAP, 0, size, 3, 1, keymap_fd, 0) < 0);      /* (sealed against writes: no shared writable mapping) */
    sys3(NR_CLOSE, keymap_fd, 0, 0);
  }

  /* the picture: a file in memory */
  const u64 w = 64, h = 48, pool_size = w * h * 4;
  const long pool_fd = sys3(NR_MEMFD_CREATE, "tcos-pool", 1 | 2, 0);      /* MFD_CLOEXEC | MFD_ALLOW_SEALING */
  CHECK(pool_fd >= 0);
  CHECK(sys3(NR_FTRUNCATE, pool_fd, pool_size, 0) == 0);
  u8 st[144];
  CHECK(sys3(NR_FSTAT, pool_fd, st, 0) == 0);
  u64 st_size;
  copy((u8*)&st_size, st + 48, 8);      /* (st_size is at 48 in both layouts, x86-64's and the generic one) */
  CHECK(st_size == pool_size);
  CHECK(sys6(NR_FCNTL, pool_fd, 1033, 2, 0, 0, 0) == 0);      /* F_ADD_SEALS: F_SEAL_SHRINK */
  CHECK(sys6(NR_FCNTL, pool_fd, 1034, 0, 0, 0, 0) == 2);
  CHECK(sys3(NR_FTRUNCATE, pool_fd, 16, 0) == -1);            /* EPERM: sealed against shrinking */
  u32* px = map_pool(pool_fd, pool_size);
  CHECK(px != 0);
  if (!px) goto done;
  for (u64 i = 0; i < w * h; ++i) px[i] = 0xFF2266CCu;
  /* what was written through the mapping is the file's (read back with pread) */
  u32 back = 0;
  CHECK(sys6(NR_PREAD, pool_fd, (long)&back, 4, 400, 0, 0) == 4 && back == 0xFF2266CCu);
  CHECK(sys3(NR_LSEEK, pool_fd, 0, 2) == (long)pool_size);    /* SEEK_END */

  begin(11, 0); put(20); put((u32)pool_size); pending_fd = (int)pool_fd; end();      /* wl_shm.create_pool(fd) */
  begin(20, 0); put(21); put(0); put((u32)w); put((u32)h); put((u32)(w * 4)); put(1); end();
  /* a decorated toplevel */
  begin(10, 0); put(30); end();
  begin(12, 2); put(31); put(30); end();
  begin(31, 1); put(32); end();
  begin(14, 1); put(33); put(32); end();
  begin(32, 2); put_s("Linux Wayland"); end();
  begin(30, 6); end();
  CHECK(wait_event(31, 0));
  begin(31, 4); put(arg(0)); end();
  begin(30, 1); put(21); put(0); put(0); end();
  begin(30, 2); put(0); put(0); put((u32)w); put((u32)h); end();
  begin(30, 3); put(40); end();
  begin(30, 6); end();
  CHECK(wait_event(40, 0));      /* drawn by the desktop */
  /* drawn again in place: the same memory, no copy */
  for (u64 i = 0; i < w * h; ++i) px[i] = 0xFF00C0DEu;
  begin(30, 2); put(0); put(0); put((u32)w); put((u32)h); end();
  begin(30, 3); put(41); end();
  begin(30, 6); end();
  CHECK(wait_event(41, 0));
  /* the pool grows (fallocate, then wl_shm_pool.resize); the second picture is past the first */
  CHECK(sys6(NR_FALLOCATE, pool_fd, 0, 0, (long)(2 * pool_size), 0, 0) == 0);
  CHECK(sys3(NR_MUNMAP, px, pool_size, 0) == 0);
  px = map_pool(pool_fd, 2 * pool_size);
  CHECK(px != 0);
  if (!px) goto done;
  CHECK(px[0] == 0xFF00C0DEu);      /* (what was there is still there) */
  for (u64 i = 0; i < w * h; ++i) px[w * h + i] = 0xFFAA5500u;
  begin(20, 2); put((u32)(2 * pool_size)); end();
  begin(20, 0); put(22); put((u32)pool_size); put((u32)w); put((u32)h); put((u32)(w * 4)); put(1); end();
  begin(30, 1); put(22); put(0); put(0); end();
  begin(30, 2); put(0); put(0); put((u32)w); put((u32)h); end();
  begin(30, 3); put(42); end();
  begin(30, 6); end();
  CHECK(wait_event(42, 0));
  begin(1, 0); put(43); end();
  CHECK(wait_event(43, 0));
  CHECK(released_21);      /* the first buffer was let go (wl_buffer.release) when the third replaced it */

  /* copy and paste through pipes (pipe2), passed both ways with SCM_RIGHTS: the desktop's text pasted here, a selection of ours read by the desktop's
   * clipboard, and pasted back into ourselves (the compositor passes our own pipe on to our source) */
  {
    CHECK(g_ddm != 0);
    begin(2, 0); put(g_ddm); put_s("wl_data_device_manager"); put(3); put(80); end();
    begin(80, 1); put(81); put(13); end();      /* get_data_device: we have the keyboard, so the selection comes */
    CHECK(wait_event(81, 5));
    const u32 desktop_offer = arg(0);
    CHECK(desktop_offer >= 0xFF000000u);
    int p[2] = {-1, -1};
    char got[256];
    CHECK(sys3(NR_PIPE2, p, 0x80000, 0) == 0 && p[0] >= 0 && p[1] >= 0);
    begin(desktop_offer, 1); put_s("text/plain;charset=utf-8"); pending_fd = p[1]; end();      /* receive */
    sys3(NR_CLOSE, p[1], 0, 0);
    unsigned long n = read_all(p[0], got, sizeof got);
    CHECK(text_is(got, n, "text of the emulated desktop"));
    sys3(NR_CLOSE, p[0], 0, 0);
    /* our selection: the compositor asks for its text at once (for the desktop's clipboard) */
    begin(80, 0); put(82); end();      /* create_data_source */
    begin(82, 0); put_s("text/plain;charset=utf-8"); end();
    begin(81, 1); put(82); put(0); end();      /* set_selection */
    CHECK(wait_event(81, 5));                   /* (first: we have the keyboard, so we are offered our own selection) */
    const u32 own_offer = arg(0);
    CHECK(wait_event(82, 1));                   /* send(mime, fd): the compositor reads it for the desktop's clipboard */
    int to_desktop = take_fd();
    CHECK(to_desktop >= 0);
    if (to_desktop >= 0) {
      const char* text = "copied by the Linux program";
      CHECK(sys3(NR_WRITE, to_desktop, text, len(text)) == (long)len(text));
      sys3(NR_CLOSE, to_desktop, 0, 0);
    }
    /* pasting our own selection: our pipe goes to our source as it is */
    CHECK(own_offer >= 0xFF000000u && own_offer != desktop_offer);
    CHECK(sys3(NR_PIPE2, p, 0x80000, 0) == 0);
    begin(own_offer, 1); put_s("text/plain;charset=utf-8"); pending_fd = p[1]; end();
    sys3(NR_CLOSE, p[1], 0, 0);
    CHECK(wait_event(82, 1));
    const int forwarded = take_fd();
    CHECK(forwarded >= 0);
    if (forwarded >= 0) {
      CHECK(sys3(NR_WRITE, forwarded, "round trip", 10) == 10);
      sys3(NR_CLOSE, forwarded, 0, 0);
    }
    n = read_all(p[0], got, sizeof got);
    CHECK(text_is(got, n, "round trip"));
    sys3(NR_CLOSE, p[0], 0, 0);
    begin(1, 0); put(83); end();
    CHECK(wait_event(83, 0));
  }
  if (argc > 1) sleep_ms(8000);      /* stay on screen a while */
done:
  out("wayland-linux: ");
  out_u(checks);
  out(" checks, ");
  out_u(failed);
  out(" failed\n");
  if (sock >= 0) sys3(NR_CLOSE, sock, 0, 0);
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
  for (;;) {}
}

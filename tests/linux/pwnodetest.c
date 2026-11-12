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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that plays and records through the kernel's PipeWire the way libpipewire's
 * pw_stream does: over $XDG_RUNTIME_DIR/pipewire-0 it makes a client-node, takes the memfds (activation, buffers, io area) and eventfds the server passes
 * with SCM_RIGHTS, maps them with mmap(MAP_SHARED), and runs the cycle: poll() the wake eventfd, read it, fill (or read) a buffer, set the io area and the
 * activation's status, write the done eventfd. First 4 s of a 500 Hz tone (F32 stereo, 48 kHz, 0.1295 on each side: what pulsetest's tone is in the mix),
 * then, on a second connection, an input node records half a second of the emulator's microphone (880 Hz left at 8000, 1320 Hz right at 4000).
 * Prints "pwnodetest: N checks, M failed". Run by tests/pipewire_emulated.sh on the CPU emulator next to a Windows program and a native stream. */

#if defined(__x86_64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_MMAP = 9, NR_SOCKET = 41, NR_CONNECT = 42, NR_SENDTO = 44, NR_RECVMSG = 47, NR_PPOLL = 271, NR_EXIT_GROUP = 231 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e,
                x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_MMAP = 222, NR_SOCKET = 198, NR_CONNECT = 203, NR_SENDTO = 206, NR_RECVMSG = 212, NR_PPOLL = 73, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e,
                a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_MMAP = 222, NR_SOCKET = 198, NR_CONNECT = 203, NR_SENDTO = 206, NR_RECVMSG = 212, NR_PPOLL = 73, NR_EXIT_GROUP = 94 };
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
typedef unsigned long long u64;

static ulong slen(const char* s) { ulong n = 0; while (s[n]) ++n; return n; }
static void out(const char* s) { sys3(NR_WRITE, 1, (long)s, (long)slen(s)); }
static void out_u(ulong v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = (char)('0' + v % 10), v /= 10;
  while (v);
  out(b + i);
}
static int checks, failed;
static void check(int ok, const char* what) {
  ++checks;
  if (!ok) { ++failed; out("FAILED: "); out(what); out("\n"); }
}
static u32 get32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
/* the shared areas are written by the server at the same time: one aligned 32-bit access each */
static u32 shared_load(const u8* p) { return __atomic_load_n((const u32*)(const void*)p, __ATOMIC_ACQUIRE); }
static void shared_store(u8* p, u32 v) { __atomic_store_n((u32*)(void*)p, v, __ATOMIC_RELEASE); }

/* ---- POD (SPA's plain old data): what the native protocol's messages are made of */
static u8 tx[4096];
static ulong tn;
static ulong stack_at[8];
static int depth;
static void w_raw32(u32 v) { put32(tx + tn, v); tn += 4; }
static void w_pad(void) { while (tn & 7) tx[tn++] = 0; }
static void w_int(int v) { w_raw32(4); w_raw32(4); w_raw32((u32)v); w_raw32(0); }
static void w_bool(int v) { w_raw32(4); w_raw32(2); w_raw32(v ? 1 : 0); w_raw32(0); }
static void w_long(u64 v) { w_raw32(8); w_raw32(5); w_raw32((u32)v); w_raw32((u32)(v >> 32)); }
static void w_none(void) { w_raw32(0); w_raw32(1); }
static void w_string(const char* s) {
  const ulong n = slen(s) + 1;
  w_raw32((u32)n); w_raw32(8);
  for (ulong i = 0; i < n; ++i) tx[tn++] = (u8)s[i];
  w_pad();
}
static void w_begin(u32 type) { stack_at[depth++] = tn; w_raw32(0); w_raw32(type); }
static void w_end(void) { const ulong at = stack_at[--depth]; put32(tx + at, (u32)(tn - at - 8)); w_pad(); }
static void w_object(u32 type, u32 id) { w_begin(15); w_raw32(type); w_raw32(id); }
static void w_prop(u32 key) { w_raw32(key); w_raw32(0); }

/* A cursor over a struct's values. */
struct Pod { const u8* p; ulong n, at; };
static int pod_next(struct Pod* c, u32* type, const u8** body, u32* size) {
  if (c->n - c->at < 8) return 0;
  *size = get32(c->p + c->at);
  *type = get32(c->p + c->at + 4);
  if (*size > c->n - c->at - 8) return 0;
  *body = c->p + c->at + 8;
  c->at += 8 + ((*size + 7ul) & ~7ul);
  if (c->at > c->n) c->at = c->n;
  return 1;
}
static int pod_int(struct Pod* c, int* v) {
  u32 t, s; const u8* b;
  if (!pod_next(c, &t, &b, &s) || (t != 4 && t != 3 && t != 2) || s < 4) return 0;      /* (Int, Id and Bool are 32-bit) */
  *v = (int)get32(b);
  return 1;
}
static int pod_fd(struct Pod* c, long* v) {
  u32 t, s; const u8* b;
  if (!pod_next(c, &t, &b, &s) || t != 18 || s < 8) return 0;
  *v = (long)((u64)get32(b) | (u64)get32(b + 4) << 32);
  return 1;
}
/* the message body: a Struct; the cursor is its contents */
static int pod_open(const u8* p, ulong n, struct Pod* c) {
  struct Pod outer = {p, n, 0};
  u32 t, s; const u8* b;
  if (!pod_next(&outer, &t, &b, &s) || t != 14) return 0;
  c->p = b; c->n = s; c->at = 0;
  return 1;
}

/* ---- the connection */
static long fd = -1;
static u32 seq;
static int send_message(u32 id, u32 opcode) {
  u8 h[16];
  put32(h, id); put32(h + 4, opcode << 24 | (u32)tn); put32(h + 8, ++seq); put32(h + 12, 0);
  u8 all[4096 + 16];
  if (tn > sizeof all - 16) return 0;
  memcpy(all, h, 16);
  memcpy(all + 16, tx, tn);
  ulong n = tn + 16, done = 0;
  while (done < n) {
    const long w = sys6(NR_SENDTO, fd, (long)(all + done), (long)(n - done), 0, 0, 0);
    if (w <= 0) return 0;
    done += (ulong)w;
  }
  return 1;
}
static void begin_message(void) { tn = 0; depth = 0; w_begin(14); }

static u8 rx[65536];
static ulong rx_len;
static long fds[64];      /* descriptors received and not yet claimed by a message, in order */
static u32 fd_count;
static int read_more(void) {
  if (rx_len == sizeof rx) return 0;
  u8 control[256];
  u64 iov[2] = {(u64)(ulong)(rx + rx_len), sizeof rx - rx_len};
  u8 msg[56];
  memset(msg, 0, sizeof msg);
  const u64 iov_at = (u64)(ulong)iov, control_at = (u64)(ulong)control, one = 1, clen = sizeof control;
  memcpy(msg + 16, &iov_at, 8); memcpy(msg + 24, &one, 8); memcpy(msg + 32, &control_at, 8); memcpy(msg + 40, &clen, 8);
  const long r = sys3(NR_RECVMSG, fd, (long)msg, 0);
  if (r <= 0) return 0;
  rx_len += (ulong)r;
  u64 got_clen;
  memcpy(&got_clen, msg + 40, 8);
  for (u64 at = 0; at + 16 <= got_clen;) {      /* the cmsghdrs: SCM_RIGHTS (level 1, type 1) */
    u64 len;
    memcpy(&len, control + at, 8);
    if (len < 16 || len > got_clen - at) break;
    if (get32(control + at + 8) == 1 && get32(control + at + 12) == 1)
      for (u64 k = 16; k + 4 <= len && fd_count < 64; k += 4) fds[fd_count++] = (long)(int)get32(control + at + k);
    at += (len + 7) & ~7ull;
  }
  return 1;
}
/* The next whole message (id, opcode, body, and its own descriptors moved to `own`). */
static u32 m_id, m_opcode, m_size;
static long own[16];
static u32 own_count;
static u8 body[65536];
static int read_message(void) {
  while (rx_len < 16 || rx_len - 16 < (get32(rx + 4) & 0xFFFFFF))
    if (!read_more()) return 0;
  m_id = get32(rx);
  m_opcode = get32(rx + 4) >> 24;
  m_size = get32(rx + 4) & 0xFFFFFF;
  const u32 n_fds = get32(rx + 12);
  own_count = 0;
  for (u32 i = 0; i < n_fds && i < 16 && i < fd_count; ++i) own[own_count++] = fds[i];
  const u32 used = n_fds < fd_count ? n_fds : fd_count;
  for (u32 i = used; i < fd_count; ++i) fds[i - used] = fds[i];
  fd_count -= used;
  memcpy(body, rx + 16, m_size);
  for (ulong i = 16 + m_size; i < rx_len; ++i) rx[i - 16 - m_size] = rx[i];
  rx_len -= 16 + m_size;
  return 1;
}

static int connect_pipewire(void) {
  fd = sys3(NR_SOCKET, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0);
  if (fd < 0) return 0;
  u8 address[110] = {1, 0};
  const char* path = "/run/user/0/pipewire-0";
  for (ulong i = 0; path[i]; ++i) address[2 + i] = (u8)path[i];
  if (sys3(NR_CONNECT, fd, (long)address, (long)(2 + slen(path) + 1)) != 0) return 0;
  rx_len = 0; fd_count = 0; seq = 0;
  begin_message(); w_int(4); w_end();      /* Core.hello(version 4) */
  return send_message(0, 1);
}

/* ---- one client-node */
enum { kNode = 3, kStatusTriggered = 1, kStatusAwake = 2, kStatusFinished = 3, kNeedData = 1, kHaveData = 2 };
struct Node {
  u32 direction;      /* 1: output (playback), 0: input (recording) */
  long mem_fd[16];    /* the memfds by mem id */
  long wake, done;
  int activation_mem, buffers_mem, io_mem;
  u32 n_buffers, stride, data_offset, data_size;
  u8 *activation, *buffers, *io;
  int started, errored;
};

static u8* map_shared(long memfd, ulong size) {
  size = (size + 4095) & ~4095ul;
  const long r = sys6(NR_MMAP, 0, (long)size, 3 /* PROT_READ | PROT_WRITE */, 1 /* MAP_SHARED */, memfd, 0);
  return r < 0 && r > -4096 ? 0 : (u8*)r;
}

static void handle(struct Node* n) {
  struct Pod c;
  if (!pod_open(body, m_size, &c)) return;
  int a = 0, b = 0, k = 0;
  long f = -1;
  if (m_id == 0 && m_opcode == 3) {      /* Core.error */
    n->errored = 1;
    out("pwnodetest: the server said error\n");
  }
  if (m_id == 0 && m_opcode == 6) {      /* Core.add_mem: id, type, fd, flags */
    if (pod_int(&c, &a) && pod_int(&c, &b) && pod_fd(&c, &f) && a >= 0 && a < 16 && f >= 0 && f < (long)own_count) n->mem_fd[a] = own[f];
  }
  if (m_id != kNode) return;
  if (m_opcode == 0) {      /* transport: readfd, writefd, mem, offset, size */
    long r = -1, w = -1;
    if (pod_fd(&c, &r) && pod_fd(&c, &w) && pod_int(&c, &a) && r >= 0 && r < (long)own_count && w >= 0 && w < (long)own_count) {
      n->wake = own[r];
      n->done = own[w];
      n->activation_mem = a;
    }
  }
  if (m_opcode == 8) {      /* port_use_buffers: direction, port, mix, flags, n, then each: mem, offset, size, metas, datas, (type, data, flags, mapoffset, maxsize) */
    int count = 0, mem = 0, off = 0, size = 0, metas = 0, datas = 0, type = 0, data = 0, flags = 0, mapoff = 0, maxsize = 0;
    if (pod_int(&c, &a) && pod_int(&c, &b) && pod_int(&c, &k) && pod_int(&c, &flags) && pod_int(&c, &count) && pod_int(&c, &mem) && pod_int(&c, &off) &&
        pod_int(&c, &size) && pod_int(&c, &metas) && pod_int(&c, &datas) && pod_int(&c, &type) && pod_int(&c, &data) && pod_int(&c, &flags) &&
        pod_int(&c, &mapoff) && pod_int(&c, &maxsize) && count > 0 && off == 0 && metas == 0 && datas == 1 && type == 1 /* MemPtr */) {
      n->buffers_mem = mem;
      n->n_buffers = (u32)count;
      n->stride = (u32)size;
      n->data_offset = (u32)data;
      n->data_size = (u32)maxsize;
    }
  }
  if (m_opcode == 9) {      /* port_set_io: direction, port, mix, id, mem, offset, size */
    int io_id = 0, mem = 0;
    if (pod_int(&c, &a) && pod_int(&c, &b) && pod_int(&c, &k) && pod_int(&c, &io_id) && pod_int(&c, &mem) && io_id == 1 /* Buffers */) n->io_mem = mem;
  }
  if (m_opcode == 4) {      /* command: an object of type Command/Node whose id is Start (2) */
    u32 t, s; const u8* o;
    if (pod_next(&c, &t, &o, &s) && t == 15 && s >= 8 && get32(o) == 0x30002 && get32(o + 4) == 2) n->started = 1;
  }
}

static int read_until(struct Node* n, int* flag) {
  for (int i = 0; i < 200 && !*flag && !n->errored; ++i) {
    if (!read_message()) return 0;
    handle(n);
  }
  return *flag;
}

static int make_node(struct Node* n, u32 direction) {
  memset(n, 0, sizeof *n);
  for (int i = 0; i < 16; ++i) n->mem_fd[i] = -1;
  n->wake = n->done = -1;
  n->activation_mem = n->buffers_mem = n->io_mem = -1;
  n->direction = direction;
  /* Core.create_object("client-node", ClientNode, 6, props, new id) */
  begin_message();
  w_string("client-node"); w_string("PipeWire:Interface:ClientNode"); w_int(6);
  w_begin(14);
  w_int(3);
  w_string("media.class"); w_string(direction ? "Stream/Output/Audio" : "Stream/Input/Audio");
  w_string("application.name"); w_string("pwnodetest");
  w_string("media.name"); w_string(direction ? "500 Hz tone" : "microphone");
  w_end();
  w_int(kNode);
  w_end();
  if (!send_message(0, 6)) return 0;
  /* update: the node's info (one port in its direction) */
  begin_message();
  w_int(3); w_int(0);
  w_begin(14);
  w_int(direction ? 0 : 1); w_int(direction ? 1 : 0); w_long(7); w_long(0);
  w_int(1); w_string("node.name"); w_string(direction ? "pwnodetest-play" : "pwnodetest-record");
  w_int(0);
  w_end();
  w_end();
  if (!send_message(kNode, 2)) return 0;
  begin_message(); w_bool(1); w_end();      /* set_active(true) */
  if (!send_message(kNode, 4)) return 0;
  /* the port appears; then the buffers it wants: 3, each 8192 bytes (1024 frames of F32 stereo) */
  begin_message();
  w_int((int)direction); w_int(0); w_int(3); w_int(0); w_none();
  w_end();
  if (!send_message(kNode, 3)) return 0;
  begin_message();
  w_int((int)direction); w_int(0); w_int(3); w_int(1);
  w_object(0x40004, 5);
  w_prop(1); w_int(3);
  w_prop(3); w_int(8192);
  w_end();
  w_none();
  w_end();
  if (!send_message(kNode, 3)) return 0;
  if (!read_until(n, &n->started)) return 0;
  if (n->activation_mem < 0 || n->buffers_mem < 0 || n->io_mem < 0 || n->wake < 0 || n->done < 0) return 0;
  if (n->activation_mem >= 16 || n->buffers_mem >= 16 || n->io_mem >= 16) return 0;
  n->activation = map_shared(n->mem_fd[n->activation_mem], 2312);
  n->buffers = map_shared(n->mem_fd[n->buffers_mem], (ulong)n->n_buffers * n->stride);
  n->io = map_shared(n->mem_fd[n->io_mem], 8);
  return n->activation && n->buffers && n->io && n->n_buffers > 0 && n->data_offset + 8192 <= n->stride;
}

/* Waits for the server's trigger: poll() on the wake eventfd, then read it. 1 when triggered. */
static int wait_trigger(struct Node* n) {
  for (int tries = 0; tries < 100; ++tries) {
    u8 pfd[8];
    put32(pfd, (u32)n->wake); put32(pfd + 4, 1 /* POLLIN */);
    u64 ts[2] = {0, 200000000};      /* 200 ms */
    const long r = sys6(NR_PPOLL, (long)pfd, 1, (long)ts, 0, 8, 0);
    if (r < 0) return 0;
    if (r == 0) continue;
    u64 value = 0;
    if (sys3(NR_READ, n->wake, (long)&value, 8) != 8 || value == 0) return 0;
    if (shared_load(n->activation) == kStatusTriggered) return 1;
  }
  return 0;
}
static void finish_cycle(struct Node* n) {
  shared_store(n->activation, kStatusFinished);
  u64 one = 1;
  sys3(NR_WRITE, n->done, (long)&one, 8);
}

static float sine[96];      /* one period of 500 Hz at 48 kHz, at 0.1295 (pulsetest's 6000 mono, panned to the centre) */
static double tone_amplitude(const float* p, u32 n, u32 step, double coefficient) {
  double s1 = 0, s2 = 0;
  for (u32 i = 0; i < n; ++i) {
    const double s0 = (double)p[i * step] * 32768.0 + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 4.0 * (s1 * s1 + s2 * s2 - coefficient * s1 * s2) / ((double)n * (double)n);      /* the amplitude, squared */
}
static float recorded[48000];      /* 0.5 s of 48 kHz stereo */

void start_c(long* stack) {
  (void)stack;
  {
    double x = 0.12948608398437500, y = 0;      /* rotate (x, y) by 2 pi / 96 each step */
    const double c = 0.9978589232386035, s = 0.06540312923014306;
    for (int i = 0; i < 96; ++i) {
      sine[i] = (float)y;
      const double nx = x * c - y * s, ny = x * s + y * c;
      x = nx;
      y = ny;
    }
  }
  /* ---- playback */
  check(connect_pipewire(), "connect to /run/user/0/pipewire-0");
  struct Node play;
  check(make_node(&play, 1), "a client-node for playback: its memfds and eventfds mapped");
  if (play.activation) {
    check(shared_load(play.activation) == 4 /* INACTIVE */ && get32(play.activation + 544) == 1, "the activation as the server wrote it");
    shared_store(play.activation, kStatusFinished);      /* the node is in our loop: ready */
    u32 cycles = 0, phase = 0, lost = 0;
    while (cycles < 188 && lost < 10) {      /* 188 quanta of 1024 frames: 4 s */
      if (!wait_trigger(&play)) { ++lost; continue; }
      shared_store(play.activation, kStatusAwake);
      if (shared_load(play.io) == kNeedData) {
        const u32 id = cycles % play.n_buffers;
        u8* buffer = play.buffers + (ulong)id * play.stride;
        float* data = (float*)(void*)(buffer + play.data_offset);
        for (u32 i = 0; i < 1024; ++i, ++phase) {
          data[2 * i] = sine[phase % 96];
          data[2 * i + 1] = sine[phase % 96];
        }
        put32(buffer, 0); put32(buffer + 4, 8192); put32(buffer + 8, 8); put32(buffer + 12, 0);      /* the chunk: offset, size, stride, flags */
        shared_store(play.io + 4, id);
        shared_store(play.io, kHaveData);
        ++cycles;
      }
      finish_cycle(&play);
    }
    check(cycles == 188, "4 s of the tone, a quantum each time the server triggered");
  }
  sys3(NR_CLOSE, fd, 0, 0);

  /* ---- recording */
  check(connect_pipewire(), "connect again");
  struct Node rec;
  check(make_node(&rec, 0), "a client-node for recording");
  if (rec.activation) {
    shared_store(rec.activation, kStatusFinished);
    u32 got = 0, lost = 0;
    while (got < sizeof recorded / sizeof recorded[0] && lost < 10) {
      if (!wait_trigger(&rec)) { ++lost; continue; }
      shared_store(rec.activation, kStatusAwake);
      const u32 id = shared_load(rec.io + 4);
      if (shared_load(rec.io) == kHaveData && id < rec.n_buffers) {
        const u8* buffer = rec.buffers + (ulong)id * rec.stride;
        const u32 offset = get32(buffer), size = get32(buffer + 4);
        if (offset <= rec.data_size && size <= rec.data_size - offset) {
          const float* data = (const float*)(const void*)(buffer + rec.data_offset + offset);
          for (u32 i = 0; i < size / 4 && got < sizeof recorded / sizeof recorded[0]; ++i) recorded[got++] = data[i];
        }
        shared_store(rec.io, kNeedData);
      }
      finish_cycle(&rec);
    }
    check(got == sizeof recorded / sizeof recorded[0], "half a second recorded");
    /* 0.1 s of the second half (880 and 1320 Hz both whole periods in it) */
    const u32 frames = 4800;
    const float* p = recorded + 2 * 12000;
    const double left880 = tone_amplitude(p, frames, 2, 1.986745531200793), left1320 = tone_amplitude(p, frames, 2, 1.9702186523095477);
    const double right1320 = tone_amplitude(p + 1, frames, 2, 1.9702186523095477), right880 = tone_amplitude(p + 1, frames, 2, 1.986745531200793);
    check(left880 > 7000.0 * 7000.0 && left880 < 9000.0 * 9000.0, "the left side heard 880 Hz at 8000");
    check(right1320 > 3400.0 * 3400.0 && right1320 < 4600.0 * 4600.0, "the right side heard 1320 Hz at 4000");
    check(left1320 < 500.0 * 500.0 && right880 < 500.0 * 500.0, "each side only its own tone");
  }
  sys3(NR_CLOSE, fd, 0, 0);
  out("pwnodetest: ");
  out_u((ulong)checks);
  out(" checks, ");
  out_u((ulong)failed);
  out(" failed\n");
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
}

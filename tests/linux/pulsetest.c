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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that plays through the kernel's PipeWire the way libpulse does: it
 * connects to $XDG_RUNTIME_DIR/pulse/native (/run/user/0/pulse/native) with socket(2)/connect(2), authenticates, names itself, creates a playback
 * stream (16-bit mono, 32 kHz), sends 4 s of a 500 Hz tone as the server's REQUESTs allow, drains it and lists the sink inputs.
 * Prints "pulsetest: N checks, M failed". Run by tests/pipewire_emulated.sh on the CPU emulator next to a Windows program and a native stream. */

#if defined(__x86_64__)
static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_SOCKET = 41, NR_CONNECT = 42, NR_EXIT_GROUP = 231 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_SOCKET = 198, NR_CONNECT = 203, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys3(long n, long a, long b, long c) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_SOCKET = 198, NR_CONNECT = 203, NR_EXIT_GROUP = 94 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  mv a0, sp\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif

/* (the compiler may call these for initialisers and copies: there is no C library) */
void* memset(void* d, int c, unsigned long n) { unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)c; return d; }
void* memcpy(void* d, const void* s, unsigned long n) { unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s; while (n--) *p++ = *q++; return d; }

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long ulong;

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

static long fd = -1;
static int send_all(const u8* p, ulong n) {
  while (n) {
    const long w = sys3(NR_WRITE, fd, (long)p, (long)n);
    if (w <= 0) return 0;
    p += w;
    n -= (ulong)w;
  }
  return 1;
}
static int recv_all(u8* p, ulong n) {
  while (n) {
    const long r = sys3(NR_READ, fd, (long)p, (long)n);
    if (r <= 0) return 0;
    p += r;
    n -= (ulong)r;
  }
  return 1;
}

/* ---- tagstruct ---- */
static u8 tx[2048];
static ulong tn;
static void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 get_be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void t_u32(u32 v) { tx[tn++] = 'L'; put_be32(tx + tn, v); tn += 4; }
static void t_u8(u8 v) { tx[tn++] = 'B'; tx[tn++] = v; }
static void t_bool(int v) { tx[tn++] = v ? '1' : '0'; }
static void t_str(const char* s) { if (!s) { tx[tn++] = 'N'; return; } tx[tn++] = 't'; for (ulong i = 0; s[i]; ++i) tx[tn++] = (u8)s[i]; tx[tn++] = 0; }
static void t_arb(const u8* d, u32 n) { tx[tn++] = 'x'; put_be32(tx + tn, n); tn += 4; for (u32 i = 0; i < n; ++i) tx[tn++] = d ? d[i] : 0; }
static void t_prop(const char* key, const char* value) { const u32 n = (u32)slen(value) + 1; t_str(key); t_u32(n); t_arb((const u8*)value, n); }
static u32 next_tag = 1;
static u32 begin(u32 command) { tn = 0; t_u32(command); t_u32(next_tag); return next_tag++; }
static int send_packet(u32 channel, const u8* payload, u32 n) {
  u8 d[20];
  put_be32(d, n); put_be32(d + 4, channel); put_be32(d + 8, 0); put_be32(d + 12, 0); put_be32(d + 16, 0);
  return send_all(d, 20) && send_all(payload, n);
}
static int send_command(void) { return send_packet(0xFFFFFFFFu, tx, (u32)tn); }

/* ---- packets in ---- */
static u8 rx[65536];
static u32 rx_len, rx_channel;
static int read_packet(void) {
  u8 d[20];
  if (!recv_all(d, 20)) return 0;
  rx_len = get_be32(d);
  rx_channel = get_be32(d + 4);
  if (rx_len > sizeof rx) return 0;
  return recv_all(rx, rx_len);
}
/* a command packet: its command, tag and where its arguments start */
static int command_of(u32* cmd, u32* tag) {
  if (rx_channel != 0xFFFFFFFFu || rx_len < 10 || rx[0] != 'L' || rx[5] != 'L') return 0;
  *cmd = get_be32(rx + 1);
  *tag = get_be32(rx + 6);
  return 1;
}
static u32 requested;      /* bytes the server let us send */
static u32 stream_channel;
/* Waits for the reply to `tag`; REQUESTs that come meanwhile are counted. Returns the command of the reply (2 reply, 0 error) or -1. */
static long wait_reply(u32 tag) {
  for (;;) {
    u32 cmd, t;
    if (!read_packet()) return -1;
    if (!command_of(&cmd, &t)) continue;
    if (cmd == 61 && rx_len >= 20 && get_be32(rx + 11) == stream_channel) requested += get_be32(rx + 16);      /* REQUEST: channel, bytes */
    if (t == tag && (cmd == 2 || cmd == 0)) return (long)cmd;
  }
}

static const short kSine[64] = {0, 588, 1171, 1742, 2296, 2828, 3333, 3806, 4243, 4638, 4989, 5292, 5543, 5742, 5885, 5971, 6000, 5971, 5885, 5742, 5543, 5292, 4989, 4638, 4243, 3806, 3333, 2828, 2296, 1742, 1171, 588, 0, -588, -1171, -1742, -2296, -2828, -3333, -3806, -4243, -4638, -4989, -5292, -5543, -5742, -5885, -5971, -6000, -5971, -5885, -5742, -5543, -5292, -4989, -4638, -4243, -3806, -3333, -2828, -2296, -1742, -1171, -588};
static u8 audio[8192];

void start_c(long* stack) {
  (void)stack;
  fd = sys3(NR_SOCKET, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0);
  check(fd >= 0, "socket");
  u8 address[110] = {1, 0};
  const char* path = "/run/user/0/pulse/native";
  for (ulong i = 0; path[i]; ++i) address[2 + i] = (u8)path[i];
  check(sys3(NR_CONNECT, fd, (long)address, (long)(2 + slen(path) + 1)) == 0, "connect to /run/user/0/pulse/native");

  u32 tag = begin(8);      /* AUTH: version 35 (with the SHM flag: the server must say no), a cookie */
  t_u32(35u | 0x80000000u);
  t_arb(0, 256);
  check(send_command() && wait_reply(tag) == 2, "AUTH");
  check(rx_len >= 15 && get_be32(rx + 11) == 35, "the server speaks version 35 without shared memory");

  tag = begin(9);          /* SET_CLIENT_NAME */
  tx[tn++] = 'P';
  t_prop("application.name", "pulsetest");
  t_prop("application.process.binary", "pulsetest");
  tx[tn++] = 'N';
  check(send_command() && wait_reply(tag) == 2, "SET_CLIENT_NAME");

  tag = begin(3);          /* CREATE_PLAYBACK_STREAM: s16le, 1 channel, 32 kHz, tlength 100 ms */
  tx[tn++] = 'a'; tx[tn++] = 3; tx[tn++] = 1; put_be32(tx + tn, 32000); tn += 4;
  tx[tn++] = 'm'; tx[tn++] = 1; tx[tn++] = 0;
  t_u32(0xFFFFFFFFu); t_str(0); t_u32(0xFFFFFFFFu); t_bool(0); t_u32(6400); t_u32(0xFFFFFFFFu); t_u32(0xFFFFFFFFu); t_u32(0);
  tx[tn++] = 'v'; tx[tn++] = 1; put_be32(tx + tn, 0x10000); tn += 4;
  for (int i = 0; i < 7; ++i) t_bool(0);
  t_bool(0); t_bool(0);
  tx[tn++] = 'P'; t_prop("media.name", "500 Hz tone"); tx[tn++] = 'N';
  t_bool(1); t_bool(0); t_bool(0); t_bool(0); t_bool(0); t_bool(0); t_bool(0);
  t_u8(0);
  check(send_command() && wait_reply(tag) == 2, "CREATE_PLAYBACK_STREAM");
  /* the reply: channel, sink input, missing, ... */
  check(rx_len >= 25 && rx[10] == 'L' && rx[15] == 'L' && rx[20] == 'L', "the stream's channel, sink input and request");
  stream_channel = get_be32(rx + 11);
  requested = get_be32(rx + 21);
  check(requested == 6400, "the first request is the target length");

  /* 4 s at 32 kHz, mono 16-bit: 256000 bytes, as the server asks for them */
  const u32 total = 256000;
  u32 sent = 0, phase = 0, stalls = 0;
  while (sent < total && stalls < 100000) {
    if (requested == 0) {
      u32 cmd, t;
      if (!read_packet()) break;
      if (command_of(&cmd, &t) && cmd == 61 && rx_len >= 20 && get_be32(rx + 11) == stream_channel) requested += get_be32(rx + 16);
      ++stalls;
      continue;
    }
    u32 n = requested < sizeof audio ? requested : sizeof audio;
    if (n > total - sent) n = total - sent;
    n &= ~1u;
    for (u32 i = 0; i < n; i += 2, ++phase) {
      const short v = kSine[phase & 63];      /* 64 samples a period at 32 kHz: 500 Hz */
      audio[i] = (u8)v;
      audio[i + 1] = (u8)((unsigned short)v >> 8);
    }
    if (!send_packet(stream_channel, audio, n)) break;
    sent += n;
    requested -= n;
  }
  check(sent == total, "the whole tone went as the server asked for it");

  tag = begin(30);         /* GET_SINK_INPUT_INFO_LIST, while it plays */
  check(send_command() && wait_reply(tag) == 2 && rx_len > 40, "GET_SINK_INPUT_INFO_LIST");
  int saw_self = 0;
  for (u32 i = 0; i + 9 < rx_len; ++i) {
    const char* want = "pulsetest";
    u32 k = 0;
    while (want[k] && rx[i + k] == (u8)want[k]) ++k;
    if (!want[k]) saw_self = 1;
  }
  check(saw_self, "this program is one of the sink inputs");

  tag = begin(12);         /* DRAIN */
  t_u32(stream_channel);
  check(send_command() && wait_reply(tag) == 2, "DRAIN (answered once it has played)");
  tag = begin(4);          /* DELETE_PLAYBACK_STREAM */
  t_u32(stream_channel);
  check(send_command() && wait_reply(tag) == 2, "DELETE_PLAYBACK_STREAM");
  sys3(NR_CLOSE, fd, 0, 0);
  out("pulsetest: ");
  out_u((ulong)checks);
  out(" checks, ");
  out_u((ulong)failed);
  out(" failed\n");
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
}

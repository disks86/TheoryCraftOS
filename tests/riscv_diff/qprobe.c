// Runs each instruction word read from fd 0 (a 32-bit word, or a 16-bit one when its low two bits are not 11; each is sent as 4 bytes) and writes one byte per word to fd 1:
// 0 executed, 1 SIGILL, 2 another signal (a fault, so the instruction was accepted), 3 hung.
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned char u8; typedef unsigned short u16;
static long sys(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n; register long a0 __asm__("a0") = a; register long a1 __asm__("a1") = b; register long a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d; register long a4 __asm__("a4") = e; register long a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
static volatile int last_signal;
static volatile long progress, seen_progress;
extern void call_word(void* buf);
extern char cont[];
static void handler(int sig, void* info, void* uc) {
  (void)info;
  u64* pc = (u64*)((char*)uc + 176);
  if (sig == 14) { if (progress == seen_progress) { last_signal = 99; *pc = (u64)cont; } seen_progress = progress; return; }
  last_signal = sig;
  *pc = (u64)cont;
}
struct ksigaction { void* handler; u64 flags; u64 mask; };
static u32 words[1 << 12];
static u8 results[1 << 12];
void c_main(void) {
  static char altstack[65536];
  struct { void* sp; int flags; long size; } ss = {altstack, 0, sizeof(altstack)};
  sys(132, (long)&ss, 0, 0, 0, 0, 0);
  struct ksigaction sa = {(void*)handler, 0x4 | 0x40000000 | 0x08000000, 0};
  static const int sigs[] = {4, 5, 6, 7, 8, 11, 14, 31};
  for (unsigned k = 0; k < sizeof(sigs) / sizeof(sigs[0]); ++k) sys(134, sigs[k], (long)&sa, 0, 8, 0, 0);
  u32* buf = (u32*)sys(222, 0, 1 << 18, 7, 0x22, -1, 0);
  struct { long isec, iusec, vsec, vusec; } timer = {0, 100000, 0, 100000};
  sys(103, 0, (long)&timer, 0, 0, 0, 0);
  for (;;) {
    long got = 0;
    while (got < (long)sizeof(words)) { long r = sys(63, 0, (long)words + got, sizeof(words) - got, 0, 0, 0); if (r <= 0) break; got += r; }
    long n = got / 4;
    if (n == 0) break;
    for (long i = 0; i < n; ++i) {
      u32* slot = buf + 4 * i;
      if ((words[i] & 3) == 3) { slot[0] = words[i]; slot[1] = 0x00100073; }      // word, ebreak
      else { u16* h = (u16*)slot; h[0] = (u16)words[i]; h[1] = 0x9002; slot[1] = 0x00100073; }      // halfword, c.ebreak
    }
    __asm__ volatile("fence.i" ::: "memory");
    for (long i = 0; i < n; ++i) {
      last_signal = 0;
      call_word(buf + 4 * i);
      results[i] = last_signal == 0 || last_signal == 5 ? 0 : last_signal == 4 ? 1 : last_signal == 99 ? 3 : 2;
      ++progress;
      sys(64, 1, (long)&results[i], 1, 0, 0, 0);
    }
    if (got < (long)sizeof(words)) break;
  }
  sys(93, 0, 0, 0, 0, 0, 0);
}

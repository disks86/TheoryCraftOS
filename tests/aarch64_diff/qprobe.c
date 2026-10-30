// Runs each 32-bit word from the file in argv-less fashion: reads words from fd 0, writes one byte per word to fd 1: 0 executed, 1 SIGILL, 2 other signal.
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned char u8;
static long sys(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n; register long x0 __asm__("x0") = a; register long x1 __asm__("x1") = b; register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d; register long x4 __asm__("x4") = e; register long x5 __asm__("x5") = f;
  __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
static volatile int last_signal;
static volatile long progress, seen_progress;
extern void call_word(void* buf);
extern char cont[];
static void handler(int sig, void* info, void* uc) {
  (void)info;
  u64* pc = (u64*)((char*)uc + 440);
  if (sig == 14) {      // the watchdog: no progress for a whole tick means the word hangs
    if (progress == seen_progress) { last_signal = 99; *pc = (u64)cont; }
    seen_progress = progress;
    return;
  }
  last_signal = sig;
  *pc += 4;
}
struct ksigaction { void* handler; u64 flags; void* restorer; u64 mask; };
static u32 words[1 << 12];
static u8 results[1 << 12];
void c_main(void) {
  static char altstack[65536];
  struct { void* sp; int flags; long size; } ss = {altstack, 0, sizeof(altstack)};
  sys(132, (long)&ss, 0, 0, 0, 0, 0);
  struct ksigaction sa = {(void*)handler, 0x4 /*SA_SIGINFO*/ | 0x40000000 /*SA_NODEFER*/ | 0x08000000 /*SA_ONSTACK*/, 0, 0};
  static const int sigs[] = {4, 5, 6, 7, 8, 11, 14, 31};
  for (unsigned k = 0; k < sizeof(sigs) / sizeof(sigs[0]); ++k) sys(134, sigs[k], (long)&sa, 0, 8, 0, 0);
  u32* buf = (u32*)sys(222, 0, 1 << 18, 7, 0x22, -1, 0);
  struct { long isec, iusec, vsec, vusec; } timer = {0, 100000, 0, 100000};
  sys(103, 0, (long)&timer, 0, 0, 0, 0);
  for (;;) {
    long got = 0;
    while (got < (long)sizeof(words)) {
      long r = sys(63, 0, (long)words + got, sizeof(words) - got, 0, 0, 0);
      if (r <= 0) break;
      got += r;
    }
    long n = got / 4;
    if (n == 0) break;
    for (long i = 0; i < n; ++i) {
      u32* slot = buf + 8 * i;      // 32 bytes: the word, ldr x17, br x17, nop, the continuation address
      slot[0] = words[i];
      slot[1] = 0x58000071;      // ldr x17, #12 (the continuation address, 16 bytes into the slot)
      slot[2] = 0xd61f0220;      // br x17
      slot[3] = 0xd503201f;
      *(u64*)(slot + 4) = (u64)cont;
    }
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" : : "r"(buf) : "memory");
    for (long i = 0; i < n; ++i) {
      last_signal = 0;
      call_word(buf + 8 * i);
      results[i] = last_signal == 0 ? 0 : last_signal == 4 ? 1 : last_signal == 99 ? 3 : 2;
      ++progress;
      sys(64, 1, (long)&results[i], 1, 0, 0, 0);
    }
    if (got < (long)sizeof(words)) break;
  }
  sys(94, 0, 0, 0, 0, 0, 0);
}

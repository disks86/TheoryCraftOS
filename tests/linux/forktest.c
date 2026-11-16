/* A freestanding Linux x86-64 program for fork(), wait, signals and the memory calls: no libc, raw system calls. Every line it prints is the same on a
   correct kernel, so the program run on the build machine's Linux is the expected output for the same program run on this OS (tests/LinuxForkTest.cpp
   under the emulator, tests/fork_qemu.sh on the real kernel). It prints nothing that differs between runs (no ids, no times). */
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;

static inline __attribute__((always_inline)) long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
#define S0(n) sys6(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define S4(n, a, b, c, d) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define S5(n, a, b, c, d, e) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define S6(n, a, b, c, d, e, f) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

enum { SYS_read = 0, SYS_write = 1, SYS_close = 3, SYS_mmap = 9, SYS_munmap = 11, SYS_rt_sigaction = 13, SYS_rt_sigprocmask = 14, SYS_mremap = 25, SYS_mincore = 27,
       SYS_madvise = 28, SYS_pause = 34, SYS_nanosleep = 35, SYS_setitimer = 38, SYS_getpid = 39, SYS_clone = 56, SYS_fork = 57, SYS_vfork = 58, SYS_exit = 60,
       SYS_wait4 = 61, SYS_kill = 62, SYS_getppid = 110, SYS_setsid = 112, SYS_getpgid = 121, SYS_getsid = 124, SYS_rt_sigpending = 127, SYS_rt_sigtimedwait = 128,
       SYS_prctl = 157, SYS_gettid = 186, SYS_tgkill = 234, SYS_exit_group = 231, SYS_waitid = 247, SYS_pipe2 = 293, SYS_clone3 = 435 };

static u64 length(const char* s) { u64 n = 0; while (s[n]) ++n; return n; }
static void put(const char* s) { S3(SYS_write, 1, s, length(s)); }
static void put_number(i64 v) {
  char text[24];
  int n = 0;
  u64 u = v < 0 ? (u64)(-v) : (u64)v;
  if (v < 0) put("-");
  do { text[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  while (n) S3(SYS_write, 1, &text[--n], 1);
}
static void report(const char* what, i64 value) { put(what); put(": "); put_number(value); put("\n"); }
static void leave(int code) { S1(SYS_exit_group, code); for (;;) {} }
static void sleep_ms(long ms) { long ts[2] = {ms / 1000, (ms % 1000) * 1000000}; S2(SYS_nanosleep, ts, 0); }

/* wait4 that tells how the child ended: its exit code, or 1000 + the signal */
static i64 wait_how(long pid, long options, long* got) {
  int status = -1;
  long r = S4(SYS_wait4, pid, &status, options, 0);
  if (got) *got = r;
  if (r <= 0) return r;
  if ((status & 0x7F) == 0) return (status >> 8) & 0xFF;
  return 1000 + (status & 0x7F);
}

static volatile long shared_word = 5;
static char child_stack[65536];

__attribute__((noinline, used)) int vfork_child(void) { leave(33); return 0; }

void start_c(void) {
  const long self = S0(SYS_getpid);

  /* --- fork: the child is a copy, with its own memory and the parent's descriptors */
  int fds[2];
  report("pipe", S2(SYS_pipe2, fds, 0));
  long pid = S0(SYS_fork);
  if (pid == 0) {
    shared_word = 99;                                   /* the child's own copy */
    const int same_parent = S0(SYS_getppid) == self;
    const int own_id = S0(SYS_getpid) != self && S0(SYS_gettid) == S0(SYS_getpid);
    S3(SYS_write, fds[1], same_parent ? "P" : "p", 1);
    S3(SYS_write, fds[1], own_id ? "I" : "i", 1);
    S3(SYS_write, fds[1], "hello", 5);
    leave(7);
  }
  report("fork returned a child", pid > 0 && pid != self);
  char buffer[16] = {0};
  long got = 0;
  while (got < 7) {
    long n = S3(SYS_read, fds[0], buffer + got, 7 - got);
    if (n <= 0) break;
    got += n;
  }
  put("child said: "); put(buffer); put("\n");
  long waited = 0;
  report("child ended with", wait_how(pid, 0, &waited));
  report("wait returned the child", waited == pid);
  report("parent's memory", shared_word);
  report("no children left", wait_how(-1, 0, 0));       /* -ECHILD */

  /* --- shared anonymous memory stays shared with the child; private memory does not */
  volatile long* both = (volatile long*)S6(SYS_mmap, 0, 4096, 3, 0x21, -1, 0);       /* MAP_SHARED | MAP_ANONYMOUS */
  volatile long* mine = (volatile long*)S6(SYS_mmap, 0, 4096, 3, 0x22, -1, 0);       /* MAP_PRIVATE | MAP_ANONYMOUS */
  both[0] = 1; mine[0] = 1;
  pid = S0(SYS_fork);
  if (pid == 0) { both[0] += 41; mine[0] += 41; leave(both[1] == 0 ? 0 : 1); }
  report("child of the shared page", wait_how(pid, 0, 0));
  report("shared page after the child wrote", both[0]);
  report("private page after the child wrote", mine[0]);

  /* --- two children, waited for in any order; WNOHANG while they sleep */
  long a = S0(SYS_fork);
  if (a == 0) { sleep_ms(150); leave(1); }
  long b = S0(SYS_fork);
  if (b == 0) { sleep_ms(150); leave(2); }
  report("nothing ended yet", wait_how(-1, 1, 0));
  i64 sum = wait_how(-1, 0, 0);
  sum += wait_how(-1, 0, 0);
  report("both ended, codes sum", sum);

  /* --- a child of a child */
  pid = S0(SYS_fork);
  if (pid == 0) {
    long inner = S0(SYS_fork);
    if (inner == 0) leave(20);
    leave((int)wait_how(inner, 0, 0) + 1);
  }
  report("grandchild's code plus one", wait_how(pid, 0, 0));

  /* --- kill: a child that sleeps is ended by SIGTERM; one that ends itself with SIGKILL; one a signal nobody handles ends */
  pid = S0(SYS_fork);
  if (pid == 0) { for (;;) sleep_ms(50); }
  report("kill(child, 0)", S2(SYS_kill, pid, 0));
  report("kill(child, SIGTERM)", S2(SYS_kill, pid, 15));
  report("ended by", wait_how(pid, 0, 0));
  report("kill of a child that is gone", S2(SYS_kill, pid, 0));
  pid = S0(SYS_fork);
  if (pid == 0) { S2(SYS_kill, S0(SYS_getpid), 9); leave(1); }
  report("ended itself by", wait_how(pid, 0, 0));
  pid = S0(SYS_fork);
  if (pid == 0) { S3(SYS_tgkill, S0(SYS_getpid), S0(SYS_gettid), 12); leave(1); }
  report("SIGUSR2 with the default action", wait_how(pid, 0, 0));
  pid = S0(SYS_fork);
  if (pid == 0) {
    long ignore[4] = {1, 0, 0, 0};                      /* SIG_IGN */
    S4(SYS_rt_sigaction, 12, ignore, 0, 8);
    S2(SYS_kill, S0(SYS_getpid), 12);
    leave(44);
  }
  report("SIGUSR2 ignored", wait_how(pid, 0, 0));

  /* --- what posix_spawn does: clone(CLONE_VM | CLONE_VFORK | SIGCHLD) with a stack for the child; and vfork(), and clone3 as a fork; waitid */
  pid = S0(SYS_vfork);
  if (pid == 0) leave(31);
  report("vfork child", wait_how(pid, 0, 0));
  {
    long args[11] = {0};
    args[4] = 17;                                       /* exit_signal = SIGCHLD */
    pid = S2(SYS_clone3, args, 88);
    if (pid == 0) leave(32);
    int info[32] = {0};
    report("waitid", S5(SYS_waitid, 1, pid, info, 4, 0));
    report("  si_signo", info[0]);
    report("  si_code", info[2]);
    report("  si_pid is the child", info[4] == pid);
    report("  si_status", info[6]);
  }
  {
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    long r;
    /* (the child must not return from this function on the new stack: it calls on) */
    __asm__ volatile("syscall\n test %%rax, %%rax\n jnz 1f\n call vfork_child\n 1:" : "=a"(r) : "a"((long)SYS_clone), "D"(0x4111L), "S"(child_stack + sizeof(child_stack) - 64), "d"(0L), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    report("clone with a stack", wait_how(r, 0, 0));
  }

  /* --- signals kept by the kernel: a blocked one waits and is taken with sigtimedwait; actions read back */
  u64 mask = 1ul << (10 - 1), old = 0, pending = 0;
  report("block SIGUSR1", S4(SYS_rt_sigprocmask, 0, &mask, &old, 8));
  report("  it was not blocked", (old & mask) == 0);
  report("raise it", S2(SYS_kill, self, 10));
  report("sigpending", S2(SYS_rt_sigpending, &pending, 8));
  report("  SIGUSR1 pending", (pending & mask) != 0);
  int info[32] = {0};
  long zero_time[2] = {0, 0};
  report("sigtimedwait", S4(SYS_rt_sigtimedwait, &mask, info, zero_time, 8));
  report("  si_signo", info[0]);
  report("nothing more", S4(SYS_rt_sigtimedwait, &mask, info, zero_time, 8));      /* -EAGAIN */
  long action[4] = {0x1234000, 0x04000000, 0x5678000, 1ul << 1}, read_back[4] = {0};  /* handler, SA_RESTORER, restorer, mask */
  report("sigaction", S4(SYS_rt_sigaction, 10, action, 0, 8));
  report("  read back", S4(SYS_rt_sigaction, 10, 0, read_back, 8));
  report("  same", read_back[0] == action[0] && read_back[1] == action[1] && read_back[2] == action[2] && read_back[3] == action[3]);
  report("SIGKILL cannot be caught", S4(SYS_rt_sigaction, 9, action, 0, 8));
  long dfl[4] = {0, 0, 0, 0};
  S4(SYS_rt_sigaction, 10, dfl, 0, 8);

  /* --- a timer: SIGALRM blocked, an interval timer of 20 ms, waited for */
  u64 alarm_mask = 1ul << (14 - 1);
  S4(SYS_rt_sigprocmask, 0, &alarm_mask, 0, 8);
  long timer[4] = {0, 0, 0, 20000};
  report("setitimer", S3(SYS_setitimer, 0, timer, 0));
  long two_seconds[2] = {2, 0};
  report("SIGALRM arrived", S4(SYS_rt_sigtimedwait, &alarm_mask, info, two_seconds, 8));

  /* --- the child keeps the parent's handlers and mask */
  pid = S0(SYS_fork);
  if (pid == 0) {
    u64 now = 0;
    S4(SYS_rt_sigprocmask, 0, 0, &now, 8);
    leave((now & mask) && (now & alarm_mask) ? 50 : 51);
  }
  report("child's mask", wait_how(pid, 0, 0));

  /* --- sessions and names */
  pid = S0(SYS_fork);
  if (pid == 0) {
    const long me = S0(SYS_getpid);
    const int ok = S0(SYS_setsid) == me && S1(SYS_getsid, 0) == me && S1(SYS_getpgid, 0) == me;
    leave(ok ? 60 : 61);
  }
  report("setsid in a child", wait_how(pid, 0, 0));
  char name[16] = {0};
  report("PR_SET_NAME", S2(SYS_prctl, 15, "forktest-name"));
  report("PR_GET_NAME", S2(SYS_prctl, 16, name));
  put("  name: "); put(name); put("\n");

  /* --- memory: mremap keeps the bytes, MADV_DONTNEED gives zeros, mincore */
  u8* m = (u8*)S6(SYS_mmap, 0, 8192, 3, 0x22, -1, 0);
  m[0] = 11; m[4096] = 22; m[8191] = 33;
  u8* grown = (u8*)S5(SYS_mremap, m, 8192, 5 * 4096, 1, 0);
  report("mremap grew", (long)grown > 0 && grown[0] == 11 && grown[4096] == 22 && grown[8191] == 33 && grown[8192] == 0 && grown[5 * 4096 - 1] == 0);
  grown[5 * 4096 - 1] = 44;
  u8* shrunk = (u8*)S5(SYS_mremap, grown, 5 * 4096, 4096, 1, 0);
  report("mremap shrank in place", shrunk == grown && shrunk[0] == 11);
  shrunk[100] = 55;
  report("madvise", S3(SYS_madvise, shrunk, 4096, 4));
  report("  zeros after MADV_DONTNEED", shrunk[0] == 0 && shrunk[100] == 0);
  u8 vec[4] = {9, 9, 9, 9};
  shrunk[0] = 1;
  report("mincore", S3(SYS_mincore, shrunk, 4096, vec));
  report("  in core", vec[0] & 1);
  report("munmap", S2(SYS_munmap, shrunk, 4096));
  put("fork ok\n");
  leave(0);
}

__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  and $-16, %rsp\n  call start_c\n");

/* A freestanding Linux program for signal handlers, for x86-64, AArch64 and RISC-V: no libc, raw system calls, on x86-64 its own restorer. Handlers entered after a system call, in the middle
   of a loop (a timer), on a processor fault (with the handler changing where the program goes on), from another process; the mask while a handler runs and
   after it; sigsuspend; calls that are cut short (EINTR) or started again (SA_RESTART); the alternate stack. Every line it prints is the same on a correct
   kernel, so the program run on the build machine's Linux is the expected output for the same program run on this OS (tests/LinuxForkTest.cpp under the
   emulator, tests/fork_qemu.sh on the real kernel). The AArch64 and RISC-V builds (tests/linux/build_signaltest.sh makes the checked-in
   tests/data/emulation/signals-*.elf) are run under qemu's user mode for what they should print. */
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;

#if defined(__x86_64__)
static inline __attribute__((always_inline)) long sys6(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
#elif defined(__loongarch__)
static inline __attribute__((always_inline)) long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n;
  register long a0 __asm__("a0") = a;
  register long a1 __asm__("a1") = b;
  register long a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d;
  register long a4 __asm__("a4") = e;
  register long a5 __asm__("a5") = f;
  __asm__ volatile("syscall 0" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "memory");
  return a0;
}
#elif defined(__riscv)
static inline __attribute__((always_inline)) long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n;
  register long a0 __asm__("a0") = a;
  register long a1 __asm__("a1") = b;
  register long a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d;
  register long a4 __asm__("a4") = e;
  register long a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
#else
static inline __attribute__((always_inline)) long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
#endif
#define S0(n) sys6(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define S4(n, a, b, c, d) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define S6(n, a, b, c, d, e, f) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

#if defined(__x86_64__)
enum { SYS_read = 0, SYS_write = 1, SYS_rt_sigaction = 13, SYS_rt_sigprocmask = 14, SYS_nanosleep = 35, SYS_setitimer = 38, SYS_getpid = 39, SYS_clone = 56,
       SYS_wait4 = 61, SYS_kill = 62, SYS_getppid = 110, SYS_rt_sigsuspend = 130, SYS_sigaltstack = 131, SYS_gettid = 186, SYS_exit_group = 231, SYS_tgkill = 234,
       SYS_pipe2 = 293 };
#else      /* the table AArch64, RISC-V and LoongArch share */
enum { SYS_read = 63, SYS_write = 64, SYS_rt_sigaction = 134, SYS_rt_sigprocmask = 135, SYS_nanosleep = 101, SYS_setitimer = 103, SYS_getpid = 172, SYS_clone = 220,
       SYS_wait4 = 260, SYS_kill = 129, SYS_getppid = 173, SYS_rt_sigsuspend = 133, SYS_sigaltstack = 132, SYS_gettid = 178, SYS_exit_group = 94, SYS_tgkill = 131,
       SYS_pipe2 = 59 };
#endif
enum { SIGILL = 4, SIGFPE = 8, SIGUSR1 = 10, SIGSEGV = 11, SIGUSR2 = 12, SIGALRM = 14, SIGTERM = 15, SIGCHLD = 17 };
#define SA_SIGINFO 4ul
#define SA_ONSTACK 0x08000000ul
#define SA_RESTART 0x10000000ul
#define SA_NODEFER 0x40000000ul
#define SA_RESETHAND 0x80000000ul
#define SA_RESTORER 0x04000000ul
#define BIT(signal) (1ul << ((signal) - 1))

/* what -ffreestanding code may still call */
void* memcpy(void* d, const void* s, u64 n) { u8* x = d; const u8* y = s; while (n--) { *x++ = *y++; __asm__ volatile("" ::: "memory"); } return d; }
void* memset(void* d, int c, u64 n) { u8* x = d; while (n--) { *x++ = (u8)c; __asm__ volatile("" ::: "memory"); } return d; }

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
static long do_fork(void) { return sys6(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0); }
static long do_pause(void) {      /* (the newer CPUs' table has no pause: sigsuspend with the mask as it is) */
  u64 now = 0;
  S4(SYS_rt_sigprocmask, 0, 0, &now, 8);
  return S2(SYS_rt_sigsuspend, &now, 8);
}
static void sleep_ms(long ms) { long ts[2] = {ms / 1000, (ms % 1000) * 1000000}; S2(SYS_nanosleep, ts, 0); }
static i64 wait_how(long pid) {
  int status = -1;
  long r;
  do { r = S4(SYS_wait4, pid, &status, 0, 0); } while (r == -4);
  if (r <= 0) return r;
  return (status & 0x7F) == 0 ? (status >> 8) & 0xFF : 1000 + (status & 0x7F);
}

/* instructions that fault at a known place and, on x86-64, what a handler returns through; and where struct ucontext keeps what the handlers look at */
long fault_read(long* address);
void fault_read_resume(void);
void fault_opcode(void);
#if defined(__x86_64__)
void sig_restorer(void);
long fault_divide(long by);
void fault_divide_resume(void);
__asm__(".text\n"
        "sig_restorer:\n  mov $15, %eax\n  syscall\n"
        "fault_read:\n  mov (%rdi), %rax\n fault_read_resume:\n  ret\n"
        "fault_divide:\n  mov $100, %eax\n  xor %edx, %edx\n  div %edi\n fault_divide_resume:\n  ret\n"
        "fault_opcode:\n  ud2\n  ret\n");
/* the registers start at +40: r8 r9 r10 r11 r12 r13 r14 r15 rdi rsi rbp rbx rdx rax rcx rsp rip eflags */
enum { UC_RESULT = 40 + 13 * 8, UC_SP = 40 + 15 * 8, UC_PC = 40 + 16 * 8, UC_TRAPNO = 40 + 160, UC_MASK = 296, UC_STACK_FLAGS = 24, ILLEGAL_BYTES = 2, HAS_DIVIDE_FAULT = 1 };
#elif defined(__loongarch__)
__asm__(".text\n"
        "fault_read:\n  ld.d $a0, $a0, 0\n fault_read_resume:\n  jr $ra\n"
        "fault_opcode:\n  .4byte 0x00000000\n  jr $ra\n");
/* uc_sigmask at +40; struct sigcontext at +176: pc, then r0..r31 (sp is r3, a0 is r4) */
enum { UC_RESULT = 184 + 4 * 8, UC_SP = 184 + 3 * 8, UC_PC = 176, UC_MASK = 40, UC_STACK_FLAGS = 24, ILLEGAL_BYTES = 4, HAS_DIVIDE_FAULT = 0 };
#elif defined(__riscv)
__asm__(".text\n .option push\n .option norvc\n"
        "fault_read:\n  ld a0, 0(a0)\n fault_read_resume:\n  ret\n"
        "fault_opcode:\n  .4byte 0xffffffff\n  ret\n .option pop\n");
/* uc_sigmask at +40; the registers at +176: pc, then x1..x31 (sp is x2, a0 is x10) */
enum { UC_RESULT = 176 + 10 * 8, UC_SP = 176 + 2 * 8, UC_PC = 176, UC_MASK = 40, UC_STACK_FLAGS = 24, ILLEGAL_BYTES = 4, HAS_DIVIDE_FAULT = 0 };
#else
__asm__(".text\n"
        "fault_read:\n  ldr x0, [x0]\n fault_read_resume:\n  ret\n"
        "fault_opcode:\n  .inst 0x00000000\n  ret\n");
/* uc_sigmask at +40; struct sigcontext at +176: fault_address, x0..x30, sp, pc */
enum { UC_RESULT = 176 + 8, UC_SP = 176 + 8 + 31 * 8, UC_PC = 176 + 8 + 32 * 8, UC_MASK = 40, UC_STACK_FLAGS = 24, ILLEGAL_BYTES = 4, HAS_DIVIDE_FAULT = 0 };
#endif
static u64 uc_word(void* uc, int at) { return *(u64*)((u8*)uc + at); }
static void uc_set(void* uc, int at, u64 v) { *(u64*)((u8*)uc + at) = v; }

/* struct sigaction as each kernel has it: x86-64 and AArch64 have a restorer between the flags and the mask (only x86-64 needs one given), RISC-V and LoongArch have none */
static void handle(int signal, void (*handler)(int, int*, void*), u64 flags, u64 mask) {
#if defined(__x86_64__)
  u64 action[4] = {(u64)handler, flags | SA_SIGINFO | SA_RESTORER, (u64)sig_restorer, mask};
#elif defined(__riscv) || defined(__loongarch__)
  u64 action[4] = {(u64)handler, flags | SA_SIGINFO, mask, 0};
#else
  u64 action[4] = {(u64)handler, flags | SA_SIGINFO, 0, mask};
#endif
  S4(SYS_rt_sigaction, signal, action, 0, 8);
}
static void set_default(int signal) { u64 action[4] = {0, 0, 0, 0}; S4(SYS_rt_sigaction, signal, action, 0, 8); }
static u64 blocked(void) { u64 now = 0; S4(SYS_rt_sigprocmask, 0, 0, &now, 8); return now; }
static void timer_ms(long ms) { long t[4] = {0, 0, ms / 1000, (ms % 1000) * 1000}; S3(SYS_setitimer, 0, t, 0); }

static volatile int seen_signal, seen_code, seen_count, order[8], order_count;
static volatile u64 seen_mask, seen_address, seen_stack, seen_stack_flags, seen_trap, seen_sender_is_me, seen_frame_ok;
static char alt_stack[32768];

static void note(int what) { if (order_count < 8) order[order_count++] = what; }

static void on_signal(int signal, int* info, void* uc) {
  seen_signal = signal;
  seen_code = info[2];
  seen_sender_is_me = info[0] == signal && info[4] == S0(SYS_getpid);
  seen_mask = blocked();
  seen_frame_ok = uc_word(uc, UC_SP) > (u64)&signal && uc_word(uc, UC_MASK) == 0 && ((u64)uc & 15) == 0;
  ++seen_count;
}
static void on_count(int signal, int* info, void* uc) { (void)info; (void)uc; seen_signal = signal; ++seen_count; }
static void on_first(int signal, int* info, void* uc) {
  (void)info; (void)uc;
  note(signal);
  S2(SYS_kill, S0(SYS_getpid), SIGUSR2);      /* blocked by this handler's mask: its handler runs when this one has returned */
  note(100 + signal);
}
static void on_second(int signal, int* info, void* uc) { (void)info; (void)uc; note(signal); }
static void on_fault(int signal, int* info, void* uc) {
  seen_signal = signal;
  seen_code = info[2];
  seen_address = *(u64*)(info + 4);
#if defined(__x86_64__)
  seen_trap = uc_word(uc, UC_TRAPNO);
#endif
  ++seen_count;
  /* the program goes on somewhere else than where it faulted: after the instruction, with a result of the handler's choosing */
  if (signal == SIGSEGV) { uc_set(uc, UC_PC, (u64)fault_read_resume); uc_set(uc, UC_RESULT, 77); }
#if defined(__x86_64__)
  if (signal == SIGFPE) { uc_set(uc, UC_PC, (u64)fault_divide_resume); uc_set(uc, UC_RESULT, 88); }
#endif
  if (signal == SIGILL) { seen_address = seen_address == uc_word(uc, UC_PC); uc_set(uc, UC_PC, uc_word(uc, UC_PC) + ILLEGAL_BYTES); }
}
static void on_stack(int signal, int* info, void* uc) {
  (void)info;
  seen_signal = signal;
  seen_stack = (u64)&signal >= (u64)alt_stack && (u64)&signal < (u64)alt_stack + sizeof(alt_stack);
  seen_stack_flags = *(u32*)((u8*)uc + UC_STACK_FLAGS);
  ++seen_count;
}

__attribute__((noinline)) static long deep(long n) { volatile char pad[4096]; pad[0] = (char)n; return deep(n + 1) + pad[0]; }

void start_c(void) {
  const long self = S0(SYS_getpid);

  /* --- a handler entered at the end of the call that raised the signal */
  handle(SIGUSR1, on_signal, 0, BIT(SIGTERM));
  report("kill(self, SIGUSR1)", S2(SYS_kill, self, SIGUSR1));
  report("handler ran", seen_count);
  report("  its signal", seen_signal);
  report("  si_code", seen_code);
  report("  si_signo and si_pid", seen_sender_is_me);
  report("  the frame", seen_frame_ok);
  report("  blocked meanwhile: itself and the action's mask", seen_mask == (BIT(SIGUSR1) | BIT(SIGTERM)));
  report("  blocked afterwards", blocked());
  seen_count = 0;
  report("tgkill to this thread", S3(SYS_tgkill, self, S0(SYS_gettid), SIGUSR1));
  report("handler ran", seen_count);
  report("  si_code", seen_code);

  /* --- a signal raised inside a handler that blocks it waits for the handler's end */
  handle(SIGUSR1, on_first, 0, BIT(SIGUSR2));
  handle(SIGUSR2, on_second, 0, 0);
  S2(SYS_kill, self, SIGUSR1);
  put("order:");
  for (int i = 0; i < order_count; ++i) { put(" "); put_number(order[i]); }
  put("\n");

  /* --- a blocked signal's handler is entered when it is unblocked; sigsuspend lets it in for the length of the call */
  handle(SIGUSR1, on_count, 0, 0);
  seen_count = 0;
  u64 mask = BIT(SIGUSR1), none = 0;
  S4(SYS_rt_sigprocmask, 0, &mask, 0, 8);
  S2(SYS_kill, self, SIGUSR1);
  report("blocked: handler not run", seen_count);
  S4(SYS_rt_sigprocmask, 1, &mask, 0, 8);
  report("unblocked: handler ran", seen_count);
  S4(SYS_rt_sigprocmask, 0, &mask, 0, 8);
  S2(SYS_kill, self, SIGUSR1);
  report("sigsuspend", S2(SYS_rt_sigsuspend, &none, 8));
  report("  handler ran", seen_count);
  report("  mask is back", blocked() == BIT(SIGUSR1));
  S4(SYS_rt_sigprocmask, 1, &mask, 0, 8);

  /* --- a timer's signal in the middle of a loop: the loop's registers are what they were when it goes on */
  handle(SIGALRM, on_count, 0, 0);
  seen_count = 0;
  timer_ms(20);
  u64 sum = 0, i = 0;
  for (; seen_count == 0 && i < 4000000000ul; ++i) sum += i ^ (i >> 3);
  u64 check = 0;
  for (u64 k = 0; k < i; ++k) check += k ^ (k >> 3);
  report("timer handler ran in the loop", seen_count);
  report("  the loop's sum is right", sum == check && i > 0);

  /* --- faults: the handler is told where and why, and sends the program on */
  handle(SIGSEGV, on_fault, 0, 0);
  handle(SIGFPE, on_fault, 0, 0);
  handle(SIGILL, on_fault, 0, 0);
  seen_count = 0;
  report("read of address 0x40", fault_read((long*)0x40));
  report("  signal", seen_signal);
  report("  si_code", seen_code);
  report("  si_addr", (i64)seen_address);
#if defined(__x86_64__)      /* (the other CPUs have no trap number in the frame, and their division does not trap) */
  report("  trap", (i64)seen_trap);
  report("division by zero", fault_divide(0));
  report("  signal", seen_signal);
  report("  si_code", seen_code);
  report("division that is fine", fault_divide(7));
#endif
  fault_opcode();
  report("invalid instruction: signal", seen_signal);
  report("  si_code", seen_code);
  report("  si_addr is the instruction", (i64)seen_address);
  report("handlers ran", seen_count);
  set_default(SIGSEGV);
  set_default(SIGFPE);
  set_default(SIGILL);

  /* --- calls that wait: cut short by a handler, or started again when the action says SA_RESTART */
  int fds[2];
  S2(SYS_pipe2, fds, 0);
  char byte = 0;
  seen_count = 0;
  timer_ms(30);
  report("read cut short", S3(SYS_read, fds[0], &byte, 1));
  report("  handler ran", seen_count);
  handle(SIGALRM, on_count, SA_RESTART, 0);
  long pid = do_fork();
  if (pid == 0) { sleep_ms(150); S3(SYS_write, fds[1], "x", 1); leave(0); }
  seen_count = 0;
  timer_ms(30);
  report("read started again", S3(SYS_read, fds[0], &byte, 1));
  report("  handler ran", seen_count);
  report("  the byte", byte);
  wait_how(pid);
  seen_count = 0;
  timer_ms(30);
  long two_seconds[2] = {2, 0}, left[2] = {0, 0};
  report("nanosleep cut short", S2(SYS_nanosleep, two_seconds, left));
  report("  time was left", left[0] == 1 && left[1] > 0);
  report("  handler ran", seen_count);

  /* --- between processes: a child signals its parent, which waits in pause(); the parent signals a child that has a handler; SIGCHLD */
  handle(SIGUSR1, on_count, 0, 0);
  handle(SIGCHLD, on_count, 0, 0);
  seen_count = 0;
  seen_signal = 0;
  mask = BIT(SIGCHLD);
  S4(SYS_rt_sigprocmask, 0, &mask, 0, 8);
  pid = do_fork();
  if (pid == 0) { sleep_ms(50); S2(SYS_kill, S0(SYS_getppid), SIGUSR1); leave(3); }
  report("pause", do_pause());
  report("  the child's signal", seen_signal);
  report("child", wait_how(pid));
  seen_signal = 0;
  report("sigsuspend for SIGCHLD", S2(SYS_rt_sigsuspend, &none, 8));
  report("  the signal", seen_signal);
  S4(SYS_rt_sigprocmask, 1, &mask, 0, 8);
  set_default(SIGCHLD);
  pid = do_fork();
  if (pid == 0) {
    seen_count = 0;
    while (seen_count == 0) do_pause();
    leave(40 + seen_signal);
  }
  sleep_ms(50);
  report("kill(child, SIGUSR1)", S2(SYS_kill, pid, SIGUSR1));
  report("child with a handler", wait_how(pid));
  pid = do_fork();
  if (pid == 0) { for (;;) do_pause(); }
  sleep_ms(50);
  S2(SYS_kill, pid, SIGTERM);
  report("child without one", wait_how(pid));
  pid = do_fork();
  if (pid == 0) {      /* a child that computes, never calling the kernel */
    volatile u64 spin = 0;
    for (;;) ++spin;
  }
  sleep_ms(50);
  S2(SYS_kill, pid, SIGTERM);
  report("child in a loop", wait_how(pid));

  /* --- SA_RESETHAND: the handler runs once, then the default action is back */
  pid = do_fork();
  if (pid == 0) {
    handle(SIGUSR2, on_count, SA_RESETHAND, 0);
    seen_count = 0;
    S2(SYS_kill, S0(SYS_getpid), SIGUSR2);
    if (seen_count != 1) leave(1);
    S2(SYS_kill, S0(SYS_getpid), SIGUSR2);
    leave(2);
  }
  report("SA_RESETHAND", wait_how(pid));

  /* --- the alternate stack: a handler that asks for it runs there; a stack that ran out can still be reported on it */
  u64 stack[3] = {(u64)alt_stack, 0, sizeof(alt_stack)};
  report("sigaltstack", S2(SYS_sigaltstack, stack, 0));
  handle(SIGUSR1, on_stack, SA_ONSTACK, 0);
  seen_count = 0;
  S2(SYS_kill, self, SIGUSR1);
  report("handler on the alternate stack", (i64)seen_stack);
  report("  uc_stack.ss_flags", (i64)seen_stack_flags);
  handle(SIGUSR1, on_stack, 0, 0);
  S2(SYS_kill, self, SIGUSR1);
  report("handler on the ordinary stack", (i64)!seen_stack);
  pid = do_fork();
  if (pid == 0) {
    handle(SIGSEGV, on_stack, SA_ONSTACK | SA_RESETHAND, 0);
    seen_count = 0;
    deep(0);      /* runs out of stack; the handler returns to the same fault, which now ends the program */
    leave(1);
  }
  report("stack overflow, with a handler on the alternate stack", wait_how(pid));
  pid = do_fork();
  if (pid == 0) {
    u64 off[3] = {0, 2, 0};
    S2(SYS_sigaltstack, off, 0);
    handle(SIGSEGV, on_stack, 0, 0);
    deep(0);      /* no room for the handler's frame: the program ends */
    leave(1);
  }
  report("stack overflow, with nowhere to run a handler", wait_how(pid));
  put("signals ok\n");
  leave(0);
}

#if defined(__x86_64__)
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__loongarch__)
__asm__(".globl _start\n_start:\n  move $fp, $zero\n  bstrins.d $sp, $zero, 3, 0\n  bl start_c\n");
#elif defined(__riscv)
__asm__(".globl _start\n_start:\n  li s0, 0\n  andi sp, sp, -16\n  call start_c\n");
#else
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#endif

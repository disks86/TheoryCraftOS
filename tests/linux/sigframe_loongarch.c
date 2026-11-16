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
/* A LoongArch Linux program (no C library) that prints the frames Linux gives its signal handlers, byte for byte: one for a signal sent with kill() while
 * known values are in the registers, one for a fault. The frame code (src/Process/LinuxSignals.cpp) is checked against this output byte for byte: tests/linux/build_sigframe_loongarch.sh runs the program under qemu's user mode (three builds of it: see VECTORS) and
 * LinuxSyscallTest reads what they printed.
 *
 * Each frame is printed as
 *   signal <n>
 *   at <the handler's stack pointer> gap <interrupted stack pointer - it> info <siginfo - it> context <ucontext - it>
 *   way-back <the two instructions the handler's return address points at>
 *   frame <every byte from the handler's stack pointer up to the interrupted one, in hex>
 * and the program ends with "returned <s0 after the handler changed it in the frame>" and "frames ok". */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static long sys(long number, long a, long b, long c, long d) {
  register long a7 __asm__("a7") = number;
  register long a0 __asm__("a0") = a;
  register long a1 __asm__("a1") = b;
  register long a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d;
  __asm__ volatile("syscall 0" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3) : "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "memory");
  return a0;
}

static char line[16384];
static u64 used;

static void flush(void) {
  sys(64, 1, (long)line, (long)used, 0);
  used = 0;
}
static void text(const char* s) {
  while (*s && used < sizeof(line)) line[used++] = *s++;
}
static void hex(u64 value, int digits) {
  for (int i = digits - 1; i >= 0 && used < sizeof(line); --i) line[used++] = "0123456789abcdef"[(value >> (4 * i)) & 15];
}
static void number(u64 value) {
  char digits[24];
  int n = 0;
  do digits[n++] = (char)('0' + value % 10); while ((value /= 10) != 0);
  while (n > 0 && used < sizeof(line)) line[used++] = digits[--n];
}

/* The handler as the kernel enters it, keeping what C code would lose: the stack pointer and the return address. */
void entry(int, void*, void*);
__asm__(".text\n.globl entry\n.type entry, @function\nentry:\n  move $a3, $sp\n  move $a4, $ra\n  b report\n");

void report(int signal, u8* info, u8* context, u8* sp, u32* way_back) {
  u64* registers = (u64*)(context + 176 + 8);      /* uc_mcontext.sc_regs */
  u8* interrupted = (u8*)registers[3];
  text("signal "); number((u64)signal);
  text("\nat "); hex((u64)sp, 16);
  text(" gap "); number((u64)(interrupted - sp));
  text(" info "); number((u64)(info - sp));
  text(" context "); number((u64)(context - sp));
  text("\nway-back "); hex(way_back[0], 8); text(" "); hex(way_back[1], 8);
  text("\nframe ");
  for (u8* at = sp; at < interrupted; ++at) hex(*at, 2);
  text("\n");
  flush();
  if (signal == 11) *(u64*)(context + 176) += 4;      /* past the instruction that faulted */
  else registers[23] = 0x5E7BAC;                      /* s0, for the program to find changed */
}

/* Built with -DVECTORS=1 or 2 the program has used the 128-bit (LSX) or 256-bit (LASX) vector registers by then, which changes the kind of record the
 * floating-point state is in: vr24 / xr24 get their upper parts set (the lowest 64 bits stay f24's). */
#if VECTORS == 2
#define VECTOR_USE "xvreplgr2vr.d $xr24, $r23\n"
#elif VECTORS == 1
#define VECTOR_USE "vreplgr2vr.d $vr24, $r23\n"
#else
#define VECTOR_USE ""
#endif

/* kill(pid, SIGUSR1) with known values in s0..s8, f24..f31 and the condition flags. What s0 holds afterwards is returned. */
static u64 raise_marked(long pid) {
  register long a0 __asm__("a0") = pid;
  __asm__ volatile(
      "li.d $r23, 0xA500000000000123\n li.d $r24, 0xA500000000000224\n li.d $r25, 0xA500000000000325\n li.d $r26, 0xA500000000000426\n"
      "li.d $r27, 0xA500000000000527\n li.d $r28, 0xA500000000000628\n li.d $r29, 0xA500000000000729\n li.d $r30, 0xA500000000000830\n"
      "li.d $r31, 0xA500000000000931\n"
      "movgr2fr.d $f24, $r23\n movgr2fr.d $f25, $r24\n movgr2fr.d $f26, $r25\n movgr2fr.d $f27, $r26\n"
      "movgr2fr.d $f28, $r27\n movgr2fr.d $f29, $r28\n movgr2fr.d $f30, $r29\n movgr2fr.d $f31, $r30\n"
      VECTOR_USE
      "li.d $t0, 1\n movgr2cf $fcc1, $t0\n movgr2cf $fcc6, $t0\n movgr2cf $fcc0, $zero\n"
      "li.d $a1, 10\n li.d $a7, 129\n syscall 0\n move $a0, $r23\n"
      : "+r"(a0)
      :
      : "a1", "a7", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31", "f24", "f25", "f26", "f27",
        "f28", "f29", "f30", "f31", "fcc0", "fcc1", "fcc6", "memory");
  return (u64)a0;
}

void run(void) {
  /* struct sigaction here: handler, flags, mask (there is no restorer) */
  u64 action[3] = {(u64)entry, 4 /* SA_SIGINFO */, 0};
  if (sys(134, 10, (long)action, 0, 8) != 0 || sys(134, 11, (long)action, 0, 8) != 0) sys(93, 1, 0, 0, 0);
  const u64 s0 = raise_marked(sys(172, 0, 0, 0, 0));
  __asm__ volatile("li.d $t0, 0x10\n st.d $zero, $t0, 0\n" : : : "t0", "memory");      /* a write to an address that has no memory */
  text("returned "); hex(s0, 16);
  text("\nframes ok\n");
  flush();
  sys(93, 0, 0, 0, 0);
}

__asm__(".text\n.globl _start\n_start:\n  bstrins.d $sp, $zero, 3, 0\n  bl run\n");

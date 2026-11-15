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

/* A 32-bit Windows program (PE32, i386) with no libraries at all, for tests/wow64_qemu.sh: it runs in the CPU's compatibility mode and calls the NT
 * services itself, the way the 32-bit ntdll.dll does - INT 2E with the service number in EAX and, in EDX, the address of the arguments, each a 64-bit
 * value, with structures laid out as the 64-bit services read them (Process/Syscall.hpp: decode_x86_32_windows). It checks that it is where a 32-bit
 * program must be (code, stack and new memory below 2 GiB), that memory it asks for works, and ends with exit code 42.
 *   i686-w64-mingw32-gcc -O1 -ffreestanding -nostdlib -Isrc/Windows -Wl,--entry=_start32 -Wl,--subsystem,console -o hello32.exe tests/windows/hello32.c */
#include "tcos_syscalls.h"

typedef unsigned long long u64;
typedef unsigned int u32;

static u32 service(u32 number, const u64 *arguments) {
  u32 status;
  __asm__ volatile("int $0x2e" : "=a"(status) : "a"(number), "d"(arguments) : "memory", "cc");
  return status;
}

static u64 g_output;      /* the handle of the standard output */
static int g_passed, g_failed;

static void print(const char *text) {
  u32 length = 0;
  while (text[length]) ++length;
  u64 status_block[2] = {0, 0};      /* IO_STATUS_BLOCK as a 64-bit program has it */
  const u64 arguments[9] = {g_output, 0, 0, 0, (u32)status_block, (u32)text, length, 0, 0};
  service(NT_SERVICE_NtWriteFile, arguments);
}

static void print_hex(u32 value) {
  char text[11] = "0x00000000";
  for (int i = 0; i < 8; ++i) text[9 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
  print(text);
}

static void check(int ok, const char *what) {
  print(ok ? "hello32: ok: " : "hello32: FAILED: ");
  print(what);
  print("\n");
  if (ok) ++g_passed; else ++g_failed;
}

static volatile u32 g_data = 0x12345678;

/* The program's first instruction. Without an ntdll.dll the kernel starts the program itself, with the address of the (64-bit) PEB as its argument. */
void __cdecl start32(const unsigned char *peb) {
  const unsigned char *parameters = (const unsigned char *)(u32) * (const u64 *)(peb + 0x20);      /* PEB.ProcessParameters */
  g_output = *(const u64 *)(parameters + 0x28);                                                      /* .StandardOutput */
  print("hello32: a 32-bit program is running\n");

  int local = 0;
  check(sizeof(void *) == 4, "pointers are 32 bits");
  check((u32)&start32 < 0x7FFF0000u && (u32)&local < 0x7FFF0000u && (u32)peb < 0x7FFF0000u, "code, stack and PEB are below 2 GiB");
  check(g_data == 0x12345678, "initialised data is there");
  g_data = 0x9ABCDEF0;
  check(g_data == 0x9ABCDEF0, "data can be written");
  u64 big = 0x100000000ull + (u32)&local;      /* 64-bit arithmetic in 32-bit code */
  check((u32)(big >> 32) == 1 && (u32)big == (u32)&local, "64-bit arithmetic");
  double half = 0.5, three = 3.0;              /* the floating point state came with the thread */
  check((int)(half * three * 4.0) == 6, "floating point");

  /* The 32-bit TEB is at FS: it points at itself, and knows the stack this code runs on. */
  u32 self, stack_base, stack_limit;
  __asm__ volatile("movl %%fs:0x18, %0" : "=r"(self));
  __asm__ volatile("movl %%fs:0x04, %0" : "=r"(stack_base));
  __asm__ volatile("movl %%fs:0x08, %0" : "=r"(stack_limit));
  check(self != 0 && self < 0x7FFF0000u && *(u32 *)(self + 0x18) == self, "FS is the 32-bit TEB");
  check((u32)&local < stack_base && (u32)&local >= stack_limit && *(u32 *)self == 0xFFFFFFFFu, "the TEB knows the stack, and the exception chain is empty");

  /* Memory: NtAllocateVirtualMemory(process, &base, zero bits, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE). */
  u64 base = 0, size = 0x30000;
  const u64 allocate[6] = {~0ull, (u32)&base, 0, (u32)&size, 0x3000, 4};
  u32 status = service(NT_SERVICE_NtAllocateVirtualMemory, allocate);
  print("hello32: allocated at ");
  print_hex((u32)base);
  print("\n");
  check(status == 0 && base != 0 && base + size <= 0x7FFF0000ull, "new memory is below 2 GiB");
  if (status == 0) {
    volatile u32 *words = (volatile u32 *)(u32)base;
    u32 sum = 0;
    for (u32 i = 0; i < 0x30000 / 4; i += 1024) words[i] = i + 1;
    for (u32 i = 0; i < 0x30000 / 4; i += 1024) sum += words[i];
    check(sum == 48 * 1 + 1024 * (47 * 48 / 2), "the new memory holds what is written to it");
  }
  /* A bad argument block is refused, not followed. */
  u32 bad;
  __asm__ volatile("int $0x2e" : "=a"(bad) : "a"(NT_SERVICE_NtWriteFile), "d"(0x7FFF8000u) : "memory", "cc");
  check(bad == 0xC0000005u, "arguments at a bad address are refused");
  check(service(0xFFFF, allocate) != 0, "an unknown service is refused");

  print(g_failed == 0 ? "hello32: all checks passed\n" : "hello32: some checks FAILED\n");
  const u64 leave[2] = {~0ull, g_failed == 0 ? 42 : 1};
  service(NT_SERVICE_NtTerminateProcess, leave);
  for (;;) {}
}

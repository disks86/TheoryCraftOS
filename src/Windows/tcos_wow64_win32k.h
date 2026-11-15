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

/* The way a library of 32-bit programs calls a win32k service (docs/Wow64.md), shared by the libraries that make such calls themselves: win32u.dll,
 * gdi32.dll, user32.dll. Only for i386.
 *
 * The kernel has one set of services, written for 64-bit programs. A 32-bit program calls one through INT 2E with the service's number in eax and,
 * in edx, the address of the arguments, each 64 bits wide. A service's stub puts the number in eax and jumps to the code WOW_WIN32K_CODE makes:
 * the caller's arguments are on its stack, each 32 bits. How many a service takes is not known here, so the most any takes (CreateWindowEx: 17) are
 * copied, which reads at most 72 bytes of the caller's stack - there is always that much above a call. Each is widened with its sign: a pointer of
 * a 32-bit program is below 2 GiB and stays what it is, a handle like HWND_TOPMOST (-1) stays -1, and the kernel takes only the low half of a
 * parameter that is 32 bits wide. The stubs are cdecl: the caller takes the arguments off its stack. */
#ifndef TCOS_WOW64_WIN32K_H
#define TCOS_WOW64_WIN32K_H
#ifdef __i386__
#define WOW_STR_(x) #x
#define WOW_STR(x) WOW_STR_(x)
#define WOW_ARGUMENTS 18
/* `label`: the code's name. `tail`: instructions run on the result (in eax) before returning, or "". */
#define WOW_WIN32K_CODE(label, tail)                                                                  \
  __asm__(".text\n"                                                                                   \
          #label ":\n"                                                                                \
          "  pushl %ebp\n"                                                                            \
          "  movl %esp, %ebp\n"                                                                       \
          "  subl $" WOW_STR(WOW_ARGUMENTS) " * 8, %esp\n"                                            \
          "  xorl %ecx, %ecx\n"                                                                       \
          "1:\n"                                                                                      \
          "  movl 8(%ebp,%ecx,4), %edx\n"                                                             \
          "  movl %edx, (%esp,%ecx,8)\n"                                                              \
          "  sarl $31, %edx\n"                                                                        \
          "  movl %edx, 4(%esp,%ecx,8)\n"                                                             \
          "  incl %ecx\n"                                                                             \
          "  cmpl $" WOW_STR(WOW_ARGUMENTS) ", %ecx\n"                                                \
          "  jb 1b\n"                                                                                 \
          "  movl %esp, %edx\n"                                                                       \
          "  int $0x2e\n"                                                                             \
          tail                                                                                        \
          "  movl %ebp, %esp\n"                                                                       \
          "  popl %ebp\n"                                                                             \
          "  ret\n")
/* The stub of service `number`, named `name` in C (cdecl), going through the code `code`. */
#define WOW_WIN32K_STUB(name, number, code) __asm__(".text\n.globl _" #name "\n_" #name ":\n movl $" WOW_STR(number) ", %eax\n jmp " #code "\n")
#endif
#endif

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

/* A 32-bit Windows program built against the old C runtime (msvcrt.dll itself, not the universal runtime's api sets), for tests/wow64_qemu.sh
 * (docs/Wow64.md). Such a program finds its standard streams by itself: stdout is 32 bytes after what __p__iob() gives it, stderr 64. Everything it
 * prints goes through that, so the lines the test looks for are themselves the first check. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -mcrtdll=msvcrt-os -o old32.exe tests/windows/old32.c */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

/* The old runtime's table of file numbers, which code compiled against it reads itself (d3dx9_42.dll does): blocks of 32 entries. */
typedef struct { intptr_t osfhnd; char osfile; char pipech; int exflag; CRITICAL_SECTION lock; } OldIoInfo;
__declspec(dllimport) extern OldIoInfo *__pioinfo[];
__declspec(dllimport) extern OldIoInfo __badioinfo;

#ifdef __i386__
/* The x87 math entry points (_CIpow and the others): the arguments on the x87 stack, the first deepest, the result in ST(0). Called the way the
 * Microsoft compiler calls them, through these two: a cdecl function returns its double in ST(0) as well. */
double ci_call1(void *function, double x);
double ci_call2(void *function, double x, double y);
__asm__(".globl _ci_call1\n_ci_call1:\n fldl 8(%esp)\n call *4(%esp)\n ret\n"
        ".globl _ci_call2\n_ci_call2:\n fldl 8(%esp)\n fldl 16(%esp)\n call *4(%esp)\n ret\n");
static int close_to(double a, double b) { const double d = a - b; return d < 1e-9 && d > -1e-9; }
/* class exception of msvcrt.dll: a virtual table, the message, whether the object owns it; its members are thiscall. */
typedef struct { void **vtable; const char *name; int do_free; } OldException;
typedef OldException *(__thiscall *ExceptionCtor)(OldException *);
typedef OldException *(__thiscall *ExceptionCtorName)(OldException *, const char *const *);
typedef OldException *(__thiscall *ExceptionCopy)(OldException *, const OldException *);
typedef void (__thiscall *ExceptionDtor)(OldException *);
typedef const char *(__thiscall *ExceptionWhat)(const OldException *);
#endif

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "old32: ok: %s\n" : "old32: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

int main(void) {
  /* (32 bytes in a 32-bit program; the same source built for x86-64 is a test of the 64-bit msvcrt.dll, where it is 48) */
  check(sizeof(FILE) == (sizeof(void *) == 8 ? 48 : 32) && (char *)stdout - (char *)stdin == sizeof(FILE) && (char *)stderr - (char *)stdin == 2 * sizeof(FILE),
        "the standard streams are one FILE apart");
  check(_fileno(stdin) == 0 && _fileno(stdout) == 1 && _fileno(stderr) == 2, "their file numbers are 0, 1 and 2");
  check(fprintf(stderr, "old32: (a line on stderr)\n") > 0 && !ferror(stderr), "fprintf to stderr");
  check(fputs("old32: (a line with fputs)\n", stdout) >= 0 && fflush(stdout) == 0, "fputs and fflush on stdout");

  /* a file through a stream of the runtime's own, beside the three */
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  strcat(path, "old32.txt");
  FILE *file = fopen(path, "w+");
  check(file != NULL && file != stdout && file != stderr, "fopen");
  char line[32] = "";
  check(file && fprintf(file, "%d %s\n", 1234, "words") == 11 && fseek(file, 0, SEEK_SET) == 0 && fgets(line, sizeof(line), file) && strcmp(line, "1234 words\n") == 0,
        "what was written to a file is read back");
  if (file) fclose(file);

  /* a Windows handle made into a file number: the handle is as wide as a pointer on the stack */
  const HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  const int number = handle != INVALID_HANDLE_VALUE ? _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY) : -1;
  char bytes[8] = "";
  check(number >= 3 && (HANDLE)_get_osfhandle(number) == handle && _read(number, bytes, 4) == 4 && memcmp(bytes, "1234", 4) == 0,
        "_open_osfhandle with its flags, _get_osfhandle and _read");
  check(number >= 3 && __pioinfo[number >> 5][number & 31].osfhnd == (intptr_t)handle && (__pioinfo[number >> 5][number & 31].osfile & 0x81) == 0x01 &&
        (__pioinfo[0][1].osfile & 0x01) && __badioinfo.osfhnd == -1, "__pioinfo has the handle of a file number, open and binary; __badioinfo none");
  if (number >= 0) _close(number);
  check(number >= 3 && __pioinfo[number >> 5][number & 31].osfhnd == -1 && __pioinfo[number >> 5][number & 31].osfile == 0, "a closed file number is gone from __pioinfo");
  DeleteFileA(path);

  {      /* "=C:" (the current folder of drive C, which the C runtime's _chdir sets) is a name; "A=B" is not */
    char value[64] = "";
    check(SetEnvironmentVariableA("=C:", "C:\\Temp") && GetEnvironmentVariableA("=C:", value, sizeof(value)) == 7 && strcmp(value, "C:\\Temp") == 0 &&
          !SetEnvironmentVariableA("A=B", "x") && GetLastError() == ERROR_INVALID_PARAMETER, "a variable name may begin with '='");
  }
  {      /* exp and pow of msvcrt.dll (2^t taken apart with the x87: the fraction had the wrong sign, and pow's operands were swapped) */
    volatile double one = 1.0, half = 0.5, nine = 9.0, ten = 10.0, two = 2.0, minus = -2.3;
    const double e1 = exp(one), eh = exp(minus), p1 = pow(nine, half), p2 = pow(two, ten), p3 = pow(ten, -2.0 * one);
    check(e1 > 2.718281828459 && e1 < 2.718281828460 && eh > 0.1002588437228 && eh < 0.1002588437229, "exp(1) and exp(-2.3)");
    check(p1 > 2.999999999999 && p1 < 3.000000000001 && p2 == 1024.0 && p3 > 0.00999999999999 && p3 < 0.01000000000001, "pow(9, 0.5), pow(2, 10), pow(10, -2)");
  }

#ifdef __i386__
  {
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    void *ci_pow = (void *)GetProcAddress(crt, "_CIpow"), *ci_sqrt = (void *)GetProcAddress(crt, "_CIsqrt"), *ci_atan2 = (void *)GetProcAddress(crt, "_CIatan2");
    void *ci_fmod = (void *)GetProcAddress(crt, "_CIfmod"), *ci_cos = (void *)GetProcAddress(crt, "_CIcos"), *ci_log10 = (void *)GetProcAddress(crt, "_CIlog10");
    check(ci_pow && ci_sqrt && ci_atan2 && ci_fmod && ci_cos && ci_log10, "the _CI math functions are there");
    if (ci_pow && ci_sqrt && ci_atan2 && ci_fmod && ci_cos && ci_log10) {
      check(close_to(ci_call2(ci_pow, 2.0, 10.0), 1024.0) && close_to(ci_call2(ci_pow, 9.0, 0.5), 3.0) && close_to(ci_call2(ci_pow, 0.5, 2.0), 0.25), "_CIpow is pow(ST(1), ST(0))");
      check(close_to(ci_call2(ci_atan2, 1.0, 0.0), 1.5707963267948966) && close_to(ci_call2(ci_fmod, 7.5, 2.0), 1.5), "_CIatan2 and _CIfmod take ST(1) first");
      check(close_to(ci_call1(ci_sqrt, 2.25), 1.5) && close_to(ci_call1(ci_cos, 0.0), 1.0) && close_to(ci_call1(ci_log10, 1000.0), 3.0), "_CIsqrt, _CIcos, _CIlog10");
      /* the x87 stack is as it was: eight more values fit (a function that left one behind would make the eighth overflow into a NaN) */
      double sum = 0;
      for (int i = 0; i < 9; ++i) sum += ci_call1(ci_sqrt, 4.0);
      check(close_to(sum, 18.0), "the _CI functions leave the x87 stack as they found it");
    }
    ExceptionCtor ctor = (ExceptionCtor)GetProcAddress(crt, "??0exception@@QAE@XZ");
    ExceptionCtorName ctor_name = (ExceptionCtorName)GetProcAddress(crt, "??0exception@@QAE@ABQBD@Z");
    ExceptionCopy copy = (ExceptionCopy)GetProcAddress(crt, "??0exception@@QAE@ABV0@@Z");
    ExceptionDtor dtor = (ExceptionDtor)GetProcAddress(crt, "??1exception@@UAE@XZ");
    ExceptionWhat what = (ExceptionWhat)GetProcAddress(crt, "?what@exception@@UBEPBDXZ");
    check(ctor && ctor_name && copy && dtor && what && GetProcAddress(crt, "?terminate@@YAXXZ"), "class exception and terminate are there");
    if (ctor && ctor_name && copy && dtor && what) {
      OldException a, b, c;
      const char *message = "bad thing";
      check(ctor(&a) == &a && a.vtable && a.name == NULL && strcmp(what(&a), "Unknown exception") == 0, "exception(): no message");
      check(ctor_name(&b, &message) == &b && b.do_free && b.name != message && strcmp(what(&b), "bad thing") == 0, "exception(const char *const &): its own copy");
      check(copy(&c, &b) == &c && c.name != b.name && strcmp(what(&c), "bad thing") == 0 && c.vtable == b.vtable, "the copy constructor copies an owned message");
      /* the virtual what() and the deleting destructor are in the table, as compiled code calls them */
      typedef const char *(__thiscall *VirtualWhat)(const OldException *);
      check(((VirtualWhat)c.vtable[1])(&c) == c.name, "what() through the virtual table");
      dtor(&a); dtor(&b); dtor(&c);      /* (each frees the message it owns: ASan-free here, so only that it returns is seen) */
    }
  }
#endif

  if (g_failed == 0) printf("old32: all %d checks passed\n", g_passed);
  else printf("old32: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}

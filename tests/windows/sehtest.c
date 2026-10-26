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

/* Structured exception handling on TheoryCraftOS's ntdll.dll (see tests/windows/build.sh; built with clang, which has __try/__except/__finally). Prints
 * "sehtest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static volatile int g_zero, g_sink;      /* (kept in memory so that the compiler does not fold the faults away) */
static int g_order[16], g_order_count;
static void note(int n) { if (g_order_count < 16) g_order[g_order_count++] = n; }

static void test_raise(void) {
  int caught = 0;
  __try {
    RaiseException(0xE0001234, 0, 0, NULL);
    check(0, "RaiseException returns");
  } __except (GetExceptionCode() == 0xE0001234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    caught = 1;
  }
  check(caught, "RaiseException is caught by __except");

  ULONG_PTR args[2] = {7, 9};
  DWORD n = 0;
  ULONG_PTR a0 = 0, a1 = 0;
  __try {
    RaiseException(0xE0000001, 0, 2, args);
  } __except (n = GetExceptionInformation()->ExceptionRecord->NumberParameters, a0 = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0], a1 = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1], EXCEPTION_EXECUTE_HANDLER) {
  }
  check(n == 2 && a0 == 7 && a1 == 9, "the arguments of RaiseException reach the filter");
}

/* (the faults are in functions of their own: clang puts only calls, not single instructions, in the scope of a __try) */
static __attribute__((noinline)) void write_to(volatile int *p) { *p = 5; }
static __attribute__((noinline)) int read_from(volatile int *p) { return *p; }
static __attribute__((noinline)) int divide(int a, int b) { return a / b; }
static __attribute__((noinline)) void trap(void) { __builtin_trap(); }
static __attribute__((noinline)) void break_in(void) { __debugbreak(); }

static void test_faults(void) {
  int code = 0;
  __try {
    write_to((volatile int *)0);
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(code == (int)EXCEPTION_ACCESS_VIOLATION, "a write to address 0 is an access violation");

  ULONG_PTR kind = 99, address = 1;
  __try {
    read_from((volatile int *)0x10);
  } __except (kind = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0], address = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1], EXCEPTION_EXECUTE_HANDLER) {
  }
  check(kind == 0 && address == 0x10, "a read: information[0] = 0 and the address");

  kind = 99;
  __try {
    write_to((volatile int *)0x20);
  } __except (kind = GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0], EXCEPTION_EXECUTE_HANDLER) {
  }
  check(kind == 1, "a write: information[0] = 1");

  code = 0;
  __try {
    g_sink = divide(100, g_zero);
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(code == (int)EXCEPTION_INT_DIVIDE_BY_ZERO, "integer division by zero");

  code = 0;
  __try {
    trap();
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(code == (int)EXCEPTION_ILLEGAL_INSTRUCTION, "ud2 is an illegal instruction");

  code = 0;
  __try {
    break_in();
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(code == (int)EXCEPTION_BREAKPOINT, "int3 is a breakpoint");
}

static void leaf_that_raises(void) { RaiseException(0xE0000002, 0, 0, NULL); }
static void middle(void) {
  volatile int buffer[64];
  buffer[0] = 1;
  __try {
    leaf_that_raises();
  } __finally {
    note(2);
  }
  note(99);      /* not reached */
}
static void test_unwind_order(void) {
  g_order_count = 0;
  __try {
    note(1);
    middle();
    note(98);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    note(3);
  }
  check(g_order_count == 3 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3, "__finally runs before the handler of the caller");
}

static int try_finally_return(int *flag) {
  __try {
    return 10;
  } __finally {
    *flag = 1;
  }
}
static void test_finally(void) {
  int flag = 0;
  check(try_finally_return(&flag) == 10 && flag == 1, "__finally runs when the __try returns");
  int normal = 0;
  __try {
    normal = 1;
  } __finally {
    normal += 10;
  }
  check(normal == 11, "__finally runs after a normal end");
}

static LONG g_filter_calls;
static LONG filter_counting(DWORD code) { ++g_filter_calls; return code == 0xE0000003 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
static void test_nested(void) {
  int outer = 0, inner = 0;
  g_filter_calls = 0;
  __try {
    __try {
      RaiseException(0xE0000003, 0, 0, NULL);
    } __except (filter_counting(0xE0000099)) {
      inner = 1;
    }
  } __except (filter_counting(GetExceptionCode())) {
    outer = 1;
  }
  check(outer == 1 && inner == 0 && g_filter_calls == 2, "the inner filter declines and the outer one handles");

  int rethrown = 0;
  __try {
    __try {
      RaiseException(0xE0000004, 0, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      RaiseException(0xE0000005, 0, 0, NULL);
    }
  } __except (GetExceptionCode() == 0xE0000005 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    rethrown = 1;
  }
  check(rethrown, "an exception raised inside a handler is caught by the outer one");
}

static LONG g_vectored_seen;
static LONG CALLBACK vectored(PEXCEPTION_POINTERS p) {
  if (p->ExceptionRecord->ExceptionCode == 0xE0000006) ++g_vectored_seen;
  return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_skip_instruction(PEXCEPTION_POINTERS p) {
  if (p->ExceptionRecord->ExceptionCode == 0xE0000007) {
    p->ContextRecord->Rax = 1234;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void test_vectored(void) {
  PVOID h = AddVectoredExceptionHandler(1, vectored);
  check(h != NULL, "AddVectoredExceptionHandler");
  __try {
    RaiseException(0xE0000006, 0, 0, NULL);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  check(g_vectored_seen == 1, "the vectored handler sees the exception first");
  check(RemoveVectoredExceptionHandler(h) != 0, "RemoveVectoredExceptionHandler");
  __try {
    RaiseException(0xE0000006, 0, 0, NULL);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  check(g_vectored_seen == 1, "a removed handler is not called");

  h = AddVectoredExceptionHandler(1, vectored_skip_instruction);
  RaiseException(0xE0000007, 0, 0, NULL);      /* continuable: the vectored handler lets it go on */
  check(1, "EXCEPTION_CONTINUE_EXECUTION from a vectored handler resumes after RaiseException");
  RemoveVectoredExceptionHandler(h);
}

static int g_fixed;
static LONG WINAPI top_level(PEXCEPTION_POINTERS p) {
  if (p->ExceptionRecord->ExceptionCode == 0xE0000009) {
    g_fixed = 2;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void test_top_level_filter(void) {
  LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(top_level);
  g_fixed = 0;
  RaiseException(0xE0000009, 0, 0, NULL);
  check(g_fixed == 2, "the unhandled-exception filter can continue the program");
  SetUnhandledExceptionFilter(old);
}

static int compare_raising(const void *a, const void *b) {
  if (*(const int *)a == 5 || *(const int *)b == 5) RaiseException(0xE000000A, 0, 0, NULL);
  return *(const int *)a - *(const int *)b;
}
static void test_through_the_runtime(void) {
  int values[] = {9, 3, 5, 1, 7};
  int caught = 0;
  __try {
    qsort(values, 5, sizeof(int), compare_raising);
  } __except (GetExceptionCode() == 0xE000000A ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    caught = 1;
  }
  check(caught, "an exception unwinds through qsort of msvcrt.dll");
}

static int recurse(int depth) {
  volatile char pad[128];
  pad[0] = (char)depth;
  if (depth == 0) { RaiseException(0xE000000B, 0, 0, NULL); return 0; }
  return recurse(depth - 1) + pad[0];
}
static void test_deep(void) {
  int caught = 0;
  __try {
    recurse(500);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    caught = 1;
  }
  check(caught, "an exception thrown 500 frames deep");
}

static void test_noncontinuable(void) {
  int code = 0;
  __try {
    RaiseException(0xE000000C, EXCEPTION_NONCONTINUABLE, 0, NULL);
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(code == (int)0xE000000C, "a noncontinuable exception");
  int second = 0;
  __try {
    __try {
      RaiseException(0xE000000D, EXCEPTION_NONCONTINUABLE, 0, NULL);
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
    }
  } __except (second = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  check(second == (int)0xC0000025, "continuing a noncontinuable exception raises STATUS_NONCONTINUABLE_EXCEPTION");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("sehtest starting\n");
  test_raise();
  test_faults();
  test_unwind_order();
  test_finally();
  test_nested();
  test_vectored();
  test_top_level_filter();
  test_through_the_runtime();
  test_deep();
  test_noncontinuable();
  printf("sehtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

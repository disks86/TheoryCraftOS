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

/* A dynamically linked Linux program (built with the development machine's C compiler and C library, not static): the kernel maps it and its
 * interpreter (ld.so), which loads libc.so.6, libm and a library opened at run time with dlopen, by open and mmap of the files (regular files
 * mapped as copies), and relocates them. Then the C library itself is used: stdio, malloc, threads, thread-local storage, a C++-style constructor.
 * Run by tests/linux_dynamic_emulated.sh on the kernel's CPU emulator. Prints "dynamic: N checks, F failed". */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int constructed;
__attribute__((constructor)) static void early(void) { constructed = 42; }

static __thread int per_thread = 7;
static void* worker(void* arg) {
  per_thread += (int)(long)arg;
  return (void*)(long)per_thread;
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);      /* (a fault later must not lose what was printed) */
  CHECK(constructed == 42);
  char text[64];
  snprintf(text, sizeof text, "%d-%s-%.2f", 12, "abc", 2.5);
  CHECK(strcmp(text, "12-abc-2.50") == 0);
  volatile double two = 2.0;
  CHECK(fabs(sqrt(two) - 1.41421356) < 1e-6);      /* libm */
  char* big = malloc(1 << 20);
  CHECK(big != NULL);
  if (big) { memset(big, 0x5A, 1 << 20); CHECK(big[(1 << 20) - 1] == 0x5A); free(big); }
  pthread_t threads[4];
  int started = 0;
  for (long i = 0; i < 4; ++i) {
    const int e = pthread_create(&threads[i], NULL, worker, (void*)i);
    CHECK(e == 0);
    if (e != 0) { printf("pthread_create: %s\n", strerror(e)); break; }
    ++started;
  }
  for (int i = 0; i < started; ++i) { void* r = NULL; CHECK(pthread_join(threads[i], &r) == 0); CHECK((long)r == 7 + i); }
  CHECK(per_thread == 7);
  /* dlopen of a library the program does not link against: libm is already there, so open it by name and find a symbol */
  void* m = dlopen("libm.so.6", RTLD_NOW);
  CHECK(m != NULL);
  if (m) {
    double (*cosine)(double) = (double (*)(double))dlsym(m, "cos");
    CHECK(cosine != NULL && fabs(cosine(0.0) - 1.0) < 1e-12);
    dlclose(m);
  }
  CHECK(dlopen("libdoesnotexist.so.9", RTLD_NOW) == NULL);
  printf("dynamic: %d checks, %d failed\n", checks, failed);
  fflush(stdout);
  return failed ? 1 : 0;
}

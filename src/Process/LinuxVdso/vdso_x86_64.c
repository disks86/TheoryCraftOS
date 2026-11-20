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
// The x86-64 Linux vDSO of TheoryCraftOS: clock_gettime, gettimeofday, time and clock_getres without a system call. Built freestanding into a small
// shared object (tools/build_linux_vdso.sh, which writes LinuxVdsoImage.inc); the kernel maps it into every x86-64 Linux process and names it in
// AT_SYSINFO_EHDR, where glibc and musl look for these functions (version LINUX_2.6, the names Linux uses). The page below the image is the kernel's
// data (vdso_data.h). Whatever the vDSO cannot answer it passes to the kernel with the same system call the C library would have made.
#include "vdso_data.h"

// The data page: just below the image (the linker script places the symbol there; hidden, so it is reached RIP-relative, with no relocation).
extern const struct tcos_vdso_data tcos_vvar __attribute__((visibility("hidden")));

struct tcos_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct tcos_timeval { int64_t tv_sec; int64_t tv_usec; };

static inline long tcos_syscall2(long number, long a, long b) {
  long result;
  __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
  return result;
}

static inline uint64_t tcos_rdtsc(void) {      // (as the kernel reads it: the two clocks see the same counter)
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

// The data is written before the program starts and never changes, but read it through a volatile view so the compiler cannot assume anything about it.
static inline const struct tcos_vdso_data* tcos_data(void) {
  const struct tcos_vdso_data* data = &tcos_vvar;
  __asm__("" : "+r"(data));
  return data;
}

enum { kSysGettimeofday = 96, kSysTime = 201, kSysClockGettime = 228, kSysClockGetres = 229 };

int __vdso_clock_gettime(int clock, struct tcos_timespec* ts) {
  const struct tcos_vdso_data* data = tcos_data();
  uint64_t ns = 0;
  if (data->tsc_ticks_per_us == 0 || !tcos_vdso_clock_ns(data, clock, tcos_rdtsc(), &ns)) return (int)tcos_syscall2(kSysClockGettime, clock, (long)ts);
  ts->tv_sec = (int64_t)(ns / 1000000000u);
  ts->tv_nsec = (int64_t)(ns % 1000000000u);
  return 0;
}

int __vdso_gettimeofday(struct tcos_timeval* tv, void* tz) {
  const struct tcos_vdso_data* data = tcos_data();
  uint64_t ns = 0;
  // (a time zone asked for goes to the kernel, so that what is written there is whatever the kernel writes)
  if (tz || data->tsc_ticks_per_us == 0 || !tcos_vdso_clock_ns(data, TCOS_CLOCK_REALTIME, tcos_rdtsc(), &ns)) return (int)tcos_syscall2(kSysGettimeofday, (long)tv, (long)tz);
  if (tv) {
    tv->tv_sec = (int64_t)(ns / 1000000000u);
    tv->tv_usec = (int64_t)(ns % 1000000000u / 1000u);
  }
  return 0;
}

int64_t __vdso_time(int64_t* t) {
  const struct tcos_vdso_data* data = tcos_data();
  uint64_t ns = 0;
  if (data->tsc_ticks_per_us == 0 || !tcos_vdso_clock_ns(data, TCOS_CLOCK_REALTIME, tcos_rdtsc(), &ns)) return tcos_syscall2(kSysTime, (long)t, 0);
  const int64_t seconds = (int64_t)(ns / 1000000000u);
  if (t) *t = seconds;
  return seconds;
}

// Every clock the kernel knows (0..11) counts in nanoseconds; anything else is the kernel's to refuse.
int __vdso_clock_getres(int clock, struct tcos_timespec* res) {
  if (clock < 0 || clock > 11) return (int)tcos_syscall2(kSysClockGetres, clock, (long)res);
  if (res) {
    res->tv_sec = 0;
    res->tv_nsec = 1;
  }
  return 0;
}

// The plain names, as Linux's vDSO also exports them.
int clock_gettime(int, struct tcos_timespec*) __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct tcos_timeval*, void*) __attribute__((weak, alias("__vdso_gettimeofday")));
int64_t time(int64_t*) __attribute__((weak, alias("__vdso_time")));
int clock_getres(int, struct tcos_timespec*) __attribute__((weak, alias("__vdso_clock_getres")));

// The note Linux's vDSO carries: the kernel version (6.1.0, what uname reports). Old glibc read it to check the kernel is new enough.
__asm__(".pushsection .note.Linux, \"a\", @note\n"
        ".balign 4\n"
        ".long 6, 4, 0\n"
        ".asciz \"Linux\"\n"
        ".balign 4\n"
        ".long 0x060100\n"
        ".popsection\n");

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

// The vDSO on the real kernel (tests/vdso_qemu.sh). Checks that the kernel names a vDSO in AT_SYSINFO_EHDR, that it exports clock_gettime, gettimeofday,
// time and clock_getres in version LINUX_2.6 (looked up here the way musl does), that what it and the C library's functions return agrees with the system
// calls read just before and after, in this process, a thread and a forked child, and that a clock read is much cheaper than the system call (on
// TheoryCraftOS the data page under the vDSO must also publish a counter rate, so that the vDSO answers without the kernel).
// Prints "vdsotest: N passed, 0 failed" and exits with 7 when everything passed. `vdsotest --then <program>` runs <program> afterwards (the dynamically
// linked build) and needs it to exit with 7 as well. Also runs on Linux itself (the TheoryCraftOS-only check is then skipped).
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static const char* tag = "vdsotest";
#define CHECK(cond, what) do { if (cond) ++passed; else { ++failed; printf("%s: FAILED %s (line %d)\n", tag, what, __LINE__); } } while (0)

typedef int (*clock_gettime_fn)(clockid_t, struct timespec*);
typedef int (*gettimeofday_fn)(struct timeval*, void*);
typedef time_t (*time_fn)(time_t*);
typedef int (*clock_getres_fn)(clockid_t, struct timespec*);

// musl's lookup: DT_HASH for the symbol count, the version from DT_VERSYM / DT_VERDEF.
static void* vdso_symbol(uintptr_t ehdr_address, const char* version, const char* name) {
  const Elf64_Ehdr* eh = (const Elf64_Ehdr*)ehdr_address;
  const Elf64_Phdr* ph = (const Elf64_Phdr*)(ehdr_address + eh->e_phoff);
  const Elf64_Dyn* dyn = 0;
  uintptr_t base = (uintptr_t)-1;
  for (int i = 0; i < eh->e_phnum; ++i) {
    if (ph[i].p_type == PT_LOAD) base = ehdr_address + ph[i].p_offset - ph[i].p_vaddr;
    else if (ph[i].p_type == PT_DYNAMIC) dyn = (const Elf64_Dyn*)(ehdr_address + ph[i].p_offset);
  }
  if (!dyn || base == (uintptr_t)-1) return 0;
  const char* strings = 0; const Elf64_Sym* syms = 0; const uint32_t* hash = 0; const uint16_t* versym = 0; const Elf64_Verdef* verdef = 0;
  for (; dyn->d_tag; ++dyn) {
    const uintptr_t p = base + dyn->d_un.d_ptr;
    if (dyn->d_tag == DT_STRTAB) strings = (const char*)p;
    else if (dyn->d_tag == DT_SYMTAB) syms = (const Elf64_Sym*)p;
    else if (dyn->d_tag == DT_HASH) hash = (const uint32_t*)p;
    else if (dyn->d_tag == DT_VERSYM) versym = (const uint16_t*)p;
    else if (dyn->d_tag == DT_VERDEF) verdef = (const Elf64_Verdef*)p;
  }
  if (!strings || !syms || !hash) return 0;
  for (uint32_t i = 0; i < hash[1]; ++i) {
    const int type = ELF64_ST_TYPE(syms[i].st_info), bind = ELF64_ST_BIND(syms[i].st_info);
    if ((type != STT_FUNC && type != STT_NOTYPE && type != STT_OBJECT) || (bind != STB_GLOBAL && bind != STB_WEAK) || !syms[i].st_shndx) continue;
    if (strcmp(name, strings + syms[i].st_name)) continue;
    if (versym && verdef) {
      const Elf64_Verdef* def = verdef;
      int found = 0;
      for (;;) {
        if (!(def->vd_flags & VER_FLG_BASE) && (def->vd_ndx & 0x7fff) == (versym[i] & 0x7fff)) {
          const Elf64_Verdaux* aux = (const Elf64_Verdaux*)((const char*)def + def->vd_aux);
          found = !strcmp(version, strings + aux->vda_name);
          break;
        }
        if (!def->vd_next) break;
        def = (const Elf64_Verdef*)((const char*)def + def->vd_next);
      }
      if (!found) continue;
    }
    return (void*)(base + syms[i].st_value);
  }
  return 0;
}

static uint64_t ns_of(struct timespec t) { return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t us_of(struct timeval t) { return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_usec; }
static uint64_t sys_clock(clockid_t clock) { struct timespec t = {0, 0}; syscall(SYS_clock_gettime, clock, &t); return ns_of(t); }

static clock_gettime_fn v_clock_gettime;
static gettimeofday_fn v_gettimeofday;
static time_fn v_time;
static clock_getres_fn v_clock_getres;

// The vDSO's answer and the C library's both lie between two system calls of the same clock.
static void check_agreement(const char* where) {
  static const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME};
  static const char* const names[] = {"CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW", "CLOCK_BOOTTIME"};
  char what[128];
  for (int i = 0; i < 4; ++i) {
    struct timespec v = {0, 0}, g = {0, 0};
    const uint64_t before = sys_clock(clocks[i]);
    const int vr = v_clock_gettime(clocks[i], &v);
    const int gr = clock_gettime(clocks[i], &g);
    const uint64_t after = sys_clock(clocks[i]);
    snprintf(what, sizeof(what), "%s: %s between system calls (%llu <= %llu <= %llu <= %llu)", where, names[i], (unsigned long long)before,
             (unsigned long long)ns_of(v), (unsigned long long)ns_of(g), (unsigned long long)after);
    CHECK(vr == 0 && gr == 0 && v.tv_nsec >= 0 && v.tv_nsec < 1000000000 && before <= ns_of(v) && ns_of(v) <= ns_of(g) && ns_of(g) <= after, what);
  }
  struct timeval sb = {0, 0}, v = {0, 0}, g = {0, 0}, sa = {0, 0};
  syscall(SYS_gettimeofday, &sb, 0);
  const int vr = v_gettimeofday(&v, 0);
  const int gr = gettimeofday(&g, 0);
  syscall(SYS_gettimeofday, &sa, 0);
  snprintf(what, sizeof(what), "%s: gettimeofday between system calls", where);
  CHECK(vr == 0 && gr == 0 && v.tv_usec >= 0 && v.tv_usec < 1000000 && us_of(sb) <= us_of(v) && us_of(v) <= us_of(g) && us_of(g) <= us_of(sa), what);
  const time_t tb = (time_t)syscall(SYS_time, 0);
  time_t stored = 0;
  const time_t tv = v_time(&stored), tg = time(0);
  const time_t ta = (time_t)syscall(SYS_time, 0);
  snprintf(what, sizeof(what), "%s: time between system calls", where);
  CHECK(tv == stored && tb <= tv && tv <= tg && tg <= ta, what);
}

static void* thread_main(void* arg) {
  (void)arg;
  check_agreement("thread");
  return 0;
}

static double seconds_for(int use_syscall, int count) {
  struct timespec t, start, end;
  syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &start);
  for (int i = 0; i < count; ++i) {
    if (use_syscall) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t);
    else clock_gettime(CLOCK_MONOTONIC, &t);
  }
  syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &end);
  return (double)(ns_of(end) - ns_of(start)) / 1e9;
}

int main(int argc, char** argv) {
  setvbuf(stdout, 0, _IONBF, 0);
  if (argc >= 2 && !strcmp(argv[1], "--dynamic")) tag = "vdsotest-dynamic";
  const uintptr_t ehdr = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
  CHECK(ehdr != 0 && !memcmp((const void*)ehdr, ELFMAG, SELFMAG), "AT_SYSINFO_EHDR names an ELF image");
  if (!ehdr) { printf("%s: %d passed, %d failed\n", tag, passed, failed); return 1; }
  v_clock_gettime = (clock_gettime_fn)vdso_symbol(ehdr, "LINUX_2.6", "__vdso_clock_gettime");
  v_gettimeofday = (gettimeofday_fn)vdso_symbol(ehdr, "LINUX_2.6", "__vdso_gettimeofday");
  v_time = (time_fn)vdso_symbol(ehdr, "LINUX_2.6", "__vdso_time");
  v_clock_getres = (clock_getres_fn)vdso_symbol(ehdr, "LINUX_2.6", "__vdso_clock_getres");
  CHECK(v_clock_gettime && v_gettimeofday && v_time && v_clock_getres, "the vDSO exports the four functions in LINUX_2.6");
  if (!v_clock_gettime || !v_gettimeofday || !v_time || !v_clock_getres) { printf("%s: %d passed, %d failed\n", tag, passed, failed); return 1; }

  // TheoryCraftOS: the data page right below the image ("TCTCVSDO", version, flags, rate, start, offset) publishes a rate, so the vDSO needs no kernel.
  // (only there: on Linux the pages below the vDSO are its own vvar area, parts of which fault when touched)
  struct utsname names;
  const int ours = uname(&names) == 0 && strstr(names.version, "TheoryCraftOS") != 0;
  if (ours) {
    const uint64_t* data = (const uint64_t*)(ehdr - 4096);
    CHECK(!memcmp(data, "TCTCVSDO", 8), "the TheoryCraftOS data page is below the vDSO");
    printf("%s: TheoryCraftOS vDSO at %#lx, counter %llu ticks per microsecond\n", tag, (unsigned long)ehdr, (unsigned long long)data[2]);
    CHECK(data[2] != 0, "the kernel published the counter rate (clock reads stay in user mode)");
  }

  check_agreement("main");
  // Monotonic: never goes back over many reads.
  struct timespec previous = {0, 0}, now;
  int backwards = 0;
  for (int i = 0; i < 200000; ++i) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (ns_of(now) < ns_of(previous)) ++backwards;
    previous = now;
  }
  CHECK(backwards == 0, "CLOCK_MONOTONIC never goes back");
  // What the kernel answers: the CPU-time clocks, an unknown clock (EINVAL), a resolution; the same through the vDSO and the system call.
  struct timespec cpu = {-1, -1};
  CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0 && cpu.tv_sec >= 0 && v_clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0, "the CPU-time clocks");
  errno = 0;
  CHECK(clock_gettime((clockid_t)99, &now) == -1 && errno == EINVAL && v_clock_gettime((clockid_t)99, &now) == -EINVAL, "an unknown clock is EINVAL");
  int same = 1;
  for (int c = -2; c <= 13; ++c) {
    struct timespec a = {7, 7}, b = {7, 7};
    const long sr = syscall(SYS_clock_getres, c, &a);
    const int vr = v_clock_getres(c, &b);
    if ((sr == 0) != (vr == 0) || (sr == 0 && (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec))) { same = 0; printf("%s: clock_getres(%d) %ld/%d\n", tag, c, sr, vr); }
  }
  CHECK(same, "clock_getres agrees with the system call for every clock");

  pthread_t thread;
  CHECK(pthread_create(&thread, 0, thread_main, 0) == 0 && pthread_join(thread, 0) == 0, "a thread reads the clocks");
  const pid_t child = fork();
  if (child == 0) {
    passed = failed = 0;
    tag = "vdsotest-child";
    check_agreement("child");
    _exit(failed ? 1 : 0);
  }
  int status = -1;
  CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "a forked child reads the clocks");

  // Cost: a clock read through the C library against the bare system call.
  const int count = 100000;
  const double fast = seconds_for(0, count), slow = seconds_for(1, count);
  printf("%s: clock_gettime %.0f ns per call, the system call %.0f ns (%.1fx)\n", tag, fast * 1e9 / count, slow * 1e9 / count, fast > 0 ? slow / fast : 0.0);
  CHECK(fast * 4 < slow, "clock_gettime is at least four times cheaper than the system call");

  if (argc >= 3 && !strcmp(argv[1], "--then")) {
    const pid_t next = fork();
    if (next == 0) {
      char* const args[] = {argv[2], (char*)"--dynamic", 0};
      execv(argv[2], args);
      _exit(127);
    }
    int code = -1;
    CHECK(next > 0 && waitpid(next, &code, 0) == next && WIFEXITED(code) && WEXITSTATUS(code) == 7, "the dynamically linked build passed");
  }
  printf("%s: %d passed, %d failed\n", tag, passed, failed);
  return failed ? 1 : 7;
}

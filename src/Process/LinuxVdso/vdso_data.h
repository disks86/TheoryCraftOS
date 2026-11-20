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
// The page the kernel shares with the Linux vDSO (vvar), and how the vDSO turns it and the timestamp counter into the time. This header is C as well as
// C++: the vDSO (vdso_x86_64.c, built freestanding by tools/build_linux_vdso.sh) and the kernel (LinuxVdso.cpp) both include it, so the two can never
// compute the time differently. It sits in the page just below the vDSO's first byte (the vDSO reaches it by a fixed offset from its own code).
#pragma once
#include <stdint.h>

#define TCOS_VDSO_MAGIC 0x4F44535643544354ull      /* "TCTCVSDO" read as a little-endian word */
#define TCOS_VDSO_VERSION 1u

// All fields are written once, before the program's first instruction, and never change while it runs (the clock's rate and origin are fixed at boot).
struct tcos_vdso_data {
  uint64_t magic;                 // TCOS_VDSO_MAGIC
  uint32_t version;               // TCOS_VDSO_VERSION
  uint32_t flags;                 // none yet (0)
  uint64_t tsc_ticks_per_us;      // timestamp counter ticks per microsecond; 0: the program must ask the kernel (every call is a system call)
  uint64_t tsc_at_start;          // the counter at the kernel's time 0 (CLOCK_MONOTONIC counts from there)
  uint64_t realtime_offset_ns;    // CLOCK_REALTIME = CLOCK_MONOTONIC + this, modulo 2^64
};
#define TCOS_VDSO_DATA_MAGIC_AT 0
#define TCOS_VDSO_DATA_VERSION_AT 8
#define TCOS_VDSO_DATA_FLAGS_AT 12
#define TCOS_VDSO_DATA_RATE_AT 16
#define TCOS_VDSO_DATA_START_AT 24
#define TCOS_VDSO_DATA_OFFSET_AT 32
#define TCOS_VDSO_DATA_BYTES 40

// Linux clock ids the vDSO answers itself.
#define TCOS_CLOCK_REALTIME 0
#define TCOS_CLOCK_MONOTONIC 1
#define TCOS_CLOCK_MONOTONIC_RAW 4
#define TCOS_CLOCK_REALTIME_COARSE 5
#define TCOS_CLOCK_MONOTONIC_COARSE 6
#define TCOS_CLOCK_BOOTTIME 7
#define TCOS_CLOCK_TAI 11
#define TCOS_TAI_OFFSET_NS 37000000000ull      // 37 leap seconds ahead of UTC (the kernel's clock_gettime adds the same)

// CLOCK_MONOTONIC in nanoseconds for counter value `tsc`: whole microseconds, exactly as the kernel's monotonic_ns() (hardware::uptime_us() * 1000).
// A counter below the start (another core's counter a little behind) reads as 0, never as a time far in the future. Requires tsc_ticks_per_us != 0.
static inline uint64_t tcos_vdso_monotonic_ns(const struct tcos_vdso_data* data, uint64_t tsc) {
  const uint64_t start = data->tsc_at_start;
  return (tsc > start ? tsc - start : 0) / data->tsc_ticks_per_us * 1000u;
}

// The time of Linux clock `clock` for counter value `tsc`, in nanoseconds; 0 (false) when the vDSO cannot answer (no rate published, or a clock it does
// not keep: the CPU-time clocks, unknown or dynamic ids), and the call has to go to the kernel.
static inline int tcos_vdso_clock_ns(const struct tcos_vdso_data* data, int clock, uint64_t tsc, uint64_t* ns) {
  if (data->tsc_ticks_per_us == 0) return 0;
  switch (clock) {
    case TCOS_CLOCK_MONOTONIC: case TCOS_CLOCK_MONOTONIC_RAW: case TCOS_CLOCK_MONOTONIC_COARSE: case TCOS_CLOCK_BOOTTIME:
      *ns = tcos_vdso_monotonic_ns(data, tsc);
      return 1;
    case TCOS_CLOCK_REALTIME: case TCOS_CLOCK_REALTIME_COARSE:
      *ns = tcos_vdso_monotonic_ns(data, tsc) + data->realtime_offset_ns;
      return 1;
    case TCOS_CLOCK_TAI:
      *ns = tcos_vdso_monotonic_ns(data, tsc) + data->realtime_offset_ns + TCOS_TAI_OFFSET_NS;
      return 1;
    default:
      return 0;
  }
}

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
// Host test of the pacing of page redraws (src/Desktop/RefreshPacing.hpp).
#include <stdio.h>

#include "Desktop/RefreshPacing.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

int main() {
  using desktop::next_refresh_after;
  // a page that draws fast is drawn ten times a second, as before
  CHECK(next_refresh_after(1000, 1000, 100, 1000) == 1100);
  CHECK(next_refresh_after(1000, 1010, 100, 1000) == 1100);
  CHECK(next_refresh_after(1000, 1033, 100, 1000) == 1100);
  // a drawing that takes long leaves the scripts twice its time before the next one
  CHECK(next_refresh_after(1000, 1050, 100, 1000) == 1150);
  CHECK(next_refresh_after(1000, 1300, 100, 1000) == 1900);
  CHECK(next_refresh_after(1000, 1499, 100, 1000) == 2497);
  // ... but never more than the longest wait
  CHECK(next_refresh_after(1000, 1500, 100, 1000) == 2500);
  CHECK(next_refresh_after(1000, 4000, 100, 1000) == 5000);
  // a clock that did not move, or went back, counts as a drawing that took no time
  CHECK(next_refresh_after(5000, 4000, 100, 1000) == 5100);
  CHECK(next_refresh_after(0, 0, 100, 1000) == 100);
  // the share of the scripts: with a drawing of 300 ms and steps of 5 ms, the scripts get 120 steps between two drawings, not one
  {
    const uint64_t started = 10000, ended = 10300, next = next_refresh_after(started, ended, 100, 1000);
    CHECK((next - ended) / 5 == 120);
  }
  // it is a constant expression (no state, usable for limits)
  static_assert(next_refresh_after(0, 300, 100, 1000) == 900, "pacing");
  printf(g_failed ? "%d check(s) failed\n" : "refresh pacing tests passed\n", g_failed);
  return g_failed ? 1 : 0;
}

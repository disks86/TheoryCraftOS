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
#pragma once
// How often a page that keeps changing is drawn again. Scripts and drawing share the page's thread: a page whose scripts work in small steps from timers, each
// step changing the page (a big site while it loads), must not get just one step between two drawings when a drawing takes long.

#include <stdint.h>

namespace desktop {

// The earliest time (ms) a page that timers keep changing is drawn again, after a drawing that began at `started_ms` and was done at `ended_ms`:
// `min_gap_ms` after the last one began (a page that draws fast: ten times a second), and, after a drawing that took long, not before the scripts had
// twice the time the drawing took - but no longer than `max_wait_ms`, so that a very slow page is still seen to change.
constexpr uint64_t next_refresh_after(uint64_t started_ms, uint64_t ended_ms, uint64_t min_gap_ms, uint64_t max_wait_ms) {
  const uint64_t cost = ended_ms > started_ms ? ended_ms - started_ms : 0;
  const uint64_t wait = cost < max_wait_ms / 2 ? cost * 2 : max_wait_ms;
  const uint64_t done = ended_ms > started_ms ? ended_ms : started_ms;
  const uint64_t by_gap = started_ms + min_gap_ms, by_cost = done + wait;
  return by_gap > by_cost ? by_gap : by_cost;
}

}  // namespace desktop

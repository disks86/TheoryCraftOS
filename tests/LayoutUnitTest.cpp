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

// Host-side tests for layout::LayoutUnit, the fixed-point number layout is done in.
#include <stdio.h>
#include "Layout/LayoutUnit.hpp"

using layout::LayoutUnit;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static void test_conversions() {
  CHECK(LayoutUnit().raw() == 0 && LayoutUnit().is_zero());
  CHECK(LayoutUnit::from_px(1).raw() == 64 && LayoutUnit::from_px(-3).raw() == -192);
  CHECK(LayoutUnit::from_px100(100).raw() == 64);          // 1px
  CHECK(LayoutUnit::from_px100(50).raw() == 32);           // 0.5px
  CHECK(LayoutUnit::from_px100(1600).floor() == 16);       // the old line height
  CHECK(LayoutUnit::from_px100(-1600).floor() == -16);
  CHECK(LayoutUnit::from_px100(1).raw() == 1);             // 0.01px is 0.64/64 rounded to 1
  CHECK(LayoutUnit::from_px100(-1).raw() == -1);
  CHECK(LayoutUnit::from_px100(100).to_px100() == 100);
  CHECK(LayoutUnit::from_px100(1234).to_px100() == 1234);  // 12.34px survives the trip within a 1/64 step
  CHECK(LayoutUnit::from_raw(96).to_double() == 1.5);
}

static void test_rounding() {
  const LayoutUnit a = LayoutUnit::from_raw(64 * 3 + 20);   // 3.3125
  CHECK(a.floor() == 3 && a.ceil() == 4 && a.round() == 3);
  const LayoutUnit b = LayoutUnit::from_raw(64 * 3 + 32);   // 3.5
  CHECK(b.floor() == 3 && b.ceil() == 4 && b.round() == 4);
  const LayoutUnit c = LayoutUnit::from_raw(-(64 * 3 + 20));
  CHECK(c.floor() == -4 && c.ceil() == -3 && c.round() == -3);
  CHECK(LayoutUnit::from_px(7).floor() == 7 && LayoutUnit::from_px(7).ceil() == 7 && LayoutUnit::from_px(7).round() == 7);
  CHECK(LayoutUnit::max().ceil() > 0 && LayoutUnit::max().round() > 0);   // no wrap at the top of the range
}

static void test_saturation() {
  const LayoutUnit big = LayoutUnit::from_px(30000000);
  CHECK((big + big).is_max() && (-big - big).is_min());
  CHECK((LayoutUnit::max() + LayoutUnit::from_px(1)).is_max());
  CHECK((LayoutUnit::min() - LayoutUnit::from_px(1)).is_min());
  CHECK((-LayoutUnit::min()).is_max() && LayoutUnit::min().abs().is_max());
  CHECK(LayoutUnit::from_px(1000000000).is_max() && LayoutUnit::from_px(-1000000000).is_min());
  CHECK(LayoutUnit::from_px100(2000000000000ll).is_max());
  CHECK((LayoutUnit::from_px(100000) * 100000).is_max());
  CHECK((LayoutUnit::from_px(5) * 3) == LayoutUnit::from_px(15) && (3 * LayoutUnit::from_px(5)) == LayoutUnit::from_px(15));
  CHECK((LayoutUnit::from_px(10) / 4) == LayoutUnit::from_raw(160) && (LayoutUnit::from_px(10) / 0).is_zero());
  CHECK((LayoutUnit::from_px(10) / -2) == LayoutUnit::from_px(-5));
}

static void test_scaled() {
  const LayoutUnit w = LayoutUnit::from_px(800);
  CHECK(w.percent(5000) == LayoutUnit::from_px(400));      // 50%
  CHECK(w.percent(2500) == LayoutUnit::from_px(200));      // 25%
  CHECK(w.percent(33333).raw() == 170666 || w.percent(33333).raw() == 170665);   // 333.33%: rounded, never wrapped
  CHECK(LayoutUnit::from_px(10).scaled(1, 3).raw() == 213);  // 10px/3 = 3.3333px = 213.33/64
  CHECK(LayoutUnit::from_px(10).scaled(-1, 3).raw() == -213);
  CHECK(LayoutUnit::from_px(10).scaled(1, -3).raw() == -213);
  CHECK(LayoutUnit::from_px(10).scaled(5, 0).is_zero());
  CHECK(LayoutUnit::max().scaled(2, 1).is_max() && LayoutUnit::min().scaled(2, 1).is_min());
  CHECK(LayoutUnit::max().scaled(INT32_MAX, 1).is_max());
  // flex shares add back up: 100px split 1:1:1 loses at most one unit to rounding
  const LayoutUnit share = LayoutUnit::from_px(100).scaled(1, 3);
  CHECK((share * 3 - LayoutUnit::from_px(100)).abs().raw() <= 2);
}

static void test_compare() {
  const LayoutUnit a = LayoutUnit::from_px(2), b = LayoutUnit::from_px(3);
  CHECK(a < b && b > a && a <= a && a >= a && a != b && a == LayoutUnit::from_px(2));
  CHECK(layout::min(a, b) == a && layout::max(a, b) == b);
  CHECK(layout::clamp(LayoutUnit::from_px(9), a, b) == b && layout::clamp(LayoutUnit::from_px(1), a, b) == a);
  LayoutUnit c = a;
  c += b;
  CHECK(c == LayoutUnit::from_px(5));
  c -= a;
  CHECK(c == b);
}

int main() {
  test_conversions();
  test_rounding();
  test_saturation();
  test_scaled();
  test_compare();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("layout unit tests passed\n");
  return 0;
}

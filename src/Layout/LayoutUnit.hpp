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

// The number layout is done in: CSS pixels as a signed fixed-point value with six fractional bits (1/64 px). Lengths that come out of
// the style sheet are hundredths of a pixel (browser::Length::px100); from_px100() converts them. Arithmetic saturates instead of
// wrapping, so a page that asks for absurd sizes (width:99999999px, margins that add up past the range) cannot overflow a size or an
// offset - it gets the largest or smallest value instead - and the usual "is this box wider than that" comparisons stay meaningful.
//
// The range is about +-33.5 million pixels. kMax and kMin are what "unbounded" means (an unconstrained available width).
#pragma once
#include <stdint.h>

namespace layout {

class LayoutUnit {
 public:
  static constexpr int kFractionBits = 6;
  static constexpr int32_t kOne = 1 << kFractionBits;
  static constexpr int32_t kRawMax = INT32_MAX;
  static constexpr int32_t kRawMin = INT32_MIN;

  constexpr LayoutUnit() = default;
  static constexpr LayoutUnit from_raw(int32_t raw) { return LayoutUnit(raw); }
  // Whole pixels (saturating).
  static constexpr LayoutUnit from_px(int64_t px) { return LayoutUnit(clamp64(px * kOne)); }
  // Hundredths of a pixel, the unit of browser::Length (rounded to the nearest 1/64 px, halves away from zero).
  static constexpr LayoutUnit from_px100(int64_t px100) {
    const int64_t scaled = px100 * kOne;
    const int64_t half = scaled >= 0 ? 50 : -50;
    return LayoutUnit(clamp64((scaled + half) / 100));
  }
  static constexpr LayoutUnit max() { return LayoutUnit(kRawMax); }
  static constexpr LayoutUnit min() { return LayoutUnit(kRawMin); }

  constexpr int32_t raw() const { return raw_; }
  constexpr bool is_max() const { return raw_ == kRawMax; }
  constexpr bool is_min() const { return raw_ == kRawMin; }

  // To whole pixels.
  constexpr int32_t floor() const { return raw_ >> kFractionBits; }
  constexpr int32_t ceil() const {
    return raw_ > kRawMax - (kOne - 1) ? (kRawMax >> kFractionBits) : (raw_ + (kOne - 1)) >> kFractionBits;
  }
  constexpr int32_t round() const {
    return raw_ > kRawMax - kOne / 2 ? (kRawMax >> kFractionBits) : (raw_ + kOne / 2) >> kFractionBits;
  }
  // To hundredths of a pixel (what Length holds and getComputedStyle prints).
  constexpr int64_t to_px100() const { return (static_cast<int64_t>(raw_) * 100 + (raw_ >= 0 ? kOne / 2 : -kOne / 2)) / kOne; }
  constexpr double to_double() const { return static_cast<double>(raw_) / kOne; }

  constexpr bool is_zero() const { return raw_ == 0; }
  constexpr LayoutUnit abs() const { return raw_ < 0 ? (raw_ == kRawMin ? max() : LayoutUnit(-raw_)) : *this; }

  friend constexpr LayoutUnit operator+(LayoutUnit a, LayoutUnit b) { return LayoutUnit(clamp64(static_cast<int64_t>(a.raw_) + b.raw_)); }
  friend constexpr LayoutUnit operator-(LayoutUnit a, LayoutUnit b) { return LayoutUnit(clamp64(static_cast<int64_t>(a.raw_) - b.raw_)); }
  constexpr LayoutUnit operator-() const { return raw_ == kRawMin ? max() : LayoutUnit(-raw_); }
  LayoutUnit& operator+=(LayoutUnit o) { return *this = *this + o; }
  LayoutUnit& operator-=(LayoutUnit o) { return *this = *this - o; }

  // Times a whole number (a count of lines, columns, items), saturating.
  friend constexpr LayoutUnit operator*(LayoutUnit a, int32_t n) { return LayoutUnit(clamp64(static_cast<int64_t>(a.raw_) * n)); }
  friend constexpr LayoutUnit operator*(int32_t n, LayoutUnit a) { return a * n; }
  // Divided by a whole number (rounded toward zero); dividing by 0 gives 0.
  friend constexpr LayoutUnit operator/(LayoutUnit a, int32_t n) { return n == 0 ? LayoutUnit() : LayoutUnit(clamp64(static_cast<int64_t>(a.raw_) / n)); }

  // a * numerator / denominator without losing precision on the way (percentages, flex shares, grid fr): rounded to nearest, halves
  // away from zero. A zero denominator gives 0. (Both are 32-bit so the product fits 64 bits: the image has no 128-bit division.)
  constexpr LayoutUnit scaled(int32_t numerator, int32_t denominator) const {
    if (denominator == 0) return LayoutUnit();
    const int64_t product = static_cast<int64_t>(raw_) * numerator;
    const int64_t d = denominator;
    const int64_t half = (d < 0 ? -d : d) / 2;
    const int64_t adjusted = (product >= 0) == (d > 0) ? product + half : product - half;
    return LayoutUnit(clamp64(adjusted / d));
  }
  // A percentage of this value (percent in hundredths: 1250 is 12.5%).
  constexpr LayoutUnit percent(int32_t percent_x100) const { return scaled(percent_x100, 10000); }

  friend constexpr bool operator==(LayoutUnit a, LayoutUnit b) { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(LayoutUnit a, LayoutUnit b) { return a.raw_ != b.raw_; }
  friend constexpr bool operator<(LayoutUnit a, LayoutUnit b) { return a.raw_ < b.raw_; }
  friend constexpr bool operator>(LayoutUnit a, LayoutUnit b) { return a.raw_ > b.raw_; }
  friend constexpr bool operator<=(LayoutUnit a, LayoutUnit b) { return a.raw_ <= b.raw_; }
  friend constexpr bool operator>=(LayoutUnit a, LayoutUnit b) { return a.raw_ >= b.raw_; }

 private:
  explicit constexpr LayoutUnit(int32_t raw) : raw_(raw) {}
  static constexpr int32_t clamp64(int64_t v) { return v > kRawMax ? kRawMax : v < kRawMin ? kRawMin : static_cast<int32_t>(v); }

  int32_t raw_ = 0;
};

constexpr LayoutUnit min(LayoutUnit a, LayoutUnit b) { return a < b ? a : b; }
constexpr LayoutUnit max(LayoutUnit a, LayoutUnit b) { return a > b ? a : b; }
constexpr LayoutUnit clamp(LayoutUnit v, LayoutUnit lo, LayoutUnit hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace layout

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

// Calendar dates (proleptic Gregorian) for the date picker: validity, month lengths, weekdays and
// day arithmetic, in plain integers.
#pragma once
#include <stdint.h>

namespace gui {

struct Date {
  int32_t year = 2000;
  int32_t month = 1;  // 1 .. 12
  int32_t day = 1;    // 1 .. days_in_month

  constexpr bool operator==(const Date& o) const { return year == o.year && month == o.month && day == o.day; }
  constexpr bool operator!=(const Date& o) const { return !(*this == o); }
  constexpr bool operator<(const Date& o) const {
    return year != o.year ? year < o.year : month != o.month ? month < o.month : day < o.day;
  }
};

constexpr int32_t kMinYear = 1, kMaxYear = 9999;

constexpr bool is_leap_year(int32_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

constexpr int32_t days_in_month(int32_t year, int32_t month) {
  constexpr int32_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && is_leap_year(year) ? 29 : kDays[month - 1];
}

constexpr bool is_valid(const Date& d) {
  return d.year >= kMinYear && d.year <= kMaxYear && d.month >= 1 && d.month <= 12 && d.day >= 1 &&
         d.day <= days_in_month(d.year, d.month);
}

// Days since 1970-01-01 (negative before it).
constexpr int64_t days_from_date(const Date& d) {
  const int64_t y = d.month <= 2 ? d.year - 1 : d.year;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (d.month + (d.month > 2 ? -3 : 9)) + 2) / 5 + d.day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

constexpr Date date_from_days(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  Date d;
  d.day = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
  d.month = static_cast<int32_t>(mp < 10 ? mp + 3 : mp - 9);
  d.year = static_cast<int32_t>(yoe + era * 400 + (d.month <= 2 ? 1 : 0));
  return d;
}

// 0 = Sunday .. 6 = Saturday.
constexpr int32_t weekday(const Date& d) {
  const int64_t days = days_from_date(d);  // 1970-01-01 was a Thursday
  const int64_t w = (days + 4) % 7;
  return static_cast<int32_t>(w < 0 ? w + 7 : w);
}

// The date `days` later (or earlier), kept within the supported years.
constexpr Date add_days(const Date& d, int32_t days) {
  const Date result = date_from_days(days_from_date(d) + days);
  if (result.year < kMinYear) return {kMinYear, 1, 1};
  if (result.year > kMaxYear) return {kMaxYear, 12, 31};
  return result;
}

// The same day of the month `months` later, or the last day if the month is shorter.
constexpr Date add_months(const Date& d, int32_t months) {
  int32_t index = d.year * 12 + (d.month - 1) + months;
  if (index < kMinYear * 12) index = kMinYear * 12;
  if (index > kMaxYear * 12 + 11) index = kMaxYear * 12 + 11;
  Date result;
  result.year = index / 12;
  result.month = index % 12 + 1;
  const int32_t last = days_in_month(result.year, result.month);
  result.day = d.day < last ? d.day : last;
  return result;
}

// "YYYY-MM-DD" and a NUL, into `out` (11 bytes).
inline void format_date(const Date& d, char* out) {
  out[0] = static_cast<char>('0' + d.year / 1000 % 10);
  out[1] = static_cast<char>('0' + d.year / 100 % 10);
  out[2] = static_cast<char>('0' + d.year / 10 % 10);
  out[3] = static_cast<char>('0' + d.year % 10);
  out[4] = '-';
  out[5] = static_cast<char>('0' + d.month / 10);
  out[6] = static_cast<char>('0' + d.month % 10);
  out[7] = '-';
  out[8] = static_cast<char>('0' + d.day / 10);
  out[9] = static_cast<char>('0' + d.day % 10);
  out[10] = 0;
}

}  // namespace gui

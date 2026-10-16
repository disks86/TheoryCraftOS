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

#include "Math/FloatMath.hpp"
#include <stdint.h>

namespace fmath {

namespace {
constexpr double kLn2 = 0.693147180559945309417;

union DoubleBits {
  double d;
  uint64_t u;
};
}  // namespace

// x = m * 2^e with m in [0.75, 1.5), then ln m = 2 atanh((m-1)/(m+1)).
double ln(double x) {
  if (x <= 0) return -1e300;
  DoubleBits b{x};
  int e = static_cast<int>((b.u >> 52) & 0x7FF) - 1023;
  b.u = (b.u & 0x000FFFFFFFFFFFFFull) | (uint64_t{1023} << 52);  // m in [1, 2)
  double m = b.d;
  if (m > 1.5) {
    m *= 0.5;
    ++e;
  }
  const double t = (m - 1) / (m + 1);
  const double t2 = t * t;
  double sum = 0, term = t;
  for (int k = 1; k < 30; k += 2) {  // t + t^3/3 + t^5/5 + ...
    sum += term / k;
    term *= t2;
  }
  return 2 * sum + e * kLn2;
}

// x = k ln2 + r with |r| <= ln2/2, Taylor series for e^r, then scale by 2^k.
double exp(double x) {
  if (x < -700) return 0;
  if (x > 700) return 1e300;
  const double kf = x / kLn2;
  int k = static_cast<int>(kf < 0 ? kf - 0.5 : kf + 0.5);
  const double r = x - k * kLn2;
  double term = 1, sum = 1;
  for (int n = 1; n < 20; ++n) {
    term *= r / n;
    sum += term;
  }
  DoubleBits scale;
  scale.u = static_cast<uint64_t>(k + 1023) << 52;  // 2^k
  return sum * scale.d;
}

double pow(double base, double exponent) {
  if (base <= 0) return 0;
  return exp(exponent * ln(base));
}

double tanh(double x) {
  if (x > 20) return 1;
  if (x < -20) return -1;
  const double e2 = exp(2 * x);
  return (e2 - 1) / (e2 + 1);
}

}  // namespace fmath

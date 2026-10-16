#include "hardware/ImageSignalProcessor/IspMath.hpp"

namespace hardware {

namespace {
constexpr double kLn2 = 0.693147180559945309417;

union DoubleBits {
  double d;
  uint64_t u;
};

// ln(x) for x > 0: x = m * 2^e with m in [0.75, 1.5), then ln m = 2 atanh((m-1)/(m+1)).
double ln(double x) {
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

// e^x: x = k ln2 + r with |r| <= ln2/2, Taylor series for e^r, then scale by 2^k.
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
}  // namespace

double isp_pow(double base, double exponent) {
  if (base <= 0) return 0;
  return exp(exponent * ln(base));
}

void build_gamma_table(uint8_t* table, uint32_t entries, uint16_t gamma_x100) {
  const double inverse_gamma = gamma_x100 ? 100.0 / gamma_x100 : 1.0;
  for (uint32_t i = 0; i < entries; ++i) {
    const double v = entries > 1 ? static_cast<double>(i) / (entries - 1) : 1.0;
    const double encoded = 255.0 * isp_pow(v, inverse_gamma) + 0.5;
    table[i] = static_cast<uint8_t>(encoded > 255.0 ? 255.0 : encoded);
  }
}

}  // namespace hardware

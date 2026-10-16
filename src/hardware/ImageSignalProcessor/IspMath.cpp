#include "hardware/ImageSignalProcessor/IspMath.hpp"
#include "Math/FloatMath.hpp"

namespace hardware {

double isp_pow(double base, double exponent) { return fmath::pow(base, exponent); }

void build_gamma_table(uint8_t* table, uint32_t entries, uint16_t gamma_x100) {
  const double inverse_gamma = gamma_x100 ? 100.0 / gamma_x100 : 1.0;
  for (uint32_t i = 0; i < entries; ++i) {
    const double v = entries > 1 ? static_cast<double>(i) / (entries - 1) : 1.0;
    const double encoded = 255.0 * fmath::pow(v, inverse_gamma) + 0.5;
    table[i] = static_cast<uint8_t>(encoded > 255.0 ? 255.0 : encoded);
  }
}

}  // namespace hardware

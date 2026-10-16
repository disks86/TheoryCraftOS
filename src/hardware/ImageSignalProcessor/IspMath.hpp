// Floating-point helpers for building lookup tables. The image has no libm.
#pragma once
#include <stdint.h>

namespace hardware {

// base^exponent for base in (0, inf), via exp(exponent * ln(base)); accurate to
// about 1e-9 relative, far finer than an 8-bit gamma table needs.
double isp_pow(double base, double exponent);

// Fills table[0..entries-1] with round(255 * (i / (entries-1)) ^ (100 / gamma_x100)),
// the encoding curve for a display gamma of gamma_x100 / 100.
void build_gamma_table(uint8_t* table, uint32_t entries, uint16_t gamma_x100);

}  // namespace hardware

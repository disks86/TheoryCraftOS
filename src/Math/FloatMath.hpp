// Floating-point functions the image lacks (it has no libm). Accurate to about
// 1e-9 relative over the useful range, plenty for gamma tables and activations.
#pragma once

namespace fmath {

double ln(double x);                       // x > 0
double exp(double x);
double pow(double base, double exponent);  // base > 0, else 0
double tanh(double x);

}  // namespace fmath

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
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Hardware/IHardware.hpp"

namespace hardware {

enum class NpuActivation : uint8_t { Relu, Sigmoid, Tanh };

// A neural / tensor accelerator: the primitives neural-network inference is
// built from. Matrices are dense row-major. Implementations live in subfolders:
// a CPU one that works anywhere, and per-vendor classes for hardware NPUs.
class INeuralProcessingUnit : public IHardware {
 public:
  virtual uint32_t compute_units() const = 0;  // independent execution units
  virtual uint64_t memory_bytes() const = 0;   // dedicated on-device memory (0 = shares system RAM)

  // C[m x n] = A[m x k] * B[k x n]. Quantized inference: 8-bit inputs, 32-bit
  // accumulators (no overflow for k up to 2^17). Each matrix is a view that must
  // hold at least its m*k, k*n or m*n elements; a smaller one is refused (false)
  // instead of being overrun.
  virtual bool matmul_i8(ArrayView<const int8_t> a, ArrayView<const int8_t> b, ArrayView<int32_t> c, uint32_t m,
                         uint32_t n, uint32_t k) = 0;
  // The same in 32-bit floating point.
  virtual bool matmul_f32(ArrayView<const float> a, ArrayView<const float> b, ArrayView<float> c, uint32_t m,
                          uint32_t n, uint32_t k) = 0;

  // Element-wise, in place, over the whole view.
  virtual bool activate(ArrayView<float> data, NpuActivation kind) = 0;
  // Softmax over the whole view, in place (numerically stable).
  virtual bool softmax(ArrayView<float> data) = 0;

 protected:
  constexpr INeuralProcessingUnit() = default;
  ~INeuralProcessingUnit() = default;
};

}  // namespace hardware

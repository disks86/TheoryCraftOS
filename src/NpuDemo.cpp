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

#include "NpuDemo.hpp"
#include "Utility/AllocatedView.hpp"
#include "console.hpp"

namespace {
constexpr uint32_t kSize = 128;  // 128 x 128 x 128: big enough to be spread over cores

uint32_t g_seed = 12345;
uint32_t next_random() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}

void report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
}
}  // namespace

bool run_npu_demo(hardware::INeuralProcessingUnit& npu, allocators::IAllocator& allocator) {
  const uint32_t cells = kSize * kSize;
  ArrayView<int8_t> a8 = allocate_view<int8_t>(allocator, cells);
  ArrayView<int8_t> b8 = allocate_view<int8_t>(allocator, cells);
  ArrayView<int32_t> c32 = allocate_view<int32_t>(allocator, cells);
  ArrayView<float> af = allocate_view<float>(allocator, cells);
  ArrayView<float> bf = allocate_view<float>(allocator, cells);
  ArrayView<float> cf = allocate_view<float>(allocator, cells);
  bool all_ok = !a8.empty() && !b8.empty() && !c32.empty() && !af.empty() && !bf.empty() && !cf.empty();

  if (all_ok) {
    for (uint32_t i = 0; i < cells; ++i) {
      a8[i] = static_cast<int8_t>(next_random());
      b8[i] = static_cast<int8_t>(next_random());
      af[i] = static_cast<float>(static_cast<int32_t>(next_random() % 33) - 16) / 8.0f;  // exact in binary
      bf[i] = static_cast<float>(static_cast<int32_t>(next_random() % 33) - 16) / 8.0f;
    }

    // 8-bit matrix product against a plain triple loop.
    bool ok = npu.matmul_i8(a8, b8, c32, kSize, kSize, kSize);
    for (uint32_t i = 0; ok && i < kSize; ++i) {
      for (uint32_t j = 0; ok && j < kSize; ++j) {
        int32_t sum = 0;
        for (uint32_t p = 0; p < kSize; ++p) sum += a8[i * kSize + p] * b8[p * kSize + j];
        ok = c32[i * kSize + j] == sum;
      }
    }
    report("int8 matmul 128x128x128", ok);
    all_ok &= ok;

    // Float product. Inputs are multiples of 1/8 and sums stay small, so every
    // intermediate is exact and the result can be compared for equality.
    ok = npu.matmul_f32(af, bf, cf, kSize, kSize, kSize);
    for (uint32_t i = 0; ok && i < kSize; ++i) {
      for (uint32_t j = 0; ok && j < kSize; ++j) {
        float sum = 0;
        for (uint32_t p = 0; p < kSize; ++p) sum += af[i * kSize + p] * bf[p * kSize + j];
        ok = cf[i * kSize + j] == sum;
      }
    }
    report("float matmul 128x128x128", ok);
    all_ok &= ok;

    // Softmax: probabilities that sum to 1, ordered like the inputs.
    float logits[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    ok = npu.softmax(ArrayView<float>(logits));
    float total = 0;
    for (float p : logits) total += p;
    ok = ok && total > 0.9999f && total < 1.0001f && logits[3] > logits[2] && logits[2] > logits[0];
    report("softmax", ok);
    all_ok &= ok;
  }

  release_view(allocator, a8);
  release_view(allocator, b8);
  release_view(allocator, c32);
  release_view(allocator, af);
  release_view(allocator, bf);
  release_view(allocator, cf);
  return all_ok;
}

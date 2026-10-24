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

#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"
#include "Math/FloatMath.hpp"

namespace hardware {

namespace {
// Rows [row_begin, row_end) of C = A * B. The p-loop sits outside the j-loop so
// B and C are both walked along their rows (cache friendly).
template <class In, class Out, class Job>
void multiply_rows(const Job& job) {
  for (uint32_t i = job.row_begin; i < job.row_end; ++i) {
    const ArrayView<Out> c_row = job.c.subview(uint64_t{i} * job.n, job.n);
    c_row.fill(0);
    const ArrayView<const In> a_row = job.a.subview(uint64_t{i} * job.k, job.k);
    for (uint32_t p = 0; p < job.k; ++p) {
      const Out a_ip = static_cast<Out>(a_row[p]);
      const ArrayView<const In> b_row = job.b.subview(uint64_t{p} * job.n, job.n);
      for (uint32_t j = 0; j < job.n; ++j) c_row[j] += a_ip * static_cast<Out>(b_row[j]);
    }
  }
}

float sigmoid(float x) { return static_cast<float>(1.0 / (1.0 + fmath::exp(-static_cast<double>(x)))); }
}  // namespace

template <class In, class Out>
bool SoftwareNeuralProcessingUnit::matmul(ArrayView<const In> a, ArrayView<const In> b, ArrayView<Out> c,
                                          uint32_t m, uint32_t n, uint32_t k) {
  if (m == 0 || n == 0 || k == 0) return false;
  BusyTimer timer(*this);
  // The views must hold the matrices the dimensions describe.
  if (a.size() < uint64_t{m} * k || b.size() < uint64_t{k} * n || c.size() < uint64_t{m} * n) return false;

  uint32_t tasks = cpu_.core_count();
  if (tasks > kMaxTasks) tasks = kMaxTasks;
  if (tasks > m) tasks = m;
  const bool small = uint64_t{m} * n * k < kParallelThreshold;
  if (small || tasks <= 1) {
    multiply_rows<In, Out>(MatmulJob<In, Out>{a, b, c, n, k, 0, m});
    return true;
  }

  using Job = MatmulJob<In, Out>;
  Job jobs[kMaxTasks];
  const uint32_t rows_per_task = (m + tasks - 1) / tasks;
  uint32_t used = 0;
  for (uint32_t begin = 0; begin < m; begin += rows_per_task, ++used) {
    const uint32_t end = begin + rows_per_task < m ? begin + rows_per_task : m;
    jobs[used] = Job{a, b, c, n, k, begin, end};
  }

  // The calling core takes the last slice itself while the others work.
  for (uint32_t i = 0; i + 1 < used; ++i) {
    const bool queued = scheduler_.submit(
        [](void* job) { multiply_rows<In, Out>(*static_cast<Job*>(job)); }, &jobs[i],
        scheduling::TaskQueue::HighPerformance);
    if (!queued) multiply_rows<In, Out>(jobs[i]);  // queue full: just do it here
  }
  multiply_rows<In, Out>(jobs[used - 1]);
  scheduler_.run();  // waits for every started task, so `jobs` may go out of scope
  return true;
}

bool SoftwareNeuralProcessingUnit::matmul_i8(ArrayView<const int8_t> a, ArrayView<const int8_t> b,
                                             ArrayView<int32_t> c, uint32_t m, uint32_t n, uint32_t k) {
  return matmul<int8_t, int32_t>(a, b, c, m, n, k);
}

bool SoftwareNeuralProcessingUnit::matmul_f32(ArrayView<const float> a, ArrayView<const float> b, ArrayView<float> c,
                                              uint32_t m, uint32_t n, uint32_t k) {
  return matmul<float, float>(a, b, c, m, n, k);
}

bool SoftwareNeuralProcessingUnit::activate(ArrayView<float> data, NpuActivation kind) {
  BusyTimer timer(*this);
  for (float& x : data) {
    switch (kind) {
      case NpuActivation::Relu:    x = x > 0 ? x : 0; break;
      case NpuActivation::Sigmoid: x = sigmoid(x); break;
      case NpuActivation::Tanh:    x = static_cast<float>(fmath::tanh(x)); break;
    }
  }
  return true;
}

bool SoftwareNeuralProcessingUnit::softmax(ArrayView<float> data) {
  if (data.empty()) return false;
  BusyTimer timer(*this);
  float largest = data[0];
  for (float x : data)
    if (x > largest) largest = x;
  double sum = 0;
  for (float& x : data) {
    const double e = fmath::exp(static_cast<double>(x) - largest);  // largest -> e^0, no overflow
    x = static_cast<float>(e);
    sum += e;
  }
  for (float& x : data) x = static_cast<float>(x / sum);
  return true;
}

}  // namespace hardware

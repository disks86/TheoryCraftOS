#include "hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"
#include "Math/FloatMath.hpp"

namespace hardware {

namespace {
// Rows [row_begin, row_end) of C = A * B. The p-loop sits outside the j-loop so
// B and C are both walked along their rows (cache friendly).
template <class In, class Out, class Job>
void multiply_rows(const Job& job) {
  for (uint32_t i = job.row_begin; i < job.row_end; ++i) {
    Out* c_row = job.c + uint64_t{i} * job.n;
    for (uint32_t j = 0; j < job.n; ++j) c_row[j] = 0;
    const In* a_row = job.a + uint64_t{i} * job.k;
    for (uint32_t p = 0; p < job.k; ++p) {
      const Out a_ip = static_cast<Out>(a_row[p]);
      const In* b_row = job.b + uint64_t{p} * job.n;
      for (uint32_t j = 0; j < job.n; ++j) c_row[j] += a_ip * static_cast<Out>(b_row[j]);
    }
  }
}

float sigmoid(float x) { return static_cast<float>(1.0 / (1.0 + fmath::exp(-static_cast<double>(x)))); }
}  // namespace

template <class In, class Out>
bool SoftwareNeuralProcessingUnit::matmul(const In* a, const In* b, Out* c, uint32_t m, uint32_t n,
                                          uint32_t k) {
  if (!a || !b || !c || m == 0 || n == 0 || k == 0) return false;

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
        scheduling::Queue::HighPerformance);
    if (!queued) multiply_rows<In, Out>(jobs[i]);  // queue full: just do it here
  }
  multiply_rows<In, Out>(jobs[used - 1]);
  scheduler_.run();  // waits for every started task, so `jobs` may go out of scope
  return true;
}

bool SoftwareNeuralProcessingUnit::matmul_i8(const int8_t* a, const int8_t* b, int32_t* c,
                                             uint32_t m, uint32_t n, uint32_t k) {
  return matmul<int8_t, int32_t>(a, b, c, m, n, k);
}

bool SoftwareNeuralProcessingUnit::matmul_f32(const float* a, const float* b, float* c, uint32_t m,
                                              uint32_t n, uint32_t k) {
  return matmul<float, float>(a, b, c, m, n, k);
}

bool SoftwareNeuralProcessingUnit::activate(float* data, uint32_t count, NpuActivation kind) {
  if (!data) return false;
  for (uint32_t i = 0; i < count; ++i) {
    switch (kind) {
      case NpuActivation::Relu:    data[i] = data[i] > 0 ? data[i] : 0; break;
      case NpuActivation::Sigmoid: data[i] = sigmoid(data[i]); break;
      case NpuActivation::Tanh:    data[i] = static_cast<float>(fmath::tanh(data[i])); break;
    }
  }
  return true;
}

bool SoftwareNeuralProcessingUnit::softmax(float* data, uint32_t count) {
  if (!data || count == 0) return false;
  float largest = data[0];
  for (uint32_t i = 1; i < count; ++i)
    if (data[i] > largest) largest = data[i];
  double sum = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const double e = fmath::exp(static_cast<double>(data[i]) - largest);  // largest -> e^0, no overflow
    data[i] = static_cast<float>(e);
    sum += e;
  }
  for (uint32_t i = 0; i < count; ++i) data[i] = static_cast<float>(data[i] / sum);
  return true;
}

}  // namespace hardware

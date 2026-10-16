#pragma once
#include "CPUScheduler/ICPUScheduler.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "hardware/NeuralProcessingUnit/INeuralProcessingUnit.hpp"

namespace hardware {

// A CPU implementation usable on any machine. Large matrix products are split
// by rows into tasks and spread over every core through the scheduler, so it
// scales with the core count the CPU interface reports. Hardware NPU classes
// use it until they have native drivers.
//
// Tasks may run on other cores, where firmware is off limits: nothing here
// touches it. The scheduler must not be running other work concurrently with a
// call (each call waits for its tasks via scheduler.run()).
class SoftwareNeuralProcessingUnit : public INeuralProcessingUnit {
 public:
  static constexpr uint32_t kMaxTasks = 32;
  // Products smaller than this many multiply-adds run on the calling core.
  static constexpr uint64_t kParallelThreshold = 1u << 16;

  SoftwareNeuralProcessingUnit(ICentralProcessingUnit& cpu, scheduling::ICPUScheduler& scheduler)
      : cpu_(cpu), scheduler_(scheduler) {}

  const char* name() const override { return "software NPU"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint32_t compute_units() const override { return cpu_.core_count(); }
  uint64_t memory_bytes() const override { return 0; }

  bool matmul_i8(const int8_t* a, const int8_t* b, int32_t* c, uint32_t m, uint32_t n,
                 uint32_t k) override;
  bool matmul_f32(const float* a, const float* b, float* c, uint32_t m, uint32_t n,
                  uint32_t k) override;
  bool activate(float* data, uint32_t count, NpuActivation kind) override;
  bool softmax(float* data, uint32_t count) override;

 private:
  template <class In, class Out>
  struct MatmulJob {
    const In* a;
    const In* b;
    Out* c;
    uint32_t n, k;
    uint32_t row_begin, row_end;
  };

  template <class In, class Out>
  bool matmul(const In* a, const In* b, Out* c, uint32_t m, uint32_t n, uint32_t k);

  ICentralProcessingUnit& cpu_;
  scheduling::ICPUScheduler& scheduler_;
};

}  // namespace hardware

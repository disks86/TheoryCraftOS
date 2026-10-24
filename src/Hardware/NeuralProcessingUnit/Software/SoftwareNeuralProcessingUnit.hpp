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
#include "CPUScheduler/ICPUScheduler.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Hardware/NeuralProcessingUnit/INeuralProcessingUnit.hpp"

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
  void set_clock(const IClock* clock) override { clock_ = clock; }
  uint64_t busy_us() const override { return __atomic_load_n(&busy_us_, __ATOMIC_RELAXED); }

  bool matmul_i8(ArrayView<const int8_t> a, ArrayView<const int8_t> b, ArrayView<int32_t> c, uint32_t m, uint32_t n,
                 uint32_t k) override;
  bool matmul_f32(ArrayView<const float> a, ArrayView<const float> b, ArrayView<float> c, uint32_t m, uint32_t n,
                  uint32_t k) override;
  bool activate(ArrayView<float> data, NpuActivation kind) override;
  bool softmax(ArrayView<float> data) override;

 private:
  template <class In, class Out>
  struct MatmulJob {
    ArrayView<const In> a;
    ArrayView<const In> b;
    ArrayView<Out> c;
    uint32_t n, k;
    uint32_t row_begin, row_end;
  };

  template <class In, class Out>
  bool matmul(ArrayView<const In> a, ArrayView<const In> b, ArrayView<Out> c, uint32_t m, uint32_t n, uint32_t k);

  // Adds the time between its construction and destruction to busy_us_ (nothing without a clock).
  class BusyTimer {
   public:
    explicit BusyTimer(SoftwareNeuralProcessingUnit& owner) : owner_(owner), start_(owner.clock_ ? owner.clock_->now_us() : 0) {}
    ~BusyTimer() {
      if (!owner_.clock_) return;
      const uint64_t end = owner_.clock_->now_us();
      if (end > start_) __atomic_fetch_add(&owner_.busy_us_, end - start_, __ATOMIC_RELAXED);
    }

   private:
    SoftwareNeuralProcessingUnit& owner_;
    uint64_t start_;
  };

  const IClock* clock_ = nullptr;
  uint64_t busy_us_ = 0;
  ICentralProcessingUnit& cpu_;
  scheduling::ICPUScheduler& scheduler_;
};

}  // namespace hardware

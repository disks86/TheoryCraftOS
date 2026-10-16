#include "hardware/Clock/TscClock.hpp"

namespace hardware {

bool TscClock::initialize() {
  if (!st_ || !st_->boot_services) return false;
  constexpr uint64_t kCalibrationUs = 50000;
  // Stall() never returns early, only late: an interruption (firmware timer work,
  // a hypervisor descheduling us) makes a run take longer and so count MORE
  // ticks. The run with the fewest ticks is therefore the least disturbed.
  uint64_t fewest = ~0ull;
  for (int run = 0; run < 3; ++run) {
    const uint64_t start = cpu_.timestamp_counter();
    st_->boot_services->stall(kCalibrationUs);
    const uint64_t ticks = cpu_.timestamp_counter() - start;
    if (ticks < fewest) fewest = ticks;
  }
  ticks_per_us_ = fewest / kCalibrationUs;
  if (ticks_per_us_ == 0) return false;
  origin_ = cpu_.timestamp_counter();
  return true;
}

uint64_t TscClock::now_us() const {
  if (!ticks_per_us_) return 0;
  return (cpu_.timestamp_counter() - origin_) / ticks_per_us_;
}

}  // namespace hardware

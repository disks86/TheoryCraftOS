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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuExecutor.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint64_t kPage = 4096;
}

IntelNpuExecutor::~IntelNpuExecutor() {
  lock();
  drop_context();
  unlock();
}

void IntelNpuExecutor::lock() {
  while (!try_lock()) {
    while (__atomic_load_n(&lock_, __ATOMIC_RELAXED)) __builtin_ia32_pause();
  }
}

void IntelNpuExecutor::drop_context() {
  if (ssid_ != 0) device_.destroy_context(ssid_);   // its queue and mappings go with it
  ssid_ = queue_ = 0;
}

bool IntelNpuExecutor::ensure_queue() {
  if (ssid_ != 0 && device_.context_faulted(ssid_)) drop_context();
  if (ssid_ == 0) {
    if (device_.create_context(&ssid_) != IntelNpuDeviceError::None) { ssid_ = 0; return false; }
    if (device_.create_queue(ssid_, IntelNpuJsm::kBandNormal, &queue_) != IntelNpuDeviceError::None) { drop_context(); return false; }
  }
  return true;
}

void IntelNpuExecutor::recover_device() {
  ++recoveries_;
  device_.recover(wall_us());
}

IntelNpuExecutor::Result IntelNpuExecutor::execute(ConstByteView commands) {
  Result result;
  lock();
  const uint64_t start = config_.clock ? config_.clock->now_us() : 0;
  auto finish = [&](Status status) {
    result.status = status;
    if (config_.clock) {
      const uint64_t end = config_.clock->now_us();
      result.elapsed_us = end > start ? end - start : 0;
    }
    unlock();
    return result;
  };
  if (device_.state() != IntelNpuDeviceState::Running) return finish(Status::NotRunning);
  if (commands.empty() || commands.size() > config_.max_command_bytes) return finish(Status::Empty);
  if (!ensure_queue()) return finish(Status::NoMemory);

  const uint64_t bytes = (commands.size() + kPage - 1) & ~(kPage - 1);
  ArrayView<uint8_t> memory = allocate_dma_view<uint8_t>(allocator_, bytes, kPage);
  if (memory.empty()) return finish(Status::NoMemory);
  memory.fill(0);
  memory.first(commands.size()).copy_from(commands);
  if (!device_.snoop()) intel_npu_flush_cache(ConstByteView(memory.data(), memory.size()));
  const IntelNpuDmaExtent extent[1] = {{intel_npu_bus_address(memory.data()), bytes}};

  Status status = Status::NoMemory;
  bool memory_is_free = true;   // false while the NPU might still read it
  uint64_t va = 0;
  IntelNpuPageTable::MapFlags flags;
  flags.read_only = true;
  if (device_.map_buffer(ssid_, ArrayView<const IntelNpuDmaExtent>(extent), bytes, flags, &va) == IntelNpuDeviceError::None) {
    IntelNpuJob job;
    job.batch_buffer = va;
    uint32_t job_id = 0;
    const IntelNpuDeviceError submitted = device_.submit(ssid_, queue_, job, &job_id);
    if (submitted != IntelNpuDeviceError::None) {
      status = submitted == IntelNpuDeviceError::Faulted ? Status::Faulted : Status::Rejected;
    } else {
      status = Status::Timeout;
      for (uint32_t i = 0; i < config_.wait_tries; ++i) {
        const IntelNpuServiceResult s = device_.service();
        IntelNpuJobResult done;
        if (device_.take_job_result(job_id, &done)) {
          result.firmware_status = done.status;
          status = done.outcome == IntelNpuJobOutcome::Completed ? (done.status == 0 ? Status::Ok : Status::Failed)
                 : done.outcome == IntelNpuJobOutcome::ContextFault ? Status::Faulted : Status::Aborted;
          break;
        }
        if (s.recovery_needed) { recover_device(); status = Status::Aborted; device_.take_job_result(job_id, &done); break; }
      }
      if (status == Status::Timeout) {
        // The job may still be running and reading its commands: reset the NPU before the memory is given back.
        recover_device();
        IntelNpuJobResult done;
        device_.take_job_result(job_id, &done);
      }
    }
    if (device_.state() == IntelNpuDeviceState::Running || device_.state() == IntelNpuDeviceState::Suspended) {
      device_.unmap_buffer(ssid_, va, ArrayView<const IntelNpuDmaExtent>(extent));
      memory_is_free = true;
    } else {
      memory_is_free = false;   // the device is Failed: the NPU could not be reset, so it may still use the memory
    }
  }
  if (memory_is_free) release_view(allocator_, memory);   // else leaked on purpose
  ++jobs_run_;
  if (status != Status::Ok) ++jobs_failed_;
  if (config_.clock && (status == Status::Ok || status == Status::Failed)) {
    const uint64_t end = config_.clock->now_us();
    if (end > start) __atomic_fetch_add(&busy_us_, end - start, __ATOMIC_RELAXED);
  }
  return finish(status);
}

IntelNpuServiceResult IntelNpuExecutor::poll() {
  IntelNpuServiceResult result;
  if (!try_lock()) return result;   // execute() is running and services the device itself
  const uint32_t arrived = __atomic_load_n(&interrupts_, __ATOMIC_ACQUIRE);
  const bool fresh = arrived != interrupts_seen_;
  interrupts_seen_ = arrived;
  if (device_.state() == IntelNpuDeviceState::Running && (fresh || !config_.interrupts || ++idle_ >= config_.idle_polls)) {
    idle_ = 0;
    result = device_.service();
    if (result.recovery_needed) recover_device();
  }
  unlock();
  return result;
}

}  // namespace hardware

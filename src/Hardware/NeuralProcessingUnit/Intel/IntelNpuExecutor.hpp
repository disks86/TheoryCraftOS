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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuDevice.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Runs work on a started IntelNpuDevice and keeps it serviced. It is the only caller of the device once it exists, which is how the device's single-threaded
// rule is kept: execute() (from any thread) and poll() (the desktop loop) take turns through a lock; the interrupt handler only counts (notify_interrupt()) and
// never touches the device.
//
// What execute() runs is a command stream the NPU firmware understands: the neural network compiler's output (an Intel NPU does not do matrix products on request;
// it runs compiled graphs). This class copies it into DMA memory, maps it in a context of its own, submits one job, services the device until the job is done, and
// unmaps again. It does not read the stream. Without a compiler there is nothing to give it but test streams; the matrix primitives of INeuralProcessingUnit stay on
// the CPU.
class IntelNpuExecutor {
 public:
  struct Config {
    const IClock* clock = nullptr;                          // times the jobs (busy_us); null: not measured
    uint64_t (*wall_clock_us)(void* context) = nullptr;     // the wall clock the firmware keeps time from, for a recovery's reboot; null: 0
    void* wall_context = nullptr;
    uint32_t wait_tries = 1000000;                          // looks at the device while a job runs
    uint32_t idle_polls = 250;                              // poll() calls without an interrupt after which it looks anyway (the interrupt may have been lost)
    bool interrupts = true;                                 // false: the NPU's interrupt does not reach us; poll() looks every call
    uint32_t max_command_bytes = 16u << 20;
  };
  enum class Status : uint8_t {
    Ok,
    NotRunning,   // the device is not running
    Empty,        // no commands, or more than max_command_bytes
    NoMemory,     // DMA memory, NPU address space, or a context or queue could not be had
    Rejected,     // the device would not take the job (queue full, no room)
    Failed,       // the firmware finished it with a non-zero status
    Faulted,      // the job's context faulted (translation fault, engine reset required): the executor made a new one for the next job
    Aborted,      // the NPU was reset under the job
    Timeout,      // no answer in time: the NPU was reset
  };
  struct Result {
    Status status = Status::NotRunning;
    uint32_t firmware_status = 0;
    uint64_t elapsed_us = 0;
  };

  IntelNpuExecutor(IntelNpuDevice& device, allocators::IAllocator& allocator, const Config& config) : device_(device), allocator_(allocator), config_(config) {}
  ~IntelNpuExecutor();
  IntelNpuExecutor(const IntelNpuExecutor&) = delete;
  IntelNpuExecutor& operator=(const IntelNpuExecutor&) = delete;

  Result execute(ConstByteView commands);
  // For the interrupt handler: one more interrupt came. Touches nothing but a counter.
  void notify_interrupt() { __atomic_fetch_add(&interrupts_, 1, __ATOMIC_ACQ_REL); }
  // For the desktop loop: looks at the device when an interrupt came (or now and then, or always without interrupts) and recovers it when it asks for that.
  // Does nothing while execute() is running (it services the device itself).
  IntelNpuServiceResult poll();

  // Microseconds jobs have been in flight on the NPU (needs a clock). The change over an interval divided by the interval is how busy the NPU was.
  uint64_t busy_us() const { return __atomic_load_n(&busy_us_, __ATOMIC_RELAXED); }
  uint32_t jobs_run() const { return __atomic_load_n(&jobs_run_, __ATOMIC_RELAXED); }
  uint32_t jobs_failed() const { return __atomic_load_n(&jobs_failed_, __ATOMIC_RELAXED); }
  uint32_t recoveries() const { return __atomic_load_n(&recoveries_, __ATOMIC_RELAXED); }
  IntelNpuDevice& device() { return device_; }

 private:
  bool try_lock() { return __atomic_exchange_n(&lock_, 1, __ATOMIC_ACQUIRE) == 0; }
  void lock();
  void unlock() { __atomic_store_n(&lock_, 0, __ATOMIC_RELEASE); }
  bool ensure_queue();
  void drop_context();
  void recover_device();
  uint64_t wall_us() const { return config_.wall_clock_us ? config_.wall_clock_us(config_.wall_context) : 0; }

  IntelNpuDevice& device_;
  allocators::IAllocator& allocator_;
  Config config_;
  uint32_t lock_ = 0;
  uint32_t interrupts_ = 0, interrupts_seen_ = 0, idle_ = 0;
  uint32_t ssid_ = 0, queue_ = 0;   // the context and queue jobs run in (0: none yet)
  uint64_t busy_us_ = 0;
  uint32_t jobs_run_ = 0, jobs_failed_ = 0, recoveries_ = 0;
};

}  // namespace hardware

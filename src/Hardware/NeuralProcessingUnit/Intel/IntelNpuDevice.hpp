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
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmware.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIpc.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIrq.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuJobs.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuMmu.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct IntelNpuDeviceConfig {
  INpuRegisters* ip = nullptr;         // BAR 0, mapped over IntelNpuHw::operational_window()
  INpuRegisters* buttress = nullptr;   // BAR 4, over IntelNpuHw::kButtressWindow
  allocators::IAllocator* allocator = nullptr;
  uint16_t device_id = 0;              // PCI
  uint8_t revision = 0;                // PCI
  uint8_t pci_bus = 0;                 // handed to the firmware as its id
  bool snoop = false;                  // the NPU's memory traffic is cache coherent: no flushing of shared memory
  uint32_t scheduler = kIntelNpuSchedAuto;
  uint32_t boot_tries = IntelNpuIpc::kDefaultReplyTries;   // looks at the FIFO while waiting for the firmware's ready message
  uint32_t reply_tries = IntelNpuIpc::kDefaultReplyTries;  // ... while waiting for a reply
  uint32_t process_id = 0;
};

enum class IntelNpuDeviceState : uint8_t {
  Created,    // nothing loaded
  Loaded,     // firmware parsed and its memory set up; the NPU is off
  Running,    // booted, accepting work
  Suspended,  // powered down after running; start() boots it again
  Failed,     // a boot or a recovery failed; the NPU is powered down; start() may try again
};

enum class IntelNpuDeviceError : uint8_t {
  None,
  BadState,     // the call does not fit the state
  Platform,     // not an Intel NPU
  Registers,    // the fuse and status registers did not answer
  Firmware,     // the firmware file was refused (IntelNpuDevice::firmware_error())
  Memory,       // out of memory or NPU address space
  PowerUp,      // IntelNpuDevice::hw_step() says which step
  Mmu,          // the MMU would not turn on or take a mapping
  BootParams,
  CpuBoot,
  BootTimeout,  // the firmware never sent its ready message
  Ipc,          // a message failed (IntelNpuDevice::ipc_status())
  NoSuchContext,
  NoSuchQueue,
  NoRoom,       // no free context, queue or job slot
  Faulted,      // the context has faulted and takes no more work
  QueueFull,
  Invalid,
};
const char* intel_npu_device_error_name(IntelNpuDeviceError error);

struct IntelNpuServiceResult {
  bool handled = false;           // the NPU had raised an interrupt
  bool recovery_needed = false;   // call recover(): a watchdog, a buttress error, a fault of the firmware's own context
  uint32_t mmu_faults = 0;        // translation faults read from the MMU's event queue
  uint32_t faulted_contexts = 0;  // user contexts that stopped (their jobs are aborted)
  uint32_t engine_resets = 0;
};

// One Intel NPU, brought up the way the Linux ivpu driver does (ivpu_drv.c, ivpu_pm.c): parse the firmware, power the IP block up, turn the MMU on, map the
// firmware's memory, load the image and boot parameters, start the NPU's CPU, wait for its ready message, enable interrupts and the message channel, set up the
// hardware scheduler. Applications get contexts (an MMU address space each), buffers mapped into them, command queues, and jobs.
//
// Always a cold boot: the firmware's state is not saved across power-down (no D0i3 state save), so suspend() followed by start() boots it afresh and the jobs in
// flight are aborted. User contexts, their mappings and queues survive; the queues register with the firmware again on their next job.
//
// Not thread safe. The owner serializes every call, service() (the interrupt handler's work) included.
class IntelNpuDevice {
 public:
  static constexpr uint32_t kMaxUserContexts = IntelNpuMmuContext::kUserMaxSsid - IntelNpuMmuContext::kUserMinSsid + 1;
  static constexpr uint32_t kMaxQueuesPerContext = 16;
  static constexpr uint32_t kJobQueueBytes = 4096;
  static constexpr uint32_t kIpcAreaBytes = 16384;
  static constexpr uint32_t kCriticalLogBytes = 512u << 10, kVerboseLogBytes = 1u << 20;

  explicit IntelNpuDevice(const IntelNpuDeviceConfig& config);
  ~IntelNpuDevice();
  IntelNpuDevice(const IntelNpuDevice&) = delete;
  IntelNpuDevice& operator=(const IntelNpuDevice&) = delete;

  IntelNpuDeviceState state() const { return state_; }
  IntelNpuDeviceError last_error() const { return last_error_; }
  IntelNpuStep hw_step() const { return hw_step_; }                      // for PowerUp and CpuBoot errors
  IntelNpuIpcStatus ipc_status() const { return ipc_status_; }           // for Ipc errors
  IntelNpuFirmwareError firmware_error() const { return firmware_error_; }
  const IntelNpuInfo& info() const { return hw_.info(); }
  const IntelNpuFirmware& firmware() const { return firmware_; }
  uint32_t boot_count() const { return boot_count_; }
  bool snoop() const { return config_.snoop; }

  // Created -> Loaded. Reads the fuses, parses the firmware file (which must stay alive: the image is copied at every boot) and allocates the memory the
  // firmware needs.
  IntelNpuDeviceError load_firmware(ConstByteView file);
  // Loaded, Suspended or Failed -> Running. `system_time_us` is the wall clock (microseconds since 1970), which the firmware keeps time from. On failure the NPU is
  // powered down again and the state is Failed.
  IntelNpuDeviceError start(uint64_t system_time_us = 0);
  // Running -> Suspended: interrupts, message channel and MMU off, jobs aborted, queues forgotten by the firmware, hardware reset and powered down.
  IntelNpuDeviceError suspend();
  // After a hang or a fatal fault: suspend (whatever its result) and start again. Contexts survive; their jobs are aborted.
  IntelNpuDeviceError recover(uint64_t system_time_us = 0);

  // The interrupt handler's work: reads and clears what is pending, drains the message channel, runs job-completion callbacks, reads MMU faults, resets the engine
  // when a job faulted. Safe to call without an interrupt (it then finds nothing). Only while Running.
  IntelNpuServiceResult service();

  // Contexts. SSIDs are handed out from IntelNpuMmuContext::kUserMinSsid.
  IntelNpuDeviceError create_context(uint32_t* ssid);
  // Aborts the context's jobs, tells the firmware to release it, and frees its queues and mappings.
  IntelNpuDeviceError destroy_context(uint32_t ssid);
  bool context_faulted(uint32_t ssid) const;
  // Maps host memory (`extents` add up to `size`) into the context at a free NPU address; the address comes back in `vpu_addr`.
  IntelNpuDeviceError map_buffer(uint32_t ssid, ArrayView<const IntelNpuDmaExtent> extents, uint64_t size, IntelNpuPageTable::MapFlags flags, uint64_t* vpu_addr);
  IntelNpuDeviceError unmap_buffer(uint32_t ssid, uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents);

  // Queues and jobs.
  IntelNpuDeviceError create_queue(uint32_t ssid, uint32_t priority_band, uint32_t* queue_id);
  IntelNpuDeviceError destroy_queue(uint32_t ssid, uint32_t queue_id);
  IntelNpuDeviceError submit(uint32_t ssid, uint32_t queue_id, const IntelNpuJob& job, uint32_t* job_id);
  // The result of a finished job (removes it from the table); false while it still runs.
  bool take_job_result(uint32_t job_id, IntelNpuJobResult* result) { return tracker_.take_result(job_id, result); }
  uint64_t jobs_in_flight() const { return tracker_.in_flight(); }
  // Asks the firmware for its heartbeat counter.
  IntelNpuDeviceError heartbeat(uint64_t* value);

  uint32_t firewall_violations() const { return irq_.firewall_count(); }
  uint32_t total_mmu_faults() const { return total_mmu_faults_; }
  uint32_t engine_reset_count() const { return engine_resets_; }

 private:
  struct Buffer {
    ArrayView<uint8_t> memory;
    uint64_t vpu_addr = 0;
    bool mapped = false;
  };
  struct Queue {
    Buffer buffer;
    IntelNpuCommandQueue jobq;
    IntelNpuRegisteredQueue reg;
  };
  struct UserContext {
    UserContext(allocators::IAllocator& a, IntelNpuMmu& m, const IntelNpuMemoryMap& map, uint32_t ssid, bool snoop) : mmu(a, m, map, ssid, snoop) {}
    IntelNpuMmuContext mmu;
    bool faulted = false;
    UniquePointer<Queue> queues[kMaxQueuesPerContext];
  };

  IntelNpuDeviceError fail(IntelNpuDeviceError error);
  IntelNpuDeviceError first_boot_setup();
  IntelNpuDeviceError boot_firmware(uint64_t system_time_us);
  void quiesce();
  void power_down_hardware();
  void forget_queues();
  bool alloc_memory(Buffer& buffer, uint64_t size);
  bool map_global(Buffer& buffer, const IntelNpuAddressRange& range);
  void free_buffer(Buffer& buffer);
  IntelNpuFirmwareMemory firmware_memory() const;
  void handle_faults(IntelNpuServiceResult& result);
  UserContext* context(uint32_t ssid);
  const UserContext* context(uint32_t ssid) const;
  IntelNpuDeviceError ipc_fail(IntelNpuIpcStatus status);

  IntelNpuDeviceConfig config_;
  IntelNpuPlatform platform_;
  IntelNpuMemoryMap map_;
  IntelNpuHw hw_;
  IntelNpuIrq irq_;
  IntelNpuMmu mmu_;
  IntelNpuIpc ipc_;
  IntelNpuDoorbells doorbells_;
  IntelNpuJobTracker tracker_;
  IntelNpuJobDoneSink job_sink_;
  IntelNpuIpcConsumer job_consumer_;
  IntelNpuQueueManager queues_;

  IntelNpuDeviceState state_ = IntelNpuDeviceState::Created;
  IntelNpuDeviceError last_error_ = IntelNpuDeviceError::None;
  IntelNpuStep hw_step_ = IntelNpuStep::None;
  IntelNpuIpcStatus ipc_status_ = IntelNpuIpcStatus::Ok;
  IntelNpuFirmwareError firmware_error_ = IntelNpuFirmwareError::None;
  ConstByteView firmware_file_;
  IntelNpuFirmware firmware_;
  bool mmu_initialized_ = false;
  bool set_up_ = false;   // the global context, the firmware mappings and the shared buffers exist
  uint32_t boot_count_ = 0, total_mmu_faults_ = 0, engine_resets_ = 0;

  Buffer boot_params_, fw_version_, runtime_, ipc_tx_, ipc_rx_, log_critical_, log_verbose_, shave_nn_;
  UniquePointer<IntelNpuMmuContext> global_, reserved_;
  UniquePointer<UserContext> contexts_[kMaxUserContexts];
};

}  // namespace hardware

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
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIpc.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// One unit of work for the NPU (struct vpu_job_queue_entry): a batch buffer of commands, an id to find it again when it is done, and optionally the buffers the
// firmware may save a preempted job's state in.
struct IntelNpuJob {
  uint64_t batch_buffer = 0;          // NPU address of the commands
  uint32_t job_id = 0;
  bool null_submission = false;       // return as successful at once without running the commands (a test mode)
  uint64_t primary_preempt_addr = 0, secondary_preempt_addr = 0;
  uint32_t primary_preempt_size = 0, secondary_preempt_size = 0;
};

// A job queue (struct vpu_job_queue): a 64-byte header with the head the NPU has consumed up to and the tail the host has written up to, then 64-byte entries,
// in memory the NPU can read and write (a page of the global context). The host fills the entry at the tail, moves the tail, and rings the queue's doorbell.
class IntelNpuCommandQueue {
 public:
  static constexpr uint32_t kHeaderSize = 64, kEntrySize = 64;
  static constexpr uint32_t kEngineIdxOffset = 0, kHeadOffset = 4, kTailOffset = 8, kFlagsOffset = 12;
  static constexpr uint32_t kFlagNoJobDone = 1u << 0, kFlagNativeFence = 1u << 1, kFlagTurbo = 1u << 2, kFlagNonInteractive = 1u << 3;
  static constexpr uint32_t kJobNullSubmission = 1u << 0;

  enum class PushResult : uint8_t { Ok, Full, Corrupt, NotReady };

  // `memory` is the host view, `vpu_addr` the address the NPU knows it by. At least the header and two entries (one is always left empty).
  bool init(ByteView memory, uint64_t vpu_addr, uint32_t flags, bool snoop);
  bool ready() const { return entry_count_ != 0; }
  uint64_t vpu_address() const { return vpu_addr_; }
  uint64_t size() const { return memory_.size(); }
  uint32_t entry_count() const { return entry_count_; }
  // Head and tail to zero (ivpu_cmdq_jobq_reset).
  void reset();
  PushResult push(const IntelNpuJob& job);
  // How many jobs the NPU has not consumed yet; Corrupt (UINT32_MAX) if it left a head outside the queue.
  uint32_t pending();

 private:
  uint32_t read_head();
  void write32(uint64_t offset, uint32_t value);
  void write64(uint64_t offset, uint64_t value);
  void flush(uint64_t offset, uint64_t size);

  ByteView memory_;
  uint64_t vpu_addr_ = 0;
  uint32_t entry_count_ = 0;
  uint32_t tail_ = 0;
  bool snoop_ = true;
};

// The doorbells (ivpu_hw_ip_db_set): a write of 1 to a register per doorbell tells the NPU that its queue has work.
class IntelNpuDoorbells {
 public:
  static constexpr uint32_t kMin = 1, kMax = 255;
  static constexpr uint64_t kBase37xx = 0x06300000, kBaseLater = 0x01300000, kStride = 0x1000;
  // Doorbell ids, handed out cyclically like ivpu's xa_alloc_cyclic.
  bool allocate(uint32_t* id);
  void release(uint32_t id);
  uint32_t in_use() const { return in_use_; }
  static bool ring(INpuRegisters& ip, IntelNpuIp generation, uint32_t id);
  // The end of the IP BAR range the doorbells need.
  static uint64_t window_end(IntelNpuIp generation) { return (generation == IntelNpuIp::Ip37xx ? kBase37xx : kBaseLater) + (kMax + 1) * kStride; }

 private:
  bool used_[kMax + 1] = {};
  uint32_t next_ = kMin;
  uint32_t in_use_ = 0;
};

enum class IntelNpuQueueStatus : uint8_t { Ok, NoDoorbell, Ipc, Invalid };

// A queue registered with the firmware: which context it is for, its ids, and which scheduler mode it was made for.
struct IntelNpuRegisteredQueue {
  uint32_t ssid = 0;
  uint32_t cmdq_id = 0;
  uint32_t doorbell = 0;     // 0: not registered
  uint32_t priority_band = IntelNpuJsm::kBandNormal;
  bool hardware_scheduler = false;
  IntelNpuIpcStatus last_ipc_status = IntelNpuIpcStatus::Ok;
};

// The firmware conversation of ivpu_cmdq_register()/ivpu_cmdq_unregister(): with the hardware scheduler a command queue is created, given its scheduling
// properties and a doorbell; with the OS scheduler the doorbell alone is registered.
class IntelNpuQueueManager {
 public:
  IntelNpuQueueManager(IntelNpuIpc& ipc, IntelNpuDoorbells& doorbells, uint32_t reply_tries = IntelNpuIpc::kDefaultReplyTries)
      : ipc_(ipc), doorbells_(doorbells), reply_tries_(reply_tries) {}
  IntelNpuQueueStatus register_queue(IntelNpuRegisteredQueue& queue, const IntelNpuCommandQueue& jobq, uint32_t process_id);
  IntelNpuQueueStatus unregister_queue(IntelNpuRegisteredQueue& queue);

 private:
  IntelNpuIpcStatus call(IntelNpuJsmMessage request);
  IntelNpuIpc& ipc_;
  IntelNpuDoorbells& doorbells_;
  uint32_t reply_tries_;
};

// What the firmware said about a finished job.
enum class IntelNpuJobOutcome : uint8_t {
  Unknown,       // not a job we have in flight
  Completed,     // done; `status` is the firmware's result (0 = success)
  ContextFault,  // the job hit a fault that needs the engine reset (and the context aborted)
};

struct IntelNpuJobResult {
  IntelNpuJobOutcome outcome = IntelNpuJobOutcome::Unknown;
  uint32_t job_id = 0, ssid = 0, status = 0;
};

// The jobs in flight: ids (the context in the upper 24 bits, a slot in the low 8, allocated cyclically per context as ivpu does) and their outcome when the
// firmware's job-return message arrives. Not thread safe.
class IntelNpuJobTracker {
 public:
  static constexpr uint32_t kSlotsPerContext = 256;
  explicit IntelNpuJobTracker(allocators::IAllocator& allocator) : jobs_(allocator), cursors_(allocator) {}
  // A job id for a new job of `ssid` (a user context, 2 and up), or false when the context has 256 jobs in flight or memory is out.
  bool begin(uint32_t ssid, uint32_t cmdq_id, uint32_t* job_id);
  void abandon(uint32_t job_id) { jobs_.remove(job_id); }
  // The firmware's JOB_DONE. A job is only completed once; a repeat or an unknown id changes nothing.
  IntelNpuJobResult complete(const IntelNpuJsm::JobDone& done);
  // Has this job been reported done? (And remove it from the table.)
  bool take_result(uint32_t job_id, IntelNpuJobResult* result);
  uint64_t in_flight() const { return jobs_.count(); }
  static uint32_t context_of(uint32_t job_id) { return (job_id >> 8) + 1; }

 private:
  struct Tracked { uint32_t ssid, cmdq_id; bool done; uint32_t status; IntelNpuJobOutcome outcome; };
  Dictionary<uint32_t, Tracked> jobs_;
  Dictionary<uint32_t, uint32_t> cursors_;  // per context: the next slot to try
};

// Submits a job: gets an id, puts the job in the queue, and rings the doorbell. On a full queue the id is given back.
enum class IntelNpuSubmit : uint8_t { Ok, NoJobId, QueueFull, QueueCorrupt, NotRegistered };
IntelNpuSubmit intel_npu_submit_job(INpuRegisters& ip, IntelNpuIp generation, IntelNpuCommandQueue& queue, const IntelNpuRegisteredQueue& registered,
                                    IntelNpuJobTracker& tracker, IntelNpuJob job, uint32_t* job_id);

// The IPC callback for the job-return channel: decodes JOB_DONE and records it in the tracker. `context` is a IntelNpuJobDoneSink.
struct IntelNpuJobDoneSink {
  IntelNpuJobTracker* tracker = nullptr;
  uint32_t completed = 0, faults = 0, ignored = 0;
  IntelNpuJobResult last;
  static void on_message(void* context, const IntelNpuIpcHeader& header, const IntelNpuJsmMessage* message);
};

}  // namespace hardware

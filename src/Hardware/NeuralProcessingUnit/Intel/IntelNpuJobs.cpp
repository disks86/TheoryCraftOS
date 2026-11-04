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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuJobs.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuPageTable.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kFirstUserSsid = 2;  // contexts 0 (global) and 1 (reserved) never run jobs
}

// ---- the job queue ----

bool IntelNpuCommandQueue::init(ByteView memory, uint64_t vpu_addr, uint32_t flags, bool snoop) {
  entry_count_ = 0;
  if (memory.size() < kHeaderSize + 2 * kEntrySize || (vpu_addr & 63) != 0) return false;
  const uint64_t entries = (memory.size() - kHeaderSize) / kEntrySize;
  if (entries > UINT32_MAX) return false;
  memory_ = memory;
  vpu_addr_ = vpu_addr;
  snoop_ = snoop;
  entry_count_ = static_cast<uint32_t>(entries);
  tail_ = 0;
  memory_.first(kHeaderSize).fill(0);
  write32(kEngineIdxOffset, IntelNpuJsm::kEngineCompute);
  write32(kFlagsOffset, flags);
  flush(0, kHeaderSize);
  return true;
}

void IntelNpuCommandQueue::write32(uint64_t offset, uint32_t value) {
  ByteWriter w(memory_.subview(offset, 4));
  w.le32(value);
}
void IntelNpuCommandQueue::write64(uint64_t offset, uint64_t value) {
  ByteWriter w(memory_.subview(offset, 8));
  w.le64(value);
}
void IntelNpuCommandQueue::flush(uint64_t offset, uint64_t size) {
  if (!snoop_) intel_npu_flush_cache(ConstByteView(memory_.subview(offset, size)));
}

uint32_t IntelNpuCommandQueue::read_head() {
  flush(kHeadOffset, 4);  // the NPU wrote it
  ByteReader r(ConstByteView(memory_.subview(kHeadOffset, 4)));
  return r.le32();
}

void IntelNpuCommandQueue::reset() {
  if (!ready()) return;
  tail_ = 0;
  write32(kHeadOffset, 0);
  write32(kTailOffset, 0);
  flush(0, kHeaderSize);
}

uint32_t IntelNpuCommandQueue::pending() {
  if (!ready()) return 0;
  const uint32_t head = read_head();
  if (head >= entry_count_) return UINT32_MAX;
  return (tail_ + entry_count_ - head) % entry_count_;
}

IntelNpuCommandQueue::PushResult IntelNpuCommandQueue::push(const IntelNpuJob& job) {
  if (!ready()) return PushResult::NotReady;
  const uint32_t head = read_head();
  if (head >= entry_count_) return PushResult::Corrupt;  // the NPU is the only writer of head: a value outside the queue means it is not behaving
  const uint32_t next = (tail_ + 1) % entry_count_;
  if (next == head) return PushResult::Full;

  const uint64_t at = kHeaderSize + uint64_t{tail_} * kEntrySize;
  ByteView entry = memory_.subview(at, kEntrySize);
  entry.fill(0);  // reserved fields and the stale parts of the last use
  ByteWriter w(entry);
  w.le64(job.batch_buffer);
  w.le32(job.job_id);
  w.le32(job.null_submission ? kJobNullSubmission : 0);
  w.zeros(8);   // doorbell_timestamp
  w.zeros(8);   // host_tracking_id
  w.le64(job.primary_preempt_addr);
  w.le32(job.primary_preempt_size);
  w.le32(job.secondary_preempt_size);
  w.le64(job.secondary_preempt_addr);
  flush(at, kEntrySize);
  // The entry must be visible before the tail that publishes it.
  __atomic_thread_fence(__ATOMIC_RELEASE);
  tail_ = next;
  write32(kTailOffset, tail_);
  flush(kTailOffset, 4);
  return PushResult::Ok;
}

// ---- doorbells ----

bool IntelNpuDoorbells::allocate(uint32_t* id) {
  for (uint32_t tried = 0; tried < kMax; ++tried) {
    const uint32_t candidate = next_;
    next_ = next_ >= kMax ? kMin : next_ + 1;
    if (used_[candidate]) continue;
    used_[candidate] = true;
    ++in_use_;
    *id = candidate;
    return true;
  }
  return false;
}

void IntelNpuDoorbells::release(uint32_t id) {
  if (id < kMin || id > kMax || !used_[id]) return;
  used_[id] = false;
  --in_use_;
}

bool IntelNpuDoorbells::ring(INpuRegisters& ip, IntelNpuIp generation, uint32_t id) {
  if (id < kMin || id > kMax || generation == IntelNpuIp::Unknown) return false;
  ip.write32((generation == IntelNpuIp::Ip37xx ? kBase37xx : kBaseLater) + uint64_t{id} * kStride, 1);
  return true;
}

// ---- registering queues with the firmware ----

IntelNpuIpcStatus IntelNpuQueueManager::call(IntelNpuJsmMessage request) {
  IntelNpuJsmMessage response;
  return ipc_.transact(request, &response, reply_tries_);
}

IntelNpuQueueStatus IntelNpuQueueManager::register_queue(IntelNpuRegisteredQueue& q, const IntelNpuCommandQueue& jobq, uint32_t process_id) {
  if (q.doorbell != 0) return IntelNpuQueueStatus::Ok;  // already registered
  if (!jobq.ready() || q.ssid < kFirstUserSsid) return IntelNpuQueueStatus::Invalid;
  if (q.hardware_scheduler) {
    q.last_ipc_status = call(IntelNpuJsm::hws_create_cmdq(q.ssid, q.ssid, q.cmdq_id, process_id, IntelNpuJsm::kEngineCompute, jobq.vpu_address(),
                                                          static_cast<uint32_t>(jobq.size())));
    if (q.last_ipc_status != IntelNpuIpcStatus::Ok) return IntelNpuQueueStatus::Ipc;
    q.last_ipc_status = call(IntelNpuJsm::hws_set_context_sched_properties(q.ssid, q.cmdq_id, q.priority_band));
    if (q.last_ipc_status != IntelNpuIpcStatus::Ok) {
      call(IntelNpuJsm::hws_destroy_cmdq(q.ssid, q.cmdq_id));
      return IntelNpuQueueStatus::Ipc;
    }
  }
  uint32_t db = 0;
  if (!doorbells_.allocate(&db)) {
    if (q.hardware_scheduler) call(IntelNpuJsm::hws_destroy_cmdq(q.ssid, q.cmdq_id));
    return IntelNpuQueueStatus::NoDoorbell;
  }
  q.last_ipc_status = call(q.hardware_scheduler ? IntelNpuJsm::hws_register_db(db, q.ssid, q.cmdq_id, jobq.vpu_address(), jobq.size())
                                                : IntelNpuJsm::register_db(db, q.ssid, jobq.vpu_address(), static_cast<uint32_t>(jobq.size())));
  if (q.last_ipc_status != IntelNpuIpcStatus::Ok) {
    doorbells_.release(db);
    if (q.hardware_scheduler) call(IntelNpuJsm::hws_destroy_cmdq(q.ssid, q.cmdq_id));
    return IntelNpuQueueStatus::Ipc;
  }
  q.doorbell = db;
  return IntelNpuQueueStatus::Ok;
}

IntelNpuQueueStatus IntelNpuQueueManager::unregister_queue(IntelNpuRegisteredQueue& q) {
  if (q.doorbell == 0) return IntelNpuQueueStatus::Ok;
  // As in ivpu, a failed message does not keep the doorbell: the firmware is being torn down or reset anyway.
  call(IntelNpuJsm::unregister_db(q.doorbell));
  if (q.hardware_scheduler) call(IntelNpuJsm::hws_destroy_cmdq(q.ssid, q.cmdq_id));
  doorbells_.release(q.doorbell);
  q.doorbell = 0;
  return IntelNpuQueueStatus::Ok;
}

// ---- jobs in flight ----

bool IntelNpuJobTracker::begin(uint32_t ssid, uint32_t cmdq_id, uint32_t* job_id) {
  if (ssid < kFirstUserSsid) return false;
  const uint32_t base = (ssid - 1) << 8;
  uint32_t* cursor = cursors_.find(ssid);
  uint32_t start = cursor ? *cursor : 0;
  for (uint32_t tried = 0; tried < kSlotsPerContext; ++tried) {
    const uint32_t slot = (start + tried) % kSlotsPerContext;
    const uint32_t id = base | slot;
    if (jobs_.contains(id)) continue;
    if (!jobs_.put(id, Tracked{ssid, cmdq_id, false, 0, IntelNpuJobOutcome::Unknown})) return false;
    if (!cursors_.put(ssid, (slot + 1) % kSlotsPerContext)) { jobs_.remove(id); return false; }
    *job_id = id;
    return true;
  }
  return false;
}

IntelNpuJobResult IntelNpuJobTracker::complete(const IntelNpuJsm::JobDone& done) {
  IntelNpuJobResult r;
  r.job_id = done.job_id;
  Tracked* t = jobs_.find(done.job_id);
  if (!t || t->done) return r;  // unknown, or reported twice
  t->done = true;
  t->status = done.job_status;
  const bool reset_needed = done.job_status == IntelNpuJsm::kStatusProcessingError ||
                            (done.job_status >= IntelNpuJsm::kStatusResetRequiredMin && done.job_status <= IntelNpuJsm::kStatusResetRequiredMax);
  t->outcome = reset_needed ? IntelNpuJobOutcome::ContextFault : IntelNpuJobOutcome::Completed;
  r.outcome = t->outcome;
  r.ssid = t->ssid;
  r.status = t->status;
  return r;
}

uint32_t IntelNpuJobTracker::abort_context(uint32_t ssid) {
  uint32_t n = 0;
  for (auto& entry : jobs_) {
    Tracked& t = entry.value;
    if (t.ssid != ssid || t.done) continue;
    t.done = true;
    t.status = IntelNpuJsm::kStatusAborted;
    t.outcome = IntelNpuJobOutcome::Aborted;
    ++n;
  }
  return n;
}

uint32_t IntelNpuJobTracker::abort_all() {
  uint32_t n = 0;
  for (auto& entry : jobs_) {
    Tracked& t = entry.value;
    if (t.done) continue;
    t.done = true;
    t.status = IntelNpuJsm::kStatusAborted;
    t.outcome = IntelNpuJobOutcome::Aborted;
    ++n;
  }
  return n;
}

bool IntelNpuJobTracker::take_result(uint32_t job_id, IntelNpuJobResult* result) {
  Tracked* t = jobs_.find(job_id);
  if (!t || !t->done) return false;
  result->outcome = t->outcome;
  result->job_id = job_id;
  result->ssid = t->ssid;
  result->status = t->status;
  jobs_.remove(job_id);
  return true;
}

IntelNpuSubmit intel_npu_submit_job(INpuRegisters& ip, IntelNpuIp generation, IntelNpuCommandQueue& queue, const IntelNpuRegisteredQueue& registered,
                                    IntelNpuJobTracker& tracker, IntelNpuJob job, uint32_t* job_id) {
  if (registered.doorbell == 0 || !queue.ready()) return IntelNpuSubmit::NotRegistered;
  uint32_t id = 0;
  if (!tracker.begin(registered.ssid, registered.cmdq_id, &id)) return IntelNpuSubmit::NoJobId;
  job.job_id = id;
  const IntelNpuCommandQueue::PushResult pushed = queue.push(job);
  if (pushed != IntelNpuCommandQueue::PushResult::Ok) {
    tracker.abandon(id);
    return pushed == IntelNpuCommandQueue::PushResult::Full ? IntelNpuSubmit::QueueFull : IntelNpuSubmit::QueueCorrupt;
  }
  if (!IntelNpuDoorbells::ring(ip, generation, registered.doorbell)) {
    tracker.abandon(id);
    return IntelNpuSubmit::NotRegistered;
  }
  *job_id = id;
  return IntelNpuSubmit::Ok;
}

void IntelNpuJobDoneSink::on_message(void* context, const IntelNpuIpcHeader&, const IntelNpuJsmMessage* message) {
  IntelNpuJobDoneSink* sink = static_cast<IntelNpuJobDoneSink*>(context);
  if (!sink || !sink->tracker || !message || message->type() != IntelNpuJsm::kJobDone || message->result() != IntelNpuJsm::kStatusSuccess) {
    if (sink) ++sink->ignored;
    return;
  }
  const IntelNpuJobResult r = sink->tracker->complete(IntelNpuJsm::parse_job_done(*message));
  sink->last = r;
  if (r.outcome == IntelNpuJobOutcome::Completed) ++sink->completed;
  else if (r.outcome == IntelNpuJobOutcome::ContextFault) {
    ++sink->faults;
    if (sink->fault_count < IntelNpuJobDoneSink::kMaxFaultedContexts) sink->faulted_contexts[sink->fault_count++] = r.ssid;
  }
  else ++sink->ignored;
}

}  // namespace hardware

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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuDevice.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint64_t kPage = 4096;
constexpr uint32_t kMaxFaultsPerService = 64;
constexpr uint64_t round_up_page(uint64_t v) { return (v + (kPage - 1)) & ~(kPage - 1); }
}  // namespace

const char* intel_npu_device_error_name(IntelNpuDeviceError error) {
  switch (error) {
    case IntelNpuDeviceError::None: return "none";
    case IntelNpuDeviceError::BadState: return "wrong state";
    case IntelNpuDeviceError::Platform: return "not an Intel NPU";
    case IntelNpuDeviceError::Registers: return "registers did not answer";
    case IntelNpuDeviceError::Firmware: return "firmware refused";
    case IntelNpuDeviceError::Memory: return "out of memory";
    case IntelNpuDeviceError::PowerUp: return "power-up failed";
    case IntelNpuDeviceError::Mmu: return "MMU failed";
    case IntelNpuDeviceError::BootParams: return "boot parameters refused";
    case IntelNpuDeviceError::CpuBoot: return "CPU start failed";
    case IntelNpuDeviceError::BootTimeout: return "firmware did not report ready";
    case IntelNpuDeviceError::Ipc: return "message failed";
    case IntelNpuDeviceError::NoSuchContext: return "no such context";
    case IntelNpuDeviceError::NoSuchQueue: return "no such queue";
    case IntelNpuDeviceError::NoRoom: return "no room";
    case IntelNpuDeviceError::Faulted: return "context faulted";
    case IntelNpuDeviceError::QueueFull: return "queue full";
    case IntelNpuDeviceError::Invalid: return "invalid argument";
  }
  return "?";
}

IntelNpuDevice::IntelNpuDevice(const IntelNpuDeviceConfig& config)
    : config_(config),
      platform_(intel_npu_platform(config.device_id)),
      map_(IntelNpuMemoryMap::for_ip(platform_.ip)),
      hw_(*config.ip, *config.buttress, platform_),
      irq_(*config.ip, *config.buttress, platform_),
      mmu_(*config.ip, *config.allocator, config.snoop),
      ipc_(*config.ip, platform_.ip, config.snoop),
      tracker_(*config.allocator),
      queues_(ipc_, doorbells_, config.reply_tries) {
  job_sink_.tracker = &tracker_;
}

IntelNpuDevice::~IntelNpuDevice() {
  if (state_ == IntelNpuDeviceState::Running) {
    quiesce();
    hw_.power_down();
  }
  for (UniquePointer<UserContext>& c : contexts_) {
    if (!c) continue;
    for (UniquePointer<Queue>& q : c->queues)
      if (q) free_buffer(q->buffer);
    c.reset();
  }
  ipc_.remove_consumer(job_consumer_);
  free_buffer(shave_nn_);
  free_buffer(log_verbose_);
  free_buffer(log_critical_);
  free_buffer(ipc_rx_);
  free_buffer(ipc_tx_);
  free_buffer(runtime_);
  free_buffer(fw_version_);
  free_buffer(boot_params_);
}

IntelNpuDeviceError IntelNpuDevice::fail(IntelNpuDeviceError error) {
  last_error_ = error;
  return error;
}

IntelNpuDeviceError IntelNpuDevice::ipc_fail(IntelNpuIpcStatus status) {
  ipc_status_ = status;
  return fail(IntelNpuDeviceError::Ipc);
}

// ---- memory ----

bool IntelNpuDevice::alloc_memory(Buffer& buffer, uint64_t size) {
  size = round_up_page(size);
  buffer.memory = allocate_dma_view<uint8_t>(*config_.allocator, size, kPage);
  if (buffer.memory.empty()) return false;
  buffer.memory.fill(0);
  return true;
}

bool IntelNpuDevice::map_global(Buffer& buffer, const IntelNpuAddressRange& range) {
  if (buffer.memory.empty() || !global_) return false;
  uint64_t addr = 0;
  if (!global_->address_space().allocate(range.start, range.end, buffer.memory.size(), &addr)) return false;
  const IntelNpuDmaExtent extent[1] = {{intel_npu_bus_address(buffer.memory.data()), buffer.memory.size()}};
  if (!global_->map(addr, ArrayView<const IntelNpuDmaExtent>(extent), buffer.memory.size(), IntelNpuPageTable::MapFlags())) {
    global_->address_space().release(addr, buffer.memory.size());
    return false;
  }
  buffer.vpu_addr = addr;
  buffer.mapped = true;
  return true;
}

void IntelNpuDevice::free_buffer(Buffer& buffer) {
  if (buffer.mapped && global_) {
    const IntelNpuDmaExtent extent[1] = {{intel_npu_bus_address(buffer.memory.data()), buffer.memory.size()}};
    global_->unmap(buffer.vpu_addr, ArrayView<const IntelNpuDmaExtent>(extent));
    global_->address_space().release(buffer.vpu_addr, buffer.memory.size());
  }
  buffer.mapped = false;
  buffer.vpu_addr = 0;
  release_view(*config_.allocator, buffer.memory);
}

IntelNpuFirmwareMemory IntelNpuDevice::firmware_memory() const {
  return {{intel_npu_bus_address(boot_params_.memory.data()), boot_params_.memory.size()},
          {intel_npu_bus_address(fw_version_.memory.data()), fw_version_.memory.size()},
          {intel_npu_bus_address(runtime_.memory.data()), runtime_.memory.size()}};
}

// ---- loading and booting ----

IntelNpuDeviceError IntelNpuDevice::load_firmware(ConstByteView file) {
  if (state_ != IntelNpuDeviceState::Created) return fail(IntelNpuDeviceError::BadState);
  if (!platform_.valid() || platform_.ip == IntelNpuIp::Unknown) return fail(IntelNpuDeviceError::Platform);
  if (!hw_.read_info().valid) return fail(IntelNpuDeviceError::Registers);
  firmware_ = IntelNpuFirmware::parse(file, platform_.ip, config_.scheduler);
  firmware_error_ = firmware_.error;
  if (!firmware_.valid()) return fail(IntelNpuDeviceError::Firmware);
  firmware_file_ = file;
  const bool ok = alloc_memory(boot_params_, firmware_.boot_params_size) && alloc_memory(fw_version_, firmware_.fw_version_size) &&
                  alloc_memory(runtime_, firmware_.runtime_size) && alloc_memory(ipc_tx_, kIpcAreaBytes) && alloc_memory(ipc_rx_, kIpcAreaBytes) &&
                  alloc_memory(log_critical_, kCriticalLogBytes) && alloc_memory(log_verbose_, kVerboseLogBytes) &&
                  (firmware_.shave_nn_size == 0 || alloc_memory(shave_nn_, firmware_.shave_nn_size));
  if (!ok) {
    free_buffer(shave_nn_); free_buffer(log_verbose_); free_buffer(log_critical_); free_buffer(ipc_rx_); free_buffer(ipc_tx_);
    free_buffer(runtime_); free_buffer(fw_version_); free_buffer(boot_params_);
    return fail(IntelNpuDeviceError::Memory);
  }
  state_ = IntelNpuDeviceState::Loaded;
  last_error_ = IntelNpuDeviceError::None;
  return IntelNpuDeviceError::None;
}

// What only the first boot does, once the MMU is on: the global and reserved contexts, the firmware's fixed mappings, the shared buffers and the message channel.
IntelNpuDeviceError IntelNpuDevice::first_boot_setup() {
  global_ = UniquePointer<IntelNpuMmuContext>::make(*config_.allocator, *config_.allocator, mmu_, map_, IntelNpuMmuContext::kGlobalSsid, config_.snoop);
  reserved_ = UniquePointer<IntelNpuMmuContext>::make(*config_.allocator, *config_.allocator, mmu_, map_, IntelNpuMmuContext::kReservedSsid, config_.snoop);
  if (!global_ || !reserved_) return IntelNpuDeviceError::Memory;
  if (!reserved_->init_reserved()) return IntelNpuDeviceError::Mmu;
  IntelNpuAddressSpace& space = global_->address_space();
  if (!space.reserve(firmware_.boot_params_addr, firmware_.boot_params_size) || !space.reserve(firmware_.fw_version_addr, firmware_.fw_version_size) ||
      !space.reserve(firmware_.runtime_addr, firmware_.runtime_size))
    return IntelNpuDeviceError::Memory;
  if (!intel_npu_map_firmware(*global_, firmware_, firmware_memory())) return IntelNpuDeviceError::Mmu;
  if (!map_global(ipc_tx_, map_.global) || !map_global(ipc_rx_, map_.global) || !map_global(log_critical_, map_.global) || !map_global(log_verbose_, map_.global))
    return IntelNpuDeviceError::Memory;
  if (shave_nn_.memory.size() != 0 && !map_global(shave_nn_, map_.shave)) return IntelNpuDeviceError::Memory;
  if (!ipc_.init(ByteView(ipc_tx_.memory.data(), ipc_tx_.memory.size()), static_cast<uint32_t>(ipc_tx_.vpu_addr),
                 ByteView(ipc_rx_.memory.data(), ipc_rx_.memory.size()), static_cast<uint32_t>(ipc_rx_.vpu_addr)))
    return IntelNpuDeviceError::Memory;
  if (!ipc_.add_consumer(job_consumer_, IntelNpuJsm::kChannelJobReturn, &IntelNpuJobDoneSink::on_message, &job_sink_)) return IntelNpuDeviceError::Memory;
  set_up_ = true;
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::boot_firmware(uint64_t system_time_us) {
  hw_step_ = IntelNpuStep::None;
  const IntelNpuResult up = hw_.power_up();
  if (!up.ok) { hw_step_ = up.failed; return IntelNpuDeviceError::PowerUp; }
  if (mmu_initialized_ ? !mmu_.enable() : !mmu_.init()) return IntelNpuDeviceError::Mmu;
  mmu_initialized_ = true;
  if (!set_up_) {
    const IntelNpuDeviceError e = first_boot_setup();
    if (e != IntelNpuDeviceError::None) return e;
  }
  // A cold boot starts from empty message areas and nothing known to the firmware.
  ipc_.disable();
  if (!ipc_.reset()) return IntelNpuDeviceError::Memory;
  hw_.prepare_boot(config_.snoop);

  if (!IntelNpuBoot::load_image(ByteView(runtime_.memory.data(), runtime_.memory.size()), firmware_)) return IntelNpuDeviceError::Memory;
  IntelNpuBootInputs in;
  in.pci_bus = config_.pci_bus;
  in.si_stepping = config_.revision;
  in.device_id = config_.device_id;
  in.min_ratio = hw_.info().min_ratio;
  in.pn_ratio = hw_.info().pn_ratio;
  in.max_ratio = hw_.info().max_ratio;
  in.tile_fuse = hw_.info().tile_disable_mask;
  in.ipc_rx_addr = ipc_rx_.vpu_addr;
  in.ipc_rx_size = static_cast<uint32_t>(ipc_rx_.memory.size());
  in.shave_nn_addr = shave_nn_.vpu_addr;
  in.crit_log_addr = log_critical_.vpu_addr;
  in.crit_log_size = static_cast<uint32_t>(log_critical_.memory.size());
  in.verbose_log_addr = log_verbose_.vpu_addr;
  in.verbose_log_size = static_cast<uint32_t>(log_verbose_.memory.size());
  in.system_time_us = system_time_us;
  if (IntelNpuBoot::build_cold(ByteView(boot_params_.memory.data(), boot_params_.memory.size()), firmware_, platform_.ip, map_, in) !=
      IntelNpuBootParamsError::None)
    return IntelNpuDeviceError::BootParams;
  if (!config_.snoop) {
    intel_npu_flush_cache(ConstByteView(boot_params_.memory.data(), boot_params_.memory.size()));
    intel_npu_flush_cache(ConstByteView(runtime_.memory.data(), runtime_.memory.size()));
  }

  const IntelNpuResult boot = hw_.boot(firmware_.cold_boot_entry_point);
  if (!boot.ok) { hw_step_ = boot.failed; return IntelNpuDeviceError::CpuBoot; }
  const IntelNpuIpcStatus ready = ipc_.wait_for_boot_message(config_.boot_tries);
  if (ready != IntelNpuIpcStatus::Ok) { ipc_status_ = ready; return IntelNpuDeviceError::BootTimeout; }
  irq_.clear();
  irq_.enable();
  ipc_.enable();
  if (firmware_.sched_mode == kIntelNpuSchedHw) {
    IntelNpuJsmMessage reply;
    const IntelNpuIpcStatus s = ipc_.transact(IntelNpuJsm::priority_band_setup(IntelNpuJsm::PriorityBands()), &reply, config_.reply_tries);
    if (s != IntelNpuIpcStatus::Ok) { ipc_status_ = s; return IntelNpuDeviceError::Ipc; }
  }
  return IntelNpuDeviceError::None;
}

void IntelNpuDevice::forget_queues() {
  doorbells_ = IntelNpuDoorbells();
  for (UniquePointer<UserContext>& c : contexts_) {
    if (!c) continue;
    for (UniquePointer<Queue>& q : c->queues) {
      if (!q) continue;
      q->reg.doorbell = 0;
      q->jobq.reset();
    }
  }
}

// ivpu_prepare_for_reset(): nothing more arrives from the NPU, and nothing more is asked of it.
void IntelNpuDevice::quiesce() {
  irq_.disable();
  ipc_.disable();
  mmu_.disable();
  tracker_.abort_all();
  forget_queues();
}

IntelNpuDeviceError IntelNpuDevice::start(uint64_t system_time_us) {
  if (state_ != IntelNpuDeviceState::Loaded && state_ != IntelNpuDeviceState::Suspended && state_ != IntelNpuDeviceState::Failed) return fail(IntelNpuDeviceError::BadState);
  const IntelNpuDeviceError e = boot_firmware(system_time_us);
  if (e != IntelNpuDeviceError::None) {
    quiesce();
    hw_.power_down();
    state_ = IntelNpuDeviceState::Failed;
    return fail(e);
  }
  state_ = IntelNpuDeviceState::Running;
  last_error_ = IntelNpuDeviceError::None;
  ++boot_count_;
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::suspend() {
  if (state_ != IntelNpuDeviceState::Running) return fail(IntelNpuDeviceError::BadState);
  quiesce();
  const IntelNpuResult down = hw_.power_down();
  state_ = IntelNpuDeviceState::Suspended;
  if (!down.ok) {
    hw_step_ = down.failed;
    return fail(IntelNpuDeviceError::PowerUp);   // the NPU is as off as it can be made; the step says what refused
  }
  last_error_ = IntelNpuDeviceError::None;
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::recover(uint64_t system_time_us) {
  if (state_ == IntelNpuDeviceState::Created) return fail(IntelNpuDeviceError::BadState);
  if (state_ == IntelNpuDeviceState::Running) suspend();   // best effort: a hung NPU may refuse parts of it
  return start(system_time_us);
}

// ---- running ----

IntelNpuDevice::UserContext* IntelNpuDevice::context(uint32_t ssid) {
  if (ssid < IntelNpuMmuContext::kUserMinSsid || ssid > IntelNpuMmuContext::kUserMaxSsid) return nullptr;
  return contexts_[ssid - IntelNpuMmuContext::kUserMinSsid].get();
}
const IntelNpuDevice::UserContext* IntelNpuDevice::context(uint32_t ssid) const {
  if (ssid < IntelNpuMmuContext::kUserMinSsid || ssid > IntelNpuMmuContext::kUserMaxSsid) return nullptr;
  return contexts_[ssid - IntelNpuMmuContext::kUserMinSsid].get();
}

bool IntelNpuDevice::context_faulted(uint32_t ssid) const {
  const UserContext* c = context(ssid);
  return c && c->faulted;
}

void IntelNpuDevice::handle_faults(IntelNpuServiceResult& result) {
  IntelNpuMmuEvent ev;
  for (uint32_t i = 0; i < kMaxFaultsPerService && mmu_.next_event(&ev); ++i) {
    ++total_mmu_faults_;
    ++result.mmu_faults;
    if (ev.ssid == IntelNpuMmuContext::kGlobalSsid || ev.ssid == IntelNpuMmuContext::kReservedSsid) {
      result.recovery_needed = true;   // the firmware's own memory: ivpu recovers the whole NPU
      continue;
    }
    UserContext* c = context(ev.ssid);
    if (!c || c->faulted) continue;
    c->faulted = true;
    ++result.faulted_contexts;
    tracker_.abort_context(ev.ssid);
    mmu_.disable_context_events(ev.ssid);
  }
  // A job that came back with a fault that needs the engine reset (ivpu: reset_engine_and_mark_faulty_contexts).
  if (job_sink_.fault_count != 0) {
    for (uint32_t i = 0; i < job_sink_.fault_count; ++i) {
      UserContext* c = context(job_sink_.faulted_contexts[i]);
      if (!c || c->faulted) continue;
      c->faulted = true;
      ++result.faulted_contexts;
      tracker_.abort_context(job_sink_.faulted_contexts[i]);
    }
    job_sink_.fault_count = 0;
    IntelNpuJsmMessage reply;
    bool ok = ipc_.transact(IntelNpuJsm::engine_reset(IntelNpuJsm::kEngineCompute), &reply, config_.reply_tries) == IntelNpuIpcStatus::Ok;
    if (ok && firmware_.sched_mode == kIntelNpuSchedHw)
      ok = ipc_.transact(IntelNpuJsm::hws_resume_engine(IntelNpuJsm::kEngineCompute), &reply, config_.reply_tries) == IntelNpuIpcStatus::Ok;
    ++engine_resets_;
    ++result.engine_resets;
    if (!ok) result.recovery_needed = true;
  }
}

IntelNpuServiceResult IntelNpuDevice::service() {
  IntelNpuServiceResult result;
  if (state_ != IntelNpuDeviceState::Running) return result;
  const IntelNpuIrqResult irq = irq_.handle(&ipc_, &mmu_);
  result.handled = irq.handled;
  result.recovery_needed = irq.recovery_needed;
  ipc_.run_callbacks();
  if (irq.mmu_events || job_sink_.fault_count != 0) handle_faults(result);
  return result;
}

IntelNpuDeviceError IntelNpuDevice::create_context(uint32_t* ssid) {
  if (state_ != IntelNpuDeviceState::Running || !ssid) return fail(IntelNpuDeviceError::BadState);
  for (uint32_t i = 0; i < kMaxUserContexts; ++i) {
    if (contexts_[i]) continue;
    const uint32_t id = IntelNpuMmuContext::kUserMinSsid + i;
    contexts_[i] = UniquePointer<UserContext>::make(*config_.allocator, *config_.allocator, mmu_, map_, id, config_.snoop);
    if (!contexts_[i]) return fail(IntelNpuDeviceError::Memory);
    *ssid = id;
    return IntelNpuDeviceError::None;
  }
  return fail(IntelNpuDeviceError::NoRoom);
}

IntelNpuDeviceError IntelNpuDevice::destroy_context(uint32_t ssid) {
  UserContext* c = context(ssid);
  if (!c) return fail(IntelNpuDeviceError::NoSuchContext);
  tracker_.abort_context(ssid);
  const bool running = state_ == IntelNpuDeviceState::Running;
  for (UniquePointer<Queue>& q : c->queues) {
    if (!q) continue;
    if (running) queues_.unregister_queue(q->reg);
    free_buffer(q->buffer);
    q.reset();
  }
  if (running) {
    IntelNpuJsmMessage reply;
    ipc_.transact(IntelNpuJsm::ssid_release(ssid), &reply, config_.reply_tries);   // a context the firmware never saw is answered like any other
  }
  contexts_[ssid - IntelNpuMmuContext::kUserMinSsid].reset();
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::map_buffer(uint32_t ssid, ArrayView<const IntelNpuDmaExtent> extents, uint64_t size, IntelNpuPageTable::MapFlags flags,
                                               uint64_t* vpu_addr) {
  UserContext* c = context(ssid);
  if (!c || !vpu_addr) return fail(IntelNpuDeviceError::NoSuchContext);
  if (c->faulted) return fail(IntelNpuDeviceError::Faulted);
  uint64_t addr = 0;
  if (!c->mmu.address_space().allocate(map_.user.start, map_.user.end, size, &addr)) return fail(IntelNpuDeviceError::NoRoom);
  if (!c->mmu.map(addr, extents, size, flags)) {
    c->mmu.address_space().release(addr, size);
    return fail(IntelNpuDeviceError::Mmu);
  }
  *vpu_addr = addr;
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::unmap_buffer(uint32_t ssid, uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents) {
  UserContext* c = context(ssid);
  if (!c) return fail(IntelNpuDeviceError::NoSuchContext);
  uint64_t size = 0;
  for (const IntelNpuDmaExtent& e : extents) {
    if (e.length > IntelNpuPageTable::kAddressLimit - size) return fail(IntelNpuDeviceError::Invalid);
    size += e.length;
  }
  if (!c->mmu.address_space().release(vpu_addr, size)) return fail(IntelNpuDeviceError::Invalid);   // not a range we handed out: leave the mapping alone
  c->mmu.unmap(vpu_addr, extents);
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::create_queue(uint32_t ssid, uint32_t priority_band, uint32_t* queue_id) {
  UserContext* c = context(ssid);
  if (!c || !queue_id) return fail(IntelNpuDeviceError::NoSuchContext);
  if (c->faulted) return fail(IntelNpuDeviceError::Faulted);
  if (priority_band >= IntelNpuJsm::kBandCount) return fail(IntelNpuDeviceError::Invalid);
  for (uint32_t i = 0; i < kMaxQueuesPerContext; ++i) {
    if (c->queues[i]) continue;
    UniquePointer<Queue> q = UniquePointer<Queue>::make(*config_.allocator);
    if (!q) return fail(IntelNpuDeviceError::Memory);
    if (!alloc_memory(q->buffer, kJobQueueBytes) || !map_global(q->buffer, map_.global)) {
      free_buffer(q->buffer);
      return fail(IntelNpuDeviceError::Memory);
    }
    if (!q->jobq.init(ByteView(q->buffer.memory.data(), q->buffer.memory.size()), q->buffer.vpu_addr, 0, config_.snoop)) {
      free_buffer(q->buffer);
      return fail(IntelNpuDeviceError::Invalid);
    }
    q->reg.ssid = ssid;
    q->reg.cmdq_id = i + 1;
    q->reg.priority_band = priority_band;
    q->reg.hardware_scheduler = firmware_.sched_mode == kIntelNpuSchedHw;
    c->queues[i] = util::move(q);
    *queue_id = i + 1;
    return IntelNpuDeviceError::None;
  }
  return fail(IntelNpuDeviceError::NoRoom);
}

IntelNpuDeviceError IntelNpuDevice::destroy_queue(uint32_t ssid, uint32_t queue_id) {
  UserContext* c = context(ssid);
  if (!c) return fail(IntelNpuDeviceError::NoSuchContext);
  if (queue_id == 0 || queue_id > kMaxQueuesPerContext || !c->queues[queue_id - 1]) return fail(IntelNpuDeviceError::NoSuchQueue);
  UniquePointer<Queue>& q = c->queues[queue_id - 1];
  if (state_ == IntelNpuDeviceState::Running) queues_.unregister_queue(q->reg);
  free_buffer(q->buffer);
  q.reset();
  return IntelNpuDeviceError::None;
}

IntelNpuDeviceError IntelNpuDevice::submit(uint32_t ssid, uint32_t queue_id, const IntelNpuJob& job, uint32_t* job_id) {
  if (state_ != IntelNpuDeviceState::Running) return fail(IntelNpuDeviceError::BadState);
  UserContext* c = context(ssid);
  if (!c || !job_id) return fail(IntelNpuDeviceError::NoSuchContext);
  if (c->faulted) return fail(IntelNpuDeviceError::Faulted);
  if (queue_id == 0 || queue_id > kMaxQueuesPerContext || !c->queues[queue_id - 1]) return fail(IntelNpuDeviceError::NoSuchQueue);
  Queue& q = *c->queues[queue_id - 1];
  if (q.reg.doorbell == 0) {
    q.jobq.reset();
    const IntelNpuQueueStatus s = queues_.register_queue(q.reg, q.jobq, config_.process_id);
    if (s == IntelNpuQueueStatus::NoDoorbell) return fail(IntelNpuDeviceError::NoRoom);
    if (s == IntelNpuQueueStatus::Ipc) return ipc_fail(q.reg.last_ipc_status);
    if (s != IntelNpuQueueStatus::Ok) return fail(IntelNpuDeviceError::Invalid);
  }
  switch (intel_npu_submit_job(*config_.ip, platform_.ip, q.jobq, q.reg, tracker_, job, job_id)) {
    case IntelNpuSubmit::Ok: return IntelNpuDeviceError::None;
    case IntelNpuSubmit::QueueFull: return fail(IntelNpuDeviceError::QueueFull);
    case IntelNpuSubmit::NoJobId: return fail(IntelNpuDeviceError::NoRoom);
    default: return fail(IntelNpuDeviceError::Invalid);   // a corrupt queue or a doorbell that is gone
  }
}

IntelNpuDeviceError IntelNpuDevice::heartbeat(uint64_t* value) {
  if (state_ != IntelNpuDeviceState::Running || !value) return fail(IntelNpuDeviceError::BadState);
  IntelNpuJsmMessage reply;
  const IntelNpuIpcStatus s = ipc_.transact(IntelNpuJsm::query_heartbeat(IntelNpuJsm::kEngineCompute), &reply, config_.reply_tries);
  if (s != IntelNpuIpcStatus::Ok) return ipc_fail(s);
  *value = IntelNpuJsm::parse_heartbeat(reply);
  return IntelNpuDeviceError::None;
}

}  // namespace hardware

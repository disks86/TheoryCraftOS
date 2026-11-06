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

#include "Hardware/StorageDevice/Usb/UasStorageDevice.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

namespace {
constexpr uint8_t kStandardInterfaceOut = 0x01, kSetInterface = 11;
constexpr uint32_t kCommandBytes = 64, kStatusBytes = 1024, kSmallStride = 2048;   // per tag: the Command IU, then (at 1024) the status
constexpr uint64_t kTimeoutUs = 10000000;           // a command that has not ended in 10 s is given up
constexpr uint64_t kDataAfterStatusUs = 200000;     // the data stage of a command whose status is in, before it is taken back
constexpr uint32_t kStatus = 0, kData = 1, kCommand = 2;
constexpr uint8_t kSenseUnitAttention = 0x06, kSenseNotReady = 0x02;

void pause() {
  if (kernel::running()) kernel::yield();
  else kernel::cpu_relax();
}
}  // namespace

bool UasStorageDevice::attach(IUsbHostController& host, uint32_t port, uint8_t configuration, ConstByteView descriptors,
                              allocators::IAllocator& allocator, efi::SystemTable* st, List<UniquePointer<IStorageDevice>>& out) {
  uas::UasInterface uas;
  if (!uas::find_uas_interface(descriptors, &uas)) return false;
  if (!host.set_configuration(port, configuration)) return false;
  if (!host.control_out(port, kStandardInterfaceOut, kSetInterface, uas.alternate, uas.number)) return false;
  // Streams when the device and the controller have them (SuperSpeed); a command needs the status and the data pipes on its stream.
  uint32_t streams = uas.streams < host.max_bulk_streams(port) ? uas.streams : host.max_bulk_streams(port);
  if (streams > kMaxTags) streams = kMaxTags;
  if (streams < 2 || !host.supports_async_bulk()) streams = 0;
  const uint8_t command = uas.endpoint[uas::kPipeCommand - 1];
  bool ok = host.start_bulk_endpoint(port, command, uas.max_packet[uas::kPipeCommand - 1]);
  for (uint8_t pipe = uas::kPipeStatus; ok && pipe <= uas::kPipeDataOut; ++pipe)
    ok = streams ? host.start_bulk_endpoint_streams(port, uas.endpoint[pipe - 1], uas.max_packet[pipe - 1], streams)
                 : host.start_bulk_endpoint(port, uas.endpoint[pipe - 1], uas.max_packet[pipe - 1]);
  if (ok) {
    UniquePointer<UasStorageDevice> device = UniquePointer<UasStorageDevice>::make(allocator, host, port, uas, streams, allocator, st);
    if (device && device->initialize()) {
      out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
      return true;
    }
    if (device) device->shutdown();
  }
  host.control_out(port, kStandardInterfaceOut, kSetInterface, 0, uas.number);   // back to Bulk-Only (alternate setting 0)
  return false;
}

bool UasStorageDevice::present() const {
  return !__atomic_load_n(&shut_down_, __ATOMIC_ACQUIRE) && host_.port_generation(port_) == generation_;
}

bool UasStorageDevice::initialize() {
  tag_count_ = streams_ ? streams_ : 1;
  if (tag_count_ > kMaxTags) tag_count_ = kMaxTags;
  // Each tag's data buffer is one 64 KiB window (a bulk transfer must not cross one); the small buffers share a page or two.
  data_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{tag_count_} * kSlotBytes, kSlotBytes);
  small_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{tag_count_} * kSmallStride, 4096);
  if (data_memory_.empty() || small_memory_.empty()) return false;
  small_memory_.fill(0);
  for (uint32_t t = 0; t < tag_count_; ++t) {
    Tag& g = tags_[t];
    g.data = data_memory_.subview(uint64_t{t} * kSlotBytes, kSlotBytes);
    g.command = small_memory_.subview(uint64_t{t} * kSmallStride, kCommandBytes);
    g.status = small_memory_.subview(uint64_t{t} * kSmallStride + 1024, kStatusBytes);
  }
  pool_.reset(tag_count_);

  uint8_t data[64] = {};
  const uint8_t inquiry[6] = {scsi::kInquiry, 0, 0, 0, 36, 0};
  if (!run(ConstByteView(inquiry), ByteView(data).first(36), true)) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(ConstByteView(data), &unit) || !unit.present || unit.device_type != scsi::kDeviceTypeDisk) return false;
  info_.kind = StorageKind::Disk;
  info_.removable = unit.removable;
  info_.media = StorageMedia::Removable;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);
  const uint8_t ready[6] = {scsi::kTestUnitReady};
  for (int i = 0; i < 10 && !run(ConstByteView(ready), ByteView(), false); ++i) {}
  const uint8_t capacity10[10] = {scsi::kReadCapacity10};
  uint64_t last = 0;
  if (!run(ConstByteView(capacity10), ByteView(data).first(8), true) || !scsi::parse_capacity10(ConstByteView(data), &last, &info_.block_size)) return false;
  if (last == 0xFFFFFFFF) {
    const uint8_t capacity16[16] = {scsi::kServiceActionIn16, scsi::kReadCapacity16Action, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    if (!run(ConstByteView(capacity16), ByteView(data).first(32), true) || !scsi::parse_capacity16(ConstByteView(data), &last, &info_.block_size)) return false;
  }
  info_.block_count = last + 1;
  if (info_.block_size < 512 || info_.block_size > 4096) return false;
  info_.queue_depth = tag_count_;
  auto runner = [this](ConstByteView cdb, ByteView buffer, bool in) { return run(cdb, buffer, in); };
  uint8_t scratch[64];
  unmap_ = scsi::probe_unmap(runner, ByteView(scratch));
  info_.discard = unmap_.unmap;
  info_.discard_zeroes = unmap_.unmap && unmap_.reads_zeros;
  write_same_max_ = scsi::probe_write_same(runner, ByteView(scratch));
  info_.write_zeroes = write_same_max_ != 0;
  return true;
}

void UasStorageDevice::shutdown() {
  kernel::ScopedMutex guard(mutex_);
  __atomic_store_n(&shut_down_, true, __ATOMIC_RELEASE);
  pool_.close();
  for (Tag& g : tags_) g = Tag{};
  release_view(allocator_, data_memory_);
  release_view(allocator_, small_memory_);
}

// ---- one command ----

bool UasStorageDevice::submit(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in) {
  Tag& g = tags_[t];
  const uint16_t stream = static_cast<uint16_t>(t + 1);   // (the tag and its stream)
  if (!uas::build_command_iu(g.command, stream, cdb) || bytes > kSlotBytes) return false;
  g.status.fill(0);
  for (bool& p : g.pending) p = false;
  // Status and data first, so that both are waiting when the device answers the command.
  if (!host_.submit_bulk(port_, interface_.endpoint[uas::kPipeStatus - 1], stream, g.status, &g.tokens[kStatus])) return false;
  g.pending[kStatus] = true;
  if (bytes) {
    const uint8_t pipe = data_in ? uas::kPipeDataIn : uas::kPipeDataOut;
    if (!host_.submit_bulk(port_, interface_.endpoint[pipe - 1], stream, g.data.first(bytes), &g.tokens[kData])) {
      __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);   // (the status read is queued and cannot be answered now)
      return false;
    }
    g.pending[kData] = true;
  }
  if (!host_.submit_bulk(port_, interface_.endpoint[uas::kPipeCommand - 1], 0, g.command.first(uas::kCommandIuBytes), &g.tokens[kCommand])) {
    __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
    return false;
  }
  g.pending[kCommand] = true;
  return true;
}

// Waits for the command's transfers. A data stage still queued once the status is in (a command the device ended early: CHECK
// CONDITION before any data) is taken back, so the tag's stream is clean for the next command.
UasStorageDevice::Result UasStorageDevice::finish(uint32_t t) {
  Tag& g = tags_[t];
  const uint64_t start = storage_now_us();
  uint64_t status_at = 0;
  bool ok[3] = {true, true, true};
  while (g.pending[kStatus] || g.pending[kData] || g.pending[kCommand]) {
    bool progress = false;
    for (uint32_t i = 0; i < 3; ++i) {
      if (!g.pending[i]) continue;
      bool done_ok = false;
      uint32_t moved = 0;
      if (host_.poll_bulk(port_, g.tokens[i], &done_ok, &moved)) {
        g.pending[i] = false;
        ok[i] = done_ok;
        progress = true;
        if (i == kStatus) status_at = storage_now_us();
      }
    }
    if (progress) continue;
    const uint64_t now = storage_now_us();
    if (!g.pending[kStatus] && g.pending[kData] && now - status_at > kDataAfterStatusUs) {
      if (!host_.cancel_bulk(port_, g.tokens[kData])) {
        __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
        return Result::Error;
      }
      g.pending[kData] = false;
      ok[kData] = false;
      continue;
    }
    if (now - start > kTimeoutUs || !present()) {
      __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);   // (its transfers may still be queued: the tags cannot be trusted)
      return Result::Error;
    }
    pause();
  }
  if (!ok[kStatus] || !ok[kCommand]) {
    __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
    return Result::Error;
  }
  const Result r = interpret(t);
  return r == Result::Good && !ok[kData] ? Result::Error : r;
}

UasStorageDevice::Result UasStorageDevice::interpret(uint32_t t) {
  Tag& g = tags_[t];
  uas::StatusIu iu;
  if (!uas::parse_status_iu(g.status, &iu) || iu.tag != t + 1 || iu.id != uas::kIuSense) return Result::Error;
  if (iu.status == scsi::kStatusGood) return Result::Good;
  if (iu.status != scsi::kStatusCheckCondition) return Result::Error;
  g.sense_key = iu.sense.size() > 2 ? iu.sense[2] & 0x0F : 0;   // (fixed format sense data)
  return Result::CheckCondition;
}

// Without streams: Command IU, then the status pipe says what comes next (Read Ready / Write Ready for the data, or the Sense IU).
UasStorageDevice::Result UasStorageDevice::exchange_serial(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in) {
  Tag& g = tags_[t];
  if (!uas::build_command_iu(g.command, static_cast<uint16_t>(t + 1), cdb) || bytes > kSlotBytes) return Result::Error;
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, interface_.endpoint[uas::kPipeCommand - 1], g.command.first(uas::kCommandIuBytes), &moved)) {
    __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
    return Result::Error;
  }
  for (int step = 0; step < 3; ++step) {
    g.status.fill(0);
    if (!host_.bulk_transfer(port_, interface_.endpoint[uas::kPipeStatus - 1], g.status, &moved)) {
      __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
      return Result::Error;
    }
    uas::StatusIu iu;
    if (!uas::parse_status_iu(g.status.first(moved), &iu) || iu.tag != t + 1) return Result::Error;
    if (iu.id == uas::kIuSense) return interpret(t);
    const bool ready_in = iu.id == uas::kIuReadReady, ready_out = iu.id == uas::kIuWriteReady;
    if ((!ready_in && !ready_out) || ready_in != data_in || bytes == 0) return Result::Error;
    const uint8_t pipe = data_in ? uas::kPipeDataIn : uas::kPipeDataOut;
    if (!host_.bulk_transfer(port_, interface_.endpoint[pipe - 1], g.data.first(bytes), &moved)) {
      __atomic_store_n(&broken_, true, __ATOMIC_RELEASE);
      return Result::Error;
    }
  }
  return Result::Error;
}

bool UasStorageDevice::start(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in) {
  if (__atomic_load_n(&broken_, __ATOMIC_ACQUIRE) || !present() || bytes > kSlotBytes) return false;
  if (streams_) return submit(t, cdb, bytes, data_in);
  kernel::ScopedMutex guard(mutex_);
  tags_[t].serial_result = static_cast<uint8_t>(exchange_serial(t, cdb, bytes, data_in));
  return true;
}

UasStorageDevice::Result UasStorageDevice::collect(uint32_t t) { return streams_ ? finish(t) : static_cast<Result>(tags_[t].serial_result); }

UasStorageDevice::Result UasStorageDevice::command(uint32_t t, ConstByteView cdb, ByteView data, bool data_in) {
  Tag& g = tags_[t];
  if (data.size() > kSlotBytes) return Result::Error;
  if (!data_in && !data.empty()) g.data.first(data.size()).copy_from(data);
  const Result r = start(t, cdb, data.size(), data_in) ? collect(t) : Result::Error;
  if (r == Result::Good && data_in && !data.empty()) data.copy_from(g.data.first(data.size()));
  return r;
}

bool UasStorageDevice::run(ConstByteView cdb, ByteView data, bool data_in) {
  const uint32_t t = pool_.acquire();
  bool ok = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const Result r = command(t, cdb, data, data_in);
    if (r == Result::Good) {
      ok = true;
      break;
    }
    if (r == Result::Error) break;
    const uint8_t key = tags_[t].sense_key;   // (UAS sends the sense data with the status: no REQUEST SENSE)
    if (key == kSenseNotReady) wait_for(st_, 50, [] { return false; });
    else if (key != kSenseUnitAttention) break;
  }
  pool_.release(t);
  return ok;
}

// ---- blocks ----

bool UasStorageDevice::run_blocks(bool write, uint64_t lba, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (!present() || bytes == 0 || bytes % info_.block_size != 0) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint64_t count = bytes / info_.block_size;
  if (lba >= info_.block_count || count > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint32_t bs = info_.block_size;
  const uint32_t max_blocks = kSlotBytes / bs;
  const uint64_t chunks = (count + max_blocks - 1) / max_blocks;
  uint32_t slots[kMaxParallelChunks];
  const uint32_t got = acquire_slots(pool_, slots, chunks < kMaxParallelChunks ? static_cast<uint32_t>(chunks) : kMaxParallelChunks);   // (1 without streams)
  auto send = [&](uint32_t t, uint64_t first, uint32_t n) {
    uint8_t cdb[16];
    const uint32_t length = scsi::build_read_write(ByteView(cdb), write, lba + first, n);
    if (write) tags_[t].data.first(uint64_t{n} * bs).copy_from(from.subview(first * bs, uint64_t{n} * bs));
    return start(t, ConstByteView(cdb).first(length), uint64_t{n} * bs, !write);
  };
  const bool ok = run_chunks(slots, got, count, max_blocks, send, [&](uint32_t t, uint64_t first, uint32_t n) {
    Result r = collect(t);
    for (int attempt = 0; attempt < 3 && r == Result::CheckCondition && tags_[t].sense_key == kSenseUnitAttention; ++attempt)
      r = send(t, first, n) ? collect(t) : Result::Error;   // (a unit attention after a reset: once more)
    if (r != Result::Good) return false;
    if (!write) into.subview(first * bs, uint64_t{n} * bs).copy_from(tags_[t].data.first(uint64_t{n} * bs));
    __atomic_fetch_add(write ? &stats_.blocks_written : &stats_.blocks_read, n, __ATOMIC_RELAXED);
    return true;
  });
  for (uint32_t i = 0; i < got; ++i) pool_.release(slots[i]);
  __atomic_fetch_add(ok ? (write ? &stats_.writes : &stats_.reads) : &stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

bool UasStorageDevice::read(uint64_t lba, ByteView buffer) { return run_blocks(false, lba, buffer, ConstByteView()); }
bool UasStorageDevice::write(uint64_t lba, ConstByteView buffer) { return run_blocks(true, lba, ByteView(), buffer); }

bool UasStorageDevice::flush() {
  const uint8_t cdb[10] = {scsi::kSynchronizeCache10};
  return run(ConstByteView(cdb), ByteView(), false);
}

bool UasStorageDevice::discard(uint64_t lba, uint64_t blocks) {
  if (!info_.discard || blocks == 0 || lba >= info_.block_count || blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint8_t parameters[scsi::kUnmapParameterBytes];
  const bool ok = scsi::unmap_range([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, unmap_, ByteView(parameters), lba, blocks);
  if (ok) {
    __atomic_fetch_add(&stats_.discards, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&stats_.blocks_discarded, blocks, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  }
  return ok;
}

bool UasStorageDevice::write_zeroes(uint64_t lba, uint64_t blocks) {
  if (!info_.write_zeroes || __atomic_load_n(&write_same_refused_, __ATOMIC_ACQUIRE) || blocks == 0 || lba >= info_.block_count ||
      blocks > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint8_t zeros[4096];
  const bool ok = scsi::write_same_zeroes([this](ConstByteView cdb, ByteView data, bool in) { return run(cdb, data, in); }, ByteView(zeros).first(info_.block_size),
                                          write_same_max_, lba, blocks);
  if (ok) {
    __atomic_fetch_add(&stats_.blocks_zeroed, blocks, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&write_same_refused_, true, __ATOMIC_RELEASE);
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
  }
  return ok;
}

StorageStatistics UasStorageDevice::statistics() const {
  StorageStatistics s;
  s.reads = __atomic_load_n(&stats_.reads, __ATOMIC_RELAXED);
  s.writes = __atomic_load_n(&stats_.writes, __ATOMIC_RELAXED);
  s.blocks_read = __atomic_load_n(&stats_.blocks_read, __ATOMIC_RELAXED);
  s.blocks_written = __atomic_load_n(&stats_.blocks_written, __ATOMIC_RELAXED);
  s.discards = __atomic_load_n(&stats_.discards, __ATOMIC_RELAXED);
  s.blocks_discarded = __atomic_load_n(&stats_.blocks_discarded, __ATOMIC_RELAXED);
  s.blocks_zeroed = __atomic_load_n(&stats_.blocks_zeroed, __ATOMIC_RELAXED);
  s.errors = __atomic_load_n(&stats_.errors, __ATOMIC_RELAXED);
  return s;
}

void UasStorageDevice::describe(IPropertySink& sink) const {
  sink.add_text("Transport", streams_ ? "USB Attached SCSI, a bulk stream per command" : "USB Attached SCSI, one command at a time (no streams)");
  sink.add_number("Queue depth", tag_count_);
  sink.add_flag("Given up after a transfer error", __atomic_load_n(&broken_, __ATOMIC_ACQUIRE));
}

}  // namespace hardware

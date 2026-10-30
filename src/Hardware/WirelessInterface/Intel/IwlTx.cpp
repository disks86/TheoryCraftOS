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


#include "Hardware/WirelessInterface/Intel/IwlTx.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFwLoad.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

using namespace iwl_tx;

namespace {
void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (uint32_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
}  // namespace

ArrayView<uint8_t> IwlCommandQueue::block(uint64_t bytes, uint64_t alignment, uint64_t* bus) {
  ArrayView<uint8_t> view = allocate_dma_view<uint8_t>(allocator_, bytes, alignment);
  if (view.size() == 0) return view;
  const uint64_t address = reinterpret_cast<uint64_t>(view.data());
  // the chip reaches 36 bits of address and its rings must not cross a 4 GiB line
  if (address >> 36 != 0 || (address + bytes - 1) >> 32 != address >> 32) {
    release_view(allocator_, view);
    return view;
  }
  view.fill(0);
  *bus = address;
  return view;
}

void IwlCommandQueue::set_tb(uint8_t* t, uint32_t tb, uint64_t bus, uint32_t length) {
  uint8_t* entry = t + 4 + tb * 6;
  put32(entry, static_cast<uint32_t>(bus));
  put16(entry + 4, static_cast<uint16_t>((length << 4) | ((bus >> 32) & 0xF)));
  t[3] = static_cast<uint8_t>(tb + 1);
}

// A TFD the chip may read but that means nothing: no buffers, the first points at a block of zeros (iwl_txq_set_tfd_invalid_gen1).
void IwlCommandQueue::invalidate(uint32_t index) {
  uint8_t* t = tfd(index);
  ByteView(t, kTfdBytes).fill(0);
  t[3] = 0;
  set_tb(t, 0, invalid_bus_, kInvalidCommandBytes);
}

bool IwlCommandQueue::init(const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  release();
  tfds_ = block(static_cast<uint64_t>(kRingEntries) * kTfdBytes, 256, &tfds_bus_);
  commands_ = block(static_cast<uint64_t>(kSlots) * kSlotBytes, 64, &commands_bus_);
  first_tbs_ = block(static_cast<uint64_t>(kSlots) * kFirstTbStride, 64, &first_tbs_bus_);
  keep_warm_ = block(4096, 4096, &keep_warm_bus_);
  byte_counts_ = block(static_cast<uint64_t>(kQueues) * kByteCountEntries * 2, 1024, &byte_counts_bus_);
  invalid_ = block(kInvalidCommandBytes, 64, &invalid_bus_);
  if (!tfds_.size() || !commands_.size() || !first_tbs_.size() || !keep_warm_.size() || !byte_counts_.size() || !invalid_.size()) {
    *error = "no memory for the command queue";
    release();
    return false;
  }
  for (uint32_t i = 0; i < kRingEntries; ++i) invalidate(i);
  write_ = read_ = 0;
  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake to set up the command queue";
    release();
    return false;
  }
  device_.write_prph(kScdTxFact, 0);                                         // all transmit FIFOs off
  device_.write32(kFhKeepWarm, static_cast<uint32_t>(keep_warm_bus_ >> 4));  // the "keep warm" page
  device_.write32(cbbc_queue(kCommandQueue), static_cast<uint32_t>(tfds_bus_ >> 8));
  device_.set_prph_bits(kScdGpCtrl, kScdGpCtrlAutoActive);
  device_.set_prph_bits(kScdGpCtrl, kScdGpCtrlEnable31Queues);               // (more than 20 queues)
  device_.release_nic_access();
  return true;
}

bool IwlCommandQueue::start(const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (!ready()) {
    *error = "the command queue is not initialised";
    return false;
  }
  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake to start the transmit queues";
    return false;
  }
  scd_base_ = device_.read_prph(kScdSramBase);
  // reset the scheduler's context, status and translation memory of all queues
  const uint32_t clear_dwords = (scd_trans_tbl_queue(kQueues) - kScdContextMemLower) / 4;
  uint32_t zero[64] = {};
  for (uint32_t done = 0; done < clear_dwords;) {
    const uint32_t n = clear_dwords - done < 64 ? clear_dwords - done : 64;
    device_.write_mem(scd_base_ + kScdContextMemLower + done * 4, zero, n);
    done += n;
  }
  device_.write_prph(kScdDramBase, static_cast<uint32_t>(byte_counts_bus_ >> 10));
  // the command queue on its FIFO (iwl_trans_pcie_txq_enable with the scheduler config)
  device_.write_prph(kScdEnCtrl, 0);                                         // scheduler off while the command queue is configured
  device_.write_prph(scd_queue_status(kCommandQueue), kQueueStatusScdActEn);   // inactive
  device_.clear_prph_bits(kScdAggrSel, 1u << kCommandQueue);
  read_ = write_ = 0;
  device_.write32(kHbusTargWrptr, (0u) | (kCommandQueue << 8));
  device_.write_prph(scd_queue_read_pointer(kCommandQueue), 0);
  const uint32_t context = scd_base_ + scd_context_queue(kCommandQueue);
  const uint32_t window = (kFrameLimit & kContextWindowMask) | (kFrameLimit << kContextFrameLimitShift);
  device_.write_mem(context, zero, 1);
  device_.write_mem(context + 4, &window, 1);
  device_.write_prph(scd_queue_status(kCommandQueue), kQueueStatusActive | (kCommandFifo << 0) | kQueueStatusWsl | kQueueStatusMask);
  device_.write_prph(kScdEnCtrl, 1u << kCommandQueue);
  device_.write_prph(kScdTxFact, 0xFF);                                      // activate the FIFOs
  for (uint32_t channel = 0; channel < kTcsrChannels; ++channel)
    device_.write32(iwl_fh::tx_config(channel), iwl_fh::kTxChannelEnable | kTcsrCreditEnable);
  device_.write32(kFhTxChickenBits, device_.read32(kFhTxChickenBits) | kFhTxChickenScdAutoRetry);
  device_.release_nic_access();
  return true;
}

void IwlCommandQueue::release() {
  release_view(allocator_, tfds_);
  release_view(allocator_, commands_);
  release_view(allocator_, first_tbs_);
  release_view(allocator_, keep_warm_);
  release_view(allocator_, byte_counts_);
  release_view(allocator_, invalid_);
  write_ = read_ = scd_base_ = 0;
}

int32_t IwlCommandQueue::enqueue(const IwlHostCommand& command, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (!ready()) {
    *error = "the command queue is not initialised";
    return -1;
  }
  const bool wide = command.group != 0;
  const uint32_t header = wide ? 8 : 4;
  if (command.data.size() > kSlotBytes - header) {
    *error = "the command is too large";
    return -1;
  }
  if (in_flight() >= kSlots - 1) {
    *error = "no space in the command queue";
    return -1;
  }
  const uint32_t index = write_;
  const uint32_t slot = index & (kSlots - 1);
  const uint32_t total = header + static_cast<uint32_t>(command.data.size());
  const uint16_t sequence = static_cast<uint16_t>((kCommandQueue << 8) | index);
  ByteView out = commands_.subview(static_cast<uint64_t>(slot) * kSlotBytes, kSlotBytes);
  out.fill(0);
  uint8_t head[8] = {command.opcode, command.group};
  put16(head + 2, sequence);
  if (wide) {
    put16(head + 4, static_cast<uint16_t>(total - 8));
    head[6] = 0;
    head[7] = command.version;
  }
  out.subview(0, header).copy_from(ConstByteView(head, header));
  if (command.data.size()) out.subview(header, command.data.size()).copy_from(command.data);
  // the first TB: the first 20 bytes, in their own block; the second: the rest, in the slot
  const uint32_t first = total < kFirstTbBytes ? total : kFirstTbBytes;
  first_tbs_.subview(static_cast<uint64_t>(slot) * kFirstTbStride, first).copy_from(out.subview(0, first));
  uint8_t* t = tfd(index);
  ByteView(t, kTfdBytes).fill(0);
  set_tb(t, 0, first_tbs_bus_ + static_cast<uint64_t>(slot) * kFirstTbStride, first);
  if (total > first) set_tb(t, 1, commands_bus_ + static_cast<uint64_t>(slot) * kSlotBytes + first, total - first);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write_ = (write_ + 1) & (kRingEntries - 1);
  if (!device_.grab_nic_access()) {
    write_ = index;
    invalidate(index);
    *error = "the MAC would not wake to send a command";
    return -1;
  }
  device_.write32(kHbusTargWrptr, write_ | (kCommandQueue << 8));
  device_.release_nic_access();
  return sequence;
}

void IwlCommandQueue::reclaim(uint32_t index) {
  if (!ready() || index >= kRingEntries) return;
  const uint32_t stop = (index + 1) & (kRingEntries - 1);
  // only what was sent can be answered: the index must lie in [read, write)
  if (((index - read_) & (kRingEntries - 1)) >= in_flight()) return;
  while (read_ != stop) {
    invalidate(read_);
    read_ = (read_ + 1) & (kRingEntries - 1);
  }
}

}  // namespace hardware

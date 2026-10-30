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


#include "Hardware/WirelessInterface/Intel/IwlRx.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace iwl_rfh;

namespace {
// The chip and the host share these words: the accesses are volatile reads and writes of aligned 16 / 32 / 64 bit values inside views that were bounds-checked by the caller.
uint32_t read_shared32(const uint32_t* p) {
  const uint32_t v = *static_cast<const volatile uint32_t*>(p);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return v;
}
uint16_t read_shared16(const uint8_t* p) {
  // (alignment: the status block starts at a 16 byte boundary and the field is the first)
  const uint16_t v = *static_cast<const volatile uint16_t*>(static_cast<const volatile void*>(p));
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return v;
}
void write_shared64(uint64_t* p, uint64_t v) {
  *static_cast<volatile uint64_t*>(p) = v;
  __atomic_thread_fence(__ATOMIC_RELEASE);
}
void write_prph64(IwlPcieDevice& d, uint32_t address, uint64_t value) {
  d.write_prph(address, static_cast<uint32_t>(value));
  d.write_prph(address + 4, static_cast<uint32_t>(value >> 32));
}
}  // namespace

bool IwlRxQueue::init(bool integrated, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  release();
  free_ring_ = allocate_dma_view<uint64_t>(allocator_, kQueueSize, 4096);
  used_ring_ = allocate_dma_view<uint32_t>(allocator_, kQueueSize, 4096);
  status_ = allocate_dma_view<uint8_t>(allocator_, 16, 16);
  buffers_ = allocate_dma_view<uint8_t>(allocator_, static_cast<uint64_t>(kQueueSize) * kBufferBytes, 4096);
  if (free_ring_.size() == 0 || used_ring_.size() == 0 || status_.size() == 0 || buffers_.size() == 0) {
    *error = "no memory for the receive queue";
    release();
    return false;
  }
  // the chip reaches 36 bits of address; none of the blocks may be above that, and a buffer must not cross a 4 GiB line (the low bits of the id sit under the address)
  const uint64_t blocks[4] = {reinterpret_cast<uint64_t>(free_ring_.data()), reinterpret_cast<uint64_t>(used_ring_.data()), reinterpret_cast<uint64_t>(status_.data()),
                              reinterpret_cast<uint64_t>(buffers_.data())};
  const uint64_t ends[4] = {blocks[0] + free_ring_.size() * 8, blocks[1] + used_ring_.size() * 4, blocks[2] + status_.size(), blocks[3] + buffers_.size()};
  for (uint32_t i = 0; i < 4; ++i)
    if (blocks[i] >> 36 != 0 || (ends[i] - 1) >> 36 != 0) {
      *error = "receive queue memory is out of the chip's reach";
      release();
      return false;
    }
  free_ring_.fill(0);
  used_ring_.fill(0);
  status_.fill(0);
  buffers_.fill(0);
  write_ = read_ = announced_ = posted_ = 0;

  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake to set up the receive queue";
    release();
    return false;
  }
  device_.write_prph(kDmaCfg, 0);                                 // stop receive DMA
  device_.write_prph(kRxqActive, 0);
  write_prph64(device_, free_ring_base(0), blocks[0]);
  write_prph64(device_, used_ring_base(0), blocks[1]);
  write_prph64(device_, status_base(0), blocks[2]);
  device_.write_prph(free_write_index(0), 0);
  device_.write_prph(free_read_index(0), 0);
  device_.write_prph(used_write_index(0), 0);
  device_.write_prph(kDmaCfg, kDmaEnable | kDmaBufferSize4K | kDmaMinBuffer4And8 | kDmaDropTooLarge | kDmaRingSize256);
  device_.write_prph(kGenCfg, kGenCfgDmaSnoop | kGenCfgServiceDmaSnoop | (integrated ? 0 : kGenCfgChunk128));       // default queue 0
  device_.write_prph(kRxqActive, 1u | (1u << 16));
  device_.release_nic_access();
  // coalescing timer (the byte at 0x004; the neighbours are other registers)
  device_.write32(kIntCoalescing, (device_.read32(kIntCoalescing) & ~0xFFu) | kIntCoalescingDefault);

  // post the buffers: all but a margin of eight, in groups of eight as the chip's write pointer wants
  for (uint32_t vid = 1; vid <= kQueueSize - 8; ++vid) post(vid);
  announce_write();
  return true;
}

void IwlRxQueue::release() {
  if (buffers_.size() != 0 && device_.grab_nic_access()) {
    device_.write_prph(kDmaCfg, 0);
    device_.write_prph(kRxqActive, 0);
    device_.release_nic_access();
  }
  release_view(allocator_, free_ring_);
  release_view(allocator_, used_ring_);
  release_view(allocator_, status_);
  release_view(allocator_, buffers_);
}

void IwlRxQueue::post(uint32_t vid) {
  const uint64_t bus = reinterpret_cast<uint64_t>(buffers_.data()) + static_cast<uint64_t>(vid - 1) * kBufferBytes;
  // the buffer is empty again: nothing stale for the next walk
  buffers_.subview(static_cast<uint64_t>(vid - 1) * kBufferBytes, 4).fill(0);
  write_shared64(&free_ring_[write_ & (kQueueSize - 1)], bus | vid);
  write_ = (write_ + 1) & (kQueueSize - 1);
  ++posted_;
}

void IwlRxQueue::announce_write() {
  const uint32_t actual = write_ & ~7u;
  if (actual == announced_) return;
  announced_ = actual;
  if (device_.grab_nic_access()) {
    device_.write32(free_write_trigger(0), actual);
    device_.release_nic_access();
  }
}

uint32_t IwlRxQueue::closed_index() const { return read_shared16(status_.data()) & 0xFFF; }
uint32_t IwlRxQueue::used_id(uint32_t index) const { return read_shared32(&used_ring_[index & (kQueueSize - 1)]) & 0x0FFF; }

uint32_t IwlRxQueue::handle_buffer(uint32_t vid, IIwlPacketSink& sink) {
  ConstByteView buffer = buffers_.subview(static_cast<uint64_t>(vid - 1) * kBufferBytes, kBufferBytes);
  uint32_t frames = 0;
  uint32_t offset = 0;
  while (kBufferBytes - offset > 8) {                               // a status word and a command header at least
    ByteReader r(buffer.subview(offset, kBufferBytes - offset));
    const uint32_t len_n_flags = r.le32();
    if (len_n_flags == iwl_frame::kInvalid) break;                  // the chip's end marker
    const uint32_t size = len_n_flags & iwl_frame::kSizeMask;         // the packet: the command header and the payload
    const uint32_t total = size + 4;                                // and the status word
    if (size < 4 || total > kBufferBytes - offset) break;
    IwlRxPacket packet;
    packet.command = r.u8();
    packet.group = r.u8();
    packet.sequence = r.le16();
    packet.payload = buffer.subview(offset + 8, size - 4);
    sink.on_packet(packet);
    ++frames;
    offset += (total + iwl_frame::kAlign - 1) & ~(iwl_frame::kAlign - 1);
    if (offset >= kBufferBytes) break;
  }
  return frames;
}

uint32_t IwlRxQueue::poll(IIwlPacketSink& sink) {
  if (!ready()) return 0;
  const uint32_t closed = closed_index();
  uint32_t frames = 0;
  uint32_t guard = 0;
  while (read_ != (closed & (kQueueSize - 1)) && guard++ < kQueueSize) {
    const uint32_t vid = used_id(read_);
    read_ = (read_ + 1) & (kQueueSize - 1);
    if (vid == 0 || vid > kQueueSize) continue;                      // the chip named a buffer that does not exist: skip it, do not trust it
    frames += handle_buffer(vid, sink);
    post(vid);
  }
  announce_write();
  return frames;
}

}  // namespace hardware

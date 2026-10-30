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


// The receive queue of an Intel Wireless-AC 9000 series card (multi-queue receive, one queue: iwl_pcie_rx_alloc / iwl_pcie_rx_mq_hw_init / iwl_pcie_rx_handle of iwlwifi). The host
// gives the chip receive buffers by posting 64 bit descriptors (the buffer's bus address with its 12 bit "virtual id", the buffer's number plus one, in the low bits) to a ring
// of free descriptors; the chip fills a buffer with one or more frames (firmware responses and notifications, each a 32 bit length and flags word, a command header and the
// payload, padded to 64 bytes), writes the buffer's id to a ring of used descriptors and the index of the last one to a status block in host memory. poll() walks the used
// ring up to that index, hands every frame to a sink and posts the buffers again. The firmware's ALIVE notification and every command response arrive here.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace iwl_rfh {
// peripheral registers
constexpr uint32_t free_ring_base(uint32_t q) { return 0xA08000 + q * 8; }
constexpr uint32_t free_write_index(uint32_t q) { return 0xA08080 + q * 4; }
constexpr uint32_t free_read_index(uint32_t q) { return 0xA080C0 + q * 4; }
constexpr uint32_t used_ring_base(uint32_t q) { return 0xA08100 + q * 8; }
constexpr uint32_t used_write_index(uint32_t q) { return 0xA08180 + q * 4; }
constexpr uint32_t status_base(uint32_t q) { return 0xA08200 + q * 8; }
constexpr uint32_t kGenCfg = 0xA09800, kRxqActive = 0xA0980C, kDmaCfg = 0xA09820;
constexpr uint32_t kGenCfgServiceDmaSnoop = 1, kGenCfgDmaSnoop = 2, kGenCfgChunk128 = 1u << 4;
constexpr uint32_t kDmaBufferSize4K = 0x4u << 16, kDmaRingSize256 = 0x8u << 20, kDmaMinBuffer4And8 = 3u << 24, kDmaDropTooLarge = 1u << 26, kDmaEnable = 1u << 31;
// CSR (direct) registers
constexpr uint32_t free_write_trigger(uint32_t q) { return 0x1C80 + q * 4; }
constexpr uint32_t kIntCoalescing = 0x004, kIntCoalescingDefault = 0x40;
}  // namespace iwl_rfh

namespace iwl_frame {
constexpr uint32_t kSizeMask = 0x3FFF, kInvalid = 0x55550000, kAlign = 0x40, kQueueMask = 0x0000F000, kQueueShift = 12;
constexpr uint16_t kSequenceRxFrame = 0x8000;
}  // namespace iwl_frame

// One frame of a receive buffer: the command header of the firmware's message and its payload (a view into the buffer, valid during the call).
struct IwlRxPacket {
  uint8_t command = 0, group = 0;
  uint16_t sequence = 0;                 // the command queue (bits 8-12) and index (bits 0-7) of a response; bit 15 set for notifications
  ConstByteView payload;
  bool is_response() const { return !(sequence & iwl_frame::kSequenceRxFrame); }
  uint8_t queue() const { return (sequence >> 8) & 0x1F; }
  uint8_t index() const { return sequence & 0xFF; }
};

class IIwlPacketSink {
 public:
  virtual void on_packet(const IwlRxPacket& packet) = 0;

 protected:
  constexpr IIwlPacketSink() = default;
  ~IIwlPacketSink() = default;
};

class IwlRxQueue {
 public:
  static constexpr uint32_t kQueueSize = 256, kBufferBytes = 4096;

  IwlRxQueue(IwlPcieDevice& device, allocators::IAllocator& allocator) : device_(device), allocator_(allocator) {}
  ~IwlRxQueue() { release(); }
  IwlRxQueue(const IwlRxQueue&) = delete;
  IwlRxQueue& operator=(const IwlRxQueue&) = delete;

  // Allocates the rings, the status block and the buffers, tells the chip where they are, enables receive DMA and posts the buffers. Needs the MAC awake (APM initialised).
  bool init(bool integrated, const char** error);
  // Stops receive DMA and frees the memory.
  void release();
  // Handles every buffer the chip has closed; returns the number of frames handed to the sink. Frames that do not make sense end the buffer.
  uint32_t poll(IIwlPacketSink& sink);
  bool ready() const { return buffers_.size() != 0; }
  uint32_t buffers_posted() const { return posted_; }

 private:
  uint32_t closed_index() const;
  uint32_t used_id(uint32_t index) const;
  void post(uint32_t vid);
  void announce_write();
  uint32_t handle_buffer(uint32_t vid, IIwlPacketSink& sink);

  IwlPcieDevice& device_;
  allocators::IAllocator& allocator_;
  ArrayView<uint64_t> free_ring_;
  ArrayView<uint32_t> used_ring_;
  ArrayView<uint8_t> status_;
  ArrayView<uint8_t> buffers_;
  uint32_t write_ = 0, read_ = 0, announced_ = 0, posted_ = 0;
};

}  // namespace hardware

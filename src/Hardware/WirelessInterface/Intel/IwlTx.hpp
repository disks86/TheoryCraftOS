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


// The host command queue of an Intel Wireless-AC 9000 series card (iwlwifi pcie/tx.c for the families before the 22000 series: iwl_pcie_txq_alloc, iwl_pcie_tx_init,
// iwl_pcie_tx_start, iwl_trans_pcie_txq_enable, iwl_pcie_enqueue_hcmd, iwl_pcie_cmdq_reclaim). The queue is a ring of 256 transmit frame descriptors (TFDs) in host memory; each
// describes a command with up to 20 transmit buffers (TBs). A command is built in a slot of a command buffer; its first 20 bytes are copied to a "first TB" block (the chip
// reads them separately), the remainder is the second TB. The chip's write pointer register (HBUS_TARG_WRPTR) tells it how far to read. The scheduler (SCD) is told about the queue
// once the firmware is running (its SRAM base is only valid then). Responses come back through the receive queue: reclaim() frees the commands up to the answered one.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace iwl_tx {
constexpr uint32_t kQueues = 31;                          // IWL_9000 num_of_queues
constexpr uint32_t kCommandQueue = 0, kCommandFifo = 7, kAuxQueue = 1, kMcastFifo = 5;   // the firmware's DQA layout (IWL_MVM_DQA_CMD_QUEUE, IWL_MVM_TX_FIFO_CMD)
constexpr uint32_t kRingEntries = 256;                    // TFDs per queue (max_tfd_queue_size)
constexpr uint32_t kSlots = 32;                           // commands in flight (IWL_CMD_QUEUE_SIZE)
constexpr uint32_t kTfdBytes = 128, kMaxTbs = 20, kFirstTbBytes = 20, kFirstTbStride = 64;
constexpr uint32_t kSlotBytes = 2048;                     // command header and data of one command
constexpr uint32_t kByteCountEntries = kRingEntries + 64; // TFD_QUEUE_BC_SIZE
constexpr uint32_t kInvalidCommandBytes = 324;
// CSR (direct) registers
constexpr uint32_t kHbusTargWrptr = 0x460;
constexpr uint32_t kFhKeepWarm = 0x1000 + 0x97C;
constexpr uint32_t cbbc_queue(uint32_t q) { return q < 16 ? 0x1000 + 0x9D0 + 4 * q : q < 20 ? 0x1000 + 0xBF0 + 4 * (q - 16) : 0x1000 + 0xB20 + 4 * (q - 20); }
constexpr uint32_t kFhTxChickenBits = 0x1000 + 0xE98, kFhTxChickenScdAutoRetry = 2;
constexpr uint32_t kTcsrChannels = 8, kTcsrCreditEnable = 8;
// peripheral registers (the scheduler)
constexpr uint32_t kScdBase = 0xA02C00;
constexpr uint32_t kScdSramBase = kScdBase + 0x0, kScdDramBase = kScdBase + 0x8, kScdTxFact = kScdBase + 0x10, kScdQueueChainSel = kScdBase + 0xE8,
                   kScdAggrSel = kScdBase + 0x248, kScdGpCtrl = kScdBase + 0x1A8, kScdEnCtrl = kScdBase + 0x254, kScdChainExtEn = kScdBase + 0x244;
constexpr uint32_t scd_queue_read_pointer(uint32_t q) { return q < 20 ? kScdBase + 0x68 + q * 4 : kScdBase + 0x2B4 + q * 4; }
constexpr uint32_t scd_queue_status(uint32_t q) { return q < 20 ? kScdBase + 0x10C + q * 4 : kScdBase + 0x334 + q * 4; }
constexpr uint32_t kScdGpCtrlEnable31Queues = 1, kScdGpCtrlAutoActive = 1u << 18;
constexpr uint32_t kScdContextMemLower = 0x600, kScdTransTblLower = 0x7E0;
constexpr uint32_t scd_context_queue(uint32_t q) { return kScdContextMemLower + q * 8; }
constexpr uint32_t scd_trans_tbl_queue(uint32_t q) { return (kScdTransTblLower + q * 2) & 0xFFFC; }
constexpr uint32_t kQueueStatusActive = 1u << 3, kQueueStatusWsl = 1u << 4, kQueueStatusScdActEn = 1u << 19, kQueueStatusMask = 0x017F0000;
constexpr uint32_t kFrameLimit = 64, kContextWindowMask = 0x7F, kContextFrameLimitShift = 16;
}  // namespace iwl_tx

// A command to the firmware: group 0 commands use the short header; others the wide one with a version.
struct IwlHostCommand {
  uint8_t group = 0;
  uint8_t opcode = 0;
  uint8_t version = 0;                    // wide commands only
  ConstByteView data;
};

class IwlCommandQueue {
 public:
  // The command queue by default; another queue (the auxiliary one the firmware sends scan probes from) is made the same way and started with start_secondary().
  IwlCommandQueue(IwlPcieDevice& device, allocators::IAllocator& allocator, uint32_t queue = iwl_tx::kCommandQueue, uint32_t fifo = iwl_tx::kCommandFifo)
      : device_(device), allocator_(allocator), queue_(queue), fifo_(fifo) {}
  ~IwlCommandQueue() { release(); }
  IwlCommandQueue(const IwlCommandQueue&) = delete;
  IwlCommandQueue& operator=(const IwlCommandQueue&) = delete;

  // Allocates the rings and buffers and tells the chip where the ring is (before the firmware is loaded; needs the MAC awake).
  bool init(const char** error);
  // After ALIVE: the scheduler's setup and the transmit DMA channels (iwl_pcie_tx_start), the command queue enabled on its FIFO.
  bool start(const char** error);
  // A queue that is not the command queue, after the scheduler is running (the command queue's start() set it up): chained, active on its FIFO.
  bool start_secondary(const char** error);
  void release();
  bool ready() const { return tfds_.size() != 0; }

  // Builds the command in the next slot and rings the doorbell. Returns the sequence number the response carries (queue in bits 8-12, index in bits 0-7), or -1 when the queue is
  // full, the command is too large or the chip is not reachable.
  int32_t enqueue(const IwlHostCommand& command, const char** error);
  // The response with this index arrived: the commands up to it are done.
  void reclaim(uint32_t index);
  uint32_t in_flight() const { return (write_ - read_) & (iwl_tx::kRingEntries - 1); }
  uint32_t write_pointer() const { return write_; }
  uint32_t scd_base() const { return scd_base_; }

 private:
  uint8_t* tfd(uint32_t index) { return &tfds_[static_cast<uint64_t>(index) * iwl_tx::kTfdBytes]; }
  void set_tb(uint8_t* tfd, uint32_t tb, uint64_t bus, uint32_t length);
  void invalidate(uint32_t index);
  ArrayView<uint8_t> block(uint64_t bytes, uint64_t alignment, uint64_t* bus);
  void configure_queue();

  IwlPcieDevice& device_;
  allocators::IAllocator& allocator_;
  ArrayView<uint8_t> tfds_, commands_, first_tbs_, keep_warm_, byte_counts_, invalid_;
  uint64_t tfds_bus_ = 0, commands_bus_ = 0, first_tbs_bus_ = 0, keep_warm_bus_ = 0, byte_counts_bus_ = 0, invalid_bus_ = 0;
  uint32_t queue_, fifo_;
  uint32_t write_ = 0, read_ = 0, scd_base_ = 0;
};

}  // namespace hardware

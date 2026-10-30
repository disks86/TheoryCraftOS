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

// The host interface's DMA rings (tb_ring_* of the Linux thunderbolt driver's nhi.c) and the parts of the NHI around them: the capability word, the interrupts (all kept off: the
// driver polls the descriptors), the firmware status and the mailbox to the connection manager firmware. A ring is an array of 16 byte descriptors in DMA memory: a 64 bit buffer
// address, a word with the length (12 bits), EOF and SOF (4 bits each) and flags (12 bits), and a time stamp word. The host posts descriptors and writes the ring's head; the
// chip sets COMPLETED on a descriptor when it has sent or filled the buffer and moves its tail. This driver uses the raw mode: every frame carries its own CRC, EOF / SOF are the
// packet type.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltRegisters.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The bus address of DMA memory (the kernel's DMA pool is identity mapped below 4 GiB).
uint64_t tb_bus_address(const void* dma_memory);

enum class TbMailboxResult : uint8_t { Ok, Timeout, Error };
// REG_OUTMAIL_CMD's operation mode of the firmware (nhi.h: enum nhi_fw_mode)
enum class TbFirmwareMode : uint8_t { Safe = 0, Authentication = 1, EndpointFailed = 2, ConnectionManager = 3, Other = 255 };

class TbNhi {
 public:
  explicit TbNhi(ITbRegisters& registers) : registers_(registers) {}

  ITbRegisters& registers() { return registers_; }
  // The registers answer (a function that fell off the bus reads all ones).
  bool present() { return registers_.read32(tb_reg::kCaps) != 0xFFFFFFFFu; }
  // The number of hops (and so TX / RX rings) the chip has; hop 0 is the control channel.
  uint32_t hop_count() { return registers_.read32(tb_reg::kCaps) & tb_reg::kCapsHopMask; }
  uint32_t capability_version() { return (registers_.read32(tb_reg::kCaps) >> tb_reg::kCapsVersionShift) & tb_reg::kCapsVersionMask; }

  // Masks every ring interrupt and clears the pending bits (nhi_disable_interrupts, for a chip without the auto-clear quirk).
  void disable_interrupts();
  // Hop numbers for the rings of data paths (hop 0 is the control channel). Returns the lowest free hop from 1 up, or -1 if there is none; the two directions are counted apart.
  int allocate_hop(bool transmit);
  void release_hop(bool transmit, uint32_t hop);

  // Unmasks (or masks again) the interrupts of one ring of the control channel, hop 0: the transmit ring's bit is the hop, the receive ring's the hop plus the number of hops.
  // With MSI-X (`vector` >= 0, one of the table's first 16 entries) the two rings' interrupts are also routed to that entry (the vector allocation registers) and the chip is told to clear
  // the status bits itself; with MSI (-1) all the rings share the one message.
  void set_control_interrupts(bool on, int vector = -1);
  // The same for any ring: the ring on `hop`, transmit or receive (a data ring once it runs; a ring's interrupt is off until this turns it on, and must be turned off before the hop is released).
  void set_ring_interrupts(bool transmit, uint32_t hop, bool on, int vector = -1);
  // What the interrupt handler does: reads all the notify registers, which is what lets the chip raise the next interrupt (they are read-to-clear, even those of rings that are masked).
  // True if any ring had something to say. Touches only registers, so it is fine in an interrupt handler.
  bool acknowledge_interrupt();

  // Is the connection manager firmware (ICM) running? It is started by the BIOS; this driver does not start it.
  bool icm_running() { return (registers_.read32(tb_reg::kFirmwareStatus) & tb_reg::kFwStsIcmEnable) != 0; }
  // Waits for the firmware's NVM image to be authenticated.
  bool wait_firmware_authenticated(uint32_t timeout_us);
  TbFirmwareMode firmware_mode();
  TbMailboxResult mailbox(uint32_t command, uint32_t data, uint32_t timeout_us);

 private:
  ITbRegisters& registers_;
  uint32_t interrupt_mask_[64] = {};
  uint32_t hops_used_[2][32] = {};            // [transmit][hop / 32]          // what is unmasked, as the chip's registers cannot be read back
};

// A finished descriptor.
struct TbCompletion {
  uint32_t length = 0;      // bytes (0 means 4096 for a descriptor posted for a 4096 byte buffer)
  uint8_t sof = 0, eof = 0;
  uint16_t flags = 0;
  uint32_t slot = 0;        // the descriptor's index in the ring
};

class TbRing {
 public:
  static constexpr uint32_t kDescriptorBytes = 16;
  static constexpr uint16_t kDescPosted = 0x4, kDescCompleted = 0x2, kDescInterrupt = 0x8, kDescCrcError = 0x1, kDescOverrun = 0x4;

  // How a ring carries its frames. The control channel uses the raw mode (EOF / SOF are the packet type, every frame carries its own CRC). A data ring uses the frame mode: the buffer is
  // up to 4096 bytes, SOF and EOF mark the first and last frame of what the sender calls a packet, and the chip checks the CRC itself. End to end flow control makes the receiver give the
  // sender credits (on the receive ring's `e2e_tx_hop`, the hop of the transmit ring of the same connection).
  struct Mode {
    bool frame = false, e2e = false;
    uint32_t e2e_tx_hop = 0;
  };
  TbRing(ITbRegisters& registers, allocators::IAllocator& allocator, uint32_t hop, bool transmit, uint32_t size)
      : registers_(registers), allocator_(allocator), hop_(hop), transmit_(transmit), size_(size) {}
  ~TbRing() { stop(); }
  TbRing(const TbRing&) = delete;
  TbRing& operator=(const TbRing&) = delete;

  // Allocates the descriptors and enables the ring. A receive ring takes frames of up to `frame_bytes` (256 is the smallest; 0 means 4096) whose SOF / EOF are in the masks.
  bool start(uint32_t frame_bytes, uint16_t sof_mask, uint16_t eof_mask);
  bool start(const Mode& mode, uint16_t sof_mask, uint16_t eof_mask);
  // Disables the ring and frees the descriptors; frames in flight are dropped.
  void stop();
  bool running() const { return descriptors_.size() != 0; }

  // Descriptors that can still be posted.
  uint32_t free_slots() const { return (tail_ + size_ - head_ - 1) % size_; }
  // Posts a frame (transmit) or an empty buffer (receive) at the bus address `bus` and tells the chip.
  bool post_transmit(uint64_t bus, uint32_t bytes, uint8_t sof, uint8_t eof);
  bool post_receive(uint64_t bus);
  // The next finished descriptor, if any.
  bool poll(TbCompletion* out);

  uint32_t hop() const { return hop_; }
  uint32_t size() const { return size_; }

 private:
  bool start_ring(const Mode& mode, uint32_t frame_bytes, uint16_t sof_mask, uint16_t eof_mask);
  bool post(uint64_t bus, uint32_t word);
  uint32_t ring_base() const { return (transmit_ ? tb_reg::kTxRingBase : tb_reg::kRxRingBase) + hop_ * 16; }
  uint32_t options_base() const { return (transmit_ ? tb_reg::kTxOptionsBase : tb_reg::kRxOptionsBase) + hop_ * 32; }

  ITbRegisters& registers_;
  allocators::IAllocator& allocator_;
  uint32_t hop_;
  bool transmit_;
  uint32_t size_;
  ArrayView<uint32_t> descriptors_;     // kDescriptorBytes / 4 words per descriptor
  uint32_t head_ = 0, tail_ = 0;
  bool frame_mode_ = false;
};

}  // namespace hardware

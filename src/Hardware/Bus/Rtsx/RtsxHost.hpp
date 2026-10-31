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

// The SD slot of the Realtek PCI-E card reader (RTS525A) as an ISdHost, after Linux's rtsx_pcr.c, rts5249.c and rtsx_pci_sdmmc.c. The reader executes queues of register commands that the host
// builds in memory (a command word is type << 30 | register << 16 | mask << 8 | data; the bytes that READ and CHECK commands read come back one after the other at the start of the same
// buffer); a card command is a handful of register writes (the command and its argument, the response type, SD_TRANSFER) and reads of the response registers. Data goes through the 512 byte
// ping-pong buffer (single blocks, a register at a time) or by ADMA from memory (several blocks: a table of 64 bit descriptors). Everything is polled - the interrupt status register is read,
// no interrupt is taken - and the PCI function's INTx is masked by the driver.
//
// Only the SD slot of this chip is driven (no Memory Stick or xD), at the default speed with 3.3 V signalling. The link power management, the PHY tuning, the over-current protection, the
// LED and the 1.8 V / UHS modes of the Linux driver are not done.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Rtsx/RtsxRegisters.hpp"
#include "Hardware/StorageDevice/Sd/ISdHost.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

class RtsxHost final : public ISdHost {
 public:
  static constexpr uint32_t kMaxBlocks = 128;                        // the data buffer holds this many blocks
  RtsxHost(IRtsxRegisters& registers, allocators::IAllocator& allocator) : registers_(registers), allocator_(allocator) {}
  ~RtsxHost() { release(); }
  RtsxHost(const RtsxHost&) = delete;
  RtsxHost& operator=(const RtsxHost&) = delete;

  // Allocates the command and data memory (below 4 GiB) and sets the chip up for the SD slot. False if the memory is not there or the chip does not answer.
  bool initialize();
  void release();

  bool card_present() override;
  bool write_protected() override;
  uint32_t generation() override;
  bool power_on() override;
  void power_off() override;
  bool set_clock(uint32_t hz) override;
  bool set_bus_width(uint32_t bits) override;
  bool command(SdCommand& command) override;
  bool read_blocks(SdCommand& command, uint32_t block_size, uint32_t blocks, ByteView data) override;
  bool write_blocks(SdCommand& command, uint32_t block_size, uint32_t blocks, ConstByteView data) override;
  uint32_t max_blocks() const override { return kMaxBlocks; }

  uint32_t errors() const { return errors_; }

 private:
  // the command queue
  void begin() { count_ = 0; }
  void add(uint32_t type, uint16_t reg, uint8_t mask, uint8_t data);
  void write(uint16_t reg, uint8_t mask, uint8_t data) { add(rtsx::kWriteRegCmd, reg, mask, data); }
  void read(uint16_t reg) { add(rtsx::kReadRegCmd, reg, 0, 0); }
  void check(uint16_t reg, uint8_t mask, uint8_t value) { add(rtsx::kCheckRegCmd, reg, mask, value); }
  bool send(uint32_t timeout_ms);                                    // runs the queue and waits for it
  void start_no_wait();
  bool wait_transfer(uint32_t timeout_ms);
  void stop();
  const uint8_t* results() const { return static_cast<const uint8_t*>(static_cast<const void*>(commands_.data())); }
  bool write_register(uint16_t reg, uint8_t mask, uint8_t data);
  bool read_register(uint16_t reg, uint8_t* value);
  bool switch_clock(uint32_t hz);
  void set_timing();
  bool send_card_command(SdCommand& command);
  bool read_ping_pong(uint32_t bytes, uint8_t* out);
  bool write_ping_pong(const uint8_t* in, uint32_t bytes);
  void command_registers(const SdCommand& command);
  void clear_error();
  bool dma(bool read, uint32_t bytes);

  IRtsxRegisters& registers_;
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  ArrayView<uint8_t> reserved_;                                      // the command buffer (1 KiB) and the descriptor table (3 KiB)
  ArrayView<uint8_t> commands_, table_, data_;
  uint32_t count_ = 0;
  uint32_t generation_ = 1, errors_ = 0, clock_hz_ = 0;
  bool was_present_ = false, powered_ = false, ready_ = false;
};

}  // namespace hardware

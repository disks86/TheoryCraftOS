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

// A 16550 compatible UART (the COM ports of a PC: eight registers at an I/O base, 0x3F8 for COM1). initialize() looks for the chip first -
// a scratch register that does not hold what is written, or a loopback test that does not echo, means nothing is there - and finds out
// whether it has the 16-byte FIFOs (16550A and later) or only one-byte registers (8250 / 16450).
//
// The registers are reached through an ISerialPortIo (the x86 I/O instructions on the machine, a fake in the tests). The UART is polled:
// poll() empties the receive FIFO into a queue and refills the transmit FIFO from another, so a program never waits for the line.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/SerialPort/ISerialPort.hpp"
#include "Hardware/SerialPort/SerialRing.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class ISerialPortIo {
 public:
  virtual uint8_t read8(uint16_t port) = 0;
  virtual void write8(uint16_t port, uint8_t value) = 0;

 protected:
  constexpr ISerialPortIo() = default;
  ~ISerialPortIo() = default;
};

class Uart16550 final : public ISerialPort {
 public:
  static constexpr uint16_t kCom1 = 0x3F8, kCom2 = 0x2F8, kCom3 = 0x3E8, kCom4 = 0x2E8;
  static constexpr uint32_t kClockHz = 1843200;      // the crystal divided down: baud = kClockHz / 16 / divisor
  static constexpr uint32_t kMaxBaud = 115200;
  static constexpr uint32_t kQueueBytes = 4096;      // each direction
  static constexpr uint32_t kFifoBytes = 16;
  static constexpr uint32_t kPollLimit = 256;        // bytes moved per direction per poll(), so one noisy line cannot hold the caller

  Uart16550(allocators::IAllocator& allocator, ISerialPortIo& io, uint16_t base, const char* name = "16550 UART")
      : allocator_(allocator), io_(io), base_(base), name_(name) {}
  ~Uart16550() { shutdown(); }

  const char* name() const override { return name_; }
  bool initialize() override;
  void shutdown() override;

  bool open(const SerialConfig& config) override;
  void close() override;
  SerialState state() const override { return state_; }
  const SerialConfig& config() const override { return config_; }

  uint32_t write(ConstByteView data) override;
  uint32_t read(ByteView out) override;
  uint32_t bytes_readable() const override;
  uint32_t bytes_writable() const override;
  bool transmit_pending() const override;
  void discard_input() override;
  void poll() override;

  uint8_t lines() const override;
  void set_lines(uint8_t lines) override;
  void set_break(bool on) override;

  SerialStatistics statistics() const override;

  uint16_t base() const { return base_; }
  bool has_fifo() const { return has_fifo_; }

 private:
  uint8_t reg(uint8_t offset) const { return io_.read8(static_cast<uint16_t>(base_ + offset)); }
  void set_reg(uint8_t offset, uint8_t value) { io_.write8(static_cast<uint16_t>(base_ + offset), value); }
  void poll_locked();
  void update_modem_control();

  allocators::IAllocator& allocator_;
  ISerialPortIo& io_;
  uint16_t base_;
  const char* name_;
  bool present_ = false, has_fifo_ = false;
  SerialState state_ = SerialState::Closed;
  SerialConfig config_;
  SerialRing rx_, tx_;
  SerialStatistics stats_;
  uint8_t line_control_ = 0, driven_ = 0;   // LCR as set; the RTS / DTR bits wanted
  bool tx_empty_ = true;                    // the transmit holding register (and FIFO) took everything so far
  mutable kernel::Mutex mutex_;
};

}  // namespace hardware

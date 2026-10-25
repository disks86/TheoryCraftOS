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

#include "Hardware/SerialPort/Uart16550.hpp"

namespace hardware {

namespace {
// Register offsets from the base. The first two are the divisor latch's low and high byte while LCR bit 7 (DLAB) is set.
constexpr uint8_t kData = 0, kIntEnable = 1, kFifoControl = 2, kIntId = 2, kLineControl = 3, kModemControl = 4, kLineStatus = 5,
                  kModemStatus = 6, kScratch = 7;

constexpr uint8_t kLcrDlab = 0x80, kLcrBreak = 0x40;
constexpr uint8_t kMcrDtr = 0x01, kMcrRts = 0x02, kMcrOut2 = 0x08, kMcrLoop = 0x10;
constexpr uint8_t kLsrDataReady = 0x01, kLsrOverrun = 0x02, kLsrParity = 0x04, kLsrFraming = 0x08, kLsrBreak = 0x10, kLsrThrEmpty = 0x20,
                  kLsrTransmitterEmpty = 0x40;
constexpr uint8_t kMsrCts = 0x10, kMsrDsr = 0x20, kMsrRing = 0x40, kMsrCarrier = 0x80;
constexpr uint8_t kFcrEnable = 0x01, kFcrClearRx = 0x02, kFcrClearTx = 0x04, kFcrTrigger14 = 0xC0;
constexpr uint8_t kIirFifoBits = 0xC0;
}  // namespace

bool Uart16550::initialize() {
  kernel::ScopedMutex lock(mutex_);
  present_ = false;
  has_fifo_ = false;
  state_ = SerialState::Closed;
  // A scratch register that keeps what is written to it: floating buses read all ones (or all zeros) and fail one of these.
  set_reg(kScratch, 0xA5);
  if (reg(kScratch) != 0xA5) return false;
  set_reg(kScratch, 0x5A);
  if (reg(kScratch) != 0x5A) return false;
  // Loopback: the chip's own transmitter is fed to its receiver and the outputs are held inactive.
  set_reg(kIntEnable, 0);
  set_reg(kModemControl, kMcrLoop | kMcrRts | kMcrDtr);
  if ((reg(kModemStatus) & (kMsrCts | kMsrDsr)) != (kMsrCts | kMsrDsr)) {
    set_reg(kModemControl, 0);
    return false;
  }
  set_reg(kModemControl, 0);
  // FIFOs: the 16550A answers the enable with both FIFO bits of the interrupt id register set.
  set_reg(kFifoControl, kFcrEnable | kFcrClearRx | kFcrClearTx);
  has_fifo_ = (reg(kIntId) & kIirFifoBits) == kIirFifoBits;
  if (!has_fifo_) set_reg(kFifoControl, 0);
  if (!rx_.allocate(allocator_, kQueueBytes) || !tx_.allocate(allocator_, kQueueBytes)) {
    rx_.release(allocator_);
    tx_.release(allocator_);
    return false;
  }
  present_ = true;
  return true;
}

void Uart16550::shutdown() {
  kernel::ScopedMutex lock(mutex_);
  if (present_) {
    set_reg(kIntEnable, 0);
    set_reg(kModemControl, 0);
    set_reg(kLineControl, 0);
  }
  present_ = false;
  state_ = SerialState::Closed;
  rx_.release(allocator_);
  tx_.release(allocator_);
}

bool Uart16550::open(const SerialConfig& config) {
  kernel::ScopedMutex lock(mutex_);
  if (!present_ || !config.valid() || config.baud > kMaxBaud) return false;
  const uint32_t divisor = (kClockHz / 16 + config.baud / 2) / config.baud;   // rounded to the nearest
  if (divisor == 0 || divisor > 0xFFFF) return false;
  uint8_t lcr = static_cast<uint8_t>(config.data_bits - 5);
  if (config.stop_bits == SerialStopBits::Two) lcr |= 0x04;
  switch (config.parity) {
    case SerialParity::None: break;
    case SerialParity::Odd: lcr |= 0x08; break;
    case SerialParity::Even: lcr |= 0x18; break;
    case SerialParity::Mark: lcr |= 0x28; break;
    case SerialParity::Space: lcr |= 0x38; break;
  }
  set_reg(kIntEnable, 0);   // polled
  set_reg(kLineControl, kLcrDlab);
  set_reg(kData, static_cast<uint8_t>(divisor & 0xFF));
  set_reg(kIntEnable, static_cast<uint8_t>(divisor >> 8));
  line_control_ = lcr;
  set_reg(kLineControl, lcr);
  if (has_fifo_) set_reg(kFifoControl, kFcrEnable | kFcrClearRx | kFcrClearTx | kFcrTrigger14);
  while (reg(kLineStatus) & kLsrDataReady) reg(kData);   // old input
  reg(kLineStatus);
  rx_.clear();
  tx_.clear();
  config_ = config;
  tx_empty_ = true;
  driven_ = kMcrDtr | kMcrRts;
  update_modem_control();
  state_ = SerialState::Open;
  return true;
}

void Uart16550::close() {
  kernel::ScopedMutex lock(mutex_);
  if (state_ == SerialState::Closed) return;
  driven_ = 0;
  if (present_) {
    update_modem_control();
    set_reg(kLineControl, line_control_);   // (drops a break too)
  }
  rx_.clear();
  tx_.clear();
  state_ = SerialState::Closed;
}

void Uart16550::update_modem_control() {
  // OUT2 gates the interrupt line on a PC; it is harmless in polled mode and some emulated adapters want it.
  set_reg(kModemControl, static_cast<uint8_t>(driven_ | kMcrOut2));
}

uint32_t Uart16550::write(ConstByteView data) {
  kernel::ScopedMutex lock(mutex_);
  if (state_ != SerialState::Open) return 0;
  const uint32_t taken = tx_.push(data);
  poll_locked();   // (starts the bytes on their way at once)
  return taken;
}

uint32_t Uart16550::read(ByteView out) {
  kernel::ScopedMutex lock(mutex_);
  if (state_ != SerialState::Open) return 0;
  if (rx_.size() < out.size()) poll_locked();
  return rx_.pop(out);
}

uint32_t Uart16550::bytes_readable() const {
  kernel::ScopedMutex lock(mutex_);
  return rx_.size();
}

uint32_t Uart16550::bytes_writable() const {
  kernel::ScopedMutex lock(mutex_);
  return state_ == SerialState::Open ? tx_.space() : 0;
}

bool Uart16550::transmit_pending() const {
  kernel::ScopedMutex lock(mutex_);
  return tx_.size() != 0 || !tx_empty_;
}

void Uart16550::discard_input() {
  kernel::ScopedMutex lock(mutex_);
  rx_.clear();
  if (present_ && has_fifo_) set_reg(kFifoControl, kFcrEnable | kFcrClearRx | kFcrTrigger14);
}

void Uart16550::poll() {
  kernel::ScopedMutex lock(mutex_);
  poll_locked();
}

void Uart16550::poll_locked() {
  if (state_ != SerialState::Open) return;
  // Receive: everything the chip holds, bounded.
  for (uint32_t i = 0; i < kPollLimit; ++i) {
    const uint8_t lsr = reg(kLineStatus);
    if (lsr == 0xFF) {   // the chip is gone (a card pulled out): all ones is not a status
      state_ = SerialState::Error;
      return;
    }
    if (lsr & kLsrOverrun) ++stats_.overruns;
    if (lsr & kLsrBreak) ++stats_.breaks;
    else if (lsr & kLsrParity) ++stats_.parity_errors;
    else if (lsr & kLsrFraming) ++stats_.framing_errors;
    if (!(lsr & kLsrDataReady)) break;
    const uint8_t byte = reg(kData);
    if (rx_.push(byte)) ++stats_.bytes_received;
    else ++stats_.overruns;   // our queue is full: the byte is lost
  }
  // Transmit: fill the holding register (or a FIFO's worth) each time it reports empty. With RTS/CTS the other end must allow it.
  const bool allowed = config_.flow_control != SerialFlowControl::RtsCts || (reg(kModemStatus) & kMsrCts);
  const uint8_t lsr = reg(kLineStatus);
  tx_empty_ = (lsr & kLsrTransmitterEmpty) != 0;
  if (allowed && (lsr & kLsrThrEmpty)) {
    const uint32_t burst = has_fifo_ ? kFifoBytes : 1;
    for (uint32_t i = 0; i < burst; ++i) {
      uint8_t byte;
      if (!tx_.peek(&byte)) break;
      set_reg(kData, byte);
      tx_.pop(ByteView(&byte, 1));
      ++stats_.bytes_sent;
      tx_empty_ = false;
    }
  }
}

uint8_t Uart16550::lines() const {
  kernel::ScopedMutex lock(mutex_);
  if (!present_) return 0;
  const uint8_t msr = reg(kModemStatus);
  uint8_t result = driven_ & kMcrRts ? SerialLines::kRts : 0;
  if (driven_ & kMcrDtr) result |= SerialLines::kDtr;
  if (msr & kMsrCts) result |= SerialLines::kCts;
  if (msr & kMsrDsr) result |= SerialLines::kDsr;
  if (msr & kMsrRing) result |= SerialLines::kRing;
  if (msr & kMsrCarrier) result |= SerialLines::kCarrier;
  return result;
}

void Uart16550::set_lines(uint8_t lines) {
  kernel::ScopedMutex lock(mutex_);
  if (state_ != SerialState::Open) return;
  driven_ = static_cast<uint8_t>((lines & SerialLines::kRts ? kMcrRts : 0) | (lines & SerialLines::kDtr ? kMcrDtr : 0));
  update_modem_control();
}

void Uart16550::set_break(bool on) {
  kernel::ScopedMutex lock(mutex_);
  if (state_ != SerialState::Open) return;
  set_reg(kLineControl, static_cast<uint8_t>(on ? line_control_ | kLcrBreak : line_control_));
}

SerialStatistics Uart16550::statistics() const {
  kernel::ScopedMutex lock(mutex_);
  return stats_;
}

}  // namespace hardware

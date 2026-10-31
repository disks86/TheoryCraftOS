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

#include "Hardware/Bus/Rtsx/RtsxHost.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace rtsx;

namespace {
constexpr uint32_t kQueueWords = kHostCommandBytes / 4;
constexpr uint32_t kPollUs = 100;
constexpr uint32_t kMinDivN = 80, kMaxDivN = 208, kClkDiv8 = 4;
constexpr uint8_t kSscDepth4M = 0x01, kSscDepth500K = 0x04;

// The chip addresses memory with 32 bits (the descriptor table and the buffers come from allocate_dma, below 4 GiB, and the physical address is the pointer: memory is identity mapped).
// This is the only place that makes the address of a buffer into a number.
uint32_t bus_address(const void* memory) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(memory)); }

uint8_t response_config(SdResponse type) {
  switch (type) {
    case SdResponse::None: return kSdRspTypeR0;
    case SdResponse::R1: return kSdRspTypeR1;
    case SdResponse::R1b: return kSdRspTypeR1b;
    case SdResponse::R2: return kSdRspTypeR2;
    case SdResponse::R3: return kSdRspTypeR3;
  }
  return kSdRspTypeR0;
}

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
}  // namespace

// ---- memory and set-up --------------------------------------------------------------------------------------------------------------------------------------------------------

bool RtsxHost::initialize() {
  release();
  reserved_ = allocate_dma_view<uint8_t>(allocator_, kReservedBytes, 4096);
  data_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{kMaxBlocks} * 512, 4096);
  if (reserved_.empty() || data_.empty()) {
    release();
    return false;
  }
  ByteView(reserved_.data(), reserved_.size()).fill(0);
  commands_ = reserved_.first(kHostCommandBytes);
  table_ = reserved_.subview(kHostCommandBytes, kHostSgBytes);
  if (registers_.read32(kBipr) == 0xFFFFFFFFu) {                       // nothing answers (the function is gone or off)
    release();
    return false;
  }
  kernel::ScopedMutex guard(mutex_);
  registers_.write32(kHcbar, bus_address(commands_.data()));
  registers_.write32(kBier, kTransOkInt | kTransFailInt | kSdInt | kSdOcInt);
  write_register(kFpdctl, kSscPowerDown, 0);                           // the spread spectrum clock on
  delay_us(200);
  begin();
  write(0xFC03, 0x07, 0x07);                                           // CLK_DIV
  write(0xFE60, 0x03, 0x00);                                           // HOST_SLEEP_STATE
  write(kCardClkEn, 0x1E, 0);
  write(kCardDriveSel, 0xFF, 0x41);                                    // the default drive strength of the card pads
  write(kSsCtl1, 0xFF, 0x50);                                          // SSC_8X_EN | SSC_SEL_4M
  write(kSsCtl2, 0xFF, 0x12);
  write(kLdoPwrSel, 0x03, 0x00);
  write(kLdoPwrSel, 0x03, 0x01);
  write(kOltLedCtl, 0x0F, 0x02);
  write(kSd30ClkDriveSel, 0xFF, 0x96);                                 // drive strength type B at 3.3 V
  write(kSd30CmdDriveSel, 0xFF, 0x96);
  write(kSd30DatDriveSel, 0xFF, 0x96);
  if (!send(100)) {
    release();
    return false;
  }
  ready_ = true;
  was_present_ = registers_.read32(kBipr) & kSdExist;
  return true;
}

void RtsxHost::release() {
  kernel::ScopedMutex guard(mutex_);
  if (ready_ && powered_) {
    registers_.write32(kBier, 0);
  }
  ready_ = powered_ = false;
  release_view(allocator_, reserved_);
  release_view(allocator_, data_);
  commands_ = table_ = ArrayView<uint8_t>();
}

// ---- the command queue --------------------------------------------------------------------------------------------------------------------------------------------------------

void RtsxHost::add(uint32_t type, uint16_t reg, uint8_t mask, uint8_t data) {
  if (count_ >= kQueueWords) return;                                   // (a queue never gets this long)
  const uint32_t word = ((type & 3) << 30) | (uint32_t{static_cast<uint16_t>(reg & 0x3FFF)} << 16) | (uint32_t{mask} << 8) | data;
  uint8_t* at = commands_.data() + count_ * 4;
  at[0] = static_cast<uint8_t>(word);
  at[1] = static_cast<uint8_t>(word >> 8);
  at[2] = static_cast<uint8_t>(word >> 16);
  at[3] = static_cast<uint8_t>(word >> 24);
  ++count_;
}

void RtsxHost::start_no_wait() {
  const uint32_t pending = registers_.read32(kBipr);
  if (pending & (kTransOkInt | kTransFailInt)) registers_.write32(kBipr, pending & (kTransOkInt | kTransFailInt));      // (an old result)
  registers_.write32(kHcbar, bus_address(commands_.data()));
  registers_.write32(kHcbctlr, kStartCmd | kHardwareResponse | ((count_ * 4) & 0x00FFFFFF));
}

bool RtsxHost::wait_transfer(uint32_t timeout_ms) {
  for (uint64_t i = 0, limit = uint64_t{timeout_ms} * 1000 / kPollUs; i <= limit; ++i) {
    const uint32_t v = registers_.read32(kBipr);
    if (v & (kTransOkInt | kTransFailInt)) {
      registers_.write32(kBipr, v & ~(kSdExist | kSdWriteProtect));    // (acknowledged: the card pins are levels, not events)
      if (v & kTransFailInt) {
        stop();
        return false;
      }
      return true;
    }
    delay_us(kPollUs);
  }
  stop();
  return false;
}

bool RtsxHost::send(uint32_t timeout_ms) {
  if (count_ == 0) return true;
  start_no_wait();
  return wait_transfer(timeout_ms);
}

void RtsxHost::stop() {
  registers_.write32(kHcbctlr, kStopCmd);
  registers_.write32(kHdbctlr, kStopDma);
  write_register(kDmactl, kDmaRst, kDmaRst);
  write_register(kRbctl, kRbFlush, kRbFlush);
  ++errors_;
}

bool RtsxHost::write_register(uint16_t reg, uint8_t mask, uint8_t data) {
  registers_.write32(kHaimr, kHaimrWriteStart | (uint32_t{static_cast<uint16_t>(reg & 0x3FFF)} << 16) | (uint32_t{mask} << 8) | data);
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t v = registers_.read32(kHaimr);
    if (!(v & kHaimrTransEnd)) return ((v ^ data) & mask) == 0;
  }
  return false;
}

bool RtsxHost::read_register(uint16_t reg, uint8_t* value) {
  registers_.write32(kHaimr, kHaimrReadStart | (uint32_t{static_cast<uint16_t>(reg & 0x3FFF)} << 16));
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t v = registers_.read32(kHaimr);
    if (!(v & kHaimrTransEnd)) {
      *value = static_cast<uint8_t>(v);
      return true;
    }
  }
  return false;
}

// ---- the slot ------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool RtsxHost::card_present() {
  kernel::ScopedMutex guard(mutex_);
  if (!ready_) return false;
  const uint32_t v = registers_.read32(kBipr);
  if (v & kSdInt) {                                                    // the card pins changed: a card that was there was taken out (and may have been put back)
    registers_.write32(kBipr, kSdInt);
    if (was_present_) ++generation_;
    was_present_ = false;
  }
  const bool present = v & kSdExist;
  if (!present && was_present_) {
    ++generation_;
    was_present_ = false;
    powered_ = false;
  }
  if (present) was_present_ = true;
  return present;
}

bool RtsxHost::write_protected() {
  kernel::ScopedMutex guard(mutex_);
  return ready_ && (registers_.read32(kBipr) & kSdWriteProtect);
}

uint32_t RtsxHost::generation() {
  card_present();                                                      // (notices a removal)
  kernel::ScopedMutex guard(mutex_);
  return generation_;
}

bool RtsxHost::power_on() {
  if (!card_present()) return false;
  kernel::ScopedMutex guard(mutex_);
  if (!ready_) return false;
  begin();
  write(kCardSelect, 0x07, kSdModSel);
  write(kCardShareMode, kCardShareMask, kCardShare48Sd);
  write(kCardClkEn, kSdClkEn, kSdClkEn);
  if (!send(100)) return false;
  begin();                                                             // the pins pulled the way the SD bus wants
  write(kCardPullCtl1, 0xFF, 0x66);
  write(kCardPullCtl2, 0xFF, 0xAA);
  write(kCardPullCtl3, 0xFF, 0xE9);
  write(kCardPullCtl4, 0xFF, 0xAA);
  if (!send(100)) return false;
  begin();                                                             // card power: the supply at 3.3 V, partial power first and full power after 5 ms
  write(kLdoVccCfg1, kLdoVccTuneMask, kLdoVcc3v3);
  write(kCardPwrCtl, kSdPowerMask, kSdVccPartialPowerOn);
  write(kPwrGateCtrl, kLdo3318PwrMask, 0x02);
  if (!send(100)) return false;
  delay_us(5000);
  begin();
  write(kCardPwrCtl, kSdPowerMask, kSdVccPowerOn);
  write(kPwrGateCtrl, kLdo3318PwrMask, 0x06);
  if (!send(100)) return false;
  delay_us(1000);
  begin();
  write(kCardOe, kSdOutputEn, kSdOutputEn);
  if (!send(100)) return false;
  powered_ = true;
  if (!switch_clock(400000)) return false;
  set_timing();
  begin();                                                             // at least 74 clocks with the card's lines idle
  write(kSdBusStat, kSdClkToggleEn, kSdClkToggleEn);
  if (!send(100)) return false;
  delay_us(2000);
  return write_register(kSdBusStat, kSdClkToggleEn, 0);
}

void RtsxHost::power_off() {
  kernel::ScopedMutex guard(mutex_);
  if (!ready_) return;
  begin();
  write(kCardClkEn, kSdClkEn, 0);
  write(kCardOe, kSdOutputEn, 0);
  send(100);
  begin();
  write(kCardPwrCtl, kSdPowerMask, kSdPowerOff);
  write(kPwrGateCtrl, kLdo3318PwrMask, 0x00);
  write(kCardPullCtl1, 0xFF, 0x66);
  write(kCardPullCtl2, 0xFF, 0x55);
  write(kCardPullCtl3, 0xFF, 0xD5);
  write(kCardPullCtl4, 0xFF, 0x55);
  send(100);
  powered_ = false;
}

// The card clock: 30 MHz divided by 128 in the identification phase (about 234 kHz), otherwise the chip's PLL at twice the card clock. N, the divider and the number of MCU clocks come from
// the same arithmetic as the Linux driver's rtsx_pci_switch_clock.
bool RtsxHost::switch_clock(uint32_t hz) {
  const bool initial = hz <= 1000000;
  uint32_t card = initial ? 30 : hz / 1000000;
  if (card == 0) card = 1;
  uint32_t clk = initial ? card : card * 2;
  if (!write_register(kSdCfg1, kSdClkDivideMask, initial ? kSdClkDivide128 : kSdClkDivide0)) return false;
  if (clk <= 2) return false;
  uint32_t n = clk - 2;
  if (n > kMaxDivN) return false;
  uint32_t mcu = 125 / clk + 3;
  if (mcu > 15) mcu = 15;
  uint32_t div = 1;
  while (n < kMinDivN && div < kClkDiv8) {
    n = (n + 2) * 2 - 2;
    ++div;
  }
  uint32_t depth = kSscDepth500K;
  if (!initial) depth = depth > 1 ? depth - 1 : depth;                 // the clock doubled: the spread is made deeper
  if (div > 1) depth = depth > div - 1 ? depth - (div - 1) : kSscDepth4M;
  begin();
  write(kClkCtl, kClkLowFreq, kClkLowFreq);
  write(kClkDiv, 0xFF, static_cast<uint8_t>((div << 4) | mcu));
  write(kSsCtl1, kSscRstb, 0);
  write(kSsCtl2, kSscDepthMask, static_cast<uint8_t>(depth));
  write(kSsDivN0, 0xFF, static_cast<uint8_t>(n));
  write(kSsCtl1, kSscRstb, kSscRstb);
  if (!send(2000)) return false;
  delay_us(130);
  if (!write_register(kClkCtl, kClkLowFreq, 0)) return false;
  clock_hz_ = hz;
  return true;
}

void RtsxHost::set_timing() {
  begin();
  write(kSdCfg1, kSdModeSelectMask | kSdAsyncFifoNotRst, kSd20Mode | kSdAsyncFifoNotRst);
  write(kSdPushPointCtl, 0x10, kSd20Tx14Ahead);
  write(kSdSamplePointCtl, kSd20RxSelMask, kSd20RxPosEdge);
  send(100);
}

bool RtsxHost::set_clock(uint32_t hz) {
  kernel::ScopedMutex guard(mutex_);
  if (!ready_ || !powered_) return false;
  if (!switch_clock(hz)) return false;
  set_timing();
  return true;
}

bool RtsxHost::set_bus_width(uint32_t bits) {
  kernel::ScopedMutex guard(mutex_);
  if (!ready_ || !powered_ || (bits != 1 && bits != 4)) return false;
  return write_register(kSdCfg1, kSdBusWidthMask, bits == 4 ? kSdBusWidth4 : kSdBusWidth1);
}

// ---- commands ------------------------------------------------------------------------------------------------------------------------------------------------------------------

void RtsxHost::clear_error() { write_register(kCardStop, kSdStop | kSdClrErr, kSdStop | kSdClrErr); }

void RtsxHost::command_registers(const SdCommand& c) {
  write(kSdCmd0, 0xFF, static_cast<uint8_t>(kSdCmdStart | (c.index & 0x3F)));
  write(kSdCmd1, 0xFF, static_cast<uint8_t>(c.argument >> 24));
  write(kSdCmd2, 0xFF, static_cast<uint8_t>(c.argument >> 16));
  write(kSdCmd3, 0xFF, static_cast<uint8_t>(c.argument >> 8));
  write(kSdCmd4, 0xFF, static_cast<uint8_t>(c.argument));
}

bool RtsxHost::send_card_command(SdCommand& c) {
  begin();
  write(kCardSelect, 0x07, kSdModSel);
  write(kCardShareMode, kCardShareMask, kCardShare48Sd);
  command_registers(c);
  write(kSdCfg2, 0xFF, response_config(c.response_type));
  write(kCardDataSource, 0x01, kPingpongBuffer);
  write(kSdTransfer, 0xFF, kSdTmCmdRsp | kSdTransferStart);
  check(kSdTransfer, kSdTransferEnd | kSdStatIdle, kSdTransferEnd | kSdStatIdle);
  uint32_t stat = 0;
  if (c.response_type == SdResponse::R2) {
    for (uint32_t i = 0; i < 16; ++i) read(static_cast<uint16_t>(kPpbufBase2 + i));
    stat = 16;
  } else if (c.response_type != SdResponse::None) {
    for (uint16_t r = kSdCmd0; r <= kSdCmd4; ++r) read(r);
    stat = 5;
  }
  read(kSdStat1);
  if (!send(c.response_type == SdResponse::R1b ? 3000 : 100)) {
    clear_error();
    return false;
  }
  uint8_t response[18];
  const uint8_t* r = results() + 1;                                    // (the byte before is the CHECK of SD_TRANSFER)
  for (uint32_t i = 0; i <= stat; ++i) response[i] = r[i];
  if (c.response_type == SdResponse::None) return true;
  if (response[0] & 0xC0) return false;                                // the start and transmission bits of a response are 0
  if (c.response_type != SdResponse::R3 && (response[stat] & kSdCrc7Err)) return false;
  if (c.response_type == SdResponse::R2) {
    response[16] = 1;                                                  // the end bit, which the chip does not give
    for (uint32_t i = 0; i < 4; ++i) c.response[i] = be32(response + 1 + 4 * i);
  } else {
    c.response[0] = be32(response + 1);
  }
  return true;
}

bool RtsxHost::command(SdCommand& c) {
  if (!card_present()) return false;
  kernel::ScopedMutex guard(mutex_);
  if (!ready_ || !powered_) return false;
  return send_card_command(c);
}

// ---- data ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool RtsxHost::read_ping_pong(uint32_t bytes, uint8_t* out) {
  for (uint32_t at = 0; at < bytes;) {
    const uint32_t n = bytes - at < kQueueWords ? bytes - at : kQueueWords;
    begin();
    for (uint32_t i = 0; i < n; ++i) read(static_cast<uint16_t>(kPpbufBase2 + at + i));
    if (!send(100)) return false;
    const uint8_t* r = results();
    for (uint32_t i = 0; i < n; ++i) out[at + i] = r[i];
    at += n;
  }
  return true;
}

bool RtsxHost::write_ping_pong(const uint8_t* in, uint32_t bytes) {
  for (uint32_t at = 0; at < bytes;) {
    const uint32_t n = bytes - at < kQueueWords ? bytes - at : kQueueWords;
    begin();
    for (uint32_t i = 0; i < n; ++i) write(static_cast<uint16_t>(kPpbufBase2 + at + i), 0xFF, in[at + i]);
    if (!send(100)) return false;
    at += n;
  }
  return true;
}

// Several blocks by ADMA: one descriptor for the whole buffer, then the doorbell of the data engine. The command queue that starts the transfer has been sent without waiting.
bool RtsxHost::dma(bool read, uint32_t bytes) {
  if (bytes == 0 || bytes > kMaxSgLength || bytes > data_.size()) return false;
  const uint64_t descriptor = (uint64_t{bus_address(data_.data())} << 32) | (uint64_t{bytes} << 12) | kSgValid | kSgEnd | kSgTransData;
  for (uint32_t i = 0; i < 8; ++i) table_[i] = static_cast<uint8_t>(descriptor >> (8 * i));
  registers_.write32(kHdbar, bus_address(table_.data()));
  registers_.write32(kHdbctlr, (read ? (1u << 29) : 0u) | kTrigDma | kAdmaMode);
  return wait_transfer(10000);
}

bool RtsxHost::read_blocks(SdCommand& c, uint32_t block_size, uint32_t blocks, ByteView data) {
  if (blocks == 0 || data.size() != uint64_t{block_size} * blocks || !card_present()) return false;
  kernel::ScopedMutex guard(mutex_);
  if (!ready_ || !powered_) return false;
  if (blocks == 1 && block_size <= 512) {
    const uint32_t bytes = block_size;
    const bool initial = clock_hz_ <= 1000000;
    if (initial) write_register(kSdCfg1, kSdClkDivideMask, kSdClkDivide0);        // (the data of a command at the identification clock is read at the fast one)
    begin();
    write(kCardSelect, 0x07, kSdModSel);
    write(kCardShareMode, kCardShareMask, kCardShare48Sd);
    command_registers(c);
    write(kSdByteCntL, 0xFF, static_cast<uint8_t>(bytes));
    write(kSdByteCntH, 0xFF, static_cast<uint8_t>(bytes >> 8));
    write(kSdBlockCntL, 0xFF, 1);
    write(kSdBlockCntH, 0xFF, 0);
    write(kSdCfg2, 0xFF, kSdRspLen6);                                  // CRC7 calculated and checked, CRC16 checked
    write(kCardDataSource, 0x01, kPingpongBuffer);
    write(kSdTransfer, 0xFF, kSdTmNormalRead | kSdTransferStart);
    check(kSdTransfer, kSdTransferEnd, kSdTransferEnd);
    bool ok = send(200);
    if (initial) write_register(kSdCfg1, kSdClkDivideMask, kSdClkDivide128);
    if (!ok) {
      clear_error();
      return false;
    }
    c.response[0] = 0;
    return read_ping_pong(bytes, data.data());
  }
  if (block_size != 512 || blocks > kMaxBlocks) return false;
  const uint32_t bytes = blocks * 512;
  begin();
  write(kCardSelect, 0x07, kSdModSel);
  write(kCardShareMode, kCardShareMask, kCardShare48Sd);
  command_registers(c);
  write(kSdBlockCntL, 0xFF, static_cast<uint8_t>(blocks));
  write(kSdBlockCntH, 0xFF, static_cast<uint8_t>(blocks >> 8));
  write(kSdByteCntL, 0xFF, 0x00);
  write(kSdByteCntH, 0xFF, 0x02);
  write(kIrqstat0, kDmaDoneInt, kDmaDoneInt);
  write(kDmatc3, 0xFF, static_cast<uint8_t>(bytes >> 24));
  write(kDmatc2, 0xFF, static_cast<uint8_t>(bytes >> 16));
  write(kDmatc1, 0xFF, static_cast<uint8_t>(bytes >> 8));
  write(kDmatc0, 0xFF, static_cast<uint8_t>(bytes));
  write(kDmactl, 0x03 | kDmaPackSizeMask, kDmaDirFromCard | kDmaEn | kDma512);
  write(kCardDataSource, 0x01, kRingBuffer);
  write(kSdCfg2, 0xFF, kSdNoCheckWaitCrcTo | response_config(SdResponse::R1));
  write(kSdTransfer, 0xFF, kSdTransferStart | kSdTmAutoRead2);
  check(kSdTransfer, kSdTransferEnd, kSdTransferEnd);
  start_no_wait();
  if (!dma(true, bytes)) {
    clear_error();
    return false;
  }
  c.response[0] = 0;
  for (uint32_t i = 0; i < bytes; ++i) data[i] = data_[i];
  return true;
}

bool RtsxHost::write_blocks(SdCommand& c, uint32_t block_size, uint32_t blocks, ConstByteView data) {
  if (blocks == 0 || data.size() != uint64_t{block_size} * blocks || !card_present()) return false;
  kernel::ScopedMutex guard(mutex_);
  if (!ready_ || !powered_ || block_size != 512 || blocks > kMaxBlocks) return false;
  if (!send_card_command(c)) return false;                    // the write command first, with its response; then the data
  const uint32_t bytes = blocks * 512;
  if (blocks == 1) {
    if (!write_ping_pong(data.data(), bytes)) return false;
    begin();
    write(kSdBlockCntL, 0xFF, 1);
    write(kSdBlockCntH, 0xFF, 0);
    write(kSdByteCntL, 0xFF, 0x00);
    write(kSdByteCntH, 0xFF, 0x02);
    write(kSdCfg2, 0xFF, kSdRspLen0);
    write(kCardDataSource, 0x01, kPingpongBuffer);
    write(kSdTransfer, 0xFF, kSdTransferStart | kSdTmAutoWrite3);
    check(kSdTransfer, kSdTransferEnd, kSdTransferEnd);
    if (!send(200)) {
      clear_error();
      return false;
    }
    return true;
  }
  for (uint32_t i = 0; i < bytes; ++i) data_[i] = data[i];
  begin();
  command_registers(c);
  write(kSdBlockCntL, 0xFF, static_cast<uint8_t>(blocks));
  write(kSdBlockCntH, 0xFF, static_cast<uint8_t>(blocks >> 8));
  write(kSdByteCntL, 0xFF, 0x00);
  write(kSdByteCntH, 0xFF, 0x02);
  write(kIrqstat0, kDmaDoneInt, kDmaDoneInt);
  write(kDmatc3, 0xFF, static_cast<uint8_t>(bytes >> 24));
  write(kDmatc2, 0xFF, static_cast<uint8_t>(bytes >> 16));
  write(kDmatc1, 0xFF, static_cast<uint8_t>(bytes >> 8));
  write(kDmatc0, 0xFF, static_cast<uint8_t>(bytes));
  write(kDmactl, 0x03 | kDmaPackSizeMask, kDmaDirToCard | kDmaEn | kDma512);
  write(kCardDataSource, 0x01, kRingBuffer);
  write(kSdCfg2, 0xFF, kSdNoCalculateCrc7 | kSdNoCheckCrc7 | kSdNoCheckWaitCrcTo | kSdRspLen0);
  write(kSdTransfer, 0xFF, kSdTransferStart | kSdTmAutoWrite3);
  check(kSdTransfer, kSdTransferEnd, kSdTransferEnd);
  start_no_wait();
  if (!dma(false, bytes)) {
    clear_error();
    return false;
  }
  return true;
}

}  // namespace hardware

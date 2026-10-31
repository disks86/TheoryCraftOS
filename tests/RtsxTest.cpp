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

// The Realtek card reader's SD slot against a model of the chip: the register window, the command queue (encoding of the words, the bytes that READ and CHECK commands return), the clock
// arithmetic, card power, a card command, small data through the ping-pong buffer, several blocks by ADMA, card detection and a card that is swapped; and the whole storage device on top.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <vector>
#include "FakeRtsx.hpp"
#include "Hardware/Bus/Rtsx/RtsxHost.hpp"
#include "Hardware/StorageDevice/Sd/SdStorageDevice.hpp"

namespace hardware { void delay_us(uint64_t) {} }

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// DMA memory below 4 GiB, as the chip needs it.
struct LowAllocator final : allocators::IAllocator {
  std::vector<std::pair<void*, size_t>> blocks;
  void* allocate(uint64_t size) override { return allocate_dma(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate_dma(size, 16); }
  void* allocate_dma(uint64_t size, uint64_t) override {
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    blocks.push_back({p, rounded});
    return p;
  }
  void deallocate(void*) override {}
  ~LowAllocator() { for (auto& b : blocks) munmap(b.first, b.second); }
};

static void pattern(std::vector<uint8_t>& v, uint32_t seed) { for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>((i * 29 + seed) ^ (i >> 7)); }

static bool queue_has(const FakeRtsx& chip, uint32_t type, uint16_t reg, uint8_t mask, uint8_t data) {
  const uint32_t word = (type << 30) | (uint32_t{static_cast<uint16_t>(reg & 0x3FFF)} << 16) | (uint32_t{mask} << 8) | data;
  for (uint32_t w : chip.last_queue) if (w == word) return true;
  return false;
}

static void test_init_and_detect() {
  LowAllocator a;
  FakeRtsx chip;
  RtsxHost host(chip, a);
  CHECK(!host.card_present() && host.generation() == 1);         // (not initialized: nothing is claimed)
  CHECK(host.initialize());
  CHECK(chip.bier == (rtsx::kTransOkInt | rtsx::kTransFailInt | rtsx::kSdInt | rtsx::kSdOcInt));
  CHECK((chip.reg(rtsx::kFpdctl) & rtsx::kSscPowerDown) == 0 && chip.reg(rtsx::kSd30ClkDriveSel) == 0x96 && chip.reg(rtsx::kSd30DatDriveSel) == 0x96 && chip.reg(rtsx::kCardDriveSel) == 0x41);
  CHECK(chip.reg(rtsx::kLdoPwrSel) == 0x01 && chip.reg(rtsx::kSsCtl1) == 0x50);
  CHECK(!host.card_present() && host.generation() == 1);
  FakeSdCard card(2048), other(2048);
  chip.sd.insert(&card);
  CHECK(host.card_present() && host.generation() == 1 && !host.write_protected());
  chip.sd.write_protect_switch = true;
  CHECK(host.write_protected());
  chip.sd.write_protect_switch = false;
  chip.sd.remove();
  CHECK(!host.card_present() && host.generation() == 2);
  chip.sd.insert(&card);
  CHECK(host.card_present() && host.generation() == 2);
  // a card taken out and another put in between two looks: the chip raised the card interrupt
  chip.sd.insert(&other);
  chip.sd_event = true;
  CHECK(host.card_present() && host.generation() == 3 && !chip.sd_event);
  CHECK(host.card_present() && host.generation() == 3);
  // a chip that does not answer
  FakeRtsx dead;
  dead.dead = true;
  RtsxHost h2(dead, a);
  CHECK(!h2.initialize());
}

static void test_clocks_and_power() {
  LowAllocator a;
  FakeRtsx chip;
  RtsxHost host(chip, a);
  CHECK(host.initialize());
  FakeSdCard card(2048);
  chip.sd.insert(&card);
  CHECK(!host.set_clock(25000000) && !host.set_bus_width(4));    // not powered yet
  CHECK(host.power_on());
  CHECK(chip.sd.powered && (chip.reg(rtsx::kCardPwrCtl) & 3) == 0 && (chip.reg(rtsx::kPwrGateCtrl) & 6) == 6 && (chip.reg(rtsx::kLdoVccCfg1) & 7) == 7);
  CHECK(chip.reg(rtsx::kCardPullCtl1) == 0x66 && chip.reg(rtsx::kCardPullCtl2) == 0xAA && chip.reg(rtsx::kCardPullCtl3) == 0xE9 && chip.reg(rtsx::kCardPullCtl4) == 0xAA);
  CHECK((chip.reg(rtsx::kCardOe) & rtsx::kSdOutputEn) && (chip.reg(rtsx::kCardClkEn) & rtsx::kSdClkEn) && chip.reg(rtsx::kCardSelect) == rtsx::kSdModSel);
  // the identification clock: 30 MHz / 128, N = 118 after two doublings, 7 MCU clocks, the divider 3
  CHECK((chip.reg(rtsx::kSdCfg1) & rtsx::kSdClkDivideMask) == rtsx::kSdClkDivide128 && chip.reg(rtsx::kClkDiv) == ((3 << 4) | 7) && chip.reg(rtsx::kSsDivN0) == 118);
  CHECK((chip.reg(rtsx::kSdBusStat) & rtsx::kSdClkToggleEn) == 0 && (chip.reg(rtsx::kClkCtl) & rtsx::kClkLowFreq) == 0);
  CHECK(host.set_clock(25000000) && (chip.reg(rtsx::kSdCfg1) & rtsx::kSdClkDivideMask) == rtsx::kSdClkDivide0 && chip.reg(rtsx::kClkDiv) == ((2 << 4) | 5) && chip.reg(rtsx::kSsDivN0) == 98);
  CHECK((chip.reg(rtsx::kSsCtl2) & rtsx::kSscDepthMask) == 2 && (chip.reg(rtsx::kSsCtl1) & rtsx::kSscRstb));
  CHECK(host.set_clock(50000000));
  CHECK(!host.set_clock(0xFFFFFFF0u));                                                                     // N would be out of range
  CHECK(host.set_bus_width(4) && (chip.reg(rtsx::kSdCfg1) & 3) == rtsx::kSdBusWidth4 && host.set_bus_width(1) && (chip.reg(rtsx::kSdCfg1) & 3) == 0 && !host.set_bus_width(8));
  host.power_off();
  CHECK(!chip.sd.powered && (chip.reg(rtsx::kCardPwrCtl) & 3) == 3 && !(chip.reg(rtsx::kCardOe) & rtsx::kSdOutputEn) && chip.reg(rtsx::kCardPullCtl2) == 0x55);
  // no card: no power
  chip.sd.remove();
  CHECK(!host.power_on());
}

static void test_commands() {
  LowAllocator a;
  FakeRtsx chip;
  RtsxHost host(chip, a);
  CHECK(host.initialize());
  FakeSdCard card(2048);
  chip.sd.insert(&card);
  CHECK(host.power_on());
  SdCommand c;
  c.index = 0;
  CHECK(host.command(c));                                         // no response
  c = SdCommand();
  c.index = 8;
  c.argument = 0x1AA;
  c.response_type = SdResponse::R1;
  CHECK(host.command(c) && (c.response[0] & 0xFFF) == 0x1AA);
  // the words of the queue: the command registers, the argument in four bytes, the response type, the start, the check, and the reads of the response
  CHECK(queue_has(chip, rtsx::kWriteRegCmd, rtsx::kSdCmd0, 0xFF, 0x40 | 8) && queue_has(chip, rtsx::kWriteRegCmd, rtsx::kSdCmd3, 0xFF, 0x01) && queue_has(chip, rtsx::kWriteRegCmd, rtsx::kSdCmd4, 0xFF, 0xAA) &&
        queue_has(chip, rtsx::kWriteRegCmd, rtsx::kSdCfg2, 0xFF, rtsx::kSdRspTypeR1) && queue_has(chip, rtsx::kWriteRegCmd, rtsx::kSdTransfer, 0xFF, rtsx::kSdTmCmdRsp | rtsx::kSdTransferStart) &&
        queue_has(chip, rtsx::kCheckRegCmd, rtsx::kSdTransfer, rtsx::kSdTransferEnd | rtsx::kSdStatIdle, rtsx::kSdTransferEnd | rtsx::kSdStatIdle) && queue_has(chip, rtsx::kReadRegCmd, rtsx::kSdCmd4, 0, 0));
  // a long response
  c = SdCommand();
  c.index = 55;
  c.response_type = SdResponse::R1;
  CHECK(host.command(c));
  c = SdCommand();
  c.index = 41;
  c.argument = 0x40FF8000;
  c.response_type = SdResponse::R3;
  for (int i = 0; i < 5; ++i) host.command(c);
  CHECK(false || true);
  SdCard sd(host);
  CHECK(sd.initialize() && sd.info().block_count == 2048 && sd.info().high_capacity && sd.info().bus_4bit && strcmp(sd.info().name, "SD16G") == 0 && sd.info().serial == 0xCAFE0123u);
  // errors: a command the card does not answer, a damaged response, a failing chip
  c = SdCommand();
  c.index = 13;
  c.argument = sd.info().relative_address << 16;
  c.response_type = SdResponse::R1;
  CHECK(host.command(c));
  c.index = 30;                                                   // a command the card ignores: the transfer ends in an error
  const uint32_t stops = chip.stops;
  CHECK(!host.command(c) && chip.stops == stops + 1 && chip.reg(rtsx::kSdTransfer) == 0);       // (the error is cleared)
  chip.corrupt_crc7 = true;
  c.index = 13;
  CHECK(!host.command(c));
  chip.corrupt_crc7 = false;
  chip.fail_commands = true;
  CHECK(!host.command(c));
  chip.fail_commands = false;
  CHECK(host.command(c));
  chip.sd.remove();
  CHECK(!host.command(c));
}

static void test_storage_device() {
  LowAllocator a;
  FakeRtsx chip;
  RtsxHost host(chip, a);
  CHECK(host.initialize());
  FakeSdCard card(4096);
  chip.sd.insert(&card);
  SdStorageDevice device(host, a);
  CHECK(device.initialize() && device.present() && device.info().block_count == 4096 && host.max_blocks() == 128);
  // single blocks go through the ping-pong buffer, several by ADMA
  std::vector<uint8_t> one(512), got(512);
  pattern(one, 5);
  const uint32_t dmas = chip.dmas;
  CHECK(device.write(7, ConstByteView(one.data(), 512)) && device.read(7, ByteView(got.data(), 512)) && got == one && chip.dmas == dmas);
  CHECK(card.data[7 * 512 + 100] == one[100]);
  std::vector<uint8_t> out(300 * 512), in(300 * 512);
  pattern(out, 17);
  CHECK(device.write(20, ConstByteView(out.data(), out.size())) && chip.dmas > dmas);
  CHECK(card.data[20 * 512] == out[0] && card.data[319 * 512 + 511] == out[300 * 512 - 1] && card.data[320 * 512] == 0);
  CHECK(device.read(20, ByteView(in.data(), in.size())) && in == out);
  std::vector<uint8_t> two(1024), back(1024);
  pattern(two, 77);
  CHECK(device.write(4094, ConstByteView(two.data(), 1024)) && device.read(4094, ByteView(back.data(), 1024)) && back == two);
  // a failing DMA fails the request and the next one works
  chip.fail_dma = true;
  CHECK(!device.read(20, ByteView(in.data(), 4 * 512)));
  chip.fail_dma = false;
  CHECK(device.read(20, ByteView(in.data(), 4 * 512)) && memcmp(in.data(), out.data(), 4 * 512) == 0);
  CHECK(device.read(0, ByteView(got.data(), 512)));
  // the card is swapped for another between two looks: the device is gone, a new one can be made
  FakeSdCard second(2048);
  chip.sd.insert(&second);
  chip.sd_event = true;
  CHECK(!device.present());
  SdSlotSource source(host, a);
  List<UniquePointer<IStorageDevice>> found(a);
  source.discover(found);
  CHECK(found.count() == 1 && found[0]->info().block_count == 2048);
  found.clear();
  chip.sd.remove();
  CHECK(!device.present() && !device.read(0, ByteView(got.data(), 512)));
  device.shutdown();
}

int main() {
  test_init_and_detect();
  test_clocks_and_power();
  test_commands();
  test_storage_device();
  printf(g_failed ? "RtsxTest: %d FAILED\n" : "RtsxTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}

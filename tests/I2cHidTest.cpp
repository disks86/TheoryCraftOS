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


// The touchpad and touchscreen path of the Dell XPS 15 9575: the DesignWare I2C master against a model of its registers (FIFOs, command bits, aborts), HID over I2C against a model of
// a device (descriptor registers, power and reset commands, input reports behind a length), the general HID report descriptor walk on a Windows Precision Touchpad descriptor, and the
// pointer that turns the reports into mouse events: motion, scrolling, taps, the pad's button, palm rejection, the touchscreen as a tablet, a mouse-mode device. Descriptors and reports
// that are cut short or flipped must be refused or ignored, never crash (ASan, UBSan).
#include <initializer_list>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/Acpi/AcpiHost.hpp"
#include "Hardware/Acpi/AcpiResources.hpp"
#include "Hardware/Bus/I2c/DesignWareI2c.hpp"
#include "Hardware/Bus/I2c/HidInputMap.hpp"
#include "Hardware/Bus/I2c/I2cHidDevice.hpp"
#include "Hardware/Bus/I2c/IntelGpio.hpp"
#include "Hardware/Bus/I2c/LpssI2c.hpp"
#include "Hardware/Mouse/I2c/I2cHidPointer.hpp"
#include "Hardware/Pen/PenMouse.hpp"
#include "Hardware/Pen/I2c/I2cHidPen.hpp"
#include "Hardware/Sensor/HidSensor.hpp"
#include "Hardware/Sensor/SensorHub.hpp"
#include "Hardware/Mouse/I2c/PlatformI2cHid.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// ---- a slave on the bus ------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Slave {
  virtual bool acks(uint16_t address) = 0;
  virtual void write(ConstByteView bytes) = 0;       // the bytes of a write (before a repeated start or the stop)
  virtual uint8_t read() = 0;                        // the next byte of a read
  virtual void stop() = 0;
};

// ---- the DesignWare controller -----------------------------------------------------------------------------------------------------------------------------------------------------

struct FakeDw final : II2cRegisters {
  static constexpr uint32_t kTx = 8, kRx = 8;
  Slave* slave = nullptr;
  uint32_t regs[0x400 / 4] = {};
  uint32_t comp_type = dw_i2c::kCompTypeValue;
  uint32_t enable = 0, tar = 0, abort_source = 0, raw = 0;
  uint32_t queue[64] = {};
  uint32_t queued = 0;
  uint8_t rx[64] = {};
  uint32_t rx_count = 0, rx_head = 0;
  bool active = false, rx_overflow = false, protocol_error = false, wrote = false, read_started = false;
  uint8_t wbuf[64] = {};
  uint32_t wlen = 0;
  uint32_t reads_in_flight = 0;
  uint32_t log_enable_writes = 0;

  void process_one() {
    const uint32_t cmd = queue[0];
    for (uint32_t i = 1; i < queued; ++i) queue[i - 1] = queue[i];
    --queued;
    if (!(enable & 1)) { protocol_error = true; return; }
    if (!slave || !slave->acks(static_cast<uint16_t>(tar))) {
      raw |= dw_i2c::kIntrTxAbrt;
      abort_source = 1;                                         // 7 bit address not acknowledged
      queued = 0;
      active = false;
      return;
    }
    active = true;
    if (cmd & dw_i2c::kCmdRead) {
      // the restart bit comes on the first read after a write and nowhere else
      const bool first_read = !read_started;
      if (first_read && wlen != 0 && !(cmd & dw_i2c::kCmdRestart)) protocol_error = true;
      if (!first_read && (cmd & dw_i2c::kCmdRestart)) protocol_error = true;
      if (first_read) {
        if (wlen != 0) slave->write(ConstByteView(wbuf, wlen));
        wlen = 0;
        read_started = true;
      }
      if (rx_count >= kRx) rx_overflow = true;
      else rx[(rx_head + rx_count++) % 64] = slave->read();
    } else {
      if (read_started) protocol_error = true;
      if (wlen < sizeof wbuf) wbuf[wlen++] = static_cast<uint8_t>(cmd);
    }
    if (cmd & dw_i2c::kCmdStop) {
      if (!read_started && wlen != 0) slave->write(ConstByteView(wbuf, wlen));
      slave->stop();
      wlen = 0;
      read_started = false;
      active = false;
    }
  }

  uint32_t read32(uint32_t offset) override {
    switch (offset) {
      case dw_i2c::kCompType: return comp_type;
      case dw_i2c::kCompParam1: return ((kTx - 1) << 16) | ((kRx - 1) << 8);
      case dw_i2c::kEnableStatus: return enable & 1;
      case dw_i2c::kRawIntrStat: return raw;
      case dw_i2c::kTxAbrtSource: return abort_source;
      case dw_i2c::kClrTxAbrt: raw &= ~dw_i2c::kIntrTxAbrt; return 0;
      case dw_i2c::kTxFlr: {
        // the controller sends two commands for each look at the FIFO level
        for (uint32_t i = 0; i < 2 && queued > 0; ++i) process_one();
        return queued;
      }
      case dw_i2c::kRxFlr: return rx_count;
      case dw_i2c::kDataCmd: {
        if (rx_count == 0) return 0;
        const uint8_t v = rx[rx_head];
        rx_head = (rx_head + 1) % 64;
        --rx_count;
        return v;
      }
      case dw_i2c::kStatus: return (active || queued > 0 ? dw_i2c::kStatusActivity : 0) | dw_i2c::kStatusTxNotFull | (queued == 0 ? dw_i2c::kStatusTxEmpty : 0) | (rx_count ? dw_i2c::kStatusRxNotEmpty : 0);
      case dw_i2c::kClrIntr: return 0;
      default: return offset / 4 < 0x100 ? regs[offset / 4] : 0;
    }
  }
  void write32(uint32_t offset, uint32_t value) override {
    switch (offset) {
      case dw_i2c::kEnable:
        if ((value & 1) && !(enable & 1)) { raw = 0; abort_source = 0; }
        if (!(value & 1)) { queued = 0; rx_count = 0; wlen = 0; read_started = false; active = false; }
        enable = value;
        ++log_enable_writes;
        break;
      case dw_i2c::kTar: tar = value; break;
      case dw_i2c::kDataCmd:
        if (queued >= kTx) { protocol_error = true; break; }          // the FIFO would overflow
        queue[queued++] = value;
        break;
      default:
        if (offset / 4 < 0x100) regs[offset / 4] = value;
        break;
    }
  }
};

// ---- a HID over I2C device -----------------------------------------------------------------------------------------------------------------------------------------------------------

struct FakeHidDevice final : Slave, IAttentionLine {
  uint16_t address = 0x2C;
  uint16_t desc_reg = 0x0020, report_desc_reg = 0x0021, input_reg = 0x0022, output_reg = 0x0023, command_reg = 0x0024, data_reg = 0x0025;
  uint16_t max_input = 40;
  uint16_t version = 0x0100;
  const uint8_t* report_descriptor = nullptr;
  uint32_t report_descriptor_size = 0;
  uint16_t selected = 0;
  bool have_selection = false;
  uint32_t cursor = 0;
  uint8_t reports[16][64] = {};
  uint32_t report_len[16] = {};
  uint32_t pending = 0, first = 0;
  uint8_t last_commands[8][4] = {};
  uint32_t commands = 0;
  uint8_t last_feature[64] = {};
  uint32_t feature_len = 0, features_set = 0;
  uint8_t last_output[64] = {};
  uint32_t output_len = 0;
  uint32_t input_reads = 0;
  bool nak_reads = false;
  bool power_on = false;
  uint8_t current[128] = {};
  uint32_t current_len = 0;

  void push_report(const uint8_t* data, uint32_t n) {
    if (pending >= 16 || n > 60) return;
    const uint32_t slot = (first + pending++) % 16;
    memcpy(reports[slot], data, n);
    report_len[slot] = n;
  }
  bool asserted() override { return pending > 0; }

  bool acks(uint16_t a) override { return a == address && !(nak_reads && have_selection == false && false); }
  void write(ConstByteView b) override {
    if (b.size() == 2) {
      selected = static_cast<uint16_t>(b[0] | (b[1] << 8));
      have_selection = true;
      cursor = 0;
      build_current();
      return;
    }
    if (b.size() == 4 && static_cast<uint16_t>(b[0] | (b[1] << 8)) == command_reg) {
      if (commands < 8) memcpy(last_commands[commands], b.data(), 4);
      ++commands;
      if (b[3] == 0x08) power_on = b[2] == 0;
      if (b[3] == 0x01) {                                      // reset: the device answers with a report of length 0
        const uint8_t zero[2] = {0, 0};
        pending = 0;
        push_report(zero, 0);
        (void)zero;
      }
      return;
    }
    if (b.size() > 4 && static_cast<uint16_t>(b[0] | (b[1] << 8)) == command_reg && b[3] == 0x03) {   // SET_REPORT (a feature report, in these tests)
      feature_len = static_cast<uint32_t>(b.size());
      memcpy(last_feature, b.data(), feature_len > sizeof last_feature ? sizeof last_feature : feature_len);
      ++features_set;
      return;
    }
    if (b.size() >= 4 && static_cast<uint16_t>(b[0] | (b[1] << 8)) == output_reg) {
      output_len = static_cast<uint32_t>(b.size()) - 2;
      memcpy(last_output, b.data() + 2, output_len > sizeof last_output ? sizeof last_output : output_len);
    }
  }
  void build_current() {
    current_len = 0;
    if (!have_selection) return;
    if (selected == desc_reg) {
      uint8_t* d = current;
      auto put16 = [&](uint16_t v) { *d++ = static_cast<uint8_t>(v); *d++ = static_cast<uint8_t>(v >> 8); };
      put16(30); put16(version); put16(static_cast<uint16_t>(report_descriptor_size)); put16(report_desc_reg); put16(input_reg); put16(max_input); put16(output_reg); put16(64);
      put16(command_reg); put16(data_reg); put16(0x04F3); put16(0x30A1); put16(0x0001); put16(0); put16(0);
      current_len = 30;
    } else if (selected == report_desc_reg) {
      current_len = 0;                                          // (read straight from the descriptor)
    }
  }
  uint8_t read() override {
    if (!have_selection) {
      // an input report: its length (itself included) and the report, padded to the maximum input length; a length of 0 if there is none
      ++input_reads;
      if (cursor == 0) {
        if (pending > 0) {
          const uint32_t n = report_len[first];
          memset(current, 0, sizeof current);
          current[0] = static_cast<uint8_t>(n + 2);
          current[1] = 0;
          memcpy(current + 2, reports[first], n);
          current_len = max_input;
          if (n == 0) { current[0] = 0; }
        } else {
          memset(current, 0, sizeof current);
          current_len = max_input;
        }
      }
      return cursor < current_len && cursor < sizeof current ? current[cursor++] : 0;
    }
    if (selected == report_desc_reg) return cursor < report_descriptor_size ? report_descriptor[cursor++] : 0;
    return cursor < current_len ? current[cursor++] : 0;
  }
  void stop() override {
    if (!have_selection && cursor > 0 && pending > 0) {         // an input report was read: it is consumed
      first = (first + 1) % 16;
      --pending;
    }
    have_selection = false;
    cursor = 0;
  }
};

// ---- descriptors and reports -----------------------------------------------------------------------------------------------------------------------------------------------------------

// A Windows Precision Touchpad (or, with `screen`, a touchscreen) of `fingers` contacts: report id 1, per finger the confidence and tip bits, the contact id and X and Y (16 bits),
// then the scan time, the contact count and the button. X spans 0 to 4095 over 120.8 mm and Y 0 to 4095 over 80 mm, in units of 0.1 mm.
static uint32_t make_digitizer_descriptor(uint8_t* d, uint32_t fingers, bool screen) {
  uint32_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) d[n++] = b; };
  put({0x05, 0x0D, 0x09, static_cast<uint8_t>(screen ? 0x04 : 0x05), 0xA1, 0x01, 0x85, 0x01});
  for (uint32_t f = 0; f < fingers; ++f) {
    put({0x09, 0x22, 0xA1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03});
    put({0x75, 0x08, 0x95, 0x01, 0x09, 0x51, 0x25, 0x0F, 0x81, 0x02});
    put({0x05, 0x01, 0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0xB8, 0x04, 0x55, 0x0E, 0x65, 0x11, 0x81, 0x02});
    put({0x09, 0x31, 0x46, 0x20, 0x03, 0x81, 0x02});
    put({0x05, 0x0D, 0xC0});
  }
  put({0x55, 0x00, 0x65, 0x00, 0x75, 0x10, 0x95, 0x01, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x09, 0x56, 0x81, 0x02});          // scan time
  put({0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02});                                                                  // contact count
  put({0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03});                              // button 1, padding
  put({0xC0});
  return n;
}

struct Touch { bool tip; bool confident; uint8_t id; uint16_t x, y; };

// one report: id 1, `fingers` slots, scan time, count, button
static uint32_t make_report(uint8_t* r, uint32_t fingers, const Touch* touches, uint32_t touch_count, uint32_t contacts, uint16_t scan, bool button) {
  uint32_t n = 0;
  r[n++] = 1;
  for (uint32_t f = 0; f < fingers; ++f) {
    Touch t = f < touch_count ? touches[f] : Touch{false, true, 0, 0, 0};
    r[n++] = static_cast<uint8_t>((t.confident ? 1 : 0) | (t.tip ? 2 : 0));
    r[n++] = t.id;
    r[n++] = static_cast<uint8_t>(t.x);
    r[n++] = static_cast<uint8_t>(t.x >> 8);
    r[n++] = static_cast<uint8_t>(t.y);
    r[n++] = static_cast<uint8_t>(t.y >> 8);
  }
  r[n++] = static_cast<uint8_t>(scan);
  r[n++] = static_cast<uint8_t>(scan >> 8);
  r[n++] = static_cast<uint8_t>(contacts);
  r[n++] = button ? 1 : 0;
  return n;
}

struct FakeClock final : IClock {
  uint64_t us = 1000000;
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return us; }
};

// ---- the tests -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Echo final : Slave {
  uint16_t address = 0x2C;
  uint8_t written[64] = {};
  uint32_t written_len = 0, writes = 0, reads = 0;
  uint8_t counter = 0;
  bool acks(uint16_t a) override { return a == address; }
  void write(ConstByteView b) override { ++writes; written_len = static_cast<uint32_t>(b.size()); memcpy(written, b.data(), b.size() < 64 ? b.size() : 64); counter = 0x10; }
  uint8_t read() override { ++reads; return counter++; }
  void stop() override {}
};

static void test_designware() {
  FakeDw dw;
  Echo echo;
  dw.slave = &echo;
  DesignWareI2c bus(dw);
  // not a DesignWare controller
  dw.comp_type = 0x12345678;
  CHECK(!bus.initialize(DesignWareI2c::Config()));
  dw.comp_type = dw_i2c::kCompTypeValue;
  // the clock counts of the kernel's formulas for the 120 MHz clock, and the hold time of Sunrise Point
  CHECK(DesignWareI2c::high_count(120000, 600, 300) == 105 && DesignWareI2c::low_count(120000, 1300, 300) == 191);
  CHECK(DesignWareI2c::high_count(120000, 4000, 300) == 513 && DesignWareI2c::low_count(120000, 4700, 300) == 599);
  CHECK(bus.initialize(DesignWareI2c::Config()) && bus.tx_depth() == 8 && bus.rx_depth() == 8);
  CHECK(dw.regs[dw_i2c::kFsHcnt / 4] == 105 && dw.regs[dw_i2c::kFsLcnt / 4] == 191 && dw.regs[dw_i2c::kSsHcnt / 4] == 513 && dw.regs[dw_i2c::kSsLcnt / 4] == 599);
  CHECK(dw.regs[dw_i2c::kSdaHold / 4] == 28);
  CHECK(dw.regs[dw_i2c::kCon / 4] == (dw_i2c::kConMaster | dw_i2c::kConSlaveDisable | dw_i2c::kConRestartEnable | dw_i2c::kConTxEmptyCtrl | dw_i2c::kConSpeedFast));
  DesignWareI2c::Config slow;
  slow.bus_hz = 100000;
  slow.fs_hcnt = 77;
  CHECK(bus.initialize(slow) && (dw.regs[dw_i2c::kCon / 4] & 6) == dw_i2c::kConSpeedStandard && dw.regs[dw_i2c::kFsHcnt / 4] == 77);
  CHECK(!bus.initialize([] { DesignWareI2c::Config c; c.bus_hz = 123; return c; }()));
  CHECK(bus.initialize(DesignWareI2c::Config()));

  // a write, a read, a write then a read (the restart), longer than the FIFOs
  const uint8_t w[3] = {1, 2, 3};
  CHECK(bus.transfer(0x2C, ConstByteView(w, 3), ByteView()) && echo.written_len == 3 && echo.written[2] == 3 && dw.tar == 0x2C);
  uint8_t r[40];
  CHECK(bus.transfer(0x2C, ConstByteView(), ByteView(r, 5)) && r[0] == 0x10 && r[4] == 0x14);
  memset(r, 0, sizeof r);
  CHECK(bus.transfer(0x2C, ConstByteView(w, 2), ByteView(r, 40)) && echo.written_len == 2 && r[0] == 0x10 && r[39] == 0x10 + 39);
  CHECK(!dw.rx_overflow && !dw.protocol_error);
  uint8_t big[40];
  for (int i = 0; i < 40; ++i) big[i] = static_cast<uint8_t>(i);
  CHECK(bus.transfer(0x2C, ConstByteView(big, 40), ByteView()) && echo.written_len == 40 && echo.written[39] == 39 && !dw.protocol_error);
  // a device that does not answer: the transfer fails and the abort source says why
  CHECK(!bus.transfer(0x2D, ConstByteView(w, 1), ByteView()) && bus.last_abort_source() == 1);
  CHECK(bus.transfer(0x2C, ConstByteView(w, 1), ByteView()));                   // (the controller is usable again)
  // refused calls
  CHECK(!bus.transfer(0x80, ConstByteView(w, 1), ByteView()) && !bus.transfer(0x2C, ConstByteView(), ByteView()) && !bus.transfer(0x2C, ConstByteView(w, 1), ByteView(r, 5000)));
}

static void test_hid_device() {
  FakeDw dw;
  FakeHidDevice dev;
  uint8_t desc[512];
  const uint32_t desc_len = make_digitizer_descriptor(desc, 2, false);
  dev.report_descriptor = desc;
  dev.report_descriptor_size = desc_len;
  dw.slave = &dev;
  DesignWareI2c bus(dw);
  CHECK(bus.initialize(DesignWareI2c::Config()));
  TestAllocator alloc;
  I2cHidDevice hid(alloc, bus, dev.address, dev.desc_reg);
  CHECK(hid.initialize());
  const I2cHidDescriptor& d = hid.descriptor();
  CHECK(d.length == 30 && d.version == 0x0100 && d.report_descriptor_length == desc_len && d.input_register == 0x0022 && d.max_input_length == 40 && d.command_register == 0x0024 &&
        d.vendor == 0x04F3 && d.product == 0x30A1);
  CHECK(hid.report_descriptor().size() == desc_len && memcmp(hid.report_descriptor().data(), desc, desc_len) == 0);
  // the power-on command (opcode 8, state 0) then the reset (opcode 1)
  CHECK(dev.commands == 2 && dev.last_commands[0][0] == 0x24 && dev.last_commands[0][1] == 0x00 && dev.last_commands[0][2] == 0x00 && dev.last_commands[0][3] == 0x08);
  CHECK(dev.last_commands[1][3] == 0x01 && dev.power_on);
  CHECK(dev.pending == 0);                                      // the reset's answer was read
  // input reports: none, one, the length prefix is dropped
  uint8_t report[64];
  uint32_t length = 99;
  CHECK(hid.read_input(ByteView(report, sizeof report), &length) && length == 0);
  const uint8_t sample[5] = {1, 2, 3, 4, 5};
  dev.push_report(sample, 5);
  CHECK(hid.read_input(ByteView(report, sizeof report), &length) && length == 5 && memcmp(report, sample, 5) == 0 && dev.pending == 0);
  CHECK(!hid.read_input(ByteView(report, 2), &length) || length == 0);          // (a report that does not fit is refused when there is one)
  dev.push_report(sample, 5);
  CHECK(!hid.read_input(ByteView(report, 2), &length) && length == 0);
  // power off, the output register
  CHECK(hid.set_power(false) && !dev.power_on);
  CHECK(hid.send_output(7, ConstByteView(sample, 3)) && dev.output_len >= 3);
  // refused: a wrong version, no device at the address
  FakeHidDevice bad;
  bad.report_descriptor = desc;
  bad.report_descriptor_size = desc_len;
  bad.version = 0x0200;
  dw.slave = &bad;
  I2cHidDevice hid2(alloc, bus, bad.address, bad.desc_reg);
  CHECK(!hid2.initialize());
  I2cHidDevice hid3(alloc, bus, 0x10, dev.desc_reg);
  dw.slave = &dev;
  CHECK(!hid3.initialize());
}

static void test_input_map() {
  TestAllocator alloc;
  uint8_t desc[512];
  const uint32_t n = make_digitizer_descriptor(desc, 2, false);
  HidInputMap map(alloc);
  CHECK(map.parse(ConstByteView(desc, n)));
  CHECK(map.has_application(hid_usage::kPageDigitizer, hid_usage::kTouchPad) && map.application_count() == 1 && map.uses_report_ids() && map.slots(1) == 2);
  // per finger confidence (bit 8), tip (bit 9), contact id (byte 2), X (bytes 3-4), Y (bytes 5-6); then the scan time, count and button
  const HidInputField* tip1 = map.find(1, hid_usage::kPageDigitizer, hid_usage::kTipSwitch, 1);
  const HidInputField* x1 = map.find(1, hid_usage::kPageGenericDesktop, hid_usage::kX, 1);
  const HidInputField* y0 = map.find(1, hid_usage::kPageGenericDesktop, hid_usage::kY, 0);
  const HidInputField* count = map.find(1, hid_usage::kPageDigitizer, hid_usage::kContactCount, -1);
  const HidInputField* button = map.find(1, hid_usage::kPageButton, 1, -1);
  CHECK(tip1 && x1 && y0 && count && button);
  if (tip1 && x1 && y0 && count && button) {
    CHECK(tip1->bit_offset == 8 + 48 + 1 && tip1->bit_size == 1 && x1->bit_offset == 8 + 48 + 16 && x1->bit_size == 16 && y0->bit_offset == 8 + 32 && x1->logical_max == 4095);
    CHECK(x1->physical_max == 1208 && x1->unit == 0x11 && x1->unit_exponent == -2);
    CHECK(count->bit_offset == 8 + 96 + 16 && button->bit_offset == 8 + 96 + 16 + 8);
  }
  CHECK(map.report_bytes(1) == 1 + 12 + 2 + 1 + 1);
  // unbalanced collections, a truncated item, a pop with nothing pushed, a report id of 0
  CHECK(!map.parse(ConstByteView(desc, n - 1)));
  const uint8_t bad1[] = {0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01};
  const uint8_t bad2[] = {0x05};
  const uint8_t bad3[] = {0xB4};
  const uint8_t bad4[] = {0x85, 0x00};
  CHECK(!map.parse(ConstByteView(bad1, sizeof bad1)) && !map.parse(ConstByteView(bad2, sizeof bad2)) && !map.parse(ConstByteView(bad3, sizeof bad3)) && !map.parse(ConstByteView(bad4, sizeof bad4)));
  // every prefix and every flipped bit: a verdict, no crash
  uint32_t accepted = 0;
  for (uint32_t cut = 0; cut <= n; ++cut) accepted += map.parse(ConstByteView(desc, cut)) ? 1 : 0;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t bit = 0; bit < 8; ++bit) {
      uint8_t copy[512];
      memcpy(copy, desc, n);
      copy[i] ^= static_cast<uint8_t>(1u << bit);
      accepted += map.parse(ConstByteView(copy, n)) ? 1 : 0;
    }
  CHECK(accepted > 0);
  // a long descriptor of nothing but fields stops at the bounds
  uint8_t flood[4096];
  uint32_t f = 0;
  flood[f++] = 0x05; flood[f++] = 0x01; flood[f++] = 0x09; flood[f++] = 0x02; flood[f++] = 0xA1; flood[f++] = 0x01;
  while (f + 8 < sizeof flood) { flood[f++] = 0x75; flood[f++] = 0x01; flood[f++] = 0x95; flood[f++] = 0x08; flood[f++] = 0x81; flood[f++] = 0x02; }
  flood[f++] = 0xC0;
  CHECK(!map.parse(ConstByteView(flood, f)));
}

// A pad of one or two finger slots on the bus, and the pointer on it
struct Rig {
  TestAllocator alloc;
  FakeDw dw;
  FakeHidDevice dev;
  FakeClock clock;
  DesignWareI2c bus{dw};
  uint8_t desc[512];
  uint32_t desc_len = 0;
  uint32_t fingers;
  I2cHidPointer* pointer = nullptr;
  uint16_t scan = 0;

  Rig(uint32_t finger_slots, bool screen, bool attention = true) : fingers(finger_slots) {
    desc_len = make_digitizer_descriptor(desc, finger_slots, screen);
    dev.report_descriptor = desc;
    dev.report_descriptor_size = desc_len;
    dev.max_input = 40;
    dw.slave = &dev;
    bus.initialize(DesignWareI2c::Config());
    pointer = new I2cHidPointer(alloc, bus, dev.address, dev.desc_reg, clock, "test pad");
    if (attention) pointer->set_attention(&dev);
  }
  ~Rig() { delete pointer; }
  void send(const Touch* t, uint32_t n, uint32_t contacts, bool button = false) {
    uint8_t r[64];
    const uint32_t len = make_report(r, fingers, t, n, contacts, scan += 100, button);
    dev.push_report(r, len);
  }
  // all the events the pointer has for what was sent
  uint32_t drain(MouseEvent* out, uint32_t capacity) {
    uint32_t n = 0;
    while (n < capacity && pointer->poll(&out[n])) ++n;
    return n;
  }
};

static int32_t sum_dx(const MouseEvent* e, uint32_t n) { int32_t s = 0; for (uint32_t i = 0; i < n; ++i) s += e[i].dx; return s; }
static int32_t sum_dy(const MouseEvent* e, uint32_t n) { int32_t s = 0; for (uint32_t i = 0; i < n; ++i) s += e[i].dy; return s; }
static int32_t sum_wheel(const MouseEvent* e, uint32_t n) { int32_t s = 0; for (uint32_t i = 0; i < n; ++i) s += e[i].wheel; return s; }

static void test_touchpad() {
  MouseEvent ev[64];
  {
    Rig rig(2, false);
    CHECK(rig.pointer->initialize() && rig.pointer->kind() == I2cHidPointer::Kind::Touchpad && !rig.pointer->is_absolute());
    // nothing to read while the attention line is quiet
    const uint32_t reads = rig.dev.input_reads;
    CHECK(rig.drain(ev, 64) == 0 && rig.dev.input_reads == reads);
    // one finger moves 100 units (2.95 mm at 120.8 mm over 4096 units): 47 counts (16 to the millimetre); the first frame only lands
    Touch t = {true, true, 1, 1000, 1000};
    rig.send(&t, 1, 1);
    uint32_t n = rig.drain(ev, 64);
    CHECK(n == 0);
    for (int i = 0; i < 10; ++i) {
      t.x += 10;
      rig.send(&t, 1, 1);
    }
    n = rig.drain(ev, 64);
    const int32_t dx = sum_dx(ev, n);
    CHECK(n > 0 && dx >= 46 && dx <= 48 && sum_dy(ev, n) == 0 && !ev[0].absolute);
    // a second finger joins and both move down 12 mm (4096 / 80 mm: 12 mm is 614 units): two notches of scroll, the pointer does not move
    Touch two[2] = {{true, true, 1, t.x, 1000}, {true, true, 2, 2000, 1000}};
    rig.send(two, 2, 2);
    rig.drain(ev, 64);
    for (int i = 0; i < 6; ++i) {
      two[0].y += 103;
      two[1].y += 103;
      rig.send(two, 2, 2);
    }
    n = rig.drain(ev, 64);
    CHECK(sum_wheel(ev, n) == 2 && sum_dx(ev, n) == 0 && sum_dy(ev, n) == 0);
    // lifting
    rig.send(nullptr, 0, 0);
    rig.drain(ev, 64);
  }
  {
    // a tap: 100 ms, a hair of movement: left down then up; a two-finger tap is the right button; a long touch is no tap
    Rig rig(2, false);
    CHECK(rig.pointer->initialize());
    Touch t = {true, true, 1, 2000, 2000};
    rig.send(&t, 1, 1);
    rig.drain(ev, 64);
    rig.clock.us += 100000;
    t.x += 4;
    rig.send(&t, 1, 1);
    rig.send(nullptr, 0, 0);
    uint32_t n = rig.drain(ev, 64);
    CHECK(n >= 2 && ev[n - 2].buttons == mouse_button::kLeft && ev[n - 1].buttons == 0);
    Touch two[2] = {{true, true, 1, 1000, 1000}, {true, true, 2, 1400, 1000}};
    rig.send(two, 2, 2);
    rig.drain(ev, 64);
    rig.clock.us += 80000;
    rig.send(nullptr, 0, 0);
    n = rig.drain(ev, 64);
    CHECK(n == 2 && ev[0].buttons == mouse_button::kRight && ev[1].buttons == 0);
    t = {true, true, 5, 2000, 2000};
    rig.send(&t, 1, 1);
    rig.drain(ev, 64);
    rig.clock.us += 600000;
    rig.send(nullptr, 0, 0);
    CHECK(rig.drain(ev, 64) == 0);
    // moving a lot is no tap either
    rig.send(&t, 1, 1);
    rig.drain(ev, 64);
    rig.clock.us += 50000;
    t.x += 300;
    rig.send(&t, 1, 1);
    rig.send(nullptr, 0, 0);
    n = rig.drain(ev, 64);
    CHECK(n > 0 && ev[n - 1].buttons == 0 && sum_dx(ev, n) > 100);
    bool clicked = false;
    for (uint32_t i = 0; i < n; ++i) clicked |= ev[i].buttons != 0;
    CHECK(!clicked);
  }
  {
    // the pad's button: left, or right in the bottom right; the button stays what it was at the press
    Rig rig(2, false);
    CHECK(rig.pointer->initialize());
    Touch t = {true, true, 1, 1000, 500};
    rig.send(&t, 1, 1, true);
    uint32_t n = rig.drain(ev, 64);
    CHECK(n == 1 && ev[0].buttons == mouse_button::kLeft);
    t.x = 3800;
    t.y = 3500;
    rig.send(&t, 1, 1, true);
    rig.drain(ev, 64);
    rig.send(&t, 1, 1, false);
    n = rig.drain(ev, 64);
    CHECK(n == 1 && ev[0].buttons == 0);
    rig.send(nullptr, 0, 0);
    rig.drain(ev, 64);
    t = {true, true, 2, 3800, 3500};
    rig.send(&t, 1, 1, true);
    n = rig.drain(ev, 64);
    CHECK(n == 1 && ev[0].buttons == mouse_button::kRight);
    // a palm (not confident) is not a finger
    rig.send(nullptr, 0, 0, false);
    rig.drain(ev, 64);
    Touch palm = {true, false, 3, 1000, 1000};
    for (int i = 0; i < 5; ++i) { palm.x += 50; rig.send(&palm, 1, 1); }
    CHECK(rig.drain(ev, 64) == 0);
  }
  {
    // a pad that reports one contact at a time (hybrid): the count in the first report says two, the second report holds the second contact
    Rig rig(1, false);
    CHECK(rig.pointer->initialize() && rig.pointer->kind() == I2cHidPointer::Kind::Touchpad);
    uint8_t r[64];
    Touch a = {true, true, 1, 1000, 1000}, b = {true, true, 2, 2000, 1000};
    for (int step = 0; step < 8; ++step) {
      uint32_t len = make_report(r, 1, &a, 1, 2, rig.scan += 100, false);
      rig.dev.push_report(r, len);
      len = make_report(r, 1, &b, 1, 0, rig.scan, false);
      rig.dev.push_report(r, len);
      a.y += 103;
      b.y += 103;
    }
    const uint32_t n = rig.drain(ev, 64);
    CHECK(sum_wheel(ev, n) >= 1 && sum_dx(ev, n) == 0);
  }
  {
    // no attention line: read at most every 8 ms
    Rig rig(2, false, false);
    CHECK(rig.pointer->initialize());
    const uint32_t reads = rig.dev.input_reads;
    Touch t = {true, true, 1, 100, 100};
    rig.send(&t, 1, 1);
    MouseEvent e;
    rig.pointer->poll(&e);
    const uint32_t after_first = rig.dev.input_reads;
    rig.pointer->poll(&e);
    CHECK(after_first > reads && rig.dev.input_reads == after_first);
    rig.clock.us += 9000;
    rig.pointer->poll(&e);
    CHECK(rig.dev.input_reads > after_first);
  }
}

static void test_touchscreen() {
  Rig rig(2, true);
  MouseEvent ev[16];
  CHECK(rig.pointer->initialize() && rig.pointer->kind() == I2cHidPointer::Kind::Touchscreen && rig.pointer->is_absolute() && rig.pointer->max_x() == 4095 && rig.pointer->max_y() == 4095);
  Touch t = {true, true, 3, 2048, 1024};
  rig.send(&t, 1, 1);
  uint32_t n = rig.drain(ev, 16);
  CHECK(n == 1 && ev[0].absolute && ev[0].x == 2048 && ev[0].y == 1024 && ev[0].buttons == mouse_button::kLeft && ev[0].max_x == 4095);
  rig.send(&t, 1, 1);                                           // the same place again: nothing new
  CHECK(rig.drain(ev, 16) == 0);
  t.x = 3000;
  rig.send(&t, 1, 1);
  n = rig.drain(ev, 16);
  CHECK(n == 1 && ev[0].x == 3000 && ev[0].buttons == mouse_button::kLeft);
  rig.send(nullptr, 0, 0);
  n = rig.drain(ev, 16);
  CHECK(n == 1 && ev[0].buttons == 0 && ev[0].x == 3000);
  // two fingers: the one with the lowest id is the pointer
  Touch two[2] = {{true, true, 7, 100, 100}, {true, true, 4, 900, 900}};
  rig.send(two, 2, 2);
  n = rig.drain(ev, 16);
  CHECK(n == 1 && ev[0].x == 900 && ev[0].y == 900);
}

static void test_mouse_mode() {
  TestAllocator alloc;
  FakeDw dw;
  FakeHidDevice dev;
  FakeClock clock;
  // a boot mouse: three buttons, X and Y as signed bytes
  const uint8_t mouse[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
                           0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
  dev.report_descriptor = mouse;
  dev.report_descriptor_size = sizeof mouse;
  dw.slave = &dev;
  DesignWareI2c bus(dw);
  CHECK(bus.initialize(DesignWareI2c::Config()));
  I2cHidPointer pointer(alloc, bus, dev.address, dev.desc_reg, clock, "mouse");
  pointer.set_attention(&dev);
  CHECK(pointer.initialize() && pointer.kind() == I2cHidPointer::Kind::Mouse);
  const uint8_t report[3] = {0x05, 0xFE, 0x03};                 // left and middle, dx -2, dy 3
  dev.push_report(report, 3);
  MouseEvent e;
  CHECK(pointer.poll(&e) && e.dx == -2 && e.dy == 3 && e.buttons == (mouse_button::kLeft | mouse_button::kMiddle) && !e.absolute);
  CHECK(!pointer.poll(&e));
  // a device that is neither: the descriptor of a keyboard
  const uint8_t keyboard[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
  FakeHidDevice kb;
  kb.report_descriptor = keyboard;
  kb.report_descriptor_size = sizeof keyboard;
  dw.slave = &kb;
  I2cHidPointer other(alloc, bus, kb.address, kb.desc_reg, clock, "keyboard");
  CHECK(!other.initialize());
}

static void test_failures_and_damage() {
  Rig rig(2, false);
  MouseEvent ev[16];
  CHECK(rig.pointer->initialize());
  // reads that fail (the device stops answering): no event, no crash, and the pointer waits before it tries again
  rig.dev.pending = 1;
  rig.dev.first = 0;
  rig.dev.report_len[0] = 4;
  rig.dev.address = 0x55;                                       // the bus no longer reaches it
  for (int i = 0; i < 6; ++i) CHECK(!rig.pointer->poll(&ev[0]));
  rig.dev.address = 0x2C;
  rig.clock.us += 200000;
  // damaged reports: truncated, the wrong id, a count larger than the slots
  uint8_t r[64];
  Touch t = {true, true, 1, 1000, 1000};
  const uint32_t len = make_report(r, 2, &t, 1, 3, 100, false);
  rig.dev.pending = 0;
  for (uint32_t cut = 0; cut <= len; ++cut) {
    rig.dev.push_report(r, cut);
    rig.drain(ev, 16);
    rig.clock.us += 20000;
  }
  for (uint32_t i = 0; i < len; ++i)
    for (uint32_t bit = 0; bit < 8; ++bit) {
      uint8_t copy[64];
      memcpy(copy, r, len);
      copy[i] ^= static_cast<uint8_t>(1u << bit);
      rig.dev.push_report(copy, len);
      rig.drain(ev, 16);
    }
  // random report descriptors: the pointer refuses or accepts, nothing else
  TestAllocator alloc;
  uint32_t seed = 12345;
  uint8_t desc[128];
  for (int round = 0; round < 300; ++round) {
    const uint32_t size = 4 + (seed >> 8) % 100;
    for (uint32_t i = 0; i < size; ++i) { seed = seed * 1664525u + 1013904223u; desc[i] = static_cast<uint8_t>(seed >> 24); }
    FakeDw dw;
    FakeHidDevice dev;
    FakeClock clock;
    dev.report_descriptor = desc;
    dev.report_descriptor_size = size;
    dw.slave = &dev;
    DesignWareI2c bus(dw);
    bus.initialize(DesignWareI2c::Config());
    I2cHidPointer p(alloc, bus, dev.address, dev.desc_reg, clock, "random");
    p.set_attention(&dev);
    (void)p.initialize();
  }
}

// ---- the resource descriptors ---------------------------------------------------------------------------------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;
static Bytes cat(std::initializer_list<Bytes> parts) { Bytes r; for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes bytes(std::initializer_list<uint8_t> v) { return Bytes(v); }

static Bytes memory32(uint32_t base, uint32_t length) {
  return bytes({0x86, 0x09, 0x00, 0x01, static_cast<uint8_t>(base), static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base >> 16), static_cast<uint8_t>(base >> 24), static_cast<uint8_t>(length),
                static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 24)});
}
static Bytes i2c_connection(uint16_t address, uint32_t speed, const char* controller) {
  Bytes r = bytes({0x8E, 0, 0, 0x01, 0x00, 0x01, 0x02, 0x00, 0x00, 0x01, 0x06, 0x00, static_cast<uint8_t>(speed), static_cast<uint8_t>(speed >> 8), static_cast<uint8_t>(speed >> 16),
                   static_cast<uint8_t>(speed >> 24), static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)});
  while (*controller) r.push_back(static_cast<uint8_t>(*controller++));
  r.push_back(0);
  const uint32_t length = static_cast<uint32_t>(r.size()) - 3;
  r[1] = static_cast<uint8_t>(length);
  r[2] = static_cast<uint8_t>(length >> 8);
  return r;
}
static Bytes gpio_interrupt(uint16_t pin, bool active_low, const char* controller) {
  const uint32_t name_len = static_cast<uint32_t>(strlen(controller)) + 1;
  const uint32_t total = 25 + name_len;
  Bytes r(total, 0);
  r[0] = 0x8C;
  r[1] = static_cast<uint8_t>(total - 3);
  r[2] = static_cast<uint8_t>((total - 3) >> 8);
  r[3] = 1;                                                    // revision
  r[4] = 0;                                                    // an interrupt connection
  r[5] = 1;                                                    // consumer
  r[7] = static_cast<uint8_t>(active_low ? 0x02 : 0x00);       // level triggered, polarity
  r[9] = 1;                                                    // pull up
  r[14] = 23;                                                  // pin table
  r[17] = 25;                                                  // resource source name
  r[19] = static_cast<uint8_t>(total);
  r[23] = static_cast<uint8_t>(pin);
  r[24] = static_cast<uint8_t>(pin >> 8);
  memcpy(&r[25], controller, name_len);
  return r;
}
static const Bytes kEnd = bytes({0x79, 0x00});

static void test_resources() {
  AcpiResources r;
  const Bytes crs = cat({i2c_connection(0x2C, 400000, "\\_SB.PCI0.I2C0"), gpio_interrupt(0x3C, true, "\\_SB.PCI0.GPI0"), memory32(0xFE000000, 0x1000), kEnd});
  CHECK(parse_acpi_resources(ConstByteView(crs.data(), crs.size()), &r));
  CHECK(r.i2c.present && r.i2c.address == 0x2C && r.i2c.speed_hz == 400000 && !r.i2c.ten_bit && strcmp(r.i2c.controller, "\\_SB.PCI0.I2C0") == 0);
  CHECK(r.gpio_interrupt.present && r.gpio_interrupt.pin == 0x3C && r.gpio_interrupt.active_low && !r.gpio_interrupt.edge && strcmp(r.gpio_interrupt.controller, "\\_SB.PCI0.GPI0") == 0);
  CHECK(r.memory_count == 1 && r.memory[0].base == 0xFE000000u && r.memory[0].length == 0x1000);
  // an extended interrupt and a small item before; a descriptor we do not know is skipped
  const Bytes irq = bytes({0x89, 0x06, 0x00, 0x01, 0x01, 0x14, 0x00, 0x00, 0x00, 0x23, 0x01, 0x00, 0x00});
  const Bytes crs2 = cat({bytes({0x22, 0x04, 0x00}), irq, bytes({0x87, 0x03, 0x00, 1, 2, 3}), memory32(0x1000, 0x10), kEnd});
  CHECK(parse_acpi_resources(ConstByteView(crs2.data(), crs2.size()), &r) && r.interrupt_count == 1 && r.interrupts[0] == 0x14 && r.memory_count == 1);
  // truncated, and every flipped bit: a verdict, no crash
  uint32_t accepted = 0;
  for (size_t cut = 0; cut <= crs.size(); ++cut) accepted += parse_acpi_resources(ConstByteView(crs.data(), cut), &r) ? 1 : 0;
  for (size_t i = 0; i < crs.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes copy = crs;
      copy[i] ^= static_cast<uint8_t>(1 << bit);
      accepted += parse_acpi_resources(ConstByteView(copy.data(), copy.size()), &r) ? 1 : 0;
    }
  CHECK(accepted > 0);
  // a resource source without an end inside the descriptor
  Bytes bad = i2c_connection(0x2C, 400000, "\\_SB.I2C0");
  bad.pop_back();
  bad[1] = static_cast<uint8_t>(bad.size() - 3);
  CHECK(!parse_acpi_resources(ConstByteView(bad.data(), bad.size()), &r));
}

// ---- the platform: AML tables, PCI, the pads ----------------------------------------------------------------------------------------------------------------------------------

static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes seg(const char* s) { Bytes r(4, '_'); for (int i = 0; i < 4 && s[i]; ++i) r[i] = static_cast<uint8_t>(s[i]); return r; }
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  std::vector<Bytes> segs;
  while (*s) {
    char t[5] = {};
    int n = 0;
    while (*s && *s != '.') t[n++] = *s++;
    segs.push_back(seg(t));
    if (*s == '.') ++s;
  }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
}
static Bytes str(const char* s) { Bytes r = b({0x0D}); while (*s) r.push_back(static_cast<uint8_t>(*s++)); r.push_back(0); return r; }
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes package(uint8_t count, const Bytes& elements) { return cat({b({0x12}), pkg(cat({b({count}), elements}))}); }
static Bytes buffer(const Bytes& init) { return cat({b({0x11}), pkg(cat({num(init.size()), init}))}); }
static Bytes make_table(const char* signature, const Bytes& aml) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = 2;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}

struct Options {
  bool touchpad_enabled = true;
  bool touchpad_has_dsm = true;
  bool wrong_controller = false;
  bool controller_not_lpss = false;
  const char* touchpad_cid = "PNP0C50";
  bool gpio = true;
};

// \_SB.PCI0 with the I2C0 controller (_ADR 00:15.0, with the timing methods), the GPIO controller INT344B (three memory ranges), the touchpad TPD0 at 0x2C (pin 60, active low) and
// the touchscreen TPL1 at 0x10 (pin 70) behind I2C0
static Bytes make_dsdt(const Options& o) {
  const Bytes ctrl = device("I2C0", cat({name_def("_ADR", num(0x00150000)), method("SSCN", 0, ret(package(3, cat({num(0x300), num(0x380), num(0x1E)})))),
                                         method("FMCN", 0, ret(package(3, cat({num(0x70), num(0xC0), num(0x1F)}))))}));
  const Bytes gpio = device("GPI0", cat({name_def("_HID", str("INT344B")), name_def("_CRS", buffer(cat({memory32(0xD0000000, 0x10000), memory32(0xD1000000, 0x10000), memory32(0xD2000000, 0x10000), kEnd})))}));
  const char* controller_path = o.wrong_controller ? "\\_SB.PCI0.I2C9" : "\\_SB.PCI0.I2C0";
  const Bytes tpd_crs = cat({i2c_connection(0x2C, 400000, controller_path), o.gpio ? gpio_interrupt(60, true, "\\_SB.PCI0.GPI0") : Bytes(), kEnd});
  const Bytes tpl_crs = cat({i2c_connection(0x10, 400000, "\\_SB.PCI0.I2C0"), gpio_interrupt(70, true, "\\_SB.PCI0.GPI0"), kEnd});
  Bytes tpd_body = cat({name_def("_HID", str("ELAN0501")), name_def("_CID", str(o.touchpad_cid)), method("_STA", 0, ret(num(o.touchpad_enabled ? 0x0F : 0x00))), name_def("_CRS", buffer(tpd_crs))});
  if (o.touchpad_has_dsm) tpd_body = cat({tpd_body, method("_DSM", 4, ret(num(0x20)))});
  const Bytes tpd = device("TPD0", tpd_body);
  const Bytes tpl = device("TPL1", cat({name_def("_HID", str("ELAN2097")), name_def("_CID", str("PNP0C50")), name_def("_CRS", buffer(tpl_crs)), method("_DSM", 4, ret(num(0x01)))}));
  // the ones that must be left alone: a device that is not HID over I2C, one on the blacklist
  const Bytes other = device("KBD0", cat({name_def("_HID", str("PNP0303"))}));
  const Bytes chipone = device("CHIP", cat({name_def("_HID", str("CHPN0001")), name_def("_CID", str("PNP0C50")), name_def("_CRS", buffer(tpl_crs)), method("_DSM", 4, ret(num(0x01)))}));
  const Bytes i2c0_with_children = device("I2C0", cat({name_def("_ADR", num(0x00150000)), method("SSCN", 0, ret(package(3, cat({num(0x300), num(0x380), num(0x1E)})))),
                                                        method("FMCN", 0, ret(package(3, cat({num(0x70), num(0xC0), num(0x1F)})))), tpd, tpl, chipone})); 
  (void)ctrl;
  return cat({scope("\\_SB", device("PCI0", cat({name_def("_HID", num(0x080AD041)), i2c0_with_children, gpio, other})))});
}

struct FakePci final : IPciBus {
  uint32_t config[64] = {};
  bool lpss = true;
  PciAddress controller = {0, 0x15, 0};
  uint32_t function_count = 1;
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  bool is_controller(PciAddress a) const { return a.bus == controller.bus && a.device == controller.device && a.function == controller.function; }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    if (!is_controller(a)) return 0xFFFFFFFFu;
    if (offset == 0) return lpss ? 0x9D608086u : 0x1234ABCDu;
    return config[(offset / 4) % 64];
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    if (is_controller(a)) const_cast<FakePci*>(this)->config[(offset / 4) % 64] = value;
  }
  uint64_t memory_bar(PciAddress a, uint8_t index) const override { return is_controller(a) && index == 0 ? 0xFE600000ull : 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override { return is_controller(a) && id == 1 ? 0x80 : 0; }
};

// the I2C controller's registers, and the slaves on its bus
struct Mux final : Slave {
  Slave* devices[4] = {};
  Slave* selected = nullptr;
  bool acks(uint16_t a) override {
    selected = nullptr;
    for (Slave* d : devices)
      if (d && d->acks(a)) { selected = d; return true; }
    return false;
  }
  void write(ConstByteView b2) override { if (selected) selected->write(b2); }
  uint8_t read() override { return selected ? selected->read() : 0; }
  void stop() override { if (selected) selected->stop(); }
};

struct FakeRegisterSource final : II2cRegisterSource {
  FakeDw* dw = nullptr;
  uint64_t last_bar = 0;
  uint32_t opens = 0;
  II2cRegisters* open(uint64_t bar) override { last_bar = bar; ++opens; return dw; }
};

alignas(16) static uint32_t g_gpio_memory[3][0x10000 / 4];

struct FakeMapper final : IRegisterMapper {
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    const uint64_t index = (physical >> 24) & 3;
    if (physical < 0xD0000000ull || index > 2 || length > 0x10000) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(g_gpio_memory[index]), length);
    return true;
  }
};

struct FakeAcpiHost final : IAcpiHost {
  bool region_read(const acpi::RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const acpi::RegionAccess&, uint64_t) override { return false; }
  void configure_ec(uint16_t, uint16_t) override {}
};

static void set_pad(uint32_t pin, bool high) {
  const uint32_t community = pin < 48 ? 0 : pin < 120 ? 1 : 2;
  const uint32_t base = community == 0 ? 0 : community == 1 ? 48 : 120;
  uint32_t& dw0 = g_gpio_memory[community][(0x400 + (pin - base) * 8) / 4];
  dw0 = (dw0 & ~2u) | (high ? 2u : 0u);
}

static void test_platform() {
  TestAllocator alloc;
  for (auto& c : g_gpio_memory) { for (uint32_t& w : c) w = 0; c[0x0C / 4] = 0x400; }       // PADBAR in each community
  set_pad(60, true);
  set_pad(70, true);
  FakePci pci;
  pci.config[0x84 / 4] = 3;                                                              // the function sleeps (D3hot)
  FakeDw dw;
  FakeHidDevice pad;                                                                      // 0x2C, descriptor at 0x20
  FakeHidDevice screen;
  screen.address = 0x10;
  screen.desc_reg = 0x0001;
  screen.report_desc_reg = 0x0002;
  screen.input_reg = 0x0003;
  screen.output_reg = 0x0004;
  screen.command_reg = 0x0005;
  screen.data_reg = 0x0006;
  uint8_t pad_desc[512], screen_desc[512];
  pad.report_descriptor_size = make_digitizer_descriptor(pad_desc, 2, false);
  pad.report_descriptor = pad_desc;
  screen.report_descriptor_size = make_digitizer_descriptor(screen_desc, 2, true);
  screen.report_descriptor = screen_desc;
  Mux mux;
  mux.devices[0] = &pad;
  mux.devices[1] = &screen;
  dw.slave = &mux;
  FakeRegisterSource source;
  source.dw = &dw;
  FakeMapper mapper;
  FakeClock clock;
  FakeAcpiHost host;
  const Bytes dsdt = make_table("DSDT", make_dsdt(Options()));
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  CompositeMouse mice;
  {
    PlatformI2cHid platform(alloc, pci, source, mapper, clock, host);
    CHECK(platform.start(tables, 1, mice) == 2 && platform.tables_loaded() == 1 && mice.count() == 2);
    CHECK(platform.report_count() == 2);
    for (uint32_t i = 0; i < platform.report_count(); ++i)
      if (platform.report(i).outcome != PlatformI2cHid::Outcome::Started) printf("  %s: %s\n", platform.report(i).device, PlatformI2cHid::outcome_name(platform.report(i).outcome));
    CHECK(platform.report(0).outcome == PlatformI2cHid::Outcome::Started && platform.report(0).kind == I2cHidPointer::Kind::Touchpad && strcmp(platform.report(0).device, "TPD0 ELAN0501") == 0);
    CHECK(platform.report(1).outcome == PlatformI2cHid::Outcome::Started && platform.report(1).kind == I2cHidPointer::Kind::Touchscreen && strcmp(platform.report(1).device, "TPL1 ELAN2097") == 0);
    // the controller: opened once, at the BAR, out of reset, in D0 with memory decoding, the timing methods' counts and the fast speed
    CHECK(source.opens == 1 && source.last_bar == 0xFE600000ull);
    CHECK(dw.regs[0x204 / 4] == 3 && pci.config[0x84 / 4] == 0 && (pci.config[1] & 2) == 2);
    CHECK(dw.regs[dw_i2c::kFsHcnt / 4] == 0x70 && dw.regs[dw_i2c::kFsLcnt / 4] == 0xC0 && dw.regs[dw_i2c::kSsHcnt / 4] == 0x300 && dw.regs[dw_i2c::kSdaHold / 4] == 0x1F);
    CHECK(pad.last_commands[0][3] == 0x08 && pad.report_descriptor_size > 0);
    // reports reach the mouse only when the pin is low
    MouseEvent e;
    uint8_t r[64];
    Touch t = {true, true, 1, 2048, 1024};
    const uint32_t len = make_report(r, 2, &t, 1, 1, 100, false);
    pad.push_report(r, len);
    const uint32_t reads = pad.input_reads;
    CHECK(!mice.poll(&e) && pad.input_reads == reads);                                    // the pin is high: nothing is read
    set_pad(60, false);
    for (int i = 0; i < 3; ++i) mice.poll(&e);
    CHECK(pad.input_reads > reads && pad.pending == 0);
    set_pad(60, true);
    // the touchscreen on its own pin
    t = {true, true, 9, 1000, 3000};
    const uint32_t len2 = make_report(r, 2, &t, 1, 1, 100, false);
    screen.push_report(r, len2);
    set_pad(70, false);
    bool got = false;
    for (int i = 0; i < 4 && !got; ++i) got = mice.poll(&e);
    CHECK(got && e.absolute && e.x == 1000 && e.y == 3000 && e.buttons == mouse_button::kLeft);
    set_pad(70, true);
    platform.stop(mice);
    CHECK(mice.count() == 0);
  }
  // variations
  auto run = [&](const Options& o, PlatformI2cHid::Outcome* first_outcome, uint32_t* reports) -> uint32_t {
    for (auto& c : g_gpio_memory) { for (uint32_t& w : c) w = 0; c[0x0C / 4] = 0x400; }
    set_pad(60, true);
    set_pad(70, true);
    dw = FakeDw();
    dw.slave = &mux;
    source.opens = 0;
    pad = FakeHidDevice();
    pad.report_descriptor_size = make_digitizer_descriptor(pad_desc, 2, false);
    pad.report_descriptor = pad_desc;
    mux.devices[0] = &pad;
    const Bytes table = make_table("DSDT", make_dsdt(o));
    const ConstByteView one[1] = {ConstByteView(table.data(), table.size())};
    CompositeMouse m;
    PlatformI2cHid p(alloc, pci, source, mapper, clock, host);
    const uint32_t started = p.start(one, 1, m);
    *reports = p.report_count();
    if (p.report_count() > 0) *first_outcome = p.report(0).outcome;
    p.stop(m);
    return started;
  };
  PlatformI2cHid::Outcome outcome = PlatformI2cHid::Outcome::Started;
  uint32_t reports = 0;
  Options o;
  o.touchpad_enabled = false;                                   // _STA says the device is not there: no report for it, the touchscreen still starts
  CHECK(run(o, &outcome, &reports) == 1 && reports == 1);
  o = Options();
  o.touchpad_has_dsm = false;
  CHECK(run(o, &outcome, &reports) == 1 && outcome == PlatformI2cHid::Outcome::NoDescriptorAddress);
  o = Options();
  o.wrong_controller = true;
  CHECK(run(o, &outcome, &reports) == 1 && outcome == PlatformI2cHid::Outcome::NoController);
  o = Options();
  o.touchpad_cid = "PNP0303";                                   // not HID over I2C by its compatible ID, and no _HID of ours either: only the touchscreen is started
  CHECK(run(o, &outcome, &reports) == 1 && reports == 1);
  o = Options();
  o.gpio = false;                                               // no interrupt pin: polled, still started
  CHECK(run(o, &outcome, &reports) == 2);
  pci.lpss = false;                                             // the function at the controller's address is not an LPSS I2C controller
  o = Options();
  CHECK(run(o, &outcome, &reports) == 0 && outcome == PlatformI2cHid::Outcome::ControllerFailed);
  pci.lpss = true;
  // damaged tables never stop it
  Bytes good = make_table("DSDT", make_dsdt(Options()));
  uint32_t seed = 99;
  for (int round = 0; round < 60; ++round) {
    Bytes copy = good;
    for (int k = 0; k < 3; ++k) { seed = seed * 1664525u + 1013904223u; copy[36 + (seed >> 8) % (copy.size() - 36)] ^= static_cast<uint8_t>(1u << ((seed >> 4) & 7)); }
    const ConstByteView one[1] = {ConstByteView(copy.data(), copy.size())};
    CompositeMouse m;
    PlatformI2cHid p(alloc, pci, source, mapper, clock, host);
    (void)p.start(one, 1, m);
    p.stop(m);
  }
}

static void test_gpio() {
  for (auto& c : g_gpio_memory) { for (uint32_t& w : c) w = 0; c[0x0C / 4] = 0x400; }
  IntelPchGpio gpio;
  FakeMapper mapper;
  MmioRegion region;
  CHECK(mapper.map_registers(0xD1000000, 0x10000, &region) && gpio.add_community(region, 48, 72));
  CHECK(!gpio.add_community(MmioRegion(), 0, 10));
  bool high = false;
  CHECK(!gpio.level(10, &high) && !gpio.level(120, &high) && gpio.has_pin(48) && gpio.has_pin(119) && !gpio.has_pin(47));
  set_pad(50, true);
  CHECK(gpio.level(50, &high) && high);
  set_pad(50, false);
  CHECK(gpio.level(50, &high) && !high);
  GpioAttentionLine low_line(gpio, 50, true), high_line(gpio, 50, false), stray(gpio, 1, true);
  CHECK(low_line.asserted() && !high_line.asserted() && !stray.asserted());
  set_pad(50, true);
  CHECK(!low_line.asserted() && high_line.asserted());
  // a community whose PADBAR points outside the window is refused
  g_gpio_memory[2][0x0C / 4] = 0xFFFF0;
  MmioRegion bad;
  IntelPchGpio g2;
  CHECK(mapper.map_registers(0xD2000000, 0x10000, &bad) && !g2.add_community(bad, 120, 32));
  CHECK(IntelPchGpio::is_sunrise_point_lp("INT344B") && !IntelPchGpio::is_sunrise_point_lp("INT3451"));
}


// ---- a pen and sensors on one device ---------------------------------------------------------------------------------------------------------------------------------------------------

typedef std::vector<uint8_t> Desc;
static void D(Desc& d, std::initializer_list<uint8_t> v) { d.insert(d.end(), v.begin(), v.end()); }

// report 1: pen (flags: tip, barrel, invert, in range; X 0..20000, Y 0..10000, pressure 0..1023, tilt X / Y -90..90 as bytes); report 2: accelerometer (X, Y, Z as 16 bits, unit exponent -2) with its
// feature report 3 (reporting state and power state, each a selector); report 4: ambient light (16 bit lux) with feature report 5.
static Desc make_pen_sensor_descriptor() {
  Desc d;
  D(d, {0x05, 0x0D, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x20, 0xA1, 0x00});
  D(d, {0x09, 0x42, 0x09, 0x44, 0x09, 0x3C, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03});
  D(d, {0x05, 0x01, 0x09, 0x30, 0x16, 0x00, 0x00, 0x26, 0x20, 0x4E, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02});
  D(d, {0x09, 0x31, 0x16, 0x00, 0x00, 0x26, 0x10, 0x27, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02});
  D(d, {0x05, 0x0D, 0x09, 0x30, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02});
  D(d, {0x09, 0x3D, 0x15, 0xA6, 0x25, 0x5A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x09, 0x3E, 0x81, 0x02});
  D(d, {0xC0, 0xC0});
  for (int k = 0; k < 2; ++k) {                                                              // accelerometer, then light
    const bool accel = k == 0;
    D(d, {0x05, 0x20, 0x09, static_cast<uint8_t>(accel ? 0x73 : 0x41), 0xA1, 0x01, 0x85, static_cast<uint8_t>(accel ? 3 : 5)});
    D(d, {0x0A, 0x16, 0x03, 0xA1, 0x02, 0x0A, 0x40, 0x08, 0x0A, 0x41, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00, 0xC0});
    D(d, {0x0A, 0x19, 0x03, 0xA1, 0x02, 0x0A, 0x50, 0x08, 0x0A, 0x51, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00, 0xC0});
    D(d, {0x85, static_cast<uint8_t>(accel ? 2 : 4)});
    if (accel) D(d, {0x0A, 0x53, 0x04, 0x0A, 0x54, 0x04, 0x0A, 0x55, 0x04, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02});
    else D(d, {0x55, 0x00, 0x0A, 0xD1, 0x04, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02});
    D(d, {0xC0});
  }
  return d;
}

struct PenRig {
  TestAllocator alloc;
  FakeDw dw;
  FakeHidDevice dev;
  FakeClock clock;
  DesignWareI2c bus{dw};
  Desc desc;
  I2cHidPointer pointer;
  PenRig(const Desc& descriptor) : desc(descriptor), pointer(alloc, bus, 0x2C, 0x0020, clock, "test digitizer") {
    dev.report_descriptor = desc.data();
    dev.report_descriptor_size = static_cast<uint32_t>(desc.size());
    dev.max_input = 40;
    dw.slave = &dev;
    bus.initialize(DesignWareI2c::Config());
    pointer.set_attention(&dev);
  }
  void pen_report(uint8_t flags, uint16_t x, uint16_t y, uint16_t pressure, int8_t tx, int8_t ty, bool shorten = false) {
    const uint8_t r[10] = {1, flags, static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(pressure),
                           static_cast<uint8_t>(pressure >> 8), static_cast<uint8_t>(tx), static_cast<uint8_t>(ty)};
    dev.push_report(r, shorten ? 5 : 10);
  }
  void accel_report(int16_t x, int16_t y, int16_t z) {
    const uint8_t r[7] = {2, static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(z), static_cast<uint8_t>(z >> 8)};
    dev.push_report(r, 7);
  }
  void light_report(uint16_t lux) {
    const uint8_t r[3] = {4, static_cast<uint8_t>(lux), static_cast<uint8_t>(lux >> 8)};
    dev.push_report(r, 3);
  }
};

static void test_pen_and_sensors() {
  const Desc descriptor = make_pen_sensor_descriptor();
  {
    PenRig rig(descriptor);
    CHECK(rig.pointer.initialize() && rig.pointer.kind() == I2cHidPointer::Kind::Other && !rig.pointer.is_absolute());
    I2cHidPen pen(rig.pointer, "test pen");
    CHECK(pen.initialize() && pen.max_x() == 20000 && pen.max_y() == 10000 && pen.max_pressure() == 1023 && pen.has_tilt());
    PenEvent e;
    CHECK(!pen.poll(&e));
    rig.pen_report(0x09, 10000, 5000, 512, 45, -30);                                       // tip + in range
    CHECK(pen.poll(&e) && e.tip && e.in_range && !e.inverted && e.buttons == 0 && e.x == 10000 && e.y == 5000 && e.pressure == 512 && e.max_pressure == 1023 && e.tilt_x == 45 && e.tilt_y == -30 && e.max_x == 20000);
    rig.pen_report(0x0A, 100, 200, 0, 0, 0);                                                // hovering, barrel button
    CHECK(pen.poll(&e) && !e.tip && e.in_range && (e.buttons & pen_button::kBarrel) && e.x == 100 && e.y == 200);
    rig.pen_report(0x0D, 100, 200, 300, 0, 0);                                              // the eraser end touching
    CHECK(pen.poll(&e) && e.tip && e.inverted);
    rig.pen_report(0x00, 0, 0, 0, 0, 0);
    CHECK(pen.poll(&e) && !e.in_range && !e.tip);
    rig.pen_report(0x09, 1, 1, 1, 0, 0, true);                                              // cut short: the fields beyond read as 0, nothing outside the report
    pen.poll(&e);
    // the pen as a mouse
    CompositePen pens;
    PenMouse mouse(pens);
    CHECK(pens.add(&pen) && pens.count() == 1);
    MouseEvent m;
    CHECK(!mouse.poll(&m));
    rig.pen_report(0x08, 2000, 3000, 0, 0, 0);                                              // hover: the pointer moves, no button
    CHECK(mouse.poll(&m) && m.absolute && m.x == 2000 && m.y == 3000 && m.buttons == 0 && m.max_x == 20000 && mouse.max_x() == 20000);
    rig.pen_report(0x08, 2000, 3000, 0, 0, 0);                                              // nothing changed
    CHECK(!mouse.poll(&m));
    rig.pen_report(0x09, 2000, 3000, 400, 0, 0);
    CHECK(mouse.poll(&m) && m.buttons == mouse_button::kLeft);
    rig.pen_report(0x0B, 2100, 3000, 400, 0, 0);                                            // barrel button: right as well
    CHECK(mouse.poll(&m) && m.buttons == (mouse_button::kLeft | mouse_button::kRight) && m.x == 2100);
    rig.pen_report(0x00, 0, 0, 0, 0, 0);                                                    // out of range: the buttons are let go where the pen was
    CHECK(mouse.poll(&m) && m.buttons == 0 && m.x == 2100);
    CHECK(pens.last().in_range == false && pens.last().x == 0);
    CHECK(pens.remove(&pen) && pens.count() == 0 && !pens.remove(&pen));
    pen.shutdown();
  }
  {   // the sensors
    PenRig rig(descriptor);
    CHECK(rig.pointer.initialize());
    SensorKind kinds[HidSensor::kMaxPerDevice];
    uint8_t ids[HidSensor::kMaxPerDevice];
    CHECK(HidSensor::discover(rig.pointer.map(), kinds, ids) == 2 && kinds[0] == SensorKind::Accelerometer && ids[0] == 2 && kinds[1] == SensorKind::AmbientLight && ids[1] == 4);
    HidSensor accel(rig.pointer, SensorKind::Accelerometer, 2, "accelerometer"), light(rig.pointer, SensorKind::AmbientLight, 4, "light");
    SensorReading r;
    CHECK(accel.initialize() && accel.kind() == SensorKind::Accelerometer && !accel.read(&r));
    // the device was told to report all events with full power: the feature report of the accelerometer (id 3), selector indexes 1 and 1
    const uint8_t expected[] = {0x24, 0x00, 0x33, 0x03, 0x25, 0x00, 0x05, 0x00, 0x03, 0x01, 0x01};
    CHECK(rig.dev.features_set == 1 && rig.dev.feature_len == sizeof expected && memcmp(rig.dev.last_feature, expected, sizeof expected) == 0);
    CHECK(light.initialize() && rig.dev.features_set == 2 && rig.dev.last_feature[2] == 0x35 && rig.dev.last_feature[8] == 0x05 && rig.dev.last_feature[9] == 1 && rig.dev.last_feature[10] == 1);
    SensorHub hub;
    CHECK(hub.add(&accel) && hub.add(&light) && hub.add(&light) && hub.count() == 2 && hub.find(SensorKind::AmbientLight) == &light && hub.find(SensorKind::Gyroscope) == nullptr && hub.find(SensorKind::Accelerometer, 1) == nullptr);
    rig.accel_report(100, -50, 981);                                                       // exponent -2: 1.00 g, -0.50 g, 9.81 g
    CHECK(accel.read(&r) && r.kind == SensorKind::Accelerometer && r.x > 0.99f && r.x < 1.01f && r.y > -0.51f && r.y < -0.49f && r.z > 9.80f && r.z < 9.82f && r.sequence == 1);
    rig.light_report(300);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && r.x == 300.0f);
    rig.accel_report(0, 0, 100);
    CHECK(accel.read(&r) && r.sequence == 2 && r.z > 0.99f && r.z < 1.01f);
    CHECK(light.read(&r) && r.x == 300.0f && !hub.read(SensorKind::Magnetometer, &r));        // (the light report is still the latest)
    CHECK(hub.remove(&light) && hub.count() == 1 && !hub.remove(&light));
    accel.shutdown();
    light.shutdown();
  }
  {   // damaged descriptors: cut at every length and with every bit flipped
    TestAllocator a;
    SensorKind kinds[HidSensor::kMaxPerDevice];
    uint8_t ids[HidSensor::kMaxPerDevice];
    for (size_t n = 0; n <= descriptor.size(); ++n) {
      HidInputMap map(a);
      if (map.parse(ConstByteView(descriptor.data(), n))) HidSensor::discover(map, kinds, ids);
    }
    for (size_t bit = 0; bit < descriptor.size() * 8; ++bit) {
      Desc flipped = descriptor;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      HidInputMap map(a);
      if (map.parse(ConstByteView(flipped.data(), flipped.size()))) {
        HidSensor::discover(map, kinds, ids);
        for (const HidFeature& f : map.features()) (void)f.usage_count;
      }
    }
  }
  {   // reports of every length into the pen and the sensors
    PenRig rig(descriptor);
    CHECK(rig.pointer.initialize());
    I2cHidPen pen(rig.pointer, "p");
    HidSensor accel(rig.pointer, SensorKind::Accelerometer, 2, "a");
    CHECK(pen.initialize() && accel.initialize());
    uint8_t full[10] = {1, 0x09, 1, 2, 3, 4, 5, 6, 7, 8};
    for (uint32_t n = 0; n <= 10; ++n) { rig.dev.push_report(full, n); PenEvent e; pen.poll(&e); }
    uint8_t acc[7] = {2, 1, 0, 2, 0, 3, 0};
    for (uint32_t n = 0; n <= 7; ++n) { rig.dev.push_report(acc, n); SensorReading r; accel.read(&r); }
    pen.shutdown();
    accel.shutdown();
  }
}

int main() {
  test_designware();
  test_hid_device();
  test_input_map();
  test_touchpad();
  test_touchscreen();
  test_mouse_mode();
  test_failures_and_damage();
  test_resources();
  test_gpio();
  test_platform();
  test_pen_and_sensors();
  printf(g_failed ? "I2cHidTest: %d FAILED\n" : "I2cHidTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}

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

// Host-side tests for the Super I/O hardware monitors, against a model of the chips' port protocol: the configuration keys (0x87 0x87 for
// Nuvoton and Fintek, 0x87 0x01 0x55 0x55/AA for ITE), logical devices and their activate/base registers, the exit sequences, and the sensor
// window at base+5/+6 with Nuvoton's banks. Checks detection of each family (and of nothing, an unknown id, an unsupported NCT668x, a window
// the firmware did not set up), every channel kind with its scaling, PWM writes and returning to the firmware's curve, register garbage (bit
// flips, 0 and 0xFFFF counts) and several threads sharing one set behind a lock without splitting an index/data pair.
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "Hardware/HardwareMonitor/HardwareMonitorSet.hpp"
#include "Hardware/HardwareMonitor/SuperIo.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The model.

enum class Key { Nuvoton, Ite };   // (Fintek listens for Nuvoton's key)

struct ModelChip {
  uint16_t config_port = 0x2E;
  Key key = Key::Nuvoton;
  bool banked = false;                       // Nuvoton: index 0x4E selects the bank, 0x4F gives the vendor id
  uint8_t global[0x30] = {};                 // configuration registers 0x00..0x2F (ids at 0x20..0x24)
  uint8_t device[16][256] = {};              // per logical device, registers 0x30 and up
  uint8_t sensors[16][256] = {};             // sensor window registers, by bank (bank 0 only when not banked)
  // Protocol state.
  uint32_t key_progress = 0;
  bool in_config = false;
  uint8_t config_index = 0;
  uint8_t logical_device = 0;
  uint8_t sensor_index = 0;
  uint8_t bank = 0;
  bool high_byte_vendor = false;
  uint32_t exits = 0;

  void set_id(uint16_t id) { global[0x20] = static_cast<uint8_t>(id >> 8); global[0x21] = static_cast<uint8_t>(id); }
  void set_vendor(uint16_t id) { global[0x23] = static_cast<uint8_t>(id >> 8); global[0x24] = static_cast<uint8_t>(id); }
  void set_device(uint8_t ld, bool active, uint16_t base) {
    device[ld][0x30] = active ? 1 : 0;
    device[ld][0x60] = static_cast<uint8_t>(base >> 8);
    device[ld][0x61] = static_cast<uint8_t>(base);
  }
  uint16_t sensor_base() const {
    const uint8_t ld = key == Key::Ite ? 0x04 : (banked ? 0x0B : 0x04);
    return static_cast<uint16_t>((device[ld][0x60] << 8 | device[ld][0x61]) & ~7);
  }
  // A Nuvoton register by its 16-bit number.
  uint8_t& nct(uint16_t reg) { return sensors[(reg >> 8) & 0x0F][reg & 0xFF]; }
  void nct16(uint16_t reg, uint16_t value) { nct(reg) = static_cast<uint8_t>(value >> 8); nct(static_cast<uint16_t>(reg + 1)) = static_cast<uint8_t>(value); }
  uint8_t& flat(uint8_t reg) { return sensors[0][reg]; }

  void key_write(uint8_t value) {
    static constexpr uint8_t kNuvoton[] = {0x87, 0x87};
    const uint8_t ite[] = {0x87, 0x01, 0x55, static_cast<uint8_t>(config_port == 0x4E ? 0xAA : 0x55)};
    const uint8_t* sequence = key == Key::Ite ? ite : kNuvoton;
    const uint32_t length = key == Key::Ite ? 4 : 2;
    if (value == sequence[key_progress]) ++key_progress;
    else key_progress = value == sequence[0] ? 1 : 0;
    if (key_progress == length) {
      in_config = true;
      key_progress = 0;
    }
  }
};

class Board final : public IPortIo {
 public:
  std::vector<ModelChip*> chips;
  // Interleaving detection: every data access must come from the thread that wrote the index before it, and a Nuvoton bank must be used by
  // the thread that selected it.
  std::atomic<bool> interleaved{false};
  uint64_t reads = 0, writes = 0;

  static int thread_id() {
    static std::atomic<int> next{1};
    thread_local int id = next++;
    return id;
  }

  uint8_t read8(uint16_t port) override {
    ++reads;
    for (ModelChip* c : chips) {
      if (port == c->config_port + 1) {
        check_owner(config_owner_);
        if (!c->in_config) return 0xFF;
        return config_read(*c);
      }
      const uint16_t base = c->sensor_base();
      if (base && c->device[ld_of(*c)][0x30] && port == base + 6) {
        check_owner(sensor_owner_);
        if (c->banked) check_owner(bank_owner_);
        return sensor_read(*c);
      }
    }
    return 0xFF;
  }

  void write8(uint16_t port, uint8_t value) override {
    ++writes;
    for (ModelChip* c : chips) {
      if (port == c->config_port) {
        config_owner_ = thread_id();
        if (!c->in_config) {
          c->key_write(value);
        } else if (value == 0xAA && c->key != Key::Ite) {
          c->in_config = false;
          ++c->exits;
        } else {
          c->config_index = value;
        }
        continue;
      }
      if (port == c->config_port + 1) {
        check_owner(config_owner_);
        if (c->in_config) config_write(*c, value);
        continue;
      }
      const uint16_t base = c->sensor_base();
      if (!base || !c->device[ld_of(*c)][0x30]) continue;
      if (port == base + 5) {
        sensor_owner_ = thread_id();
        c->sensor_index = value;
      } else if (port == base + 6) {
        check_owner(sensor_owner_);
        if (c->banked && c->sensor_index == 0x4E) {
          bank_owner_ = thread_id();
          c->bank = value & 0x0F;
          c->high_byte_vendor = (value & 0x80) != 0;
        } else {
          if (c->banked) check_owner(bank_owner_);
          c->sensors[c->banked ? c->bank : 0][c->sensor_index] = value;
        }
      }
    }
  }

 private:
  static uint8_t ld_of(const ModelChip& c) { return c.key == Key::Ite ? 0x04 : (c.banked ? 0x0B : 0x04); }
  void check_owner(int owner) {
    if (owner != thread_id()) interleaved = true;
  }
  static uint8_t config_read(ModelChip& c) {
    if (c.config_index == 0x07) return c.logical_device;
    if (c.config_index < 0x30) return c.global[c.config_index];
    return c.device[c.logical_device & 0x0F][c.config_index];
  }
  static void config_write(ModelChip& c, uint8_t value) {
    if (c.config_index == 0x07) {
      c.logical_device = value;
    } else if (c.config_index == 0x02) {
      if (c.key == Key::Ite && (value & 0x02)) {
        c.in_config = false;
        ++c.exits;
      }
    } else if (c.config_index >= 0x30) {
      c.device[c.logical_device & 0x0F][c.config_index] = value;
    } else if (c.config_index >= 0x25) {   // (the ids below stay read-only)
      c.global[c.config_index] = value;
    }
  }
  static uint8_t sensor_read(ModelChip& c) {
    if (c.banked && c.sensor_index == 0x4E) return c.bank;
    if (c.banked && c.sensor_index == 0x4F && c.bank == 0) return c.high_byte_vendor ? 0x5C : 0xA3;
    return c.sensors[c.banked ? c.bank : 0][c.sensor_index];
  }

  int config_owner_ = 0, sensor_owner_ = 0, bank_owner_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Chips as a board's firmware would leave them.

static void nct6798(ModelChip& c, uint16_t port = 0x2E) {
  c = ModelChip();
  c.config_port = port;
  c.key = Key::Nuvoton;
  c.banked = true;
  c.set_id(0xD42B);   // NCT6798D, revision bits 2:0
  c.set_vendor(0x5CA3);
  c.set_device(0x0B, true, 0x0295);   // (the low bits are not part of the base)
  c.global[0x28] = 0x10;              // I/O space locked (a global register)
  c.nct(0x621) = 1;    // SYSTIN
  c.nct(0x622) = 16;   // PECI Agent 0
  c.nct(0x027) = 35;
  c.nct16(0x150, 0x2D80);   // 45.5 degrees
  c.nct16(0x4C0, 1200);     // SYSFAN
  c.nct16(0x4C2, 900);      // CPUFAN
  c.nct16(0x4C4, 0);        // nothing on AUXFAN0
  c.nct(0x480) = 140;       // Vcore: 1120 mV
  c.nct(0x483) = 210;       // 3VCC (internal half): 3360 mV
  c.nct(0x488) = 190;       // VBAT: 3040 mV
  for (uint16_t i = 0; i < 7; ++i) {
    static constexpr uint16_t kMode[] = {0x102, 0x202, 0x302, 0x802, 0x902, 0xA02, 0xB02};
    c.nct(kMode[i]) = 0x45;   // Smart Fan IV, low bits kept
  }
  c.nct(0x003) = 0x80;   // CPUFAN duty in effect
}

static void nct6776(ModelChip& c) {
  c = ModelChip();
  c.key = Key::Nuvoton;
  c.banked = true;
  c.set_id(0xC333);
  c.set_device(0x0B, true, 0x0290);
  c.nct(0x621) = 1;
  c.nct(0x622) = 2;   // CPUTIN
  c.nct(0x623) = 0;   // the third input monitors nothing
  c.nct(0x027) = 0xF6;   // -10 degrees
  c.nct16(0x150, 0x3200);
  // 13-bit counts: 1125 (1200 RPM) is 0x465 = high byte 0x23 (bits 12:5), low byte 0x05 (bits 4:0).
  c.nct(0x630) = 0x23; c.nct(0x631) = 0x05;
  c.nct(0x632) = 0xFF; c.nct(0x633) = 0x1F;   // counter ran over: stopped
  c.nct(0x634) = 0xFF; c.nct(0x635) = 0xFF;
  c.nct(0x022) = 200;   // AVCC: 3200 mV
  c.nct(0x550) = 205;   // 3VSB: 3280 mV
}

static void nct6775(ModelChip& c) {
  c = ModelChip();
  c.key = Key::Nuvoton;
  c.banked = true;
  c.set_id(0xB473);
  c.set_device(0x0B, true, 0x0290);
  c.nct(0x621) = 1;
  c.nct16(0x630, 1125);
  c.nct16(0x632, 0xFFFF);
  c.nct16(0x634, 0);
  c.nct(0x551) = 150;   // VBAT: 2400 mV
}

static void it8686(ModelChip& c, uint16_t port = 0x2E, uint16_t id = 0x8686, uint16_t base = 0x0A40) {
  c = ModelChip();
  c.config_port = port;
  c.key = Key::Ite;
  c.set_id(id);
  c.global[0x22] = 0x02;
  c.set_device(0x04, true, base);
  c.flat(0x58) = 0x90;
  c.flat(0x00) = 0x01;
  c.flat(0x51) = 0x80;   // bits 7:6 = 2: temperature 2 is PECI
  c.flat(0x29) = 30;
  c.flat(0x2A) = 50;
  c.flat(0x2B) = 0x80;   // open
  c.flat(0x0D) = 0xA3; c.flat(0x18) = 0x02;   // 675: 1000 RPM
  c.flat(0x0E) = 0xFF; c.flat(0x19) = 0xFF;   // stopped
  c.flat(0x0F) = 0x00; c.flat(0x1A) = 0x00;   // no reading
  c.flat(0x20) = 100;   // in0: 1200 mV
  c.flat(0x23) = 138;   // +3.3V (internal half): 3312 mV
  c.flat(0x28) = 125;   // Vbat: 3000 mV
  c.flat(0x15) = 0x81;  // PWM1 automatic, temperature input 1
  c.flat(0x63) = 0x99;
  c.flat(0x13) = 0x00;
}

static void it8705(ModelChip& c) {
  c = ModelChip();
  c.key = Key::Ite;
  c.set_id(0x8705);
  c.set_device(0x04, true, 0x0290);
  c.flat(0x58) = 0x90;
  c.flat(0x0B) = 0x01 | (2 << 3) | 0x40;   // divisors 2, 4 and 8
  c.flat(0x0D) = 225;   // 3000 RPM
  c.flat(0x0E) = 0xFF;  // stopped
  c.flat(0x0F) = 0x00;  // no reading
  c.flat(0x20) = 100;   // 1600 mV
  c.flat(0x15) = 0x40;  // manual, 64: duty 128
  c.flat(0x16) = 0x81;  // automatic
}

static void f71882(ModelChip& c, uint16_t port = 0x4E) {
  c = ModelChip();
  c.config_port = port;
  c.key = Key::Nuvoton;   // (Fintek's key is the same)
  c.set_id(0x0541);
  c.set_vendor(0x1934);
  c.set_device(0x04, true, 0x0228);
  c.flat(0x01) = 0x03;
  c.flat(0x72) = 40;
  c.flat(0x74) = 0xFF;
  c.flat(0xA0) = 0x03; c.flat(0xA1) = 0xE8;   // 1000: 1500 RPM
  c.flat(0xB0) = 0x0F; c.flat(0xB1) = 0xFF;   // full count: stopped
  c.flat(0x20) = 207;   // VCC3V (internal half): 3312 mV
  c.flat(0x21) = 140;   // Vcore: 1120 mV
  c.flat(0x96) = 0x01;  // fan 1 automatic by duty, others automatic by speed
  c.flat(0xA3) = 0x80;
}

static Optional<int32_t> temp(IHardwareMonitor* m, uint32_t i) { return m->temperature_millicelsius(i); }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Detection.

static void test_detection() {
  // Nothing on the bus.
  {
    Board board;
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 0 && set.chip_count() == 0);
    CHECK(set.monitor(0) == nullptr && set.found(0) == nullptr && !set.cpu_temperature_millicelsius() && !set.fan_rpm());
  }
  // A chip that takes the key but reads 0xFF ids (or 0x0000): nothing.
  for (uint16_t id : {uint16_t(0xFFFF), uint16_t(0x0000)}) {
    static ModelChip c;
    c = ModelChip();
    c.set_id(id);
    Board board;
    board.chips.push_back(&c);
    CHECK(!superio_probe(board, 0x2E).has_value());
    CHECK(c.exits >= 1 && !c.in_config);
  }
  // An id nobody knows: reported, no driver.
  {
    static ModelChip c;
    c = ModelChip();
    c.set_id(0x1234);
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 0 && set.chip_count() == 1);
    CHECK(set.found(0)->info == nullptr && set.found(0)->raw_id == 0x1234 && set.problem(0) && set.monitor(0) == nullptr);
    CHECK(!c.in_config);
  }
  // NCT6798 at 0x2E: name, base aligned, the I/O lock cleared, left out of configuration mode.
  {
    static ModelChip c;
    nct6798(c);
    Board board;
    board.chips.push_back(&c);
    Optional<SuperIoChipFound> found = superio_probe(board, 0x2E);
    CHECK(found && found->info && found->info->chip == SuperIoChip::Nct6798 && found->sensor_base == 0x0290 && found->sensor_active);
    CHECK(found && !found->problem && strcmp(found->info->name, "NCT6798") == 0);
    CHECK((c.global[0x28] & 0x10) == 0 && !c.in_config);
    CHECK(!superio_probe(board, 0x4E).has_value());
    // NCT6796 differs from it only in bit 3 of the id.
    c.set_id(0xD423);
    found = superio_probe(board, 0x2E);
    CHECK(found && found->info->chip == SuperIoChip::Nct6796);
  }
  // NCT6687: identified, its EC interface is not implemented.
  {
    static ModelChip c;
    nct6798(c);
    c.set_id(0xD592);
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 0 && set.chip_count() == 1);
    CHECK(set.found(0)->info && set.found(0)->info->chip == SuperIoChip::Nct6687 && set.found(0)->info->support == SuperIoSupport::IdentifiedOnly);
    CHECK(set.problem(0) != nullptr && set.monitor(0) == nullptr);
  }
  // A window the firmware did not set up, a device it did not activate.
  {
    static ModelChip c;
    nct6798(c);
    c.set_device(0x0B, true, 0x0000);
    Board board;
    board.chips.push_back(&c);
    Optional<SuperIoChipFound> found = superio_probe(board, 0x2E);
    CHECK(found && found->problem && found->sensor_base == 0);
    c.set_device(0x0B, false, 0x0290);
    found = superio_probe(board, 0x2E);
    CHECK(found && found->problem && !found->sensor_active);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 0 && set.monitor(0) == nullptr);
  }
  // The right id but a window that does not answer like a Nuvoton (vendor register): the driver refuses.
  {
    static ModelChip c;
    nct6798(c);
    c.banked = false;   // (the model then decodes no Nuvoton window: the vendor register reads 0xFF)
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 0 && set.chip_count() == 1 && set.monitor(0) == nullptr && set.problem(0));
  }
  // ITE at both ports (Gigabyte's IT8686 + IT8792 at 0x4E with the 0x55 0xAA key), and the ITE key does not open a Nuvoton part.
  {
    static ModelChip a, b;
    it8686(a, 0x2E);
    it8686(b, 0x4E, 0x8733, 0x0A60);
    Board board;
    board.chips.push_back(&a);
    board.chips.push_back(&b);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 2 && set.chip_count() == 2);
    CHECK(set.found(0)->info->chip == SuperIoChip::It8686 && set.found(0)->sensor_base == 0x0A40 && set.found(0)->config_port == 0x2E);
    CHECK(set.found(1)->info->chip == SuperIoChip::It8792 && set.found(1)->sensor_base == 0x0A60 && set.found(1)->config_port == 0x4E);
    CHECK(set.found(0)->revision == 0x02);
    CHECK(!a.in_config && !b.in_config && a.exits == 1 && b.exits == 1);
  }
  // Fintek at 0x4E (same key as Nuvoton, told apart by its vendor id).
  {
    static ModelChip c;
    f71882(c);
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 1 && set.chip_count() == 1);
    CHECK(set.found(0)->info->vendor == SuperIoVendor::Fintek && set.found(0)->info->chip == SuperIoChip::F71882 && set.found(0)->sensor_base == 0x0228);
    CHECK(!c.in_config);
  }
  // The table has every chip the drivers expect and no id twice.
  {
    const ArrayView<const SuperIoChipInfo> table = superio_chip_table();
    for (const SuperIoChipInfo& a : table) {
      for (const SuperIoChipInfo& b : table) {
        if (&a != &b && a.vendor == b.vendor) CHECK((a.id & b.id_mask) != b.id);   // (no entry's id falls in another's range)
      }
      if (a.support == SuperIoSupport::Supported) {
        const bool layout = a.vendor == SuperIoVendor::Nuvoton ? nuvoton_layout(a.chip) != nullptr
                            : a.vendor == SuperIoVendor::Ite   ? ite_layout(a.chip) != nullptr
                                                               : fintek_layout(a.chip) != nullptr;
        if (!layout) printf("FAILED: %s has no layout\n", a.name);
        CHECK(layout);
      }
    }
    CHECK(superio_lookup(false, 0xD428, 0x5CA3)->chip == SuperIoChip::Nct6798);
    CHECK(superio_lookup(false, 0x0541, 0x1934)->chip == SuperIoChip::F71882);
    CHECK(superio_lookup(false, 0x0541, 0x5CA3) == nullptr);   // a Fintek id read from a Nuvoton part means nothing
    CHECK(superio_lookup(true, 0x8628, 0)->chip == SuperIoChip::It8628);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Readings and PWM.

static void test_nuvoton() {
  static ModelChip c;
  nct6798(c);
  Board board;
  board.chips.push_back(&c);
  HardwareMonitorSet set;
  CHECK(set.probe(board) == 1);
  IHardwareMonitor* m = set.monitor(0);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(strcmp(m->name(), "NCT6798") == 0);
  CHECK(m->channel_count(MonitorChannel::Temperature) == 2 && m->channel_count(MonitorChannel::Fan) == 7 && m->channel_count(MonitorChannel::Voltage) == 15 &&
        m->channel_count(MonitorChannel::Pwm) == 7);
  CHECK(strcmp(m->label(MonitorChannel::Temperature, 0), "SYSTIN") == 0 && strcmp(m->label(MonitorChannel::Temperature, 1), "PECI Agent 0") == 0);
  CHECK(m->role(MonitorChannel::Temperature, 0) == SensorRole::System && m->role(MonitorChannel::Temperature, 1) == SensorRole::Cpu);
  CHECK(m->label(MonitorChannel::Temperature, 2) == nullptr && m->label(MonitorChannel::Fan, 7) == nullptr);
  CHECK(temp(m, 0).value_or(0) == 35000 && temp(m, 1).value_or(0) == 45500 && !temp(m, 2));
  CHECK(m->fan_rpm(0).value_or(0) == 1200 && m->fan_rpm(1).value_or(0) == 900 && m->fan_rpm(2).value_or(1) == 0 && !m->fan_rpm(7));
  CHECK(strcmp(m->label(MonitorChannel::Fan, 1), "CPUFAN") == 0 && m->role(MonitorChannel::Fan, 1) == SensorRole::Cpu);
  CHECK(m->voltage_millivolts(0).value_or(0) == 1120 && m->voltage_millivolts(3).value_or(0) == 3360 && m->voltage_millivolts(8).value_or(0) == 3040);
  CHECK(strcmp(m->label(MonitorChannel::Voltage, 8), "VBAT") == 0 && !m->voltage_millivolts(15));
  CHECK(set.cpu_temperature_millicelsius().value_or(0) == 45500 && set.fan_rpm().value_or(0) == 900);

  // PWM: Smart Fan IV at 0x80, then 200 by hand, then back to the curve.
  Optional<PwmState> p = m->pwm(1);
  CHECK(p && p->duty == 0x80 && p->mode == PwmMode::Automatic && p->duty_known);
  CHECK(m->set_pwm_duty(1, 200));
  CHECK(c.nct(0x209) == 200 && c.nct(0x202) == 0x05);
  p = m->pwm(1);
  CHECK(p && p->mode == PwmMode::Manual);
  CHECK(m->set_pwm_mode(1, PwmMode::Automatic) && c.nct(0x202) == 0x45);
  CHECK(!m->set_pwm_duty(7, 1) && !m->set_pwm_mode(7, PwmMode::Manual) && !m->pwm(7));
  // Manual from automatic keeps the duty the curve had reached.
  c.nct(0x001) = 0x66;
  CHECK(m->set_pwm_mode(0, PwmMode::Manual) && c.nct(0x109) == 0x66 && c.nct(0x102) == 0x05);
  // An output the firmware ran by hand has no curve to go back to.
  {
    static ModelChip d;
    nct6798(d);
    d.nct(0x302) = 0x05;
    Board b2;
    b2.chips.push_back(&d);
    HardwareMonitorSet s2;
    CHECK(s2.probe(b2) == 1);
    CHECK(!s2.monitor(0)->set_pwm_mode(2, PwmMode::Automatic));
  }
  CHECK(!board.interleaved);

  // The RPM register garbage: 0xFFFF is no reading.
  c.nct16(0x4C0, 0xFFFF);
  CHECK(!m->fan_rpm(0));


  // NCT6776: 13-bit counts, -10 degrees, the third source unused (only two temperatures).
  static ModelChip e;
  nct6776(e);
  Board b3;
  b3.chips.push_back(&e);
  HardwareMonitorSet s3;
  CHECK(s3.probe(b3) == 1);
  IHardwareMonitor* n = s3.monitor(0);
  CHECK(n && n->channel_count(MonitorChannel::Temperature) == 2 && n->channel_count(MonitorChannel::Fan) == 5 && n->channel_count(MonitorChannel::Voltage) == 9);
  if (n) {
    CHECK(temp(n, 0).value_or(0) == -10000 && temp(n, 1).value_or(0) == 50000 && strcmp(n->label(MonitorChannel::Temperature, 1), "CPUTIN") == 0);
    CHECK(n->fan_rpm(0).value_or(0) == 1200 && n->fan_rpm(1).value_or(1) == 0 && n->fan_rpm(2).value_or(1) == 0 && n->fan_rpm(3).value_or(1) == 0);
    CHECK(n->voltage_millivolts(2).value_or(0) == 3200 && n->voltage_millivolts(7).value_or(0) == 3280 && strcmp(n->label(MonitorChannel::Voltage, 7), "3VSB") == 0);
    CHECK(s3.cpu_temperature_millicelsius().value_or(0) == 50000);
  }

  // NCT6775: 16-bit counts, 0 and 0xFFFF are 0 RPM.
  static ModelChip f;
  nct6775(f);
  Board b4;
  b4.chips.push_back(&f);
  HardwareMonitorSet s4;
  CHECK(s4.probe(b4) == 1);
  IHardwareMonitor* o = s4.monitor(0);
  CHECK(o && o->channel_count(MonitorChannel::Pwm) == 3);
  if (o) {
    CHECK(o->fan_rpm(0).value_or(0) == 1200 && o->fan_rpm(1).value_or(1) == 0 && o->fan_rpm(2).value_or(1) == 0);
    CHECK(o->voltage_millivolts(8).value_or(0) == 2400);
    CHECK(o->channel_count(MonitorChannel::Temperature) == 1);   // (sources 2 and 3 are 0)
  }
}

static void test_ite() {
  static ModelChip c;
  it8686(c);
  Board board;
  board.chips.push_back(&c);
  HardwareMonitorSet set;
  CHECK(set.probe(board) == 1);
  IHardwareMonitor* m = set.monitor(0);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(m->channel_count(MonitorChannel::Temperature) == 3 && m->channel_count(MonitorChannel::Voltage) == 10 && m->channel_count(MonitorChannel::Fan) == 5 &&
        m->channel_count(MonitorChannel::Pwm) == 5);
  CHECK(temp(m, 0).value_or(0) == 30000 && temp(m, 1).value_or(0) == 50000 && !temp(m, 2));
  CHECK(m->role(MonitorChannel::Temperature, 1) == SensorRole::Cpu && strcmp(m->label(MonitorChannel::Temperature, 1), "CPU (PECI)") == 0);
  CHECK(m->fan_rpm(0).value_or(0) == 1000 && m->fan_rpm(1).value_or(1) == 0 && !m->fan_rpm(2));
  CHECK(m->voltage_millivolts(0).value_or(0) == 1200 && m->voltage_millivolts(3).value_or(0) == 3312 && m->voltage_millivolts(8).value_or(0) == 3000);
  CHECK(set.cpu_temperature_millicelsius().value_or(0) == 50000 && set.fan_rpm().value_or(0) == 1000);

  Optional<PwmState> p = m->pwm(0);
  CHECK(p && p->mode == PwmMode::Automatic && p->duty == 0x99);
  CHECK(m->set_pwm_duty(0, 128));
  CHECK(c.flat(0x63) == 128 && c.flat(0x15) == 0x01 && (c.flat(0x13) & 0x01));
  p = m->pwm(0);
  CHECK(p && p->mode == PwmMode::Manual && p->duty == 128);
  CHECK(m->set_pwm_mode(0, PwmMode::Automatic) && c.flat(0x15) == 0x81);
  CHECK(!m->set_pwm_mode(1, PwmMode::Automatic));   // PWM2 was manual (0x16 = 0) at start
  CHECK(!m->set_pwm_duty(5, 0));

  // The old IT8705: 8-bit counts with divisors, 16 mV, 7-bit manual duty.
  static ModelChip d;
  it8705(d);
  Board b2;
  b2.chips.push_back(&d);
  HardwareMonitorSet s2;
  CHECK(s2.probe(b2) == 1);
  IHardwareMonitor* o = s2.monitor(0);
  CHECK(o != nullptr);
  if (!o) return;
  CHECK(o->fan_rpm(0).value_or(0) == 3000 && o->fan_rpm(1).value_or(1) == 0 && !o->fan_rpm(2));
  CHECK(o->voltage_millivolts(0).value_or(0) == 1600);
  p = o->pwm(0);
  CHECK(p && p->mode == PwmMode::Manual && p->duty == 128 && p->duty_known);
  p = o->pwm(1);
  CHECK(p && p->mode == PwmMode::Automatic && !p->duty_known);
  CHECK(o->set_pwm_duty(0, 200) && d.flat(0x15) == 100);
  CHECK(o->set_pwm_mode(1, PwmMode::Manual) && d.flat(0x16) == 0x7F);
  CHECK(o->set_pwm_mode(1, PwmMode::Automatic) && d.flat(0x16) == 0x81);
  CHECK(!board.interleaved && !b2.interleaved);

  // Not an ITE environment controller (0x58 wrong): refused.
  static ModelChip e;
  it8686(e);
  e.flat(0x58) = 0x00;
  Board b3;
  b3.chips.push_back(&e);
  HardwareMonitorSet s3;
  CHECK(s3.probe(b3) == 0 && s3.chip_count() == 1 && s3.monitor(0) == nullptr);
}

static void test_fintek() {
  static ModelChip c;
  f71882(c);
  Board board;
  board.chips.push_back(&c);
  HardwareMonitorSet set;
  CHECK(set.probe(board) == 1);
  IHardwareMonitor* m = set.monitor(0);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(m->channel_count(MonitorChannel::Fan) == 4 && m->channel_count(MonitorChannel::Voltage) == 9 && m->channel_count(MonitorChannel::Temperature) == 3);
  CHECK(temp(m, 0).value_or(0) == 40000 && !temp(m, 1));
  CHECK(m->fan_rpm(0).value_or(0) == 1500 && m->fan_rpm(1).value_or(1) == 0 && m->fan_rpm(2).value_or(1) == 0);
  CHECK(m->voltage_millivolts(0).value_or(0) == 3312 && m->voltage_millivolts(1).value_or(0) == 1120);
  CHECK(strcmp(m->label(MonitorChannel::Voltage, 1), "Vcore") == 0);
  Optional<PwmState> p = m->pwm(0);
  CHECK(p && p->mode == PwmMode::Automatic && p->duty == 0x80 && p->duty_known);
  p = m->pwm(1);
  CHECK(p && p->mode == PwmMode::Automatic && !p->duty_known);
  CHECK(m->set_pwm_duty(0, 77) && c.flat(0xA3) == 77 && (c.flat(0x96) & 3) == 3);
  CHECK(m->set_pwm_mode(0, PwmMode::Automatic) && (c.flat(0x96) & 3) == 1);
  CHECK(m->set_pwm_duty(1, 10) && ((c.flat(0x96) >> 2) & 3) == 3 && (c.flat(0x96) & 3) == 1);
  CHECK(m->set_pwm_mode(1, PwmMode::Automatic) && c.flat(0x96) == 0x01);
  // Monitoring off: refused.
  c.flat(0x01) = 0x00;
  CHECK(!m->initialize());
  CHECK(!board.interleaved);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Garbage.

static void check_sane(HardwareMonitorSet& set) {
  HardwareMonitorReading readings[64];
  const uint32_t n = set.snapshot(ArrayView<HardwareMonitorReading>(readings));
  for (uint32_t i = 0; i < n; ++i) {
    const HardwareMonitorReading& r = readings[i];
    CHECK(r.label != nullptr);
    if (!r.valid) continue;
    switch (r.kind) {
      case MonitorChannel::Temperature: CHECK(r.value >= -127000 && r.value <= 128000); break;
      case MonitorChannel::Fan: CHECK(r.value >= 0 && r.value <= 1500000); break;
      case MonitorChannel::Voltage: CHECK(r.value >= 0 && r.value <= 255 * 32); break;
      case MonitorChannel::Pwm: CHECK(r.value >= 0 && r.value <= 255); break;
    }
  }
  (void)set.cpu_temperature_millicelsius();
  (void)set.fan_rpm();
}

static void test_garbage() {
  std::mt19937 rng(1234);
  // Every sensor register random, then single-bit flips of the good images: readings stay in range, nothing divides by zero.
  void (*setups[])(ModelChip&) = {[](ModelChip& c) { nct6798(c); }, [](ModelChip& c) { nct6776(c); }, [](ModelChip& c) { nct6775(c); },
                                   [](ModelChip& c) { it8686(c); }, [](ModelChip& c) { it8705(c); }, [](ModelChip& c) { f71882(c); }};
  for (auto setup : setups) {
    static ModelChip c;
    setup(c);
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    CHECK(set.probe(board) == 1);
    ModelChip good = c;
    for (int round = 0; round < 300; ++round) {
      for (auto& bank : c.sensors)
        for (uint8_t& b : bank) b = static_cast<uint8_t>(rng());
      check_sane(set);
    }
    for (int round = 0; round < 2000; ++round) {
      memcpy(c.sensors, good.sensors, sizeof c.sensors);
      const uint32_t bank = rng() % 16, reg = rng() % 256;
      c.sensors[bank][reg] ^= static_cast<uint8_t>(1u << (rng() % 8));
      check_sane(set);
    }
    // Every fan count at 0 and at 0xFFFF.
    for (uint8_t fill : {uint8_t(0x00), uint8_t(0xFF)}) {
      for (auto& bank : c.sensors) memset(bank, fill, sizeof bank);
      IHardwareMonitor* m = set.monitor(0);
      if (!m) continue;
      for (uint32_t i = 0; i < m->channel_count(MonitorChannel::Fan); ++i) {
        Optional<uint32_t> rpm = m->fan_rpm(i);
        CHECK(!rpm || *rpm == 0);
      }
    }
    CHECK(!board.interleaved);
  }
  // Random configuration space: whatever id answers, the probe never reports a chip whose table id does not match, and always leaves.
  for (int round = 0; round < 3000; ++round) {
    static ModelChip c;
    nct6798(c, round & 1 ? 0x4E : 0x2E);
    c.key = (round & 2) ? Key::Ite : Key::Nuvoton;
    for (uint8_t& b : c.global) b = static_cast<uint8_t>(rng());
    if (round & 4) c.set_id(static_cast<uint16_t>(superio_chip_table()[rng() % superio_chip_table().size()].id ^ (1u << (rng() % 16))));
    for (uint8_t& b : c.device[0x0B]) b = static_cast<uint8_t>(rng());
    for (uint8_t& b : c.device[0x04]) b = static_cast<uint8_t>(rng());
    Board board;
    board.chips.push_back(&c);
    HardwareMonitorSet set;
    (void)set.probe(board);
    CHECK(!c.in_config);
    for (uint32_t i = 0; i < set.chip_count(); ++i) {
      const SuperIoChipFound* f = set.found(i);
      if (f->info) CHECK((f->raw_id & f->info->id_mask) == f->info->id);
      if (f->sensor_base) CHECK((f->sensor_base & 7) == 0);
      if (set.monitor(i)) check_sane(set);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Several threads, one set: the kernel serializes with Synchronized (a SpinLock, which masks interrupts and cannot run in a user-mode test);
// a mutex stands in for it. The model flags any index/data pair or bank selection split between threads.

static void test_threads() {
  static ModelChip a, b;
  nct6798(a, 0x2E);
  f71882(b, 0x4E);
  Board board;
  board.chips.push_back(&a);
  board.chips.push_back(&b);
  HardwareMonitorSet set;
  CHECK(set.probe(board) == 2);
  std::mutex lock;
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 1500; ++i) {
        std::lock_guard<std::mutex> guard(lock);
        if ((i + t) % 3 == 0) {
          if (set.cpu_temperature_millicelsius().value_or(0) != 45500) ++wrong;
        } else if ((i + t) % 3 == 1) {
          if (set.fan_rpm().value_or(0) != 900) ++wrong;
        } else {
          HardwareMonitorReading r[64];
          if (set.snapshot(ArrayView<HardwareMonitorReading>(r)) != 2 + 7 + 15 + 7 + 3 + 4 + 9 + 4) ++wrong;
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CHECK(wrong == 0 && !board.interleaved);

  // The detector itself: an index written by one thread and its data read by another is caught.
  Board probe;
  probe.chips.push_back(&a);
  probe.write8(0x0295, 0x27);
  std::thread([&] { (void)probe.read8(0x0296); }).join();
  CHECK(probe.interleaved);
}

int main() {
  test_detection();
  test_nuvoton();
  test_ite();
  test_fintek();
  test_garbage();
  test_threads();
  if (g_failed) {
    printf("HardwareMonitorTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("HardwareMonitorTest: all tests passed\n");
  return 0;
}

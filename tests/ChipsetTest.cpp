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

// Host-side tests for the chipset layer: the SMBus host protocol against a model of the i801/PIIX4 registers, SPD decoding (with truncated and
// bit-flipped images) and reading (EE1004 pages, SPD5118 hub pages), the Intel PCH and AMD FCH/Promontory catalogues (cross-checked with the CPU
// generation rules), and both chipset drivers on fake PCI buses.
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "Hardware/CentralProcessingUnit/Intel/IntelGenerations.hpp"
#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "Hardware/Chipset/AMD/AmdFchChipset.hpp"
#include "Hardware/Chipset/Intel/IntelPchChipset.hpp"
#include "Hardware/Chipset/Smbus/SmbusHost.hpp"
#include "Hardware/Chipset/Smbus/Spd.hpp"

using namespace hardware;

extern "C" {
void tcos_halt() {}
void tcos_pause() {}
void tcos_breakpoint() {}
void tcos_disable_interrupts() {}
void tcos_enable_interrupts() {}
uint64_t tcos_rdtsc() { return 0; }
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------------
// SMBus devices: 7-bit addresses with a 256-byte register file each. Shared by the register-level host model and the transaction-level fake.

struct SmbusDevices {
  bool present[128] = {};
  uint8_t regs[128][256] = {};
  uint8_t block[128][33] = {};   // count then data, for block reads
  bool nack_writes[128] = {};
};

// The host controller's registers as the i801 and the FCH implement them, driving SmbusDevices.
class HostModel final : public ISmbusHostPorts {
 public:
  explicit HostModel(SmbusDevices& devices, SmbusHostFlavor flavor) : devices_(devices), flavor_(flavor) {}

  bool in_use = false;          // Intel INUSE_STS
  bool held_by_firmware = false;
  bool semaphore = false;       // AMD host semaphore (SMBSLVCNT bit 4)
  bool firmware_holds_semaphore = false;
  bool hang = false;            // the next transaction never finishes
  bool collide = false;
  bool killed = false;
  uint32_t transactions = 0;
  uint32_t pauses = 0;
  uint8_t aux = 0;
  uint8_t last_aux_during_transaction = 0;

  uint8_t read8(uint8_t reg) override {
    switch (reg) {
      case smbus_reg::kHostStatus: {
        uint8_t v = status_;
        if (flavor_ == SmbusHostFlavor::IntelI801) {
          if (in_use || held_by_firmware) v |= smbus_reg::kStatusInUse;
          if (!held_by_firmware) in_use = true;   // the read takes the semaphore
        }
        return v;
      }
      case smbus_reg::kHostControl: block_index_ = 0; return control_;
      case smbus_reg::kHostCommand: return command_;
      case smbus_reg::kHostAddress: return address_;
      case smbus_reg::kHostData0: return data0_;
      case smbus_reg::kHostData1: return data1_;
      case smbus_reg::kBlockData: return block_index_ < 32 ? block_[block_index_++] : 0;
      case smbus_reg::kAmdSlaveControl:
        if (flavor_ == SmbusHostFlavor::AmdFch) return semaphore ? smbus_reg::kAmdSemaphoreRequest : 0;
        return 0;
      case smbus_reg::kIntelAuxControl: return aux;
      default: return 0;
    }
  }

  void write8(uint8_t reg, uint8_t value) override {
    switch (reg) {
      case smbus_reg::kHostStatus:
        status_ &= static_cast<uint8_t>(~(value & smbus_reg::kStatusClear));   // write 1 to clear
        if (flavor_ == SmbusHostFlavor::IntelI801 && (value & smbus_reg::kStatusInUse)) in_use = false;
        break;
      case smbus_reg::kHostControl:
        control_ = value;
        if (value & smbus_reg::kControlKill) {
          killed = true;
          status_ = smbus_reg::kStatusFailed;
        }
        if (value & smbus_reg::kControlStart) start();
        break;
      case smbus_reg::kHostCommand: command_ = value; break;
      case smbus_reg::kHostAddress: address_ = value; break;
      case smbus_reg::kHostData0: data0_ = value; break;
      case smbus_reg::kHostData1: data1_ = value; break;
      case smbus_reg::kAmdSlaveControl:
        if (flavor_ != SmbusHostFlavor::AmdFch) break;
        if ((value & smbus_reg::kAmdSemaphoreRequest) && !firmware_holds_semaphore) semaphore = true;
        if (value & smbus_reg::kAmdSemaphoreRelease) semaphore = false;
        break;
      case smbus_reg::kIntelAuxControl: aux = value; break;
      default: break;
    }
  }

  void pause_us(uint32_t) override { ++pauses; }

 private:
  void start() {
    ++transactions;
    last_aux_during_transaction = aux;
    if (hang) {
      status_ = smbus_reg::kStatusBusy;
      return;
    }
    if (collide) {
      status_ = smbus_reg::kStatusBusError;
      return;
    }
    const uint8_t device = address_ >> 1;
    const bool read = address_ & 1;
    const uint8_t protocol = control_ & 0x1C;
    if (!devices_.present[device]) {
      status_ = smbus_reg::kStatusDeviceError;
      return;
    }
    if (!read && devices_.nack_writes[device]) {
      status_ = smbus_reg::kStatusDeviceError;
      return;
    }
    switch (protocol) {
      case smbus_reg::kProtocolQuick: break;
      case smbus_reg::kProtocolByte: data0_ = devices_.regs[device][0]; break;
      case smbus_reg::kProtocolByteData:
        if (read) data0_ = devices_.regs[device][command_];
        else devices_.regs[device][command_] = data0_;
        break;
      case smbus_reg::kProtocolWordData:
        data0_ = devices_.regs[device][command_];
        data1_ = devices_.regs[device][static_cast<uint8_t>(command_ + 1)];
        break;
      case smbus_reg::kProtocolBlockData:
        data0_ = devices_.block[device][0];
        for (int i = 0; i < 32; ++i) block_[i] = devices_.block[device][i + 1];
        break;
      default: status_ = smbus_reg::kStatusFailed; return;
    }
    status_ = smbus_reg::kStatusInterrupt;
  }

  SmbusDevices& devices_;
  SmbusHostFlavor flavor_;
  uint8_t status_ = 0, control_ = 0, command_ = 0, address_ = 0, data0_ = 0, data1_ = 0;
  uint8_t block_[32] = {};
  uint8_t block_index_ = 0;
};

static void test_smbus_host() {
  for (SmbusHostFlavor flavor : {SmbusHostFlavor::IntelI801, SmbusHostFlavor::AmdFch}) {
    SmbusDevices devices;
    devices.present[0x50] = true;
    devices.regs[0x50][2] = 0x0C;
    devices.regs[0x50][3] = 0x34;
    devices.regs[0x50][4] = 0x12;
    devices.present[0x2C] = true;
    devices.block[0x2C][0] = 3;
    devices.block[0x2C][1] = 0xAA;
    devices.block[0x2C][2] = 0xBB;
    devices.block[0x2C][3] = 0xCC;
    HostModel model(devices, flavor);
    SmbusHost host;

    uint8_t v = 0;
    CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::NoController);
    host.attach(&model, flavor);
    CHECK(host.attached() && host.flavor() == flavor);

    CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Ok && v == 0x0C);
    uint16_t w = 0;
    CHECK(host.read_word_data(0x50, 3, &w) == SmbusStatus::Ok && w == 0x1234);
    CHECK(host.write_byte_data(0x50, 9, 0x77) == SmbusStatus::Ok && devices.regs[0x50][9] == 0x77);
    CHECK(host.quick(0x50, false) == SmbusStatus::Ok && host.quick(0x51, false) == SmbusStatus::NoDevice);
    CHECK(host.read_byte_data(0x57, 0, &v) == SmbusStatus::NoDevice);
    CHECK(host.read_byte_data(0x80, 0, &v) == SmbusStatus::BadArgument);

    uint8_t block[32] = {}, length = 0;
    CHECK(host.read_block_data(0x2C, 0, ArrayView<uint8_t>(block, 32), &length) == SmbusStatus::Ok && length == 3 && block[0] == 0xAA && block[2] == 0xCC);
    if (flavor == SmbusHostFlavor::IntelI801) CHECK((model.last_aux_during_transaction & smbus_reg::kIntelAuxE32b) && model.aux == 0);   // E32B on, then restored
    devices.block[0x2C][0] = 0;   // a zero count is not a block
    CHECK(host.read_block_data(0x2C, 0, ArrayView<uint8_t>(block, 32), &length) == SmbusStatus::Failed && length == 0);
    devices.block[0x2C][0] = 33;
    CHECK(host.read_block_data(0x2C, 0, ArrayView<uint8_t>(block, 32), &length) == SmbusStatus::Failed);
    devices.block[0x2C][0] = 8;   // longer than the caller's buffer
    CHECK(host.read_block_data(0x2C, 0, ArrayView<uint8_t>(block, 4), &length) == SmbusStatus::Failed);

    // Arbitration is given back after every transaction.
    if (flavor == SmbusHostFlavor::IntelI801) CHECK(!model.in_use);
    else CHECK(!model.semaphore);

    // Someone else holds the controller.
    const uint32_t before = model.transactions;
    if (flavor == SmbusHostFlavor::IntelI801) {
      model.held_by_firmware = true;
      CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Busy);
      model.held_by_firmware = false;
    } else {
      model.firmware_holds_semaphore = true;
      CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Busy);
      model.firmware_holds_semaphore = false;
    }
    CHECK(model.transactions == before);

    // A collision and a hang.
    model.collide = true;
    CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Collision);
    model.collide = false;
    model.hang = true;
    model.pauses = 0;
    CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Timeout && model.killed && model.pauses >= SmbusHost::kMaxPolls);
    model.hang = false;
    CHECK(host.read_byte_data(0x50, 2, &v) == SmbusStatus::Ok && v == 0x0C);   // usable again after the kill
    CHECK(strcmp(smbus_status_name(SmbusStatus::NoDevice), "no device acknowledged") == 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// SPD images.

// A DDR4-3200 SO-DIMM: 16 GiB, 2 ranks of x8 8 Gb dies, 64-bit bus, Samsung.
static void make_ddr4(uint8_t (&spd)[512]) {
  memset(spd, 0, sizeof spd);
  spd[0] = 0x23;
  spd[1] = 0x11;
  spd[2] = 0x0C;
  spd[3] = 0x03;          // SO-DIMM
  spd[4] = 0x45;          // 8 Gb, 4 banks x 4 groups
  spd[6] = 0x00;          // monolithic
  spd[12] = 0x09;         // x8, 2 package ranks
  spd[13] = 0x03;         // 64-bit, no ECC
  spd[17] = 0x00;
  spd[18] = 0x05;         // 625 ps
  spd[125] = 0x00;
  spd[320] = 0x80;
  spd[321] = 0xCE;        // Samsung
}

// A DDR5-5600 UDIMM: 32 GiB, 2 ranks of x8 16 Gb dies, two 32-bit sub-channels, SK hynix.
static void make_ddr5(uint8_t (&spd)[1024]) {
  memset(spd, 0, sizeof spd);
  spd[0] = 0x30;
  spd[1] = 0x10;
  spd[2] = 0x12;
  spd[3] = 0x02;          // UDIMM
  spd[4] = 0x04;          // 16 Gb, monolithic
  spd[6] = 0x20;          // x8
  spd[20] = 0x65;         // tCKAVGmin 357 ps (0x0165)
  spd[21] = 0x01;
  spd[234] = 0x08;        // 2 package ranks
  spd[235] = 0x22;        // 2 sub-channels of 32 bits
  spd[512] = 0x80;
  spd[513] = 0xAD;        // SK hynix
}

// A DDR3-1600 UDIMM: 4 GiB, 2 ranks of x8 2 Gb dies, ECC, Kingston.
static void make_ddr3(uint8_t (&spd)[256]) {
  memset(spd, 0, sizeof spd);
  spd[0] = 0x92;
  spd[2] = 0x0B;
  spd[3] = 0x02;
  spd[4] = 0x03;          // 2 Gb
  spd[7] = 0x09;          // x8, 2 ranks
  spd[8] = 0x0B;          // 64-bit + 8-bit ECC
  spd[9] = 0x11;          // FTB 1 ps
  spd[10] = 0x01;
  spd[11] = 0x08;         // MTB 1/8 ns
  spd[12] = 0x0A;         // 1.25 ns
  spd[34] = 0x00;
  spd[117] = 0x01;
  spd[118] = 0x98;        // Kingston
}

static void test_spd_decode() {
  constexpr uint64_t kGiB = uint64_t{1} << 30;
  uint8_t ddr4[512];
  make_ddr4(ddr4);
  SpdModule m;
  CHECK(spd_decode(ConstByteView(ddr4, sizeof ddr4), &m));
  CHECK(m.type == MemoryType::Ddr4 && m.form == MemoryModuleForm::SoDimm && m.capacity_bytes == 16 * kGiB && m.speed_mts == 3200);
  CHECK(m.ranks == 2 && m.device_width == 8 && !m.ecc && m.manufacturer == 0x80CE);
  FixedString<128> text;
  spd_describe(m, text);
  CHECK(strcmp(text.c_str(), "DDR4 SO-DIMM, 16 GiB, 3200 MT/s, 2 ranks of x8, Samsung") == 0);

  // DDR4-2666 (750 ps) and DDR4-2400 (875 - 42 ps): the speed bins the clocks round to.
  ddr4[18] = 0x06;
  CHECK(spd_decode(ConstByteView(ddr4, sizeof ddr4), &m) && m.speed_mts == 2666);
  ddr4[18] = 0x07;
  ddr4[125] = static_cast<uint8_t>(-42);
  CHECK(spd_decode(ConstByteView(ddr4, sizeof ddr4), &m) && m.speed_mts == 2400);
  // A 3DS RDIMM: 4 dies per package, 2 package ranks, x4 16 Gb, ECC: 8 logical ranks, 256 GiB.
  make_ddr4(ddr4);
  ddr4[3] = 0x01;
  ddr4[4] = 0x06;
  ddr4[6] = 0xB2;
  ddr4[12] = 0x08;
  ddr4[13] = 0x0B;
  CHECK(spd_decode(ConstByteView(ddr4, sizeof ddr4), &m) && m.form == MemoryModuleForm::Rdimm && m.ranks == 8 && m.device_width == 4 && m.ecc);
  CHECK(m.capacity_bytes == 256 * kGiB);
  // An unknown maker is shown by its code.
  make_ddr4(ddr4);
  ddr4[320] = 0x07;
  ddr4[321] = 0x42;
  CHECK(spd_decode(ConstByteView(ddr4, sizeof ddr4), &m));
  spd_describe(m, text);
  CHECK(strstr(text.c_str(), "JEDEC maker 0x0742"));

  uint8_t ddr5[1024];
  make_ddr5(ddr5);
  CHECK(spd_decode(ConstByteView(ddr5, sizeof ddr5), &m));
  CHECK(m.type == MemoryType::Ddr5 && m.form == MemoryModuleForm::Udimm && m.capacity_bytes == 32 * kGiB && m.speed_mts == 5600);
  CHECK(m.ranks == 2 && m.device_width == 8 && !m.ecc && strcmp(jedec_manufacturer_name(m.manufacturer), "SK hynix") == 0);
  ddr5[20] = 0xA0;   // 416 ps: DDR5-4800
  ddr5[21] = 0x01;
  CHECK(spd_decode(ConstByteView(ddr5, sizeof ddr5), &m) && m.speed_mts == 4800);

  uint8_t ddr3[256];
  make_ddr3(ddr3);
  CHECK(spd_decode(ConstByteView(ddr3, sizeof ddr3), &m));
  CHECK(m.type == MemoryType::Ddr3 && m.form == MemoryModuleForm::Udimm && m.capacity_bytes == 4 * kGiB && m.speed_mts == 1600 && m.ecc && m.ranks == 2);
  CHECK(strcmp(jedec_manufacturer_name(m.manufacturer), "Kingston") == 0);

  // Not SPD.
  uint8_t junk[8] = {0, 0, 0x99};
  CHECK(!spd_decode(ConstByteView(junk, sizeof junk), &m) && m.type == MemoryType::Unknown);
  CHECK(!spd_decode(ConstByteView(), &m));
  uint8_t lp5[4] = {0, 0, 0x15, 0};
  CHECK(spd_decode(ConstByteView(lp5, sizeof lp5), &m) && m.type == MemoryType::Lpddr5 && m.capacity_bytes == 0);

  // Truncation: every prefix decodes without reading past it, and fields only appear once their bytes are there.
  for (uint32_t n = 0; n <= sizeof ddr4; ++n) {
    make_ddr4(ddr4);
    const bool ok = spd_decode(ConstByteView(ddr4, n), &m);
    CHECK(ok == (n > 2));
    if (n <= 13) CHECK(m.capacity_bytes == 0);
    if (n <= 321) CHECK(m.manufacturer == 0);
  }
  for (uint32_t n = 0; n <= sizeof ddr5; ++n) {
    make_ddr5(ddr5);
    spd_decode(ConstByteView(ddr5, n), &m);
    if (n <= 235) CHECK(m.capacity_bytes == 0);
    if (n <= 21) CHECK(m.speed_mts == 0);
  }
  for (uint32_t n = 0; n <= sizeof ddr3; ++n) {
    make_ddr3(ddr3);
    spd_decode(ConstByteView(ddr3, n), &m);
    if (n <= 12) CHECK(m.speed_mts == 0);
  }

  // Bit flips: any single flipped bit in the decoded bytes gives a sane result (or a refusal), never a wild one.
  struct Image {
    uint8_t* bytes;
    uint32_t size;
  };
  uint8_t ddr4_copy[512], ddr5_copy[1024], ddr3_copy[256];
  const Image images[] = {{ddr4_copy, 512}, {ddr5_copy, 1024}, {ddr3_copy, 256}};
  for (int which = 0; which < 3; ++which) {
    const uint32_t limit = which == 1 ? 600 : which == 0 ? 330 : 120;
    for (uint32_t byte = 0; byte < limit; ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        make_ddr4(ddr4_copy);
        make_ddr5(ddr5_copy);
        make_ddr3(ddr3_copy);
        images[which].bytes[byte] ^= static_cast<uint8_t>(1u << bit);
        if (!spd_decode(ConstByteView(images[which].bytes, images[which].size), &m)) continue;
        CHECK(m.capacity_bytes <= (uint64_t{4} << 40));   // at most 4 TiB per module
        CHECK(m.speed_mts <= 20000);
        CHECK(m.device_width == 0 || m.device_width == 4 || m.device_width == 8 || m.device_width == 16 || m.device_width == 32);
        spd_describe(m, text);
        CHECK(!text.empty());
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// SPD reading, against a transaction-level SMBus with EE1004 and SPD5118 behaviour.

class FakeSpdBus final : public ISmbusController {
 public:
  // EE1004 (DDR4) modules: 512 bytes in two pages; the page is global to the bus.
  uint8_t ddr4[8][512] = {};
  bool has_ddr4[8] = {};
  uint8_t ee1004_page = 0;
  bool ee1004_present = false;
  // SPD5118 (DDR5) hubs.
  uint8_t ddr5[8][1024] = {};
  bool has_ddr5[8] = {};
  uint8_t hub_mode[8] = {};
  bool hub_write_protected = false;
  // Plain DDR3 EEPROMs.
  uint8_t ddr3[8][256] = {};
  bool has_ddr3[8] = {};
  uint32_t writes = 0;

  SmbusStatus quick(uint8_t, bool) override { return SmbusStatus::NoDevice; }
  SmbusStatus read_byte(uint8_t address, uint8_t* value) override {
    *value = 0;
    if (address == 0x36) return ee1004_present && ee1004_page == 0 ? SmbusStatus::Ok : SmbusStatus::NoDevice;
    if (address == 0x37) return ee1004_present && ee1004_page == 1 ? SmbusStatus::Ok : SmbusStatus::NoDevice;
    return SmbusStatus::NoDevice;
  }
  SmbusStatus read_byte_data(uint8_t address, uint8_t command, uint8_t* value) override {
    *value = 0;
    if (address < 0x50 || address > 0x57) return SmbusStatus::NoDevice;
    const uint8_t slot = address - 0x50;
    if (has_ddr4[slot]) {
      *value = ddr4[slot][ee1004_page * 256 + command];
      return SmbusStatus::Ok;
    }
    if (has_ddr5[slot]) {
      if (command < 0x80) {
        *value = command == 0 ? 0x51 : command == 1 ? 0x18 : command == 0x0B ? hub_mode[slot] : 0;
      } else {
        *value = ddr5[slot][(hub_mode[slot] & 7) * 128 + (command & 0x7F)];
      }
      return SmbusStatus::Ok;
    }
    if (has_ddr3[slot]) {
      *value = ddr3[slot][command];
      return SmbusStatus::Ok;
    }
    return SmbusStatus::NoDevice;
  }
  SmbusStatus write_byte_data(uint8_t address, uint8_t command, uint8_t value) override {
    ++writes;
    if ((address == 0x36 || address == 0x37) && ee1004_present) {
      ee1004_page = address - 0x36;
      return SmbusStatus::NoDevice;   // like some EEPROMs: the data byte is not acknowledged, the page changes anyway
    }
    if (address >= 0x50 && address <= 0x57 && has_ddr5[address - 0x50] && command == 0x0B) {
      if (hub_write_protected) return SmbusStatus::NoDevice;
      hub_mode[address - 0x50] = value;
      return SmbusStatus::Ok;
    }
    return SmbusStatus::NoDevice;
  }
  SmbusStatus read_word_data(uint8_t, uint8_t, uint16_t*) override { return SmbusStatus::NoDevice; }
  SmbusStatus read_block_data(uint8_t, uint8_t, ArrayView<uint8_t>, uint8_t*) override { return SmbusStatus::NoDevice; }
};

static void test_spd_read() {
  static FakeSpdBus bus;   // large: keep it off the stack
  SpdImage image;
  SpdModule m;
  CHECK(spd_read(bus, 8, &image) == SmbusStatus::BadArgument);
  CHECK(spd_read(bus, 0, &image) == SmbusStatus::NoDevice && image.size == 0);

  // DDR4 on page 0: page 1 visited for the maker, page 0 again at the end.
  make_ddr4(bus.ddr4[0]);
  bus.has_ddr4[0] = bus.ee1004_present = true;
  CHECK(spd_read(bus, 0, &image) == SmbusStatus::Ok && bus.ee1004_page == 0);
  CHECK(spd_decode(image.view(), &m) && m.type == MemoryType::Ddr4 && m.capacity_bytes == (uint64_t{16} << 30) && m.manufacturer == 0x80CE && m.speed_mts == 3200);
  // Left on page 1 by the firmware: still read, and page 1 again afterwards.
  make_ddr4(bus.ddr4[1]);
  bus.ddr4[1][256 + 2] = 0x00;
  bus.has_ddr4[1] = true;
  bus.ee1004_page = 1;
  CHECK(spd_read(bus, 1, &image) == SmbusStatus::Ok && bus.ee1004_page == 1);
  CHECK(spd_decode(image.view(), &m) && m.type == MemoryType::Ddr4 && m.manufacturer == 0x80CE);
  bus.has_ddr4[0] = bus.has_ddr4[1] = bus.ee1004_present = false;
  bus.ee1004_page = 0;

  // DDR5 behind a hub on page 2: pages 0, 1 and 4 read, page 2 restored.
  make_ddr5(bus.ddr5[2]);
  bus.has_ddr5[2] = true;
  bus.hub_mode[2] = 0x02;
  CHECK(spd_read(bus, 2, &image) == SmbusStatus::Ok && bus.hub_mode[2] == 0x02);
  CHECK(spd_decode(image.view(), &m) && m.type == MemoryType::Ddr5 && m.capacity_bytes == (uint64_t{32} << 30) && m.manufacturer == 0x80AD && m.speed_mts == 5600);
  // A write-protected page register: the module is still found, with what page 0 holds.
  bus.hub_mode[2] = 0x00;
  bus.hub_write_protected = true;
  CHECK(spd_read(bus, 2, &image) == SmbusStatus::Ok && bus.hub_mode[2] == 0);
  CHECK(spd_decode(image.view(), &m) && m.type == MemoryType::Ddr5 && m.capacity_bytes == 0 && m.speed_mts == 5600 && m.manufacturer == 0);
  bus.hub_write_protected = false;
  // A hub in 2-byte addressing cannot be read with byte-data commands.
  bus.hub_mode[2] = 0x08;
  CHECK(spd_read(bus, 2, &image) == SmbusStatus::Failed);
  bus.has_ddr5[2] = false;

  // DDR3: no pages, no writes.
  make_ddr3(bus.ddr3[3]);
  bus.has_ddr3[3] = true;
  const uint32_t writes = bus.writes;
  CHECK(spd_read(bus, 3, &image) == SmbusStatus::Ok && bus.writes == writes);
  CHECK(spd_decode(image.view(), &m) && m.type == MemoryType::Ddr3 && m.speed_mts == 1600 && m.capacity_bytes == (uint64_t{4} << 30));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Catalogues.

static void test_intel_catalogue() {
  // SMBus IDs are unique across hubs, and lookups find their own row.
  for (uint32_t i = 0; i < intel_pch_count(); ++i) {
    const IntelPchInfo a = intel_pch_at(i);
    CHECK(a.pch != IntelPch::Unknown && intel_pch_info(a.pch).pch == a.pch && strlen(a.products) > 0);
    for (uint16_t id : a.smbus_ids) {
      CHECK(intel_pch_of_smbus(id).pch == a.pch);
      for (uint32_t j = i + 1; j < intel_pch_count(); ++j)
        for (uint16_t other : intel_pch_at(j).smbus_ids) CHECK(other != id);
    }
  }
  CHECK(intel_pch_at(intel_pch_count()).pch == IntelPch::Unknown && intel_pch_of_smbus(0x1234).pch == IntelPch::Unknown);
  CHECK(intel_pch_of_smbus(0xA323).pch == IntelPch::CannonPointH && intel_pch_of_smbus(0x7A23).pch == IntelPch::RaptorLakeS);
  CHECK(intel_pch_of_smbus(0x2930).pch == IntelPch::Ich9 && intel_pch_of_smbus(0x7F23).pch == IntelPch::ArrowLakeS && !intel_pch_of_smbus(0x2930).spi_function);
  CHECK(intel_pch_of_smbus(0x9D23).form == IntelPchForm::OnPackage && intel_pch_of_smbus(0x7E22).form == IntelPchForm::Soc);

  // Steppings split the shared models.
  CHECK(intel_pch_for_cpu(6, 0x9E, 0x9).contains(IntelPch::UnionPoint) && intel_pch_for_cpu(6, 0x9E, 0xC).pch[0] == IntelPch::CannonPointH);
  CHECK(intel_pch_for_cpu(6, 0x8E, 0xA).pch[0] == IntelPch::SunrisePointLp && intel_pch_for_cpu(6, 0x8E, 0xB).pch[0] == IntelPch::CannonPointLp);
  CHECK(intel_pch_for_cpu(6, 0x8E, 0xC).pch[0] == IntelPch::CometLakeLp && intel_pch_for_cpu(6, 0xB7, 1).contains(IntelPch::AlderLakeS));
  CHECK(intel_pch_for_cpu(6, 0xC6, 2).pch[0] == IntelPch::ArrowLakeS && intel_pch_for_cpu(6, 0x01, 0).count == 0);
  CHECK(intel_pch_for_cpu(18, 0x01, 0).pch[0] == IntelPch::NovaLakeS && intel_pch_of_smbus(0x6E23).pch == IntelPch::NovaLakeS);
  CHECK(intel_pch_for_cpu(19, 0x01, 0).pch[0] == IntelPch::DiamondRapids && intel_pch_of_smbus(0x5827).tco == IntelTcoKind::None);

  // The TCO flavour follows i2c-i801's feature flags.
  CHECK(intel_pch_of_smbus(0xA323).tco == IntelTcoKind::Version6 && intel_pch_of_smbus(0xA123).tco == IntelTcoKind::Version4);
  CHECK(intel_pch_of_smbus(0xA3A3).tco == IntelTcoKind::Version4 && intel_pch_of_smbus(0x06A3).tco == IntelTcoKind::Version6);
  CHECK(intel_pch_of_smbus(0x2930).tco == IntelTcoKind::None && intel_pch_of_smbus(0x5796).tco == IntelTcoKind::None);

  // Every Intel processor generation the CPU rules know pairs with a catalogued hub, apart from the parts that have none of their own.
  constexpr uint32_t kNoHub[] = {0x1D /* Dunnington: 7300 chipset */, 0x56 /* Broadwell-DE SoC */, 0x85 /* Knights Mill */, 0x8A /* Lakefield */};
  for (uint32_t family : {6u, 18u, 19u}) {
    for (uint32_t model = 0; model < 256; ++model) {
      for (uint32_t stepping = 0; stepping < 16; ++stepping) {
        alignas(X86_64Cpu) unsigned char storage[sizeof(X86_64Cpu)];
        if (!create_intel_generation(storage, nullptr, {family, model, stepping})) continue;
        bool exempt = false;
        for (uint32_t m : kNoHub) exempt = exempt || (family == 6 && m == model);
        const IntelPchCandidates c = intel_pch_for_cpu(family, model, stepping);
        if (exempt) continue;
        if (c.count == 0) {
          printf("FAILED: Intel CPU family %u model %X stepping %u has no hub\n", family, model, stepping);
          ++g_failed;
        }
        for (uint8_t i = 0; i < c.count; ++i) CHECK(intel_pch_info(c.pch[i]).pch == c.pch[i]);
      }
    }
  }
}

static void test_amd_catalogue() {
  CHECK(amd_fch_of_smbus(0x1022, 0x790B, 0x61).fch == AmdFch::Kerncz && amd_fch_of_smbus(0x1022, 0x790B, 0x61).decode == AmdSmbusDecode::PmDecodeEn);
  CHECK(amd_fch_of_smbus(0x1022, 0x790B, 0x61).pm_needs_mmio && amd_fch_of_smbus(0x1022, 0x790B, 0x59).pm_needs_mmio && !amd_fch_of_smbus(0x1022, 0x790B, 0x49).pm_needs_mmio);
  CHECK(amd_fch_of_smbus(0x1022, 0x790B, 0x42).decode == AmdSmbusDecode::PmLegacy && amd_fch_of_smbus(0x1D94, 0x790B, 0x59).fch == AmdFch::Kerncz);
  CHECK(amd_fch_of_smbus(0x1002, 0x4385, 0x3A).fch == AmdFch::Sb7x0 && amd_fch_of_smbus(0x1002, 0x4385, 0x3A).decode == AmdSmbusDecode::PciConfig);
  CHECK(amd_fch_of_smbus(0x1002, 0x4385, 0x42).fch == AmdFch::Sb8x0 && amd_fch_of_smbus(0x1022, 0x780B, 0x14).fch == AmdFch::Hudson);
  CHECK(amd_fch_of_smbus(0x8086, 0x790B, 0x61).fch == AmdFch::Unknown);

  for (uint32_t i = 0; i < amd_chipset_count(); ++i) {
    const AmdChipsetInfo a = amd_chipset_at(i);
    CHECK(a.chipset != AmdChipset::None && amd_chipset_info(a.chipset).chipset == a.chipset);
    bool upstream_listed = a.upstream_port_id == 0;
    for (uint16_t id : a.device_ids) {
      CHECK(amd_chipset_of_device(id).chipset == a.chipset);
      upstream_listed = upstream_listed || id == a.upstream_port_id;
      CHECK(id != 0x149C && id != 0x7901);   // the CPU's own USB and SATA: never a chipset's
    }
    CHECK(upstream_listed);
  }
  CHECK(amd_chipset_of_device(0x43F4).chipset == AmdChipset::Promontory21 && amd_chipset_of_device(0x57AD).chipset == AmdChipset::Bixby);
  CHECK(amd_chipset_of_device(0x43EE).chipset == AmdChipset::Promontory19 && amd_chipset_of_device(0x1234).chipset == AmdChipset::None);
  CHECK(amd_chipset_at(amd_chipset_count()).chipset == AmdChipset::None);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The chipset drivers on fake PCI buses.

class FakePci final : public IPciBus {
 public:
  struct Function {
    PciDeviceInfo info;
    uint8_t config[256];
  };
  Function functions[24] = {};
  uint32_t count = 0;

  Function& add(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor, uint16_t id, uint8_t cls, uint8_t subclass, uint8_t prog_if = 0,
                uint8_t revision = 0) {
    Function& f = functions[count++];
    f.info = {{bus, device, function}, vendor, id, cls, subclass, prog_if};
    memset(f.config, 0, sizeof f.config);
    put32(f, 0x00, (uint32_t{id} << 16) | vendor);
    put32(f, 0x08, (uint32_t{cls} << 24) | (uint32_t{subclass} << 16) | (uint32_t{prog_if} << 8) | revision);
    return f;
  }
  static void put32(Function& f, uint16_t offset, uint32_t value) { memcpy(&f.config[offset], &value, 4); }

  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return count; }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= count) return false;
    *out = functions[i].info;
    return true;
  }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    for (uint32_t i = 0; i < count; ++i) {
      const PciAddress& b = functions[i].info.address;
      if (b.bus != a.bus || b.device != a.device || b.function != a.function || offset > 252) continue;
      uint32_t v;
      memcpy(&v, &functions[i].config[offset & ~3u], 4);
      return v;
    }
    return 0xFFFFFFFF;
  }
  void write32(PciAddress, uint16_t, uint32_t) const override { ++writes; }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
  mutable uint32_t writes = 0;
};

static void test_intel_chipset() {
  // A Coffee Lake desktop on a Z390 board.
  FakePci pci;
  pci.add(0, 0x00, 0, 0x8086, 0x3E30, 0x06, 0x00);                        // the CPU's host bridge: not the chipset
  pci.add(0, 0x02, 0, 0x8086, 0x3E92, 0x03, 0x00);                        // the CPU's graphics
  pci.add(0, 0x12, 0, 0x8086, 0xA379, 0x11, 0x80);                        // thermal
  pci.add(0, 0x14, 0, 0x8086, 0xA36D, 0x0C, 0x03, 0x30);                  // xHCI
  FakePci::Function& heci = pci.add(0, 0x16, 0, 0x8086, 0xA360, 0x07, 0x80);
  FakePci::put32(heci, 0x40, 0x90000245);                                 // normal, init complete, normal mode
  pci.add(0, 0x17, 0, 0x8086, 0xA352, 0x01, 0x06, 0x01);                  // SATA
  pci.add(0, 0x1B, 0, 0x8086, 0xA340, 0x06, 0x04);                        // root port
  pci.add(0, 0x1F, 0, 0x8086, 0xA305, 0x06, 0x01);                        // LPC/eSPI (Z390)
  pci.add(0, 0x1F, 2, 0x8086, 0xA321, 0x05, 0x80);                        // PMC
  pci.add(0, 0x1F, 3, 0x8086, 0xA348, 0x04, 0x03);                        // HD Audio
  FakePci::Function& smbus = pci.add(0, 0x1F, 4, 0x8086, 0xA323, 0x0C, 0x05);
  FakePci::put32(smbus, 0x04, 0x00000001);                                // I/O decoding on
  FakePci::put32(smbus, 0x20, 0x0000EFA1);                                // SMBASE 0xEFA0
  FakePci::put32(smbus, 0x40, 0x00000001);                                // HST_EN
  FakePci::put32(smbus, 0x50, 0x00000401);                                // TCOBASE 0x400
  FakePci::put32(smbus, 0x54, 0x00000100);                                // TCO decode on
  FakePci::Function& spi = pci.add(0, 0x1F, 5, 0x8086, 0xA324, 0x0C, 0x80);
  FakePci::put32(spi, 0xDC, 0x0000002A);                                  // BLE, SMM_BWP, (bit 3) - not writable
  pci.add(0, 0x1F, 6, 0x8086, 0x15BC, 0x02, 0x00);                        // Ethernet
  pci.add(3, 0x00, 0, 0x8086, 0x2723, 0x02, 0x80);                        // a Wi-Fi card on another bus: not the chipset

  IntelPchChipset chipset;
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(chipset.found() && chipset.initialize() && chipset.info().pch == IntelPch::CannonPointH && !chipset.identified_by_cpu() && !chipset.unusual_pairing());
  CHECK(chipset.vendor() == ChipsetVendor::Intel && strcmp(chipset.codename(), "Cannon Point-H") == 0 && strstr(chipset.products(), "Z390"));
  CHECK(chipset.inventory().count() == 11 && chipset.inventory().count_of(ChipsetFunction::Wireless) == 0);
  ChipsetFunctionInfo f;
  CHECK(chipset.inventory().find(ChipsetFunction::SpiFlash, &f) && f.address.function == 5);
  CHECK(chipset.inventory().find(ChipsetFunction::PowerManagement, &f) && f.address.function == 2);
  FixedString<256> line;
  chipset.inventory().summarize(line);
  CHECK(strcmp(line.c_str(), "LPC/eSPI, SMBus, SPI flash, PMC, USB, SATA, HD Audio, ME, thermal, Ethernet, PCIe port") == 0);
  const ChipsetSmbus s = chipset.smbus();
  CHECK(s.usable && s.io_base == 0xEFA0 && s.flavor == SmbusHostFlavor::IntelI801 && s.address.function == 4);
  CHECK(chipset.bios_protection().known && chipset.bios_protection().locked() && chipset.bios_protection().smm_protect);
  CHECK(chipset.me().present && chipset.me().working_state() == 5 && chipset.me().init_complete() && chipset.me().operation_mode() == 0 && !chipset.me().manufacturing_mode());
  CHECK(strcmp(intel_me_working_state_name(chipset.me().working_state()), "normal") == 0);
  CHECK(chipset.tco().usable && chipset.tco().io_base == 0x400 && chipset.tco().kind == IntelTcoKind::Version6);
  PciAddress thermal{};
  CHECK(chipset.thermal_function(&thermal) && thermal.device == 0x12);
  chipset.summarize(line);
  CHECK(strstr(line.c_str(), "Cannon Point-H (300 series") && !strstr(line.c_str(), "pairs"));
  CHECK(pci.writes == 0);   // reads only

  // TCO decoding off.
  FakePci::put32(smbus, 0x54, 0);
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(!chipset.tco().usable && strstr(chipset.tco().reason, "off"));
  FakePci::put32(smbus, 0x54, 0x100);

  // The firmware left the host off; then without I/O decoding; then no I/O BAR.
  FakePci::put32(smbus, 0x40, 0);
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(!chipset.smbus().usable && strstr(chipset.smbus().reason, "disabled"));
  FakePci::put32(smbus, 0x40, 1);
  FakePci::put32(smbus, 0x04, 0);
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(!chipset.smbus().usable && strstr(chipset.smbus().reason, "decoding"));
  FakePci::put32(smbus, 0x04, 1);
  FakePci::put32(smbus, 0x20, 0xEFA0);   // a memory BAR
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(!chipset.smbus().usable && strstr(chipset.smbus().reason, "I/O base"));

  // A writable flash.
  FakePci::put32(spi, 0xDC, 0x01);
  chipset.probe(pci, {ChipsetVendor::Intel, 6, 0x9E, 0xC});
  CHECK(chipset.bios_protection().write_enable && !chipset.bios_protection().locked());

  // QEMU q35: the emulated ICH9 (SMBus at 1F.3, BIOS_CNTL in the LPC bridge) under a Raptor Lake host CPU.
  FakePci q35;
  q35.add(0, 0x00, 0, 0x8086, 0x29C0, 0x06, 0x00);
  FakePci::Function& lpc = q35.add(0, 0x1F, 0, 0x8086, 0x2918, 0x06, 0x01);
  FakePci::put32(lpc, 0xDC, 0x00);
  q35.add(0, 0x1F, 2, 0x8086, 0x2922, 0x01, 0x06, 0x01);
  FakePci::Function& ich9_smbus = q35.add(0, 0x1F, 3, 0x8086, 0x2930, 0x0C, 0x05);
  FakePci::put32(ich9_smbus, 0x04, 1);
  FakePci::put32(ich9_smbus, 0x20, 0x0701);
  FakePci::put32(ich9_smbus, 0x40, 1);
  chipset.probe(q35, {ChipsetVendor::Intel, 6, 0xB7, 1});
  CHECK(chipset.info().pch == IntelPch::Ich9 && chipset.unusual_pairing() && chipset.smbus().usable && chipset.smbus().io_base == 0x0700);
  CHECK(chipset.bios_protection().known && !chipset.bios_protection().locked() && !chipset.me().present);
  CHECK(!chipset.tco().usable && !chipset.thermal_function(&thermal));   // ICH9: the TCO is in the LPC bridge's ACPI block
  chipset.summarize(line);
  CHECK(strstr(line.c_str(), "usually pairs with Raptor Lake PCH-S"));

  // The SMBus function hidden: the hub is named from the CPU.
  FakePci hidden;
  hidden.add(0, 0x1F, 0, 0x8086, 0x7A84, 0x06, 0x01);
  chipset.probe(hidden, {ChipsetVendor::Intel, 6, 0x97, 2});
  CHECK(chipset.info().pch == IntelPch::AlderLakeS && chipset.identified_by_cpu() && !chipset.smbus().usable && strstr(chipset.smbus().reason, "no SMBus"));
  chipset.summarize(line);
  CHECK(strstr(line.c_str(), "named from the CPU"));

  // Nothing Intel on the bus and an unknown CPU.
  FakePci empty;
  chipset.probe(empty, {ChipsetVendor::Amd, 0x19, 0x21, 0});
  CHECK(!chipset.found() && strcmp(chipset.codename(), "unknown Intel chipset") == 0);
}

class FakePm final : public IAmdPmRegisters {
 public:
  uint8_t regs[256] = {};
  uint32_t reads = 0;
  uint8_t read8(uint8_t offset) override {
    ++reads;
    return regs[offset];
  }
};

static void test_amd_chipset() {
  // A Ryzen 7000 on an X670E board: two Promontory 21 chips, Zen 4 FCH revision 0x71, one Data Fabric node.
  FakePci pci;
  pci.add(0, 0x00, 0, 0x1022, 0x14D8, 0x06, 0x00);
  pci.add(0, 0x14, 0, 0x1022, 0x790B, 0x0C, 0x05, 0, 0x71);
  pci.add(0, 0x14, 3, 0x1022, 0x790E, 0x06, 0x01, 0, 0x51);
  pci.add(0, 0x18, 0, 0x1022, 0x14E0, 0x06, 0x00);
  pci.add(0, 0x18, 3, 0x1022, 0x14E3, 0x06, 0x00);
  pci.add(2, 0x00, 0, 0x1022, 0x43F4, 0x06, 0x04);
  pci.add(3, 0x0C, 0, 0x1022, 0x43F5, 0x06, 0x04);
  pci.add(5, 0x00, 0, 0x1022, 0x43F4, 0x06, 0x04);
  pci.add(6, 0x00, 0, 0x1022, 0x43F7, 0x0C, 0x03, 0x30);
  pci.add(7, 0x00, 0, 0x1022, 0x43F6, 0x01, 0x06, 0x01);
  pci.add(9, 0x00, 0, 0x1022, 0x15B6, 0x0C, 0x03, 0x30);    // the CPU's own USB
  pci.add(9, 0x00, 2, 0x1022, 0x1649, 0x10, 0x80);          // PSP
  pci.add(10, 0x00, 0, 0x10DE, 0x2684, 0x03, 0x00);         // a graphics card: not the chipset
  FakePm ports, mmio;
  mmio.regs[0x00] = 0x10;   // SmbusAsfIoEn
  mmio.regs[0x01] = 0x0B;   // base 0x0B00
  mmio.regs[0x02] = 0x00;   // port 0
  ports.regs[0x00] = 0xFF;  // the index ports are off on this FCH
  ports.regs[0x01] = 0xFF;

  AmdFchChipset chipset;
  chipset.probe(pci, &ports, &mmio);
  CHECK(chipset.found() && chipset.fch().fch == AmdFch::Kerncz && chipset.fch_revision() == 0x71 && chipset.pm_through_mmio() && ports.reads == 0);
  CHECK(chipset.board_chipset().chipset == AmdChipset::Promontory21 && chipset.chipset_chips() == 2 && chipset.data_fabric_nodes() == 1);
  CHECK(strcmp(chipset.codename(), "Promontory 21") == 0 && strstr(chipset.products(), "X670E"));
  const ChipsetSmbus s = chipset.smbus();
  CHECK(s.usable && s.io_base == 0x0B00 && s.flavor == SmbusHostFlavor::AmdFch && chipset.smbus_port() == 0);
  CHECK(chipset.inventory().count_of(ChipsetFunction::PciePort) == 3 && chipset.inventory().count_of(ChipsetFunction::Usb) == 2);
  CHECK(chipset.inventory().count_of(ChipsetFunction::SecurityProcessor) == 1 && chipset.inventory().count() == 9);
  FixedString<256> line;
  chipset.summarize(line);
  CHECK(strstr(line.c_str(), "Promontory 21 x2 (X670") && strstr(line.c_str(), "rev 0x71") && strstr(line.c_str(), "1 Data Fabric node"));
  CHECK(pci.writes == 0);

  // Without a way to map the MMIO block the index ports are tried, and a block of 0xFF is recognised as nothing there.
  chipset.probe(pci, &ports, nullptr);
  CHECK(!chipset.pm_through_mmio() && !chipset.smbus().usable && strstr(chipset.smbus().reason, "0xFF"));
  chipset.probe(pci, nullptr, nullptr);
  CHECK(!chipset.smbus().usable && strstr(chipset.smbus().reason, "not reachable"));

  // B650: one chip. The SMBus host off, then routed to port 2.
  pci.functions[7].info.device_id = 0x43F5;
  mmio.regs[0x00] = 0x00;
  chipset.probe(pci, &ports, &mmio);
  CHECK(chipset.chipset_chips() == 1 && strstr(chipset.products(), "B650") && !chipset.smbus().usable && strstr(chipset.smbus().reason, "disabled"));
  mmio.regs[0x00] = 0x10;
  mmio.regs[0x02] = 0x10;
  chipset.probe(pci, &ports, &mmio);
  CHECK(chipset.smbus().usable && chipset.smbus_port() == 2);

  // A Zen+ laptop: FCH revision 0x59 (an MMIO one) with nothing to map MMIO, so through the index ports; no chipset on the board; the legacy decode
  // for an early revision.
  FakePci laptop;
  laptop.add(0, 0x14, 0, 0x1022, 0x790B, 0x0C, 0x05, 0, 0x59);
  laptop.add(0, 0x18, 0, 0x1022, 0x15E8, 0x06, 0x00);
  FakePm laptop_ports;
  laptop_ports.regs[0x00] = 0x10;
  laptop_ports.regs[0x01] = 0x0B;
  chipset.probe(laptop, &laptop_ports, nullptr);
  CHECK(!chipset.pm_through_mmio() && chipset.smbus().usable && chipset.smbus().io_base == 0x0B00 && chipset.board_chipset().chipset == AmdChipset::None);
  CHECK(strcmp(chipset.codename(), "FCH (KERNCZ)") == 0);
  chipset.summarize(line);
  CHECK(strstr(line.c_str(), "no chipset on the board"));
  laptop.functions[0].config[0x08] = 0x42;   // revision 0x42: PM 0x2C/0x2D
  laptop_ports.regs[0x2C] = 0x01;
  laptop_ports.regs[0x2D] = 0x0B;
  chipset.probe(laptop, &laptop_ports, nullptr);
  CHECK(chipset.fch().decode == AmdSmbusDecode::PmLegacy && chipset.smbus().usable && chipset.smbus().io_base == 0x0B00);

  // SB700: the base from configuration space.
  FakePci sb700;
  FakePci::Function& sm = sb700.add(0, 0x14, 0, 0x1002, 0x4385, 0x0C, 0x05, 0, 0x3A);
  FakePci::put32(sm, 0x90, 0x0B01);
  FakePci::put32(sm, 0xD0, 0x00010000);   // SMBHSTCFG (0xD2) bit 0
  chipset.probe(sb700, nullptr, nullptr);
  CHECK(chipset.fch().fch == AmdFch::Sb7x0 && chipset.smbus().usable && chipset.smbus().io_base == 0x0B00);

  // X570: Bixby, recognised by its switch even though its USB has the CPU's ID.
  FakePci x570;
  x570.add(0, 0x14, 0, 0x1022, 0x790B, 0x0C, 0x05, 0, 0x61);
  x570.add(2, 0x00, 0, 0x1022, 0x57AD, 0x06, 0x04);
  x570.add(3, 0x08, 0, 0x1022, 0x57A4, 0x06, 0x04);
  x570.add(4, 0x00, 0, 0x1022, 0x149C, 0x0C, 0x03, 0x30);
  chipset.probe(x570, nullptr, &mmio);
  CHECK(chipset.board_chipset().chipset == AmdChipset::Bixby && chipset.chipset_chips() == 1 && strcmp(chipset.products(), "X570") == 0);

  // Nothing AMD.
  FakePci empty;
  chipset.probe(empty, nullptr, nullptr);
  CHECK(!chipset.found() && !chipset.smbus().usable);
}

static void test_inventory() {
  ChipsetInventory inventory;
  PciDeviceInfo d{{0, 0x1F, 4}, 0x8086, 0xA323, 0x0C, 0x05, 0};
  for (uint32_t i = 0; i < ChipsetInventory::kCapacity; ++i) CHECK(inventory.add(d));
  CHECK(!inventory.add(d) && inventory.count() == ChipsetInventory::kCapacity);
  ChipsetFunctionInfo f;
  CHECK(!inventory.at(ChipsetInventory::kCapacity, &f) && !inventory.find(ChipsetFunction::Usb, &f));
  FixedString<256> line;
  inventory.summarize(line);
  CHECK(strcmp(line.c_str(), "64 SMBus") == 0);
  inventory.clear();
  inventory.summarize(line);
  CHECK(line.empty());
  // Classes shared by two kinds are split by slot.
  CHECK(chipset_function_of({{0, 0x1F, 5}, 0x8086, 0, 0x0C, 0x80, 0}) == ChipsetFunction::SpiFlash);
  CHECK(chipset_function_of({{0, 0x15, 0}, 0x8086, 0, 0x0C, 0x80, 0}) == ChipsetFunction::SerialIo);
  CHECK(chipset_function_of({{0, 0x1F, 2}, 0x8086, 0, 0x05, 0x80, 0}) == ChipsetFunction::PowerManagement);
  CHECK(chipset_function_of({{0, 0x14, 2}, 0x8086, 0, 0x05, 0x00, 0}) == ChipsetFunction::Other);
}

int main() {
  test_smbus_host();
  test_spd_decode();
  test_spd_read();
  test_intel_catalogue();
  test_amd_catalogue();
  test_intel_chipset();
  test_amd_chipset();
  test_inventory();
  if (g_failed) {
    printf("ChipsetTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("ChipsetTest: all tests passed\n");
  return 0;
}

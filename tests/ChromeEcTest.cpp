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

// Host-side tests for the Chrome EC driver (src/Hardware/EmbeddedController/ChromeEc), against a model of the EC's host interface written
// from the same understanding of the protocol (so it checks the code's consistency and robustness, not the numbers against real hardware):
// the command/status ports, the packet buffer as plain LPC I/O or behind a Microchip EMI (address registers, byte and auto-increment
// access), the memory map at 0x900, 0xE00 or behind the EMI. Covers protocol v3 framing and checksums, an EC that lies (bad checksum, wrong
// struct version, lengths too long for the packet or the caller, short answers, refused packets, bit flips in every response byte), busy
// timeouts that must end, every standard and Framework command the driver uses, the hardware monitor and keyboard backlight, the charge
// limit store, detection that refuses (and writes nothing) on a machine without the EC, and two threads whose commands must not interleave.
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcStack.hpp"

using namespace hardware;
using namespace hardware::chromeec;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The model.

enum class MapAt { Port900, PortE00, Emi, Nowhere };

struct Fault {
  bool corrupt_checksum = false;
  bool wrong_version = false;
  int32_t claim_length = -1;     // >= 0: the header claims this data length (the data written stays as it was)
  bool stuck_busy = false;       // busy forever after a command
  bool busy_before = false;      // busy forever, even before
  uint32_t busy_polls = 0;       // busy for this many status reads after each command
  uint8_t refuse = 0;            // result byte at 0x200 (the EC refused the packet)
  int32_t truncate_to = -1;      // >= 0: reply with only this many data bytes (header honest)
  int32_t flip_bit = -1;         // >= 0: flip this bit of the response packet (header + data) after it is built
  uint16_t only_command = 0;     // 0: the fault applies to every command; else only to this one
};

struct FakeEc final : IChromeEcPorts {
  // Configuration.
  bool mec = false;
  MapAt map_at = MapAt::Port900;
  bool framework = true;
  bool version3 = true;
  uint32_t fans = 1;
  uint8_t thermal_version = 2;
  Fault fault;

  // EC state.
  uint8_t packet[256] = {};
  uint8_t map[256] = {};
  uint32_t busy_left = 0;
  uint8_t result_byte = 0;
  uint8_t emi_b0 = 0, emi_b1 = 0;
  uint32_t fan_duty[4] = {}, fan_rpm_target[4] = {};
  bool fan_auto[4] = {true, true, true, true};
  uint32_t all_fans_duty_calls = 0, all_fans_auto_calls = 0, all_fans_rpm_calls = 0;
  uint8_t kbd = 30;
  uint8_t led_brightness[8][6] = {};
  bool led_auto[8] = {true, true, true, true, true, true, true, true};
  uint8_t charge_max = 0, charge_min = 0;   // 0 max: no limit
  uint8_t privacy_mic = 1, privacy_cam = 0;
  uint8_t fp_level = 1;
  uint32_t commands = 0, cmd_versions_calls = 0;
  uint16_t last_command = 0;
  uint8_t last_version = 0;
  uint32_t last_params_size = 0;

  // Port traffic.
  std::mutex mutex;
  std::vector<std::pair<uint16_t, uint8_t>> writes;
  bool record_writes = false;
  uint64_t reads = 0;
  // Interleaving detector: a transaction is open from the first byte of a request until its response has been read.
  std::thread::id owner;
  bool open = false;
  uint32_t response_left = 0;
  std::atomic<uint32_t> overlaps{0};

  FakeEc() { reset_map(); }

  void reset_map() {
    memset(map, 0, sizeof map);
    map[memmap::kId] = 'E';
    map[memmap::kId + 1] = 'C';
    map[memmap::kIdVersion] = 1;
    map[memmap::kThermalVersion] = thermal_version;
    map[memmap::kBatteryVersion] = 1;
    map[memmap::kSwitchesVersion] = 1;
    map[memmap::kHostCommandFlags] = 0x01 | memmap::kHostCommandFlagVersion3;
    map[memmap::kSwitches] = memmap::kSwitchLidOpen;
    for (uint32_t i = 0; i < 24; ++i) map[i < 16 ? i : memmap::kTempSensorB + (i - 16)] = memmap::kTempNotPresent;
    map[0] = 0x6E;                       // 110 + 200 K = 36.85 C
    map[1] = 0x7D;                       // 125 + 200 = 51.85 C
    map[2] = memmap::kTempError;         // present, failed
    map[memmap::kTempSensorB] = 0x64;    // sensor 16: 100 + 200 = 26.85 C
    for (uint32_t f = 0; f < 4; ++f) put16(memmap::kFan + 2 * f, memmap::kFanNotPresent);
    put32(memmap::kBatteryVoltage, 16800);
    put32(memmap::kBatteryRate, 1200);
    put32(memmap::kBatteryCapacity, 2750);
    map[memmap::kBatteryFlags] = memmap::kBatteryAcPresent | memmap::kBatteryPresent | memmap::kBatteryCharging;
    put32(memmap::kBatteryDesignCapacity, 3572);
    put32(memmap::kBatteryDesignVoltage, 15480);
    put32(memmap::kBatteryLastFullCharge, 3500);
    put32(memmap::kBatteryCycleCount, 42);
    memcpy(map + memmap::kBatteryManufacturer, "NVT\0\0\0\0\0", 8);
    memcpy(map + memmap::kBatteryModel, "FRANGWA", 7);
  }
  void set_fans(uint32_t n) {
    fans = n;
    for (uint32_t f = 0; f < 4; ++f) put16(memmap::kFan + 2 * f, f < n ? static_cast<uint16_t>(2000 + 100 * f) : memmap::kFanNotPresent);
  }
  void put16(uint32_t at, uint16_t v) { map[at] = v & 0xFF; map[at + 1] = v >> 8; }
  void put32(uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) map[at + i] = (v >> (8 * i)) & 0xFF; }
  uint16_t map_base() const { return map_at == MapAt::PortE00 ? 0xE00 : 0x900; }

  // --- Interleave bookkeeping.
  void touch() {
    if (open && owner != std::this_thread::get_id()) overlaps.fetch_add(1);
  }
  void request_byte(uint32_t offset) {
    if (offset == 0) {
      if (open && owner != std::this_thread::get_id()) overlaps.fetch_add(1);
      open = true;
      owner = std::this_thread::get_id();
      response_left = 0xFFFFFFFF;
    }
  }
  void response_byte(uint32_t offset) {
    if (!open || owner != std::this_thread::get_id() || response_left == 0xFFFFFFFF) return;
    if (offset < 8 + 256 && response_left > 0 && --response_left == 0) open = false;
  }

  // --- Ports.
  uint8_t in8(uint16_t port) override {
    std::lock_guard<std::mutex> g(mutex);
    ++reads;
    if (port == kPortHostCommand) {
      touch();
      if (fault.busy_before) return kStatusProcessing;
      if (busy_left > 0) {
        if (!fault.stuck_busy) --busy_left;
        return kStatusProcessing | kStatusFromHost;
      }
      return 0;
    }
    if (port == kPortHostData) {
      touch();
      if (result_byte != 0) open = false;
      return result_byte;
    }
    if (mec && port >= kPortMecEmi && port < kPortMecEmi + 8) {
      touch();
      const uint32_t lane = port - kPortMecEmi;
      if (lane == kMecEmiAddressB0) return emi_b0;
      if (lane == kMecEmiAddressB1) return emi_b1;
      if (lane < kMecEmiData) return 0;
      const uint8_t v = emi_byte(lane - kMecEmiData, nullptr);
      emi_after(lane - kMecEmiData);
      return v;
    }
    if (!mec && port >= kPortHostPacket && port < kPortHostPacket + 256) {
      touch();
      response_byte(port - kPortHostPacket);
      return packet[port - kPortHostPacket];
    }
    if ((map_at == MapAt::Port900 || map_at == MapAt::PortE00) && port >= map_base() && port < map_base() + 256) return map[port - map_base()];
    return 0xFF;   // nothing on the bus
  }

  void out8(uint16_t port, uint8_t value) override {
    std::lock_guard<std::mutex> g(mutex);
    if (record_writes) writes.push_back({port, value});
    if (port == kPortHostCommand) {
      touch();
      if (value == kCommandProtocol3) run();
      return;
    }
    if (mec && port >= kPortMecEmi && port < kPortMecEmi + 8) {
      touch();
      const uint32_t lane = port - kPortMecEmi;
      if (lane == kMecEmiAddressB0) emi_b0 = value;
      else if (lane == kMecEmiAddressB1) emi_b1 = value;
      else if (lane >= kMecEmiData) {
        (void)emi_byte(lane - kMecEmiData, &value);
        emi_after(lane - kMecEmiData);
      }
      return;
    }
    if (!mec && port >= kPortHostPacket && port < kPortHostPacket + 256) {
      touch();
      request_byte(port - kPortHostPacket);
      packet[port - kPortHostPacket] = value;
    }
  }
  void delay_us(uint32_t) override {}

  uint32_t emi_address() const { return (uint32_t{emi_b1} << 8) | (emi_b0 & 0xFC); }
  uint8_t emi_byte(uint32_t lane, const uint8_t* write) {
    const uint32_t a = emi_address() + lane;
    if (a < 0x100) {
      if (write) {
        request_byte(a);
        packet[a] = *write;
      } else {
        response_byte(a);
      }
      return packet[a];
    }
    if (a < 0x200 && map_at == MapAt::Emi) return map[a - 0x100];
    return 0xFF;
  }
  void emi_after(uint32_t lane) {
    if (lane == 3 && (emi_b0 & 3) == kMecAccessLongAutoIncrement) {
      const uint32_t next = emi_address() + 4;
      emi_b0 = static_cast<uint8_t>((next & 0xFC) | (emi_b0 & 3));
      emi_b1 = static_cast<uint8_t>((next >> 8) & 0x7F);
    }
  }

  // --- The EC's command processing.
  static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
  bool applies(uint16_t cmd) const { return fault.only_command == 0 || fault.only_command == cmd; }

  void run() {
    ++commands;
    result_byte = 0;
    busy_left = fault.busy_polls;
    if (fault.stuck_busy) busy_left = 1;
    uint8_t sum = 0;
    const uint16_t length = packet[6] | (packet[7] << 8);
    if (packet[0] != 3 || length > 248) {
      result_byte = static_cast<uint8_t>(EcResult::InvalidHeader);
      return;
    }
    for (uint32_t i = 0; i < 8u + length; ++i) sum = static_cast<uint8_t>(sum + packet[i]);
    if (sum != 0) {
      result_byte = static_cast<uint8_t>(EcResult::InvalidChecksum);
      return;
    }
    const uint16_t cmd = packet[2] | (packet[3] << 8);
    const uint8_t version = packet[4];
    last_command = cmd;
    last_version = version;
    last_params_size = length;
    uint8_t params[248];
    memcpy(params, packet + 8, length);
    uint8_t out[248] = {};
    uint32_t out_len = 0;
    const uint16_t result = handle(cmd, version, params, length, out, &out_len);
    if (applies(cmd) && fault.refuse) {
      result_byte = fault.refuse;
      return;
    }
    uint32_t data_len = out_len;
    if (applies(cmd) && fault.truncate_to >= 0 && static_cast<uint32_t>(fault.truncate_to) < data_len) data_len = fault.truncate_to;
    memset(packet, 0, sizeof packet);
    packet[0] = (applies(cmd) && fault.wrong_version) ? 2 : 3;
    packet[2] = result & 0xFF;
    packet[3] = result >> 8;
    packet[4] = data_len & 0xFF;
    packet[5] = (data_len >> 8) & 0xFF;
    memcpy(packet + 8, out, data_len);
    uint8_t s = 0;
    for (uint32_t i = 0; i < 8 + data_len; ++i) s = static_cast<uint8_t>(s + packet[i]);
    packet[1] = static_cast<uint8_t>(-s);
    if (applies(cmd) && fault.corrupt_checksum) packet[1] ^= 0x5A;
    if (applies(cmd) && fault.claim_length >= 0) {
      packet[4] = fault.claim_length & 0xFF;
      packet[5] = (fault.claim_length >> 8) & 0xFF;
    }
    if (applies(cmd) && fault.flip_bit >= 0) packet[(fault.flip_bit / 8) % 256] ^= static_cast<uint8_t>(1u << (fault.flip_bit % 8));
    const uint32_t claimed = packet[4] | (packet[5] << 8);
    response_left = 8 + (claimed > 248 ? 0 : claimed);
  }

  static void text(uint8_t* out, const char* s) { memcpy(out, s, strlen(s)); }

  uint32_t versions_of(uint16_t cmd) const {
    switch (cmd) {
      case command::kHello: case command::kGetVersion: case command::kGetChipInfo: case command::kGetProtocolInfo: case command::kGetFeatures:
      case command::kPwmGetFanTargetRpm: case command::kPwmGetKeyboardBacklight: case command::kPwmSetKeyboardBacklight: case command::kTempSensorGetInfo:
      case command::kChargeState: return 1;
      case command::kGetCommandVersions: case command::kPwmSetFanTargetRpm: case command::kPwmSetFanDuty: case command::kThermalAutoFanControl: return 3;
      case command::kLedControl: return 2;
      default:
        if (framework && cmd >= 0x3E00) return 1;
        return 0;
    }
  }

  uint16_t handle(uint16_t cmd, uint8_t version, const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t* out_len) {
    const uint32_t mask = versions_of(cmd);
    if (mask == 0) return static_cast<uint16_t>(EcResult::InvalidCommand);
    if (version >= 32 || !(mask & (1u << version))) return static_cast<uint16_t>(EcResult::InvalidVersion);
    switch (cmd) {
      case command::kHello: {
        if (in_len < 4) return 3;
        const uint32_t v = le32(in) + 0x01020304u;
        for (int i = 0; i < 4; ++i) out[i] = (v >> (8 * i)) & 0xFF;
        *out_len = 4;
        return 0;
      }
      case command::kGetVersion:
        text(out, framework ? "hx30_v0.0.1-7a61a89" : "volteer_v2.0.1");
        text(out + 32, framework ? "hx30_v0.0.1-7a61a89+rw" : "volteer_v2.0.2");
        out[96] = 2;
        *out_len = 100;
        return 0;
      case command::kGetChipInfo:
        text(out, mec ? "Microchip" : "Nuvoton");
        text(out + 32, mec ? "MEC1521" : "NPCX796FC");
        text(out + 64, "A0\x01");   // (an unprintable byte: shown as '?')
        *out_len = 96;
        return 0;
      case command::kGetProtocolInfo:
        out[0] = 1 << 3;
        out[4] = 0xF0; out[5] = 0;   // 240-byte requests at most
        out[6] = 0x00; out[7] = 1;   // 256
        *out_len = 12;
        return 0;
      case command::kGetFeatures:
        out[0] = (1 << kFeaturePwmFan) | (1 << kFeaturePwmKeyboard) | (1 << kFeatureLed);
        out[2] = 1 << (kFeatureBattery - 16);
        out[4] = 0x01;   // feature 32
        *out_len = 8;
        return 0;
      case command::kGetCommandVersions: {
        ++cmd_versions_calls;
        uint16_t which = 0;
        if (version == 1) { if (in_len < 2) return 3; which = in[0] | (in[1] << 8); }
        else { if (in_len < 1) return 3; which = in[0]; }
        const uint32_t m = versions_of(which);
        if (!m) return static_cast<uint16_t>(EcResult::InvalidParam);
        for (int i = 0; i < 4; ++i) out[i] = (m >> (8 * i)) & 0xFF;
        *out_len = 4;
        return 0;
      }
      case command::kPwmGetFanTargetRpm:
        for (int i = 0; i < 4; ++i) out[i] = (fan_rpm_target[0] >> (8 * i)) & 0xFF;
        *out_len = 4;
        return 0;
      case command::kPwmSetFanTargetRpm:
      case command::kPwmSetFanDuty: {
        const bool duty = cmd == command::kPwmSetFanDuty;
        if (in_len < (version ? 5u : 4u)) return 3;
        const uint32_t v = le32(in);
        if (duty && v > 100) return 3;
        if (version == 1) {
          if (in[4] >= fans) return 3;
          (duty ? fan_duty : fan_rpm_target)[in[4]] = v;
          fan_auto[in[4]] = false;
        } else {
          for (uint32_t f = 0; f < fans; ++f) { (duty ? fan_duty : fan_rpm_target)[f] = v; fan_auto[f] = false; }
          ++(duty ? all_fans_duty_calls : all_fans_rpm_calls);
        }
        return 0;
      }
      case command::kThermalAutoFanControl:
        if (version == 1) {
          if (in_len < 1 || in[0] >= fans) return 3;
          fan_auto[in[0]] = true;
        } else {
          for (uint32_t f = 0; f < fans; ++f) fan_auto[f] = true;
          ++all_fans_auto_calls;
        }
        return 0;
      case command::kPwmGetKeyboardBacklight:
        out[0] = kbd;
        out[1] = 1;
        *out_len = 2;
        return 0;
      case command::kPwmSetKeyboardBacklight:
        if (in_len < 1 || in[0] > 100) return 3;
        kbd = in[0];
        return 0;
      case command::kLedControl: {
        if (in_len < 8 || in[0] >= 8) return 3;
        const uint8_t led = in[0];
        if (in[1] & kLedFlagQuery) {
          const uint8_t ranges[6] = {0, 1, 0, 0, 1, 1};   // a battery LED: green, white, amber
          memcpy(out, ranges, 6);
        } else if (in[1] & kLedFlagAuto) {
          led_auto[led] = true;
        } else {
          led_auto[led] = false;
          memcpy(led_brightness[led], in + 2, 6);
        }
        *out_len = 6;
        return 0;
      }
      case command::kTempSensorGetInfo: {
        if (in_len < 1) return 3;
        static const char* const names[] = {"F75303_Local", "F75303_CPU", "F75303_DDR"};
        if (in[0] < 3) text(out, names[in[0]]);
        else if (in[0] == 16) text(out, "Battery");
        else return 3;
        out[32] = 0;
        *out_len = 33;
        return 0;
      }
      case command::kChargeState: {
        if (in_len < 1 || in[0] != 0) return 3;
        const uint32_t v[5] = {1, 17400, 2000, 3250, 79};
        for (int k = 0; k < 5; ++k) for (int i = 0; i < 4; ++i) out[4 * k + i] = (v[k] >> (8 * i)) & 0xFF;
        *out_len = 20;
        return 0;
      }
      case command::kFrameworkChargeLimitControl: {
        if (in_len < 3) return 3;
        if (in[0] & kChargeLimitDisable) { charge_max = 0; charge_min = 0; }
        if (in[0] & kChargeLimitSet) { charge_max = in[1]; charge_min = in[2]; }
        out[0] = charge_max;
        out[1] = charge_min;
        *out_len = (in[0] & kChargeLimitGet) ? 2 : 0;
        return 0;
      }
      case command::kFrameworkPrivacySwitches:
        out[0] = privacy_mic;
        out[1] = privacy_cam;
        *out_len = 2;
        return 0;
      case command::kFrameworkChassisIntrusion:
        out[0] = 1; out[1] = 0; out[2] = 3; out[3] = 1;
        *out_len = 4;
        return 0;
      case command::kFrameworkFpLedLevel:
        if (in_len < 2) return 3;
        if (in[1]) {
          static const uint8_t percent[] = {55, 40, 15};
          out[0] = percent[fp_level];
          out[1] = fp_level;
          *out_len = 2;
        } else {
          if (in[0] > 2) return 3;
          fp_level = in[0];
        }
        return 0;
      case command::kFrameworkDeckState:
        if (in_len < 1 || in[0] != 0) return 3;
        for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(i + 1);
        out[8] = 0x05;
        *out_len = 9;
        return 0;
      case command::kFrameworkExpansionBayStatus:
        out[0] = kBayEnabled | kBayDoorClosed;
        out[1] = 0x14;
        out[2] = 0x15;
        *out_len = 3;
        return 0;
      default: return static_cast<uint16_t>(EcResult::InvalidCommand);
    }
  }
};

// A machine without the EC: every port reads 0xFF; every write is remembered.
struct EmptyMachine final : IChromeEcPorts {
  std::vector<std::pair<uint16_t, uint8_t>> writes;
  uint8_t in8(uint16_t) override { return 0xFF; }
  void out8(uint16_t port, uint8_t value) override { writes.push_back({port, value}); }
  void delay_us(uint32_t) override {}
};

struct MemoryStore final : IChargeLimitStore {
  Optional<uint8_t> value;
  uint32_t stores = 0;
  Optional<uint8_t> load() override { return value; }
  bool store(uint8_t max_percent) override { value = max_percent; ++stores; return true; }
};

struct CountingLock final : IEcFirmwareLock {
  int depth = 0, max_depth = 0, locks = 0;
  void lock() override { ++depth; ++locks; if (depth > max_depth) max_depth = depth; }
  void unlock() override { --depth; }
};

static void configure(FakeEc& ec, bool mec, MapAt at) {
  ec.mec = mec;
  ec.map_at = at;
  ec.set_fans(1);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

static void test_framing() {
  uint8_t out[256];
  const uint8_t params[3] = {0x11, 0x22, 0x33};
  const uint32_t n = build_request(0x3E03, 1, ConstByteView(params), ByteView(out));
  CHECK(n == 11);
  CHECK(out[0] == 3 && out[2] == 0x03 && out[3] == 0x3E && out[4] == 1 && out[5] == 0 && out[6] == 3 && out[7] == 0);
  CHECK(out[8] == 0x11 && out[10] == 0x33);
  CHECK(byte_sum(ConstByteView(out).first(n)) == 0);
  // Too many parameters, or a buffer too small for the header: nothing is built.
  uint8_t big[249] = {};
  CHECK(build_request(1, 0, ConstByteView(big), ByteView(out)) == 0);
  uint8_t tiny[7];
  CHECK(build_request(1, 0, ConstByteView(), ByteView(tiny)) == 0);
  CHECK(checksum_complement(ConstByteView(params)) == static_cast<uint8_t>(-(0x11 + 0x22 + 0x33)));
  ResponseHeader h;
  const uint8_t resp[8] = {3, 0, 0x09, 0x00, 0x04, 0x00, 0, 0};
  CHECK(parse_response_header(ConstByteView(resp), &h) && h.struct_version == 3 && h.result == 9 && h.data_length == 4);
  CHECK(!parse_response_header(ConstByteView(resp).first(7), &h));
}

static void check_started(ChromeEcStack& stack, FakeEc& fake, const char* what) {
  if (!stack.present()) {
    printf("  (%s: %s)\n", what, stack.problem());
    CHECK(stack.present());
    return;
  }
  ChromeEc& ec = *stack.ec();
  CHECK(strcmp(ec.version().read_only.c_str(), fake.framework ? "hx30_v0.0.1-7a61a89" : "volteer_v2.0.1") == 0);
  CHECK(ec.version().current == EcImage::ReadWrite);
  CHECK(strcmp(ec.chip().name.c_str(), fake.mec ? "MEC1521" : "NPCX796FC") == 0);
  CHECK(strcmp(ec.chip().revision.c_str(), "A0?") == 0);
  CHECK(ec.has_feature(kFeaturePwmFan) && ec.has_feature(kFeaturePwmKeyboard) && ec.has_feature(kFeatureBattery) && ec.has_feature(32) && !ec.has_feature(kFeatureThermal));
  CHECK(ec.protocol().max_request_bytes == 0xF0 && ec.protocol().max_response_bytes == 0x100);
  CHECK(ec.transport_kind() == (fake.mec ? EcTransportKind::Mec : EcTransportKind::Lpc));
  Optional<uint32_t> h = ec.hello(0x10);
  CHECK(h && *h == 0x01020314u);
}

static void test_transports() {
  {
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    ChromeEcStack stack(fake);
    CHECK(stack.start(false));
    check_started(stack, fake, "LPC 0x900");
  }
  {
    FakeEc fake;
    configure(fake, false, MapAt::PortE00);
    ChromeEcStack stack(fake);
    CHECK(stack.start(true));
    check_started(stack, fake, "LPC 0xE00");
  }
  {
    FakeEc fake;
    configure(fake, true, MapAt::Emi);
    ChromeEcStack stack(fake);
    CHECK(stack.start(true));
    check_started(stack, fake, "MEC");
    // A misaligned, odd-length memory map read through the EMI (byte mode, then auto-increment).
    uint8_t battery[13] = {};
    CHECK(stack.ec()->read_memmap(memmap::kBatteryVoltage + 1, ByteView(battery)));
    CHECK(memcmp(battery, fake.map + memmap::kBatteryVoltage + 1, 13) == 0);
    uint8_t one[3] = {};
    CHECK(stack.ec()->read_memmap(memmap::kId + 1, ByteView(one)) && one[0] == 'C' && one[1] == 1);
    // Beyond the map: refused.
    uint8_t past[2];
    CHECK(!stack.ec()->read_memmap(0xFE, ByteView(past)));
    // Odd-sized, misaligned EMI writes and reads round-trip (the packet buffer).
    MecEmi emi(fake);
    for (uint32_t start = 0; start < 8; ++start)
      for (uint32_t size = 0; size < 14; ++size) {
        uint8_t data[16], back[16] = {};
        for (uint32_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(start * 31 + i * 7 + 1);
        CHECK(emi.write(0x40 + start, ConstByteView(data).first(size)));
        CHECK(emi.read(0x40 + start, ByteView(back).first(size)));
        CHECK(memcmp(data, back, size) == 0);
      }
    CHECK(!emi.read(0x1FF, ByteView(one).first(2)));
  }
  {
    // A MEC whose map the board remapped to plain I/O at 0xE00 (Framework 11th-13th gen, per cros_ec_lpc's quirk) and whose EMI does not show it.
    FakeEc fake;
    configure(fake, true, MapAt::PortE00);
    ChromeEcStack stack(fake);
    CHECK(stack.start(true));   // plain LPC host commands do not answer; the EMI does, with the map read at 0xE00
    check_started(stack, fake, "MEC, map at 0xE00");
  }
  {
    // No protocol 3 advertised: refused before any command.
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    fake.map[memmap::kHostCommandFlags] = 0x01;
    ChromeEcStack stack(fake);
    CHECK(!stack.start(false) && fake.commands == 0 && strstr(stack.problem(), "protocol 3"));
  }
}

static void test_detection_refuses() {
  {
    EmptyMachine m;
    ChromeEcStack stack(m);
    CHECK(!stack.start(false) && !stack.present() && m.writes.empty());
    CHECK(stack.ec() == nullptr && stack.monitor() == nullptr && stack.framework() == nullptr && stack.keyboard_backlight() == nullptr);
  }
  {
    // The ACPI tables name an EC but nothing answers: only the EMI address registers were written, and nothing was found.
    EmptyMachine m;
    ChromeEcStack stack(m);
    CHECK(!stack.start(true));
    bool only_emi = true;
    for (auto& w : m.writes) only_emi &= (w.first == kPortMecEmi + kMecEmiAddressB0 || w.first == kPortMecEmi + kMecEmiAddressB1);
    CHECK(only_emi && !m.writes.empty());
  }
  {
    // A plain map at 0x900: found by reads alone (no write before the first command).
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    fake.record_writes = true;
    const EcDetection d = detect_chrome_ec(fake, false);
    CHECK(d.found && d.kind == EcTransportKind::Lpc && d.lpc_memmap_base == 0x900 && fake.writes.empty());
  }
  {
    // A MEC without its ACPI id and with the map only behind the EMI: nothing identifies it, so the EMI is not touched.
    FakeEc fake;
    configure(fake, true, MapAt::Emi);
    fake.record_writes = true;
    const EcDetection d = detect_chrome_ec(fake, false);
    CHECK(!d.found && fake.writes.empty());
  }
  {
    // The EC answers HELLO wrongly: not started, nothing exposed.
    struct BadHello final : IChromeEcPorts {
      FakeEc inner;
      uint8_t in8(uint16_t port) override {
        uint8_t v = inner.in8(port);
        if (inner.last_command == command::kHello && port == kPortHostPacket + 8) v ^= 1, inner.packet[1] ^= 1;   // keep the checksum right
        return v;
      }
      void out8(uint16_t port, uint8_t value) override { inner.out8(port, value); }
      void delay_us(uint32_t) override {}
    } bad;
    configure(bad.inner, false, MapAt::Port900);
    ChromeEcStack stack(bad);
    CHECK(!stack.start(false) && !stack.present());
  }
}

static void test_untrusted_responses() {
  FakeEc fake;
  configure(fake, false, MapAt::Port900);
  ChromeEcStack stack(fake);
  CHECK(stack.start(false));
  ChromeEc& ec = *stack.ec();
  uint8_t buf[64];
  const uint8_t hello_in[4] = {1, 2, 3, 4};

  fake.fault = Fault();
  fake.fault.corrupt_checksum = true;
  EcReply r = ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf));
  CHECK(r.transfer == EcTransferStatus::BadChecksum && !r.ok() && r.data_bytes == 0);
  CHECK(!ec.hello(5));

  fake.fault = Fault();
  fake.fault.wrong_version = true;
  CHECK(ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf)).transfer == EcTransferStatus::BadResponse);

  fake.fault = Fault();
  fake.fault.claim_length = 300;   // more than the packet buffer holds
  CHECK(ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf)).transfer == EcTransferStatus::ResponseTooLarge);
  fake.fault.claim_length = 200;   // fits the packet, not the caller's buffer
  CHECK(ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf).first(16)).transfer == EcTransferStatus::ResponseTooLarge);
  fake.fault.claim_length = 40;    // claims more than was written: the stale bytes break the sum
  r = ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf));
  CHECK(!r.ok());

  fake.fault = Fault();
  fake.fault.truncate_to = 1;      // an honest but short answer: the typed call refuses it
  CHECK(!ec.hello(5));
  CHECK(!ec.read_version() && !ec.keyboard_backlight() && !ec.fan_target_rpm() && !ec.charge_state());
  CHECK(!stack.framework()->charge_limit() && !stack.framework()->expansion_bay());

  fake.fault = Fault();
  fake.fault.refuse = static_cast<uint8_t>(EcResult::InvalidChecksum);
  r = ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf));
  CHECK(r.transfer == EcTransferStatus::Ok && r.result == static_cast<uint16_t>(EcResult::InvalidChecksum) && !r.ok());

  // An EC error result passes through.
  fake.fault = Fault();
  r = ec.command(0x7777, 0, ConstByteView(), ByteView(buf));
  CHECK(r.transfer == EcTransferStatus::Ok && r.result == static_cast<uint16_t>(EcResult::InvalidCommand));
  // Larger than the EC's protocol info allows (240-byte packets): refused before it is sent.
  const uint32_t before = fake.commands;
  uint8_t params[240] = {};
  CHECK(ec.command(command::kHello, 0, ConstByteView(params), ByteView(buf)).transfer == EcTransferStatus::RequestTooLarge && fake.commands == before);
  CHECK(ec.command(command::kHello, 0, ConstByteView(params).first(232), ByteView(buf)).transfer == EcTransferStatus::Ok);

  // Busy: a few polls is fine; forever ends in Timeout within the poll budget, as does an EC busy before the request.
  fake.fault = Fault();
  fake.fault.busy_polls = 37;
  CHECK(ec.hello(9).value_or(0u) == 9u + 0x01020304u);
  fake.fault = Fault();
  fake.fault.stuck_busy = true;
  auto t0 = std::chrono::steady_clock::now();
  r = ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf));
  CHECK(r.transfer == EcTransferStatus::Timeout);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
  fake.fault = Fault();
  fake.busy_left = 0;
  fake.fault.busy_before = true;
  const uint32_t sent = fake.commands;
  CHECK(ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(buf)).transfer == EcTransferStatus::BusyBefore && fake.commands == sent);
  fake.fault = Fault();
  fake.busy_left = 0;
  CHECK(ec.hello(1).has_value());

  // Every single bit of a response flipped: never accepted as a good answer with wrong data.
  for (int bit = 0; bit < (8 + 4) * 8; ++bit) {
    fake.fault = Fault();
    fake.fault.flip_bit = bit;
    fake.busy_left = 0;
    uint8_t out[16] = {};
    r = ec.command(command::kHello, 0, ConstByteView(hello_in), ByteView(out));
    const bool good = r.ok() && r.data_bytes == 4 && out[0] == 5 && out[1] == 5 && out[2] == 5 && out[3] == 5;
    if (r.ok() && !good) {
      printf("  bit %d accepted a wrong answer\n", bit);
      CHECK(false);
    }
    if (bit / 8 == 2 || bit / 8 == 3) continue;   // (a flipped result code is an honest-looking error answer)
    CHECK(!r.ok());
  }
  fake.fault = Fault();
  // And random garbage in the whole packet buffer after a command: refused or harmless, never out of bounds.
  std::mt19937 rng(7);
  uint32_t accepted = 0;
  for (int round = 0; round < 3000; ++round) {
    struct Garbage final : IChromeEcPorts {
      FakeEc& f;
      std::mt19937& rng;
      Garbage(FakeEc& fe, std::mt19937& r) : f(fe), rng(r) {}
      uint8_t in8(uint16_t port) override {
        if (port >= kPortHostPacket && port < kPortHostPacket + 256) return static_cast<uint8_t>(rng());
        return f.in8(port);
      }
      void out8(uint16_t port, uint8_t value) override { f.out8(port, value); }
      void delay_us(uint32_t) override {}
    } garbage(fake, rng);
    LpcTransport t(garbage, kPortMemmap);
    uint8_t packet[16];
    const uint32_t n = build_request(command::kHello, 0, ConstByteView(hello_in), ByteView(packet));
    uint8_t out[32];
    const EcTransfer tr = t.transfer(ConstByteView(packet).first(n), ByteView(out).first(1 + round % 31));
    if (tr.status == EcTransferStatus::Ok) {
      ++accepted;
      CHECK(tr.data_bytes <= 1 + static_cast<uint32_t>(round % 31));
    }
  }
  CHECK(accepted < 100);   // (1 in 256 random packets sums to 0 and has version 3 and a fitting length: rare)
}

static void test_commands() {
  for (int mec = 0; mec < 2; ++mec) {
    FakeEc fake;
    configure(fake, mec != 0, mec ? MapAt::Emi : MapAt::Port900);
    fake.set_fans(2);
    ChromeEcStack stack(fake);
    CHECK(stack.start(true));
    ChromeEc& ec = *stack.ec();

    // GET_CMD_VERSIONS (version 1 with a 16-bit command), cached.
    CHECK(ec.command_versions(command::kPwmSetFanDuty).value_or(0u) == 3u);
    CHECK(ec.command_versions(command::kFrameworkChargeLimitControl).value_or(0u) == 1u);
    CHECK(!ec.command_versions(0x1234));

    // Fans.
    CHECK(ec.set_fan_target_rpm(Optional<uint8_t>(uint8_t{1}), 3200) && fake.fan_rpm_target[1] == 3200 && !fake.fan_auto[1] && fake.fan_auto[0]);
    CHECK(ec.set_fan_target_rpm(Optional<uint8_t>(), 2500) && fake.fan_rpm_target[0] == 2500 && fake.fan_rpm_target[1] == 2500 && fake.all_fans_rpm_calls == 1);
    CHECK(ec.fan_target_rpm().value_or(0u) == 2500u);
    CHECK(ec.set_fan_duty(Optional<uint8_t>(uint8_t{0}), 60) && fake.fan_duty[0] == 60 && fake.last_version == 1);
    CHECK(!ec.set_fan_duty(Optional<uint8_t>(uint8_t{0}), 101));
    CHECK(!ec.set_fan_duty(Optional<uint8_t>(uint8_t{5}), 50));   // no such fan: the EC says invalid param
    CHECK(ec.set_fan_automatic(Optional<uint8_t>(uint8_t{0})) && fake.fan_auto[0] && !fake.fan_auto[1]);
    CHECK(ec.set_fan_automatic(Optional<uint8_t>()) && fake.fan_auto[1] && fake.all_fans_auto_calls == 1);
    const uint32_t versions_calls = fake.cmd_versions_calls;
    CHECK(ec.set_fan_duty(Optional<uint8_t>(uint8_t{1}), 10) && ec.set_fan_duty(Optional<uint8_t>(uint8_t{1}), 20) && fake.cmd_versions_calls == versions_calls);

    // Keyboard backlight.
    Optional<EcKeyboardBacklight> k = ec.keyboard_backlight();
    CHECK(k && k->percent == 30 && k->enabled);
    CHECK(ec.set_keyboard_backlight(75) && fake.kbd == 75 && !ec.set_keyboard_backlight(101));
    ChromeEcKeyboardBacklight* kb = stack.keyboard_backlight();
    uint8_t p = 0;
    CHECK(kb && kb->steps() == 101 && kb->percent(&p) && p == 75);
    CHECK(kb->set_percent(250) && fake.kbd == 100);

    // LEDs.
    Optional<EcLedRange> range = ec.led_range(EcLed::Battery);
    CHECK(range && range->max[1] == 1 && range->max[0] == 0 && range->max[5] == 1 && fake.last_version == 1);
    const uint8_t white[6] = {0, 0, 0, 0, 1, 0};
    CHECK(ec.set_led(EcLed::Power, white) && !fake.led_auto[1] && fake.led_brightness[1][4] == 1);
    CHECK(ec.set_led_automatic(EcLed::Power) && fake.led_auto[1]);

    // Charge state, sensor names.
    Optional<EcChargeState> cs = ec.charge_state();
    CHECK(cs && cs->ac && cs->charge_voltage_mv == 17400 && cs->state_of_charge == 79 && fake.last_params_size == 9);
    FixedString<33> name;
    CHECK(ec.temperature_sensor_name(1, &name) && strcmp(name.c_str(), "F75303_CPU") == 0 && !ec.temperature_sensor_name(9, &name));

    // The memory map.
    Optional<EcTemperatureReading> t0 = ec.temperature(0);
    CHECK(t0 && t0->present && t0->valid && t0->millicelsius == 36850);
    Optional<EcTemperatureReading> t2 = ec.temperature(2);
    CHECK(t2 && t2->present && !t2->valid);
    Optional<EcTemperatureReading> t3 = ec.temperature(3);
    CHECK(t3 && !t3->present);
    Optional<EcTemperatureReading> t16 = ec.temperature(16);
    CHECK(t16 && t16->valid && t16->millicelsius == 26850);
    CHECK(!ec.temperature(24));
    CHECK(ec.fan_rpm(0).value_or(0u) == 2000u && ec.fan_rpm(1).value_or(0u) == 2100u && !ec.fan_rpm(2) && !ec.fan_rpm(9));
    fake.put16(memmap::kFan, memmap::kFanStalled);
    CHECK(ec.fan_rpm(0).has_value() && *ec.fan_rpm(0) == 0);
    fake.put16(memmap::kFan, 2000);
    Optional<EcBatteryInfo> b = ec.battery();
    CHECK(b && b->present && b->ac_present && b->charging && !b->discharging && b->voltage_mv == 16800 && b->full_charge_mah == 3500);
    CHECK(b && b->cycle_count == 42 && strcmp(b->manufacturer.c_str(), "NVT") == 0 && strcmp(b->model.c_str(), "FRANGWA") == 0 && b->percent() == 78);
    CHECK(ec.switches().value_or(0) == memmap::kSwitchLidOpen);
    fake.map[memmap::kBatteryVersion] = 0;
    CHECK(!ec.battery());
    fake.map[memmap::kBatteryVersion] = 1;
    // Thermal version 1: the second sensor bank is not believed.
    fake.map[memmap::kThermalVersion] = 1;
    {
      ChromeEcStack again(fake);
      CHECK(again.start(true) && !again.ec()->temperature(16));
    }
    fake.map[memmap::kThermalVersion] = 2;

    // Framework.
    FrameworkEc* fw = stack.framework();
    CHECK(fw != nullptr);
    if (!fw) continue;
    Optional<FrameworkChargeLimit> limit = fw->charge_limit();
    CHECK(limit && limit->max_percent == 100);
    CHECK(fw->set_charge_limit(80) && fake.charge_max == 80 && fw->charge_limit()->max_percent == 80);
    CHECK(!fw->set_charge_limit(10) && !fw->set_charge_limit(101) && fake.charge_max == 80);
    CHECK(fw->set_charge_limit(100) && fake.charge_max == 0 && fw->charge_limit()->max_percent == 100);
    Optional<FrameworkPrivacySwitches> ps = fw->privacy_switches();
    CHECK(ps && ps->microphone_connected && !ps->camera_connected);
    Optional<FrameworkChassisIntrusion> ci = fw->chassis_intrusion();
    CHECK(ci && ci->ever_opened && !ci->coin_cell_ever_removed && ci->open_count == 3 && ci->open_count_powered_off == 1);
    Optional<FrameworkFingerprintLed> fp = fw->fingerprint_led();
    CHECK(fp && fp->level == 1 && fp->percent == 40);
    CHECK(fw->set_fingerprint_led(FpLedLevel::Low) && fw->fingerprint_led()->percent == 15);
    CHECK(!fw->set_fingerprint_led(static_cast<FpLedLevel>(7)));
    Optional<FrameworkInputDeck> deck = fw->input_deck();
    CHECK(deck && deck->state == 5 && deck->board_ids[0] == 1 && deck->board_ids[7] == 8);
    Optional<FrameworkExpansionBay> bay = fw->expansion_bay();
    CHECK(bay && bay->enabled && bay->door_closed && !bay->fault && bay->board_id_0 == 0x14 && bay->board_id_1 == 0x15);

    // Properties.
    struct Sink final : IPropertySink {
      std::string text;
      void add(StringView n, StringView v) override { text.append(n.data(), n.size()).append("=").append(v.data(), v.size()).append("\n"); }
    } sink;
    stack.describe(sink);
    CHECK(sink.text.find("hx30_v0.0.1-7a61a89") != std::string::npos && sink.text.find("Battery charge limit=100 %") != std::string::npos);
    CHECK(sink.text.find("Camera=cut off") != std::string::npos && sink.text.find(mec ? "Microchip EMI" : "LPC, protocol 3") != std::string::npos);
  }
  {
    // Not a Framework EC: no Framework layer, the standard parts still work.
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    fake.framework = false;
    ChromeEcStack stack(fake);
    CHECK(stack.start(true) && stack.framework() == nullptr && !stack.charge_limit() && !stack.privacy_switches() && stack.monitor() != nullptr);
    MemoryStore store;
    store.value = 80;
    CHECK(!stack.set_charge_limit(80, &store) && !stack.restore_charge_limit(store) && store.stores == 0);
  }
}

static void test_monitor() {
  for (uint32_t fans = 1; fans <= 2; ++fans) {
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    fake.set_fans(fans);
    ChromeEcStack stack(fake);
    CHECK(stack.start(false));
    ChromeEcHardwareMonitor* m = stack.monitor();
    CHECK(m != nullptr);
    if (!m) continue;
    CHECK(m->channel_count(MonitorChannel::Temperature) == 4 && m->channel_count(MonitorChannel::Fan) == fans && m->channel_count(MonitorChannel::Pwm) == fans);
    CHECK(m->channel_count(MonitorChannel::Voltage) == 0 && !m->voltage_millivolts(0));
    CHECK(strcmp(m->label(MonitorChannel::Temperature, 0), "F75303_Local") == 0 && strcmp(m->label(MonitorChannel::Temperature, 3), "Battery") == 0);
    CHECK(strcmp(m->label(MonitorChannel::Fan, 0), "Fan 1") == 0 && m->label(MonitorChannel::Fan, fans) == nullptr);
    CHECK(m->role(MonitorChannel::Temperature, 1) == SensorRole::Cpu && m->role(MonitorChannel::Temperature, 0) == SensorRole::System);
    CHECK(m->role(MonitorChannel::Fan, 0) == (fans == 1 ? SensorRole::Cpu : SensorRole::Other));
    CHECK(m->temperature_millicelsius(1).value_or(0) == 51850 && !m->temperature_millicelsius(2) && !m->temperature_millicelsius(9));
    CHECK(m->fan_rpm(0).value_or(0u) == 2000u && !m->fan_rpm(fans));

    Optional<PwmState> s = m->pwm(0);
    CHECK(s && s->mode == PwmMode::Automatic && !s->duty_known);
    CHECK(!m->set_pwm_mode(0, PwmMode::Manual));   // no duty to keep yet
    CHECK(m->set_pwm_duty(0, 128) && fake.fan_duty[0] == 50 && !fake.fan_auto[0]);
    CHECK(fans == 1 ? fake.all_fans_duty_calls == 1 : fake.last_version == 1);   // one fan: version 0 (every fan); two: version 1 by index
    s = m->pwm(0);
    CHECK(s && s->mode == PwmMode::Manual && s->duty_known && s->duty == 128);
    CHECK(m->set_pwm_mode(0, PwmMode::Manual));
    CHECK(m->set_pwm_mode(0, PwmMode::Automatic) && fake.fan_auto[0]);
    s = m->pwm(0);
    CHECK(s && s->mode == PwmMode::Automatic && !s->duty_known);
    CHECK(m->set_pwm_mode(0, PwmMode::Manual) == false);
    CHECK(m->set_fan_target_rpm(0, 4100) && fake.fan_rpm_target[0] == 4100 && m->pwm(0)->mode == PwmMode::Manual);
    CHECK(!m->set_pwm_duty(fans, 10) && !m->set_pwm_mode(fans, PwmMode::Automatic) && !m->pwm(fans));
    CHECK(m->set_pwm_duty(0, 255) && fake.fan_duty[0] == 100);
    CHECK(m->set_pwm_duty(0, 0) && fake.fan_duty[0] == 0);
  }
  {
    // An EC with no sensors and no fans: no monitor.
    FakeEc fake;
    configure(fake, false, MapAt::Port900);
    fake.set_fans(0);
    for (uint32_t i = 0; i < 16; ++i) fake.map[i] = memmap::kTempNotPresent;
    fake.map[memmap::kTempSensorB] = memmap::kTempNotPresent;
    ChromeEcStack stack(fake);
    CHECK(stack.start(false) && stack.monitor() == nullptr);
  }
}

static void test_charge_limit_store() {
  FakeEc fake;
  configure(fake, true, MapAt::Emi);
  ChromeEcStack stack(fake);
  CHECK(stack.start(true));
  MemoryStore store;
  CHECK(!stack.restore_charge_limit(store));   // nothing kept
  CHECK(stack.set_charge_limit(70, &store) && store.value.value_or(0) == 70 && store.stores == 1 && fake.charge_max == 70);
  CHECK(!stack.set_charge_limit(5, &store) && store.stores == 1);
  // The EC forgets (reset); the start-up restore gives it back.
  fake.charge_max = 0;
  ChromeEcStack after_boot(fake);
  CHECK(after_boot.start(true) && after_boot.restore_charge_limit(store) && fake.charge_max == 70);
  CHECK(after_boot.charge_limit()->max_percent == 70);
  // A stored value the EC would not take (corrupt database): refused, the EC keeps what it has.
  store.value = 3;
  CHECK(!after_boot.restore_charge_limit(store) && fake.charge_max == 70);
}

static void test_firmware_lock() {
  FakeEc fake;
  configure(fake, false, MapAt::Port900);
  CountingLock lock;
  ChromeEcStack stack(fake);
  CHECK(stack.start(true, &lock));
  const int after_start = lock.locks;
  CHECK(after_start > 0 && lock.depth == 0 && lock.max_depth == 1);
  CHECK(stack.ec()->hello(1).has_value() && lock.locks == after_start + 1 && lock.depth == 0);
  CHECK(stack.ec()->fan_rpm(0).has_value() && lock.locks == after_start + 2);
}

static void test_threads() {
  for (int mec = 0; mec < 2; ++mec) {
    FakeEc fake;
    configure(fake, mec != 0, mec ? MapAt::Emi : MapAt::Port900);
    fake.set_fans(2);
    ChromeEcStack stack(fake);
    CHECK(stack.start(true));
    ChromeEc* ec = stack.ec();
    if (!ec) continue;
    fake.overlaps = 0;
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
      threads.emplace_back([&, t] {
        for (uint32_t i = 0; i < 1500; ++i) {
          const uint32_t v = (static_cast<uint32_t>(t) << 24) | i;
          if (ec->hello(v).value_or(0u) != v + 0x01020304u) ++wrong;
          switch (i % 4) {
            case 0: if (!ec->fan_rpm(static_cast<uint32_t>(t))) ++wrong; break;
            case 1: if (!ec->set_fan_duty(Optional<uint8_t>(static_cast<uint8_t>(t)), static_cast<uint8_t>(i % 100))) ++wrong; break;
            case 2: { Optional<EcTemperatureReading> r = ec->temperature(0); if (!r || r->millicelsius != 36850) ++wrong; break; }
            case 3: if (!stack.framework() || !stack.framework()->privacy_switches()) ++wrong; break;
          }
        }
      });
    }
    for (std::thread& th : threads) th.join();
    CHECK(wrong == 0);
    CHECK(fake.overlaps == 0);
  }
}

int main() {
  test_framing();
  test_transports();
  test_detection_refuses();
  test_untrusted_responses();
  test_commands();
  test_monitor();
  test_charge_limit_store();
  test_firmware_lock();
  test_threads();
  if (g_failed) {
    printf("ChromeEcTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("ChromeEcTest: all tests passed\n");
  return 0;
}

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

// UCSI: the decoders of every answer (UCSI 1.0, 1.2, 2.0 and 2.1 layouts, truncated and bit-flipped data), the core against a model policy
// manager (start-up, connector changes, busy / erroring / unsupported / silent PPMs, a clock that stands still), the system object (notifications,
// restarts, readers on other threads while it publishes) and the ACPI transport against AML shaped like a laptop's (a USBC000 device whose _DSM
// copies the mailbox to and from an "embedded controller").
#include <atomic>
#include <deque>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include "AcpiTestSupport.hpp"
#include "Hardware/Acpi/AcpiResources.hpp"
#include "Hardware/UsbTypeC/AcpiUcsiTransport.hpp"
#include "Hardware/UsbTypeC/UcsiCore.hpp"
#include "Hardware/UsbTypeC/UsbTypeCSystem.hpp"

using namespace hardware::ucsi;
using hardware::UcsiCore;
using Result = UcsiCore::Result;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Helpers

static void set_bits(Bytes& d, uint32_t offset, uint32_t width, uint64_t value) {
  for (uint32_t i = 0; i < width; ++i) {
    const uint32_t bit = offset + i;
    if (bit / 8 >= d.size()) d.resize(bit / 8 + 1, 0);
    if (value & (1ull << i)) d[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    else d[bit / 8] &= static_cast<uint8_t>(~(1u << (bit % 8)));
  }
}
static void put_le(Bytes& d, uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) d.push_back(static_cast<uint8_t>(value >> (8 * i))); }
static ConstByteView view(const Bytes& d) { return ConstByteView(d.data(), d.size()); }

static uint32_t fixed_pdo(uint32_t mv, uint32_t ma, uint32_t flags = 0) { return flags | ((mv / 50) << 10) | (ma / 10); }
static uint32_t pps_pdo(uint32_t min_mv, uint32_t max_mv, uint32_t ma) { return (3u << 30) | ((max_mv / 100) << 17) | ((min_mv / 100) << 8) | (ma / 50); }
static uint32_t fixed_rdo(uint32_t position, uint32_t op_ma, uint32_t max_ma) { return (position << 28) | ((op_ma / 10) << 10) | (max_ma / 10); }

struct FakeClock final : hardware::IUcsiClock {
  uint64_t now = 1000000;
  bool frozen = false;
  uint64_t sleeps = 0;
  uint64_t now_us() override { return now; }
  void sleep_us(uint32_t us) override { ++sleeps; if (!frozen) now += us; }
};

struct RealClock final : hardware::IUcsiClock {
  uint64_t now_us() override { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_us(uint32_t us) override { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// A model platform policy manager.

struct FakePpm {
  struct Port {
    bool connected = false;
    uint8_t partner_type = 0;          // UCSI encoding: 1 DFP, 2 UFP, ...
    bool provider = false;             // this side gives power
    uint8_t opmode = 0;                // 3 = PD
    uint32_t rdo = 0;
    bool usb = true, alt = false, usb4_gen3 = false;
    bool flipped = false;
    uint16_t pd_version = 0x0300;
    uint16_t vbus_reading = 0;         // 2.1: in voltage scale units
    std::vector<uint32_t> partner_source_pdos, own_source_pdos;
    std::vector<std::pair<uint16_t, uint32_t>> alt_modes;
    Bytes cable = {0x2B, 0x00, 100, 0x14, 0x01};   // 10 Gb/s (mantissa 10, exponent 3), 5 A, Type-C plug, 1 m
    uint16_t change = 0;
  };
  uint16_t version = kVersion1_2;
  uint32_t capacity = 16;
  uint8_t connector_total = 2;
  uint32_t features = kFeatureAltModeDetails | kFeaturePdoDetails | kFeatureCableDetails;
  Port ports[128];

  // State.
  uint32_t cci = 0;
  uint8_t message_in[256] = {};
  uint64_t notify_mask = 0;
  int resetting = -1;                  // looks at CCI left before the reset completes
  std::deque<uint8_t> changes;         // connectors that changed, oldest first
  bool waiting_ack = false;
  uint16_t error = 0;

  // Misbehaviour.
  int reset_looks = 2;
  bool never_reset = false;
  std::set<uint8_t> busy_forever, not_supported;
  std::map<uint8_t, uint16_t> errors;  // command -> GET_ERROR_STATUS
  std::function<void(uint8_t command, Bytes& data)> corrupt;
  int extra_length = 0;                // added to the CCI length (lies about the size)

  std::vector<uint64_t> log;           // every CONTROL written
  uint32_t acks = 0, cancels = 0, connector_acks = 0;

  std::mutex lock;                     // (the concurrency test changes ports from another thread)

  uint32_t read_cci() {
    std::lock_guard<std::mutex> g(lock);
    if (resetting > 0 && --resetting == 0 && !never_reset) {
      cci = kCciResetComplete;
      resetting = -1;
    }
    uint32_t v = cci;
    if (!changes.empty() && (notify_mask & kNotifyConnectChange) && resetting < 0) v |= uint32_t{changes.front()} << 1;
    return v;
  }

  void plug(uint32_t index, const Port& p, uint16_t change_bits = kChangeConnect) {
    std::lock_guard<std::mutex> g(lock);
    ports[index] = p;
    ports[index].change = change_bits;
    const uint8_t number = static_cast<uint8_t>(index + 1);
    for (uint8_t c : changes) if (c == number) return;
    changes.push_back(number);
  }

  void complete(uint8_t command, Bytes data) {
    if (corrupt) corrupt(command, data);
    const uint32_t n = static_cast<uint32_t>(data.size() > capacity ? capacity : data.size());
    for (uint32_t i = 0; i < n; ++i) message_in[i] = data[i];
    const int reported = static_cast<int>(data.size()) + extra_length;
    cci = kCciCommandComplete | (static_cast<uint32_t>(reported < 0 ? 0 : reported > 255 ? 255 : reported) << 8);
    waiting_ack = true;
  }

  Bytes connector_status(const Port& p) {
    Bytes d;
    const uint32_t size = version >= kVersion2_1 ? 19 : version >= kVersion2_0 ? 12 : version >= kVersion1_2 ? 11 : 9;
    d.resize(size, 0);
    set_bits(d, 0, 16, p.change);
    set_bits(d, 16, 3, p.connected ? p.opmode : 0);
    set_bits(d, 19, 1, p.connected);
    set_bits(d, 20, 1, p.provider);
    set_bits(d, 21, 1, p.usb);
    set_bits(d, 22, 1, p.alt);
    set_bits(d, 23, 1, p.usb4_gen3);
    set_bits(d, 29, 3, p.partner_type);
    set_bits(d, 32, 32, p.rdo);
    set_bits(d, 64, 2, 1);
    if (size >= 11) set_bits(d, 70, 16, p.pd_version);
    if (size >= 12) set_bits(d, 86, 1, p.flipped), set_bits(d, 87, 1, 1);
    if (size >= 19 && p.vbus_reading) {
      set_bits(d, 89, 1, 1);
      set_bits(d, 90, 3, 2);          // current scale: 10 mA
      set_bits(d, 93, 16, 300);       // peak 3 A
      set_bits(d, 109, 16, 200);      // average 2 A
      set_bits(d, 125, 4, 4);         // voltage scale: 20 mV
      set_bits(d, 129, 16, p.vbus_reading);
    }
    d.resize(size);
    return d;
  }

  void control(uint64_t c) {
    std::lock_guard<std::mutex> g(lock);
    log.push_back(c);
    const uint8_t command = static_cast<uint8_t>(c);
    if (command == kPpmReset) {
      cci = 0;
      resetting = reset_looks > 0 ? reset_looks : 1;
      notify_mask = 0;
      changes.clear();
      waiting_ack = false;
      return;
    }
    if (resetting >= 0) return;        // (nothing else is taken while resetting)
    if (command == kAckCcCi) {
      ++acks;
      if (c & kAckCommandComplete) waiting_ack = false;
      if ((c & kAckConnectorChange) && !changes.empty()) {
        ++connector_acks;
        ports[changes.front() - 1].change = 0;
        changes.pop_front();
      }
      cci = kCciAckComplete;
      return;
    }
    if (command == kCancel) {
      ++cancels;
      cci = kCciCancelComplete | kCciCommandComplete;
      return;
    }
    if (busy_forever.count(command)) { cci = kCciBusy; return; }
    if (not_supported.count(command)) { cci = kCciCommandComplete | kCciNotSupported; return; }
    if (auto e = errors.find(command); e != errors.end()) { error = e->second; cci = kCciCommandComplete | kCciError; return; }
    const uint32_t number = (c >> 16) & 0x7F;
    Bytes d;
    switch (command) {
      case kSetNotificationEnable: notify_mask = c & 0xFFFF0000ull; complete(command, d); return;
      case kGetErrorStatus: put_le(d, error, 2); d.resize(16, 0); complete(command, d); return;
      case kGetCapability:
        put_le(d, 0x00000044, 4);
        d.push_back(connector_total);
        put_le(d, features, 3);
        d.push_back(2);                // alternate modes
        d.push_back(0);
        put_le(d, 0x0120, 2);
        put_le(d, 0x0300, 2);
        put_le(d, 0x0200, 2);
        complete(command, d);
        return;
      default: break;
    }
    if (command == kGetAlternateModes) {
      const uint32_t recipient = (c >> 16) & 7, n = (c >> 24) & 0x7F, offset = (c >> 32) & 0xFF;
      if (n < 1 || n > connector_total) { error = kErrorNonExistentConnector; cci = kCciCommandComplete | kCciError; return; }
      const Port& p = ports[n - 1];
      if (recipient == 1 && offset < p.alt_modes.size()) {
        put_le(d, p.alt_modes[offset].first, 2);
        put_le(d, p.alt_modes[offset].second, 4);
      }
      complete(command, d);
      return;
    }
    if (number < 1 || number > connector_total) { error = kErrorNonExistentConnector; cci = kCciCommandComplete | kCciError; return; }
    Port& p = ports[number - 1];
    switch (command) {
      case kGetConnectorCapability: d = {0xE4, 0x3F}; break;
      case kGetConnectorStatus: d = connector_status(p); break;
      case kGetPdos: {
        const bool partner = c & (1ull << 23);
        const uint32_t offset = (c >> 24) & 0xFF, count = ((c >> 32) & 3) + 1;
        const std::vector<uint32_t>& list = partner ? p.partner_source_pdos : p.own_source_pdos;
        for (uint32_t i = offset; i < offset + count && i < list.size(); ++i) put_le(d, list[i], 4);
        break;
      }
      case kGetCableProperty: d = p.cable; break;
      default: cci = kCciCommandComplete | kCciNotSupported; return;
    }
    complete(command, d);
  }

  uint32_t count(uint8_t command) {
    uint32_t n = 0;
    for (uint64_t c : log) n += static_cast<uint8_t>(c) == command;
    return n;
  }
};

struct FakeTransport final : hardware::IUcsiTransport {
  FakePpm& ppm;
  bool broken = false;
  explicit FakeTransport(FakePpm& p) : ppm(p) {}
  bool read_version(uint16_t* v) override { if (broken) return false; *v = ppm.version; return true; }
  bool read_cci(uint32_t* cci) override { if (broken) return false; *cci = ppm.read_cci(); return true; }
  bool read_message_in(ByteView out) override {
    if (broken || out.size() > ppm.capacity) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = ppm.message_in[i];
    return true;
  }
  bool write_message_out(ConstByteView) override { return !broken; }
  bool write_control(uint64_t c) override { if (broken) return false; ppm.control(c); return true; }
  uint32_t message_capacity() const override { return ppm.capacity; }
};

// A charger on connector 1: 5/9/15/20 V fixed, 3.3-21 V PPS, PD contract on object 4 (20 V 3.25 A); DisplayPort and Thunderbolt modes.
static FakePpm::Port charger_dock() {
  FakePpm::Port p;
  p.connected = true;
  p.partner_type = 2;                  // a UFP: we are the host
  p.provider = false;                  // we take power
  p.opmode = 3;
  p.alt = true;
  p.partner_source_pdos = {fixed_pdo(5000, 3000, (1u << 29) | (1u << 26)), fixed_pdo(9000, 3000), fixed_pdo(15000, 3000), fixed_pdo(20000, 3250),
                           pps_pdo(3300, 21000, 3000)};
  p.rdo = fixed_rdo(4, 3250, 3250);
  p.alt_modes = {{kSvidDisplayPort, 0x00001C46u}, {kSvidThunderbolt, 0x00000001u}};   // DP: receptacle, UFP_D pins C, D, E
  return p;
}

static UcsiCore::Timing fast_timing() {
  UcsiCore::Timing t;
  t.command_timeout_us = 100000;
  t.reset_timeout_us = 200000;
  t.cancel_timeout_us = 20000;
  return t;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Decoders

static void test_decoders() {
  // GET_CAPABILITY: 8 feature bits before 2.0, 24 after
  Bytes cap;
  put_le(cap, 0x44, 4);
  cap.push_back(0x82);                 // 2 connectors (bit 7 is not part of the count)
  put_le(cap, 0x010334, 3);
  cap.push_back(5);
  cap.push_back(0);
  put_le(cap, 0x0120, 2);
  put_le(cap, 0x0310, 2);
  put_le(cap, 0x0210, 2);
  Capability c;
  CHECK(decode_capability(view(cap), kVersion1_2, &c) && c.connectors == 2 && c.features == 0x34 && c.alt_modes == 5 && c.pd_version == 0x0310 &&
        c.typec_version == 0x0210 && c.bc_version == 0x0120 && c.attributes == 0x44);
  CHECK(decode_capability(view(cap), kVersion2_0, &c) && c.features == 0x010334);
  CHECK(!decode_capability(ConstByteView(cap.data(), 7), kVersion1_2, &c));
  CHECK(decode_capability(ConstByteView(cap.data(), 8), kVersion1_2, &c) && c.alt_modes == 0 && c.pd_version == 0);   // (the rest reads as zero)

  hardware::UsbTypeCConnectorCaps caps;
  const Bytes concap = {0xE4, 0x3F};
  CHECK(decode_connector_capability(view(concap), &caps) && caps.drp && caps.usb2 && caps.usb3 && caps.alt_modes && !caps.dfp && caps.provider &&
        caps.consumer && caps.swap_to_sink);
  CHECK(!decode_connector_capability(ConstByteView(concap.data(), 1), &caps));

  // GET_CONNECTOR_STATUS in each layout
  FakePpm ppm;
  FakePpm::Port p = charger_dock();
  p.flipped = true;
  p.usb4_gen3 = true;
  p.vbus_reading = 1000;               // x 20 mV = 20 V
  struct Layout { uint16_t version; uint32_t size; } layouts[] = {{0x0100, 9}, {kVersion1_2, 11}, {kVersion2_0, 12}, {kVersion2_1, 19}};
  for (const Layout& l : layouts) {
    ppm.version = l.version;
    p.change = kChangeConnect | kChangePartner;
    const Bytes s = ppm.connector_status(p);
    CHECK(s.size() == l.size);
    hardware::UsbTypeCConnector con;
    uint16_t change = 0;
    CHECK(decode_connector_status(view(s), l.version, &con, &change));
    CHECK(change == (kChangeConnect | kChangePartner) && con.connected && con.partner == hardware::UsbTypeCPartner::Device &&
          con.data_role == hardware::UsbTypeCDataRole::Host && con.power_role == hardware::UsbTypeCPowerRole::Sink &&
          con.power_mode == hardware::UsbTypeCPowerMode::PowerDelivery && con.partner_alt_mode && con.partner_usb && con.contract.rdo == p.rdo &&
          con.battery_charging == 1);
    CHECK(con.pd_revision == (l.version >= kVersion1_2 ? 0x0300 : 0));
    CHECK(con.orientation_known == (l.version >= kVersion2_0) && con.flipped == (l.version >= kVersion2_0) && con.sink_path == (l.version >= kVersion2_0));
    CHECK(con.partner_usb4_gen3 == (l.version >= kVersion2_0));
    CHECK(con.power_reading_valid == (l.version >= kVersion2_1));
    if (l.version >= kVersion2_1) CHECK(con.vbus_mv == 20000 && con.vbus_peak_ma == 3000 && con.vbus_avg_ma == 2000);
    // the same bytes read as an older version: the newer fields stay off
    CHECK(decode_connector_status(view(s), 0x0100, &con, &change) && con.pd_revision == 0 && !con.orientation_known && !con.power_reading_valid);
    // shorter than the version says: only what is there
    if (l.size > 9) CHECK(decode_connector_status(ConstByteView(s.data(), 9), l.version, &con, &change) && con.pd_revision == 0 && !con.orientation_known);
    CHECK(!decode_connector_status(ConstByteView(s.data(), 8), l.version, &con, &change));
  }
  // nothing connected: no partner, whatever the other bits say
  {
    FakePpm::Port empty = charger_dock();
    empty.connected = false;
    Bytes s = ppm.connector_status(empty);
    set_bits(s, 29, 3, 2);
    set_bits(s, 32, 32, 0x12345678);
    hardware::UsbTypeCConnector con;
    uint16_t change = 0;
    CHECK(decode_connector_status(view(s), kVersion2_0, &con, &change) && !con.connected && con.partner == hardware::UsbTypeCPartner::None &&
          con.power_role == hardware::UsbTypeCPowerRole::None && con.contract.rdo == 0);
    set_bits(s, 19, 1, 1);
    set_bits(s, 29, 3, 7);             // a partner type with no meaning
    set_bits(s, 16, 3, 7);
    CHECK(decode_connector_status(view(s), kVersion2_0, &con, &change) && con.partner == hardware::UsbTypeCPartner::Unknown &&
          con.power_mode == hardware::UsbTypeCPowerMode::Unknown);
    set_bits(s, 29, 3, 1);
    CHECK(decode_connector_status(view(s), kVersion2_0, &con, &change) && con.partner == hardware::UsbTypeCPartner::Host &&
          con.data_role == hardware::UsbTypeCDataRole::Device);
  }

  // PDOs
  hardware::UsbPdo pdo = decode_pdo(fixed_pdo(20000, 3250));
  CHECK(pdo.kind == hardware::UsbPdoKind::Fixed && pdo.min_mv == 20000 && pdo.max_mv == 20000 && pdo.max_ma == 3250 && pdo.max_mw == 65000);
  pdo = decode_pdo(fixed_pdo(5000, 3000, (1u << 29) | (1u << 26) | (1u << 25)));
  CHECK(pdo.dual_role_power && pdo.usb_communication && pdo.dual_role_data && !pdo.unconstrained_power);
  pdo = decode_pdo((1u << 30) | (420u << 20) | (100u << 10) | 240u);   // battery 5-21 V 60 W
  CHECK(pdo.kind == hardware::UsbPdoKind::Battery && pdo.min_mv == 5000 && pdo.max_mv == 21000 && pdo.max_mw == 60000 && pdo.max_ma == 0);
  pdo = decode_pdo((2u << 30) | (240u << 20) | (100u << 10) | 150u);   // variable 5-12 V 1.5 A
  CHECK(pdo.kind == hardware::UsbPdoKind::Variable && pdo.min_mv == 5000 && pdo.max_mv == 12000 && pdo.max_ma == 1500);
  pdo = decode_pdo(pps_pdo(3300, 21000, 3000));
  CHECK(pdo.kind == hardware::UsbPdoKind::Pps && pdo.min_mv == 3300 && pdo.max_mv == 21000 && pdo.max_ma == 3000);
  pdo = decode_pdo((3u << 30) | (1u << 28) | (280u << 17) | (150u << 8) | 140u);   // EPR AVS 15-28 V 140 W
  CHECK(pdo.kind == hardware::UsbPdoKind::EprAvs && pdo.max_mv == 28000 && pdo.min_mv == 15000 && pdo.max_mw == 140000);
  CHECK(decode_pdo(0xF0000000u).kind == hardware::UsbPdoKind::Unknown);

  Bytes pdos;
  for (uint32_t v : charger_dock().partner_source_pdos) put_le(pdos, v, 4);
  hardware::UsbPdo list[7];
  CHECK(decode_pdos(view(pdos), ArrayView<hardware::UsbPdo>(list)) == 5);
  CHECK(decode_pdos(ConstByteView(pdos.data(), 7), ArrayView<hardware::UsbPdo>(list)) == 1);   // (whole objects only)
  CHECK(decode_pdos(view(pdos), ArrayView<hardware::UsbPdo>(list, 2)) == 2);

  // contracts
  hardware::UsbPdo src[5];
  CHECK(decode_pdos(view(pdos), ArrayView<hardware::UsbPdo>(src)) == 5);
  hardware::UsbPdContract k = decode_contract(fixed_rdo(4, 3250, 3250), ArrayView<const hardware::UsbPdo>(src, 5));
  CHECK(k.valid && k.object_position == 4 && k.voltage_mv == 20000 && k.current_ma == 3250 && k.max_current_ma == 3250 && k.power_mw == 65000);
  k = decode_contract((5u << 28) | (450u << 9) | 40u, ArrayView<const hardware::UsbPdo>(src, 5));   // PPS 9 V 2 A
  CHECK(k.valid && k.kind == hardware::UsbPdoKind::Pps && k.voltage_mv == 9000 && k.current_ma == 2000 && k.power_mw == 18000);
  k = decode_contract(fixed_rdo(7, 1000, 1000), ArrayView<const hardware::UsbPdo>(src, 5));   // beyond the list: current only
  CHECK(k.valid && k.voltage_mv == 0 && k.current_ma == 1000 && k.power_mw == 0);
  CHECK(!decode_contract(0, ArrayView<const hardware::UsbPdo>(src, 5)).valid);
  CHECK(!decode_contract(0x000FFFFF, ArrayView<const hardware::UsbPdo>(src, 5)).valid);    // object position 0

  // alternate modes
  Bytes alt;
  put_le(alt, kSvidDisplayPort, 2);
  put_le(alt, 0x00001C46u, 4);         // receptacle: UFP_D pins from bits 16-23 -> 0; plug would be bits 8-15
  put_le(alt, kSvidThunderbolt, 2);
  put_le(alt, 1, 4);
  hardware::UsbAltMode modes[4];
  CHECK(decode_alternate_modes(view(alt), ArrayView<hardware::UsbAltMode>(modes)) == 2);
  CHECK(modes[0].kind == hardware::UsbAltModeKind::DisplayPort && modes[0].dp_pin_assignments == 0 && modes[1].kind == hardware::UsbAltModeKind::Thunderbolt);
  Bytes plug;
  put_le(plug, kSvidDisplayPort, 2);
  put_le(plug, 0x00001C05u, 4);        // a plug (bit 6 clear): pins C, D, E in bits 8-15
  CHECK(decode_alternate_modes(view(plug), ArrayView<hardware::UsbAltMode>(modes)) == 1 && modes[0].dp_pin_assignments == 0x1C);
  CHECK(decode_alternate_modes(ConstByteView(alt.data(), 5), ArrayView<hardware::UsbAltMode>(modes)) == 0);
  Bytes zero(12, 0);
  CHECK(decode_alternate_modes(view(zero), ArrayView<hardware::UsbAltMode>(modes)) == 0);

  // cable
  hardware::UsbCable cable;
  CHECK(decode_cable_property(view(charger_dock().cable), &cable) && cable.valid && cable.speed_mbps == 10000 && cable.current_ma == 5000 &&
        cable.plug == hardware::UsbCablePlug::TypeC && cable.directional && !cable.active && cable.latency == 1);
  CHECK(!decode_cable_property(ConstByteView(charger_dock().cable.data(), 4), &cable) && !cable.valid);
  uint16_t err = 0;
  const Bytes es = {0x04, 0x00};
  CHECK(decode_error_status(view(es), &err) && err == kErrorInvalidParameter);
  CHECK(!decode_error_status(ConstByteView(es.data(), 1), &err));

  // control words
  CHECK(control_get_pdos(1, true, 4, 3, true) == (0x10ull | (1ull << 16) | (1ull << 23) | (4ull << 24) | (2ull << 32) | (1ull << 34)));
  CHECK(control_get_alternate_modes(AltModeRecipient::Sop, 2, 3, 1) == (0x0Cull | (1ull << 16) | (2ull << 24) | (3ull << 32)));
  CHECK(bit_field(view(es), 2, 1) == 1 && bit_field(view(es), 15, 8) == 0 && bit_field(ConstByteView(), 0, 32) == 0);
}

// Every decoder on every truncation and on random bit flips: never outside the data (the sanitizers watch), enums always in range.
static void test_decoder_fuzz() {
  FakePpm ppm;
  ppm.version = kVersion2_1;
  FakePpm::Port p = charger_dock();
  p.vbus_reading = 500;
  std::vector<Bytes> samples = {ppm.connector_status(p), charger_dock().cable, Bytes{0xE4, 0x3F}};
  Bytes pdos, alt, cap;
  for (uint32_t v : p.partner_source_pdos) put_le(pdos, v, 4);
  put_le(alt, kSvidDisplayPort, 2); put_le(alt, 0x1C46, 4); put_le(alt, kSvidThunderbolt, 2); put_le(alt, 1, 4);
  put_le(cap, 0x44, 4); cap.push_back(2); put_le(cap, 0x34, 3); cap.push_back(2); cap.push_back(0); put_le(cap, 0x0120, 2); put_le(cap, 0x0300, 2); put_le(cap, 0x0200, 2);
  samples.push_back(pdos);
  samples.push_back(alt);
  samples.push_back(cap);
  std::mt19937 rng(7);
  uint32_t runs = 0;
  auto run_all = [&](const Bytes& d) {
    ++runs;
    // (copied into an exact-size heap block, so the address sanitizer sees any read past the end)
    std::vector<uint8_t> exact(d.begin(), d.end());
    const ConstByteView v(exact.data(), exact.size());
    Capability c;
    (void)decode_capability(v, kVersion2_0, &c);
    CHECK(c.connectors <= 127);
    hardware::UsbTypeCConnectorCaps caps;
    (void)decode_connector_capability(v, &caps);
    for (uint16_t version : {uint16_t(0x0100), kVersion1_2, kVersion2_0, kVersion2_1}) {
      hardware::UsbTypeCConnector con;
      uint16_t change;
      if (decode_connector_status(v, version, &con, &change)) {
        CHECK(static_cast<uint8_t>(con.partner) <= static_cast<uint8_t>(hardware::UsbTypeCPartner::Unknown));
        CHECK(static_cast<uint8_t>(con.power_mode) <= static_cast<uint8_t>(hardware::UsbTypeCPowerMode::Unknown));
        CHECK(con.connected || con.partner == hardware::UsbTypeCPartner::None);
      }
    }
    hardware::UsbPdo list[7];
    const uint32_t n = decode_pdos(v, ArrayView<hardware::UsbPdo>(list));
    CHECK(n <= 7 && n <= v.size() / 4);
    for (uint32_t rdo : {0x4000ABCDu, 0xF0FFFFFFu, 0x10000000u}) {
      const hardware::UsbPdContract k = decode_contract(rdo, ArrayView<const hardware::UsbPdo>(list, n));
      CHECK(!k.valid || k.object_position != 0);
    }
    hardware::UsbAltMode modes[2];
    CHECK(decode_alternate_modes(v, ArrayView<hardware::UsbAltMode>(modes)) <= 2);
    hardware::UsbCable cable;
    (void)decode_cable_property(v, &cable);
    uint16_t e;
    (void)decode_error_status(v, &e);
  };
  for (const Bytes& s : samples) {
    for (size_t cut = 0; cut <= s.size(); ++cut) run_all(Bytes(s.begin(), s.begin() + static_cast<long>(cut)));
    for (int i = 0; i < 300; ++i) {
      Bytes f = s;
      const int flips = 1 + static_cast<int>(rng() % 4);
      for (int j = 0; j < flips && !f.empty(); ++j) f[rng() % f.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      run_all(f);
    }
  }
  CHECK(runs > 1000);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The core against the model

static void test_initialize(uint16_t version) {
  FakePpm ppm;
  ppm.version = version;
  ppm.capacity = version >= kVersion2_0 ? 256 : 16;
  ppm.ports[0] = charger_dock();
  ppm.ports[0].flipped = true;
  FakeTransport t(ppm);
  FakeClock clock;
  UcsiCore core(t, clock, fast_timing());
  hardware::UsbTypeCSnapshot s;
  const Result r = core.initialize(&s);
  CHECK(r == Result::Ok);
  CHECK(s.present && s.ucsi_version == version && s.connector_count == 2 && s.pd_version == 0x0300 && s.typec_version == 0x0200 && s.alt_mode_total == 2);
  // the sequence: reset, notifications for commands, capability, then each connector, then every notification
  CHECK(!ppm.log.empty() && static_cast<uint8_t>(ppm.log[0]) == kPpmReset);
  std::vector<uint8_t> commands;
  for (uint64_t c : ppm.log) if (static_cast<uint8_t>(c) != kAckCcCi) commands.push_back(static_cast<uint8_t>(c));
  const std::vector<uint8_t> expected = {kPpmReset, kSetNotificationEnable, kGetCapability, kGetConnectorCapability, kGetConnectorStatus, kGetPdos, kGetPdos,
                                         kGetAlternateModes, kGetAlternateModes, kGetAlternateModes, kGetCableProperty, kGetConnectorCapability,
                                         kGetConnectorStatus, kSetNotificationEnable};
  CHECK(commands == expected);
  CHECK(ppm.log[1] == (kSetNotificationEnable | kNotifyCommandComplete | kNotifyError));
  CHECK(ppm.log.back() == (kAckCcCi | kAckCommandComplete) && ppm.notify_mask == kNotifyAll);
  CHECK(ppm.acks == commands.size() - 1);    // every command but the reset acknowledged
  CHECK(!ppm.waiting_ack);
  const hardware::UsbTypeCConnector& a = s.connectors[0];
  CHECK(a.number == 1 && a.status_valid && a.connected && a.partner == hardware::UsbTypeCPartner::Device && a.data_role == hardware::UsbTypeCDataRole::Host &&
        a.power_role == hardware::UsbTypeCPowerRole::Sink && a.power_mode == hardware::UsbTypeCPowerMode::PowerDelivery);
  CHECK(a.caps.drp && a.caps.usb3 && a.caps.provider && a.caps.consumer);
  CHECK(a.source_pdo_count == 5 && a.source_pdos[3].max_mv == 20000 && a.source_pdos[4].kind == hardware::UsbPdoKind::Pps);
  CHECK(a.contract.valid && a.contract.voltage_mv == 20000 && a.contract.current_ma == 3250 && a.contract.power_mw == 65000);
  CHECK(a.alt_mode_count == 2 && a.alt_modes[0].kind == hardware::UsbAltModeKind::DisplayPort && a.alt_modes[1].kind == hardware::UsbAltModeKind::Thunderbolt);
  CHECK(a.cable.valid && a.cable.speed_mbps == 10000 && a.cable.current_ma == 5000);
  CHECK(a.orientation_known == (version >= kVersion2_0) && a.flipped == (version >= kVersion2_0));
  CHECK(a.pd_revision == (version >= kVersion1_2 ? 0x0300 : 0));
  const hardware::UsbTypeCConnector& b = s.connectors[1];
  CHECK(b.number == 2 && b.status_valid && !b.connected && b.partner == hardware::UsbTypeCPartner::None && b.source_pdo_count == 0 && !b.cable.valid);
  // GET_PDOS asked for the partner's source objects of connector 1, four then three
  uint32_t pdo_requests = 0;
  for (uint64_t c : ppm.log)
    if (static_cast<uint8_t>(c) == kGetPdos) {
      CHECK(((c >> 16) & 0x7F) == 1 && (c & (1ull << 23)) && (c & (1ull << 34)));
      CHECK(((c >> 24) & 0xFF) == (pdo_requests == 0 ? 0u : 4u));
      ++pdo_requests;
    }
  CHECK(pdo_requests == 2);
  // the text
  char line[160];
  hardware::usb_type_c_summary(a, ArrayView<char>(line));
  CHECK(strstr(line, "a device") && strstr(line, "20.0 V 3.25 A") && strstr(line, "DisplayPort") && strstr(line, "Thunderbolt"));
  hardware::usb_type_c_summary(b, ArrayView<char>(line));
  CHECK(strcmp(line, "nothing connected") == 0);
  char tiny[6];
  hardware::usb_type_c_summary(a, ArrayView<char>(tiny));
  CHECK(strlen(tiny) == 5);
  struct Sink final : hardware::IPropertySink {
    std::vector<std::pair<std::string, std::string>> lines;
    void add(StringView n, StringView v) override { lines.emplace_back(std::string(n.data(), n.size()), std::string(v.data(), v.size())); }
    std::string get(const char* n) { for (auto& l : lines) if (l.first == n) return l.second; return ""; }
  } sink;
  hardware::usb_type_c_describe(a, sink);
  CHECK(sink.get("Contract").find("20.0 V 3.25 A (65.0 W)") == 0 && sink.get("Power role").find("Sink") == 0 && sink.get("Source PDO 5").find("PPS 3.3-21.0 V 3.0 A") == 0);
  CHECK(sink.get("Cable").find("passive, 10000 Mb/s, 5.0 A") == 0 && sink.get("Data role") == "Host");
}

static void test_connector_changes() {
  FakePpm ppm;
  ppm.ports[0] = charger_dock();
  FakeTransport t(ppm);
  FakeClock clock;
  UcsiCore core(t, clock, fast_timing());
  hardware::UsbTypeCSnapshot s;
  CHECK(core.initialize(&s) == Result::Ok);
  bool changed = true;
  CHECK(core.service(&s, &changed) == Result::Ok && !changed);    // nothing happened
  // the charger goes away, a phone comes to connector 2 and takes power from us
  FakePpm::Port none;
  ppm.plug(0, none);
  FakePpm::Port phone;
  phone.connected = true;
  phone.partner_type = 2;
  phone.provider = true;
  phone.opmode = 3;
  phone.own_source_pdos = {fixed_pdo(5000, 3000), fixed_pdo(9000, 2000)};
  phone.rdo = fixed_rdo(2, 2000, 2000);
  ppm.plug(1, phone);
  const size_t before = ppm.log.size();
  CHECK(core.service(&s, &changed) == Result::Ok && changed);
  CHECK(!s.connectors[0].connected && s.connectors[0].source_pdo_count == 0 && !s.connectors[0].contract.valid && s.connectors[0].alt_mode_count == 0);
  const hardware::UsbTypeCConnector& b = s.connectors[1];
  CHECK(b.connected && b.power_role == hardware::UsbTypeCPowerRole::Source && b.source_pdo_count == 2 && b.contract.voltage_mv == 9000 &&
        b.contract.current_ma == 2000 && b.contract.power_mw == 18000);
  CHECK(ppm.changes.empty() && ppm.connector_acks == 2);
  // the change was acknowledged with GET_CONNECTOR_STATUS's completion; the source objects asked were our own (partner bit clear)
  bool combined = false, own = false;
  for (size_t i = before; i < ppm.log.size(); ++i) {
    if (ppm.log[i] == (kAckCcCi | kAckCommandComplete | kAckConnectorChange)) combined = true;
    if (static_cast<uint8_t>(ppm.log[i]) == kGetPdos && ((ppm.log[i] >> 16) & 0x7F) == 2 && !(ppm.log[i] & (1ull << 23))) own = true;
  }
  CHECK(combined && own);
  CHECK(core.service(&s, &changed) == Result::Ok && !changed);
  // a change of the contract only (no connect change): the details are read again
  FakePpm::Port lower = phone;
  lower.rdo = fixed_rdo(1, 1500, 3000);
  ppm.plug(1, lower, kChangePowerLevel);
  CHECK(core.service(&s, &changed) == Result::Ok && changed && s.connectors[1].contract.voltage_mv == 5000 && s.connectors[1].contract.current_ma == 1500);
  // a change on a connector the capability did not list is only acknowledged
  {
    std::lock_guard<std::mutex> g(ppm.lock);
    ppm.changes.push_back(5);
  }
  CHECK(core.service(&s, &changed) == Result::Ok && !changed && ppm.changes.empty());
  // a PPM that reports the same connector forever does not hold service() (one look per call)
  ppm.plug(0, none);
  const uint32_t statuses = ppm.count(kGetConnectorStatus);
  CHECK(core.service(&s, &changed) == Result::Ok);
  CHECK(ppm.count(kGetConnectorStatus) == statuses + 1);
  // refresh_connector reads the status again without a change being reported
  CHECK(core.refresh_connector(&s, 1, &changed) == Result::Ok && !changed);
  CHECK(core.refresh_connector(&s, 9, &changed) == Result::Malformed);
}

static void test_busy_and_timeouts() {
  // a PPM that stays busy with GET_CAPABILITY: the core gives up at the timeout and cancels
  {
    FakePpm ppm;
    ppm.busy_forever.insert(kGetCapability);
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    const uint64_t start = clock.now;
    CHECK(core.initialize(&s) == Result::Busy);
    CHECK(!s.present && ppm.cancels == 1 && core.cancels() == 1 && core.timeouts() >= 1);
    const uint64_t spent = clock.now - start;
    CHECK(spent >= 100000 && spent < 400000);    // the command timeout (and the cancel's), not more
  }
  // the same with a clock that never moves: bounded by the number of looks
  {
    FakePpm ppm;
    ppm.busy_forever.insert(kGetConnectorStatus);
    FakeTransport t(ppm);
    FakeClock clock;
    clock.frozen = true;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Busy);
    CHECK(clock.sleeps < 1000);
  }
  // a reset that never completes
  {
    FakePpm ppm;
    ppm.never_reset = true;
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Timeout && !s.present);
    CHECK(ppm.count(kPpmReset) >= 1);
  }
  // a PPM that says nothing at all after the command (CCI stays 0): a timeout, and no CANCEL (it was not busy)
  {
    struct Silent final : hardware::IUcsiTransport {
      bool read_version(uint16_t* v) override { *v = kVersion1_2; return true; }
      bool read_cci(uint32_t* c) override { *c = 0; return true; }
      bool read_message_in(ByteView) override { return true; }
      bool write_message_out(ConstByteView) override { return true; }
      bool write_control(uint64_t) override { return true; }
      uint32_t message_capacity() const override { return 16; }
    } silent;
    FakeClock clock;
    UcsiCore quiet(silent, clock, fast_timing());
    uint8_t data[16];
    uint32_t length = 0;
    CHECK(quiet.execute(control_word(kGetCapability), ByteView(data), &length) == Result::Timeout && quiet.cancels() == 0 && length == 0);
  }
  // a transport that fails
  {
    FakePpm ppm;
    FakeTransport t(ppm);
    t.broken = true;
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Transport);
  }
}

static void test_errors() {
  FakePpm ppm;
  ppm.ports[0] = charger_dock();
  ppm.errors[kGetPdos] = kErrorInvalidParameter;
  ppm.not_supported.insert(kGetCableProperty);
  FakeTransport t(ppm);
  FakeClock clock;
  UcsiCore core(t, clock, fast_timing());
  hardware::UsbTypeCSnapshot s;
  CHECK(core.initialize(&s) == Result::Ok);
  CHECK(core.last_error() == kErrorInvalidParameter && s.last_error == kErrorInvalidParameter && ppm.count(kGetErrorStatus) == 1);
  // no PDOs and no cable, but the rest is there; the contract has its current
  const hardware::UsbTypeCConnector& a = s.connectors[0];
  CHECK(a.connected && a.source_pdo_count == 0 && !a.cable.valid && a.alt_mode_count == 2 && a.contract.valid && a.contract.current_ma == 3250 &&
        a.contract.voltage_mv == 0);
  CHECK(!ppm.waiting_ack);
  // an error on GET_CONNECTOR_STATUS marks the status unknown
  ppm.errors[kGetConnectorStatus] = kErrorCcCommunication;
  ppm.plug(0, charger_dock());
  bool changed = false;
  CHECK(core.service(&s, &changed) == Result::Ok && changed && !s.connectors[0].status_valid && core.last_error() == kErrorCcCommunication);
  // a GET_ERROR_STATUS that errors itself does not recurse
  ppm.errors[kGetErrorStatus] = kErrorUndefined;
  uint8_t data[16];
  uint32_t length = 0;
  const uint32_t before = ppm.count(kGetErrorStatus);
  CHECK(core.execute(control_word(kGetConnectorStatus, connector_field(1)), ByteView(data), &length) == Result::Error);
  CHECK(ppm.count(kGetErrorStatus) == before + 1);
  // not supported
  CHECK(core.execute(control_word(kGetCableProperty, connector_field(1)), ByteView(data), &length) == Result::NotSupported && length == 0);
  // a mistake on the connector number
  ppm.errors.clear();
  CHECK(core.execute(control_word(kGetConnectorStatus, connector_field(9)), ByteView(data), &length) == Result::Error &&
        core.last_error() == kErrorNonExistentConnector);
  // the answer is longer than the caller's buffer: cut, but the length is reported
  uint8_t small[4];
  CHECK(core.execute(control_word(kGetConnectorStatus, connector_field(1)), ByteView(small), &length) == Result::Ok && length == 11);
}

static void test_malformed() {
  // a CCI length beyond MESSAGE IN
  {
    FakePpm ppm;
    ppm.ports[0] = charger_dock();
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Ok);
    ppm.extra_length = 40;
    uint8_t data[16];
    uint32_t length = 0;
    CHECK(core.execute(control_word(kGetConnectorStatus, connector_field(1)), ByteView(data), &length) == Result::Malformed && !ppm.waiting_ack);
    ppm.extra_length = 0;
  }
  // a GET_CAPABILITY cut short: start-up fails
  {
    FakePpm ppm;
    ppm.corrupt = [](uint8_t command, Bytes& d) { if (command == kGetCapability) d.resize(5); };
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Malformed && !s.present);
  }
  // a status cut short: the connector's status is unknown, the rest works
  {
    FakePpm ppm;
    ppm.ports[0] = charger_dock();
    ppm.corrupt = [](uint8_t command, Bytes& d) { if (command == kGetConnectorStatus) d.resize(6); };
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Ok && s.connector_count == 2 && !s.connectors[0].status_valid && !s.connectors[1].status_valid);
  }
  // a capability claiming 127 connectors: only kUsbTypeCMaxConnectors are kept
  {
    FakePpm ppm;
    ppm.connector_total = 127;
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    CHECK(core.initialize(&s) == Result::Ok && s.connector_count == hardware::kUsbTypeCMaxConnectors);
  }
  // random damage to every answer, through start-up and many changes: no fault, and whatever was published is in range
  std::mt19937 rng(99);
  for (int round = 0; round < 60; ++round) {
    FakePpm ppm;
    ppm.version = round % 3 == 0 ? kVersion1_2 : round % 3 == 1 ? kVersion2_0 : kVersion2_1;
    ppm.capacity = ppm.version >= kVersion2_0 ? 256 : 16;
    ppm.ports[0] = charger_dock();
    ppm.corrupt = [&rng](uint8_t, Bytes& d) {
      const uint32_t what = rng() % 4;
      if (what == 0 && !d.empty()) d.resize(rng() % d.size());
      else if (what == 1) for (int i = 0; i < 3 && !d.empty(); ++i) d[rng() % d.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      else if (what == 2) d.resize(d.size() + rng() % 20, static_cast<uint8_t>(rng()));
    };
    FakeTransport t(ppm);
    FakeClock clock;
    UcsiCore core(t, clock, fast_timing());
    hardware::UsbTypeCSnapshot s;
    (void)core.initialize(&s);
    for (int i = 0; i < 10; ++i) {
      ppm.plug(static_cast<uint32_t>(rng() % 2), i % 2 ? charger_dock() : FakePpm::Port());
      bool changed;
      (void)core.service(&s, &changed);
    }
    CHECK(s.connector_count <= hardware::kUsbTypeCMaxConnectors);
    for (uint32_t i = 0; i < s.connector_count; ++i) {
      CHECK(s.connectors[i].source_pdo_count <= hardware::kUsbTypeCMaxPdos && s.connectors[i].alt_mode_count <= hardware::kUsbTypeCMaxAltModes);
      CHECK(s.connectors[i].connected || s.connectors[i].partner == hardware::UsbTypeCPartner::None);
      char line[64];
      hardware::usb_type_c_summary(s.connectors[i], ArrayView<char>(line));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The system object

static void test_system() {
  TestAllocator allocator;
  FakePpm ppm;
  ppm.ports[0] = charger_dock();
  FakeTransport t(ppm);
  FakeClock clock;
  UcsiCore core(t, clock, fast_timing());
  acpi::Node* const device = reinterpret_cast<acpi::Node*>(0x1000);   // (only compared, never followed)
  hardware::UsbTypeCSystem::Options options;
  options.retry_ms = 1000;
  {
    // the PPM is not there at first; the system tries again after retry_ms, up to max_start_attempts
    t.broken = true;
    hardware::UsbTypeCSystem system(allocator, core, clock, device, options);
    CHECK(!system.start(0) && !system.started() && !system.present() && system.connector_count() == 0);
    system.poll(500);                  // too early
    t.broken = false;
    system.poll(1000);
    CHECK(system.started() && system.present() && system.connector_count() == 2);
    hardware::UsbTypeCConnector c;
    CHECK(system.connector(0, &c) && c.connected && c.contract.power_mw == 65000);
    CHECK(!system.connector(2, &c));
    const uint32_t g = system.generation();
    // the firmware notifies: the change is read at the next poll, not before
    FakePpm::Port none;
    ppm.plug(0, none);
    system.on_notify(device, 0x81);    // (another event)
    system.on_notify(nullptr, 0x80);   // (another device)
    CHECK(system.notifications() == 0);
    system.on_notify(device, 0x80);
    CHECK(system.notifications() == 1);
    system.poll(1001);
    CHECK(system.generation() == g + 1 && system.connector(0, &c) && !c.connected);
    system.poll(1002);                 // nothing new
    CHECK(system.generation() == g + 1);
    // without a notification the change is noticed by the idle look
    ppm.plug(0, charger_dock());
    system.poll(1500);
    CHECK(system.generation() == g + 1);
    system.poll(2002);
    CHECK(system.generation() == g + 2 && system.connector(0, &c) && c.connected);
    // the PPM stops answering: after max_failures the connectors are marked unknown and it is started again later
    ppm.busy_forever.insert(kGetConnectorStatus);
    ppm.plug(1, charger_dock());
    for (uint64_t at = 3100; at < 3100 + 1100 * options.max_failures; at += 1100) {
      ppm.plug(1, charger_dock());
      system.poll(at);
    }
    CHECK(!system.started() && system.connector(0, &c) && !c.status_valid);
    ppm.busy_forever.clear();
    system.poll(100000);
    CHECK(system.started() && system.connector(0, &c) && c.status_valid && c.connected);
    hardware::UsbTypeCSnapshot snap;
    CHECK(system.snapshot(&snap) && snap.present && snap.generation == system.generation());
  }
  CHECK(true);   // (the destructor freed every published copy: the address sanitizer's leak check would say otherwise)
}

// Readers on other threads while the poller publishes changes: every copy they get is whole (the two connectors of one publication agree).
static void test_concurrent_snapshots() {
  TestAllocator allocator;
  FakePpm ppm;
  FakeTransport t(ppm);
  RealClock clock;
  UcsiCore::Timing timing = fast_timing();
  timing.first_poll_us = 1;
  timing.max_poll_us = 1;
  UcsiCore core(t, clock, timing);
  acpi::Node* const device = reinterpret_cast<acpi::Node*>(0x2000);
  hardware::UsbTypeCSystem::Options options;
  options.idle_check_ms = 1;
  hardware::UsbTypeCSystem system(allocator, core, clock, device, options);
  CHECK(system.start(0));
  std::atomic<bool> done{false};
  std::atomic<uint32_t> reads{0}, torn{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&] {
      while (!done.load()) {
        hardware::UsbTypeCSnapshot s;
        if (!system.snapshot(&s)) continue;
        reads.fetch_add(1);
        // the writer plugs both connectors with the same current: a mixed copy would show two different ones
        if (s.connector_count != 2 || s.connectors[0].connected != s.connectors[1].connected ||
            s.connectors[0].contract.current_ma != s.connectors[1].contract.current_ma)
          torn.fetch_add(1);
        hardware::UsbTypeCConnector c;
        (void)system.connector(1, &c);
        (void)system.generation();
      }
    });
  std::thread poller(&hardware::UsbTypeCSystem::run, &system);
  // the "hardware" changes, the firmware notifies
  for (uint32_t i = 1; i <= 150; ++i) {
    FakePpm::Port p = charger_dock();
    p.rdo = fixed_rdo(1, 100 * i, 3000);
    p.connected = i % 3 != 0;
    {
      std::lock_guard<std::mutex> g(ppm.lock);
      ppm.ports[0] = p;
      ppm.ports[1] = p;
      ppm.ports[0].change = ppm.ports[1].change = kChangeConnect;
      ppm.changes.clear();
      ppm.changes.push_back(1);
      ppm.changes.push_back(2);
    }
    system.on_notify(device, 0x80);
    // wait until both are read (then the published copy shows the pair)
    for (int w = 0; w < 2000; ++w) {
      {
        std::lock_guard<std::mutex> g(ppm.lock);
        if (ppm.changes.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
  system.request_stop();
  poller.join();
  done.store(true);
  for (std::thread& r : readers) r.join();
  CHECK(!system.running());
  CHECK(reads.load() > 100);
  // (between the two changes of one round a copy can show connector 1 new and 2 old: the writer publishes once per service(), after both, so
  // a torn pair would mean a reader saw a half-written copy)
  CHECK(torn.load() == 0);
  CHECK(system.publications() >= 50);
  hardware::UsbTypeCConnector c;
  CHECK(system.connector(0, &c) && !c.connected);     // (the last round, 150, unplugs)
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The ACPI transport against AML

static Bytes region(const char* n, uint8_t space, uint64_t offset, uint64_t length) { return cat({b({0x5B, 0x80}), path(n), b({space}), num(offset), num(length)}); }
static Bytes field_list(const char* prefix, uint32_t bytes) {
  Bytes r;
  for (uint32_t i = 0; i < bytes; ++i) {
    char n[12];
    snprintf(n, sizeof n, "%s%02X", prefix, i);
    r = cat({r, seg(n), b({8})});
  }
  return r;
}
static Bytes field(const char* region_name, uint8_t flags, const Bytes& list) { return cat({b({0x5B, 0x81}), pkg(cat({path(region_name), b({flags}), list}))}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes lequal(const Bytes& a, const Bytes& c) { return cat({b({0x93}), a, c}); }
static Bytes if_(const Bytes& cond, const Bytes& body) { return cat({b({0xA0}), pkg(cat({cond, body}))}); }
static Bytes buffer(const Bytes& bytes) { return cat({b({0x11}), pkg(cat({num(bytes.size()), bytes}))}); }
static Bytes copy_bytes(const char* from, const char* to, uint32_t first, uint32_t count) {
  Bytes r;
  for (uint32_t i = first; i < first + count; ++i) {
    char a[12], c[12];
    snprintf(a, sizeof a, "%s%02X", from, i);
    snprintf(c, sizeof c, "%s%02X", to, i);
    r = cat({r, store(path(a), path(c))});
  }
  return r;
}

constexpr uint64_t kMailbox = 0xFE000000ull, kEcMailbox = 0xFE001000ull, kDoorbell = kEcMailbox + 0x30;
static const Bytes kUuid = {0xC2, 0x98, 0x83, 0x6F, 0xA4, 0x7C, 0xE4, 0x11, 0xAD, 0x36, 0x63, 0x10, 0x42, 0xB5, 0x00, 0x8F};

// Like a laptop's: \_SB.UBTC, _HID USBC000 and _CID PNP0CA0, a _CRS template whose base _CRS patches in, the mailbox as byte fields, and a _DSM
// that copies CONTROL and MESSAGE OUT to the "embedded controller" (another memory range here) and rings it (function 1), or rings it and copies
// VERSION, CCI and MESSAGE IN back (function 2). TNFY raises the notification. `dword` gives _CRS as a DWord address space descriptor instead.
static Bytes ucsi_dsdt(bool dword, uint8_t query_mask) {
  Bytes crs = dword ? Bytes{0x87, 0x17, 0x00, 0x00, 0x0C, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x0F, 0x00, 0xFE, 0, 0, 0, 0, 0x00, 0x10, 0, 0, 0x79, 0x00}
                    : Bytes{0x86, 0x09, 0x00, 0x01, 0, 0, 0, 0, 0x00, 0x10, 0, 0, 0x79, 0x00};
  const uint32_t base_at = dword ? 10 : 4;
  const Bytes create = cat({b({0x8A}), path("CRSB"), num(base_at), path("CBAS")});
  const Bytes crs_method = method("_CRS", 0, cat({create, store(num(kMailbox), path("CBAS")), ret(path("CRSB"))}));
  const Bytes write = cat({copy_bytes("MB", "EC", 0x08, 8), copy_bytes("MB", "EC", 0x20, 16), store(num(1), path("DBEL"))});
  const Bytes read = cat({store(num(2), path("DBEL")), copy_bytes("EC", "MB", 0x00, 8), copy_bytes("EC", "MB", 0x10, 16)});
  const Bytes dsm = method("_DSM", 4, cat({if_(lequal(b({0x68}), buffer(kUuid)),
                                              cat({if_(lequal(b({0x6A}), num(0)), ret(buffer(b({query_mask})))),
                                                   if_(lequal(b({0x6A}), num(1)), write),
                                                   if_(lequal(b({0x6A}), num(2)), read)})),
                                         ret(buffer(b({0x00})))}));
  const Bytes body = cat({name_def("_HID", num(0x00C06256)), name_def("_CID", num(0xA00CD041)), name_def("CRSB", buffer(crs)), crs_method,
                          region("UBMB", 0, kMailbox, 0x30), field("UBMB", 0x11, field_list("MB", 0x30)),
                          region("ECMB", 0, kEcMailbox, 0x31), field("ECMB", 0x11, cat({field_list("EC", 0x30), seg("DBEL"), b({8})})),
                          dsm, method("TNFY", 0, notify("\\_SB.UBTC", 0x80))});
  return make_table("DSDT", scope("\\_SB", device("UBTC", body)));
}

// The memory both the mailbox and the "embedded controller" are in; the doorbell runs the model PPM.
struct UcsiAcpiHost final : acpi::IAmlHost {
  FakePpm& ppm;
  std::map<uint64_t, uint8_t> mem;
  hardware::IAcpiNotifyListener* listener = nullptr;
  uint32_t doorbells = 0;
  explicit UcsiAcpiHost(FakePpm& p) : ppm(p) {}
  bool inside(uint64_t a) const { return (a >= kMailbox && a < kMailbox + 0x1000) || (a >= kEcMailbox && a <= kDoorbell); }
  bool region_read(const RegionAccess& a, uint64_t* v) override {
    if (a.space != RegionSpace::SystemMemory || a.width_bits != 8 || !inside(a.address)) return false;
    *v = mem[a.address];
    return true;
  }
  bool region_write(const RegionAccess& a, uint64_t v) override {
    if (a.space != RegionSpace::SystemMemory || a.width_bits != 8 || !inside(a.address)) return false;
    mem[a.address] = static_cast<uint8_t>(v);
    if (a.address == kDoorbell) {
      ++doorbells;
      if (v == 1) {
        uint64_t control = 0;
        for (int i = 0; i < 8; ++i) control |= uint64_t{mem[kEcMailbox + 8 + i]} << (8 * i);
        ppm.control(control);
      } else if (v == 2) {
        const uint32_t cci = ppm.read_cci();
        mem[kEcMailbox + 0] = static_cast<uint8_t>(ppm.version);
        mem[kEcMailbox + 1] = static_cast<uint8_t>(ppm.version >> 8);
        for (int i = 0; i < 4; ++i) mem[kEcMailbox + 4 + i] = static_cast<uint8_t>(cci >> (8 * i));
        for (int i = 0; i < 16; ++i) mem[kEcMailbox + 0x10 + i] = ppm.message_in[i];
      }
    }
    return true;
  }
  void notify(Node* device, uint32_t value) override { if (listener) listener->on_notify(device, value); }
};

static void test_acpi_transport(bool dword) {
  TestAllocator allocator;
  FakePpm ppm;
  ppm.ports[0] = charger_dock();
  UcsiAcpiHost host(ppm);
  AmlInterpreter vm(allocator, host);
  const Bytes dsdt = ucsi_dsdt(dword, 0x07);
  CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
  Node* found[4];
  CHECK(hardware::AcpiUcsiTransport::find_devices(vm, ArrayView<Node*>(found)) == 1);
  Node* device = vm.find("\\_SB.UBTC");
  CHECK(device && found[0] == device);
  char id[8];
  eisa_id_to_string(0x00C06256, id);
  CHECK(strcmp(id, "USBC000") == 0);
  eisa_id_to_string(0xA00CD041, id);
  CHECK(strcmp(id, "PNP0CA0") == 0);
  hardware::AcpiUcsiTransport transport(vm, host, device);
  CHECK(transport.initialize());
  CHECK(transport.mailbox_address() == kMailbox && transport.mailbox_length() == 0x1000 && transport.message_capacity() == 16);
  // _DSM with another UUID does nothing
  {
    kernel::ScopedRecursiveMutex guard(vm.mutex());
    uint8_t other[16] = {1};
    Value args[4];
    args[0].kind = ValueKind::Buffer; args[0].length = 16; args[0].bytes = other;
    args[1].kind = ValueKind::Integer; args[1].integer = 1;
    args[2].kind = ValueKind::Integer; args[2].integer = 1;
    args[3].kind = ValueKind::Package; args[3].length = 0; args[3].elements = nullptr;
    const uint32_t rings = host.doorbells;
    Value r;
    CHECK(vm.evaluate(vm.find("_DSM", device), args, 4, &r) && r.kind == ValueKind::Buffer && r.length == 1 && r.bytes[0] == 0 && host.doorbells == rings);
  }
  FakeClock clock;
  UcsiCore core(transport, clock, fast_timing());
  hardware::UsbTypeCSnapshot s;
  CHECK(core.initialize(&s) == Result::Ok);
  CHECK(s.present && s.ucsi_version == kVersion1_2 && s.connector_count == 2 && s.connectors[0].connected && s.connectors[0].contract.power_mw == 65000 &&
        s.connectors[0].source_pdo_count == 5 && s.connectors[0].alt_mode_count == 2 && s.connectors[0].cable.valid);
  CHECK(transport.dsm_calls() > 20 && host.doorbells > 20);
  // the notification: AML's Notify reaches the system, which reads the change at its next poll
  hardware::UsbTypeCSystem system(allocator, core, clock, device);
  host.listener = &system;
  CHECK(system.start(0));
  ppm.plug(0, FakePpm::Port());
  CHECK(vm.evaluate("\\_SB.UBTC.TNFY", nullptr));
  CHECK(system.notifications() == 1);
  system.poll(1);
  hardware::UsbTypeCConnector c;
  CHECK(system.connector(0, &c) && !c.connected);
  host.listener = nullptr;

  // a _DSM without the read and write functions, a _CRS that is too small: not used
  {
    FakePpm other;
    UcsiAcpiHost h(other);
    AmlInterpreter vm2(allocator, h);
    const Bytes t2 = ucsi_dsdt(dword, 0x01);
    CHECK(vm2.load_table(ConstByteView(t2.data(), t2.size())));
    hardware::AcpiUcsiTransport bad(vm2, h, vm2.find("\\_SB.UBTC"));
    CHECK(!bad.initialize());
    hardware::AcpiUcsiTransport none(vm2, h, nullptr);
    CHECK(!none.initialize());
    hardware::AcpiUcsiTransport root(vm2, h, vm2.root());
    CHECK(!root.initialize());
  }
}

// The address space descriptors parse_acpi_resources gained (DWord and QWord memory), with truncations.
static void test_resources() {
  hardware::AcpiResources r;
  const Bytes dw = {0x87, 0x17, 0x00, 0x00, 0x0C, 0x03, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0x0F, 0x00, 0xFE, 0, 0, 0, 0, 0x00, 0x10, 0, 0, 0x79, 0x00};
  CHECK(hardware::parse_acpi_resources(view(dw), &r) && r.memory_count == 1 && r.memory[0].base == 0xFE000000u && r.memory[0].length == 0x1000);
  Bytes qw = {0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x03};
  for (uint64_t v : {0ull, 0xFE000000ull, 0xFE000FFFull, 0ull, 0x1000ull}) for (int i = 0; i < 8; ++i) qw.push_back(static_cast<uint8_t>(v >> (8 * i)));
  qw.push_back(0x79);
  qw.push_back(0x00);
  CHECK(hardware::parse_acpi_resources(view(qw), &r) && r.memory_count == 1 && r.memory[0].base == 0xFE000000u && r.memory[0].length == 0x1000);
  // above 4 GiB, an I/O range, a range that wraps: skipped
  Bytes high = qw;
  high[6 + 8 + 4] = 1;                 // minimum 0x1_FE000000
  CHECK(hardware::parse_acpi_resources(view(high), &r) && r.memory_count == 0);
  Bytes io = dw;
  io[3] = 1;
  CHECK(hardware::parse_acpi_resources(view(io), &r) && r.memory_count == 0);
  Bytes wrap = dw;
  wrap[22] = 0xFF; wrap[23] = 0xFF; wrap[24] = 0xFF; wrap[25] = 0xFF;
  CHECK(hardware::parse_acpi_resources(view(wrap), &r) && r.memory_count == 0);
  for (size_t cut = 0; cut < dw.size(); ++cut) (void)hardware::parse_acpi_resources(ConstByteView(dw.data(), cut), &r);
  for (size_t cut = 0; cut < qw.size(); ++cut) (void)hardware::parse_acpi_resources(ConstByteView(qw.data(), cut), &r);
  CHECK(!hardware::parse_acpi_resources(ConstByteView(dw.data(), 20), &r));
}

int main() {
  test_decoders();
  test_decoder_fuzz();
  test_initialize(0x0100);
  test_initialize(kVersion1_2);
  test_initialize(kVersion2_0);
  test_initialize(kVersion2_1);
  test_connector_changes();
  test_busy_and_timeouts();
  test_errors();
  test_malformed();
  test_system();
  test_concurrent_snapshots();
  test_resources();
  test_acpi_transport(false);
  test_acpi_transport(true);
  if (g_failed) {
    printf("UcsiTest: %d check(s) failed\n", g_failed);
    return 1;
  }
  printf("UcsiTest: all passed\n");
  return 0;
}

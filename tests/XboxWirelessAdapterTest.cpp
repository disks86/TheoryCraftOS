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

// Host tests for the Xbox Wireless Adapter driver against a model of the adapter on a fake USB host controller: the frame encoders and
// decoders (with truncated and bit-flipped input), refusing without firmware, the firmware download sequence, damaged firmware files,
// controllers associating, GIP power-on, input, guide acknowledgement, rumble and battery through the 802.11 wrapping, pairing, departures,
// the adapter unplugged mid-stream, the game controller service's idle pool, and several threads using one adapter's pads at once.
//
// The model implements the layouts the driver assumes (XboxWirelessFrames.hpp); it checks that the driver is consistent with them, not that
// they are what a real adapter does.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/Usb/XboxWirelessAdapter.hpp"
#include "Hardware/GameController/Usb/XboxWirelessFrames.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

using namespace hardware;
using namespace hardware::gamepad;
namespace xw = hardware::xbox_wireless;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED); return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED); free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { __atomic_add_fetch(&g_checks, 1, __ATOMIC_RELAXED); if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); } } while (0)

typedef std::vector<uint8_t> Bytes;
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void put16(Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
static uint32_t get32(const Bytes& b, size_t at) { return uint32_t{b[at]} | uint32_t{b[at + 1]} << 8 | uint32_t{b[at + 2]} << 16 | uint32_t{b[at + 3]} << 24; }

static const MacAddress kPadA{{0x7C, 0xED, 0x8D, 0x11, 0x22, 0x33}};
static const MacAddress kPadB{{0x7C, 0xED, 0x8D, 0x44, 0x55, 0x66}};
static const MacAddress kAdapterMac{{0x62, 0x45, 0xB4, 0xA1, 0xB2, 0xC3}};

// --- packets as the adapter would send them ---------------------------------------------------------------------------------------------------

static Bytes header80211(uint8_t type, uint8_t subtype, uint8_t flags, const MacAddress& a1, const MacAddress& a2, const MacAddress& a3) {
  Bytes f = {static_cast<uint8_t>((subtype << 4) | (type << 2)), flags, 0, 0};
  for (const MacAddress* m : {&a1, &a2, &a3}) f.insert(f.end(), m->bytes, m->bytes + 6);
  put16(f, 0x0010);
  return f;
}

// An RX packet: DMA length word, RXWI (WCID, MPDU length, L2PAD), the frame (two pad bytes after a QoS header if l2pad), FCE info (port).
static Bytes rx_frame(uint8_t wcid, const Bytes& frame, bool l2pad = false, uint32_t port = 0, uint8_t event = 0) {
  Bytes body;
  put32(body, l2pad ? (1u << 14) : 0);
  put32(body, wcid | (static_cast<uint32_t>(frame.size()) << 16));
  body.resize(32, 0);
  if (l2pad) {
    body.insert(body.end(), frame.begin(), frame.begin() + 26);
    body.push_back(0);
    body.push_back(0);
    body.insert(body.end(), frame.begin() + 26, frame.end());
  } else {
    body.insert(body.end(), frame.begin(), frame.end());
  }
  while (body.size() % 4) body.push_back(0);
  Bytes p;
  put32(p, static_cast<uint32_t>(body.size()));
  p.insert(p.end(), body.begin(), body.end());
  put32(p, (port << 27) | (static_cast<uint32_t>(event) << 20));
  return p;
}

static Bytes rx_event(uint8_t event, const Bytes& data) {
  Bytes body = data;
  while (body.size() % 4) body.push_back(0);
  Bytes p;
  put32(p, static_cast<uint32_t>(body.size()));
  p.insert(p.end(), body.begin(), body.end());
  put32(p, (1u << 27) | (static_cast<uint32_t>(event) << 20));
  return p;
}

static Bytes management_from(const MacAddress& pad, uint8_t subtype, const Bytes& body) {
  Bytes f = header80211(0, subtype, 0, kAdapterMac, pad, kAdapterMac);
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

static Bytes qos_from(const MacAddress& pad, const Bytes& gip) {
  Bytes f = header80211(2, 8, 0x01 /* to DS */, kAdapterMac, pad, kAdapterMac);
  put16(f, 0);
  f.insert(f.end(), gip.begin(), gip.end());
  return f;
}

static Bytes gip_input(uint8_t a, uint8_t b, uint16_t lt, uint16_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t sequence = 5) {
  Bytes m = {xbox::kGipInput, 0x00, sequence, 0x0E, a, b, static_cast<uint8_t>(lt), static_cast<uint8_t>(lt >> 8), static_cast<uint8_t>(rt),
             static_cast<uint8_t>(rt >> 8)};
  for (int16_t v : {lx, ly, rx, ry}) put16(m, static_cast<uint16_t>(v));
  m[3] = static_cast<uint8_t>(m.size() - 4);
  return m;
}
static const Bytes kAnnounce = {xw::kGipAnnounce, 0x20, 0x01, 0x1C, 0x7C, 0xED, 0x8D, 0x11, 0x22, 0x33, 0x00, 0x00, 0x5E, 0x04, 0x12, 0x0B,
                                0x05, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// --- the model of the adapter ---------------------------------------------------------------------------------------------------------------

struct McuCommand { uint8_t command, sequence; Bytes payload; };
struct WlanPacket { uint8_t wcid; xw::Frame frame; Bytes raw; Bytes body; };

struct FakeAdapter final : IUsbHostController {
  uint16_t vendor = 0x045E, product = 0x02E6;
  bool present = true, firmware_starts = true, bulk_in_supported = true, mcu_left_running = false;
  uint32_t generation = 1;
  Bytes config;
  std::map<uint32_t, uint32_t> regs, cfg, fce;
  std::map<uint32_t, uint8_t> memory;          // what the firmware download wrote, by MCU address
  std::vector<std::string> ops;                // the order of everything that happened
  std::vector<McuCommand> commands;
  std::vector<WlanPacket> frames;
  std::deque<Bytes> in[16];
  uint32_t chunks = 0, resets = 0, ivb_loads = 0, bad_packets = 0;
  uint8_t eeprom[16] = {0, 0, 0, 0, 0x62, 0x45, 0xB4, 0xA1, 0xB2, 0xC3, 0, 0, 0, 0, 0, 0};
  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   // (the model is used from several threads in one test)

  FakeAdapter() {
    // Interface 0: vendor class, two bulk IN, three bulk OUT (the first is the command endpoint).
    Bytes b;
    auto add = [&](std::initializer_list<uint8_t> d) { b.push_back(static_cast<uint8_t>(d.size() + 1)); b.insert(b.end(), d.begin(), d.end()); };
    add({4, 0, 0, 5, 0xFF, 0xFF, 0xFF, 0});
    add({5, 0x84, 2, 0x00, 0x02, 0});
    add({5, 0x85, 2, 0x00, 0x02, 0});
    add({5, 0x08, 2, 0x00, 0x02, 0});
    add({5, 0x04, 2, 0x00, 0x02, 0});
    add({5, 0x05, 2, 0x00, 0x02, 0});
    config = {9, 2, static_cast<uint8_t>(b.size() + 9), 0, 1, 1, 0, 0x80, 250};
    config.insert(config.end(), b.begin(), b.end());
  }
  struct Guard { pthread_mutex_t& m; explicit Guard(pthread_mutex_t& x) : m(x) { pthread_mutex_lock(&m); } ~Guard() { pthread_mutex_unlock(&m); } };

  const char* name() const override { return "fake xbox adapter"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::High; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = vendor;
    out->product_id = product;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return present; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    Guard g(lock);
    if (!present || type != 0x40) return false;
    if (request == xw::mt::kVendWriteFce) {
      const uint32_t reg = index & ~3u;
      const uint32_t shift = (index & 2) ? 16 : 0;
      fce[reg] = (fce[reg] & ~(0xFFFFu << shift)) | (uint32_t{value} << shift);
      if (shift) ops.push_back("fce " + std::to_string(reg) + " " + std::to_string(fce[reg]));
      return true;
    }
    if (request == xw::mt::kVendDevMode) {
      if (value == xw::mt::kDevModeLoadIvb) {
        ++ivb_loads;
        ops.push_back("ivb");
        if (firmware_starts) regs[xw::mt::kMcuComReg0] |= 1;
      } else if (value == xw::mt::kDevModeReset) {
        ++resets;
        ops.push_back("reset");
        regs[xw::mt::kMcuComReg0] = 0;
      }
      return true;
    }
    return false;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    Guard g(lock);
    if (!present || type != 0xC0) return false;
    if (request == xw::mt::kVendMultiRead && buffer.size() == 4) {
      const uint32_t reg = (uint32_t{value} << 16) | index;
      uint32_t v = regs[reg];
      if (reg == xw::mt::kChBusy) v = channel_busy();
      if (reg == xw::mt::kChIdle) v = 1000;
      for (int i = 0; i < 4; ++i) buffer[i] = static_cast<uint8_t>(v >> (8 * i));
      return true;
    }
    if (request == xw::mt::kVendReadEeprom) {
      for (size_t i = 0; i < buffer.size(); ++i) buffer[i] = index + i < sizeof(eeprom) ? eeprom[index + i] : 0;
      return true;
    }
    return false;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    Guard g(lock);
    if (!present || type != 0x40 || data.size() != 4) return false;
    const uint32_t reg = (uint32_t{value} << 16) | index;
    const uint32_t v = uint32_t{data[0]} | uint32_t{data[1]} << 8 | uint32_t{data[2]} << 16 | uint32_t{data[3]} << 24;
    if (request == xw::mt::kVendMultiWrite) regs[reg] = v;
    else if (request == xw::mt::kVendWriteCfg) cfg[reg] = v;
    else return false;
    ops.push_back(std::string(request == xw::mt::kVendWriteCfg ? "cfg " : "w ") + std::to_string(reg) + " " + std::to_string(v));
    return true;
  }
  // The channel being listened to (the last CMD_SWITCH_CHANNEL_OP) is busy unless it is 149.
  uint32_t channel_busy() {
    for (size_t i = commands.size(); i-- > 0;)
      if (commands[i].command == xw::mt::kCmdSwitchChannelOp) return commands[i].payload[0] == 149 ? 10 : 500;
    return 0;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool start_bulk_in_stream(uint32_t, uint8_t endpoint, uint16_t) override { return present && bulk_in_supported && (endpoint == 0x84 || endpoint == 0x85); }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    Guard g(lock);
    std::deque<Bytes>& q = in[endpoint & 0x0F];
    if (!present || q.empty()) return false;
    const Bytes b = q.front();
    q.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return present; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    Guard g(lock);
    if (!present) return false;
    const Bytes b(data.data(), data.data() + data.size());
    *transferred = static_cast<uint32_t>(b.size());
    if (b.size() < 8) { ++bad_packets; return true; }
    const uint32_t info = get32(b, 0);
    const uint32_t length = info & 0xFFFF;
    if (length + 8 != b.size() || get32(b, b.size() - 4) != 0) { ++bad_packets; return true; }
    const Bytes payload(b.begin() + 4, b.begin() + 4 + length);
    if (endpoint == 0x08) {
      if (((info >> 27) & 7) != 2 || !(info & (1u << 30))) { ++bad_packets; return true; }
      const uint8_t command = (info >> 20) & 0x7F, sequence = (info >> 16) & 0xF;
      if (command == 0 && sequence == 0) {          // a firmware chunk: where the FCE registers say
        const uint32_t address = fce[xw::mt::kFceDmaAddr], dma_length = fce[xw::mt::kFceDmaLen] >> 16;
        if (dma_length != length) ++bad_packets;
        for (uint32_t i = 0; i < length; ++i) memory[address + i] = payload[i];
        ++chunks;
        ops.push_back("chunk " + std::to_string(address) + " " + std::to_string(length));
      } else {
        commands.push_back({command, sequence, payload});
        ops.push_back("mcu " + std::to_string(command));
      }
      return true;
    }
    if (endpoint == 0x04) {
      if ((info & (1u << 19)) == 0 || ((info >> 27) & 7) != 0 || payload.size() < 20) { ++bad_packets; return true; }
      WlanPacket p;
      p.wcid = payload[5];
      const uint32_t mpdu = (payload[6] | payload[7] << 8) & 0x0FFF;   // (TXWI len_ctl)
      if (20 + mpdu > payload.size()) { ++bad_packets; return true; }
      p.raw.assign(payload.begin() + 20, payload.begin() + 20 + mpdu);
      frames.push_back(p);
      WlanPacket& stored = frames.back();
      if (!xw::decode_frame(view(stored.raw), false, &stored.frame)) ++bad_packets;
      stored.body.assign(stored.frame.body.data(), stored.frame.body.data() + stored.frame.body.size());
      ops.push_back("frame");
      return true;
    }
    ++bad_packets;
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return present; }
  uint32_t port_generation(uint32_t) const override { return generation; }

  void unplug() { Guard g(lock); present = false; ++generation; }
  void push(const Bytes& packet, uint8_t endpoint = 0x84) { Guard g(lock); in[endpoint & 0x0F].push_back(packet); }
  // The GIP messages sent to a controller since `from` (frames index).
  std::vector<Bytes> gip_to(const MacAddress& pad, size_t from = 0) {
    Guard g(lock);
    std::vector<Bytes> out;
    for (size_t i = from; i < frames.size(); ++i)
      if (frames[i].frame.type == 2 && frames[i].frame.destination == pad) out.push_back(frames[i].body);
    return out;
  }
  size_t frame_count() { Guard g(lock); return frames.size(); }
  const McuCommand* last_command(uint8_t command) {
    for (size_t i = commands.size(); i-- > 0;)
      if (commands[i].command == command) return &commands[i];
    return nullptr;
  }
};

// A firmware file: header (ILM and DLM lengths, versions, build time), ILM, DLM.
static Bytes firmware_file(uint32_t ilm, uint32_t dlm) {
  Bytes f;
  put32(f, ilm);
  put32(f, dlm);
  put16(f, 0x1234);
  put16(f, 0x0100);
  put32(f, 0);
  const char build[] = "201707311234____";
  f.insert(f.end(), build, build + 16);
  for (uint32_t i = 0; i < ilm + dlm; ++i) f.push_back(static_cast<uint8_t>(i * 7 + (i >> 8)));
  return f;
}

struct FakeFirmware final : IWifiFirmwareSource {
  std::map<std::string, Bytes> files;
  std::vector<std::string> asked;
  bool read(const char* name, ByteBuffer& out) override {
    asked.push_back(name);
    auto it = files.find(name);
    if (it == files.end() || it->second.empty()) return false;
    out.clear();
    return out.append(view(it->second));
  }
};

static std::vector<std::string> g_log;
static void log_line(void*, StringView text) { g_log.push_back(std::string(text.data(), text.size())); }

struct Rig {
  TestAllocator& a;
  FakeAdapter host;
  FakeFirmware firmware;
  XboxWirelessEnvironment environment;
  UniquePointer<IGameController> pads[8];
  SharedPointer<XboxWirelessAdapter> adapter;
  XboxWirelessStatus status = XboxWirelessStatus::Ok;
  uint32_t count = 0;
  explicit Rig(TestAllocator& allocator, bool with_firmware = true) : a(allocator) {
    environment.firmware = &firmware;
    environment.log = &log_line;
    environment.channel_dwell_us = 0;
    if (with_firmware) firmware.files["xow_dongle.bin"] = firmware_file(0x8000, 0x200);
  }
  uint32_t open() {
    count = open_xbox_wireless_adapter(host, 0, a, environment, ArrayView<UniquePointer<IGameController>>(pads), &status, &adapter);
    return count;
  }
  void poll_all() { for (auto& p : pads) if (p) (void)p->poll(); }
  void associate(const MacAddress& pad) { host.push(rx_frame(0xFF, management_from(pad, xw::kSubtypeAssociationRequest, {0x00, 0x00, 0x01, 0x00}))); }
  void gip(const MacAddress& pad, uint8_t wcid, const Bytes& message, bool l2pad = false) { host.push(rx_frame(wcid, qos_from(pad, message), l2pad)); }
};

// --- frames ------------------------------------------------------------------------------------------------------------------------------------

static void test_frames() {
  uint8_t out[256];
  // An MCU message: info word, payload padded to 4, four zero bytes.
  const uint8_t payload[5] = {1, 2, 3, 4, 5};
  uint32_t n = xw::encode_mcu_message(30, 3, ConstByteView(payload), ByteView(out));
  CHECK(n == 4 + 8 + 4);
  const uint32_t info = out[0] | out[1] << 8 | out[2] << 16 | static_cast<uint32_t>(out[3]) << 24;
  CHECK((info & 0xFFFF) == 8 && ((info >> 16) & 0xF) == 3 && ((info >> 20) & 0x7F) == 30 && ((info >> 27) & 7) == 2 && (info & (1u << 30)));
  CHECK(out[4] == 1 && out[8] == 5 && out[9] == 0 && out[12] == 0 && out[15] == 0);
  CHECK(xw::encode_mcu_message(1, 1, ConstByteView(payload), ByteView(out).first(15)) == 0);

  // A data frame to a controller survives the trip through the TX encoding and the RX decoding.
  const uint8_t gip[6] = {0x09, 0x00, 0x01, 0x02, 0xAA, 0xBB};
  uint8_t frame[64];
  n = xw::encode_qos_data(kPadA, kAdapterMac, 7, ConstByteView(gip), ByteView(frame));
  CHECK(n == 26 + 6 && frame[0] == 0x88 && frame[1] == 0x02);
  xw::Frame f;
  CHECK(xw::decode_frame(ConstByteView(frame).first(n), false, &f) && f.type == 2 && f.subtype == 8 && f.destination == kPadA && f.source == kAdapterMac &&
        f.body.size() == 6 && f.body[4] == 0xAA);
  const uint32_t packet = xw::encode_wlan_packet(3, ConstByteView(frame).first(n), ByteView(out));
  CHECK(packet == 4 + 20 + 32 + 4 && out[4 + 5] == 3 && (out[4 + 6] | out[4 + 7] << 8) == 32 && out[4 + 4] == 1);
  CHECK(xw::encode_wlan_packet(3, ConstByteView(frame).first(n), ByteView(out).first(59)) == 0);

  // RX: a QoS frame with and without the radio's padding; a frame claiming more than arrived is nothing.
  const Bytes q = qos_from(kPadA, gip_input(0x10, 0, 0, 0, 0, 0, 0, 0));
  for (bool pad : {false, true}) {
    xw::RxPacket p;
    CHECK(xw::decode_rx(view(rx_frame(2, q, pad)), &p) && p.kind == xw::RxKind::Frame && p.wcid == 2 && p.frame.source == kPadA &&
          p.frame.body.size() == 18 && p.frame.body[0] == xbox::kGipInput && p.frame.body[4] == 0x10);
  }
  Bytes liar = rx_frame(2, q);
  liar[4 + 4 + 3] = 0x3F;                         // MPDU length far beyond the packet
  xw::RxPacket p;
  CHECK(!xw::decode_rx(view(liar), &p));
  CHECK(xw::decode_rx(view(rx_event(xw::xbox_table::kEventButtonPress, {})), &p) && p.kind == xw::RxKind::ButtonPress);
  CHECK(xw::decode_rx(view(rx_event(xw::xbox_table::kEventClientLost, {3})), &p) && p.kind == xw::RxKind::ClientLost && p.wcid == 3);
  CHECK(!xw::decode_rx(view(rx_event(xw::xbox_table::kEventClientLost, {})), &p));
  CHECK(xw::decode_rx(view(rx_event(0x01, {})), &p) && p.kind == xw::RxKind::OtherEvent);
  // A management frame passed on as an MCU event decodes like one from the WLAN port.
  CHECK(xw::decode_rx(view(rx_frame(0xFF, management_from(kPadB, 0, {}), false, 1, xw::xbox_table::kEventPacketRx)), &p) && p.kind == xw::RxKind::Frame &&
        p.frame.type == 0 && p.frame.source == kPadB);

  // Every prefix and every single-bit flip of each kind of packet: nothing reads outside it (ASan), short ones fail.
  const Bytes samples[] = {rx_frame(2, q, true), rx_frame(1, management_from(kPadA, 0, {1, 2, 3})), rx_event(xw::xbox_table::kEventClientLost, {1}),
                           rx_frame(0xFF, management_from(kPadB, 7, {1}), false, 1, xw::xbox_table::kEventPacketRx)};
  for (const Bytes& s : samples) {
    for (size_t len = 0; len < s.size(); ++len) {
      Bytes cut(s.begin(), s.begin() + len);
      CHECK(!xw::decode_rx(view(cut), &p));
    }
    for (size_t bit = 0; bit < s.size() * 8; ++bit) {
      Bytes flipped = s;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      if (xw::decode_rx(view(flipped), &p) && p.kind == xw::RxKind::Frame) CHECK(p.frame.body.size() <= flipped.size());
    }
  }

  // The beacon carries the channel and the pairing flag.
  uint8_t body[64];
  n = xw::encode_beacon_body(149, true, ByteView(body));
  CHECK(n == 8 + 2 + 2 + 2 + 3 + sizeof(xw::xbox_table::kBeaconVendorElement) && body[16] == 149 && body[17] == 0xDD &&
        body[17 + xw::xbox_table::kBeaconPairingIndex] == 1);
  CHECK(xw::encode_beacon_body(149, false, ByteView(body)) == n && body[17 + xw::xbox_table::kBeaconPairingIndex] == 0);
  CHECK(xw::encode_beacon_body(149, false, ByteView(body).first(20)) == 0);
  CHECK(xw::encode_association_response_body(3, ByteView(body)) == 6 && body[2] == 0 && body[4] == 3 && body[5] == 0xC0);

  // The quietest channel; ties go to the first; the busy share is what counts, not the count.
  const xw::ChannelSample s1[] = {{36, 50, 50}, {149, 10, 1000}, {6, 10, 1000}};
  CHECK(xw::choose_channel(ArrayView<const xw::ChannelSample>(s1)) == 149);
  const xw::ChannelSample s2[] = {{36, 100, 900}, {40, 20, 80}};
  CHECK(xw::choose_channel(ArrayView<const xw::ChannelSample>(s2)) == 36);
  CHECK(xw::choose_channel(ArrayView<const xw::ChannelSample>()) == 0);
  const xw::ChannelSample s3[] = {{11, 0xFFFFFFFFu, 0xFFFFFFFFu}, {1, 0, 0}};
  CHECK(xw::choose_channel(ArrayView<const xw::ChannelSample>(s3)) == 1);

  // The GIP status message's battery.
  State st;
  const Bytes status = {xbox::kGipStatus, 0x20, 1, 4, 0x08 | 0x03, 0, 0, 0};
  CHECK(xw::decode_gip_battery(view(status), &st) && st.battery_kind == BatteryKind::Rechargeable && st.battery_percent == 100);
  const Bytes alkaline = {xbox::kGipStatus, 0x20, 1, 4, 0x04 | 0x01, 0, 0, 0};
  CHECK(xw::decode_gip_battery(view(alkaline), &st) && st.battery_kind == BatteryKind::Alkaline && st.battery_percent == 33);
  CHECK(!xw::decode_gip_battery(view(Bytes(status.begin(), status.begin() + 4)), &st));
  CHECK(!xw::decode_gip_battery(view(gip_input(0, 0, 0, 0, 0, 0, 0, 0)), &st));
}

// --- refusing ----------------------------------------------------------------------------------------------------------------------------------

static void test_refusals() {
  TestAllocator a;
  {
    Rig r(a, false);                               // no firmware file
    g_log.clear();
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::FirmwareMissing);
    CHECK(r.firmware.asked.size() == 1 && r.firmware.asked[0] == "xow_dongle.bin");
    CHECK(g_log.size() == 1 && g_log[0].find("xow_dongle.bin") != std::string::npos && g_log[0].find("/firmware") != std::string::npos &&
          g_log[0].find("Xbox Wireless Adapter") != std::string::npos);
    CHECK(r.host.chunks == 0 && r.host.ivb_loads == 0 && r.host.commands.empty());
    for (auto& p : r.pads) CHECK(!p);
  }
  {
    Rig r(a, false);                               // the slim adapter wants its own file, and the first one's does not do
    r.host.product = 0x02FE;
    r.firmware.files["xow_dongle.bin"] = firmware_file(0x8000, 0x200);
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::FirmwareMissing && r.firmware.asked.size() == 1 && r.firmware.asked[0] == "xow_dongle_045e_02fe.bin");
    r.firmware.files["xow_dongle_045e_02fe.bin"] = firmware_file(0x4000, 0x100);
    CHECK(r.open() == 8 && r.status == XboxWirelessStatus::Ok);
  }
  {
    Rig r(a);                                      // a built-in adapter falls back to the first generation's file
    r.host.product = 0x02F9;
    CHECK(r.open() == 8 && r.firmware.asked.size() == 2 && r.firmware.asked[1] == "xow_dongle.bin");
  }
  {
    Rig r(a);
    r.environment.firmware = nullptr;              // no file system yet
    g_log.clear();
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::NoFirmwareSource && g_log.size() == 1);
  }
  {
    Rig r(a);
    r.host.product = 0x0719;                       // the Xbox 360 receiver: not this driver's
    g_log.clear();
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::NotAdapter && g_log.empty() && r.firmware.asked.empty());
    r.host.vendor = 0x1234;
    r.host.product = 0x02E6;
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::NotAdapter);
  }
  {
    Rig r(a);
    r.host.firmware_starts = false;
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::FirmwareNotStarted && r.host.ivb_loads == 1);
  }
  {
    Rig r(a);
    r.host.bulk_in_supported = false;              // (a controller without bulk IN streams)
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::UsbUnsupported);
  }
  {
    Rig r(a);
    r.host.config.resize(9 + 9);                   // the interface without its endpoints
    r.host.config[2] = 18;
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::NoEndpoints);
  }
  {
    Rig r(a);
    r.host.present = false;
    CHECK(r.open() == 0);
  }
  CHECK(a.live == 0);
}

// --- the firmware download ---------------------------------------------------------------------------------------------------------------------

static void test_firmware_upload() {
  TestAllocator a;
  {
    Rig r(a);
    const Bytes file = r.firmware.files["xow_dongle.bin"];
    CHECK(r.open() == 8 && r.status == XboxWirelessStatus::Ok);
    // ILM 0x8000 bytes in 0x3900-byte chunks (3), DLM 0x200 bytes (1).
    CHECK(r.host.chunks == 4 && r.host.bad_packets == 0);
    CHECK(r.host.regs[xw::mt::kTxCpuFromFceCpuDescIdx] == 4);
    bool same = true;
    for (uint32_t i = 0; i < 0x8000; ++i) same = same && r.host.memory[xw::mt::kIlmOffset + i] == file[32 + i];
    for (uint32_t i = 0; i < 0x200; ++i) same = same && r.host.memory[xw::mt::kDlmOffset + i] == file[32 + 0x8000 + i];
    CHECK(same);
    CHECK(r.host.cfg[xw::mt::kUsbU3DmaCfg] == 0x00C00020);
    CHECK((r.host.regs[xw::mt::kMcuComReg0] & 3) == 3 && r.host.regs[xw::mt::kFcePseCtrl] == 1 && r.host.regs[xw::mt::kFcePdmaGlobalConf] == 0x44 &&
          r.host.regs[xw::mt::kTxCpuFromFceBasePtr] == 0x400230 && r.host.regs[xw::mt::kFceSkipFs] == 3);
    // The order: DMA and FCE set up, each chunk after its address and length, the IVB load after the last chunk, the radio after that.
    auto at = [&](const std::string& prefix) {
      for (size_t i = 0; i < r.host.ops.size(); ++i)
        if (r.host.ops[i].compare(0, prefix.size(), prefix) == 0) return static_cast<long>(i);
      return -1L;
    };
    const long cfg = at("cfg "), first_chunk = at("chunk "), ivb = at("ivb"), first_mcu = at("mcu ");
    CHECK(cfg >= 0 && first_chunk > cfg && ivb > first_chunk && first_mcu > ivb);
    CHECK(r.host.ops[first_chunk - 1] == "fce " + std::to_string(xw::mt::kFceDmaLen) + " " + std::to_string(0x3900u << 16));
    CHECK(r.host.ops[first_chunk - 2] == "fce " + std::to_string(xw::mt::kFceDmaAddr) + " " + std::to_string(xw::mt::kIlmOffset));
    CHECK(r.host.resets == 0);
    // The radio: Q_SELECT, radio on, the address, the quietest channel (149 in the model), the beacon and its timer, the light.
    CHECK(r.adapter->channel() == 149 && r.adapter->address() == kAdapterMac);
    CHECK(r.host.regs[xw::mt::kMacAddrDw0] == 0xA1B44562u && r.host.regs[xw::mt::kMacBssidDw1] == 0xC3B2);
    CHECK(r.host.regs[xw::mt::kMacSysCtrl] == 0x0C && (r.host.regs[xw::mt::kBeaconTimeCfg] & 0xFFFF) == 1600);
    CHECK(!r.host.commands.empty() && r.host.commands[0].command == xw::mt::kCmdFunSetOp && r.host.commands[1].command == xw::mt::kCmdPowerSavingOp &&
          get32(r.host.commands[1].payload, 0) == xw::mt::kRadioOn);
    const McuCommand* beacon = r.host.last_command(xw::mt::kCmdBurstWrite);
    CHECK(beacon && get32(beacon->payload, 0) == xw::mt::kMcuMemmapWlan + xw::mt::kBeaconBase);
    const McuCommand* channel = r.host.last_command(xw::mt::kCmdSwitchChannelOp);
    CHECK(channel && channel->payload[0] == 149);
    const McuCommand* led = r.host.last_command(xw::mt::kCmdLedModeOp);
    CHECK(led && get32(led->payload, 0) == xw::mt::kLedOn);
    for (auto& p : r.pads) CHECK(p && p->present() && !p->connected());
    CHECK(strcmp(r.pads[0]->info().name, "Xbox Wireless Controller") == 0 && r.pads[0]->info().protocol == Protocol::XboxOne &&
          r.pads[0]->info().product_id == 0x02E6);
  }
  {
    Rig r(a);
    r.host.regs[xw::mt::kMcuComReg0] = 1;          // firmware left running: reset first
    CHECK(r.open() == 8 && r.host.resets == 1);
  }
  {
    Rig r(a);
    for (uint8_t& b : r.host.eeprom) b = 0xFF;     // no address burned in: a locally administered one
    CHECK(r.open() == 8 && (r.adapter->address().bytes[0] & 0x02) && !(r.adapter->address().bytes[0] & 0x01));
  }
  CHECK(a.live == 0);
}

static void test_damaged_firmware() {
  TestAllocator a;
  const Bytes good = firmware_file(0x100, 0x40);
  // Every truncation is refused before anything is sent.
  for (size_t len = 1; len < good.size(); len += (len < 80 ? 1 : 37)) {
    Rig r(a, false);
    r.firmware.files["xow_dongle.bin"] = Bytes(good.begin(), good.begin() + len);
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::FirmwareInvalid && r.host.chunks == 0);
  }
  // Every bit flip of the header: the lengths no longer add up (refused), or a version or the build time changed (fine).
  for (size_t bit = 0; bit < 32 * 8; ++bit) {
    Rig r(a, false);
    Bytes flipped = good;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    r.firmware.files["xow_dongle.bin"] = flipped;
    const uint32_t made = r.open();
    if (bit < 64) CHECK(made == 0 && r.status == XboxWirelessStatus::FirmwareInvalid && r.host.chunks == 0);
    else CHECK(made == 8 && r.host.bad_packets == 0);
  }
  // An ILM no longer than the interrupt vector block is refused too.
  {
    Rig r(a, false);
    r.firmware.files["xow_dongle.bin"] = firmware_file(0x40, 0x10);
    CHECK(r.open() == 0 && r.status == XboxWirelessStatus::FirmwareInvalid);
  }
  CHECK(a.live == 0);
}

// --- controllers -------------------------------------------------------------------------------------------------------------------------------

static void test_controllers() {
  TestAllocator a;
  Rig r(a);
  CHECK(r.open() == 8);
  IGameController& pad = *r.pads[0];

  // A controller associates: it is put in the station table and answered, but is not connected until it announces itself.
  r.associate(kPadA);
  r.poll_all();
  CHECK(r.host.regs[xw::mt::kWcidAddrBase + 8] == 0x118DED7Cu && r.host.regs[xw::mt::kWcidAddrBase + 12] == 0x3322);
  CHECK(!r.host.frames.empty());
  const WlanPacket& response = r.host.frames.back();
  CHECK(response.wcid == 1 && response.frame.type == 0 && response.frame.subtype == xw::kSubtypeAssociationResponse && response.frame.destination == kPadA &&
        response.frame.bssid == kAdapterMac && response.body.size() == 6 && response.body[2] == 0 && response.body[4] == 1 && response.body[5] == 0xC0);
  CHECK(!pad.connected());

  // Its GIP announcement: it is told to power on (and its guide light is lit); now it is connected.
  size_t mark = r.host.frame_count();
  r.gip(kPadA, 1, kAnnounce);
  CHECK(pad.poll() && pad.connected());
  std::vector<Bytes> sent = r.host.gip_to(kPadA, mark);
  CHECK(sent.size() == 2 && sent[0].size() == 5 && sent[0][0] == xbox::kGipPower && sent[0][4] == 0x00 && sent[1][0] == xbox::kGipLed);
  CHECK(r.host.frames.back().wcid == 1);
  for (uint32_t i = 1; i < 8; ++i) CHECK(!r.pads[i]->connected());

  // Input through the 802.11 wrapping, with and without the radio's padding.
  r.gip(kPadA, 1, gip_input(0x10 | 0x04, 0x01, 1023, 0, 1000, -1000, 5, -5));
  CHECK(pad.poll());
  CHECK(pad.state().buttons == (kSouth | kStart | kDpadUp) && pad.state().left_trigger == 255 && pad.state().left_x == 1000 && pad.state().right_y == -5);
  r.gip(kPadA, 1, gip_input(0x20, 0, 0, 512, 0, 0, 0, 0), true);
  CHECK(pad.poll() && pad.state().buttons == kEast && pad.state().right_trigger == 128);
  CHECK(!pad.poll());                                // nothing new

  // The guide button wants an acknowledgement; it is sent back to the controller.
  mark = r.host.frame_count();
  r.gip(kPadA, 1, {xbox::kGipGuide, 0x30, 0x07, 0x02, 0x01, 0x5B});
  CHECK(pad.poll() && (pad.state().buttons & kGuide));
  sent = r.host.gip_to(kPadA, mark);
  CHECK(sent.size() == 1 && sent[0][0] == xbox::kGipAcknowledge && sent[0][2] == 0x07 && sent[0][5] == xbox::kGipGuide);
  r.gip(kPadA, 1, {xbox::kGipGuide, 0x20, 0x08, 0x02, 0x00, 0x5B});
  CHECK(pad.poll() && !(pad.state().buttons & kGuide));

  // The battery, which input does not overwrite with "wired".
  r.gip(kPadA, 1, {xbox::kGipStatus, 0x20, 0x09, 0x04, 0x04 | 0x02, 0, 0, 0});
  CHECK(pad.poll() && pad.state().battery_kind == BatteryKind::Alkaline && pad.state().battery_percent == 66);
  r.gip(kPadA, 1, gip_input(0, 0, 0, 0, 0, 0, 0, 0));
  CHECK(pad.poll() && pad.state().battery_kind == BatteryKind::Alkaline);

  // Rumble goes out as GIP in a QoS frame to the controller's station.
  mark = r.host.frame_count();
  Output o;
  o.low_frequency = 0xFFFF;
  o.high_frequency = 0x8000;
  CHECK(pad.set_output(o));
  sent = r.host.gip_to(kPadA, mark);
  CHECK(sent.size() == 1 && sent[0][0] == xbox::kGipRumble && sent[0][8] == 99 && sent[0][9] == 49 && r.host.frames.back().wcid == 1);
  CHECK(!r.pads[1]->set_output(o));                 // (nobody there)

  // A frame claiming to be from the controller with another station's WCID, and one from a stranger, are ignored.
  r.gip(kPadA, 5, gip_input(0x80, 0, 0, 0, 0, 0, 0, 0));
  r.gip(kPadB, 2, gip_input(0x80, 0, 0, 0, 0, 0, 0, 0));
  CHECK(!pad.poll() && pad.state().buttons == 0 && !r.pads[1]->poll());

  // A second controller takes the second slot.
  r.associate(kPadB);
  r.gip(kPadB, 2, kAnnounce);
  r.gip(kPadB, 2, gip_input(0x40, 0, 0, 0, 0, 0, 0, 0));
  r.poll_all();
  CHECK(r.pads[1]->connected() && r.pads[1]->state().buttons == kWest && r.host.regs[xw::mt::kWcidAddrBase + 16] == 0x448DED7Cu);

  // Departures: a disassociation, and the radio losing a controller.
  r.host.push(rx_frame(1, management_from(kPadA, xw::kSubtypeDisassociation, {0x08, 0x00})));
  CHECK(pad.poll() && !pad.connected() && pad.state().buttons == 0 && r.host.regs[xw::mt::kWcidAddrBase + 8] == 0);
  CHECK(!pad.set_output(o));
  r.host.push(rx_event(xw::xbox_table::kEventClientLost, {2}), 0x85);
  CHECK(r.pads[1]->poll() && !r.pads[1]->connected());
  // The first controller comes back, into a free slot, and starts over.
  r.associate(kPadA);
  r.gip(kPadA, 1, gip_input(0x10, 0, 0, 0, 0, 0, 0, 0));   // (its announcement went missing: input counts)
  r.poll_all();
  CHECK(pad.connected() && pad.state().buttons == kSouth);
  // It says it switches off.
  r.gip(kPadA, 1, {xbox::kGipPower, 0x20, 0x0A, 0x01, xw::kGipPowerOff});
  CHECK(pad.poll() && !pad.connected());

  // Nine controllers: the ninth finds no slot and is not answered.
  for (uint8_t i = 0; i < 9; ++i) {
    MacAddress m = kPadA;
    m.bytes[5] = static_cast<uint8_t>(0x80 + i);
    r.associate(m);
    r.gip(m, static_cast<uint8_t>(i + 1), kAnnounce);
  }
  mark = r.host.frame_count();
  r.poll_all();
  uint32_t connected = 0;
  for (auto& p : r.pads) connected += p->connected() ? 1 : 0;
  CHECK(connected == 8);

  // Garbage on both endpoints (truncated and bit-flipped versions of real traffic) does no harm.
  const Bytes real = rx_frame(1, qos_from(kPadA, gip_input(0xFF, 0xFF, 1, 2, 3, 4, 5, 6)), true);
  for (size_t bit = 0; bit < real.size() * 8; bit += 3) {
    Bytes flipped = real;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    r.host.push(flipped, (bit & 1) ? 0x84 : 0x85);
    r.host.push(Bytes(real.begin(), real.begin() + bit / 8));
  }
  for (int i = 0; i < 40; ++i) r.poll_all();
  CHECK(r.host.bad_packets == 0);
}

static void test_pairing() {
  TestAllocator a;
  Rig r(a);
  CHECK(r.open() == 8 && !r.adapter->pairing());
  // A pairing request while pairing is closed gets no answer.
  size_t mark = r.host.frame_count();
  r.host.push(rx_frame(0xFF, management_from(kPadA, xw::xbox_table::kSubtypePair, {0x01})));
  r.poll_all();
  CHECK(r.host.frame_count() == mark);
  // The adapter's button opens it: the beacon says so and the light blinks.
  r.host.push(rx_event(xw::xbox_table::kEventButtonPress, {}), 0x85);
  r.poll_all();
  CHECK(r.adapter->pairing());
  const McuCommand* beacon = r.host.last_command(xw::mt::kCmdBurstWrite);
  // burst address (4) + TXWI (20) + header (24) + fixed fields and elements before the vendor element (17)
  CHECK(beacon && beacon->payload[4 + 20 + 24 + 17] == 0xDD && beacon->payload[4 + 20 + 24 + 17 + xw::xbox_table::kBeaconPairingIndex] == 1);
  CHECK(get32(r.host.last_command(xw::mt::kCmdLedModeOp)->payload, 0) == xw::mt::kLedBlink);
  // A controller asks: it is answered and pairing closes.
  r.host.push(rx_frame(0xFF, management_from(kPadA, xw::xbox_table::kSubtypePair, {0x01})));
  r.poll_all();
  CHECK(!r.adapter->pairing() && r.host.frame_count() == mark + 1);
  const WlanPacket& answer = r.host.frames.back();
  CHECK(answer.frame.type == 0 && answer.frame.subtype == xw::xbox_table::kSubtypePair && answer.frame.destination == kPadA &&
        answer.body.size() == sizeof(xw::xbox_table::kPairResponseBody));
  beacon = r.host.last_command(xw::mt::kCmdBurstWrite);
  CHECK(beacon->payload[4 + 20 + 24 + 17 + xw::xbox_table::kBeaconPairingIndex] == 0);
  CHECK(get32(r.host.last_command(xw::mt::kCmdLedModeOp)->payload, 0) == xw::mt::kLedOn);
  // The host can open and close it too.
  CHECK(r.adapter->set_pairing(true) && r.adapter->pairing());
  CHECK(r.adapter->set_pairing(false) && !r.adapter->pairing());
}

static void test_unplug() {
  TestAllocator a;
  {
    Rig r(a);
    CHECK(r.open() == 8);
    r.associate(kPadA);
    r.gip(kPadA, 1, kAnnounce);
    r.gip(kPadA, 1, gip_input(0x10, 0, 0, 0, 0, 0, 0, 0));
    r.poll_all();
    CHECK(r.pads[0]->connected());
    // Unplugged with input still queued: nothing more is read or sent, and every pad says it is gone.
    r.gip(kPadA, 1, gip_input(0x20, 0, 0, 0, 0, 0, 0, 0));
    const size_t frames = r.host.frame_count(), commands = r.host.commands.size();
    r.host.unplug();
    for (auto& p : r.pads) {
      CHECK(!p->poll() && !p->present() && !p->connected());
      Output o;
      o.low_frequency = 1;
      CHECK(!p->set_output(o));
      p->shutdown();
    }
    CHECK(r.pads[0]->state().buttons == kSouth && r.host.frame_count() == frames && r.host.commands.size() == commands);
    CHECK(!r.adapter->set_pairing(true));
    r.adapter = SharedPointer<XboxWirelessAdapter>();
    for (auto& p : r.pads) p = UniquePointer<IGameController>();   // the last one frees the adapter (it sends nothing: it is gone)
    CHECK(r.host.commands.size() == commands);
  }
  {
    // Let go of while still plugged in: the radio is turned off.
    Rig r(a);
    CHECK(r.open() == 8);
    r.adapter = SharedPointer<XboxWirelessAdapter>();
    for (auto& p : r.pads) p = UniquePointer<IGameController>();
    const McuCommand* off = r.host.last_command(xw::mt::kCmdPowerSavingOp);
    CHECK(off && get32(off->payload, 0) == xw::mt::kRadioOff && get32(r.host.last_command(xw::mt::kCmdLedModeOp)->payload, 0) == xw::mt::kLedOff);
  }
  CHECK(a.live == 0);
}

// The game controller service: the pads wait in its idle pool and get a player slot only while a controller is connected.
static void test_service() {
  TestAllocator a;
  {
    Rig r(a);
    CHECK(r.open() == 8);
    GameControllerService service(nullptr, a);
    for (auto& p : r.pads) CHECK(service.add(util::move(p)) == -1);   // (none connected yet: all idle)
    service.step(false);
    CHECK(service.count() == 0);
    r.associate(kPadA);
    r.gip(kPadA, 1, kAnnounce);
    service.step(false);
    CHECK(service.count() == 1);
    Info info;
    CHECK(service.info(0, &info) && strcmp(info.name, "Xbox Wireless Controller") == 0);
    r.gip(kPadA, 1, gip_input(0x10, 0, 0, 0, 0, 0, 0, 0));
    service.step(false);
    State s;
    uint32_t packet = 0;
    CHECK(service.snapshot(0, &s, &packet) && s.buttons == kSouth);
    const size_t mark = r.host.frame_count();
    CHECK(service.set_vibration(0, 0x4000, 0x2000));
    CHECK(r.host.gip_to(kPadA, mark).size() == 1);
    r.host.push(rx_event(xw::xbox_table::kEventClientLost, {1}), 0x85);
    service.step(false);
    service.step(false);
    CHECK(service.count() == 0);
    r.associate(kPadB);
    r.gip(kPadB, 1, kAnnounce);
    service.step(false);
    service.step(false);
    CHECK(service.count() == 1);
    r.host.unplug();
    service.step(true);
    service.step(true);
    CHECK(service.count() == 0);
    r.adapter = SharedPointer<XboxWirelessAdapter>();
    service.stop();
  }
  CHECK(a.live == 0);
}

// Taken up before the file system is mounted: the pads are made, and the adapter starts once a firmware source is registered.
static void test_deferred_start() {
  TestAllocator a;
  {
    Rig r(a);
    set_xbox_wireless_environment(nullptr);
    XboxWirelessStatus status = XboxWirelessStatus::Ok;
    CHECK(open_xbox_wireless_adapter(r.host, 0, a, ArrayView<UniquePointer<IGameController>>(r.pads), &status, &r.adapter) == 8);
    CHECK(status == XboxWirelessStatus::NoFirmwareSource && r.host.chunks == 0);
    r.poll_all();
    CHECK(r.host.chunks == 0 && r.pads[0]->present() && !r.pads[0]->connected());
    g_log.clear();
    set_xbox_wireless_environment(&r.environment);
    r.associate(kPadA);
    r.gip(kPadA, 1, kAnnounce);
    r.poll_all();
    CHECK(r.host.chunks == 4 && r.adapter->channel() == 149 && g_log.size() == 1 && g_log[0].find("ready") != std::string::npos);
    r.poll_all();
    CHECK(r.pads[0]->connected());
    set_xbox_wireless_environment(nullptr);
  }
  {
    Rig r(a, false);                                 // the file system comes, but without the file: refused then, and the pads leave
    set_xbox_wireless_environment(nullptr);
    CHECK(open_xbox_wireless_adapter(r.host, 0, a, ArrayView<UniquePointer<IGameController>>(r.pads)) == 8);
    g_log.clear();
    set_xbox_wireless_environment(&r.environment);
    r.poll_all();
    CHECK(g_log.size() == 1 && g_log[0].find("xow_dongle.bin") != std::string::npos && r.host.chunks == 0);
    for (auto& p : r.pads) CHECK(!p->present() && !p->connected());
    r.poll_all();
    CHECK(g_log.size() == 1 && r.firmware.asked.size() == 1);   // (tried once)
    set_xbox_wireless_environment(nullptr);
  }
  CHECK(a.live == 0);
}

// Several threads at once on one adapter: the pads polled and driven from their own threads while the radio delivers traffic.
struct ThreadArgs { Rig* rig; uint32_t slot; int rounds; };

static void* pad_thread(void* p) {
  ThreadArgs* t = static_cast<ThreadArgs*>(p);
  IGameController& pad = *t->rig->pads[t->slot];
  for (int i = 0; i < t->rounds; ++i) {
    (void)pad.poll();
    if (pad.connected()) {
      Output o;
      o.low_frequency = static_cast<uint16_t>(i);
      (void)pad.set_output(o);
    }
    (void)pad.state().buttons;
  }
  return nullptr;
}

static void* radio_thread(void* p) {
  ThreadArgs* t = static_cast<ThreadArgs*>(p);
  for (int i = 0; i < t->rounds; ++i) {
    MacAddress m = kPadA;
    m.bytes[5] = static_cast<uint8_t>(i % 4);
    t->rig->associate(m);
    t->rig->gip(m, static_cast<uint8_t>(i % 4 + 1), kAnnounce);
    t->rig->gip(m, static_cast<uint8_t>(i % 4 + 1), gip_input(static_cast<uint8_t>(i), 0, 0, 0, 0, 0, 0, 0));
    if (i % 7 == 0) t->rig->host.push(rx_event(xw::xbox_table::kEventClientLost, {static_cast<uint8_t>(i % 4 + 1)}), 0x85);
    if (i % 11 == 0) (void)t->rig->adapter->set_pairing(i % 2 == 0);
  }
  return nullptr;
}

static void test_threads() {
  TestAllocator a;
  {
    Rig r(a);
    CHECK(r.open() == 8);
    ThreadArgs args[9];
    pthread_t threads[9];
    for (uint32_t i = 0; i < 8; ++i) {
      args[i] = ThreadArgs{&r, i, 400};
      pthread_create(&threads[i], nullptr, &pad_thread, &args[i]);
    }
    args[8] = ThreadArgs{&r, 0, 300};
    pthread_create(&threads[8], nullptr, &radio_thread, &args[8]);
    for (pthread_t& t : threads) pthread_join(t, nullptr);
    for (int i = 0; i < 50; ++i) r.poll_all();
    CHECK(r.host.bad_packets == 0);
    r.adapter = SharedPointer<XboxWirelessAdapter>();
  }
  CHECK(a.live == 0);
}

int main() {
  test_frames();
  test_refusals();
  test_firmware_upload();
  test_damaged_firmware();
  test_controllers();
  test_pairing();
  test_unplug();
  test_service();
  test_deferred_start();
  test_threads();
  printf("XboxWirelessAdapterTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}

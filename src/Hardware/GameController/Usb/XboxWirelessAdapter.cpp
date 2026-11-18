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

#include "Hardware/GameController/Usb/XboxWirelessAdapter.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtFirmware.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/String.hpp"

namespace hardware {

using namespace xbox_wireless;

namespace {

constexpr uint16_t kMicrosoft = 0x045E;
constexpr uint8_t kTransferBulk = 2;
constexpr uint32_t kMcuStartPolls = 100;          // the MCU says it runs within a few milliseconds; give it a second
constexpr uint32_t kMcuStartPollUs = 10000;
constexpr uint32_t kReceiveBudget = 32;           // packets taken per endpoint and pump
constexpr uint32_t kScratchBytes = mt::kFirmwareChunk + 64;

struct AdapterModel {
  uint16_t product;
  const char* name;
  const char* firmware[2];
  uint32_t firmware_count;
};

// The firmware files are named as xow and xone name them under /lib/firmware (the slim adapter's differs from the first one's). For the
// built-in adapters no name is known: a name of their own is tried first, then the first generation's file. UNCERTAIN beyond 02E6 and 02FE.
constexpr AdapterModel kModels[] = {
    {0x02E6, "Xbox Wireless Adapter for Windows", {"xow_dongle.bin", nullptr}, 1},
    {0x02FE, "Xbox Wireless Adapter for Windows (slim)", {"xow_dongle_045e_02fe.bin", nullptr}, 1},
    {0x02F9, "Xbox Wireless Adapter (built in)", {"xow_dongle_045e_02f9.bin", "xow_dongle.bin"}, 2},
    {0x091E, "Xbox Wireless Adapter (built in)", {"xow_dongle_045e_091e.bin", "xow_dongle.bin"}, 2},
};

const AdapterModel* find_model(uint16_t vendor, uint16_t product) {
  if (vendor != kMicrosoft) return nullptr;
  for (const AdapterModel& m : kModels)
    if (m.product == product) return &m;
  return nullptr;
}

const XboxWirelessEnvironment* g_environment = nullptr;   // (atomic: set by the kernel, read from the service's thread)

void say(const XboxWirelessEnvironment& environment, allocators::IAllocator& allocator, const char* name, const char* what, const char* detail) {
  if (!environment.log) return;
  String text(allocator);
  if (!text.assign(name) || !text.append(": ") || !text.append(what)) return;
  if (detail && (!text.append(" (") || !text.append(detail) || !text.append(")"))) return;
  environment.log(environment.log_context, text.view());
}

uint32_t round_up4(uint32_t n) { return (n + 3u) & ~3u; }

bool valid_address(const MacAddress& a) {
  bool zero = true, ones = true;
  for (uint8_t b : a.bytes) {
    zero = zero && b == 0x00;
    ones = ones && b == 0xFF;
  }
  return !zero && !ones && !(a.bytes[0] & 0x01);
}

constexpr MacAddress kBroadcast{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

}  // namespace

bool is_xbox_wireless_adapter(uint16_t vendor, uint16_t product) { return find_model(vendor, product) != nullptr; }

const char* xbox_wireless_adapter_name(uint16_t product) {
  const AdapterModel* m = find_model(kMicrosoft, product);
  return m ? m->name : "Xbox Wireless Adapter";
}

const char* const* xbox_wireless_firmware_names(uint16_t product, uint32_t* count) {
  const AdapterModel* m = find_model(kMicrosoft, product);
  *count = m ? m->firmware_count : 0;
  return m ? m->firmware : nullptr;
}

void set_xbox_wireless_environment(const XboxWirelessEnvironment* environment) { __atomic_store_n(&g_environment, environment, __ATOMIC_RELEASE); }

const char* xbox_wireless_status_text(XboxWirelessStatus status) {
  switch (status) {
    case XboxWirelessStatus::Ok: return "ready";
    case XboxWirelessStatus::NotAdapter: return "not an Xbox Wireless Adapter";
    case XboxWirelessStatus::NoFirmwareSource: return "no firmware folder to load the firmware from";
    case XboxWirelessStatus::FirmwareMissing: return "firmware file not found in /firmware; copy it from Microsoft's driver package (docs/GameControllers.md)";
    case XboxWirelessStatus::FirmwareInvalid: return "firmware file is damaged or not this adapter's";
    case XboxWirelessStatus::NoEndpoints: return "no bulk endpoints for the radio";
    case XboxWirelessStatus::UsbUnsupported: return "the USB host controller cannot receive bulk transfers without waiting";
    case XboxWirelessStatus::UsbError: return "a USB transfer failed";
    case XboxWirelessStatus::FirmwareNotStarted: return "the firmware did not start";
    case XboxWirelessStatus::NoMemory: return "out of memory";
  }
  return "unknown";
}

// --- the adapter ------------------------------------------------------------------------------------------------------------------------------

XboxWirelessAdapter::XboxWirelessAdapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator)
    : host_(host), port_(port), generation_(host.port_generation(port)), allocator_(allocator), scratch_(allocator) {}

bool XboxWirelessAdapter::present() const {
  kernel::ScopedMutex lock(mutex_);
  return !gone() && !refused_;
}

void XboxWirelessAdapter::start_when_firmware_is_there(const char* name) {
  kernel::ScopedMutex lock(mutex_);
  deferred_ = !running_;
  name_ = name;
}

void XboxWirelessAdapter::try_deferred_start() {
  const XboxWirelessEnvironment* environment = __atomic_load_n(&g_environment, __ATOMIC_ACQUIRE);
  if (!environment || !environment->firmware) return;
  deferred_ = false;
  channel_dwell_us_ = environment->channel_dwell_us;
  const char* detail = nullptr;
  const XboxWirelessStatus status = start_locked(*environment->firmware, &detail);
  if (status == XboxWirelessStatus::Ok) {
    say(*environment, allocator_, name_, "ready, firmware", detail);
    return;
  }
  refused_ = true;
  say(*environment, allocator_, name_, xbox_wireless_status_text(status), detail);
}

bool XboxWirelessAdapter::read32(uint32_t offset, uint32_t* value) {
  uint8_t data[4] = {};
  if (!host_.control_in(port_, mt::kTypeVendorIn, mt::kVendMultiRead, static_cast<uint16_t>(offset >> 16), static_cast<uint16_t>(offset), ByteView(data)))
    return false;
  *value = uint32_t{data[0]} | uint32_t{data[1]} << 8 | uint32_t{data[2]} << 16 | uint32_t{data[3]} << 24;
  return true;
}

bool XboxWirelessAdapter::write32(uint32_t offset, uint32_t value) {
  const uint8_t data[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return host_.control_out_data(port_, mt::kTypeVendorOut, mt::kVendMultiWrite, static_cast<uint16_t>(offset >> 16), static_cast<uint16_t>(offset),
                                ConstByteView(data));
}

bool XboxWirelessAdapter::write_cfg32(uint32_t offset, uint32_t value) {
  const uint8_t data[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return host_.control_out_data(port_, mt::kTypeVendorOut, mt::kVendWriteCfg, static_cast<uint16_t>(offset >> 16), static_cast<uint16_t>(offset),
                                ConstByteView(data));
}

// mt76u_single_wr: the low half to the register, the high half to the register + 2, each as the request's value.
bool XboxWirelessAdapter::write_fce32(uint32_t offset, uint32_t value) {
  return host_.control_out(port_, mt::kTypeVendorOut, mt::kVendWriteFce, static_cast<uint16_t>(value), static_cast<uint16_t>(offset)) &&
         host_.control_out(port_, mt::kTypeVendorOut, mt::kVendWriteFce, static_cast<uint16_t>(value >> 16), static_cast<uint16_t>(offset + 2));
}

bool XboxWirelessAdapter::bulk_out(uint8_t endpoint, ConstByteView packet) {
  // The host controller wants a writable buffer: the packet is copied into the scratch buffer unless it is already there.
  ByteView buffer = scratch_.writable_view();
  if (packet.size() > buffer.size()) return false;
  ByteView data = buffer.first(packet.size());
  if (packet.data() != data.data() && !data.copy_from(packet)) return false;
  uint32_t transferred = 0;
  return host_.bulk_transfer(port_, endpoint, data, &transferred) && transferred == packet.size();
}

bool XboxWirelessAdapter::mcu_command(uint8_t command, ConstByteView payload) {
  const ByteView buffer = scratch_.writable_view();
  const uint32_t n = encode_mcu_message(command, mcu_sequence_, payload, buffer);
  mcu_sequence_ = static_cast<uint8_t>(mcu_sequence_ % 15 + 1);   // 1..15 (0 is the firmware download's)
  return n && bulk_out(command_out_, ConstByteView(buffer).first(n));
}

bool XboxWirelessAdapter::mcu_u32_pair(uint8_t command, uint32_t first, uint32_t second) {
  uint8_t payload[8];
  ByteWriter w{ByteView(payload)};
  w.le32(first);
  w.le32(second);
  return mcu_command(command, ConstByteView(payload));
}

bool XboxWirelessAdapter::send_frame(uint8_t wcid, ConstByteView frame) {
  const ByteView buffer = scratch_.writable_view();
  const uint32_t n = encode_wlan_packet(wcid, frame, buffer);
  return n && bulk_out(data_out_, ConstByteView(buffer).first(n));
}

bool XboxWirelessAdapter::set_led(uint32_t mode) { return mcu_u32_pair(mt::kCmdLedModeOp, mode, 0); }

bool XboxWirelessAdapter::write_station(uint8_t wcid, const MacAddress& address) {
  const uint32_t base = mt::kWcidAddrBase + uint32_t{wcid} * mt::kWcidEntryBytes;
  const uint8_t* a = address.bytes;
  return write32(base, uint32_t{a[0]} | uint32_t{a[1]} << 8 | uint32_t{a[2]} << 16 | uint32_t{a[3]} << 24) && write32(base + 4, uint32_t{a[4]} | uint32_t{a[5]} << 8);
}

bool XboxWirelessAdapter::write_beacon() {
  uint8_t body[64], frame[128], block[160], burst[192];
  const uint32_t body_length = encode_beacon_body(channel_, pairing_, ByteView(body));
  const uint32_t frame_length = encode_management(kSubtypeBeacon, kBroadcast, address_, beacon_sequence_++, ConstByteView(body).first(body_length), ByteView(frame));
  if (!body_length || !frame_length) return false;
  // The beacon memory holds the TXWI and the frame, the frame padded to a whole word.
  const uint32_t block_length = mt::kTxwiBytes + round_up4(frame_length);
  if (block_length > sizeof(block)) return false;
  ByteView all(block);
  all.first(block_length).fill(0);
  if (encode_txwi(0xFF, frame_length, false, all.first(mt::kTxwiBytes)) != mt::kTxwiBytes) return false;
  if (!all.subview(mt::kTxwiBytes, frame_length).copy_from(ConstByteView(frame).first(frame_length))) return false;
  const uint32_t n = encode_burst_write(mt::kBeaconBase, ConstByteView(block).first(block_length), ByteView(burst));
  return n && mcu_command(mt::kCmdBurstWrite, ConstByteView(burst).first(n));
}

XboxWirelessStatus XboxWirelessAdapter::find_endpoints() {
  uint8_t config[512];
  uint16_t length = 0;
  if (!host_.configuration_descriptor(port_, ByteView(config), &length)) return XboxWirelessStatus::UsbError;
  UsbDescriptorWalker walker(ConstByteView(config).at_most(length));
  ConstByteView d;
  UsbConfigurationInfo info;
  if (!walker.next(&d) || !parse_usb_configuration(d, &info)) return XboxWirelessStatus::NoEndpoints;
  configuration_ = info.value;
  // The radio's interface is the first one (alternate setting 0) with bulk endpoints: the first bulk OUT takes the firmware and the MCU
  // commands (mt76's MT_EP_OUT_INBAND_CMD), the second the 802.11 frames (MT_EP_OUT_AC_BE); the bulk IN endpoints carry frames and events,
  // which are told apart by their FCE info word, so they are read alike. UNCERTAIN: the adapter's descriptor was not seen.
  bool in_radio = false, done = false;
  uint32_t outs = 0;
  while (!done && walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorInterface && parse_usb_interface(d, &interface)) {
      if (in_radio && (outs || in_count_)) done = true;
      in_radio = interface.alternate_setting == 0;
    } else if (in_radio && type == kUsbDescriptorEndpoint && parse_usb_endpoint(d, &endpoint) && endpoint.transfer_type() == kTransferBulk) {
      if (endpoint.is_in()) {
        if (in_count_ < 2) {
          in_endpoints_[in_count_] = endpoint.address;
          in_max_[in_count_++] = endpoint.max_packet;
        }
      } else if (outs == 0) {
        command_out_ = endpoint.address;
        command_out_max_ = endpoint.max_packet;
        ++outs;
      } else if (outs == 1) {
        data_out_ = endpoint.address;
        data_out_max_ = endpoint.max_packet;
        ++outs;
      }
    }
  }
  if (outs == 0 || in_count_ == 0) return XboxWirelessStatus::NoEndpoints;
  if (outs == 1) {
    data_out_ = command_out_;
    data_out_max_ = command_out_max_;
  }
  return XboxWirelessStatus::Ok;
}

// After mt76x2u_mcu_load_firmware (and xow's Mt76::loadFirmware, which follows it): a running MCU is reset first, the USB DMA and the FCE
// are set up, the ILM and the DLM go in chunk by chunk (each: the target address and length through MT_VEND_WRITE_FCE, the bytes as an
// in-band message on the command endpoint, then the CPU descriptor index moved on), and MT_VEND_DEV_MODE 0x12 starts the firmware, which
// says so in bit 0 of MT_MCU_COM_REG0.
XboxWirelessStatus XboxWirelessAdapter::load_firmware(ConstByteView file) {
  Mt76UsbFirmware firmware;
  const char* error = nullptr;
  if (!firmware.parse(file, &error)) return XboxWirelessStatus::FirmwareInvalid;
  uint32_t com = 0;
  if (!read32(mt::kMcuComReg0, &com)) return XboxWirelessStatus::UsbError;
  if (com & 1) {                                          // (left running by an earlier start: reset it)
    if (!host_.control_out(port_, mt::kTypeVendorOut, mt::kVendDevMode, mt::kDevModeReset, 0)) return XboxWirelessStatus::UsbError;
    if (kernel::running()) kernel::sleep_us(10000);
  }
  if (!write_cfg32(mt::kUsbU3DmaCfg, mt::kUsbDmaCfgValue) || !write32(mt::kFcePseCtrl, 0x01) || !write32(mt::kTxCpuFromFceBasePtr, mt::kFceBasePtrValue) ||
      !write32(mt::kTxCpuFromFceMaxCount, 0x01) || !write32(mt::kFcePdmaGlobalConf, mt::kFcePdmaValue) || !write32(mt::kFceSkipFs, mt::kFceSkipFsValue))
    return XboxWirelessStatus::UsbError;
  if (!host_.start_bulk_endpoint(port_, command_out_, command_out_max_)) return XboxWirelessStatus::UsbError;

  auto upload = [&](ConstByteView part, uint32_t base) {
    for (uint64_t offset = 0; offset < part.size();) {
      const uint64_t left = part.size() - offset;
      const uint32_t n = static_cast<uint32_t>(left < mt::kFirmwareChunk ? left : mt::kFirmwareChunk);
      if (offset > UINT32_MAX - base) return false;
      if (!write_fce32(mt::kFceDmaAddr, base + static_cast<uint32_t>(offset)) || !write_fce32(mt::kFceDmaLen, round_up4(n) << 16)) return false;
      const ByteView buffer = scratch_.writable_view();
      const uint32_t length = encode_mcu_message(0, 0, part.subview(offset, n), buffer);
      if (!length || !bulk_out(command_out_, ConstByteView(buffer).first(length))) return false;
      uint32_t index = 0;
      if (!read32(mt::kTxCpuFromFceCpuDescIdx, &index) || !write32(mt::kTxCpuFromFceCpuDescIdx, index + 1)) return false;
      offset += n;
    }
    return true;
  };
  if (!upload(firmware.ilm(), mt::kIlmOffset) || !upload(firmware.dlm(), mt::kDlmOffset)) return XboxWirelessStatus::UsbError;
  if (!host_.control_out(port_, mt::kTypeVendorOut, mt::kVendDevMode, mt::kDevModeLoadIvb, 0)) return XboxWirelessStatus::UsbError;
  bool started = false;
  for (uint32_t i = 0; i < kMcuStartPolls && !started; ++i) {
    if (!read32(mt::kMcuComReg0, &com)) return XboxWirelessStatus::UsbError;
    started = (com & 1) != 0;
    if (!started && kernel::running()) kernel::sleep_us(kMcuStartPollUs);
  }
  if (!started) return XboxWirelessStatus::FirmwareNotStarted;
  if (!write32(mt::kMcuComReg0, com | 0x02) || !write32(mt::kFcePseCtrl, 0x01)) return XboxWirelessStatus::UsbError;
  return XboxWirelessStatus::Ok;
}

// The radio, after xow's Dongle start-up as far as it is understood: receive queues, radio on, the adapter's own address as MAC and BSSID,
// the quietest channel, the beacon and its timer, the light on. The MCU's answers to commands are not waited for (xow does not either).
XboxWirelessStatus XboxWirelessAdapter::start_radio() {
  if (data_out_ != command_out_ && !host_.start_bulk_endpoint(port_, data_out_, data_out_max_)) return XboxWirelessStatus::UsbError;
  for (uint32_t i = 0; i < in_count_; ++i)
    if (!host_.start_bulk_in_stream(port_, in_endpoints_[i], in_max_[i])) return XboxWirelessStatus::UsbUnsupported;
  if (!mcu_u32_pair(mt::kCmdFunSetOp, mt::kFunQSelect, 1) || !mcu_u32_pair(mt::kCmdPowerSavingOp, mt::kRadioOn, 0)) return XboxWirelessStatus::UsbError;

  // The address burned into the adapter (EEPROM bytes 4..9); a locally administered one if it reads as nonsense. UNCERTAIN: xow reads it
  // through the eFuse registers, this through MT_VEND_READ_EEPROM as mt76u does for its EEPROM type.
  uint8_t eeprom[8] = {};
  const bool read = host_.control_in(port_, mt::kTypeVendorIn, mt::kVendReadEeprom, 0, static_cast<uint16_t>(mt::kEepromMacOffset), ByteView(eeprom));
  for (uint32_t i = 0; i < 6; ++i) address_.bytes[i] = read ? eeprom[i] : 0;
  if (!valid_address(address_)) address_ = MacAddress{{0x02, 0x58, 0x42, 0x00, 0x00, static_cast<uint8_t>(port_)}};
  const uint8_t* a = address_.bytes;
  const uint32_t low = uint32_t{a[0]} | uint32_t{a[1]} << 8 | uint32_t{a[2]} << 16 | uint32_t{a[3]} << 24, high = uint32_t{a[4]} | uint32_t{a[5]} << 8;
  if (!write32(mt::kMacAddrDw0, low) || !write32(mt::kMacAddrDw1, high) || !write32(mt::kMacBssidDw0, low) || !write32(mt::kMacBssidDw1, high) ||
      !write32(mt::kMacSysCtrl, mt::kMacSysCtrlEnable))
    return XboxWirelessStatus::UsbError;

  // Each candidate channel is listened to for a moment; the busy and idle counters clear when read.
  ChannelSample samples[sizeof(xbox_table::kChannels)];
  uint32_t count = 0;
  for (uint8_t candidate : xbox_table::kChannels) {
    uint8_t payload[8];
    uint32_t busy = 0, idle = 0;
    const uint32_t n = encode_switch_channel(candidate, ByteView(payload));
    if (!n || !mcu_command(mt::kCmdSwitchChannelOp, ConstByteView(payload).first(n)) || !read32(mt::kChBusy, &busy) || !read32(mt::kChIdle, &idle))
      return XboxWirelessStatus::UsbError;
    if (kernel::running() && channel_dwell_us_) kernel::sleep_us(channel_dwell_us_);
    if (!read32(mt::kChBusy, &busy) || !read32(mt::kChIdle, &idle)) return XboxWirelessStatus::UsbError;
    samples[count++] = ChannelSample{candidate, busy, idle};
  }
  channel_ = choose_channel(ArrayView<const ChannelSample>(samples).first(count));
  uint8_t payload[8];
  const uint32_t n = encode_switch_channel(channel_, ByteView(payload));
  if (!n || !mcu_command(mt::kCmdSwitchChannelOp, ConstByteView(payload).first(n))) return XboxWirelessStatus::UsbError;
  if (!write_beacon() || !write32(mt::kBeaconTimeCfg, (uint32_t{mt::kBeaconIntervalTu} << 4) | mt::kBeaconTimeEnable) || !set_led(mt::kLedOn))
    return XboxWirelessStatus::UsbError;
  return XboxWirelessStatus::Ok;
}

XboxWirelessStatus XboxWirelessAdapter::start(IWifiFirmwareSource& source, const char** detail) {
  kernel::ScopedMutex lock(mutex_);
  deferred_ = false;
  return start_locked(source, detail);
}

XboxWirelessStatus XboxWirelessAdapter::start_locked(IWifiFirmwareSource& source, const char** detail) {
  const char* unused = nullptr;
  const char*& said = detail ? *detail : unused;
  said = nullptr;
  if (running_) return XboxWirelessStatus::Ok;
  if (gone()) return XboxWirelessStatus::UsbError;
  UsbDeviceDescriptor device;
  if (!host_.device_descriptor(port_, &device)) return XboxWirelessStatus::UsbError;
  const uint16_t vendor = device.vendor_id, product = device.product_id;   // (copies: the descriptor is packed)
  if (!is_xbox_wireless_adapter(vendor, product)) return XboxWirelessStatus::NotAdapter;
  scratch_.clear();
  if (!scratch_.put_zeros(kScratchBytes)) return XboxWirelessStatus::NoMemory;
  XboxWirelessStatus status = find_endpoints();
  if (status != XboxWirelessStatus::Ok) return status;
  if (!host_.set_configuration(port_, configuration_)) return XboxWirelessStatus::UsbError;

  uint32_t names = 0;
  const char* const* name = xbox_wireless_firmware_names(product, &names);
  said = names ? name[0] : nullptr;
  ByteBuffer file(allocator_);
  bool found = false;
  for (uint32_t i = 0; i < names && !found; ++i) {
    found = source.read(name[i], file);
    if (found) said = name[i];
  }
  if (!found) return XboxWirelessStatus::FirmwareMissing;
  status = load_firmware(file.view());
  if (status != XboxWirelessStatus::Ok) return status;
  status = start_radio();
  if (status != XboxWirelessStatus::Ok) return status;
  running_ = true;
  return XboxWirelessStatus::Ok;
}

void XboxWirelessAdapter::stop() {
  kernel::ScopedMutex lock(mutex_);
  if (!running_) return;
  running_ = false;
  if (gone()) return;
  (void)set_led(mt::kLedOff);
  (void)mcu_u32_pair(mt::kCmdPowerSavingOp, mt::kRadioOff, 0);
}

bool XboxWirelessAdapter::set_pairing(bool open) {
  kernel::ScopedMutex lock(mutex_);
  if (!running_ || gone()) return false;
  pairing_ = open;
  const bool beacon = write_beacon();
  return set_led(open ? mt::kLedBlink : mt::kLedOn) && beacon;
}

bool XboxWirelessAdapter::pairing() const {
  kernel::ScopedMutex lock(mutex_);
  return pairing_;
}

uint8_t XboxWirelessAdapter::channel() const {
  kernel::ScopedMutex lock(mutex_);
  return channel_;
}

MacAddress XboxWirelessAdapter::address() const {
  kernel::ScopedMutex lock(mutex_);
  return address_;
}

bool XboxWirelessAdapter::client_connected(uint32_t slot) const {
  kernel::ScopedMutex lock(mutex_);
  return slot < kMaxClients && running_ && clients_[slot].associated && clients_[slot].announced;
}

bool XboxWirelessAdapter::client_state(uint32_t slot, gamepad::State* state) const {
  kernel::ScopedMutex lock(mutex_);
  if (slot >= kMaxClients || !clients_[slot].associated) return false;
  *state = clients_[slot].state;
  return true;
}

uint32_t XboxWirelessAdapter::client_generation(uint32_t slot) const {
  kernel::ScopedMutex lock(mutex_);
  return slot < kMaxClients ? clients_[slot].generation : 0;
}

bool XboxWirelessAdapter::client_rumble(uint32_t slot, uint16_t low_frequency, uint16_t high_frequency) {
  kernel::ScopedMutex lock(mutex_);
  if (slot >= kMaxClients || !running_ || gone() || !clients_[slot].associated || !clients_[slot].announced) return false;
  uint8_t message[16];
  const uint32_t n = xbox::encode_gip_rumble(clients_[slot].gip_sequence++, low_frequency, high_frequency, ByteView(message));
  return n && send_gip(slot, ConstByteView(message).first(n));
}

int32_t XboxWirelessAdapter::slot_of(const MacAddress& address) const {
  for (uint32_t i = 0; i < kMaxClients; ++i)
    if (clients_[i].associated && clients_[i].address == address) return static_cast<int32_t>(i);
  return -1;
}

bool XboxWirelessAdapter::send_gip(uint32_t slot, ConstByteView message) {
  Client& c = clients_[slot];
  uint8_t frame[96];
  const uint32_t n = encode_qos_data(c.address, address_, c.frame_sequence++, message, ByteView(frame));
  return n && send_frame(static_cast<uint8_t>(slot + 1), ConstByteView(frame).first(n));
}

void XboxWirelessAdapter::drop_client(uint32_t slot) {
  if (slot >= kMaxClients || !clients_[slot].associated) return;
  (void)write_station(static_cast<uint8_t>(slot + 1), MacAddress{});   // (the entry goes either way; a failure only leaves a stale one)
  const uint32_t generation = clients_[slot].generation + 1;
  clients_[slot] = Client();
  clients_[slot].generation = generation;
}

void XboxWirelessAdapter::pump() {
  kernel::ScopedMutex lock(mutex_);
  if (!running_ && deferred_ && !gone()) try_deferred_start();
  if (!running_ || gone()) return;
  uint8_t buffer[1024];
  for (uint32_t e = 0; e < in_count_; ++e) {
    uint16_t length = 0;
    for (uint32_t budget = 0; budget < kReceiveBudget && host_.poll_interrupt_in(port_, in_endpoints_[e], ByteView(buffer), &length); ++budget) {
      if (length > sizeof(buffer)) length = sizeof(buffer);
      RxPacket packet;
      if (decode_rx(ConstByteView(buffer).first(length), &packet)) handle(packet);
      if (gone()) return;
    }
  }
}

void XboxWirelessAdapter::handle(const RxPacket& packet) {
  switch (packet.kind) {
    case RxKind::ButtonPress:                               // the adapter's own button: open pairing
      pairing_ = true;
      (void)write_beacon();
      (void)set_led(mt::kLedBlink);
      return;
    case RxKind::ClientLost:                                // out of range or switched off
      if (packet.wcid >= 1 && packet.wcid <= kMaxClients) drop_client(packet.wcid - 1u);
      return;
    case RxKind::Frame:
      break;
    default:
      return;
  }
  const Frame& f = packet.frame;
  if (f.type == kTypeManagement) {
    handle_management(packet);
    return;
  }
  if (f.type != kTypeData || f.subtype != kSubtypeQosData) return;
  // The radio's station match, checked against the sender (a frame of a station that left may still be in flight).
  const int32_t slot = slot_of(f.source);
  if (slot < 0 || (packet.wcid != 0xFF && packet.wcid != static_cast<uint32_t>(slot) + 1)) return;
  handle_gip(static_cast<uint32_t>(slot), f.body);
}

void XboxWirelessAdapter::handle_management(const RxPacket& packet) {
  const Frame& f = packet.frame;
  if (!(f.destination == address_) && !f.destination.is_broadcast()) return;    // (not for this adapter)
  uint8_t frame[96];
  switch (f.subtype) {
    case kSubtypeAssociationRequest: {
      int32_t slot = slot_of(f.source);
      if (slot >= 0) {
        drop_client(static_cast<uint32_t>(slot));           // (associating again: it starts over)
      } else {
        for (uint32_t i = 0; i < kMaxClients && slot < 0; ++i)
          if (!clients_[i].associated) slot = static_cast<int32_t>(i);
      }
      if (slot < 0) return;                                 // every slot taken: no answer, and the controller gives up
      const uint8_t wcid = static_cast<uint8_t>(slot + 1);
      if (!write_station(wcid, f.source)) return;
      Client& c = clients_[slot];
      c.associated = true;
      c.announced = false;
      c.address = f.source;
      c.state = gamepad::State();
      uint8_t body[16];
      const uint32_t b = encode_association_response_body(wcid, ByteView(body));
      const uint32_t n = encode_management(kSubtypeAssociationResponse, f.source, address_, c.frame_sequence++, ConstByteView(body).first(b), ByteView(frame));
      if (!b || !n || !send_frame(wcid, ConstByteView(frame).first(n))) drop_client(static_cast<uint32_t>(slot));
      return;
    }
    case xbox_table::kSubtypePair: {
      if (!pairing_) return;
      const uint32_t n = encode_management(xbox_table::kSubtypePair, f.source, address_, beacon_sequence_++, ConstByteView(xbox_table::kPairResponseBody), ByteView(frame));
      if (!n || !send_frame(0xFF, ConstByteView(frame).first(n))) return;
      pairing_ = false;                                     // one controller per press, as on a console
      (void)write_beacon();
      (void)set_led(mt::kLedOn);
      return;
    }
    case kSubtypeDisassociation:
    case kSubtypeDeauthentication: {
      const int32_t slot = slot_of(f.source);
      if (slot >= 0) drop_client(static_cast<uint32_t>(slot));
      return;
    }
    default:
      return;
  }
}

void XboxWirelessAdapter::handle_gip(uint32_t slot, ConstByteView message) {
  Client& c = clients_[slot];
  uint8_t command = 0;
  if (!message.try_get(0, &command)) return;
  uint8_t out[16];
  if (!c.announced) {
    // Its announcement (or, from a controller whose announcement was lost, anything): tell it to power on and light the guide button.
    uint32_t n = xbox::encode_gip_power_on(c.gip_sequence++, ByteView(out));
    if (!n || !send_gip(slot, ConstByteView(out).first(n))) return;
    n = xbox::encode_gip_led(c.gip_sequence++, 0x14, ByteView(out));
    if (n) (void)send_gip(slot, ConstByteView(out).first(n));
    c.announced = true;
    ++c.generation;
    if (command == kGipAnnounce) return;
  }
  if (command == xbox::kGipPower && message.size() >= 5 && message[4] == kGipPowerOff) {   // (the controller says it switches off)
    drop_client(slot);
    return;
  }
  const gamepad::State before = c.state;
  const bool battery = decode_gip_battery(message, &c.state);
  const xbox::GipResult r = xbox::decode_gip(message, &c.state);
  if (r.event == xbox::GipEvent::Input) {                 // (decode_gip says "wired"; the battery comes in the status message)
    c.state.battery_kind = before.battery_kind;
    c.state.battery_percent = before.battery_percent;
    c.state.charging = before.charging;
  }
  if (r.needs_ack) {
    const uint32_t n = xbox::encode_gip_ack(r, ByteView(out));
    if (n) (void)send_gip(slot, ConstByteView(out).first(n));
  }
  if (battery || r.event == xbox::GipEvent::Input || r.event == xbox::GipEvent::Guide) ++c.generation;
}

// --- the pads ---------------------------------------------------------------------------------------------------------------------------------

XboxWirelessPad::XboxWirelessPad(SharedPointer<XboxWirelessAdapter> adapter, uint32_t slot, uint16_t vendor, uint16_t product)
    : adapter_(util::move(adapter)), slot_(slot) {
  info_.protocol = gamepad::Protocol::XboxOne;
  info_.vendor_id = vendor;
  info_.product_id = product;
  info_.capabilities = gamepad::protocol_capabilities(gamepad::Protocol::XboxOne) | gamepad::kCapBattery;
  const char name[] = "Xbox Wireless Controller";
  static_assert(sizeof(name) <= sizeof(info_.name));
  ArrayView<char>(info_.name).first(sizeof(name)).copy_from(ArrayView<const char>(name));
  seen_generation_ = adapter_->client_generation(slot_);
}

void XboxWirelessPad::shutdown() {
  if (!connected()) return;
  (void)adapter_->client_rumble(slot_, 0, 0);
}

bool XboxWirelessPad::poll() {
  if (!adapter_->present()) return false;
  adapter_->pump();
  const uint32_t generation = adapter_->client_generation(slot_);
  if (generation == seen_generation_) return false;
  seen_generation_ = generation;
  gamepad::State s;
  if (!adapter_->client_state(slot_, &s)) s = gamepad::State();
  state_ = s;
  return true;
}

bool XboxWirelessPad::set_output(const gamepad::Output& output) {
  if (!connected()) return false;
  output_ = output;
  return adapter_->client_rumble(slot_, output.low_frequency, output.high_frequency);   // (the guide light shows no player number)
}

// --- opening ----------------------------------------------------------------------------------------------------------------------------------

namespace {

uint32_t open_adapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, const XboxWirelessEnvironment& environment,
                      ArrayView<UniquePointer<IGameController>> out, XboxWirelessStatus* status, SharedPointer<XboxWirelessAdapter>* adapter_out, bool defer) {
  XboxWirelessStatus unused = XboxWirelessStatus::Ok;
  XboxWirelessStatus& result = status ? *status : unused;
  UsbDeviceDescriptor device;
  if (!host.device_descriptor(port, &device)) {
    result = XboxWirelessStatus::NotAdapter;
    return 0;
  }
  const uint16_t vendor = device.vendor_id, product = device.product_id;
  if (!is_xbox_wireless_adapter(vendor, product)) {
    result = XboxWirelessStatus::NotAdapter;
    return 0;
  }
  const char* name = xbox_wireless_adapter_name(product);
  if (!environment.firmware && !defer) {
    result = XboxWirelessStatus::NoFirmwareSource;
    say(environment, allocator, name, xbox_wireless_status_text(result), nullptr);
    return 0;
  }
  SharedPointer<XboxWirelessAdapter> adapter = SharedPointer<XboxWirelessAdapter>::make(allocator, host, port, allocator);
  if (!adapter) {
    result = XboxWirelessStatus::NoMemory;
    say(environment, allocator, name, xbox_wireless_status_text(result), nullptr);
    return 0;
  }
  const char* detail = nullptr;
  adapter->set_channel_dwell(environment.channel_dwell_us);
  if (!environment.firmware) {
    adapter->start_when_firmware_is_there(name);         // (no file system yet: it starts from the pads' polls once there is one)
    result = XboxWirelessStatus::NoFirmwareSource;
  } else {
    result = adapter->start(*environment.firmware, &detail);
    if (result != XboxWirelessStatus::Ok) {
      say(environment, allocator, name, xbox_wireless_status_text(result), detail);
      return 0;
    }
  }
  uint32_t made = 0;
  for (UniquePointer<IGameController>& slot : out) {
    if (made >= XboxWirelessAdapter::kMaxClients) break;
    UniquePointer<XboxWirelessPad> pad = UniquePointer<XboxWirelessPad>::make(allocator, adapter, made, vendor, product);
    if (!pad) break;
    slot = UniquePointer<IGameController>(util::move(pad));
    ++made;
  }
  if (result == XboxWirelessStatus::Ok) say(environment, allocator, name, "ready, firmware", detail);
  if (adapter_out) *adapter_out = adapter;
  return made;
}

}  // namespace

uint32_t open_xbox_wireless_adapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, const XboxWirelessEnvironment& environment,
                                    ArrayView<UniquePointer<IGameController>> out, XboxWirelessStatus* status, SharedPointer<XboxWirelessAdapter>* adapter) {
  return open_adapter(host, port, allocator, environment, out, status, adapter, false);
}

uint32_t open_xbox_wireless_adapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, ArrayView<UniquePointer<IGameController>> out,
                                    XboxWirelessStatus* status, SharedPointer<XboxWirelessAdapter>* adapter) {
  const XboxWirelessEnvironment* environment = __atomic_load_n(&g_environment, __ATOMIC_ACQUIRE);
  const XboxWirelessEnvironment none;
  return open_adapter(host, port, allocator, environment ? *environment : none, out, status, adapter, true);
}

}  // namespace hardware

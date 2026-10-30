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

#include "Hardware/Bus/Thunderbolt/ThunderboltIcm.hpp"
#include "Crypto/Crypto.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace tb_icm {

namespace {
uint32_t header(ByteWriter& w, uint8_t code) {
  w.u8(code);
  w.u8(0);        // flags
  w.u8(0);        // packet id
  w.u8(0);        // total packets
  return 0;
}
TbSecurityLevel security_of(uint32_t value) { return value <= 5 ? static_cast<TbSecurityLevel>(value) : TbSecurityLevel::Unknown; }
}  // namespace

uint32_t build_get_topology(ByteView out) {
  ByteWriter w(out);
  header(w, kGetTopology);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool parse_topology_packet(ConstByteView packet, TbTopologySwitch* out) {
  if (packet.size() < kTopologyBytes) return false;
  ByteReader r(packet);
  const uint8_t code = r.u8();
  r.skip(3);
  const uint32_t route_lo = r.le32(), route_hi = r.le32();
  const uint8_t first_data = r.u8();
  r.skip(1);                                  // second_data
  r.skip(1);                                  // the DROM's I2C address index
  const uint8_t switch_index = r.u8();
  r.skip(8);
  TbTopologySwitch t;
  for (uint32_t i = 0; i < 16; ++i) t.ports[i] = r.le32();
  if (!r.ok() || code != kGetTopology) return false;
  t.used = (first_data & 1) != 0;
  t.upstream_port = static_cast<uint8_t>(first_data >> 1);
  t.switch_index = switch_index;
  t.route = (static_cast<uint64_t>(route_hi & 0x3FFFFF) << 32) | route_lo;
  *out = t;
  return true;
}

uint32_t build_driver_ready(ByteView out) {
  ByteWriter w(out);
  header(w, kDriverReady);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool parse_driver_ready(ConstByteView response, TbDriverReady* out) {
  if (response.size() < kDriverReadyResponseBytes) return false;
  ByteReader r(response);
  const uint8_t code = r.u8(), flags = r.u8();
  r.skip(2);
  out->rom_version = r.u8();
  out->ram_version = r.u8();
  const uint16_t info = r.le16();
  if (!r.ok() || code != kDriverReady) return false;
  out->security = security_of(info & 0xF);
  // (bit 13: the firmware supports the preboot ACL; bits 7 to 11 its number of entries)
  out->boot_acl_entries = (info & (1u << 13)) ? (info >> 7) & 0x1F : 0;
  out->rtd3 = (flags & kFlagRtd3) != 0;
  return true;
}

uint32_t build_approve_device(const TbDevice& device, ByteView out) {
  ByteWriter w(out);
  header(w, kApproveDevice);
  w.bytes(ConstByteView(device.uuid, sizeof device.uuid));
  w.u8(device.connection_key);
  w.u8(device.connection_id);
  w.le16(0);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_get_route(uint8_t link, uint8_t depth, ByteView out) {
  ByteWriter w(out);
  header(w, kGetRoute);
  w.le16(0);
  w.le16(static_cast<uint16_t>((static_cast<uint32_t>(depth) << kDepthShift) | link));
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

namespace {
uint32_t keyed(ByteWriter& w, uint8_t code, const TbDevice& device, ConstByteView secret) {
  header(w, code);
  w.bytes(ConstByteView(device.uuid, sizeof device.uuid));
  w.u8(device.connection_key);
  w.u8(device.connection_id);
  w.le16(0);
  w.bytes(secret);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}
}  // namespace

uint32_t build_add_device_key(const TbDevice& device, ConstByteView key, ByteView out) {
  if (key.size() != kTbKeyBytes) return 0;
  ByteWriter w(out);
  return keyed(w, kAddDeviceKey, device, key);
}

uint32_t build_challenge_device(const TbDevice& device, ConstByteView challenge, ByteView out) {
  if (challenge.size() != kTbKeyBytes) return 0;
  ByteWriter w(out);
  return keyed(w, kChallengeDevice, device, challenge);
}

bool parse_challenge_response(ConstByteView response, ByteView out) {
  if (response.size() < kChallengeResponseBytes || out.size() != kTbKeyBytes) return false;
  out.copy_from(response.subview(kKeyedBytes, kTbKeyBytes));
  return true;
}

uint8_t response_flags(ConstByteView response) {
  uint8_t flags = 0;
  return response.try_get(1, &flags) ? flags : 0;
}

bool parse_get_route(ConstByteView response, uint64_t* route) {
  if (response.size() < kGetRouteResponseBytes) return false;
  ByteReader r(response);
  r.skip(8);
  const uint32_t hi = r.le32(), lo = r.le32();
  if (!r.ok()) return false;
  *route = (static_cast<uint64_t>(hi & 0x3FFFFF) << 32) | lo;
  return true;
}

bool parse_device_connected(ConstByteView event, TbDevice* out, bool* rejected) {
  if (event.size() < kDeviceConnectedMinBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8(), flags = r.u8();
  r.skip(2);
  TbDevice d;
  const bool got = r.read_into(ByteView(d.uuid, sizeof d.uuid));
  d.connection_key = r.u8();
  d.connection_id = r.u8();
  const uint16_t link_info = r.le16();
  if (!r.ok() || !got || code != kEventDeviceConnected) return false;
  d.link = link_info & kLinkMask;
  d.depth = static_cast<uint8_t>((link_info & kDepthMask) >> kDepthShift);
  d.authorized = (link_info & kApproved) != 0;
  d.boot = (link_info & kBoot) != 0;
  d.dual_lane = (flags & kFlagDualLane) != 0;
  d.gen3 = (flags & kFlagSpeedGen3) != 0;
  d.security = security_of((flags & kFlagSecurityMask) >> kFlagSecurityShift);
  *rejected = (link_info & kRejected) != 0;
  *out = d;
  return true;
}

bool parse_device_disconnected(ConstByteView event, uint8_t* link, uint8_t* depth) {
  if (event.size() < kDeviceDisconnectedBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8();
  r.skip(5);
  const uint16_t link_info = r.le16();
  if (!r.ok() || code != kEventDeviceDisconnected) return false;
  *link = link_info & kLinkMask;
  *depth = static_cast<uint8_t>((link_info & kDepthMask) >> kDepthShift);
  return true;
}

uint32_t build_approve_xdomain(const TbXDomain& xd, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, ByteView out) {
  ByteWriter w(out);
  header(w, kApproveXdomain);
  w.le16(0);
  w.le16(static_cast<uint16_t>((static_cast<uint32_t>(xd.depth) << kDepthShift) | xd.link));
  w.bytes(ConstByteView(xd.remote_uuid, 16));
  w.le16(transmit_path);
  w.le16(transmit_ring);
  w.le16(receive_path);
  w.le16(receive_ring);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_approve_xdomain_tr(const TbXDomain& xd, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, ByteView out) {
  ByteWriter w(out);
  header(w, kApproveXdomain);
  w.le32(static_cast<uint32_t>(xd.route >> 32));
  w.le32(static_cast<uint32_t>(xd.route));
  w.bytes(ConstByteView(xd.remote_uuid, 16));
  w.le16(transmit_path);
  w.le16(transmit_ring);
  w.le16(receive_path);
  w.le16(receive_ring);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_disconnect_xdomain_tr(const TbXDomain& xd, uint8_t stage, ByteView out) {
  ByteWriter w(out);
  header(w, kDisconnectXdomain);
  w.u8(stage);
  w.zeros(3);
  w.le32(static_cast<uint32_t>(xd.route >> 32));
  w.le32(static_cast<uint32_t>(xd.route));
  w.bytes(ConstByteView(xd.remote_uuid, 16));
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

// ---- XDomain events

bool parse_xdomain_connected(ConstByteView event, TbXDomain* out) {
  if (event.size() < kXdomainConnectedBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8(), flags = r.u8();
  r.skip(2);
  r.skip(2);
  const uint16_t link_info = r.le16();
  TbXDomain x;
  x.dual_lane = (flags & kFlagDualLane) != 0;
  x.gen3 = (flags & kFlagSpeedGen3) != 0;
  const bool a = r.read_into(ByteView(x.remote_uuid, 16)), b = r.read_into(ByteView(x.local_uuid, 16));
  const uint32_t local_hi = r.le32(), local_lo = r.le32();
  r.skip(8);                                                  // the remote's route to us: not needed
  if (!r.ok() || !a || !b || code != kEventXdomainConnected) return false;
  x.link = link_info & kLinkMask;
  x.depth = static_cast<uint8_t>((link_info & kDepthMask) >> kDepthShift);
  x.route = (static_cast<uint64_t>(local_hi & 0x3FFFFF) << 32) | local_lo;
  *out = x;
  return true;
}

bool parse_xdomain_disconnected(ConstByteView event, uint8_t remote_uuid[16], uint8_t* link, uint8_t* depth) {
  if (event.size() < kXdomainDisconnectedBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8();
  r.skip(5);
  const uint16_t link_info = r.le16();
  const bool got = r.read_into(ByteView(remote_uuid, 16));
  if (!r.ok() || !got || code != kEventXdomainDisconnected) return false;
  *link = link_info & kLinkMask;
  *depth = static_cast<uint8_t>((link_info & kDepthMask) >> kDepthShift);
  return true;
}

bool parse_xdomain_disconnected_tr(ConstByteView event, uint64_t* route, uint8_t remote_uuid[16]) {
  if (event.size() < kTrXdomainDisconnectedBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8();
  r.skip(3);
  const uint32_t hi = r.le32(), lo = r.le32();
  const bool got = r.read_into(ByteView(remote_uuid, 16));
  if (!r.ok() || !got || code != kEventXdomainDisconnected) return false;
  *route = (static_cast<uint64_t>(hi & 0x3FFFFF) << 32) | lo;
  return true;
}

// ---- Titan Ridge

namespace {
uint32_t keyed_tr(ByteWriter& w, uint8_t code, const TbDevice& device, ConstByteView secret) {
  header(w, code);
  w.bytes(ConstByteView(device.uuid, sizeof device.uuid));
  w.le32(static_cast<uint32_t>(device.route >> 32));
  w.le32(static_cast<uint32_t>(device.route));
  w.u8(device.connection_id);
  w.zeros(3);
  w.bytes(secret);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}
}  // namespace

uint32_t build_approve_device_tr(const TbDevice& device, ByteView out) {
  ByteWriter w(out);
  return keyed_tr(w, kApproveDevice, device, ConstByteView());
}

uint32_t build_add_device_key_tr(const TbDevice& device, ConstByteView key, ByteView out) {
  if (key.size() != kTbKeyBytes) return 0;
  ByteWriter w(out);
  return keyed_tr(w, kAddDeviceKey, device, key);
}

uint32_t build_challenge_device_tr(const TbDevice& device, ConstByteView challenge, ByteView out) {
  if (challenge.size() != kTbKeyBytes) return 0;
  ByteWriter w(out);
  return keyed_tr(w, kChallengeDevice, device, challenge);
}

bool parse_challenge_response_tr(ConstByteView response, ByteView out) {
  if (response.size() < kTrChallengeResponseBytes || out.size() != kTbKeyBytes) return false;
  out.copy_from(response.subview(kTrKeyedBytes, kTbKeyBytes));
  return true;
}

bool parse_driver_ready_tr(ConstByteView response, TbDriverReady* out) {
  if (response.size() < kTrDriverReadyResponseBytes) return false;
  ByteReader r(response);
  const uint8_t code = r.u8(), flags = r.u8();
  r.skip(2);
  r.skip(2);                                  // reserved
  const uint16_t info = r.le16();
  if (!r.ok() || code != kDriverReady) return false;
  out->rom_version = 0;
  out->ram_version = 0;
  out->security = security_of(info & 0x7);
  out->boot_acl_entries = (info >> 7) & 0x3F;
  out->rtd3 = (flags & kFlagRtd3) != 0;
  return true;
}

bool parse_device_connected_tr(ConstByteView event, TbDevice* out, bool* rejected) {
  if (event.size() < kTrDeviceConnectedMinBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8(), flags = r.u8(), packet_id = r.u8();
  r.skip(1);
  TbDevice d;
  const bool got = r.read_into(ByteView(d.uuid, sizeof d.uuid));
  const uint32_t route_hi = r.le32(), route_lo = r.le32();
  d.connection_id = r.u8();
  r.skip(1);
  const uint16_t link_info = r.le16();
  if (!r.ok() || !got || code != kEventDeviceConnected || packet_id != 0) return false;
  d.route = (static_cast<uint64_t>(route_hi & 0x3FFFFF) << 32) | route_lo;
  d.depth = static_cast<uint8_t>(route_length(d.route));
  d.link = d.route == 0 ? 0 : static_cast<uint8_t>(d.route & 0x3F);
  d.authorized = (link_info & kApproved) != 0;
  d.boot = (link_info & kBoot) != 0;
  d.dual_lane = (flags & kFlagDualLane) != 0;
  d.gen3 = (flags & kFlagSpeedGen3) != 0;
  d.security = security_of((flags & kFlagSecurityMask) >> kFlagSecurityShift);
  *rejected = (link_info & kRejected) != 0;
  *out = d;
  return true;
}

bool parse_device_disconnected_tr(ConstByteView event, uint64_t* route) {
  if (event.size() < kTrDeviceDisconnectedBytes) return false;
  ByteReader r(event);
  const uint8_t code = r.u8();
  r.skip(3);
  const uint32_t hi = r.le32(), lo = r.le32();
  if (!r.ok() || code != kEventDeviceDisconnected) return false;
  *route = (static_cast<uint64_t>(hi & 0x3FFFFF) << 32) | lo;
  return true;
}

uint32_t route_length(uint64_t route) {
  uint32_t bits = 0;
  while (route != 0) { ++bits; route >>= 1; }
  return (bits + 7) / 8;
}

}  // namespace tb_icm

namespace {
// Both links of a dual-link port belong to the same physical port (tb_phy_port_from_link).
uint8_t physical_port(uint8_t link) { return link == 0 ? 0 : static_cast<uint8_t>((link - 1) / 2); }
}  // namespace

const char* ThunderboltIcm::outcome_name(Outcome outcome) {
  switch (outcome) {
    case Outcome::NotStarted: return "not started";
    case Outcome::Suspended: return "suspended";
    case Outcome::Started: return "started";
    case Outcome::NotPresent: return "the controller does not answer";
    case Outcome::FirmwareNotRunning: return "the connection manager firmware is not running (the BIOS did not start it)";
    case Outcome::FirmwareStartFailed: return "the connection manager firmware did not come up after it was restarted";
    case Outcome::ControlFailed: return "the control channel could not be started";
    case Outcome::AuthTimeout: return "the controller's firmware image was not authenticated";
    case Outcome::SafeMode: return "the controller is in safe mode (its NVM needs an update)";
    case Outcome::WrongMode: return "the controller's firmware is in a mode that does not manage it";
    case Outcome::DriverReadyFailed: return "the firmware did not answer DRIVER_READY";
    case Outcome::RootSwitchFailed: return "the root router did not become readable";
  }
  return "";
}

bool ThunderboltIcm::request(ConstByteView request, ByteView response, uint32_t timeout_us, uint32_t retries) {
  for (uint32_t attempt = 0; attempt <= retries; ++attempt) {
    const TbControl::Result result = control_.icm_request(request, response, timeout_us, this);
    if (result == TbControl::Result::Ok) return true;
    if (result != TbControl::Result::Timeout) return false;
    delay_us(50);
  }
  return false;
}

ThunderboltIcm::Outcome ThunderboltIcm::start(uint32_t timeout_us) {
  stop();
  return bring_up(timeout_us, false);
}

ThunderboltIcm::Outcome ThunderboltIcm::bring_up(uint32_t timeout_us, bool resuming) {
  devices_.clear();
  xdomains_.clear();
  ready_ = TbDriverReady();
  rtd3_veto_ = false;
  if (!nhi_.present()) return outcome_ = Outcome::NotPresent;
  nhi_.disable_interrupts();
  port_reset_failures_ = 0;
  if (!nhi_.icm_running()) {
    if (!options_.start_firmware || !options_.cio) return outcome_ = Outcome::FirmwareNotRunning;
    if (!start_firmware()) return outcome_ = Outcome::FirmwareStartFailed;
  }
  if (!control_.start()) return outcome_ = Outcome::ControlFailed;
  if (!nhi_.wait_firmware_authenticated(timeout_us < 3000000 ? timeout_us : 3000000)) {
    control_.stop();
    return outcome_ = Outcome::AuthTimeout;
  }
  switch (nhi_.firmware_mode()) {
    case TbFirmwareMode::Safe:
      return outcome_ = Outcome::SafeMode;
    case TbFirmwareMode::ConnectionManager:
      // like the Linux driver: the firmware reports every device; approving one is still the caller's decision
      nhi_.mailbox(tb_mailbox::kAllowAllDevices, 0, 500000);
      break;
    default:
      control_.stop();
      return outcome_ = Outcome::WrongMode;
  }
  // links that are already up are bounced, so that the firmware finds what is connected again (failures are not fatal)
  if (options_.cio && options_.reset_ports && !resuming)
    for (uint32_t physical = 0; physical < 2; ++physical)
      if (!reset_physical_port(physical)) ++port_reset_failures_;
  uint8_t request_bytes[tb_icm::kDriverReadyBytes], response[tb_icm::kTrDriverReadyResponseBytes];
  const uint32_t response_bytes = titan() ? tb_icm::kTrDriverReadyResponseBytes : tb_icm::kDriverReadyResponseBytes;
  const uint32_t n = tb_icm::build_driver_ready(ByteView(request_bytes, sizeof request_bytes));
  // (Titan Ridge answers DRIVER_READY quickly or not at all: ten tries of a quarter second)
  const bool answered = n != 0 && (titan() ? request(ConstByteView(request_bytes, n), ByteView(response, response_bytes), 250000, 10) : request(ConstByteView(request_bytes, n), ByteView(response, response_bytes), timeout_us));
  if (!answered || !(titan() ? tb_icm::parse_driver_ready_tr(ConstByteView(response, response_bytes), &ready_) : tb_icm::parse_driver_ready(ConstByteView(response, response_bytes), &ready_))) {
    control_.stop();
    return outcome_ = Outcome::DriverReadyFailed;
  }
  // wait until the root router's configuration space can be read
  bool readable = false;
  for (uint32_t i = 0; i < 50 && !readable; ++i) {
    uint32_t word = 0;
    readable = control_.read_config(0, 0, TbConfigSpace::Switch, 0, ArrayView<uint32_t>(&word, 1), 100000, nullptr, this) == TbControl::Result::Ok;
    if (!readable) delay_us(50000);
  }
  if (!readable) {
    control_.stop();
    return outcome_ = Outcome::RootSwitchFailed;
  }
  outcome_ = Outcome::Started;
  if (interrupts_) nhi_.set_control_interrupts(true, msix_vector_);          // (start-up masked them)
  return outcome_;
}

// icm_firmware_start: the ARC is told to wait for a CIO reset, restarted, and the reset is triggered through the PCIe-to-CIO registers; the firmware then authenticates its image.
bool ThunderboltIcm::start_firmware() {
  ITbRegisters& regs = nhi_.registers();
  regs.write32(tb_reg::kFirmwareStatus, regs.read32(tb_reg::kFirmwareStatus) | tb_reg::kFwStsCioResetRequest);
  regs.write32(tb_reg::kFirmwareStatus, regs.read32(tb_reg::kFirmwareStatus) | tb_reg::kFwStsIcmEnableInvert | tb_reg::kFwStsIcmEnableCpu);
  if (!options_.cio->write(TbConfigSpace::Switch, 0, titan() ? tb_cio::kTitanSwitchResetIndex : tb_cio::kSwitchResetIndex, titan() ? tb_cio::kTitanSwitchResetBit : tb_cio::kSwitchResetBit)) return false;
  return nhi_.wait_firmware_authenticated(3000000);
}

// icm_reset_phy_port: the two lanes of a physical port (ports 1 and 2, or 3 and 4) are disabled and enabled again if both are up.
bool ThunderboltIcm::reset_physical_port(uint32_t physical_port) {
  TbPcie2Cio& cio = *options_.cio;
  const uint32_t port0 = physical_port ? 3 : 1, port1 = port0 + 1;
  uint32_t v0 = 0, v1 = 0;
  if (!cio.read(TbConfigSpace::Port, port0, tb_cio::kPhyPortCs1, &v0) || !cio.read(TbConfigSpace::Port, port1, tb_cio::kPhyPortCs1, &v1)) return false;
  const uint32_t state0 = (v0 & tb_cio::kPhyLinkStateMask) >> tb_cio::kPhyLinkStateShift, state1 = (v1 & tb_cio::kPhyLinkStateMask) >> tb_cio::kPhyLinkStateShift;
  if (state0 != tb_cio::kPortStateUp || state1 != tb_cio::kPortStateUp) return true;           // nothing connected
  if (!cio.write(TbConfigSpace::Port, port0, tb_cio::kPhyPortCs1, v0 | tb_cio::kPhyLinkDisable) || !cio.write(TbConfigSpace::Port, port1, tb_cio::kPhyPortCs1, v1 | tb_cio::kPhyLinkDisable)) return false;
  delay_us(100);
  if (!cio.read(TbConfigSpace::Port, port0, tb_cio::kPhyPortCs1, &v0) || !cio.read(TbConfigSpace::Port, port1, tb_cio::kPhyPortCs1, &v1)) return false;
  return cio.write(TbConfigSpace::Port, port0, tb_cio::kPhyPortCs1, v0 & ~tb_cio::kPhyLinkDisable) && cio.write(TbConfigSpace::Port, port1, tb_cio::kPhyPortCs1, v1 & ~tb_cio::kPhyLinkDisable);
}

bool ThunderboltIcm::use_interrupts(bool on, int msix_vector) {
  if (outcome_ != Outcome::Started || msix_vector > 15) return false;          // (the vector allocation has 4 bits a source)
  interrupts_ = on;
  msix_vector_ = on ? msix_vector : -1;
  nhi_.set_control_interrupts(on, msix_vector);
  return true;
}

bool ThunderboltIcm::runtime_suspend() {
  if (!runtime_allowed()) return false;
  return suspend_controller(false);
}

bool ThunderboltIcm::suspend() { return outcome_ == Outcome::Started && suspend_controller(true); }

bool ThunderboltIcm::suspend_controller(bool save_devices) {
  xdomains_.clear();
  if (save_devices) nhi_.mailbox(tb_mailbox::kSaveDevices, 0, 500000);
  nhi_.mailbox(tb_mailbox::kDriverUnloads, 0, 500000);
  control_.stop();
  devices_.clear();
  outcome_ = Outcome::Suspended;
  return true;
}

ThunderboltIcm::Outcome ThunderboltIcm::resume(uint32_t timeout_us) {
  if (outcome_ != Outcome::Suspended) return outcome_;
  return bring_up(timeout_us, true);
}

void ThunderboltIcm::stop() {
  if (outcome_ == Outcome::Started || control_.running()) {
    nhi_.mailbox(tb_mailbox::kDriverUnloads, 0, 500000);
    control_.stop();
  }
  devices_.clear();
  xdomains_.clear();
  interrupts_ = false;
  msix_vector_ = -1;
  outcome_ = Outcome::NotStarted;
}

void ThunderboltIcm::poll() {
  if (outcome_ != Outcome::Started && outcome_ != Outcome::SafeMode) return;
  control_.poll(this);
  if (outcome_ == Outcome::Started) service_xdomains();
}

void ThunderboltIcm::remove_downstream(uint8_t link, uint8_t depth) {
  // the device and everything behind it on the same physical port
  for (uint64_t i = devices_.count(); i-- > 0;)
    if (physical_port(devices_[i].link) == physical_port(link) && devices_[i].depth >= depth) devices_.remove_at(i);
}

namespace {
bool same_uuid(const TbDevice& a, const TbDevice& b) {
  bool same = true;
  for (uint32_t k = 0; k < sizeof a.uuid; ++k) same = same && a.uuid[k] == b.uuid[k];
  return same;
}
// Titan Ridge: `child` is `parent` or hangs below it (its route starts with the parent's hops).
bool route_below(uint64_t child, uint64_t parent) {
  const uint32_t length = tb_icm::route_length(parent);
  if (length >= 8) return child == parent;
  const uint64_t mask = length == 0 ? 0 : (uint64_t{1} << (8 * length)) - 1;
  return (child & mask) == parent && tb_icm::route_length(child) >= length;
}
}  // namespace

bool ThunderboltIcm::same_place(const TbDevice& a, const TbDevice& b) const {
  if (titan()) return a.route == b.route;
  return physical_port(a.link) == physical_port(b.link) && a.depth == b.depth;
}

bool ThunderboltIcm::on_event(TbPacketType type, ConstByteView body) {
  if (type == TbPacketType::XdomainRequest) {
    answer_xdomain_request(body);
    return true;
  }
  if (type == TbPacketType::XdomainResponse) return ip_handler_ && ip_handler_->handle_ip_packet(body);
  if (type != TbPacketType::IcmEvent) return false;
  uint8_t code = 0;
  if (!body.try_get(0, &code)) return false;
  if (code == tb_icm::kEventRtd3Veto) {
    // (the veto reason follows the header; zero lifts the veto)
    ByteReader r(body);
    r.skip(4);
    const uint32_t reason = r.le32();
    if (r.ok()) rtd3_veto_ = reason != 0;
    return true;
  }
  if (code == tb_icm::kEventXdomainConnected) {
    TbXDomain x;
    if (!tb_icm::parse_xdomain_connected(body, &x)) return true;
    // the same host again, or another one at that place
    for (uint64_t i = xdomains_.count(); i-- > 0;) {
      bool same = true;
      for (uint32_t k = 0; k < 16; ++k) same = same && xdomains_[i].remote_uuid[k] == x.remote_uuid[k];
      if (same) {
        // (still there: keep what was discovered unless it moved)
        if (xdomains_[i].route == x.route) return true;
        xdomains_.remove_at(i);
      } else if (titan() ? xdomains_[i].route == x.route : (physical_port(xdomains_[i].link) == physical_port(x.link) && xdomains_[i].depth == x.depth)) {
        xdomains_.remove_at(i);
      }
    }
    if (xdomains_.count() < kMaxDevices) xdomains_.push_back(x);
    return true;
  }
  if (code == tb_icm::kEventXdomainDisconnected) {
    uint8_t uuid[16], link = 0, depth = 0;
    uint64_t route = 0;
    const bool ok = titan() ? tb_icm::parse_xdomain_disconnected_tr(body, &route, uuid) : tb_icm::parse_xdomain_disconnected(body, uuid, &link, &depth);
    if (!ok) return true;
    for (uint64_t i = xdomains_.count(); i-- > 0;) {
      bool same = true;
      for (uint32_t k = 0; k < 16; ++k) same = same && xdomains_[i].remote_uuid[k] == uuid[k];
      if (same) xdomains_.remove_at(i);
    }
    return true;
  }
  if (code == tb_icm::kEventDeviceConnected) {
    TbDevice device;
    bool rejected = false;
    if (!(titan() ? tb_icm::parse_device_connected_tr(body, &device, &rejected) : tb_icm::parse_device_connected(body, &device, &rejected)) || rejected) return true;
    // the same device again (the firmware repeats the ones that are still there after a resume), or a stale entry at that place
    for (uint64_t i = devices_.count(); i-- > 0;)
      if (same_uuid(devices_[i], device) || same_place(devices_[i], device)) devices_.remove_at(i);
    if (devices_.count() < kMaxDevices) devices_.push_back(device);
  } else if (code == tb_icm::kEventDeviceDisconnected) {
    if (titan()) {
      uint64_t route = 0;
      if (!tb_icm::parse_device_disconnected_tr(body, &route) || route == 0) return true;          // (the host router never disconnects)
      for (uint64_t i = devices_.count(); i-- > 0;)
        if (route_below(devices_[i].route, route)) devices_.remove_at(i);
    } else {
      uint8_t link = 0, depth = 0;
      if (tb_icm::parse_device_disconnected(body, &link, &depth)) remove_downstream(link, depth);
    }
  }
  return true;
}

// A device hangs from the one a step nearer on the same physical port: that one has to be approved first.
bool ThunderboltIcm::parent_authorized(const TbDevice& device) const {
  if (device.depth <= 1) return true;
  if (titan()) {
    const uint64_t parent = device.route & ((uint64_t{1} << (8 * (device.depth - 1))) - 1);
    for (uint64_t i = 0; i < devices_.count(); ++i)
      if (devices_[i].route == parent) return devices_[i].authorized;
    return false;
  }
  for (uint64_t i = 0; i < devices_.count(); ++i)
    if (physical_port(devices_[i].link) == physical_port(device.link) && devices_[i].depth == device.depth - 1) return devices_[i].authorized;
  return false;
}

bool ThunderboltIcm::approve(uint64_t index, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (outcome_ != Outcome::Started) {
    *error = "the controller is not running";
    return false;
  }
  if (index >= devices_.count()) {
    *error = "no such device";
    return false;
  }
  if (ready_.security != TbSecurityLevel::None && ready_.security != TbSecurityLevel::User) {
    *error = ready_.security == TbSecurityLevel::Secure ? "this security level needs a key: use approve_secure" : "this security level does not allow a PCIe tunnel";
    return false;
  }
  const TbDevice device = devices_[index];
  if (!parent_authorized(device)) {
    *error = "the device it hangs from is not approved";
    return false;
  }
  uint8_t request_bytes[tb_icm::kTrApproveBytes], response[tb_icm::kTrApproveBytes];
  const uint32_t n = build_approve(device, ByteView(request_bytes, sizeof request_bytes));
  if (n == 0 || !request_flags(ConstByteView(request_bytes, n), ByteView(response, approve_bytes()), kApproveTimeoutUs, error, "the PCIe tunnel could not be created")) return false;
  mark_authorized(device);
  return true;
}

// A request whose answer has the error flag set is a refusal.
bool ThunderboltIcm::request_flags(ConstByteView request_bytes, ByteView response, uint32_t timeout_us, const char** error, const char* refused) {
  if (!request(request_bytes, response, timeout_us)) {
    *error = "the firmware did not answer";
    return false;
  }
  if (tb_icm::response_flags(ConstByteView(response.data(), response.size())) & tb_icm::kFlagError) {
    *error = refused;
    return false;
  }
  return true;
}

void ThunderboltIcm::mark_authorized(const TbDevice& device) {
  // (the list may have changed while waiting: find the device again)
  for (uint64_t i = 0; i < devices_.count(); ++i) {
    bool same = true;
    for (uint32_t k = 0; k < sizeof device.uuid; ++k) same = same && devices_[i].uuid[k] == device.uuid[k];
    if (same) devices_[i].authorized = true;
  }
}

bool ThunderboltIcm::approve_secure(uint64_t index, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (outcome_ != Outcome::Started) {
    *error = "the controller is not running";
    return false;
  }
  if (ready_.security != TbSecurityLevel::Secure) {
    *error = "keys are only used at the secure security level";
    return false;
  }
  if (index >= devices_.count()) {
    *error = "no such device";
    return false;
  }
  if (!random_ || !keys_) {
    *error = "there is no key store or random source";
    return false;
  }
  const TbDevice device = devices_[index];
  if (!parent_authorized(device)) {
    *error = "the device it hangs from is not approved";
    return false;
  }
  uint8_t key[kTbKeyBytes], request_bytes[tb_icm::kTrKeyedBytes], approve_request[tb_icm::kTrApproveBytes], approve_response[tb_icm::kTrApproveBytes];
  if (keys_->load(device.uuid, key)) {
    // the device has a key: it has to prove it still does
    uint8_t challenge[kTbKeyBytes], response[tb_icm::kTrChallengeResponseBytes], expected[kTbKeyBytes], answer[kTbKeyBytes];
    if (!random_->fill(ByteView(challenge, sizeof challenge))) {
      *error = "no random numbers";
      return false;
    }
    const uint32_t n = titan() ? tb_icm::build_challenge_device_tr(device, ConstByteView(challenge, sizeof challenge), ByteView(request_bytes, sizeof request_bytes))
                               : tb_icm::build_challenge_device(device, ConstByteView(challenge, sizeof challenge), ByteView(request_bytes, sizeof request_bytes));
    if (n == 0) return false;
    const uint32_t response_bytes = titan() ? tb_icm::kTrChallengeResponseBytes : tb_icm::kChallengeResponseBytes;
    if (!request(ConstByteView(request_bytes, n), ByteView(response, response_bytes), kRequestTimeoutUs)) {
      *error = "the firmware did not answer";
      return false;
    }
    const uint8_t flags = tb_icm::response_flags(ConstByteView(response, response_bytes));
    if (flags & tb_icm::kFlagNoKey) {
      *error = "the device has no key (it was reset): remove its key to give it a new one";
      return false;
    }
    if ((flags & tb_icm::kFlagError) || !(titan() ? tb_icm::parse_challenge_response_tr(ConstByteView(response, response_bytes), ByteView(answer, sizeof answer)) : tb_icm::parse_challenge_response(ConstByteView(response, response_bytes), ByteView(answer, sizeof answer)))) {
      *error = "the device rejected the challenge";
      return false;
    }
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(key, sizeof key), ConstByteView(challenge, sizeof challenge), expected);
    if (!crypto::equal_constant_time(expected, answer, kTbKeyBytes)) {
      *error = "the device did not know the key";
      return false;
    }
  } else {
    // first time: a new key goes into the device and into the store
    if (!random_->fill(ByteView(key, sizeof key))) {
      *error = "no random numbers";
      return false;
    }
    uint8_t response[tb_icm::kTrApproveBytes];
    const uint32_t n = titan() ? tb_icm::build_add_device_key_tr(device, ConstByteView(key, sizeof key), ByteView(request_bytes, sizeof request_bytes))
                               : tb_icm::build_add_device_key(device, ConstByteView(key, sizeof key), ByteView(request_bytes, sizeof request_bytes));
    if (n == 0 || !request_flags(ConstByteView(request_bytes, n), ByteView(response, approve_bytes()), kRequestTimeoutUs, error, "the device could not keep the key")) return false;
    if (!keys_->store(device.uuid, key)) {
      *error = "the key could not be saved";
      return false;
    }
  }
  const uint32_t n = build_approve(device, ByteView(approve_request, sizeof approve_request));
  if (n == 0 || !request_flags(ConstByteView(approve_request, n), ByteView(approve_response, approve_bytes()), kApproveTimeoutUs, error, "the PCIe tunnel could not be created")) return false;
  mark_authorized(device);
  return true;
}

bool ThunderboltIcm::topology(List<TbTopologySwitch>& out) {
  out.clear();
  if (outcome_ != Outcome::Started) return false;
  uint8_t request_bytes[tb_icm::kHeaderBytes], responses[tb_icm::kTopologyPackets * tb_icm::kTopologyBytes];
  const uint32_t n = tb_icm::build_get_topology(ByteView(request_bytes, sizeof request_bytes));
  uint32_t total = 0;
  bool ok = false;
  for (uint32_t attempt = 0; attempt <= kRetries && !ok; ++attempt) {
    const TbControl::Result result = control_.icm_request_packets(ConstByteView(request_bytes, n), ByteView(responses, sizeof responses), tb_icm::kTopologyBytes, tb_icm::kTopologyPackets,
                                                                  kRequestTimeoutUs, this, &total);
    if (result == TbControl::Result::Ok) ok = true;
    else if (result != TbControl::Result::Timeout) return false;
  }
  if (!ok) return false;
  for (uint32_t i = 0; i < total; ++i) {
    TbTopologySwitch t;
    if (!tb_icm::parse_topology_packet(ConstByteView(responses + i * tb_icm::kTopologyBytes, tb_icm::kTopologyBytes), &t) || !out.push_back(t)) {
      out.clear();
      return false;
    }
  }
  return true;
}

bool ThunderboltIcm::route_in_topology(const List<TbTopologySwitch>& switches, uint8_t link, uint8_t depth, uint64_t* route) {
  if (switches.count() == 0 || depth == 0) return false;
  uint32_t index = switches[0].behind(link);
  if (index == 0 || index >= switches.count()) return false;
  const TbTopologySwitch* sw = &switches[index];
  for (uint8_t i = 1; i < depth; ++i) {
    if (!sw->used) return false;
    const TbTopologySwitch* next = nullptr;
    for (uint32_t port = 0; port < 16; ++port) {
      const uint32_t behind = sw->behind(port);
      if (behind > sw->switch_index && behind < switches.count()) {
        next = &switches[behind];
        break;
      }
    }
    if (!next) return false;                  // (Linux keeps the router when none follows; a route to the wrong router is worse than none)
    sw = next;
  }
  *route = sw->route;
  return true;
}

bool ThunderboltIcm::route(uint64_t index, uint64_t* route) {
  if (outcome_ != Outcome::Started || index >= devices_.count()) return false;
  const TbDevice device = devices_[index];
  if (titan()) {                                   // (the events carry the route string)
    *route = device.route;
    return true;
  }
  uint8_t request_bytes[tb_icm::kGetRouteBytes], response[tb_icm::kGetRouteResponseBytes];
  const uint32_t n = tb_icm::build_get_route(device.link, device.depth, ByteView(request_bytes, sizeof request_bytes));
  if (n != 0 && request(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kRequestTimeoutUs) && !(tb_icm::response_flags(ConstByteView(response, sizeof response)) & tb_icm::kFlagError) &&
      tb_icm::parse_get_route(ConstByteView(response, sizeof response), route))
    return true;
  // a firmware without GET_ROUTE (Falcon Ridge) shows the routes in its topology
  List<TbTopologySwitch> switches(allocator_);
  return topology(switches) && route_in_topology(switches, device.link, device.depth, route);
}


// ---- XDomain

bool ThunderboltIcm::set_identity(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name) {
  identity_[0] = vendor_id;
  identity_[1] = device_id;
  identity_[2] = device_revision;
  // (the names may be the members themselves: copied up to the end of the text, the rest zeroed)
  char vendor[sizeof vendor_name_] = {}, device[sizeof device_name_] = {};
  for (uint32_t i = 0; vendor_name && vendor_name[i] != 0 && i + 1 < sizeof vendor; ++i) vendor[i] = vendor_name[i];
  for (uint32_t i = 0; device_name && device_name[i] != 0 && i + 1 < sizeof device; ++i) device[i] = device_name[i];
  for (uint32_t i = 0; i < sizeof vendor_name_; ++i) vendor_name_[i] = vendor[i];
  for (uint32_t i = 0; i < sizeof device_name_; ++i) device_name_[i] = device[i];
  if (!tb_properties_build_local(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_, offer_network_, network_flags_, local_block_)) return false;
  ++local_generation_;
  return true;
}

bool ThunderboltIcm::offer_network(uint32_t flags) {
  offer_network_ = true;
  network_flags_ = flags;
  return set_identity(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_);
}

bool ThunderboltIcm::ensure_identity() { return local_block_.count() != 0 || set_identity(identity_[0], identity_[1], identity_[2], vendor_name_, device_name_); }

namespace {
bool uuid_equal(const uint8_t* a, const uint8_t* b) {
  bool same = true;
  for (uint32_t k = 0; k < 16; ++k) same = same && a[k] == b[k];
  return same;
}
constexpr uint32_t kXdpTimeoutUs = 2000000, kXdpAttempts = 4, kXdpGiveUpAfter = 5, kXdpRetryPolls = 50;
}  // namespace

// What the other host asks of us: who we are, and our properties.
void ThunderboltIcm::answer_xdomain_request(ConstByteView body) {
  tb_xdp::Header h;
  if (!tb_xdp::parse_header(body, &h) || !ensure_identity()) return;
  // the host this is for: the connection with that route, else (a UUID request may come before the connection is announced) the first one there is
  const TbXDomain* xd = nullptr;
  for (uint64_t i = 0; i < xdomains_.count(); ++i)
    if (xdomains_[i].route == h.route) xd = &xdomains_[i];
  if (!xd && xdomains_.count() != 0 && h.type == tb_xdp::kUuidRequest) xd = &xdomains_[0];
  if (!xd) return;
  const uint64_t route = xd->route;
  uint8_t local_uuid[16];
  for (uint32_t k = 0; k < 16; ++k) local_uuid[k] = xd->local_uuid[k];
  uint8_t out[tb_frame::kMaxBody];
  uint32_t n = 0;
  switch (h.type) {
    case tb_xdp::kUuidRequest:
      n = tb_xdp::build_uuid_response(route, h.sequence, local_uuid, ByteView(out, sizeof out));
      break;
    case tb_xdp::kPropertiesRequest: {
      tb_xdp::PropertiesRequest q;
      if (!tb_xdp::parse_properties_request(body, &q)) return;
      if (!uuid_equal(q.destination, local_uuid)) {
        n = tb_xdp::build_error_response(route, h.sequence, tb_xdp::kErrorUnknownDomain, ByteView(out, sizeof out));
      } else {
        n = tb_xdp::build_properties_response(route, h.sequence, local_uuid, q.source, ArrayView<const uint32_t>(&local_block_[0], local_block_.count()), local_generation_, q.offset, ByteView(out, sizeof out));
      }
      break;
    }
    case tb_xdp::kPropertiesChangedRequest:
      // (they changed their properties: read them again)
      n = tb_xdp::build_properties_changed_response(route, h.sequence, ByteView(out, sizeof out));
      for (uint64_t i = 0; i < xdomains_.count(); ++i)
        if (xdomains_[i].route == route) {
          xdomains_[i].discovered = false;
          xdomains_[i].gave_up = false;
          xdomains_[i].attempts = 0;
        }
      break;
    default:
      // link state requests and anything newer: not supported here
      n = tb_xdp::build_error_response(route, h.sequence, tb_xdp::kErrorNotSupported, ByteView(out, sizeof out));
      break;
  }
  if (n != 0) control_.send(TbPacketType::XdomainResponse, ConstByteView(out, n));
}

// The exchange with a host that was just announced (xdomain.c: the UUID request, then the properties).
bool ThunderboltIcm::discover_xdomain(uint64_t index) {
  if (!ensure_identity()) return false;
  const TbXDomain xd = xdomains_[index];
  uint8_t request[tb_frame::kMaxBody], response[tb_frame::kFrameBytes];
  uint32_t got = 0;

  // who is there
  uint32_t n = tb_xdp::build_uuid_request(xd.route, xd.attempts % 4, ByteView(request, sizeof request));
  if (n == 0 || control_.xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, kXdpTimeoutUs, this) != TbControl::Result::Ok) return false;
  tb_xdp::UuidResponse who;
  uint32_t error = 0;
  if (tb_xdp::parse_uuid_response(ConstByteView(response, got < sizeof response ? got : sizeof response), &who, &error) != tb_xdp::Parsed::Ok) return false;
  if (!uuid_equal(who.uuid, xd.remote_uuid)) return false;                   // another host than announced: it will be announced again

  // its properties, a piece at a time
  List<uint32_t> block(allocator_);
  uint32_t length = 0, offset = 0, generation = 0;
  for (uint32_t round = 0; round < tb_xdp::kMaxBlockWords; ++round) {
    n = tb_xdp::build_properties_request(xd.route, xd.attempts % 4, xd.local_uuid, xd.remote_uuid, static_cast<uint16_t>(offset), ByteView(request, sizeof request));
    if (n == 0 || control_.xdomain_request(ConstByteView(request, n), ByteView(response, sizeof response), &got, kXdpTimeoutUs, this) != TbControl::Result::Ok) return false;
    const ConstByteView packet(response, got < sizeof response ? got : sizeof response);
    tb_xdp::PropertiesResponse p;
    if (tb_xdp::parse_properties_response(packet, &p, &error) != tb_xdp::Parsed::Ok || p.offset != offset) return false;
    if (length == 0) {
      length = p.data_length;
      generation = p.generation;
      if (length == 0 || length > tb_xdp::kMaxBlockWords) return false;
      for (uint32_t i = 0; i < length; ++i)
        if (!block.push_back(0)) return false;
    } else if (p.data_length != length) {
      return false;
    }
    if (p.words == 0) return false;
    ByteReader r(packet);
    r.skip(tb_xdp::kPropertiesResponseHeaderBytes);
    for (uint32_t i = 0; i < p.words && offset + i < length; ++i) block[offset + i] = r.le32();
    if (!r.ok()) return false;
    offset += p.words;
    if (offset >= length) break;
  }
  if (offset < length) return false;

  List<TbPropertyEntry> entries(allocator_);
  if (!tb_properties_parse(ArrayView<const uint32_t>(&block[0], block.count()), entries)) return false;

  // the list may have changed while waiting
  for (uint64_t i = 0; i < xdomains_.count(); ++i) {
    TbXDomain& x = xdomains_[i];
    if (!uuid_equal(x.remote_uuid, xd.remote_uuid)) continue;
    x.generation = generation;
    const TbPropertyEntry* vendor = tb_properties_find(entries, "vendorid", TbPropertyType::Value);
    const TbPropertyEntry* device = tb_properties_find(entries, "deviceid", TbPropertyType::Value);
    x.vendor_id = vendor ? vendor->value : 0;
    x.device_id = device ? device->value : 0;
    const TbPropertyEntry* vendor_name = tb_properties_find(entries, "vendorid", TbPropertyType::Text);
    const TbPropertyEntry* device_name = tb_properties_find(entries, "deviceid", TbPropertyType::Text);
    for (uint32_t k = 0; k < sizeof x.vendor_name; ++k) x.vendor_name[k] = vendor_name && k + 1 < sizeof x.vendor_name ? vendor_name->text[k] : 0;
    for (uint32_t k = 0; k < sizeof x.device_name; ++k) x.device_name[k] = device_name && k + 1 < sizeof x.device_name ? device_name->text[k] : 0;
    // the services: a directory of the root holds the protocol's id (prtcid), its settings (prtcstns) and more, at depth 1
    x.protocol_count = 0;
    x.has_network = false;
    x.network_flags = 0;
    {
      uint32_t id = 0, flags = 0;
      bool have_id = false;
      auto close = [&]() {
        if (!have_id) return;
        if (x.protocol_count < 8) x.protocols[x.protocol_count++] = id;
        if (id == 1) {
          x.has_network = true;
          x.network_flags = flags;
        }
      };
      auto is = [](const TbPropertyEntry& e, const char* key) {
        for (uint32_t k = 0; k < 9; ++k) {
          if (e.key[k] != key[k]) return false;
          if (key[k] == 0) return true;
        }
        return true;
      };
      for (uint64_t e = 0; e < entries.count(); ++e) {
        const TbPropertyEntry& entry = entries[e];
        if (entry.depth == 0) {
          if (entry.type == TbPropertyType::Directory) {
            close();
            have_id = false;
            id = flags = 0;
          }
        } else if (entry.depth == 1 && entry.type == TbPropertyType::Value) {
          if (is(entry, "prtcid")) { id = entry.value; have_id = true; }
          else if (is(entry, "prtcstns")) flags = entry.value;
        }
      }
      close();
    }
    x.discovered = true;
    return true;
  }
  return true;
}

// Called from poll(), when no request is waiting: one host at a time, a few tries each, spaced out.
void ThunderboltIcm::service_xdomains() {
  ++polls_;
  for (uint64_t i = 0; i < xdomains_.count(); ++i) {
    const TbXDomain x = xdomains_[i];                       // (a copy: the list can change during the exchange)
    if (x.discovered || x.gave_up) continue;
    // (a retry waits some polls; the first try is at once)
    if (x.attempts != 0 && polls_ % kXdpRetryPolls != 0) continue;
    const bool ok = discover_xdomain(i);
    // find it again: the exchange can take events that change the list
    for (uint64_t k = 0; k < xdomains_.count(); ++k) {
      bool same = true;
      for (uint32_t b = 0; b < 16; ++b) same = same && xdomains_[k].remote_uuid[b] == x.remote_uuid[b];
      if (!same) continue;
      if (!ok && !xdomains_[k].discovered && ++xdomains_[k].attempts >= kXdpGiveUpAfter) xdomains_[k].gave_up = true;
      break;
    }
    return;
  }
}


bool ThunderboltIcm::approve_xdomain_paths(uint64_t index, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (outcome_ != Outcome::Started) {
    *error = "the controller is not running";
    return false;
  }
  if (index >= xdomains_.count()) {
    *error = "no such host";
    return false;
  }
  for (uint64_t i = 0; i < xdomains_.count(); ++i)
    if (xdomains_[i].tunnel) {
      *error = "the firmware supports one tunnel to another host at a time";
      return false;
    }
  const TbXDomain xd = xdomains_[index];
  uint8_t request_bytes[tb_icm::kTrApproveXdomainBytes], response[tb_icm::kTrApproveXdomainBytes];
  const uint32_t n = titan() ? tb_icm::build_approve_xdomain_tr(xd, transmit_path, transmit_ring, receive_path, receive_ring, ByteView(request_bytes, sizeof request_bytes))
                             : tb_icm::build_approve_xdomain(xd, transmit_path, transmit_ring, receive_path, receive_ring, ByteView(request_bytes, sizeof request_bytes));
  const uint32_t response_bytes = titan() ? tb_icm::kTrApproveXdomainBytes : tb_icm::kApproveXdomainBytes;
  if (n == 0 || !request_flags(ConstByteView(request_bytes, n), ByteView(response, response_bytes), kRequestTimeoutUs, error, "the firmware refused the path")) return false;
  for (uint64_t i = 0; i < xdomains_.count(); ++i)
    if (uuid_equal(xdomains_[i].remote_uuid, xd.remote_uuid)) xdomains_[i].tunnel = true;
  return true;
}

bool ThunderboltIcm::disconnect_xdomain_paths(uint64_t index) {
  if (outcome_ != Outcome::Started || index >= xdomains_.count()) return false;
  const TbXDomain xd = xdomains_[index];
  bool ok = true;
  if (titan()) {
    // two stages (icm_tr_disconnect_xdomain_paths)
    for (uint8_t stage = 1; stage <= 2 && ok; ++stage) {
      uint8_t request_bytes[tb_icm::kTrDisconnectXdomainBytes], response[tb_icm::kTrDisconnectXdomainBytes];
      const uint32_t n = tb_icm::build_disconnect_xdomain_tr(xd, stage, ByteView(request_bytes, sizeof request_bytes));
      const char* error = nullptr;
      ok = n != 0 && request_flags(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kRequestTimeoutUs, &error, "refused");
      if (stage == 1) delay_us(30);
    }
  } else {
    // (Alpine Ridge: the mailbox, one command per physical port, in two steps)
    const uint32_t command = physical_port(xd.link) == 0 ? tb_mailbox::kDisconnectPortA : tb_mailbox::kDisconnectPortB;
    nhi_.mailbox(command, 1, 500000);
    delay_us(30);
    nhi_.mailbox(command, 2, 500000);
  }
  for (uint64_t i = 0; i < xdomains_.count(); ++i)
    if (uuid_equal(xdomains_[i].remote_uuid, xd.remote_uuid)) xdomains_[i].tunnel = false;
  return ok;
}

}  // namespace hardware

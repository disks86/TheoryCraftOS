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

bool ThunderboltIcm::request(ConstByteView request, ByteView response, uint32_t timeout_us) {
  for (uint32_t attempt = 0; attempt <= kRetries; ++attempt) {
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
  ready_ = TbDriverReady();
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
  uint8_t request_bytes[tb_icm::kDriverReadyBytes], response[tb_icm::kDriverReadyResponseBytes];
  const uint32_t n = tb_icm::build_driver_ready(ByteView(request_bytes, sizeof request_bytes));
  if (n == 0 || !request(ConstByteView(request_bytes, n), ByteView(response, sizeof response), timeout_us) || !tb_icm::parse_driver_ready(ConstByteView(response, sizeof response), &ready_)) {
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
  return outcome_ = Outcome::Started;
}

// icm_firmware_start: the ARC is told to wait for a CIO reset, restarted, and the reset is triggered through the PCIe-to-CIO registers; the firmware then authenticates its image.
bool ThunderboltIcm::start_firmware() {
  ITbRegisters& regs = nhi_.registers();
  regs.write32(tb_reg::kFirmwareStatus, regs.read32(tb_reg::kFirmwareStatus) | tb_reg::kFwStsCioResetRequest);
  regs.write32(tb_reg::kFirmwareStatus, regs.read32(tb_reg::kFirmwareStatus) | tb_reg::kFwStsIcmEnableInvert | tb_reg::kFwStsIcmEnableCpu);
  if (!options_.cio->write(TbConfigSpace::Switch, 0, tb_cio::kSwitchResetIndex, tb_cio::kSwitchResetBit)) return false;
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

bool ThunderboltIcm::suspend() {
  if (outcome_ != Outcome::Started) return false;
  nhi_.mailbox(tb_mailbox::kSaveDevices, 0, 500000);
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
  outcome_ = Outcome::NotStarted;
}

void ThunderboltIcm::poll() {
  if (outcome_ == Outcome::Started || outcome_ == Outcome::SafeMode) control_.poll(this);
}

void ThunderboltIcm::remove_downstream(uint8_t link, uint8_t depth) {
  // the device and everything behind it on the same physical port
  for (uint64_t i = devices_.count(); i-- > 0;)
    if (physical_port(devices_[i].link) == physical_port(link) && devices_[i].depth >= depth) devices_.remove_at(i);
}

void ThunderboltIcm::on_event(TbPacketType type, ConstByteView body) {
  if (type != TbPacketType::IcmEvent) return;
  uint8_t code = 0;
  if (!body.try_get(0, &code)) return;
  if (code == tb_icm::kEventDeviceConnected) {
    TbDevice device;
    bool rejected = false;
    if (!tb_icm::parse_device_connected(body, &device, &rejected) || rejected) return;
    // the same device again (the firmware repeats the ones that are still there after a resume), or a stale entry at that place
    for (uint64_t i = devices_.count(); i-- > 0;) {
      const TbDevice& known = devices_[i];
      bool same = true;
      for (uint32_t k = 0; k < sizeof device.uuid; ++k) same = same && known.uuid[k] == device.uuid[k];
      if (same || (physical_port(known.link) == physical_port(device.link) && known.depth == device.depth)) devices_.remove_at(i);
    }
    if (devices_.count() < kMaxDevices) devices_.push_back(device);
  } else if (code == tb_icm::kEventDeviceDisconnected) {
    uint8_t link = 0, depth = 0;
    if (tb_icm::parse_device_disconnected(body, &link, &depth)) remove_downstream(link, depth);
  }
}

// A device hangs from the one a step nearer on the same physical port: that one has to be approved first.
bool ThunderboltIcm::parent_authorized(const TbDevice& device) const {
  if (device.depth <= 1) return true;
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
  uint8_t request_bytes[tb_icm::kApproveBytes], response[tb_icm::kApproveBytes];
  const uint32_t n = tb_icm::build_approve_device(device, ByteView(request_bytes, sizeof request_bytes));
  if (n == 0 || !request_flags(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kApproveTimeoutUs, error, "the PCIe tunnel could not be created")) return false;
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
  uint8_t key[kTbKeyBytes], request_bytes[tb_icm::kKeyedBytes], approve_request[tb_icm::kApproveBytes], approve_response[tb_icm::kApproveBytes];
  if (keys_->load(device.uuid, key)) {
    // the device has a key: it has to prove it still does
    uint8_t challenge[kTbKeyBytes], response[tb_icm::kChallengeResponseBytes], expected[kTbKeyBytes], answer[kTbKeyBytes];
    if (!random_->fill(ByteView(challenge, sizeof challenge))) {
      *error = "no random numbers";
      return false;
    }
    const uint32_t n = tb_icm::build_challenge_device(device, ConstByteView(challenge, sizeof challenge), ByteView(request_bytes, sizeof request_bytes));
    if (n == 0) return false;
    if (!request(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kRequestTimeoutUs)) {
      *error = "the firmware did not answer";
      return false;
    }
    const uint8_t flags = tb_icm::response_flags(ConstByteView(response, sizeof response));
    if (flags & tb_icm::kFlagNoKey) {
      *error = "the device has no key (it was reset): remove its key to give it a new one";
      return false;
    }
    if ((flags & tb_icm::kFlagError) || !tb_icm::parse_challenge_response(ConstByteView(response, sizeof response), ByteView(answer, sizeof answer))) {
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
    uint8_t response[tb_icm::kApproveBytes];
    const uint32_t n = tb_icm::build_add_device_key(device, ConstByteView(key, sizeof key), ByteView(request_bytes, sizeof request_bytes));
    if (n == 0 || !request_flags(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kRequestTimeoutUs, error, "the device could not keep the key")) return false;
    if (!keys_->store(device.uuid, key)) {
      *error = "the key could not be saved";
      return false;
    }
  }
  const uint32_t n = tb_icm::build_approve_device(device, ByteView(approve_request, sizeof approve_request));
  if (n == 0 || !request_flags(ConstByteView(approve_request, n), ByteView(approve_response, sizeof approve_response), kApproveTimeoutUs, error, "the PCIe tunnel could not be created")) return false;
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
  uint8_t request_bytes[tb_icm::kGetRouteBytes], response[tb_icm::kGetRouteResponseBytes];
  const uint32_t n = tb_icm::build_get_route(device.link, device.depth, ByteView(request_bytes, sizeof request_bytes));
  if (n != 0 && request(ConstByteView(request_bytes, n), ByteView(response, sizeof response), kRequestTimeoutUs) && !(tb_icm::response_flags(ConstByteView(response, sizeof response)) & tb_icm::kFlagError) &&
      tb_icm::parse_get_route(ConstByteView(response, sizeof response), route))
    return true;
  // a firmware without GET_ROUTE (Falcon Ridge) shows the routes in its topology
  List<TbTopologySwitch> switches(allocator_);
  return topology(switches) && route_in_topology(switches, device.link, device.depth, route);
}

}  // namespace hardware

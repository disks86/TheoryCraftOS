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

// The connection manager firmware (ICM) of an Alpine Ridge Thunderbolt 3 controller, from the host's side (icm.c of the Linux thunderbolt driver, the Falcon Ridge / Alpine Ridge
// messages). The firmware enumerates the devices on the ports and builds the tunnels itself; the host says it is ready (DRIVER_READY, which also tells the security level), is told
// about every device that is connected or disconnected, and asks for a device to be approved, which makes the firmware create its PCIe tunnel. The tunnel gives the device DMA
// access to memory, so nothing is approved unless the caller asks, and only at the security levels where one approval is enough (none, user).
// The messages are structures of 32 bit words in the layout the Linux driver uses (little endian in memory); TbControl puts them on the wire.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Crypto/Random.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltCio.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltDevices.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltKeys.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltRing.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace tb_icm {
// icm_pkg_code
constexpr uint8_t kGetTopology = 0x1, kDriverReady = 0x3, kApproveDevice = 0x4, kChallengeDevice = 0x5, kAddDeviceKey = 0x6, kGetRoute = 0xA;
// icm_event_code
constexpr uint8_t kEventDeviceConnected = 0x3, kEventDeviceDisconnected = 0x4, kEventXdomainConnected = 0x6, kEventXdomainDisconnected = 0x7;
// the header's flags
constexpr uint8_t kFlagError = 1u << 0, kFlagNoKey = 1u << 1, kFlagDualLane = 1u << 5, kFlagRtd3 = 1u << 6, kFlagSpeedGen3 = 1u << 7;
constexpr uint32_t kFlagSecurityShift = 3, kFlagSecurityMask = 3u << 3;
// link_info
constexpr uint16_t kLinkMask = 0x7, kDepthMask = 0xF0, kApproved = 1u << 8, kRejected = 1u << 9, kBoot = 1u << 10;
constexpr uint32_t kDepthShift = 4;
// the sizes of the structures
constexpr uint32_t kTopologyBytes = 152, kTopologyPackets = 14, kPortTypePort = 1;
constexpr uint32_t kKeyedBytes = 56, kChallengeResponseBytes = 88;
// Titan Ridge: devices are named by their route string
constexpr uint32_t kTrDriverReadyResponseBytes = 16, kTrApproveBytes = 32, kTrKeyedBytes = 64, kTrChallengeResponseBytes = 96, kTrDeviceConnectedMinBytes = 32, kTrDeviceDisconnectedBytes = 12;
constexpr uint32_t kHeaderBytes = 4, kDriverReadyBytes = 4, kDriverReadyResponseBytes = 8, kApproveBytes = 24, kGetRouteBytes = 8, kGetRouteResponseBytes = 16, kDeviceConnectedMinBytes = 24,
                   kDeviceDisconnectedBytes = 8;
}  // namespace tb_icm

// tb_security_level (the low bits of the driver ready response's info word)
enum class TbSecurityLevel : uint8_t { None = 0, User = 1, Secure = 2, DisplayPortOnly = 3, UsbOnly = 4, NoPcie = 5, Unknown = 255 };

struct TbDriverReady {
  uint8_t rom_version = 0, ram_version = 0;
  TbSecurityLevel security = TbSecurityLevel::Unknown;
  uint32_t boot_acl_entries = 0;      // 0 if the firmware has no preboot ACL
  bool rtd3 = false;
};

// A device the firmware reports.
struct TbDevice {
  uint8_t uuid[16] = {};
  uint8_t connection_key = 0, connection_id = 0;
  uint8_t link = 0, depth = 0;         // the port (1 to 4) and how many routers away from the host (Alpine Ridge)
  uint64_t route = 0;                  // the route string, 8 bits a hop (Titan Ridge; the depth is how many hops, the link is the first)
  bool authorized = false, boot = false, dual_lane = false, gen3 = false;
  TbSecurityLevel security = TbSecurityLevel::Unknown;
};

// One router of the topology the firmware reports (one GET_TOPOLOGY packet each; the packet's number is the router's index, the host is 0).
struct TbTopologySwitch {
  bool used = false;
  uint8_t upstream_port = 0, switch_index = 0;
  uint64_t route = 0;
  uint32_t ports[16] = {};         // type in the low 24 bits (1: a lane port), the index of the router behind it in the top 8 (0xFF: none)
  // The index of the router behind port `port`, or 0 if there is none (the host is 0 and is never behind a port).
  uint32_t behind(uint32_t port) const {
    if (port >= 16 || (ports[port] & 0xFFFFFF) != 1) return 0;
    const uint32_t index = ports[port] >> 24;
    return index != 0xFF ? index : 0;
  }
};

namespace tb_icm {
uint32_t build_get_topology(ByteView out);
bool parse_topology_packet(ConstByteView packet, TbTopologySwitch* out);
uint32_t build_driver_ready(ByteView out);
bool parse_driver_ready(ConstByteView response, TbDriverReady* out);
uint32_t build_approve_device(const TbDevice& device, ByteView out);
uint32_t build_get_route(uint8_t link, uint8_t depth, ByteView out);
// ADD_DEVICE_KEY and CHALLENGE_DEVICE: the device, then 32 bytes (the key, the challenge).
uint32_t build_add_device_key(const TbDevice& device, ConstByteView key, ByteView out);
// The Titan Ridge forms (the device is named by its route string; the layout has the route words after the UUID).
uint32_t build_approve_device_tr(const TbDevice& device, ByteView out);
uint32_t build_add_device_key_tr(const TbDevice& device, ConstByteView key, ByteView out);
uint32_t build_challenge_device_tr(const TbDevice& device, ConstByteView challenge, ByteView out);
bool parse_challenge_response_tr(ConstByteView response, ByteView out);
bool parse_driver_ready_tr(ConstByteView response, TbDriverReady* out);
// Only the first packet of a device connected event counts (the next ones carry quality of service information).
bool parse_device_connected_tr(ConstByteView event, TbDevice* out, bool* rejected);
bool parse_device_disconnected_tr(ConstByteView event, uint64_t* route);
// How many hops a route string has.
uint32_t route_length(uint64_t route);
uint32_t build_challenge_device(const TbDevice& device, ConstByteView challenge, ByteView out);
// The 32 byte response of a CHALLENGE_DEVICE answer.
bool parse_challenge_response(ConstByteView response, ByteView out);
// The header flags of a response (kFlagError ...), or 0.
uint8_t response_flags(ConstByteView response);
bool parse_get_route(ConstByteView response, uint64_t* route);
// An ICM_EVENT_DEVICE_CONNECTED. `rejected` is the firmware refusing the device (too deep a topology).
bool parse_device_connected(ConstByteView event, TbDevice* out, bool* rejected);
bool parse_device_disconnected(ConstByteView event, uint8_t* link, uint8_t* depth);
}  // namespace tb_icm

class ThunderboltIcm final : public IThunderboltEvents {
 public:
  enum class Outcome : uint8_t {
    NotStarted,
    Started,
    Suspended,           // suspend() was called: the channel is down, the firmware has been told

    NotPresent,          // the registers do not answer
    FirmwareNotRunning,  // the BIOS did not start the firmware, and this driver was not told to (or cannot)
    FirmwareStartFailed, // the driver restarted the firmware through the PCIe-to-CIO registers and it did not come up
    ControlFailed,       // the control channel's rings
    AuthTimeout,         // the firmware's NVM image is not authenticated
    SafeMode,            // the controller needs an NVM update before it works: no devices
    WrongMode,           // the firmware is in a mode that does not manage the controller
    DriverReadyFailed,   // no answer to DRIVER_READY
    RootSwitchFailed,    // the root router's configuration space does not become readable
  };
  // What the caller can offer beyond the NHI. `cio` is the way through the upstream port's vendor registers (nullptr: none).
  struct Options {
    TbPcie2Cio* cio = nullptr;
    bool start_firmware = false;       // start the firmware if the BIOS did not (needs `cio`); off by default: it resets the controller's CPU
    TbGeneration generation = TbGeneration::AlpineRidge;      // which messages the firmware speaks
    bool reset_ports = true;           // bounce the links of both physical ports (needs `cio`) so that the firmware enumerates what is connected afresh
  };
  static constexpr uint32_t kRequestTimeoutUs = 5000000, kApproveTimeoutUs = 10000000, kRetries = 3, kMaxDevices = 32;

  ThunderboltIcm(ITbRegisters& registers, allocators::IAllocator& allocator) : allocator_(allocator), nhi_(registers), control_(nhi_, allocator), devices_(allocator) {}
  ~ThunderboltIcm() { stop(); }
  ThunderboltIcm(const ThunderboltIcm&) = delete;
  ThunderboltIcm& operator=(const ThunderboltIcm&) = delete;

  static const char* outcome_name(Outcome outcome);
  void set_options(const Options& options) { options_ = options; }
  // How many of the two port resets failed in the last start() (a failure does not stop the start).
  uint32_t port_reset_failures() const { return port_reset_failures_; }

  // Brings the controller up: interrupts off, the control channel, the firmware's mode, DRIVER_READY, then waits for the root router. Devices that are already connected are reported
  // through events during the next poll() calls.
  Outcome start(uint32_t timeout_us = kRequestTimeoutUs);
  // System sleep (icm_suspend / icm_complete): the firmware is asked to save the devices it knows (SAVE_DEVS) and told the driver goes away, the control channel is stopped and its memory
  // freed, the device list is dropped. Putting the PCI function to sleep is for the caller. resume() brings the channel up again, restarts the firmware if it did not survive, sends
  // DRIVER_READY and waits for the root router; the firmware then reports the devices that are still connected again (with their approval), so nothing is approved by resuming.
  bool suspend();
  Outcome resume(uint32_t timeout_us = kRequestTimeoutUs);
  // Tells the firmware the driver goes away (the mailbox's DRIVER_UNLOADS) and stops the channel.
  void stop();
  bool running() const { return outcome_ == Outcome::Started; }
  // Interrupts from the control channel's rings (the caller routes the MSI and calls TbNhi::acknowledge_interrupt from its handler). Only unmasks them in the chip; the channel is still
  // read by poll(), which the caller may then call only after an interrupt. Remembered over suspend and resume. False if the controller is not running.
  bool use_interrupts(bool on);
  bool interrupts_on() const { return interrupts_; }
  Outcome outcome() const { return outcome_; }

  // Reads the control channel: events update the device list.
  void poll();

  const TbDriverReady& driver_ready() const { return ready_; }
  TbSecurityLevel security_level() const { return ready_.security; }
  uint64_t device_count() const { return devices_.count(); }
  const TbDevice& device(uint64_t index) const { return devices_[index]; }

  // Asks the firmware to approve the device (and so to create its PCIe tunnel). Refused here unless the security level is none or user. On false, `*error` says why.
  bool approve(uint64_t index, const char** error);
  // Secure connect (security level "secure"). Needs a key store and a random source. A device that has no key yet is given a new one (ADD_DEVICE_KEY: the device keeps it in its NVM, we
  // keep it in the store) and approved; one that has is sent a random challenge and approved only if its answer is HMAC-SHA256 of the challenge under the stored key. A device that
  // lost its key (or a different device with the same UUID) fails the challenge and stays unapproved. A device can only be approved after the one it hangs from.
  void set_secure(crypto::IRandom* random, ITbKeyStore* keys) { random_ = random; keys_ = keys; }
  bool approve_secure(uint64_t index, const char** error);
  // The route string of the device (ICM_GET_ROUTE).
  bool route(uint64_t index, uint64_t* route);
  // The topology the firmware reports (GET_TOPOLOGY, an answer of up to 14 packets): `out` gets one entry per packet.
  bool topology(List<TbTopologySwitch>& out);
  // The route string of the router at `link` and `depth`, found by walking that topology (icm_fr_get_route): the first router behind the link, then the next that follows in the index order.
  static bool route_in_topology(const List<TbTopologySwitch>& switches, uint8_t link, uint8_t depth, uint64_t* route);

  TbNhi& nhi() { return nhi_; }
  TbControl& control() { return control_; }

  void on_event(TbPacketType type, ConstByteView body) override;

 private:
  bool titan() const { return options_.generation == TbGeneration::TitanRidge; }
  bool same_place(const TbDevice& a, const TbDevice& b) const;
  uint32_t approve_bytes() const { return titan() ? tb_icm::kTrApproveBytes : tb_icm::kApproveBytes; }
  uint32_t build_approve(const TbDevice& device, ByteView out) const { return titan() ? tb_icm::build_approve_device_tr(device, out) : tb_icm::build_approve_device(device, out); }
  Outcome bring_up(uint32_t timeout_us, bool resuming);
  bool request(ConstByteView request, ByteView response, uint32_t timeout_us, uint32_t retries = kRetries);
  bool parent_authorized(const TbDevice& device) const;
  void mark_authorized(const TbDevice& device);
  bool request_flags(ConstByteView request, ByteView response, uint32_t timeout_us, const char** error, const char* no_answer);
  bool start_firmware();
  bool reset_physical_port(uint32_t physical_port);
  void remove_downstream(uint8_t link, uint8_t depth);

  allocators::IAllocator& allocator_;
  TbNhi nhi_;
  TbControl control_;
  List<TbDevice> devices_;
  TbDriverReady ready_;
  crypto::IRandom* random_ = nullptr;
  bool interrupts_ = false;
  ITbKeyStore* keys_ = nullptr;
  Options options_;
  uint32_t port_reset_failures_ = 0;
  Outcome outcome_ = Outcome::NotStarted;
};

}  // namespace hardware

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

// The Xbox Wireless Adapter for Windows (045E:02E6 first generation, 045E:02FE the slim second, 045E:02F9 and 045E:091E built into some
// laptops and headsets): a MediaTek radio that Xbox One and Series controllers pair with. The host loads Microsoft's firmware into it
// ("/firmware/xow_dongle.bin" and friends, which the user copies from Microsoft's driver package: see docs/GameControllers.md), turns the radio
// on on a quiet channel and sends a beacon; controllers then associate, and each talks GIP (XboxProtocol.hpp) in 802.11 QoS data frames
// (XboxWirelessFrames.hpp has the layouts and says which parts are uncertain).
//
// The adapter is one XboxWirelessAdapter (firmware, radio, the bulk endpoints, the station table); open_xbox_wireless_adapter() hands out one
// IGameController per client slot (eight), each holding a SharedPointer to it. A slot's pad is connected() from the controller's GIP
// announcement until it leaves, so the game controller service gives it a player slot only then, as with the Xbox 360 receiver.
//
// Locking: the adapter's state is behind one kernel::Mutex (a sleeping lock: its sections wait on USB transfers, which a SpinLock must not).
// Every public member takes it; nothing is called back with it held, and the pads copy their state out. The service polls the pads from its
// thread, and any pad's poll() reads the shared endpoints for all of them.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/GameController/IGameController.hpp"
#include "Hardware/GameController/Usb/XboxWirelessFrames.hpp"
#include "Hardware/WirelessInterface/WifiFirmwareSource.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Whether a USB device is one of the adapters, and what it is called ("Xbox Wireless Adapter").
[[nodiscard]] bool is_xbox_wireless_adapter(uint16_t vendor, uint16_t product);
const char* xbox_wireless_adapter_name(uint16_t product);
// The firmware files tried for an adapter, in order (names relative to the firmware folder, "/firmware"). *count gets how many.
const char* const* xbox_wireless_firmware_names(uint16_t product, uint32_t* count);

// What the driver needs from the rest of the system: the firmware files, and where to say why an adapter was refused. Set once by the
// kernel after the file system with "/firmware" is mounted (the object must outlive every adapter); null fields are allowed.
struct XboxWirelessEnvironment {
  IWifiFirmwareSource* firmware = nullptr;
  void (*log)(void* context, StringView text) = nullptr;
  void* log_context = nullptr;
  uint32_t channel_dwell_us = 10000;   // how long each candidate channel is listened to before the quietest is picked
};
void set_xbox_wireless_environment(const XboxWirelessEnvironment* environment);

enum class XboxWirelessStatus : uint8_t {
  Ok,
  NotAdapter,           // the device on the port is not one of the adapters
  NoFirmwareSource,     // no file system to read the firmware from (yet)
  FirmwareMissing,      // none of the firmware files is there
  FirmwareInvalid,      // a file is there but its header and size disagree
  NoEndpoints,          // the configuration has no bulk endpoints for the radio
  UsbUnsupported,       // the host controller cannot receive bulk transfers without waiting (xHCI and EHCI can)
  UsbError,             // a transfer failed (the adapter was unplugged, say)
  FirmwareNotStarted,   // the firmware went in but the MCU never said it runs
  NoMemory,
};
const char* xbox_wireless_status_text(XboxWirelessStatus status);

class XboxWirelessAdapter {
 public:
  static constexpr uint32_t kMaxClients = 8;

  XboxWirelessAdapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);
  ~XboxWirelessAdapter() { stop(); }
  XboxWirelessAdapter(const XboxWirelessAdapter&) = delete;
  XboxWirelessAdapter& operator=(const XboxWirelessAdapter&) = delete;

  // Loads the firmware, brings the radio up and starts the beacon. *detail (if given) gets a static text saying what failed or which file went in.
  [[nodiscard]] XboxWirelessStatus start(IWifiFirmwareSource& firmware, const char** detail);
  void set_channel_dwell(uint32_t microseconds) { channel_dwell_us_ = microseconds; }   // (before start())
  // Turns the radio off (when the service lets go of a still-plugged adapter).
  void stop();
  // Starts later, from pump(), once set_xbox_wireless_environment() has given a firmware source (the game controller service takes devices up
  // before the file system is mounted). If that start fails the adapter says why in the log and counts as gone.
  void start_when_firmware_is_there(const char* name);
  // The device is still the one that was started (and was not refused at a later start).
  bool present() const;

  // Reads what arrived on the bulk IN endpoints and acts on it (association, pairing, GIP, departures). Never blocks on input.
  void pump();

  // Opens or closes pairing (the adapter's button opens it too); it closes by itself when a controller paired.
  [[nodiscard]] bool set_pairing(bool open);
  bool pairing() const;
  uint8_t channel() const;
  MacAddress address() const;

  // A client slot (0..7, WCID slot + 1): whether a controller is there and has announced itself, and a copy of its state.
  bool client_connected(uint32_t slot) const;
  [[nodiscard]] bool client_state(uint32_t slot, gamepad::State* state) const;
  // Changes whenever a slot's controller arrives, leaves or reports something new.
  uint32_t client_generation(uint32_t slot) const;
  [[nodiscard]] bool client_rumble(uint32_t slot, uint16_t low_frequency, uint16_t high_frequency);

 private:
  struct Client {
    bool associated = false;        // in the station table
    bool announced = false;         // its GIP announcement came and it was told to power on
    MacAddress address{};
    uint8_t gip_sequence = 1;
    uint16_t frame_sequence = 0;
    uint32_t generation = 0;
    gamepad::State state;
  };

  // (all with mutex_ held)
  XboxWirelessStatus find_endpoints();
  XboxWirelessStatus load_firmware(ConstByteView file);
  XboxWirelessStatus start_radio();
  XboxWirelessStatus start_locked(IWifiFirmwareSource& firmware, const char** detail);
  void try_deferred_start();
  bool read32(uint32_t offset, uint32_t* value);
  bool write32(uint32_t offset, uint32_t value);
  bool write_cfg32(uint32_t offset, uint32_t value);
  bool write_fce32(uint32_t offset, uint32_t value);
  bool bulk_out(uint8_t endpoint, ConstByteView packet);
  bool mcu_command(uint8_t command, ConstByteView payload);
  bool mcu_u32_pair(uint8_t command, uint32_t first, uint32_t second);
  bool send_frame(uint8_t wcid, ConstByteView frame);
  bool write_beacon();
  bool set_led(uint32_t mode);
  bool write_station(uint8_t wcid, const MacAddress& address);
  void handle(const xbox_wireless::RxPacket& packet);
  void handle_management(const xbox_wireless::RxPacket& packet);
  void handle_gip(uint32_t slot, ConstByteView message);
  bool send_gip(uint32_t slot, ConstByteView message);
  void drop_client(uint32_t slot);
  int32_t slot_of(const MacAddress& address) const;
  bool gone() const { return host_.port_generation(port_) != generation_; }

  IUsbHostController& host_;
  uint32_t port_;
  uint32_t generation_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  ByteBuffer scratch_;              // one outgoing bulk packet (firmware chunks are the largest)
  uint8_t configuration_ = 0;
  uint8_t command_out_ = 0, data_out_ = 0;
  uint16_t command_out_max_ = 0, data_out_max_ = 0;
  uint8_t in_endpoints_[2] = {};
  uint16_t in_max_[2] = {};
  uint32_t in_count_ = 0;
  bool running_ = false;
  bool deferred_ = false, refused_ = false;
  const char* name_ = "Xbox Wireless Adapter";
  bool pairing_ = false;
  uint8_t channel_ = 0;
  uint32_t channel_dwell_us_ = 10000;
  uint8_t mcu_sequence_ = 1;
  uint16_t beacon_sequence_ = 0;
  MacAddress address_{};
  Client clients_[kMaxClients];
};

// Takes up the adapter on a root port: one pad per client slot into `out` (at most out.size(), normally kMaxClients); returns how many, 0 when
// the device is not an adapter or could not be started (said through the environment's log, with the reason). The first form uses the
// environment of set_xbox_wireless_environment(); while that has no firmware source yet, the pads are made at once and the adapter starts
// when one is set (start_when_firmware_is_there). The second form takes its firmware and log from the arguments and refuses without them.
uint32_t open_xbox_wireless_adapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, ArrayView<UniquePointer<IGameController>> out,
                                    XboxWirelessStatus* status = nullptr, SharedPointer<XboxWirelessAdapter>* adapter = nullptr);
uint32_t open_xbox_wireless_adapter(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, const XboxWirelessEnvironment& environment,
                                    ArrayView<UniquePointer<IGameController>> out, XboxWirelessStatus* status = nullptr,
                                    SharedPointer<XboxWirelessAdapter>* adapter = nullptr);

// One client slot of an adapter as a game controller.
class XboxWirelessPad final : public IGameController {
 public:
  XboxWirelessPad(SharedPointer<XboxWirelessAdapter> adapter, uint32_t slot, uint16_t vendor, uint16_t product);
  const char* name() const override { return info_.name; }
  bool initialize() override { return true; }
  void shutdown() override;
  const gamepad::Info& info() const override { return info_; }
  bool poll() override;
  const gamepad::State& state() const override { return state_; }
  [[nodiscard]] bool set_output(const gamepad::Output& output) override;
  bool present() const override { return adapter_->present(); }
  bool connected() const override { return adapter_->present() && adapter_->client_connected(slot_); }
  uint32_t slot() const { return slot_; }

 private:
  SharedPointer<XboxWirelessAdapter> adapter_;
  uint32_t slot_;
  uint32_t seen_generation_ = 0;
  gamepad::Info info_;
  gamepad::State state_;
  gamepad::Output output_;
};

}  // namespace hardware

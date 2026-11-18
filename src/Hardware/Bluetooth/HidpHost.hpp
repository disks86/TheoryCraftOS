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

// The classic Bluetooth HID host (HID Profile 1.1, HIDP): game controllers (DualShock 4, DualSense, Switch Pro, classic HID pads), keyboards and mice on BR/EDR links. A profile
// for the Bluetooth manager and, like HidOverGatt, a source of input devices for SystemInput. Linux's net/bluetooth/hidp is the model.
//
//   attach():   asks the device's SDP server for its HID record (the report map, HIDDescriptorList) and its PnP Information record (vendor and product id), and decides with
//               is_bluetooth_gamepad() whether it is a pad. The HIDP channels are then opened: control (PSM 0x11) before interrupt (PSM 0x13) when we connected; a bonded device
//               that reconnected opens them itself, and those are taken from the adapter (incoming_psms), or opened by us if it has not within 3 s.
//   session:    with both channels open a pad gets a HidPadChannel (its link is this host) and goes to the game controller service; any other device has its keyboard, consumer
//               (media keys) and pointer reports decoded into a BluetoothHidKeyboard / BluetoothHidMouse that SystemInput takes up. Input reports are DATA|Input (0xA1) on the
//               interrupt channel, the report id after the header when the map numbers its reports.
//   output:     DATA|Output (0xA2) on the interrupt channel; GET_REPORT (feature) and SET_REPORT on the control channel, one transaction at a time, each answered by DATA|Feature
//               (0xA3) or a HANDSHAKE within 1 s (10 polls). HID_CONTROL VIRTUAL_CABLE_UNPLUG from the device deletes its bond and disconnects it.
//   detach():   the pad's channel is detached and the keyboard and mouse leave the input system with their keys released.
//
// Threads: everything that touches L2CAP runs on the adapter's thread (attach, detach, service, the channel callbacks). A pad calls send_output() and get_feature() from the game
// controller service's thread or a program's: output is queued and sent by service(); get_feature() queues the request and waits up to kFeatureWaitMs for the adapter's thread to
// complete it. SystemInput's thread calls poll(). Every piece of state shared between them is behind a lock of its own, never held while calling L2CAP or a pad.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bluetooth/HidOverGatt.hpp"
#include "Hardware/Bluetooth/IBluetooth.hpp"
#include "Hardware/GameController/Bluetooth/BluetoothGamepad.hpp"
#include "Hardware/GameController/Bluetooth/HidPadChannel.hpp"
#include "Hardware/Hid/HidDescriptor.hpp"
#include "Hardware/InputSource.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class GameControllerService;

namespace hidp {
constexpr uint16_t kPsmControl = 0x11, kPsmInterrupt = 0x13;
constexpr uint8_t kHandshake = 0x00, kHidControl = 0x10, kGetReport = 0x40, kSetReport = 0x50, kData = 0xA0;
constexpr uint8_t kReportInput = 1, kReportOutput = 2, kReportFeature = 3;
constexpr uint8_t kControlVirtualCableUnplug = 5;
constexpr uint8_t kHandshakeSuccessful = 0;
}  // namespace hidp

// A device's reports as other threads see them: what they queue for the adapter's thread, and the feature report it brings back. The IHidReportLink of a pad's channel.
class HidpReportLink final : public IHidReportLink {
 public:
  static constexpr uint32_t kQueue = 8, kMaxReport = 128, kFeatureWaitMs = 1000;
  enum class Control : uint8_t { None, GetFeature, SetReport };

  // IHidReportLink (any thread)
  bool send_output(ConstByteView report) override;
  bool get_feature(uint8_t id, ByteView out, uint32_t* length) override;
  // A SET_REPORT (`type` 1 to 3, the report with its id first when the device numbers them): queued; one may wait at a time. Any thread.
  [[nodiscard]] bool set_report(uint8_t type, ConstByteView report);
  uint32_t set_reports_failed() const;

  // --- The adapter's thread.
  // The oldest queued output report; false if none.
  [[nodiscard]] bool take_output(ByteView out, uint32_t* length);
  // The next control transaction to send when none is in flight (it is then in flight): its header byte and payload. None if there is nothing to send.
  Control next_control(uint8_t* header, ByteView out, uint32_t* length);
  bool control_in_flight() const;
  // The answer to the transaction in flight (DATA|Feature with the report, or a HANDSHAKE: ok false unless successful for a SET_REPORT), or false to give it up (timed out).
  void control_answer(bool ok, ConstByteView data);
  // A HANDSHAKE on the control channel: ends the transaction in flight (a feature read fails; a SET_REPORT fails unless `result` is 0).
  void handshake(uint8_t result);
  // The device is going: what is queued is dropped, a waiting get_feature() returns false at once, and new calls fail.
  void close();

 private:
  enum class Feature : uint8_t { Idle, Queued, Sent, Done, Failed };
  struct Slot { uint8_t data[kMaxReport]; uint32_t length; };
  mutable kernel::Mutex mutex_;
  bool closing_ = false;
  Slot outputs_[kQueue] = {};
  uint32_t head_ = 0, count_ = 0;
  Feature feature_ = Feature::Idle;
  uint8_t feature_id_ = 0;
  uint32_t feature_generation_ = 0;
  uint8_t feature_data_[kMaxReport] = {};
  uint32_t feature_length_ = 0;
  bool set_pending_ = false;
  uint8_t set_type_ = 0;
  uint8_t set_data_[kMaxReport] = {};
  uint32_t set_length_ = 0, set_failed_ = 0;
  Control in_flight_ = Control::None;
};

class HidpHost final : public IBluetoothProfile, public IInputDeviceSource {
 public:
  static constexpr uint32_t kMaxDevices = 4, kMaxMap = 2048, kMaxRoutes = 6;
  static constexpr uint32_t kOpenTicks = 100, kIncomingWaitTicks = 30, kControlTicks = 10;    // (polls of the adapter: about 100 ms each)

  explicit HidpHost(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~HidpHost();
  HidpHost(const HidpHost&) = delete;
  HidpHost& operator=(const HidpHost&) = delete;

  // Where game controllers go (null: they are not served). Set before devices connect; the service must outlive this host.
  void set_game_controllers(GameControllerService* service) { game_controllers_ = service; }

  // IBluetoothProfile
  const char* name() const override { return "HID"; }
  bool matches(const BluetoothDeviceInfo& info, const GattDatabase& database) const override;
  bool attach(IBluetoothRemote& remote) override;
  void detach(IBluetoothRemote& remote) override;
  void service(IBluetoothRemote& remote) override;
  uint32_t incoming_psms(ArrayView<uint16_t> out) const override;

  // IInputDeviceSource
  uint32_t poll(IInputSink& sink) override;
  void detach_all(IInputSink& sink) override;

  // The report link of a connected device (by remote id), for SET_REPORT and the like; null if there is none. Valid until the device detaches (the adapter's thread).
  HidpReportLink* link(uint32_t remote_id);
  // The same for the index-th device slot (logs and tests); null for an empty one.
  HidpReportLink* link_at(uint32_t index);
  // Counters for logs and tests (the adapter's thread).
  uint32_t sessions() const { return sessions_; }
  uint32_t pads() const { return pads_; }
  uint32_t input_reports() const { return input_reports_; }
  uint32_t malformed() const { return malformed_; }
  uint32_t unplugged() const { return unplugged_; }

 private:
  struct Device;
  struct InputRecord {
    bool used = false, ready = false, gone = false, keyboard_added = false, mouse_added = false, announced = false;
    char label[40] = {};
    UniquePointer<BluetoothHidKeyboard> keyboard;
    UniquePointer<BluetoothHidMouse> mouse;
    void clear() { used = ready = gone = keyboard_added = mouse_added = announced = false; label[0] = 0; keyboard.reset(); mouse.reset(); }
  };
  Device* device_for(uint32_t remote_id);
  bool read_records(IBluetoothClassicRemote& classic, Device& device);
  bool plan_input(Device& device);
  void start_session(Device& device);
  void end_session(Device& device, bool device_left);
  void close_channels(Device& device);
  void on_control(Device& device, ConstByteView sdu);
  void on_interrupt(Device& device, ConstByteView sdu);
  friend struct Device;

  allocators::IAllocator& allocator_;
  GameControllerService* game_controllers_ = nullptr;
  UniquePointer<Device> devices_[kMaxDevices];          // (the adapter's thread only)
  kernel::Mutex mutex_;                                  // the input records (the adapter's thread and SystemInput's)
  InputRecord records_[kMaxDevices];
  uint32_t sessions_ = 0, pads_ = 0, input_reports_ = 0, malformed_ = 0, unplugged_ = 0;
};

}  // namespace hardware

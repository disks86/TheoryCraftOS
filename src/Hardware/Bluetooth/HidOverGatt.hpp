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

// HID over GATT (the HID service 0x1812, spec "HID Over GATT Profile"): Bluetooth LE keyboards and mice. A profile for the Bluetooth manager and, at the same time, a source of input
// devices for SystemInput - the one place the two worlds meet, so neither knows the other.
//
//   attach():   reads the Report Map (the HID report descriptor), takes each Report characteristic with its Report Reference (id and type) and finds the input reports of keyboards
//               and pointing devices in the descriptor; a device without a usable map but with the boot keyboard or boot mouse input characteristic is switched to the boot protocol
//               and read through its fixed layout. The input reports' notifications are switched on.
//   reports:    decoded where they arrive (hid::KeyboardDecoder / PointerDecoder) and queued for the IKeyboard / IMouse that SystemInput polls. A keyboard report is a whole state, so a
//               lost notification costs a key's transition only if two arrive between polls, and then the latest state still wins.
//   detach():   the device's keys and buttons are released before the device leaves the input system.
//
// Game controllers (Xbox pads over LE, 8BitDo, Stadia and other HID gamepads: is_bluetooth_gamepad() on the device's ids from its Device
// Information service and its report map) do not become keyboards or mice: their input reports go to a HidPadChannel and the pad
// (make_bluetooth_gamepad) to the game controller service, when one is set. Their output reports (rumble) are queued by the pad's thread and
// written from the adapter's thread (service()), since a remote is only used from there.
//
// Consumer control keys (volume, mute, play/pause, next, previous, stop, brightness) are translated to the codes of key:: and travel with the keyboard's; other consumer usages and other
// report kinds are not.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bluetooth/IBluetooth.hpp"
#include "Hardware/GameController/Bluetooth/HidPadChannel.hpp"
#include "Utility/SharedPointer.hpp"
#include "Hardware/Hid/HidDescriptor.hpp"
#include "Hardware/InputSource.hpp"
#include "Hardware/Keyboard/KeyboardBase.hpp"
#include "Hardware/Mouse/MouseBase.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class BluetoothHidKeyboard final : public KeyboardBase {
 public:
  BluetoothHidKeyboard() = default;
  const char* name() const override { return "Bluetooth keyboard"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  // Called with a decoded report from the notification path; any thread.
  void deliver(const hid::KeyboardState& state);
  // The media keys (volume, play/pause ...) come in reports of their own; they are held together with the keys of the keyboard report, whose state is kept for that.
  void deliver_consumer(const hid::KeyboardState& state);

 private:
  void fetch() override;
  void on_release_all() override;
  static constexpr uint32_t kRing = 16;
  kernel::Mutex mutex_;
  hid::KeyboardState ring_[kRing];
  uint32_t head_ = 0, count_ = 0;
  hid::KeyboardState typed_, media_;      // the latest keyboard report and the latest consumer report
  void queue(const hid::KeyboardState& combined);
  hid::KeyboardDiff diff_;
};

class BluetoothHidMouse final : public MouseBase {
 public:
  BluetoothHidMouse() = default;
  const char* name() const override { return "Bluetooth mouse"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool is_absolute() const override { return absolute_; }
  uint32_t max_x() const override { return max_x_; }
  uint32_t max_y() const override { return max_y_; }
  void configure(bool absolute, uint32_t max_x, uint32_t max_y) { absolute_ = absolute; max_x_ = max_x; max_y_ = max_y; }
  void deliver(const MouseEvent& event);

 private:
  void fetch() override;
  void on_release_all() override;
  static constexpr uint32_t kRing = 32;
  kernel::Mutex mutex_;
  MouseEvent ring_[kRing] = {};
  uint32_t head_ = 0, count_ = 0;
  MouseEvent last_ = {};
  bool absolute_ = false;
  uint32_t max_x_ = 0, max_y_ = 0;
};

class GameControllerService;

// A pad's output reports over GATT: queued from any thread, written on the adapter's thread by flush(). The latest report of each id wins.
class GattPadLink final : public IHidReportLink {
 public:
  static constexpr uint32_t kMaxOutputs = 4, kMaxOutput = 64;
  // Output Report characteristic `characteristic` carries report `id`.
  bool add_output(uint8_t id, int characteristic, bool without_response);
  bool send_output(ConstByteView report) override;
  bool get_feature(uint8_t id, ByteView out, uint32_t* length) override { (void)id; (void)out; *length = 0; return false; }   // (none of the LE pads needs one)
  // Writes what is queued (the adapter's thread).
  void flush(IBluetoothRemote& remote);

 private:
  struct Output {
    uint8_t id = 0;
    int characteristic = -1;
    bool without_response = false;
    bool pending = false;
    uint8_t data[kMaxOutput] = {};
    uint32_t length = 0;
  };
  kernel::Mutex mutex_;
  Output outputs_[kMaxOutputs];
  uint32_t count_ = 0;
};

class HidOverGatt final : public IBluetoothProfile, public IInputDeviceSource, private IBluetoothNotificationSink {
 public:
  static constexpr uint32_t kMaxDevices = 4, kMaxReports = 6, kMaxMap = 1024;
  explicit HidOverGatt(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~HidOverGatt() = default;
  HidOverGatt(const HidOverGatt&) = delete;
  HidOverGatt& operator=(const HidOverGatt&) = delete;

  // IBluetoothProfile
  const char* name() const override { return "HID over GATT"; }
  bool matches(const BluetoothDeviceInfo& info, const GattDatabase& database) const override;
  bool attach(IBluetoothRemote& remote) override;
  void detach(IBluetoothRemote& remote) override;
  void service(IBluetoothRemote& remote) override;   // (the adapter's thread: writes the pads' queued output)

  // Where game controllers go (null: they are not taken). Set before devices connect; the service must outlive this profile.
  void set_game_controllers(GameControllerService* service) { game_controllers_ = service; }

  // IInputDeviceSource
  uint32_t poll(IInputSink& sink) override;
  void detach_all(IInputSink& sink) override;

  uint32_t device_count() const;

 private:
  struct Route {
    uint16_t value_handle = 0;
    bool keyboard = false, consumer = false, pad = false;
    uint8_t report_id = 0;                  // (pad: put in front of the notification's value when the map numbers its reports)
    hid::KeyboardDecoder keyboard_decoder;
    hid::ConsumerDecoder consumer_decoder;
    hid::PointerDecoder pointer_decoder;
  };
  struct Record {
    bool used = false, ready = false, gone = false, keyboard_added = false, mouse_added = false, announced = false;
    uint32_t remote_id = 0;
    char label[40] = {};
    UniquePointer<BluetoothHidKeyboard> keyboard;
    UniquePointer<BluetoothHidMouse> mouse;
    Route routes[kMaxReports];
    uint32_t route_count = 0;
    SharedPointer<HidPadChannel> channel;   // a game controller's
    SharedPointer<GattPadLink> link;        // (shared: service() keeps its own reference while it writes)
    void clear() {
      used = ready = gone = keyboard_added = mouse_added = announced = false;
      remote_id = 0;
      label[0] = 0;
      keyboard.reset();
      mouse.reset();
      route_count = 0;
      if (channel) channel->detach();
      channel = SharedPointer<HidPadChannel>();
      link = SharedPointer<GattPadLink>();
    }
  };
  void on_notification(IBluetoothRemote& remote, uint16_t value_handle, ConstByteView value, bool indication) override;
  bool attach_reports(IBluetoothRemote& remote, Record& record);
  bool attach_pad(IBluetoothRemote& remote, Record& record, ConstByteView descriptor, const hid::ReportMap& map);
  bool attach_boot(IBluetoothRemote& remote, Record& record);
  Record* record_for(uint32_t remote_id);

  allocators::IAllocator& allocator_;
  GameControllerService* game_controllers_ = nullptr;
  kernel::Mutex mutex_;
  Record records_[kMaxDevices];
};

}  // namespace hardware

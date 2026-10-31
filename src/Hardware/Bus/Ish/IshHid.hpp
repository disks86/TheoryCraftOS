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

// The HID client of the ISH firmware (ishtp-hid-client of Linux): the sensor hub presents each of its sensors as a HID device. Over an ISHTP connection to the client with the GUID
// 33AECD58-B679-4E54-9BD9-A04D34F0C226 the host sends HOSTIF commands (a 6 byte header: command with the response bit, device id, status, flags, payload size): enumerate the devices
// (HOSTIF_DM_ENUM_DEVICES), get each one's HID descriptor and report descriptor, set feature reports (HOSTIF_SET_FEATURE_REPORT, the report with its id after the header), and receive the
// input reports (HOSTIF_PUBLISH_INPUT_REPORT: one report; _LIST: several, each with its own header). Every device is an IHidFunctionHost, so HidSensor reads it like a device on I2C.
#pragma once
#include "Hardware/Bus/Ish/Ishtp.hpp"
#include "Hardware/Hid/HidFunctionHost.hpp"
#include "Hardware/Sensor/HidSensor.hpp"
#include "Hardware/Sensor/SensorHub.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

class IshHid;

class IshHidDevice final : public IHidFunctionHost {
 public:
  static constexpr uint32_t kMaxClients = 4;
  IshHidDevice(IshHid& owner, allocators::IAllocator& allocator, uint32_t id, uint8_t device_class, uint16_t vendor, uint16_t product)
      : owner_(owner), id_(id), class_(device_class), vendor_(vendor), product_(product), report_descriptor_(allocator), map_(allocator) {}

  uint32_t id() const { return id_; }
  uint8_t device_class() const { return class_; }
  uint16_t vendor() const { return vendor_; }
  uint16_t product() const { return product_; }
  ConstByteView report_descriptor() const { return report_descriptor_.view(); }

  const HidInputMap& map() const override { return map_; }
  bool add_client(II2cHidClient* client) override;
  void remove_client(II2cHidClient* client) override;
  void pump() override;
  bool set_feature(uint8_t id, ConstByteView report) override;

 private:
  friend class IshHid;
  void deliver(ConstByteView report);
  IshHid& owner_;
  uint32_t id_;
  uint8_t class_;
  uint16_t vendor_, product_;
  ByteBuffer report_descriptor_;
  HidInputMap map_;
  bool parsed_ = false;
  II2cHidClient* clients_[kMaxClients] = {};
};

class IshHid {
 public:
  static constexpr uint32_t kMaxDevices = 16, kMaxSensors = 24;
  IshHid(allocators::IAllocator& allocator, IshtpBus& bus) : allocator_(allocator), bus_(bus), devices_(allocator), sensors_(allocator), answer_(allocator) {}
  ~IshHid() { stop(); }

  // Connects to the client, enumerates the devices, reads their descriptors and makes the sensors they describe, added to `hub` (may be nullptr). False if the firmware has no such client
  // or does not answer.
  bool start(SensorHub* hub);
  // Takes the sensors out of the hub and lets go of the connection.
  void stop();
  // Reads what the firmware sent and hands the input reports to the devices' clients. After a firmware reset the client is started again and the sensors are switched on again.
  void poll();
  uint32_t device_count() const { return static_cast<uint32_t>(devices_.count()); }
  IshHidDevice* device(uint32_t i) { return i < devices_.count() ? devices_[i].get() : nullptr; }
  uint32_t sensor_count() const { return static_cast<uint32_t>(sensors_.count()); }
  bool connected() const { return connection_ && connection_->connected; }

 private:
  friend class IshHidDevice;
  struct SensorEntry {
    char name[48] = {};
    UniquePointer<HidSensor> sensor;
  };
  static constexpr uint32_t kTimeoutPolls = 6000, kPollUs = 500;     // 3 s per request
  bool connect();
  bool enumerate(bool first);
  bool request(uint8_t command, uint8_t device_id, ConstByteView payload);
  bool wait(const bool* flag);
  void dispatch(ConstByteView message);
  void recover();
  bool send_feature(uint32_t device_id, uint8_t report_id, ConstByteView report);
  IshHidDevice* find_device(uint8_t device_id);
  void poll_locked();
  void make_sensors();

  allocators::IAllocator& allocator_;
  IshtpBus& bus_;
  kernel::Mutex mutex_;
  IshtpConnection* connection_ = nullptr;
  List<UniquePointer<IshHidDevice>> devices_;
  List<UniquePointer<SensorEntry>> sensors_;
  SensorHub* hub_ = nullptr;
  // answers being waited for
  bool enum_answered_ = false, descriptor_answered_ = false;
  uint8_t waiting_command_ = 0, waiting_device_ = 0;
  uint32_t enum_count_ = 0;
  struct Info { uint32_t id; uint8_t device_class; uint16_t product, vendor; } info_[kMaxDevices] = {};
  ByteBuffer answer_;
  bool in_start_ = false, reenable_pending_ = false;
};

}  // namespace hardware

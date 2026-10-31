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

#include "Hardware/Bus/Ish/IshHid.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// 33AECD58-B679-4E54-9BD9-A04D34F0C226 as the firmware sends it
const uint8_t kHidClientGuid[16] = {0x58, 0xCD, 0xAE, 0x33, 0x79, 0xB6, 0x54, 0x4E, 0x9B, 0xD9, 0xA0, 0x4D, 0x34, 0xF0, 0xC2, 0x26};
constexpr uint8_t kGetHidDescriptor = 0, kGetReportDescriptor = 1, kSetFeatureReport = 3, kPublishInput = 5, kPublishInputList = 6, kEnumDevices = 33;
constexpr uint32_t kHeaderBytes = 6, kDeviceInfoBytes = 9, kCommandMask = 0x7F;
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

// ---- a device -------------------------------------------------------------------------------------------------------------------------------------------------------------------

bool IshHidDevice::add_client(II2cHidClient* client) {
  if (!client) return false;
  for (II2cHidClient*& slot : clients_)
    if (!slot) { slot = client; return true; }
  return false;
}

void IshHidDevice::remove_client(II2cHidClient* client) {
  for (II2cHidClient*& slot : clients_)
    if (slot == client) slot = nullptr;
}

void IshHidDevice::pump() { owner_.poll(); }

bool IshHidDevice::set_feature(uint8_t id, ConstByteView report) { return owner_.send_feature(id_, id, report); }

void IshHidDevice::deliver(ConstByteView report) {
  for (II2cHidClient* c : clients_)
    if (c) c->on_report(report);
}

// ---- the client -----------------------------------------------------------------------------------------------------------------------------------------------------------------

bool IshHid::connect() {
  const IshtpClientProperties* client = bus_.find_client(kHidClientGuid);
  if (!client) return false;
  connection_ = bus_.connect(*client);
  return connection_ != nullptr;
}

bool IshHid::request(uint8_t command, uint8_t device_id, ConstByteView payload) {
  if (!connection_ || !connection_->connected) return false;
  ByteBuffer message(allocator_);
  const uint16_t size = static_cast<uint16_t>(payload.size());
  const bool set = command == kSetFeatureReport;                  // (as the kernel driver sends it: the size field stays 0)
  if (!message.put8(command) || !message.put8(device_id) || !message.put8(0) || !message.put8(0) || !message.put16(set ? 0 : size) || !message.append(payload)) return false;
  return bus_.send(*connection_, message.view());
}

bool IshHid::wait(const bool* flag) {
  for (uint32_t i = 0; i < kTimeoutPolls; ++i) {
    poll_locked();
    if (*flag) return true;
    delay_us(kPollUs);
  }
  return false;
}

IshHidDevice* IshHid::find_device(uint8_t device_id) {
  for (uint64_t i = 0; i < devices_.count(); ++i)
    if (static_cast<uint8_t>(devices_[i]->id()) == device_id) return devices_[i].get();
  return nullptr;
}

bool IshHid::enumerate(bool first) {
  enum_count_ = 0;
  bool ok = false;
  for (uint32_t attempt = 0; attempt < 3 && !ok; ++attempt) {
    enum_answered_ = false;
    ok = request(kEnumDevices, 0, ConstByteView()) && wait(&enum_answered_);
  }
  if (!ok || !first) return ok;
  for (uint32_t i = 0; i < enum_count_; ++i) {
    UniquePointer<IshHidDevice> d = UniquePointer<IshHidDevice>::make(allocator_, *this, allocator_, info_[i].id, info_[i].device_class, info_[i].vendor, info_[i].product);
    if (!d) return false;
    const uint8_t id = static_cast<uint8_t>(info_[i].id);
    // the HID descriptor (not needed, but the firmware expects the sequence) and the report descriptor
    waiting_device_ = id;
    waiting_command_ = kGetHidDescriptor;
    descriptor_answered_ = false;
    if (!request(kGetHidDescriptor, id, ConstByteView()) || !wait(&descriptor_answered_)) continue;
    waiting_command_ = kGetReportDescriptor;
    descriptor_answered_ = false;
    if (!request(kGetReportDescriptor, id, ConstByteView()) || !wait(&descriptor_answered_)) continue;
    waiting_command_ = 0xFF;
    if (!d->report_descriptor_.append(answer_.view()) || !d->map_.parse(d->report_descriptor_.view())) continue;   // (a device that cannot be read is left out)
    d->parsed_ = true;
    devices_.push_back(static_cast<UniquePointer<IshHidDevice>&&>(d));
  }
  return true;
}

// The sensors the devices describe. Called without the lock: switching a sensor on sends a feature report.
void IshHid::make_sensors() {
  for (uint64_t i = 0; i < devices_.count(); ++i) {
    IshHidDevice& d = *devices_[i];
    SensorKind kinds[HidSensor::kMaxPerDevice];
    uint8_t ids[HidSensor::kMaxPerDevice];
    const uint32_t found = HidSensor::discover(d.map(), kinds, ids);
    for (uint32_t k = 0; k < found && sensors_.count() < kMaxSensors; ++k) {
      UniquePointer<SensorEntry> e = UniquePointer<SensorEntry>::make(allocator_);
      if (!e) continue;
      const char prefix[] = "ISH ";
      uint32_t n = 0;
      for (const char* c = prefix; *c && n < sizeof e->name - 1; ++c) e->name[n++] = *c;
      for (const char* c = sensor_kind_name(kinds[k]); *c && n < sizeof e->name - 1; ++c) e->name[n++] = *c;
      e->sensor = UniquePointer<HidSensor>::make(allocator_, d, kinds[k], ids[k], e->name);
      if (!e->sensor || !e->sensor->initialize()) continue;
      if (hub_) hub_->add(e->sensor.get());
      sensors_.push_back(static_cast<UniquePointer<SensorEntry>&&>(e));
    }
  }
}

bool IshHid::start(SensorHub* hub) {
  bool ok;
  {
    kernel::ScopedMutex guard(mutex_);
    hub_ = hub;
    devices_.clear();
    sensors_.clear();
    in_start_ = true;
    ok = connect() && enumerate(true);
    in_start_ = false;
  }
  if (ok) make_sensors();
  return ok;
}

void IshHid::stop() {
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = 0; i < sensors_.count(); ++i) {
    if (hub_) hub_->remove(sensors_[i]->sensor.get());
    sensors_[i]->sensor->shutdown();
  }
  sensors_.clear();
  devices_.clear();
  connection_ = nullptr;
}

bool IshHid::send_feature(uint32_t device_id, uint8_t report_id, ConstByteView report) {
  kernel::ScopedMutex guard(mutex_);
  ByteBuffer body(allocator_);
  if (report_id != 0 && !body.put8(report_id)) return false;
  if (!body.append(report)) return false;
  return request(kSetFeatureReport, static_cast<uint8_t>(device_id), body.view());
}

void IshHid::dispatch(ConstByteView message) {
  uint64_t at = 0;
  while (at + kHeaderBytes <= message.size()) {
    const uint8_t command = message[at] & kCommandMask, device = message[at + 1];
    const uint64_t size = le16(message.data() + at + 4);
    if (at + kHeaderBytes + size > message.size()) return;              // a message that claims more than came
    const ConstByteView payload = message.subview(at + kHeaderBytes, size);
    at += kHeaderBytes + size;
    switch (command) {
      case kEnumDevices:
        if (!payload.empty()) {
          const uint32_t count = payload[0];
          enum_count_ = 0;
          for (uint32_t i = 0; i < count && i < kMaxDevices; ++i) {
            const uint64_t off = 1 + uint64_t{kDeviceInfoBytes} * i;
            if (off + kDeviceInfoBytes > payload.size()) break;
            const uint8_t* p = payload.data() + off;
            info_[enum_count_].id = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
            info_[enum_count_].device_class = p[4];
            info_[enum_count_].product = le16(p + 5);
            info_[enum_count_].vendor = le16(p + 7);
            ++enum_count_;
          }
          enum_answered_ = true;
        }
        break;
      case kGetHidDescriptor:
      case kGetReportDescriptor:
        if (in_start_ && command == waiting_command_ && device == waiting_device_) {
          answer_.clear();
          answer_.append(payload);
          descriptor_answered_ = true;
        }
        break;
      case kPublishInput:
        if (IshHidDevice* d = find_device(device)) d->deliver(payload);
        break;
      case kPublishInputList: {
        // total size (2), number of reports (1), flags (1), then per report: size (2, the header and the payload), the header (6), the payload
        if (payload.size() < 4) break;
        const uint32_t count = payload[2];
        uint64_t p = 4;
        for (uint32_t i = 0; i < count && p + 2 + kHeaderBytes <= payload.size(); ++i) {
          const uint64_t entry = le16(payload.data() + p);
          if (entry < kHeaderBytes || p + 2 + entry > payload.size()) break;
          const uint8_t entry_device = payload[p + 2 + 1];
          if (IshHidDevice* d = find_device(entry_device)) d->deliver(payload.subview(p + 2 + kHeaderBytes, entry - kHeaderBytes));
          p += 2 + entry;
        }
        break;
      }
      default: break;                                                    // (feature report answers and the like)
    }
  }
}

void IshHid::poll_locked() {
  bus_.poll();
  if (bus_.take_reset()) recover();
  if (!connection_) return;
  ByteBuffer message(allocator_);
  while (bus_.take(*connection_, &message)) dispatch(message.view());
}

void IshHid::recover() {
  // the firmware reset itself: the bus and the connection are started again, and every sensor is told again to report
  connection_ = nullptr;
  if (in_start_ || !bus_.start() || !connect()) return;
  in_start_ = true;                                                      // (the waits of enumerate() poll in here)
  const bool ok = enumerate(false);
  in_start_ = false;
  if (!ok) return;
  reenable_pending_ = true;
}

void IshHid::poll() {
  bool again = false;
  {
    kernel::ScopedMutex guard(mutex_);
    poll_locked();
    again = reenable_pending_;
    reenable_pending_ = false;
  }
  if (again)                                                             // (outside the lock: it sends feature reports)
    for (uint64_t i = 0; i < sensors_.count(); ++i) sensors_[i]->sensor->reenable();
}

}  // namespace hardware

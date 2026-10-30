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

#include "Hardware/Bluetooth/BluetoothService.hpp"

namespace hardware {

namespace {
void set_message(char (&out)[96], const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < sizeof out; ++n) out[n] = text[n];
  out[n] = 0;
}
}

BluetoothService::BluetoothService(IUsbBus* usb, IBluetoothFirmwareSource* firmware, allocators::IAllocator& allocator, crypto::IRandom* random, IBluetoothBondStorage* bonds)
    : usb_(usb), firmware_(firmware), allocator_(allocator), random_(random), bond_storage_(bonds), entries_(allocator) {}

bool BluetoothService::save_bonds() {
  if (!bond_storage_) return false;
  ByteBuffer bytes(allocator_);
  return bonds_.serialize(bytes) && bond_storage_->save(bytes.view());
}

void BluetoothService::start() {
  if (bond_storage_) {
    ByteBuffer bytes(allocator_);
    if (bond_storage_->load(bytes)) bonds_.parse(bytes.view());
  }
  if (!usb_) return;
  // Every port counts as new once: what is plugged in now is taken, and the watcher then reports only what changes.
  for (uint32_t c = 0; c < usb_->controller_count(); ++c) {
    IUsbHostController* host = usb_->host_controller(c);
    for (uint32_t p = 0; host && p < host->port_count(); ++p) {
      UsbDeviceDescriptor d;
      if (host->device_descriptor(p, &d)) open(c, *host, p);
    }
  }
  watcher_.attach(usb_);
}

void BluetoothService::step(bool scan) {
  for (uint32_t i = 0; i < entries_.count(); ++i)
    if (entries_[i]->state == BluetoothState::Ready) entries_[i]->controller->poll();
  if (!scan) return;
  drop_gone();
  scan_ports();
}

void BluetoothService::scan_ports() {
  watcher_.scan([this](uint32_t c, IUsbHostController& host, uint32_t port) { open(c, host, port); });
}

void BluetoothService::drop_gone() {
  for (uint32_t i = 0; i < entries_.count();) {
    if (entries_[i]->transport && !entries_[i]->transport->present()) {
      entries_.remove_at(i);
      ++removed_;
    } else {
      ++i;
    }
  }
}

uint32_t BluetoothService::ready_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < entries_.count(); ++i) n += entries_[i]->state == BluetoothState::Ready;
  return n;
}

void BluetoothService::open(uint32_t bus_controller, IUsbHostController& host, uint32_t port) {
  UsbDeviceDescriptor descriptor;
  uint8_t blob[1024];
  uint16_t length = 0;
  UsbBluetoothInterface interface;
  if (!host.device_descriptor(port, &descriptor) || !host.configuration_descriptor(port, ByteView(blob), &length) || !find_bluetooth_interface(ConstByteView(blob).at_most(length), &interface)) return;
  UniquePointer<Entry> entry = UniquePointer<Entry>::make(allocator_);
  if (!entry) return;
  entry->bus_controller = bus_controller;
  entry->port = port;
  entry->generation = host.port_generation(port);
  entry->transport = UsbHciTransport::open(host, port, allocator_);
  if (!entry->transport) {
    set_message(entry->message, "the USB interface did not start");
    entries_.push_back(util::move(entry));
    ++added_;
    return;
  }
  entry->controller = UniquePointer<HciController>::make(allocator_, *entry->transport, allocator_);
  if (!entry->controller) return;
  if (bluetooth_vendor(descriptor.vendor_id, descriptor.product_id) == BluetoothVendor::Intel) {
    entry->intel = UniquePointer<IntelBluetooth>::make(allocator_, *entry->controller);
    const IntelSetup identified = entry->intel ? entry->intel->identify() : IntelSetup::Failed;
    if (identified == IntelSetup::NeedFirmware) {
      const char* name = entry->intel->firmware_name();
      uint32_t n = 0;
      for (; name[n] && n + 1 < sizeof entry->firmware; ++n) entry->firmware[n] = name[n];
      entry->firmware[n] = 0;
      ByteBuffer image(allocator_);
      if (!firmware_ || !firmware_->read(name, image)) {
        entry->state = BluetoothState::FirmwareMissing;
        set_message(entry->message, "the controller needs its firmware file in /firmware");
        entries_.push_back(util::move(entry));
        ++added_;
        return;
      }
      const IntelSetup loaded = entry->intel->load_firmware(image.view());
      if (loaded != IntelSetup::Ok) {
        entry->state = BluetoothState::Failed;
        set_message(entry->message, loaded == IntelSetup::BadFirmware ? "the firmware file is not valid" : loaded == IntelSetup::Timeout ? "the controller did not take the firmware" : "loading the firmware failed");
        entries_.push_back(util::move(entry));
        ++added_;
        return;
      }
    } else if (identified != IntelSetup::AlreadyRunning) {
      entry->state = BluetoothState::Failed;
      set_message(entry->message, identified == IntelSetup::UnsupportedVariant ? "an Intel controller this driver does not know" : "the controller did not answer");
      entries_.push_back(util::move(entry));
      ++added_;
      return;
    }
  }
  const HciResult started = entry->controller->initialize();
  entry->state = started == HciResult::Ok ? BluetoothState::Ready : BluetoothState::Failed;
  if (started == HciResult::Ok) {
    entry->l2cap = UniquePointer<L2cap>::make(allocator_, *entry->controller);
    if (entry->l2cap) entry->att = UniquePointer<AttClient>::make(allocator_, *entry->controller, *entry->l2cap);
    if (entry->att) entry->gatt = UniquePointer<GattClient>::make(allocator_, *entry->att);
    if (entry->l2cap && random_) {
      entry->security = UniquePointer<SecurityManager>::make(allocator_, *entry->controller, *entry->l2cap, *random_, bonds_);
      if (entry->security && entry->att) entry->att->set_security_handler(&SecurityManager::att_security, entry->security.get());
    }
  }
  set_message(entry->message, started == HciResult::Ok ? "ready" : started == HciResult::Timeout ? "the controller did not answer" : "bringing the controller up failed");
  entries_.push_back(util::move(entry));
  ++added_;
}

}  // namespace hardware

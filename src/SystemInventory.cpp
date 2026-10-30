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

#include "SystemInventory.hpp"
#include "Hardware/WirelessInterface/IWirelessInterface.hpp"
#include "Desktop/Format.hpp"
#include "SystemInventoryNames.hpp"

using desktop::HardwareGroup;

using namespace system_inventory;

void SystemInventory::add(desktop::HardwareList* list, HardwareGroup group, StringView name, uint32_t id, bool wireless) { list->add(group, name, scratch_.view(), id, wireless); }

void SystemInventory::collect(desktop::HardwareList* list) {
  String name(allocator_);
  if (cpu_) {
    name.assign(cpu_->model()[0] ? cpu_->model() : cpu_->name());
    scratch_.assign(cpu_->name());
    scratch_.append(", ");
    scratch_.append_uint(cpu_->core_count());
    scratch_.append(cpu_->core_count() == 1 ? " core" : " cores");
    add(list, HardwareGroup::Processors, name.view());
  }
  if (ram_total_) {
    desktop::format_size(ram_total_, &name);
    name.append(" of memory");
    desktop::format_size(ram_usable_, &scratch_);
    scratch_.append(" in the allocator pool");
    add(list, HardwareGroup::Memory, name.view());
  }
  if (gpu_) {
    scratch_.assign("Screen ");
    scratch_.append_uint(gpu_->width());
    scratch_.push_back('x');
    scratch_.append_uint(gpu_->height());
    scratch_.append(", ");
    scratch_.append_uint(gpu_->mode_count());
    scratch_.append(" modes");
    add(list, HardwareGroup::DisplayAdapters, gpu_->name());
  }
  if (monitors_) {
    for (uint32_t i = 0; i < monitors_->count(); ++i) {
      const hardware::Monitor& m = monitors_->monitor(i);
      scratch_.assign(m.connector_name()[0] ? m.connector_name() : "display");
      if (m.connected()) {
        scratch_.append(", ");
        scratch_.append_uint(m.current_mode().width);
        scratch_.push_back('x');
        scratch_.append_uint(m.current_mode().height);
        if (m.width_mm()) {
          scratch_.append(", ");
          scratch_.append_uint(m.width_mm());
          scratch_.append(" x ");
          scratch_.append_uint(m.height_mm());
          scratch_.append(" mm");
        }
        const int32_t slot = monitors_->layout().find(m.id());
        if (slot >= 0 && monitors_->layout().at(static_cast<uint32_t>(slot)).primary) scratch_.append(", primary");
      } else {
        scratch_.append(", not connected");
      }
      add(list, HardwareGroup::Displays, m.name(), i);
    }
  }
  if (storage_) {
    for (uint32_t i = 0; i < storage_->count(); ++i) {
      hardware::IStorageDevice* d = storage_->at(i);
      if (!d) continue;
      const hardware::StorageInfo info = d->info();
      desktop::format_size(info.block_count * info.block_size, &scratch_);
      if (info.removable) scratch_.append(", removable");
      if (info.read_only) scratch_.append(", read-only");
      if (info.serial[0]) {
        scratch_.append(", serial ");
        scratch_.append(info.serial);
      }
      add(list, HardwareGroup::Storage, info.model[0] ? StringView(info.model) : StringView(d->name()), i);
    }
  }
  if (network_) {
    for (uint32_t i = 0; i < network_->count(); ++i)
      if (hardware::INetworkInterface* n = network_->at(i)) {
        scratch_.assign(n->link_up() ? "connected" : "no link");
        const uint32_t speed = n->link_speed_mbps();
        if (speed) {
          scratch_.append(", ");
          if (speed >= 1000 && speed % 1000 == 0) {
            scratch_.append_uint(speed / 1000);
            scratch_.append(" Gbps");
          } else {
            scratch_.append_uint(speed);
            scratch_.append(" Mbps");
          }
        }
        hardware::IWirelessInterface* radio = n->wireless();
        if (radio) {
          const hardware::WifiStatus wifi = radio->status();
          scratch_.assign("Wi-Fi, ");
          if (wifi.state == hardware::WifiState::Connected) {
            scratch_.append("connected to ");
            scratch_.append(wifi.network.ssid);
          } else {
            scratch_.append("not connected");
          }
        }
        add(list, HardwareGroup::Network, n->name(), i, radio != nullptr);
      }
  }
  if (audio_) {
    for (uint32_t i = 0; i < audio_->count(); ++i)
      if (hardware::IAudioDevice* d = audio_->at(i)) {
        scratch_.assign(audio_->is_hardware(i) ? "48 kHz 16-bit stereo" : "software device, plays to nowhere");
        add(list, HardwareGroup::Audio, d->name(), i);
      }
  }
  if (mpu_ && mpu_->present()) {
    scratch_.assign("MIDI output, port 0x");
    append_hex(&scratch_, mpu_->base(), 3);
    add(list, HardwareGroup::Midi, "MPU-401 MIDI port");
  }
  if (usb_midi_) {
    for (uint32_t i = 0; i < usb_midi_->device_count(); ++i) {
      hardware::UsbMidiService::DeviceInfo info;
      if (!usb_midi_->device_info(i, &info)) continue;
      scratch_.assign(info.input && info.output ? "keys and output" : info.input ? "keys" : "output");
      if (info.synthesizer) scratch_.append(", a synthesizer");
      add(list, HardwareGroup::Midi, info.name, 1 + i);
    }
  }
  if (input_) {
    for (uint32_t i = 0; i < input_->keyboards().count(); ++i)
      if (hardware::IKeyboard* k = input_->keyboards().at(i)) {
        scratch_.assign("keyboard");
        add(list, HardwareGroup::Input, k->name(), i);
      }
    for (uint32_t i = 0; i < input_->mice().count(); ++i)
      if (hardware::IMouse* m = input_->mice().at(i)) {
        scratch_.assign(m->is_absolute() ? "pointer (absolute)" : "mouse");
        add(list, HardwareGroup::Input, m->name(), kMouseIdBase + i);
      }
  }
  if (usb_) {
    for (uint32_t c = 0; c < usb_->controller_count(); ++c) {
      scratch_.assign("");
      scratch_.append_uint(usb_->port_count(c));
      scratch_.append(" ports");
      name.assign(usb_controller_name(usb_->controller_kind(c)));
      add(list, HardwareGroup::Usb, name.view(), (c << 16) | kUsbControllerPort);
      for (uint32_t p = 0; p < usb_->port_count(c); ++p) {
        hardware::UsbDeviceDescriptor d;
        if (!usb_->device_descriptor(c, p, &d)) continue;
        name.assign("USB device ");
        append_hex(&name, d.vendor_id, 4);
        name.push_back(':');
        append_hex(&name, d.product_id, 4);
        scratch_.assign("port ");
        scratch_.append_uint(p);
        scratch_.append(" of controller ");
        scratch_.append_uint(c);
        if (usb_class_name(d.device_class)[0]) {
          scratch_.append(", ");
          scratch_.append(usb_class_name(d.device_class));
        }
        add(list, HardwareGroup::Usb, name.view(), (c << 16) | p);
      }
    }
  }
  if (printers_) {
    for (uint32_t i = 0; i < printers_->count(); ++i)
      if (hardware::IPrinter* p = printers_->at(i)) {
        scratch_.assign("");
        add(list, HardwareGroup::Printers, p->name(), i);
      }
  }
  if (cameras_) {
    for (uint32_t i = 0; i < cameras_->count(); ++i)
      if (hardware::ICamera* c = cameras_->at(i)) {
        scratch_.assign("");
        add(list, HardwareGroup::Cameras, c->name(), i);
      }
  }
  if (pci_) {
    for (uint32_t i = 0; i < pci_->device_count(); ++i) {
      hardware::PciDeviceInfo d;
      if (!pci_->pci_device(i, &d)) continue;
      name.assign("PCI ");
      name.append(pci_class_name(d.class_code));
      name.append(" ");
      append_hex(&name, d.vendor_id, 4);
      name.push_back(':');
      append_hex(&name, d.device_id, 4);
      scratch_.assign("bus ");
      scratch_.append_uint(d.address.bus);
      scratch_.append(", device ");
      scratch_.append_uint(d.address.device);
      scratch_.append(", function ");
      scratch_.append_uint(d.address.function);
      add(list, HardwareGroup::System, name.view(), i);
    }
  }
}

bool SystemInventory::refresh() {
  desktop::HardwareList next(allocator_);
  collect(&next);
  if (next.same_as(current_)) return false;
  current_.clear();
  for (uint32_t i = 0; i < next.count(); ++i) current_.add(next.at(i).group, next.at(i).name, next.at(i).detail, next.at(i).id, next.at(i).wireless);
  return true;
}

bool SystemInventory::entry(uint32_t index, desktop::HardwareEntry* out) const {
  if (index >= current_.count()) return false;
  *out = current_.at(index);
  return true;
}

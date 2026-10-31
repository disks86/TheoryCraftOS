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

// SystemInventory::describe: the facts of one device for the Properties window. They are asked of the device through its class interface, so all the
// devices of a kind read the same, and the driver adds its own lines through IHardware::describe() (for the classes that are IHardware).
#include "SystemInventory.hpp"
#include "Desktop/Format.hpp"
#include "Hardware/WirelessInterface/IWirelessInterface.hpp"
#include "SystemInventoryNames.hpp"

using desktop::HardwareGroup;
using namespace system_inventory;

namespace {

class ListSink final : public hardware::IPropertySink {
 public:
  explicit ListSink(desktop::DevicePropertyList* list) : list_(list) {}
  void add(StringView name, StringView value) override { list_->add(name, value); }

 private:
  desktop::DevicePropertyList* list_;
};

void set_title(desktop::DevicePropertyList* out, StringView title) {
  uint32_t n = 0;
  for (; n < title.size() && n + 1 < sizeof out->title; ++n) out->title[n] = title[n];
  out->title[n] = 0;
}

void add_size(hardware::IPropertySink& sink, const char* name, uint64_t bytes, allocators::IAllocator& allocator) {
  String text(allocator);
  desktop::format_size(bytes, &text);
  sink.add(name, text.view());
}

// "2.0" for the BCD 0x0200.
void add_usb_version(hardware::IPropertySink& sink, const char* name, uint16_t bcd, allocators::IAllocator& allocator) {
  String text(allocator);
  text.append_uint(bcd >> 8);
  text.push_back('.');
  text.append_uint((bcd >> 4) & 15);
  if (bcd & 15) text.append_uint(bcd & 15);
  sink.add(name, text.view());
}

void add_ipv4(hardware::IPropertySink& sink, const char* name, uint32_t address, allocators::IAllocator& allocator) {
  if (!address) return;
  String text(allocator);
  for (int shift = 24; shift >= 0; shift -= 8) {
    text.append_uint((address >> shift) & 255);
    if (shift) text.push_back('.');
  }
  sink.add(name, text.view());
}

void add_mode(hardware::IPropertySink& sink, const char* name, const hardware::MonitorMode& m, allocators::IAllocator& allocator) {
  String text(allocator);
  text.append_uint(m.width);
  text.push_back('x');
  text.append_uint(m.height);
  if (m.refresh_millihz) {
    text.append(" at ");
    text.append_uint(m.refresh_millihz / 1000);
    text.push_back('.');
    text.append_uint(m.refresh_millihz % 1000 / 100);
    text.append(" Hz");
  }
  if (m.interlaced) text.append(", interlaced");
  sink.add(name, text.view());
}

const char* usb_speed_name(hardware::UsbSpeed speed) {
  switch (speed) {
    case hardware::UsbSpeed::Low: return "Low speed (1.5 Mbps)";
    case hardware::UsbSpeed::Full: return "Full speed (12 Mbps)";
    case hardware::UsbSpeed::High: return "High speed (480 Mbps)";
    case hardware::UsbSpeed::Super: return "SuperSpeed (5 Gbps)";
    case hardware::UsbSpeed::SuperPlus: return "SuperSpeed+ (10 Gbps or more)";
    default: return "";
  }
}

const char* printer_state_name(hardware::PrinterState state) {
  switch (state) {
    case hardware::PrinterState::Ready: return "Ready";
    case hardware::PrinterState::Busy: return "Busy";
    default: return "Error";
  }
}

}  // namespace

bool SystemInventory::describe(HardwareGroup group, uint32_t id, desktop::DevicePropertyList* out) {
  out->clear();
  ListSink sink(out);
  String text(allocator_);
  const char* type = desktop::hardware_group_name(group);
  switch (group) {
    case HardwareGroup::Processors: {
      if (!cpu_) return false;
      set_title(out, cpu_->model()[0] ? cpu_->model() : cpu_->name());
      sink.add("Type", "Processor");
      sink.add_text("Driver", cpu_->name());
      sink.add_text("Model", cpu_->model());
      uint32_t efficiency = 0;
      for (uint32_t c = 0; c < cpu_->core_count(); ++c)
        if (cpu_->core_kind(c) == hardware::CoreKind::Efficiency) ++efficiency;
      sink.add_number("Logical processors", cpu_->core_count());
      if (efficiency) {
        sink.add_number("Performance cores", cpu_->core_count() - efficiency);
        sink.add_number("Efficiency cores", efficiency);
      }
      cpu_->describe(sink);
      return true;
    }
    case HardwareGroup::Memory:
      if (!ram_total_) return false;
      desktop::format_size(ram_total_, &text);
      text.append(" of memory");
      set_title(out, text.view());
      sink.add("Type", "System memory");
      add_size(sink, "Installed", ram_total_, allocator_);
      add_size(sink, "Usable by the allocator", ram_usable_, allocator_);
      return true;
    case HardwareGroup::DisplayAdapters: {
      if (!gpu_) return false;
      set_title(out, gpu_->name());
      sink.add("Type", "Display adapter");
      sink.add_text("Driver", gpu_->name());
      text.append_uint(gpu_->width());
      text.push_back('x');
      text.append_uint(gpu_->height());
      sink.add("Screen", text.view());
      sink.add_number("Video modes", gpu_->mode_count());
      uint64_t total = 0, used = 0;
      if (gpu_->video_memory(&total, &used)) {
        if (total) add_size(sink, "Video memory", total, allocator_);
        add_size(sink, "Video memory in use", used, allocator_);
      }
      hardware::PciAddress where;
      if (gpu_->pci_location(&where)) {
        text.clear();
        text.append("bus ");
        text.append_uint(where.bus);
        text.append(", device ");
        text.append_uint(where.device);
        text.append(", function ");
        text.append_uint(where.function);
        sink.add("PCI location", text.view());
      }
      const int32_t load = gpu_->utilization_percent();
      if (load >= 0) sink.add_number("Usage", static_cast<uint32_t>(load), "%");
      const int32_t temperature = gpu_->temperature_millicelsius();
      if (temperature >= 0) sink.add_number("Temperature", static_cast<uint32_t>(temperature) / 1000, "C");
      gpu_->describe(sink);
      return true;
    }
    case HardwareGroup::Displays: {
      if (!monitors_ || id >= monitors_->count()) return false;
      const hardware::Monitor& m = monitors_->monitor(id);
      set_title(out, m.name());
      sink.add("Type", "Display");
      sink.add_text("Connector", m.connector_name());
      sink.add_text("Connector type", hardware::connector_type_name(m.connector_type()));
      sink.add("Status", m.connected() ? "Connected" : "Not connected");
      if (m.connected()) {
        add_mode(sink, "Current mode", m.current_mode(), allocator_);
        const int32_t slot = monitors_->layout().find(m.id());
        if (slot >= 0) sink.add_flag("Primary display", monitors_->layout().at(static_cast<uint32_t>(slot)).primary);
      }
      sink.add_number("Modes", m.mode_count());
      for (uint32_t i = 0; i < m.mode_count(); ++i) {
        hardware::MonitorMode mode;
        if (m.mode_info(i, &mode) && mode.preferred) add_mode(sink, "Native mode", mode, allocator_);
      }
      if (m.width_mm()) {
        text.clear();
        text.append_uint(m.width_mm());
        text.append(" x ");
        text.append_uint(m.height_mm());
        text.append(" mm");
        sink.add("Picture size", text.view());
      }
      if (const hardware::EdidInfo* e = m.edid()) {
        if (e->valid) {
          sink.add_text("Manufacturer", e->manufacturer);
          sink.add_hex("Product code", e->product_code);
          sink.add_text("Serial number", e->serial_text);
          if (e->year) {
            text.clear();
            text.append_uint(e->year);
            if (e->week) {
              text.append(", week ");
              text.append_uint(e->week);
            }
            sink.add("Made", text.view());
          }
          sink.add("Input", e->digital ? "Digital" : "Analog");
          if (e->bits_per_color) sink.add_number("Colour depth", e->bits_per_color, "bits per colour");
          if (e->max_vertical_hz) {
            text.clear();
            text.append_uint(e->min_vertical_hz);
            text.append(" to ");
            text.append_uint(e->max_vertical_hz);
            text.append(" Hz");
            sink.add("Vertical refresh", text.view());
          }
          if (e->hdmi) sink.add_flag("HDMI", true);
          if (e->has_audio) sink.add_flag("Audio", true);
        }
      }
      return true;
    }
    case HardwareGroup::Storage: {
      hardware::IStorageDevice* d = storage_ && id < storage_->count() ? storage_->at(id) : nullptr;
      if (!d) return false;
      const hardware::StorageInfo info = d->info();
      set_title(out, info.model[0] ? StringView(info.model) : StringView(d->name()));
      sink.add("Type", info.kind == hardware::StorageKind::Optical ? "Optical drive" : "Disk drive");
      sink.add_text("Driver", d->name());
      sink.add_text("Model", info.model);
      sink.add_text("Serial number", info.serial);
      add_size(sink, "Capacity", info.block_count * info.block_size, allocator_);
      sink.add_number("Block size", info.block_size, "bytes");
      sink.add_number("Blocks", info.block_count);
      sink.add_flag("Removable", info.removable);
      sink.add_flag("Read-only", info.read_only);
      sink.add_flag("Present", d->present());
      if (d->can_eject()) sink.add_flag("Ejected", d->ejected());
      const hardware::StorageStatistics stats = d->statistics();
      sink.add_number("Reads", stats.reads);
      sink.add_number("Writes", stats.writes);
      add_size(sink, "Data read", stats.blocks_read * info.block_size, allocator_);
      add_size(sink, "Data written", stats.blocks_written * info.block_size, allocator_);
      sink.add_number("Errors", stats.errors);
      d->describe(sink);
      return true;
    }
    case HardwareGroup::Network: {
      hardware::INetworkInterface* n = network_ && id < network_->count() ? network_->at(id) : nullptr;
      if (!n) return false;
      set_title(out, n->name());
      sink.add("Type", n->wireless() ? "Wi-Fi network adapter" : "Network adapter");
      sink.add_text("Driver", n->name());
      const hardware::MacAddress mac = n->mac_address();
      for (int i = 0; i < 6; ++i) {
        if (i) text.push_back(':');
        append_hex(&text, mac.bytes[i], 2);
      }
      sink.add("Hardware address", text.view());
      sink.add("Link", n->link_up() ? "Connected" : "Disconnected");
      if (n->link_up() && n->link_speed_mbps()) sink.add_number("Speed", n->link_speed_mbps(), "Mbps");
      const hardware::NetworkStatistics stats = n->statistics();
      sink.add_number("Frames sent", stats.frames_sent);
      sink.add_number("Frames received", stats.frames_received);
      sink.add_number("Frames dropped", stats.frames_dropped);
      sink.add_number("Send failures", stats.send_failures);
      n->describe(sink);
      return true;
    }
    case HardwareGroup::Audio: {
      hardware::IAudioDevice* d = audio_ && id < audio_->count() ? audio_->at(id) : nullptr;
      if (!d) return false;
      set_title(out, d->name());
      sink.add("Type", audio_->is_hardware(id) ? "Sound card" : "Software sound device");
      sink.add_text("Driver", d->name());
      sink.add_flag("Present", d->present());
      sink.add_flag("Plays sound", d->has_output());
      sink.add_number("Sample rate", hardware::kAudioSampleRate, "Hz");
      sink.add_number("Sample size", hardware::kAudioBytesPerSample * 8, "bits");
      sink.add_number("Channels", hardware::kAudioChannels);
      sink.add_number("Volume", d->volume(), "%");
      sink.add_flag("Muted", d->muted());
      const hardware::AudioStatistics stats = d->statistics();
      sink.add_number("Frames played", stats.frames_played);
      sink.add_number("Frames queued", stats.frames_queued);
      sink.add_number("Underruns", stats.underruns);
      sink.add_number("Refused writes", stats.write_failures);
      if (hardware::IAudioCapture* in = d->capture()) {
        sink.add_flag("Records sound", true);
        const hardware::AudioCaptureStatistics c = in->statistics();
        sink.add_number("Frames recorded", c.frames_captured);
        sink.add_number("Frames lost", c.frames_lost);
      } else {
        sink.add_flag("Records sound", false);
      }
      d->describe(sink);
      return true;
    }
    case HardwareGroup::Midi: {
      if (id == 0) {
        if (!mpu_ || !mpu_->present()) return false;
        set_title(out, "MPU-401 MIDI port");
        sink.add("Type", "MIDI output port");
        sink.add_hex("I/O port", mpu_->base(), 3);
        sink.add_flag("Present", mpu_->present());
        return true;
      }
      hardware::UsbMidiService::DeviceInfo info;
      if (!usb_midi_ || !usb_midi_->device_info(id - 1, &info)) return false;
      set_title(out, info.name);
      sink.add("Type", "USB MIDI device");
      sink.add_flag("Receives notes (output)", info.output);
      sink.add_flag("Sends notes (keys)", info.input);
      sink.add_flag("Synthesizer", info.synthesizer);
      return true;
    }
    case HardwareGroup::Input: {
      if (id >= kMouseIdBase) {
        hardware::IMouse* m = input_ && id - kMouseIdBase < input_->mice().count() ? input_->mice().at(id - kMouseIdBase) : nullptr;
        if (!m) return false;
        set_title(out, m->name());
        sink.add("Type", m->is_absolute() ? "Pointing device (absolute)" : "Mouse");
        sink.add_text("Driver", m->name());
        sink.add_flag("Reports absolute positions", m->is_absolute());
        if (m->is_absolute()) {
          text.append_uint(m->max_x());
          text.append(" x ");
          text.append_uint(m->max_y());
          sink.add("Range", text.view());
        }
        m->describe(sink);
        return true;
      }
      hardware::IKeyboard* k = input_ && id < input_->keyboards().count() ? input_->keyboards().at(id) : nullptr;
      if (!k) return false;
      set_title(out, k->name());
      sink.add("Type", "Keyboard");
      sink.add_text("Driver", k->name());
      k->describe(sink);
      return true;
    }
    case HardwareGroup::Usb: {
      const uint32_t controller = id >> 16, port = id & 0xFFFF;
      if (!usb_ || controller >= usb_->controller_count()) return false;
      if (port == kUsbControllerPort) {
        set_title(out, usb_controller_name(usb_->controller_kind(controller)));
        sink.add("Type", "USB host controller");
        sink.add_number("Ports", usb_->port_count(controller));
        uint32_t used = 0;
        for (uint32_t p = 0; p < usb_->port_count(controller); ++p) {
          hardware::UsbPortInfo info;
          if (usb_->port_info(controller, p, &info) && info.connected) ++used;
        }
        sink.add_number("Ports in use", used);
        return true;
      }
      hardware::UsbDeviceDescriptor d;
      if (port >= usb_->port_count(controller) || !usb_->device_descriptor(controller, port, &d)) return false;
      text.assign("USB device ");   // (the same name as in the tree, which tells it from another device that took the place)
      append_hex(&text, d.vendor_id, 4);
      text.push_back(':');
      append_hex(&text, d.product_id, 4);
      set_title(out, text.view());
      sink.add("Type", "USB device");
      text.clear();
      text.append_uint(controller);
      text.append(", port ");
      text.append_uint(port);
      sink.add("Connected to controller", text.view());
      hardware::UsbPortInfo info;
      if (usb_->port_info(controller, port, &info)) sink.add_text("Speed", usb_speed_name(info.speed));
      sink.add_hex("Vendor ID", d.vendor_id);
      sink.add_hex("Product ID", d.product_id);
      add_usb_version(sink, "USB version", d.usb_version, allocator_);
      add_usb_version(sink, "Device version", d.device_version, allocator_);
      sink.add_hex("Device class", d.device_class, 2);
      sink.add_text("Class", usb_class_name(d.device_class));
      sink.add_hex("Subclass", d.device_subclass, 2);
      sink.add_hex("Protocol", d.device_protocol, 2);
      sink.add_number("Configurations", d.configuration_count);
      return true;
    }
    case HardwareGroup::Printers: {
      hardware::IPrinter* p = printers_ && id < printers_->count() ? printers_->at(id) : nullptr;
      if (!p) return false;
      set_title(out, p->name());
      sink.add("Type", "Printer");
      sink.add_text("Driver", p->name());
      const hardware::PrinterInfo& info = p->info();
      sink.add_text("Manufacturer", info.manufacturer);
      sink.add_text("Model", info.model);
      sink.add("Status", printer_state_name(p->state()));
      sink.add_flag("Present", p->present());
      sink.add_number("Jobs waiting", p->pending_jobs());
      sink.add_flag("Colour", info.color);
      sink.add_flag("Double-sided", info.duplex);
      if (info.pages_per_minute) sink.add_number("Speed", info.pages_per_minute, "pages per minute");
      sink.add_number("Resolution", info.native_dpi, "dpi");
      if (info.max_dpi != info.native_dpi) sink.add_number("Highest resolution", info.max_dpi, "dpi");
      sink.add_flag("Reads PCL", info.supports(hardware::PrinterLanguage::Pcl));
      sink.add_flag("Reads PostScript", info.supports(hardware::PrinterLanguage::PostScript));
      add_ipv4(sink, "IP address", p->ipv4_address(), allocator_);
      if (p->ipv4_address()) sink.add_flag("Encrypted", p->encrypted());
      const hardware::PrinterStatistics stats = p->statistics();
      sink.add_number("Jobs sent", stats.jobs_sent);
      sink.add_number("Jobs failed", stats.jobs_failed);
      add_size(sink, "Data sent", stats.bytes_sent, allocator_);
      return true;
    }
    case HardwareGroup::Bluetooth: {
      hardware::IBluetoothAdapter* a = bluetooth_ && id < bluetooth_->adapter_count() ? bluetooth_->adapter(id) : nullptr;
      if (!a) return false;
      set_title(out, a->name());
      sink.add("Type", "Bluetooth adapter");
      const hardware::hci::BdAddr& address = a->address();
      static const char hex[] = "0123456789ABCDEF";
      text.clear();
      for (int i = 5; i >= 0; --i) {
        text.push_back(hex[address.b[i] >> 4]);
        text.push_back(hex[address.b[i] & 15]);
        if (i > 0) text.push_back(':');
      }
      sink.add("Address", text.view());
      sink.add_flag("Ready", a->ready());
      sink.add_flag("Searching for devices", a->discovering());
      sink.add_flag("Reconnects paired devices", a->auto_reconnect());
      uint32_t connected = 0, paired = 0;
      for (uint32_t i = 0; i < a->device_count(); ++i) {
        hardware::BluetoothDeviceInfo info;
        if (!a->device(i, &info)) continue;
        connected += info.state == hardware::BluetoothLinkState::Connected;
        paired += info.paired;
      }
      sink.add_number("Devices found", a->device_count());
      sink.add_number("Devices connected", connected);
      sink.add_number("Devices paired", paired);
      return true;
    }
    case HardwareGroup::Cameras: {
      hardware::ICamera* c = cameras_ && id < cameras_->count() ? cameras_->at(id) : nullptr;
      if (!c) return false;
      set_title(out, c->name());
      sink.add("Type", "Camera");
      sink.add_text("Driver", c->name());
      sink.add_flag("Present", c->present());
      sink.add_flag("Streaming", c->streaming());
      sink.add_number("Modes", c->mode_count());
      for (uint32_t i = 0; i < c->mode_count() && i < 8; ++i) {
        hardware::CameraMode m;
        if (!c->mode(i, &m)) continue;
        text.clear();
        text.append_uint(m.width);
        text.push_back('x');
        text.append_uint(m.height);
        text.append(" at ");
        text.append_uint(m.fps);
        text.append(m.codec == hardware::VideoCodec::Raw ? " fps, uncompressed" : " fps, compressed");
        sink.add("Mode", text.view());
      }
      static const char* const kControls[hardware::kCameraControlCount] = {"Brightness", "Contrast", "Saturation", "Sharpness"};
      for (uint32_t i = 0; i < hardware::kCameraControlCount; ++i) {
        const auto control = static_cast<hardware::CameraControl>(i);
        if (!c->has_control(control)) continue;
        const int percent = c->get_control(control);
        if (percent >= 0) sink.add_number(kControls[i], static_cast<uint32_t>(percent), "%");
      }
      const hardware::CameraStatistics stats = c->statistics();
      sink.add_number("Pictures received", stats.frames);
      sink.add_number("Pictures dropped", stats.frames_dropped);
      sink.add_number("Pictures damaged", stats.frames_damaged);
      c->describe(sink);
      return true;
    }
    default: {   // System devices: what the PCI bus found
      hardware::PciDeviceInfo d;
      if (!pci_ || id >= pci_->device_count() || !pci_->pci_device(id, &d)) return false;
      text.assign("PCI ");
      text.append(pci_class_name(d.class_code));
      text.append(" ");
      append_hex(&text, d.vendor_id, 4);
      text.push_back(':');
      append_hex(&text, d.device_id, 4);
      set_title(out, text.view());
      sink.add("Type", type);
      sink.add("Bus", "PCI");
      sink.add_text("Class", pci_class_name(d.class_code));
      sink.add_hex("Vendor ID", d.vendor_id);
      sink.add_hex("Device ID", d.device_id);
      sink.add_hex("Class code", d.class_code, 2);
      sink.add_hex("Subclass", d.subclass, 2);
      sink.add_hex("Programming interface", d.prog_if, 2);
      text.clear();
      text.append_uint(d.address.bus);
      text.append(", device ");
      text.append_uint(d.address.device);
      text.append(", function ");
      text.append_uint(d.address.function);
      sink.add("Location", text.view());
      return true;
    }
  }
}

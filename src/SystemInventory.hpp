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

// The computer manager's Hardware tab, filled from the machine's subsystems: the CPU, the RAM, the display adapter and the monitors, the storage,
// network, audio, MIDI, input, USB, printer and camera devices, and every device found on the PCI bus. Each source is optional (nullptr: that group
// is left out). refresh() looks at them all again - hot-plugged devices appear and go - and says whether the list changed. The sources must outlive it.
#pragma once
#include "Desktop/HardwareInventory.hpp"
#include "Utility/String.hpp"
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Camera/SystemCameras.hpp"
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/MidiDevice/Mpu401.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiService.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"
#include "Hardware/NetworkInterface/SystemNetworkInterfaces.hpp"
#include "Hardware/Printer/SystemPrinters.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/SystemInput.hpp"

class SystemInventory final : public desktop::IHardwareInventory {
 public:
  explicit SystemInventory(allocators::IAllocator& allocator) : allocator_(allocator), current_(allocator), scratch_(allocator) {}

  void set_cpu(hardware::ICentralProcessingUnit* cpu) { cpu_ = cpu; }
  void set_memory(uint64_t total_bytes, uint64_t usable_bytes) { ram_total_ = total_bytes; ram_usable_ = usable_bytes; }
  void set_gpu(hardware::IGraphicsProcessingUnit* gpu) { gpu_ = gpu; }
  void set_monitors(hardware::SystemMonitors* monitors) { monitors_ = monitors; }
  void set_storage(hardware::SystemStorageDevices* storage) { storage_ = storage; }
  void set_network(hardware::SystemNetworkInterfaces* network) { network_ = network; }
  void set_audio(hardware::SystemAudioDevices* audio) { audio_ = audio; }
  void set_midi(hardware::UsbMidiService* usb_midi, hardware::Mpu401Output* mpu) { usb_midi_ = usb_midi; mpu_ = mpu; }
  void set_input(hardware::SystemInput* input) { input_ = input; }
  void set_usb(hardware::IUsbBus* usb) { usb_ = usb; }
  void set_printers(hardware::SystemPrinters* printers) { printers_ = printers; }
  void set_cameras(hardware::SystemCameras* cameras) { cameras_ = cameras; }
  void set_pci(hardware::IPciBus* pci) { pci_ = pci; }

  bool refresh() override;
  uint32_t count() const override { return current_.count(); }
  bool entry(uint32_t index, desktop::HardwareEntry* out) const override;

 private:
  void collect(desktop::HardwareList* list);
  void add(desktop::HardwareList* list, desktop::HardwareGroup group, StringView name, uint32_t id = 0);   // detail = scratch_
  allocators::IAllocator& allocator_;
  desktop::HardwareList current_;
  String scratch_;
  hardware::ICentralProcessingUnit* cpu_ = nullptr;
  uint64_t ram_total_ = 0, ram_usable_ = 0;
  hardware::IGraphicsProcessingUnit* gpu_ = nullptr;
  hardware::SystemMonitors* monitors_ = nullptr;
  hardware::SystemStorageDevices* storage_ = nullptr;
  hardware::SystemNetworkInterfaces* network_ = nullptr;
  hardware::SystemAudioDevices* audio_ = nullptr;
  hardware::UsbMidiService* usb_midi_ = nullptr;
  hardware::Mpu401Output* mpu_ = nullptr;
  hardware::SystemInput* input_ = nullptr;
  hardware::IUsbBus* usb_ = nullptr;
  hardware::SystemPrinters* printers_ = nullptr;
  hardware::SystemCameras* cameras_ = nullptr;
  hardware::IPciBus* pci_ = nullptr;
};

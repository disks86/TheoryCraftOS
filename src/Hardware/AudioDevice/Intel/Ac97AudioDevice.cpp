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

#include "Hardware/AudioDevice/Intel/Ac97AudioDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04, kPciBar0 = 0x10, kPciBar1 = 0x14;

// Codec mixer registers (relative to BAR 0).
constexpr uint16_t kMixerReset = 0x00, kMixerMaster = 0x02, kMixerPcmOut = 0x18;
constexpr uint16_t kMixerMute = 0x8000;
constexpr uint16_t kMixerMaxAttenuation = 31;  // 1.5 dB steps

// Bus-master registers (relative to BAR 1).
constexpr uint16_t kGlobalControl = 0x2C, kGlobalStatus = 0x30;
constexpr uint32_t kGlobalColdResetOff = 1u << 1;  // 1 = the codec is not held in reset
constexpr uint32_t kGlobalPrimaryReady = 1u << 8;
// The PCM-out box.
constexpr uint16_t kPoBase = 0x10, kPoBdbar = kPoBase + 0x00, kPoCiv = kPoBase + 0x04, kPoLvi = kPoBase + 0x05,
                   kPoSr = kPoBase + 0x06, kPoPicb = kPoBase + 0x08, kPoCr = kPoBase + 0x0B;
constexpr uint8_t kCrRun = 1u << 0, kCrReset = 1u << 1;
constexpr uint16_t kSrHalted = 1u << 0, kSrClearable = 0x1C;  // last-buffer, completion and FIFO-error flags
}  // namespace

uint16_t Ac97AudioDevice::mixer_read(uint16_t r) const { return IoPort(mixer_ + r).read16(); }
void Ac97AudioDevice::mixer_write(uint16_t r, uint16_t v) const { IoPort(mixer_ + r).write16(v); }
uint8_t Ac97AudioDevice::bus_read8(uint16_t r) const { return IoPort(bus_ + r).read8(); }
uint16_t Ac97AudioDevice::bus_read16(uint16_t r) const { return IoPort(bus_ + r).read16(); }
uint32_t Ac97AudioDevice::bus_read32(uint16_t r) const { return IoPort(bus_ + r).read32(); }
void Ac97AudioDevice::bus_write8(uint16_t r, uint8_t v) const { IoPort(bus_ + r).write8(v); }
void Ac97AudioDevice::bus_write16(uint16_t r, uint16_t v) const { IoPort(bus_ + r).write16(v); }
void Ac97AudioDevice::bus_write32(uint16_t r, uint32_t v) const { IoPort(bus_ + r).write32(v); }

bool Ac97AudioDevice::initialize() {
  if (!supports(device_.vendor_id, device_.device_id)) return false;
  const uint32_t bar0 = pci_.read32(device_.address, kPciBar0), bar1 = pci_.read32(device_.address, kPciBar1);
  if (!(bar0 & 1) || !(bar1 & 1)) return false;  // both ranges are I/O ports
  mixer_ = static_cast<uint16_t>(bar0 & 0xFFFC);
  bus_ = static_cast<uint16_t>(bar1 & 0xFFFC);
  if (!mixer_ || !bus_) return false;

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 0) | (1u << 2));  // I/O space, bus master

  // Take the codec out of cold reset and wait for it.
  bus_write32(kGlobalControl, kGlobalColdResetOff);
  bool ready = false;
  for (int i = 0; i < 1000 && !ready; ++i) {
    ready = bus_read32(kGlobalStatus) & kGlobalPrimaryReady;
    if (!ready) hardware::delay_us(1000);
  }
  if (!ready) {
    shutdown();
    return false;
  }
  mixer_write(kMixerReset, 0);

  descriptors_ = allocate_view<BufferDescriptor>(allocator_, kEntries, 128);
  if (descriptors_.empty() || !ring_.allocate(allocator_, kRingBytes) ||
      reinterpret_cast<uint64_t>(descriptors_.data()) + descriptors_.size() * sizeof(BufferDescriptor) > 0x100000000ull ||
      ring_.address() + ring_.size() > 0x100000000ull) {
    shutdown();  // out of memory, or the buffers are out of the controller's 32-bit reach
    return false;
  }
  for (uint32_t i = 0; i < kEntries; ++i) {
    descriptors_[i].address = static_cast<uint32_t>(ring_.address() + uint64_t{i} * kEntryBytes);
    descriptors_[i].samples = kEntryBytes / kAudioBytesPerSample;
    descriptors_[i].flags = 0;  // no interrupts: we poll
  }

  // Reset the PCM-out box and point it at the descriptor list.
  bus_write8(kPoCr, 0);
  bus_write8(kPoCr, kCrReset);
  bool reset_done = false;
  for (int i = 0; i < 100 && !reset_done; ++i) {
    reset_done = !(bus_read8(kPoCr) & kCrReset);
    if (!reset_done) hardware::delay_us(100);
  }
  if (!reset_done) {
    shutdown();
    return false;
  }
  mixer_write(kMixerPcmOut, 0x0808);  // 0 dB; the master volume is the one we change
  running_ = true;
  apply_volume();
  __atomic_thread_fence(__ATOMIC_RELEASE);
  bus_write32(kPoBdbar, static_cast<uint32_t>(reinterpret_cast<uint64_t>(descriptors_.data())));
  bus_write8(kPoLvi, kEntries - 1);
  bus_write8(kPoCr, kCrRun);
  return true;
}

void Ac97AudioDevice::shutdown() {
  running_ = false;
  if (bus_ && owns_device_) {
    bus_write8(kPoCr, 0);
    bus_write8(kPoCr, kCrReset);  // the engine stops reading our buffers
  }
  ring_.release(allocator_);
  release_view(allocator_, descriptors_);
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

void Ac97AudioDevice::apply_volume() {
  if (!running_) return;
  const uint16_t attenuation = static_cast<uint16_t>((kAudioMaxVolume - volume_) * kMixerMaxAttenuation / kAudioMaxVolume);
  uint16_t value = static_cast<uint16_t>((attenuation << 8) | attenuation);
  if (muted_ || volume_ == 0) value |= kMixerMute;
  mixer_write(kMixerMaster, value);
}

// Where the engine is: the current descriptor, and how far into it.
uint32_t Ac97AudioDevice::hardware_position() {
  uint8_t index = bus_read8(kPoCiv);
  uint16_t left = bus_read16(kPoPicb);  // samples of this descriptor still to play
  const uint8_t again = bus_read8(kPoCiv);
  if (again != index) {  // moved on between the reads: take the new descriptor, from its start
    index = again;
    left = bus_read16(kPoPicb);
  }
  const uint32_t remaining = uint32_t{left} * kAudioBytesPerSample;
  const uint32_t played = remaining >= kEntryBytes ? 0 : kEntryBytes - remaining;
  return (uint32_t{index} % kEntries) * kEntryBytes + played;
}

void Ac97AudioDevice::refresh() {
  if (!running_) return;
  ring_.advance(hardware_position());
  // Keep the last valid descriptor just behind the current one so the list never runs out.
  const uint8_t current = bus_read8(kPoCiv);
  bus_write8(kPoLvi, static_cast<uint8_t>((current + kEntries - 1) % kEntries));
  if (bus_read16(kPoSr) & kSrHalted) {  // it stopped (we were not polled in time): clear and run again
    bus_write16(kPoSr, kSrClearable);
    bus_write8(kPoCr, kCrRun);
  }
}

}  // namespace hardware

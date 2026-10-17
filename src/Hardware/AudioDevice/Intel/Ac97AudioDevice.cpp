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
// The bus-master engines ("boxes"): PCM-in at 0x00, PCM-out at 0x10, each with the same registers.
constexpr uint16_t kPiBase = 0x00, kPoBase = 0x10;
constexpr uint16_t kBdbar = 0x00, kCiv = 0x04, kLvi = 0x05, kSr = 0x06, kPicb = 0x08, kCr = 0x0B;
constexpr uint16_t kPoBdbar = kPoBase + kBdbar, kPoLvi = kPoBase + kLvi, kPoCr = kPoBase + kCr;
// Codec registers for recording.
constexpr uint16_t kMixerRecordSelect = 0x1A, kMixerRecordGain = 0x1C, kMixerMic = 0x0E;
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
  if (!reset_box(kPoBase)) {
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
  capture_.start();  // the input is optional
  return true;
}

void Ac97AudioDevice::shutdown() {
  capture_.stop();
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

bool Ac97AudioDevice::reset_box(uint16_t box) const {
  bus_write8(box + kCr, 0);
  bus_write8(box + kCr, kCrReset);
  for (int i = 0; i < 100; ++i) {
    if (!(bus_read8(box + kCr) & kCrReset)) return true;
    hardware::delay_us(100);
  }
  return false;
}

// Where an engine is: the current descriptor, and how far into it.
uint32_t Ac97AudioDevice::box_position(uint16_t box) const {
  uint8_t index = bus_read8(box + kCiv);
  uint16_t left = bus_read16(box + kPicb);  // samples of this descriptor still to go
  const uint8_t again = bus_read8(box + kCiv);
  if (again != index) {  // moved on between the reads: take the new descriptor, from its start
    index = again;
    left = bus_read16(box + kPicb);
  }
  const uint32_t remaining = uint32_t{left} * kAudioBytesPerSample;
  const uint32_t done = remaining >= kEntryBytes ? 0 : kEntryBytes - remaining;
  return (uint32_t{index} % kEntries) * kEntryBytes + done;
}

// Keeps the last valid descriptor just behind the current one so the list never runs out.
void Ac97AudioDevice::keep_box_running(uint16_t box) const {
  const uint8_t current = bus_read8(box + kCiv);
  bus_write8(box + kLvi, static_cast<uint8_t>((current + kEntries - 1) % kEntries));
  if (bus_read16(box + kSr) & kSrHalted) {  // it stopped (we were not polled in time): clear and run again
    bus_write16(box + kSr, kSrClearable);
    bus_write8(box + kCr, kCrRun);
  }
}

void Ac97AudioDevice::refresh() {
  if (!running_) return;
  ring_.advance(box_position(kPoBase));
  keep_box_running(kPoBase);
}

// Recording: the codec's microphone input, at 0 dB, through the PCM-in engine. Gain and mute are done in software.
bool Ac97AudioDevice::Capture::start() {
  Ac97AudioDevice& d = owner_;
  descriptors_ = allocate_view<BufferDescriptor>(d.allocator_, kEntries, 128);
  if (descriptors_.empty() || !ring_.allocate(d.allocator_, kRingBytes) ||
      reinterpret_cast<uint64_t>(descriptors_.data()) + descriptors_.size() * sizeof(BufferDescriptor) > 0x100000000ull ||
      ring_.address() + ring_.size() > 0x100000000ull || !d.reset_box(kPiBase)) {
    stop();
    return false;
  }
  for (uint32_t i = 0; i < kEntries; ++i) {
    descriptors_[i].address = static_cast<uint32_t>(ring_.address() + uint64_t{i} * kEntryBytes);
    descriptors_[i].samples = kEntryBytes / kAudioBytesPerSample;
    descriptors_[i].flags = 0;
  }
  d.mixer_write(kMixerRecordSelect, 0x0000);  // record from the microphone
  d.mixer_write(kMixerRecordGain, 0x0000);    // 0 dB, not muted
  d.mixer_write(kMixerMic, 0x0008);           // microphone: 0 dB, not muted
  __atomic_thread_fence(__ATOMIC_RELEASE);
  d.bus_write32(kPiBase + kBdbar, static_cast<uint32_t>(reinterpret_cast<uint64_t>(descriptors_.data())));
  d.bus_write8(kPiBase + kLvi, kEntries - 1);
  d.bus_write8(kPiBase + kCr, kCrRun);
  running_ = true;
  return true;
}

void Ac97AudioDevice::Capture::stop() {
  Ac97AudioDevice& d = owner_;
  if (running_ && d.bus_) {
    d.bus_write8(kPiBase + kCr, 0);
    d.bus_write8(kPiBase + kCr, kCrReset);  // the engine stops writing into our buffers
  }
  running_ = false;
  ring_.release(d.allocator_);
  release_view(d.allocator_, descriptors_);
}

void Ac97AudioDevice::Capture::refresh() {
  if (!running_) return;
  ring_.advance(owner_.box_position(kPiBase));
  owner_.keep_box_running(kPiBase);
}

}  // namespace hardware

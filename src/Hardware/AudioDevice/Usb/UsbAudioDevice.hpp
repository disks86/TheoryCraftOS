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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/AudioCaptureBase.hpp"
#include "Hardware/AudioDevice/AudioDeviceBase.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDescriptors.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class UsbAudioDevice;

// The microphone side of a USB audio device (see UsbAudioDevice::capture()).
class UsbAudioCapture final : public RingAudioCapture {
 public:
  explicit UsbAudioCapture(UsbAudioDevice& device) : device_(device) {}
  const char* name() const override;
  bool start(allocators::IAllocator& allocator);
  void stop(allocators::IAllocator& allocator);

 protected:
  void refresh() override;
  bool hardware_gain() const override;
  void apply_gain() override;

 private:
  UsbAudioDevice& device_;
};

// A USB audio class device (USB Audio 1.0 and 2.0: sound cards, DACs, headsets, microphones) on a host controller that does isochronous transfers (xHCI).
// It plays and records the system's fixed format, 48 kHz 16-bit stereo: the alternate setting is chosen among those the device offers at 48 kHz
// (16-bit stereo first, else mono, 24 or 32 bits, more than two channels; the samples are converted), and a device without 48 kHz is left alone.
//
// Playback is a stream of packets, one per service interval (1 ms at full speed), which the driver keeps queued about 24 ms ahead of the controller
// from refresh(), i.e. whenever write(), queued_bytes(), free_bytes() or statistics() is called: the audio mixer does that every 5 ms, and whoever plays
// without it has to call at least every 20 ms or so. A ring that has run dry plays silence (packets of zeros), so the stream never stops. Asynchronous
// devices that have a feedback endpoint are followed through it (the packets get one frame more or less now and then); adaptive and synchronous ones are
// sent exactly 48000 frames a second. Volume and mute use the device's feature unit when it has one, else the samples are scaled here. Frames count as
// played when they are handed to the controller, about 24 ms before they sound.
class UsbAudioDevice final : public RingAudioDevice {
 public:
  static constexpr uint32_t kRingBytes = 64 * 1024;

  // Looks at every device on every host controller that can do isochronous transfers and brings up the audio ones; `out` gets them in bus order.
  static void discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<UsbAudioDevice>>& out);
  // The same for the one device on a root port (a headset that was just plugged in).
  static void discover_port(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator, List<UniquePointer<UsbAudioDevice>>& out);

  UsbAudioDevice(IUsbHostController& host, uint32_t port, const UacDevice& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator);
  ~UsbAudioDevice();

  const char* name() const override { return name_; }
  bool initialize() override;
  void shutdown() override;
  IAudioCapture* capture() override { return capture_started_ ? &capture_ : nullptr; }

  // False once the device was unplugged (the port's generation moved on) or shut down; everything it was doing has stopped by then.
  bool present() const override { return !__atomic_load_n(&shut_down_, __ATOMIC_ACQUIRE) && host_.port_generation(port_) == generation_; }
  bool has_output() const override { return out_.running; }
  bool has_input() const { return in_.running; }

 private:
  friend class UsbAudioCapture;
  static int pick_stream(const UacDevice& model, bool input);

  // One direction of the device: which stream and alternate setting are used, and what was set up for it.
  struct Path {
    int stream = -1;                  // index into model_.streams, -1 if the device has none
    int format = -1;                  // index into the stream's formats once chosen
    bool running = false;             // endpoint(s) started and the alternate setting selected
    uint32_t interval_us = 0;
    uint32_t frames_per_packet = 0;   // nominal
    uint32_t frame_bytes = 0;         // in the device's format
    const UacEntity* unit = nullptr;  // feature unit with volume / mute, if any
    bool volume_control = false, mute_control = false;   // the device can do it (found and answering)
    bool volume_master = false, mute_master = false;     // on the master channel; else on logical channels 1 and 2
    int16_t volume_min = 0, volume_max = 0;               // 1/256 dB
    int32_t last_volume = -100000, last_mute = -1;        // what was sent last
    bool feedback = false;
    uint8_t feedback_endpoint = 0;
    uint32_t feedback_value = 0;      // latest, in the endpoint's fixed-point format (0: none yet)
    uint32_t feedback_frac_bits = 0;  // 14 (full speed, 10.14) or 16 (high speed, 16.16)
    uint64_t accumulator = 0;
    uint32_t capacity = 0;            // packets the controller can hold
  };

  bool bring_up(Path& path, bool input);
  bool try_format(Path& path, bool input, uint32_t format_index);
  bool set_rate(const UacStream& stream, const UacFormat& format);
  bool set_rate_v2(const UacStream& stream);
  void tear_down(Path& path, bool input);
  void find_controls(Path& path, bool input);
  bool unit_get(const Path& path, uint8_t request, uint8_t selector, uint8_t channel, ByteView out);
  bool unit_set(const Path& path, uint8_t selector, uint8_t channel, ConstByteView value);
  void set_unit_volume(Path& path, uint8_t percent, bool muted);
  bool hardware_input_gain() const { return in_.volume_control; }
  void apply_volume() override;
  void apply_input_gain();
  void refresh() override;
  void pump_output();
  void poll_feedback();
  void drain_input(CaptureRing& ring);
  uint32_t next_frames();
  void read_name(const UsbDeviceDescriptor& descriptor);

  IUsbHostController& host_;
  uint32_t port_;
  allocators::IAllocator& allocator_;
  UacDevice model_;
  Path out_, in_;
  uint8_t soft_gain_ = 100;           // percent applied to the samples here, when the device cannot do volume
  UsbAudioCapture capture_;
  bool capture_started_ = false;
  bool configured_ = false;
  uint32_t generation_ = 0;         // the port's generation when the device was taken over
  bool shut_down_ = false;
  char name_[64] = "USB audio";
};

}  // namespace hardware

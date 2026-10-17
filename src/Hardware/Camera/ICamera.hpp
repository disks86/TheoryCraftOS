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
#include "Hardware/IHardware.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"

namespace hardware {

// A video camera (a webcam). It streams pictures at a size and rate it offers as modes; the program takes the newest one with
// read_frame(). Polled: nothing blocks. Between start() and stop() the hardware has to be serviced often (pump(), every few
// milliseconds: SystemCameras can run a thread for it), or pictures are lost. Implementations live in subfolders by transport (Usb/).
struct CameraMode {
  uint16_t width = 0, height = 0;
  uint16_t fps = 0;                    // rounded
  VideoCodec codec = VideoCodec::Raw;  // Mjpeg: compressed on the wire; Raw: uncompressed (YUV)
};

// Picture adjustments a camera may offer, each as 0..100 of the camera's own range.
enum class CameraControl : uint8_t { Brightness, Contrast, Saturation, Sharpness };
constexpr uint32_t kCameraControlCount = 4;

struct CameraStatistics {
  uint64_t frames = 0;          // complete pictures received
  uint64_t frames_read = 0;     // pictures handed out by read_frame()
  uint64_t frames_dropped = 0;  // pictures replaced before anyone read them
  uint64_t frames_damaged = 0;  // pictures thrown away: errors flagged by the camera, missing or extra data, undecodable
  uint64_t packets_bad = 0;     // packets with a header that makes no sense
};

class ICamera : public IHardware {
 public:
  // False once the camera is gone (a USB camera that was unplugged): it then streams nothing and start() fails. Cameras that cannot be removed
  // are always present.
  virtual bool present() const { return true; }

  virtual uint32_t mode_count() const = 0;
  virtual bool mode(uint32_t index, CameraMode* out) const = 0;

  // Starts streaming the mode at `index`. False if the camera cannot (not enough bandwidth, refused).
  virtual bool start_mode(uint32_t index) = 0;
  // Starts the mode nearest to the request that works: the same size or the largest below it, then the rate nearest below `fps`
  // (or above if none), compressed before uncompressed.
  virtual bool start(uint32_t width, uint32_t height, uint32_t fps) = 0;
  virtual void stop() = 0;
  virtual bool streaming() const = 0;
  // Size of the pictures while streaming; zeros otherwise. Pictures are delivered in whatever pixel format `out` of read_frame() has.
  virtual VideoFormat frame_format() const = 0;
  // True when YUV pictures from read_frame() use limited range (most cameras' uncompressed formats); false for full range (JPEG).
  virtual bool limited_range() const = 0;

  // Services the hardware: collects what the camera has sent. Cheap when nothing arrived.
  virtual void pump() = 0;
  // Copies the newest picture not yet read into `out` (allocated for frame_format()'s size, any pixel format). False if there is none.
  virtual bool read_frame(const VideoFrameBuffer& out) = 0;

  virtual bool has_control(CameraControl control) const { (void)control; return false; }
  virtual bool set_control(CameraControl control, uint8_t percent) { (void)control; (void)percent; return false; }
  virtual int get_control(CameraControl control) { (void)control; return -1; }   // percent, -1 if not available

  virtual CameraStatistics statistics() const = 0;

 protected:
  constexpr ICamera() = default;
  ~ICamera() = default;
};

}  // namespace hardware

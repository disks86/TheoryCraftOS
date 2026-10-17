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
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Camera/ICamera.hpp"
#include "Hardware/Camera/Usb/UvcDescriptors.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A USB video class camera (UVC 1.0, 1.1, 1.5) on a host controller that does isochronous transfers (xHCI). It offers the modes of its
// Motion JPEG and uncompressed (YUY2, UYVY, NV12, I420) formats; start_mode() negotiates one with the camera (probe and commit), picks the
// smallest alternate setting of the streaming interface that carries what the camera needs and collects the payloads of the isochronous
// packets into pictures (frame ID toggles and end-of-frame bits, error flags honoured). Pictures are decoded when they are read: JPEG
// through the software decoder (with the standard Huffman tables supplied when the camera leaves them out), packed YUV converted.
// What is not handled: cameras that stream over a bulk endpoint and alternate settings that need several transactions per interval
// (high-bandwidth, which the host controller code cannot schedule), frame-based formats (H.264 and the like), still image capture, and
// pan/tilt/zoom and exposure controls. Brightness, contrast, saturation and sharpness are used when the processing unit has them.
// pump() must run at least every ~10 ms at high speed (the controller holds 128 packets of 125 us) or every ~100 ms at full speed.
class UvcCamera final : public ICamera {
 public:
  static constexpr uint32_t kMaxModes = 64;

  // Looks at every device on every host controller that can do isochronous transfers and keeps the cameras (not yet streaming).
  static void discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<UvcCamera>>& out);

  UvcCamera(IUsbHostController& host, uint32_t port, const UvcDevice& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator);
  ~UvcCamera();

  const char* name() const override { return name_; }
  bool initialize() override;
  void shutdown() override;

  uint32_t mode_count() const override { return mode_count_; }
  bool mode(uint32_t index, CameraMode* out) const override;
  bool start_mode(uint32_t index) override;
  bool start(uint32_t width, uint32_t height, uint32_t fps) override;
  void stop() override;
  bool streaming() const override { return streaming_; }
  VideoFormat frame_format() const override;
  bool limited_range() const override { return active_ >= 0 && modes_[active_].kind != UvcFormatKind::Mjpeg; }
  void pump() override;
  bool read_frame(const VideoFrameBuffer& out) override;

  bool has_control(CameraControl control) const override { return controls_[static_cast<uint8_t>(control)].present; }
  bool set_control(CameraControl control, uint8_t percent) override;
  int get_control(CameraControl control) override;

  CameraStatistics statistics() const override;

 private:
  struct Mode {
    CameraMode info;
    UvcFormatKind kind = UvcFormatKind::Mjpeg;
    uint8_t format_index = 0, frame_index = 0;   // the descriptors' own numbers
    uint32_t interval = 0;                       // 100 ns units
    uint32_t max_frame_size = 0;
  };
  struct Control {
    bool present = false;
    bool is_signed = false;
    uint8_t selector = 0;
    int32_t minimum = 0, maximum = 0;
  };

  void build_modes();
  void find_controls();
  bool negotiate(const Mode& mode, uint32_t* payload, uint32_t* frame_size);
  bool control_get(uint8_t request, uint8_t selector, uint16_t index, ByteView out);
  void stop_locked();
  void release_buffers();
  void handle_packet(const uint8_t* data, uint32_t length);
  void finish_frame();
  uint32_t raw_frame_bytes(const Mode& mode) const;
  bool convert_raw(const VideoFrameBuffer& out, ConstByteView data);
  void read_name(const UsbDeviceDescriptor& descriptor);

  IUsbHostController& host_;
  uint32_t port_;
  allocators::IAllocator& allocator_;
  UvcDevice model_;
  Mode modes_[kMaxModes];
  uint32_t mode_count_ = 0;
  Control controls_[kCameraControlCount];
  bool configured_ = false;

  mutable kernel::Mutex mutex_;       // the streaming state and the buffers' roles
  kernel::Mutex read_mutex_;          // one reader at a time
  bool streaming_ = false;
  int active_ = -1;                   // index into modes_
  UvcAlternate alternate_;
  VideoFormat format_;
  ByteView assembly_, latest_, reading_;   // the picture being collected, the newest finished one, the one being decoded
  uint32_t assembly_length_ = 0, latest_length_ = 0;
  bool fresh_ = false;                // latest_ has not been read
  bool synced_ = false;               // a frame boundary has been seen
  bool finished_ = false;             // the current frame is complete: ignore the rest of it
  bool frame_error_ = false;
  uint8_t fid_ = 0;
  uint32_t expected_raw_ = 0;
  UniquePointer<VideoFrameBuffer> scratch_;   // I420 picture for packed YUV
  JpegDecoder jpeg_;
  mutable CameraStatistics stats_;
  char name_[64] = "USB camera";
};

}  // namespace hardware

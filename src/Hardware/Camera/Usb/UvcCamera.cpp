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

#include "Hardware/Camera/Usb/UvcCamera.hpp"
#include "Hardware/Camera/Usb/UvcMjpeg.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {

constexpr uint8_t kSetInterface = 11;
constexpr uint8_t kClassInterfaceOut = 0x21, kClassInterfaceIn = 0xA1;
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81, kGetMin = 0x82, kGetMax = 0x83;
constexpr uint8_t kProbeControl = 1, kCommitControl = 2;
constexpr uint32_t kMaxFrameBytes = 16u * 1024 * 1024;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void put32(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24); }

PixelFormat pixel_format_of(UvcFormatKind kind) { return kind == UvcFormatKind::Nv12 ? PixelFormat::Nv12 : PixelFormat::I420; }

struct ControlInfo { CameraControl control; uint8_t selector; uint8_t bit; bool is_signed; };
const ControlInfo kControls[kCameraControlCount] = {
    {CameraControl::Brightness, 0x02, 0, true},
    {CameraControl::Contrast, 0x03, 1, false},
    {CameraControl::Saturation, 0x07, 3, false},
    {CameraControl::Sharpness, 0x08, 4, false},
};

}  // namespace

// ------------------------------------------------------------------------------------------------ finding cameras

void UvcCamera::discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<UvcCamera>>& out) {
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    IUsbHostController* host = usb.host_controller(c);
    if (!host || !host->supports_isochronous()) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      UsbDeviceDescriptor descriptor;
      uint8_t blob[2048];
      uint16_t length = 0;
      if (!host->device_descriptor(port, &descriptor) || !host->configuration_descriptor(port, ByteView(blob), &length)) continue;
      UvcDevice model;
      if (!parse_uvc_configuration(ConstByteView(blob).at_most(length), &model) || model.stream.alternate_count == 0) continue;
      UniquePointer<UvcCamera> camera = UniquePointer<UvcCamera>::make(allocator, *host, port, model, descriptor, allocator);
      if (!camera || camera->mode_count() == 0) continue;
      if (camera->initialize()) out.push_back(util::move(camera));
    }
  }
}

UvcCamera::UvcCamera(IUsbHostController& host, uint32_t port, const UvcDevice& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator)
    : host_(host), port_(port), allocator_(allocator), model_(model), jpeg_(allocator) {
  build_modes();
  read_name(descriptor);
}

UvcCamera::~UvcCamera() { shutdown(); }

void UvcCamera::read_name(const UsbDeviceDescriptor& d) {
  const char prefix[] = "USB camera: ";
  uint32_t n = 0;
  for (; prefix[n]; ++n) name_[n] = prefix[n];
  uint8_t buffer[64];
  if (d.product_string && host_.control_in(port_, 0x80, 6, static_cast<uint16_t>((3u << 8) | d.product_string), 0x0409, ByteView(buffer)) && buffer[0] >= 4 && buffer[1] == 3) {
    const uint32_t length = buffer[0] < sizeof buffer ? buffer[0] : sizeof buffer;
    uint32_t m = n;
    for (uint32_t i = 2; i + 1 < length && m + 1 < sizeof name_; i += 2) {
      const uint8_t ch = buffer[i];
      if (buffer[i + 1] == 0 && ch >= 32 && ch < 127) name_[m++] = static_cast<char>(ch);
    }
    if (m > n) {
      name_[m] = 0;
      return;
    }
  }
  static const char digits[] = "0123456789abcdef";
  const uint16_t ids[2] = {d.vendor_id, d.product_id};
  n = 0;
  const char fallback[] = "USB camera ";
  for (; fallback[n]; ++n) name_[n] = fallback[n];
  for (int k = 0; k < 2; ++k) {
    for (int shift = 12; shift >= 0; shift -= 4) name_[n++] = digits[(ids[k] >> shift) & 15];
    if (k == 0) name_[n++] = ':';
  }
  name_[n] = 0;
}

// ------------------------------------------------------------------------------------------------ modes

void UvcCamera::build_modes() {
  static const uint32_t kRates[] = {30, 25, 24, 20, 15, 10, 5};
  mode_count_ = 0;
  const UvcStream& s = model_.stream;
  for (uint32_t f = 0; f < s.format_count; ++f) {
    const UvcFormat& format = s.formats[f];
    for (uint32_t k = 0; k < format.frame_count; ++k) {
      const UvcFrame& frame = format.frames[k];
      if (format.kind != UvcFormatKind::Mjpeg && (frame.width & 1)) continue;   // (the chroma of packed and 4:2:0 pictures needs pairs)
      uint32_t intervals[UvcFrame::kMaxIntervals + 8];
      uint32_t count = 0;
      if (frame.interval_count > 0) {
        for (uint32_t i = 0; i < frame.interval_count; ++i) intervals[count++] = frame.intervals[i];
      } else {
        for (uint32_t rate : kRates) {
          uint32_t interval = 10000000u / rate;
          if (interval < frame.interval_min || interval > frame.interval_max) continue;
          if (frame.interval_step > 1) interval = frame.interval_min + ((interval - frame.interval_min + frame.interval_step / 2) / frame.interval_step) * frame.interval_step;
          if (interval > frame.interval_max) interval = frame.interval_max;
          intervals[count++] = interval;
        }
      }
      for (uint32_t i = 0; i < count && mode_count_ < kMaxModes; ++i) {
        const uint32_t fps = (10000000u + intervals[i] / 2) / intervals[i];
        bool duplicate = false;
        for (uint32_t m = 0; m < mode_count_; ++m)
          duplicate = duplicate || (modes_[m].format_index == format.index && modes_[m].frame_index == frame.index && modes_[m].info.fps == fps);
        if (duplicate || fps == 0 || fps > 1000) continue;
        Mode& mode = modes_[mode_count_++];
        mode = Mode();
        mode.info.width = frame.width;
        mode.info.height = frame.height;
        mode.info.fps = static_cast<uint16_t>(fps);
        mode.info.codec = format.kind == UvcFormatKind::Mjpeg ? VideoCodec::Mjpeg : VideoCodec::Raw;
        mode.kind = format.kind;
        mode.format_index = format.index;
        mode.frame_index = frame.index;
        mode.interval = intervals[i];
        mode.max_frame_size = frame.max_frame_size;
      }
    }
  }
}

bool UvcCamera::mode(uint32_t index, CameraMode* out) const {
  if (index >= mode_count_) return false;
  *out = modes_[index].info;
  return true;
}

uint32_t UvcCamera::raw_frame_bytes(const Mode& mode) const {
  VideoFormat f;
  f.width = mode.info.width;
  f.height = mode.info.height;
  f.pixel_format = pixel_format_of(mode.kind);
  if (mode.kind == UvcFormatKind::Yuy2 || mode.kind == UvcFormatKind::Uyvy) return 2u * f.width * f.height;
  return static_cast<uint32_t>(f.frame_bytes());
}

VideoFormat UvcCamera::frame_format() const {
  kernel::ScopedMutex guard(mutex_);
  return streaming_ ? format_ : VideoFormat{0, 0, PixelFormat::Bgra8888};
}

CameraStatistics UvcCamera::statistics() const {
  kernel::ScopedMutex guard(mutex_);
  return stats_;
}

// ------------------------------------------------------------------------------------------------ bring-up

bool UvcCamera::initialize() {
  if (!host_.set_configuration(port_, model_.configuration)) return false;
  configured_ = true;
  find_controls();
  return true;
}

void UvcCamera::shutdown() {
  stop();
  configured_ = false;
}

bool UvcCamera::control_get(uint8_t request, uint8_t selector, uint16_t index, ByteView out) {
  return host_.control_in(port_, kClassInterfaceIn, request, static_cast<uint16_t>(selector << 8), index, out);
}

void UvcCamera::find_controls() {
  if (model_.processing_unit == 0) return;
  const uint16_t index = static_cast<uint16_t>((model_.processing_unit << 8) | model_.control_interface);
  for (uint32_t i = 0; i < kCameraControlCount; ++i) {
    const ControlInfo& info = kControls[i];
    Control& c = controls_[static_cast<uint8_t>(info.control)];
    c = Control();
    if (!((model_.processing_controls >> info.bit) & 1)) continue;
    uint8_t lo[2] = {}, hi[2] = {};
    if (!control_get(kGetMin, info.selector, index, ByteView(lo)) || !control_get(kGetMax, info.selector, index, ByteView(hi))) continue;
    c.is_signed = info.is_signed;
    c.selector = info.selector;
    const int32_t a = info.is_signed ? static_cast<int16_t>(lo[0] | (lo[1] << 8)) : (lo[0] | (lo[1] << 8));
    const int32_t b = info.is_signed ? static_cast<int16_t>(hi[0] | (hi[1] << 8)) : (hi[0] | (hi[1] << 8));
    c.minimum = a;
    c.maximum = b;
    c.present = b > a;
  }
}

bool UvcCamera::set_control(CameraControl control, uint8_t percent) {
  kernel::ScopedMutex guard(mutex_);
  Control& c = controls_[static_cast<uint8_t>(control)];
  if (!c.present) return false;
  if (percent > 100) percent = 100;
  const int32_t value = c.minimum + static_cast<int32_t>((int64_t{c.maximum} - c.minimum) * percent / 100);
  const uint8_t data[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return host_.control_out_data(port_, kClassInterfaceOut, kSetCur, static_cast<uint16_t>(c.selector << 8),
                                static_cast<uint16_t>((model_.processing_unit << 8) | model_.control_interface), ConstByteView(data, 2));
}

int UvcCamera::get_control(CameraControl control) {
  kernel::ScopedMutex guard(mutex_);
  const Control& c = controls_[static_cast<uint8_t>(control)];
  if (!c.present) return -1;
  uint8_t cur[2] = {};
  if (!control_get(kGetCur, c.selector, static_cast<uint16_t>((model_.processing_unit << 8) | model_.control_interface), ByteView(cur))) return -1;
  const int32_t value = c.is_signed ? static_cast<int16_t>(cur[0] | (cur[1] << 8)) : (cur[0] | (cur[1] << 8));
  if (value <= c.minimum) return 0;
  if (value >= c.maximum) return 100;
  return static_cast<int>((int64_t{value} - c.minimum) * 100 / (int64_t{c.maximum} - c.minimum));
}

bool UvcCamera::start(uint32_t width, uint32_t height, uint32_t fps) {
  if (width == 0) width = 65535;
  if (height == 0) height = 65535;
  bool tried[kMaxModes] = {};
  for (uint32_t round = 0; round < mode_count_; ++round) {
    int best = -1;
    uint64_t best_score = ~uint64_t{0};
    for (uint32_t i = 0; i < mode_count_; ++i) {
      if (tried[i]) continue;
      const Mode& m = modes_[i];
      const uint64_t wanted = uint64_t{width} * height, have = uint64_t{m.info.width} * m.info.height;
      const bool fits = m.info.width <= width && m.info.height <= height;
      const uint64_t area = fits ? wanted - have : (uint64_t{1} << 40) + (have > wanted ? have - wanted : wanted - have);
      const uint64_t rate = m.info.fps <= fps ? fps - m.info.fps : 1000u + (m.info.fps - fps);
      const uint64_t score = (area << 21) | (rate << 1) | (m.info.codec == VideoCodec::Mjpeg ? 0 : 1);
      if (score < best_score) {
        best_score = score;
        best = static_cast<int>(i);
      }
    }
    if (best < 0) break;
    tried[best] = true;
    if (start_mode(static_cast<uint32_t>(best))) return true;
  }
  return false;
}

bool UvcCamera::start_mode(uint32_t index) {
  if (index >= mode_count_ || !configured_) return false;
  stop();
  kernel::ScopedMutex read_guard(read_mutex_);
  kernel::ScopedMutex guard(mutex_);
  const Mode& mode = modes_[index];
  const UvcStream& stream = model_.stream;
  const uint16_t interface = stream.interface;
  const uint32_t length = uvc_probe_length(model_.uvc_version);

  // Probe: say what we want, read back what the camera can do.
  uint8_t data[48] = {};
  control_get(kGetCur, kProbeControl, interface, ByteView(data, length));
  data[0] = 1;   // bmHint: the frame interval is the fixed field
  data[1] = 0;
  data[2] = mode.format_index;
  data[3] = mode.frame_index;
  put32(data + 4, mode.interval);
  if (!host_.control_out_data(port_, kClassInterfaceOut, kSetCur, static_cast<uint16_t>(kProbeControl << 8), interface, ConstByteView(data, length))) return false;
  uint8_t answer[48] = {};
  if (!control_get(kGetCur, kProbeControl, interface, ByteView(answer, length))) return false;
  if (answer[2] != mode.format_index || answer[3] != mode.frame_index) return false;
  const uint32_t payload = le32(answer + 22);
  uint32_t frame_size = le32(answer + 18);
  if (payload == 0) return false;

  // The smallest alternate setting that carries the payload.
  const UvcAlternate* alt = nullptr;
  for (uint32_t i = 0; i < stream.alternate_count; ++i) {
    const UvcAlternate& a = stream.alternates[i];
    if (a.max_packet >= payload && (!alt || a.max_packet < alt->max_packet)) alt = &a;
  }
  if (!alt) return false;

  // Buffers.
  const bool jpeg = mode.kind == UvcFormatKind::Mjpeg;
  uint32_t capacity;
  if (jpeg) {
    if (frame_size == 0) frame_size = mode.max_frame_size;
    if (frame_size < uint32_t{mode.info.width} * mode.info.height / 2) frame_size = uint32_t{mode.info.width} * mode.info.height / 2;
    if (frame_size < 65536) frame_size = 65536;
    if (frame_size > kMaxFrameBytes) frame_size = kMaxFrameBytes;
    capacity = frame_size + kUvcHuffmanBytes;
    expected_raw_ = 0;
  } else {
    expected_raw_ = raw_frame_bytes(mode);
    if (expected_raw_ == 0 || expected_raw_ > kMaxFrameBytes) return false;
    capacity = expected_raw_ + 1;   // (one byte over is how an oversized picture is noticed)
  }
  assembly_ = allocate_view<uint8_t>(allocator_, capacity);
  latest_ = allocate_view<uint8_t>(allocator_, capacity);
  reading_ = allocate_view<uint8_t>(allocator_, capacity);
  if (assembly_.empty() || latest_.empty() || reading_.empty()) {
    release_buffers();
    return false;
  }
  format_.width = mode.info.width;
  format_.height = mode.info.height;
  format_.pixel_format = PixelFormat::Bgra8888;
  if (mode.kind == UvcFormatKind::Yuy2 || mode.kind == UvcFormatKind::Uyvy) {
    scratch_ = UniquePointer<VideoFrameBuffer>::make(allocator_, allocator_);
    VideoFormat i420 = format_;
    i420.pixel_format = PixelFormat::I420;
    if (!scratch_ || !scratch_->allocate(i420)) {
      release_buffers();
      return false;
    }
  }

  if (!host_.start_iso_endpoint(port_, alt->endpoint, alt->max_packet, alt->interval)) {
    release_buffers();
    return false;
  }
  // Commit what the camera answered, then switch the interface on.
  const bool committed = host_.control_out_data(port_, kClassInterfaceOut, kSetCur, static_cast<uint16_t>(kCommitControl << 8), interface, ConstByteView(answer, length));
  if (!committed || !host_.control_out(port_, 0x01, kSetInterface, alt->alternate, interface)) {
    host_.stop_iso_endpoint(port_, alt->endpoint);
    release_buffers();
    return false;
  }
  alternate_ = *alt;
  active_ = static_cast<int>(index);
  assembly_length_ = latest_length_ = 0;
  fresh_ = false;
  synced_ = false;
  finished_ = true;
  frame_error_ = false;
  streaming_ = true;
  return true;
}

void UvcCamera::stop() {
  kernel::ScopedMutex read_guard(read_mutex_);
  kernel::ScopedMutex guard(mutex_);
  stop_locked();
}

void UvcCamera::stop_locked() {
  if (!streaming_) return;
  host_.control_out(port_, 0x01, kSetInterface, 0, model_.stream.interface);   // back to the setting without bandwidth
  host_.stop_iso_endpoint(port_, alternate_.endpoint);
  streaming_ = false;
  active_ = -1;
  release_buffers();
}

void UvcCamera::release_buffers() {
  release_view(allocator_, assembly_);
  release_view(allocator_, latest_);
  release_view(allocator_, reading_);
  scratch_.reset();
  assembly_length_ = latest_length_ = 0;
  fresh_ = false;
}

// ------------------------------------------------------------------------------------------------ streaming

void UvcCamera::pump() {
  kernel::ScopedMutex guard(mutex_);
  if (!streaming_) return;
  uint8_t packet[1024];
  uint16_t length = 0;
  for (uint32_t guard_count = 0; guard_count < 1024 && host_.iso_receive(port_, alternate_.endpoint, ByteView(packet), &length); ++guard_count) {
    if (length > 0) handle_packet(packet, length);
  }
}

// One payload: a header (length, flags: frame ID, end of frame, error) and then a piece of the picture.
void UvcCamera::handle_packet(const uint8_t* data, uint32_t length) {
  if (length < 2) { ++stats_.packets_bad; return; }
  const uint32_t header = data[0];
  const uint8_t flags = data[1];
  if (header < 2 || header > length) {
    ++stats_.packets_bad;
    frame_error_ = true;
    return;
  }
  const uint8_t fid = flags & 1;
  if (!synced_) {   // wait for the start of a whole picture
    synced_ = true;
    fid_ = fid;
    finished_ = true;
    return;
  }
  if (fid != fid_) {   // a new picture starts
    if (!finished_ && assembly_length_ > 0) finish_frame();   // the old one never said it ended
    fid_ = fid;
    assembly_length_ = 0;
    frame_error_ = false;
    finished_ = false;
  }
  if (finished_) return;   // the rest of a picture that is already complete
  if (flags & 0x40) frame_error_ = true;
  const uint32_t payload = length - header;
  if (payload > 0) {
    if (assembly_length_ + payload > assembly_.size()) frame_error_ = true;   // too much data for this mode
    else {
      __builtin_memcpy(assembly_.data() + assembly_length_, data + header, payload);
      assembly_length_ += payload;
    }
  }
  if (flags & 0x02) finish_frame();
}

void UvcCamera::finish_frame() {
  finished_ = true;
  bool good = !frame_error_ && assembly_length_ > 0;
  uint32_t length = assembly_length_;
  if (good) {
    if (active_ >= 0 && modes_[active_].kind == UvcFormatKind::Mjpeg) {
      length = uvc_complete_jpeg(assembly_, assembly_length_);
      good = length != 0;
    } else {
      good = assembly_length_ == expected_raw_;
    }
  }
  if (!good) {
    ++stats_.frames_damaged;
    return;
  }
  ++stats_.frames;
  if (fresh_) ++stats_.frames_dropped;
  const ByteView swap = latest_;
  latest_ = assembly_;
  assembly_ = swap;
  latest_length_ = length;
  fresh_ = true;
}

// ------------------------------------------------------------------------------------------------ reading

bool UvcCamera::convert_raw(const VideoFrameBuffer& out, ConstByteView data) {
  const Mode& mode = modes_[active_];
  VideoFrame frame;
  frame.limited_range = true;
  frame.format.width = mode.info.width;
  frame.format.height = mode.info.height;
  if (mode.kind == UvcFormatKind::Yuy2 || mode.kind == UvcFormatKind::Uyvy) {
    if (!scratch_ || !scratch_->valid()) return false;
    const uint32_t w = mode.info.width, h = mode.info.height;
    const bool yuyv = mode.kind == UvcFormatKind::Yuy2;
    const uint32_t y_at = yuyv ? 0 : 1, u_at = yuyv ? 1 : 0, v_at = yuyv ? 3 : 2;
    uint8_t* luma = scratch_->plane(0).data();
    uint8_t* u = scratch_->plane(1).data();
    uint8_t* v = scratch_->plane(2).data();
    const uint32_t cw = (w + 1) / 2;
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = data.data() + uint64_t{y} * w * 2;
      for (uint32_t x = 0; x < w; ++x) luma[uint64_t{y} * w + x] = row[4 * (x >> 1) + y_at + 2 * (x & 1)];
    }
    for (uint32_t cy = 0; cy < (h + 1) / 2; ++cy) {
      const uint8_t* row0 = data.data() + uint64_t{2 * cy} * w * 2;
      const uint8_t* row1 = 2 * cy + 1 < h ? row0 + w * 2 : row0;
      for (uint32_t cx = 0; cx < cw; ++cx) {
        u[uint64_t{cy} * cw + cx] = static_cast<uint8_t>((row0[4 * cx + u_at] + row1[4 * cx + u_at] + 1) / 2);
        v[uint64_t{cy} * cw + cx] = static_cast<uint8_t>((row0[4 * cx + v_at] + row1[4 * cx + v_at] + 1) / 2);
      }
    }
    frame = scratch_->frame();
    frame.limited_range = true;
    return convert_frame(frame, out);
  }
  frame.format.pixel_format = pixel_format_of(mode.kind);
  const VideoFormat& f = frame.format;
  frame.plane[0] = data.data();
  frame.stride[0] = f.plane_stride(0);
  frame.plane[1] = data.data() + f.plane_bytes(0);
  frame.stride[1] = f.plane_stride(1);
  if (f.pixel_format == PixelFormat::I420) {
    frame.plane[2] = frame.plane[1] + f.plane_bytes(1);
    frame.stride[2] = f.plane_stride(2);
  }
  return convert_frame(frame, out);
}

bool UvcCamera::read_frame(const VideoFrameBuffer& out) {
  kernel::ScopedMutex read_guard(read_mutex_);
  uint32_t length;
  {
    kernel::ScopedMutex guard(mutex_);
    if (!streaming_ || !fresh_) return false;
    if (!out.valid() || out.format().width != format_.width || out.format().height != format_.height) return false;
    const ByteView swap = reading_;   // the finished picture changes hands without a copy; the pump keeps filling the other buffers
    reading_ = latest_;
    latest_ = swap;
    length = latest_length_;
    latest_length_ = 0;
    fresh_ = false;
  }
  // (decoding happens outside the lock so the pump is never held up; stop() waits for the reader lock)
  const ConstByteView data(reading_.data(), length);
  bool ok;
  if (modes_[active_].kind == UvcFormatKind::Mjpeg) {
    ok = jpeg_.decode(data) && jpeg_.width() == format_.width && jpeg_.height() == format_.height && jpeg_.output(out);
  } else {
    ok = convert_raw(out, data);
  }
  kernel::ScopedMutex guard(mutex_);
  if (ok) ++stats_.frames_read;
  else ++stats_.frames_damaged;
  return ok;
}

}  // namespace hardware

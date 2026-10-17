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

#include "Hardware/Camera/Usb/UvcDescriptors.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {

constexpr uint8_t kCsInterface = 0x24;
constexpr uint8_t kVcHeader = 1, kVcInputTerminal = 2, kVcProcessingUnit = 5;
constexpr uint8_t kVsFormatUncompressed = 4, kVsFrameUncompressed = 5, kVsFormatMjpeg = 6, kVsFrameMjpeg = 7;
constexpr uint16_t kTerminalCamera = 0x0201;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// The GUID of an uncompressed format: four characters, then the fixed tail of the "FourCC" GUIDs.
bool uncompressed_kind(ConstByteView d, UvcFormatKind* kind) {
  if (d.size() < 27) return false;
  static const uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  for (int i = 0; i < 12; ++i)
    if (d[9 + i] != tail[i]) return false;
  const char* code = reinterpret_cast<const char*>(&d[5]);
  auto is = [&](const char* name) { return code[0] == name[0] && code[1] == name[1] && code[2] == name[2] && code[3] == name[3]; };
  if (is("YUY2")) *kind = UvcFormatKind::Yuy2;
  else if (is("UYVY")) *kind = UvcFormatKind::Uyvy;
  else if (is("NV12")) *kind = UvcFormatKind::Nv12;
  else if (is("I420") || is("IYUV")) *kind = UvcFormatKind::I420;
  else return false;
  return true;
}

bool parse_frame(ConstByteView d, UvcFrame* frame) {
  if (d.size() < 26) return false;
  *frame = UvcFrame();
  frame->index = d[3];
  frame->width = static_cast<uint16_t>(d[5] | (d[6] << 8));
  frame->height = static_cast<uint16_t>(d[7] | (d[8] << 8));
  frame->max_frame_size = le32(&d[17]);
  frame->default_interval = le32(&d[21]);
  const uint32_t type = d[25];
  if (frame->index == 0 || frame->width == 0 || frame->height == 0) return false;
  if (type == 0) {
    if (d.size() < 38) return false;
    frame->interval_min = le32(&d[26]);
    frame->interval_max = le32(&d[30]);
    frame->interval_step = le32(&d[34]);
    return frame->interval_min != 0 && frame->interval_max >= frame->interval_min;
  }
  if (d.size() < 26 + 4 * type) return false;
  for (uint32_t i = 0; i < type && frame->interval_count < UvcFrame::kMaxIntervals; ++i) {
    const uint32_t interval = le32(&d[26 + 4 * i]);
    if (interval != 0) frame->intervals[frame->interval_count++] = interval;
  }
  return frame->interval_count > 0;
}

}  // namespace

uint32_t uvc_probe_length(uint16_t uvc_version) { return uvc_version >= 0x0150 ? 48 : uvc_version >= 0x0110 ? 34 : 26; }

bool parse_uvc_configuration(ConstByteView configuration, UvcDevice* out) {
  *out = UvcDevice();
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbConfigurationInfo config;
  if (!walker.next(&d) || !parse_usb_configuration(d, &config)) return false;
  out->configuration = config.value;

  enum class Part : uint8_t { None, Control, Streaming } part = Part::None;
  UvcStream* stream = nullptr;     // the streaming interface being read
  UvcFormat* format = nullptr;     // the format whose frames come next
  bool stream_done = false;        // a streaming interface with formats was complete before this one
  uint8_t current_alternate = 0;
  bool alternate_skipped = false;
  while (walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    if (parse_usb_interface(d, &interface)) {
      part = Part::None;
      current_alternate = interface.alternate_setting;
      alternate_skipped = false;
      if (interface.device_class != kUsbClassVideo) continue;
      if (interface.subclass == kUsbVideoSubclassControl) {
        if (!out->have_control) {
          out->have_control = true;
          out->control_interface = interface.number;
          part = Part::Control;
        }
      } else if (interface.subclass == kUsbVideoSubclassStreaming) {
        if (stream && stream->interface != interface.number) {
          // a second streaming interface: ours stays if it has formats, else it is replaced
          if (stream->format_count > 0) stream_done = true;
          else { *stream = UvcStream(); stream = nullptr; }
        }
        if (stream_done) continue;
        if (!stream) {
          stream = &out->stream;
          *stream = UvcStream();
          stream->interface = interface.number;
        }
        part = Part::Streaming;
        format = nullptr;
      }
    } else if (usb_descriptor_type(d) == kCsInterface && d.size() >= 3) {
      const uint8_t subtype = d[2];
      if (part == Part::Control) {
        if (subtype == kVcHeader && d.size() >= 5) {
          out->uvc_version = static_cast<uint16_t>(d[3] | (d[4] << 8));
        } else if (subtype == kVcInputTerminal && d.size() >= 8) {
          if ((d[4] | (d[5] << 8)) == kTerminalCamera) out->camera_terminal = true;
        } else if (subtype == kVcProcessingUnit && d.size() >= 8 && out->processing_unit == 0) {
          const uint32_t size = d[7];
          if (size >= 1 && d.size() >= 8 + size) {
            out->processing_unit = d[3];
            out->processing_controls = static_cast<uint16_t>(d[8] | (size > 1 ? d[9] << 8 : 0));
          }
        }
      } else if (part == Part::Streaming && current_alternate == 0 && stream) {
        if (subtype == kVsFormatUncompressed || subtype == kVsFormatMjpeg) {
          format = nullptr;
          UvcFormatKind kind = UvcFormatKind::Mjpeg;
          bool known = true;
          if (subtype == kVsFormatUncompressed) known = uncompressed_kind(d, &kind);
          else known = d.size() >= 11;
          if (known && d[3] != 0 && stream->format_count < UvcStream::kMaxFormats) {
            format = &stream->formats[stream->format_count++];
            *format = UvcFormat();
            format->index = d[3];
            format->kind = kind;
          }
        } else if ((subtype == kVsFrameUncompressed || subtype == kVsFrameMjpeg) && format) {
          const bool matches = (subtype == kVsFrameMjpeg) == (format->kind == UvcFormatKind::Mjpeg);
          UvcFrame frame;
          if (matches && format->frame_count < UvcFormat::kMaxFrames && parse_frame(d, &frame)) format->frames[format->frame_count++] = frame;
        } else if (subtype >= 16) {
          format = nullptr;   // frame based and stream based formats (H.264, ...): their frame descriptors are not ours
        }
      }
    } else if (part == Part::Streaming && stream && parse_usb_endpoint(d, &endpoint)) {
      if (current_alternate == 0) {
        if (endpoint.transfer_type() == 2 && endpoint.is_in()) stream->bulk = true;
      } else if (endpoint.transfer_type() == 1 && endpoint.is_in() && !alternate_skipped) {
        const uint32_t raw = d.size() >= 6 ? (d[4] | (d[5] << 8)) : 0;
        if ((raw >> 11) & 3) {
          stream->high_bandwidth = true;
          alternate_skipped = true;
        } else if (stream->alternate_count < UvcStream::kMaxAlternates && endpoint.max_packet > 0 && endpoint.interval > 0) {
          UvcAlternate* alt = &stream->alternates[stream->alternate_count++];
          alt->alternate = current_alternate;
          alt->endpoint = endpoint.address;
          alt->max_packet = endpoint.max_packet;
          alt->interval = endpoint.interval;
        }
      }
    }
  }
  if (!out->have_control || !stream) return false;
  // formats without frames are of no use
  uint8_t good = 0;
  for (uint32_t i = 0; i < stream->format_count; ++i)
    if (stream->formats[i].frame_count > 0) {
      if (good != i) stream->formats[good] = stream->formats[i];
      ++good;
    }
  stream->format_count = good;
  out->have_stream = good > 0 && (stream->alternate_count > 0 || stream->bulk);
  return out->have_stream;
}

}  // namespace hardware

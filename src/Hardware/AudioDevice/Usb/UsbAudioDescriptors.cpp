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

#include "Hardware/AudioDevice/Usb/UsbAudioDescriptors.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {

enum class Part : uint8_t { None, Control, Streaming };

uint32_t le24(const uint8_t* p) { return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16); }

UacEntity* add_entity(UacDevice* out, uint8_t id, uint8_t kind) {
  if (out->entity_count >= UacDevice::kMaxEntities) return nullptr;
  UacEntity* e = &out->entities[out->entity_count++];
  *e = UacEntity();
  e->id = id;
  e->kind = kind;
  return e;
}

// One CS_INTERFACE descriptor of the audio control interface.
void parse_entity(ConstByteView d, UacDevice* out) {
  if (d.size() < 4) return;
  const uint8_t subtype = d[2];
  const bool v2 = out->version == 2;
  switch (subtype) {
    case 1:   // header
      if (d.size() >= 5) out->version = (d[4] == 2) ? 2 : (d[4] == 1 ? 1 : 0);
      break;
    case kUacInputTerminal: {
      if (d.size() < (v2 ? 9u : 8u)) return;
      UacEntity* e = add_entity(out, d[3], kUacInputTerminal);
      if (!e) return;
      e->terminal_type = static_cast<uint16_t>(d[4] | (d[5] << 8));
      e->clock = v2 ? d[7] : 0;
      e->channels = v2 ? d[8] : d[7];
      break;
    }
    case kUacOutputTerminal: {
      if (d.size() < (v2 ? 9u : 8u)) return;
      UacEntity* e = add_entity(out, d[3], kUacOutputTerminal);
      if (!e) return;
      e->terminal_type = static_cast<uint16_t>(d[4] | (d[5] << 8));
      e->source = d[7];
      e->clock = v2 ? d[8] : 0;
      break;
    }
    case kUacMixer:
    case kUacSelector:
    case kUacClockSelector: {
      if (d.size() < 6) return;
      UacEntity* e = add_entity(out, d[3], subtype);
      if (e && d[4] > 0) e->source = d[5];   // the first input
      break;
    }
    case kUacFeatureUnit: {
      if (d.size() < 6) return;
      UacEntity* e = add_entity(out, d[3], kUacFeatureUnit);
      if (!e) return;
      e->source = d[4];
      if (!v2) {
        const uint32_t size = d[5];   // bytes per channel in bmaControls
        if (size == 0 || d.size() < 6 + size) return;
        for (uint32_t ch = 0; ch < 3; ++ch) {
          const uint32_t at = 6 + ch * size;
          if (at + size > d.size()) break;
          e->mute[ch] = d[at] & 1;
          e->volume[ch] = (d[at] >> 1) & 1;
        }
      } else {
        for (uint32_t ch = 0; ch < 3; ++ch) {
          const uint32_t at = 5 + ch * 4;
          if (at + 4 > d.size()) break;
          const uint32_t controls = d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
          e->mute[ch] = (controls & 3) == 3;          // host programmable
          e->volume[ch] = ((controls >> 2) & 3) == 3;
        }
      }
      break;
    }
    case kUacClockSource: {
      if (!v2 || d.size() < 6) return;
      UacEntity* e = add_entity(out, d[3], kUacClockSource);
      if (e) e->clock_programmable = (d[5] & 3) == 3;
      break;
    }
    default:
      break;
  }
}

// Does this UAC1 format descriptor's rate list contain 48 kHz?
bool uac1_has_48k(ConstByteView d) {
  if (d.size() < 8) return false;
  const uint32_t types = d[7];
  if (types == 0) return d.size() >= 14 && le24(&d[8]) <= 48000 && le24(&d[11]) >= 48000;   // continuous range
  if (d.size() < 8 + types * 3) return false;
  for (uint32_t i = 0; i < types; ++i)
    if (le24(&d[8 + i * 3]) == 48000) return true;
  return false;
}

}  // namespace

bool parse_uac_configuration(ConstByteView configuration, UacDevice* out) {
  *out = UacDevice();
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbConfigurationInfo config;
  if (!walker.next(&d) || !parse_usb_configuration(d, &config)) return false;
  out->configuration = config.value;

  Part part = Part::None;
  UacStream* stream = nullptr;
  UacFormat* format = nullptr;
  uint8_t iso_in_alt = 0;   // isochronous endpoints seen in the current alternate setting
  uint8_t first_iso_address = 0;
  while (walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    const uint8_t type = usb_descriptor_type(d);
    if (parse_usb_interface(d, &interface)) {
      part = Part::None;
      stream = nullptr;
      format = nullptr;
      iso_in_alt = 0;
      first_iso_address = 0;
      if (interface.device_class != kUsbClassAudio) continue;
      if (interface.subclass == kUsbAudioSubclassControl) {
        if (out->version == 0 && out->entity_count == 0) {
          out->control_interface = interface.number;
          out->version = interface.protocol == 0x20 ? 2 : 1;   // the header descriptor confirms it
        }
        part = Part::Control;
      } else if (interface.subclass == kUsbAudioSubclassStreaming) {
        part = Part::Streaming;
        for (uint32_t i = 0; i < out->stream_count; ++i)
          if (out->streams[i].interface == interface.number) stream = &out->streams[i];
        if (!stream && out->stream_count < UacDevice::kMaxStreams) {
          stream = &out->streams[out->stream_count++];
          *stream = UacStream();
          stream->interface = interface.number;
        }
        if (stream && interface.alternate_setting != 0 && stream->format_count < UacStream::kMaxFormats) {
          format = &stream->formats[stream->format_count++];
          *format = UacFormat();
          format->alternate = interface.alternate_setting;
        }
      }
    } else if (type == kUsbCsInterface) {
      if (part == Part::Control) {
        parse_entity(d, out);
      } else if (part == Part::Streaming && stream && format) {
        const uint8_t subtype = d.size() >= 3 ? d[2] : 0;
        if (subtype == 1 && out->version == 1 && d.size() >= 7) {          // AS_GENERAL, UAC1
          stream->terminal_link = d[3];
          format->pcm = (d[5] | (d[6] << 8)) == 1;
        } else if (subtype == 1 && out->version == 2 && d.size() >= 16) {  // AS_GENERAL, UAC2
          stream->terminal_link = d[3];
          format->pcm = (d[6] & 1) && d[5] == 1;
          format->channels = d[10];
        } else if (subtype == 2 && out->version == 1 && d.size() >= 8) {   // FORMAT_TYPE, UAC1
          if (d[3] != 1) format->pcm = false;
          format->channels = d[4];
          format->subslot = d[5];
          format->bits = d[6];
          format->rate_48000 = uac1_has_48k(d);
        } else if (subtype == 2 && out->version == 2 && d.size() >= 6) {   // FORMAT_TYPE, UAC2
          if (d[3] != 1) format->pcm = false;
          format->subslot = d[4];
          format->bits = d[5];
          format->rate_unknown = true;
        }
      }
    } else if (type == kUsbCsEndpoint) {
      if (part == Part::Streaming && format && out->version == 1 && d.size() >= 4 && d[2] == 1) format->rate_control = d[3] & 1;
    } else if (part == Part::Streaming && format && parse_usb_endpoint(d, &endpoint) && endpoint.transfer_type() == 1) {
      if (iso_in_alt == 0) {   // the first isochronous endpoint of the setting is the data endpoint
        format->endpoint = endpoint.address;
        format->max_packet = endpoint.max_packet;
        format->interval = endpoint.interval;
        format->sync = (endpoint.attributes >> 2) & 3;
        if (stream) stream->input = endpoint.is_in();
        first_iso_address = endpoint.address;
      } else if (iso_in_alt == 1 && (endpoint.is_in() != ((first_iso_address & 0x80) != 0) || ((endpoint.attributes >> 4) & 3) == 1)) {
        // a second one pointing the other way (or marked as feedback): where an asynchronous source reports its rate
        format->feedback_endpoint = endpoint.address;
        format->feedback_max_packet = endpoint.max_packet;
        format->feedback_interval = endpoint.interval;
      }
      if (iso_in_alt < 2) ++iso_in_alt;
    }
  }
  // streaming interfaces without any usable alternate setting are of no interest
  uint8_t kept = 0;
  for (uint32_t i = 0; i < out->stream_count; ++i) {
    UacStream& s = out->streams[i];
    uint8_t good = 0;
    for (uint32_t k = 0; k < s.format_count; ++k)
      if (s.formats[k].endpoint != 0) s.formats[good++] = s.formats[k];
    s.format_count = good;
    if (good && s.terminal_link) {
      if (kept != i) out->streams[kept] = s;
      ++kept;
    }
  }
  out->stream_count = kept;
  return (out->version == 1 || out->version == 2) && out->stream_count > 0;
}

const UacEntity* uac_feature_unit(const UacDevice& device, const UacStream& stream) {
  const UacEntity* terminal = device.find(stream.terminal_link);
  if (!terminal) return nullptr;
  uint8_t current = terminal->id;
  for (int hop = 0; hop < 8; ++hop) {
    if (!stream.input) {   // playback: from the host's terminal toward the speaker, the entity whose input is the current one
      const UacEntity* next = nullptr;
      for (uint32_t i = 0; i < device.entity_count && !next; ++i)
        if (device.entities[i].source == current && device.entities[i].kind != kUacOutputTerminal) next = &device.entities[i];
      if (!next) {
        for (uint32_t i = 0; i < device.entity_count && !next; ++i)
          if (device.entities[i].source == current) next = &device.entities[i];
      }
      if (!next) return nullptr;
      if (next->kind == kUacFeatureUnit) return next;
      current = next->id;
    } else {               // recording: from the host's output terminal back toward the microphone
      const UacEntity* e = device.find(hop == 0 ? terminal->source : current);
      if (!e) return nullptr;
      if (e->kind == kUacFeatureUnit) return e;
      current = e->source;
      if (current == 0) return nullptr;
    }
  }
  return nullptr;
}

const UacEntity* uac_clock_source(const UacDevice& device, const UacStream& stream) {
  if (device.version != 2) return nullptr;
  const UacEntity* terminal = device.find(stream.terminal_link);
  if (!terminal || !terminal->clock) return nullptr;
  const UacEntity* clock = device.find(terminal->clock);
  if (clock && clock->kind == kUacClockSelector) clock = device.find(clock->source);
  return clock && clock->kind == kUacClockSource ? clock : nullptr;
}

}  // namespace hardware

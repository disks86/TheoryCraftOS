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

#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04;

// Controller registers.
constexpr uint32_t kGcap = 0x00, kGctl = 0x08, kStatests = 0x0E, kIntctl = 0x20;
constexpr uint32_t kCorbLower = 0x40, kCorbUpper = 0x44, kCorbWrite = 0x48, kCorbRead = 0x4A, kCorbControl = 0x4C,
                   kCorbSize = 0x4E;
constexpr uint32_t kRirbLower = 0x50, kRirbUpper = 0x54, kRirbWrite = 0x58, kRirbCount = 0x5A, kRirbControl = 0x5C,
                   kRirbStatus = 0x5D, kRirbSize = 0x5E;
constexpr uint32_t kGctlReset = 1u << 0;  // 0 = held in reset
constexpr uint16_t kPointerReset = 1u << 15;
constexpr uint8_t kRingDmaRun = 1u << 1;

// Stream descriptor registers (relative to the descriptor).
constexpr uint32_t kSdControl = 0x00, kSdTag = 0x02, kSdPosition = 0x04, kSdBufferLength = 0x08, kSdLastIndex = 0x0C,
                   kSdFormat = 0x12, kSdListLower = 0x18, kSdListUpper = 0x1C;
constexpr uint8_t kSdReset = 1u << 0, kSdRun = 1u << 1;
constexpr uint16_t kFormat48k16bitStereo = 0x0011;

// Codec verbs. A 12-bit verb carries an 8-bit payload, a 4-bit verb a 16-bit one.
constexpr uint32_t kGetParameter = 0xF00, kGetConnectionList = 0xF02, kGetConfigDefault = 0xF1C;
constexpr uint32_t kSetConnectionSelect = 0x701, kSetPower = 0x705, kSetChannelStream = 0x706, kSetPinControl = 0x707,
                   kSetEapd = 0x70C;
constexpr uint32_t kSetFormat = 0x2, kSetAmplifier = 0x3;

// Parameters.
constexpr uint8_t kParamNodeCount = 0x04, kParamFunctionGroupType = 0x05, kParamWidgetCaps = 0x09, kParamPcmRates = 0x0A,
                  kParamPinCaps = 0x0C, kParamInputAmpCaps = 0x0D, kParamConnectionLength = 0x0E, kParamOutputAmpCaps = 0x12;
constexpr uint32_t kWidgetOutput = 0, kWidgetMixer = 2, kWidgetSelector = 3, kWidgetPin = 4;
constexpr uint32_t kCapsOutputAmp = 1u << 2, kCapsAmpOverride = 1u << 3, kCapsFormatOverride = 1u << 4, kCapsInputAmp = 1u << 1;
constexpr uint32_t kPinCapsOutput = 1u << 4, kPinCapsEapd = 1u << 16;
constexpr uint32_t kPcm48k = 1u << 6, kPcm16bit = 1u << 17;

constexpr uint32_t kRingTimeoutUs = 50000;

uint32_t widget_type(uint32_t caps) { return (caps >> 20) & 0xF; }
}  // namespace

uint8_t IntelHdaAudioDevice::read8(uint32_t r) const { return *reinterpret_cast<volatile uint8_t*>(registers_ + r); }
uint16_t IntelHdaAudioDevice::read16(uint32_t r) const { return *reinterpret_cast<volatile uint16_t*>(registers_ + r); }
uint32_t IntelHdaAudioDevice::read32(uint32_t r) const { return *reinterpret_cast<volatile uint32_t*>(registers_ + r); }
void IntelHdaAudioDevice::write8(uint32_t r, uint8_t v) const { *reinterpret_cast<volatile uint8_t*>(registers_ + r) = v; }
void IntelHdaAudioDevice::write16(uint32_t r, uint16_t v) const { *reinterpret_cast<volatile uint16_t*>(registers_ + r) = v; }
void IntelHdaAudioDevice::write32(uint32_t r, uint32_t v) const { *reinterpret_cast<volatile uint32_t*>(registers_ + r) = v; }

// Waits until (register & mask) == value, for up to timeout_us.
bool IntelHdaAudioDevice::wait_register(uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout_us) const {
  for (uint32_t waited = 0; waited <= timeout_us; waited += 50) {
    uint32_t current;
    switch (mask > 0xFFFF ? 4 : mask > 0xFF ? 2 : 1) {
      case 4: current = read32(reg); break;
      case 2: current = read16(reg); break;
      default: current = read8(reg); break;
    }
    if ((current & mask) == value) return true;
    hardware::delay_us(50);
  }
  return false;
}

bool IntelHdaAudioDevice::dma_reachable(uint64_t address, uint64_t bytes) const {
  return dma64_ || address + bytes <= 0x100000000ull;
}

bool IntelHdaAudioDevice::reset_controller() {
  write32(kGctl, 0);
  if (!wait_register(kGctl, kGctlReset, 0, 100000)) return false;
  hardware::delay_us(200);
  write32(kGctl, kGctlReset);
  if (!wait_register(kGctl, kGctlReset, kGctlReset, 100000)) return false;
  // A codec needs a little time to announce itself after the link comes up.
  hardware::delay_us(1000);
  for (int i = 0; i < 100 && !(read16(kStatests) & 0x7FFF); ++i) hardware::delay_us(1000);
  const uint16_t codecs = read16(kStatests) & 0x7FFF;
  if (!codecs) return false;
  codec_ = 0;
  while (!(codecs & (1u << codec_))) ++codec_;
  return true;
}

bool IntelHdaAudioDevice::start_command_rings() {
  write8(kCorbControl, 0);
  write8(kRirbControl, 0);
  // Largest ring each side supports: 256, 16 or 2 entries (size register bits 6, 5, 4).
  const uint8_t corb_caps = read8(kCorbSize) >> 4, rirb_caps = read8(kRirbSize) >> 4;
  const uint32_t corb_code = (corb_caps & 4) ? 2 : (corb_caps & 2) ? 1 : 0;
  const uint32_t rirb_code = (rirb_caps & 4) ? 2 : (rirb_caps & 2) ? 1 : 0;
  static constexpr uint32_t kEntryCounts[3] = {2, 16, 256};
  corb_entries_ = kEntryCounts[corb_code];
  rirb_entries_ = kEntryCounts[rirb_code];
  write8(kCorbSize, static_cast<uint8_t>(corb_code));
  write8(kRirbSize, static_cast<uint8_t>(rirb_code));

  corb_ = allocate_view<uint8_t>(allocator_, uint64_t{corb_entries_} * 4, 128);
  rirb_ = allocate_view<uint8_t>(allocator_, uint64_t{rirb_entries_} * 8, 128);
  if (corb_.empty() || rirb_.empty() || !dma_reachable(reinterpret_cast<uint64_t>(corb_.data()), corb_.size()) ||
      !dma_reachable(reinterpret_cast<uint64_t>(rirb_.data()), rirb_.size()))
    return false;
  corb_.fill(0);
  rirb_.fill(0);
  const uint64_t corb_address = reinterpret_cast<uint64_t>(corb_.data());
  const uint64_t rirb_address = reinterpret_cast<uint64_t>(rirb_.data());
  write32(kCorbLower, static_cast<uint32_t>(corb_address));
  write32(kCorbUpper, static_cast<uint32_t>(corb_address >> 32));
  write32(kRirbLower, static_cast<uint32_t>(rirb_address));
  write32(kRirbUpper, static_cast<uint32_t>(rirb_address >> 32));

  // Reset the read pointer of the CORB (set the bit, see it stick, clear it, see that stick) and the write pointer of the RIRB.
  write16(kCorbRead, kPointerReset);
  if (!wait_register(kCorbRead, kPointerReset, kPointerReset, 10000)) return false;
  write16(kCorbRead, 0);
  if (!wait_register(kCorbRead, kPointerReset, 0, 10000)) return false;
  write16(kCorbWrite, 0);
  corb_write_ = 0;
  write16(kRirbWrite, kPointerReset);
  rirb_read_ = 0;
  write16(kRirbCount, 1);
  write8(kRirbStatus, 0x05);
  write8(kCorbControl, kRingDmaRun);
  write8(kRirbControl, kRingDmaRun);
  return true;
}

// Sends one command (everything below the codec address) and waits for the answer.
bool IntelHdaAudioDevice::verb(uint8_t nid, uint32_t command, uint32_t* response) {
  const uint32_t entry = (uint32_t{codec_} << 28) | (uint32_t{nid} << 20) | (command & 0xFFFFF);
  corb_write_ = (corb_write_ + 1) % corb_entries_;
  const ByteView slot = corb_.subview(uint64_t{corb_write_} * 4, 4);
  for (uint32_t i = 0; i < 4; ++i) slot[i] = static_cast<uint8_t>(entry >> (8 * i));
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write16(kCorbWrite, static_cast<uint16_t>(corb_write_));

  for (uint32_t waited = 0; waited < kRingTimeoutUs; waited += 20) {
    if ((read16(kRirbWrite) & 0xFF) != rirb_read_) {
      rirb_read_ = (rirb_read_ + 1) % rirb_entries_;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      const ConstByteView answer = ConstByteView(rirb_).subview(uint64_t{rirb_read_} * 8, 4);
      if (response) *response = answer[0] | (answer[1] << 8) | (answer[2] << 16) | (uint32_t{answer[3]} << 24);
      write8(kRirbStatus, 0x05);
      return true;
    }
    hardware::delay_us(20);
  }
  return false;
}

bool IntelHdaAudioDevice::get_parameter(uint8_t nid, uint8_t parameter, uint32_t* response) {
  return verb(nid, (kGetParameter << 8) | parameter, response);
}

// The connection list of a node: the ids of the nodes it can take its input from.
uint32_t IntelHdaAudioDevice::connections(uint8_t nid, uint8_t* out, uint32_t capacity) {
  uint32_t length_word = 0;
  if (!get_parameter(nid, kParamConnectionLength, &length_word)) return 0;
  const uint32_t length = length_word & 0x7F;
  const bool long_form = length_word & 0x80;  // 16-bit entries
  const uint32_t per_word = long_form ? 2 : 4;
  uint32_t count = 0;
  uint32_t previous = 0;
  for (uint32_t index = 0; index < length && count < capacity; index += per_word) {
    uint32_t word = 0;
    if (!verb(nid, (kGetConnectionList << 8) | index, &word)) return count;
    for (uint32_t i = 0; i < per_word && index + i < length && count < capacity; ++i) {
      const uint32_t raw = long_form ? (word >> (16 * i)) & 0xFFFF : (word >> (8 * i)) & 0xFF;
      const bool range = raw & (long_form ? 0x8000u : 0x80u);
      const uint32_t value = raw & (long_form ? 0x7FFFu : 0x7Fu);
      if (range && count > 0) {  // every node from the previous entry up to this one
        for (uint32_t n = previous + 1; n <= value && count < capacity; ++n) out[count++] = static_cast<uint8_t>(n);
      } else {
        out[count++] = static_cast<uint8_t>(value);
      }
      previous = value;
    }
  }
  return count;
}

IntelHdaAudioDevice::Amplifier IntelHdaAudioDevice::amplifier(uint8_t nid, uint8_t parameter) {
  Amplifier amp;
  uint32_t caps = 0;
  const uint32_t widget = widget_caps_[nid];
  // A widget with the override bit describes its own amplifier; otherwise the function group's applies.
  if (!get_parameter((widget & kCapsAmpOverride) ? nid : function_group_, parameter, &caps)) return amp;
  amp.present = true;
  amp.offset = caps & 0x7F;
  amp.steps = (caps >> 8) & 0x7F;
  amp.can_mute = caps & 0x80000000u;
  return amp;
}

// Depth-first from a pin towards a converter: pins and mixers/selectors pass through, a converter ends it.
bool IntelHdaAudioDevice::search_converter(uint8_t nid, uint32_t depth) {
  if (depth >= kMaxPath || !widget_caps_[nid]) return false;
  const uint32_t type = widget_type(widget_caps_[nid]);
  path_[depth].nid = nid;
  if (type == kWidgetOutput) {
    path_length_ = depth + 1;
    return true;
  }
  if (type != kWidgetMixer && type != kWidgetSelector && !(type == kWidgetPin && depth == 0)) return false;
  uint8_t list[32];
  const uint32_t count = connections(nid, list, sizeof(list));
  path_[depth].connections = static_cast<uint8_t>(count);
  for (uint32_t i = 0; i < count; ++i) {
    path_[depth].select = static_cast<uint8_t>(i);
    if (search_converter(list[i], depth + 1)) return true;
  }
  return false;
}

// Reads the node tree and picks the output pin to use and the converter behind it.
bool IntelHdaAudioDevice::find_output_path() {
  uint32_t groups = 0;
  if (!get_parameter(0, kParamNodeCount, &groups)) return false;
  const uint32_t first_group = (groups >> 16) & 0xFF, group_count = groups & 0xFF;
  for (uint32_t g = first_group; g < first_group + group_count && g < 256; ++g) {
    uint32_t type = 0;
    if (get_parameter(static_cast<uint8_t>(g), kParamFunctionGroupType, &type) && (type & 0xFF) == 1) {  // audio function group
      function_group_ = static_cast<uint8_t>(g);
      break;
    }
  }
  if (!function_group_) return false;
  verb(function_group_, (kSetPower << 8) | 0, nullptr);  // D0: fully on
  hardware::delay_us(10000);

  uint32_t widgets = 0;
  if (!get_parameter(function_group_, kParamNodeCount, &widgets)) return false;
  const uint32_t first = (widgets >> 16) & 0xFF, count = widgets & 0xFF;
  for (uint32_t n = first; n < first + count && n < 256; ++n) {
    uint32_t caps = 0;
    if (get_parameter(static_cast<uint8_t>(n), kParamWidgetCaps, &caps)) widget_caps_[n] = caps ? caps : 1;  // never 0 for a widget that exists
  }

  // Candidate pins in order of preference: line out, speaker, headphones; then any other output pin.
  static constexpr uint8_t kPreference[] = {0, 1, 2, 0xFF};
  for (uint8_t wanted : kPreference) {
    for (uint32_t n = first; n < first + count && n < 256; ++n) {
      if (widget_type(widget_caps_[n]) != kWidgetPin) continue;
      uint32_t pin_caps = 0, config = 0;
      if (!get_parameter(static_cast<uint8_t>(n), kParamPinCaps, &pin_caps) || !(pin_caps & kPinCapsOutput)) continue;
      if (!verb(static_cast<uint8_t>(n), (kGetConfigDefault << 8), &config)) continue;
      if (((config >> 30) & 3) == 1) continue;  // no physical connection
      if (wanted != 0xFF && ((config >> 20) & 0xF) != wanted) continue;
      path_length_ = 0;
      if (!search_converter(static_cast<uint8_t>(n), 0)) continue;
      pin_has_eapd_ = pin_caps & kPinCapsEapd;
      pin_control_ = ((config >> 20) & 0xF) == 2 ? 0xC0 : 0x40;  // headphones also need their amplifier enabled
      return true;
    }
  }
  return false;
}

// Powers up and unmutes every node on the path and sets the converter's format and stream.
bool IntelHdaAudioDevice::configure_path() {
  const uint8_t pin = path_[0].nid, dac = path_[path_length_ - 1].nid;

  // The converter must be able to play what we send.
  uint32_t rates = 0;
  const bool own_formats = widget_caps_[dac] & kCapsFormatOverride;
  if (!get_parameter(own_formats ? dac : function_group_, kParamPcmRates, &rates) || !(rates & kPcm48k) ||
      !(rates & kPcm16bit))
    return false;

  for (uint32_t i = 0; i < path_length_; ++i) {
    const Hop& hop = path_[i];
    verb(hop.nid, (kSetPower << 8) | 0, nullptr);
    if (i + 1 < path_length_ && hop.connections > 1) verb(hop.nid, (kSetConnectionSelect << 8) | hop.select, nullptr);
    const uint32_t caps = widget_caps_[hop.nid], type = widget_type(caps);
    if (caps & kCapsOutputAmp) {
      const Amplifier amp = amplifier(hop.nid, kParamOutputAmpCaps);
      const uint32_t gain = amp.offset ? amp.offset : amp.steps;
      verb(hop.nid, (kSetAmplifier << 16) | (1u << 15) | (1u << 13) | (1u << 12) | gain, nullptr);
    }
    // A mixer or selector has an input amplifier per connection; open the one the path comes in on.
    if (i > 0 && i + 1 < path_length_ && (type == kWidgetMixer || type == kWidgetSelector) && (caps & kCapsInputAmp)) {
      const Amplifier amp = amplifier(hop.nid, kParamInputAmpCaps);
      const uint32_t gain = amp.offset ? amp.offset : amp.steps;
      verb(hop.nid, (kSetAmplifier << 16) | (1u << 14) | (1u << 13) | (1u << 12) | (uint32_t{hop.select} << 8) | gain, nullptr);
    }
  }

  verb(pin, (kSetPinControl << 8) | pin_control_, nullptr);
  if (pin_has_eapd_) verb(pin, (kSetEapd << 8) | 0x02, nullptr);
  verb(dac, (kSetChannelStream << 8) | (kStreamTag << 4), nullptr);
  verb(dac, (kSetFormat << 16) | kFormat48k16bitStereo, nullptr);

  // The volume control: the converter's amplifier if it has gain steps, else the pin's.
  volume_nid_ = 0;
  const uint8_t candidates[2] = {dac, pin};
  for (uint8_t nid : candidates) {
    if (!(widget_caps_[nid] & kCapsOutputAmp)) continue;
    const Amplifier amp = amplifier(nid, kParamOutputAmpCaps);
    if (amp.steps > 0 || amp.can_mute) {
      volume_nid_ = nid;
      volume_amp_ = amp;
      break;
    }
  }
  return true;
}

bool IntelHdaAudioDevice::start_stream() {
  descriptors_ = allocate_view<BufferDescriptor>(allocator_, kEntries, 128);
  if (descriptors_.empty() || !ring_.allocate(allocator_, kRingBytes) ||
      !dma_reachable(reinterpret_cast<uint64_t>(descriptors_.data()), descriptors_.size() * sizeof(BufferDescriptor)) ||
      !dma_reachable(ring_.address(), ring_.size()))
    return false;
  for (uint32_t i = 0; i < kEntries; ++i) {
    descriptors_[i].address = ring_.address() + uint64_t{i} * kEntryBytes;
    descriptors_[i].length = kEntryBytes;
    descriptors_[i].flags = 0;  // no interrupt on completion
  }

  // Reset the stream descriptor, then program it.
  write8(stream_ + kSdControl, kSdReset);
  if (!wait_register(stream_ + kSdControl, kSdReset, kSdReset, 10000)) return false;
  write8(stream_ + kSdControl, 0);
  if (!wait_register(stream_ + kSdControl, kSdReset, 0, 10000)) return false;

  const uint64_t list = reinterpret_cast<uint64_t>(descriptors_.data());
  write32(stream_ + kSdListLower, static_cast<uint32_t>(list));
  write32(stream_ + kSdListUpper, static_cast<uint32_t>(list >> 32));
  write32(stream_ + kSdBufferLength, kRingBytes);
  write16(stream_ + kSdLastIndex, kEntries - 1);
  write16(stream_ + kSdFormat, kFormat48k16bitStereo);
  write8(stream_ + kSdTag, static_cast<uint8_t>(kStreamTag << 4));
  __atomic_thread_fence(__ATOMIC_RELEASE);
  write8(stream_ + kSdControl, kSdRun);
  return true;
}

bool IntelHdaAudioDevice::initialize() {
  if (device_.class_code != 0x04 || device_.subclass != 0x03) return false;
  registers_ = pci_.memory_bar(device_.address, 0);
  if (!registers_) return false;

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space, bus master

  const uint16_t capabilities = read16(kGcap);
  const uint32_t input_streams = (capabilities >> 8) & 0xF, output_streams = (capabilities >> 12) & 0xF;
  dma64_ = capabilities & 1;
  if (output_streams == 0) {
    shutdown();
    return false;
  }
  stream_ = 0x80 + 0x20 * input_streams;  // the output descriptors follow the input ones

  if (!reset_controller() || !start_command_rings() || !find_output_path() || !configure_path() || !start_stream()) {
    shutdown();
    return false;
  }
  write32(kIntctl, 0);  // polled
  running_ = true;
  apply_volume();
  return true;
}

void IntelHdaAudioDevice::shutdown() {
  running_ = false;
  if (registers_ && owns_device_) {
    write8(stream_ + kSdControl, 0);
    write8(kCorbControl, 0);
    write8(kRirbControl, 0);
    write32(kGctl, 0);  // the controller stops DMA into our memory
    wait_register(kGctl, kGctlReset, 0, 100000);
  }
  ring_.release(allocator_);
  release_view(allocator_, descriptors_);
  release_view(allocator_, corb_);
  release_view(allocator_, rirb_);
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

void IntelHdaAudioDevice::apply_volume() {
  if (!running_ || !volume_nid_) return;
  const bool silent = muted_ || volume_ == 0;
  const uint32_t top = volume_amp_.offset ? volume_amp_.offset : volume_amp_.steps;  // gain value for 0 dB
  uint32_t gain = volume_ * top / kAudioMaxVolume;
  uint32_t flags = (1u << 15) | (1u << 13) | (1u << 12);  // output amplifier, both channels
  if (silent) {
    if (volume_amp_.can_mute) flags |= 1u << 7;
    else gain = 0;
  }
  verb(volume_nid_, (kSetAmplifier << 16) | flags | gain, nullptr);
}

uint32_t IntelHdaAudioDevice::hardware_position() { return read32(stream_ + kSdPosition) % kRingBytes; }

}  // namespace hardware

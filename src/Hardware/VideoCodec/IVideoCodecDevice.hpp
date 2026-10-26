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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/IHardware.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/IVideoDecoder.hpp"
#include "Hardware/VideoCodec/IVideoEncoder.hpp"
#include "Utility/UniquePointer.hpp"

namespace filesystem { class IFileSystem; }

namespace hardware {

struct VideoCodecInfo {
  // A video engine of its own is on this machine (a GPU's decode/encode block, a media chip).
  bool hardware_engine = false;
  // The sessions run on that engine. False means they run on the CPU - either because there is no engine, or
  // because this OS has no driver for it yet.
  bool accelerated = false;
  // Codecs whose sessions run on the engine even though the device as a whole is not `accelerated` (a driver for part of the engine's work). codec_mask() bits.
  uint32_t accelerated_decode_codecs = 0;
  uint32_t accelerated_encode_codecs = 0;
  uint32_t decode_codecs = 0;  // codec_mask() bits create_decoder() can serve
  uint32_t encode_codecs = 0;  // and create_encoder()
  uint32_t max_width = 0, max_height = 0;
};

// What a video engine's driver may use to find its way around the machine once it is up: a way to map physical memory (which refuses memory the OS owns), and
// where the firmware's display buffer is (video memory in use that must be left alone). Empty fields are "not known".
struct VideoEngineEnvironment {
  IRegisterMapper* mapper = nullptr;
  uint64_t framebuffer_base = 0;
  uint64_t framebuffer_bytes = 0;
  // Video memory the display driver keeps for its own framebuffer, at the end of video memory (AMD APUs): an engine must leave it alone.
  uint64_t reserved_tail_bytes = 0;
};

// Something that decodes and encodes video: a hardware engine, or the software fallback that is always
// there. A program asks the device for a session for the codec it needs. Several sessions may be open at
// once; they are independent.
class IVideoCodecDevice : public IHardware {
 public:
  virtual const VideoCodecInfo& info() const = 0;
  bool can_decode(VideoCodec codec) const { return info().decode_codecs & codec_mask(codec); }
  bool can_encode(VideoCodec codec) const { return info().encode_codecs & codec_mask(codec); }
  // A new session, or an empty pointer if the codec or size is not supported or memory ran out.
  virtual UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config) = 0;
  virtual UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config) = 0;
  // One line on what the device found out about its hardware (the GPU generation, the video engines the probe saw), for the boot log; empty if it has nothing to say.
  // Valid until the device is shut down.
  virtual const char* engine_summary() const { return ""; }
  // The GPU this engine belongs to, and how much video memory the probe of its hardware found it has (0 = not found). main hands the size to the display
  // device of the same PCI function, so the computer manager shows the card's memory and not just the framebuffer the firmware set up.
  virtual bool gpu_location(PciAddress* out) const { (void)out; return false; }
  // Microcode the engine needs from "/firmware" on the OS file system, read once the file system is mounted (it is not yet when the devices are made).
  // Returns true if it is now loaded; firmware_status() says what happened ("" when this device needs none). Nothing is started: loading is reading the
  // file and checking it.
  virtual bool load_firmware(filesystem::IFileSystem& file_system) { (void)file_system; return false; }
  virtual const char* firmware_status() const { return ""; }
  // Works out, without writing to the card, where the engine's memory would go and what address configuration it needs, and maps that memory. True if it did.
  // plan_status() says what came of it ("" when the device has nothing to plan). Needs the firmware loaded first, since its size sets the size of the block.
  virtual bool plan_engine(const VideoEngineEnvironment& environment) { (void)environment; return false; }
  virtual const char* plan_status() const { return ""; }
  // Brings the engine up if everything checked out (a plan, and the hardware agreeing with it), leaving it idle; true if it is running. firmware_status() says
  // what came of it. The device stops it again in shutdown(). Nothing decodes on a started engine unless info() says so.
  virtual bool start_hardware(const IClock& clock) { (void)clock; return false; }
  virtual uint64_t gpu_memory_bytes() const { return 0; }

 protected:
  constexpr IVideoCodecDevice() = default;
  ~IVideoCodecDevice() = default;
};

}  // namespace hardware

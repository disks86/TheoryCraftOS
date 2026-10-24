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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/Intel/IntelAvcSession.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaEngineHw.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Intel's media engine in its graphics (Quick Sync Video: the VDBOX/MFX fixed-function blocks): the video decode/encode block that comes with the GPU (PCI vendor 0x8086).
// It reports that the engine is there (info().hardware_engine) and what a look at it found; sessions run on the CPU through the software codecs unless the engine is driven.
// With set_hardware() before initialize() (SystemVideoCodecs does it by default, and set_intel_hardware_decode(false) stops it), on a Gen9.5 GPU whose video box answers, H.264 decoding runs on
// the engine (VcsDriver, IntelAvcDecoder) - one session at a time, the others (and every other codec, and encoding) stay on the CPU. That path has only been run against a simulated GPU.
class IntelMediaVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  // `gpu_device_id` is the PCI device ID of the GPU the engine belongs to; it picks the generation's codec set (0 = unknown).
  // `registers` is the GPU's register BAR (at least IntelMediaEngineHw::kRegisterWindow bytes), or an empty window to skip looking at the hardware.
  explicit IntelMediaVideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id = 0, const MmioRegion& registers = MmioRegion())
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), allocator_(allocator), gpu_(intel_gpu_identify(gpu_device_id)), registers_(registers) {}

  // What the engine driver needs: the GPU's registers (the whole first 2 MiB of the register BAR), the global GTT table, a range of eight of its entries nothing
  // uses, and memory the GPU can reach.
  struct HardwareSetup {
    intel_gt::IRegisterBank* registers = nullptr;
    MmioRegion ggtt_table;
    uint64_t ggtt_first_page = 0;
    intel_gt::IGpuMemory* memory = nullptr;
  };
  // Opts in to running H.264 decoding on the engine. Call before initialize(); everything in `setup` must outlive this device.
  void set_hardware(const HardwareSetup& setup) { setup_ = setup; }
  // Is H.264 decoding running on the engine?
  bool hardware_decode() const { return driver_ && driver_->state() == intel_gt::VcsDriver::State::Ready; }

  // Looks at the video boxes (which exist, which wake up) and, if the owner opted in and the generation is known, starts the engine driver.
  bool initialize() override;
  // Stops the engine driver. Every session made by create_decoder() must be closed first.
  void shutdown() override { driver_.reset(); info_.accelerated_decode_codecs = 0; }
  UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config) override;
  const IntelMediaEngineStatus& engine_status() const { return status_; }
  const char* engine_summary() const override { return summary_.c_str(); }

  const char* name() const override { return "Intel media engine (Quick Sync) (software path)"; }
  const IntelGpuInfo& gpu() const { return gpu_; }
  // Codecs the engine would serve once it is driven: today's sessions all run in software (info().accelerated is false).
  uint32_t engine_decode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0) |
           (gpu_.media_av1_decode ? codec_mask(VideoCodec::Av1) : 0);
  }
  uint32_t engine_encode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_av1_encode ? codec_mask(VideoCodec::Av1) : 0);
  }

 private:
  // H.264 and JPEG are on every generation that has a media engine at all.
  uint32_t base_codecs() const {
    const bool has_engine = gpu_.architecture != IntelGpuArchitecture::Unknown && gpu_.architecture != IntelGpuArchitecture::XeHpc;
    return has_engine ? codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mjpeg) : 0;
  }

  allocators::IAllocator& allocator_;
  IntelGpuInfo gpu_;
  MmioRegion registers_;
  IntelMediaEngineStatus status_;
  FixedString<120> summary_;
  HardwareSetup setup_;
  UniquePointer<intel_gt::VcsDriver> driver_;
};

}  // namespace hardware

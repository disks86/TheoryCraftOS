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
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/NVIDIA/NvidiaMediaEngineHw.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// NVIDIA's NVDEC / NVENC engines: the video decode/encode block that comes with the GPU (PCI vendor 0x10DE). The sessions
// still run on the CPU, through the software codecs, until a native driver for the engine exists - this class
// is where it will go, and what it reports now is that the engine is there (info().hardware_engine) and not used
// (info().accelerated is false). H.264, H.265, VP9 and AV1 will be served here once the engine is driven.
class NvidiaVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  // `gpu_device_id` is the PCI device ID of the GPU the engine belongs to; it picks the generation's codec set (0 = unknown). `registers` is the start of the
  // GPU's register BAR (at least NvidiaMediaEngineHw::kRegisterWindow bytes), or an empty window to skip looking at the hardware.
  explicit NvidiaVideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id = 0, const MmioRegion& registers = MmioRegion())
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), gpu_(nvidia_gpu_identify(gpu_device_id)), registers_(registers) {}

  // Reads the chip ID. What the chip says about itself outranks the device ID table: it names the architecture of a card the table does not know, corrects a
  // table entry that is wrong, and tells the chips that have no NVENC. Always succeeds: the engine is there either way.
  bool initialize() override {
    status_ = NvidiaMediaEngineHw::probe(registers_);
    if (status_.probed) {
      if (status_.architecture != gpu_.architecture) gpu_ = nvidia_gpu_info_for(status_.architecture);
      if (!status_.has_nvenc) gpu_.has_nvenc = gpu_.media_av1_encode = false;
    }
    summary_.assign(gpu_.name);
    if (status_.probed) {
      summary_.append(", chip ");
      summary_.append(status_.chip ? status_.chip : "(unnamed)");
      summary_.append(" rev ");
      if (status_.stepping_major >= 0xA) summary_.push_back(static_cast<char>('A' + (status_.stepping_major - 0xA)));
      else summary_.append_uint(status_.stepping_major);
      summary_.append_uint(status_.stepping_minor);
      summary_.append(status_.has_nvenc ? ", NVDEC+NVENC" : ", NVDEC only");
    } else {
      summary_.append(", chip ID not read");
    }
    return true;
  }
  const NvidiaChipStatus& chip_status() const { return status_; }
  const char* engine_summary() const override { return summary_.c_str(); }

  const char* name() const override { return "NVIDIA video engine (NVDEC/NVENC) (software path)"; }
  const NvidiaGpuInfo& gpu() const { return gpu_; }
  // Codecs the engine would serve once it is driven: today's sessions all run in software (info().accelerated is false).
  uint32_t engine_decode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0) |
           (gpu_.media_av1_decode ? codec_mask(VideoCodec::Av1) : 0);
  }
  uint32_t engine_encode_codecs() const {
    if (!gpu_.has_nvenc) return 0;
    return base_codecs() | codec_mask(VideoCodec::Hevc) | (gpu_.media_av1_encode ? codec_mask(VideoCodec::Av1) : 0);
  }

 private:
  // H.264 and JPEG are on every generation that has a video engine at all.
  uint32_t base_codecs() const {
    return nvidia_gpu_has_video_engine(gpu_.architecture) ? codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mjpeg) : 0;
  }

  NvidiaGpuInfo gpu_;
  MmioRegion registers_;
  NvidiaChipStatus status_;
  FixedString<120> summary_;
};

}  // namespace hardware

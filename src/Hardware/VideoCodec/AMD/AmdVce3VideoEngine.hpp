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
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdSmu7.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3Encoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The H.264 encoder of the Radeon RX Vega M (VCE 3.4), brought up the way AmdUvd6VideoEngine brings up the decoder: the microcode from /firmware, a block of the card's video
// memory the caller gives it (the AmdUvd6VideoEngine carves it from the end of its work area, so the two engines never share memory), the SMC's power-on, the start
// sequence (AmdVce3), the ring's own test (AmdVce3Ring) and an encode self-test: three pictures (an IDR and two P pictures) go through the engine and come back through the
// CPU's H.264 decoder, and only when each is close to the source (the mean squared error of the luma at most kMaxSelfTestError) does it serve sessions (AmdVce3Encoder).
// Anything else leaves the encoder on the CPU and the engine stopped. The engine serves one session at a time.
//
// The clock: after the SMC's power-on the VCE's DPM is switched on at the top level (AmdSmu7::vce_dpm_enable: the boot level in the SMC's DPM table, the level mask, the
// enable message - the order of the Linux driver's smu7_powergate_vce), and switched off again before the power-off. set_clock_level() picks the lowest instead. An SMC that
// does not answer, or whose DPM table cannot be found, leaves the engine at whatever clock the card was left with; the status line says so. Nothing here has run on a
// real card, and it is not known whether the system firmware leaves the SMC's DPM running (the driver enables it itself at load, which this OS does not); the sessions that
// hold a pointer to the ring must be closed before stop().
class AmdVce3VideoEngine {
 public:
  // The memory the engine is given: a run of the card's video memory, as the GPU (`gpu_address`) and the CPU (`physical`, through the memory BAR) address it. MiB aligned.
  struct Region {
    uint64_t gpu_address = 0;
    uint64_t physical = 0;
    uint64_t bytes = 0;
  };
  static constexpr uint64_t kMiB = 1ull << 20;
  static constexpr uint64_t kRingAreaBytes = kMiB;      // three rings, the indirect buffer and the fence, a page each
  static constexpr uint64_t kWorkBytes = 16 * kMiB;     // the sessions' pictures, coded picture buffer, bitstream and feedback
  static constexpr uint32_t kMaxSelfTestError = 160;    // mean squared luma error between the source and the decoded picture (about 26 dB)
  static constexpr uint32_t kBootAttempts = 3;

  // The VCE clock level asked of the SMC (before start()).
  void set_clock_level(AmdSmu7::VceLevel level) { clock_level_ = level; }
  AmdSmu7::VceLevel clock_level() const { return clock_level_; }
  // The level the SMC was asked for, or -1 when no request was acknowledged.
  int requested_level() const { return dpm_enabled_ ? static_cast<int>(dpm_level_) : -1; }

  // What a Region must hold: the block, the ring area and the work memory.
  static constexpr uint64_t reserve_bytes() { return (AmdVce3::kBlockBytes + kMiB - 1) / kMiB * kMiB + kRingAreaBytes + kWorkBytes; }

  AmdVce3VideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id) : allocator_(allocator), device_id_(gpu_device_id), firmware_(allocator) {}

  // The microcode from the file system (the file the amdgpu driver names for this card). False when the card has none or the file is missing or unusable.
  bool load_firmware(filesystem::IFileSystem& file_system) {
    const char* name = amd_vce3_firmware_name(device_id_);
    if (!name) return false;
    firmware_name_ = name;
    const AmdFirmwareStatus state = firmware_.load(file_system, name);
    if (state != AmdFirmwareStatus::Ok) {
      text_.assign(name);
      text_.append(": ");
      text_.append(amd_firmware_status_name(state));
      return false;
    }
    if (!AmdVce3::firmware_usable(firmware_)) {
      text_.assign(name);
      text_.append(": not a VCE 3.x image the engine's memory window holds");
      return false;
    }
    text_.assign("VCE microcode loaded, not started");
    return true;
  }
  bool firmware_usable() const { return AmdVce3::firmware_usable(firmware_); }

  // Maps the block, the rings and the work memory inside `region` (at least reserve_bytes()). `registers` and `mapper` must outlive the engine.
  bool plan(IVcnRegisters& registers, IRegisterMapper& mapper, const Region& region) {
    planned_ = false;
    if (!firmware_usable()) return false;
    const uint64_t block_bytes = (AmdVce3::kBlockBytes + kMiB - 1) / kMiB * kMiB;
    if ((region.gpu_address | region.physical | region.bytes) % kMiB != 0 || region.bytes < reserve_bytes() || region.physical > UINT64_MAX - region.bytes || region.gpu_address > UINT64_MAX - region.bytes) {
      text_.assign("VCE memory not planned: no room");
      return false;
    }
    MmioRegion block, rings;
    if (!mapper.map_registers(region.physical, block_bytes, &block) || !mapper.map_registers(region.physical + block_bytes, kRingAreaBytes, &rings)) {
      text_.assign("VCE memory not planned: the card's memory cannot be mapped");
      return false;
    }
    memory_ = AmdVce3::Memory();
    memory_.block_gpu_address = region.gpu_address;
    memory_.block = block;
    const uint64_t ring_gpu = region.gpu_address + block_bytes;
    for (uint32_t i = 0; i < AmdVce3::kMaxRings; ++i) {
      memory_.ring_gpu_address[i] = ring_gpu + uint64_t{i} * AmdVce3::kRingBytes;
      memory_.ring[i] = rings.subregion(uint64_t{i} * AmdVce3::kRingBytes, AmdVce3::kRingBytes);
    }
    ring_memory_.ib_gpu_address = ring_gpu + uint64_t{AmdVce3::kMaxRings} * AmdVce3::kRingBytes;
    ring_memory_.ib = rings.subregion(uint64_t{AmdVce3::kMaxRings} * AmdVce3::kRingBytes, AmdVce3Ring::kIndirectBufferBytes);
    ring_memory_.fence_gpu_address = ring_memory_.ib_gpu_address + AmdVce3Ring::kIndirectBufferBytes;
    ring_memory_.fence = rings.subregion(uint64_t{AmdVce3::kMaxRings} * AmdVce3::kRingBytes + AmdVce3Ring::kIndirectBufferBytes, AmdVce3Ring::kFenceBytes);
    ring_memory_.ring = memory_.ring[0];
    const uint64_t work_offset = block_bytes + kRingAreaBytes;
    if (!work_.configure(&mapper, region.gpu_address + work_offset, region.physical + work_offset, region.bytes - work_offset)) {
      text_.assign("VCE memory not planned: bad work memory");
      return false;
    }
    registers_ = &registers;
    planned_ = true;
    text_.assign("VCE memory planned, not started");
    return true;
  }
  bool planned() const { return planned_; }

  // Powers and starts the engine, runs the ring's test and the encode self-test. True when sessions run on the engine now; a failure leaves it stopped, and status() says why.
  bool start(const IClock& clock) {
    if (ready_) return true;
    if (!planned_ || !registers_) return false;
    clock_ = &clock;
    vce_ = UniquePointer<AmdVce3>::make(allocator_, *registers_, clock);
    if (!vce_) return false;
    AmdVce3::Config config;
    config.harvest_config = AmdVce3::read_harvest_config(*registers_);
    config.rings = AmdVce3::rings_for(AmdVce3::firmware_version(firmware_));
    config.boot_attempts = kBootAttempts;
    harvest_ = config.harvest_config;
    request_power(clock);
    const AmdVce3::Status status = vce_->start(firmware_, memory_, config);
    if (status != AmdVce3::Status::Ok) {
      text_.assign(status == AmdVce3::Status::TimedOut ? "VCE start failed: the engine did not answer" :
                   status == AmdVce3::Status::NoEngine ? "VCE start refused: both instances are fused off" :
                   status == AmdVce3::Status::BadMemory ? "VCE start refused: bad memory block" : "VCE start refused: unusable microcode");
      fail();
      return false;
    }
    ring_ = UniquePointer<AmdVce3Ring>::make(allocator_, *registers_, clock, ring_memory_, 0, harvest_);
    if (!ring_) {
      text_.assign("VCE not enabled: out of memory");
      fail();
      return false;
    }
    if (!ring_->test()) {
      text_.assign("VCE not enabled: the ring did not run a command");
      fail();
      return false;
    }
    const char* why = self_test();
    if (why) {
      text_.assign("VCE encode self-test failed (");
      text_.append(why);
      text_.append("); engine stopped");
      fail();
      return false;
    }
    ready_ = true;
    text_.assign(smc_note_.view());
    text_.append(": VCE started; H.264 encode runs on it (self-test passed)");
    return true;
  }

  // Puts the engine to sleep. Sessions must be closed first.
  void stop() {
    ready_ = false;
    ring_.reset();
    if (vce_) {
      vce_->stop();
      vce_.reset();
    }
    release_power();
    if (planned_) text_.assign("VCE stopped");
  }
  bool ready() const { return ready_; }
  bool wedged() const { return ring_ && ring_->wedged(); }
  const char* status() const { return text_.c_str(); }
  const AmdVcnFirmware& firmware() const { return firmware_; }
  AmdVce3Ring* ring() { return ring_.get(); }
  AmdVcnWorkMemory& work_memory() { return work_; }

  // An H.264 session on the engine, or none (not ready, a stalled ring, the engine busy with another session, a configuration it does not take).
  UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config) {
    if (!ready_ || !ring_ || ring_->wedged() || config.codec != VideoCodec::H264) return UniquePointer<IVideoEncoder>();
    AmdVce3Encoder::Engine engine;
    engine.ring = ring_.get();
    engine.memory = &work_;
    engine.stream_handle = ++next_stream_handle_;
    UniquePointer<AmdVce3Encoder> session = UniquePointer<AmdVce3Encoder>::make(allocator_, allocator_, engine, config);
    if (session && session->prepare()) return UniquePointer<IVideoEncoder>(util::move(session));
    return UniquePointer<IVideoEncoder>();
  }

 private:
  // The self-test picture: a gradient with a square that moves.
  static void paint(VideoFrameBuffer* frame, uint32_t width, uint32_t height, uint32_t picture) {
    ByteView y = frame->plane(0), u = frame->plane(1), v = frame->plane(2);
    for (uint32_t r = 0; r < height; ++r)
      for (uint32_t c = 0; c < width; ++c) {
        uint32_t value = 40 + c * 2 + r;
        const uint32_t left = 8 + picture * 2, top = 12 + picture;
        if (c >= left && c < left + 20 && r >= top && r < top + 20) value = 200 - (c - left) * 3;
        y[uint64_t{r} * width + c] = static_cast<uint8_t>(value);
      }
    for (uint32_t r = 0; r < height / 2; ++r)
      for (uint32_t c = 0; c < width / 2; ++c) {
        u[uint64_t{r} * (width / 2) + c] = static_cast<uint8_t>(96 + c * 2 + picture);
        v[uint64_t{r} * (width / 2) + c] = static_cast<uint8_t>(160 - r * 2);
      }
  }

  // Three pictures through the engine and the CPU decoder. nullptr when each came back close to what went in, else what went wrong.
  const char* self_test() {
    constexpr uint32_t kWidth = 64, kHeight = 48, kPictures = 3;
    EncoderConfig config;
    config.codec = VideoCodec::H264;
    config.width = kWidth;
    config.height = kHeight;
    config.input = PixelFormat::I420;
    config.quality = 75;
    config.keyframe_interval = kPictures;
    AmdVce3Encoder::Engine engine;
    engine.ring = ring_.get();
    engine.memory = &work_;
    engine.stream_handle = ++next_stream_handle_;
    AmdVce3Encoder session(allocator_, engine, config);
    if (!session.prepare()) return "session refused";
    H264Decoder decoder(allocator_);
    VideoFrameBuffer source(allocator_);
    if (!source.allocate({kWidth, kHeight, PixelFormat::I420})) return "out of memory";
    for (uint32_t i = 0; i < kPictures; ++i) {
      paint(&source, kWidth, kHeight, i);
      if (session.submit(source.frame(int64_t{i}, true)) != SubmitResult::Accepted || ring_->wedged()) return ring_->wedged() ? "the engine stopped answering" : "picture refused";
      VideoPacket packet;
      if (!session.receive(&packet) || packet.data.size() == 0) return "no bitstream";
      if (!decoder.decode(packet.data, packet.pts_us)) return "undecodable bitstream";
      VideoFrame out;
      if (!decoder.receive(&out)) {
        decoder.flush();
        if (!decoder.receive(&out)) return "no picture decoded";
      }
      const VideoFrame in = source.frame(0, true);
      uint64_t sum = 0;
      for (uint32_t r = 0; r < kHeight; ++r)
        for (uint32_t c = 0; c < kWidth; ++c) {
          const int d = int{in.plane[0][uint64_t{r} * in.stride[0] + c]} - int{out.plane[0][uint64_t{r} * out.stride[0] + c]};
          sum += static_cast<uint64_t>(d * d);
        }
      if (sum / (kWidth * kHeight) > kMaxSelfTestError) return "decoded picture differs from the source";
    }
    return nullptr;
  }

  void fail() {
    ready_ = false;
    ring_.reset();
    if (vce_) {
      vce_->stop();
      vce_.reset();
    }
    release_power();
  }

  // The SMC powers the engine before the driver touches it. The start goes on whatever the answer (an SMC that is not running means the system firmware left the card as it was).
  void request_power(const IClock& clock) {
    AmdSmu7 smc(*registers_, clock);
    switch (smc.vce_power(true)) {
      case AmdSmu7::Result::Ok: smc_powered_ = true; smc_note_.assign("SMC powered the engine"); break;
      case AmdSmu7::Result::NotAnswering: smc_note_.assign("SMC did not answer, engine assumed powered"); return;
      case AmdSmu7::Result::Unsupported: smc_note_.assign("SMC does not know the power-on message, engine assumed powered"); break;
      case AmdSmu7::Result::Failed: smc_note_.assign("SMC refused to power the engine"); break;
    }
    // The clock, once the SMC is there to ask.
    uint32_t level = 0;
    switch (smc.vce_dpm_enable(clock_level_, &level)) {
      case AmdSmu7::DpmResult::Ok:
        dpm_enabled_ = true;
        dpm_level_ = level;
        smc_note_.append(", clock level ");
        smc_note_.append_uint(level);
        break;
      case AmdSmu7::DpmResult::NoDpmTable:
      case AmdSmu7::DpmResult::BadDpmTable: smc_note_.append(", SMC's DPM table not found, clock as left"); break;
      case AmdSmu7::DpmResult::NotAnswering:
      case AmdSmu7::DpmResult::Unsupported:
      case AmdSmu7::DpmResult::Failed: smc_note_.append(", SMC refused the clock request, clock as left"); break;
    }
  }
  void release_power() {
    if (!registers_ || !clock_) return;
    AmdSmu7 smc(*registers_, *clock_);
    if (dpm_enabled_) smc.vce_dpm_disable();   // (before the power goes, as the driver's smu7_powergate_vce(true) does)
    dpm_enabled_ = false;
    if (smc_powered_) smc.vce_power(false);
    smc_powered_ = false;
  }

  allocators::IAllocator& allocator_;
  uint16_t device_id_ = 0;
  AmdVcnFirmware firmware_;
  const char* firmware_name_ = nullptr;
  IVcnRegisters* registers_ = nullptr;
  const IClock* clock_ = nullptr;
  AmdVce3::Memory memory_;
  AmdVce3Ring::Memory ring_memory_;
  AmdVcnWorkMemory work_;
  UniquePointer<AmdVce3> vce_;
  UniquePointer<AmdVce3Ring> ring_;
  uint32_t harvest_ = AmdVce3::kHarvestVce1;
  bool planned_ = false, ready_ = false, smc_powered_ = false, dpm_enabled_ = false;
  AmdSmu7::VceLevel clock_level_ = AmdSmu7::VceLevel::Highest;
  uint32_t dpm_level_ = 0;
  FixedString<127> smc_note_;
  uint32_t next_stream_handle_ = 0;
  FixedString<255> text_;
};

}  // namespace hardware

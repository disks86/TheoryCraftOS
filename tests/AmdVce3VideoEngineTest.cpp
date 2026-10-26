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

// Host-side tests for the Vega M's H.264 encoder engine (AmdVce3VideoEngine) against a simulated card (FakeAmdVce3.hpp): the firmware from a file system (the real
// firmware/vegam_vce.bin), planning inside a region of "video memory" that is host memory behind a mapper, the SMC's power messages, the start sequence, the ring test, the
// encode self-test (three pictures through a stand-in for the engine's firmware and back through the CPU decoder) and the sessions the engine then serves - and every way
// it can be refused: no firmware, a region that is too small or that cannot be mapped, an engine that does not boot, a ring that does not run, a firmware that writes a
// damaged bitstream.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6VideoEngine.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3VideoEngine.hpp"
#include "FakeAmdVce3.hpp"

using namespace hardware;
using namespace fakevce;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::vector<uint8_t> read_host_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

static bool firmware_fs(filesystem::RamFileSystem& fs, bool with_file) {
  if (fs.create_directory("/firmware") != filesystem::Status::Ok) return false;
  if (!with_file) return true;
  const std::vector<uint8_t> image = read_host_file("firmware/vegam_vce.bin");
  UniquePointer<filesystem::IFile> file;
  if (image.empty() || fs.open_file("/firmware/vegam_vce.bin", filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok) return false;
  uint64_t put = 0;
  return file->write(0, ConstByteView(image.data(), image.size()), &put) == filesystem::Status::Ok && put == image.size();
}

constexpr uint64_t kGpuBase = 0x4'0000'0000ull, kPhysicalBase = 0xE000'0000ull;
constexpr uint64_t kRegionBytes = AmdVce3VideoEngine::reserve_bytes();

struct Rig {
  TestAllocator alloc;
  std::vector<uint8_t> arena;
  FakeMapper mapper;
  Card card;
  Firmware fw;
  fakevcn::FakeClock clock;
  AmdVce3VideoEngine::Region region;
  std::unique_ptr<filesystem::RamFileSystem> fs;
  std::unique_ptr<AmdVce3VideoEngine> engine;
  explicit Rig(uint64_t arena_bytes = kRegionBytes) : arena(arena_bytes) {
    mapper.host = arena.data();
    mapper.physical_base = kPhysicalBase;
    mapper.size = arena.size();
    region.gpu_address = kGpuBase;
    region.physical = kPhysicalBase;
    region.bytes = arena.size() / (1 << 20) * (1 << 20);
    card.windows.push_back({kGpuBase, arena.data(), arena.size()});
    // The rings live behind the block, which is rounded up to whole MiB.
    card.ring_gpu = kGpuBase + (AmdVce3::kBlockBytes + (1 << 20) - 1) / (1 << 20) * (1 << 20);
    card.set_pointers(0);
    card.block_gpu = kGpuBase;
    card.block_host = arena.data();
    fw.ctrl = &card;
    fw.alloc = &alloc;
    card.on_ib = [this](const std::vector<uint32_t>& w) { fw.on_ib(w); };
    fs.reset(new filesystem::RamFileSystem(alloc));
    engine.reset(new AmdVce3VideoEngine(alloc, 0x694C));
  }
  bool load() {
    if (!firmware_fs(*fs, true) || !engine->load_firmware(*fs)) return false;
    memcpy(&card.expected_first_word, engine->firmware().ucode().data(), 4);
    return true;
  }
};

static EncoderConfig config(uint32_t width = 64, uint32_t height = 48) {
  EncoderConfig c;
  c.codec = VideoCodec::H264;
  c.width = width;
  c.height = height;
  c.input = PixelFormat::I420;
  c.keyframe_interval = 5;
  return c;
}

static std::vector<uint32_t> messages(const Rig& rig) { return rig.card.smc_messages; }

static void test_start_and_sessions() {
  Rig rig;
  CHECK(rig.load());
  CHECK(strstr(rig.engine->status(), "loaded"));
  CHECK(rig.engine->plan(rig.card, rig.mapper, rig.region) && rig.engine->planned());
  CHECK(!rig.engine->ready() && !rig.engine->create_encoder(config()));
  CHECK(rig.engine->start(rig.clock) && rig.engine->ready() && !rig.engine->wedged());
  CHECK(strstr(rig.engine->status(), "H.264 encode runs on it"));
  // The SMC was asked to power the engine, once, before the start; the self-test used three pictures and left the engine free.
  CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x12e, 0x15a}));
  // ... then the clock: the top of the four levels is the boot level, and the only one enabled.
  CHECK(rig.card.smc_arguments[1] == 8 && rig.card.boot_level() == 3 && rig.engine->requested_level() == 3);
  CHECK(rig.card.sram[Card::kBootLevelWord] == 0xAA03CCDDu && strstr(rig.engine->status(), "clock level 3"));
  CHECK(rig.fw.encodes == 3 && rig.fw.creates == 1 && rig.fw.destroys == 1 && rig.fw.problems == 0 && rig.card.protocol_errors == 0);
  CHECK(rig.card.boots >= 1);
  {
    UniquePointer<IVideoEncoder> session = rig.engine->create_encoder(config());
    CHECK(session);
    // One session at a time.
    CHECK(!rig.engine->create_encoder(config()));
    VideoFrameBuffer src(rig.alloc);
    CHECK(src.allocate({64, 48, PixelFormat::I420}));
    for (uint32_t i = 0; i < 64 * 48; ++i) src.plane(0)[i] = static_cast<uint8_t>(i);
    for (uint32_t i = 0; i < 32 * 24; ++i) { src.plane(1)[i] = 100; src.plane(2)[i] = 150; }
    VideoPacket packet;
    CHECK(session->submit(src.frame(0, true)) == SubmitResult::Accepted && session->receive(&packet) && packet.keyframe && packet.data.size() > 0);
    CHECK(rig.fw.encodes == 4);
  }
  // The session closed: the next one is served, with another handle. Other codecs and sizes the engine does not take are refused.
  CHECK(rig.engine->create_encoder(config()));
  EncoderConfig other = config();
  other.codec = VideoCodec::Mjpeg;
  CHECK(!rig.engine->create_encoder(other));
  CHECK(!rig.engine->create_encoder(config(63, 48)));
  rig.engine->stop();
  CHECK(!rig.engine->ready() && !rig.engine->create_encoder(config()));
  CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x12e, 0x15a, 0x15b, 0x10e}) && rig.engine->requested_level() == -1);   // DPM off, then the power
  // A stopped engine starts again.
  CHECK(rig.engine->start(rig.clock) && rig.engine->ready() && rig.card.smc_messages.size() == 8 && rig.card.smc_messages[5] == 0x10f);
  CHECK(rig.engine->create_encoder(config()));
  rig.engine->stop();
  rig.fw.encoder.reset();
  rig.engine.reset();
  rig.fs.reset();
  CHECK(rig.alloc.live == 0);
}

// The clock request: the lowest level when asked for, and an SMC whose memory holds no DPM table is left alone (the engine still starts).
static void test_clock_levels() {
  {
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.engine->set_clock_level(AmdSmu7::VceLevel::Lowest);
    CHECK(rig.engine->start(rig.clock) && rig.engine->requested_level() == 0);
    CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x15a}) && rig.card.boot_level() == 0 && strstr(rig.engine->status(), "clock level 0"));
  }
  {
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.sram[0x20000 + 52] = 0;   // the SMC's firmware is not running
    CHECK(rig.engine->start(rig.clock) && rig.engine->ready() && rig.engine->requested_level() == -1 && strstr(rig.engine->status(), "DPM table not found"));
    rig.engine->stop();
    CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x10e}));
  }
  {  // A level count nobody has: no request, the engine runs.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.sram[Card::kLevelCountWord] = 0x11000033u | (0u << 16);
    CHECK(rig.engine->start(rig.clock) && rig.engine->requested_level() == -1 && rig.card.boot_level() == 0);
  }
}

static void test_planning_refusals() {
  {  // No firmware file.
    Rig rig;
    CHECK(firmware_fs(*rig.fs, false) && !rig.engine->load_firmware(*rig.fs));
    CHECK(!rig.engine->plan(rig.card, rig.mapper, rig.region) && !rig.engine->start(rig.clock));
    CHECK(rig.card.smc_messages.empty());
  }
  {  // A card with no VCE firmware.
    Rig rig;
    AmdVce3VideoEngine other(rig.alloc, 0x15DD);
    CHECK(firmware_fs(*rig.fs, true) && !other.load_firmware(*rig.fs));
  }
  {  // A region that is too small, or not whole MiB.
    Rig rig;
    CHECK(rig.load());
    AmdVce3VideoEngine::Region small = rig.region;
    small.bytes -= 1 << 20;
    CHECK(!rig.engine->plan(rig.card, rig.mapper, small) && !rig.engine->planned() && !rig.engine->start(rig.clock));
    AmdVce3VideoEngine::Region odd = rig.region;
    odd.physical += 4096;
    CHECK(!rig.engine->plan(rig.card, rig.mapper, odd));
    AmdVce3VideoEngine::Region wrap = rig.region;
    wrap.gpu_address = UINT64_MAX - (1 << 20) + 1;
    CHECK(!rig.engine->plan(rig.card, rig.mapper, wrap));
  }
  {  // The mapper refuses (the OS owns the memory).
    Rig rig;
    CHECK(rig.load());
    rig.mapper.refuse = true;
    CHECK(!rig.engine->plan(rig.card, rig.mapper, rig.region) && strstr(rig.engine->status(), "cannot be mapped"));
  }
}

static void test_start_refusals() {
  {  // An engine that never boots: nothing is left running, the SMC is told to power it down again.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.dead = true;
    CHECK(!rig.engine->start(rig.clock) && !rig.engine->ready() && strstr(rig.engine->status(), "did not answer"));
    CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x12e, 0x15a, 0x15b, 0x10e}));
    CHECK(!rig.engine->create_encoder(config()) && rig.fw.encodes == 0);
  }
  {  // No SMC answering: the start goes on, and nothing is powered down at stop.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.smc_running = false;
    CHECK(rig.engine->start(rig.clock) && rig.engine->ready() && rig.engine->requested_level() == -1);
    rig.engine->stop();
    CHECK(messages(rig) == std::vector<uint32_t>({0x10f}) && rig.card.boot_level() == 0);   // (one message, no answer: nothing else asked)
  }
  {  // The SMC does not know the message.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.smc_result = 0xFE;
    CHECK(rig.engine->start(rig.clock) && rig.engine->ready() && rig.engine->requested_level() == -1);
    rig.engine->stop();
    CHECK(messages(rig) == std::vector<uint32_t>({0x10f, 0x12e}) && strstr(rig.engine->status(), "VCE stopped"));
  }
  {  // A fence that never comes: the self-test's first job times out.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.card.stall = true;
    CHECK(!rig.engine->start(rig.clock) && !rig.engine->ready() && strstr(rig.engine->status(), "self-test failed"));
    CHECK(!rig.card.smc_messages.empty() && rig.card.smc_messages.back() == 0x10e && rig.card.smc_messages[rig.card.smc_messages.size() - 2] == 0x15b && !rig.engine->create_encoder(config()));
  }
  {  // A firmware that writes a damaged bitstream.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.fw.corrupt = true;
    CHECK(!rig.engine->start(rig.clock) && !rig.engine->ready() && strstr(rig.engine->status(), "self-test failed"));
    CHECK(!rig.engine->create_encoder(config()));
    // The same engine, mended, starts.
    rig.fw.corrupt = false;
    rig.fw.encoder.reset();
    CHECK(rig.engine->start(rig.clock) && rig.engine->ready());
  }
  {  // A firmware that encodes another picture: the bitstream decodes, but not to the source.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.fw.wrong_picture = true;
    CHECK(!rig.engine->start(rig.clock) && !rig.engine->ready() && strstr(rig.engine->status(), "differs from the source"));
    CHECK(!rig.engine->create_encoder(config()));
  }
  {  // A firmware that reports no bitstream.
    Rig rig;
    CHECK(rig.load() && rig.engine->plan(rig.card, rig.mapper, rig.region));
    rig.fw.no_bitstream = true;
    CHECK(!rig.engine->start(rig.clock) && !rig.engine->ready());
  }
  {  // Not planned: no start, no SMC traffic.
    Rig rig;
    CHECK(rig.load());
    CHECK(!rig.engine->start(rig.clock) && rig.card.smc_messages.empty());
  }
}

// The whole device: AmdUvd6VideoEngine with the encoder enabled, on a card whose memory controller registers say 128 MiB of video memory behind a 128 MiB aperture. (The UVD
// side has no simulated engine here, so its start fails; the encoder does not depend on it.)
static bool put_host_file(filesystem::RamFileSystem& fs, const char* host, const char* path) {
  const std::vector<uint8_t> image = read_host_file(host);
  UniquePointer<filesystem::IFile> file;
  if (image.empty() || fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok) return false;
  uint64_t put = 0;
  return file->write(0, ConstByteView(image.data(), image.size()), &put) == filesystem::Status::Ok && put == image.size();
}

static void test_device(bool encode) {
  constexpr uint64_t kVideoMemory = 128ull << 20;
  TestAllocator alloc;
  std::vector<uint8_t> arena(kVideoMemory);
  FakeMapper mapper;
  mapper.host = arena.data();
  mapper.physical_base = kPhysicalBase;
  mapper.size = arena.size();
  Card card;
  card.windows.push_back({kGpuBase, arena.data(), arena.size()});
  card.global[0x150A] = static_cast<uint32_t>(kVideoMemory >> 20);                                               // CONFIG_MEMSIZE
  card.global[0x809] = static_cast<uint32_t>(kGpuBase >> 24) | (static_cast<uint32_t>(((kGpuBase + kVideoMemory) >> 24) - 1) << 16);   // MC_VM_FB_LOCATION
  card.global[0x263e] = 0x22011003;                                                                              // GB_ADDR_CONFIG
  fakevcn::FakeClock clock;
  Firmware fw;
  fw.ctrl = &card;
  fw.alloc = &alloc;
  card.on_ib = [&](const std::vector<uint32_t>& w) { fw.on_ib(w); };
  filesystem::RamFileSystem fs(alloc);
  CHECK(fs.create_directory("/firmware") == filesystem::Status::Ok);
  CHECK(put_host_file(fs, "firmware/vegam_uvd.bin", "/firmware/vegam_uvd.bin") && put_host_file(fs, "firmware/vegam_vce.bin", "/firmware/vegam_vce.bin"));
  {
    AmdUvd6VideoEngine device(alloc, 0x694C);
    device.set_register_file(card);
    device.set_aperture(kPhysicalBase, kVideoMemory);
    device.enable_encode(encode);
    CHECK(device.encode_enabled() == encode);
    CHECK(device.load_firmware(fs));
    VideoEngineEnvironment environment;
    environment.mapper = &mapper;
    CHECK(device.plan_engine(environment));
    if (encode) {
      CHECK(strstr(device.plan_status(), "encoder: 19 MiB"));
      // The encoder's memory is the end of the work area: the card must know where the block went.
      const uint64_t vce_gpu = device.plan().work_gpu_address + device.plan().work_bytes;
      // ... and the decoders' work area ends where it starts, inside video memory (short of the tail guard), so the two never share memory.
      CHECK(vce_gpu + AmdVce3VideoEngine::reserve_bytes() <= kGpuBase + kVideoMemory - kAmdUvd6TailGuardBytes);
      card.block_gpu = vce_gpu;
      card.block_host = arena.data() + (vce_gpu - kGpuBase);
      card.ring_gpu = vce_gpu + (AmdVce3::kBlockBytes + (1 << 20) - 1) / (1 << 20) * (1 << 20);
      memcpy(&card.expected_first_word, device.encoder_engine()->firmware().ucode().data(), 4);
    } else {
      CHECK(!strstr(device.plan_status(), "encoder"));
    }
    card.set_pointers(0);
    device.start_hardware(clock);
    CHECK(device.encode_ready() == encode);
    CHECK(((device.info().accelerated_encode_codecs & codec_mask(VideoCodec::H264)) != 0) == encode);
    CHECK((strstr(device.encode_status(), "H.264 encode runs on it") != nullptr) == encode);   // (the encoder has a status line of its own)
    VideoFrameBuffer src(alloc);
    CHECK(src.allocate({64, 48, PixelFormat::I420}));
    for (uint32_t i = 0; i < 64 * 48; ++i) src.plane(0)[i] = static_cast<uint8_t>(i * 3);
    for (uint32_t i = 0; i < 32 * 24; ++i) { src.plane(1)[i] = 90; src.plane(2)[i] = 170; }
    const int before = fw.encodes;
    {
      UniquePointer<IVideoEncoder> session = device.create_encoder(config());
      CHECK(session);
      VideoPacket packet;
      CHECK(session && session->submit(src.frame(0, true)) == SubmitResult::Accepted && session->receive(&packet) && packet.keyframe && packet.data.size() > 0);
      CHECK((fw.encodes == before + 1) == encode);   // on the engine, or on the CPU
      // Anything but H.264 stays on the CPU.
      EncoderConfig mjpeg = config();
      mjpeg.codec = VideoCodec::Mjpeg;
      CHECK(device.create_encoder(mjpeg));
    }
    device.shutdown();
    CHECK(!device.encode_ready());
    CHECK(device.info().accelerated_encode_codecs == 0);
    // The engine is powered down again when it was powered up.
    if (encode) CHECK(card.smc_messages.size() >= 2);
  }
  fw.encoder.reset();
}

int main() {
  test_start_and_sessions();
  test_clock_levels();
  test_planning_refusals();
  test_start_refusals();
  test_device(true);
  test_device(false);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd vce3 video engine tests passed\n");
  return 0;
}

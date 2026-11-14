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

// video::VideoPlayer with its service thread on real threads (tests/HostThreads.cpp): the thread decodes and shows the pictures while other threads ask
// where playing is, copy the picture, pause, seek and hand the player a longer snapshot of the stream (continue_media), as a page's thread and the
// thread that paints the page do in the browser. Built with the address and the thread sanitizer (tests/run_host_tests.sh VideoServiceTest).
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Kernel/Thread.hpp"
#include "Media/ByteSource.hpp"
#include "Media/Mse.hpp"
#include "Video/VideoPlayer.hpp"

using namespace media;

namespace {

// (several threads allocate: the count is atomic)
struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct File {
  std::vector<uint8_t> bytes;
  explicit File(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
      bytes.resize(static_cast<size_t>(size));
      if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    }
    fclose(f);
  }
  ConstByteView view() const { return ConstByteView(bytes.data(), bytes.size()); }
  bool ok() const { return !bytes.empty(); }
};

struct HostClock final : hardware::IClock {
  const char* name() const override { return "host clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
  }
};
struct NoPci final : hardware::IPciBus {
  const char* name() const override { return "no pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, hardware::PciDeviceInfo*) const override { return false; }
  uint32_t read32(hardware::PciAddress, uint16_t) const override { return 0; }
  void write32(hardware::PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(hardware::PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(hardware::PciAddress, uint8_t) const override { return 0; }
};

// Where the second movie fragment of a fragmented MP4 file starts.
uint64_t second_fragment(ConstByteView file) {
  uint64_t at = 0;
  int fragments = 0;
  ConstByteView head;
  while (file.try_subview(at, 8, &head)) {
    const uint64_t size = (uint64_t{head[0]} << 24) | (uint64_t{head[1]} << 16) | (uint64_t{head[2]} << 8) | head[3];
    if (head[4] == 'm' && head[5] == 'o' && head[6] == 'o' && head[7] == 'f' && ++fragments == 2) return at;
    if (size < 8 || size > file.size() - at) break;
    at += size;
  }
  return 0;
}

struct Painter {
  video::VideoPlayer* player = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> copies{0}, pictures{0};
};

// The thread that paints: copies the picture whenever it can, and counts the different ones it saw.
void painter_main(void* argument) {
  Painter& p = *static_cast<Painter*>(argument);
  std::vector<uint32_t> pixels(96 * 64);
  uint32_t last = ~0u;
  while (!p.stop.load()) {
    uint32_t serial = 0;
    if (p.player->copy_picture(ArrayView<uint32_t>(pixels.data(), pixels.size()), 96, &serial)) {
      ++p.copies;
      if (serial != last) ++p.pictures;
      last = serial;
    }
    kernel::sleep_us(700);
  }
}

void test_service() {
  TestAllocator a;
  File video("tests/data/mse_video.mp4");
  CHECK(video.ok());
  if (!video.ok()) return;
  const uint64_t cut = second_fragment(video.view());
  CHECK(cut > 0);
  if (cut == 0) return;
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    HostClock clock;
    MseBuffer b(a, MseFormat::Mp4, 1);
    MseSnapshotPlan first, both;
    ByteBuffer file1(a), file2(a);
    CHECK(b.append(video.view().first(cut), 0) == MseAppend::Ok && b.plan(500000, &first) && b.snapshot(&first, &file1));
    CHECK(b.append(video.view().subview(cut), 0) == MseAppend::Ok && b.plan(500000, &both) && b.snapshot(&both, &file2));
    media::MemoryByteSource source1(file1.view()), source2(file2.view());

    video::VideoPlayer player(clock, codecs, a);
    CHECK(player.open_media(source1) == video::OpenResult::Ok);
    CHECK(player.start_service() && player.service_running());
    Painter painter;
    painter.player = &player;
    kernel::Thread* paint_thread = kernel::spawn(painter_main, &painter, "painter");
    CHECK(paint_thread != nullptr);

    CHECK(player.play());
    // The page's thread: asks for the position all the time (it must go forward and stay inside the file), pauses and plays once, and gives the player
    // the longer file while the first one plays.
    int64_t previous = 0;
    bool forward = true, inside = true, continued = false, paused_once = false;
    const uint64_t start = clock.now_us();
    while (clock.now_us() - start < 1900000) {
      const int64_t position = player.position_us();
      if (position < previous) forward = false;
      if (position < 0 || position > player.duration_us()) inside = false;
      previous = position;
      const uint64_t elapsed = clock.now_us() - start;
      if (!continued && elapsed > 400000) {
        continued = player.continue_media(source2, both.base_us - first.base_us);
        CHECK(continued);
      }
      if (!paused_once && elapsed > 900000) {
        paused_once = true;
        player.pause();
        const int64_t held = player.position_us();
        kernel::sleep_us(30000);
        CHECK(player.position_us() == held && !player.playing());
        CHECK(player.play());
      }
      uint64_t presented = player.frames_presented(), skipped = player.frames_skipped();
      (void)presented;
      (void)skipped;
      kernel::sleep_us(300);
    }
    CHECK(forward && inside && continued);
    CHECK(previous > 1500000);                                  // (played through the place where the first file ended, at 1.08 s)
    CHECK(player.frames_presented() >= 20);                     // (25 pictures a second for 1.9 s, less whatever a loaded test machine made it skip)
    CHECK(player.frames_decoded() <= 60);                       // (and nothing decoded twice: the file has 50 pictures)
    player.seek(200000);                                        // (a seek from the page's thread while the service thread plays)
    kernel::sleep_us(50000);
    CHECK(player.position_us() >= 200000 && player.position_us() < 1000000);
    painter.stop.store(true);
    kernel::join(paint_thread);
    CHECK(painter.copies.load() > 100 && painter.pictures.load() >= 10);
    player.stop_service();
    CHECK(!player.service_running());
    player.update();   // (by hand again, with no thread)
    player.close();
    uint32_t serial = 0;
    std::vector<uint32_t> pixels(96 * 64);
    CHECK(!player.copy_picture(ArrayView<uint32_t>(pixels.data(), pixels.size()), 96, &serial));   // (nothing is open)
  }
  CHECK(a.live.load() == 0);
}

}  // namespace

int main() {
  test_service();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("video service tests passed\n");
  return 0;
}

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

// AmdGpu's use of a native display (amd_dce11::INativeDisplay) and of the Presenter: with a simulated UEFI graphics output (a framebuffer in host memory, two modes) and a
// simulated display engine that keeps the picture in its own "video memory" and takes rows by upload_picture. Checks that drawing goes to a back buffer and out to video memory by
// rows, that the CPU copies when the upload fails, that a mode change, shutdown or taking the display away gives the firmware's framebuffer what was drawn and the screen back, and
// that anything the display engine will not do leaves the firmware's framebuffer in use.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpu.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// The firmware's graphics output: mode 0 is 64x32 and mode 1 48x16, both with 64 pixels to a scan line, BGR, in one framebuffer of host memory.
constexpr uint32_t kPitchPixels = 64, kPitchBytes = kPitchPixels * 4, kFramebufferBytes = kPitchBytes * 32;
struct FakeFirmware {
  std::vector<uint32_t> framebuffer = std::vector<uint32_t>(kFramebufferBytes / 4, 0x00101010);
  efi::GraphicsModeInfo infos[2];
  efi::GraphicsMode mode;
  efi::GraphicsOutput gop;
  efi::BootServices boot_services;
  efi::SystemTable st;
  uint32_t set_mode_calls = 0;

  static FakeFirmware*& self() { static FakeFirmware* s = nullptr; return s; }
  static efi::Status query_mode(efi::GraphicsOutput*, uint32_t m, efi::Uintn* size, efi::GraphicsModeInfo** info) {
    if (m > 1) return 1;
    *size = sizeof(efi::GraphicsModeInfo);
    *info = &self()->infos[m];
    return efi::kSuccess;
  }
  static efi::Status set_mode(efi::GraphicsOutput*, uint32_t m) {
    FakeFirmware* f = self();
    if (m > 1) return 1;
    ++f->set_mode_calls;
    f->mode.mode = m;
    f->mode.info = &f->infos[m];
    return efi::kSuccess;
  }
  static efi::Status locate_protocol(const efi::Guid* guid, void*, void** iface) {
    if (memcmp(guid, &efi::kGraphicsOutputGuid, sizeof(efi::Guid)) != 0) return 1;
    *iface = &self()->gop;
    return efi::kSuccess;
  }
  static efi::Status free_pool(void*) { return efi::kSuccess; }

  FakeFirmware() {
    self() = this;
    memset(&boot_services, 0, sizeof boot_services);
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 2; ++i) {
      infos[i] = {};
      infos[i].horizontal_resolution = i == 0 ? 64 : 48;
      infos[i].vertical_resolution = i == 0 ? 32 : 16;
      infos[i].pixel_format = efi::PixelFormat::BlueGreenRedReserved8Bit;
      infos[i].pixels_per_scan_line = kPitchPixels;
    }
    mode = {};
    mode.max_mode = 2;
    mode.mode = 0;
    mode.info = &infos[0];
    mode.frame_buffer_base = reinterpret_cast<uint64_t>(framebuffer.data());
    mode.frame_buffer_size = kFramebufferBytes;
    gop = {};
    gop.query_mode = &query_mode;
    gop.set_mode = &set_mode;
    gop.mode = &mode;
    boot_services.locate_protocol = &locate_protocol;
    boot_services.free_pool = &free_pool;
    st.boot_services = &boot_services;
  }
  uint32_t pixel(uint32_t x, uint32_t y) const { return framebuffer[y * kPitchPixels + x] & 0x00FFFFFF; }
};

// A display engine of its own: engage_display copies the firmware's picture into its video memory (Display::engage) and disengage_display copies it back (Display::disengage).
struct FakeDisplay final : amd_dce11::INativeDisplay {
  std::vector<uint8_t> vram;
  uint8_t* firmware = nullptr;
  uint64_t picture = 0;
  bool engaged = false, refuse = false, oversize = false, fail_upload = false, drop_upload = false;
  uint32_t engages = 0, disengages = 0;
  amd_dce11::FirmwareFramebuffer last;
  struct Upload { uint64_t offset, bytes; bool ok; };
  std::vector<Upload> uploads;

  bool engage_display(const amd_dce11::FirmwareFramebuffer& fw, ByteView* memory) override {
    if (refuse) return false;
    ++engages;
    last = fw;
    firmware = reinterpret_cast<uint8_t*>(fw.physical);
    picture = uint64_t{fw.pitch_pixels} * 4 * fw.height;
    vram.assign(picture + (oversize ? 64 : 0), 0xEE);
    memcpy(vram.data(), firmware, picture);
    *memory = ByteView(vram.data(), vram.size());
    engaged = true;
    return true;
  }
  bool display_engaged() const override { return engaged; }
  void disengage_display() override {
    if (!engaged) return;
    memcpy(firmware, vram.data(), picture);   // what was drawn goes back into the firmware's framebuffer
    engaged = false;
    ++disengages;
  }
  bool upload_picture(ConstByteView pixels, uint64_t offset) override {
    const bool ok = engaged && !fail_upload && offset + pixels.size() <= vram.size();
    uploads.push_back({offset, pixels.size(), ok});
    if (ok && !drop_upload) memcpy(vram.data() + offset, pixels.data(), pixels.size());
    return ok;
  }
  uint32_t pixel(uint32_t x, uint32_t y) const { uint32_t v; memcpy(&v, vram.data() + y * kPitchBytes + x * 4, 4); return v & 0x00FFFFFF; }
};

struct Rig {
  TestAllocator alloc;
  FakeFirmware firmware;
  FakeDisplay display;
  GpuContext context;
  Rig() {
    context.st = &firmware.st;
    context.pci = nullptr;
    context.device = {};
    context.allocator = &alloc;
  }
};

static void test_native_display() {
  Rig rig;
  AmdGpu gpu(rig.context);
  CHECK(gpu.initialize() && gpu.width() == 64 && gpu.height() == 32 && !gpu.native_scanout() && !gpu.native_available());
  // Without a display engine the firmware's framebuffer is drawn in directly.
  gpu.fill_rect(0, 0, 2, 2, 0x00AABBCC);
  CHECK(rig.firmware.pixel(1, 1) == 0x00AABBCC);

  // Taking the screen over: the display engine is asked for the firmware's framebuffer (address, size, pitch in pixels), and drawing goes to the back buffer and out by rows.
  gpu.set_native_display(&rig.display);
  FakeDisplay& d = rig.display;
  CHECK(gpu.native_scanout() && gpu.native_available() && d.engages == 1);
  CHECK(d.last.physical == reinterpret_cast<uint64_t>(rig.firmware.framebuffer.data()) && d.last.bytes == kFramebufferBytes && d.last.width == 64 && d.last.height == 32 && d.last.pitch_pixels == 64);
  CHECK(d.pixel(1, 1) == 0x00AABBCC && d.uploads.empty());   // (what was on the screen came along)
  rig.firmware.framebuffer[5 * kPitchPixels + 5] = 0x00777777;
  gpu.fill_rect(2, 3, 5, 4, 0x00123456);
  CHECK(d.uploads.size() == 1 && d.uploads[0].offset == 3 * kPitchBytes && d.uploads[0].bytes == 4 * kPitchBytes && d.uploads[0].ok);   // rows 3..6, whole rows, one run
  CHECK(d.pixel(2, 3) == 0x00123456 && d.pixel(6, 6) == 0x00123456 && d.pixel(7, 3) == 0x00101010 && d.pixel(2, 7) == 0x00101010);
  CHECK(rig.firmware.pixel(2, 3) == 0x00101010);   // the firmware's framebuffer is not drawn in while the display engine shows ours
  // A pixel: one row.
  gpu.put_pixel(30, 20, 0x00ABCDEF);
  CHECK(d.uploads.size() == 2 && d.uploads[1].offset == 20 * kPitchBytes && d.uploads[1].bytes == kPitchBytes && d.pixel(30, 20) == 0x00ABCDEF);
  // Text: each character is drawn (and sent) on its own, the glyph's rows (16 for the 8x16 font) as one run.
  const uint32_t rows = gpu.glyph_height();
  size_t n = d.uploads.size();
  gpu.draw_text(0, 8, "Hi", 0x00FFFFFF, 0x00000000);
  CHECK(d.uploads.size() == n + 2 && d.uploads[n].offset == 8 * kPitchBytes && d.uploads[n].bytes == rows * kPitchBytes && d.uploads[n + 1].offset == 8 * kPitchBytes && d.pixel(0, 8) == 0x00000000);
  bool any_white = false;
  for (uint32_t y = 8; y < 8 + rows; ++y) for (uint32_t x = 0; x < 16; ++x) any_white = any_white || d.pixel(x, y) == 0x00FFFFFF;
  CHECK(any_white);
  // A blit and a rectangle copy.
  const uint32_t pixels[4] = {0x00010203, 0x00040506, 0x00070809, 0x000A0B0C};
  n = d.uploads.size();
  gpu.blit(20, 25, 2, 2, pixels, 2);
  CHECK(d.uploads.size() == n + 1 && d.uploads[n].offset == 25 * kPitchBytes && d.uploads[n].bytes == 2 * kPitchBytes && d.pixel(21, 26) == 0x000A0B0C);
  gpu.copy_rect(20, 25, 30, 28, 2, 2);
  CHECK(d.uploads.size() == n + 2 && d.uploads[n + 1].offset == 28 * kPitchBytes && d.uploads[n + 1].bytes == 2 * kPitchBytes && d.pixel(31, 29) == 0x000A0B0C);
  // The firmware's framebuffer is still the memory the firmware's part of video memory covers.
  uint64_t base = 0, bytes = 0;
  CHECK(gpu.framebuffer_range(&base, &bytes) && base == reinterpret_cast<uint64_t>(rig.firmware.framebuffer.data()) && bytes == kFramebufferBytes);

  // The upload fails (the DMA engine is stuck): the CPU copies the same rows, and the picture is right.
  d.fail_upload = true;
  gpu.fill_rect(0, 30, 4, 2, 0x00445566);
  CHECK(d.uploads.size() == n + 3 && !d.uploads[n + 2].ok && d.uploads[n + 2].offset == 30 * kPitchBytes && d.pixel(0, 30) == 0x00445566 && d.pixel(3, 31) == 0x00445566);
  d.fail_upload = false;

  // Giving the screen back: the firmware's framebuffer gets the picture, and drawing goes to it again.
  gpu.set_native_display(nullptr);
  CHECK(!gpu.native_scanout() && d.disengages == 1 && rig.firmware.pixel(2, 3) == 0x00123456 && rig.firmware.pixel(0, 31) == 0x00445566 && rig.firmware.pixel(30, 20) == 0x00ABCDEF);
  const size_t uploads = d.uploads.size();
  gpu.put_pixel(40, 10, 0x00998877);
  CHECK(rig.firmware.pixel(40, 10) == 0x00998877 && d.uploads.size() == uploads);
}

static void test_mode_change_and_shutdown() {
  Rig rig;
  AmdGpu gpu(rig.context);
  CHECK(gpu.initialize());
  gpu.set_native_display(&rig.display);
  FakeDisplay& d = rig.display;
  CHECK(gpu.native_scanout() && d.engages == 1);
  // Uploads that do not land (stand-in for rows the back buffer has and video memory lacks): going back to the firmware presents the whole picture first.
  d.drop_upload = true;
  gpu.fill_rect(4, 4, 3, 3, 0x00ABABAB);
  CHECK(d.pixel(4, 4) == 0x00101010);
  const size_t before = d.uploads.size();
  d.drop_upload = false;
  // A mode change: the firmware does it, so the display goes back to the firmware's framebuffer (with everything drawn), then is taken over again for the new mode.
  CHECK(gpu.set_mode(1) && rig.firmware.set_mode_calls == 1);
  CHECK(d.disengages == 1 && d.engages == 2 && d.uploads.size() == before + 1 && d.uploads[before].offset == 0 && d.uploads[before].bytes == 32 * kPitchBytes);
  CHECK(rig.firmware.pixel(4, 4) == 0x00ABABAB);   // (the whole back buffer was presented before the disengage copied video memory back)
  CHECK(gpu.native_scanout() && gpu.width() == 48 && gpu.height() == 16 && d.last.width == 48 && d.last.height == 16 && d.last.pitch_pixels == 64);
  // Drawing in the new mode goes to the new back buffer and rows of the new picture.
  gpu.fill_rect(1, 15, 2, 1, 0x00314159);
  CHECK(d.uploads.back().offset == 15 * kPitchBytes && d.uploads.back().bytes == kPitchBytes && d.pixel(1, 15) == 0x00314159 && d.pixel(2, 15) == 0x00314159);
  // Back to the first mode.
  CHECK(gpu.set_mode(0) && gpu.width() == 64 && gpu.native_scanout() && d.engages == 3 && d.disengages == 2);
  // Shutdown gives the screen back and forgets the display.
  gpu.fill_rect(0, 0, 1, 1, 0x00DDEEFF);
  gpu.shutdown();
  CHECK(!gpu.native_scanout() && d.disengages == 3 && rig.firmware.pixel(0, 0) == 0x00DDEEFF && !gpu.native_available());
}

static void test_refusals() {
  {
    // A display engine that will not take over: the firmware's framebuffer stays and is drawn in.
    Rig rig;
    rig.display.refuse = true;
    AmdGpu gpu(rig.context);
    CHECK(gpu.initialize());
    gpu.set_native_display(&rig.display);
    CHECK(!gpu.native_scanout() && gpu.native_available() && rig.display.engages == 0);
    gpu.fill_rect(0, 0, 4, 4, 0x00112233);
    CHECK(rig.firmware.pixel(3, 3) == 0x00112233 && rig.display.uploads.empty());
    // It can take over later (a mode change tries again).
    rig.display.refuse = false;
    CHECK(gpu.set_mode(1) && gpu.native_scanout() && rig.display.engages == 1);
  }
  {
    // Video memory that is not the size of the picture: no back buffer, the picture is drawn in video memory directly.
    Rig rig;
    rig.display.oversize = true;
    AmdGpu gpu(rig.context);
    CHECK(gpu.initialize());
    gpu.set_native_display(&rig.display);
    CHECK(gpu.native_scanout());
    gpu.fill_rect(5, 5, 2, 2, 0x00778899);
    CHECK(rig.display.pixel(5, 5) == 0x00778899 && rig.display.uploads.empty() && rig.firmware.pixel(5, 5) == 0x00101010);
    gpu.set_native_display(nullptr);
    CHECK(!gpu.native_scanout() && rig.firmware.pixel(5, 5) == 0x00778899);
  }
  {
    // Swapping one display for another: the first gives the screen back.
    Rig rig;
    FakeDisplay second;
    AmdGpu gpu(rig.context);
    CHECK(gpu.initialize());
    gpu.set_native_display(&rig.display);
    gpu.fill_rect(0, 0, 2, 2, 0x00135790);
    gpu.set_native_display(&second);
    CHECK(rig.display.disengages == 1 && second.engages == 1 && gpu.native_scanout() && rig.firmware.pixel(1, 1) == 0x00135790 && second.pixel(1, 1) == 0x00135790);
    gpu.set_native_display(nullptr);
    CHECK(second.disengages == 1 && !gpu.native_scanout());
  }
  {
    // The same display set again: nothing happens twice.
    Rig rig;
    AmdGpu gpu(rig.context);
    CHECK(gpu.initialize());
    gpu.set_native_display(&rig.display);
    gpu.set_native_display(&rig.display);
    CHECK(rig.display.engages == 1 && rig.display.disengages == 0 && gpu.native_scanout());
  }
}

int main() {
  test_native_display();
  test_mode_change_and_shutdown();
  test_refusals();
  printf(g_failed ? "AmdGpuTest: %d FAILED\n" : "AmdGpuTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}

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

// Host-side tests for the AMD VCN firmware loader: the real linux-firmware images shipped in firmware/ are read through an in-memory file system, and the
// header parser is run on every truncation of one and on every single-bit flip of its header (it must never read out of range, and what it accepts must
// still lie inside the file). Run from the repository root.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

static bool put_file(filesystem::IFileSystem& fs, const char* path, const std::vector<uint8_t>& bytes) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok) return false;
  uint64_t put = 0;
  return file->write(0, ConstByteView(bytes.data(), bytes.size()), &put) == filesystem::Status::Ok && put == bytes.size();
}

static void test_names_and_versions() {
  // The file a VCN 1.0 APU needs: revision 8 and up is Raven2 whatever the device, otherwise 15D8 is Picasso and 15DD is Raven.
  CHECK(strcmp(amd_vcn1_firmware_name(0x15DD, 0x00), "raven_vcn.bin") == 0 && strcmp(amd_vcn1_firmware_name(0x15DD, 0x07), "raven_vcn.bin") == 0);
  CHECK(strcmp(amd_vcn1_firmware_name(0x15DD, 0x08), "raven2_vcn.bin") == 0 && strcmp(amd_vcn1_firmware_name(0x15DD, 0xFF), "raven2_vcn.bin") == 0);
  CHECK(strcmp(amd_vcn1_firmware_name(0x15D8, 0x00), "picasso_vcn.bin") == 0 && strcmp(amd_vcn1_firmware_name(0x15D8, 0x07), "picasso_vcn.bin") == 0);
  CHECK(strcmp(amd_vcn1_firmware_name(0x15D8, 0x08), "raven2_vcn.bin") == 0);
  // Not VCN 1.0: Renoir/Cezanne (VCN 2), Navi, anything else.
  CHECK(!amd_vcn1_firmware_name(0x1636, 0) && !amd_vcn1_firmware_name(0x1638, 0) && !amd_vcn1_firmware_name(0x744C, 0) && !amd_vcn1_firmware_name(0, 0) && !amd_vcn1_firmware_name(0xFFFF, 0xFF));
  for (uint32_t id = 0; id <= 0xFFFF; ++id)
    CHECK((amd_vcn1_firmware_name(static_cast<uint16_t>(id), 0) != nullptr) == (id == 0x15DD || id == 0x15D8));

  // 0x0310F009 is what the shipped images carry: encoder 1.15, decoder 3, revision 9.
  AmdVcnFirmwareVersion v = amd_decode_vcn_firmware_version(0x0310F009);
  CHECK(v.new_naming && v.enc_major == 1 && v.enc_minor == 15 && v.dec == 3 && v.vep == 0 && v.revision == 9);
  v = amd_decode_vcn_firmware_version(0xF2FFF123);   // all the new-naming fields at their limits
  CHECK(v.new_naming && v.enc_major == 0xF && v.enc_minor == 0xFF && v.dec == 2 && v.vep == 0xF && v.revision == 0x123);
  v = amd_decode_vcn_firmware_version(0x01020304);   // bits 20-23 zero: the old convention
  CHECK(!v.new_naming && v.major == 1 && v.minor == 3 && v.family_id == 4);
  CHECK(strcmp(amd_firmware_status_name(AmdFirmwareStatus::NotFound), "not found") == 0 && strcmp(amd_firmware_status_name(AmdFirmwareStatus::Ok), "loaded") == 0);
}

static void test_real_images(TestAllocator& a) {
  {
  filesystem::RamFileSystem fs(a);
  CHECK(fs.create_directory("/firmware") == filesystem::Status::Ok);
  for (const char* name : {"raven_vcn.bin", "picasso_vcn.bin", "raven2_vcn.bin"}) {
    char host[96];
    snprintf(host, sizeof host, "firmware/%s", name);
    const std::vector<uint8_t> image = read_host_file(host);
    CHECK(image.size() > 300000);   // the file is in the repository, not an LFS pointer
    CHECK(put_file(fs, (std::string("/firmware/") + name).c_str(), image));
    AmdVcnFirmware fw(a);
    CHECK(fw.load(fs, name) == AmdFirmwareStatus::Ok && fw.loaded());
    CHECK(fw.header().size_bytes == image.size() && fw.header().header_size_bytes == 32 && fw.header().ip_version_major == 1 && fw.header().ip_version_minor == 0);
    CHECK(fw.header().header_version_major == 1 && fw.header().ucode_array_offset_bytes == 256 && fw.header().ucode_size_bytes == image.size() - 256);
    CHECK(fw.image().size() == image.size() && memcmp(fw.image().data(), image.data(), image.size()) == 0);
    CHECK(fw.ucode().size() == image.size() - 256 && fw.ucode().data() == fw.image().data() + 256);
    const AmdVcnFirmwareVersion v = fw.version();
    CHECK(v.new_naming && v.enc_major == 1 && v.enc_minor == 15 && v.dec == 3 && v.revision == 9);
    // Every prefix of the image is refused (the size field no longer matches), and a header whose fields were flipped one bit at a time is accepted only if
    // the microcode it describes lies inside the file.
    for (size_t n = 0; n < 512; ++n) CHECK(!amd_parse_firmware_header(ConstByteView(image.data(), n), nullptr));
    CHECK(!amd_parse_firmware_header(ConstByteView(image.data(), image.size() - 1), nullptr));
    for (size_t byte = 0; byte < kAmdFirmwareHeaderBytes; ++byte)
      for (int bit = 0; bit < 8; ++bit) {
        std::vector<uint8_t> t(image.begin(), image.begin() + 512);   // the header and some of the microcode
        t[byte] ^= static_cast<uint8_t>(1u << bit);
        AmdFirmwareHeader h;
        if (amd_parse_firmware_header(ConstByteView(t.data(), t.size()), &h))
          CHECK(h.ucode_array_offset_bytes >= h.header_size_bytes && h.ucode_size_bytes <= h.size_bytes - h.ucode_array_offset_bytes);
      }
    fw.clear();
    CHECK(!fw.loaded() && fw.ucode().empty());
  }
  }
  CHECK(a.live == 0);   // the loader and the file system gave everything back
}

static void test_failures(TestAllocator& a) {
  filesystem::RamFileSystem fs(a);
  fs.create_directory("/firmware");
  const std::vector<uint8_t> good = read_host_file("firmware/raven_vcn.bin");
  CHECK(put_file(fs, "/firmware/ok.bin", good));
  AmdVcnFirmware fw(a);
  CHECK(fw.load(fs, "missing.bin") == AmdFirmwareStatus::NotFound && !fw.loaded());
  CHECK(fw.load(fs, nullptr) == AmdFirmwareStatus::NotApplicable && fw.load(fs, "") == AmdFirmwareStatus::NotApplicable);
  // A name that would leave the folder is never opened.
  CHECK(put_file(fs, "/outside.bin", good));
  CHECK(fw.load(fs, "../outside.bin") == AmdFirmwareStatus::NotFound && fw.load(fs, "/outside.bin") == AmdFirmwareStatus::NotFound && fw.load(fs, "a\\\\b") == AmdFirmwareStatus::NotFound);
  // Not an image: too short for a header, text, a header whose size field lies, a header that points the microcode past the end or into itself.
  CHECK(put_file(fs, "/firmware/tiny.bin", std::vector<uint8_t>(10, 1)) && fw.load(fs, "tiny.bin") == AmdFirmwareStatus::Invalid);
  CHECK(put_file(fs, "/firmware/text.bin", std::vector<uint8_t>(400, 'x')) && fw.load(fs, "text.bin") == AmdFirmwareStatus::Invalid);
  std::vector<uint8_t> lie = good;
  lie[0] ^= 1;
  CHECK(put_file(fs, "/firmware/lie.bin", lie) && fw.load(fs, "lie.bin") == AmdFirmwareStatus::Invalid);
  std::vector<uint8_t> past = good;
  past[24] = 0xFF; past[25] = 0xFF; past[26] = 0xFF; past[27] = 0x7F;   // microcode offset near 2 GiB
  CHECK(put_file(fs, "/firmware/past.bin", past) && fw.load(fs, "past.bin") == AmdFirmwareStatus::Invalid);
  std::vector<uint8_t> wrap = good;
  wrap[20] = 0xFF; wrap[21] = 0xFF; wrap[22] = 0xFF; wrap[23] = 0xFF;   // microcode size 4 GiB - 1: offset + size would wrap a 32-bit sum
  CHECK(put_file(fs, "/firmware/wrap.bin", wrap) && fw.load(fs, "wrap.bin") == AmdFirmwareStatus::Invalid);
  std::vector<uint8_t> overlap = good;
  overlap[24] = 16; overlap[25] = overlap[26] = overlap[27] = 0;        // microcode starting inside the header
  CHECK(put_file(fs, "/firmware/overlap.bin", overlap) && fw.load(fs, "overlap.bin") == AmdFirmwareStatus::Invalid);
  std::vector<uint8_t> empty_ucode = good;
  empty_ucode[20] = empty_ucode[21] = empty_ucode[22] = empty_ucode[23] = 0;
  CHECK(put_file(fs, "/firmware/empty.bin", empty_ucode) && fw.load(fs, "empty.bin") == AmdFirmwareStatus::Invalid);
  CHECK(!fw.loaded() && fw.ucode().empty() && fw.image().empty());
  // A file bigger than anything legitimate is refused without being read.
  CHECK(put_file(fs, "/firmware/huge.bin", std::vector<uint8_t>(AmdVcnFirmware::kMaxBytes + 1, 0)) && fw.load(fs, "huge.bin") == AmdFirmwareStatus::TooLarge);
  // Memory running out is a miss that leaves nothing behind; the good file still loads afterwards and can be replaced.
  a.fail = true;
  CHECK(fw.load(fs, "ok.bin") != AmdFirmwareStatus::Ok && !fw.loaded());
  a.fail = false;
  CHECK(fw.load(fs, "ok.bin") == AmdFirmwareStatus::Ok && fw.adopt(ConstByteView(good.data(), good.size() - 4)) == AmdFirmwareStatus::Invalid && !fw.loaded());
  CHECK(fw.adopt(ConstByteView(good.data(), good.size())) == AmdFirmwareStatus::Ok && fw.loaded());
}

int main() {
  TestAllocator a;
  test_names_and_versions();
  test_real_images(a);
  test_failures(a);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdVcnFirmwareTest passed\n");
  return 0;
}

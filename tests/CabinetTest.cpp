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

// Host tests for the cabinet reader (src/Media/Cabinet.hpp) and the LZX decompressor (src/Media/Lzx.hpp). The cabinets of tests/data/cabinet are made by
// make_fixtures.py there - stored, MSZIP and LZX folders, the LZX ones by a compressor in that script whose output `cabextract` was checked to decode to
// the same files - and manifest.txt says what every file must come out as (size and CRC-32). Each is read whole, file by file, with files passed over,
// cut short at many lengths and with single bits flipped; garbage is fed to the LZX decoder directly.
//
// CABINET_TEST_FILES=a.cab:b.cab in the environment adds cabinets of the machine (the DirectX redistributables of a game, say): each is extracted
// with `cabextract` and with this reader and the files compared. Skipped, with a note, if cabextract is missing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Media/Cabinet.hpp"
#include "Media/CabinetService.hpp"
#include "Media/Lzx.hpp"

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t next_random() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 7;
  g_seed ^= g_seed << 17;
  return g_seed;
}

static bool read_file(const std::string& path, Bytes* data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  data->clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data->insert(data->end(), chunk, chunk + n);
  fclose(f);
  return true;
}

static ConstByteView view_of(const Bytes& bytes) { return ConstByteView(bytes.data(), bytes.size()); }

// What manifest.txt says about one file.
struct Expected {
  std::string name;
  uint64_t size = 0;
  uint32_t crc = 0;
  uint32_t folder = 0;
  uint32_t attributes = 0;
};

// A source that counts what is read of it.
struct CountingSource final : media::IZipSource {
  explicit CountingSource(const Bytes& bytes) : memory(view_of(bytes)) {}
  uint64_t size() const override { return memory.size(); }
  [[nodiscard]] bool read(uint64_t offset, ByteView out) override {
    bytes_read += out.size();
    return memory.read(offset, out);
  }
  media::MemoryZipSource memory;
  uint64_t bytes_read = 0;
};

// Collects the files a walk hands over, by name; passes over the ones `skip` says, stops at the file `stop_at`.
struct CollectSink final : media::ICabinetSink {
  struct File {
    std::string name;
    Bytes data;
    bool ended = false;
  };
  std::vector<File> files;
  std::vector<std::string> seen;
  bool (*skip)(size_t index) = nullptr;
  long stop_at = -1;
  bool open = false;
  bool misused = false;

  [[nodiscard]] bool begin_file(const media::CabinetEntry& entry, bool* wanted) override {
    if (open || !*wanted) misused = true;
    const size_t index = seen.size();
    seen.push_back(std::string(entry.name.view().data(), entry.name.view().size()));
    if (static_cast<long>(index) == stop_at) return false;
    if (skip && skip(index)) {
      *wanted = false;
      return true;
    }
    files.push_back(File{seen.back(), Bytes(), false});
    written = files.size() - 1;
    open = true;
    return true;
  }
  [[nodiscard]] bool write(ConstByteView bytes) override {
    if (!open || bytes.empty() || bytes.size() > 32768) {
      misused = true;
      return false;
    }
    files.back().data.insert(files.back().data.end(), bytes.begin(), bytes.end());
    return true;
  }
  [[nodiscard]] bool end_file() override {
    if (!open) misused = true;
    open = false;
    files.back().ended = true;
    return true;
  }
  // (the same bytes under another name: a copy of the file that was written last)
  [[nodiscard]] bool same_file(const media::CabinetEntry& entry, const media::CabinetEntry& first) override {
    const std::string original(first.name.view().data(), first.name.view().size());
    if (open || written >= files.size() || !files[written].ended || files[written].name != original) {
      misused = true;
      return false;
    }
    const std::string name(entry.name.view().data(), entry.name.view().size());
    seen.push_back(name);
    files.push_back(File{name, files[written].data, true});
    return true;
  }
  size_t written = 0;      // which of `files` was written last (not copied)
};

static std::string g_data = "tests/data/cabinet/";

struct Fixture {
  std::string name;
  Bytes cabinet;
  std::vector<Expected> expected;
  std::map<std::string, Bytes> plain;      // what the files are, once the whole cabinet was read and checked
};

static bool load_manifest(std::vector<Fixture>* fixtures) {
  FILE* f = fopen((g_data + "manifest.txt").c_str(), "r");
  if (!f) return false;
  char cabinet[64], name_hex[1024];
  unsigned folder, crc, attributes;
  unsigned long long size;
  while (fscanf(f, "%63s %u %llu %x %x %1023s", cabinet, &folder, &size, &crc, &attributes, name_hex) == 6) {
    if (fixtures->empty() || fixtures->back().name != cabinet) {
      fixtures->push_back(Fixture());
      fixtures->back().name = cabinet;
    }
    Expected e;
    for (size_t i = 0; name_hex[i] && name_hex[i + 1]; i += 2) {
      unsigned byte = 0;
      sscanf(name_hex + i, "%2x", &byte);
      e.name.push_back(static_cast<char>(byte));
    }
    e.size = size;
    e.crc = crc;
    e.folder = folder;
    e.attributes = attributes;
    fixtures->back().expected.push_back(e);
  }
  fclose(f);
  for (Fixture& fixture : *fixtures)
    if (!read_file(g_data + fixture.name, &fixture.cabinet)) return false;
  return !fixtures->empty();
}

// The whole cabinet, against the manifest: the directory, a walk over all files, each file on its own.
static void test_whole(Fixture& fixture) {
  TestAllocator allocator;
  {
    CountingSource source(fixture.cabinet);
    media::CabinetReader reader(allocator, source);
    CHECK(reader.open());
    const ArrayView<const media::CabinetEntry> entries = reader.entries();
    CHECK(entries.size() == fixture.expected.size());
    if (entries.size() != fixture.expected.size()) return;
    uint32_t folders = 0;
    for (size_t i = 0; i < fixture.expected.size(); ++i) {
      const Expected& e = fixture.expected[i];
      const media::CabinetEntry& entry = entries[i];
      CHECK(entry.name.view().equals(StringView(e.name.data(), e.name.size())));
      CHECK(entry.size == e.size);
      CHECK(entry.folder == e.folder);
      CHECK(entry.attributes == e.attributes);
      CHECK(entry.name_is_utf8() == ((e.attributes & 0x80) != 0));
      CHECK(entry.date == 0x5A21 && entry.time == 0x6000);
      CHECK(reader.find(StringView(e.name.data(), e.name.size())) == &entry);
      if (e.folder + 1 > folders) folders = e.folder + 1;
    }
    CHECK(reader.folder_count() == folders);
    CHECK(reader.find("no such file") == nullptr);

    CollectSink all;
    CHECK(reader.extract_all(all));
    CHECK(!all.misused && all.files.size() == fixture.expected.size());
    for (size_t i = 0; i < all.files.size() && i < fixture.expected.size(); ++i) {
      const Expected& e = fixture.expected[i];      // (the fixtures list their files folder by folder: the walk's order is the directory's)
      CHECK(all.files[i].name == e.name);
      CHECK(all.files[i].ended);
      CHECK(all.files[i].data.size() == e.size);
      CHECK(media::crc32(view_of(all.files[i].data)) == e.crc);
      fixture.plain[e.name] = all.files[i].data;
    }
    const uint64_t read_for_all = source.bytes_read;

    // One file at a time, into a buffer that holds something already.
    for (const media::CabinetEntry& entry : entries) {
      const Bytes& want = fixture.plain[std::string(entry.name.view().data(), entry.name.view().size())];
      ByteBuffer out(allocator);
      CHECK(out.put32(0x12345678));
      CHECK(reader.extract(entry, want.size(), &out));
      CHECK(out.size() == 4 + want.size() && out.view().subview(4).equals(view_of(want)));
      if (!want.empty()) {
        CHECK(!reader.extract(entry, want.size() - 1, &out));      // larger than allowed
        CHECK(out.size() == 4 + want.size());
      }
    }
    {
      // An entry of another reader is refused.
      media::MemoryZipSource other_source(view_of(fixture.cabinet));
      media::CabinetReader other(allocator, other_source);
      CHECK(other.open());
      ByteBuffer out(allocator);
      CHECK(!reader.extract(other.entries()[0], 1 << 30, &out) && out.size() == 0);
    }

    // Passing files over: every second one, all but the last, all. What is wanted comes out the same, and less of the cabinet is read.
    CollectSink odd;
    odd.skip = [](size_t index) { return index % 2 == 0; };
    source.bytes_read = 0;
    CHECK(reader.extract_all(odd));
    CHECK(!odd.misused && odd.seen.size() == fixture.expected.size() && odd.files.size() == fixture.expected.size() / 2);
    for (const CollectSink::File& file : odd.files) CHECK(file.ended && file.data == fixture.plain[file.name]);

    CollectSink none;
    none.skip = [](size_t) { return true; };
    source.bytes_read = 0;
    CHECK(reader.extract_all(none));
    CHECK(none.files.empty() && none.seen.size() == fixture.expected.size());
    CHECK(source.bytes_read == 0);      // nothing wanted: no data block is read

    CollectSink first;
    first.skip = [](size_t index) { return index != 0; };
    source.bytes_read = 0;
    CHECK(reader.extract_all(first));
    CHECK(first.files.size() == 1 && first.files[0].data == fixture.plain[first.files[0].name]);
    CHECK(source.bytes_read < read_for_all);

    // A sink that stops the walk.
    CollectSink stopping;
    stopping.stop_at = static_cast<long>(fixture.expected.size()) - 1;
    CHECK(!reader.extract_all(stopping));
    CHECK(stopping.files.size() == fixture.expected.size() - 1);
  }
  CHECK(allocator.live == 0);
}

// Reads what can be read of a damaged cabinet. With `prefixes`, whatever is handed out must be the start of the right file.
static bool read_damaged(const Fixture& fixture, const Bytes& cabinet, bool prefixes) {
  TestAllocator allocator;
  bool ok = false;
  {
    media::MemoryZipSource source(view_of(cabinet));
    media::CabinetReader reader(allocator, source);
    if (reader.open()) {
      CollectSink sink;
      ok = reader.extract_all(sink);
      if (prefixes) {
        for (const CollectSink::File& file : sink.files) {
          const auto found = fixture.plain.find(file.name);
          const bool is_prefix = found != fixture.plain.end() && file.data.size() <= found->second.size() &&
                                 (file.data.empty() || memcmp(file.data.data(), found->second.data(), file.data.size()) == 0);
          CHECK(is_prefix);
          if (!is_prefix) printf("  %s cut to %zu: %s is wrong\n", fixture.name.c_str(), cabinet.size(), file.name.c_str());
        }
      }
      for (const media::CabinetEntry& entry : reader.entries()) {      // (and the one-file way, on a few)
        if (next_random() % 8 != 0) continue;
        ByteBuffer out(allocator);
        if (!reader.extract(entry, 1 << 22, &out)) CHECK(out.size() == 0);
      }
    }
  }
  CHECK(allocator.live == 0);
  return ok;
}

static void test_truncation(const Fixture& fixture) {
  const size_t size = fixture.cabinet.size();
  std::vector<size_t> lengths;
  for (size_t n = 0; n < size && n < 700; ++n) lengths.push_back(n);
  for (size_t n = 700; n < size; n += 211 + size / 300) lengths.push_back(n);
  for (size_t n = size > 80 ? size - 80 : 0; n < size; ++n) lengths.push_back(n);
  for (const size_t n : lengths) {
    const Bytes cut(fixture.cabinet.begin(), fixture.cabinet.begin() + n);      // (its own allocation: reading past it is an error the sanitizer sees)
    CHECK(!read_damaged(fixture, cut, true));
  }
}

static void test_bit_flips(const Fixture& fixture) {
  int survived = 0;
  for (int i = 0; i < 500; ++i) {
    Bytes damaged = fixture.cabinet;
    // A fifth of the flips go to the directory at the start, where every bit means something.
    const size_t range = i % 5 == 0 && damaged.size() > 256 ? 256 : damaged.size();
    damaged[next_random() % range] ^= static_cast<uint8_t>(1u << (next_random() % 8));
    if (read_damaged(fixture, damaged, false)) ++survived;
  }
  printf("  %s: %d of 500 single-bit flips still read to the end\n", fixture.name.c_str(), survived);
}

// The LZX decoder on its own: what it refuses, and garbage.
static void test_lzx_decoder() {
  TestAllocator allocator;
  {
    media::LzxDecoder decoder(allocator);
    ByteBuffer out(allocator);
    const uint8_t zeros[64] = {};
    CHECK(!decoder.decode_block(ConstByteView(zeros), 100, &out));      // not started
    CHECK(!decoder.reset(14));
    CHECK(!decoder.reset(22));
    CHECK(!decoder.decode_block(ConstByteView(zeros), 100, &out));
    CHECK(decoder.reset(15));
    CHECK(!decoder.decode_block(ConstByteView(zeros), 0, &out));
    CHECK(decoder.reset(15));
    CHECK(!decoder.decode_block(ConstByteView(zeros), 32769, &out));
    CHECK(decoder.reset(15));
    CHECK(!decoder.decode_block(ConstByteView(), 100, &out));
    CHECK(!decoder.decode_block(ConstByteView(zeros), 100, &out));      // a failed stream stays failed
    CHECK(out.size() == 0);

    // An uncompressed block made by hand: no call translation; type 3, 5 bytes; the padding; three offsets; the bytes and their pad byte.
    const uint8_t stored[] = {0x00, 0x30, 0x02, 0x80, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
    CHECK(decoder.reset(16));
    CHECK(decoder.decode_block(ConstByteView(stored), 5, &out));
    CHECK(out.view().equals(ConstByteView(reinterpret_cast<const uint8_t*>("hello"), 5)));
    CHECK(!decoder.decode_block(ConstByteView(stored), 5, &out));      // a short frame was the last
    CHECK(out.size() == 5);

    for (int round = 0; round < 3000; ++round) {
      Bytes garbage(1 + next_random() % 600);
      for (uint8_t& byte : garbage) byte = static_cast<uint8_t>(next_random() >> (round % 3 == 0 ? 60 : 56));      // (some rounds: mostly zero bits)
      if (round % 4 == 0) garbage[0] = static_cast<uint8_t>(0x10 << (next_random() % 3));
      CHECK(decoder.reset(15 + static_cast<uint32_t>(next_random() % 7)));
      out.clear();
      uint64_t expected = 0;
      for (int block = 0; block < 3; ++block) {
        const uint32_t want = block < 2 ? 32768 : 1 + static_cast<uint32_t>(next_random() % 32768);
        if (!decoder.decode_block(view_of(garbage), want, &out)) break;
        expected += want;
      }
      CHECK(out.size() == expected);
    }
  }
  CHECK(allocator.live == 0);
}

// Cabinets the machine has, against cabextract.
static void test_real_cabinets(const char* list) {
  if (system("command -v cabextract >/dev/null 2>&1") != 0) {
    printf("  cabextract is not installed: CABINET_TEST_FILES skipped\n");
    return;
  }
  char pattern[] = "/tmp/cabinet-test-XXXXXX";
  const char* made = mkdtemp(pattern);
  CHECK(made != nullptr);
  if (!made) return;
  const std::string all = list, base = made;
  int number = 0;
  for (size_t at = 0; at <= all.size();) {
    size_t end = all.find(':', at);
    if (end == std::string::npos) end = all.size();
    const std::string path = all.substr(at, end - at);
    at = end + 1;
    if (path.empty()) continue;
    const std::string directory = base + "/" + std::to_string(number++);
    const std::string command = "cabextract -q -d '" + directory + "' '" + path + "' >/dev/null 2>&1";
    Bytes cabinet;
    if (!read_file(path, &cabinet) || system(command.c_str()) != 0) {
      printf("  %s: cannot be read, or cabextract fails on it: skipped\n", path.c_str());
      continue;
    }
    TestAllocator allocator;
    {
      media::MemoryZipSource source(view_of(cabinet));
      media::CabinetReader reader(allocator, source);
      CHECK(reader.open());
      CollectSink sink;
      CHECK(reader.extract_all(sink));
      CHECK(sink.files.size() == reader.entries().size());
      uint64_t total = 0;
      for (const CollectSink::File& file : sink.files) {
        std::string name = file.name;
        for (char& c : name)
          if (c == '\\') c = '/';
        Bytes want;
        const bool same = read_file(directory + "/" + name, &want) && want == file.data;
        CHECK(same);
        if (!same) printf("  %s: %s differs from what cabextract makes of it\n", path.c_str(), file.name.c_str());
        total += file.data.size();
      }
      printf("  %s: %zu files, %llu bytes, as cabextract\n", path.c_str(), sink.files.size(), static_cast<unsigned long long>(total));
    }
    CHECK(allocator.live == 0);
  }
  const std::string remove = "rm -rf '" + base + "'";
  if (system(remove.c_str()) != 0) printf("  could not remove %s\n", base.c_str());
}

// What cabinet.dll asks of the kernel (NtTcosCabinet's two operations, src/Media/CabinetService.hpp): the records of the files and each file by its number,
// against the manifest; then buffers that are too small, numbers past the end, and cabinets cut short or with a bit flipped.
static uint64_t record_number(const Bytes& records, size_t record, size_t at, size_t bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes; ++i) value |= uint64_t{records[record * media::kCabinetRecordSize + at + i]} << (8 * i);
  return value;
}

static void test_service(Fixture& fixture) {
  using media::CabinetServiceStatus;
  TestAllocator allocator;
  {
    const ConstByteView cabinet = view_of(fixture.cabinet);
    const uint64_t count = fixture.expected.size();
    ByteBuffer records(allocator);
    uint64_t files = 99;
    CHECK(media::describe_cabinet(allocator, cabinet, count * media::kCabinetRecordSize, &records, &files) == CabinetServiceStatus::Ok);
    CHECK(files == count && records.size() == count * media::kCabinetRecordSize);
    if (files != count || records.size() != count * media::kCabinetRecordSize) return;
    const Bytes raw(records.view().begin(), records.view().end());
    for (size_t i = 0; i < count; ++i) {
      const Expected& e = fixture.expected[i];
      CHECK(record_number(raw, i, 0, 8) == e.size);
      CHECK(record_number(raw, i, 8, 4) == e.name.size());
      CHECK(record_number(raw, i, 16, 2) == e.attributes);
      CHECK(record_number(raw, i, 18, 2) == e.folder);
      CHECK(record_number(raw, i, 20, 4) == ((e.attributes & media::CabinetEntry::kNameIsUtf8) ? media::kCabinetRecordUtf8 : 0u));
      CHECK(e.name.size() <= media::kCabinetRecordName && memcmp(raw.data() + i * media::kCabinetRecordSize + 24, e.name.data(), e.name.size()) == 0);
      for (size_t at = 24 + e.name.size(); at < media::kCabinetRecordSize; ++at) CHECK(raw[i * media::kCabinetRecordSize + at] == 0);
    }
    // Room for one record too few, and for none: the count comes back and nothing is appended.
    ByteBuffer few(allocator);
    files = 99;
    CHECK(media::describe_cabinet(allocator, cabinet, count * media::kCabinetRecordSize - 1, &few, &files) == CabinetServiceStatus::BufferTooSmall);
    CHECK(files == count && few.size() == 0);
    CHECK(media::describe_cabinet(allocator, cabinet, 0, &few, &files) == CabinetServiceStatus::BufferTooSmall && files == count && few.size() == 0);

    for (size_t i = 0; i < count; ++i) {
      const Expected& e = fixture.expected[i];
      ByteBuffer out(allocator);
      uint64_t size = 99;
      CHECK(out.put8(0x5A));      // (the file is appended to what the buffer holds)
      CHECK(media::extract_cabinet_file(allocator, cabinet, i, e.size, &out, &size) == CabinetServiceStatus::Ok);
      CHECK(size == e.size && out.size() == e.size + 1 && out.view()[0] == 0x5A);
      CHECK(media::crc32(out.view().subview(1)) == e.crc);
      if (e.size != 0) {
        ByteBuffer small(allocator);
        size = 99;
        CHECK(media::extract_cabinet_file(allocator, cabinet, i, e.size - 1, &small, &size) == CabinetServiceStatus::BufferTooSmall);
        CHECK(size == e.size && small.size() == 0);
      }
    }
    ByteBuffer none(allocator);
    uint64_t size = 99;
    CHECK(media::extract_cabinet_file(allocator, cabinet, count, ~uint64_t{0}, &none, &size) == CabinetServiceStatus::InvalidParameter && size == 0);
    CHECK(media::extract_cabinet_file(allocator, cabinet, ~uint64_t{0}, ~uint64_t{0}, &none, &size) == CabinetServiceStatus::InvalidParameter);
    CHECK(media::extract_cabinet_file(allocator, cabinet, uint64_t{1} << 32, ~uint64_t{0}, &none, &size) == CabinetServiceStatus::InvalidParameter);
    CHECK(none.size() == 0);

    // Cut short: never a crash, never more than was asked for; what does come out whole is the right file.
    for (int round = 0; round < 60; ++round) {
      const uint64_t length = round < 40 ? uint64_t(round) : next_random() % fixture.cabinet.size();
      const ConstByteView part = cabinet.first(length);
      ByteBuffer r(allocator);
      const CabinetServiceStatus described = media::describe_cabinet(allocator, part, ~uint64_t{0}, &r, &files);
      CHECK(described == CabinetServiceStatus::Ok || (described == CabinetServiceStatus::BadData && r.size() == 0));
      if (length < 36) CHECK(described == CabinetServiceStatus::BadData);
      const size_t index = size_t(next_random() % count);
      ByteBuffer out(allocator);
      const CabinetServiceStatus extracted = media::extract_cabinet_file(allocator, part, index, ~uint64_t{0}, &out, &size);
      CHECK(extracted == CabinetServiceStatus::Ok || (extracted == CabinetServiceStatus::BadData && out.size() == 0));
      if (extracted == CabinetServiceStatus::Ok) CHECK(out.size() == fixture.expected[index].size && media::crc32(out.view()) == fixture.expected[index].crc);
    }
    // A bit flipped: the same, except that a flip may change what the cabinet says is in it (a size, a name), so only the bounds are checked.
    for (int round = 0; round < 120; ++round) {
      Bytes damaged = fixture.cabinet;
      const uint64_t at = round < 60 ? next_random() % 200 % damaged.size() : next_random() % damaged.size();      // (half of them in the header and the directory)
      damaged[at] ^= uint8_t(1u << (next_random() % 8));
      ByteBuffer r(allocator);
      const CabinetServiceStatus described = media::describe_cabinet(allocator, view_of(damaged), ~uint64_t{0}, &r, &files);
      CHECK(described == CabinetServiceStatus::Ok || described == CabinetServiceStatus::BadData);
      CHECK(r.size() == (described == CabinetServiceStatus::Ok ? files * media::kCabinetRecordSize : 0));
      ByteBuffer out(allocator);
      const uint64_t capacity = 100000;
      const CabinetServiceStatus extracted = media::extract_cabinet_file(allocator, view_of(damaged), next_random() % (count + 1), capacity, &out, &size);
      CHECK(extracted == CabinetServiceStatus::Ok || extracted == CabinetServiceStatus::BadData || extracted == CabinetServiceStatus::BufferTooSmall ||
            extracted == CabinetServiceStatus::InvalidParameter);
      CHECK(out.size() == (extracted == CabinetServiceStatus::Ok ? size : 0) && out.size() <= capacity);
    }
    // More than the service takes.
    CHECK(media::describe_cabinet(allocator, ConstByteView(fixture.cabinet.data(), media::kCabinetServiceMaxCabinet + 1), ~uint64_t{0}, &few, &files) ==
          CabinetServiceStatus::InvalidParameter);
  }
  CHECK(allocator.live == 0);
}

int main() {
  std::vector<Fixture> fixtures;
  if (!load_manifest(&fixtures)) {
    printf("FAILED: tests/data/cabinet (manifest.txt and the cabinets it names) cannot be read; run from the repository's root\n");
    return 1;
  }
  CHECK(fixtures.size() == 6);
  for (Fixture& fixture : fixtures) {
    printf("%s\n", fixture.name.c_str());
    test_whole(fixture);
    if (g_failed) break;      // (the tests after this one compare with what it read)
    test_truncation(fixture);
    test_bit_flips(fixture);
    test_service(fixture);
  }
  test_lzx_decoder();
  if (const char* list = getenv("CABINET_TEST_FILES")) test_real_cabinets(list);

  if (g_failed) {
    printf("CabinetTest: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("CabinetTest: all %d checks passed\n", g_checks);
  return 0;
}

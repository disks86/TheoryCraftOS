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

// Host tests of NtTcosCommonDialog (the service behind comdlg32's GetOpenFileName, GetSaveFileName and ChooseColor): the program's structure is read, the paths go through the
// process's drive C: to the OS file system and back, the answer is written into the program's buffer, and damaged requests (every truncation, flipped bits) never go wrong.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/CommonDialog.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator A;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kBase = 0x100000, kSize = 0x10000;
struct FakeMemory final : IUserMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  uint64_t limit = kSize;      // the memory ends here (a truncated program)
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > limit || out.size() > limit - (a - kBase)) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = bytes[a - kBase + i];
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > limit || in.size() > limit - (a - kBase)) return false;
    for (uint64_t i = 0; i < in.size(); ++i) bytes[a - kBase + i] = in[i];
    return true;
  }
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(bytes[a - kBase + i]) << (8 * i); return v; }
  // Wide text (ASCII) with its NUL; returns the address after it.
  uint64_t put_wide(uint64_t a, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) { bytes[a - kBase + 2 * i] = uint8_t(s[i]); bytes[a - kBase + 2 * i + 1] = 0; }
    bytes[a - kBase + 2 * s.size()] = 0;
    bytes[a - kBase + 2 * s.size() + 1] = 0;
    return a + 2 * (s.size() + 1);
  }
  std::string get_wide(uint64_t a, uint32_t units) { std::string r; for (uint32_t i = 0; i < units; ++i) r.push_back(char(bytes[a - kBase + 2 * i])); return r; }
};

struct Screen final : ICommonDialogs {
  bool shown = false, busy = false;
  CommonDialogRequest::Kind kind = CommonDialogRequest::Kind::OpenFile;
  std::string title, start, name0, ext0, name1, ext1;
  uint32_t count = 0, filter_index = 0, color = 0;
  // The answer given
  bool accept = true;
  std::string path = "/Windows/Dir/chosen.txt";
  uint32_t answer_filter = 1, answer_color = 0x00AABBCC;
  bool run(const CommonDialogRequest& r, CommonDialogReply* reply) override {
    if (busy) return false;
    shown = true;
    kind = r.kind;
    title = std::string(r.title.c_str());
    start = std::string(r.start_path.view().data(), r.start_path.size());
    count = r.filter_count;
    filter_index = r.filter_index;
    color = r.color;
    if (count > 0) { name0 = r.filters[0].name.c_str(); ext0 = r.filters[0].extensions.c_str(); }
    if (count > 1) { name1 = r.filters[1].name.c_str(); ext1 = r.filters[1].extensions.c_str(); }
    reply->accepted = accept;
    reply->path.assign(StringView(path.data(), path.size()));
    reply->filter_index = answer_filter;
    reply->color = answer_color;
    return true;
  }
};

struct Host final : IProcessHost {
  Screen* screen = nullptr;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
  ICommonDialogs* common_dialogs() override { return screen; }
};

struct Prog {
  FakeMemory memory;
  Host host;
  filesystem::RamFileSystem fs{A};
  Process process;
  Prog() : process(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host) {
    fs.create_directory("/Windows");
    process.files().attach(A, fs, "/Windows");
  }
  // The request at kBase: layout of src/Process/WindowsDialogs.hpp. Strings follow at kBase + 0x1000.
  static constexpr uint64_t kRequest = kBase, kText = kBase + 0x1000, kResult = kBase + 0x8000;
  void request(uint32_t kind, const char* title, const char* start, const char* filters /* "a|b|c|d" pairs, '|' = NUL */, uint32_t capacity = 260) {
    memory.put32(kRequest, 72);
    memory.put32(kRequest + 4, kind);
    memory.put32(kRequest + 8, 0);
    memory.put32(kRequest + 12, 0);
    memory.put64(kRequest + 56, kResult);
    memory.put32(kRequest + 64, capacity);
    memory.put64(kRequest + 32, 0);
    memory.put64(kRequest + 40, 0);
    memory.put64(kRequest + 48, 0);
    uint64_t at = kText;
    if (title) { memory.put64(kRequest + 32, at); at = memory.put_wide(at, title); }
    if (start) { memory.put64(kRequest + 40, at); at = memory.put_wide(at, start); }
    if (filters) {
      memory.put64(kRequest + 48, at);
      std::string list(filters);
      std::string piece;
      for (char c : list) { if (c == '|') { at = memory.put_wide(at, piece); piece.clear(); } else piece.push_back(c); }
      memory.put_wide(at, "");      // the empty name that ends the list
    }
  }
  uint32_t call() {
    SyscallRequest r;
    r.number = uint64_t(WindowsSyscallNumber::NtTcosCommonDialog);
    r.args[0] = kRequest;
    r.user_stack = kBase + 0xF000;
    return uint32_t(syscall_interface(SyscallAbi::Windows, UserArch::X86_64)->dispatch(process, r));
  }
};

static void test_open() {
  Prog p;
  Screen screen;
  p.host.screen = &screen;
  p.request(1, "Open a file", "C:\\Dir\\file.txt", "Text files|*.txt;*.md|All files|*.*|");
  const uint32_t status = p.call();
  CHECK(status == 0);
  CHECK(screen.shown && screen.kind == CommonDialogRequest::Kind::OpenFile);
  CHECK(screen.title == "Open a file");
  CHECK(screen.start == "/Windows/Dir/file.txt");      // (drive C: is the folder /Windows of the OS file system)
  CHECK(screen.count == 2 && screen.name0 == "Text files" && screen.name1 == "All files");
  // (the DLL reduces the patterns before it calls; the service passes the extension lists on as they are)
  CHECK(screen.ext0 == "*.txt;*.md" && screen.ext1 == "*.*");
  CHECK(p.memory.get32(Prog::kRequest + 24) == 1);
  CHECK(p.memory.get32(Prog::kRequest + 12) == 1);      // the filter that was chosen
  const uint32_t length = p.memory.get32(Prog::kRequest + 68);
  CHECK(length == 17 && p.memory.get_wide(Prog::kResult, length + 1) == std::string("C:\\Dir\\chosen.txt\0", 18));
}

static void test_answers() {
  Prog p;
  Screen screen;
  p.host.screen = &screen;
  // cancelled
  screen.accept = false;
  p.request(2, nullptr, nullptr, nullptr);
  CHECK(p.call() == 0 && screen.kind == CommonDialogRequest::Kind::SaveFile && screen.count == 0 && screen.title.empty() && screen.start.empty());
  CHECK(p.memory.get32(Prog::kRequest + 24) == 0 && p.memory.get32(Prog::kRequest + 68) == 0);
  // a buffer that is too small gets the length it needs and nothing written
  screen.accept = true;
  p.request(1, nullptr, nullptr, nullptr, 17);
  CHECK(p.call() == 0xC0000023u && p.memory.get32(Prog::kRequest + 68) == 17 && p.memory.bytes[Prog::kResult - kBase] == 0);
  p.request(1, nullptr, nullptr, nullptr, 18);
  CHECK(p.call() == 0 && p.memory.get_wide(Prog::kResult, 17) == "C:\\Dir\\chosen.txt");
  // a file outside the C: drive cannot be named by a Windows path
  screen.path = "/Desktop/Documents/x.txt";
  p.request(1, nullptr, nullptr, nullptr);
  CHECK(p.call() == 0xC000003Au && p.memory.get32(Prog::kRequest + 24) == 0);
  screen.path = "/Windows";
  p.request(1, nullptr, nullptr, nullptr);
  CHECK(p.call() == 0 && p.memory.get_wide(Prog::kResult, 3) == "C:\\");
  // a dialog that cannot be shown
  screen.busy = true;
  p.request(1, nullptr, nullptr, nullptr);
  CHECK(p.call() == 0xC00000BBu);
}

static void test_color() {
  Prog p;
  Screen screen;
  p.host.screen = &screen;
  p.request(3, "Color", nullptr, nullptr);
  p.memory.put32(Prog::kRequest + 16, 0x00332211);      // a COLORREF is 0x00BBGGRR: red 0x11
  CHECK(p.call() == 0);
  CHECK(screen.kind == CommonDialogRequest::Kind::Color && screen.color == 0x112233 && screen.title == "Color");
  CHECK(p.memory.get32(Prog::kRequest + 24) == 1 && p.memory.get32(Prog::kRequest + 20) == 0x00CCBBAA);
}

static void test_refused() {
  Prog p;
  // no screen
  p.request(1, nullptr, nullptr, nullptr);
  CHECK(p.call() == 0xC00000BBu);
  Screen screen;
  p.host.screen = &screen;
  p.request(1, nullptr, nullptr, nullptr);
  p.memory.put32(Prog::kRequest, 71);      // wrong size
  CHECK(p.call() == 0xC000000Du && !screen.shown);
  p.request(1, nullptr, nullptr, nullptr);
  p.memory.put32(Prog::kRequest + 4, 9);      // unknown kind
  CHECK(p.call() == 0xC000000Du && !screen.shown);
  p.request(1, nullptr, nullptr, nullptr);
  p.memory.put64(Prog::kRequest + 32, 0xDEAD0000);      // a title that is not memory
  CHECK(p.call() == 0xC0000005u && !screen.shown);
  // an unterminated filter list
  p.request(1, nullptr, nullptr, nullptr);
  p.memory.put64(Prog::kRequest + 48, kBase + 0x2000);
  for (uint64_t i = 0; i < kSize - 0x2000; ++i) p.memory.bytes[0x2000 + i] = 'x';
  CHECK(p.call() != 0 && !screen.shown);
}

// Every truncation of the memory the request lives in, and every flipped bit of the request and its strings: the service answers and does not read or write outside.
static void test_damage() {
  Prog p;
  Screen screen;
  p.host.screen = &screen;
  p.request(1, "Open", "C:\\Dir\\file.txt", "Text|*.txt|All|*|");
  const std::vector<uint8_t> good = p.memory.bytes;
  for (uint64_t limit = 0; limit < 0x1100; limit += 3) {
    p.memory.bytes = good;
    p.memory.limit = limit;
    p.call();
  }
  p.memory.limit = kSize;
  for (uint64_t bit = 0; bit < 8 * 0x1100; ++bit) {
    p.memory.bytes = good;
    p.memory.bytes[(bit / 8) < 72 ? bit / 8 : 0x1000 + (bit / 8 - 72)] ^= uint8_t(1u << (bit % 8));
    p.call();
  }
}

int main() {
  test_open();
  test_answers();
  test_color();
  test_refused();
  test_damage();
  if (g_failed) { printf("WindowsDialogTest: %d failed\n", g_failed); return 1; }
  printf("WindowsDialogTest: ok\n");
  return 0;
}

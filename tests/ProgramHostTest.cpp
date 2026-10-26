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
#include <cstdio>
#include <cstdlib>
#include <string>
#include "Program/ProgramHost.hpp"
#include "Desktop/Shell.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/ProgramEnvironment.hpp"

using namespace desktop;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { ++live; return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
static TestAllocator A;
static std::string str(StringView v) { return std::string(v.data(), v.size()); }

static void test_host() {
  ProgramHost host(A);
  String out(A);
  CHECK(!host.take_output(&out) && out.empty());
  const char* a = "hello ";
  CHECK(host.write_console(1, text_bytes(a)) && host.write_console(2, text_bytes("world\n")));
  CHECK(host.take_output(&out) && str(out.view()) == "hello world\n");
  CHECK(!host.take_output(&out));
  uint64_t got = 99;
  uint8_t buffer[4];
  host.close_input();
  CHECK(host.read_console(ByteView(buffer, 4), &got) && got == 0);       // input closed and empty: end of file
  // a program that prints without stopping cannot use up memory
  std::string block(100000, 'x');
  for (int i = 0; i < 20; ++i) host.write_console(1, ConstByteView(reinterpret_cast<const uint8_t*>(block.data()), block.size()));
  out.clear();
  CHECK(host.take_output(&out) && out.size() == ProgramHost::kMaxBuffered && host.dropped() == 20 * 100000 - ProgramHost::kMaxBuffered);
  CHECK(host.write_console(1, ConstByteView()));
  // calls it did not get
  String list(A);
  CHECK(!host.take_unhandled(&list));
  for (uint64_t n = 200; n < 215; ++n) host.unhandled_syscall(process::SyscallAbi::Linux, n);
  CHECK(host.take_unhandled(&list) && str(list.view()).find("Linux system call 200") == 0 && str(list.view()).find("207") != std::string::npos && str(list.view()).find("208") == std::string::npos);
  // time moves forward, sleeping and yielding are harmless
  const uint64_t t0 = host.monotonic_ns();
  host.sleep_ns(0);
  host.yield();
  CHECK(host.monotonic_ns() >= t0);
  uint8_t r[32] = {};
  const bool got_random = host.random_bytes(ByteView(r, 32));
  if (got_random) {
    bool all_zero = true;
    for (uint8_t b : r) all_zero = all_zero && b == 0;
    CHECK(!all_zero);
  }
}

static std::string read_text(ProgramHost& host, uint64_t room, bool* ok = nullptr) {
  uint8_t buffer[64];
  uint64_t got = 99;
  const bool r = host.read_console(ByteView(buffer, room < sizeof(buffer) ? room : sizeof(buffer)), &got);
  if (ok) *ok = r;
  return std::string(reinterpret_cast<char*>(buffer), got);
}

static void test_input() {
  ProgramHost host(A);
  bool at_end = true;
  CHECK(host.console_input_available(&at_end) == 0 && !at_end);          // nothing typed yet: a read would wait
  host.push_input("a\nbc\n");
  CHECK(host.console_input_available(&at_end) == 5 && !at_end);
  CHECK(read_text(host, 64) == "a\n");                       // one line per read, like a terminal
  CHECK(read_text(host, 64) == "bc\n");
  host.push_input("xyz\n");
  CHECK(read_text(host, 2) == "xy" && read_text(host, 2) == "z\n");      // a small buffer gets the line in pieces
  host.push_input("last\n");
  host.close_input();                                                       // end of input: what is queued is still read
  host.push_input("ignored\n");
  CHECK(read_text(host, 64) == "last\n");
  bool ok = false;
  CHECK(host.console_input_available(&at_end) == 0 && at_end);              // closed and empty: a read returns at once
  CHECK(read_text(host, 64, &ok).empty() && ok);                           // then end of file, again and again
  CHECK(read_text(host, 64).empty());
  ProgramHost second(A);
  CHECK(!second.interrupted());
  second.push_input("never read\n");
  second.interrupt();
  CHECK(second.interrupted());
  read_text(second, 64, &ok);
  CHECK(!ok);                                                               // a read after Ctrl+C returns at once, as a failure
  second.sleep_ns(10ull * 1000 * 1000 * 1000);                              // and so does a long sleep
  ProgramHost third(A);
  std::string big(1000, 'q');
  for (int i = 0; i < 100; ++i) third.push_input(StringView((big + "\n").c_str()));      // more than it keeps: the excess is dropped, not queued
  uint64_t total = 0;
  third.close_input();
  for (;;) { const std::string line = read_text(third, 64); if (line.empty()) break; total += line.size(); }
  CHECK(total <= ProgramHost::kMaxInput && total > ProgramHost::kMaxInput - 2000);
}

static std::string name_of(process::SyscallAbi abi, const char* path) {
  String out(A);
  return process::program_name(abi, path, &out) ? str(out.view()) : "<error>";
}
static std::string dir_of(process::SyscallAbi abi, const char* path) {
  String out(A);
  return process::world_directory(abi, path, &out) ? str(out.view()) : "<error>";
}

static void test_environment() {
  using process::SyscallAbi;
  CHECK(name_of(SyscallAbi::Linux, "/Linux/bin/tool") == "/bin/tool");
  CHECK(name_of(SyscallAbi::Linux, "/Linux/tool") == "/tool");
  CHECK(name_of(SyscallAbi::Linux, "/DATA/stuff/tool") == "/tool");               // outside the world: only the name
  CHECK(name_of(SyscallAbi::Linux, "/Linuxes/tool") == "/tool");                  // a different folder that starts alike
  CHECK(name_of(SyscallAbi::Linux, "/Linux") == "/Linux");                         // (not a program, but must not crash)
  CHECK(name_of(SyscallAbi::Windows, "/Windows/tools/app.exe") == "C:\\tools\\app.exe");
  CHECK(name_of(SyscallAbi::Windows, "/Windows/app.exe") == "C:\\app.exe");
  CHECK(name_of(SyscallAbi::Windows, "/Linux/bin/tool.exe") == "C:\\tool.exe");
  CHECK(dir_of(SyscallAbi::Linux, "/Linux") == "/" && dir_of(SyscallAbi::Linux, "/Linux/home/me") == "/home/me");
  CHECK(dir_of(SyscallAbi::Linux, "/") == "/" && dir_of(SyscallAbi::Linux, "/DATA/x") == "/" && dir_of(SyscallAbi::Linux, "/Linuxes") == "/");
  CHECK(dir_of(SyscallAbi::Windows, "/Windows/Users") == "/Users" && dir_of(SyscallAbi::Windows, "/Linux/etc") == "/");
  CHECK(process::default_environment(SyscallAbi::Linux).size() > 3 && process::default_environment(SyscallAbi::Windows).size() > 3);
  bool has_path = false;
  for (const StringView& e : process::default_environment(SyscallAbi::Linux)) has_path |= e.size() > 5 && e.first(5).equals("PATH=");
  CHECK(has_path);
  for (const StringView& e : process::default_environment(SyscallAbi::Windows)) CHECK(e.find('=') != StringView::npos && e.find('=') > 0);
}

// The shell hands a program to the host, with its arguments, and falls back when the host cannot run it.
struct ShellProbe {
  std::string output, ran_path;
  int runs = 0, unsupported = 0;
  std::string arguments;
  bool can_run = true;
};
static void sh_write(void* c, StringView t) { static_cast<ShellProbe*>(c)->output += str(t); }
static bool sh_run(void* c, StringView path, exe::ExecutableKind kind, ArrayView<const StringView> args) {
  auto* p = static_cast<ShellProbe*>(c);
  if (!p->can_run) return false;
  ++p->runs;
  p->ran_path = str(path) + (kind == exe::ExecutableKind::Elf ? " ELF" : " PE");
  p->arguments.clear();
  for (const StringView& a : args) p->arguments += "[" + str(a) + "]";
  return true;
}
static void sh_unsupported(void* c, StringView, exe::ExecutableKind) { ++static_cast<ShellProbe*>(c)->unsupported; }

static void test_shell() {
  filesystem::RamFileSystem fs(A);
  CHECK(fs.create_directory("/bin") == filesystem::Status::Ok);
  const uint8_t elf[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
  UniquePointer<filesystem::IFile> f;
  uint64_t n = 0;
  CHECK(fs.open_file("/bin/tool", filesystem::OpenMode::CreateTruncate, &f) == filesystem::Status::Ok && f->write(0, ConstByteView(elf, sizeof(elf)), &n) == filesystem::Status::Ok);
  f.reset();
  ShellProbe probe;
  Shell shell(A);
  Shell::Host host;
  host.write = sh_write; host.unsupported_executable = sh_unsupported; host.run_executable = sh_run; host.context = &probe;
  shell.set_host(host);
  shell.set_file_system(&fs);
  shell.execute("/bin/tool one \"two words\" three");
  CHECK(probe.runs == 1 && probe.ran_path == "/bin/tool ELF" && probe.arguments == "[one][two words][three]" && probe.unsupported == 0 && probe.output.empty());
  shell.execute("cd /bin");
  shell.execute("./tool");
  CHECK(probe.runs == 2 && probe.ran_path == "/bin/tool ELF" && probe.arguments.empty());
  shell.execute("tool -x");
  CHECK(probe.runs == 3 && probe.arguments == "[-x]");
  probe.can_run = false;                                 // a host that cannot run programs: the old notice
  shell.execute("tool");
  CHECK(probe.runs == 3 && probe.unsupported == 1 && probe.output.find("unsupported executable") != std::string::npos);
  probe.output.clear();
  shell.execute("nothing");
  CHECK(probe.output.find("command not found") != std::string::npos);
  host.run_executable = nullptr;                         // no runner at all
  shell.set_host(host);
  shell.execute("tool");
  CHECK(probe.unsupported == 2);
}

int main() {
  test_host();
  test_input();
  test_environment();
  test_shell();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("ProgramHostTest: all passed\n");
  return 0;
}

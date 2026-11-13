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

// Host-side test of how the Start menu finds the installed Windows and Linux applications: shortcuts in this system's shell32 format, in
// Windows' own format (LinkInfo, 0xBEEF0004 long names) and with a relative path; desktop entries with field codes, quotes, NoDisplay and
// TryExec; path translation that cannot leave /Windows or /Linux; the labels ("Name (Windows)") and their order. Truncated and bit-flipped
// shortcuts and desktop entries do not crash the parsers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Desktop/InstalledApps.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteText.hpp"
#include "Utility/Utf16.hpp"

using namespace desktop;
namespace layout = filesystem::layout;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator g_alloc;

// --- building shortcuts ----------------------------------------------------------------------------------------------------------------

static void put16(ByteBuffer& b, uint16_t v) { uint8_t x[2] = {uint8_t(v), uint8_t(v >> 8)}; b.append(ConstByteView(x, 2)); }
static void put32(ByteBuffer& b, uint32_t v) { uint8_t x[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; b.append(ConstByteView(x, 4)); }
static void put_bytes(ByteBuffer& b, const void* p, uint64_t n) { b.append(ConstByteView(static_cast<const uint8_t*>(p), n)); }
static void put_zeros(ByteBuffer& b, uint64_t n) { for (uint64_t i = 0; i < n; ++i) { uint8_t z = 0; b.append(ConstByteView(&z, 1)); } }
static void put_utf16(ByteBuffer& b, const char* ascii, bool nul) { for (const char* p = ascii; *p; ++p) put16(b, uint8_t(*p)); if (nul) put16(b, 0); }

static const uint8_t kShellLink[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t kMyComputer[16] = {0xE0, 0x4F, 0xD0, 0x20, 0xEA, 0x3A, 0x69, 0x10, 0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D};

static void put_header(ByteBuffer& b, uint32_t flags) {
  put32(b, 0x4C);
  put_bytes(b, kShellLink, 16);
  put32(b, flags);
  put_zeros(b, 4 + 24 + 4 + 4);
  put32(b, 1);          // show command
  put_zeros(b, 2 + 10);
}
static void put_string(ByteBuffer& b, const char* text) { put16(b, uint16_t(strlen(text))); put_utf16(b, text, false); }

// The item id list this system's shell32 writes: This PC, the drive "C:\", one UTF-16 file item per component.
static void put_tcos_list(ByteBuffer& b, const char* const* components, int count) {
  ByteBuffer list(g_alloc);
  put16(list, 20); { uint8_t t[2] = {0x1F, 0x50}; put_bytes(list, t, 2); } put_bytes(list, kMyComputer, 16);
  put16(list, 25); { uint8_t d[5] = {0x23, 'C', ':', '\\', 0}; put_bytes(list, d, 5); } put_zeros(list, 25 - 7);
  for (int i = 0; i < count; ++i) {
    const uint16_t size = uint16_t(14 + 2 * (strlen(components[i]) + 1));
    put16(list, size);
    uint8_t type[2] = {uint8_t(i + 1 == count ? 0x32 : 0x31), 0};
    put_bytes(list, type, 2);
    put_zeros(list, 10);
    put_utf16(list, components[i], true);
  }
  put16(list, 0);
  put16(b, uint16_t(list.size()));
  b.append(list.view());
}

// A file item of Windows' own shell: an 8.3 name, then the 0xBEEF0004 block (version 9) with the long name.
static void put_windows_item(ByteBuffer& list, const char* short_name, const char* long_name, bool file) {
  ByteBuffer item(g_alloc);
  uint8_t type[2] = {uint8_t(file ? 0x32 : 0x31), 0};
  put_bytes(item, type, 2);
  put_zeros(item, 10);
  put_bytes(item, short_name, strlen(short_name) + 1);
  if ((item.size() + 2) & 1) put_zeros(item, 1);
  ByteBuffer block(g_alloc);
  put16(block, 0);      // size, patched below
  put16(block, 9);
  put32(block, 0xBEEF0004);
  put_zeros(block, 46 - 8);
  put_utf16(block, long_name, true);
  put16(block, 0);      // the offset of the block (not used)
  block.writable_view()[0] = uint8_t(block.size());
  block.writable_view()[1] = uint8_t(block.size() >> 8);
  item.append(block.view());
  put16(list, uint16_t(item.size() + 2));
  list.append(item.view());
}

static void write_file(filesystem::IFileSystem& fs, const char* path, ConstByteView data) {
  UniquePointer<filesystem::IFile> file;
  uint64_t put = 0;
  CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
  if (file) CHECK(file->write(0, data, &put) == filesystem::Status::Ok && put == data.size());
}
static void write_text(filesystem::IFileSystem& fs, const char* path, const char* text) { write_file(fs, path, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text))); }
static void make_folders(filesystem::IFileSystem& fs, const char* path) {
  char partial[512];
  for (size_t i = 1; path[i - 1]; ++i) {
    if (path[i] == '/' || path[i] == 0) {
      memcpy(partial, path, i);
      partial[i] = 0;
      fs.create_directory(partial);
    }
  }
}
static void write_pe(filesystem::IFileSystem& fs, const char* path) {
  uint8_t pe[0x48] = {'M', 'Z'};
  pe[0x3C] = 0x40;
  pe[0x40] = 'P'; pe[0x41] = 'E';
  write_file(fs, path, ConstByteView(pe, sizeof pe));
}
static void write_elf(filesystem::IFileSystem& fs, const char* path) {
  uint8_t elf[64] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
  write_file(fs, path, ConstByteView(elf, sizeof elf));
}

// --- the tests -------------------------------------------------------------------------------------------------------------------------

static void test_paths() {
  String out(g_alloc);
  CHECK(windows_path_to_system("C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", &out) && out.equals("/Windows/Program Files/Google/Chrome/Application/chrome.exe"));
  CHECK(windows_path_to_system("c:/Games//Thing/./run.exe", &out) && out.equals("/Windows/Games/Thing/run.exe"));
  CHECK(windows_path_to_system("\"C:\\A B\\x.exe\"", &out) && out.equals("/Windows/A B/x.exe"));
  CHECK(windows_path_to_system("%ProgramFiles(x86)%\\Steam\\steam.exe", &out) && out.equals("/Windows/Program Files (x86)/Steam/steam.exe"));
  CHECK(windows_path_to_system("%programfiles%\\App\\a.exe", &out) && out.equals("/Windows/Program Files/App/a.exe"));
  CHECK(windows_path_to_system("C:\\a\\..\\b.exe", &out) && out.equals("/Windows/b.exe"));
  CHECK(!windows_path_to_system("C:\\..\\Linux\\bin\\sh", &out));     // cannot leave C:\ .
  CHECK(!windows_path_to_system("D:\\x.exe", &out));
  CHECK(!windows_path_to_system("\\\\server\\share\\x.exe", &out));
  CHECK(!windows_path_to_system("x.exe", &out));
  CHECK(!windows_path_to_system("%UNKNOWN%\\x.exe", &out));
  CHECK(!windows_path_to_system("%ProgramFiles%x.exe", &out));
  CHECK(linux_path_to_system("/usr/bin/firefox", &out) && out.equals("/Linux/usr/bin/firefox"));
  CHECK(linux_path_to_system("/", &out) && out.equals("/Linux"));
  CHECK(!linux_path_to_system("/../Windows/x.exe", &out));
  CHECK(!linux_path_to_system("usr/bin/x", &out));
}

static void test_shell_links() {
  ShellLinkInfo info(g_alloc);
  {   // this system's shell32: an id list, Unicode strings
    ByteBuffer b(g_alloc);
    put_header(b, 0x01 | 0x10 | 0x20 | 0x80);
    const char* parts[] = {"Program Files", "Google", "Chrome", "Application", "chrome.exe"};
    put_tcos_list(b, parts, 5);
    put_string(b, "C:\\Program Files\\Google\\Chrome\\Application");
    put_string(b, "--profile-directory=Default");
    put32(b, 0);
    CHECK(parse_shell_link(b.view(), &info));
    CHECK(info.target.equals("C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"));
    CHECK(info.working_directory.equals("C:\\Program Files\\Google\\Chrome\\Application"));
    CHECK(info.arguments.equals("--profile-directory=Default"));
  }
  {   // Windows' own: an id list with 8.3 names and long names, and a LinkInfo (which wins)
    ByteBuffer b(g_alloc);
    put_header(b, 0x01 | 0x02 | 0x80);
    ByteBuffer list(g_alloc);
    put16(list, 20); { uint8_t t[2] = {0x1F, 0x50}; put_bytes(list, t, 2); } put_bytes(list, kMyComputer, 16);
    put16(list, 25); { uint8_t d[5] = {0x2F, 'C', ':', '\\', 0}; put_bytes(list, d, 5); } put_zeros(list, 25 - 7);
    put_windows_item(list, "PROGRA~1", "Program Files", false);
    put_windows_item(list, "MOZILL~1", "Mozilla Firefox", false);
    put_windows_item(list, "firefox.exe", "firefox.exe", true);
    put16(list, 0);
    put16(b, uint16_t(list.size()));
    b.append(list.view());
    const char* base = "C:\\Program Files\\Mozilla Firefox\\firefox.exe";
    const uint32_t header = 0x1C, base_at = header, suffix_at = uint32_t(base_at + strlen(base) + 1), size = suffix_at + 1;
    put32(b, size); put32(b, header); put32(b, 1); put32(b, 0); put32(b, base_at); put32(b, 0); put32(b, suffix_at);
    put_bytes(b, base, strlen(base) + 1);
    put_zeros(b, 1);
    put32(b, 0);
    CHECK(parse_shell_link(b.view(), &info));
    CHECK(info.target.equals(base));
    // Without the LinkInfo the id list alone gives the same path, from the long names.
    ByteBuffer only_list(g_alloc);
    put_header(only_list, 0x01 | 0x80);
    put16(only_list, uint16_t(list.size()));
    only_list.append(list.view());
    put32(only_list, 0);
    CHECK(parse_shell_link(only_list.view(), &info));
    CHECK(info.target.equals(base));
  }
  {   // ANSI strings, a relative path, no target
    ByteBuffer b(g_alloc);
    put_header(b, 0x08);
    const char* rel = "..\\..\\App\\app.exe";
    put16(b, uint16_t(strlen(rel)));
    put_bytes(b, rel, strlen(rel));
    CHECK(parse_shell_link(b.view(), &info));
    CHECK(info.target.empty() && info.relative_path.equals(rel));
  }
  {   // not a shortcut
    uint8_t junk[100] = {0x4C};
    CHECK(!parse_shell_link(ConstByteView(junk, sizeof junk), &info));
    CHECK(!parse_shell_link(ConstByteView(), &info));
  }
}

static void test_desktop_entries() {
  DesktopEntryInfo e(g_alloc);
  CHECK(parse_desktop_entry("# comment\n[Desktop Entry]\nType=Application\nName=Firefox\nName[de]=Feuerfuchs\nExec=firefox %u\nTerminal=false\n"
                            "[Desktop Action new-window]\nName=New Window\nExec=firefox --new-window %u\n", &e));
  CHECK(e.application && !e.hidden && e.name.equals("Firefox") && e.program.equals("firefox") && e.arguments.empty());
  CHECK(parse_desktop_entry("[Desktop Entry]\r\nType = Application\r\nName = My App\r\nExec=\"/opt/My App/run\" --title \"a b\" --pct=100%% %F\r\nPath=/opt/My App\r\n", &e));
  CHECK(e.name.equals("My App") && e.program.equals("/opt/My App/run") && e.path.equals("/opt/My App"));
  CHECK(e.arguments.equals("--title \"a b\" --pct=100%"));
  CHECK(parse_desktop_entry("[Desktop Entry]\nType=Application\nName=Q\nExec=sh -c \"echo \\\\\"hi\\\\\"\"\n", &e));   // Exec=sh -c "echo \\"hi\\"" -> echo "hi"
  CHECK(e.program.equals("sh") && e.arguments.equals("-c 'echo \"hi\"'"));
  CHECK(parse_desktop_entry("[Desktop Entry]\nType=Application\nName=Hidden\nExec=x\nNoDisplay=true\n", &e) && e.hidden);
  CHECK(parse_desktop_entry("[Desktop Entry]\nType=Link\nName=Web\nURL=https://example.com\n", &e) && !e.application);
  CHECK(!parse_desktop_entry("[Desktop Entry]\nType=Application\nName=Bad\nExec=\"unterminated\n", &e));
  CHECK(!parse_desktop_entry("Type=Application\nName=No group\n", &e));
  CHECK(!parse_desktop_entry("[Desktop Entry]\nName=A\n[Desktop Entry]\nName=B\n", &e));
}

static void test_finding() {
  filesystem::RamFileSystem fs(g_alloc);
  for (const char* f : layout::kFolders) fs.create_directory(f);
  for (const char* f : layout::kWindowsFolders) fs.create_directory(f);
  for (const char* f : layout::kLinuxFolders) fs.create_directory(f);
  make_folders(fs, layout::kWindowsStartMenuPrograms);
  make_folders(fs, "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Google Chrome");
  make_folders(fs, "/Windows/Program Files/Google/Chrome/Application");
  make_folders(fs, "/Windows/Program Files/Tool");
  make_folders(fs, "/Windows/Users/Public/AppData/Roaming/Microsoft/Windows/Start Menu/Programs");
  write_pe(fs, "/Windows/Program Files/Google/Chrome/Application/chrome.exe");
  write_pe(fs, "/Windows/Program Files/Tool/tool.exe");
  write_pe(fs, "/Windows/Program Files/Tool/unins000.exe");
  write_text(fs, "/Windows/Program Files/Tool/readme.txt", "not a program");

  auto link_to = [&](const char* lnk, const char* const* parts, int count, const char* args) {
    ByteBuffer b(g_alloc);
    put_header(b, 0x01 | 0x80 | (args ? 0x20 : 0));
    put_tcos_list(b, parts, count);
    if (args) put_string(b, args);
    put32(b, 0);
    write_file(fs, lnk, b.view());
  };
  const char* chrome[] = {"Program Files", "Google", "Chrome", "Application", "chrome.exe"};
  const char* tool[] = {"Program Files", "Tool", "tool.exe"};
  const char* uninstaller[] = {"Program Files", "Tool", "unins000.exe"};
  const char* readme[] = {"Program Files", "Tool", "readme.txt"};
  const char* missing[] = {"Program Files", "Gone", "gone.exe"};
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Google Chrome/Google Chrome.lnk", chrome, 5, "--start-maximized");
  link_to("/Windows/Users/Public/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Google Chrome.lnk", chrome, 5, nullptr);   // the same label twice
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/tool.LNK", tool, 3, nullptr);
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Uninstall Tool.lnk", uninstaller, 3, nullptr);
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Remove Tool.lnk", uninstaller, 3, nullptr);
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Readme.lnk", readme, 3, nullptr);
  link_to("/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Gone.lnk", missing, 3, nullptr);
  write_text(fs, "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/desktop.ini", "[.ShellClassInfo]\n");
  {   // a relative shortcut: from ...\Start Menu\Programs up to C:\ and down to the tool
    ByteBuffer b(g_alloc);
    put_header(b, 0x08 | 0x80);
    put_string(b, "..\\..\\..\\..\\..\\Program Files\\Tool\\tool.exe");
    put32(b, 0);
    write_file(fs, "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Tool (relative).lnk", b.view());
    ByteBuffer escape(g_alloc);
    put_header(escape, 0x08 | 0x80);
    put_string(escape, "..\\..\\..\\..\\..\\..\\Linux\\usr\\bin\\gedit");
    put32(escape, 0);
    write_file(fs, "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Escape.lnk", escape.view());
  }

  make_folders(fs, layout::kLinuxApplications);
  make_folders(fs, layout::kLinuxRootApplications);
  make_folders(fs, "/Linux/opt/My App");
  write_elf(fs, "/Linux/usr/bin/firefox");
  write_elf(fs, "/Linux/usr/bin/gedit");
  write_elf(fs, "/Linux/opt/My App/run");
  write_text(fs, "/Linux/usr/share/applications/firefox.desktop", "[Desktop Entry]\nType=Application\nName=Firefox\nExec=firefox %u\n");
  write_text(fs, "/Linux/usr/share/applications/gedit.desktop", "[Desktop Entry]\nType=Application\nName=Text Editor\nExec=gedit %U\n");
  write_text(fs, "/Linux/root/.local/share/applications/gedit.desktop", "[Desktop Entry]\nType=Application\nName=Text Editor\nExec=/usr/bin/gedit --new-window\n");
  write_text(fs, "/Linux/usr/share/applications/myapp.desktop", "[Desktop Entry]\nType=Application\nName=My App\nExec=\"/opt/My App/run\" %f\nPath=/opt/My App\n");
  write_text(fs, "/Linux/usr/share/applications/hidden.desktop", "[Desktop Entry]\nType=Application\nName=Helper\nExec=gedit\nNoDisplay=true\n");
  write_text(fs, "/Linux/usr/share/applications/absent.desktop", "[Desktop Entry]\nType=Application\nName=Absent\nExec=absent\n");
  write_text(fs, "/Linux/usr/share/applications/tryexec.desktop", "[Desktop Entry]\nType=Application\nName=Needs Absent\nTryExec=absent\nExec=gedit\n");
  write_text(fs, "/Linux/usr/share/applications/escape.desktop", "[Desktop Entry]\nType=Application\nName=Escape\nExec=/../Windows/Program Files/Tool/tool.exe\n");
  write_text(fs, "/Linux/usr/share/applications/notes.txt", "[Desktop Entry]\nType=Application\nName=Not An Entry\nExec=gedit\n");

  List<InstalledApp> apps(g_alloc);
  CHECK(find_installed_apps(fs, g_alloc, &apps));
  const char* expected[] = {"Firefox (Linux)", "Google Chrome (Windows)", "My App (Linux)", "Text Editor (Linux)", "Tool (relative) (Windows)", "tool (Windows)"};
  CHECK(apps.count() == sizeof expected / sizeof expected[0]);
  for (uint64_t i = 0; i < apps.count() && i < sizeof expected / sizeof expected[0]; ++i) {
    if (!apps[i].label.equals(expected[i])) printf("  app %llu is \"%s\", expected \"%s\"\n", (unsigned long long)i, apps[i].label.c_str(), expected[i]);
    CHECK(apps[i].label.equals(expected[i]));
  }
  for (const InstalledApp& app : apps.view()) {
    if (app.label.equals("Google Chrome (Windows)")) {
      CHECK(app.platform == AppPlatform::Windows && app.name.equals("Google Chrome"));
      CHECK(app.program.equals("/Windows/Program Files/Google/Chrome/Application/chrome.exe"));
      CHECK(app.working_directory.equals("/Windows/Program Files/Google/Chrome/Application"));
      CHECK(app.arguments.equals("--start-maximized") || app.arguments.empty());
    }
    if (app.label.equals("Tool (relative) (Windows)")) CHECK(app.program.equals("/Windows/Program Files/Tool/tool.exe"));
    if (app.label.equals("Text Editor (Linux)")) CHECK(app.program.equals("/Linux/usr/bin/gedit") && app.arguments.equals("--new-window"));   // root's entry wins
    if (app.label.equals("Firefox (Linux)")) CHECK(app.program.equals("/Linux/usr/bin/firefox") && app.arguments.empty() && app.working_directory.equals(layout::kLinuxRootHome));
    if (app.label.equals("My App (Linux)")) CHECK(app.program.equals("/Linux/opt/My App/run") && app.working_directory.equals("/Linux/opt/My App") && app.arguments.empty());
  }

  // The command lines the terminal is given.
  String line(g_alloc);
  for (const InstalledApp& app : apps.view()) {
    if (app.label.equals("My App (Linux)")) CHECK(installed_app_command_line(app, &line) && line.equals("\"/Linux/opt/My App/run\""));
    if (app.label.equals("Text Editor (Linux)")) CHECK(installed_app_command_line(app, &line) && line.equals("/Linux/usr/bin/gedit --new-window"));
  }
  CHECK(same_installed_app(apps[0], apps[0]) && !same_installed_app(apps[0], apps[1]));

  // An empty system has none.
  filesystem::RamFileSystem empty(g_alloc);
  List<InstalledApp> none(g_alloc);
  CHECK(find_installed_apps(empty, g_alloc, &none) && none.count() == 0);
}

// Every truncation and every single-bit flip of a shortcut and of a desktop entry: no crash, no read outside (ASan), and nothing left over.
static void test_damage() {
  ByteBuffer link(g_alloc);
  put_header(link, 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80);
  const char* parts[] = {"Program Files", "App", "app.exe"};
  put_tcos_list(link, parts, 3);
  const char* base = "C:\\Program Files\\App\\app.exe";
  const uint32_t header = 0x24, base_at = header, suffix_at = uint32_t(base_at + strlen(base) + 1), ubase_at = suffix_at + 1;
  const uint32_t usuffix_at = uint32_t(ubase_at + 2 * (strlen(base) + 1)), size = usuffix_at + 2;
  put32(link, size); put32(link, header); put32(link, 1); put32(link, 0); put32(link, base_at); put32(link, 0); put32(link, suffix_at);
  put32(link, ubase_at); put32(link, usuffix_at);
  put_bytes(link, base, strlen(base) + 1); put_zeros(link, 1); put_utf16(link, base, true); put16(link, 0);
  put_string(link, "App"); put_string(link, "..\\App\\app.exe"); put_string(link, "C:\\Program Files\\App"); put_string(link, "-x"); put_string(link, "app.ico");
  put32(link, 0);
  ShellLinkInfo info(g_alloc);
  CHECK(parse_shell_link(link.view(), &info) && info.target.equals(base) && info.arguments.equals("-x"));
  for (uint64_t n = 0; n < link.size(); ++n) (void)parse_shell_link(link.view().first(n), &info);
  ByteBuffer flipped(g_alloc);
  for (uint64_t bit = 0; bit < link.size() * 8; ++bit) {
    flipped.clear();
    flipped.append(link.view());
    flipped.writable_view()[bit / 8] ^= uint8_t(1u << (bit % 8));
    (void)parse_shell_link(flipped.view(), &info);
  }

  const char* entry = "[Desktop Entry]\nType=Application\nName=A\\sB\nExec=\"/opt/x y/run\" --a=\"q\\\\\"r\" %F %%\nTryExec=run\nPath=/opt\nNoDisplay=false\n";
  DesktopEntryInfo e(g_alloc);
  CHECK(parse_desktop_entry(entry, &e) && e.name.equals("A B"));
  const ConstByteView bytes = text_as_bytes(entry);
  for (uint64_t n = 0; n < bytes.size(); ++n) (void)parse_desktop_entry(bytes_as_text(bytes.first(n)), &e);
  for (uint64_t bit = 0; bit < bytes.size() * 8; ++bit) {
    flipped.clear();
    flipped.append(bytes);
    flipped.writable_view()[bit / 8] ^= uint8_t(1u << (bit % 8));
    (void)parse_desktop_entry(bytes_as_text(flipped.view()), &e);
  }
}

int main() {
  test_paths();
  test_shell_links();
  test_desktop_entries();
  test_finding();
  test_damage();
  if (g_failed) {
    printf("InstalledAppsTest: %d check(s) FAILED\n", g_failed);
    return 1;
  }
  printf("InstalledAppsTest: all checks passed\n");
  return 0;
}

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

// Host-side tests for the App Manager (src/Desktop/AppMan): the shell's pipes and text filters (src/Program/ShellTools) against the real bash, sed,
// grep, tr, head, tail, sort, uniq, cut, wc and awk of this machine (each command line run by both, the outputs compared); the AppMan catalog; picking
// the download lines out of an install script; and the installer from start to end on a RAM file system with a stand-in network (install, the files
// and the desktop entry it leaves, the Start menu finding the program, update, remove, refusals, cancelling).
//
//   AppManTest                             the tests
//   AppManTest --resolve <script> <cache>  prints the link an AppMan install script would download, curl answered from <cache> (tests/appman_interop.sh)
//   AppManTest --lines <script>            prints the lines of the script that work the link out (what --resolve runs)
//   AppManTest --winget <source2.msix> <id>...          what winget's real catalog says of those packages, fetched as an install fetches it (tests/winget_interop.sh)
//   AppManTest --winget-install <source2.msix> <id>...  and installs them into a RAM file system
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include "Desktop/AppMan/AppManCatalog.hpp"
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Desktop/AppMan/SqliteReader.hpp"
#include "Desktop/AppMan/WingetCatalog.hpp"
#include "Desktop/AppMan/WingetInstaller.hpp"
#include "Desktop/AppMan/Yaml.hpp"
#include "Desktop/InstalledApps.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Media/Zip.hpp"
#include "Program/ShellTools.hpp"
#include "SquashWriter.hpp"

using namespace squash_writer;

using namespace filesystem;
using namespace desktop;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static std::string sv(StringView v) { return std::string(v.data(), v.size()); }
static StringView view(const std::string& s) { return StringView(s.data(), s.size()); }

// ------------------------------------------------------------------ the shell's pipelines, against bash
static std::string bash_result(const std::string& script, const std::string& input) {
  char path[] = "/tmp/appman-input-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return "<no temp file>";
  if (write(fd, input.data(), input.size()) != ssize_t(input.size())) { close(fd); return "<write>"; }
  close(fd);
  const std::string command = "INPUT_FILE='" + std::string(path) + "' LC_ALL=C bash -c '" + script + "; printf %s \"$result\"' 2>/dev/null";
  FILE* p = popen(command.c_str(), "r");
  std::string out;
  char buf[4096];
  size_t n;
  while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  if (p) pclose(p);
  unlink(path);
  return out;
}

static std::string our_result(const std::string& script, const std::string& input) {
  TestAllocator a;
  std::string got;
  {
    RamFileSystem fs(a);
    UniquePointer<IFile> f;
    (void)fs.create_directory("/Linux");
    (void)fs.create_directory("/Linux/tmp");
    if (fs.open_file("/Linux/tmp/input", OpenMode::CreateTruncate, &f) == Status::Ok) {
      uint64_t done = 0;
      (void)f->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(input.data()), input.size()), &done);
    }
    f = UniquePointer<IFile>();
    String value(a);
    const StringView env[] = {"INPUT_FILE=/tmp/input", "PATH=/usr/bin:/bin"};
    if (evaluate_script_variable(a, fs, "/Linux", view(script), "result", ArrayView<const StringView>(env), nullptr, &value)) got = sv(value.view());
    else got = "<unset>";
  }
  if (a.live != 0) { printf("  leak in our shell for: %s\n", script.c_str()); ++g_failed; }
  return got;
}

static void test_pipelines() {
  if (system("command -v bash >/dev/null && command -v sed >/dev/null && command -v grep >/dev/null") != 0) { printf("  (no bash: skipped)\n"); return; }
  const std::string releases =
      "[{\"url\": \"https://api.github.com/repos/o/app/releases/1\", \"tag_name\": \"v1.2.0\", \"assets\": [\n"
      "{\"name\": \"App-1.2.0-arm64.AppImage\", \"browser_download_url\": \"https://github.com/o/app/releases/download/v1.2.0/App-1.2.0-arm64.AppImage\"},\n"
      "{\"name\": \"App-1.2.0-x86_64.AppImage\", \"browser_download_url\": \"https://github.com/o/app/releases/download/v1.2.0/App-1.2.0-x86_64.AppImage\"},\n"
      "{\"name\": \"App-1.2.0-x86_64.AppImage.zsync\", \"browser_download_url\": \"https://github.com/o/app/releases/download/v1.2.0/App-1.2.0-x86_64.AppImage.zsync\"},\n"
      "{\"name\": \"app_1.2.0_amd64.deb\", \"browser_download_url\": \"https://github.com/o/app/releases/download/v1.2.0/app_1.2.0_amd64.deb\"}]},\n"
      "{\"tag_name\": \"v1.10.3-beta\", \"assets\": [{\"browser_download_url\": \"https://github.com/o/app/releases/download/v1.10.3-beta/App-1.10.3-beta-i686.AppImage\"}]}]\n";
  const std::string page = "<html><a href=\"https://example.org/download/app-2.0-x86_64.AppImage\">get</a> <a href='https://example.org/download/app-2.0-aarch64.AppImage'>arm</a>\n"
                           "<a href=\"https://example.org/download/app-1.9-x86_64.AppImage\">old</a></html>\n";
  const std::string text = "Alpha beta\ngamma  delta\tepsilon\nalpha BETA\n\nzeta:eta:theta\nversion 10\nversion 9\nversion 1.10\nversion 1.9\nalpha beta\n";
  struct Case { std::string pipeline; const std::string* input; };
  const std::vector<Case> cases = {
      // The shapes AppMan's install scripts use.
      {"sed 's/[()\",{} ]/\\n/g' | grep -oi \"https.*mage$\" | grep -vi \"i386\\|i686\\|aarch64\\|arm64\\|armv7l\" | head -1", &releases},
      {"sed 's/[()\",{} ]/\\n/g' | grep -oi \"https.*download.*\" | grep -vi \"i386\\|i686\\|armhf\\|aarch64\\|arm64\\|armv7l\" | grep -i \"appimage$\" | head -1", &releases},
      {"tr '\"><  ' '\\n' | grep -i \"http.*x86_64.*appimage$\" | sort --version-sort | tail -1", &page},
      {"tr \"'\\\"><  \" '\\n' | grep -Eoi \"http.*download.*appimage\" | grep -i \"x86_64\\|x86-64\\|amd64\\|x64\" | head -1", &page},
      {"sed 's/[()\",{} ]/\\n/g' | grep -oi \"https.*mage$\" | grep -i \"beta\" | head -1", &releases},
      {"grep -o '\"tag_name\": \"[^\"]*' | head -1 | sed 's/.*\"//'", &releases},
      {"grep -Eo 'v[0-9]+\\.[0-9]+\\.[0-9]+' | sort -Vu | tail -1", &releases},
      {"grep browser_download_url | cut -d '\"' -f 4 | grep -v zsync | head -2", &releases},
      {"grep -o 'https://[^\"]*' | awk -F/ '{print $NF}' | sort -r | uniq | head -3", &releases},
      {"sed -n 's/.*\"tag_name\": \"\\([^\"]*\\)\".*/\\1/p'", &releases},
      {"sed -E 's/(alpha|gamma) ([a-z]+)/<\\2 \\1>/I'", &text},
      // The tools on their own.
      {"grep -i alpha", &text}, {"grep -c a", &text}, {"grep -v -e alpha -e version", &text}, {"grep -x 'version 9'", &text}, {"grep -w beta", &text},
      {"grep -m 2 version", &text}, {"grep -F 'a.b'", &text}, {"grep -E '^(zeta|gamma)'", &text}, {"grep 'eta:[[:alpha:]]*$'", &text},
      {"grep -o '[0-9]\\+'", &text}, {"grep -oE '[a-z]{4,}'", &text}, {"grep 'a\\{2\\}'", &text}, {"grep '^$' | wc -l", &text},
      {"sed 's/a/A/'", &text}, {"sed 's/a/A/g'", &text}, {"sed 's/a/A/2'", &text}, {"sed -n '/version/p'", &text}, {"sed '/^$/d'", &text},
      {"sed '3q'", &text}, {"sed 's#:#/#g;s/eta/ETA/'", &text}, {"sed -e 's/alpha/one/' -e 's/beta/two/'", &text}, {"sed 's/\\(a\\)\\(l\\)/\\2\\1/'", &text},
      {"sed 's/[[:space:]]\\+/_/g'", &text}, {"sed 's/.*/[&]/'", &text}, {"sed '$d'", &text}, {"sed '2!d'", &text}, {"sed 'y/abc/xyz/'", &text},
      {"tr a-z A-Z", &text}, {"tr -d aeiou", &text}, {"tr -s ' '", &text}, {"tr '[:lower:]' '[:upper:]'", &text}, {"tr ':' '\\n'", &text},
      {"head -3", &text}, {"head -n 2", &text}, {"head -n -9", &text}, {"head -c 7", &text}, {"tail -2", &text}, {"tail -n +10", &text}, {"tail -c 5", &text},
      {"sort", &text}, {"sort -r", &text}, {"sort -u", &text}, {"sort -V", &text}, {"sort -f", &text}, {"grep version | sort -t ' ' -n", &text},
      {"sort | uniq -c", &text}, {"sort | uniq -d", &text}, {"cut -c 1-5", &text}, {"cut -d : -f 2", &text}, {"cut -d ' ' -f 1,2 -s", &text},
      {"wc -l", &text}, {"wc -w", &text}, {"awk '{print $2}'", &text}, {"awk -F: '{print $3, $1}'", &text}, {"awk '/version/ {print $NF}'", &text},
      {"rev", &text}, {"cat | head -1", &text},
  };
  int differ = 0;
  for (const Case& c : cases) {
    const std::string script = "result=$(cat \"$INPUT_FILE\" | " + c.pipeline + ")";
    std::string quoted;
    for (char ch : script) quoted += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    const std::string want = bash_result(quoted, *c.input), got = our_result(script, *c.input);
    if (want != got) {
      ++differ;
      printf("  pipeline differs: %s\n    bash: [%s]\n    ours: [%s]\n", c.pipeline.c_str(), want.c_str(), got.c_str());
    }
    ++g_checks;
  }
  g_failed += differ;
  printf("  %zu pipelines compared with bash, %d differ\n", cases.size(), differ);
}

static void test_regex_translation() {
  TestAllocator a;
  struct T { const char* posix; bool ere; const char* want; };
  const T cases[] = {
      {"a\\|b", false, "a|b"},          {"\\(x\\)\\1", false, "(x)\\1"}, {"a+b?", false, "a\\+b\\?"},  {"a\\{2,3\\}", false, "a{2,3}"},
      {"[[:digit:]]", false, "[0-9]"}, {"[^]a]", false, "[^\\]a]"},       {"*a", false, "\\*a"},        {"a^b$c", false, "a\\^b\\$c"},
      {"^a$", false, "^a$"},             {"(a|b)+", true, "(a|b)+"},        {"a/b", false, "a\\/b"},       {"[a\\b]", false, "[a\\\\b]"},
  };
  for (const T& t : cases) {
    String out(a);
    CHECK(shell_tools::translate_regex(t.posix, t.ere, &out));
    if (!out.view().equals(t.want)) { printf("  regex %s -> %s, want %s\n", t.posix, sv(out.view()).c_str(), t.want); ++g_failed; }
  }
  {
    String out(a);
    CHECK(!shell_tools::translate_regex("[abc", false, &out));
    CHECK(!shell_tools::translate_regex("abc\\", false, &out));
  }
  CHECK(shell_tools::version_compare("1.9", "1.10") < 0 && shell_tools::version_compare("v2", "v10") < 0 && shell_tools::version_compare("a", "a") == 0);
  // Malformed and random patterns, options and input: never a crash.
  std::string chars = "[]^$.*\\(){}|+?ab:-/";
  uint32_t seed = 7;
  auto next = [&] { seed = seed * 1103515245 + 12345; return seed >> 16; };
  for (int i = 0; i < 3000; ++i) {
    std::string p;
    for (int k = int(next() % 8); k >= 0; --k) p += chars[next() % chars.size()];
    const char* tools[] = {"grep", "sed", "tr", "cut", "awk", "sort", "head"};
    const StringView tool = tools[next() % 7];
    std::string arg = std::string(tool.equals("sed") ? "s/" : "") + p + (tool.equals("sed") ? "/x/g" : "");
    const StringView args[] = {view(arg), "-c", view(p)};
    String written(a);
    int status = 0;
    (void)shell_tools::run(a, tool, ArrayView<const StringView>(args, 1 + next() % 3), "line one\nab:cd\n\n[x]\n", &written, &status);
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ the catalog
static void test_catalog() {
  TestAllocator a;
  {
    appman::Catalog catalog(a);
    const char* text =
        "\xE2\x97\x86 zed : A code editor. #itsdesktopapp\n"
        "\xE2\x97\x86 0ad : FOSS historical Real Time Strategy game. #itsdesktopapp\n"
        "garbage line\n"
        "\xE2\x97\x86 bad/name : Not an app name.\n"
        "\xE2\x97\x86 tool-cli : Command line tool for C# code #itscliapp\n"
        "\xE2\x97\x86 obsidian : A powerful knowledge base, plain text Markdown files. #itsdesktopapp\n"
        "\xE2\x97\x86 editor-x : Another text editor #\n"
        "\xE2\x97\x86 kdenlive : A powerful Video Editor provided by KDE.\n";
    CHECK(catalog.parse(text));
    CHECK(catalog.count() == 6);
    CHECK(sv(catalog.at(0).name.view()) == "0ad" && sv(catalog.at(5).name.view()) == "zed");
    const appman::CatalogEntry* obsidian = catalog.find("obsidian");
    CHECK(obsidian && sv(obsidian->description.view()) == "A powerful knowledge base, plain text Markdown files." && !obsidian->command_line);
    const appman::CatalogEntry* tool = catalog.find("tool-cli");
    CHECK(tool && tool->command_line && sv(tool->description.view()) == "Command line tool for C# code");
    CHECK(catalog.find("editor-x") && sv(catalog.find("editor-x")->description.view()) == "Another text editor");
    CHECK(!catalog.find("bad/name") && !catalog.find("nothere"));
    List<uint64_t> found(a);
    uint64_t total = 0;
    CHECK(catalog.search("editor", 10, &found, &total));
    CHECK(total == 3 && found.count() == 3);
    CHECK(sv(catalog.at(found.view()[0]).name.view()) == "editor-x");      // the name starts with it, before names and descriptions that contain it
    CHECK(catalog.search("POWERFUL video", 10, &found, &total) && total == 1 && sv(catalog.at(found.view()[0]).name.view()) == "kdenlive");
    CHECK(catalog.search("", 2, &found, &total) && total == 6 && found.count() == 2);
    CHECK(!catalog.parse("nothing here\n"));
  }
  for (const char* name : {"a", "obsidian", "0ad", "lm-studio", "app_1.2+x"}) CHECK(appman::valid_app_name(name));
  for (const char* name : {"", ".", "..", ".hidden", "-x", "a/b", "a b", "a\\b", "\xc3\xa9"}) CHECK(!appman::valid_app_name(name));
  CHECK(appman::valid_app_name(std::string(64, 'a').c_str()) && !appman::valid_app_name(std::string(65, 'a').c_str()));
  // A real copy of the list, if the interop script left one.
  if (const char* real = getenv("APPMAN_CATALOG")) {
    FILE* f = fopen(real, "rb");
    std::string data;
    char buf[65536];
    size_t n;
    while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    if (f) fclose(f);
    appman::Catalog catalog(a);
    CHECK(catalog.parse(view(data)));
    printf("  the real catalog: %llu applications\n", static_cast<unsigned long long>(catalog.count()));
    CHECK(catalog.count() > 5000 && catalog.find("obsidian"));
  }
  CHECK(a.live == 0);
}

static void test_download_lines() {
  TestAllocator a;
  String out(a);
  const char* script =
      "#!/bin/sh\n\n# AM INSTALL SCRIPT VERSION 3.5\nset -u\nAPP=myapp\nSITE=\"owner/repo\"\n\n"
      "[ -n \"$APP\" ] && mkdir -p \"/opt/$APP/tmp\" && cd \"/opt/$APP/tmp\" || exit 1\nprintf \"#!/bin/sh\\nrm -f /usr/local/bin/$APP\" > ../remove\n"
      "  CHANNEL=stable\nversion=$(curl -Ls https://api.github.com/repos/$SITE/releases | \\\n  grep -oi \"https.*mage$\" | head -1)\n"
      "wget \"$version\" || exit 1\nversion0=$(cat /opt/$APP/version)\n";
  CHECK(appman::Installer::download_lines(script, &out));
  CHECK(sv(out.view()) == "APP=myapp\nSITE=\"owner/repo\"\nCHANNEL=stable\nversion=$(curl -Ls https://api.github.com/repos/$SITE/releases |   grep -oi \"https.*mage$\" | head -1)\n");
  CHECK(!appman::Installer::download_lines("#!/bin/sh\nAPP=x\nwget foo\n", &out));
}

// ------------------------------------------------------------------ the installer, with a stand-in network
struct FakeNetwork final : appman::INetwork {
  std::map<std::string, std::string> pages;     // fetch()
  std::map<std::string, std::string> files;     // download()
  std::vector<std::string> fetched, downloaded;
  appman::Progress* cancel_after_first = nullptr;
  bool fetch(StringView url, uint64_t max_bytes, String* body) override {
    fetched.push_back(sv(url));
    auto it = pages.find(sv(url));
    if (it == pages.end() || it->second.size() > max_bytes) return false;
    return body->assign(view(it->second));
  }
  bool download(StringView url, Sink sink, void* context, uint64_t* total) override {
    downloaded.push_back(sv(url));
    auto it = files.find(sv(url));
    if (it == files.end()) return false;
    *total = it->second.size();
    for (size_t at = 0; at < it->second.size(); at += 4096) {
      if (!sink(context, ConstByteView(reinterpret_cast<const uint8_t*>(it->second.data() + at), std::min<size_t>(4096, it->second.size() - at)))) return false;
      if (cancel_after_first) cancel_after_first->cancel = true;
    }
    return true;
  }
};

static std::string read_ram(RamFileSystem& fs, const char* path) {
  UniquePointer<IFile> f;
  if (fs.open_file(path, OpenMode::Read, &f) != Status::Ok) return "<missing>";
  std::string out(f->size(), '\0');
  uint64_t got = 0;
  (void)f->read(0, ByteView(reinterpret_cast<uint8_t*>(out.data()), out.size()), &got);
  return out;
}
static bool exists(RamFileSystem& fs, const char* path) {
  DirectoryEntry e;
  return fs.stat(path, &e) == Status::Ok;
}

static std::string test_appimage(const std::string& marker, bool with_desktop = true) {
  WriterOptions o;
  o.compression = first_available() ? first_available() : uint16_t(kGzip);
  o.store_only = first_available() == 0;
  Tree t;
  t.add("/AppRun", TNode::File, "#!/bin/sh\nexec \"$APPDIR/usr/bin/app\" \"$@\"\n");
  t.add("/usr/bin/app", TNode::File, "\x7F" "ELF" + marker);
  if (with_desktop) {
    t.add("/usr/share/applications/app.desktop", TNode::File,
          "[Desktop Entry]\nName=Test App\nComment=For the tests\nExec=AppRun --no-sandbox %U\nTryExec=AppRun\nIcon=app\nType=Application\nCategories=Utility;\n"
          "[Desktop Action New]\nName=New Window\nExec=AppRun --new-window\n");
    t.add("/app.desktop", TNode::Link, "usr/share/applications/app.desktop");
  }
  t.add("/app.png", TNode::File, "\x89PNG\r\n\x1a\nicon " + marker);
  t.add("/.DirIcon", TNode::Link, "app.png");
  return fake_runtime(true, false, 2, true) + write_squash(t, o);
}

static const char* kScript =
    "#!/bin/sh\n\n# An install script in the form AppMan's take, for the tests\nset -u\nAPP=testapp\nSITE=\"o/app\"\n\n"
    "[ -n \"$APP\" ] && mkdir -p \"/opt/$APP/tmp\" \"/opt/$APP/icons\" && cd \"/opt/$APP/tmp\" || exit 1\n"
    "version=$(curl -Ls https://api.github.com/repos/o/app/releases | sed 's/[()\",{} ]/\\n/g' | grep -oi \"https.*mage$\" | grep -vi \"i386\\|i686\\|aarch64\\|arm64\\|armv7l\" | head -1)\n"
    "wget \"$version\" || exit 1\ncd ..\nmv ./tmp/*mage ./\"$APP\"\n";

static std::string releases_json(const std::string& version) {
  return "[{\"tag_name\": \"" + version + "\", \"assets\": [{\"browser_download_url\": \"https://github.com/o/app/releases/download/" + version + "/App-" + version +
         "-aarch64.AppImage\"}, {\"browser_download_url\": \"https://github.com/o/app/releases/download/" + version + "/App-" + version + "-x86_64.AppImage\"}]}]";
}
static std::string link_of(const std::string& version) { return "https://github.com/o/app/releases/download/" + version + "/App-" + version + "-x86_64.AppImage"; }

static void test_installer() {
  TestAllocator a;
  {
    RamFileSystem fs(a);
    for (const char* d : {"/Linux", "/Linux/opt", "/Linux/usr", "/Linux/usr/local", "/Linux/usr/local/bin", "/Linux/root"}) (void)fs.create_directory(d);
    FakeNetwork net;
    const std::string script_url = sv(appman::kScriptsUrl) + "testapp";
    net.pages[script_url] = kScript;
    net.pages["https://api.github.com/repos/o/app/releases"] = releases_json("v1.0");
    net.files[link_of("v1.0")] = test_appimage("one");
    appman::Installer installer(a, fs, net);
    String detail(a), url(a);
    appman::Progress progress;

    CHECK(installer.resolve("testapp", &url, &detail) == appman::Result::Ok && sv(url.view()) == link_of("v1.0"));
    CHECK(!installer.installed("testapp"));
    const appman::Result r = installer.install("testapp", &progress, &detail);
    if (r != appman::Result::Ok) printf("  install: %s (%s)\n", sv(appman::result_name(r)).c_str(), sv(detail.view()).c_str());
    CHECK(r == appman::Result::Ok);
    CHECK(installer.installed("testapp"));
    CHECK(progress.done == net.files[link_of("v1.0")].size() && progress.total == progress.done);
    CHECK(read_ram(fs, "/Linux/opt/testapp/testapp") == net.files[link_of("v1.0")]);
    CHECK(read_ram(fs, "/Linux/opt/testapp/version") == link_of("v1.0") + "\n");
    CHECK(read_ram(fs, "/Linux/opt/testapp/remove").find("rm -R -f /opt/testapp") != std::string::npos);
    CHECK(read_ram(fs, "/Linux/opt/testapp/icons/testapp") == "\x89PNG\r\n\x1a\nicon one");
    CHECK(!exists(fs, "/Linux/opt/testapp/testapp.part"));
    const std::string desktop = read_ram(fs, "/Linux/usr/local/share/applications/testapp-AM.desktop");
    CHECK(desktop.find("Exec=/opt/testapp/testapp %U\n") != std::string::npos);
    CHECK(desktop.find("Exec=/opt/testapp/testapp --new-window\n") != std::string::npos);
    CHECK(desktop.find("--no-sandbox") == std::string::npos);
    CHECK(desktop.find("Icon=/opt/testapp/icons/testapp\n") != std::string::npos && desktop.find("TryExec=/opt/testapp/testapp\n") != std::string::npos);
    CHECK(desktop.find("Name=Test App\n") != std::string::npos && desktop.find("Categories=Utility;") != std::string::npos);
    // The Start menu finds it, as a Linux program it can start.
    List<InstalledApp> apps(a);
    CHECK(find_installed_apps(fs, a, &apps));
    bool listed = false;
    for (const InstalledApp& app : apps.view())
      if (sv(app.label.view()) == "Test App (Linux)" && sv(app.program.view()) == "/Linux/opt/testapp/testapp" && sv(app.arguments.view()).empty()) listed = true;
    if (!listed) for (const InstalledApp& app : apps.view()) printf("  menu: %s -> %s\n", sv(app.label.view()).c_str(), sv(app.program.view()).c_str());
    CHECK(listed);
    List<appman::InstalledApp> installed(a);
    CHECK(installer.list_installed(&installed) && installed.count() == 1 && sv(installed.view()[0].version.view()) == link_of("v1.0"));

    // Update: nothing new, then a new release.
    CHECK(installer.update("testapp", &progress, &detail) == appman::Result::UpToDate);
    net.pages["https://api.github.com/repos/o/app/releases"] = releases_json("v2.0");
    net.files[link_of("v2.0")] = test_appimage("two");
    CHECK(installer.update("testapp", &progress, &detail) == appman::Result::Ok);
    CHECK(read_ram(fs, "/Linux/opt/testapp/testapp") == net.files[link_of("v2.0")] && read_ram(fs, "/Linux/opt/testapp/version") == link_of("v2.0") + "\n");
    CHECK(read_ram(fs, "/Linux/opt/testapp/icons/testapp") == "\x89PNG\r\n\x1a\nicon two");

    // Cancelled half way: the installed program is still the old one, no .part is left.
    net.pages["https://api.github.com/repos/o/app/releases"] = releases_json("v3.0");
    net.files[link_of("v3.0")] = test_appimage("three") + std::string(20000, 'p');
    appman::Progress cancel;
    net.cancel_after_first = &cancel;
    CHECK(installer.update("testapp", &cancel, &detail) == appman::Result::Cancelled);
    net.cancel_after_first = nullptr;
    CHECK(read_ram(fs, "/Linux/opt/testapp/testapp") == net.files[link_of("v2.0")] && !exists(fs, "/Linux/opt/testapp/testapp.part"));

    // Remove.
    CHECK(installer.remove("testapp", &detail) == appman::Result::Ok);
    CHECK(!installer.installed("testapp") && !exists(fs, "/Linux/opt/testapp") && !exists(fs, "/Linux/usr/local/share/applications/testapp-AM.desktop"));
    CHECK(installer.remove("testapp", &detail) == appman::Result::NotInstalled);
    CHECK(installer.update("testapp", &progress, &detail) == appman::Result::NotInstalled);
    CHECK(installer.list_installed(&installed) && installed.count() == 0);

    // Refusals.
    CHECK(installer.install("../etc", &progress, &detail) == appman::Result::InvalidName);
    CHECK(installer.install("nosuchapp", &progress, &detail) == appman::Result::NoScript);
    net.pages[sv(appman::kScriptsUrl) + "noline"] = "#!/bin/sh\nAPP=noline\nwget https://x/y\n";
    CHECK(installer.install("noline", &progress, &detail) == appman::Result::NoDownload);
    net.pages[sv(appman::kScriptsUrl) + "offline"] = "#!/bin/sh\nAPP=offline\nversion=$(curl -Ls https://unreachable.invalid/ | head -1)\n";
    CHECK(installer.install("offline", &progress, &detail) == appman::Result::NoDownload);
    net.pages[sv(appman::kScriptsUrl) + "weird"] = "#!/bin/sh\nversion=\"not a link; rm -rf /\"\n";
    CHECK(installer.install("weird", &progress, &detail) == appman::Result::NoDownload);
    net.pages[sv(appman::kScriptsUrl) + "zipped"] = "#!/bin/sh\nversion=https://example.org/app.zip\n";
    net.files["https://example.org/app.zip"] = std::string("PK\x03\x04", 4) + std::string(100, 'z');
    CHECK(installer.install("zipped", &progress, &detail) == appman::Result::NotAnAppImage && sv(detail.view()).find("zip") != std::string::npos);
    CHECK(!exists(fs, "/Linux/opt/zipped/zipped") && !exists(fs, "/Linux/opt/zipped/zipped.part"));
    net.pages[sv(appman::kScriptsUrl) + "gone"] = "#!/bin/sh\nversion=https://example.org/gone.AppImage\n";
    CHECK(installer.install("gone", &progress, &detail) == appman::Result::DownloadFailed);
    // An AppImage without a desktop entry still gets one.
    net.pages[sv(appman::kScriptsUrl) + "bare"] = "#!/bin/sh\nversion=https://example.org/bare.AppImage\n";
    net.files["https://example.org/bare.AppImage"] = test_appimage("bare", false);
    CHECK(installer.install("bare", &progress, &detail) == appman::Result::Ok);
    const std::string bare = read_ram(fs, "/Linux/usr/local/share/applications/bare-AM.desktop");
    CHECK(bare.find("Name=bare\n") != std::string::npos && bare.find("Exec=/opt/bare/bare\n") != std::string::npos && bare.find("Icon=/opt/bare/icons/bare\n") != std::string::npos);
  }
  {
    // A full file system: the install fails cleanly.
    RamFileSystem small(a, 64 * 1024);
    for (const char* d : {"/Linux", "/Linux/opt"}) (void)small.create_directory(d);
    FakeNetwork net;
    net.pages[sv(appman::kScriptsUrl) + "big"] = "#!/bin/sh\nversion=https://example.org/big.AppImage\n";
    std::string noise(300000, '\0');
    uint32_t seed = 99;
    for (char& c : noise) { seed = seed * 1103515245 + 12345; c = char(seed >> 24); }      // (does not compress: the image is as big as this)
    net.files["https://example.org/big.AppImage"] = test_appimage(noise);
    appman::Installer installer(a, small, net);
    String detail(a);
    appman::Progress progress;
    const appman::Result r = installer.install("big", &progress, &detail);
    if (r != appman::Result::FileError) printf("  full disk: %s (%s)\n", sv(appman::result_name(r)).c_str(), sv(detail.view()).c_str());
    CHECK(r == appman::Result::FileError);
    DirectoryEntry e;
    CHECK(small.stat("/Linux/opt/big/big.part", &e) == Status::NotFound && small.stat("/Linux/opt/big/big", &e) == Status::NotFound);
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ winget: the Windows applications
// The stand-in winget source of tests/data/winget (made by make_fixtures.py there).
static std::string fixture(const std::string& name) {
  std::string out;
  FILE* f = fopen(("tests/data/winget/" + name).c_str(), "rb");
  if (!f) { printf("  missing tests/data/winget/%s (run the tests from the repository's root)\n", name.c_str()); ++g_failed; return out; }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}
static ConstByteView data_of(const std::string& s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// The network of the fixture: every address of urls.txt, answered with its file.
static void load_winget_site(FakeNetwork* net) {
  const std::string listing = fixture("urls.txt");
  size_t at = 0;
  while (at < listing.size()) {
    const size_t end = listing.find('\n', at), tab1 = listing.find('\t', at), tab2 = listing.find('\t', tab1 + 1);
    if (end == std::string::npos || tab1 > end || tab2 > end) break;
    const std::string kind = listing.substr(at, tab1 - at), url = listing.substr(tab1 + 1, tab2 - tab1 - 1), file = listing.substr(tab2 + 1, end - tab2 - 1);
    (kind == "page" ? net->pages : net->files)[url] = fixture(file);
    at = end + 1;
  }
}

// Runs `decode` on `good` cut short at many lengths and with single bits flipped: it must neither crash nor read outside the data (the sanitizers
// say), whatever it answers.
template <class Decode>
static void damage(const std::string& good, Decode decode) {
  const size_t step = good.size() > 4000 ? good.size() / 997 : 1;
  for (size_t length = 0; length < good.size(); length += step) decode(good.substr(0, length));
  uint32_t seed = 12345;
  for (int i = 0; i < 1500 && !good.empty(); ++i) {
    seed = seed * 1664525u + 1013904223u;
    std::string bad = good;
    bad[(seed >> 8) % bad.size()] ^= char(1u << (seed & 7));
    if (i % 3 == 0) {      // (and a second one, near the start, where the headers are)
      seed = seed * 1664525u + 1013904223u;
      bad[(seed >> 8) % std::min<size_t>(bad.size(), 512)] ^= char(1u << (seed & 7));
    }
    decode(bad);
  }
}

static void test_zip() {
  TestAllocator a;
  {
    const std::string msix = fixture("source2.msix");
    media::MemoryZipSource source(data_of(msix));
    media::ZipReader zip(a, source);
    CHECK(zip.open() && zip.entries().size() == 3);
    const media::ZipEntry* logo = zip.find("Assets/AppPackageStoreLogo.scale-100.png");
    const media::ZipEntry* index = zip.find("Public/index.db");
    CHECK(logo && index && !zip.find("public/index.db"));
    ByteBuffer out(a);
    CHECK(logo && zip.extract(*logo, 1 << 20, &out) && sv(StringView(out.view())) == "\x89PNG\r\n\x1a\nnot really");
    // A member whose sizes and offset are in its ZIP64 extra field, deflated.
    ByteBuffer db(a);
    CHECK(index && index->method == 8 && index->size == 61440 && zip.extract(*index, 1 << 20, &db) && db.size() == 61440);
    CHECK(sv(StringView(db.view().first(15))) == "SQLite format 3");
    // Too large for the caller: nothing is added.
    ByteBuffer small(a);
    CHECK(index && !zip.extract(*index, 1000, &small) && small.size() == 0);
    CHECK(media::crc32(data_of(std::string("123456789"))) == 0xCBF43926u);
    // An ordinary archive (no ZIP64), with a comment after the end record.
    std::string plain;
    {
      const std::string name = "a.txt", data = "hello";
      auto le = [&](std::string& s, uint64_t v, int n) { for (int i = 0; i < n; ++i) s.push_back(char(v >> (8 * i))); };
      std::string local, central;
      le(local, 0x04034B50, 4); le(local, 20, 2); le(local, 0, 2); le(local, 0, 2); le(local, 0, 4); le(local, media::crc32(data_of(data)), 4);
      le(local, 5, 4); le(local, 5, 4); le(local, name.size(), 2); le(local, 0, 2);
      local += name + data;
      le(central, 0x02014B50, 4); le(central, 20, 2); le(central, 20, 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 4); le(central, media::crc32(data_of(data)), 4);
      le(central, 5, 4); le(central, 5, 4); le(central, name.size(), 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 4); le(central, 0, 4);
      central += name;
      plain = local + central;
      le(plain, 0x06054B50, 4); le(plain, 0, 2); le(plain, 0, 2); le(plain, 1, 2); le(plain, 1, 2); le(plain, central.size(), 4); le(plain, local.size(), 4); le(plain, 7, 2);
      plain += "comment";
    }
    media::MemoryZipSource plain_source(data_of(plain));
    media::ZipReader plain_zip(a, plain_source);
    ByteBuffer hello(a);
    CHECK(plain_zip.open() && plain_zip.entries().size() == 1 && plain_zip.extract(plain_zip.entries()[0], 100, &hello) && sv(StringView(hello.view())) == "hello");
    // Not archives.
    for (const std::string& junk : {std::string(), std::string("PK"), std::string(100, 'x'), std::string("PK\x05\x06", 4) + std::string(18, '\xFF')}) {
      media::MemoryZipSource s(data_of(junk));
      media::ZipReader z(a, s);
      CHECK(!z.open());
    }
    // Damaged: never a crash, and what is unpacked is what was packed (the CRC says) or nothing.
    int opened = 0, unpacked = 0;
    damage(msix, [&](const std::string& bad) {
      media::MemoryZipSource s(data_of(bad));
      media::ZipReader z(a, s);
      if (!z.open()) return;
      ++opened;
      for (const media::ZipEntry& entry : z.entries()) {
        ByteBuffer got(a);
        if (z.extract(entry, 1 << 20, &got)) ++unpacked;
      }
    });
    CHECK(opened > 0 && unpacked > 0);
  }
  CHECK(a.live == 0);
}

static void test_sqlite_and_catalog() {
  TestAllocator a;
  {
    const std::string msix = fixture("source2.msix");
    appman::WingetCatalog catalog(a);
    CHECK(catalog.parse_index(data_of(msix)));
    // 13 packages and 400 fillers; the one with the overlong name and the one with the bad identifier are left out.
    CHECK(catalog.count() == 414);
    const appman::WingetPackage* tool = catalog.find("test.portable");      // (any case)
    CHECK(tool && tool->id.view().equals("Test.Portable") && tool->name.view().equals("Portable Tool") && tool->moniker.view().equals("ptool") && tool->version.view().equals("2.0"));
    CHECK(catalog.find("Allen&Heath.Mixer") && !catalog.find("Long.Name") && !catalog.find("Bad/Identifier") && !catalog.find("Test.Port"));
    // Sorted by name.
    bool sorted = true;
    for (uint64_t i = 1; i < catalog.count(); ++i) sorted &= appman::compare_folded(catalog.at(i - 1).name.view(), catalog.at(i).name.view()) <= 0;
    CHECK(sorted);
    // Searching: the moniker and a name that starts with the query first, then names that contain it, then identifiers.
    List<uint64_t> found(a);
    List<uint8_t> tiers(a);
    uint64_t total = 0;
    CHECK(catalog.search("", 1000, &found, &total) && total == 414 && found.count() == 414);
    CHECK(catalog.search("ptool", 10, &found, &total, &tiers) && total == 1 && tiers[0] == 0 && catalog.at(found[0]).id.view().equals("Test.Portable"));
    CHECK(catalog.search("ZIP", 10, &found, &total, &tiers) && total == 1 && catalog.at(found[0]).name.view().equals("Zip Suite") && tiers[0] == 0);
    CHECK(catalog.search("test.", 100, &found, &total, &tiers) && total == 13 && tiers[0] == 2);      // (identifiers only)
    CHECK(catalog.search("filler 39", 5, &found, &total, &tiers) && total == 14 && found.count() == 5 && catalog.at(found[0]).name.view().equals("Filler 390") && tiers[0] == 0);      // (both words anywhere; the names that start with the query first)
    CHECK(catalog.search("39 filler", 100, &found, &total, &tiers) && total == 14 && tiers[0] == 2 && catalog.at(found[0]).name.view().equals("Filler 039"));
    CHECK(catalog.search("no such thing", 5, &found, &total) && total == 0 && found.count() == 0);
    // The kept list holds the same.
    String list(a);
    appman::WingetCatalog again(a);
    CHECK(catalog.write_list(&list) && again.parse_list(list.view()) && again.count() == catalog.count());
    const appman::WingetPackage* same = again.find("Test.Portable");
    CHECK(same && tool && same->version.view().equals("2.0") && memcmp(same->hash, tool->hash, 32) == 0 && same->moniker.view().equals("ptool"));
    CHECK(!again.parse_list("") && !again.parse_list("something else\n") && !again.parse_list("winget-packages 1\nnot\ta\tpackage\n"));
    damage(sv(list.view()), [&](const std::string& bad) {
      appman::WingetCatalog c(a);
      (void)c.parse_list(view(bad));
    });

    // Identifiers.
    CHECK(appman::valid_package_id("Mozilla.Firefox") && appman::valid_package_id("Notepad++.Notepad++") && appman::valid_package_id("Allen&Heath.SQMixPad") &&
          appman::valid_package_id("Chapp\xC3\xA9" "e.PRO"));
    for (const char* bad : {"", ".hidden", "a/b", "a\\b", "a..b", "C:x", "a*b", "tab\there", "ends.", "ends ", " starts", "q?", "a|b", "<a>", "say\"it"}) CHECK(!appman::valid_package_id(bad));

    // The database itself: the table by name, a row that spills into overflow pages, and damage.
    media::MemoryZipSource source(data_of(msix));
    media::ZipReader zip(a, source);
    ByteBuffer db(a);
    CHECK(zip.open() && zip.extract(*zip.find("Public/index.db"), 1 << 20, &db));
    appman::SqliteReader reader(a, db.view());
    uint32_t root = 0;
    CHECK(reader.open() && reader.table_root("packages", &root) && root > 1 && !reader.table_root("no_such_table", &root));
    struct Rows { uint64_t count = 0, longest = 0; int64_t last = 0; bool ordered = true; } rows;
    CHECK(reader.scan_table(root, [](void* context, int64_t rowid, ArrayView<const appman::SqliteValue> columns) {
      Rows& r = *static_cast<Rows*>(context);
      ++r.count;
      r.ordered &= rowid > r.last;
      r.last = rowid;
      if (columns.size() >= 8 && columns[2].text().size() > r.longest) r.longest = columns[2].text().size();
      return columns.size() == 8 && columns[0].kind == appman::SqliteValue::Kind::Null && columns[7].kind == appman::SqliteValue::Kind::Blob && columns[7].bytes.size() == 32;
    }, &rows));
    CHECK(rows.count == 416 && rows.longest == 6000 && rows.ordered);
    const std::string database = sv(StringView(db.view()));
    for (const std::string& junk : {std::string(), std::string("SQLite format 3"), std::string(4096, '\0'), database.substr(0, 100)}) {
      appman::WingetCatalog c(a);
      CHECK(!c.parse_database(data_of(junk)));
    }
    int read = 0;
    damage(database, [&](const std::string& bad) {
      appman::WingetCatalog c(a);
      if (c.parse_database(data_of(bad))) ++read;
    });
    CHECK(read > 0);
    damage(msix, [&](const std::string& bad) {
      appman::WingetCatalog c(a);
      (void)c.parse_index(data_of(bad));
    });
  }
  CHECK(a.live == 0);
}

static void test_yaml() {
  TestAllocator a;
  {
    appman::YamlNode root(a);
    const char* text =
        "# a comment\n---\nPlain: a value  # with a comment\nNumber: 1.8.2\nEmpty:\nQuoted: \"two\\nlines \\xA9 \\u00e9 \\\"q\\\"\"\nSingle: 'it''s'\n"
        "Folded: first part\n  second part\n\n  after a break\nLong: \"a quoted value that the\n  emitter broke, and \\\n  joined\"\n"
        "Literal: |\n  line one\n    indented\n\n  line three\nFoldedBlock: >-\n  one\n  two\nUrl: https://example.org/a:b\n"
        "List:\n- one\n- two: 2\n  three: 3\n- - nested\n  - items\nIndented:\n  - x\n  - y\nMap:\n  Inner:\n    Deep: yes\n  Other: no\nFlow: [a, b]\n\"Quoted Key\": v\nLast: end\n";
    CHECK(appman::parse_yaml(a, text, &root) && root.kind == appman::YamlNode::Kind::Map);
    CHECK(root.text("Plain").equals("a value") && root.text("Number").equals("1.8.2") && root.find("Empty") && root.text("Empty").empty());
    CHECK(sv(root.text("Quoted")) == "two\nlines \xC2\xA9 \xC3\xA9 \"q\"" && root.text("Single").equals("it's"));
    CHECK(sv(root.text("Folded")) == "first part second part\nafter a break");
    CHECK(sv(root.text("Long")) == "a quoted value that the emitter broke, and joined");
    CHECK(sv(root.text("Literal")) == "line one\n  indented\n\nline three" && root.text("FoldedBlock").equals("one two"));
    CHECK(root.text("Url").equals("https://example.org/a:b") && root.text("Flow").equals("[a, b]") && root.text("Quoted Key").equals("v") && root.text("Last").equals("end"));
    const appman::YamlNode* list = root.find("List");
    CHECK(list && list->kind == appman::YamlNode::Kind::Sequence && list->children.count() == 3);
    if (list && list->children.count() == 3) {
      CHECK(list->children[0]->value.view().equals("one") && list->children[1]->text("two").equals("2") && list->children[1]->text("three").equals("3"));
      CHECK(list->children[2]->kind == appman::YamlNode::Kind::Sequence && list->children[2]->children.count() == 2 && list->children[2]->children[1]->value.view().equals("items"));
    }
    const appman::YamlNode* indented = root.find("Indented");
    CHECK(indented && indented->children.count() == 2 && indented->children[1]->value.view().equals("y"));
    const appman::YamlNode* map = root.find("Map");
    CHECK(map && map->find("Inner") && map->find("Inner")->text("Deep").equals("yes") && map->text("Other").equals("no") && !root.find("Deep"));
    // Not YAML of this kind.
    for (const char* bad : {"a: \"never closed\n", "- x\ny: 1\n", "a: 1\n- x\n", "a: \"bad \\q escape\"\n", "key: v\n nonsense\n: x\n"}) {
      appman::YamlNode node(a);
      if (appman::parse_yaml(a, bad, &node)) printf("  parsed: %s\n", bad);
      CHECK(!appman::parse_yaml(a, bad, &node));
    }
    // An empty document, and nesting that is too deep.
    appman::YamlNode empty(a);
    CHECK(appman::parse_yaml(a, "\n# nothing\n", &empty) && empty.kind == appman::YamlNode::Kind::Map && empty.children.count() == 0);
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += std::string(size_t(i) * 2, ' ') + "k:\n";
    CHECK(!appman::parse_yaml(a, view(deep), &empty));
    std::string dashes;
    for (int i = 0; i < 200; ++i) dashes += "- ";
    CHECK(!appman::parse_yaml(a, view(dashes + "x\n"), &empty));
    damage(text, [&](const std::string& bad) {
      appman::YamlNode node(a);
      (void)appman::parse_yaml(a, view(bad), &node);
    });
  }
  CHECK(a.live == 0);
}

static void test_winget_manifests() {
  TestAllocator a;
  {
    FakeNetwork net;
    load_winget_site(&net);
    // The long version list: several MSZIP blocks, each with the ones before it as its dictionary.
    std::string packed, manifest_20;
    for (const auto& page : net.pages) {
      if (page.first.find("packages/Test.Portable/") != std::string::npos) packed = page.second;
      if (page.first.find("manifests/t/Test/Portable/2.0/") != std::string::npos) manifest_20 = page.second;
    }
    const std::string expected = fixture("versions.yaml");
    String text(a);
    CHECK(!packed.empty() && expected.size() > 65536);
    CHECK(appman::WingetInstaller::decode_mszyml(a, data_of(packed), 1 << 20, &text) && sv(text.view()) == expected);
    CHECK(!appman::WingetInstaller::decode_mszyml(a, data_of(packed), 1000, &text));      // longer than the caller allows
    CHECK(!appman::WingetInstaller::decode_mszyml(a, data_of(std::string("not packed at all, just text")), 1 << 20, &text));
    int decoded = 0;
    damage(packed, [&](const std::string& bad) {
      String out(a);
      if (appman::WingetInstaller::decode_mszyml(a, data_of(bad), 1 << 20, &out)) ++decoded;
    });
    (void)decoded;
    // The version asked for, else the first.
    String version(a), path(a), sha(a);
    CHECK(appman::WingetInstaller::find_version(a, view(expected), "1.0", &version, &path, &sha) && version.view().equals("1.0") &&
          path.view().equals("manifests/t/Test/Portable/1.0/16cd") && sha.size() == 64);
    CHECK(appman::WingetInstaller::find_version(a, view(expected), "7.7", &version, &path, &sha) && version.view().equals("2.0"));
    CHECK(!appman::WingetInstaller::find_version(a, "sV: 1.0\nvD:\n- v: 1\n  rP: ../../elsewhere\n  s256H: 00\n", "1", &version, &path, &sha));
    CHECK(!appman::WingetInstaller::find_version(a, "sV: 1.0\n", "1", &version, &path, &sha));

    // A manifest: what each installer does not say, the manifest says for all.
    appman::WingetManifest manifest(a);
    CHECK(!manifest_20.empty() && appman::WingetInstaller::parse_manifest(a, view(manifest_20), &manifest));
    CHECK(manifest.id.view().equals("Test.Portable") && manifest.name.view().equals("Portable Tool") && manifest.version.view().equals("2.0") && manifest.command.view().equals("ptool"));
    CHECK(manifest.installers.count() == 3 && manifest.installers[1]->architecture.view().equals("x64") && manifest.installers[1]->type.view().equals("portable"));
    String why(a);
    const appman::WingetInstallerEntry* chosen = appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kNative, &why);
    CHECK(chosen && chosen->architecture.view().equals("x64") && chosen->url.view().equals("https://example.org/dl/ptool-2.0-x64.exe"));
    const char* others =
        "PackageIdentifier: A.B\nPackageName: B\nPackageVersion: 1\nInstallerType: zip\nNestedInstallerType: portable\nNestedInstallerFiles:\n- RelativeFilePath: shared.exe\n"
        "Installers:\n- Architecture: neutral\n  InstallerUrl: https://example.org/n.zip\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
        "- Architecture: x64\n  InstallerType: msi\n  InstallerUrl: https://example.org/x64.msi\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
        "- Architecture: x86\n  InstallerUrl: https://example.org/x86.zip\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
        "- Architecture: x64\n  InstallerUrl: ftp://example.org/x64.zip\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(appman::WingetInstaller::parse_manifest(a, others, &manifest));
    CHECK(manifest.installers.count() == 4);
    // The x64 ones are a Windows Installer package and not a web address, the x86 one is a 32-bit program: the neutral one it is.
    chosen = appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kNative | appman::WingetInstaller::kSetup, &why);
    CHECK(chosen && chosen->architecture.view().equals("neutral") && chosen->nested.count() == 1 && chosen->nested[0]->path.view().equals("shared.exe"));
    CHECK(!appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kSetup, &why) && why.view().contains("does not come as a setup program"));
    // A setup program: its switches, success codes and product code; taken only where setup programs can be run, and after a portable one.
    const char* setups =
        "PackageIdentifier: A.S\nPackageName: S\nPackageVersion: 1\nInstallerSwitches:\n  SilentWithProgress: /passive\n  Custom: /ALL\nInstallerSuccessCodes:\n- 3010\n- nonsense\n"
        "AppsAndFeaturesEntries:\n- DisplayName: S Deluxe\n  ProductCode: S_is1\nInstallers:\n"
        "- Architecture: x64\n  InstallerType: nullsoft\n  InstallerUrl: https://example.org/s.exe\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
        "- Architecture: neutral\n  InstallerType: portable\n  InstallerUrl: https://example.org/p.exe\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
        "- Architecture: x64\n  InstallerType: exe\n  InstallerSwitches:\n    Custom: /only\n  InstallerUrl: https://example.org/e.exe\n  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000\n";
    CHECK(appman::WingetInstaller::parse_manifest(a, setups, &manifest) && manifest.installers.count() == 3);
    CHECK(manifest.installers[0]->silent.view().equals("/passive") && manifest.installers[0]->custom.view().equals("/ALL") && manifest.installers[0]->product_code.view().equals("S_is1") &&
          manifest.installers[0]->display_name.view().equals("S Deluxe") && manifest.installers[0]->success_codes.count() == 1 && manifest.installers[0]->success_codes[0] == 3010);
    CHECK(manifest.installers[2]->silent.empty() && manifest.installers[2]->custom.view().equals("/only"));      // (an installer's own switches replace the manifest's)
    chosen = appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kNative | appman::WingetInstaller::kSetup, &why);
    CHECK(chosen && chosen->type.view().equals("portable"));
    chosen = appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kSetup, &why);
    CHECK(chosen && chosen->type.view().equals("nullsoft"));
    // Command lines.
    String program(a);
    List<String> words(a);
    CHECK(appman::WingetInstaller::split_command_line(a, "\"C:\\Program Files\\App\\unins000.exe\" /SILENT  \"/DIR=C:\\My App\" /x=\\\"q\\\"", &program, &words));
    CHECK(sv(program.view()) == "C:\\Program Files\\App\\unins000.exe" && words.count() == 3 && words[0].view().equals("/SILENT") && sv(words[1].view()) == "/DIR=C:\\My App" &&
          sv(words[2].view()) == "/x=\"q\"");
    CHECK(appman::WingetInstaller::split_command_line(a, "C:\\Program Files\\My App\\Uninstall.EXE /S", &program, &words) && sv(program.view()) == "C:\\Program Files\\My App\\Uninstall.EXE" &&
          words.count() == 1 && words[0].view().equals("/S"));
    CHECK(appman::WingetInstaller::split_command_line(a, "MsiExec.exe /X{ABC}", &program, &words) && program.view().equals("MsiExec.exe") && words.count() == 1);
    CHECK(appman::WingetInstaller::split_command_line(a, "  /SP- /VERYSILENT\t/NORESTART ", nullptr, &words) && words.count() == 3 && words[2].view().equals("/NORESTART"));
    CHECK(appman::WingetInstaller::split_command_line(a, "", nullptr, &words) && words.count() == 0);
    CHECK(!appman::WingetInstaller::split_command_line(a, "", &program, &words) && !appman::WingetInstaller::split_command_line(a, "\"never closed", &program, &words));
    CHECK(!appman::WingetInstaller::parse_manifest(a, "PackageName: no identifier\nInstallers: []\n", &manifest));
    CHECK(!appman::WingetInstaller::parse_manifest(a, "- a\n- list\n", &manifest));
    damage(manifest_20, [&](const std::string& bad) {
      appman::WingetManifest m(a);
      String w(a);
      if (appman::WingetInstaller::parse_manifest(a, view(bad), &m)) (void)appman::WingetInstaller::choose_installer(m, appman::WingetInstaller::kNative | appman::WingetInstaller::kSetup, &w);
    });

    // The names of archive members.
    String safe(a);
    CHECK(appman::WingetInstaller::safe_member_path("bin\\tool.exe", &safe) && safe.view().equals("bin/tool.exe"));
    CHECK(appman::WingetInstaller::safe_member_path("a/b/", &safe) && safe.view().equals("a/b"));
    for (const char* bad : {"", "/abs", "\\abs", "../up", "a/../b", "a//b", "C:\\x", "a/./b", "nul\x01", "a/b.", "what?", "a/ b /c "}) CHECK(!appman::WingetInstaller::safe_member_path(bad, &safe));

    // A shortcut: written, and read back as the Start menu reads it.
    ByteBuffer link(a);
    ShellLinkInfo info(a);
    CHECK(write_shell_link("C:\\Program Files\\WinGet\\Packages\\Caf\xC3\xA9\\tool.exe", "C:\\Program Files\\WinGet\\Packages\\Caf\xC3\xA9", &link));
    CHECK(parse_shell_link(link.view(), &info) && sv(info.target.view()) == "C:\\Program Files\\WinGet\\Packages\\Caf\xC3\xA9\\tool.exe" &&
          sv(info.working_directory.view()) == "C:\\Program Files\\WinGet\\Packages\\Caf\xC3\xA9" && info.arguments.empty());
    CHECK(!write_shell_link("", "C:\\", &link) && link.size() == 0);
  }
  CHECK(a.live == 0);
}

static bool start_menu_has(RamFileSystem& fs, TestAllocator& a, const char* label, const char* program) {
  List<InstalledApp> apps(a);
  bool listed = false;
  if (find_installed_apps(fs, a, &apps))
    for (const InstalledApp& app : apps.view()) listed |= app.platform == AppPlatform::Windows && app.label.view().equals(label) && app.program.view().equals(program);
  return listed;
}

static void test_winget_installer() {
  TestAllocator a;
  {
    RamFileSystem fs(a);
    for (const char* d : {"/Windows", "/Windows/Program Files", "/Windows/ProgramData"}) (void)fs.create_directory(d);
    FakeNetwork net;
    load_winget_site(&net);
    appman::WingetCatalog catalog(a);
    CHECK(catalog.parse_index(data_of(fixture("source2.msix"))));
    appman::WingetInstaller installer(a, fs, net);
    String detail(a);
    appman::Progress progress;
    auto package = [&](const char* id) -> const appman::WingetPackage& {
      const appman::WingetPackage* found = catalog.find(id);
      if (!found) { printf("  no package %s\n", id); ++g_failed; return catalog.at(0); }
      return *found;
    };
    auto run = [&](appman::Result r, appman::Result want) {
      if (r != want) printf("  got: %s (%s)\n", sv(appman::result_name(r)).c_str(), sv(detail.view()).c_str());
      return r == want;
    };
    const char* kPackages = "/Windows/Program Files/WinGet/Packages";
    const std::string tool_dir = std::string(kPackages) + "/Test.Portable_Microsoft.Winget.Source_8wekyb3d8bbwe";
    const char* kMenu = "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs";

    // A portable program, version 1.0 first (as if the catalog were older), for this computer's CPU.
    appman::WingetPackage older(a);
    CHECK(older.copy_from(package("Test.Portable")) && older.version.assign("1.0"));
    CHECK(!installer.installed("Test.Portable"));
    CHECK(run(installer.install(older, &progress, &detail), appman::Result::Ok));
    CHECK(installer.installed("Test.Portable") && installer.installed("test.portable") == false);      // (the folder is named as the identifier is written)
    CHECK(read_ram(fs, (tool_dir + "/ptool.exe").c_str()) == net.files["https://example.org/dl/ptool-1.0-x64.exe"]);
    CHECK(progress.done == net.files["https://example.org/dl/ptool-1.0-x64.exe"].size() && progress.total == progress.done);
    const std::string record = read_ram(fs, (tool_dir + "/winget-install.txt").c_str());
    CHECK(record.find("PackageIdentifier: Test.Portable\n") != std::string::npos && record.find("PackageVersion: 1.0\n") != std::string::npos &&
          record.find("Shortcut: Portable Tool.lnk\n") != std::string::npos);
    CHECK(!exists(fs, (tool_dir + "/download.part").c_str()));
    CHECK(start_menu_has(fs, a, "Portable Tool (Windows)", (tool_dir + "/ptool.exe").c_str()));
    List<appman::WingetInstalled> apps(a);
    CHECK(installer.list_installed(&apps) && apps.count() == 1 && apps[0].id.view().equals("Test.Portable") && apps[0].name.view().equals("Portable Tool") &&
          apps[0].version.view().equals("1.0"));

    // Update: a download that is cancelled keeps the old version; then the new one; then nothing to do.
    progress.cancel = false;
    net.cancel_after_first = &progress;
    CHECK(run(installer.update(package("Test.Portable"), &progress, &detail), appman::Result::Cancelled));
    net.cancel_after_first = nullptr;
    progress.cancel = false;
    CHECK(read_ram(fs, (tool_dir + "/ptool.exe").c_str()) == net.files["https://example.org/dl/ptool-1.0-x64.exe"] && installer.installed("Test.Portable"));
    CHECK(run(installer.update(package("Test.Portable"), &progress, &detail), appman::Result::Ok));
    CHECK(read_ram(fs, (tool_dir + "/ptool.exe").c_str()) == net.files["https://example.org/dl/ptool-2.0-x64.exe"]);
    CHECK(installer.list_installed(&apps) && apps.count() == 1 && apps[0].version.view().equals("2.0"));
    const size_t fetched = net.fetched.size();
    CHECK(run(installer.update(package("Test.Portable"), &progress, &detail), appman::Result::UpToDate) && net.fetched.size() == fetched);      // (no network needed)
    CHECK(start_menu_has(fs, a, "Portable Tool (Windows)", (tool_dir + "/ptool.exe").c_str()));

    // An archive of two programs: everything unpacked, a shortcut for each.
    const std::string suite_dir = std::string(kPackages) + "/Test.Zipped_Microsoft.Winget.Source_8wekyb3d8bbwe";
    CHECK(run(installer.install(package("Test.Zipped"), &progress, &detail), appman::Result::Ok));
    CHECK(read_ram(fs, (suite_dir + "/bin/alpha.exe").c_str()).find("alpha") != std::string::npos && exists(fs, (suite_dir + "/beta.exe").c_str()));
    CHECK(read_ram(fs, (suite_dir + "/docs/readme.txt").c_str()).size() == 9000 && exists(fs, (suite_dir + "/data").c_str()) && !exists(fs, (suite_dir + "/download.part").c_str()));
    CHECK(start_menu_has(fs, a, "alpha (Windows)", (suite_dir + "/bin/alpha.exe").c_str()) && start_menu_has(fs, a, "beta (Windows)", (suite_dir + "/beta.exe").c_str()));
    // An archive of one: its shortcut has the package's name. An identifier with '&' in it (escaped in the address).
    CHECK(run(installer.install(package("Test.Single"), &progress, &detail), appman::Result::Ok));
    CHECK(start_menu_has(fs, a, "Single App (Windows)", (std::string(kPackages) + "/Test.Single_Microsoft.Winget.Source_8wekyb3d8bbwe/app/Single.exe").c_str()));
    CHECK(run(installer.install(package("Allen&Heath.Mixer"), &progress, &detail), appman::Result::Ok));
    CHECK(exists(fs, (std::string(kPackages) + "/Allen&Heath.Mixer_Microsoft.Winget.Source_8wekyb3d8bbwe/Mixer%20Setup.exe").c_str()));
    CHECK(installer.list_installed(&apps) && apps.count() == 4 && apps[0].name.view().equals("A&H Mixer") && apps[3].name.view().equals("Zip Suite"));

    // A shortcut of the same name that is someone else's is left alone.
    CHECK(run(installer.remove("Test.Single", &detail), appman::Result::Ok));
    {
      UniquePointer<IFile> f;
      uint64_t done = 0;
      CHECK(fs.open_file((std::string(kMenu) + "/Single App.lnk").c_str(), OpenMode::CreateTruncate, &f) == Status::Ok && f->write(0, data_of(std::string("theirs")), &done) == Status::Ok);
    }
    CHECK(run(installer.install(package("Test.Single"), &progress, &detail), appman::Result::Ok));
    CHECK(read_ram(fs, (std::string(kMenu) + "/Single App.lnk").c_str()) == "theirs" && exists(fs, (std::string(kMenu) + "/Single App (Test.Single).lnk").c_str()));
    CHECK(run(installer.remove("Test.Single", &detail), appman::Result::Ok));
    CHECK(read_ram(fs, (std::string(kMenu) + "/Single App.lnk").c_str()) == "theirs" && !exists(fs, (std::string(kMenu) + "/Single App (Test.Single).lnk").c_str()));

    // Remove: the folder and the shortcuts.
    CHECK(run(installer.remove("Test.Zipped", &detail), appman::Result::Ok));
    CHECK(!exists(fs, suite_dir.c_str()) && !exists(fs, (std::string(kMenu) + "/alpha.lnk").c_str()) && !exists(fs, (std::string(kMenu) + "/beta.lnk").c_str()));
    CHECK(!installer.installed("Test.Zipped") && run(installer.remove("Test.Zipped", &detail), appman::Result::NotInstalled));
    CHECK(run(installer.remove("../../Windows", &detail), appman::Result::InvalidName) && exists(fs, "/Windows/Program Files"));
    CHECK(exists(fs, (std::string(kMenu) + "/Portable Tool.lnk").c_str()));      // (the others' stay)

    // Refusals, none of which leaves anything behind.
    auto nothing_left = [&](const char* id) { return !exists(fs, (std::string(kPackages) + "/" + id + "_Microsoft.Winget.Source_8wekyb3d8bbwe").c_str()) && !installer.installed(id); };
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::NotSupported) && detail.view().contains("Inno Setup") && nothing_left("Test.Setup"));
    CHECK(net.downloaded.empty() || net.downloaded.back() != "https://example.org/dl/setup-9.0.exe");      // (not even downloaded: this installer has no way to run a setup program)
    CHECK(run(installer.install(package("Test.ArmOnly"), &progress, &detail), appman::Result::NotSupported) && detail.view().contains("this kind of computer") && nothing_left("Test.ArmOnly"));
    CHECK(run(installer.install(package("Test.BadSum"), &progress, &detail), appman::Result::BadChecksum) && nothing_left("Test.BadSum"));
    CHECK(run(installer.install(package("Test.Evil"), &progress, &detail), appman::Result::NotAnAppImage) && detail.view().contains("not safe") && nothing_left("Test.Evil"));
    CHECK(!exists(fs, (std::string(kPackages) + "/evil.exe").c_str()) && !exists(fs, "/Windows/Program Files/WinGet/evil.exe"));
    CHECK(run(installer.install(package("Test.NotPe"), &progress, &detail), appman::Result::NotAnAppImage) && detail.view().contains("not a Windows program") && nothing_left("Test.NotPe"));
    // A version list or a manifest that is not the one named by what came before it.
    appman::WingetPackage forged(a);
    CHECK(forged.copy_from(package("Test.Zipped")));
    std::string list_url, manifest_url;
    for (const auto& page : net.pages) {
      if (page.first.find("packages/Test.Zipped/") != std::string::npos) list_url = page.first;
      if (page.first.find("manifests/t/Test/Zipped/") != std::string::npos) manifest_url = page.first;
    }
    const std::string good_list = net.pages[list_url], good_manifest = net.pages[manifest_url];
    net.pages[list_url][40] ^= 1;
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::NoManifest) && detail.view().contains("list of versions") && nothing_left("Test.Zipped"));
    net.pages[list_url] = good_list;
    net.pages[manifest_url] += "# changed\n";
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::NoManifest) && detail.view().contains("manifest is not the one") && nothing_left("Test.Zipped"));
    net.pages[manifest_url] = good_manifest;
    net.pages.erase(manifest_url);
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::NoManifest) && nothing_left("Test.Zipped"));
    net.pages[manifest_url] = good_manifest;
    // No such download; a cancelled first install.
    const std::string zip_url = "https://example.org/dl/suite-3.1.zip", zip = net.files[zip_url];
    net.files.erase(zip_url);
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::DownloadFailed) && nothing_left("Test.Zipped"));
    net.files[zip_url] = zip;
    net.cancel_after_first = &progress;
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::Cancelled) && nothing_left("Test.Zipped"));
    net.cancel_after_first = nullptr;
    progress.cancel = false;
    // An archive that is damaged after all (the checksum is the manifest's, so the manifest is "wrong" here: the download is cut short).
    CHECK(run(installer.update(forged, &progress, &detail), appman::Result::NotInstalled));
    // A record someone edited cannot make Remove delete something else.
    CHECK(run(installer.install(forged, &progress, &detail), appman::Result::Ok));
    {
      UniquePointer<IFile> f;
      uint64_t done = 0;
      const std::string edited = "PackageIdentifier: Test.Zipped\nPackageVersion: 3.1\nShortcut: ../../../../Program Files/WinGet/Packages/Test.Portable_Microsoft.Winget.Source_8wekyb3d8bbwe/ptool.exe\nShortcut: Portable Tool.lnk\n";
      CHECK(fs.open_file((suite_dir + "/winget-install.txt").c_str(), OpenMode::CreateTruncate, &f) == Status::Ok && f->write(0, data_of(edited), &done) == Status::Ok);
    }
    CHECK(run(installer.remove("Test.Zipped", &detail), appman::Result::Ok) && exists(fs, (tool_dir + "/ptool.exe").c_str()));

    // A full disk.
    RamFileSystem tiny(a, 600);
    for (const char* d : {"/Windows", "/Windows/Program Files", "/Windows/ProgramData"}) (void)tiny.create_directory(d);
    appman::WingetInstaller cramped(a, tiny, net);
    const appman::Result full = cramped.install(package("Test.Zipped"), &progress, &detail);
    CHECK(full == appman::Result::FileError && !cramped.installed("Test.Zipped"));
  }
  CHECK(a.live == 0);
}

// The Windows world of the tests: a "setup program" that is run makes a program folder with an uninstaller in it and registers it, as Inno Setup does;
// the uninstaller, run, takes both away again.
struct FakeSetupHost final : appman::ISetupHost {
  RamFileSystem& fs;
  TestAllocator& a;
  struct Entry { std::string key, name, version, command, quiet; };
  std::vector<Entry> entries;
  std::vector<std::string> runs;       // "program|argument|argument"
  std::string app = "Setup Program", key = "Setup Program_is1", version = "1";
  uint32_t exit_code = 0;
  bool start = true, registers = true, quiet = false, decoy = false;
  bool* cancel_when_run = nullptr;
  FakeSetupHost(RamFileSystem& f, TestAllocator& al) : fs(f), a(al) {}

  void write(const std::string& path, const std::string& data) {
    UniquePointer<IFile> f;
    uint64_t done = 0;
    if (fs.open_file(view(path), OpenMode::CreateTruncate, &f) == Status::Ok) (void)f->write(0, data_of(data), &done);
  }
  bool run(StringView program, ArrayView<const StringView> arguments, const bool* cancel, uint32_t* code, String* message) override {
    std::string line = sv(program);
    for (const StringView& argument : arguments) line += "|" + sv(argument);
    runs.push_back(line);
    if (!start) { message->assign("it could not be started"); return false; }
    if (cancel_when_run) *cancel_when_run = true;
    if (cancel && *cancel) { message->assign("it was stopped"); return false; }
    *code = exit_code;
    const std::string folder = "/Windows/Program Files/" + app, root = "\\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
    if (sv(program).find("/Windows/Windows/Temp/WinGet/") == 0) {      // the setup program
      if (exit_code != 0 && exit_code != 3010) {      // it fails, and says why in the log it was asked to keep (with a control character and a long line, to be cleaned up)
        for (const StringView& argument : arguments) {
          const std::string word = sv(argument);
          if (word.rfind("/LOG=C:\\", 0) != 0) continue;
          std::string log_file = "/Windows/" + word.substr(8);
          for (char& c : log_file) if (c == '\\') c = '/';
          if (exit_code == 6)      // (a log with an error in the middle: what is quoted starts there, not at the end)
            write(log_file, "starting\r\n2026 Defaulting to Abort for suppressed message box (Abort/Retry/Ignore):\r\n   C:\\x\\unins000.exe\r\n\r\n   MoveFile failed; code 32.\r\n"
                            "2026 User chose Abort.\r\n2026 Rolling back changes.\r\n2026 Deinitializing Setup.\r\n2026 Log closed.\r\n");
          else
            write(log_file, "\xEF\xBB\xBFstarting\r\nline two\r\n" + std::string(600, 'x') + "\r\nSetup\x01 version: 6\r\nCould not create the window.\r\n");
        }
        return true;
      }
      const std::string self = read_ram(fs, sv(program).c_str());
      (void)fs.create_directory(view(folder));
      write(folder + "/app.exe", self);
      write(folder + "/unins000.exe", self);
      for (size_t i = entries.size(); i-- > 0;)
        if (entries[i].key == root + key) entries.erase(entries.begin() + long(i));
      if (decoy) entries.push_back({root + "Helper Runtime", "Helper Runtime", "3", "\"C:\\Program Files\\Helper\\remove.exe\"", ""});
      if (registers) {
        const std::string command = "\"C:\\Program Files\\" + app + "\\unins000.exe\"";
        entries.push_back({root + key, app + " version " + version, version, command, quiet ? command + " /quietly" : ""});
      }
    } else {                                                           // the uninstaller
      (void)fs.remove(view(folder + "/app.exe"));
      (void)fs.remove(view(folder + "/unins000.exe"));
      (void)fs.remove(view(folder));
      for (size_t i = entries.size(); i-- > 0;)
        if (entries[i].key == root + key) entries.erase(entries.begin() + long(i));
    }
    return true;
  }
  bool uninstall_entries(List<UniquePointer<appman::UninstallEntry>>* out) override {
    out->clear();
    for (const Entry& e : entries) {
      UniquePointer<appman::UninstallEntry> entry = UniquePointer<appman::UninstallEntry>::make(a, a);
      if (!entry || !entry->key.assign(view(e.key)) || !entry->name.assign(view(e.name)) || !entry->version.assign(view(e.version)) || !entry->command.assign(view(e.command)) ||
          !entry->quiet_command.assign(view(e.quiet)) || !out->push_back(util::move(entry)))
        return false;
    }
    return true;
  }
};

static void test_winget_setup_programs() {
  TestAllocator a;
  {
    RamFileSystem fs(a);
    for (const char* d : {"/Windows", "/Windows/Program Files", "/Windows/ProgramData", "/Windows/Windows", "/AppStore"}) (void)fs.create_directory(d);
    FakeNetwork net;
    load_winget_site(&net);
    appman::WingetCatalog catalog(a);
    CHECK(catalog.parse_index(data_of(fixture("source2.msix"))));
    FakeSetupHost host(fs, a);
    host.entries.push_back({"\\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Other", "Something Else", "1", "\"C:\\Program Files\\Other\\remove.exe\"", ""});
    appman::WingetInstaller installer(a, fs, net, &host);
    String detail(a);
    appman::Progress progress;
    auto package = [&](const char* id) -> const appman::WingetPackage& {
      const appman::WingetPackage* found = catalog.find(id);
      if (!found) { printf("  no package %s\n", id); ++g_failed; return catalog.at(0); }
      return *found;
    };
    auto run = [&](appman::Result r, appman::Result want) {
      if (r != want) printf("  got: %s (%s)\n", sv(appman::result_name(r)).c_str(), sv(detail.view()).c_str());
      return r == want;
    };
    const char* kRecord = "/AppStore/AppMan/Winget/Test.Setup.txt";
    const char* kTemp = "/Windows/Windows/Temp/WinGet";
    auto temp_empty = [&] {
      UniquePointer<IDirectory> listing;
      DirectoryEntry e;
      return fs.open_directory(kTemp, &listing) != Status::Ok || listing->next(&e) != Status::Ok;
    };

    // An Inno Setup program, version 8.0 first: downloaded, checked, run with the switches of its kind, and what it registered remembered.
    appman::WingetPackage older(a);
    CHECK(older.copy_from(package("Test.Setup")) && older.version.assign("8.0"));
    host.version = "8.0";
    static bool saw_running = false;
    saw_running = false;
    progress.changed = [](void*, const appman::Progress& p) { saw_running |= p.running_setup; };
    CHECK(run(installer.install(older, &progress, &detail), appman::Result::Ok));
    progress.changed = nullptr;
    CHECK(saw_running && !progress.running_setup);
    CHECK(host.runs.size() == 1 && host.runs[0] == "/Windows/Windows/Temp/WinGet/Test.Setup.8.0/setup-8.0.exe|/SP-|/VERYSILENT|/SUPPRESSMSGBOXES|/NORESTART|/LOG=C:\\Windows\\Temp\\WinGet\\Test.Setup.8.0\\setup.log");
    CHECK(temp_empty() && exists(fs, "/Windows/Program Files/Setup Program/app.exe"));
    const std::string record = read_ram(fs, kRecord);
    CHECK(record.find("PackageIdentifier: Test.Setup\n") != std::string::npos && record.find("PackageVersion: 8.0\n") != std::string::npos &&
          record.find("InstallerType: inno\n") != std::string::npos && record.find("Uninstall: \"C:\\Program Files\\Setup Program\\unins000.exe\"\n") != std::string::npos &&
          record.find("UninstallKey: \\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Setup Program_is1\n") != std::string::npos &&
          record.find("QuietUninstall") == std::string::npos);
    CHECK(installer.installed("Test.Setup"));
    List<appman::WingetInstalled> apps(a);
    CHECK(installer.list_installed(&apps) && apps.count() == 1 && apps[0].id.view().equals("Test.Setup") && apps[0].name.view().equals("Setup Program") && apps[0].version.view().equals("8.0"));

    // Update: the new version's setup program, over the old one (whose registry entry is there already).
    host.version = "9.0";
    CHECK(run(installer.update(package("Test.Setup"), &progress, &detail), appman::Result::Ok));
    CHECK(host.runs.size() == 2 && host.runs[1].find("/Test.Setup.9.0/setup-9.0.exe|/SP-") != std::string::npos && temp_empty());
    CHECK(read_ram(fs, kRecord).find("PackageVersion: 9.0\n") != std::string::npos && read_ram(fs, kRecord).find("unins000.exe") != std::string::npos);
    CHECK(run(installer.update(package("Test.Setup"), &progress, &detail), appman::Result::UpToDate) && host.runs.size() == 2);
    // Remove: its uninstaller, with the quiet switches of its kind.
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::Ok));
    CHECK(host.runs.size() == 3 && host.runs[2] == "/Windows/Program Files/Setup Program/unins000.exe|/VERYSILENT|/SUPPRESSMSGBOXES|/NORESTART");
    CHECK(!installer.installed("Test.Setup") && !exists(fs, kRecord) && !exists(fs, "/Windows/Program Files/Setup Program") && host.entries.size() == 1);
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::NotInstalled));

    // An "exe" installer: the manifest's own switches (the silent ones, then the custom ones), an exit code the manifest calls success, and - among
    // two new registry entries - the one with the manifest's product code, whose quiet uninstall command is used as it is.
    host.runs.clear();
    host.app = "Exe App";
    host.key = "{11111111-2222-3333-4444-555555555555}";
    host.version = "2.5";
    host.exit_code = 3010;
    host.decoy = host.quiet = true;
    CHECK(run(installer.install(package("Test.ExeSwitch"), &progress, &detail), appman::Result::Ok));
    CHECK(host.runs.size() == 1 && host.runs[0] == "/Windows/Windows/Temp/WinGet/Test.ExeSwitch.2.5/Exe%20App%20Setup.exe|/quiet|/norestart|/D=C:\\Program Files\\Exe App|/NOICONS");
    const std::string exe_record = read_ram(fs, "/AppStore/AppMan/Winget/Test.ExeSwitch.txt");
    CHECK(exe_record.find("{11111111-2222-3333-4444-555555555555}\n") != std::string::npos && exe_record.find("Helper") == std::string::npos &&
          exe_record.find("Uninstall: \"C:\\Program Files\\Exe App\\unins000.exe\" /quietly\nQuietUninstall: yes\n") != std::string::npos);
    host.exit_code = 0;
    host.decoy = host.quiet = false;
    CHECK(run(installer.remove("Test.ExeSwitch", &detail), appman::Result::Ok) && host.runs.back() == "/Windows/Program Files/Exe App/unins000.exe|/quietly");

    // Refusals: nothing is run, nothing stays.
    host.runs.clear();
    CHECK(run(installer.install(package("Test.ExeNoSwitch"), &progress, &detail), appman::Result::NotSupported) && detail.view().contains("without asking questions"));
    CHECK(run(installer.install(package("Test.Msi"), &progress, &detail), appman::Result::NotSupported) && detail.view().contains("Windows Installer"));
    CHECK(run(installer.install(package("Test.Setup32"), &progress, &detail), appman::Result::NotSupported) && detail.view().contains("32-bit"));
    CHECK(host.runs.empty() && temp_empty() && installer.list_installed(&apps) && apps.count() == 0);
    // A 32-bit setup program of Nullsoft's kind is run (and so is its uninstaller, which is 32-bit too); the one of no known kind above was refused.
    host.app = "Nsis App";
    host.key = "Nsis App";
    host.version = "3.1";
    CHECK(run(installer.install(package("Test.Nsis32"), &progress, &detail), appman::Result::Ok));
    CHECK(host.runs.size() == 1 && host.runs[0] == "/Windows/Windows/Temp/WinGet/Test.Nsis32.3.1/nsis32.exe|/S" && temp_empty());
    CHECK(read_ram(fs, "/AppStore/AppMan/Winget/Test.Nsis32.txt").find("InstallerType: nullsoft\n") != std::string::npos && installer.installed("Test.Nsis32"));
    CHECK(run(installer.remove("Test.Nsis32", &detail), appman::Result::Ok) && host.runs.size() == 2 && host.runs[1] == "/Windows/Program Files/Nsis App/unins000.exe|/S");
    CHECK(!installer.installed("Test.Nsis32") && installer.list_installed(&apps) && apps.count() == 0);
    host.runs.clear();
    // A setup program that fails, one that cannot be started, one that is cancelled while it runs.
    host.app = "Setup Program";
    host.key = "Setup Program_is1";
    host.exit_code = 5;
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::SetupFailed) && detail.view().contains("code 5") && !installer.installed("Test.Setup") && temp_empty());
    // (an Inno Setup program's log: its last three lines, cut to 400 characters and to plain text, are the reason given)
    CHECK(detail.view().contains("its log ends: ") && detail.view().ends_with("|Setup  version: 6|Could not create the window.") && !detail.view().contains("line two") &&
          detail.size() < 480);
    host.exit_code = 6;      // (and from the first error it reports, when it reports one)
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::SetupFailed) && detail.view().contains("code 6; its log ends: 2026 Defaulting to Abort") &&
          detail.view().contains("MoveFile failed; code 32.") && detail.view().contains("User chose Abort.") && !detail.view().contains("Log closed") && temp_empty());
    host.exit_code = 0;
    host.start = false;
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::SetupFailed) && detail.view().contains("could not be started") && temp_empty());
    host.start = true;
    host.cancel_when_run = &progress.cancel;
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::Cancelled) && !installer.installed("Test.Setup") && temp_empty() && !progress.running_setup);
    host.cancel_when_run = nullptr;
    progress.cancel = false;
    // A download that is not the one named is never run.
    host.runs.clear();
    std::string& download = net.files["https://example.org/dl/setup-9.0.exe"];
    const std::string good = download;
    download[600] ^= 1;
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::BadChecksum) && host.runs.empty() && temp_empty());
    download = good;

    // The uninstaller is gone (the program was removed some other way): the package is forgotten.
    host.version = "9.0";
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::Ok) && installer.installed("Test.Setup"));
    CHECK(fs.remove("/Windows/Program Files/Setup Program/unins000.exe") == Status::Ok);
    host.runs.clear();
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::Ok) && host.runs.empty() && !installer.installed("Test.Setup"));
    // An uninstaller that fails leaves the package on record.
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::Ok));
    host.exit_code = 2;
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::SetupFailed) && detail.view().contains("code 2") && installer.installed("Test.Setup"));
    host.exit_code = 0;
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::Ok));
    // A setup program that registers no uninstaller: Remove says so, and forgets the package when asked again.
    host.registers = false;
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::Ok) && read_ram(fs, kRecord).find("Uninstall") == std::string::npos);
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::NoUninstaller) && installer.installed("Test.Setup"));
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::Ok) && !installer.installed("Test.Setup"));
    host.registers = true;
    // A record whose uninstall command names something that is not a program of the C: drive is not run.
    CHECK(run(installer.install(package("Test.Setup"), &progress, &detail), appman::Result::Ok));
    host.write(kRecord, "PackageIdentifier: Test.Setup\nPackageVersion: 9.0\nInstallerType: inno\nUninstall: MsiExec.exe /X{ABC}\n");
    host.runs.clear();
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::SetupFailed) && detail.view().contains("MsiExec.exe") && host.runs.empty());
    host.write(kRecord, "PackageIdentifier: Test.Setup\nPackageVersion: 9.0\nInstallerType: inno\nUninstall: \"C:\\..\\Linux\\bin\\sh\" -c x\n");
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::SetupFailed) && host.runs.empty());

    // A package installed by a setup program stays one: without a way to run setup programs it cannot be updated or removed, and says so.
    host.write(kRecord, "PackageIdentifier: Test.Setup\nPackageName: Setup Program\nPackageVersion: 8.0\nInstallerType: inno\nUninstall: \"C:\\Program Files\\Setup Program\\unins000.exe\"\n");
    appman::WingetInstaller plain(a, fs, net);
    CHECK(plain.installed("Test.Setup") && plain.list_installed(&apps) && apps.count() == 1);
    CHECK(run(plain.update(package("Test.Setup"), &progress, &detail), appman::Result::NotSupported));
    CHECK(run(plain.remove("Test.Setup", &detail), appman::Result::NotSupported) && plain.installed("Test.Setup"));
    CHECK(run(installer.remove("Test.Setup", &detail), appman::Result::Ok));
  }
  CHECK(a.live == 0);
}

// With WINGET_SOURCE naming a copy of the real https://cdn.winget.microsoft.com/cache/source2.msix: the real index is read.
static void test_real_winget_source() {
  const char* path = getenv("WINGET_SOURCE");
  if (!path) return;
  std::string msix;
  FILE* f = fopen(path, "rb");
  char buf[65536];
  size_t n;
  while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) msix.append(buf, n);
  if (f) fclose(f);
  TestAllocator a;
  {
    appman::WingetCatalog catalog(a);
    CHECK(catalog.parse_index(data_of(msix)));
    printf("  the real source: %llu packages\n", (unsigned long long)catalog.count());
    CHECK(catalog.count() > 5000 && catalog.find("Microsoft.PowerToys") && catalog.find("jqlang.jq"));
    String list(a);
    appman::WingetCatalog again(a);
    CHECK(catalog.write_list(&list) && again.parse_list(list.view()) && again.count() == catalog.count());
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ real install scripts (tests/appman_interop.sh)
// curl answered from files in `cache`: the URL with every character other than a letter or a digit made "_", at most 200 of them (as the script's curl
// stand-in for bash names them).
struct CachedNetwork final : IScriptNetwork {
  std::string cache;
  bool fetch(StringView url, uint64_t max_bytes, String* body) override {
    std::string key;
    for (char c : sv(url)) key += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    if (key.size() > 200) key.resize(200);
    FILE* f = fopen((cache + "/" + key).c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return data.size() <= max_bytes && body->assign(view(data));
  }
};

static int resolve_real(const char* script_path, const char* cache) {
  TestAllocator a;
  FILE* f = fopen(script_path, "rb");
  if (!f) return 2;
  std::string script;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) script.append(buf, n);
  fclose(f);
  String lines(a), value(a);
  if (!appman::Installer::download_lines(view(script), &lines)) { printf("<no version line>\n"); return 1; }
  if (!cache) {
    printf("%s", sv(lines.view()).c_str());
    return 0;
  }
  RamFileSystem fs(a);
  CachedNetwork net;
  net.cache = cache;
  const StringView env[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/root"};
  if (!evaluate_script_variable(a, fs, "/Linux", lines.view(), "version", ArrayView<const StringView>(env), &net, &value)) { printf("<unset>\n"); return 1; }
  printf("%s\n", sv(value.view()).c_str());
  return 0;
}

// ------------------------------------------------------------------ the real winget source (tests/winget_interop.sh)
// The network, by this machine's curl (run directly, never through a shell: the addresses come from the catalog).
struct CurlNetwork final : appman::INetwork {
  static bool run(const std::string& url, std::string* out) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t child = fork();
    if (child == 0) {
      dup2(fds[1], 1);
      close(fds[0]);
      close(fds[1]);
      execlp("curl", "curl", "-sfL", "--max-time", "300", "--", url.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(fds[1]);
    char buf[65536];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out->append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  bool fetch(StringView url, uint64_t max_bytes, String* body) override {
    std::string out;
    return run(sv(url), &out) && out.size() <= max_bytes && body->assign(view(out));
  }
  bool download(StringView url, Sink sink, void* context, uint64_t* total) override {
    std::string out;
    if (!run(sv(url), &out)) return false;
    *total = out.size();
    for (size_t at = 0; at < out.size(); at += 65536)
      if (!sink(context, ConstByteView(reinterpret_cast<const uint8_t*>(out.data() + at), std::min<size_t>(65536, out.size() - at)))) return false;
    return true;
  }
};

// Reads the real source at `source`, then for each identifier: the manifest of its latest version (checked against the index, as an install does),
// which installer would be taken, and - with `install` - the install itself, into a RAM file system, with what the Start menu then lists.
static int winget_real(const char* source, bool install, int count, char** ids) {
  std::string msix;
  FILE* f = fopen(source, "rb");
  char buf[65536];
  size_t n;
  while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) msix.append(buf, n);
  if (f) fclose(f);
  TestAllocator a;
  int failed = 0;
  {
    appman::WingetCatalog catalog(a);
    if (!catalog.parse_index(data_of(msix))) { printf("the source could not be read\n"); return 1; }
    printf("%llu packages\n", (unsigned long long)catalog.count());
    RamFileSystem fs(a);
    for (const char* d : {"/Windows", "/Windows/Program Files", "/Windows/ProgramData"}) (void)fs.create_directory(d);
    CurlNetwork net;
    appman::WingetInstaller installer(a, fs, net);
    for (int i = 0; i < count; ++i) {
      const appman::WingetPackage* package = catalog.find(ids[i]);
      if (!package) { printf("%s: not in the catalog\n", ids[i]); ++failed; continue; }
      appman::WingetManifest manifest(a);
      String detail(a), why(a);
      const appman::Result resolved = installer.resolve(*package, &manifest, &detail);
      if (resolved != appman::Result::Ok) { printf("%s: %s (%s)\n", ids[i], sv(appman::result_name(resolved)).c_str(), sv(detail.view()).c_str()); ++failed; continue; }
      const appman::WingetInstallerEntry* entry = appman::WingetInstaller::choose_installer(manifest, appman::WingetInstaller::kNative | appman::WingetInstaller::kSetup, &why);
      printf("%s %s \"%s\": %llu installers; %s\n", sv(manifest.id.view()).c_str(), sv(manifest.version.view()).c_str(), sv(manifest.name.view()).c_str(),
             (unsigned long long)manifest.installers.count(), entry ? (sv(entry->type.view()) + " " + sv(entry->architecture.view()) + " " + sv(entry->url.view())).c_str() : sv(why.view()).c_str());
      if (!install || !entry) continue;
      appman::Progress progress;
      const appman::Result r = installer.install(*package, &progress, &detail);
      printf("  install: %s %s (%llu bytes)\n", sv(appman::result_name(r)).c_str(), sv(detail.view()).c_str(), (unsigned long long)progress.done);
      if (r != appman::Result::Ok) { ++failed; continue; }
      List<InstalledApp> apps(a);
      if (find_installed_apps(fs, a, &apps))
        for (const InstalledApp& app : apps.view()) printf("  start menu: %s -> %s\n", sv(app.label.view()).c_str(), sv(app.program.view()).c_str());
      if (installer.remove(package->id.view(), &detail) != appman::Result::Ok) { printf("  remove failed\n"); ++failed; }
    }
  }
  if (a.live != 0) { printf("leak\n"); ++failed; }
  return failed ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && (std::string(argv[1]) == "--winget" || std::string(argv[1]) == "--winget-install")) return winget_real(argv[2], std::string(argv[1]) == "--winget-install", argc - 3, argv + 3);
  if (argc == 4 && std::string(argv[1]) == "--resolve") return resolve_real(argv[2], argv[3]);
  if (argc == 3 && std::string(argv[1]) == "--lines") return resolve_real(argv[2], nullptr);
  printf("pipelines\n");
  test_pipelines();
  printf("regular expressions\n");
  test_regex_translation();
  printf("catalog\n");
  test_catalog();
  printf("install scripts\n");
  test_download_lines();
  printf("installer\n");
  test_installer();
  printf("zip archives\n");
  test_zip();
  printf("winget: the index\n");
  test_sqlite_and_catalog();
  printf("winget: YAML\n");
  test_yaml();
  printf("winget: version lists, manifests, shortcuts\n");
  test_winget_manifests();
  printf("winget: installer\n");
  test_winget_installer();
  printf("winget: setup programs\n");
  test_winget_setup_programs();
  test_real_winget_source();
  printf(g_failed ? "AppManTest: %d of %d checks FAILED\n" : "AppManTest: all %d checks passed\n", g_failed ? g_failed : g_checks, g_checks);
  return g_failed ? 1 : 0;
}

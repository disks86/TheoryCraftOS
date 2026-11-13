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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include "Desktop/AppMan/AppManCatalog.hpp"
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Desktop/InstalledApps.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
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

int main(int argc, char** argv) {
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
  printf(g_failed ? "AppManTest: %d of %d checks FAILED\n" : "AppManTest: all %d checks passed\n", g_failed ? g_failed : g_checks, g_checks);
  return g_failed ? 1 : 0;
}

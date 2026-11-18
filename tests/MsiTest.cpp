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

// Host tests for the Windows Installer engine (src/Msi) and the compound file reader under it (src/Media/CompoundFile.hpp). The packages of
// tests/data/msi are made by make_fixtures.py there (and listed by 7z's own compound file reader when the script runs); files.txt says what each of
// the product's files must come out as. The tests read the packages (streams, string pool, tables, summary), evaluate conditions and formatted text,
// install the product into a RAM file system with a stand-in registry and program runner and check every file, registry value, shortcut and program
// run, remove it again, go through the refusals and failures, and feed the reader and the engine packages cut short and with single bits flipped.
//
//   MsiTest                 the tests
//   MsiTest --list <file>      what a real package holds: its properties, tables and execute sequence (to look at one from the network)
//   MsiTest --install <file>   installs a real package into a RAM file system and removes it again, saying what was written, run and skipped
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Media/Zip.hpp"
#include "Msi/MsiDatabase.hpp"
#include "Msi/MsiExpression.hpp"
#include "Msi/MsiInstaller.hpp"

using namespace filesystem;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;
static std::string sv(StringView v) { return std::string(v.data(), v.size()); }
static StringView view(const std::string& s) { return StringView(s.data(), s.size()); }
static ConstByteView bytes_view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static const std::string kData = "tests/data/msi/";

static bool read_host_file(const std::string& path, Bytes* data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  data->clear();
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data->insert(data->end(), chunk, chunk + n);
  fclose(f);
  return true;
}

static std::string read_ram(RamFileSystem& fs, const std::string& path) {
  UniquePointer<IFile> f;
  if (fs.open_file(view(path), OpenMode::Read, &f) != Status::Ok) return "<missing>";
  std::string out(f->size(), '\0');
  uint64_t got = 0;
  if (!out.empty()) (void)f->read(0, ByteView(reinterpret_cast<uint8_t*>(out.data()), out.size()), &got);
  return out;
}
static bool exists(RamFileSystem& fs, const std::string& path) {
  DirectoryEntry e;
  return fs.stat(view(path), &e) == Status::Ok;
}
static void write_ram(RamFileSystem& fs, const std::string& path, const std::string& data) {
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) (void)fs.create_directory(view(path.substr(0, slash)));
  UniquePointer<IFile> f;
  uint64_t done = 0;
  if (fs.open_file(view(path), OpenMode::CreateTruncate, &f) == Status::Ok && !data.empty())
    (void)f->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), data.size()), &done);
}

// ASCII text as the registry keeps it: UTF-16LE with its terminator.
static std::string utf16(const std::string& text) {
  std::string out;
  for (const char c : text) out += c, out += '\0';
  return out + std::string(2, '\0');
}
static std::string lower(std::string text) {
  for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return text;
}

// The Windows world of the tests: a registry that is a map, programs that are only noted (with what they were at the time), shortcuts that are a
// line of text.
struct FakeHost final : msi::IHost {
  RamFileSystem& fs;
  struct Value { uint32_t type; std::string data; };
  std::map<std::string, Value> values;      // "root|key|name", key and name in lower case
  std::set<std::string> keys;               // "root|key"
  std::vector<std::string> runs;            // "program|arguments|what the program file held"
  std::map<std::string, uint32_t> exit_codes;      // by a piece of the arguments
  bool start = true;
  bool* cancel_when_run = nullptr;
  explicit FakeHost(RamFileSystem& f) : fs(f) {}

  static std::string place(msi::RegistryRoot root, StringView key) { return std::to_string(int(root)) + "|" + lower(sv(key)); }
  void set(msi::RegistryRoot root, const std::string& key, const std::string& name, uint32_t type, const std::string& data) {
    keys.insert(place(root, view(key)));
    values[place(root, view(key)) + "|" + lower(name)] = {type, data};
  }
  const Value* get(msi::RegistryRoot root, const std::string& key, const std::string& name) const {
    const auto found = values.find(place(root, view(key)) + "|" + lower(name));
    return found == values.end() ? nullptr : &found->second;
  }
  bool has(msi::RegistryRoot root, const std::string& key, const std::string& name, uint32_t type, const std::string& data) const {
    const Value* v = get(root, key, name);
    if (v && (v->type != type || v->data != data)) printf("  registry %s\\%s is type %u, %zu bytes\n", key.c_str(), name.c_str(), v->type, v->data.size());
    return v && v->type == type && v->data == data;
  }
  bool has_key(msi::RegistryRoot root, const std::string& key) const { return keys.count(place(root, view(key))) != 0; }

  bool run(StringView program, StringView arguments, StringView working_folder, uint32_t* exit_code, String* message) override {
    (void)working_folder;
    const std::string held = read_ram(fs, sv(program));
    const size_t zero = held.rfind('\0');      // (the words at the end of the stand-in program say which one it is)
    runs.push_back(sv(program) + "|" + sv(arguments) + "|" + held.substr(zero == std::string::npos ? 0 : zero + 1));
    if (cancel_when_run) *cancel_when_run = true;
    if (!start) { message->assign("it could not be started"); return false; }
    *exit_code = 0;
    for (const auto& code : exit_codes)
      if (sv(arguments).find(code.first) != std::string::npos) *exit_code = code.second;
    return true;
  }
  bool registry_set(msi::RegistryRoot root, StringView key, StringView name, uint32_t type, ConstByteView data) override {
    set(root, sv(key), sv(name), type, std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    return true;
  }
  bool registry_get(msi::RegistryRoot root, StringView key, StringView name, uint32_t* type, ByteBuffer* data) override {
    const Value* v = get(root, sv(key), sv(name));
    if (!v) return false;
    *type = v->type;
    data->clear();
    return data->append(ConstByteView(reinterpret_cast<const uint8_t*>(v->data.data()), v->data.size()));
  }
  void registry_delete_value(msi::RegistryRoot root, StringView key, StringView name) override { values.erase(place(root, key) + "|" + lower(sv(name))); }
  void registry_delete_key(msi::RegistryRoot root, StringView key, bool only_if_empty) override {
    const std::string at = place(root, key);
    bool empty = true;
    for (const auto& v : values) empty &= v.first.compare(0, at.size() + 1, at + "|") != 0;
    for (const std::string& k : keys) empty &= k.compare(0, at.size() + 1, at + "\\") != 0;
    if (only_if_empty && !empty) return;
    for (auto v = values.begin(); v != values.end();) v = (v->first.compare(0, at.size() + 1, at + "|") == 0 || v->first.compare(0, at.size() + 1, at + "\\") == 0) ? values.erase(v) : ++v;
    for (auto k = keys.begin(); k != keys.end();) k = (*k == at || k->compare(0, at.size() + 1, at + "\\") == 0) ? keys.erase(k) : ++k;
  }
  bool write_shortcut(StringView link, StringView target, StringView working_folder) override {
    write_ram(fs, sv(link), "shortcut to " + sv(target) + " in " + sv(working_folder));
    return true;
  }
};

struct Expected { std::string key, name, component; uint64_t size; uint32_t crc; };
static std::vector<Expected> g_files;

static bool load_expected() {
  Bytes raw;
  if (!read_host_file(kData + "files.txt", &raw)) return false;
  std::string text(raw.begin(), raw.end());
  for (size_t at = 0; at < text.size();) {
    const size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end - at);
    at = end == std::string::npos ? text.size() : end + 1;
    std::vector<std::string> parts;
    for (size_t p = 0; p <= line.size();) {
      const size_t tab = line.find('\t', p);
      parts.push_back(line.substr(p, tab - p));
      p = tab == std::string::npos ? line.size() + 1 : tab + 1;
    }
    if (parts.size() == 5) g_files.push_back({parts[0], parts[1], parts[2], strtoull(parts[3].c_str(), nullptr, 10), uint32_t(strtoul(parts[4].c_str(), nullptr, 16))});
  }
  return g_files.size() == 14;
}

static bool file_is(RamFileSystem& fs, const std::string& path, const std::string& key) {
  for (const Expected& e : g_files) {
    if (e.key != key) continue;
    const std::string got = read_ram(fs, path);
    return got.size() == e.size && media::crc32(ConstByteView(reinterpret_cast<const uint8_t*>(got.data()), got.size())) == e.crc;
  }
  return false;
}

// ------------------------------------------------------------------ the package as a database
static void test_database() {
  for (const char* name : {"basic.msi", "wide.msi"}) {
    TestAllocator a;
    Bytes raw;
    CHECK(read_host_file(kData + name, &raw));
    {
      media::MemoryZipSource source(bytes_view(raw));
      msi::Database db(a, source);
      CHECK(db.open());
      CHECK(db.platform().equals("x64") && db.package_code().equals("{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}") && db.compressed());
      const msi::Table* property = db.table("Property");
      CHECK(property && property->rows() == 8 && property->columns() == 2 && property->column("Value") == 1 && property->column("Nothing") == msi::kNoColumn);
      if (property) {
        const uint32_t row = property->find("Property", "Manufacturer");
        CHECK(row != msi::kNoColumn && property->text(row, "Value").equals("Vend\xC3\xB6r \xE2\x82\xAC Ltd"));      // (code page 1252 -> UTF-8)
        CHECK(property->find("Property", "NoSuchProperty") == msi::kNoColumn && property->text(99, 0).empty() && property->integer(0, 0) == msi::kNull);
      }
      const msi::Table* file = db.table("File");
      CHECK(file && file->rows() == g_files.size());
      for (uint32_t row = 0; file && row < file->rows() && row < g_files.size(); ++row) {
        const uint32_t at = file->find("File", view(g_files[row].key));
        CHECK(at != msi::kNoColumn && file->integer(at, "FileSize") == int32_t(g_files[row].size) && file->text(at, "Component_").equals(view(g_files[row].component)) &&
              file->integer(at, "Sequence") == int32_t(row + 1) && file->text(at, "Version").empty() && file->integer(at, "FileSize") != msi::kNull);
      }
      const msi::Table* registry = db.table("Registry");
      const uint32_t r7 = registry ? registry->find("Registry", "r7") : msi::kNoColumn;
      CHECK(r7 != msi::kNoColumn && registry->integer(r7, "Root") == -1 && registry->text(r7, "Name").empty() && registry->integer(r7, "Name") == msi::kNull);
      const msi::Table* sequence = db.table("InstallExecuteSequence");
      const uint32_t fatal = sequence ? sequence->find("Action", "FatalError") : msi::kNoColumn, unplaced = sequence ? sequence->find("Action", "NewerError2") : msi::kNoColumn;
      CHECK(fatal != msi::kNoColumn && sequence->integer(fatal, "Sequence") == -3 && unplaced != msi::kNoColumn && sequence->integer(unplaced, "Sequence") == msi::kNull);
      const msi::Table* custom = db.table("CustomAction");
      const uint32_t deferred = custom ? custom->find("Action", "DoInstall") : msi::kNoColumn;
      CHECK(deferred != msi::kNoColumn && custom->integer(deferred, "Type") == 2 + 0x400 + 0x800 && custom->integer(deferred, "ExtendedType") == msi::kNull);
      CHECK(!db.table("NoSuchTable") && db.string(0).empty() && db.string(0x7FFFFFFF).empty());
      // The streams the tables refer to: a small one (in the mini stream) and the cabinet (in sectors of its own).
      ByteBuffer program(a);
      CHECK(db.read_stream("Binary.Setup", 1 << 20, &program) && program.size() > 512 && program.view()[0] == 'M' && program.view()[1] == 'Z');
      CHECK(!db.read_stream("Binary.Setup", 100, &program) && !db.read_stream("Binary.Nothing", 1 << 20, &program) && !db.read_stream("Property", 1 << 20, &program));
      media::CompoundStream cabinet(a);
      uint8_t magic[4] = {};
      CHECK(db.open_stream("product.cab", &cabinet) && cabinet.size() > 40000 && cabinet.read(0, ByteView(magic)) && memcmp(magic, "MSCF", 4) == 0);
      uint8_t tail[8] = {};
      CHECK(cabinet.read(cabinet.size() - 8, ByteView(tail)) && !cabinet.read(cabinet.size() - 7, ByteView(tail)) && !cabinet.read(~uint64_t{0}, ByteView(tail)));
    }
    CHECK(a.live == 0);
  }
  // Not packages.
  TestAllocator a;
  {
    const Bytes nothing, text(2000, 'x');
    media::MemoryZipSource none(bytes_view(nothing)), words(bytes_view(text));
    msi::Database first(a, none), second(a, words);
    CHECK(!first.open() && !second.open());
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ conditions and formatted text
struct FakeContext final : msi::IExpressionContext {
  std::map<std::string, std::string> properties;
  StringView property(StringView name) const override {
    const auto found = properties.find(sv(name));
    return found == properties.end() ? StringView() : view(found->second);
  }
  int32_t state(char kind, StringView name) const override {
    if (name.equals("Core")) return kind == '$' ? msi::kStateLocal : msi::kStateAbsent;
    if (name.equals("Main")) return kind == '&' ? msi::kStateLocal : msi::kStateAbsent;
    return msi::kStateNone;
  }
  bool file_path(StringView key, String* out) const override { return key.equals("app.exe") && out->assign("C:\\App\\app.exe"); }
  bool component_folder(StringView key, String* out) const override { return key.equals("Core") && out->assign("C:\\App\\"); }
};

static void test_expressions() {
  TestAllocator a;
  FakeContext context;
  context.properties = {{"VersionNT", "603"}, {"Name", "Vendor App"}, {"Empty", ""}, {"Zero", "0"}, {"ORDER", "x"}, {"Path", "C:\\App\\"}, {"REMOVE", "ALL"}, {"Mixed", "AbC"}, {"A.b_1", "set"}};
  static const struct { const char* text; bool value; bool reads; } kCases[] = {
      {"", true, true}, {"   ", true, true}, {"VersionNT", true, true}, {"Missing", false, true}, {"NOT Missing", true, true}, {"Empty", false, true}, {"Zero", true, true},
      {"VersionNT >= 601", true, true}, {"VersionNT < 601", false, true}, {"VersionNT = 603", true, true}, {"VersionNT <> 603", false, true}, {"603 = VersionNT", true, true},
      {"VersionNT = \"603\"", true, true}, {"VersionNT > \"7\"", false, true},      // (text against text: by letters)
      {"Name = \"Vendor App\"", true, true}, {"Name = \"vendor app\"", false, true}, {"Name ~= \"vendor app\"", true, true}, {"Name >< \"dor\"", true, true},
      {"Name << \"Vendor\"", true, true}, {"Name >> \"App\"", true, true}, {"Name >> \"Vendor\"", false, true}, {"Name ~>< \"DOR A\"", true, true}, {"Name ~<< \"VEN\"", true, true},
      {"Name = 5", false, true}, {"Name <> 5", true, true}, {"Missing = 0", false, true}, {"Missing <> 0", true, true}, {"\"abc\" = 1", false, true}, {"\"abc\" <> 1", true, true},
      {"Missing = \"\"", true, true}, {"Missing = Empty", true, true}, {"1", true, true}, {"0", false, true}, {"-1", true, true}, {"3 > 2", true, true}, {"-3 > 2", false, true},
      {"6 >< 2", true, true}, {"4 >< 2", false, true}, {"65539 >> 3", true, true}, {"65539 << 1", true, true},
      {"VersionNT AND Missing", false, true}, {"VersionNT OR Missing", true, true}, {"Missing OR NOT Missing AND VersionNT", true, true}, {"NOT VersionNT OR Missing", false, true},
      {"NOT (VersionNT AND Missing)", true, true}, {"VersionNT XOR Name", false, true}, {"VersionNT XOR Missing", true, true}, {"Missing EQV Empty", true, true},
      {"VersionNT IMP Missing", false, true}, {"Missing IMP VersionNT", true, true}, {"not missing and versionnt", false, true},      // (names are in the case they are written in; words are not)
      {"NOT Missing aNd VersionNT", true, true}, {"ORDER", true, true}, {"ORDER = \"x\" OR Missing", true, true}, {"A.b_1 = \"set\"", true, true},
      {"REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE", true, true}, {"REMOVE~=\"all\"", true, true}, {"(VersionNT>=600) AND ((Name=\"Vendor App\") OR Missing)", true, true},
      {"$Core = 3", true, true}, {"?Core = 3", false, true}, {"?Core = 2", true, true}, {"&Main = 3 AND !Main = 2", true, true}, {"$Nothing = -1", true, true}, {"%PATH", false, true},
      {"Mixed = \"abc\"", false, true}, {"Mixed ~= \"abc\"", true, true}, {"Mixed < \"b\"", true, true},
      // What cannot be read is false.
      {"VersionNT >=", false, false}, {"(VersionNT", false, false}, {"VersionNT)", false, false}, {"\"open", false, false}, {"AND", false, false}, {"VersionNT = = 3", false, false},
      {"NOT", false, false}, {"1 2", false, false}, {"Name = \"x\" extra", false, false}, {"#", false, false}, {"- > 2", false, false},
  };
  for (const auto& c : kCases) {
    bool value = !c.value;
    const bool reads = msi::evaluate_condition(c.text, context, &value);
    if (reads != c.reads || value != c.value) printf("  condition: %s -> %d (reads %d)\n", c.text, value, reads);
    CHECK(reads == c.reads && value == c.value);
  }
  // Nesting has a limit, and a long condition does not take long.
  std::string deep(500, '('), wide = "VersionNT";
  deep += "1" + std::string(500, ')');
  for (int i = 0; i < 2000; ++i) wide += " AND VersionNT";
  bool value = true;
  CHECK(!msi::evaluate_condition(view(deep), context, &value) && !value && msi::evaluate_condition(view(wide), context, &value) && value);

  static const struct { const char* text; const char* result; } kFormats[] = {
      {"plain", "plain"}, {"[Name]", "Vendor App"}, {"a[Missing]b", "ab"}, {"[Path]bin\\[#app.exe]", "C:\\App\\bin\\C:\\App\\app.exe"}, {"[!app.exe]", "C:\\App\\app.exe"},
      {"[$Core]x", "C:\\App\\x"}, {"[#nothing]", "[#nothing]"}, {"[$Nothing]", "[$Nothing]"}, {"[\\[]1[\\]]", "[1]"}, {"[~]", "[~]"}, {"one[~]two", "one[~]two"}, {"[1] of [2]", "[1] of [2]"},
      {"[%PATH]", ""}, {"[open", "[open"}, {"close]", "close]"}, {"[]", "[]"}, {"[a b]", "[a b]"}, {"[A.b_1]", "set"}, {"[[Missing]Name]", "Vendor App"}, {"[Na[Empty]me]", "Vendor App"},
      {"[VersionNT][VersionNT]", "603603"}, {"", ""},
  };
  for (const auto& f : kFormats) {
    String out(a);
    CHECK(msi::format_text(a, f.text, context, &out));
    if (!out.view().equals(f.result)) printf("  format: %s -> %s\n", f.text, sv(out.view()).c_str());
    CHECK(out.view().equals(f.result));
  }
  std::string nested;
  for (int i = 0; i < 100; ++i) nested += "[";
  nested += "Name" + std::string(100, ']');
  {
    String out(a);
    CHECK(msi::format_text(a, view(nested), context, &out));      // (does not loop or overflow, whatever it gives)
    int64_t number = 0;
    CHECK(msi::parse_integer("-12", &number) && number == -12 && msi::parse_integer("007", &number) && number == 7 && !msi::parse_integer("", &number) && !msi::parse_integer("-", &number) &&
          !msi::parse_integer("1x", &number) && !msi::parse_integer("12345678901234567890", &number));
    String path(a);
    CHECK(msi::Installer::system_path("C:\\Program Files\\App\\a.exe", &path) && path.view().equals("/Windows/Program Files/App/a.exe"));
    CHECK(msi::Installer::system_path("c:/x\\\\y\\", &path) && path.view().equals("/Windows/x/y") && msi::Installer::system_path("C:\\", &path) && path.view().equals("/Windows"));
    CHECK(msi::Installer::system_path("C:\\Users\\Public\\Desktop\\a.lnk", &path) && path.view().equals("/Desktop/a.lnk"));
    CHECK(!msi::Installer::system_path("D:\\x", &path) && !msi::Installer::system_path("C:x", &path) && !msi::Installer::system_path("\\\\server\\share\\x", &path) &&
          !msi::Installer::system_path("C:\\a\\..\\b", &path) && !msi::Installer::system_path("C:\\a\\b:c", &path) && !msi::Installer::system_path("C:\\a\\b?.txt", &path) &&
          !msi::Installer::system_path("C:\\a\\.\\b", &path) && !msi::Installer::system_path("x", &path) && !msi::Installer::system_path("", &path) &&
          !msi::Installer::system_path("C:\\a\\b. ", &path));
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ installing and removing
static const std::string kApp = "/Windows/Program Files/Vendor Name/App", kMenu = "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs/Vendor App";
static const std::string kAppWindows = "C:\\Program Files\\Vendor Name\\App\\", kKey = "SOFTWARE\\Vendor\\App";
static const std::string kUninstall = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8A69D345-D564-463C-AFF1-A69D9E530F96}";

struct Run {
  msi::Outcome outcome = msi::Outcome::Failed;
  std::string detail, code;
  std::vector<std::string> notes, shortcuts;
};

static Run run_package(TestAllocator& a, RamFileSystem& fs, FakeHost& host, const Bytes& raw, bool remove, std::vector<std::string> properties = {}, const char* upgrading = "",
                       const bool* cancel = nullptr) {
  Run result;
  media::MemoryZipSource source(bytes_view(raw));
  msi::Database db(a, source);
  if (!db.open()) {
    result.detail = "<not a package>";
    return result;
  }
  msi::Installer installer(a, fs, host, db);
  std::vector<StringView> views;
  for (const std::string& p : properties) views.push_back(view(p));
  msi::Options options;
  options.remove = remove;
  options.properties = ArrayView<const StringView>(views.data(), views.size());
  options.package_path = "C:\\Windows\\Temp\\WinGet\\product.msi";
  options.upgrading_product = upgrading;
  options.cancel = cancel;
  String detail(a);
  result.outcome = installer.run(options, &detail);
  result.detail = sv(detail.view());
  result.code = sv(installer.property("ProductCode"));
  for (const String& note : installer.notes()) result.notes.push_back(sv(note.view()));
  for (const String& link : installer.shortcuts()) result.shortcuts.push_back(sv(link.view()));
  return result;
}

static bool noted(const Run& run, const char* piece) {
  for (const std::string& note : run.notes)
    if (note.find(piece) != std::string::npos) return true;
  return false;
}

static void test_install(const char* package) {
  TestAllocator a;
  Bytes raw;
  CHECK(read_host_file(kData + package, &raw));
  {
    RamFileSystem fs(a);
    FakeHost host(fs);
    const msi::RegistryRoot machine = msi::RegistryRoot::LocalMachine;
    // What is on the computer before: values a registry search finds, things the package says to clear away when it installs.
    host.set(machine, "SOFTWARE\\Other\\Thing", "Dir", msi::kRegString, utf16("D:\\found"));
    host.set(machine, "SOFTWARE\\Other\\Thing", "Number", msi::kRegDword, std::string("\x07\0\0\0", 4));
    host.set(machine, "SOFTWARE\\Vendor\\Old", "Stale", msi::kRegString, utf16("x"));
    host.set(machine, "SOFTWARE\\Vendor\\Old", "Kept", msi::kRegString, utf16("x"));
    host.set(machine, "SOFTWARE\\Vendor\\OldKey\\Sub", "Any", msi::kRegString, utf16("x"));
    write_ram(fs, kApp + "/stale.dat", "old");
    write_ram(fs, kApp + "/keep.dat", "kept while installed");

    const Run installed = run_package(a, fs, host, raw, false);
    if (installed.outcome != msi::Outcome::Ok) printf("  install: %s\n", installed.detail.c_str());
    CHECK(installed.outcome == msi::Outcome::Ok && installed.detail.empty() && installed.code == "{8A69D345-D564-463C-AFF1-A69D9E530F96}");

    // The files of the chosen features' components, each where its folder is - and no others.
    CHECK(file_is(fs, kApp + "/Vendor App.exe", "app.exe") && file_is(fs, kApp + "/readme.txt", "readme.txt") && file_is(fs, kApp + "/helper.exe", "helper.exe"));
    CHECK(file_is(fs, kApp + "/data/big.bin", "big.bin") && file_is(fs, kApp + "/sub.txt", "sub.txt") && file_is(fs, kApp + "/cond.txt", "cond.txt"));
    CHECK(file_is(fs, "/Windows/ProgramData/Vendor Data/settings.ini", "settings.ini"));      // (the folder a custom action placed)
    // Files the cabinet holds once under two keys: a copy of one that is installed, one more in another folder, a copy of one that is not installed.
    CHECK(file_is(fs, kApp + "/readme copy.txt", "readme2.txt") && file_is(fs, kApp + "/data/third.txt", "readme3.txt") && file_is(fs, kApp + "/extra copy.txt", "extra2.txt") &&
          !exists(fs, kApp + "/never copy.txt"));
    CHECK(!exists(fs, kApp + "/extra.txt") && !exists(fs, kApp + "/never.txt") && !exists(fs, kApp + "/off.txt") && !exists(fs, "/Windows/datadir"));
    CHECK(exists(fs, kApp + "/empty") && !exists(fs, kApp + "/stale.dat") && exists(fs, kApp + "/keep.dat"));

    // The registry.
    CHECK(host.has(machine, kKey, "InstallDir", msi::kRegString, utf16(kAppWindows)) && host.has(machine, kKey, "Count", msi::kRegDword, std::string("\x2A\0\0\0", 4)));
    CHECK(host.has(machine, kKey, "Blob", msi::kRegBinary, std::string("\x0A\xFF\x10", 3)) && host.has(machine, kKey, "Path", msi::kRegExpandString, utf16("C:\\Windows\\bin;%PATH%")));
    const std::string list = utf16("one") + utf16("two") + utf16("three") + std::string(2, '\0');
    CHECK(host.has(machine, kKey, "List", msi::kRegMultiString, list) && host.has(machine, kKey, "Hash", msi::kRegString, utf16("#literal")) && !host.get(machine, kKey, "Extra"));
    CHECK(host.has_key(machine, kKey + "\\Empty") && host.has(machine, "SOFTWARE\\Vendor\\PerMachine", "", msi::kRegString, utf16(kAppWindows + "Vendor App.exe")));
    CHECK(host.has(msi::RegistryRoot::ClassesRoot, ".vapp", "", msi::kRegString, utf16("VendorApp.File")) && host.has(machine, kKey, "Found", msi::kRegString, utf16("D:\\found and #7")));
    CHECK(host.has(msi::RegistryRoot::CurrentUser, "Software\\Vendor", "User", msi::kRegString, utf16("Default")));
    CHECK(host.has(machine, kKey + "\\1.2.3", "Folder", msi::kRegString, utf16(kAppWindows + "data\\[x]")));
    CHECK(host.has(machine, kKey, "Tag", msi::kRegString, utf16("dir=" + kAppWindows + "&name=Vendor App")));
    CHECK(!host.get(machine, "SOFTWARE\\Vendor\\Old", "Stale") && host.get(machine, "SOFTWARE\\Vendor\\Old", "Kept") && !host.has_key(machine, "SOFTWARE\\Vendor\\OldKey") &&
          !host.get(machine, "SOFTWARE\\Vendor\\OldKey\\Sub", "Any"));
    // The list of installed programs.
    CHECK(host.has(machine, kUninstall, "DisplayName", msi::kRegString, utf16("Vendor App")) && host.has(machine, kUninstall, "DisplayVersion", msi::kRegString, utf16("1.2.3")) &&
          host.has(machine, kUninstall, "UninstallString", msi::kRegString, utf16("MsiExec.exe /X{8A69D345-D564-463C-AFF1-A69D9E530F96}")) &&
          host.has(machine, kUninstall, "WindowsInstaller", msi::kRegDword, std::string("\x01\0\0\0", 4)) && host.has(machine, kUninstall, "InstallLocation", msi::kRegString, utf16("C:\\Somewhere")) &&
          host.get(machine, kUninstall, "Publisher"));

    // The shortcuts: one to the key file of its component (its target is a feature), one to a file by its path.
    CHECK(read_ram(fs, kMenu + "/Vendor App.lnk") == "shortcut to " + kAppWindows + "Vendor App.exe in C:\\Program Files\\Vendor Name\\App");
    CHECK(read_ram(fs, kMenu + "/Readme.lnk") == "shortcut to " + kAppWindows + "readme.txt in C:\\Program Files\\Vendor Name\\App" && !exists(fs, kMenu + "/Extra.lnk"));
    CHECK(installed.shortcuts.size() == 2 && installed.shortcuts[0] == kMenu + "/Vendor App.lnk");

    // The programs the package ran: the one it keeps (unpacked, run with its formatted arguments, deleted again) and an installed file a property names.
    CHECK(host.runs.size() == 2);
    if (host.runs.size() == 2) {
      CHECK(host.runs[0] == "/Windows/Windows/Installer/MSIDoInstall.exe|/install \"dir=" + kAppWindows + "&name=Vendor App\" /quiet|the program of a custom action\n");
      CHECK(host.runs[1] == kApp + "/helper.exe|/by-property|helper\n");
    }
    CHECK(!exists(fs, "/Windows/Windows/Installer/MSIDoInstall.exe"));
    // What it could not do, said.
    CHECK(noted(installed, "services") && noted(installed, "library (DLL) was skipped: DllStep") && noted(installed, "script was skipped: ScriptStep") &&
          noted(installed, "does not know was skipped: NotAnAction") && installed.notes.size() == 4);

    // Removing it: the program the package runs when it goes (an installed file, still there at that point), then everything it put in place.
    write_ram(fs, kApp + "/run.log", "written by the application");
    host.runs.clear();
    const Run removed = run_package(a, fs, host, raw, true);
    if (removed.outcome != msi::Outcome::Ok) printf("  remove: %s\n", removed.detail.c_str());
    CHECK(removed.outcome == msi::Outcome::Ok && removed.notes.empty());
    CHECK(host.runs.size() == 1 && host.runs[0] == kApp + "/helper.exe|/stop \"" + kAppWindows + "readme.txt\"|helper\n");
    CHECK(!exists(fs, "/Windows/Program Files/Vendor Name") && exists(fs, "/Windows/Program Files") && !exists(fs, kMenu) && exists(fs, "/Windows/ProgramData/Microsoft/Windows/Start Menu/Programs"));
    CHECK(!exists(fs, "/Windows/ProgramData/Vendor Data") && exists(fs, "/Windows/ProgramData"));
    CHECK(!host.get(machine, kKey, "InstallDir") && !host.get(machine, kKey, "List") && !host.get(machine, kKey, "Tag") && !host.has_key(machine, kKey + "\\1.2.3") &&
          host.has_key(machine, kKey + "\\Empty") && !host.has_key(machine, kUninstall) && !host.get(msi::RegistryRoot::ClassesRoot, ".vapp", "") &&
          !host.has_key(machine, "SOFTWARE\\Vendor\\PerMachine") && !host.get(msi::RegistryRoot::CurrentUser, "Software\\Vendor", "User"));
    CHECK(host.get(machine, "SOFTWARE\\Other\\Thing", "Dir") && host.get(machine, "SOFTWARE\\Vendor\\Old", "Kept"));      // (not the package's)

    // Removed to make room for another version: the package's own "when removed" program is not run.
    CHECK(run_package(a, fs, host, raw, false).outcome == msi::Outcome::Ok);
    host.runs.clear();
    CHECK(run_package(a, fs, host, raw, true, {}, "{1B2C3D4E-0000-4111-8222-ABCDEFABCDEF}").outcome == msi::Outcome::Ok && host.runs.empty() && !exists(fs, kApp));
  }
  CHECK(a.live == 0);
}

static void test_refusals() {
  TestAllocator a;
  Bytes basic, refused, arm, outside;
  CHECK(read_host_file(kData + "basic.msi", &basic) && read_host_file(kData + "refused.msi", &refused) && read_host_file(kData + "arm64.msi", &arm) &&
        read_host_file(kData + "outside.msi", &outside));
  {
    RamFileSystem fs(a);
    FakeHost host(fs);
    auto untouched = [&] { return !exists(fs, "/Windows/Program Files") && host.values.empty() && host.runs.empty(); };
    // The package says no: a launch condition, with its message formatted; an action that stops with a message of the Error table.
    Run r = run_package(a, fs, host, refused, false);
    CHECK(r.outcome == msi::Outcome::Refused && r.detail == "the package says: Vendor App needs a newer Windows." && untouched());
    r = run_package(a, fs, host, basic, false, {"NEWERFOUND=1"});
    CHECK(r.outcome == msi::Outcome::Refused && r.detail == "the package says: A newer version of Vendor App is installed." && untouched());
    // Packages this engine cannot install.
    r = run_package(a, fs, host, arm, false);
    CHECK(r.outcome == msi::Outcome::NotSupported && r.detail.find("another kind of processor: Arm64") != std::string::npos && untouched());
    r = run_package(a, fs, host, outside, false);
    CHECK(r.outcome == msi::Outcome::NotSupported && r.detail.find("beside the package") != std::string::npos && host.runs.empty() && !exists(fs, kApp + "/readme.txt"));
    // Cancelled before it starts, and by the time a program has run.
    bool cancel = true;
    CHECK(run_package(a, fs, host, basic, false, {}, "", &cancel).outcome == msi::Outcome::Cancelled && host.runs.empty());
    cancel = false;
    host.cancel_when_run = &cancel;
    CHECK(run_package(a, fs, host, basic, false, {}, "", &cancel).outcome == msi::Outcome::Cancelled && host.runs.size() == 1);
    host.cancel_when_run = nullptr;
    cancel = false;
  }
  {
    // A program of the package that fails stops the installation, unless the package says its result does not matter.
    RamFileSystem fs(a);
    FakeHost host(fs);
    host.exit_codes["/install"] = 1603;
    Run r = run_package(a, fs, host, basic, false);
    CHECK(r.outcome == msi::Outcome::Failed && r.detail == "the program of its step DoInstall ended with code 1603" && host.runs.size() == 1);
    CHECK(!exists(fs, "/Windows/Windows/Installer/MSIDoInstall.exe") && !host.has_key(msi::RegistryRoot::LocalMachine, kUninstall));
    host.exit_codes.clear();
    host.exit_codes["/optional"] = 5;
    host.exit_codes["/by-property"] = 9;
    host.runs.clear();
    r = run_package(a, fs, host, basic, false, {"RUNOPTIONAL=\"YES\""});
    CHECK(r.outcome == msi::Outcome::Ok && host.runs.size() == 3 && host.runs[1].find("|/optional|") != std::string::npos);
    host.start = false;
    host.runs.clear();
    r = run_package(a, fs, host, basic, false);
    CHECK(r.outcome == msi::Outcome::Failed && r.detail == "the program of its step DoInstall could not be run: it could not be started");
  }
  {
    // The command line chooses the features and places a folder; a folder outside drive C, or through "..", stops it.
    RamFileSystem fs(a);
    FakeHost host(fs);
    Run r = run_package(a, fs, host, basic, false, {"ADDLOCAL=Extra,Child", "INSTALLDIR=C:\\Elsewhere\\App"});
    CHECK(r.outcome == msi::Outcome::Ok && file_is(fs, "/Windows/Elsewhere/App/extra.txt", "extra.txt") && file_is(fs, "/Windows/Elsewhere/App/off.txt", "off.txt") &&
          !exists(fs, "/Windows/Elsewhere/App/readme.txt") && !exists(fs, kApp) && exists(fs, kMenu + "/Extra.lnk") && !exists(fs, kMenu + "/Readme.lnk"));
    CHECK(host.has(msi::RegistryRoot::LocalMachine, kKey, "Extra", msi::kRegString, utf16("no")) && !host.get(msi::RegistryRoot::LocalMachine, kKey, "Count"));
    r = run_package(a, fs, host, basic, false, {"INSTALLDIR=D:\\App"});
    CHECK(r.outcome == msi::Outcome::Failed && r.detail.find("folder this system cannot make") != std::string::npos);
    r = run_package(a, fs, host, basic, false, {"INSTALLDIR=C:\\App\\..\\..\\x"});
    CHECK(r.outcome == msi::Outcome::Failed);
    r = run_package(a, fs, host, basic, false, {"ADDLOCAL=ALL"});
    CHECK(r.outcome == msi::Outcome::Ok && exists(fs, kApp + "/extra.txt") && exists(fs, kApp + "/off.txt") && exists(fs, kApp + "/readme.txt") && !exists(fs, kApp + "/never.txt"));
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ damaged packages
static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t next_random() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 7;
  g_seed ^= g_seed << 17;
  return g_seed;
}

static void test_damage() {
  uint32_t opened = 0, installed = 0, tried = 0;
  for (const char* name : {"basic.msi", "wide.msi"}) {
    Bytes whole;
    CHECK(read_host_file(kData + name, &whole));
    auto attempt = [&](const Bytes& raw) {
      TestAllocator a;
      {
        RamFileSystem fs(a);
        FakeHost host(fs);
        const Run r = run_package(a, fs, host, raw, false);
        ++tried;
        if (r.detail != "<not a package>") ++opened;
        if (r.outcome == msi::Outcome::Ok) {
          ++installed;
          (void)run_package(a, fs, host, raw, true);
        }
      }
      CHECK(a.live == 0);
    };
    // Cut short: every length near the start, then in steps.
    for (size_t length = 0; length < whole.size(); length += length < 1200 ? 7 : 211) attempt(Bytes(whole.begin(), whole.begin() + long(length)));
    // Single bits flipped: a third of them in the header and tables at the two ends, where they do most.
    for (int i = 0; i < 700; ++i) {
      Bytes raw = whole;
      const size_t at = i % 3 == 0 ? next_random() % 600 : i % 3 == 1 ? whole.size() - 1 - next_random() % 9000 : next_random() % whole.size();
      raw[at] ^= uint8_t(1u << (next_random() % 8));
      attempt(raw);
    }
    // Whole runs of bytes overwritten.
    for (int i = 0; i < 150; ++i) {
      Bytes raw = whole;
      const size_t at = next_random() % whole.size(), run = 1 + next_random() % 64;
      for (size_t k = at; k < whole.size() && k < at + run; ++k) raw[k] = uint8_t(next_random());
      attempt(raw);
    }
  }
  printf("  damaged packages: %u tried, %u opened, %u installed all the same\n", tried, opened, installed);
  CHECK(tried > 1500 && opened > 100);
}

// ------------------------------------------------------------------ looking at a real package
static int list_package(const char* path) {
  TestAllocator a;
  Bytes raw;
  if (!read_host_file(path, &raw)) { printf("cannot read %s\n", path); return 1; }
  media::MemoryZipSource source(bytes_view(raw));
  msi::Database db(a, source);
  if (!db.open()) { printf("%s is not a Windows Installer package this reader can read\n", path); return 1; }
  printf("platform: %s  package code: %s  compressed: %d\n", sv(db.platform()).c_str(), sv(db.package_code()).c_str(), db.compressed());
  static const char* kCounted[] = {"Directory", "Feature", "Component", "File", "Media", "Registry", "Shortcut", "Binary", "ServiceInstall", "Environment", "Class", "ProgId",
                                   "Extension", "TypeLib", "SelfReg", "Font", "IniFile", "RemoveFile", "DuplicateFile", "MsiAssembly", "AppSearch", "RegLocator", "LaunchCondition", "Upgrade"};
  for (const char* name : kCounted)
    if (const msi::Table* t = db.table(name)) printf("%s: %u rows\n", name, t->rows());
  for (const char* name : {"Property", "Media", "LaunchCondition", "CustomAction", "InstallExecuteSequence"}) {
    const msi::Table* t = db.table(name);
    if (!t) continue;
    printf("--- %s\n", name);
    for (uint32_t row = 0; row < t->rows(); ++row) {
      for (uint32_t c = 0; c < t->columns(); ++c) {
        const int32_t number = t->integer(row, c);
        if (number != msi::kNull) printf("%d\t", number);
        else printf("%s\t", sv(t->text(row, c)).c_str());
      }
      printf("\n");
    }
  }
  return 0;
}

static void print_tree(RamFileSystem& fs, const std::string& folder, uint32_t* files, uint64_t* bytes, uint32_t depth = 0) {
  UniquePointer<IDirectory> listing;
  if (depth > 32 || fs.open_directory(view(folder), &listing) != Status::Ok) return;
  DirectoryEntry entry;
  while (listing->next(&entry) == Status::Ok) {
    const std::string path = folder + "/" + sv(entry.name.view());
    if (entry.type == NodeType::Directory) { print_tree(fs, path, files, bytes, depth + 1); continue; }
    if (++*files <= 25) printf("  file: %s (%llu bytes)\n", path.c_str(), (unsigned long long)entry.size);
    *bytes += entry.size;
  }
}

static int install_package(const char* path) {
  TestAllocator a;
  Bytes raw;
  if (!read_host_file(path, &raw)) { printf("cannot read %s\n", path); return 1; }
  int failed = 0;
  {
    RamFileSystem fs(a);
    FakeHost host(fs);
    const Run installed = run_package(a, fs, host, raw, false);
    printf("install: %s%s\n", installed.outcome == msi::Outcome::Ok ? "ok" : "FAILED: ", installed.detail.c_str());
    failed += installed.outcome != msi::Outcome::Ok;
    uint32_t files = 0;
    uint64_t bytes = 0;
    print_tree(fs, "/Windows", &files, &bytes);
    printf("  %u files, %llu bytes; %zu registry values; %zu shortcuts\n", files, (unsigned long long)bytes, host.values.size(), installed.shortcuts.size());
    for (const std::string& line : host.runs) printf("  ran: %s\n", line.substr(0, line.rfind('|')).c_str());
    for (const std::string& note : installed.notes) printf("  note: %s\n", note.c_str());
    host.runs.clear();
    const Run removed = run_package(a, fs, host, raw, true);
    printf("remove: %s%s\n", removed.outcome == msi::Outcome::Ok ? "ok" : "FAILED: ", removed.detail.c_str());
    failed += removed.outcome != msi::Outcome::Ok;
    files = 0, bytes = 0;
    print_tree(fs, "/Windows", &files, &bytes);
    printf("  left: %u files, %zu registry values\n", files, host.values.size());
    for (const std::string& line : host.runs) printf("  ran: %s\n", line.substr(0, line.rfind('|')).c_str());
    for (const std::string& note : removed.notes) printf("  note: %s\n", note.c_str());
  }
  if (a.live != 0) { printf("leak\n"); ++failed; }
  return failed ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--install") == 0) return install_package(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--list") == 0) return list_package(argv[2]);
  if (!load_expected()) {
    printf("FAILED: tests/data/msi (files.txt and the packages) cannot be read; run from the repository's root\n");
    return 1;
  }
  test_database();
  test_expressions();
  test_install("basic.msi");
  test_install("wide.msi");
  test_refusals();
  test_damage();
  if (g_failed) {
    printf("MsiTest: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("MsiTest: all %d checks passed\n", g_checks);
  return 0;
}

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

#include "Desktop/AppMan/WingetInstaller.hpp"
#include "Crypto/Crypto.hpp"
#include "Desktop/AppMan/InstallFiles.hpp"
#include "Desktop/AppMan/Yaml.hpp"
#include "Desktop/InstalledApps.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Media/Zip.hpp"
#include "Media/Zlib.hpp"
#include "Msi/MsiDatabase.hpp"
#include "Msi/MsiInstaller.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

using filesystem::Status;

constexpr uint64_t kMaxRecordBytes = 1u << 20;
constexpr uint32_t kMszipBlock = 32768;
constexpr uint64_t kMaxShortcuts = 256;

bool equals_folded(StringView a, StringView b) { return compare_folded(a, b) == 0; }

bool append_hex(String* out, ConstByteView bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const uint8_t byte : bytes)
    if (!out->push_back(kHex[byte >> 4]) || !out->push_back(kHex[byte & 15])) return false;
  return true;
}

// `text` as part of a web address: everything but letters, digits and - . _ ~ (and '/', if it is kept) as %XX.
bool append_escaped(String* out, StringView text, bool keep_slash) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || (keep_slash && c == '/');
    const uint8_t byte = static_cast<uint8_t>(c);
    if (plain ? !out->push_back(c) : (!out->push_back('%') || !out->push_back(kHex[byte >> 4]) || !out->push_back(kHex[byte & 15]))) return false;
  }
  return true;
}

// Is the SHA-256 of `data` the one `hex` spells (any case)?
bool sha256_is(ConstByteView data, StringView hex, allocators::IAllocator& allocator) {
  uint8_t hash[32];
  crypto::hash(crypto::HashKind::Sha256, data, hash);
  String text(allocator);
  return append_hex(&text, ConstByteView(hash)) && equals_folded(text.view(), hex.trimmed());
}

bool web_address(StringView url) {
  if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
  for (const char c : url)
    if (static_cast<uint8_t>(c) <= ' ' || c == '"' || c == '\'' || c == 0x7F) return false;
  return true;
}

// Whether a download whose SHA-256 is `hex` may be installed as `entry`: it is the file the manifest names, or the user chose to install it anyway and it
// came over HTTPS from the manifest's own link (a publisher that serves its newest file at a fixed link, as Google does for Chrome, changes the file
// before winget's catalog catches up; winget itself has --ignore-security-hash for that).
bool checksum_accepted(StringView hex, const WingetInstallerEntry& entry, bool ignore_checksum) {
  if (equals_folded(hex, entry.sha256.view())) return true;
  return ignore_checksum && WingetInstaller::may_ignore_checksum(entry.url.view());
}

// A name for a file or a shortcut out of `text`: what Windows does not allow in a name becomes '_'. "" if nothing is left.
bool safe_name(StringView text, String* out) {
  out->clear();
  text = text.trimmed();
  if (text.size() > 100) text = text.first(100);
  for (const char c : text) {
    const bool bad = static_cast<uint8_t>(c) < 0x20 || c == 0x7F || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
    if (!out->push_back(bad ? '_' : c)) return false;
  }
  while (!out->empty() && ((*out)[out->size() - 1] == '.' || (*out)[out->size() - 1] == ' ')) out->truncate(out->size() - 1);
  if (out->view().starts_with('.')) out->clear();
  return true;
}

StringView parent_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? StringView() : path.first(slash);
}
StringView leaf_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? path : path.substr(slash + 1);
}
// "tool.exe" -> "tool"
StringView stem_of(StringView name) {
  const uint64_t dot = name.rfind('.');
  return dot == StringView::npos || dot == 0 ? name : name.first(dot);
}

// "/Windows/Program Files/x/y.exe" -> "C:\Program Files\x\y.exe"
bool windows_path(StringView system_path, String* out) {
  const StringView root(filesystem::layout::kWindows);
  if (!system_path.starts_with(root) || !out->assign("C:")) return false;
  for (const char c : system_path.substr(root.size()))
    if (!out->push_back(c == '/' ? '\\' : c)) return false;
  return true;
}

bool write_bytes(filesystem::IFileSystem& files, StringView path, ConstByteView bytes) {
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != Status::Ok) return false;
  uint64_t at = 0;
  while (at < bytes.size()) {
    uint64_t done = 0;
    if (file->write(at, bytes.subview(at), &done) != Status::Ok || done == 0) return false;
    at += done;
  }
  return file->flush() == Status::Ok;
}

bool exists(filesystem::IFileSystem& files, StringView path) {
  filesystem::DirectoryEntry entry;
  return files.stat(path, &entry) == Status::Ok;
}

// The archive on the disk, read piece by piece.
class FileZipSource final : public media::IZipSource {
 public:
  explicit FileZipSource(filesystem::IFile& file) : file_(file) {}
  uint64_t size() const override { return file_.size(); }
  [[nodiscard]] bool read(uint64_t offset, ByteView out) override {
    uint64_t at = 0;
    while (at < out.size()) {
      uint64_t got = 0;
      if (file_.read(offset + at, out.subview(at), &got) != Status::Ok || got == 0) return false;
      at += got;
    }
    return true;
  }

 private:
  filesystem::IFile& file_;
};

// What the manifest calls the kinds of installer, for the message that says one cannot be installed.
StringView kind_name(StringView type) {
  if (equals_folded(type, "inno")) return "an Inno Setup program";
  if (equals_folded(type, "nullsoft")) return "an NSIS setup program";
  if (equals_folded(type, "msi") || equals_folded(type, "wix")) return "a Windows Installer package (.msi)";
  if (equals_folded(type, "msix") || equals_folded(type, "appx")) return "an MSIX package";
  if (equals_folded(type, "burn")) return "a WiX Burn setup program";
  if (equals_folded(type, "exe")) return "a setup program";
  if (equals_folded(type, "msstore")) return "a Microsoft Store application";
  if (equals_folded(type, "font")) return "a font";
  if (equals_folded(type, "zip")) return "an archive with a setup program in it";
  return "a setup program";
}

// Put in place by this package manager itself: one program, or an archive of programs.
bool is_native(const WingetInstallerEntry& entry) {
  if (!web_address(entry.url.view()) || entry.sha256.size() != 64) return false;
  if (equals_folded(entry.type.view(), "portable")) return true;
  return equals_folded(entry.type.view(), "zip") && equals_folded(entry.nested_type.view(), "portable") && entry.nested.count() > 0;
}

// The switches that make a setup program of this kind ask nothing ("" if the kind has none of its own), and its uninstaller.
StringView silent_switches(StringView type) {
  if (equals_folded(type, "inno")) return "/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
  if (equals_folded(type, "nullsoft")) return "/S";
  return "";
}
StringView silent_uninstall_switches(StringView type) {
  if (equals_folded(type, "inno")) return "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
  if (equals_folded(type, "nullsoft")) return "/S";
  return "";
}

// A setup program this package manager can run without a person: Inno Setup, NSIS, or any other that the manifest has a silent switch for.
bool is_setup(const WingetInstallerEntry& entry) {
  if (!web_address(entry.url.view()) || entry.sha256.size() != 64) return false;
  if (equals_folded(entry.type.view(), "inno") || equals_folded(entry.type.view(), "nullsoft")) return true;
  return equals_folded(entry.type.view(), "exe") && !entry.silent.empty();
}

// A Windows Installer package: not a program to run but a description this system installs by (Msi/MsiInstaller.hpp).
bool is_msi(const WingetInstallerEntry& entry) {
  if (!web_address(entry.url.view()) || entry.sha256.size() != 64) return false;
  return equals_folded(entry.type.view(), "msi") || equals_folded(entry.type.view(), "wix");
}

// "{8A69D345-D564-463C-AFF1-A69D9E530F96}": a product code, which names the kept package's file.
bool product_code(StringView text) {
  if (text.size() != 38 || text[0] != '{' || text[37] != '}') return false;
  for (const char c : text.substr(1, 36))
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || c == '-')) return false;
  return true;
}

// The last part of a registry key's name.
StringView key_leaf(StringView key) {
  const uint64_t slash = key.rfind('\\');
  return slash == StringView::npos ? key : key.substr(slash + 1);
}

// 0 for the architecture this computer runs best, higher for worse, -1 for one it cannot run. 32-bit x86 programs come last: the Windows layer runs
// them only in part so far (docs/Wow64.md), so an x86 installer is taken only when there is nothing else, and only of a kind known to run
// (x86_setup_runs).
int architecture_rank(StringView architecture) {
  if (equals_folded(architecture, "x64")) return 0;
  if (equals_folded(architecture, "neutral")) return 1;
  if (equals_folded(architecture, "x86")) return 2;
  return -1;
}

// A 32-bit setup program of a kind the 32-bit layer is known to run (docs/Wow64.md): the only x86 installers taken, and only when no x64 or
// neutral one can be. What it installs is often 32-bit too, which runs as far as that layer goes.
bool x86_setup_runs(const WingetInstallerEntry& entry) {
  return equals_folded(entry.type.view(), "inno") || equals_folded(entry.type.view(), "nullsoft");
}

}  // namespace

// What is remembered of a package a setup program installed.
struct WingetInstaller::SetupRecord {
  explicit SetupRecord(allocators::IAllocator& allocator) : type(allocator), uninstall(allocator), package(allocator), product(allocator) {}
  String type;            // InstallerType
  String uninstall;       // the uninstall command line ("" if the setup program registered none)
  bool quiet = false;     // it is the registry's QuietUninstallString: no switches are added
  // Of a Windows Installer package:
  String package;         // the kept package (a file in layout::kWindowsInstallerCache), which removes the product ("" if it could not be kept)
  String product;         // its product code
};

namespace {

// The Windows world as the Windows Installer engine needs it, out of what the App Manager has: the setup host (programs and the registry) and the
// file system (shortcuts).
class MsiHost final : public msi::IHost {
 public:
  MsiHost(allocators::IAllocator& allocator, filesystem::IFileSystem& files, ISetupHost& setup, const bool* cancel) : allocator_(allocator), files_(files), setup_(setup), cancel_(cancel) {}

  [[nodiscard]] bool run(StringView program, StringView arguments, StringView working_folder, uint32_t* exit_code, String* message) override {
    (void)working_folder;      // (a program is started in its own folder)
    List<String> words(allocator_);
    List<StringView> views(allocator_);
    bool ok = WingetInstaller::split_command_line(allocator_, arguments, nullptr, &words);
    for (const String& word : static_cast<const List<String>&>(words).view()) ok = ok && views.push_back(word.view());
    if (!ok) {
      message->assign("there is not enough memory");
      return false;
    }
    return setup_.run(program, static_cast<const List<StringView>&>(views).view(), cancel_, exit_code, message);
  }
  [[nodiscard]] bool registry_set(msi::RegistryRoot root, StringView key, StringView name, uint32_t type, ConstByteView data) override { return setup_.registry_set(root, key, name, type, data); }
  [[nodiscard]] bool registry_get(msi::RegistryRoot root, StringView key, StringView name, uint32_t* type, ByteBuffer* data) override { return setup_.registry_get(root, key, name, type, data); }
  void registry_delete_value(msi::RegistryRoot root, StringView key, StringView name) override { setup_.registry_delete_value(root, key, name); }
  void registry_delete_key(msi::RegistryRoot root, StringView key, bool only_if_empty) override { setup_.registry_delete_key(root, key, only_if_empty); }
  [[nodiscard]] bool write_shortcut(StringView link, StringView target, StringView working_folder) override {
    ByteBuffer bytes(allocator_);
    return write_shell_link(target, working_folder, &bytes) && write_bytes(files_, link, bytes.view());
  }

 private:
  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  ISetupHost& setup_;
  const bool* cancel_;
};

Result result_of(msi::Outcome outcome) {
  switch (outcome) {
    case msi::Outcome::Ok: return Result::Ok;
    case msi::Outcome::NotSupported: return Result::NotSupported;
    case msi::Outcome::Refused:
    case msi::Outcome::Failed: return Result::SetupFailed;
    case msi::Outcome::FileError: return Result::FileError;
    case msi::Outcome::Cancelled: return Result::Cancelled;
    case msi::Outcome::OutOfMemory: break;
  }
  return Result::OutOfMemory;
}

}  // namespace

struct WingetInstaller::Program {
  explicit Program(allocators::IAllocator& allocator) : path(allocator), label(allocator) {}
  String path;       // in the package's folder
  String label;      // of its shortcut
};

bool WingetInstaller::safe_member_path(StringView raw, String* out) {
  out->clear();
  if (raw.empty() || raw.size() > 1024) return false;
  StringView part;
  uint64_t start = 0;
  for (uint64_t i = 0; i <= raw.size(); ++i) {
    if (i < raw.size() && raw[i] != '/' && raw[i] != '\\') continue;
    part = raw.substr(start, i - start);
    start = i + 1;
    if (part.empty()) {
      if (i == 0) return false;                  // an absolute path
      if (i == raw.size()) break;                // a folder's own entry ("a/b/")
      return false;                              // "a//b"
    }
    if (part.equals(".") || part.equals("..") || part.size() > 255 || part.ends_with('.') || part.ends_with(' ')) return false;
    for (const char c : part)
      if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    if (!out->empty() && !out->push_back('/')) return false;
    if (!out->append(part)) return false;
  }
  return !out->empty();
}

bool WingetInstaller::decode_mszyml(allocators::IAllocator& allocator, ConstByteView packed, uint64_t max_bytes, String* text) {
  text->clear();
  ByteReader in(packed);
  // The header of Windows' compression interface: a magic number, the header's size, the algorithm (2: MSZIP), the size of the text.
  if (in.le32() != 0xC0E5510Au) return false;
  const uint16_t header_size = in.le16();
  in.skip(1);      // a check byte of the header
  const uint8_t algorithm = in.u8();
  const uint64_t size = in.le64();
  if (!in.ok() || header_size < 24 || algorithm != 2 || size > max_bytes || !in.seek(header_size)) return false;
  ByteBuffer out(allocator);
  while (out.size() < size) {
    const uint32_t block_size = in.le32();
    ByteReader block(in.take(block_size));
    if (!in.ok() || block.u8() != 'C' || block.u8() != 'K' || !block.ok()) return false;
    const uint64_t left = size - out.size(), before = out.size();
    if (!media::inflate_with_history(block.rest(), &out, left < kMszipBlock ? left : kMszipBlock, before < kMszipBlock ? before : kMszipBlock)) return false;
    if (out.size() == before) return false;      // (an empty block would never end)
  }
  return text->assign(StringView(out.view()));
}

bool WingetInstaller::find_version(allocators::IAllocator& allocator, StringView version_list, StringView version, String* found_version, String* path, String* sha256) {
  YamlNode root(allocator);
  if (!parse_yaml(allocator, version_list, &root)) return false;
  const YamlNode* versions = root.find("vD");
  if (!versions || versions->kind != YamlNode::Kind::Sequence || versions->children.count() == 0) return false;
  const YamlNode* chosen = versions->children.view()[0].get();
  for (const UniquePointer<YamlNode>& item : versions->children.view())
    if (item->text("v").equals(version)) {
      chosen = item.get();
      break;
    }
  const StringView where = chosen->text("rP");
  // The path is put into an address: it must be a plain path below the cache.
  String checked(allocator);
  if (where.empty() || !safe_member_path(where, &checked) || !checked.view().equals(where)) return false;
  return found_version->assign(chosen->text("v")) && path->assign(where) && sha256->assign(chosen->text("s256H")) && sha256->size() == 64;
}

bool WingetInstaller::parse_manifest(allocators::IAllocator& allocator, StringView yaml, WingetManifest* manifest) {
  manifest->installers.clear();
  YamlNode root(allocator);
  if (!parse_yaml(allocator, yaml, &root) || root.kind != YamlNode::Kind::Map) return false;
  if (!manifest->id.assign(root.text("PackageIdentifier")) || !manifest->name.assign(root.text("PackageName")) || !manifest->version.assign(root.text("PackageVersion")))
    return false;
  manifest->command.clear();
  const YamlNode* commands = root.find("Commands");
  if (commands && commands->kind == YamlNode::Kind::Sequence && commands->children.count() > 0 && commands->children.view()[0]->kind == YamlNode::Kind::Scalar &&
      !manifest->command.assign(commands->children.view()[0]->value.view()))
    return false;
  const YamlNode* installers = root.find("Installers");
  if (manifest->id.empty() || !installers || installers->kind != YamlNode::Kind::Sequence) return false;
  for (const UniquePointer<YamlNode>& item : installers->children.view()) {
    if (item->kind != YamlNode::Kind::Map) continue;
    UniquePointer<WingetInstallerEntry> entry = UniquePointer<WingetInstallerEntry>::make(allocator, allocator);
    if (!entry) return false;
    // What an installer does not say for itself, the manifest says for all of them.
    auto own_or_shared = [&](StringView key) { return item->find(key) ? item->text(key) : root.text(key); };
    if (!entry->architecture.assign(item->text("Architecture")) || !entry->type.assign(own_or_shared("InstallerType")) ||
        !entry->nested_type.assign(own_or_shared("NestedInstallerType")) || !entry->url.assign(item->text("InstallerUrl")) ||
        !entry->sha256.assign(item->text("InstallerSha256")))
      return false;
    const YamlNode* switches = item->find("InstallerSwitches");
    if (!switches) switches = root.find("InstallerSwitches");
    if (switches && switches->kind == YamlNode::Kind::Map) {
      const StringView silent = !switches->text("Silent").empty() ? switches->text("Silent") : switches->text("SilentWithProgress");
      if (!entry->silent.assign(silent) || !entry->custom.assign(switches->text("Custom"))) return false;
    }
    const YamlNode* apps = item->find("AppsAndFeaturesEntries");
    if (!apps) apps = root.find("AppsAndFeaturesEntries");
    const YamlNode* app = apps && apps->kind == YamlNode::Kind::Sequence && apps->children.count() > 0 ? apps->children.view()[0].get() : nullptr;
    const StringView product_code = !own_or_shared("ProductCode").empty() ? own_or_shared("ProductCode") : app ? app->text("ProductCode") : StringView();
    if (!entry->product_code.assign(product_code) || (app && !entry->display_name.assign(app->text("DisplayName")))) return false;
    const YamlNode* codes = item->find("InstallerSuccessCodes");
    if (!codes) codes = root.find("InstallerSuccessCodes");
    if (codes && codes->kind == YamlNode::Kind::Sequence) {
      for (const UniquePointer<YamlNode>& code : codes->children.view()) {
        uint64_t value = 0;
        bool number = code->kind == YamlNode::Kind::Scalar && !code->value.empty() && code->value.size() <= 10;
        for (const char c : code->value.view()) number = number && c >= '0' && c <= '9', value = value * 10 + static_cast<uint64_t>(c - '0');
        if (number && value <= 0xFFFFFFFFull && entry->success_codes.count() < 64 && !entry->success_codes.push_back(static_cast<uint32_t>(value))) return false;
      }
    }
    const YamlNode* nested = item->find("NestedInstallerFiles");
    if (!nested) nested = root.find("NestedInstallerFiles");
    if (nested && nested->kind == YamlNode::Kind::Sequence) {
      for (const UniquePointer<YamlNode>& file : nested->children.view()) {
        if (file->kind != YamlNode::Kind::Map || entry->nested.count() >= kMaxShortcuts) continue;
        UniquePointer<WingetNestedFile> one = UniquePointer<WingetNestedFile>::make(allocator, allocator);
        if (!one || !one->path.assign(file->text("RelativeFilePath")) || !one->alias.assign(file->text("PortableCommandAlias")) || !entry->nested.push_back(util::move(one)))
          return false;
      }
    }
    if (!manifest->installers.push_back(util::move(entry))) return false;
  }
  return true;
}

const WingetInstallerEntry* WingetInstaller::choose_installer(const WingetManifest& manifest, uint32_t kinds, String* why) {
  why->clear();
  const WingetInstallerEntry* best = nullptr;
  const WingetInstallerEntry* other = nullptr;      // one for this computer that cannot be installed
  int best_rank = 0;
  for (const UniquePointer<WingetInstallerEntry>& entry : manifest.installers.view()) {
    const int architecture = architecture_rank(entry->architecture.view());
    if (architecture < 0) continue;
    const bool x86 = architecture == 2;
    const bool native = !x86 && (kinds & kNative) && is_native(*entry), setup = (kinds & kSetup) && is_setup(*entry) && (!x86 || x86_setup_runs(*entry)),
               package = !x86 && (kinds & kSetup) && is_msi(*entry);
    if (!native && !setup && !package) {
      if (!other) other = entry.get();
      continue;
    }
    // (a setup program of a kind that is known to run here before a package this system installs by its description, which may ask for more than it does)
    // (and any program for this computer's own architecture before a 32-bit one)
    const int rank = (x86 ? 100 : 0) + (native ? 0 : setup ? 10 : 20) + architecture;
    if (!best || rank < best_rank) best = entry.get(), best_rank = rank;
  }
  if (best) return best;
  if (!other) {
    (void)why->assign("there is no version of it for this kind of computer (64-bit x64)");
  } else if (architecture_rank(other->architecture.view()) == 2) {
    (void)(why->assign("there is only a 32-bit version of it, installed by ") && why->append(kind_name(other->type.view())) && why->append(", which cannot be run here yet"));
  } else if (is_native(*other)) {      // (asked for a setup program only: an update of a package that was installed by one)
    (void)why->assign("this version does not come as a setup program, as the installed one did; remove it and install it again");
  } else if ((kinds & kSetup) && equals_folded(other->type.view(), "exe")) {
    (void)why->assign("its setup program has no way to run without asking questions");
  } else if (is_setup(*other) || is_msi(*other)) {
    (void)(why->assign("it is installed by ") && why->append(kind_name(other->type.view())) && why->append(kinds & kNative ? ", which cannot be run here yet" : ", not put in place as the installed version was; remove it and install it again"));
  } else {
    const StringView type = equals_folded(other->type.view(), "zip") && !other->nested_type.empty() && !equals_folded(other->nested_type.view(), "portable") ? other->nested_type.view() : other->type.view();
    (void)(why->assign("it is installed by ") && why->append(kind_name(type)) && why->append(", which cannot be run here yet"));
  }
  return nullptr;
}

bool WingetInstaller::split_command_line(allocators::IAllocator& allocator, StringView text, String* program, List<String>* arguments) {
  arguments->clear();
  text = text.trimmed();
  if (program) {
    program->clear();
    if (text.starts_with('"')) {
      const uint64_t close = text.find('"', 1);
      if (close == StringView::npos) return false;
      if (!program->assign(text.substr(1, close - 1))) return false;
      text = text.substr(close + 1);
    } else {
      // Up to the first ".exe" that ends a word, else the first word.
      uint64_t end = StringView::npos;
      for (uint64_t i = 0; i + 4 <= text.size() && end == StringView::npos; ++i)
        if (equals_folded(text.substr(i, 4), ".exe") && (i + 4 == text.size() || text[i + 4] == ' ' || text[i + 4] == '\t')) end = i + 4;
      if (end == StringView::npos) end = text.find(' ') == StringView::npos ? text.size() : text.find(' ');
      if (!program->assign(text.first(end))) return false;
      text = text.substr(end);
    }
    if (program->empty()) return false;
  }
  // The arguments: words, with "..." keeping spaces together and \" a quote.
  String word(allocator);
  bool in_word = false, quoted = false;
  for (uint64_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    if (i < text.size() && c == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
      if (!word.push_back('"')) return false;
      in_word = true;
      ++i;
    } else if (i < text.size() && c == '"') {
      quoted = !quoted;
      in_word = true;
    } else if ((c == ' ' || c == '\t') && (!quoted || i == text.size())) {
      if (in_word) {
        String done(allocator);
        if (arguments->count() >= 256 || !done.assign(word.view()) || !arguments->push_back(util::move(done))) return false;
      }
      word.clear();
      in_word = false;
    } else {
      if (!word.push_back(c)) return false;
      in_word = true;
    }
  }
  return true;
}

bool WingetInstaller::folder_of(StringView id, String* out) const {
  return valid_package_id(id) && join(out, filesystem::layout::kWindowsWingetPackages, "/", id, kFolderSuffix);
}

bool WingetInstaller::read_record(StringView id, WingetInstalled* app, List<String>* shortcuts) const {
  String folder(allocator_), path(allocator_), text(allocator_);
  if (!folder_of(id, &folder) || !join(&path, folder.view(), "/", kRecordName) || !read_text_file(files_, allocator_, path.view(), kMaxRecordBytes, &text)) return false;
  bool ok = true, named = false;
  StringView rest = text.view(), line;
  while (ok && rest.next_token('\n', &line)) {
    StringView key, value;
    if (!line.split_once(':', &key, &value)) continue;
    value = value.trimmed();
    if (key.equals("PackageIdentifier")) ok = app->id.assign(value), named = !value.empty();
    else if (key.equals("PackageName")) ok = app->name.assign(value);
    else if (key.equals("PackageVersion")) ok = app->version.assign(value);
    else if (key.equals("Shortcut") && shortcuts && shortcuts->count() < kMaxShortcuts) {
      // (a name of a file in the Start menu's folder and nothing else: the record is a file anyone can edit)
      String name(allocator_), safe(allocator_);
      if (safe_member_path(value, &safe) && !safe.view().contains('/') && safe.view().ends_with(".lnk")) ok = name.assign(safe.view()) && shortcuts->push_back(util::move(name));
    }
  }
  return ok && named;
}

bool WingetInstaller::setup_record_path(StringView id, String* out) const {
  return valid_package_id(id) && join(out, filesystem::layout::kWingetRecords, "/", id, ".txt");
}

bool WingetInstaller::read_setup_record(StringView id, WingetInstalled* app, SetupRecord* record) const {
  String path(allocator_), text(allocator_);
  if (!setup_record_path(id, &path) || !read_text_file(files_, allocator_, path.view(), kMaxRecordBytes, &text)) return false;
  bool ok = true, named = false;
  StringView rest = text.view(), line;
  while (ok && rest.next_token('\n', &line)) {
    StringView key, value;
    if (!line.split_once(':', &key, &value)) continue;
    value = value.trimmed();
    if (key.equals("PackageIdentifier")) ok = app->id.assign(value), named = !value.empty();
    else if (key.equals("PackageName")) ok = app->name.assign(value);
    else if (key.equals("PackageVersion")) ok = app->version.assign(value);
    else if (key.equals("InstallerType")) ok = record->type.assign(value);
    else if (key.equals("Uninstall")) ok = record->uninstall.assign(value);
    else if (key.equals("QuietUninstall")) record->quiet = value.equals("yes");
    else if (key.equals("ProductCode") && product_code(value)) ok = record->product.assign(value);
    else if (key.equals("Package")) {
      // (a file in the folder of kept packages and nothing else: the record is a file anyone can edit)
      const StringView folder(filesystem::layout::kWindowsInstallerCache);
      if (value.starts_with(folder) && value.size() > folder.size() + 1 && value[folder.size()] == '/' && !value.substr(folder.size() + 1).contains('/') && value.ends_with(".msi") &&
          !value.contains(".."))
        ok = record->package.assign(value);
    }
  }
  return ok && named;
}

bool WingetInstaller::installed(StringView id) const {
  WingetInstalled app(allocator_);
  SetupRecord record(allocator_);
  return read_record(id, &app, nullptr) || read_setup_record(id, &app, &record);
}

// Is the file at `path` a Windows program for the processor this system runs programs on (x86-64)?
bool WingetInstaller::is_for_this_processor(StringView path) const {
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(path, filesystem::OpenMode::Read, &file) != Status::Ok) return false;
  ByteBuffer head(allocator_);
  const uint64_t size = file->size() < exe::kIdentifyBytes ? file->size() : exe::kIdentifyBytes;
  uint64_t got = 0;
  if (!head.put_zeros(size) || file->read(0, head.writable_view(), &got) != Status::Ok || got > size) return false;
  const ConstByteView bytes = head.view().first(got);
  return exe::identify_executable(bytes) == exe::ExecutableKind::Pe && exe::identify_architecture(bytes) == exe::Architecture::X86_64;
}

// Can the setup program (or uninstaller) at `path`, of a package of installer type `type`, be run here? A 64-bit Windows program can. A 32-bit one
// (most setup programs are, whatever they install) can if it is of a kind that has been run here: Nullsoft's and Inno Setup's (docs/Wow64.md). One of
// no known kind ("exe") is anybody's program, and what it needs of the 32-bit libraries is not known.
bool WingetInstaller::setup_program_runs(StringView path, StringView type) const {
  if (is_for_this_processor(path)) return true;
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(path, filesystem::OpenMode::Read, &file) != Status::Ok) return false;
  ByteBuffer head(allocator_);
  const uint64_t size = file->size() < exe::kIdentifyBytes ? file->size() : exe::kIdentifyBytes;
  uint64_t got = 0;
  if (!head.put_zeros(size) || file->read(0, head.writable_view(), &got) != Status::Ok || got > size) return false;
  const ConstByteView bytes = head.view().first(got);
  return exe::identify_executable(bytes) == exe::ExecutableKind::Pe && exe::identify_architecture(bytes) == exe::Architecture::X86_32 && (equals_folded(type, "nullsoft") || equals_folded(type, "inno"));
}

bool WingetInstaller::list_installed(List<WingetInstalled>* apps) const {
  apps->clear();
  UniquePointer<filesystem::IDirectory> listing;
  filesystem::DirectoryEntry entry;
  const bool have_folder = files_.open_directory(filesystem::layout::kWindowsWingetPackages, &listing) == Status::Ok;      // (not there: nothing put in place yet)
  for (uint32_t guard = 0; have_folder && guard < 100000 && listing->next(&entry) == Status::Ok; ++guard) {
    const StringView folder = entry.name.view();
    if (entry.type != filesystem::NodeType::Directory || !folder.ends_with(kFolderSuffix)) continue;
    const StringView id = folder.first(folder.size() - kFolderSuffix.size());
    WingetInstalled app(allocator_);
    if (!read_record(id, &app, nullptr)) continue;
    if (!app.id.assign(id)) return false;      // (the folder is the identifier, whatever the record says)
    if (app.name.empty() && !app.name.assign(id)) return false;
    if (!apps->push_back(util::move(app))) return false;
  }
  // The ones a setup program installed.
  UniquePointer<filesystem::IDirectory> records;
  if (files_.open_directory(filesystem::layout::kWingetRecords, &records) == Status::Ok) {
    for (uint32_t guard = 0; guard < 100000 && records->next(&entry) == Status::Ok; ++guard) {
      const StringView file = entry.name.view();
      if (entry.type != filesystem::NodeType::File || !file.ends_with(".txt")) continue;
      const StringView id = file.first(file.size() - 4);
      WingetInstalled app(allocator_);
      SetupRecord record(allocator_);
      if (!read_setup_record(id, &app, &record)) continue;
      bool listed = false;
      for (const WingetInstalled& other : static_cast<const List<WingetInstalled>*>(apps)->view()) listed |= equals_folded(other.id.view(), id);
      if (listed) continue;
      if (!app.id.assign(id)) return false;
      if (app.name.empty() && !app.name.assign(id)) return false;
      if (!apps->push_back(util::move(app))) return false;
    }
  }
  ArrayView<WingetInstalled> list = apps->view();
  for (uint64_t i = 1; i < list.size(); ++i)
    for (uint64_t j = i; j > 0 && compare_folded(list[j].name.view(), list[j - 1].name.view()) < 0; --j) {
      WingetInstalled t = util::move(list[j]);
      list[j] = util::move(list[j - 1]);
      list[j - 1] = util::move(t);
    }
  return true;
}

Result WingetInstaller::resolve(const WingetPackage& package, WingetManifest* manifest, String* detail) {
  detail->clear();
  if (!valid_package_id(package.id.view())) return Result::InvalidName;
  // 1. The version list, which must be the one the index names.
  String address(allocator_), packed(allocator_), list(allocator_);
  if (!address.assign(kWingetCache) || !address.append("packages/") || !append_escaped(&address, package.id.view(), false) || !address.push_back('/') ||
      !append_hex(&address, ConstByteView(package.hash).first(4)) || !address.append("/versionData.mszyml"))
    return Result::OutOfMemory;
  if (!network_.fetch(address.view(), kMaxVersionDataBytes, &packed) || packed.empty()) {
    detail->assign(address.view());
    return Result::NoManifest;
  }
  uint8_t hash[32];
  crypto::hash(crypto::HashKind::Sha256, packed.view().bytes(), hash);
  if (!crypto::equal_constant_time(hash, package.hash, 32)) {
    detail->assign("its list of versions is not the one the package list names; choose Refresh List and try again");
    return Result::NoManifest;
  }
  String version(allocator_), path(allocator_), sha256(allocator_);
  if (!decode_mszyml(allocator_, packed.view().bytes(), kMaxVersionListBytes, &list) || !find_version(allocator_, list.view(), package.version.view(), &version, &path, &sha256)) {
    detail->assign("its list of versions could not be read");
    return Result::NoManifest;
  }
  // 2. The manifest, which must be the one the version list names.
  String yaml(allocator_);
  if (!address.assign(kWingetCache) || !append_escaped(&address, path.view(), true)) return Result::OutOfMemory;
  if (!network_.fetch(address.view(), kMaxManifestBytes, &yaml) || yaml.empty()) {
    detail->assign(address.view());
    return Result::NoManifest;
  }
  if (!sha256_is(yaml.view().bytes(), sha256.view(), allocator_)) {
    detail->assign("its manifest is not the one its list of versions names");
    return Result::NoManifest;
  }
  if (!parse_manifest(allocator_, yaml.view(), manifest) || !equals_folded(manifest->id.view(), package.id.view())) {
    detail->assign("its manifest could not be read");
    return Result::NoManifest;
  }
  if (manifest->version.empty() && !manifest->version.assign(version.view())) return Result::OutOfMemory;
  if (manifest->name.empty() && !manifest->name.assign(package.name.view())) return Result::OutOfMemory;
  return Result::Ok;
}

Result WingetInstaller::install(const WingetPackage& package, Progress* progress, String* detail, bool ignore_checksum) {
  WingetManifest manifest(allocator_);
  const Result r = resolve(package, &manifest, detail);
  if (r != Result::Ok) return r;
  // A package that is installed stays of the kind it was installed as.
  WingetInstalled current(allocator_);
  SetupRecord record(allocator_);
  const bool was_native = read_record(package.id.view(), &current, nullptr);
  const bool was_setup = !was_native && read_setup_record(package.id.view(), &current, &record);
  const uint32_t can_run = setup_ ? uint32_t{kSetup} : 0u;
  const uint32_t kinds = was_native ? uint32_t{kNative} : was_setup ? can_run : kNative | can_run;
  const WingetInstallerEntry* entry = choose_installer(manifest, kinds, detail);
  if (!entry) return Result::NotSupported;
  if (is_native(*entry)) return place(manifest, *entry, was_native, ignore_checksum, progress, detail);
  return is_msi(*entry) ? place_msi(manifest, *entry, ignore_checksum, progress, detail) : place_setup(manifest, *entry, ignore_checksum, progress, detail);
}

Result WingetInstaller::update(const WingetPackage& package, Progress* progress, String* detail, bool ignore_checksum) {
  detail->clear();
  WingetInstalled current(allocator_);
  SetupRecord record(allocator_);
  if (!read_record(package.id.view(), &current, nullptr) && !read_setup_record(package.id.view(), &current, &record)) return Result::NotInstalled;
  if (current.version.view().equals(package.version.view())) return Result::UpToDate;
  return install(package, progress, detail, ignore_checksum);
}

void WingetInstaller::remove_shortcuts(const List<String>& shortcuts) {
  for (const String& name : shortcuts.view()) {
    String path(allocator_);
    if (join(&path, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view())) (void)files_.remove(path.view());
  }
}

// Empties the folder but for the file called `keep`.
bool WingetInstaller::clear_folder(StringView folder, StringView keep) {
  List<String> names(allocator_);
  {
    UniquePointer<filesystem::IDirectory> listing;
    if (files_.open_directory(folder, &listing) != Status::Ok) return false;
    filesystem::DirectoryEntry child;
    for (uint32_t guard = 0; guard < 100000 && listing->next(&child) == Status::Ok; ++guard) {
      String full(allocator_);
      if (child.name.view().equals(keep)) continue;
      if (!join(&full, folder, "/", child.name.view()) || !names.push_back(util::move(full))) return false;
    }
  }
  for (const String& child : names.view())
    if (!remove_tree(files_, allocator_, child.view())) return false;
  return true;
}

// Steps 4 and 5.
Result WingetInstaller::place(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool was_installed, bool ignore_checksum, Progress* progress, String* detail) {
  const StringView id = manifest.id.view();
  const bool archive = equals_folded(entry.type.view(), "zip");
  String folder(allocator_), part(allocator_);
  if (!folder_of(id, &folder)) return Result::InvalidName;
  if (!join(&part, folder.view(), "/download.part")) return Result::OutOfMemory;
  if (!make_folders(files_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  // Until the new files are in place a failure leaves what was there: the old version, or nothing.
  auto give_up = [&](Result result) {
    (void)files_.remove(part.view());
    if (!was_installed) (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };

  // 4. The download, and its checksum.
  crypto::Hasher hasher(crypto::HashKind::Sha256);
  const Result fetched = download_to_file(files_, network_, entry.url.view(), part.view(), progress, &hasher, detail);
  if (fetched != Result::Ok) return give_up(fetched);
  uint8_t hash[32];
  hasher.finish(hash);
  String hex(allocator_);
  if (!append_hex(&hex, ConstByteView(hash))) return give_up(Result::OutOfMemory);
  if (!checksum_accepted(hex.view(), entry, ignore_checksum)) {
    detail->assign(entry.url.view());
    return give_up(Result::BadChecksum);
  }
  if (!archive && !is_for_this_processor(part.view())) {
    const bool windows_program = exe::identify_executable_file(files_, part.view()) == exe::ExecutableKind::Pe;
    detail->assign(windows_program ? "it is a 32-bit program (or one for another processor), which cannot be run here yet" : "it is not a Windows program");
    return give_up(windows_program ? Result::NotSupported : Result::NotAnAppImage);
  }

  // The programs the package has, and what their shortcuts are called.
  List<UniquePointer<Program>> programs(allocator_);
  if (archive) {
    for (const UniquePointer<WingetNestedFile>& nested : entry.nested.view()) {
      UniquePointer<Program> program = UniquePointer<Program>::make(allocator_, allocator_);
      if (!program) return give_up(Result::OutOfMemory);
      if (!safe_member_path(nested->path.view(), &program->path)) {
        detail->assign("its manifest names a program outside the package");
        return give_up(Result::NotAnAppImage);
      }
      const StringView label = entry.nested.count() == 1 ? manifest.name.view() : !nested->alias.empty() ? nested->alias.view() : stem_of(leaf_of(program->path.view()));
      if (!safe_name(label, &program->label) || !programs.push_back(util::move(program))) return give_up(Result::OutOfMemory);
    }
  } else {
    // One program, called after its command (as winget calls it), else after the file of the download.
    UniquePointer<Program> program = UniquePointer<Program>::make(allocator_, allocator_);
    if (!program) return give_up(Result::OutOfMemory);
    StringView name = manifest.command.view();
    if (name.empty()) {
      name = entry.url.view();
      const uint64_t query = name.find('?');
      if (query != StringView::npos) name = name.first(query);
      name = leaf_of(name);
    }
    if (!safe_name(name, &program->path)) return give_up(Result::OutOfMemory);
    if (program->path.empty() && !safe_name(id, &program->path)) return give_up(Result::OutOfMemory);
    const StringView so_far = program->path.view();
    if ((so_far.size() < 4 || !equals_folded(so_far.substr(so_far.size() - 4), ".exe")) && !program->path.append(".exe")) return give_up(Result::OutOfMemory);
    if (!safe_name(manifest.name.view(), &program->label) || !programs.push_back(util::move(program))) return give_up(Result::OutOfMemory);
  }

  // 5. In place of the old version: its shortcuts and its files go (the download stays).
  List<String> old_shortcuts(allocator_);
  WingetInstalled old(allocator_);
  if (was_installed) (void)read_record(id, &old, &old_shortcuts);
  remove_shortcuts(old_shortcuts);
  List<String> shortcuts(allocator_);
  auto fail = [&](Result result) {      // (from here on a failure leaves nothing: half a program is worse)
    remove_shortcuts(shortcuts);
    (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };
  if (!clear_folder(folder.view(), "download.part")) {
    detail->assign(folder.view());
    return fail(Result::FileError);
  }
  if (archive) {
    const Result unpacked = unpack(part.view(), folder.view(), detail);
    (void)files_.remove(part.view());
    if (unpacked != Result::Ok) return fail(unpacked);
  } else {
    String program(allocator_);
    if (!join(&program, folder.view(), "/", programs.view()[0]->path.view())) return fail(Result::OutOfMemory);
    if (files_.rename(part.view(), program.view()) != Status::Ok) {
      detail->assign(program.view());
      return fail(Result::FileError);
    }
  }

  // The shortcuts (of the programs that are Windows programs), and the record.
  String record(allocator_);
  if (!join(&record, "PackageIdentifier: ", id, "\nPackageName: ", manifest.name.view()) || !record.append("\nPackageVersion: ") || !record.append(manifest.version.view()) ||
      !record.append("\nInstallerUrl: ") || !record.append(entry.url.view()) || !record.push_back('\n'))
    return fail(Result::OutOfMemory);
  if (!make_folders(files_, filesystem::layout::kWindowsStartMenuPrograms)) {
    detail->assign(filesystem::layout::kWindowsStartMenuPrograms);
    return fail(Result::FileError);
  }
  for (const UniquePointer<Program>& program : programs.view()) {
    String path(allocator_), target(allocator_), directory(allocator_), link(allocator_), name(allocator_);
    if (!join(&path, folder.view(), "/", program->path.view())) return fail(Result::OutOfMemory);
    if (!is_for_this_processor(path.view())) {
      if (programs.count() == 1) {
        detail->assign("the program its manifest names is not in the archive, or is not a Windows program for this computer's processor");
        return fail(Result::NotAnAppImage);
      }
      continue;
    }
    if (program->label.empty() && !program->label.assign(stem_of(leaf_of(program->path.view())))) return fail(Result::OutOfMemory);
    // A shortcut of that name that is someone else's stays theirs: this one says whose it is.
    if (!join(&name, program->label.view(), ".lnk") || !join(&link, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view())) return fail(Result::OutOfMemory);
    bool taken = exists(files_, link.view());
    for (const String& mine : shortcuts.view()) taken |= equals_folded(mine.view(), name.view());
    if (taken) {
      String owner(allocator_);
      if (!safe_name(id, &owner) || !join(&name, program->label.view(), " (", owner.view(), ").lnk") || !join(&link, filesystem::layout::kWindowsStartMenuPrograms, "/", name.view()))
        return fail(Result::OutOfMemory);
    }
    ByteBuffer bytes(allocator_);
    if (!windows_path(path.view(), &target) || !windows_path(parent_of(path.view()), &directory) || !write_shell_link(target.view(), directory.view(), &bytes) ||
        !write_bytes(files_, link.view(), bytes.view())) {
      detail->assign(link.view());
      return fail(Result::FileError);
    }
    if (!record.append("Shortcut: ") || !record.append(name.view()) || !record.push_back('\n') || !shortcuts.push_back(util::move(name))) return fail(Result::OutOfMemory);
  }
  String record_path(allocator_);
  if (!join(&record_path, folder.view(), "/", kRecordName)) return fail(Result::OutOfMemory);
  if (!write_text_file(files_, record_path.view(), record.view())) {
    detail->assign(record_path.view());
    return fail(Result::FileError);
  }
  return Result::Ok;
}

// Every file of the archive at `archive`, into `folder`.
Result WingetInstaller::unpack(StringView archive, StringView folder, String* detail) {
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(archive, filesystem::OpenMode::Read, &file) != Status::Ok) {
    detail->assign(archive);
    return Result::FileError;
  }
  FileZipSource source(*file);
  media::ZipReader zip(allocator_, source);
  if (!zip.open()) {
    detail->assign("the download is not a .zip archive");
    return Result::NotAnAppImage;
  }
  uint64_t total = 0;
  for (const media::ZipEntry& entry : zip.entries()) {
    String relative(allocator_), path(allocator_);
    if (!safe_member_path(entry.name.view(), &relative)) {
      detail->assign("the archive holds a file with a name that is not safe to unpack");
      return Result::NotAnAppImage;
    }
    if (!join(&path, folder, "/", relative.view())) return Result::OutOfMemory;
    if (entry.directory()) {
      if (!make_folders(files_, path.view())) {
        detail->assign(path.view());
        return Result::FileError;
      }
      continue;
    }
    if (entry.size > kMaxMemberBytes || entry.size > kMaxArchiveBytes - total) {
      detail->assign("the archive holds more than this program can unpack");
      return Result::NotAnAppImage;
    }
    total += entry.size;
    ByteBuffer bytes(allocator_);
    if (!zip.extract(entry, kMaxMemberBytes, &bytes)) {
      (void)(detail->assign("the archive is damaged, or there is not enough memory to unpack it: ") && detail->append(relative.view()));
      return Result::NotAnAppImage;
    }
    if (!make_folders(files_, parent_of(path.view())) || !write_bytes(files_, path.view(), bytes.view())) {
      detail->assign(path.view());
      return Result::FileError;
    }
  }
  return Result::Ok;
}

Result WingetInstaller::remove(StringView id, String* detail) {
  detail->clear();
  if (!valid_package_id(id)) return Result::InvalidName;
  WingetInstalled app(allocator_);
  List<String> shortcuts(allocator_);
  String folder(allocator_);
  if (!read_record(id, &app, &shortcuts)) {
    SetupRecord record(allocator_);
    return read_setup_record(id, &app, &record) ? remove_setup(id, record, detail) : Result::NotInstalled;
  }
  if (!folder_of(id, &folder)) return Result::OutOfMemory;
  remove_shortcuts(shortcuts);
  if (!remove_tree(files_, allocator_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  return Result::Ok;
}

// What a setup program's log says of why it gave up, as plain text: six lines (600 characters at most) from the first place it reports an error (Inno
// Setup writes "Exception message:" or "Defaulting to ... for suppressed message box" there, and what follows is the rolling back), else its last
// three lines (400 characters at most). The log is the program's own writing, so nothing is taken on trust: only printable ASCII is kept. False if there is no log to read.
bool WingetInstaller::log_tail(StringView path, String* out) const {
  constexpr uint64_t kLogLimit = 4 * 1024 * 1024, kTailCharacters = 400, kErrorCharacters = 600;
  constexpr uint32_t kTailLines = 3, kErrorLines = 6;      // (an error is a heading, the file it is about, a blank line and the message)
  String text(allocator_);
  out->clear();
  if (!read_text_file(files_, allocator_, path, kLogLimit, &text)) return false;
  StringView rest = text.view();
  while (!rest.empty() && (rest[rest.size() - 1] == '\n' || rest[rest.size() - 1] == '\r')) rest = rest.first(rest.size() - 1);
  // The first line that reports an error, if any.
  uint64_t error_at = StringView::npos;
  static constexpr StringView kMarks[] = {"Exception message:", "Defaulting to ", "Message box ("};      // ("Defaulting to OK / Abort / No for suppressed message box")
  for (const StringView mark : kMarks) {
    const uint64_t at = rest.find(mark);
    if (at != StringView::npos && (error_at == StringView::npos || at < error_at)) error_at = at;
  }
  if (error_at != StringView::npos) {
    while (error_at > 0 && rest[error_at - 1] != '\n') --error_at;      // (from the start of that line)
    rest = rest.without_prefix(error_at);
    uint64_t end = 0;
    for (uint32_t lines = 0; end < rest.size(); ++end) {
      if (rest[end] == '\n' && ++lines == kErrorLines) break;
    }
    rest = rest.first(end);
    if (rest.size() > kErrorCharacters) rest = rest.first(kErrorCharacters);
  } else {
    uint64_t start = rest.size();
    for (uint32_t lines = 0; start > 0; --start) {
      if (rest[start - 1] != '\n') continue;
      if (++lines == kTailLines) break;
    }
    rest = rest.without_prefix(start);
    if (rest.size() > kTailCharacters) rest = rest.without_prefix(rest.size() - kTailCharacters);
  }
  for (const char c : rest) {
    const char shown = c == '\n' ? '|' : (c >= 0x20 && c < 0x7F) ? c : c == '\r' ? '\0' : ' ';
    if (shown != '\0' && !out->push_back(shown)) return false;
  }
  return true;
}

// A package that comes as a setup program: downloaded, checked, run, and what it registered remembered.
Result WingetInstaller::place_setup(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool ignore_checksum, Progress* progress, String* detail) {
  if (!setup_) return Result::NotSupported;
  const StringView id = manifest.id.view();
  String folder(allocator_), version(allocator_), name(allocator_), program(allocator_), record_path(allocator_);
  if (!setup_record_path(id, &record_path)) return Result::InvalidName;
  if (!safe_name(manifest.version.view(), &version)) return Result::OutOfMemory;
  StringView leaf = entry.url.view();
  const uint64_t query = leaf.find('?');
  if (query != StringView::npos) leaf = leaf.first(query);
  if (!safe_name(leaf_of(leaf), &name) || (name.empty() && !name.assign("setup"))) return Result::OutOfMemory;
  if ((name.size() < 4 || !equals_folded(name.view().substr(name.size() - 4), ".exe")) && !name.append(".exe")) return Result::OutOfMemory;
  if (!join(&folder, filesystem::layout::kWindowsWingetTemp, "/", id, ".") || !folder.append(version.view()) || !join(&program, folder.view(), "/", name.view())) return Result::OutOfMemory;
  if (!make_folders(files_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  auto finish = [&](Result result) {      // (the setup program is not kept, whatever came of it)
    if (progress) __atomic_store_n(&progress->running_setup, false, __ATOMIC_RELAXED);
    (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };

  // The download, and that it is what the manifest names and something this system can run.
  crypto::Hasher hasher(crypto::HashKind::Sha256);
  const Result fetched = download_to_file(files_, network_, entry.url.view(), program.view(), progress, &hasher, detail);
  if (fetched != Result::Ok) return finish(fetched);
  uint8_t hash[32];
  hasher.finish(hash);
  String hex(allocator_);
  if (!append_hex(&hex, ConstByteView(hash))) return finish(Result::OutOfMemory);
  if (!checksum_accepted(hex.view(), entry, ignore_checksum)) {
    detail->assign(entry.url.view());
    return finish(Result::BadChecksum);
  }
  if (!setup_program_runs(program.view(), entry.type.view())) {
    const bool windows_program = exe::identify_executable_file(files_, program.view()) == exe::ExecutableKind::Pe;
    detail->assign(windows_program ? "its setup program is a 32-bit program of a kind that cannot be run here yet (or one for another processor)" : "its setup program is not a Windows program");
    return finish(windows_program ? Result::NotSupported : Result::NotAnAppImage);
  }

  // The switches: the manifest's own for a run that asks nothing, else the usual ones of the kind; then the ones the manifest always wants.
  List<String> switches(allocator_), custom(allocator_);
  if (!split_command_line(allocator_, !entry.silent.empty() ? entry.silent.view() : silent_switches(entry.type.view()), nullptr, &switches) ||
      !split_command_line(allocator_, entry.custom.view(), nullptr, &custom))
    return finish(Result::OutOfMemory);
  List<StringView> arguments(allocator_);
  for (const String& word : static_cast<const List<String>&>(switches).view())
    if (!arguments.push_back(word.view())) return finish(Result::OutOfMemory);
  for (const String& word : static_cast<const List<String>&>(custom).view())
    if (!arguments.push_back(word.view())) return finish(Result::OutOfMemory);
  // An Inno Setup program keeps a log when asked (as winget asks it): what it says last is the reason given when it fails. The log is in the folder of
  // the downloaded setup program, as the program names it (the Windows world is the folder /Windows, drive C), and goes with that folder.
  String log_path(allocator_), log_switch(allocator_);
  constexpr StringView kWindowsRoot("/Windows/");
  if (equals_folded(entry.type.view(), "inno") && folder.view().starts_with(kWindowsRoot)) {
    if (!join(&log_path, folder.view(), "/setup.log") || !log_switch.assign("/LOG=C:\\")) return finish(Result::OutOfMemory);
    for (const char c : log_path.view().without_prefix(kWindowsRoot.size()))
      if (!log_switch.push_back(c == '/' ? '\\' : c)) return finish(Result::OutOfMemory);
    if (!arguments.push_back(log_switch.view())) return finish(Result::OutOfMemory);
  }

  // Run it, with the registry's list of installed programs taken before and after.
  List<UniquePointer<UninstallEntry>> before(allocator_), after(allocator_);
  if (!setup_->uninstall_entries(&before)) return finish(Result::OutOfMemory);
  if (progress) {
    __atomic_store_n(&progress->running_setup, true, __ATOMIC_RELAXED);
    if (progress->changed) progress->changed(progress->context, *progress);
  }
  uint32_t code = 0;
  String message(allocator_);
  const bool ran = setup_->run(program.view(), static_cast<const List<StringView>&>(arguments).view(), progress ? &progress->cancel : nullptr, &code, &message);
  if (!ran) {
    if (progress && __atomic_load_n(&progress->cancel, __ATOMIC_RELAXED)) return finish(Result::Cancelled);
    (void)(detail->assign("the setup program: ") && detail->append(message.view()));
    return finish(Result::SetupFailed);
  }
  bool success = code == 0;
  for (const uint32_t good : entry.success_codes.view()) success |= code == good;
  if (!success) {
    (void)(detail->assign("the setup program ended with code ") && detail->append_uint(code));
    String tail(allocator_);
    if (!log_path.empty() && log_tail(log_path.view(), &tail) && !tail.empty()) (void)(detail->append("; its log ends: ") && detail->append(tail.view()));
    return finish(Result::SetupFailed);
  }
  if (!setup_->uninstall_entries(&after)) return finish(Result::OutOfMemory);

  // What it registered: an entry that was not there before (the one the manifest names, if there are several); else, for a program that was
  // installed already, the one the manifest names.
  const StringView wanted_name = !entry.display_name.empty() ? entry.display_name.view() : manifest.name.view();
  const UninstallEntry* found = nullptr;
  int found_score = -1;
  for (const UniquePointer<UninstallEntry>& candidate : after.view()) {
    bool is_new = true;
    for (const UniquePointer<UninstallEntry>& old : before.view()) is_new &= !equals_folded(old->key.view(), candidate->key.view());
    const bool by_code = !entry.product_code.empty() && equals_folded(key_leaf(candidate->key.view()), entry.product_code.view());
    const bool by_name = !wanted_name.empty() && contains_folded(candidate->name.view(), wanted_name);
    if (!is_new && !by_code && !by_name) continue;
    const int score = (by_code ? 4 : 0) + (by_name ? 2 : 0) + (is_new ? 1 : 0);
    if (score > found_score) found = candidate.get(), found_score = score;
  }

  String record(allocator_);
  if (!join(&record, "PackageIdentifier: ", id, "\nPackageName: ", manifest.name.view()) || !record.append("\nPackageVersion: ") || !record.append(manifest.version.view()) ||
      !record.append("\nInstallerType: ") || !record.append(entry.type.view()) || !record.append("\nInstallerUrl: ") || !record.append(entry.url.view()) || !record.push_back('\n'))
    return finish(Result::OutOfMemory);
  if (found) {
    const bool quiet = !found->quiet_command.empty();
    const StringView command = quiet ? found->quiet_command.view() : found->command.view();
    bool one_line = !command.empty();
    for (const char c : command) one_line &= static_cast<uint8_t>(c) >= 0x20;
    if (one_line && (!record.append("UninstallKey: ") || !record.append(found->key.view()) || !record.append("\nUninstall: ") || !record.append(command) ||
                     !record.append(quiet ? "\nQuietUninstall: yes\n" : "\n")))
      return finish(Result::OutOfMemory);
  }
  String asked(allocator_);
  if (join(&asked, record_path.view(), ".asked")) (void)files_.remove(asked.view());      // (of an earlier Remove that found no uninstaller)
  if (!make_folders(files_, filesystem::layout::kWingetRecords) || !write_text_file(files_, record_path.view(), record.view())) {
    detail->assign(record_path.view());
    return finish(Result::FileError);
  }
  return finish(Result::Ok);
}

// A package that is a Windows Installer package: downloaded, checked, and installed by this system from its description. The package is kept (as
// Windows keeps it) because removing the product is running the same package the other way.
Result WingetInstaller::place_msi(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool ignore_checksum, Progress* progress, String* detail) {
  if (!setup_) return Result::NotSupported;
  const StringView id = manifest.id.view();
  String folder(allocator_), version(allocator_), name(allocator_), package(allocator_), record_path(allocator_);
  if (!setup_record_path(id, &record_path)) return Result::InvalidName;
  if (!safe_name(manifest.version.view(), &version)) return Result::OutOfMemory;
  StringView leaf = entry.url.view();
  const uint64_t query = leaf.find('?');
  if (query != StringView::npos) leaf = leaf.first(query);
  if (!safe_name(leaf_of(leaf), &name) || (name.empty() && !name.assign("package"))) return Result::OutOfMemory;
  if ((name.size() < 4 || !equals_folded(name.view().substr(name.size() - 4), ".msi")) && !name.append(".msi")) return Result::OutOfMemory;
  if (!join(&folder, filesystem::layout::kWindowsWingetTemp, "/", id, ".") || !folder.append(version.view()) || !join(&package, folder.view(), "/", name.view())) return Result::OutOfMemory;
  if (!make_folders(files_, folder.view())) {
    detail->assign(folder.view());
    return Result::FileError;
  }
  auto finish = [&](Result result) {
    if (progress) __atomic_store_n(&progress->running_setup, false, __ATOMIC_RELAXED);
    (void)remove_tree(files_, allocator_, folder.view());
    return result;
  };

  crypto::Hasher hasher(crypto::HashKind::Sha256);
  const Result fetched = download_to_file(files_, network_, entry.url.view(), package.view(), progress, &hasher, detail);
  if (fetched != Result::Ok) return finish(fetched);
  uint8_t hash[32];
  hasher.finish(hash);
  String hex(allocator_);
  if (!append_hex(&hex, ConstByteView(hash))) return finish(Result::OutOfMemory);
  if (!checksum_accepted(hex.view(), entry, ignore_checksum)) {
    detail->assign(entry.url.view());
    return finish(Result::BadChecksum);
  }

  // The properties the manifest's switches set (NAME=value; the switches of msiexec itself, /qn and the like, say nothing this system does not do anyway).
  List<String> words(allocator_), more(allocator_);
  if (!split_command_line(allocator_, entry.silent.view(), nullptr, &words) || !split_command_line(allocator_, entry.custom.view(), nullptr, &more)) return finish(Result::OutOfMemory);
  List<StringView> properties(allocator_);
  auto add_properties = [&](const List<String>& list) {
    for (const String& word : list.view())
      if (!word.view().starts_with('/') && !word.view().starts_with('-') && word.view().contains('=') && !properties.push_back(word.view())) return false;
    return true;
  };
  if (!add_properties(words) || !add_properties(more)) return finish(Result::OutOfMemory);

  const bool* cancel = progress ? &progress->cancel : nullptr;
  MsiHost host(allocator_, files_, *setup_, cancel);
  String code(allocator_), windows_package(allocator_), notes(allocator_);
  if (!windows_path(package.view(), &windows_package)) return (Result::OutOfMemory);
  // (in a function of its own: the package's file is closed again before anything is done with the file itself)
  auto run_package = [&]() -> Result {
    UniquePointer<filesystem::IFile> file;
    if (files_.open_file(package.view(), filesystem::OpenMode::Read, &file) != Status::Ok) {
      detail->assign(package.view());
      return (Result::FileError);
    }
    FileZipSource source(*file);
    msi::Database database(allocator_, source);
    if (!database.open()) {
      detail->assign("the download is not a Windows Installer package, or is damaged");
      return (Result::NotAnAppImage);
    }
    const msi::Table* own = database.table("Property");
    const uint32_t code_row = own ? own->find("Property", "ProductCode") : msi::kNoColumn;
    if (code_row != msi::kNoColumn && !code.assign(own->text(code_row, "Value"))) return (Result::OutOfMemory);

    // An older version that is another product goes first (its package says how), as "being upgraded": what it keeps for the new one stays.
    WingetInstalled old(allocator_);
    SetupRecord old_record(allocator_);
    if (read_setup_record(id, &old, &old_record) && !old_record.package.empty() && !equals_folded(old_record.product.view(), code.view())) {
      String ignored(allocator_);
      (void)run_msi_removal(old_record.package.view(), code.view(), &ignored);
      (void)files_.remove(old_record.package.view());
    }

    if (progress) {
      __atomic_store_n(&progress->running_setup, true, __ATOMIC_RELAXED);
      if (progress->changed) progress->changed(progress->context, *progress);
    }
    msi::Installer installer(allocator_, files_, host, database);
    msi::Options options;
    options.properties = static_cast<const List<StringView>&>(properties).view();
    options.package_path = windows_package.view();
    options.cancel = cancel;
    const msi::Outcome outcome = installer.run(options, detail);
    if (outcome != msi::Outcome::Ok) {
      // What was put in place before it stopped is taken away again with the same package (not if the package was refused: nothing was done).
      if (outcome != msi::Outcome::NotSupported && outcome != msi::Outcome::Refused) {
        msi::Installer undo(allocator_, files_, host, database);
        msi::Options removal;
        removal.remove = true;
        removal.undo = true;
        removal.package_path = windows_package.view();
        String ignored(allocator_);
        (void)undo.run(removal, &ignored);
      }
      return (result_of(outcome));
    }
    for (const String& note : installer.notes())
      if (!notes.append("Note: ") || !notes.append(note.view()) || !notes.push_back('\n')) return (Result::OutOfMemory);
    return Result::Ok;
  };
  const Result ran = run_package();
  if (ran != Result::Ok) return finish(ran);

  // The package is kept under its product code, and the record says where.
  String kept(allocator_);
  if (product_code(code.view()) && make_folders(files_, filesystem::layout::kWindowsInstallerCache) && join(&kept, filesystem::layout::kWindowsInstallerCache, "/", code.view(), ".msi")) {
    (void)files_.remove(kept.view());
    if (files_.rename(package.view(), kept.view()) != Status::Ok) kept.clear();
  } else {
    kept.clear();
  }
  String record(allocator_);
  if (!join(&record, "PackageIdentifier: ", id, "\nPackageName: ", manifest.name.view()) || !record.append("\nPackageVersion: ") || !record.append(manifest.version.view()) ||
      !record.append("\nInstallerType: msi\nInstallerUrl: ") || !record.append(entry.url.view()) || !record.push_back('\n'))
    return finish(Result::OutOfMemory);
  if (!kept.empty() && (!record.append("ProductCode: ") || !record.append(code.view()) || !record.append("\nPackage: ") || !record.append(kept.view()) || !record.push_back('\n')))
    return finish(Result::OutOfMemory);
  if (!record.append(notes.view())) return finish(Result::OutOfMemory);
  String asked(allocator_);
  if (join(&asked, record_path.view(), ".asked")) (void)files_.remove(asked.view());
  if (!make_folders(files_, filesystem::layout::kWingetRecords) || !write_text_file(files_, record_path.view(), record.view())) {
    detail->assign(record_path.view());
    return finish(Result::FileError);
  }
  return finish(Result::Ok);
}

// Runs the kept package at `package` the other way: the product it installed is removed. `upgrading`: the product code of the version that replaces it.
Result WingetInstaller::run_msi_removal(StringView package, StringView upgrading, String* detail) {
  if (!setup_) {
    detail->assign("its package cannot be run here");
    return Result::NotSupported;
  }
  UniquePointer<filesystem::IFile> file;
  if (files_.open_file(package, filesystem::OpenMode::Read, &file) != Status::Ok) {
    detail->assign(package);
    return Result::FileError;
  }
  FileZipSource source(*file);
  msi::Database database(allocator_, source);
  if (!database.open()) {
    detail->assign("the package that was kept to remove it with cannot be read");
    return Result::SetupFailed;
  }
  MsiHost host(allocator_, files_, *setup_, nullptr);
  msi::Installer installer(allocator_, files_, host, database);
  msi::Options options;
  String windows_package(allocator_);
  if (!windows_path(package, &windows_package)) return Result::OutOfMemory;
  options.remove = true;
  options.package_path = windows_package.view();
  options.upgrading_product = upgrading;
  return result_of(installer.run(options, detail));
}

// Runs the uninstaller the setup program registered, then forgets the package.
Result WingetInstaller::remove_setup(StringView id, const SetupRecord& record, String* detail) {
  String record_path(allocator_), windows_program(allocator_), program(allocator_);
  if (!setup_record_path(id, &record_path)) return Result::InvalidName;
  if (equals_folded(record.type.view(), "msi") && !record.package.empty()) {
    // A Windows Installer package: the kept package removes the product. (One that is gone was removed some other way: the product is forgotten.)
    if (exists(files_, record.package.view())) {
      const Result removed = run_msi_removal(record.package.view(), StringView(), detail);
      if (removed != Result::Ok) return removed;
      (void)files_.remove(record.package.view());
    }
    if (files_.remove(record_path.view()) != Status::Ok) {
      detail->assign(record_path.view());
      return Result::FileError;
    }
    return Result::Ok;
  }
  if (record.uninstall.empty()) {
    detail->assign("remove it with its own means, then choose Remove again");
    // (asked a second time, the package is forgotten: there is nothing else this program can do)
    String marker(allocator_);
    if (!join(&marker, record_path.view(), ".asked")) return Result::OutOfMemory;
    if (exists(files_, marker.view())) {
      detail->clear();
      (void)files_.remove(marker.view());
      return files_.remove(record_path.view()) == Status::Ok ? Result::Ok : Result::FileError;
    }
    (void)write_text_file(files_, marker.view(), "asked\n");
    return Result::NoUninstaller;
  }
  if (!setup_) {
    detail->assign("its uninstaller cannot be run here");
    return Result::NotSupported;
  }
  List<String> words(allocator_), more(allocator_);
  if (!split_command_line(allocator_, record.uninstall.view(), &windows_program, &words) ||
      !split_command_line(allocator_, record.quiet ? StringView() : silent_uninstall_switches(record.type.view()), nullptr, &more))
    return Result::OutOfMemory;
  if (!windows_path_to_system(windows_program.view(), &program)) {
    (void)(detail->assign("its uninstaller is not a program on this computer's disk: ") && detail->append(windows_program.view()));
    return Result::SetupFailed;
  }
  if (!exists(files_, program.view())) {      // (it was uninstalled some other way: nothing is left to do but forget it)
    return files_.remove(record_path.view()) == Status::Ok ? Result::Ok : Result::FileError;
  }
  if (!setup_program_runs(program.view(), record.type.view())) {
    detail->assign("its uninstaller is a 32-bit program of a kind that cannot be run here yet (or one for another processor)");
    return Result::NotSupported;
  }
  List<StringView> arguments(allocator_);
  for (const String& word : static_cast<const List<String>&>(words).view())
    if (!arguments.push_back(word.view())) return Result::OutOfMemory;
  for (const String& word : static_cast<const List<String>&>(more).view())
    if (!arguments.push_back(word.view())) return Result::OutOfMemory;
  uint32_t code = 0;
  String message(allocator_);
  if (!setup_->run(program.view(), static_cast<const List<StringView>&>(arguments).view(), nullptr, &code, &message)) {
    (void)(detail->assign("the uninstaller: ") && detail->append(message.view()));
    return Result::SetupFailed;
  }
  if (code != 0) {
    (void)(detail->assign("the uninstaller ended with code ") && detail->append_uint(code));
    return Result::SetupFailed;
  }
  if (files_.remove(record_path.view()) != Status::Ok) {
    detail->assign(record_path.view());
    return Result::FileError;
  }
  return Result::Ok;
}

}  // namespace desktop::appman

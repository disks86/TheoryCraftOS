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

#include "Desktop/InstalledApps.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ByteText.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/Utf16.hpp"

namespace desktop {

using filesystem::DirectoryEntry;
using filesystem::IDirectory;
using filesystem::IFile;
using filesystem::IFileSystem;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

namespace {

constexpr uint64_t kMaxShortcutBytes = 1024 * 1024;   // as shell32 itself reads them
constexpr uint64_t kMaxDesktopEntryBytes = 64 * 1024;
constexpr uint32_t kMaxFolderDepth = 4;               // Start Menu\Programs\<Vendor>\<Product>\... and applications/<vendor>/...

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool starts_with_ignore_case(StringView text, StringView prefix) {
  return prefix.size() <= text.size() && text.first(prefix.size()).equals_ignore_case(prefix);
}
bool ends_with_ignore_case(StringView text, StringView suffix) {
  return suffix.size() <= text.size() && text.substr(text.size() - suffix.size()).equals_ignore_case(suffix);
}

// <0, 0, >0 comparing ASCII letters without regard to case (the order of the Start menu).
int compare_ignore_case(StringView a, StringView b) {
  const uint64_t n = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < n; ++i) {
    const uint8_t x = static_cast<uint8_t>(lower(a[i])), y = static_cast<uint8_t>(lower(b[i]));
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

// Bytes of a Windows "ANSI" string (taken as Latin-1, the first 256 code points of every Windows code page that matters here) as UTF-8.
bool append_latin1(String* out, ConstByteView bytes) {
  for (const uint8_t b : bytes) {
    if (b < 0x80) {
      if (!out->push_back(static_cast<char>(b))) return false;
    } else {
      const char two[2] = {static_cast<char>(0xC0 | (b >> 6)), static_cast<char>(0x80 | (b & 0x3F))};
      if (!out->append(StringView(two, 2))) return false;
    }
  }
  return true;
}

// The bytes before the first NUL (all of them if there is none).
ConstByteView until_nul(ConstByteView bytes) {
  for (uint64_t i = 0; i < bytes.size(); ++i)
    if (bytes[i] == 0) return bytes.first(i);
  return bytes;
}
// The UTF-16LE units before the first NUL unit (an odd last byte is dropped).
ConstByteView until_nul16(ConstByteView bytes) {
  const uint64_t units = bytes.size() / 2;
  for (uint64_t i = 0; i < units; ++i)
    if (bytes[2 * i] == 0 && bytes[2 * i + 1] == 0) return bytes.first(2 * i);
  return bytes.first(units * 2);
}

// Adds the components of `rest` (separated by '/' or '\') to the system path `path`. "." is skipped, ".." takes the last component off;
// false if that would go above the first `floor` characters (the root of the tree the path was given in).
bool append_components(String* path, StringView rest, uint64_t floor) {
  uint64_t i = 0;
  while (i < rest.size()) {
    uint64_t end = i;
    while (end < rest.size() && rest[end] != '/' && rest[end] != '\\') ++end;
    const StringView part = rest.substr(i, end - i);
    i = end + 1;
    if (part.empty() || part.equals(".")) continue;
    if (part.equals("..")) {
      if (path->size() <= floor) return false;
      const uint64_t slash = path->view().rfind('/');
      path->truncate(slash == StringView::npos || slash < floor ? floor : slash);
      continue;
    }
    if (!path->push_back('/') || !path->append(part)) return false;
  }
  return true;
}

// The folder part of a system path ("/a/b/c" -> "/a/b").
StringView parent_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  if (slash == StringView::npos) return StringView();
  return slash == 0 ? StringView("/") : path.first(slash);
}
StringView file_name_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? path : path.substr(slash + 1);
}

// Puts one word on a command line for the shell (Desktop/Shell.cpp: blanks separate, quotes group, no other escapes).
bool append_shell_word(String* line, StringView word) {
  if (!line->empty() && !line->push_back(' ')) return false;
  bool blank = word.empty(), dquote = false, squote = false;
  for (const char c : word) {
    if (c == ' ' || c == '\t') blank = true;
    if (c == '"') dquote = true;
    if (c == '\'') squote = true;
  }
  if (!blank && !dquote && !squote) return line->append(word);
  // A quoted run ends at its own quote character, so a word holding one kind is quoted with the other; a word with both switches kinds
  // where needed ('x'"'y" reads back as x'y).
  char quote = dquote ? '\'' : '"';
  if (!line->push_back(quote)) return false;
  for (const char c : word) {
    if (c == quote) {
      if (!line->push_back(quote)) return false;     // close this run
      quote = quote == '"' ? '\'' : '"';
      if (!line->push_back(quote)) return false;     // and open one of the other kind
    }
    if (!line->push_back(c)) return false;
  }
  return line->push_back(quote);
}

// --- shortcuts ([MS-SHLLINK]) ---------------------------------------------------------------------------------------------------------

constexpr uint32_t kHasIdList = 0x01, kHasLinkInfo = 0x02, kHasName = 0x04, kHasRelativePath = 0x08, kHasWorkingDir = 0x10, kHasArguments = 0x20,
                   kHasIcon = 0x40, kIsUnicode = 0x80;
constexpr uint8_t kShellLinkClsid[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr uint8_t kMyComputerClsid[16] = {0xE0, 0x4F, 0xD0, 0x20, 0xEA, 0x3A, 0x69, 0x10, 0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D};

bool same_bytes(ConstByteView a, const uint8_t (&b)[16]) {
  if (a.size() != 16) return false;
  for (uint64_t i = 0; i < 16; ++i)
    if (a[i] != b[i]) return false;
  return true;
}

// A StringData entry: a 16-bit count of characters, then the characters (UTF-16LE or ANSI).
bool read_link_string(ByteReader& in, bool unicode, String* out) {
  const uint16_t count = in.le16();
  const ConstByteView chars = in.take(uint64_t{count} * (unicode ? 2 : 1));
  if (!in.ok()) return false;
  out->clear();
  return unicode ? util::append_utf8_from_utf16le(out, chars) : append_latin1(out, chars);
}

// The long name of a file item of Windows' own shell: the 0xBEEF0004 extension block after the 8.3 name. False if there is none.
bool beef0004_name(ConstByteView item, uint64_t short_name_at, String* out) {
  const ConstByteView short_name = until_nul(item.subview(short_name_at));
  uint64_t at = short_name_at + short_name.size() + 1;
  if (at & 1) ++at;                       // the block is 2-aligned within the item
  ConstByteView block;
  if (!item.try_subview(at, item.size() > at ? item.size() - at : 0, &block)) return false;
  ByteReader in(block);
  const uint16_t size = in.le16();
  const uint16_t version = in.le16();
  const uint32_t signature = in.le32();
  if (!in.ok() || signature != 0xBEEF0004 || size > block.size() || version < 3) return false;
  const uint64_t name_at = version >= 9 ? 46 : version >= 8 ? 42 : version >= 7 ? 38 : 20;
  ConstByteView name;
  if (!block.first(size).try_subview(name_at, size > name_at ? size - name_at : 0, &name)) return false;
  const ConstByteView units = until_nul16(name);
  if (units.empty()) return false;
  out->clear();
  return util::append_utf8_from_utf16le(out, units);
}

// The name of a file or folder item: this system's shell32 stores it as UTF-16 right after the fixed part, Windows' shell as an 8.3 name
// followed by the long name in an extension block.
bool file_item_name(ConstByteView item, String* out) {
  constexpr uint64_t kNameAt = 14;      // [cb 2][type][pad][size 4][date 2][time 2][attributes 2]
  if (item.size() <= kNameAt) return false;
  if (beef0004_name(item, kNameAt, out)) return true;
  out->clear();
  uint8_t first = 0, second = 1;
  if (item.try_get(kNameAt, &first) && item.try_get(kNameAt + 1, &second) && first != 0 && second == 0)
    return util::append_utf8_from_utf16le(out, until_nul16(item.subview(kNameAt))) && !out->empty();
  return append_latin1(out, until_nul(item.subview(kNameAt))) && !out->empty();
}

// The Windows path an item id list names ("C:\a\b"), for a list of This PC, a drive and file items. False for anything else (other virtual
// folders, network shares, a list relative to the desktop), which is not an error.
bool id_list_path(ConstByteView list, String* out) {
  ByteReader in(list);
  out->clear();
  bool first_item = true, have_drive = false;
  String name(out->allocator());
  while (in.ok() && in.remaining() >= 2) {
    const uint16_t cb = in.le16();
    if (cb == 0) return have_drive;
    if (cb < 3) return false;
    const ConstByteView body = in.take(cb - 2);
    if (!in.ok()) return false;
    const ConstByteView item = list.subview(in.position() - cb, cb);
    const uint8_t type = body[0];
    if (type == 0x1F) {                       // a virtual folder: only This PC leads to drives
      ConstByteView clsid;
      if (!first_item || !body.try_subview(2, 16, &clsid) || !same_bytes(clsid, kMyComputerClsid)) return false;
    } else if ((type & 0x70) == 0x20) {       // a drive: "C:\"
      uint8_t letter = 0, colon = 0;
      if (have_drive || !body.try_get(1, &letter) || !body.try_get(2, &colon) || colon != ':') return false;
      const char text[3] = {static_cast<char>(letter), ':', '\\'};
      if (!out->assign(StringView(text, 3))) return false;
      have_drive = true;
    } else if ((type & 0x70) == 0x30) {       // a folder or a file
      if (!have_drive || !file_item_name(item, &name)) return false;
      if ((!out->view().ends_with('\\') && !out->push_back('\\')) || !out->append(name.view())) return false;
    } else {
      return false;
    }
    first_item = false;
  }
  return false;   // no terminating 0 inside the list
}

// --- desktop entries --------------------------------------------------------------------------------------------------------------------

// A value with its escapes (\s \n \t \r \\) turned into what they stand for.
bool unescape_value(StringView value, String* out) {
  out->clear();
  for (uint64_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c == '\\' && i + 1 < value.size()) {
      const char e = value[++i];
      c = e == 's' ? ' ' : e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
    }
    if (!out->push_back(c)) return false;
  }
  return true;
}

bool is_true(StringView value) { return value.equals("true") || value.equals("1"); }

StringView trim(StringView text) {
  uint64_t a = 0, b = text.size();
  while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) ++a;
  while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) --b;
  return text.substr(a, b - a);
}

// Splits an Exec value into words (blanks separate; inside double quotes a backslash makes the next character literal), drops the field codes
// and "%%" becomes "%". The first word goes to `program`, the others to `arguments` quoted for the shell. False on an unterminated quote.
bool split_exec(StringView exec, String* program, String* arguments) {
  program->clear();
  arguments->clear();
  String word(program->allocator());
  bool have_program = false;
  uint64_t i = 0;
  while (i < exec.size()) {
    while (i < exec.size() && (exec[i] == ' ' || exec[i] == '\t')) ++i;
    if (i >= exec.size()) break;
    word.clear();
    bool quoted = false, only_codes = true, any = false;
    for (; i < exec.size(); ++i) {
      const char c = exec[i];
      if (quoted) {
        if (c == '"') { quoted = false; continue; }
        if (c == '\\' && i + 1 < exec.size()) {
          if (!word.push_back(exec[++i])) return false;
        } else if (!word.push_back(c)) {
          return false;
        }
        only_codes = false;
        any = true;
        continue;
      }
      if (c == ' ' || c == '\t') break;
      if (c == '"') { quoted = true; any = true; only_codes = false; continue; }
      if (c == '%' && i + 1 < exec.size()) {
        const char code = exec[++i];
        any = true;
        if (code == '%') {
          if (!word.push_back('%')) return false;
          only_codes = false;
        }
        continue;                                // %f %F %u %U %i %c %k ... : nothing is opened from the menu
      }
      if (!word.push_back(c)) return false;
      only_codes = false;
      any = true;
    }
    if (quoted) return false;
    if (!any || (only_codes && word.empty())) continue;
    if (!have_program) {
      if (!program->assign(word.view())) return false;
      have_program = true;
    } else if (!append_shell_word(arguments, word.view())) {
      return false;
    }
  }
  return true;
}

// --- finding them ----------------------------------------------------------------------------------------------------------------------

bool read_small_file(IFileSystem& fs, StringView path, uint64_t limit, ByteBuffer* out) {
  DirectoryEntry entry;
  if (fs.stat(path, &entry) != Status::Ok || entry.type != NodeType::File || entry.size == 0 || entry.size > limit) return false;
  UniquePointer<IFile> file;
  if (fs.open_file(path, OpenMode::Read, &file) != Status::Ok) return false;
  out->clear();
  uint8_t chunk[4096];
  for (uint64_t at = 0; at < entry.size;) {
    uint64_t got = 0;
    if (file->read(at, ByteView(chunk, sizeof(chunk)), &got) != Status::Ok || got == 0) break;
    if (got > entry.size - at) got = entry.size - at;
    if (!out->append(ConstByteView(chunk, got))) return false;
    at += got;
  }
  return out->size() == entry.size;
}

bool is_program_file(IFileSystem& fs, StringView path) {
  DirectoryEntry entry;
  return fs.stat(path, &entry) == Status::Ok && entry.type == NodeType::File && exe::identify_executable_file(fs, path) != exe::ExecutableKind::None;
}

struct Finder {
  IFileSystem& fs;
  allocators::IAllocator& allocator;
  List<InstalledApp>* out;
  bool memory_ok = true;

  // Adds `app` (label made here) in label order, unless an entry of that label is there already.
  void add(InstalledApp&& app) {
    if (!app.label.assign(app.name.view()) || !app.label.append(" (") || !app.label.append(app_platform_name(app.platform)) || !app.label.push_back(')')) {
      memory_ok = false;
      return;
    }
    uint64_t at = 0;
    for (; at < out->count(); ++at) {
      const int order = compare_ignore_case(app.label.view(), (*out)[at].label.view());
      if (order == 0 && (*out)[at].label.equals(app.label.view())) return;
      if (order < 0) break;
    }
    if (!out->insert(at, static_cast<InstalledApp&&>(app))) memory_ok = false;
  }

  // Calls visit(path, name) for every file in `folder` and its sub-folders (down to kMaxFolderDepth).
  template <typename Visit>
  void walk(StringView folder, uint32_t depth, Visit&& visit) {
    UniquePointer<IDirectory> dir;
    if (fs.open_directory(folder, &dir) != Status::Ok) return;
    DirectoryEntry entry;
    String path(allocator);
    while (memory_ok && dir->next(&entry) == Status::Ok) {
      const StringView name = entry.name.view();
      if (name.empty() || name.equals(".") || name.equals("..")) continue;
      if (!path.assign(folder) || !path.push_back('/') || !path.append(name)) { memory_ok = false; return; }
      if (entry.type == NodeType::Directory) {
        if (depth + 1 < kMaxFolderDepth) walk(path.view(), depth + 1, visit);
      } else {
        visit(path.view(), name);
      }
    }
  }

  void windows_shortcut(StringView path, StringView file_name) {
    if (!ends_with_ignore_case(file_name, ".lnk")) return;
    const StringView name = file_name.first(file_name.size() - 4);
    if (name.empty() || starts_with_ignore_case(name, "Uninstall")) return;
    ByteBuffer data(allocator);
    if (!read_small_file(fs, path, kMaxShortcutBytes, &data)) return;
    ShellLinkInfo link(allocator);
    if (!parse_shell_link(data.view(), &link)) return;
    InstalledApp app(allocator);
    app.platform = AppPlatform::Windows;
    bool found = !link.target.empty() && windows_path_to_system(link.target.view(), &app.program);
    if (!found && !link.relative_path.empty()) {   // relative to the folder the shortcut is in
      found = app.program.assign(parent_of(path)) && append_components(&app.program, link.relative_path.view(), StringView(filesystem::layout::kWindows).size());
    }
    if (!found || starts_with_ignore_case(file_name_of(app.program.view()), "unins") || !is_program_file(fs, app.program.view())) return;
    if (link.working_directory.empty() || !windows_path_to_system(link.working_directory.view(), &app.working_directory)) {
      if (!app.working_directory.assign(parent_of(app.program.view()))) { memory_ok = false; return; }
    }
    if (!app.name.assign(name) || !app.arguments.assign(link.arguments.view())) { memory_ok = false; return; }
    add(static_cast<InstalledApp&&>(app));
  }

  // A program named in a desktop entry: an absolute Linux path, or a bare name looked up in the usual PATH folders.
  bool resolve_linux_program(StringView program, String* out) {
    if (program.empty()) return false;
    if (program.contains('/')) return program[0] == '/' && linux_path_to_system(program, out) && is_program_file(fs, out->view());
    static constexpr const char* kPath[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin"};
    for (const char* folder : kPath) {
      if (!linux_path_to_system(folder, out) || !out->push_back('/') || !out->append(program)) return false;
      if (is_program_file(fs, out->view())) return true;
    }
    return false;
  }

  void linux_entry(StringView path, StringView file_name) {
    if (!ends_with_ignore_case(file_name, ".desktop")) return;
    ByteBuffer data(allocator);
    if (!read_small_file(fs, path, kMaxDesktopEntryBytes, &data)) return;
    const ConstByteView bytes = data.view();
    DesktopEntryInfo entry(allocator);
    if (!parse_desktop_entry(bytes_as_text(bytes), &entry)) return;
    if (!entry.application || entry.hidden || entry.name.empty()) return;
    InstalledApp app(allocator);
    app.platform = AppPlatform::Linux;
    if (!entry.try_exec.empty() && !resolve_linux_program(entry.try_exec.view(), &app.program)) return;
    if (!resolve_linux_program(entry.program.view(), &app.program)) return;
    if (entry.path.empty() || !linux_path_to_system(entry.path.view(), &app.working_directory)) {
      if (!app.working_directory.assign(filesystem::layout::kLinuxRootHome)) { memory_ok = false; return; }
    }
    if (!app.name.assign(entry.name.view()) || !app.arguments.assign(entry.arguments.view())) { memory_ok = false; return; }
    add(static_cast<InstalledApp&&>(app));
  }
};

}  // namespace

StringView app_platform_name(AppPlatform platform) { return platform == AppPlatform::Windows ? StringView("Windows") : StringView("Linux"); }

bool installed_app_command_line(const InstalledApp& app, String* out) {
  out->clear();
  if (!append_shell_word(out, app.program.view())) return false;
  return app.arguments.empty() || (out->push_back(' ') && out->append(app.arguments.view()));
}

bool same_installed_app(const InstalledApp& a, const InstalledApp& b) {
  return a.platform == b.platform && a.label.equals(b.label.view()) && a.program.equals(b.program.view()) && a.arguments.equals(b.arguments.view()) &&
         a.working_directory.equals(b.working_directory.view());
}

bool parse_shell_link(ConstByteView data, ShellLinkInfo* out) {
  out->target.clear();
  out->relative_path.clear();
  out->working_directory.clear();
  out->arguments.clear();
  ByteReader in(data);
  if (in.le32() != 0x4C || !same_bytes(in.take(16), kShellLinkClsid)) return false;
  const uint32_t flags = in.le32();
  in.skip(4 + 24 + 4 + 4 + 4 + 2 + 10);     // attributes, three times, size, icon index, show command, hot key, reserved
  if (!in.ok()) return false;
  String from_list(out->target.allocator());
  bool have_list_path = false;
  if (flags & kHasIdList) {
    const uint16_t size = in.le16();
    const ConstByteView list = in.take(size);
    if (!in.ok()) return false;
    have_list_path = id_list_path(list, &from_list);
  }
  if (flags & kHasLinkInfo) {
    const uint64_t start = in.position();
    const uint32_t size = in.le32();
    if (!in.ok() || size < 28 || size > data.size() - start) return false;
    const ConstByteView info = data.subview(start, size);
    ByteReader header(info);
    header.skip(4);
    const uint32_t header_size = header.le32();
    const uint32_t info_flags = header.le32();
    header.skip(4);                          // the volume id
    const uint32_t base_at = header.le32();
    header.skip(4);                          // the network location
    const uint32_t suffix_at = header.le32();
    uint32_t base_at_unicode = 0, suffix_at_unicode = 0;
    if (header_size >= 0x24) {
      base_at_unicode = header.le32();
      suffix_at_unicode = header.le32();
    }
    if (!header.ok()) return false;
    if (info_flags & 1) {                    // VolumeIDAndLocalBasePath
      String& t = out->target;
      if (base_at_unicode && base_at_unicode < size) {
        if (!util::append_utf8_from_utf16le(&t, until_nul16(info.subview(base_at_unicode)))) return false;
        if (suffix_at_unicode && suffix_at_unicode < size && !util::append_utf8_from_utf16le(&t, until_nul16(info.subview(suffix_at_unicode)))) return false;
      } else if (base_at && base_at < size) {
        if (!append_latin1(&t, until_nul(info.subview(base_at)))) return false;
        if (suffix_at && suffix_at < size && !append_latin1(&t, until_nul(info.subview(suffix_at)))) return false;
      }
    }
    in.seek(start + size);
  }
  if (out->target.empty() && have_list_path && !out->target.assign(from_list.view())) return false;
  const bool unicode = (flags & kIsUnicode) != 0;
  String ignored(out->target.allocator());
  if ((flags & kHasName) && !read_link_string(in, unicode, &ignored)) return false;
  if ((flags & kHasRelativePath) && !read_link_string(in, unicode, &out->relative_path)) return false;
  if ((flags & kHasWorkingDir) && !read_link_string(in, unicode, &out->working_directory)) return false;
  if ((flags & kHasArguments) && !read_link_string(in, unicode, &out->arguments)) return false;
  if ((flags & kHasIcon) && !read_link_string(in, unicode, &ignored)) return false;
  return in.ok();
}

bool parse_desktop_entry(StringView text, DesktopEntryInfo* out) {
  out->name.clear();
  out->program.clear();
  out->arguments.clear();
  out->try_exec.clear();
  out->path.clear();
  out->application = false;
  out->hidden = false;
  bool in_group = false, found_group = false;
  String value(out->name.allocator()), exec(out->name.allocator());
  bool have_exec = false;
  uint64_t i = 0;
  while (i < text.size()) {
    uint64_t end = text.find('\n', i);
    if (end == StringView::npos) end = text.size();
    const StringView line = trim(text.substr(i, end - i));
    i = end + 1;
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in_group = line.equals("[Desktop Entry]");
      if (in_group) {
        if (found_group) return false;      // the group may appear only once
        found_group = true;
      }
      continue;
    }
    if (!in_group) continue;
    const uint64_t equals = line.find('=');
    if (equals == StringView::npos) continue;
    const StringView key = trim(line.first(equals));
    if (!unescape_value(trim(line.substr(equals + 1)), &value)) return false;
    if (key.equals("Type")) out->application = value.equals("Application");
    else if (key.equals("Name")) { if (!out->name.assign(value.view())) return false; }
    else if (key.equals("Exec")) { if (!exec.assign(value.view())) return false; have_exec = true; }
    else if (key.equals("TryExec")) { if (!out->try_exec.assign(value.view())) return false; }
    else if (key.equals("Path")) { if (!out->path.assign(value.view())) return false; }
    else if (key.equals("NoDisplay") || key.equals("Hidden")) { if (is_true(value.view())) out->hidden = true; }
  }
  if (!found_group) return false;
  return !have_exec || split_exec(exec.view(), &out->program, &out->arguments);
}

bool windows_path_to_system(StringView windows_path, String* out) {
  StringView path = trim(windows_path);
  if (path.size() >= 2 && path[0] == '"' && path[path.size() - 1] == '"') path = path.substr(1, path.size() - 2);
  if (path.starts_with(StringView("\\\\?\\"))) path = path.substr(4);
  // An environment variable at the start, for the folders every installation has.
  static constexpr struct { const char* variable; const char* value; } kVariables[] = {
      {"%ProgramFiles%", "C:\\Program Files"}, {"%ProgramW6432%", "C:\\Program Files"}, {"%ProgramFiles(x86)%", "C:\\Program Files (x86)"},
      {"%CommonProgramFiles%", "C:\\Program Files\\Common Files"}, {"%SystemRoot%", "C:\\Windows"}, {"%windir%", "C:\\Windows"},
      {"%ProgramData%", "C:\\ProgramData"}, {"%ALLUSERSPROFILE%", "C:\\ProgramData"}, {"%PUBLIC%", "C:\\Users\\Public"}, {"%SystemDrive%", "C:"},
  };
  StringView rest;
  if (path.starts_with('%')) {
    bool known = false;
    for (const auto& v : kVariables) {
      if (starts_with_ignore_case(path, v.variable)) {
        if (!out->assign(filesystem::layout::kWindows)) return false;
        const StringView value(v.value);
        if (!append_components(out, value.substr(2), out->size())) return false;
        rest = path.substr(StringView(v.variable).size());
        if (!rest.empty() && rest[0] != '\\' && rest[0] != '/') return false;
        known = true;
        break;
      }
    }
    if (!known) return false;
  } else {
    if (path.size() < 3 || lower(path[0]) != 'c' || path[1] != ':' || (path[2] != '\\' && path[2] != '/')) return false;
    if (!out->assign(filesystem::layout::kWindows)) return false;
    rest = path.substr(3);
  }
  return append_components(out, rest, StringView(filesystem::layout::kWindows).size());
}

bool linux_path_to_system(StringView linux_path, String* out) {
  if (linux_path.empty() || linux_path[0] != '/') return false;
  if (!out->assign(filesystem::layout::kLinux)) return false;
  return append_components(out, linux_path.substr(1), out->size());
}

bool find_installed_apps(IFileSystem& fs, allocators::IAllocator& allocator, List<InstalledApp>* out) {
  namespace layout = filesystem::layout;
  Finder finder{fs, allocator, out};
  // Windows: every user's own shortcuts, then the ones for all users.
  {
    UniquePointer<IDirectory> users;
    if (fs.open_directory(layout::kWindowsUsers, &users) == Status::Ok) {
      DirectoryEntry entry;
      String folder(allocator);
      while (finder.memory_ok && users->next(&entry) == Status::Ok) {
        if (entry.type != NodeType::Directory || entry.name.view().empty() || entry.name.view()[0] == '.') continue;
        if (!folder.assign(layout::kWindowsUsers) || !folder.push_back('/') || !folder.append(entry.name.view()) || !folder.push_back('/') ||
            !folder.append(layout::kWindowsUserStartMenuPrograms)) {
          finder.memory_ok = false;
          break;
        }
        finder.walk(folder.view(), 0, [&](StringView path, StringView name) { finder.windows_shortcut(path, name); });
      }
    }
  }
  finder.walk(layout::kWindowsStartMenuPrograms, 0, [&](StringView path, StringView name) { finder.windows_shortcut(path, name); });
  // Linux: the user's entries override the local ones, which override the distribution's (the first of a label is kept).
  static constexpr const char* kLinuxFolders[] = {layout::kLinuxRootApplications, layout::kLinuxLocalApplications, layout::kLinuxApplications};
  for (const char* folder : kLinuxFolders)
    finder.walk(folder, 0, [&](StringView path, StringView name) { finder.linux_entry(path, name); });
  return finder.memory_ok;
}

}  // namespace desktop

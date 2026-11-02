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
#include "Process/ObjectPath.hpp"

namespace process::objpath {

namespace {

constexpr StringView kBaseNamed = "BaseNamedObjects";

bool all_digits(StringView s) {
  if (s.empty()) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

bool copy_out(StringView text, char* out, uint32_t capacity, uint32_t* size) {
  if (text.size() >= capacity) return false;
  for (uint64_t i = 0; i < text.size(); ++i) out[i] = text[i];
  out[text.size()] = 0;
  *size = uint32_t(text.size());
  return true;
}

// The places that are always there: directories, and links with their targets.
struct Builtin { const char* path; const char* target; };      // target null: a directory
const Builtin kBuiltins[] = {
    {"\\", nullptr},
    {"\\BaseNamedObjects", nullptr},
    {"\\BaseNamedObjects\\Session", nullptr},
    {"\\BaseNamedObjects\\Local", "\\BaseNamedObjects"},
    {"\\BaseNamedObjects\\Global", "\\BaseNamedObjects"},
    {"\\Sessions", nullptr},
    {"\\Sessions\\1", nullptr},
    {"\\Sessions\\1\\BaseNamedObjects", nullptr},
    {"\\??", nullptr},
    {"\\??\\GLOBALROOT", ""},      // (its target is the empty string: the root of the namespace)
    {"\\DosDevices", "\\??"},
    {"\\Device", nullptr},
    {"\\Driver", nullptr},
    {"\\FileSystem", nullptr},
    {"\\KnownDlls", nullptr},
    {"\\ObjectTypes", nullptr},
    {"\\Callback", nullptr},
    {"\\Security", nullptr},
    {"\\Windows", nullptr},
    {"\\RPC Control", nullptr},
    {"\\NLS", nullptr},
};

void fill_text(Entry* e, StringView text) {
  e->text_size = 0;
  copy_out(text, e->text, kMaxPath, &e->text_size);
}

// The built-in entry at a path (the spelling in the table is the right one; the last folder of `path` is put right if `fix` is set).
bool find_builtin(Path* path, bool case_insensitive, Entry* out) {
  char text[kMaxPath];
  uint32_t size = 0;
  if (!path->format(text, sizeof(text), &size)) return false;
  const StringView wanted(text, size);
  for (const Builtin& b : kBuiltins) {
    const StringView name(b.path);
    if (!(case_insensitive ? name.equals_ignore_case(wanted) : name == wanted)) continue;
    *out = Entry();
    out->builtin = true;
    if (b.target) { out->kind = EntryKind::Link; fill_text(out, b.target); }
    else { out->kind = EntryKind::Directory; fill_text(out, name); }
    if (case_insensitive && path->count() != 0) {      // (the spelling of the table is the one the names are kept in)
      const uint64_t at = name.rfind('\\');
      path->replace_last(name.substr(at + 1));
    }
    return true;
  }
  return false;
}

bool find_entry(ILookup& lookup, Path* path, bool case_insensitive, Entry* out) {
  *out = Entry();
  if (find_builtin(path, case_insensitive, out)) return true;
  char flat[kMaxPath];
  uint32_t size = 0;
  if (!path->to_flat(flat, sizeof(flat), &size) || size == 0) return false;
  if (!lookup.find(StringView(flat, size), case_insensitive, out)) return false;
  return true;
}

}  // namespace

bool Path::push(StringView part) {
  if (part.empty() || count_ >= kMaxParts || part.size() > kMaxPath - used_) return false;
  begin_[count_] = uint16_t(used_);
  length_[count_] = uint16_t(part.size());
  for (uint64_t i = 0; i < part.size(); ++i) text_[used_ + i] = part[i];
  used_ += uint32_t(part.size());
  ++count_;
  return true;
}

bool Path::replace_last(StringView part) {
  if (count_ == 0 || part.empty()) return false;
  const uint32_t start = begin_[count_ - 1];
  if (part.size() > kMaxPath - start) return false;
  for (uint64_t i = 0; i < part.size(); ++i) text_[start + i] = part[i];
  length_[count_ - 1] = uint16_t(part.size());
  used_ = start + uint32_t(part.size());
  return true;
}

bool Path::parse(StringView text) {
  count_ = 0;
  used_ = 0;
  if (!text.starts_with('\\')) return false;
  text = text.without_prefix(1);
  while (!text.empty()) {
    const uint64_t at = text.find('\\');
    const StringView part = at == StringView::npos ? text : text.first(at);
    if (!push(part)) return false;
    text = at == StringView::npos ? StringView() : text.without_prefix(at + 1);
    if (at != StringView::npos && text.empty()) return false;      // (a trailing backslash)
  }
  return true;
}

bool Path::format(char* out, uint32_t capacity, uint32_t* size) const {
  uint32_t n = 0;
  if (count_ == 0) {
    if (capacity < 2) return false;
    out[n++] = '\\';
  }
  for (uint32_t i = 0; i < count_; ++i) {
    if (uint64_t{n} + 1 + length_[i] + 1 > capacity) return false;
    out[n++] = '\\';
    for (uint32_t k = 0; k < length_[i]; ++k) out[n++] = text_[begin_[i] + k];
  }
  out[n] = 0;
  *size = n;
  return true;
}

bool Path::to_flat(char* out, uint32_t capacity, uint32_t* size) const {
  uint32_t n = 0, first = 0;
  auto put = [&](char c) { if (n + 1 >= capacity) return false; out[n++] = c; return true; };
  if (count_ == 0) {
    if (!put('\\')) return false;
  } else if (part(0).equals_ignore_case(kBaseNamed)) {
    first = 1;
  } else if (count_ >= 3 && part(0).equals_ignore_case("Sessions") && all_digits(part(1)) && part(2).equals_ignore_case(kBaseNamed)) {
    if (!put(kSessionMark)) return false;
    first = 3;
  } else {
    if (!put('\\')) return false;
  }
  for (uint32_t i = first; i < count_; ++i) {
    if (i != first && !put('\\')) return false;
    for (uint32_t k = 0; k < length_[i]; ++k) if (!put(text_[begin_[i] + k])) return false;
  }
  out[n] = 0;
  *size = n;
  return true;
}

void Path::apply_alias() {
  if (count_ < 3 || !part(0).equals_ignore_case(kBaseNamed) || !part(1).equals_ignore_case("Session") || !all_digits(part(2))) return;
  Path rewritten;
  rewritten.push("Sessions");
  rewritten.push(part(2));
  rewritten.push(kBaseNamed);
  for (uint32_t i = 3; i < count_; ++i) rewritten.push(part(i));
  *this = rewritten;
}

bool flat_to_path(StringView flat, Path* out) {
  *out = Path();
  StringView rest = flat;
  if (flat.empty() || flat[0] != '\\') {
    if (flat.starts_with(kSessionMark)) {
      rest = flat.without_prefix(1);
      if (!out->push("Sessions") || !out->push("1") || !out->push(kBaseNamed)) return false;
    } else if (!out->push(kBaseNamed)) {
      return false;
    }
    while (!rest.empty()) {
      const uint64_t at = rest.find('\\');
      if (!out->push(at == StringView::npos ? rest : rest.first(at))) return false;
      rest = at == StringView::npos ? StringView() : rest.without_prefix(at + 1);
    }
    return true;
  }
  return out->parse(flat);
}

void split_flat(StringView flat, StringView* directory, StringView* leaf) {
  const uint64_t at = flat.rfind('\\');
  if (flat.starts_with('\\')) {
    if (at == 0) { *directory = flat.first(1); *leaf = flat.without_prefix(1); }
    else { *directory = flat.first(at); *leaf = flat.without_prefix(at + 1); }
    return;
  }
  const uint64_t skip = flat.starts_with(kSessionMark) ? 1 : 0;
  if (at == StringView::npos) { *directory = flat.first(skip); *leaf = flat.without_prefix(skip); return; }
  *directory = flat.first(at);
  *leaf = flat.without_prefix(at + 1);
}

void resolve(ILookup& lookup, const ResolveRequest& rq, Resolved* r) {
  *r = Resolved();
  Path cur;
  char buffers[2][2 * kMaxPath + 2];      // the text still to walk after a link was followed (two, so that the second link does not overwrite what is being read)
  uint32_t which = 0;
  StringView remaining;
  if (rq.has_root) {
    if (!cur.parse(rq.root_path)) { r->status = kStatusPathNotFound; return; }
    if (!rq.name_given) { r->status = kStatusNameInvalid; return; }
    if (rq.name.starts_with('\\')) { r->status = kStatusPathSyntaxBad; return; }
    remaining = rq.name;
    r->is_root = rq.name.empty();
  } else {
    if (!rq.name_given || !rq.name.starts_with('\\')) { r->status = kStatusPathSyntaxBad; return; }
    remaining = rq.name.without_prefix(1);
  }
  bool have_more = !remaining.empty(), finished = false;
  uint32_t follows = 0;
  Entry e;
  while (have_more) {
    StringView token;
    const uint64_t at = remaining.find('\\');
    if (at == StringView::npos) { token = remaining; remaining = StringView(); have_more = false; }
    else { token = remaining.first(at); remaining = remaining.without_prefix(at + 1); }
    if (token.empty()) { r->status = kStatusNameInvalid; return; }
    const bool last = !have_more;
    Path next = cur;
    if (!next.push(token)) { r->status = kStatusNameTooLong; return; }
    next.apply_alias();
    find_entry(lookup, &next, rq.case_insensitive, &e);
    switch (e.kind) {
      case EntryKind::None:
        if (!last) { r->status = kStatusPathNotFound; return; }
        cur = next;
        finished = true;
        r->entry = e;
        break;
      case EntryKind::Directory:
        cur = next;
        if (last) { finished = true; r->entry = e; }
        break;
      case EntryKind::Link: {
        if (last && rq.open_link) { cur = next; finished = true; r->entry = e; break; }
        if (++follows > 16) { r->status = kStatusPathNotFound; return; }
        const StringView target(e.text, e.text_size);
        if (!target.empty() && !target.starts_with('\\')) { r->status = kStatusPathNotFound; return; }
        const StringView tail = target.without_prefix(1);
        char* buffer = buffers[which ^= 1];
        uint64_t n = 0;
        if (tail.size() + remaining.size() + 2 > sizeof(buffers[0])) { r->status = kStatusNameTooLong; return; }
        for (char c : tail) buffer[n++] = c;
        if (have_more) {      // (the rest of the path follows the target, even if it is only the backslash at its end)
          if (!tail.empty()) buffer[n++] = '\\';
          for (char c : remaining) buffer[n++] = c;
        }
        remaining = StringView(buffer, n);
        have_more = !remaining.empty();
        cur = Path();
        break;
      }
      case EntryKind::Object:
        if (!last) { r->status = kStatusTypeMismatch; return; }
        cur = next;
        finished = true;
        r->entry = e;
        break;
    }
    if (finished) break;
  }
  if (!finished) find_entry(lookup, &cur, rq.case_insensitive, &r->entry);      // (no name, or a link to a directory)
  cur.format(r->path, sizeof(r->path), &r->path_size);
  cur.to_flat(r->flat, sizeof(r->flat), &r->flat_size);
  if (r->entry.kind != EntryKind::None && !r->entry.builtin && r->entry.flat_size != 0) {      // (found in another case: the name it has)
    for (uint32_t i = 0; i < r->entry.flat_size; ++i) r->flat[i] = r->entry.flat[i];
    r->flat[r->entry.flat_size] = 0;
    r->flat_size = r->entry.flat_size;
  }
}

}  // namespace process::objpath

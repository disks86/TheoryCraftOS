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

#include "Program/ShellTools.hpp"
#include "ObjectContainer/List.hpp"
#include "Regex/Regex.hpp"
#include "Utility/Move.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop::shell_tools {

namespace {

constexpr uint64_t npos = StringView::npos;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool parse_count(StringView text, int64_t* out) {
  bool negative = false;
  if (text.starts_with('-') || text.starts_with('+')) {
    negative = text[0] == '-';
    text = text.substr(1);
  }
  uint64_t value = 0;
  if (!text.parse_uint(&value) || value > (uint64_t{1} << 40)) return false;
  *out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
  return true;
}

// The lines of `text` (a last line without its newline is a line too).
bool split_lines(StringView text, List<StringView>* lines) {
  uint64_t start = 0;
  while (start < text.size()) {
    uint64_t end = text.find('\n', start);
    if (end == npos) end = text.size();
    if (!lines->push_back(text.substr(start, end - start))) return false;
    start = end + 1;
  }
  return true;
}

bool put_line(String* out, StringView line) { return out->append(line) && out->push_back('\n'); }

bool append_escaped(String* out, char c) {
  const StringView special = "\\^$.*+?()[]{}|/";
  if (special.contains(c) && !out->push_back('\\')) return false;
  return out->push_back(c);
}

// [:name:] as the inside of an ECMAScript class.
StringView class_ranges(StringView name) {
  if (name.equals("alpha")) return "a-zA-Z";
  if (name.equals("digit")) return "0-9";
  if (name.equals("alnum")) return "a-zA-Z0-9";
  if (name.equals("upper")) return "A-Z";
  if (name.equals("lower")) return "a-z";
  if (name.equals("space")) return " \\t\\n\\r\\f\\v";
  if (name.equals("blank")) return " \\t";
  if (name.equals("xdigit")) return "0-9A-Fa-f";
  if (name.equals("punct")) return "!-\\/:-@\\[-`{-~";
  if (name.equals("print")) return " -~";
  if (name.equals("graph")) return "!-~";
  if (name.equals("cntrl")) return "\\x00-\\x1f\\x7f";
  return StringView();
}

// A compiled pattern of a tool.
struct Pattern {
  explicit Pattern(allocators::IAllocator& allocator) : re(allocator) {}
  regex::Regex re;
  bool compile(allocators::IAllocator& allocator, StringView posix, bool extended, bool ignore_case, bool fixed = false, bool whole_line = false, bool word = false) {
    String body(allocator), full(allocator);
    if (fixed) {
      for (char c : posix)
        if (!append_escaped(&body, c)) return false;
    } else if (!translate_regex(posix, extended, &body)) {
      return false;
    }
    if (!full.assign(whole_line ? StringView("^(?:") : word ? StringView("\\b(?:") : StringView("(?:")) || !full.append(body.view()) ||
        !full.append(whole_line ? StringView(")$") : word ? StringView(")\\b") : StringView(")")))
      return false;
    return re.compile(full.view(), ignore_case ? regex::Flags::IgnoreCase : regex::Flags::None) == regex::Error::None;
  }
  bool search(StringView text, regex::Match* m, uint64_t from = 0) const { return re.search(text, m, from) == regex::Result::Matched; }
};

// ---------------------------------------------------------------- grep
int grep(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  bool only = false, invert = false, ignore = false, extended = false, fixed = false, count = false, quiet = false, whole = false, word = false;
  int64_t max = -1;
  List<StringView> patterns(allocator);
  bool have_pattern = false;
  for (uint64_t i = 0; i < args.size(); ++i) {
    const StringView a = args[i];
    if (a.equals("-e") || a.equals("--regexp")) {
      if (i + 1 >= args.size()) return 2;
      if (!patterns.push_back(args[++i])) return 2;
      have_pattern = true;
      continue;
    }
    if (a.starts_with("--regexp=")) { if (!patterns.push_back(a.substr(9))) return 2; have_pattern = true; continue; }
    if (a.equals("-m") || a.equals("--max-count")) {
      if (i + 1 >= args.size() || !parse_count(args[++i], &max)) return 2;
      continue;
    }
    if (a.starts_with("--max-count=")) { if (!parse_count(a.substr(12), &max)) return 2; continue; }
    if (a.equals("--only-matching")) { only = true; continue; }
    if (a.equals("--invert-match")) { invert = true; continue; }
    if (a.equals("--ignore-case")) { ignore = true; continue; }
    if (a.equals("--extended-regexp")) { extended = true; continue; }
    if (a.starts_with("-") && a.size() > 1 && !a.starts_with("--")) {
      for (uint64_t k = 1; k < a.size(); ++k) {
        switch (a[k]) {
          case 'o': only = true; break;
          case 'v': invert = true; break;
          case 'i': ignore = true; break;
          case 'E': extended = true; break;
          case 'F': fixed = true; break;
          case 'G': extended = false; break;
          case 'P': extended = true; break;
          case 'c': count = true; break;
          case 'q': quiet = true; break;
          case 'x': whole = true; break;
          case 'w': word = true; break;
          case 's': case 'h': case 'a': case 'I': break;
          case 'm': {
            if (!parse_count(a.substr(k + 1), &max)) {
              if (k + 1 < a.size() || i + 1 >= args.size() || !parse_count(args[++i], &max)) return 2;
            }
            k = a.size();
            break;
          }
          case 'e':
            if (k + 1 < a.size()) { if (!patterns.push_back(a.substr(k + 1))) return 2; }
            else if (i + 1 < args.size()) { if (!patterns.push_back(args[++i])) return 2; }
            else return 2;
            have_pattern = true;
            k = a.size();
            break;
          default: return 2;
        }
      }
      continue;
    }
    if (!have_pattern) {
      if (!patterns.push_back(a)) return 2;
      have_pattern = true;
    }
    // (file operands: this grep only reads its input)
  }
  if (!have_pattern) return 2;
  // One pattern of the alternatives; a pattern with newlines is several (as grep reads them).
  List<UniquePointer<Pattern>> compiled(allocator);
  for (const StringView& p : patterns.view()) {
    List<StringView> parts(allocator);
    if (p.empty() ? !parts.push_back(p) : !split_lines(p, &parts)) return 2;
    for (const StringView part : parts.view()) {
      UniquePointer<Pattern> pattern = UniquePointer<Pattern>::make(allocator, allocator);
      if (!pattern || !pattern->compile(allocator, part, extended, ignore, fixed, whole, word) || !compiled.push_back(util::move(pattern))) return 2;
    }
  }
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  int64_t selected = 0;
  for (const StringView line : lines.view()) {
    if (max >= 0 && selected >= max) break;
    bool matched = false;
    regex::Match m;
    for (const UniquePointer<Pattern>& p : compiled.view())
      if (p->search(line, &m)) { matched = true; break; }
    if (matched == invert) continue;
    ++selected;
    if (quiet || count) continue;
    if (only && !invert) {
      // Every match of any pattern, left to right.
      for (uint64_t from = 0; from <= line.size();) {
        uint64_t best_begin = npos, best_end = 0;
        for (const UniquePointer<Pattern>& p : compiled.view()) {
          regex::Match k;
          if (p->search(line, &k, from) && (k.begin() < best_begin || (k.begin() == best_begin && k.end() > best_end))) {
            best_begin = k.begin();
            best_end = k.end();
          }
        }
        if (best_begin == npos) break;
        if (best_end > best_begin && !put_line(out, line.substr(best_begin, best_end - best_begin))) return 2;
        from = best_end > best_begin ? best_end : best_begin + 1;
      }
    } else if (!put_line(out, line)) {
      return 2;
    }
  }
  if (count && !quiet && (!out->append_uint(uint64_t(selected)) || !out->push_back('\n'))) return 2;
  return selected ? 0 : 1;
}

// ---------------------------------------------------------------- sed
struct SedCommand {
  explicit SedCommand(allocators::IAllocator& allocator) : pattern(allocator), address(allocator), replacement(allocator), from(allocator), to(allocator) {}
  char op = 0;                 // s d p q y
  Pattern pattern;             // s's pattern
  // Address: none, a line number, the last line, or a pattern.
  enum class Where : uint8_t { All, Line, Last, Pattern } where = Where::All;
  int64_t line = 0;
  Pattern address;
  bool negate = false;
  String replacement, from, to;   // s's replacement; y's two sets
  bool global = false, print = false;
  int64_t occurrence = 1;
};

// Reads up to the unescaped `delimiter` from script[*i]; "\<delimiter>" becomes the delimiter itself, other escapes stay. *i ends past the delimiter.
bool sed_part(StringView script, uint64_t* i, char delimiter, String* out, bool regex_part) {
  out->clear();
  while (*i < script.size()) {
    const char c = script[*i];
    if (c == '\\' && *i + 1 < script.size()) {
      const char d = script[*i + 1];
      if (d == delimiter) {
        if (!out->push_back(d)) return false;
      } else if (!regex_part && d == 'n') {
        if (!out->push_back('\x01') || !out->push_back('n')) return false;      // (marked: a newline, unlike "\\" + "n")
      } else if (!out->push_back('\\') || !out->push_back(d)) {
        return false;
      }
      *i += 2;
      continue;
    }
    if (c == '[' && regex_part) {      // a bracket expression may hold the delimiter
      const uint64_t start = *i;
      uint64_t j = *i + 1;
      if (j < script.size() && script[j] == '^') ++j;
      if (j < script.size() && script[j] == ']') ++j;
      while (j < script.size() && script[j] != ']') {
        if (script[j] == '[' && j + 1 < script.size() && script[j + 1] == ':') {
          const uint64_t close = script.find(":]", j + 2);
          if (close == npos) return false;
          j = close + 2;
          continue;
        }
        ++j;
      }
      if (j >= script.size()) return false;
      if (!out->append(script.substr(start, j + 1 - start))) return false;
      *i = j + 1;
      continue;
    }
    if (c == delimiter) {
      ++*i;
      return true;
    }
    if (!out->push_back(c)) return false;
    ++*i;
  }
  return false;
}

bool parse_sed(allocators::IAllocator& allocator, StringView script, bool extended, List<UniquePointer<SedCommand>>* commands) {
  uint64_t i = 0;
  String part(allocator);
  while (i < script.size()) {
    while (i < script.size() && (script[i] == ' ' || script[i] == '\t' || script[i] == '\n' || script[i] == ';')) ++i;
    if (i >= script.size()) break;
    UniquePointer<SedCommand> cmd = UniquePointer<SedCommand>::make(allocator, allocator);
    if (!cmd) return false;
    // Address.
    if (is_digit(script[i])) {
      uint64_t j = i;
      while (j < script.size() && is_digit(script[j])) ++j;
      if (!parse_count(script.substr(i, j - i), &cmd->line)) return false;
      cmd->where = SedCommand::Where::Line;
      i = j;
    } else if (script[i] == '$') {
      cmd->where = SedCommand::Where::Last;
      ++i;
    } else if (script[i] == '/' || script[i] == '\\') {
      char delimiter = '/';
      if (script[i] == '\\') {
        if (i + 1 >= script.size()) return false;
        delimiter = script[++i];
      }
      ++i;
      if (!sed_part(script, &i, delimiter, &part, true) || !cmd->address.compile(allocator, part.view(), extended, false)) return false;
      cmd->where = SedCommand::Where::Pattern;
    }
    while (i < script.size() && script[i] == ' ') ++i;
    if (i < script.size() && script[i] == '!') {
      cmd->negate = true;
      ++i;
    }
    if (i >= script.size()) return false;
    cmd->op = script[i++];
    if (cmd->op == 's' || cmd->op == 'y') {
      if (i >= script.size()) return false;
      const char delimiter = script[i++];
      if (!sed_part(script, &i, delimiter, &part, cmd->op == 's')) return false;
      if (cmd->op == 's') {
        bool ignore = false;
        String replacement(allocator);
        if (!sed_part(script, &i, delimiter, &replacement, false)) return false;
        cmd->replacement = util::move(replacement);
        while (i < script.size() && script[i] != ';' && script[i] != '\n' && script[i] != '}') {
          const char f = script[i++];
          if (f == 'g') cmd->global = true;
          else if (f == 'i' || f == 'I') ignore = true;
          else if (f == 'p') cmd->print = true;
          else if (is_digit(f)) cmd->occurrence = f - '0';
          else if (f == ' ') continue;
          else return false;
        }
        if (!cmd->pattern.compile(allocator, part.view(), extended, ignore)) return false;
      } else {
        if (!cmd->from.assign(part.view())) return false;
        if (!sed_part(script, &i, delimiter, &cmd->to, false)) return false;
      }
    } else if (cmd->op != 'd' && cmd->op != 'p' && cmd->op != 'q') {
      return false;
    }
    if (!commands->push_back(util::move(cmd))) return false;
  }
  return true;
}

// s's replacement for one match: & the match, \1..\9 its groups, \n a newline.
bool sed_replacement(StringView replacement, StringView text, const regex::Match& m, String* out) {
  for (uint64_t i = 0; i < replacement.size(); ++i) {
    const char c = replacement[i];
    if (c == '\x01' && i + 1 < replacement.size() && replacement[i + 1] == 'n') {
      if (!out->push_back('\n')) return false;
      ++i;
      continue;
    }
    if (c == '&') {
      if (!out->append(text.substr(m.begin(), m.length()))) return false;
      continue;
    }
    if (c == '\\' && i + 1 < replacement.size()) {
      const char d = replacement[++i];
      if (d >= '1' && d <= '9') {
        const uint32_t g = uint32_t(d - '0');
        if (g < m.count && m.has(g) && !out->append(text.substr(m.begin(g), m.length(g)))) return false;
      } else if (d == 't') {
        if (!out->push_back('\t')) return false;
      } else if (!out->push_back(d)) {
        return false;
      }
      continue;
    }
    if (!out->push_back(c)) return false;
  }
  return true;
}

int sed(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  bool quiet = false, extended = false;
  String script(allocator);
  bool have_script = false;
  for (uint64_t i = 0; i < args.size(); ++i) {
    const StringView a = args[i];
    if (a.equals("-e") || a.equals("--expression")) {
      if (i + 1 >= args.size()) return 2;
      if ((have_script && !script.push_back('\n')) || !script.append(args[++i])) return 2;
      have_script = true;
    } else if (a.starts_with("--expression=")) {
      if ((have_script && !script.push_back('\n')) || !script.append(a.substr(13))) return 2;
      have_script = true;
    } else if (a.equals("-n") || a.equals("--quiet") || a.equals("--silent")) {
      quiet = true;
    } else if (a.equals("-E") || a.equals("-r") || a.equals("--regexp-extended")) {
      extended = true;
    } else if (a.starts_with("-") && a.size() > 1) {
      for (uint64_t k = 1; k < a.size(); ++k) {
        if (a[k] == 'n') quiet = true;
        else if (a[k] == 'E' || a[k] == 'r') extended = true;
        else if (a[k] != 'i' && a[k] != 'u' && a[k] != 's') return 2;
      }
    } else if (!have_script) {
      if (!script.assign(a)) return 2;
      have_script = true;
    }
  }
  if (!have_script) return 2;
  List<UniquePointer<SedCommand>> commands(allocator);
  if (!parse_sed(allocator, script.view(), extended, &commands)) return 2;
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  String space(allocator), next(allocator);
  for (uint64_t n = 0; n < lines.count(); ++n) {
    if (!space.assign(lines.view()[n])) return 2;
    bool deleted = false, quit = false;
    for (const UniquePointer<SedCommand>& cmd : commands.view()) {
      bool applies = true;
      regex::Match am;
      switch (cmd->where) {
        case SedCommand::Where::All: break;
        case SedCommand::Where::Line: applies = int64_t(n + 1) == cmd->line; break;
        case SedCommand::Where::Last: applies = n + 1 == lines.count(); break;
        case SedCommand::Where::Pattern: applies = cmd->address.search(space.view(), &am); break;
      }
      if (cmd->negate) applies = !applies;
      if (!applies) continue;
      if (cmd->op == 'd') { deleted = true; break; }
      if (cmd->op == 'p') { if (!put_line(out, space.view())) return 2; continue; }
      if (cmd->op == 'q') { quit = true; break; }
      if (cmd->op == 'y') {
        for (uint64_t k = 0; k < space.size(); ++k) {
          const uint64_t at = cmd->from.view().find(space[k]);
          if (at != npos && at < cmd->to.size()) space.set(k, cmd->to[at]);
        }
        continue;
      }
      // s
      next.clear();
      const StringView text = space.view();
      uint64_t from = 0, copied = 0;
      int64_t seen = 0;
      bool replaced = false;
      while (from <= text.size()) {
        regex::Match m;
        if (!cmd->pattern.search(text, &m, from)) break;
        ++seen;
        const bool take = cmd->global ? seen >= cmd->occurrence : seen == cmd->occurrence;
        if (take) {
          if (!next.append(text.substr(copied, m.begin() - copied)) || !sed_replacement(cmd->replacement.view(), text, m, &next)) return 2;
          copied = m.end();
          replaced = true;
        }
        if (!cmd->global && seen >= cmd->occurrence) break;
        from = m.end() > m.begin() ? m.end() : m.begin() + 1;
        if (m.end() == m.begin() && m.begin() < text.size() && take) {      // an empty match: keep the character it stood before
          if (!next.push_back(text[m.begin()])) return 2;
          copied = m.begin() + 1;
        }
      }
      if (replaced) {
        if (!next.append(text.substr(copied))) return 2;
        space = util::move(next);
        next = String(allocator);
        if (cmd->print && !put_line(out, space.view())) return 2;
      }
    }
    if (!deleted && !quiet && !put_line(out, space.view())) return 2;
    if (quit) break;
  }
  return 0;
}

// ---------------------------------------------------------------- tr
bool tr_set(StringView spec, String* out) {
  out->clear();
  for (uint64_t i = 0; i < spec.size(); ++i) {
    char c = spec[i];
    if (c == '[' && i + 1 < spec.size() && spec[i + 1] == ':') {
      const uint64_t close = spec.find(":]", i + 2);
      if (close != npos) {
        const StringView name = spec.substr(i + 2, close - i - 2);
        for (int x = 0; x < 256; ++x) {
          const char ch = char(x);
          const bool lower = ch >= 'a' && ch <= 'z', upper = ch >= 'A' && ch <= 'Z', digit = is_digit(ch);
          const bool space = ch == ' ' || (ch >= '\t' && ch <= '\r');
          bool in = false;
          if (name.equals("lower")) in = lower;
          else if (name.equals("upper")) in = upper;
          else if (name.equals("alpha")) in = lower || upper;
          else if (name.equals("digit")) in = digit;
          else if (name.equals("alnum")) in = lower || upper || digit;
          else if (name.equals("space")) in = space;
          else if (name.equals("blank")) in = ch == ' ' || ch == '\t';
          else if (name.equals("punct")) in = x > 32 && x < 127 && !lower && !upper && !digit;
          else return false;
          if (in && !out->push_back(ch)) return false;
        }
        i = close + 1;
        continue;
      }
    }
    if (c == '\\' && i + 1 < spec.size()) {
      const char d = spec[++i];
      c = d == 'n' ? '\n' : d == 't' ? '\t' : d == 'r' ? '\r' : d == '0' ? '\0' : d;
    }
    if (i + 2 < spec.size() && spec[i + 1] == '-') {
      char last = spec[i + 2];
      uint64_t step = 2;
      if (last == '\\' && i + 3 < spec.size()) { last = spec[i + 3]; step = 3; }
      if (uint8_t(last) >= uint8_t(c)) {
        for (uint32_t x = uint8_t(c); x <= uint8_t(last); ++x)
          if (!out->push_back(char(x))) return false;
        i += step;
        continue;
      }
    }
    if (!out->push_back(c)) return false;
  }
  return true;
}

int tr(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  bool del = false, squeeze = false;
  List<StringView> sets(allocator);
  for (const StringView a : args) {
    if (a.starts_with('-') && a.size() > 1) {
      for (uint64_t k = 1; k < a.size(); ++k) {
        if (a[k] == 'd') del = true;
        else if (a[k] == 's') squeeze = true;
        else return 2;
      }
    } else if (!sets.push_back(a)) {
      return 2;
    }
  }
  if (sets.count() == 0 || (!del && !squeeze && sets.count() < 2)) return 2;
  String one(allocator), two(allocator);
  if (!tr_set(sets.view()[0], &one) || (sets.count() > 1 && !tr_set(sets.view()[1], &two))) return 2;
  int16_t map[256];
  bool remove[256] = {}, squeezable[256] = {};
  for (int x = 0; x < 256; ++x) map[x] = int16_t(x);
  if (del) {
    for (char c : one.view()) remove[uint8_t(c)] = true;
  } else if (!two.empty()) {
    for (uint64_t k = 0; k < one.size(); ++k) map[uint8_t(one[k])] = uint8_t(k < two.size() ? two[k] : two[two.size() - 1]);
  }
  const StringView squeeze_set = (del || two.empty()) ? (sets.count() > 1 ? two.view() : one.view()) : two.view();
  for (char c : squeeze_set) squeezable[uint8_t(c)] = true;
  int last = -1;
  for (char c : input) {
    if (remove[uint8_t(c)]) continue;
    const char t = char(map[uint8_t(c)]);
    if (squeeze && squeezable[uint8_t(t)] && last == uint8_t(t)) continue;
    if (!out->push_back(t)) return 2;
    last = uint8_t(t);
  }
  return 0;
}

// ---------------------------------------------------------------- head, tail
bool line_count(ArrayView<const StringView> args, int64_t* lines, int64_t* bytes, bool* from_start) {
  *lines = 10;
  *bytes = -1;
  *from_start = false;
  for (uint64_t i = 0; i < args.size(); ++i) {
    StringView a = args[i];
    StringView value;
    bool is_bytes = false;
    if (a.equals("-n") || a.equals("--lines") || a.equals("-c") || a.equals("--bytes")) {
      if (i + 1 >= args.size()) return false;
      is_bytes = a.equals("-c") || a.equals("--bytes");
      value = args[++i];
    } else if (a.starts_with("--lines=")) {
      value = a.substr(8);
    } else if (a.starts_with("--bytes=")) {
      value = a.substr(8);
      is_bytes = true;
    } else if (a.starts_with("-n") || a.starts_with("-c")) {
      is_bytes = a[1] == 'c';
      value = a.substr(2);
    } else if (a.starts_with('-') && a.size() > 1 && is_digit(a[1])) {
      value = a.substr(1);
    } else if (a.equals("-q") || a.equals("-s")) {
      continue;
    } else {
      continue;      // (a file operand: only the input is read)
    }
    if (value.starts_with('+')) *from_start = true;
    int64_t n = 0;
    if (!parse_count(value, &n)) return false;
    if (is_bytes) *bytes = n < 0 ? -n : n;
    else *lines = n;
  }
  return true;
}

int head_tail(allocators::IAllocator& allocator, bool head, ArrayView<const StringView> args, StringView input, String* out) {
  int64_t n = 0, bytes = 0;
  bool from_start = false;
  if (!line_count(args, &n, &bytes, &from_start)) return 2;
  if (bytes >= 0) {
    const uint64_t b = uint64_t(bytes) < input.size() ? uint64_t(bytes) : input.size();
    return out->append(head ? input.first(b) : input.last(b)) ? 0 : 2;
  }
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  const int64_t total = int64_t(lines.count());
  int64_t first = 0, last = total;      // [first, last)
  if (head) {
    last = n >= 0 ? (n < total ? n : total) : (total + n > 0 ? total + n : 0);
  } else if (from_start) {
    first = n > 0 ? n - 1 : 0;
  } else {
    const int64_t keep = n < 0 ? -n : n;
    first = total > keep ? total - keep : 0;
  }
  for (int64_t i = first; i < last; ++i)
    if (!put_line(out, lines.view()[uint64_t(i)])) return 2;
  return 0;
}

// ---------------------------------------------------------------- sort, uniq
int numeric_compare(StringView a, StringView b) {
  int64_t x = 0, y = 0;
  auto leading = [](StringView s, int64_t* v) {
    s = s.trimmed();
    uint64_t k = 0;
    if (k < s.size() && (s[k] == '-' || s[k] == '+')) ++k;
    while (k < s.size() && is_digit(s[k])) ++k;
    if (!parse_count(s.first(k), v)) *v = 0;
  };
  leading(a, &x);
  leading(b, &y);
  return x < y ? -1 : x > y ? 1 : 0;
}

int sort(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  bool reverse = false, unique = false, numeric = false, version = false, fold = false;
  for (uint64_t i = 0; i < args.size(); ++i) {
    const StringView a = args[i];
    if (a.equals("-t") || a.equals("-k") || a.equals("--field-separator") || a.equals("--key")) {      // (fields are not picked: the whole line is the key)
      ++i;
      continue;
    }
    if (a.starts_with("-t") || a.starts_with("-k") || a.starts_with("--field-separator=") || a.starts_with("--key=")) continue;
    if (a.equals("--version-sort")) version = true;
    else if (a.equals("--reverse")) reverse = true;
    else if (a.equals("--unique")) unique = true;
    else if (a.equals("--numeric-sort")) numeric = true;
    else if (a.equals("--ignore-case")) fold = true;
    else if (a.starts_with('-') && a.size() > 1 && !a.starts_with("--")) {
      for (uint64_t k = 1; k < a.size(); ++k) {
        switch (a[k]) {
          case 'r': reverse = true; break;
          case 'u': unique = true; break;
          case 'n': case 'g': case 'h': numeric = true; break;
          case 'V': version = true; break;
          case 'f': fold = true; break;
          case 'b': case 's': break;
          default: return 2;
        }
      }
    }
  }
  List<StringView> lines(allocator), scratch(allocator);
  if (!split_lines(input, &lines)) return 2;
  auto compare = [&](StringView a, StringView b) {
    int c;
    if (version) c = version_compare(a, b);
    else if (numeric) c = numeric_compare(a, b);
    else if (fold) {
      c = 0;
      for (uint64_t k = 0; k < a.size() && k < b.size() && c == 0; ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        c = x < y ? -1 : x > y ? 1 : 0;
      }
      if (c == 0) c = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    } else {
      c = a.compare(b);
    }
    if (c == 0 && (numeric || version || fold)) c = a.compare(b);      // (the last resort comparison of GNU sort)
    return reverse ? -c : c;
  };
  // A stable merge sort (inputs can be tens of thousands of lines).
  const uint64_t n = lines.count();
  for (uint64_t i = 0; i < n; ++i)
    if (!scratch.push_back(StringView())) return 2;
  ArrayView<StringView> a = lines.view(), b = scratch.view();
  for (uint64_t width = 1; width < n; width *= 2) {
    for (uint64_t lo = 0; lo < n; lo += 2 * width) {
      const uint64_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
      uint64_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) b[k++] = compare(a[j], a[i]) < 0 ? a[j++] : a[i++];
      while (i < mid) b[k++] = a[i++];
      while (j < hi) b[k++] = a[j++];
    }
    ArrayView<StringView> t = a;
    a = b;
    b = t;
  }
  for (uint64_t i = 0; i < n; ++i) {
    if (unique && i > 0 && compare(a[i], a[i - 1]) == 0) continue;
    if (!put_line(out, a[i])) return 2;
  }
  return 0;
}

int uniq(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  bool counts = false, only_repeated = false, only_unique = false;
  for (const StringView a : args) {
    if (a.equals("-c")) counts = true;
    else if (a.equals("-d")) only_repeated = true;
    else if (a.equals("-u")) only_unique = true;
  }
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  for (uint64_t i = 0; i < lines.count();) {
    uint64_t j = i + 1;
    while (j < lines.count() && lines.view()[j].equals(lines.view()[i])) ++j;
    const uint64_t count = j - i;
    if ((!only_repeated || count > 1) && (!only_unique || count == 1)) {
      if (counts) {
        String number(allocator);
        if (!number.append_uint(count)) return 2;
        for (uint64_t pad = number.size(); pad < 7; ++pad)
          if (!out->push_back(' ')) return 2;
        if (!out->append(number.view()) || !out->push_back(' ')) return 2;
      }
      if (!put_line(out, lines.view()[i])) return 2;
    }
    i = j;
  }
  return 0;
}

// ---------------------------------------------------------------- cut, wc, rev, awk
// "1,3-5,7-" as a set of 1-based positions (up to 4096).
bool parse_list(StringView list, bool (&wanted)[4097], bool* open_end, uint64_t* open_from) {
  *open_end = false;
  StringView rest = list, part;
  while (rest.next_token(',', &part)) {
    if (part.empty()) continue;
    const uint64_t dash = part.find('-');
    uint64_t a = 1, b = 0;
    if (dash == npos) {
      if (!part.parse_uint(&a)) return false;
      b = a;
    } else {
      if (dash > 0 && !part.first(dash).parse_uint(&a)) return false;
      if (dash + 1 == part.size()) {
        *open_end = true;
        *open_from = a;
        continue;
      }
      if (!part.substr(dash + 1).parse_uint(&b)) return false;
    }
    if (a == 0 || b > 4096) return false;
    for (uint64_t k = a; k <= b; ++k) wanted[k] = true;
  }
  return true;
}

int cut(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  char delimiter = '\t';
  StringView fields, chars;
  bool only_delimited = false;
  for (uint64_t i = 0; i < args.size(); ++i) {
    StringView a = args[i];
    auto value = [&](StringView flag) -> StringView {
      if (a.equals(flag)) return i + 1 < args.size() ? args[++i] : StringView();
      return a.substr(flag.size());
    };
    if (a.starts_with("-d")) { const StringView d = value("-d"); delimiter = d.empty() ? '\t' : d[0]; }
    else if (a.starts_with("-f")) fields = value("-f");
    else if (a.starts_with("-c") || a.starts_with("-b")) chars = value(a.starts_with("-c") ? "-c" : "-b");
    else if (a.equals("-s")) only_delimited = true;
  }
  bool wanted[4097] = {};
  bool open_end = false;
  uint64_t open_from = 0;
  if (!parse_list(fields.empty() ? chars : fields, wanted, &open_end, &open_from)) return 2;
  auto want = [&](uint64_t position) { return (position <= 4096 && wanted[position]) || (open_end && position >= open_from); };
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  for (const StringView line : lines.view()) {
    if (fields.empty()) {
      for (uint64_t k = 0; k < line.size(); ++k)
        if (want(k + 1) && !out->push_back(line[k])) return 2;
    } else {
      if (!line.contains(delimiter)) {
        if (!only_delimited && !put_line(out, line)) return 2;
        continue;
      }
      StringView rest = line, field;
      uint64_t index = 0;
      bool first = true;
      for (uint64_t pos = 0; pos <= rest.size();) {
        uint64_t end = rest.find(delimiter, pos);
        if (end == npos) end = rest.size();
        field = rest.substr(pos, end - pos);
        ++index;
        if (want(index)) {
          if ((!first && !out->push_back(delimiter)) || !out->append(field)) return 2;
          first = false;
        }
        if (end == rest.size()) break;
        pos = end + 1;
      }
    }
    if (!out->push_back('\n')) return 2;
  }
  return 0;
}

int wc(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out) {
  uint64_t lines = 0, words = 0;
  bool in_word = false;
  for (char c : input) {
    if (c == '\n') ++lines;
    const bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (!blank && !in_word) ++words;
    in_word = !blank;
  }
  bool l = false, w = false, c = false;
  for (const StringView a : args) {
    if (a.contains('l')) l = true;
    if (a.contains('w')) w = true;
    if (a.contains('c') || a.contains('m')) c = true;
  }
  if (!l && !w && !c) l = w = c = true;
  bool first = true;
  auto put = [&](uint64_t value) {
    if (!first && !out->push_back(' ')) return false;
    first = false;
    return out->append_uint(value);
  };
  if ((l && !put(lines)) || (w && !put(words)) || (c && !put(input.size())) || !out->push_back('\n')) return 2;
  (void)allocator;
  return 0;
}

int rev(allocators::IAllocator& allocator, StringView input, String* out) {
  List<StringView> lines(allocator);
  if (!split_lines(input, &lines)) return 2;
  for (const StringView line : lines.view()) {
    for (uint64_t k = line.size(); k > 0; --k)
      if (!out->push_back(line[k - 1])) return 2;
    if (!out->push_back('\n')) return 2;
  }
  return 0;
}

// awk with one rule: [/re/] { print $a, $b ... }  (or no action: print the line). -F sets the field separator.
int awk(allocators::IAllocator& allocator, ArrayView<const StringView> args, StringView input, String* out, bool* known) {
  StringView separator, program;
  for (uint64_t i = 0; i < args.size(); ++i) {
    const StringView a = args[i];
    if (a.equals("-F")) separator = i + 1 < args.size() ? args[++i] : StringView();
    else if (a.starts_with("-F")) separator = a.substr(2);
    else if (program.empty()) program = a;
  }
  program = program.trimmed();
  Pattern filter(allocator);
  bool filtered = false;
  if (program.starts_with('/')) {
    const uint64_t close = program.find('/', 1);
    if (close == npos || !filter.compile(allocator, program.substr(1, close - 1), true, false)) return 2;
    filtered = true;
    program = program.substr(close + 1).trimmed();
  }
  List<int64_t> columns(allocator);      // -1: NF
  bool whole = program.empty();
  if (!whole) {
    if (!program.starts_with('{') || !program.ends_with('}')) { *known = false; return 127; }
    StringView body = program.substr(1, program.size() - 2).trimmed();
    if (body.ends_with(';')) body = body.first(body.size() - 1).trimmed();
    if (!body.starts_with("print")) { *known = false; return 127; }
    body = body.substr(5).trimmed();
    if (body.empty()) whole = true;
    StringView part;
    while (!whole && body.next_token(',', &part)) {
      part = part.trimmed();
      if (part.equals("$NF")) { if (!columns.push_back(-1)) return 2; continue; }
      int64_t n = 0;
      if (!part.starts_with('$') || !parse_count(part.substr(1), &n) || n < 0) { *known = false; return 127; }
      if (!columns.push_back(n)) return 2;
    }
  }
  List<StringView> lines(allocator), fields(allocator);
  if (!split_lines(input, &lines)) return 2;
  for (const StringView line : lines.view()) {
    regex::Match m;
    if (filtered && !filter.search(line, &m)) continue;
    if (whole) { if (!put_line(out, line)) return 2; continue; }
    fields.clear();
    if (separator.empty() || separator.equals(" ")) {
      for (uint64_t k = 0; k < line.size();) {
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        const uint64_t start = k;
        while (k < line.size() && line[k] != ' ' && line[k] != '\t') ++k;
        if (k > start && !fields.push_back(line.substr(start, k - start))) return 2;
      }
    } else {
      for (uint64_t pos = 0;;) {
        const uint64_t end = line.find(separator, pos);
        if (!fields.push_back(line.substr(pos, (end == npos ? line.size() : end) - pos))) return 2;
        if (end == npos) break;
        pos = end + separator.size();
      }
    }
    bool first = true;
    for (const int64_t column : columns.view()) {
      if (!first && !out->push_back(' ')) return 2;
      first = false;
      const uint64_t index = column < 0 ? fields.count() : uint64_t(column);
      const StringView value = index == 0 ? line : index <= fields.count() ? fields.view()[index - 1] : StringView();
      if (!out->append(value)) return 2;
    }
    if (!out->push_back('\n')) return 2;
  }
  return 0;
}

}  // namespace

bool translate_regex(StringView p, bool extended, String* out) {
  out->clear();
  bool at_start = true;
  for (uint64_t i = 0; i < p.size();) {
    const char c = p[i];
    bool next_at_start = false;
    if (c == '[') {
      uint64_t j = i + 1;
      if (!out->push_back('[')) return false;
      if (j < p.size() && p[j] == '^') {
        if (!out->push_back('^')) return false;
        ++j;
      }
      bool first = true;
      for (;; ++j) {
        if (j >= p.size()) return false;
        const char ch = p[j];
        if (ch == ']' && !first) break;
        first = false;
        if (ch == '[' && j + 1 < p.size() && (p[j + 1] == ':' || p[j + 1] == '.' || p[j + 1] == '=')) {
          const char kind = p[j + 1];
          const char end_mark[3] = {kind, ']', 0};
          const uint64_t close = p.find(StringView(end_mark), j + 2);
          if (close == npos) return false;
          const StringView name = p.substr(j + 2, close - j - 2);
          if (kind == ':') {
            const StringView ranges = class_ranges(name);
            if (ranges.empty() || !out->append(ranges)) return false;
          } else {
            for (char x : name)
              if (!append_escaped(out, x)) return false;
          }
          j = close + 1;
          continue;
        }
        if ((ch == '\\' || ch == ']' || ch == '[' || ch == '^') && !out->push_back('\\')) return false;
        if (!out->push_back(ch)) return false;
      }
      if (!out->push_back(']')) return false;
      i = j + 1;
    } else if (c == '\\') {
      if (i + 1 >= p.size()) return false;
      const char d = p[i + 1];
      bool ok = true;
      if (!extended && (d == '(' || d == ')' || d == '{' || d == '}' || d == '|' || d == '+' || d == '?')) {
        ok = out->push_back(d);
        next_at_start = d == '(' || d == '|';
      } else if (d == '<' || d == '>') {
        ok = out->append("\\b");
      } else if ((d >= '1' && d <= '9') || d == 'n' || d == 't' || d == 'w' || d == 'W' || d == 's' || d == 'S' || d == 'b' || d == 'B') {
        ok = out->push_back('\\') && out->push_back(d);
      } else if (d == '`') {
        ok = out->push_back('^');
      } else if (d == '\'') {
        ok = out->push_back('$');
      } else {
        ok = append_escaped(out, d);
      }
      if (!ok) return false;
      i += 2;
    } else if (!extended && (c == '(' || c == ')' || c == '{' || c == '}' || c == '|' || c == '+' || c == '?')) {
      if (!out->push_back('\\') || !out->push_back(c)) return false;
      ++i;
    } else if (c == '*' && (at_start || (extended && i == 0))) {
      if (!out->append("\\*")) return false;
      ++i;
    } else if (c == '^') {
      if (!(extended || at_start ? out->push_back('^') : out->append("\\^"))) return false;
      next_at_start = true;
      ++i;
    } else if (c == '$') {
      const bool end = extended || i + 1 == p.size() || (p.substr(i + 1).starts_with("\\)") || p.substr(i + 1).starts_with("\\|"));
      if (!(end ? out->push_back('$') : out->append("\\$"))) return false;
      ++i;
    } else if (c == ']' || c == '/') {
      if (!out->push_back('\\') || !out->push_back(c)) return false;
      ++i;
    } else if (extended && (c == '(' || c == '|')) {
      if (!out->push_back(c)) return false;
      next_at_start = true;
      ++i;
    } else {
      if (!out->push_back(c)) return false;
      ++i;
    }
    at_start = next_at_start;
  }
  return true;
}

int version_compare(StringView a, StringView b) {
  uint64_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (is_digit(a[i]) && is_digit(b[j])) {
      while (i < a.size() && a[i] == '0') ++i;
      while (j < b.size() && b[j] == '0') ++j;
      uint64_t x = i, y = j;
      while (x < a.size() && is_digit(a[x])) ++x;
      while (y < b.size() && is_digit(b[y])) ++y;
      if (x - i != y - j) return x - i < y - j ? -1 : 1;
      for (uint64_t k = 0; k < x - i; ++k)
        if (a[i + k] != b[j + k]) return a[i + k] < b[j + k] ? -1 : 1;
      i = x;
      j = y;
      continue;
    }
    if (a[i] != b[j]) return uint8_t(a[i]) < uint8_t(b[j]) ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

bool run(allocators::IAllocator& allocator, StringView name, ArrayView<const StringView> arguments, StringView input, String* out, int* status) {
  bool known = true;
  if (name.equals("grep") || name.equals("egrep") || name.equals("fgrep")) {
    if (name.equals("grep")) {
      *status = grep(allocator, arguments, input, out);
    } else {
      List<StringView> args(allocator);
      if (!args.push_back(name.equals("egrep") ? StringView("-E") : StringView("-F"))) { *status = 2; return true; }
      for (const StringView a : arguments)
        if (!args.push_back(a)) { *status = 2; return true; }
      *status = grep(allocator, args.view(), input, out);
    }
  } else if (name.equals("sed")) {
    *status = sed(allocator, arguments, input, out);
  } else if (name.equals("tr")) {
    *status = tr(allocator, arguments, input, out);
  } else if (name.equals("head") || name.equals("tail")) {
    *status = head_tail(allocator, name.equals("head"), arguments, input, out);
  } else if (name.equals("sort")) {
    *status = sort(allocator, arguments, input, out);
  } else if (name.equals("uniq")) {
    *status = uniq(allocator, arguments, input, out);
  } else if (name.equals("cut")) {
    *status = cut(allocator, arguments, input, out);
  } else if (name.equals("wc")) {
    *status = wc(allocator, arguments, input, out);
  } else if (name.equals("rev")) {
    *status = rev(allocator, input, out);
  } else if (name.equals("awk") || name.equals("gawk") || name.equals("mawk")) {
    *status = awk(allocator, arguments, input, out, &known);
  } else {
    known = false;
  }
  return known;
}

}  // namespace desktop::shell_tools

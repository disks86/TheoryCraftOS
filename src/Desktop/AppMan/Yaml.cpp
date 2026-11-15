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

#include "Desktop/AppMan/Yaml.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

struct Line {
  uint32_t indent = 0;      // leading spaces
  StringView text;          // without them and without trailing white space; "" for a blank line
  bool blank = false;       // nothing, or only a comment
};

bool is_space(char c) { return c == ' ' || c == '\t'; }

StringView trim_right(StringView text) {
  while (!text.empty() && (is_space(text[text.size() - 1]) || text[text.size() - 1] == '\r')) text = text.first(text.size() - 1);
  return text;
}

// Where a plain scalar's comment starts (" #"), or its length.
uint64_t comment_start(StringView text) {
  for (uint64_t i = 1; i < text.size(); ++i)
    if (text[i] == '#' && is_space(text[i - 1])) return i;
  return text.size();
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool append_code_point(String* out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
  if (cp < 0x80) return out->push_back(static_cast<char>(cp));
  if (cp < 0x800) return out->push_back(static_cast<char>(0xC0 | (cp >> 6))) && out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  if (cp < 0x10000)
    return out->push_back(static_cast<char>(0xE0 | (cp >> 12))) && out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) &&
           out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  return out->push_back(static_cast<char>(0xF0 | (cp >> 18))) && out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))) &&
         out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) && out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

class Parser {
 public:
  Parser(allocators::IAllocator& allocator) : allocator_(allocator), lines_(allocator) {}

  bool run(StringView text, YamlNode* root) {
    if (text.size() > kMaxYamlBytes) return false;
    if (text.starts_with("\xEF\xBB\xBF")) text = text.substr(3);
    StringView rest = text, raw;
    while (rest.next_token('\n', &raw)) {
      Line line;
      while (line.indent < raw.size() && raw[line.indent] == ' ') ++line.indent;
      line.text = trim_right(raw.substr(line.indent));
      line.blank = line.text.empty() || line.text[0] == '#';
      // (a document start, a document end and directives are skipped: there is one document)
      if (line.indent == 0 && (line.text.equals("---") || line.text.equals("...") || line.text.starts_with('%'))) line.blank = true, line.text = StringView();
      if (!lines_.push_back(line)) return false;
    }
    skip_blank();
    if (at_ >= lines_.count()) {      // an empty document: an empty mapping
      root->kind = YamlNode::Kind::Map;
      return true;
    }
    if (!block(lines_[at_].indent, 0, root)) return false;
    skip_blank();
    return at_ == lines_.count();
  }

 private:
  void skip_blank() {
    while (at_ < lines_.count() && lines_[at_].blank) ++at_;
  }
  static bool is_item(StringView text) { return text.equals("-") || text.starts_with("- "); }

  // The length of the key of "key: value" / "key:" (a plain or quoted key), or npos if the line is not an entry of a mapping.
  static uint64_t key_end(StringView text) {
    if (text.empty()) return StringView::npos;
    if (text[0] == '"' || text[0] == '\'') {
      const uint64_t close = text.find(text[0], 1);
      if (close == StringView::npos || close + 1 >= text.size() || text[close + 1] != ':') return StringView::npos;
      return close + 1 == text.size() - 1 || is_space(text[close + 2]) ? close + 1 : StringView::npos;
    }
    for (uint64_t i = 0; i < text.size(); ++i) {
      if (text[i] == '#' && i > 0 && is_space(text[i - 1])) return StringView::npos;
      if (text[i] == ':' && (i + 1 == text.size() || is_space(text[i + 1]))) return i == 0 ? StringView::npos : i;      // (a key is not empty)
    }
    return StringView::npos;
  }

  // The mapping or sequence whose lines are indented by `indent`, starting at the current line.
  bool block(uint32_t indent, uint32_t depth, YamlNode* node) {
    if (depth > kMaxYamlDepth || at_ >= lines_.count()) return false;
    if (is_item(lines_[at_].text)) return sequence(indent, depth, node);
    if (key_end(lines_[at_].text) != StringView::npos) return mapping(indent, depth, node);
    // A scalar on lines of its own (the value of a key, put on the next line).
    node->kind = YamlNode::Kind::Scalar;
    const StringView first = lines_[at_].text;
    ++at_;
    return scalar(first, indent == 0 ? 0 : indent - 1, &node->value);
  }

  UniquePointer<YamlNode> new_node() {
    if (++nodes_ > kMaxYamlNodes) return UniquePointer<YamlNode>();
    return UniquePointer<YamlNode>::make(allocator_, allocator_);
  }

  bool sequence(uint32_t indent, uint32_t depth, YamlNode* node) {
    node->kind = YamlNode::Kind::Sequence;
    while (true) {
      skip_blank();
      if (at_ >= lines_.count() || lines_[at_].indent != indent || !is_item(lines_[at_].text)) break;
      UniquePointer<YamlNode> item = new_node();
      if (!item) return false;
      Line& line = lines_[at_];
      StringView rest = line.text.substr(1);
      uint32_t inner = indent + 1;
      while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1), ++inner;
      if (rest.empty() || rest[0] == '#') {      // "-" alone: the item is the block below it
        ++at_;
        skip_blank();
        if (at_ < lines_.count() && lines_[at_].indent > indent) {
          if (!block(lines_[at_].indent, depth + 1, item.get())) return false;
        }
      } else if (is_item(rest) || key_end(rest) != StringView::npos) {
        // "- key: value" or "- - x": the rest of the line starts a block indented to where it begins.
        line.indent = inner;
        line.text = rest;
        if (!block(inner, depth + 1, item.get())) return false;
      } else {
        ++at_;
        if (!scalar(rest, indent, &item->value)) return false;
      }
      if (!node->children.push_back(util::move(item))) return false;
    }
    // (a line indented further than the items that belongs to none of them is not YAML)
    return at_ >= lines_.count() || lines_[at_].indent <= indent;
  }

  bool mapping(uint32_t indent, uint32_t depth, YamlNode* node) {
    node->kind = YamlNode::Kind::Map;
    while (true) {
      skip_blank();
      if (at_ >= lines_.count() || lines_[at_].indent != indent) break;
      const StringView text = lines_[at_].text;
      const uint64_t colon = key_end(text);
      if (colon == StringView::npos) {
        if (is_item(text)) break;      // (the sequence of the mapping above, at this indentation)
        return false;
      }
      UniquePointer<YamlNode> entry = new_node();
      if (!entry) return false;
      StringView key = text.first(colon);
      if (key.size() >= 2 && (key[0] == '"' || key[0] == '\'')) key = key.substr(1, key.size() - 2);
      if (!entry->key.assign(key)) return false;
      StringView rest = text.substr(colon + 1);
      while (!rest.empty() && is_space(rest[0])) rest = rest.substr(1);
      ++at_;
      if (rest.empty() || rest[0] == '#') {
        // The value is the block below: indented further, or a sequence at this very indentation.
        skip_blank();
        if (at_ < lines_.count() && (lines_[at_].indent > indent || (lines_[at_].indent == indent && is_item(lines_[at_].text)))) {
          if (!block(lines_[at_].indent, depth + 1, entry.get())) return false;
        }
      } else if (!scalar(rest, indent, &entry->value)) {
        return false;
      }
      if (!node->children.push_back(util::move(entry))) return false;
    }
    return at_ >= lines_.count() || lines_[at_].indent <= indent;
  }

  // A scalar that starts with `first` (the rest of its first line); lines indented by more than `parent` continue it. The current line is the one
  // after the first.
  bool scalar(StringView first, uint32_t parent, String* out) {
    out->clear();
    if (first[0] == '|' || first[0] == '>') return block_scalar(first[0] == '>', parent, out);
    if (first[0] == '"' || first[0] == '\'') return quoted(first, parent, out);
    // Plain: the lines are joined by single spaces.
    if (!out->assign(trim_right(first.first(comment_start(first))))) return false;
    while (at_ < lines_.count() && (lines_[at_].blank ? lines_[at_].text.empty() : lines_[at_].indent > parent)) {
      const Line& line = lines_[at_];
      if (line.blank) {      // an empty line inside: a line break - if the scalar goes on after it
        uint64_t next = at_;
        while (next < lines_.count() && lines_[next].blank) ++next;
        if (next >= lines_.count() || lines_[next].indent <= parent) break;
        if (!out->push_back('\n')) return false;
        at_ = next;
        continue;
      }
      const StringView piece = trim_right(line.text.first(comment_start(line.text)));
      if (!out->empty() && !out->view().ends_with('\n') && !out->push_back(' ')) return false;
      if (!out->append(piece)) return false;
      ++at_;
    }
    return true;
  }

  // "..." or '...', which may close on a later line. Line breaks inside fold to a space; an empty line is a line break.
  bool quoted(StringView first, uint32_t parent, String* out) {
    const char quote = first[0];
    StringView text = first.substr(1);
    bool pending_space = false;
    while (true) {
      uint64_t i = 0;
      // (the white space around a line break went with the indentation and the trimmed line ends)
      while (i < text.size()) {
        const char c = text[i];
        if (c == quote) {
          if (quote == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {      // '' is a quote
            if (!out->push_back('\'')) return false;
            i += 2;
            continue;
          }
          const StringView after = text.substr(i + 1).trimmed();
          return after.empty() || after[0] == '#';
        }
        if (quote == '"' && c == '\\') {
          if (i + 1 >= text.size()) {      // an escaped line break: the lines join without a space
            pending_space = false;
            i = text.size();
            goto next_line;
          }
          const char e = text[i + 1];
          i += 2;
          uint32_t digits = 0;
          switch (e) {
            case 'n': if (!out->push_back('\n')) return false; break;
            case 't': if (!out->push_back('\t')) return false; break;
            case 'r': if (!out->push_back('\r')) return false; break;
            case '0': if (!out->push_back(' ')) return false; break;      // (a NUL would end the text for every reader after this one)
            case 'a': case 'b': case 'v': case 'f': case 'e': break;      // (control characters: dropped)
            case ' ': case '"': case '/': case '\\': if (!out->push_back(e)) return false; break;
            case 'N': if (!append_code_point(out, 0x85)) return false; break;
            case '_': if (!append_code_point(out, 0xA0)) return false; break;
            case 'L': if (!append_code_point(out, 0x2028)) return false; break;
            case 'P': if (!append_code_point(out, 0x2029)) return false; break;
            case 'x': digits = 2; break;
            case 'u': digits = 4; break;
            case 'U': digits = 8; break;
            default: return false;
          }
          if (digits) {
            if (digits > text.size() - i) return false;
            uint32_t cp = 0;
            for (uint32_t k = 0; k < digits; ++k) {
              const int d = hex_digit(text[i + k]);
              if (d < 0) return false;
              cp = (cp << 4) | static_cast<uint32_t>(d);
            }
            i += digits;
            if (!append_code_point(out, cp < 0x20 && cp != '\n' && cp != '\t' ? ' ' : cp)) return false;
          }
          continue;
        }
        if (!out->push_back(c)) return false;
        ++i;
      }
      pending_space = true;
    next_line:
      // The scalar goes on in the next line.
      uint32_t breaks = 0;
      while (at_ < lines_.count() && lines_[at_].text.empty()) ++at_, ++breaks;
      if (at_ >= lines_.count() || lines_[at_].indent <= parent) return false;      // (it never closed)
      // (a "comment" line inside quotes is text, which Line::blank does not know: its text is still there)
      if (breaks) {
        for (uint32_t k = 0; k < breaks; ++k)
          if (!out->push_back('\n')) return false;
      } else if (pending_space && !out->push_back(' ')) {
        return false;
      }
      text = lines_[at_].text;
      ++at_;
    }
  }

  // | (lines kept) or > (lines folded), with the lines below indented by more than `parent`.
  bool block_scalar(bool folded, uint32_t parent, String* out) {
    uint32_t indent = 0;
    bool first = true, after_break = true;
    while (at_ < lines_.count()) {
      const Line& line = lines_[at_];
      const bool empty = line.text.empty();
      if (!empty && line.indent <= parent) break;
      if (!empty && indent == 0) indent = line.indent;
      if (!empty && line.indent < indent) break;
      if (empty) {
        if (!first && !out->push_back('\n')) return false;
        after_break = true;
      } else {
        if (!first && !after_break && !out->push_back(folded ? ' ' : '\n')) return false;
        if (!first && after_break && !folded && !out->push_back('\n')) return false;
        for (uint32_t k = indent; k < line.indent; ++k)
          if (!out->push_back(' ')) return false;
        if (!out->append(line.text)) return false;
        after_break = false;
      }
      first = false;
      ++at_;
    }
    while (!out->empty() && (*out)[out->size() - 1] == '\n') out->truncate(out->size() - 1);
    return true;
  }

  allocators::IAllocator& allocator_;
  List<Line> lines_;
  uint64_t at_ = 0;
  uint64_t nodes_ = 0;
};

}  // namespace

const YamlNode* YamlNode::find(StringView name) const {
  if (kind != Kind::Map) return nullptr;
  for (const UniquePointer<YamlNode>& child : children.view())
    if (child->key.view().equals(name)) return child.get();
  return nullptr;
}

StringView YamlNode::text(StringView name) const {
  const YamlNode* node = find(name);
  return node && node->kind == Kind::Scalar ? node->value.view() : StringView();
}

bool parse_yaml(allocators::IAllocator& allocator, StringView text, YamlNode* root) {
  root->key.clear();
  root->value.clear();
  root->children.clear();
  Parser parser(allocator);
  return parser.run(text, root);
}

}  // namespace desktop::appman

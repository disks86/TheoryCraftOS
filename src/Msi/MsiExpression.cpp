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

#include "Msi/MsiExpression.hpp"

namespace msi {

bool parse_integer(StringView text, int64_t* value) {
  const bool negative = text.starts_with('-');
  if (negative) text = text.without_prefix(1);
  if (text.empty() || text.size() > 18) return false;
  int64_t v = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *value = negative ? -v : v;
  return true;
}

bool equals_folded(StringView a, StringView b) { return a.equals_ignore_case(b); }

namespace {

constexpr uint32_t kMaxDepth = 64;

bool name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool name_part(char c) { return name_start(c) || (c >= '0' && c <= '9') || c == '.'; }

int compare_text(StringView a, StringView b, bool folded) {
  const uint64_t shared = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < shared; ++i) {
    const uint8_t x = static_cast<uint8_t>(folded ? fold(a[i]) : a[i]), y = static_cast<uint8_t>(folded ? fold(b[i]) : b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

bool contains_text(StringView text, StringView part, bool folded) {
  if (part.size() > text.size()) return false;
  for (uint64_t at = 0; at + part.size() <= text.size(); ++at)
    if (compare_text(text.substr(at, part.size()), part, folded) == 0) return true;
  return false;
}

enum class Operator : uint8_t { None, Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual, Contains, StartsWith, EndsWith };

struct Value {
  enum class Kind : uint8_t { Integer, Literal, Symbol } kind = Kind::Symbol;
  int64_t integer = 0;
  StringView text;
};

bool compare_integers(int64_t a, Operator op, int64_t b) {
  switch (op) {
    case Operator::Equal: return a == b;
    case Operator::NotEqual: return a != b;
    case Operator::Less: return a < b;
    case Operator::Greater: return a > b;
    case Operator::LessEqual: return a <= b;
    case Operator::GreaterEqual: return a >= b;
    case Operator::Contains: return (a & b) != 0;                                 // bits in common
    case Operator::StartsWith: return ((a >> 16) & 0xFFFF) == b;                  // the high word
    case Operator::EndsWith: return (a & 0xFFFF) == b;                            // the low word
    case Operator::None: break;
  }
  return false;
}

bool compare_texts(StringView a, Operator op, StringView b, bool folded) {
  switch (op) {
    case Operator::Equal: return compare_text(a, b, folded) == 0;
    case Operator::NotEqual: return compare_text(a, b, folded) != 0;
    case Operator::Less: return compare_text(a, b, folded) < 0;
    case Operator::Greater: return compare_text(a, b, folded) > 0;
    case Operator::LessEqual: return compare_text(a, b, folded) <= 0;
    case Operator::GreaterEqual: return compare_text(a, b, folded) >= 0;
    case Operator::Contains: return contains_text(a, b, folded);
    case Operator::StartsWith: return b.size() <= a.size() && compare_text(a.first(b.size()), b, folded) == 0;
    case Operator::EndsWith: return b.size() <= a.size() && compare_text(a.last(b.size()), b, folded) == 0;
    case Operator::None: break;
  }
  return false;
}

// A recursive-descent reader of a condition. Every step takes at least one character or fails, and nesting is limited.
class ConditionReader {
 public:
  ConditionReader(StringView text, const IExpressionContext& context) : text_(text), context_(context) {}

  bool read(bool* result) {
    if (!expression(result, 0)) return false;
    skip_blanks();
    return at_ == text_.size();
  }

 private:
  void skip_blanks() {
    while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_;
  }
  // The word at the reading position, if it is `word` (as a whole word, in any case): taken.
  bool keyword(StringView word) {
    skip_blanks();
    StringView here;
    if (!text_.try_substr(at_, word.size(), &here) || !here.equals_ignore_case(word)) return false;
    char after = 0;
    if (text_.try_get(at_ + word.size(), &after) && name_part(after)) return false;
    at_ += word.size();
    return true;
  }
  bool symbol(char c) {
    skip_blanks();
    char here = 0;
    if (!text_.try_get(at_, &here) || here != c) return false;
    ++at_;
    return true;
  }

  // OR, XOR, EQV and IMP bind alike, from left to right; AND binds tighter, NOT tighter still.
  bool expression(bool* result, uint32_t depth) {
    if (depth > kMaxDepth || !term(result, depth)) return false;
    for (;;) {
      bool right = false;
      if (keyword("OR")) {
        if (!term(&right, depth)) return false;
        *result = *result || right;
      } else if (keyword("XOR")) {
        if (!term(&right, depth)) return false;
        *result = *result != right;
      } else if (keyword("EQV")) {
        if (!term(&right, depth)) return false;
        *result = *result == right;
      } else if (keyword("IMP")) {
        if (!term(&right, depth)) return false;
        *result = !*result || right;
      } else {
        return true;
      }
    }
  }
  bool term(bool* result, uint32_t depth) {
    if (!factor(result, depth)) return false;
    while (keyword("AND")) {
      bool right = false;
      if (!factor(&right, depth)) return false;
      *result = *result && right;
    }
    return true;
  }
  bool factor(bool* result, uint32_t depth) {
    if (depth > kMaxDepth) return false;
    if (keyword("NOT")) {
      if (!factor(result, depth + 1)) return false;
      *result = !*result;
      return true;
    }
    if (symbol('(')) return expression(result, depth + 1) && symbol(')');
    Value left;
    if (!value(&left)) return false;
    bool folded = false;
    const Operator op = comparison(&folded);
    if (op == Operator::None) {
      *result = left.kind == Value::Kind::Integer ? left.integer != 0 : !left.text.empty();
      return true;
    }
    Value right;
    if (!value(&right)) return false;
    const bool left_number = left.kind == Value::Kind::Integer, right_number = right.kind == Value::Kind::Integer;
    if (left_number && right_number) {
      *result = compare_integers(left.integer, op, right.integer);
    } else if (!left_number && !right_number) {
      *result = compare_texts(left.text, op, right.text, folded);
    } else if (left.kind == Value::Kind::Literal || right.kind == Value::Kind::Literal) {
      *result = op == Operator::NotEqual;      // (text is never a number)
    } else {
      // A property against a number: as a number, if the property's value is one.
      int64_t number = 0;
      if (!parse_integer(left_number ? right.text : left.text, &number)) *result = op == Operator::NotEqual;
      else *result = left_number ? compare_integers(left.integer, op, number) : compare_integers(number, op, right.integer);
    }
    return true;
  }
  Operator comparison(bool* folded) {
    skip_blanks();
    const uint64_t start = at_;
    *folded = false;
    char c = 0, next = 0;
    if (text_.try_get(at_, &c) && c == '~') {
      *folded = true;
      ++at_;
    }
    if (!text_.try_get(at_, &c)) {
      at_ = start;
      return Operator::None;
    }
    (void)text_.try_get(at_ + 1, &next);
    Operator op = Operator::None;
    uint32_t length = 1;
    if (c == '=') op = Operator::Equal;
    else if (c == '<' && next == '>') op = Operator::NotEqual, length = 2;
    else if (c == '<' && next == '=') op = Operator::LessEqual, length = 2;
    else if (c == '<' && next == '<') op = Operator::StartsWith, length = 2;
    else if (c == '>' && next == '=') op = Operator::GreaterEqual, length = 2;
    else if (c == '>' && next == '<') op = Operator::Contains, length = 2;
    else if (c == '>' && next == '>') op = Operator::EndsWith, length = 2;
    else if (c == '<') op = Operator::Less;
    else if (c == '>') op = Operator::Greater;
    if (op == Operator::None) at_ = start;
    else at_ += length;
    return op;
  }
  bool name(StringView* out) {
    const uint64_t start = at_;
    char c = 0;
    if (!text_.try_get(at_, &c) || !name_start(c)) return false;
    while (text_.try_get(at_, &c) && name_part(c)) ++at_;
    *out = text_.substr(start, at_ - start);
    return true;
  }
  bool value(Value* out) {
    skip_blanks();
    char c = 0;
    if (!text_.try_get(at_, &c)) return false;
    if (c == '"') {
      const uint64_t close = text_.find('"', at_ + 1);
      if (close == StringView::npos) return false;
      out->kind = Value::Kind::Literal;
      out->text = text_.substr(at_ + 1, close - at_ - 1);
      at_ = close + 1;
      return true;
    }
    if ((c >= '0' && c <= '9') || c == '-') {
      const uint64_t start = at_++;
      while (text_.try_get(at_, &c) && c >= '0' && c <= '9') ++at_;
      out->kind = Value::Kind::Integer;
      return parse_integer(text_.substr(start, at_ - start), &out->integer);
    }
    if (c == '$' || c == '?' || c == '&' || c == '!') {
      ++at_;
      StringView key;
      if (!name(&key)) return false;
      out->kind = Value::Kind::Integer;
      out->integer = context_.state(c, key);
      return true;
    }
    if (c == '%') {      // an environment variable: installations run with none set
      ++at_;
      StringView key;
      if (!name(&key)) return false;
      out->kind = Value::Kind::Symbol;
      out->text = StringView();
      return true;
    }
    StringView key;
    if (!name(&key)) return false;
    static constexpr StringView kWords[] = {"AND", "OR", "NOT", "XOR", "EQV", "IMP"};
    for (const StringView word : kWords)
      if (key.equals_ignore_case(word)) return false;      // (a word of the language where a value belongs)
    out->kind = Value::Kind::Symbol;
    out->text = context_.property(key);
    return true;
  }

  StringView text_;
  const IExpressionContext& context_;
  uint64_t at_ = 0;
};

bool format_into(allocators::IAllocator& allocator, StringView text, const IExpressionContext& context, String* out, uint32_t depth) {
  uint64_t at = 0;
  while (at < text.size()) {
    const char c = text[at];
    if (c != '[') {
      if (!out->push_back(c)) return false;
      ++at;
      continue;
    }
    // "[\x]" is the character x, whatever it is (a bracket, usually).
    if (at + 3 < text.size() && text[at + 1] == '\\' && text[at + 3] == ']') {
      if (!out->push_back(text[at + 2])) return false;
      at += 4;
      continue;
    }
    // The bracket that closes this one (brackets nest: "[[Folder]Name]").
    uint64_t close = at + 1;
    uint32_t open = 1;
    bool nested = false;
    for (; close < text.size(); ++close) {
      if (text[close] == '[' && (close == 0 || text[close - 1] != '\\')) ++open, nested = true;
      else if (text[close] == ']' && --open == 0) break;
    }
    if (close >= text.size()) {      // (no closing bracket: text)
      if (!out->push_back(c)) return false;
      ++at;
      continue;
    }
    const StringView inner = text.substr(at + 1, close - at - 1);
    String resolved(allocator);
    StringView key = inner;
    if (nested && depth < 8) {
      if (!format_into(allocator, inner, context, &resolved, depth + 1)) return false;
      key = resolved.view();
    }
    at = close + 1;
    String path(allocator);
    int64_t number = 0;
    bool plain = !key.empty();
    for (const char k : key) plain &= name_part(k);
    if (key.starts_with('%')) {
      // (an environment variable: none are set)
    } else if ((key.starts_with('#') || key.starts_with('!')) && context.file_path(key.without_prefix(1), &path)) {
      if (!out->append(path.view())) return false;
    } else if (key.starts_with('$') && context.component_folder(key.without_prefix(1), &path)) {
      if (!out->append(path.view())) return false;
    } else if (plain && !parse_integer(key, &number)) {
      if (!out->append(context.property(key))) return false;
    } else {      // "[~]", "[1]" and what is not a name at all stay as written
      if (!out->push_back('[') || !out->append(key) || !out->push_back(']')) return false;
    }
  }
  return true;
}

}  // namespace

bool evaluate_condition(StringView condition, const IExpressionContext& context, bool* result) {
  *result = true;
  if (condition.trimmed().empty()) return true;
  ConditionReader reader(condition, context);
  if (reader.read(result)) return true;
  *result = false;
  return false;
}

bool format_text(allocators::IAllocator& allocator, StringView text, const IExpressionContext& context, String* out) { return format_into(allocator, text, context, out, 0); }

}  // namespace msi

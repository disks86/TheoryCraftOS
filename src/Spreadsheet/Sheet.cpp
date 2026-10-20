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

#include "Spreadsheet/Sheet.hpp"
#include "Math/FloatMath.hpp"
#include <new>
#include "Utility/AllocatedView.hpp"

namespace spreadsheet {

namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

double power_of_ten(int32_t n) {
  static const double kTable[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  const bool negative = n < 0;
  uint32_t k = static_cast<uint32_t>(negative ? -n : n);
  double result = 1;
  while (k > 22) {
    result *= 1e22;
    k -= 22;
    if (result > 1e300) break;
  }
  result *= kTable[k > 22 ? 22 : k];
  return negative ? 1.0 / result : result;
}

double floor_of(double x) {
  if (x >= 9e18 || x <= -9e18) return x;
  int64_t t = static_cast<int64_t>(x);
  if (static_cast<double>(t) > x) --t;
  return static_cast<double>(t);
}

bool equal_text(StringView a, StringView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (upper(a[i]) != upper(b[i])) return false;
  return true;
}
}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Numbers and names

bool Sheet::parse_number(StringView text, double* out) {
  uint64_t i = 0, n = text.size();
  while (i < n && text[i] == ' ') ++i;
  while (n > i && text[n - 1] == ' ') --n;
  if (i >= n) return false;
  bool negative = false;
  if (text[i] == '-' || text[i] == '+') {
    negative = text[i] == '-';
    ++i;
  }
  uint64_t mantissa = 0;
  int32_t exponent = 0, digits = 0;
  bool any = false;
  for (; i < n && is_digit(text[i]); ++i) {
    any = true;
    if (digits < 19) {
      mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
      if (mantissa) ++digits;
    } else {
      ++exponent;   // more digits than a 64-bit integer holds: they only move the point
    }
  }
  if (i < n && text[i] == '.') {
    ++i;
    for (; i < n && is_digit(text[i]); ++i) {
      any = true;
      if (digits < 19) {
        mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
        if (mantissa) ++digits;
        --exponent;
      }
    }
  }
  if (!any) return false;
  if (i < n && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool exponent_negative = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) {
      exponent_negative = text[i] == '-';
      ++i;
    }
    if (i >= n || !is_digit(text[i])) return false;
    int32_t e = 0;
    for (; i < n && is_digit(text[i]); ++i)
      if (e < 10000) e = e * 10 + (text[i] - '0');
    exponent += exponent_negative ? -e : e;
  }
  if (i != n) return false;
  double value = static_cast<double>(mantissa);
  if (exponent > 308) value = value == 0 ? 0 : 1e308;
  else if (exponent < -330) value = 0;
  else if (exponent < 0 && exponent >= -22) value /= power_of_ten(-exponent);
  else if (exponent != 0) value *= power_of_ten(exponent);
  *out = negative ? -value : value;
  return true;
}

void Sheet::format_number(double v, String* out) {
  out->clear();
  if (v != v || v > 1e308 || v < -1e308) {
    out->assign("#NUM!");
    return;
  }
  if (v == 0) {
    out->assign("0");
    return;
  }
  if (v < 0) {
    out->push_back('-');
    v = -v;
  }
  int32_t e = 0;   // v = m * 10^e with 1 <= m < 10
  double m = v;
  while (m >= 1e16) { m /= 1e16; e += 16; }
  while (m >= 10) { m /= 10; ++e; }
  while (m < 1e-15) { m *= 1e16; e -= 16; }
  while (m < 1) { m *= 10; --e; }
  // Whole numbers show all their digits (up to 15), anything else ten significant digits: 1/3 reads 0.3333333333, and 0.1+0.2 reads 0.3.
  const bool whole = v == floor_of(v) && v < 1e15;
  const int32_t places = whole ? 15 : 10;
  uint64_t d = static_cast<uint64_t>(m * power_of_ten(places - 1) + 0.5);
  if (d >= static_cast<uint64_t>(power_of_ten(places))) {
    d /= 10;
    ++e;
  }
  char digits[16];
  for (int i = places - 1; i >= 0; --i) {
    digits[i] = static_cast<char>('0' + d % 10);
    d /= 10;
  }
  int32_t count = places;
  while (count > 1 && digits[count - 1] == '0') --count;
  if (e >= 15 || e < -5) {   // scientific
    out->push_back(digits[0]);
    if (count > 1) {
      out->push_back('.');
      out->append(StringView(digits + 1, static_cast<uint64_t>(count - 1)));
    }
    out->push_back('E');
    out->push_back(e < 0 ? '-' : '+');
    out->append_uint(static_cast<uint64_t>(e < 0 ? -e : e));
    return;
  }
  if (e >= 0) {
    for (int32_t i = 0; i <= e; ++i) out->push_back(i < count ? digits[i] : '0');
    if (count > e + 1) {
      out->push_back('.');
      out->append(StringView(digits + e + 1, static_cast<uint64_t>(count - e - 1)));
    }
  } else {
    out->append("0.");
    for (int32_t i = 0; i < -e - 1; ++i) out->push_back('0');
    out->append(StringView(digits, static_cast<uint64_t>(count)));
  }
}

const char* Sheet::error_text(ErrorKind error) {
  switch (error) {
    case ErrorKind::DivideByZero: return "#DIV/0!";
    case ErrorKind::Value: return "#VALUE!";
    case ErrorKind::Reference: return "#REF!";
    case ErrorKind::Name: return "#NAME?";
    case ErrorKind::Number: return "#NUM!";
    case ErrorKind::Cycle: return "#CYCLE!";
    case ErrorKind::Syntax: return "#ERR!";
    default: return "#ERR!";
  }
}

void Sheet::column_name(uint32_t column, String* out) {
  out->clear();
  if (column >= 26) out->push_back(static_cast<char>('A' + column / 26 - 1));
  out->push_back(static_cast<char>('A' + column % 26));
}

bool Sheet::parse_reference(StringView text, uint32_t* column, uint32_t* row) {
  uint64_t i = 0;
  if (i < text.size() && text[i] == '$') ++i;
  uint32_t letters = 0, c = 0;
  while (i < text.size() && is_letter(text[i]) && letters < 3) {
    c = c * 26 + static_cast<uint32_t>(upper(text[i]) - 'A' + 1);
    ++letters;
    ++i;
  }
  if (letters == 0 || letters > 2) return false;
  if (i < text.size() && text[i] == '$') ++i;
  uint32_t r = 0, digits = 0;
  while (i < text.size() && is_digit(text[i]) && digits < 6) {
    r = r * 10 + static_cast<uint32_t>(text[i] - '0');
    ++digits;
    ++i;
  }
  if (digits == 0 || i != text.size() || r == 0 || r > kRows || c == 0 || c > kColumns) return false;
  *column = c - 1;
  *row = r - 1;
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Formulas

class FormulaParser {
 public:
  FormulaParser(Sheet& sheet, StringView source) : sheet_(sheet), a_(sheet.allocator_), src_(source) {}

  Value parse_all() {
    Value v = expression();
    blanks();
    if (pos_ < src_.size() || syntax_) return error(ErrorKind::Syntax);
    if (v.kind == ValueKind::Range) return error(ErrorKind::Value);
    return v;
  }

 private:
  static constexpr uint32_t kMaxDepth = 40;

  Value empty() { return Value(a_); }
  Value number(double d) {
    Value v(a_);
    v.kind = ValueKind::Number;
    v.number = d;
    return v;
  }
  Value error(ErrorKind e) {
    Value v(a_);
    v.kind = ValueKind::Error;
    v.error = e;
    return v;
  }
  Value text(StringView s) {
    Value v(a_);
    v.kind = ValueKind::Text;
    v.text.assign(s);
    return v;
  }
  Value copy(const Value& from) {
    Value v(a_);
    v.kind = from.kind;
    v.number = from.number;
    v.error = from.error;
    v.text.assign(from.text.view());
    v.c0 = from.c0; v.r0 = from.r0; v.c1 = from.c1; v.r1 = from.r1;
    return v;
  }

  void blanks() {
    while (pos_ < src_.size() && src_[pos_] == ' ') ++pos_;
  }
  char peek() {
    blanks();
    return pos_ < src_.size() ? src_[pos_] : '\0';
  }
  bool eat(char c) {
    if (peek() != c) return false;
    ++pos_;
    return true;
  }

  // What a value is as a number: Number, an empty cell (0), text that reads as a number. Anything else is the error to give.
  ErrorKind as_number(const Value& v, double* out) {
    switch (v.kind) {
      case ValueKind::Number: *out = v.number; return ErrorKind::None;
      case ValueKind::Empty: *out = 0; return ErrorKind::None;
      case ValueKind::Text: return Sheet::parse_number(v.text.view(), out) ? ErrorKind::None : ErrorKind::Value;
      case ValueKind::Error: return v.error;
      default: return ErrorKind::Value;
    }
  }
  void as_text(const Value& v, String* out) {
    if (v.kind == ValueKind::Number) Sheet::format_number(v.number, out);
    else if (v.kind == ValueKind::Text) out->assign(v.text.view());
    else out->clear();
  }

  Value expression() {
    if (++depth_ > kMaxDepth) {
      syntax_ = true;
      --depth_;
      return error(ErrorKind::Syntax);
    }
    Value left = concatenation();
    for (;;) {
      blanks();
      if (pos_ >= src_.size()) break;
      const char c = src_[pos_];
      int op = 0;   // 1 =, 2 <>, 3 <, 4 >, 5 <=, 6 >=
      uint32_t length = 1;
      if (c == '=') op = 1;
      else if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') { op = 2; length = 2; }
      else if (c == '<' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { op = 5; length = 2; }
      else if (c == '<') op = 3;
      else if (c == '>' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') { op = 6; length = 2; }
      else if (c == '>') op = 4;
      if (!op) break;
      pos_ += length;
      Value right = concatenation();
      left = compare(op, left, right);
    }
    --depth_;
    return left;
  }

  Value compare(int op, const Value& l, const Value& r) {
    if (skip_) return number(0);
    if (l.kind == ValueKind::Error) return copy(l);
    if (r.kind == ValueKind::Error) return copy(r);
    if (l.kind == ValueKind::Range || r.kind == ValueKind::Range) return error(ErrorKind::Value);
    int order;
    if (l.kind == ValueKind::Text && r.kind == ValueKind::Text) {
      order = 0;
      const uint64_t n = l.text.size() < r.text.size() ? l.text.size() : r.text.size();
      for (uint64_t i = 0; i < n && order == 0; ++i) {
        const char a = upper(l.text[i]), b = upper(r.text[i]);
        order = a < b ? -1 : a > b ? 1 : 0;
      }
      if (order == 0) order = l.text.size() < r.text.size() ? -1 : l.text.size() > r.text.size() ? 1 : 0;
    } else if (l.kind == ValueKind::Text || r.kind == ValueKind::Text) {
      order = l.kind == ValueKind::Text ? 1 : -1;   // numbers sort before text
      if (l.kind == ValueKind::Empty || r.kind == ValueKind::Empty) order = l.kind == ValueKind::Text ? (l.text.empty() ? 0 : 1) : (r.text.empty() ? 0 : -1);
    } else {
      const double a = l.kind == ValueKind::Number ? l.number : 0, b = r.kind == ValueKind::Number ? r.number : 0;
      const double scale = (a < 0 ? -a : a) > (b < 0 ? -b : b) ? (a < 0 ? -a : a) : (b < 0 ? -b : b);
      const double diff = a - b;
      order = (diff < 0 ? -diff : diff) <= scale * 1e-12 ? 0 : (a < b ? -1 : 1);
    }
    bool result = false;
    switch (op) {
      case 1: result = order == 0; break;
      case 2: result = order != 0; break;
      case 3: result = order < 0; break;
      case 4: result = order > 0; break;
      case 5: result = order <= 0; break;
      default: result = order >= 0; break;
    }
    return number(result ? 1 : 0);
  }

  Value concatenation() {
    Value left = additive();
    while (peek() == '&') {
      ++pos_;
      Value right = additive();
      if (skip_) { left = number(0); continue; }
      if (left.kind == ValueKind::Error) continue;
      if (right.kind == ValueKind::Error) { left = copy(right); continue; }
      if (left.kind == ValueKind::Range || right.kind == ValueKind::Range) { left = error(ErrorKind::Value); continue; }
      String joined(a_), part(a_);
      as_text(left, &joined);
      as_text(right, &part);
      joined.append(part.view());
      left = text(joined.view());
    }
    return left;
  }

  Value arithmetic(char op, const Value& l, const Value& r) {
    if (skip_) return number(0);
    double a, b;
    ErrorKind e = as_number(l, &a);
    if (e == ErrorKind::None) e = as_number(r, &b);
    if (e != ErrorKind::None) return error(e);
    switch (op) {
      case '+': return number(a + b);
      case '-': return number(a - b);
      case '*': return number(a * b);
      case '/': return b == 0 ? error(ErrorKind::DivideByZero) : number(a / b);
      default: return raise(a, b);
    }
  }

  Value raise(double a, double b) {
    const bool integer = b == floor_of(b) && b >= -1000 && b <= 1000;
    if (integer) {   // by repeated squaring: exact for the usual cases, and defined for a negative base
      if (a == 0 && b < 0) return error(ErrorKind::DivideByZero);
      uint32_t k = static_cast<uint32_t>(b < 0 ? -b : b);
      double result = 1, base = a;
      while (k) {
        if (k & 1) result *= base;
        base *= base;
        k >>= 1;
      }
      return number(b < 0 ? 1.0 / result : result);
    }
    if (a < 0) return error(ErrorKind::Number);
    if (a == 0) return number(b > 0 ? 0 : 1);
    return number(fmath::pow(a, b));
  }

  Value additive() {
    Value left = multiplicative();
    for (;;) {
      const char c = peek();
      if (c != '+' && c != '-') break;
      ++pos_;
      Value right = multiplicative();
      left = arithmetic(c, left, right);
    }
    return left;
  }

  Value multiplicative() {
    Value left = power();
    for (;;) {
      const char c = peek();
      if (c != '*' && c != '/') break;
      ++pos_;
      Value right = power();
      left = arithmetic(c, left, right);
    }
    return left;
  }

  Value power() {
    Value base = unary();
    while (peek() == '^') {
      ++pos_;
      Value exponent = unary();
      base = arithmetic('^', base, exponent);
    }
    return base;
  }

  Value unary() {
    const char c = peek();
    if (c == '-' || c == '+') {
      ++pos_;
      if (++depth_ > kMaxDepth) {
        syntax_ = true;
        --depth_;
        return error(ErrorKind::Syntax);
      }
      Value v = unary();
      --depth_;
      if (c == '+' || skip_) return v;
      double d;
      const ErrorKind e = as_number(v, &d);
      return e != ErrorKind::None ? error(e) : number(-d);
    }
    return primary();
  }

  // A cell's value, or Pending when the sheet has to work it out first.
  Value cell_value(uint32_t column, uint32_t row) {
    Value v(a_);
    if (skip_) {
      v.kind = ValueKind::Number;
      return v;
    }
    sheet_.value_of(column, row, &v);
    return v;
  }

  bool reference_at(uint32_t* column, uint32_t* row, bool* ok) {
    // The text from pos_ that looks like a cell reference: $? letters $? digits.
    uint64_t i = pos_;
    while (i < src_.size() && (src_[i] == '$' || is_letter(src_[i]) || is_digit(src_[i]) || src_[i] == '_')) ++i;
    if (i == pos_) return false;
    const StringView word = src_.substr(pos_, i - pos_);
    *ok = Sheet::parse_reference(word, column, row);
    pos_ = i;
    return true;
  }

  Value primary() {
    const char c = peek();
    if (c == '\0') {
      syntax_ = true;
      return error(ErrorKind::Syntax);
    }
    if (c == '(') {
      ++pos_;
      Value v = expression();
      if (!eat(')')) syntax_ = true;
      return v;
    }
    if (c == '"') {
      ++pos_;
      String s(a_);
      for (;;) {
        if (pos_ >= src_.size()) {
          syntax_ = true;
          return error(ErrorKind::Syntax);
        }
        const char ch = src_[pos_++];
        if (ch == '"') {
          if (pos_ < src_.size() && src_[pos_] == '"') {   // "" is a quote inside the text
            s.push_back('"');
            ++pos_;
            continue;
          }
          break;
        }
        s.push_back(ch);
      }
      return text(s.view());
    }
    if (is_digit(c) || c == '.') {
      uint64_t i = pos_;
      while (i < src_.size() && (is_digit(src_[i]) || src_[i] == '.')) ++i;
      if (i < src_.size() && (src_[i] == 'e' || src_[i] == 'E')) {
        uint64_t j = i + 1;
        if (j < src_.size() && (src_[j] == '+' || src_[j] == '-')) ++j;
        if (j < src_.size() && is_digit(src_[j])) {
          while (j < src_.size() && is_digit(src_[j])) ++j;
          i = j;
        }
      }
      double d;
      if (!Sheet::parse_number(src_.substr(pos_, i - pos_), &d)) {
        syntax_ = true;
        return error(ErrorKind::Syntax);
      }
      pos_ = i;
      return number(d);
    }
    if (is_letter(c) || c == '$' || c == '_') {
      const uint64_t start = pos_;
      uint64_t i = pos_;
      while (i < src_.size() && (is_letter(src_[i]) || is_digit(src_[i]) || src_[i] == '$' || src_[i] == '_' || src_[i] == '.')) ++i;
      const StringView word = src_.substr(start, i - start);
      if (i < src_.size() && src_[i] == '(') {
        pos_ = i + 1;
        return function(word);
      }
      uint32_t column, row;
      if (Sheet::parse_reference(word, &column, &row)) {
        pos_ = i;
        if (peek() == ':') {
          ++pos_;
          blanks();
          uint32_t c2, r2;
          bool ok = false;
          if (!reference_at(&c2, &r2, &ok)) {
            syntax_ = true;
            return error(ErrorKind::Syntax);
          }
          if (!ok) return error(ErrorKind::Reference);
          Value range(a_);
          range.kind = ValueKind::Range;
          range.c0 = column < c2 ? column : c2;
          range.c1 = column < c2 ? c2 : column;
          range.r0 = row < r2 ? row : r2;
          range.r1 = row < r2 ? r2 : row;
          return range;
        }
        return cell_value(column, row);
      }
      pos_ = i;
      if (equal_text(word, "TRUE")) return number(1);
      if (equal_text(word, "FALSE")) return number(0);
      if (word.size() >= 2 && word[0] == '#') return error(ErrorKind::Syntax);
      return error(ErrorKind::Name);
    }
    if (c == '#') {   // an error typed or copied into a formula
      pos_ = src_.size();
      return error(ErrorKind::Reference);
    }
    syntax_ = true;
    return error(ErrorKind::Syntax);
  }

  // --- Functions. The name is read, the opening bracket is consumed.
  // Folds the numbers of one argument (a range or a single value) into the totals; the first error wins.
  struct Totals {
    double sum = 0, min = 0, max = 0;
    uint32_t count = 0;
    ErrorKind error = ErrorKind::None;
  };

  void fold(Totals* t, double d) {
    if (t->count == 0 || d < t->min) t->min = d;
    if (t->count == 0 || d > t->max) t->max = d;
    t->sum += d;
    ++t->count;
  }

  void fold_argument(Totals* t, const Value& v) {
    if (t->error != ErrorKind::None || skip_) return;
    if (v.kind == ValueKind::Error) {
      t->error = v.error;
    } else if (v.kind == ValueKind::Range) {
      for (uint32_t r = v.r0; r <= v.r1; ++r) {
        for (uint32_t c = v.c0; c <= v.c1; ++c) {
          Value cv(a_);
          sheet_.value_of(c, r, &cv);   // (queues the cells that are not worked out; the value then says Pending)
          if (cv.kind == ValueKind::Error) {
            if (t->error == ErrorKind::None || cv.error == ErrorKind::Pending) t->error = cv.error;
          } else if (cv.kind == ValueKind::Number) {
            fold(t, cv.number);
          }
        }
      }
    } else if (v.kind == ValueKind::Number) {
      fold(t, v.number);
    } else if (v.kind == ValueKind::Text) {
      double d;
      if (Sheet::parse_number(v.text.view(), &d)) fold(t, d);
      else t->error = ErrorKind::Value;
    }
  }

  // Reads one argument; the sheet marks any cell it needs.
  Value argument() { return expression(); }

  bool close() {
    if (eat(')')) return true;
    syntax_ = true;
    return false;
  }

  Value one_number_function(StringView name, const Value& arg) {
    if (skip_) return number(0);
    double d;
    const ErrorKind e = as_number(arg, &d);
    if (e != ErrorKind::None) return error(e);
    if (equal_text(name, "ABS")) return number(d < 0 ? -d : d);
    if (equal_text(name, "INT")) return number(floor_of(d));
    if (equal_text(name, "SQRT")) return d < 0 ? error(ErrorKind::Number) : number(fmath::sqrt(d));
    return number(d == 0 ? 1 : 0);   // NOT
  }

  Value function(StringView name) {
    if (++depth_ > kMaxDepth) {
      syntax_ = true;
      --depth_;
      return error(ErrorKind::Syntax);
    }
    Value result = function_body(name);
    --depth_;
    return result;
  }

  Value function_body(StringView name) {
    const bool aggregate = equal_text(name, "SUM") || equal_text(name, "AVERAGE") || equal_text(name, "MIN") || equal_text(name, "MAX") || equal_text(name, "COUNT");
    if (aggregate) {
      Totals totals;
      if (peek() != ')') {
        do {
          Value v = argument();
          fold_argument(&totals, v);
        } while (eat(','));
      }
      if (!close()) return error(ErrorKind::Syntax);
      if (skip_) return number(0);
      if (totals.error != ErrorKind::None) return error(totals.error);
      if (equal_text(name, "SUM")) return number(totals.sum);
      if (equal_text(name, "COUNT")) return number(totals.count);
      if (equal_text(name, "AVERAGE")) return totals.count ? number(totals.sum / totals.count) : error(ErrorKind::DivideByZero);
      if (equal_text(name, "MIN")) return number(totals.count ? totals.min : 0);
      return number(totals.count ? totals.max : 0);
    }
    if (equal_text(name, "IF")) {
      Value condition = argument();
      double truth = 0;
      ErrorKind e = ErrorKind::None;
      if (!skip_) {
        if (condition.kind == ValueKind::Number) truth = condition.number;
        else if (condition.kind == ValueKind::Empty) truth = 0;
        else e = condition.kind == ValueKind::Error ? condition.error : ErrorKind::Value;
      }
      const bool outer_skip = skip_;
      Value chosen = empty();
      if (!eat(',')) { syntax_ = true; return error(ErrorKind::Syntax); }
      skip_ = outer_skip || e != ErrorKind::None || truth == 0;
      Value yes = argument();
      skip_ = outer_skip;
      Value no = number(0);
      bool has_else = false;
      if (eat(',')) {
        has_else = true;
        skip_ = outer_skip || e != ErrorKind::None || truth != 0;
        no = argument();
        skip_ = outer_skip;
      }
      if (!close()) return error(ErrorKind::Syntax);
      if (skip_) return number(0);
      if (e != ErrorKind::None) return error(e);
      if (truth != 0) return yes.kind == ValueKind::Range ? error(ErrorKind::Value) : copy(yes);
      if (!has_else) return number(0);   // (FALSE)
      return no.kind == ValueKind::Range ? error(ErrorKind::Value) : copy(no);
    }
    if (equal_text(name, "ABS") || equal_text(name, "INT") || equal_text(name, "SQRT") || equal_text(name, "NOT")) {
      Value arg = argument();
      if (!close()) return error(ErrorKind::Syntax);
      return one_number_function(name, arg);
    }
    if (equal_text(name, "LEN")) {
      Value arg = argument();
      if (!close()) return error(ErrorKind::Syntax);
      if (skip_) return number(0);
      if (arg.kind == ValueKind::Error) return copy(arg);
      if (arg.kind == ValueKind::Range) return error(ErrorKind::Value);
      String s(a_);
      as_text(arg, &s);
      return number(static_cast<double>(s.size()));
    }
    if (equal_text(name, "ROUND") || equal_text(name, "MOD") || equal_text(name, "POWER")) {
      Value x = argument();
      Value y = empty();
      if (eat(',')) y = argument();
      else syntax_ = true;
      if (!close()) return error(ErrorKind::Syntax);
      if (skip_) return number(0);
      double a, b;
      ErrorKind e = as_number(x, &a);
      if (e == ErrorKind::None) e = as_number(y, &b);
      if (e != ErrorKind::None) return error(e);
      if (equal_text(name, "POWER")) return raise(a, b);
      if (equal_text(name, "MOD")) {
        if (b == 0) return error(ErrorKind::DivideByZero);
        return number(a - b * floor_of(a / b));
      }
      int32_t digits = static_cast<int32_t>(b < -15 ? -15 : b > 15 ? 15 : floor_of(b));
      const double scale = power_of_ten(digits);
      const double scaled = a * scale;
      if (scaled >= 9e18 || scaled <= -9e18) return number(a);
      const double rounded = scaled < 0 ? -floor_of(-scaled + 0.5) : floor_of(scaled + 0.5);
      return number(rounded / scale);
    }
    if (equal_text(name, "AND") || equal_text(name, "OR")) {
      const bool is_and = equal_text(name, "AND");
      bool result = is_and;
      ErrorKind e = ErrorKind::None;
      if (peek() != ')') {
        do {
          Value v = argument();
          if (skip_) continue;
          double d;
          const ErrorKind ve = v.kind == ValueKind::Range ? ErrorKind::Value : as_number(v, &d);
          if (ve != ErrorKind::None) {
            if (e == ErrorKind::None) e = ve;
          } else if (is_and) {
            result = result && d != 0;
          } else {
            result = result || d != 0;
          }
        } while (eat(','));
      }
      if (!close()) return error(ErrorKind::Syntax);
      if (skip_) return number(0);
      return e != ErrorKind::None ? error(e) : number(result ? 1 : 0);
    }
    // An unknown function: its arguments are read (to find the end) and ignored.
    if (peek() != ')') {
      do {
        argument();
      } while (eat(','));
    }
    if (!close()) return error(ErrorKind::Syntax);
    return error(ErrorKind::Name);
  }

  Sheet& sheet_;
  allocators::IAllocator& a_;
  StringView src_;
  uint64_t pos_ = 0;
  bool skip_ = false;
  bool syntax_ = false;
  uint32_t depth_ = 0;
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// The sheet

Sheet::Sheet(allocators::IAllocator& allocator) : allocator_(allocator), pending_(allocator) {
  ArrayView<Cell*> view = allocate_view<Cell*>(allocator, kColumns * kRows);
  cells_ = view.data();
  for (uint64_t i = 0; i < view.size(); ++i) cells_[i] = nullptr;
}

Sheet::~Sheet() {
  if (cells_) {
    for (uint32_t i = 0; i < kColumns * kRows; ++i) {
      if (cells_[i]) {
        cells_[i]->~Cell();
        allocator_.deallocate(cells_[i]);
      }
    }
    allocator_.deallocate(cells_);
  }
}

Sheet::Cell* Sheet::make_cell(uint32_t column, uint32_t row) {
  if (!cells_ || column >= kColumns || row >= kRows) return nullptr;
  Cell*& slot = cells_[row * kColumns + column];
  if (!slot) {
    void* memory = allocator_.allocate(sizeof(Cell));
    if (!memory) return nullptr;
    slot = new (memory) Cell(allocator_);
  }
  return slot;
}

bool Sheet::set_text(uint32_t column, uint32_t row, StringView typed) {
  if (column >= kColumns || row >= kRows || !cells_) return false;
  Cell* c = cell(column, row);
  if (!c) {
    if (typed.empty()) return true;
    c = make_cell(column, row);
    if (!c) return false;
  }
  if (c->typed.size() == typed.size()) {   // nothing changes: nothing is recalculated
    bool same = true;
    for (uint64_t i = 0; i < typed.size() && same; ++i) same = c->typed[i] == typed[i];
    if (same) return true;
  }
  if (!c->typed.assign(typed)) return false;
  changed();
  return true;
}

StringView Sheet::text(uint32_t column, uint32_t row) const {
  const Cell* c = cell(column, row);
  return c ? c->typed.view() : StringView();
}

bool Sheet::is_valid(uint32_t column, uint32_t row) const {
  const Cell* c = cell(column, row);
  return !c || c->typed.empty() || c->generation == generation_;
}

void Sheet::queue(uint32_t column, uint32_t row) {
  const uint32_t index = row * kColumns + column;
  for (uint64_t i = 0; i < pending_.count(); ++i)
    if (pending_[i] == index) return;
  pending_.push_back(index);
}

void Sheet::value_of(uint32_t column, uint32_t row, Value* out) {
  out->kind = ValueKind::Empty;
  out->text.clear();
  Cell* c = cell(column, row);
  if (!c || c->typed.empty()) return;
  if (c->generation == generation_) {
    out->kind = c->kind;
    out->number = c->number;
    out->error = c->error;
    out->text.assign(c->text.view());
    return;
  }
  out->kind = ValueKind::Error;
  if (c->evaluating) {   // it is being worked out right now: this formula leads back to itself
    out->error = ErrorKind::Cycle;
    return;
  }
  queue(column, row);
  out->error = ErrorKind::Pending;
}

// One run of the formula in a cell. Returns false when it had to give up because a cell it reads is not worked out yet.
bool Sheet::evaluate(Cell* c) {
  Value v(allocator_);
  const StringView typed = c->typed.view();
  if (typed.empty()) {
    v.kind = ValueKind::Empty;
  } else if (typed[0] == '=' && typed.size() > 1) {
    FormulaParser parser(*this, typed.substr(1));
    v = parser.parse_all();
    if (v.kind == ValueKind::Error && v.error == ErrorKind::Pending) return false;
    if (v.kind == ValueKind::Empty) {   // (a reference to an empty cell shows as 0)
      v.kind = ValueKind::Number;
      v.number = 0;
    }
  } else {
    double d;
    if (parse_number(typed, &d)) {
      v.kind = ValueKind::Number;
      v.number = d;
    } else {
      v.kind = ValueKind::Text;
      v.text.assign(typed);
    }
  }
  c->kind = v.kind;
  c->number = v.number;
  c->error = v.error;
  c->text.assign(v.text.view());
  return true;
}

// Works out a cell by keeping the cells to do on a list of our own, not by calling down into each cell a formula reads: a long chain of cells would
// otherwise use as much stack as it is long.
bool Sheet::make_valid(uint32_t column, uint32_t row) {
  if (is_valid(column, row)) return true;
  List<uint32_t> stack(allocator_);
  auto begin = [&](uint32_t index) {
    cells_[index]->evaluating = true;
    return stack.push_back(index);
  };
  if (!begin(row * kColumns + column)) return false;
  bool ok = true;
  while (stack.count() && ok) {
    const uint32_t index = stack[stack.count() - 1];
    Cell* c = cells_[index];
    pending_.clear();
    if (evaluate(c)) {
      c->generation = generation_;
      c->evaluating = false;
      stack.remove_at(stack.count() - 1);
      continue;
    }
    // It waits for the cells in pending_: they go on top, and the formula runs again once they are done.
    for (uint64_t i = 0; i < pending_.count() && ok; ++i) {
      const uint32_t p = pending_[i];
      if (cells_[p]->generation != generation_ && !cells_[p]->evaluating) ok = begin(p);
    }
    if (pending_.count() == 0) ok = false;   // (cannot happen: it said it was waiting)
  }
  for (uint64_t i = 0; i < stack.count(); ++i) cells_[stack[i]]->evaluating = false;   // out of memory: leave nothing marked
  pending_.clear();
  return ok;
}

bool Sheet::display(uint32_t column, uint32_t row, String* out, bool* right) {
  out->clear();
  if (right) *right = false;
  const Cell* c = cell(column, row);
  if (!c || c->typed.empty()) return true;
  if (!make_valid(column, row)) return false;
  switch (c->kind) {
    case ValueKind::Number:
      format_number(c->number, out);
      if (right) *right = true;
      return true;
    case ValueKind::Text: return out->assign(c->text.view());
    case ValueKind::Error: return out->assign(error_text(c->error));
    default: return true;
  }
}

bool Sheet::number(uint32_t column, uint32_t row, double* out) {
  const Cell* c = cell(column, row);
  if (!c || c->typed.empty() || !make_valid(column, row)) return false;
  if (c->kind != ValueKind::Number) return false;
  *out = c->number;
  return true;
}

void Sheet::clear() {
  if (!cells_) return;
  bool any = false;
  for (uint32_t i = 0; i < kColumns * kRows; ++i) {
    if (cells_[i]) {
      cells_[i]->~Cell();
      allocator_.deallocate(cells_[i]);
      cells_[i] = nullptr;
      any = true;
    }
  }
  if (any) changed();
}

void Sheet::clear_range(uint32_t c0, uint32_t r0, uint32_t c1, uint32_t r1) {
  for (uint32_t r = r0; r <= r1 && r < kRows; ++r)
    for (uint32_t c = c0; c <= c1 && c < kColumns; ++c)
      if (cell(c, r)) set_text(c, r, StringView());
}

void Sheet::used_extent(uint32_t* columns, uint32_t* rows) const {
  uint32_t max_c = 0, max_r = 0;
  for (uint32_t r = 0; r < kRows; ++r) {
    for (uint32_t c = 0; c < kColumns; ++c) {
      const Cell* cl = cells_ ? cells_[r * kColumns + c] : nullptr;
      if (cl && !cl->typed.empty()) {
        if (c + 1 > max_c) max_c = c + 1;
        if (r + 1 > max_r) max_r = r + 1;
      }
    }
  }
  *columns = max_c;
  *rows = max_r;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// CSV

bool Sheet::load_csv(StringView t, uint32_t* dropped) {
  clear();
  uint32_t lost = 0;
  uint32_t column = 0, row = 0;
  String field(allocator_);
  bool quoted = false, in_quotes = false, field_started = false, ok = true;
  auto finish_field = [&]() {
    if (column < kColumns && row < kRows) {
      if (!field.empty() && !set_text(column, row, field.view())) ok = false;
    } else if (!field.empty()) {
      ++lost;
    }
    field.clear();
    quoted = field_started = false;
  };
  for (uint64_t i = 0; i < t.size() && ok; ++i) {
    const char ch = t[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < t.size() && t[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(ch);
      }
      continue;
    }
    if (ch == '"' && !field_started) {
      in_quotes = quoted = field_started = true;
    } else if (ch == ',') {
      finish_field();
      ++column;
    } else if (ch == '\n' || ch == '\r') {
      if (ch == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
      finish_field();
      column = 0;
      ++row;
    } else {
      field_started = true;
      field.push_back(ch);
    }
  }
  if (field_started || !field.empty() || column > 0) finish_field();
  (void)quoted;
  modified_ = false;
  if (dropped) *dropped = lost;
  return ok;
}

bool Sheet::save_csv(String* out) const {
  out->clear();
  uint32_t columns, rows;
  used_extent(&columns, &rows);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < columns; ++c) {
      if (c > 0 && !out->push_back(',')) return false;
      const StringView v = text(c, r);
      bool quote = false;
      for (uint64_t i = 0; i < v.size(); ++i) {
        const char ch = v[i];
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') quote = true;
      }
      if (!v.empty() && (v[0] == ' ' || v[v.size() - 1] == ' ')) quote = true;
      if (!quote) {
        if (!out->append(v)) return false;
        continue;
      }
      if (!out->push_back('"')) return false;
      for (uint64_t i = 0; i < v.size(); ++i) {
        if (v[i] == '"' && !out->push_back('"')) return false;
        if (!out->push_back(v[i])) return false;
      }
      if (!out->push_back('"')) return false;
    }
    if (!out->push_back('\n')) return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Moving references (pasting a formula somewhere else)

bool Sheet::shift_references(StringView f, int32_t dc, int32_t dr, String* out) {
  out->clear();
  uint64_t i = 0;
  while (i < f.size()) {
    const char ch = f[i];
    if (ch == '"') {   // text is copied as it is
      uint64_t j = i + 1;
      while (j < f.size()) {
        if (f[j] == '"') {
          if (j + 1 < f.size() && f[j + 1] == '"') {
            j += 2;
            continue;
          }
          ++j;
          break;
        }
        ++j;
      }
      if (!out->append(f.substr(i, j - i))) return false;
      i = j;
      continue;
    }
    if (is_letter(ch) || ch == '$' || ch == '_') {
      uint64_t j = i;
      while (j < f.size() && (is_letter(f[j]) || is_digit(f[j]) || f[j] == '$' || f[j] == '_' || f[j] == '.')) ++j;
      const StringView word = f.substr(i, j - i);
      uint32_t c, r;
      if ((j >= f.size() || f[j] != '(') && parse_reference(word, &c, &r)) {
        const bool column_fixed = word[0] == '$';
        uint64_t k = column_fixed ? 1 : 0;
        while (k < word.size() && is_letter(word[k])) ++k;
        const bool row_fixed = k < word.size() && word[k] == '$';
        const int64_t nc = column_fixed ? static_cast<int64_t>(c) : static_cast<int64_t>(c) + dc;
        const int64_t nr = row_fixed ? static_cast<int64_t>(r) : static_cast<int64_t>(r) + dr;
        if (nc < 0 || nc >= static_cast<int64_t>(kColumns) || nr < 0 || nr >= static_cast<int64_t>(kRows)) {
          if (!out->append("#REF!")) return false;
        } else {
          if (column_fixed && !out->push_back('$')) return false;
          if (nc >= 26 && !out->push_back(static_cast<char>('A' + nc / 26 - 1))) return false;
          if (!out->push_back(static_cast<char>('A' + nc % 26))) return false;
          if (row_fixed && !out->push_back('$')) return false;
          if (!out->append_uint(static_cast<uint64_t>(nr + 1))) return false;
        }
        i = j;
        continue;
      }
      if (!out->append(word)) return false;
      i = j;
      continue;
    }
    if (!out->push_back(ch)) return false;
    ++i;
  }
  return true;
}

}  // namespace spreadsheet

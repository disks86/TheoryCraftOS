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

#include "Database/Local/LocalSql.hpp"
#include "Database/NumberText.hpp"
#include "Utility/Move.hpp"

namespace database::local {

namespace {

using allocators::IAllocator;

char lc(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c) || c == '$'; }

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Words

enum class Tk : uint8_t { End, Ident, Quoted, Number, String, Param, Punct, Bad };

struct Token {
  Tk kind = Tk::End;
  StringView text;   // without the quotes of a string or a quoted name
  uint64_t pos = 0;
};

Token lex(StringView sql, uint64_t* p) {
  uint64_t i = *p;
  const uint64_t n = sql.size();
  for (;;) {
    while (i < n && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n')) ++i;
    if (i + 1 < n && sql[i] == '-' && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    if (i + 1 < n && sql[i] == '/' && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      i = i + 1 < n ? i + 2 : n;
      continue;
    }
    break;
  }
  Token t;
  t.pos = i;
  if (i >= n) {
    *p = i;
    return t;
  }
  const char c = sql[i];
  if (is_ident_start(c)) {
    uint64_t j = i + 1;
    while (j < n && is_ident_char(sql[j])) ++j;
    t.kind = Tk::Ident;
    t.text = sql.substr(i, j - i);
    *p = j;
  } else if (c == '"' || c == '\'') {
    uint64_t j = i + 1;
    for (;;) {
      while (j < n && sql[j] != c) ++j;
      if (j >= n) break;
      if (c == '\'' && j + 1 < n && sql[j + 1] == '\'') {   // a doubled quote is a quote in the text
        j += 2;
        continue;
      }
      break;
    }
    if (j >= n) {
      t.kind = Tk::Bad;
      t.text = sql.substr(i, 1);
      *p = n;
    } else {
      t.kind = c == '"' ? Tk::Quoted : Tk::String;
      t.text = sql.substr(i + 1, j - i - 1);
      *p = j + 1;
    }
  } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(sql[i + 1]))) {
    uint64_t j = i;
    while (j < n && is_digit(sql[j])) ++j;
    if (j < n && sql[j] == '.') {
      ++j;
      while (j < n && is_digit(sql[j])) ++j;
    }
    if (j < n && (sql[j] == 'e' || sql[j] == 'E')) {
      uint64_t k = j + 1;
      if (k < n && (sql[k] == '+' || sql[k] == '-')) ++k;
      if (k < n && is_digit(sql[k])) {
        while (k < n && is_digit(sql[k])) ++k;
        j = k;
      }
    }
    t.kind = Tk::Number;
    t.text = sql.substr(i, j - i);
    *p = j;
  } else if (c == '?') {
    t.kind = Tk::Param;
    t.text = sql.substr(i, 1);
    *p = i + 1;
  } else {
    t.kind = Tk::Punct;
    uint64_t len = 1;
    if (i + 1 < n) {
      const char d = sql[i + 1];
      if ((c == '<' && (d == '=' || d == '>')) || (c == '>' && d == '=') || (c == '!' && d == '=') || (c == '|' && d == '|') || (c == '=' && d == '=')) len = 2;
    }
    t.text = sql.substr(i, len);
    const char* const kPunct = "(),;*+-/%=<>.";
    bool known = len == 2;
    for (const char* q = kPunct; *q && !known; ++q) known = *q == c;
    if (!known) t.kind = Tk::Bad;
    *p = i + len;
  }
  return t;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Expressions

enum class Op : uint8_t { Literal, Column, Param, Neg, Not, Add, Sub, Mul, Div, Mod, Concat, Eq, Ne, Lt, Le, Gt, Ge, And, Or, IsNull, NotNull, Like, NotLike, Func, Count, Sum, Avg, Min, Max };
enum class Fn : uint8_t { Coalesce, Lower, Upper, Length, Abs };

struct Expr {
  explicit Expr(IAllocator& allocator) : literal(allocator), name(allocator) {}
  Expr(Expr&&) = default;
  Expr& operator=(Expr&&) = default;
  Op op = Op::Literal;
  int32_t a = -1, b = -1;
  int32_t args[4] = {-1, -1, -1, -1};
  uint8_t argc = 0;
  bool star = false;
  Fn fn = Fn::Coalesce;
  uint32_t column = 0;
  uint32_t param = 0;
  Value literal;
  String name;   // a column's name
};

bool is_aggregate(Op op) { return op == Op::Count || op == Op::Sum || op == Op::Avg || op == Op::Min || op == Op::Max; }
bool is_boolean_op(Op op) {
  switch (op) {
    case Op::Not: case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::And: case Op::Or: case Op::IsNull: case Op::NotNull: case Op::Like: case Op::NotLike:
      return true;
    default: return false;
  }
}

DbType type_of_kind(Value::Kind kind) {
  switch (kind) {
    case Value::Kind::Integer: return DbType::Integer;
    case Value::Kind::Real: return DbType::Real;
    case Value::Kind::Text: return DbType::Text;
    case Value::Kind::Bytes: return DbType::Bytes;
    case Value::Kind::Null: break;
  }
  return DbType::Unknown;
}

int rank(const Value& v) {
  switch (v.kind) {
    case Value::Kind::Integer:
    case Value::Kind::Real: return 1;
    case Value::Kind::Text: return 2;
    default: return 3;
  }
}

// Orders two values that are not NULL: numbers before text before bytes.
int compare_values(const Value& x, const Value& y) {
  const int rx = rank(x), ry = rank(y);
  if (rx != ry) return rx < ry ? -1 : 1;
  if (rx == 1) {
    if (x.kind == Value::Kind::Integer && y.kind == Value::Kind::Integer) return x.integer < y.integer ? -1 : x.integer > y.integer ? 1 : 0;
    const double dx = x.kind == Value::Kind::Integer ? static_cast<double>(x.integer) : x.real;
    const double dy = y.kind == Value::Kind::Integer ? static_cast<double>(y.integer) : y.real;
    return dx < dy ? -1 : dx > dy ? 1 : 0;
  }
  return x.text.view().compare(y.text.view());
}

bool like_match(StringView s, StringView p) {
  uint64_t si = 0, pi = 0, star = ~uint64_t{0}, mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && (p[pi] == '_' || (p[pi] != '%' && lc(p[pi]) == lc(s[si])))) {
      ++si;
      ++pi;
    } else if (pi < p.size() && p[pi] == '%') {
      star = pi++;
      mark = si;
    } else if (star != ~uint64_t{0}) {
      pi = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '%') ++pi;
  return pi == p.size();
}

bool append_hex(String* out, StringView bytes) {
  static const char kDigits[] = "0123456789abcdef";
  if (!out->append("\\x")) return false;
  for (uint64_t i = 0; i < bytes.size(); ++i) {
    const uint8_t b = static_cast<uint8_t>(bytes[i]);
    if (!out->push_back(kDigits[b >> 4]) || !out->push_back(kDigits[b & 15])) return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Running statements

struct Ctx {
  const Table* table = nullptr;
  uint64_t row = 0;
  bool has_row = false;
  const List<uint64_t>* group = nullptr;   // the rows an aggregate runs over
};

struct SelectItem {
  explicit SelectItem(IAllocator& allocator) : alias(allocator) {}
  SelectItem(SelectItem&&) = default;
  SelectItem& operator=(SelectItem&&) = default;
  int32_t root = -1;
  bool star = false;
  String alias;
};

struct OrderItem {
  int32_t root;
  bool descending;
};

class Exec {
 public:
  Exec(Catalog& catalog, IAllocator& allocator, StringView sql, ArrayView<const DbParam> params, DbError* error)
      : catalog_(catalog), a_(allocator), sql_(sql), params_(params), err_(error), pool_(allocator) {}

  bool changed = false;

  void advance() { tok_ = lex(sql_, &pos_); }
  bool at_end() const { return tok_.kind == Tk::End; }
  bool at_punct(const char* p) const { return tok_.kind == Tk::Punct && tok_.text.equals(p); }
  void skip_semicolons() {
    while (at_punct(";")) advance();
  }
  bool statement(UniquePointer<TextResultSet>* out);
  bool end_statement() {
    if (at_end() || at_punct(";")) return true;
    return syntax();
  }
  void new_statement() { pool_.clear(); }

 private:
  // --- Errors
  bool fail(const char* state, StringView message, StringView detail = StringView()) {
    err_->set(DbStatus::Server, message);
    err_->sqlstate.assign(state);
    if (!detail.empty()) err_->detail.assign(detail);
    return false;
  }
  bool fail_name(const char* state, const char* before, StringView name, const char* after) {
    String m(a_);
    m.assign(before);
    m.append(name);
    m.append(after);
    return fail(state, m.view());
  }
  bool oom() {
    err_->set(DbStatus::OutOfMemory, "out of memory");
    return false;
  }
  // After a call that returned false: its own error stands, and one that only ran out of memory is reported as that.
  bool failed_or_oom() {
    if (err_->status == DbStatus::Ok) oom();
    return false;
  }
  bool syntax() {
    String m(a_);
    if (tok_.kind == Tk::End) {
      m.assign("syntax error at end of input");
    } else {
      m.assign("syntax error at or near \"");
      m.append(tok_.text);
      m.append("\"");
    }
    String d(a_);
    d.assign("position ");
    d.append_uint(tok_.pos + 1);
    return fail("42601", m.view(), d.view());
  }
  bool unsupported(StringView what) {
    String m(a_);
    m.assign(what);
    m.append(" is not supported");
    return fail("0A000", m.view());
  }

  // --- Reading tokens
  bool kw(const char* word) const { return tok_.kind == Tk::Ident && tok_.text.equals_ignore_case(word); }
  bool accept_kw(const char* word) {
    if (!kw(word)) return false;
    advance();
    return true;
  }
  bool expect_kw(const char* word) { return accept_kw(word) || syntax(); }
  bool accept_punct(const char* p) {
    if (!at_punct(p)) return false;
    advance();
    return true;
  }
  bool expect_punct(const char* p) { return accept_punct(p) || syntax(); }
  bool name(String* out) {
    if (tok_.kind != Tk::Ident && tok_.kind != Tk::Quoted) return syntax();
    if (!out->assign(tok_.text)) return oom();
    advance();
    return true;
  }

  // --- Expressions: parsing
  int32_t add(Expr&& e) {
    if (!pool_.push_back(util::move(e))) {
      oom();
      return -1;
    }
    return static_cast<int32_t>(pool_.count() - 1);
  }
  int32_t binary(Op op, int32_t l, int32_t r) {
    if (l < 0 || r < 0) return -1;
    Expr e(a_);
    e.op = op;
    e.a = l;
    e.b = r;
    return add(util::move(e));
  }
  int32_t unary(Op op, int32_t x) {
    if (x < 0) return -1;
    Expr e(a_);
    e.op = op;
    e.a = x;
    return add(util::move(e));
  }
  int32_t parse_expr();
  int32_t parse_and();
  int32_t parse_not();
  int32_t parse_comparison();
  int32_t parse_concat();
  int32_t parse_sum();
  int32_t parse_product();
  int32_t parse_unary();
  int32_t parse_primary();
  int32_t parse_call(StringView function);
  bool unescape(StringView raw, String* out) {
    out->clear();
    for (uint64_t i = 0; i < raw.size(); ++i) {
      if (!out->push_back(raw[i])) return oom();
      if (raw[i] == '\'' && i + 1 < raw.size() && raw[i + 1] == '\'') ++i;
    }
    return true;
  }

  // --- Expressions: running
  bool bind(const Table* table);
  bool eval(const Ctx& ctx, int32_t i, Value* out);
  bool eval_arith(Op op, const Value& x, const Value& y, Value* out);
  bool eval_aggregate(const Ctx& ctx, const Expr& e, Value* out);
  bool to_number(const Value& v, Value* out);
  bool to_text(const Value& v, String* out);
  bool truthy(const Value& v) const { return v.kind == Value::Kind::Integer ? v.integer != 0 : v.kind == Value::Kind::Real ? v.real != 0 : false; }
  bool has_aggregate(int32_t i) const;
  bool coerce(Value* v, const ColumnDef& column);
  bool filter(Table* table, int32_t where, List<uint64_t>* rows);
  bool eval_count(int32_t root, int64_t* out);
  bool render(TextResultSet* rs, const Value& v, DbType type);

  // --- Statements
  bool create_table(UniquePointer<TextResultSet>* out);
  bool drop_table(UniquePointer<TextResultSet>* out);
  bool insert(UniquePointer<TextResultSet>* out);
  bool select(UniquePointer<TextResultSet>* out);
  bool update(UniquePointer<TextResultSet>* out);
  bool remove_rows(UniquePointer<TextResultSet>* out);
  bool show(UniquePointer<TextResultSet>* out);
  bool describe(StringView table_name, UniquePointer<TextResultSet>* out);
  bool find_table(StringView table_name, Table** table);
  bool key_conflict(const Table& table, const Value* candidate, uint64_t skip_row, bool* conflict, String* detail);
  bool make_result(UniquePointer<TextResultSet>* out, TextResultSet** rs) {
    *out = UniquePointer<TextResultSet>::make(a_, a_);
    if (!*out) return oom();
    *rs = out->get();
    return true;
  }
  bool tag_result(UniquePointer<TextResultSet>* out, StringView command, uint64_t affected) {
    TextResultSet* rs;
    if (!make_result(out, &rs)) return false;
    rs->set_command(command, affected);
    return true;
  }

  Catalog& catalog_;
  IAllocator& a_;
  StringView sql_;
  ArrayView<const DbParam> params_;
  DbError* err_;
  uint64_t pos_ = 0;
  Token tok_;
  List<Expr> pool_;
  uint32_t next_param_ = 0;
  uint32_t depth_ = 0;
};

// --- parsing expressions

int32_t Exec::parse_expr() {
  if (++depth_ > 64) {
    fail("54001", "the expression is too complex");
    --depth_;
    return -1;
  }
  int32_t left = parse_and();
  while (left >= 0 && accept_kw("or")) left = binary(Op::Or, left, parse_and());
  --depth_;
  return left;
}

int32_t Exec::parse_and() {
  int32_t left = parse_not();
  while (left >= 0 && accept_kw("and")) left = binary(Op::And, left, parse_not());
  return left;
}

int32_t Exec::parse_not() {
  if (accept_kw("not")) {
    if (++depth_ > 64) {
      --depth_;
      fail("54001", "the expression is too complex");
      return -1;
    }
    const int32_t x = unary(Op::Not, parse_not());
    --depth_;
    return x;
  }
  return parse_comparison();
}

int32_t Exec::parse_comparison() {
  int32_t left = parse_concat();
  while (left >= 0) {
    Op op;
    if (at_punct("=") || at_punct("==")) op = Op::Eq;
    else if (at_punct("<>") || at_punct("!=")) op = Op::Ne;
    else if (at_punct("<")) op = Op::Lt;
    else if (at_punct("<=")) op = Op::Le;
    else if (at_punct(">")) op = Op::Gt;
    else if (at_punct(">=")) op = Op::Ge;
    else if (kw("is")) {
      advance();
      const bool negated = accept_kw("not");
      if (!accept_kw("null")) {
        syntax();
        return -1;
      }
      left = unary(negated ? Op::NotNull : Op::IsNull, left);
      continue;
    } else if (kw("like") || kw("not")) {
      const bool negated = accept_kw("not");
      if (!accept_kw("like")) {
        syntax();
        return -1;
      }
      left = binary(negated ? Op::NotLike : Op::Like, left, parse_concat());
      continue;
    } else {
      break;
    }
    advance();
    left = binary(op, left, parse_concat());
  }
  return left;
}

int32_t Exec::parse_concat() {
  int32_t left = parse_sum();
  while (left >= 0 && at_punct("||")) {
    advance();
    left = binary(Op::Concat, left, parse_sum());
  }
  return left;
}

int32_t Exec::parse_sum() {
  int32_t left = parse_product();
  while (left >= 0 && (at_punct("+") || at_punct("-"))) {
    const Op op = at_punct("+") ? Op::Add : Op::Sub;
    advance();
    left = binary(op, left, parse_product());
  }
  return left;
}

int32_t Exec::parse_product() {
  int32_t left = parse_unary();
  while (left >= 0 && (at_punct("*") || at_punct("/") || at_punct("%"))) {
    const Op op = at_punct("*") ? Op::Mul : at_punct("/") ? Op::Div : Op::Mod;
    advance();
    left = binary(op, left, parse_unary());
  }
  return left;
}

int32_t Exec::parse_unary() {
  if (at_punct("-") || at_punct("+")) {
    const bool negate = at_punct("-");
    advance();
    if (++depth_ > 64) {
      --depth_;
      fail("54001", "the expression is too complex");
      return -1;
    }
    const int32_t x = parse_unary();
    --depth_;
    return negate ? unary(Op::Neg, x) : x;
  }
  return parse_primary();
}

int32_t Exec::parse_primary() {
  Expr e(a_);
  switch (tok_.kind) {
    case Tk::Number: {
      int64_t i;
      double d;
      if (!tok_.text.contains('.') && !tok_.text.contains('e') && !tok_.text.contains('E') && parse_int64(tok_.text, &i)) e.literal.set_int(i);
      else if (parse_double(tok_.text, &d)) e.literal.set_real(d);
      else {
        syntax();
        return -1;
      }
      advance();
      return add(util::move(e));
    }
    case Tk::String:
      if (!unescape(tok_.text, &e.literal.text)) return -1;
      e.literal.kind = Value::Kind::Text;
      advance();
      return add(util::move(e));
    case Tk::Param:
      e.op = Op::Param;
      e.param = next_param_++;
      advance();
      return add(util::move(e));
    case Tk::Punct:
      if (at_punct("(")) {
        advance();
        const int32_t x = parse_expr();
        if (x < 0 || !expect_punct(")")) return -1;
        return x;
      }
      syntax();
      return -1;
    case Tk::Quoted:
    case Tk::Ident: {
      const bool quoted = tok_.kind == Tk::Quoted;
      const StringView word = tok_.text;
      if (!quoted) {
        if (word.equals_ignore_case("null")) {
          advance();
          return add(util::move(e));
        }
        if (word.equals_ignore_case("true") || word.equals_ignore_case("false")) {
          e.literal.set_int(word.equals_ignore_case("true") ? 1 : 0);
          advance();
          return add(util::move(e));
        }
      }
      advance();
      if (!quoted && at_punct("(")) return parse_call(word);
      e.op = Op::Column;
      if (at_punct(".")) {   // table.column: there is only one table, so the qualifier is not needed
        advance();
        if (tok_.kind != Tk::Ident && tok_.kind != Tk::Quoted) {
          syntax();
          return -1;
        }
        e.name.assign(tok_.text);
        advance();
      } else {
        e.name.assign(word);
      }
      return add(util::move(e));
    }
    default: syntax(); return -1;
  }
}

int32_t Exec::parse_call(StringView function) {
  Expr e(a_);
  advance();   // the "("
  struct Named { const char* name; Op op; Fn fn; int min_args; int max_args; };
  static const Named kFunctions[] = {
      {"count", Op::Count, Fn::Coalesce, 1, 1}, {"sum", Op::Sum, Fn::Coalesce, 1, 1}, {"avg", Op::Avg, Fn::Coalesce, 1, 1}, {"min", Op::Min, Fn::Coalesce, 1, 1},
      {"max", Op::Max, Fn::Coalesce, 1, 1},     {"coalesce", Op::Func, Fn::Coalesce, 1, 4}, {"ifnull", Op::Func, Fn::Coalesce, 2, 2}, {"lower", Op::Func, Fn::Lower, 1, 1},
      {"upper", Op::Func, Fn::Upper, 1, 1},     {"length", Op::Func, Fn::Length, 1, 1},     {"abs", Op::Func, Fn::Abs, 1, 1},
  };
  const Named* found = nullptr;
  for (const Named& f : kFunctions)
    if (function.equals_ignore_case(f.name)) found = &f;
  if (!found) {
    String m(a_);
    m.assign("function ");
    m.append(function);
    m.append("() does not exist");
    fail("42883", m.view());
    return -1;
  }
  e.op = found->op;
  e.fn = found->fn;
  if (is_aggregate(found->op) && at_punct("*")) {
    if (found->op != Op::Count) {
      syntax();
      return -1;
    }
    advance();
    e.star = true;
  } else if (!at_punct(")")) {
    for (;;) {
      if (e.argc >= 4) {
        fail("54023", "too many arguments");
        return -1;
      }
      const int32_t x = parse_expr();
      if (x < 0) return -1;
      e.args[e.argc++] = x;
      if (!accept_punct(",")) break;
    }
  }
  if (!expect_punct(")")) return -1;
  if (!e.star && (e.argc < found->min_args || e.argc > found->max_args)) {
    String m(a_);
    m.assign("wrong number of arguments for ");
    m.append(function);
    fail("42883", m.view());
    return -1;
  }
  return add(util::move(e));
}

// --- running expressions

bool Exec::bind(const Table* table) {
  for (uint64_t i = 0; i < pool_.count(); ++i) {
    Expr& e = pool_[i];
    if (e.op != Op::Column) continue;
    const int32_t column = table ? table->find_column(e.name.view()) : -1;
    if (column < 0) return fail_name("42703", "column \"", e.name.view(), "\" does not exist");
    e.column = static_cast<uint32_t>(column);
  }
  return true;
}

bool Exec::has_aggregate(int32_t i) const {
  if (i < 0) return false;
  const Expr& e = pool_[i];
  if (is_aggregate(e.op)) return true;
  if (has_aggregate(e.a) || has_aggregate(e.b)) return true;
  for (uint8_t k = 0; k < e.argc; ++k)
    if (has_aggregate(e.args[k])) return true;
  return false;
}

bool Exec::to_number(const Value& v, Value* out) {
  if (v.kind == Value::Kind::Integer) {
    out->set_int(v.integer);
    return true;
  }
  if (v.kind == Value::Kind::Real) {
    out->set_real(v.real);
    return true;
  }
  int64_t i;
  double d;
  if (v.kind == Value::Kind::Text && parse_int64(v.text.view(), &i)) {
    out->set_int(i);
    return true;
  }
  if (v.kind == Value::Kind::Text && parse_double(v.text.view(), &d)) {
    out->set_real(d);
    return true;
  }
  return fail_name("22P02", "invalid input syntax for a number: \"", v.text.view(), "\"");
}

bool Exec::to_text(const Value& v, String* out) {
  out->clear();
  bool ok = true;
  switch (v.kind) {
    case Value::Kind::Null: break;
    case Value::Kind::Integer: ok = out->append_int(v.integer); break;
    case Value::Kind::Real: ok = append_double(out, v.real); break;
    default: ok = out->assign(v.text.view()); break;
  }
  return ok || oom();
}

bool Exec::eval_arith(Op op, const Value& x, const Value& y, Value* out) {
  if (x.is_null() || y.is_null()) {
    out->set_null();
    return true;
  }
  Value nx(a_), ny(a_);
  if (!to_number(x, &nx) || !to_number(y, &ny)) return false;
  if (nx.kind == Value::Kind::Integer && ny.kind == Value::Kind::Integer) {
    const uint64_t ux = static_cast<uint64_t>(nx.integer), uy = static_cast<uint64_t>(ny.integer);
    switch (op) {
      case Op::Add: out->set_int(static_cast<int64_t>(ux + uy)); return true;
      case Op::Sub: out->set_int(static_cast<int64_t>(ux - uy)); return true;
      case Op::Mul: out->set_int(static_cast<int64_t>(ux * uy)); return true;
      default: break;
    }
    if (ny.integer == 0) return fail("22012", "division by zero");
    if (ny.integer == -1) {   // INT64_MIN / -1 does not fit
      out->set_int(op == Op::Div ? static_cast<int64_t>(0 - ux) : 0);
      return true;
    }
    out->set_int(op == Op::Div ? nx.integer / ny.integer : nx.integer % ny.integer);
    return true;
  }
  const double dx = nx.kind == Value::Kind::Integer ? static_cast<double>(nx.integer) : nx.real;
  const double dy = ny.kind == Value::Kind::Integer ? static_cast<double>(ny.integer) : ny.real;
  switch (op) {
    case Op::Add: out->set_real(dx + dy); return true;
    case Op::Sub: out->set_real(dx - dy); return true;
    case Op::Mul: out->set_real(dx * dy); return true;
    default: break;
  }
  if (dy == 0) return fail("22012", "division by zero");
  if (op == Op::Div) {
    out->set_real(dx / dy);
    return true;
  }
  const double q = dx / dy;
  if (q > 9.0e18 || q < -9.0e18) return fail("22003", "the remainder is out of range");
  out->set_real(dx - static_cast<double>(static_cast<int64_t>(q)) * dy);
  return true;
}

bool Exec::eval_aggregate(const Ctx& ctx, const Expr& e, Value* out) {
  if (!ctx.group) return fail("42803", "aggregate functions are only allowed in the list of a SELECT");
  Ctx inner = ctx;
  inner.group = nullptr;
  int64_t count = 0;
  Value best(a_), sum_value(a_);
  bool all_integer = true;
  double real_sum = 0;
  int64_t int_sum = 0;
  for (uint64_t k = 0; k < ctx.group->count(); ++k) {
    inner.row = (*ctx.group)[k];
    inner.has_row = true;
    if (e.star) {
      ++count;
      continue;
    }
    Value v(a_);
    if (!eval(inner, e.args[0], &v)) return false;
    if (v.is_null()) continue;
    ++count;
    if (e.op == Op::Min || e.op == Op::Max) {
      if (best.is_null() || (e.op == Op::Min ? compare_values(v, best) < 0 : compare_values(v, best) > 0))
        if (!best.copy_from(v)) return oom();
    } else if (e.op == Op::Sum || e.op == Op::Avg) {
      Value n(a_);
      if (!to_number(v, &n)) return false;
      if (n.kind == Value::Kind::Integer) {
        int_sum = static_cast<int64_t>(static_cast<uint64_t>(int_sum) + static_cast<uint64_t>(n.integer));
        real_sum += static_cast<double>(n.integer);
      } else {
        all_integer = false;
        real_sum += n.real;
      }
    }
  }
  switch (e.op) {
    case Op::Count: out->set_int(count); break;
    case Op::Min:
    case Op::Max:
      if (!out->copy_from(best)) return oom();
      break;
    case Op::Sum:
      if (count == 0) out->set_null();
      else if (all_integer) out->set_int(int_sum);
      else out->set_real(real_sum);
      break;
    default:   // Avg
      if (count == 0) out->set_null();
      else out->set_real(real_sum / static_cast<double>(count));
      break;
  }
  return true;
}

bool Exec::eval(const Ctx& ctx, int32_t i, Value* out) {
  const Expr& e = pool_[i];
  switch (e.op) {
    case Op::Literal:
      return out->copy_from(e.literal) || oom();
    case Op::Column:
      if (!ctx.table) return fail_name("42703", "column \"", e.name.view(), "\" cannot be used here");
      if (!ctx.has_row) {
        out->set_null();
        return true;
      }
      return out->copy_from(ctx.table->at(ctx.row, e.column)) || oom();
    case Op::Param: {
      const DbParam& p = params_[e.param];
      switch (p.kind) {
        case DbParam::Kind::Null: out->set_null(); return true;
        case DbParam::Kind::Boolean: out->set_int(p.boolean ? 1 : 0); return true;
        case DbParam::Kind::Integer: out->set_int(p.integer); return true;
        case DbParam::Kind::Real: out->set_real(p.real); return true;
        case DbParam::Kind::Text: return out->set_text(p.text) || oom();
        case DbParam::Kind::Bytes: return out->set_bytes(StringView(p.bytes)) || oom();
      }
      return fail("22023", "unknown parameter kind");
    }
    case Op::Neg: {
      Value x(a_), n(a_);
      if (!eval(ctx, e.a, &x)) return false;
      if (x.is_null()) {
        out->set_null();
        return true;
      }
      if (!to_number(x, &n)) return false;
      if (n.kind == Value::Kind::Integer) out->set_int(static_cast<int64_t>(0 - static_cast<uint64_t>(n.integer)));
      else out->set_real(-n.real);
      return true;
    }
    case Op::Not: {
      Value x(a_);
      if (!eval(ctx, e.a, &x)) return false;
      if (x.is_null()) out->set_null();
      else out->set_int(truthy(x) ? 0 : 1);
      return true;
    }
    case Op::And:
    case Op::Or: {
      const bool is_and = e.op == Op::And;
      Value l(a_), r(a_);
      if (!eval(ctx, e.a, &l)) return false;
      if (!l.is_null() && truthy(l) != is_and) {   // decided: false AND x, true OR x
        out->set_int(is_and ? 0 : 1);
        return true;
      }
      if (!eval(ctx, e.b, &r)) return false;
      if (!r.is_null() && truthy(r) != is_and) {
        out->set_int(is_and ? 0 : 1);
        return true;
      }
      if (l.is_null() || r.is_null()) out->set_null();
      else out->set_int(is_and ? 1 : 0);
      return true;
    }
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
      Value x(a_), y(a_);
      return eval(ctx, e.a, &x) && eval(ctx, e.b, &y) && eval_arith(e.op, x, y, out);
    }
    case Op::Concat: {
      Value x(a_), y(a_);
      if (!eval(ctx, e.a, &x) || !eval(ctx, e.b, &y)) return false;
      if (x.is_null() || y.is_null()) {
        out->set_null();
        return true;
      }
      String s(a_), t(a_);
      if (!to_text(x, &s) || !to_text(y, &t) || !s.append(t.view())) return oom();
      return out->set_text(s.view()) || oom();
    }
    case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
      Value x(a_), y(a_);
      if (!eval(ctx, e.a, &x) || !eval(ctx, e.b, &y)) return false;
      if (x.is_null() || y.is_null()) {
        out->set_null();
        return true;
      }
      const int c = compare_values(x, y);
      bool r = false;
      switch (e.op) {
        case Op::Eq: r = c == 0; break;
        case Op::Ne: r = c != 0; break;
        case Op::Lt: r = c < 0; break;
        case Op::Le: r = c <= 0; break;
        case Op::Gt: r = c > 0; break;
        default: r = c >= 0; break;
      }
      out->set_int(r ? 1 : 0);
      return true;
    }
    case Op::IsNull:
    case Op::NotNull: {
      Value x(a_);
      if (!eval(ctx, e.a, &x)) return false;
      out->set_int((x.is_null() == (e.op == Op::IsNull)) ? 1 : 0);
      return true;
    }
    case Op::Like:
    case Op::NotLike: {
      Value x(a_), y(a_);
      if (!eval(ctx, e.a, &x) || !eval(ctx, e.b, &y)) return false;
      if (x.is_null() || y.is_null()) {
        out->set_null();
        return true;
      }
      String s(a_), p(a_);
      if (!to_text(x, &s) || !to_text(y, &p)) return false;
      out->set_int(like_match(s.view(), p.view()) == (e.op == Op::Like) ? 1 : 0);
      return true;
    }
    case Op::Func: {
      if (e.fn == Fn::Coalesce) {
        for (uint8_t k = 0; k < e.argc; ++k) {
          Value v(a_);
          if (!eval(ctx, e.args[k], &v)) return false;
          if (!v.is_null()) return out->copy_from(v) || oom();
        }
        out->set_null();
        return true;
      }
      Value x(a_);
      if (!eval(ctx, e.args[0], &x)) return false;
      if (x.is_null()) {
        out->set_null();
        return true;
      }
      if (e.fn == Fn::Abs) {
        Value n(a_);
        if (!to_number(x, &n)) return false;
        if (n.kind == Value::Kind::Integer) out->set_int(n.integer < 0 ? static_cast<int64_t>(0 - static_cast<uint64_t>(n.integer)) : n.integer);
        else out->set_real(n.real < 0 ? -n.real : n.real);
        return true;
      }
      String s(a_);
      if (!to_text(x, &s)) return false;
      if (e.fn == Fn::Length) {
        int64_t chars = 0;
        for (uint64_t k = 0; k < s.size(); ++k)
          if (x.kind == Value::Kind::Bytes || (static_cast<uint8_t>(s[k]) & 0xC0) != 0x80) ++chars;
        out->set_int(chars);
        return true;
      }
      for (uint64_t k = 0; k < s.size(); ++k) {
        const char c = s[k];
        if (e.fn == Fn::Lower && c >= 'A' && c <= 'Z') s.set(k, static_cast<char>(c + 32));
        if (e.fn == Fn::Upper && c >= 'a' && c <= 'z') s.set(k, static_cast<char>(c - 32));
      }
      return out->set_text(s.view()) || oom();
    }
    case Op::Count: case Op::Sum: case Op::Avg: case Op::Min: case Op::Max:
      return eval_aggregate(ctx, e, out);
  }
  return fail("XX000", "unknown expression");
}

// Makes a value fit its column: text for a number column is read as a number, and so on.
bool Exec::coerce(Value* v, const ColumnDef& column) {
  if (v->is_null() || column.type == DbType::Unknown) return true;
  auto bad = [&]() {
    String m(a_);
    m.assign("invalid input syntax for type ");
    m.append(db_type_name(column.type));
    String d(a_);
    d.assign("column \"");
    d.append(column.name.view());
    d.append("\"");
    return fail("22P02", m.view(), d.view());
  };
  switch (column.type) {
    case DbType::Integer:
    case DbType::Boolean: {
      int64_t i = 0;
      if (v->kind == Value::Kind::Integer) {
        i = v->integer;
      } else if (v->kind == Value::Kind::Real) {
        if (!(v->real > -9.2e18 && v->real < 9.2e18) || v->real != static_cast<double>(static_cast<int64_t>(v->real))) return bad();
        i = static_cast<int64_t>(v->real);
      } else if (v->kind == Value::Kind::Text) {
        double d;
        const StringView t = v->text.view().trimmed();
        if (column.type == DbType::Boolean) {
          static const char* const kTrue[] = {"t", "true", "yes", "y", "on", "1"};
          static const char* const kFalse[] = {"f", "false", "no", "n", "off", "0"};
          bool found = false;
          for (const char* w : kTrue)
            if (t.equals_ignore_case(w)) i = 1, found = true;
          for (const char* w : kFalse)
            if (t.equals_ignore_case(w)) i = 0, found = true;
          if (!found) return bad();
        } else if (parse_int64(t, &i)) {
        } else if (parse_double(t, &d) && d > -9.2e18 && d < 9.2e18 && d == static_cast<double>(static_cast<int64_t>(d))) {
          i = static_cast<int64_t>(d);
        } else {
          return bad();
        }
      } else {
        return bad();
      }
      v->set_int(column.type == DbType::Boolean ? (i != 0 ? 1 : 0) : i);
      return true;
    }
    case DbType::Real:
    case DbType::Decimal: {
      double d;
      if (v->kind == Value::Kind::Real) return true;
      if (v->kind == Value::Kind::Integer) d = static_cast<double>(v->integer);
      else if (v->kind != Value::Kind::Text || !parse_double(v->text.view().trimmed(), &d)) return bad();
      v->set_real(d);
      return true;
    }
    case DbType::Bytes:
      if (v->kind == Value::Kind::Text || v->kind == Value::Kind::Bytes) {
        v->kind = Value::Kind::Bytes;
        return true;
      }
      return bad();
    default: {   // text and the kinds that are kept as text
      if (v->kind == Value::Kind::Text) return true;
      String s(a_);
      if (!to_text(*v, &s)) return false;
      return v->set_text(s.view()) || oom();
    }
  }
}

bool Exec::render(TextResultSet* rs, const Value& v, DbType type) {
  String s(a_);
  switch (v.kind) {
    case Value::Kind::Null: return rs->add_null() || oom();
    case Value::Kind::Integer:
      if (type == DbType::Boolean) return rs->add_cell(v.integer ? "true" : "false") || oom();
      s.append_int(v.integer);
      return rs->add_cell(s.view()) || oom();
    case Value::Kind::Real:
      append_double(&s, v.real);
      return rs->add_cell(s.view()) || oom();
    case Value::Kind::Text: return rs->add_cell(v.text.view()) || oom();
    case Value::Kind::Bytes: return (append_hex(&s, v.text.view()) && rs->add_cell(s.view())) || oom();
  }
  return false;
}

bool Exec::find_table(StringView table_name, Table** table) {
  *table = catalog_.find(table_name);
  if (*table) return true;
  return fail_name("42P01", "relation \"", table_name, "\" does not exist");
}

bool Exec::filter(Table* table, int32_t where, List<uint64_t>* rows) {
  Ctx ctx;
  ctx.table = table;
  const uint64_t n = table->row_count();
  for (uint64_t r = 0; r < n; ++r) {
    if (where >= 0) {
      ctx.row = r;
      ctx.has_row = true;
      Value v(a_);
      if (!eval(ctx, where, &v)) return false;
      if (v.is_null() || !truthy(v)) continue;
    }
    if (!rows->push_back(r)) return oom();
  }
  return true;
}

// Evaluates a LIMIT or OFFSET (no row involved) to a count.
bool Exec::eval_count(int32_t root, int64_t* out) {
  Ctx ctx;
  Value v(a_), n(a_);
  if (!eval(ctx, root, &v)) return false;
  if (v.is_null() || !to_number(v, &n)) return v.is_null() ? fail("2201W", "LIMIT and OFFSET must not be NULL") : false;
  const int64_t i = n.kind == Value::Kind::Integer ? n.integer : static_cast<int64_t>(n.real);
  if (i < 0) return fail("2201W", "LIMIT and OFFSET must not be negative");
  *out = i;
  return true;
}

// Does a row hold the same key as `candidate` (a full row of values)? *conflict says, and `detail` describes the key.
bool Exec::key_conflict(const Table& table, const Value* candidate, uint64_t skip_row, bool* conflict, String* detail) {
  *conflict = false;
  if (!table.has_key()) return true;
  const uint64_t cols = table.columns.count();
  const uint64_t rows = table.row_count();
  for (uint64_t r = 0; r < rows; ++r) {
    if (r == skip_row) continue;
    bool same = true;
    for (uint64_t c = 0; c < cols && same; ++c)
      if (table.columns[c].primary_key) same = compare_values(table.at(r, c), candidate[c]) == 0;
    if (!same) continue;
    *conflict = true;
    if (detail) {
      detail->assign("Key (");
      bool first = true;
      for (uint64_t c = 0; c < cols; ++c)
        if (table.columns[c].primary_key) {
          if (!first) detail->append(", ");
          detail->append(table.columns[c].name.view());
          first = false;
        }
      detail->append(")=(");
      first = true;
      for (uint64_t c = 0; c < cols; ++c)
        if (table.columns[c].primary_key) {
          if (!first) detail->append(", ");
          String s(a_);
          to_text(candidate[c], &s);
          detail->append(s.view());
          first = false;
        }
      detail->append(") already exists.");
    }
    return true;
  }
  return true;
}

// --- statements

bool Exec::statement(UniquePointer<TextResultSet>* out) {
  if (kw("select")) return select(out);
  if (kw("insert")) {
    changed = true;
    return insert(out);
  }
  if (kw("update")) {
    changed = true;
    return update(out);
  }
  if (kw("delete")) {
    changed = true;
    return remove_rows(out);
  }
  if (kw("create")) {
    changed = true;
    return create_table(out);
  }
  if (kw("drop")) {
    changed = true;
    return drop_table(out);
  }
  if (kw("show")) return show(out);
  if (kw("describe") || kw("desc")) {
    advance();
    String table_name(a_);
    return name(&table_name) && describe(table_name.view(), out);
  }
  if (kw("begin") || kw("commit") || kw("rollback") || kw("start") || kw("end")) return fail("25P01", "transactions are controlled through the connection");
  return syntax();
}

bool Exec::show(UniquePointer<TextResultSet>* out) {
  advance();
  if (accept_kw("columns")) {
    if (!expect_kw("from")) return false;
    String table_name(a_);
    return name(&table_name) && describe(table_name.view(), out);
  }
  if (!expect_kw("tables")) return false;
  TextResultSet* rs;
  if (!make_result(out, &rs)) return false;
  rs->add_column("name", DbType::Text);
  rs->add_column("columns", DbType::Integer);
  rs->add_column("rows", DbType::Integer);
  for (uint64_t t = 0; t < catalog_.table_count(); ++t) {
    Table& table = catalog_.table_at(t);
    String n(a_);
    rs->begin_row();
    rs->add_cell(table.name.view());
    n.append_uint(table.columns.count());
    rs->add_cell(n.view());
    n.clear();
    n.append_uint(table.row_count());
    rs->add_cell(n.view());
  }
  String c(a_);
  c.assign("SHOW ");
  c.append_uint(rs->row_count());
  rs->set_command(c.view(), rs->row_count());
  return true;
}

bool Exec::describe(StringView table_name, UniquePointer<TextResultSet>* out) {
  Table* table;
  if (!find_table(table_name, &table)) return false;
  TextResultSet* rs;
  if (!make_result(out, &rs)) return false;
  rs->add_column("name", DbType::Text);
  rs->add_column("type", DbType::Text);
  rs->add_column("not_null", DbType::Boolean);
  rs->add_column("primary_key", DbType::Boolean);
  rs->add_column("default", DbType::Text);
  for (uint64_t c = 0; c < table->columns.count(); ++c) {
    const ColumnDef& column = table->columns[c];
    rs->begin_row();
    rs->add_cell(column.name.view());
    rs->add_cell(column.type == DbType::Unknown ? "any" : db_type_name(column.type));
    rs->add_cell(column.not_null || column.primary_key ? "true" : "false");
    rs->add_cell(column.primary_key ? "true" : "false");
    if (column.has_default) render(rs, column.default_value, column.type);
    else rs->add_null();
  }
  rs->set_command("DESCRIBE", rs->row_count());
  return true;
}

bool Exec::create_table(UniquePointer<TextResultSet>* out) {
  advance();
  if (!expect_kw("table")) return false;
  bool if_not_exists = false;
  if (accept_kw("if")) {
    if (!expect_kw("not") || !expect_kw("exists")) return false;
    if_not_exists = true;
  }
  String table_name(a_);
  if (!name(&table_name) || !expect_punct("(")) return false;
  UniquePointer<Table> table = UniquePointer<Table>::make(a_, a_, table_name.view());
  if (!table) return oom();
  List<String> key_names(a_);
  for (;;) {
    if (kw("primary")) {   // PRIMARY KEY (a, b)
      advance();
      if (!expect_kw("key") || !expect_punct("(")) return false;
      for (;;) {
        String key(a_);
        if (!name(&key) || !key_names.push_back(util::move(key))) return false;
        if (!accept_punct(",")) break;
      }
      if (!expect_punct(")")) return false;
    } else {
      ColumnDef column(a_);
      if (!name(&column.name)) return false;
      if (table->find_column(column.name.view()) >= 0) return fail_name("42701", "column \"", column.name.view(), "\" is given twice");
      if (tok_.kind == Tk::Ident && !kw("primary") && !kw("not") && !kw("null") && !kw("default")) {
        struct Named { const char* name; DbType type; };
        static const Named kTypes[] = {
            {"integer", DbType::Integer}, {"int", DbType::Integer},  {"bigint", DbType::Integer}, {"smallint", DbType::Integer}, {"boolean", DbType::Boolean}, {"bool", DbType::Boolean},
            {"real", DbType::Real},       {"float", DbType::Real},   {"double", DbType::Real},   {"numeric", DbType::Real},     {"decimal", DbType::Real},    {"text", DbType::Text},
            {"varchar", DbType::Text},    {"char", DbType::Text},    {"character", DbType::Text}, {"string", DbType::Text},     {"blob", DbType::Bytes},      {"bytea", DbType::Bytes},
        };
        bool known = false;
        for (const Named& t : kTypes)
          if (tok_.text.equals_ignore_case(t.name)) column.type = t.type, known = true;
        if (!known) return fail_name("42704", "type \"", tok_.text, "\" does not exist");
        advance();
        if (kw("precision") || kw("varying")) advance();
        if (accept_punct("(")) {   // (length) or (precision, scale): not used
          while (!at_end() && !at_punct(")")) advance();
          if (!expect_punct(")")) return false;
        }
      }
      for (;;) {
        if (accept_kw("primary")) {
          if (!expect_kw("key")) return false;
          column.primary_key = true;
        } else if (accept_kw("not")) {
          if (!expect_kw("null")) return false;
          column.not_null = true;
        } else if (accept_kw("null")) {
        } else if (accept_kw("default")) {
          pool_.clear();
          const int32_t root = parse_unary();
          if (root < 0 || !bind(nullptr)) return false;
          Ctx ctx;
          if (!eval(ctx, root, &column.default_value) || !coerce(&column.default_value, column)) return false;
          column.has_default = true;
        } else if (kw("unique") || kw("references") || kw("check") || kw("autoincrement")) {
          return unsupported(tok_.text);
        } else {
          break;
        }
      }
      if (!table->columns.push_back(util::move(column))) return oom();
    }
    if (!accept_punct(",")) break;
  }
  if (!expect_punct(")")) return false;
  if (table->columns.count() == 0) return syntax();
  for (uint64_t k = 0; k < key_names.count(); ++k) {
    const int32_t c = table->find_column(key_names[k].view());
    if (c < 0) return fail_name("42703", "column \"", key_names[k].view(), "\" does not exist");
    table->columns[c].primary_key = true;
  }
  if (catalog_.find(table_name.view())) {
    if (if_not_exists) return tag_result(out, "CREATE TABLE", 0);
    return fail_name("42P07", "relation \"", table_name.view(), "\" already exists");
  }
  if (!catalog_.add(util::move(table))) return oom();
  return tag_result(out, "CREATE TABLE", 0);
}

bool Exec::drop_table(UniquePointer<TextResultSet>* out) {
  advance();
  if (!expect_kw("table")) return false;
  bool if_exists = false;
  if (accept_kw("if")) {
    if (!expect_kw("exists")) return false;
    if_exists = true;
  }
  String table_name(a_);
  if (!name(&table_name)) return false;
  if (!catalog_.drop(table_name.view()) && !if_exists) return fail_name("42P01", "relation \"", table_name.view(), "\" does not exist");
  return tag_result(out, "DROP TABLE", 0);
}

bool Exec::insert(UniquePointer<TextResultSet>* out) {
  advance();
  bool replace = false, ignore = false;
  if (accept_kw("or")) {
    if (accept_kw("replace")) replace = true;
    else if (accept_kw("ignore")) ignore = true;
    else return syntax();
  }
  if (!expect_kw("into")) return false;
  String table_name(a_);
  if (!name(&table_name)) return false;
  Table* table;
  if (!find_table(table_name.view(), &table)) return false;
  const uint64_t cols = table->columns.count();
  List<int32_t> targets(a_);
  if (accept_punct("(")) {
    for (;;) {
      String column(a_);
      if (!name(&column)) return false;
      const int32_t c = table->find_column(column.view());
      if (c < 0) return fail_name("42703", "column \"", column.view(), "\" does not exist");
      for (uint64_t k = 0; k < targets.count(); ++k)
        if (targets[k] == c) return fail_name("42701", "column \"", column.view(), "\" is given twice");
      if (!targets.push_back(c)) return oom();
      if (!accept_punct(",")) break;
    }
    if (!expect_punct(")")) return false;
  } else {
    for (uint64_t c = 0; c < cols; ++c) targets.push_back(static_cast<int32_t>(c));
  }
  if (!expect_kw("values")) return false;
  List<int32_t> roots(a_);
  uint64_t tuples = 0;
  for (;;) {
    if (!expect_punct("(")) return false;
    uint64_t n = 0;
    for (;;) {
      const int32_t root = parse_expr();
      if (root < 0 || !roots.push_back(root)) return false;
      ++n;
      if (!accept_punct(",")) break;
    }
    if (!expect_punct(")")) return false;
    if (n != targets.count()) return fail("42601", n > targets.count() ? "INSERT has more expressions than target columns" : "INSERT has more target columns than expressions");
    ++tuples;
    if (!accept_punct(",")) break;
  }
  if (!bind(nullptr)) return false;
  uint64_t inserted = 0;
  Ctx ctx;
  for (uint64_t t = 0; t < tuples; ++t) {
    List<Value> row(a_);
    for (uint64_t c = 0; c < cols; ++c) {
      Value v(a_);
      const ColumnDef& column = table->columns[c];
      if (column.has_default && !v.copy_from(column.default_value)) return oom();
      if (!row.push_back(util::move(v))) return oom();
    }
    for (uint64_t k = 0; k < targets.count(); ++k) {
      Value v(a_);
      if (!eval(ctx, roots[t * targets.count() + k], &v)) return false;
      row[targets[k]] = util::move(v);
    }
    for (uint64_t c = 0; c < cols; ++c) {
      const ColumnDef& column = table->columns[c];
      if (!coerce(&row[c], column)) return false;
      if (row[c].is_null() && (column.not_null || column.primary_key)) return fail_name("23502", "null value in column \"", column.name.view(), "\" violates the not-null constraint");
    }
    bool conflict = false;
    String detail(a_);
    if (!key_conflict(*table, row.view().data(), ~uint64_t{0}, &conflict, &detail)) return false;
    if (conflict) {
      if (ignore) continue;
      if (replace) {   // the old row goes, the new one takes its place at the end
        List<uint8_t> flags(a_);
        for (uint64_t r = 0; r < table->row_count(); ++r) {
          bool same = true;
          for (uint64_t c = 0; c < cols && same; ++c)
            if (table->columns[c].primary_key) same = compare_values(table->at(r, c), row[c]) == 0;
          if (!flags.push_back(same ? 1 : 0)) return oom();
        }
        table->erase_rows(flags);
      } else {
        return fail("23505", "duplicate key value violates a unique constraint", detail.view());
      }
    }
    if (!table->add_row(row.view())) return oom();
    ++inserted;
  }
  String tag(a_);
  tag.assign("INSERT 0 ");
  tag.append_uint(inserted);
  return tag_result(out, tag.view(), inserted);
}

bool Exec::select(UniquePointer<TextResultSet>* out) {
  advance();
  if (kw("distinct")) return unsupported("DISTINCT");
  List<SelectItem> items(a_);
  for (;;) {
    SelectItem item(a_);
    if (at_punct("*")) {
      advance();
      item.star = true;
    } else {
      item.root = parse_expr();
      if (item.root < 0) return false;
      if (accept_kw("as")) {
        if (!name(&item.alias)) return false;
      } else if (tok_.kind == Tk::Ident && !kw("from") && !kw("where") && !kw("order") && !kw("limit") && !kw("offset") && !kw("group")) {
        if (!name(&item.alias)) return false;
      }
    }
    if (!items.push_back(util::move(item))) return oom();
    if (!accept_punct(",")) break;
  }
  Table* table = nullptr;
  if (accept_kw("from")) {
    String table_name(a_);
    if (!name(&table_name) || !find_table(table_name.view(), &table)) return false;
    if (tok_.kind == Tk::Ident && !kw("where") && !kw("order") && !kw("limit") && !kw("offset") && !kw("group")) advance();   // an alias for the table
  }
  // A * is every column of the table.
  List<SelectItem> expanded(a_);
  for (uint64_t k = 0; k < items.count(); ++k) {
    if (!items[k].star) {
      if (!expanded.push_back(util::move(items[k]))) return oom();
      continue;
    }
    if (!table) return fail("42601", "SELECT * needs a FROM clause");
    for (uint64_t c = 0; c < table->columns.count(); ++c) {
      Expr e(a_);
      e.op = Op::Column;
      e.name.assign(table->columns[c].name.view());
      SelectItem item(a_);
      item.root = add(util::move(e));
      if (item.root < 0 || !expanded.push_back(util::move(item))) return false;
    }
  }
  int32_t where = -1;
  if (accept_kw("where")) {
    where = parse_expr();
    if (where < 0) return false;
  }
  if (kw("group")) return unsupported("GROUP BY");
  List<OrderItem> order(a_);
  if (accept_kw("order")) {
    if (!expect_kw("by")) return false;
    for (;;) {
      OrderItem o{-1, false};
      if (tok_.kind == Tk::Number) {   // ORDER BY 2: the second item of the list
        uint64_t n = 0;
        if (!tok_.text.parse_uint(&n) || n == 0 || n > expanded.count()) return fail("42P10", "ORDER BY position is not in the select list");
        o.root = expanded[n - 1].root;
        advance();
      } else {
        bool by_alias = false;
        if (tok_.kind == Tk::Ident || tok_.kind == Tk::Quoted) {   // a name from the select list
          for (uint64_t k = 0; k < expanded.count() && !by_alias; ++k)
            if (!expanded[k].alias.empty() && expanded[k].alias.view().equals_ignore_case(tok_.text)) {
              o.root = expanded[k].root;
              by_alias = true;
              advance();
            }
        }
        if (!by_alias) o.root = parse_expr();
      }
      if (o.root < 0) return false;
      if (accept_kw("desc")) o.descending = true;
      else accept_kw("asc");
      if (!order.push_back(o)) return oom();
      if (!accept_punct(",")) break;
    }
  }
  int32_t limit = -1, offset = -1;
  if (accept_kw("limit")) {
    limit = parse_expr();
    if (limit < 0) return false;
    if (accept_punct(",")) {   // LIMIT offset, count
      offset = limit;
      limit = parse_expr();
      if (limit < 0) return false;
    }
  }
  if (accept_kw("offset")) {
    offset = parse_expr();
    if (offset < 0) return false;
  }
  if (!bind(table)) return false;

  bool aggregated = false;
  for (uint64_t k = 0; k < expanded.count(); ++k) aggregated = aggregated || has_aggregate(expanded[k].root);
  List<uint64_t> matches(a_);
  if (table) {
    if (!filter(table, where, &matches)) return false;
  } else {
    Ctx ctx;
    ctx.has_row = false;
    bool pass = true;
    if (where >= 0) {
      Value v(a_);
      if (!eval(ctx, where, &v)) return false;
      pass = !v.is_null() && truthy(v);
    }
    if (pass && !matches.push_back(0)) return oom();
  }

  const uint64_t item_count = expanded.count();
  List<Value> cells(a_);
  uint64_t out_rows = 0;
  Ctx ctx;
  ctx.table = table;
  if (aggregated) {
    ctx.group = &matches;
    ctx.has_row = table && matches.count() > 0;
    ctx.row = ctx.has_row ? matches[0] : 0;
    for (uint64_t k = 0; k < item_count; ++k) {
      Value v(a_);
      if (!eval(ctx, expanded[k].root, &v) || !cells.push_back(util::move(v))) return failed_or_oom();
    }
    out_rows = 1;
  } else {
    // Order: a sort of positions in `matches` by keys worked out once per row.
    List<uint64_t> positions(a_);
    for (uint64_t p = 0; p < matches.count(); ++p)
      if (!positions.push_back(p)) return oom();
    if (order.count() > 0 && matches.count() > 1) {
      const uint64_t k = order.count();
      List<Value> keys(a_);
      for (uint64_t p = 0; p < matches.count(); ++p) {
        ctx.row = matches[p];
        ctx.has_row = table != nullptr;
        for (uint64_t j = 0; j < k; ++j) {
          Value v(a_);
          if (!eval(ctx, order[j].root, &v) || !keys.push_back(util::move(v))) return failed_or_oom();
        }
      }
      auto before = [&](uint64_t x, uint64_t y) {   // does position x sort before position y?
        for (uint64_t j = 0; j < k; ++j) {
          const Value& vx = keys[x * k + j];
          const Value& vy = keys[y * k + j];
          int c;
          if (vx.is_null() || vy.is_null()) c = vx.is_null() == vy.is_null() ? 0 : vx.is_null() ? -1 : 1;
          else c = compare_values(vx, vy);
          if (order[j].descending) c = -c;
          if (c != 0) return c < 0;
        }
        return false;
      };
      List<uint64_t> scratch(a_);
      for (uint64_t p = 0; p < positions.count(); ++p)
        if (!scratch.push_back(0)) return oom();
      const uint64_t n = positions.count();
      for (uint64_t width = 1; width < n; width *= 2) {   // a merge sort, which keeps equal rows in their order
        for (uint64_t lo = 0; lo < n; lo += 2 * width) {
          const uint64_t mid = lo + width < n ? lo + width : n;
          const uint64_t hi = lo + 2 * width < n ? lo + 2 * width : n;
          uint64_t i = lo, j = mid, o = lo;
          while (i < mid && j < hi) scratch[o++] = before(positions[j], positions[i]) ? positions[j++] : positions[i++];
          while (i < mid) scratch[o++] = positions[i++];
          while (j < hi) scratch[o++] = positions[j++];
        }
        for (uint64_t p = 0; p < n; ++p) positions[p] = scratch[p];
      }
    }
    int64_t skip = 0, take = -1;
    if (offset >= 0 && !eval_count(offset, &skip)) return false;
    if (limit >= 0 && !eval_count(limit, &take)) return false;
    for (uint64_t p = static_cast<uint64_t>(skip); p < positions.count(); ++p) {
      if (take >= 0 && out_rows >= static_cast<uint64_t>(take)) break;
      ctx.row = matches[positions[p]];
      ctx.has_row = table != nullptr;
      for (uint64_t k = 0; k < item_count; ++k) {
        Value v(a_);
        if (!eval(ctx, expanded[k].root, &v) || !cells.push_back(util::move(v))) return failed_or_oom();
      }
      ++out_rows;
    }
  }

  TextResultSet* rs;
  if (!make_result(out, &rs)) return false;
  List<DbType> types(a_);
  for (uint64_t k = 0; k < item_count; ++k) {
    const Expr& e = pool_[expanded[k].root];
    DbType type = DbType::Unknown;
    if (e.op == Op::Column) type = table->columns[e.column].type;
    else if (is_boolean_op(e.op)) type = DbType::Boolean;
    for (uint64_t r = 0; r < out_rows && type == DbType::Unknown; ++r) type = type_of_kind(cells[r * item_count + k].kind);
    if (type == DbType::Unknown) type = DbType::Text;
    String column_name(a_);
    if (!expanded[k].alias.empty()) column_name.assign(expanded[k].alias.view());
    else if (e.op == Op::Column) column_name.assign(e.name.view());
    else if (is_aggregate(e.op)) column_name.assign(e.op == Op::Count ? "count" : e.op == Op::Sum ? "sum" : e.op == Op::Avg ? "avg" : e.op == Op::Min ? "min" : "max");
    else column_name.assign("?column?");
    if (!types.push_back(type) || !rs->add_column(column_name.view(), type)) return oom();
  }
  for (uint64_t r = 0; r < out_rows; ++r) {
    if (!rs->begin_row()) return oom();
    for (uint64_t k = 0; k < item_count; ++k)
      if (!render(rs, cells[r * item_count + k], types[k])) return false;
  }
  String tag(a_);
  tag.assign("SELECT ");
  tag.append_uint(out_rows);
  rs->set_command(tag.view(), out_rows);
  return true;
}

bool Exec::update(UniquePointer<TextResultSet>* out) {
  advance();
  String table_name(a_);
  if (!name(&table_name)) return false;
  Table* table;
  if (!find_table(table_name.view(), &table)) return false;
  if (!expect_kw("set")) return false;
  List<int32_t> columns(a_), roots(a_);
  for (;;) {
    String column(a_);
    if (!name(&column)) return false;
    const int32_t c = table->find_column(column.view());
    if (c < 0) return fail_name("42703", "column \"", column.view(), "\" does not exist");
    if (!(at_punct("=") || at_punct("=="))) return syntax();
    advance();
    const int32_t root = parse_expr();
    if (root < 0 || !columns.push_back(c) || !roots.push_back(root)) return false;
    if (!accept_punct(",")) break;
  }
  int32_t where = -1;
  if (accept_kw("where")) {
    where = parse_expr();
    if (where < 0) return false;
  }
  if (!bind(table)) return false;
  List<uint64_t> matches(a_);
  if (!filter(table, where, &matches)) return false;
  // New values are worked out from the old rows first, so that SET a = b, b = a swaps.
  const uint64_t cols = table->columns.count();
  List<Value> fresh(a_);
  Ctx ctx;
  ctx.table = table;
  for (uint64_t m = 0; m < matches.count(); ++m) {
    ctx.row = matches[m];
    ctx.has_row = true;
    for (uint64_t c = 0; c < cols; ++c) {
      Value v(a_);
      if (!v.copy_from(table->at(ctx.row, c)) || !fresh.push_back(util::move(v))) return oom();
    }
    for (uint64_t k = 0; k < columns.count(); ++k) {
      Value v(a_);
      if (!eval(ctx, roots[k], &v)) return false;
      const ColumnDef& column = table->columns[columns[k]];
      if (!coerce(&v, column)) return false;
      if (v.is_null() && (column.not_null || column.primary_key)) return fail_name("23502", "null value in column \"", column.name.view(), "\" violates the not-null constraint");
      fresh[m * cols + columns[k]] = util::move(v);
    }
  }
  for (uint64_t m = 0; m < matches.count(); ++m)
    for (uint64_t c = 0; c < cols; ++c) table->at(matches[m], c) = util::move(fresh[m * cols + c]);
  if (table->has_key()) {
    for (uint64_t m = 0; m < matches.count(); ++m) {
      bool conflict = false;
      String detail(a_);
      if (!key_conflict(*table, &table->at(matches[m], 0), matches[m], &conflict, &detail)) return false;
      if (conflict) return fail("23505", "duplicate key value violates a unique constraint", detail.view());
    }
  }
  String tag(a_);
  tag.assign("UPDATE ");
  tag.append_uint(matches.count());
  return tag_result(out, tag.view(), matches.count());
}

bool Exec::remove_rows(UniquePointer<TextResultSet>* out) {
  advance();
  if (!expect_kw("from")) return false;
  String table_name(a_);
  if (!name(&table_name)) return false;
  Table* table;
  if (!find_table(table_name.view(), &table)) return false;
  int32_t where = -1;
  if (accept_kw("where")) {
    where = parse_expr();
    if (where < 0) return false;
  }
  if (!bind(table)) return false;
  List<uint64_t> matches(a_);
  if (!filter(table, where, &matches)) return false;
  List<uint8_t> flags(a_);
  for (uint64_t r = 0; r < table->row_count(); ++r)
    if (!flags.push_back(0)) return oom();
  for (uint64_t m = 0; m < matches.count(); ++m) flags[matches[m]] = 1;
  table->erase_rows(flags);
  String tag(a_);
  tag.assign("DELETE ");
  tag.append_uint(matches.count());
  return tag_result(out, tag.view(), matches.count());
}

}  // namespace

DbStatus run_sql(Catalog& catalog, IAllocator& allocator, StringView sql, ArrayView<const DbParam> params, UniquePointer<TextResultSet>* result, bool* changed, DbError* error) {
  error->clear();
  result->reset();
  *changed = false;
  // The ? markers must match the parameters before anything runs.
  uint32_t markers = 0;
  for (uint64_t pos = 0;;) {
    const Token t = lex(sql, &pos);
    if (t.kind == Tk::End) break;
    if (t.kind == Tk::Bad) {
      error->set(DbStatus::Server, "syntax error: unterminated quote or unknown character");
      error->sqlstate.assign("42601");
      return DbStatus::Server;
    }
    if (t.kind == Tk::Param) ++markers;
  }
  if (markers != params.size()) {
    String m(allocator);
    m.assign("the statement has ");
    m.append_uint(markers);
    m.append(" parameter marker(s) but ");
    m.append_uint(params.size());
    m.append(" value(s) were given");
    error->set(DbStatus::InvalidArgument, m.view());
    return DbStatus::InvalidArgument;
  }
  Exec exec(catalog, allocator, sql, params, error);
  exec.advance();
  for (;;) {
    exec.skip_semicolons();
    if (exec.at_end()) break;
    exec.new_statement();
    result->reset();
    if (!exec.statement(result) || !exec.end_statement()) {
      if (error->status == DbStatus::Ok) error->set(DbStatus::Server, "the statement failed");
      *changed = exec.changed;
      return error->status;
    }
  }
  *changed = exec.changed;
  if (!*result) {
    *result = UniquePointer<TextResultSet>::make(allocator, allocator);
    if (!*result) {
      error->set(DbStatus::OutOfMemory, "out of memory");
      return DbStatus::OutOfMemory;
    }
  }
  return DbStatus::Ok;
}

}  // namespace database::local

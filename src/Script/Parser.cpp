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

// The lexer and the recursive-descent parser. The language is ECMAScript 5 with most of what came after: let / const, arrow functions, template literals
// (also tagged), spread, for-of, default parameters, destructuring (declarations, assignments, parameters, for-of heads, catch), classes (extends, super,
// static members, fields, private names as properties whose name starts with #, accessors), getters and setters, generators (function*, yield, yield*),
// regular expression literals, ?? ?. and the logical assignments (&&= ||= ??=), numeric separators, new.target, async functions and await. Classes,
// destructuring, tagged templates, async generators and for await are turned into plain code here (see class_body(), destructure(), tagged_template(),
// wrap_async_generator(), lower_for_await()); a BigInt literal is a call of BigInt(). Left out: `with`.
#include <initializer_list>
#include <new>
#include "Script/Ast.hpp"
#include "Script/Number.hpp"
#include "Utility/Move.hpp"

namespace script {

namespace {

enum class T : uint8_t {
  Eof, Number, String, Ident, Backtick,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi, Comma, Dot, Ellipsis, Question, QuestionDot, Colon, Arrow,
  Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign, StarStarAssign, ShlAssign, ShrAssign, UShrAssign, AmpAssign, PipeAssign, CaretAssign,
  Plus, Minus, Star, Slash, Percent, StarStar, Inc, Dec, Amp, Pipe, Caret, Tilde, Bang, Shl, Shr, UShr, AmpAmp, PipePipe, QQ, AmpAmpAssign, PipePipeAssign, QQAssign,
  Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge,
};

struct Token {
  T kind = T::Eof;
  StringView text;        // identifiers: the name; strings: the decoded text
  double number = 0;
  bool newline_before = false;
  uint32_t line = 1;
  bool bigint = false;    // a number written 123n (text holds its digits)
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || static_cast<uint8_t>(c) >= 0x80; }
bool is_ident_part(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

void append_utf8(uint32_t cp, String* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  struct ExportEntry {
    enum Kind : uint8_t { Local, From, Star, StarAs } kind;
    StringView exported, local;
    uint32_t request;
  };

  Parser(StringView source, Arena& arena, String* error) : src_(source), arena_(arena), error_(error), scratch_(arena.allocator()) {}

  // A module: see parse_module() in Ast.hpp.
  Node* module_program(ModuleRequests* requests) {
    module_ = true;
    requests_ = requests;
    in_async_ = true;   // (top-level await)
    next();
    Node* body = arena_.make(NodeKind::Block, 1);
    if (!body) return nullptr;
    Node** tail = &body->list;
    while (cur_.kind != T::Eof && !failed_) {
      Node* s = statement();
      if (!s) break;
      if (s->kind == NodeKind::Empty && s->flags == 0xFF) continue;   // (an export that leaves no statement)
      *tail = s;
      tail = &s->next;
    }
    if (failed_) return nullptr;
    // imports first, then the definitions of the exports, then the module's own statements
    Node* head = nullptr;
    Node** head_tail = &head;
    Node* rest = nullptr;
    Node** rest_tail = &rest;
    for (Node* s = body->list; s;) {
      Node* following = s->next;
      s->next = nullptr;
      if (s->kind == NodeKind::Import) {
        *head_tail = s;
        head_tail = &s->next;
      } else {
        *rest_tail = s;
        rest_tail = &s->next;
      }
      s = following;
    }
    for (uint64_t i = 0; i < exports_.count() && !failed_; ++i) {
      const ExportEntry& e = exports_[i];
      Node* call = nullptr;
      if (e.kind == ExportEntry::Star) {
        call = new_call(new_ident("__export_star"), {new_ident("__exports"), deps_index(e.request)});
      } else {
        Node* value = nullptr;
        if (e.kind == ExportEntry::Local) value = new_ident(e.local);
        else if (e.kind == ExportEntry::StarAs) value = deps_index(e.request);
        else {
          Node* index = make(NodeKind::Index);
          if (!index) return nullptr;
          index->a = deps_index(e.request);
          index->b = new_string(e.local);
          value = index;
        }
        Node* getter = make(NodeKind::Function);
        if (!getter || !value) return nullptr;
        getter->flags = kFlagArrow | kFlagExprBody;
        getter->a = value;
        call = new_call(new_ident("__export_get"), {new_ident("__exports"), new_string(e.exported), getter});
      }
      if (!call) return nullptr;
      Node* st = new_statement(call);
      if (!st) return nullptr;
      *head_tail = st;
      head_tail = &st->next;
    }
    *head_tail = rest;
    body->list = head;
    Node* fn = make(NodeKind::Function);
    if (!fn) return nullptr;
    fn->flags = kFlagAsync | kFlagStrict;   // (module code is strict)
    fn->a = body;
    Node** params = &fn->list;
    for (const char* name : {"__deps", "__exports", "import.meta"}) {
      Node* p = new_ident(name);
      if (!p) return nullptr;
      *params = p;
      params = &p->next;
    }
    Node* program = arena_.make(NodeKind::Block, 1);
    Node* st = new_statement(fn);
    if (!program || !st) return nullptr;
    program->list = st;
    return program;
  }

  Node* program() {
    next();
    Node* block = arena_.make(NodeKind::Block, 1);
    if (!block) return nullptr;
    Node** tail = &block->list;
    while (cur_.kind != T::Eof && !failed_) {
      Node* s = statement();
      if (!s) break;
      *tail = s;
      tail = &s->next;
    }
    return failed_ ? nullptr : block;
  }

 private:
  // --- Errors
  void fail(const char* message) {
    if (failed_) return;
    failed_ = true;
    error_->assign(message);
    error_->append(" (line ");
    error_->append_uint(cur_.line);
    error_->append(")");
  }
  Node* null_fail(const char* message) {
    fail(message);
    return nullptr;
  }

  // --- Lexer
  bool at_end() const { return pos_ >= src_.size(); }
  char peek(uint64_t ahead = 0) const { return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0'; }

  void skip_space(bool* newline) {
    for (;;) {
      const char c = peek();
      if (c == '\n') {
        *newline = true;
        ++line_;
        ++pos_;
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
        ++pos_;
      } else if (c == '/' && peek(1) == '/') {
        while (!at_end() && peek() != '\n') ++pos_;
      } else if (c == '/' && peek(1) == '*') {
        pos_ += 2;
        while (!at_end() && !(peek() == '*' && peek(1) == '/')) {
          if (peek() == '\n') {
            *newline = true;
            ++line_;
          }
          ++pos_;
        }
        pos_ += at_end() ? 0 : 2;
      } else if (c == '<' && peek(1) == '!' && peek(2) == '-' && peek(3) == '-') {   // (an HTML comment opener in a script)
        while (!at_end() && peek() != '\n') ++pos_;
      } else {
        return;
      }
    }
  }

  // Reads the escape after a backslash into `out` (pos_ is after the backslash).
  void read_escape(String* out) {
    const char c = peek();
    ++pos_;
    switch (c) {
      case 'n': out->push_back('\n'); break;
      case 't': out->push_back('\t'); break;
      case 'r': out->push_back('\r'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'v': out->push_back('\v'); break;
      case '0': out->push_back('\0'); break;
      case '\r': if (peek() == '\n') ++pos_; ++line_; break;
      case '\n': ++line_; break;
      case 'x': {
        const int h = hex_value(peek()), l = hex_value(peek(1));
        if (h < 0 || l < 0) {
          fail("Invalid hexadecimal escape sequence");
          return;
        }
        pos_ += 2;
        append_utf8(static_cast<uint32_t>(h * 16 + l), out);
        break;
      }
      case 'u': {
        uint32_t cp = 0;
        if (peek() == '{') {
          ++pos_;
          while (!at_end() && peek() != '}') {
            const int d = hex_value(peek());
            if (d < 0) { fail("Invalid Unicode escape sequence"); return; }
            cp = cp * 16 + static_cast<uint32_t>(d);
            ++pos_;
          }
          ++pos_;
        } else {
          for (int i = 0; i < 4; ++i) {
            const int d = hex_value(peek());
            if (d < 0) { fail("Invalid Unicode escape sequence"); return; }
            cp = cp * 16 + static_cast<uint32_t>(d);
            ++pos_;
          }
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF && out->size() >= 3) {   // "\uD83D\uDE00": a surrogate pair is one character
          const StringView tail = out->view().substr(out->size() - 3);
          if (static_cast<uint8_t>(tail[0]) == 0xED && static_cast<uint8_t>(tail[1]) >= 0xA0 && static_cast<uint8_t>(tail[1]) <= 0xAF) {
            const uint32_t high = 0xD000u + ((static_cast<uint8_t>(tail[1]) & 0x3Fu) << 6) + (static_cast<uint8_t>(tail[2]) & 0x3Fu);
            out->truncate(out->size() - 3);
            cp = 0x10000 + ((high - 0xD800) << 10) + (cp - 0xDC00);
          }
        }
        append_utf8(cp, out);
        break;
      }
      default: out->push_back(c); break;
    }
  }

  void next() {
    Token t;
    bool newline = false;
    skip_space(&newline);
    t.newline_before = newline;
    t.line = line_;
    prev_end_ = pos_;
    if (at_end()) {
      t.kind = T::Eof;
      cur_ = t;
      return;
    }
    const char c = peek();
    if (is_ident_start(c) || (c == '#' && is_ident_start(peek(1)))) {   // (#name: a private member, kept as a property whose name starts with #)
      const uint64_t start = pos_;
      ++pos_;
      while (is_ident_part(peek())) ++pos_;
      t.kind = T::Ident;
      t.text = src_.substr(start, pos_ - start);
    } else if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
      lex_number(&t);
    } else if (c == '"' || c == '\'') {
      ++pos_;
      scratch_.clear();
      while (!at_end() && peek() != c) {
        if (peek() == '\n') {
          fail("Invalid or unexpected token");
          break;
        }
        if (peek() == '\\') {
          ++pos_;
          read_escape(&scratch_);
        } else {
          scratch_.push_back(peek());
          ++pos_;
        }
      }
      if (at_end()) fail("Invalid or unexpected token");
      ++pos_;
      t.kind = T::String;
      t.text = arena_.copy(scratch_.view());
    } else if (c == '`') {
      ++pos_;
      t.kind = T::Backtick;
    } else {
      lex_punct(&t);
    }
    cur_ = t;
  }

  // A number's text without the _ separators (a copy in the arena when there are any).
  StringView without_separators(StringView text) {
    if (!text.contains('_')) return text;
    String clean(arena_.allocator());
    for (const char c : text)
      if (c != '_') clean.push_back(c);
    return arena_.copy(clean.view());
  }

  void lex_number(Token* t) {
    const uint64_t start = pos_;
    t->kind = T::Number;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' || peek(1) == 'b' || peek(1) == 'B')) {
      pos_ += 2;
      while (hex_value(peek()) >= 0 || (peek() == '_' && hex_value(peek(1)) >= 0)) ++pos_;
      double v = 0;
      if (!string_to_number(without_separators(src_.substr(start, pos_ - start)), &v)) fail("Invalid number");
      t->number = v;
      if (peek() == 'n') {   // 0xFFn
        t->bigint = true;
        t->text = without_separators(src_.substr(start, pos_ - start));
        ++pos_;
      }
      return;
    }
    auto digits = [&] { while (is_digit(peek()) || (peek() == '_' && is_digit(peek(1)))) ++pos_; };   // (1_000_000)
    digits();
    if (peek() == '.') {
      ++pos_;
      digits();
    }
    if (peek() == 'e' || peek() == 'E') {
      const uint64_t save = pos_;
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!is_digit(peek())) pos_ = save;
      digits();
    }
    StringView text = without_separators(src_.substr(start, pos_ - start));
    if (peek() == 'n' && !text.contains('.') && !text.contains('e') && !text.contains('E')) {   // 123n
      t->bigint = true;
      t->text = text;
      ++pos_;
      double ignored = 0;
      parse_decimal(text, &ignored);
      return;
    }
    double v = 0;
    if (text.ends_with('.')) text = text.without_suffix(1);
    if (!parse_decimal(text, &v)) fail("Invalid number");
    t->number = v;
    if (is_ident_start(peek())) fail("Invalid or unexpected token");
  }

  void lex_punct(Token* t) {
    struct P {
      const char* text;
      T kind;
    };
    static const P kPuncts[] = {
        {">>>=", T::UShrAssign}, {"&&=", T::AmpAmpAssign}, {"||=", T::PipePipeAssign}, {"?\?=", T::QQAssign}, {"...", T::Ellipsis}, {"===", T::StrictEq}, {"!==", T::StrictNe}, {"**=", T::StarStarAssign}, {"<<=", T::ShlAssign},
        {">>=", T::ShrAssign},   {">>>", T::UShr},     {"=>", T::Arrow},      {"==", T::Eq},        {"!=", T::Ne},        {"<=", T::Le},
        {">=", T::Ge},           {"&&", T::AmpAmp},    {"||", T::PipePipe},   {"??", T::QQ},        {"?.", T::QuestionDot}, {"++", T::Inc},
        {"--", T::Dec},          {"+=", T::PlusAssign}, {"-=", T::MinusAssign}, {"*=", T::StarAssign}, {"/=", T::SlashAssign}, {"%=", T::PercentAssign},
        {"&=", T::AmpAssign},    {"|=", T::PipeAssign}, {"^=", T::CaretAssign}, {"**", T::StarStar},  {"<<", T::Shl},       {">>", T::Shr},
        {"(", T::LParen},        {")", T::RParen},     {"{", T::LBrace},      {"}", T::RBrace},     {"[", T::LBracket},   {"]", T::RBracket},
        {";", T::Semi},          {",", T::Comma},      {".", T::Dot},         {"?", T::Question},   {":", T::Colon},      {"=", T::Assign},
        {"+", T::Plus},          {"-", T::Minus},      {"*", T::Star},        {"/", T::Slash},      {"%", T::Percent},    {"&", T::Amp},
        {"|", T::Pipe},          {"^", T::Caret},      {"~", T::Tilde},       {"!", T::Bang},       {"<", T::Lt},         {">", T::Gt},
    };
    for (const P& p : kPuncts) {
      const StringView text(p.text);
      if (src_.substr(pos_).starts_with(text)) {
        if (p.kind == T::QuestionDot && is_digit(peek(2))) continue;   // (a ? .5 : x)
        // (a "/" where an operand is wanted starts a regular expression: see regex_literal())
        pos_ += text.size();
        t->kind = p.kind;
        return;
      }
    }
    pos_ += 1;
    fail("Invalid or unexpected token");
    t->kind = T::Eof;
  }

  // --- Parser helpers
  bool is(T kind) const { return cur_.kind == kind; }
  bool is_word(const char* word) const { return cur_.kind == T::Ident && cur_.text.equals(word); }
  bool accept(T kind) {
    if (cur_.kind != kind) return false;
    next();
    return true;
  }
  bool accept_word(const char* word) {
    if (!is_word(word)) return false;
    next();
    return true;
  }
  bool expect(T kind, const char* message) {
    if (cur_.kind == kind) {
      next();
      return true;
    }
    fail(message);
    return false;
  }
  static bool reserved(StringView w) {
    static const char* const kWords[] = {"break", "case", "catch", "continue", "default", "delete", "do", "else", "finally", "for", "function", "if", "in", "instanceof",
                                         "new", "return", "switch", "this", "throw", "try", "typeof", "var", "void", "while", "const", "let", "null", "true", "false", "class",
                                         "with", "yield", "enum", "export", "import", "super", "debugger"};
    for (const char* k : kWords)
      if (w.equals(k)) return true;
    return false;
  }
  void consume_semicolon() {
    if (accept(T::Semi)) return;
    if (is(T::RBrace) || is(T::Eof) || cur_.newline_before) return;   // (automatic semicolon insertion)
    fail("Unexpected token");
  }
  Node* make(NodeKind kind) {
    Node* n = arena_.make(kind, cur_.line);
    if (!n) fail("Out of memory");
    else if (kind == NodeKind::Function && strict_parse_) n->flags |= kFlagStrict;
    return n;
  }
  // While one is alive the functions made are strict mode code (class bodies are).
  struct StrictScope {
    explicit StrictScope(bool& flag) : flag_(flag), saved_(flag) { flag_ = true; }
    ~StrictScope() { flag_ = saved_; }
    bool& flag_;
    bool saved_;
  };

  // Saves and restores the lexer, for the few places that look ahead.
  struct Mark {
    uint64_t pos;
    uint32_t line;
    Token cur;
    uint64_t prev_end;
  };
  Mark mark() const { return {pos_, line_, cur_, prev_end_}; }
  void rewind(const Mark& m) {
    pos_ = m.pos;
    line_ = m.line;
    cur_ = m.cur;
    prev_end_ = m.prev_end;
  }

  // --- Statements
  // Nesting is limited: the parser and the compiler recurse, and the stack they run on is not large.
  struct Depth {
    Parser& p;
    bool ok;
    explicit Depth(Parser& parser) : p(parser), ok(parser.depth_ < 600) {
      if (ok) ++p.depth_;
      else p.fail("Too much nesting");
    }
    ~Depth() {
      if (ok) --p.depth_;
    }
  };

  Node* statement() {
    Depth guard(*this);
    return guard.ok ? statement_inner() : nullptr;
  }

  Node* statement_inner() {
    if (failed_) return nullptr;
    if (is(T::LBrace)) return block();
    if (is(T::Semi)) {
      Node* n = make(NodeKind::Empty);
      next();
      return n;
    }
    if (module_ && depth_ == 1 && cur_.kind == T::Ident) {
      if (cur_.text.equals("export")) return export_declaration();
      if (cur_.text.equals("import") && !import_expression_ahead()) return import_declaration();
    }
    if (cur_.kind == T::Ident) {
      const StringView w = cur_.text;
      if (w.equals("var") || w.equals("const") || (w.equals("let") && let_declaration_ahead())) {
        Node* n = var_declaration(false);
        if (n) consume_semicolon();
        return n;
      }
      if (w.equals("function")) return function_declaration(false);
      if (w.equals("async") && async_function_ahead()) {
        next();   // `async`
        return function_declaration(true);
      }
      if (w.equals("if")) return if_statement();
      if (w.equals("for")) return for_statement();
      if (w.equals("while")) return while_statement();
      if (w.equals("do")) return do_statement();
      if (w.equals("return")) return return_statement();
      if (w.equals("break") || w.equals("continue")) return jump_statement();
      if (w.equals("throw")) return throw_statement();
      if (w.equals("try")) return try_statement();
      if (w.equals("switch")) return switch_statement();
      if (w.equals("class")) return class_declaration();
      if (w.equals("debugger")) {
        next();
        consume_semicolon();
        return make(NodeKind::Empty);
      }
      if (!reserved(w)) {   // a label?
        const Mark m = mark();
        const StringView name = w;
        next();
        if (is(T::Colon)) {
          Node* n = make(NodeKind::Labeled);
          next();
          if (!n) return nullptr;
          n->text = name;
          n->a = statement();
          return n->a ? n : nullptr;
        }
        rewind(m);
      }
    }
    Node* n = make(NodeKind::ExprStmt);
    if (!n) return nullptr;
    n->a = expression(false);
    if (!n->a) return nullptr;
    consume_semicolon();
    return n;
  }

  // --- Modules
  Node* deps_index(uint32_t request) {
    Node* index = make(NodeKind::Index);
    Node* number = make(NodeKind::Number);
    if (!index || !number) return nullptr;
    number->number = request;
    index->a = new_ident("__deps");
    index->b = number;
    return index->a ? index : nullptr;
  }
  uint32_t request_of(StringView specifier) {
    for (uint64_t i = 0; i < requests_->specifiers.count(); ++i)
      if (requests_->specifiers[i].view().equals(specifier)) return static_cast<uint32_t>(i);
    String text(arena_.allocator());
    text.append(specifier);
    if (!requests_->specifiers.push_back(util::move(text))) fail("Out of memory");
    return static_cast<uint32_t>(requests_->specifiers.count() - 1);
  }
  // `import(` or `import.` ahead: an expression, not a declaration.
  bool import_expression_ahead() {
    const Mark m = mark();
    next();
    const bool yes = is(T::LParen) || is(T::Dot);
    rewind(m);
    return yes;
  }
  // A name in an import or export list: an identifier (reserved words allowed) or a string.
  bool module_name(StringView* out) {
    if (!is(T::Ident) && !is(T::String)) {
      fail("Unexpected token");
      return false;
    }
    *out = cur_.text;
    next();
    return true;
  }
  bool module_specifier(uint32_t* request) {
    if (!is(T::String)) {
      fail("Unexpected token");
      return false;
    }
    *request = request_of(cur_.text);
    next();
    return !failed_;
  }
  void add_export(ExportEntry::Kind kind, StringView exported, StringView local, uint32_t request) {
    if (kind != ExportEntry::Star)
      for (uint64_t i = 0; i < exports_.count(); ++i)
        if (exports_[i].kind != ExportEntry::Star && exports_[i].exported.equals(exported)) {
          fail("Duplicate export");
          return;
        }
    if (!exports_.push_back(ExportEntry{kind, exported, local, request})) fail("Out of memory");
    if (kind == ExportEntry::Star) {
      String entry(arena_.allocator());
      entry.append_uint(request);
      if (!requests_->stars.push_back(util::move(entry))) fail("Out of memory");
    } else {
      note_name(requests_->exports, exported);
      if (kind == ExportEntry::From) note_import(request, local);
    }
  }

  Node* import_declaration() {
    Node* n = make(NodeKind::Import);
    if (!n) return nullptr;
    next();   // `import`
    Node** tail = &n->list;
    auto add_spec = [&](StringView local, StringView imported, bool is_namespace) -> bool {
      Node* d = make(NodeKind::Declarator);
      if (!d) return false;
      d->text = local;
      if (!is_namespace) {
        d->a = new_string(imported);
        if (!d->a) return false;
      }
      *tail = d;
      tail = &d->next;
      return true;
    };
    uint32_t request = 0;
    if (is(T::String)) {   // import "module";
      if (!module_specifier(&request)) return nullptr;
      n->number = request;
      consume_semicolon();
      return failed_ ? nullptr : n;
    }
    bool more = true;
    if (is(T::Ident) && !is(T::LBrace)) {   // the default import
      if (reserved(cur_.text)) return null_fail("Unexpected token");
      if (!add_spec(cur_.text, "default", false)) return nullptr;
      next();
      more = accept(T::Comma);
    }
    if (more) {
      if (accept(T::Star)) {
        if (!accept_word("as") || !is(T::Ident) || reserved(cur_.text)) return null_fail("Unexpected token");
        if (!add_spec(cur_.text, StringView(), true)) return nullptr;
        next();
      } else if (accept(T::LBrace)) {
        while (!is(T::RBrace) && !is(T::Eof) && !failed_) {
          const bool word = is(T::Ident);
          StringView imported, local;
          if (!module_name(&imported)) return nullptr;
          local = imported;
          if (accept_word("as")) {
            if (!is(T::Ident) || reserved(cur_.text)) return null_fail("Unexpected token");
            local = cur_.text;
            next();
          } else if (!word || reserved(imported)) {
            return null_fail("Unexpected token");
          }
          if (!add_spec(local, imported, false)) return nullptr;
          if (!accept(T::Comma)) break;
        }
        if (!expect(T::RBrace, "Unexpected token")) return nullptr;
      } else {
        return null_fail("Unexpected token");
      }
    }
    if (!accept_word("from") || !module_specifier(&request)) return null_fail("Unexpected token");
    n->number = request;
    for (Node* d = n->list; d; d = d->next)
      if (d->a) note_import(request, d->a->text);
    consume_semicolon();
    return failed_ ? nullptr : n;
  }

  void note_import(uint32_t request, StringView name) {
    String entry(arena_.allocator());
    entry.append_uint(request);
    entry.push_back(':');
    entry.append(name);
    if (!requests_->imports.push_back(util::move(entry))) fail("Out of memory");
  }
  void note_name(List<String>& list, StringView name) {
    String entry(arena_.allocator());
    entry.append(name);
    if (!list.push_back(util::move(entry))) fail("Out of memory");
  }

  // The Empty statement that stands for "nothing to run here" (the caller drops it).
  Node* no_statement() {
    Node* n = make(NodeKind::Empty);
    if (n) n->flags = 0xFF;
    return n;
  }

  Node* export_declaration() {
    next();   // `export`
    if (accept_word("default")) return export_default();
    if (accept(T::Star)) {
      StringView name;
      const bool as = accept_word("as");
      if (as && !module_name(&name)) return nullptr;
      uint32_t request;
      if (!accept_word("from") || !module_specifier(&request)) return null_fail("Unexpected token");
      add_export(as ? ExportEntry::StarAs : ExportEntry::Star, name, StringView(), request);
      consume_semicolon();
      return failed_ ? nullptr : no_statement();
    }
    if (accept(T::LBrace)) {
      struct Pair { StringView local, exported; };
      List<Pair> pairs(arena_.allocator());
      while (!is(T::RBrace) && !is(T::Eof) && !failed_) {
        Pair pr;
        if (!module_name(&pr.local)) return nullptr;
        pr.exported = pr.local;
        if (accept_word("as") && !module_name(&pr.exported)) return nullptr;
        pairs.push_back(pr);
        if (!accept(T::Comma)) break;
      }
      if (!expect(T::RBrace, "Unexpected token")) return nullptr;
      uint32_t request = 0;
      const bool from = accept_word("from");
      if (from && !module_specifier(&request)) return nullptr;
      for (uint64_t i = 0; i < pairs.count(); ++i) add_export(from ? ExportEntry::From : ExportEntry::Local, pairs[i].exported, pairs[i].local, request);
      consume_semicolon();
      return failed_ ? nullptr : no_statement();
    }
    if (is_word("var") || is_word("const") || (is_word("let") && let_declaration_ahead())) {
      Node* decl = var_declaration(false);
      if (!decl) return nullptr;
      consume_semicolon();
      for (Node* d = decl->list; d; d = d->next)
        if (!d->text.empty() && d->text[0] != '%') add_export(ExportEntry::Local, d->text, d->text, 0);
      return decl;
    }
    if (is_word("function") || (is_word("async") && async_function_ahead())) {
      const bool is_async = is_word("async");
      if (is_async) next();
      Node* decl = function_declaration(is_async);
      if (decl) add_export(ExportEntry::Local, decl->text, decl->text, 0);
      return decl;
    }
    if (is_word("class")) {
      Node* decl = class_declaration();
      if (decl && decl->list) add_export(ExportEntry::Local, decl->list->text, decl->list->text, 0);
      return decl;
    }
    return null_fail("Unexpected token");
  }

  Node* export_default() {
    // a named function or class is a declaration that is also exported as "default"; anything else is an expression kept in a variable of its own
    bool declaration = false;
    bool is_async = false;
    {
      const Mark m = mark();
      if (is_word("async") && async_function_ahead()) {
        is_async = true;
        next();
      }
      if (is_word("function")) {
        next();
        accept(T::Star);
        declaration = is(T::Ident) && !reserved(cur_.text);
      } else if (is_word("class")) {
        next();
        declaration = is(T::Ident) && !reserved(cur_.text) && !is_word("extends");
        is_async = false;
      }
      rewind(m);
    }
    if (declaration) {
      Node* decl;
      if (is_word("class")) {
        decl = class_declaration();
        if (decl && decl->list) add_export(ExportEntry::Local, "default", decl->list->text, 0);
      } else {
        if (is_async) next();
        decl = function_declaration(is_async);
        if (decl) add_export(ExportEntry::Local, "default", decl->text, 0);
      }
      return decl;
    }
    Node* value;
    if (is_word("function") || (is_word("async") && async_function_ahead())) {
      const bool a = is_word("async");
      if (a) next();
      value = function_rest(true, a);
      if (value) {
        value->text = "default";   // (the function's name, which is not a binding inside it)
        value->flags &= static_cast<uint8_t>(~kFlagExpression);
      }
    } else {
      value = assignment(false);
      consume_semicolon();
    }
    if (!value) return nullptr;
    Node* v = make(NodeKind::Var);
    Node* d = make(NodeKind::Declarator);
    if (!v || !d) return nullptr;
    v->var_kind = VarKind::Let;
    d->text = "*default*";
    d->a = value;
    v->list = d;
    add_export(ExportEntry::Local, "default", "*default*", 0);
    return v;
  }

  bool let_declaration_ahead() {
    const Mark m = mark();
    next();
    const bool yes = (cur_.kind == T::Ident && !reserved(cur_.text)) || cur_.kind == T::LBracket || cur_.kind == T::LBrace;
    rewind(m);
    return yes;
  }

  Node* block() {
    Node* n = make(NodeKind::Block);
    if (!n || !expect(T::LBrace, "Unexpected token")) return nullptr;
    Node** tail = &n->list;
    while (!is(T::RBrace) && !is(T::Eof) && !failed_) {
      Node* s = statement();
      if (!s) return nullptr;
      *tail = s;
      tail = &s->next;
    }
    if (!expect(T::RBrace, "Unexpected end of input")) return nullptr;
    return n;
  }

  Node* var_declaration(bool no_in) {
    Node* n = make(NodeKind::Var);
    if (!n) return nullptr;
    n->var_kind = is_word("var") ? VarKind::Var : is_word("let") ? VarKind::Let : VarKind::Const;
    next();
    Node** tail = &n->list;
    do {
      if (is(T::LBracket) || is(T::LBrace)) {   // let [a, b] = ... / const { x, y } = ...
        Node* pattern = is(T::LBracket) ? array_literal() : object_literal();
        if (!pattern) return nullptr;
        Node* value = nullptr;
        if (accept(T::Assign)) {
          value = assignment(no_in);
          if (!value) return nullptr;
        } else if (!(no_in && (is_word("of") || is_word("in")))) {
          return null_fail("Missing initializer in destructuring declaration");
        }
        if (!value) {   // for (const [k, v] of x): the loop gives a value to a name of its own, the pattern is unpacked in the body
          Node* d = make(NodeKind::Declarator);
          if (!d) return nullptr;
          d->text = temp_name("%f");
          d->b = pattern;
          *tail = d;
          tail = &d->next;
          continue;
        }
        Sink sink;
        if (!destructure(pattern, value, false, &sink)) return nullptr;
        if (sink.head) {
          *tail = sink.head;
          tail = sink.tail;
        }
        continue;
      }
      if (!is(T::Ident) || reserved(cur_.text)) return null_fail("Unexpected token");
      Node* d = make(NodeKind::Declarator);
      if (!d) return nullptr;
      d->text = cur_.text;
      next();
      if (accept(T::Assign)) {
        d->a = assignment(no_in);
        if (!d->a) return nullptr;
      }
      *tail = d;
      tail = &d->next;
    } while (accept(T::Comma));
    return n;
  }

  // Is `async` (the current token) followed by `function` on the same line?
  bool async_function_ahead() {
    const Mark m = mark();
    next();
    const bool yes = is_word("function") && !cur_.newline_before;
    rewind(m);
    return yes;
  }

  Node* function_declaration(bool is_async) {
    Node* f = function_rest(false, is_async);
    if (!f) return nullptr;
    if (f->text.empty()) return null_fail("Function statements require a function name");
    Node* n = make(NodeKind::FunctionDecl);
    if (!n) return nullptr;
    n->a = f;
    n->text = f->text;
    return n;
  }

  Node* if_statement() {
    Node* n = make(NodeKind::If);
    next();
    if (!n || !expect(T::LParen, "Unexpected token")) return nullptr;
    n->a = expression(false);
    if (!n->a || !expect(T::RParen, "Unexpected token")) return nullptr;
    n->b = statement();
    if (!n->b) return nullptr;
    if (accept_word("else")) {
      n->c = statement();
      if (!n->c) return nullptr;
    }
    return n;
  }

  Node* while_statement() {
    Node* n = make(NodeKind::While);
    next();
    if (!n || !expect(T::LParen, "Unexpected token")) return nullptr;
    n->a = expression(false);
    if (!n->a || !expect(T::RParen, "Unexpected token")) return nullptr;
    n->d = statement();
    return n->d ? n : nullptr;
  }

  Node* do_statement() {
    Node* n = make(NodeKind::DoWhile);
    next();
    if (!n) return nullptr;
    n->d = statement();
    if (!n->d) return nullptr;
    if (!accept_word("while") || !expect(T::LParen, "Unexpected token")) return null_fail("Unexpected token");
    n->a = expression(false);
    if (!n->a || !expect(T::RParen, "Unexpected token")) return nullptr;
    accept(T::Semi);
    return n;
  }

  Node* for_statement() {
    const uint32_t line = cur_.line;
    next();
    bool for_await = false;
    if (is_word("await") && (in_async_ || in_async_generator_)) {
      for_await = true;
      next();
    }
    if (!expect(T::LParen, "Unexpected token")) return nullptr;
    Node* init = nullptr;
    if (!is(T::Semi)) {
      if (is_word("var") || is_word("const") || (is_word("let") && let_declaration_ahead())) init = var_declaration(true);
      else {
        Node* e = expression(true);
        if (!e) return nullptr;
        init = e;
      }
      if (!init) return nullptr;
      // for (x in y) / for (x of y)
      const bool is_in = is_word("in"), is_of = is_word("of");
      if ((is_in || is_of) && ((init->kind == NodeKind::Var && init->list && !init->list->next && !init->list->a) || init->kind == NodeKind::Ident ||
                              init->kind == NodeKind::Member || init->kind == NodeKind::Index)) {
        Node* n = arena_.make(is_in ? NodeKind::ForIn : NodeKind::ForOf, line);
        if (!n) return null_fail("Out of memory");
        next();
        n->a = init;
        n->b = is_of ? assignment(false) : expression(false);
        if (!n->b || !expect(T::RParen, "Unexpected token")) return nullptr;
        n->d = statement();
        if (!n->d) return nullptr;
        if (init->kind == NodeKind::Var && init->list->b) {   // for (const [k, v] of x): the pattern is unpacked at the top of the body
          Node* source = make(NodeKind::Ident);
          Node* v = make(NodeKind::Var);
          Node* body = make(NodeKind::Block);
          if (!source || !v || !body) return nullptr;
          source->text = init->list->text;
          Sink sink;
          if (!destructure(init->list->b, source, false, &sink)) return nullptr;
          v->var_kind = init->var_kind;
          v->list = sink.head;
          v->next = n->d;
          body->list = v;
          n->d = body;
          init->list->b = nullptr;
        }
        return for_await ? lower_for_await(n, init) : n;
      }
    }
    Node* n = arena_.make(NodeKind::For, line);
    if (!n) return null_fail("Out of memory");
    n->a = init;
    if (!expect(T::Semi, "Unexpected token")) return nullptr;
    if (!is(T::Semi)) {
      n->b = expression(false);
      if (!n->b) return nullptr;
    }
    if (!expect(T::Semi, "Unexpected token")) return nullptr;
    if (!is(T::RParen)) {
      n->c = expression(false);
      if (!n->c) return nullptr;
    }
    if (!expect(T::RParen, "Unexpected token")) return nullptr;
    n->d = statement();
    return n->d ? n : nullptr;
  }

  // `await x` where an await is allowed (in an async generator it is a yield, see unary_inner())
  Node* make_await(Node* operand) {
    Node* a = make(NodeKind::Unary);
    if (!a || !operand) return nullptr;
    if (in_async_generator_) {
      a->op = Op::Yield;
      a->a = new_call(new_ident("__aw"), {operand});
      return a->a ? a : nullptr;
    }
    a->op = Op::Await;
    a->a = operand;
    return a;
  }

  // for await (target of iterable) body  becomes
  //   { const it = __async_iter(iterable); for (;;) { const r = await it.next(); if (r.done) break; target = r.value; body } }
  Node* lower_for_await(Node* loop, Node* target) {
    const StringView iterator = temp_name("%i"), step = temp_name("%r");
    Node* block_node = make(NodeKind::Block);
    Node* for_node = make(NodeKind::For);
    Node* body = make(NodeKind::Block);
    Node* guard = make(NodeKind::If);
    Node* stop = make(NodeKind::Break);
    Node* done = new_member(new_ident(step), "done");
    Node* next_call = new_call(new_member(new_ident(iterator), "next"), {});
    Node* awaited = make_await(next_call);
    if (!block_node || !for_node || !body || !guard || !stop || !done || !awaited) return nullptr;
    guard->a = done;
    guard->b = stop;
    Node* make_iterator = new_var(iterator, new_call(new_ident("__async_iter"), {loop->b}));
    Node* read_step = new_var(step, awaited);
    if (!make_iterator || !read_step) return nullptr;
    read_step->var_kind = VarKind::Const;
    make_iterator->var_kind = VarKind::Const;
    Node* value = new_member(new_ident(step), "value");
    Node* bind = nullptr;
    if (target->kind == NodeKind::Var) {
      Node* d = target->list;
      if (d->b) {   // a pattern: unpacked from the value
        Sink sink;
        if (!destructure(d->b, value, false, &sink)) return nullptr;
        bind = make(NodeKind::Var);
        if (!bind) return nullptr;
        bind->var_kind = target->var_kind;
        bind->list = sink.head;
      } else {
        bind = new_var(d->text, value);
        if (bind) bind->var_kind = target->var_kind;
      }
    } else {
      Node* assign = make(NodeKind::Assign);
      if (!assign) return nullptr;
      assign->op = Op::Assign;
      assign->a = target;
      assign->b = value;
      bind = new_statement(assign);
    }
    if (!bind) return nullptr;
    read_step->next = guard;
    guard->next = bind;
    bind->next = loop->d;
    body->list = read_step;
    for_node->d = body;
    make_iterator->next = for_node;
    block_node->list = make_iterator;
    return block_node;
  }

  Node* return_statement() {
    Node* n = make(NodeKind::Return);
    next();
    if (!n) return nullptr;
    if (!is(T::Semi) && !is(T::RBrace) && !is(T::Eof) && !cur_.newline_before) {
      n->a = expression(false);
      if (!n->a) return nullptr;
    }
    consume_semicolon();
    return n;
  }

  Node* jump_statement() {
    Node* n = make(is_word("break") ? NodeKind::Break : NodeKind::Continue);
    next();
    if (!n) return nullptr;
    if (is(T::Ident) && !cur_.newline_before && !reserved(cur_.text)) {
      n->text = cur_.text;
      next();
    }
    consume_semicolon();
    return n;
  }

  Node* throw_statement() {
    Node* n = make(NodeKind::Throw);
    next();
    if (!n) return nullptr;
    if (cur_.newline_before) return null_fail("Illegal newline after throw");
    n->a = expression(false);
    if (!n->a) return nullptr;
    consume_semicolon();
    return n;
  }

  Node* try_statement() {
    Node* n = make(NodeKind::Try);
    next();
    if (!n) return nullptr;
    n->a = block();
    if (!n->a) return nullptr;
    Node* catch_pattern = nullptr;
    if (accept_word("catch")) {
      if (accept(T::LParen)) {
        Node* pattern = nullptr;
        if (is(T::LBracket) || is(T::LBrace)) {
          pattern = is(T::LBracket) ? array_literal() : object_literal();
          if (!pattern) return nullptr;
        } else if (!is(T::Ident) || reserved(cur_.text)) {
          return null_fail("Unexpected token");
        }
        n->b = make(NodeKind::Ident);
        if (!n->b) return nullptr;
        if (pattern) {
          n->b->text = temp_name("%e");
        } else {
          n->b->text = cur_.text;
          next();
        }
        if (!expect(T::RParen, "Unexpected token")) return nullptr;
        catch_pattern = pattern;
      }
      n->c = block();
      if (!n->c) return nullptr;
      if (catch_pattern) {   // catch ({ message }): unpacked first thing in the handler
        Node* source = make(NodeKind::Ident);
        Node* v = make(NodeKind::Var);
        if (!source || !v) return nullptr;
        source->text = n->b->text;
        Sink sink;
        if (!destructure(catch_pattern, source, false, &sink)) return nullptr;
        v->var_kind = VarKind::Let;
        v->list = sink.head;
        v->next = n->c->list;
        n->c->list = v;
      }
    }
    if (accept_word("finally")) {
      n->d = block();
      if (!n->d) return nullptr;
    }
    if (!n->c && !n->d) return null_fail("Missing catch or finally after try");
    return n;
  }

  Node* switch_statement() {
    Node* n = make(NodeKind::Switch);
    next();
    if (!n || !expect(T::LParen, "Unexpected token")) return nullptr;
    n->a = expression(false);
    if (!n->a || !expect(T::RParen, "Unexpected token") || !expect(T::LBrace, "Unexpected token")) return nullptr;
    Node** tail = &n->list;
    while (!is(T::RBrace) && !is(T::Eof) && !failed_) {
      Node* c = make(NodeKind::Case);
      if (!c) return nullptr;
      if (accept_word("case")) {
        c->a = expression(false);
        if (!c->a) return nullptr;
      } else if (!accept_word("default")) {
        return null_fail("Unexpected token");
      }
      if (!expect(T::Colon, "Unexpected token")) return nullptr;
      Node** stail = &c->list;
      while (!is(T::RBrace) && !is_word("case") && !is_word("default") && !is(T::Eof) && !failed_) {
        Node* s = statement();
        if (!s) return nullptr;
        *stail = s;
        stail = &s->next;
      }
      *tail = c;
      tail = &c->next;
    }
    if (!expect(T::RBrace, "Unexpected end of input")) return nullptr;
    return n;
  }

  // --- Functions
  // After `function` (the keyword is the current token): [name] (params) { body }.
  Node* function_rest(bool expression, bool is_async = false) {
    Node* f = make(NodeKind::Function);
    if (!f) return nullptr;
    if (is_async) f->flags |= kFlagAsync;
    next();   // `function`
    const bool generator = accept(T::Star);
    if (generator) f->flags |= kFlagGenerator;
    if (is(T::Ident) && !reserved(cur_.text)) {
      f->text = cur_.text;
      next();
    }
    if (expression) f->flags |= kFlagExpression;
    if (!parameters(f)) return nullptr;
    f->a = function_body(is_async, generator);
    if (f->a && is_async && generator) wrap_async_generator(f);
    return f->a ? f : nullptr;
  }

  // async function* f(a) { body }  becomes  function f(a) { return __async_gen((function* () { body }).apply(this, arguments)); }: the body is a plain
  // generator (an `await` in it was made a yield), and the driver in the standard library turns it into an async iterator.
  bool wrap_async_generator(Node* f) {
    Node* inner = make(NodeKind::Function);
    Node* body = make(NodeKind::Block);
    Node* ret = make(NodeKind::Return);
    Node* self = make(NodeKind::This);
    if (!inner || !body || !ret || !self) return false;
    inner->flags = kFlagGenerator | kFlagExpression;
    inner->a = f->a;
    Node* apply = new_call(new_member(inner, "apply"), {self, new_ident("arguments")});
    Node* wrapped = apply ? new_call(new_ident("__async_gen"), {apply}) : nullptr;
    if (!wrapped) return false;
    ret->a = wrapped;
    body->list = ret;
    f->a = body;
    f->flags &= static_cast<uint8_t>(~(kFlagAsync | kFlagGenerator));
    return true;
  }

  bool parameters(Node* f) {
    if (!expect(T::LParen, "Unexpected token")) return false;
    Node** tail = &f->list;
    Node* prologue = nullptr;   // the statements that unpack the parameters that are patterns
    Node** prologue_tail = &prologue;
    while (!is(T::RParen) && !is(T::Eof)) {
      bool rest = false;
      if (accept(T::Ellipsis)) rest = true;
      Node* pattern = nullptr;
      if (is(T::LBracket) || is(T::LBrace)) {   // function f({ a, b }, [c, d]) - the parameter has a name of its own, the pattern is unpacked first thing in the body
        pattern = is(T::LBracket) ? array_literal() : object_literal();
        if (!pattern) return false;
      } else if (!is(T::Ident) || reserved(cur_.text)) {
        fail("Unexpected token");
        return false;
      }
      Node* p = make(NodeKind::Ident);
      if (!p) return false;
      if (pattern) {
        p->text = temp_name("%p");
      } else {
        p->text = cur_.text;
        next();
      }
      if (accept(T::Assign)) {
        p->a = assignment(false);
        if (!p->a) return false;
      }
      *tail = p;
      tail = &p->next;
      if (pattern) {
        Node* source = make(NodeKind::Ident);
        if (!source) return false;
        source->text = p->text;
        Sink sink;
        if (!destructure(pattern, source, false, &sink)) return false;
        Node* v = make(NodeKind::Var);
        if (!v) return false;
        v->var_kind = VarKind::Let;
        v->list = sink.head;
        *prologue_tail = v;
        prologue_tail = &v->next;
      }
      if (rest) {
        f->flags |= kFlagRest;
        break;
      }
      if (!accept(T::Comma)) break;
    }
    if (!expect(T::RParen, "Unexpected token")) return false;
    pending_prologue_ = prologue;
    return true;
  }

  Node* function_body(bool is_async = false, bool is_generator = false) {
    if (!is(T::LBrace)) return null_fail("Unexpected token");
    Node* prologue = pending_prologue_;
    pending_prologue_ = nullptr;
    const bool saved = in_function_, saved_async = in_async_, saved_generator = in_generator_, saved_async_generator = in_async_generator_;
    in_function_ = true;
    in_async_ = is_async;
    in_generator_ = is_generator;
    in_async_generator_ = is_async && is_generator;   // (await becomes a yield the driver of the async generator understands)
    Node* b = block();
    in_function_ = saved;
    in_async_ = saved_async;
    in_generator_ = saved_generator;
    in_async_generator_ = saved_async_generator;
    if (b && prologue) {
      Node* last = prologue;
      while (last->next) last = last->next;
      last->next = b->list;
      b->list = prologue;
    }
    return b;
  }

  // An arrow function from `(`: is `(` ... `)` followed by `=>`?
  bool arrow_ahead() {
    const Mark m = mark();
    const bool before = failed_;
    int depth = 0;
    bool yes = false;
    while (!is(T::Eof) && !failed_) {
      if (is(T::LParen) || is(T::LBracket) || is(T::LBrace)) ++depth;
      else if (is(T::RParen) || is(T::RBracket) || is(T::RBrace)) {
        if (--depth == 0) {
          next();
          yes = is(T::Arrow) && !cur_.newline_before;
          break;
        }
      } else if (is(T::Backtick)) {
        break;   // (templates are not scanned: not an arrow parameter list)
      }
      next();
    }
    rewind(m);
    failed_ = before;
    return yes;
  }

  Node* arrow_function(bool from_parens, bool is_async = false) {
    Node* f = make(NodeKind::Function);
    if (!f) return nullptr;
    f->flags |= kFlagArrow;
    if (is_async) f->flags |= kFlagAsync;
    if (from_parens) {
      if (!parameters(f)) return nullptr;
    } else {
      Node* p = make(NodeKind::Ident);
      if (!p) return nullptr;
      p->text = cur_.text;
      next();
      f->list = p;
    }
    if (!expect(T::Arrow, "Unexpected token")) return nullptr;
    if (is(T::LBrace)) {
      f->a = function_body(is_async);
    } else {
      Node* prologue = pending_prologue_;
      pending_prologue_ = nullptr;
      f->flags |= kFlagExprBody;
      const bool saved = in_function_, saved_async = in_async_, saved_generator = in_generator_, saved_async_generator = in_async_generator_;
      in_function_ = true;
      in_async_ = is_async;
      in_generator_ = false;
      in_async_generator_ = false;
      f->a = assignment(false);
      in_function_ = saved;
      in_async_ = saved_async;
      in_generator_ = saved_generator;
      in_async_generator_ = saved_async_generator;
      if (f->a && prologue) {   // (parameters that are patterns: the expression becomes a block that unpacks them and returns the value)
        Node* block_node = make(NodeKind::Block);
        Node* ret = make(NodeKind::Return);
        if (!block_node || !ret) return nullptr;
        ret->a = f->a;
        Node* last = prologue;
        while (last->next) last = last->next;
        last->next = ret;
        block_node->list = prologue;
        f->a = block_node;
        f->flags &= static_cast<uint8_t>(~kFlagExprBody);
      }
    }
    return f->a ? f : nullptr;
  }

  // --- Expressions
  Node* expression(bool no_in) {
    Node* e = assignment(no_in);
    if (!e || !is(T::Comma)) return e;
    Node* seq = make(NodeKind::Sequence);
    if (!seq) return nullptr;
    seq->list = e;
    Node** tail = &e->next;
    while (accept(T::Comma)) {
      Node* n = assignment(no_in);
      if (!n) return nullptr;
      *tail = n;
      tail = &n->next;
    }
    return seq;
  }

  static bool assign_op(T t, Op* op) {
    switch (t) {
      case T::Assign: *op = Op::Assign; return true;
      case T::PlusAssign: *op = Op::Add; return true;
      case T::MinusAssign: *op = Op::Sub; return true;
      case T::StarAssign: *op = Op::Mul; return true;
      case T::SlashAssign: *op = Op::Div; return true;
      case T::PercentAssign: *op = Op::Mod; return true;
      case T::StarStarAssign: *op = Op::Pow; return true;
      case T::ShlAssign: *op = Op::Shl; return true;
      case T::ShrAssign: *op = Op::Shr; return true;
      case T::UShrAssign: *op = Op::UShr; return true;
      case T::AmpAssign: *op = Op::BitAnd; return true;
      case T::PipeAssign: *op = Op::BitOr; return true;
      case T::CaretAssign: *op = Op::BitXor; return true;
      default: return false;
    }
  }

  Node* assignment(bool no_in) {
    Depth guard(*this);
    return guard.ok ? assignment_inner(no_in) : nullptr;
  }

  Node* assignment_inner(bool no_in) {
    if (failed_) return nullptr;
    if (in_generator_ && is_word("yield")) {   // yield, yield value, yield* iterable
      Node* y = make(NodeKind::Unary);
      if (!y) return nullptr;
      y->op = Op::Yield;
      next();
      if (accept(T::Star)) y->flags |= kFlagDelegate;
      const bool operand = !(cur_.newline_before && !(y->flags & kFlagDelegate)) && !is(T::RParen) && !is(T::RBracket) && !is(T::RBrace) && !is(T::Comma) && !is(T::Semi) &&
                           !is(T::Colon) && !is(T::Eof);
      if (operand || (y->flags & kFlagDelegate)) {
        y->a = assignment(no_in);
        if (!y->a) return nullptr;
      }
      return y;
    }
    // Async arrow functions: `async x => ...` and `async (...) => ...`.
    if (is_word("async")) {
      const Mark m = mark();
      next();
      if (!cur_.newline_before) {
        if (is(T::Ident) && !reserved(cur_.text)) {
          next();
          const bool arrow = is(T::Arrow) && !cur_.newline_before;
          rewind(m);
          if (arrow) {
            next();   // `async`
            return arrow_function(false, true);
          }
        } else if (is(T::LParen) && arrow_ahead()) {
          rewind(m);
          next();   // `async`
          return arrow_function(true, true);
        }
      }
      rewind(m);
    }
    // Arrow functions: `x => ...` and `(...) => ...`.
    if (is(T::Ident) && !reserved(cur_.text)) {
      const Mark m = mark();
      next();
      const bool arrow = is(T::Arrow) && !cur_.newline_before;
      rewind(m);
      if (arrow) return arrow_function(false);
    } else if (is(T::LParen) && arrow_ahead()) {
      return arrow_function(true);
    }
    Node* left = conditional(no_in);
    if (!left) return nullptr;
    if (cur_.kind == T::AmpAmpAssign || cur_.kind == T::PipePipeAssign || cur_.kind == T::QQAssign) {   // a ||= b  is  a || (a = b)
      const Op logical = cur_.kind == T::AmpAmpAssign ? Op::And : cur_.kind == T::PipePipeAssign ? Op::Or : Op::Nullish;
      if (left->kind != NodeKind::Ident && left->kind != NodeKind::Member && left->kind != NodeKind::Index) return null_fail("Invalid left-hand side in assignment");
      Node* node = make(NodeKind::Logical);
      if (!node) return nullptr;
      next();
      node->flags |= kFlagLogicalAssign;
      node->op = logical;
      node->a = left;
      node->b = assignment(no_in);
      return node->b ? node : nullptr;
    }
    Op op;
    if (assign_op(cur_.kind, &op)) {
      if (op == Op::Assign && (left->kind == NodeKind::ArrayLit || left->kind == NodeKind::ObjectLit)) {   // [a, b] = value
        next();
        Node* value = assignment(no_in);
        return value ? destructuring_assignment(left, value) : nullptr;
      }
      if (left->kind != NodeKind::Ident && left->kind != NodeKind::Member && left->kind != NodeKind::Index) return null_fail("Invalid left-hand side in assignment");
      Node* n = make(NodeKind::Assign);
      if (!n) return nullptr;
      next();
      n->op = op;
      n->a = left;
      n->b = assignment(no_in);
      return n->b ? n : nullptr;
    }
    return left;
  }

  Node* conditional(bool no_in) {
    Node* test = binary(1, no_in);
    if (!test || !is(T::Question)) return test;
    Node* n = make(NodeKind::Conditional);
    if (!n) return nullptr;
    next();
    n->a = test;
    n->b = assignment(false);
    if (!n->b || !expect(T::Colon, "Unexpected token")) return nullptr;
    n->c = assignment(no_in);
    return n->c ? n : nullptr;
  }

  // The precedence of the binary operator at the current token (0: none) and what it is.
  int binary_precedence(bool no_in, Op* op, bool* logical) {
    *logical = false;
    switch (cur_.kind) {
      case T::QQ: *op = Op::Nullish; *logical = true; return 1;
      case T::PipePipe: *op = Op::Or; *logical = true; return 2;
      case T::AmpAmp: *op = Op::And; *logical = true; return 3;
      case T::Pipe: *op = Op::BitOr; return 4;
      case T::Caret: *op = Op::BitXor; return 5;
      case T::Amp: *op = Op::BitAnd; return 6;
      case T::Eq: *op = Op::Eq; return 7;
      case T::Ne: *op = Op::Ne; return 7;
      case T::StrictEq: *op = Op::StrictEq; return 7;
      case T::StrictNe: *op = Op::StrictNe; return 7;
      case T::Lt: *op = Op::Lt; return 8;
      case T::Gt: *op = Op::Gt; return 8;
      case T::Le: *op = Op::Le; return 8;
      case T::Ge: *op = Op::Ge; return 8;
      case T::Ident:
        if (cur_.text.equals("instanceof")) { *op = Op::InstanceOf; return 8; }
        if (cur_.text.equals("in") && !no_in) { *op = Op::In; return 8; }
        return 0;
      case T::Shl: *op = Op::Shl; return 9;
      case T::Shr: *op = Op::Shr; return 9;
      case T::UShr: *op = Op::UShr; return 9;
      case T::Plus: *op = Op::Add; return 10;
      case T::Minus: *op = Op::Sub; return 10;
      case T::Star: *op = Op::Mul; return 11;
      case T::Slash: *op = Op::Div; return 11;
      case T::Percent: *op = Op::Mod; return 11;
      case T::StarStar: *op = Op::Pow; return 12;
      default: return 0;
    }
  }

  Node* binary(int min_precedence, bool no_in) {
    Depth guard(*this);
    if (!guard.ok) return nullptr;
    Node* left = unary();
    if (!left) return nullptr;
    for (uint32_t chain = 0;; ++chain) {
      if (chain > 2500) return null_fail("Expression too long");
      Op op;
      bool logical;
      const int precedence = binary_precedence(no_in, &op, &logical);
      if (precedence == 0 || precedence < min_precedence) return left;
      Node* n = make(logical ? NodeKind::Logical : NodeKind::Binary);
      if (!n) return nullptr;
      next();
      n->op = op;
      n->a = left;
      n->b = binary(op == Op::Pow ? precedence : precedence + 1, no_in);   // (** is right associative)
      if (!n->b) return nullptr;
      left = n;
    }
  }

  Node* unary() {
    Depth guard(*this);
    return guard.ok ? unary_inner() : nullptr;
  }

  Node* unary_inner() {
    Op op = Op::None;
    switch (cur_.kind) {
      case T::Bang: op = Op::Not; break;
      case T::Tilde: op = Op::BitNot; break;
      case T::Minus: op = Op::Neg; break;
      case T::Plus: op = Op::Plus; break;
      case T::Ident:
        if (cur_.text.equals("typeof")) op = Op::TypeOf;
        else if (cur_.text.equals("void")) op = Op::Void;
        else if (cur_.text.equals("delete")) op = Op::Delete;
        else if (in_async_ && cur_.text.equals("await")) op = Op::Await;
        break;
      case T::Inc:
      case T::Dec: {
        Node* n = make(NodeKind::Update);
        if (!n) return nullptr;
        n->op = is(T::Inc) ? Op::Inc : Op::Dec;
        n->flags = kFlagPrefix;
        next();
        n->a = unary();
        if (!n->a) return nullptr;
        if (n->a->kind != NodeKind::Ident && n->a->kind != NodeKind::Member && n->a->kind != NodeKind::Index) return null_fail("Invalid left-hand side expression in prefix operation");
        return n;
      }
      default: break;
    }
    if (op == Op::Await && in_async_generator_) {   // in an async generator `await x` is `yield __aw(x)`: the driver waits for x and resumes the generator
      Node* y = make(NodeKind::Unary);
      if (!y) return nullptr;
      next();
      Node* operand = unary();
      if (!operand) return nullptr;
      y->op = Op::Yield;
      y->a = new_call(new_ident("__aw"), {operand});
      return y->a ? y : nullptr;
    }
    if (op != Op::None) {
      Node* n = make(NodeKind::Unary);
      if (!n) return nullptr;
      next();
      n->op = op;
      n->a = unary();
      return n->a ? n : nullptr;
    }
    return postfix();
  }

  Node* postfix() {
    Node* e = call_or_member();
    if (!e) return nullptr;
    if ((is(T::Inc) || is(T::Dec)) && !cur_.newline_before) {
      if (e->kind != NodeKind::Ident && e->kind != NodeKind::Member && e->kind != NodeKind::Index) return null_fail("Invalid left-hand side expression in postfix operation");
      Node* n = make(NodeKind::Update);
      if (!n) return nullptr;
      n->op = is(T::Inc) ? Op::Inc : Op::Dec;
      next();
      n->a = e;
      return n;
    }
    return e;
  }

  bool arguments(Node* call) {
    if (!expect(T::LParen, "Unexpected token")) return false;
    Node** tail = &call->list;
    while (!is(T::RParen) && !is(T::Eof)) {
      Node* arg;
      if (is(T::Ellipsis)) {
        arg = make(NodeKind::Spread);
        if (!arg) return false;
        next();
        arg->a = assignment(false);
        if (!arg->a) return false;
      } else {
        arg = assignment(false);
        if (!arg) return false;
      }
      *tail = arg;
      tail = &arg->next;
      if (!accept(T::Comma)) break;
    }
    return expect(T::RParen, "Unexpected token");
  }

  // --- Destructuring
  // A pattern is read as an expression (an array or object literal, with defaults as assignments) and then taken apart into a list of simple steps: a
  // temporary for each value that is used more than once, and a declarator (or an assignment) for each name. `declare` tells whether the names are declared by
  // the list (let / const / var: the steps are Declarator nodes) or already exist (the steps are statements).
  struct Sink {
    Node* head = nullptr;
    Node** tail = &head;
    void push(Node* n) {
      *tail = n;
      tail = &n->next;
    }
  };

  StringView temp_name(const char* prefix) {
    String name(arena_.allocator());
    name.append(prefix);
    name.append_uint(temp_counter_++);
    return arena_.copy(name.view());
  }

  Node* void_zero() {
    Node* u = make(NodeKind::Unary);
    Node* zero = make(NodeKind::Number);
    if (!u || !zero) return nullptr;
    u->op = Op::Void;
    u->a = zero;
    return u;
  }

  bool bind_temp(StringView name, Node* value, bool declare, Sink* sink) {
    Node* d = make(NodeKind::Declarator);
    if (!d || !value) return false;
    d->text = name;
    d->a = value;
    if (declare) {   // the pieces of a statement list
      Node* v = make(NodeKind::Var);
      if (!v) return false;
      v->var_kind = VarKind::Let;
      v->list = d;
      sink->push(v);
    } else {
      sink->push(d);
    }
    return true;
  }

  bool bind_target(Node* target, Node* value, bool statements, Sink* sink) {
    if (!value) return false;
    if (!statements) {
      if (target->kind != NodeKind::Ident) {
        fail("Invalid destructuring assignment target");
        return false;
      }
      Node* d = make(NodeKind::Declarator);
      if (!d) return false;
      d->text = target->text;
      d->a = value;
      sink->push(d);
      return true;
    }
    Node* assign = make(NodeKind::Assign);
    Node* statement = make(NodeKind::ExprStmt);
    if (!assign || !statement) return false;
    assign->op = Op::Assign;
    assign->a = target;
    assign->b = value;
    statement->a = assign;
    sink->push(statement);
    return true;
  }

  // Takes `pattern` apart, given the expression `value` (which is evaluated once). `statements`: assign to existing targets instead of declaring names.
  bool destructure(Node* pattern, Node* value, bool statements, Sink* sink) {
    Depth guard(*this);
    if (!guard.ok || !pattern || !value) return false;
    switch (pattern->kind) {
      case NodeKind::Ident:
      case NodeKind::Member:
      case NodeKind::Index: return bind_target(pattern, value, statements, sink);
      case NodeKind::Call:   // { a: [b] = [1] }: the parser has already lowered `[b] = [1]`
        if (!(pattern->flags & kFlagPatternAssign) || !pattern->c) break;
        return destructure(pattern->c, value, statements, sink);
      case NodeKind::Assign: {   // target = default
        if (pattern->op != Op::Assign) break;
        const StringView tmp = temp_name("%t");
        if (!bind_temp(tmp, value, statements, sink)) return false;
        Node* cond = make(NodeKind::Conditional);
        Node* test = make(NodeKind::Binary);
        Node* a = new_ident(tmp);
        Node* b = new_ident(tmp);
        Node* zero = void_zero();
        if (!cond || !test || !a || !b || !zero) return false;
        test->op = Op::StrictEq;
        test->a = a;
        test->b = zero;
        cond->a = test;
        cond->b = pattern->b;
        cond->c = b;
        return destructure(pattern->a, cond, statements, sink);
      }
      case NodeKind::ArrayLit: {
        const StringView t = temp_name("%t");
        int64_t wanted = 0;
        bool has_rest = false;
        for (Node* e = pattern->list; e; e = e->next) {
          if (e->kind == NodeKind::Spread) has_rest = true;
          else ++wanted;
        }
        Node* count = number_node(has_rest ? -1 : static_cast<double>(wanted));
        Node* call = new_call(new_ident("__to_array"), {value, count});
        if (!call || !bind_temp(t, call, statements, sink)) return false;
        uint32_t index = 0;
        for (Node* e = pattern->list; e; e = e->next, ++index) {
          Node* element = nullptr;
          if (e->kind == NodeKind::Spread) {
            Node* slice = new_call(new_member(new_ident(t), "slice"), {number_node(index)});
            if (!slice || !destructure(e->a, slice, statements, sink)) return false;
            continue;
          }
          if (e->kind == NodeKind::Ident && e->text.equals("undefined")) continue;   // a hole
          element = make(NodeKind::Index);
          if (!element) return false;
          element->a = new_ident(t);
          element->b = number_node(index);
          if (!element->a || !element->b) return false;
          Node* detached = e;
          Node* saved_next = detached->next;
          detached->next = nullptr;
          const bool ok = destructure(detached, element, statements, sink);
          detached->next = saved_next;
          if (!ok) return false;
        }
        return true;
      }
      case NodeKind::ObjectLit: {
        const StringView t = temp_name("%t");
        Node* call = new_call(new_ident("__req_object"), {value});
        if (!call || !bind_temp(t, call, statements, sink)) return false;
        Node* keys = make(NodeKind::ArrayLit);   // the names taken so far, for a rest element
        Node** key_tail = &keys->list;
        if (!keys) return false;
        for (Node* p = pattern->list; p; p = p->next) {
          if (p->kind == NodeKind::Spread) {
            Node* rest = new_call(new_ident("__rest"), {new_ident(t), keys});
            return rest && destructure(p->a, rest, statements, sink);
          }
          Node* source;
          Node* key_for_rest;
          if (p->flags & kFlagComputed) {
            const StringView kt = temp_name("%t");
            if (!bind_temp(kt, p->b, statements, sink)) return false;
            source = make(NodeKind::Index);
            if (!source) return false;
            source->a = new_ident(t);
            source->b = new_ident(kt);
            key_for_rest = new_ident(kt);
          } else {
            source = new_member(new_ident(t), p->text);
            key_for_rest = new_string(p->text);
          }
          if (!source || !key_for_rest) return false;
          *key_tail = key_for_rest;
          key_tail = &key_for_rest->next;
          Node* target = p->a;
          if (p->c) {   // { name = default }
            Node* with_default = make(NodeKind::Assign);
            if (!with_default) return false;
            with_default->op = Op::Assign;
            with_default->a = p->a;
            with_default->b = p->c;
            target = with_default;
          }
          if (!destructure(target, source, statements, sink)) return false;
        }
        return true;
      }
      default: break;
    }
    fail("Invalid destructuring assignment target");
    return false;
  }

  // [a, b] = value as an expression: an arrow function that takes the value and assigns the pieces runs at once, and gives the value back.
  Node* destructuring_assignment(Node* pattern, Node* value) {
    Node* fn = make(NodeKind::Function);
    Node* body = make(NodeKind::Block);
    Node* param = make(NodeKind::Ident);
    if (!fn || !body || !param) return nullptr;
    const StringView whole = temp_name("%a");
    param->text = whole;
    Node* source = new_ident(whole);
    Sink sink;
    if (!source || !destructure(pattern, source, true, &sink)) return nullptr;
    Node* ret = make(NodeKind::Return);
    Node* result = new_ident(whole);
    if (!ret || !result) return nullptr;
    ret->a = result;
    sink.push(ret);
    body->list = sink.head;
    fn->flags |= kFlagArrow;
    fn->list = param;
    fn->a = body;
    Node* call = new_call(fn, {value});
    Node* original = make(NodeKind::Assign);   // a nested `[b] = [1]` inside a pattern is a default value: destructure() takes the pattern apart again from this
    if (!call || !original) return nullptr;
    original->op = Op::Assign;
    original->a = pattern;
    original->b = value;
    call->c = original;
    call->flags |= kFlagPatternAssign;
    return call;
  }

  // --- Classes
  // `class` is turned into ordinary code: a function expression called at once, which makes the constructor function, chains the prototypes with __class(), puts
  // the methods on with __def() (getters and setters too), sets the static fields and returns the constructor. `super` is the variable %S of that function.
  Node* new_ident(StringView name) {
    Node* n = make(NodeKind::Ident);
    if (n) n->text = name;
    return n;
  }
  Node* new_string(StringView text) {
    Node* n = make(NodeKind::String);
    if (n) n->text = text;
    return n;
  }
  Node* new_member(Node* object, StringView name) {
    Node* n = make(NodeKind::Member);
    if (!n) return nullptr;
    n->a = object;
    n->text = name;
    return n;
  }
  // callee(arguments...): the arguments are given as a null terminated array of nodes
  Node* new_call(Node* callee, std::initializer_list<Node*> arguments) {
    Node* n = make(NodeKind::Call);
    if (!n || !callee) return nullptr;
    n->a = callee;
    Node** tail = &n->list;
    for (Node* a : arguments) {
      if (!a) return nullptr;
      *tail = a;
      tail = &a->next;
    }
    return n;
  }
  Node* new_statement(Node* expression) {
    if (!expression) return nullptr;
    Node* n = make(NodeKind::ExprStmt);
    if (n) n->a = expression;
    return n;
  }
  Node* new_var(StringView name, Node* init) {
    Node* v = make(NodeKind::Var);
    Node* d = make(NodeKind::Declarator);
    if (!v || !d || !init) return nullptr;
    v->var_kind = VarKind::Var;
    d->text = name;
    d->a = init;
    v->list = d;
    return v;
  }

  struct ClassMember {
    Node* key = nullptr;       // a String node, or the computed key expression
    Node* value = nullptr;     // the function, or the field's initializer (null: none)
    bool is_static = false, is_field = false, computed = false, is_private = false, is_block = false;   // is_block: static { ... }
    uint8_t accessor = 0;      // 1 getter, 2 setter
    ClassMember* next = nullptr;
  };

  Node* class_declaration() {
    next();   // `class`
    if (!is(T::Ident) || reserved(cur_.text)) return null_fail("Unexpected token");
    const StringView name = cur_.text;
    Node* value = class_body(name);
    if (!value) return nullptr;
    Node* v = make(NodeKind::Var);
    Node* d = make(NodeKind::Declarator);
    if (!v || !d) return nullptr;
    v->var_kind = VarKind::Let;
    d->text = name;
    d->a = value;
    v->list = d;
    return v;
  }

  Node* class_expression() {
    next();   // `class`
    StringView name;
    if (is(T::Ident) && !reserved(cur_.text) && !is_word("extends")) name = cur_.text;
    return class_body(name);
  }

  // At the class name (or at `extends` / `{` for a class without one).
  Node* class_body(StringView name) {
    StrictScope strict(strict_parse_);
    if (!name.empty()) next();
    Node* parent = nullptr;
    if (is_word("extends")) {
      next();
      parent = call_or_member();
      if (!parent) return nullptr;
    }
    if (!expect(T::LBrace, "Unexpected token")) return nullptr;
    const StringView saved_super = class_super_;
    const bool saved_static = class_static_;
    class_super_ = parent ? StringView("%S") : StringView();
    const StringView cname = name.empty() ? StringView("%C") : name;
    Node* constructor = nullptr;
    ClassMember* first = nullptr;
    ClassMember** tail = &first;
    while (!is(T::RBrace) && !is(T::Eof) && !failed_) {
      if (accept(T::Semi)) continue;
      void* mem = arena_.allocate(sizeof(ClassMember));
      if (!mem) return null_fail("Out of memory");
      ClassMember* m = new (mem) ClassMember();
      Node* holder = make(NodeKind::Property);   // (property_name() fills a Property node)
      if (!holder) return nullptr;
      bool word = false;
      bool async_method = false, generator_method = false;
      if (is_word("static")) {
        const Mark mk = mark();
        next();
        if (is(T::LParen) || is(T::Assign) || is(T::Semi) || is(T::RBrace)) rewind(mk);   // a member that is called "static"
        else m->is_static = true;
      }
      if (m->is_static && is(T::LBrace)) {   // static { ... }: runs when the class is made, with `this` the class
        class_static_ = true;
        Node* f = make(NodeKind::Function);
        if (!f) return nullptr;
        f->flags |= kFlagExpression;
        f->a = function_body(false, false);
        class_static_ = saved_static;
        if (!f->a) return nullptr;
        m->is_block = true;
        m->value = f;
        *tail = m;
        tail = &m->next;
        continue;
      }
      if (accept(T::Star)) generator_method = true;
      if (!property_name(holder, &word)) return nullptr;
      if (word && !(holder->flags & kFlagComputed) && holder->text.equals("async") && !cur_.newline_before && !is(T::LParen) && !is(T::Assign) && !is(T::Semi) && !is(T::RBrace)) {
        async_method = true;
        if (accept(T::Star)) generator_method = true;
        if (!property_name(holder, &word)) return nullptr;
      } else if (word && !(holder->flags & kFlagComputed) && (holder->text.equals("get") || holder->text.equals("set")) && !is(T::LParen) && !is(T::Assign) && !is(T::Semi) &&
                 !is(T::RBrace)) {
        m->accessor = holder->text.equals("get") ? 1 : 2;
        if (!property_name(holder, &word)) return nullptr;
      }
      m->computed = (holder->flags & kFlagComputed) != 0;
      m->is_private = !m->computed && !holder->text.empty() && holder->text[0] == '#';
      m->key = m->computed ? holder->b : new_string(holder->text);
      if (!m->key) return nullptr;
      class_static_ = m->is_static;
      if (is(T::LParen)) {   // a method (or the constructor)
        Node* f = make(NodeKind::Function);
        if (!f) return nullptr;
        f->text = m->computed ? StringView() : holder->text;
        if (async_method) f->flags |= kFlagAsync;
        if (generator_method) f->flags |= kFlagGenerator;
        if (!parameters(f)) return nullptr;
        f->a = function_body(async_method, generator_method);
        if (!f->a) return nullptr;
        if (async_method && generator_method) wrap_async_generator(f);
        m->value = f;
        if (!m->is_static && !m->accessor && !m->computed && holder->text.equals("constructor")) {
          constructor = f;
          class_static_ = saved_static;
          continue;
        }
      } else {   // a field: name = value;
        m->is_field = true;
        if (accept(T::Assign)) {
          const bool saved_fn = in_function_;
          in_function_ = true;
          m->value = assignment(false);
          in_function_ = saved_fn;
          if (!m->value) return nullptr;
        }
        consume_semicolon();
      }
      class_static_ = saved_static;
      *tail = m;
      tail = &m->next;
    }
    class_super_ = saved_super;
    class_static_ = saved_static;
    if (!expect(T::RBrace, "Unexpected token")) return nullptr;

    // the constructor: its own, or an empty one (a derived class passes its arguments on)
    Node* ctor = constructor;
    if (!ctor) {
      ctor = make(NodeKind::Function);
      Node* body = make(NodeKind::Block);
      if (!ctor || !body) return nullptr;
      ctor->a = body;
      if (parent) {
        Node* rest = new_ident("args");
        Node* spread = make(NodeKind::Spread);
        if (!rest || !spread) return nullptr;
        spread->a = new_ident("args");
        Node* self = make(NodeKind::This);
        if (!spread->a || !self) return nullptr;
        ctor->list = rest;
        ctor->flags |= kFlagRest;
        body->list = new_statement(new_call(new_ident("__super_init"), {new_ident("%S"), self, spread}));
        if (!body->list) return nullptr;
      }
    }
    ctor->text = cname;
    ctor->flags |= kFlagExpression;
    {   // a class constructor cannot be called without new
      Node* target = make(NodeKind::Ident);
      if (!target) return nullptr;
      target->text = "new.target";
      Node* check = new_statement(new_call(new_ident("__class_check"), {target, new_string(cname)}));
      if (!check) return nullptr;
      check->next = ctor->a->list;
      ctor->a->list = check;
    }
    // the instance fields go in first (after super(...) in a derived class)
    Node* fields = nullptr;
    Node** field_tail = &fields;
    for (ClassMember* m = first; m; m = m->next) {
      if (m->is_block || !m->is_field || m->is_static) continue;
      Node* self = make(NodeKind::This);
      if (!self) return nullptr;
      Node* init = m->value ? m->value : make(NodeKind::Ident);
      if (init && !m->value) init->text = "undefined";
      Node* statement;
      if (m->is_private) {
        Node* self2 = make(NodeKind::This);
        statement = new_statement(new_call(new_ident("__def"), {self2, m->key, init, number_node(3)}));
      } else {
        Node* target = make(m->computed ? NodeKind::Index : NodeKind::Member);
        Node* assign = make(NodeKind::Assign);
        if (!target || !assign) return nullptr;
        target->a = self;
        if (m->computed) target->b = m->key;
        else target->text = m->key->text;
        assign->op = Op::Assign;
        assign->a = target;
        assign->b = init;
        statement = new_statement(assign);
      }
      if (!statement) return nullptr;
      *field_tail = statement;
      field_tail = &statement->next;
    }
    if (fields) {
      Node** at = &ctor->a->list;
      if (parent) {   // after the statement that calls super
        for (Node** p = &ctor->a->list; *p; p = &(*p)->next) {
          Node* s = *p;
          if (s->kind == NodeKind::ExprStmt && s->a && s->a->kind == NodeKind::Call && s->a->a && s->a->a->kind == NodeKind::Ident && s->a->a->text.equals("__super_init")) {
            at = &s->next;
            break;
          }
        }
      }
      *field_tail = *at;
      *at = fields;
    }

    // the function that builds the class
    Node* wrapper = make(NodeKind::Function);
    Node* body = make(NodeKind::Block);
    if (!wrapper || !body) return nullptr;
    wrapper->flags |= kFlagExpression;
    wrapper->a = body;
    Node** out = &body->list;
    auto add = [&](Node* statement) -> bool {
      if (!statement) return false;
      *out = statement;
      out = &statement->next;
      return true;
    };
    if (parent && !add(new_var("%S", parent))) return nullptr;
    if (!add(new_var(cname, ctor))) return nullptr;
    Node* parent_ref = parent ? new_ident("%S") : make(NodeKind::Ident);
    if (!parent) parent_ref->text = "undefined";
    if (!add(new_var("%P", new_call(new_ident("__class"), {new_ident(cname), parent_ref})))) return nullptr;
    for (ClassMember* m = first; m; m = m->next) {
      if (m->is_block) {
        if (!add(new_statement(new_call(new_member(m->value, "call"), {new_ident(cname)})))) return nullptr;
        continue;
      }
      if (m->is_field && !m->is_static) continue;
      Node* target = new_ident(m->is_static ? cname : StringView("%P"));
      if (m->is_field) {   // static field
        Node* init = m->value ? m->value : new_ident("undefined");
        Node* statement;
        if (m->is_private) {
          statement = new_statement(new_call(new_ident("__def"), {target, m->key, init, number_node(3)}));
        } else {
          Node* member = make(m->computed ? NodeKind::Index : NodeKind::Member);
          Node* assign = make(NodeKind::Assign);
          if (!member || !assign) return nullptr;
          member->a = target;
          if (m->computed) member->b = m->key;
          else member->text = m->key->text;
          assign->op = Op::Assign;
          assign->a = member;
          assign->b = init;
          statement = new_statement(assign);
        }
        if (!add(statement)) return nullptr;
        continue;
      }
      if (!add(new_statement(new_call(new_ident("__def"), {target, m->key, m->value, number_node(m->accessor)})))) return nullptr;
    }
    Node* ret = make(NodeKind::Return);
    if (!ret) return nullptr;
    ret->a = new_ident(cname);
    if (!add(ret)) return nullptr;
    Node* call = make(NodeKind::Call);
    if (!call) return nullptr;
    call->a = wrapper;
    return call;
  }

  Node* number_node(double v) {
    Node* n = make(NodeKind::Number);
    if (n) n->number = v;
    return n;
  }

  // super(...), super.name, super[expr]
  Node* super_expression() {
    if (class_super_.empty()) return null_fail("'super' keyword unexpected here");
    next();   // `super`
    Node* self = make(NodeKind::This);
    if (!self) return nullptr;
    if (is(T::LParen)) {
      Node* call = make(NodeKind::Call);
      if (!call) return nullptr;
      call->a = new_ident("__super_init");
      Node* parent = new_ident(class_super_);
      if (!call->a || !parent) return nullptr;
      parent->next = self;
      call->list = parent;
      if (!is(T::LParen)) return null_fail("Unexpected token");
      Node* args = make(NodeKind::Call);   // (its argument list is read into a scratch call, then appended after `this`)
      if (!args || !arguments(args)) return nullptr;
      self->next = args->list;
      return call;
    }
    Node* base = class_static_ ? new_ident(class_super_) : new_member(new_ident(class_super_), "prototype");
    if (!base) return nullptr;
    if (accept(T::Dot)) {
      if (!is(T::Ident)) return null_fail("Unexpected token");
      Node* m = new_member(base, cur_.text);
      if (!m) return nullptr;
      m->flags |= kFlagSuper;
      next();
      return m;
    }
    if (accept(T::LBracket)) {
      Node* index = make(NodeKind::Index);
      if (!index) return nullptr;
      index->a = base;
      index->b = expression(false);
      if (!index->b || !expect(T::RBracket, "Unexpected token")) return nullptr;
      index->flags |= kFlagSuper;
      return index;
    }
    return null_fail("'super' keyword unexpected here");
  }

  // /pattern/flags - read again from the "/" that was taken for a division, since an operand is what is wanted here. It becomes `new RegExp("pattern", "flags")`.
  Node* regex_literal() {
    uint64_t at = prev_end_ + 1;
    bool in_class = false;
    while (at < src_.size()) {
      const char c = src_[at];
      if (c == '\n' || c == '\r') break;
      if (c == '\\') {
        at += 2;
        continue;
      }
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      else if (c == '/' && !in_class) break;
      ++at;
    }
    if (at >= src_.size() || src_[at] != '/') return null_fail("Invalid regular expression: missing /");
    const StringView body = src_.substr(prev_end_ + 1, at - prev_end_ - 1);
    uint64_t end = at + 1;
    while (end < src_.size() && is_ident_part(src_[end])) ++end;
    const StringView flags = src_.substr(at + 1, end - at - 1);
    Node* n = make(NodeKind::New);
    Node* callee = make(NodeKind::Ident);
    Node* pattern = make(NodeKind::String);
    Node* options = make(NodeKind::String);
    if (!n || !callee || !pattern || !options) return nullptr;
    callee->text = "RegExp";
    pattern->text = arena_.copy(body);
    options->text = arena_.copy(flags);
    pattern->next = options;
    n->a = callee;
    n->list = pattern;
    pos_ = end;
    next();
    return n;
  }

  // tag`text ${value} more`: tag(strings, value), the strings array carrying .raw
  Node* tagged_template(Node* tag) {
    Node* raw_chunks = nullptr;
    Node* tpl = template_literal(&raw_chunks);
    if (!tpl) return nullptr;
    Node* cooked = make(NodeKind::ArrayLit);
    Node* raw = make(NodeKind::ArrayLit);
    Node* helper = make(NodeKind::Call);
    Node* helper_name = make(NodeKind::Ident);
    Node* call = make(NodeKind::Call);
    if (!cooked || !raw || !helper || !helper_name || !call) return nullptr;
    helper_name->text = "__template";
    Node** cooked_tail = &cooked->list;
    Node* expressions = nullptr;
    Node** expression_tail = &expressions;
    bool is_string = true;
    for (Node* item = tpl->list; item;) {
      Node* following = item->next;
      item->next = nullptr;
      if (is_string) {
        *cooked_tail = item;
        cooked_tail = &item->next;
      } else {
        *expression_tail = item;
        expression_tail = &item->next;
      }
      is_string = !is_string;
      item = following;
    }
    raw->list = raw_chunks;
    helper->a = helper_name;
    helper->list = cooked;
    cooked->next = raw;
    call->a = tag;
    call->list = helper;
    helper->next = expressions;
    // the strings array comes from the helper call
    return call;
  }

  Node* call_or_member() {
    Node* e;
    if (is_word("new")) {
      const Mark after_new = mark();
      next();
      if (is(T::Dot)) {   // new.target
        next();
        if (!is_word("target")) return null_fail("Unexpected token");
        next();
        Node* target = make(NodeKind::Ident);
        if (!target) return nullptr;
        target->text = "new.target";
        return members(target, true);
      }
      rewind(after_new);
    }
    if (is_word("new")) {
      Node* n = make(NodeKind::New);
      if (!n) return nullptr;
      next();
      // new X(args): the callee is a member expression without calls.
      Node* callee = is_word("new") ? call_or_member_no_call() : primary();
      if (!callee) return nullptr;
      callee = members(callee, false);
      if (!callee) return nullptr;
      n->a = callee;
      if (is(T::LParen) && !arguments(n)) return nullptr;
      e = n;
    } else {
      e = primary();
      if (!e) return nullptr;
    }
    return members(e, true);
  }

  Node* call_or_member_no_call() { return call_or_member(); }

  // Follows `.name`, `[expr]`, `?.`, and (when allowed) `(args)` after an expression.
  Node* members(Node* e, bool calls) {
    bool optional_seen = false;
    Node* r = members_links(e, calls, &optional_seen);
    if (!r || !optional_seen) return r;
    Node* w = make(NodeKind::OptionalChain);
    if (!w) return nullptr;
    w->a = r;
    return w;
  }
  Node* members_links(Node* e, bool calls, bool* optional_seen) {
    for (uint32_t chain = 0;; ++chain) {
      if (failed_) return nullptr;
      if (chain > 2500) return null_fail("Expression too long");
      if (is(T::Dot) || is(T::QuestionDot)) {
        const bool optional = is(T::QuestionDot);
        if (optional) *optional_seen = true;
        next();
        if (optional && is(T::LParen)) {
          if (!calls) return e;
          Node* c = make(NodeKind::Call);
          if (!c) return nullptr;
          c->a = e;
          c->flags |= kFlagOptional;
          if (!arguments(c)) return nullptr;
          e = c;
          continue;
        }
        if (optional && is(T::LBracket)) {
          next();
          Node* n = make(NodeKind::Index);
          if (!n) return nullptr;
          n->a = e;
          n->flags |= kFlagOptional;
          n->b = expression(false);
          if (!n->b || !expect(T::RBracket, "Unexpected token")) return nullptr;
          e = n;
          continue;
        }
        if (!is(T::Ident)) return null_fail("Unexpected token");
        Node* n = make(NodeKind::Member);
        if (!n) return nullptr;
        n->a = e;
        n->text = cur_.text;
        if (optional) n->flags |= kFlagOptional;
        next();
        e = n;
      } else if (is(T::LBracket)) {
        Node* n = make(NodeKind::Index);
        if (!n) return nullptr;
        next();
        n->a = e;
        n->b = expression(false);
        if (!n->b || !expect(T::RBracket, "Unexpected token")) return nullptr;
        e = n;
      } else if (is(T::LParen) && calls) {
        Node* c = make(NodeKind::Call);
        if (!c) return nullptr;
        c->a = e;
        if (!arguments(c)) return nullptr;
        e = c;
      } else if (is(T::Backtick) && calls) {
        Node* c = tagged_template(e);
        if (!c) return nullptr;
        e = c;
      } else {
        return e;
      }
    }
  }

  Node* primary() {
    switch (cur_.kind) {
      case T::Number: {
        if (cur_.bigint) {   // 10n  is  BigInt("10")
          Node* literal = make(NodeKind::String);
          if (!literal) return nullptr;
          literal->text = cur_.text;
          Node* call = new_call(new_ident("BigInt"), {literal});
          next();
          return call;
        }
        Node* n = make(NodeKind::Number);
        if (!n) return nullptr;
        n->number = cur_.number;
        next();
        return n;
      }
      case T::String: {
        Node* n = make(NodeKind::String);
        if (!n) return nullptr;
        n->text = cur_.text;
        next();
        return n;
      }
      case T::Backtick: return template_literal();
      case T::LParen: {
        next();
        Node* e = expression(false);
        if (!e || !expect(T::RParen, "Unexpected token")) return nullptr;
        if (e->kind == NodeKind::String) e->flags |= kFlagParenthesized;
        return e;
      }
      case T::LBracket: return array_literal();
      case T::LBrace: return object_literal();
      case T::Ident: break;
      case T::Slash:
      case T::SlashAssign: return regex_literal();
      default: return null_fail("Unexpected token");
    }
    const StringView w = cur_.text;
    if (w.equals("import")) return import_expression();
    if (w.equals("function")) return function_rest(true);
    if (w.equals("class")) return class_expression();
    if (w.equals("super")) return super_expression();
    if (w.equals("async") && async_function_ahead()) {
      next();   // `async`
      return function_rest(true, true);
    }
    if (w.equals("this")) {
      Node* n = make(NodeKind::This);
      next();
      return n;
    }
    if (w.equals("null")) {
      Node* n = make(NodeKind::Null);
      next();
      return n;
    }
    if (w.equals("true") || w.equals("false")) {
      Node* n = make(w.equals("true") ? NodeKind::True : NodeKind::False);
      next();
      return n;
    }
    if (w.equals("class")) return null_fail("Classes are not supported");
    if (reserved(w)) return null_fail("Unexpected token");
    Node* n = make(NodeKind::Ident);
    if (!n) return nullptr;
    n->text = w;
    next();
    return n;
  }

  // import.meta (in a module) and import(specifier) (anywhere): a call of __dynamic_import(specifier, the address of the module or undefined).
  Node* import_expression() {
    next();   // `import`
    if (accept(T::Dot)) {
      if (!is_word("meta")) return null_fail("Unexpected token");
      if (!module_) return null_fail("Cannot use 'import.meta' outside a module");
      next();
      return new_ident("import.meta");
    }
    if (!is(T::LParen)) return null_fail("Unexpected token");
    next();
    Node* specifier = assignment(false);
    if (!specifier) return nullptr;
    accept(T::Comma);
    if (!expect(T::RParen, "Unexpected token")) return nullptr;
    Node* referrer = module_ ? new_member(new_ident("import.meta"), "url") : new_ident("undefined");
    return new_call(new_ident("__dynamic_import"), {specifier, referrer});
  }

  Node* array_literal() {
    Node* n = make(NodeKind::ArrayLit);
    if (!n) return nullptr;
    next();
    Node** tail = &n->list;
    while (!is(T::RBracket) && !is(T::Eof)) {
      Node* e;
      if (is(T::Comma)) {   // a hole
        e = make(NodeKind::Ident);
        if (!e) return nullptr;
        e->text = "undefined";
      } else if (is(T::Ellipsis)) {
        e = make(NodeKind::Spread);
        if (!e) return nullptr;
        next();
        e->a = assignment(false);
        if (!e->a) return nullptr;
      } else {
        e = assignment(false);
        if (!e) return nullptr;
      }
      *tail = e;
      tail = &e->next;
      if (!accept(T::Comma)) break;
    }
    return expect(T::RBracket, "Unexpected token") ? n : nullptr;
  }

  // A property name in an object literal or class body: a word, a string, a number or [computed]. Sets p->text (or p->b and kFlagComputed); *word tells it was an identifier.
  bool property_name(Node* p, bool* word) {
    *word = false;
    p->flags &= static_cast<uint8_t>(~kFlagComputed);
    if (is(T::LBracket)) {
      next();
      p->flags |= kFlagComputed;
      p->b = assignment(false);
      return p->b && expect(T::RBracket, "Unexpected token");
    }
    if (is(T::Ident) || is(T::String)) {
      *word = is(T::Ident);
      p->text = cur_.text;
      next();
      return true;
    }
    if (is(T::Number)) {
      String number(arena_.allocator());   // (the number as a property name: its text)
      number_to_string(cur_.number, &number);
      p->text = arena_.copy(number.view());
      next();
      return true;
    }
    fail("Unexpected token");
    return false;
  }

  Node* object_literal() {
    Node* n = make(NodeKind::ObjectLit);
    if (!n) return nullptr;
    next();
    Node** tail = &n->list;
    while (!is(T::RBrace) && !is(T::Eof)) {
      Node* p;
      if (is(T::Ellipsis)) {
        p = make(NodeKind::Spread);
        if (!p) return nullptr;
        next();
        p->a = assignment(false);
        if (!p->a) return nullptr;
      } else {
        p = make(NodeKind::Property);
        if (!p) return nullptr;
        bool word = false, method_async = false;
        uint8_t accessor = 0;   // 1: get name() {}, 2: set name(v) {}
        bool generator = accept(T::Star);   // *name() { ... }
        if (!property_name(p, &word)) return nullptr;
        if (word && !(p->flags & kFlagComputed) && p->text.equals("async") && !cur_.newline_before && (is(T::Ident) || is(T::String) || is(T::Star) || is(T::LBracket))) {   // async name() { ... }
          method_async = true;
          if (accept(T::Star)) generator = true;
          if (!property_name(p, &word)) return nullptr;
        } else if (word && !(p->flags & kFlagComputed) && (p->text.equals("get") || p->text.equals("set")) &&
                   (is(T::Ident) || is(T::String) || is(T::Number) || is(T::LBracket))) {   // get name() { ... } / set name(v) { ... }
          accessor = p->text.equals("get") ? 1 : 2;
          if (!property_name(p, &word)) return nullptr;
        }
        if (accessor) {
          Node* f = make(NodeKind::Function);
          if (!f) return nullptr;
          f->text = p->text;
          if (!parameters(f)) return nullptr;
          f->a = function_body(false);
          if (!f->a) return nullptr;
          p->a = f;
          p->flags |= accessor == 1 ? kFlagGetter : kFlagSetter;
        } else if (accept(T::Colon)) {
          p->a = assignment(false);
          if (!p->a) return nullptr;
        } else if (is(T::LParen)) {   // a method: name() { ... }
          Node* f = make(NodeKind::Function);
          if (!f) return nullptr;
          f->text = p->text;
          if (method_async) f->flags |= kFlagAsync;
          if (generator) f->flags |= kFlagGenerator;
          if (!parameters(f)) return nullptr;
          f->a = function_body(method_async, generator);
          if (!f->a) return nullptr;
          if (method_async && generator) wrap_async_generator(f);
          p->a = f;
        } else if (word && !(p->flags & kFlagComputed) && !reserved(p->text)) {   // shorthand: { name }
          p->flags |= kFlagShorthand;
          Node* id = make(NodeKind::Ident);
          if (!id) return nullptr;
          id->text = p->text;
          p->a = id;
          if (accept(T::Assign)) {   // { name = default }: only meaningful in a destructuring pattern
            p->c = assignment(false);
            if (!p->c) return nullptr;
          }
        } else {
          return null_fail("Unexpected token");
        }
      }
      *tail = p;
      tail = &p->next;
      if (!accept(T::Comma)) break;
    }
    return expect(T::RBrace, "Unexpected token") ? n : nullptr;
  }

  // `...${expr}...`: pos_ is after the backtick. The result's list alternates a String node and an expression, starting and ending with a String.
  Node* template_literal(Node** raw_chunks = nullptr) {
    Node* n = make(NodeKind::Template);
    if (!n) return nullptr;
    Node** tail = &n->list;
    Node** raw_tail = raw_chunks;
    for (;;) {
      scratch_.clear();
      bool expression_follows = false;
      const uint64_t chunk_start = pos_;
      for (;;) {
        if (at_end()) return null_fail("Unterminated template literal");
        const char c = peek();
        if (c == '`') {
          ++pos_;
          break;
        }
        if (c == '$' && peek(1) == '{') {
          pos_ += 2;
          expression_follows = true;
          break;
        }
        if (c == '\\') {
          ++pos_;
          read_escape(&scratch_);
          if (failed_) return nullptr;
        } else {
          if (c == '\n') ++line_;
          if (c != '\r') scratch_.push_back(c);
          ++pos_;
        }
      }
      Node* chunk = make(NodeKind::String);
      if (!chunk) return nullptr;
      chunk->text = arena_.copy(scratch_.view());
      *tail = chunk;
      tail = &chunk->next;
      if (raw_tail) {   // the text as it was written (line ends as \n)
        Node* raw_chunk = make(NodeKind::String);
        if (!raw_chunk) return nullptr;
        String raw_text(arena_.allocator());
        const uint64_t chunk_end = pos_ - (expression_follows ? 2 : 1);
        for (uint64_t i = chunk_start; i < chunk_end; ++i)
          if (src_[i] != '\r') raw_text.push_back(src_[i]);
        raw_chunk->text = arena_.copy(raw_text.view());
        *raw_tail = raw_chunk;
        raw_tail = &raw_chunk->next;
      }
      if (!expression_follows) break;
      next();   // the first token of the expression
      Node* e = expression(false);
      if (!e) return nullptr;
      if (!is(T::RBrace)) return null_fail("Unexpected token in template literal");
      // (the lexer is just after the "}": the string goes on from here)
      *tail = e;
      tail = &e->next;
    }
    next();   // the token after the template
    return n;
  }

  StringView src_;
  Arena& arena_;
  String* error_;
  String scratch_;
  uint64_t pos_ = 0, prev_end_ = 0;
  uint32_t line_ = 1;
  Token cur_;
  bool failed_ = false, in_function_ = false, in_async_ = false, in_generator_ = false, in_async_generator_ = false;
  Node* pending_prologue_ = nullptr;   // the statements that unpack the parameters of the function whose body comes next
  uint32_t temp_counter_ = 0;
  StringView class_super_;     // while a class body is parsed: the variable that holds its parent ("%S"), empty for a class without one
  bool class_static_ = false;  // ... and whether the member being parsed is static
  uint32_t depth_ = 0;
  // modules
  bool module_ = false;
  bool strict_parse_ = false;
  ModuleRequests* requests_ = nullptr;
  List<ExportEntry> exports_{arena_.allocator()};
};

}  // namespace

Node* parse_module(StringView source, Arena& arena, String* error, ModuleRequests* requests) {
  Parser parser(source, arena, error);
  return parser.module_program(requests);
}

Node* parse_program(StringView source, Arena& arena, String* error) {
  Parser parser(source, arena, error);
  return parser.program();
}

}  // namespace script

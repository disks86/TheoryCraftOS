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

// The lexer and the recursive-descent parser. The language is a practical subset of ECMAScript 5 with some of ES2015 - let / const, arrow functions,
// template literals, spread, for-of, default parameters, ?? and ?. - and left out: classes, regular expression literals, destructuring, generators,
// async functions, getters and setters, labels on blocks, `with`, `eval`.
#include "Script/Ast.hpp"
#include "Script/Number.hpp"

namespace script {

namespace {

enum class T : uint8_t {
  Eof, Number, String, Ident, Backtick,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi, Comma, Dot, Ellipsis, Question, QuestionDot, Colon, Arrow,
  Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign, StarStarAssign, ShlAssign, ShrAssign, UShrAssign, AmpAssign, PipeAssign, CaretAssign,
  Plus, Minus, Star, Slash, Percent, StarStar, Inc, Dec, Amp, Pipe, Caret, Tilde, Bang, Shl, Shr, UShr, AmpAmp, PipePipe, QQ,
  Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge,
};

struct Token {
  T kind = T::Eof;
  StringView text;        // identifiers: the name; strings: the decoded text
  double number = 0;
  bool newline_before = false;
  uint32_t line = 1;
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
  Parser(StringView source, Arena& arena, String* error) : src_(source), arena_(arena), error_(error), scratch_(arena.allocator()) {}

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
    if (is_ident_start(c)) {
      const uint64_t start = pos_;
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

  void lex_number(Token* t) {
    const uint64_t start = pos_;
    t->kind = T::Number;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' || peek(1) == 'b' || peek(1) == 'B')) {
      pos_ += 2;
      while (hex_value(peek()) >= 0) ++pos_;
      double v = 0;
      if (!string_to_number(src_.substr(start, pos_ - start), &v)) fail("Invalid number");
      t->number = v;
      return;
    }
    while (is_digit(peek())) ++pos_;
    if (peek() == '.') {
      ++pos_;
      while (is_digit(peek())) ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      const uint64_t save = pos_;
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!is_digit(peek())) pos_ = save;
      while (is_digit(peek())) ++pos_;
    }
    StringView text = src_.substr(start, pos_ - start);
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
        {">>>=", T::UShrAssign}, {"...", T::Ellipsis}, {"===", T::StrictEq}, {"!==", T::StrictNe}, {"**=", T::StarStarAssign}, {"<<=", T::ShlAssign},
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
        // A "/" here is a division or an operator; regular expression literals are not supported.
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
    return n;
  }

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
    if (cur_.kind == T::Ident) {
      const StringView w = cur_.text;
      if (w.equals("var") || w.equals("const") || (w.equals("let") && let_declaration_ahead())) {
        Node* n = var_declaration(false);
        if (n) consume_semicolon();
        return n;
      }
      if (w.equals("function")) return function_declaration();
      if (w.equals("if")) return if_statement();
      if (w.equals("for")) return for_statement();
      if (w.equals("while")) return while_statement();
      if (w.equals("do")) return do_statement();
      if (w.equals("return")) return return_statement();
      if (w.equals("break") || w.equals("continue")) return jump_statement();
      if (w.equals("throw")) return throw_statement();
      if (w.equals("try")) return try_statement();
      if (w.equals("switch")) return switch_statement();
      if (w.equals("class")) return null_fail("Classes are not supported");
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

  bool let_declaration_ahead() {
    const Mark m = mark();
    next();
    const bool yes = cur_.kind == T::Ident && !reserved(cur_.text);
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

  Node* function_declaration() {
    Node* f = function_rest(false);
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
        return n->d ? n : nullptr;
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
    if (accept_word("catch")) {
      if (accept(T::LParen)) {
        if (!is(T::Ident) || reserved(cur_.text)) return null_fail("Unexpected token");
        n->b = make(NodeKind::Ident);
        if (!n->b) return nullptr;
        n->b->text = cur_.text;
        next();
        if (!expect(T::RParen, "Destructuring in catch is not supported")) return nullptr;
      }
      n->c = block();
      if (!n->c) return nullptr;
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
  Node* function_rest(bool expression) {
    Node* f = make(NodeKind::Function);
    if (!f) return nullptr;
    next();   // `function`
    if (is(T::Ident) && !reserved(cur_.text)) {
      f->text = cur_.text;
      next();
    }
    if (expression) f->flags |= kFlagExpression;
    if (!parameters(f)) return nullptr;
    f->a = function_body();
    return f->a ? f : nullptr;
  }

  bool parameters(Node* f) {
    if (!expect(T::LParen, "Unexpected token")) return false;
    Node** tail = &f->list;
    while (!is(T::RParen) && !is(T::Eof)) {
      bool rest = false;
      if (accept(T::Ellipsis)) rest = true;
      if (!is(T::Ident) || reserved(cur_.text)) {
        fail("Unexpected token");
        return false;
      }
      Node* p = make(NodeKind::Ident);
      if (!p) return false;
      p->text = cur_.text;
      next();
      if (accept(T::Assign)) {
        p->a = assignment(false);
        if (!p->a) return false;
      }
      *tail = p;
      tail = &p->next;
      if (rest) {
        f->flags |= kFlagRest;
        break;
      }
      if (!accept(T::Comma)) break;
    }
    return expect(T::RParen, "Unexpected token");
  }

  Node* function_body() {
    if (!is(T::LBrace)) return null_fail("Unexpected token");
    const bool saved = in_function_;
    in_function_ = true;
    Node* b = block();
    in_function_ = saved;
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

  Node* arrow_function(bool from_parens) {
    Node* f = make(NodeKind::Function);
    if (!f) return nullptr;
    f->flags |= kFlagArrow;
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
      f->a = function_body();
    } else {
      f->flags |= kFlagExprBody;
      const bool saved = in_function_;
      in_function_ = true;
      f->a = assignment(false);
      in_function_ = saved;
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
    Op op;
    if (assign_op(cur_.kind, &op)) {
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

  Node* call_or_member() {
    Node* e;
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
    for (uint32_t chain = 0;; ++chain) {
      if (failed_) return nullptr;
      if (chain > 2500) return null_fail("Expression too long");
      if (is(T::Dot) || is(T::QuestionDot)) {
        const bool optional = is(T::QuestionDot);
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
        return null_fail("Tagged templates are not supported");
      } else {
        return e;
      }
    }
  }

  Node* primary() {
    switch (cur_.kind) {
      case T::Number: {
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
        return e;
      }
      case T::LBracket: return array_literal();
      case T::LBrace: return object_literal();
      case T::Ident: break;
      case T::Slash:
      case T::SlashAssign: return null_fail("Regular expression literals are not supported");
      default: return null_fail("Unexpected token");
    }
    const StringView w = cur_.text;
    if (w.equals("function")) return function_rest(true);
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
        bool word = false;
        if (is(T::LBracket)) {
          next();
          p->flags |= kFlagComputed;
          p->b = assignment(false);
          if (!p->b || !expect(T::RBracket, "Unexpected token")) return nullptr;
        } else if (is(T::Ident) || is(T::String)) {
          word = is(T::Ident);
          p->text = cur_.text;
          next();
        } else if (is(T::Number)) {
          String number(arena_.allocator());   // (the number as a property name: its text)
          number_to_string(cur_.number, &number);
          p->text = arena_.copy(number.view());
          next();
        } else {
          return null_fail("Unexpected token");
        }
        if (accept(T::Colon)) {
          p->a = assignment(false);
          if (!p->a) return nullptr;
        } else if (is(T::LParen)) {   // a method: name() { ... }
          Node* f = make(NodeKind::Function);
          if (!f) return nullptr;
          f->text = p->text;
          if (!parameters(f)) return nullptr;
          f->a = function_body();
          if (!f->a) return nullptr;
          p->a = f;
        } else if (word && !(p->flags & kFlagComputed) && !reserved(p->text)) {   // shorthand: { name }
          p->flags |= kFlagShorthand;
          Node* id = make(NodeKind::Ident);
          if (!id) return nullptr;
          id->text = p->text;
          p->a = id;
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
  Node* template_literal() {
    Node* n = make(NodeKind::Template);
    if (!n) return nullptr;
    Node** tail = &n->list;
    for (;;) {
      scratch_.clear();
      bool expression_follows = false;
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
  bool failed_ = false, in_function_ = false;
  uint32_t depth_ = 0;
};

}  // namespace

Node* parse_program(StringView source, Arena& arena, String* error) {
  Parser parser(source, arena, error);
  return parser.program();
}

}  // namespace script

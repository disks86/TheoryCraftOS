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

// A GLSL preprocessor with the observable behaviour of Mesa's glcpp. Its structure follows glcpp's flex rules and yacc grammar (glcpp-lex.l and glcpp-parse.y,
// Copyright (c) 2010 Intel Corporation, MIT licence, text in tests/data/mesa/glcpp/README.md), since what comes out, down to the column of a message, depends on them:
//
//  - Lexer: the flex rules as a hand-written longest-match scanner with the same start conditions. Every match moves the location, also those that return no token.
//  - Parser: the LALR automaton as plain code. Actions run at the moment bison would reduce, because the lexer's state depends on them. Where the grammar has a state
//    with at most four expected tokens the message lists them ("expecting NEWLINE"), as bison's verbose errors do; any syntax error ends the run, as the grammar has no
//    error recovery. A location of an expanded #if or #line is that of the line's newline, because tokens read back from a list do not move bison's location.
//  - Macro expansion: glcpp's token lists, with nodes and tokens in arenas addressed by index. Index 0 is a sentinel for "no node", so that walking off a list reads a
//    harmless token of type 0 instead of stopping the OS.
//
// Differences from glcpp, all of them where glcpp has undefined behaviour or no limit: expression stack, argument nesting, memory, work and output are bounded (exceeding
// one is a clean error); arithmetic in #if wraps; INT_MIN / -1 and shifts by an out-of-range count have a defined result and a warning.
#include "WebGl/GlslPreprocessor.hpp"

namespace shader {
namespace {

// The token numbers. Punctuation is its own character, as in the grammar.
enum : int16_t {
  kEnd = 0,
  kDefined = 258, kElifExpanded, kHash, kDefineToken, kFuncIdentifier, kObjIdentifier, kElif, kElse, kEndif, kErrorToken, kIf, kIfdef, kIfndef, kLine, kPragma, kUndef,
  kVersionToken, kGarbage, kIdentifier, kIfExpanded, kInteger, kIntegerString, kLineExpanded, kNewline, kOther, kPlaceholder, kSpace, kPlusPlus, kMinusMinus, kPath,
  kInclude, kPaste, kOr, kAnd, kEqual, kNotEqual, kLessOrEqual, kGreaterOrEqual, kLeftShift, kRightShift,
};

// Bounds on what one source can make the preprocessor do. Exceeding any is a preprocessor error that ends the run.
constexpr uint32_t kMaxNodes = 1u << 22;       // live list nodes (8 bytes each)
constexpr uint32_t kMaxTokens = 1u << 21;      // live tokens (40 bytes each)
constexpr uint64_t kBaseWork = 1ull << 26;     // steps of macro expansion, plus kWorkPerByte per byte of source
constexpr uint64_t kWorkPerByte = 64;
constexpr uint64_t kMaxPastedBytes = 1u << 24; // text made by ## in total
constexpr uint64_t kMaxOutput = 1u << 26;      // bytes of output
constexpr uint64_t kMaxLog = 1u << 20;         // bytes of log; later messages are dropped
constexpr uint32_t kMaxExpandDepth = 256;      // macro calls nested in arguments of macro calls (each costs a few stack frames; a thread has 8 MiB by default)
constexpr uint32_t kMaxExpressionDepth = 10000;  // what bison's stack would take in #if, on the heap here
constexpr uint32_t kMacroBuckets = 2048;

struct Location {
  int32_t line = 1;
  int32_t column = 1;
  uint32_t source = 0;
};

struct Token {
  StringView text;     // identifiers, numbers, paths, other text; refers to the source or to pasted text
  int64_t value = 0;   // kInteger
  Location location{0, 0, 0};
  int16_t type = kEnd;
  bool expanding = false;
};

struct Node {
  uint32_t token = 0;
  uint32_t next = 0;
};

// present == false is glcpp's NULL list, which is not the same thing as an empty one.
struct TokenList {
  uint32_t head = 0;
  uint32_t tail = 0;
  uint32_t non_space_tail = 0;
  bool present = false;
};

struct Macro {
  StringView name;
  TokenList parameters;
  TokenList replacements;
  uint32_t active = 0;  // how many times it is on the stack of macros being expanded
  uint32_t next = 0;    // in its hash bucket
  bool is_function = false;
};

struct ActiveMacro {
  uint32_t macro = 0;   // 0 for __LINE__ and __FILE__, which take no part in recursion
  uint32_t marker = 0;  // the node after this macro's expansion: reaching it ends the expansion
};

enum class SkipType : uint8_t { kNoSkip, kToElse, kToEndif };

struct SkipNode {
  Location location;
  SkipType type = SkipType::kNoSkip;
  bool has_else = false;
};

enum class ExpandMode : uint8_t { kIgnoreDefined, kEvaluateDefined };

enum class LexState : uint8_t { kInitial, kComment, kDefine, kDone, kHash, kNewlineCatchup };

struct ExpressionValue {
  int64_t value = 0;
  StringView undefined_macro;  // empty: none
};

// Growable storage addressed by index that can be emptied back to an earlier size. Slot 0 is the sentinel.
template <class T>
class Arena {
 public:
  Arena(allocators::IAllocator& allocator, uint32_t limit) : items_(allocator), limit_(limit) {}
  bool init() { return items_.push_back(T{}) && (used_ = 1); }
  // A fresh slot, or 0 if the limit or the memory ran out.
  uint32_t allocate() {
    if (used_ >= limit_) return 0;
    if (used_ < items_.count()) items_[used_] = T{};
    else if (!items_.push_back(T{})) return 0;
    return used_++;
  }
  T& operator[](uint32_t index) { return items_[index]; }
  const T& operator[](uint32_t index) const { return items_[index]; }
  uint32_t used() const { return used_; }
  void release_to(uint32_t mark) { used_ = mark; }

 private:
  List<T> items_;
  uint32_t limit_;
  uint32_t used_ = 0;
};

constexpr bool is_hspace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }
constexpr bool is_newline_char(char c) { return c == '\r' || c == '\n'; }
constexpr bool is_space(char c) { return is_hspace(c) || is_newline_char(c); }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_identifier_start(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_identifier_char(char c) { return is_identifier_start(c) || is_digit(c); }
constexpr bool is_hex_digit(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

constexpr bool is_punctuation(char c) {
  switch (c) {
    case '[': case ']': case '(': case ')': case '{': case '}': case '.': case '&': case '*': case '~': case '!': case '/': case '%': case '<': case '>': case '^':
    case '|': case ';': case ',': case '=': case '+': case '-':
      return true;
    default:
      return false;
  }
}
// glcpp's OTHER: whatever no other rule has a use for.
constexpr bool is_other(char c) { return !is_identifier_char(c) && !is_punctuation(c) && !is_space(c) && c != '#'; }
// The characters of glcpp's PATH and of the path in #include.
constexpr bool is_path_char(char c) {
  if (is_identifier_char(c)) return true;
  switch (c) {
    case ']': case '^': case '.': case '/': case ' ': case '+': case '*': case '%': case '[': case '(': case ')': case '{': case '}': case '|': case '&': case '~':
    case '=': case '!': case ':': case ';': case ',': case '?': case '-':
      return true;
    default:
      return false;
  }
}

// What bison calls a token in a message.
StringView token_name(int type) {
  switch (type) {
    case kEnd: return "end of file";
    case kDefined: return "DEFINED";
    case kElifExpanded: return "ELIF_EXPANDED";
    case kHash: return "HASH_TOKEN";
    case kDefineToken: return "DEFINE_TOKEN";
    case kFuncIdentifier: return "FUNC_IDENTIFIER";
    case kObjIdentifier: return "OBJ_IDENTIFIER";
    case kElif: return "ELIF";
    case kElse: return "ELSE";
    case kEndif: return "ENDIF";
    case kErrorToken: return "ERROR_TOKEN";
    case kIf: return "IF";
    case kIfdef: return "IFDEF";
    case kIfndef: return "IFNDEF";
    case kLine: return "LINE";
    case kPragma: return "PRAGMA";
    case kUndef: return "UNDEF";
    case kVersionToken: return "VERSION_TOKEN";
    case kGarbage: return "GARBAGE";
    case kIdentifier: return "IDENTIFIER";
    case kIfExpanded: return "IF_EXPANDED";
    case kInteger: return "INTEGER";
    case kIntegerString: return "INTEGER_STRING";
    case kLineExpanded: return "LINE_EXPANDED";
    case kNewline: return "NEWLINE";
    case kOther: return "OTHER";
    case kPlaceholder: return "PLACEHOLDER";
    case kSpace: return "SPACE";
    case kPlusPlus: return "PLUS_PLUS";
    case kMinusMinus: return "MINUS_MINUS";
    case kPath: return "PATH";
    case kInclude: return "INCLUDE";
    case kPaste: return "PASTE";
    case kOr: return "OR";
    case kAnd: return "AND";
    case kEqual: return "EQUAL";
    case kNotEqual: return "NOT_EQUAL";
    case kLessOrEqual: return "LESS_OR_EQUAL";
    case kGreaterOrEqual: return "GREATER_OR_EQUAL";
    case kLeftShift: return "LEFT_SHIFT";
    case kRightShift: return "RIGHT_SHIFT";
    default: return "?";
  }
}

// Whether a token can be part of pp_tokens.
constexpr bool is_pp_token(int type) {
  switch (type) {
    case kIdentifier: case kIntegerString: case kPath: case kDefined: case kOther: case kSpace: case '[': case ']': case '(': case ')': case '{': case '}': case '.':
    case '&': case '*': case '+': case '-': case '~': case '!': case '/': case '%': case kLeftShift: case kRightShift: case '<': case '>': case kLessOrEqual:
    case kGreaterOrEqual: case kEqual: case kNotEqual: case '^': case '|': case kAnd: case kOr: case ';': case ',': case '=': case kPaste: case kPlusPlus: case kMinusMinus:
      return true;
    default:
      return false;
  }
}

// strtoll for what the lexer calls an integer. The base is 8 or 16 by prefix when `base` is 0. Stops at the first character that is no digit; too large a number gives INT64_MAX.
int64_t parse_integer(StringView text, int base) {
  uint64_t i = 0;
  if (base == 0) {
    base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && is_hex_digit(text[2])) {
      base = 16;
      i = 2;
    } else if (text.size() > 0 && text[0] == '0') {
      base = 8;
    }
  }
  uint64_t value = 0;
  bool overflow = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    int digit;
    if (is_digit(c)) digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else break;
    if (digit >= base) break;
    const uint64_t limit = static_cast<uint64_t>(INT64_MAX);
    if (value > (limit - static_cast<uint64_t>(digit)) / static_cast<uint64_t>(base)) overflow = true;
    else value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
  }
  return overflow ? INT64_MAX : static_cast<int64_t>(value);
}

constexpr int binary_precedence(int type) {
  switch (type) {
    case kOr: return 1;
    case kAnd: return 2;
    case '|': return 3;
    case '^': return 4;
    case '&': return 5;
    case kEqual: case kNotEqual: return 6;
    case '<': case '>': case kLessOrEqual: case kGreaterOrEqual: return 7;
    case kLeftShift: case kRightShift: return 8;
    case '+': case '-': return 9;
    case '*': case '/': case '%': return 10;
    default: return 0;
  }
}

class Preprocessor {
 public:
  Preprocessor(allocators::IAllocator& allocator, const PreprocessOptions& options, PreprocessResult* result)
      : options_(options), result_(*result), nodes_(allocator, kMaxNodes), tokens_(allocator, kMaxTokens), macros_(allocator, kMaxTokens),
        active_(allocator, kMaxNodes), arguments_(allocator, kMaxNodes), buckets_(allocator), skip_stack_(allocator), pasted_(allocator), continued_(allocator),
        expression_values_(allocator), expression_operators_(allocator) {}

  bool run(StringView source);

 private:
  // --- Lexer (glcpp-lex.l)
  char at(uint64_t index) const { return index < text_.size() ? text_[index] : '\0'; }
  bool looking_at(StringView word) const { return text_.without_prefix(pos_).starts_with(word); }
  uint64_t hspace_run(uint64_t from) const {
    uint64_t end = from;
    while (is_hspace(at(end))) ++end;
    return end - from;
  }
  uint64_t rest_of_line(uint64_t from) const {
    uint64_t end = from;
    while (end < text_.size() && !is_newline_char(text_[end])) ++end;
    return end - from;
  }
  uint64_t newline_length(uint64_t index) const {
    const char c = at(index), d = at(index + 1);
    if ((c == '\r' && d == '\n') || (c == '\n' && d == '\r')) return 2;
    return is_newline_char(c) ? 1 : 0;
  }
  void matched(uint64_t length);
  bool update_state_per_token(int token);
  int lex_token();
  int lex_initial();
  int lex_hash();
  int lex_define();
  int lex_comment();
  int lex_common_newline(uint64_t length);
  int lex_end_of_input();
  bool lex_comment_start();
  // The lexer's RETURN_TOKEN and friends: -1 means no token was produced and the scan goes on.
  int return_never_skip(int token) { return update_state_per_token(token) ? token : -1; }
  int return_token(int token) { return skipping_ ? -1 : return_never_skip(token); }
  int return_string(int token, uint64_t start, uint64_t length) {
    if (skipping_) return -1;
    yy_text_ = text_.substr(start, length);
    return return_never_skip(token);
  }

  // --- Parser (the grammar of glcpp-parse.y)
  int parser_lex();
  int peek() {
    if (!has_lookahead_) {
      lookahead_ = parser_lex();
      has_lookahead_ = true;
    }
    return lookahead_;
  }
  void advance() { has_lookahead_ = false; }
  bool expect(int token, StringView name);
  void syntax_error(StringView expected = StringView());
  void parse_input();
  void parse_line();
  void parse_control_line();
  void parse_define();
  void parse_expanded_condition(int head);
  void parse_expanded_line();
  bool parse_expression(ExpressionValue* out);
  void reduce_binary();
  void reduce_top_operator();
  void read_pp_token(TokenList* list);
  void read_pp_tokens(TokenList* list);

  // --- Token lists
  int type_of_node(uint32_t node) const { return tokens_[nodes_[node].token].type; }
  uint32_t next_of(uint32_t node) const { return nodes_[node].next; }
  void set_next(uint32_t node, uint32_t next) {
    if (node) nodes_[node].next = next;
  }
  StringView text_of_node(uint32_t node) const { return tokens_[nodes_[node].token].text; }
  uint32_t new_token(int type);
  uint32_t new_integer_token(int64_t value);
  uint32_t new_string_token(int type, StringView text);
  uint32_t new_node(uint32_t token);
  static TokenList new_list() {
    TokenList list;
    list.present = true;
    return list;
  }
  void append_token(TokenList* list, uint32_t token);
  void append_list(TokenList* list, const TokenList& tail);
  TokenList copy_list(const TokenList& other);
  void trim_trailing_space(TokenList* list) {
    if (list->non_space_tail) {
      set_next(list->non_space_tail, 0);
      list->tail = list->non_space_tail;
    }
  }
  bool list_is_empty_ignoring_space(const TokenList& list) const;
  bool lists_equal_ignoring_space(const TokenList& a, const TokenList& b) const;
  bool strings_equal(const TokenList& a, const TokenList& b) const;
  TokenList list_with_one(uint32_t token) {
    TokenList list = new_list();
    append_token(&list, token);
    return list;
  }
  TokenList list_with_one_space() { return list_with_one(new_token(kSpace)); }

  // --- Macros
  uint32_t find_macro(StringView name) const;
  void store_macro(StringView name, bool is_function, const TokenList& parameters, const TokenList& replacements);
  void remove_macro(StringView name);
  void add_builtin_define(StringView name, int32_t value);
  void check_reserved_macro_name(const Location& location, StringView name);
  bool macro_equals_existing(uint32_t macro, bool is_function, const TokenList& parameters, const TokenList& replacements) const;
  void define_macro(const Location& location, StringView name, bool is_function, const TokenList& parameters, const TokenList& replacements);
  void handle_undef(const Location& location, StringView name);
  void handle_version(int64_t version, StringView identifier, bool has_identifier, bool explicitly_set);
  void resolve_implicit_version();

  // --- Expansion
  int evaluate_defined(uint32_t node, uint32_t* last);
  void evaluate_defined_in_list(TokenList* list);
  void expand_and_lex_from(int head, TokenList* list, ExpandMode mode);
  void lex_from(const TokenList& list);
  uint32_t paste_tokens(uint32_t token, uint32_t other);
  void apply_pastes(TokenList* list);
  enum class ArgumentStatus { kSuccess, kNotAFunction, kUnbalanced };
  ArgumentStatus parse_arguments(uint32_t node, uint32_t* last);
  bool expand_function(uint32_t node, uint32_t* last, ExpandMode mode, uint32_t macro, TokenList* expansion);
  bool expand_node(uint32_t node, uint32_t* last, ExpandMode mode, int32_t line, TokenList* expansion, uint32_t* macro);
  void expand_list(TokenList* list, ExpandMode mode);
  void pop_active() {
    const uint32_t top = active_.used() - 1;
    const uint32_t macro = active_[top].macro;
    if (macro) --macros_[macro].active;
    active_.release_to(top);
  }
  bool active_top_is(uint32_t marker) const { return active_.used() > 1 && active_[active_.used() - 1].marker == marker; }
  void print_expanded_list(TokenList* list);

  // --- Skip stack
  bool skip_top_is(SkipType type) const { return skip_stack_.count() != 0 && skip_stack_[skip_stack_.count() - 1].type == type; }
  void skip_push_if(const Location& location, bool condition);
  void skip_change_if(const Location& location, StringView type, bool condition);
  void skip_pop(const Location& location);
  bool not_skipping() const { return skip_stack_.count() == 0 || skip_top_is(SkipType::kNoSkip); }

  // --- Output and messages
  void emit(StringView text);
  void emit_char(char c) { emit(StringView(&c, 1)); }
  void emit_token(uint32_t token);
  void log_text(StringView text);
  void log_number(uint32_t number);
  void log_token(uint32_t token);
  void log_message_start(const Location& location, StringView kind);
  void error(const Location& location, StringView message, StringView argument = StringView(), StringView tail = StringView());
  void warning(const Location& location, StringView message);
  void fail_limit();
  // One step of macro-expansion work.
  void tick() {
    if (++work_ > work_limit_ && !aborted_) fail_limit();
  }

  const PreprocessOptions& options_;
  PreprocessResult& result_;
  Arena<Node> nodes_;
  Arena<Token> tokens_;
  Arena<Macro> macros_;
  Arena<ActiveMacro> active_;
  Arena<TokenList> arguments_;
  List<uint32_t> buckets_;
  List<SkipNode> skip_stack_;
  List<String> pasted_;
  String continued_;
  List<ExpressionValue> expression_values_;
  List<int16_t> expression_operators_;

  StringView text_;
  uint64_t pos_ = 0;
  LexState state_ = LexState::kInitial;
  LexState saved_state_ = LexState::kInitial;
  Location location_;        // bison's yylloc
  int32_t lineno_ = 1;       // flex's yylineno
  int32_t column_ = 0;       // flex's yycolumn
  StringView yy_text_;
  int64_t yy_value_ = 0;
  uint32_t commented_newlines_ = 0;
  bool skipping_ = false;
  bool lexing_directive_ = false;
  bool lexing_version_directive_ = false;
  bool space_tokens_ = true;
  bool last_token_was_newline_ = false;
  bool last_token_was_space_ = false;
  bool first_non_space_token_this_line_ = true;
  bool newline_as_space_ = false;
  bool in_control_line_ = false;
  bool in_define_ = false;
  int64_t paren_count_ = 0;
  bool has_new_line_number_ = false;
  int32_t new_line_number_ = 1;
  bool has_new_source_number_ = false;
  uint32_t new_source_number_ = 0;

  bool has_lookahead_ = false;
  int lookahead_ = 0;
  bool lex_from_active_ = false;
  uint32_t lex_from_node_ = 0;

  int64_t version_ = 0;
  bool version_set_ = false;
  bool is_gles_ = false;

  uint32_t expand_depth_ = 0;
  uint64_t work_ = 0;
  uint64_t work_limit_ = kBaseWork;
  uint64_t pasted_bytes_ = 0;
  uint32_t node_mark_ = 1;
  uint32_t token_mark_ = 1;
  bool keep_arena_ = false;
  bool aborted_ = false;   // a syntax error or a limit ended the run
  bool has_error_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Output and messages

void Preprocessor::emit(StringView text) {
  if (aborted_) return;
  if (text.size() > kMaxOutput - result_.output.size()) {
    fail_limit();
    return;
  }
  if (!result_.output.append(text)) {
    has_error_ = true;
    aborted_ = true;
  }
}

void Preprocessor::emit_token(uint32_t token) {
  const Token& t = tokens_[token];
  switch (t.type) {
    case kEnd: case kPlaceholder: break;
    case kInteger: {
      if (aborted_) break;
      if (result_.output.size() >= kMaxOutput) { fail_limit(); break; }
      if (!result_.output.append_int(t.value)) { has_error_ = true; aborted_ = true; }
      break;
    }
    case kIdentifier: case kIntegerString: case kPath: case kOther: emit(t.text); break;
    case kSpace: emit_char(' '); break;
    case kLeftShift: emit("<<"); break;
    case kRightShift: emit(">>"); break;
    case kLessOrEqual: emit("<="); break;
    case kGreaterOrEqual: emit(">="); break;
    case kEqual: emit("=="); break;
    case kNotEqual: emit("!="); break;
    case kAnd: emit("&&"); break;
    case kOr: emit("||"); break;
    case kPaste: emit("##"); break;
    case kPlusPlus: emit("++"); break;
    case kMinusMinus: emit("--"); break;
    case kDefined: emit("defined"); break;
    default:
      if (t.type < 256) emit_char(static_cast<char>(t.type));
      break;
  }
}

void Preprocessor::log_text(StringView text) {
  if (text.size() > kMaxLog - result_.log.size() || !result_.log.append(text)) return;
}

void Preprocessor::log_number(uint32_t number) {
  if (result_.log.size() >= kMaxLog) return;
  (void)result_.log.append_uint(number);
}

// The token as _token_print writes it, for the message about a bad paste.
void Preprocessor::log_token(uint32_t token) {
  const Token& t = tokens_[token];
  switch (t.type) {
    case kEnd: case kPlaceholder: break;
    case kInteger:
      if (result_.log.size() < kMaxLog) (void)result_.log.append_int(t.value);
      break;
    case kIdentifier: case kIntegerString: case kPath: case kOther: log_text(t.text); break;
    case kSpace: log_text(" "); break;
    case kLeftShift: log_text("<<"); break;
    case kRightShift: log_text(">>"); break;
    case kLessOrEqual: log_text("<="); break;
    case kGreaterOrEqual: log_text(">="); break;
    case kEqual: log_text("=="); break;
    case kNotEqual: log_text("!="); break;
    case kAnd: log_text("&&"); break;
    case kOr: log_text("||"); break;
    case kPaste: log_text("##"); break;
    case kPlusPlus: log_text("++"); break;
    case kMinusMinus: log_text("--"); break;
    case kDefined: log_text("defined"); break;
    default:
      if (t.type < 256) {
        const char c = static_cast<char>(t.type);
        log_text(StringView(&c, 1));
      }
      break;
  }
}

void Preprocessor::log_message_start(const Location& location, StringView kind) {
  log_number(location.source);
  log_text(":");
  log_number(static_cast<uint32_t>(location.line));
  log_text("(");
  log_number(static_cast<uint32_t>(location.column));
  log_text("): preprocessor ");
  log_text(kind);
  log_text(": ");
}

// glcpp_error: `message`, optionally followed by `argument` and `tail` (the common shape of its format strings), and a newline.
void Preprocessor::error(const Location& location, StringView message, StringView argument, StringView tail) {
  has_error_ = true;
  log_message_start(location, "error");
  log_text(message);
  log_text(argument);
  log_text(tail);
  log_text("\n");
}

void Preprocessor::warning(const Location& location, StringView message) {
  log_message_start(location, "warning");
  log_text(message);
  log_text("\n");
}

// Not glcpp's: the source asked for more work or memory than it may have (or the allocator ran out).
void Preprocessor::fail_limit() {
  if (aborted_) return;
  error(location_, "macro expansion exceeds the limits of the preprocessor");
  aborted_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Lexer

// flex's YY_USER_ACTION, which runs for every match of a rule, and the advance over it.
void Preprocessor::matched(uint64_t length) {
  if (has_new_line_number_) lineno_ = new_line_number_;
  if (has_new_source_number_) location_.source = new_source_number_;
  location_.column = static_cast<int32_t>(static_cast<uint32_t>(column_) + 1);
  location_.line = lineno_;
  column_ = static_cast<int32_t>(static_cast<uint32_t>(column_) + static_cast<uint32_t>(length));
  has_new_line_number_ = false;
  has_new_source_number_ = false;
  pos_ += length;
}

// False if no token should be returned at all, which suppresses runs of spaces.
bool Preprocessor::update_state_per_token(int token) {
  if (token != kNewline && token != kSpace && token != kHash && !lexing_version_directive_) resolve_implicit_version();
  if (token == kNewline) first_non_space_token_this_line_ = true;
  else if (token != kSpace) first_non_space_token_this_line_ = false;
  last_token_was_newline_ = token == kNewline;
  if (token == kSpace) {
    const bool first = !last_token_was_space_;
    last_token_was_space_ = true;
    return first;
  }
  last_token_was_space_ = false;
  return true;
}

// The comment rules, valid in the initial, #define and # states. True if one matched.
bool Preprocessor::lex_comment_start() {
  if (at(pos_) != '/') return false;
  if (at(pos_ + 1) == '/') {
    matched(rest_of_line(pos_));
    return true;
  }
  if (at(pos_ + 1) == '*') {
    matched(2);
    saved_state_ = state_;
    state_ = LexState::kComment;
    return true;
  }
  return false;
}

// <*>{NEWLINE}
int Preprocessor::lex_common_newline(uint64_t length) {
  matched(length);
  state_ = commented_newlines_ ? LexState::kNewlineCatchup : LexState::kInitial;
  space_tokens_ = true;
  lexing_directive_ = false;
  lexing_version_directive_ = false;
  lineno_ = static_cast<int32_t>(static_cast<uint32_t>(lineno_) + 1);
  column_ = 0;
  return return_never_skip(kNewline);
}

// <<EOF>>. This is no match of a rule, so the location stays where it is.
int Preprocessor::lex_end_of_input() {
  if (state_ == LexState::kComment) error(location_, "Unterminated comment");
  state_ = LexState::kDone;
  lexing_directive_ = false;
  lexing_version_directive_ = false;
  if (!last_token_was_newline_) return return_token(kNewline);
  return -1;
}

int Preprocessor::lex_comment() {
  uint64_t length = 0;
  if (at(pos_) == '*') {
    while (at(pos_ + length) == '*') ++length;
    if (at(pos_ + length) == '/') {
      matched(length + 1);
      state_ = saved_state_;
      if (space_tokens_ && state_ != LexState::kHash) return return_token(kSpace);
      return -1;
    }
    for (char c = at(pos_ + length); pos_ + length < text_.size() && c != '*' && c != '/' && !is_newline_char(c); c = at(pos_ + length)) ++length;
  } else {
    for (char c = at(pos_); pos_ + length < text_.size() && c != '*' && !is_newline_char(c); c = at(pos_ + length)) ++length;
  }
  const uint64_t newline = newline_length(pos_ + length);
  matched(length + newline);
  if (newline) {
    lineno_ = static_cast<int32_t>(static_cast<uint32_t>(lineno_) + 1);
    column_ = 0;
    ++commented_newlines_;
  }
  return -1;
}

int Preprocessor::lex_hash() {
  if (lex_comment_start()) return -1;
  const uint64_t start = pos_;
  const char c = at(pos_);
  // A directive's keyword always beats the catch-all rule at the end, being longer.
  if (looking_at("version")) {
    const uint64_t run = hspace_run(pos_ + 7);
    if (run) {
      matched(7 + run);
      state_ = LexState::kInitial;
      space_tokens_ = false;
      lexing_version_directive_ = true;
      return return_string(kVersionToken, start, 7 + run);
    }
  }
  if (looking_at("pragma")) {
    // An empty #pragma is swallowed, so as not to confuse the compiler downstream.
    const uint64_t run = hspace_run(pos_ + 6);
    if (is_newline_char(at(pos_ + 6 + run))) {
      matched(6 + run);
      state_ = LexState::kInitial;
      return -1;
    }
  }
  if (looking_at("pragma") || looking_at("extension")) {
    const uint64_t length = rest_of_line(pos_);
    matched(length);
    state_ = LexState::kInitial;
    return return_string(kPragma, start, length);
  }
  if (looking_at("include")) {
    const uint64_t run = hspace_run(pos_ + 7);
    if (run) {
      const uint64_t path = pos_ + 7 + run;
      if (at(path) == '"' || at(path) == '<') {
        uint64_t end = path + 1;
        while (is_path_char(at(end))) ++end;
        if (end > path + 1 && (at(end) == '"' || at(end) == '>')) {
          const uint64_t length = end + 1 - pos_;
          matched(length);
          state_ = LexState::kInitial;
          return return_string(kInclude, start, length);
        }
      }
    }
  }
  if (looking_at("line")) {
    const uint64_t run = hspace_run(pos_ + 4);
    if (run) {
      matched(4 + run);
      state_ = LexState::kInitial;
      return return_token(kLine);
    }
  }
  const uint64_t newline = newline_length(pos_);
  if (newline) {
    matched(newline);
    state_ = LexState::kInitial;
    space_tokens_ = false;
    lineno_ = static_cast<int32_t>(static_cast<uint32_t>(lineno_) + 1);
    column_ = 0;
    return return_never_skip(kNewline);
  }
  // The directives that matter even when skipping.
  const auto conditional = [&](uint64_t length, int token, bool expression) -> int {
    matched(length);
    if (in_define_) return -1;
    state_ = LexState::kInitial;
    if (expression) lexing_directive_ = true;
    space_tokens_ = false;
    return return_never_skip(token);
  };
  if (looking_at("ifdef")) return conditional(5, kIfdef, true);
  if (looking_at("ifndef")) return conditional(6, kIfndef, true);
  if (looking_at("if") && pos_ + 2 < text_.size() && !is_identifier_char(at(pos_ + 2))) return conditional(2, kIf, true);
  if (looking_at("elif") && pos_ + 4 < text_.size() && !is_identifier_char(at(pos_ + 4))) return conditional(4, kElif, true);
  if (looking_at("else")) return conditional(4, kElse, false);
  if (looking_at("endif")) return conditional(5, kEndif, false);
  if (looking_at("error")) {
    const uint64_t length = rest_of_line(pos_);
    matched(length);
    state_ = LexState::kInitial;
    return return_string(kErrorToken, start, length);
  }
  if (looking_at("define")) {
    matched(6 + hspace_run(pos_ + 6));
    in_define_ = true;
    if (!skipping_) {
      state_ = LexState::kDefine;
      space_tokens_ = false;
      return return_token(kDefineToken);
    }
    return -1;
  }
  if (looking_at("undef")) {
    matched(5);
    state_ = LexState::kInitial;
    space_tokens_ = false;
    return return_token(kUndef);
  }
  if (is_hspace(c)) {
    matched(hspace_run(pos_));
    return -1;
  }
  matched(1);
  if (!skipping_) {
    state_ = LexState::kInitial;
    return return_token(kGarbage);
  }
  return -1;
}

int Preprocessor::lex_define() {
  if (lex_comment_start()) return -1;
  const uint64_t start = pos_;
  const char c = at(pos_);
  if (is_identifier_start(c)) {
    uint64_t length = 1;
    while (is_identifier_char(at(pos_ + length))) ++length;
    const int token = at(pos_ + length) == '(' ? kFuncIdentifier : kObjIdentifier;
    matched(length);
    state_ = LexState::kInitial;
    return return_string(token, start, length);
  }
  if (is_hspace(c)) {
    matched(hspace_run(pos_));
    return -1;
  }
  const uint64_t newline = newline_length(pos_);
  if (newline) return lex_common_newline(newline);
  if (c == '/' && pos_ + 1 >= text_.size()) {
    // No rule wants a final slash but the one that should never be reached.
    matched(1);
    error(location_, "Internal compiler error: Unexpected character: ", "/");
    return -1;
  }
  // Something that cannot start an identifier: it runs to the next space. (After a slash the rule takes any one character first, a newline included.)
  uint64_t length = c == '/' ? 2 : 1;
  while (pos_ + length < text_.size() && !is_space(at(pos_ + length))) ++length;
  matched(length);
  state_ = LexState::kInitial;
  const StringView text = text_.substr(start, length);
  error(location_, "#define followed by a non-identifier: ", text);
  return return_string(kIntegerString, start, length);
}

int Preprocessor::lex_initial() {
  if (lex_comment_start()) return -1;
  const uint64_t start = pos_;
  const char c = at(pos_), d = at(pos_ + 1);
  if (c == '#') {
    if (d == '#') {
      matched(2);
      if (!skipping_) {
        if (is_gles_) error(location_, "Token pasting (##) is illegal in GLES");
        return return_token(kPaste);
      }
      return -1;
    }
    matched(1);
    if (first_non_space_token_this_line_) {
      state_ = LexState::kHash;
      in_define_ = false;
    }
    return return_never_skip(kHash);
  }
  if (is_digit(c) || (c == '.' && is_digit(d))) {
    // A preprocessing number, which an integer is when one of the integer rules matches just as much of it.
    uint64_t length = 1;
    for (;;) {
      const char e = at(pos_ + length);
      if ((e == 'e' || e == 'E' || e == 'p' || e == 'P') && (at(pos_ + length + 1) == '-' || at(pos_ + length + 1) == '+')) length += 2;
      else if (e == '.' || is_identifier_char(e)) ++length;
      else break;
    }
    uint64_t integer = 0;
    if (c >= '1' && c <= '9') {
      integer = 1;
      while (is_digit(at(pos_ + integer))) ++integer;
    } else if (c == '0' && (d == 'x' || d == 'X') && is_hex_digit(at(pos_ + 2))) {
      integer = 2;
      while (is_hex_digit(at(pos_ + integer))) ++integer;
    } else if (c == '0') {
      integer = 1;
      while (at(pos_ + integer) >= '0' && at(pos_ + integer) <= '7') ++integer;
    }
    if (integer && (at(pos_ + integer) == 'u' || at(pos_ + integer) == 'U')) ++integer;
    matched(length);
    return return_string(integer == length ? kIntegerString : kOther, start, length);
  }
  if (is_identifier_start(c)) {
    uint64_t length = 1;
    while (is_identifier_char(at(pos_ + length))) ++length;
    matched(length);
    const StringView word = text_.substr(start, length);
    if (word == "defined") return return_token(kDefined);
    return return_string(kIdentifier, start, length);
  }
  int two_char = 0;
  if (c == '<' && d == '<') two_char = kLeftShift;
  else if (c == '>' && d == '>') two_char = kRightShift;
  else if (c == '<' && d == '=') two_char = kLessOrEqual;
  else if (c == '>' && d == '=') two_char = kGreaterOrEqual;
  else if (c == '=' && d == '=') two_char = kEqual;
  else if (c == '!' && d == '=') two_char = kNotEqual;
  else if (c == '&' && d == '&') two_char = kAnd;
  else if (c == '|' && d == '|') two_char = kOr;
  else if (c == '+' && d == '+') two_char = kPlusPlus;
  else if (c == '-' && d == '-') two_char = kMinusMinus;
  if (two_char) {
    matched(2);
    return return_token(two_char);
  }
  if (is_punctuation(c)) {
    matched(1);
    return return_token(c);
  }
  if (is_hspace(c)) {
    matched(1);
    return space_tokens_ ? return_token(kSpace) : -1;
  }
  const uint64_t newline = newline_length(pos_);
  if (newline) return lex_common_newline(newline);
  // OTHER, unless a path is longer (and when it is no longer, OTHER's rule comes first).
  uint64_t other = 0;
  while (pos_ + other < text_.size() && is_other(at(pos_ + other))) ++other;
  uint64_t path = 0;
  if (c == '"') {
    uint64_t end = pos_ + 1;
    while (is_path_char(at(end))) ++end;
    if (at(end) == '"') path = end + 1 - pos_;
  }
  if (path > other) {
    matched(path);
    return return_string(kPath, start, path);
  }
  matched(other);
  return return_string(kOther, start, other);
}

// One token from the lexer; 0 at the end. The code before the loop is what flex's rules section has ahead of its first rule: it runs once a call, not once a match.
int Preprocessor::lex_token() {
  if (state_ == LexState::kNewlineCatchup) {
    if (commented_newlines_) --commented_newlines_;
    if (commented_newlines_ == 0) state_ = LexState::kInitial;
    return return_never_skip(kNewline);
  }
  skipping_ = skip_stack_.count() != 0 && !skip_top_is(SkipType::kNoSkip) && !lexing_directive_;
  for (;;) {
    if (state_ == LexState::kDone) return kEnd;
    int token;
    if (pos_ >= text_.size()) {
      token = lex_end_of_input();
    } else {
      switch (state_) {
        case LexState::kHash: token = lex_hash(); break;
        case LexState::kDefine: token = lex_define(); break;
        case LexState::kComment: token = lex_comment(); break;
        default: token = lex_initial(); break;
      }
    }
    if (token >= 0) return token;
  }
}

// glcpp_parser_lex: the next token, from the list of an expanded directive if there is one. While a function-like macro's name waits for its closing parenthesis, a newline is a space.
int Preprocessor::parser_lex() {
  if (!lex_from_active_) {
    int token = lex_token();
    if (newline_as_space_) {
      if (token == '(') {
        ++paren_count_;
      } else if (token == ')') {
        --paren_count_;
        if (paren_count_ == 0) newline_as_space_ = false;
      } else if (token == kNewline) {
        token = kSpace;
      } else if (token != kSpace) {
        if (paren_count_ == 0) newline_as_space_ = false;
      }
    } else if (in_control_line_) {
      if (token == kNewline) in_control_line_ = false;
    } else if (token == kDefineToken || token == kUndef || token == kIf || token == kIfdef || token == kIfndef || token == kElif || token == kElse || token == kEndif ||
               token == kHash) {
      in_control_line_ = true;
    } else if (token == kIdentifier) {
      const uint32_t macro = find_macro(yy_text_);
      if (macro && macros_[macro].is_function) {
        newline_as_space_ = true;
        paren_count_ = 0;
      }
    }
    return token;
  }
  const uint32_t node = lex_from_node_;
  if (node == 0) {
    lex_from_active_ = false;
    return kNewline;
  }
  const Token& token = tokens_[nodes_[node].token];
  yy_text_ = token.text;
  yy_value_ = token.value;
  lex_from_node_ = next_of(node);
  return token.type;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Parser

// bison's verbose message for a token no action exists for, which ends the run: the grammar has no recovery.
void Preprocessor::syntax_error(StringView expected) {
  has_error_ = true;
  log_message_start(location_, "error");
  log_text("syntax error, unexpected ");
  const int token = peek();
  if (token > 0 && token < 256) {
    const char c = static_cast<char>(token);
    log_text("'");
    log_text(StringView(&c, 1));
    log_text("'");
  } else {
    log_text(token_name(token));
  }
  if (!expected.empty()) {
    log_text(", expecting ");
    log_text(expected);
  }
  log_text("\n");
  aborted_ = true;
}

bool Preprocessor::expect(int token, StringView name) {
  if (peek() != token) {
    syntax_error(name);
    return false;
  }
  advance();
  return true;
}

// preprocessing_token: makes a token of the lookahead and shifts it. The state after every token is consistent, so bison runs pp_tokens' action before lexing any more.
void Preprocessor::read_pp_token(TokenList* list) {
  const int type = peek();
  uint32_t token;
  if (type == kIdentifier || type == kIntegerString || type == kPath || type == kOther) token = new_string_token(type, yy_text_);
  else token = new_token(type);
  advance();
  space_tokens_ = true;
  append_token(list, token);
}

// pp_tokens, as far as they go.
void Preprocessor::read_pp_tokens(TokenList* list) {
  while (!aborted_ && is_pp_token(peek())) read_pp_token(list);
}

void Preprocessor::parse_input() {
  while (!aborted_) {
    if (!lex_from_active_) {
      // Whatever the previous line made and did not define a macro with is free again.
      if (keep_arena_) {
        node_mark_ = nodes_.used();
        token_mark_ = tokens_.used();
        keep_arena_ = false;
      } else {
        nodes_.release_to(node_mark_);
        tokens_.release_to(token_mark_);
      }
    }
    if (peek() == kEnd) return;
    parse_line();
  }
}

void Preprocessor::parse_line() {
  int token = peek();
  switch (token) {
    case kIfExpanded: case kElifExpanded:
      parse_expanded_condition(token);
      return;
    case kLineExpanded:
      parse_expanded_line();
      return;
    case kHash:
      parse_control_line();
      return;
    case kNewline:
      advance();
      emit_char('\n');
      return;
    default:
      break;
  }
  TokenList list = new_list();
  if (token == kSpace) {
    // Whether this is a space in a text line or ahead of a directive takes the next token to know, whose location the space token then ends up with.
    advance();
    token = peek();
    if (token == kHash) {
      parse_control_line();
      return;
    }
    const uint32_t space = new_token(kSpace);
    space_tokens_ = true;
    append_token(&list, space);
  }
  read_pp_tokens(&list);
  if (aborted_) return;
  if (peek() != kNewline) {
    syntax_error();
    return;
  }
  advance();
  print_expanded_list(&list);
  emit_char('\n');
}

void Preprocessor::parse_define() {
  const int token = peek();
  const StringView name = yy_text_;
  const Location name_location = location_;
  if (token == kObjIdentifier) {
    advance();
    TokenList replacements;
    if (is_pp_token(peek())) {
      replacements = new_list();
      read_pp_tokens(&replacements);
    }
    if (aborted_ || !expect(kNewline, "NEWLINE")) return;
    keep_arena_ = true;
    define_macro(name_location, name, false, TokenList(), replacements);
    return;
  }
  // kFuncIdentifier: the lexer saw the parenthesis.
  advance();
  if (!expect('(', "'('")) return;
  TokenList parameters;
  if (peek() == kIdentifier) {
    parameters = new_list();
    for (;;) {
      append_token(&parameters, new_string_token(kIdentifier, yy_text_));
      advance();
      if (aborted_) return;
      if (peek() != ',') break;
      advance();
      if (peek() != kIdentifier) {
        syntax_error("IDENTIFIER");
        return;
      }
    }
    if (peek() != ')') {
      syntax_error("')' or ','");
      return;
    }
  } else if (peek() != ')') {
    syntax_error("IDENTIFIER or ')'");
    return;
  }
  advance();
  TokenList replacements;
  if (is_pp_token(peek())) {
    replacements = new_list();
    read_pp_tokens(&replacements);
  }
  if (aborted_ || !expect(kNewline, "NEWLINE")) return;
  keep_arena_ = true;
  define_macro(name_location, name, true, parameters, replacements);
}

// control_line, with the # as the lookahead.
void Preprocessor::parse_control_line() {
  const Location hash = location_;
  advance();
  const int token = peek();
  switch (token) {
    case kNewline:
      advance();
      resolve_implicit_version();
      break;
    case kDefineToken: {
      advance();
      const int next = peek();
      if (next == kNewline) {
        advance();
        error(hash, "#define without macro name");
        return;  // no newline in the output
      }
      if (next != kObjIdentifier && next != kFuncIdentifier) {
        syntax_error("FUNC_IDENTIFIER or OBJ_IDENTIFIER or NEWLINE");
        return;
      }
      parse_define();
      if (aborted_) return;
      break;
    }
    case kUndef: {
      advance();
      if (peek() != kIdentifier) {
        syntax_error("IDENTIFIER");
        return;
      }
      const StringView name = yy_text_;
      advance();
      if (!expect(kNewline, "NEWLINE")) return;
      handle_undef(hash, name);
      break;
    }
    case kInclude: {
      const StringView path = yy_text_;
      advance();
      if (!expect(kNewline, "NEWLINE")) return;
      error(hash, path, " not found");  // no ARB_shading_language_include here, so no named string exists
      break;
    }
    case kIf: case kElif: {
      advance();
      if (peek() == kNewline) {
        advance();
        if (token == kIf) {
          if (not_skipping()) error(hash, "#if with no expression");
          skip_push_if(hash, false);
        } else if (skip_top_is(SkipType::kToElse)) {
          error(hash, "#elif with no expression");
        } else if (skip_stack_.count() != 0 && skip_stack_[skip_stack_.count() - 1].has_else) {
          error(hash, "#elif after #else");
        } else {
          skip_change_if(hash, "elif", false);
          warning(hash, "ignoring illegal #elif without expression");
        }
        break;
      }
      if (!is_pp_token(peek())) {
        syntax_error();
        return;
      }
      TokenList list = new_list();
      read_pp_tokens(&list);
      if (aborted_) return;
      if (peek() != kNewline) {
        syntax_error();
        return;
      }
      advance();
      if (token == kIf) {
        // Only an #if outside skipped text evaluates anything, so that skipped ones give no diagnostics.
        if (not_skipping()) {
          expand_and_lex_from(kIfExpanded, &list, ExpandMode::kEvaluateDefined);  // the expanded line, which comes next, does the rest
        } else {
          skip_push_if(hash, false);
          skip_stack_[skip_stack_.count() - 1].type = SkipType::kToEndif;
        }
      } else if (skip_top_is(SkipType::kToElse)) {
        expand_and_lex_from(kElifExpanded, &list, ExpandMode::kEvaluateDefined);
      } else if (skip_stack_.count() != 0 && skip_stack_[skip_stack_.count() - 1].has_else) {
        error(hash, "#elif after #else");
      } else {
        skip_change_if(hash, "elif", false);
      }
      break;
    }
    case kIfdef: case kIfndef: {
      advance();
      if (peek() != kIdentifier) {
        syntax_error("IDENTIFIER");
        return;
      }
      const StringView name = yy_text_;
      const Location name_location = location_;
      advance();
      if (is_pp_token(peek())) {
        const Location first = location_;
        TokenList junk = new_list();
        read_pp_tokens(&junk);
        if (aborted_) return;
        error(first, "extra tokens at end of directive");
      }
      if (!expect(kNewline, "NEWLINE")) return;
      const bool defined = find_macro(name) != 0;
      if (token == kIfdef) skip_push_if(hash, defined);
      else skip_push_if(name_location, !defined);
      break;
    }
    case kElse:
      advance();
      lexing_directive_ = true;  // before the next token, so that garbage after #else in skipped text gets seen
      if (!expect(kNewline, "NEWLINE")) return;
      if (skip_stack_.count() != 0 && skip_stack_[skip_stack_.count() - 1].has_else) {
        error(hash, "multiple #else");
      } else {
        skip_change_if(hash, "else", true);
        if (skip_stack_.count() != 0) skip_stack_[skip_stack_.count() - 1].has_else = true;
      }
      break;
    case kEndif:
      advance();
      skip_pop(hash);
      if (!expect(kNewline, "NEWLINE")) return;
      break;
    case kVersionToken: {
      advance();
      if (peek() != kIntegerString) {
        syntax_error("INTEGER_STRING");
        return;
      }
      const StringView digits = yy_text_;
      int64_t version = 0;
      if (digits.size() > 1 && digits[0] == '0') error(location_, "invalid #version \"", digits, "\" (not a decimal constant)");  // octal or hexadecimal
      else version = parse_integer(digits, 10);
      advance();
      StringView identifier;
      bool has_identifier = false;
      if (peek() == kIdentifier) {
        identifier = yy_text_;
        has_identifier = true;
        advance();
        if (!expect(kNewline, "NEWLINE")) return;
      } else if (peek() == kNewline) {
        advance();
      } else {
        syntax_error("IDENTIFIER or NEWLINE");
        return;
      }
      if (version_set_) error(hash, "#version must appear on the first line");
      handle_version(version, identifier, has_identifier, true);
      break;
    }
    case kPragma: {
      const StringView text = yy_text_;
      advance();
      if (!expect(kNewline, "NEWLINE")) return;
      emit_char('#');
      emit(text);
      break;
    }
    case kErrorToken: {
      const StringView text = yy_text_;
      advance();
      if (!expect(kNewline, "NEWLINE")) return;
      error(hash, "#", text);
      return;  // no newline in the output
    }
    case kGarbage: case kLine: {
      advance();
      if (!is_pp_token(peek())) {
        syntax_error();
        return;
      }
      TokenList list = new_list();
      read_pp_tokens(&list);
      if (aborted_) return;
      if (peek() != kNewline) {
        syntax_error();
        return;
      }
      advance();
      if (token == kGarbage) error(hash, "Illegal non-directive after #");
      else if (not_skipping()) expand_and_lex_from(kLineExpanded, &list, ExpandMode::kIgnoreDefined);
      return;  // no newline in the output: the expanded #line prints its own
    }
    default:
      syntax_error();
      return;
  }
  emit_char('\n');
}

// IF_EXPANDED expression NEWLINE, and ELIF_EXPANDED. The value goes through an int in glcpp on its way to the skip stack.
void Preprocessor::parse_expanded_condition(int head) {
  const Location location = location_;
  advance();
  ExpressionValue value;
  if (!parse_expression(&value)) return;
  if (peek() != kNewline) {
    syntax_error();
    return;
  }
  advance();
  if (is_gles_ && !value.undefined_macro.empty()) error(location, "undefined macro ", value.undefined_macro, " in expression (illegal in GLES)");
  const bool condition = static_cast<int32_t>(static_cast<uint64_t>(value.value)) != 0;
  if (head == kIfExpanded) skip_push_if(location, condition);
  else skip_change_if(location, "elif", condition);
}

// LINE_EXPANDED integer_constant [integer_constant | PATH] NEWLINE
void Preprocessor::parse_expanded_line() {
  advance();
  const auto integer_constant = [&]() -> int64_t {
    return lookahead_ == kInteger ? yy_value_ : parse_integer(yy_text_, 0);
  };
  int token = peek();
  if (token != kInteger && token != kIntegerString) {
    syntax_error("INTEGER or INTEGER_STRING");
    return;
  }
  const int64_t line = integer_constant();
  advance();
  token = peek();
  const auto set_line = [&]() {
    has_new_line_number_ = true;
    new_line_number_ = static_cast<int32_t>(static_cast<uint64_t>(line));
    emit("#line ");
    if (!aborted_ && !result_.output.append_int(line)) fail_limit();
  };
  if (token == kNewline) {
    advance();
    set_line();
    emit_char('\n');
  } else if (token == kInteger || token == kIntegerString) {
    const int64_t source = integer_constant();
    advance();
    if (!expect(kNewline, "NEWLINE")) return;
    set_line();
    has_new_source_number_ = true;
    new_source_number_ = static_cast<uint32_t>(static_cast<uint64_t>(source));
    emit_char(' ');
    if (!aborted_ && !result_.output.append_int(source)) fail_limit();
    emit_char('\n');
  } else if (token == kPath) {
    const StringView path = yy_text_;
    advance();
    if (!expect(kNewline, "NEWLINE")) return;
    set_line();
    emit_char(' ');
    emit(path);
    emit_char('\n');
  } else {
    syntax_error("INTEGER or INTEGER_STRING or NEWLINE or PATH");
  }
}

// One reduction of a binary rule. Everything wraps instead of overflowing. A zero divisor is reported and yields the left side, which is what yacc's $$ then holds; the
// right side was evaluated in any case, as the grammar does no short-circuiting.
void Preprocessor::reduce_binary() {
  const uint64_t count = expression_values_.count();
  const int type = expression_operators_[expression_operators_.count() - 1];
  (void)expression_operators_.remove_at(expression_operators_.count() - 1);
  const ExpressionValue right = expression_values_[count - 1];
  ExpressionValue left = expression_values_[count - 2];
  (void)expression_values_.remove_at(count - 1);
  const int64_t a = left.value, b = right.value;
  const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
  int64_t value = a;
  switch (type) {
    case kOr: value = a || b; break;
    case kAnd: value = a && b; break;
    case '|': value = static_cast<int64_t>(ua | ub); break;
    case '^': value = static_cast<int64_t>(ua ^ ub); break;
    case '&': value = static_cast<int64_t>(ua & ub); break;
    case kNotEqual: value = a != b; break;
    case kEqual: value = a == b; break;
    case kGreaterOrEqual: value = a >= b; break;
    case kLessOrEqual: value = a <= b; break;
    case '>': value = a > b; break;
    case '<': value = a < b; break;
    case kRightShift:
      if (b < 0 || b > 63) {
        warning(location_, "shift count out of range in preprocessor directive");
        value = a < 0 ? -1 : 0;
      } else {
        value = a >> b;
      }
      break;
    case kLeftShift:
      if (b < 0 || b > 63) {
        warning(location_, "shift count out of range in preprocessor directive");
        value = 0;
      } else {
        value = static_cast<int64_t>(ua << b);
      }
      break;
    case '-': value = static_cast<int64_t>(ua - ub); break;
    case '+': value = static_cast<int64_t>(ua + ub); break;
    case '*': value = static_cast<int64_t>(ua * ub); break;
    case '%':
      if (b == 0) error(location_, "zero modulus in preprocessor directive");
      else if (b == -1) value = 0;
      else value = a % b;
      break;
    case '/':
      if (b == 0) {
        error(location_, "division by 0 in preprocessor directive");
      } else if (b == -1) {
        if (a == INT64_MIN) warning(location_, "integer overflow in preprocessor directive");
        value = static_cast<int64_t>(0 - ua);
      } else {
        value = a / b;
      }
      break;
    default: break;
  }
  // Only the side that is evaluated counts for "undefined macro" in || and &&.
  if (left.undefined_macro.empty()) {
    if (type == kOr) { if (a == 0) left.undefined_macro = right.undefined_macro; }
    else if (type == kAnd) { if (a != 0) left.undefined_macro = right.undefined_macro; }
    else left.undefined_macro = right.undefined_macro;
  }
  left.value = value;
  expression_values_[count - 2] = left;
}

// Reduces the operator on top of the stack, unary or binary. Unary operators are stacked as their negated character.
void Preprocessor::reduce_top_operator() {
  const int top = expression_operators_[expression_operators_.count() - 1];
  if (top >= 0) {
    reduce_binary();
    return;
  }
  (void)expression_operators_.remove_at(expression_operators_.count() - 1);
  ExpressionValue& operand = expression_values_[expression_values_.count() - 1];
  const uint64_t u = static_cast<uint64_t>(operand.value);
  switch (-top) {
    case '!': operand.value = !operand.value; break;
    case '~': operand.value = static_cast<int64_t>(~u); break;
    case '-': operand.value = static_cast<int64_t>(0 - u); break;
    default: break;
  }
}

// expression: an operator-precedence parser with its stacks on the heap, since a source can nest as deeply as it likes. It reduces in the order the LALR automaton does,
// which shows when a division by zero and a syntax error are on the same line. Leaves the token after the expression as the lookahead. False after a syntax error.
bool Preprocessor::parse_expression(ExpressionValue* out) {
  expression_values_.clear();
  expression_operators_.clear();
  for (;;) {
    // An operand, after any number of prefixes.
    for (;;) {
      if (expression_operators_.count() + expression_values_.count() >= kMaxExpressionDepth) {
        error(location_, "memory exhausted");
        aborted_ = true;
        return false;
      }
      const int token = peek();
      if (token == '!' || token == '~' || token == '-' || token == '+') {
        if (!expression_operators_.push_back(static_cast<int16_t>(-token))) { fail_limit(); return false; }
        advance();
      } else if (token == '(') {
        if (!expression_operators_.push_back(static_cast<int16_t>('('))) { fail_limit(); return false; }
        advance();
      } else {
        break;
      }
    }
    ExpressionValue operand;
    const int token = peek();
    if (token == kIntegerString) {
      operand.value = parse_integer(yy_text_, 0);
    } else if (token == kInteger) {
      operand.value = yy_value_;
    } else if (token == kIdentifier) {
      if (is_gles_) operand.undefined_macro = yy_text_;
    } else {
      syntax_error();
      return false;
    }
    advance();
    if (!expression_values_.push_back(operand)) { fail_limit(); return false; }
    // Operators and closing parentheses, until one is followed by an operand again.
    for (;;) {
      const int next = peek();
      const int precedence = binary_precedence(next);
      if (precedence) {
        // Left associative: everything at least as tightly bound goes first.
        while (expression_operators_.count() != 0) {
          const int top = expression_operators_[expression_operators_.count() - 1];
          if (top == '(' || (top > 0 && binary_precedence(top) < precedence)) break;
          reduce_top_operator();
        }
        if (!expression_operators_.push_back(static_cast<int16_t>(next))) { fail_limit(); return false; }
        advance();
        break;
      }
      // Anything else ends all that is open up to the nearest parenthesis.
      while (expression_operators_.count() != 0 && expression_operators_[expression_operators_.count() - 1] != '(') reduce_top_operator();
      if (expression_operators_.count() == 0) {
        *out = expression_values_[0];
        return true;
      }
      if (next != ')') {
        syntax_error();
        return false;
      }
      (void)expression_operators_.remove_at(expression_operators_.count() - 1);
      advance();
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Token lists

uint32_t Preprocessor::new_token(int type) {
  const uint32_t index = tokens_.allocate();
  if (!index) {
    fail_limit();
    return 0;
  }
  Token& token = tokens_[index];
  token.type = static_cast<int16_t>(type);
  token.location = location_;
  return index;
}

uint32_t Preprocessor::new_integer_token(int64_t value) {
  const uint32_t index = new_token(kInteger);
  if (index) {
    tokens_[index].value = value;
    tokens_[index].location = Location{0, 0, 0};  // glcpp gives these none
  }
  return index;
}

uint32_t Preprocessor::new_string_token(int type, StringView text) {
  const uint32_t index = new_token(type);
  if (index) tokens_[index].text = text;
  return index;
}

uint32_t Preprocessor::new_node(uint32_t token) {
  tick();
  const uint32_t index = nodes_.allocate();
  if (!index) {
    fail_limit();
    return 0;
  }
  nodes_[index].token = token;
  return index;
}

void Preprocessor::append_token(TokenList* list, uint32_t token) {
  const uint32_t node = new_node(token);
  if (!node) return;
  if (list->head == 0) list->head = node;
  else set_next(list->tail, node);
  list->tail = node;
  if (tokens_[token].type != kSpace) list->non_space_tail = node;
}

void Preprocessor::append_list(TokenList* list, const TokenList& tail) {
  if (!tail.present || tail.head == 0) return;
  if (list->head == 0) list->head = tail.head;
  else set_next(list->tail, tail.head);
  list->tail = tail.tail;
  list->non_space_tail = tail.non_space_tail;
}

TokenList Preprocessor::copy_list(const TokenList& other) {
  TokenList copy;
  if (!other.present) return copy;
  copy = new_list();
  // Tokens never change once made, so a copy can share them.
  for (uint32_t node = other.head; node && !aborted_; node = next_of(node)) append_token(&copy, nodes_[node].token);
  return copy;
}

bool Preprocessor::list_is_empty_ignoring_space(const TokenList& list) const {
  if (!list.present) return true;
  uint32_t node = list.head;
  while (node && type_of_node(node) == kSpace) node = next_of(node);
  return node == 0;
}

bool Preprocessor::lists_equal_ignoring_space(const TokenList& a, const TokenList& b) const {
  if (!a.present || !b.present) return list_is_empty_ignoring_space(a) == list_is_empty_ignoring_space(b);
  uint32_t node_a = a.head, node_b = b.head;
  const auto skip_spaces = [this](uint32_t node) {
    while (node && type_of_node(node) == kSpace) node = next_of(node);
    return node;
  };
  for (;;) {
    if (!node_a && !node_b) return true;
    // Trailing whitespace does not count.
    if (!node_a && type_of_node(node_b) == kSpace) node_b = skip_spaces(node_b);
    if (!node_a && !node_b) return true;
    if (!node_b && type_of_node(node_a) == kSpace) node_a = skip_spaces(node_a);
    if (!node_a && !node_b) return true;
    if (!node_a || !node_b) return false;
    // Whitespace must be in the same places, but need not be the same amount.
    if (type_of_node(node_a) == kSpace && type_of_node(node_b) == kSpace) {
      node_a = skip_spaces(node_a);
      node_b = skip_spaces(node_b);
      continue;
    }
    const int type = type_of_node(node_a);
    if (type != type_of_node(node_b)) return false;
    if (type == kInteger) {
      if (tokens_[nodes_[node_a].token].value != tokens_[nodes_[node_b].token].value) return false;
    } else if (type == kIdentifier || type == kIntegerString || type == kOther) {
      if (text_of_node(node_a) != text_of_node(node_b)) return false;
    }
    node_a = next_of(node_a);
    node_b = next_of(node_b);
  }
}

bool Preprocessor::strings_equal(const TokenList& a, const TokenList& b) const {
  uint32_t node_a = a.head, node_b = b.head;
  for (; node_a && node_b; node_a = next_of(node_a), node_b = next_of(node_b))
    if (text_of_node(node_a) != text_of_node(node_b)) return false;
  return node_a == node_b;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Macros

namespace {
uint32_t hash_name(StringView name) {
  uint32_t hash = 2166136261u;
  for (const char c : name) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  return hash % kMacroBuckets;
}
}  // namespace

uint32_t Preprocessor::find_macro(StringView name) const {
  for (uint32_t macro = buckets_[hash_name(name)]; macro; macro = macros_[macro].next)
    if (macros_[macro].name == name) return macro;
  return 0;
}

// Inserts a macro, or replaces what the name had.
void Preprocessor::store_macro(StringView name, bool is_function, const TokenList& parameters, const TokenList& replacements) {
  uint32_t macro = find_macro(name);
  if (!macro) {
    macro = macros_.allocate();
    if (!macro) {
      fail_limit();
      return;
    }
    const uint32_t bucket = hash_name(name);
    macros_[macro].name = name;
    macros_[macro].next = buckets_[bucket];
    buckets_[bucket] = macro;
  }
  Macro& entry = macros_[macro];
  entry.is_function = is_function;
  entry.parameters = parameters;
  entry.replacements = replacements;
}

void Preprocessor::remove_macro(StringView name) {
  const uint32_t bucket = hash_name(name);
  uint32_t previous = 0;
  for (uint32_t macro = buckets_[bucket]; macro; previous = macro, macro = macros_[macro].next) {
    if (macros_[macro].name != name) continue;
    if (previous) macros_[previous].next = macros_[macro].next;
    else buckets_[bucket] = macros_[macro].next;
    return;
  }
}

void Preprocessor::add_builtin_define(StringView name, int32_t value) {
  keep_arena_ = true;
  const uint32_t token = new_integer_token(value);
  if (aborted_) return;
  store_macro(name, false, TokenList(), list_with_one(token));
}

void Preprocessor::check_reserved_macro_name(const Location& location, StringView name) {
  if (name.contains("__")) warning(location, "Macro names containing \"__\" are reserved for use by the implementation.\n");
  if (name.starts_with("GL_")) error(location, "Macro names starting with \"GL_\" are reserved.\n");
  if (name == "defined") error(location, "\"defined\" cannot be used as a macro name");
}

bool Preprocessor::macro_equals_existing(uint32_t macro, bool is_function, const TokenList& parameters, const TokenList& replacements) const {
  const Macro& existing = macros_[macro];
  if (existing.is_function != is_function) return false;
  if (is_function && !strings_equal(existing.parameters, parameters)) return false;
  return lists_equal_ignoring_space(existing.replacements, replacements);
}

void Preprocessor::define_macro(const Location& location, StringView name, bool is_function, const TokenList& parameters, const TokenList& replacements) {
  check_reserved_macro_name(location, name);
  if (is_function) {
    // Duplicate parameters: the first one found twice, a walk over all pairs that counts as work.
    for (uint32_t a = parameters.head; a && !aborted_; a = next_of(a)) {
      bool duplicate = false;
      for (uint32_t b = next_of(a); b && !aborted_; b = next_of(b)) {
        tick();
        if (text_of_node(a) == text_of_node(b)) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        error(location, "Duplicate macro parameter \"", text_of_node(a), "\"");
        break;
      }
    }
  }
  if (aborted_) return;
  const uint32_t previous = find_macro(name);
  if (previous) {
    if (macro_equals_existing(previous, is_function, parameters, replacements)) return;
    error(location, "Redefinition of macro ", name, "\n");
  }
  store_macro(name, is_function, parameters, replacements);
}

void Preprocessor::handle_undef(const Location& location, StringView name) {
  // Section 3.4 of the GLSL ES 3.00 spec makes undefining a built-in an error; dEQP expects the same of ES 1.00. Later desktop specs only reserve names with __.
  if (name.starts_with("GL_")) {
    error(location, "Built-in (pre-defined) names beginning with GL_ cannot be undefined.");
  } else if (name.contains("__")) {
    if (is_gles_ && version_ >= 300 && (name == "__LINE__" || name == "__FILE__" || name == "__VERSION__")) error(location, "Built-in (pre-defined) names cannot be undefined.");
    else if (is_gles_ && version_ <= 300) error(location, " names containing consecutive underscores are reserved.");
    else warning(location, " names containing consecutive underscores are reserved.");
  }
  remove_macro(name);
}

void Preprocessor::handle_version(int64_t version, StringView identifier, bool has_identifier, bool explicitly_set) {
  if (version_set_) return;
  version_ = version;
  version_set_ = true;
  const int32_t narrow = static_cast<int32_t>(static_cast<uint64_t>(version));
  add_builtin_define("__VERSION__", narrow);
  is_gles_ = version == 100 || (has_identifier && identifier == "es") || (!explicitly_set && options_.default_es);
  const bool is_compatibility = version >= 150 && has_identifier && identifier == "compatibility";
  if (is_gles_) {
    add_builtin_define("GL_ES", 1);
    add_builtin_define("GL_es_profile", 1);
  } else if (is_compatibility) {
    add_builtin_define("GL_compatibility_profile", 1);
  } else if (version >= 150) {
    add_builtin_define("GL_core_profile", 1);
  }
  // All ES implementations here support highp in the fragment shader.
  if (version >= 130 || is_gles_) add_builtin_define("GL_FRAGMENT_PRECISION_HIGH", 1);
  for (const StringView& name : options_.extension_macros) add_builtin_define(name, 1);

  result_.version = static_cast<uint32_t>(narrow);
  result_.version_declared = explicitly_set;
  result_.es = is_gles_;
  result_.profile = 0;
  if (has_identifier && identifier == "core") result_.profile = 1;
  else if (has_identifier && identifier == "compatibility") result_.profile = 2;

  if (explicitly_set) {
    emit("#version ");
    if (!aborted_ && !result_.output.append_int(version)) fail_limit();
    if (has_identifier) {
      emit_char(' ');
      emit(identifier);
    }
  }
}

void Preprocessor::resolve_implicit_version() {
  if (!version_set_) handle_version(options_.default_version, StringView(), false, false);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Expansion

// A `defined` token: 1 or 0 if an identifier follows, optionally in parentheses, and *last is the last node used. Otherwise an error and -1.
int Preprocessor::evaluate_defined(uint32_t node, uint32_t* last) {
  const uint32_t defined = node;
  const auto fail = [&]() {
    error(tokens_[nodes_[defined].token].location, "\"defined\" not followed by an identifier");
    return -1;
  };
  node = next_of(node);
  while (node && type_of_node(node) == kSpace) node = next_of(node);
  if (!node) return fail();
  uint32_t argument;
  if (type_of_node(node) == kIdentifier || type_of_node(node) == kOther) {
    argument = node;
  } else if (type_of_node(node) == '(') {
    node = next_of(node);
    while (node && type_of_node(node) == kSpace) node = next_of(node);
    if (!node || (type_of_node(node) != kIdentifier && type_of_node(node) != kOther)) return fail();
    argument = node;
    node = next_of(node);
    while (node && type_of_node(node) == kSpace) node = next_of(node);
    if (!node || type_of_node(node) != ')') return fail();
  } else {
    return fail();
  }
  *last = node;
  return find_macro(text_of_node(argument)) ? 1 : 0;
}

void Preprocessor::evaluate_defined_in_list(TokenList* list) {
  if (!list->present) return;
  uint32_t previous = 0, node = list->head;
  while (node && !aborted_) {
    tick();
    if (type_of_node(node) == kDefined) {
      uint32_t last = 0;
      const int value = evaluate_defined(node, &last);
      if (value != -1) {
        const uint32_t token = new_integer_token(value);
        const uint32_t replacement = new_node(token);
        if (!replacement) return;
        if (previous) set_next(previous, replacement);
        else list->head = replacement;
        set_next(replacement, next_of(last));
        if (last == list->tail) list->tail = replacement;
        node = replacement;
      }
    }
    previous = node;
    node = next_of(node);
  }
}

void Preprocessor::lex_from(const TokenList& list) {
  // The parser gets no spaces from here.
  TokenList filtered = new_list();
  for (uint32_t node = list.head; node && !aborted_; node = next_of(node))
    if (type_of_node(node) != kSpace) append_token(&filtered, nodes_[node].token);
  lex_from_node_ = filtered.head;
  lex_from_active_ = filtered.head != 0;
}

void Preprocessor::expand_and_lex_from(int head, TokenList* list, ExpandMode mode) {
  TokenList expanded = new_list();
  const uint32_t token = new_token(head);
  append_token(&expanded, token);
  expand_list(list, mode);
  if (aborted_) return;
  append_list(&expanded, *list);
  lex_from(expanded);
}

// The token made by pasting two. On failure, the first one and an error.
uint32_t Preprocessor::paste_tokens(uint32_t token, uint32_t other) {
  const int type = tokens_[token].type, other_type = tokens_[other].type;
  if (other_type == kPlaceholder) return token;
  if (type == kPlaceholder) return other;
  int combined = 0;
  switch (type) {
    case '<': combined = other_type == '<' ? kLeftShift : other_type == '=' ? kLessOrEqual : 0; break;
    case '>': combined = other_type == '>' ? kRightShift : other_type == '=' ? kGreaterOrEqual : 0; break;
    case '=': combined = other_type == '=' ? kEqual : 0; break;
    case '!': combined = other_type == '=' ? kNotEqual : 0; break;
    case '&': combined = other_type == '&' ? kAnd : 0; break;
    case '|': combined = other_type == '|' ? kOr : 0; break;
    default: break;
  }
  if (combined) {
    const uint32_t result = new_token(combined);
    if (result) tokens_[result].location = tokens_[token].location;
    return result ? result : token;
  }
  const auto is_text = [](int t) { return t == kIdentifier || t == kOther || t == kIntegerString || t == kInteger; };
  bool valid = is_text(type) && is_text(other_type);
  // Only digits can be pasted onto an integer.
  if (valid && (type == kIntegerString || type == kInteger)) {
    if (other_type == kIntegerString) valid = is_digit(tokens_[other].text[0]);
    else if (other_type == kInteger) valid = tokens_[other].value >= 0;
    else valid = false;
  }
  if (!valid) {
    const Location location = tokens_[token].location;
    error(location, "");
    log_text("Pasting \"");
    log_token(token);
    log_text("\" and \"");
    log_token(other);
    log_text("\" does not give a valid preprocessing token.\n");
    return token;
  }
  String joined(result_.output.allocator());
  const bool appended = (type == kInteger ? joined.append_int(tokens_[token].value) : joined.append(tokens_[token].text)) &&
                        (other_type == kInteger ? joined.append_int(tokens_[other].value) : joined.append(tokens_[other].text));
  if (!appended || joined.size() > kMaxPastedBytes - pasted_bytes_) {
    fail_limit();
    return token;
  }
  pasted_bytes_ += joined.size();
  const char* const text = joined.data();
  const uint64_t size = joined.size();
  // A String's characters stay where they are when the String moves, so the view stays good when the list of them grows.
  if (!pasted_.push_back(static_cast<String&&>(joined))) {
    fail_limit();
    return token;
  }
  const uint32_t result = new_string_token(type == kInteger ? kIntegerString : type, StringView(text, size));
  if (!result) return token;
  tokens_[result].location = tokens_[token].location;
  return result;
}

void Preprocessor::apply_pastes(TokenList* list) {
  uint32_t node = list->head;
  while (node && !aborted_) {
    tick();
    uint32_t next_non_space = next_of(node);
    while (next_non_space && type_of_node(next_non_space) == kSpace) next_non_space = next_of(next_non_space);
    if (!next_non_space) break;
    if (type_of_node(next_non_space) != kPaste) {
      node = next_non_space;
      continue;
    }
    next_non_space = next_of(next_non_space);
    while (next_non_space && type_of_node(next_non_space) == kSpace) next_non_space = next_of(next_non_space);
    if (!next_non_space) {
      error(tokens_[nodes_[node].token].location, "'##' cannot appear at either end of a macro expansion\n");
      return;
    }
    const uint32_t pasted = paste_tokens(nodes_[node].token, nodes_[next_non_space].token);
    nodes_[node].token = pasted;
    set_next(node, next_of(next_non_space));
    if (next_non_space == list->tail) list->tail = node;
  }
  list->non_space_tail = list->tail;
}

// The arguments of a function-like macro: looks for a balanced set of parentheses after `node`, adds one list per argument to arguments_, and sets *last to the closing
// parenthesis.
Preprocessor::ArgumentStatus Preprocessor::parse_arguments(uint32_t node, uint32_t* last) {
  node = next_of(node);
  while (node && type_of_node(node) == kSpace) {
    tick();
    node = next_of(node);
  }
  if (!node || type_of_node(node) != '(') return ArgumentStatus::kNotAFunction;
  node = next_of(node);
  uint32_t current = arguments_.allocate();
  if (!current) {
    fail_limit();
    return ArgumentStatus::kNotAFunction;
  }
  arguments_[current] = new_list();
  int64_t paren_count = 1;
  for (; node && !aborted_; node = next_of(node)) {
    const int type = type_of_node(node);
    if (type == '(') {
      ++paren_count;
    } else if (type == ')') {
      if (--paren_count == 0) break;
    }
    if (type == ',' && paren_count == 1) {
      // Each list's storage may move as arguments_ grows, so nothing keeps a reference across an allocation.
      TokenList finished = arguments_[current];
      trim_trailing_space(&finished);
      arguments_[current] = finished;
      current = arguments_.allocate();
      if (!current) {
        fail_limit();
        return ArgumentStatus::kNotAFunction;
      }
      arguments_[current] = new_list();
    } else {
      // Initial whitespace is no part of the argument.
      if (arguments_[current].head == 0 && type == kSpace) continue;
      TokenList list = arguments_[current];
      append_token(&list, nodes_[node].token);
      arguments_[current] = list;
    }
  }
  if (paren_count) return ArgumentStatus::kUnbalanced;
  *last = node;
  return ArgumentStatus::kSuccess;
}

bool Preprocessor::expand_function(uint32_t node, uint32_t* last, ExpandMode mode, uint32_t macro, TokenList* expansion) {
  const StringView identifier = text_of_node(node);
  const Location location = tokens_[nodes_[node].token].location;
  const uint32_t base = arguments_.used();
  const auto finish = [&](bool result) {
    arguments_.release_to(base);
    return result;
  };
  const ArgumentStatus status = parse_arguments(node, last);
  if (aborted_ || status == ArgumentStatus::kNotAFunction) return finish(false);
  if (status == ArgumentStatus::kUnbalanced) {
    error(location, "Macro ", identifier, " call has unbalanced parentheses\n");
    return finish(false);
  }
  const TokenList replacements = macros_[macro].replacements;
  const TokenList parameters = macros_[macro].parameters;
  // A macro defined as empty becomes a space, whatever its arguments.
  if (!replacements.present) {
    *expansion = list_with_one_space();
    return finish(true);
  }
  const uint32_t argument_count = arguments_.used() - base;
  uint32_t parameter_count = 0;
  for (uint32_t p = parameters.head; p; p = next_of(p)) ++parameter_count;
  if (!(argument_count == parameter_count || (parameter_count == 0 && argument_count == 1 && arguments_[base].head == 0))) {
    has_error_ = true;
    log_message_start(location, "error");
    log_text("Error: macro ");
    log_text(identifier);
    log_text(" invoked with ");
    log_number(argument_count);
    log_text(" arguments (expected ");
    log_number(parameter_count);
    log_text(")\n\n");
    return finish(false);
  }
  TokenList substituted = new_list();
  bool previous_was_paste = false;
  for (uint32_t r = replacements.head; r && !aborted_; r = next_of(r)) {
    const int type = type_of_node(r);
    uint32_t index = 0, parameter = 0;
    if (type == kIdentifier) {
      const StringView name = text_of_node(r);
      for (parameter = parameters.head; parameter; parameter = next_of(parameter), ++index) {
        tick();
        if (text_of_node(parameter) == name) break;
      }
    }
    if (parameter) {
      // An argument is expanded before it is substituted, unless a ## is next to it. An empty one becomes a placeholder.
      const TokenList argument = arguments_[base + index];
      if (argument.head) {
        TokenList expanded = copy_list(argument);
        uint32_t next_non_space = next_of(r);
        while (next_non_space && type_of_node(next_non_space) == kSpace) next_non_space = next_of(next_non_space);
        if (!previous_was_paste && (!next_non_space || type_of_node(next_non_space) != kPaste)) expand_list(&expanded, mode);
        if (aborted_) return finish(false);
        append_list(&substituted, expanded);
      } else {
        append_token(&substituted, new_token(kPlaceholder));
      }
    } else {
      append_token(&substituted, nodes_[r].token);
    }
    if (type != kSpace) previous_was_paste = type == kPaste;
  }
  trim_trailing_space(&substituted);
  apply_pastes(&substituted);
  *expansion = substituted;
  return finish(true);
}

// The complete expansion of a node and, for a function-like macro, of the nodes that are its arguments. False if there is none. *last is the last node consumed.
bool Preprocessor::expand_node(uint32_t node, uint32_t* last, ExpandMode mode, int32_t line, TokenList* expansion, uint32_t* macro) {
  {
    const Token& token = tokens_[nodes_[node].token];
    if (token.expanding || token.type != kIdentifier) return false;
  }
  *last = node;
  const StringView identifier = text_of_node(node);
  if (identifier.size() > 0 && identifier[0] == '_') {
    if (identifier == "__LINE__") {
      *expansion = list_with_one(new_integer_token(line));
      return true;
    }
    if (identifier == "__FILE__") {
      const int32_t source = static_cast<int32_t>(tokens_[nodes_[node].token].location.source);
      *expansion = list_with_one(new_integer_token(source));
      return true;
    }
  }
  const uint32_t found = find_macro(identifier);
  if (!found) return false;
  *macro = found;
  // Already being expanded: it stays as it is, then and in any later scan.
  if (macros_[found].active) {
    const uint32_t final_token = new_string_token(kIdentifier, identifier);
    if (final_token) {
      tokens_[final_token].expanding = true;
      tokens_[final_token].location = Location{0, 0, 0};
    }
    *expansion = list_with_one(final_token);
    return true;
  }
  if (!macros_[found].is_function) {
    const TokenList replacements = macros_[found].replacements;
    if (!replacements.present) {
      *expansion = list_with_one_space();
      return true;
    }
    *expansion = copy_list(replacements);
    apply_pastes(expansion);
    return true;
  }
  return expand_function(node, last, mode, found, expansion);
}

// Walks over a list replacing nodes by their expansion, and those in turn, in place. With kEvaluateDefined any `defined` in it, or out of an expansion, becomes 0 or 1.
void Preprocessor::expand_list(TokenList* list, ExpandMode mode) {
  if (!list->present || aborted_) return;
  if (expand_depth_ >= kMaxExpandDepth) {
    fail_limit();
    return;
  }
  ++expand_depth_;
  const uint32_t active_initial = active_.used();
  trim_trailing_space(list);
  const int32_t line = tokens_[nodes_[list->tail].token].location.line;
  uint32_t previous = 0;
  // Taken ahead of the evaluation of `defined`, as glcpp does: a `defined` at the very start is walked over as it was.
  uint32_t node = list->head;
  if (mode == ExpandMode::kEvaluateDefined) evaluate_defined_in_list(list);
  while (node && !aborted_) {
    tick();
    while (active_top_is(node)) pop_active();
    uint32_t last = 0, macro = 0;
    TokenList expansion;
    if (expand_node(node, &last, mode, line, &expansion, &macro)) {
      if (aborted_) break;
      if (mode == ExpandMode::kEvaluateDefined) evaluate_defined_in_list(&expansion);
      const uint32_t after = next_of(last);
      for (uint32_t n = node; n && n != after; n = next_of(n)) {
        tick();
        while (active_top_is(n)) pop_active();
      }
      const uint32_t slot = active_.allocate();
      if (!slot) {
        fail_limit();
        break;
      }
      active_[slot].macro = macro;
      active_[slot].marker = after;
      if (macro) ++macros_[macro].active;
      if (expansion.head) {
        if (previous) set_next(previous, expansion.head);
        else list->head = expansion.head;
        set_next(expansion.tail, after);
        if (last == list->tail) list->tail = expansion.tail;
      } else {
        if (previous) set_next(previous, after);
        else list->head = after;
        if (last == list->tail) list->tail = 0;
      }
    } else {
      previous = node;
    }
    node = previous ? next_of(previous) : list->head;
  }
  while (active_.used() > active_initial) pop_active();
  list->non_space_tail = list->tail;
  --expand_depth_;
}

void Preprocessor::print_expanded_list(TokenList* list) {
  if (!list->present) return;
  expand_list(list, ExpandMode::kIgnoreDefined);
  if (aborted_) return;
  trim_trailing_space(list);
  for (uint32_t node = list->head; node && !aborted_; node = next_of(node)) {
    emit_token(nodes_[node].token);
    // No accidental concatenation of tokens: "+ +" must not become "++".
    const uint32_t next = next_of(node);
    if (next) {
      const int a = type_of_node(node), b = type_of_node(next);
      if (((a == '+' || a == '-') && b == a) || (a == kIdentifier && (b == kIdentifier || b == kInteger))) emit_char(' ');
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Skip stack

void Preprocessor::skip_push_if(const Location& location, bool condition) {
  const SkipType current = skip_stack_.count() ? skip_stack_[skip_stack_.count() - 1].type : SkipType::kNoSkip;
  SkipNode node;
  node.location = location;
  node.type = current != SkipType::kNoSkip ? SkipType::kToEndif : (condition ? SkipType::kNoSkip : SkipType::kToElse);
  if (!skip_stack_.push_back(node)) fail_limit();
}

void Preprocessor::skip_change_if(const Location& location, StringView type, bool condition) {
  if (skip_stack_.count() == 0) {
    error(location, "#", type, " without #if\n");
    return;
  }
  SkipNode& top = skip_stack_[skip_stack_.count() - 1];
  if (top.type == SkipType::kToElse) {
    if (condition) top.type = SkipType::kNoSkip;
  } else {
    top.type = SkipType::kToEndif;
  }
}

void Preprocessor::skip_pop(const Location& location) {
  if (skip_stack_.count() == 0) {
    error(location, "#endif without #if\n");
    return;
  }
  (void)skip_stack_.remove_at(skip_stack_.count() - 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Line continuations, ahead of everything else

namespace {
uint64_t skip_newline(StringView text, uint64_t index) {
  const char c = index < text.size() ? text[index] : '\0';
  const char d = index + 1 < text.size() ? text[index + 1] : '\0';
  if ((c == '\r' && d == '\n') || (c == '\n' && d == '\r')) return index + 2;
  return c == '\r' || c == '\n' ? index + 1 : index;
}
}  // namespace

// Collapses the lines joined by a backslash, and makes up for them at the next newline so that line numbers stay right. The inserted newlines are of the kind the source
// uses first. False if memory ran out. Without any backslash `*out` stays the source itself.
bool remove_line_continuations(StringView source, String* buffer, StringView* out) {
  *out = source;
  constexpr uint64_t npos = StringView::npos;
  uint64_t backslash = source.find('\\');
  if (backslash == npos) return true;
  uint64_t cr = source.find('\r'), lf = source.find('\n');
  StringView separator = "\n";
  if (cr == npos) {
  } else if (lf == npos) {
    separator = "\r";
  } else if (lf == cr + 1) {
    separator = "\r\n";
  } else if (cr == lf + 1) {
    separator = "\n\r";
  }
  uint64_t shader = 0, search = 0, collapsed = 0;
  const auto copy = [&](uint64_t from, uint64_t to) { return from >= to || buffer->append(source.substr(from, to - from)); };
  for (;;) {
    if (collapsed) {
      // Moved on only when passed, so that many backslashes do not make for a quadratic search.
      if (cr != npos && cr < search) cr = source.find('\r', search);
      if (lf != npos && lf < search) lf = source.find('\n', search);
      const uint64_t newline = cr < lf ? cr : lf;
      if (newline != npos && (backslash == npos || newline < backslash)) {
        if (!copy(shader, newline + 1)) return false;
        for (; collapsed; --collapsed)
          if (!buffer->append(separator)) return false;
        shader = skip_newline(source, newline);
        search = shader;
      }
    }
    if (backslash == npos) break;
    search = backslash + 1;
    const char next = search < source.size() ? source[search] : '\0';
    if (next == '\r' || next == '\n') {
      ++collapsed;
      if (!copy(shader, backslash)) return false;
      shader = skip_newline(source, backslash + 1);
      search = shader;
    }
    backslash = source.find('\\', search);
  }
  if (!copy(shader, source.size())) return false;
  *out = buffer->view();
  return true;
}

bool Preprocessor::run(StringView source) {
  const uint64_t nul = source.find('\0');
  if (nul != StringView::npos) source = source.first(nul);
  work_limit_ = kBaseWork + source.size() * kWorkPerByte;
  if (!nodes_.init() || !tokens_.init() || !macros_.init() || !active_.init() || !arguments_.init() || !buckets_.reserve(kMacroBuckets)) return false;
  for (uint32_t i = 0; i < kMacroBuckets; ++i) (void)buckets_.push_back(0);
  if (options_.disable_line_continuation) text_ = source;
  else if (!remove_line_continuations(source, &continued_, &text_)) return false;

  parse_input();

  if (skip_stack_.count() != 0) error(skip_stack_[skip_stack_.count() - 1].location, "Unterminated #if\n");
  // The run may have ended before any token decided this; the macros no longer matter then, and neither does a full arena.
  const bool was_aborted = aborted_;
  aborted_ = false;
  resolve_implicit_version();
  aborted_ = was_aborted;
  return !has_error_;
}

}  // namespace

bool preprocess_glsl(allocators::IAllocator& allocator, StringView source, const PreprocessOptions& options, PreprocessResult* result) {
  result->output.clear();
  result->log.clear();
  result->ok = true;
  result->version = options.default_version;
  result->version_declared = false;
  result->es = options.default_es;
  result->profile = 0;
  Preprocessor preprocessor(allocator, options, result);
  result->ok = preprocessor.run(source);
  return result->ok;
}

}  // namespace shader

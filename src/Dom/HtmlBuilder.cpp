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

// The HTML parser of the HTML Standard (section 13.2): the tokenizer with all its states, and the tree construction with its insertion modes, the list of
// active formatting elements and the adoption agency algorithm, foster parenting, foreign content (SVG and MathML), templates, and the fragment case.
// Scripts are handed to an IScriptRunner as the parser meets them; what they document.write is read in their place. Not done: the doctype node itself
// (only its effect on quirks mode), the character references with two code points.
#include "Dom/HtmlBuilder.hpp"
#include <initializer_list>
#include "Dom/Entities.hpp"
#include "Utility/Move.hpp"

namespace dom {

namespace {

const char kSvgNs[] = "http://www.w3.org/2000/svg";
const char kMathNs[] = "http://www.w3.org/1998/Math/MathML";

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
bool is_letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_upper(int c) { return c >= 'A' && c <= 'Z'; }

bool name_in(StringView name, std::initializer_list<const char*> names) {
  for (const char* n : names)
    if (name.equals(n)) return true;
  return false;
}

bool starts_with_ci(StringView s, const char* prefix) {
  uint64_t i = 0;
  for (; prefix[i]; ++i)
    if (i >= s.size() || lower(s[i]) != prefix[i]) return false;
  return true;
}

struct Attr {
  explicit Attr(allocators::IAllocator& a) : name(a), value(a) {}
  String name, value;
  bool dropped = false;   // a repeated name: the first one counts
};

struct Token {
  enum Type : uint8_t { None, Doctype, StartTag, EndTag, Comment, Characters, Eof };
  explicit Token(allocators::IAllocator& a) : name(a), attrs(a), data(a), pub(a), sys(a) {}
  void reset() {
    type = None;
    name.clear();
    attrs.clear();
    data.clear();
    pub.clear();
    sys.clear();
    self_closing = acknowledged = has_pub = has_sys = force_quirks = false;
  }
  Type type = None;
  String name;          // tags and doctype
  List<Attr> attrs;
  String data;          // comments and characters
  String pub, sys;      // doctype identifiers
  bool self_closing = false, acknowledged = false, has_pub = false, has_sys = false, force_quirks = false;
  bool has_attr(StringView n) const {
    for (uint64_t i = 0; i < attrs.count(); ++i)
      if (!attrs[i].dropped && attrs[i].name.view().equals(n)) return true;
    return false;
  }
  bool attr(StringView n, StringView* v) const {
    for (uint64_t i = 0; i < attrs.count(); ++i)
      if (!attrs[i].dropped && attrs[i].name.view().equals(n)) {
        *v = attrs[i].value.view();
        return true;
      }
    return false;
  }
};

enum class State : uint8_t {
  Data, Rcdata, Rawtext, ScriptData, Plaintext, TagOpen, EndTagOpen, TagName,
  RcdataLt, RcdataEndOpen, RcdataEndName, RawtextLt, RawtextEndOpen, RawtextEndName,
  ScriptLt, ScriptEndOpen, ScriptEndName, ScriptEscapeStart, ScriptEscapeStartDash, ScriptEscaped, ScriptEscapedDash, ScriptEscapedDashDash,
  ScriptEscapedLt, ScriptEscapedEndOpen, ScriptEscapedEndName, ScriptDoubleEscapeStart, ScriptDoubleEscaped, ScriptDoubleEscapedDash, ScriptDoubleEscapedDashDash,
  ScriptDoubleEscapedLt, ScriptDoubleEscapeEnd,
  BeforeAttrName, AttrName, AfterAttrName, BeforeAttrValue, AttrValueDq, AttrValueSq, AttrValueUnquoted, AfterAttrValueQuoted, SelfClosing,
  BogusComment, MarkupDeclOpen, CommentStart, CommentStartDash, Comment, CommentLt, CommentLtBang, CommentLtBangDash, CommentLtBangDashDash, CommentEndDash, CommentEnd,
  CommentEndBang,
  Doctype, BeforeDoctypeName, DoctypeName, AfterDoctypeName, AfterDoctypePublicKw, BeforePublicId, PublicIdDq, PublicIdSq, AfterPublicId, BetweenIds,
  AfterDoctypeSystemKw, BeforeSystemId, SystemIdDq, SystemIdSq, AfterSystemId, BogusDoctype,
  Cdata, CdataBracket, CdataEnd,
};

enum class Mode : uint8_t {
  Initial, BeforeHtml, BeforeHead, InHead, InHeadNoscript, AfterHead, InBody, Text, InTable, InTableText, InCaption, InColumnGroup, InTableBody, InRow, InCell,
  InSelect, InSelectInTable, InTemplate, AfterBody, InFrameset, AfterFrameset, AfterAfterBody, AfterAfterFrameset,
};

enum class Scope : uint8_t { Default, ListItem, Button, Table, Select };

bool quirky_public_id(StringView id) {
  static const char* const kPrefixes[] = {
      "+//silmaril//dtd html pro v0r11 19970101//", "-//as//dtd html 3.0 aswedit + extensions//", "-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
      "-//ietf//dtd html 2.0 level 1//", "-//ietf//dtd html 2.0 level 2//", "-//ietf//dtd html 2.0 strict level 1//", "-//ietf//dtd html 2.0 strict level 2//",
      "-//ietf//dtd html 2.0 strict//", "-//ietf//dtd html 2.0//", "-//ietf//dtd html 2.1e//", "-//ietf//dtd html 3.0//", "-//ietf//dtd html 3.2 final//",
      "-//ietf//dtd html 3.2//", "-//ietf//dtd html 3//", "-//ietf//dtd html level 0//", "-//ietf//dtd html level 1//", "-//ietf//dtd html level 2//",
      "-//ietf//dtd html level 3//", "-//ietf//dtd html strict level 0//", "-//ietf//dtd html strict level 1//", "-//ietf//dtd html strict level 2//",
      "-//ietf//dtd html strict level 3//", "-//ietf//dtd html strict//", "-//ietf//dtd html//", "-//metrius//dtd metrius presentational//",
      "-//microsoft//dtd internet explorer 2.0 html strict//", "-//microsoft//dtd internet explorer 2.0 html//", "-//microsoft//dtd internet explorer 2.0 tables//",
      "-//microsoft//dtd internet explorer 3.0 html strict//", "-//microsoft//dtd internet explorer 3.0 html//", "-//microsoft//dtd internet explorer 3.0 tables//",
      "-//netscape comm. corp.//dtd html//", "-//netscape comm. corp.//dtd strict html//", "-//o'reilly and associates//dtd html 2.0//",
      "-//o'reilly and associates//dtd html extended 1.0//", "-//o'reilly and associates//dtd html extended relaxed 1.0//", "-//sq//dtd html 2.0 hotmetal + extensions//",
      "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//", "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
      "-//spyglass//dtd html 2.0 extended//", "-//sun microsystems corp.//dtd hotjava html//", "-//sun microsystems corp.//dtd hotjava strict html//",
      "-//w3c//dtd html 3 1995-03-24//", "-//w3c//dtd html 3.2 draft//", "-//w3c//dtd html 3.2 final//", "-//w3c//dtd html 3.2//", "-//w3c//dtd html 3.2s draft//",
      "-//w3c//dtd html 4.0 frameset//", "-//w3c//dtd html 4.0 transitional//", "-//w3c//dtd html experimental 19960712//", "-//w3c//dtd html experimental 970421//",
      "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html 3.0//", "-//webtechs//dtd mozilla html 2.0//", "-//webtechs//dtd mozilla html//"};
  for (const char* p : kPrefixes)
    if (starts_with_ci(id, p)) return true;
  return id.equals_ignore_case("-//w3o//dtd w3 html strict 3.0//en//") || id.equals_ignore_case("-/w3c/dtd html 4.0 transitional/en") || id.equals_ignore_case("html");
}

class TreeBuilder {
 public:
  static constexpr uint32_t kMaxDepth = 400;

  TreeBuilder(Document& doc, IScriptRunner* scripts, Node* fragment, Node* context)
      : doc_(doc), scripts_(scripts), fragment_(fragment), context_(context), a_(doc.allocator()), open_(a_), afe_(a_), tmpl_modes_(a_), input_(a_), chars_(a_), temp_(a_),
        last_start_(a_), attr_name_(a_), attr_value_(a_), tok_a_(a_), tok_b_(a_), tok_c_(a_), pending_text_(a_), table_text_(a_), written_(a_) {}

  bool run(StringView html) {
    normalize_input(html);
    scripting_ = fragment_ ? true : scripts_ != nullptr;
    if (fragment_) setup_fragment();
    else {
      mode_ = Mode::Initial;
      doc_.quirks_mode = true;   // (until a doctype says otherwise)
    }
    parse();
    if (fragment_) finish_fragment();
    return !failed_;
  }

 private:
  // ======================================================================== input
  // Newlines are normalized (CR LF and CR are LF) when the input is read.
  void normalize_input(StringView html) {
    input_.reserve(html.size());
    for (uint64_t i = 0; i < html.size(); ++i) {
      if (html[i] == '\r') {
        input_.push_back('\n');
        if (i + 1 < html.size() && html[i + 1] == '\n') ++i;
      } else {
        input_.push_back(html[i]);
      }
    }
  }
  int get() { return pos_ < input_.size() ? static_cast<unsigned char>(input_[pos_++]) : -1; }
  void unget(int c) {
    if (c >= 0) --pos_;
  }
  bool lookahead(const char* word, bool ignore_case) const {
    uint64_t i = 0;
    for (; word[i]; ++i) {
      if (pos_ + i >= input_.size()) return false;
      const char c = input_[pos_ + i];
      if (ignore_case ? lower(c) != lower(word[i]) : c != word[i]) return false;
    }
    return true;
  }

  // ======================================================================== tokenizer
  Token& tb() { return *tb_; }
  void emit_char(char c) { chars_.push_back(c); }
  void emit_str(StringView s) { chars_.append(s); }
  void emit_fffd() { chars_.append("\xEF\xBF\xBD"); }
  bool appropriate_end() const { return tb_->name.view().equals(last_start_.view()); }
  void new_tag(Token::Type t) {
    tb_->reset();
    tb_->type = t;
  }
  void new_attr() {
    // the previous attribute is complete: a name already used is dropped
    attrs_check();
    tb_->attrs.push_back(Attr(a_));
  }
  void attrs_check() {
    if (tb_->attrs.count() == 0) return;
    Attr& last = tb_->attrs[tb_->attrs.count() - 1];
    for (uint64_t i = 0; i + 1 < tb_->attrs.count(); ++i)
      if (!tb_->attrs[i].dropped && tb_->attrs[i].name.view().equals(last.name.view())) last.dropped = true;
  }
  Attr& cur_attr() { return tb_->attrs[tb_->attrs.count() - 1]; }
  // The token is complete: it is handed out after the characters before it.
  void finish_token() {
    if (tb_->type == Token::StartTag || tb_->type == Token::EndTag) {
      attrs_check();
      if (tb_->type == Token::StartTag) last_start_.assign(tb_->name.view());
    }
    ready_ = true;
  }
  void char_reference(bool in_attribute, String* into) {
    // the '&' was just read
    uint64_t consumed = decode_reference(input_.view(), pos_ - 1, in_attribute, into);
    if (consumed == 0) consumed = 1;
    pos_ += consumed - 1;
  }
  void append_ref_text(bool attribute) {
    String text(a_);
    char_reference(attribute, &text);
    if (attribute) attr_append(text.view());
    else emit_str(text.view());
  }
  void attr_append(StringView s) { cur_attr().value.append(s); }

  // Reads the next token into out_. The characters read so far come out first.
  Token* next_token() {
    if (held_) {
      Token* t = held_;
      held_ = nullptr;
      return t;
    }
    ready_ = false;
    tb_ = &tok_a_;
    tb_->reset();
    tokenize();
    Token* result = tb_;
    if (!chars_.empty()) {   // characters come first; the token that ended them follows
      tok_c_.reset();
      tok_c_.type = Token::Characters;
      tok_c_.data.assign(chars_.view());
      chars_.clear();
      if (ready_) {
        held_ = result;
        // keep the held token intact: it lives in tok_a_, the characters in tok_c_
      }
      return &tok_c_;
    }
    return result;
  }

  void finish_eof() {
    tb_->reset();
    tb_->type = Token::Eof;
    ready_ = true;
  }

  void tokenize() {
    for (;;) {
      if (ready_) return;
      if (chars_.size() >= 8192 && tb_->type == Token::None) return;   // (a long run of text is handed over in pieces)
      switch (state_) {
        case State::Data: {
          const int c = get();
          if (c == '&') append_ref_text(false);
          else if (c == '<') state_ = State::TagOpen;
          else if (c == 0) emit_char('\0');
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::Rcdata: {
          const int c = get();
          if (c == '&') append_ref_text(false);
          else if (c == '<') state_ = State::RcdataLt;
          else if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::Rawtext: {
          const int c = get();
          if (c == '<') state_ = State::RawtextLt;
          else if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::ScriptData: {
          const int c = get();
          if (c == '<') state_ = State::ScriptLt;
          else if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::Plaintext: {
          const int c = get();
          if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::TagOpen: {
          const int c = get();
          if (c == '!') state_ = State::MarkupDeclOpen;
          else if (c == '/') state_ = State::EndTagOpen;
          else if (is_letter(c)) {
            new_tag(Token::StartTag);
            unget(c);
            state_ = State::TagName;
          } else if (c == '?') {
            tb_->reset();
            tb_->type = Token::Comment;
            unget(c);
            state_ = State::BogusComment;
          } else if (c < 0) {
            emit_char('<');
            finish_eof();
          } else {
            emit_char('<');
            unget(c);
            state_ = State::Data;
          }
          break;
        }
        case State::EndTagOpen: {
          const int c = get();
          if (is_letter(c)) {
            new_tag(Token::EndTag);
            unget(c);
            state_ = State::TagName;
          } else if (c == '>') state_ = State::Data;
          else if (c < 0) {
            emit_str("</");
            finish_eof();
          } else {
            tb_->reset();
            tb_->type = Token::Comment;
            unget(c);
            state_ = State::BogusComment;
          }
          break;
        }
        case State::TagName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BeforeAttrName;
          else if (c == '/') state_ = State::SelfClosing;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c == 0) tb_->name.append("\xEF\xBF\xBD");
          else if (c < 0) finish_eof_in_tag();
          else tb_->name.push_back(lower(static_cast<char>(c)));
          break;
        }
        case State::RcdataLt: case State::RawtextLt: case State::ScriptLt: {
          const int c = get();
          const State base = state_ == State::RcdataLt ? State::Rcdata : state_ == State::RawtextLt ? State::Rawtext : State::ScriptData;
          if (c == '/') {
            temp_.clear();
            state_ = base == State::Rcdata ? State::RcdataEndOpen : base == State::Rawtext ? State::RawtextEndOpen : State::ScriptEndOpen;
          } else if (c == '!' && base == State::ScriptData) {
            emit_str("<!");
            state_ = State::ScriptEscapeStart;
          } else {
            emit_char('<');
            unget(c);
            state_ = base;
          }
          break;
        }
        case State::RcdataEndOpen: case State::RawtextEndOpen: case State::ScriptEndOpen: {
          const int c = get();
          const State name_state = state_ == State::RcdataEndOpen ? State::RcdataEndName : state_ == State::RawtextEndOpen ? State::RawtextEndName : State::ScriptEndName;
          const State base = state_ == State::RcdataEndOpen ? State::Rcdata : state_ == State::RawtextEndOpen ? State::Rawtext : State::ScriptData;
          if (is_letter(c)) {
            new_tag(Token::EndTag);
            unget(c);
            state_ = name_state;
          } else {
            emit_str("</");
            unget(c);
            state_ = base;
          }
          break;
        }
        case State::RcdataEndName: case State::RawtextEndName: case State::ScriptEndName: case State::ScriptEscapedEndName: {
          const State base = state_ == State::RcdataEndName ? State::Rcdata : state_ == State::RawtextEndName ? State::Rawtext : state_ == State::ScriptEndName ? State::ScriptData : State::ScriptEscaped;
          end_tag_name(base);
          break;
        }
        case State::ScriptEscapeStart: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptEscapeStartDash;
          } else {
            unget(c);
            state_ = State::ScriptData;
          }
          break;
        }
        case State::ScriptEscapeStartDash: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptEscapedDashDash;
          } else {
            unget(c);
            state_ = State::ScriptData;
          }
          break;
        }
        case State::ScriptEscaped: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptEscapedDash;
          } else if (c == '<') state_ = State::ScriptEscapedLt;
          else if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::ScriptEscapedDash: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptEscapedDashDash;
          } else if (c == '<') state_ = State::ScriptEscapedLt;
          else if (c == 0) {
            emit_fffd();
            state_ = State::ScriptEscaped;
          } else if (c < 0) finish_eof();
          else {
            emit_char(static_cast<char>(c));
            state_ = State::ScriptEscaped;
          }
          break;
        }
        case State::ScriptEscapedDashDash: {
          const int c = get();
          if (c == '-') emit_char('-');
          else if (c == '<') state_ = State::ScriptEscapedLt;
          else if (c == '>') {
            emit_char('>');
            state_ = State::ScriptData;
          } else if (c == 0) {
            emit_fffd();
            state_ = State::ScriptEscaped;
          } else if (c < 0) finish_eof();
          else {
            emit_char(static_cast<char>(c));
            state_ = State::ScriptEscaped;
          }
          break;
        }
        case State::ScriptEscapedLt: {
          const int c = get();
          if (c == '/') {
            temp_.clear();
            state_ = State::ScriptEscapedEndOpen;
          } else if (is_letter(c)) {
            temp_.clear();
            emit_char('<');
            unget(c);
            state_ = State::ScriptDoubleEscapeStart;
          } else {
            emit_char('<');
            unget(c);
            state_ = State::ScriptEscaped;
          }
          break;
        }
        case State::ScriptEscapedEndOpen: {
          const int c = get();
          if (is_letter(c)) {
            new_tag(Token::EndTag);
            unget(c);
            state_ = State::ScriptEscapedEndName;
          } else {
            emit_str("</");
            unget(c);
            state_ = State::ScriptEscaped;
          }
          break;
        }
        case State::ScriptDoubleEscapeStart: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>') {
            state_ = temp_.view().equals("script") ? State::ScriptDoubleEscaped : State::ScriptEscaped;
            emit_char(static_cast<char>(c));
          } else if (is_letter(c)) {
            temp_.push_back(lower(static_cast<char>(c)));
            emit_char(static_cast<char>(c));
          } else {
            unget(c);
            state_ = State::ScriptEscaped;
          }
          break;
        }
        case State::ScriptDoubleEscaped: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptDoubleEscapedDash;
          } else if (c == '<') {
            emit_char('<');
            state_ = State::ScriptDoubleEscapedLt;
          } else if (c == 0) emit_fffd();
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::ScriptDoubleEscapedDash: {
          const int c = get();
          if (c == '-') {
            emit_char('-');
            state_ = State::ScriptDoubleEscapedDashDash;
          } else if (c == '<') {
            emit_char('<');
            state_ = State::ScriptDoubleEscapedLt;
          } else if (c == 0) {
            emit_fffd();
            state_ = State::ScriptDoubleEscaped;
          } else if (c < 0) finish_eof();
          else {
            emit_char(static_cast<char>(c));
            state_ = State::ScriptDoubleEscaped;
          }
          break;
        }
        case State::ScriptDoubleEscapedDashDash: {
          const int c = get();
          if (c == '-') emit_char('-');
          else if (c == '<') {
            emit_char('<');
            state_ = State::ScriptDoubleEscapedLt;
          } else if (c == '>') {
            emit_char('>');
            state_ = State::ScriptData;
          } else if (c == 0) {
            emit_fffd();
            state_ = State::ScriptDoubleEscaped;
          } else if (c < 0) finish_eof();
          else {
            emit_char(static_cast<char>(c));
            state_ = State::ScriptDoubleEscaped;
          }
          break;
        }
        case State::ScriptDoubleEscapedLt: {
          const int c = get();
          if (c == '/') {
            temp_.clear();
            emit_char('/');
            state_ = State::ScriptDoubleEscapeEnd;
          } else {
            unget(c);
            state_ = State::ScriptDoubleEscaped;
          }
          break;
        }
        case State::ScriptDoubleEscapeEnd: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>') {
            state_ = temp_.view().equals("script") ? State::ScriptEscaped : State::ScriptDoubleEscaped;
            emit_char(static_cast<char>(c));
          } else if (is_letter(c)) {
            temp_.push_back(lower(static_cast<char>(c)));
            emit_char(static_cast<char>(c));
          } else {
            unget(c);
            state_ = State::ScriptDoubleEscaped;
          }
          break;
        }
        case State::BeforeAttrName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          if (c == '/' || c == '>' || c < 0) {
            unget(c);
            state_ = State::AfterAttrName;
          } else if (c == '=') {
            new_attr();
            cur_attr().name.push_back('=');
            state_ = State::AttrName;
          } else {
            new_attr();
            unget(c);
            state_ = State::AttrName;
          }
          break;
        }
        case State::AttrName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>' || c < 0) {
            unget(c);
            state_ = State::AfterAttrName;
          } else if (c == '=') state_ = State::BeforeAttrValue;
          else if (c == 0) cur_attr().name.append("\xEF\xBF\xBD");
          else cur_attr().name.push_back(lower(static_cast<char>(c)));
          break;
        }
        case State::AfterAttrName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          if (c == '/') state_ = State::SelfClosing;
          else if (c == '=') state_ = State::BeforeAttrValue;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) finish_eof_in_tag();
          else {
            new_attr();
            unget(c);
            state_ = State::AttrName;
          }
          break;
        }
        case State::BeforeAttrValue: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          if (c == '"') state_ = State::AttrValueDq;
          else if (c == '\'') state_ = State::AttrValueSq;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else {
            unget(c);
            state_ = State::AttrValueUnquoted;
          }
          break;
        }
        case State::AttrValueDq: case State::AttrValueSq: {
          const int quote = state_ == State::AttrValueDq ? '"' : '\'';
          const int c = get();
          if (c == quote) state_ = State::AfterAttrValueQuoted;
          else if (c == '&') append_ref_text(true);
          else if (c == 0) attr_append("\xEF\xBF\xBD");
          else if (c < 0) finish_eof_in_tag();
          else cur_attr().value.push_back(static_cast<char>(c));
          break;
        }
        case State::AttrValueUnquoted: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BeforeAttrName;
          else if (c == '&') append_ref_text(true);
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c == 0) attr_append("\xEF\xBF\xBD");
          else if (c < 0) finish_eof_in_tag();
          else cur_attr().value.push_back(static_cast<char>(c));
          break;
        }
        case State::AfterAttrValueQuoted: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BeforeAttrName;
          else if (c == '/') state_ = State::SelfClosing;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) finish_eof_in_tag();
          else {
            unget(c);
            state_ = State::BeforeAttrName;
          }
          break;
        }
        case State::SelfClosing: {
          const int c = get();
          if (c == '>') {
            tb_->self_closing = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) finish_eof_in_tag();
          else {
            unget(c);
            state_ = State::BeforeAttrName;
          }
          break;
        }
        case State::BogusComment: {
          const int c = get();
          if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) {
            comment_then_eof();
          } else if (c == 0) tb_->data.append("\xEF\xBF\xBD");
          else tb_->data.push_back(static_cast<char>(c));
          break;
        }
        case State::MarkupDeclOpen: {
          if (lookahead("--", false)) {
            pos_ += 2;
            tb_->reset();
            tb_->type = Token::Comment;
            state_ = State::CommentStart;
          } else if (lookahead("doctype", true)) {
            pos_ += 7;
            state_ = State::Doctype;
          } else if (lookahead("[CDATA[", false)) {
            pos_ += 7;
            if (adjusted_current_foreign()) state_ = State::Cdata;
            else {
              tb_->reset();
              tb_->type = Token::Comment;
              tb_->data.assign("[CDATA[");
              state_ = State::BogusComment;
            }
          } else {
            tb_->reset();
            tb_->type = Token::Comment;
            state_ = State::BogusComment;
          }
          break;
        }
        case State::CommentStart: {
          const int c = get();
          if (c == '-') state_ = State::CommentStartDash;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else {
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::CommentStartDash: {
          const int c = get();
          if (c == '-') state_ = State::CommentEnd;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) comment_then_eof();
          else {
            tb_->data.push_back('-');
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::Comment: {
          const int c = get();
          if (c == '<') {
            tb_->data.push_back('<');
            state_ = State::CommentLt;
          } else if (c == '-') state_ = State::CommentEndDash;
          else if (c == 0) tb_->data.append("\xEF\xBF\xBD");
          else if (c < 0) comment_then_eof();
          else tb_->data.push_back(static_cast<char>(c));
          break;
        }
        case State::CommentLt: {
          const int c = get();
          if (c == '!') {
            tb_->data.push_back('!');
            state_ = State::CommentLtBang;
          } else if (c == '<') tb_->data.push_back('<');
          else {
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::CommentLtBang: {
          const int c = get();
          if (c == '-') state_ = State::CommentLtBangDash;
          else {
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::CommentLtBangDash: {
          const int c = get();
          if (c == '-') state_ = State::CommentLtBangDashDash;
          else {
            unget(c);
            state_ = State::CommentEndDash;
          }
          break;
        }
        case State::CommentLtBangDashDash: {
          const int c = get();
          unget(c);
          state_ = State::CommentEnd;
          break;
        }
        case State::CommentEndDash: {
          const int c = get();
          if (c == '-') state_ = State::CommentEnd;
          else if (c < 0) comment_then_eof();
          else {
            tb_->data.push_back('-');
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::CommentEnd: {
          const int c = get();
          if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c == '!') state_ = State::CommentEndBang;
          else if (c == '-') tb_->data.push_back('-');
          else if (c < 0) comment_then_eof();
          else {
            tb_->data.append("--");
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::CommentEndBang: {
          const int c = get();
          if (c == '-') {
            tb_->data.append("--!");
            state_ = State::CommentEndDash;
          } else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) comment_then_eof();
          else {
            tb_->data.append("--!");
            unget(c);
            state_ = State::Comment;
          }
          break;
        }
        case State::Doctype: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BeforeDoctypeName;
          else if (c < 0) doctype_eof(true);
          else {
            unget(c);
            state_ = State::BeforeDoctypeName;
          }
          break;
        }
        case State::BeforeDoctypeName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          tb_->reset();
          tb_->type = Token::Doctype;
          if (c == '>') {
            tb_->force_quirks = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else {
            if (c == 0) tb_->name.append("\xEF\xBF\xBD");
            else tb_->name.push_back(lower(static_cast<char>(c)));
            state_ = State::DoctypeName;
          }
          break;
        }
        case State::DoctypeName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::AfterDoctypeName;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c == 0) tb_->name.append("\xEF\xBF\xBD");
          else if (c < 0) doctype_eof(true);
          else tb_->name.push_back(lower(static_cast<char>(c)));
          break;
        }
        case State::AfterDoctypeName: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else {
            unget(c);
            if (lookahead("public", true)) {
              pos_ += 6;
              state_ = State::AfterDoctypePublicKw;
            } else if (lookahead("system", true)) {
              pos_ += 6;
              state_ = State::AfterDoctypeSystemKw;
            } else {
              tb_->force_quirks = true;
              state_ = State::BogusDoctype;
            }
          }
          break;
        }
        case State::AfterDoctypePublicKw: case State::BeforePublicId: {
          const int c = get();
          if ((c == '\t' || c == '\n' || c == '\f' || c == ' ')) {
            state_ = State::BeforePublicId;
          } else if (c == '"' || c == '\'') {
            tb_->has_pub = true;
            tb_->pub.clear();
            state_ = c == '"' ? State::PublicIdDq : State::PublicIdSq;
          } else if (c == '>') {
            tb_->force_quirks = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else {
            tb_->force_quirks = true;
            unget(c);
            state_ = State::BogusDoctype;
          }
          break;
        }
        case State::PublicIdDq: case State::PublicIdSq: {
          const int quote = state_ == State::PublicIdDq ? '"' : '\'';
          const int c = get();
          if (c == quote) state_ = State::AfterPublicId;
          else if (c == 0) tb_->pub.append("\xEF\xBF\xBD");
          else if (c == '>') {
            tb_->force_quirks = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else tb_->pub.push_back(static_cast<char>(c));
          break;
        }
        case State::AfterPublicId: case State::BetweenIds: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BetweenIds;
          else if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c == '"' || c == '\'') {
            tb_->has_sys = true;
            tb_->sys.clear();
            state_ = c == '"' ? State::SystemIdDq : State::SystemIdSq;
          } else if (c < 0) doctype_eof(true);
          else {
            tb_->force_quirks = true;
            unget(c);
            state_ = State::BogusDoctype;
          }
          break;
        }
        case State::AfterDoctypeSystemKw: case State::BeforeSystemId: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') state_ = State::BeforeSystemId;
          else if (c == '"' || c == '\'') {
            tb_->has_sys = true;
            tb_->sys.clear();
            state_ = c == '"' ? State::SystemIdDq : State::SystemIdSq;
          } else if (c == '>') {
            tb_->force_quirks = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else {
            tb_->force_quirks = true;
            unget(c);
            state_ = State::BogusDoctype;
          }
          break;
        }
        case State::SystemIdDq: case State::SystemIdSq: {
          const int quote = state_ == State::SystemIdDq ? '"' : '\'';
          const int c = get();
          if (c == quote) state_ = State::AfterSystemId;
          else if (c == 0) tb_->sys.append("\xEF\xBF\xBD");
          else if (c == '>') {
            tb_->force_quirks = true;
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else tb_->sys.push_back(static_cast<char>(c));
          break;
        }
        case State::AfterSystemId: {
          const int c = get();
          if (c == '\t' || c == '\n' || c == '\f' || c == ' ') break;
          if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(true);
          else {
            unget(c);
            state_ = State::BogusDoctype;
          }
          break;
        }
        case State::BogusDoctype: {
          const int c = get();
          if (c == '>') {
            state_ = State::Data;
            finish_token();
          } else if (c < 0) doctype_eof(false);
          break;
        }
        case State::Cdata: {
          const int c = get();
          if (c == ']') state_ = State::CdataBracket;
          else if (c < 0) finish_eof();
          else emit_char(static_cast<char>(c));
          break;
        }
        case State::CdataBracket: {
          const int c = get();
          if (c == ']') state_ = State::CdataEnd;
          else {
            emit_char(']');
            unget(c);
            state_ = State::Cdata;
          }
          break;
        }
        case State::CdataEnd: {
          const int c = get();
          if (c == ']') emit_char(']');
          else if (c == '>') state_ = State::Data;
          else {
            emit_str("]]");
            unget(c);
            state_ = State::Cdata;
          }
          break;
        }
      }
    }
  }

  void end_tag_name(State base) {
    const int c = get();
    const bool anything_else_needed = true;
    (void)anything_else_needed;
    if ((c == '\t' || c == '\n' || c == '\f' || c == ' ') && appropriate_end()) state_ = State::BeforeAttrName;
    else if (c == '/' && appropriate_end()) state_ = State::SelfClosing;
    else if (c == '>' && appropriate_end()) {
      state_ = State::Data;
      finish_token();
    } else if (is_letter(c)) {
      tb_->name.push_back(lower(static_cast<char>(c)));
      temp_.push_back(static_cast<char>(c));
    } else {
      emit_str("</");
      emit_str(temp_.view());
      unget(c);
      state_ = base;
    }
  }

  // The input ended inside a tag: the tag is dropped.
  void finish_eof_in_tag() { finish_eof(); }
  void comment_then_eof() {
    // the comment is emitted, then the end of the file
    state_ = State::Data;
    finish_token();
    eof_next_ = true;
  }
  void doctype_eof(bool force) {
    if (force) tb_->force_quirks = true;
    if (tb_->type != Token::Doctype) {
      tb_->type = Token::Doctype;
    }
    state_ = State::Data;
    finish_token();
    eof_next_ = true;
  }

  bool adjusted_current_foreign() const {
    const Node* n = adjusted_current();
    return n && n->ns_set;
  }
  const Node* adjusted_current() const {
    if (fragment_ && open_.count() == 1) return context_;
    return open_.count() ? open_[open_.count() - 1] : nullptr;
  }

  // ======================================================================== the tree: elements and the stack
  bool is_html(const Node* n) const { return !n->ns_set; }
  bool is_el(const Node* n, StringView name) const { return n && n->type == NodeType::Element && !n->ns_set && n->name.view().equals(name); }
  bool in_ns(const Node* n, const char* ns) const { return n && n->ns_set && n->ns.view().equals(ns); }
  Node* cur() const { return open_.count() ? open_[open_.count() - 1] : nullptr; }
  bool cur_is(StringView name) const { return is_el(cur(), name); }

  bool is_special(const Node* n) const {
    if (!n->is_element()) return false;
    const StringView t = n->name.view();
    if (!n->ns_set)
      return name_in(t, {"address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote", "body", "br", "button", "caption", "center", "col", "colgroup",
                         "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5",
                         "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img", "input", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed",
                         "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre", "script", "search", "section", "select", "source", "style", "summary",
                         "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp"});
    if (n->ns.view().equals(kMathNs)) return name_in(t, {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
    if (n->ns.view().equals(kSvgNs)) return name_in(t, {"foreignobject", "desc", "title"});
    return false;
  }
  bool is_formatting(const Node* n) const {
    return is_html(n) && name_in(n->name.view(), {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"});
  }
  bool is_mathml_text_ip(const Node* n) const { return in_ns(n, kMathNs) && name_in(n->name.view(), {"mi", "mo", "mn", "ms", "mtext"}); }
  bool is_html_ip(const Node* n) const {
    if (in_ns(n, kSvgNs)) return name_in(n->name.view(), {"foreignobject", "desc", "title"});
    if (in_ns(n, kMathNs) && n->name.view().equals("annotation-xml")) {
      StringView enc;
      return get_attribute(n, "encoding", &enc) && (enc.equals_ignore_case("text/html") || enc.equals_ignore_case("application/xhtml+xml"));
    }
    return false;
  }

  bool scope_boundary(const Node* n, Scope s) const {
    if (!n->ns_set) {
      const StringView t = n->name.view();
      if (s == Scope::Select) return !name_in(t, {"optgroup", "option"});
      if (s == Scope::Table) return name_in(t, {"html", "table", "template"});
      if (name_in(t, {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "template"})) return true;
      if (s == Scope::ListItem && name_in(t, {"ol", "ul"})) return true;
      if (s == Scope::Button && t.equals("button")) return true;
      return false;
    }
    if (s == Scope::Select || s == Scope::Table) return false;
    if (in_ns(n, kMathNs)) return name_in(n->name.view(), {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
    return in_ns(n, kSvgNs) && name_in(n->name.view(), {"foreignobject", "desc", "title"});
  }
  bool in_scope(StringView name, Scope s = Scope::Default) const {
    for (uint64_t i = open_.count(); i > 0; --i) {
      const Node* n = open_[i - 1];
      if (is_el(n, name)) return true;
      if (scope_boundary(n, s)) return false;
    }
    return false;
  }
  bool node_in_scope(const Node* target, Scope s = Scope::Default) const {
    for (uint64_t i = open_.count(); i > 0; --i) {
      if (open_[i - 1] == target) return true;
      if (scope_boundary(open_[i - 1], s)) return false;
    }
    return false;
  }
  bool heading_in_scope() const {
    for (uint64_t i = open_.count(); i > 0; --i) {
      const Node* n = open_[i - 1];
      if (is_html(n) && name_in(n->name.view(), {"h1", "h2", "h3", "h4", "h5", "h6"})) return true;
      if (scope_boundary(n, Scope::Default)) return false;
    }
    return false;
  }
  bool on_stack(const Node* n) const {
    for (uint64_t i = 0; i < open_.count(); ++i)
      if (open_[i] == n) return true;
    return false;
  }
  bool template_on_stack() const {
    for (uint64_t i = 0; i < open_.count(); ++i)
      if (is_el(open_[i], "template")) return true;
    return false;
  }
  int64_t stack_index(const Node* n) const {
    for (uint64_t i = open_.count(); i > 0; --i)
      if (open_[i - 1] == n) return static_cast<int64_t>(i - 1);
    return -1;
  }
  void stack_remove(const Node* n) {
    const int64_t at = stack_index(n);
    if (at >= 0) open_.remove_at(static_cast<uint64_t>(at));
  }
  void pop() {
    if (open_.count()) open_.remove_at(open_.count() - 1);
  }
  bool push(Node* n) {
    if (open_.count() >= kMaxDepth) return false;
    if (!open_.push_back(n)) {
      failed_ = true;
      return false;
    }
    return true;
  }
  void pop_until(StringView name) {
    while (open_.count()) {
      Node* n = cur();
      pop();
      if (is_el(n, name)) return;
    }
  }
  void pop_until_one_of(std::initializer_list<const char*> names) {
    while (open_.count()) {
      Node* n = cur();
      pop();
      if (is_html(n) && name_in(n->name.view(), names)) return;
    }
  }
  void pop_until_node(const Node* target) {
    while (open_.count()) {
      Node* n = cur();
      pop();
      if (n == target) return;
    }
  }
  void generate_implied(StringView except = StringView()) {
    while (cur() && is_html(cur()) && name_in(cur()->name.view(), {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"}) && !(!except.empty() && cur()->name.view().equals(except))) pop();
  }
  void generate_implied_thoroughly() {
    while (cur() && is_html(cur()) && name_in(cur()->name.view(), {"caption", "colgroup", "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc", "tbody", "td", "tfoot", "th", "thead", "tr"})) pop();
  }
  void close_p() {
    generate_implied("p");
    pop_until("p");
  }
  void close_p_if_in_button_scope() {
    if (in_scope("p", Scope::Button)) close_p();
  }

  // --- Where a node goes
  struct Place {
    Node* parent = nullptr;
    Node* before = nullptr;
  };
  Place appropriate_place(Node* override_target = nullptr) {
    Node* target = override_target ? override_target : cur();
    if (!target) target = fragment_ ? fragment_ : doc_.root();
    Place p;
    p.parent = target;
    if (foster_ && target->is_element() && is_html(target) && name_in(target->name.view(), {"table", "tbody", "tfoot", "thead", "tr"})) {
      int64_t last_template = -1, last_table = -1;
      for (uint64_t i = open_.count(); i > 0; --i) {
        if (last_template < 0 && is_el(open_[i - 1], "template")) last_template = static_cast<int64_t>(i - 1);
        if (last_table < 0 && is_el(open_[i - 1], "table")) last_table = static_cast<int64_t>(i - 1);
      }
      if (last_template >= 0 && (last_table < 0 || last_template > last_table)) {
        p.parent = open_[static_cast<uint64_t>(last_template)];
      } else if (last_table < 0) {
        p.parent = open_[0];
      } else if (open_[static_cast<uint64_t>(last_table)]->parent) {
        p.parent = open_[static_cast<uint64_t>(last_table)]->parent;
        p.before = open_[static_cast<uint64_t>(last_table)];
      } else {
        p.parent = last_table > 0 ? open_[static_cast<uint64_t>(last_table) - 1] : open_[0];
      }
    }
    if (is_el(p.parent, "template")) {
      Node* content = template_content(p.parent);
      if (content) p.parent = content;
    }
    return p;
  }
  void put(Node* parent, Node* before, Node* n) {
    if (!n) return;
    if (insert_before(parent, n, before) != DomError::None) failed_ = true;
  }

  Node* make_element(StringView name) {
    Node* n = doc_.create_element(name);
    if (!n) (doc_.full() ? stopped_ : failed_) = true;   // (a page too big for the node limit is cut off where it stands)
    else if (name.equals("script")) n->script_started = true;   // (the parser runs the script itself, or never: not the host when it is put in the tree)
    return n;
  }

  static const char* svg_camel_tag(StringView t) {
    static const char* const kCamel[] = {"altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion", "animateTransform", "clipPath", "feBlend", "feColorMatrix",
                                         "feComponentTransfer", "feComposite", "feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap", "feDistantLight", "feDropShadow",
                                         "feFlood", "feFuncA", "feFuncB", "feFuncG", "feFuncR", "feGaussianBlur", "feImage", "feMerge", "feMergeNode", "feMorphology", "feOffset",
                                         "fePointLight", "feSpecularLighting", "feSpotLight", "feTile", "feTurbulence", "foreignObject", "glyphRef", "linearGradient",
                                         "radialGradient", "textPath"};
    for (const char* c : kCamel)
      if (t.equals_ignore_case(c)) return c;
    return nullptr;
  }
  static const char* svg_camel_attr(StringView t) {
    static const char* const kAttrs[] = {"attributeName", "attributeType", "baseFrequency", "baseProfile", "calcMode", "clipPathUnits", "diffuseConstant", "edgeMode", "filterUnits",
                                         "glyphRef", "gradientTransform", "gradientUnits", "kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines", "keyTimes", "lengthAdjust",
                                         "limitingConeAngle", "markerHeight", "markerUnits", "markerWidth", "maskContentUnits", "maskUnits", "numOctaves", "pathLength",
                                         "patternContentUnits", "patternTransform", "patternUnits", "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha", "preserveAspectRatio",
                                         "primitiveUnits", "refX", "refY", "repeatCount", "repeatDur", "requiredExtensions", "requiredFeatures", "specularConstant",
                                         "specularExponent", "spreadMethod", "startOffset", "stdDeviation", "stitchTiles", "surfaceScale", "systemLanguage", "tableValues", "targetX",
                                         "targetY", "textLength", "viewBox", "viewTarget", "xChannelSelector", "yChannelSelector", "zoomAndPan"};
    for (const char* c : kAttrs)
      if (t.equals_ignore_case(c)) return c;
    return nullptr;
  }

  // An element for a tag token. `ns` is null for HTML.
  Node* create_for(const Token& t, const char* ns) {
    Node* el = make_element(t.name.view());
    if (!el) return nullptr;
    if (ns) {
      el->ns_set = true;
      el->ns.assign(ns);
      if (ns == kSvgNs)
        if (const char* camel = svg_camel_tag(t.name.view())) el->qname.assign(camel);
    }
    for (uint64_t i = 0; i < t.attrs.count(); ++i) {
      const Attr& a = t.attrs[i];
      if (a.dropped) continue;
      add_attribute(el, a.name.view(), a.value.view(), ns);
    }
    return el;
  }
  void add_attribute(Node* el, StringView name, StringView value, const char* ns) {
    if (has_attribute(el, name)) return;
    const char* shown = nullptr;
    if (ns == kSvgNs) shown = svg_camel_attr(name);
    else if (ns == kMathNs && name.equals("definitionurl")) shown = "definitionURL";
    if (!shown) {
      if (set_attribute(el, name, value) != DomError::None) failed_ = true;
      return;
    }
    Attribute attr(doc_.allocator());
    attr.name.assign(shown);
    attr.value.assign(value);
    if (!el->attrs.push_back(util::move(attr))) failed_ = true;
  }
  Node* clone_for_formatting(const Node* src) {
    Node* el = make_element(src->name.view());
    if (!el) return nullptr;
    el->ns_set = src->ns_set;
    el->ns.assign(src->ns.view());
    el->qname.assign(src->qname.view());
    for (uint64_t i = 0; i < src->attrs.count(); ++i) {
      Attribute copy(doc_.allocator());
      copy.name.assign(src->attrs[i].name.view());
      copy.value.assign(src->attrs[i].value.view());
      if (!el->attrs.push_back(util::move(copy))) failed_ = true;
    }
    return el;
  }

  Node* insert_element(const Token& t, const char* ns = nullptr, bool push_it = true) {
    Node* el = create_for(t, ns);
    if (!el) return nullptr;
    const Place p = appropriate_place();
    put(p.parent, p.before, el);
    if (push_it) push(el);
    return el;
  }
  Node* insert_html(const Token& t) { return insert_element(t, nullptr, true); }
  void insert_void(Token& t) {
    insert_element(t, nullptr, false);
    t.acknowledged = true;
  }
  void t_ack(Token& t) { t.acknowledged = true; }

  void insert_comment(const Token& t, Node* target = nullptr) {
    Node* c = doc_.create_comment(t.data.view());
    if (!c) {
      (doc_.full() ? stopped_ : failed_) = true;
      return;
    }
    if (target) put(target, nullptr, c);
    else {
      const Place p = appropriate_place();
      put(p.parent, p.before, c);
    }
  }
  void insert_text(StringView s) {
    if (s.empty()) return;
    const Place p = appropriate_place();
    if (!p.parent || p.parent->type == NodeType::Document) return;
    Node* prev = p.before ? p.before->prev : p.parent->last_child;
    if (prev && prev->is_text()) {
      prev->data.append(s);
      doc_.touch();
      return;
    }
    Node* t = doc_.create_text(s);
    if (!t) {
      (doc_.full() ? stopped_ : failed_) = true;
      return;
    }
    put(p.parent, p.before, t);
  }

  // --- Active formatting elements (nullptr is a marker)
  bool same_attrs(const Node* a, const Node* b) const {
    if (a->attrs.count() != b->attrs.count()) return false;
    for (uint64_t i = 0; i < a->attrs.count(); ++i) {
      StringView v;
      if (!get_attribute(b, a->attrs[i].name.view(), &v) || !v.equals(a->attrs[i].value.view())) return false;
    }
    return true;
  }
  void push_afe(Node* el) {
    uint32_t same = 0;
    int64_t earliest = -1;
    for (uint64_t i = afe_.count(); i > 0; --i) {
      Node* n = afe_[i - 1];
      if (!n) break;
      if (n->name.view().equals(el->name.view()) && n->ns.view().equals(el->ns.view()) && same_attrs(n, el)) {
        ++same;
        earliest = static_cast<int64_t>(i - 1);
      }
    }
    if (same >= 3 && earliest >= 0) afe_.remove_at(static_cast<uint64_t>(earliest));
    afe_.push_back(el);
  }
  int64_t afe_index(const Node* n) const {
    for (uint64_t i = afe_.count(); i > 0; --i)
      if (afe_[i - 1] == n) return static_cast<int64_t>(i - 1);
    return -1;
  }
  void afe_remove(const Node* n) {
    const int64_t at = afe_index(n);
    if (at >= 0) afe_.remove_at(static_cast<uint64_t>(at));
  }
  void afe_marker() { afe_.push_back(nullptr); }
  void afe_clear_to_marker() {
    while (afe_.count()) {
      Node* n = afe_[afe_.count() - 1];
      afe_.remove_at(afe_.count() - 1);
      if (!n) return;
    }
  }
  void reconstruct_afe() {
    if (afe_.count() == 0) return;
    uint64_t i = afe_.count() - 1;
    if (!afe_[i] || on_stack(afe_[i])) return;
    while (i > 0) {
      --i;
      if (!afe_[i] || on_stack(afe_[i])) {
        ++i;
        break;
      }
    }
    for (; i < afe_.count(); ++i) {
      Node* el = clone_for_formatting(afe_[i]);
      if (!el) return;
      const Place p = appropriate_place();
      put(p.parent, p.before, el);
      push(el);
      afe_[i] = el;
    }
  }

  // The adoption agency algorithm for an end tag (or the start of an <a> or <nobr>) named `subject`. False if the "any other end tag" steps apply.
  bool adoption_agency(StringView subject) {
    if (cur() && is_html(cur()) && cur()->name.view().equals(subject) && afe_index(cur()) < 0) {
      pop();
      return true;
    }
    for (int outer = 0; outer < 8; ++outer) {
      Node* formatting = nullptr;
      for (uint64_t i = afe_.count(); i > 0; --i) {
        if (!afe_[i - 1]) break;
        if (is_html(afe_[i - 1]) && afe_[i - 1]->name.view().equals(subject)) {
          formatting = afe_[i - 1];
          break;
        }
      }
      if (!formatting) return false;
      const int64_t fi = stack_index(formatting);
      if (fi < 0) {
        afe_remove(formatting);
        return true;
      }
      if (!node_in_scope(formatting)) return true;
      Node* furthest = nullptr;
      for (uint64_t i = static_cast<uint64_t>(fi) + 1; i < open_.count(); ++i)
        if (is_special(open_[i])) {
          furthest = open_[i];
          break;
        }
      if (!furthest) {
        while (open_.count() && cur() != formatting) pop();
        pop();
        afe_remove(formatting);
        return true;
      }
      Node* common = open_[static_cast<uint64_t>(fi) - 1];
      int64_t bookmark = afe_index(formatting);
      Node* last = furthest;
      Node* node = furthest;
      int64_t node_index = stack_index(node);
      for (int inner = 1;; ++inner) {
        // the element above `node` in the stack (it may have been removed: use the position it had)
        node_index = node_index - 1;
        if (node_index < 0) break;
        node = open_[static_cast<uint64_t>(node_index)];
        if (node == formatting) break;
        if (inner > 3 && afe_index(node) >= 0) {
          const int64_t at = afe_index(node);
          afe_.remove_at(static_cast<uint64_t>(at));
          if (at < bookmark) --bookmark;
        }
        if (afe_index(node) < 0) {
          open_.remove_at(static_cast<uint64_t>(node_index));
          continue;
        }
        Node* fresh = clone_for_formatting(node);
        if (!fresh) return true;
        afe_[static_cast<uint64_t>(afe_index(node))] = fresh;
        open_[static_cast<uint64_t>(node_index)] = fresh;
        node = fresh;
        if (last == furthest) bookmark = afe_index(node) + 1;
        detach(last);
        put(node, nullptr, last);
        last = node;
      }
      detach(last);
      const Place p = appropriate_place(common);
      put(p.parent, p.before, last);
      Node* fresh = clone_for_formatting(formatting);
      if (!fresh) return true;
      while (furthest->first_child) {
        Node* c = furthest->first_child;
        detach(c);
        put(fresh, nullptr, c);
      }
      put(furthest, nullptr, fresh);
      const int64_t old_at = afe_index(formatting);
      afe_.remove_at(static_cast<uint64_t>(old_at));
      if (old_at < bookmark) --bookmark;
      if (bookmark > static_cast<int64_t>(afe_.count())) bookmark = static_cast<int64_t>(afe_.count());
      afe_.insert(static_cast<uint64_t>(bookmark), fresh);
      stack_remove(formatting);
      const int64_t fa = stack_index(furthest);
      open_.insert(static_cast<uint64_t>(fa) + 1, fresh);
    }
    return true;
  }

  // ======================================================================== insertion modes
  void reset_mode() {
    for (uint64_t i = open_.count(); i > 0; --i) {
      Node* n = open_[i - 1];
      const bool last = i == 1;
      if (last && fragment_) n = context_;
      if (!n || !n->is_element()) continue;
      if (!is_html(n)) {
        if (last) {
          mode_ = Mode::InBody;
          return;
        }
        continue;
      }
      const StringView t = n->name.view();
      if (t.equals("select")) {
        mode_ = Mode::InSelect;
        for (uint64_t j = i - 1; j > 0 && !last; --j) {
          Node* a = open_[j - 1];
          if (is_el(a, "template")) break;
          if (is_el(a, "table")) {
            mode_ = Mode::InSelectInTable;
            break;
          }
        }
        return;
      }
      if ((t.equals("td") || t.equals("th")) && !last) { mode_ = Mode::InCell; return; }
      if (t.equals("tr")) { mode_ = Mode::InRow; return; }
      if (name_in(t, {"tbody", "thead", "tfoot"})) { mode_ = Mode::InTableBody; return; }
      if (t.equals("caption")) { mode_ = Mode::InCaption; return; }
      if (t.equals("colgroup")) { mode_ = Mode::InColumnGroup; return; }
      if (t.equals("table")) { mode_ = Mode::InTable; return; }
      if (t.equals("template")) { mode_ = tmpl_modes_.count() ? tmpl_modes_[tmpl_modes_.count() - 1] : Mode::InTemplate; return; }
      if (t.equals("head") && !last) { mode_ = Mode::InHead; return; }
      if (t.equals("body")) { mode_ = Mode::InBody; return; }
      if (t.equals("frameset")) { mode_ = Mode::InFrameset; return; }
      if (t.equals("html")) { mode_ = head_ ? Mode::AfterHead : Mode::BeforeHead; return; }
      if (last) {
        mode_ = Mode::InBody;
        return;
      }
    }
    mode_ = Mode::InBody;
  }

  void setup_fragment() {
    Node* root = make_element("html");
    if (!root) return;
    root_ = root;
    push(root);
    if (is_el(context_, "template")) tmpl_modes_.push_back(Mode::InTemplate);
    reset_mode();
    if (is_el(context_, "form")) form_ = context_;
    else
      for (Node* a = context_; a; a = a->parent)
        if (is_el(a, "form")) {
          form_ = a;
          break;
        }
    // the tokenizer starts in the state the context asks for
    if (is_html(context_)) {
      const StringView t = context_->name.view();
      if (name_in(t, {"title", "textarea"})) state_ = State::Rcdata;
      else if (name_in(t, {"style", "xmp", "iframe", "noembed", "noframes"})) state_ = State::Rawtext;
      else if (t.equals("script")) state_ = State::ScriptData;
      else if (t.equals("noscript") && scripting_) state_ = State::Rawtext;
      else if (t.equals("plaintext")) state_ = State::Plaintext;
    }
  }
  void finish_fragment() {
    if (!root_) return;
    while (root_->first_child) {
      Node* c = root_->first_child;
      detach(c);
      put(fragment_, nullptr, c);
    }
  }

  void parse() {
    if (fragment_ && !root_) return;
    for (;;) {
      if (failed_ || stopped_) return;
      Token* t = (held_ || !eof_next_) ? next_token() : nullptr;
      if (!t) {
        eof_next_ = false;
        tok_b_.reset();
        tok_b_.type = Token::Eof;
        t = &tok_b_;
      }
      if (skip_lf_) {
        skip_lf_ = false;
        if (t->type == Token::Characters && !t->data.empty() && t->data[0] == '\n') {
          t->data.erase(0, 1);
          if (t->data.empty()) continue;
        }
      }
      if (t->type == Token::Characters) characters(t->data.view());
      else process(*t);
      if (t->type == Token::Eof) {
        flush_table_text();
        return;
      }
    }
  }

  // --- The tree construction dispatcher
  bool foreign_dispatch(Token::Type type, StringView name) const {
    const Node* node = adjusted_current();
    if (!node || is_html(node) || type == Token::Eof) return false;
    if (is_mathml_text_ip(node)) {
      if (type == Token::StartTag && !name.equals("mglyph") && !name.equals("malignmark")) return false;
      if (type == Token::Characters) return false;
    }
    if (in_ns(node, kMathNs) && node->name.view().equals("annotation-xml") && type == Token::StartTag && name.equals("svg")) return false;
    if (is_html_ip(node) && (type == Token::StartTag || type == Token::Characters)) return false;
    return true;
  }

  void characters(StringView s) {
    // runs of text go through the dispatcher one byte at a time only when the mode needs it
    if (!foreign_chars(s)) {
      for (uint64_t i = 0; i < s.size(); ++i) {
        if (failed_ || stopped_) return;
        char_in_mode(s[i]);
      }
    }
  }
  bool foreign_chars(StringView s) {
    if (!foreign_dispatch(Token::Characters, StringView())) return false;
    for (uint64_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c == '\0') insert_text("\xEF\xBF\xBD");
      else {
        insert_text(StringView(&c, 1));
        if (!is_space(c)) frameset_ok_ = false;
      }
    }
    return true;
  }

  void process(Token& t) {
    if (stopped_ || failed_) return;
    if (foreign_dispatch(t.type, t.name.view())) {
      foreign(t);
      return;
    }
    process_in(mode_, t);
  }

  // ======================================================================== characters by mode
  void char_in_body(char c) {
    if (c == '\0') return;
    reconstruct_afe();
    insert_text(StringView(&c, 1));
    if (!is_space(c)) frameset_ok_ = false;
  }
  void char_in_mode(char c) { char_in(mode_, c); }
  void char_in(Mode m, char c) {
    switch (m) {
      case Mode::Initial:
        if (is_space(c)) return;
        mode_ = Mode::BeforeHtml;
        return char_in(mode_, c);
      case Mode::BeforeHtml:
        if (is_space(c)) return;
        make_html_root();
        mode_ = Mode::BeforeHead;
        return char_in(mode_, c);
      case Mode::BeforeHead:
        if (is_space(c)) return;
        synth_head();
        return char_in(mode_, c);
      case Mode::InHead:
        if (is_space(c)) return insert_text(StringView(&c, 1));
        pop();
        mode_ = Mode::AfterHead;
        return char_in(mode_, c);
      case Mode::InHeadNoscript:
        if (is_space(c)) return insert_text(StringView(&c, 1));
        pop();
        mode_ = Mode::InHead;
        return char_in(mode_, c);
      case Mode::AfterHead:
        if (is_space(c)) return insert_text(StringView(&c, 1));
        synth_body();
        return char_in(mode_, c);
      case Mode::InBody: case Mode::InCaption: case Mode::InCell: case Mode::InTemplate:
        return char_in_body(c);
      case Mode::Text:
        return insert_text(StringView(&c, 1));
      case Mode::InTable: case Mode::InTableBody: case Mode::InRow:
        if (cur() && is_html(cur()) && name_in(cur()->name.view(), {"table", "tbody", "tfoot", "thead", "tr"})) {
          table_text_.clear();
          table_text_non_space_ = false;
          original_mode_ = mode_;
          mode_ = Mode::InTableText;
          return char_in(mode_, c);
        }
        {   // anything else: as in the body, with foster parenting
          const bool saved = foster_;
          foster_ = true;
          char_in_body(c);
          foster_ = saved;
        }
        return;
      case Mode::InTableText:
        if (c == '\0') return;
        table_text_.push_back(c);
        if (!is_space(c)) table_text_non_space_ = true;
        return;
      case Mode::InColumnGroup:
        if (is_space(c)) return insert_text(StringView(&c, 1));
        if (!cur_is("colgroup")) return;
        pop();
        mode_ = Mode::InTable;
        return char_in(mode_, c);
      case Mode::InSelect: case Mode::InSelectInTable:
        if (c == '\0') return;
        return insert_text(StringView(&c, 1));
      case Mode::AfterBody:
        if (is_space(c)) return char_in_body(c);
        mode_ = Mode::InBody;
        return char_in(mode_, c);
      case Mode::InFrameset: case Mode::AfterFrameset:
        if (is_space(c)) insert_text(StringView(&c, 1));
        return;
      case Mode::AfterAfterBody:
        if (is_space(c)) return char_in_body(c);
        mode_ = Mode::InBody;
        return char_in(mode_, c);
      case Mode::AfterAfterFrameset:
        if (is_space(c)) char_in_body(c);
        return;
    }
  }
  void flush_table_text() {
    if (mode_ != Mode::InTableText) return;
    mode_ = original_mode_;
    if (table_text_non_space_) {
      const bool saved = foster_;
      foster_ = true;
      for (uint64_t i = 0; i < table_text_.size(); ++i) char_in_body(table_text_[i]);
      foster_ = saved;
    } else {
      insert_text(table_text_.view());
    }
    table_text_.clear();
  }

  // --- Synthesized elements
  void make_html_root() {
    Node* html = make_element("html");
    if (!html) return;
    put(doc_.root(), nullptr, html);
    push(html);
  }
  void synth(const char* name, Mode next) {
    Token t(a_);
    t.type = Token::StartTag;
    t.name.assign(name);
    Node* el = insert_html(t);
    (void)el;
    mode_ = next;
  }
  void synth_head() {
    Token t(a_);
    t.type = Token::StartTag;
    t.name.assign("head");
    head_ = insert_html(t);
    mode_ = Mode::InHead;
  }
  void synth_body() {
    synth("body", Mode::InBody);
  }

  void merge_attributes(Node* target, const Token& t) {
    for (uint64_t i = 0; i < t.attrs.count(); ++i)
      if (!t.attrs[i].dropped) add_attribute(target, t.attrs[i].name.view(), t.attrs[i].value.view(), nullptr);
  }

  void raw_text(const Token& t, State state) {
    insert_html(t);
    state_ = state;
    original_mode_ = mode_;
    mode_ = Mode::Text;
  }

  // ======================================================================== modes for tags, comments, doctypes and the end of the input
  void process_in(Mode m, Token& t) {
    switch (m) {
      case Mode::Initial: return mode_initial(t);
      case Mode::BeforeHtml: return mode_before_html(t);
      case Mode::BeforeHead: return mode_before_head(t);
      case Mode::InHead: return mode_in_head(t);
      case Mode::InHeadNoscript: return mode_in_head_noscript(t);
      case Mode::AfterHead: return mode_after_head(t);
      case Mode::InBody: return mode_in_body(t);
      case Mode::Text: return mode_text(t);
      case Mode::InTable: return mode_in_table(t);
      case Mode::InTableText: flush_table_text(); return process_in(mode_, t);
      case Mode::InCaption: return mode_in_caption(t);
      case Mode::InColumnGroup: return mode_in_column_group(t);
      case Mode::InTableBody: return mode_in_table_body(t);
      case Mode::InRow: return mode_in_row(t);
      case Mode::InCell: return mode_in_cell(t);
      case Mode::InSelect: return mode_in_select(t);
      case Mode::InSelectInTable: return mode_in_select_in_table(t);
      case Mode::InTemplate: return mode_in_template(t);
      case Mode::AfterBody: return mode_after_body(t);
      case Mode::InFrameset: return mode_in_frameset(t);
      case Mode::AfterFrameset: return mode_after_frameset(t);
      case Mode::AfterAfterBody: return mode_after_after_body(t);
      case Mode::AfterAfterFrameset: return mode_after_after_frameset(t);
    }
  }
  bool start(const Token& t, const char* name) const { return t.type == Token::StartTag && t.name.view().equals(name); }
  bool end(const Token& t, const char* name) const { return t.type == Token::EndTag && t.name.view().equals(name); }
  bool start_in(const Token& t, std::initializer_list<const char*> names) const { return t.type == Token::StartTag && name_in(t.name.view(), names); }
  bool end_in(const Token& t, std::initializer_list<const char*> names) const { return t.type == Token::EndTag && name_in(t.name.view(), names); }

  void set_quirks(const Token& t) {
    StringView pub = t.has_pub ? t.pub.view() : StringView();
    StringView sys = t.has_sys ? t.sys.view() : StringView();
    bool quirks = t.force_quirks || !t.name.view().equals("html") || quirky_public_id(pub) ||
                  (t.has_sys && sys.equals_ignore_case("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"));
    if (!quirks && !t.has_sys && (starts_with_ci(pub, "-//w3c//dtd html 4.01 frameset//") || starts_with_ci(pub, "-//w3c//dtd html 4.01 transitional//"))) quirks = true;
    doc_.quirks_mode = quirks;
  }

  void mode_initial(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t, doc_.root());
    if (t.type == Token::Doctype) {
      set_quirks(t);
      mode_ = Mode::BeforeHtml;
      return;
    }
    doc_.quirks_mode = true;
    mode_ = Mode::BeforeHtml;
    process_in(mode_, t);
  }
  void mode_before_html(Token& t) {
    if (t.type == Token::Doctype) return;
    if (t.type == Token::Comment) return insert_comment(t, doc_.root());
    if (start(t, "html")) {
      Node* html = create_for(t, nullptr);
      if (!html) return;
      put(doc_.root(), nullptr, html);
      push(html);
      mode_ = Mode::BeforeHead;
      return;
    }
    if (t.type == Token::EndTag && !end_in(t, {"head", "body", "html", "br"})) return;
    make_html_root();
    mode_ = Mode::BeforeHead;
    process_in(mode_, t);
  }
  void mode_before_head(Token& t) {
    if (t.type == Token::Doctype) return;
    if (t.type == Token::Comment) return insert_comment(t);
    if (start(t, "html")) return mode_in_body(t);
    if (start(t, "head")) {
      head_ = insert_html(t);
      mode_ = Mode::InHead;
      return;
    }
    if (t.type == Token::EndTag && !end_in(t, {"head", "body", "html", "br"})) return;
    synth_head();
    process_in(mode_, t);
  }
  void mode_in_head(Token& t) {
    if (t.type == Token::Doctype) return;
    if (t.type == Token::Comment) return insert_comment(t);
    if (start(t, "html")) return mode_in_body(t);
    if (start_in(t, {"base", "basefont", "bgsound", "link"})) return insert_void(t);
    if (start(t, "meta")) return insert_void(t);
    if (start(t, "title")) return raw_text(t, State::Rcdata);
    if (start(t, "noscript") && scripting_) return raw_text(t, State::Rawtext);
    if (start_in(t, {"noframes", "style"})) return raw_text(t, State::Rawtext);
    if (start(t, "noscript")) {
      insert_html(t);
      mode_ = Mode::InHeadNoscript;
      return;
    }
    if (start(t, "script")) {
      const Place p = appropriate_place();
      Node* el = create_for(t, nullptr);
      if (!el) return;
      put(p.parent, p.before, el);
      push(el);
      state_ = State::ScriptData;
      original_mode_ = mode_;
      mode_ = Mode::Text;
      return;
    }
    if (end(t, "head")) {
      pop();
      mode_ = Mode::AfterHead;
      return;
    }
    if (start(t, "template")) {
      Node* el = insert_html(t);
      if (el) template_content(el);
      afe_marker();
      frameset_ok_ = false;
      mode_ = Mode::InTemplate;
      tmpl_modes_.push_back(Mode::InTemplate);
      return;
    }
    if (end(t, "template")) {
      if (!template_on_stack()) return;
      generate_implied_thoroughly();
      pop_until("template");
      afe_clear_to_marker();
      if (tmpl_modes_.count()) tmpl_modes_.remove_at(tmpl_modes_.count() - 1);
      reset_mode();
      return;
    }
    if (start(t, "head")) return;
    if (t.type == Token::EndTag && !end(t, "br")) return;
    pop();
    mode_ = Mode::AfterHead;
    process_in(mode_, t);
  }
  void mode_in_head_noscript(Token& t) {
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (end(t, "noscript")) {
      pop();
      mode_ = Mode::InHead;
      return;
    }
    if (t.type == Token::Comment || start_in(t, {"basefont", "bgsound", "link", "meta", "noframes", "style"})) return mode_in_head(t);
    if (start_in(t, {"head", "noscript"})) return;
    if (t.type == Token::EndTag && !end(t, "br")) return;
    pop();
    mode_ = Mode::InHead;
    process_in(mode_, t);
  }
  void mode_after_head(Token& t) {
    if (t.type == Token::Doctype) return;
    if (t.type == Token::Comment) return insert_comment(t);
    if (start(t, "html")) return mode_in_body(t);
    if (start(t, "body")) {
      insert_html(t);
      frameset_ok_ = false;
      mode_ = Mode::InBody;
      return;
    }
    if (start(t, "frameset")) {
      insert_html(t);
      mode_ = Mode::InFrameset;
      return;
    }
    if (start_in(t, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
      // processed using the rules for "in head", with the head element pointer back on the stack for a moment
      if (head_) push(head_);
      mode_in_head(t);
      if (head_) stack_remove(head_);
      return;
    }
    if (end(t, "template")) return mode_in_head(t);
    if (start(t, "head")) return;
    if (t.type == Token::EndTag && !end_in(t, {"body", "html", "br"})) return;
    synth_body();
    frameset_ok_ = true;
    process_in(mode_, t);
  }

  // --- The body
  void any_other_end_tag(const Token& t) {
    for (uint64_t i = open_.count(); i > 0; --i) {
      Node* n = open_[i - 1];
      if (is_html(n) && n->name.view().equals(t.name.view())) {
        generate_implied(t.name.view());
        pop_until_node(n);
        return;
      }
      if (is_special(n)) return;
    }
  }

  void mode_in_body(Token& t) {
    switch (t.type) {
      case Token::Comment: return insert_comment(t);
      case Token::Doctype: return;
      case Token::Eof:
        if (tmpl_modes_.count()) return mode_in_template(t);
        stopped_ = true;
        return;
      case Token::StartTag: return body_start(t);
      case Token::EndTag: return body_end(t);
      default: return;
    }
  }

  void body_start(Token& t) {
    const StringView n = t.name.view();
    if (n.equals("html")) {
      if (template_on_stack() || open_.count() == 0) return;
      merge_attributes(open_[0], t);
      return;
    }
    if (name_in(n, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) return mode_in_head(t);
    if (n.equals("body")) {
      if (open_.count() < 2 || !is_el(open_[1], "body") || template_on_stack()) return;
      frameset_ok_ = false;
      merge_attributes(open_[1], t);
      return;
    }
    if (n.equals("frameset")) {
      if (open_.count() < 2 || !is_el(open_[1], "body") || !frameset_ok_) return;
      Node* body = open_[1];
      if (body->parent) detach(body);
      while (open_.count() > 1) pop();
      insert_html(t);
      mode_ = Mode::InFrameset;
      return;
    }
    if (name_in(n, {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header",
                    "hgroup", "main", "menu", "nav", "ol", "p", "search", "section", "summary", "ul"})) {
      close_p_if_in_button_scope();
      insert_html(t);
      return;
    }
    if (name_in(n, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
      close_p_if_in_button_scope();
      if (cur() && is_html(cur()) && name_in(cur()->name.view(), {"h1", "h2", "h3", "h4", "h5", "h6"})) pop();
      insert_html(t);
      return;
    }
    if (name_in(n, {"pre", "listing"})) {
      close_p_if_in_button_scope();
      insert_html(t);
      skip_lf_ = true;
      frameset_ok_ = false;
      return;
    }
    if (n.equals("form")) {
      if (form_ && !template_on_stack()) return;
      close_p_if_in_button_scope();
      Node* el = insert_html(t);
      if (!template_on_stack()) form_ = el;
      return;
    }
    if (n.equals("li")) {
      frameset_ok_ = false;
      for (uint64_t i = open_.count(); i > 0; --i) {
        Node* node = open_[i - 1];
        if (is_el(node, "li")) {
          generate_implied("li");
          pop_until("li");
          break;
        }
        if (is_special(node) && !(is_html(node) && name_in(node->name.view(), {"address", "div", "p"}))) break;
      }
      close_p_if_in_button_scope();
      insert_html(t);
      return;
    }
    if (name_in(n, {"dd", "dt"})) {
      frameset_ok_ = false;
      for (uint64_t i = open_.count(); i > 0; --i) {
        Node* node = open_[i - 1];
        if (is_el(node, "dd") || is_el(node, "dt")) {
          const char* which = is_el(node, "dd") ? "dd" : "dt";
          generate_implied(which);
          pop_until(which);
          break;
        }
        if (is_special(node) && !(is_html(node) && name_in(node->name.view(), {"address", "div", "p"}))) break;
      }
      close_p_if_in_button_scope();
      insert_html(t);
      return;
    }
    if (n.equals("plaintext")) {
      close_p_if_in_button_scope();
      insert_html(t);
      state_ = State::Plaintext;
      return;
    }
    if (n.equals("button")) {
      if (in_scope("button")) {
        generate_implied();
        pop_until("button");
      }
      reconstruct_afe();
      insert_html(t);
      frameset_ok_ = false;
      return;
    }
    if (n.equals("a")) {
      for (uint64_t i = afe_.count(); i > 0; --i) {
        Node* e = afe_[i - 1];
        if (!e) break;
        if (is_el(e, "a")) {
          adoption_agency("a");
          afe_remove(e);
          stack_remove(e);
          break;
        }
      }
      reconstruct_afe();
      Node* el = insert_html(t);
      if (el) push_afe(el);
      return;
    }
    if (name_in(n, {"b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u"})) {
      reconstruct_afe();
      Node* el = insert_html(t);
      if (el) push_afe(el);
      return;
    }
    if (n.equals("nobr")) {
      reconstruct_afe();
      if (in_scope("nobr")) {
        adoption_agency("nobr");
        reconstruct_afe();
      }
      Node* el = insert_html(t);
      if (el) push_afe(el);
      return;
    }
    if (name_in(n, {"applet", "marquee", "object"})) {
      reconstruct_afe();
      insert_html(t);
      afe_marker();
      frameset_ok_ = false;
      return;
    }
    if (n.equals("table")) {
      if (!doc_.quirks_mode) close_p_if_in_button_scope();
      insert_html(t);
      frameset_ok_ = false;
      mode_ = Mode::InTable;
      return;
    }
    if (name_in(n, {"area", "br", "embed", "img", "keygen", "wbr"})) {
      reconstruct_afe();
      insert_void(t);
      frameset_ok_ = false;
      return;
    }
    if (n.equals("input")) {
      reconstruct_afe();
      insert_void(t);
      StringView type;
      if (!(t.attr("type", &type) && type.equals_ignore_case("hidden"))) frameset_ok_ = false;
      return;
    }
    if (name_in(n, {"param", "source", "track"})) return insert_void(t);
    if (n.equals("hr")) {
      close_p_if_in_button_scope();
      insert_void(t);
      frameset_ok_ = false;
      return;
    }
    if (n.equals("image")) {
      t.name.assign("img");
      return body_start(t);
    }
    if (n.equals("textarea")) {
      insert_html(t);
      skip_lf_ = true;
      state_ = State::Rcdata;
      original_mode_ = mode_;
      frameset_ok_ = false;
      mode_ = Mode::Text;
      return;
    }
    if (n.equals("xmp")) {
      close_p_if_in_button_scope();
      reconstruct_afe();
      frameset_ok_ = false;
      return raw_text(t, State::Rawtext);
    }
    if (n.equals("iframe")) {
      frameset_ok_ = false;
      return raw_text(t, State::Rawtext);
    }
    if (n.equals("noembed") || (n.equals("noscript") && scripting_)) return raw_text(t, State::Rawtext);
    if (n.equals("select")) {
      reconstruct_afe();
      insert_html(t);
      frameset_ok_ = false;
      mode_ = (mode_ == Mode::InTable || mode_ == Mode::InCaption || mode_ == Mode::InTableBody || mode_ == Mode::InRow || mode_ == Mode::InCell) ? Mode::InSelectInTable : Mode::InSelect;
      return;
    }
    if (name_in(n, {"optgroup", "option"})) {
      if (cur_is("option")) pop();
      reconstruct_afe();
      insert_html(t);
      return;
    }
    if (name_in(n, {"rb", "rtc"})) {
      if (in_scope("ruby")) generate_implied();
      insert_html(t);
      return;
    }
    if (name_in(n, {"rp", "rt"})) {
      if (in_scope("ruby")) generate_implied("rtc");
      insert_html(t);
      return;
    }
    if (n.equals("math") || n.equals("svg")) {
      reconstruct_afe();
      const char* ns = n.equals("math") ? kMathNs : kSvgNs;
      Node* el = insert_element(t, ns, !t.self_closing);
      if (t.self_closing) t_ack(t);
      (void)el;
      return;
    }
    if (name_in(n, {"caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
    reconstruct_afe();
    insert_html(t);
  }

  void body_end(Token& t) {
    const StringView n = t.name.view();
    if (n.equals("template")) return mode_in_head(t);
    if (n.equals("body")) {
      if (!in_scope("body")) return;
      mode_ = Mode::AfterBody;
      return;
    }
    if (n.equals("html")) {
      if (!in_scope("body")) return;
      mode_ = Mode::AfterBody;
      return process_in(mode_, t);
    }
    if (name_in(n, {"address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header",
                    "hgroup", "listing", "main", "menu", "nav", "ol", "pre", "search", "section", "summary", "ul"})) {
      if (!in_scope(t.name.view())) return;
      generate_implied();
      pop_until(t.name.view());
      return;
    }
    if (n.equals("form")) {
      if (!template_on_stack()) {
        Node* node = form_;
        form_ = nullptr;
        if (!node || !node_in_scope(node)) return;
        generate_implied();
        stack_remove(node);
      } else {
        if (!in_scope("form")) return;
        generate_implied();
        pop_until("form");
      }
      return;
    }
    if (n.equals("p")) {
      if (!in_scope("p", Scope::Button)) {
        Token s(a_);
        s.type = Token::StartTag;
        s.name.assign("p");
        insert_html(s);
      }
      close_p();
      return;
    }
    if (n.equals("li")) {
      if (!in_scope("li", Scope::ListItem)) return;
      generate_implied("li");
      pop_until("li");
      return;
    }
    if (name_in(n, {"dd", "dt"})) {
      if (!in_scope(t.name.view())) return;
      generate_implied(t.name.view());
      pop_until(t.name.view());
      return;
    }
    if (name_in(n, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
      if (!heading_in_scope()) return;
      generate_implied();
      pop_until_one_of({"h1", "h2", "h3", "h4", "h5", "h6"});
      return;
    }
    if (name_in(n, {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"})) {
      if (!adoption_agency(n)) any_other_end_tag(t);
      return;
    }
    if (name_in(n, {"applet", "marquee", "object"})) {
      if (!in_scope(t.name.view())) return;
      generate_implied();
      pop_until(t.name.view());
      afe_clear_to_marker();
      return;
    }
    if (n.equals("br")) {
      t.type = Token::StartTag;
      t.attrs.clear();
      t.self_closing = false;
      return body_start(t);
    }
    any_other_end_tag(t);
  }

  // --- Text
  void mode_text(Token& t) {
    if (t.type == Token::Eof) {
      if (cur_is("script")) cur()->script_started = true;
      pop();
      mode_ = original_mode_;
      return process_in(mode_, t);
    }
    if (end(t, "script")) {
      Node* script = cur();
      pop();
      mode_ = original_mode_;
      if (script && is_el(script, "script")) run_script(script);
      return;
    }
    pop();
    mode_ = original_mode_;
  }

  // --- Tables
  void clear_to_context(std::initializer_list<const char*> names) {
    while (cur() && !(is_html(cur()) && name_in(cur()->name.view(), names)) && !is_el(cur(), "template") && !is_el(cur(), "html")) pop();
  }
  void in_table_foster(Token& t) {
    const bool saved = foster_;
    foster_ = true;
    mode_in_body(t);
    foster_ = saved;
  }
  void mode_in_table(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t);
    if (t.type == Token::Doctype) return;
    if (start(t, "caption")) {
      clear_to_context({"table"});
      afe_marker();
      insert_html(t);
      mode_ = Mode::InCaption;
      return;
    }
    if (start(t, "colgroup")) {
      clear_to_context({"table"});
      insert_html(t);
      mode_ = Mode::InColumnGroup;
      return;
    }
    if (start(t, "col")) {
      clear_to_context({"table"});
      synth("colgroup", Mode::InColumnGroup);
      return process_in(mode_, t);
    }
    if (start_in(t, {"tbody", "tfoot", "thead"})) {
      clear_to_context({"table"});
      insert_html(t);
      mode_ = Mode::InTableBody;
      return;
    }
    if (start_in(t, {"td", "th", "tr"})) {
      clear_to_context({"table"});
      synth("tbody", Mode::InTableBody);
      return process_in(mode_, t);
    }
    if (start(t, "table")) {
      if (!in_scope("table", Scope::Table)) return;
      pop_until("table");
      reset_mode();
      return process_in(mode_, t);
    }
    if (end(t, "table")) {
      if (!in_scope("table", Scope::Table)) return;
      pop_until("table");
      reset_mode();
      return;
    }
    if (end_in(t, {"body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
    if (start_in(t, {"style", "script", "template"}) || end(t, "template")) return mode_in_head(t);
    if (start(t, "input")) {
      StringView type;
      if (t.attr("type", &type) && type.equals_ignore_case("hidden")) return insert_void(t);
    }
    if (start(t, "form")) {
      if (template_on_stack() || form_) return;
      form_ = insert_element(t, nullptr, false);
      return;
    }
    if (t.type == Token::Eof) return mode_in_body(t);
    in_table_foster(t);
  }
  void mode_in_caption(Token& t) {
    if (end(t, "caption")) {
      if (!in_scope("caption", Scope::Table)) return;
      generate_implied();
      pop_until("caption");
      afe_clear_to_marker();
      mode_ = Mode::InTable;
      return;
    }
    if (start_in(t, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"}) || end(t, "table")) {
      if (!in_scope("caption", Scope::Table)) return;
      generate_implied();
      pop_until("caption");
      afe_clear_to_marker();
      mode_ = Mode::InTable;
      return process_in(mode_, t);
    }
    if (end_in(t, {"body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
    mode_in_body(t);
  }
  void mode_in_column_group(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t);
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (start(t, "col")) return insert_void(t);
    if (end(t, "colgroup")) {
      if (!cur_is("colgroup")) return;
      pop();
      mode_ = Mode::InTable;
      return;
    }
    if (end(t, "col")) return;
    if (start(t, "template") || end(t, "template")) return mode_in_head(t);
    if (t.type == Token::Eof) return mode_in_body(t);
    if (!cur_is("colgroup")) return;
    pop();
    mode_ = Mode::InTable;
    process_in(mode_, t);
  }
  void mode_in_table_body(Token& t) {
    if (start(t, "tr")) {
      clear_to_context({"tbody", "tfoot", "thead"});
      insert_html(t);
      mode_ = Mode::InRow;
      return;
    }
    if (start_in(t, {"th", "td"})) {
      clear_to_context({"tbody", "tfoot", "thead"});
      synth("tr", Mode::InRow);
      return process_in(mode_, t);
    }
    if (end_in(t, {"tbody", "tfoot", "thead"})) {
      if (!in_scope(t.name.view(), Scope::Table)) return;
      clear_to_context({"tbody", "tfoot", "thead"});
      pop();
      mode_ = Mode::InTable;
      return;
    }
    if (start_in(t, {"caption", "col", "colgroup", "tbody", "tfoot", "thead"}) || end(t, "table")) {
      if (!in_scope("tbody", Scope::Table) && !in_scope("thead", Scope::Table) && !in_scope("tfoot", Scope::Table)) return;
      clear_to_context({"tbody", "tfoot", "thead"});
      pop();
      mode_ = Mode::InTable;
      return process_in(mode_, t);
    }
    if (end_in(t, {"body", "caption", "col", "colgroup", "html", "td", "th", "tr"})) return;
    mode_in_table(t);
  }
  void mode_in_row(Token& t) {
    if (start_in(t, {"th", "td"})) {
      clear_to_context({"tr"});
      insert_html(t);
      mode_ = Mode::InCell;
      afe_marker();
      return;
    }
    if (end(t, "tr")) {
      if (!in_scope("tr", Scope::Table)) return;
      clear_to_context({"tr"});
      pop();
      mode_ = Mode::InTableBody;
      return;
    }
    if (start_in(t, {"caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"}) || end(t, "table")) {
      if (!in_scope("tr", Scope::Table)) return;
      clear_to_context({"tr"});
      pop();
      mode_ = Mode::InTableBody;
      return process_in(mode_, t);
    }
    if (end_in(t, {"tbody", "tfoot", "thead"})) {
      if (!in_scope(t.name.view(), Scope::Table) || !in_scope("tr", Scope::Table)) return;
      clear_to_context({"tr"});
      pop();
      mode_ = Mode::InTableBody;
      return process_in(mode_, t);
    }
    if (end_in(t, {"body", "caption", "col", "colgroup", "html", "td", "th"})) return;
    mode_in_table(t);
  }
  void close_cell() {
    generate_implied();
    pop_until_one_of({"td", "th"});
    afe_clear_to_marker();
    mode_ = Mode::InRow;
  }
  void mode_in_cell(Token& t) {
    if (end_in(t, {"td", "th"})) {
      if (!in_scope(t.name.view(), Scope::Table)) return;
      generate_implied();
      pop_until(t.name.view());
      afe_clear_to_marker();
      mode_ = Mode::InRow;
      return;
    }
    if (start_in(t, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"})) {
      if (!in_scope("td", Scope::Table) && !in_scope("th", Scope::Table)) return;
      close_cell();
      return process_in(mode_, t);
    }
    if (end_in(t, {"body", "caption", "col", "colgroup", "html"})) return;
    if (end_in(t, {"table", "tbody", "tfoot", "thead", "tr"})) {
      if (!in_scope(t.name.view(), Scope::Table)) return;
      close_cell();
      return process_in(mode_, t);
    }
    mode_in_body(t);
  }

  // --- Select
  void mode_in_select(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t);
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (start(t, "option")) {
      if (cur_is("option")) pop();
      insert_html(t);
      return;
    }
    if (start(t, "optgroup")) {
      if (cur_is("option")) pop();
      if (cur_is("optgroup")) pop();
      insert_html(t);
      return;
    }
    if (start(t, "hr")) {
      if (cur_is("option")) pop();
      if (cur_is("optgroup")) pop();
      insert_void(t);
      return;
    }
    if (end(t, "optgroup")) {
      if (cur_is("option") && open_.count() >= 2 && is_el(open_[open_.count() - 2], "optgroup")) pop();
      if (cur_is("optgroup")) pop();
      return;
    }
    if (end(t, "option")) {
      if (cur_is("option")) pop();
      return;
    }
    if (end(t, "select")) {
      if (!in_scope("select", Scope::Select)) return;
      pop_until("select");
      reset_mode();
      return;
    }
    if (start(t, "select")) {
      if (!in_scope("select", Scope::Select)) return;
      pop_until("select");
      reset_mode();
      return;
    }
    if (start_in(t, {"input", "keygen", "textarea"})) {
      if (!in_scope("select", Scope::Select)) return;
      pop_until("select");
      reset_mode();
      return process_in(mode_, t);
    }
    if (start_in(t, {"script", "template"}) || end(t, "template")) return mode_in_head(t);
    if (t.type == Token::Eof) return mode_in_body(t);
  }
  void mode_in_select_in_table(Token& t) {
    if (start_in(t, {"caption", "table", "tbody", "tfoot", "thead", "tr", "td", "th"})) {
      pop_until("select");
      reset_mode();
      return process_in(mode_, t);
    }
    if (end_in(t, {"caption", "table", "tbody", "tfoot", "thead", "tr", "td", "th"})) {
      if (!in_scope(t.name.view(), Scope::Table)) return;
      pop_until("select");
      reset_mode();
      return process_in(mode_, t);
    }
    mode_in_select(t);
  }

  // --- Templates
  void mode_in_template(Token& t) {
    if (t.type == Token::Comment || t.type == Token::Doctype) return mode_in_body(t);
    if (start_in(t, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"}) || end(t, "template")) return mode_in_head(t);
    auto switch_to = [&](Mode m) {
      if (tmpl_modes_.count()) tmpl_modes_.remove_at(tmpl_modes_.count() - 1);
      tmpl_modes_.push_back(m);
      mode_ = m;
      process_in(mode_, t);
    };
    if (start_in(t, {"caption", "colgroup", "tbody", "tfoot", "thead"})) return switch_to(Mode::InTable);
    if (start(t, "col")) return switch_to(Mode::InColumnGroup);
    if (start(t, "tr")) return switch_to(Mode::InTableBody);
    if (start_in(t, {"td", "th"})) return switch_to(Mode::InRow);
    if (t.type == Token::StartTag) return switch_to(Mode::InBody);
    if (t.type == Token::EndTag) return;
    if (t.type == Token::Eof) {
      if (!template_on_stack()) {
        stopped_ = true;
        return;
      }
      pop_until("template");
      afe_clear_to_marker();
      if (tmpl_modes_.count()) tmpl_modes_.remove_at(tmpl_modes_.count() - 1);
      reset_mode();
      return process_in(mode_, t);
    }
  }

  // --- After the body
  void mode_after_body(Token& t) {
    if (t.type == Token::Comment) {
      if (open_.count()) insert_comment(t, open_[0]);
      return;
    }
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (end(t, "html")) {
      if (fragment_) return;
      mode_ = Mode::AfterAfterBody;
      return;
    }
    if (t.type == Token::Eof) {
      stopped_ = true;
      return;
    }
    mode_ = Mode::InBody;
    process_in(mode_, t);
  }
  void mode_in_frameset(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t);
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (start(t, "frameset")) {
      insert_html(t);
      return;
    }
    if (end(t, "frameset")) {
      if (open_.count() == 1) return;
      pop();
      if (!fragment_ && !cur_is("frameset")) mode_ = Mode::AfterFrameset;
      return;
    }
    if (start(t, "frame")) return insert_void(t);
    if (start(t, "noframes")) return mode_in_head(t);
    if (t.type == Token::Eof) stopped_ = true;
  }
  void mode_after_frameset(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t);
    if (t.type == Token::Doctype) return;
    if (start(t, "html")) return mode_in_body(t);
    if (end(t, "html")) {
      mode_ = Mode::AfterAfterFrameset;
      return;
    }
    if (start(t, "noframes")) return mode_in_head(t);
    if (t.type == Token::Eof) stopped_ = true;
  }
  void mode_after_after_body(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t, doc_.root());
    if (t.type == Token::Doctype || start(t, "html")) return mode_in_body(t);
    if (t.type == Token::Eof) {
      stopped_ = true;
      return;
    }
    mode_ = Mode::InBody;
    process_in(mode_, t);
  }
  void mode_after_after_frameset(Token& t) {
    if (t.type == Token::Comment) return insert_comment(t, doc_.root());
    if (t.type == Token::Doctype || start(t, "html")) return mode_in_body(t);
    if (start(t, "noframes")) return mode_in_head(t);
    if (t.type == Token::Eof) stopped_ = true;
  }

  // ======================================================================== foreign content
  void foreign(Token& t) {
    switch (t.type) {
      case Token::Comment: return insert_comment(t);
      case Token::Doctype: return;
      case Token::StartTag: break;
      case Token::EndTag: return foreign_end(t);
      default: return;
    }
    const StringView n = t.name.view();
    StringView ignored;
    const bool breakout = name_in(n, {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr",
                                      "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table",
                                      "tt", "u", "ul", "var"}) ||
                          (n.equals("font") && (t.attr("color", &ignored) || t.attr("face", &ignored) || t.attr("size", &ignored)));
    if (breakout) {
      if (fragment_ && open_.count() == 1) {
        // (a fragment's context element is the adjusted current node: nothing to pop)
      } else {
        while (cur() && !is_html(cur()) && !is_mathml_text_ip(cur()) && !is_html_ip(cur())) pop();
      }
      return process(t);
    }
    const Node* adjusted = adjusted_current();
    const char* ns = adjusted->ns.view().equals(kSvgNs) ? kSvgNs : kMathNs;
    Node* el = insert_element(t, ns, false);
    if (!el) return;
    if (t.self_closing) {
      if (n.equals("script") && ns == kSvgNs) { /* (acts as an end tag: nothing runs) */ }
      t_ack(t);
    } else {
      push(el);
    }
  }
  void foreign_end(Token& t) {
    if (t.name.view().equals("script") && cur() && in_ns(cur(), kSvgNs) && cur()->name.view().equals("script")) {
      pop();
      return;
    }
    uint64_t i = open_.count();
    if (i == 0) return;
    Node* node = open_[i - 1];
    for (;;) {
      if (i == 1) return;
      if (node->name.view().equals(t.name.view())) {
        pop_until_node(node);
        return;
      }
      --i;
      node = open_[i - 1];
      if (is_html(node)) return process_in(mode_, t);
    }
  }

  // ======================================================================== scripts
  void run_script(Node* script) {
    StringView type, src;
    const bool has_type = get_attribute(script, "type", &type);
    const bool has_src = get_attribute(script, "src", &src) && !src.empty();
    uint32_t index = 0;
    if (has_src) index = external_scripts_++;
    const bool javascript = !has_type || type.empty() || type.equals_ignore_case("text/javascript") || type.equals_ignore_case("application/javascript") ||
                            type.equals_ignore_case("text/ecmascript") || type.equals_ignore_case("application/ecmascript");
    if (!scripts_ || fragment_ || !javascript || script_depth_ > 3 || has_attribute(script, "nomodule")) return;
    for (const Node* p = script->parent; p; p = p->parent)
      if (p->is_tag("template") || p->template_content) return;   // (what is in a template is inert)
    String inline_text(doc_.allocator());
    text_content(script, &inline_text);
    const StringView source = has_src ? scripts_->external(index, src) : inline_text.view();
    if (source.empty()) return;
    written_.clear();
    ++script_depth_;
    scripts_->run(script, source, &written_);
    if (!written_.empty()) input_.insert(pos_, written_.view());   // (what the script wrote is read next, in front of the rest of the page)
    written_.clear();
    --script_depth_;
  }

  Document& doc_;
  IScriptRunner* scripts_;
  Node* fragment_;
  Node* context_;
  allocators::IAllocator& a_;
  List<Node*> open_;
  List<Node*> afe_;
  List<Mode> tmpl_modes_;
  String input_, chars_, temp_, last_start_, attr_name_, attr_value_;
  uint64_t pos_ = 0;
  State state_ = State::Data;
  Mode mode_ = Mode::Initial, original_mode_ = Mode::InBody;
  Token tok_a_, tok_b_, tok_c_;
  Token* tb_ = nullptr;
  Token* held_ = nullptr;
  String pending_text_, table_text_, written_;
  bool table_text_non_space_ = false;
  Node *head_ = nullptr, *form_ = nullptr, *root_ = nullptr;
  bool ready_ = false, eof_next_ = false, skip_lf_ = false, frameset_ok_ = true, foster_ = false, scripting_ = false, failed_ = false, stopped_ = false;
  uint32_t external_scripts_ = 0, script_depth_ = 0;
};

}  // namespace

static void move_into_content(Node* t) {
  while (t->first_child) {
    Node* c = t->first_child;
    remove_child(t, c);
    append_child(t->content, c);
  }
}

Node* template_content(Node* tmpl) {
  if (!tmpl->content) {
    tmpl->content = tmpl->owner->create_fragment();
    if (!tmpl->content) return nullptr;
    tmpl->content->template_content = true;
  }
  move_into_content(tmpl);   // (what has been read into the template so far, when a script asks while the page is being read)
  return tmpl->content;
}

void adopt_template_contents(Document& doc) {
  for (uint32_t serial = 1; serial <= doc.node_count(); ++serial) {
    Node* t = doc.by_serial(serial);
    if (t && t->is_tag("template")) template_content(t);
  }
}

bool build_document(Document& doc, StringView html, IScriptRunner* scripts) {
  TreeBuilder builder(doc, scripts, nullptr, nullptr);
  const bool ok = builder.run(html);
  adopt_template_contents(doc);
  return ok;
}

static Node* parse_in_context(Document& doc, StringView html, Node* context) {
  Node* fragment = doc.create_fragment();
  if (!fragment) return nullptr;
  TreeBuilder builder(doc, nullptr, fragment, context);
  const bool ok = builder.run(html);
  adopt_template_contents(doc);
  return ok ? fragment : nullptr;
}

Node* parse_fragment(Document& doc, StringView html, StringView context_tag) {
  Node* context = doc.create_element(context_tag);
  if (!context) return nullptr;
  return parse_in_context(doc, html, context);
}

DomError set_inner_html(Node* element, StringView html) {
  if (element->type != NodeType::Element && element->type != NodeType::Fragment) return DomError::HierarchyRequest;
  Node* context = element;
  if (element->type == NodeType::Fragment) {   // (a shadow root or fragment: parsed as for a <body>)
    context = element->owner->create_element("body");
    if (!context) return DomError::OutOfMemory;
  }
  Node* fragment = parse_in_context(*element->owner, html, context);
  if (!fragment) return DomError::OutOfMemory;
  Node* target = element->is_tag("template") ? template_content(element) : element;   // (a template's markup is its content)
  if (!target) return DomError::OutOfMemory;
  while (target->first_child) remove_child(target, target->first_child);
  return append_child(target, fragment);
}

}  // namespace dom

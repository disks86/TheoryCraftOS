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

#include "Dom/Selector.hpp"
#include "Dom/Form.hpp"

namespace dom {

namespace {
// Is the element editable through contenteditable (its own attribute, or the nearest ancestor that has one)?
bool editing_host(const Node* node) {
  for (const Node* e = node; e && e->is_element(); e = e->parent) {
    StringView v;
    if (!get_attribute(e, "contenteditable", &v)) continue;
    return v.empty() || v.equals_ignore_case("true") || v.equals_ignore_case("plaintext-only");
  }
  return false;
}
}  // namespace

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || static_cast<uint8_t>(c) >= 0x80 || c == '\\'; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

bool equals_fold(StringView a, StringView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

bool has_word(StringView list, StringView word, bool fold) {
  uint64_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && is_space(list[i])) ++i;
    const uint64_t start = i;
    while (i < list.size() && !is_space(list[i])) ++i;
    if (i > start) {
      const StringView w = list.substr(start, i - start);
      if (fold ? equals_fold(w, word) : w.equals(word)) return true;
    }
  }
  return false;
}

bool is_form_control(StringView tag) {
  return tag.equals("input") || tag.equals("button") || tag.equals("select") || tag.equals("textarea") || tag.equals("optgroup") || tag.equals("option") ||
         tag.equals("fieldset");
}

}  // namespace

void SelectorSet::clear() {
  pool_.clear();
  tests_.clear();
  compounds_.clear();
  selectors_.clear();
  tops_.clear();
  refs_.clear();
}

uint32_t SelectorSet::tag(uint32_t i) const { return selectors_[tops_[i]].tag; }
uint32_t SelectorSet::specificity(uint32_t i) const { return selectors_[tops_[i]].specificity; }
uint8_t SelectorSet::pseudo(uint32_t i) const { return selectors_[tops_[i]].pseudo; }
bool SelectorSet::uses_host(uint32_t i) const { return selectors_[tops_[i]].host; }
bool SelectorSet::uses_slotted(uint32_t i) const { return selectors_[tops_[i]].slotted; }

// --- Reading selectors -------------------------------------------------------------------------------------------

class SelectorSet::Reader {
 public:
  Reader(SelectorSet& set, StringView text, uint32_t tag) : set_(set), text_(text), tag_(tag), scratch_(set.alloc_) {}

  // The whole text as a list of selectors added at top level. *syntax: false if it is not a selector list.
  bool top(uint32_t* added, bool* syntax, bool forgiving) {
    uint32_t first, count;
    *syntax = list(false, false, forgiving, &first, &count);
    skip_space();
    if (*syntax && pos_ < text_.size()) *syntax = false;   // (a stray ")" or the like)
    *added = *syntax ? count : 0;
    return !oom_;
  }

 private:
  uint8_t pseudo_element_ = 0;   // the pseudo-element the selector being read ends in (1 ::before, 2 ::after, 3 ::marker)
  bool end() const { return pos_ >= text_.size(); }
  char cur() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void skip_space() {
    while (pos_ < text_.size() && is_space(text_[pos_])) ++pos_;
  }

  // An identifier (escapes decoded) into scratch_.
  bool ident() {
    scratch_.clear();
    bool any = false;
    if (pos_ < text_.size() && (is_digit(text_[pos_]) || (text_[pos_] == '-' && pos_ + 1 < text_.size() && is_digit(text_[pos_ + 1])))) return false;   // (an identifier does not start with a digit)
    while (pos_ < text_.size() && is_ident_char(text_[pos_])) {
      char c = text_[pos_];
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) return false;
        if (is_hex(text_[pos_])) {
          uint32_t cp = 0;
          int n = 0;
          while (n < 6 && pos_ < text_.size() && is_hex(text_[pos_])) {
            const char h = text_[pos_++];
            cp = cp * 16 + static_cast<uint32_t>(is_digit(h) ? h - '0' : (h | 32) - 'a' + 10);
            ++n;
          }
          if (pos_ < text_.size() && is_space(text_[pos_])) ++pos_;
          scratch_.push_back(cp < 0x80 ? static_cast<char>(cp) : '?');
        } else {
          scratch_.push_back(text_[pos_++]);
        }
      } else {
        scratch_.push_back(c);
        ++pos_;
      }
      any = true;
    }
    return any;
  }

  bool keep(StringView s, uint32_t* offset) {
    *offset = static_cast<uint32_t>(set_.pool_.size());
    if (!set_.pool_.append(s)) {
      oom_ = true;
      return false;
    }
    return true;
  }

  // a selector list up to the end or a ")": *first / *count describe where the new selectors are in refs_ (for inner lists) or tops_.
  bool list(bool inner, bool relative, bool forgiving, uint32_t* first, uint32_t* count) {
    List<uint32_t> found(set_.alloc_);
    for (;;) {
      skip_space();
      bool syntax = true, usable = false;
      uint32_t index = 0;
      const uint64_t before = pos_;
      if (end() || cur() == ')' || cur() == ',') {
        syntax = false;   // an empty selector
      } else {
        syntax = complex(inner, relative, &usable, &index);
      }
      if (!syntax) {
        if (!forgiving) return false;
        // skip to the next comma outside brackets
        pos_ = before;
        int depth = 0;
        char quote = 0;
        while (!end()) {
          const char c = text_[pos_];
          if (quote) {
            if (c == '\\') ++pos_;
            else if (c == quote) quote = 0;
          } else if (c == '"' || c == '\'') {
            quote = c;
          } else if (c == '(' || c == '[') {
            ++depth;
          } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
          } else if (depth == 0 && (c == ',' || c == ')')) {
            break;
          }
          ++pos_;
        }
      } else if (usable) {
        found.push_back(index);
      }
      skip_space();
      if (cur() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    *count = static_cast<uint32_t>(found.count());
    if (inner) {
      *first = static_cast<uint32_t>(set_.refs_.count());
      for (uint64_t i = 0; i < found.count(); ++i) set_.refs_.push_back(found[i]);
    } else {
      *first = static_cast<uint32_t>(set_.tops_.count());
      for (uint64_t i = 0; i < found.count(); ++i) set_.tops_.push_back(found[i]);
    }
    return true;
  }

  // One complex selector. Returns false for a syntax error; *usable is false when it is fine but can never match (it is dropped).
  bool complex(bool inner, bool relative, bool* usable, uint32_t* index) {
    List<Compound> mine(set_.alloc_);   // (put in the set when the selector is complete: a :not() inside adds compounds of its own meanwhile)
    pseudo_element_ = 0;
    uint32_t specificity = 0;
    Combinator comb = Combinator::None;
    bool have_combinator = false, drop = false, syntax = true;
    uint32_t compounds = 0;
    for (;;) {
      const bool space = !end() && is_space(cur());
      skip_space();
      if (end() || cur() == ',' || cur() == ')') break;
      const char c = cur();
      if (c == '>' || c == '+' || c == '~') {
        if (have_combinator || (compounds == 0 && !relative)) {
          syntax = false;
          break;
        }
        comb = c == '>' ? Combinator::Child : c == '+' ? Combinator::Adjacent : Combinator::Sibling;
        have_combinator = true;
        ++pos_;
        continue;
      }
      if (space && compounds > 0 && !have_combinator) comb = Combinator::Descendant;
      Compound compound;
      compound.combinator = compounds == 0 ? (relative ? (have_combinator ? comb : Combinator::Descendant) : Combinator::None) : comb;
      compound.first_test = static_cast<uint32_t>(set_.tests_.count());
      compound.test_count = 0;
      if (!compound_tests(&compound, &specificity, &drop)) {
        syntax = false;
        break;
      }
      if (oom_) return false;
      if (compound.test_count == 0 && !universal_) {
        syntax = false;
        break;
      }
      mine.push_back(compound);
      ++compounds;
      comb = Combinator::None;
      have_combinator = false;
    }
    if (compounds == 0 || have_combinator) syntax = false;
    if (!syntax || drop) {
      *usable = false;
      return syntax;
    }
    Selector s;
    s.first_compound = static_cast<uint32_t>(set_.compounds_.count());
    for (uint64_t i = 0; i < mine.count(); ++i)
      if (!set_.compounds_.push_back(mine[i])) oom_ = true;
    s.compound_count = compounds;
    s.specificity = specificity;
    s.tag = tag_;
    s.inner = inner;
    s.pseudo = inner ? 0 : pseudo_element_;
    set_.selectors_.push_back(s);
    *index = static_cast<uint32_t>(set_.selectors_.count() - 1);
    *usable = true;
    return true;
  }

  Test blank(TestKind kind) {
    Test t{};
    t.kind = kind;
    t.op = AttrOp::Exists;
    return t;
  }

  // The tests of one compound selector; false for a syntax error. `drop` is set when the compound can never match.
  bool compound_tests(Compound* c, uint32_t* specificity, bool* drop) {
    universal_ = false;
    bool first = true;
    local_.clear();   // (the tests are put in the set when the compound is complete: a :not() inside it adds tests of its own meanwhile)
    List<Test> mine(set_.alloc_);
    bool ok = compound_loop(c, specificity, drop, &mine, &first);
    c->first_test = static_cast<uint32_t>(set_.tests_.count());
    for (uint64_t i = 0; i < mine.count(); ++i)
      if (!set_.tests_.push_back(mine[i])) oom_ = true;
    c->test_count = static_cast<uint32_t>(mine.count());
    return ok;
  }

  bool compound_loop(Compound* c, uint32_t* specificity, bool* drop, List<Test>* mine, bool* first_flag) {
    bool first = *first_flag;
    (void)c;
    while (!end()) {
      const char k = cur();
      if (is_space(k) || k == '>' || k == '+' || k == '~' || k == ',' || k == ')') break;
      Test t = blank(TestKind::Tag);
      if (k == '*') {
        ++pos_;
        universal_ = true;
        first = false;
        continue;
      }
      if (is_ident_start(k) && (first || mine->count() == 0)) {
        if (!ident()) return false;
        t.kind = TestKind::Tag;
        String lowered(set_.alloc_);
        for (uint64_t i = 0; i < scratch_.size(); ++i) lowered.push_back(lower(scratch_[i]));
        if (!keep(lowered.view(), &t.name_offset)) return false;
        t.name_length = static_cast<uint32_t>(lowered.size());
        *specificity += 1;
      } else if (k == '#' || k == '.') {
        ++pos_;
        if (!ident()) return false;
        t.kind = k == '#' ? TestKind::Id : TestKind::Class;
        if (!keep(scratch_.view(), &t.name_offset)) return false;
        t.name_length = static_cast<uint32_t>(scratch_.size());
        *specificity += k == '#' ? 10000 : 100;
      } else if (k == '[') {
        if (!attribute(&t)) return false;
        *specificity += 100;
      } else if (k == ':') {
        bool applies = true;
        if (!pseudo(&t, specificity, &applies)) return false;
        if (!applies) {
          *drop = true;
          t.kind = TestKind::Never;
        }
      } else {
        return false;
      }
      first = false;
      if (!mine->push_back(t)) oom_ = true;
      if (oom_) return false;
    }
    return true;
  }

  bool attribute(Test* t) {
    ++pos_;   // [
    skip_space();
    if (!ident()) return false;
    String name(set_.alloc_);
    for (uint64_t i = 0; i < scratch_.size(); ++i) name.push_back(lower(scratch_[i]));
    skip_space();
    t->kind = TestKind::Attr;
    t->op = AttrOp::Exists;
    String value(set_.alloc_);
    if (cur() == '=' || (pos_ + 1 < text_.size() && text_[pos_ + 1] == '=' && (cur() == '~' || cur() == '|' || cur() == '^' || cur() == '$' || cur() == '*'))) {
      const char op = cur();
      if (op == '=') {
        t->op = AttrOp::Equals;
        ++pos_;
      } else {
        t->op = op == '~' ? AttrOp::Includes : op == '|' ? AttrOp::Dash : op == '^' ? AttrOp::Prefix : op == '$' ? AttrOp::Suffix : AttrOp::Contains;
        pos_ += 2;
      }
      skip_space();
      if (cur() == '"' || cur() == '\'') {
        const char q = text_[pos_++];
        while (!end() && cur() != q) {
          if (cur() == '\\' && pos_ + 1 < text_.size()) ++pos_;
          value.push_back(text_[pos_++]);
        }
        if (end()) return false;
        ++pos_;
      } else {
        if (!ident()) {
          // numbers and other unquoted values
          scratch_.clear();
          while (!end() && !is_space(cur()) && cur() != ']') scratch_.push_back(text_[pos_++]);
          if (scratch_.empty()) return false;
        }
        value.assign(scratch_.view());
      }
      skip_space();
      if (cur() == 'i' || cur() == 'I') {
        t->fold_case = true;
        ++pos_;
        skip_space();
      } else if (cur() == 's' || cur() == 'S') {
        ++pos_;
        skip_space();
      }
    }
    if (cur() != ']') return false;
    ++pos_;
    if (!keep(name.view(), &t->name_offset) || !keep(value.view(), &t->value_offset)) return false;
    t->name_length = static_cast<uint32_t>(name.size());
    t->value_length = static_cast<uint32_t>(value.size());
    return true;
  }

  // The "an+b" of the :nth family, up to the closing parenthesis.
  bool nth(int32_t* a, int32_t* b) {
    skip_space();
    String s(set_.alloc_);
    while (!end() && cur() != ')') {
      if (is_space(cur())) {   // "of S" ends the An+B
        uint64_t k = pos_;
        while (k < text_.size() && is_space(text_[k])) ++k;
        if (k + 2 < text_.size() && lower(text_[k]) == 'o' && lower(text_[k + 1]) == 'f' && is_space(text_[k + 2])) break;
      } else {
        s.push_back(lower(cur()));
      }
      ++pos_;
    }
    if (s.view().equals("odd")) {
      *a = 2;
      *b = 1;
      return true;
    }
    if (s.view().equals("even")) {
      *a = 2;
      *b = 0;
      return true;
    }
    const uint64_t n = s.view().find('n');
    auto number = [](StringView v, int32_t* out) {
      bool neg = false;
      uint64_t i = 0;
      if (i < v.size() && (v[i] == '+' || v[i] == '-')) neg = v[i++] == '-';
      if (i >= v.size()) return false;
      int64_t value = 0;
      for (; i < v.size(); ++i) {
        if (!is_digit(v[i])) return false;
        value = value * 10 + (v[i] - '0');
        if (value > 1000000) value = 1000000;
      }
      *out = static_cast<int32_t>(neg ? -value : value);
      return true;
    };
    if (n == StringView::npos) {
      *a = 0;
      return number(s.view(), b);
    }
    const StringView head = s.view().first(n), tail = s.view().substr(n + 1);
    if (head.empty() || head.equals("+")) *a = 1;
    else if (head.equals("-")) *a = -1;
    else if (!number(head, a)) return false;
    if (tail.empty()) *b = 0;
    else if (!number(tail, b)) return false;
    return true;
  }

  bool pseudo(Test* t, uint32_t* specificity, bool* applies) {
    ++pos_;   // :
    if (cur() == ':') {   // a pseudo-element
      ++pos_;
      if (!ident()) return false;
      String element_name(set_.alloc_);
      for (uint64_t i = 0; i < scratch_.size(); ++i) element_name.push_back(lower(scratch_[i]));
      const bool known = element_name.equals("before") || element_name.equals("after") || element_name.equals("marker");
      if (element_name.equals("slotted") && cur() == '(') {   // ::slotted(selector): what is assigned to a slot, if it matches the selector
        ++pos_;
        uint32_t first, count;
        if (!list(true, false, true, &first, &count)) return false;
        skip_space();
        if (cur() != ')') return false;
        ++pos_;
        t->kind = TestKind::Is;
        t->first_ref = first;
        t->ref_count = count;
        if (count == 0) *applies = false;
        *specificity += 1;
        return true;
      }
      if (cur() == '(') {
        ++pos_;
        skip_parens_body();
        *applies = false;
      } else if (known) {
        pseudo_element_ = element_name.equals("before") ? 1 : element_name.equals("after") ? 2 : 3;
        t->kind = TestKind::Always;
      } else {
        *applies = false;
      }
      *specificity += 1;
      return true;
    }
    if (!ident()) return false;
    String name(set_.alloc_);
    for (uint64_t i = 0; i < scratch_.size(); ++i) name.push_back(lower(scratch_[i]));
    const StringView n = name.view();
    *specificity += 100;
    if (cur() == '(') {
      ++pos_;
      if (n.equals("nth-child") || n.equals("nth-last-child") || n.equals("nth-of-type") || n.equals("nth-last-of-type")) {
        t->kind = TestKind::Nth;
        t->from_end = n.equals("nth-last-child") || n.equals("nth-last-of-type");
        t->of_type = n.equals("nth-of-type") || n.equals("nth-last-of-type");
        if (!nth(&t->a, &t->b)) return false;
        skip_space();
        if (cur() != ')' && !t->of_type) {   // :nth-child(2n+1 of S): counted among the siblings that match S
          if (!ident() || !scratch_.view().equals_ignore_case("of")) return false;
          skip_space();
          uint32_t first, count;
          if (!list(true, false, true, &first, &count)) return false;
          skip_space();
          t->first_ref = first;
          t->ref_count = count;
          uint32_t best = 0;
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t sp = set_.selectors_[set_.refs_[first + i]].specificity;
            if (sp > best) best = sp;
          }
          *specificity += best;
        }
        if (cur() != ')') return false;
        ++pos_;
        return true;
      }
      if (n.equals("not") || n.equals("is") || n.equals("where") || n.equals("matches") || n.equals("-webkit-any") || n.equals("-moz-any") || n.equals("has") || n.equals("host")) {
        const bool has = n.equals("has");
        skip_space();
        if (cur() == ')' && (has || n.equals("not"))) return false;   // (an empty :not() or :has() is a syntax error)
        t->kind = n.equals("not") ? TestKind::Not : has ? TestKind::Has : n.equals("host") ? TestKind::HostIs : TestKind::Is;
        // the specificity inside counts (the most specific), except for :where()
        const uint64_t selectors_before = set_.selectors_.count();
        uint32_t first, count;
        if (!list(true, has, true, &first, &count)) return false;
        skip_space();
        if (cur() != ')') return false;
        ++pos_;
        t->first_ref = first;
        t->ref_count = count;
        uint32_t best = 0;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t s = set_.selectors_[set_.refs_[first + i]].specificity;
          if (s > best) best = s;
        }
        (void)selectors_before;
        *specificity -= 100;
        if (!n.equals("where")) *specificity += best;
        if (count == 0 && !n.equals("not")) *applies = false;   // an empty :is() matches nothing
        return true;
      }
      if (n.equals("lang")) {   // :lang(range, "range", ...): the language of the element (its own lang attribute or the nearest ancestor's) is one of them
        String ranges(set_.alloc_);
        for (;;) {
          skip_space();
          if (cur() == '"' || cur() == '\'') {
            const char q = text_[pos_++];
            while (!end() && cur() != q) ranges.push_back(text_[pos_++]);
            if (end()) return false;
            ++pos_;
          } else if (cur() == '*' || ident()) {
            if (cur() == '*') { ranges.push_back('*'); ++pos_; }
            else ranges.append(scratch_.view());
          } else {
            return false;
          }
          skip_space();
          if (cur() == ',') {
            ranges.push_back('\n');
            ++pos_;
            continue;
          }
          break;
        }
        if (cur() != ')') return false;
        ++pos_;
        t->kind = TestKind::Lang;
        if (!keep(ranges.view(), &t->value_offset)) return false;
        t->value_length = static_cast<uint32_t>(ranges.size());
        return true;
      }
      if (n.equals("dir")) {   // :dir(ltr) and :dir(rtl)
        skip_space();
        if (!ident()) return false;
        const bool rtl = scratch_.view().equals_ignore_case("rtl");
        if (!rtl && !scratch_.view().equals_ignore_case("ltr")) return false;
        skip_space();
        if (cur() != ')') return false;
        ++pos_;
        t->kind = TestKind::Dir;
        t->a = rtl ? 1 : 0;
        return true;
      }
      // :state(), :host-context() and the other functional states that need a page's own state: never match
      if (!(n.equals("dir") || n.equals("state") || n.equals("host-context") || n.equals("current") || n.equals("past") || n.equals("future") || n.equals("nth-col") || n.equals("nth-last-col"))) unknown_pseudo_ = true;
      skip_parens_body();
      *applies = false;
      return true;
    }
    if (n.equals("first-child")) t->kind = TestKind::FirstChild;
    else if (n.equals("last-child")) t->kind = TestKind::LastChild;
    else if (n.equals("only-child")) t->kind = TestKind::OnlyChild;
    else if (n.equals("first-of-type")) t->kind = TestKind::FirstOfType;
    else if (n.equals("last-of-type")) t->kind = TestKind::LastOfType;
    else if (n.equals("only-of-type")) t->kind = TestKind::OnlyOfType;
    else if (n.equals("empty")) t->kind = TestKind::Empty;
    else if (n.equals("root")) t->kind = TestKind::Root;
    else if (n.equals("scope")) t->kind = TestKind::Scope;
    else if (n.equals("host")) t->kind = TestKind::Host;
    else if (n.equals("link") || n.equals("any-link")) t->kind = TestKind::Link;
    else if (n.equals("checked")) t->kind = TestKind::Checked;
    else if (n.equals("disabled")) t->kind = TestKind::Disabled;
    else if (n.equals("enabled")) t->kind = TestKind::Enabled;
    else if (n.equals("focus") || n.equals("focus-visible")) t->kind = TestKind::Focus;
    else if (n.equals("focus-within")) t->kind = TestKind::FocusWithin;
    else if (n.equals("fullscreen") || n.equals("-webkit-full-screen") || n.equals("-moz-full-screen")) t->kind = TestKind::Fullscreen;
    else if (n.equals("required")) t->kind = TestKind::Required;
    else if (n.equals("optional")) t->kind = TestKind::Optional;
    else if (n.equals("read-only")) t->kind = TestKind::ReadOnly;
    else if (n.equals("read-write")) t->kind = TestKind::ReadWrite;
    else if (n.equals("valid")) t->kind = TestKind::Valid;
    else if (n.equals("invalid")) t->kind = TestKind::Invalid;
    else if (n.equals("placeholder-shown")) t->kind = TestKind::PlaceholderShown;
    else if (n.equals("before") || n.equals("after")) {   // (the old pseudo-element syntax)
      pseudo_element_ = n.equals("before") ? 1 : 2;
      t->kind = TestKind::Always;
      *specificity -= 99;
    } else if (n.equals("hover")) t->kind = TestKind::Hover;
    else if (n.equals("active")) t->kind = TestKind::Active;
    else {   // visited, target, first-line and the rest: states a page being read does not have
      static const char* const kInert[] = {"visited", "target", "target-within", "first-line", "first-letter", "indeterminate", "default", "in-range", "out-of-range", "user-valid", "user-invalid", "defined",
                                           "paused", "playing", "popover-open", "modal", "open", "closed", "autofill", "-webkit-autofill", "blank", "local-link", "picture-in-picture", "seeking", "stalled",
                                           "buffering", "muted", "volume-locked", "current", "past", "future", "active-view-transition", "scope-root", "has-slotted", "xr-overlay", "left", "right", "first", "root-like", "selection"};
      bool known = false;
      for (const char* k : kInert) known = known || n.equals(k);
      if (!known) unknown_pseudo_ = true;
      *applies = false;
    }
    return true;
  }

  void skip_parens_body() {   // after the "(": up to and including the ")"
    int depth = 1;
    char quote = 0;
    while (!end() && depth > 0) {
      const char c = text_[pos_++];
      if (quote) {
        if (c == '\\') ++pos_;
        else if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')') {
        --depth;
      }
    }
  }
  void skip_parens() {
    ++pos_;
    skip_parens_body();
  }

  SelectorSet& set_;
  StringView text_;
  uint32_t tag_;
  String scratch_;
  uint64_t pos_ = 0;
  bool oom_ = false, universal_ = false;
public:
  bool unknown_pseudo_ = false;   // a pseudo-class that is none of the ones known (a stylesheet skips the rule; the selector API reports a syntax error)
private:
  List<Test> local_{set_.alloc_};
};

bool SelectorSet::parse(StringView text, uint32_t tag, uint32_t* added, bool forgiving) {
  Reader reader(*this, text, tag);
  bool syntax = true;
  const uint64_t before = tops_.count();
  const bool ok = reader.top(added, &syntax, forgiving);
  if (!forgiving && reader.unknown_pseudo_) return false;
  if (text.contains(":host"))
    for (uint64_t i = before; i < tops_.count(); ++i) selectors_[tops_[i]].host = true;
  if (text.contains("::slotted"))
    for (uint64_t i = before; i < tops_.count(); ++i) selectors_[tops_[i]].slotted = true;
  return ok && syntax;
}

// --- Matching ----------------------------------------------------------------------------------------------------

bool SelectorSet::matches(uint32_t i, const Node* element, const Node* scope, bool with_pseudo) const {
  if (!element || !element->is_element() || i >= tops_.count()) return false;
  const Selector& s = selectors_[tops_[i]];
  if (s.pseudo && !with_pseudo) return false;
  return match_chain(s, s.compound_count - 1, element, scope);
}

bool SelectorSet::matches_any(const Node* element, const Node* scope) const {
  for (uint32_t i = 0; i < tops_.count(); ++i)
    if (matches(i, element, scope)) return true;
  return false;
}

// The compound is written with :host or :host(...).
bool SelectorSet::compound_is_host(const Compound& c) const {
  for (uint32_t i = 0; i < c.test_count; ++i) {
    const TestKind k = tests_[c.first_test + i].kind;
    if (k == TestKind::Host || k == TestKind::HostIs) return true;
  }
  return false;
}

bool SelectorSet::match_chain(const Selector& s, uint32_t k, const Node* node, const Node* scope) const {
  const Compound& c = compounds_[s.first_compound + k];
  if (!match_compound(c, node, scope)) return false;
  if (k == 0) return true;
  const bool to_host = compound_is_host(compounds_[s.first_compound + k - 1]);   // (":host(.x) p": from the shadow tree up to its host)
  switch (c.combinator) {
    case Combinator::Child:
      if (node->parent && node->parent->is_element()) return match_chain(s, k - 1, node->parent, scope);
      return to_host && node->parent && node->parent->shadow_host && match_chain(s, k - 1, node->parent->shadow_host, scope);
    case Combinator::Adjacent: {
      const Node* p = previous_element_sibling(node);
      return p && match_chain(s, k - 1, p, scope);
    }
    case Combinator::Sibling:
      for (const Node* p = previous_element_sibling(node); p; p = previous_element_sibling(p))
        if (match_chain(s, k - 1, p, scope)) return true;
      return false;
    default:
      for (const Node* p = node->parent; p && p->is_element(); p = p->parent)
        if (match_chain(s, k - 1, p, scope)) return true;
      if (to_host) {
        const Node* top = node;
        while (top->parent) top = top->parent;
        return top->shadow_host && match_chain(s, k - 1, top->shadow_host, scope);
      }
      return false;
  }
}

bool SelectorSet::match_compound(const Compound& c, const Node* node, const Node* scope) const {
  for (uint32_t i = 0; i < c.test_count; ++i)
    if (!match_test(tests_[c.first_test + i], node, scope)) return false;
  return true;
}

bool SelectorSet::match_inner(const Test& t, const Node* node, const Node* scope) const {
  for (uint32_t i = 0; i < t.ref_count; ++i) {
    const Selector& s = selectors_[refs_[t.first_ref + i]];
    if (match_chain(s, s.compound_count - 1, node, scope)) return true;
  }
  return false;
}

// A control is disabled by its own attribute or by a fieldset around it that is (the controls in that fieldset's first legend stay enabled).
static bool is_disabled_control(const Node* node) {
  if (has_attribute(node, "disabled")) return true;
  const Node* child = node;
  for (const Node* p = node->parent; p && p->is_element(); child = p, p = p->parent) {
    if (!p->is_tag("fieldset") || !has_attribute(p, "disabled")) continue;
    const Node* legend = nullptr;
    for (const Node* k = p->first_child; k && !legend; k = k->next)
      if (k->is_tag("legend")) legend = k;
    if (legend != child) return true;
  }
  return false;
}

bool SelectorSet::match_test(const Test& t, const Node* node, const Node* scope) const {
  StringView value;
  switch (t.kind) {
    case TestKind::Tag: return node->name.view().equals(text(t.name_offset, t.name_length));
    case TestKind::Id: return get_attribute(node, "id", &value) && value.equals(text(t.name_offset, t.name_length));
    case TestKind::Class: return has_class(node, text(t.name_offset, t.name_length));
    case TestKind::Attr: {
      if (!get_attribute(node, text(t.name_offset, t.name_length), &value)) return false;
      const StringView want = text(t.value_offset, t.value_length);
      auto same = [&](StringView a, StringView b) { return t.fold_case ? equals_fold(a, b) : a.equals(b); };
      switch (t.op) {
        case AttrOp::Exists: return true;
        case AttrOp::Equals: return same(value, want);
        case AttrOp::Includes: return !want.empty() && has_word(value, want, t.fold_case);
        case AttrOp::Dash: return same(value, want) || (value.size() > want.size() && value[want.size()] == '-' && same(value.first(want.size()), want));
        case AttrOp::Prefix: return !want.empty() && value.size() >= want.size() && same(value.first(want.size()), want);
        case AttrOp::Suffix: return !want.empty() && value.size() >= want.size() && same(value.substr(value.size() - want.size()), want);
        default:
          if (want.empty()) return false;
          for (uint64_t i = 0; i + want.size() <= value.size(); ++i)
            if (same(value.substr(i, want.size()), want)) return true;
          return false;
      }
    }
    case TestKind::FirstChild: return !previous_element_sibling(node);
    case TestKind::LastChild: return !next_element_sibling(node);
    case TestKind::OnlyChild: return !previous_element_sibling(node) && !next_element_sibling(node);
    case TestKind::FirstOfType:
    case TestKind::LastOfType:
    case TestKind::OnlyOfType: {
      const bool before = t.kind != TestKind::LastOfType, after = t.kind != TestKind::FirstOfType;
      if (before)
        for (const Node* p = previous_element_sibling(node); p; p = previous_element_sibling(p))
          if (p->name.view().equals(node->name.view())) return false;
      if (after)
        for (const Node* p = next_element_sibling(node); p; p = next_element_sibling(p))
          if (p->name.view().equals(node->name.view())) return false;
      return true;
    }
    case TestKind::Nth: {
      int64_t position = 1;
      if (t.ref_count > 0 && !match_inner(t, node, scope)) return false;   // (of S: the element must match S itself)
      auto counts = [&](const Node* p) {
        if (t.ref_count > 0) return match_inner(t, p, scope);
        return !t.of_type || p->name.view().equals(node->name.view());
      };
      if (t.from_end) {
        for (const Node* p = next_element_sibling(node); p; p = next_element_sibling(p))
          if (counts(p)) ++position;
      } else {
        for (const Node* p = previous_element_sibling(node); p; p = previous_element_sibling(p))
          if (counts(p)) ++position;
      }
      if (t.a == 0) return position == t.b;
      const int64_t d = position - t.b;
      return d % t.a == 0 && d / t.a >= 0;
    }
    case TestKind::Empty:
      for (const Node* c = node->first_child; c; c = c->next)
        if (c->is_element() || (c->is_text() && !c->data.empty())) return false;
      return true;
    case TestKind::Root: return node->parent && node->parent->type == NodeType::Document;
    case TestKind::Scope: return scope ? node == scope : (node->parent && node->parent->type == NodeType::Document);
    case TestKind::Link: return (node->is_tag("a") || node->is_tag("area") || node->is_tag("link")) && has_attribute(node, "href");
    case TestKind::Checked: {
      if (node->is_tag("input")) {
        const InputType it = input_type(node);
        return (it == InputType::Checkbox || it == InputType::Radio) && is_checked(node);
      }
      return node->is_tag("option") && is_checked(node);
    }
    case TestKind::Focus: return (node->form_state & kFormFocused) != 0;
    case TestKind::FocusWithin:
      if (node->form_state & kFormFocused) return true;
      for (const Node* d = next_in_tree(node, node); d; d = next_in_tree(d, node))
        if (d->is_element() && (d->form_state & kFormFocused)) return true;
      return false;
    case TestKind::Fullscreen: return (node->form_state & kFormFullscreen) != 0;
    case TestKind::Hover: return (node->ui_state & kUiHover) != 0;
    case TestKind::Active: return (node->ui_state & kUiActive) != 0;
    case TestKind::Always: return true;
    case TestKind::Required: return is_submittable(node) && has_attribute(node, "required");
    case TestKind::Optional: return is_submittable(node) && !has_attribute(node, "required");
    case TestKind::Dir: {   // the dir attribute of the element or the nearest ancestor that has one (auto and nothing: left to right)
      bool rtl = false;
      for (const Node* e = node; e && e->is_element(); e = e->parent) {
        StringView d;
        if (!get_attribute(e, "dir", &d)) continue;
        if (d.equals_ignore_case("rtl")) { rtl = true; break; }
        if (d.equals_ignore_case("ltr")) break;
      }
      return rtl == (t.a == 1);
    }
    case TestKind::ReadOnly: return !(editing_host(node) || (node->is_tag("textarea") || node->is_tag("input")) && is_text_field(node->is_tag("input") ? input_type(node) : InputType::Text) &&
                                     !is_read_only(node) && !is_disabled(node));
    case TestKind::ReadWrite: return editing_host(node) || (node->is_tag("textarea") || node->is_tag("input")) && is_text_field(node->is_tag("input") ? input_type(node) : InputType::Text) && !is_read_only(node) && !is_disabled(node);
    case TestKind::Valid: return will_validate(node) && validity(node) == 0;
    case TestKind::Invalid: return will_validate(node) && validity(node) != 0;
    case TestKind::PlaceholderShown: {
      if (!(node->is_tag("textarea") || (node->is_tag("input") && is_text_field(input_type(node)))) || !has_attribute(node, "placeholder")) return false;
      String v(node->owner->allocator());
      get_value(node, &v);
      return v.empty();
    }
    case TestKind::Disabled: return is_form_control(node->name.view()) && is_disabled_control(node);
    case TestKind::Enabled: return is_form_control(node->name.view()) && !is_disabled_control(node);
    case TestKind::Not: return !match_inner(t, node, scope);
    case TestKind::Is: return match_inner(t, node, scope);
    case TestKind::Host: return node->shadow != nullptr;
    case TestKind::HostIs: return node->shadow != nullptr && match_inner(t, node, scope);
    case TestKind::Has: {
      // (approximately: some descendant, child or following sibling is one of the selectors on its own)
      for (uint32_t i = 0; i < t.ref_count; ++i) {
        const Selector& s = selectors_[refs_[t.first_ref + i]];
        const Combinator lead = compounds_[s.first_compound].combinator;
        if (lead == Combinator::Adjacent || lead == Combinator::Sibling) {
          for (const Node* p = next_element_sibling(node); p; p = next_element_sibling(p)) {
            if (match_chain(s, s.compound_count - 1, p, scope)) return true;
            if (lead == Combinator::Adjacent) break;
          }
        } else {
          for (const Node* d = next_in_tree(node, node); d; d = next_in_tree(d, node)) {
            if (!d->is_element()) continue;
            if (lead == Combinator::Child && d->parent != node) continue;
            if (match_chain(s, s.compound_count - 1, d, scope)) return true;
          }
        }
      }
      return false;
    }
    case TestKind::Never: return false;
    case TestKind::Lang: {
      StringView lang;
      bool found = false;
      for (const Node* e = node; e && e->is_element() && !found; e = e->parent) found = get_attribute(e, "lang", &lang) || get_attribute(e, "xml:lang", &lang);
      if (!found) return false;
      const StringView ranges = text(t.value_offset, t.value_length);
      uint64_t from = 0;
      while (from <= ranges.size()) {
        uint64_t to = from;
        while (to < ranges.size() && ranges[to] != '\n') ++to;
        const StringView r = ranges.substr(from, to - from);
        if (r.equals("*") ? !lang.empty() : (equals_fold(lang, r) || (lang.size() > r.size() && lang[r.size()] == '-' && equals_fold(lang.first(r.size()), r)))) return true;
        from = to + 1;
      }
      return false;
    }
  }
  return false;
}

}  // namespace dom

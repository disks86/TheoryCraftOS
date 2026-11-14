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

#include "Program/AppRunScript.hpp"
#include "Program/ShellTools.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"
#include "Utility/UniquePointer.hpp"

// A shell just big enough for AppImage launcher scripts (see AppRunScript.hpp). The script is cut into tokens once; the interpreter then walks the
// tokens with a cursor, parsing and running in one pass (a branch not taken is parsed with `run` false), and jumps back to a remembered position for loops
// and function calls. Nothing here starts a process: the only effect is the AppRunLaunch it fills in.
namespace desktop {

namespace {

constexpr uint64_t npos = StringView::npos;
constexpr uint64_t kMaxScriptBytes = 1u << 20;
constexpr uint32_t kMaxSteps = 200000, kMaxDepth = 24, kMaxLinkHops = 40;
constexpr uint64_t kMaxCapture = 64ull << 20;      // what one command may write into a pipe or a $( ) (a web page, a JSON answer)
constexpr uint64_t kMaxFetch = 32ull << 20;      // what curl may fetch
constexpr int kNotFound = 127;

enum class TokenKind : uint8_t { Word, Op, Newline, Redirect, End };
struct Token {
  TokenKind kind = TokenKind::End;
  uint32_t start = 0, size = 0;
};

bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool is_operator_char(char c) { return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>' || c == '\n'; }
bool is_glob_char(char c) { return c == '*' || c == '?' || c == '['; }

bool is_name(StringView text) {
  if (text.empty() || !is_name_start(text[0])) return false;
  for (char c : text)
    if (!is_name_char(c)) return false;
  return true;
}

bool parse_int(StringView text, int64_t* out) {
  text = text.trimmed();
  bool negative = false;
  if (text.starts_with('-') || text.starts_with('+')) {
    negative = text[0] == '-';
    text = text.substr(1);
  }
  uint64_t value = 0;
  if (!text.parse_uint(&value) || value > (uint64_t{1} << 62)) return false;
  *out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
  return true;
}

// Glob matching (`*`, `?`, `[...]`, `\` escapes), as case patterns, ${X%pattern} and pathname expansion use it.
bool glob_match(StringView pattern, StringView text) {
  uint64_t p = 0, t = 0, star_p = npos, star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size()) {
      const char c = pattern[p];
      if (c == '*') {
        star_p = ++p;
        star_t = t;
        continue;
      }
      if (c == '?') { ++p; ++t; continue; }
      if (c == '[') {
        uint64_t q = p + 1;
        bool negate = false, matched = false;
        if (q < pattern.size() && (pattern[q] == '!' || pattern[q] == '^')) { negate = true; ++q; }
        const uint64_t first = q;
        while (q < pattern.size() && (pattern[q] != ']' || q == first)) {
          if (q + 2 < pattern.size() && pattern[q + 1] == '-' && pattern[q + 2] != ']') {
            if (text[t] >= pattern[q] && text[t] <= pattern[q + 2]) matched = true;
            q += 3;
          } else {
            if (pattern[q] == text[t]) matched = true;
            ++q;
          }
        }
        if (q < pattern.size() && matched != negate) {
          p = q + 1;
          ++t;
          continue;
        }
        if (q >= pattern.size() && text[t] == '[') { ++p; ++t; continue; }      // (an unclosed '[' is itself)
      } else {
        char want = c;
        uint64_t step = 1;
        if (c == '\\' && p + 1 < pattern.size()) { want = pattern[p + 1]; step = 2; }
        if (want == text[t]) { p += step; ++t; continue; }
      }
    }
    if (star_p == npos) return false;
    p = star_p;
    t = ++star_t;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

// The end of a quoted or bracketed piece that starts at `i` (the opening character), or npos if it is not closed.
uint64_t skip_piece(StringView text, uint64_t i);

uint64_t skip_group(StringView text, uint64_t i) {      // text[i] is '(' or '{'
  const char open = text[i], close = open == '(' ? ')' : '}';
  uint32_t depth = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\\') { i += 2; continue; }
    if (c == '\'' || c == '"' || c == '`') {
      i = skip_piece(text, i);
      if (i == npos) return npos;
      continue;
    }
    if (c == open) ++depth;
    else if (c == close && --depth == 0) return i + 1;
    ++i;
  }
  return npos;
}

uint64_t skip_piece(StringView text, uint64_t i) {
  const char c = text[i];
  if (c == '\'') {
    const uint64_t end = text.find('\'', i + 1);
    return end == npos ? npos : end + 1;
  }
  if (c == '`') {
    for (uint64_t j = i + 1; j < text.size(); ++j) {
      if (text[j] == '\\') { ++j; continue; }
      if (text[j] == '`') return j + 1;
    }
    return npos;
  }
  if (c == '"') {
    for (uint64_t j = i + 1; j < text.size();) {
      const char d = text[j];
      if (d == '\\') { j += 2; continue; }
      if (d == '"') return j + 1;
      if (d == '`') {
        j = skip_piece(text, j);
        if (j == npos) return npos;
        continue;
      }
      if (d == '$' && j + 1 < text.size() && (text[j + 1] == '(' || text[j + 1] == '{')) {
        j = skip_group(text, j + 1);
        if (j == npos) return npos;
        continue;
      }
      ++j;
    }
    return npos;
  }
  return skip_group(text, i);
}

struct Script {
  explicit Script(allocators::IAllocator& allocator) : text(allocator), tokens(allocator) {}
  String text;
  List<Token> tokens;
  StringView at(const Token& token) const { return text.view().substr(token.start, token.size); }
};

// Cuts a script into tokens. False if a quote or a bracket is never closed.
bool tokenize(Script* script) {
  const StringView text = script->text.view();
  String heredoc(script->text.allocator());
  bool heredoc_strip = false;
  auto push = [&](TokenKind kind, uint64_t start, uint64_t end) {
    Token token;
    token.kind = kind;
    token.start = static_cast<uint32_t>(start);
    token.size = static_cast<uint32_t>(end - start);
    return script->tokens.push_back(token);
  };
  if (text.size() > kMaxScriptBytes) return false;
  uint64_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (is_blank(c)) { ++i; continue; }
    if (c == '\\' && i + 1 < text.size() && text[i + 1] == '\n') { i += 2; continue; }
    if (c == '#') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (c == '\n') {
      if (!push(TokenKind::Newline, i, i + 1)) return false;
      ++i;
      if (!heredoc.empty()) {      // the body of a here-document: skipped up to its delimiter line
        while (i < text.size()) {
          uint64_t end = text.find('\n', i);
          if (end == npos) end = text.size();
          StringView line = text.substr(i, end - i);
          if (heredoc_strip) while (!line.empty() && line[0] == '\t') line = line.substr(1);
          i = end < text.size() ? end + 1 : end;
          if (line.equals(heredoc.view())) break;
        }
        heredoc.clear();
      }
      continue;
    }
    // Redirections: [n]> [n]>> [n]< <> >& &> &>> << <<- <<<, with their target.
    uint64_t r = i;
    while (r < text.size() && is_digit(text[r])) ++r;
    const bool redirect = (r < text.size() && (text[r] == '<' || text[r] == '>')) || (c == '&' && i + 1 < text.size() && text[i + 1] == '>');
    if (redirect && (r == i || r < text.size())) {
      const uint64_t start = i;
      uint64_t j = r == i ? i : r;
      if (text[j] == '&') ++j;
      bool here = false;
      if (j + 1 < text.size() && text[j] == '<' && text[j + 1] == '<') {
        j += 2;
        if (j < text.size() && text[j] == '<') ++j;      // <<< word: an ordinary target
        else {
          here = true;
          heredoc_strip = j < text.size() && text[j] == '-';
          if (heredoc_strip) ++j;
        }
      } else {
        ++j;
        while (j < text.size() && (text[j] == '>' || text[j] == '<' || text[j] == '|')) ++j;
      }
      if (j < text.size() && text[j] == '&') {
        ++j;
        while (j < text.size() && (is_digit(text[j]) || text[j] == '-')) ++j;
      } else {
        while (j < text.size() && is_blank(text[j])) ++j;
        const uint64_t word = j;
        while (j < text.size() && !is_blank(text[j]) && !is_operator_char(text[j])) {
          if (text[j] == '\'' || text[j] == '"') {
            j = skip_piece(text, j);
            if (j == npos) return false;
          } else {
            ++j;
          }
        }
        if (here) {
          for (char d : text.substr(word, j - word))
            if (d != '\'' && d != '"' && d != '\\' && !heredoc.push_back(d)) return false;
        }
      }
      if (!push(TokenKind::Redirect, start, j)) return false;
      i = j;
      continue;
    }
    if (is_operator_char(c)) {
      uint64_t j = i + 1;
      if (j < text.size() && ((c == '&' && text[j] == '&') || (c == '|' && text[j] == '|') || (c == ';' && text[j] == ';'))) ++j;
      if (!push(TokenKind::Op, i, j)) return false;
      i = j;
      continue;
    }
    // A word.
    const uint64_t start = i;
    while (i < text.size() && !is_blank(text[i]) && !is_operator_char(text[i])) {
      const char d = text[i];
      if (d == '\\') { i += 2; continue; }
      if (d == '\'' || d == '"' || d == '`') {
        i = skip_piece(text, i);
        if (i == npos) return false;
        continue;
      }
      if (d == '$' && i + 1 < text.size() && (text[i + 1] == '(' || text[i + 1] == '{')) {
        i = skip_group(text, i + 1);
        if (i == npos) return false;
        continue;
      }
      if (d == '=' && i + 1 < text.size() && text[i + 1] == '(') {      // NAME=( ... ) or NAME+=( ... ): an array
        StringView name = text.substr(start, i - start);
        if (name.ends_with('+')) name = name.first(name.size() - 1);
        if (is_name(name)) {
          i = skip_group(text, i + 1);
          if (i == npos) return false;
          continue;
        }
      }
      ++i;
    }
    if (i > text.size()) i = text.size();
    if (!push(TokenKind::Word, start, i)) return false;
  }
  return push(TokenKind::End, text.size(), text.size());
}

struct Var {
  explicit Var(allocators::IAllocator& allocator) : name(allocator), value(allocator), items(allocator) {}
  String name, value;
  List<String> items;
  bool array = false, exported = false, is_set = true;
};

struct Function {
  explicit Function(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  const Script* script = nullptr;
  uint64_t begin = 0, end = 0;
};

struct Cursor {
  const Script* script;
  uint64_t pos, end;
};

class Shell {
 public:
  Shell(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView root, AppRunLaunch* launch)
      : allocator_(allocator), files_(files), root_(root), launch_(launch), vars_(allocator), functions_(allocator), scripts_(allocator), zero_(allocator),
        positional_(allocator), cwd_(allocator), trap_(allocator), appdir_(allocator) {}

  bool init(ArrayView<const StringView> arguments, ArrayView<const StringView> environment) {
    for (const StringView entry : environment) {
      StringView name, value;
      if (!entry.split_once('=', &name, &value) || !is_name(name)) continue;
      Var* var = set_var(name, value);
      if (!var) return false;
      var->exported = true;
    }
    for (const StringView argument : arguments) {
      String copy(allocator_);
      if (!copy.assign(argument) || !positional_.push_back(util::move(copy))) return false;
    }
    if (const Var* dir = find_var("APPDIR")) {
      if (!appdir_.assign(dir->value.view())) return false;
    }
    if (const Var* pwd = find_var("PWD"); pwd && pwd->value.view().starts_with('/')) return cwd_.assign(pwd->value.view());
    return cwd_.assign("/");
  }

  // Runs the program file at `path` (an ELF: the launch; a script: interpreted). `exec` says whether the script hands over to it.
  int run_file(StringView path, ArrayView<String> argv, bool exec) {
    String head(allocator_);
    if (!read_file(path, 4096, &head)) return kNotFound;
    if (head.view().starts_with("\x7F" "ELF")) {
      if (no_launch_) {
        halt(false);
        return 0;
      }
      if (!launch(path, argv)) halt(true);
      return 0;
    }
    // A script: interpreted, as its own program (its own $0 and arguments).
    String saved_zero(allocator_);
    List<String> saved(allocator_);
    if (!saved_zero.assign(zero_.view())) return fail_memory();
    for (String& arg : positional_.view()) {
      String copy(allocator_);
      if (!copy.assign(arg.view()) || !saved.push_back(util::move(copy))) return fail_memory();
    }
    if (!zero_.assign(path)) return fail_memory();
    positional_.clear();
    for (uint64_t i = 1; i < argv.size(); ++i) {
      String copy(allocator_);
      if (!copy.assign(argv[i].view()) || !positional_.push_back(util::move(copy))) return fail_memory();
    }
    const int status = source(path, true);
    if (exec || halted_) {
      halt(false);
      return status;
    }
    zero_ = util::move(saved_zero);
    if (!restore(saved)) return fail_memory();
    return status;
  }

  bool out_of_memory() const { return out_of_memory_; }
  bool launched() const { return launched_; }

  // For evaluate_script_variable: no program is ever started, curl goes through `network`.
  void set_network(IScriptNetwork* network) { network_ = network; }
  void forbid_launch() { no_launch_ = true; }
  int run_program_text(StringView text) {
    UniquePointer<Script> script = UniquePointer<Script>::make(allocator_, allocator_);
    if (!script || !script->text.assign(text)) return fail_memory();
    return run_text(util::move(script), true);
  }
  bool variable(StringView name, String* out) const {
    const Var* var = find_var(name);
    if (!var || !var->is_set) return false;
    return out->assign(var->array ? (var->items.count() ? var->items.view()[0].view() : StringView()) : var->value.view());
  }

 private:
  // ---------------------------------------------------------------- variables
  Var* find_var(StringView name) const {
    for (const UniquePointer<Var>& var : vars_.view())
      if (var->name.view().equals(name)) return var.get();
    return nullptr;
  }
  Var* make_var(StringView name) {
    if (Var* var = find_var(name)) return var;
    UniquePointer<Var> var = UniquePointer<Var>::make(allocator_, allocator_);
    if (!var || !var->name.assign(name)) return nullptr;
    Var* raw = var.get();
    if (!vars_.push_back(util::move(var))) return nullptr;
    return raw;
  }
  Var* set_var(StringView name, StringView value) {
    Var* var = make_var(name);
    if (!var || !var->value.assign(value)) return nullptr;
    var->items.clear();
    var->array = false;
    var->is_set = true;
    return var;
  }
  void unset_var(StringView name) {
    for (uint64_t i = 0; i < vars_.count(); ++i) {
      if (vars_.view()[i]->name.view().equals(name)) {
        vars_.remove_at(i);
        return;
      }
    }
  }

  // What a subshell ($( ) and ( )) may change without the shell around it seeing it: the variables, the positional parameters and the current folder.
  // (`$(set -- "$APPDIR"/*.desktop; echo "${1##*/}")`, which sharun's hooks use, must leave "$@" as it was.)
  struct Snapshot {
    explicit Snapshot(allocators::IAllocator& allocator) : vars(allocator), positional(allocator), cwd(allocator) {}
    List<UniquePointer<Var>> vars;
    List<String> positional;
    String cwd;
  };
  bool take_snapshot(Snapshot* snapshot) {
    for (const UniquePointer<Var>& var : vars_.view()) {
      UniquePointer<Var> copy = UniquePointer<Var>::make(allocator_, allocator_);
      if (!copy || !copy->name.assign(var->name.view()) || !copy->value.assign(var->value.view())) return false;
      for (const String& item : var->items.view()) {
        String text(allocator_);
        if (!text.assign(item.view()) || !copy->items.push_back(util::move(text))) return false;
      }
      copy->array = var->array;
      copy->exported = var->exported;
      copy->is_set = var->is_set;
      if (!snapshot->vars.push_back(util::move(copy))) return false;
    }
    for (const String& arg : positional_.view()) {
      String copy(allocator_);
      if (!copy.assign(arg.view()) || !snapshot->positional.push_back(util::move(copy))) return false;
    }
    return snapshot->cwd.assign(cwd_.view());
  }
  bool restore_snapshot(Snapshot& snapshot) {
    vars_.clear();
    for (UniquePointer<Var>& var : snapshot.vars.view())
      if (!vars_.push_back(util::move(var))) return false;
    return restore(snapshot.positional) && cwd_.assign(snapshot.cwd.view());
  }

  // The positional parameters back as they were (List cannot be assigned).
  bool restore(List<String>& saved) {
    positional_.clear();
    for (String& arg : saved.view())
      if (!positional_.push_back(util::move(arg))) return false;
    return true;
  }
  int fail_memory() {
    out_of_memory_ = true;
    halt(false);
    return 1;
  }
  void halt(bool memory) {
    if (memory) out_of_memory_ = true;
    halted_ = true;
  }

  // ---------------------------------------------------------------- files
  bool host(StringView path, String* out) const {
    if (!out->assign(root_)) return false;
    if (path.equals("/")) return true;
    return out->append(path);
  }
  // Makes `path` absolute (against the current folder) and removes ".", ".." and repeated slashes, without looking at links.
  bool absolute(StringView path, String* out) const {
    String joined(allocator_);
    if (!path.starts_with('/')) {
      if (!joined.assign(cwd_.view()) || !joined.push_back('/')) return false;
    }
    if (!joined.append(path) || !out->assign("")) return false;
    StringView rest = joined.view();
    StringView part;
    while (rest.next_token('/', &part)) {
      if (part.empty() || part.equals(".")) continue;
      if (part.equals("..")) {
        const uint64_t slash = out->view().rfind('/');
        out->truncate(slash == npos ? 0 : slash);
        continue;
      }
      if (!out->push_back('/') || !out->append(part)) return false;
    }
    return out->empty() ? out->assign("/") : true;
  }
  // readlink -f: every symbolic link resolved (the last name need not exist).
  bool canonical(StringView path, String* out) const {
    String pending(allocator_), result(allocator_), target(allocator_), probe(allocator_), next(allocator_);
    if (!absolute(path, &pending) || !result.assign("")) return false;
    uint32_t hops = 0;
    uint64_t pos = 0;
    while (pos < pending.size()) {
      while (pos < pending.size() && pending[pos] == '/') ++pos;
      uint64_t end = pos;
      while (end < pending.size() && pending[end] != '/') ++end;
      const StringView part = pending.view().substr(pos, end - pos);
      if (part.empty() || part.equals(".")) { pos = end; continue; }
      if (part.equals("..")) {
        const uint64_t slash = result.view().rfind('/');
        result.truncate(slash == npos ? 0 : slash);
        pos = end;
        continue;
      }
      if (!probe.assign(result.view()) || !probe.push_back('/') || !probe.append(part)) return false;
      String host_path(allocator_);
      bool is_directory = false;
      if (hops < kMaxLinkHops && host(probe.view(), &host_path) && files_.read_symbolic_link(host_path.view(), &target, &is_directory) == filesystem::Status::Ok && !target.empty()) {
        ++hops;
        if (!next.assign(target.view()) || !next.push_back('/') || !next.append(pending.view().substr(end))) return false;
        if (target.view().starts_with('/')) result.clear();
        pending = util::move(next);
        next = String(allocator_);
        pos = 0;
        continue;
      }
      result = util::move(probe);
      probe = String(allocator_);
      pos = end;
    }
    return result.empty() ? out->assign("/") : out->assign(result.view());
  }
  bool stat(StringView path, filesystem::DirectoryEntry* entry) const {
    String full(allocator_), host_path(allocator_);
    return absolute(path, &full) && host(full.view(), &host_path) && files_.stat(host_path.view(), entry) == filesystem::Status::Ok;
  }
  bool read_file(StringView path, uint64_t limit, String* out) const {
    String full(allocator_), host_path(allocator_);
    if (!absolute(path, &full) || !host(full.view(), &host_path)) return false;
    UniquePointer<filesystem::IFile> file;
    if (files_.open_file(host_path.view(), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return false;
    const uint64_t size = file->size() < limit ? file->size() : limit;
    ByteBuffer bytes(allocator_);
    if (!bytes.put_zeros(size)) return false;
    uint64_t done = 0;
    while (done < size) {
      uint64_t got = 0;
      if (file->read(done, bytes.writable_view().subview(done), &got) != filesystem::Status::Ok || got == 0) break;
      done += got;
    }
    return out->assign(StringView(bytes.view().first(done)));
  }
  bool is_file(StringView path) const {
    filesystem::DirectoryEntry entry;
    return stat(path, &entry) && entry.type == filesystem::NodeType::File;
  }
  // Where a command name leads: a path if it has a slash, else the first match along $PATH. Empty if nowhere.
  bool find_program(StringView name, String* out) const {
    out->clear();
    if (name.empty()) return true;
    if (name.contains('/')) return is_file(name) ? absolute(name, out) : true;
    const Var* path = find_var("PATH");
    StringView rest = path ? path->value.view() : StringView("/usr/local/bin:/usr/bin:/bin");
    StringView dir;
    String candidate(allocator_);
    while (rest.next_token(':', &dir)) {
      if (!candidate.assign(dir.empty() ? StringView(".") : dir) || !candidate.push_back('/') || !candidate.append(name)) return false;
      if (is_file(candidate.view())) return absolute(candidate.view(), out);
    }
    return true;
  }

  // ---------------------------------------------------------------- the hand-over
  bool launch(StringView path, ArrayView<String> argv) {
    if (!canonical(path, &launch_->program)) return false;
    launch_->arguments.clear();
    for (uint64_t i = 1; i < argv.size(); ++i) {
      String copy(allocator_);
      if (!copy.assign(argv[i].view()) || !launch_->arguments.push_back(util::move(copy))) return false;
    }
    launch_->environment.clear();
    for (const UniquePointer<Var>& var : vars_.view()) {
      if (!var->exported || !var->is_set) continue;
      String entry(allocator_);
      const StringView value = var->array ? (var->items.count() ? var->items.view()[0].view() : StringView()) : var->value.view();
      if (!entry.assign(var->name.view()) || !entry.push_back('=') || !entry.append(value) || !launch_->environment.push_back(util::move(entry))) return false;
    }
    launched_ = true;
    halted_ = true;
    return true;
  }

  // ---------------------------------------------------------------- scripts
  // Interprets the file at `path` in this shell (source / .), or as the program the shell runs (`program`: the EXIT trap runs when it ends).
  int source(StringView path, bool program) {
    if (depth_ >= kMaxDepth) return 1;
    UniquePointer<Script> script = UniquePointer<Script>::make(allocator_, allocator_);
    if (!script) return fail_memory();
    if (!read_file(path, kMaxScriptBytes, &script->text)) return 1;
    return run_text(util::move(script), program);
  }
  int eval(StringView text) {
    UniquePointer<Script> script = UniquePointer<Script>::make(allocator_, allocator_);
    if (!script || !script->text.assign(text)) return fail_memory();
    return run_text(util::move(script), false);
  }
  int run_text(UniquePointer<Script> script, bool program) {
    if (!tokenize(script.get())) return 2;
    const Script* raw = script.get();
    if (!scripts_.push_back(util::move(script))) return fail_memory();      // (kept: functions defined in it point into it)
    ++depth_;
    Cursor c{raw, 0, raw->tokens.count()};
    int status = list(c, true);
    returning_ = false;
    if (program && !launched_ && !trap_.empty() && !out_of_memory_) {
      String command(allocator_);
      if (!command.assign(trap_.view())) { --depth_; return fail_memory(); }
      trap_.clear();
      halted_ = false;
      status = eval(command.view());
    }
    --depth_;
    return status;
  }

  // ---------------------------------------------------------------- parsing and running
  const Token& token(const Cursor& c) const {
    static const Token end;
    return c.pos < c.end ? c.script->tokens.view()[c.pos] : end;
  }
  StringView text(const Cursor& c) const { return c.pos < c.end ? c.script->at(token(c)) : StringView(); }
  bool word_is(const Cursor& c, StringView keyword) const { return token(c).kind == TokenKind::Word && text(c).equals(keyword); }
  bool op_is(const Cursor& c, StringView op) const { return token(c).kind == TokenKind::Op && text(c).equals(op); }
  bool at_stop(const Cursor& c) const {
    const Token& t = token(c);
    if (t.kind == TokenKind::End) return true;
    if (t.kind == TokenKind::Op) return text(c).equals(")") || text(c).equals(";;");
    if (t.kind != TokenKind::Word) return false;
    const StringView w = text(c);
    return w.equals("then") || w.equals("elif") || w.equals("else") || w.equals("fi") || w.equals("do") || w.equals("done") || w.equals("esac") || w.equals("}");
  }
  void skip_separators(Cursor& c) const {
    while (token(c).kind == TokenKind::Newline || op_is(c, ";") || op_is(c, "&")) ++c.pos;
  }
  void skip_newlines(Cursor& c) const {
    while (token(c).kind == TokenKind::Newline) ++c.pos;
  }
  bool expect(Cursor& c, StringView keyword) {
    skip_separators(c);
    if (!word_is(c, keyword)) {
      syntax_ = true;
      return false;
    }
    ++c.pos;
    return true;
  }
  bool live(bool run) const { return run && !halted_ && !breaking_ && !continuing_ && !returning_ && !syntax_; }

  int list(Cursor& c, bool run) {
    int status = 0;
    for (;;) {
      skip_separators(c);
      if (at_stop(c) || syntax_ || (halted_ && run)) return status;
      const bool was_live = live(run);
      const int s = and_or(c, run);
      if (was_live) status = s;
      if (halted_ && run) return status;
    }
  }
  int and_or(Cursor& c, bool run) {
    int status = pipeline(c, run);
    for (;;) {
      const bool is_and = op_is(c, "&&"), is_or = op_is(c, "||");
      if (!is_and && !is_or) return status;
      ++c.pos;
      skip_newlines(c);
      const bool take = live(run) && (is_and ? status == 0 : status != 0);
      const int s = pipeline(c, take);
      if (take) status = s;
    }
  }
  // Each stage writes into a buffer of its own, which is the next stage's input; the last one's goes where the pipeline's output goes.
  int pipeline(Cursor& c, bool run) {
    bool negate = false;
    if (word_is(c, "!")) { negate = true; ++c.pos; }
    String* const outer = capture_;
    const StringView outer_input = input_;
    String stage(allocator_), previous(allocator_);
    int status = 0;
    for (;;) {
      stage.clear();
      capture_ = &stage;
      status = command(c, run);
      if (!op_is(c, "|") && !op_is(c, "|&")) break;
      ++c.pos;
      skip_newlines(c);
      previous = util::move(stage);
      stage = String(allocator_);
      input_ = previous.view();
    }
    capture_ = outer;
    input_ = outer_input;
    output(stage.view());
    if (negate) status = status == 0 ? 1 : 0;
    if (run && !syntax_) last_status_ = status;
    return status;
  }

  int command(Cursor& c, bool run) {
    if (++steps_ > kMaxSteps) { halt(false); return 1; }
    const Token& t = token(c);
    if (t.kind == TokenKind::Word) {
      const StringView w = text(c);
      if (w.equals("if")) return if_clause(c, run);
      if (w.equals("while") || w.equals("until")) return while_clause(c, run);
      if (w.equals("for")) return for_clause(c, run);
      if (w.equals("case")) return case_clause(c);
      if (w.equals("{")) {
        ++c.pos;
        const int status = list(c, run);
        expect(c, "}");
        skip_redirects(c);
        return status;
      }
      if (w.equals("function")) {
        ++c.pos;
        return function_definition(c, run);
      }
      Cursor ahead = c;
      ++ahead.pos;
      if (is_name(w) && op_is(ahead, "(")) {
        ++ahead.pos;
        if (op_is(ahead, ")")) return function_definition(c, run);
      }
    }
    if (op_is(c, "(")) {      // a subshell: what it changes stays in it
      ++c.pos;
      Snapshot snapshot(allocator_);
      if (run && !take_snapshot(&snapshot)) return fail_memory();
      const int status = list(c, run);
      if (run && !launched_ && !restore_snapshot(snapshot)) return fail_memory();
      if (op_is(c, ")")) ++c.pos;
      else syntax_ = true;
      skip_redirects(c);
      return status;
    }
    return simple_command(c, run);
  }
  void skip_redirects(Cursor& c) const {
    while (token(c).kind == TokenKind::Redirect) ++c.pos;
  }

  int if_clause(Cursor& c, bool run) {
    ++c.pos;
    int status = list(c, run);
    if (!expect(c, "then")) return 2;
    bool done = live(run) && status == 0;
    int result = 0;
    const int body = list(c, done);
    if (done) result = body;
    for (;;) {
      skip_separators(c);
      if (word_is(c, "elif")) {
        ++c.pos;
        const bool test = live(run) && !done;
        status = list(c, test);
        if (!expect(c, "then")) return 2;
        const bool take = test && status == 0;
        const int s = list(c, take);
        if (take) { result = s; done = true; }
        continue;
      }
      if (word_is(c, "else")) {
        ++c.pos;
        const bool take = live(run) && !done;
        const int s = list(c, take);
        if (take) result = s;
        continue;
      }
      break;
    }
    expect(c, "fi");
    skip_redirects(c);
    return result;
  }

  int while_clause(Cursor& c, bool run) {
    const bool until = word_is(c, "until");
    ++c.pos;
    const uint64_t start = c.pos;
    int result = 0;
    for (bool first = true;; first = false) {
      c.pos = start;
      const bool test = live(run);
      const int status = list(c, test);
      if (!expect(c, "do")) return 2;
      const bool take = test && ((status == 0) != until);
      const int s = list(c, take);
      if (take) result = s;
      expect(c, "done");
      continuing_ = false;
      if (!take || breaking_ || halted_ || syntax_ || ++steps_ > kMaxSteps) break;
      (void)first;
    }
    breaking_ = false;
    skip_redirects(c);
    return result;
  }

  int for_clause(Cursor& c, bool run) {
    ++c.pos;
    if (token(c).kind != TokenKind::Word) { syntax_ = true; return 2; }
    const StringView name = text(c);
    ++c.pos;
    List<String> values(allocator_);
    bool have_in = false;
    skip_newlines(c);
    if (word_is(c, "in")) {
      have_in = true;
      ++c.pos;
      while (token(c).kind == TokenKind::Word) {
        if (live(run) && !expand(text(c), Mode::Fields, &values)) return fail_memory();
        ++c.pos;
      }
    }
    if (!have_in && live(run)) {
      for (String& arg : positional_.view()) {
        String copy(allocator_);
        if (!copy.assign(arg.view()) || !values.push_back(util::move(copy))) return fail_memory();
      }
    }
    if (!expect(c, "do")) return 2;
    const uint64_t body = c.pos;
    int result = 0;
    if (!live(run) || values.count() == 0) {
      list(c, false);
    } else {
      for (String& value : values.view()) {
        c.pos = body;
        if (!set_var(name, value.view())) return fail_memory();
        result = list(c, live(run));
        continuing_ = false;
        if (breaking_ || halted_ || syntax_) break;
        if (!word_is(c, "done")) skip_separators(c);
      }
      if (breaking_ || halted_) {
        c.pos = body;
        breaking_ = false;
        list(c, false);
      }
    }
    expect(c, "done");
    breaking_ = false;
    skip_redirects(c);
    return result;
  }

  // case ... esac is not run (the launchers use it for option parsing that does not decide the program); it is skipped as a whole.
  int case_clause(Cursor& c) {
    uint32_t depth = 0;
    while (token(c).kind != TokenKind::End) {
      if (token(c).kind == TokenKind::Word) {
        if (text(c).equals("case")) ++depth;
        else if (text(c).equals("esac") && --depth == 0) {
          ++c.pos;
          return 0;
        }
      }
      ++c.pos;
    }
    syntax_ = true;
    return 2;
  }

  int function_definition(Cursor& c, bool run) {
    if (token(c).kind != TokenKind::Word) { syntax_ = true; return 2; }
    const StringView name = text(c);
    ++c.pos;
    if (op_is(c, "(")) {
      ++c.pos;
      if (!op_is(c, ")")) { syntax_ = true; return 2; }
      ++c.pos;
    }
    skip_newlines(c);
    const uint64_t begin = c.pos;
    command(c, false);
    if (!live(run)) return 0;
    Function* function = nullptr;
    for (UniquePointer<Function>& f : functions_.view())
      if (f->name.view().equals(name)) function = f.get();
    if (!function) {
      UniquePointer<Function> made = UniquePointer<Function>::make(allocator_, allocator_);
      if (!made || !made->name.assign(name)) return fail_memory();
      function = made.get();
      if (!functions_.push_back(util::move(made))) return fail_memory();
    }
    function->script = c.script;
    function->begin = begin;
    function->end = c.pos;
    return 0;
  }
  const Function* find_function(StringView name) const {
    for (const UniquePointer<Function>& f : functions_.view())
      if (f->name.view().equals(name)) return f.get();
    return nullptr;
  }

  int simple_command(Cursor& c, bool run) {
    const uint64_t first = c.pos;
    bool test_brackets = word_is(c, "[[");
    while (true) {
      const Token& t = token(c);
      if (t.kind == TokenKind::End || t.kind == TokenKind::Newline) break;
      if (t.kind == TokenKind::Op) {
        if (!test_brackets) break;
      }
      if (test_brackets && t.kind == TokenKind::Word && text(c).equals("]]")) {
        ++c.pos;
        test_brackets = false;
        continue;
      }
      ++c.pos;
      if (test_brackets) continue;
    }
    if (c.pos == first) {
      // A stray operator: step over it so the walk always moves.
      if (token(c).kind == TokenKind::Op && !op_is(c, ")") && !op_is(c, ";;")) ++c.pos;
      return 0;
    }
    if (!live(run)) return 0;

    // Assignments before the command name, then the words.
    Cursor w{c.script, first, c.pos};
    List<String> assignments(allocator_);
    while (w.pos < w.end && token(w).kind == TokenKind::Word && is_assignment(text(w))) {
      String copy(allocator_);
      if (!copy.assign(text(w)) || !assignments.push_back(util::move(copy))) return fail_memory();
      ++w.pos;
    }
    List<String> argv(allocator_);
    const bool brackets = w.pos < w.end && text(w).equals("[[");
    for (; w.pos < w.end; ++w.pos) {
      const Token& t = token(w);
      if (t.kind == TokenKind::Redirect) continue;
      if (t.kind == TokenKind::Op) {      // (inside [[ ]] only)
        String copy(allocator_);
        if (!copy.assign(text(w)) || !argv.push_back(util::move(copy))) return fail_memory();
        continue;
      }
      if (!expand(text(w), brackets ? Mode::Single : Mode::Fields, &argv)) {
        if (out_of_memory_) return 1;
        return kNotFound;      // a construct this shell cannot expand: the command is not run
      }
    }
    if (argv.count() == 0) {
      for (String& assignment : assignments.view())
        if (!assign(assignment.view(), false)) return out_of_memory_ ? 1 : 1;
      return last_substitution_;
    }
    return dispatch(argv, assignments);
  }

  static bool is_assignment(StringView word) {
    const uint64_t eq = word.find('=');
    if (eq == npos || eq == 0) return false;
    StringView name = word.first(eq);
    if (name.ends_with('+')) name = name.first(name.size() - 1);
    if (name.ends_with(']')) {      // NAME[i]=value
      const uint64_t open = name.find('[');
      if (open == npos) return false;
      name = name.first(open);
    }
    return is_name(name);
  }

  // NAME=value, NAME+=value, NAME=(a b c), NAME+=(d). False if the value could not be expanded (the variable is left alone) or memory ran out.
  bool assign(StringView word, bool export_it) {
    const uint64_t eq = word.find('=');
    StringView name = word.first(eq);
    const StringView value = word.substr(eq + 1);
    const bool append = name.ends_with('+');
    if (append) name = name.first(name.size() - 1);
    if (name.ends_with(']')) name = name.first(name.find('['));      // (an element: the whole variable is replaced by it)
    if (value.starts_with('(') && value.ends_with(')')) {
      List<String> items(allocator_);
      const StringView inner = value.substr(1, value.size() - 2);
      UniquePointer<Script> words = UniquePointer<Script>::make(allocator_, allocator_);
      if (!words || !words->text.assign(inner)) return fail_memory() == 0;
      if (!tokenize(words.get())) return false;
      for (const Token& t : words->tokens.view())
        if (t.kind == TokenKind::Word && !expand(words->at(t), Mode::Fields, &items)) return false;
      Var* var = make_var(name);
      if (!var) return fail_memory() == 0;
      if (!append || !var->array) {
        if (!append || !var->is_set) var->items.clear();
        else {
          String old(allocator_);
          if (!old.assign(var->value.view())) return fail_memory() == 0;
          var->items.clear();
          if (!var->items.push_back(util::move(old))) return fail_memory() == 0;
        }
      }
      for (String& item : items.view())
        if (!var->items.push_back(util::move(item))) return fail_memory() == 0;
      var->array = true;
      var->is_set = true;
      if (export_it) var->exported = true;
      return true;
    }
    List<String> fields(allocator_);
    if (!expand(value, Mode::Assign, &fields)) return false;
    const StringView expanded = fields.count() ? fields.view()[0].view() : StringView();
    Var* var = make_var(name);
    if (!var) return fail_memory() == 0;
    if (append && var->is_set) {
      if (var->array) {
        String copy(allocator_);
        if (!copy.assign(expanded) || !var->items.push_back(util::move(copy))) return fail_memory() == 0;
      } else if (!var->value.append(expanded)) {
        return fail_memory() == 0;
      }
    } else {
      if (!var->value.assign(expanded)) return fail_memory() == 0;
      var->items.clear();
      var->array = false;
    }
    var->is_set = true;
    if (export_it) var->exported = true;
    return true;
  }

  void output(StringView text) {
    if (capture_ && text.size() <= kMaxCapture - capture_->size()) {
      if (!capture_->append(text)) out_of_memory_ = true;
    }
  }

  // ---------------------------------------------------------------- commands
  int dispatch(List<String>& argv, List<String>& assignments) {
    const StringView name = argv.view()[0].view();
    ArrayView<String> args = argv.view();
    auto arg = [&](uint64_t i) { return i < args.size() ? args[i].view() : StringView(); };

    if (name.equals("exec")) {
      if (args.size() == 1) return 0;      // (only redirections)
      uint64_t skip = 1;
      while (skip < args.size() && arg(skip).starts_with('-') && arg(skip).size() > 1) ++skip;      // exec -a name / -c / -l
      if (skip < args.size() && args[skip - 1].view().equals("-a")) ++skip;
      if (skip >= args.size()) return 0;
      for (String& assignment : assignments.view()) assign(assignment.view(), true);
      String program(allocator_);
      if (!find_program(arg(skip), &program)) return fail_memory();
      if (program.empty()) {
        halt(false);      // a failed exec ends a non-interactive shell
        return kNotFound;
      }
      return run_file(program.view(), args.subview(skip), true);
    }
    if (name.equals(":") || name.equals("true") || (name.equals("set") && arg(1) != "--") || name.equals("shopt") || name.equals("ulimit") || name.equals("umask") ||
        name.equals("hash") || name.equals("alias") || name.equals("unalias") || name.equals("wait") || name.equals("mkdir") || name.equals("builtin"))
      return 0;
    if (name.equals("false") || name.equals("read") || name.equals("getopts")) return 1;
    if (name.equals("set")) {      // set -- args
      positional_.clear();
      for (uint64_t i = 2; i < args.size(); ++i) {
        String copy(allocator_);
        if (!copy.assign(arg(i)) || !positional_.push_back(util::move(copy))) return fail_memory();
      }
      return 0;
    }
    if (name.equals("export") || name.equals("readonly") || name.equals("local") || name.equals("declare") || name.equals("typeset")) {
      bool export_it = name.equals("export");
      for (uint64_t i = 1; i < args.size(); ++i) {
        const StringView a = arg(i);
        if (a.starts_with('-')) {
          if (a.contains('x')) export_it = true;
          continue;
        }
        if (a.contains('=')) {
          if (!assign_literal(a, export_it)) return out_of_memory_ ? 1 : 1;
        } else if (export_it && is_name(a)) {
          const bool existed = find_var(a) != nullptr;
          Var* var = make_var(a);
          if (!var) return fail_memory();
          if (!existed) var->is_set = false;      // (exported, but still unset: not in the environment until it gets a value)
          var->exported = true;
        } else if (is_name(a) && !find_var(a)) {
          Var* var = make_var(a);
          if (!var) return fail_memory();
          var->is_set = false;
        }
      }
      return 0;
    }
    if (name.equals("unset")) {
      bool functions = false;
      for (uint64_t i = 1; i < args.size(); ++i) {
        if (arg(i).equals("-f")) { functions = true; continue; }
        if (arg(i).starts_with('-')) continue;
        if (functions) {
          for (uint64_t f = 0; f < functions_.count(); ++f)
            if (functions_.view()[f]->name.view().equals(arg(i))) { functions_.remove_at(f); break; }
        } else {
          unset_var(arg(i));
        }
      }
      return 0;
    }
    if (name.equals("shift")) {
      int64_t n = 1;
      if (args.size() > 1 && !parse_int(arg(1), &n)) return 1;
      if (n < 0 || uint64_t(n) > positional_.count()) return 1;
      for (int64_t i = 0; i < n; ++i) positional_.remove_at(0);
      return 0;
    }
    if (name.equals("cd")) {
      StringView target = arg(1);
      if (target.empty()) {
        const Var* home = find_var("HOME");
        target = home ? home->value.view() : StringView("/");
      }
      String full(allocator_);
      if (!absolute(target, &full)) return fail_memory();
      filesystem::DirectoryEntry entry;
      if (!stat(full.view(), &entry) || entry.type != filesystem::NodeType::Directory) return 1;
      cwd_ = util::move(full);
      if (!set_var("PWD", cwd_.view())) return fail_memory();
      return 0;
    }
    if (name.equals("source") || name.equals(".")) {
      if (args.size() < 2) return 2;
      String path(allocator_);
      if (!find_program(arg(1), &path)) return fail_memory();
      if (path.empty() && !absolute(arg(1), &path)) return fail_memory();
      return source(path.view(), false);
    }
    if (name.equals("eval")) {
      String text(allocator_);
      for (uint64_t i = 1; i < args.size(); ++i)
        if ((i > 1 && !text.push_back(' ')) || !text.append(arg(i))) return fail_memory();
      return eval(text.view());
    }
    if (name.equals("exit")) {
      int64_t code = last_status_;
      if (args.size() > 1) parse_int(arg(1), &code);
      halted_ = true;      // (the EXIT trap still runs: run_text)
      return static_cast<int>(code & 0xFF);
    }
    if (name.equals("return")) {
      int64_t code = last_status_;
      if (args.size() > 1) parse_int(arg(1), &code);
      returning_ = true;
      return static_cast<int>(code & 0xFF);
    }
    if (name.equals("break")) { breaking_ = true; return 0; }
    if (name.equals("continue")) { continuing_ = true; return 0; }
    if (name.equals("trap")) {
      if (args.size() >= 3) {
        bool exit_signal = false;
        for (uint64_t i = 2; i < args.size(); ++i) exit_signal |= arg(i).equals("EXIT") || arg(i).equals("0");
        if (exit_signal) {
          if (arg(1).equals("-")) trap_.clear();
          else if (!trap_.assign(arg(1))) return fail_memory();
        }
      }
      return 0;
    }
    if (name.equals("test") || name.equals("[") || name.equals("[[")) {
      ArrayView<String> words = args.subview(1);
      if (!name.equals("test")) {
        const StringView close = name.equals("[") ? StringView("]") : StringView("]]");
        if (words.empty() || !words.back().view().equals(close)) return 2;
        words = words.first(words.size() - 1);
      }
      uint64_t pos = 0;
      bool error = false;
      const bool value = test_or(words, &pos, name.equals("[["), &error);
      if (error || pos != words.size()) return 2;
      return value ? 0 : 1;
    }
    if (name.equals("echo")) {
      uint64_t i = 1;
      bool newline = true;
      while (i < args.size() && (arg(i).equals("-n") || arg(i).equals("-e") || arg(i).equals("-E"))) {
        if (arg(i).equals("-n")) newline = false;
        ++i;
      }
      for (uint64_t first = i; i < args.size(); ++i) {
        if (i > first) output(" ");
        output(arg(i));
      }
      if (newline) output("\n");
      return 0;
    }
    if (name.equals("printf")) return printf(args);
    if (name.equals("pwd")) {
      output(cwd_.view());
      output("\n");
      return 0;
    }
    if (name.equals("dirname")) {
      StringView path = arg(1);
      while (path.size() > 1 && path.ends_with('/')) path = path.first(path.size() - 1);
      const uint64_t slash = path.rfind('/');
      if (slash == npos) output(".");
      else {
        StringView dir = path.first(slash);
        while (dir.size() > 1 && dir.ends_with('/')) dir = dir.first(dir.size() - 1);
        output(dir.empty() ? StringView("/") : dir);
      }
      output("\n");
      return 0;
    }
    if (name.equals("basename")) {
      StringView path = arg(1);
      while (path.size() > 1 && path.ends_with('/')) path = path.first(path.size() - 1);
      const uint64_t slash = path.rfind('/');
      StringView base = slash == npos || path.size() == 1 ? path : path.substr(slash + 1);
      const StringView suffix = arg(2);
      if (!suffix.empty() && base.size() > suffix.size() && base.ends_with(suffix)) base = base.first(base.size() - suffix.size());
      output(base);
      output("\n");
      return 0;
    }
    if (name.equals("readlink") || name.equals("realpath")) {
      bool canonicalize = name.equals("realpath");
      StringView path;
      for (uint64_t i = 1; i < args.size(); ++i) {
        if (arg(i).starts_with('-') && arg(i).size() > 1) {
          if (arg(i).contains('f') || arg(i).contains('e') || arg(i).contains('m')) canonicalize = true;
          continue;
        }
        path = arg(i);
      }
      if (path.empty()) return 1;
      String result(allocator_);
      if (canonicalize) {
        if (!canonical(path, &result)) return fail_memory();
      } else {
        String full(allocator_), host_path(allocator_);
        bool is_directory = false;
        if (!absolute(path, &full) || !host(full.view(), &host_path)) return fail_memory();
        if (files_.read_symbolic_link(host_path.view(), &result, &is_directory) != filesystem::Status::Ok) return 1;
      }
      output(result.view());
      output("\n");
      return 0;
    }
    if (name.equals("uname")) {
      const StringView option = arg(1);
      output(option.equals("-m") || option.equals("-p") || option.equals("-i") ? StringView("x86_64") : option.equals("-r") ? StringView("6.6.0") : StringView("Linux"));
      output("\n");
      return 0;
    }
    if (name.equals("id")) {
      output(arg(1).contains('n') ? StringView("root") : StringView("0"));
      output("\n");
      return 0;
    }
    if (name.equals("whoami")) {
      output("root\n");
      return 0;
    }
    if (name.equals("cat")) {
      if (args.size() == 1 || (args.size() == 2 && arg(1).equals("-"))) {
        output(input_);
        return 0;
      }
      for (uint64_t i = 1; i < args.size(); ++i) {
        String contents(allocator_);
        if (!read_file(arg(i), kMaxCapture, &contents)) return 1;
        output(contents.view());
      }
      return 0;
    }
    if (name.equals("command") || name.equals("type") || name.equals("which")) {
      uint64_t i = 1;
      while (i < args.size() && arg(i).starts_with('-')) ++i;
      if (i >= args.size()) return 1;
      if (name.equals("command") && !arg(1).starts_with('-')) {      // command prog args: run it, functions excepted
        List<String> rest(allocator_), none(allocator_);
        for (uint64_t j = 1; j < args.size(); ++j) {
          String copy(allocator_);
          if (!copy.assign(arg(j)) || !rest.push_back(util::move(copy))) return fail_memory();
        }
        return dispatch(rest, none);
      }
      if (find_function(arg(i)) || is_builtin(arg(i))) {
        output(arg(i));
        output("\n");
        return 0;
      }
      String program(allocator_);
      if (!find_program(arg(i), &program)) return fail_memory();
      if (program.empty()) return 1;
      output(program.view());
      output("\n");
      return 0;
    }
    if (const Function* function = find_function(name)) return call(*function, args);
    if (name.equals("curl") || name.equals("wget")) return fetch(name.equals("curl"), args);
    {
      // The text filters (ShellTools.hpp) over this command's input.
      List<StringView> tool_args(allocator_);
      for (uint64_t i = 1; i < args.size(); ++i)
        if (!tool_args.push_back(arg(i))) return fail_memory();
      String written(allocator_);
      int status = 0;
      if (shell_tools::run(allocator_, name, tool_args.view(), input_, &written, &status)) {
        output(written.view());
        return status;
      }
    }

    // Another program. One inside the AppImage is where the script hands over; anything else this shell does not have.
    String program(allocator_);
    if (!find_program(name, &program)) return fail_memory();
    if (program.empty()) return kNotFound;
    if (inside_app(program.view()) && substitutions_ == 0) {
      for (String& assignment : assignments.view()) assign(assignment.view(), true);
      return run_file(program.view(), args, false);
    }
    return kNotFound;
  }

  // curl URL / wget -O- URL: the body, through the network the caller gave (none: as if the host could not be found, exit status 6 / 4).
  int fetch(bool curl, ArrayView<String> args) {
    StringView url;
    bool to_stdout = curl;
    for (uint64_t i = 1; i < args.size(); ++i) {
      const StringView a = args[i].view();
      if (curl && (a.equals("-A") || a.equals("-H") || a.equals("-u") || a.equals("-e") || a.equals("-m") || a.equals("--user-agent") || a.equals("--header") ||
                   a.equals("--max-time") || a.equals("--retry") || a.equals("--connect-timeout") || a.equals("-x"))) {
        ++i;
        continue;
      }
      if (curl && (a.equals("-o") || a.equals("-O") || a.equals("--output") || a.equals("--remote-name"))) return kNotFound;      // (downloads to files: not here)
      if (!curl && (a.equals("-O") || a.equals("--output-document"))) {
        to_stdout = i + 1 < args.size() && args[i + 1].view().equals("-");
        ++i;
        continue;
      }
      if (!curl && (a.equals("-qO-") || a.equals("-O-") || a.equals("-qO") || a.equals("--output-document=-"))) {
        to_stdout = !a.equals("-qO") || (i + 1 < args.size() && args[++i].view().equals("-"));
        continue;
      }
      if (a.starts_with('-')) continue;
      if (url.empty()) url = a;
    }
    if (url.empty() || !to_stdout) return kNotFound;
    if (!network_) return curl ? 6 : 4;
    String body(allocator_);
    if (!network_->fetch(url, kMaxFetch, &body)) return curl ? 22 : 8;
    output(body.view());
    return 0;
  }

  bool assign_literal(StringView word, bool export_it) {
    // (the argument of export / local was expanded already: store it as it is)
    const uint64_t eq = word.find('=');
    StringView name = word.first(eq);
    if (name.ends_with('+')) name = name.first(name.size() - 1);
    if (!is_name(name)) return false;
    const StringView value = word.substr(eq + 1);
    Var* var = make_var(name);
    if (!var) return fail_memory() == 0;
    const bool append = word[eq - 1] == '+';
    if (append && var->is_set && !var->array) {
      if (!var->value.append(value)) return fail_memory() == 0;
    } else {
      if (!var->value.assign(value)) return fail_memory() == 0;
      var->items.clear();
      var->array = false;
    }
    var->is_set = true;
    if (export_it) var->exported = true;
    return true;
  }

  static bool is_builtin(StringView name) {
    static const char* const kBuiltins[] = {"exec", ":", "true", "false", "set", "export", "readonly", "local", "declare", "typeset", "unset", "shift", "cd", "source", ".",
                                            "eval", "exit", "return", "break", "continue", "trap", "test", "[", "[[", "echo", "printf", "pwd", "command", "type", "read"};
    for (const char* builtin : kBuiltins)
      if (name.equals(builtin)) return true;
    return false;
  }

  bool inside_app(StringView program) const {
    if (appdir_.empty()) return false;
    return program.starts_with(appdir_.view()) && (program.size() == appdir_.size() || program[appdir_.size()] == '/');
  }

  int call(const Function& function, ArrayView<String> args) {
    if (depth_ >= kMaxDepth) return 1;
    List<String> saved(allocator_);
    for (String& arg : positional_.view()) {
      String copy(allocator_);
      if (!copy.assign(arg.view()) || !saved.push_back(util::move(copy))) return fail_memory();
    }
    positional_.clear();
    for (uint64_t i = 1; i < args.size(); ++i) {
      String copy(allocator_);
      if (!copy.assign(args[i].view()) || !positional_.push_back(util::move(copy))) return fail_memory();
    }
    ++depth_;
    Cursor body{function.script, function.begin, function.end};
    const int status = command(body, true);
    --depth_;
    returning_ = false;
    if (!restore(saved)) return fail_memory();
    return status;
  }

  int printf(ArrayView<String> args) {
    if (args.size() < 2) return 1;
    const StringView format = args[1].view();
    uint64_t next = 2;
    String out(allocator_);
    for (uint64_t i = 0; i < format.size(); ++i) {
      const char c = format[i];
      if (c == '\\' && i + 1 < format.size()) {
        const char e = format[++i];
        if (!out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e)) return fail_memory();
        continue;
      }
      if (c == '%' && i + 1 < format.size()) {
        const char f = format[++i];
        if (f == '%') { if (!out.push_back('%')) return fail_memory(); continue; }
        if (next < args.size() && !out.append(args[next].view())) return fail_memory();
        ++next;
        continue;
      }
      if (!out.push_back(c)) return fail_memory();
    }
    output(out.view());
    return 0;
  }

  // ---------------------------------------------------------------- tests
  bool test_or(ArrayView<String> w, uint64_t* pos, bool brackets, bool* error) {
    bool value = test_and(w, pos, brackets, error);
    while (*pos < w.size() && (w[*pos].view().equals("-o") || (brackets && w[*pos].view().equals("||")))) {
      ++*pos;
      const bool right = test_and(w, pos, brackets, error);
      value = value || right;
    }
    return value;
  }
  bool test_and(ArrayView<String> w, uint64_t* pos, bool brackets, bool* error) {
    bool value = test_not(w, pos, brackets, error);
    while (*pos < w.size() && (w[*pos].view().equals("-a") || (brackets && w[*pos].view().equals("&&")))) {
      ++*pos;
      const bool right = test_not(w, pos, brackets, error);
      value = value && right;
    }
    return value;
  }
  bool test_not(ArrayView<String> w, uint64_t* pos, bool brackets, bool* error) {
    if (*pos < w.size() && w[*pos].view().equals("!") && *pos + 1 < w.size()) {
      ++*pos;
      return !test_not(w, pos, brackets, error);
    }
    return test_primary(w, pos, brackets, error);
  }
  bool test_primary(ArrayView<String> w, uint64_t* pos, bool brackets, bool* error) {
    if (*pos >= w.size()) { *error = true; return false; }
    const StringView a = w[*pos].view();
    if (a.equals("(")) {
      ++*pos;
      const bool value = test_or(w, pos, brackets, error);
      if (*pos >= w.size() || !w[*pos].view().equals(")")) *error = true;
      else ++*pos;
      return value;
    }
    // Binary operators.
    if (*pos + 2 < w.size()) {
      const StringView op = w[*pos + 1].view(), b = w[*pos + 2].view();
      bool known = true, value = false;
      int64_t x = 0, y = 0;
      if (op.equals("=") || op.equals("==")) value = brackets ? glob_match(b, a) : a.equals(b);
      else if (op.equals("!=")) value = brackets ? !glob_match(b, a) : !a.equals(b);
      else if (op.equals("<")) value = a < b;
      else if (op.equals(">")) value = b < a;
      else if (op.equals("-eq") || op.equals("-ne") || op.equals("-lt") || op.equals("-le") || op.equals("-gt") || op.equals("-ge")) {
        const bool numbers = parse_int(a, &x) && parse_int(b, &y);
        if (!numbers && !brackets) { *error = true; return false; }
        if (!numbers) { x = 0; y = 0; (void)parse_int(a, &x); (void)parse_int(b, &y); }
        value = op.equals("-eq") ? x == y : op.equals("-ne") ? x != y : op.equals("-lt") ? x < y : op.equals("-le") ? x <= y : op.equals("-gt") ? x > y : x >= y;
      } else if (op.equals("=~")) {
        value = false;      // (no regular expressions here)
      } else {
        known = false;
      }
      if (known) {
        *pos += 3;
        return value;
      }
    }
    // Unary operators.
    if (a.size() == 2 && a[0] == '-' && *pos + 1 < w.size()) {
      const StringView b = w[*pos + 1].view();
      const char op = a[1];
      bool known = true, value = false;
      filesystem::DirectoryEntry entry;
      switch (op) {
        case 'z': value = b.empty(); break;
        case 'n': value = !b.empty(); break;
        case 'e': case 'a': value = stat(b, &entry); break;
        case 'f': value = stat(b, &entry) && entry.type == filesystem::NodeType::File; break;
        case 'd': value = stat(b, &entry) && entry.type == filesystem::NodeType::Directory; break;
        case 'x': case 'r': case 'w': value = stat(b, &entry); break;
        case 's': value = stat(b, &entry) && (entry.size > 0 || entry.type == filesystem::NodeType::Directory); break;
        case 'L': case 'h': {
          String target(allocator_), full(allocator_), host_path(allocator_);
          bool is_directory = false;
          value = absolute(b, &full) && host(full.view(), &host_path) && files_.read_symbolic_link(host_path.view(), &target, &is_directory) == filesystem::Status::Ok;
          break;
        }
        case 'v': value = find_var(b) != nullptr; break;
        default: known = false;
      }
      if (known) {
        *pos += 2;
        return value;
      }
    }
    ++*pos;
    return !a.empty();
  }

  // ---------------------------------------------------------------- expansion
 public:
  enum class Mode : uint8_t { Fields, Assign, Single };

 private:
  // A word being built: the text, and a copy for pathname expansion in which the characters that were quoted are escaped.
  struct Builder {
    explicit Builder(allocators::IAllocator& allocator) : text(allocator), pattern(allocator) {}
    String text, pattern;
    bool have = false, glob = false;
  };

  bool append_literal(Builder& b, StringView part, bool quoted) {
    if (!b.text.append(part)) return false;
    for (char c : part) {
      if (quoted && (is_glob_char(c) || c == '\\')) {
        if (!b.pattern.push_back('\\')) return false;
      } else if (!quoted && is_glob_char(c)) {
        b.glob = true;
      }
      if (!b.pattern.push_back(c)) return false;
    }
    return true;
  }
  bool end_field(Builder& b, Mode mode, List<String>* out) {
    if (b.glob && mode == Mode::Fields) {
      const uint64_t before = out->count();
      if (!glob(b.pattern.view(), out)) return false;
      if (out->count() == before && !out->push_back(util::move(b.text))) return false;
    } else if (!out->push_back(util::move(b.text))) {
      return false;
    }
    b.text = String(allocator_);
    b.pattern = String(allocator_);
    b.have = b.glob = false;
    return true;
  }
  // An expansion's value: quoted (or when assigning) it joins the word as it is; otherwise it is split at blanks.
  bool append_value(Builder& b, StringView value, bool quoted, Mode mode, List<String>* out) {
    if (quoted || mode != Mode::Fields) {
      b.have = b.have || quoted;
      return append_literal(b, value, true);
    }
    for (uint64_t i = 0; i < value.size();) {
      if (value[i] == ' ' || value[i] == '\t' || value[i] == '\n') {
        if ((b.have || !b.text.empty()) && !end_field(b, mode, out)) return false;
        ++i;
        continue;
      }
      uint64_t end = i;
      while (end < value.size() && value[end] != ' ' && value[end] != '\t' && value[end] != '\n') ++end;
      if (!append_literal(b, value.substr(i, end - i), true)) return false;
      b.have = true;
      i = end;
    }
    return true;
  }
  bool append_list(Builder& b, ArrayView<String> items, bool quoted, Mode mode, List<String>* out) {
    if (mode != Mode::Fields || !quoted) {
      for (uint64_t i = 0; i < items.size(); ++i) {
        if (i > 0 && !append_value(b, " ", quoted, mode, out)) return false;
        if (!append_value(b, items[i].view(), quoted, mode, out)) return false;
      }
      return true;
    }
    for (uint64_t i = 0; i < items.size(); ++i) {
      if (i > 0 && !end_field(b, mode, out)) return false;
      if (!append_literal(b, items[i].view(), true)) return false;
      b.have = true;
    }
    return true;
  }

  // Expands one word into fields (Fields), or into one string (Assign, Single). False if a construct is not understood (or memory ran out).
  bool expand(StringView raw, Mode mode, List<String>* out) {
    // "$@" and "${name[@]}" alone: one field per item, none for an empty list.
    if (mode == Mode::Fields) {
      StringView inner = raw;
      if (inner.size() >= 2 && inner.starts_with('"') && inner.ends_with('"')) inner = inner.substr(1, inner.size() - 2);
      ArrayView<String> items;
      if (inner.equals("$@") || inner.equals("${@}")) items = positional_.view();
      else if (inner.starts_with("${") && inner.ends_with("[@]}") && is_name(inner.substr(2, inner.size() - 6))) {
        Var* var = find_var(inner.substr(2, inner.size() - 6));
        if (var && var->array) items = var->items.view();
        else if (var && var->is_set) {
          String copy(allocator_);
          if (!copy.assign(var->value.view()) || !out->push_back(util::move(copy))) return fail_memory() == 0;
          return true;
        } else {
          return true;
        }
      } else {
        items = ArrayView<String>();
        inner = StringView();
      }
      if (!inner.empty()) {
        for (String& item : items) {
          String copy(allocator_);
          if (!copy.assign(item.view()) || !out->push_back(util::move(copy))) return fail_memory() == 0;
        }
        return true;
      }
    }
    Builder b(allocator_);
    if (!expand_into(b, raw, false, mode, out)) return false;
    if ((b.have || !b.text.empty() || mode != Mode::Fields) && !end_field(b, mode, out)) return fail_memory() == 0;
    return true;
  }

  bool expand_into(Builder& b, StringView raw, bool in_quotes, Mode mode, List<String>* out) {
    for (uint64_t i = 0; i < raw.size();) {
      const char c = raw[i];
      if (c == '\'' && !in_quotes) {
        const uint64_t end = raw.find('\'', i + 1);
        if (end == npos) return false;
        if (!append_literal(b, raw.substr(i + 1, end - i - 1), true)) return fail_memory() == 0;
        b.have = true;
        i = end + 1;
        continue;
      }
      if (c == '"' && !in_quotes) {
        const uint64_t end = skip_piece(raw, i);
        if (end == npos) return false;
        if (!expand_into(b, raw.substr(i + 1, end - i - 2), true, mode, out)) return false;
        b.have = true;
        i = end;
        continue;
      }
      if (c == '\\') {
        if (i + 1 >= raw.size()) { ++i; continue; }
        const char next = raw[i + 1];
        if (in_quotes && next != '$' && next != '`' && next != '"' && next != '\\' && next != '\n') {
          if (!append_literal(b, "\\", true)) return fail_memory() == 0;
          ++i;
          continue;
        }
        if (next != '\n' && !append_literal(b, raw.substr(i + 1, 1), true)) return fail_memory() == 0;
        b.have = true;
        i += 2;
        continue;
      }
      if (c == '`') {
        const uint64_t end = skip_piece(raw, i);
        if (end == npos) return false;
        String result(allocator_);
        if (!substitute(raw.substr(i + 1, end - i - 2), &result)) return false;
        if (!append_value(b, result.view(), in_quotes, mode, out)) return fail_memory() == 0;
        i = end;
        continue;
      }
      if (c == '$' && i + 1 < raw.size()) {
        const uint64_t consumed = dollar(b, raw.substr(i), in_quotes, mode, out);
        if (consumed == 0) return false;
        i += consumed;
        continue;
      }
      if (c == '~' && i == 0 && !in_quotes && (raw.size() == 1 || raw[1] == '/')) {
        const Var* home = find_var("HOME");
        if (!append_literal(b, home ? home->value.view() : StringView("/root"), true)) return fail_memory() == 0;
        b.have = true;
        ++i;
        continue;
      }
      if (!append_literal(b, raw.substr(i, 1), in_quotes)) return fail_memory() == 0;
      if (in_quotes) b.have = true;
      ++i;
    }
    return true;
  }

  // One $ expansion at the start of `raw`; returns how many characters it took (0: not understood).
  uint64_t dollar(Builder& b, StringView raw, bool quoted, Mode mode, List<String>* out) {
    const char c = raw[1];
    if (c == '(') {
      if (raw.size() > 2 && raw[2] == '(') return 0;      // $(( )): no arithmetic here
      const uint64_t end = skip_group(raw, 1);
      if (end == npos) return 0;
      String result(allocator_);
      if (!substitute(raw.substr(2, end - 3), &result)) return 0;
      if (!append_value(b, result.view(), quoted, mode, out)) return fail_memory(), 0;
      return end;
    }
    if (c == '{') {
      const uint64_t end = skip_group(raw, 1);
      if (end == npos) return 0;
      if (!braced(b, raw.substr(2, end - 3), quoted, mode, out)) return 0;
      return end;
    }
    if (c == '\'' ) return 0;      // $'...'
    if (c == '@' || c == '*') {
      if (!append_list(b, positional_.view(), quoted && c == '@', mode, out)) return fail_memory(), 0;
      if (quoted) b.have = b.have || positional_.count() > 0;
      return 2;
    }
    String value(allocator_);
    uint64_t length = 2;
    if (c == '#') {
      if (!value.append_uint(positional_.count())) return 0;
    } else if (c == '?') {
      if (!value.append_uint(uint64_t(last_status_))) return 0;
    } else if (c == '$') {
      if (!value.assign("4242")) return 0;
    } else if (c == '!' || c == '-') {
    } else if (is_digit(c)) {
      if (!positional(uint64_t(c - '0'), &value)) return 0;
    } else if (is_name_start(c)) {
      while (length < raw.size() && is_name_char(raw[length])) ++length;
      if (const Var* var = find_var(raw.substr(1, length - 1)); var && var->is_set) {
        if (!value.assign(var->array ? (var->items.count() ? var->items.view()[0].view() : StringView()) : var->value.view())) return 0;
      }
    } else {
      if (!append_literal(b, "$", quoted)) return 0;      // a lone $
      return 1;
    }
    if (!append_value(b, value.view(), quoted, mode, out)) return fail_memory(), 0;
    return length;
  }
  bool positional(uint64_t index, String* value) {
    if (index == 0) return value->assign(zero_.view());
    if (index <= positional_.count()) return value->assign(positional_.view()[index - 1].view());
    return value->assign("");
  }

  // ${...}
  bool braced(Builder& b, StringView inside, bool quoted, Mode mode, List<String>* out) {
    if (inside.empty()) return false;
    bool length_of = false;
    if (inside[0] == '#' && inside.size() > 1) {
      length_of = true;
      inside = inside.substr(1);
    } else if (inside[0] == '!') {
      return false;      // indirection
    }
    uint64_t n = 0;
    if (is_name_start(inside[0])) {
      while (n < inside.size() && is_name_char(inside[n])) ++n;
    } else if (is_digit(inside[0])) {
      while (n < inside.size() && is_digit(inside[n])) ++n;
    } else if (inside[0] == '@' || inside[0] == '*' || inside[0] == '#' || inside[0] == '?' || inside[0] == '$' || inside[0] == '0') {
      n = 1;
    } else {
      return false;
    }
    const StringView name = inside.first(n);
    StringView rest = inside.substr(n);
    bool whole_list = false;
    int64_t element = -1;
    if (rest.starts_with('[')) {
      const uint64_t close = rest.find(']');
      if (close == npos) return false;
      const StringView index = rest.substr(1, close - 1);
      if (index.equals("@") || index.equals("*")) whole_list = true;
      else if (!parse_int(index, &element) || element < 0) return false;
      rest = rest.substr(close + 1);
    }
    // The value (or list).
    List<String> items(allocator_);
    bool is_set = false, is_list = false;
    String value(allocator_);
    if (name.equals("@") || name.equals("*")) {
      is_list = true;
      is_set = positional_.count() > 0;
      for (String& arg : positional_.view()) {
        String copy(allocator_);
        if (!copy.assign(arg.view()) || !items.push_back(util::move(copy))) return fail_memory() == 0;
      }
    } else if (name.equals("#")) {
      is_set = value.append_uint(positional_.count());
    } else if (name.equals("?")) {
      is_set = value.append_uint(uint64_t(last_status_));
    } else if (name.equals("$")) {
      is_set = value.assign("4242");
    } else if (is_digit(name[0])) {
      uint64_t index = 0;
      if (!name.parse_uint(&index)) return false;
      is_set = index == 0 || index <= positional_.count();
      if (!positional(index, &value)) return fail_memory() == 0;
    } else if (const Var* var = find_var(name); var && var->is_set) {
      is_set = true;
      if (var->array) {
        if (whole_list) {
          is_list = true;
          for (const String& item : var->items.view()) {
            String copy(allocator_);
            if (!copy.assign(item.view()) || !items.push_back(util::move(copy))) return fail_memory() == 0;
          }
          is_set = items.count() > 0;
        } else {
          const uint64_t index = element < 0 ? 0 : uint64_t(element);
          is_set = index < var->items.count();
          if (is_set && !value.assign(var->items.view()[index].view())) return fail_memory() == 0;
        }
      } else if (element > 0) {
        is_set = false;
      } else {
        if (!value.assign(var->value.view())) return fail_memory() == 0;
        if (whole_list) {
          is_list = true;
          String copy(allocator_);
          if (!copy.assign(var->value.view()) || !items.push_back(util::move(copy))) return fail_memory() == 0;
        }
      }
    }
    if (length_of) {
      if (!rest.empty()) return false;
      String count(allocator_);
      if (!count.append_uint(is_list ? items.count() : value.size())) return fail_memory() == 0;
      return append_value(b, count.view(), quoted, mode, out) || fail_memory() != 0;
    }
    if (rest.empty()) {
      if (is_list) return append_list(b, items.view(), quoted, mode, out) || fail_memory() != 0;
      return append_value(b, value.view(), quoted, mode, out) || fail_memory() != 0;
    }
    // An operator and its word.
    const bool colon = rest.starts_with(':');
    StringView op = rest.substr(colon ? 1 : 0, 1);
    const bool empty = !is_set || (is_list ? items.count() == 0 : value.empty());
    const bool missing = colon ? empty : !is_set;
    if (op.equals("-") || op.equals("=") || op.equals("+") || op.equals("?")) {
      const StringView word = rest.substr(colon ? 2 : 1);
      if (op.equals("+")) {
        if (missing) return true;
        return expand_word_part(b, word, quoted, mode, out);
      }
      if (!missing) {
        if (is_list) return append_list(b, items.view(), quoted, mode, out) || fail_memory() != 0;
        return append_value(b, value.view(), quoted, mode, out) || fail_memory() != 0;
      }
      if (op.equals("?")) {
        halt(false);
        return true;
      }
      if (op.equals("=")) {
        List<String> fields(allocator_);
        if (!expand(word, Mode::Assign, &fields)) return false;
        const StringView assigned = fields.count() ? fields.view()[0].view() : StringView();
        if (!is_name(name) || !set_var(name, assigned)) return false;
        return append_value(b, assigned, quoted, mode, out) || fail_memory() != 0;
      }
      return expand_word_part(b, word, quoted, mode, out);
    }
    if (colon) return false;      // ${X:offset:length}
    // Pattern removal.
    const bool suffix = rest.starts_with('%'), prefix = rest.starts_with('#');
    if (!suffix && !prefix) return false;
    const bool longest = rest.size() > 1 && rest[1] == rest[0];
    List<String> fields(allocator_);
    if (!expand(rest.substr(longest ? 2 : 1), Mode::Single, &fields)) return false;
    const StringView pattern = fields.count() ? fields.view()[0].view() : StringView();
    const StringView text = value.view();
    StringView result = text;
    if (suffix) {
      for (uint64_t k = 0; k <= text.size(); ++k) {
        const uint64_t start = longest ? k : text.size() - k;
        if (glob_match(pattern, text.substr(start))) {
          result = text.first(start);
          break;
        }
      }
    } else {
      for (uint64_t k = 0; k <= text.size(); ++k) {
        const uint64_t len = longest ? text.size() - k : k;
        if (glob_match(pattern, text.first(len))) {
          result = text.substr(len);
          break;
        }
      }
    }
    return append_value(b, result, quoted, mode, out) || fail_memory() != 0;
  }

  // The word of ${X:-word}: its own quotes are removed; inside double quotes it is not split.
  bool expand_word_part(Builder& b, StringView word, bool quoted, Mode mode, List<String>* out) {
    if (quoted) b.have = true;
    return expand_into(b, word, false, quoted ? Mode::Single : mode, out);
  }

  // $( ... ): runs the commands with their output collected, trailing newlines removed.
  bool substitute(StringView commands, String* result) {
    if (depth_ >= kMaxDepth) return false;
    String* saved_capture = capture_;
    const bool saved_halted = halted_;
    capture_ = result;
    Snapshot snapshot(allocator_);      // (a subshell: what it changes stays in it)
    if (!take_snapshot(&snapshot)) { fail_memory(); return false; }
    ++substitutions_;
    const int status = eval(commands);
    --substitutions_;
    capture_ = saved_capture;
    if (launched_) return true;
    if (!restore_snapshot(snapshot)) { fail_memory(); return false; }
    halted_ = saved_halted;
    last_substitution_ = status;
    while (result->size() && (*result)[result->size() - 1] == '\n') result->truncate(result->size() - 1);
    return !out_of_memory_;
  }

  // Pathname expansion of the last component of `pattern` (escaped as the Builder makes it), in sorted order.
  bool glob(StringView pattern, List<String>* out) {
    const uint64_t slash = pattern.rfind('/');
    const StringView dir_pattern = slash == npos ? StringView() : pattern.first(slash);
    const StringView name_pattern = slash == npos ? pattern : pattern.substr(slash + 1);
    String dir(allocator_);
    for (uint64_t i = 0; i < dir_pattern.size(); ++i) {
      char c = dir_pattern[i];
      if (c == '\\' && i + 1 < dir_pattern.size()) c = dir_pattern[++i];
      else if (is_glob_char(c)) return true;      // (only the last component is expanded)
      if (!dir.push_back(c)) return false;
    }
    String full(allocator_), host_path(allocator_);
    if (!absolute(slash == npos ? StringView(".") : (dir.empty() ? StringView("/") : dir.view()), &full) || !host(full.view(), &host_path)) return false;
    UniquePointer<filesystem::IDirectory> listing;
    if (files_.open_directory(host_path.view(), &listing) != filesystem::Status::Ok) return true;
    const uint64_t first = out->count();
    filesystem::DirectoryEntry entry;
    for (uint32_t guard = 0; guard < 100000 && listing->next(&entry) == filesystem::Status::Ok; ++guard) {
      const StringView name = entry.name.view();
      if (name.starts_with('.') && !name_pattern.starts_with('.')) continue;
      if (!glob_match(name_pattern, name)) continue;
      String match(allocator_);
      if (slash != npos && (!match.assign(dir.view()) || !match.push_back('/'))) return false;
      if (!match.append(name) || !out->push_back(util::move(match))) return false;
    }
    // Sorted (insertion sort: listings are short).
    ArrayView<String> found = out->view().subview(first);
    for (uint64_t i = 1; i < found.size(); ++i)
      for (uint64_t j = i; j > 0 && found[j].view() < found[j - 1].view(); --j) {
        String t = util::move(found[j]);
        found[j] = util::move(found[j - 1]);
        found[j - 1] = util::move(t);
      }
    return true;
  }

 public:
  int last_status_ = 0;

 private:
  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  StringView root_;
  AppRunLaunch* launch_;
  List<UniquePointer<Var>> vars_;
  List<UniquePointer<Function>> functions_;
  List<UniquePointer<Script>> scripts_;
  String zero_;
  List<String> positional_;
  String cwd_, trap_, appdir_;
  String* capture_ = nullptr;
  StringView input_;      // what the command reads (the pipe before it; nothing at the start)
  IScriptNetwork* network_ = nullptr;
  bool no_launch_ = false;
  uint32_t steps_ = 0, depth_ = 0, substitutions_ = 0;
  int last_substitution_ = 0;
  bool halted_ = false, launched_ = false, out_of_memory_ = false, syntax_ = false;
  bool breaking_ = false, continuing_ = false, returning_ = false;
};

}  // namespace

AppRunResult evaluate_apprun(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView world_root, StringView entry,
                             ArrayView<const StringView> arguments, ArrayView<const StringView> environment, AppRunLaunch* launch) {
  Shell shell(allocator, files, world_root, launch);
  if (!shell.init(arguments, environment)) return AppRunResult::OutOfMemory;
  List<String> argv(allocator);
  String zero(allocator);
  if (!zero.assign(entry) || !argv.push_back(util::move(zero))) return AppRunResult::OutOfMemory;
  for (const StringView argument : arguments) {
    String copy(allocator);
    if (!copy.assign(argument) || !argv.push_back(util::move(copy))) return AppRunResult::OutOfMemory;
  }
  shell.run_file(entry, argv.view(), true);
  if (shell.out_of_memory()) return AppRunResult::OutOfMemory;
  return shell.launched() ? AppRunResult::Launch : AppRunResult::NoProgram;
}

}  // namespace desktop

namespace desktop {

bool evaluate_script_variable(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView world_root, StringView script, StringView variable,
                              ArrayView<const StringView> environment, IScriptNetwork* network, String* value) {
  AppRunLaunch unused(allocator);
  Shell shell(allocator, files, world_root, &unused);
  if (!shell.init(ArrayView<const StringView>(), environment)) return false;
  shell.set_network(network);
  shell.forbid_launch();
  shell.run_program_text(script);
  if (shell.out_of_memory()) return false;
  return shell.variable(variable, value);
}

}  // namespace desktop

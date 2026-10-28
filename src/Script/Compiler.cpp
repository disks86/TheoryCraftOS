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

#include "Script/Compiler.hpp"
#include "Script/Number.hpp"

namespace script {

namespace {

struct Binding {
  StringView name;
  uint32_t slot;
  StringView import_name;   // an imported binding: the name of the export it reads (from the namespace object the slot holds)
  bool lexical = false;     // let / const / class: reading it before it is initialized is a ReferenceError
  bool is_const = false;    // const: assigning to it is a TypeError
  uint32_t level = 0;       // 0: a slot of the function's Env; n: a slot of the n-th block Env that is open (see EnvScope)
};

// A block whose variables live in an Env of their own (a closure made in it may capture them, and each time round a loop they must be new ones).
struct EnvScope {
  uint32_t scope_depth;     // the FuncState::scope_depth of the block
  uint32_t count;           // variables declared so far
  uint32_t capacity;        // variables the PushEnv made room for
};

struct Ctx {
  enum Kind : uint8_t { Loop, Switch, Try } kind;
  StringView label;
  uint32_t id;
  bool handler_active;     // Try: an exception handler of the VM is set for the code being compiled
  Node* finalizer;         // Try: the finally block to run on the way out (or null)
  int32_t continue_target; // Loop: where `continue` goes if that is known already (else it is patched)
  uint32_t env_depth = 0;  // how many block Envs were open where it began (a break or continue leaves the ones above)
  int32_t iter_slot = -1;  // Loop over values (for-of): the slot of its iterator, which is closed when the loop is left early
};

struct Patch {
  uint32_t ctx_id;
  bool is_continue;
  uint32_t instr;
};

struct FuncState {
  explicit FuncState(allocators::IAllocator& a) : bindings(a), scope_marks(a), ctx(a), patches(a), envs(a) {}
  FunctionCode* code = nullptr;
  FuncState* parent = nullptr;
  List<Binding> bindings;
  List<uint32_t> scope_marks;
  List<Ctx> ctx;
  List<Patch> patches;
  List<EnvScope> envs;          // the block Envs that are open here, outermost first
  bool is_global = false;
  bool arrow = false;        // a function seen from a direct eval has no code of its own: this says whether it is an arrow
  bool frozen = false;       // ... and it cannot get an `arguments` variable any more
  bool is_program = false;   // the code of the whole source (a strict eval is one that is not global: its variables are its own)
  uint32_t scope_depth = 0;     // blocks entered (0: the function's own top level)
  int32_t completion_slot = -1;
  uint32_t next_ctx_id = 1;
};

class Compiler {
 public:
  Compiler(Vm& vm, String* error, bool library, bool local_scope) : vm_(vm), error_(error), library_(library), local_scope_(local_scope), withs_(vm.allocator()) {}

  // The program of a direct eval: compiled as a function inside the functions that were around the call, so that it reads and writes their variables.
  bool eval_program(Node* block, const EvalScope& scope, FunctionCode** out) {
    allocators::IAllocator& a = vm_.allocator();
    List<FuncState*> chain(a);
    for (uint64_t f = 0; f < scope.frames.count(); ++f) {
      const EvalFrame& frame = scope.frames[f];
      void* mem = a.allocate(sizeof(FuncState));
      if (!mem) return fail("Out of memory", 0);
      FuncState* fs = new (mem) FuncState(a);
      chain.push_back(fs);
      fs->arrow = frame.arrow;
      fs->frozen = true;
      fs->is_global = frame.global;
      for (uint32_t k = 0; k < frame.env_count; ++k) fs->envs.push_back(EnvScope{0, 0, 0});
      for (uint32_t k = 0; k < frame.count; ++k) {
        const EvalBinding& e = scope.bindings[frame.first + k];
        Binding b{scope.text.view().substr(e.name_at, e.name_len), e.slot, scope.text.view().substr(e.import_at, e.import_len)};
        b.lexical = e.lexical;
        b.is_const = e.is_const;
        b.level = e.level;
        fs->bindings.push_back(b);
      }
      if (f > 0) chain[f - 1]->parent = fs;
    }
    eval_parent_ = chain.count() ? chain[0] : nullptr;
    for (uint64_t f = chain.count(); f > 0; --f) {   // the objects that hold the variables other evals declared (outermost first: the innermost is looked in first)
      FuncState* fs = chain[f - 1];
      for (uint64_t i = 0; i < fs->bindings.count(); ++i)
        if (fs->bindings[i].name.starts_with("%ev")) {
          withs_.push_back(WithCtx{fs, 0, fs->bindings[i].name, true});
          if (f == 1) eval_dynamic_ = fs->bindings[i].name;
        }
    }
    eval_strict_ = scope.frames.count() && scope.frames[0].strict;
    const bool ok = program(block, out);
    for (uint64_t i = 0; i < chain.count(); ++i) {
      chain[i]->~FuncState();
      a.deallocate(chain[i]);
    }
    return ok;
  }

  bool program(Node* block, FunctionCode** out) {
    FuncState fs(vm_.allocator());
    fs.code = vm_.new_code();
    if (!fs.code) return fail("Out of memory", 0);
    fs.code->library = library_;
    fs.code->strict = (block->flags & kFlagStrict) != 0 || has_use_strict(block->list);
    fs.is_program = true;
    fs.is_global = !(local_scope_ && fs.code->strict);
    if (eval_parent_) {   // a direct eval: the code is a closure made where the eval is called
      fs.parent = eval_parent_;
      fs.is_global = false;
      fs.code->is_arrow = true;
      fs.code->strict = fs.code->strict || eval_strict_;
    }
    cur_ = &fs;
    fs.completion_slot = static_cast<int32_t>(new_slot());
    check_redeclarations(block->list);
    hoist_globals(block->list);
    declare_lexicals(block->list);   // (nothing for a global program: its let and const are globals)
    init_functions(block->list);
    for (Node* s = block->list; s && !failed_; s = s->next) statement(s);
    emit(Opc::GetVar, fs.completion_slot, fn_hops(), 0);
    emit(Opc::Return, 0, 0, 0);
    fs.code->slot_count = next_slot_of(fs);
    cur_ = nullptr;
    if (failed_) return false;
    *out = fs.code;
    return true;
  }

 private:
  // The names a statement list declares with let, const and class twice, or with let / const / class and var (or a function) both, are an early error.
  void check_redeclarations(const Node* list) {
    for (const Node* s = list; s && !failed_; s = s->next) {
      List<StringView> names(vm_.allocator());
      if (s->kind == NodeKind::Var && s->var_kind != VarKind::Var) for (const Node* d = s->list; d; d = d->next) names.push_back(d->text);
      else if (s->kind == NodeKind::Var) continue;
      else if (s->kind == NodeKind::FunctionDecl) continue;
      for (uint64_t i = 0; i < names.count(); ++i) {
        const StringView name = names[i];
        if (name.empty() || name[0] == '%') continue;
        for (const Node* o = list; o; o = o->next) {
          if (o == s) {   // (this declaration's own other declarators)
            uint32_t seen = 0;
            for (const Node* d = s->list; d; d = d->next) seen += d->text.equals(name) ? 1 : 0;
            if (seen > 1) fail("Identifier has already been declared", s->line);
            continue;
          }
          const bool same_name_lexical = o->kind == NodeKind::Var && o->var_kind != VarKind::Var;
          if (o->kind == NodeKind::Var) {
            for (const Node* d = o->list; d; d = d->next)
              if (d->text.equals(name) && (same_name_lexical || true)) fail("Identifier has already been declared", s->line);
          } else if (o->kind == NodeKind::FunctionDecl && o->text.equals(name)) {
            fail("Identifier has already been declared", s->line);
          }
        }
      }
    }
  }

  // Does a statement list begin with the "use strict" directive?
  static bool has_use_strict(const Node* list) {
    while (list && list->kind == NodeKind::Var && (list->flags & kFlagPrologue)) list = list->next;   // (the unpacking of pattern parameters is not a statement of the body)
    for (const Node* s = list; s && s->kind == NodeKind::ExprStmt && s->a && s->a->kind == NodeKind::String; s = s->next)
      if (s->a->text.equals("use strict") && !(s->a->flags & kFlagParenthesized)) return true;
    return false;
  }

  // --- Errors and emitting
  bool fail(const char* message, uint32_t line) {
    if (!failed_) {
      failed_ = true;
      error_->assign(message);
      if (line) {
        error_->append(" (line ");
        error_->append_uint(line);
        error_->append(")");
      }
    }
    return false;
  }

  uint32_t slots_used(FuncState& fs) const { return fs.code->slot_count; }
  uint32_t next_slot_of(FuncState& fs) const { return fs.code->slot_count; }
  uint32_t new_slot() {
    return cur_->code->slot_count++;
  }

  uint32_t pos() const { return static_cast<uint32_t>(cur_->code->code.count()); }
  uint32_t emit(Opc op, int32_t a, uint8_t b, uint32_t line) {
    Instr in{op, b, 0, a};
    if (!cur_->code->code.push_back(in) || !cur_->code->lines.push_back(line ? line : line_)) fail("Out of memory", 0);
    return pos() - 1;
  }
  uint32_t emit(Opc op, int32_t a = 0) { return emit(op, a, 0, line_); }
  void patch(uint32_t instr) {
    if (instr < cur_->code->code.count()) cur_->code->code[instr].a = static_cast<int32_t>(pos());
  }
  void patch_to(uint32_t instr, uint32_t target) {
    if (instr < cur_->code->code.count()) cur_->code->code[instr].a = static_cast<int32_t>(target);
  }

  uint32_t constant_string(StringView text) {
    JsString* s = vm_.intern(text);
    List<Value>& k = cur_->code->constants;
    for (uint64_t i = 0; i < k.count(); ++i)
      if (k[i].is_string() && k[i].as_string() == s) return static_cast<uint32_t>(i);
    if (!k.push_back(Value::make_string(s))) fail("Out of memory", 0);
    return static_cast<uint32_t>(k.count() - 1);
  }
  uint32_t constant_number(double d) {
    List<Value>& k = cur_->code->constants;
    for (uint64_t i = 0; i < k.count(); ++i)
      if (k[i].is_number() && k[i].u.number == d && (d != 0 || (1 / k[i].u.number > 0) == (1 / d > 0))) return static_cast<uint32_t>(i);
    if (!k.push_back(Value::make_number(d))) fail("Out of memory", 0);
    return static_cast<uint32_t>(k.count() - 1);
  }

  // --- Scopes and names
  void push_scope() {
    cur_->scope_marks.push_back(static_cast<uint32_t>(cur_->bindings.count()));
    ++cur_->scope_depth;
  }
  void pop_scope() {
    const uint32_t mark = cur_->scope_marks[cur_->scope_marks.count() - 1];
    cur_->scope_marks.remove_at(cur_->scope_marks.count() - 1);
    while (cur_->bindings.count() > mark) cur_->bindings.remove_at(cur_->bindings.count() - 1);
    --cur_->scope_depth;
  }
  // Early errors of strict mode code on a name: `eval` and `arguments` cannot be bound or assigned, and the words reserved for the future are not identifiers.
  void strict_name(StringView name, uint32_t line, bool binding, bool force = false) {
    if (!cur_ || !(cur_->code->strict || force) || library_ || name.empty() || name[0] == '%') return;
    if (binding && (name.equals("eval") || name.equals("arguments"))) {
      fail("Unexpected eval or arguments in strict mode", line ? line : line_);
      return;
    }
    static const char* const kReserved[] = {"implements", "interface", "let", "package", "private", "protected", "public", "static", "yield"};
    for (const char* word : kReserved)
      if (name.equals(word)) {
        fail("Unexpected strict mode reserved word", line ? line : line_);
        return;
      }
  }
  // A variable of the current block (let / const) or of the function (var, parameters).
  uint32_t declare(StringView name) {
    strict_name(name, 0, true);
    for (uint64_t i = cur_->bindings.count(); i > 0; --i) {
      if (cur_->bindings[i - 1].name.equals(name) && i - 1 >= (cur_->scope_marks.count() ? cur_->scope_marks[cur_->scope_marks.count() - 1] : 0)) return cur_->bindings[i - 1].slot;
    }
    Binding b{name, 0};
    if (cur_->envs.count() && cur_->envs[cur_->envs.count() - 1].scope_depth == cur_->scope_depth) {   // a variable of a block that has its own Env
      EnvScope& e = cur_->envs[cur_->envs.count() - 1];
      if (e.count >= e.capacity) {
        fail("Too many block variables", line_);
        return 0;
      }
      b.slot = e.count++;
      b.level = static_cast<uint32_t>(cur_->envs.count());
    } else {
      b.slot = new_slot();
    }
    cur_->bindings.push_back(b);
    return b.slot;
  }
  // How many Envs out the function's own Env is.
  uint8_t fn_hops() const { return static_cast<uint8_t>(cur_->envs.count() < 250 ? cur_->envs.count() : 250); }
  void begin_env(uint32_t capacity, uint32_t line) {
    emit(Opc::PushEnv, static_cast<int32_t>(capacity), 0, line);
    cur_->envs.push_back(EnvScope{cur_->scope_depth, 0, capacity});
  }
  void end_env(uint32_t line) {
    emit(Opc::PopEnv, 0, 0, line);
    cur_->envs.remove_at(cur_->envs.count() - 1);
  }
  // Does anything in this subtree make a function (which could capture a variable)?
  static bool makes_closure(const Node* n, uint32_t depth = 0) {
    if (!n) return false;
    if (n->kind == NodeKind::Function || n->kind == NodeKind::FunctionDecl || depth > 400) return true;
    if (makes_closure(n->a, depth + 1) || makes_closure(n->b, depth + 1) || makes_closure(n->c, depth + 1) || makes_closure(n->d, depth + 1)) return true;
    for (const Node* c = n->list; c; c = c->next)
      if (makes_closure(c, depth + 1)) return true;
    return false;
  }
  static uint32_t lexical_count(const Node* list) {
    uint32_t n = 0;
    for (const Node* s = list; s; s = s->next)
      if (s->kind == NodeKind::Var && s->var_kind != VarKind::Var)
        for (const Node* d = s->list; d; d = d->next) ++n;
    return n;
  }

  // A let / const / class variable of the current block. A second declaration in the same block is an early error.
  uint32_t declare_lexical(StringView name, bool is_const, uint32_t line) {
    const uint64_t floor = cur_->scope_marks.count() ? cur_->scope_marks[cur_->scope_marks.count() - 1] : 0;
    for (uint64_t i = cur_->bindings.count(); i > floor; --i)
      if (cur_->bindings[i - 1].name.equals(name) && cur_->bindings[i - 1].lexical) {
        fail("Identifier has already been declared", line);
        return cur_->bindings[i - 1].slot;
      }
    const uint32_t slot = declare(name);
    for (uint64_t i = cur_->bindings.count(); i > 0; --i)
      if (cur_->bindings[i - 1].slot == slot) {
        cur_->bindings[i - 1].lexical = true;
        cur_->bindings[i - 1].is_const = is_const;
        break;
      }
    return slot;
  }

  // Where a name lives: depth = how many functions out, slot; false for a global.
  bool resolve(StringView name, uint32_t* depth, uint32_t* slot) {
    uint32_t hops = 0, funcs = 0;
    resolved_import_ = StringView();
    resolved_lexical_ = resolved_const_ = false;
    for (FuncState* fs = cur_; fs; fs = fs->parent, ++funcs) {
      for (uint64_t i = fs->bindings.count(); i > 0; --i) {
        if (fs->bindings[i - 1].name.equals(name)) {
          *depth = hops + static_cast<uint32_t>(fs->envs.count()) - fs->bindings[i - 1].level;
          *slot = fs->bindings[i - 1].slot;
          resolved_funcs_ = funcs;
          resolved_import_ = fs->bindings[i - 1].import_name;
          resolved_lexical_ = fs->bindings[i - 1].lexical;
          resolved_const_ = fs->bindings[i - 1].is_const;
          return true;
        }
      }
      if (name.equals("arguments") && !fs->is_global && !fs->is_program && !fs->frozen && !(fs->code ? fs->code->is_arrow : fs->arrow)) {
        FuncState* saved = cur_;
        cur_ = fs;
        const uint32_t s = new_slot();
        fs->bindings.insert(0, Binding{name, s});   // (visible everywhere in the function)
        for (uint64_t m = 0; m < fs->scope_marks.count(); ++m) ++fs->scope_marks[m];
        fs->code->arguments_slot = static_cast<int32_t>(s);
        cur_ = saved;
        *depth = hops + static_cast<uint32_t>(fs->envs.count());
        *slot = s;
        resolved_funcs_ = funcs;
        return true;
      }
      hops += static_cast<uint32_t>(fs->envs.count()) + 1;
    }
    return false;
  }

  // Is this declaration at the top level of the script (a global)?
  bool global_scope() const { return cur_->is_global && cur_->scope_depth == 0; }

  // --- Hoisting: `var` names and function declarations belong to the whole function.
  void hoist_vars(Node* list, bool functions_only_declared) {
    for (Node* s = list; s; s = s->next) hoist_stmt(s, functions_only_declared);
  }
  void hoist_stmt(Node* s, bool nested) {
    if (!s) return;
    switch (s->kind) {
      case NodeKind::Var:
        if (s->var_kind == VarKind::Var)
          for (Node* d = s->list; d; d = d->next) declare_var(d->text);
        break;
      case NodeKind::FunctionDecl: declare_var(s->text); break;
      case NodeKind::Block: hoist_vars(s->list, true); break;
      case NodeKind::If: hoist_stmt(s->b, true); hoist_stmt(s->c, true); break;
      case NodeKind::For: hoist_stmt(s->a, true); hoist_stmt(s->d, true); break;
      case NodeKind::ForIn:
      case NodeKind::ForOf: hoist_stmt(s->a, true); hoist_stmt(s->d, true); break;
      case NodeKind::While:
      case NodeKind::With:
      case NodeKind::DoWhile: hoist_stmt(s->d, true); break;
      case NodeKind::Labeled: hoist_stmt(s->a, true); break;
      case NodeKind::Try: hoist_stmt(s->a, true); hoist_stmt(s->c, true); hoist_stmt(s->d, true); break;
      case NodeKind::Switch:
        for (Node* c = s->list; c; c = c->next) hoist_vars(c->list, true);
        break;
      default: break;
    }
    (void)nested;
  }
  void declare_var(StringView name) {
    strict_name(name, 0, true);
    if (!eval_dynamic_.empty() && cur_->is_program && cur_->parent == eval_parent_ && !cur_->code->strict) {   // sloppy direct eval: a var is a variable of the calling function
      uint32_t d, s;
      if (resolve(name, &d, &s) && resolved_funcs_ == 1) return;   // (the function has it: nothing to declare)
      emit(Opc::PushStr, static_cast<int32_t>(constant_string(name)));
      load_plain(eval_dynamic_, 0);
      emit(binary_opcode(Op::In));
      const uint32_t has = emit(Opc::JumpIfTrue);
      load_plain(eval_dynamic_, 0);
      emit(Opc::PushUndef);
      emit(Opc::SetProp, static_cast<int32_t>(constant_string(name)));
      emit(Opc::Pop);
      patch(has);
      return;
    }
    if (cur_->is_global) {
      emit(Opc::DeclareGlobal, static_cast<int32_t>(constant_string(name)));
      return;
    }
    // function-level bindings go first so that block variables of the same name shadow them
    for (uint64_t i = 0; i < cur_->bindings.count(); ++i)
      if (cur_->bindings[i].name.equals(name) && i < function_level_count_) return;
    const uint32_t slot = new_slot();
    cur_->bindings.insert(function_level_count_, Binding{name, slot});
    ++function_level_count_;
    for (uint64_t m = 0; m < cur_->scope_marks.count(); ++m) ++cur_->scope_marks[m];
  }
  void hoist_globals(Node* list) { hoist_vars(list, false); }

  // The imports of a module function: each name is a variable holding the namespace object of its module (the first parameter is the array of them);
  // reading a named import reads the property of that object.
  void bind_imports(Node* list) {
    for (Node* s = list; s && !failed_; s = s->next) {
      if (s->kind != NodeKind::Import) continue;
      for (Node* d = s->list; d; d = d->next) {
        declare_var(d->text);
        uint32_t slot = 0;
        for (uint64_t i = 0; i < cur_->bindings.count(); ++i) {
          if (!cur_->bindings[i].name.equals(d->text)) continue;
          slot = cur_->bindings[i].slot;
          if (d->a) cur_->bindings[i].import_name = d->a->text;
          break;
        }
        emit(Opc::GetVar, 0, fn_hops(), s->line);
        emit(Opc::PushInt, static_cast<int32_t>(s->number), 0, s->line);
        emit(Opc::GetElem, 0, 0, s->line);
        emit(Opc::SetVar, static_cast<int32_t>(slot), fn_hops(), s->line);
        emit(Opc::Pop);
      }
    }
  }

  // At the start of a statement list: the functions declared in it exist.
  void init_functions(Node* list) {
    for (Node* s = list; s; s = s->next) {
      if (s->kind != NodeKind::FunctionDecl) continue;
      if (!(s->a->flags & kFlagExprBody)) strict_name(s->text, s->line, true, has_use_strict(s->a->a->list));   // (a function with the directive in its body is strict, and so is its name)
      function_value(s->a);
      store_name(s->text, s->line);
      emit(Opc::Pop);
    }
  }
  // Declares the let / const of a statement list in the current scope (not at the script's top level, where they are globals).
  void declare_lexicals(Node* list) {
    if (global_scope()) return;
    for (Node* s = list; s; s = s->next)
      if (s->kind == NodeKind::Var && s->var_kind != VarKind::Var)
        for (Node* d = s->list; d; d = d->next) {
          const uint32_t slot = declare_lexical(d->text, s->var_kind == VarKind::Const, d->line);
          emit(Opc::TdzMark, static_cast<int32_t>(slot), static_cast<uint8_t>(cur_->envs.count() - cur_->bindings[cur_->bindings.count() - 1].level), d->line);
        }
  }

  // --- Variables
  // --- with: while the body of a with statement is compiled, a name is looked up in the object first
  struct WithCtx {
    FuncState* fs;
    uint64_t mark;    // the bindings of fs from here on were declared inside the body (they shadow the object)
    StringView var;   // the hidden variable that holds the object
    bool whole = false;   // the object of a function that calls eval: the names of the whole function are bound inside it
  };
  // Is `name` bound inside the with statement (by a let, a parameter or a variable of a function in the body)?
  bool bound_inside(const WithCtx& ctx, StringView name) {
    for (FuncState* fs = cur_; fs; fs = fs->parent) {
      for (uint64_t i = fs->bindings.count(); i > 0; --i) {
        if (!fs->bindings[i - 1].name.equals(name)) continue;
        if (fs == ctx.fs) return i - 1 >= ctx.mark;
        for (FuncState* f = fs->parent; f; f = f->parent)
          if (f == ctx.fs) return true;
        return false;
      }
    }
    return false;
  }
  bool with_applies(StringView name) const { return !withs_.empty() && !name.empty() && name[0] != '%' && name.find('.') == StringView::npos; }

  void load_name(StringView name, uint32_t line) {
    strict_name(name, line, false);
    if (!with_applies(name)) return load_plain(name, line);
    List<uint32_t> ends(vm_.allocator());
    for (uint64_t i = withs_.count(); i > 0; --i) {
      const WithCtx ctx = withs_[i - 1];
      if (bound_inside(ctx, name)) break;
      emit(Opc::PushStr, static_cast<int32_t>(constant_string(name)), 0, line);
      load_plain(ctx.var, line);
      emit(binary_opcode(Op::In), 0, 0, line);
      const uint32_t no = emit(Opc::JumpIfFalse);
      load_plain(ctx.var, line);
      emit(Opc::GetProp, static_cast<int32_t>(constant_string(name)), 0, line);
      ends.push_back(emit(Opc::Jump));
      patch(no);
    }
    load_plain(name, line);
    for (uint64_t i = 0; i < ends.count(); ++i) patch(ends[i]);
  }
  void store_name(StringView name, uint32_t line, bool initialize = false) {
    if (!initialize) strict_name(name, line, true);
    if (!with_applies(name) || initialize) return store_plain(name, line, initialize);
    List<uint32_t> ends(vm_.allocator());
    for (uint64_t i = withs_.count(); i > 0; --i) {
      const WithCtx ctx = withs_[i - 1];
      if (bound_inside(ctx, name)) break;
      emit(Opc::PushStr, static_cast<int32_t>(constant_string(name)), 0, line);
      load_plain(ctx.var, line);
      emit(binary_opcode(Op::In), 0, 0, line);
      const uint32_t no = emit(Opc::JumpIfFalse);
      load_plain(ctx.var, line);   // [value, object]
      emit(Opc::Swap);
      emit(Opc::SetProp, static_cast<int32_t>(constant_string(name)), 0, line);   // [value]
      ends.push_back(emit(Opc::Jump));
      patch(no);
    }
    store_plain(name, line, initialize);
    for (uint64_t i = 0; i < ends.count(); ++i) patch(ends[i]);
  }

  void load_plain(StringView name, uint32_t line) {
    uint32_t depth, slot;
    if (resolve(name, &depth, &slot)) {
      if (depth > 250) {
        fail("Functions are nested too deeply", line);
        return;
      }
      const uint32_t at = emit(Opc::GetVar, static_cast<int32_t>(slot), static_cast<uint8_t>(depth), line);
      if (resolved_lexical_) cur_->code->code[at].unused = checked_mark(name);
      if (!resolved_import_.empty()) emit(Opc::GetProp, static_cast<int32_t>(constant_string(resolved_import_)), 0, line);   // (an import is read from its module's namespace each time: a live binding)
    } else if (name.equals("undefined")) {
      emit(Opc::PushUndef);
    } else {
      emit(Opc::GetGlobal, static_cast<int32_t>(constant_string(name)), 0, line);
    }
  }
  // Stores the value on the top of the stack into a name, leaving the value there.
  // The spare bits of a GetVar / SetVar of a let / const / class variable (see kChecked).
  uint16_t checked_mark(StringView name) {
    const uint32_t k = constant_string(name);
    return k < 60000 ? static_cast<uint16_t>(kChecked + k) : kChecked;
  }
  // `initialize`: this store is the declaration itself (for (const x of ...) gives x its value), not an assignment.
  void store_plain(StringView name, uint32_t line, bool initialize = false) {
    uint32_t depth, slot;
    if (resolve(name, &depth, &slot)) {
      if (depth > 250) {
        fail("Functions are nested too deeply", line);
        return;
      }
      if (!resolved_import_.empty()) fail("Assignment to constant variable", line);
      const uint32_t at = emit(Opc::SetVar, static_cast<int32_t>(slot), static_cast<uint8_t>(depth), line);
      if (!initialize) {
        if (resolved_const_) cur_->code->code[at].unused = kConstAssign;
        else if (resolved_lexical_) cur_->code->code[at].unused = checked_mark(name);
      }
    } else {
      emit(Opc::SetGlobal, static_cast<int32_t>(constant_string(name)), 0, line);
    }
  }

  // --- Functions
  // expression() of a value that is stored under a name: an anonymous function or arrow gets that name
  void named_expression(Node* value, StringView name) {
    if (value && value->kind == NodeKind::Function && value->text.empty() && !name.empty()) infer_name_ = name;
    expression(value);
    infer_name_ = StringView();
  }

  void function_value(Node* fn) {
    FunctionCode* child = compile_function(fn);
    if (!child || failed_) return;
    cur_->code->children.push_back(child);
    emit(Opc::Closure, static_cast<int32_t>(cur_->code->children.count() - 1), 0, fn->line);
  }

  FunctionCode* compile_function(Node* fn) {
    FuncState fs(vm_.allocator());
    fs.code = vm_.new_code();
    if (!fs.code) {
      fail("Out of memory", fn->line);
      return nullptr;
    }
    const StringView inferred = infer_name_;
    infer_name_ = StringView();
    fs.parent = cur_;
    fs.code->library = library_;
    fs.code->strict = (cur_ && cur_->code->strict) || (fn->flags & kFlagStrict) != 0 || (!(fn->flags & kFlagExprBody) && has_use_strict(fn->a->list));
    FuncState* saved = cur_;
    const uint32_t saved_level = function_level_count_;
    function_level_count_ = 0;
    cur_ = &fs;
    fs.code->is_arrow = (fn->flags & kFlagArrow) != 0;
    fs.code->is_async = (fn->flags & kFlagAsync) != 0;
    fs.code->is_generator = (fn->flags & kFlagGenerator) != 0 && !(fn->flags & kFlagArrow);
    fs.code->name = !fn->text.empty() ? vm_.intern(fn->text) : !inferred.empty() ? vm_.intern(inferred) : nullptr;
    {   // early errors of the parameter list
      bool simple = !(fn->flags & kFlagRest);
      for (Node* p = fn->list; p; p = p->next) simple = simple && !p->a && !(!p->text.empty() && p->text[0] == '%');
      if (!simple && !(fn->flags & kFlagExprBody) && has_use_strict(fn->a->list)) fail("Illegal 'use strict' directive in function with non-simple parameter list", fn->line);
      if (fs.code->strict || fs.code->is_arrow || !simple) {
        for (Node* p = fn->list; p; p = p->next) {
          if (!p->text.empty() && p->text[0] == '%') continue;
          for (Node* q = fn->list; q != p; q = q->next)
            if (q->text.equals(p->text)) fail("Duplicate parameter name not allowed in this context", p->line);
        }
      }
      if (!(fn->flags & kFlagExprBody))   // a name the patterns of the parameters bind is a parameter too
        for (Node* v = fn->a->list; v && v->kind == NodeKind::Var && (v->flags & kFlagPrologue); v = v->next)
          for (Node* d = v->list; d; d = d->next)
            for (Node* p = fn->list; p; p = p->next)
              if (p->text.equals(d->text)) fail("Duplicate parameter name not allowed in this context", d->line);
      for (Node* p = fn->list; p; p = p->next) strict_name(p->text, p->line, true);
      if ((fn->flags & kFlagExpression) && !fn->text.empty()) strict_name(fn->text, fn->line, true);
    }
    uint32_t params = 0;
    bool counting = true;
    for (Node* p = fn->list; p; p = p->next) {
      const uint32_t slot = new_slot();   // (a parameter is slot `params`: they come first)
      fs.bindings.push_back(Binding{p->text, slot});
      ++params;
      if (p->a || (!p->next && (fn->flags & kFlagRest))) counting = false;   // (length: the parameters before the first default or the rest)
      if (counting) fs.code->expected_args = params;
    }
    function_level_count_ = static_cast<uint32_t>(fs.bindings.count());
    {
      bool plain = !fs.code->strict && !fs.code->is_arrow && !(fn->flags & kFlagRest);
      for (Node* p = fn->list; p && plain; p = p->next) plain = !p->a;
      fs.code->mapped_arguments = plain;
    }
    fs.code->param_count = params;
    fs.code->has_rest = (fn->flags & kFlagRest) != 0;
    if ((fn->flags & kFlagExpression) && !fn->text.empty()) {
      bool is_param = false;
      for (Node* p = fn->list; p; p = p->next) is_param = is_param || p->text.equals(fn->text);
      if (!is_param) {
        fs.code->self_slot = static_cast<int32_t>(new_slot());
        fs.bindings.insert(function_level_count_, Binding{fn->text, static_cast<uint32_t>(fs.code->self_slot)});
        ++function_level_count_;
      }
    }
    if (!(fn->flags & kFlagExprBody)) hoist_vars(fn->a->list, false);
    if (!(fn->flags & kFlagExprBody)) bind_imports(fn->a->list);
    bool has_dynamic = false;
    if (!(fn->flags & kFlagExprBody) && (fn->a->flags & kFlagHasEval) && !fs.code->strict && !library_) {   // eval(...) here may declare variables: they live in an object that the names of the function look in
      String text(vm_.allocator());
      text.append("%ev");
      text.append_uint(++with_counter_);
      const StringView var = vm_.intern(text.view())->view();
      emit(Opc::NewObject, 0, 0, fn->line);
      emit(Opc::PushNull);
      emit(Opc::SetLiteralProto);
      const uint32_t slot = new_slot();
      cur_->bindings.insert(function_level_count_, Binding{var, slot});
      ++function_level_count_;
      store_plain(var, fn->line, true);
      emit(Opc::Pop);
      withs_.push_back(WithCtx{cur_, 0, var, true});
      has_dynamic = true;
    }
    // default values of parameters
    uint32_t index = 0;
    for (Node* p = fn->list; p; p = p->next, ++index) {
      if (!p->a) continue;
      emit(Opc::GetVar, static_cast<int32_t>(index), fn_hops(), p->line);
      emit(Opc::PushUndef);
      emit(Opc::StrictEq);
      const uint32_t skip = emit(Opc::JumpIfFalse);
      named_expression(p->a, p->text);
      emit(Opc::SetVar, static_cast<int32_t>(index), fn_hops(), p->line);
      emit(Opc::Pop);
      patch(skip);
    }
    if (fs.code->is_generator) {   // calling a generator function runs it up to here (the parameters are bound); the first next() goes on
      emit(Opc::PushUndef);
      emit(Opc::Yield);
      emit(Opc::Pop);
    }
    if (fn->flags & kFlagExprBody) {
      expression(fn->a);
      emit(Opc::Return);
    } else {
      declare_lexicals(fn->a->list);   // (before the functions: they may use the let / const of the body)
      init_functions(fn->a->list);
      for (Node* s = fn->a->list; s && !failed_; s = s->next) statement(s);
      emit(Opc::PushUndef);
      emit(Opc::Return);
    }
    if (has_dynamic) withs_.remove_at(withs_.count() - 1);
    cur_ = saved;
    function_level_count_ = saved_level;
    return failed_ ? nullptr : fs.code;
  }

  // --- Control flow contexts
  Ctx& push_ctx(Ctx::Kind kind, bool handler_active = false, Node* finalizer = nullptr) {
    Ctx c{kind, pending_label_, cur_->next_ctx_id++, handler_active, finalizer, -1, static_cast<uint32_t>(cur_->envs.count()), -1};
    pending_label_ = StringView();
    cur_->ctx.push_back(c);
    return cur_->ctx[cur_->ctx.count() - 1];
  }
  // Ends a loop or switch: its breaks (and continues, to `continue_target`) are patched.
  void pop_ctx(int32_t continue_target = -1) {
    const uint32_t id = cur_->ctx[cur_->ctx.count() - 1].id;
    cur_->ctx.remove_at(cur_->ctx.count() - 1);
    uint64_t keep = 0;
    for (uint64_t i = 0; i < cur_->patches.count(); ++i) {
      const Patch p = cur_->patches[i];
      if (p.ctx_id == id) {
        if (p.is_continue) patch_to(p.instr, continue_target >= 0 ? static_cast<uint32_t>(continue_target) : pos());
        else patch(p.instr);
      } else {
        cur_->patches[keep++] = p;
      }
    }
    while (cur_->patches.count() > keep) cur_->patches.remove_at(cur_->patches.count() - 1);
  }

  // Leaves the contexts above index `target` (-1: all of them): handlers are ended and finally blocks run.
  void unwind_to(int64_t target) {
    List<Ctx> removed(vm_.allocator());
    const uint64_t count = cur_->ctx.count();
    for (uint64_t i = count; i > static_cast<uint64_t>(target + 1) && !failed_; --i) {
      const Ctx c = cur_->ctx[i - 1];
      if (c.kind == Ctx::Loop && c.iter_slot >= 0) {   // (leaving a for-of: its iterator is closed)
        emit(Opc::GetVar, c.iter_slot, fn_hops(), line_);
        emit(Opc::IterClose, 0, 0, line_);
      }
      if (c.kind != Ctx::Try) continue;
      if (c.handler_active) emit(Opc::TryEnd);
      if (c.finalizer) {
        // compile the finally block as if outside this try
        while (cur_->ctx.count() > i - 1) {
          removed.push_back(cur_->ctx[cur_->ctx.count() - 1]);
          cur_->ctx.remove_at(cur_->ctx.count() - 1);
        }
        statement(c.finalizer);
        while (removed.count() > 0) {
          cur_->ctx.push_back(removed[removed.count() - 1]);
          removed.remove_at(removed.count() - 1);
        }
      }
    }
  }

  int64_t find_ctx(bool is_continue, StringView label, uint32_t line) {
    for (uint64_t i = cur_->ctx.count(); i > 0; --i) {
      const Ctx& c = cur_->ctx[i - 1];
      if (c.kind == Ctx::Try) continue;
      if (!label.empty()) {
        if (c.label.equals(label) && (!is_continue || c.kind == Ctx::Loop)) return static_cast<int64_t>(i - 1);
        continue;
      }
      if (is_continue ? c.kind == Ctx::Loop : true) return static_cast<int64_t>(i - 1);
    }
    fail(is_continue ? "Illegal continue statement" : "Illegal break statement", line);
    return -1;
  }

  // --- Statements
  void statement_list(Node* list) {
    for (Node* s = list; s && !failed_; s = s->next) statement(s);
  }

  void statement(Node* n) {
    if (!n || failed_) return;
    line_ = n->line;
    switch (n->kind) {
      case NodeKind::Empty: return;
      case NodeKind::Import: return;   // (bound at the start of the module function)
      case NodeKind::FunctionDecl: return;   // (made when its list began)
      case NodeKind::ExprStmt:
        expression(n->a);
        if (cur_->is_program && cur_->completion_slot >= 0) {
          emit(Opc::SetVar, cur_->completion_slot, fn_hops(), n->line);
        }
        emit(Opc::Pop);
        return;
      case NodeKind::Var: var_statement(n); return;
      case NodeKind::Block: {
        push_scope();
        const uint32_t lexicals = global_scope() ? 0 : lexical_count(n->list);
        const bool own_env = lexicals > 0 && makes_closure(n);
        if (own_env) begin_env(lexicals, n->line);
        declare_lexicals(n->list);
        init_functions(n->list);
        statement_list(n->list);
        if (own_env) end_env(n->line);
        pop_scope();
        return;
      }
      case NodeKind::If: {
        if (cur_->code->strict && !library_ && ((n->b && n->b->kind == NodeKind::FunctionDecl) || (n->c && n->c->kind == NodeKind::FunctionDecl))) {
          fail("In strict mode code, functions can only be declared at top level or inside a block", n->line);
          return;
        }
        expression(n->a);
        const uint32_t to_else = emit(Opc::JumpIfFalse);
        statement(n->b);
        if (n->c) {
          const uint32_t to_end = emit(Opc::Jump);
          patch(to_else);
          statement(n->c);
          patch(to_end);
        } else {
          patch(to_else);
        }
        return;
      }
      case NodeKind::With: {
        if (cur_->code->strict) {
          fail("Strict mode code may not include a with statement", n->line);
          return;
        }
        expression(n->a);
        String text(vm_.allocator());
        text.append("%w");
        text.append_uint(++with_counter_);
        const StringView var = vm_.intern(text.view())->view();
        const uint32_t slot = new_slot();
        cur_->bindings.insert(function_level_count_, Binding{var, slot});
        ++function_level_count_;
        for (uint64_t m = 0; m < cur_->scope_marks.count(); ++m) ++cur_->scope_marks[m];
        for (uint64_t k = 0; k < withs_.count(); ++k)
          if (withs_[k].fs == cur_ && !withs_[k].whole) ++withs_[k].mark;
        store_plain(var, n->line, true);
        emit(Opc::Pop);
        withs_.push_back(WithCtx{cur_, cur_->bindings.count(), var});
        statement(n->d);
        withs_.remove_at(withs_.count() - 1);
        return;
      }
      case NodeKind::While: {
        push_ctx(Ctx::Loop);
        cur_->ctx[cur_->ctx.count() - 1].continue_target = static_cast<int32_t>(pos());
        const uint32_t top = pos();
        expression(n->a);
        const uint32_t to_end = emit(Opc::JumpIfFalse);
        statement(n->d);
        emit(Opc::Jump, static_cast<int32_t>(top));
        patch(to_end);
        pop_ctx(static_cast<int32_t>(top));
        return;
      }
      case NodeKind::DoWhile: {
        push_ctx(Ctx::Loop);
        const uint32_t top = pos();
        statement(n->d);
        const uint32_t cont = pos();
        expression(n->a);
        emit(Opc::JumpIfTrue, static_cast<int32_t>(top));
        pop_ctx(static_cast<int32_t>(cont));
        return;
      }
      case NodeKind::For: for_statement(n); return;
      case NodeKind::ForIn:
      case NodeKind::ForOf: for_each_statement(n); return;
      case NodeKind::Return: {
        if (n->a) expression(n->a);
        else emit(Opc::PushUndef);
        bool has_finally = false;
        for (uint64_t i = 0; i < cur_->ctx.count(); ++i)
          has_finally = has_finally || (cur_->ctx[i].kind == Ctx::Try && cur_->ctx[i].finalizer) || (cur_->ctx[i].kind == Ctx::Loop && cur_->ctx[i].iter_slot >= 0);
        if (has_finally) {
          const uint32_t tmp = new_slot();
          emit(Opc::SetVar, static_cast<int32_t>(tmp), fn_hops(), n->line);
          emit(Opc::Pop);
          unwind_to(-1);
          emit(Opc::GetVar, static_cast<int32_t>(tmp), fn_hops(), n->line);
        }
        emit(Opc::Return, 0, 0, n->line);
        return;
      }
      case NodeKind::Break:
      case NodeKind::Continue: {
        const bool is_continue = n->kind == NodeKind::Continue;
        const int64_t target = find_ctx(is_continue, n->text, n->line);
        if (target < 0) return;
        unwind_to(target);
        if (!is_continue && cur_->ctx[static_cast<uint64_t>(target)].iter_slot >= 0) {   // (a break leaves the for-of it ends)
          emit(Opc::GetVar, cur_->ctx[static_cast<uint64_t>(target)].iter_slot, fn_hops(), n->line);
          emit(Opc::IterClose, 0, 0, n->line);
        }
        Ctx& c = cur_->ctx[static_cast<uint64_t>(target)];
        for (uint64_t k = cur_->envs.count(); k > c.env_depth; --k) emit(Opc::PopEnv, 0, 0, n->line);   // (the block Envs the jump leaves)
        if (is_continue && c.continue_target >= 0) {
          emit(Opc::Jump, c.continue_target);
        } else {
          const uint32_t at = emit(Opc::Jump);
          cur_->patches.push_back(Patch{c.id, is_continue, at});
        }
        return;
      }
      case NodeKind::Throw:
        expression(n->a);
        emit(Opc::Throw, 0, 0, n->line);
        return;
      case NodeKind::Try: try_statement(n); return;
      case NodeKind::Switch: switch_statement(n); return;
      case NodeKind::Labeled: {
        const NodeKind k = n->a->kind;
        if (k == NodeKind::FunctionDecl && cur_->code->strict && !library_) {
          fail("In strict mode code, functions can only be declared at top level or inside a block", n->line);
          return;
        }
        if (k == NodeKind::For || k == NodeKind::ForIn || k == NodeKind::ForOf || k == NodeKind::While || k == NodeKind::DoWhile) {
          pending_label_ = n->text;
          statement(n->a);
        } else {
          pending_label_ = n->text;
          push_ctx(Ctx::Switch);
          statement(n->a);
          pop_ctx();
        }
        return;
      }
      default:
        fail("Unsupported statement", n->line);
        return;
    }
  }

  void var_statement(Node* n) {
    for (Node* d = n->list; d && !failed_; d = d->next) {
      line_ = d->line;
      if (d->a) {
        named_expression(d->a, d->text);
      } else if (n->var_kind == VarKind::Var) {
        continue;   // `var x;` does nothing more than the declaration
      } else {
        emit(Opc::PushUndef);
      }
      if (n->var_kind != VarKind::Var && !global_scope()) {
        uint32_t depth, slot;
        if (resolve(d->text, &depth, &slot) && resolved_funcs_ == 0) emit(Opc::SetVar, static_cast<int32_t>(slot), static_cast<uint8_t>(depth), d->line);
        else store_name(d->text, d->line);
      } else {
        store_name(d->text, d->line);
      }
      emit(Opc::Pop);
    }
  }

  void for_statement(Node* n) {
    push_scope();
    // for (let i ...) { ... function () { i } ... }: every iteration has its own i
    const bool own_env = n->a && n->a->kind == NodeKind::Var && n->a->var_kind != VarKind::Var && !global_scope() && makes_closure(n);
    if (own_env) begin_env(lexical_count(n->a), n->line);
    if (n->a) {
      if (n->a->kind == NodeKind::Var) {
        if (n->a->var_kind != VarKind::Var && !global_scope())
          for (Node* d = n->a->list; d; d = d->next) declare_lexical(d->text, n->a->var_kind == VarKind::Const, d->line);
        var_statement(n->a);
      } else {
        expression(n->a);
        emit(Opc::Pop);
      }
    }
    push_ctx(Ctx::Loop);
    const uint32_t top = pos();
    uint32_t to_end = 0;
    bool has_cond = false;
    if (n->b) {
      expression(n->b);
      to_end = emit(Opc::JumpIfFalse);
      has_cond = true;
    }
    statement(n->d);
    const uint32_t cont = pos();
    if (own_env) emit(Opc::CopyEnv, 0, 0, n->line);
    if (n->c) {
      expression(n->c);
      emit(Opc::Pop);
    }
    emit(Opc::Jump, static_cast<int32_t>(top));
    if (has_cond) patch(to_end);
    pop_ctx(static_cast<int32_t>(cont));
    if (own_env) end_env(n->line);
    pop_scope();
  }

  void for_each_statement(Node* n) {
    push_scope();
    const bool of = n->kind == NodeKind::ForOf;
    Node* target = n->a;
    StringView name;
    const bool lexical = target->kind == NodeKind::Var && target->var_kind != VarKind::Var && !global_scope();
    if (target->kind == NodeKind::Var) name = target->list->text;
    expression(n->b);
    emit(of ? Opc::ForOfStart : Opc::ForInStart);
    const uint32_t it = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(it), fn_hops(), n->line);
    emit(Opc::Pop);
    push_ctx(Ctx::Loop);
    if (of) cur_->ctx[cur_->ctx.count() - 1].iter_slot = static_cast<int32_t>(it);
    const uint32_t top = pos();
    emit(Opc::GetVar, static_cast<int32_t>(it), fn_hops(), n->line);
    const uint32_t to_end = emit(Opc::IterNext);
    uint32_t guard = 0;
    if (of) {   // an exception in the body closes the iterator on its way out
      guard = emit(Opc::TryBegin);
      push_ctx(Ctx::Try, true, nullptr);
    }
    // for (let x of xs) { ... function () { x } ... }: every iteration has a variable of its own
    const bool own_env = lexical && makes_closure(n->d);
    if (own_env) begin_env(1, n->line);
    if (lexical) declare_lexical(name, target->var_kind == VarKind::Const, n->line);
    if (target->kind == NodeKind::Var || target->kind == NodeKind::Ident) {
      if (target->kind == NodeKind::Ident) name = target->text;
      store_name(name, n->line, target->kind == NodeKind::Var);
      emit(Opc::Pop);
    } else if (target->kind == NodeKind::Member) {
      expression(target->a);
      emit(Opc::Swap);
      emit(Opc::SetProp, static_cast<int32_t>(constant_string(target->text)));
      emit(Opc::Pop);
    } else {
      fail("Unsupported loop variable", n->line);
    }
    statement(n->d);
    if (own_env) end_env(n->line);
    if (of) {
      cur_->ctx.remove_at(cur_->ctx.count() - 1);
      emit(Opc::TryEnd);
    }
    emit(Opc::Jump, static_cast<int32_t>(top));
    if (of) {
      patch(guard);   // (the exception is on the stack)
      emit(Opc::GetVar, static_cast<int32_t>(it), fn_hops(), n->line);
      emit(Opc::IterClose, 0, 1, n->line);
      emit(Opc::Throw, 0, 0, n->line);
    }
    patch(to_end);
    pop_ctx(static_cast<int32_t>(top));
    pop_scope();
  }

  void try_statement(Node* n) {
    const bool has_catch = n->c != nullptr, has_finally = n->d != nullptr;
    const uint32_t begin = emit(Opc::TryBegin);
    push_ctx(Ctx::Try, true, has_finally ? n->d : nullptr);
    statement(n->a);
    cur_->ctx.remove_at(cur_->ctx.count() - 1);
    emit(Opc::TryEnd);
    if (has_finally) statement(n->d);
    const uint32_t to_end = emit(Opc::Jump);
    uint32_t to_end2 = 0, skip_catch = 0;
    bool second = false;
    if (has_catch) {
      patch(begin);   // the exception is on the stack here
      skip_catch = emit(Opc::CheckReturn);   // (a generator's return() is not caught: only the finally block runs)
      push_scope();
      // catch (e) { ... function () { e } ... }: the variable is in an Env of its own, so that every exception has a new one
      const bool own_env = n->b && makes_closure(n->c);
      uint32_t held = 0;
      if (own_env) {
        held = new_slot();
        emit(Opc::SetVar, static_cast<int32_t>(held), fn_hops(), n->line);
      } else if (n->b) {
        const uint32_t slot = declare(n->b->text);
        emit(Opc::SetVar, static_cast<int32_t>(slot), fn_hops(), n->line);
      }
      emit(Opc::Pop);
      uint32_t rethrow_begin = 0;
      if (has_finally) {
        rethrow_begin = emit(Opc::TryBegin);
        push_ctx(Ctx::Try, true, n->d);
      }
      if (own_env) {
        begin_env(1, n->line);
        const uint32_t slot = declare(n->b->text);
        emit(Opc::GetVar, static_cast<int32_t>(held), fn_hops(), n->line);
        emit(Opc::SetVar, static_cast<int32_t>(slot), 0, n->line);
        emit(Opc::Pop);
      }
      statement(n->c);
      if (own_env) end_env(n->line);
      pop_scope();
      if (has_finally) {
        cur_->ctx.remove_at(cur_->ctx.count() - 1);
        emit(Opc::TryEnd);
        statement(n->d);
        to_end2 = emit(Opc::Jump);
        second = true;
        patch(rethrow_begin);
        finally_rethrow(n->d);
      } else {
        to_end2 = emit(Opc::Jump);
        second = true;
      }
    } else {
      patch(begin);
      finally_rethrow(n->d);
    }
    if (has_catch) {
      patch(skip_catch);
      if (has_finally) finally_rethrow(n->d);
      else emit(Opc::Throw);
    }
    patch(to_end);
    if (second) patch(to_end2);
  }

  // The exception is on the stack: keep it, run the finally block, throw it again.
  void finally_rethrow(Node* finalizer) {
    const uint32_t tmp = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(tmp), fn_hops(), finalizer->line);
    emit(Opc::Pop);
    statement(finalizer);
    emit(Opc::GetVar, static_cast<int32_t>(tmp), fn_hops(), finalizer->line);
    emit(Opc::Throw);
  }

  void switch_statement(Node* n) {
    push_scope();
    expression(n->a);
    const uint32_t disc = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(disc), fn_hops(), n->line);
    emit(Opc::Pop);
    for (Node* c = n->list; c; c = c->next) declare_lexicals(c->list);
    push_ctx(Ctx::Switch);
    List<uint32_t> jumps(vm_.allocator());
    int64_t default_index = -1;
    uint64_t index = 0;
    for (Node* c = n->list; c; c = c->next, ++index) {
      if (!c->a) {
        default_index = static_cast<int64_t>(index);
        jumps.push_back(0);
        continue;
      }
      emit(Opc::GetVar, static_cast<int32_t>(disc), fn_hops(), c->line);
      expression(c->a);
      emit(Opc::StrictEq);
      jumps.push_back(emit(Opc::JumpIfTrue));
    }
    const uint32_t to_default = emit(Opc::Jump);
    index = 0;
    uint32_t default_at = 0;
    bool has_default = default_index >= 0;
    for (Node* c = n->list; c; c = c->next, ++index) {
      if (!c->a) {
        default_at = pos();
      } else {
        patch(jumps[index]);
      }
      init_functions(c->list);
      statement_list(c->list);
    }
    if (has_default) patch_to(to_default, default_at);
    else patch(to_default);
    pop_ctx();
    pop_scope();
  }

  // --- Expressions
  void expression(Node* n) {
    if (!n || failed_) return;
    line_ = n->line;
    switch (n->kind) {
      case NodeKind::Number: {
        if ((n->flags & kFlagLegacyOctal) && cur_->code->strict && !library_) { fail("Octal literals are not allowed in strict mode", n->line); return; }
        const double d = n->number;
        if (d == static_cast<double>(static_cast<int32_t>(d)) && !(d == 0 && 1 / d < 0)) emit(Opc::PushInt, static_cast<int32_t>(d));
        else emit(Opc::PushNum, static_cast<int32_t>(constant_number(d)));
        return;
      }
      case NodeKind::String:
        if ((n->flags & kFlagLegacyOctal) && cur_->code->strict && !library_) { fail("Octal escape sequences are not allowed in strict mode", n->line); return; }
        emit(Opc::PushStr, static_cast<int32_t>(constant_string(n->text))); return;
      case NodeKind::Template: {
        bool first = true;
        for (Node* p = n->list; p; p = p->next) {
          if (p->kind == NodeKind::String) {
            if (first || !p->text.empty()) {
              emit(Opc::PushStr, static_cast<int32_t>(constant_string(p->text)));
              if (!first) emit(Opc::Add);
            }
          } else {
            expression(p);
            emit(Opc::ToStr);
            emit(Opc::Add);
          }
          first = false;
        }
        return;
      }
      case NodeKind::Ident:
        if (n->text.equals("new.target")) emit(Opc::NewTarget);
        else load_name(n->text, n->line);
        return;
      case NodeKind::This: emit(Opc::PushThis); return;
      case NodeKind::Null: emit(Opc::PushNull); return;
      case NodeKind::True: emit(Opc::PushTrue); return;
      case NodeKind::False: emit(Opc::PushFalse); return;
      case NodeKind::ArrayLit:
        emit(Opc::NewArray);
        for (Node* e = n->list; e; e = e->next) {
          if (e->kind == NodeKind::Spread) {
            expression(e->a);
            emit(Opc::ArraySpread);
          } else {
            if (e->kind == NodeKind::Ident && (e->flags & kFlagHole)) emit(Opc::PushHole);
            else expression(e);
            emit(Opc::ArrayPush);
          }
        }
        return;
      case NodeKind::ObjectLit:
        emit(Opc::NewObject);
        for (Node* p = n->list; p; p = p->next) {
          if (p->kind == NodeKind::Spread) {
            expression(p->a);
            emit(Opc::ObjectSpread);
          } else if (p->flags & (kFlagGetter | kFlagSetter)) {
            const uint8_t which = (p->flags & kFlagGetter) ? 1 : 2;
            if (p->flags & kFlagComputed) {
              expression(p->b);
              expression(p->a);
              emit(Opc::DefineAccessorElem, 0, which, p->line);
            } else {
              expression(p->a);
              emit(Opc::DefineAccessor, static_cast<int32_t>(constant_string(p->text)), which, p->line);
            }
          } else if (p->flags & kFlagComputed) {
            expression(p->b);
            expression(p->a);
            emit(Opc::DefineElem);
          } else if (p->text.equals("__proto__") && !(p->flags & kFlagShorthand)) {   // { __proto__: x } sets the prototype
            expression(p->a);
            emit(Opc::SetLiteralProto);
          } else {
            named_expression(p->a, p->text);
            emit(Opc::DefineProp, static_cast<int32_t>(constant_string(p->text)));
          }
        }
        return;
      case NodeKind::Function: function_value(n); return;
      case NodeKind::Unary: unary(n); return;
      case NodeKind::Update: update(n); return;
      case NodeKind::Binary:
        expression(n->a);
        expression(n->b);
        emit(binary_opcode(n->op), 0, 0, n->line);
        return;
      case NodeKind::Logical: {
        if (n->flags & kFlagLogicalAssign) {
          logical_assignment(n);
          return;
        }
        expression(n->a);
        const Opc jump = n->op == Op::And ? Opc::JumpIfFalseKeep : n->op == Op::Or ? Opc::JumpIfTrueKeep : Opc::JumpIfNotNullishKeep;
        const uint32_t at = emit(jump);
        expression(n->b);
        patch(at);
        return;
      }
      case NodeKind::Conditional: {
        expression(n->a);
        const uint32_t to_else = emit(Opc::JumpIfFalse);
        expression(n->b);
        const uint32_t to_end = emit(Opc::Jump);
        patch(to_else);
        expression(n->c);
        patch(to_end);
        return;
      }
      case NodeKind::Assign: assignment(n); return;
      case NodeKind::Call: call(n, false); return;
      case NodeKind::New: call(n, true); return;
      case NodeKind::OptionalChain: {
        const uint64_t first = chain_jumps_.count();
        chain_marks_.push_back(static_cast<uint32_t>(first));
        expression(n->a);
        end_chain(first, false);
        return;
      }
      case NodeKind::Member:
        expression(n->a);
        if (n->flags & kFlagOptional) {
          const bool g = optional_guard();
          emit(Opc::GetProp, static_cast<int32_t>(constant_string(n->text)), 0, n->line);
          optional_end(g);
        } else {
          emit(Opc::GetProp, static_cast<int32_t>(constant_string(n->text)), 0, n->line);
        }
        return;
      case NodeKind::Index:
        expression(n->a);
        if (n->flags & kFlagOptional) {
          const bool g = optional_guard();
          expression(n->b);
          emit(Opc::GetElem, 0, 0, n->line);
          optional_end(g);
        } else {
          expression(n->b);
          emit(Opc::GetElem, 0, 0, n->line);
        }
        return;
      case NodeKind::Sequence:
        for (Node* e = n->list; e; e = e->next) {
          expression(e);
          if (e->next) emit(Opc::Pop);
        }
        return;
      case NodeKind::Spread: fail("Unexpected spread", n->line); return;
      default: fail("Unsupported expression", n->line); return;
    }
  }

  // `a?.b`: with the object on the stack, nothing nullish is read from; the result is undefined instead.
  // The end of an optional chain that began with chain_jumps_[first]: where a nullish value lands ([x] is on the stack at each jump) and leaves undefined (or true, for delete).
  void end_chain(uint64_t first, bool delete_result) {
    chain_marks_.remove_at(chain_marks_.count() - 1);
    if (chain_jumps_.count() <= first) return;
    const uint32_t to_end = emit(Opc::Jump);
    for (uint64_t i = first; i < chain_jumps_.count(); ++i) patch(chain_jumps_[i]);
    emit(Opc::Pop);
    emit(delete_result ? Opc::PushTrue : Opc::PushUndef);
    patch(to_end);
    while (chain_jumps_.count() > first) chain_jumps_.remove_at(chain_jumps_.count() - 1);
  }
  bool optional_guard() {
    emit(Opc::Dup);
    emit(Opc::PushNull);
    emit(Opc::Ne);
    if (chain_marks_.count()) {   // inside a chain: a nullish value ends the whole chain
      chain_jumps_.push_back(emit(Opc::JumpIfFalse));
      return true;
    }
    optional_jumps_.push_back(emit(Opc::JumpIfFalse));
    return false;
  }
  void optional_end(bool in_chain) {
    if (in_chain) return;
    const uint32_t to_end = emit(Opc::Jump);
    patch(optional_jumps_[optional_jumps_.count() - 1]);
    optional_jumps_.remove_at(optional_jumps_.count() - 1);
    emit(Opc::Pop);
    emit(Opc::PushUndef);
    patch(to_end);
  }

  static Opc binary_opcode(Op op) {
    switch (op) {
      case Op::Add: return Opc::Add;
      case Op::Sub: return Opc::Sub;
      case Op::Mul: return Opc::Mul;
      case Op::Div: return Opc::Div;
      case Op::Mod: return Opc::Mod;
      case Op::Pow: return Opc::Pow;
      case Op::BitAnd: return Opc::BitAnd;
      case Op::BitOr: return Opc::BitOr;
      case Op::BitXor: return Opc::BitXor;
      case Op::Shl: return Opc::Shl;
      case Op::Shr: return Opc::Shr;
      case Op::UShr: return Opc::UShr;
      case Op::Eq: return Opc::Eq;
      case Op::Ne: return Opc::Ne;
      case Op::StrictEq: return Opc::StrictEq;
      case Op::StrictNe: return Opc::StrictNe;
      case Op::Lt: return Opc::Lt;
      case Op::Gt: return Opc::Gt;
      case Op::Le: return Opc::Le;
      case Op::Ge: return Opc::Ge;
      case Op::In: return Opc::In;
      default: return Opc::InstanceOf;
    }
  }

  void unary(Node* n) {
    switch (n->op) {
      case Op::Delete:
        if (n->a->kind == NodeKind::Ident && cur_->code->strict && !library_) {
          fail("Delete of an unqualified identifier in strict mode", n->line);
          return;
        }
        if (n->a->kind == NodeKind::OptionalChain && (n->a->a->kind == NodeKind::Member || n->a->a->kind == NodeKind::Index)) {   // delete a?.b
          Node* link = n->a->a;
          const uint64_t first = chain_jumps_.count();
          chain_marks_.push_back(static_cast<uint32_t>(first));
          expression(link->a);
          const bool g = (link->flags & kFlagOptional) ? optional_guard() : false;
          if (link->kind == NodeKind::Member) {
            emit(Opc::DeleteProp, static_cast<int32_t>(constant_string(link->text)));
          } else {
            expression(link->b);
            emit(Opc::DeleteElem);
          }
          optional_end((link->flags & kFlagOptional) ? g : true);
          end_chain(first, true);
        } else if (n->a->kind == NodeKind::Member) {
          expression(n->a->a);
          emit(Opc::DeleteProp, static_cast<int32_t>(constant_string(n->a->text)));
        } else if (n->a->kind == NodeKind::Index) {
          expression(n->a->a);
          expression(n->a->b);
          emit(Opc::DeleteElem);
        } else {
          expression(n->a);
          emit(Opc::Pop);
          emit(Opc::PushTrue);
        }
        return;
      case Op::TypeOf: {
        uint32_t depth, slot;
        List<uint32_t> ends(vm_.allocator());
        if (n->a->kind == NodeKind::Ident && with_applies(n->a->text)) {   // typeof name, where a with object or the variables of an eval may have it
          for (uint64_t i = withs_.count(); i > 0; --i) {
            const WithCtx ctx = withs_[i - 1];
            if (bound_inside(ctx, n->a->text)) break;
            emit(Opc::PushStr, static_cast<int32_t>(constant_string(n->a->text)), 0, n->line);
            load_plain(ctx.var, n->line);
            emit(binary_opcode(Op::In), 0, 0, n->line);
            const uint32_t no = emit(Opc::JumpIfFalse);
            load_plain(ctx.var, n->line);
            emit(Opc::GetProp, static_cast<int32_t>(constant_string(n->a->text)), 0, n->line);
            emit(Opc::Typeof);
            ends.push_back(emit(Opc::Jump));
            patch(no);
          }
        }
        if (n->a->kind == NodeKind::Ident && !resolve(n->a->text, &depth, &slot) && !n->a->text.equals("undefined")) {
          emit(Opc::TypeofGlobal, static_cast<int32_t>(constant_string(n->a->text)), 0, n->line);
        } else {
          expression(n->a);
          emit(Opc::Typeof);
        }
        for (uint64_t i = 0; i < ends.count(); ++i) patch(ends[i]);
        return;
      }
      case Op::Void:
        expression(n->a);
        emit(Opc::Pop);
        emit(Opc::PushUndef);
        return;
      case Op::Await: expression(n->a); emit(Opc::Await, 0, 0, n->line); return;
      case Op::Yield:
        if (!(n->flags & kFlagDelegate)) {
          if (n->a) expression(n->a);
          else emit(Opc::PushUndef);
          emit(Opc::Yield, 0, 0, n->line);
          return;
        }
        {   // yield* iterable: every value it gives is given on
          expression(n->a);
          emit(Opc::ForOfStart);
          const uint32_t it = new_slot();
          emit(Opc::SetVar, static_cast<int32_t>(it), fn_hops(), n->line);
          emit(Opc::Pop);
          const uint32_t top = pos();
          emit(Opc::GetVar, static_cast<int32_t>(it), fn_hops(), n->line);
          const uint32_t to_end = emit(Opc::IterNext);
          emit(Opc::Yield, 0, 0, n->line);
          emit(Opc::Pop);
          emit(Opc::Jump, static_cast<int32_t>(top));
          patch(to_end);
          emit(Opc::IterReturn);
        }
        return;
      case Op::Not: expression(n->a); emit(Opc::Not); return;
      case Op::Neg: expression(n->a); emit(Opc::Neg, 0, 0, n->line); return;
      case Op::Plus: expression(n->a); emit(Opc::Plus, 0, 0, n->line); return;
      default: expression(n->a); emit(Opc::BitNot, 0, 0, n->line); return;
    }
  }

  void update(Node* n) {
    const bool prefix = (n->flags & kFlagPrefix) != 0;
    const Opc step = n->op == Op::Inc ? Opc::Inc : Opc::Dec;
    Node* t = n->a;
    if (t->kind == NodeKind::Ident) {
      load_name(t->text, n->line);
      if (prefix) {
        emit(step, 0, 0, n->line);
        store_name(t->text, n->line);
      } else {
        emit(Opc::ToNumber, 0, 0, n->line);
        emit(Opc::Dup);
        emit(step, 0, 0, n->line);
        store_name(t->text, n->line);
        emit(Opc::Pop);
      }
    } else if (t->kind == NodeKind::Member) {
      expression(t->a);
      emit(Opc::Dup);
      emit(Opc::GetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
      if (prefix) {
        emit(step, 0, 0, n->line);
        emit(Opc::SetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
      } else {
        emit(Opc::ToNumber, 0, 0, n->line);
        emit(Opc::DupX1);
        emit(step, 0, 0, n->line);
        emit(Opc::SetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
        emit(Opc::Pop);
      }
    } else {
      expression(t->a);
      expression(t->b);
      emit(Opc::Dup2);
      emit(Opc::GetElem, 0, 0, n->line);
      if (prefix) {
        emit(step, 0, 0, n->line);
        emit(Opc::SetElem, 0, 0, n->line);
      } else {
        emit(Opc::ToNumber, 0, 0, n->line);
        emit(Opc::DupX2);
        emit(step, 0, 0, n->line);
        emit(Opc::SetElem, 0, 0, n->line);
        emit(Opc::Pop);
      }
    }
  }

  // a ||= b, a &&= b, a ??= b: the target's object and key are evaluated once, and the store happens only when the value is not short-circuited.
  void logical_assignment(Node* n) {
    Node* t = n->a;
    const Opc jump = n->op == Op::And ? Opc::JumpIfFalseKeep : n->op == Op::Or ? Opc::JumpIfTrueKeep : Opc::JumpIfNotNullishKeep;
    if (t->kind == NodeKind::Ident) {
      load_name(t->text, n->line);
      const uint32_t at = emit(jump);
      expression(n->b);
      store_name(t->text, n->line);
      patch(at);
      return;
    }
    const bool member = t->kind == NodeKind::Member;
    expression(t->a);
    if (member) {
      emit(Opc::Dup);
      emit(Opc::GetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
    } else {
      expression(t->b);
      emit(Opc::Dup2);
      emit(Opc::GetElem, 0, 0, n->line);
    }
    const uint32_t at = emit(jump);
    expression(n->b);
    if (member) emit(Opc::SetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
    else emit(Opc::SetElem, 0, 0, n->line);
    const uint32_t to_end = emit(Opc::Jump);
    patch(at);   // short-circuited: [object, (key,) value] -> [value]
    if (!member) {
      emit(Opc::Swap);
      emit(Opc::Pop);
    }
    emit(Opc::Swap);
    emit(Opc::Pop);
    patch(to_end);
  }

  void assignment(Node* n) {
    Node* t = n->a;
    const bool plain = n->op == Op::Assign;
    if (t->kind == NodeKind::Ident) {
      if (!plain) load_name(t->text, n->line);
      if (plain) named_expression(n->b, t->text); else expression(n->b);
      if (!plain) emit(binary_opcode(n->op), 0, 0, n->line);
      store_name(t->text, n->line);
    } else if (t->kind == NodeKind::Member) {
      expression(t->a);
      if (!plain) {
        emit(Opc::Dup);
        emit(Opc::GetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
      }
      expression(n->b);
      if (!plain) emit(binary_opcode(n->op), 0, 0, n->line);
      emit(Opc::SetProp, static_cast<int32_t>(constant_string(t->text)), 0, n->line);
    } else {
      expression(t->a);
      expression(t->b);
      if (!plain) {
        emit(Opc::Dup2);
        emit(Opc::GetElem, 0, 0, n->line);
      }
      expression(n->b);
      if (!plain) emit(binary_opcode(n->op), 0, 0, n->line);
      emit(Opc::SetElem, 0, 0, n->line);
    }
  }

  bool has_spread(Node* list) {
    for (Node* a = list; a; a = a->next)
      if (a->kind == NodeKind::Spread) return true;
    return false;
  }

  // Pushes the arguments of a call; returns false if they were gathered in an array (spread).
  bool arguments(Node* list, uint32_t* count) {
    *count = 0;
    if (has_spread(list)) {
      emit(Opc::NewArray);
      for (Node* a = list; a; a = a->next) {
        if (a->kind == NodeKind::Spread) {
          expression(a->a);
          emit(Opc::ArraySpread);
        } else {
          expression(a);
          emit(Opc::ArrayPush);
        }
      }
      return false;
    }
    for (Node* a = list; a; a = a->next) {
      expression(a);
      ++*count;
    }
    return true;
  }

  void call(Node* n, bool is_new) {
    Node* callee = n->a;
    const bool optional_call = !is_new && (n->flags & kFlagOptional);
    uint32_t guard_skip = kNoJump;
    if (is_new) {
      expression(callee);
      emit(Opc::PushUndef);
    } else if ((callee->kind == NodeKind::Member || callee->kind == NodeKind::Index) && (callee->flags & kFlagSuper)) {   // super.method(...): looked up on the parent, called on `this`
      expression(callee);
      emit(Opc::PushThis);
    } else if (callee->kind == NodeKind::Member) {
      expression(callee->a);
      if (callee->flags & kFlagOptional) {   // `a?.b(...)`: a nullish `a` skips the whole call
        guard_skip = optional_call_guard();
      }
      emit(Opc::GetMethod, static_cast<int32_t>(constant_string(callee->text)), 0, n->line);
    } else if (callee->kind == NodeKind::Index) {
      expression(callee->a);
      if (callee->flags & kFlagOptional) guard_skip = optional_call_guard();
      expression(callee->b);
      emit(Opc::GetMethodElem, 0, 0, n->line);
    } else {
      expression(callee);
      emit(Opc::PushUndef);
    }
    uint32_t skip = 0;
    if (optional_call) {   // [fn, this]: nothing is called if fn is nullish
      emit(Opc::Swap);
      emit(Opc::Dup);
      emit(Opc::PushNull);
      emit(Opc::Ne);
      skip = emit(Opc::JumpIfFalse);
      emit(Opc::Swap);
    }
    uint32_t count;
    const bool plain = arguments(n->list, &count);
    if (plain && !is_new && callee->kind == NodeKind::Ident && callee->text.equals("eval") && count < 250 && !library_) emit_eval_scope(count, n->line);
    if (plain) emit(is_new ? Opc::New : Opc::Call, static_cast<int32_t>(count), 0, n->line);
    else emit(is_new ? Opc::NewSpread : Opc::CallSpread, 0, 0, n->line);
    if (optional_call) {
      const uint32_t to_end = emit(Opc::Jump);
      patch(skip);
      if (chain_marks_.count()) {   // in a chain: the rest of it is skipped too
        emit(Opc::Pop);
        chain_jumps_.push_back(emit(Opc::Jump));
      } else {
        emit(Opc::Pop);
        emit(Opc::Pop);
        emit(Opc::PushUndef);
      }
      patch(to_end);
    }
    if (guard_skip != kNoJump) {   // the object was nullish: [obj] -> [undefined]
      if (chain_marks_.count()) {
        chain_jumps_.push_back(guard_skip);
      } else {
        const uint32_t to_end = emit(Opc::Jump);
        patch(guard_skip);
        emit(Opc::Pop);
        emit(Opc::PushUndef);
        patch(to_end);
      }
    }
  }

  // eval(...): remembers the variables that can be seen from here, for the case that the function called is the real eval (a direct eval runs in them).
  void emit_eval_scope(uint32_t argc, uint32_t line) {
    uint32_t d, s;
    resolve(StringView("arguments"), &d, &s);   // (the function that encloses the call gets its `arguments` now: the eval cannot add a variable to it later)
    allocators::IAllocator& a = vm_.allocator();
    void* mem = a.allocate(sizeof(EvalScope));
    if (!mem) { fail("Out of memory", line); return; }
    EvalScope* scope = new (mem) EvalScope(a);
    if (!cur_->code->eval_scopes.push_back(scope) || cur_->code->eval_scopes.count() > 60000) { fail("Out of memory", line); return; }
    auto text_of = [&](StringView v, uint32_t* at, uint32_t* len) {
      *at = static_cast<uint32_t>(scope->text.size());
      *len = static_cast<uint32_t>(v.size());
      scope->text.append(v);
    };
    for (FuncState* fs = cur_; fs; fs = fs->parent) {
      EvalFrame frame{static_cast<uint32_t>(scope->bindings.count()), 0, static_cast<uint32_t>(fs->envs.count()), fs->code ? fs->code->strict : false, fs->code ? fs->code->is_arrow : fs->arrow, fs->is_global};
      for (uint64_t i = 0; i < fs->bindings.count(); ++i) {
        const Binding& b = fs->bindings[i];
        EvalBinding e{};
        text_of(b.name, &e.name_at, &e.name_len);
        text_of(b.import_name, &e.import_at, &e.import_len);
        e.slot = b.slot; e.level = b.level; e.lexical = b.lexical; e.is_const = b.is_const;
        scope->bindings.push_back(e);
        ++frame.count;
      }
      scope->frames.push_back(frame);
    }
    emit(Opc::EvalScope, static_cast<int32_t>(cur_->code->eval_scopes.count() - 1), static_cast<uint8_t>(argc), line);
  }

  static constexpr uint32_t kNoJump = 0xFFFFFFFFu;

  // With the object on the stack, jumps (to be patched) when it is nullish; the object stays on the stack.
  uint32_t optional_call_guard() {
    emit(Opc::Dup);
    emit(Opc::PushNull);
    emit(Opc::Ne);
    return emit(Opc::JumpIfFalse);
  }

  Vm& vm_;
  String* error_;
  FuncState* cur_ = nullptr;
  bool failed_ = false;
  bool library_ = false;
  bool local_scope_ = false;
  StringView eval_dynamic_;   // direct eval: the variable that holds the object for the `var`s of a sloppy eval (the calling function's)
  FuncState* eval_parent_ = nullptr;   // direct eval: the innermost of the functions around the call
  bool eval_strict_ = false;
  uint32_t resolved_funcs_ = 0;   // set by resolve(): how many functions out the name was found
  bool resolved_lexical_ = false, resolved_const_ = false;   // set by resolve(): the binding is let / const / class, or const
  StringView resolved_import_;   // set by resolve(): the export an import binding reads (empty for any other)
  uint32_t line_ = 0;
  uint32_t function_level_count_ = 0;   // how many of the current function's bindings are function-level (parameters, var, functions)
  StringView pending_label_;
  StringView infer_name_;   // the name an anonymous function expression takes from where it is stored (var f = function () {}), taken by the next compile_function
  List<WithCtx> withs_;     // the with statements whose body is being compiled
  uint32_t with_counter_ = 0;
  List<uint32_t>* dummy_ = nullptr;
  List<uint32_t> optional_jumps_{vm_.allocator()};
  List<uint32_t> chain_jumps_{vm_.allocator()};   // the nullish exits of the optional chains being compiled (where each chain's begin in chain_marks_)
  List<uint32_t> chain_marks_{vm_.allocator()};
};

}  // namespace

bool compile_eval_program(Vm& vm, Node* program, const EvalScope& scope, FunctionCode** out, String* error) {
  Compiler compiler(vm, error, false, true);
  return compiler.eval_program(program, scope, out);
}

bool compile_program(Vm& vm, Node* program, FunctionCode** out, String* error, bool library, bool local_scope) {
  Compiler compiler(vm, error, library, local_scope);
  return compiler.program(program, out);
}

}  // namespace script

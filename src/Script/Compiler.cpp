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
};

struct Ctx {
  enum Kind : uint8_t { Loop, Switch, Try } kind;
  StringView label;
  uint32_t id;
  bool handler_active;     // Try: an exception handler of the VM is set for the code being compiled
  Node* finalizer;         // Try: the finally block to run on the way out (or null)
  int32_t continue_target; // Loop: where `continue` goes if that is known already (else it is patched)
};

struct Patch {
  uint32_t ctx_id;
  bool is_continue;
  uint32_t instr;
};

struct FuncState {
  explicit FuncState(allocators::IAllocator& a) : bindings(a), scope_marks(a), ctx(a), patches(a) {}
  FunctionCode* code = nullptr;
  FuncState* parent = nullptr;
  List<Binding> bindings;
  List<uint32_t> scope_marks;
  List<Ctx> ctx;
  List<Patch> patches;
  bool is_global = false;
  uint32_t scope_depth = 0;     // blocks entered (0: the function's own top level)
  int32_t completion_slot = -1;
  uint32_t next_ctx_id = 1;
};

class Compiler {
 public:
  Compiler(Vm& vm, String* error) : vm_(vm), error_(error) {}

  bool program(Node* block, FunctionCode** out) {
    FuncState fs(vm_.allocator());
    fs.code = vm_.new_code();
    if (!fs.code) return fail("Out of memory", 0);
    fs.is_global = true;
    cur_ = &fs;
    fs.completion_slot = static_cast<int32_t>(new_slot());
    hoist_globals(block->list);
    init_functions(block->list);
    for (Node* s = block->list; s && !failed_; s = s->next) statement(s);
    emit(Opc::GetVar, fs.completion_slot, 0, 0);
    emit(Opc::Return, 0, 0, 0);
    fs.code->slot_count = next_slot_of(fs);
    cur_ = nullptr;
    if (failed_) return false;
    *out = fs.code;
    return true;
  }

 private:
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
  // A variable of the current block (let / const) or of the function (var, parameters).
  uint32_t declare(StringView name) {
    for (uint64_t i = cur_->bindings.count(); i > 0; --i) {
      if (cur_->bindings[i - 1].name.equals(name) && i - 1 >= (cur_->scope_marks.count() ? cur_->scope_marks[cur_->scope_marks.count() - 1] : 0)) return cur_->bindings[i - 1].slot;
    }
    const uint32_t slot = new_slot();
    cur_->bindings.push_back(Binding{name, slot});
    return slot;
  }

  // Where a name lives: depth = how many functions out, slot; false for a global.
  bool resolve(StringView name, uint32_t* depth, uint32_t* slot) {
    uint32_t d = 0;
    for (FuncState* fs = cur_; fs; fs = fs->parent, ++d) {
      for (uint64_t i = fs->bindings.count(); i > 0; --i) {
        if (fs->bindings[i - 1].name.equals(name)) {
          *depth = d;
          *slot = fs->bindings[i - 1].slot;
          return true;
        }
      }
      if (name.equals("arguments") && !fs->is_global && !fs->code->is_arrow) {
        FuncState* saved = cur_;
        cur_ = fs;
        const uint32_t s = new_slot();
        fs->bindings.insert(0, Binding{name, s});   // (visible everywhere in the function)
        for (uint64_t m = 0; m < fs->scope_marks.count(); ++m) ++fs->scope_marks[m];
        fs->code->arguments_slot = static_cast<int32_t>(s);
        cur_ = saved;
        *depth = d;
        *slot = s;
        return true;
      }
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

  // At the start of a statement list: the functions declared in it exist.
  void init_functions(Node* list) {
    for (Node* s = list; s; s = s->next) {
      if (s->kind != NodeKind::FunctionDecl) continue;
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
        for (Node* d = s->list; d; d = d->next) declare(d->text);
  }

  // --- Variables
  void load_name(StringView name, uint32_t line) {
    uint32_t depth, slot;
    if (resolve(name, &depth, &slot)) {
      if (depth > 250) {
        fail("Functions are nested too deeply", line);
        return;
      }
      emit(Opc::GetVar, static_cast<int32_t>(slot), static_cast<uint8_t>(depth), line);
    } else if (name.equals("undefined")) {
      emit(Opc::PushUndef);
    } else {
      emit(Opc::GetGlobal, static_cast<int32_t>(constant_string(name)), 0, line);
    }
  }
  // Stores the value on the top of the stack into a name, leaving the value there.
  void store_name(StringView name, uint32_t line) {
    uint32_t depth, slot;
    if (resolve(name, &depth, &slot)) {
      if (depth > 250) {
        fail("Functions are nested too deeply", line);
        return;
      }
      emit(Opc::SetVar, static_cast<int32_t>(slot), static_cast<uint8_t>(depth), line);
    } else {
      emit(Opc::SetGlobal, static_cast<int32_t>(constant_string(name)), 0, line);
    }
  }

  // --- Functions
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
    fs.parent = cur_;
    FuncState* saved = cur_;
    const uint32_t saved_level = function_level_count_;
    function_level_count_ = 0;
    cur_ = &fs;
    fs.code->is_arrow = (fn->flags & kFlagArrow) != 0;
    fs.code->is_async = (fn->flags & kFlagAsync) != 0;
    fs.code->is_generator = (fn->flags & kFlagGenerator) != 0 && !(fn->flags & kFlagArrow);
    fs.code->name = fn->text.empty() ? nullptr : vm_.intern(fn->text);
    uint32_t params = 0;
    for (Node* p = fn->list; p; p = p->next) {
      const uint32_t slot = new_slot();   // (a parameter is slot `params`: they come first)
      fs.bindings.push_back(Binding{p->text, slot});
      ++params;
    }
    function_level_count_ = static_cast<uint32_t>(fs.bindings.count());
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
    // default values of parameters
    uint32_t index = 0;
    for (Node* p = fn->list; p; p = p->next, ++index) {
      if (!p->a) continue;
      emit(Opc::GetVar, static_cast<int32_t>(index), 0, p->line);
      emit(Opc::PushUndef);
      emit(Opc::StrictEq);
      const uint32_t skip = emit(Opc::JumpIfFalse);
      expression(p->a);
      emit(Opc::SetVar, static_cast<int32_t>(index), 0, p->line);
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
      init_functions(fn->a->list);
      declare_lexicals(fn->a->list);
      for (Node* s = fn->a->list; s && !failed_; s = s->next) statement(s);
      emit(Opc::PushUndef);
      emit(Opc::Return);
    }
    cur_ = saved;
    function_level_count_ = saved_level;
    return failed_ ? nullptr : fs.code;
  }

  // --- Control flow contexts
  Ctx& push_ctx(Ctx::Kind kind, bool handler_active = false, Node* finalizer = nullptr) {
    Ctx c{kind, pending_label_, cur_->next_ctx_id++, handler_active, finalizer, -1};
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
      case NodeKind::FunctionDecl: return;   // (made when its list began)
      case NodeKind::ExprStmt:
        expression(n->a);
        if (cur_->is_global && cur_->parent == nullptr && cur_->completion_slot >= 0) {
          emit(Opc::SetVar, cur_->completion_slot, 0, n->line);
        }
        emit(Opc::Pop);
        return;
      case NodeKind::Var: var_statement(n); return;
      case NodeKind::Block:
        push_scope();
        init_functions(n->list);
        declare_lexicals(n->list);
        statement_list(n->list);
        pop_scope();
        return;
      case NodeKind::If: {
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
        for (uint64_t i = 0; i < cur_->ctx.count(); ++i) has_finally = has_finally || (cur_->ctx[i].kind == Ctx::Try && cur_->ctx[i].finalizer);
        if (has_finally) {
          const uint32_t tmp = new_slot();
          emit(Opc::SetVar, static_cast<int32_t>(tmp), 0, n->line);
          emit(Opc::Pop);
          unwind_to(-1);
          emit(Opc::GetVar, static_cast<int32_t>(tmp), 0, n->line);
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
        Ctx& c = cur_->ctx[static_cast<uint64_t>(target)];
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
        expression(d->a);
      } else if (n->var_kind == VarKind::Var) {
        continue;   // `var x;` does nothing more than the declaration
      } else {
        emit(Opc::PushUndef);
      }
      if (n->var_kind != VarKind::Var && !global_scope()) {
        uint32_t depth, slot;
        if (resolve(d->text, &depth, &slot) && depth == 0) emit(Opc::SetVar, static_cast<int32_t>(slot), 0, d->line);
        else store_name(d->text, d->line);
      } else {
        store_name(d->text, d->line);
      }
      emit(Opc::Pop);
    }
  }

  void for_statement(Node* n) {
    push_scope();
    if (n->a) {
      if (n->a->kind == NodeKind::Var) {
        if (n->a->var_kind != VarKind::Var && !global_scope())
          for (Node* d = n->a->list; d; d = d->next) declare(d->text);
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
    if (n->c) {
      expression(n->c);
      emit(Opc::Pop);
    }
    emit(Opc::Jump, static_cast<int32_t>(top));
    if (has_cond) patch(to_end);
    pop_ctx(static_cast<int32_t>(cont));
    pop_scope();
  }

  void for_each_statement(Node* n) {
    push_scope();
    const bool of = n->kind == NodeKind::ForOf;
    Node* target = n->a;
    StringView name;
    bool declared_here = false;
    if (target->kind == NodeKind::Var) {
      name = target->list->text;
      if (target->var_kind != VarKind::Var && !global_scope()) {
        declare(name);
        declared_here = true;
      }
    }
    (void)declared_here;
    expression(n->b);
    emit(of ? Opc::ForOfStart : Opc::ForInStart);
    const uint32_t it = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(it), 0, n->line);
    emit(Opc::Pop);
    push_ctx(Ctx::Loop);
    const uint32_t top = pos();
    emit(Opc::GetVar, static_cast<int32_t>(it), 0, n->line);
    const uint32_t to_end = emit(Opc::IterNext);
    if (target->kind == NodeKind::Var || target->kind == NodeKind::Ident) {
      if (target->kind == NodeKind::Ident) name = target->text;
      store_name(name, n->line);
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
    emit(Opc::Jump, static_cast<int32_t>(top));
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
    uint32_t to_end2 = 0;
    bool second = false;
    if (has_catch) {
      patch(begin);   // the exception is on the stack here
      push_scope();
      if (n->b) {
        const uint32_t slot = declare(n->b->text);
        emit(Opc::SetVar, static_cast<int32_t>(slot), 0, n->line);
      }
      emit(Opc::Pop);
      uint32_t rethrow_begin = 0;
      if (has_finally) {
        rethrow_begin = emit(Opc::TryBegin);
        push_ctx(Ctx::Try, true, n->d);
      }
      statement(n->c);
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
    patch(to_end);
    if (second) patch(to_end2);
  }

  // The exception is on the stack: keep it, run the finally block, throw it again.
  void finally_rethrow(Node* finalizer) {
    const uint32_t tmp = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(tmp), 0, finalizer->line);
    emit(Opc::Pop);
    statement(finalizer);
    emit(Opc::GetVar, static_cast<int32_t>(tmp), 0, finalizer->line);
    emit(Opc::Throw);
  }

  void switch_statement(Node* n) {
    push_scope();
    expression(n->a);
    const uint32_t disc = new_slot();
    emit(Opc::SetVar, static_cast<int32_t>(disc), 0, n->line);
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
      emit(Opc::GetVar, static_cast<int32_t>(disc), 0, c->line);
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
        const double d = n->number;
        if (d == static_cast<double>(static_cast<int32_t>(d)) && !(d == 0 && 1 / d < 0)) emit(Opc::PushInt, static_cast<int32_t>(d));
        else emit(Opc::PushNum, static_cast<int32_t>(constant_number(d)));
        return;
      }
      case NodeKind::String: emit(Opc::PushStr, static_cast<int32_t>(constant_string(n->text))); return;
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
            expression(e);
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
          } else {
            expression(p->a);
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
      case NodeKind::Member:
        expression(n->a);
        if (n->flags & kFlagOptional) {
          optional_guard();
          emit(Opc::GetProp, static_cast<int32_t>(constant_string(n->text)), 0, n->line);
          optional_end();
        } else {
          emit(Opc::GetProp, static_cast<int32_t>(constant_string(n->text)), 0, n->line);
        }
        return;
      case NodeKind::Index:
        expression(n->a);
        if (n->flags & kFlagOptional) {
          optional_guard();
          expression(n->b);
          emit(Opc::GetElem, 0, 0, n->line);
          optional_end();
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
  void optional_guard() {
    emit(Opc::Dup);
    emit(Opc::PushNull);
    emit(Opc::Ne);
    optional_jumps_.push_back(emit(Opc::JumpIfFalse));
  }
  void optional_end() {
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
        if (n->a->kind == NodeKind::Member) {
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
        if (n->a->kind == NodeKind::Ident && !resolve(n->a->text, &depth, &slot) && !n->a->text.equals("undefined")) {
          emit(Opc::TypeofGlobal, static_cast<int32_t>(constant_string(n->a->text)), 0, n->line);
        } else {
          expression(n->a);
          emit(Opc::Typeof);
        }
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
          emit(Opc::SetVar, static_cast<int32_t>(it), 0, n->line);
          emit(Opc::Pop);
          const uint32_t top = pos();
          emit(Opc::GetVar, static_cast<int32_t>(it), 0, n->line);
          const uint32_t to_end = emit(Opc::IterNext);
          emit(Opc::Yield, 0, 0, n->line);
          emit(Opc::Pop);
          emit(Opc::Jump, static_cast<int32_t>(top));
          patch(to_end);
          emit(Opc::PushUndef);
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
      expression(n->b);
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
    if (plain) emit(is_new ? Opc::New : Opc::Call, static_cast<int32_t>(count), 0, n->line);
    else emit(is_new ? Opc::NewSpread : Opc::CallSpread, 0, 0, n->line);
    if (optional_call) {
      const uint32_t to_end = emit(Opc::Jump);
      patch(skip);
      emit(Opc::Pop);
      emit(Opc::Pop);
      emit(Opc::PushUndef);
      patch(to_end);
    }
    if (guard_skip != kNoJump) {   // the object was nullish: [obj] -> [undefined]
      const uint32_t to_end = emit(Opc::Jump);
      patch(guard_skip);
      emit(Opc::Pop);
      emit(Opc::PushUndef);
      patch(to_end);
    }
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
  uint32_t line_ = 0;
  uint32_t function_level_count_ = 0;   // how many of the current function's bindings are function-level (parameters, var, functions)
  StringView pending_label_;
  List<uint32_t>* dummy_ = nullptr;
  List<uint32_t> optional_jumps_{vm_.allocator()};
};

}  // namespace

bool compile_program(Vm& vm, Node* program, FunctionCode** out, String* error) {
  Compiler compiler(vm, error);
  return compiler.program(program, out);
}

}  // namespace script

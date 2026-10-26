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

// The syntax tree the parser builds and the compiler reads, and the arena it lives in (all of a program's nodes are freed together).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace script {

enum class NodeKind : uint8_t {
  // expressions
  Number, String, Template, Ident, This, Null, True, False, ArrayLit, ObjectLit, Property, Function, Unary, Update, Binary, Logical, Assign, Conditional,
  Call, New, Member, Index, Sequence, Spread,
  // statements
  Var, Declarator, ExprStmt, Block, If, For, ForIn, ForOf, While, DoWhile, Return, Break, Continue, Throw, Try, Switch, Case, FunctionDecl, Empty, Labeled,
  Import,   // import declaration of a module (number: index of the request; list: Declarators, text = local name, a = String node with the imported name or null for `* as`)
};

// Operators and punctuation (what Node::op holds, and what the lexer reads).
enum class Op : uint8_t {
  None,
  Add, Sub, Mul, Div, Mod, Pow, BitAnd, BitOr, BitXor, Shl, Shr, UShr,
  Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge, In, InstanceOf,
  And, Or, Nullish,
  Not, BitNot, Neg, Plus, TypeOf, Void, Delete, Await,
  Inc, Dec,
  Assign, Yield,
};

enum NodeFlag : uint8_t {
  kFlagPrefix = 1,       // Update: ++x
  kFlagComputed = 2,     // Property: [key]: value
  kFlagShorthand = 4,    // Property: { name }
  kFlagOptional = 8,     // Member / Index / Call: ?.
  kFlagPatternAssign = 64,   // Call: a lowered `pattern = value`; c is the original Assign, for when it turns out to be a nested pattern with a default
  kFlagArrow = 1,        // Function
  kFlagExprBody = 2,     // Function: arrow with an expression as its body
  kFlagRest = 4,         // Function: the last parameter is ...rest
  kFlagExpression = 8,   // Function: a function expression (its own name is visible inside)
  kFlagAsync = 16,       // Function: async
  kFlagGenerator = 32,   // Function: function* (the same bit as the Property flags: they are on other kinds of node)
  kFlagDelegate = 1,     // Unary Yield: yield* (the same bit as kFlagPrefix)
  kFlagGetter = 32,      // Property: get name() { ... }
  kFlagSetter = 64,      // Property: set name(v) { ... }
  kFlagLogicalAssign = 1,   // Logical: a ||= b - a is the target (evaluated once), b the value
  kFlagSuper = 128,      // Member / Index: super.name (the object the method is looked up on is the prototype of the parent class; `this` stays the receiver)
};

enum class VarKind : uint8_t { Var, Let, Const };

struct Node {
  NodeKind kind;
  Op op = Op::None;
  uint8_t flags = 0;
  uint32_t line = 0;
  double number = 0;
  StringView text;
  Node *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
  Node* list = nullptr;   // the first of a list of children (arguments, statements, elements, properties, declarators)
  Node* next = nullptr;   // the next in the list this node is in
  VarKind var_kind = VarKind::Var;
  explicit Node(NodeKind k) : kind(k) {}
};

// Memory for nodes and for the text they point to (the decoded text of string literals).
class Arena {
 public:
  explicit Arena(allocators::IAllocator& allocator) : allocator_(allocator), blocks_(allocator) {}
  ~Arena() {
    for (uint64_t i = 0; i < blocks_.count(); ++i) allocator_.deallocate(blocks_[i]);
  }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  allocators::IAllocator& allocator() const { return allocator_; }

  Node* make(NodeKind kind, uint32_t line) {
    void* mem = allocate(sizeof(Node));
    if (!mem) return nullptr;
    Node* n = new (mem) Node(kind);
    n->line = line;
    return n;
  }
  // A copy of `text` (NUL-terminated); empty view for nothing or when memory ran out.
  StringView copy(StringView text) {
    char* mem = static_cast<char*>(allocate(text.size() + 1));
    if (!mem) return StringView();
    for (uint64_t i = 0; i < text.size(); ++i) mem[i] = text[i];
    mem[text.size()] = 0;
    return StringView(mem, text.size());
  }
  void* allocate(uint64_t size) {
    if (size > UINT64_MAX - 15) return nullptr;
    size = (size + 15) & ~uint64_t{15};
    if (size > kBlock) {   // (a long string literal: a block of its own, so the current block keeps its room)
      void* big = allocator_.allocate(size);
      if (!big) return nullptr;
      if (!blocks_.push_back(big)) {
        allocator_.deallocate(big);
        return nullptr;
      }
      return big;
    }
    if (!current_ || used_ + size > kBlock) {
      current_ = static_cast<char*>(allocator_.allocate(kBlock));
      if (!current_ || !blocks_.push_back(current_)) return nullptr;
      used_ = 0;
    }
    void* p = current_ + used_;
    used_ += size;
    return p;
  }

 private:
  static constexpr uint64_t kBlock = 32 * 1024;
  allocators::IAllocator& allocator_;
  List<void*> blocks_;
  char* current_ = nullptr;
  uint64_t used_ = 0;
};

// What a module asks for: the module specifiers of its import and export-from declarations, in the order they first appear (the dependencies array the
// module function is called with follows that order).
struct ModuleRequests {
  explicit ModuleRequests(allocators::IAllocator& allocator) : specifiers(allocator), imports(allocator), exports(allocator), stars(allocator) {}
  List<String> specifiers;
  List<String> imports;   // "request:name" for each name the module imports (or re-exports) from another: checked when the modules are linked
  List<String> exports;   // the names it exports itself (not through export *)
  List<String> stars;     // "request" of each `export * from`

};

// Parses a module. The result is a program (a Block) whose one statement is the module as an async function expression
// `async function (__deps, __exports, import.meta) { ... }`: the import declarations are Import nodes at the head of its body, and the exports are
// defined at its start (each as an accessor on __exports: __export_get(__exports, name, () => local), __export_star(__exports, namespace)).
// nullptr (with *error set) for a syntax error.
Node* parse_module(StringView source, Arena& arena, String* error, ModuleRequests* requests);

// Parses a whole program: a Block node whose list holds its statements. nullptr (with *error set to "message (line N)") for a syntax error.
Node* parse_program(StringView source, Arena& arena, String* error);

}  // namespace script

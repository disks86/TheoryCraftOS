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

#include "Kernel/Dxgkrnl/DxilBitcode.hpp"
#include "Utility/Move.hpp"

namespace dxgk {
namespace dxil {

namespace {

constexpr uint32_t kMaxDepth = 8, kMaxRecordOperands = 1u << 14, kMaxRecords = 1u << 21, kMaxAbbrevs = 128, kMaxAbbrevOps = 64;

// --- The bits -------------------------------------------------------------------------------------------------------------------------------------------------------

class BitReader {
 public:
  explicit BitReader(ConstByteView data) : data_(data) {}
  uint64_t position() const { return position_; }
  uint64_t bit_size() const { return uint64_t{data_.size()} * 8; }
  bool at_end() const { return position_ >= bit_size(); }
  bool read(uint32_t bits, uint64_t* out) {
    if (bits > 64 || bits > bit_size() - position_) return false;
    uint64_t value = 0;
    for (uint32_t done = 0; done < bits;) {
      const uint64_t byte_index = (position_ + done) / 8;
      const uint32_t shift = static_cast<uint32_t>((position_ + done) % 8);
      uint8_t byte = 0;
      if (!data_.try_get(byte_index, &byte)) return false;
      const uint32_t take = bits - done < 8 - shift ? bits - done : 8 - shift;
      value |= (uint64_t{static_cast<uint32_t>(byte >> shift)} & ((1u << take) - 1)) << done;
      done += take;
    }
    position_ += bits;
    *out = value;
    return true;
  }
  // A variable bit rate number of chunks of `width` bits (the top bit of a chunk says another follows).
  bool vbr(uint32_t width, uint64_t* out) {
    if (width < 2 || width > 32) return false;
    uint64_t value = 0, chunk = 0;
    const uint64_t high = uint64_t{1} << (width - 1);
    for (uint32_t shift = 0;; shift += width - 1) {
      if (!read(width, &chunk)) return false;
      const uint64_t payload = chunk & (high - 1);
      if (shift >= 64) {
        if (payload) return false;   // (more than 64 bits of number)
      } else {
        if (shift > 0 && ((payload << shift) >> shift) != payload) return false;
        value |= payload << shift;
      }
      if (!(chunk & high)) break;
    }
    *out = value;
    return true;
  }
  bool align32() {
    const uint64_t aligned = (position_ + 31) / 32 * 32;
    if (aligned > bit_size()) return false;
    position_ = aligned;
    return true;
  }
  bool skip_bytes(uint64_t bytes, ConstByteView* out) {
    if (position_ % 8 != 0 || bytes > (bit_size() - position_) / 8) return false;
    if (!data_.try_subview(position_ / 8, bytes, out)) return false;
    position_ += bytes * 8;
    return true;
  }

 private:
  ConstByteView data_;
  uint64_t position_ = 0;
};

// --- Abbreviations ----------------------------------------------------------------------------------------------------------------------------------------------------

enum class OpKind : uint8_t { Literal, Fixed, Vbr, Array, Char6, Blob };
struct AbbrevOp {
  OpKind kind = OpKind::Literal;
  uint64_t value = 0;
};
struct Abbrev {
  uint32_t start = 0, count = 0;   // in the pool
};
struct BlockInfoAbbrev {
  uint32_t block_id;
  Abbrev abbrev;
};

uint8_t char6(uint64_t v) {
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
  return static_cast<uint8_t>(kChars[v & 63]);
}

class Parser {
 public:
  Parser(allocators::IAllocator& allocator, ConstByteView bytes, BitstreamVisitor& visitor)
      : reader_(bytes), visitor_(visitor), pool_(allocator), info_(allocator), operands_(allocator), allocator_(allocator) {}

  NtStatus run() {
    uint64_t magic = 0;
    if (!reader_.read(32, &magic) || magic != 0xDEC04342u) return status::kInvalidParameter;   // 'B' 'C' 0xC0 0xDE
    while (!reader_.at_end()) {
      uint64_t id = 0;
      if (!reader_.read(2, &id)) return status::kInvalidParameter;
      if (id != 1) return status::kInvalidParameter;   // (only blocks at the top level)
      if (!enter_block(0, 1)) return status::kInvalidParameter;
    }
    return status::kSuccess;
  }

 private:
  bool define_abbrev(List<Abbrev>& into) {
    uint64_t count = 0;
    if (!reader_.vbr(5, &count) || count == 0 || count > kMaxAbbrevOps) return false;
    Abbrev abbrev;
    abbrev.start = static_cast<uint32_t>(pool_.count());
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t literal = 0;
      if (!reader_.read(1, &literal)) return false;
      AbbrevOp op;
      if (literal) {
        op.kind = OpKind::Literal;
        if (!reader_.vbr(8, &op.value)) return false;
      } else {
        uint64_t encoding = 0;
        if (!reader_.read(3, &encoding) || encoding < 1 || encoding > 5) return false;
        op.kind = static_cast<OpKind>(encoding);
        if ((op.kind == OpKind::Fixed || op.kind == OpKind::Vbr) && (!reader_.vbr(5, &op.value) || op.value > 64)) return false;
      }
      if (!pool_.push_back(op)) return false;
    }
    abbrev.count = static_cast<uint32_t>(count);
    // (an array is followed by its element type, a blob is last)
    for (uint32_t i = 0; i < abbrev.count; ++i) {
      const OpKind kind = pool_[abbrev.start + i].kind;
      if (kind == OpKind::Array && i + 2 != abbrev.count) return false;
      if (kind == OpKind::Blob && i + 1 != abbrev.count) return false;
    }
    return into.count() < kMaxAbbrevs && into.push_back(abbrev);
  }

  bool read_scalar(const AbbrevOp& op, uint64_t* out) {
    switch (op.kind) {
      case OpKind::Literal: *out = op.value; return true;
      case OpKind::Fixed: return reader_.read(static_cast<uint32_t>(op.value), out);
      case OpKind::Vbr: return op.value == 0 ? (*out = 0, true) : reader_.vbr(static_cast<uint32_t>(op.value), out);
      case OpKind::Char6: {
        uint64_t v = 0;
        if (!reader_.read(6, &v)) return false;
        *out = char6(v);
        return true;
      }
      default: return false;
    }
  }

  // One abbreviated record: its code and operands.
  bool read_abbreviated(const Abbrev& abbrev, uint32_t* code, ConstByteView* blob) {
    operands_.clear();
    bool first = true;
    uint64_t code_value = 0;
    for (uint32_t i = 0; i < abbrev.count; ++i) {
      const AbbrevOp op = pool_[abbrev.start + i];
      if (op.kind == OpKind::Array) {
        uint64_t length = 0;
        if (!reader_.vbr(6, &length) || length > kMaxRecordOperands) return false;
        const AbbrevOp element = pool_[abbrev.start + i + 1];
        for (uint64_t k = 0; k < length; ++k) {
          uint64_t v = 0;
          if (!read_scalar(element, &v)) return false;
          if (first) {
            code_value = v;
            first = false;
          } else if (!operands_.push_back(v)) {
            return false;
          }
        }
        break;
      }
      if (op.kind == OpKind::Blob) {
        uint64_t length = 0;
        if (!reader_.vbr(6, &length) || !reader_.align32() || !reader_.skip_bytes(length, blob) || !reader_.align32()) return false;
        break;
      }
      uint64_t v = 0;
      if (!read_scalar(op, &v)) return false;
      if (first) {
        code_value = v;
        first = false;
      } else if (operands_.count() >= kMaxRecordOperands || !operands_.push_back(v)) {
        return false;
      }
    }
    if (first || code_value > 0xFFFFFFFFu) return false;
    *code = static_cast<uint32_t>(code_value);
    return true;
  }

  bool enter_block(uint32_t parent, uint32_t depth) {
    (void)parent;
    if (depth > kMaxDepth) return false;
    uint64_t block_id = 0, width = 0, words = 0;
    if (!reader_.vbr(8, &block_id) || !reader_.vbr(4, &width) || !reader_.align32() || !reader_.read(32, &words)) return false;
    if (width < 2 || width > 32 || block_id > 0xFFFF) return false;
    const uint64_t end = reader_.position() + words * 32;
    if (end > reader_.bit_size()) return false;
    const uint32_t id = static_cast<uint32_t>(block_id);
    if (!visitor_.enter_block(id)) return false;
    List<Abbrev> local(allocator_);
    for (const BlockInfoAbbrev& entry : info_.view())
      if (entry.block_id == id && !local.push_back(entry.abbrev)) return false;
    int64_t current_bid = -1;   // (the BLOCKINFO block: the block the abbreviations that follow are for)
    for (;;) {
      if (++records_ > kMaxRecords) return false;
      uint64_t abbrev_id = 0;
      if (!reader_.read(static_cast<uint32_t>(width), &abbrev_id)) return false;
      if (abbrev_id == 0) {   // END_BLOCK
        if (!reader_.align32() || reader_.position() != end) return false;
        return visitor_.exit_block(id);
      }
      if (abbrev_id == 1) {   // ENTER_SUBBLOCK
        if (!enter_block(id, depth + 1)) return false;
        continue;
      }
      if (abbrev_id == 2) {   // DEFINE_ABBREV
        if (id == 0) {
          if (current_bid < 0) return false;
          List<Abbrev> one(allocator_);
          if (!define_abbrev(one) || !info_.push_back({static_cast<uint32_t>(current_bid), one[0]})) return false;
        } else if (!define_abbrev(local)) {
          return false;
        }
        continue;
      }
      uint32_t code = 0;
      ConstByteView blob;
      if (abbrev_id == 3) {   // UNABBREV_RECORD
        uint64_t code_value = 0, count = 0;
        if (!reader_.vbr(6, &code_value) || !reader_.vbr(6, &count) || count > kMaxRecordOperands || code_value > 0xFFFFFFFFu) return false;
        operands_.clear();
        for (uint64_t i = 0; i < count; ++i) {
          uint64_t v = 0;
          if (!reader_.vbr(6, &v) || !operands_.push_back(v)) return false;
        }
        code = static_cast<uint32_t>(code_value);
      } else {
        if (abbrev_id - 4 >= local.count() || !read_abbreviated(local[abbrev_id - 4], &code, &blob)) return false;
      }
      if (id == 0) {
        if (code == 1 && operands_.count() >= 1) current_bid = static_cast<int64_t>(operands_[0] & 0xFFFF);   // SETBID
        continue;
      }
      if (!visitor_.record(id, code, ArrayView<const uint64_t>(operands_.view().data(), operands_.count()), blob)) return false;
    }
  }

  BitReader reader_;
  BitstreamVisitor& visitor_;
  List<AbbrevOp> pool_;
  List<BlockInfoAbbrev> info_;
  List<uint64_t> operands_;
  allocators::IAllocator& allocator_;
  uint32_t records_ = 0;
};

}  // namespace

NtStatus parse_bitstream(allocators::IAllocator& allocator, ConstByteView bytes, BitstreamVisitor& visitor) {
  Parser parser(allocator, bytes, visitor);
  return parser.run();
}


// --- The module -----------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

enum BlockId : uint32_t { kModule = 8, kParamAttr = 9, kParamAttrGroup = 10, kConstants = 11, kFunctionBlock = 12, kIdentification = 13, kValueSymtab = 14, kMetadata = 15, kMetadataAttachment = 16,
                          kTypes = 17, kUseList = 18, kModuleStrtab = 19, kGlobalValSummary = 20, kOperandBundleTags = 21, kMetadataKind = 22 };

constexpr uint32_t kMaxTypes = 4096, kMaxValues = 1u << 16, kMaxFunctions = 256, kMaxInstructions = 1u << 16, kMaxBlocksPerFunction = 4096, kMaxMeta = 1u << 14;

class ModuleBuilder final : public BitstreamVisitor {
 public:
  explicit ModuleBuilder(Module& module) : m_(module), stack_(module.allocator()) {}

  NtStatus status() const { return status_; }

  bool enter_block(uint32_t id) override {
    if (stack_.count() >= kMaxDepth || !stack_.push_back(id)) return fail(status::kInvalidParameter);
    if (id == kFunctionBlock) return begin_function();
    if (id == kConstants) current_type_ = 0;
    return true;
  }

  bool exit_block(uint32_t id) override {
    stack_.remove_at(stack_.count() - 1);
    if (id == kFunctionBlock) return end_function();
    if (id == kModule) module_done_ = true;
    return true;
  }

  bool record(uint32_t block, uint32_t code, ArrayView<const uint64_t> ops, ConstByteView) override {
    switch (block) {
      case kTypes: return type_record(code, ops);
      case kModule: return module_record(code, ops);
      case kConstants: return constant_record(code, ops);
      case kMetadata: return metadata_record(code, ops);
      case kValueSymtab: return symtab_record(code, ops);
      case kFunctionBlock: return instruction_record(code, ops);
      default: return true;
    }
  }

  bool done() const { return module_done_; }

 private:
  bool fail(NtStatus s) {
    if (status_ == status::kSuccess) status_ = s;
    return false;
  }
  bool in_function() const {
    for (uint32_t b : stack_.view())
      if (b == kFunctionBlock) return true;
    return false;
  }

  // --- types
  bool type_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (code == 1 || code == 19) return true;   // NUMENTRY, STRUCT_NAME
    Type t;
    switch (code) {
      case 2: t.kind = TypeKind::Void; break;
      case 3: t.kind = TypeKind::Float; t.width = 32; break;
      case 4: t.kind = TypeKind::Double; t.width = 64; break;
      case 10: t.kind = TypeKind::Half; t.width = 16; break;
      case 5: t.kind = TypeKind::Label; break;
      case 16: t.kind = TypeKind::Metadata; break;
      case 7:   // INTEGER [width]
        if (ops.size() < 1 || ops[0] == 0 || ops[0] > 64) return fail(status::kNotSupported);
        t.kind = TypeKind::Int, t.width = static_cast<uint32_t>(ops[0]);
        break;
      case 8:   // POINTER [pointee, address space]
        if (ops.size() < 1 || ops[0] >= m_.types.count() + 1) return fail(status::kInvalidParameter);   // (the pointee may be a type that comes later: not a forward reference we need)
        t.kind = TypeKind::Pointer, t.first = static_cast<uint32_t>(ops[0]);
        break;
      case 11: case 12:   // ARRAY / VECTOR [length, element]
        if (ops.size() < 2 || ops[0] > 0xFFFFFF) return fail(status::kInvalidParameter);
        t.kind = code == 11 ? TypeKind::Array : TypeKind::Vector, t.count = static_cast<uint32_t>(ops[0]), t.first = static_cast<uint32_t>(ops[1]);
        break;
      case 18: case 20: {   // STRUCT_ANON / STRUCT_NAMED [packed, elements...]
        if (ops.size() < 1) return fail(status::kInvalidParameter);
        t.kind = TypeKind::Struct, t.count = static_cast<uint32_t>(ops.size() - 1), t.first = static_cast<uint32_t>(m_.type_refs.count());
        for (uint64_t i = 1; i < ops.size(); ++i)
          if (!m_.type_refs.push_back(static_cast<uint32_t>(ops[i]))) return fail(status::kNoMemory);
        break;
      }
      case 21: {   // FUNCTION [vararg, return type, parameters...]
        if (ops.size() < 2) return fail(status::kInvalidParameter);
        t.kind = TypeKind::Function, t.result = static_cast<uint32_t>(ops[1]), t.count = static_cast<uint32_t>(ops.size() - 2), t.first = static_cast<uint32_t>(m_.type_refs.count());
        for (uint64_t i = 2; i < ops.size(); ++i)
          if (!m_.type_refs.push_back(static_cast<uint32_t>(ops[i]))) return fail(status::kNoMemory);
        break;
      }
      default: t.kind = TypeKind::Other; break;
    }
    if (m_.types.count() >= kMaxTypes || !m_.types.push_back(t)) return fail(status::kNotSupported);
    return true;
  }

  // --- values
  bool add_value(uint32_t type, const Constant& constant) {
    if (m_.value_types.count() >= kMaxValues) return fail(status::kNotSupported);
    if (!m_.value_types.push_back(type) || !m_.constants.push_back(constant)) return fail(status::kNoMemory);
    return true;
  }

  bool module_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (code == 7 || code == 9 || code == 14) return add_value(0, Constant());   // GLOBALVAR, ALIAS: a value the translator does not use
    if (code == 8) {                                                            // FUNCTION [type, cc, isproto, ...]
      if (ops.size() < 3 || m_.functions.count() >= kMaxFunctions) return fail(status::kInvalidParameter);
      Function f(m_.allocator());
      f.value = static_cast<uint32_t>(m_.value_types.count());
      f.type = static_cast<uint32_t>(ops[0]);
      f.declaration = ops[2] != 0;
      if (!add_value(f.type, Constant()) || !m_.functions.push_back(static_cast<Function&&>(f))) return fail(status::kNoMemory);
    }
    return true;
  }

  bool constant_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (code == 1) {   // SETTYPE
      if (ops.size() < 1) return fail(status::kInvalidParameter);
      current_type_ = static_cast<uint32_t>(ops[0]);
      return true;
    }
    Constant c;
    c.type = current_type_;
    switch (code) {
      case 2: c.kind = ConstantKind::Null; break;
      case 3: c.kind = ConstantKind::Undef; break;
      case 4:   // INTEGER (sign in the lowest bit)
        if (ops.size() < 1) return fail(status::kInvalidParameter);
        c.kind = ConstantKind::Int;
        c.bits = (ops[0] & 1) ? (~(ops[0] >> 1) + 1) : (ops[0] >> 1);
        break;
      case 6:   // FLOAT [bits]
        if (ops.size() < 1) return fail(status::kInvalidParameter);
        c.kind = ConstantKind::Float, c.bits = ops[0];
        break;
      case 7: c.kind = ConstantKind::Aggregate; break;
      default: c.kind = ConstantKind::Other; break;
    }
    return add_value(current_type_, c);
  }

  bool symtab_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (in_function() || code != 1 || ops.size() < 1) return true;   // (names in function bodies matter to nothing here)
    for (Function& f : m_.functions.view())
      if (f.value == ops[0]) {
        for (uint64_t i = 1; i < ops.size(); ++i)
          if (!f.name.push_back(static_cast<char>(ops[i]))) return fail(status::kNoMemory);
      }
    return true;
  }

  // --- metadata
  bool metadata_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (in_function()) return true;
    if (code == 6) return true;   // KIND
    if (code == 4) {              // NAME: the name of the NAMED_NODE that follows
      pending_name_.clear();
      for (uint64_t op : ops) pending_name_.push_back(static_cast<char>(op));
      return true;
    }
    if (code == 10) {             // NAMED_NODE [node ids]
      NamedMeta named(m_.allocator());
      if (!named.name.append(pending_name_.view())) return fail(status::kNoMemory);
      named.first = static_cast<uint32_t>(m_.meta_refs.count());
      named.count = static_cast<uint32_t>(ops.size());
      for (uint64_t op : ops)
        if (!m_.meta_refs.push_back(static_cast<uint32_t>(op))) return fail(status::kNoMemory);
      return m_.named.count() < 64 && m_.named.push_back(static_cast<NamedMeta&&>(named)) ? true : fail(status::kNotSupported);
    }
    Meta node;
    if (code == 1) {              // STRING_OLD
      String text(m_.allocator());
      for (uint64_t op : ops)
        if (!text.push_back(static_cast<char>(op))) return fail(status::kNoMemory);
      node.kind = MetaKind::String, node.value = static_cast<uint32_t>(m_.strings.count());
      if (!m_.strings.push_back(static_cast<String&&>(text))) return fail(status::kNoMemory);
    } else if (code == 2) {       // VALUE [type, value]
      if (ops.size() < 2) return fail(status::kInvalidParameter);
      node.kind = MetaKind::Value, node.value = static_cast<uint32_t>(ops[1]);
    } else if (code == 3) {       // NODE [ids + 1]
      node.kind = MetaKind::Node, node.first = static_cast<uint32_t>(m_.meta_refs.count()), node.count = static_cast<uint32_t>(ops.size());
      for (uint64_t op : ops)
        if (!m_.meta_refs.push_back(static_cast<uint32_t>(op))) return fail(status::kNoMemory);
    } else {
      return true;
    }
    return m_.meta.count() < kMaxMeta && m_.meta.push_back(node) ? true : fail(status::kNotSupported);
  }

  // --- function bodies
  bool begin_function() {
    // the next function with a body
    while (next_body_ < m_.functions.count() && m_.functions[next_body_].declaration) ++next_body_;
    if (next_body_ >= m_.functions.count()) return fail(status::kInvalidParameter);
    if (!module_values_set_) {
      m_.module_values = static_cast<uint32_t>(m_.value_types.count());
      module_values_set_ = true;
    }
    Function& f = m_.functions[next_body_];
    current_ = next_body_++;
    f.declaration = false;
    f.first_value = static_cast<uint32_t>(m_.value_types.count());
    f.first_instruction = static_cast<uint32_t>(m_.instructions.count());
    f.first_block_start = static_cast<uint32_t>(m_.blocks.count());
    const Type* ft = m_.type_at(f.type);
    if (!ft || ft->kind != TypeKind::Function) return fail(status::kInvalidParameter);
    for (uint32_t i = 0; i < ft->count; ++i) {
      uint32_t type = 0;
      if (ft->first + i < m_.type_refs.count()) type = m_.type_refs[ft->first + i];
      if (!add_value(type, Constant())) return false;
    }
    instruction_number_ = static_cast<uint32_t>(m_.value_types.count());
    block_open_ = false;
    blocks_declared_ = 0;
    return true;
  }

  bool end_function() {
    Function& f = m_.functions[current_];
    f.instruction_count = static_cast<uint32_t>(m_.instructions.count()) - f.first_instruction;
    if (block_open_) close_block();
    f.block_count = static_cast<uint32_t>(m_.blocks.count()) - f.first_block_start;
    if (f.block_count != blocks_declared_ && blocks_declared_ != 0) return fail(status::kInvalidParameter);
    // the function-local values are kept in the function and go away from the module's tables
    f.local_first = static_cast<uint32_t>(m_.local_types.count());
    for (uint64_t i = m_.module_values; i < m_.value_types.count(); ++i)
      if (!m_.local_types.push_back(m_.value_types[i]) || !m_.local_constants.push_back(m_.constants[i])) return fail(status::kNoMemory);
    f.local_count = static_cast<uint32_t>(m_.value_types.count() - m_.module_values);
    while (m_.value_types.count() > m_.module_values) m_.value_types.remove_at(m_.value_types.count() - 1);
    while (m_.constants.count() > m_.module_values) m_.constants.remove_at(m_.constants.count() - 1);
    return true;
  }

  void close_block() {
    Block& b = m_.blocks[m_.blocks.count() - 1];
    b.count = static_cast<uint32_t>(m_.instructions.count()) - b.first;
    block_open_ = false;
  }

  // The value an operand names: relative to the instruction number, and a forward reference (an id not below it) is followed by a type.
  bool operand(ArrayView<const uint64_t> ops, uint64_t* at, uint32_t* value, bool with_type) {
    if (*at >= ops.size()) return false;
    const uint32_t id = static_cast<uint32_t>(instruction_number_ - static_cast<uint32_t>(ops[(*at)++]));
    if (with_type && id >= instruction_number_) {
      if (*at >= ops.size()) return false;
      ++*at;   // (the type of a value defined later)
    }
    *value = id;
    return true;
  }
  uint32_t type_of(uint32_t value) const { return value < m_.value_types.count() ? m_.value_types[value] : 0; }
  uint32_t bool_type() const {
    for (uint64_t i = 0; i < m_.types.count(); ++i)
      if (m_.types[i].kind == TypeKind::Int && m_.types[i].width == 1) return static_cast<uint32_t>(i);
    return 0;
  }

  bool push_instruction(Instruction& ins, bool has_value) {
    if (m_.instructions.count() >= kMaxInstructions) return fail(status::kNotSupported);
    if (!block_open_) {
      if (m_.blocks.count() - m_.functions[current_].first_block_start >= kMaxBlocksPerFunction) return fail(status::kNotSupported);
      Block b;
      b.first = static_cast<uint32_t>(m_.instructions.count());
      if (!m_.blocks.push_back(b)) return fail(status::kNoMemory);
      block_open_ = true;
    }
    ins.has_value = has_value;
    ins.result = has_value ? instruction_number_ : ~0u;
    if (has_value && !add_value(ins.type, Constant())) return false;
    if (!m_.instructions.push_back(ins)) return fail(status::kNoMemory);
    return true;
  }
  bool terminate(Instruction& ins, bool has_value) {
    if (!push_instruction(ins, has_value)) return false;
    close_block();
    return true;
  }

  bool instruction_record(uint32_t code, ArrayView<const uint64_t> ops) {
    if (code == 1) {   // DECLAREBLOCKS
      if (ops.size() < 1 || ops[0] == 0 || ops[0] > kMaxBlocksPerFunction) return fail(status::kInvalidParameter);
      blocks_declared_ = static_cast<uint32_t>(ops[0]);
      return true;
    }
    if (stack_.count() && stack_[stack_.count() - 1] != kFunctionBlock) return true;
    if (code == 33 || code == 35) return true;   // DEBUG_LOC(_AGAIN)
    instruction_number_ = static_cast<uint32_t>(m_.value_types.count());   // (arguments, then constants, then the results of instructions, each next to the other)
    Instruction ins;
    uint64_t at = 0;
    switch (code) {
      case 2: {   // BINOP
        ins.op = Op::Binop;
        if (!operand(ops, &at, &ins.operands[0], true) || !operand(ops, &at, &ins.operands[1], false) || at >= ops.size()) return fail(status::kInvalidParameter);
        ins.code = static_cast<uint32_t>(ops[at]);
        ins.type = type_of(ins.operands[0]);
        return push_instruction(ins, true);
      }
      case 3: {   // CAST
        ins.op = Op::Cast;
        if (!operand(ops, &at, &ins.operands[0], true) || at + 2 > ops.size()) return fail(status::kInvalidParameter);
        ins.type = static_cast<uint32_t>(ops[at]);
        ins.code = static_cast<uint32_t>(ops[at + 1]);
        return push_instruction(ins, true);
      }
      case 28: {  // CMP2
        ins.op = Op::Cmp;
        if (!operand(ops, &at, &ins.operands[0], true) || !operand(ops, &at, &ins.operands[1], false) || at >= ops.size()) return fail(status::kInvalidParameter);
        ins.code = static_cast<uint32_t>(ops[at]);
        ins.type = bool_type();
        return push_instruction(ins, true);
      }
      case 29: {  // VSELECT [true, false, condition]
        ins.op = Op::Select;
        if (!operand(ops, &at, &ins.operands[0], true) || !operand(ops, &at, &ins.operands[1], false) || !operand(ops, &at, &ins.operands[2], true)) return fail(status::kInvalidParameter);
        ins.type = type_of(ins.operands[0]);
        return push_instruction(ins, true);
      }
      case 26: {  // EXTRACTVAL [aggregate, index]
        ins.op = Op::ExtractValue;
        if (!operand(ops, &at, &ins.operands[0], true) || at >= ops.size() || ops.size() - at != 1) return fail(status::kInvalidParameter);
        ins.index = static_cast<uint32_t>(ops[at]);
        const Type* t = m_.type_at(type_of(ins.operands[0]));
        if (t && t->kind == TypeKind::Struct && ins.index < t->count && t->first + ins.index < m_.type_refs.count()) ins.type = m_.type_refs[t->first + ins.index];
        return push_instruction(ins, true);
      }
      case 34: {  // CALL [attrs, cc, (fmf), function type, callee, arguments...]
        ins.op = Op::Call;
        if (ops.size() < 4) return fail(status::kInvalidParameter);
        const uint64_t cc = ops[1];
        at = 2;
        if (cc & (1u << 17)) ++at;   // fast-math flags
        if (!(cc & (1u << 15)) || at >= ops.size()) return fail(status::kNotSupported);   // (an explicit function type, as the writer of this version always gives)
        const uint32_t function_type = static_cast<uint32_t>(ops[at++]);
        if (!operand(ops, &at, &ins.operands[0], true)) return fail(status::kInvalidParameter);
        const Type* ft = m_.type_at(function_type);
        if (!ft || ft->kind != TypeKind::Function || ft->count > kMaxCallArguments) return fail(status::kNotSupported);
        ins.arg_count = ft->count;
        for (uint32_t i = 0; i < ft->count; ++i)
          if (!operand(ops, &at, &ins.args[i], false)) return fail(status::kInvalidParameter);
        if (at != ops.size()) return fail(status::kNotSupported);
        ins.type = ft->result;
        const Type* result = m_.type_at(ft->result);
        const bool value = result && result->kind != TypeKind::Void;
        return push_instruction(ins, value);
      }
      case 11: {  // BR [true block, false block, condition] or [block]
        if (ops.size() == 1) {
          ins.op = Op::Br;
          ins.blocks[0] = static_cast<uint32_t>(ops[0]);
        } else if (ops.size() == 3) {
          ins.op = Op::CondBr;
          ins.blocks[0] = static_cast<uint32_t>(ops[0]), ins.blocks[1] = static_cast<uint32_t>(ops[1]);
          at = 2;
          if (!operand(ops, &at, &ins.operands[0], false)) return fail(status::kInvalidParameter);
        } else {
          return fail(status::kInvalidParameter);
        }
        return terminate(ins, false);
      }
      case 10: {  // RET [] or [value]
        ins.op = Op::Ret;
        if (ops.size() > 0 && !operand(ops, &at, &ins.operands[0], true)) return fail(status::kInvalidParameter);
        return terminate(ins, false);
      }
      case 15: ins.op = Op::Unreachable; return terminate(ins, false);
      case 16: {  // PHI [type, (signed relative value, block)...]
        ins.op = Op::Phi;
        if (ops.size() < 3 || (ops.size() - 1) % 2 != 0) return fail(status::kInvalidParameter);
        ins.type = static_cast<uint32_t>(ops[0]);
        ins.phi_first = static_cast<uint32_t>(m_.phi_values.count());
        ins.phi_count = static_cast<uint32_t>((ops.size() - 1) / 2);
        for (uint64_t i = 1; i + 1 < ops.size(); i += 2) {
          const uint64_t raw = ops[i];
          const int64_t relative = (raw & 1) ? -static_cast<int64_t>(raw >> 1) : static_cast<int64_t>(raw >> 1);
          const uint32_t value = static_cast<uint32_t>(static_cast<int64_t>(instruction_number_) - relative);
          if (!m_.phi_values.push_back(value) || !m_.phi_blocks.push_back(static_cast<uint32_t>(ops[i + 1]))) return fail(status::kNoMemory);
        }
        return push_instruction(ins, true);
      }
      case 12: ins.op = Op::Switch; return fail(status::kNotSupported);
      case 19: ins.op = Op::Alloca; return push_instruction(ins, true);   // (numbered; the translator refuses it)
      case 20: ins.op = Op::Load; return push_instruction(ins, true);
      case 43: ins.op = Op::Gep; return push_instruction(ins, true);
      case 44: ins.op = Op::Store; return push_instruction(ins, false);
      default: return fail(status::kNotSupported);
    }
  }

  Module& m_;
  List<uint32_t> stack_;
  NtStatus status_ = status::kSuccess;
  uint32_t current_type_ = 0;
  uint64_t next_body_ = 0, current_ = 0;
  uint32_t instruction_number_ = 0;
  bool block_open_ = false, module_values_set_ = false, module_done_ = false;
  uint32_t blocks_declared_ = 0;
  String pending_name_{m_.allocator()};
};

}  // namespace

NtStatus read_module(allocators::IAllocator& allocator, ConstByteView bitcode, Module* out) {
  ModuleBuilder builder(*out);
  const NtStatus parsed = parse_bitstream(allocator, bitcode, builder);
  if (builder.status() != status::kSuccess) return builder.status();
  if (parsed != status::kSuccess || !builder.done()) return status::kInvalidParameter;
  return status::kSuccess;
}

}  // namespace dxil
}  // namespace dxgk

/* spirv.inc, part 2: reading the module and finding what the entry point uses. */

static void imm_leaf(Leaf *l, sp_u8 n, sp_u8 cls, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  sp_zero(l, sizeof(Leaf));
  l->op.file = OF_IMM;
  l->op.ncomp = n;
  l->op.imm[0] = a, l->op.imm[1] = b, l->op.imm[2] = c, l->op.imm[3] = d;
  l->op.swz[0] = 0, l->op.swz[1] = 1, l->op.swz[2] = 2, l->op.swz[3] = 3;
  l->n = n;
  l->cls = cls;
}

static Val *new_val(Mod *m, uint32_t type, uint32_t nleaf) {
  Val *v = (Val *)sp_alloc(m, sizeof(Val));
  if (!v) return 0;
  v->type = type;
  v->nleaf = nleaf;
  /* (room for a few leaves more than the shape: a handler that reads a leaf its operand does not have reads zeros) */
  v->leaf = (Leaf *)sp_alloc(m, ((sp_u64)nleaf + 4) * sizeof(Leaf));
  if (!v->leaf) return 0;
  for (uint32_t i = 0; i < nleaf + 4; ++i) imm_leaf(&v->leaf[i], 1, CLS_F, 0, 0, 0, 0);
  return v;
}

static int spec_value(Mod *m, uint32_t id, uint32_t *value) {
  uint32_t spec = 0;
  if (!deco_find(m, id, 0xFFFFFFFFu, DecoSpecId, &spec)) return 0;
  for (uint32_t i = 0; i < m->spec_count; ++i)
    if (m->spec_ids[i] == spec) {
      *value = m->spec_values[i];
      return 1;
    }
  return 0;
}

static int make_constant(Mod *m, uint32_t off) {
  uint32_t op = OPC(off), len = LEN(off);
  uint32_t type = m->w[off + 1], id = m->w[off + 2];
  Type *t = get_type(m, type);
  if (!t) return sp_fail(m, "constant of an unknown type");
  int n = type_nleaf(m, type);
  if (n < 0) return 1;
  Val *v = new_val(m, type, (uint32_t)n);
  if (!v) return sp_fail(m, "out of memory");
  if (op == OpConstantTrue || op == OpConstantFalse || op == OpSpecConstantTrue || op == OpSpecConstantFalse) {
    uint32_t on = (op == OpConstantTrue || op == OpSpecConstantTrue);
    uint32_t sv;
    if ((op == OpSpecConstantTrue || op == OpSpecConstantFalse) && spec_value(m, id, &sv)) on = sv != 0;
    imm_leaf(&v->leaf[0], 1, CLS_B, on ? 0xFFFFFFFFu : 0, 0, 0, 0);
  } else if (op == OpConstant || op == OpSpecConstant) {
    if (t->kind != T_INT && t->kind != T_FLOAT) return sp_fail(m, "constant of a wide type");
    uint32_t value = len > 3 ? m->w[off + 3] : 0, sv;
    if (op == OpSpecConstant && spec_value(m, id, &sv)) value = sv;
    imm_leaf(&v->leaf[0], 1, t->cls, value, 0, 0, 0);
  } else if (op == OpConstantNull) {
    Leaf shape[SP_MAX_ID_LEAVES];
    uint32_t count = 0;
    if (leaves_of(m, type, shape, &count, SP_MAX_ID_LEAVES)) return 1;
    for (uint32_t i = 0; i < count; ++i) imm_leaf(&v->leaf[i], shape[i].n, shape[i].cls, 0, 0, 0, 0);
  } else {   /* a composite */
    if (t->kind == T_VEC) {
      uint32_t comps[4] = {0, 0, 0, 0};
      Type *e = get_type(m, t->elem);
      for (uint32_t i = 0; i < len - 3 && i < 4; ++i) {
        uint32_t member = m->w[off + 3 + i];
        if (member >= m->bound || !m->vals[member] || !m->vals[member]->nleaf) return sp_fail(m, "constant composite member");
        comps[i] = m->vals[member]->leaf[0].op.imm[0];
      }
      imm_leaf(&v->leaf[0], (sp_u8)t->count, e->cls, comps[0], comps[1], comps[2], comps[3]);
    } else {
      uint32_t k = 0;
      for (uint32_t i = 0; i < len - 3; ++i) {
        uint32_t member = m->w[off + 3 + i];
        if (member >= m->bound || !m->vals[member]) return sp_fail(m, "constant composite member");
        Val *mv = m->vals[member];
        for (uint32_t j = 0; j < mv->nleaf; ++j) {
          if (k >= v->nleaf) return sp_fail(m, "constant composite shape");
          v->leaf[k++] = mv->leaf[j];
        }
      }
    }
  }
  set_val(m, id, v);
  return 0;
}

static int make_type(Mod *m, uint32_t off) {
  uint32_t op = OPC(off), len = LEN(off);
  uint32_t id = m->w[off + 1];
  if (id >= m->bound || m->types[id]) return sp_fail(m, "type id out of range or defined twice");
  Type *t = (Type *)sp_alloc(m, sizeof(Type));
  if (!t) return sp_fail(m, "out of memory");
  t->nleaf = -1;
  switch (op) {
    case OpTypeVoid: t->kind = T_VOID; break;
    case OpTypeBool: t->kind = T_BOOL, t->cls = CLS_B, t->width = 32; break;
    case OpTypeInt:
      t->kind = T_INT, t->width = (sp_u8)m->w[off + 2], t->sign = (sp_u8)m->w[off + 3];
      t->cls = t->sign ? CLS_I : CLS_U;
      if (m->w[off + 2] != 32) return sp_fail(m, "integers of other widths than 32 bits");
      break;
    case OpTypeFloat:
      t->kind = T_FLOAT, t->cls = CLS_F, t->width = (sp_u8)m->w[off + 2];
      if (m->w[off + 2] != 32) return sp_fail(m, "floats of other widths than 32 bits");
      break;
    case OpTypeVector: {
      t->kind = T_VEC, t->elem = m->w[off + 2], t->count = m->w[off + 3];
      Type *e = get_type(m, t->elem);
      if (!e || t->count < 2 || t->count > 4) return sp_fail(m, "bad vector type");
      t->cls = e->cls;
      break;
    }
    case OpTypeMatrix: {
      t->kind = T_MAT, t->elem = m->w[off + 2], t->count = m->w[off + 3];
      Type *e = get_type(m, t->elem);
      if (!e || e->kind != T_VEC || t->count < 2 || t->count > 4) return sp_fail(m, "bad matrix type");
      t->cls = CLS_F;
      break;
    }
    case OpTypeArray: {
      t->kind = T_ARRAY, t->elem = m->w[off + 2];
      uint32_t length_id = m->w[off + 3];
      if (!get_type(m, t->elem) || length_id >= m->bound || !m->vals[length_id] || !m->vals[length_id]->nleaf) return sp_fail(m, "bad array type");
      t->count = m->vals[length_id]->leaf[0].op.imm[0];
      if (t->count == 0 || t->count > 4096) return sp_fail(m, "array length");
      t->cls = get_type(m, t->elem)->cls;
      break;
    }
    case OpTypeRuntimeArray:
      t->kind = T_ARRAY, t->elem = m->w[off + 2], t->count = 0;
      if (!get_type(m, t->elem)) return sp_fail(m, "bad array type");
      break;
    case OpTypeStruct: {
      t->kind = T_STRUCT, t->nmem = len - 2;
      t->mem = (uint32_t *)sp_alloc(m, (sp_u64)(t->nmem ? t->nmem : 1) * 4);
      if (!t->mem) return sp_fail(m, "out of memory");
      for (uint32_t i = 0; i < t->nmem; ++i) {
        t->mem[i] = m->w[off + 2 + i];
        if (!get_type(m, t->mem[i])) return sp_fail(m, "struct member of a type not declared yet");
      }
      break;
    }
    case OpTypePointer: t->kind = T_PTR, t->storage = m->w[off + 2], t->elem = m->w[off + 3]; break;
    case OpTypeImage:
      t->kind = T_IMAGE, t->elem = m->w[off + 2], t->dim = m->w[off + 3], t->depth = m->w[off + 4], t->arrayed = m->w[off + 5], t->ms = m->w[off + 6], t->sampled = m->w[off + 7];
      break;
    case OpTypeSampler: t->kind = T_SAMPLER; break;
    case OpTypeSampledImage: t->kind = T_SAMPLED, t->elem = m->w[off + 2]; break;
    case OpTypeFunction: t->kind = T_FUNC, t->elem = m->w[off + 2]; break;
    default: return 0;
  }
  m->types[id] = t;
  return 0;
}

static int parse_functions(Mod *m) {
  uint32_t count = 0;
  for (uint32_t off = 5; off < m->nw; off += LEN(off))
    if (OPC(off) == OpFunction) ++count;
  m->funcs = (Func *)sp_alloc(m, (sp_u64)(count + 1) * sizeof(Func));
  m->blk = (Block **)sp_alloc(m, (sp_u64)m->bound * sizeof(Block *));
  if (!m->funcs || !m->blk) return sp_fail(m, "out of memory");
  Func *f = 0;
  Block *b = 0;
  for (uint32_t off = 5; off < m->nw; off += LEN(off)) {
    uint32_t op = OPC(off);
    if (op == OpFunction) {
      f = &m->funcs[m->nfuncs++];
      f->id = m->w[off + 2], f->offset = off, f->ret_type = m->w[off + 1];
      b = 0;
    } else if (op == OpFunctionParameter && f) {
      ++f->nparams;
    } else if (op == OpFunctionEnd) {
      f = 0, b = 0;
    } else if (f && op == OpLabel) {
      b = (Block *)sp_alloc(m, sizeof(Block));
      if (!b) return sp_fail(m, "out of memory");
      b->label = m->w[off + 1], b->start = off + LEN(off), b->order = f->nblocks;
      if (b->label >= m->bound) return sp_fail(m, "label out of range");
      m->blk[b->label] = b;
      if (!f->nblocks) f->first_label = b->label;
      f->last_label = b->label;
      ++f->nblocks;
    } else if (f && b) {
      switch (op) {
        case OpSelectionMerge: b->merge_kind = 1, b->merge = m->w[off + 1]; break;
        case OpLoopMerge: b->merge_kind = 2, b->merge = m->w[off + 1], b->cont = m->w[off + 2]; break;
        case OpBranch: b->term = off; break;
        case OpBranchConditional: b->term = off; break;
        case OpSwitch: b->term = off; break;
        case OpKill: case OpTerminateInvocation: case OpDemoteToHelperInvocation: case OpUnreachable: b->term = off; break;
        case OpReturn: case OpReturnValue: b->term = off, ++f->nreturns; break;
        default: break;
      }
    }
  }
  for (uint32_t i = 0; i < m->nfuncs; ++i) {   /* predecessor counts */
    Func *fn = &m->funcs[i];
    uint32_t l = fn->first_label;
    for (uint32_t off = fn->offset; off < m->nw; off += LEN(off)) {
      uint32_t op = OPC(off);
      (void)l;
      if (op == OpFunctionEnd) break;
      if (op == OpBranch && m->w[off + 1] < m->bound && m->blk[m->w[off + 1]]) ++m->blk[m->w[off + 1]]->npred;
      else if (op == OpBranchConditional) {
        for (int k = 2; k <= 3; ++k)
          if (m->w[off + k] < m->bound && m->blk[m->w[off + k]]) ++m->blk[m->w[off + k]]->npred;
      } else if (op == OpSwitch) {
        uint32_t words = LEN(off);
        if (m->w[off + 2] < m->bound && m->blk[m->w[off + 2]]) ++m->blk[m->w[off + 2]]->npred;
        for (uint32_t k = 4; k < words; k += 2)
          if (m->w[off + k] < m->bound && m->blk[m->w[off + k]]) ++m->blk[m->w[off + k]]->npred;
      }
    }
  }
  return 0;
}

SpvModule *spv_open(const uint32_t *words, uint32_t count, const SpvAllocator *allocator) {
  if (!words || count < 6 || words[0] != 0x07230203u || !allocator || !allocator->alloc) return 0;
  Mod boot;
  sp_zero(&boot, sizeof boot);
  boot.a = *allocator;
  Mod *m = (Mod *)sp_alloc(&boot, sizeof(Mod));
  if (!m) {
    if (boot.blocks) {
      void *block = boot.blocks;
      while (block) {
        void *next = *(void **)block;
        allocator->release(allocator->user, block);
        block = next;
      }
    }
    return 0;
  }
  *m = boot;
  /* A copy with zero words after it: the decoding reads a few words past the one it is on and must not leave the buffer on a module that is cut short. */
  uint32_t *copy = (uint32_t *)sp_alloc(m, ((sp_u64)count + 16) * 4);
  if (!copy) {
    spv_close(m);
    return 0;
  }
  sp_copy(copy, words, (sp_u64)count * 4);
  m->w = copy, m->nw = count, m->bound = words[3];
  if (m->bound == 0 || m->bound > (1u << 20) || words[1] > 0x00010600u) {
    spv_close(m);
    return 0;
  }
  m->types = (Type **)sp_alloc(m, (sp_u64)m->bound * sizeof(Type *));
  m->vals = (Val **)sp_alloc(m, (sp_u64)m->bound * sizeof(Val *));
  m->inst = (uint32_t *)sp_alloc(m, (sp_u64)m->bound * 4);
  if (!m->types || !m->vals || !m->inst) {
    spv_close(m);
    return 0;
  }
  return m;
}

void spv_close(SpvModule *m) {
  if (!m) return;
  SpvAllocator a = m->a;
  void *block = m->blocks;
  while (block) {
    void *next = *(void **)block;
    a.release(a.user, block);
    block = next;
  }
}

const char *spv_error(const SpvModule *m) { return m->error[0] ? m->error : "unknown error"; }
uint32_t spv_resource_count(const SpvModule *m) { return m->nres; }
SpvResource *spv_resource(SpvModule *m, uint32_t index) { return index < m->nres ? &m->res[index] : 0; }
uint32_t spv_input_mask(const SpvModule *m) { return m->input_mask; }
uint32_t spv_output_mask(const SpvModule *m) { return m->output_mask; }
uint32_t spv_uses_frag_coord(const SpvModule *m) { return m->frag_coord; }

/* The pointee of a variable's pointer type, stripping the arrays of resources. */
static uint32_t strip_arrays(Mod *m, uint32_t tid, uint32_t *array_size) {
  Type *t = get_type(m, tid);
  *array_size = 1;
  while (t && t->kind == T_ARRAY && t->count) {
    *array_size *= t->count;
    tid = t->elem;
    t = get_type(m, tid);
  }
  return tid;
}

static int string_words(const uint32_t *w, uint32_t max) {
  for (uint32_t i = 0; i < max; ++i)
    for (int b = 0; b < 4; ++b)
      if (((w[i] >> (8 * b)) & 0xFF) == 0) return (int)i + 1;
  return -1;
}

static int name_equals(const uint32_t *w, uint32_t words, const char *name) {
  for (uint32_t i = 0; i < words * 4; ++i) {
    char c = (char)((w[i / 4] >> (8 * (i % 4))) & 0xFF);
    if (c != *name) return 0;
    if (!c) return 1;
    ++name;
  }
  return 0;
}

static void note_io(Mod *m, uint32_t var, uint32_t storage) {
  uint32_t builtin = 0, loc = 0;
  Val *dummy = 0;
  (void)dummy;
  uint32_t ptr_type = m->w[m->inst[var] + 1];
  Type *pt = get_type(m, ptr_type);
  if (!pt || pt->kind != T_PTR) return;
  uint32_t pointee = pt->elem;
  Type *t = get_type(m, pointee);
  if (deco_find(m, var, 0xFFFFFFFFu, DecoBuiltIn, &builtin)) {
    if (builtin == BuiltInFragCoord) m->frag_coord = 1;
    if (builtin == BuiltInVertexIndex || builtin == BuiltInVertexId) m->uses_vertex_id = 1;
    if (builtin == BuiltInInstanceIndex || builtin == BuiltInInstanceId) m->uses_instance_id = 1;
    return;
  }
  if (t && t->kind == T_STRUCT) return;   /* gl_PerVertex */
  if (!deco_find(m, var, 0xFFFFFFFFu, DecoLocation, &loc)) return;
  uint32_t array = 1;
  uint32_t elem = strip_arrays(m, pointee, &array);
  Type *et = get_type(m, elem);
  uint32_t slots = array;
  if (et && et->kind == T_MAT) slots *= et->count;
  for (uint32_t i = 0; i < slots && loc + i < 32; ++i) {
    if (storage == SCInput) m->input_mask |= 1u << (loc + i);
    else m->output_mask |= 1u << (loc + i);
  }
}

int spv_reflect(SpvModule *m, uint32_t stage, const char *entry_name, const uint32_t *spec_ids, const uint32_t *spec_values, uint32_t spec_count) {
  m->stage = stage;
  m->spec_ids = spec_ids, m->spec_values = spec_values, m->spec_count = spec_count;
  if (stage != SPV_STAGE_VERTEX && stage != SPV_STAGE_FRAGMENT && stage != SPV_STAGE_COMPUTE) return sp_fail(m, "unsupported shader stage");
  /* Every instruction is whole. */
  for (uint32_t off = 5; off < m->nw; off += LEN(off))
    if (LEN(off) == 0 || off + LEN(off) > m->nw) return sp_fail(m, "truncated module");
  /* The module's declarations in order. */
  uint32_t entry_off = 0;
  for (uint32_t off = 5; off < m->nw;) {
    uint32_t len = LEN(off), op = OPC(off);
    if (len == 0 || off + len > m->nw) return sp_fail(m, "truncated module");
    switch (op) {
      case OpDecorate:
        if (len < 3) return sp_fail(m, "bad decoration");
        add_deco(m, m->w[off + 1], 0xFFFFFFFFu, m->w[off + 2], len > 3 ? m->w[off + 3] : 0);
        break;
      case OpMemberDecorate:
        if (len < 4) return sp_fail(m, "bad decoration");
        add_deco(m, m->w[off + 1], m->w[off + 2], m->w[off + 3], len > 4 ? m->w[off + 4] : 0);
        break;
      case OpEntryPoint: {
        if (len < 4) return sp_fail(m, "bad entry point");
        if (m->w[off + 1] != stage) break;
        int sw = string_words(&m->w[off + 3], len - 3);
        if (sw < 0) return sp_fail(m, "bad entry point name");
        if (!entry_off && (!entry_name || name_equals(&m->w[off + 3], (uint32_t)sw, entry_name))) {
          entry_off = off;
          m->entry_func = m->w[off + 2];
          m->ninterface = 0;
          for (uint32_t i = 3 + (uint32_t)sw; i < len && m->ninterface < 64; ++i) m->interface_ids[m->ninterface++] = m->w[off + i];
        }
        break;
      }
      case OpExecutionMode:
        if (len >= 6 && m->w[off + 2] == 17 && m->w[off + 1] == m->entry_func) {
          m->local_size[0] = m->w[off + 3], m->local_size[1] = m->w[off + 4], m->local_size[2] = m->w[off + 5];
        }
        if (m->w[off + 2] == 12) m->depth_replacing = 1;
        break;
      default:
        if ((op >= OpTypeVoid && op <= OpTypeSampledImage) || op == OpTypeArray || op == OpTypeRuntimeArray || op == OpTypeStruct || op == OpTypePointer || op == OpTypeFunction) {
          if (make_type(m, off)) return 1;
          if (m->w[off + 1] < m->bound) m->inst[m->w[off + 1]] = off;
        } else if ((op >= OpConstantTrue && op <= OpConstantNull && op != 45) || (op >= OpSpecConstantTrue && op <= OpSpecConstantComposite)) {
          if (make_constant(m, off)) return 1;
          if (m->w[off + 2] < m->bound) m->inst[m->w[off + 2]] = off;
        } else if (op == OpSpecConstantOp) {
          return sp_fail(m, "OpSpecConstantOp");
        } else if (op == OpVariable) {
          if (m->w[off + 2] < m->bound) m->inst[m->w[off + 2]] = off;
        } else if (op == OpFunction) {
          goto functions;
        }
        break;
    }
    off += len;
  }
functions:
  if (!entry_off) return sp_fail(m, "the entry point is not in the module");
  if (m->failed) return 1;
  if (parse_functions(m)) return 1;
  /* The variables the entry point reaches. */
  sp_u8 *used = (sp_u8 *)sp_alloc(m, m->bound);
  sp_u8 *seen_func = (sp_u8 *)sp_alloc(m, m->nfuncs + 1);
  if (!used || !seen_func) return sp_fail(m, "out of memory");
  uint32_t stack[32], sp_top = 0;
  stack[sp_top++] = m->entry_func;
  while (sp_top) {
    uint32_t fid = stack[--sp_top];
    Func *fn = 0;
    uint32_t fi = 0;
    for (; fi < m->nfuncs; ++fi)
      if (m->funcs[fi].id == fid) {
        fn = &m->funcs[fi];
        break;
      }
    if (!fn) return sp_fail(m, "call of a function that is not defined");
    if (seen_func[fi]) continue;
    seen_func[fi] = 1;
    for (uint32_t off = fn->offset; off < m->nw && OPC(off) != OpFunctionEnd; off += LEN(off)) {
      uint32_t op = OPC(off), len = LEN(off);
      switch (op) {
        case OpLoad: case OpAccessChain: case OpInBoundsAccessChain: case OpCopyObject:
          if (m->w[off + 3] < m->bound) used[m->w[off + 3]] = 1;
          break;
        case OpStore:
          if (m->w[off + 1] < m->bound) used[m->w[off + 1]] = 1;
          break;
        case OpCopyMemory:
          used[m->w[off + 1]] = 1, used[m->w[off + 2]] = 1;
          break;
        case OpFunctionCall:
          for (uint32_t k = 4; k < len; ++k)
            if (m->w[off + k] < m->bound) used[m->w[off + k]] = 1;
          if (sp_top < 32) stack[sp_top++] = m->w[off + 3];
          break;
        default: break;
      }
    }
  }
  /* Global variables: the resources and the inputs and outputs. */
  for (uint32_t id = 1; id < m->bound; ++id) {
    uint32_t off = m->inst[id];
    if (!off || OPC(off) != OpVariable) continue;
    uint32_t storage = m->w[off + 3];
    Type *pt = get_type(m, m->w[off + 1]);
    if (!pt || pt->kind != T_PTR) continue;
    if (storage == SCInput || storage == SCOutput) {
      int listed = m->ninterface == 0;
      for (uint32_t i = 0; i < m->ninterface; ++i) listed |= m->interface_ids[i] == id;
      if (listed || used[id]) note_io(m, id, storage);
      continue;
    }
    if (!used[id]) continue;
    if (storage != SCUniformConstant && storage != SCUniform && storage != SCPushConstant && storage != SCStorageBuffer) continue;
    if (m->nres >= SP_MAX_RES) return sp_fail(m, "too many resources");
    SpvResource *r = &m->res[m->nres];
    m->res_var[m->nres] = id;
    sp_zero(r, sizeof *r);
    r->sampler_slot = 0xFFFFFFFFu;
    uint32_t array = 1;
    uint32_t elem = strip_arrays(m, pt->elem, &array);
    Type *et = get_type(m, elem);
    r->array_size = array;
    deco_find(m, id, 0xFFFFFFFFu, DecoDescriptorSet, &r->set);
    deco_find(m, id, 0xFFFFFFFFu, DecoBinding, &r->binding);
    if (!et) return sp_fail(m, "resource of an unknown type");
    if (storage == SCPushConstant) {
      r->kind = SPV_RES_PUSH, r->bytes = block_bytes(m, elem, 0, 0);
    } else if (storage == SCStorageBuffer || (storage == SCUniform && et->kind == T_STRUCT && deco_find(m, elem, 0xFFFFFFFFu, DecoBufferBlock, 0))) {
      r->kind = SPV_RES_STORAGE, r->bytes = block_bytes(m, elem, 0, 0);
    } else if (storage == SCUniform) {
      r->kind = SPV_RES_UNIFORM, r->bytes = block_bytes(m, elem, 0, 0);
    } else if (et->kind == T_SAMPLER) {
      r->kind = SPV_RES_SAMPLER;
    } else if (et->kind == T_IMAGE || et->kind == T_SAMPLED) {
      Type *image = et->kind == T_SAMPLED ? get_type(m, et->elem) : et;
      if (!image || image->kind != T_IMAGE) return sp_fail(m, "bad image type");
      if (image->sampled == 2) return sp_fail(m, "storage images");
      if (image->dim == 6) return sp_fail(m, "subpass inputs");
      if (image->dim == 0 || image->dim == 5 || image->dim == 4) return sp_fail(m, "1D, rect and buffer images");
      if (image->ms) return sp_fail(m, "multisampled images in shaders");
      r->kind = et->kind == T_SAMPLED ? SPV_RES_COMBINED : SPV_RES_IMAGE;
      r->dim = image->dim == 2 ? SPV_DIM_3D : image->dim == 3 ? SPV_DIM_CUBE : image->arrayed ? SPV_DIM_2D_ARRAY : SPV_DIM_2D;
      if (image->dim == 3 && image->arrayed) return sp_fail(m, "cube map arrays");
      r->depth = image->depth == 1;
    } else {
      return sp_fail(m, "unsupported resource");
    }
    r->used = 1;
    ++m->nres;
  }
  return 0;
}

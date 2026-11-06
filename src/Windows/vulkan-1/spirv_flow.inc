/* spirv.inc, part 4: variables, control flow, functions. */

/* ---- inputs and outputs ------------------------------------------------------------------------------------------------------------------------------------------- */
#define POS_REG 15          /* the register of the position (vertex output, pixel input) */
#define FACE_REG 12
#define INSTANCE_REG 13
#define VERTEX_REG 14
enum { DECL_NONE = 0, DECL_PLAIN = 1, DECL_POSITION = 2, DECL_VERTEX_ID = 3, DECL_INSTANCE_ID = 4, DECL_FRONT_FACING = 5, DECL_FLAT = 6 };

static void identity_swz(SpOp *o, uint32_t base, uint32_t n) {
  for (uint32_t i = 0; i < 4; ++i) o->swz[i] = (sp_u8)(base + (i < n ? i : n - 1));
}

typedef struct IoCtx {
  uint32_t storage, loc, comp_base, flat;
  uint32_t builtin;      /* 0xFFFFFFFF: none */
} IoCtx;

static int io_leaf(Mod *m, IoCtx *c, Type *t, Leaf *out) {
  int is_input = c->storage == SCInput;
  Type *e = t->kind == T_VEC ? get_type(m, t->elem) : t;
  uint32_t n = t->kind == T_VEC ? t->count : 1;
  sp_zero(out, sizeof *out);
  out->n = (sp_u8)n, out->cls = e->cls;
  out->op.ncomp = (sp_u8)n;
  if (c->builtin != 0xFFFFFFFFu) {
    switch (c->builtin) {
      case BuiltInPosition:
        if (is_input) return sp_fail(m, "reading gl_Position");
        out->op.file = OF_OUT, out->op.idx = POS_REG, out->op.pad = 1;
        identity_swz(&out->op, 0, 4);
        m->out_decl[POS_REG] = DECL_POSITION;
        return 0;
      case BuiltInPointSize: case BuiltInClipDistance: case BuiltInCullDistance:
        if (is_input) return sp_fail(m, "reading a clip distance or point size");
        out->op.file = OF_NULL;
        return 0;
      case BuiltInFragCoord:
        out->op.file = OF_IN, out->op.idx = POS_REG;
        identity_swz(&out->op, 0, 4);
        m->in_decl[POS_REG] = DECL_POSITION;
        m->frag_coord = 1;
        return 0;
      case BuiltInVertexIndex: case BuiltInVertexId:
        out->op.file = OF_IN, out->op.idx = VERTEX_REG;
        identity_swz(&out->op, 0, 1);
        m->in_decl[VERTEX_REG] = DECL_VERTEX_ID;
        return 0;
      case BuiltInInstanceIndex: case BuiltInInstanceId:
        out->op.file = OF_IN, out->op.idx = INSTANCE_REG;
        identity_swz(&out->op, 0, 1);
        m->in_decl[INSTANCE_REG] = DECL_INSTANCE_ID;
        return 0;
      case BuiltInFrontFacing:
        out->op.file = OF_IN, out->op.idx = FACE_REG;
        identity_swz(&out->op, 0, 1);
        m->in_decl[FACE_REG] = DECL_FRONT_FACING;
        return 0;
      case BuiltInFragDepth: return sp_fail(m, "gl_FragDepth");
      case BuiltInGlobalInvocationId: case BuiltInWorkgroupId: case BuiltInLocalInvocationId: case BuiltInLocalInvocationIndex: {
        if (!is_input || m->stage != SPV_STAGE_COMPUTE) return sp_fail(m, "a compute built-in outside a compute shader");
        uint32_t file = c->builtin == BuiltInGlobalInvocationId ? OF_THREAD_ID : c->builtin == BuiltInWorkgroupId ? OF_GROUP_ID : c->builtin == BuiltInLocalInvocationId ? OF_THREAD_IN_GROUP : OF_THREAD_INDEX;
        out->op.file = (sp_u8)file;
        identity_swz(&out->op, 0, n);
        m->system_used |= 1u << (file - OF_THREAD_ID);
        return 0;
      }
      case BuiltInNumWorkgroups: return sp_fail(m, "gl_NumWorkGroups");
      default: return sp_fail2(m, "unsupported built-in", c->builtin);
    }
  }
  if (e->kind != T_FLOAT) return sp_fail(m, "integer or boolean inputs and outputs");
  if (c->loc >= 16) return sp_fail(m, "input or output location out of range");
  out->op.file = is_input ? OF_IN : OF_OUT;
  out->op.idx = c->loc;
  identity_swz(&out->op, c->comp_base, n);
  if (is_input) m->in_decl[c->loc] = c->flat ? DECL_FLAT : DECL_PLAIN;
  else m->out_decl[c->loc] = DECL_PLAIN;
  ++c->loc;
  return 0;
}

static int io_assign(Mod *m, IoCtx *c, uint32_t tid, Leaf *out, uint32_t *k) {
  Type *t = get_type(m, tid);
  if (!t) return sp_fail(m, "interface variable of an unknown type");
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: case T_VEC:
      if (*k >= SP_MAX_ID_LEAVES) return sp_fail(m, "interface too large");
      if (io_leaf(m, c, t, &out[*k])) return 1;
      ++*k;
      return 0;
    case T_MAT: case T_ARRAY:
      for (uint32_t i = 0; i < t->count; ++i)
        if (io_assign(m, c, t->elem, out, k)) return 1;
      return 0;
    case T_STRUCT:
      for (uint32_t i = 0; i < t->nmem; ++i) {
        IoCtx inner = *c;
        uint32_t v;
        if (deco_find(m, tid, i, DecoBuiltIn, &v)) inner.builtin = v;
        else if (deco_find(m, tid, i, DecoLocation, &v)) inner.loc = v;
        if (deco_find(m, tid, i, DecoFlat, 0)) inner.flat = 1;
        if (io_assign(m, &inner, t->mem[i], out, k)) return 1;
        if (inner.builtin == 0xFFFFFFFFu) c->loc = inner.loc;
      }
      return 0;
    default: return sp_fail(m, "unsupported interface type");
  }
}

static int setup_variables(Mod *m) {
  for (uint32_t id = 1; id < m->bound; ++id) {
    uint32_t off = m->inst[id];
    if (!off || OPC(off) != OpVariable) continue;
    uint32_t storage = m->w[off + 3];
    Type *pt = get_type(m, m->w[off + 1]);
    if (!pt || pt->kind != T_PTR) continue;
    uint32_t pointee = pt->elem;
    Val *v = 0;
    if (storage == SCInput || storage == SCOutput) {
      int listed = m->ninterface == 0;
      for (uint32_t i = 0; i < m->ninterface; ++i) listed |= m->interface_ids[i] == id;
      if (!listed) continue;
      int n = type_nleaf(m, pointee);
      if (n < 0) return 1;
      v = new_val(m, pointee, (uint32_t)n);
      if (!v) return sp_fail(m, "out of memory");
      IoCtx c;
      c.storage = storage, c.loc = 0, c.comp_base = 0, c.flat = 0, c.builtin = 0xFFFFFFFFu;
      uint32_t x;
      if (deco_find(m, id, 0xFFFFFFFFu, DecoBuiltIn, &x)) c.builtin = x;
      if (deco_find(m, id, 0xFFFFFFFFu, DecoLocation, &x)) c.loc = x;
      if (deco_find(m, id, 0xFFFFFFFFu, DecoComponent, &x)) c.comp_base = x;
      if (deco_find(m, id, 0xFFFFFFFFu, DecoFlat, 0)) c.flat = 1;
      uint32_t k = 0;
      if (io_assign(m, &c, pointee, v->leaf, &k)) return 1;
      v->nleaf = k;
      v->is_ptr = 1, v->var = id, v->comp = 4;
    } else if (storage == SCPrivate || storage == SCFunction) {
      if (storage == SCFunction) continue;   /* made when its block is reached */
      int n = type_nleaf(m, pointee);
      if (n < 0) return 1;
      v = new_val(m, pointee, (uint32_t)n);
      if (!v) return sp_fail(m, "out of memory");
      Leaf shape[SP_MAX_ID_LEAVES];
      uint32_t count = 0;
      if (leaves_of(m, pointee, shape, &count, SP_MAX_ID_LEAVES)) return 1;
      for (uint32_t i = 0; i < count; ++i) v->leaf[i] = temp_leaf(m, shape[i].n, shape[i].cls, 1);
      v->is_ptr = 1, v->var = id, v->comp = 4;
      if (LEN(off) > 4 && m->w[off + 4] < m->bound && m->vals[m->w[off + 4]]) {
        Val *init = m->vals[m->w[off + 4]];
        for (uint32_t i = 0; i < count && i < init->nleaf; ++i) store_leaf(m, &v->leaf[i], &init->leaf[i], 0);
      }
    } else if (storage == SCUniform || storage == SCPushConstant || storage == SCStorageBuffer || storage == SCUniformConstant) {
      uint32_t r = 0;
      for (; r < m->nres; ++r)
        if (m->res_var[r] == id) break;
      if (r == m->nres) continue;
      v = new_val(m, pointee, 0);
      if (!v) return sp_fail(m, "out of memory");
      if (m->res[r].kind == SPV_RES_STORAGE) {
        v->is_mem = 1, v->is_ptr = 1, v->comp = 4, v->var = r;
        v->mem_file = m->res[r].uav ? OF_UAV : OF_RES;
        v->mem_idx = m->res[r].slot;
        v->res_array = m->res[r].array_size > 1;
      } else if (m->res[r].kind == SPV_RES_UNIFORM || m->res[r].kind == SPV_RES_PUSH) {
        if (m->res[r].array_size > 1) return sp_fail(m, "arrays of uniform blocks");
        v->is_cb = 1, v->var = r, v->mstride = 16, v->is_ptr = 1, v->comp = 4;
      } else {
        v->is_image = 1, v->image_res = r, v->sampler_res = 0xFFFFFFFFu, v->is_ptr = 1;
      }
    } else if (storage == SCWorkgroup) {
      /* one after the other in the group-shared memory g0 */
      if (m->stage != SPV_STAGE_COMPUTE) return sp_fail(m, "Workgroup memory outside a compute shader");
      uint32_t bytes = (mem_size(m, pointee, 0, 0) + 3) & ~3u;
      if (bytes == 0 || bytes > 32768 || m->shared_bytes > 32768 - bytes) return sp_fail(m, "more Workgroup memory than the adapter has (32 KiB)");
      v = new_val(m, pointee, 0);
      if (!v) return sp_fail(m, "out of memory");
      v->is_mem = 1, v->is_ptr = 1, v->comp = 4, v->var = id;
      v->mem_file = OF_TGSM, v->mem_idx = 0, v->byte_off = m->shared_bytes;
      m->shared_bytes += bytes;
    } else {
      continue;
    }
    set_val(m, id, v);
  }
  return 0;
}

/* ---- phi nodes ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
static int phi_pairs(Mod *m, uint32_t from, uint32_t to, Val **dst, Val **src, uint32_t cap) {
  Block *tb = to < m->bound ? m->blk[to] : 0;
  if (!tb) return 0;
  uint32_t n = 0;
  for (uint32_t off = tb->start; off < m->nw && OPC(off) == OpPhi; off += LEN(off)) {
    uint32_t len = LEN(off);
    for (uint32_t k = 3; k + 1 < len; k += 2)
      if (m->w[off + k + 1] == from && n < cap) {
        dst[n] = m->w[off + 2] < m->bound ? m->vals[m->w[off + 2]] : 0;
        src[n] = val_of(m, m->w[off + k]);
        ++n;
      }
  }
  return (int)n;
}

static void phi_copies(Mod *m, uint32_t from, uint32_t to) {
  Val *dst[16], *src[16];
  int n = phi_pairs(m, from, to, dst, src, 16);
  if (n == 0 || m->failed) return;
  Leaf staged[16][8];
  for (int p = 0; p < n; ++p) {
    if (!dst[p] || !src[p]) return;
    if (n > 1 && src[p]->nleaf <= 8) {
      for (uint32_t i = 0; i < src[p]->nleaf; ++i) staged[p][i] = copy_to_temp(m, &src[p]->leaf[i]), staged[p][i].cls = src[p]->leaf[i].cls;
    }
  }
  for (int p = 0; p < n; ++p)
    for (uint32_t i = 0; i < dst[p]->nleaf && i < src[p]->nleaf; ++i) store_leaf(m, &dst[p]->leaf[i], n > 1 && src[p]->nleaf <= 8 ? &staged[p][i] : &src[p]->leaf[i], 0);
}

static int phi_needed(Mod *m, uint32_t from, uint32_t to) {
  Val *dst[16], *src[16];
  return phi_pairs(m, from, to, dst, src, 16) > 0;
}

/* ---- control flow ------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { K_FLOW = 0, K_STOP, K_JUMP };

static void emit_simple(Mod *m, uint16_t op) { emit_ins(m, op); }

static void emit_test(Mod *m, uint16_t op, const Leaf *cond, int nonzero) {
  SpIns *i = emit_ins(m, op);
  Leaf c = *cond;
  i->src[0] = src_of(&c, 1);
  i->nsrc = 1;
  i->test = (sp_u8)(nonzero ? 1 : 0);
}

static int gen_cfg(Mod *m, uint32_t label, uint32_t stop);

static Leaf flag_leaf(uint32_t vreg) {
  Leaf l;
  sp_zero(&l, sizeof l);
  l.op = op_temp(vreg);
  l.op.ncomp = 1;
  l.n = 1, l.cls = CLS_B;
  return l;
}

static void set_flag(Mod *m, uint32_t vreg, uint32_t value) {
  Leaf f = flag_leaf(vreg);
  Leaf v = imm_u(value, 1, CLS_B);
  store_leaf(m, &f, &v, 0);
}

static void note_range(Mod *m, uint32_t begin, uint32_t end) {
  if (m->nloops == m->cap_loops) {
    uint32_t cap = m->cap_loops ? m->cap_loops * 2 : 16;
    Range *n = (Range *)sp_alloc(m, (sp_u64)cap * sizeof(Range));
    if (!n) return;
    if (m->loops) sp_copy(n, m->loops, (sp_u64)m->nloops * sizeof(Range));
    m->loops = n;
    m->cap_loops = cap;
  }
  m->loops[m->nloops].begin = begin, m->loops[m->nloops].end = end;
  ++m->nloops;
}

/* Opens a DXBC loop that a branch to `exit_target` leaves. Returns the instruction index of the loop. */
static int open_ctx(Mod *m, uint32_t exit_target, uint32_t *begin) {
  if (m->ncstack >= 20) return sp_fail(m, "constructs nested too deep");
  Ctx *c = &m->cstack[m->ncstack++];
  sp_zero(c, sizeof *c);
  c->exit_target = exit_target;
  c->flag = new_vreg(m, 1);
  set_flag(m, c->flag, 0);
  *begin = m->ncode;
  emit_simple(m, D_LOOP);
  return 0;
}

static void close_ctx(Mod *m, uint32_t begin) {
  Ctx done = m->cstack[m->ncstack - 1];
  --m->ncstack;
  emit_simple(m, D_ENDLOOP);
  note_range(m, begin, m->ncode - 1);
  for (uint32_t i = 0; i < done.ncrossed; ++i) {   /* an exit of an outer construct was taken inside: go on leaving */
    Leaf f = flag_leaf(done.crossed[i]);
    emit_test(m, D_BREAKC, &f, 1);
  }
}

static int find_exit(Mod *m, uint32_t target) {
  for (int i = (int)m->ncstack - 1; i >= 0; --i)
    if (m->cstack[i].exit_target == target) return i;
  return -1;
}

/* Leaves the loop `index` of the stack. */
static void do_exit(Mod *m, int index) {
  int top = (int)m->ncstack - 1;
  if (index != top) {
    uint32_t flag = m->cstack[index].flag;
    set_flag(m, flag, 0xFFFFFFFFu);
    for (int j = index + 1; j <= top; ++j) {
      Ctx *c = &m->cstack[j];
      uint32_t k = 0;
      for (; k < c->ncrossed; ++k)
        if (c->crossed[k] == flag) break;
      if (k == c->ncrossed && c->ncrossed < 8) c->crossed[c->ncrossed++] = flag;
    }
  }
  emit_simple(m, D_BREAK);
}

static int target_kind(Mod *m, uint32_t t, uint32_t stop) {
  if (t == stop) return K_STOP;
  if (find_exit(m, t) >= 0) return K_JUMP;
  return K_FLOW;
}

static void jump(Mod *m, uint32_t target) { do_exit(m, find_exit(m, target)); }

static void arm(Mod *m, uint32_t from, uint32_t to, uint32_t merge) {
  phi_copies(m, from, to);
  if (to != merge) gen_cfg(m, to, merge);
}

static int gen_if(Mod *m, uint32_t from, const Leaf *cond, uint32_t t, uint32_t f, uint32_t merge) {
  if (t == merge) {   /* nothing in the then branch */
    emit_test(m, D_IF, cond, 0);
    arm(m, from, f, merge);
    if (phi_needed(m, from, t)) {
      emit_simple(m, D_ELSE);
      phi_copies(m, from, t);
    }
    emit_simple(m, D_ENDIF);
    return 0;
  }
  emit_test(m, D_IF, cond, 1);
  arm(m, from, t, merge);
  if (f != merge || phi_needed(m, from, f)) {
    emit_simple(m, D_ELSE);
    arm(m, from, f, merge);
  }
  emit_simple(m, D_ENDIF);
  return 0;
}

static int gen_function(Mod *m, uint32_t func_id, Val **args, uint32_t nargs, uint32_t result_type, uint32_t result_id);

static int gen_block_body(Mod *m, Block *b, uint32_t end) {
  for (uint32_t off = b->start; off < end && !m->failed; off += LEN(off)) {
    uint32_t op = OPC(off);
    if (op == OpVariable) {   /* a function variable: new registers each time the function is expanded */
      Type *vt = get_type(m, m->w[off + 1]);
      if (!vt || vt->kind != T_PTR) return sp_fail(m, "variable of a non-pointer type");
      uint32_t pointee = vt->elem;
      int n = type_nleaf(m, pointee);
      if (n < 0) return 1;
      Val *v = new_val(m, pointee, (uint32_t)n);
      if (!v) return sp_fail(m, "out of memory");
      Leaf shape[SP_MAX_ID_LEAVES];
      uint32_t count = 0;
      if (leaves_of(m, pointee, shape, &count, SP_MAX_ID_LEAVES)) return 1;
      for (uint32_t i = 0; i < count; ++i) v->leaf[i] = temp_leaf(m, shape[i].n, shape[i].cls, 1);
      v->is_ptr = 1, v->var = m->w[off + 2], v->comp = 4;
      set_val(m, m->w[off + 2], v);
      if (LEN(off) > 4 && m->w[off + 4] < m->bound && m->vals[m->w[off + 4]]) {
        Val *init = m->vals[m->w[off + 4]];
        for (uint32_t i = 0; i < count && i < init->nleaf; ++i) store_leaf(m, &v->leaf[i], &init->leaf[i], 0);
      }
      continue;
    }
    if (op == OpFunctionCall) {
      uint32_t nargs = LEN(off) - 4;
      Val *args[16];
      if (nargs > 16) return sp_fail(m, "too many arguments");
      for (uint32_t k = 0; k < nargs; ++k) {
        args[k] = val_of(m, m->w[off + 4 + k]);
        if (!args[k]) return 1;
      }
      if (gen_function(m, m->w[off + 3], args, nargs, m->w[off + 1], m->w[off + 2])) return 1;
      continue;
    }
    if (gen_instruction(m, off)) return 1;
  }
  return m->failed;
}

static int gen_loop(Mod *m, Block *h) {
  uint32_t merge = h->merge, cont = h->cont;
  Block *cb = cont < m->bound ? m->blk[cont] : 0;
  if (!cb) return sp_fail(m, "a loop without a continue target");
  int wrapper = cont != h->label ? cb->npred > 1 : h->npred > 2;
  uint32_t begin_loop = 0, begin_once = 0;
  if (open_ctx(m, merge, &begin_loop)) return 1;
  if (wrapper && open_ctx(m, cont, &begin_once)) return 1;
  /* The header's own instructions and what follows up to the continue target. */
  if (cont == h->label) {
    if (gen_block_body(m, h, h->term)) return 1;
    if (OPC(h->term) == OpBranchConditional) {
      Val *cond = val_of(m, m->w[h->term + 1]);
      if (!cond) return 1;
      uint32_t t = m->w[h->term + 2], f = m->w[h->term + 3];
      if (t == h->label && f == merge) {
        emit_test(m, D_IF, &cond->leaf[0], 0);
        phi_copies(m, h->label, merge);
        jump(m, merge);
        emit_simple(m, D_ENDIF);
        phi_copies(m, h->label, h->label);
      } else if (f == h->label && t == merge) {
        emit_test(m, D_IF, &cond->leaf[0], 1);
        phi_copies(m, h->label, merge);
        jump(m, merge);
        emit_simple(m, D_ENDIF);
        phi_copies(m, h->label, h->label);
      } else {
        return sp_fail(m, "unsupported loop back edge");
      }
    } else {
      phi_copies(m, h->label, h->label);
    }
    if (wrapper) {
      emit_simple(m, D_BREAK);
      close_ctx(m, begin_once);
    }
  } else {
    Block saved = *h;
    h->merge_kind = 0;     /* (so that gen_cfg treats it as an ordinary block) */
    int r = gen_cfg(m, h->label, cont);
    *h = saved;
    if (r) return 1;
    if (wrapper) {
      emit_simple(m, D_BREAK);   /* (the natural end of the body) */
      close_ctx(m, begin_once);
    }
    /* The continue construct: its flow ends at the header, the back edge. */
    Block csaved = *cb;
    cb->merge_kind = 0;
    int r2 = gen_cfg(m, cont, h->label);
    *cb = csaved;
    if (r2) return 1;
  }
  close_ctx(m, begin_loop);
  return 0;
}

static int gen_return(Mod *m) {
  Frame *f = &m->fstack[m->nfstack - 1];
  if (!f->wrapper) return 0;
  jump(m, RET_TARGET);
  return 0;
}

static int gen_cfg(Mod *m, uint32_t label, uint32_t stop) {
  while (!m->failed) {
    if (label == stop) return 0;
    if (++m->steps > 20000) return sp_fail(m, "control flow too complex (or a cycle without a loop)");
    Block *b = label < m->bound ? m->blk[label] : 0;
    if (!b) return sp_fail(m, "branch to a block that does not exist");
    if (b->merge_kind == 2) {
      if (gen_loop(m, b)) return 1;
      label = b->merge;
      continue;
    }
    if (gen_block_body(m, b, b->term)) return 1;
    uint32_t t = b->term, op = OPC(t);
    switch (op) {
      case OpBranch: {
        uint32_t target = m->w[t + 1];
        phi_copies(m, label, target);
        int k = target_kind(m, target, stop);
        if (k == K_STOP) return 0;
        if (k == K_JUMP) {
          jump(m, target);
          return 0;
        }
        label = target;
        continue;
      }
      case OpBranchConditional: {
        Val *cv = val_of(m, m->w[t + 1]);
        if (!cv) return 1;
        const Leaf cond = cv->leaf[0];
        uint32_t tt = m->w[t + 2], ff = m->w[t + 3];
        if (b->merge_kind == 1) {
          if (gen_if(m, label, &cond, tt, ff, b->merge)) return 1;
          label = b->merge;
          continue;
        }
        int kt = target_kind(m, tt, stop), kf = target_kind(m, ff, stop);
        if (kt == K_FLOW && kf == K_FLOW) return sp_fail(m, "a conditional branch that has no merge");
        if (kt != K_FLOW && kf != K_FLOW) {
          emit_test(m, D_IF, &cond, 1);
          phi_copies(m, label, tt);
          if (kt == K_JUMP) jump(m, tt);
          emit_simple(m, D_ELSE);
          phi_copies(m, label, ff);
          if (kf == K_JUMP) jump(m, ff);
          emit_simple(m, D_ENDIF);
          return 0;
        }
        if (kt != K_FLOW) {
          if (kt == K_JUMP && find_exit(m, tt) == (int)m->ncstack - 1 && !phi_needed(m, label, tt)) {
            emit_test(m, D_BREAKC, &cond, 1);
          } else {
            emit_test(m, D_IF, &cond, 1);
            phi_copies(m, label, tt);
            if (kt == K_JUMP) jump(m, tt);
            emit_simple(m, D_ENDIF);
          }
          phi_copies(m, label, ff);
          label = ff;
        } else {
          if (kf == K_JUMP && find_exit(m, ff) == (int)m->ncstack - 1 && !phi_needed(m, label, ff)) {
            emit_test(m, D_BREAKC, &cond, 0);
          } else {
            emit_test(m, D_IF, &cond, 0);
            phi_copies(m, label, ff);
            if (kf == K_JUMP) jump(m, ff);
            emit_simple(m, D_ENDIF);
          }
          phi_copies(m, label, tt);
          label = tt;
        }
        continue;
      }
      case OpSwitch: {
        Val *sel = val_of(m, m->w[t + 1]);
        if (!sel) return 1;
        uint32_t len = LEN(t), def = m->w[t + 2], merge = b->merge_kind == 1 ? b->merge : 0xFFFFFFFFu;
        if (merge == 0xFFFFFFFFu) return sp_fail(m, "a switch without a merge block");
        uint32_t ncase = len >= 3 ? (len - 3) / 2 : 0;
        uint32_t begin = 0, depth = 0;
        if (open_ctx(m, merge, &begin)) return 1;   /* (a loop that runs once: a branch to the merge block from inside is a break) */
        for (uint32_t c = 0; c < ncase; ++c) {
          uint32_t lit = m->w[t + 3 + 2 * c], target = m->w[t + 4 + 2 * c];
          int seen = 0;
          for (uint32_t d = 0; d < c; ++d) seen |= m->w[t + 4 + 2 * d] == target;
          if (seen) continue;
          Leaf lv = imm_u(lit, 1, CLS_I);
          Leaf eq = alu(m, D_IEQ, 1, CLS_B, &sel->leaf[0], &lv, 0);
          for (uint32_t d = c + 1; d < ncase; ++d)   /* cases that share a target are tested together */
            if (m->w[t + 4 + 2 * d] == target) {
              Leaf lv2 = imm_u(m->w[t + 3 + 2 * d], 1, CLS_I);
              Leaf eq2 = alu(m, D_IEQ, 1, CLS_B, &sel->leaf[0], &lv2, 0);
              eq = alu(m, D_OR, 1, CLS_B, &eq, &eq2, 0);
            }
          emit_test(m, D_IF, &eq, 1);
          arm(m, label, target, merge);
          emit_simple(m, D_ELSE);
          ++depth;
        }
        arm(m, label, def, merge);
        while (depth--) emit_simple(m, D_ENDIF);
        emit_simple(m, D_BREAK);
        close_ctx(m, begin);
        label = merge;
        continue;
      }
      case OpKill: case OpTerminateInvocation: case OpDemoteToHelperInvocation: {
        if (m->stage != SPV_STAGE_FRAGMENT) return sp_fail(m, "discard outside a fragment shader");
        Leaf all = imm_u(0xFFFFFFFFu, 1, CLS_B);
        emit_test(m, D_DISCARD, &all, 1);
        return 0;
      }
      case OpReturn:
        return gen_return(m);
      case OpReturnValue: {
        Frame *f = &m->fstack[m->nfstack - 1];
        Val *v = val_of(m, m->w[t + 1]);
        if (!v) return 1;
        if (f->ret)
          for (uint32_t i = 0; i < f->ret->nleaf && i < v->nleaf; ++i) store_leaf(m, &f->ret->leaf[i], &v->leaf[i], 0);
        return gen_return(m);
      }
      case OpUnreachable:
        return 0;
      default:
        return sp_fail(m, "a block without a terminator");
    }
  }
  return 1;
}

static int gen_function(Mod *m, uint32_t func_id, Val **args, uint32_t nargs, uint32_t result_type, uint32_t result_id) {
  Func *fn = 0;
  for (uint32_t i = 0; i < m->nfuncs; ++i)
    if (m->funcs[i].id == func_id) fn = &m->funcs[i];
  if (!fn) return sp_fail(m, "call of a missing function");
  if (m->nfstack >= 15) return sp_fail(m, "functions nested too deep");
  if (++m->steps > 20000) return sp_fail(m, "control flow too complex");
  Frame *f = &m->fstack[m->nfstack++];
  sp_zero(f, sizeof *f);
  f->func = func_id;
  Block *last = m->blk[fn->last_label];
  f->wrapper = fn->nreturns > 1 || (fn->nreturns == 1 && last && OPC(last->term) != OpReturn && OPC(last->term) != OpReturnValue);
  /* the parameters */
  uint32_t pi = 0;
  for (uint32_t off = fn->offset; off < m->nw && OPC(off) != OpFunctionEnd; off += LEN(off))
    if (OPC(off) == OpFunctionParameter) {
      if (pi < nargs) set_val(m, m->w[off + 2], args[pi]);
      ++pi;
    }
  /* the return value */
  Type *rt = get_type(m, fn->ret_type);
  if (rt && rt->kind != T_VOID) {
    f->ret = temp_val(m, fn->ret_type, result_id);
    if (!f->ret) return 1;
    (void)result_type;
  }
  /* phi variables of the whole function */
  for (uint32_t off = fn->offset; off < m->nw && OPC(off) != OpFunctionEnd; off += LEN(off))
    if (OPC(off) == OpPhi) {
      Val *pv = result_val(m, m->w[off + 1], m->w[off + 2]);
      if (!pv) return 1;
      Leaf shape[SP_MAX_ID_LEAVES];
      uint32_t count = 0;
      if (leaves_of(m, m->w[off + 1], shape, &count, SP_MAX_ID_LEAVES)) return 1;
      for (uint32_t i = 0; i < count; ++i) pv->leaf[i] = temp_leaf(m, shape[i].n, shape[i].cls, 1);
    }
  uint32_t begin = 0;
  if (f->wrapper && open_ctx(m, RET_TARGET, &begin)) return 1;
  int r = gen_cfg(m, fn->first_label, 0xFFFFFFFFu);
  if (f->wrapper) {
    emit_simple(m, D_BREAK);
    close_ctx(m, begin);
  }
  --m->nfstack;
  if (m->nfstack > 0 && f->ret && result_id < m->bound) set_val(m, result_id, f->ret);
  return r;
}

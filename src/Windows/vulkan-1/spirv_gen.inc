/* spirv.inc, part 3: the instruction list and the translation of the SPIR-V instructions that are not control flow. */

static uint32_t new_vreg(Mod *m, int is_var) {
  if (m->nvreg == m->cap_vreg) {
    uint32_t cap = m->cap_vreg ? m->cap_vreg * 2 : 256;
    sp_u8 *n = (sp_u8 *)sp_alloc(m, cap);
    if (!n) return 0;
    if (m->vreg_var) sp_copy(n, m->vreg_var, m->nvreg);
    m->vreg_var = n;
    m->cap_vreg = cap;
  }
  m->vreg_var[m->nvreg] = (sp_u8)is_var;
  return m->nvreg++;
}

static SpIns *emit_ins(Mod *m, uint16_t op) {
  if (m->ncode >= SP_MAX_CODE) {
    sp_fail(m, "the shader needs more instructions than the adapter's front end takes");
    static SpIns dummy;
    sp_zero(&dummy, sizeof dummy);
    return &dummy;
  }
  if (m->ncode == m->cap_code) {
    uint32_t cap = m->cap_code ? m->cap_code * 2 : 128;
    SpIns *n = (SpIns *)sp_alloc(m, (sp_u64)cap * sizeof(SpIns));
    if (!n) {
      static SpIns dummy;
      return &dummy;
    }
    if (m->code) sp_copy(n, m->code, (sp_u64)m->ncode * sizeof(SpIns));
    m->code = n;
    m->cap_code = cap;
  }
  SpIns *i = &m->code[m->ncode++];
  sp_zero(i, sizeof *i);
  i->op = op;
  return i;
}

static SpOp op_none(void) {
  SpOp o;
  sp_zero(&o, sizeof o);
  o.file = OF_NONE;
  return o;
}

static SpOp op_temp(uint32_t vreg) {
  SpOp o;
  sp_zero(&o, sizeof o);
  o.file = OF_TEMP, o.idx = vreg;
  o.swz[0] = 0, o.swz[1] = 1, o.swz[2] = 2, o.swz[3] = 3;
  return o;
}

static Leaf temp_leaf(Mod *m, sp_u8 n, sp_u8 cls, int is_var) {
  Leaf l;
  sp_zero(&l, sizeof l);
  l.op = op_temp(new_vreg(m, is_var));
  l.op.ncomp = n;
  l.n = n, l.cls = cls;
  return l;
}

/* The source operand for a leaf read as `want` components: a scalar is replicated, the swizzle padded with its last component. */
static SpOp src_of(const Leaf *l, int want) {
  SpOp o = l->op;
  if (l->n == 1) {
    o.swz[1] = o.swz[2] = o.swz[3] = o.swz[0];
  } else {
    for (int i = l->n; i < 4; ++i) o.swz[i] = o.swz[l->n - 1];
  }
  (void)want;
  return o;
}

static uint32_t full_mask(int n) { return (1u << n) - 1; }

static void add_src(SpIns *i, const Leaf *l, int want) { i->src[i->nsrc++] = src_of(l, want); }

static SpOp dst_temp(const Leaf *l) {
  SpOp o = l->op;
  o.mask = (sp_u8)full_mask(l->n);
  return o;
}

/* One component-wise instruction: result of n components in a new temporary. */
static Leaf alu(Mod *m, uint16_t dop, int n, int cls, const Leaf *a, const Leaf *b, const Leaf *c) {
  Leaf r = temp_leaf(m, (sp_u8)n, (sp_u8)cls, 0);
  SpIns *i = emit_ins(m, dop);
  i->dst[0] = dst_temp(&r);
  i->ndst = 1;
  if (a) add_src(i, a, n);
  if (b) add_src(i, b, n);
  if (c) add_src(i, c, n);
  return r;
}

static Leaf imm_f(float v, int n) {
  union { float f; uint32_t u; } x;
  x.f = v;
  Leaf l;
  imm_leaf(&l, (sp_u8)n, CLS_F, x.u, x.u, x.u, x.u);
  return l;
}
static Leaf imm_u(uint32_t v, int n, int cls) {
  Leaf l;
  imm_leaf(&l, (sp_u8)n, (sp_u8)cls, v, v, v, v);
  return l;
}

/* mov dst <- src for a leaf of a variable: the write mask comes from the destination's components. Negates Y when the destination is the position (see the top of spirv.inc). */
static void store_leaf(Mod *m, const Leaf *dst, const Leaf *src, int comp_only) {
  if (dst->op.file == OF_NULL) return;
  int n = comp_only ? 1 : dst->n;
  uint32_t mask = 0;
  sp_u8 full[4] = {0, 0, 0, 0};
  for (int k = 0; k < n; ++k) {
    int pos = comp_only ? comp_only - 1 : dst->op.swz[k];
    mask |= 1u << pos;
    full[pos] = src->n == 1 ? src->op.swz[0] : src->op.swz[k < src->n ? k : src->n - 1];
  }
  int flip = dst->op.pad == 1 && (mask & 2);
  for (int part = 0; part < (flip ? 2 : 1); ++part) {
    uint32_t pm = flip ? (part == 0 ? (mask & ~2u) : 2u) : mask;
    if (!pm) continue;
    SpIns *i = emit_ins(m, D_MOV);
    i->dst[0] = dst->op;
    i->dst[0].mask = (sp_u8)pm;
    i->ndst = 1;
    SpOp s = src->op;
    for (int k = 0; k < 4; ++k) s.swz[k] = full[k];
    if (flip && part == 1) s.neg ^= 1;
    i->src[0] = s;
    i->nsrc = 1;
  }
}

static Leaf copy_to_temp(Mod *m, const Leaf *src) {
  Leaf t = temp_leaf(m, src->n, src->cls, 0);
  SpIns *i = emit_ins(m, D_MOV);
  i->dst[0] = dst_temp(&t);
  i->ndst = 1;
  add_src(i, src, src->n);
  return t;
}

/* A leaf that is a temporary, so a modifier or swizzle can be written to or relative addressing can use it. */
static Leaf as_temp(Mod *m, const Leaf *l) {
  if (l->op.file == OF_TEMP && !l->op.neg && !l->op.abs && l->op.swz[0] == 0 && (l->n < 2 || l->op.swz[1] == 1) && (l->n < 3 || l->op.swz[2] == 2) && (l->n < 4 || l->op.swz[3] == 3)) return *l;
  return copy_to_temp(m, l);
}

/* Components of leaves put together in one vector: no instruction when they are components of one operand or all constants. */
static Leaf gather(Mod *m, const Leaf *const *from, const sp_u8 *comp, int n, sp_u8 cls) {
  int same = 1, all_imm = 1;
  for (int i = 0; i < n; ++i)
    if (comp[i] >= from[i]->n || comp[i] > 3) {
      sp_fail(m, "component index out of range");
      return *from[0];
    }
  for (int i = 0; i < n; ++i) {
    const SpOp *a = &from[0]->op, *b = &from[i]->op;
    if (b->file != OF_IMM) all_imm = 0;
    if (!(a->file == b->file && a->idx == b->idx && a->idx2 == b->idx2 && a->neg == b->neg && a->abs == b->abs && a->rel == b->rel && a->rel_reg == b->rel_reg && a->rel_comp == b->rel_comp &&
          a->file != OF_IMM && a->file != OF_OUT))
      same = 0;
  }
  Leaf r;
  sp_zero(&r, sizeof r);
  if (all_imm) {
    imm_leaf(&r, (sp_u8)n, cls, 0, 0, 0, 0);
    for (int i = 0; i < n; ++i) r.op.imm[i] = from[i]->op.imm[from[i]->op.swz[comp[i]]];
    return r;
  }
  if (same) {
    r = *from[0];
    r.n = (sp_u8)n, r.cls = cls;
    r.op.ncomp = (sp_u8)n;
    for (int i = 0; i < n; ++i) r.op.swz[i] = from[i]->op.swz[comp[i]];
    for (int i = n; i < 4; ++i) r.op.swz[i] = r.op.swz[n - 1];
    return r;
  }
  r = temp_leaf(m, (sp_u8)n, cls, 0);
  for (int i = 0; i < n; ++i) {
    SpIns *ins = emit_ins(m, D_MOV);
    ins->dst[0] = r.op;
    ins->dst[0].mask = (sp_u8)(1u << i);
    ins->ndst = 1;
    SpOp s = from[i]->op;
    sp_u8 c = from[i]->op.swz[comp[i]];
    s.swz[0] = s.swz[1] = s.swz[2] = s.swz[3] = c;
    ins->src[0] = s;
    ins->nsrc = 1;
  }
  return r;
}

static Leaf component_of(const Leaf *l, int comp) {
  if (comp < 0 || comp >= l->n) comp = 0;
  Leaf r = *l;
  r.n = 1;
  r.op.ncomp = 1;
  r.op.swz[0] = r.op.swz[1] = r.op.swz[2] = r.op.swz[3] = l->op.swz[comp];
  if (l->op.file == OF_IMM) r.op.imm[0] = l->op.imm[l->op.swz[comp]], r.op.swz[0] = r.op.swz[1] = r.op.swz[2] = r.op.swz[3] = 0, r.op.imm[1] = r.op.imm[2] = r.op.imm[3] = r.op.imm[0];
  return r;
}

/* ---- values ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static Val *val_of(Mod *m, uint32_t id) {
  if (id >= m->bound || !m->vals[id]) {
    sp_fail2(m, "use of an id that has no value:", id);
    return 0;
  }
  return m->vals[id];
}

static Val *result_val(Mod *m, uint32_t type, uint32_t id) {
  int n = type_nleaf(m, type);
  if (n < 0) return 0;
  Val *v = new_val(m, type, (uint32_t)n);
  if (!v) {
    sp_fail(m, "out of memory");
    return 0;
  }
  if (id < m->bound) set_val(m, id, v);
  return v;
}

/* A result with fresh temporaries of the shape of `type`. */
static Val *temp_val(Mod *m, uint32_t type, uint32_t id) {
  Val *v = result_val(m, type, id);
  if (!v) return 0;
  Leaf shape[SP_MAX_ID_LEAVES];
  uint32_t count = 0;
  if (leaves_of(m, type, shape, &count, SP_MAX_ID_LEAVES)) return 0;
  for (uint32_t i = 0; i < count; ++i) v->leaf[i] = temp_leaf(m, shape[i].n, shape[i].cls, 0);
  return v;
}

/* Component-wise: result leaves from the leaves of the operands. `dop` DXBC opcode, `nsrc` 1..3. When swap, the first two operands are exchanged (a > b is b < a). */
static int ew(Mod *m, uint32_t rtype, uint32_t id, uint16_t dop, int nsrc, uint32_t a_id, uint32_t b_id, uint32_t c_id, int swap, int result_cls_override) {
  Val *a = val_of(m, a_id), *b = nsrc > 1 ? val_of(m, b_id) : 0, *c = nsrc > 2 ? val_of(m, c_id) : 0;
  if (!a || (nsrc > 1 && !b) || (nsrc > 2 && !c)) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  Leaf shape[SP_MAX_ID_LEAVES];
  uint32_t count = 0;
  if (leaves_of(m, rtype, shape, &count, SP_MAX_ID_LEAVES)) return 1;
  if (count != a->nleaf || (b && b->nleaf != count) || (c && c->nleaf != count)) return sp_fail(m, "operand shape differs from the result's");
  for (uint32_t i = 0; i < count; ++i) {
    const Leaf *x = &a->leaf[i], *y = b ? &b->leaf[i] : 0, *z = c ? &c->leaf[i] : 0;
    if (swap) {
      const Leaf *t = x;
      x = y, y = t;
    }
    r->leaf[i] = alu(m, dop, shape[i].n, result_cls_override >= 0 ? result_cls_override : shape[i].cls, x, y, z);
  }
  return 0;
}

/* ---- constant buffer reads ------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct CbRead {
  uint32_t res;
  uint32_t dyn_on, dyn_reg, dyn_comp;
  uint32_t mstride;
} CbRead;

static int cb_leaves(Mod *m, const CbRead *rd, uint32_t tid, uint32_t offset, Leaf *out, uint32_t *k) {
  Type *t = get_type(m, tid);
  if (!t) return sp_fail(m, "uniform of an unknown type");
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: case T_VEC: {
      uint32_t n = t->kind == T_VEC ? t->count : 1;
      uint32_t comp = (offset % 16) / 4;
      if (offset % 4 || comp + n > 4) return sp_fail(m, "a uniform member straddles two 16 byte registers");
      Leaf l;
      sp_zero(&l, sizeof l);
      Type *e = t->kind == T_VEC ? get_type(m, t->elem) : t;
      l.op.file = OF_CB;
      l.op.idx = m->res[rd->res].slot;
      l.op.idx2 = offset / 16;
      l.op.ncomp = (sp_u8)n;
      for (uint32_t i = 0; i < 4; ++i) l.op.swz[i] = (sp_u8)(comp + (i < n ? i : n - 1));
      if (rd->dyn_on) l.op.rel = 1, l.op.rel_reg = rd->dyn_reg, l.op.rel_comp = (sp_u8)rd->dyn_comp;
      l.n = (sp_u8)n, l.cls = e->cls;
      out[(*k)++] = l;
      return 0;
    }
    case T_MAT:
      for (uint32_t c = 0; c < t->count; ++c)
        if (cb_leaves(m, rd, t->elem, offset + c * rd->mstride, out, k)) return 1;
      return 0;
    case T_ARRAY: {
      uint32_t stride = 0;
      if (!deco_find(m, tid, 0xFFFFFFFFu, DecoArrayStride, &stride)) stride = block_bytes(m, t->elem, 0, 0), stride = (stride + 15) & ~15u;
      for (uint32_t i = 0; i < t->count; ++i)
        if (cb_leaves(m, rd, t->elem, offset + i * stride, out, k)) return 1;
      return 0;
    }
    case T_STRUCT:
      for (uint32_t i = 0; i < t->nmem; ++i) {
        uint32_t off = 0, ms = 16;
        deco_find(m, tid, i, DecoOffset, &off);
        if (deco_find(m, tid, i, DecoRowMajor, 0)) return sp_fail(m, "row-major matrices in uniform blocks");
        CbRead inner = *rd;
        inner.mstride = deco_find(m, tid, i, DecoMatrixStride, &ms) ? ms : rd->mstride;
        if (cb_leaves(m, &inner, t->mem[i], offset + off, out, k)) return 1;
      }
      return 0;
    default:
      return sp_fail(m, "unsupported uniform member type");
  }
}

/* ---- access chains and loads and stores --------------------------------------------------------------------------------------------------------------------------- */
static int const_index(Mod *m, uint32_t id, uint32_t *value) {
  Val *v = val_of(m, id);
  if (!v || !v->nleaf) return 0;
  if (v->leaf[0].op.file != OF_IMM) return 0;
  *value = v->leaf[0].op.imm[0];
  return 1;
}

static int do_access_chain(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2], base = m->w[off + 3];
  Val *bv = val_of(m, base);
  if (!bv) return 1;
  Type *pt = get_type(m, rtype);
  if (!pt || pt->kind != T_PTR) return sp_fail(m, "access chain of a non-pointer type");
  Val *r = new_val(m, pt->elem, 0);
  if (!r) return sp_fail(m, "out of memory");
  {
    Leaf *keep = r->leaf;
    *r = *bv;
    r->leaf = keep, r->nleaf = 0;
  }
  r->type = pt->elem;
  /* the type to walk: the base's pointee */
  uint32_t cur = bv->type;
  for (uint32_t k = 4; k < len; ++k) {
    Type *t = get_type(m, cur);
    uint32_t index = 0;
    int is_const = const_index(m, m->w[off + k], &index);
    if (!t) return sp_fail(m, "access chain through an unknown type");
    if (bv->is_image) {   /* an element of an array of images or samplers */
      if (t->kind != T_ARRAY || !is_const) return sp_fail(m, "dynamic indexing of an array of images");
      r->image_unit_off = r->image_unit_off * t->count + index;
      cur = t->elem;
      continue;
    }
    if (r->is_cb) {
      switch (t->kind) {
        case T_STRUCT: {
          if (!is_const || index >= t->nmem) return sp_fail(m, "dynamic struct index");
          uint32_t o = 0, ms;
          deco_find(m, cur, index, DecoOffset, &o);
          if (deco_find(m, cur, index, DecoRowMajor, 0)) return sp_fail(m, "row-major matrices in uniform blocks");
          if (deco_find(m, cur, index, DecoMatrixStride, &ms)) r->mstride = ms;
          r->byte_off += o;
          cur = t->mem[index];
          break;
        }
        case T_ARRAY: {
          uint32_t stride = 0;
          if (!deco_find(m, cur, 0xFFFFFFFFu, DecoArrayStride, &stride)) stride = (block_bytes(m, t->elem, 0, 0) + 15) & ~15u;
          if (is_const) {
            r->byte_off += index * stride;
          } else {
            if (stride % 16) return sp_fail(m, "dynamic indexing of an array whose stride is not a multiple of 16 bytes");
            Val *iv = val_of(m, m->w[off + k]);
            if (!iv) return 1;
            Leaf ix = as_temp(m, &iv->leaf[0]);
            Leaf scaled = ix;
            if (stride != 16) {
              Leaf mul_k = imm_u(stride / 16, 1, CLS_I);
              scaled = alu(m, D_IMUL, 1, CLS_I, &ix, &mul_k, 0);
              /* imul has two destinations: the high word first. Rewrite as such. */
              SpIns *ins = &m->code[m->ncode - 1];
              ins->dst[1] = ins->dst[0];
              ins->dst[0] = op_none();
              ins->dst[0].file = OF_NULL;
              ins->ndst = 2;
            }
            if (r->dyn_on) {
              Leaf prev;
              sp_zero(&prev, sizeof prev);
              prev.op = op_temp(r->dyn_reg);
              prev.op.swz[0] = prev.op.swz[1] = prev.op.swz[2] = prev.op.swz[3] = (sp_u8)r->dyn_comp;
              prev.n = 1, prev.cls = CLS_I;
              scaled = alu(m, D_IADD, 1, CLS_I, &prev, &scaled, 0);
            }
            r->dyn_on = 1, r->dyn_reg = scaled.op.idx, r->dyn_comp = scaled.op.swz[0];
          }
          cur = t->elem;
          break;
        }
        case T_MAT:
          if (!is_const) return sp_fail(m, "dynamic matrix column index");
          r->byte_off += index * r->mstride;
          cur = t->elem;
          break;
        case T_VEC:
          if (!is_const) return sp_fail(m, "dynamic vector component index");
          r->byte_off += index * 4;
          cur = t->elem;
          break;
        default: return sp_fail(m, "bad access chain");
      }
      continue;
    }
    /* a register-resident variable */
    if (!is_const) return sp_fail(m, "dynamic indexing of an array or vector held in registers");
    switch (t->kind) {
      case T_STRUCT:
        if (index >= t->nmem) return sp_fail(m, "struct index");
        for (uint32_t i = 0; i < index; ++i) r->leaf0 += (uint32_t)type_nleaf(m, t->mem[i]);
        cur = t->mem[index];
        break;
      case T_ARRAY: case T_MAT:
        if (index >= t->count) return sp_fail(m, "index past the end of an array");
        r->leaf0 += index * (uint32_t)type_nleaf(m, t->elem);
        cur = t->elem;
        break;
      case T_VEC:
        if (index >= t->count) return sp_fail(m, "vector component index");
        r->comp = (sp_u8)index;
        cur = t->elem;
        break;
      default: return sp_fail(m, "bad access chain");
    }
  }
  r->type = cur;
  if (id < m->bound) set_val(m, id, r);
  return 0;
}

static const Leaf *var_leaf(Mod *m, const Val *ptr, uint32_t i) {
  Val *var = m->vals[ptr->var];
  if (!var || ptr->leaf0 + i >= var->nleaf) {
    sp_fail(m, "access past the end of a variable");
    return 0;
  }
  return &var->leaf[ptr->leaf0 + i];
}

static int do_load(Mod *m, uint32_t off) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *p = val_of(m, m->w[off + 3]);
  if (!p) return 1;
  Type *rt = get_type(m, rtype);
  if (rt && (rt->kind == T_IMAGE || rt->kind == T_SAMPLER || rt->kind == T_SAMPLED)) {
    Val *r = new_val(m, rtype, 0);
    if (!r) return sp_fail(m, "out of memory");
    {
      Leaf *keep = r->leaf;
      *r = *p;
      r->leaf = keep, r->nleaf = 0;
    }
    r->is_ptr = 0;
    r->type = rtype;
    set_val(m, id, r);
    return 0;
  }
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  if (p->is_cb) {
    CbRead rd;
    rd.res = p->var, rd.dyn_on = p->dyn_on, rd.dyn_reg = p->dyn_reg, rd.dyn_comp = p->dyn_comp, rd.mstride = p->mstride;
    uint32_t k = 0;
    Leaf tmp[SP_MAX_ID_LEAVES];
    if (cb_leaves(m, &rd, rtype, p->byte_off, tmp, &k)) return 1;
    if (k != r->nleaf) return sp_fail(m, "uniform load shape");
    for (uint32_t i = 0; i < k; ++i) r->leaf[i] = tmp[i];
    return 0;
  }
  Val *var = m->vals[p->var];
  if (!var) return sp_fail(m, "load from an unknown variable");
  Type *vt = get_type(m, var->type);
  (void)vt;
  uint32_t storage = (uint32_t)(sp_u64)0;
  (void)storage;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    const Leaf *src = var_leaf(m, p, i);
    if (!src) return 1;
    Leaf s = *src;
    if (p->comp < 4) s = component_of(&s, p->comp);
    s.cls = rt->kind == T_VEC || rt->kind == T_MAT ? get_type(m, rt->kind == T_MAT ? get_type(m, rt->elem)->elem : rt->elem)->cls : rt->cls;
    if (rt->kind != T_VEC && rt->kind != T_MAT && rt->kind != T_ARRAY && rt->kind != T_STRUCT) s.cls = rt->cls;
    if (src->op.file == OF_IN || src->op.file == OF_CB || src->op.file == OF_IMM) {
      r->leaf[i] = s;
    } else {
      r->leaf[i] = copy_to_temp(m, &s);
      r->leaf[i].cls = s.cls;
    }
  }
  return 0;
}

static int do_store(Mod *m, uint32_t ptr_id, uint32_t value_id) {
  Val *p = val_of(m, ptr_id), *v = val_of(m, value_id);
  if (!p || !v) return 1;
  if (p->is_cb) return sp_fail(m, "store to a uniform block");
  Val *var = m->vals[p->var];
  if (!var) return sp_fail(m, "store to an unknown variable");
  for (uint32_t i = 0; i < v->nleaf; ++i) {
    const Leaf *dst = var_leaf(m, p, i);
    if (!dst) return 1;
    if (dst->op.file == OF_IN || dst->op.file == OF_CB) return sp_fail(m, "store to an input");
    store_leaf(m, dst, &v->leaf[i], p->comp < 4 ? p->comp + 1 : 0);
  }
  return 0;
}

/* ---- images --------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static int image_unit(Mod *m, const Val *image, uint32_t *unit, uint32_t *res_index) {
  *res_index = image->image_res;
  if (*res_index >= m->nres) return sp_fail(m, "image without a resource");
  *unit = m->res[*res_index].slot + image->image_unit_off;
  if (*unit >= 8) return sp_fail(m, "texture unit out of range");
  m->unit_used[*unit] = 1;
  m->image_dim_by_unit[*unit] = m->res[*res_index].dim;
  if (*unit + 1 > m->nunits) m->nunits = *unit + 1;
  return 0;
}

static SpOp res_op(uint32_t file, uint32_t unit) {
  SpOp o;
  sp_zero(&o, sizeof o);
  o.file = (sp_u8)file, o.idx = unit;
  o.swz[0] = 0, o.swz[1] = 1, o.swz[2] = 2, o.swz[3] = 3;
  return o;
}

static int do_image_sample(Mod *m, uint32_t off) {
  uint32_t op = OPC(off), len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *si = val_of(m, m->w[off + 3]), *coord = val_of(m, m->w[off + 4]);
  if (!si || !coord) return 1;
  uint32_t unit, ri;
  if (image_unit(m, si, &unit, &ri)) return 1;
  int dref = op == OpImageSampleDrefImplicitLod || op == OpImageSampleDrefExplicitLod;
  int explicit_lod = op == OpImageSampleExplicitLod || op == OpImageSampleDrefExplicitLod;
  int proj = op == OpImageSampleProjImplicitLod || op == OpImageSampleProjExplicitLod || op == OpImageSampleProjDrefImplicitLod || op == OpImageSampleProjDrefExplicitLod;
  if (proj) return sp_fail(m, "projective texture lookups");
  uint32_t k = 5;
  Val *dref_v = 0;
  if (dref) dref_v = val_of(m, m->w[off + k++]);
  uint32_t mask = k < len ? m->w[off + k++] : 0;
  Val *bias = 0, *lod = 0, *gx = 0, *gy = 0;
  if (mask & 1) bias = val_of(m, m->w[off + k++]);
  if (mask & 2) lod = val_of(m, m->w[off + k++]);
  if (mask & 4) gx = val_of(m, m->w[off + k]), gy = val_of(m, m->w[off + k + 1]), k += 2;
  if (mask & ~7u) return sp_fail(m, "texture lookup with offsets or a minimum lod");
  if (m->failed) return 1;
  (void)explicit_lod;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  /* a cube map, 3D texture and array take their whole coordinate; a 2D one the first two components */
  Leaf coord_use = coord->leaf[0];
  if (m->res[ri].dim == SPV_DIM_2D && coord_use.n > 2) {
    const Leaf *src2[2] = {&coord->leaf[0], &coord->leaf[0]};
    sp_u8 comps[2] = {0, 1};
    coord_use = gather(m, src2, comps, 2, CLS_F);
  }
  Leaf result = temp_leaf(m, dref ? 1 : 4, CLS_F, 0);
  uint16_t dop = D_SAMPLE;
  if (dref) dop = (lod && lod->leaf[0].op.file == OF_IMM && lod->leaf[0].op.imm[0] == 0) || !lod ? (lod || op == OpImageSampleDrefExplicitLod ? D_SAMPLE_C_LZ : D_SAMPLE_C) : 0;
  else if (lod) dop = D_SAMPLE_L;
  else if (bias) dop = D_SAMPLE_B;
  else if (gx) dop = D_SAMPLE_D;
  if (!dop) return sp_fail(m, "a depth comparison with an explicit lod other than 0");
  /* In a vertex shader there are no derivatives: an implicit lookup is a lookup at lod 0. */
  Leaf zero_lod = imm_f(0.0f, 1);
  if (m->stage == SPV_STAGE_VERTEX && dop == D_SAMPLE) dop = D_SAMPLE_L, lod = 0;
  SpIns *i = emit_ins(m, dop);
  i->dst[0] = dst_temp(&result);
  i->ndst = 1;
  add_src(i, &coord_use, coord_use.n);
  SpOp rop = res_op(OF_RES, unit);
  i->src[i->nsrc++] = rop;
  i->src[i->nsrc++] = res_op(OF_SAMP, unit);
  if (dref) add_src(i, &dref_v->leaf[0], 1);
  if (dop == D_SAMPLE_L && !dref) add_src(i, lod ? &lod->leaf[0] : &zero_lod, 1);
  if (dop == D_SAMPLE_B) add_src(i, &bias->leaf[0], 1);
  if (dop == D_SAMPLE_D) add_src(i, &gx->leaf[0], gx->leaf[0].n), add_src(i, &gy->leaf[0], gy->leaf[0].n);
  if (m->res[ri].kind == SPV_RES_IMAGE && si->sampler_res < m->nres) m->res[ri].sampler_slot = si->sampler_res;
  r->leaf[0] = result;
  if (dref) {
    /* the result is a scalar; sample_c replicates it: the leaf is its first component */
    r->leaf[0].n = 1;
    r->leaf[0].op.swz[1] = r->leaf[0].op.swz[2] = r->leaf[0].op.swz[3] = 0;
  }
  return 0;
}

static int do_image_fetch(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *img = val_of(m, m->w[off + 3]), *coord = val_of(m, m->w[off + 4]);
  if (!img || !coord) return 1;
  uint32_t unit, ri;
  if (image_unit(m, img, &unit, &ri)) return 1;
  Val *lod = 0;
  uint32_t mask = len > 5 ? m->w[off + 5] : 0, k = 6;
  if (mask & 1) k++;
  if (mask & 2) lod = val_of(m, m->w[off + k++]);
  if (mask & ~2u) return sp_fail(m, "texel fetch with offsets or a sample");
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  /* address: the coordinates, then the mip level */
  Leaf addr = temp_leaf(m, 4, CLS_I, 0);
  uint32_t nc = coord->leaf[0].n;
  SpIns *a = emit_ins(m, D_MOV);
  a->dst[0] = addr.op, a->dst[0].mask = (sp_u8)full_mask(nc), a->ndst = 1;
  add_src(a, &coord->leaf[0], nc);
  SpIns *b = emit_ins(m, D_MOV);
  b->dst[0] = addr.op, b->dst[0].mask = (sp_u8)(1u << nc), b->ndst = 1;
  Leaf zero = imm_u(0, 1, CLS_I);
  add_src(b, lod ? &lod->leaf[0] : &zero, 1);
  Leaf result = temp_leaf(m, 4, CLS_F, 0);
  SpIns *i = emit_ins(m, D_LD);
  i->dst[0] = dst_temp(&result);
  i->ndst = 1;
  i->src[0] = src_of(&addr, 4);
  i->src[1] = res_op(OF_RES, unit);
  i->nsrc = 2;
  r->leaf[0] = result;
  return 0;
}

static int do_image_size(Mod *m, uint32_t off) {
  uint32_t op = OPC(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *img = val_of(m, m->w[off + 3]);
  if (!img) return 1;
  uint32_t unit, ri;
  if (image_unit(m, img, &unit, &ri)) return 1;
  Leaf zero = imm_u(0, 1, CLS_U);
  Val *lod = op == OpImageQuerySizeLod ? val_of(m, m->w[off + 4]) : 0;
  Leaf info = temp_leaf(m, 4, CLS_U, 0);
  SpIns *i = emit_ins(m, D_RESINFO);
  i->dst[0] = dst_temp(&info);
  i->ndst = 1;
  i->extra = 2;   /* uint */
  add_src(i, lod ? &lod->leaf[0] : &zero, 1);
  i->src[i->nsrc++] = res_op(OF_RES, unit);
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  Type *rt = get_type(m, rtype);
  uint32_t n = rt->kind == T_VEC ? rt->count : 1;
  Leaf out = info;
  out.n = (sp_u8)n, out.cls = CLS_I;
  r->leaf[0] = out;
  return 0;
}

/* ---- GLSL.std.450 ------------------------------------------------------------------------------------------------------------------------------------------------------- */
static Val *arg_val(Mod *m, uint32_t off, uint32_t k) { return val_of(m, m->w[off + 5 + k]); }

static int do_ext_inst(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2], inst = m->w[off + 4];
  (void)len;
  Val *a = arg_val(m, off, 0);
  if (!a) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  Leaf shape[SP_MAX_ID_LEAVES];
  uint32_t count = 0;
  if (leaves_of(m, rtype, shape, &count, SP_MAX_ID_LEAVES)) return 1;
  Val *b = len > 6 ? arg_val(m, off, 1) : a, *c = len > 7 ? arg_val(m, off, 2) : a;
  if (m->failed || !b || !c) return 1;
  uint16_t simple = 0xFFFF;
  int nsrc = 1;
  int cls_override = -1;
  switch (inst) {
    case 2: simple = D_ROUND_NE; break;                       /* RoundEven */
    case 1: simple = D_ROUND_NE; break;                       /* Round */
    case 3: simple = D_ROUND_Z; break;                        /* Trunc */
    case 8: simple = D_ROUND_NI; break;                       /* Floor */
    case 9: simple = D_ROUND_PI; break;                       /* Ceil */
    case 10: simple = D_FRC; break;                           /* Fract */
    case 31: simple = D_SQRT; break;
    case 32: simple = D_RSQ; break;
    case 29: simple = D_EXP; break;                           /* Exp2 */
    case 30: simple = D_LOG; break;                           /* Log2 */
    case 37: simple = D_MIN, nsrc = 2; break;                 /* FMin */
    case 40: simple = D_MAX, nsrc = 2; break;                 /* FMax */
    case 79: simple = D_MIN, nsrc = 2; break;                 /* NMin */
    case 80: simple = D_MAX, nsrc = 2; break;                 /* NMax */
    case 38: simple = D_UMIN, nsrc = 2; break;
    case 39: simple = D_IMIN, nsrc = 2; break;
    case 41: simple = D_UMAX, nsrc = 2; break;
    case 42: simple = D_IMAX, nsrc = 2; break;
    case 50: simple = D_MAD, nsrc = 3; break;                 /* Fma */
    default: break;
  }
  if (simple != 0xFFFF) {
    for (uint32_t i = 0; i < count; ++i) r->leaf[i] = alu(m, simple, shape[i].n, shape[i].cls, &a->leaf[i], nsrc > 1 ? &b->leaf[i] : 0, nsrc > 2 ? &c->leaf[i] : 0);
    (void)cls_override;
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    int n = shape[i].n;
    const Leaf *x = &a->leaf[i];
    Leaf one = imm_f(1.0f, 1), zero = imm_f(0.0f, 1);
    switch (inst) {
      case 4: {   /* FAbs */
        Leaf t = *x;
        t.op.abs = 1, t.op.neg = 0;
        r->leaf[i] = t;
        break;
      }
      case 5: {   /* SAbs: max(x, -x) */
        Leaf neg = alu(m, D_INEG, n, CLS_I, x, 0, 0);
        r->leaf[i] = alu(m, D_IMAX, n, CLS_I, x, &neg, 0);
        break;
      }
      case 6: {   /* FSign: (x > 0) - (x < 0) */
        Leaf z = imm_f(0.0f, 1);
        Leaf gt = alu(m, D_LT, n, CLS_B, &z, x, 0);
        Leaf lt = alu(m, D_LT, n, CLS_B, x, &z, 0);
        Leaf pos = imm_u(0x3F800000u, 1, CLS_U), neg = imm_u(0xBF800000u, 1, CLS_U);
        Leaf p = alu(m, D_AND, n, CLS_F, &gt, &pos, 0);
        Leaf q = alu(m, D_AND, n, CLS_F, &lt, &neg, 0);
        r->leaf[i] = alu(m, D_ADD, n, CLS_F, &p, &q, 0);
        break;
      }
      case 11: {  /* Radians */
        Leaf k = imm_f(0.017453292519943295f, 1);
        r->leaf[i] = alu(m, D_MUL, n, CLS_F, x, &k, 0);
        break;
      }
      case 12: {  /* Degrees */
        Leaf k = imm_f(57.29577951308232f, 1);
        r->leaf[i] = alu(m, D_MUL, n, CLS_F, x, &k, 0);
        break;
      }
      case 13: case 14: case 15: {   /* Sin, Cos, Tan */
        Leaf sn = temp_leaf(m, (sp_u8)n, CLS_F, 0), cs = temp_leaf(m, (sp_u8)n, CLS_F, 0);
        SpIns *s = emit_ins(m, D_SINCOS);
        s->dst[0] = dst_temp(&sn);
        s->dst[1] = dst_temp(&cs);
        s->ndst = 2;
        add_src(s, x, n);
        if (inst == 13) r->leaf[i] = sn;
        else if (inst == 14) r->leaf[i] = cs;
        else r->leaf[i] = alu(m, D_DIV, n, CLS_F, &sn, &cs, 0);
        break;
      }
      case 26: {  /* Pow: exp2(y * log2(x)) */
        Leaf l = alu(m, D_LOG, n, CLS_F, x, 0, 0);
        Leaf p = alu(m, D_MUL, n, CLS_F, &b->leaf[i], &l, 0);
        r->leaf[i] = alu(m, D_EXP, n, CLS_F, &p, 0, 0);
        break;
      }
      case 27: {  /* Exp: exp2(x * log2(e)) */
        Leaf k = imm_f(1.4426950408889634f, 1);
        Leaf p = alu(m, D_MUL, n, CLS_F, x, &k, 0);
        r->leaf[i] = alu(m, D_EXP, n, CLS_F, &p, 0, 0);
        break;
      }
      case 28: {  /* Log: log2(x) * ln(2) */
        Leaf k = imm_f(0.6931471805599453f, 1);
        Leaf l = alu(m, D_LOG, n, CLS_F, x, 0, 0);
        r->leaf[i] = alu(m, D_MUL, n, CLS_F, &l, &k, 0);
        break;
      }
      case 43: {  /* FClamp */
        Leaf lo = alu(m, D_MAX, n, CLS_F, x, &b->leaf[i], 0);
        r->leaf[i] = alu(m, D_MIN, n, CLS_F, &lo, &c->leaf[i], 0);
        break;
      }
      case 81: {  /* NClamp */
        Leaf lo = alu(m, D_MAX, n, CLS_F, x, &b->leaf[i], 0);
        r->leaf[i] = alu(m, D_MIN, n, CLS_F, &lo, &c->leaf[i], 0);
        break;
      }
      case 44: {  /* UClamp */
        Leaf lo = alu(m, D_UMAX, n, CLS_U, x, &b->leaf[i], 0);
        r->leaf[i] = alu(m, D_UMIN, n, CLS_U, &lo, &c->leaf[i], 0);
        break;
      }
      case 45: {  /* SClamp */
        Leaf lo = alu(m, D_IMAX, n, CLS_I, x, &b->leaf[i], 0);
        r->leaf[i] = alu(m, D_IMIN, n, CLS_I, &lo, &c->leaf[i], 0);
        break;
      }
      case 46: {  /* FMix: x + (y - x) * a */
        Leaf neg = *x;
        neg.op.neg ^= 1;
        Leaf d = alu(m, D_ADD, n, CLS_F, &b->leaf[i], &neg, 0);
        r->leaf[i] = alu(m, D_MAD, n, CLS_F, &d, &c->leaf[i], x);
        break;
      }
      case 48: {  /* Step(edge, x): x >= edge ? 1 : 0 */
        Leaf ge = alu(m, D_GE, n, CLS_B, &b->leaf[i], x, 0);
        Leaf bits = imm_u(0x3F800000u, 1, CLS_U);
        r->leaf[i] = alu(m, D_AND, n, CLS_F, &ge, &bits, 0);
        break;
      }
      case 49: {  /* SmoothStep(e0, e1, x) */
        Leaf neg = a->leaf[i];
        neg.op.neg ^= 1;
        Leaf num = alu(m, D_ADD, n, CLS_F, &c->leaf[i], &neg, 0);
        Leaf den = alu(m, D_ADD, n, CLS_F, &b->leaf[i], &neg, 0);
        Leaf q = alu(m, D_DIV, n, CLS_F, &num, &den, 0);
        Leaf lo = alu(m, D_MAX, n, CLS_F, &q, &zero, 0);
        Leaf t = alu(m, D_MIN, n, CLS_F, &lo, &one, 0);
        Leaf two = imm_f(-2.0f, 1), three = imm_f(3.0f, 1);
        Leaf s = alu(m, D_MAD, n, CLS_F, &t, &two, &three);
        Leaf tt = alu(m, D_MUL, n, CLS_F, &t, &t, 0);
        r->leaf[i] = alu(m, D_MUL, n, CLS_F, &tt, &s, 0);
        break;
      }
      case 66: case 67: case 69: {   /* Length, Distance, Normalize: below, on whole vectors */
        break;
      }
      default:
        break;
    }
    if (inst == 66 || inst == 67 || inst == 69 || inst == 68 || inst == 71) break;
    if (inst != 4 && inst != 5 && inst != 6 && inst != 11 && inst != 12 && inst != 13 && inst != 14 && inst != 15 && inst != 26 && inst != 27 && inst != 28 && inst != 43 && inst != 81 && inst != 44 &&
        inst != 45 && inst != 46 && inst != 48 && inst != 49)
      return sp_fail2(m, "unsupported GLSL.std.450 instruction", inst);
  }
  /* Vector functions. */
  if (inst == 66 || inst == 67 || inst == 69 || inst == 68 || inst == 71) {
    const Leaf *x = &a->leaf[0];
    int n = x->n;
    uint16_t dp = n == 1 ? D_MUL : n == 2 ? D_DP2 : n == 3 ? D_DP3 : D_DP4;
    if (inst == 66 || inst == 67) {   /* Length, Distance */
      Leaf d = *x;
      if (inst == 67) {
        Leaf neg = b->leaf[0];
        neg.op.neg ^= 1;
        d = alu(m, D_ADD, n, CLS_F, x, &neg, 0);
      }
      Leaf sq = alu(m, dp, 1, CLS_F, &d, &d, 0);
      r->leaf[0] = alu(m, D_SQRT, 1, CLS_F, &sq, 0, 0);
    } else if (inst == 69) {          /* Normalize */
      Leaf sq = alu(m, dp, 1, CLS_F, x, x, 0);
      Leaf rs = alu(m, D_RSQ, 1, CLS_F, &sq, 0, 0);
      r->leaf[0] = n == 1 ? alu(m, D_MUL, 1, CLS_F, x, &rs, 0) : alu(m, D_MUL, n, CLS_F, x, &rs, 0);
    } else if (inst == 68) {          /* Cross */
      static const sp_u8 s1[4] = {1, 2, 0, 0}, s2[4] = {2, 0, 1, 0};
      Leaf y = b->leaf[0];
      Leaf xa = *x, xb = *x, ya = y, yb = y;
      for (int k = 0; k < 3; ++k) {
        xa.op.swz[k] = x->op.swz[s1[k]], xb.op.swz[k] = x->op.swz[s2[k]];
        ya.op.swz[k] = y.op.swz[s1[k]], yb.op.swz[k] = y.op.swz[s2[k]];
      }
      xa.n = xb.n = ya.n = yb.n = 3;
      /* x.yzx * y.zxy - x.zxy * y.yzx */
      Leaf t = alu(m, D_MUL, 3, CLS_F, &xb, &ya, 0);
      t.op.neg ^= 1;
      r->leaf[0] = alu(m, D_MAD, 3, CLS_F, &xa, &yb, &t);
    } else {                          /* Reflect(I, N) = I - 2 * dot(N, I) * N */
      Leaf d = alu(m, dp, 1, CLS_F, &b->leaf[0], x, 0);
      Leaf two = imm_f(-2.0f, 1);
      Leaf s = alu(m, D_MUL, 1, CLS_F, &d, &two, 0);
      r->leaf[0] = alu(m, D_MAD, n, CLS_F, &s, &b->leaf[0], x);
    }
  }
  return 0;
}

/* ---- matrices ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static uint16_t dot_op(int n) { return n == 1 ? D_MUL : n == 2 ? D_DP2 : n == 3 ? D_DP3 : D_DP4; }

static Leaf scalar_of(const Leaf *v, int comp) { return component_of(v, comp); }

/* M * v: the sum of the columns scaled by the components of v. */
static Leaf mat_times_vec(Mod *m, const Leaf *cols, int ncols, int rows, const Leaf *v) {
  Leaf s0 = scalar_of(v, 0);
  Leaf acc = alu(m, D_MUL, rows, CLS_F, &cols[0], &s0, 0);
  for (int c = 1; c < ncols; ++c) {
    Leaf s = scalar_of(v, c);
    acc = alu(m, D_MAD, rows, CLS_F, &cols[c], &s, &acc);
  }
  return acc;
}

static int do_matrix(Mod *m, uint32_t off) {
  uint32_t op = OPC(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = op != OpTranspose ? val_of(m, m->w[off + 4]) : 0;
  if (!a || (op != OpTranspose && !b)) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  switch (op) {
    case OpMatrixTimesVector:
      r->leaf[0] = mat_times_vec(m, a->leaf, (int)a->nleaf, a->leaf[0].n, &b->leaf[0]);
      break;
    case OpVectorTimesMatrix:
      /* result[j] = dot(v, column j) */
      for (uint32_t j = 0; j < b->nleaf; ++j) {
        int rows = b->leaf[0].n;
        Leaf d = alu(m, dot_op(rows), 1, CLS_F, &a->leaf[0], &b->leaf[j], 0);
        if (j == 0) r->leaf[0] = temp_leaf(m, (sp_u8)b->nleaf, CLS_F, 0);
        SpIns *i = emit_ins(m, D_MOV);
        i->dst[0] = r->leaf[0].op, i->dst[0].mask = (sp_u8)(1u << j), i->ndst = 1;
        add_src(i, &d, 1);
      }
      break;
    case OpMatrixTimesMatrix:
      for (uint32_t j = 0; j < b->nleaf; ++j) r->leaf[j] = mat_times_vec(m, a->leaf, (int)a->nleaf, a->leaf[0].n, &b->leaf[j]);
      break;
    case OpMatrixTimesScalar:
      for (uint32_t j = 0; j < a->nleaf; ++j) {
        Leaf s = b->leaf[0];
        r->leaf[j] = alu(m, D_MUL, a->leaf[j].n, CLS_F, &a->leaf[j], &s, 0);
      }
      break;
    case OpOuterProduct:
      for (uint32_t j = 0; j < (uint32_t)b->leaf[0].n; ++j) {
        Leaf s = scalar_of(&b->leaf[0], (int)j);
        r->leaf[j] = alu(m, D_MUL, a->leaf[0].n, CLS_F, &a->leaf[0], &s, 0);
      }
      break;
    case OpTranspose: {
      int cols = (int)a->nleaf, rows = a->leaf[0].n;
      for (int j = 0; j < rows; ++j) {
        const Leaf *from[4];
        sp_u8 comps[4];
        for (int k = 0; k < cols; ++k) from[k] = &a->leaf[k], comps[k] = (sp_u8)j;
        r->leaf[j] = gather(m, from, comps, cols, CLS_F);
      }
      break;
    }
    default: return 1;
  }
  return 0;
}

/* ---- the plain instructions ---------------------------------------------------------------------------------------------------------------------------------------- */
static int do_composite_construct(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Type *t = get_type(m, rtype);
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  if (t->kind == T_VEC) {
    const Leaf *from[4];
    sp_u8 comps[4];
    uint32_t n = 0;
    for (uint32_t k = 3; k < len; ++k) {
      Val *mv = val_of(m, m->w[off + k]);
      if (!mv) return 1;
      for (int c = 0; c < mv->leaf[0].n && n < 4; ++c) from[n] = &mv->leaf[0], comps[n] = (sp_u8)c, ++n;
    }
    if (n != t->count) return sp_fail(m, "vector construction of the wrong size");
    r->leaf[0] = gather(m, from, comps, (int)n, get_type(m, t->elem)->cls);
    return 0;
  }
  uint32_t k = 0;
  for (uint32_t i = 3; i < len; ++i) {
    Val *mv = val_of(m, m->w[off + i]);
    if (!mv) return 1;
    for (uint32_t j = 0; j < mv->nleaf; ++j) {
      if (k >= r->nleaf) return sp_fail(m, "composite construction shape");
      r->leaf[k++] = mv->leaf[j];
    }
  }
  return 0;
}

/* The leaf range and component of a literal index path in a composite. */
static int walk_path(Mod *m, uint32_t tid, const uint32_t *idx, uint32_t n, uint32_t *leaf0, int *comp, uint32_t *out_type) {
  *leaf0 = 0, *comp = -1;
  for (uint32_t i = 0; i < n; ++i) {
    Type *t = get_type(m, tid);
    if (!t) return sp_fail(m, "composite access through an unknown type");
    switch (t->kind) {
      case T_STRUCT:
        if (idx[i] >= t->nmem) return sp_fail(m, "member index");
        for (uint32_t j = 0; j < idx[i]; ++j) *leaf0 += (uint32_t)type_nleaf(m, t->mem[j]);
        tid = t->mem[idx[i]];
        break;
      case T_ARRAY: case T_MAT:
        *leaf0 += idx[i] * (uint32_t)type_nleaf(m, t->elem);
        tid = t->elem;
        break;
      case T_VEC:
        *comp = (int)idx[i];
        tid = t->elem;
        break;
      default: return sp_fail(m, "composite access into a scalar");
    }
  }
  *out_type = tid;
  return 0;
}

static int do_composite_extract(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *c = val_of(m, m->w[off + 3]);
  if (!c) return 1;
  uint32_t leaf0, t;
  int comp;
  if (walk_path(m, c->type, &m->w[off + 4], len - 4, &leaf0, &comp, &t)) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  if (leaf0 + (comp >= 0 ? 1 : r->nleaf) > c->nleaf) return sp_fail(m, "composite extract shape");
  if (comp >= 0) {
    r->leaf[0] = component_of(&c->leaf[leaf0], comp);
    r->leaf[0].cls = get_type(m, rtype)->cls;
    return 0;
  }
  for (uint32_t i = 0; i < r->nleaf; ++i) r->leaf[i] = c->leaf[leaf0 + i];
  return 0;
}

static int do_composite_insert(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *obj = val_of(m, m->w[off + 3]), *c = val_of(m, m->w[off + 4]);
  if (!obj || !c) return 1;
  uint32_t leaf0, t;
  int comp;
  if (walk_path(m, c->type, &m->w[off + 5], len - 5, &leaf0, &comp, &t)) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) r->leaf[i] = c->leaf[i];
  if (comp >= 0) {
    Leaf v = copy_to_temp(m, &c->leaf[leaf0]);
    SpIns *i = emit_ins(m, D_MOV);
    i->dst[0] = v.op, i->dst[0].mask = (sp_u8)(1u << comp), i->ndst = 1;
    add_src(i, &obj->leaf[0], 1);
    r->leaf[leaf0] = v;
    return 0;
  }
  for (uint32_t i = 0; i < obj->nleaf; ++i) r->leaf[leaf0 + i] = obj->leaf[i];
  return 0;
}

static int do_vector_shuffle(Mod *m, uint32_t off) {
  uint32_t len = LEN(off), rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
  if (!a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  uint32_t n = len - 5;
  const Leaf *from[4];
  sp_u8 comps[4];
  Leaf undef = imm_u(0, 1, CLS_F);
  for (uint32_t i = 0; i < n && i < 4; ++i) {
    uint32_t sel = m->w[off + 5 + i];
    if (sel == 0xFFFFFFFFu) from[i] = &undef, comps[i] = 0;
    else if (sel < a->leaf[0].n) from[i] = &a->leaf[0], comps[i] = (sp_u8)sel;
    else from[i] = &b->leaf[0], comps[i] = (sp_u8)(sel - a->leaf[0].n);
  }
  r->leaf[0] = gather(m, from, comps, (int)n, a->leaf[0].cls);
  return 0;
}

static int do_convert(Mod *m, uint32_t off, uint16_t dop) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  return ew(m, rtype, id, dop, 1, m->w[off + 3], 0, 0, 0, -1);
}

static int do_bitcast(Mod *m, uint32_t off) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]);
  if (!a) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  Leaf shape[SP_MAX_ID_LEAVES];
  uint32_t count = 0;
  if (leaves_of(m, rtype, shape, &count, SP_MAX_ID_LEAVES) || count != a->nleaf) return sp_fail(m, "bitcast between different shapes");
  for (uint32_t i = 0; i < count; ++i) {
    Leaf l = a->leaf[i];
    if (l.op.neg || l.op.abs) l = copy_to_temp(m, &l);
    l.cls = shape[i].cls;
    r->leaf[i] = l;
  }
  return 0;
}

static int do_select(Mod *m, uint32_t off) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *cond = val_of(m, m->w[off + 3]), *a = val_of(m, m->w[off + 4]), *b = val_of(m, m->w[off + 5]);
  if (!cond || !a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    const Leaf *cl = cond->nleaf == 1 ? &cond->leaf[0] : &cond->leaf[i];
    r->leaf[i] = alu(m, D_MOVC, a->leaf[i].n, a->leaf[i].cls, cl, &a->leaf[i], &b->leaf[i]);
  }
  return 0;
}

static int do_any_all(Mod *m, uint32_t off, int all) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]);
  if (!a) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  Leaf acc = component_of(&a->leaf[0], 0);
  for (int c = 1; c < a->leaf[0].n; ++c) {
    Leaf x = component_of(&a->leaf[0], c);
    acc = alu(m, all ? D_AND : D_OR, 1, CLS_B, &acc, &x, 0);
  }
  r->leaf[0] = acc;
  return 0;
}

static int do_dot(Mod *m, uint32_t off) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
  if (!a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  r->leaf[0] = alu(m, dot_op(a->leaf[0].n), 1, CLS_F, &a->leaf[0], &b->leaf[0], 0);
  return 0;
}

/* x - y * floor(x / y), or with trunc (FRem). */
static int do_fmod(Mod *m, uint32_t off, int trunc) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
  if (!a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    int n = a->leaf[i].n;
    Leaf q = alu(m, D_DIV, n, CLS_F, &a->leaf[i], &b->leaf[i], 0);
    Leaf f = alu(m, trunc ? D_ROUND_Z : D_ROUND_NI, n, CLS_F, &q, 0, 0);
    Leaf neg = b->leaf[i];
    neg.op.neg ^= 1;
    r->leaf[i] = alu(m, D_MAD, n, CLS_F, &neg, &f, &a->leaf[i]);
  }
  return 0;
}

static int do_unsigned_div(Mod *m, uint32_t off, int remainder) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
  if (!a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    int n = a->leaf[i].n;
    Leaf q = temp_leaf(m, (sp_u8)n, CLS_U, 0), rem = temp_leaf(m, (sp_u8)n, CLS_U, 0);
    SpIns *ins = emit_ins(m, D_UDIV);
    ins->dst[0] = dst_temp(&q);
    ins->dst[1] = dst_temp(&rem);
    ins->ndst = 2;
    add_src(ins, &a->leaf[i], n);
    add_src(ins, &b->leaf[i], n);
    r->leaf[i] = remainder ? rem : q;
  }
  return 0;
}

static int do_imul(Mod *m, uint32_t off) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
  if (!a || !b) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    int n = a->leaf[i].n;
    Leaf lo = temp_leaf(m, (sp_u8)n, a->leaf[i].cls, 0);
    SpIns *ins = emit_ins(m, D_IMUL);
    ins->dst[0] = op_none();
    ins->dst[0].file = OF_NULL;
    ins->dst[1] = dst_temp(&lo);
    ins->ndst = 2;
    add_src(ins, &a->leaf[i], n);
    add_src(ins, &b->leaf[i], n);
    r->leaf[i] = lo;
  }
  return 0;
}

static int do_negate(Mod *m, uint32_t off, int is_float) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]);
  if (!a) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    Leaf l = a->leaf[i];
    if (is_float) {
      if (l.op.file == OF_IMM) {
        for (int k = 0; k < 4; ++k) l.op.imm[k] ^= 0x80000000u;
      } else {
        l.op.neg ^= 1;
      }
      r->leaf[i] = l;
    } else {
      r->leaf[i] = alu(m, D_INEG, l.n, l.cls, &l, 0, 0);
    }
  }
  return 0;
}

static int do_derivative(Mod *m, uint32_t off, int mode) {
  uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
  Val *a = val_of(m, m->w[off + 3]);
  if (!a) return 1;
  Val *r = result_val(m, rtype, id);
  if (!r) return 1;
  for (uint32_t i = 0; i < r->nleaf; ++i) {
    int n = a->leaf[i].n;
    if (mode == 0) r->leaf[i] = alu(m, D_DDX, n, CLS_F, &a->leaf[i], 0, 0);
    else if (mode == 1) r->leaf[i] = alu(m, D_DDY, n, CLS_F, &a->leaf[i], 0, 0);
    else {
      Leaf x = alu(m, D_DDX, n, CLS_F, &a->leaf[i], 0, 0), y = alu(m, D_DDY, n, CLS_F, &a->leaf[i], 0, 0);
      x.op.abs = 1, y.op.abs = 1;
      r->leaf[i] = alu(m, D_ADD, n, CLS_F, &x, &y, 0);
    }
  }
  return 0;
}

/* One non-terminator instruction of a block. */
static int gen_instruction(Mod *m, uint32_t off) {
  uint32_t op = OPC(off);
  switch (op) {
    case OpNop: case OpSourceContinued: case OpSource: case OpSourceExtension: case OpString: case OpLine: case OpNoLine: case OpName: case OpMemberName: case OpModuleProcessed:
    case OpSelectionMerge: case OpLoopMerge: case OpPhi: case OpFunctionParameter: case OpLabel:
      return 0;
    case OpUndef: {
      Val *r = result_val(m, m->w[off + 1], m->w[off + 2]);
      if (!r) return 1;
      Leaf shape[SP_MAX_ID_LEAVES];
      uint32_t count = 0;
      if (leaves_of(m, m->w[off + 1], shape, &count, SP_MAX_ID_LEAVES)) return 1;
      for (uint32_t i = 0; i < count; ++i) imm_leaf(&r->leaf[i], shape[i].n, shape[i].cls, 0, 0, 0, 0);
      return 0;
    }
    case OpLoad: return do_load(m, off);
    case OpStore: return do_store(m, m->w[off + 1], m->w[off + 2]);
    case OpCopyMemory: {
      Val *src = val_of(m, m->w[off + 2]);
      if (!src) return 1;
      /* load into a temporary value, then store */
      Val tmp;
      sp_zero(&tmp, sizeof tmp);
      return sp_fail(m, "OpCopyMemory");
    }
    case OpAccessChain: case OpInBoundsAccessChain: return do_access_chain(m, off);
    case OpCopyObject: {
      Val *a = val_of(m, m->w[off + 3]);
      if (!a) return 1;
      set_val(m, m->w[off + 2], a);
      return 0;
    }
    case OpCompositeConstruct: return do_composite_construct(m, off);
    case OpCompositeExtract: return do_composite_extract(m, off);
    case OpCompositeInsert: return do_composite_insert(m, off);
    case OpVectorShuffle: return do_vector_shuffle(m, off);
    case OpVectorExtractDynamic: return sp_fail(m, "dynamic vector indexing");
    case OpVectorInsertDynamic: return sp_fail(m, "dynamic vector indexing");
    case OpTranspose: case OpMatrixTimesVector: case OpVectorTimesMatrix: case OpMatrixTimesMatrix: case OpMatrixTimesScalar: case OpOuterProduct: return do_matrix(m, off);
    case OpVectorTimesScalar: {
      uint32_t rtype = m->w[off + 1], id = m->w[off + 2];
      Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
      if (!a || !b) return 1;
      Val *r = result_val(m, rtype, id);
      if (!r) return 1;
      r->leaf[0] = alu(m, D_MUL, a->leaf[0].n, CLS_F, &a->leaf[0], &b->leaf[0], 0);
      return 0;
    }
    case OpDot: return do_dot(m, off);
    case OpSampledImage: {
      Val *image = val_of(m, m->w[off + 3]), *sampler = val_of(m, m->w[off + 4]);
      if (!image || !sampler) return 1;
      Val *r = new_val(m, m->w[off + 1], 0);
      if (!r) return sp_fail(m, "out of memory");
      {
        Leaf *keep = r->leaf;
        *r = *image;
        r->leaf = keep, r->nleaf = 0;
      }
      r->sampler_res = sampler->image_res;
      r->type = m->w[off + 1];
      set_val(m, m->w[off + 2], r);
      return 0;
    }
    case OpImage: {
      Val *si = val_of(m, m->w[off + 3]);
      if (!si) return 1;
      set_val(m, m->w[off + 2], si);
      return 0;
    }
    case OpImageSampleImplicitLod: case OpImageSampleExplicitLod: case OpImageSampleDrefImplicitLod: case OpImageSampleDrefExplicitLod: return do_image_sample(m, off);
    case OpImageFetch: return do_image_fetch(m, off);
    case OpImageQuerySizeLod: case OpImageQuerySize: return do_image_size(m, off);
    case OpExtInst: return do_ext_inst(m, off);
    case OpConvertFToS: return do_convert(m, off, D_FTOI);
    case OpConvertFToU: return do_convert(m, off, D_FTOU);
    case OpConvertSToF: return do_convert(m, off, D_ITOF);
    case OpConvertUToF: return do_convert(m, off, D_UTOF);
    case OpBitcast: return do_bitcast(m, off);
    case OpSNegate: return do_negate(m, off, 0);
    case OpFNegate: return do_negate(m, off, 1);
    case OpIAdd: return ew(m, m->w[off + 1], m->w[off + 2], D_IADD, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpISub: {
      Val *b = val_of(m, m->w[off + 4]);
      if (!b) return 1;
      Val *nb = new_val(m, b->type, b->nleaf);
      if (!nb) return sp_fail(m, "out of memory");
      for (uint32_t i = 0; i < b->nleaf; ++i) nb->leaf[i] = alu(m, D_INEG, b->leaf[i].n, b->leaf[i].cls, &b->leaf[i], 0, 0);
      uint32_t tmp_id = 0;
      (void)tmp_id;
      Val *a = val_of(m, m->w[off + 3]);
      Val *r = result_val(m, m->w[off + 1], m->w[off + 2]);
      if (!a || !r) return 1;
      for (uint32_t i = 0; i < r->nleaf; ++i) r->leaf[i] = alu(m, D_IADD, a->leaf[i].n, a->leaf[i].cls, &a->leaf[i], &nb->leaf[i], 0);
      return 0;
    }
    case OpFAdd: return ew(m, m->w[off + 1], m->w[off + 2], D_ADD, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpFSub: {
      Val *a = val_of(m, m->w[off + 3]), *b = val_of(m, m->w[off + 4]);
      if (!a || !b) return 1;
      Val *r = result_val(m, m->w[off + 1], m->w[off + 2]);
      if (!r) return 1;
      for (uint32_t i = 0; i < r->nleaf; ++i) {
        Leaf nb = b->leaf[i];
        if (nb.op.file == OF_IMM) {
          for (int k = 0; k < 4; ++k) nb.op.imm[k] ^= 0x80000000u;
        } else {
          nb.op.neg ^= 1;
        }
        r->leaf[i] = alu(m, D_ADD, a->leaf[i].n, CLS_F, &a->leaf[i], &nb, 0);
      }
      return 0;
    }
    case OpIMul: return do_imul(m, off);
    case OpFMul: return ew(m, m->w[off + 1], m->w[off + 2], D_MUL, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpFDiv: return ew(m, m->w[off + 1], m->w[off + 2], D_DIV, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpUDiv: return do_unsigned_div(m, off, 0);
    case OpUMod: return do_unsigned_div(m, off, 1);
    case OpSDiv: case OpSRem: case OpSMod: return sp_fail(m, "signed integer division");
    case OpFMod: return do_fmod(m, off, 0);
    case OpFRem: return do_fmod(m, off, 1);
    case OpAny: return do_any_all(m, off, 0);
    case OpAll: return do_any_all(m, off, 1);
    case OpIsNan: {
      Val *a = val_of(m, m->w[off + 3]);
      if (!a) return 1;
      Val *r = result_val(m, m->w[off + 1], m->w[off + 2]);
      if (!r) return 1;
      for (uint32_t i = 0; i < r->nleaf; ++i) r->leaf[i] = alu(m, D_NE, a->leaf[i].n, CLS_B, &a->leaf[i], &a->leaf[i], 0);
      return 0;
    }
    case OpLogicalEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_IEQ, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpLogicalNotEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_INE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpLogicalOr: return ew(m, m->w[off + 1], m->w[off + 2], D_OR, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpLogicalAnd: return ew(m, m->w[off + 1], m->w[off + 2], D_AND, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpLogicalNot: return ew(m, m->w[off + 1], m->w[off + 2], D_NOT, 1, m->w[off + 3], 0, 0, 0, CLS_B);
    case OpSelect: return do_select(m, off);
    case OpIEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_IEQ, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpINotEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_INE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpUGreaterThan: return ew(m, m->w[off + 1], m->w[off + 2], D_ULT, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpSGreaterThan: return ew(m, m->w[off + 1], m->w[off + 2], D_ILT, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpUGreaterThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_UGE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpSGreaterThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_IGE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpULessThan: return ew(m, m->w[off + 1], m->w[off + 2], D_ULT, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpSLessThan: return ew(m, m->w[off + 1], m->w[off + 2], D_ILT, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpULessThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_UGE, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpSLessThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_IGE, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpFOrdEqual: case OpFUnordEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_EQ, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpFOrdNotEqual: case OpFUnordNotEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_NE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpFOrdLessThan: case OpFUnordLessThan: return ew(m, m->w[off + 1], m->w[off + 2], D_LT, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpFOrdGreaterThan: case OpFUnordGreaterThan: return ew(m, m->w[off + 1], m->w[off + 2], D_LT, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpFOrdLessThanEqual: case OpFUnordLessThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_GE, 2, m->w[off + 3], m->w[off + 4], 0, 1, CLS_B);
    case OpFOrdGreaterThanEqual: case OpFUnordGreaterThanEqual: return ew(m, m->w[off + 1], m->w[off + 2], D_GE, 2, m->w[off + 3], m->w[off + 4], 0, 0, CLS_B);
    case OpShiftRightLogical: return ew(m, m->w[off + 1], m->w[off + 2], D_USHR, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpShiftRightArithmetic: return ew(m, m->w[off + 1], m->w[off + 2], D_ISHR, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpShiftLeftLogical: return ew(m, m->w[off + 1], m->w[off + 2], D_ISHL, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpBitwiseOr: return ew(m, m->w[off + 1], m->w[off + 2], D_OR, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpBitwiseXor: return ew(m, m->w[off + 1], m->w[off + 2], D_XOR, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpBitwiseAnd: return ew(m, m->w[off + 1], m->w[off + 2], D_AND, 2, m->w[off + 3], m->w[off + 4], 0, 0, -1);
    case OpNot: return ew(m, m->w[off + 1], m->w[off + 2], D_NOT, 1, m->w[off + 3], 0, 0, 0, -1);
    case OpDPdx: case OpDPdxFine: case OpDPdxCoarse: return do_derivative(m, off, 0);
    case OpDPdy: case OpDPdyFine: case OpDPdyCoarse: return do_derivative(m, off, 1);
    case OpFwidth: case OpFwidthFine: case OpFwidthCoarse: return do_derivative(m, off, 2);
    default:
      return sp_fail2(m, "unsupported SPIR-V instruction", op);
  }
}

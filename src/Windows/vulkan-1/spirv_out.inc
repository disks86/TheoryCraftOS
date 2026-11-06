/* spirv.inc, part 5: giving out the registers and writing the DXBC container. */

#define MAX_PHYS 256   /* the kernel's d3d9::kMaxTemps */

typedef struct Interval {
  int32_t first, last;
  sp_u8 first_write, last_read_only, used;
} Interval;

static void touch(Interval *iv, uint32_t v, int32_t at, int write) {
  Interval *x = &iv[v];
  if (!x->used) {
    x->used = 1, x->first = at, x->last = at, x->first_write = (sp_u8)write, x->last_read_only = (sp_u8)!write;
    return;
  }
  if (at > x->last) x->last = at, x->last_read_only = (sp_u8)!write;
  else if (at == x->last && write) x->last_read_only = 0;
}

static int allocate_registers(Mod *m, uint32_t *phys) {
  Interval *iv = (Interval *)sp_alloc(m, (sp_u64)(m->nvreg + 1) * sizeof(Interval));
  if (!iv) return sp_fail(m, "out of memory");
  for (uint32_t i = 0; i < m->ncode; ++i) {
    SpIns *c = &m->code[i];
    /* (an instruction with two results may write the first before it has read its sources for the second: none of them shares a register with a source) */
    const int no_sharing = c->ndst > 1;
    for (uint32_t s = 0; s < c->nsrc; ++s) {
      if (c->src[s].file == OF_TEMP) touch(iv, c->src[s].idx, (int32_t)i, no_sharing);
      if (c->src[s].rel) touch(iv, c->src[s].rel_reg, (int32_t)i, no_sharing);
    }
    for (uint32_t d = 0; d < c->ndst; ++d)
      if (c->dst[d].file == OF_TEMP) touch(iv, c->dst[d].idx, (int32_t)i, 1);
  }
  /* A variable lives through every loop it appears in; a value made before a loop and read in it lives to the loop's end. */
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t li = 0; li < m->nloops; ++li) {
      int32_t b = (int32_t)m->loops[li].begin, e = (int32_t)m->loops[li].end;
      for (uint32_t v = 0; v < m->nvreg; ++v) {
        Interval *x = &iv[v];
        if (!x->used || x->first > e || x->last < b) continue;
        if (m->vreg_var[v]) {
          if (x->first > b) x->first = b, x->first_write = 1;
          if (x->last < e) x->last = e, x->last_read_only = 0;
        } else if (x->first < b && x->last < e) {
          x->last = e, x->last_read_only = 0;
        }
      }
    }
  /* Interval colouring in the order of the first occurrence. */
  int32_t busy_until[MAX_PHYS];
  sp_u8 busy_read_only[MAX_PHYS];
  for (int i = 0; i < MAX_PHYS; ++i) busy_until[i] = -1, busy_read_only[i] = 0;
  uint32_t used_regs = 0;
  /* a stable order: by first, then index (a counting sort: the firsts are instruction numbers) */
  uint32_t *order = (uint32_t *)sp_alloc(m, (sp_u64)(m->nvreg + 1) * 4);
  uint32_t *start = (uint32_t *)sp_alloc(m, ((sp_u64)m->ncode + 2) * 4);
  if (!order || !start) return sp_fail(m, "out of memory");
  uint32_t n = 0;
  for (uint32_t v = 0; v < m->nvreg; ++v)
    if (iv[v].used) ++start[iv[v].first + 1], ++n;
  for (uint32_t i = 1; i <= m->ncode; ++i) start[i] += start[i - 1];
  for (uint32_t v = 0; v < m->nvreg; ++v)
    if (iv[v].used) order[start[iv[v].first]++] = v;
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t v = order[k];
    int chosen = -1;
    for (int r = 0; r < MAX_PHYS && chosen < 0; ++r) {
      if (busy_until[r] < iv[v].first || (busy_until[r] == iv[v].first && busy_read_only[r] && iv[v].first_write)) chosen = r;
    }
    if (chosen < 0) return sp_fail(m, "the shader needs more than 256 temporary registers");
    busy_until[chosen] = iv[v].last, busy_read_only[chosen] = iv[v].last_read_only;
    phys[v] = (uint32_t)chosen;
    if ((uint32_t)chosen + 1 > used_regs) used_regs = (uint32_t)chosen + 1;
  }
  m->temps_used = used_regs;
  return 0;
}

/* ---- the DXBC token stream ------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct Writer {
  Mod *m;
  uint32_t *t;
  uint32_t n, cap;
  const uint32_t *phys;
} Writer;

static void put(Writer *w, uint32_t v) {
  if (w->n >= w->cap) {
    sp_fail(w->m, "the shader is longer than the adapter's front end takes");
    return;
  }
  w->t[w->n++] = v;
}

static uint32_t swz_bits(const sp_u8 *s) { return (uint32_t)(s[0] | (s[1] << 2) | (s[2] << 4) | (s[3] << 6)); }

/* An operand. mode: 0 source (swizzle), 1 destination (write mask), 2 source with one component selected, 3 no components (a sampler). */
static void put_operand(Writer *w, const SpOp *o, int mode) {
  uint32_t type = o->file;
  uint32_t dims = 1;
  if (o->file == OF_IMM) dims = 0;
  else if (o->file == OF_NULL || IS_SYSTEM_FILE(o->file)) dims = 0;
  else if (o->file == OF_CB) dims = 2;
  uint32_t token = (type << 12) | (dims << 20);
  uint32_t ext = 0;
  uint32_t mod = (o->abs ? 2u : 0u) | (o->neg ? 1u : 0u);
  if (mod && mode != 1 && o->file != OF_IMM) token |= 0x80000000u, ext = 1u | (mod << 6);
  if (o->file == OF_IMM) {
    uint32_t v[4];
    for (int i = 0; i < 4; ++i) {
      v[i] = o->imm[o->swz[i]];
      if (o->abs) v[i] &= 0x7FFFFFFFu;   /* (modifiers of a constant are applied to it) */
      if (o->neg) v[i] ^= 0x80000000u;
    }
    int scalar = v[0] == v[1] && v[1] == v[2] && v[2] == v[3];
    token |= scalar ? 1u : 2u;
    put(w, token);
    if (scalar) put(w, v[0]);
    else
      for (int i = 0; i < 4; ++i) put(w, v[i]);
    return;
  }
  if (o->file == OF_NULL) {
    put(w, token);
    return;
  }
  if (IS_SYSTEM_FILE(o->file)) {   /* (vThreadID and the others: no register number) */
    put(w, token | 2u | (1u << 2) | (swz_bits(o->swz) << 4));
    if (ext) put(w, ext);
    return;
  }
  if (mode == 3) {
    put(w, token);
    put(w, o->idx);
    return;
  }
  if (mode == 1) token |= 2u | ((uint32_t)(o->mask & 0xF) << 4);
  else if (mode == 2) token |= 2u | (2u << 2) | ((uint32_t)(o->swz[0] & 3) << 4);
  else token |= 2u | (1u << 2) | (swz_bits(o->swz) << 4);
  if (o->file == OF_CB && o->rel) token |= 3u << 25;
  put(w, token);
  if (ext) put(w, ext);
  if (o->file == OF_CB) {
    put(w, o->idx);
    put(w, o->idx2);
    if (o->rel) {
      put(w, (0u << 12) | (1u << 20) | 2u | (2u << 2) | ((uint32_t)(o->rel_comp & 3) << 4));
      put(w, w->phys[o->rel_reg]);
    }
    return;
  }
  uint32_t index = o->idx;
  if (o->file == OF_TEMP) index = w->phys[o->idx];
  put(w, index);
}

static void put_instruction(Writer *w, const SpIns *c) {
  uint32_t start = w->n;
  uint32_t token = c->op;
  if (c->sat) token |= 1u << 13;
  if (c->test) token |= 1u << 18;
  if (c->op == D_RESINFO || c->op == D_SYNC) token |= c->extra << 11;
  put(w, token);
  switch (c->op) {
    case D_IF: case D_BREAKC: case D_DISCARD:
      put_operand(w, &c->src[0], 2);
      break;
    default:
      for (uint32_t d = 0; d < c->ndst; ++d) put_operand(w, &c->dst[d], c->dst[d].file == OF_NULL ? 0 : 1);
      for (uint32_t s = 0; s < c->nsrc; ++s) {
        int mode = 0;
        if (c->src[s].file == OF_SAMP) mode = 3;
        put_operand(w, &c->src[s], mode);
      }
      break;
  }
  w->t[start] |= (w->n - start) << 24;
}

static uint32_t reg_mask_token(uint32_t type, uint32_t mask) { return (1u << 20) | (type << 12) | 2u | (mask << 4); }

static int write_signature(Mod *m, sp_u8 *out, uint32_t *size, int outputs) {
  /* elements: name, index, system value, register, mask */
  struct Elem {
    const char *name;
    uint32_t index, sys, reg, mask;
  } elems[17];
  uint32_t n = 0;
  const sp_u8 *decl = outputs ? m->out_decl : m->in_decl;
  for (uint32_t r = 0; r < 16; ++r) {
    if (!decl[r]) continue;
    if (r == POS_REG && decl[r] == DECL_POSITION) {
      if (!outputs) continue;   /* a pixel shader's position is declared as a system value, not in the signature */
      elems[n].name = "SV_Position", elems[n].index = 0, elems[n].sys = 1;
    } else if (r >= 12 && !outputs && (decl[r] == DECL_VERTEX_ID || decl[r] == DECL_INSTANCE_ID || decl[r] == DECL_FRONT_FACING)) {
      continue;
    } else if (m->stage == SPV_STAGE_VERTEX && !outputs) {
      elems[n].name = "ATTR", elems[n].index = r, elems[n].sys = 0;
    } else if (m->stage == SPV_STAGE_FRAGMENT && outputs) {
      elems[n].name = "SV_Target", elems[n].index = r, elems[n].sys = 0;
    } else {
      elems[n].name = r < 2 ? "COLOR" : "TEXCOORD";
      elems[n].index = r < 2 ? r : r - 2;
      elems[n].sys = 0;
    }
    elems[n].reg = r, elems[n].mask = 0xF;
    ++n;
  }
  uint32_t head = 8 + n * 24;
  uint32_t pos = head;
  uint32_t *w = (uint32_t *)out;
  w[0] = n, w[1] = 8;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t len = 0;
    while (elems[i].name[len]) ++len;
    w[2 + i * 6 + 0] = pos;
    w[2 + i * 6 + 1] = elems[i].index;
    w[2 + i * 6 + 2] = elems[i].sys;
    w[2 + i * 6 + 3] = 3;   /* float */
    w[2 + i * 6 + 4] = elems[i].reg;
    w[2 + i * 6 + 5] = elems[i].mask | (outputs ? 0u : (0xFu << 8));
    sp_copy(out + pos, elems[i].name, len + 1);
    pos += len + 1;
  }
  while (pos & 3) out[pos++] = 0;
  *size = pos;
  return 0;
}

static uint32_t imm_total_cb(Mod *m, uint32_t slot) {
  uint32_t most = 0;
  for (uint32_t i = 0; i < m->ncode; ++i)
    for (uint32_t s = 0; s < m->code[i].nsrc; ++s)
      if (m->code[i].src[s].file == OF_CB && m->code[i].src[s].idx == slot && m->code[i].src[s].idx2 + 1 > most) most = m->code[i].src[s].idx2 + 1;
  return most;
}

void spv_declare_cb(SpvModule *m, uint32_t slot, uint32_t vec4) {
  if (slot < 16) m->cb_decl[slot] = vec4;
}

static int write_container(Mod *m, const uint32_t *phys) {
  /* (an instruction is at most 1 + 2 destinations and 6 sources of up to 6 words each: 50 words; the declarations a few hundred) */
  const uint32_t kMaxWords = 4096 + m->ncode * 50;
  uint32_t *tokens = (uint32_t *)sp_alloc(m, (sp_u64)(kMaxWords + 64) * 4);
  if (!tokens) return sp_fail(m, "out of memory");
  Writer w;
  w.m = m, w.t = tokens, w.n = 0, w.cap = kMaxWords, w.phys = phys;
  uint32_t type = m->stage == SPV_STAGE_VERTEX ? 1u : m->stage == SPV_STAGE_FRAGMENT ? 0u : 5u;
  put(&w, (type << 16) | (m->stage == SPV_STAGE_COMPUTE || m->uses_memory ? 0x50u : 0x40u));
  put(&w, 0);   /* the length, filled in below */
  put(&w, D_DCL_GLOBAL_FLAGS | (1u << 11) | (1u << 24));
  /* constant buffers: every slot of the pipeline, in order (the kernel puts them end to end by slot) */
  uint32_t top = 0;
  for (uint32_t s = 0; s < 16; ++s)
    if (m->cb_decl[s] || imm_total_cb(m, s)) top = s + 1;
  for (uint32_t s = 0; s < top; ++s) {
    uint32_t size = m->cb_decl[s];
    uint32_t need = imm_total_cb(m, s);
    if (need > size) size = need;
    if (size == 0) size = 1;
    put(&w, D_DCL_CB | (4u << 24));
    put(&w, (2u << 20) | (8u << 12) | 2u | (1u << 2) | (0xE4u << 4));
    put(&w, s);
    put(&w, size);
  }
  for (uint32_t u = 0; u < m->nunits; ++u) {
    if (!m->unit_used[u]) continue;
    put(&w, D_DCL_SAMPLER | (3u << 24));
    put(&w, (OF_SAMP << 12) | (1u << 20));
    put(&w, u);
    uint32_t dim = m->image_dim_by_unit[u];
    uint32_t d = dim == SPV_DIM_3D ? 5 : dim == SPV_DIM_2D_ARRAY || dim == SPV_DIM_CUBE || dim == SPV_DIM_CUBE_ARRAY ? 8 : 3;   /* (cubes: texture2darray) */
    put(&w, D_DCL_RESOURCE | (d << 11) | (4u << 24));
    put(&w, (OF_RES << 12) | (1u << 20));
    put(&w, u);
    put(&w, 0x5555u);
  }
  /* storage buffers (raw), the group-shared memory, the work group and the compute system values */
  for (uint32_t u = 0; u < 32; ++u)
    if ((m->uav_used >> u) & 1) {
      put(&w, D_DCL_UAV_RAW | (3u << 24));
      put(&w, (OF_UAV << 12) | (1u << 20));
      put(&w, u);
    }
  for (uint32_t t = 0; t < 32; ++t)
    if ((m->srv_used >> t) & 1) {
      put(&w, D_DCL_RESOURCE_RAW | (3u << 24));
      put(&w, (OF_RES << 12) | (1u << 20));
      put(&w, t);
    }
  if (m->shared_bytes) {
    put(&w, D_DCL_TGSM_RAW | (4u << 24));
    put(&w, (OF_TGSM << 12) | (1u << 20));
    put(&w, 0);
    put(&w, m->shared_bytes);
  }
  if (m->stage == SPV_STAGE_COMPUTE) {
    uint32_t size[3];
    spv_local_size(m, size);
    put(&w, D_DCL_THREAD_GROUP | (4u << 24));
    for (int i = 0; i < 3; ++i) put(&w, size[i]);
    for (uint32_t file = OF_THREAD_ID; file <= OF_THREAD_INDEX; ++file)
      if ((m->system_used >> (file - OF_THREAD_ID)) & 1) {
        put(&w, D_DCL_INPUT | (2u << 24));
        put(&w, (file << 12) | 2u | ((file == OF_THREAD_INDEX ? 1u : 7u) << 4));
      }
  }
  for (uint32_t r = 0; r < 16; ++r) {
    uint32_t k = m->in_decl[r];
    if (!k) continue;
    if (k == DECL_POSITION) {
      put(&w, D_DCL_INPUT_PS_SIV | (2u << 11) | (4u << 24));
      put(&w, reg_mask_token(1, 0xF));
      put(&w, r);
      put(&w, 1);
    } else if (k == DECL_VERTEX_ID || k == DECL_INSTANCE_ID) {
      put(&w, D_DCL_INPUT_SGV | (4u << 24));
      put(&w, reg_mask_token(1, 1));
      put(&w, r);
      put(&w, k == DECL_VERTEX_ID ? 6u : 8u);
    } else if (k == DECL_FRONT_FACING) {
      put(&w, 99u | (4u << 24));   /* dcl_input_ps_sgv */
      put(&w, reg_mask_token(1, 1));
      put(&w, r);
      put(&w, 9);
    } else if (m->stage == SPV_STAGE_FRAGMENT) {
      put(&w, D_DCL_INPUT_PS | ((k == DECL_FLAT ? 1u : 2u) << 11) | (3u << 24));
      put(&w, reg_mask_token(1, 0xF));
      put(&w, r);
    } else {
      put(&w, D_DCL_INPUT | (3u << 24));
      put(&w, reg_mask_token(1, 0xF));
      put(&w, r);
    }
  }
  for (uint32_t r = 0; r < 16; ++r) {
    uint32_t k = m->out_decl[r];
    if (!k) continue;
    if (k == DECL_POSITION) {
      put(&w, D_DCL_OUTPUT_SIV | (4u << 24));
      put(&w, reg_mask_token(2, 0xF));
      put(&w, r);
      put(&w, 1);
    } else {
      put(&w, D_DCL_OUTPUT | (3u << 24));
      put(&w, reg_mask_token(2, 0xF));
      put(&w, r);
    }
  }
  if (m->temps_used) {
    put(&w, D_DCL_TEMPS | (2u << 24));
    put(&w, m->temps_used);
  }
  for (uint32_t i = 0; i < m->ncode; ++i) put_instruction(&w, &m->code[i]);
  put(&w, D_RET | (1u << 24));
  if (m->failed) return 1;
  tokens[1] = w.n;
  /* the container */
  sp_u8 *isgn = (sp_u8 *)sp_alloc(m, 512), *osgn = (sp_u8 *)sp_alloc(m, 512);
  if (!isgn || !osgn) return sp_fail(m, "out of memory");
  uint32_t isize = 0, osize = 0;
  write_signature(m, isgn, &isize, 0);
  write_signature(m, osgn, &osize, 1);
  uint32_t shdr_bytes = w.n * 4;
  uint32_t total = 32 + 3 * 4 + (8 + isize) + (8 + osize) + (8 + shdr_bytes);
  sp_u8 *out = (sp_u8 *)sp_alloc(m, total + 16);
  if (!out) return sp_fail(m, "out of memory");
  uint32_t *h = (uint32_t *)out;
  h[0] = 0x43425844u;
  h[5] = 1, h[6] = total, h[7] = 3;
  uint32_t pos = 32 + 12;
  uint32_t offs[3];
  offs[0] = pos;
  pos += 8 + isize;
  offs[1] = pos;
  pos += 8 + osize;
  offs[2] = pos;
  pos += 8 + shdr_bytes;
  h[8] = offs[0], h[9] = offs[1], h[10] = offs[2];
  uint32_t *c0 = (uint32_t *)(out + offs[0]);
  c0[0] = 0x4E475349u, c0[1] = isize;
  sp_copy(out + offs[0] + 8, isgn, isize);
  uint32_t *c1 = (uint32_t *)(out + offs[1]);
  c1[0] = 0x4E47534Fu, c1[1] = osize;
  sp_copy(out + offs[1] + 8, osgn, osize);
  uint32_t *c2 = (uint32_t *)(out + offs[2]);
  c2[0] = 0x52444853u, c2[1] = shdr_bytes;
  sp_copy(out + offs[2] + 8, tokens, shdr_bytes);
  m->out = out, m->out_size = total;
  return 0;
}

int spv_emit(SpvModule *m, const uint8_t **out, uint32_t *size) {
  if (m->failed) return 1;
  m->ncode = 0, m->nvreg = 0, m->nloops = 0;
  if (setup_variables(m)) return 1;
  if (gen_function(m, m->entry_func, 0, 0, 0, 0)) return 1;
  if (m->failed) return 1;
  uint32_t *phys = (uint32_t *)sp_alloc(m, (sp_u64)(m->nvreg + 1) * 4);
  if (!phys) return sp_fail(m, "out of memory");
  if (allocate_registers(m, phys)) return 1;
  if (write_container(m, phys)) return 1;
  *out = m->out, *size = m->out_size;
  return 0;
}

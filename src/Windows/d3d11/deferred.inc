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

/* Part of d3d11.c: deferred contexts and command lists.
 *
 * A deferred context has bindings of its own, which its setters change as the immediate context's do. What it is asked to do - draws, dispatches, clears,
 * copies, updates, queries, the writes of a Map - is recorded: each command with a copy of the bindings it was asked under (StateCopy, shared by the commands
 * between two changes, holding references to what is bound). FinishCommandList hands the commands over to a command list; ExecuteCommandList on the immediate
 * context runs them in order, each under its bindings, through the immediate context's own methods, and then gives the immediate context back its bindings
 * (RestoreContextState) or clears them, as Direct3D 11 says. ExecuteCommandList on a deferred context records the list's commands into it. */

#define STATE_OBJECTS ((CTX_OBJECTS_END - CTX_OBJECTS_FIRST) / sizeof(DxObj *))
#define STATE_VALUE_BYTES (CTX_VALUES_END - CTX_VALUES_FIRST)
_Static_assert(CTX_OBJECTS_END == CTX_VALUES_FIRST, "Ctx: the values follow the objects");
_Static_assert((CTX_OBJECTS_END - CTX_OBJECTS_FIRST) % sizeof(DxObj *) == 0, "Ctx: the objects are pointers");

typedef struct StateCopy {
  LONG ref;                       /* the commands that use it (and the context, for the last one made) */
  DxObj *objects[STATE_OBJECTS];  /* references */
  BYTE values[STATE_VALUE_BYTES];
} StateCopy;


/* A deferred Map of a dynamic resource: the memory the program writes, until FinishCommandList (a WRITE_NO_OVERWRITE map after a WRITE_DISCARD one of the
 * same recording gets the same memory). */
typedef struct DeferredMap { DxObj *resource; UINT sub; UINT type; BYTE *data; SIZE_T bytes; BOOL mapped; } DeferredMap;
typedef struct Deferred {
  Command *commands;
  UINT count, cap;
  StateCopy *last;               /* the copy of the bindings the last command got (a reference) */
  DeferredMap *maps;
  UINT map_count, map_cap;
} Deferred;

typedef struct CmdList {
  Child c;
  UINT flags;
  Command *commands;
  UINT count;
} CmdList;

static void state_release(StateCopy *s) {
  if (!s || InterlockedDecrement(&s->ref) != 0) return;
  for (UINT i = 0; i < STATE_OBJECTS; ++i) if (s->objects[i]) dx_Release(s->objects[i]);
  mem_free(s);
}
static void command_release(Command *m) {
  state_release(m->state);
  for (UINT i = 0; i < 8; ++i) if (m->obj[i]) dx_Release(m->obj[i]);
  mem_free(m->data);
  mem_free(m->rects);
}
static void commands_free(Command *commands, UINT count) {
  for (UINT i = 0; i < count; ++i) command_release(&commands[i]);
  mem_free(commands);
}

/* The bindings of a context now: the last copy when nothing changed since it was made, else a new one. */
static StateCopy *state_of(Ctx *c) {
  const BYTE *objects = (const BYTE *)c + CTX_OBJECTS_FIRST, *values = (const BYTE *)c + CTX_VALUES_FIRST;
  Deferred *r = c->rec;
  if (r && r->last && memcmp(r->last->objects, objects, sizeof r->last->objects) == 0 && memcmp(r->last->values, values, STATE_VALUE_BYTES) == 0) {
    InterlockedIncrement(&r->last->ref);
    return r->last;
  }
  StateCopy *s = mem_alloc(sizeof *s);
  if (!s) return NULL;
  s->ref = 1;
  memcpy(s->objects, objects, sizeof s->objects);
  memcpy(s->values, values, STATE_VALUE_BYTES);
  for (UINT i = 0; i < STATE_OBJECTS; ++i) if (s->objects[i]) dx_AddRef(s->objects[i]);
  if (r) {
    state_release(r->last);
    InterlockedIncrement(&s->ref);
    r->last = s;
  }
  return s;
}
/* A context takes on recorded bindings. */
static void state_apply(Ctx *c, const StateCopy *s) {
  UINT n;
  DxObj **objects = ctx_objects(c, &n);
  for (UINT i = 0; i < n; ++i) bind(&objects[i], s->objects[i]);
  memcpy((BYTE *)c + CTX_VALUES_FIRST, s->values, STATE_VALUE_BYTES);
  c->dev->dirty |= DIRTY_ALL & ~DIRTY_SO;      /* (the stream output targets are not part of the bindings: see so_record) */
}

/* Does the command need the bindings it was recorded under? (Draws, dispatches and the predicated commands do; queries and the writes of a Map do not.) */
static BOOL op_needs_state(UINT op) { return op != REC_BEGIN && op != REC_END && op != REC_SO_TARGETS && op != REC_UAV_COUNTS && op != REC_MIN_LOD && op != REC_MAPPED && op != REC_DISCARD; }

static Command *deferred_push(Deferred *r) {
  if (r->count == r->cap) {
    const UINT cap = r->cap ? r->cap * 2 : 64;
    Command *n = mem_grow(r->commands, (SIZE_T)cap * sizeof *n);
    if (!n) return NULL;
    r->commands = n;
    r->cap = cap;
  }
  Command *m = &r->commands[r->count++];
  memset(m, 0, sizeof *m);
  return m;
}
static void deferred_record(Ctx *c, const RecArgs *a) {
  Deferred *r = c->rec;
  if (!r) return;
  Command *m = deferred_push(r);
  if (!m) return;
  m->op = a->op;
  if (op_needs_state(a->op)) m->state = state_of(c);
  for (UINT i = 0; i < 8; ++i) if ((m->obj[i] = a->obj[i]) != NULL) dx_AddRef(m->obj[i]);
  memcpy(m->u, a->u, sizeof m->u);
  memcpy(m->f, a->f, sizeof m->f);
  if (a->box) { m->box = *a->box; m->has_box = TRUE; }
  if (a->data && a->bytes) {
    m->data = mem_alloc(a->bytes);
    if (m->data) { memcpy(m->data, a->data, a->bytes); m->bytes = a->bytes; }
  }
  if (a->rects && a->rect_count) {
    m->rects = mem_alloc(sizeof *m->rects * a->rect_count);
    if (m->rects) { memcpy(m->rects, a->rects, sizeof *m->rects * a->rect_count); m->rect_count = a->rect_count; }
  }
}

/* ---- Map on a deferred context ------------------------------------------------------------------------------------------------------------------------- */

/* The bytes a mapped subresource has (a buffer's width; a texture level's rows of its slices), and the pitches a Map gives. */
static SIZE_T mapped_bytes(ID3D11Resource *resource, UINT sub, UINT *row_pitch, UINT *depth_pitch) {
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (b) { *row_pitch = *depth_pitch = b->desc.ByteWidth; return b->desc.ByteWidth; }
  if (!t || sub >= tex_subresources(t)) return 0;
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  *row_pitch = tex_pitch(t, w);
  *depth_pitch = *row_pitch * tex_rows(t, h);
  return (SIZE_T)*depth_pitch * tex_slices(t, sub);
}
static HRESULT deferred_map(Ctx *c, ID3D11Resource *resource, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE *out) {
  Deferred *r = c->rec;
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  const D3D11_USAGE usage = b ? b->desc.Usage : t ? t->desc.Usage : D3D11_USAGE_DEFAULT;
  /* (a deferred context maps dynamic resources for writing only, the whole of them or on from what this recording wrote) */
  if (!r || usage != D3D11_USAGE_DYNAMIC || (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE)) return E_INVALIDARG;
  UINT row_pitch = 0, depth_pitch = 0;
  const SIZE_T bytes = mapped_bytes(resource, sub, &row_pitch, &depth_pitch);
  if (!bytes) return E_INVALIDARG;
  DeferredMap *m = NULL;
  for (UINT i = 0; i < r->map_count; ++i) if (r->maps[i].resource == (DxObj *)resource && r->maps[i].sub == sub) m = &r->maps[i];
  if (!m) {
    if (type == D3D11_MAP_WRITE_NO_OVERWRITE) return E_INVALIDARG;      /* (not mapped with WRITE_DISCARD in this recording: there is nothing to go on from) */
    if (r->map_count == r->map_cap) {
      const UINT cap = r->map_cap ? r->map_cap * 2 : 8;
      DeferredMap *n = mem_grow(r->maps, (SIZE_T)cap * sizeof *n);
      if (!n) return E_OUTOFMEMORY;
      r->maps = n;
      r->map_cap = cap;
    }
    m = &r->maps[r->map_count++];
    memset(m, 0, sizeof *m);
    m->resource = (DxObj *)resource;
    dx_AddRef(m->resource);
    m->sub = sub;
    m->data = mem_alloc(bytes);
    if (!m->data) { dx_Release(m->resource); --r->map_count; return E_OUTOFMEMORY; }
    memset(m->data, 0, bytes);
    m->bytes = bytes;
  }
  if (m->mapped) return E_INVALIDARG;
  m->mapped = TRUE;
  out->pData = m->data;
  out->RowPitch = row_pitch;
  out->DepthPitch = depth_pitch;
  m->type = (UINT)type;      /* (how it is mapped now, until Unmap) */
  return S_OK;
}
static void deferred_unmap(Ctx *c, ID3D11Resource *resource, UINT sub) {
  Deferred *r = c->rec;
  if (!r) return;
  for (UINT i = 0; i < r->map_count; ++i) {
    DeferredMap *m = &r->maps[i];
    if (m->resource != (DxObj *)resource || m->sub != sub || !m->mapped) continue;
    const UINT type = m->type;
    m->mapped = FALSE;
    /* (the writes: replayed as a Map of the same kind on the immediate context, with these bytes) */
    deferred_record(c, &(RecArgs){.op = REC_MAPPED, .obj = {resource}, .u = {sub, type}, .data = m->data, .bytes = m->bytes});
    return;
  }
}
static void deferred_maps_free(Deferred *r) {
  for (UINT i = 0; i < r->map_count; ++i) { dx_Release(r->maps[i].resource); mem_free(r->maps[i].data); }
  mem_free(r->maps);
  r->maps = NULL;
  r->map_count = r->map_cap = 0;
}

/* UpdateSubresource on a deferred context: the bytes it names are copied into the recording. */
static void deferred_update(Ctx *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch) {
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  SIZE_T bytes = 0;
  if (!data) return;
  if (b) {
    if (sub || (box && (box->left >= box->right || box->right > b->desc.ByteWidth))) return;
    bytes = box ? box->right - box->left : b->desc.ByteWidth;
  } else if (t && sub < tex_subresources(t)) {
    const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
    const UINT x0 = box ? box->left : 0, y0 = box ? box->top : 0, x1 = box ? box->right : w, y1 = box ? box->bottom : h;
    if (x0 >= x1 || y0 >= y1 || x1 > w || y1 > h) return;
    const UINT slices = box && t->kind == 1 ? box->back - box->front : tex_slices(t, sub);
    if (!slices) return;
    const UINT row_bytes = tex_pitch(t, x1 - x0), rows = tex_rows(t, y1 - y0);
    const UINT pitch = row_pitch ? row_pitch : row_bytes, slice_pitch = depth_pitch ? depth_pitch : pitch * rows;
    bytes = (SIZE_T)(slices - 1) * slice_pitch + (SIZE_T)(rows - 1) * pitch + row_bytes;
  } else return;
  deferred_record(c, &(RecArgs){.op = REC_UPDATE, .obj = {resource}, .u = {sub, row_pitch, depth_pitch}, .box = box, .data = data, .bytes = bytes});
}

/* ---- command lists ----------------------------------------------------------------------------------------------------------------------------------------- */

static void cmdlist_destroy(DxObj *o) {
  CmdList *l = (CmdList *)o;
  commands_free(l->commands, l->count);
  child_destroy(o);
}
static UINT STDMETHODCALLTYPE cmdlist_GetContextFlags(CmdList *l) { return l->flags; }

static void ctx_ClearState(Ctx *c);
static void ctx_init_faces(Ctx *c);
static void STDMETHODCALLTYPE ctx_GenerateMips(Ctx *c, ID3D11ShaderResourceView *view);
static HRESULT STDMETHODCALLTYPE ctx_FinishCommandList(Ctx *c, BOOL restore, ID3D11CommandList **out) {
  extern const ID3D11CommandListVtbl cmdlist_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11CommandList, NULL};
  if (out) *out = NULL;
  if (!c->deferred) return DXGI_ERROR_INVALID_CALL;
  if (!out) return E_INVALIDARG;
  Deferred *r = c->rec;
  for (UINT i = 0; i < r->map_count; ++i) if (r->maps[i].mapped) return DXGI_ERROR_INVALID_CALL;      /* (a resource still mapped) */
  CmdList *l = child_new(sizeof *l, c->dev, &cmdlist_vtbl, iids, cmdlist_destroy);
  if (!l) return E_OUTOFMEMORY;
  l->flags = c->flags;
  l->commands = r->commands;
  l->count = r->count;
  r->commands = NULL;
  r->count = r->cap = 0;
  state_release(r->last);
  r->last = NULL;
  deferred_maps_free(r);
  if (!restore) ctx_ClearState(c);      /* (the deferred context starts over from the default bindings) */
  *out = (ID3D11CommandList *)l;
  return S_OK;
}

/* Runs one recorded command on the immediate context `c`. */
static void command_run(Ctx *c, const Command *m) {
  const UINT *u = m->u;
  switch (m->op) {
    case REC_DRAW: ctx_Draw(c, u[0], u[1]); break;
    case REC_DRAW_INDEXED: ctx_DrawIndexed(c, u[0], u[1], (INT)u[2]); break;
    case REC_DRAW_INSTANCED: ctx_DrawInstanced(c, u[0], u[1], u[2], u[3]); break;
    case REC_DRAW_INDEXED_INSTANCED: ctx_DrawIndexedInstanced(c, u[0], u[1], u[2], (INT)u[3], u[4]); break;
    case REC_DRAW_AUTO: ctx_DrawAuto(c); break;
    case REC_DRAW_INDIRECT: ctx_DrawInstancedIndirect(c, (ID3D11Buffer *)m->obj[0], u[0]); break;
    case REC_DRAW_INDEXED_INDIRECT: ctx_DrawIndexedInstancedIndirect(c, (ID3D11Buffer *)m->obj[0], u[0]); break;
    case REC_DISPATCH: ctx_Dispatch(c, u[0], u[1], u[2]); break;
    case REC_DISPATCH_INDIRECT: ctx_DispatchIndirect(c, (ID3D11Buffer *)m->obj[0], u[0]); break;
    case REC_CLEAR_RTV: ctx_ClearRenderTargetView(c, (ID3D11RenderTargetView *)m->obj[0], m->f); break;
    case REC_CLEAR_DSV: ctx_ClearDepthStencilView(c, (ID3D11DepthStencilView *)m->obj[0], u[0], m->f[0], (UINT8)u[1]); break;
    case REC_CLEAR_UAV_UINT: ctx_ClearUnorderedAccessViewUint(c, (ID3D11UnorderedAccessView *)m->obj[0], u); break;
    case REC_CLEAR_UAV_FLOAT: ctx_ClearUnorderedAccessViewFloat(c, (ID3D11UnorderedAccessView *)m->obj[0], m->f); break;
    case REC_COPY_REGION:
      ctx_CopySubresourceRegion(c, (ID3D11Resource *)m->obj[0], u[0], u[1], u[2], u[3], (ID3D11Resource *)m->obj[1], u[4], m->has_box ? &m->box : NULL);
      break;
    case REC_COPY_RESOURCE: ctx_CopyResource(c, (ID3D11Resource *)m->obj[0], (ID3D11Resource *)m->obj[1]); break;
    case REC_UPDATE: if (m->data) ctx_UpdateSubresource(c, (ID3D11Resource *)m->obj[0], u[0], m->has_box ? &m->box : NULL, m->data, u[1], u[2]); break;
    case REC_COPY_STRUCTURE_COUNT: ctx_CopyStructureCount(c, (ID3D11Buffer *)m->obj[0], u[0], (ID3D11UnorderedAccessView *)m->obj[1]); break;
    case REC_RESOLVE: ctx_ResolveSubresource(c, (ID3D11Resource *)m->obj[0], u[0], (ID3D11Resource *)m->obj[1], u[1], (DXGI_FORMAT)u[2]); break;
    case REC_GENERATE_MIPS: ctx_GenerateMips(c, (ID3D11ShaderResourceView *)m->obj[0]); break;
    case REC_BEGIN: ctx_Begin(c, (ID3D11Asynchronous *)m->obj[0]); break;
    case REC_END: ctx_End(c, (ID3D11Asynchronous *)m->obj[0]); break;
    case REC_SO_TARGETS: ctx_SOSetTargets(c, u[0], (ID3D11Buffer *const *)m->obj, u + 1); break;
    case REC_UAV_COUNTS:
      /* (the hidden counts of append and counter views, set when the bind is run) */
      for (UINT i = 0; i < 8 && i < u[0]; ++i) {
        View *v = (View *)m->obj[i];
        if (v && v->buf && (v->desc.uav.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) && m->data) memcpy(&v->counter, m->data + 4 * i, 4);
      }
      break;
    case REC_MIN_LOD: ctx_SetResourceMinLOD(c, (ID3D11Resource *)m->obj[0], m->f[0]); break;
    case REC_MAPPED: {
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (m->data && SUCCEEDED(ctx_Map(c, (ID3D11Resource *)m->obj[0], u[0], (D3D11_MAP)u[1], 0, &mapped)) && mapped.pData) {
        memcpy(mapped.pData, m->data, m->bytes);
        ctx_Unmap(c, (ID3D11Resource *)m->obj[0], u[0]);
      }
      break;
    }
    case REC_CLEAR_VIEW: ctx_ClearView(c, (ID3D11View *)m->obj[0], m->f, m->rects, m->rect_count); break;
    case REC_DISCARD: break;      /* (what a discard leaves is undefined: the contents stay) */
  }
}
/* The commands of a list on the immediate context, each under its bindings. */
static void commands_run(Ctx *c, const Command *commands, UINT count) {
  const StateCopy *current = NULL;
  for (UINT i = 0; i < count; ++i) {
    const Command *m = &commands[i];
    if (m->state && m->state != current) { state_apply(c, m->state); current = m->state; }
    command_run(c, m);
  }
}

static CmdList *cmdlist_of(ID3D11CommandList *l) {
  extern const ID3D11CommandListVtbl cmdlist_vtbl;
  return l && ((DxObj *)l)->lpVtbl == (const void *)&cmdlist_vtbl ? (CmdList *)l : NULL;
}
static void STDMETHODCALLTYPE ctx_ExecuteCommandList(Ctx *c, ID3D11CommandList *list, BOOL restore) {
  CmdList *l = cmdlist_of(list);
  if (!l) return;
  if (c->deferred) {
    /* (into this recording: the list's commands, after which the deferred context's bindings stay or are cleared) */
    for (UINT i = 0; i < l->count && c->rec; ++i) {
      Command *m = deferred_push(c->rec);
      if (!m) break;
      *m = l->commands[i];
      if (m->state) InterlockedIncrement(&m->state->ref);
      for (UINT k = 0; k < 8; ++k) if (m->obj[k]) dx_AddRef(m->obj[k]);
      if (l->commands[i].data) { m->data = mem_alloc(m->bytes); if (m->data) memcpy(m->data, l->commands[i].data, m->bytes); else m->bytes = 0; }
      if (l->commands[i].rects) { m->rects = mem_alloc(sizeof *m->rects * m->rect_count); if (m->rects) memcpy(m->rects, l->commands[i].rects, sizeof *m->rects * m->rect_count); else m->rect_count = 0; }
    }
    if (!restore) ctx_ClearState(c);
    return;
  }
  LOCK(c);
  StateCopy *saved = restore ? state_of(c) : NULL;
  Buf *saved_so[4] = {NULL, NULL, NULL, NULL};
  if (restore) for (UINT i = 0; i < 4; ++i) bind(&saved_so[i], c->so_buf[i]);
  commands_run(c, l->commands, l->count);
  if (saved) {
    state_apply(c, saved);
    state_release(saved);
    /* (the stream output targets as they were: their records go on from where they end) */
    const UINT append[4] = {(UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1};
    ctx_SOSetTargets(c, 4, (ID3D11Buffer *const *)saved_so, append);
    for (UINT i = 0; i < 4; ++i) bind(&saved_so[i], NULL);
  } else ctx_ClearState(c);
  UNLOCK(c);
}

/* ---- deferred contexts ---------------------------------------------------------------------------------------------------------------------------------------- */

static void deferred_destroy(DxObj *o) {
  Ctx *c = (Ctx *)o;
  Dev *d = c->dev;
  ctx_release_all(c);
  if (c->rec) {
    commands_free(c->rec->commands, c->rec->count);
    state_release(c->rec->last);
    deferred_maps_free(c->rec);
    mem_free(c->rec);
  }
  dx_Release(OBJ(d));
}
static HRESULT STDMETHODCALLTYPE dev_CreateDeferredContext(Dev *d, UINT flags, ID3D11DeviceContext **out) {
  if (out) *out = NULL;
  if (flags) return E_INVALIDARG;      /* (no flags are defined) */
  if (d->flags & D3D11_CREATE_DEVICE_SINGLETHREADED) return DXGI_ERROR_INVALID_CALL;      /* (a single-threaded device has no deferred contexts) */
  if (!out) return E_INVALIDARG;
  Ctx *c = mem_alloc(sizeof *c);
  if (!c) return E_OUTOFMEMORY;
  memset(c, 0, sizeof *c);
  dx_init(&c->o, &ctx_vtbl, g_ctx_iids, deferred_destroy);
  c->dev = d;
  dx_AddRef(OBJ(d));
  c->deferred = TRUE;
  c->flags = flags;
  ctx_init_faces(c);
  c->sample_mask = 0xFFFFFFFFu;
  for (UINT i = 0; i < 4; ++i) c->blend_factor[i] = 1.0f;
  c->rec = mem_alloc(sizeof *c->rec);
  if (!c->rec) { dx_Release(OBJ(c)); return E_OUTOFMEMORY; }
  memset(c->rec, 0, sizeof *c->rec);
  *out = (ID3D11DeviceContext *)c;
  return S_OK;
}

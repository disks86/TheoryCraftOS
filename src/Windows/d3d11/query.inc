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

/* Part of d3d11.c: queries, predicates, predication and counters.
 *
 * The queue runs each command list before it returns, and End sends what was recorded, so a query is answered at its End: an event is done, a timestamp is the
 * time then (QueryPerformanceCounter, whose frequency TIMESTAMP_DISJOINT gives). What the GPU knows is measured by the kernel for each draw: the samples that
 * passed (occlusion queries and predicates), the pixel shader's invocations, the stream output primitives a stream needed. The kernel has one query open at a
 * time, so a draw is measured for one of them - the samples first, then the stream output, then the pixel shader's invocations - when queries of more than
 * one kind are active. The other pipeline statistics are counted here from the draws and dispatches (docs/Direct3D11.md says which and how). */

/* A draw the kernel measured: what it counted (MEASURE_*), and the queries the count goes to (references). */
#define MEASURE_SAMPLES 0u
#define MEASURE_PIXELS 1u
#define MEASURE_STREAM 2u      /* + the stream */
typedef struct Measure {
  UINT what;
  UINT count;
  Query *queries[8];
} Measure;

/* (the queries of a stream come in pairs, statistics then overflow predicate: SO_STATISTICS_STREAM0 8, SO_OVERFLOW_PREDICATE_STREAM0 9, SO_STATISTICS_STREAM1 10 ...) */
static BOOL query_of_one_stream(D3D11_QUERY q) { return q >= D3D11_QUERY_SO_STATISTICS_STREAM0 && q <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3; }
static BOOL query_is_predicate(D3D11_QUERY q) {
  return q == D3D11_QUERY_OCCLUSION_PREDICATE || q == D3D11_QUERY_SO_OVERFLOW_PREDICATE || (query_of_one_stream(q) && (q - D3D11_QUERY_SO_STATISTICS_STREAM0) % 2 == 1);
}
static UINT query_data_size(D3D11_QUERY q) {
  switch (q) {
    case D3D11_QUERY_EVENT: return sizeof(BOOL);
    case D3D11_QUERY_OCCLUSION: case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
    case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
    case D3D11_QUERY_PIPELINE_STATISTICS: return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D11_QUERY_OCCLUSION_PREDICATE: return sizeof(BOOL);
    case D3D11_QUERY_SO_STATISTICS: case D3D11_QUERY_SO_STATISTICS_STREAM0: case D3D11_QUERY_SO_STATISTICS_STREAM1: case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3: return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE: case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0: case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2: case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: return sizeof(BOOL);
    default: return 0;
  }
}
/* The stream a stream-output query is of; 4 for all of them (SO_STATISTICS, SO_OVERFLOW_PREDICATE); 5 for another query. */
static UINT query_stream(D3D11_QUERY q) {
  if (q == D3D11_QUERY_SO_STATISTICS || q == D3D11_QUERY_SO_OVERFLOW_PREDICATE) return 4;
  if (query_of_one_stream(q)) return (q - D3D11_QUERY_SO_STATISTICS_STREAM0) / 2;
  return 5;
}
/* What the kernel measures for a query (MEASURE_*); ~0u for one it measures nothing for. */
static UINT query_measures(D3D11_QUERY q) {
  if (q == D3D11_QUERY_OCCLUSION || q == D3D11_QUERY_OCCLUSION_PREDICATE) return MEASURE_SAMPLES;
  if (q == D3D11_QUERY_PIPELINE_STATISTICS) return MEASURE_PIXELS;
  const UINT stream = query_stream(q);
  if (stream < 4) return MEASURE_STREAM + stream;
  if (stream == 4) return MEASURE_STREAM;      /* (all streams: stream 0 is measured - the one stream of a program that has one) */
  return ~0u;
}

static void query_destroy(DxObj *o) { child_destroy(o); }
static HRESULT query_make(Dev *d, const D3D11_QUERY_DESC *desc, BOOL predicate, void **out) {
  extern const ID3D11PredicateVtbl pred_vtbl;
  static const GUID *const pred_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Asynchronous, &IID_ID3D11Query, &IID_ID3D11Predicate, NULL};
  if (out) *out = NULL;
  if (!desc || (desc->MiscFlags & ~(UINT)D3D11_QUERY_MISC_PREDICATEHINT) || query_data_size(desc->Query) == 0) return E_INVALIDARG;
  if (predicate && !query_is_predicate(desc->Query)) return E_INVALIDARG;
  if ((desc->MiscFlags & D3D11_QUERY_MISC_PREDICATEHINT) && !query_is_predicate(desc->Query)) return E_INVALIDARG;
  /* (what the kernel measures needs its occlusion queries: an adapter without them has none of these) */
  if (query_measures(desc->Query) != ~0u && !d->measure_heap) return DXGI_ERROR_UNSUPPORTED;
  if (!out) return S_FALSE;
  const BOOL is_predicate = query_is_predicate(desc->Query);
  Query *q = child_new(sizeof *q, d, is_predicate ? (const void *)&pred_vtbl : (const void *)&query_vtbl, is_predicate ? pred_iids : g_query_iids, query_destroy);
  if (!q) return E_OUTOFMEMORY;
  q->desc = *desc;
  return give(q, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateQuery(Dev *d, const D3D11_QUERY_DESC *desc, ID3D11Query **out) { return query_make(d, desc, FALSE, (void **)out); }
static HRESULT STDMETHODCALLTYPE dev_CreatePredicate(Dev *d, const D3D11_QUERY_DESC *desc, ID3D11Predicate **out) { return query_make(d, desc, TRUE, (void **)out); }
static void STDMETHODCALLTYPE query_GetDesc(Query *q, D3D11_QUERY_DESC *desc) { if (desc) *desc = q->desc; }
static UINT STDMETHODCALLTYPE query_GetDataSize(Query *q) { return query_data_size(q->desc.Query); }
static UINT64 query_now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (UINT64)t.QuadPart;
}

/* ---- counters ---------------------------------------------------------------------------------------------------------------------------------------------- */
/* Direct3D 11 has no counters of its own, only those a driver adds after D3D11_COUNTER_DEVICE_DEPENDENT_0; this device adds none. */

static void STDMETHODCALLTYPE dev_CheckCounterInfo(Dev *d, D3D11_COUNTER_INFO *info) {
  (void)d;
  if (!info) return;
  info->LastDeviceDependentCounter = 0;      /* (0: there are none) */
  info->NumSimultaneousCounters = 0;
  info->NumDetectableParallelUnits = 1;
}
static HRESULT STDMETHODCALLTYPE dev_CheckCounter(Dev *d, const D3D11_COUNTER_DESC *desc, D3D11_COUNTER_TYPE *type, UINT *active, LPSTR name, UINT *name_length,
                                                  LPSTR units, UINT *units_length, LPSTR description, UINT *description_length) {
  (void)d; (void)type; (void)active; (void)name; (void)name_length; (void)units; (void)units_length; (void)description; (void)description_length;
  if (!desc) return E_INVALIDARG;
  return desc->Counter >= D3D11_COUNTER_DEVICE_DEPENDENT_0 ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE dev_CreateCounter(Dev *d, const D3D11_COUNTER_DESC *desc, ID3D11Counter **out) {
  (void)d;
  if (out) *out = NULL;
  if (!desc || desc->MiscFlags) return E_INVALIDARG;
  return desc->Counter >= D3D11_COUNTER_DEVICE_DEPENDENT_0 ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}

/* ---- what the kernel measures ---------------------------------------------------------------------------------------------------------------------------- */

/* After the list's last command: the results of its measurements into memory the CPU reads. */
static void measures_resolve(Dev *d) {
  if (!d->measure_count) return;
  ID3D12GraphicsCommandList_ResolveQueryData(d->list, d->measure_heap, D3D12_QUERY_TYPE_OCCLUSION, 0, d->measure_count, d->measure_readback, 0);
}
/* After the list ran: each measurement added to its queries. */
static void measures_collect(Dev *d) {
  if (!d->measure_count) return;
  const UINT64 *values = NULL;
  if (FAILED(ID3D12Resource_Map(d->measure_readback, 0, NULL, (void **)&values))) values = NULL;
  for (UINT i = 0; i < d->measure_count; ++i) {
    Measure *m = &d->measures[i];
    const UINT64 v = values ? values[i] : 0;
    for (UINT k = 0; k < m->count; ++k) {
      Query *q = m->queries[k];
      if (m->what == MEASURE_SAMPLES) q->samples += v;
      else if (m->what == MEASURE_PIXELS) q->stats.PSInvocations += v;
      else q->so_needed[m->what - MEASURE_STREAM] += v;
      dx_Release(OBJ(q));
    }
    m->count = 0;
  }
  if (values) ID3D12Resource_Unmap(d->measure_readback, 0, NULL);
  d->measure_count = 0;
}

/* Before a draw: the kernel is asked to measure it for the active queries (one kind). Returns whether a measurement was begun (measure_end ends it). */
static BOOL measure_begin(Dev *d) {
  if (!d->active_count || !d->measure_heap) return FALSE;
  /* (the samples first, then a stream, then the pixels) */
  BOOL samples = FALSE, pixels = FALSE;
  UINT stream = ~0u;
  for (UINT i = 0; i < d->active_count; ++i) {
    const UINT m = query_measures(d->active[i]->desc.Query);
    if (m == MEASURE_SAMPLES) samples = TRUE;
    else if (m == MEASURE_PIXELS) pixels = TRUE;
    else if (m != ~0u && stream == ~0u) stream = m;
  }
  const UINT what = samples ? MEASURE_SAMPLES : stream != ~0u ? stream : pixels ? MEASURE_PIXELS : ~0u;
  if (what == ~0u) return FALSE;
  if (d->measure_count == MEASURES) flush(d);
  Measure *m = &d->measures[d->measure_count];
  m->what = what;
  m->count = 0;
  for (UINT i = 0; i < d->active_count && m->count < 8; ++i)
    if (query_measures(d->active[i]->desc.Query) == what) { dx_AddRef(OBJ(d->active[i])); m->queries[m->count++] = d->active[i]; }
  /* the kernel's kinds: 0 the samples, 5 the pixel shader's invocations, 8 | stream << 8 the stream output's primitives needed */
  const UINT kind = what == MEASURE_SAMPLES ? 0u : what == MEASURE_PIXELS ? 5u : 8u | ((what - MEASURE_STREAM) << 8);
  TcosD3D12BeginQueryKind(d->list, d->measure_heap, d->measure_count, kind);
  ++d->measure_count;
  return TRUE;
}
static void measure_end(Dev *d, BOOL begun) {
  if (begun) ID3D12GraphicsCommandList_EndQuery(d->list, d->measure_heap, D3D12_QUERY_TYPE_OCCLUSION, d->measure_count - 1);
}

/* ---- what this library counts ------------------------------------------------------------------------------------------------------------------------------- */

/* The primitives `vertices` vertices of a topology make. */
static UINT64 topology_primitives(D3D11_PRIMITIVE_TOPOLOGY t, UINT64 vertices) {
  switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return vertices;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: return vertices / 2;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: return vertices >= 2 ? vertices - 1 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return vertices / 3;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return vertices >= 3 ? vertices - 2 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return vertices / 4;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return vertices >= 4 ? vertices - 3 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return vertices / 6;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return vertices >= 6 ? (vertices - 4) / 2 : 0;
    default:
      if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
        return vertices / (UINT64)(t - D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
      return 0;
  }
}

/* The pipeline statistics of a draw of `vertices` vertices (indices) `instances` times: the input assembler's vertices and primitives, the vertex shader's
 * invocations (once for every vertex read, as the kernel's renderer runs it), the hull shader's (a patch each), the geometry shader's (a primitive each, by its
 * instances), and with neither geometry nor tessellation the primitives the rasterizer is given (C invocations; C primitives the same: clipping is not counted
 * apart). Of a draw with tessellation the domain shader's invocations, and the primitives that come out of the tessellator or a geometry shader, are not known
 * here: they are not added. */
static void query_count_draw(Ctx *c, UINT vertices, UINT instances) {
  Dev *d = c->dev;
  if (!d->active_count) return;
  const UINT64 v = (UINT64)vertices * instances, p = topology_primitives(c->topology, vertices) * instances;
  for (UINT i = 0; i < d->active_count; ++i) {
    Query *q = d->active[i];
    if (q->desc.Query != D3D11_QUERY_PIPELINE_STATISTICS) continue;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS *s = &q->stats;
    s->IAVertices += v;
    s->IAPrimitives += p;
    s->VSInvocations += v;
    if (c->hs) s->HSInvocations += p;
    if (c->gs && !c->hs) s->GSInvocations += p * (c->gs->gs_instances ? c->gs->gs_instances : 1);
    if (!c->gs && !c->hs) { s->CInvocations += p; s->CPrimitives += p; }
  }
}
static void query_count_dispatch(Ctx *c, UINT x, UINT y, UINT z) {
  Dev *d = c->dev;
  const UINT64 threads = (UINT64)x * y * z * c->cs->group[0] * c->cs->group[1] * c->cs->group[2];
  for (UINT i = 0; i < d->active_count; ++i)
    if (d->active[i]->desc.Query == D3D11_QUERY_PIPELINE_STATISTICS) d->active[i]->stats.CSInvocations += threads;
}

/* ---- the context's methods ------------------------------------------------------------------------------------------------------------------------------ */

static Query *query_of(ID3D11Asynchronous *a) {
  extern const ID3D11PredicateVtbl pred_vtbl;
  if (!a || (((DxObj *)a)->lpVtbl != (const void *)&query_vtbl && ((DxObj *)a)->lpVtbl != (const void *)&pred_vtbl)) return NULL;      /* (this library makes no counters) */
  return (Query *)a;
}
static void query_deactivate(Dev *d, Query *q) {
  for (UINT i = 0; i < d->active_count; ++i)
    if (d->active[i] == q) {
      d->active[i] = d->active[--d->active_count];
      q->active = FALSE;
      dx_Release(OBJ(q));
      return;
    }
}
static UINT64 so_filled(Ctx *c, UINT stream);
static UINT64 so_primitive_bytes(Ctx *c, UINT stream);
static void STDMETHODCALLTYPE ctx_Begin(Ctx *c, ID3D11Asynchronous *async) {
  Query *q = query_of(async);
  Dev *d = c->dev;
  if (!q || q->desc.Query == D3D11_QUERY_EVENT || q->desc.Query == D3D11_QUERY_TIMESTAMP) return;      /* (those only End) */
  EnterCriticalSection(&d->lock);
  /* (begun again: counting starts over) */
  if (!q->active && d->active_count == ACTIVE_QUERIES) { LeaveCriticalSection(&d->lock); return; }
  if (!q->active) { dx_AddRef(OBJ(q)); d->active[d->active_count++] = q; q->active = TRUE; }
  q->begun = TRUE;
  q->ended = FALSE;
  q->samples = 0;
  memset(&q->stats, 0, sizeof q->stats);
  memset(q->so_written, 0, sizeof q->so_written);
  memset(q->so_needed, 0, sizeof q->so_needed);
  if (query_stream(q->desc.Query) <= 4)
    for (UINT s = 0; s < 4; ++s) q->so_filled_at_begin[s] = so_filled(c, s);
  q->start = query_now();
  LeaveCriticalSection(&d->lock);
}
static void STDMETHODCALLTYPE ctx_End(Ctx *c, ID3D11Asynchronous *async) {
  Query *q = query_of(async);
  Dev *d = c->dev;
  if (!q) return;
  if (q->desc.Query != D3D11_QUERY_EVENT && q->desc.Query != D3D11_QUERY_TIMESTAMP && !q->active) return;      /* (an End without a Begin) */
  EnterCriticalSection(&d->lock);
  dx_AddRef(OBJ(q));
  query_deactivate(d, q);
  flush(d);      /* (what came before is done, and what the kernel measured for it is in) */
  if (query_stream(q->desc.Query) <= 4)
    for (UINT s = 0; s < 4; ++s) {
      /* (the primitives written: the growth of what the stream's targets hold, a primitive being the stride of a vertex times the vertices of one) */
      const UINT64 filled = so_filled(c, s), grown = filled > q->so_filled_at_begin[s] ? filled - q->so_filled_at_begin[s] : 0;
      const UINT64 per = so_primitive_bytes(c, s);
      q->so_written[s] = per ? grown / per : 0;
      if (q->so_needed[s] < q->so_written[s]) q->so_needed[s] = q->so_written[s];
    }
  q->stop = query_now();
  q->begun = FALSE;
  q->ended = TRUE;
  LeaveCriticalSection(&d->lock);
  dx_Release(OBJ(q));
}
/* The result of a predicate: an occlusion predicate is true when some sample passed, a stream-output one when a stream needed more room than it had. */
static BOOL query_predicate_value(const Query *q) {
  if (q->desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE) return q->samples != 0;
  const UINT stream = query_stream(q->desc.Query);
  for (UINT s = 0; s < 4; ++s)
    if ((stream == 4 || stream == s) && q->so_needed[s] > q->so_written[s]) return TRUE;
  return FALSE;
}
static HRESULT STDMETHODCALLTYPE ctx_GetData(Ctx *c, ID3D11Asynchronous *async, void *data, UINT size, UINT flags) {
  (void)c;
  Query *q = query_of(async);
  if (!q || (flags & ~(UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH)) return E_INVALIDARG;
  const UINT need = query_data_size(q->desc.Query);
  if (data && size != need && size != 0) return E_INVALIDARG;
  if (q->begun && !q->ended) return S_FALSE;      /* (begun and not ended: not done) */
  if (!q->ended) return DXGI_ERROR_INVALID_CALL;  /* (never issued) */
  if (!data || size == 0) return S_OK;
  const UINT stream = query_stream(q->desc.Query);
  switch (q->desc.Query) {
    case D3D11_QUERY_EVENT: *(BOOL *)data = TRUE; break;
    case D3D11_QUERY_OCCLUSION: *(UINT64 *)data = q->samples; break;
    case D3D11_QUERY_TIMESTAMP: *(UINT64 *)data = q->stop; break;
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *t = data;
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      t->Frequency = (UINT64)f.QuadPart;
      t->Disjoint = FALSE;      /* (QueryPerformanceCounter is not interrupted) */
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: memcpy(data, &q->stats, sizeof q->stats); break;
    default:
      if (query_is_predicate(q->desc.Query)) { *(BOOL *)data = query_predicate_value(q); break; }
      if (stream <= 4) {
        D3D11_QUERY_DATA_SO_STATISTICS *so = data;
        so->NumPrimitivesWritten = so->PrimitivesStorageNeeded = 0;
        for (UINT s = 0; s < 4; ++s)
          if (stream == 4 || stream == s) { so->NumPrimitivesWritten += q->so_written[s]; so->PrimitivesStorageNeeded += q->so_needed[s]; }
        break;
      }
      return E_INVALIDARG;
  }
  return S_OK;
}

/* ---- predication -------------------------------------------------------------------------------------------------------------------------------------------- */
/* Draws, dispatches, clears, copies, updates and resolves are left out while the predicate's result is `value`. The predicate's result is known once it has ended
 * (End sends the list); one still being built does not hold anything back. */

static void STDMETHODCALLTYPE ctx_SetPredication(Ctx *c, ID3D11Predicate *predicate, BOOL value) {
  Dev *d = c->dev;
  Query *q = query_of((ID3D11Asynchronous *)predicate);
  EnterCriticalSection(&d->lock);
  bind(&d->predicate, q);
  d->predicate_value = value;
  LeaveCriticalSection(&d->lock);
}
static void STDMETHODCALLTYPE ctx_GetPredication(Ctx *c, ID3D11Predicate **predicate, BOOL *value) {
  Dev *d = c->dev;
  if (predicate) { *predicate = (ID3D11Predicate *)d->predicate; if (d->predicate) dx_AddRef(OBJ(d->predicate)); }
  if (value) *value = d->predicate_value;
}
/* Whether the next predicated command is left out. */
static BOOL predicated_out(const Dev *d) { return d->predicate && d->predicate->ended && query_predicate_value(d->predicate) == d->predicate_value; }

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
 * time then (QueryPerformanceCounter, whose frequency TIMESTAMP_DISJOINT gives). What the GPU counts is measured by the kernel, with Direct3D 12 queries of
 * d3d12.dll: a measurement is open over a run of draws and dispatches while the queries that are active stay the same (Begin, End and the end of the list close
 * it), and it has a query for every kind the active queries need, all open at once - the samples that passed (occlusion queries and predicates), the pipeline
 * statistics, and the stream output statistics (primitives written and needed) of each stream a stream output query is of. What a measurement counted is added to
 * each query it was for. */

/* A measurement: the kinds it has (MEASURE_*: the samples, the pipeline statistics, the stream output statistics of stream n), and the queries its counts go to
 * (references). Its queries are slot `index` of the samples and statistics heaps, and slots 4 * index .. 4 * index + 3 of the stream output one. */
#define MEASURE_SAMPLES 1u
#define MEASURE_STATS 2u
#define MEASURE_STREAM(n) (4u << (n))
typedef struct Measure {
  UINT what;
  UINT count;
  Query *queries[ACTIVE_QUERIES];
} Measure;
/* Where the results of slot i are in the read-back buffer (measures_resolve). */
#define MEASURE_SAMPLES_AT(i) ((UINT64)(i) * 8)
#define MEASURE_STATS_AT(i) ((UINT64)MEASURES * 8 + (UINT64)(i) * sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS))
#define MEASURE_STREAM_AT(i, n) ((UINT64)MEASURES * (8 + sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS)) + ((UINT64)(i) * 4 + (n)) * 16)
#define MEASURE_READBACK_BYTES ((UINT64)MEASURES * (8 + sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS) + 4 * 16))

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
/* What the kernel measures for a query (MEASURE_* bits); 0 for one it measures nothing for. */
static UINT query_measures(D3D11_QUERY q) {
  if (q == D3D11_QUERY_OCCLUSION || q == D3D11_QUERY_OCCLUSION_PREDICATE) return MEASURE_SAMPLES;
  if (q == D3D11_QUERY_PIPELINE_STATISTICS) return MEASURE_STATS;
  const UINT stream = query_stream(q);
  if (stream < 4) return MEASURE_STREAM(stream);
  if (stream == 4) return MEASURE_STREAM(0) | MEASURE_STREAM(1) | MEASURE_STREAM(2) | MEASURE_STREAM(3);      /* (all streams) */
  return 0;
}
/* Whether the device has the heaps a query's measurements need (an adapter without the kernel's queries, or without its statistics, has not). */
static BOOL query_measurable(const Dev *d, D3D11_QUERY q) {
  const UINT m = query_measures(q);
  return (!(m & MEASURE_SAMPLES) || d->measure_heap) && (!(m & MEASURE_STATS) || d->measure_stats_heap) && (!(m & ~(MEASURE_SAMPLES | MEASURE_STATS)) || d->measure_so_heap);
}

static void query_destroy(DxObj *o) { child_destroy(o); }
static HRESULT query_make(Dev *d, const D3D11_QUERY_DESC *desc, BOOL predicate, void **out) {
  static const GUID *const pred_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Asynchronous, &IID_ID3D11Query, &IID_ID3D11Query1, &IID_ID3D11Predicate, NULL};
  if (out) *out = NULL;
  if (!desc || (desc->MiscFlags & ~(UINT)D3D11_QUERY_MISC_PREDICATEHINT) || query_data_size(desc->Query) == 0) return E_INVALIDARG;
  if (predicate && !query_is_predicate(desc->Query)) return E_INVALIDARG;
  if ((desc->MiscFlags & D3D11_QUERY_MISC_PREDICATEHINT) && !query_is_predicate(desc->Query)) return E_INVALIDARG;
  /* (what the kernel measures needs its queries: an adapter without them has none of these) */
  if (!query_measurable(d, desc->Query)) return DXGI_ERROR_UNSUPPORTED;
  if (!out) return S_FALSE;
  const BOOL is_predicate = query_is_predicate(desc->Query);
  /* (a predicate is a query with no methods of its own: the same table, ID3D11Query1's, serves it) */
  Query *q = child_new(sizeof *q, d, &query_vtbl, is_predicate ? pred_iids : g_query_iids, query_destroy);
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

static void measure_close(Dev *d);
/* After the list's last command: the results of its measurements into memory the CPU reads. */
static void measures_resolve(Dev *d) {
  measure_close(d);
  if (!d->measure_count) return;
  if (d->measure_heap) ID3D12GraphicsCommandList_ResolveQueryData(d->list, d->measure_heap, D3D12_QUERY_TYPE_OCCLUSION, 0, d->measure_count, d->measure_readback, MEASURE_SAMPLES_AT(0));
  if (d->measure_stats_heap)
    ID3D12GraphicsCommandList_ResolveQueryData(d->list, d->measure_stats_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, d->measure_count, d->measure_readback, MEASURE_STATS_AT(0));
  if (d->measure_so_heap)
    for (UINT n = 0; n < 4; ++n) {
      /* (each stream's slots are every fourth one: resolved one by one where their measurement has them) */
      for (UINT i = 0; i < d->measure_count; ++i)
        if (d->measures[i].what & MEASURE_STREAM(n))
          ID3D12GraphicsCommandList_ResolveQueryData(d->list, d->measure_so_heap, (D3D12_QUERY_TYPE)(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + n), 4 * i + n, 1, d->measure_readback,
                                                    MEASURE_STREAM_AT(i, n));
    }
}
/* After a list ran: each of its measurements added to its queries (`b`: the list's batch). */
static void measures_collect_batch(struct MeasureBatch *b) {
  if (!b->count) return;
  const BYTE *values = NULL;
  if (FAILED(ID3D12Resource_Map(b->readback, 0, NULL, (void **)&values))) values = NULL;
  for (UINT i = 0; i < b->count; ++i) {
    Measure *m = &b->measures[i];
    UINT64 samples = 0;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
    D3D12_QUERY_DATA_SO_STATISTICS so[4];
    memset(&stats, 0, sizeof stats);
    memset(so, 0, sizeof so);
    if (values) {
      if (m->what & MEASURE_SAMPLES) memcpy(&samples, values + MEASURE_SAMPLES_AT(i), 8);
      if (m->what & MEASURE_STATS) memcpy(&stats, values + MEASURE_STATS_AT(i), sizeof stats);      /* (D3D12_QUERY_DATA_PIPELINE_STATISTICS has the same fields) */
      for (UINT n = 0; n < 4; ++n)
        if (m->what & MEASURE_STREAM(n)) memcpy(&so[n], values + MEASURE_STREAM_AT(i, n), sizeof so[n]);
    }
    for (UINT k = 0; k < m->count; ++k) {
      Query *q = m->queries[k];
      const UINT wants = query_measures(q->desc.Query) & m->what;
      if (wants & MEASURE_SAMPLES) q->samples += samples;
      if (wants & MEASURE_STATS) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS *s = &q->stats;
        s->IAVertices += stats.IAVertices, s->IAPrimitives += stats.IAPrimitives, s->VSInvocations += stats.VSInvocations;
        s->GSInvocations += stats.GSInvocations, s->GSPrimitives += stats.GSPrimitives, s->CInvocations += stats.CInvocations, s->CPrimitives += stats.CPrimitives;
        s->PSInvocations += stats.PSInvocations, s->HSInvocations += stats.HSInvocations, s->DSInvocations += stats.DSInvocations, s->CSInvocations += stats.CSInvocations;
      }
      for (UINT n = 0; n < 4; ++n)
        if (wants & MEASURE_STREAM(n)) { q->so_written[n] += so[n].NumPrimitivesWritten; q->so_needed[n] += so[n].PrimitivesStorageNeeded; }
      if (q->outstanding) --q->outstanding;
      unhold(q);
    }
    m->count = 0;
  }
  if (values) ID3D12Resource_Unmap(b->readback, 0, NULL);
  b->count = 0;
}

/* Reads the oldest list's measurements (waiting for it if the device has not counted them yet) and frees its buffer for another list. */
static void measures_collect_oldest(Dev *d) {
  if (!d->batch_count) return;
  struct MeasureBatch *b = &d->batches[d->batch_first];
  measures_collect_batch(b);
  d->free_sets[d->free_set_count].readback = b->readback;
  d->free_sets[d->free_set_count].measures = b->measures;
  ++d->free_set_count;
  b->readback = NULL;
  b->measures = NULL;
  d->batch_first = (d->batch_first + 1) % MEASURE_BATCHES;
  --d->batch_count;
}
/* After a list was sent: its measurements wait in a batch, read when one of their queries is asked for (GetData), not now - asking the device for its counts right
 * after every list would have it finish its work each time (Direct3D 11's End does not wait either). The next list measures into a free buffer. */
static void measures_send(Dev *d) {
  if (!d->measure_count) return;
  if (d->batch_count == MEASURE_BATCHES) measures_collect_oldest(d);
  ID3D12Resource *readback = NULL;
  Measure *measures = NULL;
  if (d->free_set_count) {
    --d->free_set_count;
    readback = d->free_sets[d->free_set_count].readback;
    measures = d->free_sets[d->free_set_count].measures;
  } else {
    readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, MEASURE_READBACK_BYTES);
    measures = mem_alloc(sizeof(Measure) * MEASURES);
    if (measures) memset(measures, 0, sizeof(Measure) * MEASURES);
  }
  if (!readback || !measures) {      /* (no room for another: this list's are read now) */
    if (readback) ID3D12Resource_Release(readback);
    mem_free(measures);
    struct MeasureBatch now = {d->measure_readback, d->measures, d->measure_count};
    measures_collect_batch(&now);
    d->measure_count = 0;
    return;
  }
  struct MeasureBatch *b = &d->batches[(d->batch_first + d->batch_count) % MEASURE_BATCHES];
  b->readback = d->measure_readback;
  b->measures = d->measures;
  b->count = d->measure_count;
  ++d->batch_count;
  d->measure_readback = readback;
  d->measures = measures;
  d->measure_count = 0;
}
/* Whether a measurement of the list being recorded is for `q`. */
static BOOL measured_in_list(const Dev *d, const Query *q) {
  for (UINT i = 0; i < d->measure_count; ++i)
    for (UINT k = 0; k < d->measures[i].count; ++k)
      if (d->measures[i].queries[k] == q) return TRUE;
  return FALSE;
}
/* The counts of `q` are all in: the list it was measured in sent (unless `no_flush`: then FALSE), the batches up to its last read. */
static BOOL query_settle(Dev *d, Query *q, BOOL no_flush) {
  if (!q->outstanding) return TRUE;
  if (measured_in_list(d, q)) {
    if (no_flush) return FALSE;
    flush(d);
  }
  while (q->outstanding && d->batch_count) measures_collect_oldest(d);
  return TRUE;
}
/* At the device's end: every batch read (its queries let go) and the buffers freed. */
static void measures_release(Dev *d) {
  while (d->batch_count) measures_collect_oldest(d);
  for (UINT i = 0; i < d->free_set_count; ++i) {
    if (d->free_sets[i].readback) ID3D12Resource_Release(d->free_sets[i].readback);
    mem_free(d->free_sets[i].measures);
  }
  d->free_set_count = 0;
}

/* Ends the measurement that is open (Begin and End of a query change what one is for; the list's end ends it too). */
static void measure_close(Dev *d) {
  if (!d->measure_open) return;
  d->measure_open = FALSE;
  const UINT i = d->measure_count - 1;
  const UINT what = d->measures[i].what;
  if (what & MEASURE_SAMPLES) ID3D12GraphicsCommandList_EndQuery(d->list, d->measure_heap, D3D12_QUERY_TYPE_OCCLUSION, i);
  if (what & MEASURE_STATS) ID3D12GraphicsCommandList_EndQuery(d->list, d->measure_stats_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
  for (UINT n = 0; n < 4; ++n)
    if (what & MEASURE_STREAM(n)) ID3D12GraphicsCommandList_EndQuery(d->list, d->measure_so_heap, (D3D12_QUERY_TYPE)(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + n), 4 * i + n);
}
/* Before a draw or a dispatch is prepared: room for a measurement (the list is sent when its measurements are all used; not after the draw's state is recorded on it). */
static void measure_room(Dev *d) {
  if (!d->measure_open && d->active_count && d->measure_count == MEASURES) flush(d);
}
/* Before a draw or a dispatch: a measurement for the active queries is open (one that is open already goes on). */
static void measure_draw(Dev *d) {
  if (d->measure_open || !d->active_count) return;
  UINT what = 0;
  for (UINT i = 0; i < d->active_count; ++i) what |= query_measures(d->active[i]->desc.Query);
  if (!what || d->measure_count == MEASURES) return;      /* (measure_room left room: only a draw that cannot be measured comes here) */
  const UINT i = d->measure_count;
  Measure *m = &d->measures[i];
  m->what = what;
  m->count = 0;
  for (UINT k = 0; k < d->active_count; ++k)
    if (query_measures(d->active[k]->desc.Query)) { hold(d->active[k]); ++d->active[k]->outstanding; m->queries[m->count++] = d->active[k]; }
  if (what & MEASURE_SAMPLES) ID3D12GraphicsCommandList_BeginQuery(d->list, d->measure_heap, D3D12_QUERY_TYPE_OCCLUSION, i);
  if (what & MEASURE_STATS) ID3D12GraphicsCommandList_BeginQuery(d->list, d->measure_stats_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
  for (UINT n = 0; n < 4; ++n)
    if (what & MEASURE_STREAM(n)) ID3D12GraphicsCommandList_BeginQuery(d->list, d->measure_so_heap, (D3D12_QUERY_TYPE)(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + n), 4 * i + n);
  d->measure_count = i + 1;
  d->measure_open = TRUE;
  d->recorded = 1;
}

/* ---- the context's methods ------------------------------------------------------------------------------------------------------------------------------ */

static Query *query_of(ID3D11Asynchronous *a) {
  extern const ID3D11PredicateVtbl pred_vtbl;
  static const void *const kinds[] = {&query_vtbl, &pred_vtbl};
  return (Query *)tcos_own_any(a, kinds, 2);      /* (this library makes no counters; tcos_object.h) */
}
static void query_deactivate(Dev *d, Query *q) {
  for (UINT i = 0; i < d->active_count; ++i)
    if (d->active[i] == q) {
      d->active[i] = d->active[--d->active_count];
      q->active = FALSE;
      unhold(q);
      return;
    }
}
static void STDMETHODCALLTYPE ctx_Begin(Ctx *c, ID3D11Asynchronous *async) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_BEGIN, .obj = {async}}); return; }
  Query *q = query_of(async);
  Dev *d = c->dev;
  if (!q || q->desc.Query == D3D11_QUERY_EVENT || q->desc.Query == D3D11_QUERY_TIMESTAMP) return;      /* (those only End) */
  EnterCriticalSection(&d->lock);
  /* (begun again: counting starts over) */
  if (!q->active && d->active_count == ACTIVE_QUERIES) { LeaveCriticalSection(&d->lock); return; }
  if (query_measures(q->desc.Query)) {
    /* (the draws from here on are measured for it too: a new measurement. Begun again while active, it starts over: what the measurements so far counted for it is not
     * added - they are for the queries they were made for, and it is taken out of them) */
    measure_close(d);
    for (UINT i = 0; i < d->measure_count; ++i) {
      Measure *m = &d->measures[i];
      for (UINT k = 0; k < m->count; ++k)
        if (m->queries[k] == q) { m->queries[k] = m->queries[--m->count]; --q->outstanding; unhold(q); --k; }
    }
    /* (... and so are those of lists sent before whose counts have not been read: they no longer go to it) */
    for (UINT j = 0; j < d->batch_count; ++j) {
      struct MeasureBatch *b = &d->batches[(d->batch_first + j) % MEASURE_BATCHES];
      for (UINT i = 0; i < b->count; ++i) {
        Measure *m = &b->measures[i];
        for (UINT k = 0; k < m->count; ++k)
          if (m->queries[k] == q) { m->queries[k] = m->queries[--m->count]; --q->outstanding; unhold(q); --k; }
      }
    }
  }
  if (!q->active) { hold(q); d->active[d->active_count++] = q; q->active = TRUE; }
  q->begun = TRUE;
  q->ended = FALSE;
  q->samples = 0;
  memset(&q->stats, 0, sizeof q->stats);
  memset(q->so_written, 0, sizeof q->so_written);
  memset(q->so_needed, 0, sizeof q->so_needed);
  q->start = query_now();
  LeaveCriticalSection(&d->lock);
}
static void STDMETHODCALLTYPE ctx_End(Ctx *c, ID3D11Asynchronous *async) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_END, .obj = {async}}); return; }
  Query *q = query_of(async);
  Dev *d = c->dev;
  if (!q) return;
  if (q->desc.Query != D3D11_QUERY_EVENT && q->desc.Query != D3D11_QUERY_TIMESTAMP && !q->active) return;      /* (an End without a Begin) */
  EnterCriticalSection(&d->lock);
  hold(q);
  if (query_measures(q->desc.Query)) measure_close(d);      /* (the draws after this are not its) */
  query_deactivate(d, q);
  /* (an event is done when what came before it is: that goes now. The counts of the others are read when they are asked for - GetData -, as Windows' End does not wait
   * for the device either) */
  if (q->desc.Query == D3D11_QUERY_EVENT) flush(d);
  q->stop = query_now();
  q->begun = FALSE;
  q->ended = TRUE;
  LeaveCriticalSection(&d->lock);
  unhold(q);
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
  if (c->deferred) return DXGI_ERROR_INVALID_CALL;      /* (a deferred context answers no queries) */
  Query *q = query_of(async);
  if (!q || (flags & ~(UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH)) return E_INVALIDARG;
  const UINT need = query_data_size(q->desc.Query);
  if ((data && size != need && size != 0) || (!data && size)) return E_INVALIDARG;
  if (q->begun && !q->ended) return DXGI_ERROR_INVALID_CALL;      /* (begun and not ended: being built, nothing to ask yet - as Windows answers) */
  if (!q->ended) return DXGI_ERROR_INVALID_CALL;  /* (never issued) */
  /* (its counts: the list it was measured in is sent unless DONOTFLUSH - then the result is not there yet, S_FALSE, as Windows answers -, and read back) */
  Dev *d = c->dev;
  EnterCriticalSection(&d->lock);
  const BOOL settled = query_settle(d, q, (flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) != 0);
  LeaveCriticalSection(&d->lock);
  if (!settled) return S_FALSE;
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
  Query *q = query_of((ID3D11Asynchronous *)predicate);
  if (predicate && (!q || !query_is_predicate(q->desc.Query))) q = NULL;
  bind(&c->predicate, q);
  c->predicate_value = value;
}
static void STDMETHODCALLTYPE ctx_GetPredication(Ctx *c, ID3D11Predicate **predicate, BOOL *value) {
  if (predicate) { *predicate = (ID3D11Predicate *)c->predicate; if (c->predicate) child_ref(c->predicate); }
  if (value) *value = c->predicate_value;
}
/* Whether the next predicated command of the immediate context is left out. */
static BOOL predicated_out(Dev *d) {
  Query *p = d->ctx->predicate;
  if (p && p->ended) (void)query_settle(d, p, FALSE);      /* (its result is needed now) */
  return p && p->ended && query_predicate_value(p) == d->ctx->predicate_value;
}

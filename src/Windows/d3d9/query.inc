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

/* Part of d3d9.c: queries (IDirect3DQuery9). What Windows' hardware drivers have:
 *   D3DQUERYTYPE_EVENT              done when the commands before its end have run: the list they are in was sent (the queue runs it before it returns).
 *   D3DQUERYTYPE_OCCLUSION          the samples that passed between its begin and its end: a Direct3D 12 occlusion query. Direct3D 12 begins and ends a query in one
 *                                   command list and this device sends its list often, so a query is begun on the device at the first draw after Issue(BEGIN), ended
 *                                   when the list is sent and begun again at the next draw, and what each list counted is added up (a query with no draw counts 0).
 *   D3DQUERYTYPE_TIMESTAMP          a Direct3D 12 timestamp at its end (nanoseconds); D3DQUERYTYPE_TIMESTAMPFREQ the frequency (10^9); D3DQUERYTYPE_TIMESTAMPDISJOINT
 *                                   whether the frequency changed between its begin and end (it does not).
 *   D3DQUERYTYPE_VCACHE             the vertex cache Direct3D 9 describes (D3DDEVINFO_VCACHE): none, so the cache size is 0 and the strips method is what a mesh should
 *                                   be optimised for, as drivers without one answer.
 * The others (resource manager and vertex statistics of the debug runtime, the timings, the cache utilisation, 9Ex's memory pressure) are not there: CreateQuery
 * answers D3DERR_NOTAVAILABLE, as Windows does where a driver has not them. */

typedef struct Query {
  Child c;
  D3DQUERYTYPE type;
  int state;                     /* 0 nothing asked yet, 1 begun (building), 2 ended (issued) */
  BOOL ever_issued;
  UINT epoch;                    /* the list its end is in (EVENT, OCCLUSION, TIMESTAMP) */
  UINT64 total;                  /* OCCLUSION: what the lists sent since its begin counted; TIMESTAMP: the time */
  UINT slot;                     /* the slot of the device's query heap it has in the list being recorded (QUERY_SLOTS: none) */
  BOOL running;                  /* OCCLUSION: begun on the device in the list being recorded */
  BOOL resolving;                /* its slot's result is read when the list has run */
  struct Query *next;            /* the device's queries that have something in the list (occlusion begun; any resolving) */
  BOOL listed;
} Query;

static UINT query_data_size(D3DQUERYTYPE type) {
  switch (type) {
    case D3DQUERYTYPE_VCACHE: return sizeof(D3DDEVINFO_VCACHE);
    case D3DQUERYTYPE_EVENT: return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP: case D3DQUERYTYPE_TIMESTAMPFREQ: return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    default: return 0;
  }
}
static BOOL query_supported(Dev *d, D3DQUERYTYPE type) {
  switch (type) {
    case D3DQUERYTYPE_VCACHE: case D3DQUERYTYPE_EVENT: return TRUE;
    case D3DQUERYTYPE_OCCLUSION: return d->occlusion_heap != NULL;
    case D3DQUERYTYPE_TIMESTAMP: case D3DQUERYTYPE_TIMESTAMPFREQ: case D3DQUERYTYPE_TIMESTAMPDISJOINT: return d->timestamp_heap != NULL;
    default: return FALSE;
  }
}
/* The heaps and the buffer the results are read from, made with the device: an adapter without timestamps (or occlusion queries) has none of those queries. */
static void queries_start(Dev *d) {
  D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, QUERY_SLOTS, 0};
  if (FAILED(ID3D12Device_CreateQueryHeap(d->d12, &desc, &IID_ID3D12QueryHeap, (void **)&d->occlusion_heap))) d->occlusion_heap = NULL;
  desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  if (FAILED(ID3D12Device_CreateQueryHeap(d->d12, &desc, &IID_ID3D12QueryHeap, (void **)&d->timestamp_heap))) d->timestamp_heap = NULL;
  if (d->occlusion_heap || d->timestamp_heap) d->query_readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)QUERY_SLOTS * 16);
  if (!d->query_readback) {
    if (d->occlusion_heap) ID3D12QueryHeap_Release(d->occlusion_heap);
    if (d->timestamp_heap) ID3D12QueryHeap_Release(d->timestamp_heap);
    d->occlusion_heap = d->timestamp_heap = NULL;
  }
}
static void queries_stop(Dev *d) {
  if (d->occlusion_heap) ID3D12QueryHeap_Release(d->occlusion_heap);
  if (d->timestamp_heap) ID3D12QueryHeap_Release(d->timestamp_heap);
  if (d->query_readback) ID3D12Resource_Release(d->query_readback);
  d->occlusion_heap = d->timestamp_heap = NULL;
  d->query_readback = NULL;
}
static void query_list(Dev *d, Query *q) {
  if (q->listed) return;
  q->next = d->open_queries;
  d->open_queries = q;
  q->listed = TRUE;
}
static void query_unlist(Dev *d, Query *q) {
  for (Query **p = &d->open_queries; *p; p = &(*p)->next)
    if (*p == q) { *p = q->next; break; }
  q->listed = FALSE;
  q->next = NULL;
}
/* A slot of a heap for the list being recorded; QUERY_SLOTS when they are all taken (the caller sends the list, which frees them, and asks again). */
static UINT query_slot_take(BYTE *used) {
  for (UINT i = 0; i < QUERY_SLOTS; ++i) if (!used[i]) { used[i] = 1; return i; }
  return QUERY_SLOTS;
}
static BYTE *query_slots(Dev *d, const Query *q) { return q->type == D3DQUERYTYPE_OCCLUSION ? d->query_slot_used : d->timestamp_slot_used; }
/* Records the end of a query's device query and the copy of its result to the read-back buffer. */
static void query_record_end(Dev *d, Query *q) {
  ID3D12QueryHeap *heap = q->type == D3DQUERYTYPE_OCCLUSION ? d->occlusion_heap : d->timestamp_heap;
  const D3D12_QUERY_TYPE type = q->type == D3DQUERYTYPE_OCCLUSION ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_TIMESTAMP;
  ID3D12GraphicsCommandList_EndQuery(d->list, heap, type, q->slot);
  ID3D12GraphicsCommandList_ResolveQueryData(d->list, heap, type, q->slot, 1, d->query_readback, (UINT64)(q->type == D3DQUERYTYPE_OCCLUSION ? 0 : QUERY_SLOTS) * 8 + (UINT64)q->slot * 8);
  q->resolving = TRUE;
  d->recorded = 1;
}
/* Before a draw: the occlusion queries begun since the list started counting are begun on the device. */
static void queries_before_draw(Dev *d) {
  for (Query *q = d->open_queries; q; q = q->next) {
    if (q->type != D3DQUERYTYPE_OCCLUSION || q->state != 1 || q->running) continue;
    if (q->slot == QUERY_SLOTS) q->slot = query_slot_take(d->query_slot_used);
    if (q->slot == QUERY_SLOTS) continue;      /* (no slot left in this list: what it counts is lost until the list goes; queries_slot_wait avoids it) */
    ID3D12GraphicsCommandList_BeginQuery(d->list, d->occlusion_heap, D3D12_QUERY_TYPE_OCCLUSION, q->slot);
    q->running = TRUE;
    d->recorded = 1;
  }
}
/* Whether a draw would find no slot for an occlusion query that is to begin: the list is sent first then. */
static BOOL queries_need_slots(Dev *d) {
  UINT wanted = 0, free_slots = 0;
  for (Query *q = d->open_queries; q; q = q->next) if (q->type == D3DQUERYTYPE_OCCLUSION && q->state == 1 && !q->running && q->slot == QUERY_SLOTS) ++wanted;
  for (UINT i = 0; i < QUERY_SLOTS; ++i) if (!d->query_slot_used[i]) ++free_slots;
  return wanted > free_slots;
}
/* As the list is sent: the occlusion queries counting in it are ended there (they begin again at the next draw). */
static void queries_before_submit(Dev *d) {
  for (Query *q = d->open_queries; q; q = q->next)
    if (q->type == D3DQUERYTYPE_OCCLUSION && q->running) {
      query_record_end(d, q);
      q->running = FALSE;
    }
}
/* When the list has run: the results it resolved are read, the slots are free again. */
static void queries_after_submit(Dev *d) {
  const UINT64 *results = NULL;
  BOOL mapped = FALSE;
  for (Query *q = d->open_queries; q;) {
    Query *next = q->next;
    if (q->resolving) {
      if (!mapped) {
        const D3D12_RANGE all = {0, (SIZE_T)QUERY_SLOTS * 16};
        mapped = SUCCEEDED(ID3D12Resource_Map(d->query_readback, 0, &all, (void **)&results)) && results;
      }
      const UINT64 value = mapped ? results[(q->type == D3DQUERYTYPE_OCCLUSION ? 0 : QUERY_SLOTS) + q->slot] : 0;
      if (q->type == D3DQUERYTYPE_OCCLUSION) q->total += value;
      else q->total = value;
      q->resolving = FALSE;
      query_slots(d, q)[q->slot] = 0;
      q->slot = QUERY_SLOTS;
    }
    if (q->state != 1 || q->type != D3DQUERYTYPE_OCCLUSION) query_unlist(d, q);
    q = next;
  }
  if (mapped) ID3D12Resource_Unmap(d->query_readback, 0, NULL);
}

static void query_destroy(DxObj *o) {
  Query *q = (Query *)o;
  Dev *d = q->c.dev;
  EnterCriticalSection(&d->lock);
  if (q->resolving || q->running) flush(d);      /* (the list writes its slot) */
  if (q->listed) query_unlist(d, q);
  if (q->slot != QUERY_SLOTS) query_slots(d, q)[q->slot] = 0;
  LeaveCriticalSection(&d->lock);
}
static HRESULT STDMETHODCALLTYPE dev_CreateQuery(Dev *d, D3DQUERYTYPE type, IDirect3DQuery9 **out) {
  if (!query_supported(d, type)) return D3DERR_NOTAVAILABLE;
  if (!out) return D3D_OK;      /* (NULL asks whether the type is there) */
  Query *q = child_new(sizeof *q, d, &query_vtbl, g_query_iids, query_destroy);
  if (!q) { *out = NULL; return E_OUTOFMEMORY; }
  q->type = type;
  q->slot = QUERY_SLOTS;
  *out = (IDirect3DQuery9 *)q;
  return D3D_OK;
}
static D3DQUERYTYPE STDMETHODCALLTYPE query_GetType(Query *q) { return q->type; }
static DWORD STDMETHODCALLTYPE query_GetDataSize(Query *q) { return query_data_size(q->type); }
/* Issue: D3DISSUE_BEGIN (the queries that have one: occlusion, timestamp disjoint) and D3DISSUE_END. A begin again restarts the count; an end with no begin is an
 * end of nothing (0 samples). */
static HRESULT STDMETHODCALLTYPE query_Issue(Query *q, DWORD flags) {
  Dev *d = q->c.dev;
  const BOOL has_begin = q->type == D3DQUERYTYPE_OCCLUSION || q->type == D3DQUERYTYPE_TIMESTAMPDISJOINT;
  if (flags != D3DISSUE_BEGIN && flags != D3DISSUE_END) return D3DERR_INVALIDCALL;
  if (flags == D3DISSUE_BEGIN && !has_begin) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = D3D_OK;
  if (q->type == D3DQUERYTYPE_OCCLUSION) {
    if (flags == D3DISSUE_BEGIN) {
      if (q->running || q->resolving) flush(d);      /* (what it counted before goes; the new count starts in a list of its own) */
      q->total = 0;
      q->state = 1;
      query_list(d, q);
    } else {
      if (q->state != 1) { q->total = 0; q->state = 1; }
      if (q->running) { query_record_end(d, q); q->running = FALSE; }
      q->state = 2;
      q->epoch = d->epoch;
      query_list(d, q);
    }
  } else if (q->type == D3DQUERYTYPE_TIMESTAMP) {
    if (q->resolving) flush(d);
    if (q->slot == QUERY_SLOTS) q->slot = query_slot_take(d->timestamp_slot_used);
    if (q->slot == QUERY_SLOTS) { flush(d); q->slot = query_slot_take(d->timestamp_slot_used); }
    if (q->slot == QUERY_SLOTS) hr = E_OUTOFMEMORY;
    else {
      query_record_end(d, q);
      q->state = 2;
      q->epoch = d->epoch;
      query_list(d, q);
    }
  } else {
    q->state = flags == D3DISSUE_BEGIN ? 1 : 2;
    q->epoch = d->epoch;
  }
  if (SUCCEEDED(hr)) q->ever_issued = TRUE;
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* GetData: S_OK with the result when it is there, S_FALSE when not yet (D3DGETDATA_FLUSH sends what was recorded, so that it will be). At most `size` bytes of
 * it are written; none with a size of 0. A query that was never issued answers S_OK with what Windows' drivers write for it. */
static HRESULT STDMETHODCALLTYPE query_GetData(Query *q, void *data, DWORD size, DWORD flags) {
  Dev *d = q->c.dev;
  if (flags & ~(DWORD)D3DGETDATA_FLUSH) return D3DERR_INVALIDCALL;
  if (size && !data) return D3DERR_INVALIDCALL;
  const UINT need = query_data_size(q->type);
  union { BOOL b; DWORD dw; UINT64 u; D3DDEVINFO_VCACHE vcache; BYTE bytes[16]; } out;
  memset(&out, 0, sizeof out);
  HRESULT hr = S_OK;
  EnterCriticalSection(&d->lock);
  switch (q->type) {
    case D3DQUERYTYPE_VCACHE:
      out.vcache.Pattern = MAKEFOURCC('C', 'A', 'C', 'H');
      out.vcache.OptMethod = 0;      /* (strips: there is no vertex cache to fit meshes to) */
      out.vcache.CacheSize = 0;
      out.vcache.MagicNumber = 0;
      break;
    case D3DQUERYTYPE_TIMESTAMPFREQ: {
      UINT64 frequency = 0;
      if (FAILED(ID3D12CommandQueue_GetTimestampFrequency(d->queue, &frequency))) frequency = 1000000000ull;
      out.u = frequency;
      break;
    }
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
      if (q->state == 1) hr = S_FALSE;      /* (building: no answer until it is ended) */
      out.b = FALSE;
      break;
    default:
      if (!q->ever_issued) {
        memset(&out, 0xDD, sizeof out);      /* (as Windows' drivers fill a query that was never issued) */
        if (q->type == D3DQUERYTYPE_EVENT) out.b = TRUE;
        break;
      }
      if (q->state == 1) {
        /* (begun, not ended: there is nothing to give until the end has run) */
        if ((flags & D3DGETDATA_FLUSH) && d->recorded) flush(d);
        hr = S_FALSE;
        break;
      }
      if ((q->epoch == d->epoch && d->recorded) || q->resolving) {
        if (!(flags & D3DGETDATA_FLUSH)) { hr = S_FALSE; break; }
        flush(d);
      }
      if (q->type == D3DQUERYTYPE_EVENT) out.b = TRUE;
      else if (q->type == D3DQUERYTYPE_OCCLUSION) out.dw = q->total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)q->total;
      else out.u = q->total;
      break;
  }
  LeaveCriticalSection(&d->lock);
  if (hr == S_OK && size) {
    /* (no more than the result: what is after it in the program's memory is left as it is, but for an occlusion count's high word, which drivers write too) */
    UINT n = size < need ? size : need;
    if (q->type == D3DQUERYTYPE_OCCLUSION && size >= 8 && q->ever_issued) n = 8;
    memcpy(data, &out, n);
  }
  return hr;
}

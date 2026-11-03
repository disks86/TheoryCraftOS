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

/* ntdll.dll: generic tables with balanced links, dynamic hash tables, memory zones and block lookasides, correlation vectors, the remaining heap calls, security identifiers,
 * paths and the registry query table. Included by ntdll.c after rtlx1.inc. */

#define STATUS_INVALID_SID_ ((NTSTATUS)0xC0000078)
#define STATUS_NOT_SUPPORTED_ ((NTSTATUS)0xC00000BB)

/* ---- generic tables (AVL): the elements are kept in order in a doubly linked list through LeftChild (the one before) and RightChild (the one after) ---- */

typedef struct AVL_LINKS_ { struct AVL_LINKS_ *parent, *left, *right; CHAR balance; UCHAR reserved[3]; } AVL_LINKS_;
typedef enum { AVL_LESS = 0, AVL_GREATER = 1, AVL_EQUAL = 2 } AVL_COMPARE_;
struct AVL_TABLE_;
typedef AVL_COMPARE_ (NTAPI *AVL_COMPARE_FN_)(struct AVL_TABLE_ *, PVOID, PVOID);
typedef PVOID (NTAPI *AVL_ALLOCATE_FN_)(struct AVL_TABLE_ *, ULONG);
typedef void (NTAPI *AVL_FREE_FN_)(struct AVL_TABLE_ *, PVOID);
typedef struct AVL_TABLE_ {
  AVL_LINKS_ root;
  ULONG ordered_element;
  ULONG which;
  ULONG count;
  ULONG depth;
  AVL_LINKS_ *restart;
  ULONG delete_count;
  AVL_COMPARE_FN_ compare;
  AVL_ALLOCATE_FN_ allocate;
  AVL_FREE_FN_ release;
  PVOID context;
} AVL_TABLE_;
#define AVL_USER(links) ((PVOID)((BYTE *)(links) + sizeof(AVL_LINKS_)))
#define AVL_LINKS_OF(user) ((AVL_LINKS_ *)((BYTE *)(user) - sizeof(AVL_LINKS_)))
EXPORT void NTAPI RtlInitializeGenericTableAvl(AVL_TABLE_ *t, AVL_COMPARE_FN_ compare, AVL_ALLOCATE_FN_ allocate, AVL_FREE_FN_ release, PVOID context) {
  memset(t, 0, sizeof(*t));
  t->root.parent = &t->root;
  t->compare = compare; t->allocate = allocate; t->release = release; t->context = context;
}
EXPORT ULONG NTAPI RtlNumberGenericTableElementsAvl(AVL_TABLE_ *t) { return t->count; }
EXPORT BOOLEAN NTAPI RtlIsGenericTableEmptyAvl(AVL_TABLE_ *t) { return t->count == 0; }
/* The element equal to the buffer (or the last one before it, or none): `before` gets the one that comes before the place where it would go. */
static AVL_LINKS_ *avl_search(AVL_TABLE_ *t, PVOID buffer, AVL_LINKS_ **before, int *order) {
  AVL_LINKS_ *previous = NULL;
  for (AVL_LINKS_ *e = t->root.right; e; previous = e, e = e->right) {
    const AVL_COMPARE_ c = t->compare(t, buffer, AVL_USER(e));
    if (c == AVL_EQUAL) { *before = e->left; *order = 2; return e; }
    if (c == AVL_LESS) { *before = previous; *order = 0; return NULL; }
  }
  *before = previous;
  *order = 1;
  return NULL;
}
EXPORT PVOID NTAPI RtlInsertElementGenericTableAvl(AVL_TABLE_ *t, PVOID buffer, ULONG size, PBOOLEAN newly) {
  AVL_LINKS_ *before;
  int order;
  AVL_LINKS_ *found = avl_search(t, buffer, &before, &order);
  if (found) { if (newly) *newly = FALSE; return AVL_USER(found); }
  AVL_LINKS_ *e = t->allocate(t, size + (ULONG)sizeof(AVL_LINKS_));
  if (!e) { if (newly) *newly = FALSE; return NULL; }
  memset(e, 0, sizeof(*e));
  memcpy(AVL_USER(e), buffer, size);
  e->left = before;
  e->right = before ? before->right : t->root.right;
  if (e->right) e->right->left = e;
  if (before) before->right = e; else t->root.right = e;
  ++t->count;
  t->restart = e;
  if (newly) *newly = TRUE;
  return AVL_USER(e);
}
EXPORT PVOID NTAPI RtlInsertElementGenericTableFullAvl(AVL_TABLE_ *t, PVOID buffer, ULONG size, PBOOLEAN newly, PVOID node, ULONG result) {
  (void)node; (void)result;
  return RtlInsertElementGenericTableAvl(t, buffer, size, newly);
}
EXPORT PVOID NTAPI RtlLookupElementGenericTableAvl(AVL_TABLE_ *t, PVOID buffer) {
  AVL_LINKS_ *before;
  int order;
  AVL_LINKS_ *found = avl_search(t, buffer, &before, &order);
  if (found) t->restart = found;
  return found ? AVL_USER(found) : NULL;
}
EXPORT PVOID NTAPI RtlLookupElementGenericTableFullAvl(AVL_TABLE_ *t, PVOID buffer, PVOID *node, ULONG *result) {
  AVL_LINKS_ *before;
  int order;
  AVL_LINKS_ *found = avl_search(t, buffer, &before, &order);
  *node = found ? (PVOID)found : (PVOID)before;
  *result = t->count == 0 ? 0 : (found ? 1 : (order == 0 ? 2 : 3));      /* TableEmptyTree, TableFoundNode, TableInsertAsLeft, TableInsertAsRight */
  return found ? AVL_USER(found) : NULL;
}
static void avl_remove(AVL_TABLE_ *t, AVL_LINKS_ *e) {
  if (e->left) e->left->right = e->right; else t->root.right = e->right;
  if (e->right) e->right->left = e->left;
  if (t->restart == e) t->restart = e->right ? e->right : e->left;
  --t->count;
  ++t->delete_count;
  t->release(t, e);
}
EXPORT BOOLEAN NTAPI RtlDeleteElementGenericTableAvl(AVL_TABLE_ *t, PVOID buffer) {
  AVL_LINKS_ *before;
  int order;
  AVL_LINKS_ *found = avl_search(t, buffer, &before, &order);
  if (!found) return FALSE;
  avl_remove(t, found);
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlDeleteElementGenericTableAvlEx(AVL_TABLE_ *t, PVOID node) { avl_remove(t, node); return TRUE; }
EXPORT PVOID NTAPI RtlGetElementGenericTableAvl(AVL_TABLE_ *t, ULONG index) {
  ULONG i = 0;
  for (AVL_LINKS_ *e = t->root.right; e; e = e->right, ++i) if (i == index) { t->restart = e; return AVL_USER(e); }
  return NULL;
}
EXPORT PVOID NTAPI RtlEnumerateGenericTableAvl(AVL_TABLE_ *t, BOOLEAN restart) {
  AVL_LINKS_ *next = restart || !t->restart ? t->root.right : t->restart->right;
  if (!restart && t->restart == NULL) next = t->root.right;
  if (!next) return NULL;
  t->restart = next;
  return AVL_USER(next);
}
EXPORT PVOID NTAPI RtlEnumerateGenericTableWithoutSplayingAvl(AVL_TABLE_ *t, PVOID *restart_key) {
  AVL_LINKS_ *next = *restart_key ? ((AVL_LINKS_ *)*restart_key)->right : t->root.right;
  if (!next) return NULL;
  *restart_key = next;
  return AVL_USER(next);
}
EXPORT PVOID NTAPI RtlLookupFirstMatchingElementGenericTableAvl(AVL_TABLE_ *t, PVOID buffer, PVOID *restart_key) {
  AVL_LINKS_ *before;
  int order;
  AVL_LINKS_ *found = avl_search(t, buffer, &before, &order);
  if (!found) return NULL;
  *restart_key = found;
  return AVL_USER(found);
}
typedef AVL_COMPARE_ (NTAPI *AVL_MATCH_FN_)(struct AVL_TABLE_ *, PVOID, PVOID);
EXPORT PVOID NTAPI RtlEnumerateGenericTableLikeADirectory(AVL_TABLE_ *t, AVL_MATCH_FN_ match, PVOID match_context, ULONG next_flag, PULONG restart_key, PULONG delete_count, PVOID buffer) {
  /* restart_key counts the elements passed (the position in the directory); the table is searched in order from there for one that fits */
  if (*delete_count != t->delete_count) { *delete_count = t->delete_count; }
  ULONG index = 0, position = *restart_key;
  if (next_flag == 0 && position > 0) --position;
  for (AVL_LINKS_ *e = t->root.right; e; e = e->right, ++index) {
    if (index < position + (next_flag ? 0 : 0)) continue;
    if (index == position && next_flag && *restart_key != 0 && index < *restart_key) continue;
    if (match && match(t, buffer, AVL_USER(e)) != AVL_EQUAL) { if (match) continue; }
    if (t->compare(t, buffer, AVL_USER(e)) == AVL_GREATER && !match) continue;
    *restart_key = index + 1;
    return AVL_USER(e);
  }
  return NULL;
}

/* ---- dynamic hash tables: a fixed array of chains ---- */

typedef struct HASH_ENTRY_ { LIST_ENTRY linkage; ULONG_PTR signature; } HASH_ENTRY_;
typedef struct HASH_TABLE_ { ULONG flags, shift, table_size, pivot, divisor_mask, entries, non_empty, enumerators; LIST_ENTRY *directory; } HASH_TABLE_;
typedef struct HASH_CONTEXT_ { LIST_ENTRY *chain_head; LIST_ENTRY *previous; ULONG_PTR signature; } HASH_CONTEXT_;
typedef struct HASH_ENUM_ { LIST_ENTRY *current; ULONG_PTR pad[2]; LIST_ENTRY *chain_head; ULONG bucket; } HASH_ENUM_;
#define HASH_BUCKETS 128
EXPORT BOOLEAN NTAPI RtlCreateHashTable(HASH_TABLE_ **out, ULONG shift, ULONG flags) {
  (void)flags;
  HASH_TABLE_ *t = *out;
  if (!t) { t = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(*t)); if (!t) return FALSE; }
  t->directory = RtlAllocateHeap(process_heap(), 0, sizeof(LIST_ENTRY) * HASH_BUCKETS);
  if (!t->directory) { if (!*out) RtlFreeHeap(process_heap(), 0, t); return FALSE; }
  for (ULONG i = 0; i < HASH_BUCKETS; ++i) t->directory[i].Flink = t->directory[i].Blink = &t->directory[i];
  t->shift = shift ? shift : 1;
  t->table_size = HASH_BUCKETS;
  t->divisor_mask = HASH_BUCKETS - 1;
  t->flags = flags;
  t->entries = t->non_empty = t->enumerators = 0;
  *out = t;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlCreateHashTableEx(HASH_TABLE_ **out, ULONG shift, ULONG flags, ULONG initial) { (void)initial; return RtlCreateHashTable(out, shift, flags); }
EXPORT void NTAPI RtlDeleteHashTable(HASH_TABLE_ *t) { RtlFreeHeap(process_heap(), 0, t->directory); t->directory = NULL; RtlFreeHeap(process_heap(), 0, t); }
EXPORT BOOLEAN NTAPI RtlInsertEntryHashTable(HASH_TABLE_ *t, HASH_ENTRY_ *entry, ULONG_PTR signature, HASH_CONTEXT_ *context) {
  (void)context;
  LIST_ENTRY *head = &t->directory[signature % t->table_size];
  entry->signature = signature;
  if (head->Flink == head) ++t->non_empty;
  entry->linkage.Flink = head->Flink; entry->linkage.Blink = head;
  head->Flink->Blink = &entry->linkage; head->Flink = &entry->linkage;
  ++t->entries;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlRemoveEntryHashTable(HASH_TABLE_ *t, HASH_ENTRY_ *entry, HASH_CONTEXT_ *context) {
  (void)context;
  LIST_ENTRY *head = &t->directory[entry->signature % t->table_size];
  entry->linkage.Blink->Flink = entry->linkage.Flink;
  entry->linkage.Flink->Blink = entry->linkage.Blink;
  if (head->Flink == head) --t->non_empty;
  --t->entries;
  return TRUE;
}
EXPORT HASH_ENTRY_ *NTAPI RtlLookupEntryHashTable(HASH_TABLE_ *t, ULONG_PTR signature, HASH_CONTEXT_ *context) {
  LIST_ENTRY *head = &t->directory[signature % t->table_size];
  if (context) { context->chain_head = head; context->previous = head; context->signature = signature; }
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) if (((HASH_ENTRY_ *)e)->signature == signature) { if (context) context->previous = e->Blink; return (HASH_ENTRY_ *)e; }
  return NULL;
}
EXPORT HASH_ENTRY_ *NTAPI RtlGetNextEntryHashTable(HASH_TABLE_ *t, HASH_ENTRY_ *previous, HASH_CONTEXT_ *context) {
  (void)t;
  LIST_ENTRY *head = context->chain_head;
  for (LIST_ENTRY *e = previous->linkage.Flink; e != head; e = e->Flink) if (((HASH_ENTRY_ *)e)->signature == context->signature) { context->previous = e->Blink; return (HASH_ENTRY_ *)e; }
  return NULL;
}
EXPORT BOOLEAN NTAPI RtlInitEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { e->current = NULL; e->chain_head = NULL; e->bucket = 0; ++t->enumerators; return TRUE; }
EXPORT BOOLEAN NTAPI RtlInitStrongEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { return RtlInitEnumerationHashTable(t, e); }
EXPORT BOOLEAN NTAPI RtlInitWeakEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { return RtlInitEnumerationHashTable(t, e); }
EXPORT HASH_ENTRY_ *NTAPI RtlEnumerateEntryHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) {
  for (;;) {
    if (e->current) {      /* on to the next in the chain */
      LIST_ENTRY *next = e->current->Flink;
      if (next != e->chain_head) { e->current = next; return (HASH_ENTRY_ *)next; }
      e->current = NULL;
      ++e->bucket;
    }
    if (!e->chain_head && e->bucket == 0) { e->chain_head = &t->directory[0]; }
    while (e->bucket < t->table_size) {
      e->chain_head = &t->directory[e->bucket];
      if (e->chain_head->Flink != e->chain_head) { e->current = e->chain_head->Flink; return (HASH_ENTRY_ *)e->current; }
      ++e->bucket;
    }
    return NULL;
  }
}
EXPORT HASH_ENTRY_ *NTAPI RtlStronglyEnumerateEntryHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { return RtlEnumerateEntryHashTable(t, e); }
EXPORT HASH_ENTRY_ *NTAPI RtlWeaklyEnumerateEntryHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { return RtlEnumerateEntryHashTable(t, e); }
EXPORT void NTAPI RtlEndEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { (void)e; if (t->enumerators) --t->enumerators; }
EXPORT void NTAPI RtlEndStrongEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { RtlEndEnumerationHashTable(t, e); }
EXPORT void NTAPI RtlEndWeakEnumerationHashTable(HASH_TABLE_ *t, HASH_ENUM_ *e) { RtlEndEnumerationHashTable(t, e); }
EXPORT BOOLEAN NTAPI RtlExpandHashTable(HASH_TABLE_ *t) { (void)t; return TRUE; }
EXPORT BOOLEAN NTAPI RtlContractHashTable(HASH_TABLE_ *t) { (void)t; return TRUE; }

/* ---- memory zones and block lookasides ---- */

typedef struct ZONE_SEGMENT_ { struct ZONE_SEGMENT_ *next; SIZE_T size; BYTE *free; BYTE *limit; } ZONE_SEGMENT_;
typedef struct ZONE_ { ZONE_SEGMENT_ segment; SRWLOCK lock; ULONG lock_count; ZONE_SEGMENT_ *first; } ZONE_;
static PVOID zone_memory(SIZE_T size) {
  PVOID base = NULL;
  SIZE_T region = size;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &region, 0x3000, 0x04))) return NULL;
  return base;
}
EXPORT NTSTATUS NTAPI RtlCreateMemoryZone(ZONE_ **out, SIZE_T size, ULONG flags) {
  (void)flags;
  if (size < sizeof(ZONE_SEGMENT_) + 8) return STATUS_INVALID_PARAMETER;
  BYTE *block = zone_memory(size);
  if (!block) return STATUS_NO_MEMORY;
  ZONE_ *z = (ZONE_ *)block;
  memset(z, 0, sizeof(*z));
  z->segment.size = size;
  z->segment.free = block + ((sizeof(ZONE_) + 15) & ~(SIZE_T)15);
  z->segment.limit = block + size;
  z->first = &z->segment;
  *out = z;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAllocateMemoryZone(ZONE_ *z, SIZE_T size, PVOID *out) {
  size = (size + 15) & ~(SIZE_T)15;
  for (ZONE_SEGMENT_ *s = z->first; s; s = s->next) {
    if (s->free && (SIZE_T)(s->limit - s->free) >= size) { *out = s->free; s->free += size; return STATUS_SUCCESS; }
  }
  return STATUS_NO_MEMORY;
}
EXPORT NTSTATUS NTAPI RtlExtendMemoryZone(ZONE_ *z, SIZE_T increment) {
  BYTE *block = zone_memory(increment + sizeof(ZONE_SEGMENT_));
  if (!block) return STATUS_NO_MEMORY;
  ZONE_SEGMENT_ *s = (ZONE_SEGMENT_ *)block;
  s->size = increment + sizeof(ZONE_SEGMENT_);
  s->free = block + ((sizeof(ZONE_SEGMENT_) + 15) & ~(SIZE_T)15);
  s->limit = block + s->size;
  s->next = z->first;
  z->first = s;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlResetMemoryZone(ZONE_ *z) {
  for (ZONE_SEGMENT_ *s = z->first; s; s = s->next) s->free = (BYTE *)s + (s == &z->segment ? ((sizeof(ZONE_) + 15) & ~(SIZE_T)15) : ((sizeof(ZONE_SEGMENT_) + 15) & ~(SIZE_T)15));
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDestroyMemoryZone(ZONE_ *z) {
  ZONE_SEGMENT_ *s = z->first;
  while (s && s != &z->segment) { ZONE_SEGMENT_ *next = s->next; PVOID base = s; SIZE_T size = 0; NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, 0x8000); s = next; }
  PVOID base = z;
  SIZE_T size = 0;
  return NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, 0x8000);
}
EXPORT NTSTATUS NTAPI RtlLockMemoryZone(ZONE_ *z) { RtlAcquireSRWLockExclusive(&z->lock); ++z->lock_count; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlUnlockMemoryZone(ZONE_ *z) { if (z->lock_count) { --z->lock_count; RtlReleaseSRWLockExclusive(&z->lock); } return STATUS_SUCCESS; }

typedef struct LOOKASIDE_ { SIZE_T minimum, maximum; LONG locked; } LOOKASIDE_;
EXPORT NTSTATUS NTAPI RtlCreateMemoryBlockLookaside(PVOID *out, ULONG flags, ULONG initial, ULONG minimum, ULONG maximum) {
  (void)flags; (void)initial;
  LOOKASIDE_ *l = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(*l));
  if (!l) return STATUS_NO_MEMORY;
  l->minimum = minimum; l->maximum = maximum;
  *out = l;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDestroyMemoryBlockLookaside(PVOID lookaside) { RtlFreeHeap(process_heap(), 0, lookaside); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlAllocateMemoryBlockLookaside(PVOID lookaside, ULONG size, PVOID *block) {
  LOOKASIDE_ *l = lookaside;
  if (size < l->minimum || size > l->maximum) return STATUS_INVALID_PARAMETER;
  *block = RtlAllocateHeap(process_heap(), 0, size);
  return *block ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
EXPORT NTSTATUS NTAPI RtlFreeMemoryBlockLookaside(PVOID lookaside, PVOID block) { (void)lookaside; RtlFreeHeap(process_heap(), 0, block); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlExtendMemoryBlockLookaside(PVOID lookaside, ULONG increment) { (void)lookaside; (void)increment; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlResetMemoryBlockLookaside(PVOID lookaside) { (void)lookaside; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlLockMemoryBlockLookaside(PVOID lookaside) { ((LOOKASIDE_ *)lookaside)->locked = 1; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlUnlockMemoryBlockLookaside(PVOID lookaside) { ((LOOKASIDE_ *)lookaside)->locked = 0; return STATUS_SUCCESS; }

/* ---- correlation vectors: "base.n.m..." (V1: a 16 character base and 63 characters at most; V2: 22 and 127) ---- */

typedef struct CV_ { CHAR version; CHAR vector[127]; } CV_;
static const char cv_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static ULONG cv_length(const CV_ *v) { ULONG n = 0; while (n < 127 && v->vector[n]) ++n; return n; }
static ULONG cv_max(const CV_ *v) { return v->version == 0 ? 63 : 127; }
EXPORT NTSTATUS NTAPI RtlInitializeCorrelationVector(CV_ *v, int version, const BYTE *guid) {
  if (version != 0 && version != 1) return STATUS_INVALID_PARAMETER;
  BYTE bytes[16];
  if (guid) memcpy(bytes, guid, 16);
  else {
    ETW_GUID_ made;
    EtwEventActivityIdControl(3, &made);
    memcpy(bytes, &made, 16);
  }
  const ULONG base = version == 0 ? 16 : 22, source = version == 0 ? 12 : 16;
  memset(v, 0, version == 0 ? 64 : 128);
  v->version = (CHAR)version;
  ULONG out = 0;
  for (ULONG i = 0; i < source && out < base; i += 3) {
    const ULONG chunk = (ULONG)bytes[i] << 16 | (i + 1 < source ? (ULONG)bytes[i + 1] << 8 : 0) | (i + 2 < source ? bytes[i + 2] : 0);
    for (int k = 0; k < 4 && out < base; ++k) v->vector[out++] = cv_alphabet[(chunk >> (18 - 6 * k)) & 63];
  }
  v->vector[out++] = '.';
  v->vector[out++] = '0';
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlValidateCorrelationVector(const CV_ *v) {
  if (v->version != 0 && v->version != 1) return STATUS_INVALID_PARAMETER;
  const ULONG base = v->version == 0 ? 16 : 22, n = cv_length(v);
  if (n < base + 2 || n > cv_max(v) || v->vector[base] != '.') return STATUS_INVALID_PARAMETER;
  for (ULONG i = 0; i < base; ++i) { const char c = v->vector[i]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return STATUS_INVALID_PARAMETER; }
  for (ULONG i = base + 1; i < n; ++i) { const char c = v->vector[i]; if (!((c >= '0' && c <= '9') || (c == '.' && v->vector[i - 1] != '.' && i + 1 < n))) return STATUS_INVALID_PARAMETER; }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlExtendCorrelationVector(CV_ *v) {
  NTSTATUS status = RtlValidateCorrelationVector(v);
  if (!NT_SUCCESS(status)) return status;
  const ULONG n = cv_length(v);
  if (n + 2 > cv_max(v)) return ((NTSTATUS)0xC0000001);
  v->vector[n] = '.'; v->vector[n + 1] = '0'; v->vector[n + 2] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlIncrementCorrelationVector(CV_ *v) {
  NTSTATUS status = RtlValidateCorrelationVector(v);
  if (!NT_SUCCESS(status)) return status;
  ULONG n = cv_length(v), start = n;
  while (start > 0 && v->vector[start - 1] != '.') --start;
  ULONG value = 0;
  for (ULONG i = start; i < n; ++i) value = value * 10 + (ULONG)(v->vector[i] - '0');
  if (value >= 0x7FFFFFFFu) return ((NTSTATUS)0xC0000001);
  ++value;
  char digits[12];
  ULONG count = 0;
  for (ULONG x = value; x; x /= 10) digits[count++] = (char)('0' + x % 10);
  if (start + count > cv_max(v)) return ((NTSTATUS)0xC0000001);
  for (ULONG i = 0; i < count; ++i) v->vector[start + i] = digits[count - 1 - i];
  v->vector[start + count] = 0;
  return STATUS_SUCCESS;
}

/* ---- heaps: the calls of the debugging heap that have nothing to do here ---- */

EXPORT BOOLEAN NTAPI RtlProtectHeap(PVOID heap, BOOLEAN protect) { (void)heap; (void)protect; return TRUE; }
EXPORT NTSTATUS NTAPI RtlZeroHeap(PVOID heap, ULONG flags) { (void)heap; (void)flags; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlUsageHeap(PVOID heap, ULONG flags, PVOID usage) { (void)heap; (void)flags; (void)usage; return STATUS_NOT_IMPLEMENTED; }
EXPORT NTSTATUS NTAPI RtlExtendHeap(PVOID heap, ULONG flags, PVOID base, SIZE_T size) { (void)heap; (void)flags; (void)base; (void)size; return STATUS_NOT_IMPLEMENTED; }
EXPORT void NTAPI RtlFlushHeaps(void) {}
EXPORT ULONG NTAPI RtlCreateTagHeap(PVOID heap, ULONG flags, PWSTR name, PWSTR tag_names) { (void)heap; (void)flags; (void)name; (void)tag_names; return 0; }
EXPORT PWSTR NTAPI RtlQueryTagHeap(PVOID heap, ULONG flags, USHORT tag, BOOLEAN reset, PVOID info) { (void)heap; (void)flags; (void)tag; (void)reset; (void)info; return NULL; }
EXPORT NTSTATUS NTAPI RtlDetectHeapLeaks(void) { return STATUS_NOT_IMPLEMENTED; }
EXPORT NTSTATUS NTAPI RtlQueryProcessHeapInformation(PVOID buffer) { (void)buffer; return STATUS_NOT_IMPLEMENTED; }
EXPORT void NTAPI RtlHeapTrkInitialize(void) {}
EXPORT NTSTATUS NTAPI RtlEnumProcessHeaps(NTSTATUS (NTAPI *callback)(PVOID heap, PVOID parameter), PVOID parameter) {
  PVOID heaps[64];
  const ULONG n = RtlGetProcessHeaps(64, heaps);
  NTSTATUS status = STATUS_SUCCESS;
  for (ULONG i = 0; i < n && i < 64 && NT_SUCCESS(status); ++i) status = callback(heaps[i], parameter);
  return status;
}
EXPORT BOOLEAN NTAPI RtlValidateProcessHeaps(void) {
  PVOID heaps[64];
  const ULONG n = RtlGetProcessHeaps(64, heaps);
  for (ULONG i = 0; i < n && i < 64; ++i) if (!RtlValidateHeap(heaps[i], 0, NULL)) return FALSE;
  return TRUE;
}
EXPORT ULONG NTAPI RtlMultipleAllocateHeap(PVOID heap, ULONG flags, SIZE_T size, ULONG count, PVOID *array) {
  ULONG i = 0;
  for (; i < count; ++i) { array[i] = RtlAllocateHeap(heap, flags, size); if (!array[i]) break; }
  return i;
}
EXPORT ULONG NTAPI RtlMultipleFreeHeap(PVOID heap, ULONG flags, ULONG count, PVOID *array) {
  ULONG freed = 0;
  for (ULONG i = 0; i < count; ++i) if (RtlFreeHeap(heap, flags, array[i])) ++freed;
  return freed;
}

/* ---- security identifiers ---- */

PVOID NTAPI RtlSubAuthoritySid(PVOID sid, ULONG index);
NTSTATUS NTAPI RtlInitializeSid(PVOID sid, PVOID authority, UCHAR count);
ULONG NTAPI RtlLengthRequiredSid(ULONG count);
ULONG NTAPI RtlLengthSid(PVOID sid);
BOOLEAN NTAPI RtlEqualSid(PVOID a, PVOID b);
NTSTATUS NTAPI RtlCopySid(ULONG length, PVOID to, PVOID from);
NTSTATUS NTAPI NtQueryInformationToken(HANDLE token, ULONG information_class, PVOID information, ULONG length, PULONG returned);
NTSTATUS NTAPI NtOpenThreadToken(HANDLE thread, ULONG access, BOOLEAN self, PHANDLE token);
NTSTATUS NTAPI NtOpenProcessToken(HANDLE process, ULONG access, PHANDLE token);
NTSTATUS NTAPI NtDuplicateToken(HANDLE token, ULONG access, POBJECT_ATTRIBUTES attributes, BOOLEAN effective_only, ULONG type, PHANDLE out);
EXPORT NTSTATUS NTAPI RtlAllocateAndInitializeSidEx(PVOID authority, UCHAR count, const ULONG *sub_authorities, PVOID *sid) {
  if (count > 15) return STATUS_INVALID_SID_;
  BYTE *s = RtlAllocateHeap(process_heap(), 0, RtlLengthRequiredSid(count));
  if (!s) return STATUS_NO_MEMORY;
  RtlInitializeSid(s, authority, count);
  for (UCHAR i = 0; i < count; ++i) *(ULONG *)RtlSubAuthoritySid(s, i) = sub_authorities[i];
  *sid = s;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInitializeSidEx(PVOID sid, PVOID authority, UCHAR count, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, count);
  if (count > 15) { __builtin_va_end(args); return STATUS_INVALID_SID_; }
  RtlInitializeSid(sid, authority, count);
  for (UCHAR i = 0; i < count; ++i) *(ULONG *)RtlSubAuthoritySid(sid, i) = __builtin_va_arg(args, ULONG);
  __builtin_va_end(args);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlCopySidAndAttributesArray(ULONG count, const BYTE *source, ULONG buffer_size, BYTE *destination, BYTE *sid_area, BYTE **sid_area_end, ULONG *remaining) {
  /* SID_AND_ATTRIBUTES is a pointer and a flags word: the SIDs are copied to the area */
  ULONG room = buffer_size;
  BYTE *area = sid_area;
  for (ULONG i = 0; i < count; ++i) {
    PVOID sid = *(PVOID *)(source + 16 * (SIZE_T)i);
    const ULONG length = RtlLengthSid(sid);
    if (length > room) return (NTSTATUS)0xC0000023;
    memcpy(area, sid, length);
    *(PVOID *)(destination + 16 * (SIZE_T)i) = area;
    *(ULONG *)(destination + 16 * (SIZE_T)i + 8) = *(const ULONG *)(source + 16 * (SIZE_T)i + 8);
    area += length; room -= length;
  }
  if (sid_area_end) *sid_area_end = area;
  if (remaining) *remaining = room;
  return STATUS_SUCCESS;
}
/* The text length of a SID as "S-1-5-..." (characters, no end mark). */
EXPORT NTSTATUS NTAPI RtlLengthSidAsUnicodeString(PVOID sid, PULONG length) {
  const BYTE *s = sid;
  if (s[0] != 1 || s[1] > 15) return STATUS_INVALID_SID_;
  ULONG n = 4;      /* "S-1-" */
  ULONGLONG authority = 0;
  for (int i = 2; i < 8; ++i) authority = authority << 8 | s[i];
  if (authority >= 0x100000000ull) n += 2 + 12; else { ULONG d = 1; for (ULONGLONG a = authority; a >= 10; a /= 10) ++d; n += d; }
  for (UCHAR i = 0; i < s[1]; ++i) { ULONG d = 1; for (ULONG a = *(const ULONG *)(s + 8 + 4 * i); a >= 10; a /= 10) ++d; n += 1 + d; }
  *length = n * 2;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlIsPackageSid(PVOID sid) {      /* S-1-15-2-... with 8 sub-authorities */
  const BYTE *s = sid;
  return s[1] == 9 && s[7] == 15 && *(const ULONG *)(s + 8) == 2;
}
EXPORT BOOLEAN NTAPI RtlIsCapabilitySid(PVOID sid) {      /* S-1-15-3-... */
  const BYTE *s = sid;
  return s[1] >= 2 && s[7] == 15 && *(const ULONG *)(s + 8) == 3;
}
EXPORT BOOLEAN NTAPI RtlIsValidProcessTrustLabelSid(PVOID sid) { const BYTE *s = sid; return s[1] == 2 && s[7] == 19; }
EXPORT NTSTATUS NTAPI RtlCreateVirtualAccountSid(const UNICODE_STRING *name, ULONG base_sub_authority, PVOID sid, PULONG length) {
  /* S-1-5-<base>-<five 32-bit parts of the SHA-1 hash of the upper-cased name> */
  if (!name || !length) return STATUS_INVALID_PARAMETER;
  if (*length < 36) { *length = 36; return (NTSTATUS)0xC0000023; }
  SHA_CTX_ ctx;
  BYTE digest[20];
  A_SHAInit(&ctx);
  for (USHORT i = 0; i < name->Length / 2; ++i) { const WCHAR c = RtlUpcaseUnicodeChar(name->Buffer[i]); A_SHAUpdate(&ctx, (const BYTE *)&c, 2); }
  A_SHAFinal(&ctx, digest);
  BYTE authority[6] = {0, 0, 0, 0, 0, 5};
  RtlInitializeSid(sid, authority, 6);
  *(ULONG *)RtlSubAuthoritySid(sid, 0) = base_sub_authority;
  for (int i = 0; i < 5; ++i) *(ULONG *)RtlSubAuthoritySid(sid, (ULONG)i + 1) = *(const ULONG *)(digest + 4 * i);
  *length = 36;
  return STATUS_SUCCESS;
}
/* Is the SID among the enabled groups (or the user) of the token? A null token is the thread's own token (an impersonation) or the process's, made one. */
EXPORT NTSTATUS NTAPI RtlCheckTokenMembershipEx(HANDLE token, PVOID sid, ULONG flags, PBOOLEAN is_member) {
  (void)flags;
  HANDLE own = NULL, use = token;
  *is_member = FALSE;
  if (!use) {
    if (!NT_SUCCESS(NtOpenThreadToken((HANDLE)(LONG_PTR)-2, 0x8, TRUE, &own))) {
      HANDLE primary = NULL;
      NTSTATUS status = NtOpenProcessToken((HANDLE)(LONG_PTR)-1, 0x2 | 0x8, &primary);      /* TOKEN_DUPLICATE | TOKEN_QUERY */
      if (!NT_SUCCESS(status)) return status;
      OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, NULL, 0, NULL, NULL};
      status = NtDuplicateToken(primary, 0x8, &attributes, FALSE, 2 /* TokenImpersonation */, &own);
      NtClose(primary);
      if (!NT_SUCCESS(status)) return status;
    }
    use = own;
  }
  NTSTATUS status = STATUS_SUCCESS;
  BYTE user[256];
  ULONG got = 0;
  if (NT_SUCCESS(NtQueryInformationToken(use, 1 /* TokenUser */, user, sizeof(user), &got)) && RtlEqualSid(*(PVOID *)user, sid)) *is_member = TRUE;
  if (!*is_member) {
    BYTE *groups = RtlAllocateHeap(process_heap(), 0, 4096);
    if (!groups) status = STATUS_NO_MEMORY;
    else {
      status = NtQueryInformationToken(use, 2 /* TokenGroups */, groups, 4096, &got);
      if (NT_SUCCESS(status)) {
        const ULONG count = *(ULONG *)groups;
        for (ULONG i = 0; i < count; ++i) {
          const BYTE *entry = groups + 8 + 16 * (SIZE_T)i;      /* SID_AND_ATTRIBUTES: a pointer and the attributes */
          if ((*(const ULONG *)(entry + 8) & 0x4 /* SE_GROUP_ENABLED */) && RtlEqualSid(*(PVOID *)entry, sid)) { *is_member = TRUE; break; }
        }
      }
      RtlFreeHeap(process_heap(), 0, groups);
    }
  }
  if (own) NtClose(own);
  return status;
}
EXPORT NTSTATUS NTAPI RtlCheckTokenMembership(HANDLE token, PVOID sid, PBOOLEAN is_member) { return RtlCheckTokenMembershipEx(token, sid, 0, is_member); }

/* ---- threads and stacks ---- */

EXPORT NTSTATUS NTAPI RtlCreateUserThread(HANDLE process, PVOID descriptor, BOOLEAN suspended, ULONG zero_bits, SIZE_T reserve, SIZE_T commit, PVOID start, PVOID parameter, PHANDLE thread_out, ULONG_PTR *client_id) {
  (void)descriptor;
  HANDLE thread = NULL;
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, process, start, parameter, suspended ? 1 : 0, zero_bits, commit, reserve, NULL);
  if (!NT_SUCCESS(status)) return status;
  if (client_id) {
    ULONG64 info[6];
    if (NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL))) { client_id[0] = (ULONG_PTR)info[2]; client_id[1] = (ULONG_PTR)info[3]; }
  }
  if (thread_out) *thread_out = thread; else NtClose(thread);
  return STATUS_SUCCESS;
}
/* INITIAL_TEB: OldStackBase, OldStackLimit, StackBase, StackLimit, AllocatedStackBase */
EXPORT NTSTATUS NTAPI RtlCreateUserStack(SIZE_T commit, SIZE_T reserve, ULONG_PTR zero_bits, SIZE_T guard_align, SIZE_T reserve_align, ULONG_PTR *initial_teb) {
  (void)guard_align;
  if (!reserve) reserve = 0x100000;
  if (!reserve_align) reserve_align = 0x10000;
  reserve = (reserve + reserve_align - 1) & ~(reserve_align - 1);
  if (commit > reserve) reserve = commit;
  PVOID base = NULL;
  SIZE_T size = reserve;
  NTSTATUS status = NtAllocateVirtualMemory(NtCurrentProcess(), &base, zero_bits, &size, 0x2000, 0x04);
  if (!NT_SUCCESS(status)) return status;
  commit = (commit + 0xFFF) & ~(SIZE_T)0xFFF;
  if (commit == 0) commit = 0x2000;
  PVOID top = (BYTE *)base + size - commit;
  SIZE_T commit_size = commit;
  status = NtAllocateVirtualMemory(NtCurrentProcess(), &top, 0, &commit_size, 0x1000, 0x04);
  if (!NT_SUCCESS(status)) { SIZE_T zero = 0; NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, 0x8000); return status; }
  initial_teb[0] = initial_teb[1] = 0;
  initial_teb[2] = (ULONG_PTR)base + size;
  initial_teb[3] = (ULONG_PTR)top;
  initial_teb[4] = (ULONG_PTR)base;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlFreeUserStack(PVOID allocated_base) { SIZE_T zero = 0; return NtFreeVirtualMemory(NtCurrentProcess(), &allocated_base, &zero, 0x8000); }
EXPORT void NTAPI RtlFreeUserThreadStack(HANDLE process, HANDLE thread) { (void)process; (void)thread; }
EXPORT NTSTATUS NTAPI RtlCreateUserFiberShadowStack(void) { return STATUS_NOT_SUPPORTED_; }
EXPORT NTSTATUS NTAPI RtlFreeUserFiberShadowStack(void) { return STATUS_NOT_SUPPORTED_; }


/* ole32: structured storage.
 *
 * The compound file format (MS-CFB: version 3 with 512-byte sectors, version 4 with 4096-byte sectors, 64-byte mini sectors below the 4096-byte cutoff, the FAT reached
 * through the DIFAT, the mini FAT, directory entries kept in red-black trees ordered by name length and then by upper-case name). A file is read whole into a tree of
 * nodes on open (every sector index and size from the file is checked, chains and the directory tree are walked with "used" bitmaps so a loop or a cross link is an
 * error, nesting is capped) and the tree is written back whole: on Commit and Release of a direct root (as Windows writes a direct file), on Commit of a transacted root.
 * A transacted storage works on its own copy of the subtree and Commit copies it into its parent; Revert throws the copy away and invalidates what was open in it.
 *
 * Objects: IStorage (+ IDirectWriterLock for a direct single-writer root, IRootStorage on roots, IPropertySetStorage from propstg.inc), IStream, IEnumSTATSTG. All the
 * objects of one root share the root's critical section. A child keeps the memory of its parent but not its parent's open state: when the last reference to a storage
 * goes (and no enumerator keeps it open) its children read STG_E_REVERTED, as on Windows.
 *
 * Files: an ILockBytes over a file handle; StgOpenStorage / StgCreateDocfile take the share and access of Windows and the byte-range locks of Windows (the ranges at
 * 0x7FFFFF00.. that tell other openers who reads, writes and denies what), so that sharing works between processes and with the tests of Wine. */

static HRESULT propsetstg_create(IStorage *storage, IPropertySetStorage **out);      /* (propstg.inc) */

/* ---- flags and small helpers ---------------------------------------------------------------------------------------------------------------------------------- */

#define STG_ACCESS(m) ((DWORD)(m) & 0x0000000Fu)
#define STG_SHARE(m) ((DWORD)(m) & 0x000000F0u)
#define STG_CREATEMODE(m) ((DWORD)(m) & 0x0000F000u)
#define STG_KNOWN_FLAGS (0xF0FFu | STGM_TRANSACTED | STGM_CONVERT | STGM_PRIORITY | STGM_NOSCRATCH | STGM_NOSNAPSHOT | STGM_DIRECT_SWMR | STGM_DELETEONRELEASE | STGM_SIMPLE)

static HRESULT stg_validate_mode(DWORD m) {
  DWORD access = STG_ACCESS(m), share = STG_SHARE(m), create = STG_CREATEMODE(m);
  if (m & ~(DWORD)STG_KNOWN_FLAGS) return E_FAIL;
  if (access != STGM_READ && access != STGM_WRITE && access != STGM_READWRITE) return E_FAIL;
  switch (share) {
    case STGM_SHARE_DENY_NONE: case STGM_SHARE_DENY_READ: case STGM_SHARE_DENY_WRITE: case STGM_SHARE_EXCLUSIVE: break;
    case 0: if (!(m & STGM_TRANSACTED)) return E_FAIL; break;
    default: return E_FAIL;
  }
  if (create != STGM_CREATE && create != STGM_FAILIFTHERE) return E_FAIL;
  if ((m & STGM_TRANSACTED) && (m & STGM_SIMPLE)) return E_FAIL;
  if ((m & STGM_CREATE) && (m & STGM_CONVERT)) return E_FAIL;
  if ((m & STGM_NOSCRATCH) && !(m & STGM_TRANSACTED)) return E_FAIL;
  if ((m & STGM_NOSNAPSHOT) && (!(m & STGM_TRANSACTED) || share == STGM_SHARE_EXCLUSIVE || share == STGM_SHARE_DENY_WRITE)) return E_FAIL;
  return S_OK;
}

static WORD stg_rd16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD stg_rd32(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }
static ULONGLONG stg_rd64(const BYTE *p) { return (ULONGLONG)stg_rd32(p) | (ULONGLONG)stg_rd32(p + 4) << 32; }
static void stg_wr16(BYTE *p, WORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void stg_wr32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static void stg_wr64(BYTE *p, ULONGLONG v) { stg_wr32(p, (DWORD)v); stg_wr32(p + 4, (DWORD)(v >> 32)); }

static BOOL stg_bit(const BYTE *map, ULONG i) { return (map[i >> 3] >> (i & 7)) & 1; }
static void stg_set_bit(BYTE *map, ULONG i) { map[i >> 3] |= (BYTE)(1u << (i & 7)); }

static WCHAR *stg_cotask_dup(const WCHAR *s) {
  SIZE_T n = wlen(s) + 1;
  WCHAR *r = CoTaskMemAlloc(n * sizeof(WCHAR));
  if (r) mem_copy(r, s, n * sizeof(WCHAR));
  return r;
}

/* An element name: 1..31 characters, none of ! : / \ (the characters MS-CFB forbids). */
static HRESULT stg_check_name(const WCHAR *name) {
  SIZE_T n;
  if (!name) return STG_E_INVALIDNAME;
  for (n = 0; name[n]; n++) {
    WCHAR c = name[n];
    if (n >= 31) return STG_E_INVALIDNAME;
    if (c == '!' || c == ':' || c == '/' || c == '\\') return STG_E_INVALIDNAME;
  }
  return n ? S_OK : STG_E_INVALIDNAME;
}

/* The order of the directory: the shorter name first, then the upper-case characters one by one. Lookups are case-insensitive as on Windows. */
static int stg_name_cmp(const WCHAR *a, const WCHAR *b) {
  SIZE_T la = wlen(a), lb = wlen(b), i;
  if (la != lb) return la < lb ? -1 : 1;
  for (i = 0; i < la; i++) {
    WCHAR x = wup(a[i]), y = wup(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

/* ---- the tree in memory --------------------------------------------------------------------------------------------------------------------------------------- */

#define STG_MAX_STREAM 0x7FFFF000u        /* largest stream kept in memory */
#define STG_MAX_DEPTH 256                 /* nesting of storages (bounds every recursion over a tree) */
#define STG_MAX_KIDS 0x00FFFFFFu

typedef struct StgNode StgNode;
struct StgNode {
  LONG refs;                 /* the tree and the objects that use it (under the root lock) */
  LONG open_count;           /* objects opened on this element: a second open is STG_E_ACCESSDENIED */
  BOOL removed;              /* destroyed, overwritten or reverted: objects on it read STG_E_REVERTED */
  BYTE type;                 /* STGTY_STORAGE (the root too) or STGTY_STREAM */
  WCHAR name[32];
  CLSID clsid;
  DWORD state_bits;
  FILETIME ctime, mtime;
  BYTE *data;                /* stream */
  SIZE_T size, cap;
  StgNode **kids;            /* storage: sorted by stg_name_cmp */
  ULONG nkids, kidcap;
};

static StgNode *stg_node_new(BYTE type, const WCHAR *name) {
  StgNode *n = mem_alloc(sizeof(*n));
  if (!n) return NULL;
  n->refs = 1;
  n->type = type;
  if (name) wcopy_n(n->name, name, 32);
  return n;
}
static void stg_node_release(StgNode *n) {
  ULONG i;
  if (!n || --n->refs > 0) return;
  for (i = 0; i < n->nkids; i++) stg_node_release(n->kids[i]);
  mem_free(n->kids);
  mem_free(n->data);
  mem_free(n);
}
static void stg_node_mark_removed(StgNode *n) {
  ULONG i;
  n->removed = TRUE;
  for (i = 0; i < n->nkids; i++) stg_node_mark_removed(n->kids[i]);
}
/* the child called `name` or NULL; *at = its place or the place it would take */
static StgNode *stg_node_find(const StgNode *p, const WCHAR *name, ULONG *at) {
  ULONG lo = 0, hi = p->nkids;
  while (lo < hi) {
    ULONG mid = lo + (hi - lo) / 2;
    int c = stg_name_cmp(p->kids[mid]->name, name);
    if (c == 0) { if (at) *at = mid; return p->kids[mid]; }
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  if (at) *at = lo;
  return NULL;
}
/* the first child whose name sorts after `name` (the enumerator's cursor), or nkids */
static ULONG stg_node_after(const StgNode *p, const WCHAR *name) {
  ULONG lo = 0, hi = p->nkids;
  while (lo < hi) {
    ULONG mid = lo + (hi - lo) / 2;
    if (stg_name_cmp(p->kids[mid]->name, name) <= 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}
static HRESULT stg_node_insert(StgNode *p, StgNode *k, ULONG at) {
  ULONG i;
  if (p->nkids >= STG_MAX_KIDS) return STG_E_INSUFFICIENTMEMORY;
  if (p->nkids == p->kidcap) {
    ULONG cap = p->kidcap ? p->kidcap * 2 : 4;
    StgNode **a = mem_realloc(p->kids, (SIZE_T)cap * sizeof(*a));
    if (!a) return STG_E_INSUFFICIENTMEMORY;
    p->kids = a;
    p->kidcap = cap;
  }
  for (i = p->nkids; i > at; i--) p->kids[i] = p->kids[i - 1];
  p->kids[at] = k;
  p->nkids++;
  return S_OK;
}
static StgNode *stg_node_take(StgNode *p, ULONG at) {
  StgNode *k = p->kids[at];
  ULONG i;
  for (i = at; i + 1 < p->nkids; i++) p->kids[i] = p->kids[i + 1];
  p->nkids--;
  return k;
}
static HRESULT stg_node_resize(StgNode *n, SIZE_T size) {
  if (size > STG_MAX_STREAM) return STG_E_MEDIUMFULL;
  if (size > n->cap) {
    SIZE_T cap = n->cap < 256 ? 256 : n->cap;
    while (cap < size) cap = cap > STG_MAX_STREAM / 2 ? STG_MAX_STREAM : cap * 2;
    BYTE *d = mem_realloc(n->data, cap);
    if (!d) return STG_E_INSUFFICIENTMEMORY;
    n->data = d;
    n->cap = cap;
  }
  if (size > n->size) mem_zero(n->data + n->size, size - n->size);
  n->size = size;
  return S_OK;
}
static StgNode *stg_node_clone(const StgNode *n) {
  StgNode *c = stg_node_new(n->type, n->name);
  ULONG i;
  if (!c) return NULL;
  c->clsid = n->clsid;
  c->state_bits = n->state_bits;
  c->ctime = n->ctime;
  c->mtime = n->mtime;
  if (n->size) {
    c->data = mem_alloc(n->size);
    if (!c->data) goto fail;
    mem_copy(c->data, n->data, n->size);
    c->size = c->cap = n->size;
  }
  if (n->nkids) {
    c->kids = mem_alloc((SIZE_T)n->nkids * sizeof(StgNode *));
    if (!c->kids) goto fail;
    c->kidcap = n->nkids;
    for (i = 0; i < n->nkids; i++) {
      StgNode *k = stg_node_clone(n->kids[i]);
      if (!k) goto fail;
      c->kids[c->nkids++] = k;
    }
  }
  return c;
fail:
  stg_node_release(c);
  return NULL;
}
/* dst takes a copy of what src holds (the commit of a transacted storage into its parent) */
static HRESULT stg_node_assign(StgNode *dst, const StgNode *src) {
  StgNode *c = stg_node_clone(src);
  ULONG i;
  if (!c) return STG_E_INSUFFICIENTMEMORY;
  for (i = 0; i < dst->nkids; i++) { stg_node_mark_removed(dst->kids[i]); stg_node_release(dst->kids[i]); }
  mem_free(dst->kids);
  mem_free(dst->data);
  dst->kids = c->kids; dst->nkids = c->nkids; dst->kidcap = c->kidcap;
  dst->data = c->data; dst->size = c->size; dst->cap = c->cap;
  dst->clsid = c->clsid; dst->state_bits = c->state_bits; dst->ctime = c->ctime; dst->mtime = c->mtime;
  c->kids = NULL; c->nkids = 0; c->data = NULL; c->size = 0;
  stg_node_release(c);
  return S_OK;
}
static ULONG stg_node_count(const StgNode *n) {
  ULONG total = 1, i;
  for (i = 0; i < n->nkids; i++) {
    ULONG k = stg_node_count(n->kids[i]);
    if (k > 0x0FFFFFFFu - total) return 0x0FFFFFFFu;
    total += k;
  }
  return total;
}

/* ---- reading a compound file ---------------------------------------------------------------------------------------------------------------------------------- */

#define CFB_FREESECT 0xFFFFFFFFu
#define CFB_ENDOFCHAIN 0xFFFFFFFEu
#define CFB_FATSECT 0xFFFFFFFDu
#define CFB_DIFSECT 0xFFFFFFFCu
#define CFB_NOSTREAM 0xFFFFFFFFu
#define CFB_CUTOFF 4096u
#define CFB_MAX_FILE 0xFFFFFFFFull        /* files read whole into memory: 4 GB at most */

static const BYTE kCfbSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
static const WCHAR kRootEntryName[] = {'R', 'o', 'o', 't', ' ', 'E', 'n', 't', 'r', 'y', 0};

static HRESULT cfb_read_at(ILockBytes *lb, ULONGLONG off, void *buf, ULONG n, ULONG *got) {
  ULARGE_INTEGER o;
  ULONG dummy = 0;
  o.QuadPart = off;
  if (!got) got = &dummy;
  *got = 0;
  return ILockBytes_ReadAt(lb, o, buf, n, got);
}
static HRESULT cfb_write_at(ILockBytes *lb, ULONGLONG off, const void *buf, ULONG n) {
  ULARGE_INTEGER o;
  ULONG done = 0;
  HRESULT hr;
  if (!n) return S_OK;
  o.QuadPart = off;
  hr = ILockBytes_WriteAt(lb, o, buf, n, &done);
  if (FAILED(hr)) return hr;
  return done == n ? S_OK : STG_E_WRITEFAULT;
}

typedef struct CfbIn {
  ILockBytes *lb;
  ULONGLONG fsize;
  ULONG shift, ss, nsect;
  DWORD *fat;
  ULONG nfat;
  DWORD *minifat;
  ULONG nminifat;
  BYTE *used;            /* sectors already part of a chain */
  BYTE *mused;           /* mini sectors already part of a chain */
  BYTE *dir;
  ULONG ndir;
  BYTE *mini;            /* the mini stream */
  SIZE_T minisize;
  ULONG cutoff;
  BYTE *visited;         /* directory entries already placed in the tree */
} CfbIn;

/* `want` bytes of the sectors from `first` on (consecutive); the part past the end of the file reads as zeros */
static HRESULT cfb_read_run(CfbIn *in, ULONG first, BYTE *dst, SIZE_T want) {
  ULONGLONG off = ((ULONGLONG)first + 1) << in->shift;
  ULONG got = 0;
  HRESULT hr;
  if (!want || off >= in->fsize) return S_OK;
  hr = cfb_read_at(in->lb, off, dst, (ULONG)want, &got);
  if (FAILED(hr)) return hr;
  if (got < want && off + got < in->fsize) return STG_E_READFAULT;
  return S_OK;
}
static BOOL cfb_take_sector(CfbIn *in, DWORD s) {
  if (s >= in->nsect || s >= in->nfat || stg_bit(in->used, s)) return FALSE;
  stg_set_bit(in->used, s);
  return TRUE;
}
/* `size` bytes of the chain from `start` (consecutive sectors are read together) */
static HRESULT cfb_read_chain(CfbIn *in, DWORD start, SIZE_T size, BYTE *dst) {
  ULONG need = (ULONG)((size + in->ss - 1) >> in->shift), i, run_first = 0, run_len = 0;
  SIZE_T pos = 0;
  DWORD cur = start;
  HRESULT hr;
  for (i = 0; i < need; i++) {
    if (!cfb_take_sector(in, cur)) return STG_E_DOCFILECORRUPT;
    if (run_len && cur == run_first + run_len && run_len < 0x10000) run_len++;
    else {
      if (run_len) {
        SIZE_T n = (SIZE_T)run_len << in->shift;
        if (n > size - pos) n = size - pos;
        hr = cfb_read_run(in, run_first, dst + pos, n);
        if (FAILED(hr)) return hr;
        pos += n;
      }
      run_first = cur;
      run_len = 1;
    }
    cur = in->fat[cur];
  }
  if (run_len) {
    SIZE_T n = (SIZE_T)run_len << in->shift;
    if (n > size - pos) n = size - pos;
    hr = cfb_read_run(in, run_first, dst + pos, n);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}
static HRESULT cfb_read_mini(CfbIn *in, DWORD start, SIZE_T size, BYTE *dst) {
  ULONG need = (ULONG)((size + 63) >> 6), i;
  SIZE_T pos = 0;
  DWORD cur = start;
  for (i = 0; i < need; i++) {
    SIZE_T off, n;
    if (cur >= in->nminifat || stg_bit(in->mused, cur)) return STG_E_DOCFILECORRUPT;
    off = (SIZE_T)cur << 6;
    if (off >= in->minisize) return STG_E_DOCFILECORRUPT;
    n = size - pos < 64 ? size - pos : 64;
    if (n > in->minisize - off) return STG_E_DOCFILECORRUPT;
    stg_set_bit(in->mused, cur);
    mem_copy(dst + pos, in->mini + off, n);
    pos += n;
    cur = in->minifat[cur];
  }
  return S_OK;
}
/* the sectors of a chain whose length is not stored (directory): counted (and marked) first */
static HRESULT cfb_chain_length(CfbIn *in, DWORD start, ULONG *count) {
  ULONG n = 0;
  DWORD cur = start;
  while (cur != CFB_ENDOFCHAIN) {
    if (!cfb_take_sector(in, cur)) return STG_E_DOCFILECORRUPT;
    n++;
    cur = in->fat[cur];
  }
  *count = n;
  return S_OK;
}

static StgNode *cfb_entry_node(CfbIn *in, ULONG id, HRESULT *hr) {
  const BYTE *e = in->dir + (SIZE_T)id * 128;
  WCHAR name[32];
  ULONG chars = stg_rd16(e + 64) / 2, i;
  BYTE type = e[66];
  StgNode *n;
  if (chars > 32) chars = 32;
  for (i = 0; i < chars && i < 31; i++) {
    name[i] = (WCHAR)stg_rd16(e + 2 * i);
    if (!name[i]) break;
  }
  name[i] = 0;
  n = stg_node_new(type == 2 ? STGTY_STREAM : STGTY_STORAGE, name);
  if (!n) { *hr = STG_E_INSUFFICIENTMEMORY; return NULL; }
  mem_copy(&n->clsid, e + 80, 16);
  n->state_bits = stg_rd32(e + 96);
  n->ctime.dwLowDateTime = stg_rd32(e + 100); n->ctime.dwHighDateTime = stg_rd32(e + 104);
  n->mtime.dwLowDateTime = stg_rd32(e + 108); n->mtime.dwHighDateTime = stg_rd32(e + 112);
  if (type == 2) {
    DWORD start = stg_rd32(e + 116);
    ULONGLONG size = in->shift == 9 ? stg_rd32(e + 120) : stg_rd64(e + 120);      /* version 3: the high half is not used */
    mem_zero(&n->clsid, sizeof(n->clsid));
    if (size) {
      if (size > STG_MAX_STREAM || (size >= in->cutoff ? size > ((ULONGLONG)in->nsect << in->shift) : size > in->minisize)) { *hr = STG_E_DOCFILECORRUPT; stg_node_release(n); return NULL; }
      n->data = mem_alloc((SIZE_T)size);
      if (!n->data) { *hr = STG_E_INSUFFICIENTMEMORY; stg_node_release(n); return NULL; }
      n->size = n->cap = (SIZE_T)size;
      *hr = size >= in->cutoff ? cfb_read_chain(in, start, (SIZE_T)size, n->data) : cfb_read_mini(in, start, (SIZE_T)size, n->data);
      if (FAILED(*hr)) { stg_node_release(n); return NULL; }
    }
  }
  *hr = S_OK;
  return n;
}

typedef struct CfbPending { StgNode *storage; DWORD tree; ULONG depth; } CfbPending;

/* places the entries of the red-black tree `tree` under `parent` (iteratively; each entry at most once) and queues the storages among them */
static HRESULT cfb_fill_storage(CfbIn *in, StgNode *parent, DWORD tree, ULONG depth, DWORD *walk, CfbPending *pending, ULONG *npending) {
  ULONG top = 0;
  HRESULT hr = S_OK;
  if (tree == CFB_NOSTREAM) return S_OK;
  walk[top++] = tree;
  while (top) {
    DWORD id = walk[--top];
    const BYTE *e;
    BYTE type;
    if (id >= in->ndir || stg_bit(in->visited, id)) return STG_E_DOCFILECORRUPT;
    stg_set_bit(in->visited, id);
    e = in->dir + (SIZE_T)id * 128;
    if (stg_rd32(e + 68) != CFB_NOSTREAM) walk[top++] = stg_rd32(e + 68);
    if (stg_rd32(e + 72) != CFB_NOSTREAM) walk[top++] = stg_rd32(e + 72);
    type = e[66];
    if (type == 1 || type == 2) {
      ULONG at;
      StgNode *n = cfb_entry_node(in, id, &hr);
      if (!n) return hr;
      if (!n->name[0] || stg_node_find(parent, n->name, &at)) { stg_node_release(n); continue; }      /* a nameless or doubled entry is left out */
      hr = stg_node_insert(parent, n, at);
      if (FAILED(hr)) { stg_node_release(n); return hr; }
      if (type == 1) {
        if (depth + 1 >= STG_MAX_DEPTH) return STG_E_DOCFILECORRUPT;
        pending[*npending].storage = n;
        pending[*npending].tree = stg_rd32(e + 76);
        pending[*npending].depth = depth + 1;
        (*npending)++;
      }
    } else if (type != 0) return STG_E_DOCFILECORRUPT;
  }
  return S_OK;
}

static HRESULT cfb_check_header(const BYTE *h, ULONG *shift) {
  WORD major = stg_rd16(h + 0x1A), sshift = stg_rd16(h + 0x1E);
  if (mem_cmp(h, kCfbSignature, 8)) return STG_E_INVALIDHEADER;
  if (stg_rd16(h + 0x1C) != 0xFFFE) return STG_E_INVALIDHEADER;
  if (!((major == 3 && sshift == 9) || (major == 4 && sshift == 12))) return STG_E_INVALIDHEADER;
  if (stg_rd16(h + 0x20) != 6) return STG_E_INVALIDHEADER;
  *shift = sshift;
  return S_OK;
}

/* Reads the compound file in `lb` into a tree. STG_E_INVALIDHEADER: not a compound file; STG_E_DOCFILECORRUPT: a compound file that cannot be read. */
static HRESULT cfb_read(ILockBytes *lb, StgNode **tree_out, ULONG *shift_out, DWORD *sig_out) {
  CfbIn in;
  STATSTG st;
  BYTE *h = NULL;
  DWORD *fatsec = NULL, *walk = NULL;
  CfbPending *pending = NULL;
  StgNode *tree = NULL;
  ULONG nfs, i, got = 0, hdr_area, npending = 0;
  HRESULT hr;

  *tree_out = NULL;
  mem_zero(&in, sizeof(in));
  in.lb = lb;
  hr = ILockBytes_Stat(lb, &st, STATFLAG_NONAME);
  if (FAILED(hr)) return hr;
  if (st.pwcsName) CoTaskMemFree(st.pwcsName);
  in.fsize = st.cbSize.QuadPart;
  if (in.fsize > CFB_MAX_FILE) return STG_E_DOCFILETOOLARGE;
  h = mem_alloc(512);
  if (!h) return STG_E_INSUFFICIENTMEMORY;
  hr = cfb_read_at(lb, 0, h, 512, &got);
  if (SUCCEEDED(hr) && got < 512) hr = STG_E_INVALIDHEADER;
  if (SUCCEEDED(hr)) hr = cfb_check_header(h, &in.shift);
  if (FAILED(hr)) goto done;
  in.ss = 1u << in.shift;
  hdr_area = in.shift == 12 ? 4096 : 512;
  if (in.fsize <= hdr_area) { hr = STG_E_INVALIDHEADER; goto done; }
  in.nsect = (ULONG)((in.fsize - hdr_area + in.ss - 1) >> in.shift);
  in.cutoff = stg_rd32(h + 0x38);
  if (!in.cutoff || in.cutoff > STG_MAX_STREAM) { hr = STG_E_DOCFILECORRUPT; goto done; }
  in.used = mem_alloc(in.nsect / 8 + 1);
  if (!in.used) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }

  /* the FAT, through the DIFAT */
  nfs = stg_rd32(h + 0x2C);
  if (nfs > in.nsect || nfs == 0) { hr = STG_E_DOCFILECORRUPT; goto done; }
  fatsec = mem_alloc((SIZE_T)nfs * sizeof(DWORD));
  if (!fatsec) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
  {
    ULONG have = 0, ndif = stg_rd32(h + 0x48), d;
    DWORD dsec = stg_rd32(h + 0x44);
    for (i = 0; i < 109 && have < nfs; i++) fatsec[have++] = stg_rd32(h + 0x4C + 4 * i);
    if (have < nfs) {
      BYTE *ds = mem_alloc(in.ss);
      if (!ds) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
      for (d = 0; d < ndif && have < nfs && SUCCEEDED(hr); d++) {
        if (dsec >= in.nsect || stg_bit(in.used, dsec)) { hr = STG_E_DOCFILECORRUPT; break; }
        stg_set_bit(in.used, dsec);
        hr = cfb_read_run(&in, dsec, ds, in.ss);
        for (i = 0; SUCCEEDED(hr) && i + 1 < in.ss / 4 && have < nfs; i++) fatsec[have++] = stg_rd32(ds + 4 * i);
        dsec = stg_rd32(ds + in.ss - 4);
      }
      mem_free(ds);
      if (FAILED(hr)) goto done;
      if (have < nfs) { hr = STG_E_DOCFILECORRUPT; goto done; }
    }
  }
  if ((ULONGLONG)nfs * (in.ss / 4) > 0x3FFFFFFFull) { hr = STG_E_DOCFILECORRUPT; goto done; }
  in.nfat = nfs * (in.ss / 4);
  in.fat = mem_alloc((SIZE_T)in.nfat * sizeof(DWORD));
  if (!in.fat) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
  for (i = 0; i < nfs; i++) {
    if (fatsec[i] >= in.nsect || stg_bit(in.used, fatsec[i])) { hr = STG_E_DOCFILECORRUPT; goto done; }
    stg_set_bit(in.used, fatsec[i]);
    hr = cfb_read_run(&in, fatsec[i], (BYTE *)(in.fat + (SIZE_T)i * (in.ss / 4)), in.ss);
    if (FAILED(hr)) goto done;
  }
  for (i = 0; i < in.nfat; i++) in.fat[i] = stg_rd32((const BYTE *)&in.fat[i]);      /* (little-endian on disk) */

  /* the directory */
  {
    ULONG count = 0, per = in.ss / 128;
    DWORD first = stg_rd32(h + 0x30), cur;
    hr = cfb_chain_length(&in, first, &count);
    if (FAILED(hr)) goto done;
    if (!count || (ULONGLONG)count * per > 0x00FFFFFFull) { hr = STG_E_DOCFILECORRUPT; goto done; }
    in.ndir = count * per;
    in.dir = mem_alloc((SIZE_T)count << in.shift);
    in.visited = mem_alloc(in.ndir / 8 + 1);
    if (!in.dir || !in.visited) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
    for (i = 0, cur = first; i < count && SUCCEEDED(hr); i++, cur = in.fat[cur]) hr = cfb_read_run(&in, cur, in.dir + ((SIZE_T)i << in.shift), in.ss);
    if (FAILED(hr)) goto done;
  }
  if (in.dir[66] != 5) { hr = STG_E_DOCFILECORRUPT; goto done; }

  /* the mini FAT and the mini stream (the data of the root entry) */
  {
    ULONG nmf = stg_rd32(h + 0x40);
    DWORD first = stg_rd32(h + 0x3C);
    ULONGLONG rootsize = in.shift == 9 ? stg_rd32(in.dir + 120) : stg_rd64(in.dir + 120);
    if (nmf && first != CFB_ENDOFCHAIN) {
      if (nmf > in.nsect) { hr = STG_E_DOCFILECORRUPT; goto done; }
      in.nminifat = nmf * (in.ss / 4);
      in.minifat = mem_alloc((SIZE_T)nmf << in.shift);
      in.mused = mem_alloc(in.nminifat / 8 + 1);
      if (!in.minifat || !in.mused) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
      hr = cfb_read_chain(&in, first, (SIZE_T)nmf << in.shift, (BYTE *)in.minifat);
      if (FAILED(hr)) goto done;
      for (i = 0; i < in.nminifat; i++) in.minifat[i] = stg_rd32((const BYTE *)&in.minifat[i]);
    }
    if (rootsize && in.nminifat) {
      if (rootsize > ((ULONGLONG)in.nsect << in.shift) || rootsize > STG_MAX_STREAM) { hr = STG_E_DOCFILECORRUPT; goto done; }
      in.minisize = (SIZE_T)rootsize;
      in.mini = mem_alloc(in.minisize);
      if (!in.mini) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
      hr = cfb_read_chain(&in, stg_rd32(in.dir + 116), in.minisize, in.mini);
      if (FAILED(hr)) goto done;
    }
  }

  /* the tree */
  tree = cfb_entry_node(&in, 0, &hr);
  if (!tree) goto done;
  tree->type = STGTY_STORAGE;
  if (!tree->name[0]) wcopy_n(tree->name, kRootEntryName, 32);
  stg_set_bit(in.visited, 0);
  walk = mem_alloc(((SIZE_T)in.ndir * 2 + 2) * sizeof(DWORD));
  pending = mem_alloc(((SIZE_T)in.ndir + 1) * sizeof(CfbPending));
  if (!walk || !pending) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
  pending[0].storage = tree;
  pending[0].tree = stg_rd32(in.dir + 76);
  pending[0].depth = 0;
  npending = 1;
  while (npending && SUCCEEDED(hr)) {
    CfbPending p = pending[--npending];
    hr = cfb_fill_storage(&in, p.storage, p.tree, p.depth, walk, pending, &npending);
  }
  if (SUCCEEDED(hr)) {
    *tree_out = tree;
    tree = NULL;
    *shift_out = in.shift;
    *sig_out = stg_rd32(h + 0x34);
  }
done:
  stg_node_release(tree);
  mem_free(h); mem_free(fatsec); mem_free(walk); mem_free(pending);
  mem_free(in.fat); mem_free(in.minifat); mem_free(in.used); mem_free(in.mused); mem_free(in.dir); mem_free(in.mini); mem_free(in.visited);
  return hr;
}

/* ---- writing a compound file ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct CfbOutEnt {
  StgNode *n;
  DWORD left, right, child, start;
  ULONG depth;
  BYTE color;            /* 0 red, 1 black */
} CfbOutEnt;

/* the entries [lo, hi) (sorted) as a balanced tree: the middle one at the top; the depth of each is recorded for the colors */
static DWORD cfb_build_tree(CfbOutEnt *e, ULONG lo, ULONG hi, ULONG depth, ULONG *maxd) {
  ULONG mid;
  if (lo >= hi) return CFB_NOSTREAM;
  mid = lo + (hi - lo) / 2;
  e[mid].depth = depth;
  if (depth > *maxd) *maxd = depth;
  e[mid].left = cfb_build_tree(e, lo, mid, depth + 1, maxd);
  e[mid].right = cfb_build_tree(e, mid + 1, hi, depth + 1, maxd);
  return mid;
}
static void cfb_chain(DWORD *fat, ULONG first, ULONG count) {
  ULONG i;
  for (i = 0; i < count; i++) fat[first + i] = i + 1 < count ? first + i + 1 : CFB_ENDOFCHAIN;
}
static void cfb_put_entry(BYTE *d, const CfbOutEnt *e, BOOL root, DWORD size_lo, ULONGLONG size, ULONG shift) {
  const StgNode *n = e->n;
  SIZE_T len = wlen(n->name), i;
  if (len > 31) len = 31;
  for (i = 0; i < len; i++) stg_wr16(d + 2 * i, n->name[i]);
  stg_wr16(d + 64, (WORD)((len + 1) * 2));
  d[66] = root ? 5 : n->type == STGTY_STREAM ? 2 : 1;
  d[67] = e->color;
  stg_wr32(d + 68, e->left);
  stg_wr32(d + 72, e->right);
  stg_wr32(d + 76, e->child);
  if (n->type != STGTY_STREAM || root) {
    mem_copy(d + 80, &n->clsid, 16);
    stg_wr32(d + 96, n->state_bits);
    stg_wr32(d + 100, n->ctime.dwLowDateTime); stg_wr32(d + 104, n->ctime.dwHighDateTime);
    stg_wr32(d + 108, n->mtime.dwLowDateTime); stg_wr32(d + 112, n->mtime.dwHighDateTime);
  }
  stg_wr32(d + 116, e->start);
  if (shift == 9) stg_wr32(d + 120, size_lo); else stg_wr64(d + 120, size);
}

/* Writes the tree as a whole compound file over `lb` (and sets its size). */
static HRESULT cfb_write(ILockBytes *lb, StgNode *tree, ULONG shift, DWORD sig) {
  const ULONG ss = 1u << shift, epf = ss / 4, hdr_area = shift == 12 ? 4096 : 512;
  CfbOutEnt *e = NULL;
  BYTE *hdr = NULL, *fatbuf = NULL, *difbuf = NULL, *dirbuf = NULL, *mfbuf = NULL, *minibuf = NULL, *zero = NULL;
  DWORD *fat;
  ULONG count = stg_node_count(tree), filled = 1, i, k;
  ULONGLONG nmini = 0, big = 0, ms, mf, ds, d, f = 1, x = 0, t, next;
  HRESULT hr = S_OK;

  if (count >= 0x00FFFFFFu) return STG_E_INSUFFICIENTMEMORY;
  e = mem_alloc((SIZE_T)count * sizeof(CfbOutEnt));
  if (!e) return STG_E_INSUFFICIENTMEMORY;
  /* the entries: breadth-first, the children of each storage together, as a balanced red-black tree */
  e[0].n = tree;
  e[0].left = e[0].right = e[0].child = CFB_NOSTREAM;
  e[0].color = 1;
  for (i = 0; i < filled; i++) {
    StgNode *n = e[i].n;
    ULONG base = filled, maxd = 0, m = n->nkids;
    if (n->type != STGTY_STORAGE) { e[i].child = CFB_NOSTREAM; continue; }
    for (k = 0; k < m && filled < count; k++) { e[filled].n = n->kids[k]; e[filled].child = CFB_NOSTREAM; filled++; }
    m = filled - base;
    e[i].child = cfb_build_tree(e, base, base + m, 0, &maxd);
    {
      BOOL perfect = maxd < 31 && (ULONGLONG)m == ((1ull << (maxd + 1)) - 1);
      for (k = base; k < base + m; k++) e[k].color = (!perfect && e[k].depth == maxd) ? 0 : 1;
    }
  }
  /* where the streams go */
  for (i = 1; i < filled; i++) {
    StgNode *n = e[i].n;
    if (n->type != STGTY_STREAM || !n->size) { e[i].start = n->type == STGTY_STREAM ? CFB_ENDOFCHAIN : 0; continue; }
    if (n->size < CFB_CUTOFF) { e[i].start = (DWORD)nmini; nmini += (n->size + 63) / 64; }
    else big += (n->size + ss - 1) / ss;
  }
  ms = (nmini * 64 + ss - 1) / ss;
  mf = (nmini * 4 + ss - 1) / ss;
  ds = ((ULONGLONG)filled * 128 + ss - 1) / ss;
  d = big + ms + mf + ds;
  for (;;) {
    ULONGLONG f2, x2;
    t = d + f + x;
    f2 = (t + epf - 1) / epf;
    x2 = f2 > 109 ? (f2 - 109 + (epf - 1) - 1) / (epf - 1) : 0;
    if (f2 <= f && x2 <= x) break;
    f = f2 > f ? f2 : f;
    x = x2 > x ? x2 : x;
  }
  if (t >= 0x0FFFFFF0ull || ((t << shift) + hdr_area) > CFB_MAX_FILE) { mem_free(e); return STG_E_MEDIUMFULL; }

  hdr = mem_alloc(hdr_area);
  fatbuf = mem_alloc((SIZE_T)f * ss);
  difbuf = x ? mem_alloc((SIZE_T)x * ss) : NULL;
  dirbuf = mem_alloc((SIZE_T)ds * ss);
  mfbuf = mf ? mem_alloc((SIZE_T)mf * ss) : NULL;
  minibuf = ms ? mem_alloc((SIZE_T)ms * ss) : NULL;
  zero = mem_alloc(ss);
  if (!hdr || !fatbuf || (x && !difbuf) || !dirbuf || (mf && !mfbuf) || (ms && !minibuf) || !zero) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }

  /* the FAT: [FAT][DIFAT][directory][mini FAT][mini stream][streams] */
  fat = (DWORD *)fatbuf;
  for (i = 0; i < f * epf; i++) fat[i] = CFB_FREESECT;
  for (i = 0; i < f; i++) fat[i] = CFB_FATSECT;
  for (i = 0; i < x; i++) fat[f + i] = CFB_DIFSECT;
  next = f + x;
  cfb_chain(fat, (ULONG)next, (ULONG)ds);
  {
    ULONGLONG dir_first = next, mf_first, ms_first;
    next += ds;
    mf_first = next;
    cfb_chain(fat, (ULONG)next, (ULONG)mf);
    next += mf;
    ms_first = next;
    cfb_chain(fat, (ULONG)next, (ULONG)ms);
    next += ms;
    for (i = 1; i < filled; i++) {
      StgNode *n = e[i].n;
      if (n->type == STGTY_STREAM && n->size >= CFB_CUTOFF) {
        ULONG c = (ULONG)((n->size + ss - 1) / ss);
        e[i].start = (DWORD)next;
        cfb_chain(fat, (ULONG)next, c);
        next += c;
      }
    }
    /* the header */
    mem_copy(hdr, kCfbSignature, 8);
    stg_wr16(hdr + 0x18, 0x003E);
    stg_wr16(hdr + 0x1A, shift == 12 ? 4 : 3);
    stg_wr16(hdr + 0x1C, 0xFFFE);
    stg_wr16(hdr + 0x1E, (WORD)shift);
    stg_wr16(hdr + 0x20, 6);
    stg_wr32(hdr + 0x28, shift == 12 ? (DWORD)ds : 0);
    stg_wr32(hdr + 0x2C, (DWORD)f);
    stg_wr32(hdr + 0x30, (DWORD)dir_first);
    stg_wr32(hdr + 0x34, sig);
    stg_wr32(hdr + 0x38, CFB_CUTOFF);
    stg_wr32(hdr + 0x3C, mf ? (DWORD)mf_first : CFB_ENDOFCHAIN);
    stg_wr32(hdr + 0x40, (DWORD)mf);
    stg_wr32(hdr + 0x44, x ? (DWORD)f : CFB_ENDOFCHAIN);
    stg_wr32(hdr + 0x48, (DWORD)x);
    for (i = 0; i < 109; i++) stg_wr32(hdr + 0x4C + 4 * i, i < f ? i : CFB_FREESECT);
    /* the DIFAT sectors: the FAT sectors after the first 109, and the next DIFAT sector at the end of each */
    for (i = 0; i < x; i++) {
      BYTE *s = difbuf + (SIZE_T)i * ss;
      for (k = 0; k + 1 < epf; k++) {
        ULONGLONG fs = 109 + (ULONGLONG)i * (epf - 1) + k;
        stg_wr32(s + 4 * k, fs < f ? (DWORD)fs : CFB_FREESECT);
      }
      stg_wr32(s + ss - 4, i + 1 < x ? (DWORD)(f + i + 1) : CFB_ENDOFCHAIN);
    }
    /* the mini FAT and the mini stream */
    if (mfbuf) {
      DWORD *mfat = (DWORD *)mfbuf;
      for (i = 0; i < mf * epf; i++) mfat[i] = CFB_FREESECT;
    }
    for (i = 1; i < filled; i++) {
      StgNode *n = e[i].n;
      if (n->type == STGTY_STREAM && n->size && n->size < CFB_CUTOFF) {
        ULONG c = (ULONG)((n->size + 63) / 64);
        cfb_chain((DWORD *)mfbuf, e[i].start, c);
        mem_copy(minibuf + (SIZE_T)e[i].start * 64, n->data, n->size);
      }
    }
    /* the directory */
    for (i = 0; i < ds * (ss / 128); i++) {
      BYTE *de = dirbuf + (SIZE_T)i * 128;
      if (i < filled) {
        StgNode *n = e[i].n;
        if (i == 0) {
          e[0].start = ms ? (DWORD)ms_first : CFB_ENDOFCHAIN;
          cfb_put_entry(de, &e[0], TRUE, (DWORD)(nmini * 64), nmini * 64, shift);
        } else cfb_put_entry(de, &e[i], FALSE, n->type == STGTY_STREAM ? (DWORD)n->size : 0, n->type == STGTY_STREAM ? n->size : 0, shift);
      } else {
        stg_wr32(de + 68, CFB_NOSTREAM);
        stg_wr32(de + 72, CFB_NOSTREAM);
        stg_wr32(de + 76, CFB_NOSTREAM);
      }
    }
    for (i = 0; i < f * epf; i++) stg_wr32(fatbuf + 4 * (SIZE_T)i, fat[i]);
    if (mfbuf) for (i = 0; i < mf * epf; i++) stg_wr32(mfbuf + 4 * (SIZE_T)i, ((DWORD *)mfbuf)[i]);

    /* out */
    hr = cfb_write_at(lb, 0, hdr, hdr_area);
    if (SUCCEEDED(hr)) hr = cfb_write_at(lb, hdr_area, fatbuf, (ULONG)(f * ss));
    if (SUCCEEDED(hr) && x) hr = cfb_write_at(lb, hdr_area + f * ss, difbuf, (ULONG)(x * ss));
    if (SUCCEEDED(hr)) hr = cfb_write_at(lb, hdr_area + dir_first * ss, dirbuf, (ULONG)(ds * ss));
    if (SUCCEEDED(hr) && mf) hr = cfb_write_at(lb, hdr_area + mf_first * ss, mfbuf, (ULONG)(mf * ss));
    if (SUCCEEDED(hr) && ms) hr = cfb_write_at(lb, hdr_area + ms_first * ss, minibuf, (ULONG)(ms * ss));
    for (i = 1; i < filled && SUCCEEDED(hr); i++) {
      StgNode *n = e[i].n;
      if (n->type == STGTY_STREAM && n->size >= CFB_CUTOFF) {
        ULONGLONG off = hdr_area + (ULONGLONG)e[i].start * ss;
        ULONG tail = (ULONG)(n->size % ss);
        hr = cfb_write_at(lb, off, n->data, (ULONG)n->size);
        if (SUCCEEDED(hr) && tail) hr = cfb_write_at(lb, off + n->size, zero, ss - tail);
      }
    }
    if (SUCCEEDED(hr)) {
      ULARGE_INTEGER total;
      total.QuadPart = hdr_area + t * ss;
      hr = ILockBytes_SetSize(lb, total);
    }
  }
done:
  mem_free(e); mem_free(hdr); mem_free(fatbuf); mem_free(difbuf); mem_free(dirbuf); mem_free(mfbuf); mem_free(minibuf); mem_free(zero);
  return hr;
}

/* ---- a byte array on a file ----------------------------------------------------------------------------------------------------------------------------------- */

typedef struct FileLockBytes {
  ILockBytes iface;
  LONG refs;
  CRITICAL_SECTION cs;       /* the file position */
  HANDLE file;
  WCHAR *path;               /* (heap) or NULL */
  BOOL delete_on_release;
} FileLockBytes;

static const ILockBytesVtbl g_flb_vtbl;
static FileLockBytes *flb_of(ILockBytes *l) { return (FileLockBytes *)l; }

static HRESULT WINAPI flb_qi(ILockBytes *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ILockBytes)) { *out = me; ILockBytes_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI flb_addref(ILockBytes *me) { return (ULONG)InterlockedIncrement(&flb_of(me)->refs); }
static ULONG WINAPI flb_release(ILockBytes *me) {
  FileLockBytes *l = flb_of(me);
  LONG refs = InterlockedDecrement(&l->refs);
  if (!refs) {
    CloseHandle(l->file);
    if (l->delete_on_release && l->path) DeleteFileW(l->path);
    DeleteCriticalSection(&l->cs);
    mem_free(l->path);
    mem_free(l);
  }
  return (ULONG)refs;
}
static BOOL flb_seek(FileLockBytes *l, ULONGLONG off) {
  LARGE_INTEGER p;
  if (off > 0x7FFFFFFFFFFFFFFFull) return FALSE;
  p.QuadPart = (LONGLONG)off;
  return SetFilePointerEx(l->file, p, NULL, FILE_BEGIN);
}
static HRESULT WINAPI flb_readat(ILockBytes *me, ULARGE_INTEGER off, void *buf, ULONG n, ULONG *got) {
  FileLockBytes *l = flb_of(me);
  ULONG total = 0;
  HRESULT hr = S_OK;
  if (got) *got = 0;
  if (!buf) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&l->cs);
  if (!flb_seek(l, off.QuadPart)) hr = STG_E_READFAULT;
  while (SUCCEEDED(hr) && total < n) {
    DWORD want = n - total > 0x01000000u ? 0x01000000u : n - total, r = 0;
    if (!ReadFile(l->file, (BYTE *)buf + total, want, &r, NULL)) { hr = STG_E_READFAULT; break; }
    if (!r) break;
    total += r;
  }
  LeaveCriticalSection(&l->cs);
  if (got) *got = total;
  return hr;
}
static HRESULT WINAPI flb_writeat(ILockBytes *me, ULARGE_INTEGER off, const void *buf, ULONG n, ULONG *done) {
  FileLockBytes *l = flb_of(me);
  ULONG total = 0;
  HRESULT hr = S_OK;
  if (done) *done = 0;
  if (!buf) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&l->cs);
  if (!flb_seek(l, off.QuadPart)) hr = STG_E_WRITEFAULT;
  while (SUCCEEDED(hr) && total < n) {
    DWORD want = n - total > 0x01000000u ? 0x01000000u : n - total, w = 0;
    if (!WriteFile(l->file, (const BYTE *)buf + total, want, &w, NULL)) { hr = GetLastError() == ERROR_DISK_FULL ? STG_E_MEDIUMFULL : STG_E_WRITEFAULT; break; }
    if (!w) { hr = STG_E_WRITEFAULT; break; }
    total += w;
  }
  LeaveCriticalSection(&l->cs);
  if (done) *done = total;
  return hr;
}
static HRESULT WINAPI flb_flush(ILockBytes *me) { FlushFileBuffers(flb_of(me)->file); return S_OK; }
static HRESULT WINAPI flb_setsize(ILockBytes *me, ULARGE_INTEGER size) {
  FileLockBytes *l = flb_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&l->cs);
  if (!flb_seek(l, size.QuadPart) || !SetEndOfFile(l->file)) hr = STG_E_MEDIUMFULL;
  LeaveCriticalSection(&l->cs);
  return hr;
}
static HRESULT WINAPI flb_lock(ILockBytes *me, ULARGE_INTEGER off, ULARGE_INTEGER n, DWORD type) {
  FileLockBytes *l = flb_of(me);
  OVERLAPPED ol;
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
  if (type & LOCK_WRITE) return STG_E_INVALIDFUNCTION;
  if (type & (LOCK_EXCLUSIVE | LOCK_ONLYONCE)) flags |= LOCKFILE_EXCLUSIVE_LOCK;
  mem_zero(&ol, sizeof(ol));
  ol.Offset = off.LowPart;
  ol.OffsetHigh = off.HighPart;
  if (LockFileEx(l->file, flags, 0, n.LowPart, n.HighPart, &ol)) return S_OK;
  switch (GetLastError()) {
    case ERROR_LOCK_VIOLATION: case ERROR_SHARING_VIOLATION: return STG_E_LOCKVIOLATION;
    case ERROR_ACCESS_DENIED: return STG_E_ACCESSDENIED;
    default: return S_OK;      /* this file system has no byte-range locks: none are taken */
  }
}
static HRESULT WINAPI flb_unlock(ILockBytes *me, ULARGE_INTEGER off, ULARGE_INTEGER n, DWORD type) {
  FileLockBytes *l = flb_of(me);
  OVERLAPPED ol;
  (void)type;
  mem_zero(&ol, sizeof(ol));
  ol.Offset = off.LowPart;
  ol.OffsetHigh = off.HighPart;
  return UnlockFileEx(l->file, 0, n.LowPart, n.HighPart, &ol) ? S_OK : STG_E_LOCKVIOLATION;
}
static HRESULT WINAPI flb_stat(ILockBytes *me, STATSTG *st, DWORD flags) {
  FileLockBytes *l = flb_of(me);
  LARGE_INTEGER size;
  if (!st) return STG_E_INVALIDPOINTER;
  mem_zero(st, sizeof(*st));
  if (!GetFileSizeEx(l->file, &size)) return STG_E_INVALIDHANDLE;
  st->type = STGTY_LOCKBYTES;
  st->cbSize.QuadPart = (ULONGLONG)size.QuadPart;
  st->grfLocksSupported = LOCK_EXCLUSIVE | LOCK_ONLYONCE;
  if (!(flags & STATFLAG_NONAME) && l->path) {
    st->pwcsName = stg_cotask_dup(l->path);
    if (!st->pwcsName) return STG_E_INSUFFICIENTMEMORY;
  }
  return S_OK;
}
static const ILockBytesVtbl g_flb_vtbl = {flb_qi, flb_addref, flb_release, flb_readat, flb_writeat, flb_flush, flb_setsize, flb_lock, flb_unlock, flb_stat};

/* takes the handle (closed with the byte array) */
static FileLockBytes *flb_new(HANDLE file, const WCHAR *path, BOOL delete_on_release) {
  FileLockBytes *l = mem_alloc(sizeof(*l));
  if (!l) return NULL;
  l->iface.lpVtbl = &g_flb_vtbl;
  l->refs = 1;
  InitializeCriticalSection(&l->cs);
  l->file = file;
  l->path = path ? wdup_heap(path) : NULL;
  l->delete_on_release = delete_on_release;
  return l;
}

static WCHAR *stg_full_path(const WCHAR *name) {
  DWORD n = GetFullPathNameW(name, 0, NULL, NULL);
  WCHAR *buf;
  if (!n || n > 0x8000) return wdup_heap(name);
  buf = mem_alloc(((SIZE_T)n + 1) * sizeof(WCHAR));
  if (!buf) return NULL;
  if (!GetFullPathNameW(name, n + 1, buf, NULL)) { mem_free(buf); return wdup_heap(name); }
  return buf;
}
static HRESULT stg_file_error(DWORD err) {
  switch (err) {
    case ERROR_FILE_NOT_FOUND: return STG_E_FILENOTFOUND;
    case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: return STG_E_PATHNOTFOUND;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return STG_E_ACCESSDENIED;
    case ERROR_SHARING_VIOLATION: return STG_E_SHAREVIOLATION;
    case ERROR_LOCK_VIOLATION: return STG_E_LOCKVIOLATION;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return STG_E_FILEALREADYEXISTS;
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: return STG_E_INVALIDNAME;
    case ERROR_TOO_MANY_OPEN_FILES: return STG_E_TOOMANYOPENFILES;
    case ERROR_DISK_FULL: return STG_E_MEDIUMFULL;
    default: return E_FAIL;
  }
}
/* the file share of Windows: the storage locks do the rest */
static DWORD stg_file_share(DWORD mode) {
  DWORD s = STG_SHARE(mode);
  return (s == STGM_SHARE_DENY_WRITE || s == STGM_SHARE_EXCLUSIVE) ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE;
}
static DWORD stg_file_access(DWORD mode) {
  return (STG_ACCESS(mode) == STGM_READ && !(mode & STGM_DIRECT_SWMR)) ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
}

/* ---- roots and the locks of Windows ------------------------------------------------------------------------------------------------------------------------------ */

#define RL_PRIORITY2_FIRST 0x7FFFFF58u
#define RL_PRIORITY2_LAST 0x7FFFFF6Bu
#define RL_NOSNAPSHOT_FIRST 0x7FFFFF6Cu
#define RL_NOSNAPSHOT_LAST 0x7FFFFF7Fu
#define RL_COMMIT 0x7FFFFF80u
#define RL_PRIORITY1_FIRST 0x7FFFFF81u
#define RL_PRIORITY1_LAST 0x7FFFFF91u
#define RL_CHECKLOCKS 0x7FFFFF92u
#define RL_READ_FIRST 0x7FFFFF93u
#define RL_READ_LAST 0x7FFFFFA6u
#define RL_WRITE_FIRST 0x7FFFFFA7u
#define RL_WRITE_LAST 0x7FFFFFBAu
#define RL_DENY_READ_FIRST 0x7FFFFFBBu
#define RL_DENY_READ_LAST 0x7FFFFFCEu
#define RL_DENY_WRITE_FIRST 0x7FFFFFCFu
#define RL_DENY_WRITE_LAST 0x7FFFFFE2u
#define RL_LAST 0x7FFFFFFFu
#define RL_TRANSACTION_FIRST RL_COMMIT
#define RL_TRANSACTION_LAST RL_CHECKLOCKS

typedef struct StorageObj StorageObj;

typedef struct StgRoot {
  CRITICAL_SECTION cs;       /* the lock of every object of this tree */
  LONG objects;              /* storage, stream and enumerator objects alive (under cs) */
  ILockBytes *lb;
  WCHAR *path;               /* the full name of the file (heap), or NULL */
  ULONG shift;               /* 9 or 12 */
  DWORD sig;                 /* the transaction signature last written or read */
  DWORD locks_supported;     /* of the byte array */
  ULONG locked[8];           /* the bytes this root locked */
  ULONG nlocked;
  BOOL dirty;                /* (direct) changed since the last write */
  StgNode *base;             /* (transacted) the tree as last committed */
  StorageObj *top;           /* the root storage while it is open */
} StgRoot;

static HRESULT stg_lock_region(StgRoot *r, ULONGLONG off, ULONGLONG len, BOOL *supported) {
  ULARGE_INTEGER o, l;
  if (!(r->locks_supported & LOCK_ONLYONCE)) { if (supported) *supported = FALSE; return S_OK; }
  if (supported) *supported = TRUE;
  o.QuadPart = off;
  l.QuadPart = len;
  return ILockBytes_LockRegion(r->lb, o, l, LOCK_ONLYONCE);
}
static void stg_unlock_region(StgRoot *r, ULONGLONG off, ULONGLONG len) {
  ULARGE_INTEGER o, l;
  if (!(r->locks_supported & LOCK_ONLYONCE)) return;
  o.QuadPart = off;
  l.QuadPart = len;
  ILockBytes_UnlockRegion(r->lb, o, l, LOCK_ONLYONCE);
}
/* a lock that others hold only for a moment: waited for (20 s at most, as Windows) */
static HRESULT stg_lock_sync(StgRoot *r, ULONGLONG off, ULONGLONG len, BOOL *supported) {
  DWORD start = GetTickCount(), delay = 0;
  for (;;) {
    HRESULT hr = stg_lock_region(r, off, len, supported);
    if (hr != STG_E_ACCESSDENIED && hr != STG_E_LOCKVIOLATION) return hr;
    if (GetTickCount() - start >= 20000) return hr;
    Sleep(delay);
    if (delay < 150) delay++;
  }
}
static HRESULT stg_check_range(StgRoot *r, ULONG first, ULONG last, HRESULT fail) {
  ULONGLONG len = (ULONGLONG)last - first + 1;
  HRESULT hr = stg_lock_region(r, first, len, NULL);
  if (SUCCEEDED(hr)) stg_unlock_region(r, first, len);
  return FAILED(hr) ? fail : S_OK;
}
static HRESULT stg_lock_one(StgRoot *r, ULONG first, ULONG last) {
  HRESULT hr = S_OK;
  ULONG i;
  for (i = first; i <= last; i++) {
    hr = stg_lock_region(r, i, 1, NULL);
    if (hr != STG_E_ACCESSDENIED && hr != STG_E_LOCKVIOLATION) break;
  }
  if (SUCCEEDED(hr) && r->nlocked < 8) r->locked[r->nlocked++] = i;
  return hr;
}
static void stg_unlock_all(StgRoot *r) {
  ULONG i;
  for (i = 0; i < r->nlocked; i++) stg_unlock_region(r, r->locked[i], 1);
  r->nlocked = 0;
}
/* The locks Windows takes for an open in `mode` after it checked nobody's locks forbid it. */
static HRESULT stg_grab_locks(StgRoot *r, DWORD mode) {
  DWORD share = STG_SHARE(mode), access = STG_ACCESS(mode);
  BOOL supported = FALSE, take;
  HRESULT hr;
  if (mode & STGM_NOSNAPSHOT) {
    if (share == STGM_SHARE_DENY_READ) share = STGM_SHARE_EXCLUSIVE;
    else if (share != STGM_SHARE_EXCLUSIVE) share = STGM_SHARE_DENY_WRITE;
  }
  hr = stg_lock_sync(r, RL_CHECKLOCKS, 1, &supported);
  if (!supported) return S_OK;
  if (FAILED(hr)) return hr;
  hr = S_OK;
  if (mode & STGM_PRIORITY) hr = stg_check_range(r, RL_COMMIT, RL_COMMIT, STG_E_LOCKVIOLATION);
  if (SUCCEEDED(hr) && access != STGM_WRITE) hr = stg_check_range(r, RL_DENY_READ_FIRST, RL_DENY_READ_LAST, STG_E_SHAREVIOLATION);
  if (SUCCEEDED(hr) && access != STGM_READ) hr = stg_check_range(r, RL_DENY_WRITE_FIRST, RL_DENY_WRITE_LAST, STG_E_SHAREVIOLATION);
  if (SUCCEEDED(hr) && (share == STGM_SHARE_DENY_READ || share == STGM_SHARE_EXCLUSIVE)) hr = stg_check_range(r, RL_READ_FIRST, RL_READ_LAST, STG_E_LOCKVIOLATION);
  if (SUCCEEDED(hr) && (share == STGM_SHARE_DENY_WRITE || share == STGM_SHARE_EXCLUSIVE)) hr = stg_check_range(r, RL_WRITE_FIRST, RL_WRITE_LAST, STG_E_LOCKVIOLATION);
  if (SUCCEEDED(hr) && access == STGM_READ && share == STGM_SHARE_EXCLUSIVE) {
    hr = stg_check_range(r, 0, RL_CHECKLOCKS - 1, STG_E_LOCKVIOLATION);
    if (SUCCEEDED(hr)) hr = stg_check_range(r, RL_CHECKLOCKS + 1, RL_LAST, STG_E_LOCKVIOLATION);
  }
  /* a direct reader that denies writing relies on the file share alone */
  take = (mode & STGM_TRANSACTED) || (mode & STGM_PRIORITY) || access != STGM_READ || share != STGM_SHARE_DENY_WRITE;
  if (take) {
    if (SUCCEEDED(hr) && (mode & STGM_PRIORITY)) {
      hr = stg_lock_one(r, RL_PRIORITY1_FIRST, RL_PRIORITY1_LAST);
      if (SUCCEEDED(hr)) hr = stg_lock_one(r, RL_PRIORITY2_FIRST, RL_PRIORITY2_LAST);
    }
    if (SUCCEEDED(hr) && access != STGM_WRITE) hr = stg_lock_one(r, RL_READ_FIRST, RL_READ_LAST);
    if (SUCCEEDED(hr) && access != STGM_READ) hr = stg_lock_one(r, RL_WRITE_FIRST, RL_WRITE_LAST);
    if (SUCCEEDED(hr) && (share == STGM_SHARE_DENY_READ || share == STGM_SHARE_EXCLUSIVE)) hr = stg_lock_one(r, RL_DENY_READ_FIRST, RL_DENY_READ_LAST);
    if (SUCCEEDED(hr) && (share == STGM_SHARE_DENY_WRITE || share == STGM_SHARE_EXCLUSIVE)) hr = stg_lock_one(r, RL_DENY_WRITE_FIRST, RL_DENY_WRITE_LAST);
    if (SUCCEEDED(hr) && (mode & STGM_NOSNAPSHOT)) hr = stg_lock_one(r, RL_NOSNAPSHOT_FIRST, RL_NOSNAPSHOT_LAST);
  }
  stg_unlock_region(r, RL_CHECKLOCKS, 1);
  return hr;
}

static void stg_root_free(StgRoot *r) {
  DeleteCriticalSection(&r->cs);
  stg_node_release(r->base);
  mem_free(r->path);
  mem_free(r);
}

/* ---- the storage object ---------------------------------------------------------------------------------------------------------------------------------------- */

struct StorageObj {
  IStorage iface;
  IDirectWriterLock dwl;
  IRootStorage rootstg;
  LONG refs;                 /* the references of the caller */
  LONG mem;                  /* holds on the memory (under the lock): 1 while open, 1 per child object and enumerator */
  LONG enums;                /* enumerators keep the storage open */
  BOOL closed, reverted;
  StorageObj *parent;
  StgRoot *root;
  DWORD mode;                /* as Stat reports it */
  BOOL transacted, simple, swmr_writer;
  StgNode *node;             /* the tree it works on (the private copy of a transacted storage) */
  StgNode *base;             /* transacted, not the root: the element in the parent */
  ULONG depth;
};

typedef struct StreamObj {
  IStream iface;
  LONG refs;
  StorageObj *parent;
  StgRoot *root;
  StgNode *node;
  DWORD mode;
  ULONGLONG pos;
  BOOL simple;               /* created in a simple-mode storage: Stat reports the position, at least 4096 bytes are kept */
} StreamObj;

typedef struct EnumObj {
  IEnumSTATSTG iface;
  LONG refs;
  StorageObj *stg;
  StgRoot *root;
  WCHAR last[32];
  BOOL started;
} EnumObj;

static const IStorageVtbl g_stg_vtbl;
static const IStreamVtbl g_stm_vtbl;
static const IEnumSTATSTGVtbl g_stgenum_vtbl;
static const IDirectWriterLockVtbl g_dwl_vtbl;
static const IRootStorageVtbl g_rootstg_vtbl;

static StorageObj *stg_of(IStorage *s) { return (StorageObj *)s; }
static StorageObj *stg_from_dwl(IDirectWriterLock *d) { return (StorageObj *)((BYTE *)d - offsetof(StorageObj, dwl)); }
static StorageObj *stg_from_rootstg(IRootStorage *r) { return (StorageObj *)((BYTE *)r - offsetof(StorageObj, rootstg)); }
static StreamObj *stm_of(IStream *s) { return (StreamObj *)s; }
static EnumObj *stgenum_of(IEnumSTATSTG *e) { return (EnumObj *)e; }
/* one of ours, or NULL */
static StorageObj *stg_ours(IStorage *s) { return s && s->lpVtbl == &g_stg_vtbl ? stg_of(s) : NULL; }

static BOOL stg_alive(const StorageObj *s) {
  for (; s; s = s->parent) {
    const StgNode *n;
    if (s->closed || s->reverted) return FALSE;
    n = (s->parent && s->transacted) ? s->base : s->node;
    if (!n || n->removed) return FALSE;
  }
  return TRUE;
}
static BOOL stg_writable(const StorageObj *s) { return s->transacted || STG_ACCESS(s->mode) != STGM_READ; }
static BOOL stg_direct_chain(const StorageObj *s) { for (; s; s = s->parent) if (s->transacted) return FALSE; return TRUE; }
static void stg_touch(StorageObj *s) { s->root->dirty = TRUE; }

static StorageObj *stg_obj_new(StgRoot *r, StorageObj *parent, DWORD mode) {
  StorageObj *s = mem_alloc(sizeof(*s));
  if (!s) return NULL;
  s->iface.lpVtbl = &g_stg_vtbl;
  s->dwl.lpVtbl = &g_dwl_vtbl;
  s->rootstg.lpVtbl = &g_rootstg_vtbl;
  s->refs = 1;
  s->mem = 1;
  s->root = r;
  s->parent = parent;
  s->mode = mode;
  r->objects++;
  if (parent) {
    parent->mem++;
    s->depth = parent->depth + 1;
    s->simple = parent->simple;
  }
  return s;
}
/* drops a hold on the memory of `s` (and so on up the parents whose last hold it was) */
static void stg_unhold_locked(StorageObj *s) {
  while (s && --s->mem == 0) {
    StorageObj *p = s->parent;
    s->root->objects--;
    mem_free(s);
    s = p;
  }
}
static HRESULT stg_flush_locked(StgRoot *r) {
  HRESULT hr;
  if (!r->top || !r->lb || !r->top->node) return S_OK;
  hr = cfb_write(r->lb, r->top->node, r->shift, r->sig);
  if (SUCCEEDED(hr)) { r->dirty = FALSE; ILockBytes_Flush(r->lb); }
  return hr;
}
/* the last reference went: a direct root writes its file, a transacted one forgets its changes, the element is no longer open */
static void stg_close_locked(StorageObj *s) {
  StgRoot *r = s->root;
  if (s->closed) return;
  s->closed = TRUE;
  if (!s->parent) {
    if (!s->transacted && STG_ACCESS(s->mode) != STGM_READ && r->dirty) stg_flush_locked(r);
    r->top = NULL;
    if (r->lb) {
      stg_unlock_all(r);
      ILockBytes_Release(r->lb);
      r->lb = NULL;
    }
    stg_node_mark_removed(s->node);
    stg_node_release(s->node);
    stg_node_release(r->base);
    r->base = NULL;
  } else if (s->transacted) {
    stg_node_mark_removed(s->node);
    stg_node_release(s->node);
    s->base->open_count--;
    stg_node_release(s->base);
  } else {
    s->node->open_count--;
    stg_node_release(s->node);
  }
  s->node = s->base = NULL;
  stg_unhold_locked(s);
}
/* leaves the lock and frees the root when nothing of it is left */
static void stg_leave(StgRoot *r) {
  LONG left = r->objects;
  LeaveCriticalSection(&r->cs);
  if (!left) stg_root_free(r);
}

static HRESULT stg_open_child_locked(StorageObj *s, StgNode *n, DWORD mode, IStorage **out) {
  StorageObj *c = stg_obj_new(s->root, s, mode & ~(DWORD)STGM_CREATE);
  if (!c) return STG_E_INSUFFICIENTMEMORY;
  if (mode & STGM_TRANSACTED) {
    StgNode *work = stg_node_clone(n);
    if (!work) { c->mem = 1; stg_unhold_locked(c); return STG_E_INSUFFICIENTMEMORY; }
    c->transacted = TRUE;
    c->node = work;
    c->base = n;
  } else c->node = n;
  n->refs++;
  n->open_count++;
  *out = &c->iface;
  return S_OK;
}
static HRESULT stm_open_locked(StorageObj *s, StgNode *n, DWORD mode, BOOL simple, IStream **out) {
  StreamObj *m = mem_alloc(sizeof(*m));
  if (!m) return STG_E_INSUFFICIENTMEMORY;
  m->iface.lpVtbl = &g_stm_vtbl;
  m->refs = 1;
  m->parent = s;
  m->root = s->root;
  m->node = n;
  m->mode = mode & ~(DWORD)STGM_CREATE;
  m->simple = simple;
  n->refs++;
  n->open_count++;
  s->mem++;
  s->root->objects++;
  *out = &m->iface;
  return S_OK;
}
static void stg_fill_stat(const StgNode *n, STATSTG *st, DWORD mode) {
  st->type = n->type;
  if (n->type == STGTY_STREAM) st->cbSize.QuadPart = n->size;
  else {
    st->mtime = n->mtime;
    st->ctime = n->ctime;
    st->clsid = n->clsid;
    st->grfStateBits = n->state_bits;
  }
  st->grfMode = mode;
}

static HRESULT WINAPI stg_qi(IStorage *me, REFIID riid, void **out) {
  StorageObj *s = stg_of(me);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IStorage)) *out = &s->iface;
  else if (IsEqualGUID(riid, &IID_IPropertySetStorage)) return propsetstg_create(me, (IPropertySetStorage **)out);
  else if (IsEqualGUID(riid, &IID_IRootStorage) && !s->parent) *out = &s->rootstg;
  else if (IsEqualGUID(riid, &IID_IDirectWriterLock) && s->swmr_writer) *out = &s->dwl;
  else return E_NOINTERFACE;
  IStorage_AddRef(me);
  return S_OK;
}
static ULONG WINAPI stg_addref(IStorage *me) { return (ULONG)InterlockedIncrement(&stg_of(me)->refs); }
static ULONG WINAPI stg_release(IStorage *me) {
  StorageObj *s = stg_of(me);
  LONG refs = InterlockedDecrement(&s->refs);
  if (refs == 0) {
    StgRoot *r = s->root;
    EnterCriticalSection(&r->cs);
    if (!s->closed && s->enums == 0) stg_close_locked(s);
    stg_leave(r);
  }
  return refs < 0 ? 0 : (ULONG)refs;
}

static HRESULT WINAPI stg_create_stream(IStorage *me, LPCOLESTR name, DWORD mode, DWORD r1, DWORD r2, IStream **out) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  ULONG at;
  HRESULT hr;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (!name) return STG_E_INVALIDNAME;
  if (r1 || r2) return STG_E_INVALIDPARAMETER;
  if (FAILED(stg_validate_mode(mode)) || STG_SHARE(mode) != STGM_SHARE_EXCLUSIVE) return STG_E_INVALIDFLAG;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (mode & (STGM_DELETEONRELEASE | STGM_TRANSACTED)) hr = STG_E_INVALIDFUNCTION;
  else if (!s->transacted && STG_ACCESS(mode) > STG_ACCESS(s->mode)) hr = STG_E_ACCESSDENIED;
  else if (s->simple && (mode & STGM_CREATE)) hr = STG_E_INVALIDFLAG;
  else if (FAILED(hr = stg_check_name(name))) {}
  else {
    n = stg_node_find(s->node, name, &at);
    if (n) {
      if (STG_CREATEMODE(mode) != STGM_CREATE) hr = STG_E_FILEALREADYEXISTS;
      else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
      else {
        n = stg_node_take(s->node, at);
        stg_node_mark_removed(n);
        stg_node_release(n);
      }
    } else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
    if (SUCCEEDED(hr)) {
      n = stg_node_new(STGTY_STREAM, name);
      if (!n) hr = STG_E_INSUFFICIENTMEMORY;
      else if (FAILED(hr = stg_node_insert(s->node, n, at))) stg_node_release(n);
      else {
        stg_touch(s);
        hr = stm_open_locked(s, n, mode, s->simple, out);
      }
    }
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_open_stream(IStorage *me, LPCOLESTR name, void *r1, DWORD mode, DWORD r2, IStream **out) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  HRESULT hr;
  if (!name || !out) return E_INVALIDARG;
  *out = NULL;
  if (r1 || r2) return STG_E_INVALIDPARAMETER;
  if (FAILED(stg_validate_mode(mode)) || STG_SHARE(mode) != STGM_SHARE_EXCLUSIVE) return STG_E_INVALIDFLAG;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (mode & (STGM_DELETEONRELEASE | STGM_TRANSACTED)) hr = STG_E_INVALIDFUNCTION;
  else if (!s->transacted && STG_ACCESS(mode) > STG_ACCESS(s->mode)) hr = STG_E_INVALIDFLAG;
  else if (!(n = stg_node_find(s->node, name, NULL)) || n->type != STGTY_STREAM) hr = STG_E_FILENOTFOUND;
  else if (n->open_count) hr = STG_E_ACCESSDENIED;
  else hr = stm_open_locked(s, n, mode, FALSE, out);
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_create_storage(IStorage *me, LPCOLESTR name, DWORD mode, DWORD r1, DWORD r2, IStorage **out) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  ULONG at;
  HRESULT hr;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (s->simple) return STG_E_INVALIDFUNCTION;
  if (!name) return STG_E_INVALIDNAME;
  if (FAILED(stg_validate_mode(mode)) || (mode & STGM_DELETEONRELEASE) || STG_SHARE(mode) != STGM_SHARE_EXCLUSIVE) return STG_E_INVALIDFLAG;
  (void)r1; (void)r2;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!s->transacted && STG_ACCESS(mode) > STG_ACCESS(s->mode)) hr = STG_E_ACCESSDENIED;
  else if (FAILED(hr = stg_check_name(name))) {}
  else if (s->depth + 1 >= STG_MAX_DEPTH) hr = STG_E_INSUFFICIENTMEMORY;
  else {
    n = stg_node_find(s->node, name, &at);
    if (n) {
      if (STG_CREATEMODE(mode) == STGM_CREATE && stg_writable(s)) {
        n = stg_node_take(s->node, at);
        stg_node_mark_removed(n);
        stg_node_release(n);
      } else hr = STG_E_FILEALREADYEXISTS;
    } else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
    if (SUCCEEDED(hr)) {
      n = stg_node_new(STGTY_STORAGE, name);
      if (!n) hr = STG_E_INSUFFICIENTMEMORY;
      else if (FAILED(hr = stg_node_insert(s->node, n, at))) stg_node_release(n);
      else {
        GetSystemTimeAsFileTime(&n->ctime);
        n->mtime = n->ctime;
        stg_touch(s);
        hr = stg_open_child_locked(s, n, mode, out);
      }
    }
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_open_storage(IStorage *me, LPCOLESTR name, IStorage *prio, DWORD mode, SNB snb, DWORD reserved, IStorage **out) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  HRESULT hr;
  (void)prio;
  if (!name || !out) return E_INVALIDARG;
  *out = NULL;
  if (s->simple) return STG_E_INVALIDFUNCTION;
  if (snb || reserved) return STG_E_INVALIDPARAMETER;
  if (FAILED(stg_validate_mode(mode))) return STG_E_INVALIDFLAG;
  if (STG_SHARE(mode) != STGM_SHARE_EXCLUSIVE || (mode & (STGM_DELETEONRELEASE | STGM_PRIORITY))) return STG_E_INVALIDFUNCTION;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!s->transacted && STG_ACCESS(mode) > STG_ACCESS(s->mode)) hr = STG_E_ACCESSDENIED;
  else if (!(n = stg_node_find(s->node, name, NULL)) || n->type != STGTY_STORAGE) hr = STG_E_FILENOTFOUND;
  else if (n->open_count) hr = STG_E_ACCESSDENIED;
  else hr = stg_open_child_locked(s, n, mode, out);
  stg_leave(s->root);
  return hr;
}

/* Copies a (private) tree into a storage through its interface: classes, state bits, storages merged into existing ones, streams replaced. */
static BOOL stg_in_snb(SNB snb, const WCHAR *name) {
  ULONG i;
  if (!snb) return FALSE;
  for (i = 0; snb[i] && i < 0x10000; i++) if (!stg_name_cmp(snb[i], name)) return TRUE;
  return FALSE;
}
static HRESULT stg_write_stream_to(IStorage *dest, const WCHAR *name, const StgNode *n, DWORD create) {
  IStream *stm;
  HRESULT hr = IStorage_CreateStream(dest, name, create | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stm);
  if (FAILED(hr)) return hr;
  if (n->size) hr = stream_write_all(stm, n->data, (ULONG)n->size);
  IStream_Release(stm);
  return hr;
}
static HRESULT stg_copy_tree_to(const StgNode *src, IStorage *dest, BOOL skip_storages, BOOL skip_streams, SNB snb) {
  ULONG i;
  HRESULT hr = IStorage_SetClass(dest, &src->clsid);
  if (SUCCEEDED(hr)) IStorage_SetStateBits(dest, src->state_bits, 0xFFFFFFFFu);
  for (i = 0; i < src->nkids && SUCCEEDED(hr); i++) {
    const StgNode *k = src->kids[i];
    if (stg_in_snb(snb, k->name)) continue;
    if (k->type == STGTY_STORAGE) {
      IStorage *sub;
      if (skip_storages) continue;
      hr = IStorage_CreateStorage(dest, k->name, STGM_FAILIFTHERE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &sub);
      if (hr == STG_E_FILEALREADYEXISTS) hr = IStorage_OpenStorage(dest, k->name, NULL, STGM_WRITE | STGM_SHARE_EXCLUSIVE, NULL, 0, &sub);
      if (FAILED(hr)) break;
      hr = stg_copy_tree_to(k, sub, FALSE, skip_streams, NULL);
      IStorage_Release(sub);
    } else {
      if (skip_streams) continue;
      hr = stg_write_stream_to(dest, k->name, k, STGM_CREATE);
    }
  }
  return hr;
}
/* is `d` (one of ours) `s` or inside it? */
static BOOL stg_is_within(const StorageObj *d, const StorageObj *s) {
  for (; d; d = d->parent) if (d == s) return TRUE;
  return FALSE;
}

static HRESULT WINAPI stg_copyto(IStorage *me, DWORD ciid, const IID *iids, SNB snb, IStorage *dest) {
  StorageObj *s = stg_of(me), *d = stg_ours(dest);
  BOOL skip_storages = FALSE, skip_streams = FALSE;
  StgNode *snap = NULL;
  HRESULT hr = S_OK;
  ULONG i;
  if (!dest) return STG_E_INVALIDPOINTER;
  if (ciid && !iids) return STG_E_INVALIDPOINTER;
  for (i = 0; i < ciid; i++) {
    if (IsEqualGUID(&iids[i], &IID_IStorage)) skip_storages = TRUE;
    else if (IsEqualGUID(&iids[i], &IID_IStream)) skip_streams = TRUE;
  }
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (d && d->root == s->root && stg_is_within(d, s) && !skip_storages) hr = STG_E_ACCESSDENIED;
  else if (!(snap = stg_node_clone(s->node))) hr = STG_E_INSUFFICIENTMEMORY;
  stg_leave(s->root);
  if (FAILED(hr)) return hr;
  hr = stg_copy_tree_to(snap, dest, skip_storages, skip_streams, snb);
  stg_node_release(snap);
  return hr;
}

static HRESULT WINAPI stg_move_element_to(IStorage *me, LPCOLESTR name, IStorage *dest, LPCOLESTR newname, DWORD flags) {
  StorageObj *s = stg_of(me), *d = stg_ours(dest);
  StgNode *n, *snap = NULL;
  HRESULT hr = S_OK;
  if (!name) return STG_E_INVALIDNAME;
  if (flags != STGMOVE_MOVE && flags != STGMOVE_COPY) return STG_E_INVALIDFLAG;
  if (!newname) return STG_E_INVALIDNAME;
  if (!dest) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!(n = stg_node_find(s->node, name, NULL))) hr = STG_E_FILENOTFOUND;
  else if (d == s && !stg_name_cmp(name, newname)) hr = STG_E_ACCESSDENIED;
  else if (flags == STGMOVE_MOVE && (n->open_count || !stg_writable(s))) hr = STG_E_ACCESSDENIED;
  else {
    if (d && d->root == s->root) {
      const StorageObj *a;
      for (a = d; a; a = a->parent) if (((a->parent && a->transacted) ? a->base : a->node) == n) hr = STG_E_ACCESSDENIED;
    }
    if (SUCCEEDED(hr) && !(snap = stg_node_clone(n))) hr = STG_E_INSUFFICIENTMEMORY;
  }
  stg_leave(s->root);
  if (FAILED(hr)) return hr;
  {
    DWORD create = flags == STGMOVE_MOVE ? STGM_FAILIFTHERE : STGM_CREATE;
    if (snap->type == STGTY_STREAM) hr = stg_write_stream_to(dest, newname, snap, create);
    else {
      IStorage *sub;
      hr = IStorage_CreateStorage(dest, newname, create | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &sub);
      if (SUCCEEDED(hr)) {
        hr = stg_copy_tree_to(snap, sub, FALSE, FALSE, NULL);
        IStorage_Release(sub);
      }
    }
  }
  stg_node_release(snap);
  if (SUCCEEDED(hr) && flags == STGMOVE_MOVE) hr = IStorage_DestroyElement(me, name);
  return hr;
}

/* the commit of a transacted root: the whole tree into the byte array, unless someone else committed since and STGC_ONLYIFCURRENT asks */
static HRESULT stg_commit_root_locked(StorageObj *s, DWORD flags) {
  StgRoot *r = s->root;
  BOOL locked = FALSE, supported = FALSE;
  DWORD newsig = r->sig + 1;
  StgNode *nb;
  HRESULT hr;
  if (!r->lb) return STG_E_REVERTED;
  if (r->locks_supported & LOCK_ONLYONCE) {
    BYTE h[512];
    ULONG got = 0;
    hr = stg_lock_sync(r, RL_TRANSACTION_FIRST, RL_TRANSACTION_LAST - RL_TRANSACTION_FIRST + 1, &supported);
    if (FAILED(hr)) return hr;
    locked = supported;
    hr = cfb_read_at(r->lb, 0, h, sizeof(h), &got);
    if (SUCCEEDED(hr)) {
      DWORD filesig = got >= 0x38 ? stg_rd32(h + 0x34) : 0;
      ULONG shift;
      if (filesig != r->sig && (got < 512 || FAILED(cfb_check_header(h, &shift)))) hr = STG_E_INVALIDHEADER;
      else if ((flags & STGC_ONLYIFCURRENT) && filesig != r->sig) hr = STG_E_NOTCURRENT;
      else newsig = filesig + 1;
    }
    if (FAILED(hr)) goto done;
  }
  hr = cfb_write(r->lb, s->node, r->shift, newsig);
  if (FAILED(hr)) goto done;
  r->sig = newsig;
  ILockBytes_Flush(r->lb);
  nb = stg_node_clone(s->node);
  if (nb) { stg_node_release(r->base); r->base = nb; }
  else hr = STG_E_INSUFFICIENTMEMORY;
done:
  if (locked) stg_unlock_region(r, RL_TRANSACTION_FIRST, RL_TRANSACTION_LAST - RL_TRANSACTION_FIRST + 1);
  return hr;
}

static HRESULT WINAPI stg_commit(IStorage *me, DWORD flags) {
  StorageObj *s = stg_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (s->transacted && STG_ACCESS(s->mode) == STGM_READ) hr = STG_E_ACCESSDENIED;
  else if (!s->parent && s->transacted) hr = stg_commit_root_locked(s, flags);
  else {
    if (s->transacted) {
      hr = stg_node_assign(s->base, s->node);
      if (SUCCEEDED(hr)) stg_touch(s);
    }
    if (SUCCEEDED(hr) && stg_direct_chain(s->transacted ? s->parent : s) && s->root->dirty && STG_ACCESS(s->root->top ? s->root->top->mode : 0) != STGM_READ)
      hr = stg_flush_locked(s->root);
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_revert(IStorage *me) {
  StorageObj *s = stg_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (s->transacted) {
    StgNode *base = s->parent ? s->base : s->root->base;
    StgNode *fresh = base ? stg_node_clone(base) : NULL;
    if (!fresh) hr = STG_E_INSUFFICIENTMEMORY;
    else {
      stg_node_mark_removed(s->node);
      stg_node_release(s->node);
      s->node = fresh;
    }
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_enum_elements(IStorage *me, DWORD r1, void *r2, DWORD r3, IEnumSTATSTG **out) {
  StorageObj *s = stg_of(me);
  EnumObj *e;
  HRESULT hr = S_OK;
  (void)r1; (void)r2; (void)r3;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!(e = mem_alloc(sizeof(*e)))) hr = STG_E_INSUFFICIENTMEMORY;
  else {
    e->iface.lpVtbl = &g_stgenum_vtbl;
    e->refs = 1;
    e->stg = s;
    e->root = s->root;
    s->enums++;
    s->mem++;
    s->root->objects++;
    *out = &e->iface;
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_destroy_element(IStorage *me, LPCOLESTR name) {
  StorageObj *s = stg_of(me);
  ULONG at;
  HRESULT hr = S_OK;
  if (!name) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
  else if (!stg_node_find(s->node, name, &at)) hr = STG_E_FILENOTFOUND;
  else {
    StgNode *n = stg_node_take(s->node, at);
    stg_node_mark_removed(n);
    stg_node_release(n);
    stg_touch(s);
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_rename_element(IStorage *me, LPCOLESTR oldname, LPCOLESTR newname) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  ULONG at, to;
  HRESULT hr = S_OK;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (FAILED(hr = stg_check_name(newname))) {}
  else if (!oldname) hr = STG_E_INVALIDNAME;
  else if (stg_node_find(s->node, newname, NULL)) hr = STG_E_FILEALREADYEXISTS;
  else if (!(n = stg_node_find(s->node, oldname, &at))) hr = STG_E_FILENOTFOUND;
  else if (n->open_count || !stg_writable(s)) hr = STG_E_ACCESSDENIED;
  else {
    stg_node_take(s->node, at);
    wcopy_n(n->name, newname, 32);
    stg_node_find(s->node, n->name, &to);
    hr = stg_node_insert(s->node, n, to);      /* (cannot fail: the array had room for it) */
    if (FAILED(hr)) { stg_node_mark_removed(n); stg_node_release(n); }
    stg_touch(s);
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_set_element_times(IStorage *me, LPCOLESTR name, const FILETIME *c, const FILETIME *a, const FILETIME *m) {
  StorageObj *s = stg_of(me);
  StgNode *n;
  HRESULT hr = S_OK;
  (void)a;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!(n = name ? stg_node_find(s->node, name, NULL) : s->node)) hr = STG_E_FILENOTFOUND;
  else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
  else {
    if (c) n->ctime = *c;
    if (m) n->mtime = *m;
    stg_touch(s);
  }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_set_class(IStorage *me, REFCLSID clsid) {
  StorageObj *s = stg_of(me);
  HRESULT hr = S_OK;
  if (!clsid) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
  else { s->node->clsid = *clsid; stg_touch(s); }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_set_state_bits(IStorage *me, DWORD bits, DWORD mask) {
  StorageObj *s = stg_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else if (!stg_writable(s)) hr = STG_E_ACCESSDENIED;
  else { s->node->state_bits = (s->node->state_bits & ~mask) | (bits & mask); stg_touch(s); }
  stg_leave(s->root);
  return hr;
}

static HRESULT WINAPI stg_stat(IStorage *me, STATSTG *st, DWORD flags) {
  StorageObj *s = stg_of(me);
  HRESULT hr = S_OK;
  if (!st) return STG_E_INVALIDPOINTER;
  if (flags > STATFLAG_NOOPEN) return STG_E_INVALIDFLAG;
  EnterCriticalSection(&s->root->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else {
    mem_zero(st, sizeof(*st));
    stg_fill_stat(s->node, st, s->mode);
    if (!(flags & STATFLAG_NONAME)) {
      const WCHAR *name = s->parent ? s->node->name : s->root->path;
      if (name && !(st->pwcsName = stg_cotask_dup(name))) hr = STG_E_INSUFFICIENTMEMORY;
    }
  }
  stg_leave(s->root);
  return hr;
}

static const IStorageVtbl g_stg_vtbl = {stg_qi, stg_addref, stg_release, stg_create_stream, stg_open_stream, stg_create_storage, stg_open_storage, stg_copyto,
                                        stg_move_element_to, stg_commit, stg_revert, stg_enum_elements, stg_destroy_element, stg_rename_element, stg_set_element_times,
                                        stg_set_class, stg_set_state_bits, stg_stat};

/* IDirectWriterLock (a direct root opened for single-writer, multiple-reader access): this process is the writer for as long as it has the storage open */
static HRESULT WINAPI dwl_qi(IDirectWriterLock *me, REFIID riid, void **out) { return IStorage_QueryInterface(&stg_from_dwl(me)->iface, riid, out); }
static ULONG WINAPI dwl_addref(IDirectWriterLock *me) { return IStorage_AddRef(&stg_from_dwl(me)->iface); }
static ULONG WINAPI dwl_release(IDirectWriterLock *me) { return IStorage_Release(&stg_from_dwl(me)->iface); }
static HRESULT WINAPI dwl_wait(IDirectWriterLock *me, DWORD timeout) { (void)me; (void)timeout; return S_OK; }
static HRESULT WINAPI dwl_release_access(IDirectWriterLock *me) { (void)me; return S_OK; }
static HRESULT WINAPI dwl_have_access(IDirectWriterLock *me) { (void)me; return S_OK; }
static const IDirectWriterLockVtbl g_dwl_vtbl = {dwl_qi, dwl_addref, dwl_release, dwl_wait, dwl_release_access, dwl_have_access};

/* IRootStorage: the storage goes on in a copy of its file under another name */
static HRESULT WINAPI rootstg_qi(IRootStorage *me, REFIID riid, void **out) { return IStorage_QueryInterface(&stg_from_rootstg(me)->iface, riid, out); }
static ULONG WINAPI rootstg_addref(IRootStorage *me) { return IStorage_AddRef(&stg_from_rootstg(me)->iface); }
static ULONG WINAPI rootstg_release(IRootStorage *me) { return IStorage_Release(&stg_from_rootstg(me)->iface); }
static HRESULT WINAPI rootstg_switch(IRootStorage *me, LPOLESTR name) {
  StorageObj *s = stg_from_rootstg(me);
  StgRoot *r = s->root;
  HANDLE file;
  FileLockBytes *l;
  WCHAR *full;
  HRESULT hr;
  if (!name) return STG_E_INVALIDPOINTER;
  full = stg_full_path(name);
  if (!full) return STG_E_INSUFFICIENTMEMORY;
  file = CreateFileW(full, stg_file_access(s->mode), stg_file_share(s->mode), NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) { hr = stg_file_error(GetLastError()); mem_free(full); return hr; }
  l = flb_new(file, full, FALSE);
  if (!l) { CloseHandle(file); mem_free(full); return STG_E_INSUFFICIENTMEMORY; }
  EnterCriticalSection(&r->cs);
  if (!stg_alive(s)) hr = STG_E_REVERTED;
  else hr = cfb_write(&l->iface, s->transacted ? r->base : s->node, r->shift, r->sig);
  if (SUCCEEDED(hr)) {
    stg_unlock_all(r);
    ILockBytes_Release(r->lb);
    r->lb = &l->iface;
    l = NULL;
    mem_free(r->path);
    r->path = full;
    full = NULL;
    stg_grab_locks(r, s->mode);
  }
  stg_leave(r);
  if (l) ILockBytes_Release(&l->iface);
  mem_free(full);
  return hr;
}
static const IRootStorageVtbl g_rootstg_vtbl = {rootstg_qi, rootstg_addref, rootstg_release, rootstg_switch};

/* ---- the stream object ----------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL stm_alive(const StreamObj *m) { return !m->node->removed && stg_alive(m->parent); }

static HRESULT WINAPI stm_qi(IStream *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream)) {
    *out = me;
    IStream_AddRef(me);
    return S_OK;
  }
  return E_NOINTERFACE;
}
static ULONG WINAPI stm_addref(IStream *me) { return (ULONG)InterlockedIncrement(&stm_of(me)->refs); }
static ULONG WINAPI stm_release(IStream *me) {
  StreamObj *m = stm_of(me);
  LONG refs = InterlockedDecrement(&m->refs);
  if (refs == 0) {
    StgRoot *r = m->root;
    EnterCriticalSection(&r->cs);
    if (m->simple && stm_alive(m) && m->node->size < 4096 && SUCCEEDED(stg_node_resize(m->node, 4096))) stg_touch(m->parent);
    m->node->open_count--;
    stg_node_release(m->node);
    stg_unhold_locked(m->parent);
    r->objects--;
    mem_free(m);
    stg_leave(r);
  }
  return refs < 0 ? 0 : (ULONG)refs;
}
static HRESULT WINAPI stm_read(IStream *me, void *buf, ULONG n, ULONG *got) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  ULONG have = 0;
  if (got) *got = 0;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else if (!buf) hr = STG_E_INVALIDPOINTER;
  else if (STG_ACCESS(m->mode) == STGM_WRITE) hr = STG_E_ACCESSDENIED;
  else {
    if (m->pos < m->node->size) {
      SIZE_T left = m->node->size - (SIZE_T)m->pos;
      have = left < n ? (ULONG)left : n;
      mem_copy(buf, m->node->data + m->pos, have);
      m->pos += have;
    }
    if (got) *got = have;
  }
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_write(IStream *me, const void *buf, ULONG n, ULONG *done) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  if (done) *done = 0;
  if (!buf) return STG_E_INVALIDPOINTER;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else if (STG_ACCESS(m->mode) == STGM_READ) hr = STG_E_ACCESSDENIED;
  else if (n) {
    if (m->pos > STG_MAX_STREAM || n > STG_MAX_STREAM - m->pos) hr = STG_E_MEDIUMFULL;
    else {
      SIZE_T end = (SIZE_T)m->pos + n;
      if (end > m->node->size) hr = stg_node_resize(m->node, end);
      if (SUCCEEDED(hr)) {
        mem_copy(m->node->data + m->pos, buf, n);
        m->pos = end;
        if (done) *done = n;
        stg_touch(m->parent);
      }
    }
  }
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_seek(IStream *me, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else {
    LONGLONG base;
    switch (origin) {
      case STREAM_SEEK_SET: base = 0; break;
      case STREAM_SEEK_CUR: base = (LONGLONG)m->pos; break;
      case STREAM_SEEK_END: base = (LONGLONG)m->node->size; break;
      default: hr = STG_E_INVALIDFUNCTION; base = 0; break;
    }
    if (SUCCEEDED(hr)) {
      if ((move.QuadPart > 0 && base > 0x7FFFFFFFFFFFFFFFll - move.QuadPart) || base + move.QuadPart < 0) hr = STG_E_INVALIDFUNCTION;
      else {
        m->pos = (ULONGLONG)(base + move.QuadPart);
        if (out) out->QuadPart = m->pos;
      }
    }
  }
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_set_size(IStream *me, ULARGE_INTEGER size) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else if (STG_ACCESS(m->mode) == STGM_READ) hr = STG_E_ACCESSDENIED;
  else if (size.HighPart) hr = STG_E_INVALIDFUNCTION;
  else if (size.QuadPart != m->node->size) {
    hr = stg_node_resize(m->node, (SIZE_T)size.QuadPart);
    if (SUCCEEDED(hr)) stg_touch(m->parent);
  }
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_copyto(IStream *me, IStream *dest, ULARGE_INTEGER n, ULARGE_INTEGER *read_total, ULARGE_INTEGER *written_total) {
  ULONGLONG rt = 0, wt = 0, left = n.QuadPart;
  BYTE *buf;
  HRESULT hr = S_OK;
  if (read_total) read_total->QuadPart = 0;
  if (written_total) written_total->QuadPart = 0;
  if (!dest) return STG_E_INVALIDPOINTER;
  buf = mem_alloc(65536);
  if (!buf) return STG_E_INSUFFICIENTMEMORY;
  while (left) {
    ULONG want = left < 65536 ? (ULONG)left : 65536, got = 0, put = 0;
    hr = stm_read(me, buf, want, &got);
    if (FAILED(hr) || !got) break;
    rt += got;
    hr = IStream_Write(dest, buf, got, &put);
    wt += put;
    if (FAILED(hr)) break;
    if (put != got) { hr = STG_E_MEDIUMFULL; break; }
    left -= got;
    if (got < want) break;
  }
  mem_free(buf);
  if (read_total) read_total->QuadPart = rt;
  if (written_total) written_total->QuadPart = wt;
  return hr;
}
static HRESULT WINAPI stm_commit(IStream *me, DWORD flags) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  (void)flags;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else if (stg_direct_chain(m->parent) && m->root->dirty && m->root->top && STG_ACCESS(m->root->top->mode) != STGM_READ) hr = stg_flush_locked(m->root);
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_revert(IStream *me) {
  StreamObj *m = stm_of(me);
  HRESULT hr;
  EnterCriticalSection(&m->root->cs);
  hr = stm_alive(m) ? S_OK : STG_E_REVERTED;
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_lock(IStream *me, ULARGE_INTEGER off, ULARGE_INTEGER n, DWORD type) { (void)me; (void)off; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI stm_unlock(IStream *me, ULARGE_INTEGER off, ULARGE_INTEGER n, DWORD type) { (void)me; (void)off; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI stm_stat(IStream *me, STATSTG *st, DWORD flags) {
  StreamObj *m = stm_of(me);
  HRESULT hr = S_OK;
  if (!st) return STG_E_INVALIDPOINTER;
  if (flags > STATFLAG_NOOPEN) return STG_E_INVALIDFLAG;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else {
    mem_zero(st, sizeof(*st));
    stg_fill_stat(m->node, st, m->mode);
    if (m->simple) st->cbSize.QuadPart = m->pos;
    if (!(flags & STATFLAG_NONAME) && !(st->pwcsName = stg_cotask_dup(m->node->name))) hr = STG_E_INSUFFICIENTMEMORY;
  }
  stg_leave(m->root);
  return hr;
}
static HRESULT WINAPI stm_clone(IStream *me, IStream **out) {
  StreamObj *m = stm_of(me);
  HRESULT hr;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  EnterCriticalSection(&m->root->cs);
  if (!stm_alive(m)) hr = STG_E_REVERTED;
  else {
    hr = stm_open_locked(m->parent, m->node, m->mode, FALSE, out);
    if (SUCCEEDED(hr)) stm_of(*out)->pos = m->pos;
  }
  stg_leave(m->root);
  return hr;
}
static const IStreamVtbl g_stm_vtbl = {stm_qi, stm_addref, stm_release, stm_read, stm_write, stm_seek, stm_set_size, stm_copyto, stm_commit, stm_revert, stm_lock, stm_unlock,
                                       stm_stat, stm_clone};

/* ---- the enumerator of the elements of a storage: it reads the storage as it is now, from the name it returned last ----------------------------------------------- */

static HRESULT WINAPI stgenum_qi(IEnumSTATSTG *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumSTATSTG)) { *out = me; IEnumSTATSTG_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI stgenum_addref(IEnumSTATSTG *me) { return (ULONG)InterlockedIncrement(&stgenum_of(me)->refs); }
static ULONG WINAPI stgenum_release(IEnumSTATSTG *me) {
  EnumObj *e = stgenum_of(me);
  LONG refs = InterlockedDecrement(&e->refs);
  if (refs == 0) {
    StgRoot *r = e->root;
    StorageObj *s = e->stg;
    EnterCriticalSection(&r->cs);
    s->enums--;
    if (!s->enums && !s->refs && !s->closed) stg_close_locked(s);
    stg_unhold_locked(s);
    r->objects--;
    mem_free(e);
    stg_leave(r);
  }
  return refs < 0 ? 0 : (ULONG)refs;
}
/* moves to the next element; FALSE at the end */
static BOOL stgenum_step(EnumObj *e, const StgNode **out) {
  const StgNode *p = e->stg->node;
  ULONG at = e->started ? stg_node_after(p, e->last) : 0;
  if (at >= p->nkids) return FALSE;
  *out = p->kids[at];
  wcopy_n(e->last, p->kids[at]->name, 32);
  e->started = TRUE;
  return TRUE;
}
static HRESULT WINAPI stgenum_next(IEnumSTATSTG *me, ULONG n, STATSTG *out, ULONG *fetched) {
  EnumObj *e = stgenum_of(me);
  ULONG count = 0, i;
  HRESULT hr = S_OK;
  if (!out) return STG_E_INVALIDPOINTER;
  if (!fetched && n != 1) return STG_E_INVALIDPARAMETER;
  EnterCriticalSection(&e->root->cs);
  if (!stg_alive(e->stg)) hr = STG_E_REVERTED;
  else {
    const StgNode *k;
    while (count < n && stgenum_step(e, &k)) {
      STATSTG *st = &out[count];
      mem_zero(st, sizeof(*st));
      st->pwcsName = stg_cotask_dup(k->name);
      if (!st->pwcsName) { hr = STG_E_INSUFFICIENTMEMORY; break; }
      stg_fill_stat(k, st, 0);
      count++;
    }
    if (SUCCEEDED(hr)) {
      for (i = count; i < n; i++) mem_zero(&out[i], sizeof(out[i]));
      if (count < n) hr = S_FALSE;
    }
  }
  stg_leave(e->root);
  if (fetched) *fetched = count;
  return hr;
}
static HRESULT WINAPI stgenum_skip(IEnumSTATSTG *me, ULONG n) {
  EnumObj *e = stgenum_of(me);
  HRESULT hr = S_OK;
  ULONG count = 0;
  EnterCriticalSection(&e->root->cs);
  if (!stg_alive(e->stg)) hr = STG_E_REVERTED;
  else {
    const StgNode *k;
    while (count < n && stgenum_step(e, &k)) count++;
    if (count < n) hr = S_FALSE;
  }
  stg_leave(e->root);
  return hr;
}
static HRESULT WINAPI stgenum_reset(IEnumSTATSTG *me) {
  EnumObj *e = stgenum_of(me);
  HRESULT hr = S_OK;
  EnterCriticalSection(&e->root->cs);
  if (!stg_alive(e->stg)) hr = STG_E_REVERTED;
  else e->started = FALSE;
  stg_leave(e->root);
  return hr;
}
static HRESULT WINAPI stgenum_clone(IEnumSTATSTG *me, IEnumSTATSTG **out) {
  EnumObj *e = stgenum_of(me), *c;
  HRESULT hr = S_OK;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  EnterCriticalSection(&e->root->cs);
  if (!stg_alive(e->stg)) hr = STG_E_REVERTED;
  else if (!(c = mem_alloc(sizeof(*c)))) hr = STG_E_INSUFFICIENTMEMORY;
  else {
    c->iface.lpVtbl = &g_stgenum_vtbl;
    c->stg = e->stg;
    c->root = e->root;
    c->started = e->started;
    mem_copy(c->last, e->last, sizeof(c->last));
    c->refs = 1;
    e->stg->enums++;
    e->stg->mem++;
    e->root->objects++;
    *out = &c->iface;
  }
  stg_leave(e->root);
  return hr;
}
static const IEnumSTATSTGVtbl g_stgenum_vtbl = {stgenum_qi, stgenum_addref, stgenum_release, stgenum_next, stgenum_skip, stgenum_reset, stgenum_clone};

/* ---- opening and creating roots -------------------------------------------------------------------------------------------------------------------------------- */

/* the elements an SNB names are opened empty */
static void stg_apply_snb(StgNode *tree, SNB snb) {
  ULONG i, k;
  for (i = 0; snb[i] && i < 0x10000; i++) {
    StgNode *n = stg_node_find(tree, snb[i], NULL);
    if (!n) continue;
    if (n->type == STGTY_STREAM) n->size = 0;
    else {
      for (k = 0; k < n->nkids; k++) stg_node_release(n->kids[k]);
      n->nkids = 0;
    }
  }
}

/* A root storage on `lb` (the root takes its own reference). `create`: a new, empty compound file with `shift`; else the file in `lb` is read. */
static HRESULT stg_root_open(ILockBytes *lb, const WCHAR *path, DWORD mode, BOOL create, ULONG shift, SNB snb, IStorage **out) {
  StgRoot *r = mem_alloc(sizeof(*r));
  StgNode *tree = NULL, *work = NULL;
  StorageObj *s;
  STATSTG st;
  HRESULT hr;
  *out = NULL;
  if (!r) return STG_E_INSUFFICIENTMEMORY;
  InitializeCriticalSection(&r->cs);
  r->lb = lb;
  ILockBytes_AddRef(lb);
  r->shift = shift;
  if (path && !(r->path = wdup_heap(path))) { hr = STG_E_INSUFFICIENTMEMORY; goto fail; }
  mem_zero(&st, sizeof(st));
  if (SUCCEEDED(ILockBytes_Stat(lb, &st, STATFLAG_NONAME))) {
    r->locks_supported = st.grfLocksSupported;
    if (st.pwcsName) CoTaskMemFree(st.pwcsName);
  }
  hr = stg_grab_locks(r, mode);
  if (FAILED(hr)) goto fail;
  if (create) {
    tree = stg_node_new(STGTY_STORAGE, kRootEntryName);
    if (!tree) { hr = STG_E_INSUFFICIENTMEMORY; goto fail; }
    hr = cfb_write(lb, tree, shift, 0);
  } else {
    /* a transacted root that others may write takes its snapshot under the commit lock */
    DWORD share = STG_SHARE(mode);
    BOOL shared = (mode & STGM_TRANSACTED) && !(mode & STGM_NOSNAPSHOT) && share != STGM_SHARE_DENY_WRITE && share != STGM_SHARE_EXCLUSIVE, supported = FALSE;
    if (shared) {
      hr = stg_lock_sync(r, RL_COMMIT, 1, &supported);
      if (FAILED(hr)) goto fail;
    }
    hr = cfb_read(lb, &tree, &r->shift, &r->sig);
    if (shared && supported) stg_unlock_region(r, RL_COMMIT, 1);
  }
  if (FAILED(hr)) goto fail;
  if (snb) stg_apply_snb(tree, snb);
  if ((mode & STGM_TRANSACTED) && !(work = stg_node_clone(tree))) { hr = STG_E_INSUFFICIENTMEMORY; goto fail; }
  s = stg_obj_new(r, NULL, mode & ~(DWORD)STGM_CREATE);
  if (!s) { hr = STG_E_INSUFFICIENTMEMORY; goto fail; }
  if (work) {
    s->transacted = TRUE;
    s->node = work;
    r->base = tree;
  } else s->node = tree;
  s->simple = (mode & STGM_SIMPLE) != 0;
  s->swmr_writer = (mode & STGM_DIRECT_SWMR) && STG_ACCESS(mode) != STGM_READ;
  r->top = s;
  if (snb && !work) r->dirty = TRUE;
  *out = &s->iface;
  return S_OK;
fail:
  stg_node_release(work);
  stg_node_release(tree);
  stg_unlock_all(r);
  ILockBytes_Release(lb);
  stg_root_free(r);
  return hr;
}

static HRESULT stg_query(IStorage *stg, REFIID riid, void **out) {
  HRESULT hr;
  if (!riid || IsEqualGUID(riid, &IID_IStorage)) { *out = stg; return S_OK; }
  hr = IStorage_QueryInterface(stg, riid, out);
  IStorage_Release(stg);
  return hr;
}

static HRESULT stg_create_file(const WCHAR *name, DWORD mode, DWORD attrs, ULONG sector_size, REFIID riid, void **out) {
  WCHAR *full = NULL, *tmpdir = NULL, *tmpname = NULL;
  DWORD creation;
  HANDLE file;
  FileLockBytes *l;
  IStorage *stg;
  HRESULT hr;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (attrs) return STG_E_INVALIDPARAMETER;
  if (sector_size != 512 && sector_size != 4096) return STG_E_INVALIDPARAMETER;
  if (!STG_SHARE(mode)) mode |= STGM_SHARE_DENY_NONE;
  if (FAILED(stg_validate_mode(mode))) return STG_E_INVALIDFLAG;
  if (STG_ACCESS(mode) != STGM_WRITE && STG_ACCESS(mode) != STGM_READWRITE) return STG_E_INVALIDFLAG;
  if (!(mode & STGM_TRANSACTED) && STG_SHARE(mode) != STGM_SHARE_EXCLUSIVE && !((mode & STGM_DIRECT_SWMR) && STG_SHARE(mode) == STGM_SHARE_DENY_WRITE)) return STG_E_INVALIDFLAG;
  if (mode & (STGM_PRIORITY | STGM_CONVERT)) return STG_E_INVALIDFLAG;
  if (!name) {
    tmpdir = mem_alloc((MAX_PATH + 1) * sizeof(WCHAR));
    tmpname = mem_alloc((MAX_PATH + 1) * sizeof(WCHAR));
    if (!tmpdir || !tmpname) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
    if (!GetTempPathW(MAX_PATH, tmpdir)) { tmpdir[0] = '.'; tmpdir[1] = 0; }
    if (!GetTempFileNameW(tmpdir, W('S', 'T', 'O'), 0, tmpname)) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
    name = tmpname;
    creation = TRUNCATE_EXISTING;
  } else creation = (mode & STGM_CREATE) ? CREATE_ALWAYS : CREATE_NEW;
  full = stg_full_path(name);
  if (!full) { hr = STG_E_INSUFFICIENTMEMORY; goto done; }
  file = CreateFileW(full, stg_file_access(mode), stg_file_share(mode), NULL, creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    hr = (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) ? STG_E_FILEALREADYEXISTS : stg_file_error(err);
    goto done;
  }
  l = flb_new(file, full, (mode & STGM_DELETEONRELEASE) != 0);
  if (!l) { CloseHandle(file); DeleteFileW(full); hr = STG_E_INSUFFICIENTMEMORY; goto done; }
  hr = stg_root_open(&l->iface, full, mode, TRUE, sector_size == 4096 ? 12 : 9, NULL, &stg);
  ILockBytes_Release(&l->iface);
  if (SUCCEEDED(hr)) hr = stg_query(stg, riid, out);
done:
  mem_free(full); mem_free(tmpdir); mem_free(tmpname);
  return hr;
}

static HRESULT stg_open_file(const WCHAR *name, DWORD mode, SNB snb, DWORD reserved, IStorage **out) {
  WCHAR *full;
  HANDLE file;
  FileLockBytes *l;
  LARGE_INTEGER size;
  DWORD share, access;
  HRESULT hr;
  if (!name) return STG_E_INVALIDNAME;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (reserved) return STG_E_INVALIDPARAMETER;
  if (mode & STGM_PRIORITY) {
    if (mode & (STGM_TRANSACTED | STGM_SIMPLE | STGM_NOSCRATCH | STGM_NOSNAPSHOT)) return STG_E_INVALIDFLAG;
    if (mode & STGM_DELETEONRELEASE) return STG_E_INVALIDFUNCTION;
    if (STG_ACCESS(mode) != STGM_READ) return STG_E_INVALIDFLAG;
    mode = (mode & ~0xF0u) | STGM_SHARE_DENY_NONE;
  }
  share = STG_SHARE(mode);
  if (mode & STGM_DIRECT_SWMR) {
    if (share != STGM_SHARE_DENY_WRITE && share != STGM_SHARE_DENY_NONE) return STG_E_INVALIDFLAG;
  } else if (!(mode & (STGM_TRANSACTED | STGM_PRIORITY))) {
    if (share != STGM_SHARE_EXCLUSIVE && share != STGM_SHARE_DENY_WRITE) return STG_E_INVALIDFLAG;
  }
  if (FAILED(stg_validate_mode(mode)) || (mode & STGM_CREATE)) return STG_E_INVALIDFLAG;
  if (share == STGM_SHARE_DENY_WRITE && STG_ACCESS(mode) == STGM_READWRITE && !(mode & (STGM_TRANSACTED | STGM_DIRECT_SWMR))) return STG_E_INVALIDFLAG;
  full = stg_full_path(name);
  if (!full) return STG_E_INSUFFICIENTMEMORY;
  access = stg_file_access(mode);
  file = CreateFileW(full, access, stg_file_share(mode), NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
  if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED && access != GENERIC_READ && STG_ACCESS(mode) == STGM_READ)
    file = CreateFileW(full, GENERIC_READ, stg_file_share(mode), NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
  if (file == INVALID_HANDLE_VALUE) { hr = stg_file_error(GetLastError()); mem_free(full); return hr; }
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0x100) { CloseHandle(file); mem_free(full); return STG_E_FILEALREADYEXISTS; }
  l = flb_new(file, full, (mode & STGM_DELETEONRELEASE) != 0);
  if (!l) { CloseHandle(file); mem_free(full); return STG_E_INSUFFICIENTMEMORY; }
  hr = stg_root_open(&l->iface, full, mode, FALSE, 9, snb, out);
  ILockBytes_Release(&l->iface);
  mem_free(full);
  if (hr == STG_E_INVALIDHEADER) hr = STG_E_FILEALREADYEXISTS;
  return hr;
}

/* ---- the exports ----------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI StgCreateDocfile(const WCHAR *name, DWORD mode, DWORD reserved, IStorage **out) {
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (reserved) return STG_E_INVALIDPARAMETER;
  return stg_create_file(name, mode, 0, 512, &IID_IStorage, (void **)out);
}

EXPORT HRESULT WINAPI StgCreateDocfileOnILockBytes(ILockBytes *lb, DWORD mode, DWORD reserved, IStorage **out) {
  if (!out || !lb) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (reserved) return STG_E_INVALIDPARAMETER;
  if (FAILED(stg_validate_mode(mode)) || (mode & STGM_DELETEONRELEASE)) return STG_E_INVALIDFLAG;
  return stg_root_open(lb, NULL, mode, TRUE, 9, NULL, out);
}

EXPORT HRESULT WINAPI StgOpenStorage(const WCHAR *name, IStorage *prio, DWORD mode, SNB snb, DWORD reserved, IStorage **out) {
  WCHAR *prio_name = NULL;
  HRESULT hr;
  if (prio) {
    STATSTG st;
    hr = IStorage_Stat(prio, &st, STATFLAG_DEFAULT);
    if (FAILED(hr)) return hr;
    prio_name = st.pwcsName;
    IStorage_Release(prio);
    name = prio_name;
  }
  hr = stg_open_file(name, mode, snb, reserved, out);
  CoTaskMemFree(prio_name);
  return hr;
}

EXPORT HRESULT WINAPI StgOpenStorageOnILockBytes(ILockBytes *lb, IStorage *prio, DWORD mode, SNB snb, DWORD reserved, IStorage **out) {
  HRESULT hr;
  if (!lb || !out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (reserved) return STG_E_INVALIDPARAMETER;
  if (FAILED(stg_validate_mode(mode))) return STG_E_INVALIDFLAG;
  if (prio) IStorage_Release(prio);
  if (mode & STGM_PRIORITY) mode = (mode & ~0xF0u) | STGM_SHARE_DENY_NONE;
  hr = stg_root_open(lb, NULL, mode & ~(DWORD)STGM_CREATE, FALSE, 9, snb, out);
  if (hr == STG_E_INVALIDHEADER) hr = STG_E_FILEALREADYEXISTS;
  return hr;
}

EXPORT HRESULT WINAPI StgCreateStorageEx(const WCHAR *name, DWORD mode, DWORD fmt, DWORD attrs, STGOPTIONS *options, PSECURITY_DESCRIPTOR sd, REFIID riid, void **out) {
  (void)sd;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (fmt != STGFMT_FILE && attrs) return STG_E_INVALIDPARAMETER;
  if (fmt == STGFMT_FILE) return (attrs && attrs != FILE_FLAG_NO_BUFFERING) ? STG_E_INVALIDPARAMETER : STG_E_UNIMPLEMENTEDFUNCTION;
  if (fmt != STGFMT_STORAGE && fmt != STGFMT_DOCFILE) return STG_E_INVALIDPARAMETER;
  if (options && options->usVersion > 2) return STG_E_INVALIDPARAMETER;
  return stg_create_file(name, mode, 0, options ? options->ulSectorSize : 512, riid, out);
}

EXPORT HRESULT WINAPI StgOpenStorageEx(const WCHAR *name, DWORD mode, DWORD fmt, DWORD attrs, STGOPTIONS *options, PSECURITY_DESCRIPTOR sd, REFIID riid, void **out) {
  IStorage *stg;
  HRESULT hr;
  (void)options; (void)sd;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (fmt != STGFMT_DOCFILE && attrs) return STG_E_INVALIDPARAMETER;
  switch (fmt) {
    case STGFMT_STORAGE: case STGFMT_ANY: break;
    case STGFMT_DOCFILE: if (attrs && attrs != FILE_FLAG_NO_BUFFERING) return STG_E_INVALIDPARAMETER; break;
    default: return STG_E_INVALIDPARAMETER;      /* STGFMT_FILE (NTFS property sets) included */
  }
  hr = StgOpenStorage(name, NULL, mode, NULL, 0, &stg);
  if (FAILED(hr)) return hr;
  return stg_query(stg, riid, out);
}

EXPORT HRESULT WINAPI StgOpenStorageOnHandle(HANDLE handle, DWORD mode, void *reserved1, void *reserved2, REFIID riid, void **out) {
  HANDLE dup;
  FileLockBytes *l;
  IStorage *stg;
  HRESULT hr;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  if (reserved1 || reserved2) return STG_E_INVALIDPARAMETER;
  if (!handle || handle == INVALID_HANDLE_VALUE) return STG_E_INVALIDHANDLE;
  if (FAILED(stg_validate_mode(mode)) || (mode & (STGM_CREATE | STGM_DELETEONRELEASE | STGM_PRIORITY))) return STG_E_INVALIDFLAG;
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) return STG_E_INVALIDHANDLE;
  l = flb_new(dup, NULL, FALSE);
  if (!l) { CloseHandle(dup); return STG_E_INSUFFICIENTMEMORY; }
  hr = stg_root_open(&l->iface, NULL, mode, FALSE, 9, NULL, &stg);
  ILockBytes_Release(&l->iface);
  if (hr == STG_E_INVALIDHEADER) hr = STG_E_FILEALREADYEXISTS;
  if (FAILED(hr)) return hr;
  return stg_query(stg, riid, out);
}

EXPORT HRESULT WINAPI StgIsStorageFile(const WCHAR *name) {
  HANDLE file;
  BYTE magic[8];
  DWORD got = 0;
  if (!name) return STG_E_INVALIDNAME;
  file = CreateFileW(name, GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return STG_E_FILENOTFOUND;
  if (!ReadFile(file, magic, 8, &got, NULL)) { CloseHandle(file); return STG_E_FILENOTFOUND; }
  CloseHandle(file);
  if (got != 8) return S_FALSE;
  return mem_cmp(magic, kCfbSignature, 8) ? S_FALSE : S_OK;
}

EXPORT HRESULT WINAPI StgIsStorageILockBytes(ILockBytes *lb) {
  BYTE magic[8];
  ULONG got = 0;
  if (!lb) return STG_E_INVALIDPOINTER;
  if (FAILED(cfb_read_at(lb, 0, magic, 8, &got)) || got != 8) return S_FALSE;
  return mem_cmp(magic, kCfbSignature, 8) ? S_FALSE : S_OK;
}

EXPORT HRESULT WINAPI StgSetTimes(const WCHAR *name, const FILETIME *ctime, const FILETIME *atime, const FILETIME *mtime) {
  IStorage *stg;
  HRESULT hr = StgOpenStorage(name, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, NULL, 0, &stg);
  if (FAILED(hr)) return hr;
  hr = IStorage_SetElementTimes(stg, NULL, ctime, atime, mtime);
  IStorage_Release(stg);
  return hr;
}

/* Asynchronous docfiles (filled as their data arrives): not part of this implementation; Windows reports a missing implementation the same way. */
EXPORT HRESULT WINAPI StgOpenAsyncDocfileOnIFillLockBytes(IFillLockBytes *fill, DWORD mode, DWORD flags, IStorage **out) {
  (void)fill; (void)mode; (void)flags;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
EXPORT HRESULT WINAPI StgGetIFillLockBytesOnILockBytes(ILockBytes *lb, IFillLockBytes **out) {
  (void)lb;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
EXPORT HRESULT WINAPI StgGetIFillLockBytesOnFile(OLECHAR const *name, IFillLockBytes **out) {
  (void)name;
  if (out) *out = NULL;
  return E_NOTIMPL;
}

/* The class of a file: the class of the root storage of a compound file, else the class the registry gives to the extension (HKCR\.ext -> ProgID -> CLSID). */
EXPORT HRESULT WINAPI GetClassFile(LPCOLESTR name, CLSID *clsid) {
  WCHAR *progid;
  const WCHAR *ext = NULL, *p;
  DWORD attr;
  HRESULT hr;
  if (!name || !clsid) return E_INVALIDARG;
  mem_zero(clsid, sizeof(*clsid));
  attr = GetFileAttributesW(name);
  if (attr == INVALID_FILE_ATTRIBUTES) return MK_E_CANTOPENFILE;
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && StgIsStorageFile(name) == S_OK) {
    IStorage *stg;
    hr = StgOpenStorage(name, NULL, STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &stg);
    if (FAILED(hr)) hr = StgOpenStorage(name, NULL, STGM_READ | STGM_SHARE_DENY_NONE | STGM_TRANSACTED, NULL, 0, &stg);
    if (FAILED(hr)) return hr;
    hr = ReadClassStg(stg, clsid);
    IStorage_Release(stg);
    return hr;
  }
  for (p = name; *p; p++) {
    if (*p == '\\' || *p == '/') ext = NULL;
    else if (*p == '.') ext = p;
  }
  if (!ext || !ext[1] || wlen(ext) > 200) return MK_E_INVALIDEXTENSION;
  progid = mem_alloc(260 * sizeof(WCHAR));
  if (!progid) return E_OUTOFMEMORY;
  hr = MK_E_INVALIDEXTENSION;
  if (reg_class_string(ext, NULL, progid, 260) == ERROR_SUCCESS && progid[0] && CLSIDFromProgID(progid, clsid) == S_OK) hr = S_OK;
  mem_free(progid);
  return hr;
}

EXPORT HRESULT WINAPI WriteClassStg(LPSTORAGE stg, REFCLSID clsid) {
  if (!stg) return E_INVALIDARG;
  if (!clsid) return STG_E_INVALIDPOINTER;
  return IStorage_SetClass(stg, clsid);
}
EXPORT HRESULT WINAPI ReadClassStg(LPSTORAGE stg, CLSID *clsid) {
  STATSTG st;
  HRESULT hr;
  if (!stg || !clsid) return E_INVALIDARG;
  mem_zero(&st, sizeof(st));
  hr = IStorage_Stat(stg, &st, STATFLAG_NONAME);
  if (SUCCEEDED(hr)) *clsid = st.clsid;
  if (st.pwcsName) CoTaskMemFree(st.pwcsName);
  return hr;
}

/* ---- \1CompObj: the user type, the clipboard format and the ProgID of an object (MS-OLEDS CompObjStream) ------------------------------------------------------- */

static const WCHAR kCompObjName[] = {1, 'C', 'o', 'm', 'p', 'O', 'b', 'j', 0};
static const WCHAR kOle1Name[] = {1, 'O', 'l', 'e', 0};
#define STG_COMPOBJ_UNICODE_MARKER 0x71B239F4u
#define STG_OLESTREAM_VERSION 0x02000001u
#define STG_OLESTREAM_CONVERT 0x00000004u

/* a length-prefixed string of the ANSI code page (0 for none) */
static HRESULT stg_write_ansi(IStream *stm, const WCHAR *s) {
  DWORD n = 0;
  char *buf = NULL;
  HRESULT hr;
  if (s && *s) {
    int need = WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0 || need > 0x100000) return E_INVALIDARG;
    buf = mem_alloc((SIZE_T)need);
    if (!buf) return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_ACP, 0, s, -1, buf, need, NULL, NULL);
    n = (DWORD)need;
  }
  hr = stream_write_all(stm, &n, 4);
  if (SUCCEEDED(hr) && n) hr = stream_write_all(stm, buf, n);
  mem_free(buf);
  return hr;
}
static HRESULT stg_write_unicode(IStream *stm, const WCHAR *s) {
  DWORD n = (s && *s) ? (DWORD)wlen(s) + 1 : 0;
  HRESULT hr = stream_write_all(stm, &n, 4);
  if (SUCCEEDED(hr) && n) hr = stream_write_all(stm, s, n * sizeof(WCHAR));
  return hr;
}
static HRESULT stg_write_cf(IStream *stm, CLIPFORMAT cf) {
  DWORD v[2];
  if (!cf) { v[0] = 0; return stream_write_all(stm, v, 4); }
  v[0] = 0xFFFFFFFFu;
  v[1] = cf;
  return stream_write_all(stm, v, 8);
}

EXPORT HRESULT WINAPI WriteFmtUserTypeStg(LPSTORAGE stg, CLIPFORMAT cf, LPOLESTR usertype) {
  static const BYTE header[12] = {0x01, 0x00, 0xFE, 0xFF, 0x03, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  IStream *stm;
  STATSTG st;
  LPOLESTR progid = NULL;
  DWORD marker = STG_COMPOBJ_UNICODE_MARKER;
  HRESULT hr;
  if (!stg) return E_INVALIDARG;
  /* (an existing stream is looked at first, as Windows does) */
  if (SUCCEEDED(IStorage_OpenStream(stg, kCompObjName, NULL, STGM_SHARE_EXCLUSIVE, 0, &stm))) IStream_Release(stm);
  mem_zero(&st, sizeof(st));
  hr = IStorage_Stat(stg, &st, STATFLAG_NONAME);
  if (FAILED(hr)) return hr;
  if (st.pwcsName) CoTaskMemFree(st.pwcsName);
  ProgIDFromCLSID(&st.clsid, &progid);
  hr = IStorage_CreateStream(stg, kCompObjName, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stm);
  if (SUCCEEDED(hr)) {
    hr = stream_write_all(stm, header, sizeof(header));
    if (SUCCEEDED(hr)) hr = stream_write_all(stm, &st.clsid, sizeof(CLSID));
    if (SUCCEEDED(hr)) hr = stg_write_ansi(stm, usertype);
    if (SUCCEEDED(hr)) hr = stg_write_cf(stm, cf);
    if (SUCCEEDED(hr)) hr = stg_write_ansi(stm, progid);
    if (SUCCEEDED(hr)) hr = stream_write_all(stm, &marker, 4);
    if (SUCCEEDED(hr)) hr = stg_write_unicode(stm, usertype);
    if (SUCCEEDED(hr)) hr = stg_write_cf(stm, cf);
    if (SUCCEEDED(hr)) hr = stg_write_unicode(stm, progid);
    IStream_Release(stm);
  }
  CoTaskMemFree(progid);
  return hr;
}

/* a reader over the bytes of a stream read whole */
typedef struct StgBytes { const BYTE *p; SIZE_T n, at; } StgBytes;
static BOOL stg_take32(StgBytes *b, DWORD *v) {
  if (b->n - b->at < 4) return FALSE;
  *v = stg_rd32(b->p + b->at);
  b->at += 4;
  return TRUE;
}
static BOOL stg_skip(StgBytes *b, SIZE_T n) {
  if (b->n - b->at < n) return FALSE;
  b->at += n;
  return TRUE;
}
/* the clipboard format field: a format number, a name (no registered formats here: 0) or nothing */
static BOOL stg_take_cf(StgBytes *b, BOOL wide, CLIPFORMAT *cf) {
  DWORD v;
  if (!stg_take32(b, &v)) return FALSE;
  *cf = 0;
  if (v == 0xFFFFFFFFu || v == 0xFFFFFFFEu) {
    DWORD f;
    if (!stg_take32(b, &f)) return FALSE;
    *cf = (CLIPFORMAT)f;
    return TRUE;
  }
  if (v > 0x10000) return FALSE;
  return stg_skip(b, wide ? (SIZE_T)v * 2 : v);
}

EXPORT HRESULT WINAPI ReadFmtUserTypeStg(LPSTORAGE stg, CLIPFORMAT *pcf, LPOLESTR *pusertype) {
  IStream *stm;
  STATSTG st;
  BYTE *data;
  StgBytes b;
  DWORD alen, v;
  const BYTE *ansi;
  const WCHAR *wide = NULL;
  DWORD wlen_chars = 0;
  CLIPFORMAT cf = 0, wcf = 0;
  BOOL have_wcf = FALSE;
  HRESULT hr;
  if (pcf) *pcf = 0;
  if (pusertype) *pusertype = NULL;
  if (!stg) return E_INVALIDARG;
  hr = IStorage_OpenStream(stg, kCompObjName, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stm);
  if (FAILED(hr)) return hr;
  hr = IStream_Stat(stm, &st, STATFLAG_NONAME);
  if (SUCCEEDED(hr) && (st.cbSize.QuadPart < 28 || st.cbSize.QuadPart > 0x100000)) hr = STG_E_INVALIDHEADER;
  if (FAILED(hr)) { IStream_Release(stm); return hr; }
  data = mem_alloc((SIZE_T)st.cbSize.QuadPart);
  if (!data) { IStream_Release(stm); return E_OUTOFMEMORY; }
  hr = stream_read_all(stm, data, (ULONG)st.cbSize.QuadPart);
  IStream_Release(stm);
  if (FAILED(hr)) { mem_free(data); return hr; }
  b.p = data; b.n = (SIZE_T)st.cbSize.QuadPart; b.at = 28;
  if (!stg_take32(&b, &alen) || alen > 0x10000 || (ansi = b.p + b.at, !stg_skip(&b, alen)) || !stg_take_cf(&b, FALSE, &cf) || !stg_take32(&b, &v) || v > 0x10000 || !stg_skip(&b, v)) {
    mem_free(data);
    return STG_E_INVALIDHEADER;
  }
  if (stg_take32(&b, &v) && v == STG_COMPOBJ_UNICODE_MARKER && stg_take32(&b, &wlen_chars) && wlen_chars <= 0x10000 && b.n - b.at >= (SIZE_T)wlen_chars * 2) {
    wide = (const WCHAR *)(b.p + b.at);
    b.at += (SIZE_T)wlen_chars * 2;
    have_wcf = stg_take_cf(&b, TRUE, &wcf);
  } else wlen_chars = 0;
  if (pcf) *pcf = have_wcf && wcf ? wcf : cf;
  if (pusertype) {
    WCHAR *out = NULL;
    if (wlen_chars) {
      out = CoTaskMemAlloc((SIZE_T)wlen_chars * sizeof(WCHAR) + sizeof(WCHAR));
      if (out) { mem_copy(out, wide, (SIZE_T)wlen_chars * sizeof(WCHAR)); out[wlen_chars] = 0; }
    } else if (alen) {
      int n = MultiByteToWideChar(CP_ACP, 0, (const char *)ansi, (int)alen, NULL, 0);
      if (n > 0) {
        out = CoTaskMemAlloc(((SIZE_T)n + 1) * sizeof(WCHAR));
        if (out) { MultiByteToWideChar(CP_ACP, 0, (const char *)ansi, (int)alen, out, n); out[n] = 0; }
      }
    }
    if ((wlen_chars || alen) && !out) hr = E_OUTOFMEMORY;
    *pusertype = out;
  }
  mem_free(data);
  return hr;
}

/* ---- \1Ole: the flags of an embedded object (the "convert" bit) ------------------------------------------------------------------------------------------------- */

/* A new \1Ole stream with `flags` (an empty object: no monikers). Shared with the OLE helpers. */
static HRESULT storage_create_ole_stream(IStorage *stg, DWORD flags) {
  DWORD data[5];
  IStream *stm;
  HRESULT hr = IStorage_CreateStream(stg, kOle1Name, STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stm);
  if (FAILED(hr)) return hr;
  data[0] = STG_OLESTREAM_VERSION;
  data[1] = flags;
  data[2] = data[3] = data[4] = 0;
  hr = stream_write_all(stm, data, sizeof(data));
  IStream_Release(stm);
  return hr;
}

EXPORT HRESULT WINAPI SetConvertStg(LPSTORAGE stg, WINBOOL convert) {
  DWORD flags = convert ? STG_OLESTREAM_CONVERT : 0, header[2];
  IStream *stm;
  HRESULT hr;
  if (!stg) return E_INVALIDARG;
  hr = IStorage_OpenStream(stg, kOle1Name, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &stm);
  if (FAILED(hr)) return hr == STG_E_FILENOTFOUND ? storage_create_ole_stream(stg, flags) : hr;
  hr = stream_read_all(stm, header, sizeof(header));
  if (SUCCEEDED(hr) && ((header[1] ^ flags) & STG_OLESTREAM_CONVERT)) {
    DWORD nflags = (header[1] & STG_OLESTREAM_CONVERT) ? header[1] & ~STG_OLESTREAM_CONVERT : header[1] | STG_OLESTREAM_CONVERT;
    hr = stream_seek_to(stm, 4);
    if (SUCCEEDED(hr)) hr = stream_write_all(stm, &nflags, 4);
  }
  IStream_Release(stm);
  return hr;
}

EXPORT HRESULT WINAPI GetConvertStg(LPSTORAGE stg) {
  DWORD header[2];
  IStream *stm;
  HRESULT hr;
  if (!stg) return E_INVALIDARG;
  hr = IStorage_OpenStream(stg, kOle1Name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stm);
  if (FAILED(hr)) return hr;
  hr = stream_read_all(stm, header, sizeof(header));
  IStream_Release(stm);
  if (FAILED(hr)) return hr;
  if (header[0] != STG_OLESTREAM_VERSION) return E_FAIL;
  return (header[1] & STG_OLESTREAM_CONVERT) ? S_OK : S_FALSE;
}

/* The "document" bit of a storage (OLE 2 kept it in the state bits). */
EXPORT HRESULT WINAPI SetDocumentBitStg(IStorage *stg, BOOL document) {
  if (!stg) return E_INVALIDARG;
  return IStorage_SetStateBits(stg, document ? 1 : 0, 1);
}
EXPORT HRESULT WINAPI GetDocumentBitStg(IStorage *stg) {
  STATSTG st;
  HRESULT hr;
  if (!stg) return E_INVALIDARG;
  mem_zero(&st, sizeof(st));
  hr = IStorage_Stat(stg, &st, STATFLAG_NONAME);
  if (FAILED(hr)) return hr;
  if (st.pwcsName) CoTaskMemFree(st.pwcsName);
  return (st.grfStateBits & 1) ? S_OK : S_FALSE;
}

EXPORT HRESULT WINAPI OpenOrCreateStream(IStorage *stg, const OLECHAR *name, IStream **out) {
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  if (!stg) { *out = NULL; return E_INVALIDARG; }
  hr = IStorage_OpenStream(stg, name, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, out);
  if (FAILED(hr)) hr = IStorage_CreateStream(stg, name, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, out);
  return hr;
}

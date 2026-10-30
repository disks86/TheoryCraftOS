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
/* ntdll.dll: structures and routines of the Rtl library that Wine's tests look at: the splay tree, the red-black tree, handle tables, pointer encoding, random numbers and
   a few small helpers. Included by ntdll.c after rtl2.inc. */

typedef struct SplayLinks { struct SplayLinks *Parent, *LeftChild, *RightChild; } RTL_SPLAY_LINKS_, *PRTL_SPLAY_LINKS_;
#define RTL_SPLAY_LINKS RTL_SPLAY_LINKS_
#define RtlParent(l) ((l)->Parent)
#define RtlLeftChild(l) ((l)->LeftChild)
#define RtlRightChild(l) ((l)->RightChild)
#define RtlIsRoot(l) ((l)->Parent == (l))
#define RtlIsLeftChild(l) (RtlLeftChild(RtlParent(l)) == (l))
#define RtlIsRightChild(l) (RtlRightChild(RtlParent(l)) == (l))
#define RtlInsertAsLeftChild(p, c) do { (p)->LeftChild = (c); (c)->Parent = (p); } while (0)
#define RtlInsertAsRightChild(p, c) do { (p)->RightChild = (c); (c)->Parent = (p); } while (0)

/* ---- splay trees ---- */
static void rtl_splay_replace(RTL_SPLAY_LINKS *x, RTL_SPLAY_LINKS *y, RTL_SPLAY_LINKS **root)
{
    if (RtlIsRoot(x))
        *root = y;
    else if (RtlIsLeftChild(x))
        x->Parent->LeftChild = y;
    else
        x->Parent->RightChild = y;

    if (y)
        y->Parent = RtlIsRoot(x) ? y : x->Parent;
}

static void rtl_splay_left_rotate(RTL_SPLAY_LINKS *x)
{
    RTL_SPLAY_LINKS *y = x->RightChild;

    if (y)
    {
        x->RightChild = y->LeftChild;
        if (y->LeftChild)
            y->LeftChild->Parent = x;

        y->Parent = RtlIsRoot(x) ? y : x->Parent;
    }

    if (!RtlIsRoot(x))
    {
        if (RtlIsLeftChild(x))
            x->Parent->LeftChild = y;
        else
            x->Parent->RightChild = y;
    }

    if (y)
        y->LeftChild = x;
    x->Parent = y;
}

static void rtl_splay_right_rotate(RTL_SPLAY_LINKS *x)
{
    RTL_SPLAY_LINKS *y = x->LeftChild;

    if (y)
    {
        x->LeftChild = y->RightChild;
        if (y->RightChild)
            y->RightChild->Parent = x;

        y->Parent = RtlIsRoot(x) ? y : x->Parent;
    }

    if (!RtlIsRoot(x))
    {
        if (RtlIsLeftChild(x))
            x->Parent->LeftChild = y;
        else
            x->Parent->RightChild = y;
    }

    if (y)
        y->RightChild = x;
    x->Parent = y;
}

/******************************************************************************
 *  RtlSubtreePredecessor           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlSubtreePredecessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    child = RtlLeftChild(links);
    if (!child)
        return NULL;

    while (RtlRightChild(child))
        child = RtlRightChild(child);

    return child;
}

/******************************************************************************
 *  RtlSubtreeSuccessor           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlSubtreeSuccessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    child = RtlRightChild(links);
    if (!child)
        return NULL;

    while (RtlLeftChild(child))
        child = RtlLeftChild(child);

    return child;
}

/******************************************************************************
 *  RtlRealPredecessor           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlRealPredecessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    child = RtlLeftChild(links);
    if (child)
    {
        while (RtlRightChild(child))
            child = RtlRightChild(child);
        return child;
    }

    child = links;
    while (RtlIsLeftChild(child))
        child = RtlParent(child);

    if (RtlIsRightChild(child))
        return RtlParent(child);

    return NULL;
}

/******************************************************************************
 *  RtlRealSuccessor           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlRealSuccessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    child = RtlRightChild(links);
    if (child)
    {
        while (RtlLeftChild(child))
            child = RtlLeftChild(child);
        return child;
    }

    child = links;
    while (RtlIsRightChild(child))
        child = RtlParent(child);

    if (RtlIsLeftChild(child))
        return RtlParent(child);

    return NULL;
}

/******************************************************************************
 *  RtlSplay           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlSplay(RTL_SPLAY_LINKS *links)
{

    while (!RtlIsRoot(links))
    {
        if (RtlIsRoot(links->Parent))
        {
            /* Zig */
            if (RtlIsLeftChild(links))
                rtl_splay_right_rotate(links->Parent);
            /* Zag */
            else
                rtl_splay_left_rotate(links->Parent);
        }
        /* Zig-Zig */
        else if (RtlIsLeftChild(links->Parent) && RtlIsLeftChild(links))
        {
            rtl_splay_right_rotate(links->Parent->Parent);
            rtl_splay_right_rotate(links->Parent);
        }
        /* Zag-Zag */
        else if (RtlIsRightChild(links->Parent) && RtlIsRightChild(links))
        {
            rtl_splay_left_rotate(links->Parent->Parent);
            rtl_splay_left_rotate(links->Parent);
        }
        /* Zig-Zag */
        else if (RtlIsRightChild(links->Parent) && RtlIsLeftChild(links))
        {
            rtl_splay_right_rotate(links->Parent);
            rtl_splay_left_rotate(links->Parent);
        }
        /* Zag-Zig */
        else
        {
            rtl_splay_left_rotate(links->Parent);
            rtl_splay_right_rotate(links->Parent);
        }
    }

    return links;
}

/******************************************************************************
 *  RtlDeleteNoSplay           [NTDLL.@]
 */
EXPORT void NTAPI RtlDeleteNoSplay(RTL_SPLAY_LINKS *links, RTL_SPLAY_LINKS **root)
{

    if (RtlIsRoot(links) && !RtlLeftChild(links) && !RtlRightChild(links))
    {
        *root = NULL;
    }
    else  if (!links->LeftChild)
    {
        rtl_splay_replace(links, links->RightChild, root);
    }
    else if (!links->RightChild)
    {
        rtl_splay_replace(links, links->LeftChild, root);
    }
    else
    {
        RTL_SPLAY_LINKS *predecessor = RtlSubtreePredecessor(links);
        if (predecessor->Parent != links)
        {
            rtl_splay_replace(predecessor, predecessor->LeftChild, root);
            RtlInsertAsLeftChild(predecessor, links->LeftChild);
        }
        rtl_splay_replace(links, predecessor, root);
        RtlInsertAsRightChild(predecessor, links->RightChild);
    }
}

/******************************************************************************
 *  RtlDelete           [NTDLL.@]
 */
EXPORT RTL_SPLAY_LINKS * NTAPI RtlDelete(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *root, *to_splay;

    if (RtlIsRoot(links) && !RtlLeftChild(links) && !RtlRightChild(links))
    {
        return NULL;
    }
    else if (!links->LeftChild)
    {
        rtl_splay_replace(links, links->RightChild, &root);
        if (RtlIsRoot(links))
            return links->RightChild;

        to_splay = links->Parent;
    }
    else if (!links->RightChild)
    {
        rtl_splay_replace(links, links->LeftChild, &root);
        if (RtlIsRoot(links))
            return links->LeftChild;

        to_splay = links->Parent;
    }
    else
    {
        RTL_SPLAY_LINKS *predecessor = RtlSubtreePredecessor(links);
        if (predecessor->Parent != links)
        {
            rtl_splay_replace(predecessor, predecessor->LeftChild, &root);
            RtlInsertAsLeftChild(predecessor, links->LeftChild);
            /* Delete operation first swap the value of node to delete and that of the predecessor
             * and then delete the predecessor instead. Finally, the parent of the actual deleted
             * node, which is the predecessor, is splayed afterwards */
            to_splay = predecessor->Parent;
        }
        else
        {
            /* links is the parent of predecessor. So after swapping, the parent of links is in
             * fact the predecessor. So predecessor gets splayed */
            to_splay = predecessor;
        }
        rtl_splay_replace(links, predecessor, &root);
        RtlInsertAsRightChild(predecessor, links->RightChild);
    }

    return RtlSplay(to_splay);
}

/* ---- red-black trees ---- */
typedef struct BalancedNode {
  union { struct BalancedNode *Children[2]; struct { struct BalancedNode *Left, *Right; }; };
  union { unsigned char Red : 1; unsigned char Balance : 2; ULONG_PTR ParentValue; };
} RTL_BALANCED_NODE_;
typedef struct { RTL_BALANCED_NODE_ *Root, *Min; } RTL_RB_TREE_;
#define RTL_BALANCED_NODE_RESERVED_PARENT_MASK 3

static RTL_BALANCED_NODE_ *rtl_node_parent( RTL_BALANCED_NODE_ *node )
{
    return (RTL_BALANCED_NODE_ *)(node->ParentValue & ~(ULONG_PTR)RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static void rtl_set_node_parent( RTL_BALANCED_NODE_ *node, RTL_BALANCED_NODE_ *parent )
{
    node->ParentValue = (ULONG_PTR)parent | (node->ParentValue & RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static void rtl_rotate( RTL_RB_TREE_ *tree, RTL_BALANCED_NODE_ *n, int right )
{
    RTL_BALANCED_NODE_ *child = n->Children[!right];
    RTL_BALANCED_NODE_ *parent = rtl_node_parent( n );

    if (!parent)                tree->Root = child;
    else if (parent->Left == n) parent->Left = child;
    else                        parent->Right = child;

    n->Children[!right] = child->Children[right];
    if (n->Children[!right]) rtl_set_node_parent( n->Children[!right], n );
    child->Children[right] = n;
    rtl_set_node_parent( child, parent );
    rtl_set_node_parent( n, child );
}

static void rtl_flip_color( RTL_BALANCED_NODE_ *node )
{
    node->Red = !node->Red;
    node->Left->Red = !node->Left->Red;
    node->Right->Red = !node->Right->Red;
}

/*********************************************************************
 *           RtlRbInsertNodeEx [NTDLL.@]
 */
EXPORT void NTAPI RtlRbInsertNodeEx( RTL_RB_TREE_ *tree, RTL_BALANCED_NODE_ *parent, BOOLEAN right, RTL_BALANCED_NODE_ *node )
{
    RTL_BALANCED_NODE_ *grandparent;

    node->ParentValue = (ULONG_PTR)parent;
    node->Left = NULL;
    node->Right = NULL;

    if (!parent)
    {
        tree->Root = tree->Min = node;
        return;
    }
    if (right > 1)
    {
        return;
    }
    if (parent->Children[right])
    {
        return;
    }

    node->Red = 1;
    parent->Children[right] = node;
    if (tree->Min == parent && parent->Left == node) tree->Min = node;
    grandparent = rtl_node_parent( parent );
    while (parent->Red)
    {
        right = (parent == grandparent->Right);
        if (grandparent->Children[!right] && grandparent->Children[!right]->Red)
        {
            node = grandparent;
            rtl_flip_color( node );
            if (!(parent = rtl_node_parent( node ))) break;
            grandparent = rtl_node_parent( parent );
            continue;
        }
        if (node == parent->Children[!right])
        {
            node = parent;
            rtl_rotate( tree, node, right );
            parent = rtl_node_parent( node );
            grandparent = rtl_node_parent( parent );
        }
        parent->Red = 0;
        grandparent->Red = 1;
        rtl_rotate( tree, grandparent, !right );
    }
    tree->Root->Red = 0;
}

/*********************************************************************
 *           RtlRbRemoveNode [NTDLL.@]
 */
EXPORT void NTAPI RtlRbRemoveNode( RTL_RB_TREE_ *tree, RTL_BALANCED_NODE_ *node )
{
    RTL_BALANCED_NODE_ *iter = NULL, *child, *parent, *w;
    BOOL need_fixup;
    int right;

    if (node->Right && (node->Left || tree->Min == node))
    {
        for (iter = node->Right; iter->Left; iter = iter->Left)
            ;
        if (tree->Min == node) tree->Min = iter;
    }
    else if (tree->Min == node) tree->Min = rtl_node_parent( node );
    if (!iter || !node->Left) iter = node;

    child = iter->Left ? iter->Left : iter->Right;

    if (!(parent = rtl_node_parent( iter ))) tree->Root = child;
    else if (iter == parent->Left)           parent->Left = child;
    else                                     parent->Right = child;

    if (child) rtl_set_node_parent( child, parent );

    need_fixup = !iter->Red;

    if (node != iter)
    {
        *iter = *node;
        if (!(w = rtl_node_parent( iter ))) tree->Root = iter;
        else if (node == w->Left)           w->Left = iter;
        else                                w->Right = iter;

        if (iter->Right) rtl_set_node_parent( iter->Right, iter );
        if (iter->Left)  rtl_set_node_parent( iter->Left, iter );
        if (parent == node) parent = iter;
    }

    if (!need_fixup)
    {
        if (tree->Root) tree->Root->Red = 0;
        return;
    }

    while (parent && !(child && child->Red))
    {
        right = (child == parent->Right);
        w = parent->Children[!right];
        if (w->Red)
        {
            w->Red = 0;
            parent->Red = 1;
            rtl_rotate( tree, parent, right );
            w = parent->Children[!right];
        }
        if ((w->Left && w->Left->Red) || (w->Right && w->Right->Red))
        {
            if (!(w->Children[!right] && w->Children[!right]->Red))
            {
                w->Children[right]->Red = 0;
                w->Red = 1;
                rtl_rotate( tree, w, !right );
                w = parent->Children[!right];
            }
            w->Red = parent->Red;
            parent->Red = 0;
            if (w->Children[!right]) w->Children[!right]->Red = 0;
            rtl_rotate( tree, parent, right );
            child = NULL;
            break;
        }
        w->Red = 1;
        child = parent;
        parent = rtl_node_parent( child );
    }
    if (child) child->Red = 0;
    if (tree->Root) tree->Root->Red = 0;
}

/* ---- memory, byte order, random numbers ------------------------------------------------------------------------------------------------------------------ */

EXPORT SIZE_T NTAPI RtlCompareMemoryUlong(void *src, SIZE_T len, ULONG val) {
  SIZE_T i = 0;
  len /= sizeof(ULONG);
  while (i < len && ((ULONG *)src)[i] == val) i++;
  return i * sizeof(ULONG);
}
EXPORT void NTAPI RtlFillMemoryUlong(ULONG *dest, ULONG len, ULONG val) {
  len /= sizeof(ULONG);
  while (len--) *dest++ = val;
}
EXPORT USHORT NTAPI RtlUshortByteSwap(USHORT v) { return (USHORT)((v >> 8) | (v << 8)); }
EXPORT ULONG NTAPI RtlUlongByteSwap(ULONG v) { return __builtin_bswap32(v); }
EXPORT ULONGLONG NTAPI RtlUlonglongByteSwap(ULONGLONG v) { return __builtin_bswap64(v); }
EXPORT ULONG NTAPI RtlUniform(ULONG *seed) { return (*seed = (ULONG)(((ULONGLONG)*seed * 0x7fffffed + 0x7fffffc3) % 0x7fffffff)); }
EXPORT ULONG NTAPI RtlRandom(PULONG seed) {
  static ULONG saved_value[128] = {
    0x4c8bc0aa, 0x4c022957, 0x2232827a, 0x2f1e7626, 0x7f8bdafb, 0x5c37d02a, 0x0ab48f72, 0x2f0c4ffa,
    0x290e1954, 0x6b635f23, 0x5d3885c0, 0x74b49ff8, 0x5155fa54, 0x6214ad3f, 0x111e9c29, 0x242a3a09,
    0x75932ae1, 0x40ac432e, 0x54f7ba7a, 0x585ccbd5, 0x6df5c727, 0x0374dad1, 0x7112b3f1, 0x735fc311,
    0x404331a9, 0x74d97781, 0x64495118, 0x323e04be, 0x5974b425, 0x4862e393, 0x62389c1d, 0x28a68b82,
    0x0f95da37, 0x7a50bbc6, 0x09b0091c, 0x22cdb7b4, 0x4faaed26, 0x66417ccd, 0x189e4bfa, 0x1ce4e8dd,
    0x5274c742, 0x3bdcf4dc, 0x2d94e907, 0x32eac016, 0x26d33ca3, 0x60415a8a, 0x31f57880, 0x68c8aa52,
    0x23eb16da, 0x6204f4a1, 0x373927c1, 0x0d24eb7c, 0x06dd7379, 0x2b3be507, 0x0f9c55b1, 0x2c7925eb,
    0x36d67c9a, 0x42f831d9, 0x5e3961cb, 0x65d637a8, 0x24bb3820, 0x4d08e33d, 0x2188754f, 0x147e409e,
    0x6a9620a0, 0x62e26657, 0x7bd8ce81, 0x11da0abb, 0x5f9e7b50, 0x23e444b6, 0x25920c78, 0x5fc894f0,
    0x5e338cbb, 0x404237fd, 0x1d60f80f, 0x320a1743, 0x76013d2b, 0x070294ee, 0x695e243b, 0x56b177fd,
    0x752492e1, 0x6decd52f, 0x125f5219, 0x139d2e78, 0x1898d11e, 0x2f7ee785, 0x4db405d8, 0x1a028a35,
    0x63f6f323, 0x1f6d0078, 0x307cfd67, 0x3f32a78a, 0x6980796c, 0x462b3d83, 0x34b639f2, 0x53fce379,
    0x74ba50f4, 0x1abc2c4b, 0x5eeaeb8d, 0x335a7a0d, 0x3973dd20, 0x0462d66b, 0x159813ff, 0x1e4643fd,
    0x06bc5c62, 0x3115e3fc, 0x09101613, 0x47af2515, 0x4f11ec54, 0x78b99911, 0x3db8dd44, 0x1ec10b9b,
    0x5b5506ca, 0x773ce092, 0x567be81a, 0x5475b975, 0x7a2cde1a, 0x494536f5, 0x34737bb4, 0x76d9750b,
    0x2a1f6232, 0x2e49644d, 0x7dddcbe7, 0x500cebdb, 0x619dab9e, 0x48c626fe, 0x1cda3193, 0x52dabe9d };
  const ULONG rand = (ULONG)(((ULONGLONG)*seed * 0x7fffffed + 0x7fffffc3) % 0x7fffffff);
  *seed = (ULONG)(((ULONGLONG)rand * 0x7fffffed + 0x7fffffc3) % 0x7fffffff);
  const int pos = *seed & 0x7f;
  const ULONG result = saved_value[pos];
  saved_value[pos] = rand;
  return result;
}
/* The cookie that pointers are encoded with: the process cookie of the kernel. */
static ULONG process_cookie_value(void) {
  static ULONG cookie;
  if (!cookie) {
    ULONG c = 0;
    NtQueryInformationProcess((HANDLE)-1, 36 /* ProcessCookie */, &c, sizeof(c), NULL);
    __atomic_store_n(&cookie, c ? c : 0x2a5bd7c3u, __ATOMIC_RELAXED);
  }
  return cookie;
}
EXPORT PVOID NTAPI RtlEncodePointer(PVOID ptr) {
  const ULONG cookie = process_cookie_value();
  const ULONG rotate = cookie % 64;
  const ULONG_PTR value = (ULONG_PTR)ptr ^ cookie;
  return (void *)((value >> rotate) | (value << ((64 - rotate) % 64)));
}
EXPORT PVOID NTAPI RtlDecodePointer(PVOID ptr) {
  const ULONG cookie = process_cookie_value();
  const ULONG rotate = cookie % 64;
  ULONG_PTR value = (ULONG_PTR)ptr;
  value = (value << rotate) | (value >> ((64 - rotate) % 64));
  return (void *)(value ^ cookie);
}
EXPORT DWORD NTAPI RtlComputeCrc32(DWORD initial, const BYTE *data, INT length) {
  DWORD crc = ~initial;
  for (INT i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}
EXPORT BOOL NTAPI RtlIsCriticalSectionLocked(RTL_CRITICAL_SECTION *crit) { return crit->RecursionCount != 0; }
EXPORT BOOL NTAPI RtlIsCriticalSectionLockedByThread(RTL_CRITICAL_SECTION *crit) { return crit->OwningThread == (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb()) && crit->RecursionCount; }

/* ---- handle tables ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct HandleEntry { struct HandleEntry *Next; } RTL_HANDLE_;
typedef struct {
  ULONG MaxHandleCount, HandleSize, Unused[2];
  PVOID NextFree, FirstHandle, ReservedMemory, MaxHandle;
} RTL_HANDLE_TABLE_;
EXPORT void NTAPI RtlInitializeHandleTable(ULONG max_count, ULONG handle_size, RTL_HANDLE_TABLE_ *table) {
  memset(table, 0, sizeof(*table));
  table->MaxHandleCount = max_count;
  table->HandleSize = handle_size;
}
EXPORT NTSTATUS NTAPI RtlDestroyHandleTable(RTL_HANDLE_TABLE_ *table) {
  SIZE_T size = 0;
  return NtFreeVirtualMemory((HANDLE)-1, &table->FirstHandle, &size, 0x8000 /* MEM_RELEASE */);
}
static NTSTATUS handle_table_grow(RTL_HANDLE_TABLE_ *table) {
  if (!table->FirstHandle) {
    PVOID first = NULL;
    SIZE_T max_size = (SIZE_T)table->MaxHandleCount * table->HandleSize;
    const NTSTATUS status = NtAllocateVirtualMemory((HANDLE)-1, &first, 0, &max_size, 0x2000 /* MEM_RESERVE */, 4);
    if (status) return status;
    table->FirstHandle = first;
    table->ReservedMemory = first;
    table->MaxHandle = (char *)first + max_size;
  }
  if (!table->NextFree) {
    SIZE_T offset, commit = 4096;
    RTL_HANDLE_ *free_handle = NULL;
    PVOID next = table->ReservedMemory;
    if (table->ReservedMemory >= table->MaxHandle) return 0xC0000017u;      /* STATUS_NO_MEMORY: the table is full */
    const NTSTATUS status = NtAllocateVirtualMemory((HANDLE)-1, &next, 0, &commit, 0x1000 /* MEM_COMMIT */, 4);
    if (status) return status;
    for (offset = 0; offset < commit; offset += table->HandleSize) {
      if ((char *)table->ReservedMemory + offset >= (char *)table->MaxHandle) break;
      free_handle = (RTL_HANDLE_ *)((char *)table->ReservedMemory + offset);
      free_handle->Next = (RTL_HANDLE_ *)((char *)table->ReservedMemory + offset + table->HandleSize);
    }
    if (!free_handle) return 0xC0000017u;
    free_handle->Next = NULL;
    table->NextFree = table->ReservedMemory;
    table->ReservedMemory = (char *)table->ReservedMemory + commit;
  }
  return STATUS_SUCCESS;
}
EXPORT RTL_HANDLE_ *NTAPI RtlAllocateHandle(RTL_HANDLE_TABLE_ *table, ULONG *index) {
  if (!table->NextFree && handle_table_grow(table) != STATUS_SUCCESS) return NULL;
  RTL_HANDLE_ *ret = table->NextFree;
  table->NextFree = ret->Next;
  if (index) *index = (ULONG)(((char *)ret - (char *)table->FirstHandle) / table->HandleSize);
  return ret;
}
EXPORT BOOLEAN NTAPI RtlFreeHandle(RTL_HANDLE_TABLE_ *table, RTL_HANDLE_ *handle) {
  memset(handle, 0, table->HandleSize);
  handle->Next = table->NextFree;
  table->NextFree = handle;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlIsValidHandle(const RTL_HANDLE_TABLE_ *table, const RTL_HANDLE_ *handle) {
  return handle >= (RTL_HANDLE_ *)table->FirstHandle && handle < (RTL_HANDLE_ *)table->ReservedMemory && !((ULONG_PTR)handle & (table->HandleSize - 1)) && ((ULONG_PTR)handle->Next & 1);
}
EXPORT BOOLEAN NTAPI RtlIsValidIndexHandle(const RTL_HANDLE_TABLE_ *table, ULONG index, RTL_HANDLE_ **valid) {
  RTL_HANDLE_ *handle = (RTL_HANDLE_ *)((char *)table->FirstHandle + (SIZE_T)index * table->HandleSize);
  if (RtlIsValidHandle(table, handle)) { *valid = handle; return TRUE; }
  return FALSE;
}

/* ---- relocations ------------------------------------------------------------------------------------------------------------------------------------------ */

/* Applies the relocations of one page of a mapped image; returns the entry after the last one, or NULL for a type that is not known. */
EXPORT PVOID NTAPI LdrProcessRelocationBlock(void *page, UINT count, USHORT *relocs, INT_PTR delta) {
  while (count--) {
    const USHORT offset = *relocs & 0xfff;
    const int type = *relocs >> 12;
    BYTE *at = (BYTE *)page + offset;
    switch (type) {
      case 0: break;      /* IMAGE_REL_BASED_ABSOLUTE */
      case 1: *(short *)at += (short)((delta >> 16) & 0xFFFF); break;      /* HIGH */
      case 2: *(short *)at += (short)(delta & 0xFFFF); break;      /* LOW */
      case 3: *(int *)at += (int)delta; break;      /* HIGHLOW */
      case 10: *(INT_PTR *)at += delta; break;      /* DIR64 */
      default: return NULL;
    }
    relocs++;
  }
  return relocs;
}
EXPORT NTSTATUS NTAPI RtlDeleteTimer(HANDLE queue, HANDLE timer, HANDLE event) {
  (void)queue; (void)event;
  if (!timer) return 0xC00000EFu;      /* STATUS_INVALID_PARAMETER_1 */
  return 0xC000000Du;
}

/* ---- compression (LZNT1) ---------------------------------------------------------------------------------------------------------------------------------- */

/******************************************************************************
 *  RtlGetCompressionWorkSpaceSize		[NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlGetCompressionWorkSpaceSize(USHORT format, PULONG compress_workspace,
                                               PULONG decompress_workspace)
{

    switch (format & 0x00FF)
    {
        case 2:
            if (compress_workspace)
            {
                /* FIXME: The current implementation of RtlCompressBuffer does not use a
                 * workspace buffer, but Windows applications might expect a nonzero value. */
                *compress_workspace = 16;
            }
            if (decompress_workspace)
                *decompress_workspace = 0x1000;
            return STATUS_SUCCESS;

        case 0:
        case 1:
            return STATUS_INVALID_PARAMETER_;

        default:
            return 0xC000025Fu;
    }
}

/* One chunk of up to 4096 bytes in LZNT1: groups of eight items behind a flag byte, a literal or a (displacement, length) token whose split of the 16 bits depends on how far
   into the chunk the item is. Returns the size of the compressed data, or 0 when it would not be smaller than the chunk itself. */
static ULONG lznt1_compress_chunk(const UCHAR *src, ULONG n, UCHAR *out, ULONG out_capacity) {
  ULONG in = 0, size = 0, flag_at = 0;
  UCHAR flags = 0;
  int bit = 0;
  while (in < n) {
    if (bit == 0) { if (size >= out_capacity) return 0; flag_at = size++; flags = 0; }
    ULONG disp_bits = 4;
    while (disp_bits < 12 && (1u << disp_bits) < in) ++disp_bits;      /* (enough bits for the farthest back that can be asked for) */
    const ULONG length_bits = 16 - disp_bits;
    const ULONG max_length = (1u << length_bits) + 2, max_disp = 1u << disp_bits;
    ULONG best_length = 0, best_disp = 0;
    if (in > 0 && n - in >= 3) {
      const ULONG window = in < max_disp ? in : max_disp;
      for (ULONG back = 1; back <= window; ++back) {
        ULONG length = 0;
        while (length < max_length && in + length < n && src[in + length - back] == src[in + length]) ++length;
        if (length > best_length) { best_length = length; best_disp = back; if (length == max_length) break; }
      }
    }
    if (best_length >= 3) {
      if (size + 2 > out_capacity) return 0;
      const USHORT token = (USHORT)(((best_disp - 1) << length_bits) | (best_length - 3));
      out[size++] = (UCHAR)token;
      out[size++] = (UCHAR)(token >> 8);
      flags |= (UCHAR)(1u << bit);
      in += best_length;
    } else {
      if (size >= out_capacity) return 0;
      out[size++] = src[in++];
    }
    out[flag_at] = flags;
    bit = (bit + 1) & 7;
  }
  return size < n ? size : 0;
}

/* compress data using LZNT1 */
static NTSTATUS lznt1_compress(UCHAR *src, ULONG src_size, UCHAR *dst, ULONG dst_size,
                               ULONG chunk_size, ULONG *final_size, UCHAR *workspace)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG block_size;
    (void)chunk_size; (void)workspace;

    while (src_cur < src_end)
    {
        /* determine size of current chunk */
        block_size = min(0x1000, src_end - src_cur);
        if (dst_cur + sizeof(USHORT) + 1 > dst_end)
            return STATUS_BUFFER_TOO_SMALL_;

        const ULONG room = (ULONG)(dst_end - dst_cur) - sizeof(USHORT);
        const ULONG packed = lznt1_compress_chunk(src_cur, block_size, dst_cur + sizeof(USHORT), room < block_size ? room : block_size);
        if (packed) {
            *(USHORT *)dst_cur = (USHORT)(0xB000 | (packed - 1));      /* a compressed chunk */
            dst_cur += sizeof(USHORT) + packed;
        } else {
            if (dst_cur + sizeof(USHORT) + block_size > dst_end)
                return STATUS_BUFFER_TOO_SMALL_;
            *(USHORT *)dst_cur = 0x3000 | (block_size - 1);      /* stored as it is */
            dst_cur += sizeof(USHORT);
            memcpy(dst_cur, src_cur, block_size);
            dst_cur += block_size;
        }
        src_cur += block_size;
    }

    if (final_size)
        *final_size = dst_cur - dst;

    return STATUS_SUCCESS;
}

/******************************************************************************
 *  RtlCompressBuffer		[NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlCompressBuffer(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                                  PUCHAR compressed, ULONG compressed_size, ULONG chunk_size,
                                  PULONG final_size, PVOID workspace)
{

    switch (format & 0x00FF)
    {
        case 2:
            return lznt1_compress(uncompressed, uncompressed_size, compressed,
                                  compressed_size, chunk_size, final_size, workspace);

        case 0:
        case 1:
            return STATUS_INVALID_PARAMETER_;

        default:
            return 0xC000025Fu;
    }
}

/* decompress a single LZNT1 chunk */
static UCHAR *lznt1_decompress_chunk(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG displacement_bits, length_bits;
    ULONG code_displacement, code_length;
    USHORT flags, code;

    while (src_cur < src_end && dst_cur < dst_end)
    {
        flags = 0x8000 | *src_cur++;
        while ((flags & 0xff00) && src_cur < src_end)
        {
            if (flags & 1)
            {
                /* backwards reference */
                if (src_cur + sizeof(USHORT) > src_end)
                    return NULL;

                code = *(USHORT *)src_cur;
                src_cur += sizeof(USHORT);

                /* find length / displacement bits */
                for (displacement_bits = 12; displacement_bits > 4; displacement_bits--)
                    if ((1 << (displacement_bits - 1)) < dst_cur - dst) break;

                length_bits       = 16 - displacement_bits;
                code_length       = (code & ((1 << length_bits) - 1)) + 3;
                code_displacement = (code >> length_bits) + 1;

                if (dst_cur < dst + code_displacement)
                    return NULL;

                /* copy bytes of chunk - we can't use memcpy() since source and dest can
                 * be overlapping, and the same bytes can be repeated over and over again */
                while (code_length--)
                {
                    if (dst_cur >= dst_end) return dst_cur;
                    *dst_cur = *(dst_cur - code_displacement);
                    dst_cur++;
                }
            }
            else
            {
                /* uncompressed data */
                if (dst_cur >= dst_end) return dst_cur;
                *dst_cur++ = *src_cur++;
            }
            flags >>= 1;
        }
    }

    return dst_cur;
}

/* decompress data encoded with LZNT1 */
static NTSTATUS lznt1_decompress(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size,
                                 ULONG offset, ULONG *final_size, UCHAR *workspace, int fragment)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG chunk_size, block_size;
    USHORT chunk_header;
    UCHAR *ptr;

    if (src_cur + sizeof(USHORT) > src_end)
        return 0xC0000242u;

    /* skip over chunks with a distance >= 0x1000 to the destination offset */
    while (offset >= 0x1000 && src_cur + sizeof(USHORT) <= src_end)
    {
        chunk_header = *(USHORT *)src_cur;
        src_cur += sizeof(USHORT);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return 0xC0000242u;

        src_cur += chunk_size;
        offset  -= 0x1000;
    }

    /* handle partially included chunk */
    if (offset && src_cur + sizeof(USHORT) <= src_end)
    {
        chunk_header = *(USHORT *)src_cur;
        src_cur += sizeof(USHORT);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return 0xC0000242u;

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            if (!workspace) return STATUS_ACCESS_VIOLATION;
            ptr = lznt1_decompress_chunk(workspace, 0x1000, src_cur, chunk_size);
            if (!ptr) return 0xC0000242u;
            if (ptr - workspace > offset)
            {
                block_size = min((ptr - workspace) - offset, dst_end - dst_cur);
                memcpy(dst_cur, workspace + offset, block_size);
                dst_cur += block_size;
            }
            else
            {
                /* the offset is behind the data of the chunk: the rest of the chunk is padding */
                block_size = min(0x1000 - offset, dst_end - dst_cur);
                memset(dst_cur, 0, block_size);
                dst_cur += block_size;
            }
        }
        else
        {
            /* uncompressed chunk (a fragment cannot be taken from one) */
            if (fragment) return 0xC0000242u;
            if (chunk_size > offset)
            {
                block_size = min(chunk_size - offset, dst_end - dst_cur);
                memcpy(dst_cur, src_cur + offset, block_size);
                dst_cur += block_size;
            }
        }

        src_cur += chunk_size;
    }

    /* handle remaining chunks */
    while (src_cur + sizeof(USHORT) <= src_end)
    {
        chunk_header = *(USHORT *)src_cur;
        src_cur += sizeof(USHORT);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return 0xC0000242u;

        /* fill space with padding when the previous chunk was decompressed
         * to less than 4096 bytes. no padding is needed for the last chunk
         * or when the next chunk is truncated */
        block_size = ((dst_cur - dst) + offset) & 0xfff;
        if (block_size)
        {
            block_size = 0x1000 - block_size;
            if (dst_cur + block_size >= dst_end)
                goto out;
            memset(dst_cur, 0, block_size);
            dst_cur += block_size;
        }

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            dst_cur = lznt1_decompress_chunk(dst_cur, dst_end - dst_cur, src_cur, chunk_size);
            if (!dst_cur) return 0xC0000242u;
        }
        else
        {
            /* uncompressed chunk */
            if (fragment) return 0xC0000242u;
            block_size = min(chunk_size, dst_end - dst_cur);
            memcpy(dst_cur, src_cur, block_size);
            dst_cur += block_size;
        }

        src_cur += chunk_size;
    }

out:
    if (final_size)
        *final_size = dst_cur - dst;

    return STATUS_SUCCESS;

}

/******************************************************************************
 *  RtlDecompressFragment	[NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlDecompressFragment(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                               PUCHAR compressed, ULONG compressed_size, ULONG offset,
                               PULONG final_size, PVOID workspace)
{

    switch (format & 0x00FF)
    {
        case 2:
            return lznt1_decompress(uncompressed, uncompressed_size, compressed,
                                    compressed_size, offset, final_size, workspace, 1);

        case 0:
        case 1:
            return STATUS_INVALID_PARAMETER_;

        default:
            return 0xC000025Fu;
    }
}


/******************************************************************************
 *  RtlDecompressBuffer		[NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlDecompressBuffer(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                                    PUCHAR compressed, ULONG compressed_size, PULONG final_size)
{

    switch (format & 0x00FF)
    {
        case 2: return lznt1_decompress(uncompressed, uncompressed_size, compressed, compressed_size, 0, final_size, NULL, 0);
        case 0:
        case 1: return STATUS_INVALID_PARAMETER_;
        default: return 0xC000025Fu;
    }
}

/* ---- debug output ------------------------------------------------------------------------------------------------------------------------------------------ */

IMPORT void NTAPI RtlCaptureContext(CONTEXT *);
static BOOL debugger_handled(EXCEPTION_RECORD *record, CONTEXT *context, BOOLEAN first_chance);
EXPORT BOOLEAN NTAPI RtlDispatchException(EXCEPTION_RECORD *record, CONTEXT *context);

/* Prints formatted text for the debugger: when the process is being debugged the text goes to it as a DBG_PRINTEXCEPTION_C exception. Only the error level (bit 0 of the level
   mask, or the level 0 itself) is shown. */
EXPORT NTSTATUS NTAPI vDbgPrintExWithPrefix(const char *prefix, ULONG id, ULONG level, const char *fmt, va_list_ args) {
  const ULONG level_mask = level <= 31 ? (1u << level) : level;
  char buf[1024];
  size_t_ len = strlen(prefix);
  (void)id;
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, prefix, len);
  buf[len] = 0;
  const int written = _vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
  if (written < 0) buf[sizeof(buf) - 1] = 0;
  if ((level_mask & 1u) && NtCurrentPeb()->BeingDebugged) {
    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = 0x40010006u;      /* DBG_PRINTEXCEPTION_C */
    record.ExceptionAddress = (PVOID)RtlRaiseException;
    record.NumberParameters = 2;
    record.ExceptionInformation[1] = (ULONG_PTR)buf;
    record.ExceptionInformation[0] = strlen(buf) + 1;
    CONTEXT context;
    RtlCaptureContext(&context);
    if (!debugger_handled(&record, &context, TRUE)) RtlDispatchException(&record, &context);      /* (nobody has to want it: the print goes on either way) */
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI vDbgPrintEx(ULONG id, ULONG level, const char *fmt, va_list_ args) { return vDbgPrintExWithPrefix("", id, level, fmt, args); }
EXPORT NTSTATUS DbgPrintEx(ULONG id, ULONG level, const char *fmt, ...) {
  va_list_ args;
  __builtin_va_start(args, fmt);
  const NTSTATUS status = vDbgPrintEx(id, level, fmt, args);
  __builtin_va_end(args);
  return status;
}
EXPORT NTSTATUS DbgPrint(const char *fmt, ...) {
  va_list_ args;
  __builtin_va_start(args, fmt);
  const NTSTATUS status = vDbgPrintEx(0, 0, fmt, args);
  __builtin_va_end(args);
  return status;
}

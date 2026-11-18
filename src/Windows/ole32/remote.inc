/* ole32: objects in other processes. A program that creates an object of a class served by another program (CLSCTX_LOCAL_SERVER: a "local server", which may be a
 * service) gets a proxy; calls on the proxy travel to the server's process, are made there on the real object, and what comes back travels home. Interfaces that are
 * passed as arguments are objects of the other side in turn (a callback is a call the other way).
 *
 * This is this system's own design, not DCOM's wire protocol - both ends are always this library:
 *
 *   endpoint     Every process that hands objects out listens on a named pipe, "\\.\pipe\tcos-com-<its process id in hexadecimal>". A thread accepts connections, and
 *                each connection has a thread of its own that reads a request, carries it out and writes the reply, one after another. A caller keeps its
 *                connections to a server open and uses a free one (or makes one more) for each call, so calls from several threads and calls inside calls
 *                do not wait for each other. All incoming calls arrive on these threads, whatever apartment made the object (as for a free-threaded object).
 *   objects      The standard marshaler (marshal.inc) writes the same OBJREF as ever; the OXID in it now ends in the process id. Unmarshaling in another process
 *                asks the owner to hand the marshaled reference over to it (kRemoteUnmarshal) and makes a proxy for the object's OID. The owner keeps what each
 *                client process holds (RemoteRef) and lets go when the client releases its proxy, or when its last connection closes (it ended).
 *   interfaces   A proxy answers QueryInterface by asking the owner. IClassFactory is built in. Any other interface needs a type library (the registry's
 *                Interface\{iid}\TypeLib, as an "oleautomation" interface has: Windows' universal marshaler does the same): its methods' parameters say what
 *                to send. The proxy's virtual table is the same for every interface - 64 small pieces of code that pass their slot number and the caller's
 *                arguments to remote_proxy_call. The owner's side makes the call with the arguments rebuilt (remote_stub_call, remote_invoke).
 *                Sent as they are: integers of every size, BSTR and text that ends in a zero, interface pointers, VARIANT (of those), and pointers to those for results. Not: structures,
 *                arrays, floating point arguments in registers (64-bit), IDispatch's own methods.
 *   activation   A server announces a class it registered for CLSCTX_LOCAL_SERVER in the registry (HKLM\SOFTWARE\TheoryCraftOS\COM\Running, the class and its
 *                process id; the registry is in memory and shared). CoGetClassObject looks there; if nobody serves the class it starts what the registry
 *                names - the service of the class's AppID (LocalService, with its ServiceParameters) or its LocalServer32 program - and waits for the
 *                announcement.
 *
 * There is no security here: any process may call any object another one handed out, and CoImpersonateClient says "the caller is in this process". */

/* ---- oleaut32, used when it is there (BSTRs and type libraries are its) ---------------------------------------------------------------------------------- */

typedef struct Automation {
  BSTR (WINAPI *alloc_len)(const OLECHAR *, UINT);
  void (WINAPI *free_string)(BSTR);
  UINT (WINAPI *string_len)(BSTR);
  HRESULT (WINAPI *load_reg_type_lib)(REFGUID, WORD, WORD, LCID, ITypeLib **);
  HRESULT (WINAPI *variant_clear)(VARIANTARG *);
} Automation;
static Automation g_automation;
static LONG g_automation_state;      /* 0: not looked at, 1: there, 2: not there */

static const Automation *automation(void) {
  if (g_automation_state == 0) {
    Automation a;
    HMODULE module = LoadLibraryW(W('o', 'l', 'e', 'a', 'u', 't', '3', '2', '.', 'd', 'l', 'l'));
    mem_zero(&a, sizeof(a));
    if (module) {
      a.alloc_len = (void *)GetProcAddress(module, "SysAllocStringLen");
      a.free_string = (void *)GetProcAddress(module, "SysFreeString");
      a.string_len = (void *)GetProcAddress(module, "SysStringLen");
      a.load_reg_type_lib = (void *)GetProcAddress(module, "LoadRegTypeLib");
      a.variant_clear = (void *)GetProcAddress(module, "VariantClear");
    }
    const BOOL whole = a.alloc_len && a.free_string && a.string_len && a.load_reg_type_lib && a.variant_clear;
    lock();
    if (g_automation_state == 0) {
      g_automation = a;
      g_automation_state = whole ? 1 : 2;
    }
    unlock();
  }
  return g_automation_state == 1 ? &g_automation : NULL;
}

/* ---- a buffer of bytes that is written to and read from ---------------------------------------------------------------------------------------------------- */

#define REMOTE_MAX_MESSAGE (16u << 20)

typedef struct RBuf {
  BYTE *data;
  DWORD size, capacity, at;
  BOOL bad;      /* memory ran out while writing, or a read went past the end: everything after gives zeros */
} RBuf;

static void rbuf_free(RBuf *b) { mem_free(b->data); mem_zero(b, sizeof(*b)); }
static void rbuf_put(RBuf *b, const void *bytes, DWORD n) {
  if (b->bad || n > REMOTE_MAX_MESSAGE || b->size > REMOTE_MAX_MESSAGE - n) { b->bad = TRUE; return; }
  if (b->size + n > b->capacity) {
    DWORD wanted = b->capacity ? b->capacity * 2 : 256;
    while (wanted < b->size + n) wanted *= 2;
    BYTE *grown = mem_realloc(b->data, wanted);
    if (!grown) { b->bad = TRUE; return; }
    b->data = grown;
    b->capacity = wanted;
  }
  mem_copy(b->data + b->size, bytes, n);
  b->size += n;
}
static void rbuf_put32(RBuf *b, DWORD v) { rbuf_put(b, &v, 4); }
static void rbuf_put64(RBuf *b, ULONGLONG v) { rbuf_put(b, &v, 8); }
static const BYTE *rbuf_take(RBuf *b, DWORD n) {
  if (b->bad || n > b->size - b->at) { b->bad = TRUE; return NULL; }
  const BYTE *p = b->data + b->at;
  b->at += n;
  return p;
}
static DWORD rbuf_get32(RBuf *b) { DWORD v = 0; const BYTE *p = rbuf_take(b, 4); if (p) mem_copy(&v, p, 4); return v; }
static ULONGLONG rbuf_get64(RBuf *b) { ULONGLONG v = 0; const BYTE *p = rbuf_take(b, 8); if (p) mem_copy(&v, p, 8); return v; }

/* An interface pointer as its OBJREF (a length first; 0: a null pointer), and back. */
static void rbuf_put_interface(RBuf *b, REFIID iid, IUnknown *unk) {
  IStream *stream = NULL;
  HGLOBAL memory = NULL;
  STATSTG stat;
  if (!unk) { rbuf_put32(b, 0); return; }
  if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) { b->bad = TRUE; return; }
  if (SUCCEEDED(CoMarshalInterface(stream, iid, unk, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL)) && SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) &&
      SUCCEEDED(GetHGlobalFromStream(stream, &memory)) && stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart < 4096) {
    const BYTE *bytes = GlobalLock(memory);
    rbuf_put32(b, (DWORD)stat.cbSize.QuadPart);
    if (bytes) rbuf_put(b, bytes, (DWORD)stat.cbSize.QuadPart);
    else b->bad = TRUE;
    GlobalUnlock(memory);
  } else {
    b->bad = TRUE;
  }
  IStream_Release(stream);
}
static HRESULT rbuf_get_interface(RBuf *b, REFIID iid, void **out) {
  IStream *stream = NULL;
  *out = NULL;
  const DWORD size = rbuf_get32(b);
  if (size == 0) return b->bad ? RPC_E_INVALID_DATAPACKET : S_OK;
  const BYTE *bytes = rbuf_take(b, size);
  if (!bytes) return RPC_E_INVALID_DATAPACKET;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
  if (FAILED(hr)) return hr;
  hr = stream_write_all(stream, bytes, size);
  if (SUCCEEDED(hr)) hr = stream_seek_to(stream, 0);
  if (SUCCEEDED(hr)) hr = CoUnmarshalInterface(stream, iid, out);
  IStream_Release(stream);
  return hr;
}

/* ---- messages ------------------------------------------------------------------------------------------------------------------------------------------------ */

#define REMOTE_MAGIC 0x4D4F4354u      /* "TCOM" */
enum {
  kRemoteUnmarshal = 1,        /* iid: the IPID of a marshaled reference, which becomes the calling process's; the reply's payload is the object's OID */
  kRemoteRelease = 2,          /* oid, method: how many references the calling process gives back */
  kRemoteQuery = 3,            /* oid, iid: does the object have that interface? */
  kRemoteCall = 4,             /* oid, iid, method (the slot of the virtual table); the payload is the arguments, the reply's the result and what the method gave back */
  kRemoteGetClass = 5,         /* iid: a class; the payload is the interface wanted; the reply's payload is the class object, marshaled */
  kRemoteCreateInstance = 6,   /* oid: a class object; the payload is the interface wanted; the reply's payload is the new object, marshaled */
};

typedef struct RemoteRequest {
  DWORD magic, op, pid, size;      /* pid: who asks; size: of the payload that follows */
  ULONGLONG oid;
  GUID iid;
  DWORD method, reserved;
} RemoteRequest;
typedef struct RemoteReply {
  DWORD magic;
  HRESULT status;                  /* of carrying the request out (the result of a method is in the payload) */
  DWORD size, reserved;
} RemoteReply;

static void remote_pipe_name(DWORD pid, WCHAR *out) {
  const WCHAR *prefix = W('\\', '\\', '.', '\\', 'p', 'i', 'p', 'e', '\\', 't', 'c', 'o', 's', '-', 'c', 'o', 'm', '-');
  while (*prefix) *out++ = *prefix++;
  for (int shift = 28; shift >= 0; shift -= 4) *out++ = (WCHAR)"0123456789abcdef"[(pid >> shift) & 15];
  *out = 0;
}
static BOOL pipe_read_all(HANDLE pipe, void *buffer, DWORD n) {
  BYTE *at = buffer;
  while (n) {
    DWORD got = 0;
    if (!ReadFile(pipe, at, n, &got, NULL) || got == 0) return FALSE;
    at += got;
    n -= got;
  }
  return TRUE;
}
static BOOL pipe_write_all(HANDLE pipe, const void *buffer, DWORD n) {
  const BYTE *at = buffer;
  while (n) {
    DWORD done = 0;
    if (!WriteFile(pipe, at, n, &done, NULL) || done == 0) return FALSE;
    at += done;
    n -= done;
  }
  return TRUE;
}

/* ---- the calling side: connections ---------------------------------------------------------------------------------------------------------------------------- */

typedef struct RemoteConnection {
  struct RemoteConnection *next;
  DWORD pid;
  HANDLE pipe;
  BOOL busy;
} RemoteConnection;
static RemoteConnection *g_connections;

/* A connection to that process that no call is using: one that is open, else a new one. */
static RemoteConnection *remote_connect(DWORD pid) {
  WCHAR name[40];
  RemoteConnection *c;
  lock();
  for (c = g_connections; c; c = c->next) if (c->pid == pid && !c->busy) { c->busy = TRUE; break; }
  unlock();
  if (c) return c;
  remote_pipe_name(pid, name);
  HANDLE pipe = INVALID_HANDLE_VALUE;
  /* (the owner makes the next instance of its pipe when it has taken a connection: for a moment there may be none, or none free) */
  for (int attempt = 0; attempt < 250 && pipe == INVALID_HANDLE_VALUE; attempt++) {
    pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && !(error == ERROR_FILE_NOT_FOUND && attempt < 25)) break;
    Sleep(20);
  }
  if (pipe == INVALID_HANDLE_VALUE) return NULL;
  c = mem_alloc(sizeof(*c));
  if (!c) { CloseHandle(pipe); return NULL; }
  c->pid = pid;
  c->pipe = pipe;
  c->busy = TRUE;
  lock();
  c->next = g_connections;
  g_connections = c;
  unlock();
  return c;
}
static void remote_connection_done(RemoteConnection *c, BOOL broken) {
  lock();
  if (broken) {
    for (RemoteConnection **link = &g_connections, *x; (x = *link) != NULL; link = &x->next) if (x == c) { *link = x->next; break; }
  } else {
    c->busy = FALSE;
  }
  unlock();
  if (broken) { CloseHandle(c->pipe); mem_free(c); }
}

/* Sends a request to the process `pid` and waits for its reply, whose payload goes into `reply` (may be NULL). The status is the owner's, or says that it
 * cannot be reached. */
static HRESULT remote_transact(DWORD pid, DWORD op, ULONGLONG oid, REFGUID iid, DWORD method, const RBuf *payload, RBuf *reply) {
  RemoteRequest request;
  RemoteReply answer;
  if (payload && payload->bad) return E_OUTOFMEMORY;
  RemoteConnection *c = remote_connect(pid);
  if (!c) return HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
  mem_zero(&request, sizeof(request));
  request.magic = REMOTE_MAGIC;
  request.op = op;
  request.pid = GetCurrentProcessId();
  request.size = payload ? payload->size : 0;
  request.oid = oid;
  if (iid) request.iid = *iid;
  request.method = method;
  BOOL ok = pipe_write_all(c->pipe, &request, sizeof(request)) && (request.size == 0 || pipe_write_all(c->pipe, payload->data, request.size)) &&
            pipe_read_all(c->pipe, &answer, sizeof(answer)) && answer.magic == REMOTE_MAGIC && answer.size <= REMOTE_MAX_MESSAGE;
  if (ok && answer.size) {
    BYTE *bytes = mem_alloc(answer.size);
    ok = bytes && pipe_read_all(c->pipe, bytes, answer.size);
    if (ok && reply) { reply->data = bytes; reply->size = reply->capacity = answer.size; reply->at = 0; reply->bad = FALSE; }
    else mem_free(bytes);
  }
  remote_connection_done(c, !ok);
  return ok ? answer.status : RPC_E_DISCONNECTED;
}

/* ---- the owning side: what other processes hold --------------------------------------------------------------------------------------------------------------- */

typedef struct RemoteRef {
  struct RemoteRef *next;
  ULONGLONG oid;
  IUnknown *identity;      /* one reference of the object, for as long as this process holds any */
  DWORD pid;
  LONG count;
} RemoteRef;
static RemoteRef *g_remote_refs;

typedef struct RemoteClient { struct RemoteClient *next; DWORD pid; LONG connections; } RemoteClient;
static RemoteClient *g_remote_clients;

/* The object another process named by its OID, with a reference (NULL: nobody holds that object). */
static IUnknown *remote_object(ULONGLONG oid) {
  IUnknown *found = NULL;
  lock();
  for (RemoteRef *r = g_remote_refs; r; r = r->next) if (r->oid == oid) { found = r->identity; IUnknown_AddRef(found); break; }
  unlock();
  return found;
}
/* `count` references less (0: all) for the process `pid` on the object `oid` (0: on every object: the process is gone). */
static void remote_drop(DWORD pid, ULONGLONG oid, LONG count) {
  for (;;) {
    IUnknown *released = NULL;
    lock();
    for (RemoteRef **link = &g_remote_refs, *r; (r = *link) != NULL; link = &r->next) {
      if (r->pid != pid || (oid && r->oid != oid)) continue;
      if (count > 0 && r->count > count) { r->count -= count; break; }
      *link = r->next;
      released = r->identity;
      mem_free(r);
      break;
    }
    unlock();
    if (!released) return;
    IUnknown_Release(released);
    if (oid) return;
  }
}
/* The marshaled reference `ipid` becomes the process `pid`'s. */
static HRESULT remote_claim(REFGUID ipid, DWORD pid, ULONGLONG *oid) {
  IUnknown *identity = NULL;
  HRESULT hr = std_claim(ipid, &identity, oid);      /* (marshal.inc: the entry is used up as an unmarshal in this process would use it; the reference is ours now) */
  if (FAILED(hr)) return hr;
  RemoteRef *fresh = mem_alloc(sizeof(*fresh)), *r;
  if (!fresh) { IUnknown_Release(identity); return E_OUTOFMEMORY; }
  lock();
  for (r = g_remote_refs; r; r = r->next) if (r->oid == *oid && r->pid == pid) break;
  if (r) {
    r->count++;
  } else {
    fresh->oid = *oid;
    fresh->identity = identity;
    fresh->pid = pid;
    fresh->count = 1;
    fresh->next = g_remote_refs;
    g_remote_refs = fresh;
  }
  unlock();
  if (r) { IUnknown_Release(identity); mem_free(fresh); }
  return S_OK;
}

static HRESULT remote_stub_call(IUnknown *identity, REFIID iid, DWORD slot, RBuf *in, RBuf *out);      /* (below) */
/* One request, carried out. */
static HRESULT remote_dispatch(const RemoteRequest *request, RBuf *in, RBuf *out) {
  IUnknown *object = NULL, *made = NULL;
  IClassFactory *factory = NULL;
  ULONGLONG oid = 0;
  GUID wanted;
  HRESULT hr;
  BOOL found = FALSE;
  switch (request->op) {
    case kRemoteUnmarshal:
      hr = remote_claim(&request->iid, request->pid, &oid);
      if (SUCCEEDED(hr)) rbuf_put64(out, oid);
      return hr;
    case kRemoteRelease:
      remote_drop(request->pid, request->oid, request->method ? (LONG)request->method : 1);
      return S_OK;
    case kRemoteQuery:
      object = remote_object(request->oid);
      if (!object) return RPC_E_DISCONNECTED;
      hr = IUnknown_QueryInterface(object, &request->iid, (void **)&made);
      if (SUCCEEDED(hr) && made) IUnknown_Release(made);
      IUnknown_Release(object);
      return hr;
    case kRemoteCall:
      object = remote_object(request->oid);
      if (!object) return RPC_E_DISCONNECTED;
      hr = remote_stub_call(object, &request->iid, request->method, in, out);
      IUnknown_Release(object);
      return hr;
    case kRemoteGetClass: {
      const BYTE *bytes = rbuf_take(in, sizeof(GUID));
      if (!bytes) return RPC_E_INVALID_DATAPACKET;
      mem_copy(&wanted, bytes, sizeof(GUID));
      hr = get_registered_class(&request->iid, CLSCTX_LOCAL_SERVER, &wanted, (void **)&made, &found);
      if (!found) return REGDB_E_CLASSNOTREG;
      if (FAILED(hr)) return hr;
      rbuf_put_interface(out, &wanted, made);
      IUnknown_Release(made);
      return out->bad ? E_OUTOFMEMORY : S_OK;
    }
    case kRemoteCreateInstance: {
      const BYTE *bytes = rbuf_take(in, sizeof(GUID));
      if (!bytes) return RPC_E_INVALID_DATAPACKET;
      mem_copy(&wanted, bytes, sizeof(GUID));
      object = remote_object(request->oid);
      if (!object) return RPC_E_DISCONNECTED;
      hr = IUnknown_QueryInterface(object, &IID_IClassFactory, (void **)&factory);
      IUnknown_Release(object);
      if (FAILED(hr)) return hr;
      hr = IClassFactory_CreateInstance(factory, NULL, &wanted, (void **)&made);
      IClassFactory_Release(factory);
      if (FAILED(hr)) return hr;
      rbuf_put_interface(out, &wanted, made);
      if (made) IUnknown_Release(made);
      return out->bad ? E_OUTOFMEMORY : S_OK;
    }
    default: return RPC_E_INVALID_DATAPACKET;
  }
}

/* A connection: requests are read and answered until the other side closes it. When the last connection of a process closes, the process has ended (it keeps
 * its connections open while it lives), and what it held is let go. */
static DWORD WINAPI remote_connection_thread(LPVOID parameter) {
  HANDLE pipe = parameter;
  RemoteClient *client = NULL;
  const BOOL initialized = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
  for (;;) {
    RemoteRequest request;
    RemoteReply reply;
    RBuf in, out;
    mem_zero(&in, sizeof(in));
    mem_zero(&out, sizeof(out));
    if (!pipe_read_all(pipe, &request, sizeof(request)) || request.magic != REMOTE_MAGIC || request.size > REMOTE_MAX_MESSAGE) break;
    if (request.size) {
      in.data = mem_alloc(request.size);
      if (!in.data || !pipe_read_all(pipe, in.data, request.size)) { rbuf_free(&in); break; }
      in.size = in.capacity = request.size;
    }
    if (!client) {      /* (the first request says who is at the other end) */
      lock();
      for (client = g_remote_clients; client; client = client->next) if (client->pid == request.pid) break;
      if (!client && (client = mem_alloc(sizeof(*client))) != NULL) { client->pid = request.pid; client->next = g_remote_clients; g_remote_clients = client; }
      if (client) client->connections++;
      unlock();
    }
    HRESULT status = remote_dispatch(&request, &in, &out);
    if (SUCCEEDED(status) && out.bad) status = E_OUTOFMEMORY;
    reply.magic = REMOTE_MAGIC;
    reply.status = status;
    reply.size = FAILED(status) && request.op != kRemoteCall ? 0 : out.size;
    reply.reserved = 0;
    const BOOL sent = pipe_write_all(pipe, &reply, sizeof(reply)) && (reply.size == 0 || pipe_write_all(pipe, out.data, reply.size));
    rbuf_free(&in);
    rbuf_free(&out);
    if (!sent) break;
  }
  CloseHandle(pipe);
  if (client) {
    lock();
    const BOOL gone = --client->connections == 0;
    const DWORD pid = client->pid;
    unlock();
    if (gone) remote_drop(pid, 0, 0);
  }
  if (initialized) CoUninitialize();
  return 0;
}

static LONG g_endpoint_state;      /* 0: none, 1: being started, 2: listening */

static DWORD WINAPI remote_listener_thread(LPVOID parameter) {
  WCHAR name[40];
  HANDLE first = parameter;
  remote_pipe_name(GetCurrentProcessId(), name);
  for (;;) {
    HANDLE pipe = first ? first : CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, NULL);
    first = NULL;
    if (pipe == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
    if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); continue; }
    HANDLE thread = CreateThread(NULL, 0, remote_connection_thread, pipe, 0, NULL);
    if (thread) CloseHandle(thread);
    else CloseHandle(pipe);
  }
  return 0;
}

/* Makes sure this process can be called: its pipe is there before this returns. */
static BOOL remote_endpoint_start(void) {
  WCHAR name[40];
  if (g_endpoint_state == 2) return TRUE;
  if (InterlockedCompareExchange(&g_endpoint_state, 1, 0) != 0) {
    for (int i = 0; i < 500 && g_endpoint_state == 1; i++) Sleep(10);
    return g_endpoint_state == 2;
  }
  remote_pipe_name(GetCurrentProcessId(), name);
  HANDLE first = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, NULL);
  HANDLE thread = first != INVALID_HANDLE_VALUE ? CreateThread(NULL, 0, remote_listener_thread, first, 0, NULL) : NULL;
  if (!thread) {
    if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
    InterlockedExchange(&g_endpoint_state, 0);
    return FALSE;
  }
  CloseHandle(thread);
  InterlockedExchange(&g_endpoint_state, 2);
  return TRUE;
}

/* ---- type information: what the methods of an interface take ------------------------------------------------------------------------------------------------ */

enum { kKindNone, kKindInt, kKindLong, kKindString, kKindInterface, kKindVariant, kKindPointer, kKindWide, kKindAnsi };      /* (Int: 32 bits; Long: 64; Wide, Ansi: text that ends in a zero) */

typedef struct RemoteParameter {
  BYTE kind;           /* how the argument itself travels */
  BYTE pointee;        /* for kKindPointer: what it points to */
  BOOL in, out;
  GUID iid;            /* of an interface (the argument's, or the pointee's) */
} RemoteParameter;

typedef struct RemoteMethod {
  BOOL known;
  UINT count;
  RemoteParameter *parameters;
} RemoteMethod;

typedef struct RemoteInterface {
  struct RemoteInterface *next;
  GUID iid;
  UINT slots;                  /* of the virtual table */
  RemoteMethod *methods;       /* by slot (the first three, and IDispatch's four, are not `known`) */
} RemoteInterface;
static RemoteInterface *g_remote_interfaces;

#define REMOTE_SLOTS 64u
#ifdef _WIN64
#define REMOTE_WORD 8u
#else
#define REMOTE_WORD 4u
#endif

/* What a type means for sending it. An alias is followed, an enumeration is a number, a pointer to an interface is the interface. */
static BYTE remote_type_kind(ITypeInfo *info, const TYPEDESC *type, GUID *iid, BYTE *pointee, UINT depth) {
  ITypeInfo *named = NULL;
  TYPEATTR *attributes = NULL;
  BYTE kind = kKindNone;
  if (depth > 8) return kKindNone;
  switch (type->vt) {
    case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_BOOL: case VT_ERROR: case VT_HRESULT: return kKindInt;
    case VT_I8: case VT_UI8: case VT_CY: case VT_DATE: return kKindLong;
#ifndef _WIN64
    case VT_R4: return kKindInt;      /* (on the stack as its bits; a 64-bit program passes it in a register this code does not read) */
    case VT_R8: return kKindLong;
#endif
    case VT_BSTR: return kKindString;
    case VT_LPWSTR: return kKindWide;      /* ([in, string] const WCHAR *: not an automation type, but type libraries have it and Chromium's interfaces use it) */
    case VT_LPSTR: return kKindAnsi;
    case VT_VARIANT: return kKindVariant;
    case VT_UNKNOWN: *iid = IID_IUnknown; return kKindInterface;
    case VT_DISPATCH: *iid = IID_IDispatch; return kKindInterface;
    case VT_USERDEFINED:
      if (FAILED(ITypeInfo_GetRefTypeInfo(info, type->hreftype, &named)) || !named) return kKindNone;
      if (SUCCEEDED(ITypeInfo_GetTypeAttr(named, &attributes)) && attributes) {
        if (attributes->typekind == TKIND_ENUM) kind = kKindInt;
        else if (attributes->typekind == TKIND_ALIAS) kind = remote_type_kind(named, &attributes->tdescAlias, iid, pointee, depth + 1);
        else if (attributes->typekind == TKIND_INTERFACE || attributes->typekind == TKIND_DISPATCH) { *iid = attributes->guid; kind = 0xFF; }      /* (an interface by value: only a pointer to it is something) */
        ITypeInfo_ReleaseTypeAttr(named, attributes);
      }
      ITypeInfo_Release(named);
      return kind;
    case VT_PTR: {
      BYTE inner_pointee = kKindNone;
      if (!type->lptdesc) return kKindNone;
      const BYTE inner = remote_type_kind(info, type->lptdesc, iid, &inner_pointee, depth + 1);
      if (inner == 0xFF) return kKindInterface;                                      /* IFoo * */
      if (inner == kKindNone || inner == kKindPointer) return kKindNone;             /* (a pointer to a pointer to a value, a structure, ...) */
      *pointee = inner;                                                              /* LONG *, BSTR *, IFoo **, VARIANT * */
      return kKindPointer;
    }
    default: return kKindNone;
  }
}

/* The methods of `info` and of the interfaces it derives from (up to IUnknown or IDispatch), by slot. */
static BOOL remote_collect_methods(ITypeInfo *info, RemoteInterface *made, UINT depth) {
  TYPEATTR *attributes = NULL;
  BOOL ok = TRUE;
  if (depth > 8 || FAILED(ITypeInfo_GetTypeAttr(info, &attributes)) || !attributes) return FALSE;
  const UINT functions = attributes->cFuncs, bases = attributes->cImplTypes, slots = attributes->cbSizeVft / REMOTE_WORD;
  ITypeInfo_ReleaseTypeAttr(info, attributes);
  if (depth == 0) {
    if (slots < 3 || slots > REMOTE_SLOTS) return FALSE;
    made->slots = slots;
    made->methods = mem_alloc(slots * sizeof(RemoteMethod));
    if (!made->methods) return FALSE;
  }
  if (bases >= 1) {
    HREFTYPE reference = 0;
    ITypeInfo *base = NULL;
    TYPEATTR *base_attributes = NULL;
    if (SUCCEEDED(ITypeInfo_GetRefTypeOfImplType(info, 0, &reference)) && SUCCEEDED(ITypeInfo_GetRefTypeInfo(info, reference, &base)) && base) {
      BOOL root = TRUE;
      if (SUCCEEDED(ITypeInfo_GetTypeAttr(base, &base_attributes)) && base_attributes) {
        root = IsEqualGUID(&base_attributes->guid, &IID_IUnknown) || IsEqualGUID(&base_attributes->guid, &IID_IDispatch);
        ITypeInfo_ReleaseTypeAttr(base, base_attributes);
      }
      if (!root) ok = remote_collect_methods(base, made, depth + 1);
      ITypeInfo_Release(base);
    }
  }
  for (UINT i = 0; ok && i < functions; i++) {
    FUNCDESC *function = NULL;
    if (FAILED(ITypeInfo_GetFuncDesc(info, i, &function)) || !function) { ok = FALSE; break; }
    const UINT slot = (UINT)function->oVft / REMOTE_WORD;
    if (function->funckind != FUNC_DISPATCH && slot >= 3 && slot < made->slots && function->cParams >= 0 && function->cParams <= 32) {
      RemoteMethod *method = &made->methods[slot];
      method->count = (UINT)function->cParams;
      method->parameters = mem_alloc((method->count ? method->count : 1) * sizeof(RemoteParameter));
      method->known = method->parameters != NULL && (function->elemdescFunc.tdesc.vt == VT_HRESULT || function->elemdescFunc.tdesc.vt == VT_VOID || function->elemdescFunc.tdesc.vt == VT_I4);
      for (UINT p = 0; method->known && p < method->count; p++) {
        const ELEMDESC *element = &function->lprgelemdescParam[p];
        RemoteParameter *parameter = &method->parameters[p];
        const USHORT flags = element->paramdesc.wParamFlags;
        parameter->kind = remote_type_kind(info, &element->tdesc, &parameter->iid, &parameter->pointee, 0);
        if (parameter->kind == kKindNone || parameter->kind == 0xFF) method->known = FALSE;      /* (a kind of argument that is not sent: the method cannot be called) */
        parameter->out = parameter->kind == kKindPointer && (flags & (PARAMFLAG_FOUT | PARAMFLAG_FRETVAL)) != 0;
        parameter->in = parameter->kind != kKindPointer || (flags & PARAMFLAG_FIN) != 0 || !parameter->out;
      }
    }
    ITypeInfo_ReleaseFuncDesc(info, function);
  }
  return ok;
}

/* The interface `iid` as its type library describes it, or NULL (no type library names it, or oleaut32 is missing). Kept for the life of the process. */
static const RemoteInterface *remote_interface(REFIID iid) {
  WCHAR path[80], text[64], version[32];
  RemoteInterface *found;
  ITypeLib *library = NULL;
  ITypeInfo *info = NULL;
  TYPEATTR *attributes = NULL;
  GUID libid;
  lock();
  for (found = g_remote_interfaces; found; found = found->next) if (IsEqualGUID(&found->iid, iid)) break;
  unlock();
  if (found) return found->methods ? found : NULL;
  const Automation *oa = automation();
  RemoteInterface *made = mem_alloc(sizeof(*made));
  if (!made) return NULL;
  made->iid = *iid;
  /* Interface\{iid}\TypeLib: the library (and its version, "1.0" if the registry does not say). */
  WCHAR *p = path;
  for (const WCHAR *prefix = W('I', 'n', 't', 'e', 'r', 'f', 'a', 'c', 'e', '\\'); *prefix;) *p++ = *prefix++;
  guid_to_text(iid, p);
  p += wlen(p);
  for (const WCHAR *suffix = W('\\', 'T', 'y', 'p', 'e', 'L', 'i', 'b'); *suffix;) *p++ = *suffix++;
  *p = 0;
  if (oa && reg_class_string(path, NULL, text, 64) == ERROR_SUCCESS && SUCCEEDED(CLSIDFromString(text, &libid))) {
    WORD major = 1, minor = 0;
    if (reg_class_string(path, W('V', 'e', 'r', 's', 'i', 'o', 'n'), version, 32) == ERROR_SUCCESS) {
      UINT numbers[2] = {0, 0}, which = 0;
      for (const WCHAR *v = version; *v && which < 2; v++) {
        if (*v == '.') which++;
        else if (*v >= '0' && *v <= '9') numbers[which] = numbers[which] * 16 + (UINT)(*v - '0');
        else if ((*v | 32) >= 'a' && (*v | 32) <= 'f') numbers[which] = numbers[which] * 16 + (UINT)((*v | 32) - 'a' + 10);
      }
      major = (WORD)numbers[0];
      minor = (WORD)numbers[1];
    }
    if (SUCCEEDED(oa->load_reg_type_lib(&libid, major, minor, 0, &library)) && library) {
      if (SUCCEEDED(ITypeLib_GetTypeInfoOfGuid(library, iid, &info)) && info) {
        /* (a dual interface is listed as a dispatch interface: its virtual table is the interface that one is "implemented" by) */
        if (SUCCEEDED(ITypeInfo_GetTypeAttr(info, &attributes)) && attributes) {
          const BOOL dispatch = attributes->typekind == TKIND_DISPATCH;
          ITypeInfo_ReleaseTypeAttr(info, attributes);
          if (dispatch) {
            HREFTYPE reference = 0;
            ITypeInfo *table = NULL;
            if (SUCCEEDED(ITypeInfo_GetRefTypeOfImplType(info, (UINT)-1, &reference)) && SUCCEEDED(ITypeInfo_GetRefTypeInfo(info, reference, &table)) && table) {
              ITypeInfo_Release(info);
              info = table;
            } else {
              ITypeInfo_Release(info);
              info = NULL;
            }
          }
        }
        if (info && !remote_collect_methods(info, made, 0)) { mem_free(made->methods); made->methods = NULL; made->slots = 0; }
        /* (the type information stays loaded: the descriptions were copied out of it) */
        if (info) ITypeInfo_Release(info);
      }
      ITypeLib_Release(library);
    }
  }
  lock();
  for (found = g_remote_interfaces; found; found = found->next) if (IsEqualGUID(&found->iid, iid)) break;
  if (!found) { made->next = g_remote_interfaces; g_remote_interfaces = made; found = made; made = NULL; }
  unlock();
  mem_free(made);      /* (another thread was first: its copy is the one) */
  return found->methods ? found : NULL;
}

/* ---- values on the wire ---------------------------------------------------------------------------------------------------------------------------------------- */

static void rbuf_put_string(RBuf *b, BSTR s) {
  const Automation *oa = automation();
  if (!s || !oa) { rbuf_put32(b, 0xFFFFFFFFu); return; }
  const UINT chars = oa->string_len(s);
  rbuf_put32(b, chars);
  rbuf_put(b, s, chars * sizeof(WCHAR));
}
static BSTR rbuf_get_string(RBuf *b) {
  const Automation *oa = automation();
  const DWORD chars = rbuf_get32(b);
  if (chars == 0xFFFFFFFFu || b->bad || !oa || chars > REMOTE_MAX_MESSAGE / sizeof(WCHAR)) return NULL;
  const BYTE *text = rbuf_take(b, chars * sizeof(WCHAR));
  return text || chars == 0 ? oa->alloc_len((const OLECHAR *)text, chars) : NULL;
}
/* A VARIANT: its type, then its value if it is of a kind that is sent (others arrive empty). */
static void rbuf_put_variant(RBuf *b, const VARIANT *v) {
  const VARTYPE vt = v ? V_VT(v) : VT_EMPTY;
  switch (vt) {
    case VT_EMPTY: case VT_NULL: rbuf_put32(b, vt); break;
    case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_BOOL: case VT_ERROR: case VT_R4:
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
      rbuf_put32(b, vt);
      rbuf_put64(b, V_UI8(v));
      break;
    case VT_BSTR: rbuf_put32(b, vt); rbuf_put_string(b, V_BSTR(v)); break;
    case VT_UNKNOWN: rbuf_put32(b, vt); rbuf_put_interface(b, &IID_IUnknown, V_UNKNOWN(v)); break;
    case VT_DISPATCH: rbuf_put32(b, vt); rbuf_put_interface(b, &IID_IDispatch, (IUnknown *)V_DISPATCH(v)); break;
    default: rbuf_put32(b, VT_EMPTY); break;
  }
}
static void rbuf_get_variant(RBuf *b, VARIANT *v) {
  mem_zero(v, sizeof(*v));
  const VARTYPE vt = (VARTYPE)rbuf_get32(b);
  switch (vt) {
    case VT_EMPTY: case VT_NULL: V_VT(v) = vt; break;
    case VT_BSTR: V_VT(v) = vt; V_BSTR(v) = rbuf_get_string(b); break;
    case VT_UNKNOWN: if (SUCCEEDED(rbuf_get_interface(b, &IID_IUnknown, (void **)&V_UNKNOWN(v)))) V_VT(v) = vt; break;
    case VT_DISPATCH: if (SUCCEEDED(rbuf_get_interface(b, &IID_IDispatch, (void **)&V_DISPATCH(v)))) V_VT(v) = vt; break;
    default: V_VT(v) = vt; V_UI8(v) = rbuf_get64(b); break;
  }
}

/* Text that ends in a zero (0xFFFFFFFF: a null pointer; else its length in bytes without the zero), and back as memory of the task allocator. */
static void rbuf_put_text(RBuf *b, const void *text, BOOL wide) {
  DWORD bytes = 0;
  if (!text) { rbuf_put32(b, 0xFFFFFFFFu); return; }
  if (wide) bytes = (DWORD)(wlen(text) * sizeof(WCHAR));
  else while (((const char *)text)[bytes]) bytes++;
  rbuf_put32(b, bytes);
  rbuf_put(b, text, bytes);
}
static void *rbuf_get_text(RBuf *b) {
  const DWORD bytes = rbuf_get32(b);
  if (bytes == 0xFFFFFFFFu || b->bad) return NULL;
  const BYTE *text = rbuf_take(b, bytes);
  BYTE *copy = text || bytes == 0 ? mem_alloc(bytes + sizeof(WCHAR)) : NULL;      /* (zeroed: the end is there) */
  if (copy && bytes) mem_copy(copy, text, bytes);
  return copy;
}

/* A value of `kind` at `storage` (as it lies in memory: 4 or 8 bytes, a BSTR, an interface pointer, a VARIANT), written and read. */
static void remote_put_value(RBuf *b, BYTE kind, REFIID iid, const void *storage) {
  switch (kind) {
    case kKindInt: rbuf_put32(b, *(const DWORD *)storage); break;
    case kKindLong: rbuf_put64(b, *(const ULONGLONG *)storage); break;
    case kKindString: rbuf_put_string(b, *(BSTR const *)storage); break;
    case kKindWide: rbuf_put_text(b, *(void *const *)storage, TRUE); break;
    case kKindAnsi: rbuf_put_text(b, *(void *const *)storage, FALSE); break;
    case kKindInterface: rbuf_put_interface(b, iid, *(IUnknown *const *)storage); break;
    case kKindVariant: rbuf_put_variant(b, storage); break;
    default: b->bad = TRUE; break;
  }
}
static void remote_get_value(RBuf *b, BYTE kind, REFIID iid, void *storage) {
  switch (kind) {
    case kKindInt: *(DWORD *)storage = rbuf_get32(b); break;
    case kKindLong: *(ULONGLONG *)storage = rbuf_get64(b); break;
    case kKindString: *(BSTR *)storage = rbuf_get_string(b); break;
    case kKindWide: case kKindAnsi: *(void **)storage = rbuf_get_text(b); break;
    case kKindInterface: if (FAILED(rbuf_get_interface(b, iid, storage))) *(void **)storage = NULL; break;
    case kKindVariant: rbuf_get_variant(b, storage); break;
    default: b->bad = TRUE; break;
  }
}
/* Lets go of what a value read by remote_get_value holds. */
static void remote_clear_value(BYTE kind, void *storage) {
  const Automation *oa = automation();
  switch (kind) {
    case kKindString: if (oa && *(BSTR *)storage) oa->free_string(*(BSTR *)storage); *(BSTR *)storage = NULL; break;
    case kKindWide: case kKindAnsi: mem_free(*(void **)storage); *(void **)storage = NULL; break;
    case kKindInterface: if (*(IUnknown **)storage) IUnknown_Release(*(IUnknown **)storage); *(IUnknown **)storage = NULL; break;
    case kKindVariant: if (oa) oa->variant_clear(storage); break;
    default: break;
  }
}
static UINT remote_value_bytes(BYTE kind) { return kind == kKindLong ? 8 : kind == kKindVariant ? sizeof(VARIANT) : kind == kKindInt ? 4 : sizeof(void *); }
/* How many words of the stack an argument takes. */
static UINT remote_argument_words(const RemoteParameter *parameter) {
#ifdef _WIN64
  (void)parameter;
  return 1;      /* (a VARIANT is passed as a pointer to a copy) */
#else
  return parameter->kind == kKindLong ? 2 : parameter->kind == kKindVariant ? sizeof(VARIANT) / 4 : 1;
#endif
}

/* ---- the calling side: proxies ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct RemoteProxy RemoteProxy;
typedef struct RemoteFace {      /* one interface of a proxy: what a caller holds */
  const void *const *vtable;     /* g_remote_vtable */
  struct RemoteFace *next;
  RemoteProxy *owner;
  const RemoteInterface *type;
  GUID iid;
} RemoteFace;
struct RemoteProxy {
  IUnknown identity;
  IClassFactory factory;
  RemoteProxy *next;
  LONG refs;
  DWORD pid;                     /* the owner of the object */
  ULONGLONG oid;
  LONG held;                     /* references this process has with the owner */
  RemoteFace *faces;
};
static RemoteProxy *g_remote_proxies;
static const void *g_remote_vtable[REMOTE_SLOTS];

static HRESULT remote_proxy_query(RemoteProxy *proxy, REFIID iid, void **out) {
  RemoteFace *face;
  *out = NULL;
  if (IsEqualGUID(iid, &IID_IUnknown)) { *out = &proxy->identity; InterlockedIncrement(&proxy->refs); return S_OK; }
  if (IsEqualGUID(iid, &IID_IMarshal)) return E_NOINTERFACE;      /* (marshaled again, a proxy is an object of this process that passes its calls on) */
  lock();
  for (face = proxy->faces; face; face = face->next) if (IsEqualGUID(&face->iid, iid)) break;
  unlock();
  if (face) { *out = face; InterlockedIncrement(&proxy->refs); return S_OK; }
  HRESULT hr = remote_transact(proxy->pid, kRemoteQuery, proxy->oid, iid, 0, NULL, NULL);
  if (FAILED(hr)) return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ? hr : E_NOINTERFACE;
  if (IsEqualGUID(iid, &IID_IClassFactory)) { *out = &proxy->factory; InterlockedIncrement(&proxy->refs); return S_OK; }
  const RemoteInterface *type = remote_interface(iid);
  if (!type) return E_NOINTERFACE;      /* (the object has it, but nothing says how its calls are sent) */
  RemoteFace *made = mem_alloc(sizeof(*made));
  if (!made) return E_OUTOFMEMORY;
  made->vtable = g_remote_vtable;
  made->owner = proxy;
  made->type = type;
  made->iid = *iid;
  lock();
  for (face = proxy->faces; face; face = face->next) if (IsEqualGUID(&face->iid, iid)) break;
  if (!face) { made->next = proxy->faces; proxy->faces = made; face = made; made = NULL; }
  unlock();
  mem_free(made);
  *out = face;
  InterlockedIncrement(&proxy->refs);
  return S_OK;
}
static ULONG remote_proxy_release(RemoteProxy *proxy) {
  lock();
  const LONG refs = InterlockedDecrement(&proxy->refs);
  if (refs == 0) {
    for (RemoteProxy **link = &g_remote_proxies, *x; (x = *link) != NULL; link = &x->next) if (x == proxy) { *link = x->next; break; }
  }
  unlock();
  if (refs != 0) return (ULONG)refs;
  (void)remote_transact(proxy->pid, kRemoteRelease, proxy->oid, NULL, (DWORD)proxy->held, NULL, NULL);
  for (RemoteFace *face = proxy->faces, *following; face; face = following) { following = face->next; mem_free(face); }
  mem_free(proxy);
  return 0;
}

static HRESULT WINAPI remote_identity_qi(IUnknown *me, REFIID iid, void **out) { return out ? remote_proxy_query(CONTAINING_RECORD(me, RemoteProxy, identity), iid, out) : E_POINTER; }
static ULONG WINAPI remote_identity_addref(IUnknown *me) { return (ULONG)InterlockedIncrement(&CONTAINING_RECORD(me, RemoteProxy, identity)->refs); }
static ULONG WINAPI remote_identity_release(IUnknown *me) { return remote_proxy_release(CONTAINING_RECORD(me, RemoteProxy, identity)); }
static const IUnknownVtbl g_remote_identity_vtbl = {remote_identity_qi, remote_identity_addref, remote_identity_release};

/* IClassFactory of an object in another process. */
static HRESULT WINAPI remote_factory_qi(IClassFactory *me, REFIID iid, void **out) { return out ? remote_proxy_query(CONTAINING_RECORD(me, RemoteProxy, factory), iid, out) : E_POINTER; }
static ULONG WINAPI remote_factory_addref(IClassFactory *me) { return (ULONG)InterlockedIncrement(&CONTAINING_RECORD(me, RemoteProxy, factory)->refs); }
static ULONG WINAPI remote_factory_release(IClassFactory *me) { return remote_proxy_release(CONTAINING_RECORD(me, RemoteProxy, factory)); }
static HRESULT WINAPI remote_factory_create(IClassFactory *me, IUnknown *outer, REFIID iid, void **out) {
  RemoteProxy *proxy = CONTAINING_RECORD(me, RemoteProxy, factory);
  RBuf request, reply;
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  mem_zero(&request, sizeof(request));
  mem_zero(&reply, sizeof(reply));
  rbuf_put(&request, iid, sizeof(GUID));
  HRESULT hr = remote_transact(proxy->pid, kRemoteCreateInstance, proxy->oid, NULL, 0, &request, &reply);
  if (SUCCEEDED(hr)) hr = rbuf_get_interface(&reply, iid, out);
  if (SUCCEEDED(hr) && !*out) hr = E_NOINTERFACE;
  rbuf_free(&request);
  rbuf_free(&reply);
  return hr;
}
static HRESULT WINAPI remote_factory_lock(IClassFactory *me, BOOL lock_it) { (void)me; (void)lock_it; return S_OK; }
static const IClassFactoryVtbl g_remote_factory_vtbl = {remote_factory_qi, remote_factory_addref, remote_factory_release, remote_factory_create, remote_factory_lock};

/* An OBJREF of another process, unmarshaled (called by the standard marshaler): the reference becomes this process's, and a proxy for the object answers `iid`.
 * Without `out` the reference is only given back (CoReleaseMarshalData). */
static HRESULT remote_unmarshal(DWORD pid, REFGUID ipid, REFIID iid, void **out) {
  RBuf reply;
  RemoteProxy *proxy, *made;
  mem_zero(&reply, sizeof(reply));
  HRESULT hr = remote_transact(pid, kRemoteUnmarshal, 0, ipid, 0, NULL, &reply);
  const ULONGLONG oid = rbuf_get64(&reply);
  if (SUCCEEDED(hr) && reply.bad) hr = RPC_E_INVALID_DATAPACKET;
  rbuf_free(&reply);
  if (FAILED(hr)) return hr;
  if (!out) { (void)remote_transact(pid, kRemoteRelease, oid, NULL, 1, NULL, NULL); return S_OK; }
  made = mem_alloc(sizeof(*made));
  if (!made) { (void)remote_transact(pid, kRemoteRelease, oid, NULL, 1, NULL, NULL); return E_OUTOFMEMORY; }
  made->identity.lpVtbl = (IUnknownVtbl *)&g_remote_identity_vtbl;
  made->factory.lpVtbl = (IClassFactoryVtbl *)&g_remote_factory_vtbl;
  made->refs = 1;
  made->pid = pid;
  made->oid = oid;
  made->held = 1;
  lock();
  for (proxy = g_remote_proxies; proxy; proxy = proxy->next) if (proxy->pid == pid && proxy->oid == oid) break;
  if (proxy) { InterlockedIncrement(&proxy->refs); proxy->held++; }      /* (one proxy per object: the new reference with the owner is kept by it too) */
  else { made->next = g_remote_proxies; g_remote_proxies = made; proxy = made; made = NULL; }
  unlock();
  mem_free(made);
  hr = remote_proxy_query(proxy, iid, out);
  remote_proxy_release(proxy);
  return hr;
}

/* A call on a proxy: `slot` of the virtual table, `arguments` the caller's (the first is the object). Returns the method's result; *pop gets the bytes of the
 * arguments (which a 32-bit method takes off the stack itself). */
static HRESULT remote_proxy_invoke(DWORD slot, ULONG_PTR *arguments, DWORD *pop) {
  RemoteFace *face = (RemoteFace *)arguments[0];
  RemoteProxy *proxy = face->owner;
  RBuf request, reply;
  *pop = REMOTE_WORD;
  if (slot == 0) { *pop = 3 * REMOTE_WORD; return arguments[2] ? remote_proxy_query(proxy, (REFIID)arguments[1], (void **)arguments[2]) : E_POINTER; }
  if (slot == 1) return (HRESULT)InterlockedIncrement(&proxy->refs);
  if (slot == 2) return (HRESULT)remote_proxy_release(proxy);
  const RemoteMethod *method = slot < face->type->slots ? &face->type->methods[slot] : NULL;
  if (!method || !method->parameters) {
    /* IDispatch's own methods of a dual interface (their sizes are known), or a slot the type library does not have. */
    static const BYTE kDispatchWords[4] = {2, 4, 6, 9};
    if (slot >= 3 && slot <= 6) *pop = kDispatchWords[slot - 3] * REMOTE_WORD;
    return E_NOTIMPL;
  }
  UINT words = 1;
  for (UINT i = 0; i < method->count; i++) words += remote_argument_words(&method->parameters[i]);
  *pop = words * REMOTE_WORD;
  if (!method->known) return E_NOTIMPL;      /* (an argument of a kind that is not sent) */
  mem_zero(&request, sizeof(request));
  mem_zero(&reply, sizeof(reply));
  (void)remote_endpoint_start();      /* (an interface among the arguments is called back here) */
  UINT at = 1;
  for (UINT i = 0; i < method->count; i++) {
    const RemoteParameter *parameter = &method->parameters[i];
    void *where = &arguments[at];
#ifdef _WIN64
    if (parameter->kind == kKindVariant) where = (void *)arguments[at];
#endif
    at += remote_argument_words(parameter);
    if (parameter->kind == kKindPointer) {
      void *target = *(void **)where;
      rbuf_put32(&request, target ? 1 : 0);      /* (the caller may pass no place for a result) */
      if (target && parameter->in) remote_put_value(&request, parameter->pointee, &parameter->iid, target);
    } else {
      remote_put_value(&request, parameter->kind, &parameter->iid, where);
    }
  }
  HRESULT hr = remote_transact(proxy->pid, kRemoteCall, proxy->oid, &face->iid, slot, &request, &reply);
  HRESULT result = hr;
  BOOL results = FALSE;
  if (SUCCEEDED(hr)) {
    result = (HRESULT)rbuf_get32(&reply);
    results = rbuf_get32(&reply) != 0 && !reply.bad;
    if (reply.bad) result = RPC_E_INVALID_DATAPACKET;
  }
  at = 1;
  for (UINT i = 0; i < method->count; i++) {
    const RemoteParameter *parameter = &method->parameters[i];
    void *target = parameter->kind == kKindPointer ? (void *)arguments[at] : NULL;
    at += remote_argument_words(parameter);
    if (!target || !parameter->out) continue;
    if (parameter->in && results) remote_clear_value(parameter->pointee, target);      /* (what the caller passed in is replaced) */
    if (results) remote_get_value(&reply, parameter->pointee, &parameter->iid, target);
    else if (!parameter->in) mem_zero(target, remote_value_bytes(parameter->pointee));
  }
  rbuf_free(&request);
  rbuf_free(&reply);
  return result;
}

/* The pieces of code the virtual table of every proxy points at: number k passes k and the address of its caller's arguments to remote_proxy_call. They are all the
 * same size, so the table is filled by address. A 32-bit method takes its arguments off the stack; how many bytes comes back with the result. */
#ifdef _WIN64
#define REMOTE_THUNK_BYTES 128
HRESULT remote_proxy_call(DWORD slot, ULONG_PTR *arguments) __asm__("remote_proxy_call");
__attribute__((used)) HRESULT remote_proxy_call(DWORD slot, ULONG_PTR *arguments) {
  DWORD pop = 0;
  return remote_proxy_invoke(slot, arguments, &pop);
}
#define REMOTE_THUNK_BODY \
  "  movq %rcx, 8(%rsp)\n  movq %rdx, 16(%rsp)\n  movq %r8, 24(%rsp)\n  movq %r9, 32(%rsp)\n" \
  "  leaq 8(%rsp), %rdx\n" \
  "  subq $0x28, %rsp\n" \
  "  movl $\\k, %ecx\n" \
  "  call remote_proxy_call\n" \
  "  addq $0x28, %rsp\n" \
  "  ret\n"
#else
#define REMOTE_THUNK_BYTES 32
unsigned long long __cdecl remote_proxy_call(DWORD slot, ULONG_PTR *arguments) __asm__("remote_proxy_call");
__attribute__((used)) unsigned long long __cdecl remote_proxy_call(DWORD slot, ULONG_PTR *arguments) {
  DWORD pop = 0;
  const HRESULT hr = remote_proxy_invoke(slot, arguments, &pop);
  return ((unsigned long long)pop << 32) | (DWORD)hr;      /* (EAX: the result; EDX: the bytes of the arguments) */
}
#define REMOTE_THUNK_BODY \
  "  leal 4(%esp), %eax\n" \
  "  pushl %eax\n" \
  "  pushl $\\k\n" \
  "  call remote_proxy_call\n" \
  "  addl $8, %esp\n" \
  "  popl %ecx\n" \
  "  addl %edx, %esp\n" \
  "  jmp *%ecx\n"
#endif
#define REMOTE_STRINGIFY2(x) #x
#define REMOTE_STRINGIFY(x) REMOTE_STRINGIFY2(x)
__asm__(
  ".text\n"
  ".balign " REMOTE_STRINGIFY(REMOTE_THUNK_BYTES) "\n"
  ".globl remote_thunks\n"
  "remote_thunks:\n"
  ".irp k,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,"
  "32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63\n"
  ".balign " REMOTE_STRINGIFY(REMOTE_THUNK_BYTES) "\n"
  REMOTE_THUNK_BODY
  ".endr\n"
  ".balign " REMOTE_STRINGIFY(REMOTE_THUNK_BYTES) "\n"
);
extern const BYTE remote_thunks[] __asm__("remote_thunks");

static void remote_init(void) {
  for (UINT k = 0; k < REMOTE_SLOTS; k++) g_remote_vtable[k] = remote_thunks + k * REMOTE_THUNK_BYTES;
}

/* ---- the owning side: making the call --------------------------------------------------------------------------------------------------------------------------- */

/* remote_invoke(function, words, count): calls `function` with `count` words as its arguments (the first is the object), the way a method is called. */
ULONG_PTR remote_invoke(const void *function, const ULONG_PTR *words, ULONG_PTR count) __asm__("remote_invoke");
#ifdef _WIN64
__asm__(
  ".text\n"
  ".globl remote_invoke\n"
  ".def remote_invoke; .scl 2; .type 32; .endef\n"
  ".seh_proc remote_invoke\n"
  "remote_invoke:\n"
  "  pushq %rbp\n  .seh_pushreg %rbp\n"
  "  pushq %rsi\n  .seh_pushreg %rsi\n"
  "  pushq %rdi\n  .seh_pushreg %rdi\n"
  "  movq %rsp, %rbp\n  .seh_setframe %rbp, 0\n"
  "  .seh_endprologue\n"
  "  movq %rcx, %r10\n"
  "  movq %rdx, %rsi\n"
  "  movq %r8, %r11\n"
  "  movq %r11, %rax\n"
  "  cmpq $4, %rax\n"
  "  jae 1f\n"
  "  movl $4, %eax\n"
  "1:\n"
  "  leaq 15(,%rax,8), %rax\n"
  "  andq $-16, %rax\n"
  "  subq %rax, %rsp\n"
  "  andq $-16, %rsp\n"
  "  xorl %eax, %eax\n"
  "  movq %rax, (%rsp)\n  movq %rax, 8(%rsp)\n  movq %rax, 16(%rsp)\n  movq %rax, 24(%rsp)\n"
  "  movq %rsp, %rdi\n"
  "  movq %r11, %rcx\n"
  "  cld\n"
  "  rep movsq\n"
  "  movq (%rsp), %rcx\n  movq 8(%rsp), %rdx\n  movq 16(%rsp), %r8\n  movq 24(%rsp), %r9\n"
  "  call *%r10\n"
  "  movq %rbp, %rsp\n"
  "  popq %rdi\n  popq %rsi\n  popq %rbp\n"
  "  ret\n"
  ".seh_endproc\n"
);
#else
__asm__(
  ".text\n"
  ".globl remote_invoke\n"
  "remote_invoke:\n"
  "  pushl %ebp\n"
  "  movl %esp, %ebp\n"
  "  pushl %esi\n"
  "  pushl %edi\n"
  "  movl 16(%ebp), %ecx\n"
  "  movl 12(%ebp), %esi\n"
  "  leal (,%ecx,4), %eax\n"
  "  subl %eax, %esp\n"
  "  andl $-16, %esp\n"
  "  movl %esp, %edi\n"
  "  cld\n"
  "  rep movsl\n"
  "  call *8(%ebp)\n"                  /* (the method takes its arguments off the stack; the frame pointer puts it right whatever it did) */
  "  leal -8(%ebp), %esp\n"
  "  popl %edi\n"
  "  popl %esi\n"
  "  popl %ebp\n"
  "  ret\n"
);
#endif

/* A call that arrived for `identity`: the method at `slot` of its interface `iid`, with the arguments of `in`. Into `out`: the method's result, whether what it gave
 * back follows (it does if the method succeeded), and that. */
static HRESULT remote_stub_call(IUnknown *identity, REFIID iid, DWORD slot, RBuf *in, RBuf *out) {
  enum { kMostWords = 1 + 32 * 4 };
  typedef union { ULONGLONG number; void *pointer; VARIANT variant; } Storage;
  ULONG_PTR words[kMostWords];
  Storage storage[32];
  BOOL passed[32];
  void *face = NULL;
  const RemoteInterface *type = remote_interface(iid);
  if (!type || slot < 3 || slot >= type->slots || !type->methods[slot].known) return RPC_E_INVALIDMETHOD;
  const RemoteMethod *method = &type->methods[slot];
  HRESULT hr = IUnknown_QueryInterface(identity, iid, &face);
  if (FAILED(hr) || !face) return FAILED(hr) ? hr : E_NOINTERFACE;
  (void)remote_endpoint_start();      /* (an interface among the results is called back here) */
  UINT used = 0;
  words[used++] = (ULONG_PTR)face;
  mem_zero(storage, sizeof(storage));
  for (UINT i = 0; i < method->count; i++) {
    const RemoteParameter *parameter = &method->parameters[i];
    passed[i] = TRUE;
    if (parameter->kind == kKindPointer) {
      passed[i] = rbuf_get32(in) != 0;
      if (passed[i] && parameter->in) remote_get_value(in, parameter->pointee, &parameter->iid, &storage[i]);
      words[used++] = passed[i] ? (ULONG_PTR)&storage[i] : 0;
      continue;
    }
    remote_get_value(in, parameter->kind, &parameter->iid, &storage[i]);
#ifdef _WIN64
    words[used++] = parameter->kind == kKindVariant ? (ULONG_PTR)&storage[i].variant : parameter->kind == kKindInt ? (ULONG_PTR)(DWORD)storage[i].number : (ULONG_PTR)storage[i].number;
#else
    mem_copy(&words[used], &storage[i], remote_argument_words(parameter) * 4);
    used += remote_argument_words(parameter);
#endif
  }
  HRESULT result = RPC_E_INVALID_DATAPACKET;
  if (!in->bad) {
    const void *const *vtable = *(const void *const *const *)face;
    result = (HRESULT)remote_invoke(vtable[slot], words, used);
  }
  rbuf_put32(out, (DWORD)result);
  rbuf_put32(out, SUCCEEDED(result) && !in->bad ? 1 : 0);
  for (UINT i = 0; i < method->count; i++) {
    const RemoteParameter *parameter = &method->parameters[i];
    const BYTE kind = parameter->kind == kKindPointer ? parameter->pointee : parameter->kind;
    if (parameter->kind == kKindPointer && parameter->out && passed[i] && SUCCEEDED(result) && !in->bad) remote_put_value(out, kind, &parameter->iid, &storage[i]);
    if (passed[i]) remote_clear_value(kind, &storage[i]);
  }
  IUnknown_Release((IUnknown *)face);
  return in->bad ? RPC_E_INVALID_DATAPACKET : S_OK;
}

/* ---- activation ------------------------------------------------------------------------------------------------------------------------------------------------- */

#define REMOTE_RUNNING_KEY L"SOFTWARE\\TheoryCraftOS\\COM\\Running"
#define REMOTE_START_WAIT_MS 30000

/* The registry's note of who serves a class: written (pid), read (0: nobody), or taken away (pid 0). */
static void remote_announce(REFCLSID clsid, DWORD pid) {
  WCHAR name[40];
  HKEY key;
  guid_to_text(clsid, name);
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, REMOTE_RUNNING_KEY, 0, NULL, REG_OPTION_VOLATILE, KEY_READ | KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
  if (pid) RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid));
  else RegDeleteValueW(key, name);
  RegCloseKey(key);
}
static DWORD remote_running(REFCLSID clsid) {
  WCHAR name[40];
  HKEY key;
  DWORD pid = 0, type = 0, size = sizeof(pid);
  guid_to_text(clsid, name);
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REMOTE_RUNNING_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
  if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&pid, &size) != ERROR_SUCCESS || type != REG_DWORD) pid = 0;
  RegCloseKey(key);
  return pid;
}
/* Every class this process serves to other processes (and does not hold back) is announced; `gone`: the class with that cookie is not served any more. */
static void remote_announce_all(void) {
  CLSID classes[32];
  UINT count = 0;
  lock();
  for (ClassEntry *e = g_classes; e && count < 32; e = e->next) if ((e->context & CLSCTX_LOCAL_SERVER) && !e->suspended) classes[count++] = e->clsid;
  unlock();
  if (count && remote_endpoint_start())
    for (UINT i = 0; i < count; i++) remote_announce(&classes[i], GetCurrentProcessId());
}

/* Starts what serves the class: the service its AppID names, else its LocalServer32 program. S_FALSE: the registry names neither. */
static HRESULT remote_start_server(REFCLSID clsid) {
  WCHAR path[100], appid[64], service[260], parameters[512], command[MAX_PATH * 2];
  clsid_key_path(clsid, NULL, path);
  if (reg_class_string(path, W('A', 'p', 'p', 'I', 'D'), appid, 64) == ERROR_SUCCESS && appid[0]) {
    WCHAR *p = path;
    for (const WCHAR *prefix = W('A', 'p', 'p', 'I', 'D', '\\'); *prefix;) *p++ = *prefix++;
    wcopy_n(p, appid, 64);
    if (reg_class_string(path, W('L', 'o', 'c', 'a', 'l', 'S', 'e', 'r', 'v', 'i', 'c', 'e'), service, 260) == ERROR_SUCCESS && service[0]) {
      const WCHAR *arguments[16];
      DWORD count = 0;
      parameters[0] = 0;
      (void)reg_class_string(path, W('S', 'e', 'r', 'v', 'i', 'c', 'e', 'P', 'a', 'r', 'a', 'm', 'e', 't', 'e', 'r', 's'), parameters, 512);
      for (WCHAR *word = parameters; *word && count < 16;) {      /* (the parameters are words: the service's arguments) */
        while (*word == ' ') word++;
        if (!*word) break;
        arguments[count++] = word;
        while (*word && *word != ' ') word++;
        if (*word) *word++ = 0;
      }
      HRESULT hr = CO_E_SERVER_EXEC_FAILURE;
      SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
      SC_HANDLE handle = manager ? OpenServiceW(manager, service, SERVICE_START | SERVICE_QUERY_STATUS) : NULL;
      if (handle) {
        if (StartServiceW(handle, count, count ? arguments : NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) hr = S_OK;
        else hr = HRESULT_FROM_WIN32(GetLastError());
        CloseServiceHandle(handle);
      } else {
        hr = HRESULT_FROM_WIN32(GetLastError());
      }
      if (manager) CloseServiceHandle(manager);
      return hr;
    }
  }
  clsid_key_path(clsid, W('L', 'o', 'c', 'a', 'l', 'S', 'e', 'r', 'v', 'e', 'r', '3', '2'), path);
  if (reg_class_string(path, NULL, command, MAX_PATH * 2) == ERROR_SUCCESS && command[0]) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION made;
    mem_zero(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &made)) return CO_E_SERVER_EXEC_FAILURE;
    CloseHandle(made.hThread);
    CloseHandle(made.hProcess);
    return S_OK;
  }
  return S_FALSE;
}

/* The class object of a class another process serves (CoGetClassObject with CLSCTX_LOCAL_SERVER). */
static HRESULT remote_get_class(REFCLSID clsid, REFIID iid, void **out) {
  BOOL started = FALSE;
  const DWORD begun = GetTickCount();
  for (;;) {
    const DWORD pid = remote_running(clsid);
    if (pid && pid != GetCurrentProcessId()) {
      RBuf request, reply;
      mem_zero(&request, sizeof(request));
      mem_zero(&reply, sizeof(reply));
      rbuf_put(&request, iid, sizeof(GUID));
      HRESULT hr = remote_transact(pid, kRemoteGetClass, 0, clsid, 0, &request, &reply);
      if (SUCCEEDED(hr)) hr = rbuf_get_interface(&reply, iid, out);
      if (SUCCEEDED(hr) && !*out) hr = E_NOINTERFACE;
      rbuf_free(&request);
      rbuf_free(&reply);
      if (hr != HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) && hr != RPC_E_DISCONNECTED && hr != REGDB_E_CLASSNOTREG) return hr;
      remote_announce(clsid, 0);      /* (it is gone, or serves the class no more: the note was stale) */
    }
    if (!started) {
      const HRESULT hr = remote_start_server(clsid);
      if (hr == S_FALSE) return REGDB_E_CLASSNOTREG;
      if (FAILED(hr)) return hr;
      started = TRUE;
    }
    if (GetTickCount() - begun > REMOTE_START_WAIT_MS) return CO_E_SERVER_EXEC_FAILURE;
    Sleep(50);
  }
}

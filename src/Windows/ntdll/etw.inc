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

/* ntdll.dll: event tracing (providers register and write events nobody listens to, the control functions of trace sessions answer that there are none), the software quality
 * metrics calls (all of which do nothing), the hash functions of the old crypto interface (MD4, MD5, SHA-1), CRCs and prefix tables. Included by ntdll.c after threadpool.inc. */

typedef struct ETW_GUID_ { ULONG a; USHORT b, c; BYTE d[8]; } ETW_GUID_;

/* ---- providers ---- */

typedef struct ETW_PROVIDER_ { ETW_GUID_ guid; PVOID callback, context; } ETW_PROVIDER_;
EXPORT ULONG NTAPI EtwEventRegister(const ETW_GUID_ *guid, PVOID callback, PVOID context, ULONGLONG *handle) {
  if (!guid || !handle) return 87;      /* ERROR_INVALID_PARAMETER */
  ETW_PROVIDER_ *p = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(*p));
  if (!p) return 8;      /* ERROR_NOT_ENOUGH_MEMORY */
  p->guid = *guid;
  p->callback = callback;
  p->context = context;
  *handle = (ULONGLONG)(ULONG_PTR)p;
  return 0;
}
EXPORT ULONG NTAPI EtwEventUnregister(ULONGLONG handle) {
  if (!handle) return 6;      /* ERROR_INVALID_HANDLE */
  RtlFreeHeap(process_heap(), 0, (PVOID)(ULONG_PTR)handle);
  return 0;
}
EXPORT BOOLEAN NTAPI EtwEventEnabled(ULONGLONG handle, const PVOID descriptor) { (void)handle; (void)descriptor; return FALSE; }
EXPORT BOOLEAN NTAPI EtwEventProviderEnabled(ULONGLONG handle, UCHAR level, ULONGLONG keyword) { (void)handle; (void)level; (void)keyword; return FALSE; }
EXPORT ULONG NTAPI EtwEventWrite(ULONGLONG handle, const PVOID descriptor, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteTransfer(ULONGLONG handle, const PVOID descriptor, const PVOID activity, const PVOID related, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)activity; (void)related; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteFull(ULONGLONG handle, const PVOID descriptor, USHORT tag, const PVOID activity, const PVOID related, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)tag; (void)activity; (void)related; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteEx(ULONGLONG handle, const PVOID descriptor, ULONGLONG filter, ULONG flags, const PVOID activity, const PVOID related, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)filter; (void)flags; (void)activity; (void)related; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteString(ULONGLONG handle, UCHAR level, ULONGLONG keyword, PCWSTR text) { (void)handle; (void)level; (void)keyword; (void)text; return 0; }
EXPORT ULONG NTAPI EtwEventWriteNoRegistration(const ETW_GUID_ *guid, const PVOID descriptor, ULONG count, PVOID data) { (void)guid; (void)descriptor; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteStartScenario(ULONGLONG handle, const PVOID descriptor, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventWriteEndScenario(ULONGLONG handle, const PVOID descriptor, ULONG count, PVOID data) { (void)handle; (void)descriptor; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwEventSetInformation(ULONGLONG handle, ULONG information_class, PVOID information, ULONG length) { (void)handle; (void)information_class; (void)information; (void)length; return 0; }
EXPORT ULONG NTAPI EtwNotificationRegister(const ETW_GUID_ *guid, ULONG type, PVOID callback, PVOID context, ULONGLONG *handle) { (void)type; return EtwEventRegister(guid, callback, context, handle); }
EXPORT ULONG NTAPI EtwNotificationUnregister(ULONGLONG handle, PVOID context) { (void)context; return EtwEventUnregister(handle); }
EXPORT ULONG NTAPI EtwRegisterSecurityProvider(void) { return 0; }
EXPORT ULONG NTAPI EtwWriteUMSecurityEvent(const PVOID descriptor, USHORT tag, ULONG count, PVOID data) { (void)descriptor; (void)tag; (void)count; (void)data; return 0; }
EXPORT ULONG NTAPI EtwCheckCoverage(PVOID a, PVOID b) { (void)a; (void)b; return 0; }
EXPORT ULONG NTAPI EtwEnumerateProcessRegGuids(PVOID buffer, ULONG size, PULONG needed) { (void)buffer; (void)size; if (needed) *needed = 0; return 0; }
EXPORT ULONG NTAPI EtwDeliverDataBlock(PVOID a) { (void)a; return 0; }
EXPORT ULONG NTAPI EtwSetMark(PVOID a, PVOID b, ULONG c) { (void)a; (void)b; (void)c; return 0; }
EXPORT ULONG NTAPI EtwRegisterTraceGuidsW(PVOID callback, PVOID context, const ETW_GUID_ *control, ULONG count, PVOID guids, PCWSTR image, PCWSTR resource, ULONGLONG *handle) {
  (void)count; (void)guids; (void)image; (void)resource;
  return EtwEventRegister(control, callback, context, handle);
}
EXPORT ULONG NTAPI EtwRegisterTraceGuidsA(PVOID callback, PVOID context, const ETW_GUID_ *control, ULONG count, PVOID guids, PCSTR image, PCSTR resource, ULONGLONG *handle) {
  (void)count; (void)guids; (void)image; (void)resource;
  return EtwEventRegister(control, callback, context, handle);
}
EXPORT ULONG NTAPI EtwUnregisterTraceGuids(ULONGLONG handle) { return EtwEventUnregister(handle); }
EXPORT ULONG NTAPI EtwGetTraceLoggerHandle(PVOID header) { (void)header; return 0xFFFFFFFFu; }
EXPORT UCHAR NTAPI EtwGetTraceEnableLevel(ULONGLONG handle) { (void)handle; return 0; }
EXPORT ULONG NTAPI EtwGetTraceEnableFlags(ULONGLONG handle) { (void)handle; return 0; }
EXPORT ULONG NTAPI EtwCreateTraceInstanceId(PVOID handle, PVOID instance) { (void)handle; (void)instance; return 0; }
EXPORT ULONG NTAPI EtwTraceEvent(ULONGLONG logger, PVOID header) { (void)logger; (void)header; return 0; }
EXPORT ULONG NTAPI EtwTraceEventInstance(ULONGLONG logger, PVOID header, PVOID instance, PVOID parent) { (void)logger; (void)header; (void)instance; (void)parent; return 0; }
EXPORT ULONG NTAPI EtwLogTraceEvent(ULONGLONG logger, PVOID header) { (void)logger; (void)header; return 0; }
EXPORT ULONG NTAPI EtwTraceMessage(ULONGLONG logger, ULONG flags, const ETW_GUID_ *guid, USHORT number, ...) { (void)logger; (void)flags; (void)guid; (void)number; return 0; }
EXPORT ULONG NTAPI EtwTraceMessageVa(ULONGLONG logger, ULONG flags, const ETW_GUID_ *guid, USHORT number, PVOID arguments) { (void)logger; (void)flags; (void)guid; (void)number; (void)arguments; return 0; }
/* The sessions of tracing are made and ended by the system's event service, which is not here: nothing can be started, nothing is running. */
EXPORT ULONG NTAPI EtwStartTraceA(PULONGLONG handle, PCSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 5; }
EXPORT ULONG NTAPI EtwStartTraceW(PULONGLONG handle, PCWSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 5; }
EXPORT ULONG NTAPI EtwStopTraceA(ULONGLONG handle, PCSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwStopTraceW(ULONGLONG handle, PCWSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwQueryTraceA(ULONGLONG handle, PCSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwQueryTraceW(ULONGLONG handle, PCWSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwUpdateTraceA(ULONGLONG handle, PCSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwUpdateTraceW(ULONGLONG handle, PCWSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwFlushTraceA(ULONGLONG handle, PCSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwFlushTraceW(ULONGLONG handle, PCWSTR name, PVOID properties) { (void)handle; (void)name; (void)properties; return 4201; }
EXPORT ULONG NTAPI EtwControlTraceA(ULONGLONG handle, PCSTR name, PVOID properties, ULONG control) { (void)handle; (void)name; (void)properties; (void)control; return 4201; }
EXPORT ULONG NTAPI EtwControlTraceW(ULONGLONG handle, PCWSTR name, PVOID properties, ULONG control) { (void)handle; (void)name; (void)properties; (void)control; return 4201; }
EXPORT ULONG NTAPI EtwQueryAllTracesA(PVOID properties, ULONG count, PULONG returned) { (void)properties; (void)count; if (returned) *returned = 0; return 0; }
EXPORT ULONG NTAPI EtwQueryAllTracesW(PVOID properties, ULONG count, PULONG returned) { (void)properties; (void)count; if (returned) *returned = 0; return 0; }
EXPORT ULONG NTAPI EtwEnableTrace(ULONG enable, ULONG flags, ULONG level, const ETW_GUID_ *guid, ULONGLONG session) { (void)enable; (void)flags; (void)level; (void)guid; (void)session; return 4201; }
EXPORT ULONG NTAPI EtwEnumerateTraceGuids(PVOID guids, ULONG count, PULONG returned) { (void)guids; (void)count; if (returned) *returned = 0; return 0; }

/* The activity id of the thread (EtwEventActivityIdControl: 1 get, 2 set, 3 create, 4 get and set, 5 create and set; the id is in the TEB at 0x1B0). */
EXPORT ULONG NTAPI EtwEventActivityIdControl(ULONG control, ETW_GUID_ *id) {
  ETW_GUID_ *current = (ETW_GUID_ *)((BYTE *)NtCurrentTeb() + 0x1B0);
  if (control < 1 || control > 5) return 87;
  if (!id) return 87;
  if (control == 1) { *id = *current; return 0; }
  if (control == 2) { *current = *id; return 0; }
  if (control == 4) { ETW_GUID_ old = *current; *current = *id; *id = old; return 0; }
  static volatile ULONG counter;      /* (an activity id only has to be different from the others) */
  LARGE_INTEGER time;
  NtQueryPerformanceCounter(&time, NULL);
  const ULONG n = __sync_add_and_fetch(&counter, 1);
  ULONG seed = (ULONG)time.QuadPart ^ n * 0x9E3779B1u ^ (ULONG)(ULONG_PTR)current;
  ETW_GUID_ made;
  ULONG parts[4];
  for (int i = 0; i < 4; ++i) parts[i] = RtlUniform(&seed);
  made.a = parts[0]; made.b = (USHORT)parts[1]; made.c = (USHORT)((parts[1] >> 16 & 0x0FFF) | 0x4000);
  for (int i = 0; i < 8; ++i) made.d[i] = (BYTE)(parts[2 + i / 4] >> ((i % 4) * 8));
  made.d[0] = (BYTE)((made.d[0] & 0x3F) | 0x80);
  if (control == 5) *current = made;
  *id = made;
  return 0;
}

/* ---- software quality metrics: nobody is opted in ---- */

#define SQM_NOOP(name) EXPORT ULONG NTAPI name(void) { return 0; }
SQM_NOOP(WinSqmAddToAverageDWORD) SQM_NOOP(WinSqmAddToStream) SQM_NOOP(WinSqmAddToStreamEx) SQM_NOOP(WinSqmCheckEscalationAddToStreamEx)
SQM_NOOP(WinSqmCheckEscalationSetDWORD) SQM_NOOP(WinSqmCheckEscalationSetDWORD64) SQM_NOOP(WinSqmCheckEscalationSetString) SQM_NOOP(WinSqmCommonDatapointDelete)
SQM_NOOP(WinSqmCommonDatapointSetDWORD) SQM_NOOP(WinSqmCommonDatapointSetDWORD64) SQM_NOOP(WinSqmCommonDatapointSetStreamEx) SQM_NOOP(WinSqmCommonDatapointSetString)
SQM_NOOP(WinSqmEndSession) SQM_NOOP(WinSqmEventEnabled) SQM_NOOP(WinSqmEventWrite) SQM_NOOP(WinSqmGetEscalationRuleStatus) SQM_NOOP(WinSqmGetInstrumentationProperty)
SQM_NOOP(WinSqmIncrementDWORD) SQM_NOOP(WinSqmIsOptedIn) SQM_NOOP(WinSqmIsOptedInEx) SQM_NOOP(WinSqmIsSessionDisabled) SQM_NOOP(WinSqmSetDWORD) SQM_NOOP(WinSqmSetDWORD64)
SQM_NOOP(WinSqmSetEscalationInfo) SQM_NOOP(WinSqmSetIfMaxDWORD) SQM_NOOP(WinSqmSetIfMinDWORD) SQM_NOOP(WinSqmSetString) SQM_NOOP(WinSqmStartSession)
SQM_NOOP(WinSqmStartSessionForPartner) SQM_NOOP(WinSqmStartSqmOptinListener) SQM_NOOP(RtlReportSqmEscalation) SQM_NOOP(WerReportSQMEvent)
SQM_NOOP(EvtIntReportEventAndSourceAsync) SQM_NOOP(EvtIntReportAuthzEventAndSourceAsync)

/* ---- MD4, MD5 and SHA-1 (the context layouts are the ones of the Windows headers) ---- */

typedef struct MD_CTX_ { ULONG count[2]; ULONG state[4]; BYTE buffer[64]; BYTE digest[16]; } MD_CTX_;
#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void md_store(BYTE *out, const ULONG *in, SIZE_T words) { for (SIZE_T i = 0; i < words; ++i) { out[4*i] = (BYTE)in[i]; out[4*i+1] = (BYTE)(in[i] >> 8); out[4*i+2] = (BYTE)(in[i] >> 16); out[4*i+3] = (BYTE)(in[i] >> 24); } }
static void md_load(ULONG *out, const BYTE *in, SIZE_T words) { for (SIZE_T i = 0; i < words; ++i) out[i] = (ULONG)in[4*i] | (ULONG)in[4*i+1] << 8 | (ULONG)in[4*i+2] << 16 | (ULONG)in[4*i+3] << 24; }

static void md5_block(ULONG state[4], const BYTE *block) {
  static const ULONG k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
  static const BYTE s[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20, 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  ULONG m[16], a = state[0], b = state[1], c = state[2], d = state[3];
  md_load(m, block, 16);
  for (int i = 0; i < 64; ++i) {
    ULONG f; int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    const ULONG t = d; d = c; c = b;
    b = b + ROL32(a + f + k[i] + m[g], s[i]);
    a = t;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}
static void md4_block(ULONG state[4], const BYTE *block) {
  ULONG x[16], a = state[0], b = state[1], c = state[2], d = state[3];
  md_load(x, block, 16);
#define MD4F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD4G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define MD4H(x, y, z) ((x) ^ (y) ^ (z))
  static const BYTE r1[4] = {3, 7, 11, 19}, r2[4] = {3, 5, 9, 13}, r3[4] = {3, 9, 11, 15};
  static const BYTE o3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
  ULONG *v[4] = {&a, &b, &c, &d};
  for (int i = 0; i < 16; ++i) { ULONG *p = v[(4 - i % 4) % 4], *q = v[(5 - i % 4) % 4], *r = v[(6 - i % 4) % 4], *s = v[(7 - i % 4) % 4]; *p = ROL32(*p + MD4F(*q, *r, *s) + x[i], r1[i % 4]); }
  for (int i = 0; i < 16; ++i) { ULONG *p = v[(4 - i % 4) % 4], *q = v[(5 - i % 4) % 4], *r = v[(6 - i % 4) % 4], *s = v[(7 - i % 4) % 4]; *p = ROL32(*p + MD4G(*q, *r, *s) + x[(i % 4) * 4 + i / 4] + 0x5a827999, r2[i % 4]); }
  for (int i = 0; i < 16; ++i) { ULONG *p = v[(4 - i % 4) % 4], *q = v[(5 - i % 4) % 4], *r = v[(6 - i % 4) % 4], *s = v[(7 - i % 4) % 4]; *p = ROL32(*p + MD4H(*q, *r, *s) + x[o3[i]] + 0x6ed9eba1, r3[i % 4]); }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}
static void md_update(MD_CTX_ *c, const BYTE *input, ULONG length, void (*block)(ULONG *, const BYTE *)) {
  ULONG have = (c->count[0] >> 3) & 63;
  const ULONG bits = length << 3;
  if ((c->count[0] += bits) < bits) ++c->count[1];
  c->count[1] += length >> 29;
  while (length) {
    const ULONG take = 64 - have < length ? 64 - have : length;
    memcpy(c->buffer + have, input, take);
    have += take; input += take; length -= take;
    if (have == 64) { block(c->state, c->buffer); have = 0; }
  }
}
static void md_final(MD_CTX_ *c, void (*block)(ULONG *, const BYTE *)) {
  BYTE length[8];
  ULONG words[2] = {c->count[0], c->count[1]};
  md_store(length, words, 2);
  static const BYTE padding[64] = {0x80};
  const ULONG have = (c->count[0] >> 3) & 63;
  md_update(c, padding, have < 56 ? 56 - have : 120 - have, block);
  md_update(c, length, 8, block);
  md_store(c->digest, c->state, 4);
}
EXPORT void NTAPI MD5Init(MD_CTX_ *c) { c->count[0] = c->count[1] = 0; c->state[0] = 0x67452301; c->state[1] = 0xefcdab89; c->state[2] = 0x98badcfe; c->state[3] = 0x10325476; }
EXPORT void NTAPI MD5Update(MD_CTX_ *c, const BYTE *input, ULONG length) { md_update(c, input, length, md5_block); }
EXPORT void NTAPI MD5Final(MD_CTX_ *c) { md_final(c, md5_block); }
EXPORT void NTAPI MD4Init(MD_CTX_ *c) { MD5Init(c); }
EXPORT void NTAPI MD4Update(MD_CTX_ *c, const BYTE *input, ULONG length) { md_update(c, input, length, md4_block); }
EXPORT void NTAPI MD4Final(MD_CTX_ *c) { md_final(c, md4_block); }

typedef struct SHA_CTX_ { ULONG unknown[6]; ULONG state[5]; ULONG count[2]; BYTE buffer[64]; } SHA_CTX_;
static void etw_sha1_block(ULONG state[5], const BYTE *block) {
  ULONG w[80], a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 16; ++i) w[i] = (ULONG)block[4*i] << 24 | (ULONG)block[4*i+1] << 16 | (ULONG)block[4*i+2] << 8 | block[4*i+3];
  for (int i = 16; i < 80; ++i) w[i] = ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  for (int i = 0; i < 80; ++i) {
    ULONG f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else { f = b ^ c ^ d; k = 0xca62c1d6; }
    const ULONG t = ROL32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = ROL32(b, 30); b = a; a = t;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}
EXPORT void NTAPI A_SHAInit(SHA_CTX_ *c) {
  memset(c, 0, sizeof(*c));
  c->state[0] = 0x67452301; c->state[1] = 0xefcdab89; c->state[2] = 0x98badcfe; c->state[3] = 0x10325476; c->state[4] = 0xc3d2e1f0;
}
EXPORT void NTAPI A_SHAUpdate(SHA_CTX_ *c, const BYTE *input, ULONG length) {
  ULONG have = c->count[1] & 63;
  if ((c->count[1] += length) < length) ++c->count[0];
  while (length) {
    const ULONG take = 64 - have < length ? 64 - have : length;
    memcpy(c->buffer + have, input, take);
    have += take; input += take; length -= take;
    if (have == 64) { etw_sha1_block(c->state, c->buffer); have = 0; }
  }
}
EXPORT void NTAPI A_SHAFinal(SHA_CTX_ *c, BYTE *digest) {
  const ULONG high = c->count[0], low = c->count[1];
  const ULONGLONG bits = ((ULONGLONG)high << 35) | ((ULONGLONG)low << 3);
  BYTE padding[72] = {0x80}, length[8];
  for (int i = 0; i < 8; ++i) length[i] = (BYTE)(bits >> (56 - 8 * i));
  const ULONG have = low & 63;
  A_SHAUpdate(c, padding, have < 56 ? 56 - have : 120 - have);
  A_SHAUpdate(c, length, 8);
  for (int i = 0; i < 5; ++i) { digest[4*i] = (BYTE)(c->state[i] >> 24); digest[4*i+1] = (BYTE)(c->state[i] >> 16); digest[4*i+2] = (BYTE)(c->state[i] >> 8); digest[4*i+3] = (BYTE)c->state[i]; }
}

/* ---- CRCs ---- */

EXPORT ULONG NTAPI RtlCrc32(const void *buffer, SIZE_T size, ULONG initial) {
  static ULONG table[256];
  static volatile LONG made;
  if (!made) {
    for (ULONG i = 0; i < 256; ++i) { ULONG c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
    made = 1;
  }
  ULONG crc = ~initial;
  for (const BYTE *p = buffer; size--; ++p) crc = table[(crc ^ *p) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
EXPORT ULONGLONG NTAPI RtlCrc64(const void *buffer, SIZE_T size, ULONGLONG initial) {
  static ULONGLONG table[256];
  static volatile LONG made;
  if (!made) {
    for (ULONG i = 0; i < 256; ++i) { ULONGLONG c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xC96C5795D7870F42ull ^ (c >> 1) : c >> 1; table[i] = c; }
    made = 1;
  }
  ULONGLONG crc = ~initial;
  for (const BYTE *p = buffer; size--; ++p) crc = table[(crc ^ *p) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

/* ---- prefix tables: the names that are known are a list; a lookup answers the longest of them that starts a path name ---- */

typedef struct PFX_TABLE_ { SHORT type, length; struct PFX_ENTRY_ *first; } PFX_TABLE_;
typedef struct PFX_ENTRY_ { SHORT type, length; struct PFX_ENTRY_ *next; PVOID links[3]; PANSI_STRING prefix; } PFX_ENTRY_;
EXPORT void NTAPI PfxInitialize(PFX_TABLE_ *table) { table->type = 0x0826; table->length = 0x0; table->first = NULL; }
static BYTE pfx_upcase(BYTE c) { return c >= 'a' && c <= 'z' ? (BYTE)(c - 32) : c; }
static BOOLEAN pfx_prefix_of(const ANSI_STRING *prefix, const ANSI_STRING *name, BOOLEAN ci) {
  if (prefix->Length > name->Length) return FALSE;
  for (USHORT i = 0; i < prefix->Length; ++i) {
    BYTE a = (BYTE)prefix->Buffer[i], b = (BYTE)name->Buffer[i];
    if (ci) { a = pfx_upcase(a); b = pfx_upcase(b); }
    if (a != b) return FALSE;
  }
  return name->Length == prefix->Length || name->Buffer[prefix->Length] == '\\' || (prefix->Length > 0 && prefix->Buffer[prefix->Length - 1] == '\\');
}
EXPORT BOOLEAN NTAPI PfxInsertPrefix(PFX_TABLE_ *table, PANSI_STRING prefix, PFX_ENTRY_ *entry) {
  for (PFX_ENTRY_ *e = table->first; e; e = e->next) if (e->prefix->Length == prefix->Length && pfx_prefix_of(e->prefix, prefix, TRUE)) return FALSE;
  entry->type = 0x0827;
  entry->length = (SHORT)prefix->Length;
  entry->prefix = prefix;
  entry->next = table->first;
  table->first = entry;
  return TRUE;
}
EXPORT void NTAPI PfxRemovePrefix(PFX_TABLE_ *table, PFX_ENTRY_ *entry) {
  for (PFX_ENTRY_ **link = &table->first; *link; link = &(*link)->next) if (*link == entry) { *link = entry->next; return; }
}
EXPORT PFX_ENTRY_ *NTAPI PfxFindPrefix(PFX_TABLE_ *table, PANSI_STRING name, BOOLEAN case_insensitive) {
  PFX_ENTRY_ *best = NULL;
  for (PFX_ENTRY_ *e = table->first; e; e = e->next) if (pfx_prefix_of(e->prefix, name, case_insensitive) && (!best || e->prefix->Length > best->prefix->Length)) best = e;
  return best;
}

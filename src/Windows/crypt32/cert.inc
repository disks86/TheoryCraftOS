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

/* Certificates and stores (included by crypt32.c).
 *
 * A certificate is parsed from its DER into one block: the CERT_CONTEXT, the CERT_INFO, the extension array, a copy of the encoding (which the blobs of CERT_INFO point
 * into), the serial number in little-endian order (as Windows keeps it) and the dotted object identifiers. The DER reader checks every length against what is left, so
 * a truncated or damaged certificate is refused (CRYPT_E_ASN1_*), never read past.
 *
 * Lifetimes follow Windows: a context handed out holds a reference on its store, so the store (and the other certificates in it, reached through hCertStore) lives on
 * after CertCloseStore until the last context is freed. A store's reference count is its open handles plus the contexts handed out; at 0 it is freed with its
 * certificates, and a collection then lets go of its member stores. One lock guards every store: the operations are short. */

typedef struct { const BYTE *p; SIZE_T n; } Span;

/* One TLV from the front of *s (which moves past it): the tag, its contents, and the whole encoding. Only single-byte tags and definite lengths up to 4 bytes. */
static BOOL der_next(Span *s, BYTE *tag, Span *value, Span *whole) {
  if (s->n < 2 || (s->p[0] & 0x1F) == 0x1F) return FALSE;
  SIZE_T header = 2, length = s->p[1];
  if (length & 0x80) {
    const SIZE_T k = length & 0x7F;
    if (k == 0 || k > 4 || s->n < 2 + k) return FALSE;
    length = 0;
    for (SIZE_T i = 0; i < k; ++i) length = length << 8 | s->p[2 + i];
    header += k;
  }
  if (length > s->n - header) return FALSE;
  *tag = s->p[0];
  if (value) { value->p = s->p + header; value->n = length; }
  if (whole) { whole->p = s->p; whole->n = header + length; }
  s->p += header + length;
  s->n -= header + length;
  return TRUE;
}
/* The same, but the tag must be `want`. */
static BOOL der_expect(Span *s, BYTE want, Span *value, Span *whole) {
  Span copy = *s;
  BYTE tag;
  if (!der_next(&copy, &tag, value, whole) || tag != want) return FALSE;
  *s = copy;
  return TRUE;
}
static BOOL der_peek(const Span *s, BYTE want) { return s->n >= 1 && s->p[0] == want; }

/* The dotted text of an object identifier (the contents of an OID TLV) into out (room for `capacity` chars with the NUL); the length with the NUL, 0 if it is broken.
 * With out == NULL only the length. */
static SIZE_T oid_text(Span oid, char *out, SIZE_T capacity) {
  if (!oid.n) {      /* (an empty identifier, which Windows takes as "") */
    if (out) { if (!capacity) return 0; out[0] = 0; }
    return 1;
  }
  if (oid.p[oid.n - 1] & 0x80) return 0;
  SIZE_T length = 0, i = 0;
  BOOL first = TRUE;
  while (i < oid.n) {
    ULONG64 arc = 0;
    if (oid.p[i] == 0x80) return 0;      /* (a non-minimal encoding) */
    do {
      if (arc >> 56) return 0;
      arc = arc << 7 | (oid.p[i] & 0x7F);
    } while (oid.p[i++] & 0x80);
    ULONG64 arcs[2] = {arc, 0};
    int count = 1;
    if (first) {
      arcs[0] = arc < 40 ? 0 : arc < 80 ? 1 : 2;
      arcs[1] = arc - arcs[0] * 40;
      count = 2;
      first = FALSE;
    }
    for (int a = 0; a < count; ++a) {
      char digits[24];
      int d = 0;
      ULONG64 v = arcs[a];
      do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
      if (length) { if (out && length + 1 < capacity) out[length] = '.'; ++length; }
      while (d) { --d; if (out && length + 1 < capacity) out[length] = digits[d]; ++length; }
    }
  }
  if (out) {
    if (length + 1 > capacity) return 0;
    out[length] = 0;
  }
  return length + 1;
}

/* An OID TLV's contents for a dotted text (only for comparing): the text of `oid` equals `text`. */
static BOOL oid_is(Span oid, const char *text) {
  char buffer[128];
  if (!oid_text(oid, buffer, sizeof buffer)) return FALSE;
  for (SIZE_T i = 0;; ++i) {
    if (buffer[i] != text[i]) return FALSE;
    if (!buffer[i]) return TRUE;
  }
}
static BOOL text_equal(const char *a, const char *b) {
  if (!a || !b) return FALSE;
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

/* UTCTime / GeneralizedTime (only the "Z" forms DER allows) to a FILETIME. */
static BOOL der_time(BYTE tag, Span v, FILETIME *out) {
  int digits[14];
  SIZE_T year_digits = tag == 0x17 ? 2 : tag == 0x18 ? 4 : 0;
  if (!year_digits || v.n != year_digits + 11 || v.p[v.n - 1] != 'Z') return FALSE;
  for (SIZE_T i = 0; i + 1 < v.n; ++i) {
    if (v.p[i] < '0' || v.p[i] > '9') return FALSE;
    digits[i] = v.p[i] - '0';
  }
  int year = 0, k = 0;
  for (; k < (int)year_digits; ++k) year = year * 10 + digits[k];
  if (year_digits == 2) year += year < 50 ? 2000 : 1900;
  const int month = digits[k] * 10 + digits[k + 1], day = digits[k + 2] * 10 + digits[k + 3], hour = digits[k + 4] * 10 + digits[k + 5];
  const int minute = digits[k + 6] * 10 + digits[k + 7], second = digits[k + 8] * 10 + digits[k + 9];
  if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return FALSE;
  /* days since 1601-01-01 (the civil-from-days arithmetic, shifted to a March year) */
  const int y = month <= 2 ? year - 1 : year, m = month <= 2 ? month + 9 : month - 3;
  const LONG64 era_days = (LONG64)y * 365 + y / 4 - y / 100 + y / 400 + (153 * m + 2) / 5 + day - 1;
  const LONG64 epoch = (LONG64)1600 * 365 + 1600 / 4 - 1600 / 100 + 1600 / 400 + (153 * 10 + 2) / 5;      /* 1601-01-01 = March year 1600, m = 10, day 1 */
  const ULONG64 ticks = (ULONG64)((era_days - epoch) * 86400 + hour * 3600 + minute * 60 + second) * 10000000ull;
  out->dwLowDateTime = (DWORD)ticks;
  out->dwHighDateTime = (DWORD)(ticks >> 32);
  return TRUE;
}

/* AlgorithmIdentifier ::= SEQUENCE { OID, parameters ANY OPTIONAL } */
static BOOL der_algorithm(Span *s, Span *oid, Span *parameters) {
  Span seq;
  if (!der_expect(s, 0x30, &seq, NULL) || !der_expect(&seq, 0x06, oid, NULL)) return FALSE;
  parameters->p = seq.p;
  parameters->n = 0;
  if (seq.n) {
    BYTE tag;
    if (!der_next(&seq, &tag, NULL, parameters) || seq.n) return FALSE;
  }
  return TRUE;
}

/* A BIT STRING's contents: the unused-bit count and the bytes. */
static BOOL der_bits(Span v, Span *bits, BYTE *unused) {
  if (!v.n || v.p[0] > 7 || (v.n == 1 && v.p[0])) return FALSE;
  *unused = v.p[0];
  bits->p = v.p + 1;
  bits->n = v.n - 1;
  return TRUE;
}

#define MAX_EXTENSIONS 64
typedef struct {
  DWORD version;
  Span serial, signature_oid, signature_parameters, issuer, subject, key_oid, key_parameters, key, issuer_id, subject_id;
  BYTE key_unused, issuer_id_unused, subject_id_unused;
  FILETIME not_before, not_after;
  DWORD extensions;
  Span extension_oid[MAX_EXTENSIONS], extension_value[MAX_EXTENSIONS];
  BOOL extension_critical[MAX_EXTENSIONS];
} Parsed;

/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature BIT STRING }. Like Windows this also takes a bare TBSCertificate (no signature). A failure is
 * CRYPT_E_ASN1_EOD where the data ran out, CRYPT_E_ASN1_BADTAG otherwise. */
#define BAD(span) ((span).n ? CRYPT_E_ASN1_BADTAG : CRYPT_E_ASN1_EOD)
static DWORD parse_certificate(Span encoded, Parsed *c) {
  mem_zero(c, sizeof *c);
  Span outer = encoded, cert, tbs, validity;
  BYTE tag;
  if (encoded.n < 2) return CRYPT_E_ASN1_EOD;
  if (encoded.p[0] != 0x30) return CRYPT_E_ASN1_BADTAG;
  if (!der_expect(&outer, 0x30, &cert, NULL)) return CRYPT_E_ASN1_EOD;      /* (a length past the end) */
  if (outer.n) return CRYPT_E_ASN1_CORRUPT;      /* (bytes after the certificate) */
  if (der_peek(&cert, 0x30)) {      /* signed: the TBSCertificate, then the signature's algorithm and value */
    if (!der_expect(&cert, 0x30, &tbs, NULL)) return BAD(cert);
    Span outer_oid, outer_parameters, signature;
    if (!der_algorithm(&cert, &outer_oid, &outer_parameters) || !der_expect(&cert, 0x03, &signature, NULL)) return BAD(cert);
    if (cert.n) return CRYPT_E_ASN1_CORRUPT;
  } else {
    tbs = cert;
  }
  c->version = 0;
  if (der_peek(&tbs, 0xA0)) {      /* [0] EXPLICIT Version */
    Span explicit_version, number;
    if (!der_expect(&tbs, 0xA0, &explicit_version, NULL) || !der_expect(&explicit_version, 0x02, &number, NULL) || explicit_version.n || number.n != 1 || number.p[0] > 2)
      return CRYPT_E_ASN1_BADTAG;
    c->version = number.p[0];
  }
  if (!der_expect(&tbs, 0x02, &c->serial, NULL) || !c->serial.n) return BAD(tbs);
  if (!der_algorithm(&tbs, &c->signature_oid, &c->signature_parameters)) return BAD(tbs);
  if (!der_expect(&tbs, 0x30, NULL, &c->issuer)) return BAD(tbs);
  if (!der_expect(&tbs, 0x30, &validity, NULL)) return BAD(tbs);
  Span time;
  if (!der_next(&validity, &tag, &time, NULL) || !der_time(tag, time, &c->not_before)) return CRYPT_E_ASN1_BADTAG;
  if (!der_next(&validity, &tag, &time, NULL) || !der_time(tag, time, &c->not_after) || validity.n) return CRYPT_E_ASN1_BADTAG;
  if (!der_expect(&tbs, 0x30, NULL, &c->subject)) return BAD(tbs);
  Span key_info, key_bits;
  if (!der_expect(&tbs, 0x30, &key_info, NULL) || !der_algorithm(&key_info, &c->key_oid, &c->key_parameters) || !der_expect(&key_info, 0x03, &key_bits, NULL) || key_info.n ||
      !der_bits(key_bits, &c->key, &c->key_unused))
    return CRYPT_E_ASN1_BADTAG;
  Span v;
  if (der_peek(&tbs, 0x81)) { if (!der_expect(&tbs, 0x81, &v, NULL) || !der_bits(v, &c->issuer_id, &c->issuer_id_unused)) return CRYPT_E_ASN1_BADTAG; }
  if (der_peek(&tbs, 0x82)) { if (!der_expect(&tbs, 0x82, &v, NULL) || !der_bits(v, &c->subject_id, &c->subject_id_unused)) return CRYPT_E_ASN1_BADTAG; }
  if (der_peek(&tbs, 0xA3)) {
    Span explicit_extensions, list;
    if (!der_expect(&tbs, 0xA3, &explicit_extensions, NULL) || !der_expect(&explicit_extensions, 0x30, &list, NULL) || explicit_extensions.n) return CRYPT_E_ASN1_BADTAG;
    while (list.n) {
      Span extension;
      if (c->extensions == MAX_EXTENSIONS) return CRYPT_E_ASN1_LARGE;
      if (!der_expect(&list, 0x30, &extension, NULL)) return CRYPT_E_ASN1_BADTAG;
      const DWORD e = c->extensions;
      if (!der_expect(&extension, 0x06, &c->extension_oid[e], NULL) || !oid_text(c->extension_oid[e], NULL, 0)) return CRYPT_E_ASN1_BADTAG;
      if (der_peek(&extension, 0x01)) {
        Span flag;
        if (!der_expect(&extension, 0x01, &flag, NULL) || flag.n != 1) return CRYPT_E_ASN1_BADTAG;
        c->extension_critical[e] = flag.p[0] != 0;
      }
      if (!der_expect(&extension, 0x04, &c->extension_value[e], NULL) || extension.n) return CRYPT_E_ASN1_BADTAG;
      ++c->extensions;
    }
  }
  if (tbs.n) return CRYPT_E_ASN1_BADTAG;
  if (!oid_text(c->signature_oid, NULL, 0) || !oid_text(c->key_oid, NULL, 0)) return CRYPT_E_ASN1_BADTAG;
  return ERROR_SUCCESS;
}

/* ----------------------------------------------------------------------------------------------------------------------------- the objects */

#define STORE_MAGIC 0x53544F52u
#define CERT_MAGIC 0x43455254u
#define MAX_SIBLINGS 32

typedef struct Store Store;
typedef struct Cert {
  DWORD magic;
  struct Cert *next;
  Store *store;
  BOOL deleted;
  CERT_CONTEXT context;
  CERT_INFO info;
} Cert;      /* followed by the extensions, the encoding, the serial and the OID texts */

struct Store {
  DWORD magic;
  LONG refs;      /* open handles + contexts handed out */
  BOOL collection, read_only;
  Cert *first, *last;
  Cert *deleted;      /* taken out by CertDeleteCertificateFromStore: their memory stays until the store goes (a caller may still hold one) */
  DWORD siblings;
  Store *sibling[MAX_SIBLINGS];
  DWORD sibling_flags[MAX_SIBLINGS];
};

static SRWLOCK g_store_lock = SRWLOCK_INIT;

static Store *store_of(HCERTSTORE handle) {
  Store *s = (Store *)handle;
  return s && s->magic == STORE_MAGIC ? s : NULL;
}
static Cert *cert_of(PCCERT_CONTEXT context) {
  if (!context) return NULL;
  Cert *c = (Cert *)((BYTE *)context - __builtin_offsetof(Cert, context));
  return c->magic == CERT_MAGIC ? c : NULL;
}

static void store_release_locked(Store *s);
static void store_free_locked(Store *s) {
  for (int list = 0; list < 2; ++list) {
    for (Cert *c = list ? s->deleted : s->first, *next; c; c = next) {
      next = c->next;
      c->magic = 0;
      heap_free(c);
    }
  }
  s->magic = 0;
  for (DWORD i = 0; i < s->siblings; ++i) store_release_locked(s->sibling[i]);
  heap_free(s);
}
static void store_release_locked(Store *s) {
  if (--s->refs == 0) store_free_locked(s);
}

/* A context reference for the caller: the store it lives in gains one. */
static PCCERT_CONTEXT hand_out_locked(Cert *c) {
  ++c->store->refs;
  return &c->context;
}

static Store *store_new(BOOL collection, BOOL read_only) {
  Store *s = heap_zalloc(sizeof *s);
  if (!s) return NULL;
  s->magic = STORE_MAGIC;
  s->refs = 1;
  s->collection = collection;
  s->read_only = read_only;
  return s;
}

/* A new certificate block from DER (not yet in a store). */
static DWORD cert_build(const BYTE *encoded, DWORD size, Cert **out) {
  *out = NULL;
  if (!encoded || !size) return CRYPT_E_ASN1_EOD;
  Parsed *p = heap_alloc(sizeof *p);      /* (large: the extension spans) */
  if (!p) return ERROR_NOT_ENOUGH_MEMORY;
  DWORD error = parse_certificate((Span){encoded, size}, p);
  if (error != ERROR_SUCCESS) { heap_free(p); return error; }
  SIZE_T text = oid_text(p->signature_oid, NULL, 0) + oid_text(p->key_oid, NULL, 0);
  for (DWORD e = 0; e < p->extensions; ++e) text += oid_text(p->extension_oid[e], NULL, 0);
  const SIZE_T total = sizeof(Cert) + sizeof(CERT_EXTENSION) * p->extensions + size + p->serial.n + text;
  Cert *c = heap_zalloc(total);
  if (!c) { heap_free(p); return ERROR_NOT_ENOUGH_MEMORY; }
  CERT_EXTENSION *extensions = (CERT_EXTENSION *)(c + 1);
  BYTE *copy = (BYTE *)(extensions + p->extensions), *serial = copy + size;
  char *texts = (char *)(serial + p->serial.n), *end = (char *)c + total;
  mem_copy(copy, encoded, size);
#define REBASE(span) ((BYTE *)copy + ((span).p - encoded))
  CERT_INFO *info = &c->info;
  info->dwVersion = p->version;
  for (SIZE_T i = 0; i < p->serial.n; ++i) serial[i] = p->serial.p[p->serial.n - 1 - i];
  info->SerialNumber.cbData = (DWORD)p->serial.n;
  info->SerialNumber.pbData = serial;
  SIZE_T n = oid_text(p->signature_oid, texts, (SIZE_T)(end - texts));
  info->SignatureAlgorithm.pszObjId = texts;
  texts += n;
  info->SignatureAlgorithm.Parameters.cbData = (DWORD)p->signature_parameters.n;
  info->SignatureAlgorithm.Parameters.pbData = p->signature_parameters.n ? REBASE(p->signature_parameters) : NULL;
  info->Issuer.cbData = (DWORD)p->issuer.n;
  info->Issuer.pbData = REBASE(p->issuer);
  info->NotBefore = p->not_before;
  info->NotAfter = p->not_after;
  info->Subject.cbData = (DWORD)p->subject.n;
  info->Subject.pbData = REBASE(p->subject);
  n = oid_text(p->key_oid, texts, (SIZE_T)(end - texts));
  info->SubjectPublicKeyInfo.Algorithm.pszObjId = texts;
  texts += n;
  info->SubjectPublicKeyInfo.Algorithm.Parameters.cbData = (DWORD)p->key_parameters.n;
  info->SubjectPublicKeyInfo.Algorithm.Parameters.pbData = p->key_parameters.n ? REBASE(p->key_parameters) : NULL;
  info->SubjectPublicKeyInfo.PublicKey.cbData = (DWORD)p->key.n;
  info->SubjectPublicKeyInfo.PublicKey.pbData = REBASE(p->key);
  info->SubjectPublicKeyInfo.PublicKey.cUnusedBits = p->key_unused;
  if (p->issuer_id.p) { info->IssuerUniqueId.cbData = (DWORD)p->issuer_id.n; info->IssuerUniqueId.pbData = REBASE(p->issuer_id); info->IssuerUniqueId.cUnusedBits = p->issuer_id_unused; }
  if (p->subject_id.p) { info->SubjectUniqueId.cbData = (DWORD)p->subject_id.n; info->SubjectUniqueId.pbData = REBASE(p->subject_id); info->SubjectUniqueId.cUnusedBits = p->subject_id_unused; }
  info->cExtension = p->extensions;
  info->rgExtension = p->extensions ? extensions : NULL;
  for (DWORD e = 0; e < p->extensions; ++e) {
    n = oid_text(p->extension_oid[e], texts, (SIZE_T)(end - texts));
    extensions[e].pszObjId = texts;
    texts += n;
    extensions[e].fCritical = p->extension_critical[e];
    extensions[e].Value.cbData = (DWORD)p->extension_value[e].n;
    extensions[e].Value.pbData = REBASE(p->extension_value[e]);
  }
#undef REBASE
  heap_free(p);
  c->magic = CERT_MAGIC;
  c->context.dwCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
  c->context.pbCertEncoded = copy;
  c->context.cbCertEncoded = size;
  c->context.pCertInfo = info;
  *out = c;
  return ERROR_SUCCESS;
}

static Cert *find_same_locked(Store *s, const BYTE *encoded, DWORD size) {
  for (Cert *c = s->first; c; c = c->next)
    if (c->context.cbCertEncoded == size && mem_equal(c->context.pbCertEncoded, encoded, size)) return c;
  return NULL;
}

/* Adds DER to a store with a disposition (CERT_STORE_ADD_*). */
static BOOL add_encoded(HCERTSTORE handle, const BYTE *encoded, DWORD size, DWORD disposition, PCCERT_CONTEXT *out) {
  if (out) *out = NULL;
  if (disposition < CERT_STORE_ADD_NEW || disposition > CERT_STORE_ADD_NEWER_INHERIT_PROPERTIES) { SetLastError(E_INVALIDARG); return FALSE; }
  /* The store first (a collection adds to its first member that takes additions), held by a reference while the certificate is parsed outside the lock. */
  AcquireSRWLockExclusive(&g_store_lock);
  Store *s = store_of(handle);
  DWORD error = s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
  for (int depth = 0; s && s->collection && error == ERROR_SUCCESS; ++depth) {
    Store *target = NULL;
    for (DWORD i = 0; i < s->siblings && !target; ++i) if (s->sibling_flags[i] & CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG) target = s->sibling[i];
    if (!target || depth > 16) error = E_ACCESSDENIED;
    else s = target;
  }
  if (error == ERROR_SUCCESS && s->read_only) error = E_ACCESSDENIED;
  if (error != ERROR_SUCCESS) { ReleaseSRWLockExclusive(&g_store_lock); SetLastError(error); return FALSE; }
  ++s->refs;
  ReleaseSRWLockExclusive(&g_store_lock);
  Cert *fresh;
  error = cert_build(encoded, size, &fresh);
  AcquireSRWLockExclusive(&g_store_lock);
  BOOL ok = FALSE;
  if (error == ERROR_SUCCESS) {
    Cert *existing = disposition == CERT_STORE_ADD_ALWAYS ? NULL : find_same_locked(s, encoded, size);
    if (existing) {
      heap_free(fresh);      /* the same bytes: replacing it would change nothing */
      if (disposition == CERT_STORE_ADD_NEW || disposition == CERT_STORE_ADD_NEWER || disposition == CERT_STORE_ADD_NEWER_INHERIT_PROPERTIES) error = CRYPT_E_EXISTS;
      else {
        if (out) *out = hand_out_locked(existing);
        ok = TRUE;
      }
    } else {
      fresh->store = s;
      fresh->context.hCertStore = s;
      if (s->last) s->last->next = fresh; else s->first = fresh;
      s->last = fresh;
      if (out) *out = hand_out_locked(fresh);
      ok = TRUE;
    }
  }
  store_release_locked(s);
  ReleaseSRWLockExclusive(&g_store_lock);
  if (!ok) SetLastError(error);
  return ok;
}

/* `s` is `target` or a collection that has it (at any depth; CertAddStoreToCollection refuses cycles). */
static BOOL holds_locked(const Store *s, const Store *target, int depth) {
  if (s == target) return TRUE;
  if (!s->collection || depth > 16) return FALSE;
  for (DWORD i = 0; i < s->siblings; ++i) if (holds_locked(s->sibling[i], target, depth + 1)) return TRUE;
  return FALSE;
}

/* The certificate after `previous` in a store (a collection walks its members in order), or NULL. */
static Cert *next_locked(Store *s, Cert *previous) {
  if (!s->collection) {
    if (!previous) return s->first;
    return previous->store == s && !previous->deleted ? previous->next : NULL;      /* (a deleted certificate ends the walk) */
  }
  DWORD i = 0;
  if (previous) {
    while (i < s->siblings && !holds_locked(s->sibling[i], previous->store, 0)) ++i;
    if (i == s->siblings) return NULL;
    Cert *c = next_locked(s->sibling[i], previous);
    if (c) return c;
    ++i;
  }
  for (; i < s->siblings; ++i) {
    Cert *c = next_locked(s->sibling[i], NULL);
    if (c) return c;
  }
  return NULL;
}

/* ----------------------------------------------------------------------------------------------------------------------------- stores */

static BOOL provider_is(LPCSTR provider, ULONG_PTR number, const char *name) {
  if ((ULONG_PTR)provider >> 16 == 0) return (ULONG_PTR)provider == number;
  const char *a = provider, *b = name;
  for (; *a && *b; ++a, ++b) if ((*a | 0x20) != (*b | 0x20)) return FALSE;
  return !*a && !*b;
}

EXPORT HCERTSTORE WINAPI CertOpenStore(LPCSTR provider, DWORD encoding, HCRYPTPROV_LEGACY crypt_provider, DWORD flags, const void *parameter) {
  if (flags & CERT_STORE_DELETE_FLAG) { SetLastError(ERROR_SUCCESS); return NULL; }      /* (nothing is kept, so a deletion has nothing to do) */
  const BOOL read_only = (flags & CERT_STORE_READONLY_FLAG) != 0;
  Store *s = NULL;
  if (provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_MEMORY, "Memory")) s = store_new(FALSE, FALSE);
  else if (provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_COLLECTION, "Collection")) s = store_new(TRUE, FALSE);
  else if (provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_SYSTEM_W, "System") || provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_SYSTEM_A, "System") ||
           provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_SYSTEM_REGISTRY_W, "SystemRegistry") ||
           provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_SYSTEM_REGISTRY_A, "SystemRegistry") || provider_is(provider, (ULONG_PTR)CERT_STORE_PROV_PHYSICAL_W, "Physical")) {
    if (!parameter) { SetLastError(E_INVALIDARG); return NULL; }
    s = store_new(FALSE, read_only);      /* the system stores are there, and empty */
  } else {
    SetLastError(ERROR_NOT_SUPPORTED);      /* files, the registry, PKCS #7, serialized stores, LDAP, smart cards */
    return NULL;
  }
  if (!s) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return s;
}

EXPORT HCERTSTORE WINAPI CertOpenSystemStoreW(HCRYPTPROV_LEGACY provider, LPCWSTR name) {
  if (!name) { SetLastError(E_INVALIDARG); return NULL; }
  return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, provider, CERT_SYSTEM_STORE_CURRENT_USER, name);
}
EXPORT HCERTSTORE WINAPI CertOpenSystemStoreA(HCRYPTPROV_LEGACY provider, LPCSTR name) {
  if (!name) { SetLastError(E_INVALIDARG); return NULL; }
  return CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, provider, CERT_SYSTEM_STORE_CURRENT_USER, name);
}

EXPORT HCERTSTORE WINAPI CertDuplicateStore(HCERTSTORE handle) {
  AcquireSRWLockExclusive(&g_store_lock);
  Store *s = store_of(handle);
  if (s) ++s->refs;
  ReleaseSRWLockExclusive(&g_store_lock);
  if (!s) SetLastError(ERROR_INVALID_HANDLE);
  return s;
}

EXPORT BOOL WINAPI CertCloseStore(HCERTSTORE handle, DWORD flags) {
  if (!handle) return TRUE;
  AcquireSRWLockExclusive(&g_store_lock);
  Store *s = store_of(handle);
  if (!s) { ReleaseSRWLockExclusive(&g_store_lock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  const BOOL pending = s->refs > 1;
  store_release_locked(s);      /* contexts still out keep the store alive, even with CERT_CLOSE_STORE_FORCE_FLAG (freeing them under the caller would be worse) */
  ReleaseSRWLockExclusive(&g_store_lock);
  if (pending && (flags & CERT_CLOSE_STORE_CHECK_FLAG)) { SetLastError(CRYPT_E_PENDING_CLOSE); return FALSE; }
  return TRUE;
}

EXPORT BOOL WINAPI CertAddStoreToCollection(HCERTSTORE collection, HCERTSTORE sibling, DWORD flags, DWORD priority) {
  AcquireSRWLockExclusive(&g_store_lock);
  Store *c = store_of(collection), *s = store_of(sibling);
  DWORD error = ERROR_SUCCESS;
  if (!c || !s) error = ERROR_INVALID_HANDLE;
  else if (!c->collection || holds_locked(s, c, 0)) error = E_INVALIDARG;      /* (a store in itself, directly or not) */
  else if (c->siblings == MAX_SIBLINGS) error = ERROR_NOT_ENOUGH_MEMORY;
  else {
    for (DWORD i = 0; i < c->siblings; ++i) if (c->sibling[i] == s) { ReleaseSRWLockExclusive(&g_store_lock); return TRUE; }
    DWORD at = c->siblings;      /* higher priority first; equal priorities in the order added (the priority is kept in the flags' upper half) */
    while (at > 0 && (c->sibling_flags[at - 1] >> 16) < (priority & 0xFFFF)) {
      c->sibling[at] = c->sibling[at - 1];
      c->sibling_flags[at] = c->sibling_flags[at - 1];
      --at;
    }
    c->sibling[at] = s;
    c->sibling_flags[at] = (flags & 0xFFFF) | (priority & 0xFFFF) << 16;
    ++c->siblings;
    ++s->refs;
  }
  ReleaseSRWLockExclusive(&g_store_lock);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  return TRUE;
}

EXPORT void WINAPI CertRemoveStoreFromCollection(HCERTSTORE collection, HCERTSTORE sibling) {
  AcquireSRWLockExclusive(&g_store_lock);
  Store *c = store_of(collection);
  if (c && c->collection) {
    for (DWORD i = 0; i < c->siblings; ++i) {
      if (c->sibling[i] != (Store *)sibling) continue;
      Store *s = c->sibling[i];
      for (DWORD j = i + 1; j < c->siblings; ++j) { c->sibling[j - 1] = c->sibling[j]; c->sibling_flags[j - 1] = c->sibling_flags[j]; }
      --c->siblings;
      store_release_locked(s);
      break;
    }
  }
  ReleaseSRWLockExclusive(&g_store_lock);
}

/* Resync, commit, change notification (the event is never signalled: nothing changes the stores from outside), cancelling it: all fine. */
EXPORT BOOL WINAPI CertControlStore(HCERTSTORE handle, DWORD flags, DWORD control, const void *parameter) {
  AcquireSRWLockShared(&g_store_lock);
  Store *s = store_of(handle);
  ReleaseSRWLockShared(&g_store_lock);
  if (!s) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (control < CERT_STORE_CTRL_RESYNC || control > CERT_STORE_CTRL_CANCEL_NOTIFY) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  return TRUE;
}

/* ----------------------------------------------------------------------------------------------------------------------------- contexts */

EXPORT BOOL WINAPI CertAddEncodedCertificateToStore(HCERTSTORE store, DWORD encoding, const BYTE *encoded, DWORD size, DWORD disposition, PCCERT_CONTEXT *out) {
  if (out) *out = NULL;
  if (!(encoding & X509_ASN_ENCODING) && GET_CERT_ENCODING_TYPE(encoding)) { SetLastError(E_INVALIDARG); return FALSE; }
  if (!store) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return add_encoded(store, encoded, size, disposition, out);
}

EXPORT BOOL WINAPI CertAddCertificateContextToStore(HCERTSTORE store, PCCERT_CONTEXT context, DWORD disposition, PCCERT_CONTEXT *out) {
  if (out) *out = NULL;
  if (!context || !context->pbCertEncoded) { SetLastError(E_INVALIDARG); return FALSE; }
  if (!store) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return add_encoded(store, context->pbCertEncoded, context->cbCertEncoded, disposition, out);
}

/* A certificate of its own: in a hidden memory store, which lives as long as the context. */
EXPORT PCCERT_CONTEXT WINAPI CertCreateCertificateContext(DWORD encoding, const BYTE *encoded, DWORD size) {
  if (!(encoding & X509_ASN_ENCODING) && GET_CERT_ENCODING_TYPE(encoding)) { SetLastError(E_INVALIDARG); return NULL; }
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
  if (!store) return NULL;
  PCCERT_CONTEXT out = NULL;
  const BOOL ok = add_encoded(store, encoded, size, CERT_STORE_ADD_ALWAYS, &out);
  const DWORD error = GetLastError();
  CertCloseStore(store, 0);
  if (!ok) SetLastError(error);
  return ok ? out : NULL;
}

EXPORT PCCERT_CONTEXT WINAPI CertDuplicateCertificateContext(PCCERT_CONTEXT context) {
  if (!context) return NULL;
  AcquireSRWLockExclusive(&g_store_lock);
  Cert *c = cert_of(context);
  PCCERT_CONTEXT out = c ? hand_out_locked(c) : NULL;
  ReleaseSRWLockExclusive(&g_store_lock);
  return out;
}

EXPORT BOOL WINAPI CertFreeCertificateContext(PCCERT_CONTEXT context) {
  if (!context) return TRUE;
  AcquireSRWLockExclusive(&g_store_lock);
  Cert *c = cert_of(context);
  if (c) store_release_locked(c->store);
  ReleaseSRWLockExclusive(&g_store_lock);
  return TRUE;
}

/* Takes the certificate out of its store and frees the context given (as Windows does, even when it fails). */
EXPORT BOOL WINAPI CertDeleteCertificateFromStore(PCCERT_CONTEXT context) {
  if (!context) return TRUE;
  AcquireSRWLockExclusive(&g_store_lock);
  Cert *c = cert_of(context);
  if (!c) { ReleaseSRWLockExclusive(&g_store_lock); SetLastError(E_INVALIDARG); return FALSE; }
  Store *s = c->store;
  DWORD error = ERROR_SUCCESS;
  if (s->read_only) error = E_ACCESSDENIED;
  else {
    Cert *previous = NULL, *walk = s->first;
    while (walk && walk != c) { previous = walk; walk = walk->next; }
    if (walk) {
      if (previous) previous->next = c->next; else s->first = c->next;
      if (s->last == c) s->last = previous;
      c->next = s->deleted;
      c->deleted = TRUE;
      s->deleted = c;
    }
  }
  store_release_locked(s);
  ReleaseSRWLockExclusive(&g_store_lock);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  return TRUE;
}

EXPORT PCCERT_CONTEXT WINAPI CertEnumCertificatesInStore(HCERTSTORE handle, PCCERT_CONTEXT previous) {
  AcquireSRWLockExclusive(&g_store_lock);
  Store *s = store_of(handle);
  Cert *p = cert_of(previous);
  Cert *next = s ? next_locked(s, p) : NULL;
  PCCERT_CONTEXT out = next ? hand_out_locked(next) : NULL;
  if (p) store_release_locked(p->store);      /* (the previous context is freed, as on Windows) */
  ReleaseSRWLockExclusive(&g_store_lock);
  if (!out) SetLastError(s ? CRYPT_E_NOT_FOUND : ERROR_INVALID_HANDLE);
  return out;
}

/* ----------------------------------------------------------------------------------------------------------------------------- names */

static const struct { const char *oid, *name; } kAttributeNames[] = {
    {"2.5.4.3", "CN"}, {"2.5.4.6", "C"}, {"2.5.4.7", "L"}, {"2.5.4.8", "S"}, {"2.5.4.9", "STREET"}, {"2.5.4.10", "O"}, {"2.5.4.11", "OU"}, {"2.5.4.12", "T"},
    {"2.5.4.4", "SN"}, {"2.5.4.5", "SERIALNUMBER"}, {"2.5.4.42", "G"}, {"2.5.4.43", "I"}, {"1.2.840.113549.1.9.1", "E"}, {"0.9.2342.19200300.100.1.25", "DC"},
};

/* An output buffer of wide characters that counts what does not fit. */
typedef struct { WCHAR *p; SIZE_T capacity, length; } WideOut;
static void out_char(WideOut *o, WCHAR c) { if (o->length + 1 < o->capacity) o->p[o->length] = c; ++o->length; }
static void out_ascii(WideOut *o, const char *s) { while (*s) out_char(o, (BYTE)*s++); }

/* A directory string (the value of a name attribute) as UTF-16. FALSE for types that are not strings. */
static BOOL string_value(BYTE tag, Span v, WideOut *o) {
  switch (tag) {
    case 0x0C: {      /* UTF8String */
      SIZE_T i = 0;
      while (i < v.n) {
        ULONG c = v.p[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + (SIZE_T)extra >= v.n) { out_char(o, 0xFFFD); ++i; continue; }      /* (a bad lead byte, or a sequence cut off) */
        c = extra ? c & (0x3F >> extra) : c;
        BOOL bad = FALSE;
        for (int k = 1; k <= extra; ++k) { if ((v.p[i + k] & 0xC0) != 0x80) bad = TRUE; c = c << 6 | (v.p[i + k] & 0x3F); }
        if (bad) { out_char(o, 0xFFFD); ++i; continue; }
        i += 1 + extra;
        if (c >= 0x10000 && c <= 0x10FFFF) { c -= 0x10000; out_char(o, (WCHAR)(0xD800 + (c >> 10))); out_char(o, (WCHAR)(0xDC00 + (c & 0x3FF))); }
        else out_char(o, c > 0x10FFFF ? 0xFFFD : (WCHAR)c);
      }
      return TRUE;
    }
    case 0x12: case 0x13: case 0x14: case 0x16: case 0x1A: case 0x1B:      /* Numeric, Printable, T61, IA5, Visible, General: bytes as Latin-1 */
      for (SIZE_T i = 0; i < v.n; ++i) out_char(o, v.p[i]);
      return TRUE;
    case 0x1E:      /* BMPString */
      for (SIZE_T i = 0; i + 1 < v.n; i += 2) out_char(o, (WCHAR)(v.p[i] << 8 | v.p[i + 1]));
      return TRUE;
    case 0x1C:      /* UniversalString */
      for (SIZE_T i = 0; i + 3 < v.n; i += 4) {
        ULONG c = (ULONG)v.p[i] << 24 | (ULONG)v.p[i + 1] << 16 | v.p[i + 2] << 8 | v.p[i + 3];
        if (c >= 0x10000 && c <= 0x10FFFF) { c -= 0x10000; out_char(o, (WCHAR)(0xD800 + (c >> 10))); out_char(o, (WCHAR)(0xDC00 + (c & 0x3FF))); }
        else out_char(o, c > 0x10FFFF ? 0xFFFD : (WCHAR)c);
      }
      return TRUE;
  }
  return FALSE;
}

/* Walks a Name (SEQUENCE OF SET OF AttributeTypeAndValue): calls `visit` for each attribute with its RDN index and whether it is the first of its RDN. FALSE if broken. */
typedef BOOL (*AttributeVisit)(void *context, DWORD rdn, BOOL first_in_rdn, Span oid, BYTE tag, Span value);
static BOOL walk_name(Span name, AttributeVisit visit, void *context) {
  Span seq;
  if (!der_expect(&name, 0x30, &seq, NULL) || name.n) return FALSE;
  for (DWORD rdn = 0; seq.n; ++rdn) {
    Span set;
    if (!der_expect(&seq, 0x31, &set, NULL)) return FALSE;
    for (BOOL first = TRUE; set.n; first = FALSE) {
      Span attribute, oid, value;
      BYTE tag;
      if (!der_expect(&set, 0x30, &attribute, NULL) || !der_expect(&attribute, 0x06, &oid, NULL) || !der_next(&attribute, &tag, &value, NULL) || attribute.n) return FALSE;
      if (!visit(context, rdn, first, oid, tag, value)) return TRUE;      /* (the visitor has what it wants) */
    }
  }
  return TRUE;
}

/* The first attribute of a type (by dotted OID) of a name, as a string. */
typedef struct { const char *oid; WideOut *out; BOOL found; } FindAttribute;
static BOOL find_attribute_visit(void *context, DWORD rdn, BOOL first, Span oid, BYTE tag, Span value) {
  FindAttribute *f = context;
  if (!oid_is(oid, f->oid)) return TRUE;
  f->found = string_value(tag, value, f->out);
  return !f->found;
}
static BOOL name_attribute(Span name, const char *oid, WideOut *out) {
  FindAttribute f = {oid, out, FALSE};
  const SIZE_T start = out->length;
  walk_name(name, find_attribute_visit, &f);
  if (!f.found) out->length = start;
  return f.found;
}

/* CertNameToStr: each attribute as "X=value" (or the bare value, or "OID=value"), RDNs joined with ", " ("; ", "\r\n"), attributes of one RDN with " + ". */
typedef struct { DWORD type; WideOut *out; DWORD rdns; Span *order; } NameToString;
static BOOL quote_needed(const WCHAR *s, SIZE_T n) {
  if (!n) return TRUE;
  if (s[0] == ' ' || s[n - 1] == ' ' || s[0] == '#') return TRUE;
  for (SIZE_T i = 0; i < n; ++i) if (s[i] == ',' || s[i] == '+' || s[i] == '=' || s[i] == '"' || s[i] == '\n' || s[i] == '<' || s[i] == '>' || s[i] == ';') return TRUE;
  return FALSE;
}
static void append_attribute(DWORD type, WideOut *o, Span oid, BYTE tag, Span value) {
  const DWORD kind = type & 0xFF;
  if (kind != CERT_SIMPLE_NAME_STR) {
    char text[128];
    const char *label = NULL;
    if (oid_text(oid, text, sizeof text)) {
      if (kind == CERT_X500_NAME_STR) for (SIZE_T i = 0; i < sizeof kAttributeNames / sizeof kAttributeNames[0] && !label; ++i) if (text_equal(text, kAttributeNames[i].oid)) label = kAttributeNames[i].name;
      if (!label) { if (kind == CERT_X500_NAME_STR) out_ascii(o, "OID."); out_ascii(o, text); }
      else out_ascii(o, label);
    }
    out_char(o, '=');
  }
  WCHAR buffer[1024];
  WideOut v = {buffer, sizeof buffer / sizeof buffer[0], 0};
  if (!string_value(tag, value, &v)) {      /* not a string: the hex of its encoding, "#..." */
    static const char hex[] = "0123456789ABCDEF";
    out_char(&v, '#');
    out_char(&v, hex[tag >> 4]);
    out_char(&v, hex[tag & 15]);
    for (SIZE_T i = 0; i < value.n; ++i) { out_char(&v, hex[value.p[i] >> 4]); out_char(&v, hex[value.p[i] & 15]); }
  }
  const SIZE_T n = v.length < v.capacity ? v.length : v.capacity - 1;
  const BOOL quote = !(type & CERT_NAME_STR_NO_QUOTING_FLAG) && buffer[0] != '#' && quote_needed(buffer, n);
  if (quote) out_char(o, '"');
  for (SIZE_T i = 0; i < n; ++i) { if (quote && buffer[i] == '"') out_char(o, '"'); out_char(o, buffer[i]); }
  if (quote) out_char(o, '"');
}
typedef struct { DWORD type, wanted, index; WideOut *out; } RdnAppend;
static BOOL rdn_append_visit(void *context, DWORD rdn, BOOL first, Span oid, BYTE tag, Span value) {
  RdnAppend *r = context;
  if (rdn != r->wanted) return TRUE;
  if (!first) out_ascii(r->out, (r->type & CERT_NAME_STR_NO_PLUS_FLAG) ? " " : " + ");
  append_attribute(r->type, r->out, oid, tag, value);
  return TRUE;
}
static BOOL count_visit(void *context, DWORD rdn, BOOL first, Span oid, BYTE tag, Span value) { DWORD *n = context; if (rdn + 1 > *n) *n = rdn + 1; return TRUE; }

static BOOL name_to_string(Span name, DWORD type, WideOut *o) {
  DWORD rdns = 0;
  if (!walk_name(name, count_visit, &rdns)) return FALSE;
  const char *separator = (type & CERT_NAME_STR_SEMICOLON_FLAG) ? "; " : (type & CERT_NAME_STR_CRLF_FLAG) ? "\r\n" : ", ";
  for (DWORD i = 0; i < rdns; ++i) {
    if (i) out_ascii(o, separator);
    RdnAppend r = {type, (type & CERT_NAME_STR_REVERSE_FLAG) ? rdns - 1 - i : i, 0, o};
    walk_name(name, rdn_append_visit, &r);
  }
  return TRUE;
}

static DWORD finish_string(WideOut *o, LPWSTR out, DWORD capacity) {
  const SIZE_T needed = o->length + 1;
  if (!out || !capacity) return (DWORD)needed;
  const SIZE_T n = o->length < capacity ? o->length : capacity - 1;
  out[n] = 0;
  return (DWORD)(n + 1);
}

EXPORT DWORD WINAPI CertNameToStrW(DWORD encoding, PCERT_NAME_BLOB name, DWORD type, LPWSTR out, DWORD capacity) {
  if (out && capacity) out[0] = 0;
  if (!name || (name->cbData && !name->pbData)) { SetLastError(E_INVALIDARG); return 1; }
  WideOut o = {out, out ? capacity : 0, 0};
  if (!name_to_string((Span){name->pbData, name->cbData}, type, &o)) { o.length = 0; if (out && capacity) out[0] = 0; }
  return finish_string(&o, out, capacity);
}

/* An alternative-name extension's first entry of a kind ([1] email, [2] DNS name, [6] URL). */
static BOOL alt_name(const CERT_INFO *info, BOOL issuer, BYTE kind, WideOut *o) {
  const char *oid = issuer ? "2.5.29.18" : "2.5.29.17";
  for (DWORD e = 0; e < info->cExtension; ++e) {
    if (!text_equal(info->rgExtension[e].pszObjId, oid)) continue;
    Span value = {info->rgExtension[e].Value.pbData, info->rgExtension[e].Value.cbData}, names;
    if (!der_expect(&value, 0x30, &names, NULL)) return FALSE;
    while (names.n) {
      BYTE tag;
      Span v;
      if (!der_next(&names, &tag, &v, NULL)) return FALSE;
      if (tag == (0x80 | kind)) { for (SIZE_T i = 0; i < v.n; ++i) out_char(o, v.p[i]); return TRUE; }
    }
  }
  return FALSE;
}

EXPORT DWORD WINAPI CertGetNameStringW(PCCERT_CONTEXT context, DWORD type, DWORD flags, void *type_parameter, LPWSTR out, DWORD capacity) {
  if (out && capacity) out[0] = 0;
  if (!context || !context->pCertInfo) return 1;
  const CERT_INFO *info = context->pCertInfo;
  const BOOL issuer = (flags & CERT_NAME_ISSUER_FLAG) != 0;
  const Span name = issuer ? (Span){info->Issuer.pbData, info->Issuer.cbData} : (Span){info->Subject.pbData, info->Subject.cbData};
  WideOut o = {out, out ? capacity : 0, 0};
  switch (type) {
    case CERT_NAME_EMAIL_TYPE:
      if (!alt_name(info, issuer, 1, &o)) name_attribute(name, "1.2.840.113549.1.9.1", &o);
      break;
    case CERT_NAME_RDN_TYPE:
      name_to_string(name, type_parameter ? *(const DWORD *)type_parameter : CERT_X500_NAME_STR, &o);
      break;
    case CERT_NAME_ATTR_TYPE:
      if (type_parameter) name_attribute(name, type_parameter, &o);
      break;
    case CERT_NAME_SIMPLE_DISPLAY_TYPE:
    case CERT_NAME_FRIENDLY_DISPLAY_TYPE:      /* (no friendly-name property is ever set) */
      if (!name_attribute(name, "2.5.4.3", &o) && !name_attribute(name, "2.5.4.11", &o) && !name_attribute(name, "2.5.4.10", &o) &&
          !name_attribute(name, "1.2.840.113549.1.9.1", &o))
        alt_name(info, issuer, 1, &o);
      break;
    case CERT_NAME_DNS_TYPE:
      if (!alt_name(info, issuer, 2, &o)) name_attribute(name, "2.5.4.3", &o);
      break;
    case CERT_NAME_URL_TYPE:
      alt_name(info, issuer, 6, &o);
      break;
    default:      /* CERT_NAME_UPN_TYPE and the rest: none */
      break;
  }
  return finish_string(&o, out, capacity);
}

EXPORT BOOL WINAPI CertCompareCertificateName(DWORD encoding, PCERT_NAME_BLOB a, PCERT_NAME_BLOB b) {
  if (!a || !b) { SetLastError(E_INVALIDARG); return FALSE; }
  return a->cbData == b->cbData && (!a->cbData || mem_equal(a->pbData, b->pbData, a->cbData));
}

static BOOL blob_equal(const CRYPT_INTEGER_BLOB *a, const CRYPT_INTEGER_BLOB *b) { return a->cbData == b->cbData && (!a->cbData || mem_equal(a->pbData, b->pbData, a->cbData)); }

/* ----------------------------------------------------------------------------------------------------------------------------- properties, usages, time */

EXPORT BOOL WINAPI CertGetCertificateContextProperty(PCCERT_CONTEXT context, DWORD property, void *data, DWORD *size) {
  if (!context || !size) { SetLastError(E_INVALIDARG); return FALSE; }
  LPCWSTR algorithm = NULL;
  DWORD length = 0;
  switch (property) {
    case CERT_SHA1_HASH_PROP_ID: algorithm = BCRYPT_SHA1_ALGORITHM; length = 20; break;
    case CERT_MD5_HASH_PROP_ID: algorithm = BCRYPT_MD5_ALGORITHM; length = 16; break;
    case CERT_SHA256_HASH_PROP_ID: algorithm = BCRYPT_SHA256_ALGORITHM; length = 32; break;
    default: SetLastError(CRYPT_E_NOT_FOUND); return FALSE;      /* no other property is kept */
  }
  if (!data) { *size = length; return TRUE; }
  if (*size < length) { *size = length; SetLastError(ERROR_MORE_DATA); return FALSE; }
  if (!digest(algorithm, context->pbCertEncoded, context->cbCertEncoded, data, length)) { SetLastError(NTE_FAIL); return FALSE; }
  *size = length;
  return TRUE;
}

static const CERT_EXTENSION *extension_of(const CERT_INFO *info, const char *oid) {
  for (DWORD e = 0; e < info->cExtension; ++e) if (text_equal(info->rgExtension[e].pszObjId, oid)) return &info->rgExtension[e];
  return NULL;
}

EXPORT BOOL WINAPI CertGetEnhancedKeyUsage(PCCERT_CONTEXT context, DWORD flags, PCERT_ENHKEY_USAGE usage, DWORD *size) {
  if (!context || !context->pCertInfo || !size) { SetLastError(E_INVALIDARG); return FALSE; }
  const CERT_EXTENSION *e = (flags & CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG) ? NULL : extension_of(context->pCertInfo, "2.5.29.37");
  Span list = {NULL, 0};
  DWORD count = 0;
  SIZE_T text = 0;
  if (e) {
    Span value = {e->Value.pbData, e->Value.cbData};
    if (!der_expect(&value, 0x30, &list, NULL) || value.n) { SetLastError(CRYPT_E_ASN1_BADTAG); return FALSE; }
    for (Span walk = list; walk.n;) {
      Span oid;
      SIZE_T n;
      if (!der_expect(&walk, 0x06, &oid, NULL) || !(n = oid_text(oid, NULL, 0))) { SetLastError(CRYPT_E_ASN1_BADTAG); return FALSE; }
      ++count;
      text += n;
    }
  }
  const SIZE_T needed = sizeof(CERT_ENHKEY_USAGE) + sizeof(LPSTR) * count + text;
  if (!usage) { *size = (DWORD)needed; return TRUE; }
  if (*size < needed) { *size = (DWORD)needed; SetLastError(ERROR_MORE_DATA); return FALSE; }
  LPSTR *array = (LPSTR *)(usage + 1);
  char *texts = (char *)(array + count), *end = (char *)usage + needed;
  usage->cUsageIdentifier = count;
  usage->rgpszUsageIdentifier = count ? array : NULL;
  DWORD i = 0;
  for (Span walk = list; walk.n; ++i) {
    Span oid;
    der_expect(&walk, 0x06, &oid, NULL);
    array[i] = texts;
    texts += oid_text(oid, texts, (SIZE_T)(end - texts));
  }
  *size = (DWORD)needed;
  if (!count) SetLastError(CRYPT_E_NOT_FOUND);      /* (no usage extension: good for every use, as Windows says it) */
  return TRUE;
}

EXPORT BOOL WINAPI CertGetIntendedKeyUsage(DWORD encoding, PCERT_INFO info, BYTE *usage, DWORD size) {
  if (usage) mem_zero(usage, size);
  if (!info) { SetLastError(E_INVALIDARG); return FALSE; }
  const CERT_EXTENSION *e = extension_of(info, "2.5.29.15");
  if (!e) { SetLastError(ERROR_SUCCESS); return FALSE; }
  Span value = {e->Value.pbData, e->Value.cbData}, bits_tlv, bits;
  BYTE unused;
  if (!der_expect(&value, 0x03, &bits_tlv, NULL) || !der_bits(bits_tlv, &bits, &unused)) { SetLastError(CRYPT_E_ASN1_BADTAG); return FALSE; }
  for (SIZE_T i = 0; i < bits.n && i < size; ++i) usage[i] = bits.p[i];
  return TRUE;
}

EXPORT LONG WINAPI CertVerifyTimeValidity(LPFILETIME time, PCERT_INFO info) {
  FILETIME now;
  if (!info) return 0;
  if (!time) { GetSystemTimeAsFileTime(&now); time = &now; }
  if (CompareFileTime(time, &info->NotBefore) < 0) return -1;
  if (CompareFileTime(time, &info->NotAfter) > 0) return 1;
  return 0;
}

/* ----------------------------------------------------------------------------------------------------------------------------- finding */

static BOOL wide_contains_nocase(const WCHAR *text, SIZE_T n, LPCWSTR part) {
  const SIZE_T m = wide_length(part);
  if (!m) return TRUE;
  for (SIZE_T i = 0; i + m <= n; ++i) {
    SIZE_T k = 0;
    for (; k < m; ++k) {
      WCHAR a = text[i + k], b = part[k];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
    }
    if (k == m) return TRUE;
  }
  return FALSE;
}

static BOOL matches(const Cert *c, DWORD type, const void *parameter) {
  const CERT_INFO *info = &c->info;
  switch (type) {
    case CERT_FIND_ANY: return TRUE;
    case CERT_FIND_SUBJECT_NAME: return CertCompareCertificateName(0, (PCERT_NAME_BLOB)&info->Subject, (PCERT_NAME_BLOB)parameter);
    case CERT_FIND_ISSUER_NAME: return CertCompareCertificateName(0, (PCERT_NAME_BLOB)&info->Issuer, (PCERT_NAME_BLOB)parameter);
    case CERT_FIND_SUBJECT_STR_W:
    case CERT_FIND_ISSUER_STR_W: {
      WCHAR buffer[2048];
      WideOut o = {buffer, sizeof buffer / sizeof buffer[0], 0};
      const CERT_NAME_BLOB *name = type == CERT_FIND_SUBJECT_STR_W ? &info->Subject : &info->Issuer;
      if (!name_to_string((Span){name->pbData, name->cbData}, CERT_SIMPLE_NAME_STR, &o)) return FALSE;
      return wide_contains_nocase(buffer, o.length < o.capacity ? o.length : o.capacity - 1, parameter);
    }
    case CERT_FIND_EXISTING: {
      const CERT_CONTEXT *other = parameter;
      return other->cbCertEncoded == c->context.cbCertEncoded && mem_equal(other->pbCertEncoded, c->context.pbCertEncoded, other->cbCertEncoded);
    }
    case CERT_FIND_SUBJECT_CERT: {
      const CERT_INFO *other = parameter;
      return CertCompareCertificateName(0, (PCERT_NAME_BLOB)&info->Issuer, (PCERT_NAME_BLOB)&other->Issuer) && blob_equal(&info->SerialNumber, &other->SerialNumber);
    }
    case CERT_FIND_ISSUER_OF: {
      const CERT_CONTEXT *subject = parameter;
      return subject->pCertInfo && CertCompareCertificateName(0, (PCERT_NAME_BLOB)&info->Subject, &subject->pCertInfo->Issuer);
    }
    case CERT_FIND_PUBLIC_KEY: {
      const CERT_PUBLIC_KEY_INFO *key = parameter;
      return key->PublicKey.cbData == info->SubjectPublicKeyInfo.PublicKey.cbData &&
             mem_equal(key->PublicKey.pbData, info->SubjectPublicKeyInfo.PublicKey.pbData, key->PublicKey.cbData);
    }
    case CERT_FIND_SHA1_HASH:
    case CERT_FIND_MD5_HASH: {
      const CRYPT_HASH_BLOB *hash = parameter;
      BYTE d[20];
      const DWORD n = type == CERT_FIND_SHA1_HASH ? 20 : 16;
      return hash->cbData == n && digest(n == 20 ? BCRYPT_SHA1_ALGORITHM : BCRYPT_MD5_ALGORITHM, c->context.pbCertEncoded, c->context.cbCertEncoded, d, n) && mem_equal(d, hash->pbData, n);
    }
  }
  return FALSE;
}

EXPORT PCCERT_CONTEXT WINAPI CertFindCertificateInStore(HCERTSTORE handle, DWORD encoding, DWORD find_flags, DWORD type, const void *parameter, PCCERT_CONTEXT previous) {
  const BOOL known = type == CERT_FIND_ANY || type == CERT_FIND_SUBJECT_NAME || type == CERT_FIND_ISSUER_NAME || type == CERT_FIND_SUBJECT_STR_W ||
                     type == CERT_FIND_ISSUER_STR_W || type == CERT_FIND_EXISTING || type == CERT_FIND_SUBJECT_CERT || type == CERT_FIND_ISSUER_OF ||
                     type == CERT_FIND_PUBLIC_KEY || type == CERT_FIND_SHA1_HASH || type == CERT_FIND_MD5_HASH;
  if (!known || (type != CERT_FIND_ANY && !parameter)) {
    CertFreeCertificateContext(previous);
    SetLastError(known ? E_INVALIDARG : CRYPT_E_NOT_FOUND);      /* (finding by key identifier, usage, property ... is not here: nothing has those) */
    return NULL;
  }
  AcquireSRWLockExclusive(&g_store_lock);
  Store *s = store_of(handle);
  Cert *p = cert_of(previous), *c = s ? next_locked(s, p) : NULL;
  while (c && !matches(c, type, parameter)) c = next_locked(s, c);
  PCCERT_CONTEXT out = c ? hand_out_locked(c) : NULL;
  if (p) store_release_locked(p->store);
  ReleaseSRWLockExclusive(&g_store_lock);
  if (!out) SetLastError(s ? CRYPT_E_NOT_FOUND : ERROR_INVALID_HANDLE);
  return out;
}

EXPORT PCCERT_CONTEXT WINAPI CertGetSubjectCertificateFromStore(HCERTSTORE handle, DWORD encoding, PCERT_INFO id) {
  if (!id) { SetLastError(E_INVALIDARG); return NULL; }
  return CertFindCertificateInStore(handle, encoding, 0, CERT_FIND_SUBJECT_CERT, id, NULL);
}

/* No certificate here has a private key, so no chain of one is found. */
EXPORT PCCERT_CHAIN_CONTEXT WINAPI CertFindChainInStore(HCERTSTORE handle, DWORD encoding, DWORD flags, DWORD type, const void *parameter, PCCERT_CHAIN_CONTEXT previous) {
  SetLastError(store_of(handle) ? CRYPT_E_NOT_FOUND : ERROR_INVALID_HANDLE);
  return NULL;
}

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

/* crypt32.dll and secur32.dll: DPAPI round trips (description, entropy, tampering, truncation), CryptProtectMemory, certificate stores (memory, system, collection,
 * lifetimes after CertCloseStore), a parsed certificate (CERT_INFO, names, hashes, key usages, validity, finding) and damaged encodings, GetUserNameEx and the SSPI
 * functions that report no packages (mingw gcc, -lcrypt32 -lsecur32; run it with `TMPDIR=... tests/emulated_windows.sh tests/windows/crypt32test.c` or as crypt32_own.exe
 * through `scripts/winetest.sh crypt32 own`). Prints "crypt32test: N checks, M failed"; the exit code is the number of failures. */
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <windows.h>
#include <wincrypt.h>
#include <sspi.h>
#include <secext.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL line %d: %s (last error 0x%lx)\n", line, what, GetLastError());
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

/* A self-signed P-256 certificate (openssl): C=US, O=Theory Craft, OU=Tests, CN=tcos.test; DNS www.tcos.test, email a@tcos.test; server and client auth; key usage
 * digitalSignature | keyEncipherment (critical); valid 2026-11-07 05:57:43 to 2036-11-04 05:57:43; SHA-1 7507...0D65. */
static const BYTE kCert[] = {
  0x30, 0x82, 0x02, 0x3e, 0x30, 0x82, 0x01, 0xe3, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14, 0x28, 0x5d, 0x04, 0x54, 0x47, 0x17, 0xc4, 0x6a, 0xa2, 0x31,
  0x3d, 0x0c, 0x7f, 0x80, 0xde, 0x94, 0xa2, 0xeb, 0xeb, 0xce, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x48, 0x31,
  0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x15, 0x30, 0x13, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x0c, 0x54, 0x68,
  0x65, 0x6f, 0x72, 0x79, 0x20, 0x43, 0x72, 0x61, 0x66, 0x74, 0x31, 0x0e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x0b, 0x0c, 0x05, 0x54, 0x65, 0x73, 0x74,
  0x73, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x09, 0x74, 0x63, 0x6f, 0x73, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1e, 0x17, 0x0d,
  0x32, 0x36, 0x31, 0x31, 0x30, 0x37, 0x30, 0x35, 0x35, 0x37, 0x34, 0x33, 0x5a, 0x17, 0x0d, 0x33, 0x36, 0x31, 0x31, 0x30, 0x34, 0x30, 0x35, 0x35, 0x37,
  0x34, 0x33, 0x5a, 0x30, 0x48, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x15, 0x30, 0x13, 0x06, 0x03, 0x55,
  0x04, 0x0a, 0x0c, 0x0c, 0x54, 0x68, 0x65, 0x6f, 0x72, 0x79, 0x20, 0x43, 0x72, 0x61, 0x66, 0x74, 0x31, 0x0e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x04, 0x0b,
  0x0c, 0x05, 0x54, 0x65, 0x73, 0x74, 0x73, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x09, 0x74, 0x63, 0x6f, 0x73, 0x2e, 0x74, 0x65,
  0x73, 0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07,
  0x03, 0x42, 0x00, 0x04, 0xe4, 0xa7, 0x1d, 0xe5, 0x6f, 0xe7, 0x80, 0xf7, 0xb3, 0x26, 0x94, 0x9c, 0xcf, 0x46, 0x3f, 0x23, 0xbf, 0x17, 0x37, 0x9e, 0x2f,
  0xdc, 0x3d, 0x91, 0xb5, 0xfe, 0x7d, 0x9d, 0xe1, 0xe9, 0x93, 0xf9, 0x3f, 0x35, 0xe5, 0x60, 0xac, 0x93, 0xa3, 0x05, 0x88, 0x10, 0xaa, 0x62, 0x40, 0x1d,
  0xa8, 0x13, 0x06, 0x89, 0x38, 0xef, 0xc0, 0xcf, 0x1c, 0x76, 0x6a, 0x83, 0x08, 0xad, 0x30, 0xbe, 0xcc, 0xf7, 0xa3, 0x81, 0xaa, 0x30, 0x81, 0xa7, 0x30,
  0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xed, 0x3b, 0x5b, 0x3c, 0x0a, 0x4b, 0x01, 0x49, 0x5a, 0x78, 0xb0, 0x0c, 0x5f, 0xb9, 0xa3,
  0x8c, 0x69, 0xb8, 0x0b, 0xc3, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xed, 0x3b, 0x5b, 0x3c, 0x0a, 0x4b, 0x01,
  0x49, 0x5a, 0x78, 0xb0, 0x0c, 0x5f, 0xb9, 0xa3, 0x8c, 0x69, 0xb8, 0x0b, 0xc3, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05,
  0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x25, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x04, 0x1e, 0x30, 0x1c, 0x82, 0x0d, 0x77, 0x77, 0x77, 0x2e, 0x74, 0x63, 0x6f,
  0x73, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x81, 0x0b, 0x61, 0x40, 0x74, 0x63, 0x6f, 0x73, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d,
  0x25, 0x04, 0x16, 0x30, 0x14, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02,
  0x30, 0x0e, 0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x05, 0xa0, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
  0x04, 0x03, 0x02, 0x03, 0x49, 0x00, 0x30, 0x46, 0x02, 0x21, 0x00, 0x98, 0xad, 0xc7, 0x63, 0x71, 0x20, 0xc0, 0x93, 0xb1, 0xd0, 0x77, 0xfe, 0x17, 0x72,
  0x38, 0xb5, 0xf2, 0x47, 0xdf, 0x57, 0xc7, 0x3a, 0x11, 0xd9, 0xf0, 0x76, 0x39, 0x14, 0xd0, 0x53, 0x6b, 0xa8, 0x02, 0x21, 0x00, 0x8d, 0x38, 0xde, 0x74,
  0x06, 0x0d, 0xc1, 0x27, 0xb9, 0x79, 0xcf, 0x08, 0x1c, 0x78, 0xbb, 0x39, 0x8b, 0x96, 0x8d, 0x61, 0xb0, 0xe5, 0xe9, 0xf6, 0xaa, 0xe8, 0x6a, 0x53, 0x75,
  0xc6, 0x71, 0x32};

static FILETIME file_time(int year, int month, int day) {
  SYSTEMTIME st = {0};
  FILETIME ft;
  st.wYear = (WORD)year;
  st.wMonth = (WORD)month;
  st.wDay = (WORD)day;
  SystemTimeToFileTime(&st, &ft);
  return ft;
}

static void test_dpapi(void) {
  BYTE secret[100];
  for (int i = 0; i < 100; ++i) secret[i] = (BYTE)(i * 7 + 1);
  DATA_BLOB in = {sizeof secret, secret}, out = {0}, back = {0}, entropy = {5, (BYTE *)"salty"}, wrong = {5, (BYTE *)"salts"};
  check(CryptProtectData(&in, L"cookie key", &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out), "CryptProtectData");
  check(out.pbData && out.cbData > sizeof secret, "a blob");
  LPWSTR description = NULL;
  check(CryptUnprotectData(&out, &description, &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &back), "CryptUnprotectData");
  check(back.cbData == sizeof secret && back.pbData && !memcmp(back.pbData, secret, sizeof secret), "the data back");
  check(description && !wcscmp(description, L"cookie key"), "the description back");
  LocalFree(description);
  LocalFree(back.pbData);
  back.pbData = NULL;
  SetLastError(0);
  check(!CryptUnprotectData(&out, NULL, &wrong, NULL, NULL, 0, &back) && GetLastError() == ERROR_INVALID_DATA && !back.pbData, "the wrong entropy fails");
  check(!CryptUnprotectData(&out, NULL, NULL, NULL, NULL, 0, &back) && GetLastError() == ERROR_INVALID_DATA, "no entropy fails");
  /* every byte changed fails, and every truncation */
  int tampered_ok = 1, truncated_ok = 1;
  for (DWORD i = 0; i < out.cbData; ++i) {
    out.pbData[i] ^= 0x10;
    DATA_BLOB b = {0};
    if (CryptUnprotectData(&out, NULL, &entropy, NULL, NULL, 0, &b) || b.pbData) tampered_ok = 0;
    out.pbData[i] ^= 0x10;
  }
  for (DWORD n = 0; n < out.cbData; ++n) {
    DATA_BLOB cut = {n, out.pbData}, b = {0};
    if (CryptUnprotectData(&cut, NULL, &entropy, NULL, NULL, 0, &b) || b.pbData) truncated_ok = 0;
  }
  check(tampered_ok, "every changed byte is refused");
  check(truncated_ok, "every truncation is refused");
  DATA_BLOB again = {0};
  check(CryptProtectData(&in, L"cookie key", &entropy, NULL, NULL, 0, &again) && again.cbData == out.cbData && memcmp(again.pbData, out.pbData, out.cbData), "two blobs differ");
  LocalFree(again.pbData);
  LocalFree(out.pbData);
  /* empty data, no description */
  DATA_BLOB empty = {0, NULL}, e_out = {0}, e_back = {0};
  check(CryptProtectData(&empty, NULL, NULL, NULL, NULL, 0, &e_out), "protect nothing");
  description = (LPWSTR)1;
  check(CryptUnprotectData(&e_out, &description, NULL, NULL, NULL, 0, &e_back) && e_back.cbData == 0, "unprotect nothing");
  check(description && description[0] == 0, "an empty description");
  LocalFree(description);
  LocalFree(e_back.pbData);
  LocalFree(e_out.pbData);
  check(!CryptProtectData(NULL, NULL, NULL, NULL, NULL, 0, &e_out) && GetLastError() == ERROR_INVALID_PARAMETER, "no input");
  BYTE junk[200] = {0};
  DATA_BLOB j = {sizeof junk, junk};
  check(!CryptUnprotectData(&j, NULL, NULL, NULL, NULL, 0, &e_back) && GetLastError() == ERROR_INVALID_DATA, "a blob of zeros");
}

static void test_memory(void) {
  BYTE data[32], copy[32];
  for (int i = 0; i < 32; ++i) data[i] = copy[i] = (BYTE)i;
  check(CryptProtectMemory(data, 32, CRYPTPROTECTMEMORY_SAME_PROCESS), "CryptProtectMemory");
  check(memcmp(data, copy, 32) != 0, "the memory is scrambled");
  check(CryptUnprotectMemory(data, 32, CRYPTPROTECTMEMORY_SAME_PROCESS), "CryptUnprotectMemory");
  check(!memcmp(data, copy, 32), "and back");
  check(!CryptProtectMemory(data, 15, 0) && GetLastError() == ERROR_INVALID_PARAMETER, "not a multiple of 16");
  check(!CryptProtectMemory(data, 16, 7) && GetLastError() == ERROR_INVALID_PARAMETER, "a bad flag");
}

static void test_certificate(void) {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
  check(store != NULL, "a memory store");
  SetLastError(0);
  check(!CertEnumCertificatesInStore(store, NULL) && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "an empty store");
  PCCERT_CONTEXT c = NULL;
  check(CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_ALWAYS, &c) && c, "add a certificate");
  if (!c) { CertCloseStore(store, 0); return; }
  const CERT_INFO *info = c->pCertInfo;
  check(c->cbCertEncoded == sizeof kCert && !memcmp(c->pbCertEncoded, kCert, sizeof kCert) && c->hCertStore == store, "the context");
  check(info->dwVersion == CERT_V3, "version 3");
  check(info->SerialNumber.cbData == 20 && info->SerialNumber.pbData[0] == 0xCE && info->SerialNumber.pbData[19] == 0x28, "the serial, little-endian");
  check(!strcmp(info->SignatureAlgorithm.pszObjId, szOID_ECDSA_SHA256), "ecdsa-with-SHA256");
  check(!strcmp(info->SubjectPublicKeyInfo.Algorithm.pszObjId, szOID_ECC_PUBLIC_KEY) && info->SubjectPublicKeyInfo.PublicKey.cbData == 65 &&
        info->SubjectPublicKeyInfo.PublicKey.pbData[0] == 4, "the public key");
  check(info->cExtension == 6, "the extensions");
  check(CertCompareCertificateName(X509_ASN_ENCODING, (PCERT_NAME_BLOB)&info->Subject, (PCERT_NAME_BLOB)&info->Issuer), "self-signed: subject == issuer");
  FILETIME t2020 = file_time(2020, 1, 1), t2030 = file_time(2030, 1, 1), t2040 = file_time(2040, 1, 1);
  check(CertVerifyTimeValidity(&t2020, (PCERT_INFO)info) == -1 && CertVerifyTimeValidity(&t2030, (PCERT_INFO)info) == 0 &&
        CertVerifyTimeValidity(&t2040, (PCERT_INFO)info) == 1, "the validity");
  SYSTEMTIME st;
  FileTimeToSystemTime(&info->NotBefore, &st);
  check(st.wYear == 2026 && st.wMonth == 11 && st.wDay == 7 && st.wHour == 5 && st.wMinute == 57 && st.wSecond == 43, "notBefore");
  WCHAR name[256];
  check(CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256) == 10 && !wcscmp(name, L"tcos.test"), "simple display name");
  check(CertGetNameStringW(c, CERT_NAME_DNS_TYPE, 0, NULL, name, 256) && !wcscmp(name, L"www.tcos.test"), "DNS name");
  check(CertGetNameStringW(c, CERT_NAME_EMAIL_TYPE, 0, NULL, name, 256) && !wcscmp(name, L"a@tcos.test"), "email");
  check(CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, name, 256) && !wcscmp(name, L"Theory Craft"), "the O attribute");
  DWORD str_type = CERT_X500_NAME_STR;
  check(CertGetNameStringW(c, CERT_NAME_RDN_TYPE, 0, &str_type, name, 256) && !wcscmp(name, L"C=US, O=Theory Craft, OU=Tests, CN=tcos.test"), "the RDN string");
  str_type = CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG;
  check(CertGetNameStringW(c, CERT_NAME_RDN_TYPE, CERT_NAME_ISSUER_FLAG, &str_type, name, 256) && !wcscmp(name, L"CN=tcos.test, OU=Tests, O=Theory Craft, C=US"), "reversed");
  check(CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, NULL, 0) == 10, "the size of a name");
  check(CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 4) == 4 && !wcscmp(name, L"tco"), "a cut name");
  check(CertGetNameStringW(c, CERT_NAME_URL_TYPE, 0, NULL, name, 256) == 1 && name[0] == 0, "no URL");
  check(CertNameToStrW(X509_ASN_ENCODING, (PCERT_NAME_BLOB)&info->Subject, CERT_SIMPLE_NAME_STR, name, 256) && !wcscmp(name, L"US, Theory Craft, Tests, tcos.test"), "CertNameToStrW");
  BYTE hash[32];
  DWORD size = 0;
  static const BYTE sha1[20] = {0x75, 0x07, 0x24, 0xCE, 0x4D, 0xF5, 0x86, 0x86, 0x6C, 0xC2, 0x44, 0x35, 0x4A, 0x1C, 0xD2, 0xCC, 0xFE, 0x1B, 0x0D, 0x65};
  check(CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, NULL, &size) && size == 20, "the SHA-1 size");
  size = 10;
  check(!CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, hash, &size) && GetLastError() == ERROR_MORE_DATA && size == 20, "too small");
  check(CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, hash, &size) && !memcmp(hash, sha1, 20), "the SHA-1 thumbprint");
  size = sizeof hash;
  check(!CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, hash, &size) && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "no key");
  BYTE usage_buffer[512];
  PCERT_ENHKEY_USAGE usage = (PCERT_ENHKEY_USAGE)usage_buffer;
  size = sizeof usage_buffer;
  check(CertGetEnhancedKeyUsage(c, 0, usage, &size) && usage->cUsageIdentifier == 2 && !strcmp(usage->rgpszUsageIdentifier[0], szOID_PKIX_KP_SERVER_AUTH) &&
        !strcmp(usage->rgpszUsageIdentifier[1], szOID_PKIX_KP_CLIENT_AUTH), "the extended key usages");
  BYTE key_usage[2] = {0xFF, 0xFF};
  check(CertGetIntendedKeyUsage(X509_ASN_ENCODING, (PCERT_INFO)info, key_usage, 2) && key_usage[0] == 0xA0 && key_usage[1] == 0, "the key usage");
  /* finding */
  CRYPT_HASH_BLOB thumb = {20, (BYTE *)sha1};
  PCCERT_CONTEXT f = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &thumb, NULL);
  check(f && f->pbCertEncoded == c->pbCertEncoded, "find by thumbprint");
  f = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &thumb, f);
  check(!f && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "only once");
  f = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, L"THEORY craft", NULL);
  check(f != NULL, "find by a part of the subject");
  CertFreeCertificateContext(f);
  f = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_ISSUER_OF, c, NULL);
  check(f != NULL, "its own issuer");
  CertFreeCertificateContext(f);
  f = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, L"nobody", NULL);
  check(!f && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "nothing found");
  /* dispositions */
  PCCERT_CONTEXT d = NULL;
  check(!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_NEW, &d) && GetLastError() == (DWORD)CRYPT_E_EXISTS && !d,
        "ADD_NEW of one that is there");
  check(CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_USE_EXISTING, &d) && d && d->pbCertEncoded == c->pbCertEncoded, "USE_EXISTING");
  CertFreeCertificateContext(d);
  int count = 0;
  for (PCCERT_CONTEXT e = CertEnumCertificatesInStore(store, NULL); e; e = CertEnumCertificatesInStore(store, e)) ++count;
  check(count == 1, "one certificate in the store");
  PCCERT_CONTEXT extra = NULL;
  check(CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_ALWAYS, &extra) && extra && extra != c, "ADD_ALWAYS adds a copy");
  check(CertDeleteCertificateFromStore(extra), "delete it");
  count = 0;
  for (PCCERT_CONTEXT e = CertEnumCertificatesInStore(store, NULL); e; e = CertEnumCertificatesInStore(store, e)) ++count;
  check(count == 1, "one again");
  /* a collection with the store in it; then the store is closed but the context keeps it */
  HCERTSTORE collection = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, NULL);
  HCERTSTORE second = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
  check(collection && second, "a collection");
  check(CertAddStoreToCollection(collection, second, CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG, 0) && CertAddStoreToCollection(collection, store, 0, 0), "members");
  check(!CertAddStoreToCollection(collection, collection, 0, 0), "not in itself");
  check(CertAddEncodedCertificateToStore(collection, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_ALWAYS, NULL), "add through the collection");
  count = 0;
  for (PCCERT_CONTEXT e = CertEnumCertificatesInStore(collection, NULL); e; e = CertEnumCertificatesInStore(collection, e)) ++count;
  check(count == 2, "the collection sees both stores");
  check(CertCloseStore(second, 0) && CertCloseStore(collection, 0), "close the collection");
  check(!CertCloseStore(store, CERT_CLOSE_STORE_CHECK_FLAG) && GetLastError() == (DWORD)CRYPT_E_PENDING_CLOSE, "a context is still out");
  PCCERT_CONTEXT e = CertEnumCertificatesInStore(c->hCertStore, NULL);
  check(e && e->pbCertEncoded == c->pbCertEncoded, "the closed store lives while its context does");
  CertFreeCertificateContext(e);
  PCCERT_CONTEXT dup = CertDuplicateCertificateContext(c);
  check(dup == c, "duplicate");
  CertFreeCertificateContext(dup);
  check(CertFreeCertificateContext(c), "free");
  check(CertFreeCertificateContext(NULL), "free nothing");
  /* standalone */
  PCCERT_CONTEXT alone = CertCreateCertificateContext(X509_ASN_ENCODING, kCert, sizeof kCert);
  check(alone && alone->pCertInfo->cExtension == 6, "CertCreateCertificateContext");
  CertFreeCertificateContext(alone);
  /* damaged encodings: every truncation is refused; flipped bits never crash */
  int truncated_ok = 1, survived = 1;
  HCERTSTORE s2 = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
  for (DWORD n = 0; n < sizeof kCert; ++n) if (CertAddEncodedCertificateToStore(s2, X509_ASN_ENCODING, kCert, n, CERT_STORE_ADD_ALWAYS, NULL)) truncated_ok = 0;
  BYTE damaged[sizeof kCert];
  for (DWORD i = 0; i < sizeof kCert; ++i) {
    for (int bit = 0; bit < 8; bit += 3) {
      memcpy(damaged, kCert, sizeof kCert);
      damaged[i] ^= (BYTE)(1 << bit);
      PCCERT_CONTEXT x = NULL;
      if (CertAddEncodedCertificateToStore(s2, X509_ASN_ENCODING, damaged, sizeof damaged, CERT_STORE_ADD_ALWAYS, &x)) {
        WCHAR n2[64];
        if (!x || !x->pCertInfo) survived = 0;
        else {
          CertGetNameStringW(x, CERT_NAME_RDN_TYPE, 0, NULL, n2, 64);
          CertGetNameStringW(x, CERT_NAME_DNS_TYPE, 0, NULL, n2, 64);
          DWORD u = sizeof usage_buffer;
          CertGetEnhancedKeyUsage(x, 0, usage, &u);
        }
        CertFreeCertificateContext(x);
      }
    }
  }
  check(truncated_ok, "every truncated certificate is refused");
  check(survived, "damaged certificates");
  check(!CertAddEncodedCertificateToStore(s2, X509_ASN_ENCODING, (const BYTE *)"\x30\x84\xff\xff\xff\xff", 6, CERT_STORE_ADD_ALWAYS, NULL), "a huge length");
  CertCloseStore(s2, 0);
}

static void test_system_stores(void) {
  HCERTSTORE root = CertOpenSystemStoreW(0, L"ROOT");
  check(root != NULL, "the ROOT store");
  check(!CertEnumCertificatesInStore(root, NULL), "is empty");
  check(CertControlStore(root, 0, CERT_STORE_CTRL_RESYNC, NULL), "resync");
  check(!CertFindCertificateInStore(root, X509_ASN_ENCODING, 0, CERT_FIND_ANY, NULL, NULL) && GetLastError() == (DWORD)CRYPT_E_NOT_FOUND, "find nothing");
  check(!CertFindChainInStore(root, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, NULL, NULL), "no chains");
  check(CertCloseStore(root, CERT_CLOSE_STORE_CHECK_FLAG), "close");
  HCERTSTORE my = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"MY");
  check(my != NULL, "MY");
  check(!CertAddEncodedCertificateToStore(my, X509_ASN_ENCODING, kCert, sizeof kCert, CERT_STORE_ADD_ALWAYS, NULL) && GetLastError() == (DWORD)E_ACCESSDENIED, "read-only");
  CertCloseStore(my, 0);
  check(!CertOpenStore(CERT_STORE_PROV_FILENAME_W, 0, 0, 0, L"x.sst") && GetLastError() == ERROR_NOT_SUPPORTED, "file stores are not here");
  HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 1;
  DWORD spec = 1;
  BOOL free_it = TRUE;
  check(!CryptAcquireCertificatePrivateKey(NULL, 0, NULL, &key, &spec, &free_it) && !key && !spec && !free_it, "no private keys");
  DWORD enc = 1, content = 1, format = 1;
  HCERTSTORE qs = (HCERTSTORE)1;
  HCRYPTMSG qm = (HCRYPTMSG)1;
  check(!CryptQueryObject(CERT_QUERY_OBJECT_FILE, L"C:\\x.exe", CERT_QUERY_CONTENT_FLAG_ALL, CERT_QUERY_FORMAT_FLAG_ALL, 0, &enc, &content, &format, &qs, &qm, NULL) &&
        !enc && !content && !format && !qs && !qm, "CryptQueryObject fails cleanly");
}

static void test_secur32(void) {
  WCHAR name[300];
  ULONG size = 0;
  check(!GetUserNameExW(NameSamCompatible, NULL, &size) && GetLastError() == ERROR_MORE_DATA && size > 2, "the size of the SAM name");
  ULONG needed = size;
  check(GetUserNameExW(NameSamCompatible, name, &size) && size == needed - 1 && wcschr(name, L'\\') && wcslen(name) == size, "NameSamCompatible");
  WCHAR user[300];
  DWORD user_size = 300;
  GetUserNameW(user, &user_size);
  check(wcschr(name, L'\\') && !wcscmp(wcschr(name, L'\\') + 1, user), "its user part");
  size = 300;
  check(GetUserNameExW(NameDisplay, name, &size) && !wcscmp(name, user), "NameDisplay");
  size = 300;
  check(!GetUserNameExW(NameFullyQualifiedDN, name, &size) && GetLastError() == ERROR_NONE_MAPPED, "no domain names");
  char narrow[300];
  size = 300;
  check(GetUserNameExA(NameSamCompatible, narrow, &size) && strchr(narrow, '\\') && strlen(narrow) == size, "GetUserNameExA");
  CredHandle credential = {1, 1};
  TimeStamp expiry;
  check(AcquireCredentialsHandleW(NULL, (LPWSTR)L"Negotiate", SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, NULL, &credential, &expiry) == SEC_E_SECPKG_NOT_FOUND &&
        !credential.dwLower && !credential.dwUpper, "no Negotiate");
  PSecPkgInfoW package = (PSecPkgInfoW)1;
  check(QuerySecurityPackageInfoW((LPWSTR)L"NTLM", &package) == SEC_E_SECPKG_NOT_FOUND && !package, "no NTLM");
  check(FreeCredentialsHandle(&credential) == SEC_E_INVALID_HANDLE && DeleteSecurityContext(&credential) == SEC_E_INVALID_HANDLE, "no handles");
  check(FreeContextBuffer(NULL) == SEC_E_OK, "FreeContextBuffer");
}

int main(void) {
  test_dpapi();
  test_memory();
  test_certificate();
  test_system_stores();
  test_secur32();
  printf("crypt32test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

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

/* advapi32 on TheoryCraftOS, the parts beyond the registry basics: hive files, change notification, the CryptoAPI, the event log, event tracing, credentials, services, LSA rights and
 * the audit policy (mingw gcc; see tests/windows/build.sh). Started by the service control manager (the environment names the service) it runs as a service instead.
 * Prints "advapitest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <winsvc.h>
#include <wincrypt.h>
#include <wincred.h>
#include <evntrace.h>
#include <evntprov.h>
#include <evntcons.h>
#include <ntsecapi.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}

/* ---- as a service ---------------------------------------------------------------------------------------------------------------------------------------------- */

static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS g_status;
static HANDLE g_stop_event;
static DWORD WINAPI control_handler(DWORD control, DWORD type, LPVOID data, LPVOID context) {
  (void)type; (void)data; (void)context;
  if (control == SERVICE_CONTROL_STOP) {
    g_status.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_status_handle, &g_status);
    SetEvent(g_stop_event);
  }
  return NO_ERROR;
}
static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
  g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  g_status_handle = RegisterServiceCtrlHandlerExW(argv[0], control_handler, NULL);
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = SERVICE_RUNNING;
  g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  SetServiceStatus(g_status_handle, &g_status);
  /* the first argument beyond the name tells the test that the arguments arrived */
  if (argc > 1 && argv[1][0] == 'x') g_status.dwServiceSpecificExitCode = 7;
  WaitForSingleObject(g_stop_event, 20000);
  g_status.dwCurrentState = SERVICE_STOPPED;
  g_status.dwControlsAccepted = 0;
  SetServiceStatus(g_status_handle, &g_status);
}

static void hex(const BYTE *p, DWORD n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (DWORD i = 0; i < n; ++i) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
  out[2 * n] = 0;
}
static void enable_privilege(LPCWSTR name) {
  HANDLE token;
  TOKEN_PRIVILEGES tp;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
  CloseHandle(token);
}

static void test_registry(void) {
  HKEY base = NULL, a = NULL, b = NULL, c = NULL;
  DWORD v = 5, disposition = 0;
  WCHAR file[] = L"C:\\Windows\\Temp\\advapi.hiv";
  check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AdvapiTest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &base, &disposition) == ERROR_SUCCESS, "create base key");
  check(RegCreateKeyExW(base, L"One", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &a, NULL) == ERROR_SUCCESS, "create One");
  RegSetValueExW(a, L"Number", 0, REG_DWORD, (BYTE *)&v, 4);
  RegSetValueExW(a, L"Text", 0, REG_SZ, (const BYTE *)L"hello", 12);
  RegCreateKeyExW(a, L"Deep", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &b, NULL);
  RegSetValueExW(b, L"Leaf", 0, REG_SZ, (const BYTE *)L"x", 4);
  RegCloseKey(b);
  /* RegCopyTree and RegRenameKey */
  check(RegCreateKeyExW(base, L"Copy", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &c, NULL) == ERROR_SUCCESS, "create Copy");
  check(RegCopyTreeW(a, NULL, c) == ERROR_SUCCESS, "RegCopyTree");
  {
    WCHAR text[16]; DWORD size = sizeof(text);
    check(RegGetValueW(c, L"Deep", L"Leaf", RRF_RT_REG_SZ, NULL, text, &size) == ERROR_SUCCESS && text[0] == 'x', "copied deep value");
  }
  RegCloseKey(c);
  check(RegRenameKey(base, L"Copy", L"Renamed") == ERROR_SUCCESS, "RegRenameKey");
  check(RegOpenKeyExW(base, L"Renamed", 0, KEY_READ, &c) == ERROR_SUCCESS, "open renamed key");
  RegCloseKey(c);
  check(RegOpenKeyExW(base, L"Copy", 0, KEY_READ, &c) == ERROR_FILE_NOT_FOUND, "old name is gone");
  /* several values at once */
  {
    VALENTW list[2] = {{(LPWSTR)L"Number", 0, 0, 0}, {(LPWSTR)L"Text", 0, 0, 0}};
    BYTE buffer[64]; DWORD total = sizeof(buffer);
    check(RegQueryMultipleValuesW(a, list, 2, (LPWSTR)buffer, &total) == ERROR_SUCCESS && list[0].ve_type == REG_DWORD && *(DWORD *)list[0].ve_valueptr == 5, "RegQueryMultipleValues");
  }
  /* change notification: asynchronous with an event */
  {
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(RegNotifyChangeKeyValue(a, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event, TRUE) == ERROR_SUCCESS, "RegNotifyChangeKeyValue");
    check(WaitForSingleObject(event, 50) == WAIT_TIMEOUT, "no change yet");
    v = 6;
    RegSetValueExW(a, L"Number", 0, REG_DWORD, (BYTE *)&v, 4);
    check(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "the change signalled the event");
    CloseHandle(event);
  }
  /* hive files: they need the backup and restore privileges */
  enable_privilege(L"SeBackupPrivilege");
  enable_privilege(L"SeRestorePrivilege");
  CreateDirectoryW(L"C:\\Windows\\Temp", NULL);
  DeleteFileW(file);
  {
    LSTATUS r = RegSaveKeyW(a, file, NULL);
    if (r == ERROR_PRIVILEGE_NOT_HELD) check(1, "RegSaveKey refuses without the privilege");
    else {
      check(r == ERROR_SUCCESS, "RegSaveKey");
      check(RegSaveKeyW(a, file, NULL) == ERROR_ALREADY_EXISTS, "RegSaveKey over a file");
      v = 99;
      RegSetValueExW(a, L"Number", 0, REG_DWORD, (BYTE *)&v, 4);
      check(RegRestoreKeyW(a, file, REG_FORCE_RESTORE) == ERROR_SUCCESS, "RegRestoreKey");
      { DWORD got = 0, size = 4; RegQueryValueExW(a, L"Number", NULL, NULL, (BYTE *)&got, &size); check(got == 6, "the saved value is back"); }
      check(RegLoadKeyW(HKEY_USERS, L"AdvapiHive", file) == ERROR_SUCCESS, "RegLoadKey");
      check(RegOpenKeyExW(HKEY_USERS, L"AdvapiHive\\Deep", 0, KEY_READ, &c) == ERROR_SUCCESS, "the loaded hive has the subkey");
      RegCloseKey(c);
      check(RegUnLoadKeyW(HKEY_USERS, L"AdvapiHive") == ERROR_SUCCESS, "RegUnLoadKey");
      check(RegOpenKeyExW(HKEY_USERS, L"AdvapiHive", 0, KEY_READ, &c) == ERROR_FILE_NOT_FOUND, "the hive is gone");
    }
  }
  RegCloseKey(a);
  check(RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\AdvapiTest") == ERROR_SUCCESS, "delete the tree");
  RegCloseKey(base);
}

static void test_crypto(void) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  HCRYPTKEY key = 0;
  BYTE digest[64];
  DWORD size;
  char text[200];
  check(CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT), "CryptAcquireContext");
  static const struct { ALG_ID alg; const char *want; } hashes[] = {
      {CALG_MD5, "900150983cd24fb0d6963f7d28e17f72"}, {CALG_SHA1, "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {CALG_SHA_256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
  for (int i = 0; i < 3; ++i) {
    size = sizeof(digest);
    check(CryptCreateHash(prov, hashes[i].alg, 0, 0, &hash) && CryptHashData(hash, (const BYTE *)"abc", 3, 0) && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0), "hash abc");
    hex(digest, size, text);
    check(!strcmp(text, hashes[i].want), "hash value");
    CryptDestroyHash(hash);
  }
  /* AES-128-CBC and RC4 round trips through a key derived from a hash */
  {
    HCRYPTHASH base = 0;
    BYTE data[48] = "the quick brown fox jumps over the lazy dog!";
    DWORD length = 44;
    CryptCreateHash(prov, CALG_SHA_256, 0, 0, &base);
    CryptHashData(base, (const BYTE *)"password", 8, 0);
    check(CryptDeriveKey(prov, CALG_AES_128, base, 128u << 16, &key), "CryptDeriveKey");
    check(CryptEncrypt(key, 0, TRUE, 0, data, &length, sizeof(data)) && length == 48, "CryptEncrypt (padded)");
    check(memcmp(data, "the quick", 9) != 0, "it is encrypted");
    check(CryptDecrypt(key, 0, TRUE, 0, data, &length) && length == 44 && !memcmp(data, "the quick brown", 15), "CryptDecrypt");
    CryptDestroyKey(key);
    CryptDestroyHash(base);
  }
  {
    BYTE a[16], b[16];
    check(CryptGenRandom(prov, 16, a) && CryptGenRandom(prov, 16, b) && memcmp(a, b, 16) != 0, "CryptGenRandom gives different bytes");
  }
  /* HMAC-SHA1 (RFC 2202 test 2: key "Jefe", data "what do ya want for nothing?") */
  {
    HCRYPTKEY hkey = 0;
    struct { BLOBHEADER h; DWORD n; BYTE k[4]; } blob = {{PLAINTEXTKEYBLOB, CUR_BLOB_VERSION, 0, CALG_RC4}, 4, {'J', 'e', 'f', 'e'}};
    HMAC_INFO info = {CALG_SHA1, NULL, 0, NULL, 0};
    size = sizeof(digest);
    check(CryptImportKey(prov, (BYTE *)&blob, sizeof(blob), 0, 0, &hkey), "import a plain key");
    check(CryptCreateHash(prov, CALG_HMAC, hkey, 0, &hash) && CryptSetHashParam(hash, HP_HMAC_INFO, (BYTE *)&info, 0) &&
              CryptHashData(hash, (const BYTE *)"what do ya want for nothing?", 28, 0) && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0), "HMAC");
    hex(digest, size, text);
    check(!strcmp(text, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "HMAC-SHA1 value");
    CryptDestroyHash(hash);
    CryptDestroyKey(hkey);
  }
  CryptReleaseContext(prov, 0);
}

static void test_eventlog(void) {
  HANDLE source = RegisterEventSourceW(NULL, L"AdvapiTest"), log;
  LPCWSTR strings[2] = {L"first", L"second"};
  check(source != NULL, "RegisterEventSource");
  check(ReportEventW(source, EVENTLOG_WARNING_TYPE, 3, 0x1234, NULL, 2, 4, strings, "data"), "ReportEvent");
  DeregisterEventSource(source);
  log = OpenEventLogW(NULL, L"Application");
  check(log != NULL, "OpenEventLog");
  {
    DWORD count = 0, oldest = 0, read = 0, needed = 0;
    BYTE buffer[1024];
    check(GetNumberOfEventLogRecords(log, &count) && count >= 1, "GetNumberOfEventLogRecords");
    check(GetOldestEventLogRecord(log, &oldest) && oldest >= 1, "GetOldestEventLogRecord");
    check(ReadEventLogW(log, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, buffer, sizeof(buffer), &read, &needed) && read > 0, "ReadEventLog");
    {
      EVENTLOGRECORD *r = (EVENTLOGRECORD *)buffer;
      check(r->EventID == 0x1234 && r->NumStrings == 2 && r->EventType == EVENTLOG_WARNING_TYPE && r->DataLength == 4, "the record");
      check(!wcscmp((WCHAR *)(buffer + sizeof(EVENTLOGRECORD)), L"AdvapiTest"), "its source");
      check(!wcscmp((WCHAR *)(buffer + r->StringOffset), L"first"), "its strings");
    }
  }
  check(ClearEventLogW(log, NULL), "ClearEventLog");
  { DWORD count = 5; GetNumberOfEventLogRecords(log, &count); check(count == 0, "the log is empty"); }
  CloseEventLog(log);
}

static int g_events_seen;
static VOID WINAPI on_record(PEVENT_RECORD r) { if (r->UserDataLength == 4 && !memcmp(r->UserData, "ping", 4)) ++g_events_seen; }
static int g_enable_calls;
static VOID NTAPI on_enable(LPCGUID source, ULONG enabled, UCHAR level, ULONGLONG any, ULONGLONG all, PEVENT_FILTER_DESCRIPTOR filter, PVOID context) {
  (void)source; (void)level; (void)any; (void)all; (void)filter; (void)context;
  g_enable_calls += enabled ? 1 : 0;
}
static void test_etw(void) {
  static const GUID provider_guid = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  static const GUID session_guid = {0x87654321, 0x4321, 0x4321, {8, 7, 6, 5, 4, 3, 2, 1}};
  char buffer[sizeof(EVENT_TRACE_PROPERTIES) + 512];
  EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)buffer;
  TRACEHANDLE session = 0;
  REGHANDLE reg = 0;
  memset(buffer, 0, sizeof(buffer));
  props->Wnode.BufferSize = sizeof(buffer);
  props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  props->Wnode.Guid = session_guid;
  props->Wnode.ClientContext = 1;
  props->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
  props->LogFileNameOffset = sizeof(*props) + 40;
  wcscpy((WCHAR *)(buffer + props->LogFileNameOffset), L"C:\\Windows\\Temp\\advapi.etl");
  CreateDirectoryW(L"C:\\Windows\\Temp", NULL);
  DeleteFileW(L"C:\\Windows\\Temp\\advapi.etl");
  check(EventRegister(&provider_guid, on_enable, NULL, &reg) == ERROR_SUCCESS, "EventRegister");
  check(!EventProviderEnabled(reg, 4, 0), "not enabled before a session");
  check(StartTraceW(&session, L"AdvapiSession", props) == ERROR_SUCCESS, "StartTrace");
  check(StartTraceW(&session, L"AdvapiSession", props) == ERROR_ALREADY_EXISTS, "StartTrace twice");
  check(EnableTraceEx2(session, &provider_guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, 5, 0, 0, 0, NULL) == ERROR_SUCCESS, "EnableTraceEx2");
  for (int i = 0; i < 40 && !g_enable_calls; ++i) Sleep(50);
  check(g_enable_calls >= 1, "the provider heard of it");
  check(EventProviderEnabled(reg, 4, 0), "now enabled");
  {
    EVENT_DESCRIPTOR d = {1, 0, 0, 4, 0, 0, 0};
    EVENT_DATA_DESCRIPTOR data;
    EventDataDescCreate(&data, "ping", 4);
    check(EventWrite(reg, &d, 1, &data) == ERROR_SUCCESS, "EventWrite");
    check(EventWrite(reg, &d, 1, &data) == ERROR_SUCCESS, "EventWrite again");
  }
  {
    EVENT_TRACE_PROPERTIES *query = (EVENT_TRACE_PROPERTIES *)buffer;
    check(QueryTraceW(session, NULL, query) == ERROR_SUCCESS, "QueryTrace");
  }
  check(StopTraceW(session, NULL, props) == ERROR_SUCCESS, "StopTrace");
  EventUnregister(reg);
  {
    EVENT_TRACE_LOGFILEW log;
    TRACEHANDLE consumer;
    memset(&log, 0, sizeof(log));
    log.LogFileName = L"C:\\Windows\\Temp\\advapi.etl";
    log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = on_record;
    consumer = OpenTraceW(&log);
    check(consumer != (TRACEHANDLE)INVALID_HANDLE_VALUE, "OpenTrace");
    check(ProcessTrace(&consumer, 1, NULL, NULL) == ERROR_SUCCESS, "ProcessTrace");
    check(g_events_seen == 2, "the consumer saw both events");
  }
}

static void test_credentials(void) {
  CREDENTIALW c, *read = NULL, **list = NULL;
  DWORD count = 0;
  BYTE secret[4] = {1, 2, 3, 4};
  memset(&c, 0, sizeof(c));
  c.Type = CRED_TYPE_GENERIC;
  c.TargetName = (LPWSTR)L"AdvapiTest/target";
  c.UserName = (LPWSTR)L"someone";
  c.CredentialBlob = secret;
  c.CredentialBlobSize = 4;
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;
  check(CredWriteW(&c, 0), "CredWrite");
  check(CredReadW(L"ADVAPITEST/TARGET", CRED_TYPE_GENERIC, 0, &read) && read->CredentialBlobSize == 4 && !memcmp(read->CredentialBlob, secret, 4) && !wcscmp(read->UserName, L"someone"), "CredRead");
  CredFree(read);
  check(CredEnumerateW(L"AdvapiTest*", 0, &count, &list) && count == 1, "CredEnumerate");
  CredFree(list);
  check(CredDeleteW(L"AdvapiTest/target", CRED_TYPE_GENERIC, 0), "CredDelete");
  check(!CredReadW(L"AdvapiTest/target", CRED_TYPE_GENERIC, 0, &read) && GetLastError() == ERROR_NOT_FOUND, "gone");
}

static void test_services(WCHAR *self) {
  SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS), svc;
  SERVICE_STATUS_PROCESS st;
  DWORD needed = 0, service_pid = 0;
  WCHAR path[300];
  check(scm != NULL, "OpenSCManager");
  wcscpy(path, L"\"");
  wcscat(path, self);
  wcscat(path, L"\"");
  svc = CreateServiceW(scm, L"AdvapiSvc", L"Advapi test service", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);
  check(svc != NULL, "CreateService");
  check(CreateServiceW(scm, L"AdvapiSvc", NULL, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL) == NULL &&
            GetLastError() == ERROR_SERVICE_EXISTS, "CreateService twice");
  {
    SERVICE_DESCRIPTIONW d = {(LPWSTR)L"for the test"};
    BYTE buffer[256];
    check(ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d), "ChangeServiceConfig2");
    check(QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, buffer, sizeof(buffer), &needed) && !wcscmp(((SERVICE_DESCRIPTIONW *)buffer)->lpDescription, L"for the test"), "QueryServiceConfig2");
  }
  {
    BYTE buffer[512];
    QUERY_SERVICE_CONFIGW *cfg = (QUERY_SERVICE_CONFIGW *)buffer;
    check(QueryServiceConfigW(svc, cfg, sizeof(buffer), &needed) && cfg->dwStartType == SERVICE_DEMAND_START && !wcscmp(cfg->lpDisplayName, L"Advapi test service"), "QueryServiceConfig");
  }
  check(QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &needed) && st.dwCurrentState == SERVICE_STOPPED, "stopped at first");
  {
    LPCWSTR args[1] = {L"x"};
    check(StartServiceW(svc, 1, args), "StartService");
  }
  check(QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &needed) && st.dwCurrentState == SERVICE_RUNNING && st.dwProcessId != 0, "running");
  service_pid = st.dwProcessId;
  check(!StartServiceW(svc, 0, NULL) && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING, "start while running");
  {
    BYTE buffer[4096];
    DWORD returned = 0, resume = 0;
    check(EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buffer, sizeof(buffer), &needed, &returned, &resume, NULL) && returned >= 1, "EnumServicesStatusEx lists it");
  }
  {
    SERVICE_STATUS ss;
    check(ControlService(svc, SERVICE_CONTROL_STOP, &ss), "ControlService stop");
  }
  for (int i = 0; i < 100; ++i) { QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &needed); if (st.dwCurrentState == SERVICE_STOPPED) break; Sleep(50); }
  check(st.dwCurrentState == SERVICE_STOPPED, "stopped");
  {      /* the process itself goes too */
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, service_pid);
    if (process) { check(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0, "the service process ended"); CloseHandle(process); }
  }
  check(DeleteService(svc), "DeleteService");
  CloseServiceHandle(svc);
  check(OpenServiceW(scm, L"AdvapiSvc", SERVICE_QUERY_STATUS) == NULL && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST, "the service is gone");
  CloseServiceHandle(scm);
}

static void test_lsa_and_audit(void) {
  LSA_OBJECT_ATTRIBUTES attrs;
  LSA_HANDLE policy = NULL;
  BYTE sid_buffer[SECURITY_MAX_SID_SIZE];
  DWORD sid_size = sizeof(sid_buffer);
  LSA_UNICODE_STRING right;
  PLSA_UNICODE_STRING rights = NULL;
  ULONG count = 0;
  memset(&attrs, 0, sizeof(attrs));
  check(LsaOpenPolicy(NULL, &attrs, POLICY_ALL_ACCESS, &policy) == 0, "LsaOpenPolicy");
  check(CreateWellKnownSid(WinBuiltinUsersSid, NULL, sid_buffer, &sid_size), "a SID");
  right.Buffer = (PWSTR)L"SeBatchLogonRight";
  right.Length = 17 * 2;
  right.MaximumLength = 18 * 2;
  check(LsaAddAccountRights(policy, sid_buffer, &right, 1) == 0, "LsaAddAccountRights");
  check(LsaEnumerateAccountRights(policy, sid_buffer, &rights, &count) == 0 && count == 1 && !wcsncmp(rights[0].Buffer, L"SeBatchLogonRight", 17), "LsaEnumerateAccountRights");
  LsaFreeMemory(rights);
  {
    PVOID accounts = NULL;
    count = 0;
    check(LsaEnumerateAccountsWithUserRight(policy, &right, &accounts, &count) == 0 && count == 1, "accounts with the right");
    LsaFreeMemory(accounts);
  }
  check(LsaRemoveAccountRights(policy, sid_buffer, FALSE, &right, 1) == 0, "LsaRemoveAccountRights");
  {
    GUID *categories = NULL;
    ULONG n = 0;
    check(AuditEnumerateCategories(&categories, &n) && n == 9, "AuditEnumerateCategories");
    {
      LPWSTR name = NULL;
      check(AuditLookupCategoryNameW(&categories[0], &name) && !wcscmp(name, L"System"), "AuditLookupCategoryName");
      AuditFree(name);
    }
    AuditFree(categories);
  }
  LsaClose(policy);
}

int main(void) {
  WCHAR self[300];
  GetModuleFileNameW(NULL, self, 300);
  if (GetEnvironmentVariableW(L"TCOS_SERVICE_NAME", NULL, 0) != 0) {
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)L"AdvapiSvc", service_main}, {NULL, NULL}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
  }
  test_registry();
  test_crypto();
  test_eventlog();
  test_etw();
  test_credentials();
  test_services(self);
  test_lsa_and_audit();
  printf("advapitest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}

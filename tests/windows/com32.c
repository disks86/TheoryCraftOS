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

/* COM between processes (docs/WindowsCom.md), for tests/wow64_qemu.sh (as com32, a 32-bit program) and built for x86-64 as well (com64). One program in three
 * roles:
 *
 *   com32.exe              the client. It registers the classes, the interfaces and the type library (which is a resource of this file) in the registry, and a
 *                          service; creates an object of each class with CLSCTX_LOCAL_SERVER - which starts the other two - and calls it.
 *   com32.exe -Embedding   the local server of the first class: it registers its class object and waits until a client tells it to quit.
 *   com32.exe service      the service that serves the second class, started by the service control manager because the class's AppID names it.
 *
 * The interfaces are com32.idl's. Ends with exit code 42 if every check passed.
 *   widl -m32 -I /usr/include/wine/windows -t -T com32.tlb -h -H com32_h.h -u -U com32_i.c tests/windows/com32.idl
 *   echo '1 TYPELIB "com32.tlb"' | i686-w64-mingw32-windres -o com32res.o
 *   i686-w64-mingw32-gcc -O1 -I . -o com32.exe tests/windows/com32.c com32_i.c com32res.o -lole32 -loleaut32 -ladvapi32 -luuid */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <winsvc.h>
#include "com32_h.h"

#ifdef _WIN64
#define NAME "com64"
#else
#define NAME "com32"
#endif
#define SERVICE_NAME L"TcosComTestService"

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? NAME ": ok: %s\n" : NAME ": FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

/* ---- the object the servers hand out ---------------------------------------------------------------------------------------------------------------------- */

static HANDLE g_quit;
static LONG g_calls, g_objects;
static WCHAR g_arguments[256];      /* what the service was started with */

typedef struct Test { ITcosTest2 iface; LONG refs; } Test;
static HRESULT make_test(REFIID iid, void **out);

static HRESULT WINAPI test_qi(ITcosTest2 *me, REFIID iid, void **out) {
  *out = NULL;
  if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_ITcosTest) && !IsEqualGUID(iid, &IID_ITcosTest2)) return E_NOINTERFACE;
  *out = me;
  ITcosTest2_AddRef(me);
  return S_OK;
}
static ULONG WINAPI test_addref(ITcosTest2 *me) { return (ULONG)InterlockedIncrement(&((Test *)me)->refs); }
static ULONG WINAPI test_release(ITcosTest2 *me) {
  const LONG refs = InterlockedDecrement(&((Test *)me)->refs);
  if (refs == 0) { InterlockedDecrement(&g_objects); HeapFree(GetProcessHeap(), 0, me); }
  return (ULONG)refs;
}
static HRESULT WINAPI test_add(ITcosTest2 *me, LONG a, LONG b, LONG *sum) { (void)me; InterlockedIncrement(&g_calls); *sum = a + b; return S_OK; }
static HRESULT WINAPI test_echo(ITcosTest2 *me, BSTR text, BSTR *back) {
  (void)me;
  InterlockedIncrement(&g_calls);
  const UINT length = SysStringLen(text);
  *back = SysAllocStringLen(NULL, length * 2);
  if (!*back) return E_OUTOFMEMORY;
  for (UINT i = 0; i < length; i++) (*back)[i] = (*back)[length + i] = text[i];      /* (the text, twice) */
  return S_OK;
}
static HRESULT WINAPI test_big(ITcosTest2 *me, LONGLONG value, LONG shift, LONGLONG *shifted) { (void)me; InterlockedIncrement(&g_calls); *shifted = value << shift; return S_OK; }
static HRESULT WINAPI test_call_back(ITcosTest2 *me, ITcosCallback *callback, LONG times) {
  (void)me;
  InterlockedIncrement(&g_calls);
  if (!callback) return E_POINTER;
  for (LONG i = 0; i < times; i++) {
    BSTR text = SysAllocString(L"from the server");
    const HRESULT hr = ITcosCallback_Notify(callback, i + 1, text);
    SysFreeString(text);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}
static HRESULT WINAPI test_make(ITcosTest2 *me, ITcosTest **another) { (void)me; InterlockedIncrement(&g_calls); return make_test(&IID_ITcosTest, (void **)another); }
static HRESULT WINAPI test_turn(ITcosTest2 *me, VARIANT value, VARIANT *back) {
  (void)me;
  InterlockedIncrement(&g_calls);
  VariantInit(back);
  if (V_VT(&value) == VT_I4) { V_VT(back) = VT_BSTR; V_BSTR(back) = SysAllocString(V_I4(&value) == 7 ? L"seven" : L"a number"); return S_OK; }
  if (V_VT(&value) == VT_BSTR) { V_VT(back) = VT_I4; V_I4(back) = (LONG)SysStringLen(V_BSTR(&value)); return S_OK; }
  if (V_VT(&value) == VT_EMPTY) { V_VT(back) = VT_BOOL; V_BOOL(back) = VARIANT_TRUE; return S_OK; }
  return DISP_E_TYPEMISMATCH;
}
static HRESULT WINAPI test_where(ITcosTest2 *me, LONG *process, BSTR *arguments) {
  (void)me;
  InterlockedIncrement(&g_calls);
  if (process) *process = (LONG)GetCurrentProcessId();
  if (arguments) *arguments = SysAllocString(g_arguments);
  return S_OK;
}
static HRESULT WINAPI test_fail(ITcosTest2 *me, LONG code) { (void)me; InterlockedIncrement(&g_calls); return (HRESULT)code; }
static HRESULT WINAPI test_quit(ITcosTest2 *me) { (void)me; SetEvent(g_quit); return S_OK; }
static HRESULT WINAPI test_count(ITcosTest2 *me, LONG *calls) { (void)me; *calls = InterlockedIncrement(&g_calls); return S_OK; }
static HRESULT WINAPI test_length(ITcosTest2 *me, LPWSTR text, LPWSTR none, LONG *length, LPWSTR *upper) {
  (void)me;
  if (!text || none) return E_INVALIDARG;
  *length = (LONG)wcslen(text);
  *upper = CoTaskMemAlloc((wcslen(text) + 1) * sizeof(WCHAR));
  if (!*upper) return E_OUTOFMEMORY;
  for (size_t i = 0; i <= wcslen(text); i++) (*upper)[i] = text[i] >= 'a' && text[i] <= 'z' ? (WCHAR)(text[i] - 32) : text[i];
  return S_OK;
}
static const ITcosTest2Vtbl g_test_vtbl = {test_qi, test_addref, test_release, test_add, test_echo, test_big, test_call_back, test_make, test_turn, test_where, test_fail, test_quit, test_count, test_length};

static HRESULT make_test(REFIID iid, void **out) {
  Test *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
  *out = NULL;
  if (!t) return E_OUTOFMEMORY;
  t->iface.lpVtbl = (ITcosTest2Vtbl *)&g_test_vtbl;
  t->refs = 1;
  InterlockedIncrement(&g_objects);
  const HRESULT hr = ITcosTest2_QueryInterface(&t->iface, iid, out);
  ITcosTest2_Release(&t->iface);
  return hr;
}

static HRESULT WINAPI factory_qi(IClassFactory *me, REFIID iid, void **out) {
  *out = NULL;
  if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IClassFactory)) return E_NOINTERFACE;
  *out = me;
  return S_OK;
}
static ULONG WINAPI factory_addref(IClassFactory *me) { (void)me; return 2; }
static ULONG WINAPI factory_release(IClassFactory *me) { (void)me; return 1; }
static HRESULT WINAPI factory_create(IClassFactory *me, IUnknown *outer, REFIID iid, void **out) { (void)me; return outer ? CLASS_E_NOAGGREGATION : make_test(iid, out); }
static HRESULT WINAPI factory_lock(IClassFactory *me, BOOL lock) { (void)me; (void)lock; return S_OK; }
static const IClassFactoryVtbl g_factory_vtbl = {factory_qi, factory_addref, factory_release, factory_create, factory_lock};
static IClassFactory g_factory = {(IClassFactoryVtbl *)&g_factory_vtbl};

/* Serves `clsid` until a client says to quit. */
static int serve(REFCLSID clsid) {
  DWORD cookie = 0;
  g_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 3;
  /* (held back and then resumed, as a server with several classes does) */
  if (FAILED(CoRegisterClassObject(clsid, (IUnknown *)&g_factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &cookie)) || FAILED(CoResumeClassObjects())) return 4;
  WaitForSingleObject(g_quit, 60000);
  Sleep(200);      /* (the call that said so gets its answer) */
  CoRevokeClassObject(cookie);
  CoUninitialize();
  return 0;
}

/* ---- the service ---------------------------------------------------------------------------------------------------------------------------------------------- */

static SERVICE_STATUS_HANDLE g_status_handle;
static void report(DWORD state) {
  SERVICE_STATUS status;
  memset(&status, 0, sizeof(status));
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  SetServiceStatus(g_status_handle, &status);
}
static DWORD WINAPI service_control(DWORD control, DWORD type, LPVOID data, LPVOID context) {
  (void)type; (void)data; (void)context;
  if (control == SERVICE_CONTROL_STOP && g_quit) SetEvent(g_quit);
  return NO_ERROR;
}
static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
  g_status_handle = RegisterServiceCtrlHandlerExW(L"", service_control, NULL);      /* (a service with a process of its own need not say its name: Chromium's do not) */
  for (DWORD i = 1; i < argc; i++) {      /* (the class's ServiceParameters arrive as the service's arguments) */
    if (i > 1) wcscat(g_arguments, L" ");
    wcsncat(g_arguments, argv[i], 60);
  }
  report(SERVICE_RUNNING);
  serve(&CLSID_TcosServiceTest);
  report(SERVICE_STOPPED);
}

/* ---- the client ----------------------------------------------------------------------------------------------------------------------------------------------- */

static LONG g_notified, g_notify_sum, g_notify_thread_other;
static DWORD g_main_thread;
static HRESULT WINAPI callback_qi(ITcosCallback *me, REFIID iid, void **out) {
  *out = NULL;
  if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_ITcosCallback)) return E_NOINTERFACE;
  *out = me;
  return S_OK;
}
static ULONG WINAPI callback_addref(ITcosCallback *me) { (void)me; return 2; }
static ULONG WINAPI callback_release(ITcosCallback *me) { (void)me; return 1; }
static HRESULT WINAPI callback_notify(ITcosCallback *me, LONG value, BSTR text) {
  (void)me;
  InterlockedIncrement(&g_notified);
  InterlockedExchangeAdd(&g_notify_sum, value);
  if (GetCurrentThreadId() != g_main_thread) InterlockedIncrement(&g_notify_thread_other);
  return text && wcscmp(text, L"from the server") == 0 ? S_OK : E_INVALIDARG;
}
static const ITcosCallbackVtbl g_callback_vtbl = {callback_qi, callback_addref, callback_release, callback_notify};
static ITcosCallback g_callback = {(ITcosCallbackVtbl *)&g_callback_vtbl};

static BOOL set_key(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value) {
  HKEY key;
  if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return FALSE;
  const LONG r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

/* What an installer of these classes would write: where the servers are, and how the interfaces travel. */
static BOOL register_everything(const WCHAR *self) {
  WCHAR path[200], value[MAX_PATH + 40], clsid[40], service_clsid[40], library[40], appid[] = L"{7C051A12-5C0D-4E57-9A52-544353434F01}";
  static const GUID *const kInterfaces[] = {&IID_ITcosCallback, &IID_ITcosTest, &IID_ITcosTest2};
  BOOL ok = TRUE;
  StringFromGUID2(&CLSID_TcosTest, clsid, 40);
  StringFromGUID2(&CLSID_TcosServiceTest, service_clsid, 40);
  StringFromGUID2(&LIBID_TcosComTest, library, 40);
  swprintf(path, 200, L"Software\\Classes\\CLSID\\%ls\\LocalServer32", clsid);
  swprintf(value, MAX_PATH + 40, L"\"%ls\" -Embedding", self);
  ok &= set_key(HKEY_LOCAL_MACHINE, path, NULL, value);
  swprintf(path, 200, L"Software\\Classes\\CLSID\\%ls", service_clsid);
  ok &= set_key(HKEY_LOCAL_MACHINE, path, L"AppID", appid);
  swprintf(path, 200, L"Software\\Classes\\AppID\\%ls", appid);
  ok &= set_key(HKEY_LOCAL_MACHINE, path, L"LocalService", SERVICE_NAME) && set_key(HKEY_LOCAL_MACHINE, path, L"ServiceParameters", L"--from-com second");
  for (int i = 0; i < 3; i++) {
    WCHAR iid[40];
    StringFromGUID2(kInterfaces[i], iid, 40);
    swprintf(path, 200, L"Software\\Classes\\Interface\\%ls\\ProxyStubClsid32", iid);
    ok &= set_key(HKEY_LOCAL_MACHINE, path, NULL, L"{00020424-0000-0000-C000-000000000046}");
    swprintf(path, 200, L"Software\\Classes\\Interface\\%ls\\TypeLib", iid);
    ok &= set_key(HKEY_LOCAL_MACHINE, path, NULL, library) && set_key(HKEY_LOCAL_MACHINE, path, L"Version", L"1.0");
  }
  swprintf(path, 200, L"Software\\Classes\\TypeLib\\%ls\\1.0\\0\\win%d", library, (int)sizeof(void *) * 8);
  ok &= set_key(HKEY_LOCAL_MACHINE, path, NULL, self);
  /* The service. */
  SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
  swprintf(value, MAX_PATH + 40, L"\"%ls\" service", self);
  SC_HANDLE service = manager ? CreateServiceW(manager, SERVICE_NAME, SERVICE_NAME, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, value, NULL, NULL,
                                               NULL, NULL, NULL) : NULL;
  ok &= service != NULL || GetLastError() == ERROR_SERVICE_EXISTS;
  if (service) CloseServiceHandle(service);
  if (manager) CloseServiceHandle(manager);
  return ok;
}

static void test_object(REFCLSID clsid, const char *which, BOOL service) {
  char what[160];
  ITcosTest *test = NULL;
  ITcosTest2 *second = NULL;
  IUnknown *unknown = NULL, *again = NULL;
  LONG sum = 0, process = 0, count = 0;
  LONGLONG shifted = 0;
  BSTR text = SysAllocString(L"abé"), back = NULL, arguments = NULL;
  VARIANT value, turned;

  HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&unknown);
  sprintf(what, "%s: CoCreateInstance(CLSCTX_LOCAL_SERVER) starts the server and gives an object (0x%08lx)", which, (unsigned long)hr);
  check(SUCCEEDED(hr) && unknown != NULL, what);
  if (!unknown) return;
  hr = IUnknown_QueryInterface(unknown, &IID_ITcosTest, (void **)&test);
  sprintf(what, "%s: QueryInterface for an interface of the type library (0x%08lx)", which, (unsigned long)hr);
  check(SUCCEEDED(hr) && test != NULL, what);
  check(IUnknown_QueryInterface(unknown, &IID_IPersist, (void **)&again) == E_NOINTERFACE && again == NULL, "an interface the object does not have is refused");
  if (!test) { IUnknown_Release(unknown); return; }
  check(SUCCEEDED(ITcosTest_QueryInterface(test, &IID_IUnknown, (void **)&again)) && again == unknown, "the object's IUnknown is the same pointer however it is asked for");
  if (again) IUnknown_Release(again);

  check(ITcosTest_Add(test, 40, 2, &sum) == S_OK && sum == 42, "a call with numbers and a result");
  check(ITcosTest_Echo(test, text, &back) == S_OK && back && SysStringLen(back) == 6 && wcscmp(back, L"abéabé") == 0, "a string there and a string back");
  SysFreeString(back);
  check(ITcosTest_Big(test, 0x123456789LL, 8, &shifted) == S_OK && shifted == 0x12345678900LL, "64-bit numbers");
  check(ITcosTest_Where(test, &process, &arguments) == S_OK && process != 0 && (DWORD)process != GetCurrentProcessId(), "the object lives in another process");
  if (service) check(arguments && wcscmp(arguments, L"--from-com second") == 0, "the service got the class's ServiceParameters as its arguments");
  SysFreeString(arguments);
  check(ITcosTest_Where(test, NULL, NULL) == S_OK, "results nobody asked for are not sent");
  check(ITcosTest_Fail(test, (LONG)0x80070005) == (HRESULT)0x80070005 && ITcosTest_Fail(test, 1) == S_FALSE, "a method's own result comes back as it is");

  g_notified = g_notify_sum = g_notify_thread_other = 0;
  check(ITcosTest_CallBack(test, &g_callback, 5) == S_OK && g_notified == 5 && g_notify_sum == 15 && g_notify_thread_other == 5,
        "an interface passed along is called back, five times, while the call waits");
  check(ITcosTest_CallBack(test, NULL, 1) == E_POINTER, "a null interface arrives as null");

  VariantInit(&value);
  VariantInit(&turned);
  V_VT(&value) = VT_I4;
  V_I4(&value) = 7;
  check(ITcosTest_Turn(test, value, &turned) == S_OK && V_VT(&turned) == VT_BSTR && wcscmp(V_BSTR(&turned), L"seven") == 0, "a VARIANT with a number, one with a string back");
  VariantClear(&turned);
  V_VT(&value) = VT_BSTR;
  V_BSTR(&value) = text;
  check(ITcosTest_Turn(test, value, &turned) == S_OK && V_VT(&turned) == VT_I4 && V_I4(&turned) == 3, "and the other way");
  V_VT(&value) = VT_EMPTY;
  check(ITcosTest_Turn(test, value, &turned) == S_OK && V_VT(&turned) == VT_BOOL && V_BOOL(&turned) == VARIANT_TRUE, "an empty VARIANT");

  /* An object made by a call, and the interface derived from the first one. */
  ITcosTest *another = NULL;
  check(ITcosTest_Make(test, &another) == S_OK && another != NULL && another != test, "a method gives another object");
  if (another) {
    check(ITcosTest_Add(another, 1, 2, &sum) == S_OK && sum == 3, "which is called like the first");
    check(SUCCEEDED(ITcosTest_QueryInterface(another, &IID_ITcosTest2, (void **)&second)) && second != NULL, "the derived interface");
    if (second) {
      check(ITcosTest2_Add(second, 5, 6, &sum) == S_OK && sum == 11 && ITcosTest2_Count(second, &count) == S_OK && count > 10, "its own method, after the inherited ones");
      WCHAR *upper = NULL;
      LONG length = 0;
      hr = ITcosTest2_Length(second, (LPWSTR)L"plain text", NULL, &length, &upper);
      sprintf(what, "text that ends in a zero (not a BSTR), a null one, and one back in task memory (0x%08lx, %ld)", (unsigned long)hr, (long)length);
      check(hr == S_OK && length == 10 && upper && wcscmp(upper, L"PLAIN TEXT") == 0, what);
      CoTaskMemFree(upper);
      ITcosTest2_Release(second);
    }
    ITcosTest_Release(another);
  }
  /* Calls from several threads at once. */
  {
    struct Job { ITcosTest *test; LONG bad; } job = {test, 0};
    HANDLE threads[4];
    DWORD WINAPI worker(LPVOID parameter);
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, worker, &job, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, 30000);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
    check(job.bad == 0, "four threads call at once, fifty times each");
  }
  check(ITcosTest_Quit(test) == S_OK, "the server is told to end");
  ITcosTest_Release(test);
  IUnknown_Release(unknown);
  SysFreeString(text);
}
DWORD WINAPI worker(LPVOID parameter) {
  struct Job { ITcosTest *test; LONG bad; } *job = parameter;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  for (LONG i = 0; i < 50; i++) {
    LONG sum = 0;
    if (ITcosTest_Add(job->test, i, (LONG)GetCurrentThreadId(), &sum) != S_OK || sum != i + (LONG)GetCurrentThreadId()) InterlockedIncrement(&job->bad);
  }
  CoUninitialize();
  return 0;
}

int wmain(int argc, WCHAR **argv);
int main(void) {
  int argc = 0;
  WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  return wmain(argc, argv);
}
int wmain(int argc, WCHAR **argv) {
  WCHAR self[MAX_PATH];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  if (argc >= 2 && wcscmp(argv[1], L"-Embedding") == 0) return serve(&CLSID_TcosTest);
  if (argc >= 2 && wcscmp(argv[1], L"service") == 0) {
    const SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)SERVICE_NAME, service_main}, {NULL, NULL}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 5;
  }
  g_main_thread = GetCurrentThreadId();
  check(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)), "CoInitializeEx");
  check(register_everything(self), "the classes, the interfaces, the type library and the service are registered");
  IUnknown *nothing = NULL;
  static const GUID kNobody = {0x7C051AFF, 0x5C0D, 0x4E57, {0x9A, 0x52, 0x54, 0x43, 0x53, 0x43, 0x4F, 0x01}};
  check(CoCreateInstance(&kNobody, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&nothing) == REGDB_E_CLASSNOTREG, "a class nobody serves is not registered");
  test_object(&CLSID_TcosTest, "a local server", FALSE);
  test_object(&CLSID_TcosServiceTest, "a service", TRUE);
  CoUninitialize();
  printf(g_failed == 0 ? NAME ": all %d checks passed\n" : NAME ": %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}

/* Tests of the libraries a game engine (Godot, SDL) loads at start-up, which TheoryCraftOS answers for a machine with no game controller and no audio device behind them:
 * winmm (the multimedia timer, joysticks, wave/MIDI devices), xinput (every name of it), dinput8 (an empty DirectInput), avrt (MMCSS registration) and iphlpapi (the
 * adapter list). The libraries are loaded by name, as the engines do.
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */

#include <stdarg.h>
#include <stdio.h>

#define DIRECTINPUT_VERSION 0x0800
#include "winsock2.h"
#include "ws2ipdef.h"
#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "xinput.h"
#include "dinput.h"
#include "avrt.h"
#include "iphlpapi.h"
#include "objbase.h"
#include "wine/test.h"

static void test_time(void)
{
    TIMECAPS caps;
    MMTIME mm;
    DWORD a, b;

    ok(timeGetDevCaps(&caps, sizeof(caps)) == TIMERR_NOERROR && caps.wPeriodMin == 1 && caps.wPeriodMax >= 1000, "timeGetDevCaps %u %u\n", caps.wPeriodMin, caps.wPeriodMax);
    ok(timeGetDevCaps(NULL, sizeof(caps)) != TIMERR_NOERROR, "no output\n");
    ok(timeBeginPeriod(1) == TIMERR_NOERROR && timeEndPeriod(1) == TIMERR_NOERROR, "period 1\n");
    ok(timeBeginPeriod(0) == TIMERR_NOCANDO, "period 0\n");
    a = timeGetTime();
    Sleep(50);
    b = timeGetTime();
    ok(b - a >= 40 && b - a < 1000, "timeGetTime advanced %lu ms over a 50 ms sleep\n", b - a);
    ok(timeGetSystemTime(&mm, sizeof(mm)) == TIMERR_NOERROR && mm.wType == TIME_MS && mm.u.ms >= b, "timeGetSystemTime %u %lu\n", mm.wType, mm.u.ms);
}

static LONG fired;
static void CALLBACK count_proc(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2) { InterlockedIncrement(&fired); }

static void test_time_events(void)
{
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT id, ids[17], i;

    fired = 0;
    id = timeSetEvent(10, 1, count_proc, 0, TIME_PERIODIC);
    ok(id != 0, "periodic timeSetEvent\n");
    Sleep(200);
    ok(timeKillEvent(id) == TIMERR_NOERROR, "timeKillEvent\n");
    ok(fired >= 5 && fired <= 25, "a 10 ms periodic event fired %ld times in 200 ms\n", fired);
    ok(timeKillEvent(id) == TIMERR_NOCANDO, "killed twice\n");

    id = timeSetEvent(10, 0, (LPTIMECALLBACK)event, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
    ok(id != 0, "one-shot event timer\n");
    ok(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0, "the event was set\n");
    Sleep(20);
    ok(timeKillEvent(id) == TIMERR_NOCANDO, "a one-shot event is gone once it fired\n");

    for (i = 0; i < 17; i++) ids[i] = timeSetEvent(10000, 0, count_proc, 0, TIME_ONESHOT);
    for (i = 0; i < 16; i++) ok(ids[i] != 0, "%u) timeSetEvent\n", i);
    ok(ids[16] == 0, "a 17th event is refused\n");
    for (i = 0; i < 16; i++) ok(timeKillEvent(ids[i]) == TIMERR_NOERROR, "%u) timeKillEvent\n", i);
    id = timeSetEvent(10000, 0, count_proc, 0, TIME_ONESHOT);
    ok(id != 0 && id != ids[0], "ids are not reused right away (%u)\n", id);
    timeKillEvent(id);
    ok(timeSetEvent(0, 0, count_proc, 0, TIME_ONESHOT) == 0, "a delay of 0 is refused\n");
    ok(timeSetEvent(10, 0, NULL, 0, TIME_ONESHOT) == 0, "no callback\n");
    CloseHandle(event);
}

static void test_devices(void)
{
    JOYINFOEX joy = {sizeof(joy), JOY_RETURNALL};
    JOYCAPSW caps;
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    HWAVEOUT wave = (HWAVEOUT)0xdeadbeef;
    HMIDIOUT midi = (HMIDIOUT)0xdeadbeef;

    ok(joyGetNumDevs() == 16, "joyGetNumDevs %u\n", joyGetNumDevs());
    ok(joyGetDevCapsW(0, &caps, sizeof(caps)) == JOYERR_PARMS, "no joystick in slot 0\n");
    ok(joyGetDevCapsW(16, &caps, sizeof(caps)) == MMSYSERR_NODRIVER, "slot 16\n");
    ok(joyGetPosEx(JOYSTICKID1, &joy) == JOYERR_UNPLUGGED, "joyGetPosEx %u\n", joyGetPosEx(JOYSTICKID1, &joy));
    ok(waveOutGetNumDevs() == 0, "no wave device\n");
    ok(waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NODRIVER && wave == NULL, "waveOutOpen mapper %p\n", wave);
    ok(waveOutOpen(&wave, 0, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_BADDEVICEID, "waveOutOpen device 0\n");
    ok(midiOutOpen(&midi, 0, 0, 0, CALLBACK_NULL) == MMSYSERR_BADDEVICEID && midi == NULL, "midiOutOpen %p\n", midi);
}

static void test_xinput(void)
{
    static const char *names[] = {"xinput1_4.dll", "XInput1_3.dll", "XInput9_1_0.dll"};
    DWORD (WINAPI *get_state)(DWORD, XINPUT_STATE *);
    DWORD (WINAPI *get_state_ex)(DWORD, XINPUT_STATE *);
    DWORD (WINAPI *set_state)(DWORD, XINPUT_VIBRATION *);
    DWORD (WINAPI *get_caps)(DWORD, DWORD, XINPUT_CAPABILITIES *);
    XINPUT_STATE state;
    XINPUT_VIBRATION vibration = {0};
    XINPUT_CAPABILITIES caps;
    HMODULE xinput;
    unsigned i, slot;

    for (i = 0; i < ARRAY_SIZE(names); i++)
    {
        xinput = LoadLibraryA(names[i]);
        ok(xinput != NULL, "LoadLibrary %s\n", names[i]);
        if (!xinput) continue;
        get_state = (void *)GetProcAddress(xinput, "XInputGetState");
        ok(get_state != NULL, "%s XInputGetState\n", names[i]);
        if (!get_state) continue;
        for (slot = 0; slot < XUSER_MAX_COUNT; slot++)
            ok(get_state(slot, &state) == ERROR_DEVICE_NOT_CONNECTED, "%s slot %u is not connected\n", names[i], slot);
        ok(get_state(XUSER_MAX_COUNT, &state) == ERROR_BAD_ARGUMENTS, "slot 4\n");
    }
    xinput = GetModuleHandleA("xinput1_4.dll");
    get_state_ex = (void *)GetProcAddress(xinput, (const char *)100);
    ok(get_state_ex != NULL && get_state_ex(0, &state) == ERROR_DEVICE_NOT_CONNECTED, "XInputGetStateEx (@100)\n");
    set_state = (void *)GetProcAddress(xinput, "XInputSetState");
    ok(set_state(0, &vibration) == ERROR_DEVICE_NOT_CONNECTED, "XInputSetState\n");
    get_caps = (void *)GetProcAddress(xinput, "XInputGetCapabilities");
    ok(get_caps(0, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_DEVICE_NOT_CONNECTED, "XInputGetCapabilities\n");
    ok(get_caps(0, 0x10, &caps) == ERROR_BAD_ARGUMENTS, "an unknown flag\n");
}

static BOOL CALLBACK count_device(const DIDEVICEINSTANCEW *instance, void *context) { ++*(int *)context; return DIENUM_CONTINUE; }

static void test_dinput8(void)
{
    static const GUID iid_w = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID iid_a = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID sys_mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    HRESULT (WINAPI *create)(HINSTANCE, DWORD, REFIID, void **, IUnknown *);
    HMODULE dinput = LoadLibraryA("dinput8.dll");
    IDirectInput8W *di = NULL;
    IDirectInput8A *dia = NULL;
    IDirectInputDevice8W *device = (void *)0xdeadbeef;
    int count = 0;
    HRESULT hr;

    ok(dinput != NULL, "LoadLibrary dinput8\n");
    create = (void *)GetProcAddress(dinput, "DirectInput8Create");
    ok(create != NULL, "DirectInput8Create\n");
    if (!create) return;
    di = (void *)0xdeadbeef;
    hr = create(GetModuleHandleW(NULL), 0x0700, &iid_w, (void **)&di, NULL);
    ok(hr == DIERR_OLDDIRECTINPUTVERSION && di == NULL, "version 7: %#lx %p\n", hr, di);
    hr = create(NULL, DIRECTINPUT_VERSION, &iid_w, (void **)&di, NULL);
    ok(hr == DIERR_INVALIDPARAM && di == NULL, "no instance: %#lx\n", hr);
    hr = create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &iid_w, (void **)&di, NULL);
    ok(hr == DI_OK && di != NULL, "DirectInput8Create %#lx %p\n", hr, di);      /* (Godot's joypad code uses the object right after DI_OK) */
    if (!di) return;
    hr = IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, count_device, &count, DIEDFL_ATTACHEDONLY);
    ok(hr == DI_OK && count == 0, "EnumDevices %#lx %d\n", hr, count);
    ok(IDirectInput8_EnumDevices(di, DI8DEVCLASS_ALL, NULL, NULL, 0) == DIERR_INVALIDPARAM, "no callback\n");
    hr = IDirectInput8_CreateDevice(di, &sys_mouse, &device, NULL);
    ok(hr == DIERR_DEVICENOTREG && device == NULL, "CreateDevice %#lx %p\n", hr, device);
    ok(IDirectInput8_GetDeviceStatus(di, &sys_mouse) == DIERR_DEVICENOTREG, "GetDeviceStatus\n");
    hr = IDirectInput8_QueryInterface(di, &iid_a, (void **)&dia);
    ok(hr == S_OK && dia != NULL, "QueryInterface A %#lx\n", hr);
    if (dia)
    {
        count = 0;
        ok(IDirectInput8_EnumDevices(dia, DI8DEVCLASS_ALL, (LPDIENUMDEVICESCALLBACKA)count_device, &count, 0) == DI_OK && count == 0, "A EnumDevices\n");
        ok(IDirectInput8_Release(dia) == 1, "A release\n");
    }
    ok(IDirectInput8_Release(di) == 0, "last release\n");

    /* SDL makes the object with CoCreateInstance and initializes it itself */
    {
        static const GUID clsid = {0x25E609E4, 0xB259, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        di = NULL;
        hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid_w, (void **)&di);
        ok(hr == S_OK && di != NULL, "CoCreateInstance(CLSID_DirectInput8) %#lx\n", hr);
        if (di)
        {
            count = 0;
            ok(IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, count_device, &count, 0) == DIERR_NOTINITIALIZED, "before Initialize\n");
            ok(IDirectInput8_Initialize(di, GetModuleHandleW(NULL), DIRECTINPUT_VERSION) == DI_OK, "Initialize\n");
            ok(IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, count_device, &count, 0) == DI_OK && count == 0, "after Initialize\n");
            IDirectInput8_Release(di);
        }
        CoUninitialize();
    }
}

static void test_avrt(void)
{
    HMODULE avrt = LoadLibraryA("avrt.dll");
    HANDLE (WINAPI *set)(LPCWSTR, LPDWORD) = (void *)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
    BOOL (WINAPI *revert)(HANDLE) = (void *)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
    BOOL (WINAPI *responsiveness)(HANDLE, PULONG) = (void *)GetProcAddress(avrt, "AvQuerySystemResponsiveness");
    DWORD index = 0;
    ULONG value = 0;
    HANDLE h;

    ok(set && revert && responsiveness, "avrt exports\n");
    if (!set || !revert || !responsiveness) return;
    h = set(L"Pro Audio", &index);
    ok(h != NULL && index != 0, "AvSetMmThreadCharacteristicsW %p %lu\n", h, index);
    ok(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_TIME_CRITICAL, "priority %d\n", GetThreadPriority(GetCurrentThread()));
    ok(responsiveness(h, &value) && value == 20, "responsiveness %lu\n", value);
    ok(revert(h), "AvRevertMmThreadCharacteristics\n");
    ok(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL, "priority back %d\n", GetThreadPriority(GetCurrentThread()));
    ok(!revert(h) && GetLastError() == ERROR_INVALID_HANDLE, "reverted twice\n");
    ok(!revert((HANDLE)0x1234), "a made-up handle\n");
    SetLastError(0xdeadbeef);
    ok(set(L"No Such Task", &index) == NULL && GetLastError() == 1550 /* ERROR_INVALID_TASK_NAME */, "unknown task: %lu\n", GetLastError());
}

static void test_adapters(void)
{
    HMODULE iphlpapi = LoadLibraryA("iphlpapi.dll");
    ULONG (WINAPI *get)(ULONG, ULONG, void *, IP_ADAPTER_ADDRESSES *, ULONG *) = (void *)GetProcAddress(iphlpapi, "GetAdaptersAddresses");
    DWORD (WINAPI *luid_to_guid)(const NET_LUID *, GUID *) = (void *)GetProcAddress(iphlpapi, "ConvertInterfaceLuidToGuid");
    IP_ADAPTER_ADDRESSES *list, *aa;
    ULONG size = 0, size2 = 0, ret;
    BOOL loopback = FALSE;

    ok(get != NULL && luid_to_guid != NULL, "iphlpapi exports\n");
    if (!get || !luid_to_guid) return;
    ok(get(AF_UNSPEC, 0, NULL, NULL, NULL) == ERROR_INVALID_PARAMETER, "no size\n");
    ok(get(AF_UNSPEC, 0, NULL, NULL, &size) == ERROR_BUFFER_OVERFLOW && size > 0, "size %lu\n", size);
    ok(get(AF_UNSPEC, GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size2) == ERROR_BUFFER_OVERFLOW && size2 == size, "the size does not depend on the flags\n");
    ok(get(5, 0, NULL, NULL, &size2) == ERROR_INVALID_PARAMETER, "an unknown family\n");
    list = HeapAlloc(GetProcessHeap(), 0, size);
    /* Godot's flags */
    ret = get(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, list, &size);
    ok(ret == ERROR_SUCCESS, "GetAdaptersAddresses %lu\n", ret);
    for (aa = ret == ERROR_SUCCESS ? list : NULL; aa; aa = aa->Next)
    {
        const SOCKADDR_IN *in;
        GUID guid;
        char text[40];

        ok(aa->Length == sizeof(IP_ADAPTER_ADDRESSES_LH), "length %lu\n", aa->Length);
        ok(aa->AdapterName && aa->FriendlyName && aa->Description && aa->DnsSuffix, "names\n");
        ok(!aa->FriendlyName[0], "GAA_FLAG_SKIP_FRIENDLY_NAME leaves the name empty\n");
        ok(aa->FirstDnsServerAddress == NULL, "GAA_FLAG_SKIP_DNS_SERVER\n");
        ok(luid_to_guid(&aa->Luid, &guid) == NO_ERROR, "ConvertInterfaceLuidToGuid\n");
        sprintf(text, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        ok(!strcmp(text, aa->AdapterName), "adapter name %s, GUID of the LUID %s\n", aa->AdapterName, text);
        if (aa->IfType != IF_TYPE_SOFTWARE_LOOPBACK) continue;
        loopback = TRUE;
        ok(aa->IfIndex == 1 && aa->OperStatus == IfOperStatusUp, "loopback index %lu status %u\n", aa->IfIndex, aa->OperStatus);
        ok(aa->FirstUnicastAddress && !aa->FirstUnicastAddress->Next, "one loopback address\n");
        if (!aa->FirstUnicastAddress) continue;
        in = (const SOCKADDR_IN *)aa->FirstUnicastAddress->Address.lpSockaddr;
        ok(in->sin_family == AF_INET && in->sin_addr.s_addr == 0x0100007F /* 127.0.0.1 */, "loopback address %#lx\n", in->sin_addr.s_addr);
        ok(aa->FirstUnicastAddress->OnLinkPrefixLength == 8, "prefix %u\n", aa->FirstUnicastAddress->OnLinkPrefixLength);
        ok(aa->FirstPrefix == NULL, "no prefixes without GAA_FLAG_INCLUDE_PREFIX\n");
    }
    ok(loopback, "the loopback adapter is listed\n");
    size2 = size;
    ret = get(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, list, &size2);
    ok(ret == ERROR_SUCCESS && list->FirstPrefix && list->FirstPrefix->Next && list->FirstPrefix->Next->Next, "three prefixes\n");
    HeapFree(GetProcessHeap(), 0, list);
}

START_TEST(tcos_gameinput)
{
    test_time();
    test_time_events();
    test_devices();
    test_xinput();
    test_dinput8();
    test_avrt();
    test_adapters();
}

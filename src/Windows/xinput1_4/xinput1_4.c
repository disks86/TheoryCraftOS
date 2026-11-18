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
/* xinput1_4.dll for TheoryCraftOS: the XInput API over the kernel's game controllers. NtTcosGamepad (512, src/Process/WindowsGamepad.hpp) reads the
 * player slots of the game controller service (src/Hardware/GameController): Xbox 360 and Xbox One/Series pads, DualShock 4, DualSense, Switch Pro and
 * generic HID pads, all in XInput's layout. XInput's users 0..3 are the service's slots 0..3, so a pad keeps its user while it stays plugged in. An
 * empty slot is ERROR_DEVICE_NOT_CONNECTED, which is what a program polls for; a slot outside 0..3, an unknown flag or a missing output is
 * ERROR_BAD_ARGUMENTS, as on Windows. The older names of the library (xinput1_3, xinput9_1_0, ...) are folded into this one by the loader
 * (canonical_library_name), so the ordinal-only exports of xinput1_3/1_4 (XInputGetStateEx @100 ...) are here too (xinput1_4.def). Built
 * freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <xinput.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) NTSTATUS NTAPI NtTcosGamepad(ULONG64 *arguments);

#define XUSER_INDEX_ANY_ 0xFF
#define GUIDE_BUTTON_ 0x0400
#define STATUS_DEVICE_NOT_CONNECTED_ ((NTSTATUS)0xC000009D)
#define STATUS_NOT_SUPPORTED_ ((NTSTATUS)0xC00000BB)

/* Operations of NtTcosGamepad. */
enum { OP_GET_STATE = 1, OP_SET_VIBRATION = 2, OP_GET_INFO = 3 };
/* What GET_INFO says about the pad. */
enum { CAP_RUMBLE = 1, CAP_TRIGGERS = 16, CAP_RIGHT_STICK = 32 };
enum { KIND_UNKNOWN = 0, KIND_WIRED = 1, KIND_ALKALINE = 2, KIND_RECHARGEABLE = 3 };

/* XInputEnable(FALSE): the pads read as idle and their motors stop until it is enabled again (what a program does when it loses the focus). */
static volatile LONG g_disabled;

static NTSTATUS call(ULONG64 *w) { return NtTcosGamepad(w); }

static DWORD to_error(NTSTATUS status) {
  if (status == 0) return ERROR_SUCCESS;
  if (status == STATUS_DEVICE_NOT_CONNECTED_ || status == STATUS_NOT_SUPPORTED_) return ERROR_DEVICE_NOT_CONNECTED;   /* (no service: no pads) */
  return ERROR_BAD_ARGUMENTS;
}

/* The state of user `index` with the guide button or without (XInputGetState hides it, XInputGetStateEx shows it). */
static DWORD get_state(DWORD index, XINPUT_STATE *state, BOOL with_guide) {
  if (!state || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {OP_GET_STATE, index};
  const DWORD error = to_error(call(w));
  if (error != ERROR_SUCCESS) return error;
  state->dwPacketNumber = (DWORD)w[4];
  WORD buttons = (WORD)(w[5] & 0xF7FF);
  if (!with_guide) buttons &= (WORD)~GUIDE_BUTTON_;
  XINPUT_GAMEPAD *g = &state->Gamepad;
  g->wButtons = buttons;
  g->bLeftTrigger = (BYTE)w[6];
  g->bRightTrigger = (BYTE)(w[6] >> 8);
  g->sThumbLX = (SHORT)(WORD)w[7];
  g->sThumbLY = (SHORT)(WORD)(w[7] >> 16);
  g->sThumbRX = (SHORT)(WORD)(w[7] >> 32);
  g->sThumbRY = (SHORT)(WORD)(w[7] >> 48);
  if (g_disabled) {                         /* connected, but idle while disabled */
    g->wButtons = 0;
    g->bLeftTrigger = g->bRightTrigger = 0;
    g->sThumbLX = g->sThumbLY = g->sThumbRX = g->sThumbRY = 0;
  }
  return ERROR_SUCCESS;
}

static DWORD set_motors(DWORD index, WORD left, WORD right) {
  ULONG64 w[8] = {OP_SET_VIBRATION, index, left, right};
  const NTSTATUS status = call(w);
  if (status == STATUS_NOT_SUPPORTED_) return ERROR_SUCCESS;   /* a connected pad without motors: XInput says nothing about it */
  return to_error(status);
}

EXPORT void WINAPI XInputEnable(BOOL enable) {
  InterlockedExchange(&g_disabled, enable ? 0 : 1);
  if (!enable)
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) (void)set_motors(i, 0, 0);
}

EXPORT DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state) { return get_state(index, state, FALSE); }
EXPORT DWORD WINAPI XInputGetStateEx(DWORD index, XINPUT_STATE *state) { return get_state(index, state, TRUE); }

EXPORT DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration) {
  if (!vibration || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  if (g_disabled) return set_motors(index, 0, 0);
  return set_motors(index, vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed);
}

/* GET_INFO for user `index`: the words, or an error. */
static DWORD get_info(DWORD index, ULONG64 *w) {
  w[0] = OP_GET_INFO;
  w[1] = index;
  return to_error(call(w));
}

static void fill_capabilities(const ULONG64 *w, XINPUT_CAPABILITIES *c) {
  const ULONG64 caps = w[6];
  ZeroMemory(c, sizeof *c);
  c->Type = XINPUT_DEVTYPE_GAMEPAD;
  c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
  c->Flags = 0;
  /* What the pad has, as Windows reports it: the buttons and full-resolution sticks and triggers it supports. */
  c->Gamepad.wButtons = 0xF3FF;
  c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = (caps & CAP_TRIGGERS) ? 0xFF : 0x01;
  c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = (SHORT)0xFFC0;
  c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (caps & CAP_RIGHT_STICK) ? (SHORT)0xFFC0 : 0;
  if (caps & CAP_RUMBLE) {
    c->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
    c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFFFF;
  }
}

EXPORT DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities) {
  if (!capabilities || (flags & ~XINPUT_FLAG_GAMEPAD) != 0 || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  if (error != ERROR_SUCCESS) return error;
  fill_capabilities(w, capabilities);
  return ERROR_SUCCESS;
}
/* XInputGetCapabilitiesEx (@108): XINPUT_CAPABILITIES followed by vendor, product, revision and an id; version 1 is the only one. */
EXPORT DWORD WINAPI XInputGetCapabilitiesEx(DWORD version, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *capabilities) {
  if (version != 1 || !capabilities || (flags & ~XINPUT_FLAG_GAMEPAD) != 0 || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  if (error != ERROR_SUCCESS) return error;
  ZeroMemory(capabilities, sizeof *capabilities);
  fill_capabilities(w, &capabilities->Capabilities);
  capabilities->VendorId = (WORD)w[5];
  capabilities->ProductId = (WORD)(w[5] >> 16);
  return ERROR_SUCCESS;
}

/* Key presses: the changes of a user's buttons and triggers since the last look, one per call, as VK_PAD_* codes. A pad's history is this
 * process's (as on Windows); threads that ask at once may see an event twice or not at all, which XInput does not promise either. */
typedef struct { WORD mask; WORD key; } key_map;
static const key_map k_keys[] = {
    {XINPUT_GAMEPAD_A, VK_PAD_A}, {XINPUT_GAMEPAD_B, VK_PAD_B}, {XINPUT_GAMEPAD_X, VK_PAD_X}, {XINPUT_GAMEPAD_Y, VK_PAD_Y},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER}, {XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER},
    {XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP}, {XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN}, {XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT},
    {XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT}, {XINPUT_GAMEPAD_START, VK_PAD_START}, {XINPUT_GAMEPAD_BACK, VK_PAD_BACK},
    {XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS}, {XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS}};
#define TRIGGER_BIT_L 0x10000u
#define TRIGGER_BIT_R 0x20000u
static DWORD g_last_keys[XUSER_MAX_COUNT];

static DWORD keys_of(const XINPUT_STATE *s) {
  DWORD keys = s->Gamepad.wButtons;
  if (s->Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) keys |= TRIGGER_BIT_L;
  if (s->Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) keys |= TRIGGER_BIT_R;
  return keys;
}

/* The first change on user `index`, written to *k and remembered; FALSE if nothing changed. */
static BOOL next_keystroke(DWORD index, DWORD now, XINPUT_KEYSTROKE *k) {
  const DWORD changed = now ^ g_last_keys[index];
  if (!changed) return FALSE;
  WORD key = 0;
  DWORD bit = 0;
  for (unsigned i = 0; i < sizeof k_keys / sizeof k_keys[0] && !key; ++i)
    if (changed & k_keys[i].mask) { key = k_keys[i].key; bit = k_keys[i].mask; }
  if (!key && (changed & TRIGGER_BIT_L)) { key = VK_PAD_LTRIGGER; bit = TRIGGER_BIT_L; }
  if (!key && (changed & TRIGGER_BIT_R)) { key = VK_PAD_RTRIGGER; bit = TRIGGER_BIT_R; }
  if (!key) {                               /* (a change of a bit with no key: the guide button) */
    g_last_keys[index] = now;
    return FALSE;
  }
  g_last_keys[index] ^= bit;
  ZeroMemory(k, sizeof *k);
  k->VirtualKey = key;
  k->Flags = (now & bit) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
  k->UserIndex = (BYTE)index;
  return TRUE;
}

EXPORT DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, XINPUT_KEYSTROKE *keystroke) {
  (void)reserved;
  if (!keystroke) return ERROR_BAD_ARGUMENTS;
  if (index != XUSER_INDEX_ANY_ && index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  const DWORD first = index == XUSER_INDEX_ANY_ ? 0 : index, last = index == XUSER_INDEX_ANY_ ? XUSER_MAX_COUNT - 1 : index;
  BOOL any = FALSE;
  for (DWORD i = first; i <= last; ++i) {
    XINPUT_STATE s;
    if (get_state(i, &s, FALSE) != ERROR_SUCCESS) {
      g_last_keys[i] = 0;
      continue;
    }
    any = TRUE;
    if (next_keystroke(i, keys_of(&s), keystroke)) return ERROR_SUCCESS;
  }
  return any ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
}

EXPORT DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION *battery) {
  if (!battery || type > BATTERY_DEVTYPE_HEADSET || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  if (error != ERROR_SUCCESS) return error;
  if (type == BATTERY_DEVTYPE_HEADSET) {    /* no headsets */
    battery->BatteryType = BATTERY_TYPE_DISCONNECTED;
    battery->BatteryLevel = BATTERY_LEVEL_EMPTY;
    return ERROR_SUCCESS;
  }
  const unsigned kind = (unsigned)(w[7] & 0xFF), percent = (unsigned)((w[7] >> 8) & 0xFF);
  switch (kind) {
    case KIND_WIRED: battery->BatteryType = BATTERY_TYPE_WIRED; break;
    case KIND_ALKALINE: battery->BatteryType = BATTERY_TYPE_ALKALINE; break;
    case KIND_RECHARGEABLE: battery->BatteryType = BATTERY_TYPE_NIMH; break;
    default: battery->BatteryType = BATTERY_TYPE_UNKNOWN; break;
  }
  if (kind == KIND_WIRED || kind == KIND_UNKNOWN) battery->BatteryLevel = BATTERY_LEVEL_FULL;
  else battery->BatteryLevel = percent >= 70 ? BATTERY_LEVEL_FULL : percent >= 40 ? BATTERY_LEVEL_MEDIUM : percent >= 10 ? BATTERY_LEVEL_LOW : BATTERY_LEVEL_EMPTY;
  return ERROR_SUCCESS;
}

/* No headsets: a connected pad has no audio devices (empty strings, zero GUIDs). */
EXPORT DWORD WINAPI XInputGetAudioDeviceIds(DWORD index, LPWSTR render, UINT *render_count, LPWSTR capture, UINT *capture_count) {
  if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  if (error != ERROR_SUCCESS) return error;
  if (render && render_count && *render_count) render[0] = 0;
  if (render_count) *render_count = 0;
  if (capture && capture_count && *capture_count) capture[0] = 0;
  if (capture_count) *capture_count = 0;
  return ERROR_SUCCESS;
}
/* xinput1_3's form of the audio query (DirectSound device GUIDs). */
EXPORT DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture) {
  if (!render || !capture || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  if (error != ERROR_SUCCESS) return error;
  ZeroMemory(render, sizeof *render);
  ZeroMemory(capture, sizeof *capture);
  return ERROR_SUCCESS;
}

/* The ordinal-only helpers of the Xbox guide button and the controller power (@101 ... @104). Waiting for the guide button is answered at once
 * for a connected pad (the caller polls XInputGetStateEx instead); a USB pad cannot be powered off. */
EXPORT DWORD WINAPI XInputWaitForGuideButton(DWORD index, DWORD flags, void *listen) {
  (void)flags; (void)listen;
  if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  return get_info(index, w);
}
EXPORT DWORD WINAPI XInputCancelGuideButtonWait(DWORD index) {
  if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  return get_info(index, w);
}
EXPORT DWORD WINAPI XInputPowerOffController(DWORD index) {
  if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  return error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error;
}
EXPORT DWORD WINAPI XInputGetBaseBusInformation(DWORD index, void *information) {
  if (!information || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
  ULONG64 w[8] = {0};
  const DWORD error = get_info(index, w);
  return error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reason; (void)reserved;
  return TRUE;
}

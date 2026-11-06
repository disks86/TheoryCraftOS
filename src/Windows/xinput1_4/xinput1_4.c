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

/* xinput1_4.dll for TheoryCraftOS: the XInput API with no controller attached. Every query of one of the four user slots answers ERROR_DEVICE_NOT_CONNECTED, which is what a
 * program polls for (a stub answering 0 = ERROR_SUCCESS made every program see four phantom gamepads). A slot outside 0..3, an unknown flag or a missing output is
 * ERROR_BAD_ARGUMENTS, as on Windows. The older names of the library (xinput1_3, xinput9_1_0, ...) are folded into this one by the loader (canonical_library_name), so the
 * ordinal-only exports of xinput1_3/1_4 (XInputGetStateEx @100 ...) are here too (xinput1_4.def). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define XUSER_INDEX_ANY_ 0xFF

/* The answer for a slot: not connected for 0..3, bad arguments otherwise. */
static DWORD slot_answer(DWORD index) { return index < XUSER_MAX_COUNT ? ERROR_DEVICE_NOT_CONNECTED : ERROR_BAD_ARGUMENTS; }

EXPORT void WINAPI XInputEnable(BOOL enable) { (void)enable; }

EXPORT DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state) {
  if (!state) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}
EXPORT DWORD WINAPI XInputGetStateEx(DWORD index, XINPUT_STATE *state) { return XInputGetState(index, state); }

EXPORT DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration) {
  if (!vibration) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}

EXPORT DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities) {
  if (!capabilities || (flags & ~XINPUT_FLAG_GAMEPAD) != 0) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}
/* XInputGetCapabilitiesEx (@108): XINPUT_CAPABILITIES followed by vendor, product, revision and an id; version 1 is the only one. */
EXPORT DWORD WINAPI XInputGetCapabilitiesEx(DWORD version, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *capabilities) {
  if (version != 1 || !capabilities || (flags & ~XINPUT_FLAG_GAMEPAD) != 0) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}

/* A key press of any slot (XUSER_INDEX_ANY) or of one: there is nothing to read from a controller that is not there. */
EXPORT DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, XINPUT_KEYSTROKE *keystroke) {
  (void)reserved;
  if (!keystroke) return ERROR_BAD_ARGUMENTS;
  if (index == XUSER_INDEX_ANY_) return ERROR_DEVICE_NOT_CONNECTED;
  return slot_answer(index);
}

EXPORT DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION *battery) {
  if (!battery || type > BATTERY_DEVTYPE_HEADSET) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}

EXPORT DWORD WINAPI XInputGetAudioDeviceIds(DWORD index, LPWSTR render, UINT *render_count, LPWSTR capture, UINT *capture_count) {
  (void)render; (void)render_count; (void)capture; (void)capture_count;
  return slot_answer(index);
}
/* xinput1_3's form of the audio query (DirectSound device GUIDs). */
EXPORT DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture) {
  if (!render || !capture) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}

/* The ordinal-only helpers of the Xbox guide button and the controller power (@101 ... @104). */
EXPORT DWORD WINAPI XInputWaitForGuideButton(DWORD index, DWORD flags, void *listen) {
  (void)flags; (void)listen;
  return slot_answer(index);
}
EXPORT DWORD WINAPI XInputCancelGuideButtonWait(DWORD index) { return slot_answer(index); }
EXPORT DWORD WINAPI XInputPowerOffController(DWORD index) { return slot_answer(index); }
EXPORT DWORD WINAPI XInputGetBaseBusInformation(DWORD index, void *information) {
  if (!information) return ERROR_BAD_ARGUMENTS;
  return slot_answer(index);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reason; (void)reserved;
  return TRUE;
}

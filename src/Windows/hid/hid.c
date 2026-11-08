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

/* hid.dll for TheoryCraftOS: the HID class library. A program gets to a HID device through its interface path (SetupDi* with GUID_DEVINTERFACE_HID) and CreateFile;
 * no such path is offered to programs here (setupapi lists no devices), so every HidD_* call on a handle fails - ERROR_INVALID_HANDLE for a NULL or invalid handle,
 * ERROR_NOT_SUPPORTED for any other, since that is not a HID device - and no preparsed data ever exists, so the HidP_* parsers give
 * HIDP_STATUS_INVALID_PREPARSED_DATA with their outputs zeroed. Real: HidD_GetHidGuid and HidP_UsageListDifference (they need no
 * device). HidD_Hello stays a stub. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <hidsdi.h>
#include <hidpi.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define ST_INVALID_PREPARSED ((NTSTATUS)0xC0110001)      /* HIDP_STATUS_INVALID_PREPARSED_DATA */
#define ST_HIDP_SUCCESS ((NTSTATUS)0x00110000)           /* HIDP_STATUS_SUCCESS */

static void zero(void *p, SIZE_T n) {
  BYTE *b = p;
  if (b)
    while (n--) *b++ = 0;
}

/* ------------------------------------------------------------------------------------------------------------------------ HidD */

static BOOLEAN no_device(HANDLE device) {
  SetLastError(device == NULL || device == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE : ERROR_NOT_SUPPORTED);
  return FALSE;
}

/* {4D1E55B2-F16F-11CF-88CB-001111000030} */
EXPORT void NTAPI HidD_GetHidGuid(LPGUID guid) {
  static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
  if (guid) *guid = kHid;
}

EXPORT BOOLEAN NTAPI HidD_FlushQueue(HANDLE device) { return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetAttributes(HANDLE device, PHIDD_ATTRIBUTES attributes) {
  if (attributes) { zero(attributes, sizeof *attributes); attributes->Size = sizeof *attributes; }
  return no_device(device);
}
EXPORT BOOLEAN NTAPI HidD_GetConfiguration(HANDLE device, PHIDD_CONFIGURATION configuration, ULONG length) { zero(configuration, length); return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_SetConfiguration(HANDLE device, PHIDD_CONFIGURATION configuration, ULONG length) { (void)configuration; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetFeature(HANDLE device, PVOID report, ULONG length) { (void)report; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_SetFeature(HANDLE device, PVOID report, ULONG length) { (void)report; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetInputReport(HANDLE device, PVOID report, ULONG length) { (void)report; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_SetOutputReport(HANDLE device, PVOID report, ULONG length) { (void)report; (void)length; return no_device(device); }
/* The string queries leave an empty string in the buffer (for callers that print it without looking at the result). */
static BOOLEAN no_string(HANDLE device, PVOID buffer, ULONG length) {
  if (buffer && length >= sizeof(WCHAR)) *(WCHAR *)buffer = 0;
  return no_device(device);
}
EXPORT BOOLEAN NTAPI HidD_GetIndexedString(HANDLE device, ULONG index, PVOID buffer, ULONG length) { (void)index; return no_string(device, buffer, length); }
EXPORT BOOLEAN NTAPI HidD_GetManufacturerString(HANDLE device, PVOID buffer, ULONG length) { return no_string(device, buffer, length); }
EXPORT BOOLEAN NTAPI HidD_GetProductString(HANDLE device, PVOID buffer, ULONG length) { return no_string(device, buffer, length); }
EXPORT BOOLEAN NTAPI HidD_GetSerialNumberString(HANDLE device, PVOID buffer, ULONG length) { return no_string(device, buffer, length); }
EXPORT BOOLEAN NTAPI HidD_GetMsGenreDescriptor(HANDLE device, PVOID buffer, ULONG length) { (void)buffer; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetPhysicalDescriptor(HANDLE device, PVOID buffer, ULONG length) { (void)buffer; (void)length; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetNumInputBuffers(HANDLE device, PULONG count) { if (count) *count = 0; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_SetNumInputBuffers(HANDLE device, ULONG count) { (void)count; return no_device(device); }
EXPORT BOOLEAN NTAPI HidD_GetPreparsedData(HANDLE device, PHIDP_PREPARSED_DATA *data) { if (data) *data = NULL; return no_device(device); }
/* No preparsed data is ever given out, so none can be freed. */
EXPORT BOOLEAN NTAPI HidD_FreePreparsedData(PHIDP_PREPARSED_DATA data) { (void)data; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }

/* ------------------------------------------------------------------------------------------------------------------------ HidP */

EXPORT NTSTATUS NTAPI HidP_GetCaps(PHIDP_PREPARSED_DATA data, PHIDP_CAPS caps) { (void)data; zero(caps, sizeof *caps); return ST_INVALID_PREPARSED; }
EXPORT NTSTATUS NTAPI HidP_GetLinkCollectionNodes(PHIDP_LINK_COLLECTION_NODE nodes, PULONG length, PHIDP_PREPARSED_DATA data) {
  (void)nodes; (void)data;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetButtonCaps(HIDP_REPORT_TYPE type, PHIDP_BUTTON_CAPS caps, PUSHORT length, PHIDP_PREPARSED_DATA data) {
  (void)type; (void)caps; (void)data;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetSpecificButtonCaps(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PHIDP_BUTTON_CAPS caps, PUSHORT length,
                                                 PHIDP_PREPARSED_DATA data) {
  (void)page; (void)collection; (void)usage;
  return HidP_GetButtonCaps(type, caps, length, data);
}
EXPORT NTSTATUS NTAPI HidP_GetValueCaps(HIDP_REPORT_TYPE type, PHIDP_VALUE_CAPS caps, PUSHORT length, PHIDP_PREPARSED_DATA data) {
  (void)type; (void)caps; (void)data;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetSpecificValueCaps(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PHIDP_VALUE_CAPS caps, PUSHORT length,
                                                PHIDP_PREPARSED_DATA data) {
  (void)page; (void)collection; (void)usage;
  return HidP_GetValueCaps(type, caps, length, data);
}
EXPORT NTSTATUS NTAPI HidP_GetExtendedAttributes(HIDP_REPORT_TYPE type, USHORT index, PHIDP_PREPARSED_DATA data, PHIDP_EXTENDED_ATTRIBUTES attributes, PULONG length) {
  (void)type; (void)index; (void)data; (void)attributes;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_InitializeReportForID(HIDP_REPORT_TYPE type, UCHAR id, PHIDP_PREPARSED_DATA data, PCHAR report, ULONG length) {
  (void)type; (void)id; (void)data; (void)report; (void)length;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetData(HIDP_REPORT_TYPE type, PHIDP_DATA list, PULONG length, PHIDP_PREPARSED_DATA data, PCHAR report, ULONG report_length) {
  (void)type; (void)list; (void)data; (void)report; (void)report_length;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_SetData(HIDP_REPORT_TYPE type, PHIDP_DATA list, PULONG length, PHIDP_PREPARSED_DATA data, PCHAR report, ULONG report_length) {
  (void)type; (void)list; (void)data; (void)report; (void)report_length;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT ULONG NTAPI HidP_MaxDataListLength(HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA data) { (void)type; (void)data; return 0; }
EXPORT ULONG NTAPI HidP_MaxUsageListLength(HIDP_REPORT_TYPE type, USAGE page, PHIDP_PREPARSED_DATA data) { (void)type; (void)page; (void)data; return 0; }

#define USAGE_LIST_CALL(name)                                                                                                                                       \
  EXPORT NTSTATUS NTAPI name(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, PUSAGE list, PULONG length, PHIDP_PREPARSED_DATA data, PCHAR report,             \
                             ULONG report_length) {                                                                                                                 \
    (void)type; (void)page; (void)collection; (void)list; (void)data; (void)report; (void)report_length;                                                            \
    if (length) *length = 0;                                                                                                                                        \
    return ST_INVALID_PREPARSED;                                                                                                                                    \
  }
USAGE_LIST_CALL(HidP_GetUsages)
USAGE_LIST_CALL(HidP_SetUsages)
USAGE_LIST_CALL(HidP_UnsetUsages)

EXPORT NTSTATUS NTAPI HidP_GetUsagesEx(HIDP_REPORT_TYPE type, USHORT collection, PUSAGE_AND_PAGE list, ULONG *length, PHIDP_PREPARSED_DATA data, PCHAR report,
                                       ULONG report_length) {
  (void)type; (void)collection; (void)list; (void)data; (void)report; (void)report_length;
  if (length) *length = 0;
  return ST_INVALID_PREPARSED;
}

EXPORT NTSTATUS NTAPI HidP_GetUsageValue(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PULONG value, PHIDP_PREPARSED_DATA data, PCHAR report,
                                         ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)data; (void)report; (void)report_length;
  if (value) *value = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetScaledUsageValue(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PLONG value, PHIDP_PREPARSED_DATA data, PCHAR report,
                                               ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)data; (void)report; (void)report_length;
  if (value) *value = 0;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_SetUsageValue(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, ULONG value, PHIDP_PREPARSED_DATA data, PCHAR report,
                                         ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)value; (void)data; (void)report; (void)report_length;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_SetScaledUsageValue(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, LONG value, PHIDP_PREPARSED_DATA data, PCHAR report,
                                               ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)value; (void)data; (void)report; (void)report_length;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_GetUsageValueArray(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PCHAR value, USHORT value_length,
                                              PHIDP_PREPARSED_DATA data, PCHAR report, ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)data; (void)report; (void)report_length;
  zero(value, value_length);
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_SetUsageValueArray(HIDP_REPORT_TYPE type, USAGE page, USHORT collection, USAGE usage, PCHAR value, USHORT value_length,
                                              PHIDP_PREPARSED_DATA data, PCHAR report, ULONG report_length) {
  (void)type; (void)page; (void)collection; (void)usage; (void)value; (void)value_length; (void)data; (void)report; (void)report_length;
  return ST_INVALID_PREPARSED;
}
EXPORT NTSTATUS NTAPI HidP_TranslateUsagesToI8042ScanCodes(PUSAGE changed, ULONG length, HIDP_KEYBOARD_DIRECTION action, PHIDP_KEYBOARD_MODIFIER_STATE modifiers,
                                                           PHIDP_INSERT_SCANCODES insert, PVOID context) {
  (void)changed; (void)length; (void)action; (void)modifiers; (void)insert; (void)context;
  return (NTSTATUS)0xC0110003;      /* HIDP_STATUS_I8042_TRANS_UNKNOWN */
}

/* The usages of the current list that are not in the previous one ("make") and the other way round ("break"); lists end at the first 0 or at length. */
static BOOL usage_in(const USAGE *list, ULONG length, USAGE u) {
  for (ULONG i = 0; i < length && list[i]; ++i)
    if (list[i] == u) return TRUE;
  return FALSE;
}
EXPORT NTSTATUS NTAPI HidP_UsageListDifference(PUSAGE previous, PUSAGE current, PUSAGE broken, PUSAGE made, ULONG length) {
  if (!previous || !current || !broken || !made) return (NTSTATUS)0xC0110002;      /* (HIDP_STATUS_INVALID_REPORT_TYPE: what Windows returns for bad lists) */
  ULONG b = 0, m = 0;
  for (ULONG i = 0; i < length && previous[i]; ++i)
    if (!usage_in(current, length, previous[i]) && !usage_in(broken, b, previous[i])) broken[b++] = previous[i];
  for (ULONG i = 0; i < length && current[i]; ++i)
    if (!usage_in(previous, length, current[i]) && !usage_in(made, m, current[i])) made[m++] = current[i];
  for (; b < length; ++b) broken[b] = 0;
  for (; m < length; ++m) made[m] = 0;
  return ST_HIDP_SUCCESS;
}

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

// Minimal hand-written UEFI definitions: only what this OS touches.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EFIAPI  // mingw-w64 default ABI already matches UEFI (Microsoft x64)

namespace efi {

using Handle = void*;
using Event = void*;
using Status = uint64_t;
using Char16 = char16_t;

constexpr Status kSuccess = 0;
constexpr Status kBufferTooSmall = 0x8000000000000005;  // EFI_BUFFER_TOO_SMALL
constexpr Status kNotReady = 0x8000000000000006;        // EFI_NOT_READY

using Uintn = uint64_t;

enum class LocateSearchType : uint32_t { AllHandles, ByRegisterNotify, ByProtocol };

enum class AllocateType : uint32_t { AnyPages, MaxAddress, Address };

// Memory types 1-10 are real RAM; 0 (reserved), 11+ (MMIO, PAL code,
// persistent) are holes or device space.
enum class MemoryType : uint32_t {
  LoaderData = 2,
  ConventionalMemory = 7,
};
constexpr uint32_t kFirstRamType = 1;
constexpr uint32_t kLastRamType = 10;

struct MemoryDescriptor {
  uint32_t type;
  uint32_t pad;
  uint64_t physical_start;
  uint64_t virtual_start;
  uint64_t number_of_pages;  // 4 KiB pages
  uint64_t attribute;
};

struct Guid {
  uint32_t a;
  uint16_t b, c;
  uint8_t d[8];
};

struct ConfigurationTable {
  Guid vendor_guid;
  void* vendor_table;
};

constexpr Guid kAcpi20TableGuid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
constexpr Guid kAcpi10TableGuid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

struct TableHeader {
  uint64_t signature;
  uint32_t revision, header_size, crc32, reserved;
};

struct SimpleTextOutput {
  void* reset;
  Status (*output_string)(SimpleTextOutput*, const Char16*);
  void* test_string;
  void* query_mode;
  void* set_mode;
  void* set_attribute;
  Status (*clear_screen)(SimpleTextOutput*);
  void* set_cursor_position;
  void* enable_cursor;
  void* mode;
};

struct InputKey {
  uint16_t scan_code;
  Char16 unicode_char;
};

struct SimpleTextInput {
  void* reset;
  Status (*read_key_stroke)(SimpleTextInput*, InputKey*);
  void* wait_for_key;
};

struct BootServices {
  TableHeader hdr;
  void* raise_tpl;
  void* restore_tpl;
  Status (*allocate_pages)(AllocateType, MemoryType, Uintn pages, uint64_t* address);
  Status (*free_pages)(uint64_t address, Uintn pages);
  Status (*get_memory_map)(Uintn* map_size, MemoryDescriptor* map, Uintn* map_key,
                           Uintn* descriptor_size, uint32_t* descriptor_version);
  Status (*allocate_pool)(MemoryType, Uintn size, void** buffer);
  Status (*free_pool)(void* buffer);
  Status (*create_event)(uint32_t type, Uintn tpl, void* notify, void* context, Event*);
  void* set_timer;
  void* wait_for_event;
  void* signal_event;
  Status (*close_event)(Event);
  Status (*check_event)(Event);
  void* install_protocol_interface;
  void* reinstall_protocol_interface;
  void* uninstall_protocol_interface;
  Status (*handle_protocol)(Handle, const Guid*, void** iface);
  void* reserved;
  void* register_protocol_notify;
  void* locate_handle;
  void* locate_device_path;
  void* install_configuration_table;
  void* load_image;
  void* start_image;
  void* exit;
  void* unload_image;
  void* exit_boot_services;
  void* get_next_monotonic_count;
  Status (*stall)(uint64_t microseconds);
  Status (*set_watchdog_timer)(uint64_t timeout, uint64_t code, uint64_t size, Char16* data);
  Status (*connect_controller)(Handle controller, Handle* driver_image, void* remaining_device_path,
                               uint8_t recursive);
  Status (*disconnect_controller)(Handle controller, Handle driver_image, Handle child);
  void* open_protocol;
  void* close_protocol;
  void* open_protocol_information;
  void* protocols_per_handle;
  Status (*locate_handle_buffer)(LocateSearchType, const Guid* protocol, void* key,
                                 Uintn* handle_count, Handle** buffer);
  Status (*locate_protocol)(const Guid*, void* registration, void** iface);
};

enum class ResetType : uint32_t { Cold, Warm, Shutdown };

// EFI_TIME: the real-time clock, as the firmware reports it.
struct Time {
  uint16_t year;  // 1900..9999
  uint8_t month;  // 1..12
  uint8_t day;    // 1..31
  uint8_t hour, minute, second;
  uint8_t pad1;
  uint32_t nanosecond;
  int16_t time_zone;  // minutes from UTC, or kUnspecifiedTimeZone: the fields are local time
  uint8_t daylight;
  uint8_t pad2;
};
constexpr int16_t kUnspecifiedTimeZone = 0x07FF;

struct TimeCapabilities {
  uint32_t resolution, accuracy;
  uint8_t sets_to_zero;
};

struct RuntimeServices {
  TableHeader hdr;
  Status (*get_time)(Time* time, TimeCapabilities* capabilities);  // capabilities may be null
  void* set_time;
  void* get_wakeup_time;
  void* set_wakeup_time;
  void* set_virtual_address_map;
  void* convert_pointer;
  void* get_variable;
  void* get_next_variable_name;
  void* set_variable;
  void* get_next_high_monotonic_count;
  void (*reset_system)(ResetType, Status, uint64_t size, void* data);
};

struct SystemTable {
  TableHeader hdr;
  Char16* firmware_vendor;
  uint32_t firmware_revision;
  Handle console_in_handle;
  SimpleTextInput* con_in;
  Handle console_out_handle;
  SimpleTextOutput* con_out;
  Handle std_err_handle;
  SimpleTextOutput* std_err;
  RuntimeServices* runtime_services;
  BootServices* boot_services;
  uint64_t number_of_table_entries;
  ConfigurationTable* configuration_table;
};

struct LoadedImage {
  uint32_t revision;
  Handle parent_handle;
  SystemTable* system_table;
  Handle device_handle;
  void* file_path;
  void* reserved;
  uint32_t load_options_size;
  void* load_options;
  void* image_base;
  uint64_t image_size;
};

constexpr Guid kLoadedImageGuid = {
    0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

// EFI_GRAPHICS_OUTPUT_PROTOCOL: the firmware's linear framebuffer.
enum class PixelFormat : uint32_t {
  RedGreenBlueReserved8Bit,  // memory order R, G, B, x
  BlueGreenRedReserved8Bit,  // memory order B, G, R, x
  BitMask,
  BltOnly,  // no directly addressable framebuffer
};

struct PixelBitmask {
  uint32_t red, green, blue, reserved;
};

struct GraphicsModeInfo {
  uint32_t version;
  uint32_t horizontal_resolution;
  uint32_t vertical_resolution;
  PixelFormat pixel_format;
  PixelBitmask pixel_information;
  uint32_t pixels_per_scan_line;
};

struct GraphicsMode {
  uint32_t max_mode;
  uint32_t mode;
  GraphicsModeInfo* info;
  Uintn size_of_info;
  uint64_t frame_buffer_base;
  Uintn frame_buffer_size;
};

struct GraphicsOutput {
  Status (*query_mode)(GraphicsOutput*, uint32_t mode, Uintn* size_of_info, GraphicsModeInfo** info);
  Status (*set_mode)(GraphicsOutput*, uint32_t mode);
  void* blt;
  GraphicsMode* mode;
};

constexpr Guid kGraphicsOutputGuid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

// EFI_EDID_DISCOVERED_PROTOCOL / EFI_EDID_ACTIVE_PROTOCOL: the EDID the display told the firmware, and the one the firmware is using.
struct EdidProtocol {
  uint32_t size_of_edid;
  uint8_t* edid;
};
constexpr Guid kEdidDiscoveredGuid = {0x1c0c34f6, 0xd380, 0x41fa, {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa}};
constexpr Guid kEdidActiveGuid = {0xbd8c1056, 0x9f36, 0x44ec, {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86}};

// EFI_PCI_IO_PROTOCOL: only enough to tell which PCI function a handle is.
struct PciIo {
  void* poll_mem;
  void* poll_io;
  void* mem_read;
  void* mem_write;
  void* io_read;
  void* io_write;
  void* pci_read;
  void* pci_write;
  void* copy_mem;
  void* map;
  void* unmap;
  void* allocate_buffer;
  void* free_buffer;
  void* flush;
  Status (*get_location)(PciIo*, Uintn* segment, Uintn* bus, Uintn* device, Uintn* function);
};

constexpr Guid kPciIoGuid = {
    0x4cf5b200, 0x68b8, 0x4ca5, {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}};

// EFI_SIMPLE_NETWORK_PROTOCOL: the firmware's own network driver for a NIC.
enum class SimpleNetworkState : uint32_t { Stopped, Started, Initialized };

struct SimpleNetworkMode {
  uint32_t state;
  uint32_t hw_address_size;
  uint32_t media_header_size;
  uint32_t max_packet_size;
  uint32_t nv_ram_size;
  uint32_t nv_ram_access_size;
  uint32_t receive_filter_mask;
  uint32_t receive_filter_setting;
  uint32_t max_mcast_filter_count;
  uint32_t mcast_filter_count;
  uint8_t mcast_filter[16][32];
  uint8_t current_address[32];
  uint8_t broadcast_address[32];
  uint8_t permanent_address[32];
  uint32_t if_type;
  uint8_t mac_address_changeable;
  uint8_t multiple_tx_supported;
  uint8_t media_present_supported;
  uint8_t media_present;
};

constexpr uint32_t kReceiveFilterUnicast = 0x01, kReceiveFilterMulticast = 0x02, kReceiveFilterBroadcast = 0x04, kReceiveFilterPromiscuousMulticast = 0x10;

struct SimpleNetwork {
  uint64_t revision;
  Status (*start)(SimpleNetwork*);
  Status (*stop)(SimpleNetwork*);
  Status (*initialize)(SimpleNetwork*, Uintn extra_rx_buffer, Uintn extra_tx_buffer);
  Status (*reset)(SimpleNetwork*, uint8_t extended_verification);
  Status (*shutdown)(SimpleNetwork*);
  Status (*receive_filters)(SimpleNetwork*, uint32_t enable, uint32_t disable, uint8_t reset_mcast_filter,
                            Uintn mcast_filter_count, void* mcast_filter);
  void* station_address;
  void* statistics;
  void* mcast_ip_to_mac;
  void* nv_data;
  Status (*get_status)(SimpleNetwork*, uint32_t* interrupt_status, void** tx_buffer);
  Status (*transmit)(SimpleNetwork*, Uintn header_size, Uintn buffer_size, void* buffer, void* source_address,
                     void* destination_address, uint16_t* protocol);
  Status (*receive)(SimpleNetwork*, Uintn* header_size, Uintn* buffer_size, void* buffer, void* source_address,
                    void* destination_address, uint16_t* protocol);
  void* wait_for_packet;
  SimpleNetworkMode* mode;
};

constexpr Guid kSimpleNetworkGuid = {
    0xA19832B9, 0xAC25, 0x11D3, {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

// EFI_BLOCK_IO_PROTOCOL: the firmware's driver for a disk (or a partition of one).
struct BlockIoMedia {
  uint32_t media_id;
  uint8_t removable_media;
  uint8_t media_present;
  uint8_t logical_partition;
  uint8_t read_only;
  uint8_t write_caching;
  uint32_t block_size;
  uint32_t io_align;
  uint64_t last_block;
};

struct BlockIo {
  uint64_t revision;
  BlockIoMedia* media;
  Status (*reset)(BlockIo*, uint8_t extended_verification);
  Status (*read_blocks)(BlockIo*, uint32_t media_id, uint64_t lba, Uintn buffer_size, void* buffer);
  Status (*write_blocks)(BlockIo*, uint32_t media_id, uint64_t lba, Uintn buffer_size, void* buffer);
  Status (*flush_blocks)(BlockIo*);
};

constexpr Guid kBlockIoGuid = {
    0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

// EFI_MP_SERVICES_PROTOCOL: enumerate processors and run code on them.
// Procedures run on application processors and must not call firmware.
using ApProcedure = void (*)(void* argument);

struct ProcessorInformation {
  uint64_t processor_id;
  uint32_t status_flag;  // bit0 BSP, bit1 enabled, bit2 healthy
  uint32_t package, core, thread;
};
constexpr uint32_t kProcessorEnabled = 1u << 1;
constexpr uint32_t kProcessorHealthy = 1u << 2;

struct MpServices {
  Status (*get_number_of_processors)(MpServices*, Uintn* total, Uintn* enabled);
  Status (*get_processor_info)(MpServices*, Uintn id, ProcessorInformation*);
  Status (*startup_all_aps)(MpServices*, ApProcedure, uint8_t single_thread, Event wait,
                            Uintn timeout_us, void* argument, Uintn** failed);
  Status (*startup_this_ap)(MpServices*, ApProcedure, Uintn id, Event wait, Uintn timeout_us,
                            void* argument, uint8_t* finished);
  void* switch_bsp;
  void* enable_disable_ap;
  Status (*who_am_i)(MpServices*, Uintn* id);
};

constexpr Guid kMpServicesGuid = {
    0x3fdda605, 0xa76e, 0x4f46, {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}};

}  // namespace efi

// Minimal hand-written UEFI definitions: only what this OS touches.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EFIAPI  // mingw-w64 default ABI already matches UEFI (Microsoft x64)

namespace efi {

using Handle = void*;
using Status = uint64_t;
using Char16 = char16_t;

constexpr Status kSuccess = 0;
constexpr Status kBufferTooSmall = 0x8000000000000005;  // EFI_BUFFER_TOO_SMALL

using Uintn = uint64_t;

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
  void* create_event;
  void* set_timer;
  void* wait_for_event;
  void* signal_event;
  void* close_event;
  void* check_event;
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
};

enum class ResetType : uint32_t { Cold, Warm, Shutdown };

struct RuntimeServices {
  TableHeader hdr;
  void* get_time;
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
  void* configuration_table;
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

}  // namespace efi

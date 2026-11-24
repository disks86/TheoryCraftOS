#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Generates the Venus backend's command layer of the shared Vulkan driver (docs/Venus.md):

  src/Vulkan/vk_names.h       #define vkX dx_vkX for every function the Direct3D backend (and the platforms) define: included before the Khronos headers, so
                              that backend's functions and prototypes carry the dx_ prefix and the exported names are free for the dispatch layer
  src/Vulkan/vn_commands.inc  vn_vkX for every command the Venus protocol carries that needs nothing but forwarding: vkCmd* are encoded into the command
                              buffer's stream; commands that create objects get their ids first; the rest are sent on the instance's ring (with a reply when
                              they return something or fill in an output, without one otherwise)
  src/Vulkan/vk_dispatch.inc  the exported entry points: each calls the Venus backend's function when the process runs on Venus (vn_selected()), the Direct3D
                              backend's otherwise; and kVnProcs, the table vkGetInstanceProcAddr / vkGetDeviceProcAddr answer from on Venus (the core
                              version or the extension each function belongs to, and its aliases)

The commands written by hand (src/Vulkan/vk_venus.inc) are listed in SPECIAL. Reads venus-protocol's vk.xml (the registry its encoders were generated
from) and src/Vulkan/venus-protocol (which commands have encoders).

Usage: scripts/gen-vulkan-venus.py <venus-protocol checkout>"""
import pathlib, re, sys
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "src/Vulkan"
if len(sys.argv) != 2:
    sys.exit(__doc__)
registry = ET.parse(pathlib.Path(sys.argv[1]) / "xmls/vk.xml").getroot()

# ---- what the protocol carries ---------------------------------------------------------------------------------------------------------------------------
protocol = "".join(p.read_text() for p in sorted((src / "venus-protocol").glob("vn_protocol_driver_*.h")))
has_call = set(re.findall(r"static inline \w+ vn_call_(vk\w+)\(", protocol))
has_async = set(re.findall(r"static inline void vn_async_(vk\w+)\(", protocol))
has_cmd = set(re.findall(r"static inline void vn_encode_(vkCmd\w+)\(struct vn_cs_encoder", protocol))
carried = has_call | has_async | has_cmd

# ---- the registry ----------------------------------------------------------------------------------------------------------------------------------------
def text_of(e):
    return "".join(e.itertext()).strip()

commands, aliases = {}, {}
for c in registry.find("commands"):
    if c.get("api") and "vulkan" not in c.get("api").split(","):
        continue
    if c.get("alias"):
        aliases[c.get("name")] = c.get("alias")
        continue
    proto = c.find("proto")
    name = proto.find("name").text
    ret = text_of(proto)[: -len(name)].strip()
    params = []
    for p in c.findall("param"):
        if p.get("api") and "vulkan" not in p.get("api").split(","):
            continue
        decl = re.sub(r"\s+", " ", text_of(p))
        pname = p.find("name").text
        ptype = p.find("type").text
        params.append({"decl": decl, "name": pname, "type": ptype, "len": p.get("len"), "optional": p.get("optional"),
                       "const": decl.startswith("const "), "pointer": "*" in decl, "array": "[" in decl})
    commands[name] = {"ret": ret, "params": params}

handles = {}
for t in registry.find("types"):
    if t.get("category") == "handle" and t.find("name") is not None:
        handles[t.find("name").text] = text_of(t).startswith("VK_DEFINE_HANDLE")
for t in registry.find("types"):
    if t.get("category") == "handle" and t.get("alias"):
        handles[t.get("name")] = handles.get(t.get("alias"), False)

core_version = {}      # command -> "VK_VERSION_1_x"
for f in registry.findall("feature"):
    # (newer registries split each version into VK_BASE_VERSION_x_y, VK_COMPUTE_VERSION_x_y, VK_GRAPHICS_VERSION_x_y and VK_VERSION_x_y)
    if "vulkan" not in f.get("api", "vulkan").split(",") or not re.match(r"VK_(BASE_|COMPUTE_|GRAPHICS_)?VERSION_\d+_\d+$", f.get("name")):
        continue
    version = "VK_VERSION_" + f.get("number").replace(".", "_")
    for r in f.findall("require"):
        for c in r.findall("command"):
            core_version.setdefault(c.get("name"), version)
extension_of = {}      # command -> (extension, kind)
for e in registry.find("extensions"):
    if e.get("supported") in ("disabled",) or "vulkan" not in e.get("supported", "vulkan").split(","):
        continue
    for r in e.findall("require"):
        for c in r.findall("command"):
            extension_of.setdefault(c.get("name"), (e.get("name"), e.get("type")))

# ---- the Direct3D backend and the platforms ---------------------------------------------------------------------------------------------------------------
fn = re.compile(r"^VKAPI\s+[\w\s\*]+?VKAPI_CALL\s+(vk\w+)\(", re.M)
generated = {"vn_commands.inc", "vk_dispatch.inc", "vk_venus.inc", "vk_exports.inc", "vk_external.inc"}
dx = set()
for f in sorted(src.glob("*.inc")):
    if f.name in generated or f.name.startswith("spirv") or f.name.startswith("vn_"):
        continue
    dx.update(fn.findall(f.read_text()))
platform_files = [src / "vk_external.inc", root / "src/Windows/vulkan-1/vulkan-1.c",   # (vk_external.inc: only the platforms with external memory have it)
                  root / "src/Linux/libvulkan/libvulkan.c", root / "src/Linux/libvulkan/wayland.inc",
                  root / "src/Linux/libvulkan/x11.inc"]
platform = set()
for f in platform_files:
    platform.update(fn.findall(f.read_text()))
dx |= platform
# Surfaces are the platform's on either backend (a VkSurfaceKHR is a window, not a device object).
SHARED = {n for n in dx if re.match(r"vkCreate\w+Surface(KHR|EXT)$", n)} | {"vkDestroySurfaceKHR"}

# ---- written by hand in vk_venus.inc -----------------------------------------------------------------------------------------------------------------------
SPECIAL = {
    "vkCreateInstance", "vkDestroyInstance", "vkEnumerateInstanceVersion", "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
    "vkEnumeratePhysicalDevices", "vkEnumeratePhysicalDeviceGroups", "vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
    "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceFeatures2",
    "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties",
    "vkGetPhysicalDeviceExternalBufferProperties", "vkGetPhysicalDeviceExternalFenceProperties", "vkGetPhysicalDeviceExternalSemaphoreProperties",
    "vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceToolProperties",
    "vkCreateDevice", "vkDestroyDevice", "vkGetDeviceQueue", "vkGetDeviceQueue2", "vkDeviceWaitIdle", "vkQueueWaitIdle", "vkWaitForFences",
    "vkWaitSemaphores", "vkGetQueryPoolResults",
    "vkAllocateMemory", "vkFreeMemory", "vkMapMemory", "vkUnmapMemory", "vkMapMemory2", "vkUnmapMemory2",
    "vkCreateCommandPool", "vkDestroyCommandPool", "vkResetCommandPool", "vkAllocateCommandBuffers", "vkFreeCommandBuffers", "vkBeginCommandBuffer",
    "vkEndCommandBuffer", "vkResetCommandBuffer",
    "vkCreateDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplate", "vkUpdateDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplate",
    "vkCmdPushDescriptorSetWithTemplate2", "vkSetPrivateData", "vkGetPrivateData", "vkCreatePrivateDataSlot", "vkDestroyPrivateDataSlot",
    "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR",
    "vkGetPhysicalDeviceSurfacePresentModesKHR", "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR",
    "vkQueuePresentKHR", "vkAcquireNextImage2KHR", "vkGetDeviceGroupPresentCapabilitiesKHR", "vkGetDeviceGroupSurfacePresentModesKHR",
    "vkGetPhysicalDevicePresentRectanglesKHR",
}
# Per platform: the presentation-support queries (vk_venus.inc answers them for the platforms that have them).
PLATFORM_SPECIAL = {"vkGetPhysicalDeviceWin32PresentationSupportKHR", "vkGetPhysicalDeviceWaylandPresentationSupportKHR",
                    "vkGetPhysicalDeviceXcbPresentationSupportKHR", "vkGetPhysicalDeviceXlibPresentationSupportKHR"}

# Extensions whose commands the Venus backend leaves out (the host's way of doing them does not reach the guest: external handles, host pointers,
# presentation the host does, deferred host operations ...). Their commands are not forwarded and the extensions are not offered.
DENIED_EXTENSIONS = re.compile(r"^VK_(KHR_external_(memory|semaphore|fence)_(fd|win32)|EXT_external_memory_(dma_buf|host|acquire_unmodified)|EXT_image_drm_format_modifier"
                               r"|ANDROID_\w+|EXT_queue_family_foreign|EXT_host_image_copy|EXT_map_memory_placed|KHR_deferred_host_operations"
                               r"|KHR_acceleration_structure|KHR_ray_\w+|EXT_descriptor_buffer|EXT_descriptor_heap|EXT_physical_device_drm|EXT_pci_bus_info"
                               r"|EXT_calibrated_timestamps|KHR_calibrated_timestamps|KHR_performance_query|EXT_device_memory_report|KHR_present_\w+"
                               r"|EXT_swapchain_\w+|KHR_swapchain_\w+|KHR_display\w*|EXT_display\w*|EXT_full_screen_exclusive|GOOGLE_display_timing"
                               r"|EXT_hdr_metadata|KHR_shared_presentable_image|KHR_incremental_present|KHR_video\w*|EXT_debug_\w+|EXT_tooling_info"
                               r"|KHR_pipeline_binary|EXT_shader_module_identifier|KHR_get_surface_capabilities2|EXT_surface_maintenance1"
                               r"|KHR_surface_maintenance1|KHR_swapchain_maintenance1|KHR_device_group_creation)$")
# Core commands whose features need host access the guest does not have (Vulkan 1.4's host image copy): left out like the extension above.
DENIED_COMMANDS = {"vkCopyMemoryToImage", "vkCopyImageToMemory", "vkCopyImageToImage", "vkTransitionImageLayout"}

def version_number(v):
    m = re.match(r"VK_VERSION_(\d+)_(\d+)", v)
    return (int(m.group(1)), int(m.group(2)))

def forwardable(name):
    if name in SPECIAL or name in PLATFORM_SPECIAL or name in DENIED_COMMANDS or name not in carried or name not in commands:
        return False
    ext = extension_of.get(name)
    if name not in core_version and (not ext or DENIED_EXTENSIONS.match(ext[0])):
        return False
    if name in core_version and version_number(core_version[name]) > (1, 3):
        return False      # (the backend offers Vulkan 1.3 at most: docs/Venus.md)
    return True

forwarded = sorted(n for n in commands if forwardable(n))

def call_args(c, replace_allocator=True):
    out = []
    for p in c["params"]:
        out.append("NULL" if replace_allocator and p["type"] == "VkAllocationCallbacks" else p["name"])
    return ", ".join(out)

def signature(name, c, prefix):
    return f"{c['ret']} VKAPI_CALL {prefix}{name}({', '.join(p['decl'] for p in c['params'])})"

def ring_of(c):
    first = c["params"][0]
    kind = {"VkInstance": "instance", "VkPhysicalDevice": "physical", "VkDevice": "device", "VkQueue": "queue", "VkCommandBuffer": "command_buffer"}.get(first["type"])
    if not kind:
        raise SystemExit(f"no dispatchable first parameter: {first}")
    return f"vn_ring_of_{kind}({first['name']})"

def output_handles(c):
    outs = []
    for p in c["params"]:
        if p["type"] in handles and not handles[p["type"]] and p["pointer"] and not p["const"]:
            outs.append(p)
    return outs

def has_output(c):
    return any(p["pointer"] and not p["const"] for p in c["params"]) or c["ret"] != "void"

def handle_value(t, idexpr):
    return f"VN_HANDLE({t}, {idexpr})"

body = []
for n in forwarded:
    c = commands[n]
    sig = signature(n, c, "vn_")
    alloc = [p for p in c["params"] if p["type"] == "VkAllocationCallbacks"]
    unused = "".join(f"  (void){p['name']};\n" for p in alloc)
    if n in has_cmd and n.startswith("vkCmd"):
        cb = c["params"][0]["name"]
        args = call_args(c)
        body.append(f"static {sig} {{\n{unused}  VnCommandBuffer *cmd = vn_command_buffer({cb});\n"
                    f"  if (!vn_command_reserve(cmd, vn_sizeof_{n}({args}))) return;\n  vn_encode_{n}(&cmd->cs, 0, {args});\n}}\n")
        continue
    outs = output_handles(c)
    ring = ring_of(c)
    args = call_args(c)
    lines = [f"static {sig} {{", unused.rstrip("\n")] if unused else [f"static {sig} {{"]
    for p in outs:
        count = p["len"].split(",")[0] if p["len"] else "1"
        lines.append(f"  vn_new_handles((uint64_t *){p['name']}, {count});")
    use_call = has_output(c) and n in has_call
    if not use_call and n not in has_async:
        use_call = n in has_call
    if use_call:
        if c["ret"] == "void":
            lines.append(f"  vn_call_{n}({ring}, {args});")
        else:
            lines.append(f"  const {c['ret']} result = vn_call_{n}({ring}, {args});")
            for p in outs:
                count = p["len"].split(",")[0] if p["len"] else "1"
                lines.append(f"  if (result == VK_ERROR_OUT_OF_HOST_MEMORY) vn_clear_handles((uint64_t *){p['name']}, {count});")
            lines.append("  return result;")
    else:
        lines.append(f"  vn_async_{n}({ring}, {args});")
        if c["ret"] == "VkResult":
            lines.append("  return VK_SUCCESS;")
    lines.append("}")
    body.append("\n".join(lines) + "\n")

header = "/* Generated by scripts/gen-vulkan-venus.py from venus-protocol's vk.xml: do not edit. */\n"
(src / "vn_commands.inc").write_text(header + "/* The Venus backend's forwarded commands (docs/Venus.md): " + str(len(forwarded)) + " of them. */\n\n" + "\n".join(body))

# ---- the rename header --------------------------------------------------------------------------------------------------------------------------------------
names = sorted(dx)
(src / "vk_names.h").write_text(header + """/* The Direct3D backend's functions carry the dx_ prefix: included by a platform before the Khronos headers, so that their prototypes are renamed too.
 * vk_dispatch.inc (included last) defines the exported names. */
#ifndef TCOS_VK_NAMES_H
#define TCOS_VK_NAMES_H
""" + "".join(f"#define {n} dx_{n}\n" for n in names) + "#endif\n")

# ---- the dispatch layer ---------------------------------------------------------------------------------------------------------------------------------
venus_functions = set(forwarded) | SPECIAL | PLATFORM_SPECIAL
# What the platform exports: everything the Direct3D backend defines, the core of Vulkan 1.0 - 1.3 and the surface and swapchain functions (a real vulkan-1.dll's
# and libvulkan.so.1's exports).
exported = set(dx)
for n, v in core_version.items():
    if version_number(v) <= (1, 3) and n in commands:
        exported.add(n)
for n, (e, _) in extension_of.items():
    if e in ("VK_KHR_surface", "VK_KHR_swapchain") and n in commands:
        exported.add(n)
exported.discard("vkGetInstanceProcAddr")
exported.discard("vkGetDeviceProcAddr")

def failure(ret):
    if ret == "void":
        return "return;"
    if ret == "VkResult":
        return "return VK_ERROR_FEATURE_NOT_PRESENT;"
    if ret.startswith("PFN_"):
        return "return NULL;"
    return f"return ({ret})0;"

out = [header, """/* The exported Vulkan entry points of the platform (docs/Venus.md): one backend per process, chosen by vn_selected() (Venus when the system has a render node with
 * the Venus context type, unless TCOS_VULKAN_BACKEND=dxg; the Direct3D backend otherwise). Included last by the platform, after its own functions. */
"""]
platform_guard = {"vkCreateWin32SurfaceKHR": "VK_USE_PLATFORM_WIN32_KHR"}
out.append("#undef VKAPI\n#define VKAPI VK_OS_EXPORT VKAPI_ATTR\n")
for n in names:
    out.append(f"#undef {n}\n")
exported_table = []
for n in sorted(exported):
    c = commands.get(n)
    if not c:
        continue
    in_dx, in_vn = n in dx, n in venus_functions
    args = call_args(c, replace_allocator=False)
    ret = "" if c["ret"] == "void" else "return "
    guard_open, guard_close = "", ""
    if n in platform:
        # (a platform function exists only on the platforms that define it)
        guard_open, guard_close = f"#if defined(TCOS_VK_HAS_{n})\n", "#endif\n"
    if n in SHARED:
        b = f"  {ret}dx_{n}({args});"
    elif in_dx and in_vn:
        b = f"  if (vn_selected()) {{\n    {ret}vn_{n}({args});\n" + ("    return;\n" if not ret else "") + f"  }}\n  {ret}dx_{n}({args});"
    elif in_dx:
        b = f"  if (vn_selected()) {{\n    vn_not_offered(\"{n}\");\n    {failure(c['ret'])}\n  }}\n  {ret}dx_{n}({args});"
    elif in_vn:
        b = f"  if (vn_selected()) {{\n    {ret}vn_{n}({args});\n" + ("    return;\n" if not ret else "") + f"  }}\n  vn_not_offered(\"{n}\");\n  {failure(c['ret'])}"
    else:
        b = f"  vn_not_offered(\"{n}\");\n  {failure(c['ret'])}"
    out.append(f"{guard_open}VKAPI {signature(n, c, '')} {{\n{b}\n}}\n{guard_close}")
    exported_table.append(f"{guard_open}    {{\"{n}\", (PFN_vkVoidFunction){n}}},\n{guard_close}")

# The exported entry points by name: what vkGetInstanceProcAddr / vkGetDeviceProcAddr answer for a name the platform exports (the same pointer a program linked
# against the export gets), on either backend.
out.append("\ntypedef struct VkExportedProc {\n  const char *name;\n  PFN_vkVoidFunction fn;\n} VkExportedProc;\nstatic const VkExportedProc kExportedProcs[] = {\n" + "".join(exported_table) + "};\n")

# kVnProcs: name -> function, with the version (major << 8 | minor, 0 for an extension's) and the extension it needs (NULL for core).
procs = []
for n in sorted(venus_functions):
    if n not in commands or n in ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr"):
        continue
    v = core_version.get(n)
    ext = extension_of.get(n)
    names_of = [n] + sorted(a for a, t in aliases.items() if t == n)
    for alias in names_of:
        aext = extension_of.get(alias, ext)
        version = 0
        e_name = "NULL"
        if alias == n and v:
            mj, mn = version_number(v)
            version = mj << 8 | mn
        elif aext:
            e_name = f'"{aext[0]}"'
        elif v:
            mj, mn = version_number(v)
            version = mj << 8 | mn
        if e_name != "NULL" and DENIED_EXTENSIONS.match(e_name.strip('"')):
            continue
        guard = f"#if defined(TCOS_VK_HAS_{n})\n" if n in PLATFORM_SPECIAL else ""
        level = 1 if commands[n]["params"] and commands[n]["params"][0]["type"] in ("VkDevice", "VkQueue", "VkCommandBuffer") else 0
        procs.append(f"{guard}    {{\"{alias}\", (PFN_vkVoidFunction)vn_{n}, {version}, {e_name}, {level}}},\n" + ("#endif\n" if guard else ""))
out.append("\nstatic const VnProc kVnProcs[] = {\n" + "".join(procs) + "};\n")
# The platform's surface functions, the same on both backends (found by name on Venus too).
shared = []
for n in sorted(SHARED):
    shared.append(f"#if defined(TCOS_VK_HAS_{n})\n    {{\"{n}\", (PFN_vkVoidFunction)dx_{n}, 0, NULL, 0}},\n#endif\n")
out.append("\nstatic const VnProc kVnShared[] = {\n    {\"vkDestroySurfaceKHR\", (PFN_vkVoidFunction)dx_vkDestroySurfaceKHR, 0, \"VK_KHR_surface\", 0},\n" +
           "".join(x for x in shared if "vkDestroySurfaceKHR" not in x) + "};\n\n#include \"vn_procaddr.inc\"\n")
(src / "vk_dispatch.inc").write_text("".join(out))

# ---- the device extensions the backend may offer: carried by the protocol (its info table), device extensions, not denied ----------------------------------------
info = (src / "venus-protocol/vn_protocol_driver_info.h").read_text()
carried_extensions = re.findall(r'\{ "(VK_\w+)", (\d+), (\d+) \}', info)
kinds = {e.get("name"): e.get("type") for e in registry.find("extensions")}
allowed = [(n, int(num)) for n, num, _ in carried_extensions if kinds.get(n) == "device" and not DENIED_EXTENSIONS.match(n[3:] if False else n)]
allowed = [(n, num) for n, num in allowed if not DENIED_EXTENSIONS.match(n)]
(src / "vn_tables.inc").write_text(header + "/* The device extensions the Venus backend offers when the host has them (carried by the protocol, nothing of them needs what the guest cannot do). */\n"
                                   "static const struct { const char *name; uint32_t number; } kVnDeviceExtensions[] = {\n" +
                                   "".join(f'    {{"{n}", {num}}},\n' for n, num in allowed) + "};\n")
print(f"{len(forwarded)} forwarded, {len(venus_functions)} Venus functions, {len(exported)} exports, {len(names)} renamed, {len(procs)} proc entries")

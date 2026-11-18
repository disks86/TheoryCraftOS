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

#include "Hardware/Bus/Thunderbolt/ThunderboltDevices.hpp"

namespace hardware {

namespace {
// (nhi.h: PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_*_NHI, TITAN_RIDGE_*_NHI, ICL_NHI0 / NHI1 and the USB4 hosts from TGL_NHI0 on)
constexpr TbNhiDevice kDevices[] = {
    {0x15BF, "Alpine Ridge LP", TbGeneration::AlpineRidge},          {0x15D2, "Alpine Ridge C 4C", TbGeneration::AlpineRidge},
    {0x15D9, "Alpine Ridge C 2C", TbGeneration::AlpineRidge},        {0x15DC, "Alpine Ridge LP (USB only)", TbGeneration::AlpineRidge},
    {0x15DD, "Alpine Ridge (USB only)", TbGeneration::AlpineRidge},  {0x15DE, "Alpine Ridge C (USB only)", TbGeneration::AlpineRidge},
    {0x15E8, "Titan Ridge 2C", TbGeneration::TitanRidge},            {0x15EB, "Titan Ridge 4C", TbGeneration::TitanRidge},
    {0x8A17, "Ice Lake NHI 0", TbGeneration::IceLake},                {0x8A0D, "Ice Lake NHI 1", TbGeneration::IceLake},
    {0x9A1B, "Tiger Lake NHI 0", TbGeneration::Usb4},                 {0x9A1D, "Tiger Lake NHI 1", TbGeneration::Usb4},
    {0x9A1F, "Tiger Lake-H NHI 0", TbGeneration::Usb4},               {0x9A21, "Tiger Lake-H NHI 1", TbGeneration::Usb4},
    {0x463E, "Alder Lake NHI 0", TbGeneration::Usb4},                 {0x466D, "Alder Lake NHI 1", TbGeneration::Usb4},
    {0xA73E, "Raptor Lake NHI 0", TbGeneration::Usb4},                {0xA76D, "Raptor Lake NHI 1", TbGeneration::Usb4},
    {0x7EB2, "Meteor Lake-M NHI 0", TbGeneration::Usb4},              {0x7EC2, "Meteor Lake-P NHI 0", TbGeneration::Usb4},
    {0x7EC3, "Meteor Lake-P NHI 1", TbGeneration::Usb4},              {0xA833, "Lunar Lake NHI 0", TbGeneration::Usb4},
    {0xA834, "Lunar Lake NHI 1", TbGeneration::Usb4},                 {0x5781, "Barlow Ridge host (80G)", TbGeneration::Usb4},
    {0x5784, "Barlow Ridge host (40G)", TbGeneration::Usb4},
};
}  // namespace

namespace {
// USB4 host interfaces of other makers that have a name here (they are found by their class whether or not they are listed). The AMD ids
// are those of Linux's nhi.c as remembered (unverified); none of them needs anything but the standard USB4 host interface.
constexpr struct { uint16_t vendor_id, device_id; TbNhiDevice device; } kOtherHosts[] = {
    {0x1022, 0x162E, {0x162E, "AMD Rembrandt USB4 host 0", TbGeneration::Usb4}},
    {0x1022, 0x162F, {0x162F, "AMD Rembrandt USB4 host 1", TbGeneration::Usb4}},
    {0x1022, 0x1668, {0x1668, "AMD Phoenix USB4 host 0", TbGeneration::Usb4}},
    {0x1022, 0x1669, {0x1669, "AMD Phoenix USB4 host 1", TbGeneration::Usb4}},
};
constexpr TbNhiDevice kGenericUsb4 = {0, "USB4 host interface", TbGeneration::Usb4};
}  // namespace

const TbNhiDevice* tb_find_host(uint16_t vendor_id, uint16_t device_id, uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
  if (const TbNhiDevice* intel = tb_find_nhi(vendor_id, device_id)) return intel;
  if (class_code != 0x0C || subclass != 0x03 || prog_if != kUsb4ProgIf) return nullptr;
  for (const auto& h : kOtherHosts)
    if (h.vendor_id == vendor_id && h.device_id == device_id) return &h.device;
  return &kGenericUsb4;
}

const TbNhiDevice* tb_find_nhi(uint16_t vendor_id, uint16_t device_id) {
  if (vendor_id != 0x8086) return nullptr;
  for (const TbNhiDevice& d : kDevices)
    if (d.device_id == device_id) return &d;
  return nullptr;
}

}  // namespace hardware

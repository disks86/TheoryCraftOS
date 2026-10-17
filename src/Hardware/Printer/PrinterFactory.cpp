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

#include "Hardware/Printer/PrinterFactory.hpp"
#include "Hardware/Printer/IppPrinter.hpp"
#include "Hardware/Printer/NetworkPrinter.hpp"

namespace hardware {

namespace {
void copy_text(char* dst, uint32_t capacity, const char* src) {
  uint32_t n = 0;
  for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
  dst[n] = 0;
}

// The strings and the PrinterInfo that point into them, constructed before the printer that keeps a copy of the info.
struct OwnedInfo {
  explicit OwnedInfo(const DiscoveredPrinter& d) {
    copy_text(manufacturer, sizeof manufacturer, d.manufacturer);
    copy_text(model, sizeof model, d.make_model[0] ? d.make_model : d.name);
    copy_text(ipp_path, sizeof ipp_path, d.ipp_path[0] ? d.ipp_path : "/ipp/print");
    copy_text(ipps_path, sizeof ipps_path, d.ipps_path[0] ? d.ipps_path : "/ipp/print");
    copy_text(lpd_queue, sizeof lpd_queue, d.lpd_queue[0] ? d.lpd_queue : "lp");
    info.manufacturer = manufacturer;
    info.model = model;
    const bool pcl = d.supports_pcl(), ps = d.supports_postscript();
    info.languages = static_cast<uint8_t>((pcl || !ps ? 1 << static_cast<int>(PrinterLanguage::Pcl) : 0) | (ps ? 1 << static_cast<int>(PrinterLanguage::PostScript) : 0));
    info.paper_sizes = static_cast<uint16_t>((1 << static_cast<int>(PaperSize::A4)) | (1 << static_cast<int>(PaperSize::Letter)));
    use_pcl = pcl || !ps;
  }
  char manufacturer[32] = {}, model[64] = {}, ipp_path[48] = {}, ipps_path[48] = {}, lpd_queue[32] = {};
  PrinterInfo info;
  bool use_pcl = true;
};

class DiscoveredIppPrinter final : private OwnedInfo, public IppPrinter {
 public:
  DiscoveredIppPrinter(network::INetworkStack& stack, allocators::IAllocator& allocator, const DiscoveredPrinter& d, const network::TlsConfig* tls)
      : OwnedInfo(d), IppPrinter(stack, d.address, OwnedInfo::info, allocator, tls ? OwnedInfo::ipps_path : OwnedInfo::ipp_path, tls ? d.ipps_port : d.ipp_port) {
    set_document_format(use_pcl ? "application/vnd.hp-PCL" : "application/postscript");
    if (tls) set_tls(*tls);
  }
};

class DiscoveredNetworkPrinter final : private OwnedInfo, public NetworkPrinter {
 public:
  DiscoveredNetworkPrinter(network::INetworkStack& stack, allocators::IAllocator& allocator, const DiscoveredPrinter& d, PrintProtocol protocol)
      : OwnedInfo(d), NetworkPrinter(stack, d.address, OwnedInfo::info, allocator, protocol) {
    set_lpd_queue(OwnedInfo::lpd_queue);
  }
};
}  // namespace

UniquePointer<IPrinter> make_discovered_printer(network::INetworkStack& stack, allocators::IAllocator& allocator, const DiscoveredPrinter& found, const network::TlsConfig* tls) {
  if (found.has_ipps && tls) return UniquePointer<IPrinter>(UniquePointer<DiscoveredIppPrinter>::make(allocator, stack, allocator, found, tls));
  if (found.has_ipp) return UniquePointer<IPrinter>(UniquePointer<DiscoveredIppPrinter>::make(allocator, stack, allocator, found, nullptr));
  if ((found.has_raw && found.raw_port == 9100) || found.raw_assumed())
    return UniquePointer<IPrinter>(UniquePointer<DiscoveredNetworkPrinter>::make(allocator, stack, allocator, found, PrintProtocol::Raw9100));
  if (found.has_lpd && found.lpd_port == 515)
    return UniquePointer<IPrinter>(UniquePointer<DiscoveredNetworkPrinter>::make(allocator, stack, allocator, found, PrintProtocol::Lpd));
  return UniquePointer<IPrinter>();
}

}  // namespace hardware

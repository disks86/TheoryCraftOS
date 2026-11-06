// What a controller with TCP segmentation offload does with one large IPv4 or IPv6 TCP segment, written for the tests independently of the drivers:
// the payload in pieces of `mss`, each frame with the headers copied, its own IP total length, identification (+1 each) and header checksum,
// sequence number, PSH and FIN only on the last, and the TCP checksum. `seed_without_length`: the TCP checksum field holds the pseudo-header
// sum without the length (as Intel's controllers want it prepared, igb_tso); else the field holds the stack's seed with the whole length
// (Linux's CHECKSUM_PARTIAL). Either way the checksum is built from the field the way the hardware does, so a wrong seed gives wrong frames.
#pragma once
#include <stdint.h>
#include <vector>

inline uint32_t tso_sum16(const uint8_t* p, size_t n, uint32_t sum = 0) {
  for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t(p[i]) << 8) | p[i + 1];
  if (n & 1) sum += uint32_t(p[n - 1]) << 8;
  return sum;
}
inline uint16_t tso_fold(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<uint16_t>(sum);
}

inline std::vector<std::vector<uint8_t>> reference_segment(const uint8_t* frame, size_t length, uint16_t mss, bool seed_without_length) {
  std::vector<std::vector<uint8_t>> out;
  const bool v6 = frame[12] == 0x86 && frame[13] == 0xDD;
  const uint32_t ip = 14, ihl = v6 ? 40u : (frame[14] & 0x0F) * 4u, tcp = ip + ihl, header = tcp + (frame[tcp + 12] >> 4) * 4u;
  if (length <= header || mss == 0) return out;
  const uint32_t payload = static_cast<uint32_t>(length - header);
  const uint16_t id = static_cast<uint16_t>((frame[ip + 4] << 8) | frame[ip + 5]);
  const uint32_t sequence = (uint32_t(frame[tcp + 4]) << 24) | (frame[tcp + 5] << 16) | (frame[tcp + 6] << 8) | frame[tcp + 7];
  const uint16_t field = static_cast<uint16_t>((frame[tcp + 16] << 8) | frame[tcp + 17]);
  // the field as a sum without the length
  uint32_t base = field;
  if (!seed_without_length) base += static_cast<uint16_t>(~(header - tcp + payload));
  for (uint32_t offset = 0, k = 0; offset < payload; offset += mss, ++k) {
    const uint32_t piece = payload - offset < mss ? payload - offset : mss;
    const bool last = offset + piece == payload;
    std::vector<uint8_t> f(frame, frame + header);
    f.insert(f.end(), frame + header + offset, frame + header + offset + piece);
    if (v6) {  // the payload length; no identification, no header checksum
      const uint32_t payload_length = header - tcp + piece;
      f[ip + 4] = static_cast<uint8_t>(payload_length >> 8); f[ip + 5] = static_cast<uint8_t>(payload_length);
    } else {
      const uint32_t total = header - ip + piece;
      f[ip + 2] = static_cast<uint8_t>(total >> 8); f[ip + 3] = static_cast<uint8_t>(total);
      f[ip + 4] = static_cast<uint8_t>((id + k) >> 8); f[ip + 5] = static_cast<uint8_t>(id + k);
      f[ip + 10] = f[ip + 11] = 0;
      const uint16_t ip_sum = static_cast<uint16_t>(~tso_fold(tso_sum16(&f[ip], ihl)));
      f[ip + 10] = static_cast<uint8_t>(ip_sum >> 8); f[ip + 11] = static_cast<uint8_t>(ip_sum);
    }
    const uint32_t s = sequence + offset;
    f[tcp + 4] = static_cast<uint8_t>(s >> 24); f[tcp + 5] = static_cast<uint8_t>(s >> 16); f[tcp + 6] = static_cast<uint8_t>(s >> 8); f[tcp + 7] = static_cast<uint8_t>(s);
    if (!last) f[tcp + 13] &= static_cast<uint8_t>(~0x09);  // PSH, FIN
    f[tcp + 16] = f[tcp + 17] = 0;
    const uint32_t tcp_length = header - tcp + piece;
    const uint16_t sum = static_cast<uint16_t>(~tso_fold(tso_sum16(&f[tcp], f.size() - tcp, base + tcp_length)));
    f[tcp + 16] = static_cast<uint8_t>(sum >> 8); f[tcp + 17] = static_cast<uint8_t>(sum);
    out.push_back(f);
  }
  return out;
}

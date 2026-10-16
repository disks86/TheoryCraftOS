#include "Network/Dns/DnsMessage.hpp"

namespace network::dns {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Reads a (possibly compressed) name at `offset` into dotted text. Sets *next to
// the offset just past the name as it appears at `offset` (compression pointers
// are not followed for that). False on malformed data.
bool read_name(const uint8_t* m, uint32_t length, uint32_t offset, char* out, uint32_t capacity, uint32_t* next) {
  uint32_t written = 0, jumps = 0;
  bool jumped = false;
  for (;;) {
    if (offset >= length) return false;
    const uint8_t b = m[offset];
    if ((b & 0xC0) == 0xC0) {  // compression pointer: the name continues elsewhere
      if (offset + 1 >= length) return false;
      if (!jumped) *next = offset + 2;
      jumped = true;
      offset = ((b & 0x3F) << 8) | m[offset + 1];
      if (++jumps > 64) return false;  // a pointer loop
    } else if (b == 0) {
      if (!jumped) *next = offset + 1;
      if (written >= capacity) return false;
      out[written] = '\0';
      return true;
    } else if (b & 0xC0) {
      return false;  // reserved label types
    } else {
      if (offset + 1 + b > length) return false;
      if (written + b + 2 > capacity) return false;
      if (written) out[written++] = '.';
      for (uint32_t i = 0; i < b; ++i) out[written++] = m[offset + 1 + i];
      offset += 1 + b;
    }
  }
}

void copy_name(char* to, const char* from) {
  while ((*to++ = *from++)) {}
}
}  // namespace

bool names_equal(const char* a, const char* b) {
  for (;; ++a, ++b) {
    if (*a == '.' && a[1] == '\0') ++a;  // ignore one trailing dot
    if (*b == '.' && b[1] == '\0') ++b;
    if (lower(*a) != lower(*b)) return false;
    if (*a == '\0') return true;
  }
}

uint32_t encode_name(const char* name, uint8_t* out, uint32_t capacity) {
  if (!name || !*name) return 0;
  uint32_t length = 0, written = 0;
  while (name[length]) ++length;
  if (name[length - 1] == '.') --length;  // an absolute name: drop the trailing dot
  if (length == 0 || length > kMaxTextName) return 0;

  uint32_t label_start = 0;
  for (uint32_t i = 0; i <= length; ++i) {
    if (i < length && name[i] != '.') continue;
    const uint32_t label = i - label_start;
    if (label == 0 || label > 63) return 0;  // empty label ("a..b") or too long
    if (written + 1 + label + 1 > capacity) return 0;
    out[written++] = static_cast<uint8_t>(label);
    for (uint32_t k = 0; k < label; ++k) out[written++] = static_cast<uint8_t>(name[label_start + k]);
    label_start = i + 1;
  }
  out[written++] = 0;
  return written;
}

uint32_t build_query(uint16_t id, const char* name, uint8_t* out, uint32_t capacity) {
  if (capacity < kHeaderSize + 5) return 0;
  store16(out, id);
  store16(out + 2, 0x0100);  // standard query, recursion desired
  store16(out + 4, 1);       // one question
  store16(out + 6, 0);
  store16(out + 8, 0);
  store16(out + 10, 0);
  const uint32_t n = encode_name(name, out + kHeaderSize, capacity - kHeaderSize - 4);
  if (!n) return 0;
  store16(out + kHeaderSize + n, kTypeA);
  store16(out + kHeaderSize + n + 2, kClassIn);
  return kHeaderSize + n + 4;
}

ParseResult parse_response(const uint8_t* m, uint32_t length, uint16_t id, const char* name, Answer* answer) {
  if (length < kHeaderSize || load16(m) != id) return ParseResult::Invalid;
  const uint16_t flags = load16(m + 2);
  if (!(flags & 0x8000) || (flags & 0x7800)) return ParseResult::Invalid;  // not a response to a standard query
  const uint16_t questions = load16(m + 4), answers = load16(m + 6);
  if (questions != 1) return ParseResult::Invalid;

  // The question must be the one we asked.
  char qname[kMaxName + 1];
  uint32_t o = kHeaderSize;
  if (!read_name(m, length, o, qname, sizeof(qname), &o) || o + 4 > length) return ParseResult::Invalid;
  if (!names_equal(qname, name) || load16(m + o) != kTypeA || load16(m + o + 2) != kClassIn) return ParseResult::Invalid;
  o += 4;

  if (flags & 0x0200) return ParseResult::Truncated;
  const uint8_t rcode = flags & 0x0F;
  if (rcode == 3) return ParseResult::NoSuchName;
  if (rcode != 0) return ParseResult::ServerError;

  // Follow the CNAME chain from the name asked for to an A record.
  char target[kMaxName + 1];
  copy_name(target, qname);
  uint32_t min_ttl = 0xFFFFFFFF;
  for (uint16_t i = 0; i < answers; ++i) {
    char owner[kMaxName + 1];
    if (!read_name(m, length, o, owner, sizeof(owner), &o) || o + 10 > length) return ParseResult::Invalid;
    const uint16_t type = load16(m + o), klass = load16(m + o + 2);
    const uint32_t ttl = load32(m + o + 4);
    const uint16_t rdlength = load16(m + o + 8);
    o += 10;
    if (o + rdlength > length) return ParseResult::Invalid;
    if (klass == kClassIn && names_equal(owner, target)) {
      if (type == kTypeA && rdlength == 4) {
        answer->address = Ipv4Address::read(m + o);
        answer->ttl = ttl < min_ttl ? ttl : min_ttl;
        return ParseResult::Answer;
      }
      if (type == kTypeCname) {  // the name is an alias: from here on look for the target's records
        uint32_t unused;
        char next[kMaxName + 1];
        if (!read_name(m, length, o, next, sizeof(next), &unused)) return ParseResult::Invalid;
        copy_name(target, next);
        if (ttl < min_ttl) min_ttl = ttl;
      }
    }
    o += rdlength;
  }
  return ParseResult::NoAddress;
}

}  // namespace network::dns

// DNS wire format helpers (RFC 1035): host-name encoding with compression
// pointers, and building / parsing the one kind of message the resolver uses -
// a recursive A-record question and its answer. Private to Network/Dns/.
#pragma once
#include "Network/NetworkTypes.hpp"

namespace network::dns {

constexpr uint16_t kTypeA = 1, kTypeCname = 5, kClassIn = 1;
constexpr uint32_t kHeaderSize = 12;
constexpr uint32_t kMaxName = 255;       // longest encoded name, including the length bytes
constexpr uint32_t kMaxTextName = 253;   // longest name as dotted text

// Writes `name` (dotted text) as length-prefixed labels ending in a zero byte.
// Returns the bytes written, or 0 if the name is invalid or does not fit.
uint32_t encode_name(const char* name, uint8_t* out, uint32_t capacity);

// A question: 12-byte header (recursion desired) and one A/IN question for
// `name`. Returns the message length, 0 on error.
uint32_t build_query(uint16_t id, const char* name, uint8_t* out, uint32_t capacity);

enum class ParseResult : uint8_t {
  Answer,       // an A record was found: fields below are set
  NoSuchName,   // NXDOMAIN
  NoAddress,    // the name exists but has no A record
  ServerError,  // the server reported an error (SERVFAIL, REFUSED, ...)
  Truncated,    // the TC bit is set: ask again over TCP
  Invalid,      // not an answer to our question (wrong ID/name, malformed, ...)
};

struct Answer {
  Ipv4Address address;
  uint32_t ttl;  // seconds, the smallest TTL along the CNAME chain
};

// Checks that `message` answers the question (id, `name`) and extracts the
// address. CNAME chains and name compression are handled.
ParseResult parse_response(const uint8_t* message, uint32_t length, uint16_t id, const char* name, Answer* answer);

// Case-insensitive comparison of two host names given as text (a trailing dot is ignored).
bool names_equal(const char* a, const char* b);

}  // namespace network::dns

// DxvaDecoderTest: what the HEVC and AV1 cases share - the reference MD5s ffmpeg made (tests/data/dxva_reference_md5.txt, see make_dxva_references.sh), MD5 of a
// picture laid out as ffmpeg's framemd5 hashes it, and a recorded program call (to damage and replay).
#ifndef DXVA_DECODER_TEST_REFS_INC
#define DXVA_DECODER_TEST_REFS_INC
#include <algorithm>
#include <map>
#include <random>
#include "Crypto/Crypto.hpp"

// The MD5s (as hex) of the pictures ffmpeg outputs for `file`, in output order.
static std::vector<std::string> dxva_reference_md5(const std::string& file) {
  static std::map<std::string, std::vector<std::string>> table;
  if (table.empty()) {
    std::vector<uint8_t> text = read_file("tests/data/dxva_reference_md5.txt");
    std::string all(text.begin(), text.end());
    size_t pos = 0;
    while (pos < all.size()) {
      size_t end = all.find('\n', pos);
      if (end == std::string::npos) end = all.size();
      const std::string line = all.substr(pos, end - pos);
      const size_t space = line.find(' ');
      if (space != std::string::npos) table[line.substr(0, space)].push_back(line.substr(space + 1));
      pos = end + 1;
    }
  }
  auto it = table.find(file);
  return it == table.end() ? std::vector<std::string>() : it->second;
}

static std::string dxva_md5_hex(const std::vector<uint8_t>& bytes) {
  crypto::Hasher h(crypto::HashKind::Md5);
  h.update(ConstByteView(bytes.data(), bytes.size()));
  uint8_t digest[16];
  h.finish(digest);
  char hex[33];
  for (int i = 0; i < 16; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return std::string(hex, 32);
}

// One decode() call as the program made it: the buffers and the key list, kept so that it can be replayed or damaged.
struct DxvaRecordedCall {
  std::vector<uint8_t> params, qm, slices, bitstream;
  std::vector<uint32_t> keys;
  uint32_t output_key = kNoDxvaKey;
  DxvaPicture picture() const {
    DxvaPicture p;
    p.picture_parameters = ConstByteView(params.data(), params.size());
    p.quantization_matrix = ConstByteView(qm.data(), qm.size());
    p.slice_control = ConstByteView(slices.data(), slices.size());
    p.bitstream = ConstByteView(bitstream.data(), bitstream.size());
    p.surface_keys = ArrayView<const uint32_t>(keys.data(), keys.size());
    p.output_key = output_key;
    return p;
  }
};

// How the program names surfaces: Direct3D 11 (index = array slice, no key list), Direct3D 12 (the indices are positions in a list made anew for each picture, here
// shuffled), Direct3D 12 with the output named apart (output_key, the current index outside the list).
enum class DxvaNaming { D3d11, D3d12Shuffled, D3d12OutputKey };
static const char* dxva_naming_name(DxvaNaming n) { return n == DxvaNaming::D3d11 ? "d3d11" : n == DxvaNaming::D3d12Shuffled ? "d3d12" : "d3d12-output"; }

// Damages every buffer of `call` (truncated at many lengths, bits flipped) and decodes it on `decoder` after `prefix` (the calls before it, to have references): no
// fault and no sanitizer report is what is checked; the result may be anything but a picture without a frame.
template <class MakeDecoder>
static int dxva_damage(MakeDecoder make, const std::vector<DxvaRecordedCall>& prefix, const DxvaRecordedCall& call, uint32_t seed) {
  std::mt19937 rng(seed);
  int runs = 0;
  auto run = [&](const DxvaRecordedCall& c) {
    UniquePointer<IDxvaDecoder> d = make();
    if (!d) return;
    for (const DxvaRecordedCall& p : prefix) {
      DxvaDecodedPicture o;
      (void)d->decode(p.picture(), &o);
    }
    DxvaDecodedPicture o;
    const DxvaResult r = d->decode(c.picture(), &o);
    if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) CHECK(o.frame.plane[0] != nullptr);
    ++runs;
  };
  for (int which = 0; which < 4; ++which) {
    const std::vector<uint8_t>& buffer = which == 0 ? call.params : which == 1 ? call.qm : which == 2 ? call.slices : call.bitstream;
    if (buffer.empty()) continue;
    // truncation
    const size_t step = buffer.size() > 64 ? buffer.size() / 24 : 1;
    for (size_t len = 0; len < buffer.size(); len += step) {
      DxvaRecordedCall c = call;
      std::vector<uint8_t>& b = which == 0 ? c.params : which == 1 ? c.qm : which == 2 ? c.slices : c.bitstream;
      b.resize(len);
      run(c);
    }
    // bit flips (one or several)
    for (int k = 0; k < 40; ++k) {
      DxvaRecordedCall c = call;
      std::vector<uint8_t>& b = which == 0 ? c.params : which == 1 ? c.qm : which == 2 ? c.slices : c.bitstream;
      const int flips = 1 + static_cast<int>(rng() % 4);
      for (int f = 0; f < flips; ++f) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      run(c);
    }
    // random bytes over a whole field-sized run
    for (int k = 0; k < 10; ++k) {
      DxvaRecordedCall c = call;
      std::vector<uint8_t>& b = which == 0 ? c.params : which == 1 ? c.qm : which == 2 ? c.slices : c.bitstream;
      const size_t at = rng() % b.size();
      for (size_t i = at; i < b.size() && i < at + 4; ++i) b[i] = static_cast<uint8_t>(rng());
      run(c);
    }
  }
  return runs;
}

#endif

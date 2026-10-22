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

#include "Browser/Objects.hpp"
#include <initializer_list>
#include "Browser/Url.hpp"
#include "Media/ImageCodecs.hpp"

namespace browser {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

StringView attr(const dom::Node* n, const char* name) {
  StringView v;
  return dom::get_attribute(n, name, &v) ? v : StringView();
}

double parse_number(StringView text, bool* ok) {
  uint64_t i = 0;
  double whole = 0;
  bool any = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    whole = whole * 10 + (text[i++] - '0');
    any = true;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    double scale = 0.1;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      whole += (text[i++] - '0') * scale;
      scale /= 10;
      any = true;
    }
  }
  *ok = any && i == text.size();
  return whole;
}

// Image types the codecs read.
bool supported_image_type(StringView type) {
  type = type.trimmed();
  if (type.empty()) return true;
  if (!type.starts_with("image/")) return false;
  const StringView sub = type.substr(6);
  for (const char* ok : {"png", "apng", "jpeg", "jpg", "pjpeg", "gif", "bmp", "x-bmp", "x-ms-bmp", "webp", "tiff", "x-tga", "x-targa", "qoi", "x-portable-pixmap", "x-portable-graymap", "x-portable-bitmap", "x-portable-anymap", "vnd.mozilla.apng", "svg+xml"})
    if (sub.equals_ignore_case(ok)) return true;
  return false;
}

bool supported_video_type(StringView type) {
  type = type.trimmed();
  if (type.empty()) return true;
  const uint64_t semi = type.find(';');
  if (semi != StringView::npos) type = type.first(semi).trimmed();
  for (const char* ok : {"video/mp4", "video/quicktime", "video/x-m4v", "video/x-msvideo", "video/avi", "video/msvideo", "image/gif", "image/apng", "image/png", "image/webp", "video/x-yuv4mpeg"})
    if (type.equals_ignore_case(ok)) return true;
  return false;
}

bool supported_audio_type(StringView type) {
  type = type.trimmed();
  if (type.empty()) return true;
  const uint64_t semi = type.find(';');
  if (semi != StringView::npos) type = type.first(semi).trimmed();
  for (const char* ok : {"audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg", "audio/mp4", "audio/x-m4a", "audio/m4a", "audio/aac", "audio/aacp", "audio/ogg", "audio/vorbis",
                         "application/ogg", "audio/flac", "audio/x-flac", "audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave", "audio/aiff", "audio/x-aiff", "audio/basic",
                         "audio/midi", "audio/x-midi", "audio/mid", "audio/sp-midi"})
    if (type.equals_ignore_case(ok)) return true;
  return false;
}

// srcset: "a.png 1x, b.png 2x" or "s.png 400w, l.png 800w": the one for a display `viewport` pixels wide.
bool pick_srcset(StringView srcset, int32_t viewport, String* out) {
  struct Candidate {
    StringView url;
    double density;
    double width;
  };
  Candidate best{StringView(), 0, 0};
  bool have = false, use_width = false;
  uint64_t i = 0;
  Candidate candidate_storage[32];
  const ArrayView<Candidate> candidates(candidate_storage);
  uint32_t count = 0;
  while (i < srcset.size() && count < 32) {
    while (i < srcset.size() && (is_space(srcset[i]) || srcset[i] == ',')) ++i;
    const uint64_t start = i;
    while (i < srcset.size() && !is_space(srcset[i])) ++i;
    StringView url = srcset.substr(start, i - start);
    if (url.empty()) break;
    Candidate c{url, 1, 0};
    if (url.ends_with(',')) {
      while (url.ends_with(',')) url = url.first(url.size() - 1);
      c.url = url;
    } else {
      const uint64_t d0 = i;
      while (i < srcset.size() && srcset[i] != ',') ++i;
      const StringView desc = srcset.substr(d0, i - d0).trimmed();
      if (!desc.empty()) {
        bool ok;
        const double v = parse_number(desc.first(desc.size() - 1), &ok);
        if (ok && desc.ends_with('w')) {
          c.width = v;
          c.density = 0;
          use_width = true;
        } else if (ok && desc.ends_with('x')) {
          c.density = v;
        }
      }
    }
    if (!c.url.empty()) candidates[count++] = c;
  }
  for (uint32_t k = 0; k < count; ++k) {
    const Candidate& c = candidates[k];
    if (!have) {
      best = c;
      have = true;
      continue;
    }
    if (use_width) {
      const bool best_enough = best.width >= viewport, c_enough = c.width >= viewport;
      if (best_enough && c_enough ? c.width < best.width : (!best_enough && (c_enough || c.width > best.width))) best = c;
    } else {
      const bool best_enough = best.density >= 1, c_enough = c.density >= 1;
      if (best_enough && c_enough ? c.density < best.density : (!best_enough && (c_enough || c.density > best.density))) best = c;
    }
  }
  if (!have) return false;
  out->assign(best.url);
  return true;
}

// Scales a picture to w x h: the average of the source pixels that fall in a pixel when shrinking, bilinear when enlarging.
bool scale_pixels(const ImageSource& src, uint32_t w, uint32_t h, ArrayView<uint32_t> dst) {
  if (!src.valid() || w == 0 || h == 0 || dst.size() < uint64_t{w} * h) return false;
  const bool shrink_x = src.width > w, shrink_y = src.height > h;
  for (uint32_t j = 0; j < h; ++j) {
    const uint64_t y0 = uint64_t{j} * src.height / h;
    uint64_t y1 = (uint64_t{j + 1} * src.height + h - 1) / h;
    if (y1 <= y0) y1 = y0 + 1;
    for (uint32_t i = 0; i < w; ++i) {
      const uint64_t x0 = uint64_t{i} * src.width / w;
      uint64_t x1 = (uint64_t{i + 1} * src.width + w - 1) / w;
      if (x1 <= x0) x1 = x0 + 1;
      if (!shrink_x && !shrink_y) {
        // enlarging (or the same size): bilinear between the four nearest source pixels
        const double fx = (i + 0.5) * src.width / w - 0.5, fy = (j + 0.5) * src.height / h - 0.5;
        int64_t ix = static_cast<int64_t>(fx < 0 ? 0 : fx), iy = static_cast<int64_t>(fy < 0 ? 0 : fy);
        const double tx = fx < 0 ? 0 : fx - ix, ty = fy < 0 ? 0 : fy - iy;
        const int64_t ix1 = ix + 1 < src.width ? ix + 1 : ix, iy1 = iy + 1 < src.height ? iy + 1 : iy;
        const uint32_t p00 = src.pixels[static_cast<uint64_t>(iy) * src.pitch + static_cast<uint64_t>(ix)], p10 = src.pixels[static_cast<uint64_t>(iy) * src.pitch + static_cast<uint64_t>(ix1)];
        const uint32_t p01 = src.pixels[static_cast<uint64_t>(iy1) * src.pitch + static_cast<uint64_t>(ix)], p11 = src.pixels[static_cast<uint64_t>(iy1) * src.pitch + static_cast<uint64_t>(ix1)];
        if (p00 == p10 && p00 == p01 && p00 == p11) {
          dst[uint64_t{j} * w + i] = p00;
          continue;
        }
        const double w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
        const double a00 = (p00 >> 24) * w00, a10 = (p10 >> 24) * w10, a01 = (p01 >> 24) * w01, a11 = (p11 >> 24) * w11;
        const double a = a00 + a10 + a01 + a11;
        if (a < 0.5) {
          dst[uint64_t{j} * w + i] = 0;
          continue;
        }
        uint32_t out = static_cast<uint32_t>(a + 0.5) << 24;
        for (int shift = 16; shift >= 0; shift -= 8) {
          const double c = (((p00 >> shift) & 0xFF) * a00 + ((p10 >> shift) & 0xFF) * a10 + ((p01 >> shift) & 0xFF) * a01 + ((p11 >> shift) & 0xFF) * a11) / a;
          out |= static_cast<uint32_t>(c > 255 ? 255 : c + 0.5) << shift;
        }
        dst[uint64_t{j} * w + i] = out;
        continue;
      }
      uint64_t sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
      for (uint64_t y = y0; y < y1 && y < src.height; ++y)
        for (uint64_t x = x0; x < x1 && x < src.width; ++x) {
          const uint32_t p = src.pixels[y * src.pitch + x];
          const uint32_t a = p >> 24;
          sa += a;
          sr += ((p >> 16) & 0xFF) * a;
          sg += ((p >> 8) & 0xFF) * a;
          sb += (p & 0xFF) * a;
          ++n;
        }
      if (n == 0 || sa == 0) {
        dst[uint64_t{j} * w + i] = 0;
        continue;
      }
      dst[uint64_t{j} * w + i] = (static_cast<uint32_t>((sa + n / 2) / n) << 24) | (static_cast<uint32_t>(sr / sa) << 16) | (static_cast<uint32_t>(sg / sa) << 8) | static_cast<uint32_t>(sb / sa);
    }
  }
  return true;
}

void format_time(double seconds, String* out) {
  const uint64_t s = seconds > 0 ? static_cast<uint64_t>(seconds) : 0;
  out->append_uint(s / 60);
  out->push_back(':');
  if (s % 60 < 10) out->push_back('0');
  out->append_uint(s % 60);
}

}  // namespace

bool decode_data_url(allocators::IAllocator& allocator, StringView url, ByteBuffer* out, String* media_type) {
  (void)allocator;
  if (!url.starts_with("data:")) return false;
  const uint64_t comma = url.find(',');
  if (comma == StringView::npos) return false;
  StringView head = url.substr(5, comma - 5);
  const StringView data = url.substr(comma + 1);
  bool base64 = false;
  const uint64_t semi = head.find(';');
  StringView type = semi == StringView::npos ? head : head.first(semi);
  if (semi != StringView::npos) {
    const StringView params = head.substr(semi + 1);
    base64 = params.equals_ignore_case("base64") || params.ends_with(";base64") || params.contains(";base64;") || params.starts_with("base64");
  }
  if (media_type) media_type->assign(type.empty() ? StringView("text/plain") : type);
  out->clear();
  if (base64) {
    uint32_t acc = 0;
    int bits = 0;
    for (uint64_t i = 0; i < data.size(); ++i) {
      char c = data[i];
      if (c == '=' || is_space(c)) continue;
      if (c == '%' && i + 2 < data.size() + 0 && hex_value(data[i + 1]) >= 0 && hex_value(data[i + 2]) >= 0) {
        c = static_cast<char>(hex_value(data[i + 1]) * 16 + hex_value(data[i + 2]));
        i += 2;
      }
      const int v = base64_value(c);
      if (v < 0) return false;
      acc = (acc << 6) | static_cast<uint32_t>(v);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        if (!out->put8(static_cast<uint8_t>(acc >> bits))) return false;
        acc &= (1u << bits) - 1;
      }
    }
    return true;
  }
  for (uint64_t i = 0; i < data.size(); ++i) {
    uint8_t c = static_cast<uint8_t>(data[i]);
    if (c == '%' && i + 2 < data.size() + 0 && hex_value(data[i + 1]) >= 0 && hex_value(data[i + 2]) >= 0) {
      c = static_cast<uint8_t>(hex_value(data[i + 1]) * 16 + hex_value(data[i + 2]));
      i += 2;
    }
    if (!out->put8(c)) return false;
  }
  return true;
}

PageObjects::PageObjects(allocators::IAllocator& allocator)
    : allocator_(allocator), cache_(allocator), images_(allocator), videos_(allocator), canvases_(allocator), svgs_(allocator), events_(allocator) {}

PageObjects::~PageObjects() {
  for (uint64_t i = 0; i < images_.count(); ++i) release_scaled(&images_[i]->scaled);
  for (uint64_t i = 0; i < videos_.count(); ++i) {
    release_scaled(&videos_[i]->scaled);
    release_scaled(&videos_[i]->poster_scaled);
    release_view(allocator_, videos_[i]->picture);
  }
}

void PageObjects::release_scaled(Scaled* s) {
  release_view(allocator_, s->pixels);
  s->width = s->height = 0;
  s->frame = 0xFFFFFFFFu;
}

PageObjects::ImageSlot* PageObjects::find_image(uint32_t serial) {
  for (uint64_t i = 0; i < images_.count(); ++i)
    if (images_[i]->serial == serial) return images_[i].get();
  return nullptr;
}
PageObjects::SvgSlot* PageObjects::find_svg(uint32_t serial) {
  for (uint64_t i = 0; i < svgs_.count(); ++i)
    if (svgs_[i]->serial == serial) return svgs_[i].get();
  return nullptr;
}

// An <svg> of the page: drawn again whenever the document has changed.
void PageObjects::sync_svg(dom::Document& doc, dom::Node* n) {
  for (const dom::Node* p = n->parent; p; p = p->parent)
    if (p->is_tag("svg")) return;   // (inside another svg: that one draws it)
  if (!dom::is_connected(n)) return;
  SvgSlot* slot = find_svg(n->serial);
  if (!slot) {
    auto fresh = UniquePointer<SvgSlot>::make(allocator_);
    if (!fresh || !svgs_.push_back(util::move(fresh))) return;
    slot = svgs_[svgs_.count() - 1].get();
    slot->serial = n->serial;
  }
  slot->seen = true;
  if (slot->version == doc.version() && slot->bitmap) return;
  uint32_t w, h;
  svg_natural_size(n, &w, &h);
  if (uint64_t{w} * h > 4u << 20) return;
  auto bitmap = UniquePointer<CanvasBitmap>::make(allocator_, allocator_, w, h);
  if (!bitmap || !bitmap->valid()) return;
  draw_svg(n, bitmap.get());
  slot->bitmap = util::move(bitmap);
  slot->width = w;
  slot->height = h;
  slot->version = doc.version();
  dirty_ = true;
}

PageObjects::VideoSlot* PageObjects::find_video(uint32_t serial) {
  for (uint64_t i = 0; i < videos_.count(); ++i)
    if (videos_[i]->serial == serial) return videos_[i].get();
  return nullptr;
}
PageObjects::CanvasSlot* PageObjects::find_canvas(uint32_t serial) {
  for (uint64_t i = 0; i < canvases_.count(); ++i)
    if (canvases_[i]->serial == serial) return canvases_[i].get();
  return nullptr;
}

void PageObjects::queue(uint32_t serial, const char* type) {
  if (events_.count() < 512) events_.push_back(Event{serial, type});
}

bool PageObjects::take_event(Event* out) {
  Lock lock(*this);
  if (events_.count() == 0) return false;
  *out = events_[0];
  events_.remove_at(0);
  return true;
}

bool PageObjects::take_dirty() {
  Lock lock(*this);
  const bool d = dirty_;
  dirty_ = false;
  return d;
}

// --- Choosing and loading a picture --------------------------------------------------------------------------------------------

bool PageObjects::choose_source(const dom::Node* img, const MediaEnvironment& media, String* out) {
  const dom::Node* parent = img->parent;
  if (parent && parent->is_tag("picture")) {
    for (const dom::Node* c = parent->first_child; c && c != img; c = c->next) {
      if (!c->is_tag("source")) continue;
      StringView m;
      if (dom::get_attribute(c, "media", &m) && !media_matches(m, media)) continue;
      StringView type;
      if (dom::get_attribute(c, "type", &type) && !supported_image_type(type)) continue;
      const StringView set = attr(c, "srcset");
      if (!set.empty() && pick_srcset(set, media.width, out)) return true;
      const StringView src = attr(c, "src");
      if (!src.empty()) {
        out->assign(src);
        return true;
      }
    }
  }
  const StringView set = attr(img, "srcset");
  if (!set.empty() && pick_srcset(set, media.width, out)) return true;
  const StringView src = attr(img, "src");
  if (src.trimmed().empty()) return false;
  out->assign(src.trimmed());
  return true;
}

// The decoded picture at an address (fetched if it was not yet). The index in cache_, or -1.
int32_t PageObjects::load_image_url(IObjectEnvironment& env, StringView url, bool* failed) {
  *failed = false;
  for (uint64_t i = 0; i < cache_.count(); ++i)
    if (cache_[i]->url.view().equals(url)) {
      *failed = !cache_[i]->ok;
      return cache_[i]->ok ? static_cast<int32_t>(i) : -1;
    }
  if (fetched_images_ >= kMaxImages * 2) return -1;
  ++fetched_images_;
  auto entry = UniquePointer<CachedImage>::make(allocator_, allocator_);
  if (!entry) return -1;
  entry->url.assign(url);
  ByteBuffer bytes(allocator_);
  bool got;
  if (url.starts_with("data:")) {
    String type(allocator_);
    got = decode_data_url(allocator_, url, &bytes, &type);
  } else {
    got = env.fetch_bytes(url, kMaxImageBytes, &bytes);
  }
  bool decoded = got && media::decode_image(bytes.view(), &entry->image);
  if (!decoded && got) decoded = rasterize_svg(allocator_, StringView(bytes.view()), &entry->image);   // (an SVG file is drawn once, at its own size)
  if (decoded) {
    const uint64_t pixels = uint64_t{entry->image.width()} * entry->image.height();
    if (pixels > kMaxImagePixels) {
      entry->image.clear();
    } else {
      while (entry->image.frame_count() > 1 && pixels * entry->image.frame_count() > kMaxImagePixels) entry->image.limit_frames(entry->image.frame_count() - 1);
      entry->ok = !entry->image.empty();
    }
  }
  const bool ok = entry->ok;
  if (!cache_.push_back(util::move(entry))) return -1;
  *failed = !ok;
  return ok ? static_cast<int32_t>(cache_.count() - 1) : -1;
}

void PageObjects::sync_image(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched) {
  (void)fetched;
  ImageSlot* slot = find_image(n->serial);
  String candidate(allocator_);
  if (!choose_source(n, media, &candidate)) {
    if (slot) {
      slot->url.clear();
      slot->state = 0;
      slot->cache = -1;
      slot->announced = false;
      release_scaled(&slot->scaled);
      slot->seen = true;
    }
    return;
  }
  String absolute(allocator_);
  bool resolved = true;
  if (candidate.view().starts_with("data:")) absolute.assign(candidate.view());
  else resolved = resolve_reference(allocator_, base, candidate.view(), &absolute);
  if (!slot) {
    auto fresh = UniquePointer<ImageSlot>::make(allocator_, allocator_);
    if (!fresh || !images_.push_back(util::move(fresh))) return;
    slot = images_[images_.count() - 1].get();
    slot->serial = n->serial;
  }
  slot->seen = true;
  if (resolved && slot->url.view().equals(absolute.view()) && slot->state != 0) return;
  if (!resolved) {
    slot->url.assign(candidate.view());
    slot->state = 2;
    slot->announced = true;
    queue(n->serial, "error");
    return;
  }
  slot->url.assign(absolute.view());
  release_scaled(&slot->scaled);
  slot->announced = false;
  bool failed;
  const int32_t idx = load_image_url(env, absolute.view(), &failed);
  if (idx >= 0) {
    slot->cache = idx;
    slot->state = 1;
    if (cache_[static_cast<uint64_t>(idx)]->started_ms == 0) cache_[static_cast<uint64_t>(idx)]->started_ms = now_ms_ ? now_ms_ : 1;
    slot->announced = true;
    queue(n->serial, "load");
    dirty_ = true;
  } else if (failed) {
    slot->cache = -1;
    slot->state = 2;
    slot->announced = true;
    queue(n->serial, "error");
  }
}

// --- Videos -----------------------------------------------------------------------------------------------------------------------

void PageObjects::sync_video(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched) {
  (void)media;
  VideoSlot* slot = find_video(n->serial);
  if (!slot) {
    if (videos_.count() >= kMaxVideos) return;
    auto fresh = UniquePointer<VideoSlot>::make(allocator_, allocator_);
    if (!fresh || !videos_.push_back(util::move(fresh))) return;
    slot = videos_[videos_.count() - 1].get();
    slot->serial = n->serial;
  }
  slot->seen = true;
  slot->audio = n->is_tag("audio");
  slot->controls = dom::has_attribute(n, "controls");
  slot->loop = dom::has_attribute(n, "loop");
  slot->autoplay = dom::has_attribute(n, "autoplay");
  if (dom::has_attribute(n, "muted")) slot->muted = slot->muted || slot->state == 0;
  // the poster
  const StringView poster = slot->audio ? StringView() : attr(n, "poster").trimmed();
  if (poster.empty()) {
    slot->poster.clear();
    slot->poster_cache = -1;
  } else {
    String abs(allocator_);
    if (poster.starts_with("data:") ? abs.assign(poster) : resolve_reference(allocator_, base, poster, &abs)) {
      if (!slot->poster.view().equals(abs.view())) {
        slot->poster.assign(abs.view());
        bool failed;
        slot->poster_cache = load_image_url(env, abs.view(), &failed);
        release_scaled(&slot->poster_scaled);
        dirty_ = true;
      }
    }
  }
  // the file: src, or the first <source> that is a type that can be played
  String candidate(allocator_);
  StringView src = attr(n, "src").trimmed();
  if (!src.empty()) {
    candidate.assign(src);
  } else {
    for (const dom::Node* c = n->first_child; c; c = c->next) {
      if (!c->is_tag("source")) continue;
      const StringView s = attr(c, "src").trimmed();
      if (s.empty() || !(slot->audio ? supported_audio_type(attr(c, "type")) : supported_video_type(attr(c, "type")))) continue;
      candidate.assign(s);
      break;
    }
  }
  if (candidate.empty()) {
    if (slot->state != 0 || slot->clip) {
      slot->clip = UniquePointer<IVideoClip>();
      slot->state = 0;
      slot->url.clear();
      slot->have_picture = false;
    }
    return;
  }
  String absolute(allocator_);
  const bool resolved = candidate.view().starts_with("data:") ? absolute.assign(candidate.view()) : resolve_reference(allocator_, base, candidate.view(), &absolute);
  if (resolved && slot->url.view().equals(absolute.view()) && slot->state != 0) {
    if (slot->clip) slot->clip->set_looping(slot->loop);
    return;
  }
  if (*fetched >= 4) return;   // (a few big downloads per drawing of the page)
  ++*fetched;
  slot->url.assign(resolved ? absolute.view() : candidate.view());
  slot->clip = UniquePointer<IVideoClip>();
  slot->have_picture = false;
  slot->picture_serial = 0xFFFFFFFFu;
  release_scaled(&slot->scaled);
  if (!resolved) {
    slot->state = 2;
    queue(n->serial, "error");
    return;
  }
  auto bytes = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  bool got = false;
  if (bytes) {
    if (absolute.view().starts_with("data:")) {
      String type(allocator_);
      got = decode_data_url(allocator_, absolute.view(), bytes.get(), &type);
    } else {
      got = env.fetch_bytes(absolute.view(), kMaxVideoBytes, bytes.get());
    }
  }
  if (got) slot->clip = slot->audio ? env.open_audio(util::move(bytes)) : env.open_video(util::move(bytes));
  if (!slot->clip) {
    slot->state = 2;
    queue(n->serial, "error");
    dirty_ = true;
    return;
  }
  slot->state = 1;
  slot->ended = false;
  slot->clip->set_looping(slot->loop);
  slot->clip->set_volume(slot->volume, slot->muted);
  if (!slot->audio) slot->picture = allocate_view<uint32_t>(allocator_, uint64_t{slot->clip->width() ? slot->clip->width() : 1} * (slot->clip->height() ? slot->clip->height() : 1));
  queue(n->serial, "loadstart");
  queue(n->serial, "loadedmetadata");
  queue(n->serial, "loadeddata");
  queue(n->serial, "canplay");
  if (slot->autoplay || slot->wants_play) {
    if (slot->clip->play()) {
      slot->was_playing = true;
      queue(n->serial, "play");
      queue(n->serial, "playing");
    }
  }
  dirty_ = true;
}

// --- The page's side -------------------------------------------------------------------------------------------------------------

void PageObjects::sync(dom::Document& doc, IObjectEnvironment& env, StringView base, const MediaEnvironment& media) {
  Lock lock(*this);
  for (uint64_t i = 0; i < images_.count(); ++i) images_[i]->seen = false;
  for (uint64_t i = 0; i < videos_.count(); ++i) videos_[i]->seen = false;
  for (uint64_t i = 0; i < svgs_.count(); ++i) svgs_[i]->seen = false;
  uint32_t video_fetches = 0;
  for (uint32_t serial = 1; serial <= doc.node_count(); ++serial) {
    dom::Node* n = doc.by_serial(serial);
    if (!n || !n->is_element()) continue;
    if (n->is_tag("img")) sync_image(n, env, base, media, nullptr);
    else if (n->is_tag("video") || n->is_tag("audio")) sync_video(n, env, base, media, &video_fetches);
    else if (n->is_tag("svg")) sync_svg(doc, n);
  }
  for (uint64_t i = svgs_.count(); i > 0; --i)
    if (!svgs_[i - 1]->seen) svgs_.remove_at(i - 1);
  for (uint64_t i = images_.count(); i > 0; --i) {
    if (images_[i - 1]->seen) continue;
    release_scaled(&images_[i - 1]->scaled);
    images_.remove_at(i - 1);
  }
  for (uint64_t i = videos_.count(); i > 0; --i) {
    if (videos_[i - 1]->seen) continue;
    release_scaled(&videos_[i - 1]->scaled);
    release_scaled(&videos_[i - 1]->poster_scaled);
    release_view(allocator_, videos_[i - 1]->picture);
    videos_.remove_at(i - 1);
  }
}

uint32_t PageObjects::image_frame(const CachedImage& c) const {
  if (c.image.frame_count() <= 1) return 0;
  const uint64_t total = c.image.total_duration_ms();
  if (total == 0) return 0;
  uint64_t t = now_ms_ > c.started_ms ? now_ms_ - c.started_ms : 0;
  const uint32_t loops = c.image.loop_count();
  if (loops != 0 && t >= total * loops) return c.image.frame_count() - 1;
  t %= total;
  for (uint32_t f = 0; f < c.image.frame_count(); ++f) {
    const uint32_t d = c.image.delay_ms(f) ? c.image.delay_ms(f) : 100;
    if (t < d) return f;
    t -= d;
  }
  return c.image.frame_count() - 1;
}

void PageObjects::update(uint64_t now_ms) {
  Lock lock(*this);
  now_ms_ = now_ms;
  for (uint64_t i = 0; i < images_.count(); ++i) {
    ImageSlot& s = *images_[i];
    if (s.state != 1 || s.cache < 0) continue;
    const CachedImage& c = *cache_[static_cast<uint64_t>(s.cache)];
    if (c.image.frame_count() > 1 && image_frame(c) != s.scaled.frame) dirty_ = true;
  }
  for (uint64_t i = 0; i < videos_.count(); ++i) {
    VideoSlot& v = *videos_[i];
    if (!v.clip) continue;
    v.clip->update();
    const bool playing = v.clip->playing();
    if (playing) {
      dirty_ = true;
      if (now_ms >= v.last_timeupdate_ms + 250) {
        v.last_timeupdate_ms = now_ms;
        queue(v.serial, "timeupdate");
      }
    }
    if (v.was_playing && !playing) {
      // it stopped by itself: the end
      const int64_t duration = v.clip->duration_us();
      if (duration <= 0 || v.clip->position_us() >= duration - 150000) {
        v.ended = true;
        v.wants_play = false;
        queue(v.serial, "timeupdate");
        queue(v.serial, "pause");
        queue(v.serial, "ended");
        dirty_ = true;
      }
    }
    v.was_playing = playing;
  }
}

bool PageObjects::animating() {
  Lock lock(*this);
  for (uint64_t i = 0; i < videos_.count(); ++i)
    if (videos_[i]->clip && videos_[i]->clip->playing()) return true;
  for (uint64_t i = 0; i < images_.count(); ++i)
    if (images_[i]->state == 1 && images_[i]->cache >= 0 && cache_[static_cast<uint64_t>(images_[i]->cache)]->image.frame_count() > 1) return true;
  return false;
}

// --- What the renderer asks ------------------------------------------------------------------------------------------------------

bool PageObjects::object_info(const dom::Node* element, ObjectInfo* out) {
  *out = ObjectInfo();
  if (!element->is_element()) return false;
  Lock lock(*this);
  if (element->is_tag("img")) {
    out->kind = ObjectKind::Image;
    const ImageSlot* s = find_image(element->serial);
    if (s && s->state == 1 && s->cache >= 0) {
      const media::Image& im = cache_[static_cast<uint64_t>(s->cache)]->image;
      out->width = im.width();
      out->height = im.height();
      out->ready = true;
    }
    return true;
  }
  if (element->is_tag("video")) {
    out->kind = ObjectKind::Video;
    out->ready = true;
    const VideoSlot* v = find_video(element->serial);
    if (v && v->clip && v->state == 1) {
      out->width = v->clip->width();
      out->height = v->clip->height();
    } else if (v && v->poster_cache >= 0) {
      const media::Image& im = cache_[static_cast<uint64_t>(v->poster_cache)]->image;
      out->width = im.width();
      out->height = im.height();
    } else {
      out->width = 300;
      out->height = 150;
    }
    return true;
  }
  if (element->is_tag("svg")) {
    out->kind = ObjectKind::Svg;
    out->ready = true;
    svg_natural_size(element, &out->width, &out->height);
    return true;
  }
  if (element->is_tag("audio")) {   // (without controls there is nothing to show)
    if (!dom::has_attribute(element, "controls")) return false;
    out->kind = ObjectKind::Audio;
    out->ready = true;
    out->width = 300;
    out->height = 32;
    return true;
  }
  if (element->is_tag("canvas")) {
    out->kind = ObjectKind::Canvas;
    out->ready = true;
    bool ok;
    out->width = 300;
    out->height = 150;
    StringView w = attr(element, "width").trimmed(), h = attr(element, "height").trimmed();
    uint64_t v;
    if (!w.empty() && w.parse_uint(&v) && v > 0 && v <= CanvasBitmap::kMaxDimension) out->width = static_cast<uint32_t>(v);
    if (!h.empty() && h.parse_uint(&v) && v > 0 && v <= CanvasBitmap::kMaxDimension) out->height = static_cast<uint32_t>(v);
    (void)ok;
    return true;
  }
  return false;
}

// --- Painting ---------------------------------------------------------------------------------------------------------------------

// Draws a picture w x h at (x, y), scaled from `src` (a copy at that size is kept until the picture, the size or the frame changes).
void PageObjects::draw_scaled(graphics::IRenderer2D& r, Scaled& cache, const ImageSource& src, uint32_t frame, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip,
                              bool fit) {
  (void)fit;
  if (!src.valid() || w == 0 || h == 0) return;
  const int64_t vx0 = x > clip.x ? x : clip.x, vy0 = y > clip.y ? y : clip.y;
  const int64_t vx1 = int64_t{x} + w < int64_t{clip.x} + clip.width ? int64_t{x} + w : int64_t{clip.x} + clip.width;
  const int64_t vy1 = int64_t{y} + h < int64_t{clip.y} + clip.height ? int64_t{y} + h : int64_t{clip.y} + clip.height;
  if (vx1 <= vx0 || vy1 <= vy0) return;
  graphics::Surface s;
  if (src.width == w && src.height == h) {
    // (the surface type is mutable; drawing from it only reads)
    s.pixels = ArrayView<uint32_t>(const_cast<uint32_t*>(src.pixels.data()), src.pixels.size());
    s.pitch = src.pitch;
  } else {
    if (cache.pixels.empty() || cache.width != w || cache.height != h || cache.frame != frame) {
      release_scaled(&cache);
      if (uint64_t{w} * h > (64u << 20)) return;
      cache.pixels = allocate_view<uint32_t>(allocator_, uint64_t{w} * h);
      if (cache.pixels.empty()) return;
      if (!scale_pixels(src, w, h, cache.pixels)) {
        release_scaled(&cache);
        return;
      }
      cache.width = w;
      cache.height = h;
      cache.frame = frame;
    }
    s.pixels = cache.pixels;
    s.pitch = w;
  }
  s.width = w;
  s.height = h;
  r.set_blend(graphics::BlendMode::Alpha);
  r.draw_image(s, {static_cast<int32_t>(vx0 - x), static_cast<int32_t>(vy0 - y), static_cast<uint32_t>(vx1 - vx0), static_cast<uint32_t>(vy1 - vy0)}, static_cast<int32_t>(vx0),
               static_cast<int32_t>(vy0));
  r.set_clip(clip);
}

void PageObjects::draw_video(graphics::IRenderer2D& r, VideoSlot& v, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) {
  r.set_blend(graphics::BlendMode::Alpha);
  const int64_t vx0 = x > clip.x ? x : clip.x, vy0 = y > clip.y ? y : clip.y;
  const int64_t vx1 = int64_t{x} + w < int64_t{clip.x} + clip.width ? int64_t{x} + w : int64_t{clip.x} + clip.width;
  const int64_t vy1 = int64_t{y} + h < int64_t{clip.y} + clip.height ? int64_t{y} + h : int64_t{clip.y} + clip.height;
  if (vx1 <= vx0 || vy1 <= vy0) return;
  r.fill_rect({static_cast<int32_t>(vx0), static_cast<int32_t>(vy0), static_cast<uint32_t>(vx1 - vx0), static_cast<uint32_t>(vy1 - vy0)}, 0xFF000000u);
  auto fit_draw = [&](Scaled& cache, const ImageSource& src, uint32_t frame) {
    uint32_t dw = w, dh = static_cast<uint32_t>(uint64_t{src.height} * w / src.width);
    if (dh > h) {
      dh = h;
      dw = static_cast<uint32_t>(uint64_t{src.width} * h / src.height);
    }
    if (dw == 0) dw = 1;
    if (dh == 0) dh = 1;
    draw_scaled(r, cache, src, frame, x + static_cast<int32_t>((w - dw) / 2), y + static_cast<int32_t>((h - dh) / 2), dw, dh, clip, true);
  };
  bool drawn = v.audio;   // (no picture to look for)
  if (!v.audio && v.clip && v.state == 1 && !v.picture.empty()) {
    uint32_t serial = 0;
    const uint32_t cw = v.clip->width();
    if (cw && v.clip->copy_picture(v.picture, cw, &serial)) {
      v.have_picture = true;
      v.picture_serial = serial;
    }
    if (v.have_picture) {
      ImageSource src;
      src.pixels = v.picture;
      src.width = cw;
      src.height = v.clip->height();
      src.pitch = cw;
      fit_draw(v.scaled, src, v.picture_serial);
      drawn = true;
    }
  }
  if (!drawn && v.poster_cache >= 0) {
    const CachedImage& c = *cache_[static_cast<uint64_t>(v.poster_cache)];
    const ImageSource src = {c.image.frame(0), c.image.width(), c.image.height(), c.image.width()};
    fit_draw(v.poster_scaled, src, 0);
    drawn = true;
  }
  if (!drawn) {
    const char* message = v.state == 2 ? "This video cannot be played" : v.state == 0 && v.url.empty() ? "No video" : "Loading video...";
    const int32_t tw = static_cast<int32_t>(StringView(message).size()) * 8;
    if (static_cast<int32_t>(w) > tw && h >= 16) r.draw_text(x + (static_cast<int32_t>(w) - tw) / 2, y + (static_cast<int32_t>(h) - 16) / 2, message, 0xFFB0B0B0u);
  }
  if (v.controls && w >= 80 && (v.audio ? h >= 24 : h >= 40)) {
    const int32_t bar = v.audio ? static_cast<int32_t>(h - 24) / 2 : static_cast<int32_t>(h) - 24;
    r.fill_rect(v.audio ? Rect2D{x, y, w, h} : Rect2D{x, y + bar, w, 24}, v.audio ? 0xFF202020u : 0xC0101010u);
    const bool playing = v.clip && v.clip->playing();
    if (playing) {
      r.fill_rect({x + 9, y + bar + 6, 4, 12}, 0xFFFFFFFFu);
      r.fill_rect({x + 16, y + bar + 6, 4, 12}, 0xFFFFFFFFu);
    } else {
      r.fill_triangle({static_cast<float>(x + 9), static_cast<float>(y + bar + 6)}, {static_cast<float>(x + 9), static_cast<float>(y + bar + 18)},
                      {static_cast<float>(x + 21), static_cast<float>(y + bar + 12)}, 0xFFFFFFFFu);
    }
    String text(allocator_);
    const double duration = v.clip ? static_cast<double>(v.clip->duration_us()) / 1e6 : 0;
    const double current = v.clip ? static_cast<double>(v.clip->position_us()) / 1e6 : 0;
    format_time(current, &text);
    text.append(" / ");
    format_time(duration, &text);
    const int32_t text_w = static_cast<int32_t>(text.size()) * 8;
    const bool show_time = static_cast<int32_t>(w) > 32 + text_w + 40;
    const int32_t track_x = x + 32, track_w = static_cast<int32_t>(w) - 40 - (show_time ? text_w + 8 : 0);
    if (track_w > 8) {
      r.fill_rect({track_x, y + bar + 11, static_cast<uint32_t>(track_w), 2}, 0xFF707070u);
      const double fraction = duration > 0 ? (current > duration ? 1 : current / duration) : 0;
      const int32_t played = static_cast<int32_t>(fraction * track_w);
      if (played > 0) r.fill_rect({track_x, y + bar + 11, static_cast<uint32_t>(played), 2}, 0xFFFFFFFFu);
      r.fill_rect({track_x + played - 2, y + bar + 7, 5, 10}, 0xFFFFFFFFu);
    }
    if (show_time) r.draw_text(x + static_cast<int32_t>(w) - text_w - 6, y + bar + 4, text.data(), 0xFFFFFFFFu);
  }
  r.set_clip(clip);
}

void PageObjects::paint_object(graphics::IRenderer2D& r, uint32_t node, ObjectKind kind, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) {
  Lock lock(*this);
  accelerated_ = r.accelerated();
  switch (kind) {
    case ObjectKind::Image: {
      ImageSlot* s = find_image(node);
      if (!s || s->state != 1 || s->cache < 0) return;
      CachedImage& c = *cache_[static_cast<uint64_t>(s->cache)];
      const uint32_t frame = image_frame(c);
      const ArrayView<uint32_t> px = c.image.frame(frame);
      const ImageSource src = {px, c.image.width(), c.image.height(), c.image.width()};
      draw_scaled(r, s->scaled, src, frame, x, y, w, h, clip, false);
      break;
    }
    case ObjectKind::Video:
    case ObjectKind::Audio: {
      VideoSlot* v = find_video(node);
      if (v) draw_video(r, *v, x, y, w, h, clip);
      break;
    }
    case ObjectKind::Svg: {
      SvgSlot* s = find_svg(node);
      if (s && s->bitmap) s->bitmap->paint(r, x, y, w, h, clip);
      break;
    }
    case ObjectKind::Canvas: {
      CanvasSlot* c = find_canvas(node);
      if (c && c->bitmap) c->bitmap->paint(r, x, y, w, h, clip);
      break;
    }
    default: break;
  }
}

bool PageObjects::object_clicked(uint32_t serial, int32_t lx, int32_t ly, uint32_t w, uint32_t h) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v || !v->controls || !v->clip) return false;
  const bool on_bar = v->audio ? h >= 24 : h >= 40 && ly >= static_cast<int32_t>(h) - 24;
  if (on_bar && lx >= 32) {
    const int32_t track_w = static_cast<int32_t>(w) - 40 - 
        (static_cast<int32_t>(w) > 32 + 13 * 8 + 40 ? 13 * 8 + 8 : 0);
    if (track_w > 8 && lx <= 32 + track_w) {
      const double fraction = static_cast<double>(lx - 32) / track_w;
      const double duration = static_cast<double>(v->clip->duration_us());
      v->clip->seek(static_cast<int64_t>(fraction * duration));
      v->ended = false;
      queue(serial, "seeking");
      queue(serial, "seeked");
      queue(serial, "timeupdate");
      dirty_ = true;
      return true;
    }
    return false;
  }
  if (v->clip->playing()) {
    v->clip->pause();
    v->was_playing = false;
    v->wants_play = false;
    queue(serial, "pause");
  } else {
    if (v->ended) v->clip->seek(0);
    v->ended = false;
    if (v->clip->play()) {
      v->was_playing = true;
      v->wants_play = true;
      queue(serial, "play");
      queue(serial, "playing");
    }
  }
  dirty_ = true;
  return true;
}

// --- Script side ----------------------------------------------------------------------------------------------------------------

void PageObjects::image_state(uint32_t serial, ImageState* out) {
  Lock lock(*this);
  ImageSlot* s = find_image(serial);
  if (!s) return;
  out->known = true;
  out->current_source.assign(s->url.view());
  if (s->state == 1 && s->cache >= 0) {
    out->complete = true;
    out->width = cache_[static_cast<uint64_t>(s->cache)]->image.width();
    out->height = cache_[static_cast<uint64_t>(s->cache)]->image.height();
  } else if (s->state == 2) {
    out->complete = true;
    out->broken = true;
  } else {
    out->complete = s->url.empty();
  }
}

bool PageObjects::image_pixels(uint32_t serial, ImageSource* out) {
  Lock lock(*this);
  ImageSlot* s = find_image(serial);
  if (!s || s->state != 1 || s->cache < 0) return false;
  const CachedImage& c = *cache_[static_cast<uint64_t>(s->cache)];
  const ArrayView<uint32_t> px = c.image.frame(image_frame(c));
  out->pixels = px;
  out->width = c.image.width();
  out->height = c.image.height();
  out->pitch = c.image.width();
  return true;
}

bool PageObjects::video_pixels(uint32_t serial, ImageSource* out) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v || !v->clip || v->picture.empty()) return false;
  const uint32_t cw = v->clip->width();
  uint32_t picture_serial = 0;
  if (cw && v->clip->copy_picture(v->picture, cw, &picture_serial)) {
    v->have_picture = true;
    v->picture_serial = picture_serial;
  }
  if (!v->have_picture) return false;
  out->pixels = v->picture;
  out->width = cw;
  out->height = v->clip->height();
  out->pitch = cw;
  return true;
}

PageObjects::CanvasSlot* PageObjects::find_canvas_slot(uint32_t serial) { return find_canvas(serial); }

CanvasBitmap* PageObjects::existing_canvas(uint32_t serial) {
  Lock lock(*this);
  CanvasSlot* s = find_canvas(serial);
  return s && s->bitmap ? s->bitmap.get() : nullptr;
}

CanvasBitmap* PageObjects::canvas(uint32_t serial, uint32_t width, uint32_t height) {
  Lock lock(*this);
  CanvasSlot* s = find_canvas(serial);
  if (s && s->bitmap && s->width == width && s->height == height) return s->bitmap.get();
  if (s && s->bitmap) {
    canvas_pixels_ -= uint64_t{s->width} * s->height;
    s->bitmap = UniquePointer<CanvasBitmap>();
  }
  if (canvas_pixels_ + uint64_t{width} * height > kMaxCanvasPixels) return nullptr;
  if (!s) {
    auto fresh = UniquePointer<CanvasSlot>::make(allocator_);
    if (!fresh || !canvases_.push_back(util::move(fresh))) return nullptr;
    s = canvases_[canvases_.count() - 1].get();
    s->serial = serial;
  }
  auto bitmap = UniquePointer<CanvasBitmap>::make(allocator_, allocator_, width, height);
  if (!bitmap || !bitmap->valid()) return nullptr;
  s->bitmap = util::move(bitmap);
  s->width = width;
  s->height = height;
  canvas_pixels_ += uint64_t{width} * height;
  dirty_ = true;
  return s->bitmap.get();
}

void PageObjects::video_state(uint32_t serial, VideoState* out) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  out->known = true;
  out->ready = v->clip && v->state == 1;
  out->loop = v->loop;
  out->muted = v->muted;
  out->volume = v->volume;
  out->ended = v->ended;
  if (out->ready) {
    out->paused = !v->clip->playing();
    out->current_time = static_cast<double>(v->clip->position_us()) / 1e6;
    out->duration = static_cast<double>(v->clip->duration_us()) / 1e6;
    out->width = v->clip->width();
    out->height = v->clip->height();
  } else {
    out->paused = !v->wants_play;
  }
}

bool PageObjects::video_play(uint32_t serial) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return false;
  v->wants_play = true;
  if (!v->clip) return true;   // (plays as soon as it is there)
  if (v->ended) v->clip->seek(0);
  v->ended = false;
  if (!v->clip->playing()) {
    if (!v->clip->play()) return false;
    queue(serial, "play");
    queue(serial, "playing");
  }
  v->was_playing = true;
  dirty_ = true;
  return true;
}

void PageObjects::video_pause(uint32_t serial) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  v->wants_play = false;
  v->was_playing = false;
  if (v->clip && v->clip->playing()) {
    v->clip->pause();
    queue(serial, "pause");
    dirty_ = true;
  }
}

void PageObjects::video_seek(uint32_t serial, double seconds) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v || !v->clip || seconds != seconds) return;
  if (seconds < 0) seconds = 0;
  v->clip->seek(static_cast<int64_t>(seconds * 1e6));
  v->ended = false;
  queue(serial, "seeking");
  queue(serial, "seeked");
  queue(serial, "timeupdate");
  dirty_ = true;
}

void PageObjects::video_set_volume(uint32_t serial, double volume) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  v->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
  if (v->clip) v->clip->set_volume(v->volume, v->muted);
  queue(serial, "volumechange");
}

void PageObjects::video_set_muted(uint32_t serial, bool muted) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  v->muted = muted;
  if (v->clip) v->clip->set_volume(v->volume, v->muted);
  queue(serial, "volumechange");
}

void PageObjects::video_set_loop(uint32_t serial, bool loop) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  v->loop = loop;
  if (v->clip) v->clip->set_looping(loop);
}

void PageObjects::video_load(uint32_t serial) {
  Lock lock(*this);
  VideoSlot* v = find_video(serial);
  if (!v) return;
  v->clip = UniquePointer<IVideoClip>();
  v->state = 0;
  v->url.clear();
  v->have_picture = false;
  v->ended = false;
  dirty_ = true;
}

}  // namespace browser

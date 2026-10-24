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

#include "Desktop/FileProperties.hpp"
#include "Desktop/FileByteSource.hpp"
#include "Desktop/FileStatus.hpp"
#include "Desktop/FileTransfer.hpp"
#include "Desktop/Format.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/ImageCodecs.hpp"
#include "Media/Mp4.hpp"
#include "Media/VideoFiles.hpp"
#include "Media/Webm.hpp"
#include "Utility/Move.hpp"
#include "Video/AviFile.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

namespace {

// "1 234 567 bytes (1.2 MB)"
void append_size(String* out, uint64_t bytes) {
  String shown(out->allocator());
  out->append_uint(bytes);
  out->append(bytes == 1 ? " byte" : " bytes");
  if (bytes >= 1024 && format_size(bytes, &shown)) {
    out->append(" (");
    out->append(shown.view());
    out->push_back(')');
  }
}

void append_pixels(String* out, uint32_t width, uint32_t height) {
  out->append_uint(width);
  out->append(" x ");
  out->append_uint(height);
  out->append(" pixels");
}

// "1:02.250" or "1:02:03.250"
void append_duration(String* out, uint64_t ms) {
  const uint64_t total_seconds = ms / 1000, hours = total_seconds / 3600, minutes = total_seconds / 60 % 60, seconds = total_seconds % 60;
  auto two = [&](uint64_t v) {
    if (v < 10) out->push_back('0');
    out->append_uint(v);
  };
  if (hours) {
    out->append_uint(hours);
    out->push_back(':');
    two(minutes);
  } else {
    out->append_uint(minutes);
  }
  out->push_back(':');
  two(seconds);
  out->push_back('.');
  const uint64_t fraction = ms % 1000;
  if (fraction < 100) out->push_back('0');
  if (fraction < 10) out->push_back('0');
  out->append_uint(fraction);
}

// A frame rate given as rate / scale, e.g. "29.97 frames per second".
void append_rate(String* out, uint64_t rate, uint64_t scale) {
  if (scale == 0) scale = 1;
  const uint64_t hundredths = rate * 100 / scale;
  out->append_uint(hundredths / 100);
  if (hundredths % 100) {
    out->push_back('.');
    if (hundredths % 100 < 10) out->push_back('0');
    out->append_uint(hundredths % 100);
  }
  out->append(" frames per second");
}

void append_channels(String* out, uint32_t channels) {
  if (channels == 1) out->append("mono");
  else if (channels == 2) out->append("stereo");
  else {
    out->append_uint(channels);
    out->append(" channels");
  }
}

void append_hertz(String* out, uint32_t rate) {
  out->append_uint(rate);
  out->append(" Hz");
}

constexpr uint64_t kHeadBytes = 64 * 1024;
}  // namespace

FileProperties::FileProperties(allocators::IAllocator& allocator)
    : Frame(allocator), path_(allocator), grid_(allocator), buttons_(allocator), copy_button_(allocator), close_button_(allocator), names_(allocator), values_(allocator) {
  set_title("Properties");
  set_bounds({150, 90, 440, 400});
  set_padding(Insets::all(8));
  set_minimize_enabled(true);

  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 30);
  close_button_.set_text("Close");
  close_button_.set_dock(DockStyle::Right);
  close_button_.set_size(90, 0);
  close_button_.on_click(Handler::immediate(&FileProperties::on_close, this));
  copy_button_.set_text("Copy Row");
  copy_button_.set_dock(DockStyle::Left);
  copy_button_.set_size(100, 0);
  copy_button_.on_click({&FileProperties::on_copy, this});
  buttons_.add(&close_button_);
  buttons_.add(&copy_button_);

  grid_.add_column("Property", 130);
  grid_.add_column("Value", 270);
  grid_.set_dock(DockStyle::Fill);
  add(&buttons_);
  add(&grid_);   // (Fill goes last)
}

void FileProperties::on_copy(void* self, Control&) { static_cast<FileProperties*>(self)->grid_.copy_selected_row(); }
void FileProperties::on_close(void* self, Control&) { static_cast<FileProperties*>(self)->close(); }

StringView FileProperties::value_of(StringView name) const {
  for (uint64_t i = 0; i < names_.count(); ++i)
    if (names_[i].view().equals(name)) return values_[i].view();
  return StringView();
}

void FileProperties::clear() {
  names_.clear();
  values_.clear();
  grid_.clear_rows();
}

void FileProperties::add_row(StringView name, StringView value) {
  String n(allocator()), v(allocator());
  if (!n.assign(name) || !v.assign(value) || !names_.push_back(util::move(n))) return;
  if (!values_.push_back(util::move(v))) {
    names_.remove_at(names_.count() - 1);
    return;
  }
  const StringView cells[2] = {name, value};
  grid_.add_row(cells, 2);
}

bool FileProperties::show(StringView path) {
  UiGuard lock(this);
  clear();
  path_.assign(path);
  DirectoryEntry entry;
  const bool root = path.size() <= 1;
  const Status status = !fs_ ? Status::NotFound : root ? Status::Ok : fs_->stat(path, &entry);
  if (status != Status::Ok) {
    add_row("Path", path);
    add_row("Status", fs_ ? StringView(describe_status(status)) : StringView("No storage available."));
    set_title("Properties");
    return false;
  }
  const uint64_t slash = path.rfind('/');
  String name(allocator()), where(allocator());
  if (root) {
    name.assign("/");
  } else {
    name.assign(path.substr(slash + 1));
    where.assign(slash == 0 ? StringView("/") : path.substr(0, slash));
  }
  String title(allocator());
  title.append(name.view());
  title.append(" Properties");
  set_title(title.view());

  const bool folder = root || entry.type == NodeType::Directory;
  add_row("Name", name.view());
  if (!root) add_row("Location", where.view());
  add_row("Path", path);
  add_row("Type", folder ? StringView("Folder") : StringView("File"));
  if (folder) {
    add_folder_contents(path);
  } else {
    String size(allocator());
    append_size(&size, entry.size);
    add_row("Size", size.view());
  }
  add_row("Permissions", entry.read_only ? StringView("Read-only") : StringView("Read and write"));
  if (!folder) add_media(path, entry.size);
  return true;
}

void FileProperties::add_folder_contents(StringView path) {
  FileTransfer transfer(allocator(), *fs_);
  const Status status = transfer.measure(path);
  String text(allocator());
  if (status != Status::Ok) {
    add_row("Contents", describe_status(status));
    return;
  }
  text.append_uint(transfer.files_total());
  text.append(transfer.files_total() == 1 ? " file" : " files");
  add_row("Contents", text.view());
  text.clear();
  append_size(&text, transfer.bytes_total());
  add_row("Size", text.view());
}

// Reads the whole file when it is small enough.
bool FileProperties::read_whole(StringView path, uint64_t size, ByteBuffer* out) {
  if (size == 0 || size > kMaxInspectBytes) return false;
  UniquePointer<filesystem::IFile> file;
  if (fs_->open_file(path, OpenMode::Read, &file) != Status::Ok) return false;
  uint8_t chunk[4096];
  for (uint64_t at = 0; at < size;) {
    uint64_t got = 0;
    if (file->read(at, ByteView(chunk, sizeof(chunk)), &got) != Status::Ok || got == 0) break;
    if (!out->append(ConstByteView(chunk, got))) return false;
    at += got;
  }
  return out->size() == size;
}

// Finds out what the contents are and adds what is known about that kind of file.
void FileProperties::add_media(StringView path, uint64_t size) {
  if (size == 0) return;
  ByteBuffer head(allocator());
  {
    UniquePointer<filesystem::IFile> file;
    if (fs_->open_file(path, OpenMode::Read, &file) != Status::Ok) return;
    const uint64_t want = size < kHeadBytes ? size : kHeadBytes;
    if (!head.put_zeros(want)) return;
    uint64_t got = 0;
    if (file->read(0, head.writable_view(), &got) != Status::Ok) return;
    head.truncate(got);
  }
  const ConstByteView bytes = head.view();
  if (media::detect_image_format(bytes) != media::ImageFormat::Unknown) {
    ByteBuffer all(allocator());
    if (read_whole(path, size, &all)) add_image(all.view());
    else add_row("Format", media::image_format_name(media::detect_image_format(bytes)));
  } else if (media::detect_video_file(bytes) != media::VideoFileFormat::Unknown) {
    add_video(path, bytes, size);
  } else if (media::detect_audio_format(bytes) != media::AudioFileFormat::Unknown) {
    ByteBuffer all(allocator());
    if (read_whole(path, size, &all)) add_audio(all.view());
    else add_row("Format", media::audio_format_name(media::detect_audio_format(bytes)));
  }
}

void FileProperties::add_image(ConstByteView data) {
  media::Image image(allocator());
  media::ImageFormat format = media::ImageFormat::Unknown;
  if (!media::decode_image(data, &image, &format)) {
    add_row("Format", media::image_format_name(media::detect_image_format(data)));
    add_row("Picture", "Cannot be read");
    return;
  }
  String text(allocator());
  add_row("Format", media::image_format_name(format));
  append_pixels(&text, image.width(), image.height());
  add_row("Dimensions", text.view());
  add_row("Transparency", image.has_alpha() ? "Yes" : "No");
  if (image.animated()) {
    text.clear();
    text.append_uint(image.frame_count());
    add_row("Frames", text.view());
    uint64_t ms = 0;
    for (uint32_t i = 0; i < image.frame_count(); ++i) ms += image.delay_ms(i);
    text.clear();
    append_duration(&text, ms);
    add_row("Length", text.view());
  }
}

void FileProperties::add_video(StringView path, ConstByteView head, uint64_t size) {
  const media::VideoFileFormat format = media::detect_video_file(head);
  // Animated pictures and Y4M are decoded whole, like pictures.
  if (format == media::VideoFileFormat::Gif || format == media::VideoFileFormat::Apng || format == media::VideoFileFormat::AnimatedWebp ||
      format == media::VideoFileFormat::Y4m) {
    ByteBuffer all(allocator());
    media::Image image(allocator());
    if (!read_whole(path, size, &all) || !(format == media::VideoFileFormat::Y4m ? media::decode_y4m(all.view(), &image) : media::decode_image(all.view(), &image))) {
      add_row("Format", media::video_file_format_name(format));
      return;
    }
    String text(allocator());
    add_row("Format", media::video_file_format_name(format));
    append_pixels(&text, image.width(), image.height());
    add_row("Dimensions", text.view());
    text.clear();
    text.append_uint(image.frame_count());
    add_row("Frames", text.view());
    uint64_t ms = 0;
    for (uint32_t i = 0; i < image.frame_count(); ++i) ms += image.delay_ms(i);
    text.clear();
    append_duration(&text, ms);
    add_row("Length", text.view());
    return;
  }
  UniquePointer<filesystem::IFile> file;
  if (fs_->open_file(path, OpenMode::Read, &file) != Status::Ok) return;
  FileByteSource source(util::move(file));
  String text(allocator());
  add_row("Format", media::video_file_format_name(format));
  auto add_audio_line = [&](uint32_t rate, uint32_t channels) {
    text.clear();
    append_hertz(&text, rate);
    text.append(", ");
    append_channels(&text, channels);
    add_row("Audio", text.view());
  };
  if (format == media::VideoFileFormat::Avi) {
    video::AviFile avi(allocator());
    if (!avi.open(source)) return;
    append_pixels(&text, avi.video().width, avi.video().height);
    add_row("Dimensions", text.view());
    text.clear();
    append_duration(&text, static_cast<uint64_t>(avi.duration_us() / 1000));
    add_row("Length", text.view());
    text.clear();
    append_rate(&text, avi.video().rate, avi.video().scale);
    add_row("Frame rate", text.view());
    if (avi.audio().present) add_audio_line(avi.audio().format.sample_rate, avi.audio().format.channels);
  } else if (format == media::VideoFileFormat::Mp4) {
    media::Mp4File mp4(allocator());
    if (!mp4.open(source)) return;
    const media::Mp4Track* video = mp4.find(media::Mp4TrackKind::Video);
    const media::Mp4Track* audio = mp4.find(media::Mp4TrackKind::Audio);
    const media::Mp4Track* longest = video ? video : audio;
    if (video) {
      append_pixels(&text, video->width, video->height);
      add_row("Dimensions", text.view());
    }
    if (longest && longest->timescale) {
      text.clear();
      append_duration(&text, longest->duration * 1000 / longest->timescale);
      add_row("Length", text.view());
    }
    if (audio) add_audio_line(audio->sample_rate, audio->channels);
  } else if (format == media::VideoFileFormat::Webm) {
    media::WebmFile webm(allocator());
    if (!webm.open(source)) return;
    const media::WebmTrack* video = webm.find(media::WebmTrackKind::Video);
    const media::WebmTrack* audio = webm.find(media::WebmTrackKind::Audio);
    if (video) {
      append_pixels(&text, video->display_width ? video->display_width : video->width, video->display_height ? video->display_height : video->height);
      add_row("Dimensions", text.view());
    }
    text.clear();
    append_duration(&text, webm.duration_us() > 0 ? static_cast<uint64_t>(webm.duration_us() / 1000) : 0);
    add_row("Length", text.view());
    if (audio) add_audio_line(audio->sample_rate, audio->channels);
  }
}

void FileProperties::add_audio(ConstByteView data) {
  media::AudioData sound(allocator());
  media::AudioFileFormat format = media::AudioFileFormat::Unknown;
  if (!media::decode_audio(data, &sound, &format)) {
    add_row("Format", media::audio_format_name(media::detect_audio_format(data)));
    add_row("Sound", "Cannot be read");
    return;
  }
  String text(allocator());
  add_row("Format", media::audio_format_name(format));
  append_duration(&text, sound.duration_ms());
  add_row("Length", text.view());
  text.clear();
  append_hertz(&text, sound.format().sample_rate);
  add_row("Sample rate", text.view());
  text.clear();
  append_channels(&text, sound.format().channels);
  add_row("Channels", text.view());
}

}  // namespace desktop

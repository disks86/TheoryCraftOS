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
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/FileByteSource.hpp"
#include "Desktop/FileStatus.hpp"
#include "Desktop/FileTransfer.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
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
    : Frame(allocator), path_(allocator), name_(allocator), body_(allocator), name_row_(allocator), filler_(allocator), gap_(allocator), gap2_(allocator),
      name_label_(allocator), message_(allocator), name_box_(allocator), grid_(allocator), buttons_(allocator), copy_button_(allocator), ok_(allocator),
      cancel_(allocator), apply_(allocator), ask_(allocator), names_(allocator), values_(allocator) {
  set_title("Properties");
  set_bounds({150, 90, 460, 420});
  set_padding(Insets::all(8));
  set_minimize_enabled(false);   // a secondary frame: a title, a Close button, nothing else
  set_maximize_enabled(false);

  // In reading order (the order Tab takes): the name, the properties, the buttons.
  name_row_.set_style(PanelStyle::Transparent);
  name_row_.set_size(0, 30);
  name_row_.set_padding(Insets{0, 0, 0, 6});
  name_label_.set_text("Name:");
  name_label_.set_dock(DockStyle::Left);
  name_label_.set_size(70, 0);
  name_box_.set_dock(DockStyle::Fill);
  name_box_.on_changed(Handler::immediate(&FileProperties::on_name_changed, this));
  name_row_.add(&name_label_);
  name_row_.add(&name_box_);

  grid_.add_column("Property", 130);
  grid_.add_column("Value", 290);
  grid_.set_dock(DockStyle::Fill);

  message_.set_size(0, 22);

  // Copy Row serves the table and sits at its side; the transaction buttons are right-aligned, the default one first, all the same width.
  buttons_.set_size(0, 30);
  copy_button_.set_text("Copy Row");
  copy_button_.set_size(100, 0);
  copy_button_.on_click({&FileProperties::on_copy, this});
  filler_.set_style(PanelStyle::Transparent);
  filler_.set_dock(DockStyle::Fill);
  ok_.set_text("OK");
  ok_.set_size(84, 0);
  ok_.set_default(true);
  ok_.on_click({&FileProperties::on_ok, this});
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_size(8, 0);
  cancel_.set_text("Cancel");
  cancel_.set_size(84, 0);
  cancel_.on_click({&FileProperties::on_cancel, this});
  gap2_.set_style(PanelStyle::Transparent);
  gap2_.set_size(8, 0);
  apply_.set_text("Apply");
  apply_.set_size(84, 0);
  apply_.set_enabled(false);
  apply_.on_click({&FileProperties::on_apply, this});
  buttons_.add(&copy_button_);
  buttons_.add(&filler_);
  buttons_.add(&ok_);
  buttons_.add(&gap_);
  buttons_.add(&cancel_);
  buttons_.add(&gap2_);
  buttons_.add(&apply_);

  ask_.on_result({&FileProperties::on_answer, this});

  body_.set_dock(DockStyle::Fill);
  body_.add(&name_row_);
  body_.add(&grid_);
  body_.add(&message_);
  body_.add(&buttons_);
  add(&body_);
}

void FileProperties::on_copy(void* self, Control&) { static_cast<FileProperties*>(self)->grid_.copy_selected_row(); }

// --- OK, Cancel, Apply, Close ------------------------------------------------------------

bool FileProperties::pending() const {
  const StringView typed = name_box_.text().trimmed();
  return !name_.empty() && !typed.empty() && !typed.equals(name_.view());
}

void FileProperties::on_name_changed(void* self, Control&) {
  auto* p = static_cast<FileProperties*>(self);
  p->apply_.set_enabled(p->pending());
  p->message_.set_text(StringView());
}

// What is wrong with the change: the sheet stays open and the field at fault gets the focus.
void FileProperties::refuse(StringView text) {
  if (Window* w = window()) w->set_focus(&name_box_);
  name_box_.select_all();
  if (!NoticeDialog::post(notices_, manager(), NoticeKind::Error, text, title())) message_.set_text(text);
}

bool FileProperties::apply() {
  if (!pending()) return true;
  String wanted(allocator()), target(allocator()), message(allocator());
  if (!wanted.assign(name_box_.text().trimmed())) return false;
  if (wanted.view().find('/') != StringView::npos) {
    format_message(&message, "\"%1\" was not renamed because a name cannot contain a slash. Choose another name.", name_.view());
    refuse(message.view());
    return false;
  }
  const StringView path = path_.view();
  const uint64_t slash = path.rfind('/');
  if (!fs_ || slash == StringView::npos || !target.assign(path.first(slash + 1)) || !target.append(wanted.view())) return false;
  DirectoryEntry taken;
  Status status = fs_->stat(target.view(), &taken) == Status::NotFound ? fs_->rename(path, target.view()) : Status::AlreadyExists;
  if (status != Status::Ok) {
    describe_failure(&message, "\"%1\" could not be renamed because %2.", name_.view(), status);
    refuse(message.view());
    return false;
  }
  fs_->flush();
  show(target.view());   // the sheet now shows the object under its new name (and the title follows it)
  fire(changed_);
  return true;
}

void FileProperties::close_now() {
  closing_ = true;
  close();
  closing_ = false;
}

void FileProperties::on_ok(void* self, Control&) {
  auto* p = static_cast<FileProperties*>(self);
  if (p->apply()) p->close_now();
}

void FileProperties::on_cancel(void* self, Control&) { static_cast<FileProperties*>(self)->close_now(); }   // what was not applied is dropped
void FileProperties::on_apply(void* self, Control&) { static_cast<FileProperties*>(self)->apply(); }

// The Close button is not Cancel: a change that was not applied is asked about.
bool FileProperties::close_requested() {
  if (closing_ || !pending() || !manager()) return true;
  String line(allocator());
  format_message(&line, "The name was changed to \"%1\".", name_box_.text().trimmed().size() > 20 ? name_box_.text().trimmed().first(20) : name_box_.text().trimmed());
  ask_.set_texts(title(), line.view(), "Do you want to apply the change?", "Apply", "Cancel");
  ask_.set_alternate("Don't Apply");
  ask_.show(*manager());
  return false;
}

void FileProperties::on_answer(void* self, Control&) {
  auto* p = static_cast<FileProperties*>(self);
  if (p->ask_.choice() == MessageDialog::Choice::Accept) {
    if (p->apply()) p->close_now();
  } else if (p->ask_.choice() == MessageDialog::Choice::Alternate) {
    p->close_now();
  }
}

// Enter is OK and Esc is Cancel, wherever the focus is (a button with the focus takes Enter itself).
bool FileProperties::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter && !(args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt))) {
    if (apply()) close_now();
    return true;
  }
  if (args.usage == hardware::key::kEscape) {
    close_now();
    return true;
  }
  return Frame::on_key(args);
}

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
  String kept(allocator());   // (`path` may be a view of path_ itself)
  kept.assign(path);
  path = kept.view();
  path_.assign(path);
  name_.clear();
  message_.set_text(StringView());
  apply_.set_enabled(false);
  DirectoryEntry entry;
  const bool root = path.size() <= 1;
  const Status status = !fs_ ? Status::NotFound : root ? Status::Ok : fs_->stat(path, &entry);
  if (status != Status::Ok) {
    String why(allocator()), title(allocator());
    const StringView last = path.substr(path.rfind('/') + 1);
    if (fs_) describe_failure(&why, "\"%1\" cannot be shown because %2.", last, status);
    else why.assign("There is no storage to look on.");
    add_row("Path", path);
    add_row("Status", why.view());
    name_box_.set_text(last);
    name_box_.set_enabled(false);
    if (format_message(&title, "%1 Properties", last)) set_title(title.view());
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
  if (format_message(&title, "%1 Properties", name.view())) set_title(title.view());
  // The name can be changed, except that of the list of drives and of what cannot be written to.
  const bool can_rename = !root && !entry.read_only;
  if (can_rename) name_.assign(name.view());
  name_box_.set_text(name.view());
  name_box_.set_enabled(can_rename);

  const bool folder = root || entry.type == NodeType::Directory;
  add_row("Name", name.view());
  if (!root) add_row("Location", where.view());
  add_row("Path", path);
  String type(allocator());   // the type in the user's words, as the file explorer's list names it
  DirectoryEntry named = entry;
  named.type = folder ? NodeType::Directory : entry.type;
  named.name.assign(name.view());
  describe_entry_type(named, &type);
  add_row("Type", type.view());
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

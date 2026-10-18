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

#include "Desktop/MediaPlayer.hpp"
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/FileStatus.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Media/AudioStreaming.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;
using filesystem::IFile;
using filesystem::OpenMode;
using filesystem::Status;

namespace {
const char kMediaExtensions[] = "wav;aif;aiff;au;flac;ogg;mp3;aac;m4a;mid;midi;kar;rmi;avi;mp4;m4v;mov;gif;apng;webp;y4m";
const FileDialog::Filter kFilters[] = {{"Media files", kMediaExtensions}, {"All files", "*"}};
}  // namespace

MediaPlayer::MediaPlayer(allocators::IAllocator& allocator)
    : Frame(allocator), menu_(allocator), status_bar_(allocator), status_(allocator), path_(allocator) {
  set_title("Media Player");
  set_bounds({80, 50, 560, 420});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("Open...", {&MediaPlayer::on_open_menu, this}, "Ctrl+O");
    file->add_item("Close File", {&MediaPlayer::on_close_file, this});
    file->add_separator();
    file->add_item("Exit", {&MediaPlayer::on_exit, this});
  }
  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&status_);
  status_.set_text("Choose File > Open to play a sound or video file.");
  add(&menu_);
  add(&status_bar_);
  on_closed({&MediaPlayer::on_window_closed, this});
}

MediaPlayer::~MediaPlayer() {
  close_media();
  video_control_.reset();   // (the controls refer to the players)
  audio_control_.reset();
  video_player_.reset();
  audio_player_.reset();
}

void MediaPlayer::set_services(audio::AudioMixer* mixer, hardware::SystemVideoCodecs* codecs, const hardware::IClock* clock) {
  if (mixer && !audio_player_) {
    audio_player_ = UniquePointer<audio::AudioPlayer>::make(allocator(), *mixer, allocator());
    if (audio_player_) {
      audio_control_ = UniquePointer<AudioPlayerControl>::make(allocator(), allocator(), *audio_player_);
      if (audio_control_) {
        audio_control_->set_dock(DockStyle::Fill);
        audio_control_->set_visible(false);
        add(audio_control_.get());
      }
    }
  }
  if (codecs && clock && !video_player_) {
    video_player_ = UniquePointer<video::VideoPlayer>::make(allocator(), *clock, *codecs, allocator(), mixer);
    if (video_player_) {
      video_control_ = UniquePointer<VideoPlayerControl>::make(allocator(), allocator(), *video_player_);
      if (video_control_) {
        video_control_->set_dock(DockStyle::Fill);
        video_control_->set_visible(false);
        add(video_control_.get());
      }
    }
  }
}

bool MediaPlayer::is_media_name(StringView name) { return name_has_extension(name, kMediaExtensions); }

void MediaPlayer::close_media() {
  if (video_control_) video_control_->stop_service();
  if (audio_control_) audio_control_->stop_service();
  if (video_player_) video_player_->close();   // (the players go before what they read from)
  if (audio_player_) audio_player_->close();
  sound_.reset();
  file_.reset();
  if (video_control_) video_control_->set_visible(false);
  if (audio_control_) audio_control_->set_visible(false);
  kind_ = Kind::None;
  path_.clear();
  set_title("Media Player");
}

void MediaPlayer::ask_open() {
  if (!dialog_ || !manager()) {
    notify(NoticeKind::Error, "No file dialog available.");
    return;
  }
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kFilters, 2, path_.view(), {&MediaPlayer::on_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

void MediaPlayer::on_picked(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  if (m->dialog_->accepted()) m->open_file(m->dialog_->selected_path());
}

bool MediaPlayer::open_file(StringView path) {
  if (!fs_) {
    notify(NoticeKind::Error, "No storage available.");
    return false;
  }
  close_media();
  String name(allocator());
  name.assign(path);
  UniquePointer<IFile> file;
  const Status status = fs_->open_file(path, OpenMode::Read, &file);
  String message(allocator());
  if (status != Status::Ok) {
    message.append("Cannot open ");
    message.append(path);
    message.append(": ");
    message.append(describe_status(status));
    notify(NoticeKind::Error, message.view());
    return false;
  }
  if (file->size() == 0) {
    notify(NoticeKind::Error, "The file is empty.");
    return false;
  }
  file_ = UniquePointer<FileByteSource>::make(allocator(), util::move(file));
  if (!file_) {
    notify(NoticeKind::Error, "Out of memory.");
    return false;
  }
  if (!open_source()) {
    close_media();
    return false;
  }
  path_ = util::move(name);
  const uint64_t slash = path.rfind('/');
  String title(allocator());
  title.assign("Media Player - ");
  title.append(slash == StringView::npos ? path : path.substr(slash + 1));
  set_title(title.view());
  message.append("Playing ");
  message.append(path);
  if (kind_ == Kind::Audio && playing_midi_) message.append(playing_midi_hardware_ ? " (on the MIDI hardware)" : " (synthesized)");
  status_.set_text(message.view());
  return true;
}

// Works out what the open file is and starts it playing, reading it from file_ as it goes.
bool MediaPlayer::open_source() {
  media::IByteSource& source = *file_;
  uint8_t head[16384];
  const uint64_t got = source.read(0, ByteView(head, source.size() < sizeof head ? source.size() : sizeof head));
  const media::AudioFileFormat format = media::detect_audio_format(ConstByteView(head, got));
  // Sound first, except MP4 (which may hold a movie): that is tried as video, then as sound.
  const bool try_video = format == media::AudioFileFormat::Unknown || format == media::AudioFileFormat::Mp4;
  video::OpenResult video_result = video::OpenResult::Unsupported;
  if (try_video && video_player_) {
    video_result = video_player_->open_media(source);
    if (video_result == video::OpenResult::Ok) {
      kind_ = Kind::Video;
      if (audio_control_) audio_control_->set_visible(false);
      video_control_->set_visible(true);
      video_player_->play();
      video_control_->start_service();
      return true;
    }
  }
  if (format != media::AudioFileFormat::Unknown) {
    playing_midi_hardware_ = playing_midi_ = false;
    if (format == media::AudioFileFormat::Midi) {
      playing_midi_ = true;   // on the MIDI hardware if the machine has some, else synthesized
      playing_midi_hardware_ = midi_output_ != nullptr;
      sound_ = midi::open_midi_stream(source, allocator(), midi_output_);
    } else {
      sound_ = media::open_audio_stream(source, allocator());
    }
    if (sound_) {
      if (!audio_player_) {
        notify(NoticeKind::Error, "No audio output is available.");
        return false;
      }
      if (!audio_player_->open(sound_.get())) {
        notify(NoticeKind::Error, "The sound cannot be played on this audio device.");
        return false;
      }
      kind_ = Kind::Audio;
      if (video_control_) video_control_->set_visible(false);
      audio_control_->set_visible(true);
      audio_player_->play();
      audio_control_->start_service();
      return true;
    }
  }
  if (!video_player_ && format == media::AudioFileFormat::Unknown) {
    notify(NoticeKind::Error, "Video playback is not available on this machine.");
    return false;
  }
  switch (video_result) {
    case video::OpenResult::NoDecoder: notify(NoticeKind::Error, "No video decoder on this machine can play that file."); break;
    case video::OpenResult::OutOfMemory: notify(NoticeKind::Error, "Out of memory."); break;
    default: notify(NoticeKind::Error, "That is not a sound or video file this player can read."); break;
  }
  return false;
}

void MediaPlayer::on_open_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->ask_open(); }
void MediaPlayer::on_close_file(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  m->close_media();
  m->status_.set_text("Choose File > Open to play a sound or video file.");
}
void MediaPlayer::on_exit(void* self, Control&) { static_cast<MediaPlayer*>(self)->close(); }
void MediaPlayer::on_window_closed(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  m->close_media();
  m->status_.set_text("Choose File > Open to play a sound or video file.");
}

bool MediaPlayer::on_key(const KeyArgs& args) {
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 14) {   // Ctrl+O
    ask_open();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop

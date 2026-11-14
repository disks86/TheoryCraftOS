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
#include "Desktop/Format.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Media/AudioStreaming.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;
using filesystem::IFile;
using filesystem::OpenMode;
using filesystem::Status;

namespace {
const char kMediaExtensions[] = "wav;aif;aiff;au;flac;ogg;mp3;aac;m4a;mid;midi;kar;rmi;avi;mp4;m4v;mov;webm;mkv;weba;mka;opus;gif;apng;webp;y4m";
const FileDialog::Filter kFilters[] = {{"Media files", kMediaExtensions}, {"All files", "*"}};
constexpr const char* kNothingOpen = "Choose File > Open to play a sound or video file.";

// The file's name as the file explorer shows it: the path without its folders.
StringView name_of(StringView path) {
  const uint64_t slash = path.rfind('/');
  return slash == StringView::npos ? path : path.substr(slash + 1);
}
}  // namespace

MediaPlayer::MediaPlayer(allocators::IAllocator& allocator)
    : Frame(allocator), menu_(allocator), playback_menu_(allocator), status_bar_(allocator), status_(allocator), path_(allocator), playlist_(allocator) {
  set_title("Media Player");
  set_bounds({80, 50, 560, 420});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    open_item_ = file->add_item("Open...", {&MediaPlayer::on_open_menu, this}, "Ctrl+O");
    close_item_ = file->add_item("Close", {&MediaPlayer::on_close_file, this});
    file->add_separator();
    file->add_item("Exit", {&MediaPlayer::on_exit, this});
  }
  if (Menu* playback = menu_.add_menu("Playback")) {
    play_items_[0] = playback->add_item("Play", {&MediaPlayer::on_play_menu, this}, "Space");   // (named after what it will do: Play or Pause)
    stop_items_[0] = playback->add_item("Stop", {&MediaPlayer::on_stop_menu, this});
    playback->add_separator();
    previous_items_[0] = playback->add_item("Previous Track", {&MediaPlayer::on_previous_menu, this}, "Ctrl+Left");
    next_items_[0] = playback->add_item("Next Track", {&MediaPlayer::on_next_menu, this}, "Ctrl+Right");
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Media Player", {&MediaPlayer::on_about, this});
  menu_.on_opening(Handler::immediate(&MediaPlayer::on_menu_opening, this));
  // The context menu of the player's area: the playback commands (all of them are in the Playback menu too).
  play_items_[1] = playback_menu_.add_item("Play", {&MediaPlayer::on_play_menu, this});
  stop_items_[1] = playback_menu_.add_item("Stop", {&MediaPlayer::on_stop_menu, this});
  playback_menu_.add_separator();
  previous_items_[1] = playback_menu_.add_item("Previous Track", {&MediaPlayer::on_previous_menu, this});
  next_items_[1] = playback_menu_.add_item("Next Track", {&MediaPlayer::on_next_menu, this});
  playback_menu_.on_opening(Handler::immediate(&MediaPlayer::on_menu_opening, this));
  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&status_);
  status_.set_text(kNothingOpen);
  add(&menu_);
  add(&status_bar_);
  on_closed({&MediaPlayer::on_window_closed, this});
  update_commands();
}

bool MediaPlayer::playing() const {
  if (kind_ == Kind::Audio && audio_player_) return audio_player_->playing();
  if (kind_ == Kind::Video && video_player_) return video_player_->playing();
  return false;
}

bool MediaPlayer::toggle_play() {
  if (kind_ == Kind::Audio && audio_player_) {
    if (audio_player_->playing()) audio_player_->pause();
    else audio_player_->play();
  } else if (kind_ == Kind::Video && video_player_) {
    if (video_player_->playing()) video_player_->pause();
    else video_player_->play();
  } else {
    return false;
  }
  update_commands();
  return true;
}

bool MediaPlayer::stop_playback() {
  if (kind_ == Kind::Audio && audio_player_) audio_player_->stop();
  else if (kind_ == Kind::Video && video_player_) video_player_->stop();
  else return false;
  update_commands();
  return true;
}

// A command that does not apply is disabled, not removed: nothing to close, play or stop while no file is open, no next track at the end of
// the list. (Previous Track always applies while a file is open: it goes back to the start of the track first.)
void MediaPlayer::update_commands() {
  const bool open = kind_ != Kind::None, now_playing = playing();
  if (open_item_) open_item_->set_enabled(dialog_ != nullptr);
  if (close_item_) close_item_->set_enabled(open);
  for (uint32_t i = 0; i < 2; ++i) {
    if (play_items_[i]) {
      play_items_[i]->text.assign(now_playing ? "Pause" : "Play");
      play_items_[i]->set_enabled(open);
    }
    if (stop_items_[i]) stop_items_[i]->set_enabled(open);
    if (previous_items_[i]) previous_items_[i]->set_enabled(open);
    if (next_items_[i]) next_items_[i]->set_enabled(open && playlist_.following(false) != Playlist::kNone);
  }
}

void MediaPlayer::on_menu_opening(void* self, Control&) { static_cast<MediaPlayer*>(self)->update_commands(); }
void MediaPlayer::on_play_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->toggle_play(); }
void MediaPlayer::on_stop_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->stop_playback(); }
void MediaPlayer::on_about(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  NoticeDialog::post(m->notices_, m->manager(), NoticeKind::Information, "Media Player is part of TheoryCraftOS.", "About Media Player");
}

void MediaPlayer::notify_file(StringView pattern, StringView name) {
  String message(allocator());
  if (format_message(&message, pattern, name)) notify(NoticeKind::Error, message.view());
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
        audio_control_->set_context_menu(&playback_menu_);
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
        video_control_->set_context_menu(&playback_menu_);
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
  was_playing_ = false;
  set_title("Media Player");
  update_commands();
}

void MediaPlayer::ask_open() {
  if (!dialog_ || !manager()) return;   // (Open is disabled then)
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kFilters, 2, path_.view(), {&MediaPlayer::on_picked, this}))
    notify(NoticeKind::Information, "A file cannot be opened while another file dialog is open. Close that dialog, then choose Open again.");
}

void MediaPlayer::on_picked(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  if (m->dialog_->accepted()) m->open_file(m->dialog_->selected_path());
}

bool MediaPlayer::open_file(StringView path) {
  uint64_t at = playlist_.index_of(path);
  if (at == Playlist::kNone) {
    load_folder(path);
    at = playlist_.index_of(path);
  }
  if (at != Playlist::kNone) playlist_.set_current(at);
  return play_path(path);
}

// The playlist becomes the media files of the folder of `path` (and `path` itself, whatever it is called), in the order of the folder listing.
void MediaPlayer::load_folder(StringView path) {
  playlist_.clear();
  if (fs_) {
    List<filesystem::DirectoryEntry> entries(allocator());
    const StringView folder = Playlist::folder_of(path);
    if (!folder.empty() && read_sorted_directory(*fs_, folder, &entries) == Status::Ok) {
      for (uint64_t i = 0; i < entries.count(); ++i) {
        const filesystem::DirectoryEntry& e = entries[i];
        const StringView name = e.name.view();
        if (e.type == filesystem::NodeType::Directory || !is_media_name(name)) continue;
        String full(allocator());
        full.assign(folder);
        if (folder.size() > 1) full.append("/");
        full.append(name);
        if (full.view() == path || is_media_name(full.view())) playlist_.add(full.view());
      }
    }
  }
  if (playlist_.index_of(path) == Playlist::kNone) playlist_.add(path);
}

bool MediaPlayer::play_index(uint64_t index) {
  if (index == Playlist::kNone) return false;
  String path(allocator());
  path.assign(playlist_.path(index));
  playlist_.set_current(index);
  return play_path(path.view());
}

int64_t MediaPlayer::position_ms() {
  if (kind_ == Kind::Audio && audio_player_ && audio_player_->sample_rate())
    return static_cast<int64_t>(audio_player_->position() * 1000 / audio_player_->sample_rate());
  if (kind_ == Kind::Video && video_player_) return video_player_->position_us() / 1000;
  return 0;
}

bool MediaPlayer::next_track() {
  if (kind_ == Kind::None) return false;
  return play_index(playlist_.following(false));
}

bool MediaPlayer::previous_track() {
  if (kind_ == Kind::None) return false;
  if (position_ms() > 3000) return play_index(playlist_.current());   // (back to the start of this one first)
  const uint64_t before = playlist_.preceding(false);
  return play_index(before != Playlist::kNone ? before : playlist_.current());
}

void MediaPlayer::pump() {
  if (kind_ == Kind::None) {
    was_playing_ = false;
    return;
  }
  bool playing = false, empty = false;
  int64_t duration_ms = 0;
  if (kind_ == Kind::Audio && audio_player_) {
    playing = audio_player_->playing();
    empty = audio_player_->state() == audio::PlayerState::Empty;
    duration_ms = audio_player_->sample_rate() ? static_cast<int64_t>(audio_player_->duration() * 1000 / audio_player_->sample_rate()) : 0;
  } else if (kind_ == Kind::Video && video_player_) {
    playing = video_player_->playing();
    empty = video_player_->state() == video::VideoState::Empty;
    duration_ms = video_player_->duration_us() / 1000;
  }
  if (playing) {
    was_playing_ = true;
    last_position_ms_ = position_ms();
  } else if (was_playing_ && !empty) {
    was_playing_ = false;
    // it stopped by itself if the last position seen was at the end (a stop by the user was somewhere in the middle)
    if (duration_ms > 0 && last_position_ms_ >= duration_ms - 1000) next_track();
  } else if (empty) {
    was_playing_ = false;
  }
}

void MediaPlayer::on_next_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->next_track(); }
void MediaPlayer::on_previous_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->previous_track(); }

bool MediaPlayer::play_path(StringView path) {
  // (every message names the file as the file explorer shows it, says why it cannot be played, and what can be done where something can)
  String name(allocator()), full(allocator());
  if (!name.assign(name_of(path)) || !full.assign(path)) return false;
  if (!fs_) {
    notify_file("\"%1\" could not be opened because no disk is ready.", name.view());
    return false;
  }
  close_media();
  UniquePointer<IFile> file;
  const Status status = fs_->open_file(full.view(), OpenMode::Read, &file);
  String message(allocator());
  if (status != Status::Ok) {
    if (describe_failure(&message, "\"%1\" could not be opened because %2.", name.view(), status)) notify(NoticeKind::Error, message.view());
    return false;
  }
  if (file->size() == 0) {
    notify_file("\"%1\" cannot be played because the file is empty.", name.view());
    return false;
  }
  file_ = UniquePointer<FileByteSource>::make(allocator(), util::move(file));
  if (!file_) {
    notify_file("\"%1\" could not be opened because there is not enough memory. Close something you do not need and try again.", name.view());
    return false;
  }
  if (!open_source(name.view())) {
    close_media();
    return false;
  }
  path_ = util::move(full);
  // The title: the file first, then the program.
  String title(allocator());
  if (format_message(&title, "%1 - Media Player", name.view())) set_title(title.view());
  // The status line: one whole sentence for each case (never glued from pieces).
  const bool numbered = playlist_.count() > 1 && playlist_.current() != Playlist::kNone;
  const bool midi = kind_ == Kind::Audio && playing_midi_;
  StringView pattern = numbered ? "Playing \"%1\" (%2 of %3)" : "Playing \"%1\"";
  if (midi && playing_midi_hardware_) pattern = numbered ? "Playing \"%1\" on the MIDI hardware (%2 of %3)" : "Playing \"%1\" on the MIDI hardware";
  else if (midi) pattern = numbered ? "Playing \"%1\" with the built-in synthesizer (%2 of %3)" : "Playing \"%1\" with the built-in synthesizer";
  String at(allocator()), of(allocator());
  if (numbered) {
    at.append_uint(playlist_.current() + 1);
    of.append_uint(playlist_.count());
  }
  if (format_message(&message, pattern, name.view(), at.view(), of.view())) status_.set_text(message.view());
  update_commands();
  return true;
}

// Works out what the open file is and starts it playing, reading it from file_ as it goes.
bool MediaPlayer::open_source(StringView name) {
  media::IByteSource& source = *file_;
  uint8_t head[16384];
  const uint64_t got = source.read(0, ByteView(head, source.size() < sizeof head ? source.size() : sizeof head));
  const media::AudioFileFormat format = media::detect_audio_format(ConstByteView(head, got));
  // Sound first, except MP4 and WebM (which may hold a movie): that is tried as video, then as sound.
  const bool try_video = format == media::AudioFileFormat::Unknown || format == media::AudioFileFormat::Mp4 || format == media::AudioFileFormat::Webm;
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
      playing_midi_hardware_ = midi_output_ && midi_output_->available();
      sound_ = midi::open_midi_stream(source, allocator(), playing_midi_hardware_ ? midi_output_ : nullptr);
    } else {
      sound_ = media::open_audio_stream(source, allocator());
    }
    if (sound_) {
      if (!audio_player_) {
        notify_file("\"%1\" cannot be played because this machine has no sound output. Connect a sound device and try again.", name);
        return false;
      }
      if (!audio_player_->open(sound_.get())) {
        notify_file("\"%1\" cannot be played because the sound device cannot play its kind of sound.", name);
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
    notify_file("\"%1\" cannot be played because it is not a sound file and this machine cannot play video.", name);
    return false;
  }
  switch (video_result) {
    case video::OpenResult::NoDecoder: notify_file("\"%1\" cannot be played because this machine cannot decode its kind of video.", name); break;
    case video::OpenResult::OutOfMemory:
      notify_file("\"%1\" could not be opened because there is not enough memory. Close something you do not need and try again.", name);
      break;
    default: notify_file("\"%1\" cannot be played because it is not a sound or video file that Media Player can read.", name); break;
  }
  return false;
}

void MediaPlayer::on_open_menu(void* self, Control&) { static_cast<MediaPlayer*>(self)->ask_open(); }
void MediaPlayer::on_close_file(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  m->close_media();
  m->playlist_.clear();
  m->status_.set_text(kNothingOpen);
  m->update_commands();
}
void MediaPlayer::on_exit(void* self, Control&) { static_cast<MediaPlayer*>(self)->close(); }
void MediaPlayer::on_window_closed(void* self, Control&) {
  auto* m = static_cast<MediaPlayer*>(self);
  m->close_media();
  m->playlist_.clear();
  m->status_.set_text(kNothingOpen);
  m->update_commands();
}

bool MediaPlayer::on_key(const KeyArgs& args) {
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 14) {   // Ctrl+O
    ask_open();
    return true;
  }
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kRight) { next_track(); return true; }
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kLeft) { previous_track(); return true; }
  // Space that no control used (a button with the focus is pressed by it): Play / Pause.
  if (!(args.modifiers & (hardware::modifier::kControl | hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) && args.usage == hardware::key::kSpace && toggle_play()) return true;
  return Frame::on_key(args);
}

}  // namespace desktop

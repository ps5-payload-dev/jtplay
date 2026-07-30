// SPDX-License-Identifier: GPL-3.0-or-later
//
// Full-screen playback. For video the UI chrome hides and the decoded
// frames fill the screen behind a transparent document body, with an
// auto-hiding info/transport bar. For audio a persistent now-playing card
// is shown instead.
#include <algorithm>

#include "app.h"
#include "app_internal.h"

using namespace appdetail;

void App::PlayEntry(const browse::Entry& entry) {
  playing_ = entry;
  EnterWatch(entry);

  // Resolve and open on the worker: a source with expiring URLs mints one
  // here, as late as possible, and both steps may block on the network.
  const browse::Entry item = playing_;
  const browse::SourcePtr source = current_source_;
  busy_ops_++;
  PostTask([this, item, source] {
    std::string error;
    std::string url = item.res_url;
    bool ok;
    if (source)
      ok = source->Resolve(item, url, error);
    else
      ok = !url.empty();
    if (ok)
      ok = player_->Open(url, error);
    else if (error.empty())
      error = "this item has no playable resource";
    {
      std::lock_guard<std::mutex> lock(pending_.mutex);
      pending_.play_ready = true;
      pending_.play_ok = ok;
      pending_.play_error = error;
    }
    busy_ops_--;
  });
}

void App::EnterWatch(const browse::Entry& entry) {
  bind_watching_ = true;
  bind_watch_audio_ = entry.IsAudio();
  bind_watch_title_ = entry.title.empty() ? "(untitled)" : entry.title;

  std::string meta;
  if (!entry.artist.empty())
    meta = entry.artist;
  if (!entry.album.empty())
    meta += (meta.empty() ? "" : "  -  ") + entry.album;
  if (meta.empty() && !entry.resolution.empty())
    meta = entry.resolution;
  bind_watch_meta_ = meta;

  bind_watch_paused_ = false;
  bind_watch_time_ = "";
  bind_watch_progress_ = "0%";
  bind_watch_vtrack_ = "";
  bind_watch_atrack_ = "";
  bind_watch_multi_video_ = false;
  bind_watch_multi_audio_ = false;
  model_.DirtyVariable("watching");
  model_.DirtyVariable("watch_audio");
  model_.DirtyVariable("watch_title");
  model_.DirtyVariable("watch_meta");
  model_.DirtyVariable("watch_paused");
  model_.DirtyVariable("watch_time");
  model_.DirtyVariable("watch_progress");
  model_.DirtyVariable("watch_vtrack");
  model_.DirtyVariable("watch_atrack");
  model_.DirtyVariable("watch_multi_video");
  model_.DirtyVariable("watch_multi_audio");
  RefreshArtBindings();
  ShowWatchInfo(kWatchInfoSec);
}

void App::ExitWatch() {
  bind_watching_ = false;
  bind_info_visible_ = false;
  model_.DirtyVariable("watching");
  model_.DirtyVariable("info_visible");
  RefreshArtBindings();
}

void App::StopPlayback() {
  ExitWatch();
  PostTask([this] { player_->Stop(); });
}

void App::ShowWatchInfo(double seconds) {
  bind_info_visible_ = true;
  info_deadline_ = Now() + seconds;
  model_.DirtyVariable("info_visible");
}

void App::UpdateWatchOverlay() {
  const int64_t pos = player_->PositionUs();
  const int64_t dur = player_->DurationUs();

  Rml::String time;
  Rml::String progress = "0%";
  if (pos >= 0) {
    time = FormatTime(pos);
    if (dur > 0) {
      time += " / " + FormatTime(dur);
      const int pct = std::clamp((int)(pos * 100 / dur), 0, 100);
      progress = Rml::String(std::to_string(pct) + "%");
    }
  }
  if (bind_watch_time_ != time) {
    bind_watch_time_ = time;
    model_.DirtyVariable("watch_time");
  }
  if (bind_watch_progress_ != progress) {
    bind_watch_progress_ = progress;
    model_.DirtyVariable("watch_progress");
  }

  const bool paused = player_->IsPaused();
  if (bind_watch_paused_ != paused) {
    bind_watch_paused_ = paused;
    model_.DirtyVariable("watch_paused");
  }

  const bool seekable = player_->CanSeek();
  if (bind_watch_seekable_ != seekable) {
    bind_watch_seekable_ = seekable;
    model_.DirtyVariable("watch_seekable");
  }

  // Codec lines for the info bar. These are the labels of the streams
  // actually being decoded, so they follow a track switch; the counts only
  // decide whether the switch hints are worth showing.
  const Rml::String vtrack = player_->CurrentVideoLabel();
  if (bind_watch_vtrack_ != vtrack) {
    bind_watch_vtrack_ = vtrack;
    model_.DirtyVariable("watch_vtrack");
  }

  const Rml::String atrack = player_->CurrentAudioLabel();
  if (bind_watch_atrack_ != atrack) {
    bind_watch_atrack_ = atrack;
    model_.DirtyVariable("watch_atrack");
  }

  const bool multi_video = player_->VideoTrackCount() > 1;
  if (bind_watch_multi_video_ != multi_video) {
    bind_watch_multi_video_ = multi_video;
    model_.DirtyVariable("watch_multi_video");
  }

  const bool multi_audio = player_->AudioTrackCount() > 1;
  if (bind_watch_multi_audio_ != multi_audio) {
    bind_watch_multi_audio_ = multi_audio;
    model_.DirtyVariable("watch_multi_audio");
  }
}

// Triangle / T: switches to the next audio track (wrapping around) and
// announces the choice with a toast.
void App::CycleAudioTrack() {
  const std::vector<AudioTrackInfo> tracks = player_->AudioTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one audio track");
    return;
  }

  const int cur = player_->CurrentAudioStream();
  size_t i = 0;
  while (i < tracks.size() && tracks[i].stream_index != cur)
    i++;
  const size_t next = (i + 1) % tracks.size(); // cur not found -> wraps to 0

  if (player_->SelectAudioTrack(tracks[next].stream_index)) {
    ShowToast("Audio " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label);
  }
}

// Square: switches to the next video track (wrapping around). Streams that
// carry several camera angles or bitrate variants show up as separate video
// streams in one container, which is what this walks.
void App::CycleVideoTrack() {
  const std::vector<VideoTrackInfo> tracks = player_->VideoTracks();
  if (tracks.size() < 2) {
    if (!tracks.empty())
      ShowToast("Only one video track");
    return;
  }

  const int cur = player_->CurrentVideoStream();
  size_t i = 0;
  while (i < tracks.size() && tracks[i].stream_index != cur)
    i++;
  const size_t next = (i + 1) % tracks.size(); // cur not found -> wraps to 0

  if (player_->SelectVideoTrack(tracks[next].stream_index)) {
    ShowToast("Video " + std::to_string(next + 1) + "/" +
              std::to_string(tracks.size()) + ": " + tracks[next].label);
  }
}

// Finds the next (direction=+1) or previous (direction=-1) playable
// non-container item in the current folder and starts it. Used by the
// track-skip keys and by audio auto-advance.
bool App::PlayNeighbor(int direction) {
  const BrowseLevel* level = path_.empty() ? nullptr : &path_.back();
  if (!level)
    return false;

  // Locate the playing item by id; the selection may have moved.
  int index = -1;
  for (size_t i = 0; i < level->entries.size(); i++) {
    if (level->entries[i].id == playing_.id) {
      index = (int)i;
      break;
    }
  }
  if (index < 0)
    return false;

  for (int i = index + direction; i >= 0 && i < (int)level->entries.size(); i += direction) {
    const browse::Entry& e = level->entries[i];
    if (!e.IsPlayable() || e.IsImage())
      continue;
    sel_entry_ = i;
    model_.DirtyVariable("sel_entry");
    RebuildDetail();
    scroll_entries_pending_ = true;
    PlayEntry(e);
    return true;
  }
  return false;
}

void App::HandlePlaybackEnd() {
  if (PlayNeighbor(+1))
    return;
  StopPlayback();
}

void App::HandleKeyWatch(Rml::Event& event, int key) {
  switch (key) {
  case Rml::Input::KI_LEFT:
  case Rml::Input::KI_RIGHT:
  case Rml::Input::KI_UP:
  case Rml::Input::KI_DOWN:
    if (player_->CanSeek()) {
      int64_t delta = 0;
      if (key == Rml::Input::KI_LEFT)  delta = -kSeekSmallUs;
      if (key == Rml::Input::KI_RIGHT) delta = +kSeekSmallUs;
      if (key == Rml::Input::KI_UP)    delta = +kSeekLargeUs;
      if (key == Rml::Input::KI_DOWN)  delta = -kSeekLargeUs;
      player_->SeekRelative(delta);
      ShowWatchInfo(kWatchInfoSec);
    }
    break;

  case Rml::Input::KI_PRIOR: // page up / left shoulder: previous track
    PlayNeighbor(-1);
    break;
  case Rml::Input::KI_NEXT:  // page down / right shoulder: next track
    PlayNeighbor(+1);
    break;

  case Rml::Input::KI_RETURN:
  case Rml::Input::KI_NUMPADENTER: // cross: play / pause
    player_->TogglePause();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_T: // triangle: next audio track
    CycleAudioTrack();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_SPACE: // square: next video track
    CycleVideoTrack();
    ShowWatchInfo(kWatchInfoSec);
    break;

  case Rml::Input::KI_F1: // options: toggle the info bar
    if (bind_info_visible_ && !bind_watch_audio_) {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    } else {
      ShowWatchInfo(kWatchInfoSec);
    }
    break;

  case Rml::Input::KI_BACK:   // circle: back to the browse list
  case Rml::Input::KI_ESCAPE:
    StopPlayback();
    break;

  default:
    return;
  }
  event.StopPropagation();
}

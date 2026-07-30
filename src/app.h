// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application shell, structured after ps5-payload-dev/tvhp: all RmlUi access
// happens on the main thread; anything that touches the network or the disk
// (discovery, browsing, opening a stream) runs on a single worker thread
// whose results are polled once per frame in Update().
//
// The app itself is source-agnostic: it browses browse::Source objects that
// browse::Providers discover (DLNA servers, local directories, ...).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>

#include "browse/source.h"
#include "player.h"

class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  struct Options {
    std::string assets_dir = "assets";   // directory containing main.rml
    std::string cache_dir;               // artwork cache; "" = XDG default
    std::string plugins_dir;             // JS plugin scripts; "" = none
    std::vector<std::string> media_dirs; // extra local roots to browse
  };

  // Creates the data model and loads <assets_dir>/main.rml. Must be called
  // before the first context update.
  bool Initialize(Rml::Context* context, const Options& options, std::string& error);
  void Shutdown();

  // Per-frame housekeeping; call before context->Update().
  void Update();

  // Hook for the player to draw video beneath the UI; call between
  // Backend::BeginFrame() and context->Render().
  void RenderVideo(int width, int height);

  // Rml::EventListener (document-level keydown, capture phase).
  void ProcessEvent(Rml::Event& event) override;

private:
  enum class View { Sources, Browse };

  // One level of the browse tree; kept on a stack so backing out is
  // instant and restores the selection.
  struct BrowseLevel {
    std::string id;
    std::string title;
    std::vector<browse::Entry> entries;
    int selection = 0;
  };

  // Rows exposed to the RmlUi data model.
  struct SourceRow {
    Rml::String icon;
    Rml::String name;
    Rml::String detail;
  };
  struct EntryRow {
    Rml::String icon;   // glyph for the kind of entry
    Rml::String title;
    Rml::String meta;   // duration / child count / codec hints
    bool folder = false;
  };

  bool SetupDataModel(Rml::Context* context, std::string& error);

  // --- Worker ------------------------------------------------------------
  // Tasks run strictly in order on one background thread; that thread is
  // the only one allowed to call Source::Browse()/Provider::Discover() or
  // player_->Open()/Stop().
  void PostTask(std::function<void()> task);
  void WorkerMain();

  // --- Sources view ------------------------------------------------------
  void StartDiscovery();
  void RebuildSourceRows();
  void HandleKeySources(Rml::Event& event, int key);

  // --- Browse view -------------------------------------------------------
  void OpenSource(int index);            // browse the root of a source
  void EnterContainer(const browse::Entry& entry);
  void LeaveContainer();                 // back one level (or to sources)
  void RequestBrowse(const std::string& id, const std::string& title);
  void RebuildEntryRows();
  void RebuildDetail();
  void RebuildCrumb();
  void MoveSelection(int delta);
  void ActivateSelection();
  void HandleKeyBrowse(Rml::Event& event, int key);
  const browse::Entry* SelectedEntry() const;
  BrowseLevel* CurrentLevel();

  // --- Playback ----------------------------------------------------------
  void PlayEntry(const browse::Entry& entry);
  void StopPlayback();                   // posts the stop; exits watch UI
  void EnterWatch(const browse::Entry& entry);
  void ExitWatch();
  void ShowWatchInfo(double seconds);
  void UpdateWatchOverlay();
  // AtEnd(): audio advances to the next track in the folder, video exits.
  void HandlePlaybackEnd();
  bool PlayNeighbor(int direction);      // next/previous playable item
  void CycleAudioTrack();                // triangle: next audio track
  void CycleVideoTrack();                // square: next video track
  void HandleKeyWatch(Rml::Event& event, int key);

  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  void ShowToast(const std::string& text);

  // --- Artwork -----------------------------------------------------------
  // Cover URLs in the metadata are either plain http URLs or local file
  // paths. Local paths are bound directly; RmlUi only loads textures
  // through the file interface, so the worker downloads each http URL into
  // a small on-disk cache and the UI binds the cached path.
  // Returns the displayable path, or "" while a download is (or has been
  // scheduled to be) fetched in the background, or if it failed.
  std::string ArtPathFor(const std::string& url);
  void RefreshArtBindings();       // re-resolve detail/now-playing art

  // Data model bound state (main thread only).
  Rml::DataModelHandle model_;
  Rml::String bind_view_ = "sources";
  Rml::String bind_status_;       // sources view status line
  Rml::String bind_toast_;
  Rml::String bind_crumb_;        // breadcrumb of the browse path
  Rml::String bind_source_name_;  // topbar: connected source
  Rml::String bind_clock_;
  Rml::String bind_detail_title_;
  Rml::String bind_detail_meta_;
  Rml::String bind_detail_desc_;
  Rml::String bind_player_status_;
  bool bind_busy_ = false;         // a browse/discovery is in flight
  bool bind_watching_ = false;     // full-screen playback, chrome hidden
  bool bind_info_visible_ = false; // watch info bar shown (auto-hides)
  bool bind_watch_audio_ = false;  // audio-only: persistent now-playing card
  bool bind_watch_paused_ = false;
  bool bind_watch_seekable_ = false;
  Rml::String bind_watch_title_;
  Rml::String bind_watch_meta_;
  Rml::String bind_watch_time_;
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  // Codec lines shown in the watch bar: the label of the video and audio
  // track being decoded ("h264 1920x1080", "eng · ac3 5.1(side)"), or ""
  // when the file has no stream of that kind. The multi_ flags say whether
  // there is anything to switch to, and gate the button hints.
  Rml::String bind_watch_vtrack_;
  Rml::String bind_watch_atrack_;
  bool bind_watch_multi_video_ = false;
  bool bind_watch_multi_audio_ = false;
  Rml::String bind_detail_art_;    // local image path, "" = none
  Rml::String bind_np_art_;        // now-playing artwork path, "" = none

  std::vector<SourceRow> source_rows_;
  std::vector<EntryRow> entry_rows_;
  int sel_source_ = 0;
  int source_count_ = 0;
  int sel_entry_ = 0;
  int entry_count_ = 0;

  // Backing data (main thread).
  std::vector<std::unique_ptr<browse::Provider>> providers_;
  std::vector<browse::SourcePtr> sources_;
  browse::SourcePtr current_source_;   // source being browsed, or null
  std::vector<BrowseLevel> path_;      // root first
  browse::Entry playing_;              // item in the player right now

  std::unique_ptr<Player> player_;
  Rml::Context* context_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  View view_ = View::Sources;

  // Worker plumbing.
  std::thread worker_;
  std::mutex tasks_mutex_;
  std::condition_variable tasks_cv_;
  std::deque<std::function<void()>> tasks_;
  std::atomic<bool> worker_running_{false};

  // Results the worker leaves for Update() to pick up.
  struct Pending {
    std::mutex mutex;
    bool sources_ready = false;
    std::vector<browse::SourcePtr> sources;
    std::string discover_error;
    bool browse_ready = false;
    uint32_t browse_request = 0;    // matches browse_request_ or is stale
    std::string browse_id;
    std::string browse_title;
    browse::Listing browse;
    std::string browse_error;
    bool play_ready = false;
    bool play_ok = false;
    std::string play_error;
    // url -> cached local path ("" = download failed)
    std::vector<std::pair<std::string, std::string>> art;
  };
  Pending pending_;

  // Artwork cache (main thread).
  std::string art_dir_;                          // "" = cache unavailable
  std::map<std::string, std::string> art_paths_; // url -> path ("" = failed)
  std::set<std::string> art_inflight_;
  uint32_t browse_request_ = 0;     // id of the browse we are waiting for
  std::atomic<int> busy_ops_{0};

  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0;      // watch info bar auto-hide
  bool scroll_entries_pending_ = false;
  bool scroll_sources_pending_ = false;
};

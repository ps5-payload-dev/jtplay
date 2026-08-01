// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <sys/stat.h>

#include <SDL.h>

#include "app.h"
#include "app_internal.h"
#include "artcache.h"
#include "browse/dlna_source.h"
#include "browse/fs_source.h"
#include "browse/js_source.h"

using namespace appdetail;


App::App() : player_(std::make_unique<Player>()) {}

App::~App() = default;

bool App::Initialize(Rml::Context* context, const Options& options, std::string& error) {
  context_ = context;

  if (!SetupDataModel(context, error))
    return false;

  const std::string doc_path = options.assets_dir + "/main.rml";
  if (!(document_ = context->LoadDocument(doc_path))) {
    error = "failed to load " + doc_path;
    return false;
  }

  document_->Show();
  document_->AddEventListener(Rml::EventId::Keydown, this, true);

  if (!player_->Initialize(error))
    return false;

  // Artwork cache; if the directory can't be used, remote art is simply
  // skipped and local art still works.
  std::string cache_dir = options.cache_dir;
  if (cache_dir.empty()) {
    const char* base = std::getenv("XDG_CACHE_HOME");
    cache_dir = (base && *base) ? std::string(base)
      : (std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.cache"
                             : std::string("/tmp"));
    ::mkdir(cache_dir.c_str(), 0755); // the parent; artcache makes the leaf
    cache_dir += "/jtplay";
  }
  if (!artcache::Initialize(cache_dir))
    Rml::Log::Message(Rml::Log::LT_WARNING,
      "Cannot use cache directory '%s' (%s); remote artwork disabled",
      cache_dir.c_str(), std::strerror(errno));

  providers_.push_back(std::make_unique<browse::FsProvider>());
  providers_.push_back(std::make_unique<browse::DlnaProvider>(kDiscoveryWaitMs));
  if (!options.plugins_dir.empty())
    browse::LoadJsProviders(options.plugins_dir, providers_);

  worker_running_ = true;
  worker_ = std::thread(&App::WorkerMain, this);

  StartDiscovery();
  return true;
}

void App::Shutdown() {
  // Stop the worker first; its tasks capture 'this'.
  if (worker_running_) {
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      worker_running_ = false;
      tasks_.clear();
    }
    tasks_cv_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

  artcache::Shutdown(); // after the worker is joined; it calls Fetch()

  if (player_) {
    player_->Stop();
    player_->Shutdown();
  }

  if (document_) {
    document_->RemoveEventListener(Rml::EventId::Keydown, this, true);
    document_->Close();
    document_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void App::PostTask(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    if (!worker_running_)
      return;
    tasks_.push_back(std::move(task));
  }
  tasks_cv_.notify_one();
}

void App::WorkerMain() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(tasks_mutex_);
      tasks_cv_.wait(lock, [&] { return !tasks_.empty() || !worker_running_; });
      if (!worker_running_)
        return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

bool App::SetupDataModel(Rml::Context* context, std::string& error) {
  Rml::DataModelConstructor ctor = context->CreateDataModel("jtplay");
  if (!ctor) {
    error = "failed to create data model";
    return false;
  }

  if (auto row = ctor.RegisterStruct<SourceRow>()) {
    row.RegisterMember("icon", &SourceRow::icon);
    row.RegisterMember("name", &SourceRow::name);
    row.RegisterMember("detail", &SourceRow::detail);
  }
  ctor.RegisterArray<std::vector<SourceRow>>();

  if (auto row = ctor.RegisterStruct<EntryRow>()) {
    row.RegisterMember("icon", &EntryRow::icon);
    row.RegisterMember("name", &EntryRow::name);
    row.RegisterMember("description", &EntryRow::description);
    row.RegisterMember("folder", &EntryRow::folder);
  }
  ctor.RegisterArray<std::vector<EntryRow>>();

  ctor.Bind("view", &bind_view_);
  ctor.Bind("status", &bind_status_);
  ctor.Bind("toast", &bind_toast_);
  ctor.Bind("crumb", &bind_crumb_);
  ctor.Bind("source_name", &bind_source_name_);
  ctor.Bind("clock", &bind_clock_);
  ctor.Bind("busy", &bind_busy_);
  ctor.Bind("launch_status", &bind_launch_status_);

  ctor.Bind("sources", &source_rows_);
  ctor.Bind("source_count", &source_count_);
  ctor.Bind("sel_source", &sel_source_);

  ctor.Bind("entries", &entry_rows_);
  ctor.Bind("entry_count", &entry_count_);
  ctor.Bind("sel_entry", &sel_entry_);

  ctor.Bind("detail_name", &bind_detail_name_);
  ctor.Bind("detail_description", &bind_detail_description_);
  ctor.Bind("player_status", &bind_player_status_);

  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_audio", &bind_watch_audio_);
  ctor.Bind("watch_paused", &bind_watch_paused_);
  ctor.Bind("watch_seekable", &bind_watch_seekable_);
  ctor.Bind("watch_name", &bind_watch_name_);
  ctor.Bind("watch_description", &bind_watch_description_);
  ctor.Bind("watch_time", &bind_watch_time_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_vtrack", &bind_watch_vtrack_);
  ctor.Bind("watch_atrack", &bind_watch_atrack_);
  ctor.Bind("watch_multi_video", &bind_watch_multi_video_);
  ctor.Bind("watch_multi_audio", &bind_watch_multi_audio_);
  ctor.Bind("detail_image", &bind_detail_image_);
  ctor.Bind("watch_image", &bind_watch_image_);

  // Mouse/touch support on the lists.
  ctor.BindEventCallback("select_source",
    [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
      if (args.size() != 1)
        return;
      const int index = args[0].Get<int>(-1);
      if (index < 0 || index >= (int)sources_.size())
        return;
      if (sel_source_ == index)
        OpenSource(index);
      sel_source_ = index;
      model_.DirtyVariable("sel_source");
    });
  ctor.BindEventCallback("select_entry",
    [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
      if (args.size() != 1)
        return;
      const int index = args[0].Get<int>(-1);
      BrowseLevel* level = CurrentLevel();
      if (!level || index < 0 || index >= (int)level->entries.size())
        return;
      if (sel_entry_ == index) {
        ActivateSelection();
      } else {
        sel_entry_ = index;
        level->selection = index;
        model_.DirtyVariable("sel_entry");
        RebuildDetail();
      }
    });

  model_ = ctor.GetModelHandle();
  return true;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void App::Update() {
  const double now = Now();

  // Clock in the top bar.
  {
    char buf[16] = {};
    const std::time_t t = std::time(nullptr);
    std::tm tm = {};
    localtime_r(&t, &tm);
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (bind_clock_ != buf) {
      bind_clock_ = buf;
      model_.DirtyVariable("clock");
    }
  }

  // Busy indicator.
  {
    const bool busy = busy_ops_.load() > 0;
    if (bind_busy_ != busy) {
      bind_busy_ = busy;
      model_.DirtyVariable("busy");
    }
  }

  // How far an in-flight launch has got. Only shown once the open has been
  // slow enough to be worth mentioning.
  {
    Rml::String status;
    if (launch_request_ && now >= launch_visible_at_) {
      switch (launch_phase_.load()) {
      case kLaunchResolving: status = "Resolving..."; break;
      case kLaunchOpening:   status = "Opening stream..."; break;
      default:               break;
      }
    }
    if (bind_launch_status_ != status) {
      bind_launch_status_ = status;
      model_.DirtyVariable("launch_status");
    }
  }

  // Pick up worker results.
  {
    std::lock_guard<std::mutex> lock(pending_.mutex);

    if (pending_.sources_ready) {
      pending_.sources_ready = false;
      sources_ = std::move(pending_.sources);
      RebuildSourceRows();
      if (sources_.empty()) {
        bind_status_ = pending_.discover_error.empty()
          ? "No media sources found."
          : pending_.discover_error;
      } else {
        bind_status_ = std::to_string(sources_.size()) +
          (sources_.size() == 1 ? " source found" : " sources found");
        if (!pending_.discover_error.empty())
          bind_status_ += "  -  " + pending_.discover_error;
      }
      model_.DirtyVariable("status");
    }

    if (pending_.browse_ready) {
      pending_.browse_ready = false;
      if (pending_.browse_request == browse_request_) {
        if (!pending_.browse_error.empty()) {
          ShowToast(pending_.browse_error);
          // A failed root browse drops back to the source list.
          if (path_.empty()) {
            view_ = View::Sources;
            bind_view_ = "sources";
            model_.DirtyVariable("view");
          }
        } else {
          BrowseLevel level;
          level.id = pending_.browse_id;
          level.name = pending_.browse_name;
          level.entries = std::move(pending_.browse);
          path_.push_back(std::move(level));
          sel_entry_ = 0;
          RebuildEntryRows();
          RebuildCrumb();
          RebuildDetail();
          scroll_entries_pending_ = true;
        }
      }
    }

    if (pending_.play_ready) {
      pending_.play_ready = false;
      if (!launch_request_ || pending_.play_request != launch_request_) {
        // Abandoned while it was opening (circle, or another item started).
        // If it did open, take it down again rather than showing it.
        if (pending_.play_ok)
          PostTask([this] { player_->Stop(); });
      } else if (!pending_.play_ok) {
        ShowToast(pending_.play_error.empty() ? "Playback failed"
                                              : pending_.play_error);
        CancelLaunch();
      } else {
        // The player is running: only now is there anything to look at.
        playing_ = launch_entry_;
        CancelLaunch();
        EnterWatch(playing_);
      }
    }

    if (!pending_.images.empty()) {
      // The result itself already sits in artcache; all the UI has to do is
      // stop treating the URI as in flight and ask again.
      for (const std::string& uri : pending_.images)
        image_inflight_.erase(uri);
      pending_.images.clear();
      RefreshImageBindings();
    }
  }

  // Player status in the top bar.
  {
    const Rml::String status = player_->StatusText();
    if (bind_player_status_ != status) {
      bind_player_status_ = status;
      model_.DirtyVariable("player_status");
    }
  }

  if (bind_watching_) {
    UpdateWatchOverlay();
    if (player_->IsPlaying() && player_->AtEnd())
      HandlePlaybackEnd();
    if (bind_info_visible_ && !bind_watch_audio_ && now > info_deadline_) {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    }
  }

  if (!bind_toast_.empty() && now > toast_deadline_) {
    bind_toast_.clear();
    model_.DirtyVariable("toast");
  }

  // Deferred scrolling: the rows generated by data bindings exist only
  // after the context update that follows the change, so scroll one frame
  // later using the fixed row pitch.
  if (scroll_entries_pending_) {
    EnsureRowVisible("entry-list", sel_entry_, kEntryRowPitch);
    scroll_entries_pending_ = false;
  }
  if (scroll_sources_pending_) {
    EnsureRowVisible("source-list", sel_source_, kSourceRowPitch);
    scroll_sources_pending_ = false;
  }
}

void App::RenderVideo(int width, int height) {
  player_->RenderVideo(width, height);
}

void App::EnsureRowVisible(const char* list_id, int index, float row_pitch) {
  if (!document_)
    return;
  Rml::Element* list = document_->GetElementById(list_id);
  if (!list)
    return;

  const float view_h = list->GetClientHeight();
  const float row_top = index * row_pitch;
  const float row_bottom = row_top + row_pitch;
  float scroll = list->GetScrollTop();

  if (row_top < scroll)
    scroll = row_top;
  else if (row_bottom > scroll + view_h)
    scroll = row_bottom - view_h;
  list->SetScrollTop(scroll);
}

void App::ShowToast(const std::string& text) {
  bind_toast_ = text;
  toast_deadline_ = Now() + kToastSec;
  model_.DirtyVariable("toast");
}

// ---------------------------------------------------------------------------
// Input routing
// ---------------------------------------------------------------------------

void App::ProcessEvent(Rml::Event& event) {
  if (event.GetId() != Rml::EventId::Keydown)
    return;

  const int key = event.GetParameter<int>("key_identifier", 0);

  // Global quit (keyboard only; controllers use the launcher).
  if (key == Rml::Input::KI_Q &&
      (event.GetParameter<int>("ctrl_key", 0) != 0)) {
    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    event.StopPropagation();
    return;
  }

  if (bind_watching_) {
    HandleKeyWatch(event, key);
    return;
  }
  if (view_ == View::Sources) {
    HandleKeySources(event, key);
    return;
  }
  HandleKeyBrowse(event, key);
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

std::string App::ImagePathFor(const std::string& uri) {
  // artcache answers for a local file or an already-cached download without
  // any I/O; everything else has to go to the worker, because RmlUi wants a
  // path now and the download is a blocking request.
  std::string path;
  switch (artcache::Lookup(uri, path)) {
  case artcache::Status::Ready:
    return path;
  case artcache::Status::Failed:
    return {};
  case artcache::Status::Missing:
    break;
  }

  if (!image_inflight_.insert(uri).second)
    return {}; // already being fetched; the next Refresh picks it up

  PostTask([this, uri] {
    artcache::Fetch(uri); // the cache holds the result; we only signal
    std::lock_guard<std::mutex> lock(pending_.mutex);
    pending_.images.push_back(uri);
  });

  return {};
}

void App::RefreshImageBindings() {
  // Details pane follows the browse selection.
  Rml::String detail;
  if (view_ == View::Browse) {
    if (const browse::Entry* entry = SelectedEntry())
      detail = ImagePathFor(entry->image);
  }
  if (bind_detail_image_ != detail) {
    bind_detail_image_ = detail;
    model_.DirtyVariable("detail_image");
  }

  // Now-playing card / transport bar follows the playing item.
  Rml::String watch;
  if (bind_watching_)
    watch = ImagePathFor(playing_.image);
  if (bind_watch_image_ != watch) {
    bind_watch_image_ = watch;
    model_.DirtyVariable("watch_image");
  }
}

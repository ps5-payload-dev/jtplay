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
#include "browse/dlna_source.h"
#include "browse/fs_source.h"
#include "browse/js_source.h"
#include "upnp/http.h"

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

  // Artwork cache directory; if it can't be created, art is simply skipped.
  if (!options.cache_dir.empty()) {
    if (::mkdir(options.cache_dir.c_str(), 0755) == 0 || errno == EEXIST)
      art_dir_ = options.cache_dir;
    else
      Rml::Log::Message(Rml::Log::LT_WARNING,
        "Cannot create cache directory '%s' (%s); artwork disabled",
        options.cache_dir.c_str(), std::strerror(errno));
  } else {
    const char* base = std::getenv("XDG_CACHE_HOME");
    std::string dir = (base && *base) ? std::string(base)
      : (std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.cache"
                             : std::string("/tmp"));
    ::mkdir(dir.c_str(), 0755);
    dir += "/jtplay";
    if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST)
      art_dir_ = dir;
  }

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
    row.RegisterMember("title", &EntryRow::title);
    row.RegisterMember("meta", &EntryRow::meta);
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

  ctor.Bind("sources", &source_rows_);
  ctor.Bind("source_count", &source_count_);
  ctor.Bind("sel_source", &sel_source_);

  ctor.Bind("entries", &entry_rows_);
  ctor.Bind("entry_count", &entry_count_);
  ctor.Bind("sel_entry", &sel_entry_);

  ctor.Bind("detail_title", &bind_detail_title_);
  ctor.Bind("detail_meta", &bind_detail_meta_);
  ctor.Bind("detail_desc", &bind_detail_desc_);
  ctor.Bind("player_status", &bind_player_status_);

  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_audio", &bind_watch_audio_);
  ctor.Bind("watch_paused", &bind_watch_paused_);
  ctor.Bind("watch_seekable", &bind_watch_seekable_);
  ctor.Bind("watch_title", &bind_watch_title_);
  ctor.Bind("watch_meta", &bind_watch_meta_);
  ctor.Bind("watch_time", &bind_watch_time_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_vtrack", &bind_watch_vtrack_);
  ctor.Bind("watch_atrack", &bind_watch_atrack_);
  ctor.Bind("watch_multi_video", &bind_watch_multi_video_);
  ctor.Bind("watch_multi_audio", &bind_watch_multi_audio_);
  ctor.Bind("detail_art", &bind_detail_art_);
  ctor.Bind("np_art", &bind_np_art_);

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
          level.title = pending_.browse_title;
          level.entries = std::move(pending_.browse.entries);
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
      if (!pending_.play_ok) {
        ShowToast(pending_.play_error.empty() ? "Playback failed"
                                              : pending_.play_error);
        if (bind_watching_)
          ExitWatch();
      }
    }

    if (!pending_.art.empty()) {
      for (auto& done : pending_.art) {
        art_paths_[done.first] = done.second;
        art_inflight_.erase(done.first);
      }
      pending_.art.clear();
      RefreshArtBindings();
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

namespace {

// Tiny FNV-1a, good enough to key cache filenames by URL.
uint64_t HashUrl(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// SDL_image picks the decoder from the extension we store under, so sniff
// the actual bytes rather than trusting the server's Content-Type.
const char* SniffImageExt(const std::string& b) {
  if (b.size() >= 3 && (unsigned char)b[0] == 0xff && (unsigned char)b[1] == 0xd8)
    return "jpg";
  if (b.size() >= 8 && b.compare(0, 4, "\x89PNG") == 0)
    return "png";
  if (b.size() >= 6 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0))
    return "gif";
  if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M')
    return "bmp";
  if (b.size() >= 12 && b.compare(8, 4, "WEBP") == 0)
    return "webp";
  return nullptr;
}

constexpr size_t kMaxArtBytes = 12 * 1024 * 1024;

} // namespace

std::string App::ArtPathFor(const std::string& url) {
  if (url.empty())
    return {};

  // Local paths (filesystem sources) need no download; RmlUi loads them
  // through the file interface directly.
  if (url[0] == '/')
    return url;

  if (art_dir_.empty())
    return {};

  auto it = art_paths_.find(url);
  if (it != art_paths_.end())
    return it->second; // may be "" if the download failed; don't retry

  if (!art_inflight_.insert(url).second)
    return {};

  const std::string dir = art_dir_;
  PostTask([this, url, dir]() {
    std::string path; // stays empty on failure

    upnp::HttpResponse resp;
    std::string error;
    if (upnp::HttpGet(url, resp, error) && resp.status == 200 &&
        !resp.body.empty() && resp.body.size() <= kMaxArtBytes) {
      if (const char* ext = SniffImageExt(resp.body)) {
        char name[64];
        std::snprintf(name, sizeof(name), "/art-%016llx.%s",
                      (unsigned long long)HashUrl(url), ext);
        const std::string full = dir + name;
        if (FILE* f = std::fopen(full.c_str(), "wb")) {
          const bool ok =
            std::fwrite(resp.body.data(), 1, resp.body.size(), f) == resp.body.size();
          std::fclose(f);
          if (ok)
            path = full;
          else
            std::remove(full.c_str());
        }
      }
    }

    std::lock_guard<std::mutex> lock(pending_.mutex);
    pending_.art.emplace_back(url, path);
  });

  return {};
}

void App::RefreshArtBindings() {
  // Details pane follows the browse selection.
  Rml::String detail;
  if (view_ == View::Browse) {
    if (const browse::Entry* entry = SelectedEntry())
      detail = ArtPathFor(entry->art_url);
  }
  if (bind_detail_art_ != detail) {
    bind_detail_art_ = detail;
    model_.DirtyVariable("detail_art");
  }

  // Now-playing card / transport bar follows the playing item.
  Rml::String np;
  if (bind_watching_)
    np = ArtPathFor(playing_.art_url);
  if (bind_np_art_ != np) {
    bind_np_art_ = np;
    model_.DirtyVariable("np_art");
  }
}

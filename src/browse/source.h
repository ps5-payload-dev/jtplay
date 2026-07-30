// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browse abstraction: a Source is one browsable tree of media (a DLNA
// server, a local directory, ...) and a Provider discovers Sources of one
// kind. The app only ever talks to these interfaces; everything protocol
// specific lives in the implementations (dlna_source.cpp, fs_source.cpp).
//
// All Browse()/Discover() calls are blocking I/O; the app invokes them from
// its worker thread only. Sources are handed around as shared_ptr so an
// in-flight browse survives a rescan that replaces the source list.
#ifndef BROWSE_SOURCE_H
#define BROWSE_SOURCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace browse {

// One entry of a listing: either a folder or a (hopefully) playable item.
// This is the only shape of media metadata the UI understands; every source
// converts its native objects into it.
struct Entry {
  enum class Kind { Folder, Audio, Video, Image, Other };

  std::string id;           // source-specific opaque id (object id, path, ...)
  std::string title;
  Kind kind = Kind::Other;
  int child_count = -1;     // folders only; -1 = not reported

  // Metadata, all optional.
  std::string artist;
  std::string album;
  std::string genre;
  std::string date;
  std::string art_url;      // http(s) URL or an absolute local file path
  std::string format;       // MIME type or container hint, e.g. "video/mp4"

  // Best playable resource in a form ffmpeg can open (URL or local path).
  // May be empty (or stale) when 'resolvable' is set; see Source::Resolve().
  std::string res_url;
  int64_t duration_us = -1; // -1 = unknown
  int64_t size_bytes = -1;
  std::string resolution;   // e.g. "1920x1080"

  // The source can mint a res_url for this entry from 'id' at playback
  // time. Set by sources whose URLs are signed or otherwise short-lived,
  // where the one seen while browsing may already have expired.
  bool resolvable = false;

  bool IsFolder() const { return kind == Kind::Folder; }
  bool IsAudio() const { return kind == Kind::Audio; }
  bool IsVideo() const { return kind == Kind::Video; }
  bool IsImage() const { return kind == Kind::Image; }
  bool IsPlayable() const {
    return !IsFolder() && (!res_url.empty() || resolvable);
  }
};

struct Listing {
  std::vector<Entry> entries;
};

// One browsable tree of media.
class Source {
public:
  virtual ~Source() = default;

  virtual const std::string& Name() const = 0;   // row title
  virtual const std::string& Detail() const = 0; // row subtitle (host, path)
  virtual const char* Icon() const = 0;          // emoji glyph for the row

  // Id of the root folder; passed back to Browse() to start.
  virtual std::string RootId() const = 0;

  // Lists the direct children of 'id'. Blocking; worker thread only.
  virtual bool Browse(const std::string& id, Listing& out,
                      std::string& error) = 0;

  // Turns 'entry' into something ffmpeg can open, immediately before
  // playback starts. The default hands back the res_url captured while
  // browsing, which is all a DLNA server or a local file needs; sources
  // whose URLs carry a token that expires between listing and playing
  // override this and mint a fresh one from entry.id.
  //
  // Blocking (it may do I/O); worker thread only.
  virtual bool Resolve(const Entry& entry, std::string& url,
                       std::string& error) {
    url = entry.res_url;
    if (url.empty()) {
      error = "this item has no playable resource";
      return false;
    }
    return true;
  }
};

using SourcePtr = std::shared_ptr<Source>;

// Discovers Sources of one kind (SSDP scan, mounted filesystems, ...).
class Provider {
public:
  virtual ~Provider() = default;

  virtual const char* Name() const = 0; // for error messages

  // Appends whatever it finds to 'out'. Returning false (with a reason in
  // 'error') means the scan itself failed; an empty result is not an error.
  // Blocking; worker thread only.
  virtual bool Discover(std::vector<SourcePtr>& out, std::string& error) = 0;
};

} // namespace browse

#endif
